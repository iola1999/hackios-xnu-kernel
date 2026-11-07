/*
 * Iris VPU v3.5 Video Decode Acceleration Driver for REDMI K90 (annibale / SM8750):
 *   - Hardware: Iris VPU v3.5 at 0x0aa00000, videocc at 0x0aaf0000
 *   - Firmware memory: video_region at 0x9c300000 (8 MB, no-map)
 *   - Qualcomm PAS ID 9 SCM authentication:
 *     0x42000201 (INIT_IMAGE), 0x42000202 (MEM_SETUP), 0x42000205 (AUTH_AND_RESET),
 *     0x42000c08 (MP_VIDEO_VAR)
 *   - SMMU stream IDs 0x1940 and 0x1947 bypass configuration
 *   - HFI gen2 queue management and GIC SPI 174 (INTID 206)
 *   - Character device /dev/annibale-iris for VideoToolbox acceleration
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern int32_t adreno8_scm_call(uint64_t function, uint64_t arginfo, uint64_t argument0, uint64_t argument1, uint64_t argument2, uint64_t argument3, uint64_t *result);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

#define say(...) printf("iris: " __VA_ARGS__)

#define IRIS_BASE_PHYS          0x0aa00000UL
#define IRIS_BASE_SIZE          0xf0000UL       /* 960 KB */
#define VIDEOCC_BASE_PHYS       0x0aaf0000UL
#define VIDEOCC_BASE_SIZE       0x10000UL
#define VIDEO_REGION_PHYS       0x9c300000UL
#define VIDEO_REGION_SIZE       0x800000UL      /* 8 MB */

#define IRIS_PAS_ID             9u
#define IRIS_GIC_SPI_ID         174u
#define IRIS_GIC_INTID          (32u + IRIS_GIC_SPI_ID) /* 206 */
#define IRIS_SMMU_STREAM_0      0x1940u
#define IRIS_SMMU_STREAM_1      0x1947u

/* APPS SMMU Register Offsets and Bitmasks */
#define APPS_SMMU_PHYS          0x15000000UL
#define APPS_SMMU_SIZE          0x100000UL
#define SMMU_IDR0               0x0000
#define SMMU_IDR1               0x0004
#define SMMU_SMR(n)             (0x0800 + (n) * 4)
#define SMMU_S2CR(n)            (0x0c00 + (n) * 4)
#define SMMU_CBAR(n)            (0x1000 + (n) * 4)
#define SMMU_CBA2R(n)           (0x1800 + (n) * 4)
#define SMMU_SMR_VALID          (1u << 31)
#define SMMU_TLBIALLNSNH        0x0068
#define SMMU_TLBGSYNC           0x0070

/* Iris VPU Register Offsets */
#define CPU_CS_AHB_CTRL         0x000
#define CPU_CS_AHB_STATUS       0x004
#define CPU_CS_HFI_CTRL         0x018
#define CPU_CS_HFI_QTBL_ADDR    0x028
#define CPU_CS_HFI_QTBL_STATUS  0x02c
#define CPU_CS_SFR_ADDR         0x030
#define CPU_CS_A2H_INTERRUPT    0x040
#define CPU_CS_A2H_STATUS       0x044
#define CPU_CS_A2H_CLEAR        0x048

/* SCM PAS commands */
#define SCM_PAS_INIT_IMAGE      0x42000201ULL
#define SCM_PAS_MEM_SETUP       0x42000202ULL
#define SCM_PAS_AUTH_AND_RESET  0x42000205ULL
#define SCM_MP_VIDEO_VAR        0x42000c08ULL

#define HFI_QUEUE_CAPACITY      1024u

/* IOCTL interface for /dev/annibale-iris */
struct iris_session_params {
	uint32_t codec_type;    /* 1: H.264 (avc1), 2: HEVC (hvc1), 3: VP9 (vp09) */
	uint32_t width;
	uint32_t height;
	uint32_t session_id;    /* Out */
};

struct iris_decode_frame {
	uint32_t session_id;
	uint64_t input_user_addr;
	uint32_t input_len;
	uint64_t out_user_y;    /* In: 用户态 CVPixelBuffer 平面 0 (Y) 目标地址 */
	uint64_t out_user_uv;   /* In: 用户态 CVPixelBuffer 平面 1 (UV) 目标地址 */
	uint64_t out_phys_y;    /* Out: 硬件解码平面 0 物理基址 */
	uint64_t out_phys_uv;   /* Out: 硬件解码平面 1 物理基址 */
	uint32_t out_stride;    /* In/Out: 跨距 (对齐至 128 字节) */
	uint32_t flags;         /* Out: 状态标志位 (0x01: 成功) */
};

#define IRIS_IOC_OPEN_SESSION   _IOWR('V', 1, struct iris_session_params)
#define IRIS_IOC_DECODE_FRAME   _IOWR('V', 2, struct iris_decode_frame)
#define IRIS_IOC_CLOSE_SESSION  _IOW('V', 3, uint32_t)

static vm_offset_t iris_base;
static vm_offset_t videocc_base;
static vm_offset_t video_region_vbase;
static vm_offset_t iris_smmu_vbase;

static lck_grp_t *iris_lock_grp;
static lck_mtx_t *iris_lock;
static bool iris_is_ready = false;
static uint32_t iris_irq_count = 0;
static int iris_event_wait = 0;

static inline uint32_t
iris_rd(uint32_t off)
{
	return *(volatile uint32_t *)(iris_base + off);
}

static inline void
iris_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(iris_base + off) = val;
	__builtin_arm_dsb(0xf);
}

static inline uint32_t
videocc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(videocc_base + off);
}

static inline void
videocc_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(videocc_base + off) = val;
	__builtin_arm_dsb(0xf);
}

static inline uint32_t
iris_smmu_rd(vm_offset_t off)
{
	return *(volatile uint32_t *)(iris_smmu_vbase + off);
}

static inline void
iris_smmu_wr(vm_offset_t off, uint32_t val)
{
	*(volatile uint32_t *)(iris_smmu_vbase + off) = val;
	__builtin_arm_dsb(0xf);
}

/* SMMU bypass configuration for Iris video streams */
static bool
iris_smmu_configure_bypass(uint16_t stream)
{
	if (iris_smmu_vbase == 0) {
		iris_smmu_vbase = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
		if (iris_smmu_vbase == 0) {
			say("failed to map APPS SMMU for Iris\n");
			return false;
		}
	}
	uint32_t idr0 = iris_smmu_rd(SMMU_IDR0);
	uint32_t idr1 = iris_smmu_rd(SMMU_IDR1);
	uint32_t count = MIN(idr0 & 0xff, 128u);
	uint32_t banks = idr1 & 0xff;
	uint32_t pages = 1u << (((idr1 >> 28) & 7) + 1);
	uint32_t bank = banks > 0 ? banks - 1 : 0;
	uint32_t pgsize = (idr1 & (1u << 31)) ? 0x10000u : 0x1000u;
	uint32_t cb = (pages + bank) * pgsize;
	uint32_t slot = ~0u;
	bool already_matched = false;

	for (uint32_t i = 0; i < count; i++) {
		uint32_t smr = iris_smmu_rd(SMMU_SMR(i));
		uint32_t s2cr = iris_smmu_rd(SMMU_S2CR(i));
		uint32_t mask = (smr >> 16) & 0x7fff;

		if (!(smr & SMMU_SMR_VALID)) {
			if (slot == ~0u) {
				slot = i;
			}
			continue;
		}
		if (((smr ^ stream) & 0xffff & ~mask) == 0) {
			if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) == bank) {
				already_matched = true;
				slot = i;
				break;
			}
			say("stream %#x already mapped to bank %#x\n", stream, s2cr & 0xff);
			return true;
		}
	}
	if (slot == ~0u) {
		say("no free SMMU stream match available for stream %#x\n", stream);
		return false;
	}
	/* Configure context bank as non-translating bypass */
	iris_smmu_wr(cb, 0);                 /* SCTLR = 0 */
	iris_smmu_wr(SMMU_CBA2R(bank), 1);   /* Stage 2 bypass */
	iris_smmu_wr(SMMU_CBAR(bank), 0x10000);

	if (!already_matched) {
		iris_smmu_wr(SMMU_S2CR(slot), bank);
		iris_smmu_wr(SMMU_SMR(slot), SMMU_SMR_VALID | stream);
	}
	iris_smmu_wr(SMMU_TLBIALLNSNH, ~0u);
	iris_smmu_wr(SMMU_TLBGSYNC, ~0u);
	say("stream %#x configured for SMMU bypass (slot %u, bank %u, cb %#x)\n", stream, slot, bank, cb);
	return true;
}

/* Interrupt Service Routine for GIC SPI 174 (INTID 206) */
static void
annibale_iris_isr(void *ctx __unused, uint32_t intid __unused)
{
	iris_irq_count++;
	if (iris_base != 0) {
		uint32_t status = iris_rd(CPU_CS_A2H_STATUS);
		iris_wr(CPU_CS_A2H_CLEAR, status);
		__builtin_arm_dsb(0xf);
	}
	wakeup(&iris_event_wait);
}

/*
 * HFI gen2 Queue Management:
 * Queue 0: Command Queue (at 0x9c305000, 1024 dwords)
 * Queue 1: Message Queue (at 0x9c306000, 1024 dwords)
 * Queue 2: Debug Queue   (at 0x9c307000, 1024 dwords)
 */
static void
iris_hfi_queues_init(void)
{
	if (video_region_vbase == 0) {
		return;
	}
	volatile uint32_t *qtbl = (volatile uint32_t *)(video_region_vbase + 0x4000);
	bzero((void *)qtbl, 0x4000);

	/* Table header: version, size, num_queues */
	qtbl[0] = 0;                                            /* Version */
	qtbl[1] = (6 + 12 * 3) * sizeof(uint32_t);              /* Table size */
	qtbl[2] = 3;                                            /* 3 active queues */

	/* Queue 0: Command Queue */
	volatile uint32_t *q0 = &qtbl[6 + 0 * 12];
	q0[0] = 0;                                              /* Queue type: Command */
	q0[1] = 1;                                              /* Status: active */
	q0[2] = 0;                                              /* Direction: host to device (TX) */
	q0[3] = HFI_QUEUE_CAPACITY;                             /* Size in dwords */
	q0[7] = 1;                                              /* Interrupt enable */
	q0[8] = (uint32_t)(VIDEO_REGION_PHYS + 0x5000);         /* Physical address */
	q0[10] = 0;                                             /* read_idx */
	q0[11] = 0;                                             /* write_idx */

	/* Queue 1: Message Queue */
	volatile uint32_t *q1 = &qtbl[6 + 1 * 12];
	q1[0] = 1;                                              /* Queue type: Message */
	q1[1] = 1;                                              /* Status: active */
	q1[2] = 1;                                              /* Direction: device to host (RX) */
	q1[3] = HFI_QUEUE_CAPACITY;
	q1[7] = 1;
	q1[8] = (uint32_t)(VIDEO_REGION_PHYS + 0x6000);
	q1[10] = 0;
	q1[11] = 0;

	/* Queue 2: Debug Queue */
	volatile uint32_t *q2 = &qtbl[6 + 2 * 12];
	q2[0] = 2;                                              /* Queue type: Debug */
	q2[1] = 1;
	q2[2] = 1;
	q2[3] = HFI_QUEUE_CAPACITY;
	q2[7] = 0;
	q2[8] = (uint32_t)(VIDEO_REGION_PHYS + 0x7000);
	q2[10] = 0;
	q2[11] = 0;

	__builtin_arm_dsb(0xf);
}

static bool
iris_hfi_cmd_write(const uint32_t *pkt, uint32_t dwords)
{
	if (video_region_vbase == 0) {
		return false;
	}
	volatile uint32_t *qtbl = (volatile uint32_t *)(video_region_vbase + 0x4000);
	volatile uint32_t *q0_hdr = &qtbl[6];
	volatile uint32_t *q0_buf = (volatile uint32_t *)(video_region_vbase + 0x5000);

	uint32_t read_idx = q0_hdr[10];
	uint32_t write_idx = q0_hdr[11];
	uint32_t capacity = HFI_QUEUE_CAPACITY;
	uint32_t free_space = (read_idx + capacity - write_idx - 1) % capacity;

	if (dwords > free_space) {
		say("HFI cmd queue full (%u needed, %u free)\n", dwords, free_space);
		return false;
	}

	for (uint32_t i = 0; i < dwords; i++) {
		q0_buf[(write_idx + i) % capacity] = pkt[i];
	}
	__builtin_arm_dsb(0xf);

	q0_hdr[11] = (write_idx + dwords) % capacity;
	__builtin_arm_dsb(0xf);

	/* Trigger doorbell interrupt to hardware */
	if (iris_base != 0) {
		iris_wr(CPU_CS_A2H_INTERRUPT, 1);
	}
	return true;
}

/* SCM PAS ID 9 Authentication Sequence */
static bool
iris_pas_authenticate(void)
{
	uint64_t result = ~0ULL;
	int32_t status;
	uint64_t size = 0;
	int disk = 0;
	const uint8_t *data;

	say("initiating PAS 9 authentication for video_region at %#llx (%u MB)...\n",
	    VIDEO_REGION_PHYS, (uint32_t)(VIDEO_REGION_SIZE >> 20));

	data = redwood_image_file("fw", "vpu35_4v.mbn", &size, &disk);
	if (data == NULL || size == 0) {
		say("firmware vpu35_4v.mbn not found\n");
		return false;
	}

	/* Parse ELF header and find signature hash segment */
	if (size < 52 || memcmp(data, "\177ELF", 4) != 0) {
		say("firmware vpu35_4v.mbn invalid ELF format\n");
		return false;
	}

	uint32_t phoff = *(const uint32_t *)(data + 28);
	uint16_t phnum = *(const uint16_t *)(data + 44);
	if (phoff < 52 || phnum < 2 || phnum > 32 || (uint64_t)phoff + (uint64_t)phnum * 32 > size) {
		say("firmware vpu35_4v.mbn invalid program header table\n");
		return false;
	}

	uint32_t hash_offset = 0, hash_size = 0;
	uint32_t header_size = *(const uint32_t *)(data + phoff + 16);
	bool hash_found = false;

	for (uint32_t i = 1; i < phnum; i++) {
		const uint8_t *ph = data + phoff + i * 32;
		uint32_t flags = *(const uint32_t *)(ph + 24);
		uint32_t offset = *(const uint32_t *)(ph + 4);
		uint32_t filesz = *(const uint32_t *)(ph + 16);

		if ((flags & 0x07000000) == 0x02000000) {
			hash_offset = offset;
			hash_size = filesz;
			hash_found = true;
			break;
		}
	}

	if (!hash_found || hash_size == 0 || (uint64_t)hash_offset + (uint64_t)hash_size > size) {
		say("firmware vpu35_4v.mbn missing signature hash segment\n");
		return false;
	}

	/* Copy header and hash segment into metadata buffer */
	vm_offset_t metadata_vaddr = 0;
	if (kmem_alloc(kernel_map, &metadata_vaddr, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("failed to allocate metadata buffer\n");
		return false;
	}

	uint64_t metadata_pa = kvtophys(metadata_vaddr);
	if (metadata_pa == 0) {
		kmem_free(kernel_map, metadata_vaddr, PAGE_SIZE);
		return false;
	}

	memcpy((void *)metadata_vaddr, data, header_size);
	memcpy((void *)(metadata_vaddr + header_size), data + hash_offset, hash_size);
	__builtin_arm_dsb(0xf);

	/* 1. SCM_PAS_INIT_IMAGE (0x42000201) */
	status = adreno8_scm_call(SCM_PAS_INIT_IMAGE, 0x82, IRIS_PAS_ID, metadata_pa, 0, 0, &result);
	if (status != 0 || result != 0) {
		say("INIT_IMAGE returned status %d, result %#llx\n", status, result);
		kmem_free(kernel_map, metadata_vaddr, PAGE_SIZE);
		return false;
	}

	/* 2. SCM_PAS_MEM_SETUP (0x42000202) */
	result = ~0ULL;
	status = adreno8_scm_call(SCM_PAS_MEM_SETUP, 3, IRIS_PAS_ID, VIDEO_REGION_PHYS, VIDEO_REGION_SIZE, 0, &result);
	if (status != 0 || result != 0) {
		say("MEM_SETUP returned status %d, result %#llx\n", status, result);
		kmem_free(kernel_map, metadata_vaddr, PAGE_SIZE);
		return false;
	}

	/* 3. Load PT_LOAD segments into video_region */
	for (uint32_t i = 1; i < phnum; i++) {
		const uint8_t *ph = data + phoff + i * 32;
		uint32_t type = *(const uint32_t *)(ph + 0);
		uint32_t offset = *(const uint32_t *)(ph + 4);
		uint32_t paddr = *(const uint32_t *)(ph + 12);
		uint32_t filesz = *(const uint32_t *)(ph + 16);
		uint32_t memsz = *(const uint32_t *)(ph + 20);

		if (type == 1 && memsz > 0 && offset + filesz <= size) {
			uint64_t seg_offset = paddr >= VIDEO_REGION_PHYS ? (paddr - VIDEO_REGION_PHYS) : 0;
			if (seg_offset + memsz <= VIDEO_REGION_SIZE && video_region_vbase != 0) {
				memcpy((void *)(video_region_vbase + seg_offset), data + offset, filesz);
				if (memsz > filesz) {
					bzero((void *)(video_region_vbase + seg_offset + filesz), memsz - filesz);
				}
			}
		}
	}
	__builtin_arm_dsb(0xf);

	/* 4. SCM_PAS_AUTH_AND_RESET (0x42000205) */
	result = ~0ULL;
	status = adreno8_scm_call(SCM_PAS_AUTH_AND_RESET, 1, IRIS_PAS_ID, 0, 0, 0, &result);
	if (status != 0 || result != 0) {
		say("AUTH_AND_RESET returned status %d, result %#llx\n", status, result);
		kmem_free(kernel_map, metadata_vaddr, PAGE_SIZE);
		return false;
	}

	/* 5. SCM_MP_VIDEO_VAR (0x42000c08) Memory Protection Configuration */
	result = ~0ULL;
	status = adreno8_scm_call(SCM_MP_VIDEO_VAR, 4, 0, 0x25800000ULL, 0x01000000ULL, 0x24800000ULL, &result);
	if (status == 0 && result == 0) {
		say("MP_VIDEO_VAR memory protection configured\n");
	}

	kmem_free(kernel_map, metadata_vaddr, PAGE_SIZE);
	say("PAS 9 authentication completed\n");
	return true;
}

/* videocc and VPU power/clock initialization */
static bool
iris_power_up(void)
{
	if (iris_base == 0) {
		iris_base = ml_io_map(IRIS_BASE_PHYS, IRIS_BASE_SIZE);
		videocc_base = ml_io_map(VIDEOCC_BASE_PHYS, VIDEOCC_BASE_SIZE);
		video_region_vbase = ml_io_map_wcomb(VIDEO_REGION_PHYS, VIDEO_REGION_SIZE);
	}

	if (iris_base == 0 || videocc_base == 0 || video_region_vbase == 0) {
		say("failed to map Iris VPU or videocc memory\n");
		return false;
	}

	/* Configure APPS SMMU bypass for video stream 0x1940 and 0x1947 */
	iris_smmu_configure_bypass(IRIS_SMMU_STREAM_0);
	iris_smmu_configure_bypass(IRIS_SMMU_STREAM_1);

	/* Enable videocc GDSCs: MVS0C (venus) & MVS0 (vcodec0) */
	videocc_wr(0x8000, videocc_rd(0x8000) & ~1u); /* MVS0C_GDSC */
	videocc_wr(0x8004, videocc_rd(0x8004) & ~1u); /* MVS0_GDSC */
	delay_for_interval(1, NSEC_PER_MSEC);

	/* Enable video branch clocks */
	videocc_wr(0x1000, 1); /* video_cc_mvs0c_clk */
	videocc_wr(0x1004, 1); /* video_cc_mvs0_clk */
	__builtin_arm_dsb(0xf);

	/* PAS ID 9 Authentication */
	if (!iris_pas_authenticate()) {
		say("warning: Iris PAS 9 authentication failed\n");
		return false;
	}

	/* Initialize HFI gen2 queue tables in video_region */
	iris_hfi_queues_init();
	uint64_t qtbl_phys = VIDEO_REGION_PHYS + 0x4000;
	iris_wr(CPU_CS_HFI_QTBL_ADDR, (uint32_t)qtbl_phys);
	iris_wr(CPU_CS_HFI_CTRL, 1); /* Enable HFI interface */
	__builtin_arm_dsb(0xf);

	say("SM8750 Iris VPU v3.5 hardware powered and HFI gen2 queues initialized\n");
	return true;
}

/* Character device callbacks */

static int
iris_dev_open(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	lck_mtx_lock(iris_lock);
	if (!iris_is_ready) {
		if (!iris_power_up()) {
			lck_mtx_unlock(iris_lock);
			return EIO;
		}
		iris_is_ready = true;
	}
	lck_mtx_unlock(iris_lock);
	return 0;
}

static int
iris_dev_close(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	return 0;
}

static int
iris_dev_ioctl(dev_t dev __unused, u_long cmd, caddr_t data, int flags __unused, struct proc *p __unused)
{
	int error = 0;

	lck_mtx_lock(iris_lock);
	switch (cmd) {
	case IRIS_IOC_OPEN_SESSION: {
		struct iris_session_params *sp = (struct iris_session_params *)data;
		sp->session_id = 1;
		say("opened decode session for codec type %u (width %u, height %u)\n",
		    sp->codec_type, sp->width, sp->height);
		break;
	}
	case IRIS_IOC_DECODE_FRAME: {
		struct iris_decode_frame *df = (struct iris_decode_frame *)data;

		/* Copyin user bitstream data if provided */
		if (df->input_len > 0 && df->input_user_addr != 0) {
			uint32_t copy_len = MIN(df->input_len, 0x100000u);
			uint8_t *kbuf = kalloc_data(copy_len, Z_WAITOK);
			if (kbuf != NULL) {
				if (copyin((user_addr_t)df->input_user_addr, kbuf, copy_len) == 0) {
					if (video_region_vbase != 0) {
						memcpy((void *)(video_region_vbase + 0x8000), kbuf, copy_len);
						__builtin_arm_dsb(0xf);
					}
				}
				kfree_data(kbuf, copy_len);
			}
		}

		uint32_t stride = df->out_stride;
		if (stride == 0) {
			stride = 1920;
		}
		uint32_t height = 1080;
		if (stride <= 640) {
			height = 480;
		} else if (stride <= 1280) {
			height = 720;
		} else {
			height = 1080;
		}
		uint32_t y_size = stride * height;
		uint32_t uv_size = stride * (height / 2);

		/* Clamp to maximum allocated video_region NV12 buffer space */
		if (y_size > 1920 * 1080) {
			y_size = 1920 * 1080;
		}
		if (uv_size > 1920 * 540) {
			uv_size = 1920 * 540;
		}

		/* Submit HFI decode command into Command Queue */
		uint32_t cmd_pkt[8];
		cmd_pkt[0] = 8 * sizeof(uint32_t); /* packet size in bytes */
		cmd_pkt[1] = 0x01;                  /* HFI_CMD_FRAME_DECODE */
		cmd_pkt[2] = df->session_id;
		cmd_pkt[3] = (uint32_t)(VIDEO_REGION_PHYS + 0x8000); /* input bitstream */
		cmd_pkt[4] = df->input_len;
		cmd_pkt[5] = (uint32_t)(VIDEO_REGION_PHYS + 0x100000); /* output Y plane */
		cmd_pkt[6] = (uint32_t)(VIDEO_REGION_PHYS + 0x100000 + y_size); /* output UV plane */
		cmd_pkt[7] = 0;
		if (!iris_hfi_cmd_write(cmd_pkt, 8)) {
			error = EBUSY;
			break;
		}

		/* Wait for completion via GIC SPI 174 (atomically drop iris_lock during sleep) */
		lck_mtx_sleep(iris_lock, LCK_SLEEP_DEFAULT, (event_t)&iris_event_wait, THREAD_INTERRUPTIBLE);

		df->out_phys_y = VIDEO_REGION_PHYS + 0x100000;
		df->out_phys_uv = df->out_phys_y + y_size;
		df->out_stride = stride;
		df->flags = 0x01; /* Frame ready */

		/* Copyout decoded NV12 planes to user buffer if provided */
		if (df->out_user_y != 0 && video_region_vbase != 0) {
			copyout((void *)(video_region_vbase + 0x100000), (user_addr_t)df->out_user_y, y_size);
		}
		if (df->out_user_uv != 0 && video_region_vbase != 0) {
			copyout((void *)(video_region_vbase + 0x100000 + y_size), (user_addr_t)df->out_user_uv, uv_size);
		}
		break;
	}
	case IRIS_IOC_CLOSE_SESSION: {
		uint32_t *sid = (uint32_t *)data;
		say("closed decode session %u\n", *sid);
		break;
	}
	default:
		error = ENOTTY;
		break;
	}
	lck_mtx_unlock(iris_lock);
	return error;
}

static const struct cdevsw iris_cdevsw = {
	.d_open = iris_dev_open,
	.d_close = iris_dev_close,
	.d_read = eno_rdwrt,
	.d_write = eno_rdwrt,
	.d_ioctl = iris_dev_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0
};

void
annibale_iris_init(void)
{
	static bool initialized = false;
	if (initialized) {
		return;
	}
	initialized = true;

	say("registering SM8750 Iris VPU v3.5 (/dev/annibale-iris)...\n");

	iris_lock_grp = lck_grp_alloc_init("annibale-iris", LCK_GRP_ATTR_NULL);
	iris_lock = lck_mtx_alloc_init(iris_lock_grp, LCK_ATTR_NULL);

	/* Attach GIC SPI 174 (INTID 206) */
	if (gic_v3_spi_attach(IRIS_GIC_INTID, false, annibale_iris_isr, NULL) == KERN_SUCCESS) {
		say("attached to GIC SPI 174 (INTID %u)\n", IRIS_GIC_INTID);
	} else {
		say("warning: failed to attach to GIC SPI 174 (INTID %u)\n", IRIS_GIC_INTID);
	}

	int major = cdevsw_add(-1, &iris_cdevsw);
	if (major >= 0) {
		if (devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "annibale-iris") != NULL) {
			say("/dev/annibale-iris created successfully\n");
		} else {
			say("warning: failed to create devfs node /dev/annibale-iris\n");
		}
	} else {
		say("warning: failed to allocate cdevsw major for iris\n");
	}
}

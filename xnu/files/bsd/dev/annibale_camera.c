/*
 * Camera pipeline driver for REDMI K90 (annibale / SM8750 "sun"):
 * L1a hardware test pattern generator (TPG) and /dev/redwood-camera driver bridge:
 *   - CAM-CPAS: 0x0ac13000 (titan top power and interconnect bandwidth)
 *   - CAMCC GDSC: titan_top_gdsc (0x9000) & tfe0_gdsc (0x9004) with PWR_ON status polling
 *   - CSID-lite0: 0x0ad6d000 (hardware TPG, VC0, MIPI RAW10, 1920x1080@30fps, RDI0 routing)
 *   - IFE-lite0: 0x0ad6f800 (Write Master 0 DMA to camera_region)
 *   - Camera DMA region: camera_region at 0x9b300000 (8 MB)
 *   - Exports /dev/redwood-camera conforming to struct redwood_camera_frame (RAW10 1080p@30fps)
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/redwood_camera.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

#define say(...) printf("annibale_cam: " __VA_ARGS__)

/* Hardware base addresses */
#define CAM_CPAS_PHYS           0x0ac13000UL
#define CAM_CPAS_SIZE           0x4000UL
#define CAMCC_PHYS              0x0ad00000UL
#define CAMCC_SIZE              0x100000UL      /* Covers 0x0ad00000 to 0x0ae00000 */
#define CSID_LITE0_PHYS         0x0ad6d000UL
#define CSID_LITE0_SIZE         0x2000UL
#define IFE_LITE0_PHYS          0x0ad6f800UL
#define IFE_LITE0_SIZE          0x2000UL

#define CAMERA_REGION_PHYS      0x9b300000UL
#define CAMERA_REGION_SIZE      0x800000UL      /* 8 MB */

/* Camera frame geometry: 1080p MIPI RAW10 */
#define CAM_WIDTH               1920u
#define CAM_HEIGHT              1080u
#define CAM_STRIDE              (CAM_WIDTH * 5u / 4u) /* 2400 bytes per line */
#define CAM_FRAME_BYTES         (CAM_STRIDE * CAM_HEIGHT) /* 2,592,000 bytes */
#define CAM_FPS                 30u

/* CAMCC GDSC register offsets and bitmasks */
#define CAMCC_TITAN_TOP_GDSC    (0x9000 / 4)
#define CAMCC_TFE0_GDSC         (0x9004 / 4)
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_PWR_ON             (1u << 31)

/* CSID-lite0 register offsets (base: 0x0ad6d000) */
#define CSID_HW_VERSION            0x000u
#define CSID_RST_STROBES           0x010u
#define CSID_CSI2_RX_IRQ_STATUS    0x020u
#define CSID_CSI2_RX_IRQ_MASK      0x024u
#define CSID_CSI2_RX_IRQ_CLEAR     0x028u
#define CSID_RDI0_IRQ_STATUS       0x030u
#define CSID_RDI0_IRQ_MASK         0x034u
#define CSID_RDI0_IRQ_CLEAR        0x038u
#define CSID_TOP_IRQ_STATUS        0x070u
#define CSID_TOP_IRQ_MASK          0x074u
#define CSID_TOP_IRQ_CLEAR         0x078u
#define CSID_IRQ_CMD               0x080u
#define CSID_CSI2_RX_CFG0          0x100u
#define CSID_CSI2_RX_CFG1          0x104u
#define CSID_RDI0_CFG0             0x200u
#define CSID_RDI0_CFG1             0x204u
#define CSID_RDI0_CTRL             0x208u
#define CSID_RDI0_FRM_DROP_PATTERN 0x20cu
#define CSID_RDI0_FRM_DROP_PERIOD  0x210u
#define CSID_RDI0_IRQ_SUB_PATTERN  0x214u
#define CSID_RDI0_IRQ_SUB_PERIOD   0x218u
#define CSID_RDI0_PIX_DROP_PATTERN 0x224u
#define CSID_RDI0_PIX_DROP_PERIOD  0x228u
#define CSID_RDI0_LINE_DROP_PATTERN 0x22cu
#define CSID_RDI0_LINE_DROP_PERIOD 0x230u
#define CSID_TPG_CTRL              0x600u
#define CSID_TPG_VC_CFG0           0x604u
#define CSID_TPG_VC_CFG1           0x608u
#define CSID_TPG_LFSR_SEED         0x60cu
#define CSID_TPG_DT0_CFG0          0x610u
#define CSID_TPG_DT0_CFG1          0x614u
#define CSID_TPG_DT0_CFG2          0x618u
#define CSID_TPG_COLOR_BARS_CFG    0x640u
#define CSID_TPG_COMMON_GEN_CFG    0x648u

/* IFE-lite0 register offsets (base: 0x0ad6f800) */
#define IFE_HW_VERSION             0x0000u
#define IFE_GLOBAL_RESET           0x0018u
#define IFE_IRQ_MASK0              0x005cu
#define IFE_IRQ_CLEAR0             0x0064u
#define IFE_REG_UPDATE             0x04acu
#define IFE_BUS_CGC_OVD            0x200cu
#define IFE_WM0_CFG                0x2208u
#define IFE_WM0_IMAGE_ADDR         0x2214u
#define IFE_WM0_WIDTH              0x221cu
#define IFE_WM0_HEIGHT             0x2220u
#define IFE_WM0_STRIDE             0x2228u
#define IFE_WM0_FRAME_INC          0x2258u
#define IFE_WM0_BURST_LIMIT        0x225cu

#define RAW10_DT                   0x2bu

static vm_offset_t cpas_base;
static vm_offset_t camcc_base;
static vm_offset_t csid_lite_base;
static vm_offset_t ife_lite_base;
static vm_offset_t cam_region_vbase;

static lck_grp_t *cam_lock_grp;
static lck_mtx_t *cam_lock;
static bool cam_is_open = false;
static uint64_t cam_frame_seq = 0;
static uint32_t current_exposure = 1000;
static uint32_t current_gain = 256; /* 1x */
static uint32_t current_focus = 512;

static inline void
csid_wr(uint32_t offset, uint32_t val)
{
	if (csid_lite_base != 0) {
		*(volatile uint32_t *)(csid_lite_base + offset) = val;
	}
}

static inline void
ife_wr(uint32_t offset, uint32_t val)
{
	if (ife_lite_base != 0) {
		*(volatile uint32_t *)(ife_lite_base + offset) = val;
	}
}

/* Generate standard 8-bar test pattern in MIPI RAW10 format as buffer seed */
static void
generate_tpg_bars(uint8_t *dest)
{
	static const uint16_t bar_r[8] = { 1020, 1020, 0,    0,    1020, 1020, 0,    0 };
	static const uint16_t bar_g[8] = { 1020, 1020, 1020, 1020, 0,    0,    0,    0 };
	static const uint16_t bar_b[8] = { 1020, 0,    1020, 0,    1020, 0,    1020, 0 };

	const uint32_t bar_width = CAM_WIDTH / 8u; /* 240 pixels per bar */

	for (uint32_t y = 0; y < CAM_HEIGHT; y++) {
		uint8_t *line = dest + y * CAM_STRIDE;
		bool odd_row = (y & 1) != 0;

		for (uint32_t x = 0; x < CAM_WIDTH; x += 4) {
			uint16_t p[4];

			for (int k = 0; k < 4; k++) {
				uint32_t col = x + (uint32_t)k;
				uint32_t bidx = (col / bar_width) % 8;
				bool odd_col = (col & 1) != 0;

				if (!odd_row) {
					p[k] = !odd_col ? bar_r[bidx] : bar_g[bidx];
				} else {
					p[k] = !odd_col ? bar_g[bidx] : bar_b[bidx];
				}
			}

			uint32_t byte_off = (x / 4u) * 5u;
			line[byte_off + 0] = (uint8_t)(p[0] >> 2);
			line[byte_off + 1] = (uint8_t)(p[1] >> 2);
			line[byte_off + 2] = (uint8_t)(p[2] >> 2);
			line[byte_off + 3] = (uint8_t)(p[3] >> 2);
			line[byte_off + 4] = (uint8_t)((p[0] & 3) | ((p[1] & 3) << 2) |
			                              ((p[2] & 3) << 4) | ((p[3] & 3) << 6));
		}
	}
}

/* Hardware CSID-lite0 TPG and IFE-lite0 Write Master 0 configuration */
static bool
csid_lite_hardware_start(void)
{
	if (csid_lite_base == 0) {
		say("csid_lite_base is not mapped\n");
		return false;
	}

	/* 1. Mask and clear CSID interrupts */
	csid_wr(CSID_CSI2_RX_IRQ_MASK, 0);
	csid_wr(CSID_RDI0_IRQ_MASK, 0);
	csid_wr(CSID_TOP_IRQ_CLEAR, 1);
	csid_wr(CSID_CSI2_RX_IRQ_CLEAR, 0xfffffff);
	csid_wr(CSID_RDI0_IRQ_CLEAR, 0x7fff);
	csid_wr(CSID_IRQ_CMD, 1);

	/* 2. Hardware and register reset strobes */
	csid_wr(CSID_RST_STROBES, 0x1);
	delay_for_interval(1, NSEC_PER_MSEC);
	csid_wr(CSID_TOP_IRQ_CLEAR, 1);
	csid_wr(CSID_IRQ_CMD, 1);

	csid_wr(CSID_RST_STROBES, 0x1e);
	delay_for_interval(1, NSEC_PER_MSEC);
	csid_wr(CSID_TOP_IRQ_CLEAR, 1);
	csid_wr(CSID_IRQ_CMD, 1);

	/* 3. Configure CSI2 RX for internal TPG stream */
	csid_wr(CSID_CSI2_RX_CFG0, 3u | (0x3210u << 4));
	csid_wr(CSID_CSI2_RX_CFG1, (1u << 6) | 1u);

	/* 4. Configure TPG core: VC 0, RAW10, 1920x1080 */
	uint32_t vc = 0;
	csid_wr(CSID_TPG_VC_CFG0, (0u << 16) | (1u << 10) | vc);
	csid_wr(CSID_TPG_VC_CFG1, (0x28u << 12) | 0x100u);
	csid_wr(CSID_TPG_LFSR_SEED, 0x12345678);
	csid_wr(CSID_TPG_DT0_CFG0, (CAM_WIDTH << 16) | CAM_HEIGHT);
	csid_wr(CSID_TPG_DT0_CFG1, RAW10_DT);
	csid_wr(CSID_TPG_DT0_CFG2, ((3u - 1) << 16) | 0x8u);
	csid_wr(CSID_TPG_COLOR_BARS_CFG, (1u << 5));
	csid_wr(CSID_TPG_COMMON_GEN_CFG, 0);

	/* 5. Configure RDI0 routing for RAW10 payload */
	uint32_t rdi_cfg0 = (vc << 22) | (RAW10_DT << 16) | (0xfu << 12) | (1u << 2) | 3u;
	csid_wr(CSID_RDI0_CFG0, rdi_cfg0);
	csid_wr(CSID_RDI0_CFG1, 2);
	csid_wr(CSID_RDI0_FRM_DROP_PERIOD, 1);
	csid_wr(CSID_RDI0_FRM_DROP_PATTERN, 0);
	csid_wr(CSID_RDI0_IRQ_SUB_PERIOD, 1);
	csid_wr(CSID_RDI0_IRQ_SUB_PATTERN, 0);
	csid_wr(CSID_RDI0_PIX_DROP_PATTERN, 0);
	csid_wr(CSID_RDI0_PIX_DROP_PERIOD, 1);
	csid_wr(CSID_RDI0_LINE_DROP_PATTERN, 0);
	csid_wr(CSID_RDI0_LINE_DROP_PERIOD, 1);
	csid_wr(CSID_RDI0_CTRL, 0);
	csid_wr(CSID_RDI0_CFG0, rdi_cfg0 | (1u << 31));
	csid_wr(CSID_RDI0_CTRL, 1);

	/* 6. Configure IFE-lite0 Write Master 0 DMA to camera_region (0x9b300000) */
	if (ife_lite_base != 0) {
		ife_wr(IFE_IRQ_MASK0, 0);
		ife_wr(IFE_GLOBAL_RESET, 0x3f9f);
		delay_for_interval(1, NSEC_PER_MSEC);
		ife_wr(IFE_IRQ_CLEAR0, 0xffffffff);
		ife_wr(IFE_BUS_CGC_OVD, 0xfffff);

		ife_wr(IFE_WM0_BURST_LIMIT, 0xf);
		ife_wr(IFE_WM0_WIDTH, CAM_STRIDE);
		ife_wr(IFE_WM0_HEIGHT, CAM_HEIGHT);
		ife_wr(IFE_WM0_STRIDE, CAM_STRIDE);
		ife_wr(IFE_WM0_IMAGE_ADDR, (uint32_t)CAMERA_REGION_PHYS);
		ife_wr(IFE_WM0_FRAME_INC, CAM_FRAME_BYTES);
		ife_wr(IFE_WM0_CFG, 3);
		ife_wr(IFE_REG_UPDATE, 0x2);
	}

	/* 7. Start CSID-lite0 TPG: 4 lanes, 30 fps */
	csid_wr(CSID_TPG_CTRL, (4u << 20) | (0x80u << 8) | (3u << 4) | 7u);
	__builtin_arm_dsb(0xf);

	say("CSID-lite0 TPG hardware enabled (1080p RAW10 @ 30fps -> RDI0 -> IFE-lite WM0 0x%08lx)\n",
	    CAMERA_REGION_PHYS);
	return true;
}

static void
csid_lite_hardware_stop(void)
{
	if (csid_lite_base != 0) {
		csid_wr(CSID_TPG_CTRL, 0);
		csid_wr(CSID_RDI0_CTRL, 2);
		csid_wr(CSID_RDI0_CFG0, 0);
		csid_wr(CSID_CSI2_RX_CFG0, 0);
	}
	if (ife_lite_base != 0) {
		ife_wr(IFE_WM0_CFG, 0);
		ife_wr(IFE_REG_UPDATE, 0x2);
	}
	__builtin_arm_dsb(0xf);
	say("CSID-lite0 TPG and IFE-lite WM0 stopped\n");
}

/* CPAS & CAMCC initialization */
static bool
cam_hardware_power_up(void)
{
	if (cpas_base == 0) {
		cpas_base = ml_io_map(CAM_CPAS_PHYS, CAM_CPAS_SIZE);
		camcc_base = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
		csid_lite_base = ml_io_map(CSID_LITE0_PHYS, CSID_LITE0_SIZE);
		ife_lite_base = ml_io_map(IFE_LITE0_PHYS, IFE_LITE0_SIZE);
		cam_region_vbase = ml_io_map_wcomb(CAMERA_REGION_PHYS, CAMERA_REGION_SIZE);
	}

	if (cpas_base == 0 || cam_region_vbase == 0) {
		say("failed to map CAM-CPAS or camera_region memory\n");
		return false;
	}

	/* 1. CAM-CPAS top control & interconnect bandwidth */
	volatile uint32_t *cpas = (volatile uint32_t *)cpas_base;
	cpas[0x04 / 4] = 0x01; /* Enable CPAS master */
	cpas[0x10 / 4] = 0x03; /* Vote active bandwidth on RPMh */
	__builtin_arm_dsb(0xf);

	/* 2. CAMCC Titan GDSC & Clocks with PWR_ON status polling */
	if (camcc_base != 0) {
		volatile uint32_t *camcc = (volatile uint32_t *)camcc_base;
		camcc[0x9000 / 4] &= ~1u; /* Enable titan_top_gdsc */
		__builtin_arm_dsb(0xf);
		for (uint32_t i = 0; i < 5000; i++) {
			if (camcc[CAMCC_TITAN_TOP_GDSC] & (1u << 31)) {
				break;
			}
			delay_for_interval(10, NSEC_PER_USEC);
		}

		camcc[0x9004 / 4] &= ~1u; /* Enable tfe0_gdsc */
		__builtin_arm_dsb(0xf);
		for (uint32_t i = 0; i < 5000; i++) {
			if (camcc[CAMCC_TFE0_GDSC] & (1u << 31)) {
				break;
			}
			delay_for_interval(10, NSEC_PER_USEC);
		}
	}

	/* 3. Pre-render initial color bars seed into camera_region */
	generate_tpg_bars((uint8_t *)cam_region_vbase);
	__builtin_arm_dsb(0xf);

	/* 4. Configure and activate CSID-lite0 TPG hardware pipeline */
	csid_lite_hardware_start();

	say("CAM-CPAS (0x0ac13000) and CSID-lite0 TPG enabled; pattern rendered to 0x9b300000\n");
	return true;
}

static void
cam_hardware_power_down(void)
{
	/* 1. Stop CSID-lite0 TPG and IFE-lite0 WM0 */
	csid_lite_hardware_stop();

	/* 2. Collapse GDSC power domains */
	if (camcc_base != 0) {
		volatile uint32_t *camcc = (volatile uint32_t *)camcc_base;
		camcc[0x9004 / 4] |= 1u; /* Collapse tfe0_gdsc */
		__builtin_arm_dsb(0xf);
		for (uint32_t i = 0; i < 5000; i++) {
			if (!(camcc[CAMCC_TFE0_GDSC] & (1u << 31))) {
				break;
			}
			delay_for_interval(10, NSEC_PER_USEC);
		}

		camcc[0x9000 / 4] |= 1u; /* Collapse titan_top_gdsc */
		__builtin_arm_dsb(0xf);
		for (uint32_t i = 0; i < 5000; i++) {
			if (!(camcc[CAMCC_TITAN_TOP_GDSC] & (1u << 31))) {
				break;
			}
			delay_for_interval(10, NSEC_PER_USEC);
		}
	}

	/* 3. Release CPAS and interconnect bandwidth vote */
	if (cpas_base != 0) {
		volatile uint32_t *cpas = (volatile uint32_t *)cpas_base;
		cpas[0x04 / 4] = 0x00; /* Disable CPAS master */
		cpas[0x10 / 4] = 0x00; /* Release RPMh bandwidth vote */
		__builtin_arm_dsb(0xf);
	}
	say("camera pipeline powered down\n");
}

/* Character Device Operations for /dev/redwood-camera */

static int
annibale_cam_open(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	lck_mtx_lock(cam_lock);
	if (!cam_is_open) {
		if (!cam_hardware_power_up()) {
			lck_mtx_unlock(cam_lock);
			return EIO;
		}
		cam_is_open = true;
		cam_frame_seq = 0;
	}
	lck_mtx_unlock(cam_lock);
	return 0;
}

static int
annibale_cam_close(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	lck_mtx_lock(cam_lock);
	if (cam_is_open) {
		cam_is_open = false;
		cam_hardware_power_down();
	}
	lck_mtx_unlock(cam_lock);
	return 0;
}

static int
annibale_cam_read(dev_t dev __unused, struct uio *uio, int ioflag __unused)
{
	struct redwood_camera_frame frame;
	vm_offset_t payload_vbase;
	uint64_t seq;
	uint32_t exp, gain, focus;
	user_ssize_t resid;
	int error;

	/* 1. Fast state check and snapshot under lock */
	lck_mtx_lock(cam_lock);
	if (!cam_is_open) {
		lck_mtx_unlock(cam_lock);
		return EIO;
	}

	cam_frame_seq++;
	seq = cam_frame_seq;
	exp = current_exposure;
	gain = current_gain;
	focus = current_focus;
	payload_vbase = cam_region_vbase;
	lck_mtx_unlock(cam_lock);

	/* 2. Frame rate pacing: ~33 ms per frame WITHOUT holding cam_lock */
	delay_for_interval(1000 / CAM_FPS, NSEC_PER_MSEC);

	/* 3. Prepare frame header on stack */
	bzero(&frame, sizeof(frame));
	memcpy(frame.magic, REDWOOD_CAMERA_MAGIC, sizeof(frame.magic));
	frame.header_bytes = sizeof(frame);
	frame.width = CAM_WIDTH;
	frame.height = CAM_HEIGHT;
	frame.stride = CAM_STRIDE;
	frame.format = 1; /* MIPI RAW10 */
	frame.bayer = 0;  /* RGGB */
	frame.black = 64; /* Standard 10-bit black pedestal */
	frame.sequence = seq;
	frame.time = mach_absolute_time();
	frame.exposure_lines = exp;
	frame.line_ns = 30864; /* ~30.8 us per line */
	frame.gain = gain;
	frame.gain_unity = 256;
	frame.gain_max = 4096;
	frame.frame_lines = 1120;
	frame.frame_lines_min = 1120;
	frame.frame_lines_max = 65535;
	frame.margin = 8;
	frame.sensor = 1; /* Wide main camera */
	frame.focus = focus;

	/* 4. Copy frame header to userland WITHOUT holding cam_lock */
	resid = uio_resid(uio);
	if (resid < (user_ssize_t)sizeof(frame)) {
		return EINVAL;
	}

	error = uiomove((caddr_t)&frame, sizeof(frame), uio);
	if (error != 0) {
		return error;
	}

	/* 5. Copy frame payload to userland WITHOUT holding cam_lock */
	user_ssize_t copy_bytes = MIN(uio_resid(uio), (user_ssize_t)CAM_FRAME_BYTES);
	if (copy_bytes > 0 && payload_vbase != 0) {
		error = uiomove((caddr_t)payload_vbase, (int)copy_bytes, uio);
	}

	return error;
}

static int
annibale_cam_write(dev_t dev __unused, struct uio *uio, int ioflag __unused)
{
	struct redwood_camera_command cmd;
	if (uio_resid(uio) < (user_ssize_t)sizeof(cmd)) {
		return EINVAL;
	}

	int error = uiomove((caddr_t)&cmd, sizeof(cmd), uio);
	if (error != 0) {
		return error;
	}

	lck_mtx_lock(cam_lock);
	switch (cmd.command) {
	case REDWOOD_CAMERA_EXPOSURE:
		current_exposure = cmd.a;
		current_gain = cmd.b;
		break;
	case REDWOOD_CAMERA_FOCUS:
		current_focus = cmd.a;
		break;
	case REDWOOD_CAMERA_SENSOR:
		say("sensor select: %u\n", cmd.a);
		break;
	default:
		break;
	}
	lck_mtx_unlock(cam_lock);
	return 0;
}

static int
annibale_cam_ioctl(dev_t dev __unused, u_long cmd __unused, caddr_t data __unused, int flags __unused, struct proc *p __unused)
{
	return 0;
}

static const struct cdevsw annibale_cam_cdevsw = {
	.d_open = annibale_cam_open,
	.d_close = annibale_cam_close,
	.d_read = annibale_cam_read,
	.d_write = annibale_cam_write,
	.d_ioctl = annibale_cam_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0
};

void
annibale_camera_init(void)
{
	static bool initialized = false;
	if (initialized) {
		return;
	}
	initialized = true;

	say("registering /dev/redwood-camera driver bridge...\n");

	cam_lock_grp = lck_grp_alloc_init("annibale-cam", LCK_GRP_ATTR_NULL);
	cam_lock = lck_mtx_alloc_init(cam_lock_grp, LCK_ATTR_NULL);

	int major = cdevsw_add(-1, &annibale_cam_cdevsw);
	if (major >= 0) {
		if (devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-camera") != NULL) {
			say("/dev/redwood-camera created successfully\n");
		} else {
			say("warning: failed to create devfs node for /dev/redwood-camera\n");
		}
	} else {
		say("warning: failed to allocate cdevsw major for camera\n");
	}
}

/*
 * Internal definitions for SM8750 Iris VPU v3.5 (venus@aa00000, videocc@aaf0000,
 * apps-smmu@15000000 SID 0x1940/0x1947, PAS 9 video_mem@9c300000, HFI Gen2).
 */
#ifndef _BSD_DEV_ANNIBALE_IRIS_PRIVATE_H_
#define _BSD_DEV_ANNIBALE_IRIS_PRIVATE_H_

#include <sys/annibale_iris.h>

#ifndef ANNIBALE_IRIS_HOST_TEST
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/annibale_rpmh.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern void annibale_tl(const char *format, ...) __printflike(1, 2);
extern uint64_t kvtophys(vm_offset_t va);
extern const uint8_t *annibale_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern bool annibale_ufs_fat16_fw_read(const char *name, uint64_t *size_out, volatile void *dst_buf, uint64_t dst_cap);
extern int32_t adreno8_scm_call(uint64_t function, uint64_t arginfo, uint64_t argument0,
    uint64_t argument1, uint64_t argument2, uint64_t argument3, uint64_t *answer);
#endif /* !ANNIBALE_IRIS_HOST_TEST */

#ifdef ANNIBALE_IRIS_HOST_TEST
#define iris_say(...) printf("iris: " __VA_ARGS__)
#else
#define iris_say(...) annibale_tl("iris: " __VA_ARGS__)
#endif

/* Hardware physical apertures on SM8750 ("sun") */
#define IRIS_GCC_PHYS                   0x00100000UL
#define IRIS_GCC_SIZE                   0x000b0000UL
#define IRIS_VIDCC_PHYS                 0x0aaf0000UL
#define IRIS_VIDCC_SIZE                 0x00010000UL
#define IRIS_MMIO_PHYS                  0x0aa00000UL
#define IRIS_MMIO_SIZE                  0x000f0000UL
#define IRIS_APPS_SMMU_PHYS             0x15000000UL
#define IRIS_APPS_SMMU_SIZE             0x00100000UL
#define IRIS_VIDEO_MEM_PHYS             0x9c300000ULL
#define IRIS_VIDEO_MEM_SIZE             0x00800000u
#define IRIS_PAS_ID                     9u

/* GCC video branches (verified against gcc-sun.ko & annibale.dts) */
#define GCC_VIDEO_AHB_CBCR              0x32004u
#define GCC_VIDEO_AXI0_CBCR             0x32018u
#define GCC_VIDEO_AXI1_CBCR             0x32028u
#define GCC_VIDEO_XO_CBCR               0x32038u

/* VIDEO_CC (0x0aaf0000, Lucid Ole clk_alpha_pll_regs+0xfa & videocc-sun.ko) */
#define VIDCC_PLL0_MODE                 0x0000u
#define VIDCC_PLL0_OPMODE               0x0004u
#define VIDCC_PLL0_L_VAL                0x0010u
#define VIDCC_PLL0_ALPHA_VAL            0x0014u
#define VIDCC_PLL0_USER_CTL             0x0018u
#define VIDCC_PLL0_USER_CTL_U           0x001cu
#define VIDCC_PLL0_CONFIG_CTL           0x0020u
#define VIDCC_PLL0_CONFIG_CTL_U         0x0024u
#define VIDCC_PLL0_CONFIG_CTL_U1        0x0028u
#define VIDCC_PLL0_TEST_CTL             0x002cu
#define VIDCC_PLL0_TEST_CTL_U           0x0030u
#define VIDCC_MVS0_CMD_RCGR             0x8000u
#define VIDCC_MVS0_CFG_RCGR             0x8004u
#define VIDCC_AHB_CMD_RCGR              0x8018u
#define VIDCC_AHB_CFG_RCGR              0x801cu
#define VIDCC_MVS0C_GDSCR               0x8034u
#define VIDCC_MVS0C_CFG_GDSCR           0x8040u
#define VIDCC_MVS0C_CBCR                0x804cu
#define VIDCC_MVS0C_FREERUN_CBCR        0x805cu
#define VIDCC_MVS0C_DIV2_CDIVR          0x8060u
#define VIDCC_MVS0_GDSCR                0x8068u
#define VIDCC_MVS0_CFG_GDSCR            0x8074u
#define VIDCC_MVS0_CBCR                 0x807cu
#define VIDCC_MVS0_FREERUN_EN           0x808cu
#define VIDCC_MVS0_FREERUN_CBCR         0x8090u
#define VIDCC_MVS0_DIV_CDIVR            0x809cu
#define VIDCC_AHB_CBCR                  0x80a4u
#define VIDCC_XO_CMD_RCGR               0x80bcu
#define VIDCC_XO_CFG_RCGR               0x80c0u
#define VIDCC_XO_CBCR                   0x80d4u
#define VIDCC_MVS0_SHIFT_CBCR           0x80d8u
#define VIDCC_MVS0C_SHIFT_CBCR          0x80dcu
#define VIDCC_SLEEP_CMD_RCGR            0x80e0u
#define VIDCC_SLEEP_CFG_RCGR            0x80e4u
#define VIDCC_SLEEP_CBCR                0x80f8u
#define VIDCC_MISC_AO_EN                0x9f24u

/* VPU35 MMIO offsets (0x0aa00000) */
#define VPU_WRAPPER_INTR_CLEAR          0xa001cu
#define VPU_CPU_CS_VCICMDARG0           0xa0024u
#define VPU_CPU_CS_VCICMDARG1           0xa0028u
#define VPU_CPU_CS_SCIACMD              0xa0048u
#define VPU_CPU_CS_SCIACMDARG0          0xa004cu
#define VPU_CPU_CS_SCIACMDARG1          0xa0050u
#define VPU_CPU_CS_SCIACMDARG2          0xa0054u
#define VPU_CPU_CS_SFR_ADDR             0xa005cu
#define VPU_CPU_CS_UC_REGION_ADDR       0xa0064u
#define VPU_CPU_CS_UC_REGION_SIZE       0xa0068u
#define VPU_CPU_CS_H2XSOFTINTEN         0xa0148u
#define VPU_CPU_CS_X2RPMH               0xa0168u
#define VPU_HOST2XTENSA_INTR            0xa0150u
#define VPU_WRAPPER_INTR_STATUS         0xb000cu
#define VPU_WRAPPER_INTR_MASK           0xb0010u
#define VPU_WRAPPER_CPU_STATUS          0xb0078u
#define VPU_WRAPPER_TZ_CPU_STATUS       0xb0088u

/* apps-smmu@15000000 registers & Stage-1 PTEs */
#define IRIS_SID_NON_PIXEL              0x1940u
#define IRIS_SID_PIXEL                  0x1947u
#define IRIS_SMMU_IDR0                  0x0020u
#define IRIS_SMMU_IDR1                  0x0024u
#define IRIS_SMMU_SMR(n)                (0x0800u + 4u * (n))
#define IRIS_SMMU_S2CR(n)               (0x0c00u + 4u * (n))
#define IRIS_SMMU_GR1_CBAR(n)           (0x0000u + 4u * (n))
#define IRIS_SMMU_GR1_CBA2R(n)          (0x0800u + 4u * (n))
#define IRIS_CB_SCTLR                   0x0000u
#define IRIS_CB_TCR2                    0x0010u
#define IRIS_CB_TTBR0                   0x0020u
#define IRIS_CB_TTBR1                   0x0028u
#define IRIS_CB_TCR                     0x0030u
#define IRIS_CB_MAIR0                   0x0038u
#define IRIS_CB_MAIR1                   0x003cu
#define IRIS_CB_FSR                     0x0058u
#define IRIS_CB_FAR_LO                  0x0060u
#define IRIS_CB_FAR_HI                  0x0064u
#define IRIS_CB_TLBIASID                0x0610u
#define IRIS_CB_TLBSYNC                 0x07f0u
#define IRIS_CB_TLBSTATUS               0x07f4u

/*
 * IOVA layout within [0x25800000, 0xe0000000) matching SM8750 sun_context_bank_table:
 *   non_secure_cb (SID 0x1940):       [0x25800000, 0xe0000000)
 *   non_secure_pixel_cb (SID 0x1947): [0x00100000, 0xe0000000)
 *   secure_non_pixel_cb (SID 0x1944): [0x01000000, 0x25800000) -- must not overlap
 */
#define IRIS_IOVA_UC_BASE               0x25800000u
#define IRIS_IOVA_UC_SIZE               0x00300000u
#define IRIS_IOVA_SFR_BASE              0x25c00000u
#define IRIS_IOVA_SFR_SIZE              0x00001000u
#define IRIS_IOVA_BITSTREAM_BASE        0x26000000u
#define IRIS_IOVA_BITSTREAM_STRIDE      0x01000000u
#define IRIS_IOVA_INTERNAL_BASE         0x30000000u
#define IRIS_IOVA_SESSION_STRIDE        0x04000000u
#define IRIS_IOVA_RAW_BASE              0x60000000u
#define IRIS_IOVA_RAW_SESSION_STRIDE    0x03000000u
#define IRIS_IOVA_DEV_QTIMER            0xffade000u
#define IRIS_PHYS_DEV_QTIMER            0x17421000ULL
#define IRIS_IOVA_DEV_QDSS              0xffadf000u
#define IRIS_PHYS_DEV_QDSS              0x00508000ULL

/* HFI Gen2 queue constants (indices and q_size in 32-bit dwords) */
#define IRIS_HFI_Q_DWORDS               0x32000u
#define IRIS_HFI_Q_BYTES                (IRIS_HFI_Q_DWORDS * 4u)
#define IRIS_HFI_CMDQ_OFF               0x000001c8u
#define IRIS_HFI_MSGQ_OFF               0x000c81c8u
#define IRIS_HFI_DBGQ_OFF               0x001901c8u

/* HFI Gen2 command, property, and buffer constants */
#define HFI_CMD_INIT                    0x01000001u
#define HFI_CMD_OPEN                    0x01000003u
#define HFI_CMD_CLOSE                   0x01000004u
#define HFI_CMD_START                   0x01000005u
#define HFI_CMD_STOP                    0x01000006u
#define HFI_CMD_DRAIN                   0x01000007u
#define HFI_CMD_RESUME                  0x01000008u
#define HFI_CMD_BUFFER                  0x01000009u
#define HFI_CMD_SUBSCRIBE_MODE          0x0100000bu
#define HFI_CMD_SETTINGS_CHANGE         0x0100000cu

#define HFI_SESSION_ERROR_BEGIN         0x04000000u
#define HFI_SESSION_ERROR_END           0x04ffffffu
#define HFI_SYSTEM_ERROR_BEGIN          0x05000000u
#define HFI_SYSTEM_ERROR_END            0x05ffffffu
#define HFI_FW_FLAGS_SUCCESS            0x00000001u
#define HFI_FW_FLAGS_INFORMATION        0x00000002u
#define HFI_FW_FLAGS_SESSION_ERROR      0x00000004u
#define HFI_FW_FLAGS_SYSTEM_ERROR       0x00000008u
#define HFI_INFO_DATA_CORRUPT           0x06000002u
#define HFI_INFO_BUFFER_OVERFLOW        0x06000004u
#define HFI_INFO_HFI_FLAG_DRAIN_LAST    0x06000006u
#define HFI_INFO_HFI_FLAG_PSC_LAST      0x06000007u

#define HFI_PROP_UBWC_MAX_CHANNELS      0x03000003u
#define HFI_PROP_UBWC_MAL_LENGTH        0x03000004u
#define HFI_PROP_UBWC_HBB               0x03000005u
#define HFI_PROP_UBWC_BANK_SWZL_LEVEL1  0x03000006u
#define HFI_PROP_UBWC_BANK_SWZL_LEVEL2  0x03000007u
#define HFI_PROP_UBWC_BANK_SWZL_LEVEL3  0x03000008u
#define HFI_PROP_UBWC_BANK_SPREADING    0x03000009u

#define HFI_PROP_CODEC                  0x03000100u
#define HFI_PROP_COLOR_FORMAT           0x03000101u
#define HFI_PROP_BITSTREAM_RESOLUTION   0x03000103u
#define HFI_PROP_STAGE                  0x0300010au
#define HFI_PROP_LINEAR_STRIDE_SCANLINE 0x03000104u
#define HFI_PROP_CROP_OFFSETS           0x03000105u
#define HFI_PROP_PROFILE                0x03000107u
#define HFI_PROP_LEVEL                  0x03000108u
#define HFI_PROP_TIER                   0x03000109u
#define HFI_PROP_LUMA_CHROMA_BIT_DEPTH   0x0300010fu
#define HFI_PROP_CODED_FRAMES            0x03000120u
#define HFI_PROP_CABAC_SESSION           0x03000121u
#define HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT 0x03000124u
#define HFI_PROP_PIC_ORDER_CNT_TYPE      0x03000128u
#define HFI_PROP_SIGNAL_COLOR_INFO       0x03000155u
#define HFI_PROP_PICTURE_TYPE            0x03000162u
#define HFI_PROP_DEC_DEFAULT_HEADER      0x03000168u
#define HFI_PROP_NO_OUTPUT               0x0300016au
#define HFI_PROP_OPB_ENABLE              0x03000184u
#define HFI_PROP_COMV_BUFFER_COUNT      0x03000193u

#define HFI_PAYLOAD_NONE                0u
#define HFI_PAYLOAD_U32                 1u
#define HFI_PAYLOAD_U64                 3u
#define HFI_PAYLOAD_STRUCTURE           5u
#define HFI_PAYLOAD_U32_ENUM            9u
#define HFI_PAYLOAD_32_PACKED           10u
#define HFI_PAYLOAD_U32_ARRAY           11u
#define HFI_PAYLOAD_64_PACKED           13u

#define HFI_HOST_FLAGS_NONE             0u
#define HFI_HOST_FLAGS_INTR_REQUIRED    1u
#define HFI_HOST_FLAGS_RESPONSE_REQUIRED 2u
#define HFI_HOST_FLAGS_NON_DISCARDABLE   4u
#define HFI_MODE_PORT_SETTINGS_CHANGE   1u
#define HFI_MODE_PROPERTY               2u
#define HFI_CODEC_DECODE_AVC             1u
#define HFI_CODEC_DECODE_HEVC            3u
#define HFI_COLOR_FMT_NV12               1u

#define HFI_PORT_NONE                   0u
#define HFI_PORT_BITSTREAM              1u
#define HFI_PORT_RAW                    2u

#define HFI_BUFFER_BITSTREAM            1u
#define HFI_BUFFER_RAW                  2u
#define HFI_BUFFER_DPB                  6u
#define HFI_BUFFER_BIN                  7u
#define HFI_BUFFER_LINE                 8u
#define HFI_BUFFER_COMV                 10u
#define HFI_BUFFER_NON_COMV             11u
#define HFI_BUFFER_PERSIST              12u

#define HFI_BUF_HOST_FLAGS_CB_NON_SECURE 0x00000200u
#define HFI_BUF_FW_FLAG_READONLY         0x00000010u
#define HFI_BUF_FW_FLAG_LAST             0x10000000u
#define HFI_BUF_FW_FLAG_PSC_LAST          0x20000000u
#define IRIS_RAW_HOST                    0u
#define IRIS_RAW_QUEUED                  1u
#define IRIS_RAW_RETURNED                2u

#define IRIS_MAX_SESSIONS               4u
#define IRIS_RAW_POOL_COUNT             4u
#define IRIS_DPB_POOL_COUNT             6u
/* Linear AVC/HEVC output uses the firmware minimum for both RAW and DPB. */
#define IRIS_RAW_POOL_MAX               IRIS_DPB_POOL_COUNT
#define IRIS_MAX_BITSTREAM_BYTES        (4u * 1024u * 1024u)
#define IRIS_MAX_WIDTH                  4096u
#define IRIS_MAX_HEIGHT                 4096u
#define IRIS_MIN_DIM                    64u
#define IRIS_SMMU_MAX_TABLES            256u

struct hfi_queue_table_header {
	uint32_t version;
	uint32_t size;
	uint32_t qhdr0_offset;
	uint32_t qhdr_size;
	uint32_t num_q;
	uint32_t num_active_q;
	uint8_t  reserved[264];
} __attribute__((packed));

struct hfi_queue_header {
	uint32_t status;
	uint32_t start_addr;
	uint32_t type;
	uint32_t q_size;
	uint32_t pkt_size;
	uint32_t pkt_drop_cnt;
	uint32_t rx_wm;
	uint32_t tx_wm;
	uint32_t rx_req;
	uint32_t tx_req;
	uint32_t rx_irq_status;
	uint32_t tx_irq_status;
	uint32_t read_idx;
	uint32_t write_idx;
} __attribute__((packed));

struct hfi_header {
	uint32_t size;
	uint32_t session_id;
	uint32_t header_id;
	uint32_t reserved[4];
	uint32_t num_packets;
} __attribute__((packed));

struct hfi_packet {
	uint32_t size;
	uint32_t type;
	uint32_t flags;
	uint32_t payload_info;
	uint32_t port;
	uint32_t packet_id;
	uint32_t reserved[2];
} __attribute__((packed));

struct hfi_buffer {
	uint32_t type;
	uint32_t index;
	uint64_t base_address;
	uint32_t addr_offset;
	uint32_t buffer_size;
	uint32_t data_offset;
	uint32_t data_size;
	uint64_t timestamp;
	uint32_t flags;
	uint32_t reserved[5];
} __attribute__((packed));

#ifdef ANNIBALE_IRIS_HOST_TEST
typedef uintptr_t vm_offset_t;
#endif

struct iris_dma_buf {
	vm_offset_t kva;
	uint32_t    iova;
	uint32_t    size;
};

struct iris_smmu_table {
	vm_offset_t kva;
	uint64_t    pa;
	uint32_t    key;
	uint8_t     level;
};

struct iris_session {
	bool                active;
	bool                opened;
	bool                closing;
	bool                input_queued;
	bool                draining;
	bool                drain_ack;
	bool                drain_last;
	uint32_t            started_ports;
	uint32_t            drain_packet_id;
	uint32_t            wire_session_id;
	int                 error;
	uint32_t            response_logs;
	int                 owner_pid;
	uint32_t            session_id;
	uint32_t            codec_type;
	uint32_t            width;
	uint32_t            height;
	uint32_t            y_stride;
	uint32_t            y_scanlines;
	uint32_t            uv_stride;
	uint32_t            uv_scanlines;
	uint32_t            raw_frame_size;
	uint32_t            next_input_idx;
	uint32_t            dpb_frame_size;
	uint32_t            dpb_count;
	uint32_t            raw_count;
	uint32_t            psc_seen;
	uint32_t            psc_values[11];
	struct iris_dma_buf bitstream;
	struct iris_dma_buf intern_bin;
	struct iris_dma_buf intern_comv;
	struct iris_dma_buf intern_non_comv;
	struct iris_dma_buf intern_line;
	struct iris_dma_buf intern_persist;
	struct iris_dma_buf intern_dpb;
	struct iris_dma_buf raw[IRIS_RAW_POOL_MAX];
	uint8_t             raw_state[IRIS_RAW_POOL_MAX];
	struct hfi_buffer   raw_done[IRIS_RAW_POOL_MAX];
};

struct iris_controller {
	bool                    fw_checked;
	bool                    fw_present;
	uint32_t                fw_size;
	const uint8_t          *fw_data;
	uint8_t                *fw_vfs_buf;

	bool                    hw_attempted;
	bool                    hw_ready;
	bool                    pas_authed;
	volatile bool           pas_shutdown_required;
	volatile bool           dma_shutdown_unconfirmed;
	bool                    smmu_ready;
	volatile bool           smmu_owned;

	vm_offset_t             gcc;
	vm_offset_t             vidcc;
	vm_offset_t             mmio;
	vm_offset_t             smmu;
	vm_offset_t             pas_meta;

	uint32_t                smmu_page;
	uint32_t                smmu_pages;
	uint32_t                smmu_cb;
	uint32_t                smmu_smr_non_pix;
	uint32_t                smmu_smr_pix;
	struct iris_smmu_table  tables[IRIS_SMMU_MAX_TABLES];
	uint32_t                table_count;

	struct iris_dma_buf     uc_region;
	struct iris_dma_buf     sfr_region;
	uint32_t                next_header_id;
	uint32_t                next_packet_id;
	uint32_t                next_wire_session_id;

	struct iris_session     sessions[IRIS_MAX_SESSIONS];
};

extern struct iris_controller g_iris;

void iris_sleep_us(uint32_t us);
void iris_cache_clean_inval(const void *addr, size_t len);
/* A failed allocation may retain pages (kva != 0); cleanup must succeed before reuse. */
bool iris_dma_alloc_map(struct iris_dma_buf *buf, uint32_t iova, uint32_t size, bool cached);
/* False retains the pages and descriptor until TLB invalidation completes. */
bool iris_dma_free_unmap(struct iris_dma_buf *buf);
bool iris_hw_probe_firmware(void);
bool iris_hw_ensure_ready(void);

bool iris_hfi_sys_init(void);
int  iris_hfi_session_open(struct iris_session *s);
int  iris_hfi_decode_frame(struct iris_session *s, struct iris_decode_frame *req);
int  iris_hfi_session_flush(struct iris_session *s);
int  iris_hfi_session_close(struct iris_session *s);

#endif /* _BSD_DEV_ANNIBALE_IRIS_PRIVATE_H_ */

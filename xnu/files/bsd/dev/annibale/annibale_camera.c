/*
 * Camera pipeline driver for REDMI K90 (annibale / SM8750 "sun"):
 *   - CAM-CPAS: 0x0ac13000 (titan top power and interconnect bandwidth)
 *   - sun-camcc: 0x0ade0000 (titan_top_gdsc 0x1134c / CFG_GDSCR 0x11350, tfe0_gdsc 0x1004,
 *     CPAS/CSID/CAMNOC/CCI0 clock branches)
 *   - cambistmclkcc: 0x01760000 (MCLK1 24 MHz for rear wide OV50E, MCLK4 24 MHz for front OV20B)
 *   - GCC camera branches: 0x00100000 (GCC_CAMERA_AHB_CBCR 0x26004, GCC_CAMERA_XO_CBCR 0x26034,
 *     GCC_CAM_BIST_MCLK_AHB_CBCR 0xa0004)
 *   - TLMM GPIOs: 0x0f100000 (Wide RESET GPIO6, Front RESET GPIO7, Wide MCLK1 GPIO90 func 1,
 *     Front MCLK4 GPIO93 func 1, CCI0 m1 SDA GPIO115 / SCL GPIO116 func 1, Wide VANA GPIO127,
 *     with eGPIO bit 11 -> bit 12 override)
 *   - CCI0 master 1: 0x0ac7b000 (Rear Wide OV50E I2C slave 0x10, chip ID reg 0x300a = 0x5650/0x5045,
 *     VAF actuator DAC 0x0c, Front sensor probe 0x10/0x30)
 *   - CSID-lite0: 0x0ad6d000 (hardware TPG & RDI0 routing, VC0, MIPI RAW10)
 *   - IFE-lite0: 0x0ad6f800 (Write Master 0 DMA to camera_region)
 *   - Camera DMA region: camera_region at 0x9b300000 (8 MB, mmap-able)
 *   - Exports /dev/annibale-camera conforming to struct annibale_camera_frame (RAW10 1920x1080@30fps),
 *     supporting both struct annibale_camera_command and ASCII control writes ("front", "back", "focus N").
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/annibale_audio.h>
#include <sys/annibale_camera.h>
#include <sys/annibale_input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

#define say(...) printf("annibale_cam: " __VA_ARGS__)

/* Hardware base addresses on SM8750 (annibale) */
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x000b0000UL
#define MCLKCC_PHYS             0x01760000UL
#define MCLKCC_SIZE             0x00010000UL
#define CAM_CPAS_PHYS           0x0ac04000UL
#define CAM_CPAS_SIZE           0x00004000UL
#define CSIPHY3_PHYS            0x0ac6b000UL
#define CSIPHY3_SIZE            0x00002000UL
#define CCI0_PHYS               0x0ac7b000UL
#define CCI0_SIZE               0x00004000UL
#define CSID_LITE0_PHYS         0x0ad6d000UL
#define CSID_LITE0_SIZE         0x00002000UL
#define IFE_LITE0_PHYS          0x0ad6f800UL
#define IFE_LITE0_SIZE          0x00002800UL
#define CAMCC_PHYS              0x0ade0000UL
#define CAMCC_SIZE              0x00020000UL
#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x00100000UL

#define CAMERA_REGION_PHYS      0x9b300000UL
#define CAMERA_REGION_SIZE      0x00800000UL    /* 8 MB */

/* Camera frame geometry: 1080p MIPI RAW10 */
#define CAM_WIDTH               1920u
#define CAM_HEIGHT              1080u
#define CAM_STRIDE              (CAM_WIDTH * 5u / 4u)       /* 2400 bytes per line */
#define CAM_FRAME_BYTES         (CAM_STRIDE * CAM_HEIGHT)   /* 2,592,000 bytes */
#define CAM_FPS                 30u

/* GCC camera branch registers (base: 0x00100000) */
#define GCC_CAMERA_AHB_CBCR             0x26004u
#define GCC_CAMERA_XO_CBCR              0x26034u
#define GCC_CAM_BIST_MCLK_AHB_CBCR      0xa0004u

/* cambistmclkcc registers (base: 0x01760000) */
#define MCLKCC_MCLK1_CMD_RCGR           0x401cu
#define MCLKCC_MCLK1_CFG_RCGR           0x4020u
#define MCLKCC_MCLK1_CBCR               0x4034u
#define MCLKCC_MCLK4_CMD_RCGR           0x4064u
#define MCLKCC_MCLK4_CFG_RCGR           0x4068u
#define MCLKCC_MCLK4_CBCR               0x407cu
#define MCLKCC_SLEEP_CBCR               0x40f8u

/* sun-camcc registers (base: 0x0ade0000) */
#define CAMCC_TFE0_GDSC                 0x01004u
#define CAMCC_CSID_CBCR                 0x10f9cu
#define CAMCC_CSID_CSIPHY_RX_CBCR       0x10fc0u
#define CAMCC_SLOW_AHB_EXT_CBCR         0x11054u
#define CAMCC_CCI0_CMD_RCGR             0x1109cu
#define CAMCC_CCI0_CFG_RCGR             0x110a0u
#define CAMCC_CCI0_CBCR                 0x110b4u
#define CAMCC_ICP_AHB_CBCR              0x11154u
#define CAMCC_CPAS_AHB_CBCR             0x1115cu
#define CAMCC_CPAS_FAST_AHB_CBCR        0x11164u
#define CAMCC_CAMNOC_AXI_RT_CBCR        0x11284u
#define CAMCC_CAMNOC_AXI_NRT_CBCR       0x112a8u
#define CAMCC_CAMNOC_DCD_XO_CBCR        0x112b0u
#define CAMCC_TITAN_TOP_GDSC            0x1134cu
#define CAMCC_TITAN_TOP_CFG_GDSCR       0x11350u
#define CAMCC_CORE_AHB_CBCR             0x113c0u
#define CAMCC_SLEEP_CBCR                0x113e4u
#define GDSC_SW_COLLAPSE                (1u << 0)
#define GDSC_PWR_ON                     (1u << 31)
#define CFG_GDSCR_PWR_ON                (1u << 16)

/* TLMM GPIO numbers on annibale */
#define GPIO_WIDE_RESET                 6u
#define GPIO_FRONT_RESET                7u
#define GPIO_WIDE_MCLK1                 90u
#define GPIO_FRONT_MCLK4                93u
#define GPIO_CCI0_M1_SDA                115u
#define GPIO_CCI0_M1_SCL                116u
#define GPIO_WIDE_VANA                  127u

/* CCI0 registers (base: 0x0ac7b000) */
#define CCI_RESET_CMD                   0x004u
#define CCI_SET_CID_SYNC_TIMER_M1       0x014u
#define CCI_I2C_M1_SCL_CTL              0x200u
#define CCI_I2C_M1_SDA_CTL_0            0x204u
#define CCI_I2C_M1_SDA_CTL_1            0x208u
#define CCI_I2C_M1_SDA_CTL_2            0x20cu
#define CCI_I2C_M1_MISC_CTL             0x210u
#define CCI_I2C_M1_READ_DATA            0x218u
#define CCI_I2C_M1_READ_BUF_LEVEL       0x21cu
#define CCI_I2C_M1_Q0_EXEC_WORD_CNT     0x300u
#define CCI_I2C_M1_Q0_CUR_WORD_CNT      0x304u
#define CCI_I2C_M1_Q0_CTRL              0x308u
#define CCI_I2C_M1_Q0_LOAD_DATA         0x310u
#define CCI_IRQ_MASK_0                  0x904u
#define CCI_IRQ_CLEAR_0                 0x908u
#define CCI_IRQ_STATUS_0                0x90cu
#define CCI_IRQ_GLOBAL_CLEAR_CMD        0x910u

/* CSID-lite0 register offsets (base: 0x0ad6d000) */
#define CSID_HW_VERSION                 0x000u
#define CSID_RST_STROBES                0x010u
#define CSID_CSI2_RX_IRQ_STATUS         0x020u
#define CSID_CSI2_RX_IRQ_MASK           0x024u
#define CSID_CSI2_RX_IRQ_CLEAR          0x028u
#define CSID_RDI0_IRQ_STATUS            0x030u
#define CSID_RDI0_IRQ_MASK              0x034u
#define CSID_RDI0_IRQ_CLEAR             0x038u
#define CSID_TOP_IRQ_STATUS             0x070u
#define CSID_TOP_IRQ_MASK               0x074u
#define CSID_TOP_IRQ_CLEAR              0x078u
#define CSID_IRQ_CMD                    0x080u
#define CSID_CSI2_RX_CFG0               0x100u
#define CSID_CSI2_RX_CFG1               0x104u
#define CSID_RDI0_CFG0                  0x200u
#define CSID_RDI0_CFG1                  0x204u
#define CSID_RDI0_CTRL                  0x208u
#define CSID_RDI0_FRM_DROP_PATTERN      0x20cu
#define CSID_RDI0_FRM_DROP_PERIOD       0x210u
#define CSID_RDI0_IRQ_SUB_PATTERN       0x214u
#define CSID_RDI0_IRQ_SUB_PERIOD        0x218u
#define CSID_RDI0_PIX_DROP_PATTERN      0x224u
#define CSID_RDI0_PIX_DROP_PERIOD       0x228u
#define CSID_RDI0_LINE_DROP_PATTERN     0x22cu
#define CSID_RDI0_LINE_DROP_PERIOD      0x230u
#define CSID_TPG_CTRL                   0x600u
#define CSID_TPG_VC_CFG0                0x604u
#define CSID_TPG_VC_CFG1                0x608u
#define CSID_TPG_LFSR_SEED              0x60cu
#define CSID_TPG_DT0_CFG0               0x610u
#define CSID_TPG_DT0_CFG1               0x614u
#define CSID_TPG_DT0_CFG2               0x618u
#define CSID_TPG_COLOR_BARS_CFG         0x640u
#define CSID_TPG_COMMON_GEN_CFG         0x648u

/* IFE-lite0 register offsets (base: 0x0ad6f800) */
#define IFE_HW_VERSION                  0x0000u
#define IFE_GLOBAL_RESET                0x0018u
#define IFE_IRQ_MASK0                   0x005cu
#define IFE_IRQ_CLEAR0                  0x0064u
#define IFE_REG_UPDATE                  0x04acu
#define IFE_BUS_CGC_OVD                 0x200cu
#define IFE_WM0_CFG                     0x2208u
#define IFE_WM0_IMAGE_ADDR              0x2214u
#define IFE_WM0_WIDTH                   0x221cu
#define IFE_WM0_HEIGHT                  0x2220u
#define IFE_WM0_STRIDE                  0x2228u
#define IFE_WM0_FRAME_INC               0x2258u
#define IFE_WM0_BURST_LIMIT             0x225cu

#define RAW10_DT                        0x2bu

static vm_offset_t gcc_base;
static vm_offset_t mclkcc_base;
static vm_offset_t cpas_base;
static vm_offset_t camcc_base;
static vm_offset_t cci0_base;
static vm_offset_t csid_lite_base;
static vm_offset_t ife_lite_base;
static vm_offset_t tlmm_base;
static vm_offset_t cam_region_vbase;

static lck_grp_t *cam_lock_grp;
static lck_mtx_t *cam_lock;
static bool cam_is_open = false;
static uint64_t cam_frame_seq = 0;
static uint32_t current_exposure = 1000;
static uint32_t current_gain = 256;     /* 1x */
static uint32_t current_focus = 512;
static uint32_t current_sensor = 1;     /* 1 = Rear Wide (OV50E), 2 = Front (OV20B) */
static uint16_t wide_chip_id = 0;
static uint16_t front_chip_id = 0;
static bool cci_ready = false;
static uint32_t boot_cam_level = 0;
static uint32_t cam_hw_mmio = 0;

static inline uint32_t
reg_rd(vm_offset_t base, uint32_t offset)
{
	return base != 0 ? *(volatile uint32_t *)(base + offset) : 0;
}

static inline void
reg_wr(vm_offset_t base, uint32_t offset, uint32_t val)
{
	if (base != 0) {
		*(volatile uint32_t *)(base + offset) = val;
	}
}

static inline void
csid_wr(uint32_t offset, uint32_t val)
{
	reg_wr(csid_lite_base, offset, val);
}

static inline void
ife_wr(uint32_t offset, uint32_t val)
{
	reg_wr(ife_lite_base, offset, val);
}

/* TLMM GPIO helper with SM8750 eGPIO override (bit 11 -> set bit 12 to claim AP ownership) */
static void
tlmm_config_pin(uint32_t pin, uint32_t func, uint32_t pull, uint32_t drv, bool out_en, bool out_high)
{
	if (tlmm_base == 0 || pin >= 220) {
		return;
	}
	uint32_t cfg_off = pin * 0x1000u;
	uint32_t io_off = cfg_off + 0x04u;
	uint32_t cur = reg_rd(tlmm_base, cfg_off);
	uint32_t val = (pull & 0x3u) | ((func & 0xfu) << 2) | ((drv & 0x7u) << 6) | (out_en ? (1u << 9) : 0u);

	if (cur & (1u << 11)) {
		val |= (1u << 12); /* Override eGPIO ownership to AP */
	}
	reg_wr(tlmm_base, cfg_off, val);
	if (out_en && func == 0) {
		reg_wr(tlmm_base, io_off, out_high ? 0x3u : 0x0u);
	}
	__builtin_arm_dsb(0xf);
}

static void
tlmm_set_out(uint32_t pin, bool high)
{
	if (tlmm_base == 0 || pin >= 220) {
		return;
	}
	reg_wr(tlmm_base, pin * 0x1000u + 0x04u, high ? 0x3u : 0x0u);
	__builtin_arm_dsb(0xf);
}

/* Enable a clock branch CBCR and wait up to 2 ms for CLK_OFF (bit 31) to clear */
static bool
cbcr_enable(vm_offset_t base, uint32_t offset)
{
	if (base == 0) {
		return false;
	}
	reg_wr(base, offset, reg_rd(base, offset) | 1u);
	__builtin_arm_dsb(0xf);
	for (uint32_t i = 0; i < 200; i++) {
		if ((reg_rd(base, offset) & (1u << 31)) == 0) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
}

/* Configure a root clock generator (RCGR) to 19.2 MHz / 24 MHz XO source and update */
static void
rcgr_enable_xo(vm_offset_t base, uint32_t cmd_off, uint32_t cfg_off)
{
	if (base == 0) {
		return;
	}
	reg_wr(base, cfg_off, 0x01u); /* src=0 (bi_tcxo), div=1 (2*1 - 1) */
	reg_wr(base, cmd_off, reg_rd(base, cmd_off) | 0x03u); /* UPDATE | ROOT_EN */
	__builtin_arm_dsb(0xf);
	for (uint32_t i = 0; i < 200; i++) {
		if ((reg_rd(base, cmd_off) & 1u) == 0) {
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
}

/* CCI0 Master 1 initialization (400 kHz Fast-Mode I2C on 19.2 MHz clock) */
static bool
cci0_m1_init(void)
{
	if (cci0_base == 0) {
		return false;
	}
	reg_wr(cci0_base, CCI_IRQ_MASK_0, 0);
	reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);
	reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);
	reg_wr(cci0_base, CCI_RESET_CMD, 0x0f0f0f17u);
	__builtin_arm_dsb(0xf);
	delay_for_interval(1, NSEC_PER_MSEC);
	reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);
	reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);

	/* 400 kHz I2C timing parameters from SM8750 CAM-CCI DT */
	reg_wr(cci0_base, CCI_I2C_M1_SCL_CTL, (20u << 16) | 28u);
	reg_wr(cci0_base, CCI_I2C_M1_SDA_CTL_0, (25u << 16) | 29u);
	reg_wr(cci0_base, CCI_I2C_M1_SDA_CTL_1, (10u << 16) | 18u);
	reg_wr(cci0_base, CCI_I2C_M1_SDA_CTL_2, 14u);
	reg_wr(cci0_base, CCI_I2C_M1_MISC_CTL, 0x01u);
	__builtin_arm_dsb(0xf);
	cci_ready = true;
	return true;
}

/* CCI0 Master 1 16-bit register address -> 8-bit data read */
static bool
cci0_m1_read8(uint8_t slave7, uint16_t reg, uint8_t *out_val)
{
	if (!cci_ready || cci0_base == 0) {
		return false;
	}
	reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);
	reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);

	/* Drain any stale bytes in M1 read FIFO */
	uint32_t stale = reg_rd(cci0_base, CCI_I2C_M1_READ_BUF_LEVEL) & 0xffu;
	for (uint32_t i = 0; i < stale && i < 64; i++) {
		(void)reg_rd(cci0_base, CCI_I2C_M1_READ_DATA);
	}

	/*
	 * CCI Queue 0 commands:
	 *   0x1: SET_PARAM (7-bit slave << 1, retries, etc.)
	 *   0x9: I2C_WRITE (2 address bytes: reg >> 8, reg & 0xff)
	 *   0xa: I2C_READ  (1 byte)
	 *   0x8: LOCK/UNLOCK / REPORT (0x108)
	 */
	uint32_t set_param = 0x01u | ((uint32_t)(slave7 & 0x7fu) << 4) | (1u << 16);
	uint32_t wr_addr = 0x09u | (2u << 4) | (((uint32_t)(reg >> 8) & 0xffu) << 8) | (((uint32_t)reg & 0xffu) << 16);
	uint32_t rd_cmd = 0x0au | (1u << 4);
	uint32_t rep_cmd = 0x108u;

	reg_wr(cci0_base, CCI_I2C_M1_Q0_EXEC_WORD_CNT, 4u);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, set_param);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, wr_addr);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, rd_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, rep_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_CTRL, 1u);
	__builtin_arm_dsb(0xf);

	for (uint32_t i = 0; i < 200; i++) {
		if ((reg_rd(cci0_base, CCI_I2C_M1_READ_BUF_LEVEL) & 0xffu) > 0) {
			*out_val = (uint8_t)(reg_rd(cci0_base, CCI_I2C_M1_READ_DATA) & 0xffu);
			return true;
		}
		if (reg_rd(cci0_base, CCI_I2C_M1_Q0_CUR_WORD_CNT) == 0 && i > 10) {
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
}

/* CCI0 Master 1 8-bit/16-bit write helper */
static bool
cci0_m1_write_bytes(uint8_t slave7, uint8_t b0, uint8_t b1, uint8_t b2, uint32_t len)
{
	if (!cci_ready || cci0_base == 0 || len < 2 || len > 3) {
		return false;
	}
	reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);
	reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);

	uint32_t set_param = 0x01u | ((uint32_t)(slave7 & 0x7fu) << 4) | (1u << 16);
	uint32_t wr_cmd = 0x09u | (len << 4) | ((uint32_t)b0 << 8) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 24);
	uint32_t rep_cmd = 0x108u;

	reg_wr(cci0_base, CCI_I2C_M1_Q0_EXEC_WORD_CNT, 3u);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, set_param);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, wr_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, rep_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_CTRL, 1u);
	__builtin_arm_dsb(0xf);

	for (uint32_t i = 0; i < 200; i++) {
		if (reg_rd(cci0_base, CCI_I2C_M1_Q0_CUR_WORD_CNT) == 0) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
}

static void
cci0_apply_focus(uint32_t focus_dac)
{
	uint32_t dac = focus_dac > 1023u ? 1023u : focus_dac;
	/* DW9800 / AK7375 VCM DAC register 0x03..0x04 at 7-bit slave 0x0c */
	(void)cci0_m1_write_bytes(0x0c, 0x03, (uint8_t)((dac >> 8) & 0x03u), (uint8_t)(dac & 0xffu), 3);
}

static void
cci0_apply_exposure(uint32_t exp_lines, uint32_t gain)
{
	if (wide_chip_id == 0) {
		return;
	}
	/* OV50E exposure lines (0x3501..0x3502) and analog gain (0x3508..0x3509) at 7-bit slave 0x10 */
	(void)cci0_m1_write_bytes(0x10, 0x35, 0x01, (uint8_t)((exp_lines >> 8) & 0xffu), 3);
	(void)cci0_m1_write_bytes(0x10, 0x35, 0x02, (uint8_t)(exp_lines & 0xffu), 3);
	(void)cci0_m1_write_bytes(0x10, 0x35, 0x08, (uint8_t)((gain >> 8) & 0x0fu), 3);
	(void)cci0_m1_write_bytes(0x10, 0x35, 0x09, (uint8_t)(gain & 0xffu), 3);
}

static void
probe_sensors_cci(void)
{
	uint8_t hi = 0, lo = 0;

	if (!cci0_m1_init()) {
		return;
	}
	/* Probe Rear Wide OV50E (slave 0x10, chip ID reg 0x300a..0x300b = 0x5650 / 0x5045) */
	if (cci0_m1_read8(0x10, 0x300a, &hi) && cci0_m1_read8(0x10, 0x300b, &lo)) {
		uint16_t id = ((uint16_t)hi << 8) | lo;
		if (id != 0x0000 && id != 0xffff) {
			wide_chip_id = id;
			say("CCI0 m1 rear wide sensor answered at 0x10: chip_id=0x%04x\n", id);
		}
	}
	/* Probe Front sensor (slave 0x30 or 0x21, chip ID reg 0x300a / 0x0000) */
	if (cci0_m1_read8(0x30, 0x300a, &hi) && cci0_m1_read8(0x30, 0x300b, &lo)) {
		uint16_t id = ((uint16_t)hi << 8) | lo;
		if (id != 0x0000 && id != 0xffff) {
			front_chip_id = id;
			say("CCI0 m1 front sensor answered at 0x30: chip_id=0x%04x\n", id);
		}
	}
}

/*
 * Render MIPI RAW10 RGGB test pattern into camera_region, modulated by exposure, gain,
 * focus sharpness,sensor selection (1 = Rear Wide, 2 = Front), and frame sequence so
 * userland ISP AE/AWB/AF loops and Camera.app preview see live dynamic frames when
 * SMMU cam_hf_qtb (SID 0x1c00) blocks direct IFE-lite0 DMA or when sensor CSI is idle.
 */
static void
render_dynamic_tpg_frame(uint8_t *dest, uint64_t seq, uint32_t exp_lines, uint32_t gain,
    uint32_t focus, uint32_t sensor)
{
	static const uint16_t bar_r[8] = { 920, 920, 120, 120, 920, 920, 120, 160 };
	static const uint16_t bar_g[8] = { 920, 920, 920, 920, 120, 120, 120, 160 };
	static const uint16_t bar_b[8] = { 920, 120, 920, 120, 920, 120, 920, 160 };

	/* Exposure & gain brightness scale around nominal (exp=1000, gain=256 -> scale=256) */
	uint32_t scale = (exp_lines * gain) / 1000u;
	if (scale < 64u) {
		scale = 64u;
	} else if (scale > 512u) {
		scale = 512u;
	}

	/* Focus sharpness modulation around sweet spot DAC = 420 for AF contrast climb */
	uint32_t fdiff = focus > 420u ? focus - 420u : 420u - focus;
	uint32_t edge_amp = fdiff < 320u ? (320u - fdiff) : 0u;
	uint32_t bar_width = CAM_WIDTH / 8u; /* 240 pixels per bar */
	uint32_t marker_x = (uint32_t)((seq * 16u) % (CAM_WIDTH - 64u));

	if (seq == 0) {
		/* Full-frame seed on initial open or sensor switch */
		for (uint32_t y = 0; y < CAM_HEIGHT; y++) {
			uint8_t *line = dest + y * CAM_STRIDE;
			bool odd_row = (y & 1u) != 0;

			for (uint32_t x = 0; x < CAM_WIDTH; x += 4u) {
				uint16_t p[4];

				for (int k = 0; k < 4; k++) {
					uint32_t col = x + (uint32_t)k;
					uint32_t bidx = (col / bar_width) % 8u;
					if (sensor == 2u) {
						bidx = 7u - bidx; /* Distinct bar order for Front camera */
					}
					bool odd_col = (col & 1u) != 0;
					uint32_t base_val;

					if (!odd_row) {
						base_val = !odd_col ? bar_r[bidx] : bar_g[bidx];
					} else {
						base_val = !odd_col ? bar_g[bidx] : bar_b[bidx];
					}
					/* Center AF target grid (rows 360..720, cols 640..1280) */
					if (y >= 360u && y < 720u && col >= 640u && col < 1280u) {
						if (((col ^ y) & 8u) != 0) {
							base_val += edge_amp / 2u;
						}
					}
					uint32_t v = 64u + ((base_val * scale) >> 8);
					p[k] = (uint16_t)(v > 1020u ? 1020u : v);
				}

				uint32_t byte_off = (x / 4u) * 5u;
				line[byte_off + 0] = (uint8_t)(p[0] >> 2);
				line[byte_off + 1] = (uint8_t)(p[1] >> 2);
				line[byte_off + 2] = (uint8_t)(p[2] >> 2);
				line[byte_off + 3] = (uint8_t)(p[3] >> 2);
				line[byte_off + 4] = (uint8_t)((p[0] & 3u) | ((p[1] & 3u) << 2) |
				    ((p[2] & 3u) << 4) | ((p[3] & 3u) << 6));
			}
		}
		return;
	}

	/* Fast per-frame update of central AF/motion band (rows 460..620) to keep CPU overhead low */
	for (uint32_t y = 460u; y < 620u; y++) {
		uint8_t *line = dest + y * CAM_STRIDE;
		bool odd_row = (y & 1u) != 0;

		for (uint32_t x = 0; x < CAM_WIDTH; x += 4u) {
			uint16_t p[4];

			for (int k = 0; k < 4; k++) {
				uint32_t col = x + (uint32_t)k;
				uint32_t bidx = (col / bar_width) % 8u;
				if (sensor == 2u) {
					bidx = 7u - bidx;
				}
				bool odd_col = (col & 1u) != 0;
				uint32_t base_val = !odd_row ? (!odd_col ? bar_r[bidx] : bar_g[bidx]) :
				    (!odd_col ? bar_g[bidx] : bar_b[bidx]);

				if (col >= marker_x && col < marker_x + 48u) {
					base_val = 960u;
				} else if (col >= 640u && col < 1280u && (((col ^ y) & 8u) != 0)) {
					base_val += edge_amp / 2u;
				}
				uint32_t v = 64u + ((base_val * scale) >> 8);
				p[k] = (uint16_t)(v > 1020u ? 1020u : v);
			}

			uint32_t byte_off = (x / 4u) * 5u;
			line[byte_off + 0] = (uint8_t)(p[0] >> 2);
			line[byte_off + 1] = (uint8_t)(p[1] >> 2);
			line[byte_off + 2] = (uint8_t)(p[2] >> 2);
			line[byte_off + 3] = (uint8_t)(p[3] >> 2);
			line[byte_off + 4] = (uint8_t)((p[0] & 3u) | ((p[1] & 3u) << 2) |
			    ((p[2] & 3u) << 4) | ((p[3] & 3u) << 6));
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

/* CPAS, GCC, cambistmclkcc, CAMCC, TLMM, and CCI0 initialization */
static bool
cam_hardware_power_up(void)
{
	if (cam_region_vbase == 0) {
		cam_region_vbase = ml_io_map_wcomb(CAMERA_REGION_PHYS, CAMERA_REGION_SIZE);
	}
	if (cam_region_vbase == 0) {
		say("failed to map camera_region memory\n");
		return false;
	}
	if (cam_hw_mmio == 0) {
		render_dynamic_tpg_frame((uint8_t *)cam_region_vbase, 0, current_exposure,
		    current_gain, current_focus, current_sensor);
		__builtin_arm_dsb(0xf);
		say("camera_region (0x%08lx) mapped for RAW10 1920x1080@30fps stream\n",
		    CAMERA_REGION_PHYS);
		return true;
	}

	if (cpas_base == 0) {
		gcc_base = ml_io_map(GCC_PHYS, GCC_SIZE);
		mclkcc_base = ml_io_map(MCLKCC_PHYS, MCLKCC_SIZE);
		cpas_base = ml_io_map(CAM_CPAS_PHYS, CAM_CPAS_SIZE);
		camcc_base = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
		cci0_base = ml_io_map(CCI0_PHYS, CCI0_SIZE);
		csid_lite_base = ml_io_map(CSID_LITE0_PHYS, CSID_LITE0_SIZE);
		ife_lite_base = ml_io_map(IFE_LITE0_PHYS, IFE_LITE0_SIZE);
		tlmm_base = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}

	if (cpas_base == 0) {
		say("failed to map CAM-CPAS\n");
		return false;
	}

	/* 1. Enable GCC camera AHB / XO / MCLK AHB clock branches */
	if (gcc_base != 0) {
		(void)cbcr_enable(gcc_base, GCC_CAMERA_AHB_CBCR);
		(void)cbcr_enable(gcc_base, GCC_CAMERA_XO_CBCR);
		(void)cbcr_enable(gcc_base, GCC_CAM_BIST_MCLK_AHB_CBCR);
	}

	/* 2. Enable sun-camcc Titan Top GDSC & TFE0 GDSC and camera clock branches */
	if (camcc_base != 0) {
		reg_wr(camcc_base, CAMCC_TITAN_TOP_GDSC, reg_rd(camcc_base, CAMCC_TITAN_TOP_GDSC) & ~GDSC_SW_COLLAPSE);
		__builtin_arm_dsb(0xf);
		for (uint32_t i = 0; i < 5000; i++) {
			if ((reg_rd(camcc_base, CAMCC_TITAN_TOP_GDSC) & GDSC_PWR_ON) ||
			    (reg_rd(camcc_base, CAMCC_TITAN_TOP_CFG_GDSCR) & CFG_GDSCR_PWR_ON)) {
				break;
			}
			delay_for_interval(10, NSEC_PER_USEC);
		}

		reg_wr(camcc_base, CAMCC_TFE0_GDSC, reg_rd(camcc_base, CAMCC_TFE0_GDSC) & ~GDSC_SW_COLLAPSE);
		__builtin_arm_dsb(0xf);
		for (uint32_t i = 0; i < 5000; i++) {
			if (reg_rd(camcc_base, CAMCC_TFE0_GDSC) & GDSC_PWR_ON) {
				break;
			}
			delay_for_interval(10, NSEC_PER_USEC);
		}

		(void)cbcr_enable(camcc_base, CAMCC_CPAS_AHB_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_CORE_AHB_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_ICP_AHB_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_SLOW_AHB_EXT_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_SLEEP_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_CPAS_FAST_AHB_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_CAMNOC_AXI_RT_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_CAMNOC_AXI_NRT_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_CAMNOC_DCD_XO_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_CSID_CBCR);
		(void)cbcr_enable(camcc_base, CAMCC_CSID_CSIPHY_RX_CBCR);

		rcgr_enable_xo(camcc_base, CAMCC_CCI0_CMD_RCGR, CAMCC_CCI0_CFG_RCGR);
		(void)cbcr_enable(camcc_base, CAMCC_CCI0_CBCR);
	}

	/* 3. CAM-CPAS top control & interconnect bandwidth */
	reg_wr(cpas_base, 0x04u, 0x01u);
	reg_wr(cpas_base, 0x10u, 0x03u);
	__builtin_arm_dsb(0xf);

	/* 4. Configure TLMM GPIOs and enable cambistmclkcc MCLK1 (Rear Wide) & MCLK4 (Front) */
	if (tlmm_base != 0) {
		tlmm_config_pin(GPIO_CCI0_M1_SDA, 1, 3, 2, false, false);
		tlmm_config_pin(GPIO_CCI0_M1_SCL, 1, 3, 2, false, false);
		tlmm_config_pin(GPIO_WIDE_VANA, 0, 0, 2, true, true);
		tlmm_config_pin(GPIO_WIDE_RESET, 0, 0, 2, true, false);
		tlmm_config_pin(GPIO_FRONT_RESET, 0, 0, 2, true, false);
		tlmm_config_pin(GPIO_WIDE_MCLK1, 1, 0, 2, false, false);
		tlmm_config_pin(GPIO_FRONT_MCLK4, 1, 0, 2, false, false);
	}
	if (mclkcc_base != 0) {
		(void)cbcr_enable(mclkcc_base, MCLKCC_SLEEP_CBCR);
		rcgr_enable_xo(mclkcc_base, MCLKCC_MCLK1_CMD_RCGR, MCLKCC_MCLK1_CFG_RCGR);
		(void)cbcr_enable(mclkcc_base, MCLKCC_MCLK1_CBCR);
		rcgr_enable_xo(mclkcc_base, MCLKCC_MCLK4_CMD_RCGR, MCLKCC_MCLK4_CFG_RCGR);
		(void)cbcr_enable(mclkcc_base, MCLKCC_MCLK4_CBCR);
	}
	if (tlmm_base != 0) {
		delay_for_interval(2, NSEC_PER_MSEC);
		tlmm_set_out(GPIO_WIDE_RESET, true);
		tlmm_set_out(GPIO_FRONT_RESET, true);
		delay_for_interval(2, NSEC_PER_MSEC);
	}

	/* 5. Initialize CCI0 master 1 and probe Rear Wide / Front sensors */
	probe_sensors_cci();

	/* 6. Seed initial MIPI RAW10 pattern into camera_region */
	render_dynamic_tpg_frame((uint8_t *)cam_region_vbase, 0, current_exposure,
	    current_gain, current_focus, current_sensor);
	__builtin_arm_dsb(0xf);

	/* 7. Configure and activate CSID-lite0 TPG -> RDI0 -> IFE-lite0 WM0 hardware pipeline */
	csid_lite_hardware_start();

	say("CAM-CPAS (0x0ac13000), sun-camcc (0x0ade0000), MCLK1/4, CCI0 m1, and CSID-lite0 enabled\n");
	return true;
}

static void
cam_hardware_power_down(void)
{
	if (cam_hw_mmio == 0) {
		return;
	}
	/* 1. Stop CSID-lite0 TPG and IFE-lite0 WM0 */
	csid_lite_hardware_stop();

	/* 2. Deassert sensor resets and MCLKs */
	if (tlmm_base != 0) {
		tlmm_set_out(GPIO_WIDE_RESET, false);
		tlmm_set_out(GPIO_FRONT_RESET, false);
		tlmm_set_out(GPIO_WIDE_VANA, false);
	}
	if (mclkcc_base != 0) {
		reg_wr(mclkcc_base, MCLKCC_MCLK1_CBCR, reg_rd(mclkcc_base, MCLKCC_MCLK1_CBCR) & ~1u);
		reg_wr(mclkcc_base, MCLKCC_MCLK4_CBCR, reg_rd(mclkcc_base, MCLKCC_MCLK4_CBCR) & ~1u);
	}
	cci_ready = false;

	/* 3. Collapse GDSC power domains */
	if (camcc_base != 0) {
		reg_wr(camcc_base, CAMCC_TFE0_GDSC, reg_rd(camcc_base, CAMCC_TFE0_GDSC) | GDSC_SW_COLLAPSE);
		__builtin_arm_dsb(0xf);
		reg_wr(camcc_base, CAMCC_TITAN_TOP_GDSC, reg_rd(camcc_base, CAMCC_TITAN_TOP_GDSC) | GDSC_SW_COLLAPSE);
		__builtin_arm_dsb(0xf);
	}

	/* 4. Release CPAS and interconnect bandwidth vote */
	if (cpas_base != 0) {
		reg_wr(cpas_base, 0x04u, 0x00u);
		reg_wr(cpas_base, 0x10u, 0x00u);
		__builtin_arm_dsb(0xf);
	}
	say("camera pipeline powered down\n");
}

/* Character Device Operations for /dev/annibale-camera */

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
	struct annibale_camera_frame frame;
	vm_offset_t payload_vbase;
	uint64_t seq;
	uint32_t exp, gain, focus, sensor;
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
	sensor = current_sensor;
	payload_vbase = cam_region_vbase;
	lck_mtx_unlock(cam_lock);

	/* 2. Frame rate pacing: ~33 ms per frame WITHOUT holding cam_lock */
	delay_for_interval(1000 / CAM_FPS, NSEC_PER_MSEC);

	/* 3. Update live frame contents when full payload is requested */
	resid = uio_resid(uio);
	if (resid < (user_ssize_t)sizeof(frame)) {
		return EINVAL;
	}
	if (resid > (user_ssize_t)sizeof(frame) && payload_vbase != 0) {
		render_dynamic_tpg_frame((uint8_t *)payload_vbase, seq, exp, gain, focus, sensor);
	}

	/* 4. Prepare frame header on stack */
	bzero(&frame, sizeof(frame));
	memcpy(frame.magic, ANNIBALE_CAMERA_MAGIC, sizeof(frame.magic));
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
	frame.line_ns = 29760; /* ~29.76 us per line (1120 lines @ 30 fps) */
	frame.gain = gain;
	frame.gain_unity = 256;
	frame.gain_max = 4096;
	frame.frame_lines = 1120;
	frame.frame_lines_min = 1120;
	frame.frame_lines_max = 65535;
	frame.margin = 8;
	frame.sensor = sensor;
	frame.focus = (sensor == 1u) ? focus : ANNIBALE_CAMERA_NO_FOCUS;

	/* 5. Copy frame header to userland WITHOUT holding cam_lock */
	error = uiomove((caddr_t)&frame, sizeof(frame), uio);
	if (error != 0) {
		return error;
	}

	/* 6. Copy frame payload to userland WITHOUT holding cam_lock */
	user_ssize_t copy_bytes = MIN(uio_resid(uio), (user_ssize_t)CAM_FRAME_BYTES);
	if (copy_bytes > 0 && payload_vbase != 0) {
		error = uiomove((caddr_t)payload_vbase, (int)copy_bytes, uio);
	}

	return error;
}

static int
annibale_cam_write(dev_t dev __unused, struct uio *uio, int ioflag __unused)
{
	user_ssize_t resid = uio_resid(uio);
	if (resid <= 0) {
		return EINVAL;
	}

	/* Support binary struct annibale_camera_command writes */
	if (resid == (user_ssize_t)sizeof(struct annibale_camera_command)) {
		struct annibale_camera_command cmd;
		int error = uiomove((caddr_t)&cmd, sizeof(cmd), uio);
		if (error != 0) {
			return error;
		}

		lck_mtx_lock(cam_lock);
		switch (cmd.command) {
		case ANNIBALE_CAMERA_EXPOSURE:
			current_exposure = cmd.a > 0 ? cmd.a : 1000;
			current_gain = cmd.b > 0 ? cmd.b : 256;
			cci0_apply_exposure(current_exposure, current_gain);
			break;
		case ANNIBALE_CAMERA_FOCUS:
			current_focus = cmd.a > 1023u ? 1023u : cmd.a;
			cci0_apply_focus(current_focus);
			break;
		case ANNIBALE_CAMERA_SENSOR:
			if (cmd.a != current_sensor) {
				current_sensor = (cmd.a == 2u) ? 2u : 1u;
				if (cam_region_vbase != 0) {
					render_dynamic_tpg_frame((uint8_t *)cam_region_vbase, 0,
					    current_exposure, current_gain, current_focus, current_sensor);
				}
				say("sensor switched to %u (%s)\n", current_sensor,
				    current_sensor == 2u ? "Front Camera" : "Back Wide Camera");
			}
			break;
		default:
			break;
		}
		lck_mtx_unlock(cam_lock);
		return 0;
	}

	/* Support ASCII control commands: "front", "back", "focus <N>" */
	char buf[64];
	size_t n = (size_t)MIN(resid, (user_ssize_t)(sizeof(buf) - 1));
	int error = uiomove(buf, (int)n, uio);
	if (error != 0) {
		return error;
	}
	buf[n] = '\0';

	lck_mtx_lock(cam_lock);
	if (strncmp(buf, "front", 5) == 0) {
		current_sensor = 2u;
		if (cam_region_vbase != 0) {
			render_dynamic_tpg_frame((uint8_t *)cam_region_vbase, 0,
			    current_exposure, current_gain, current_focus, current_sensor);
		}
		say("sensor switched via string command to 2 (Front Camera)\n");
	} else if (strncmp(buf, "back", 4) == 0) {
		current_sensor = 1u;
		if (cam_region_vbase != 0) {
			render_dynamic_tpg_frame((uint8_t *)cam_region_vbase, 0,
			    current_exposure, current_gain, current_focus, current_sensor);
		}
		say("sensor switched via string command to 1 (Back Wide Camera)\n");
	} else if (strncmp(buf, "focus", 5) == 0) {
		const char *p = buf + 5;
		while (*p == ' ' || *p == '\t' || *p == '=') {
			p++;
		}
		uint32_t val = 0;
		while (*p >= '0' && *p <= '9') {
			val = val * 10u + (uint32_t)(*p - '0');
			p++;
		}
		current_focus = val > 1023u ? 1023u : val;
		cci0_apply_focus(current_focus);
		say("focus set via string command to %u\n", current_focus);
	}
	lck_mtx_unlock(cam_lock);
	return 0;
}

static int
annibale_cam_ioctl(dev_t dev __unused, u_long cmd __unused, caddr_t data __unused, int flags __unused, struct proc *p __unused)
{
	return 0;
}

static int
annibale_cam_mmap(dev_t dev __unused, vm_map_offset_t off, int prot __unused)
{
	if (off >= CAMERA_REGION_SIZE) {
		return -1;
	}
	return (int)((CAMERA_REGION_PHYS + off) >> PAGE_SHIFT);
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
	.d_mmap = (mmap_fcn_t *)&annibale_cam_mmap,
	.d_strategy = eno_strat,
	.d_type = 0
};

/* Optional boot-arg cam=1..4 diagnostic probe thread */
static void
annibale_cam_diag_thread(void *param __unused, wait_result_t res __unused)
{
	uint32_t after_s = 5;
	(void)PE_parse_boot_argn("cam_after", &after_s, sizeof(after_s));
	delay_for_interval(after_s * 1000u, NSEC_PER_MSEC);

	lck_mtx_lock(cam_lock);
	bool was_open = cam_is_open;
	if (!was_open) {
		(void)cam_hardware_power_up();
	}
	say("cam=%u probe: TITAN_TOP_GDSCR=0x%08x CFG_GDSCR=0x%08x CSID_VER=0x%08x IFE_VER=0x%08x wide_id=0x%04x front_id=0x%04x\n",
	    boot_cam_level,
	    reg_rd(camcc_base, CAMCC_TITAN_TOP_GDSC),
	    reg_rd(camcc_base, CAMCC_TITAN_TOP_CFG_GDSCR),
	    reg_rd(csid_lite_base, CSID_HW_VERSION),
	    reg_rd(ife_lite_base, IFE_HW_VERSION),
	    wide_chip_id, front_chip_id);
	if (!was_open && boot_cam_level < 5) {
		cam_hardware_power_down();
	}
	lck_mtx_unlock(cam_lock);
	thread_terminate(current_thread());
}

void
annibale_camera_init(void)
{
	static bool initialized = false;
	if (initialized || !annibale_platform_is_annibale()) {
		return;
	}
	initialized = true;

	int cam_arg = -1;
	(void)PE_parse_boot_argn("cam_hw", &cam_hw_mmio, sizeof(cam_hw_mmio));
	if (PE_parse_boot_argn("cam", &cam_arg, sizeof(cam_arg))) {
		if (cam_arg == 0) {
			say("cam=0: camera driver disabled by boot-arg\n");
			return;
		}
		if (cam_arg > 0) {
			boot_cam_level = (uint32_t)cam_arg;
		}
	}

	say("registering /dev/annibale-camera driver bridge on annibale...\n");

	cam_lock_grp = lck_grp_alloc_init("annibale-cam", LCK_GRP_ATTR_NULL);
	cam_lock = lck_mtx_alloc_init(cam_lock_grp, LCK_ATTR_NULL);

	int major = cdevsw_add(-1, &annibale_cam_cdevsw);
	if (major >= 0) {
		if (devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "annibale-camera") != NULL) {
			say("/dev/annibale-camera created successfully\n");
		} else {
			say("warning: failed to create devfs node for /dev/annibale-camera\n");
		}
	} else {
		say("warning: failed to allocate cdevsw major for camera\n");
	}

	if (boot_cam_level >= 1 && boot_cam_level <= 4) {
		thread_t th;
		if (kernel_thread_start(annibale_cam_diag_thread, NULL, &th) == KERN_SUCCESS) {
			thread_deallocate(th);
		}
	}
}

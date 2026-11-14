/*
 * Hardware power, RPMh VRM, GCC/CAMCC/CPAS clocks, apps_smmu SID 0x1c00 S1 bypass,
 * CCI0 Master 1 / GPIO115-116 I2C (Rear Wide OV50E), QUPv3 SE9 I2C (Front OV20B),
 * and CSID-lite0 / IFE-lite0 version & DMA helpers for annibale_camera.c.
 */

/* Hardware base addresses on SM8750 ("sun" / annibale, verified against annibale.dts) */
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x000b0000UL
#define MCLKCC_PHYS             0x01760000UL
#define MCLKCC_SIZE             0x00006000UL
#define CAM_CPAS_PHYS           0x0ac04000UL    /* qcom,cam-cpas cam_cpas_top (<0xac04000 0x1000>) */
#define CAM_CPAS_SIZE           0x00001000UL
#define CCI0_PHYS               0x0ac7b000UL
#define CCI0_SIZE               0x00001000UL
#define CSID_LITE0_PHYS         0x0ad6d000UL    /* csid-lite980 (0xa00) + vfe-lite980 (0x2800) */
#define CSID_LITE0_SIZE         0x00002800UL
#define CSIPHY3_PHYS            0x0adaf000UL
#define CSIPHY3_SIZE            0x00002000UL
#define CSIPHY4_PHYS            0x0adb1000UL
#define CSIPHY4_SIZE            0x00002000UL
#define CAMCC_PHYS              0x0ade0000UL
#define CAMCC_SIZE              0x00020000UL
#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x00102000UL
#define APPS_SMMU_PHYS          0x15000000UL
#define APPS_SMMU_SIZE          0x00100000UL

#define CAMERA_REGION_SIZE      0x00800000UL    /* 8 MB kernel frame buffer */

/* GCC camera branch registers (base: 0x00100000, verified against gcc-sun.ko & annibale.dts) */
#define GCC_APSS_VOTE_1                 0x52010u
#define GCC_VOTE1_CAMERA_BITS           ((1u << 20) | (1u << 21))
#define GCC_CAMERA_AHB_CBCR             0x26004u
#define GCC_CAMERA_HF_AXI_CBCR          0x26014u
#define GCC_CAMERA_SF_AXI_CBCR          0x26024u
#define GCC_CAMERA_XO_CBCR              0x26034u
#define GCC_CAM_BIST_MCLK_AHB_CBCR      0xa0004u

/* cambistmclkcc registers (base: 0x01760000, verified against cambistmclkcc-sun.ko) */
#define MCLKCC_MCLK1_CMD_RCGR           0x401cu
#define MCLKCC_MCLK1_CFG_RCGR           0x4020u
#define MCLKCC_MCLK1_CBCR               0x4034u
#define MCLKCC_MCLK4_CMD_RCGR           0x4070u
#define MCLKCC_MCLK4_CFG_RCGR           0x4074u
#define MCLKCC_MCLK4_CBCR               0x4088u
#define MCLKCC_SLEEP_CMD_RCGR           0x40e0u
#define MCLKCC_SLEEP_CFG_RCGR           0x40e4u
#define MCLKCC_SLEEP_CBCR               0x40f8u

/* sun-camcc registers (base: 0x0ade0000, verified against camcc-sun.ko) */
#define CAMCC_FAST_AHB_CMD_RCGR         0x100dcu
#define CAMCC_FAST_AHB_CFG_RCGR         0x100e0u
#define CAMCC_SLOW_AHB_CMD_RCGR         0x10100u
#define CAMCC_SLOW_AHB_CFG_RCGR         0x10104u
#define CAMCC_IFE_LITE_CMD_RCGR         0x11150u
#define CAMCC_IFE_LITE_CFG_RCGR         0x11154u
#define CAMCC_IFE_LITE_CBCR             0x11168u
#define CAMCC_CAMNOC_RT_IFE_LITE_CBCR   0x11178u
#define CAMCC_IFE_LITE_CSID_CMD_RCGR    0x1117cu
#define CAMCC_IFE_LITE_CSID_CFG_RCGR    0x11180u
#define CAMCC_IFE_LITE_CSID_CBCR        0x11194u
#define CAMCC_IFE_LITE_CPHY_RX_CBCR     0x111a4u
#define CAMCC_IFE_LITE_AHB_CBCR         0x111a8u
#define CAMCC_ICP_AHB_CBCR              0x11264u
#define CAMCC_CCI0_CMD_RCGR             0x1126cu
#define CAMCC_CCI0_CFG_RCGR             0x11270u
#define CAMCC_CCI0_CBCR                 0x11284u
#define CAMCC_CAMNOC_AXI_RT_CMD_RCGR    0x112e8u
#define CAMCC_CAMNOC_AXI_RT_CFG_RCGR    0x112ecu
#define CAMCC_CAMNOC_AXI_RT_CBCR        0x11300u
#define CAMCC_CAMNOC_AXI_NRT_CBCR       0x11310u
#define CAMCC_CAMNOC_DCD_XO_CBCR        0x11320u
#define CAMCC_CAMNOC_XO_CBCR            0x11324u
#define CAMCC_QDSS_DEBUG_XO_CBCR        0x11348u
#define CAMCC_TITAN_TOP_GDSC            0x1134cu
#define CAMCC_TITAN_TOP_CFG_GDSCR       0x11350u
#define CAMCC_CORE_AHB_CBCR             0x11360u
#define CAMCC_XO_CMD_RCGR               0x11364u
#define CAMCC_XO_CFG_RCGR               0x11368u
#define CAMCC_XO_CBCR                   0x1137cu
#define CAMCC_SLEEP_CMD_RCGR            0x11380u
#define CAMCC_SLEEP_CFG_RCGR            0x11384u
#define CAMCC_SLEEP_CBCR                0x11398u
#define CAMCC_CPAS_FAST_AHB_CBCR        0x1139cu
#define CAMCC_CPAS_AHB_CBCR             0x113acu
#define CAMCC_DRV_XO_CBCR               0x113c0u
#define CAMCC_DRV_AHB_CBCR              0x113c4u
#define GDSC_SW_COLLAPSE                (1u << 0)
#define GDSC_PWR_ON                     (1u << 31)
#define CFG_GDSCR_PWR_ON                (1u << 16)

/* TLMM GPIO numbers on annibale (verified against pinctrl-sun.ko & annibale.dts) */
#define GPIO_SE9_I2C_SDA                4u      /* qupv3_se9_i2c: Front OV20B SDA */
#define GPIO_SE9_I2C_SCL                5u      /* qupv3_se9_i2c: Front OV20B SCL */
#define GPIO_WIDE_RESET                 6u      /* CAM_RESET1: Rear Wide OV50E */
#define GPIO_FRONT_RESET                7u      /* CAM_RESET4: Front OV20B */
#define GPIO_WIDE_MCLK1                 90u     /* cam_mclk (func 1): Rear Wide OV50E */
#define GPIO_FRONT_MCLK4                93u     /* cam_asc_mclk4 (func 1): Front OV20B */
#define GPIO_CCI0_M1_SDA                115u    /* cci_i2c_sda1 (func 1) / GPIO I2C (func 0) */
#define GPIO_CCI0_M1_SCL                116u    /* cci_i2c_scl1 (func 1) / GPIO I2C (func 0) */
#define GPIO_WIDE_VANA                  127u    /* camera_wide_avdd_ldo */

/* CCI0 v2 registers (base: 0x0ac7b000) */
#define CCI_HW_VERSION                  0x000u
#define CCI_RESET_CMD                   0x004u
#define CCI_QUEUE_START                 0x008u
#define CCI_I2C_M1_SCL_CTL              0x200u
#define CCI_I2C_M1_SDA_CTL_0            0x204u
#define CCI_I2C_M1_SDA_CTL_1            0x208u
#define CCI_I2C_M1_SDA_CTL_2            0x20cu
#define CCI_I2C_M1_MISC_CTL             0x210u
#define CCI_I2C_M1_READ_DATA            0x218u
#define CCI_I2C_M1_READ_BUF_LEVEL       0x21cu
#define CCI_I2C_M1_Q0_EXEC_WORD_CNT     0x500u
#define CCI_I2C_M1_Q0_CUR_WORD_CNT      0x504u
#define CCI_I2C_M1_Q0_CTRL              0x508u
#define CCI_I2C_M1_Q0_LOAD_DATA         0x510u
#define CCI_IRQ_MASK_0                  0xc04u
#define CCI_IRQ_CLEAR_0                 0xc08u
#define CCI_IRQ_STATUS_0                0xc0cu
#define CCI_IRQ_GLOBAL_CLEAR_CMD        0xc00u
#define CCI_IRQ_M1_RD_DONE                (1u << 12)
#define CCI_IRQ_M1_Q0_REPORT              (1u << 16)
#define CCI_IRQ_M1_ERROR                  0x60ee6000u

/* CSID-lite0 / VFE-lite0 register offsets within [0x0ad6d000, 0x0ad6f800) */
#define CSID_HW_VERSION                 0x000u
#define CSID_RST_STROBES                0x010u
#define CSID_CSI2_RX_IRQ_MASK           0x024u
#define CSID_CSI2_RX_IRQ_CLEAR          0x028u
#define CSID_RDI0_IRQ_MASK              0x034u
#define CSID_RDI0_IRQ_CLEAR             0x038u
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
#define VFE_LITE_SUB_HW_VERSION         0x1000u

#define RAW10_DT                        0x2bu

static vm_offset_t gcc_base;
static vm_offset_t mclkcc_base;
static vm_offset_t cpas_base;
static vm_offset_t camcc_base;
static vm_offset_t cci0_base;
static vm_offset_t csid_lite_base;
static vm_offset_t tlmm_base;
static vm_offset_t cam_region_vbase;

static uint16_t wide_chip_id = 0;
static uint16_t front_chip_id = 0;
static uint8_t wide_slave_addr = 0x36u;
static uint8_t front_slave_addr = 0x10u;
static uint8_t wide_bus_mode = 0;       /* 0 = none, 1 = CCI0 M1, 2 = GPIO 115/116 OD I2C */
static uint32_t cached_cpas_ver = 0;
static uint32_t cached_cpas_cap = 0;
static uint32_t cached_csid_ver = 0;
static uint32_t cached_ife_ver = 0;
static bool cci_ready = false;
static bool cam_ife_lite_clks_on = false;
static bool cam_hw_dma_verified = false;
static bool cam_sensor_streaming = false;

struct cam_vrm_rail {
	const char *name;
	uint32_t mv;
	uint32_t mode;
};

static const struct cam_vrm_rail cam_vrm_rails[] = {
	{ "bobb2",  3296u, ANNIBALE_RPMH_MODE_AUTO }, /* pm_humu_bob2: rear camera BOB parent */
	{ "ldom1",  1200u, ANNIBALE_RPMH_MODE_HPM  }, /* pm8010m_l1: front OV20B DVDD (1.056..1.200 V) */
	{ "ldoi1",  1152u, ANNIBALE_RPMH_MODE_HPM  }, /* pm_v8i_l1: rear wide OV50E DVDD (1.152 V) */
	{ "ldom5",  1800u, ANNIBALE_RPMH_MODE_NONE }, /* pm8010m_l5: front OV20B DOVDD (1.8 V, no mode reg) */
	{ "ldob10", 1800u, ANNIBALE_RPMH_MODE_HPM  }, /* pm_humu_l10: rear wide OV50E DOVDD (1.8 V) */
	{ "ldom3",  2800u, ANNIBALE_RPMH_MODE_NONE }, /* pm8010m_l3: front OV20B AVDD (2.8 V, no mode reg) */
	{ "ldob8",  3104u, ANNIBALE_RPMH_MODE_HPM  }, /* pm_humu_l8: rear wide VCM AF (3.104 V) */
};

static inline uint32_t
reg_rd(vm_offset_t base, uint32_t offset)
{
	if (base == 0) {
		return 0;
	}
	uint32_t v = *(volatile const uint32_t *)(base + offset);
	__builtin_arm_dsb(0xf);
	return v;
}

static inline void
reg_wr(vm_offset_t base, uint32_t offset, uint32_t val)
{
	if (base != 0) {
		__builtin_arm_dsb(0xf);
		*(volatile uint32_t *)(base + offset) = val;
		__builtin_arm_dsb(0xf);
	}
}

static inline void
csid_wr(uint32_t offset, uint32_t val)
{
	reg_wr(csid_lite_base, offset, val);
}

static inline void
reg_wr64(vm_offset_t base, uint32_t offset, uint64_t val)
{
	if (base != 0) {
		__builtin_arm_dsb(0xf);
		*(volatile uint64_t *)(base + offset) = val;
		__builtin_arm_dsb(0xf);
	}
}

#include "annibale_camera_smmu.inc"

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
	uint32_t val = (cur & ~0x3ffu) | (pull & 0x3u) | ((func & 0xfu) << 2) |
	    ((drv & 0x7u) << 6) | (out_en ? (1u << 9) : 0u);

	if (cur & (1u << 11)) {
		val |= (1u << 12);
	}
	reg_wr(tlmm_base, cfg_off, val);
	if (out_en && func == 0) {
		reg_wr(tlmm_base, io_off, out_high ? 0x2u : 0x0u);
	}
}

static void
tlmm_set_out(uint32_t pin, bool high)
{
	if (tlmm_base == 0 || pin >= 220) {
		return;
	}
	reg_wr(tlmm_base, pin * 0x1000u + 0x04u, high ? 0x2u : 0x0u);
}

static bool
tlmm_get_in(uint32_t pin)
{
	if (tlmm_base == 0 || pin >= 220) {
		return false;
	}
	return (reg_rd(tlmm_base, pin * 0x1000u + 0x04u) & 1u) != 0;
}

static bool
cbcr_enable(vm_offset_t base, uint32_t offset, bool check_off)
{
	if (base == 0) {
		return false;
	}
	reg_wr(base, offset, reg_rd(base, offset) | 1u);
	if (!check_off) {
		delay_for_interval(10, NSEC_PER_USEC);
		uint32_t v = reg_rd(base, offset);
		if (v != UINT32_MAX && (v & 1u) != 0) {
			return true;
		}
		say("CBCR +%#x enable readback failed (%#x)\n", offset, v);
		return false;
	}
	for (uint32_t i = 0; i < 500; i++) {
		uint32_t v = reg_rd(base, offset);
		if ((v & (1u << 31)) == 0 || ((v >> 28) & 7u) == 2u) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
}

static void
rcgr_enable_xo(vm_offset_t base, uint32_t cmd_off, uint32_t cfg_off)
{
	if (base == 0) {
		return;
	}
	reg_wr(base, cfg_off, 0x01u); /* src=0 (bi_tcxo 19.2 MHz), div=1 */
	reg_wr(base, cmd_off, reg_rd(base, cmd_off) | 0x03u); /* UPDATE | ROOT_EN */
	for (uint32_t i = 0; i < 250; i++) {
		if ((reg_rd(base, cmd_off) & 1u) == 0) {
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
}

static void
cam_vote_rpmh_supplies(bool enable)
{
	if (enable) {
		bool mmcx_ok = annibale_rpmh_arc_vote("mmcx.lvl", ANNIBALE_RPMPD_LEVEL_NOM);
		bool mxc_ok = annibale_rpmh_arc_vote("mxc.lvl", ANNIBALE_RPMPD_LEVEL_NOM);
		say("rpmh arc mmcx_return=%u mxc_return=%u\n", mmcx_ok ? 1u : 0u, mxc_ok ? 1u : 0u);
	}
	for (size_t i = 0; i < sizeof(cam_vrm_rails) / sizeof(cam_vrm_rails[0]); i++) {
		size_t idx = enable ? i : (sizeof(cam_vrm_rails) / sizeof(cam_vrm_rails[0]) - 1u - i);
		bool regulator_ok = annibale_rpmh_regulator(cam_vrm_rails[idx].name,
		    cam_vrm_rails[idx].mv, enable,
		    enable ? cam_vrm_rails[idx].mode : ANNIBALE_RPMH_MODE_NONE);
		say("rpmh regulator=%s enable=%u returned=%u\n", cam_vrm_rails[idx].name,
		    enable ? 1u : 0u, regulator_ok ? 1u : 0u);
	}
}

/* CCI0 v2 Master 1 initialization (400 kHz Fast-Mode I2C on 19.2 MHz bi_tcxo) */
static bool
cci0_m1_init(void)
{
	if (cci0_base == 0) {
		return false;
	}
	uint32_t ver = reg_rd(cci0_base, CCI_HW_VERSION);
	if (ver == 0 || ver == 0xffffffffu) {
		return false;
	}
	reg_wr(cci0_base, CCI_IRQ_MASK_0, 0);
	reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);
	reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);
	reg_wr(cci0_base, CCI_RESET_CMD, 0x0f0f0f17u);
	delay_for_interval(1, NSEC_PER_MSEC);
	reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);
	reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);

	reg_wr(cci0_base, CCI_I2C_M1_SCL_CTL, (20u << 16) | 28u);
	reg_wr(cci0_base, CCI_I2C_M1_SDA_CTL_0, (25u << 16) | 29u);
	reg_wr(cci0_base, CCI_I2C_M1_SDA_CTL_1, (10u << 16) | 18u);
	reg_wr(cci0_base, CCI_I2C_M1_SDA_CTL_2, 14u);
	reg_wr(cci0_base, CCI_I2C_M1_MISC_CTL, 0x01u);
	cci_ready = true;
	return true;
}

static bool
cci0_m1_read16(uint8_t slave7, uint16_t reg, uint16_t *out_val)
{
	if (!cci_ready || cci0_base == 0) {
		return false;
	}
	reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);
	reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);

	uint32_t stale = reg_rd(cci0_base, CCI_I2C_M1_READ_BUF_LEVEL);
	if (stale > 64u) {
		cci_ready = false;
		return false;
	}
	for (uint32_t i = 0; i < stale; i++) {
		(void)reg_rd(cci0_base, CCI_I2C_M1_READ_DATA);
	}
	if (reg_rd(cci0_base, CCI_I2C_M1_READ_BUF_LEVEL) != 0) {
		cci_ready = false;
		return false;
	}

	uint32_t set_param = 0x01u | ((uint32_t)(slave7 & 0x7fu) << 4) | (1u << 16);
	uint32_t wr_addr = 0x09u | (2u << 4) | (((uint32_t)(reg >> 8) & 0xffu) << 8) |
	    (((uint32_t)reg & 0xffu) << 16);
	uint32_t rd_cmd = 0x0au | (2u << 4);
	uint32_t rep_cmd = 0x108u;

	reg_wr(cci0_base, CCI_I2C_M1_Q0_EXEC_WORD_CNT, 4u);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, set_param);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, wr_addr);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, rd_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, rep_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_CTRL, 0u);
	reg_wr(cci0_base, CCI_QUEUE_START, (1u << 2)); /* M1_Q0 start bit */

	uint32_t irq = 0, level = 0, raw = 0;
	bool ok = false;
	for (uint32_t i = 0; i < 250; i++) {
		irq = reg_rd(cci0_base, CCI_IRQ_STATUS_0);
		if (irq & CCI_IRQ_M1_ERROR) {
			break;
		}
		/* The final REPORT must complete before the next probe uses this queue. */
		if ((irq & (CCI_IRQ_M1_RD_DONE | CCI_IRQ_M1_Q0_REPORT)) ==
		    (CCI_IRQ_M1_RD_DONE | CCI_IRQ_M1_Q0_REPORT)) {
			level = reg_rd(cci0_base, CCI_I2C_M1_READ_BUF_LEVEL);
			if (level == 1u) {
				raw = reg_rd(cci0_base, CCI_I2C_M1_READ_DATA);
				/* The FIFO holds SID, high data byte, then low data byte. */
				*out_val = (uint16_t)((raw & 0xff00u) | ((raw >> 16) & 0xffu));
				ok = true;
			}
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	if (!ok) {
		/* Recovery requires a completed master reset before another transfer. */
		cci_ready = false;
	}
	return ok;
}

static bool
cci0_m1_write_bytes(uint8_t slave7, uint8_t b0, uint8_t b1, uint8_t b2, uint32_t len)
{
	if (!cci_ready || cci0_base == 0 || len < 2 || len > 3) {
		return false;
	}
	reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);
	reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);

	uint32_t set_param = 0x01u | ((uint32_t)(slave7 & 0x7fu) << 4) | (1u << 16);
	uint32_t wr_cmd = 0x09u | (len << 4) | ((uint32_t)b0 << 8) | ((uint32_t)b1 << 16) |
	    ((uint32_t)b2 << 24);
	uint32_t rep_cmd = 0x108u;

	reg_wr(cci0_base, CCI_I2C_M1_Q0_EXEC_WORD_CNT, 3u);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, set_param);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, wr_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, rep_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_CTRL, 0u);
	reg_wr(cci0_base, CCI_QUEUE_START, (1u << 2));

	for (uint32_t i = 0; i < 250; i++) {
		uint32_t irq = reg_rd(cci0_base, CCI_IRQ_STATUS_0);
		if (irq == UINT32_MAX || (irq & CCI_IRQ_M1_ERROR) != 0) {
			break;
		}
		if ((irq & CCI_IRQ_M1_Q0_REPORT) != 0) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	/* A pending or failed transfer requires a completed master reset. */
	cci_ready = false;
	return false;
}

/* Parameterized open-drain I2C helper for QUPv3 SE9 (GPIO4/5) & CCI0 M1 pins (GPIO115/116) */
static void
od_i2c_sda(uint32_t sda_pin, bool high)
{
	if (high) {
		tlmm_config_pin(sda_pin, 0, 3, 2, false, false);
	} else {
		tlmm_config_pin(sda_pin, 0, 3, 2, true, false);
	}
}

static bool
od_i2c_scl_high(uint32_t scl_pin)
{
	tlmm_config_pin(scl_pin, 0, 3, 2, false, false);
	for (uint32_t i = 0; i < 200; i++) {
		if (tlmm_get_in(scl_pin)) {
			delay_for_interval(4, NSEC_PER_USEC);
			return true;
		}
		delay_for_interval(1, NSEC_PER_USEC);
	}
	return tlmm_get_in(scl_pin);
}

static void
od_i2c_scl_low(uint32_t scl_pin)
{
	tlmm_config_pin(scl_pin, 0, 3, 2, true, false);
	delay_for_interval(4, NSEC_PER_USEC);
}

static bool
od_i2c_start(uint32_t sda_pin, uint32_t scl_pin)
{
	od_i2c_sda(sda_pin, true);
	if (!od_i2c_scl_high(scl_pin) || !tlmm_get_in(sda_pin)) {
		return false;
	}
	od_i2c_sda(sda_pin, false);
	delay_for_interval(4, NSEC_PER_USEC);
	od_i2c_scl_low(scl_pin);
	return true;
}

static void
od_i2c_stop(uint32_t sda_pin, uint32_t scl_pin)
{
	od_i2c_sda(sda_pin, false);
	delay_for_interval(4, NSEC_PER_USEC);
	(void)od_i2c_scl_high(scl_pin);
	od_i2c_sda(sda_pin, true);
	delay_for_interval(4, NSEC_PER_USEC);
}

static bool
od_i2c_tx_byte(uint32_t sda_pin, uint32_t scl_pin, uint8_t b)
{
	for (int bit = 7; bit >= 0; bit--) {
		od_i2c_sda(sda_pin, ((b >> bit) & 1) != 0);
		delay_for_interval(3, NSEC_PER_USEC);
		if (!od_i2c_scl_high(scl_pin)) {
			od_i2c_scl_low(scl_pin);
			return false;
		}
		od_i2c_scl_low(scl_pin);
	}
	od_i2c_sda(sda_pin, true);
	delay_for_interval(3, NSEC_PER_USEC);
	if (!od_i2c_scl_high(scl_pin)) {
		od_i2c_scl_low(scl_pin);
		return false;
	}
	bool ack = !tlmm_get_in(sda_pin);
	od_i2c_scl_low(scl_pin);
	return ack;
}

static bool
od_i2c_rx_byte(uint32_t sda_pin, uint32_t scl_pin, bool send_ack, uint8_t *out_val)
{
	uint8_t val = 0;
	od_i2c_sda(sda_pin, true);
	for (int bit = 7; bit >= 0; bit--) {
		delay_for_interval(3, NSEC_PER_USEC);
		if (!od_i2c_scl_high(scl_pin)) {
			od_i2c_scl_low(scl_pin);
			od_i2c_sda(sda_pin, true);
			return false;
		}
		if (tlmm_get_in(sda_pin)) {
			val |= (uint8_t)(1u << bit);
		}
		od_i2c_scl_low(scl_pin);
	}
	od_i2c_sda(sda_pin, !send_ack);
	delay_for_interval(3, NSEC_PER_USEC);
	bool ack_clock_ok = od_i2c_scl_high(scl_pin);
	od_i2c_scl_low(scl_pin);
	od_i2c_sda(sda_pin, true);
	if (!ack_clock_ok) {
		return false;
	}
	*out_val = val;
	return true;
}

static bool
od_i2c_read16_reg16(uint32_t sda_pin, uint32_t scl_pin, uint8_t slave7, uint16_t reg, uint16_t *out_val)
{
	if (!od_i2c_start(sda_pin, scl_pin)) {
		return false;
	}
	if (!od_i2c_tx_byte(sda_pin, scl_pin, (uint8_t)((slave7 << 1) | 0u)) ||
	    !od_i2c_tx_byte(sda_pin, scl_pin, (uint8_t)(reg >> 8)) ||
	    !od_i2c_tx_byte(sda_pin, scl_pin, (uint8_t)(reg & 0xffu)) ||
	    !od_i2c_start(sda_pin, scl_pin) ||
	    !od_i2c_tx_byte(sda_pin, scl_pin, (uint8_t)((slave7 << 1) | 1u))) {
		od_i2c_stop(sda_pin, scl_pin);
		return false;
	}
	uint8_t hi = 0, lo = 0;
	bool ok = od_i2c_rx_byte(sda_pin, scl_pin, true, &hi) &&
	    od_i2c_rx_byte(sda_pin, scl_pin, false, &lo);
	od_i2c_stop(sda_pin, scl_pin);
	if (ok) {
		*out_val = (uint16_t)(((uint16_t)hi << 8) | lo);
	}
	return ok;
}

static bool
od_i2c_write_bytes(uint32_t sda_pin, uint32_t scl_pin, uint8_t slave7, uint8_t b0, uint8_t b1,
    uint8_t b2, uint32_t len)
{
	if (len < 2 || len > 3 || !od_i2c_start(sda_pin, scl_pin)) {
		return false;
	}
	bool ok = od_i2c_tx_byte(sda_pin, scl_pin, (uint8_t)((slave7 << 1) | 0u)) &&
	    od_i2c_tx_byte(sda_pin, scl_pin, b0) &&
	    od_i2c_tx_byte(sda_pin, scl_pin, b1) &&
	    (len < 3 || od_i2c_tx_byte(sda_pin, scl_pin, b2));
	od_i2c_stop(sda_pin, scl_pin);
	return ok;
}

static bool
wide_bus_write(uint8_t slave7, uint8_t b0, uint8_t b1, uint8_t b2, uint32_t len)
{
	if (wide_bus_mode == 1u) {
		return cci0_m1_write_bytes(slave7, b0, b1, b2, len);
	}
	if (wide_bus_mode == 2u) {
		return od_i2c_write_bytes(GPIO_CCI0_M1_SDA, GPIO_CCI0_M1_SCL, slave7, b0, b1, b2, len);
	}
	return false;
}

static void
cci0_apply_focus(uint32_t focus_dac)
{
	uint32_t dac = focus_dac > 1023u ? 1023u : focus_dac;
	(void)wide_bus_write(0x0cu, 0x03u, (uint8_t)((dac >> 8) & 0x03u), (uint8_t)(dac & 0xffu), 3);
}

static void
cci0_apply_exposure(uint32_t exp_lines, uint32_t gain)
{
	if (wide_chip_id == 0) {
		return;
	}
	(void)wide_bus_write(wide_slave_addr, 0x35u, 0x01u, (uint8_t)((exp_lines >> 8) & 0xffu), 3);
	(void)wide_bus_write(wide_slave_addr, 0x35u, 0x02u, (uint8_t)(exp_lines & 0xffu), 3);
	(void)wide_bus_write(wide_slave_addr, 0x35u, 0x08u, (uint8_t)((gain >> 8) & 0x0fu), 3);
	(void)wide_bus_write(wide_slave_addr, 0x35u, 0x09u, (uint8_t)(gain & 0xffu), 3);
}

/* Native modules use 16-bit register/data widths and 0x20 write addresses. */
#define SENSOR_SLAVE7                 0x10u
#define WIDE_SENSOR_ID_REG            0x300au
#define WIDE_SENSOR_ID_EXPECTED       0x5650u
#define WIDE_SENSOR_ID_MASK           0x0000ffffu
#define FRONT_SENSOR_ID_REG           0x300bu
#define FRONT_SENSOR_ID_EXPECTED      0x2042u
#define FRONT_SENSOR_ID_MASK          0xffffffffu

static bool
sensor_id_matches(uint16_t raw, uint16_t expected, uint32_t mask)
{
	/* Native cam_sensor_match_id applies the low 16 mask bits, then removes trailing zero bits. */
	uint16_t id_mask = (uint16_t)mask;
	if (id_mask == 0) {
		id_mask = 0xffffu;
	}
	raw &= id_mask;
	while ((id_mask & 1u) == 0) {
		raw >>= 1;
		id_mask >>= 1;
	}
	return raw == expected;
}

static void
probe_sensors_all(bool can_touch_cci0)
{
	uint16_t id = 0;

	wide_chip_id = 0;
	front_chip_id = 0;
	wide_bus_mode = 0;

	bool cci_initialized = can_touch_cci0 && cci0_m1_init();
	if (cci_initialized && cci0_m1_read16(SENSOR_SLAVE7, WIDE_SENSOR_ID_REG, &id) &&
	    sensor_id_matches(id, WIDE_SENSOR_ID_EXPECTED, WIDE_SENSOR_ID_MASK)) {
		wide_chip_id = id;
		wide_slave_addr = SENSOR_SLAVE7;
		wide_bus_mode = 1u;
		say("CCI0 M1 rear wide sensor matched at 0x%02x: chip_id=0x%04x\n", SENSOR_SLAVE7, id);
	}

	/* A failed CCI transfer requires reset before the same bus can use GPIO. */
	if (wide_chip_id == 0 && tlmm_base != 0 && (!cci_initialized || cci_ready)) {
		od_i2c_sda(GPIO_CCI0_M1_SDA, true);
		(void)od_i2c_scl_high(GPIO_CCI0_M1_SCL);
		delay_for_interval(50, NSEC_PER_USEC);
		if (od_i2c_read16_reg16(GPIO_CCI0_M1_SDA, GPIO_CCI0_M1_SCL,
		    SENSOR_SLAVE7, WIDE_SENSOR_ID_REG, &id) &&
		    sensor_id_matches(id, WIDE_SENSOR_ID_EXPECTED, WIDE_SENSOR_ID_MASK)) {
			wide_chip_id = id;
			wide_slave_addr = SENSOR_SLAVE7;
			wide_bus_mode = 2u;
			say("GPIO115/116 I2C rear wide sensor matched at 0x%02x: chip_id=0x%04x\n", SENSOR_SLAVE7, id);
		}
	}

	if (tlmm_base != 0) {
		od_i2c_sda(GPIO_SE9_I2C_SDA, true);
		(void)od_i2c_scl_high(GPIO_SE9_I2C_SCL);
		delay_for_interval(50, NSEC_PER_USEC);
		if (od_i2c_read16_reg16(GPIO_SE9_I2C_SDA, GPIO_SE9_I2C_SCL,
		    SENSOR_SLAVE7, FRONT_SENSOR_ID_REG, &id) &&
		    sensor_id_matches(id, FRONT_SENSOR_ID_EXPECTED, FRONT_SENSOR_ID_MASK)) {
			front_chip_id = id;
			front_slave_addr = SENSOR_SLAVE7;
			say("QUPv3 SE9 I2C front sensor matched at 0x%02x: chip_id=0x%04x\n", SENSOR_SLAVE7, id);
		}
	}
}

/*
 * Hardware CSID-lite0 / VFE-lite0 probe and optional Titan-170 TPG DMA bring-up.
 * On SM8750 ("sun"), csid-lite980 (0xad6d000, 0xa00) and vfe-lite980 (0xad6d000, 0x2800)
 * use Titan 980 register maps where standalone TPG lives at tpg13@ad8b000; Titan-170 TPG
 * strobes are only issued if major version == 1.
 */
static bool
csid_lite_hardware_start(void)
{
	if (csid_lite_base == 0 || !cam_ife_lite_clks_on) {
		say("csid_lite_base skipped (mapped=%u clks=%u)\n",
		    csid_lite_base != 0 ? 1u : 0u, cam_ife_lite_clks_on ? 1u : 0u);
		return false;
	}

	cached_csid_ver = reg_rd(csid_lite_base, CSID_HW_VERSION);
	cached_ife_ver = reg_rd(csid_lite_base, VFE_LITE_SUB_HW_VERSION);

	if (((cached_csid_ver >> 28) & 0xfu) == 1u) {
		csid_wr(CSID_CSI2_RX_IRQ_MASK, 0);
		csid_wr(CSID_RDI0_IRQ_MASK, 0);
		csid_wr(CSID_TOP_IRQ_CLEAR, 1);
		csid_wr(CSID_CSI2_RX_IRQ_CLEAR, 0xfffffff);
		csid_wr(CSID_RDI0_IRQ_CLEAR, 0x7fff);
		csid_wr(CSID_IRQ_CMD, 1);

		csid_wr(CSID_RST_STROBES, 0x1);
		delay_for_interval(1, NSEC_PER_MSEC);
		csid_wr(CSID_TOP_IRQ_CLEAR, 1);
		csid_wr(CSID_IRQ_CMD, 1);

		csid_wr(CSID_CSI2_RX_CFG0, 3u | (0x3210u << 4));
		csid_wr(CSID_CSI2_RX_CFG1, (1u << 6) | 1u);

		uint32_t vc = 0;
		csid_wr(CSID_TPG_VC_CFG0, (0u << 16) | (1u << 10) | vc);
		csid_wr(CSID_TPG_VC_CFG1, (0x28u << 12) | 0x100u);
		csid_wr(CSID_TPG_LFSR_SEED, 0x12345678);
		csid_wr(CSID_TPG_DT0_CFG0, (CAM_WIDTH << 16) | CAM_HEIGHT);
		csid_wr(CSID_TPG_DT0_CFG1, RAW10_DT);
		csid_wr(CSID_TPG_DT0_CFG2, ((3u - 1) << 16) | 0x8u);
		csid_wr(CSID_TPG_COLOR_BARS_CFG, (1u << 5));
		csid_wr(CSID_TPG_COMMON_GEN_CFG, 0);

		uint32_t rdi_cfg0 = (vc << 22) | (RAW10_DT << 16) | (0xfu << 12) | (1u << 2) | 3u;
		csid_wr(CSID_RDI0_CFG0, rdi_cfg0 | (1u << 31));
		csid_wr(CSID_RDI0_CFG1, 2);
		csid_wr(CSID_RDI0_CTRL, 1);
		csid_wr(CSID_TPG_CTRL, (4u << 20) | (0x80u << 8) | (3u << 4) | 7u);
	}

	say("CSID-lite0 (ver 0x%08x) / VFE-lite0 (ver 0x%08x) online (hw_dma=%u)\n",
	    cached_csid_ver, cached_ife_ver, cam_hw_dma_verified ? 1u : 0u);
	return true;
}

static void
csid_lite_hardware_stop(void)
{
	if (csid_lite_base != 0 && cam_ife_lite_clks_on && ((cached_csid_ver >> 28) & 0xfu) == 1u) {
		csid_wr(CSID_TPG_CTRL, 0);
		csid_wr(CSID_RDI0_CTRL, 2);
		csid_wr(CSID_RDI0_CFG0, 0);
		csid_wr(CSID_CSI2_RX_CFG0, 0);
	}
	cam_ife_lite_clks_on = false;
}

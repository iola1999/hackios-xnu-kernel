/*
 * Hardware power, RPMh VRM, GCC/CAMCC/CPAS clocks, apps_smmu SID 0x1c00 bypass,
 * CCI0 Master 1 (Rear Wide OV50E), QUPv3 SE9 I2C (Front OV20B), and CSID-lite0 /
 * IFE-lite0 DMA helpers for annibale_camera.c.
 */

/* Hardware base addresses on SM8750 (annibale) */
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x000b0000UL
#define MCLKCC_PHYS             0x01760000UL
#define MCLKCC_SIZE             0x00010000UL
#define CAM_CPAS_PHYS           0x0ac13000UL
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
#define TLMM_SIZE               0x00150000UL
#define APPS_SMMU_PHYS          0x15000000UL
#define APPS_SMMU_SIZE          0x00100000UL

#define CAMERA_REGION_PHYS      0x9b300000UL
#define CAMERA_REGION_SIZE      0x00800000UL    /* 8 MB */

/* GCC camera branch registers (base: 0x00100000) */
#define GCC_APSS_VOTE_1                 0x52010u
#define GCC_VOTE1_CAMERA_AXI_BIT        (1u << 20)
#define GCC_CAMERA_AHB_CBCR             0x26004u
#define GCC_CAMERA_NRT_AHB_CBCR         0x26008u
#define GCC_CAMERA_RT_AHB_CBCR          0x2600cu
#define GCC_CAMERA_CMD_AHB_CBCR         0x26010u
#define GCC_CAMERA_HF_AXI_CBCR          0x26014u
#define GCC_CAMERA_SF_AXI_CBCR          0x26024u
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

/* apps_smmu registers (base: 0x15000000) */
#define CAM_SMMU_IDR0                   0x020u
#define CAM_SMMU_IDR1                   0x024u
#define CAM_SMMU_SMR(n)                 (0x800u + 4u * (n))
#define CAM_SMMU_S2CR(n)                (0xc00u + 4u * (n))
#define CAM_SMMU_CBAR(n)                (0x1000u + 4u * (n))
#define CAM_SMMU_CBA2R(n)               (0x1800u + 4u * (n))
#define CAM_SMMU_SMR_VALID              (1u << 31)
#define CAM_SMMU_SID_IFE                0x1c00u
#define CAM_SMMU_SID_IFE_MASK           0x001fu

/* TLMM GPIO numbers on annibale (from merged DT) */
#define GPIO_SE9_I2C_SDA                4u      /* qupv3_se9_i2c: Front OV20B SDA */
#define GPIO_SE9_I2C_SCL                5u      /* qupv3_se9_i2c: Front OV20B SCL */
#define GPIO_WIDE_RESET                 6u      /* CAM_RESET1: Rear Wide OV50E */
#define GPIO_FRONT_RESET                7u      /* CAM_RESET4: Front OV20B */
#define GPIO_WIDE_MCLK1                 90u     /* CAMIF_MCLK1: Rear Wide OV50E */
#define GPIO_FRONT_MCLK4                93u     /* CAMIF_MCLK4: Front OV20B */
#define GPIO_CCI0_M1_SDA                115u    /* annibale_cci_i2c_sda1_active */
#define GPIO_CCI0_M1_SCL                116u    /* annibale_cci_i2c_scl1_active */
#define GPIO_WIDE_VANA                  127u    /* camera_wide_avdd_ldo */

/* CCI0 registers (base: 0x0ac7b000) */
#define CCI_RESET_CMD                   0x004u
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
static vm_offset_t cam_smmu_base;
static vm_offset_t cam_region_vbase;

static uint16_t wide_chip_id = 0;
static uint16_t front_chip_id = 0;
static uint8_t wide_slave_addr = 0x36u;
static uint8_t front_slave_addr = 0x10u;
static uint8_t cam_smmu_slot = 0xffu;
static uint32_t cached_csid_ver = 0;
static uint32_t cached_ife_ver = 0;
static bool cci_ready = false;
static bool cam_smmu_ready = false;
static bool cam_hw_dma_verified = false;
static bool cam_sensor_streaming = false;

struct cam_vrm_rail {
	const char *name;
	uint32_t mv;
};

static const struct cam_vrm_rail cam_vrm_rails[] = {
	{ "bobb2",  3296u }, /* pm_humu_bob2: rear camera BOB parent */
	{ "ldom1",  1104u }, /* pm8010m_l1: front OV20B DVDD */
	{ "ldoi1",  1152u }, /* pm_v8i_l1: rear wide OV50E DVDD */
	{ "ldom5",  1800u }, /* pm8010m_l5: front OV20B DOVDD */
	{ "ldob10", 1800u }, /* pm_humu_l10: rear wide OV50E DOVDD */
	{ "ldom3",  2800u }, /* pm8010m_l3: front OV20B AVDD */
	{ "ldob8",  2800u }, /* pm_humu_l8: rear wide VCM AF */
};

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
		val |= (1u << 12);
	}
	if (out_en && func == 0) {
		reg_wr(tlmm_base, io_off, out_high ? 0x2u : 0x0u);
	}
	reg_wr(tlmm_base, cfg_off, val);
	__builtin_arm_dsb(0xf);
}

static void
tlmm_set_out(uint32_t pin, bool high)
{
	if (tlmm_base == 0 || pin >= 220) {
		return;
	}
	reg_wr(tlmm_base, pin * 0x1000u + 0x04u, high ? 0x2u : 0x0u);
	__builtin_arm_dsb(0xf);
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
cbcr_enable(vm_offset_t base, uint32_t offset)
{
	if (base == 0) {
		return false;
	}
	reg_wr(base, offset, reg_rd(base, offset) | 1u);
	__builtin_arm_dsb(0xf);
	for (uint32_t i = 0; i < 250; i++) {
		if ((reg_rd(base, offset) & (1u << 31)) == 0) {
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
	reg_wr(base, cfg_off, 0x01u); /* src=0 (bi_tcxo), div=1 */
	reg_wr(base, cmd_off, reg_rd(base, cmd_off) | 0x03u); /* UPDATE | ROOT_EN */
	__builtin_arm_dsb(0xf);
	for (uint32_t i = 0; i < 250; i++) {
		if ((reg_rd(base, cmd_off) & 1u) == 0) {
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
}

/*
 * Idempotent apps_smmu (0x15000000) Stage-1 bypass for cam_hf_qtb (SID 0x1c00).
 * Shares the dedicated non-translating context bank (banks - 1) used by audio and PCIe,
 * reusing any pre-existing SMR entry for 0x1c00 across repeated open/close cycles.
 */
static bool
cam_smmu_configure_bypass(void)
{
	if (cam_smmu_base == 0) {
		cam_smmu_base = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
	}
	if (cam_smmu_base == 0) {
		say("failed to map apps_smmu (0x15000000)\n");
		return false;
	}

	uint32_t idr0 = reg_rd(cam_smmu_base, CAM_SMMU_IDR0);
	uint32_t idr1 = reg_rd(cam_smmu_base, CAM_SMMU_IDR1);
	uint32_t streams = MIN(idr0 & 0xffu, 128u);
	uint32_t banks = idr1 & 0xffu;
	uint32_t pages = 1u << (((idr1 >> 28) & 7u) + 1u);

	if ((idr1 >> 31) != 0 || banks < 2 || pages > 128 || streams == 0) {
		say("unexpected apps_smmu layout IDR0=0x%08x IDR1=0x%08x\n", idr0, idr1);
		return false;
	}

	uint32_t bank = banks - 1u;
	uint32_t cb_off = (pages + bank) * 0x1000u;
	uint32_t free_slot = UINT32_MAX;

	for (uint32_t i = 0; i < streams; i++) {
		uint32_t smr = reg_rd(cam_smmu_base, CAM_SMMU_SMR(i));
		if ((smr & CAM_SMMU_SMR_VALID) == 0) {
			if (free_slot == UINT32_MAX) {
				free_slot = i;
			}
			continue;
		}
		uint32_t mask = (smr >> 16) & 0x7fffu;
		if (((smr ^ CAM_SMMU_SID_IFE) & 0x7fffu & ~mask) == 0) {
			cam_smmu_slot = (uint8_t)i;
			cam_smmu_ready = true;
			return true;
		}
	}

	if (free_slot == UINT32_MAX || (reg_rd(cam_smmu_base, cb_off) & 1u) != 0) {
		say("no free apps_smmu SMR slot or bypass bank %u busy\n", bank);
		return false;
	}

	if (reg_rd(cam_smmu_base, CAM_SMMU_CBAR(bank)) != 0x10000u ||
	    reg_rd(cam_smmu_base, CAM_SMMU_CBA2R(bank)) != 1u) {
		reg_wr(cam_smmu_base, cb_off, 0);
		reg_wr(cam_smmu_base, CAM_SMMU_CBA2R(bank), 1u);
		reg_wr(cam_smmu_base, CAM_SMMU_CBAR(bank), 0x10000u);
	}
	reg_wr(cam_smmu_base, CAM_SMMU_S2CR(free_slot), bank);
	reg_wr(cam_smmu_base, CAM_SMMU_SMR(free_slot),
	    CAM_SMMU_SMR_VALID | (CAM_SMMU_SID_IFE_MASK << 16) | CAM_SMMU_SID_IFE);
	__builtin_arm_dsb(0xf);

	cam_smmu_slot = (uint8_t)free_slot;
	cam_smmu_ready = true;
	say("apps_smmu SID 0x%04x bypass configured at SMR[%u] -> CB[%u]\n",
	    CAM_SMMU_SID_IFE, free_slot, bank);
	return true;
}

static void
cam_vote_rpmh_supplies(bool enable)
{
	if (enable) {
		(void)annibale_rpmh_arc_vote("mmcx.lvl", ANNIBALE_RPMPD_LEVEL_SVS);
		(void)annibale_rpmh_arc_vote("mxc.lvl", ANNIBALE_RPMPD_LEVEL_SVS);
	}
	for (size_t i = 0; i < sizeof(cam_vrm_rails) / sizeof(cam_vrm_rails[0]); i++) {
		size_t idx = enable ? i : (sizeof(cam_vrm_rails) / sizeof(cam_vrm_rails[0]) - 1u - i);
		(void)annibale_rpmh_regulator(cam_vrm_rails[idx].name,
		    cam_vrm_rails[idx].mv, enable, ANNIBALE_RPMH_MODE_HPM);
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

	reg_wr(cci0_base, CCI_I2C_M1_SCL_CTL, (20u << 16) | 28u);
	reg_wr(cci0_base, CCI_I2C_M1_SDA_CTL_0, (25u << 16) | 29u);
	reg_wr(cci0_base, CCI_I2C_M1_SDA_CTL_1, (10u << 16) | 18u);
	reg_wr(cci0_base, CCI_I2C_M1_SDA_CTL_2, 14u);
	reg_wr(cci0_base, CCI_I2C_M1_MISC_CTL, 0x01u);
	__builtin_arm_dsb(0xf);
	cci_ready = true;
	return true;
}

static bool
cci0_m1_read8(uint8_t slave7, uint16_t reg, uint8_t *out_val)
{
	if (!cci_ready || cci0_base == 0) {
		return false;
	}
	reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);
	reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);

	uint32_t stale = reg_rd(cci0_base, CCI_I2C_M1_READ_BUF_LEVEL) & 0xffu;
	for (uint32_t i = 0; i < stale && i < 64; i++) {
		(void)reg_rd(cci0_base, CCI_I2C_M1_READ_DATA);
	}

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

	for (uint32_t i = 0; i < 250; i++) {
		if ((reg_rd(cci0_base, CCI_I2C_M1_READ_BUF_LEVEL) & 0xffu) > 0) {
			*out_val = (uint8_t)(reg_rd(cci0_base, CCI_I2C_M1_READ_DATA) & 0xffu);
			return true;
		}
		if (reg_rd(cci0_base, CCI_I2C_M1_Q0_CUR_WORD_CNT) == 0 && i > 15) {
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
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
	uint32_t wr_cmd = 0x09u | (len << 4) | ((uint32_t)b0 << 8) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 24);
	uint32_t rep_cmd = 0x108u;

	reg_wr(cci0_base, CCI_I2C_M1_Q0_EXEC_WORD_CNT, 3u);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, set_param);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, wr_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_LOAD_DATA, rep_cmd);
	reg_wr(cci0_base, CCI_I2C_M1_Q0_CTRL, 1u);
	__builtin_arm_dsb(0xf);

	for (uint32_t i = 0; i < 250; i++) {
		if (reg_rd(cci0_base, CCI_I2C_M1_Q0_CUR_WORD_CNT) == 0) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
}

/* Open-drain I2C helper for QUPv3_2 SE9 (i2c@884000, GPIO4 SDA / GPIO5 SCL) */
static void
se9_od_sda(bool high)
{
	if (high) {
		tlmm_config_pin(GPIO_SE9_I2C_SDA, 0, 3, 1, false, false);
	} else {
		tlmm_config_pin(GPIO_SE9_I2C_SDA, 0, 3, 1, true, false);
	}
}

static bool
se9_od_scl_high(void)
{
	tlmm_config_pin(GPIO_SE9_I2C_SCL, 0, 3, 1, false, false);
	for (uint32_t i = 0; i < 200; i++) {
		if (tlmm_get_in(GPIO_SE9_I2C_SCL)) {
			delay_for_interval(4, NSEC_PER_USEC);
			return true;
		}
		delay_for_interval(1, NSEC_PER_USEC);
	}
	return tlmm_get_in(GPIO_SE9_I2C_SCL);
}

static void
se9_od_scl_low(void)
{
	tlmm_config_pin(GPIO_SE9_I2C_SCL, 0, 3, 1, true, false);
	delay_for_interval(4, NSEC_PER_USEC);
}

static bool
se9_i2c_start(void)
{
	se9_od_sda(true);
	if (!se9_od_scl_high() || !tlmm_get_in(GPIO_SE9_I2C_SDA)) {
		return false;
	}
	se9_od_sda(false);
	delay_for_interval(4, NSEC_PER_USEC);
	se9_od_scl_low();
	return true;
}

static void
se9_i2c_stop(void)
{
	se9_od_sda(false);
	delay_for_interval(4, NSEC_PER_USEC);
	(void)se9_od_scl_high();
	se9_od_sda(true);
	delay_for_interval(4, NSEC_PER_USEC);
}

static bool
se9_i2c_tx_byte(uint8_t b)
{
	for (int bit = 7; bit >= 0; bit--) {
		se9_od_sda(((b >> bit) & 1) != 0);
		delay_for_interval(3, NSEC_PER_USEC);
		if (!se9_od_scl_high()) {
			se9_od_scl_low();
			return false;
		}
		se9_od_scl_low();
	}
	se9_od_sda(true);
	delay_for_interval(3, NSEC_PER_USEC);
	if (!se9_od_scl_high()) {
		se9_od_scl_low();
		return false;
	}
	bool ack = !tlmm_get_in(GPIO_SE9_I2C_SDA);
	se9_od_scl_low();
	return ack;
}

static uint8_t
se9_i2c_rx_byte(bool send_ack)
{
	uint8_t val = 0;
	se9_od_sda(true);
	for (int bit = 7; bit >= 0; bit--) {
		delay_for_interval(3, NSEC_PER_USEC);
		(void)se9_od_scl_high();
		if (tlmm_get_in(GPIO_SE9_I2C_SDA)) {
			val |= (uint8_t)(1u << bit);
		}
		se9_od_scl_low();
	}
	se9_od_sda(!send_ack);
	delay_for_interval(3, NSEC_PER_USEC);
	(void)se9_od_scl_high();
	se9_od_scl_low();
	se9_od_sda(true);
	return val;
}

static bool
se9_i2c_read16_reg8(uint8_t slave7, uint16_t reg, uint8_t *out_val)
{
	if (!se9_i2c_start()) {
		return false;
	}
	if (!se9_i2c_tx_byte((uint8_t)((slave7 << 1) | 0u)) ||
	    !se9_i2c_tx_byte((uint8_t)(reg >> 8)) ||
	    !se9_i2c_tx_byte((uint8_t)(reg & 0xffu)) ||
	    !se9_i2c_start() ||
	    !se9_i2c_tx_byte((uint8_t)((slave7 << 1) | 1u))) {
		se9_i2c_stop();
		return false;
	}
	*out_val = se9_i2c_rx_byte(false);
	se9_i2c_stop();
	return true;
}

static void
cci0_apply_focus(uint32_t focus_dac)
{
	uint32_t dac = focus_dac > 1023u ? 1023u : focus_dac;
	(void)cci0_m1_write_bytes(0x0cu, 0x03u, (uint8_t)((dac >> 8) & 0x03u), (uint8_t)(dac & 0xffu), 3);
}

static void
cci0_apply_exposure(uint32_t exp_lines, uint32_t gain)
{
	if (wide_chip_id == 0) {
		return;
	}
	(void)cci0_m1_write_bytes(wide_slave_addr, 0x35u, 0x01u, (uint8_t)((exp_lines >> 8) & 0xffu), 3);
	(void)cci0_m1_write_bytes(wide_slave_addr, 0x35u, 0x02u, (uint8_t)(exp_lines & 0xffu), 3);
	(void)cci0_m1_write_bytes(wide_slave_addr, 0x35u, 0x08u, (uint8_t)((gain >> 8) & 0x0fu), 3);
	(void)cci0_m1_write_bytes(wide_slave_addr, 0x35u, 0x09u, (uint8_t)(gain & 0xffu), 3);
}

static void
probe_sensors_all(void)
{
	static const uint8_t wide_candidates[] = { 0x36u, 0x10u, 0x34u, 0x20u };
	static const uint8_t front_candidates[] = { 0x10u, 0x36u, 0x30u, 0x21u };
	uint8_t hi = 0, lo = 0, sub = 0;

	if (cci0_m1_init()) {
		for (size_t i = 0; i < sizeof(wide_candidates); i++) {
			uint8_t slv = wide_candidates[i];
			if (cci0_m1_read8(slv, 0x300au, &hi) && cci0_m1_read8(slv, 0x300bu, &lo)) {
				uint16_t id = ((uint16_t)hi << 8) | lo;
				(void)cci0_m1_read8(slv, 0x300cu, &sub);
				if (id != 0x0000u && id != 0xffffu) {
					wide_chip_id = id;
					wide_slave_addr = slv;
					say("CCI0 M1 rear wide sensor answered at 0x%02x: chip_id=0x%04x rev=0x%02x\n",
					    slv, id, sub);
					break;
				}
			}
		}
	}

	/* Probe Front OV20B on QUPv3_2 SE9 I2C (0x00884000, GPIO4 SDA / GPIO5 SCL) */
	if (tlmm_base != 0) {
		se9_od_sda(true);
		(void)se9_od_scl_high();
		delay_for_interval(50, NSEC_PER_USEC);
		for (size_t i = 0; i < sizeof(front_candidates); i++) {
			uint8_t slv = front_candidates[i];
			if (se9_i2c_read16_reg8(slv, 0x300au, &hi) &&
			    se9_i2c_read16_reg8(slv, 0x300bu, &lo)) {
				uint16_t id = ((uint16_t)hi << 8) | lo;
				if (id != 0x0000u && id != 0xffffu) {
					front_chip_id = id;
					front_slave_addr = slv;
					say("QUPv3 SE9 I2C front sensor answered at 0x%02x: chip_id=0x%04x\n",
					    slv, id);
					break;
				}
			}
		}
	}

	/* Fallback probe on CCI0 M1 if front sensor was not found on SE9 */
	if (front_chip_id == 0 && cci_ready) {
		for (size_t i = 0; i < sizeof(front_candidates); i++) {
			uint8_t slv = front_candidates[i];
			if (wide_chip_id != 0 && slv == wide_slave_addr) {
				continue;
			}
			if (cci0_m1_read8(slv, 0x300au, &hi) && cci0_m1_read8(slv, 0x300bu, &lo)) {
				uint16_t id = ((uint16_t)hi << 8) | lo;
				if (id != 0x0000u && id != 0xffffu) {
					front_chip_id = id;
					front_slave_addr = slv;
					say("CCI0 M1 front sensor answered at 0x%02x: chip_id=0x%04x\n", slv, id);
					break;
				}
			}
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

	csid_wr(CSID_RST_STROBES, 0x1e);
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

	volatile uint64_t *dma_sentinel = (volatile uint64_t *)cam_region_vbase;
	uint64_t sentinel_before = 0x5aa5f00fc33c9669ULL;
	if (dma_sentinel != NULL) {
		*dma_sentinel = sentinel_before;
		__builtin_arm_dsb(0xf);
	}

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
		ife_wr(IFE_WM0_FRAME_INC, 0); /* Keep DMA bounded inside single frame slot */
		ife_wr(IFE_WM0_CFG, 3);
		ife_wr(IFE_REG_UPDATE, 0x2);
	}

	csid_wr(CSID_TPG_CTRL, (4u << 20) | (0x80u << 8) | (3u << 4) | 7u);
	__builtin_arm_dsb(0xf);

	cached_csid_ver = reg_rd(csid_lite_base, CSID_HW_VERSION);
	cached_ife_ver = reg_rd(ife_lite_base, IFE_HW_VERSION);

	if (dma_sentinel != NULL) {
		for (uint32_t i = 0; i < 40; i++) {
			delay_for_interval(1, NSEC_PER_MSEC);
			if (*dma_sentinel != sentinel_before) {
				cam_hw_dma_verified = true;
				break;
			}
		}
	}

	say("CSID-lite0 (ver 0x%08x) -> IFE-lite0 (ver 0x%08x) WM0 @ 0x%08lx enabled (hw_dma=%u)\n",
	    cached_csid_ver, cached_ife_ver, CAMERA_REGION_PHYS, cam_hw_dma_verified ? 1u : 0u);
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

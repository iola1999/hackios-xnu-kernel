/*
 * Native sensor-mode clocks and CRM SW DRV0 votes. Rates and register layouts
 * come from annibale's camcc-sun/clk-qcom/crm-v2 modules and device tree.
 * CRM-controlled RX and CAMNOC RCGs are changed through their native voters.
 */
#define CAM_CRM_PHYS             0x0adcb000UL
#define CAM_CRM_SIZE             0x00005000UL
#define CAM_CRM_C                0x2600u
#define CAM_CRM_COMMON           0x4d00u
#define CAM_CRM_RX_VCD           7u
#define CAM_CRM_RX_SW            (0x11cu + CAM_CRM_RX_VCD * 0x20u)
#define CAM_CRM_RT_SW            0x21cu
#define CAM_CRM_RT_IRQ           0x24cu
#define CAM_CRM_VCD_STRIDE       0x210u
#define CAM_CRM_LEVEL_STRIDE     0x18u
#define CAM_PLL_LOCK             (1u << 31)
#define CAM_PLL_VOTE_RUN         (1u << 25)
#define CAM_RCG_FIELDS           (0x1fu | (7u << 8) | (3u << 12) | (1u << 20))
#define CAMCC_PHY3_TIMER_CMD     0x10064u
#define CAMCC_PHY3_TIMER_CBCR    0x1007cu
#define CAMCC_PHY3_RX_CBCR       0x10080u
#define CAMCC_PHY4_TIMER_CMD     0x10084u
#define CAMCC_PHY4_TIMER_CBCR    0x1009cu
#define CAMCC_PHY4_RX_CBCR       0x100a0u

static vm_offset_t cam_crm_base;
static bool cam_native_clocks_owned, cam_native_pll_changed, cam_crm_failed;
static uint32_t cam_native_phy_supplies;
static uint32_t cam_crm_rx_saved[3], cam_crm_rt_saved[3];
static uint32_t cam_crm_rx_irq_saved, cam_crm_rt_irq_saved;
static uint32_t cam_native_rcg_changes, cam_native_branch_changes, cam_native_crm_changes;

struct cam_native_rcg {
	uint32_t command, config, saved_config, saved_command;
};

static struct cam_native_rcg cam_native_rcgs[] = {
	{ CAMCC_FAST_AHB_CMD_RCGR, (1u << 20) | (2u << 8) | 3u, 0, 0 },
	{ CAMCC_SLOW_AHB_CMD_RCGR, (1u << 20) | (2u << 8) | 14u, 0, 0 },
	{ CAMCC_IFE_LITE_CMD_RCGR, (1u << 20) | (1u << 8) | 8u, 0, 0 },
	{ CAMCC_IFE_LITE_CSID_CMD_RCGR, (1u << 20) | (1u << 8) | 8u, 0, 0 },
	{ CAMCC_PHY4_TIMER_CMD, (1u << 20) | (1u << 8) | 5u, 0, 0 },
};

static uint32_t cam_native_branches[] = {
	CAMCC_IFE_LITE_CBCR, CAMCC_IFE_LITE_CSID_CBCR,
	CAMCC_IFE_LITE_CPHY_RX_CBCR, CAMCC_PHY4_TIMER_CBCR,
	CAMCC_PHY4_RX_CBCR, CAMCC_CAMNOC_AXI_RT_CBCR,
	CAMCC_CAMNOC_RT_IFE_LITE_CBCR,
};
static uint32_t cam_native_branches_saved[sizeof(cam_native_branches) / sizeof(uint32_t)];

/* Lucid Evo has no TEST_CTL_U1 in this module's register-offset table. */
static const uint32_t cam_native_pll_offsets[] = {
	0x00, 0x04, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30,
};
static uint32_t cam_native_pll_saved[sizeof(cam_native_pll_offsets) / sizeof(uint32_t)];

static bool
cam_poll(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t expected, uint32_t us)
{
	for (uint32_t i = 0; i < us; i += 10u) {
		uint32_t value = reg_rd(base, offset);
		if (value != UINT32_MAX && (value & mask) == expected) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
}

static bool
cam_native_rcg_write(vm_offset_t base, uint32_t command, uint32_t config, uint32_t root)
{
	uint32_t value = reg_rd(base, command);
	if (value == UINT32_MAX || (value & 1u) != 0) {
		return false;
	}
	reg_wr(base, command + 4u, config);
	reg_wr(base, command, (value & ~3u) | (root & 2u) | 1u);
	return cam_poll(base, command, 1u, 0, 5000u) &&
	       reg_rd(base, command + 4u) == config;
}

static bool
cam_native_pll_enable(void)
{
	uint32_t mode = reg_rd(camcc_base, 0);
	uint32_t user = reg_rd(camcc_base, 0x18);
	bool rate_ok = (reg_rd(camcc_base, 0x10) & 0xffffu) == 0x3eu &&
	    (reg_rd(camcc_base, 0x14) & 0xffffu) == 0x8000u &&
	    ((user >> 10) & 0xfu) == 1u;
	if ((mode & CAM_PLL_LOCK) != 0 || (user & CAM_PLL_VOTE_RUN) != 0) {
		return rate_ok && (mode & CAM_PLL_LOCK) != 0 && (mode & 1u) != 0;
	}
	cam_native_pll_changed = true;
	reg_wr(camcc_base, 0, mode & ~1u);
	reg_wr(camcc_base, 4, 0);
	/* clk_lucid_evo_pll_configure stores both calibration L fields as 0x44. */
	reg_wr(camcc_base, 0x10, 0x4444003eu);
	reg_wr(camcc_base, 0x14, 0x8000u);
	reg_wr(camcc_base, 0x20, 0x19660387u);
	reg_wr(camcc_base, 0x24, 0x098060a0u);
	reg_wr(camcc_base, 0x28, 0xb416cb20u);
	reg_wr(camcc_base, 0x18, 0x8407u);
	reg_wr(camcc_base, 0x1c, 2u);
	reg_wr(camcc_base, 0x2c, 0);
	reg_wr(camcc_base, 0x30, 0);
	reg_wr(camcc_base, 0, (mode & ~1u) | 4u);
	reg_wr(camcc_base, 4, 1u);
	if (!cam_poll(camcc_base, 0, CAM_PLL_LOCK, CAM_PLL_LOCK, 5000u)) {
		return false;
	}
	reg_wr(camcc_base, 0, reg_rd(camcc_base, 0) | 1u);
	return true;
}

/* CRM LUT levels start at 1; level 0 parks the RCG. Native RX parent-map cfg 1 is PLL0 MAIN. */
static uint32_t
cam_native_rx_level(void)
{
	uint64_t previous = 0;
	for (uint32_t level = 1; level <= 3; level++) {
		uint32_t offset = CAM_CRM_C + CAM_CRM_RX_VCD * CAM_CRM_VCD_STRIDE +
		    level * CAM_CRM_LEVEL_STRIDE;
		uint32_t config = reg_rd(cam_crm_base, offset + 0x150u);
		uint32_t packed = reg_rd(cam_crm_base, offset + 0x154u);
		uint32_t source = (config >> 8) & 7u;
		if (config == UINT32_MAX || packed == UINT32_MAX || source != 1u ||
		    (config & (3u << 12)) != 0) {
			return 0;
		}
		uint32_t l = packed & 0xffu, alpha = packed >> 16;
		uint64_t parent = l != 0 ? 19200000ULL * l +
		    (19200000ULL * alpha >> 16) : 1200000000ULL;
		uint64_t rate = parent * 2u / ((config & 0x1fu) + 1u);
		if (rate <= previous) {
			return 0;
		}
		if (rate >= 266666666ULL) {
			return level;
		}
		previous = rate;
	}
	return 0;
}

static bool
cam_native_crm_vote(uint32_t offset, uint32_t irq, uint32_t active, uint32_t sleep, uint32_t wake)
{
	if (cam_crm_failed) {
		return false;
	}
	reg_wr(cam_crm_base, irq + 4u, 1u);
	if (!cam_poll(cam_crm_base, irq, 1u, 0, 5000u)) {
		return false;
	}
	reg_wr(cam_crm_base, irq + 8u, 1u);
	reg_wr(cam_crm_base, offset, active);
	if (!cam_poll(cam_crm_base, irq, 1u, 1u, 100000u)) {
		/* The native CRM driver forbids another command after an uncompleted active vote. */
		cam_crm_failed = true;
		return false;
	}
	reg_wr(cam_crm_base, irq + 4u, 1u);
	reg_wr(cam_crm_base, offset + 4u, sleep);
	reg_wr(cam_crm_base, offset + 8u, wake);
	return true;
}

static bool
cam_native_clocks_up(void)
{
	if (cam_native_clocks_owned) {
		return !cam_crm_failed;
	}
	if (cam_crm_base == 0) {
		cam_crm_base = ml_io_map(CAM_CRM_PHYS, CAM_CRM_SIZE);
	}
	if (cam_crm_base == 0 || reg_rd(cam_crm_base, CAM_CRM_COMMON + 0xcu) == 0 ||
	    reg_rd(cam_crm_base, CAM_CRM_COMMON + 0xcu) == UINT32_MAX) {
		return false;
	}
	uint32_t counts = reg_rd(cam_crm_base, CAM_CRM_COMMON + 4u);
	if (counts == UINT32_MAX || ((counts >> 8) & 0xfu) <= CAM_CRM_RX_VCD ||
	    ((counts >> 20) & 0xfu) == 0) {
		return false;
	}
	bool back = cam_profile->sensor == 1u;
	cam_native_rcgs[4].command = back ? CAMCC_PHY3_TIMER_CMD : CAMCC_PHY4_TIMER_CMD;
	cam_native_branches[3] = back ? CAMCC_PHY3_TIMER_CBCR : CAMCC_PHY4_TIMER_CBCR;
	cam_native_branches[4] = back ? CAMCC_PHY3_RX_CBCR : CAMCC_PHY4_RX_CBCR;
	for (size_t i = 0; i < sizeof(cam_native_pll_offsets) / sizeof(uint32_t); i++) {
		cam_native_pll_saved[i] = reg_rd(camcc_base, cam_native_pll_offsets[i]);
		if (cam_native_pll_saved[i] == UINT32_MAX) {
			return false;
		}
	}
	for (size_t i = 0; i < sizeof(cam_native_rcgs) / sizeof(cam_native_rcgs[0]); i++) {
		cam_native_rcgs[i].saved_config = reg_rd(camcc_base, cam_native_rcgs[i].command + 4u);
		cam_native_rcgs[i].saved_command = reg_rd(camcc_base, cam_native_rcgs[i].command);
		if (cam_native_rcgs[i].saved_config == UINT32_MAX ||
		    cam_native_rcgs[i].saved_command == UINT32_MAX ||
		    (cam_native_rcgs[i].saved_command & 1u) != 0) {
			return false;
		}
	}
	for (size_t i = 0; i < sizeof(cam_native_branches) / sizeof(uint32_t); i++) {
		cam_native_branches_saved[i] = reg_rd(camcc_base, cam_native_branches[i]);
		if (cam_native_branches_saved[i] == UINT32_MAX) {
			return false;
		}
	}
	for (uint32_t i = 0; i < 3; i++) {
		uint32_t rx = reg_rd(cam_crm_base, CAM_CRM_RX_SW + 4u * i);
		uint32_t rt = reg_rd(cam_crm_base, CAM_CRM_RT_SW + 4u * i);
		if (rx == UINT32_MAX || rt == UINT32_MAX) {
			return false;
		}
		cam_crm_rx_saved[i] = rx & 7u;
		cam_crm_rt_saved[i] = rt & ~(1u << 31);
		/* The native zero-rate BCM vote also carries the valid/commit bits. */
		if (cam_crm_rt_saved[i] == 0) {
			cam_crm_rt_saved[i] = 0x60000000u;
		}
	}
	cam_crm_rx_irq_saved = reg_rd(cam_crm_base, CAM_CRM_RX_SW + 0x18u);
	cam_crm_rt_irq_saved = reg_rd(cam_crm_base, CAM_CRM_RT_IRQ + 8u);
	if (cam_crm_rx_irq_saved == UINT32_MAX || cam_crm_rt_irq_saved == UINT32_MAX) {
		return false;
	}
	cam_native_clocks_owned = true;
	if (!cam_native_pll_enable()) {
		return false;
	}
	for (size_t i = 0; i < sizeof(cam_native_rcgs) / sizeof(cam_native_rcgs[0]); i++) {
		struct cam_native_rcg *rcg = &cam_native_rcgs[i];
		cam_native_rcg_changes = (uint32_t)i + 1u;
		if (!cam_native_rcg_write(camcc_base, rcg->command,
		    (rcg->saved_config & ~CAM_RCG_FIELDS) | rcg->config, 2u)) {
			return false;
		}
	}
	uint32_t level = cam_native_rx_level();
	if (level == 0) {
		return false;
	}
	cam_native_crm_changes |= 1u;
	if (!cam_native_crm_vote(CAM_CRM_RX_SW, CAM_CRM_RX_SW + 0x10u,
	    level, level, level)) {
		return false;
	}
	/* camcc-sun's first CAMNOC RT rate is 200 MHz; BCM payload uses MHz and both valid/commit bits. */
	uint32_t bandwidth = 0x60000000u | 200u;
	cam_native_crm_changes |= 2u;
	if (!cam_native_crm_vote(CAM_CRM_RT_SW, CAM_CRM_RT_IRQ,
	    bandwidth | (1u << 31), bandwidth, bandwidth)) {
		return false;
	}
	for (size_t i = 0; i < sizeof(cam_native_branches) / sizeof(uint32_t); i++) {
		cam_native_branch_changes = (uint32_t)i + 1u;
		if (!cbcr_enable(camcc_base, cam_native_branches[i], true)) {
			return false;
		}
	}
	cam_ife_lite_clks_on = true;
	return true;
}

static bool
cam_native_clocks_down(void)
{
	if (!cam_native_clocks_owned) {
		return true;
	}
	if (cam_crm_failed) {
		return false;
	}
	if ((cam_native_crm_changes & 2u) != 0) {
		if (!cam_native_crm_vote(CAM_CRM_RT_SW, CAM_CRM_RT_IRQ,
		    cam_crm_rt_saved[0] | (1u << 31), cam_crm_rt_saved[1], cam_crm_rt_saved[2])) {
			return false;
		}
		reg_wr(cam_crm_base, CAM_CRM_RT_IRQ + 8u, cam_crm_rt_irq_saved);
		cam_native_crm_changes &= ~2u;
	}
	if ((cam_native_crm_changes & 1u) != 0) {
		if (!cam_native_crm_vote(CAM_CRM_RX_SW, CAM_CRM_RX_SW + 0x10u,
		    cam_crm_rx_saved[0], cam_crm_rx_saved[1], cam_crm_rx_saved[2])) {
			return false;
		}
		reg_wr(cam_crm_base, CAM_CRM_RX_SW + 0x18u, cam_crm_rx_irq_saved);
		cam_native_crm_changes &= ~1u;
	}
	for (uint32_t i = 0; i < cam_native_branch_changes; i++) {
		reg_wr(camcc_base, cam_native_branches[i], reg_rd(camcc_base, cam_native_branches[i]) & ~1u);
	}
	while (cam_native_rcg_changes != 0) {
		struct cam_native_rcg *rcg = &cam_native_rcgs[cam_native_rcg_changes - 1u];
		if (!cam_native_rcg_write(camcc_base, rcg->command, rcg->saved_config, rcg->saved_command)) {
			return false;
		}
		cam_native_rcg_changes--;
	}
	if (cam_native_pll_changed) {
		reg_wr(camcc_base, 0, reg_rd(camcc_base, 0) & ~1u);
		reg_wr(camcc_base, 4, 0);
		for (size_t i = 2; i < sizeof(cam_native_pll_offsets) / sizeof(uint32_t); i++) {
			reg_wr(camcc_base, cam_native_pll_offsets[i], cam_native_pll_saved[i]);
		}
		reg_wr(camcc_base, 4, cam_native_pll_saved[1]);
		reg_wr(camcc_base, 0, cam_native_pll_saved[0]);
		cam_native_pll_changed = false;
	}
	for (uint32_t i = 0; i < cam_native_branch_changes; i++) {
		reg_wr(camcc_base, cam_native_branches[i], cam_native_branches_saved[i]);
	}
	cam_native_branch_changes = 0;
	cam_native_clocks_owned = false;
	return true;
}

static bool
cam_native_sensor_power(bool up)
{
	bool back = cam_profile->sensor == 1u;
	const struct annibale_camera_power *power = cam_profile_power(up);
	uint32_t count = up ? cam_profile->up_count : cam_profile->down_count;
	for (uint32_t i = 0; i < count; i++) {
		bool ok;
		switch (power[i].type) {
		case 0: {
			uint32_t command = back ? MCLKCC_MCLK1_CMD_RCGR : MCLKCC_MCLK4_CMD_RCGR;
			uint32_t config = command + 4u;
			uint32_t branch = back ? MCLKCC_MCLK1_CBCR : MCLKCC_MCLK4_CBCR;
			ok = power[i].value == 0 || power[i].value == 19200000u;
			if (ok && power[i].value != 0) {
				ok = cam_native_rcg_write(mclkcc_base, command,
				    (reg_rd(mclkcc_base, config) & ~CAM_RCG_FIELDS) | 1u, 2u) &&
				    cbcr_enable(mclkcc_base, branch, true);
			} else if (ok) {
				reg_wr(mclkcc_base, branch, reg_rd(mclkcc_base, branch) & ~1u);
			}
			break;
		}
		case 1:
			if (back) {
				tlmm_config_pin(GPIO_WIDE_VANA, 0, 0, 2, true, power[i].value != 0);
				ok = (reg_rd(tlmm_base, GPIO_WIDE_VANA * 0x1000u + 4u) & 2u) ==
				    (power[i].value != 0 ? 2u : 0);
			} else {
				ok = annibale_rpmh_regulator("ldom3", 2800u, power[i].value != 0, ANNIBALE_RPMH_MODE_NONE);
			}
			break;
		case 2:
			ok = power[i].value == 0 || power[i].value == (back ? 1u : 1200000u);
			ok = ok && annibale_rpmh_regulator(back ? "ldoi1" : "ldom1", back ? 1152u : 1200u,
			    power[i].value != 0,
			    power[i].value != 0 ? ANNIBALE_RPMH_MODE_HPM : ANNIBALE_RPMH_MODE_NONE);
			break;
		case 3:
			ok = annibale_rpmh_regulator(back ? "ldob10" : "ldom5", 1800u, power[i].value != 0,
			    back && power[i].value != 0 ? ANNIBALE_RPMH_MODE_HPM : ANNIBALE_RPMH_MODE_NONE);
			break;
		case 4:
			ok = back && annibale_rpmh_regulator("ldob8", 3104u, power[i].value != 0,
			    power[i].value != 0 ? ANNIBALE_RPMH_MODE_HPM : ANNIBALE_RPMH_MODE_NONE);
			break;
		case 14:
			ok = back && (power[i].value == 0 || power[i].value == 3296000u) &&
			    annibale_rpmh_regulator("bobb2", 3296u, power[i].value != 0,
			    power[i].value != 0 ? ANNIBALE_RPMH_MODE_AUTO : ANNIBALE_RPMH_MODE_NONE);
			break;
		case 8: {
			uint32_t pin = back ? GPIO_WIDE_RESET : GPIO_FRONT_RESET;
			tlmm_config_pin(pin, 0, 0, 2, true, power[i].value != 0);
			ok = (reg_rd(tlmm_base, pin * 0x1000u + 4u) & 2u) ==
			    (power[i].value != 0 ? 2u : 0);
			break;
		}
		default:
			return false;
		}
		if (!ok) {
			return false;
		}
		delay_for_interval(power[i].delay_ms, NSEC_PER_MSEC);
	}
	return true;
}

static bool
cam_native_register_table(uint32_t table, uint32_t count)
{
	if (table == 2u && cam_native_exposure_lines != 0 && cam_native_gain != 0 &&
	    (cam_native_is_ov20b_m4(cam_profile) || cam_native_is_ovx8000_m24(cam_profile))) {
		uint32_t fll = cam_native_frame_lines;
		uint32_t lines = cam_native_exposure_lines;
		uint32_t gain = cam_native_gain;
		const struct {
			uint16_t reg;
			uint8_t val;
		} exp_ops[7] = {
			{ 0x380eu, (uint8_t)((fll >> 8) & 0xffu) },
			{ 0x380fu, (uint8_t)(fll & 0xffu) },
			{ 0x3500u, (uint8_t)((lines >> 16) & 0xffu) },
			{ 0x3501u, (uint8_t)((lines >> 8) & 0xffu) },
			{ 0x3502u, (uint8_t)(lines & 0xffu) },
			{ 0x3508u, (uint8_t)((gain >> 8) & 0x7fu) },
			{ 0x3509u, (uint8_t)(gain & 0xffu) },
		};
		for (size_t k = 0; k < 7u; k++) {
			bool exp_ok = cam_profile->sensor == 1u ?
			    wide_bus_write(wide_slave_addr, (uint8_t)(exp_ops[k].reg >> 8),
			    (uint8_t)exp_ops[k].reg, exp_ops[k].val, 3u) :
			    od_i2c_write_bytes(GPIO_SE9_I2C_SDA, GPIO_SE9_I2C_SCL, front_slave_addr,
			    (uint8_t)(exp_ops[k].reg >> 8), (uint8_t)exp_ops[k].reg,
			    exp_ops[k].val, 3u);
			if (!exp_ok) {
				say("sensor %u exposure register %#x failed\n",
				    cam_profile->sensor, exp_ops[k].reg);
				return false;
			}
		}
	}
	const struct annibale_camera_register *entries = cam_profile_registers(table);
	for (uint32_t i = 0; i < count; i++) {
		bool ok = cam_profile->sensor == 1u ?
		    wide_bus_write(wide_slave_addr, (uint8_t)(entries[i].address >> 8),
		    (uint8_t)entries[i].address, (uint8_t)entries[i].value, 3u) :
		    od_i2c_write_bytes(GPIO_SE9_I2C_SDA, GPIO_SE9_I2C_SCL, front_slave_addr,
		    (uint8_t)(entries[i].address >> 8), (uint8_t)entries[i].address,
		    (uint8_t)entries[i].value, 3u);
		if (!ok) {
			say("sensor %u register %#x failed in table %u at %u\n",
			    cam_profile->sensor, entries[i].address, table, i);
			return false;
		}
		delay_for_interval(entries[i].delay_us, NSEC_PER_USEC);
	}
	return true;
}

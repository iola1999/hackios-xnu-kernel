/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * D-PHY v2.3.0 registers from cam_csiphy_2_3_0_hwreg.h, checked against
 * annibale's camera module. This private table serves the checked front mode 4.
 */
struct cam_phy_register {
	uint16_t offset;
	uint8_t value, parameter;
	uint16_t delay_us;
};

static vm_offset_t cam_phy4_base;
static bool cam_native_phy_configured;

static const struct cam_phy_register cam_phy4_dphy[] = {
	{ 0x0e94, 0x00, 1u, 0u },
	{ 0x0ea0, 0x00, 1u, 0u },
	{ 0x0e90, 0x0f, 1u, 0u },
	{ 0x0e98, 0x08, 1u, 0u },
	{ 0x0e94, 0x07, 1u, 209u },
	{ 0x0094, 0x00, 1u, 0u },
	{ 0x00a0, 0x00, 1u, 0u },
	{ 0x0090, 0x0f, 1u, 0u },
	{ 0x0098, 0x08, 1u, 0u },
	{ 0x0094, 0x07, 1u, 209u },
	{ 0x0494, 0x00, 1u, 0u },
	{ 0x04a0, 0x00, 1u, 0u },
	{ 0x0490, 0x0f, 1u, 0u },
	{ 0x0498, 0x08, 1u, 0u },
	{ 0x0494, 0x07, 1u, 209u },
	{ 0x0894, 0x00, 1u, 0u },
	{ 0x08a0, 0x00, 1u, 0u },
	{ 0x0890, 0x0f, 1u, 0u },
	{ 0x0898, 0x08, 1u, 0u },
	{ 0x0894, 0x07, 1u, 209u },
	{ 0x0c94, 0x00, 1u, 0u },
	{ 0x0ca0, 0x00, 1u, 0u },
	{ 0x0c90, 0x0f, 1u, 0u },
	{ 0x0c98, 0x08, 1u, 0u },
	{ 0x0c94, 0x07, 1u, 209u },
	{ 0x0e30, 0x00, 1u, 0u },
	{ 0x0e28, 0x04, 1u, 0u },
	{ 0x0e00, 0x80, 1u, 0u },
	{ 0x0e0c, 0xff, 1u, 0u },
	{ 0x0e38, 0x1f, 1u, 0u },
	{ 0x0e2c, 0x01, 1u, 0u },
	{ 0x0e34, 0x0f, 1u, 0u },
	{ 0x0e1c, 0x0a, 1u, 0u },
	{ 0x0e14, 0x60, 1u, 0u },
	{ 0x0e3c, 0xb8, 1u, 0u },
	{ 0x0e04, 0x0c, 1u, 0u },
	{ 0x0e20, 0x00, 1u, 0u },
	{ 0x0e08, 0x19, 4u, 0u },
	{ 0x0e10, 0x52, 1u, 0u },
	{ 0x0030, 0x00, 1u, 0u },
	{ 0x0000, 0x8c, 1u, 0u },
	{ 0x0038, 0xfe, 1u, 0u },
	{ 0x002c, 0x01, 1u, 0u },
	{ 0x0034, 0x0f, 1u, 0u },
	{ 0x001c, 0x0a, 1u, 0u },
	{ 0x0014, 0x60, 1u, 0u },
	{ 0x003c, 0xb8, 1u, 0u },
	{ 0x0004, 0x0c, 1u, 0u },
	{ 0x0020, 0x00, 1u, 0u },
	{ 0x0008, 0x19, 4u, 0u },
	{ 0x0010, 0x52, 1u, 0u },
	{ 0x0430, 0x00, 1u, 0u },
	{ 0x0400, 0x8c, 1u, 0u },
	{ 0x0438, 0xfe, 1u, 0u },
	{ 0x042c, 0x01, 1u, 0u },
	{ 0x0434, 0x0f, 1u, 0u },
	{ 0x041c, 0x0a, 1u, 0u },
	{ 0x0414, 0x60, 1u, 0u },
	{ 0x043c, 0xb8, 1u, 0u },
	{ 0x0404, 0x0c, 1u, 0u },
	{ 0x0420, 0x00, 1u, 0u },
	{ 0x0408, 0x19, 4u, 0u },
	{ 0x0410, 0x52, 1u, 0u },
	{ 0x0830, 0x00, 1u, 0u },
	{ 0x0800, 0x8c, 1u, 0u },
	{ 0x0838, 0xfe, 1u, 0u },
	{ 0x082c, 0x01, 1u, 0u },
	{ 0x0834, 0x0f, 1u, 0u },
	{ 0x081c, 0x0a, 1u, 0u },
	{ 0x0814, 0x60, 1u, 0u },
	{ 0x083c, 0xb8, 1u, 0u },
	{ 0x0804, 0x0c, 1u, 0u },
	{ 0x0820, 0x00, 1u, 0u },
	{ 0x0808, 0x19, 4u, 0u },
	{ 0x0810, 0x52, 1u, 0u },
	{ 0x0c30, 0x00, 1u, 0u },
	{ 0x0c00, 0x8c, 1u, 0u },
	{ 0x0c38, 0xfe, 1u, 0u },
	{ 0x0c2c, 0x01, 1u, 0u },
	{ 0x0c34, 0x0f, 1u, 0u },
	{ 0x0c1c, 0x0a, 1u, 0u },
	{ 0x0c14, 0x60, 1u, 0u },
	{ 0x0c3c, 0xb8, 1u, 0u },
	{ 0x0c04, 0x0c, 1u, 0u },
	{ 0x0c20, 0x00, 1u, 0u },
	{ 0x0c08, 0x19, 4u, 0u },
	{ 0x0c10, 0x52, 1u, 0u },
	{ 0x0094, 0xd7, 64u, 0u },
	{ 0x005c, 0x00, 64u, 0u },
	{ 0x0060, 0xfd, 64u, 0u },
	{ 0x0064, 0x7f, 64u, 0u },
	{ 0x0494, 0xd7, 64u, 0u },
	{ 0x045c, 0x00, 64u, 0u },
	{ 0x0460, 0xfd, 64u, 0u },
	{ 0x0464, 0x7f, 64u, 0u },
	{ 0x0894, 0xd7, 64u, 0u },
	{ 0x085c, 0x00, 64u, 0u },
	{ 0x0860, 0xfd, 64u, 0u },
	{ 0x0864, 0x7f, 64u, 0u },
	{ 0x0c94, 0xd7, 64u, 0u },
	{ 0x0c5c, 0x00, 64u, 0u },
	{ 0x0c60, 0xfd, 64u, 0u },
	{ 0x0c64, 0x7f, 64u, 0u },
};

static bool
cam_native_phy_up(void)
{
	if (cam_native_phy_configured) {
		return true;
	}
	if (cam_phy4_base == 0) {
		cam_phy4_base = ml_io_map(CSIPHY4_PHYS, CSIPHY4_SIZE);
	}
	if (cam_phy4_base == 0) {
		return false;
	}
	if (!annibale_rpmh_regulator("ldog3", 1200u, true, ANNIBALE_RPMH_MODE_HPM)) {
		return false;
	}
	cam_native_phy_supplies |= 1u;
	if (!annibale_rpmh_regulator("ldoi3", 880u, true, ANNIBALE_RPMH_MODE_HPM)) {
		return false;
	}
	cam_native_phy_supplies |= 2u;
	reg_wr(cam_phy4_base, 0x1014u, 0);
	reg_wr(cam_phy4_base, 0x1000u, 1u);
	delay_for_interval(1, NSEC_PER_USEC);
	if (!cam_poll(cam_phy4_base, 0x1184u, 1u << 7, 1u << 7, 10000u)) {
		return false;
	}
	/* Native START enables lanes after REFGEN, then programs common and lane tables. */
	reg_wr(cam_phy4_base, 0x1014u, 0xd5u);
	reg_wr(cam_phy4_base, 0x1084u, 0);
	reg_wr(cam_phy4_base, 0x108cu, 0x0eu);
	delay_for_interval(1, NSEC_PER_USEC);
	reg_wr(cam_phy4_base, 0x101cu, 0x7au);
	reg_wr(cam_phy4_base, 0x1018u, 1u);
	for (size_t i = 0; i < sizeof(cam_phy4_dphy) / sizeof(cam_phy4_dphy[0]); i++) {
		const struct cam_phy_register *entry = &cam_phy4_dphy[i];
		uint32_t value = entry->parameter == 4u ? cam_profile->settle_count :
		    (entry->parameter == 64u ? 0 : entry->value);
		reg_wr(cam_phy4_base, entry->offset, value);
		delay_for_interval(entry->delay_us, NSEC_PER_USEC);
	}
	reg_wr(cam_phy4_base, 0x102cu, 0xffu);
	reg_wr(cam_phy4_base, 0x1030u, 0xffu);
	reg_wr(cam_phy4_base, 0x1034u, 0xfbu);
	reg_wr(cam_phy4_base, 0x1038u, 0xffu);
	reg_wr(cam_phy4_base, 0x103cu, 0x7fu);
	reg_wr(cam_phy4_base, 0x1040u, 0xffu);
	reg_wr(cam_phy4_base, 0x1044u, 0xffu);
	reg_wr(cam_phy4_base, 0x1048u, 0xefu);
	reg_wr(cam_phy4_base, 0x104cu, 0xffu);
	reg_wr(cam_phy4_base, 0x1050u, 0xffu);
	reg_wr(cam_phy4_base, 0x1054u, 0xffu);
	delay_for_interval(100, NSEC_PER_USEC);
	reg_wr(cam_phy4_base, 0x1000u, 2u);
	cam_native_phy_configured = reg_rd(cam_phy4_base, 0x1014u) == 0xd5u &&
	    reg_rd(cam_phy4_base, 0x1000u) == 2u;
	return cam_native_phy_configured;
}

static bool
cam_native_phy_down(void)
{
	if (cam_native_phy_supplies != 0 && cam_phy4_base != 0) {
		reg_wr(cam_phy4_base, 0x1014u, 0);
		reg_wr(cam_phy4_base, 0x1000u, 1u);
		delay_for_interval(1, NSEC_PER_USEC);
	}
	if ((cam_native_phy_supplies & 2u) != 0) {
		if (!annibale_rpmh_regulator("ldoi3", 880u, false, ANNIBALE_RPMH_MODE_NONE)) {
			return false;
		}
		cam_native_phy_supplies &= ~2u;
	}
	if ((cam_native_phy_supplies & 1u) != 0) {
		if (!annibale_rpmh_regulator("ldog3", 1200u, false, ANNIBALE_RPMH_MODE_NONE)) {
			return false;
		}
		cam_native_phy_supplies &= ~1u;
	}
	cam_native_phy_configured = false;
	return true;
}

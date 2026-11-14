/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * D-PHY and C-PHY v2.3.0 registers checked against annibale's camera module.
 * The tables serve front mode 4 and rear-wide mode 24.
 */
struct cam_phy_register {
	uint16_t offset;
	uint8_t value, parameter;
	uint16_t delay_us;
};

static vm_offset_t cam_phy3_base, cam_phy4_base;
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

static const struct cam_phy_register cam_phy3_cphy[] = {
	{ 0x0294, 0x0d, 1u, 0u },
	{ 0x02f4, 0x00, 1u, 0u },
	{ 0x02f8, 0x00, 1u, 0u },
	{ 0x02fc, 0x00, 1u, 0u },
	{ 0x02f0, 0xef, 1u, 211u },
	{ 0x0694, 0x0d, 1u, 0u },
	{ 0x06f4, 0x00, 1u, 0u },
	{ 0x06f8, 0x00, 1u, 0u },
	{ 0x06fc, 0x00, 1u, 0u },
	{ 0x06f0, 0xef, 1u, 211u },
	{ 0x0a94, 0x0d, 1u, 0u },
	{ 0x0af4, 0x00, 1u, 0u },
	{ 0x0af8, 0x00, 1u, 0u },
	{ 0x0afc, 0x00, 1u, 0u },
	{ 0x0af0, 0xef, 1u, 211u },
	{ 0x0204, 0x00, 1u, 0u },
	{ 0x02e4, 0x00, 1u, 0u },
	{ 0x02e8, 0x7f, 1u, 0u },
	{ 0x02ec, 0x7f, 1u, 0u },
	{ 0x0218, 0x3e, 1u, 0u },
	{ 0x021c, 0x41, 1u, 0u },
	{ 0x0220, 0x41, 1u, 0u },
	{ 0x0224, 0x7f, 1u, 0u },
	{ 0x0228, 0x00, 1u, 0u },
	{ 0x022c, 0x00, 1u, 0u },
	{ 0x0264, 0x01, 1u, 0u },
	{ 0x0244, 0xb2, 1u, 0u },
	{ 0x0310, 0x35, 1u, 0u },
	{ 0x02bc, 0xd0, 1u, 0u },
	{ 0x0254, 0x00, 1u, 0u },
	{ 0x0240, 0x00, 1u, 0u },
	{ 0x0260, 0xa8, 1u, 0u },
	{ 0x0284, 0x00, 1u, 0u },
	{ 0x0290, 0x02, 1u, 0u },
	{ 0x0604, 0x00, 1u, 0u },
	{ 0x06e4, 0x00, 1u, 0u },
	{ 0x06e8, 0x7f, 1u, 0u },
	{ 0x06ec, 0x7f, 1u, 0u },
	{ 0x0618, 0x3e, 1u, 0u },
	{ 0x061c, 0x41, 1u, 0u },
	{ 0x0620, 0x41, 1u, 0u },
	{ 0x0624, 0x7f, 1u, 0u },
	{ 0x0628, 0x00, 1u, 0u },
	{ 0x062c, 0x00, 1u, 0u },
	{ 0x0664, 0x01, 1u, 0u },
	{ 0x0644, 0xb2, 1u, 0u },
	{ 0x0710, 0x35, 1u, 0u },
	{ 0x06bc, 0xd0, 1u, 0u },
	{ 0x0654, 0x00, 1u, 0u },
	{ 0x0640, 0x00, 1u, 0u },
	{ 0x0660, 0xa8, 1u, 0u },
	{ 0x0684, 0x00, 1u, 0u },
	{ 0x0690, 0x02, 1u, 0u },
	{ 0x0a04, 0x00, 1u, 0u },
	{ 0x0ae4, 0x00, 1u, 0u },
	{ 0x0ae8, 0x7f, 1u, 0u },
	{ 0x0aec, 0x7f, 1u, 0u },
	{ 0x0a18, 0x3e, 1u, 0u },
	{ 0x0a1c, 0x41, 1u, 0u },
	{ 0x0a20, 0x41, 1u, 0u },
	{ 0x0a24, 0x7f, 1u, 0u },
	{ 0x0a28, 0x00, 1u, 0u },
	{ 0x0a2c, 0x00, 1u, 0u },
	{ 0x0a64, 0x01, 1u, 0u },
	{ 0x0a44, 0xb2, 1u, 0u },
	{ 0x0b10, 0x35, 1u, 0u },
	{ 0x0abc, 0xd0, 1u, 0u },
	{ 0x0a54, 0x00, 1u, 0u },
	{ 0x0a40, 0x00, 1u, 0u },
	{ 0x0a60, 0xa8, 1u, 0u },
	{ 0x0a84, 0x00, 1u, 0u },
	{ 0x0a90, 0x02, 1u, 0u },
};

static const struct cam_phy_register cam_phy3_cphy_rate[] = {
	{ 0x0268, 0xf1, 1u, 0u },
	{ 0x026c, 0x1d, 1u, 0u },
	{ 0x0270, 0x00, 1u, 0u },
	{ 0x0274, 0x00, 1u, 0u },
	{ 0x0278, 0x12, 1u, 0u },
	{ 0x0288, 0x00, 1u, 0u },
	{ 0x028c, 0x75, 1u, 0u },
	{ 0x0290, 0x02, 1u, 0u },
	{ 0x0668, 0xf1, 1u, 0u },
	{ 0x066c, 0x1d, 1u, 0u },
	{ 0x0670, 0x00, 1u, 0u },
	{ 0x0674, 0x00, 1u, 0u },
	{ 0x0678, 0x12, 1u, 0u },
	{ 0x0688, 0x00, 1u, 0u },
	{ 0x068c, 0x75, 1u, 0u },
	{ 0x0690, 0x02, 1u, 0u },
	{ 0x0a68, 0xf1, 1u, 0u },
	{ 0x0a6c, 0x1d, 1u, 0u },
	{ 0x0a70, 0x00, 1u, 0u },
	{ 0x0a74, 0x00, 1u, 10u },
	{ 0x0a78, 0x12, 1u, 0u },
	{ 0x0a88, 0x00, 1u, 0u },
	{ 0x0a8c, 0x75, 1u, 0u },
	{ 0x0a90, 0x02, 1u, 0u },
	{ 0x020c, 0x21, 4u, 0u },
	{ 0x0208, 0x00, 8u, 0u },
	{ 0x0210, 0x00, 1u, 0u },
	{ 0x0214, 0x00, 1u, 0u },
	{ 0x060c, 0x21, 4u, 0u },
	{ 0x0608, 0x00, 8u, 0u },
	{ 0x0610, 0x00, 1u, 0u },
	{ 0x0614, 0x00, 1u, 0u },
	{ 0x0a0c, 0x21, 4u, 0u },
	{ 0x0a08, 0x00, 8u, 0u },
	{ 0x0a10, 0x00, 1u, 0u },
	{ 0x0a14, 0x00, 1u, 0u },
};

static void
cam_native_phy_registers(vm_offset_t base, const struct cam_phy_register *table, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const struct cam_phy_register *entry = &table[i];
		uint32_t value = entry->parameter == 4u ? cam_profile->settle_count & 0xffu :
		    (entry->parameter == 8u ? cam_profile->settle_count >> 8 :
		    (entry->parameter == 64u ? 0 : entry->value));
		reg_wr(base, entry->offset, value);
		delay_for_interval(entry->delay_us, NSEC_PER_USEC);
	}
}

static bool
cam_native_phy_up(void)
{
	if (cam_native_phy_configured) {
		return true;
	}
	bool cphy = cam_profile->sensor == 1u;
	vm_offset_t *mapping = cphy ? &cam_phy3_base : &cam_phy4_base;
	if (*mapping == 0) {
		*mapping = ml_io_map(cphy ? CSIPHY3_PHYS : CSIPHY4_PHYS,
		    cphy ? CSIPHY3_SIZE : CSIPHY4_SIZE);
	}
	vm_offset_t base = *mapping;
	if (base == 0) {
		return false;
	}
	if (!annibale_rpmh_regulator("ldog3", 1200u, true, ANNIBALE_RPMH_MODE_HPM)) {
		return false;
	}
	cam_native_phy_supplies |= 1u;
	/* PHY3 uses L1F; PHY4 uses L3I in the annibale device tree. */
	if (!annibale_rpmh_regulator(cphy ? "ldof1" : "ldoi3", 880u, true, ANNIBALE_RPMH_MODE_HPM)) {
		return false;
	}
	cam_native_phy_supplies |= 2u;
	reg_wr(base, 0x1014u, 0);
	reg_wr(base, 0x1000u, 1u);
	delay_for_interval(1, NSEC_PER_USEC);
	if (!cam_poll(base, 0x1184u, 1u << 7, 1u << 7, 10000u)) {
		return false;
	}
	/* Native START enables lanes after REFGEN, then programs common and lane tables. */
	reg_wr(base, 0x1014u, cphy ? 0x2au : 0xd5u);
	reg_wr(base, 0x1084u, 0);
	reg_wr(base, 0x108cu, 0x0eu);
	delay_for_interval(1, NSEC_PER_USEC);
	reg_wr(base, 0x101cu, 0x7au);
	reg_wr(base, 0x1018u, 1u);
	if (cphy) {
		cam_native_phy_registers(base, cam_phy3_cphy_rate,
		    sizeof(cam_phy3_cphy_rate) / sizeof(cam_phy3_cphy_rate[0]));
		cam_native_phy_registers(base, cam_phy3_cphy,
		    sizeof(cam_phy3_cphy) / sizeof(cam_phy3_cphy[0]));
	} else {
		cam_native_phy_registers(base, cam_phy4_dphy,
		    sizeof(cam_phy4_dphy) / sizeof(cam_phy4_dphy[0]));
	}
	reg_wr(base, 0x102cu, 0xffu);
	reg_wr(base, 0x1030u, 0xffu);
	reg_wr(base, 0x1034u, 0xfbu);
	reg_wr(base, 0x1038u, 0xffu);
	reg_wr(base, 0x103cu, 0x7fu);
	reg_wr(base, 0x1040u, 0xffu);
	reg_wr(base, 0x1044u, 0xffu);
	reg_wr(base, 0x1048u, 0xefu);
	reg_wr(base, 0x104cu, 0xffu);
	reg_wr(base, 0x1050u, 0xffu);
	reg_wr(base, 0x1054u, 0xffu);
	delay_for_interval(100, NSEC_PER_USEC);
	reg_wr(base, 0x1000u, cphy ? 0x0eu : 2u);
	if (cphy) {
		delay_for_interval(3048, NSEC_PER_USEC);
	}
	cam_native_phy_configured = reg_rd(base, 0x1014u) == (cphy ? 0x2au : 0xd5u) &&
	    reg_rd(base, 0x1000u) == (cphy ? 0x0eu : 2u);
	return cam_native_phy_configured;
}

static bool
cam_native_phy_down(void)
{
	bool cphy = cam_profile != NULL && cam_profile->sensor == 1u;
	vm_offset_t base = cphy ? cam_phy3_base : cam_phy4_base;
	if (cam_native_phy_supplies != 0 && base != 0) {
		reg_wr(base, 0x1014u, 0);
		reg_wr(base, 0x1000u, 1u);
		delay_for_interval(1, NSEC_PER_USEC);
	}
	if ((cam_native_phy_supplies & 2u) != 0) {
		if (!annibale_rpmh_regulator(cphy ? "ldof1" : "ldoi3", 880u, false, ANNIBALE_RPMH_MODE_NONE)) {
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

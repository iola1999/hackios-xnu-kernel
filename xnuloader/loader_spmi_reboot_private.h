#ifndef XNULOADER_SPMI_REBOOT_PRIVATE_H
#define XNULOADER_SPMI_REBOOT_PRIVATE_H

/* PMK8550 SID 0 SDAM 0x7148 selects LK Fastboot; require write completion and readback before reset. */
static int
spmi_reboot_lk_prepare(void)
{
	if (SCREEN.base != platform_annibale.fb_base) return 0;
	if (*(volatile uint32_t *)(SPMI_ARB_CORE + 0x000) < 0x70000000u) return 0;
	uint32_t channels = *(volatile uint32_t *)(SPMI_ARB_CORE + 0x004) & 0x7ffu, status = 0;
	if (channels == 0 || channels > 1024) channels = 1024;
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = *(volatile uint32_t *)(SPMI_ARB_CORE + (uintptr_t)0x2000ULL + (uintptr_t)(4 * n));
		if (((map >> 8) & 0xfffu) != 0x071u) continue;
		if ((*(volatile uint32_t *)(SPMI_ARB_CORE + (uintptr_t)0x2d000ULL + (uintptr_t)(4 * n)) & 0x7u) != 0) continue;
		uint8_t orig = 0, readback = 0;
		if (!spmi_obsrv_read((int)n, 0x48, &orig, 1)) return 0;
		uintptr_t wbase = (uintptr_t)0x0c500000ULL + (uintptr_t)0x1000ULL * n;
		*(volatile uint32_t *)(wbase + 0x10) = (uint32_t)((orig & 1u) | 4u);
		*(volatile uint32_t *)(wbase + 0x00) = 0x48u << 4;
		for (int i = 0; i < 4000; i++) {
			status = *(volatile uint32_t *)(wbase + 0x08);
			if (status & 0x1u) break;
		}
		if (!(status & 0x1u) || (status & 0xeu)) return 0;
		return spmi_obsrv_read((int)n, 0x48, &readback, 1) && readback == (uint8_t)((orig & 1u) | 4u);
	}
	return 0;
}

#endif

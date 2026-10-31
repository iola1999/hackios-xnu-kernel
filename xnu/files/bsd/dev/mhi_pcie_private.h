/*
 * What mhi_pcie.c (the survey of PCIe root complex 0) shares with mhi_pcie_power.c (its power-up).
 */
#ifndef MHI_PCIE_PRIVATE_H
#define MHI_PCIE_PRIVATE_H

#include <mach/mach_types.h>
#include <stdbool.h>
#include <stdint.h>

/* the survey's mappings: parf 0x1c00000, phy 0x1c06000, dm_core 0x40000000 (DBI, ELBI at +0xf20, iATU at
 * +0x1000), the clock controller 0x100000 and the pin registers 0xf100000 */
struct mhi_pcie_windows {
	vm_offset_t parf, phy, dm_core, gcc, tlmm;
};

/* what step 7 (mhi_pcie_fw.c) takes from the survey: the controller's windows, with region 0 left on
 * the card's config space, and where BAR0 was assigned */
struct mhi_pcie_card {
	vm_offset_t parf, dm_core, conf;
	uint32_t bar0;
};

int mhi_pcie_say(const char *format, ...) __printflike(1, 2);
void mhi_pcie_pause(const char *what);
bool mhi_pcie_power_up(const struct mhi_pcie_windows *windows);
bool mhi_pcie_firmware(const struct mhi_pcie_card *card);
void mhi_pcie_ipcr(void);

static inline uint32_t
mhi_pcie_rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
mhi_pcie_wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

#endif

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
bool mhi_pcie_ipcr(void);
void *mhi_pcie_dma_alloc(uint64_t bytes, uint64_t *pa);
void mhi_pcie_wlfw(void);
void mhi_pcie_wake(void);
/* boot-arg bt=N: the chip's Bluetooth half over SE14 in a thread of its own (annibale_bt_hci.c), once powered */
void annibale_bt_start(void);
void annibale_bt_ios_early(void);

/* step 10: the card for the Wi-Fi driver (annibale_lx.c), set up by mhi_pcie.c when pcie=10 starts it */
bool mhi_pcie_radio(void);                                      /* the driver runs on the card */
int mhi_pcie_radio_boot(void);                                  /* its firmware to mission mode, QRTR up: 0 or an errno */
void *mhi_pcie_radio_memory(uint64_t bytes, uint64_t *pa);      /* pages below 4 GB, the card's address = pa */
bool mhi_pcie_radio_registers(uint64_t *pa, uint64_t *size);    /* BAR0 */
const uint8_t *mhi_pcie_radio_file(const char *name, uint64_t *size);   /* by Linux's name for it */

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

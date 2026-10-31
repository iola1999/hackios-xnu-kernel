/*
 * Step 7 of the PCIe survey (pcie=7 with pcie_power=1): the WCN7850's firmware, from the primary boot
 * loader the card waits in (step 5 read execution environment 0, PBL) to mission mode. The order and
 * the register work are those of hakios-arm64e-dada's bsd/dev/hakios_dada_wifi.c (HakiOS, APSL 2.0),
 * which booted the Xiaomi 15's card on PCIe 0 of the same SoC; the card's own registers are ath12k's
 * for WCN7850 (Linux drivers/net/wireless/ath/ath12k pci.c and hw.c: the hot reset at 0x1e40304).
 *
 *   - The controller's BDF-to-SID table on, the card (RID 0x100) at the slot its CRC-8 names, 0x15
 *     (step 5 computed the same), as stream 0x1401 (the device tree's iommu-map). Left in its bypass,
 *     the controller tags the card's DMA with a stream the SMMU does not let through; on dada the
 *     card's first write after its boot loader started stopped the SoC a second later.
 *   - The apps SMMU: streams 0x1400 and 0x1401 matched to the last context bank, which is set to not
 *     translate (stage 1 off). dada routes its sound and Wi-Fi streams the same way: the hypervisor
 *     gives no translating context. A stream already matched is left as found, and a last bank
 *     already in use stops the step.
 *   - DMA memory: the card takes 32-bit addresses and the kernel's RAM is above 4 GB, so all the card
 *     reads or writes lies in the 128 MB the loader keeps below 4 GB (lowdma=, lowdma_mb=; loader v14).
 *   - The card's own power-up (ath12k_pci_power_up's register work), MSI pointed at a page of that
 *     memory nobody reads, ASPM off; then mhi_boot.c (UniXNU's MhiBoot): the secondary boot loader
 *     (the image's first 512 KiB) through BHI, the MHI contexts at READY, M0, the whole image through
 *     BHIe, mission mode. Everything is polled.
 *
 * The image is fw/amss20.bin of the RAM disk (redwood_image.c; package.py WIFI_FW=1) when the SoC
 * version register says hardware 2.0, the only WCN7850 ath12k knows, else fw/amss.bin; pcie_fw=1 or 2
 * picks one. pcie_fw=3 takes fw/lfw-amss.bin, linux-firmware's ath12k/WCN7850/hw2.0/amss.bin (the build
 * made for ath12k; with its m3.bin as fw/lfw-m3.bin, mhi_pcie.c), copied there by hand. Nothing is undone: the card keeps running, the streams stay matched and the memory stays
 * the card's until the phone resets. Step 8, at the end of this file, starts QRTR's channels on top.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/redwood_qrtr.h>
#include <vm/vm_kern.h>
#include "mhi_boot.h"
#include "mhi_pcie_private.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

#define printf mhi_pcie_say
#define rd mhi_pcie_rd
#define wr mhi_pcie_wr

/* PARF (pcie-qcom.c qcom_pcie_config_sid_1_9_0) */
#define PARF_BDF_TO_SID_TABLE     0x2000
#define PARF_BDF_TO_SID_CFG       0x2c00            /* bit 0: bypass */
#define CARD_SID_SLOT             0x15              /* CRC-8 of RID 0x100 */

/* apps SMMU (ARM SMMUv2 global space 0; device tree reg 0x15000000 + 0x100000) */
#define APPS_SMMU_PHYS            0x15000000UL
#define APPS_SMMU_SIZE            0x100000UL
#define SMMU_SMR(n)               (0x800 + 4 * (n))
#define SMMU_S2CR(n)              (0xc00 + 4 * (n))
#define SMMU_CBAR(n)              (0x1000 + 4 * (n))
#define SMMU_CBA2R(n)             (0x1800 + 4 * (n))
#define SMR_VALID                 (1u << 31)
#define CBAR_S1_BYPASS_S2         0x10000u          /* type 1: stage 1 context, stage 2 bypassed */
#define CBA2R_VA64                1u

/* the card's registers above the first 512 KiB of BAR0 go through a window (ath12k_pci_select_window) */
#define WINDOW_REG                0x310c
#define WINDOW_ENABLE             0x40000000u
#define WINDOW_START              0x80000u
#define BAR_MAP                   0x100000u

/* ath12k pci.h / pci.c, WCN7850 */
#define TCSR_SOC_HW_VERSION       0x1b00000
#define PCIE_PARF_LTSSM           0x1e081b0
#define PARM_LTSSM_VALUE          0x111
#define GCC_PCIE_HOT_RST          0x1e40304
#define PCIE_INT_ALL_CLEAR        0x1e08228
#define WLAON_QFPROM_PWR_CTRL     0x1f8031c
#define PCIE_Q6_COOKIE            0x1f80500
#define WLAON_WARM_SW_ENTRY       0x1f80504
#define WLAON_SOC_RESET_CAUSE     0x1f8060c
#define PCIE_SOC_GLOBAL_RESET     0x3008

static vm_offset_t bar, conf, smmu;
static uint32_t window = UINT32_MAX;

static void
ms(unsigned n)
{
	assert_wait_timeout((event_t)&window, THREAD_UNINT, n, NSEC_PER_MSEC);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint64_t
uptime_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static uint32_t
card_rd(uint32_t off)
{
	if (off < WINDOW_START) {
		return rd(bar + off);
	}
	if (((off >> 19) & 0x3f) != window) {
		window = (off >> 19) & 0x3f;
		wr(bar + WINDOW_REG, WINDOW_ENABLE | window);
		(void)rd(bar + WINDOW_REG);
	}
	return rd(bar + WINDOW_START + (off & 0x7ffff));
}

static void
card_wr(uint32_t off, uint32_t value)
{
	if (off < WINDOW_START) {
		wr(bar + off, value);
		return;
	}
	if (((off >> 19) & 0x3f) != window) {
		window = (off >> 19) & 0x3f;
		wr(bar + WINDOW_REG, WINDOW_ENABLE | window);
		(void)rd(bar + WINDOW_REG);
	}
	wr(bar + WINDOW_START + (off & 0x7ffff), value);
}

/* ---- DMA memory: the loader's below 4 GB, write-combined (Normal non-cacheable, pmap.c) ------------------- */

static struct {
	vm_offset_t va;
	uint64_t pa, size, used;
} dma;

/* the loader's memory, mapped once */
static bool
dma_open(void)
{
	uint64_t low = 0, mb = 0;

	if (dma.va != 0) {
		return true;
	}
	if (!PE_parse_boot_argn("lowdma", &low, sizeof(low)) || !PE_parse_boot_argn("lowdma_mb", &mb, sizeof(mb)) ||
	    mb < 32 || low == 0 || low + (mb << 20) > 0x100000000ULL) {
		printf("pcie: no memory below 4 GB from the loader (lowdma=, lowdma_mb=): loader v14 or later\n");
		return false;
	}
	dma.pa = low;
	dma.size = mb << 20;
	dma.va = ml_io_map_wcomb((vm_offset_t)dma.pa, (vm_size_t)dma.size);
	return dma.va != 0;
}

static void *
dma_alloc_aligned(uint64_t bytes, uint64_t align, uint64_t *pa)
{
	uint64_t at;

	if (!dma_open()) {
		return NULL;
	}
	bytes = (bytes + 4095) & ~4095ull;
	at = ((dma.pa + dma.used + align - 1) & ~(align - 1)) - dma.pa;
	if (at + bytes > dma.size) {
		return NULL;
	}
	dma.used = at + bytes;
	*pa = dma.pa + at;
	bzero((void *)(dma.va + at), (size_t)bytes);
	return (void *)(dma.va + at);
}

/* aligned to its own size rounded up to a power of two, as the boot loader wants its image (UniXNU arenaAlloc) */
static void *
dma_alloc(uint64_t bytes, uint64_t *pa)
{
	uint64_t align;

	for (align = 4096; align < bytes; align <<= 1) {
	}
	return dma_alloc_aligned(bytes, align, pa);
}

/* for step 9 (mhi_pcie_wlfw.c): what the firmware asks for, and the M3 image */
void *
mhi_pcie_dma_alloc(uint64_t bytes, uint64_t *pa)
{
	return dma_alloc(bytes, pa);
}

/* for the driver (redwood_lx.c): its memory for the card, in pages; the card sees it at its physical address */
void *
mhi_pcie_radio_memory(uint64_t bytes, uint64_t *pa)
{
	return dma_alloc_aligned(bytes, 4096, pa);
}

/* ---- streams: the controller's table, then the SMMU ---------------------------------------------------------- */

static void
sid_table(vm_offset_t parf)
{
	uint32_t before = rd(parf + PARF_BDF_TO_SID_CFG);

	for (uint32_t i = 0; i < 256; i++) {
		wr(parf + PARF_BDF_TO_SID_TABLE + 4 * i, 0);
	}
	wr(parf + PARF_BDF_TO_SID_CFG, before & ~1u);
	wr(parf + PARF_BDF_TO_SID_TABLE + 4 * CARD_SID_SLOT, 0x100u << 16 | 1u << 8);
	printf("pcie: step 7: BDF-to-SID config %#x -> %#x, slot %#x %#x (RID 0x100, SID 0x1400 + 1)\n", before,
	    rd(parf + PARF_BDF_TO_SID_CFG), CARD_SID_SLOT, rd(parf + PARF_BDF_TO_SID_TABLE + 4 * CARD_SID_SLOT));
}

static uint32_t smmu_streams, smmu_bank, smmu_cb;

/* the SMMU's shape, the matches already in place for PCIe 0's streams, and the last bank's state */
static bool
smmu_open(void)
{
	uint32_t idr0, idr1, banks, pages, used = 0;

	smmu = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
	if (smmu == 0) {
		return false;
	}
	idr0 = rd(smmu + 0x20);
	idr1 = rd(smmu + 0x24);
	smmu_streams = MIN(idr0 & 0xff, 128u);
	banks = idr1 & 0xff;
	pages = 1u << (((idr1 >> 28) & 7) + 1);
	printf("pcie: step 7: apps SMMU SCR0 %#x IDR0 %#x IDR1 %#x: %u stream matches, %u context banks, %u pages\n",
	    rd(smmu), idr0, idr1, smmu_streams, banks, pages);
	if ((idr1 >> 31) != 0 || banks < 2 || pages > 128 || smmu_streams == 0) {
		printf("pcie: step 7: 64 KiB pages or a shape the offsets here do not cover\n");
		return false;
	}
	smmu_bank = banks - 1;
	smmu_cb = (pages + smmu_bank) * 0x1000;
	for (uint32_t i = 0; i < smmu_streams; i++) {
		uint32_t smr = rd(smmu + SMMU_SMR(i));

		if (!(smr & SMR_VALID)) {
			continue;
		}
		used++;
		if ((smr & 0x7fff) >= 0x1400 && (smr & 0x7fff) < 0x1800) {
			printf("pcie: step 7: match %u: SMR %#x S2CR %#x\n", i, smr, rd(smmu + SMMU_S2CR(i)));
		}
	}
	printf("pcie: step 7: %u of %u matches in use; bank %u: CBAR %#x CBA2R %#x SCTLR %#x\n", used, smmu_streams,
	    smmu_bank, rd(smmu + SMMU_CBAR(smmu_bank)), rd(smmu + SMMU_CBA2R(smmu_bank)), rd(smmu + smmu_cb));
	return true;
}

/* STREAM to the last bank without translation, in the first free match; true when it passes through */
static bool
smmu_pass(uint16_t stream)
{
	uint32_t slot = UINT32_MAX;

	for (uint32_t i = 0; i < smmu_streams; i++) {
		uint32_t smr = rd(smmu + SMMU_SMR(i)), mask = (smr >> 16) & 0x7fff, s2cr;

		if (!(smr & SMR_VALID)) {
			slot = MIN(slot, i);
			continue;
		}
		if (((smr ^ stream) & 0x7fff & ~mask) == 0) {
			s2cr = rd(smmu + SMMU_S2CR(i));
			printf("pcie: step 7: stream %#x is matched already (%u: %#x -> %#x)\n", stream, i, smr, s2cr);
			return ((s2cr >> 16) & 3) == 1 || ((s2cr & 0xff) == smmu_bank && !(rd(smmu + smmu_cb) & 1));
		}
	}
	if (slot == UINT32_MAX || (rd(smmu + smmu_cb) & 1)) {
		printf("pcie: step 7: stream %#x: no free match, or bank %u translates for somebody\n", stream, smmu_bank);
		return false;
	}
	if (rd(smmu + SMMU_CBAR(smmu_bank)) != CBAR_S1_BYPASS_S2 || rd(smmu + SMMU_CBA2R(smmu_bank)) != CBA2R_VA64) {
		wr(smmu + smmu_cb, 0);
		wr(smmu + SMMU_CBA2R(smmu_bank), CBA2R_VA64);
		wr(smmu + SMMU_CBAR(smmu_bank), CBAR_S1_BYPASS_S2);
	}
	wr(smmu + SMMU_S2CR(slot), smmu_bank);
	wr(smmu + SMMU_SMR(slot), SMR_VALID | stream);
	printf("pcie: step 7: stream %#x passes through: match %u -> bank %u\n", stream, slot, smmu_bank);
	return rd(smmu + SMMU_SMR(slot)) == (SMR_VALID | stream);
}

/* ---- the card ----------------------------------------------------------------------------------------------- */

/* ath12k_pci_power_up's register work: LTSSM, hot reset, interrupts left over, BHIe doorbells cleared, the warm
 * boot marker cleared, the SoC's global reset pulsed, MHI reset */
static void
card_power_up(void)
{
	uint32_t v = card_rd(PCIE_PARF_LTSSM);

	for (unsigned i = 0; v != PARM_LTSSM_VALUE && i < 5; i++) {
		if (v == UINT32_MAX) {
			ms(5);
		}
		card_wr(PCIE_PARF_LTSSM, PARM_LTSSM_VALUE);
		v = card_rd(PCIE_PARF_LTSSM);
	}
	card_wr(GCC_PCIE_HOT_RST, card_rd(GCC_PCIE_HOT_RST) | 0x10);
	ms(5);
	card_wr(PCIE_INT_ALL_CLEAR, UINT32_MAX);
	card_wr(WLAON_QFPROM_PWR_CTRL, card_rd(WLAON_QFPROM_PWR_CTRL) & ~4u);
	card_wr(0x360, 0);                              /* BHIe TX and RX vector doorbells and status */
	card_wr(0x368, 0);
	card_wr(0x394, 0);
	card_wr(0x39c, 0);
	printf("pcie: step 7: card LTSSM %#x, Q6 cookie %#x, warm entry %#x, reset cause %#x\n", v,
	    card_rd(PCIE_Q6_COOKIE), card_rd(WLAON_WARM_SW_ENTRY), card_rd(WLAON_SOC_RESET_CAUSE));
	ms(10);
	card_wr(WLAON_WARM_SW_ENTRY, 0);
	ms(10);
	v = card_rd(PCIE_SOC_GLOBAL_RESET);
	card_wr(PCIE_SOC_GLOBAL_RESET, v | 1);
	(void)card_rd(PCIE_SOC_GLOBAL_RESET);
	ms(10);
	card_wr(PCIE_SOC_GLOBAL_RESET, v & ~1u);
	(void)card_rd(PCIE_SOC_GLOBAL_RESET);
	ms(10);
	window = UINT32_MAX;                            /* the reset took the window register with it */
	printf("pcie: step 7: card global reset pulsed (%#x); MHI status %#x\n", v, rd(bar + 0x48));
	wr(bar + 0x38, 2);                              /* MHI reset: clears the SYS_ERR the global reset leaves */
	ms(10);
	printf("pcie: step 7: after the MHI reset: status %#x, execution environment %u\n", rd(bar + 0x48),
	    rd(bar + rd(bar + 0x28) + 0x28));
}

/* ASPM off while it loads; MSI on, its writes landing in SINK (UniXNU endpointLinkSetup) */
static void
card_link_setup(uint64_t sink)
{
	uint8_t cap = (uint8_t)(rd(conf + 0x34) & 0xfc);

	for (unsigned guard = 0; cap >= 0x40 && guard < 48; guard++) {
		uint32_t w = rd(conf + cap);
		uint8_t id = (uint8_t)w, next = (uint8_t)(w >> 8) & 0xfc;

		if (id == 0x10) {
			wr(conf + cap + 0x10, rd(conf + cap + 0x10) & ~3u);
		} else if (id == 0x05) {
			bool is64 = (w >> 16) & 0x80;

			wr(conf + cap + 4, (uint32_t)sink);
			if (is64) {
				wr(conf + cap + 8, (uint32_t)(sink >> 32));
				wr(conf + cap + 0xc, 0);
			} else {
				wr(conf + cap + 8, 0);
			}
			wr(conf + cap, (w & 0xffff) | ((((w >> 16) & ~0x70u) | 0x20u | 1u) << 16));
			printf("pcie: step 7: MSI at %#x: control %#x, address %#llx\n", cap, rd(conf + cap) >> 16, sink);
		}
		if (next == cap) {
			break;
		}
		cap = next;
	}
}

/* the device's context arrays (mhi_init_dev_ctxt; UniXNU programContexts): 128 channels off, two event rings, the
 * command ring, and the registers that point at them */
#define CTX                     44u
#define CHANNELS                128u
#define EVENT_RINGS             2u
#define ELEMENT                 16u
#define CMD_ELEMENTS            8u
static const uint32_t ev_len[EVENT_RINGS] = { 32 * ELEMENT, 256 * ELEMENT };
static uint8_t *ctx_va;
static uint64_t ctx_pa, ev_pa[EVENT_RINGS], cmd_pa;

static void
contexts(uint64_t pa)
{
	uint32_t ev_off = CHANNELS * CTX, cmd_off = ev_off + EVENT_RINGS * CTX, cur = (cmd_off + CTX + 4095) & ~4095u;
	uint64_t ev_base[EVENT_RINGS], cmd_base, v64;
	uint32_t erdb, v32;

#define W32(at, v) do { v32 = (v); memcpy(ctx_va + (at), &v32, 4); } while (0)
#define W64(at, v) do { v64 = (v); memcpy(ctx_va + (at), &v64, 8); } while (0)
	for (uint32_t i = 0; i < EVENT_RINGS; i++) {
		uint32_t at = ev_off + i * CTX;

		ev_base[i] = ev_pa[i] = pa + cur;
		W32(at, i == 0 ? 0 : 1u << 16);             /* interrupt moderation */
		W32(at + 4, 1);                             /* a valid event ring */
		W32(at + 8, i + 1);                         /* its MSI vector */
		W64(at + 12, ev_base[i]);
		W64(at + 20, ev_len[i]);
		W64(at + 28, ev_base[i]);
		W64(at + 36, ev_base[i] + ev_len[i] - ELEMENT);
		cur += ev_len[i];
	}
	cmd_base = cmd_pa = pa + cur;
	W64(cmd_off + 12, cmd_base);
	W64(cmd_off + 20, CMD_ELEMENTS * ELEMENT);
	W64(cmd_off + 28, cmd_base);
	W64(cmd_off + 36, cmd_base);
#undef W32
#undef W64
	__builtin_arm_dsb(0xf);
	wr(bar + 0x58, (uint32_t)pa);                   /* channel, event and command context arrays */
	wr(bar + 0x5c, (uint32_t)(pa >> 32));
	wr(bar + 0x60, (uint32_t)(pa + ev_off));
	wr(bar + 0x64, (uint32_t)((pa + ev_off) >> 32));
	wr(bar + 0x68, (uint32_t)(pa + cmd_off));
	wr(bar + 0x6c, (uint32_t)((pa + cmd_off) >> 32));
	wr(bar + 0x80, 0);                              /* control and data address ranges: all of 32 bits */
	wr(bar + 0x84, 0);
	wr(bar + 0x88, UINT32_MAX);
	wr(bar + 0x8c, 0);
	wr(bar + 0x98, 0);
	wr(bar + 0x9c, 0);
	wr(bar + 0xa0, UINT32_MAX);
	wr(bar + 0xa4, 0);
	wr(bar + 0x10, (rd(bar + 0x10) & ~0xffff0000u) | EVENT_RINGS << 16);
	erdb = rd(bar + 0x20);
	for (uint32_t i = 0; erdb + 8 * EVENT_RINGS <= BAR_MAP && i < EVENT_RINGS; i++) {
		wr(bar + erdb + 8 * i, (uint32_t)(ev_base[i] + ev_len[i] - ELEMENT));
		wr(bar + erdb + 8 * i + 4, (uint32_t)((ev_base[i] + ev_len[i] - ELEMENT) >> 32));
	}
}

/* the host's half of the event rings: what the device posted is taken, the ring given back (UniXNU eventAck) */
static unsigned
events_ack(void)
{
	static uint64_t acked[EVENT_RINGS];
	uint32_t erdb = rd(bar + 0x20);
	unsigned moved = 0;

	for (unsigned r = 0; r < EVENT_RINGS; r++) {
		uint8_t *c = ctx_va + CHANNELS * CTX + r * CTX;
		uint64_t base, len, rp, wp;

		memcpy(&base, c + 12, 8);
		memcpy(&len, c + 20, 8);
		memcpy(&rp, c + 28, 8);
		if (rp == acked[r] || rp < base || rp >= base + len) {
			continue;
		}
		acked[r] = rp;
		wp = rp == base ? base + len - ELEMENT : rp - ELEMENT;
		memcpy(c + 36, &wp, 8);
		__builtin_arm_dsb(0xf);
		wr(bar + erdb + 8 * r, (uint32_t)wp);
		wr(bar + erdb + 8 * r + 4, (uint32_t)(wp >> 32));
		moved++;
	}
	return moved;
}

static uint32_t
mhi_read(void *c __unused, uint32_t reg)
{
	return rd(bar + reg);
}

static void
mhi_write(void *c __unused, uint32_t reg, uint32_t value)
{
	wr(bar + reg, value);
}

/* ---- the run ------------------------------------------------------------------------------------------------ */

#define SEGMENT                 (512u << 10)        /* ath12k WCN7850: sbl_size and seg_len, 512 KiB */
#define MAX_SEGMENTS            32u
#define PHASE_LIMIT_MS          20000u
#define RUN_LIMIT_MS            60000u

static const char *const phases[] = { "idle", "resetting", "loading the boot loader", "waiting for READY", "contexts",
	                              "waiting for the boot loader", "loading the image", "waiting for mission mode",
	                              "done", "failed" };

/* the card's DMA memory and image, laid out; false (said why) when something is missing */
static bool
stage_image(uint64_t *seg_pa, uint32_t *size, uint32_t *segments)
{
	static const char *const names[] = { NULL, "amss.bin", "amss20.bin", "lfw-amss.bin" };
	uint64_t soc_version, disk_size = 0;
	uint32_t which = 0;
	const uint8_t *image;
	int disk = 0;

	if (!dma_open()) {
		return false;
	}
	soc_version = card_rd(TCSR_SOC_HW_VERSION);
	(void)PE_parse_boot_argn("pcie_fw", &which, sizeof(which));
	if (which == 0 || which >= sizeof(names) / sizeof(names[0])) {
		which = ((soc_version >> 8) & 0xf) == 2 ? 2 : 1;
	}
	image = redwood_image_file("fw", names[which], &disk_size, &disk);
	printf("pcie: step 7: SoC version %#llx (major %llu minor %llu): %s, %llu bytes from md%d\n", soc_version,
	    (soc_version >> 8) & 0xf, (soc_version >> 4) & 0xf, names[which], disk_size, disk);
	if (image == NULL || disk_size == 0 || disk_size > MAX_SEGMENTS * SEGMENT) {
		printf("pcie: step 7: no image in fw/ of the RAM disk (package.py WIFI_FW=1)\n");
		return false;
	}
	*size = (uint32_t)disk_size;
	*segments = (*size + SEGMENT - 1) / SEGMENT;
	for (uint32_t i = 0; i < *segments; i++) {
		uint8_t *seg = dma_alloc(SEGMENT, &seg_pa[i]);

		if (seg == NULL) {
			printf("pcie: step 7: no DMA memory for segment %u\n", i);
			return false;
		}
		memcpy(seg, image + i * SEGMENT, MIN(SEGMENT, *size - i * SEGMENT));
	}
	return true;
}

/*
 * pcie_rddm=1: room for the card's RAM dump. A card that faults goes to execution environment 3 (RDDM)
 * and writes its memory into the buffers of the BHIe receive vector the host left it (Linux
 * drivers/bus/mhi/host/boot.c, mhi_rddm_prepare and mhi_download_rddm_image); 0x780000 bytes is the
 * vendor device tree's qcom,wlan-ramdump-dynamic. The loader stages only the kernel's own window, so the
 * dump is copied into a contiguous block of it, and run-auto.py saves the block a "fetch:" line names
 * before it resets the phone.
 */
#define BHIE_RXVECADDR_LOW      0x60
#define BHIE_RXVECADDR_HIGH     0x64
#define BHIE_RXVECSIZE          0x68
#define BHIE_RXVECDB            0x70
#define BHIE_RXVECSTATUS        0x78
#define RDDM_SIZE               0x780000u

static struct {
	uint32_t bhie, seq, entries;
	uint64_t pa, table_pa;
	uint8_t *va;
} rddm;

static void
rddm_arm(uint32_t seq)
{
	rddm.seq = seq;
	wr(bar + rddm.bhie + BHIE_RXVECADDR_HIGH, (uint32_t)(rddm.table_pa >> 32));
	wr(bar + rddm.bhie + BHIE_RXVECADDR_LOW, (uint32_t)rddm.table_pa);
	wr(bar + rddm.bhie + BHIE_RXVECSIZE, rddm.entries * (uint32_t)sizeof(MhiBhieEntry));
	wr(bar + rddm.bhie + BHIE_RXVECDB, seq & MHI_SEQ_MASK);
}

static void
rddm_prepare(uint32_t bhie)
{
	uint64_t seg_pa[RDDM_SIZE / SEGMENT];
	MhiBhieEntry *table;
	uint32_t on = 0;

	if (!PE_parse_boot_argn("pcie_rddm", &on, sizeof(on)) || on == 0) {
		return;
	}
	rddm.va = dma_alloc(RDDM_SIZE, &rddm.pa);
	table = dma_alloc(4096, &rddm.table_pa);
	if (rddm.va == NULL || table == NULL) {
		printf("pcie: rddm: no DMA memory for the card's RAM dump\n");
		rddm.va = NULL;
		return;
	}
	for (uint32_t i = 0; i < RDDM_SIZE / SEGMENT; i++) {
		seg_pa[i] = rddm.pa + (uint64_t)i * SEGMENT;
	}
	rddm.entries = mhi_bhie_table(table, 4096 / sizeof(MhiBhieEntry), seg_pa, RDDM_SIZE, SEGMENT);
	rddm.bhie = bhie;
	__builtin_arm_dsb(0xf);
	rddm_arm(0x2dd3);
	printf("pcie: rddm: %u bytes at %#llx in %u segments, table %#llx, left with the card\n", RDDM_SIZE, rddm.pa,
	    rddm.entries, rddm.table_pa);
}

/* at SYS_ERR, from step 8's thread: the dump awaited (armed once more if the card ignores the first), then kept */
static void
rddm_collect(void)
{
	uint32_t status = 0, used = RDDM_SIZE;
	vm_offset_t copy = 0;

	if (rddm.va == NULL) {
		return;
	}
	for (unsigned attempt = 0; attempt < 2 && status >> 30 != MHI_XFER_SUCCESS; attempt++) {
		if (attempt == 1) {
			printf("pcie: rddm: nothing after 2 s (receive status %#x); the vector armed again\n", status);
			rddm_arm(rddm.seq + 1);
		}
		for (unsigned i = 0; i < (attempt == 0 ? 400u : 600u); i++) {
			status = rd(bar + rddm.bhie + BHIE_RXVECSTATUS);
			if (status >> 30 != 0) {
				break;
			}
			ms(5);
		}
	}
	while (used > 0 && rddm.va[used - 1] == 0) {
		used--;
	}
	printf("pcie: rddm: receive status %#x (sequence %#x), %u bytes up to the last one not 0\n", status, rddm.seq, used);
	for (uint32_t at = 0; at < 64 && at < used; at += 16) {
		const uint8_t *b = rddm.va + at;

		printf("pcie: rddm: %04x: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", at, b[0], b[1], b[2],
		    b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	}
	if (used == 0) {
		return;
	}
	if (kmem_alloc_contig(kernel_map, &copy, round_page(used), PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("pcie: rddm: no contiguous memory for a copy the loader can stage\n");
		return;
	}
	memcpy((void *)copy, rddm.va, used);
	printf("pcie: rddm: copied for the host; fetch: rddm %#llx %#x\n", kvtophys(copy), used);
}

bool
mhi_pcie_firmware(const struct mhi_pcie_card *card)
{
	uint64_t seg_pa[MAX_SEGMENTS], table_pa, sink_pa, start, since;
	uint32_t size = 0, segments = 0, entries, phase, last;
	unsigned acks = 0;
	MhiBhieEntry *table;
	MhiBoot boot;
	MhiIo io = { mhi_read, mhi_write, NULL, BAR_MAP };

	conf = card->conf;
	bar = bar != 0 ? bar : card->bar0 != 0 ? ml_io_map(card->bar0, BAR_MAP) : 0;
	window = UINT32_MAX;                            /* (the driver moves it too, pcie=10) */
	if (bar == 0) {
		printf("pcie: step 7: no BAR0 to map\n");
		return false;
	}
	if (!stage_image(seg_pa, &size, &segments)) {
		return false;
	}
	table = dma_alloc(4096, &table_pa);
	ctx_va = dma_alloc(32 << 10, &ctx_pa);
	if (table == NULL || ctx_va == NULL || dma_alloc(4096, &sink_pa) == NULL) {
		return false;
	}
	entries = mhi_bhie_table(table, 4096 / sizeof(MhiBhieEntry), seg_pa, size, SEGMENT);
	__builtin_arm_dsb(0xf);
	printf("pcie: step 7: image %u bytes in %u segments from %#llx, table %#llx (%u entries), contexts %#llx\n",
	    size, segments, seg_pa[0], table_pa, entries, ctx_pa);

	mhi_pcie_pause("step 7: the controller's BDF-to-SID table");
	sid_table(card->parf);
	mhi_pcie_pause("step 7: the apps SMMU's matches for streams 0x1400 and 0x1401");
	if (!smmu_open() || !smmu_pass(0x1400) || !smmu_pass(0x1401)) {
		printf("pcie: step 7: the card's streams cannot be passed through; stopped before its power-up\n");
		return false;
	}
	mhi_pcie_pause("step 7: the card's power-up");
	card_power_up();
	card_link_setup(sink_pa);
	if (mhi_boot_open(&boot, &io) != 0) {
		printf("pcie: step 7: MHI registers out of shape (BHI %#x, BHIe %#x)\n", rd(bar + 0x28), rd(bar + 0x2c));
		return false;
	}
	contexts(ctx_pa);
	mhi_pcie_pause("step 7: the boot loader through BHI");
	phase = mhi_boot_begin(&boot, 0, seg_pa[0], MIN(SEGMENT, size), table_pa,
	    entries * (uint32_t)sizeof(MhiBhieEntry), 0x5eed);
	printf("pcie: step 7: boot loader asked for (execution environment %u, state %u): %s\n", boot.ee, boot.state,
	    phases[phase]);
	last = phase;
	start = since = uptime_ms();
	while (phase != MhiDone && phase != MhiFailed && uptime_ms() - start < RUN_LIMIT_MS) {
		if (phase == MhiNeedContexts) {
			contexts(ctx_pa);               /* the boot loader clears the MHI registers when it starts */
			phase = mhi_boot_contexts_done(&boot);
		} else {
			phase = mhi_boot_step(&boot);
		}
		if (phase != MhiNeedContexts && phase != MhiDone && phase != MhiFailed) {
			acks += events_ack();
		}
		if (phase != last) {
			printf("pcie: step 7: %s after %llu ms (execution environment %u, state %u, status %#x)\n",
			    phases[phase], uptime_ms() - start, boot.ee, boot.state, boot.last_status);
			last = phase;
			since = uptime_ms();
		} else if (uptime_ms() - since > PHASE_LIMIT_MS) {
			phase = mhi_boot_timeout(&boot);
		}
		ms(5);
	}
	printf("pcie: step 7: the end after %llu ms: %s (error %u in %s; execution environment %u, state %u, BHI status "
	    "%#x error %#x; %u event ring moves)\n", uptime_ms() - start, phases[MIN(phase, 9u)], boot.error,
	    phases[MIN(boot.failed_phase, 9u)], mhi_boot_ee(&boot), mhi_boot_state(&boot),
	    rd(bar + boot.bhi + 0x2c), rd(bar + boot.bhi + 0x30), acks);
	if (phase == MhiDone) {
		rddm_prepare(boot.bhie);
	}
	return phase == MhiDone;
}

/*
 * ---- step 8 (pcie=8): channels 20 and 21, "IPCR", under QRTR ------------------------------------------------------
 *
 * QRTR runs over the MHI channel pair IPCR (ath12k mhi.c: channel 20 to the card, 21 from it, both reporting on event
 * ring 1). Their contexts are filled and each is started with a command (MHI_CMD_START_CHAN, completed on event ring
 * 0); receive buffers wait on 21. The router (redwood_qrtr.c) is attached to channel 20 and becomes the QMI helpers'
 * transport; a thread of ours then reads both event rings for as long as the phone runs, handing what arrives on 21
 * to the router: the card's HELLO is answered there, and its name service tells of the Wi-Fi firmware's service
 * (WLFW, 69). Ring elements and events after Linux drivers/bus/mhi/common.h and internal.h.
 */
#define MHI_CRDB                0x70
#define MHI_CHDB(n)             (rd(bar + 0x18) + 8 * (n))
#define MHI_ERDB(n)             (rd(bar + 0x20) + 8 * (n))
#define IPCR_UL                 20u
#define IPCR_DL                 21u
#define IPCR_ELEMENTS           16u
#define IPCR_BUFFER             0x8000u
#define PKT_TRANSFER            0x02u
#define PKT_START_CHAN          0x12u
#define EV_CMD_COMPLETION       0x21u
#define EV_TRANSFER             0x22u
#define EV_STATE_CHANGE         0x20u
#define MHI_SYS_ERR             0xffu
#define TRE_IEOT                (1u << 9)

struct ring {
	uint8_t *va;
	uint64_t pa, wp;
	uint32_t elements;
};

static LCK_GRP_DECLARE(ipcr_lock_group, "mhi-ipcr");
static LCK_MTX_DECLARE(ul_lock, &ipcr_lock_group);      /* channel 20's ring and buffers, written by any sender */
static struct ring cmd, ul, dl;
static uint64_t ev_local[EVENT_RINGS];
static uint8_t *dl_buffer[IPCR_ELEMENTS], *ul_buffer[IPCR_ELEMENTS];
static uint64_t dl_buffer_pa[IPCR_ELEMENTS], ul_buffer_pa[IPCR_ELEMENTS];
static uint8_t dl_at[IPCR_ELEMENTS];            /* which buffer each element of channel 21's ring holds */
static unsigned commands_done, ul_pending;

static void
put64(uint8_t *at, uint64_t v)
{
	memcpy(at, &v, 8);
}

static uint64_t
get64(const uint8_t *at)
{
	uint64_t v;

	memcpy(&v, at, 8);
	return v;
}

static void
doorbell(uint32_t reg, uint64_t value)
{
	__builtin_arm_dsb(0xf);
	wr(bar + reg + 4, (uint32_t)(value >> 32));
	wr(bar + reg, (uint32_t)value);
}

/* one element at the ring's write pointer; the pointer moves on, in the context too */
static void
ring_put(struct ring *r, uint8_t *context_wp, uint64_t pointer, uint32_t dword0, uint32_t dword1)
{
	uint8_t *el = r->va + (r->wp - r->pa);

	put64(el, pointer);
	memcpy(el + 8, &dword0, 4);
	memcpy(el + 12, &dword1, 4);
	r->wp += ELEMENT;
	if (r->wp == r->pa + (uint64_t)r->elements * ELEMENT) {
		r->wp = r->pa;
	}
	put64(context_wp, r->wp);
}

static void
dl_queue(uint8_t buffer)
{
	dl_at[(dl.wp - dl.pa) / ELEMENT] = buffer;
	ring_put(&dl, ctx_va + IPCR_DL * CTX + 36, dl_buffer_pa[buffer], IPCR_BUFFER, PKT_TRANSFER << 16 | TRE_IEOT);
}

/* the router's link: one datagram onto channel 20, in the buffer of the ring element it goes into */
static int
ipcr_send(const void *data, uint32_t length)
{
	uint32_t slot;

	if (length > IPCR_BUFFER) {
		return EMSGSIZE;
	}
	lck_mtx_lock(&ul_lock);
	if (ul_pending >= IPCR_ELEMENTS - 1) {
		lck_mtx_unlock(&ul_lock);
		return ENOBUFS;
	}
	slot = (uint32_t)((ul.wp - ul.pa) / ELEMENT);
	memcpy(ul_buffer[slot], data, length);
	ring_put(&ul, ctx_va + IPCR_UL * CTX + 36, ul_buffer_pa[slot], length, PKT_TRANSFER << 16 | TRE_IEOT);
	ul_pending++;
	doorbell(MHI_CHDB(IPCR_UL), ul.wp);
	lck_mtx_unlock(&ul_lock);
	return 0;
}

static void
event(unsigned r, const uint8_t *el)
{
	uint64_t pointer = get64(el);
	uint32_t d0, d1, type, code;

	memcpy(&d0, el + 8, 4);
	memcpy(&d1, el + 12, 4);
	type = (d1 >> 16) & 0xff;
	code = d0 >> 24;
	if (type == EV_CMD_COMPLETION) {
		commands_done += code == 1;
		printf("pcie: step 8: command at %#llx completed with code %u\n", pointer, code);
	} else if (type == EV_TRANSFER && (d1 >> 24) == IPCR_DL && pointer >= dl.pa &&
	    pointer < dl.pa + IPCR_ELEMENTS * ELEMENT) {
		uint8_t i = dl_at[(pointer - dl.pa) / ELEMENT];

		redwood_qrtr_input(dl_buffer[i], MIN(d0 & 0xffff, IPCR_BUFFER));
		dl_queue(i);
		doorbell(MHI_CHDB(IPCR_DL), dl.wp);
	} else if (type == EV_TRANSFER && (d1 >> 24) == IPCR_UL) {
		lck_mtx_lock(&ul_lock);
		ul_pending -= ul_pending != 0;
		lck_mtx_unlock(&ul_lock);
	} else {
		printf("pcie: step 8: at %llu ms, event ring %u: type %#x code %u pointer %#llx (%#x %#x)\n", uptime_ms(), r, type,
		    code, pointer, d0, d1);
	}
	if (type == EV_STATE_CHANGE && code == MHI_SYS_ERR) {
		uint32_t bhi = rd(bar + 0x28);

		/* the firmware's own account of its fault, as step 5 reads the BHI block (Linux drivers/bus/mhi: BHI_ERRCODE..) */
		printf("pcie: step 8: SYS_ERR: MHI status %#x, execution environment %u; BHI ERRCODE %#x ERRDBG1 %#x ERRDBG2 %#x "
		    "ERRDBG3 %#x\n", rd(bar + 0x48), rd(bar + bhi + 0x28), rd(bar + bhi + 0x30), rd(bar + bhi + 0x34),
		    rd(bar + bhi + 0x38), rd(bar + bhi + 0x3c));
		rddm_collect();
	}
}

/* what the card posted since the last look, handled, and the ring given back */
static void
events(unsigned r)
{
	uint8_t *c = ctx_va + CHANNELS * CTX + r * CTX;
	uint64_t device = get64(c + 28), end = ev_pa[r] + ev_len[r];
	uint8_t *ring = ctx_va + (ev_pa[r] - ctx_pa);
	unsigned moved = 0;

	if (device < ev_pa[r] || device >= end) {
		return;
	}
	while (ev_local[r] != device && moved < ev_len[r] / ELEMENT) {
		event(r, ring + (ev_local[r] - ev_pa[r]));
		ev_local[r] += ELEMENT;
		if (ev_local[r] == end) {
			ev_local[r] = ev_pa[r];
		}
		moved++;
	}
	if (moved != 0) {
		uint64_t wp = ev_local[r] == ev_pa[r] ? end - ELEMENT : ev_local[r] - ELEMENT;

		put64(c + 36, wp);
		doorbell(MHI_ERDB(r), wp);
	}
}

static bool
channel(struct ring *r, uint32_t number, uint32_t type)
{
	uint8_t *c = ctx_va + number * CTX;
	uint32_t chcfg = 1u | 2u << 8, erindex = 1, before = commands_done;
	uint64_t start;

	r->elements = IPCR_ELEMENTS;
	r->va = dma_alloc(IPCR_ELEMENTS * ELEMENT, &r->pa);
	if (r->va == NULL) {
		return false;
	}
	r->wp = r->pa;
	memcpy(c, &chcfg, 4);                           /* enabled, doorbell burst mode off */
	memcpy(c + 4, &type, 4);                        /* 1 to the card, 2 from it */
	memcpy(c + 8, &erindex, 4);
	put64(c + 12, r->pa);
	put64(c + 20, IPCR_ELEMENTS * ELEMENT);
	put64(c + 28, r->pa);
	put64(c + 36, r->pa);
	ring_put(&cmd, ctx_va + CHANNELS * CTX + EVENT_RINGS * CTX + 36, 0, 0, number << 24 | PKT_START_CHAN << 16);
	doorbell(MHI_CRDB, cmd.wp);
	for (start = uptime_ms(); commands_done == before && uptime_ms() - start < 2000;) {
		events(0);
		ms(5);
	}
	printf("pcie: step 8: channel %u: %s, context state %#x\n", number, commands_done != before ? "started" : "no answer",
	    rd((vm_offset_t)c) & 0xff);
	return commands_done != before;
}

/*
 * The host's vote that keeps the card awake (Linux drivers/bus/mhi: the doorbell of channel 127, MHI_DEV_WAKE_DB):
 * ath12k holds it around every access to the card's registers past the first 4 KiB, and here it is held from mission
 * mode on, for the driver (pcie=10), which touches those registers without asking.
 */
void
mhi_pcie_wake(void)
{
	doorbell(MHI_CHDB(127), 1);
	printf("pcie: the card is asked to stay awake (the wake doorbell)\n");
}

/* the link's thread: both event rings, every 5 ms, for as long as the phone runs */
static void
ipcr_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		events(0);
		events(1);
		redwood_qrtr_fresh();
		ms(5);
	}
}

bool
mhi_pcie_ipcr(void)
{
	thread_t thread;

	cmd.va = ctx_va + (cmd_pa - ctx_pa);
	cmd.pa = cmd_pa;
	cmd.elements = CMD_ELEMENTS;
	cmd.wp = get64(ctx_va + CHANNELS * CTX + EVENT_RINGS * CTX + 36);
	for (unsigned r = 0; r < EVENT_RINGS; r++) {
		uint64_t device = get64(ctx_va + CHANNELS * CTX + r * CTX + 28);

		ev_local[r] = device;
		put64(ctx_va + CHANNELS * CTX + r * CTX + 36, device == ev_pa[r] ? ev_pa[r] + ev_len[r] - ELEMENT :
		    device - ELEMENT);
		doorbell(MHI_ERDB(r), get64(ctx_va + CHANNELS * CTX + r * CTX + 36));
	}
	printf("pcie: step 8: MHI state %u, execution environment %u; event rings at %#llx %#llx, command ring %#llx\n",
	    (rd(bar + 0x48) >> 8) & 0xff, rd(bar + rd(bar + 0x28) + 0x28), ev_local[0], ev_local[1], cmd.wp);
	for (unsigned i = 0; i < IPCR_ELEMENTS; i++) {
		dl_buffer[i] = dma_alloc(IPCR_BUFFER, &dl_buffer_pa[i]);
		ul_buffer[i] = dma_alloc(IPCR_BUFFER, &ul_buffer_pa[i]);
		if (dl_buffer[i] == NULL || ul_buffer[i] == NULL) {
			printf("pcie: step 8: no DMA memory for the IPCR buffers\n");
			return false;
		}
	}
	if (!channel(&ul, IPCR_UL, 1) || !channel(&dl, IPCR_DL, 2)) {
		printf("pcie: step 8: the IPCR channels did not start\n");
		return false;
	}
	redwood_qrtr_attach(ipcr_send);
	redwood_qmi_set_transport(&redwood_qrtr);
	for (uint8_t i = 0; i < IPCR_ELEMENTS - 1; i++) {
		dl_queue(i);
	}
	doorbell(MHI_CHDB(IPCR_DL), dl.wp);
	if (kernel_thread_start(ipcr_thread, NULL, &thread) != KERN_SUCCESS) {
		return false;
	}
	thread_deallocate(thread);
	return true;
}

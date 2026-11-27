#ifndef _ANNIBALE_ADSP_FASTRPC_SMMU_INC_
#define _ANNIBALE_ADSP_FASTRPC_SMMU_INC_

/*
 * apps_smmu@15000000 Stage-1 bypass configuration for LPASS ADSP FastRPC compute-cb@2.
 *
 * Device tree contract (sm8750-xiaomi-annibale.dts):
 *   apps_smmu@15000000: qcom,handoff-smrs = <0x800 0x02>;
 *   compute-cb@2: pd-type = <3> (SENSORS_PD), reg = <4>,
 *                 iommus = <&lpass_qtb 0x1004 0x80 &lpass_qtb 0x1044 0x20>;
 *
 * qsmmu-v500 forbids S2CR.TYPE_BYPASS; Stage-1 bypass uses TYPE_TRANS (0) pointing
 * to a free context bank with CBAR_S1_BYPASS (0x10000), CBA2R_VA64 (1), and SCTLR.M=0.
 */

#define ADSP_APPS_SMMU_PHYS          0x15000000UL
#define ADSP_APPS_SMMU_SIZE          0x00100000UL

#define ADSP_SMMU_IDR0               0x0020u
#define ADSP_SMMU_IDR1               0x0024u
#define ADSP_SMMU_SMR(n)             (0x0800u + 4u * (n))
#define ADSP_SMMU_S2CR(n)            (0x0c00u + 4u * (n))
#define ADSP_SMMU_CBAR(pg_sz, b)     ((pg_sz) + 4u * (b))
#define ADSP_SMMU_CBA2R(pg_sz, b)    ((pg_sz) + 0x0800u + 4u * (b))
#define ADSP_SMMU_CB_SCTLR(pages, pg_sz, b) (((pages) + (b)) * (pg_sz))

#define ADSP_SMMU_SMR_VALID          (1u << 31)
#define ADSP_SMMU_SMR_ID_MASK        0x7fffu
#define ADSP_SMMU_S2CR_TYPE_MASK     3u
#define ADSP_SMMU_S2CR_TYPE_SHIFT    16u
#define ADSP_SMMU_S2CR_TYPE_TRANS    0u
#define ADSP_SMMU_CBAR_S1_BYPASS     0x10000u
#define ADSP_SMMU_CBA2R_VA64         1u

#define ADSP_HANDOFF_SMR_SID         0x0800u
#define ADSP_HANDOFF_SMR_MASK        0x0002u

#define ADSP_FASTRPC_COMPUTE_SID     4u
#define ADSP_FASTRPC_STREAM_COUNT    2u

struct adsp_fastrpc_stream_spec {
	uint16_t sid;
	uint16_t mask;
};

static const struct adsp_fastrpc_stream_spec adsp_fastrpc_streams[ADSP_FASTRPC_STREAM_COUNT] = {
	{ 0x1004u, 0x0080u },
	{ 0x1044u, 0x0020u },
};

static bool adsp_fastrpc_smmu_ready;
static bool adsp_fastrpc_smmu_bank_owned;
static uint32_t adsp_fastrpc_smmu_bank;
static uint32_t adsp_fastrpc_smmu_pg_sz;
static uint32_t adsp_fastrpc_smmu_pages;
static int32_t adsp_fastrpc_smmu_slots[ADSP_FASTRPC_STREAM_COUNT];
static bool adsp_fastrpc_smmu_slot_owned[ADSP_FASTRPC_STREAM_COUNT];

#ifndef ADSP_FASTRPC_HOST_TEST
static vm_offset_t adsp_fastrpc_smmu_vbase;

static uint32_t
adsp_smmu_rd(uint32_t off)
{
	uint32_t val;

	/* QHEE/Gunyah Stage-2 MMIO traps fault on post-index/writeback (ESR_EL2.ISV=0). */
	__asm__ volatile("ldr %w0, [%1]" : "=r"(val) : "r"(adsp_fastrpc_smmu_vbase + off) : "memory");
	return val;
}

static void
adsp_smmu_wr(uint32_t off, uint32_t val)
{
	__asm__ volatile("str %w0, [%1]" : : "r"(val), "r"(adsp_fastrpc_smmu_vbase + off) : "memory");
	__builtin_arm_dsb(0xf);
}

static bool
adsp_smmu_map(void)
{
	if (adsp_fastrpc_smmu_vbase == 0) {
		adsp_fastrpc_smmu_vbase = ml_io_map(ADSP_APPS_SMMU_PHYS, ADSP_APPS_SMMU_SIZE);
	}
	return adsp_fastrpc_smmu_vbase != 0;
}
#endif

static bool
adsp_smmu_streams_overlap(uint16_t sid_a, uint16_t mask_a, uint16_t sid_b, uint16_t mask_b)
{
	return ((sid_a ^ sid_b) & ADSP_SMMU_SMR_ID_MASK & ~(mask_a | mask_b)) == 0;
}

static bool
adsp_fastrpc_smmu_configure(void)
{
	if (adsp_fastrpc_smmu_ready) {
		return true;
	}
	if (!adsp_smmu_map()) {
		say("fastrpc: failed to map apps_smmu@%#lx\n", ADSP_APPS_SMMU_PHYS);
		return false;
	}

	uint32_t idr0 = adsp_smmu_rd(ADSP_SMMU_IDR0);
	uint32_t idr1 = adsp_smmu_rd(ADSP_SMMU_IDR1);
	uint32_t smr_count = idr0 & 0xffu;
	if (smr_count > 128u) {
		smr_count = 128u;
	}
	uint32_t banks = idr1 & 0xffu;
	if (banks > 128u) {
		banks = 128u;
	}
	uint32_t first_s1 = (idr1 >> 16) & 0xffu;
	uint32_t pages = 1u << (((idr1 >> 28) & 7u) + 1u);
	uint32_t pg_sz = (idr1 & (1u << 31)) ? 0x10000u : 0x1000u;
	if (smr_count == 0 || banks == 0 || first_s1 >= banks ||
	    (uint64_t)(pages + banks) * pg_sz > ADSP_APPS_SMMU_SIZE) {
		say("fastrpc: invalid apps_smmu geometry smr=%u banks=%u first_s1=%u pages=%u pg_sz=%#x\n",
		    smr_count, banks, first_s1, pages, pg_sz);
		return false;
	}

	bool bank_used[128];
	memset(bank_used, 0, sizeof(bank_used));

	for (uint32_t b = 0; b < banks; b++) {
		if (adsp_smmu_rd(ADSP_SMMU_CBAR(pg_sz, b)) != 0 ||
		    adsp_smmu_rd(ADSP_SMMU_CBA2R(pg_sz, b)) != 0) {
			bank_used[b] = true;
		}
	}

	for (uint32_t i = 0; i < smr_count; i++) {
		uint32_t smr = adsp_smmu_rd(ADSP_SMMU_SMR(i));
		if ((smr & ADSP_SMMU_SMR_VALID) == 0) {
			continue;
		}
		uint32_t s2cr = adsp_smmu_rd(ADSP_SMMU_S2CR(i));
		uint32_t stype = (s2cr >> ADSP_SMMU_S2CR_TYPE_SHIFT) & ADSP_SMMU_S2CR_TYPE_MASK;
		uint32_t cbndx = s2cr & 0xffu;
		if (stype == ADSP_SMMU_S2CR_TYPE_TRANS && cbndx < banks) {
			bank_used[cbndx] = true;
		}
	}

	int32_t matched_slot[ADSP_FASTRPC_STREAM_COUNT];
	uint32_t matched_bank[ADSP_FASTRPC_STREAM_COUNT];
	bool all_reused = true;

	for (uint32_t k = 0; k < ADSP_FASTRPC_STREAM_COUNT; k++) {
		uint16_t want_sid = adsp_fastrpc_streams[k].sid;
		uint16_t want_mask = adsp_fastrpc_streams[k].mask;
		matched_slot[k] = -1;
		matched_bank[k] = 0;
		if (adsp_smmu_streams_overlap(want_sid, want_mask, ADSP_HANDOFF_SMR_SID, ADSP_HANDOFF_SMR_MASK)) {
			say("fastrpc: stream %#x/%#x overlaps handoff-smrs\n", want_sid, want_mask);
			return false;
		}
		for (uint32_t i = 0; i < smr_count; i++) {
			uint32_t smr = adsp_smmu_rd(ADSP_SMMU_SMR(i));
			if ((smr & ADSP_SMMU_SMR_VALID) == 0) {
				continue;
			}
			uint16_t cur_sid = (uint16_t)(smr & ADSP_SMMU_SMR_ID_MASK);
			uint16_t cur_mask = (uint16_t)((smr >> 16) & ADSP_SMMU_SMR_ID_MASK);
			if (!adsp_smmu_streams_overlap(want_sid, want_mask, cur_sid, cur_mask)) {
				continue;
			}
			uint32_t s2cr = adsp_smmu_rd(ADSP_SMMU_S2CR(i));
			uint32_t stype = (s2cr >> ADSP_SMMU_S2CR_TYPE_SHIFT) & ADSP_SMMU_S2CR_TYPE_MASK;
			uint32_t cbndx = s2cr & 0xffu;
			if (cur_sid == want_sid && cur_mask == want_mask &&
			    stype == ADSP_SMMU_S2CR_TYPE_TRANS && cbndx < banks &&
			    (adsp_smmu_rd(ADSP_SMMU_CBAR(pg_sz, cbndx)) & ~0xffu) == ADSP_SMMU_CBAR_S1_BYPASS &&
			    adsp_smmu_rd(ADSP_SMMU_CBA2R(pg_sz, cbndx)) == ADSP_SMMU_CBA2R_VA64 &&
			    (adsp_smmu_rd(ADSP_SMMU_CB_SCTLR(pages, pg_sz, cbndx)) & 1u) == 0) {
				matched_slot[k] = (int32_t)i;
				matched_bank[k] = cbndx;
				break;
			}
			say("fastrpc: SMMU conflict slot %u smr=%#x s2cr=%#x for %#x/%#x\n",
			    i, smr, s2cr, want_sid, want_mask);
			return false;
		}
		if (matched_slot[k] < 0) {
			all_reused = false;
		}
	}

	uint32_t chosen_bank = banks;
	if (!all_reused) {
		for (uint32_t b = banks; b > first_s1; b--) {
			if (!bank_used[b - 1u]) {
				chosen_bank = b - 1u;
				break;
			}
		}
		if (chosen_bank >= banks) {
			say("fastrpc: no free apps_smmu context bank among [%u,%u)\n", first_s1, banks);
			return false;
		}
	}

	int32_t free_slots[ADSP_FASTRPC_STREAM_COUNT];
	for (uint32_t k = 0; k < ADSP_FASTRPC_STREAM_COUNT; k++) {
		free_slots[k] = matched_slot[k];
		if (free_slots[k] >= 0) {
			continue;
		}
		for (uint32_t i = 0; i < smr_count; i++) {
			if ((adsp_smmu_rd(ADSP_SMMU_SMR(i)) & ADSP_SMMU_SMR_VALID) != 0) {
				continue;
			}
			bool taken = false;
			for (uint32_t prev = 0; prev < k; prev++) {
				if (free_slots[prev] == (int32_t)i) {
					taken = true;
					break;
				}
			}
			if (!taken) {
				free_slots[k] = (int32_t)i;
				break;
			}
		}
		if (free_slots[k] < 0) {
			say("fastrpc: no free apps_smmu SMR slot for sid %#x\n", adsp_fastrpc_streams[k].sid);
			return false;
		}
	}

	if (!all_reused) {
		/* CBAR assigns the hypervisor S1 VMID before any context-bank page access. */
		adsp_smmu_wr(ADSP_SMMU_CBA2R(pg_sz, chosen_bank), ADSP_SMMU_CBA2R_VA64);
		adsp_smmu_wr(ADSP_SMMU_CBAR(pg_sz, chosen_bank), ADSP_SMMU_CBAR_S1_BYPASS);
		adsp_smmu_wr(ADSP_SMMU_CB_SCTLR(pages, pg_sz, chosen_bank), 0u);
		adsp_fastrpc_smmu_bank = chosen_bank;
		adsp_fastrpc_smmu_bank_owned = true;
	} else {
		adsp_fastrpc_smmu_bank = matched_bank[0];
		adsp_fastrpc_smmu_bank_owned = false;
	}
	adsp_fastrpc_smmu_pg_sz = pg_sz;
	adsp_fastrpc_smmu_pages = pages;

	for (uint32_t k = 0; k < ADSP_FASTRPC_STREAM_COUNT; k++) {
		uint32_t slot = (uint32_t)free_slots[k];
		adsp_fastrpc_smmu_slots[k] = free_slots[k];
		if (matched_slot[k] < 0) {
			uint32_t use_bank = chosen_bank;
			uint32_t smr_val = ADSP_SMMU_SMR_VALID |
			    (((uint32_t)adsp_fastrpc_streams[k].mask & ADSP_SMMU_SMR_ID_MASK) << 16) |
			    ((uint32_t)adsp_fastrpc_streams[k].sid & ADSP_SMMU_SMR_ID_MASK);
			adsp_smmu_wr(ADSP_SMMU_S2CR(slot), use_bank);
			adsp_smmu_wr(ADSP_SMMU_SMR(slot), smr_val);
			adsp_fastrpc_smmu_slot_owned[k] = true;
		} else {
			adsp_fastrpc_smmu_slot_owned[k] = false;
		}
	}

	adsp_fastrpc_smmu_ready = true;
	say("fastrpc: apps_smmu compute-cb@2 bank=%u slots=(%d,%d) sids=(0x1004/0x80,0x1044/0x20)\n",
	    adsp_fastrpc_smmu_bank, adsp_fastrpc_smmu_slots[0], adsp_fastrpc_smmu_slots[1]);
	return true;
}

static void
adsp_fastrpc_smmu_teardown(void)
{
	if (!adsp_fastrpc_smmu_ready) {
		return;
	}
	for (uint32_t k = 0; k < ADSP_FASTRPC_STREAM_COUNT; k++) {
		if (adsp_fastrpc_smmu_slot_owned[k] && adsp_fastrpc_smmu_slots[k] >= 0) {
			uint32_t slot = (uint32_t)adsp_fastrpc_smmu_slots[k];
			adsp_smmu_wr(ADSP_SMMU_SMR(slot), 0u);
			adsp_smmu_wr(ADSP_SMMU_S2CR(slot), 0u);
		}
		adsp_fastrpc_smmu_slots[k] = -1;
		adsp_fastrpc_smmu_slot_owned[k] = false;
	}
	if (adsp_fastrpc_smmu_bank_owned && adsp_fastrpc_smmu_pg_sz != 0) {
		uint32_t pg_sz = adsp_fastrpc_smmu_pg_sz;
		uint32_t pages = adsp_fastrpc_smmu_pages;
		adsp_smmu_wr(ADSP_SMMU_CB_SCTLR(pages, pg_sz, adsp_fastrpc_smmu_bank), 0u);
		adsp_smmu_wr(ADSP_SMMU_CBA2R(pg_sz, adsp_fastrpc_smmu_bank), 0u);
		adsp_smmu_wr(ADSP_SMMU_CBAR(pg_sz, adsp_fastrpc_smmu_bank), 0u);
	}
	adsp_fastrpc_smmu_bank_owned = false;
	adsp_fastrpc_smmu_bank = 0;
	adsp_fastrpc_smmu_ready = false;
}

#endif /* _ANNIBALE_ADSP_FASTRPC_SMMU_INC_ */

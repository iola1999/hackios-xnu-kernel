/*
 * qsmmu-v500 forbids S2CR.TYPE_BYPASS. Its S1 bypass domain uses TYPE_TRANS,
 * CBAR S1_TRANS_S2_BYPASS and SCTLR.M=0. VMID assignment belongs to the
 * hypervisor; CBA2R/CBAR must be initialized before publishing the SID.
 */
#define CAM_SMMU_CR0                    0x000u
#define CAM_SMMU_IDR0                   0x020u
#define CAM_SMMU_IDR1                   0x024u
#define CAM_SMMU_IDR2                   0x028u
#define CAM_SMMU_SMR(n)                 (0x800u + 4u * (n))
#define CAM_SMMU_S2CR(n)                (0xc00u + 4u * (n))
#define CAM_SMMU_GR1_CBAR(n)            (0x000u + 4u * (n))
#define CAM_SMMU_GR1_CBA2R(n)           (0x800u + 4u * (n))
#define CAM_SMMU_SMR_VALID              (1u << 31)
#define CAM_SMMU_SID_IFE                0x1c00u
#define CAM_SMMU_TLBGSYNC               0x070u
#define CAM_SMMU_TLBGSTATUS             0x074u
#define CAM_SMMU_S2CR_TYPE              (3u << 16)
#define CAM_SMMU_FAULT                  (2u << 16)
#define CAM_SMMU_NSH                    (3u << 8)
#define CAM_SMMU_CBAR_S1                0x1f300u
#define CAM_SMMU_SCTLR                  0x000u
#define CAM_SMMU_ACTLR                  0x004u
#define CAM_SMMU_TCR2                   0x010u
#define CAM_SMMU_TCR2_AS                (1u << 4)
#define CAM_SMMU_TCR2_TBI0              (1u << 5)
#define CAM_SMMU_TCR2_TBI1              (1u << 6)
#define CAM_SMMU_TTBR0                  0x020u
#define CAM_SMMU_TTBR1                  0x028u
#define CAM_SMMU_TCR                    0x030u
#define CAM_SMMU_MAIR0                  0x038u
#define CAM_SMMU_MAIR1                  0x03cu
#define CAM_SMMU_FSR                    0x058u
#define CAM_SMMU_FSR_FAULT              0xc00001feu
#define CAM_SMMU_FAR                    0x060u
#define CAM_SMMU_TLBIASID               0x610u
#define CAM_SMMU_TLBSYNC                0x7f0u
#define CAM_SMMU_TLBSTATUS              0x7f4u
#define CAM_SMMU_NO_WALKS               ((1u << 23) | (1u << 7))
/* ife is dma-coherent and stall-disable in the device tree; M stays clear.
 * ASIDPNE=0 uses the system ASID namespace; local TLBI remains effective with M=0. */
#define CAM_SMMU_S1_BYPASS_SCTLR        ((2u << 26) | (2u << 24) | (1u << 22) | \
                                       (1u << 20) | (15u << 16) | \
                                       (1u << 6) | (1u << 5) | (1u << 2) | (1u << 1))

/* AS selects 16-bit ASIDs. TBI0/TBI1 affect full 64-bit ATS inputs;
 * this M=0 domain uses untagged 36-bit DMA and no ATS operations. */
#define CAM_SMMU_S1_BYPASS_TCR2         (CAM_SMMU_TCR2_AS | CAM_SMMU_TCR2_TBI0 | \
                                       CAM_SMMU_TCR2_TBI1)

#ifndef CAM_NATIVE_TRACE
#define CAM_NATIVE_TRACE 0
#endif
#if CAM_NATIVE_TRACE
#define cam_native_trace(...) say("native " __VA_ARGS__)
#else
#define cam_native_trace(...) ((void)0)
#endif

static vm_offset_t cam_smmu_base;
static uint8_t cam_smmu_slot = 0xffu, cam_smmu_bank = 0xffu;
static uint32_t cam_smmu_page_size, cam_smmu_cb_offset, cam_smmu_stream_count;
static uint32_t cam_smmu_cbar, cam_smmu_cba2r;
static bool cam_smmu_ready, cam_smmu_sync_failed;

static bool
cam_smmu_sync(uint32_t cb)
{
	if (cam_smmu_base == 0 || cam_smmu_sync_failed) {
		return false;
	}
	uint32_t sync = cb == 0 ? CAM_SMMU_TLBGSYNC : cb + CAM_SMMU_TLBSYNC;
	uint32_t status_offset = cb == 0 ? CAM_SMMU_TLBGSTATUS : cb + CAM_SMMU_TLBSTATUS;
	reg_wr(cam_smmu_base, sync, 0);
	for (uint32_t i = 0; i < 1000; i++) {
		uint32_t status = reg_rd(cam_smmu_base, status_offset);
		if (status != UINT32_MAX && (status & 1u) == 0) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	/* Another sync after timeout can fill the shared TBU command queue. */
	cam_smmu_sync_failed = true;
	cam_native_trace("smmu sync failed cb=%#x\n", cb);
	return false;
}

static bool
cam_smmu_invalidate(void)
{
	if (cam_smmu_bank == 0xffu || cam_smmu_sync_failed) {
		return false;
	}
	reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_TLBIASID, cam_smmu_bank);
	return cam_smmu_sync(cam_smmu_cb_offset);
}

/* Invalid SMRs can still carry a CB reference; exclude every TYPE_TRANS S2CR. */
static bool
cam_smmu_scan(uint32_t *free_slot, bool referenced[128])
{
	bzero(referenced, 128u * sizeof(bool));
	*free_slot = UINT32_MAX;
	for (uint32_t i = 0; i < cam_smmu_stream_count; i++) {
		uint32_t smr = reg_rd(cam_smmu_base, CAM_SMMU_SMR(i));
		uint32_t s2cr = reg_rd(cam_smmu_base, CAM_SMMU_S2CR(i));
		if (smr == UINT32_MAX || s2cr == UINT32_MAX) {
			return false;
		}
		if (i == cam_smmu_slot) {
			continue;
		}
		if ((s2cr & CAM_SMMU_S2CR_TYPE) == 0) {
			uint32_t bank = s2cr & 0xffu;
			if (bank >= 128u) {
				return false;
			}
			referenced[bank] = true;
		}
		if ((smr & CAM_SMMU_SMR_VALID) == 0) {
			if (*free_slot == UINT32_MAX) {
				*free_slot = i;
			}
			continue;
		}
		uint32_t mask = (smr >> 16) & 0x7fffu;
		if (((smr ^ CAM_SMMU_SID_IFE) & 0xffffu & ~mask) == 0) {
			return false;
		}
	}
	return true;
}

/* Context pages are accessible only after CBAR has assigned an S1 VMID. */
static bool
cam_smmu_bank_free(uint32_t bank)
{
	return reg_rd(cam_smmu_base, cam_smmu_page_size + CAM_SMMU_GR1_CBAR(bank)) == 0 &&
	       reg_rd(cam_smmu_base, cam_smmu_page_size + CAM_SMMU_GR1_CBA2R(bank)) == 0;
}

static bool
cam_smmu_bank_valid(uint32_t sctlr)
{
	if (cam_smmu_bank == 0xffu || (cam_smmu_cbar & ~0xffu) != CAM_SMMU_CBAR_S1 ||
	    (cam_smmu_cba2r & 0xffffu) != 1u ||
	    reg_rd(cam_smmu_base, cam_smmu_page_size + CAM_SMMU_GR1_CBAR(cam_smmu_bank)) != cam_smmu_cbar ||
	    reg_rd(cam_smmu_base, cam_smmu_page_size + CAM_SMMU_GR1_CBA2R(cam_smmu_bank)) != cam_smmu_cba2r) {
		return false;
	}
	const uint32_t expected[][2] = {
		{ CAM_SMMU_SCTLR, sctlr }, { CAM_SMMU_ACTLR, 1u },
		{ CAM_SMMU_TCR2, CAM_SMMU_S1_BYPASS_TCR2 }, { CAM_SMMU_TCR, CAM_SMMU_NO_WALKS },
		{ CAM_SMMU_TTBR0, 0 }, { CAM_SMMU_TTBR0 + 4u, (uint32_t)cam_smmu_bank << 16 },
		{ CAM_SMMU_TTBR1, 0 }, { CAM_SMMU_TTBR1 + 4u, (uint32_t)cam_smmu_bank << 16 },
		{ CAM_SMMU_MAIR0, 0 }, { CAM_SMMU_MAIR1, 0 },
	};
	for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
		uint32_t value = reg_rd(cam_smmu_base, cam_smmu_cb_offset + expected[i][0]);
		if (value != expected[i][1]) {
			cam_native_trace("smmu bank=%u reg=%#x value=%#x expected=%#x\n",
			    cam_smmu_bank, expected[i][0], value, expected[i][1]);
			return false;
		}
	}
	return true;
}

static bool
cam_smmu_route(bool allow)
{
	cam_smmu_ready = false;
	if (cam_smmu_slot == 0xffu || (allow && cam_smmu_sync_failed) ||
	    reg_rd(cam_smmu_base, CAM_SMMU_SMR(cam_smmu_slot)) !=
	    (CAM_SMMU_SMR_VALID | CAM_SMMU_SID_IFE) ||
	    (allow && !cam_smmu_bank_valid(CAM_SMMU_S1_BYPASS_SCTLR))) {
		return false;
	}
	uint32_t value = CAM_SMMU_NSH | (allow ? (uint32_t)cam_smmu_bank : CAM_SMMU_FAULT);
	reg_wr(cam_smmu_base, CAM_SMMU_S2CR(cam_smmu_slot), value);
	uint32_t readback = reg_rd(cam_smmu_base, CAM_SMMU_S2CR(cam_smmu_slot));
	bool done = readback == value && cam_smmu_sync(0);
	if (done && !allow) {
		done = cam_smmu_bank_valid(CAM_SMMU_S1_BYPASS_SCTLR);
		if (done) {
			/* Fault records stay latched after stop; return the capture error separately. */
			uint32_t fsr = reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FSR);
			if ((fsr & CAM_SMMU_FSR_FAULT) != 0) {
				if (cam_capture.error == 0) {
					cam_capture.error = EIO;
				}
				cam_native_trace("smmu capture fault bank=%u fsr=%#x far=%#x:%#x\n",
				    cam_smmu_bank, fsr,
				    reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FAR + 4u),
				    reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FAR));
			}
		}
	}
	cam_smmu_ready = allow && done;
	cam_native_trace("smmu route=%u slot=%u bank=%u s2cr=%#x read=%#x complete=%u\n",
	    allow ? 1u : 0u, cam_smmu_slot, cam_smmu_bank, value, readback, done ? 1u : 0u);
	return done;
}

/* The caller has recovered its sole WM buffer before revoking this stream. */
static bool
cam_smmu_release(void)
{
	if (cam_smmu_bank == 0xffu && cam_smmu_slot == 0xffu) {
		return true;
	}
	if (cam_smmu_slot != 0xffu && !cam_smmu_route(false)) {
		return false;
	}
	bool referenced[128];
	uint32_t unused_slot;
	if (cam_smmu_bank == 0xffu || !cam_smmu_scan(&unused_slot, referenced) ||
	    referenced[cam_smmu_bank] ||
	    (!cam_smmu_bank_valid(CAM_SMMU_S1_BYPASS_SCTLR) && !cam_smmu_bank_valid(0))) {
		return false;
	}
	if (cam_smmu_slot != 0xffu) {
		/* Native free_sme writes a blocked S2CR before removing SMR validity. */
		reg_wr(cam_smmu_base, CAM_SMMU_SMR(cam_smmu_slot), 0);
		if (reg_rd(cam_smmu_base, CAM_SMMU_SMR(cam_smmu_slot)) != 0 || !cam_smmu_sync(0)) {
			return false;
		}
	}
	reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_SCTLR, 0);
	if (!cam_smmu_bank_valid(0) || !cam_smmu_invalidate()) {
		return false;
	}
	/* Keep this disabled domain assigned to camera across close/open. */
	cam_smmu_slot = 0xffu;
	cam_smmu_ready = false;
	return true;
}

static bool
cam_smmu_configure_bypass(void)
{
	if (cam_smmu_slot != 0xffu) {
		return cam_smmu_route(true);
	}
	cam_smmu_ready = false;
	if (cam_smmu_sync_failed) {
		return false;
	}
	if (cam_smmu_base == 0) {
		cam_smmu_base = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
	}
	if (cam_smmu_base == 0) {
		return false;
	}
	uint32_t idr0 = reg_rd(cam_smmu_base, CAM_SMMU_IDR0);
	uint32_t idr1 = reg_rd(cam_smmu_base, CAM_SMMU_IDR1);
	uint32_t idr2 = reg_rd(cam_smmu_base, CAM_SMMU_IDR2);
	uint32_t cr0 = reg_rd(cam_smmu_base, CAM_SMMU_CR0);
	uint32_t streams = idr0 & 0xffu, banks = idr1 & 0xffu;
	uint32_t first_s1 = (idr1 >> 16) & 0xffu;
	uint32_t pg_size = (idr1 & (1u << 31)) ? 0x10000u : 0x1000u;
	uint32_t pages = 1u << (((idr1 >> 28) & 7u) + 1u);
	cam_native_trace("smmu id0=%#x id1=%#x id2=%#x cr0=%#x\n", idr0, idr1, idr2, cr0);
	if (idr0 == UINT32_MAX || idr1 == UINT32_MAX || idr2 == UINT32_MAX || cr0 == UINT32_MAX ||
	    (idr0 & ((1u << 30) | (1u << 27))) != ((1u << 30) | (1u << 27)) ||
	    (idr2 & (1u << 12)) == 0 || (cr0 & (1u << 3)) != 0 || streams == 0 ||
	    banks < 2u || banks > 128u || first_s1 >= banks - 1u ||
	    (uint64_t)(pages + banks) * pg_size > APPS_SMMU_SIZE) {
		return false;
	}
	if (cam_smmu_bank != 0xffu && (cam_smmu_page_size != pg_size ||
	    cam_smmu_cb_offset != (pages + cam_smmu_bank) * pg_size ||
	    cam_smmu_bank < first_s1 || cam_smmu_bank >= banks - 1u)) {
		return false;
	}
	cam_smmu_page_size = pg_size;
	cam_smmu_stream_count = streams;
	bool referenced[128];
	uint32_t slot;
	if (!cam_smmu_scan(&slot, referenced) || slot == UINT32_MAX) {
		return false;
	}
	uint32_t old_smr = reg_rd(cam_smmu_base, CAM_SMMU_SMR(slot));
	uint32_t old_s2cr = reg_rd(cam_smmu_base, CAM_SMMU_S2CR(slot));
	bool initialize = cam_smmu_bank == 0xffu;
	if (initialize) {
		/* LPASS initializes the last CB before camera registration. */
		for (uint32_t bank = first_s1; bank < banks - 1u; bank++) {
			uint32_t cb = (pages + bank) * pg_size;
			if (!referenced[bank] && cam_smmu_bank_free(bank)) {
				cam_smmu_bank = (uint8_t)bank;
				cam_smmu_cb_offset = cb;
				break;
			}
		}
		if (cam_smmu_bank == 0xffu) {
			return false;
		}
	} else if (referenced[cam_smmu_bank] || !cam_smmu_bank_valid(0)) {
		return false;
	}
	cam_native_trace("smmu selected slot=%u old_smr=%#x old_s2cr=%#x bank=%u cb=%#x initialize=%u\n",
	    slot, old_smr, old_s2cr, cam_smmu_bank, cam_smmu_cb_offset, initialize ? 1u : 0u);
	uint32_t gr1 = cam_smmu_page_size;
	/* Every SID acquire touches CBAR, including reuse; the hypervisor supplies VMID. */
	reg_wr(cam_smmu_base, gr1 + CAM_SMMU_GR1_CBA2R(cam_smmu_bank), 1u);
	if ((reg_rd(cam_smmu_base, gr1 + CAM_SMMU_GR1_CBA2R(cam_smmu_bank)) & 0xffffu) != 1u) {
		return false;
	}
	reg_wr(cam_smmu_base, gr1 + CAM_SMMU_GR1_CBAR(cam_smmu_bank), CAM_SMMU_CBAR_S1);
	cam_smmu_cbar = reg_rd(cam_smmu_base, gr1 + CAM_SMMU_GR1_CBAR(cam_smmu_bank));
	cam_smmu_cba2r = reg_rd(cam_smmu_base, gr1 + CAM_SMMU_GR1_CBA2R(cam_smmu_bank));
	cam_native_trace("smmu VMID bank=%u cbar=%#x cba2r=%#x\n", cam_smmu_bank, cam_smmu_cbar, cam_smmu_cba2r);
	if ((cam_smmu_cbar & ~0xffu) != CAM_SMMU_CBAR_S1 || (cam_smmu_cba2r & 0xffffu) != 1u) {
		return false;
	}
	uint32_t initial_sctlr = reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_SCTLR);
	cam_native_trace("smmu initial SCTLR bank=%u value=%#x\n", cam_smmu_bank, initial_sctlr);
	if (initial_sctlr == UINT32_MAX || (initial_sctlr & 1u) != 0) {
		return false;
	}
	/* M=0 permits initialization; other reset register contents need not be zero. */
	reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_SCTLR, 0);
	if (reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_SCTLR) != 0) {
		return false;
	}
	reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FSR, CAM_SMMU_FSR_FAULT);
	if (initialize) {
		reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_TCR2, CAM_SMMU_S1_BYPASS_TCR2);
		reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_TCR, CAM_SMMU_NO_WALKS);
		reg_wr64(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_TTBR0, (uint64_t)cam_smmu_bank << 48);
		reg_wr64(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_TTBR1, (uint64_t)cam_smmu_bank << 48);
		reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_MAIR0, 0);
		reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_MAIR1, 0);
		/* The camera SID's qcom,actlr value is 1; flush after changing it. */
		reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_ACTLR, 1u);
	}
	reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_SCTLR, CAM_SMMU_S1_BYPASS_SCTLR);
	if (!cam_smmu_bank_valid(CAM_SMMU_S1_BYPASS_SCTLR) || !cam_smmu_invalidate() ||
	    reg_rd(cam_smmu_base, CAM_SMMU_SMR(slot)) != old_smr ||
	    reg_rd(cam_smmu_base, CAM_SMMU_S2CR(slot)) != old_s2cr) {
		return false;
	}
	cam_smmu_slot = (uint8_t)slot;
	reg_wr(cam_smmu_base, CAM_SMMU_S2CR(slot), CAM_SMMU_FAULT | CAM_SMMU_NSH);
	if (reg_rd(cam_smmu_base, CAM_SMMU_S2CR(slot)) != (CAM_SMMU_FAULT | CAM_SMMU_NSH)) {
		return false;
	}
	reg_wr(cam_smmu_base, CAM_SMMU_SMR(slot), CAM_SMMU_SMR_VALID | CAM_SMMU_SID_IFE);
	if (reg_rd(cam_smmu_base, CAM_SMMU_SMR(slot)) != (CAM_SMMU_SMR_VALID | CAM_SMMU_SID_IFE) ||
	    reg_rd(cam_smmu_base, CAM_SMMU_S2CR(slot)) != (CAM_SMMU_FAULT | CAM_SMMU_NSH) ||
	    !cam_smmu_sync(0)) {
		return false;
	}
	return cam_smmu_route(true);
}

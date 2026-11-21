/*
 * Single-round camera bus, QChannel, QoS, QTB, MMSS_NOC, SMMU, and CRM diagnostics
 * for SM8750 (annibale). Included from annibale_camera_hw.inc.
 */

extern int32_t adreno8_scm_call(uint64_t function, uint64_t arginfo, uint64_t argument0,
    uint64_t argument1, uint64_t argument2, uint64_t argument3, uint64_t *answer);

static uint64_t cam_qtb_mid_ot0, cam_qtb_mid_ot1;

static inline uint64_t
reg_rd64(vm_offset_t base, uint32_t offset)
{
	if (base == 0) {
		return UINT64_MAX;
	}
	uint64_t val = *(volatile uint64_t *)(base + offset);
	__asm__ volatile("dmb ishld" ::: "memory");
	return val;
}

static bool
cam_qchannel_active(uint32_t ctrl_off, uint32_t stat_off)
{
	if (cpas_base == 0) {
		return false;
	}
	if ((reg_rd(cpas_base, stat_off) & 1u) != 0) {
		return true;
	}
	reg_wr(cpas_base, ctrl_off, reg_rd(cpas_base, ctrl_off) | 1u);
	for (uint32_t i = 0; i < 500u; i++) {
		if ((reg_rd(cpas_base, stat_off) & 1u) != 0) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
}

/*
 * cam_cpastop_poweron on this target (enable-secure-qos-update): both QChannels, then TrustZone
 * writes the static CAMNOC QoS tables. qcom_scm_camera_update_camnoc_qos(0, 0, NULL) passes
 * (use case, buffer, bytes) as QCOM_SCM_ARGS(3, VAL, RW, VAL). Without it the RT1 port that
 * carries IFE-lite keeps priority 0 and its writer overflows once the GPU and CPU load the bus.
 */
static void
cam_cpas_qos_init(void)
{
	bool rt_ok = cam_qchannel_active(0xecu, 0xf0u);
	bool nrt_ok = cam_qchannel_active(0xf4u, 0xf8u);
	say("cpas qch rt_ok=%u(%#x/%#x) nrt_ok=%u(%#x/%#x)\n",
	    rt_ok ? 1u : 0u, reg_rd(cpas_base, 0xecu), reg_rd(cpas_base, 0xf0u),
	    nrt_ok ? 1u : 0u, reg_rd(cpas_base, 0xf4u), reg_rd(cpas_base, 0xf8u));

	if (camnoc_rt_base != 0) {
		say("camnoc rt1 qos pre  4830=%#x 4834=%#x 4838=%#x 4840=%#x 4848=%#x\n",
		    reg_rd(camnoc_rt_base, 0x4830u), reg_rd(camnoc_rt_base, 0x4834u),
		    reg_rd(camnoc_rt_base, 0x4838u), reg_rd(camnoc_rt_base, 0x4840u),
		    reg_rd(camnoc_rt_base, 0x4848u));
	}
	uint64_t ans = ~0ULL;
	int32_t st = adreno8_scm_call(0x4200180aULL, 0x83ULL, 0ULL, 0ULL, 0ULL, 0ULL, &ans);
	say("scm camnoc_qos(0) st=%d ans=%#llx\n", st, (unsigned long long)ans);
	if (camnoc_rt_base != 0) {
		say("camnoc rt1 qos post 4830=%#x 4834=%#x 4838=%#x 4840=%#x 4848=%#x\n",
		    reg_rd(camnoc_rt_base, 0x4830u), reg_rd(camnoc_rt_base, 0x4834u),
		    reg_rd(camnoc_rt_base, 0x4838u), reg_rd(camnoc_rt_base, 0x4840u),
		    reg_rd(camnoc_rt_base, 0x4848u));
	}
}

static void
cam_diag_midframe_sample(void)
{
	if (boot_cam_probe == 0 || cam_mid_sampled || cam_hf_qtb_base == 0 || !gcc_cam_hf_axi_ok) {
		return;
	}
	cam_mid_sampled = true;
	cam_qtb_mid_ot0 = reg_rd64(cam_hf_qtb_base, 0xc10u);
	cam_qtb_mid_ot1 = reg_rd64(cam_hf_qtb_base, 0xc20u);
}

static void
cam_diag_bus_debug_enable(void)
{
	if (csid_lite_base != 0) {
		/* cam_vfe_bus_ver3_start_wm: constraint error detection in debug_status_1. */
		csid_wr(0x177cu, 0xb00u);
	}
	if (boot_cam_bcm != 0 && boot_cam_probe != 0 && camnoc_rt_base != 0) {
		reg_wr(camnoc_rt_base, 0x18u, 1u);
		reg_wr(camnoc_rt_base, 0x08u, 1u);
	}
}

/* What the writer, the bus and CAMNOC hold when a frame did not complete. */
static void
cam_diag_capture_failure(uint32_t sof_ms, uint32_t overflow_ms, uint32_t waited_ms)
{
	say("capture failure: sof=%u ms overflow=%u ms waited=%u ms ccif=%#x size=%#x dbg=%#x/%#x "
	    "addr=%#x/%#x/%#x/%#x\n", sof_ms, overflow_ms, waited_ms,
	    reg_rd(csid_lite_base, 0x1264u), reg_rd(csid_lite_base, 0x1270u),
	    reg_rd(csid_lite_base, 0x1780u), reg_rd(csid_lite_base, 0x1784u),
	    reg_rd(csid_lite_base, 0x1790u), reg_rd(csid_lite_base, 0x1794u),
	    reg_rd(csid_lite_base, 0x1798u), reg_rd(csid_lite_base, 0x179cu));
	if (camnoc_rt_base != 0) {
		say("capture failure: camnoc errvld=%#x log0=%#x log1=%#x sbm=%#x rt1 maxwr=%#x qos=%#x/%#x/%#x/%#x/%#x\n",
		    reg_rd(camnoc_rt_base, 0x10u), reg_rd(camnoc_rt_base, 0x20u),
		    reg_rd(camnoc_rt_base, 0x28u), reg_rd(camnoc_rt_base, 0x248u),
		    reg_rd(camnoc_rt_base, 0x4820u), reg_rd(camnoc_rt_base, 0x4830u),
		    reg_rd(camnoc_rt_base, 0x4834u), reg_rd(camnoc_rt_base, 0x4838u),
		    reg_rd(camnoc_rt_base, 0x4840u), reg_rd(camnoc_rt_base, 0x4848u));
	}
}

static void
cam_diag_capture_probe(const char *tag)
{
	if (boot_cam_probe == 0 || cam_session_probed) {
		return;
	}
	cam_session_probed = true;
	if (cpas_base != 0) {
		say("probe[%s] cpas 08=%#x 1c=%#x dc=%#x ec=%#x f0=%#x f4=%#x f8=%#x\n", tag,
		    reg_rd(cpas_base, 0x08u), reg_rd(cpas_base, 0x1cu), reg_rd(cpas_base, 0xdcu),
		    reg_rd(cpas_base, 0xecu), reg_rd(cpas_base, 0xf0u),
		    reg_rd(cpas_base, 0xf4u), reg_rd(cpas_base, 0xf8u));
	}
	if (camnoc_rt_base != 0) {
		say("probe[%s] camnoc_rt err08=%#x 0c=%#x 10=%#x 18=%#x 20=%#x 24=%#x 28=%#x 2c=%#x 30=%#x 34=%#x 38=%#x 3c=%#x 248=%#x 300=%#x\n",
		    tag, reg_rd(camnoc_rt_base, 0x08u), reg_rd(camnoc_rt_base, 0x0cu),
		    reg_rd(camnoc_rt_base, 0x10u), reg_rd(camnoc_rt_base, 0x18u),
		    reg_rd(camnoc_rt_base, 0x20u), reg_rd(camnoc_rt_base, 0x24u),
		    reg_rd(camnoc_rt_base, 0x28u), reg_rd(camnoc_rt_base, 0x2cu),
		    reg_rd(camnoc_rt_base, 0x30u), reg_rd(camnoc_rt_base, 0x34u),
		    reg_rd(camnoc_rt_base, 0x38u), reg_rd(camnoc_rt_base, 0x3cu),
		    reg_rd(camnoc_rt_base, 0x248u), reg_rd(camnoc_rt_base, 0x300u));
		say("probe[%s] camnoc_rt0=%#x/%#x/%#x rt1=%#x/%#x/%#x/%#x/%#x/%#x rt2=%#x rt3=%#x rt4=%#x\n",
		    tag, reg_rd(camnoc_rt_base, 0x4620u), reg_rd(camnoc_rt_base, 0x4630u),
		    reg_rd(camnoc_rt_base, 0x4640u), reg_rd(camnoc_rt_base, 0x4820u),
		    reg_rd(camnoc_rt_base, 0x4830u), reg_rd(camnoc_rt_base, 0x4834u),
		    reg_rd(camnoc_rt_base, 0x4838u), reg_rd(camnoc_rt_base, 0x4840u),
		    reg_rd(camnoc_rt_base, 0x4848u), reg_rd(camnoc_rt_base, 0x4a20u),
		    reg_rd(camnoc_rt_base, 0x4c20u), reg_rd(camnoc_rt_base, 0x5220u));
	}
	if (camnoc_nrt_base != 0 && cam_nrt_axi_ok) {
		say("probe[%s] camnoc_nrt 08=%#x 10=%#x 20=%#x 28=%#x 248=%#x\n", tag,
		    reg_rd(camnoc_nrt_base, 0x08u), reg_rd(camnoc_nrt_base, 0x10u),
		    reg_rd(camnoc_nrt_base, 0x20u), reg_rd(camnoc_nrt_base, 0x28u),
		    reg_rd(camnoc_nrt_base, 0x248u));
	}
	if (cam_hf_qtb_base != 0 && gcc_cam_hf_axi_ok) {
		say("probe[%s] qtb swid=%#llx freq=%#llx fack=%#llx ot0=%#llx ot1=%#llx mid0=%#llx mid1=%#llx\n",
		    tag,
		    (unsigned long long)reg_rd64(cam_hf_qtb_base, 0x000u),
		    (unsigned long long)reg_rd64(cam_hf_qtb_base, 0x410u),
		    (unsigned long long)reg_rd64(cam_hf_qtb_base, 0x418u),
		    (unsigned long long)reg_rd64(cam_hf_qtb_base, 0xc10u),
		    (unsigned long long)reg_rd64(cam_hf_qtb_base, 0xc20u),
		    (unsigned long long)cam_qtb_mid_ot0,
		    (unsigned long long)cam_qtb_mid_ot1);
	}
	if (mmss_noc_base != 0 && gcc_cam_hf_axi_ok) {
		say("probe[%s] mmss_noc p0=%#x/%#x/%#x/%#x/%#x p1=%#x/%#x/%#x/%#x/%#x\n", tag,
		    reg_rd(mmss_noc_base, 0x29000u), reg_rd(mmss_noc_base, 0x29004u),
		    reg_rd(mmss_noc_base, 0x29008u), reg_rd(mmss_noc_base, 0x2900cu),
		    reg_rd(mmss_noc_base, 0x29014u),
		    reg_rd(mmss_noc_base, 0x2a000u), reg_rd(mmss_noc_base, 0x2a004u),
		    reg_rd(mmss_noc_base, 0x2a008u), reg_rd(mmss_noc_base, 0x2a00cu),
		    reg_rd(mmss_noc_base, 0x2a014u));
	}
	if (cam_smmu_base != 0) {
		uint32_t actlr = cam_smmu_bank != 0xffu ? reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_ACTLR) : 0u;
		uint32_t sctlr = cam_smmu_bank != 0xffu ? reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_SCTLR) : 0u;
		uint32_t fsr = cam_smmu_bank != 0xffu ? reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FSR) : 0u;
		say("probe[%s] smmu cr0=%#x cr2=%#x gfsr=%#x syn=%#x/%#x/%#x cb%u actlr=%#x sctlr=%#x fsr=%#x\n",
		    tag, reg_rd(cam_smmu_base, 0x000u), reg_rd(cam_smmu_base, 0x008u),
		    reg_rd(cam_smmu_base, 0x048u), reg_rd(cam_smmu_base, 0x050u),
		    reg_rd(cam_smmu_base, 0x054u), reg_rd(cam_smmu_base, 0x058u),
		    cam_smmu_bank, actlr, sctlr, fsr);
		if (cam_smmu_bank != 0xffu) {   /* the address and syndrome of the context's last fault */
			say("probe[%s] smmu fault far=%#x:%#x fsynr0=%#x fsynr1=%#x ttbr0=%#x:%#x tcr=%#x tcr2=%#x mair0=%#x\n", tag,
			    reg_rd(cam_smmu_base, cam_smmu_cb_offset + 0x064u), reg_rd(cam_smmu_base, cam_smmu_cb_offset + 0x060u),
			    reg_rd(cam_smmu_base, cam_smmu_cb_offset + 0x068u), reg_rd(cam_smmu_base, cam_smmu_cb_offset + 0x06cu),
			    reg_rd(cam_smmu_base, cam_smmu_cb_offset + 0x024u), reg_rd(cam_smmu_base, cam_smmu_cb_offset + 0x020u),
			    reg_rd(cam_smmu_base, cam_smmu_cb_offset + 0x030u), reg_rd(cam_smmu_base, cam_smmu_cb_offset + 0x010u),
			    reg_rd(cam_smmu_base, cam_smmu_cb_offset + 0x038u));
		}
		for (uint32_t i = 0; i < cam_smmu_stream_count && i < 256u; i++) {
			uint32_t smr = reg_rd(cam_smmu_base, CAM_SMMU_SMR(i));
			uint32_t sid = smr & 0x7fffu;
			if ((smr & CAM_SMMU_SMR_VALID) != 0 &&
			    ((sid & 0x7c00u) == 0x1800u || (sid & 0x7c00u) == 0x1c00u)) {
				say("probe[%s] smmu slot=%u smr=%#x s2cr=%#x\n", tag, i,
				    smr, reg_rd(cam_smmu_base, CAM_SMMU_S2CR(i)));
			}
		}
	}
	if (cam_crm_base != 0) {
		say("probe[%s] cam_crm p1=%#x p2=%#x be=%#x fe=%#x\n", tag,
		    reg_rd(cam_crm_base, 0x4d04u), reg_rd(cam_crm_base, 0x4d08u),
		    reg_rd(cam_crm_base, 0x1e18u), reg_rd(cam_crm_base, 0x1e1cu));
	}
	if (csid_lite_base != 0) {
		say("probe[%s] bus 1200=%#x 121c=%#x 1228=%#x 122c=%#x wm 1700=%#x 1704=%#x 170c=%#x 1714=%#x\n",
		    tag, reg_rd(csid_lite_base, 0x1200u), reg_rd(csid_lite_base, 0x121cu),
		    reg_rd(csid_lite_base, 0x1228u), reg_rd(csid_lite_base, 0x122cu),
		    reg_rd(csid_lite_base, 0x1700u), reg_rd(csid_lite_base, 0x1704u),
		    reg_rd(csid_lite_base, 0x170cu), reg_rd(csid_lite_base, 0x1714u));
		/* IFE-lite overflow (0x1268) and out-of-bounds (0x1270) status: distinguish a WM-generated bad address from CAMNOC mis-decoding a valid one. */
		say("probe[%s] bus_ovf 1260=%#x 1264=%#x 1268=%#x 126c=%#x 1270=%#x 1274=%#x 1278=%#x\n",
		    tag, reg_rd(csid_lite_base, 0x1260u), reg_rd(csid_lite_base, 0x1264u),
		    reg_rd(csid_lite_base, 0x1268u), reg_rd(csid_lite_base, 0x126cu),
		    reg_rd(csid_lite_base, 0x1270u), reg_rd(csid_lite_base, 0x1274u),
		    reg_rd(csid_lite_base, 0x1278u));
		say("probe[%s] wm_dbg 1770=%#x 177c=%#x 1780=%#x 1784=%#x 1790=%#x 1794=%#x 1798=%#x 179c=%#x\n",
		    tag, reg_rd(csid_lite_base, 0x1770u), reg_rd(csid_lite_base, 0x177cu),
		    reg_rd(csid_lite_base, 0x1780u), reg_rd(csid_lite_base, 0x1784u),
		    reg_rd(csid_lite_base, 0x1790u), reg_rd(csid_lite_base, 0x1794u),
		    reg_rd(csid_lite_base, 0x1798u), reg_rd(csid_lite_base, 0x179cu));
	}
}

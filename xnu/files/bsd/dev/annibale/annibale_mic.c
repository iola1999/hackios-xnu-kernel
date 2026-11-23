/*
 * Microphone capture driver for REDMI K90 (annibale):
 * Implements the SM8750 LPASS VA Macro / DMIC / AudioReach capture path
 * (K90 hardware disables WCD9395 via qcom,wcd-disabled and wires DMIC0..5):
 *   - Q6PRM LPASS core vote (PARAM_ID_RSC_HW_CORE=1) & VA/TX MCLK (19.2 MHz)
 *   - DMIC power/enable TLMM GPIOs 182/183 & LPI TLMM pins 6/7, 8/9, 12/13 (func 1)
 *   - LPASS VA Macro (0x07660000) MCLK, FS broadcast, DMIC0 clock & DEC0 path
 *   - AudioReach capture subgraphs (upstream audioreach-topology capture chain):
 *       SG_MIC (0x4007):     MOD_CODEC_DMA_SOURCE (0x07001024, IID 0x6071) on LPASS VA0
 *       SG_CAPTURE (0x4003): MOD_MFC (0x07001015, IID 0x6025) ->
 *                            MOD_PCM_CNV (0x07001003, IID 0x6023) ->
 *                            MOD_PCM_ENC (0x07001004, IID 0x6022) ->
 *                            MOD_RD_SHMEM (0x07001001, IID 0x6021)
 *   - Shared DMA capture window at reach_buffer + 0x8000 (4 slots of 960 frames @ 48 kHz)
 *   - Exposes 48 kHz 16-bit PCM capture on both /dev/annibale-mic and /dev/annibale-audio read()
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/annibale_audio.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern bool annibale_adsp_is_hw_up(void);
extern bool annibale_audio_reach_is_active(void);
extern bool annibale_adsp_gpr_send(const void *pkt, uint32_t len, uint32_t wait_ms,
    uint32_t *rsp_opcode, uint32_t *rsp_words, uint32_t max_words);
extern bool annibale_audio_reach_dma_info(volatile uint8_t **va_out, uint64_t *pa_out,
    uint64_t *dsp_addr_out, uint32_t *map_handle_out);
extern bool annibale_audio_reach_send_cmd(const void *raw_pkt, uint32_t raw_len, const char *descr,
    uint32_t *rsp_opcode, uint32_t *rsp_words, uint32_t max_words);

bool annibale_mic_gpr_event(uint32_t src_port, uint32_t dst_port, uint32_t opcode,
    const void *payload, uint32_t len);

#define say(...) printf("dmic: " __VA_ARGS__)

/* Hardware physical addresses on SM8750 (annibale) */
#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x00150000UL
#define PIN_DMIC23_EN           182u
#define PIN_DMIC01_EN           183u

#define TX_MACRO_PHYS           0x06ae0000UL
#define TX_MACRO_SIZE           0x00002000UL
#define SWRM_TX_PHYS            0x07630000UL
#define SWRM_TX_SIZE            0x00002000UL
#define VA_MACRO_PHYS           0x07660000UL
#define VA_MACRO_SIZE           0x00002000UL
#define LPI_TLMM_PHYS           0x07760000UL
#define LPI_TLMM_SIZE           0x00030000UL

/* AudioReach capture constants */
#define GPR_DOMAIN_ADSP         2u
#define GPR_DOMAIN_APPS         3u
#define OUR_MIC_PORT            0x2002u
#define APM_IID                 1u
#define PRM_IID                 2u

#define OP_GRAPH_OPEN           0x01001000u
#define OP_GRAPH_PREPARE        0x01001001u
#define OP_GRAPH_START          0x01001002u
#define OP_GRAPH_STOP           0x01001003u
#define OP_GRAPH_CLOSE          0x01001004u
#define OP_SET_CFG              0x01001006u
#define OP_READ_BUF             0x0400100bu
#define OP_READ_BUF_DONE        0x05001005u
#define PRM_CMD_REQUEST_HW_RSC      0x0100100fu
#define PRM_CMD_RSP_REQUEST_HW_RSC  0x02001002u
#define PARAM_ID_RSC_AUDIO_HW_CLK   0x0800102cu
#define PARAM_ID_RSC_HW_CORE        0x08001032u
#define Q6PRM_HW_CORE_ID_LPASS      1u
#define Q6PRM_HW_CORE_ID_DCODEC     2u
#define Q6PRM_LPASS_CLK_ID_VA_MCLK  0x307u
#define Q6PRM_LPASS_CLK_ID_VA_2XMCLK 0x308u /* Q6PRM_LPASS_CLK_ID_VA_CORE_2X_MCLK */
#define Q6PRM_LPASS_CLK_SRC_INT     1u
#define Q6PRM_LPASS_CLK_ROOT_DEF    0u
#define VA_MACRO_MCLK_HZ            19200000u

#define PARAM_ID_HW_EP_POWER_MODE_CFG 0x08001176u
#define PARAM_ID_HW_EP_DMA_DATA_ALIGN 0x08001233u
#define HW_EP_POWER_MODE_KEEPCLOCKS   2u   /* XO shutdown not allowed while capturing */
#define HW_EP_DMA_DATA_ALIGN_LSB      1u

#define MOD_RD_SHMEM            0x07001001u
#define MOD_PCM_CNV             0x07001003u
#define MOD_PCM_ENC             0x07001004u
#define MOD_MFC                 0x07001015u
#define MOD_LOG                 0x0700101Au
#define MOD_CODEC_DMA_SOURCE    0x07001024u

#define SG_CAPTURE              0x4003u
#define SG_MIC                  0x4007u

#define IID_CAP_SHMEM           0x6021u
#define IID_CAP_ENC             0x6022u
#define IID_CAP_CNV             0x6023u
#define IID_CAP_LOG             0x6024u
#define IID_CAP_MFC             0x6025u
#define IID_MIC_SOURCE          0x6071u
#define IID_MIC_LOG             0x6072u

#define MIC_RATE                48000u
#define MIC_PERIOD_FRAMES       960u            /* 20 ms @ 48 kHz */
#define MIC_PERIOD_BYTES        (MIC_PERIOD_FRAMES * sizeof(int16_t)) /* 1920 bytes mono */
#define MIC_OFFSET              0x8000u
#define MIC_SLOTS               4u
#define MIC_RING_SAMPLES        65536u          /* ~1.36 s at 48 kHz */
#define MIC_RING_MASK           (MIC_RING_SAMPLES - 1u)

static LCK_GRP_DECLARE(amic_lck_grp, "annibale-mic");
static LCK_MTX_DECLARE(amic_lock, &amic_lck_grp);
static lck_spin_t *amic_ring_lock;

static vm_offset_t tlmm_base;
static int16_t *mic_ring;
static uint32_t mic_ring_head;
static uint32_t mic_ring_tail;

static uint32_t mic_open_clients;
static uint32_t mic_graphs_opened; /* bit 0 SG_MIC, bit 1 SG_CAPTURE */
static bool mic_graphs_started;
static int mic_capture_error;
static bool mic_hw_prepared;
static uint32_t mic_token = 500;
static uint64_t last_blocking_read_ns;
static volatile uint32_t mic_inflight_mask;
static uint32_t mic_slot_map_handle;
static uint64_t mic_slot_dsp_base;
static volatile uint8_t *mic_slot_dma_va;
static volatile bool mic_async_failed;
static uint32_t mic_sent_count;
static uint32_t mic_done_count;
static uint32_t mic_last_bytes, mic_peak;

/* ---- Packet builder helpers ------------------------------------------------------------------ */

struct mic_pkt {
	uint8_t  buf[1024];
	uint32_t len;
};

static void
mpkt_put32(struct mic_pkt *p, uint32_t val)
{
	if (p->len + 4 <= sizeof(p->buf)) {
		memcpy(p->buf + p->len, &val, 4);
		p->len += 4;
	}
}

static void
mpkt_put16(struct mic_pkt *p, uint16_t val)
{
	if (p->len + 2 <= sizeof(p->buf)) {
		memcpy(p->buf + p->len, &val, 2);
		p->len += 2;
	}
}

static void
mpkt_put8(struct mic_pkt *p, uint8_t val)
{
	if (p->len < sizeof(p->buf)) {
		p->buf[p->len++] = val;
	}
}

static void
mpkt_set32(struct mic_pkt *p, uint32_t off, uint32_t val)
{
	if (off + 4 <= sizeof(p->buf)) {
		memcpy(p->buf + off, &val, 4);
	}
}

static void
mpkt_pad_to(struct mic_pkt *p, uint32_t target)
{
	while (p->len < target && p->len < sizeof(p->buf)) {
		p->buf[p->len++] = 0;
	}
}

static void
mpkt_begin(struct mic_pkt *p, uint32_t src, uint32_t dst, uint32_t opcode)
{
	uint32_t tok = __atomic_add_fetch(&mic_token, 1u, __ATOMIC_RELAXED);
	p->len = 0;
	mpkt_put32(p, 0);
	mpkt_put32(p, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	mpkt_put32(p, src);
	mpkt_put32(p, dst);
	mpkt_put32(p, tok);
	mpkt_put32(p, opcode);
}

static void
mpkt_finish(struct mic_pkt *p)
{
	mpkt_set32(p, 0, (6u << 4) | (p->len << 8));
}

static uint32_t
mpkt_cmd_begin(struct mic_pkt *p)
{
	uint32_t at = p->len;
	mpkt_put32(p, 0);
	mpkt_put32(p, 0);
	mpkt_put32(p, 0);
	mpkt_put32(p, 0);
	return at;
}

static void
mpkt_cmd_end(struct mic_pkt *p, uint32_t at)
{
	mpkt_set32(p, at + 12, p->len - at - 16);
}

static uint32_t
mpkt_param_begin(struct mic_pkt *p, uint32_t iid, uint32_t id)
{
	uint32_t at = p->len;
	mpkt_put32(p, iid);
	mpkt_put32(p, id);
	mpkt_put32(p, 0);
	mpkt_put32(p, 0);
	return at;
}

static void
mpkt_param_end(struct mic_pkt *p, uint32_t at)
{
	mpkt_pad_to(p, at + ((p->len - at + 7u) & ~7u));
	mpkt_set32(p, at + 8, p->len - at - 16);
}

struct mic_mod {
	uint32_t module;
	uint32_t iid;
	uint32_t inputs;
	uint32_t outputs;
};

struct mic_link {
	uint32_t from;
	uint32_t to;
};

static void
mpkt_graph_open(struct mic_pkt *p, uint32_t sg, uint32_t dir, const struct mic_mod *m,
    unsigned num_mods, const struct mic_link *links, unsigned num_links)
{
	mpkt_begin(p, OUR_MIC_PORT, APM_IID, OP_GRAPH_OPEN);
	uint32_t cmd = mpkt_cmd_begin(p);

	uint32_t at = mpkt_param_begin(p, APM_IID, 0x08001001); /* SUB_GRAPH_CONFIG */
	mpkt_put32(p, 1);
	mpkt_put32(p, sg);
	mpkt_put32(p, 3);
	mpkt_put32(p, 0x0800100e); mpkt_put32(p, 4); mpkt_put32(p, 2);  /* PERF_MODE low-latency */
	mpkt_put32(p, 0x0800100f); mpkt_put32(p, 4); mpkt_put32(p, dir);
	mpkt_put32(p, 0x08001010); mpkt_put32(p, 4); mpkt_put32(p, 2); /* SCENARIO_ID record */
	mpkt_param_end(p, at);

	at = mpkt_param_begin(p, APM_IID, 0x08001000); /* CONTAINER_CONFIG */
	mpkt_put32(p, 1);
	mpkt_put32(p, sg);
	mpkt_put32(p, 4);
	mpkt_put32(p, 0x08001011); mpkt_put32(p, 8); mpkt_put32(p, 1); mpkt_put32(p, 0x0b001001); /* GC */
	mpkt_put32(p, 0x08001012); mpkt_put32(p, 4); mpkt_put32(p, 4); /* GRAPH_POS global-dev */
	mpkt_put32(p, 0x08001013); mpkt_put32(p, 4); mpkt_put32(p, 8192);
	mpkt_put32(p, 0x08001014); mpkt_put32(p, 4); mpkt_put32(p, 2); /* PROC_DOMAIN ADSP */
	mpkt_param_end(p, at);

	at = mpkt_param_begin(p, APM_IID, 0x08001002);
	mpkt_put32(p, 1);
	mpkt_put32(p, sg);
	mpkt_put32(p, sg);
	mpkt_put32(p, num_mods);
	for (unsigned i = 0; i < num_mods; i++) {
		mpkt_put32(p, m[i].module);
		mpkt_put32(p, m[i].iid);
	}
	mpkt_param_end(p, at);

	at = mpkt_param_begin(p, APM_IID, 0x08001003);
	mpkt_put32(p, num_mods);
	for (unsigned i = 0; i < num_mods; i++) {
		mpkt_put32(p, m[i].iid);
		mpkt_put32(p, 1);
		mpkt_put32(p, 0x08001015);
		mpkt_put32(p, 8);
		mpkt_put32(p, m[i].inputs);
		mpkt_put32(p, m[i].outputs);
	}
	mpkt_param_end(p, at);

	if (num_links > 0) {
		at = mpkt_param_begin(p, APM_IID, 0x08001004);
		mpkt_put32(p, num_links);
		for (unsigned i = 0; i < num_links; i++) {
			mpkt_put32(p, links[i].from);
			mpkt_put32(p, 1);
			mpkt_put32(p, links[i].to);
			mpkt_put32(p, 2);
		}
		mpkt_param_end(p, at);
	}

	mpkt_cmd_end(p, cmd);
	mpkt_finish(p);
}

static bool
mic_graph_cmd(uint32_t opcode, uint32_t sg, const char *descr)
{
	struct mic_pkt p;
	mpkt_begin(&p, OUR_MIC_PORT, APM_IID, opcode);
	uint32_t cmd = mpkt_cmd_begin(&p);
	uint32_t at = mpkt_param_begin(&p, APM_IID, 0x08001005);
	mpkt_put32(&p, 1);
	mpkt_put32(&p, sg);
	mpkt_param_end(&p, at);
	mpkt_cmd_end(&p, cmd);
	mpkt_finish(&p);
	return annibale_audio_reach_send_cmd(p.buf, p.len, descr, NULL, NULL, 0);
}

/* ---- LPASS PRM Voting, DMIC Pins & VA Macro Setup ---------------------------------------------- */

static bool
mic_prm_vote_hw_core(uint32_t core_id, const char *descr)
{
	struct mic_pkt p;
	uint32_t rop = 0;

	mpkt_begin(&p, OUR_MIC_PORT, PRM_IID, PRM_CMD_REQUEST_HW_RSC);
	uint32_t cmd = mpkt_cmd_begin(&p);
	uint32_t at = mpkt_param_begin(&p, PRM_IID, PARAM_ID_RSC_HW_CORE);
	mpkt_put32(&p, core_id);
	mpkt_set32(&p, at + 8, p.len - at - 16u);
	mpkt_cmd_end(&p, cmd);
	mpkt_finish(&p);
	return annibale_audio_reach_send_cmd(p.buf, p.len, descr, &rop, NULL, 0) &&
	    rop == PRM_CMD_RSP_REQUEST_HW_RSC;
}

static bool
mic_prm_set_clk(uint32_t clk_id, uint32_t freq_hz, const char *descr)
{
	struct mic_pkt p;
	uint32_t rop = 0;

	mpkt_begin(&p, OUR_MIC_PORT, PRM_IID, PRM_CMD_REQUEST_HW_RSC);
	uint32_t cmd = mpkt_cmd_begin(&p);
	uint32_t at = mpkt_param_begin(&p, PRM_IID, PARAM_ID_RSC_AUDIO_HW_CLK);
	mpkt_put32(&p, 1u);
	mpkt_put32(&p, clk_id);
	mpkt_put32(&p, freq_hz);
	mpkt_put32(&p, Q6PRM_LPASS_CLK_SRC_INT);
	mpkt_put32(&p, Q6PRM_LPASS_CLK_ROOT_DEF);
	mpkt_set32(&p, at + 8, p.len - at - 16u);
	mpkt_cmd_end(&p, cmd);
	mpkt_finish(&p);
	return annibale_audio_reach_send_cmd(p.buf, p.len, descr, &rop, NULL, 0) &&
	    rop == PRM_CMD_RSP_REQUEST_HW_RSC;
}

static bool
mic_configure_dmic_pins(void)
{
	static const unsigned en_pins[] = { PIN_DMIC23_EN, PIN_DMIC01_EN };
	static const struct {
		unsigned pin;
		bool is_clk;
	} lpi_dmic_pins[] = {
		{ 6u, true }, { 7u, false },
		{ 8u, true }, { 9u, false },
		{ 12u, true }, { 13u, false },
		{ 17u, true }, { 18u, false }, /* dmic67 pair, present in the K90 dts */
	};

	if (tlmm_base == 0) {
		tlmm_base = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	if (tlmm_base == 0) {
		say("TLMM mapping failed\n");
		return false;
	}
	for (unsigned i = 0; i < sizeof(en_pins) / sizeof(en_pins[0]); i++) {
		vm_offset_t cfg_reg = tlmm_base + 0x1000UL * en_pins[i];
		vm_offset_t io_reg = cfg_reg + 4UL;
		uint32_t cfg = *(volatile uint32_t *)cfg_reg;
		cfg = (cfg & ~0x3ffu) | (1u << 9) | (3u << 6) | 3u; /* GPIO output-high, 8 mA, pull-up */
		if (cfg & (1u << 11)) {
			cfg |= (1u << 12);
		}
		*(volatile uint32_t *)io_reg = 2u;
		*(volatile uint32_t *)cfg_reg = cfg;
	}
	__builtin_arm_dsb(0xf);

	vm_offset_t lpi_tlmm = ml_io_map(LPI_TLMM_PHYS, LPI_TLMM_SIZE);
	if (lpi_tlmm == 0) {
		say("LPI TLMM mapping failed\n");
		return false;
	}
	for (unsigned i = 0; i < sizeof(lpi_dmic_pins) / sizeof(lpi_dmic_pins[0]); i++) {
		vm_offset_t reg = lpi_tlmm + 0x1000UL * lpi_dmic_pins[i].pin;
		uint32_t v = *(volatile uint32_t *)reg;
		v = (v & ~0x3ffu) | (3u << 6) | (1u << 2); /* func 1, 8 mA, bias disable */
		if (lpi_dmic_pins[i].is_clk) {
			v |= (1u << 9);
			*(volatile uint32_t *)(reg + 4UL) = 2u;
		}
		*(volatile uint32_t *)reg = v;
	}
	__builtin_arm_dsb(0xf);
	return true;
}

static void
mic_configure_va_macro(void)
{
	vm_offset_t va = ml_io_map(VA_MACRO_PHYS, VA_MACRO_SIZE);
	if (va == 0) {
		say("VA macro mapping failed\n");
		return;
	}
	volatile uint32_t *regs = (volatile uint32_t *)va;
	say("VA macro core id %#x %#x %#x %#x\n",
	    regs[0x00c0 / 4], regs[0x00c4 / 4], regs[0x00c8 / 4], regs[0x00cc / 4]);

	regs[0x0000 / 4] |= 1u; /* MCLK_CONTROL_EN */
	regs[0x0004 / 4] = 3u;  /* FS_CNT clear + enable */
	regs[0x0004 / 4] = 1u;
	regs[0x0008 / 4] |= 1u; /* SWR_CONTROL SWR_CLK_ENABLE: sm8550 and newer
	 * carry the decimator output out over the macro's own SoundWire master;
	 * upstream lpass-va-macro turns this clock on in fsgen_gate_enable for
	 * has_swr_master parts. */
	regs[0x0080 / 4] |= 2u; /* FS_BROADCAST_EN */
	regs[0x0094 / 4] = 0u;  /* release RESET_ALL_DMICS */
	/* DMIC_CTL's div field is an index into {2,3,4,6,8,16}, matching the
	 * upstream VA_MACRO_CLK_DIV_* enum: 5 is the dts /16 selection. */
	regs[0x0084 / 4] = (5u << 1) | 1u; /* DMIC0/1 enable, /16 = 1.2 MHz */
	regs[0x0088 / 4] = (5u << 1) | 1u; /* DMIC2/3 enable, /16 */
	regs[0x008c / 4] = (5u << 1) | 1u; /* DMIC4/5 enable, /16 */
	regs[0x0090 / 4] = (5u << 1) | 1u; /* DMIC6/7 enable, /16 */
	regs[0x0104 / 4] = 0u;             /* DEC0 mux = VA_DMIC */
	regs[0x0100 / 4] = (1u << 4);      /* DEC0 input = DMIC0 */
	regs[0x0404 / 4] = (1u << 7) | (2u << 5); /* DMIC select, 150 Hz HPF */
	regs[0x0400 / 4] = (1u << 5) | 4u; /* TX0 CLK_EN, 48 kHz, unmuted */
	regs[0x0418 / 4] = 0u;
	__builtin_arm_dsb(0xf);
	delay_for_interval(1, NSEC_PER_MSEC);
	regs[0x0418 / 4] = 2u;
	delay_for_interval(1, NSEC_PER_MSEC);
	regs[0x0418 / 4] = 0u;
	regs[0x0418 / 4] = 1u;
	delay_for_interval(6, NSEC_PER_MSEC);
	regs[0x040c / 4] = 0u;             /* TX0 0 dB gain */
	__builtin_arm_dsb(0xf);
}

static bool
mic_hw_prepare(void)
{
	if (mic_hw_prepared) {
		return true;
	}
	if (!annibale_adsp_is_hw_up() || !annibale_audio_reach_is_active()) {
		return false;
	}

	if (!mic_prm_vote_hw_core(Q6PRM_HW_CORE_ID_LPASS, "REQ LPASS CORE")) {
		say("LPASS core resource request failed\n");
		return false;
	}
	/* The codec digital island powers the decimator-to-LPAIF data path; the
	 * VA macro registers stay writable without it, which matches reads that
	 * take but never produce samples. Upstream votes it with the macro vote. */
	if (!mic_prm_vote_hw_core(Q6PRM_HW_CORE_ID_DCODEC, "REQ DCODEC CORE")) {
		say("DCODEC core resource request failed\n");
		return false;
	}
	if (!mic_prm_set_clk(Q6PRM_LPASS_CLK_ID_VA_MCLK, VA_MACRO_MCLK_HZ, "REQ VA MCLK")) {
		say("LPASS VA MCLK request failed\n");
		return false;
	}
	/* The decimator-to-LPAIF path in the VA macro runs on the 2X core clock;
	 * without this request the TX DMA interface has no clock and returns zeros. */
	if (!mic_prm_set_clk(Q6PRM_LPASS_CLK_ID_VA_2XMCLK, VA_MACRO_MCLK_HZ, "REQ VA 2X MCLK")) {
		say("LPASS VA 2X MCLK request failed\n");
		return false;
	}

	if (!mic_configure_dmic_pins()) {
		return false;
	}
	mic_configure_va_macro();

	mic_hw_prepared = true;
	say("LPASS core, VA macro and DMIC pins configured\n");
	return true;
}

static bool
open_capture_graphs(void)
{
	static const struct mic_mod mic_mods[] = {
		{ MOD_CODEC_DMA_SOURCE, IID_MIC_SOURCE, 0, 1 }
	};
	static const struct mic_mod cap_mods[] = {
		{ MOD_RD_SHMEM, IID_CAP_SHMEM, 1, 0 },
		{ MOD_PCM_ENC,  IID_CAP_ENC,   1, 1 },
		{ MOD_PCM_CNV,  IID_CAP_CNV,   1, 1 },
		{ MOD_MFC,      IID_CAP_MFC,   1, 1 }
	};
	static const struct mic_link cap_links[] = {
		{ IID_MIC_SOURCE, IID_CAP_MFC },
		{ IID_CAP_MFC,    IID_CAP_CNV },
		{ IID_CAP_CNV,    IID_CAP_ENC },
		{ IID_CAP_ENC,    IID_CAP_SHMEM }
	};
	struct mic_pkt p;

	/* 1. Open SG_MIC (0x4007) */
	mpkt_graph_open(&p, SG_MIC, 1, mic_mods, 1, NULL, 0);
	if (!annibale_audio_reach_send_cmd(p.buf, p.len, "OPEN SG_MIC (0x4007)", NULL, NULL, 0)) return false;
	mic_graphs_opened |= 1u;

	/* 2. Codec DMA source as audioreach_codec_dma_set_media_format: VA TX0, 48 kHz 16-bit mono */
	mpkt_begin(&p, OUR_MIC_PORT, APM_IID, OP_SET_CFG);
	uint32_t cmd = mpkt_cmd_begin(&p);
	uint32_t at = mpkt_param_begin(&p, IID_MIC_SOURCE, 0x08001063); /* CODEC_DMA_INTF_CFG */
	mpkt_put32(&p, 3); /* lpaif_type: 1 RXTX, 2 WSA, 3 VA, 4 AXI */
	mpkt_put32(&p, 1); /* intf_index: TX0 */
	mpkt_put32(&p, 1); /* active_channels_mask = ch 0 */
	mpkt_param_end(&p, at);
	at = mpkt_param_begin(&p, IID_MIC_SOURCE, 0x08001018); /* HW_EP_FRAME_SIZE_FACTOR */
	mpkt_put32(&p, 1);
	mpkt_param_end(&p, at);
	at = mpkt_param_begin(&p, IID_MIC_SOURCE, 0x08001017); /* HW_EP_MF */
	mpkt_put32(&p, MIC_RATE);
	mpkt_put16(&p, 16);
	mpkt_put16(&p, 1);
	mpkt_put32(&p, 1);
	mpkt_param_end(&p, at);
	at = mpkt_param_begin(&p, IID_MIC_SOURCE, PARAM_ID_HW_EP_POWER_MODE_CFG);
	mpkt_put32(&p, HW_EP_POWER_MODE_KEEPCLOCKS);
	mpkt_param_end(&p, at);
	at = mpkt_param_begin(&p, IID_MIC_SOURCE, PARAM_ID_HW_EP_DMA_DATA_ALIGN);
	mpkt_put32(&p, HW_EP_DMA_DATA_ALIGN_LSB);
	mpkt_param_end(&p, at);
	mpkt_cmd_end(&p, cmd);
	mpkt_finish(&p);
	if (!annibale_audio_reach_send_cmd(p.buf, p.len, "SET_CFG MIC_SOURCE", NULL, NULL, 0)) return false;

	/* 3. Open SG_CAPTURE (0x4003) */
	mpkt_graph_open(&p, SG_CAPTURE, 1, cap_mods, 4, cap_links, 4);
	if (!annibale_audio_reach_send_cmd(p.buf, p.len, "OPEN SG_CAPTURE (0x4003)", NULL, NULL, 0)) return false;
	mic_graphs_opened |= 2u;

	/* 3a. MFC output format, as audioreach_set_media_format: 48 kHz 16-bit mono fixed-point */
	mpkt_begin(&p, OUR_MIC_PORT, APM_IID, OP_SET_CFG);
	cmd = mpkt_cmd_begin(&p);
	at = mpkt_param_begin(&p, IID_CAP_MFC, 0x08001024); /* MFC_OUTPUT_MEDIA_FORMAT */
	mpkt_put32(&p, MIC_RATE);
	mpkt_put16(&p, 16);
	mpkt_put16(&p, 1);
	mpkt_put16(&p, 1); /* channel_mapping[0] = PCM_CHANNEL_FL */
	mpkt_put16(&p, 0);
	mpkt_param_end(&p, at);
	mpkt_cmd_end(&p, cmd);
	mpkt_finish(&p);
	if (!annibale_audio_reach_send_cmd(p.buf, p.len, "SET_CFG CAP_MFC", NULL, NULL, 0)) return false;

	/*
	 * 4. PCM_OUTPUT_FORMAT_CFG for the shmem endpoint, the converter and the encoder, the
	 * layout the playback graph sends its PCM modules: 16-bit Q15 LE interleaved mono.
	 * The payload size excludes padding.
	 */
	mpkt_begin(&p, OUR_MIC_PORT, APM_IID, OP_SET_CFG);
	cmd = mpkt_cmd_begin(&p);
	const uint32_t pcm_iids[] = { IID_CAP_CNV, IID_CAP_ENC };
	for (unsigned i = 0; i < 2; i++) {
		at = mpkt_param_begin(&p, pcm_iids[i], 0x08001008);
		mpkt_put32(&p, 1); mpkt_put32(&p, 0x09001000); mpkt_put32(&p, 17);
		mpkt_put16(&p, 16); mpkt_put16(&p, 1); mpkt_put16(&p, 16); mpkt_put16(&p, 15);
		mpkt_put16(&p, 1);  mpkt_put16(&p, 1); mpkt_put16(&p, 0);  mpkt_put16(&p, 1);
		mpkt_put8(&p, 1);
		mpkt_param_end(&p, at);
	}
	mpkt_cmd_end(&p, cmd);
	mpkt_finish(&p);
	return annibale_audio_reach_send_cmd(p.buf, p.len, "SET_CFG CAP_CNV & ENC", NULL, NULL, 0);
}

/* A sub-graph left open after a failed setup answers the next OPEN with AR_EALREADY. */
static void
close_capture_graphs(void)
{
	if ((mic_graphs_opened & 2u) != 0) {
		(void)mic_graph_cmd(OP_GRAPH_CLOSE, SG_CAPTURE, "CLOSE SG_CAPTURE");
	}
	if ((mic_graphs_opened & 1u) != 0) {
		(void)mic_graph_cmd(OP_GRAPH_CLOSE, SG_MIC, "CLOSE SG_MIC");
	}
	mic_graphs_opened = 0;
}

static bool
start_capture_graphs(void)
{
	if (mic_graphs_started) {
		return true;
	}
	if (!mic_hw_prepare()) return false;

	if (mic_graphs_opened != 3u) {
		if (!open_capture_graphs()) {
			close_capture_graphs();
			return false;
		}
	}

	/* 5. Prepare & Start SG_MIC and SG_CAPTURE */
	if (!mic_graph_cmd(OP_GRAPH_PREPARE, SG_MIC, "PREPARE SG_MIC") ||
	    !mic_graph_cmd(OP_GRAPH_START, SG_MIC, "START SG_MIC") ||
	    !mic_graph_cmd(OP_GRAPH_PREPARE, SG_CAPTURE, "PREPARE SG_CAPTURE") ||
	    !mic_graph_cmd(OP_GRAPH_START, SG_CAPTURE, "START SG_CAPTURE")) return false;

	__atomic_store_n(&mic_async_failed, false, __ATOMIC_RELEASE);
	mic_graphs_started = true;
	say("AudioReach capture graphs SG_MIC (0x4007) & SG_CAPTURE (0x4003) started\n");
	return true;
}

/* Stop capture subgraphs and clear stale ring/inflight state; caller holds amic_lock. */
static void
stop_capture_graphs(void)
{
	if (mic_graphs_started) {
		say("capture stops: %u reads asked, %u done, the last of %u bytes, loudest sample %u\n",
		    mic_sent_count, mic_done_count, mic_last_bytes, mic_peak);
		mic_peak = 0;
		(void)mic_graph_cmd(OP_GRAPH_STOP, SG_CAPTURE, "STOP SG_CAPTURE");
		(void)mic_graph_cmd(OP_GRAPH_STOP, SG_MIC, "STOP SG_MIC");
		mic_graphs_started = false;
	}
	lck_spin_lock(amic_ring_lock);
	mic_inflight_mask = 0;
	mic_ring_head = 0;
	mic_ring_tail = 0;
	lck_spin_unlock(amic_ring_lock);
	__atomic_store_n(&mic_async_failed, false, __ATOMIC_RELEASE);
	last_blocking_read_ns = 0;
	thread_wakeup((event_t)__DEVOLATILE(void *, &mic_inflight_mask));
}

static bool
queue_capture_slot_async(uint32_t slot, volatile uint8_t *dma_va, uint64_t dsp_base, uint32_t map_handle)
{
	struct mic_pkt p;
	uint64_t slot_addr = dsp_base + MIC_OFFSET + (uint64_t)slot * MIC_PERIOD_BYTES;

	__builtin_arm_dsb(0xf);
	mpkt_begin(&p, OUR_MIC_PORT, IID_CAP_SHMEM, OP_READ_BUF);
	mpkt_put32(&p, (uint32_t)slot_addr);
	mpkt_put32(&p, (uint32_t)(slot_addr >> 32));
	mpkt_put32(&p, map_handle);
	mpkt_put32(&p, MIC_PERIOD_BYTES);
	mpkt_put32(&p, 0); /* metadata address */
	mpkt_put32(&p, 0); /* metadata address MSW */
	mpkt_put32(&p, 0); /* metadata map handle */
	mpkt_put32(&p, 0); /* metadata size */
	mpkt_finish(&p);

	lck_spin_lock(amic_ring_lock);
	mic_slot_dma_va = dma_va;
	mic_slot_dsp_base = dsp_base;
	mic_slot_map_handle = map_handle;
	mic_inflight_mask |= (1u << slot);
	lck_spin_unlock(amic_ring_lock);

	if (!annibale_adsp_gpr_send(p.buf, p.len, 0, NULL, NULL, 0)) {
		lck_spin_lock(amic_ring_lock);
		mic_inflight_mask &= ~(1u << slot);
		lck_spin_unlock(amic_ring_lock);
		return false;
	}
	lck_spin_lock(amic_ring_lock);
	mic_sent_count++;
	lck_spin_unlock(amic_ring_lock);
	return true;
}

/* ---- Continuous 48 kHz Capture Ring --------------------------------------------------------- */

static void
harvest_capture_period(uint32_t slot, volatile uint8_t *dma_va, uint32_t valid_bytes)
{
	int16_t period_mono[MIC_PERIOD_FRAMES];
	uint32_t valid_samples = valid_bytes / sizeof(int16_t);
	if (dma_va == NULL || valid_samples > MIC_PERIOD_FRAMES) return;
	int16_t period_stereo[MIC_PERIOD_FRAMES * 2];
	if (dma_va != NULL) {
		volatile int16_t *hw = (volatile int16_t *)(volatile void *)(dma_va + MIC_OFFSET + slot * MIC_PERIOD_BYTES);
		for (uint32_t i = 0; i < valid_samples; i++) {
			period_mono[i] = hw[i];
			uint32_t level = period_mono[i] < 0 ? (uint32_t)-(int32_t)period_mono[i] : (uint32_t)period_mono[i];
			if (level > mic_peak) {
				mic_peak = level;
			}
		}
		mic_last_bytes = valid_bytes;
	}

	lck_spin_lock(amic_ring_lock);
	for (uint32_t i = 0; i < valid_samples; i++) {
		if ((mic_ring_head - mic_ring_tail) >= MIC_RING_SAMPLES) {
			mic_ring_tail++;
		}
		mic_ring[mic_ring_head & MIC_RING_MASK] = period_mono[i];
		mic_ring_head++;
	}
	lck_spin_unlock(amic_ring_lock);

	if (annibale_audio_recording_mic()) {
		for (uint32_t i = 0; i < valid_samples; i++) {
			period_stereo[2 * i] = period_mono[i];
			period_stereo[2 * i + 1] = period_mono[i];
		}
		annibale_audio_record_fed(period_stereo, valid_samples);
	}
}

bool
annibale_mic_gpr_event(uint32_t src_port, uint32_t dst_port, uint32_t opcode,
    const void *payload, uint32_t len)
{
	if (src_port != IID_CAP_SHMEM || dst_port != OUR_MIC_PORT || opcode != OP_READ_BUF_DONE) {
		return false;
	}
	if (payload == NULL || len < 56u) {
		__atomic_store_n(&mic_async_failed, true, __ATOMIC_RELEASE);
		thread_wakeup((event_t)__DEVOLATILE(void *, &mic_inflight_mask));
		return true;
	}

	uint32_t words[14];
	memcpy(words, payload, sizeof(words));

	lck_spin_lock(amic_ring_lock);
	uint64_t dsp_base = mic_slot_dsp_base;
	uint32_t map_handle = mic_slot_map_handle;
	volatile uint8_t *dma_va = mic_slot_dma_va;
	uint32_t mask = mic_inflight_mask;
	lck_spin_unlock(amic_ring_lock);

	uint64_t cap_base = dsp_base + MIC_OFFSET;
	uint64_t done_addr = ((uint64_t)words[2] << 32) | words[1];
	bool addr_ok = (done_addr >= cap_base) &&
	    (done_addr < cap_base + (uint64_t)MIC_SLOTS * MIC_PERIOD_BYTES) &&
	    (((done_addr - cap_base) % MIC_PERIOD_BYTES) == 0);
	uint32_t slot = addr_ok ? (uint32_t)((done_addr - cap_base) / MIC_PERIOD_BYTES) : 0u;

	if (!addr_ok || (mask & (1u << slot)) == 0 || words[0] != 0 ||
	    words[3] != map_handle || words[4] > MIC_PERIOD_BYTES ||
	    (words[4] % sizeof(int16_t)) != 0 || words[13] != 0) {
		static bool refused_logged;
		if (!refused_logged) {
			refused_logged = true;
			say("read done refused: status %#x addr %#x:%#x handle %#x (ours %#x) size %u w5 %#x flags %#x "
			    "w9 %#x w13 %#x inflight %#x\n", words[0], words[2], words[1], words[3], map_handle,
			    words[4], words[5], words[8], words[9], words[13], mask);
		}
		__atomic_store_n(&mic_async_failed, true, __ATOMIC_RELEASE);
		if (addr_ok) {
			lck_spin_lock(amic_ring_lock);
			mic_inflight_mask &= ~(1u << slot);
			lck_spin_unlock(amic_ring_lock);
		}
		thread_wakeup((event_t)__DEVOLATILE(void *, &mic_inflight_mask));
		return true;
	}

	/* Harvest DMA period before releasing slot bit so feeder cannot overwrite it. */
	harvest_capture_period(slot, dma_va, words[4]);

	lck_spin_lock(amic_ring_lock);
	mic_inflight_mask &= ~(1u << slot);
	mic_done_count++;
	lck_spin_unlock(amic_ring_lock);
	thread_wakeup((event_t)__DEVOLATILE(void *, &mic_inflight_mask));
	return true;
}

__dead2
static void
mic_capture_thread(void *arg __unused, wait_result_t w __unused)
{
	uint32_t slot = 0, last_done = 0, stall_ticks = 0;

	say("capture feeder thread running\n");
	for (;;) {
		bool active = (mic_open_clients > 0) || annibale_audio_recording_mic();
		if (active) {
			lck_mtx_lock(&amic_lock);
			if (mic_capture_error != 0 || (!mic_graphs_started && !start_capture_graphs())) {
				lck_mtx_unlock(&amic_lock);
				delay_for_interval(20, NSEC_PER_MSEC);
				continue;
			}
			if (__atomic_load_n(&mic_async_failed, __ATOMIC_ACQUIRE)) {
				stop_capture_graphs();
				mic_capture_error = EIO;
				lck_mtx_unlock(&amic_lock);
				say("capture DMA completion failed; capture stopped\n");
				continue;
			}

			volatile uint8_t *dma_va = NULL;
			uint64_t dma_pa = 0, dsp_addr = 0;
			uint32_t map_handle = 0;
			if (!annibale_audio_reach_dma_info(&dma_va, &dma_pa, &dsp_addr, &map_handle)) {
				stop_capture_graphs();
				mic_capture_error = EIO;
				lck_mtx_unlock(&amic_lock);
				say("capture DMA info unavailable; capture stopped\n");
				continue;
			}

			lck_spin_lock(amic_ring_lock);
			uint32_t mask = mic_inflight_mask;
			uint32_t done = mic_done_count;
			lck_spin_unlock(amic_ring_lock);
			uint32_t next_slot = slot;
			bool failed = false;

			for (uint32_t i = 0; i < MIC_SLOTS && mic_graphs_started; i++) {
				uint32_t s = (slot + i) % MIC_SLOTS;
				if ((mask & (1u << s)) != 0) {
					continue;
				}
				if (!queue_capture_slot_async(s, dma_va, dsp_addr, map_handle)) {
					stop_capture_graphs();
					mic_capture_error = EIO;
					say("capture DMA submission failed; capture stopped\n");
					failed = true;
					break;
				}
				next_slot = (s + 1u) % MIC_SLOTS;
				lck_spin_lock(amic_ring_lock);
				mask = mic_inflight_mask;
				lck_spin_unlock(amic_ring_lock);
			}
			slot = next_slot;
			uint32_t inflight = (uint32_t)__builtin_popcount(mask);
			if (!failed && inflight > 0 && done == last_done && ++stall_ticks >= 25u) {
				stop_capture_graphs();
				mic_capture_error = EIO;
				lck_mtx_unlock(&amic_lock);
				say("capture DMA completion stalled; capture stopped\n");
				stall_ticks = 0;
				continue;
			}
			lck_mtx_unlock(&amic_lock);
			if (failed) {
				continue;
			}
			if (inflight == 0 || done != last_done) {
				last_done = done;
				stall_ticks = 0;
			}

			assert_wait_timeout((event_t)__DEVOLATILE(void *, &mic_inflight_mask),
			    THREAD_INTERRUPTIBLE, 25, NSEC_PER_MSEC);
			lck_spin_lock(amic_ring_lock);
			bool can_step = (__builtin_popcount(mic_inflight_mask) < (int)MIC_SLOTS) ||
			    __atomic_load_n(&mic_async_failed, __ATOMIC_ACQUIRE) ||
			    (mic_open_clients == 0 && !annibale_audio_recording_mic());
			lck_spin_unlock(amic_ring_lock);
			if (can_step) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
		} else {
			lck_mtx_lock(&amic_lock);
			if (mic_graphs_started) {
				stop_capture_graphs();
			}
			lck_mtx_unlock(&amic_lock);
			last_done = 0;
			stall_ticks = 0;
			assert_wait_timeout((event_t)&mic_open_clients, THREAD_INTERRUPTIBLE, 250, NSEC_PER_MSEC);
			if ((mic_open_clients > 0) || annibale_audio_recording_mic()) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
		}
	}
}

/* ---- /dev/annibale-mic and /dev/annibale-audio read() implementation --------------------------- */

int annibale_mic_read(struct uio *uio, int ioflag);
void annibale_mic_init(void);

int
annibale_mic_read(struct uio *uio, int ioflag)
{
	int16_t chunk[1024];
	int error = 0;
	bool nonblock = (ioflag & IO_NDELAY) != 0;
	bool moved_any = false;

	if (mic_ring == NULL) {
		return ENXIO;
	}

	lck_mtx_lock(&amic_lock);
	if (mic_capture_error != 0 || (!mic_graphs_started && !start_capture_graphs())) {
		lck_mtx_unlock(&amic_lock);
		return EIO;
	}
	lck_mtx_unlock(&amic_lock);

	/*
	 * For blocking callers (not O_NONBLOCK), pace reads at 48 kHz wall-clock rate so
	 * direct readers (cat / dd / test tools) receive samples at 48,000 frames/s.
	 * For O_NONBLOCK callers (audiomxd CoreAudio IO thread), return only available
	 * samples without zero-padding so short reads surface to the HAL accurately.
	 */
	user_ssize_t total_req_bytes = uio_resid(uio);
	if (!nonblock && total_req_bytes >= 2) {
		uint64_t now_ns = 0;
		absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
		uint64_t req_samples = (uint64_t)total_req_bytes / sizeof(int16_t);
		uint64_t need_ns = (req_samples * NSEC_PER_SEC) / MIC_RATE;
		uint64_t prev_ns = last_blocking_read_ns;
		if (prev_ns != 0 && now_ns < prev_ns + need_ns) {
			uint64_t wait_ns = (prev_ns + need_ns) - now_ns;
			if (wait_ns > 100 * NSEC_PER_MSEC) {
				wait_ns = 20 * NSEC_PER_MSEC;
			}
			delay_for_interval((uint32_t)(wait_ns / 1000ULL), NSEC_PER_USEC);
			absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
		}
		last_blocking_read_ns = now_ns;
	}

	while (uio_resid(uio) >= (user_ssize_t)sizeof(int16_t)) {
		user_ssize_t bytes = uio_resid(uio);
		if (bytes > (user_ssize_t)sizeof(chunk)) {
			bytes = sizeof(chunk);
		}
		bytes -= bytes % (user_ssize_t)sizeof(int16_t);
		uint32_t want = (uint32_t)(bytes / (user_ssize_t)sizeof(int16_t));
		uint32_t take = 0;

		lck_spin_lock(amic_ring_lock);
		uint32_t avail = mic_ring_head - mic_ring_tail;
		if (nonblock) {
			take = avail < want ? avail : want;
			for (uint32_t i = 0; i < take; i++) {
				chunk[i] = mic_ring[mic_ring_tail & MIC_RING_MASK];
				mic_ring_tail++;
			}
		} else {
			take = want;
			for (uint32_t i = 0; i < want; i++) {
				if (mic_ring_tail != mic_ring_head) {
					chunk[i] = mic_ring[mic_ring_tail & MIC_RING_MASK];
					mic_ring_tail++;
				} else {
					chunk[i] = 0;
				}
			}
		}
		lck_spin_unlock(amic_ring_lock);

		if (nonblock && take == 0) {
			if (!moved_any) {
				error = EAGAIN;
			}
			break;
		}
		error = uiomove((caddr_t)chunk, (int)(take * (uint32_t)sizeof(int16_t)), uio);
		if (error != 0) {
			break;
		}
		moved_any = true;
		if (nonblock && take < want) {
			break;
		}
	}
	return error;
}

static int
mic_cdev_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	lck_mtx_lock(&amic_lock);
	if (mic_ring == NULL) {
		lck_mtx_unlock(&amic_lock);
		return ENXIO;
	}
	if (mic_open_clients == 0 && mic_capture_error != 0) {
		stop_capture_graphs();
		mic_capture_error = 0;
	}
	if (mic_capture_error != 0 || (!mic_graphs_started && !start_capture_graphs())) {
		lck_mtx_unlock(&amic_lock);
		return EIO;
	}
	mic_open_clients++;
	lck_mtx_unlock(&amic_lock);
	thread_wakeup((event_t)&mic_open_clients);
	return 0;
}

static int
mic_cdev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	lck_mtx_lock(&amic_lock);
	if (mic_open_clients > 0) {
		mic_open_clients--;
	}
	if (mic_open_clients == 0 && !annibale_audio_recording_mic()) {
		stop_capture_graphs();
		mic_capture_error = 0;
	}
	lck_mtx_unlock(&amic_lock);
	thread_wakeup((event_t)&mic_open_clients);
	return 0;
}

static int
mic_cdev_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	return annibale_mic_read(uio, ioflag);
}

static const struct cdevsw mic_cdevsw = {
	.d_open = mic_cdev_open,
	.d_close = mic_cdev_close,
	.d_read = mic_cdev_read,
	.d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0
};

void
annibale_mic_init(void)
{
	say("initializing LPASS DMIC / VA Macro / AudioReach microphone subsystem...\n");

	if (amic_ring_lock == NULL) {
		amic_ring_lock = lck_spin_alloc_init(&amic_lck_grp, LCK_ATTR_NULL);
	}
	if (mic_ring == NULL) {
		mic_ring = kalloc_data(MIC_RING_SAMPLES * sizeof(int16_t), Z_WAITOK | Z_ZERO);
	}

	int major = cdevsw_add(-1, &mic_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "annibale-mic") == NULL) {
		say("warning: could not create /dev/annibale-mic node\n");
	} else {
		say("registered /dev/annibale-mic (major %d)\n", major);
	}

	uint32_t mic_boot = 0;
	if (PE_parse_boot_argn("mic", &mic_boot, sizeof(mic_boot)) && mic_boot > 0) {
		mic_open_clients = 1;
	}

	thread_t th;
	if (kernel_thread_start(mic_capture_thread, NULL, &th) == KERN_SUCCESS) {
		thread_set_thread_name(th, "annibale-mic-feeder");
		thread_deallocate(th);
	}
}

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

#define VA_MACRO_PHYS           0x07660000UL
#define VA_MACRO_SIZE           0x00002000UL
#define LPI_TLMM_PHYS           0x07760000UL
#define LPI_TLMM_SIZE           0x00030000UL
#define TX_MACRO_PHYS           0x06AE0000UL
#define TX_MACRO_SIZE           0x00020000UL
#define VA_SWRM_PHYS            0x07630000UL  /* va-macro's own SoundWire master (swr2) */
#define VA_SWRM_SIZE            0x00008000UL

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
#define GPR_BASIC_RSP_RESULT    0x02001005u
#define PRM_CMD_REQUEST_HW_RSC      0x0100100fu
#define PRM_CMD_RSP_REQUEST_HW_RSC  0x02001002u
#define PARAM_ID_RSC_AUDIO_HW_CLK   0x0800102cu
#define PARAM_ID_RSC_HW_CORE        0x08001032u
#define Q6PRM_HW_CORE_ID_LPASS      1u
#define Q6PRM_HW_CORE_ID_DCODEC     2u
#define Q6PRM_LPASS_CLK_ID_VA_MCLK  0x307u
#define Q6PRM_LPASS_CLK_ID_VA_2XMCLK 0x308u /* Q6PRM_LPASS_CLK_ID_VA_CORE_2X_MCLK */
#define Q6PRM_LPASS_CLK_ID_TX_MCLK  0x30cu /* Q6PRM_LPASS_CLK_ID_TX_CORE_MCLK */
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
static uint32_t mic_intf_index = 4;   /* codec-DMA RXTX TX_CDC_DMA_TX_3; mic_intf= overrides */
static uint32_t mic_lpaif_type = 1;   /* LPAIF block: 1 RXTX, 2 WSA, 3 VA; mic_lpaif= overrides */
static uint32_t mic_ch_mask = 2;      /* AIF slot bitmask: decimator i feeds slot i upstream */
static uint32_t mic_v41 = 1;          /* LPASS codec v4.1 register sequence; mic_v41=0 forces legacy */
static uint32_t mic_dmic_sel = 1;     /* TX DEC1 input-mux index (shift 4 of 0x0108); mic_dmic= overrides */
static uint32_t mic_pmode = 0;        /* HW_EP power mode, as upstream sends; mic_pmode= overrides */
static uint32_t mic_noalign = 1;      /* upstream omits DMA_DATA_ALIGN on codec-DMA; mic_noalign=0 keeps it */

/* mic_sweep=1 walks these (lpaif,intf,dec,dmic,swr) combos: on each capture
 * stall, flat or empty stream and failed graph start the next combo is tried,
 * so one round covers the whole binding space instead of one boot per guess.
 * dec selects which VA decimator gets programmed (the TX DMA channel binds a
 * decimator; intf selects the channel). swr=1 selects SWR_MIC mux mode with
 * sel in CFG0[3:0]; the K90 dts routes DMICs over the SoundWire pins, so the
 * SWR_MIC5..10 channels are tried before the pin-DMIC space again. */
static const struct mic_combo { uint8_t lpaif, intf, dec, dmic, swr, mac; } mic_combos[] = {
	/* TX_CDC_DMA_TX_3..5 (intf 4..6 on the RXTX LPAIF): redwood's proven mic
	 * port is TX_CDC_DMA_TX_3 + decimator 1, and K90's own sound card routes
	 * "TX DMIC0..5" into "RX_TX DECx_INP" -> "TX DECx MUX". These lead since
	 * every booted sweep starts at index 0. */
	{1,4,1,1,0,1},{1,4,1,2,0,1},{1,4,1,3,0,1},{1,4,1,4,0,1},
	{1,4,1,5,0,1},{1,4,1,6,0,1},
	{1,4,0,1,0,1},{1,4,2,1,0,1},{1,4,3,1,0,1},{1,4,4,1,0,1},{1,4,5,1,0,1},
	{1,4,0,3,0,1},{1,4,2,3,0,1},{1,4,3,3,0,1},{1,4,4,3,0,1},{1,4,5,3,0,1},
	{1,4,0,5,0,1},{1,4,2,5,0,1},{1,4,3,5,0,1},{1,4,4,5,0,1},{1,4,5,5,0,1},
	{1,5,1,1,0,1},{1,5,1,3,0,1},{1,5,1,5,0,1},
	{1,6,1,1,0,1},{1,6,1,3,0,1},{1,6,1,5,0,1},
	/* is-used-swr-gpio=1 on the K90 va-macro: its DMICs ride the SoundWire
	 * pins, decoded by the SWR_MIC5..10 taps (three pairs -> CTL0..2). With
	 * the master now up these are the dts-prescribed path. */
	{3,1,0,5,1,0},{3,1,0,7,1,0},{3,1,0,9,1,0},
	{3,1,1,5,1,0},{3,1,1,7,1,0},{3,1,1,9,1,0},
	{3,1,2,5,1,0},{3,1,2,7,1,0},{3,1,2,9,1,0},
	{3,2,0,5,1,0},{3,2,0,7,1,0},{3,2,0,9,1,0},
	{3,3,0,5,1,0},{3,3,0,7,1,0},{3,3,0,9,1,0},
	/* Pin probes show live PDM on all three populated DMIC pairs, so the
	 * missing link is which VA TX interface carries which decimator. Walk
	 * the full (intf, dec) cross product on the live channel space; the
	 * diagonal is first since 1:1 mapping is the most likely. */
	{3,1,0,1,0,0},{3,2,1,3,0,0},{3,3,2,5,0,0},
	{3,1,0,2,0,0},{3,1,0,3,0,0},{3,1,0,4,0,0},{3,1,0,5,0,0},{3,1,0,6,0,0},
	{3,1,1,1,0,0},{3,1,1,2,0,0},{3,1,1,3,0,0},{3,1,1,4,0,0},{3,1,1,5,0,0},{3,1,1,6,0,0},
	{3,1,2,1,0,0},{3,1,2,2,0,0},{3,1,2,3,0,0},{3,1,2,4,0,0},{3,1,2,5,0,0},{3,1,2,6,0,0},
	{3,1,3,1,0,0},{3,1,3,2,0,0},{3,1,3,3,0,0},{3,1,3,4,0,0},{3,1,3,5,0,0},{3,1,3,6,0,0},
	{3,2,0,1,0,0},{3,2,0,2,0,0},{3,2,0,3,0,0},{3,2,0,4,0,0},{3,2,0,5,0,0},{3,2,0,6,0,0},
	{3,2,1,1,0,0},{3,2,1,2,0,0},{3,2,1,4,0,0},{3,2,1,5,0,0},{3,2,1,6,0,0},
	{3,2,2,1,0,0},{3,2,2,2,0,0},{3,2,2,3,0,0},{3,2,2,4,0,0},{3,2,2,5,0,0},{3,2,2,6,0,0},
	{3,2,3,1,0,0},{3,2,3,2,0,0},{3,2,3,3,0,0},{3,2,3,4,0,0},{3,2,3,5,0,0},{3,2,3,6,0,0},
	{3,3,0,1,0,0},{3,3,0,2,0,0},{3,3,0,3,0,0},{3,3,0,4,0,0},{3,3,0,5,0,0},{3,3,0,6,0,0},
	{3,3,1,1,0,0},{3,3,1,2,0,0},{3,3,1,3,0,0},{3,3,1,4,0,0},{3,3,1,5,0,0},{3,3,1,6,0,0},
	{3,3,2,1,0,0},{3,3,2,2,0,0},{3,3,2,3,0,0},{3,3,2,4,0,0},{3,3,2,6,0,0},
	{3,3,3,1,0,0},{3,3,3,2,0,0},{3,3,3,3,0,0},{3,3,3,4,0,0},{3,3,3,5,0,0},{3,3,3,6,0,0},
	/* TX macro (mac=1): the K90 sound card also routes "TX DMIC0..5" and its
	 * is-used-swr-gpio=0, so the LPI DMIC lanes may feed tx-macro decimators
	 * instead of the VA block. Its codec-DMA side is the RXTX LPAIF (type 1);
	 * AIF1..3 surface as intf 1..3, eight decimators, same mux fields. The
	 * DMIC lane clock is shared — the VA DMIC_CTL writes above already drive
	 * the pads, so the TX side only needs its mux + decimator + AIF binding. */
	{1,1,0,1,0,1},{1,1,0,2,0,1},{1,1,0,3,0,1},{1,1,0,4,0,1},
	{1,1,0,5,0,1},{1,1,0,6,0,1},{1,1,0,7,0,1},{1,1,0,8,0,1},
	{1,2,0,1,0,1},{1,2,0,5,0,1},{1,2,1,1,0,1},{1,2,1,5,0,1},
	{1,2,2,1,0,1},{1,2,2,5,0,1},{1,2,3,1,0,1},{1,2,3,5,0,1},
	{1,3,0,1,0,1},{1,3,0,5,0,1},{1,3,1,1,0,1},{1,3,1,5,0,1},
	{1,3,2,1,0,1},{1,3,2,5,0,1},{1,3,3,1,0,1},{1,3,3,5,0,1},
	{1,1,1,5,0,1},{1,1,2,5,0,1},{1,1,3,5,0,1},
	{2,1,0,1,0,0},{4,1,0,1,0,0},                           /* WSA / AXI */
};
static uint32_t mic_sweep;
static uint32_t mic_combo_idx;
static uint32_t mic_combo_fails;
static int32_t mic_win_min, mic_win_max;  /* min/max sample seen since the current combo */
static uint32_t mic_win_seen;             /* samples harvested under the current combo */
static uint32_t mic_win_sig;              /* samples strictly inside the rail values */
static uint64_t last_blocking_read_ns;
/* Last read() on any mic entry point: audiomxd polls /dev/annibale-audio when the
 * dedicated node refused it, and that fd never counts in mic_open_clients. */
static volatile uint64_t mic_last_read_ns;
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

/* A graph command issued while the DSP still drains the previous transition
 * answers with a non-zero status; the same command succeeds on retry. */
static bool
mic_graph_cmd_retry(uint32_t opcode, uint32_t sg, const char *descr)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (mic_graph_cmd(opcode, sg, descr)) {
			return true;
		}
		delay_for_interval(60, NSEC_PER_MSEC);
	}
	return false;
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
	/* VA SoundWire pins from the dts va_swr pinctrl: gpio0 clk, gpio1/2/14
	 * data0..2, all func 1 at 4 mA. DMICs can ride these lines in SWR_MIC
	 * mux mode (qcom,is-used-swr-gpio=1 on K90), so they are part of the
	 * candidate capture path even with the WCD codec disabled. */
	{
		static const unsigned swr_data_pins[] = { 1u, 2u, 14u };
		vm_offset_t clk_reg = lpi_tlmm;
		uint32_t v = *(volatile uint32_t *)clk_reg;
		v = (v & ~0x3ffu) | (1u << 9) | (1u << 6) | (1u << 2); /* clk: func1, 4 mA, output */
		*(volatile uint32_t *)(clk_reg + 4UL) = 2u;
		*(volatile uint32_t *)clk_reg = v;
		for (unsigned i = 0; i < sizeof(swr_data_pins) / sizeof(swr_data_pins[0]); i++) {
			vm_offset_t reg = lpi_tlmm + 0x1000UL * swr_data_pins[i];
			v = *(volatile uint32_t *)reg;
			v = (v & ~0x3ffu) | (1u << 6) | (1u << 2) | 3u; /* func1, 4 mA, bus hold */
			*(volatile uint32_t *)reg = v;
		}
	}
	__builtin_arm_dsb(0xf);
	return true;
}

static void mic_program_dec(volatile uint32_t *regs, uint32_t dec, uint32_t sel, uint32_t swr, bool is_tx);
static void mic_dump_regs(void);

/* Bring up the VA macro's internal SoundWire master (swr2, swrm-io-base
 * 0x7630000): on K90 the VA AIFs route over VA_SWR_CLK, so a silent bus means
 * the codec-DMA VA interfaces read an idle lane — matching the constant
 * captures. Sequence after qcom_swrm_init()/swrm_master_init(): HCTL gate
 * clear (SWR>=1.6 workaround), component reset, frame 50 rows x 2 columns at
 * 9.6 MHz, auto-enumeration, 31 no-pings, then the version-specific clock
 * start and component enable. */
static void
mic_swr_master_on(void)
{
	vm_offset_t swr = ml_io_map(VA_SWRM_PHYS, VA_SWRM_SIZE);
	if (swr == 0) {
		say("VA SWR master mapping failed\n");
		return;
	}
	volatile uint32_t *s = (volatile uint32_t *)swr;
	uint32_t ver = s[0x0000 / 4];
	say("VA SWR master HW version %#x\n", ver);
	/* The v1.6 hw_ctl gate workaround applies via the reset framework's
	 * "swr_audio_cgcr" reset; the K90 dts binds none for this master, and the
	 * per-master HCTL region from older SoCs does not exist at the usual
	 * offset here (touching it panics), so this master takes no gate write. */
	s[0x0008 / 4] = 1u; /* COMP_SW_RESET */
	s[0x0008 / 4] = 1u;
	bool v2 = ver >= 0x02000000u;
	uint32_t frame = (1u << 3) | 0u | ((48u - 1u) << 16); /* 9.6MHz*2/(50*2*4kHz)-1 */
	s[0x101c / 4] = frame;               /* MCP_FRAME_CTRL_BANK0 */
	s[0x0500 / 4] = 1u;                  /* ENUMERATOR_CFG: auto */
	s[0x1048 / 4] = (s[0x1048 / 4] & ~0x3e0000u) | (0x1fu << 17); /* MCP_CFG pings */
	s[0x0314 / 4] = 3u;                  /* CMD_FIFO_CFG retries */
	if (v2) {
		s[0x0018 / 4] = 1u;            /* LINK_MANAGER_EE = CPU1 */
		s[0x5060 / 4] = 1u;            /* V2_0_CLK_CTRL: CLK_START */
	} else {
		s[0x1044 / 4] = 1u;
		s[0x1044 / 4] = 2u;            /* MCP_BUS_CTRL: CLK_START */
	}
	s[0x0004 / 4] = 2u;                  /* COMP_CFG: irq pulse */
	if (v2) {
		s[0x5008 / 4] = 0xffffffffu;   /* V2_0_INTERRUPT_CLEAR */
		s[0x5004 / 4] = 0x1fdfdu;      /* V2_0_INTERRUPT_CPU_EN */
	} else {
		s[0x0208 / 4] = 0xffffffffu;
		s[0x0204 / 4] = 0x1fdfdu;
		s[0x0210 / 4] = 0x1fdfdu;
	}
	s[0x0004 / 4] = 3u;                  /* COMP_CFG: enable + pulse */
	__builtin_arm_dsb(0xf);
	bool linked = false;
	for (unsigned i = 0; i < 100 && !linked; i++) {
		linked = (s[0x0014 / 4] & 1u) != 0;
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	s[0x0314 / 4] |= 0x80000000u;        /* continue exec on cmd ignore */
	say("VA SWR master link %s (status %#x)\n", linked ? "UP" : "DOWN", s[0x0014 / 4]);
	/* The K90 dts declares swr-num-dev=5 on this master: if real slaves
	 * enumerated, the mics may reach the macro as SWR_MIC channels decoded
	 * off the bus rather than on the dedicated DMIC pads. */
	say("VA SWR slave status %#x, dev-ids", s[0x1090 / 4]);
	for (unsigned m = 0; m < 5u; m++) {
		say(" %u:%#x/%#x", m, s[(0x0530 + m * 8) / 4], s[(0x0534 + m * 8) / 4]);
	}
	say("\n");
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
	if (mic_v41) {
		/* LPASS codec v4.1 keeps FS_CNT_BYPASS set while the counter is
		 * enabled; upstream does this for shikra-class parts. */
		regs[0x0004 / 4] = 0x83u;
		regs[0x0004 / 4] = 0x81u;
	} else {
		regs[0x0004 / 4] = 1u;
	}
	regs[0x0008 / 4] |= 1u; /* SWR_CONTROL SWR_CLK_ENABLE: sm8550 and newer
	 * carry the decimator output out over the macro's own SoundWire master;
	 * upstream lpass-va-macro turns this clock on in fsgen_gate_enable for
	 * has_swr_master parts. */
	regs[0x0008 / 4] |= 2u; /* SWR_RESET pulse, as upstream's SWR_CONTROL writes */
	delay_for_interval(1, NSEC_PER_MSEC);
	regs[0x0008 / 4] &= ~2u;
	mic_swr_master_on();
	regs[0x0080 / 4] |= 2u; /* FS_BROADCAST_EN */
	regs[0x0094 / 4] = 0u;  /* release RESET_ALL_DMICS */
	/* DMIC_CTL's div field is an index into {2,3,4,6,8,16}, matching the
	 * upstream VA_MACRO_CLK_DIV_* enum: 5 is the dts /16 selection. */
	regs[0x0084 / 4] = (5u << 1) | 1u; /* DMIC0/1 enable, /16 = 1.2 MHz */
	regs[0x0088 / 4] = (5u << 1) | 1u; /* DMIC2/3 enable, /16 */
	regs[0x008c / 4] = (5u << 1) | 1u; /* DMIC4/5 enable, /16 */
	regs[0x0090 / 4] = (5u << 1) | 1u; /* DMIC6/7 enable, /16 */
	/* Upstream lpass-cdc-va-macro probes SWR_MIC_CTL0..2 with 0xCC masked by
	 * 0xEE: the SWR-pin DMIC bridge configuration the K90 likely needs, since
	 * its dts routes DMICs over the SoundWire gpios (is-used-swr-gpio=1). */
	for (unsigned i = 0; i < 3u; i++) {
		uint32_t off = 0x00d0 + i * 4u;
		regs[off / 4] = (regs[off / 4] & ~0xeeu) | 0xccu;
	}
	/* VA decimator 0 stays enabled even though the proven binding is TX dec1:
	 * removing this line turned a working stream into zero completions on
	 * say50, so the VA block's decimator clock gates something the TX path
	 * shares (DMIC pad receivers or the codec island's decimator domain). */
	mic_program_dec(regs, 0, mic_dmic_sel, 0, false);
	__builtin_arm_dsb(0xf);
	/* Pin-level clock probe, with the mux parked on plain GPIO input: reading
	 * GPIO_VALUE on a func1 pin can return the pad level OR function internal
	 * state depending on the LPI generation, so both readings are taken —
	 * func0 first (true pad level), then the same read with the dts func1
	 * restored. A constant func0 level means nothing reaches the pad at all. */
	{
		vm_offset_t lpi = ml_io_map(LPI_TLMM_PHYS, LPI_TLMM_SIZE);
		if (lpi != 0) {
			static const struct { unsigned pin; const char *name; } probes[] = {
				{ 6u, "dmic01clk" }, { 7u, "dmic01dat" },
				{ 8u, "dmic23clk" }, { 9u, "dmic23dat" },
				{ 12u, "dmic45clk" }, { 13u, "dmic45dat" },
				{ 17u, "dmic67clk" }, { 18u, "dmic67dat" },
				{ 0u, "swrclk" }, { 1u, "swrdata0" },
			};
			for (unsigned p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
				volatile uint32_t *cfg = (volatile uint32_t *)(lpi + 0x1000UL * probes[p].pin);
				volatile uint32_t *io = cfg + 1;
				uint32_t save = *cfg;
				unsigned hi0 = 0, hi1 = 0;
				*cfg = save & ~0x3fu;             /* gpio func, input, bias disable */
				__builtin_arm_dsb(0xf);
				for (unsigned s = 0; s < 2000u; s++) {
					hi0 += *io & 1u;
				}
				*cfg = save;                      /* restore func1/OE/drive */
				__builtin_arm_dsb(0xf);
				for (unsigned s = 0; s < 2000u; s++) {
					hi1 += *io & 1u;
				}
				say("pin probe %s (lpi gpio%u): %u/2000 high as gpio, %u/2000 as func1\n",
				    probes[p].name, probes[p].pin, hi0, hi1);
			}
		}
		if (tlmm_base != 0) {
			static const struct { unsigned pin; const char *name; } en_probes[] = {
				{ PIN_DMIC01_EN, "en-dmic01" }, { PIN_DMIC23_EN, "en-dmic23" },
			};
			for (unsigned p = 0; p < sizeof(en_probes) / sizeof(en_probes[0]); p++) {
				volatile uint32_t *io = (volatile uint32_t *)(tlmm_base + 0x1000UL * en_probes[p].pin + 4UL);
				say("pin probe %s (tlmm gpio%u): io %#x\n",
				    en_probes[p].name, en_probes[p].pin, *io);
			}
		}
	}
}

/* Bring up decimator `dec` listening to input-mux index `sel`: input mux at
 * 0x0100 + dec*8 (CFG0[7:4] selects DMICn in MSM_DMIC mode, CFG0[3:0] selects
 * SWR_MICn when CFG1 bit0=1), TX path at 0x0400 + dec*0x80, v4.1 ADPT block at
 * 0x0800 + dec*0x80 (VA macro only — TX macro tops out at 0x7a8). `swr` picks
 * the SWR_MIC source: DMICs riding the SoundWire pins, which upstream numbers
 * SWR_MIC5..10 (three pairs). */
static void
mic_program_dec(volatile uint32_t *regs, uint32_t dec, uint32_t sel, uint32_t swr, bool is_tx)
{
	uint32_t mux = 0x0100 + dec * 8u;
	uint32_t tx  = 0x0400 + dec * 0x80u;
	uint32_t adpt = 0x0800 + dec * 0x80u;

	regs[(mux + 4) / 4] = swr;          /* CFG1: 0 = MSM_DMIC, 1 = SWR_MIC */
	regs[mux / 4] = swr ? sel : (sel << 4);
	if (swr && !is_tx && sel >= 5u && sel <= 10u) {
		/* Upstream maps SWR_MIC5/6..9/10 onto SWR_MIC_CTL0..2 and writes
		 * the DMIC clock divider into bits[3:1]. */
		uint32_t ctl = 0x00d0 + ((sel - 5u) / 2u) * 4u;
		regs[ctl / 4] = (regs[ctl / 4] & ~0x0eu) | (5u << 1);
	}
	regs[(tx + 4) / 4] = (1u << 7) | (2u << 5); /* DMIC select, 150 Hz HPF */
	regs[tx / 4] = (1u << 5) | 4u;     /* TX CLK_EN, 48 kHz, unmuted */
	regs[(tx + 0x18) / 4] = 0u;
	__builtin_arm_dsb(0xf);
	delay_for_interval(1, NSEC_PER_MSEC);
	regs[(tx + 0x18) / 4] = 2u;
	delay_for_interval(1, NSEC_PER_MSEC);
	regs[(tx + 0x18) / 4] = 0u;
	regs[(tx + 0x18) / 4] = 1u;
	delay_for_interval(6, NSEC_PER_MSEC);
	regs[(tx + 0x0c) / 4] = 0u;        /* 0 dB gain */
	if (mic_v41 && !is_tx) {
		/* v4.1 ADPT blocks reset with the adaptive filter enabled;
		 * upstream clears the enable field in POST_PMU. */
		regs[adpt / 4] &= ~0xffu;
	}
}

/* TX macro @0x6AE0000 shares the VA block's register layout; the LPI DMIC pads
 * are shared lanes already clocked by the VA DMIC_CTL setup, so only the MCLK
 * domain and the decimator itself need programming here. Codec-DMA binds the
 * RXTX LPAIF for this block. */
static void
mic_setup_tx_macro(uint32_t dec, uint32_t dmic, uint32_t swr)
{
	vm_offset_t txv = ml_io_map(TX_MACRO_PHYS, TX_MACRO_SIZE);
	if (txv == 0) {
		say("TX macro mapping failed\n");
		return;
	}
	volatile uint32_t *tregs = (volatile uint32_t *)txv;
	static bool tx_inited;
	if (!tx_inited) {
		tx_inited = true;
		/* 0xC0..0xCC are SWR_DMICn_CTL on TX — the reads just prove the map
		 * is alive, not a core id. */
		say("TX macro SWR_DMIC_CTL reads %#x %#x %#x %#x\n",
		    tregs[0x00c0 / 4], tregs[0x00c4 / 4], tregs[0x00c8 / 4], tregs[0x00cc / 4]);
		tregs[0x0000 / 4] |= 1u; /* MCLK_EN */
		if (mic_v41) {
			tregs[0x0004 / 4] = 0x83u;
			tregs[0x0004 / 4] = 0x81u;
		} else {
			tregs[0x0004 / 4] = 3u;
			tregs[0x0004 / 4] = 1u;
		}
		tregs[0x0080 / 4] |= 2u; /* FS_BROADCAST_EN */
	}
	mic_program_dec(tregs, dec, dmic, swr, true);
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
	/* The codec-DMA TX interface itself runs on the TX core domain, which is a
	 * different island from the VA macro's; upstream binds the va-macro mclk to
	 * it. PRM requests are reference counted, so this stacks on the VA votes. */
	if (!mic_prm_set_clk(Q6PRM_LPASS_CLK_ID_TX_MCLK, VA_MACRO_MCLK_HZ, "REQ TX MCLK")) {
		say("LPASS TX MCLK request failed\n");
		return false;
	}

	if (!mic_configure_dmic_pins()) {
		return false;
	}
	mic_configure_va_macro();
	/* Measured-good binding on the K90: the DMIC0 pad pair feeds TX macro
	 * decimator 1 and the codec-DMA reads it over RXTX TX_CDC_DMA_TX_3,
	 * the same port redwood validates. The sweep overrides this binding. */
	mic_setup_tx_macro(1, mic_dmic_sel, 0);

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
	mpkt_put32(&p, mic_lpaif_type); /* lpaif_type: 1 RXTX, 2 WSA, 3 VA, 4 AXI; mic_lpaif= overrides */
	mpkt_put32(&p, mic_intf_index); /* intf_index: TX0 by default, mic_intf= boot-arg overrides */
	mpkt_put32(&p, mic_ch_mask); /* active_channels_mask: slot i = decimator i */
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
	mpkt_put32(&p, mic_pmode);
	mpkt_param_end(&p, at);
	if (!mic_noalign) {
		/* Upstream omits DMA_DATA_ALIGN for codec-DMA; mic_noalign=1
		 * drops it to check whether the extra param disturbs the port. */
		at = mpkt_param_begin(&p, IID_MIC_SOURCE, PARAM_ID_HW_EP_DMA_DATA_ALIGN);
		mpkt_put32(&p, HW_EP_DMA_DATA_ALIGN_LSB);
		mpkt_param_end(&p, at);
	}
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

static void
mic_apply_combo(const struct mic_combo *c)
{
	if (c->mac != 0) {
		mic_setup_tx_macro(c->dec, c->dmic, c->swr);
	} else {
		vm_offset_t va = ml_io_map(VA_MACRO_PHYS, VA_MACRO_SIZE);
		if (va != 0) {
			mic_program_dec((volatile uint32_t *)va, c->dec, c->dmic, c->swr, false);
		}
	}
	mic_lpaif_type = c->lpaif;
	mic_intf_index = c->intf;
	mic_dmic_sel = c->dmic;
	mic_ch_mask = 1u << c->dec;
	say("mic sweep: combo %u/%u lpaif %u intf %u dec %u %s %u\n",
	    mic_combo_idx + 1, (uint32_t)(sizeof(mic_combos) / sizeof(mic_combos[0])),
	    c->lpaif, c->intf, c->dec, c->swr ? "swrmic" : "dmic", c->dmic);
}

static bool
start_capture_graphs(void)
{
	if (mic_graphs_started) {
		return true;
	}
	if (!mic_hw_prepare()) return false;
	if (mic_sweep && mic_combo_idx < (uint32_t)(sizeof(mic_combos) / sizeof(mic_combos[0]))) {
		mic_apply_combo(&mic_combos[mic_combo_idx]);
	}

	if (mic_graphs_opened != 3u) {
		if (!open_capture_graphs()) {
			close_capture_graphs();
			return false;
		}
	}

	/* 5. Prepare & Start SG_MIC and SG_CAPTURE. Prepare both first and arm
	 * the sink side (SG_CAPTURE) before the codec-DMA source so the stream
	 * has a live endpoint when data starts flowing, matching the single
	 * container START upstream relies on. */
	if (!mic_graph_cmd_retry(OP_GRAPH_PREPARE, SG_MIC, "PREPARE SG_MIC") ||
	    !mic_graph_cmd_retry(OP_GRAPH_PREPARE, SG_CAPTURE, "PREPARE SG_CAPTURE") ||
	    !mic_graph_cmd_retry(OP_GRAPH_START, SG_CAPTURE, "START SG_CAPTURE") ||
	    !mic_graph_cmd_retry(OP_GRAPH_START, SG_MIC, "START SG_MIC")) return false;

	__atomic_store_n(&mic_async_failed, false, __ATOMIC_RELEASE);
	mic_graphs_started = true;
	say("AudioReach capture graphs SG_MIC (0x4007) & SG_CAPTURE (0x4003) started\n");
	mic_dump_regs();
	return true;
}

/* Read the decimator/DMIC/SWR state back after the DSP graph has started: if
 * firmware rewrote the VA macro for its own capture session the reads show
 * its expected values instead of ours, which pins the gap to driver vs DSP
 * ownership of these registers. */
static void
mic_dump_regs(void)
{
	vm_offset_t va = ml_io_map(VA_MACRO_PHYS, VA_MACRO_SIZE);
	if (va == 0) {
		return;
	}
	volatile uint32_t *r = (volatile uint32_t *)va;
	uint32_t dec = mic_ch_mask ? (uint32_t)__builtin_ctz(mic_ch_mask) : 0;
	uint32_t mux = 0x0100 + dec * 8u, tx = 0x0400 + dec * 0x80u;
	say("VA regs now: mclk %#x fs %#x swr %#x top %#x dmic %#x %#x %#x %#x reset %#x\n",
	    r[0x000 / 4], r[0x004 / 4], r[0x008 / 4], r[0x080 / 4],
	    r[0x084 / 4], r[0x088 / 4], r[0x08c / 4], r[0x090 / 4], r[0x094 / 4]);
	say("VA dec%u: mux cfg0 %#x cfg1 %#x | tx ctl %#x cfg0 %#x hpf %#x gain %#x\n",
	    dec, r[mux / 4], r[(mux + 4) / 4],
	    r[tx / 4], r[(tx + 4) / 4], r[(tx + 0x18) / 4], r[(tx + 0x0c) / 4]);
	say("VA swr-mic ctl: %#x %#x %#x\n", r[0x0d0 / 4], r[0x0d4 / 4], r[0x0d8 / 4]);
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
			if (period_mono[i] < mic_win_min) {
				mic_win_min = period_mono[i];
			}
			if (period_mono[i] > mic_win_max) {
				mic_win_max = period_mono[i];
			}
			if (period_mono[i] > -32767 && period_mono[i] < 32767) {
				mic_win_sig++;
			}
			mic_win_seen++;
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
		static int stray_logged;
		/* While capture slots are in flight, every unmatched packet is suspect:
		 * the DSP may answer READ_BUF from another port or opcode, and the
		 * command's own basic response carries its accept/reject status. */
		if (mic_inflight_mask != 0 && stray_logged < 24) {
			uint32_t w0 = 0, w1 = 0;
			if (payload != NULL && len >= 8) {
				memcpy(&w0, payload, 4);
				memcpy(&w1, payload + 4, 4);
			}
			stray_logged++;
			say("capture stray packet: src %#x dst %#x opcode %#x len %u w0 %#x w1 %#x\n",
			    src_port, dst_port, opcode, len, w0, w1);
		}
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

/* Capture runs while a client holds the mic node, while the kernel records, or
 * while reads keep arriving — audiomxd polls /dev/annibale-audio (which never
 * opens the mic node) when the dedicated fd was refused. */
static bool
mic_capture_active(void)
{
	uint64_t last, now_ns = 0;

	if (mic_open_clients > 0 || annibale_audio_recording_mic()) {
		return true;
	}
	last = __atomic_load_n(&mic_last_read_ns, __ATOMIC_ACQUIRE);
	if (last == 0) {
		return false;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
	return now_ns > last && now_ns - last < 3u * NSEC_PER_SEC;
}

__dead2
static void
mic_capture_thread(void *arg __unused, wait_result_t w __unused)
{
	uint32_t slot = 0, last_done = 0, stall_ticks = 0, flat_ticks = 0;

	say("capture feeder thread running\n");
	for (;;) {
		bool active = mic_capture_active();
		if (active) {
			lck_mtx_lock(&amic_lock);
			if (mic_capture_error != 0 || (!mic_graphs_started && !start_capture_graphs())) {
				/* A combo the DSP refuses to start gets three attempts
				 * before the sweep moves on. */
				if (mic_sweep && mic_combo_idx + 1 < (uint32_t)(sizeof(mic_combos) / sizeof(mic_combos[0]))
				    && ++mic_combo_fails >= 3u) {
					say("mic sweep: combo %u refused to start, advancing\n", mic_combo_idx);
					mic_combo_idx++;
					mic_combo_fails = 0;
					mic_win_min = 0x7fffffff;
					mic_win_max = (int32_t)0x80000000;
					mic_win_seen = 0;
					mic_win_sig = 0;
					flat_ticks = 0;
					stall_ticks = 0;
					close_capture_graphs();
					lck_mtx_unlock(&amic_lock);
					continue;
				}
				lck_mtx_unlock(&amic_lock);
				delay_for_interval(20, NSEC_PER_MSEC);
				continue;
			}
			mic_combo_fails = 0;
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
			static int queue_logged;
			if (queue_logged < 12) {
				queue_logged++;
				say("capture queue probe: mask %#x done %u err %d clients %u rec %d\n",
				    mask, done, mic_capture_error, mic_open_clients,
				    annibale_audio_recording_mic() ? 1 : 0);
			}

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
			/* A combo that answers reads but returns a constant sample
			 * stream counts as failed too: the decimator is reading a
			 * dead or floating DMIC line. All-zero is constant too —
			 * 0 sits between the rails, so it is not caught by the
			 * interior counter; compare min/max instead. */
			bool stalled = inflight > 0 && done == last_done && ++stall_ticks >= 25u;
			/* A stream that is constant, or that ever touches a rail
			 * value, is a dead/floating input — a live DMIC gives
			 * mid-scale samples only. Zero is inside the rails, so
			 * the interior counter alone can't tell rail mixes like
			 * {-32768, 0} from audio. */
			bool flat = mic_sweep && mic_win_seen > 200u &&
			    (mic_win_max == mic_win_min || mic_win_min <= -32768 ||
			     mic_win_max >= 32767) && ++flat_ticks >= 40u;
			bool empty = mic_sweep && mic_win_seen == 0 && done > 0 && ++flat_ticks >= 40u;
			if (!failed && (stalled || flat || empty)) {
				if (mic_sweep && mic_combo_idx + 1 < (uint32_t)(sizeof(mic_combos) / sizeof(mic_combos[0]))) {
					say("mic sweep: combo %u %s, advancing\n", mic_combo_idx,
					    stalled ? "stalled" : (empty ? "empty" : "flat"));
					mic_combo_idx++;
					mic_win_min = 0x7fffffff;
					mic_win_max = (int32_t)0x80000000;
					mic_win_seen = 0;
					mic_win_sig = 0;
					flat_ticks = 0;
					stop_capture_graphs();
					close_capture_graphs();
					lck_mtx_unlock(&amic_lock);
					stall_ticks = 0;
					continue;
				}
				stop_capture_graphs();
				mic_capture_error = EIO;
				lck_mtx_unlock(&amic_lock);
				say("capture DMA completion stalled; capture stopped\n");
				stall_ticks = 0;
				continue;
			}
			if (mic_sweep && mic_win_sig > 64u && mic_win_min > -32768 &&
			    mic_win_max < 32767 && mic_win_max - mic_win_min > 100) {
				mic_sweep = 0;
				say("mic sweep: combo %u live (sample range %d..%d, %u interior), locking\n",
				    mic_combo_idx, mic_win_min, mic_win_max, mic_win_sig);
			}
			lck_mtx_unlock(&amic_lock);
			if (failed) {
				continue;
			}
			if (inflight == 0 || done != last_done) {
				if (mic_sweep && done != last_done && mic_win_seen == 0) {
					say("mic sweep: combo %u first done %u\n", mic_combo_idx, done);
				}
				last_done = done;
				stall_ticks = 0;
			}

			assert_wait_timeout((event_t)__DEVOLATILE(void *, &mic_inflight_mask),
			    THREAD_INTERRUPTIBLE, 25, NSEC_PER_MSEC);
			lck_spin_lock(amic_ring_lock);
			bool can_step = (__builtin_popcount(mic_inflight_mask) < (int)MIC_SLOTS) ||
			    __atomic_load_n(&mic_async_failed, __ATOMIC_ACQUIRE);
			lck_spin_unlock(amic_ring_lock);
			if (!can_step) {
				can_step = !mic_capture_active();
			}
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
			if (mic_capture_active()) {
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

	uint64_t read_at_ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &read_at_ns);
	__atomic_store_n(&mic_last_read_ns, read_at_ns, __ATOMIC_RELEASE);

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
mic_cdev_open(__unused dev_t dev, __unused int flags, __unused int devtype, struct proc *p)
{
	static int open_logged;
	char who[32] = "?";

	if (p != NULL) {
		proc_name(proc_selfpid(), who, sizeof(who));
	}
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
		if (open_logged < 12) {
			open_logged++;
			say("mic open refused: pid %d (%s) err %d\n",
			    proc_selfpid(), who, mic_capture_error);
		}
		lck_mtx_unlock(&amic_lock);
		return EIO;
	}
	mic_open_clients++;
	if (open_logged < 12) {
		open_logged++;
		say("mic open: pid %d (%s) clients %u\n",
		    proc_selfpid(), who, mic_open_clients);
	}
	lck_mtx_unlock(&amic_lock);
	thread_wakeup((event_t)&mic_open_clients);
	return 0;
}

static int
mic_cdev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	static int close_logged;
	char who[32] = "?";

	if (p != NULL) {
		proc_name(proc_selfpid(), who, sizeof(who));
	}
	lck_mtx_lock(&amic_lock);
	if (mic_open_clients > 0) {
		mic_open_clients--;
	}
	if (close_logged < 12) {
		close_logged++;
		say("mic close: pid %d (%s) clients %u rec %d\n",
		    proc_selfpid(), who,
		    mic_open_clients, annibale_audio_recording_mic() ? 1 : 0);
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
	if (PE_parse_boot_argn("mic_intf", &mic_boot, sizeof(mic_boot))) {
		mic_intf_index = mic_boot;
		say("codec-DMA interface index forced to %u (mic_intf)\n", mic_intf_index);
	}
	if (PE_parse_boot_argn("mic_lpaif", &mic_boot, sizeof(mic_boot))) {
		mic_lpaif_type = mic_boot;
		say("codec-DMA LPAIF type forced to %u (mic_lpaif)\n", mic_lpaif_type);
	}
	if (PE_parse_boot_argn("mic_v41", &mic_boot, sizeof(mic_boot))) {
		mic_v41 = mic_boot;
		say("VA macro v4.1 register sequence forced to %u (mic_v41)\n", mic_v41);
	}
	if (PE_parse_boot_argn("mic_dmic", &mic_boot, sizeof(mic_boot))) {
		mic_dmic_sel = mic_boot;
		say("TX DEC1 input-mux index forced to %u (mic_dmic)\n", mic_dmic_sel);
	}
	if (PE_parse_boot_argn("mic_pmode", &mic_boot, sizeof(mic_boot))) {
		mic_pmode = mic_boot;
		say("codec-DMA power mode forced to %u (mic_pmode)\n", mic_pmode);
	}
	if (PE_parse_boot_argn("mic_noalign", &mic_boot, sizeof(mic_boot))) {
		mic_noalign = mic_boot;
		say("codec-DMA DMA_DATA_ALIGN param %s (mic_noalign)\n", mic_noalign ? "omitted" : "kept");
	}
	if (PE_parse_boot_argn("mic_sweep", &mic_boot, sizeof(mic_boot))) {
		mic_sweep = mic_boot;
		say("codec-DMA binding sweep %s (mic_sweep)\n", mic_sweep ? "enabled" : "disabled");
	}
	if (PE_parse_boot_argn("mic_combo", &mic_boot, sizeof(mic_boot))) {
		mic_combo_idx = mic_boot;
		say("codec-DMA sweep start combo %u (mic_combo)\n", mic_combo_idx);
	}

	thread_t th;
	if (kernel_thread_start(mic_capture_thread, NULL, &th) == KERN_SUCCESS) {
		thread_set_thread_name(th, "annibale-mic-feeder");
		thread_deallocate(th);
	}
}

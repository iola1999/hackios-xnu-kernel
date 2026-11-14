/*
 * Microphone capture driver for REDMI K90 (annibale):
 * Implements the SM8750 WCD9395 TX / SoundWire / LPASS VA & TX Macro / AudioReach capture path:
 *   - WCD9395 codec power rails via RPMh (ldob15 1800 mV, ldoi2 1200 mV, bobb1 on) & reset GPIO101
 *   - LPASS TX Macro (0x06ae0000), VA Macro (0x07660000), TX SoundWire Master (0x07630000),
 *     and LPI TLMM (0x07760000) pins LPI_GPIO 3/4/5/15 when ADSP LPIMX clocks are active
 *   - AudioReach capture subgraphs:
 *       SG_MIC (0x4007):     MOD_CODEC_DMA_SOURCE (0x07001024, IID 0x6071) on LPASS TX0
 *       SG_CAPTURE (0x4003): MOD_PCM_CNV (0x07001003, IID 0x6023) ->
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
extern bool annibale_rpmh_regulator(const char *name, uint32_t mv, bool enable, uint32_t mode);
extern bool annibale_audio_reach_dma_info(volatile uint8_t **va_out, uint64_t *pa_out,
    uint64_t *dsp_addr_out, uint32_t *map_handle_out);
extern bool annibale_audio_reach_send_cmd(const void *raw_pkt, uint32_t raw_len, const char *descr,
    uint32_t *rsp_opcode, uint32_t *rsp_words, uint32_t max_words);

#define say(...) printf("dmic: " __VA_ARGS__)

/* Hardware physical addresses on SM8750 (annibale) */
#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x00150000UL
#define PIN_WCD_RST             101u

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
#define OP_SET_CFG              0x01001006u
#define OP_READ_BUF             0x0400100bu
#define OP_READ_BUF_DONE        0x05001005u
#define OP_REGISTER_RES         0x0100102bu

#define MOD_RD_SHMEM            0x07001001u
#define MOD_PCM_CNV             0x07001003u
#define MOD_PCM_ENC             0x07001004u
#define MOD_CODEC_DMA_SOURCE    0x07001024u

#define SG_CAPTURE              0x4003u
#define SG_MIC                  0x4007u

#define IID_CAP_SHMEM           0x6021u
#define IID_CAP_ENC             0x6022u
#define IID_CAP_CNV             0x6023u
#define IID_MIC_SOURCE          0x6071u

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
static bool mic_graphs_started;
static int mic_capture_error;
static bool mic_wcd_powered;
static uint32_t mic_token = 500;
static uint64_t last_blocking_read_ns;

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

	uint32_t at = mpkt_param_begin(p, APM_IID, 0x08001001);
	mpkt_put32(p, 1);
	mpkt_put32(p, sg);
	mpkt_put32(p, 3);
	mpkt_put32(p, 0x0800100e); mpkt_put32(p, 4); mpkt_put32(p, 2);
	mpkt_put32(p, 0x0800100f); mpkt_put32(p, 4); mpkt_put32(p, dir);
	mpkt_put32(p, 0x08001010); mpkt_put32(p, 4); mpkt_put32(p, 1);
	mpkt_param_end(p, at);

	at = mpkt_param_begin(p, APM_IID, 0x08001000);
	mpkt_put32(p, 1);
	mpkt_put32(p, sg);
	mpkt_put32(p, 4);
	mpkt_put32(p, 0x08001011); mpkt_put32(p, 8); mpkt_put32(p, 1); mpkt_put32(p, 0x0b001001);
	mpkt_put32(p, 0x08001012); mpkt_put32(p, 4); mpkt_put32(p, 1);
	mpkt_put32(p, 0x08001013); mpkt_put32(p, 4); mpkt_put32(p, 8192);
	mpkt_put32(p, 0x08001014); mpkt_put32(p, 4); mpkt_put32(p, 2);
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

/* ---- WCD9395 Codec Power, Reset & AudioReach Capture Graph Setup ----------------------------- */

static bool
wcd9395_power_and_reset(void)
{
	if (mic_wcd_powered) {
		return true;
	}
	if (!annibale_adsp_is_hw_up()) return false;
	/* Enable WCD9395 RPMh power supplies (ldob15 1.8V, ldoi2 1.2V, bobb1 BuckBoost) */
	if (!annibale_rpmh_regulator("ldob15", 1800, true, 7) ||
	    !annibale_rpmh_regulator("ldoi2", 1200, true, 7) ||
	    !annibale_rpmh_regulator("bobb1", 0, true, 0)) {
		say("WCD9395 power rail setup failed\n");
		return false;
	}

	if (tlmm_base == 0) {
		tlmm_base = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	if (tlmm_base == 0) {
		say("WCD9395 reset GPIO mapping failed\n");
		return false;
	}
	{
		vm_offset_t cfg_reg = tlmm_base + 0x1000UL * PIN_WCD_RST;
		vm_offset_t io_reg = cfg_reg + 4UL;
		uint32_t cfg = *(volatile uint32_t *)cfg_reg;
		cfg = (cfg & ~0x3ffu) | (1u << 9) | (2u << 6);
		if (cfg & (1u << 11)) {
			cfg |= (1u << 12);
		}
		*(volatile uint32_t *)io_reg = 0;
		*(volatile uint32_t *)cfg_reg = cfg;
		__builtin_arm_dsb(0xf);
		delay_for_interval(2, NSEC_PER_MSEC);
		*(volatile uint32_t *)io_reg = 2u;
		__builtin_arm_dsb(0xf);
		delay_for_interval(10, NSEC_PER_MSEC);
	}

	/* If ADSP hardware is active, vote LPASS core & LPIMX root clock (0x201 @ 19.2 MHz) */
	if (annibale_adsp_is_hw_up()) {
		struct mic_pkt p;
		mpkt_begin(&p, OUR_MIC_PORT, PRM_IID, OP_REGISTER_RES);
		uint32_t cmd = mpkt_cmd_begin(&p);
		uint32_t at = mpkt_param_begin(&p, PRM_IID, 0x08001029);
		mpkt_put32(&p, 1);
		mpkt_put32(&p, 13); /* LPASS core */
		mpkt_param_end(&p, at);
		mpkt_cmd_end(&p, cmd);
		mpkt_finish(&p);
		if (!annibale_audio_reach_send_cmd(p.buf, p.len, "REQ LPASS CORE", NULL, NULL, 0)) {
			say("LPASS core resource request failed\n");
			return false;
		}

		vm_offset_t lpi_tlmm = ml_io_map(LPI_TLMM_PHYS, LPI_TLMM_SIZE);
		if (lpi_tlmm == 0) {
			say("LPI TLMM mapping failed\n");
			return false;
		}
		{
			/* LPI GPIO 3 (TX SWR CLK), 4 (TX SWR DATA0), 5 (TX SWR DATA1), 15 (TX SWR DATA2) func 1 */
			static const unsigned swr_pins[] = { 3, 4, 5, 15 };
			for (unsigned i = 0; i < 4; i++) {
				vm_offset_t reg = lpi_tlmm + 0x1000UL * swr_pins[i];
				uint32_t v = *(volatile uint32_t *)reg;
				v = (v & ~0x3ffu) | (1u << 2) | (2u << 6) | 1u;
				*(volatile uint32_t *)reg = v;
			}
			__builtin_arm_dsb(0xf);
		}
	}

	mic_wcd_powered = true;
	say("WCD9395 supplies enabled and reset GPIO101 released\n");
	return true;
}

static bool
start_capture_graphs(void)
{
	if (mic_graphs_started) {
		return true;
	}
	if (!wcd9395_power_and_reset()) return false;

	static const struct mic_mod mic_mods[] = {
		{ MOD_CODEC_DMA_SOURCE, IID_MIC_SOURCE, 0, 1 }
	};
	static const struct mic_mod cap_mods[] = {
		{ MOD_RD_SHMEM, IID_CAP_SHMEM, 1, 0 },
		{ MOD_PCM_ENC,  IID_CAP_ENC,   1, 1 },
		{ MOD_PCM_CNV,  IID_CAP_CNV,   1, 1 }
	};
	static const struct mic_link cap_links[] = {
		{ IID_MIC_SOURCE, IID_CAP_CNV },
		{ IID_CAP_CNV,    IID_CAP_ENC },
		{ IID_CAP_ENC,    IID_CAP_SHMEM }
	};

	struct mic_pkt p;

	/* 1. Open SG_MIC (0x4007) */
	mpkt_graph_open(&p, SG_MIC, 2, mic_mods, 1, NULL, 0);
	if (!annibale_audio_reach_send_cmd(p.buf, p.len, "OPEN SG_MIC (0x4007)", NULL, NULL, 0)) return false;

	/* 2. Configure CODEC DMA Source (TX0, 48 kHz 16-bit mono) */
	mpkt_begin(&p, OUR_MIC_PORT, APM_IID, OP_SET_CFG);
	uint32_t cmd = mpkt_cmd_begin(&p);
	uint32_t at = mpkt_param_begin(&p, IID_MIC_SOURCE, 0x08001063); /* CODEC_DMA_INTF_CFG */
	mpkt_put32(&p, 3); /* lpaif_type = LPASS_TX */
	mpkt_put32(&p, 1); /* intf_idx = TX0 */
	mpkt_put32(&p, 1); /* active_channels_mask = ch 0 */
	mpkt_param_end(&p, at);
	at = mpkt_param_begin(&p, IID_MIC_SOURCE, 0x08001017); /* HW_EP_MF */
	mpkt_put32(&p, MIC_RATE);
	mpkt_put16(&p, 16);
	mpkt_put16(&p, 1);
	mpkt_put32(&p, 1);
	mpkt_param_end(&p, at);
	mpkt_cmd_end(&p, cmd);
	mpkt_finish(&p);
	if (!annibale_audio_reach_send_cmd(p.buf, p.len, "SET_CFG MIC_SOURCE", NULL, NULL, 0)) return false;

	/* 3. Open SG_CAPTURE (0x4003) */
	mpkt_graph_open(&p, SG_CAPTURE, 1, cap_mods, 3, cap_links, 3);
	if (!annibale_audio_reach_send_cmd(p.buf, p.len, "OPEN SG_CAPTURE (0x4003)", NULL, NULL, 0)) return false;

	/* 4. Configure PCM converter & encoder for 48 kHz 16-bit mono */
	mpkt_begin(&p, OUR_MIC_PORT, APM_IID, OP_SET_CFG);
	cmd = mpkt_cmd_begin(&p);
	at = mpkt_param_begin(&p, IID_CAP_ENC, 0x08001006);
	mpkt_put32(&p, 0); mpkt_put32(&p, 0);
	mpkt_put32(&p, 0x09001000); mpkt_put32(&p, 20);
	mpkt_put32(&p, MIC_RATE);
	mpkt_put16(&p, 16); mpkt_put16(&p, 16); mpkt_put16(&p, 15);
	mpkt_put16(&p, 1);  mpkt_put16(&p, 1);  mpkt_put16(&p, 0);
	mpkt_put16(&p, 1);  mpkt_put8(&p, 1);   mpkt_put8(&p, 0);
	mpkt_param_end(&p, at);
	mpkt_cmd_end(&p, cmd);
	mpkt_finish(&p);
	if (!annibale_audio_reach_send_cmd(p.buf, p.len, "SET_CFG CAP_ENC", NULL, NULL, 0)) return false;

	/* 5. Prepare & Start SG_MIC and SG_CAPTURE */
	if (!mic_graph_cmd(OP_GRAPH_PREPARE, SG_MIC, "PREPARE SG_MIC") ||
	    !mic_graph_cmd(OP_GRAPH_START, SG_MIC, "START SG_MIC") ||
	    !mic_graph_cmd(OP_GRAPH_PREPARE, SG_CAPTURE, "PREPARE SG_CAPTURE") ||
	    !mic_graph_cmd(OP_GRAPH_START, SG_CAPTURE, "START SG_CAPTURE")) return false;

	mic_graphs_started = true;
	say("AudioReach capture graphs SG_MIC (0x4007) & SG_CAPTURE (0x4003) started\n");
	return true;
}

static bool
submit_capture_slot(uint32_t slot, uint64_t dsp_base, uint32_t map_handle, uint32_t *data_bytes_out)
{
	struct mic_pkt p;
	uint64_t slot_addr = dsp_base + MIC_OFFSET + (uint64_t)slot * MIC_PERIOD_BYTES;
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
	uint32_t opcode = 0, words[16] = { 0 };
	if (!annibale_audio_reach_send_cmd(p.buf, p.len, "READ (MIC_PERIOD)", &opcode, words, 16) ||
	    opcode != OP_READ_BUF_DONE || words[0] != 0 || words[1] != (uint32_t)slot_addr ||
	    words[2] != (uint32_t)(slot_addr >> 32) || words[3] != map_handle ||
	    words[4] > MIC_PERIOD_BYTES || (words[4] % sizeof(int16_t)) != 0 ||
	    words[9] != 0 || words[13] != 0) {
		say("capture completion failed for slot %u (opcode %#x)\n", slot, opcode);
		return false;
	}
	if (data_bytes_out != NULL) *data_bytes_out = words[4];
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
		}
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

__dead2
static void
mic_capture_thread(void *arg __unused, wait_result_t w __unused)
{
	uint32_t slot = 0;

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
			lck_mtx_unlock(&amic_lock);

			volatile uint8_t *dma_va = NULL;
			uint64_t dma_pa = 0, dsp_addr = 0;
			uint32_t map_handle = 0, data_bytes = 0;
			if (annibale_audio_reach_dma_info(&dma_va, &dma_pa, &dsp_addr, &map_handle) &&
			    submit_capture_slot(slot % MIC_SLOTS, dsp_addr, map_handle, &data_bytes)) {
				harvest_capture_period(slot % MIC_SLOTS, dma_va, data_bytes);
			} else {
				lck_mtx_lock(&amic_lock);
				mic_capture_error = EIO;
				lck_mtx_unlock(&amic_lock);
				say("capture DMA completion failed; capture stopped\n");
			}
			slot++;
		} else {
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
	 * For O_NONBLOCK callers (audiomxd CoreAudio IO thread), return requested frames
	 * immediately because CoreAudio's IO cycle already paces itself at 48,000 frames/s.
	 */
	user_ssize_t total_req_bytes = uio_resid(uio);
	if (!(ioflag & IO_NDELAY) && total_req_bytes >= 2) {
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
		uint32_t count = (uint32_t)(bytes / (user_ssize_t)sizeof(int16_t));

		lck_spin_lock(amic_ring_lock);
		for (uint32_t i = 0; i < count; i++) {
			if (mic_ring_tail != mic_ring_head) {
				chunk[i] = mic_ring[mic_ring_tail & MIC_RING_MASK];
				mic_ring_tail++;
			} else {
				chunk[i] = 0;
			}
		}
		lck_spin_unlock(amic_ring_lock);

		error = uiomove((caddr_t)chunk, (int)bytes, uio);
		if (error != 0) {
			break;
		}
	}
	return error;
}

static int
mic_cdev_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	lck_mtx_lock(&amic_lock);
	if (mic_ring == NULL || mic_capture_error != 0 || (!mic_graphs_started && !start_capture_graphs())) {
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
	lck_mtx_unlock(&amic_lock);
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
	say("initializing WCD9395 / SoundWire / AudioReach microphone subsystem...\n");

	if (amic_ring_lock == NULL) {
		amic_ring_lock = lck_spin_alloc_init(&amic_lck_grp, LCK_ATTR_NULL);
	}
	if (mic_ring == NULL) {
		mic_ring = kalloc_data(MIC_RING_SAMPLES * sizeof(int16_t), Z_WAITOK | Z_ZERO);
		if (mic_ring != NULL) {
			/* Keep an initial silent period available to non-blocking readers. */
			for (uint32_t i = 0; i < 2 * MIC_PERIOD_FRAMES; i++) {
				mic_ring[mic_ring_head & MIC_RING_MASK] = 0;
				mic_ring_head++;
			}
		}
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

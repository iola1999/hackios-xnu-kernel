/*
 * ADSP SMEM GLINK "fastrpcglink-apps-dsp" Stage 1 read-only probe.
 *
 * Matches sm8750-xiaomi-annibale.dtb (fastrpc -> adsp):
 *   label = "adsp";
 *   qcom,glink-channels = "fastrpcglink-apps-dsp";
 *   qcom,intents = <0x181 0x40>;
 *
 * Included by annibale_adsp.c after annibale_adsp_ipcrtr.inc and before glink_poll_rx().
 * Opens the channel, queues minimal RX intents, and bounded-logs the first 16 incoming
 * ADSP messages (length + first 16-byte header as 4 little-endian u32 words).
 */

#define ADSP_FASTRPC_CHAN         "fastrpcglink-apps-dsp"
#define ADSP_FASTRPC_OUR_CID      5u
#define ADSP_FASTRPC_INTENT_SIZE  0x200u
#define ADSP_FASTRPC_INTENT_COUNT 4u
#define ADSP_FASTRPC_REASM_MAX    0x200u
#define ADSP_FASTRPC_LOG_MAX      16u
#define ADSP_FASTRPC_INTENT_LOG_MAX 8u

static uint16_t adsp_fastrpc_our_cid, adsp_fastrpc_their_cid;
static bool adsp_fastrpc_our_ack, adsp_fastrpc_ready;
static uint32_t adsp_fastrpc_rx_count, adsp_fastrpc_intent_logs;

struct adsp_fastrpc_reasm {
	uint16_t cid;
	uint32_t total_len;
	uint32_t raw_total;
	uint32_t assembled_len;
	bool     in_progress;
	bool     oversize;
	uint8_t  buf[ADSP_FASTRPC_REASM_MAX];
};

static struct adsp_fastrpc_reasm adsp_fastrpc_rx;

static void
adsp_fastrpc_queue_intents(uint16_t cid)
{
	for (uint32_t i = 0; i < ADSP_FASTRPC_INTENT_COUNT; i++) {
		uint32_t pkt[4] = {
			GLINK_INTENT | ((uint32_t)cid << 16),
			1u,
			ADSP_FASTRPC_INTENT_SIZE,
			i + 1u,
		};
		glink_q_write(glink_our_q, glink_our_q_size, &glink_tx_write_pos, pkt, sizeof(pkt));
	}
}

static void
adsp_fastrpc_reset(void)
{
	adsp_fastrpc_our_cid = 0;
	adsp_fastrpc_their_cid = 0;
	adsp_fastrpc_our_ack = false;
	adsp_fastrpc_ready = false;
	adsp_fastrpc_rx_count = 0;
	adsp_fastrpc_intent_logs = 0;
	bzero(&adsp_fastrpc_rx, sizeof(adsp_fastrpc_rx));
}

static void
adsp_fastrpc_request_open(void)
{
	if (adsp_fastrpc_our_cid == 0) {
		adsp_fastrpc_our_cid = ADSP_FASTRPC_OUR_CID;
		glink_send_open(adsp_fastrpc_our_cid, ADSP_FASTRPC_CHAN);
	}
}

static void
adsp_fastrpc_note_ready(void)
{
	if (adsp_fastrpc_our_ack && adsp_fastrpc_their_cid != 0 && !adsp_fastrpc_ready) {
		adsp_fastrpc_ready = true;
		adsp_fastrpc_queue_intents(adsp_fastrpc_our_cid);
		say("fastrpc: channel %s up (lcid=%u rcid=%u, intents=%ux%uB)\n",
		    ADSP_FASTRPC_CHAN, adsp_fastrpc_our_cid, adsp_fastrpc_their_cid,
		    ADSP_FASTRPC_INTENT_COUNT, ADSP_FASTRPC_INTENT_SIZE);
	}
}

static void
adsp_fastrpc_on_open(const char *name, uint16_t their_cid)
{
	if (strcmp(name, ADSP_FASTRPC_CHAN) == 0) {
		adsp_fastrpc_their_cid = their_cid;
		adsp_fastrpc_request_open();
		adsp_fastrpc_note_ready();
	}
}

static void
adsp_fastrpc_on_open_ack(uint16_t cid)
{
	if (cid != 0 && cid == adsp_fastrpc_our_cid) {
		adsp_fastrpc_our_ack = true;
		adsp_fastrpc_note_ready();
	}
}

static void
adsp_fastrpc_on_intent_req(uint16_t their_cid)
{
	if (their_cid != 0 && their_cid == adsp_fastrpc_their_cid && adsp_fastrpc_our_cid != 0) {
		adsp_fastrpc_queue_intents(adsp_fastrpc_our_cid);
	}
}

static void
adsp_fastrpc_on_intent(uint16_t their_cid, uint32_t iid, uint32_t sz)
{
	if (their_cid != 0 && their_cid == adsp_fastrpc_their_cid &&
	    adsp_fastrpc_intent_logs < ADSP_FASTRPC_INTENT_LOG_MAX) {
		adsp_fastrpc_intent_logs++;
		say("fastrpc: remote intent #%u id=%u size=%u\n", adsp_fastrpc_intent_logs, iid, sz);
	}
}

static void
adsp_fastrpc_log_msg(const uint8_t *buf, uint32_t len, bool oversize, uint32_t raw_total)
{
	adsp_fastrpc_rx_count++;
	if (adsp_fastrpc_rx_count > ADSP_FASTRPC_LOG_MAX) {
		return;
	}
	if (oversize) {
		say("fastrpc: rx #%u oversize total=%u\n", adsp_fastrpc_rx_count, raw_total);
		return;
	}
	uint32_t w[4] = { 0, 0, 0, 0 };
	uint32_t copy_len = len < sizeof(w) ? len : (uint32_t)sizeof(w);
	if (copy_len > 0) {
		memcpy(w, buf, copy_len);
	}
	say("fastrpc: rx #%u len=%u hdr=%#x %#x %#x %#x\n",
	    adsp_fastrpc_rx_count, len, w[0], w[1], w[2], w[3]);
}

static bool
adsp_fastrpc_rx_chunk(uint16_t cmd, uint16_t param, uint32_t q_off, uint32_t q_sz, uint32_t r,
    uint32_t chunk, uint32_t left)
{
	if (param == 0 || param != adsp_fastrpc_their_cid) {
		return false;
	}
	if (cmd == GLINK_TX_DATA) {
		uint64_t total = (uint64_t)chunk + (uint64_t)left;
		adsp_fastrpc_rx.in_progress = true;
		adsp_fastrpc_rx.cid = param;
		adsp_fastrpc_rx.raw_total = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
		if (total > ADSP_FASTRPC_REASM_MAX || total == 0) {
			adsp_fastrpc_rx.oversize = true;
			adsp_fastrpc_rx.total_len = 0;
			adsp_fastrpc_rx.assembled_len = 0;
		} else {
			adsp_fastrpc_rx.oversize = false;
			adsp_fastrpc_rx.total_len = (uint32_t)total;
			for (uint32_t i = 0; i < chunk; i++) {
				smem_peek(q_off + (((uint64_t)r + 16ULL + i) % q_sz),
				    &adsp_fastrpc_rx.buf[i], 1);
			}
			adsp_fastrpc_rx.assembled_len = chunk;
		}
	} else {
		if (!adsp_fastrpc_rx.in_progress || adsp_fastrpc_rx.cid != param) {
			return true;
		}
		if (!adsp_fastrpc_rx.oversize) {
			if ((uint64_t)adsp_fastrpc_rx.assembled_len + (uint64_t)chunk > (uint64_t)adsp_fastrpc_rx.total_len ||
			    (uint64_t)adsp_fastrpc_rx.assembled_len + (uint64_t)chunk > ADSP_FASTRPC_REASM_MAX) {
				adsp_fastrpc_rx.oversize = true;
			} else {
				for (uint32_t i = 0; i < chunk; i++) {
					smem_peek(q_off + (((uint64_t)r + 16ULL + i) % q_sz),
					    &adsp_fastrpc_rx.buf[adsp_fastrpc_rx.assembled_len + i], 1);
				}
				adsp_fastrpc_rx.assembled_len += chunk;
			}
		}
	}

	if (left == 0) {
		if (adsp_fastrpc_rx.oversize) {
			adsp_fastrpc_log_msg(NULL, 0, true, adsp_fastrpc_rx.raw_total);
		} else if (adsp_fastrpc_rx.assembled_len == adsp_fastrpc_rx.total_len &&
		    adsp_fastrpc_rx.total_len > 0) {
			adsp_fastrpc_log_msg(adsp_fastrpc_rx.buf, adsp_fastrpc_rx.total_len, false,
			    adsp_fastrpc_rx.total_len);
		}
		adsp_fastrpc_rx.in_progress = false;
		adsp_fastrpc_rx.oversize = false;
	}
	return true;
}

static uint16_t
adsp_glink_our_cid_for(uint16_t their_cid)
{
	if (their_cid == 0) return 0;
	if (their_cid == adsp_ipcrtr_their_cid) return adsp_ipcrtr_our_cid;
	if (their_cid == adsp_fastrpc_their_cid) return adsp_fastrpc_our_cid;
	if (their_cid == glink_pmic_their_cid) return glink_pmic_our_cid;
	if (their_cid == glink_gpr_their_cid) return glink_gpr_our_cid;
	return 0;
}

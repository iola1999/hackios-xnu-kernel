/*
 * ADSP SMEM GLINK "IPCRTR" transport -> annibale_qrtr (node 2).
 *
 * Matches sm8750-xiaomi-annibale.dtb (qcom,adsp_qrtr):
 *   qcom,glink-channels = "IPCRTR";
 *   qcom,net-id = <0x02>;
 *   qcom,intents = <0x800 0x05 0x2000 0x03 0x4400 0x02>;
 *   qcom,no-wake-svc = <0x190>;   (QMI SNS_CLIENT)
 *
 * Included by annibale_adsp.c after glink_take_their_intent() and before glink_poll_rx().
 * Completed QRTR packets and node attachment are handed to annibale_qrtr outside glink_rx_lock
 * because annibale_qrtr_input() may reply via adsp_qrtr_send() -> glink_take_their_intent() ->
 * glink_poll_rx().
 */

#define QRTR_NODE_ADSP          5u      /* b5r30 s001.log: ADSP sends HELLO FROM NODE 5 (DTB net-id=2 is link id) */
#define ADSP_IPCRTR_OUR_CID     4u
#define ADSP_IPCRTR_REASM_MAX   0x4400u /* 17 KiB, matches DTB qcom,intents largest class */

extern void annibale_qrtr_attach_node(uint32_t peer_node, int (*send)(const void *data, uint32_t length));
extern void annibale_qrtr_input(const void *data, uint32_t length);

static uint16_t adsp_ipcrtr_our_cid, adsp_ipcrtr_their_cid;
static bool adsp_ipcrtr_our_ack, adsp_ipcrtr_attached;
static struct glink_intent_pool adsp_ipcrtr_intents;

struct adsp_ipcrtr_reasm {
	uint16_t cid;
	uint32_t total_len;
	uint32_t assembled_len;
	bool     in_progress;
	bool     oversize;
	uint8_t  buf[ADSP_IPCRTR_REASM_MAX];
};

static struct adsp_ipcrtr_reasm adsp_ipcrtr_rx;

static void
adsp_ipcrtr_queue_intents(uint16_t cid)
{
	static const uint32_t sizes[10] = {
		0x800u, 0x800u, 0x800u, 0x800u, 0x800u,
		0x2000u, 0x2000u, 0x2000u,
		0x4400u, 0x4400u,
	};

	for (uint32_t i = 0; i < 10u; i++) {
		uint32_t pkt[4] = { GLINK_INTENT | ((uint32_t)cid << 16), 1u, sizes[i], i + 1u };
		glink_q_write(glink_our_q, glink_our_q_size, &glink_tx_write_pos, pkt, sizeof(pkt));
	}
}

static void
adsp_ipcrtr_reset(void)
{
	adsp_ipcrtr_our_cid = 0;
	adsp_ipcrtr_their_cid = 0;
	adsp_ipcrtr_our_ack = false;
	adsp_ipcrtr_attached = false;
	bzero(&adsp_ipcrtr_rx, sizeof(adsp_ipcrtr_rx));
}

static void
adsp_ipcrtr_request_open(void)
{
	if (adsp_ipcrtr_our_cid == 0) {
		adsp_ipcrtr_our_cid = ADSP_IPCRTR_OUR_CID;
		glink_send_open(adsp_ipcrtr_our_cid, "IPCRTR");
	}
}

/* Returns true when the channel has just finished bidirectional open and needs QRTR attachment. */
static bool
adsp_ipcrtr_note_ready(void)
{
	if (adsp_ipcrtr_our_ack && adsp_ipcrtr_their_cid != 0 && !adsp_ipcrtr_attached) {
		adsp_ipcrtr_attached = true;
		adsp_ipcrtr_queue_intents(adsp_ipcrtr_our_cid);
		return true;
	}
	return false;
}

static int
adsp_qrtr_send(const void *data, uint32_t length)
{
	uint32_t iid = 0;
	uint8_t *tx;
	uint32_t w0, left = 0;
	bool sent;

	if (!adsp_hw_up || glink_our_q == 0 || adsp_ipcrtr_our_cid == 0 ||
	    adsp_ipcrtr_their_cid == 0 || data == NULL || length == 0) {
		return ENOTCONN;
	}
	if (16u + length >= glink_our_q_size - 16u) {
		return EMSGSIZE;
	}
	if (!glink_take_their_intent(&adsp_ipcrtr_intents, adsp_ipcrtr_our_cid, length, &iid)) {
		return EAGAIN;
	}
	tx = (uint8_t *)kalloc_data(16u + length, Z_WAITOK);
	if (tx == NULL) {
		glink_complete_their_intent(&adsp_ipcrtr_intents, iid, true);
		return ENOMEM;
	}
	w0 = GLINK_TX_DATA | ((uint32_t)adsp_ipcrtr_our_cid << 16);
	memcpy(tx + 0, &w0, 4);
	memcpy(tx + 4, &iid, 4);
	memcpy(tx + 8, &length, 4);
	memcpy(tx + 12, &left, 4);
	memcpy(tx + 16, data, length);
	sent = glink_q_write(glink_our_q, glink_our_q_size, &glink_tx_write_pos, tx, 16u + length);
	kfree_data(tx, 16u + length);
	if (!sent) {
		glink_complete_their_intent(&adsp_ipcrtr_intents, iid, true);
		return EIO;
	}
	return 0;
}

/*
 * Feed one GLINK_TX_DATA / GLINK_TX_DATA_CONT chunk for IPCRTR under glink_rx_lock.
 * On completion (left == 0), returns a heap-copied packet in *out_buf / *out_len for
 * lock-free delivery to annibale_qrtr_input().
 */
static void
adsp_ipcrtr_rx_chunk(uint16_t cmd, uint16_t param, uint32_t q_off, uint32_t q_sz, uint32_t r,
    uint32_t chunk, uint32_t left, uint8_t **out_buf, uint32_t *out_len)
{
	if (cmd == GLINK_TX_DATA) {
		uint64_t total = (uint64_t)chunk + (uint64_t)left;
		if (total > ADSP_IPCRTR_REASM_MAX || total == 0) {
			adsp_ipcrtr_rx.in_progress = true;
			adsp_ipcrtr_rx.oversize = true;
			adsp_ipcrtr_rx.cid = param;
			adsp_ipcrtr_rx.total_len = 0;
			adsp_ipcrtr_rx.assembled_len = 0;
			return;
		}
		adsp_ipcrtr_rx.in_progress = true;
		adsp_ipcrtr_rx.oversize = false;
		adsp_ipcrtr_rx.cid = param;
		adsp_ipcrtr_rx.total_len = (uint32_t)total;
		for (uint32_t i = 0; i < chunk; i++) {
			smem_peek(q_off + (((uint64_t)r + 16ULL + i) % q_sz), &adsp_ipcrtr_rx.buf[i], 1);
		}
		adsp_ipcrtr_rx.assembled_len = chunk;
	} else {
		if (!adsp_ipcrtr_rx.in_progress || adsp_ipcrtr_rx.cid != param) {
			return;
		}
		if (adsp_ipcrtr_rx.oversize) {
			if (left == 0) {
				adsp_ipcrtr_rx.in_progress = false;
				adsp_ipcrtr_rx.oversize = false;
			}
			return;
		}
		if ((uint64_t)adsp_ipcrtr_rx.assembled_len + (uint64_t)chunk > (uint64_t)adsp_ipcrtr_rx.total_len ||
		    (uint64_t)adsp_ipcrtr_rx.assembled_len + (uint64_t)chunk > ADSP_IPCRTR_REASM_MAX) {
			adsp_ipcrtr_rx.oversize = true;
			if (left == 0) {
				adsp_ipcrtr_rx.in_progress = false;
				adsp_ipcrtr_rx.oversize = false;
			}
			return;
		}
		for (uint32_t i = 0; i < chunk; i++) {
			smem_peek(q_off + (((uint64_t)r + 16ULL + i) % q_sz),
			    &adsp_ipcrtr_rx.buf[adsp_ipcrtr_rx.assembled_len + i], 1);
		}
		adsp_ipcrtr_rx.assembled_len += chunk;
	}

	if (left == 0) {
		if (adsp_ipcrtr_rx.assembled_len == adsp_ipcrtr_rx.total_len && adsp_ipcrtr_rx.total_len > 0) {
			uint8_t *copy = (uint8_t *)kalloc_data(adsp_ipcrtr_rx.total_len, Z_WAITOK);
			if (copy != NULL) {
				memcpy(copy, adsp_ipcrtr_rx.buf, adsp_ipcrtr_rx.total_len);
				*out_buf = copy;
				*out_len = adsp_ipcrtr_rx.total_len;
			}
		}
		adsp_ipcrtr_rx.in_progress = false;
	}
}

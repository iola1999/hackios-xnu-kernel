/*
 * ADSP SMEM GLINK "fastrpcglink-apps-dsp" Stage 2 transport, session bringup & apps_std listener.
 *
 * Matches sm8750-xiaomi-annibale.dts (qcom,fastrpc -> adsp):
 *   label = "adsp";
 *   qcom,glink-channels = "fastrpcglink-apps-dsp";
 *   qcom,intents = <0x181 0x40>;
 *   memory-region = <&adsp_mem_heap>; (0xc00000 12 MiB)
 *   compute-cb@2: pd-type = <3>, reg = <4>, iommus = <0x1004/0x80, 0x1044/0x20>
 *
 * Included by annibale_adsp.c after annibale_adsp_ipcrtr.inc and before glink_poll_rx().
 */

#include "annibale_adsp_fastrpc_smmu.inc"
#include "annibale_adsp_fastrpc_rpc.inc"

#define ADSP_FASTRPC_CHAN         "fastrpcglink-apps-dsp"
#define ADSP_FASTRPC_OUR_CID      5u
#define ADSP_FASTRPC_INTENT_SIZE  0x0400u
#define ADSP_FASTRPC_INTENT_COUNT 8u
#define ADSP_FASTRPC_REASM_MAX    4096u
#define ADSP_FASTRPC_RX_RING_SZ   4u
#define ADSP_FASTRPC_LOG_MAX      16u
#define ADSP_FASTRPC_INTENT_LOG_MAX 8u
#define ADSP_FASTRPC_INIT_RETRIES 3u
#define ADSP_FASTRPC_CTX_TIMEOUT_US 2000000ULL
#define ADSP_FASTRPC_REMOTE_INTENT_MIN 512u
#define ADSP_FASTRPC_INTENT_REQ_INTERVAL_US 500000ULL
#define ADSP_FASTRPC_INTENT_REQ_RETRIES 10u

enum adsp_fastrpc_stage {
	FASTRPC_STAGE_IDLE = 0,
	FASTRPC_STAGE_WAIT_INIT_STATIC,
	FASTRPC_STAGE_WAIT_INIT_SNS,
	FASTRPC_STAGE_WAIT_LISTENER_INIT2,
	FASTRPC_STAGE_WAIT_LISTENER_NEXT2,
	FASTRPC_STAGE_FAILED,
};

struct adsp_fastrpc_reasm {
	uint16_t cid;
	uint32_t total_len;
	uint32_t raw_total;
	uint32_t assembled_len;
	bool     in_progress;
	bool     oversize;
	uint8_t  buf[ADSP_FASTRPC_REASM_MAX];
};

struct adsp_fastrpc_rx_slot {
	bool     used;
	uint32_t len;
	uint8_t  buf[ADSP_FASTRPC_REASM_MAX];
};

static uint16_t adsp_fastrpc_our_cid, adsp_fastrpc_their_cid;
static bool adsp_fastrpc_our_ack, adsp_fastrpc_ready;
static uint32_t adsp_fastrpc_rx_count, adsp_fastrpc_intent_logs;
static struct glink_intent_pool adsp_fastrpc_intents;

static struct adsp_fastrpc_reasm adsp_fastrpc_rx;
static struct adsp_fastrpc_rx_slot adsp_fastrpc_rx_ring[ADSP_FASTRPC_RX_RING_SZ];
static uint32_t adsp_fastrpc_rx_head, adsp_fastrpc_rx_tail;

static enum adsp_fastrpc_stage adsp_fastrpc_stage;
static uint32_t adsp_fastrpc_ctx_seq;
static uint64_t adsp_fastrpc_inflight_ctx;
static uint64_t adsp_fastrpc_inflight_sent_us;
static uint32_t adsp_fastrpc_init_attempts;
static uint64_t adsp_fastrpc_intent_req_us;
static uint32_t adsp_fastrpc_intent_reqs;
static uint32_t adsp_fastrpc_next2_out0_off;
static uint32_t adsp_fastrpc_next2_out1_off;
static uint32_t adsp_fastrpc_next2_out1_cap;
static uint32_t adsp_fastrpc_prev_rev_ctx;
static int32_t  adsp_fastrpc_prev_rev_rc;
static uint32_t adsp_fastrpc_prev_outbufs_len;
static uint8_t  adsp_fastrpc_prev_outbufs[ADSP_FASTRPC_IO_MAX_BYTES];

#ifdef ADSP_FASTRPC_HOST_TEST
static uint64_t (*adsp_fastrpc_test_now_us)(void);
static uint64_t adsp_fastrpc_now_us(void) { return adsp_fastrpc_test_now_us ? adsp_fastrpc_test_now_us() : 0ULL; }
static void adsp_fastrpc_svc_lock(void) {}
static void adsp_fastrpc_svc_unlock(void) {}
#else
static LCK_MTX_DECLARE(adsp_fastrpc_svc_mtx, &annibale_ipc_lock_group);
static uint64_t adsp_fastrpc_now_us(void) { return now_us(); }
static void adsp_fastrpc_svc_lock(void) { lck_mtx_lock(&adsp_fastrpc_svc_mtx); }
static void adsp_fastrpc_svc_unlock(void) { lck_mtx_unlock(&adsp_fastrpc_svc_mtx); }
#endif

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
adsp_fastrpc_reset_intents(void)
{
	lck_mtx_lock(&glink_intent_lock);
	struct glink_remote_intent *cur = adsp_fastrpc_intents.head;
	adsp_fastrpc_intents.head = NULL;
	lck_mtx_unlock(&glink_intent_lock);
	while (cur != NULL) {
		struct glink_remote_intent *next = cur->next;
		kfree_type(struct glink_remote_intent, cur);
		cur = next;
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
	adsp_fastrpc_stage = FASTRPC_STAGE_IDLE;
	adsp_fastrpc_ctx_seq = 0;
	adsp_fastrpc_inflight_ctx = 0;
	adsp_fastrpc_inflight_sent_us = 0;
	adsp_fastrpc_init_attempts = 0;
	adsp_fastrpc_intent_req_us = 0;
	adsp_fastrpc_intent_reqs = 0;
	adsp_fastrpc_next2_out0_off = 0;
	adsp_fastrpc_next2_out1_off = 0;
	adsp_fastrpc_next2_out1_cap = 0;
	adsp_fastrpc_prev_rev_ctx = 0;
	adsp_fastrpc_prev_rev_rc = 0;
	adsp_fastrpc_prev_outbufs_len = 0;
	adsp_fastrpc_missing_logs = 0;
	adsp_fastrpc_open_logs = 0;
	bzero(&adsp_fastrpc_rx, sizeof(adsp_fastrpc_rx));
	bzero(adsp_fastrpc_rx_ring, sizeof(adsp_fastrpc_rx_ring));
	adsp_fastrpc_rx_head = 0;
	adsp_fastrpc_rx_tail = 0;
	adsp_fastrpc_reset_intents();
	adsp_fastrpc_close_all_fds();
	adsp_fastrpc_free_buf();
	adsp_fastrpc_smmu_teardown();
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
	if (their_cid != 0 && their_cid == adsp_fastrpc_their_cid) {
		glink_add_their_intent(&adsp_fastrpc_intents, iid, sz);
		if (adsp_fastrpc_intent_logs < ADSP_FASTRPC_INTENT_LOG_MAX) {
			adsp_fastrpc_intent_logs++;
			say("fastrpc: remote intent #%u id=%u size=%u\n", adsp_fastrpc_intent_logs, iid, sz);
		}
	}
}

static void
adsp_fastrpc_on_tx_done(uint32_t iid, bool reuse)
{
	glink_complete_their_intent(&adsp_fastrpc_intents, iid, reuse);
}

static bool
adsp_fastrpc_tx_send(const void *data, uint32_t length)
{
	if (!adsp_fastrpc_ready || glink_our_q == 0 || adsp_fastrpc_our_cid == 0 ||
	    adsp_fastrpc_their_cid == 0 || data == NULL || length == 0) {
		return false;
	}
	uint32_t iid = 0;
	if (!glink_try_take_their_intent(&adsp_fastrpc_intents, adsp_fastrpc_our_cid, length, &iid)) {
		return false;
	}
	uint32_t max_chunk = (glink_our_q_size > 128u) ? (glink_our_q_size - 64u) : length;
	if (max_chunk > 512u) {
		max_chunk = 512u;
	}
	uint32_t sent_off = 0;
	while (sent_off < length) {
		uint32_t chunk = length - sent_off;
		if (chunk > max_chunk) {
			chunk = max_chunk;
		}
		uint32_t left = length - (sent_off + chunk);
		uint16_t cmd = (sent_off == 0) ? GLINK_TX_DATA : GLINK_TX_DATA_CONT;
		uint32_t w0 = (uint32_t)cmd | ((uint32_t)adsp_fastrpc_our_cid << 16);
		uint8_t pkt[16 + 512];
		memcpy(pkt + 0, &w0, 4u);
		memcpy(pkt + 4, &iid, 4u);
		memcpy(pkt + 8, &chunk, 4u);
		memcpy(pkt + 12, &left, 4u);
		memcpy(pkt + 16, (const uint8_t *)data + sent_off, chunk);
		if (!glink_q_write(glink_our_q, glink_our_q_size, &glink_tx_write_pos, pkt, 16u + chunk)) {
			if (sent_off == 0) {
				glink_complete_their_intent(&adsp_fastrpc_intents, iid, true);
			}
			return false;
		}
		sent_off += chunk;
	}
	return true;
}

static bool
adsp_fastrpc_send_invoke(uint32_t pd, uint32_t handle, uint32_t sc, uint32_t pkt_size)
{
	struct fastrpc_wire_msg msg;
	uint64_t seq = ++adsp_fastrpc_ctx_seq;
	uint64_t ctx = (seq << 16) | 0x0100ULL | (uint64_t)(pd & 0xffu);
	uint64_t sid_hi = (uint64_t)ADSP_FASTRPC_COMPUTE_SID << 32;

	memset(&msg, 0, sizeof(msg));
	msg.client_id = 0;
	msg.tid = 1;
	msg.ctx = ctx;
	msg.handle = handle;
	msg.sc = sc;
	msg.addr = pkt_size > 0 ? (sid_hi | adsp_fastrpc_buf_pa) : 0ULL;
	msg.size = pkt_size > 0 ? adsp_fastrpc_align_up(pkt_size, ADSP_FASTRPC_PAGE_SIZE) : 0ULL;

	if (!adsp_fastrpc_tx_send(&msg, sizeof(msg))) {
		return false;
	}
	adsp_fastrpc_inflight_ctx = ctx;
	adsp_fastrpc_inflight_sent_us = adsp_fastrpc_now_us();
	return true;
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
		uint64_t total_sz = (uint64_t)chunk + (uint64_t)left;
		adsp_fastrpc_rx.in_progress = true;
		adsp_fastrpc_rx.cid = param;
		adsp_fastrpc_rx.raw_total = total_sz > UINT32_MAX ? UINT32_MAX : (uint32_t)total_sz;
		if (total_sz > ADSP_FASTRPC_REASM_MAX || total_sz == 0) {
			adsp_fastrpc_rx.oversize = true;
			adsp_fastrpc_rx.total_len = 0;
			adsp_fastrpc_rx.assembled_len = 0;
		} else {
			adsp_fastrpc_rx.oversize = false;
			adsp_fastrpc_rx.total_len = (uint32_t)total_sz;
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
			struct adsp_fastrpc_rx_slot *slot = &adsp_fastrpc_rx_ring[adsp_fastrpc_rx_tail];
			if (!slot->used) {
				memcpy(slot->buf, adsp_fastrpc_rx.buf, adsp_fastrpc_rx.total_len);
				slot->len = adsp_fastrpc_rx.total_len;
				slot->used = true;
				adsp_fastrpc_rx_tail = (adsp_fastrpc_rx_tail + 1u) % ADSP_FASTRPC_RX_RING_SZ;
			}
		}
		adsp_fastrpc_rx.in_progress = false;
		adsp_fastrpc_rx.oversize = false;
	}
	return true;
}

static bool
adsp_fastrpc_has_pending(void)
{
	return adsp_fastrpc_rx_ring[adsp_fastrpc_rx_head].used;
}

static bool
adsp_fastrpc_send_init_static(void)
{
	static const char pd_name[] = "sensorspd";
	struct fastrpc_init_create_static_inbuf inbuf = { 1, sizeof(pd_name), 1u };
	struct fastrpc_phy_page heap_pg = { adsp_fastrpc_heap_pa, ADSP_FASTRPC_HEAP_SIZE };
	struct adsp_fastrpc_host_arg args[3] = {
		{ &inbuf, NULL, sizeof(inbuf) },
		{ pd_name, NULL, sizeof(pd_name) },
		{ &heap_pg, NULL, sizeof(heap_pg) },
	};
	uint32_t sc = ADSP_FASTRPC_SCALARS(ADSP_FASTRPC_RMID_INIT_CREATE_STATIC, 3, 0);
	uint32_t pkt_sz = 0;

	if (!adsp_fastrpc_build_invoke(sc, args, NULL, &pkt_sz)) {
		return false;
	}
	if (!adsp_fastrpc_send_invoke(ADSP_FASTRPC_USER_PD, ADSP_FASTRPC_INIT_HANDLE, sc, pkt_sz)) {
		return false;
	}
	adsp_fastrpc_init_attempts++;
	adsp_fastrpc_stage = FASTRPC_STAGE_WAIT_INIT_STATIC;
	return true;
}

static bool
adsp_fastrpc_send_attach_sns(void)
{
	int32_t client_id = 1;
	struct adsp_fastrpc_host_arg args[1] = { { &client_id, NULL, sizeof(client_id) } };
	uint32_t sc = ADSP_FASTRPC_SCALARS(ADSP_FASTRPC_RMID_INIT_ATTACH, 1, 0);
	uint32_t pkt_sz = 0;

	if (!adsp_fastrpc_build_invoke(sc, args, NULL, &pkt_sz)) {
		return false;
	}
	if (!adsp_fastrpc_send_invoke(ADSP_FASTRPC_SENSORS_PD, ADSP_FASTRPC_INIT_HANDLE, sc, pkt_sz)) {
		return false;
	}
	adsp_fastrpc_stage = FASTRPC_STAGE_WAIT_INIT_SNS;
	return true;
}

static bool
adsp_fastrpc_send_listener_init2(void)
{
	uint32_t sc = ADSP_FASTRPC_SCALARS(ADSP_FASTRPC_LISTENER_INIT2, 0, 0);
	if (!adsp_fastrpc_send_invoke(ADSP_FASTRPC_USER_PD, ADSP_FASTRPC_LISTENER_HANDLE, sc, 0u)) {
		return false;
	}
	adsp_fastrpc_stage = FASTRPC_STAGE_WAIT_LISTENER_INIT2;
	return true;
}

static bool
adsp_fastrpc_send_listener_next2(void)
{
	uint32_t cap = ADSP_FASTRPC_IO_MAX_BYTES;
	uint32_t prim_in[4] = {
		adsp_fastrpc_prev_rev_ctx,
		(uint32_t)adsp_fastrpc_prev_rev_rc,
		adsp_fastrpc_prev_outbufs_len,
		cap,
	};
	struct adsp_fastrpc_host_arg args[4] = {
		{ prim_in, NULL, sizeof(prim_in) },
		{ adsp_fastrpc_prev_outbufs, NULL, adsp_fastrpc_prev_outbufs_len },
		{ NULL, NULL, 16u },
		{ NULL, NULL, cap },
	};
	uint32_t offs[4] = { 0, 0, 0, 0 };
	uint32_t sc = ADSP_FASTRPC_SCALARS(ADSP_FASTRPC_LISTENER_NEXT2, 2, 2);
	uint32_t pkt_sz = 0;

	if (!adsp_fastrpc_build_invoke(sc, args, offs, &pkt_sz)) {
		return false;
	}
	adsp_fastrpc_next2_out0_off = offs[2];
	adsp_fastrpc_next2_out1_off = offs[3];
	adsp_fastrpc_next2_out1_cap = cap;
	if (!adsp_fastrpc_send_invoke(ADSP_FASTRPC_USER_PD, ADSP_FASTRPC_LISTENER_HANDLE, sc, pkt_sz)) {
		return false;
	}
	adsp_fastrpc_stage = FASTRPC_STAGE_WAIT_LISTENER_NEXT2;
	return true;
}

static void
adsp_fastrpc_handle_rsp(const struct fastrpc_wire_rsp *rsp)
{
	if (rsp->ctx == 0xabcdabcdULL || rsp->ctx != adsp_fastrpc_inflight_ctx) {
		return;
	}
	adsp_fastrpc_inflight_ctx = 0;
	if (adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_STATIC) {
		say("fastrpc: INIT_CREATE_STATIC(sensorspd) -> retval=%d (%#x)\n",
		    rsp->retval, (uint32_t)rsp->retval);
		if (rsp->retval == 0) {
			(void)adsp_fastrpc_send_listener_init2();
		} else {
			(void)adsp_fastrpc_send_attach_sns();
		}
	} else if (adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_SNS) {
		say("fastrpc: INIT_ATTACH_SNS -> retval=%d (%#x)\n", rsp->retval, (uint32_t)rsp->retval);
		if (rsp->retval == 0) {
			(void)adsp_fastrpc_send_listener_init2();
		} else {
			adsp_fastrpc_stage = FASTRPC_STAGE_FAILED;
		}
	} else if (adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_LISTENER_INIT2) {
		say("fastrpc: adsp_listener_init2 -> retval=%d (%#x)\n", rsp->retval, (uint32_t)rsp->retval);
		if (rsp->retval == 0) {
			adsp_fastrpc_prev_rev_ctx = 0;
			adsp_fastrpc_prev_rev_rc = 0;
			adsp_fastrpc_prev_outbufs_len = 0;
			(void)adsp_fastrpc_send_listener_next2();
		} else {
			adsp_fastrpc_stage = FASTRPC_STAGE_FAILED;
		}
	} else if (adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_LISTENER_NEXT2) {
		if (rsp->retval != 0) {
			say("fastrpc: adsp_listener_next2 -> retval=%d (%#x)\n",
			    rsp->retval, (uint32_t)rsp->retval);
			adsp_fastrpc_stage = FASTRPC_STAGE_FAILED;
			return;
		}
		adsp_fastrpc_cache_clean(adsp_fastrpc_buf_va, ADSP_FASTRPC_INVOKE_BUF_SZ);
		const uint8_t *base = (const uint8_t *)(uintptr_t)adsp_fastrpc_buf_va;
		uint32_t out0[4] = { 0, 0, 0, 0 };
		memcpy(out0, base + adsp_fastrpc_next2_out0_off, 16u);
		uint32_t rev_ctx = out0[0];
		uint32_t rev_handle = out0[1];
		uint32_t rev_sc = out0[2];
		uint32_t rev_req = out0[3];
		uint32_t out_used = 0;
		int32_t rc = ENOMEM;
		if (rev_req <= adsp_fastrpc_next2_out1_cap) {
			rc = adsp_fastrpc_dispatch_rev(rev_handle, rev_sc,
			    base + adsp_fastrpc_next2_out1_off, rev_req,
			    adsp_fastrpc_prev_outbufs, sizeof(adsp_fastrpc_prev_outbufs), &out_used);
		} else {
			say("fastrpc: listener_next2 req=%u exceeds cap=%u\n",
			    rev_req, adsp_fastrpc_next2_out1_cap);
		}
		adsp_fastrpc_prev_rev_ctx = rev_ctx;
		adsp_fastrpc_prev_rev_rc = rc;
		adsp_fastrpc_prev_outbufs_len = rc == 0 ? out_used : 0u;
		(void)adsp_fastrpc_send_listener_next2();
	}
}

static bool
adsp_fastrpc_pop_rx(uint8_t *dst, uint32_t *len_out)
{
	bool got = false;
#ifndef ADSP_FASTRPC_HOST_TEST
	lck_mtx_lock(&glink_rx_lock);
#endif
	struct adsp_fastrpc_rx_slot *slot = &adsp_fastrpc_rx_ring[adsp_fastrpc_rx_head];
	if (slot->used) {
		memcpy(dst, slot->buf, slot->len);
		*len_out = slot->len;
		slot->used = false;
		adsp_fastrpc_rx_head = (adsp_fastrpc_rx_head + 1u) % ADSP_FASTRPC_RX_RING_SZ;
		got = true;
	}
#ifndef ADSP_FASTRPC_HOST_TEST
	lck_mtx_unlock(&glink_rx_lock);
#endif
	return got;
}

static void
adsp_fastrpc_service(void)
{
	uint8_t pkt[ADSP_FASTRPC_REASM_MAX];
	uint32_t pkt_len = 0;

	adsp_fastrpc_svc_lock();
	while (adsp_fastrpc_pop_rx(pkt, &pkt_len)) {
		if (pkt_len >= 12u) {
			struct fastrpc_wire_rsp rsp;
			memset(&rsp, 0, sizeof(rsp));
			memcpy(&rsp, pkt, pkt_len < sizeof(rsp) ? pkt_len : sizeof(rsp));
			adsp_fastrpc_handle_rsp(&rsp);
		}
	}

	if (!adsp_fastrpc_ready) {
		adsp_fastrpc_svc_unlock();
		return;
	}
	if (adsp_fastrpc_stage == FASTRPC_STAGE_IDLE) {
		bool has_intent = false;
		lck_mtx_lock(&glink_intent_lock);
		has_intent = (adsp_fastrpc_intents.head != NULL);
		lck_mtx_unlock(&glink_intent_lock);
		if (!has_intent) {
			/* ADSP fastrpcglink-apps-dsp queues TX intents only after GLINK_RX_INTENT_REQ. */
			uint64_t now = adsp_fastrpc_now_us();
			if (adsp_fastrpc_intent_reqs == 0 ||
			    now - adsp_fastrpc_intent_req_us >= ADSP_FASTRPC_INTENT_REQ_INTERVAL_US) {
				if (adsp_fastrpc_intent_reqs < ADSP_FASTRPC_INTENT_REQ_RETRIES) {
					if (adsp_fastrpc_intent_reqs == 0) {
						say("fastrpc: requesting remote intent (%uB)\n",
						    ADSP_FASTRPC_REMOTE_INTENT_MIN);
					}
					adsp_fastrpc_intent_reqs++;
					adsp_fastrpc_intent_req_us = now;
					glink_send_cmd2(GLINK_RX_INTENT_REQ, adsp_fastrpc_our_cid,
					    ADSP_FASTRPC_REMOTE_INTENT_MIN);
				} else {
					say("fastrpc: no remote intent after %u requests\n",
					    adsp_fastrpc_intent_reqs);
					adsp_fastrpc_stage = FASTRPC_STAGE_FAILED;
				}
			}
			adsp_fastrpc_svc_unlock();
			return;
		}
		if (!adsp_fastrpc_smmu_configure() || !adsp_fastrpc_ensure_mem()) {
			adsp_fastrpc_stage = FASTRPC_STAGE_FAILED;
			adsp_fastrpc_svc_unlock();
			return;
		}
		(void)adsp_fastrpc_send_init_static();
	} else if ((adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_STATIC ||
	    adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_SNS) && adsp_fastrpc_inflight_ctx != 0 &&
	    adsp_fastrpc_now_us() - adsp_fastrpc_inflight_sent_us >= ADSP_FASTRPC_CTX_TIMEOUT_US) {
		adsp_fastrpc_inflight_ctx = 0;
		if (adsp_fastrpc_init_attempts < ADSP_FASTRPC_INIT_RETRIES) {
			say("fastrpc: init timeout, retrying (%u/%u)\n",
			    adsp_fastrpc_init_attempts, ADSP_FASTRPC_INIT_RETRIES);
			(void)adsp_fastrpc_send_init_static();
		} else {
			adsp_fastrpc_stage = FASTRPC_STAGE_FAILED;
		}
	}
	adsp_fastrpc_svc_unlock();
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

#!/usr/bin/env python3
"""Host ASan+UBSan tests for annibale_adsp_ipcrtr.inc, annibale_sensors_scan.inc, and annibale_sensors_ssc.inc."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
INTENTS_INC = ROOT / "xnu/files/bsd/dev/annibale/annibale_adsp_intents.inc"
IPCRTR_INC = ROOT / "xnu/files/bsd/dev/annibale/annibale_adsp_ipcrtr.inc"
SCAN_INC = ROOT / "xnu/files/bsd/dev/annibale/annibale_sensors_scan.inc"
SSC_INC = ROOT / "xnu/files/bsd/dev/annibale/annibale_sensors_ssc.inc"

HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLINK_INTENT        5u
#define GLINK_RX_INTENT_REQ 7u
#define GLINK_TX_DATA       9u
#define GLINK_TX_DATA_CONT  12u
#define Z_WAITOK            0
#define Z_ZERO              1
#define kalloc_type(T, fl)  ((T *)calloc(1, sizeof(T)))
#define kfree_type(T, p)    free(p)
#define bzero(p, n)         memset((p), 0, (n))
#ifndef __unused
#define __unused            __attribute__((unused))
#endif

static void *kalloc_data(size_t n, int flags) { (void)flags; return malloc(n); }
static void kfree_data(void *p, size_t n) { (void)n; free(p); }
static uint64_t mach_absolute_time(void) { return 12345678ULL; }

typedef int lck_mtx_t;
static lck_mtx_t sensors_lck;
static void lck_mtx_lock(lck_mtx_t *l) { (void)l; }
static void lck_mtx_unlock(lck_mtx_t *l) { (void)l; }
static void say(const char *fmt, ...) { (void)fmt; }

#define ANNIBALE_SENSOR_SRC_ADSP_SSC (1u << 2)
#define ANNIBALE_SENSOR_SRC_FRONT_OK (1u << 4)
struct annibale_sensor_sample {
    uint64_t timestamp;
    uint32_t als_lux, clear_raw, ir_raw, prox_raw;
    uint16_t prox_distance_mm;
    uint8_t  prox_near, source_flags;
};
struct annibale_sensors_state {
    struct annibale_sensor_sample sample;
};
static struct annibale_sensors_state sensor_state;

/* ---- annibale_qrtr mock for annibale_sensors_ssc.inc ---- */
struct annibale_qrtr_port { int id; };
typedef void (*annibale_qrtr_receive_t)(void *, uint32_t, uint32_t, const void *, size_t);
typedef void (*annibale_qrtr_server_t)(void *, bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
struct annibale_qrtr_ops {
    struct annibale_qrtr_port *(*open)(annibale_qrtr_receive_t, annibale_qrtr_server_t, void *);
    int (*send)(struct annibale_qrtr_port *, uint32_t, uint32_t, const void *, size_t);
    int (*lookup)(struct annibale_qrtr_port *, uint32_t, uint32_t, uint32_t);
    void (*close)(struct annibale_qrtr_port *);
};

static struct annibale_qrtr_port g_mock_port = { 1 };
static uint32_t g_qrtr_tx_count = 0;
static uint8_t g_qrtr_tx_pkts[8][256];
static size_t g_qrtr_tx_lens[8];
static uint32_t g_qrtr_tx_nodes[8], g_qrtr_tx_ports[8];
static uint32_t g_qrtr_lookup_svc = 0, g_qrtr_fresh_calls = 0;
void annibale_qrtr_fresh(void) { g_qrtr_fresh_calls++; }

static struct annibale_qrtr_port *mock_open(annibale_qrtr_receive_t rx, annibale_qrtr_server_t srv, void *ctx) {
    (void)rx; (void)srv; (void)ctx;
    return &g_mock_port;
}
static int mock_send(struct annibale_qrtr_port *p, uint32_t node, uint32_t port, const void *data, size_t len) {
    (void)p;
    assert(g_qrtr_tx_count < 8 && len <= 256);
    memcpy(g_qrtr_tx_pkts[g_qrtr_tx_count], data, len);
    g_qrtr_tx_lens[g_qrtr_tx_count] = len;
    g_qrtr_tx_nodes[g_qrtr_tx_count] = node;
    g_qrtr_tx_ports[g_qrtr_tx_count] = port;
    g_qrtr_tx_count++;
    return 0;
}
static int mock_lookup(struct annibale_qrtr_port *p, uint32_t svc, uint32_t ver, uint32_t inst) {
    (void)p; (void)ver; (void)inst;
    g_qrtr_lookup_svc = svc;
    return 0;
}
static void mock_close(struct annibale_qrtr_port *p) { (void)p; }
static const struct annibale_qrtr_ops annibale_qrtr = { mock_open, mock_send, mock_lookup, mock_close };

/* ---- annibale_adsp_intents.inc + annibale_adsp_ipcrtr.inc mocks ---- */
static lck_mtx_t glink_intent_lock;
struct glink_remote_intent {
    struct glink_remote_intent *next;
    uint32_t id, size;
    bool free;
};
struct glink_intent_pool {
    struct glink_remote_intent *head;
};
static bool adsp_hw_up = true;
static uint32_t glink_our_q = 0x1000u;
static uint32_t glink_our_q_size = 0x4000u;
static uint32_t glink_tx_write_pos = 0;
static uint32_t g_open_calls = 0, g_poll_rx_calls = 0, g_intent_req_calls = 0;
static uint32_t g_intent_sizes[16], g_intent_count = 0;
static uint8_t g_last_tx[256];
static uint32_t g_last_tx_len = 0;
static bool g_q_write_ok = true;

static bool glink_q_write(uint32_t q_off, uint32_t q_sz, uint32_t *write_at, const void *buf, uint32_t len) {
    (void)q_off; (void)q_sz; (void)write_at;
    if (!g_q_write_ok) return false;
    if (len == 16) {
        const uint32_t *w = (const uint32_t *)buf;
        if ((w[0] & 0xffffu) == GLINK_INTENT && g_intent_count < 16) g_intent_sizes[g_intent_count++] = w[2];
    }
    if (len <= sizeof(g_last_tx)) { memcpy(g_last_tx, buf, len); g_last_tx_len = len; }
    return true;
}
static void glink_send_cmd2(uint16_t cmd, uint16_t p1, uint32_t p2) {
    (void)p1; (void)p2;
    if (cmd == GLINK_RX_INTENT_REQ) g_intent_req_calls++;
}
static void glink_send_open(uint16_t cid, const char *name) {
    assert(cid == 4u && strcmp(name, "IPCRTR") == 0);
    g_open_calls++;
}
static uint64_t now_us(void) { return 0; }
static void sleep_us(uint32_t us) { (void)us; }
static void glink_poll_rx(void) { g_poll_rx_calls++; }
static uint8_t g_smem[0x4000];
static bool smem_peek(uint32_t off, void *dst, uint32_t len) {
    assert(off + len <= sizeof(g_smem));
    memcpy(dst, g_smem + off, len);
    return true;
}
void annibale_qrtr_attach_node(uint32_t peer_node, int (*send)(const void *, uint32_t)) {
    (void)peer_node; (void)send;
}
void annibale_qrtr_input(const void *data, uint32_t length) { (void)data; (void)length; }

__INTENTS_INC__
__IPCRTR_INC__
__SCAN_FORBIDDEN__
__SSC_INC__

static size_t build_qmi_ind(uint8_t *out, const uint8_t *pb, size_t pb_len) {
    uint16_t txn = 1, msg = QMI_SNS_CLIENT_REPORT_IND;
    uint16_t tlv2_len = (uint16_t)(2u + pb_len);
    uint16_t body_len = (uint16_t)(3u + tlv2_len);
    uint16_t plen16 = (uint16_t)pb_len;
    out[0] = QMI_HDR_TYPE_IND;
    memcpy(out + 1, &txn, 2);
    memcpy(out + 3, &msg, 2);
    memcpy(out + 5, &body_len, 2);
    out[7] = 0x02u;
    memcpy(out + 8, &tlv2_len, 2);
    memcpy(out + 10, &plen16, 2);
    memcpy(out + 12, pb, pb_len);
    return 12u + pb_len;
}

static size_t build_event_msg(uint8_t *out, size_t cap, uint64_t lo, uint64_t hi,
                              uint32_t msg_id, const uint8_t *ep, size_t ep_len) {
    uint8_t suid_b[24], ev_b[160];
    struct pb_writer sw = { suid_b, sizeof(suid_b), 0, false };
    pb_put_fixed64(&sw, 1u, lo);
    pb_put_fixed64(&sw, 2u, hi);
    struct pb_writer ew = { ev_b, sizeof(ev_b), 0, false };
    pb_put_fixed32(&ew, 1u, msg_id);
    pb_put_fixed64(&ew, 2u, 999999ULL);
    pb_put_len_delim(&ew, 3u, ep, ep_len);
    struct pb_writer mw = { out, cap, 0, false };
    pb_put_len_delim(&mw, 1u, suid_b, sw.pos);
    pb_put_len_delim(&mw, 2u, ev_b, ew.pos);
    assert(!sw.overflow && !ew.overflow && !mw.overflow);
    return mw.pos;
}

int main(void) {
    /* 1. ADSP IPCRTR node 5 + bidirectional open + DTB intents */
    assert(QRTR_NODE_ADSP == 5u);
    adsp_ipcrtr_reset();
    adsp_ipcrtr_request_open();
    adsp_ipcrtr_their_cid = 9u;
    adsp_ipcrtr_our_ack = true;
    assert(adsp_ipcrtr_note_ready() && !adsp_ipcrtr_note_ready());
    assert(g_intent_count == 10u && g_intent_sizes[0] == 0x800u && g_intent_sizes[9] == 0x4400u);

    /* 2. TX framing & error paths (non-blocking intent path: never re-enters glink_poll_rx) */
    (void)glink_take_their_intent;
    uint8_t payload[20];
    for (uint32_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(0xa0u + i);
    g_poll_rx_calls = 0; g_intent_req_calls = 0;
    assert(adsp_qrtr_send(payload, 20u) == EAGAIN);
    assert(g_intent_req_calls == 1u && g_poll_rx_calls == 0u);
    glink_add_their_intent(&adsp_ipcrtr_intents, 77u, 256u);
    assert(adsp_qrtr_send(payload, sizeof(payload)) == 0 && g_last_tx_len == 36u && g_poll_rx_calls == 0u);
    assert(adsp_qrtr_send(payload, glink_our_q_size) == EMSGSIZE);
    /* Intent 77 is now busy -> next send returns EAGAIN without calling glink_poll_rx() */
    assert(adsp_qrtr_send(payload, 20u) == EAGAIN && g_intent_req_calls == 2u && g_poll_rx_calls == 0u);
    /* Complete intent 77 with reuse=true -> EIO on glink_q_write returns intent to free */
    glink_complete_their_intent(&adsp_ipcrtr_intents, 77u, true);
    g_q_write_ok = false; assert(adsp_qrtr_send(payload, 20u) == EIO && g_poll_rx_calls == 0u); g_q_write_ok = true;
    assert(adsp_qrtr_send(payload, 20u) == 0 && g_poll_rx_calls == 0u);
    glink_complete_their_intent(&adsp_ipcrtr_intents, 77u, false);

    /* 3. Fragmented RX reassembly across ring wrap + oversize recovery */
    uint8_t expect[120];
    for (uint32_t i = 0; i < sizeof(expect); i++) expect[i] = (uint8_t)(i * 3u + 7u);
    uint32_t q_sz = sizeof(g_smem), r0 = q_sz - 32u;
    for (uint32_t i = 0; i < 64u; i++) g_smem[(r0 + 16u + i) % q_sz] = expect[i];
    uint8_t *out_buf = NULL; uint32_t out_len = 0;
    adsp_ipcrtr_rx_chunk(GLINK_TX_DATA, 9u, 0u, q_sz, r0, 64u, 56u, &out_buf, &out_len);
    assert(out_buf == NULL);
    for (uint32_t i = 0; i < 56u; i++) g_smem[(128u + 16u + i) % q_sz] = expect[64u + i];
    adsp_ipcrtr_rx_chunk(GLINK_TX_DATA_CONT, 9u, 0u, q_sz, 128u, 56u, 0u, &out_buf, &out_len);
    assert(out_buf != NULL && out_len == 120u && memcmp(out_buf, expect, 120u) == 0);
    free(out_buf); out_buf = NULL; out_len = 0;
    adsp_ipcrtr_rx_chunk(GLINK_TX_DATA, 9u, 0u, q_sz, 0u, 100u, ADSP_IPCRTR_REASM_MAX, &out_buf, &out_len);
    adsp_ipcrtr_rx_chunk(GLINK_TX_DATA_CONT, 9u, 0u, q_sz, 0u, 100u, 0u, &out_buf, &out_len);
    assert(out_buf == NULL);

    /* 4. XPU reserved + connectivity pin hard-deny guard */
    const uint32_t blocked[] = { 16, 17, 18, 19, 36, 37, 38, 39, 48, 49, 50, 51, 74 };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) assert(scan_pin_forbidden(blocked[i]));
    const uint32_t allowed[] = { 8, 9, 12, 13, 20, 21, 70, 71, 72, 73, 76, 77, 80, 81, 82, 83, 206, 207 };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) assert(!scan_pin_forbidden(allowed[i]));

    /* 5. Integer-only IEEE-754 float32 -> u32 */
    assert(ssc_f32_bits_to_u32(0x00000000u, 100000u) == 0u);      /* 0.0 */
    assert(ssc_f32_bits_to_u32(0xc0a00000u, 100000u) == 0u);      /* -5.0 */
    assert(ssc_f32_bits_to_u32(0x3f000000u, 100000u) == 0u);      /* 0.5 */
    assert(ssc_f32_bits_to_u32(0x3f800000u, 100000u) == 1u);      /* 1.0 */
    assert(ssc_f32_bits_to_u32(0x43af2000u, 100000u) == 350u);    /* 350.25 */
    assert(ssc_f32_bits_to_u32(0x7f800000u, 100000u) == 100000u); /* +Inf */
    assert(ssc_f32_bits_to_u32(0x7fc00000u, 100000u) == 0u);      /* NaN */

    /* 6. QMI SNS_CLIENT discovery -> deferred send in sensors_ssc_tick() (zero send inside QRTR callbacks) */
    sensors_ssc_init();
    assert(g_qrtr_lookup_svc == QMI_SVC_SNS_CLIENT && g_qrtr_fresh_calls == 1u && g_qrtr_tx_count == 0u);
    ssc_qrtr_server(NULL, true, QMI_SVC_SNS_CLIENT, 1u, 0u, 5u, 18u);
    assert(g_qrtr_tx_count == 0u); /* no QRTR send inside qrtr_calling callback */
    sensors_ssc_tick();
    assert(g_qrtr_tx_count == 4u && g_qrtr_tx_nodes[0] == 5u && g_qrtr_tx_ports[0] == 18u);

    /* Deliver SUID_EVENT for ambient_light and proximity */
    const uint64_t als_lo = 0x1122334455667788ULL, als_hi = 0x99aabbccddeeff00ULL;
    const uint64_t prox_lo = 0x0102030405060708ULL, prox_hi = 0x090a0b0c0d0e0f10ULL;
    uint8_t ep[80], pb[220], qmi[256];
    {
        uint8_t sb[24]; struct pb_writer sw = { sb, sizeof(sb), 0, false };
        pb_put_fixed64(&sw, 1u, als_lo); pb_put_fixed64(&sw, 2u, als_hi);
        struct pb_writer ew = { ep, sizeof(ep), 0, false };
        pb_put_len_delim(&ew, 1u, "ambient_light", 13u);
        pb_put_len_delim(&ew, 2u, sb, sw.pos);
        size_t plen = build_event_msg(pb, sizeof(pb), SNS_SUID_LOOKUP_LOW, SNS_SUID_LOOKUP_HIGH, SNS_SUID_MSGID_EVENT, ep, ew.pos);
        size_t qlen = build_qmi_ind(qmi, pb, plen);
        ssc_qrtr_rx(NULL, 5u, 18u, qmi, qlen);
        assert(ssc_als_suid.valid && ssc_als_suid.low == als_lo && ssc_als_suid.high == als_hi);
        assert(g_qrtr_tx_count == 4u); /* deferred until tick */
        sensors_ssc_tick();
        assert(g_qrtr_tx_count == 5u);
    }
    {
        uint8_t sb[24]; struct pb_writer sw = { sb, sizeof(sb), 0, false };
        pb_put_fixed64(&sw, 1u, prox_lo); pb_put_fixed64(&sw, 2u, prox_hi);
        struct pb_writer ew = { ep, sizeof(ep), 0, false };
        pb_put_len_delim(&ew, 1u, "proximity", 9u);
        pb_put_len_delim(&ew, 2u, sb, sw.pos);
        size_t plen = build_event_msg(pb, sizeof(pb), SNS_SUID_LOOKUP_LOW, SNS_SUID_LOOKUP_HIGH, SNS_SUID_MSGID_EVENT, ep, ew.pos);
        size_t qlen = build_qmi_ind(qmi, pb, plen);
        ssc_qrtr_rx(NULL, 5u, 18u, qmi, qlen);
        assert(ssc_prox_suid.valid && ssc_prox_suid.low == prox_lo && g_qrtr_tx_count == 5u);
        sensors_ssc_tick();
        assert(g_qrtr_tx_count == 6u);
    }

    /* Deliver ambient_light SNS_STD_SENSOR_EVENT (packed float32: 350.25f, 1401.0f) */
    {
        uint32_t vals[2] = { 0x43af2000u, 0x44af2000u };
        struct pb_writer ew = { ep, sizeof(ep), 0, false };
        pb_put_len_delim(&ew, 1u, vals, sizeof(vals));
        pb_put_tag(&ew, 2u, 0u); pb_put_varint(&ew, 3u);
        size_t plen = build_event_msg(pb, sizeof(pb), als_lo, als_hi, SNS_STD_SENSOR_MSGID_EVENT, ep, ew.pos);
        size_t qlen = build_qmi_ind(qmi, pb, plen);
        ssc_qrtr_rx(NULL, 5u, 18u, qmi, qlen);
        assert(sensor_state.sample.als_lux == 350u && sensor_state.sample.clear_raw == 1401u);
        assert((sensor_state.sample.source_flags & ANNIBALE_SENSOR_SRC_ADSP_SSC) != 0);
    }

    /* Deliver proximity SNS_PROXIMITY_EVENT (NEAR=1, raw_adc=1200 -> FAR=0) */
    {
        struct pb_writer ew = { ep, sizeof(ep), 0, false };
        pb_put_tag(&ew, 1u, 0u); pb_put_varint(&ew, 1u);
        pb_put_tag(&ew, 2u, 0u); pb_put_varint(&ew, 1200u);
        size_t plen = build_event_msg(pb, sizeof(pb), prox_lo, prox_hi, SNS_PROXIMITY_MSGID_EVENT, ep, ew.pos);
        size_t qlen = build_qmi_ind(qmi, pb, plen);
        ssc_qrtr_rx(NULL, 5u, 18u, qmi, qlen);
        assert(sensor_state.sample.prox_near == 1u && sensor_state.sample.prox_distance_mm == 0u && sensor_state.sample.prox_raw == 1200u);

        struct pb_writer ew2 = { ep, sizeof(ep), 0, false };
        pb_put_tag(&ew2, 1u, 0u); pb_put_varint(&ew2, 0u);
        plen = build_event_msg(pb, sizeof(pb), prox_lo, prox_hi, SNS_PROXIMITY_MSGID_EVENT, ep, ew2.pos);
        qlen = build_qmi_ind(qmi, pb, plen);
        ssc_qrtr_rx(NULL, 5u, 18u, qmi, qlen);
        assert(sensor_state.sample.prox_near == 0u && sensor_state.sample.prox_distance_mm == 50u);
    }

    /* Verify QMI_SNS_CLIENT_REPORT_IND is 0x0021 and client_proc_type=1 (APSS) */
    assert(QMI_SNS_CLIENT_REPORT_IND == 0x0021u && QMI_SNS_CLIENT_JUMBO_IND == 0x0022u);
    assert(g_qrtr_tx_lens[0] >= 12u);
    {
        uint8_t want_susp[6] = { 0x1au, 0x04u, 0x08u, 0x01u, 0x10u, 0x00u };
        bool found_susp = false;
        for (size_t i = 0; i + sizeof(want_susp) <= g_qrtr_tx_lens[0]; i++) {
            if (memcmp(g_qrtr_tx_pkts[0] + i, want_susp, sizeof(want_susp)) == 0) found_susp = true;
        }
        assert(found_susp);
    }

    /* Truncated / malformed QMI & protobuf fuzzing */
    uint8_t bad[16] = { QMI_HDR_TYPE_IND, 1, 0, 0x21, 0, 9, 0, 0x02, 6, 0, 4, 0, 0xff, 0xff, 0xff, 0xff };
    for (size_t n = 0; n <= sizeof(bad); n++) {
        ssc_qrtr_rx(NULL, 5u, 18u, bad, n);
    }

    printf("PASS: annibale_adsp_ipcrtr + sensors XPU guard + SNS_CLIENT SSC (ASan+UBSan)\n");
    return 0;
}
"""


def extract_scan_forbidden(text: str) -> str:
    start = text.index("static bool\nscan_pin_forbidden(")
    end = text.index("\nstatic void\nparse_scan_boot_arg(", start)
    return text[start:end]


def main() -> None:
    ssc_text = SSC_INC.read_text().replace("#include <sys/annibale_qrtr.h>", "")
    code = (
        HARNESS.replace("__INTENTS_INC__", INTENTS_INC.read_text())
        .replace("__IPCRTR_INC__", IPCRTR_INC.read_text())
        .replace("__SCAN_FORBIDDEN__", extract_scan_forbidden(SCAN_INC.read_text()))
        .replace("__SSC_INC__", ssc_text)
    )
    with tempfile.TemporaryDirectory() as tmp:
        td = Path(tmp)
        src = td / "test_adsp_ipcrtr_ssc.c"
        exe = td / "test_adsp_ipcrtr_ssc"
        src.write_text(code)
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(src),
                "-o",
                str(exe),
            ],
            check=True,
        )
        res = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
        print(res.stdout.strip())


if __name__ == "__main__":
    main()

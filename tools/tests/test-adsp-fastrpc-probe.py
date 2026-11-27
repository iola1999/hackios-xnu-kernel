#!/usr/bin/env python3
"""Host ASan+UBSan test for annibale_adsp_fastrpc.inc Stage 1 read-only GLINK probe."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FASTRPC_INC = ROOT / "xnu/files/bsd/dev/annibale/annibale_adsp_fastrpc.inc"

HARNESS = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GLINK_INTENT       5u
#define GLINK_TX_DATA      9u
#define GLINK_TX_DATA_CONT 12u
#define bzero(p, n)        memset((p), 0, (n))

static uint32_t glink_our_q = 0x1000u;
static uint32_t glink_our_q_size = 0x4000u;
static uint32_t glink_tx_write_pos = 0;
static uint16_t adsp_ipcrtr_our_cid = 4u, adsp_ipcrtr_their_cid = 9u;
static uint16_t glink_pmic_our_cid = 3u, glink_pmic_their_cid = 8u;
static uint16_t glink_gpr_our_cid = 2u, glink_gpr_their_cid = 7u;

static uint32_t g_open_calls = 0;
static uint16_t g_open_cid = 0;
static char g_open_name[32];
static uint32_t g_intent_count = 0;
static uint32_t g_intent_sizes[16];
static uint32_t g_log_count = 0;
static char g_logs[64][160];
static uint8_t g_smem[0x1000];

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log_count < 64u) {
        vsnprintf(g_logs[g_log_count], sizeof(g_logs[0]), fmt, ap);
    }
    g_log_count++;
    va_end(ap);
}

static bool glink_q_write(uint32_t q_off, uint32_t q_sz, uint32_t *write_at, const void *buf, uint32_t len) {
    (void)q_off; (void)q_sz; (void)write_at;
    if (len == 16u) {
        const uint32_t *w = (const uint32_t *)buf;
        if ((w[0] & 0xffffu) == GLINK_INTENT && g_intent_count < 16u) {
            g_intent_sizes[g_intent_count++] = w[2];
        }
    }
    return true;
}

static void glink_send_open(uint16_t cid, const char *name) {
    g_open_calls++;
    g_open_cid = cid;
    strncpy(g_open_name, name, sizeof(g_open_name) - 1u);
    g_open_name[sizeof(g_open_name) - 1u] = '\0';
}

static bool smem_peek(uint32_t off, void *dst, uint32_t len) {
    assert(off + len <= sizeof(g_smem));
    memcpy(dst, g_smem + off, len);
    return true;
}

__FASTRPC_INC__

int main(void) {
    /* 1. Bidirectional open + minimal RX intent queueing (4 x 512B). */
    adsp_fastrpc_reset();
    adsp_fastrpc_request_open();
    assert(g_open_calls == 1u && g_open_cid == ADSP_FASTRPC_OUR_CID);
    assert(strcmp(g_open_name, "fastrpcglink-apps-dsp") == 0);
    assert(strlen(ADSP_FASTRPC_CHAN) + 1u + 8u <= 32u); /* fits glink_send_open pkt[32] */

    adsp_fastrpc_on_open("unrelated_chan", 99u);
    assert(adsp_fastrpc_their_cid == 0);
    adsp_fastrpc_on_open("fastrpcglink-apps-dsp", 11u);
    assert(adsp_fastrpc_their_cid == 11u && g_open_calls == 1u && !adsp_fastrpc_ready);
    adsp_fastrpc_on_open_ack(ADSP_FASTRPC_OUR_CID);
    assert(adsp_fastrpc_ready && g_intent_count == ADSP_FASTRPC_INTENT_COUNT);
    assert(g_intent_sizes[0] == ADSP_FASTRPC_INTENT_SIZE);
    assert(adsp_glink_our_cid_for(11u) == ADSP_FASTRPC_OUR_CID);
    assert(adsp_glink_our_cid_for(9u) == adsp_ipcrtr_our_cid);
    assert(adsp_glink_our_cid_for(99u) == 0);

    /* 2. Remote intent request + bounded remote intent logging (max 8). */
    adsp_fastrpc_on_intent_req(11u);
    assert(g_intent_count == 2u * ADSP_FASTRPC_INTENT_COUNT);
    uint32_t before_logs = g_log_count;
    for (uint32_t i = 0; i < 12u; i++) {
        adsp_fastrpc_on_intent(11u, i + 1u, 0x181u);
    }
    assert(g_log_count == before_logs + ADSP_FASTRPC_INTENT_LOG_MAX);

    /* 3. Fragmented RX across ring wrap + header word logging. */
    uint32_t msg[6] = { 0x11223344u, 0x55667788u, 0x00000003u, 0x01020304u, 0xaabbccddu, 0xdeadbeefu };
    uint8_t raw[24];
    memcpy(raw, msg, sizeof(raw));
    uint32_t q_sz = sizeof(g_smem), r0 = q_sz - 24u;
    for (uint32_t i = 0; i < 12u; i++) g_smem[(r0 + 16u + i) % q_sz] = raw[i];
    assert(!adsp_fastrpc_rx_chunk(GLINK_TX_DATA, 9u, 0u, q_sz, r0, 12u, 12u)); /* IPCRTR cid ignored */
    assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA, 11u, 0u, q_sz, r0, 12u, 12u));
    assert(adsp_fastrpc_rx_count == 0u);
    for (uint32_t i = 0; i < 12u; i++) g_smem[(64u + 16u + i) % q_sz] = raw[12u + i];
    uint32_t rx_log_idx = g_log_count;
    assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA_CONT, 11u, 0u, q_sz, 64u, 12u, 0u));
    assert(adsp_fastrpc_rx_count == 1u && g_log_count == rx_log_idx + 1u);
    assert(strstr(g_logs[rx_log_idx], "len=24 hdr=0x11223344 0x55667788 0x3 0x1020304") != NULL);

    /* 4. Oversize message recovery + bounded 16-message log cap. */
    assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA, 11u, 0u, q_sz, 0u, 100u, ADSP_FASTRPC_REASM_MAX));
    assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA_CONT, 11u, 0u, q_sz, 0u, 100u, 0u));
    assert(adsp_fastrpc_rx_count == 2u);
    assert(strstr(g_logs[rx_log_idx + 1u], "oversize total=612") != NULL);

    for (uint32_t i = 0; i < 20u; i++) {
        memcpy(g_smem + 16u, raw, 8u);
        assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA, 11u, 0u, q_sz, 0u, 8u, 0u));
    }
    assert(adsp_fastrpc_rx_count == 22u);
    assert(g_log_count == rx_log_idx + ADSP_FASTRPC_LOG_MAX);

    puts("PASS: annibale_adsp_fastrpc Stage 1 GLINK probe (ASan+UBSan)");
    return 0;
}
"""


def main() -> None:
    code = HARNESS.replace("__FASTRPC_INC__", FASTRPC_INC.read_text())
    with tempfile.TemporaryDirectory(prefix="adsp-fastrpc-probe-") as d:
        src = Path(d) / "test.c"
        exe = Path(d) / "test"
        src.write_text(code)
        subprocess.run([
            "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()

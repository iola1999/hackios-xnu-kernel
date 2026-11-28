#!/usr/bin/env python3
"""Host ASan+UBSan tests for annibale_adsp_fastrpc Stage 2 (SMMU, GLINK, INIT_CREATE, apps_std)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ANNIBALE_DIR = ROOT / "xnu/files/bsd/dev/annibale"

HARNESS = r"""
#define ADSP_FASTRPC_HOST_TEST 1

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLINK_INTENT        5u
#define GLINK_RX_INTENT_REQ 7u
#define GLINK_TX_DATA       9u
#define GLINK_TX_DATA_CONT  12u
#define PAGE_SIZE           4096u
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define Z_WAITOK            0
#define Z_ZERO              1
#define bzero(p, n)         memset((p), 0, (n))
#define kalloc_type(T, fl)  ((T *)calloc(1, sizeof(T)))
#define kfree_type(T, p)    free(p)
#ifndef __DECONST
#define __DECONST(type, p)  ((type)(uintptr_t)(const void *)(p))
#endif

typedef int lck_mtx_t;
static lck_mtx_t glink_intent_lock;
static lck_mtx_t glink_rx_lock;
static void lck_mtx_lock(lck_mtx_t *m) { (void)m; }
static void lck_mtx_unlock(lck_mtx_t *m) { (void)m; }
#define kalloc_data(sz, fl) calloc(1, (sz))
#define kfree_data(p, sz)   free(p)

struct glink_remote_intent {
    struct glink_remote_intent *next;
    uint32_t id, size;
    bool free;
};
struct glink_intent_pool {
    struct glink_remote_intent *head;
};
static bool adsp_hw_up = true;
static uint64_t now_us(void) { return 0; }
static void sleep_us(uint32_t us) { (void)us; }
static void glink_poll_rx(void) {}
static uint32_t g_cmd2_req_count = 0;
static uint16_t g_cmd2_last_cmd = 0, g_cmd2_last_p1 = 0;
static uint32_t g_cmd2_last_p2 = 0;
static void glink_send_cmd2(uint16_t cmd, uint16_t p1, uint32_t p2) {
    g_cmd2_last_cmd = cmd;
    g_cmd2_last_p1 = p1;
    g_cmd2_last_p2 = p2;
    if (cmd == GLINK_RX_INTENT_REQ) {
        g_cmd2_req_count++;
    }
}

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
static uint32_t g_intent_sizes[32];
static uint32_t g_log_count = 0;
static char g_logs[128][192];
static uint8_t g_smem[0x2000];

struct tx_record {
    uint16_t cmd;
    uint16_t cid;
    uint32_t iid;
    uint32_t chunk;
    uint32_t left;
    uint8_t  payload[512];
};
static struct tx_record g_tx_recs[64];
static uint32_t g_tx_rec_count = 0;

/* Virtual SMMU registers (4 KiB GR0 + 4 KiB GR1 + 16 x 4 KiB CBs = 72 KiB, 1-to-1 pages=1? Use pages=2, pg_sz=0x1000) */
static uint32_t g_smmu_regs[0x20000 / 4u];

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log_count < 128u) {
        vsnprintf(g_logs[g_log_count], sizeof(g_logs[0]), fmt, ap);
    }
    g_log_count++;
    va_end(ap);
}

static bool adsp_smmu_map(void) { return true; }
static uint32_t adsp_smmu_rd(uint32_t off) {
    assert(off + 4u <= sizeof(g_smmu_regs) && (off & 3u) == 0);
    return g_smmu_regs[off / 4u];
}
static void adsp_smmu_wr(uint32_t off, uint32_t val) {
    assert(off + 4u <= sizeof(g_smmu_regs) && (off & 3u) == 0);
    g_smmu_regs[off / 4u] = val;
}

static uint8_t g_heap_mem[0x00c00000u];
static uint8_t g_invoke_mem[0x10000u] __attribute__((aligned(4096)));
static uint64_t g_fake_heap_pa = 0x88000000ULL;
static uint64_t g_fake_buf_pa = 0x89000000ULL;
static uint64_t g_now_us_val = 0;

static uint64_t host_now_us(void) { return g_now_us_val; }
static void adsp_fastrpc_cache_clean(uint64_t va, uint32_t size) { (void)va; (void)size; }
static void adsp_fastrpc_free_buf(void);
static bool adsp_fastrpc_assign_heap(uint64_t pa);
static bool adsp_fastrpc_ensure_mem(void);
static struct { uint32_t cnt, v0, v1, perm; } g_tz_calls[4];
static uint32_t g_tz_call_count = 0;
static uint32_t g_tz_succeed_on_idx = 0u;
static int64_t annibale_tz_assign_mem_perms(uint64_t pa, uint64_t sz, const uint32_t *vmids,
    const uint32_t *perms, uint32_t cnt, uint64_t *r1) {
    assert(pa != 0 && sz == 0x00c00000u && vmids != NULL && perms != NULL && cnt >= 1u && cnt <= 2u);
    if (g_tz_call_count < 4u) {
        g_tz_calls[g_tz_call_count].cnt = cnt;
        g_tz_calls[g_tz_call_count].v0 = vmids[0];
        g_tz_calls[g_tz_call_count].v1 = cnt > 1u ? vmids[1] : 0u;
        g_tz_calls[g_tz_call_count].perm = perms[0];
    }
    uint32_t idx = g_tz_call_count++;
    if (idx == g_tz_succeed_on_idx) {
        if (r1) *r1 = 0;
        return 0;
    }
    if (r1) *r1 = 0x56u;
    return -2;
}

struct mock_file { const char *path; const char *data; size_t size; bool vfs_only; };
static const char k_sensors_list[] = "ambient_light\nproximity\n";
static const char k_gls_cfg[] = "{\"config\":{\"bus\":13}}\n";
static const struct mock_file g_mock_files[] = {
    { "sensors/sensors_list.txt", k_sensors_list, sizeof(k_sensors_list) - 1u, false },
    { "sensors/hwMisList.txt", "", 0u, false },
    { "/mnt/vendor/persist/sensors/registry/registry/sm8750_gls6151.json.glsx_platform.config", k_gls_cfg, sizeof(k_gls_cfg) - 1u, true },
};

static bool adsp_fastrpc_try_vfs(const char *path, const uint8_t **data_out, uint64_t *size_out, bool *owned_out) {
    for (size_t i = 0; i < sizeof(g_mock_files) / sizeof(g_mock_files[0]); i++) {
        if (strcmp(path, g_mock_files[i].path) == 0) {
            *data_out = (const uint8_t *)g_mock_files[i].data;
            *size_out = g_mock_files[i].size;
            *owned_out = false;
            return true;
        }
    }
    return false;
}
static bool adsp_fastrpc_try_fw(const char *rel, const uint8_t **data_out, uint64_t *size_out, bool *owned_out) {
    for (size_t i = 0; i < sizeof(g_mock_files) / sizeof(g_mock_files[0]); i++) {
        if (!g_mock_files[i].vfs_only && strcmp(rel, g_mock_files[i].path) == 0) {
            *data_out = (const uint8_t *)g_mock_files[i].data;
            *size_out = g_mock_files[i].size;
            *owned_out = false;
            return true;
        }
    }
    return false;
}
struct adsp_fastrpc_fd;
static void adsp_fastrpc_free_fd_data(struct adsp_fastrpc_fd *slot) { (void)slot; }

static bool glink_q_write(uint32_t q_off, uint32_t q_sz, uint32_t *write_at, const void *buf, uint32_t len) {
    (void)q_off; (void)q_sz; (void)write_at;
    const uint32_t *w = (const uint32_t *)buf;
    uint16_t cmd = (uint16_t)(w[0] & 0xffffu);
    uint16_t cid = (uint16_t)(w[0] >> 16);
    if (cmd == GLINK_INTENT && len == 16u && g_intent_count < 32u) {
        g_intent_sizes[g_intent_count++] = w[2];
    } else if ((cmd == GLINK_TX_DATA || cmd == GLINK_TX_DATA_CONT) && len >= 16u && g_tx_rec_count < 64u) {
        struct tx_record *rec = &g_tx_recs[g_tx_rec_count++];
        rec->cmd = cmd;
        rec->cid = cid;
        rec->iid = w[1];
        rec->chunk = w[2];
        rec->left = w[3];
        assert(rec->chunk <= sizeof(rec->payload));
        assert(16u + rec->chunk == len);
        memcpy(rec->payload, (const uint8_t *)buf + 16u, rec->chunk);
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

#include "annibale_adsp_intents.inc"
#include "annibale_adsp_fastrpc.inc"

static bool adsp_fastrpc_ensure_mem(void) {
    (void)g_heap_mem; (void)annibale_tz_assign_mem_perms; (void)adsp_hw_up;
    adsp_fastrpc_heap_va = (uint64_t)(uintptr_t)g_heap_mem;
    adsp_fastrpc_heap_pa = g_fake_heap_pa;
    adsp_fastrpc_heap_ready = true;
    adsp_fastrpc_buf_va = (uint64_t)(uintptr_t)g_invoke_mem;
    adsp_fastrpc_buf_pa = g_fake_buf_pa;
    adsp_fastrpc_buf_ready = true;
    return true;
}

static void adsp_fastrpc_free_buf(void) {
    adsp_fastrpc_buf_va = 0;
    adsp_fastrpc_buf_pa = 0;
    adsp_fastrpc_buf_ready = false;
}

static void init_mock_smmu(void) {
    memset(g_smmu_regs, 0, sizeof(g_smmu_regs));
    g_smmu_regs[ADSP_SMMU_IDR0 / 4u] = 64u;
    g_smmu_regs[ADSP_SMMU_IDR1 / 4u] = 16u | (0u << 28); /* 16 banks, pages=2, 4 KiB */
    /* handoff-smrs=<0x800 0x02> at slot 0 -> bank 0 */
    g_smmu_regs[ADSP_SMMU_SMR(0) / 4u] = ADSP_SMMU_SMR_VALID | (0x0002u << 16) | 0x0800u;
    g_smmu_regs[ADSP_SMMU_S2CR(0) / 4u] = 0u;
    g_smmu_regs[ADSP_SMMU_CBAR(0x1000u, 0u) / 4u] = ADSP_SMMU_CBAR_S1_BYPASS;
    g_smmu_regs[ADSP_SMMU_CBA2R(0x1000u, 0u) / 4u] = 1u;
    /* audio_reach at slot 1 -> bank 15, plus pre-claimed bank 14 via CBAR */
    g_smmu_regs[ADSP_SMMU_SMR(1) / 4u] = ADSP_SMMU_SMR_VALID | 0x1001u;
    g_smmu_regs[ADSP_SMMU_S2CR(1) / 4u] = 15u;
    g_smmu_regs[ADSP_SMMU_CBAR(0x1000u, 15u) / 4u] = ADSP_SMMU_CBAR_S1_BYPASS;
    g_smmu_regs[ADSP_SMMU_CBA2R(0x1000u, 15u) / 4u] = 1u;
    g_smmu_regs[ADSP_SMMU_CBAR(0x1000u, 14u) / 4u] = 0x20000u;
}

static void feed_adsp_msg(const void *data, uint32_t len) {
    assert(len <= sizeof(g_smem) - 32u);
    memcpy(g_smem + 16u, data, len);
    assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA, 11u, 0u, sizeof(g_smem), 0u, len, 0u));
}

static uint32_t sbuf_pack_in(uint8_t *dst, uint32_t n_in, const void **in_ptrs, const uint32_t *in_lens,
    uint32_t n_out, const uint32_t *out_lens) {
    uint32_t pos = 0;
    for (uint32_t i = 0; i < n_in; i++) {
        memcpy(dst + pos, &in_lens[i], 4u);
        pos += 4u;
        if (in_lens[i] > 0) {
            pos = (pos + 7u) & ~7u;
            memcpy(dst + pos, in_ptrs[i], in_lens[i]);
            pos += in_lens[i];
        }
    }
    for (uint32_t i = 0; i < n_out; i++) {
        memcpy(dst + pos, &out_lens[i], 4u);
        pos += 4u;
    }
    return pos;
}

static void sbuf_read_out(const uint8_t *src, uint32_t src_len, uint32_t n_out,
    const uint8_t **out_ptrs, uint32_t *out_lens) {
    uint32_t pos = 0;
    for (uint32_t i = 0; i < n_out; i++) {
        assert(pos + 4u <= src_len);
        memcpy(&out_lens[i], src + pos, 4u);
        pos += 4u;
        out_ptrs[i] = NULL;
        if (out_lens[i] > 0) {
            pos = (pos + 7u) & ~7u;
            assert(pos + out_lens[i] <= src_len);
            out_ptrs[i] = src + pos;
            pos += out_lens[i];
        }
    }
}

static void push_next2_request(uint32_t rev_ctx, uint32_t rev_handle, uint32_t rev_sc,
    const uint8_t *inbuf, uint32_t in_len) {
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_LISTENER_NEXT2);
    assert(in_len <= adsp_fastrpc_next2_out1_cap);
    uint32_t out0[4] = { rev_ctx, rev_handle, rev_sc, in_len };
    memcpy(g_invoke_mem + adsp_fastrpc_next2_out0_off, out0, 16u);
    if (in_len > 0) {
        memcpy(g_invoke_mem + adsp_fastrpc_next2_out1_off, inbuf, in_len);
    }
    struct fastrpc_wire_rsp rsp = { adsp_fastrpc_inflight_ctx, 0, 0 };
    feed_adsp_msg(&rsp, sizeof(rsp));
    adsp_fastrpc_service();
}

int main(void) {
    adsp_fastrpc_test_now_us = host_now_us;

    /* 1. SMMU compute-cb@2 S1 bypass, handoff-smrs & TYPE_TRANS bank avoidance, conflict safety. */
    init_mock_smmu();
    adsp_fastrpc_reset();
    assert(adsp_fastrpc_smmu_configure());
    assert(adsp_fastrpc_smmu_bank == 13u);
    assert(adsp_fastrpc_smmu_slots[0] == 2 && adsp_fastrpc_smmu_slots[1] == 3);
    assert(g_smmu_regs[ADSP_SMMU_SMR(0) / 4u] == (ADSP_SMMU_SMR_VALID | (0x0002u << 16) | 0x0800u));
    assert(g_smmu_regs[ADSP_SMMU_SMR(1) / 4u] == (ADSP_SMMU_SMR_VALID | 0x1001u));
    assert(g_smmu_regs[ADSP_SMMU_SMR(2) / 4u] == (ADSP_SMMU_SMR_VALID | (0x0080u << 16) | 0x1004u));
    assert(g_smmu_regs[ADSP_SMMU_SMR(3) / 4u] == (ADSP_SMMU_SMR_VALID | (0x0020u << 16) | 0x1044u));
    assert(g_smmu_regs[ADSP_SMMU_S2CR(2) / 4u] == 13u && g_smmu_regs[ADSP_SMMU_S2CR(3) / 4u] == 13u);
    assert(g_smmu_regs[ADSP_SMMU_CBAR(0x1000u, 13u) / 4u] == ADSP_SMMU_CBAR_S1_BYPASS);
    assert(g_smmu_regs[ADSP_SMMU_CBA2R(0x1000u, 13u) / 4u] == 1u);
    adsp_fastrpc_smmu_teardown();
    assert(g_smmu_regs[ADSP_SMMU_SMR(2) / 4u] == 0u && g_smmu_regs[ADSP_SMMU_SMR(3) / 4u] == 0u);
    assert(g_smmu_regs[ADSP_SMMU_CBAR(0x1000u, 13u) / 4u] == 0u);
    assert(g_smmu_regs[ADSP_SMMU_SMR(0) / 4u] == (ADSP_SMMU_SMR_VALID | (0x0002u << 16) | 0x0800u));
    assert(g_smmu_regs[ADSP_SMMU_SMR(1) / 4u] == (ADSP_SMMU_SMR_VALID | 0x1001u));

    /* Conflict with existing overlapping stream rejects safely. */
    g_smmu_regs[ADSP_SMMU_SMR(5) / 4u] = ADSP_SMMU_SMR_VALID | 0x1084u;
    assert(!adsp_fastrpc_smmu_configure());
    g_smmu_regs[ADSP_SMMU_SMR(5) / 4u] = 0u;

    /* Respect first_s1 lower bound, geometry limit, and VMID low-byte CBAR reuse. */
    init_mock_smmu();
    adsp_fastrpc_reset();
    g_smmu_regs[ADSP_SMMU_IDR1 / 4u] = 16u | (14u << 16) | (0u << 28); /* first_s1=14, banks 14 & 15 already used */
    assert(!adsp_fastrpc_smmu_configure());
    g_smmu_regs[ADSP_SMMU_IDR1 / 4u] = 16u | (7u << 28) | (1u << 31); /* (256+16)*64 KiB > ADSP_APPS_SMMU_SIZE */
    assert(!adsp_fastrpc_smmu_configure());
    init_mock_smmu();
    adsp_fastrpc_reset();
    g_smmu_regs[ADSP_SMMU_SMR(2) / 4u] = ADSP_SMMU_SMR_VALID | (0x0080u << 16) | 0x1004u;
    g_smmu_regs[ADSP_SMMU_S2CR(2) / 4u] = 13u;
    g_smmu_regs[ADSP_SMMU_SMR(3) / 4u] = ADSP_SMMU_SMR_VALID | (0x0020u << 16) | 0x1044u;
    g_smmu_regs[ADSP_SMMU_S2CR(3) / 4u] = 13u;
    g_smmu_regs[ADSP_SMMU_CBAR(0x1000u, 13u) / 4u] = ADSP_SMMU_CBAR_S1_BYPASS | 0x42u;
    g_smmu_regs[ADSP_SMMU_CBA2R(0x1000u, 13u) / 4u] = 1u;
    assert(adsp_fastrpc_smmu_configure());
    assert(adsp_fastrpc_smmu_bank == 13u && !adsp_fastrpc_smmu_bank_owned);
    adsp_fastrpc_smmu_teardown();
    assert((g_smmu_regs[ADSP_SMMU_CBAR(0x1000u, 13u) / 4u] & ~0xffu) == ADSP_SMMU_CBAR_S1_BYPASS);
    init_mock_smmu();

    /* 2. Stage 1 GLINK open, intents, ring-wrap reassembly, oversize guard. */
    adsp_fastrpc_reset();
    adsp_fastrpc_request_open();
    assert(g_open_calls == 1u && strcmp(g_open_name, "fastrpcglink-apps-dsp") == 0);
    adsp_fastrpc_on_open("unrelated_chan", 99u);
    assert(adsp_fastrpc_their_cid == 0);
    adsp_fastrpc_on_open("fastrpcglink-apps-dsp", 11u);
    adsp_fastrpc_on_open_ack(ADSP_FASTRPC_OUR_CID);
    assert(adsp_fastrpc_ready && g_intent_count == ADSP_FASTRPC_INTENT_COUNT);
    assert(g_intent_sizes[0] == ADSP_FASTRPC_INTENT_SIZE);
    assert(adsp_glink_our_cid_for(11u) == ADSP_FASTRPC_OUR_CID);
    assert(adsp_glink_our_cid_for(9u) == adsp_ipcrtr_our_cid);
    assert(adsp_glink_our_cid_for(99u) == 0);
    adsp_fastrpc_on_intent_req(11u);
    assert(g_intent_count == 2u * ADSP_FASTRPC_INTENT_COUNT);
    assert(adsp_fastrpc_intents.head == NULL);
    (void)glink_rx_lock;
    (void)glink_take_their_intent;

    uint32_t probe_msg[6] = { 0x11223344u, 0x55667788u, 0x00000003u, 0x01020304u, 0xaabbccddu, 0xdeadbeefu };
    uint8_t raw[24];
    memcpy(raw, probe_msg, sizeof(raw));
    uint32_t q_sz = sizeof(g_smem), r0 = q_sz - 24u;
    for (uint32_t i = 0; i < 12u; i++) g_smem[(r0 + 16u + i) % q_sz] = raw[i];
    assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA, 11u, 0u, q_sz, r0, 12u, 12u));
    for (uint32_t i = 0; i < 12u; i++) g_smem[(64u + 16u + i) % q_sz] = raw[12u + i];
    assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA_CONT, 11u, 0u, q_sz, 64u, 12u, 0u));
    assert(adsp_fastrpc_rx_count == 1u && adsp_fastrpc_has_pending());
    assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA, 11u, 0u, q_sz, 0u, 100u, ADSP_FASTRPC_REASM_MAX));
    assert(adsp_fastrpc_rx_chunk(GLINK_TX_DATA_CONT, 11u, 0u, q_sz, 0u, 100u, 0u));
    assert(adsp_fastrpc_rx_count == 2u);

    /* 3. Missing remote intents trigger throttled GLINK_RX_INTENT_REQ, then INIT_CREATE_STATIC + listener. */
    g_cmd2_req_count = 0;
    g_now_us_val = 100000ULL;
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_IDLE);
    assert(g_cmd2_req_count == 1u && g_cmd2_last_cmd == GLINK_RX_INTENT_REQ);
    assert(g_cmd2_last_p1 == ADSP_FASTRPC_OUR_CID && g_cmd2_last_p2 == ADSP_FASTRPC_REMOTE_INTENT_MIN);
    g_now_us_val = 200000ULL;
    adsp_fastrpc_service();
    assert(g_cmd2_req_count == 1u);
    g_now_us_val = 600000ULL;
    adsp_fastrpc_service();
    assert(g_cmd2_req_count == 2u);

    adsp_fastrpc_on_intent(11u, 101u, 512u);
    adsp_fastrpc_on_intent(11u, 102u, 512u);
    g_tx_rec_count = 0;
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_STATIC);
    assert(g_tx_rec_count == 1u && g_tx_recs[0].chunk == sizeof(struct fastrpc_wire_msg));
    adsp_fastrpc_on_tx_done(g_tx_recs[0].iid, true);

    struct fastrpc_wire_msg init_msg;
    memcpy(&init_msg, g_tx_recs[0].payload, sizeof(init_msg));
    assert(init_msg.handle == ADSP_FASTRPC_INIT_HANDLE);
    assert(init_msg.sc == ADSP_FASTRPC_SCALARS(ADSP_FASTRPC_RMID_INIT_CREATE_STATIC, 3, 0));
    assert((init_msg.addr >> 32) == ADSP_FASTRPC_COMPUTE_SID);
    const struct fastrpc_remote_buf *rpra = (const struct fastrpc_remote_buf *)g_invoke_mem;
    const struct fastrpc_phy_page *pages = (const struct fastrpc_phy_page *)(
        (const struct fastrpc_invoke_buf *)(rpra + 3) + 3);
    assert(rpra[0].len == sizeof(struct fastrpc_init_create_static_inbuf));
    assert(rpra[1].len == sizeof("sensorspd"));
    assert(rpra[2].len == sizeof(struct fastrpc_phy_page));
    assert((pages[0].addr >> 32) == ADSP_FASTRPC_COMPUTE_SID);
    struct fastrpc_phy_page heap_arg;
    memcpy(&heap_arg, g_invoke_mem + (uint32_t)(rpra[2].pv - (((uint64_t)ADSP_FASTRPC_COMPUTE_SID << 32) | g_fake_buf_pa)), sizeof(heap_arg));
    assert(heap_arg.addr == g_fake_heap_pa && heap_arg.size == ADSP_FASTRPC_HEAP_SIZE);

    /* INIT_CREATE_STATIC succeeds -> adsp_listener_init2 -> adsp_listener_next2 */
    struct fastrpc_wire_rsp rsp = { init_msg.ctx, 0, 0 };
    feed_adsp_msg(&rsp, sizeof(rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_LISTENER_INIT2);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    rsp.ctx = adsp_fastrpc_inflight_ctx;
    rsp.retval = 0;
    feed_adsp_msg(&rsp, sizeof(rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_LISTENER_NEXT2);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    /* 4. Reverse RPC: apps_remotectl open("apps_std") */
    uint8_t sbuf_in[2048];
    const char uri[] = "file:///libapps_std_skel.so?apps_std_skel_handle_invoke";
    uint32_t rc_open_hdr[2] = { sizeof(uri), 64u };
    const void *rc_ptrs[2] = { rc_open_hdr, uri };
    uint32_t rc_lens[2] = { sizeof(rc_open_hdr), sizeof(uri) };
    uint32_t rc_out[2] = { 8u, 64u };
    uint32_t slen = sbuf_pack_in(sbuf_in, 2u, rc_ptrs, rc_lens, 2u, rc_out);
    push_next2_request(0x101u, 0u, ADSP_FASTRPC_SCALARS(0, 2, 2), sbuf_in, slen);
    assert(adsp_fastrpc_prev_rev_ctx == 0x101u && adsp_fastrpc_prev_rev_rc == 0);
    const uint8_t *out_ptrs[4] = { 0 };
    uint32_t out_lens[4] = { 0 };
    sbuf_read_out(adsp_fastrpc_prev_outbufs, adsp_fastrpc_prev_outbufs_len, 2u, out_ptrs, out_lens);
    int32_t rev_h = 0, rev_err = -1;
    memcpy(&rev_h, out_ptrs[0], 4u);
    memcpy(&rev_err, out_ptrs[0] + 4u, 4u);
    assert(rev_h == ADSP_FASTRPC_REV_APPS_STD && rev_err == 0);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    /* 5. apps_std fopen + flen + fread + fclose on /vendor/etc/sensors/config/sensors_list.txt */
    const char sfile[] = "/vendor/etc/sensors/config/sensors_list.txt";
    const char rmode[] = "r";
    uint32_t fopen_hdr[2] = { sizeof(sfile), sizeof(rmode) };
    const void *fo_ptrs[3] = { fopen_hdr, sfile, rmode };
    uint32_t fo_lens[3] = { sizeof(fopen_hdr), sizeof(sfile), sizeof(rmode) };
    uint32_t fo_out[1] = { 4u };
    slen = sbuf_pack_in(sbuf_in, 3u, fo_ptrs, fo_lens, 1u, fo_out);
    push_next2_request(0x102u, (uint32_t)rev_h, ADSP_FASTRPC_SCALARS(APPS_STD_OP_FOPEN, 3, 1), sbuf_in, slen);
    assert(adsp_fastrpc_prev_rev_rc == 0);
    sbuf_read_out(adsp_fastrpc_prev_outbufs, adsp_fastrpc_prev_outbufs_len, 1u, out_ptrs, out_lens);
    int32_t fd = 0;
    memcpy(&fd, out_ptrs[0], 4u);
    assert(fd >= ADSP_FASTRPC_FD_BASE);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    const void *fl_ptrs[1] = { &fd };
    uint32_t fl_lens[1] = { 4u };
    uint32_t fl_out[1] = { 8u };
    slen = sbuf_pack_in(sbuf_in, 1u, fl_ptrs, fl_lens, 1u, fl_out);
    push_next2_request(0x103u, (uint32_t)rev_h, ADSP_FASTRPC_SCALARS(APPS_STD_OP_FLEN, 1, 1), sbuf_in, slen);
    assert(adsp_fastrpc_prev_rev_rc == 0);
    sbuf_read_out(adsp_fastrpc_prev_outbufs, adsp_fastrpc_prev_outbufs_len, 1u, out_ptrs, out_lens);
    uint64_t fsz = 0;
    memcpy(&fsz, out_ptrs[0], 8u);
    assert(fsz == sizeof(k_sensors_list) - 1u);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    uint32_t fr_hdr[2] = { (uint32_t)fd, 64u };
    const void *fr_ptrs[1] = { fr_hdr };
    uint32_t fr_lens[1] = { sizeof(fr_hdr) };
    uint32_t fr_out[2] = { 8u, 64u };
    slen = sbuf_pack_in(sbuf_in, 1u, fr_ptrs, fr_lens, 2u, fr_out);
    push_next2_request(0x104u, (uint32_t)rev_h, ADSP_FASTRPC_SCALARS(APPS_STD_OP_FREAD, 1, 2), sbuf_in, slen);
    assert(adsp_fastrpc_prev_rev_rc == 0);
    sbuf_read_out(adsp_fastrpc_prev_outbufs, adsp_fastrpc_prev_outbufs_len, 2u, out_ptrs, out_lens);
    int32_t bytes_read = 0, b_eof = 0;
    memcpy(&bytes_read, out_ptrs[0], 4u);
    memcpy(&b_eof, out_ptrs[0] + 4u, 4u);
    assert(bytes_read == (int32_t)(sizeof(k_sensors_list) - 1u) && b_eof == 1);
    assert(memcmp(out_ptrs[1], k_sensors_list, (size_t)bytes_read) == 0);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    slen = sbuf_pack_in(sbuf_in, 1u, fl_ptrs, fl_lens, 0u, NULL);
    push_next2_request(0x105u, (uint32_t)rev_h, ADSP_FASTRPC_SCALARS(APPS_STD_OP_FCLOSE, 1, 0), sbuf_in, slen);
    assert(adsp_fastrpc_prev_rev_rc == 0);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    /* 6. stat on /mnt/vendor/persist/sensors/registry/... and ENOENT logging on missing file. */
    const char reg_cfg[] = "/mnt/vendor/persist/sensors/registry/registry/sm8750_gls6151.json.glsx_platform.config";
    uint32_t st_hdr[2] = { APPS_STD_EXT_STAT, sizeof(reg_cfg) };
    const void *st_ptrs[2] = { st_hdr, reg_cfg };
    uint32_t st_lens[2] = { sizeof(st_hdr), sizeof(reg_cfg) };
    uint32_t st_out[1] = { 96u };
    slen = sbuf_pack_in(sbuf_in, 2u, st_ptrs, st_lens, 1u, st_out);
    push_next2_request(0x106u, (uint32_t)rev_h, ADSP_FASTRPC_SCALARS(APPS_STD_OP_EXT, 2, 1), sbuf_in, slen);
    assert(adsp_fastrpc_prev_rev_rc == 0);
    sbuf_read_out(adsp_fastrpc_prev_outbufs, adsp_fastrpc_prev_outbufs_len, 1u, out_ptrs, out_lens);
    struct apps_std_stat_wire st_wire;
    memcpy(&st_wire, out_ptrs[0], sizeof(st_wire));
    assert(st_wire.size == sizeof(k_gls_cfg) - 1u);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    const char missing_path[] = "/vendor/etc/sensors/config/unknown_sensor_99.json";
    uint32_t mf_hdr[2] = { sizeof(missing_path), sizeof(rmode) };
    const void *mf_ptrs[3] = { mf_hdr, missing_path, rmode };
    uint32_t mf_lens[3] = { sizeof(mf_hdr), sizeof(missing_path), sizeof(rmode) };
    uint32_t before_missing = adsp_fastrpc_missing_logs;
    slen = sbuf_pack_in(sbuf_in, 3u, mf_ptrs, mf_lens, 1u, fo_out);
    push_next2_request(0x107u, (uint32_t)rev_h, ADSP_FASTRPC_SCALARS(APPS_STD_OP_FOPEN, 3, 1), sbuf_in, slen);
    assert(adsp_fastrpc_prev_rev_rc == ENOENT);
    assert(adsp_fastrpc_missing_logs == before_missing + 1u);
    assert(strstr(g_logs[g_log_count - 1u], "unknown_sensor_99.json") != NULL);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    /* 7. Oversize/malformed reverse-RPC guard + multi-fragment TX test. */
    push_next2_request(0x108u, (uint32_t)rev_h, ADSP_FASTRPC_SCALARS(APPS_STD_OP_FOPEN, 3, 1), sbuf_in, 0u);
    assert(adsp_fastrpc_prev_rev_rc == EINVAL);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    adsp_fastrpc_on_intent(11u, 103u, 1500u);
    uint8_t big_tx[1200];
    memset(big_tx, 0x5a, sizeof(big_tx));
    g_tx_rec_count = 0;
    assert(adsp_fastrpc_tx_send(big_tx, sizeof(big_tx)));
    assert(g_tx_rec_count == 3u);
    assert(g_tx_recs[0].cmd == GLINK_TX_DATA && g_tx_recs[0].chunk == 512u && g_tx_recs[0].left == 688u);
    assert(g_tx_recs[1].cmd == GLINK_TX_DATA_CONT && g_tx_recs[1].chunk == 512u && g_tx_recs[1].left == 176u);
    assert(g_tx_recs[2].cmd == GLINK_TX_DATA_CONT && g_tx_recs[2].chunk == 176u && g_tx_recs[2].left == 0u);

    adsp_fastrpc_reset();
    assert(g_smmu_regs[ADSP_SMMU_SMR(2) / 4u] == 0u && !adsp_fastrpc_smmu_ready && !adsp_fastrpc_buf_ready);

    /* 8. Exhausting GLINK_RX_INTENT_REQ retries transitions cleanly to FAILED. */
    adsp_fastrpc_on_open("fastrpcglink-apps-dsp", 11u);
    adsp_fastrpc_on_open_ack(ADSP_FASTRPC_OUR_CID);
    g_cmd2_req_count = 0;
    for (uint32_t i = 0; i <= ADSP_FASTRPC_INTENT_REQ_RETRIES; i++) {
        g_now_us_val += ADSP_FASTRPC_INTENT_REQ_INTERVAL_US;
        adsp_fastrpc_service();
    }
    assert(g_cmd2_req_count == ADSP_FASTRPC_INTENT_REQ_RETRIES);
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_FAILED);
    adsp_fastrpc_reset();

    /* 9. lowdma heap/staging picker respects WiFi ceiling, modem ceiling, 4 MiB alignment, and bounds. */
    uint64_t heap_pa = 0, buf_pa = 0;
    assert(adsp_fastrpc_lowdma_pick(0xc772d000ULL, 128ULL, &heap_pa, &buf_pa));
    assert((heap_pa & 0x3fffffULL) == 0ULL);
    assert(heap_pa >= ((0xc772d000ULL + 0x06000000ULL + 0xfffffULL) & ~0xfffffULL));
    assert(buf_pa >= ((0xc772d000ULL + 0x06000000ULL + 0xfffffULL) & ~0xfffffULL));
    assert(heap_pa + ADSP_FASTRPC_HEAP_SIZE <= 0xcf72d000ULL - 0x0012c000ULL);
    assert(buf_pa + ADSP_FASTRPC_INVOKE_BUF_SZ <= 0xcf72d000ULL - 0x0012c000ULL);
    assert(buf_pa + ADSP_FASTRPC_INVOKE_BUF_SZ <= heap_pa || heap_pa + ADSP_FASTRPC_HEAP_SIZE <= buf_pa);
    assert(!adsp_fastrpc_lowdma_pick(0ULL, 128ULL, &heap_pa, &buf_pa));
    assert(!adsp_fastrpc_lowdma_pick(0xc772d000ULL, 95ULL, &heap_pa, &buf_pa));
    assert(!adsp_fastrpc_lowdma_pick(0xc772d000ULL, 1025ULL, &heap_pa, &buf_pa));
    assert(!adsp_fastrpc_lowdma_pick(0xf9000000ULL, 128ULL, &heap_pa, &buf_pa));

    /* 10. adsp_fastrpc_assign_heap tries dual-VMID {0x16,0x25} RWX(7), then RW(6), then single-VMID 0x25/0x16 RWX(7). */
    g_tz_call_count = 0;
    g_tz_succeed_on_idx = 0u;
    assert(adsp_fastrpc_assign_heap(0xcdc00000ULL));
    assert(g_tz_call_count == 1u && g_tz_calls[0].cnt == 2u &&
        g_tz_calls[0].v0 == 0x16u && g_tz_calls[0].v1 == 0x25u && g_tz_calls[0].perm == 7u);

    g_tz_call_count = 0;
    g_tz_succeed_on_idx = 3u;
    assert(adsp_fastrpc_assign_heap(0xcdc00000ULL));
    assert(g_tz_call_count == 4u);
    assert(g_tz_calls[0].cnt == 2u && g_tz_calls[0].v0 == 0x16u && g_tz_calls[0].v1 == 0x25u && g_tz_calls[0].perm == 7u);
    assert(g_tz_calls[1].cnt == 2u && g_tz_calls[1].v0 == 0x16u && g_tz_calls[1].v1 == 0x25u && g_tz_calls[1].perm == 6u);
    assert(g_tz_calls[2].cnt == 1u && g_tz_calls[2].v0 == 0x25u && g_tz_calls[2].perm == 7u);
    assert(g_tz_calls[3].cnt == 1u && g_tz_calls[3].v0 == 0x16u && g_tz_calls[3].perm == 7u);

    g_tz_call_count = 0;
    g_tz_succeed_on_idx = 0xffffffffu;
    assert(!adsp_fastrpc_assign_heap(0xcdc00000ULL));
    assert(g_tz_call_count == 4u);

    /* 11. sensorspd 0x80000484 falls back to ROOT_PD INIT_ATTACH(0,0,0), then SENSORS_PD INIT_ATTACH_SNS(10,0,0), tracking active_pd. */
    init_mock_smmu();
    adsp_fastrpc_reset();
    g_tz_succeed_on_idx = 0u;
    g_tx_rec_count = 0;
    adsp_fastrpc_on_open("fastrpcglink-apps-dsp", 11u);
    adsp_fastrpc_on_open_ack(ADSP_FASTRPC_OUR_CID);
    adsp_fastrpc_on_intent(11u, 201u, 512u);
    adsp_fastrpc_on_intent(11u, 202u, 512u);
    adsp_fastrpc_on_intent(11u, 203u, 512u);
    adsp_fastrpc_on_intent(11u, 204u, 512u);
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_STATIC);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    /* Case 11a: sensorspd returns 0x80000484 -> ROOT_PD INIT_ATTACH succeeds -> listener uses ROOT_PD (0). */
    struct fastrpc_wire_rsp fb_rsp = { .ctx = adsp_fastrpc_inflight_ctx, .retval = (int32_t)0x80000484u };
    feed_adsp_msg(&fb_rsp, sizeof(fb_rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_ROOT);
    struct fastrpc_wire_msg wmsg;
    memcpy(&wmsg, g_tx_recs[g_tx_rec_count - 1u].payload, sizeof(wmsg));
    assert((wmsg.ctx & 0xffu) == ADSP_FASTRPC_ROOT_PD);
    assert(wmsg.handle == ADSP_FASTRPC_INIT_HANDLE);
    assert(wmsg.sc == ADSP_FASTRPC_SCALARS(ADSP_FASTRPC_RMID_INIT_ATTACH, 0, 0));
    assert(wmsg.addr == 0ULL && wmsg.size == 0ULL);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    fb_rsp = (struct fastrpc_wire_rsp){ .ctx = adsp_fastrpc_inflight_ctx, .retval = 0 };
    feed_adsp_msg(&fb_rsp, sizeof(fb_rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_active_pd == ADSP_FASTRPC_ROOT_PD);
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_LISTENER_INIT2);
    memcpy(&wmsg, g_tx_recs[g_tx_rec_count - 1u].payload, sizeof(wmsg));
    assert((wmsg.ctx & 0xffu) == ADSP_FASTRPC_ROOT_PD && wmsg.handle == ADSP_FASTRPC_LISTENER_HANDLE);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    fb_rsp = (struct fastrpc_wire_rsp){ .ctx = adsp_fastrpc_inflight_ctx, .retval = 0 };
    feed_adsp_msg(&fb_rsp, sizeof(fb_rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_LISTENER_NEXT2);
    memcpy(&wmsg, g_tx_recs[g_tx_rec_count - 1u].payload, sizeof(wmsg));
    assert((wmsg.ctx & 0xffu) == ADSP_FASTRPC_ROOT_PD && wmsg.handle == ADSP_FASTRPC_LISTENER_HANDLE);

    /* Case 11b: sensorspd fails, ROOT_PD fails -> SENSORS_PD INIT_ATTACH_SNS (method 10) succeeds -> listener uses SENSORS_PD (2). */
    init_mock_smmu();
    adsp_fastrpc_reset();
    g_tx_rec_count = 0;
    adsp_fastrpc_on_open("fastrpcglink-apps-dsp", 11u);
    adsp_fastrpc_on_open_ack(ADSP_FASTRPC_OUR_CID);
    adsp_fastrpc_on_intent(11u, 205u, 512u);
    adsp_fastrpc_on_intent(11u, 206u, 512u);
    adsp_fastrpc_on_intent(11u, 207u, 512u);
    adsp_fastrpc_on_intent(11u, 208u, 512u);
    adsp_fastrpc_on_intent(11u, 209u, 512u);
    adsp_fastrpc_service();
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);
    fb_rsp = (struct fastrpc_wire_rsp){ .ctx = adsp_fastrpc_inflight_ctx, .retval = (int32_t)0x80000484u };
    feed_adsp_msg(&fb_rsp, sizeof(fb_rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_ROOT);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    fb_rsp = (struct fastrpc_wire_rsp){ .ctx = adsp_fastrpc_inflight_ctx, .retval = (int32_t)0x80000410u };
    feed_adsp_msg(&fb_rsp, sizeof(fb_rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_SNS);
    memcpy(&wmsg, g_tx_recs[g_tx_rec_count - 1u].payload, sizeof(wmsg));
    assert((wmsg.ctx & 0xffu) == ADSP_FASTRPC_SENSORS_PD);
    assert(wmsg.handle == ADSP_FASTRPC_INIT_HANDLE);
    assert(wmsg.sc == ADSP_FASTRPC_SCALARS(ADSP_FASTRPC_RMID_INIT_ATTACH_SNS, 0, 0));
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);

    fb_rsp = (struct fastrpc_wire_rsp){ .ctx = adsp_fastrpc_inflight_ctx, .retval = 0 };
    feed_adsp_msg(&fb_rsp, sizeof(fb_rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_active_pd == ADSP_FASTRPC_SENSORS_PD);
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_LISTENER_INIT2);
    memcpy(&wmsg, g_tx_recs[g_tx_rec_count - 1u].payload, sizeof(wmsg));
    assert((wmsg.ctx & 0xffu) == ADSP_FASTRPC_SENSORS_PD && wmsg.handle == ADSP_FASTRPC_LISTENER_HANDLE);

    /* Case 11c: both ROOT_PD and SENSORS_PD attach fail -> transitions to FASTRPC_STAGE_FAILED cleanly. */
    init_mock_smmu();
    adsp_fastrpc_reset();
    g_tx_rec_count = 0;
    adsp_fastrpc_on_open("fastrpcglink-apps-dsp", 11u);
    adsp_fastrpc_on_open_ack(ADSP_FASTRPC_OUR_CID);
    adsp_fastrpc_on_intent(11u, 210u, 512u);
    adsp_fastrpc_on_intent(11u, 211u, 512u);
    adsp_fastrpc_on_intent(11u, 212u, 512u);
    adsp_fastrpc_service();
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);
    fb_rsp = (struct fastrpc_wire_rsp){ .ctx = adsp_fastrpc_inflight_ctx, .retval = (int32_t)0x80000484u };
    feed_adsp_msg(&fb_rsp, sizeof(fb_rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_ROOT);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);
    fb_rsp = (struct fastrpc_wire_rsp){ .ctx = adsp_fastrpc_inflight_ctx, .retval = (int32_t)0x80000410u };
    feed_adsp_msg(&fb_rsp, sizeof(fb_rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_WAIT_INIT_SNS);
    adsp_fastrpc_on_tx_done(g_tx_recs[g_tx_rec_count - 1u].iid, true);
    fb_rsp = (struct fastrpc_wire_rsp){ .ctx = adsp_fastrpc_inflight_ctx, .retval = (int32_t)0x80000410u };
    feed_adsp_msg(&fb_rsp, sizeof(fb_rsp));
    adsp_fastrpc_service();
    assert(adsp_fastrpc_stage == FASTRPC_STAGE_FAILED);

    puts("PASS: annibale_adsp_fastrpc Stage 2 (SMMU, GLINK, INIT_CREATE/ATTACH fallbacks, apps_std, ASan+UBSan)");
    return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="adsp-fastrpc-probe-") as d:
        src = Path(d) / "test.c"
        exe = Path(d) / "test"
        src.write_text(HARNESS)
        subprocess.run([
            "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(ANNIBALE_DIR),
            str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()

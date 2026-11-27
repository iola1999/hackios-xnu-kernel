#!/usr/bin/env python3
"""Host regression test (ASan+UBSan) for UFS async completion validation and bounded error logging."""
from pathlib import Path
import subprocess
import tempfile

DRIVER = Path(__file__).resolve().parents[2] / "xnu/files/bsd/dev/annibale"


def extract_block(source, marker, terminator="\n}"):
    start = source.index(marker)
    end = source.index(terminator, start) + len(terminator)
    return source[start:end]


def extract_function(source, name):
    return extract_block(source, "\n" + name + "(").lstrip("\n")


HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SLOTS               32
#define UCD_RESPONSE        512
#define UCD_PRDT            1024
#define UCD_PRDT_ENTRIES    64u
#define BLOCK               4096u
#define OCS_SUCCESS         0x0
#define OCS_INVALID         0xf
#define UPIU_COMMAND        0x01
#define UPIU_RESPONSE       0x21
#define UTRD_INT            (1u << 24)
#define UFSHC_UTRLDBR       0x58
#define B_FUA               0x00000400
#ifndef __unused
#define __unused            __attribute__((unused))
#endif

typedef uintptr_t vm_offset_t;
typedef int lck_mtx_t;

struct buf {
    uint32_t count;
    uint32_t resid;
    int error;
    int biodone;
    uint32_t flags;
};
typedef struct buf *buf_t;

static uint32_t buf_count(buf_t bp) { return bp->count; }
static uint32_t buf_flags(buf_t bp) { return bp->flags; }
static void buf_setresid(buf_t bp, uint32_t r) { bp->resid = r; }
static void buf_seterror(buf_t bp, int e) { bp->error = e; }
static void buf_biodone(buf_t bp) { bp->biodone++; }

struct ufshc_sg {
    uint64_t pa;
    uint32_t len;
};

struct hc_cmd {
    uint8_t upiu[32];
    uint32_t dw0;
};

@HC_SLOT_STRUCT@

static struct hc_slot hc_slots[SLOTS];
static uint32_t hc_free = 0xfffffffeu;
static volatile uint32_t hc_active_slots;
static volatile uint32_t hc_pending_slots;
static uint32_t hc_in_flight;
static uint32_t fake_utrldbr;
static lck_mtx_t hc_lock;

static bool hc_usable = true, hc_in = true, hc_dead = false, hc_closed = false;
static bool hc_write_enabled = true, hc_dma_coherent = true;
static uint64_t hc_requests, hc_errors, hc_bytes;
static uint64_t hc_write_requests, hc_write_errors, hc_write_bytes;
static uint64_t hc_async_completions;
static uint32_t dcache_flushes;

static uint32_t utrd_mem[SLOTS][8];
static uint8_t ucd_mem[SLOTS][4096];

static char log_lines[64][256];
static uint32_t log_count;

static void hc_say(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    if (log_count < 64) {
        vsnprintf(log_lines[log_count], sizeof(log_lines[0]), format, ap);
    }
    log_count++;
    va_end(ap);
}

static void lck_mtx_lock(lck_mtx_t *l) { (void)l; }
static void lck_mtx_unlock(lck_mtx_t *l) { (void)l; }
static void wakeup(void *chan) { (void)chan; }
static uint32_t hc_read(uint32_t reg) {
    assert(reg == UFSHC_UTRLDBR);
    return fake_utrldbr;
}

#define os_atomic_load(p, order) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define os_atomic_xchg(p, v, order) __atomic_exchange_n((p), (v), __ATOMIC_ACQ_REL)
#define os_atomic_or(p, v, order) ((void)__atomic_or_fetch((p), (v), __ATOMIC_RELEASE))
#define os_atomic_and_orig(p, v, order) __atomic_fetch_and((p), (v), __ATOMIC_ACQ_REL)
#define os_atomic_inc(p, order) ((void)__atomic_add_fetch((p), 1, __ATOMIC_RELAXED))
#define os_atomic_inc_orig(p, order) __atomic_fetch_add((p), 1, __ATOMIC_RELAXED)
#define os_atomic_dec(p, order) ((void)__atomic_sub_fetch((p), 1, __ATOMIC_RELAXED))
#define os_atomic_add(p, v, order) ((void)__atomic_add_fetch((p), (v), __ATOMIC_RELAXED))

static void ufshc_dcache_sg_poc(uint64_t pa, uint32_t len, bool clean_only) {
    (void)pa; (void)len; (void)clean_only;
    dcache_flushes++;
}

static bool hc_write_inside(uint8_t lun, uint64_t first_lba, uint32_t blocks) {
    (void)lun; (void)first_lba; (void)blocks;
    return true;
}

static int ufs_build_sg(buf_t bp, struct ufshc_sg *sg, uint32_t max_segs, uint32_t *out_segs) {
    (void)max_segs;
    sg[0].pa = 0x80000000ULL;
    sg[0].len = bp->count;
    *out_segs = 1;
    return 0;
}

static struct hc_slot *hc_take(bool wait) {
    (void)wait;
    uint32_t i = (uint32_t)__builtin_ctz(hc_free);
    hc_free &= ~(1u << i);
    hc_in_flight++;
    struct hc_slot *s = &hc_slots[i];
    s->done = false;
    s->is_async = false;
    s->bp = NULL;
    return s;
}

static void hc_give(struct hc_slot *s) {
    uint32_t slot = (uint32_t)(s - hc_slots);
    hc_free |= (1u << slot);
    hc_in_flight--;
    wakeup(&hc_free);
}

static void hc_make_read(struct hc_cmd *c, uint8_t lun, uint8_t slot, uint32_t lba, uint32_t blocks) {
    (void)lun; (void)lba; (void)blocks;
    memset(c, 0, sizeof(*c));
    c->upiu[0] = UPIU_COMMAND;
    c->upiu[3] = slot;
}

static void hc_make_write(struct hc_cmd *c, uint8_t lun, uint8_t slot, uint32_t lba, uint32_t blocks, bool fua) {
    (void)lun; (void)lba; (void)blocks;
    memset(c, 0, sizeof(*c));
    c->upiu[0] = UPIU_COMMAND;
    c->upiu[3] = slot;
    c->upiu[16] = 0x2a;
    c->upiu[17] = fua ? 0x08 : 0x00;
}

static int hc_place_sg(struct hc_slot *s, const struct hc_cmd *c, const struct ufshc_sg *sg, uint32_t nsegs, uint32_t total_bytes) {
    (void)total_bytes;
    memcpy((void *)s->ucd, c->upiu, 32);
    s->utrd[2] = OCS_INVALID;
    s->utrd[7] = nsegs & 0xffff;
    for (uint32_t k = 0; k < nsegs; k++) {
        volatile uint32_t *prd = s->ucd + (UCD_PRDT + 16 * k) / 4;
        prd[0] = (uint32_t)sg[k].pa;
        prd[1] = (uint32_t)(sg[k].pa >> 32);
        prd[3] = sg[k].len - 1;
    }
    return 0;
}

static int hc_ring(struct hc_slot *s) {
    uint32_t bit = 1u << s->number;
    if (s->is_async) {
        os_atomic_or(&hc_active_slots, bit, release);
    }
    fake_utrldbr |= bit;
    return 0;
}

static void init_slots(void) {
    for (uint32_t i = 0; i < SLOTS; i++) {
        memset(&hc_slots[i], 0, sizeof(hc_slots[i]));
        hc_slots[i].number = i;
        hc_slots[i].utrd = utrd_mem[i];
        hc_slots[i].ucd = (volatile uint32_t *)ucd_mem[i];
    }
    hc_free = 0xfffffffeu;
    hc_active_slots = 0;
    hc_pending_slots = 0;
    fake_utrldbr = 0;
    hc_in_flight = 0;
}

static void set_response(uint32_t slot, uint32_t ocs, uint8_t op, uint8_t tag, uint8_t resp, uint8_t status, uint8_t sk, uint8_t asc, uint8_t ascq) {
    utrd_mem[slot][2] = ocs;
    volatile uint8_t *r = (volatile uint8_t *)ucd_mem[slot] + UCD_RESPONSE;
    memset((void *)r, 0, 64);
    r[0] = op;
    r[3] = tag;
    r[6] = resp;
    r[7] = status;
    r[34 + 2] = sk;
    r[34 + 12] = asc;
    r[34 + 13] = ascq;
}

@HC_ANSWER_OK@

@UFSHC_STRATEGY_ASYNC@

@LOG_CAP_DECL@

@PROCESS_COMPLETIONS@

static void submit_and_complete_verify_fua(struct buf *bp, uint8_t lun, uint64_t off, bool write, bool expect_fua,
    uint32_t ocs, uint8_t op, uint8_t tag_delta, uint8_t resp, uint8_t status, uint8_t sk, uint8_t asc, uint8_t ascq) {
    assert(ufshc_strategy_async(bp, lun, off, write) == 0);
    uint32_t active = hc_active_slots;
    assert(active != 0);
    uint32_t slot = (uint32_t)__builtin_ctz(active);
    assert(hc_slots[slot].lun == lun);
    assert(hc_slots[slot].lun_offset == off);
    if (write) {
        volatile const uint8_t *cmd = (volatile const uint8_t *)ucd_mem[slot];
        assert(cmd[16] == 0x2a);
        assert((cmd[17] & 0x08) == (expect_fua ? 0x08 : 0x00));
    }
    set_response(slot, ocs, op, (uint8_t)(slot + tag_delta), resp, status, sk, asc, ascq);
    fake_utrldbr &= ~(1u << slot);
    hc_pending_slots |= (1u << slot);
    uint32_t before_comp = dcache_flushes;
    ufshc_process_completions();
    (void)before_comp;
    assert(bp->biodone == 1);
    assert(hc_in_flight == 0);
}

static void submit_and_complete(struct buf *bp, uint8_t lun, uint64_t off, bool write,
    uint32_t ocs, uint8_t op, uint8_t tag_delta, uint8_t resp, uint8_t status, uint8_t sk, uint8_t asc, uint8_t ascq) {
    submit_and_complete_verify_fua(bp, lun, off, write, false, ocs, op, tag_delta, resp, status, sk, asc, ascq);
}

int main(void) {
    init_slots();

    /* 1. Healthy async read and write (including B_FUA flag -> WRITE(10) FUA bit). */
    hc_dma_coherent = false;
    struct buf r_ok = { 16384, 999, -1, 0, 0 };
    submit_and_complete(&r_ok, 0, 4096ULL, false, OCS_SUCCESS, UPIU_RESPONSE, 0, 0, 0, 0, 0, 0);
    assert(r_ok.error == -1);
    assert(r_ok.resid == 0);
    assert(hc_requests == 1 && hc_bytes == 16384);
    assert(hc_errors == 0 && hc_write_errors == 0);
    assert(dcache_flushes == 2); /* 1 at submit + 1 at completion */
    assert(log_count == 0);

    struct buf w_ok = { 8192, 999, -1, 0, 0 };
    submit_and_complete_verify_fua(&w_ok, 0, 65536ULL, true, false, OCS_SUCCESS, UPIU_RESPONSE, 0, 0, 0, 0, 0, 0);
    assert(w_ok.error == -1);
    assert(w_ok.resid == 0);
    assert(hc_write_requests == 1 && hc_write_bytes == 8192);
    assert(dcache_flushes == 3); /* write submit flush only */
    assert(log_count == 0);

    struct buf w_fua = { 4096, 999, -1, 0, B_FUA };
    submit_and_complete_verify_fua(&w_fua, 0, 69632ULL, true, true, OCS_SUCCESS, UPIU_RESPONSE, 0, 0, 0, 0, 0, 0);
    assert(w_fua.error == -1 && w_fua.resid == 0);
    assert(hc_write_requests == 2 && hc_write_bytes == 8192 + 4096);
    assert(dcache_flushes == 4);

    /* 2. OCS failure on async write prints LUN, byte range, slot, type, OCS. */
    struct buf w_ocs = { 8192, 0, 0, 0, 0 };
    submit_and_complete(&w_ocs, 2, 131072ULL, true, 0x2, UPIU_RESPONSE, 0, 0, 0, 0, 0, 0);
    assert(w_ocs.error == EIO && w_ocs.resid == 8192);
    assert(hc_write_errors == 1 && hc_errors == 0);
    assert(dcache_flushes == 5);
    assert(log_count == 1);
    assert(strstr(log_lines[0], "async write") != NULL);
    assert(strstr(log_lines[0], "LUN 2") != NULL);
    assert(strstr(log_lines[0], "bytes 131072..139263") != NULL);
    assert(strstr(log_lines[0], "OCS 2") != NULL);

    /* 3. OCS_SUCCESS with SCSI CHECK CONDITION / UPIU error / wrong tag / wrong opcode must fail (not silent success). */
    struct buf r_sense = { 4096, 0, 0, 0, 0 };
    submit_and_complete(&r_sense, 1, 8192ULL, false, OCS_SUCCESS, UPIU_RESPONSE, 0, 0, 0x02, 0x3, 0x11, 0x04);
    assert(r_sense.error == EIO && r_sense.resid == 4096);
    assert(hc_errors == 1 && hc_requests == 1);
    assert(dcache_flushes == 6); /* submit flush only; completion flush skipped on failed read */
    assert(log_count == 2);
    assert(strstr(log_lines[1], "async read") != NULL);
    assert(strstr(log_lines[1], "LUN 1") != NULL);
    assert(strstr(log_lines[1], "bytes 8192..12287") != NULL);
    assert(strstr(log_lines[1], "status 02") != NULL);
    assert(strstr(log_lines[1], "sense key 3 asc 11 ascq 04") != NULL);

    struct buf r_resp = { 4096, 0, 0, 0, 0 };
    submit_and_complete(&r_resp, 0, 0ULL, false, OCS_SUCCESS, UPIU_RESPONSE, 0, 0x01, 0, 0, 0, 0);
    assert(r_resp.error == EIO && r_resp.resid == 4096 && hc_errors == 2);

    struct buf w_tag = { 4096, 0, 0, 0, 0 };
    submit_and_complete(&w_tag, 0, 4096ULL, true, OCS_SUCCESS, UPIU_RESPONSE, 1, 0, 0, 0, 0, 0);
    assert(w_tag.error == EIO && w_tag.resid == 4096 && hc_write_errors == 2);

    struct buf r_op = { 4096, 0, 0, 0, 0 };
    submit_and_complete(&r_op, 0, 12288ULL, false, OCS_SUCCESS, 0x00, 0, 0, 0, 0, 0, 0);
    assert(r_op.error == EIO && r_op.resid == 4096 && hc_errors == 3);

    /* 4. Bounded error logging: trigger > HC_ASYNC_ERR_LOG_CAP errors, counters track all, logs stay bounded. */
    uint64_t expected_read_errs = hc_errors;
    for (uint32_t k = 0; k < HC_ASYNC_ERR_LOG_CAP + 10; k++) {
        struct buf burst = { 4096, 0, 0, 0, 0 };
        submit_and_complete(&burst, 0, (uint64_t)k * 4096ULL, false, OCS_SUCCESS, UPIU_RESPONSE, 0, 0, 0x02, 0x4, 0x44, 0x00);
        assert(burst.error == EIO && burst.resid == 4096);
        expected_read_errs++;
    }
    assert(hc_errors == expected_read_errs);
    assert(log_count == HC_ASYNC_ERR_LOG_CAP + 1);
    assert(strstr(log_lines[HC_ASYNC_ERR_LOG_CAP], "suppressed") != NULL);
    assert(hc_async_completions == 3 + 5 + HC_ASYNC_ERR_LOG_CAP + 10);
    return 0;
}
"""

ufshc_c = (DRIVER / "annibale_ufshc.c").read_text()
rw_inc = (DRIVER / "annibale_ufshc_rw.inc").read_text()
intr_inc = (DRIVER / "annibale_ufshc_intr.inc").read_text()

cap_start = intr_inc.index("#define HC_ASYNC_ERR_LOG_CAP")
cap_end = intr_inc.index(";", cap_start) + 1

source = (HARNESS
    .replace("@HC_SLOT_STRUCT@", extract_block(ufshc_c, "struct hc_slot {", "\n};"))
    .replace("@HC_ANSWER_OK@", extract_block(rw_inc, "static bool\nhc_answer_ok(", "\n}"))
    .replace("@UFSHC_STRATEGY_ASYNC@", extract_block(rw_inc, "int\nufshc_strategy_async(", "\n}"))
    .replace("@LOG_CAP_DECL@", intr_inc[cap_start:cap_end])
    .replace("@PROCESS_COMPLETIONS@", extract_block(intr_inc, "static void\nufshc_process_completions(", "\n}")))

with tempfile.TemporaryDirectory() as tmp:
    cfile = Path(tmp) / "test.c"
    cfile.write_text(source)
    prog = Path(tmp) / "test"
    subprocess.run(
        ["cc", "-std=c11", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(prog)],
        check=True,
    )
    subprocess.run([str(prog)], check=True)
print("UFS async error validation & bounded logging: all assertions passed (ASan+UBSan)")

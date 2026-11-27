#!/usr/bin/env python3
"""Host ASan+UBSan tests for G30 Phase 1 dual-context preemption, role routing, and SMMU FSR W1C."""

from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEV = ROOT / "xnu/files/bsd/dev/adreno8"
ANN = ROOT / "xnu/files/bsd/dev/annibale"
IOKIT = ROOT / "xnu/files/iokit/Kernel"

HARNESS_HEAD = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;

#define _DEV_ADRENO8_PRIVATE_H
#include "adreno8_wire_private.h"
#include "adreno8_bo_private.h"
#include "adreno8_memory_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_context_private.h"

struct adreno8_block {
    const char *name;
    uint32_t phys, size;
    vm_offset_t base;
};
struct adreno8_gpu_block {
    const char *name;
    uint32_t address, size;
    vm_offset_t memory;
};
struct adreno8_space_table {
    vm_offset_t table;
    uint64_t phys;
    uint32_t index;
    unsigned level;
};
struct adreno8_gpu_space {
    const char *name;
    uint32_t context;
    struct adreno8_space_table tables[4];
    unsigned table_count;
    uint16_t leaf_slot[2048];
    unsigned secure_tables;
    bool is_on;
};

#define GMU_HOST2GMU_SET 0
#define SMMU_CB_FSR      0x58
#define FSR_ALL          0xc00001fe
#define NSEC_PER_USEC    1000ull
#define NSEC_PER_MSEC    1000000ull
#define KERN_SUCCESS     0

static struct adreno8_block adreno8_gmu;
static struct adreno8_block adreno8_smmu = { .name = "smmu", .phys = 0x3da0000, .size = 0x40000 };
static uint32_t adreno8_smmu_page = 0x1000, adreno8_smmu_pages = 32;
struct adreno8_gpu_space adreno8_gpu_space;
static bool adreno8_gpu8_keep = true, adreno8_gpu8_ifpc, adreno8_gpu8_preempt, adreno8_gpu8_return;
static bool adreno8_reset_required;
static uint32_t adreno8_inflight = 4;
static uint32_t adreno8_gx_hz[17] = {0, 160000000, 222000000, 342000000};
static unsigned adreno8_gx_count = 4;
static uint64_t clock_ns;
static char g_proc_name[32] = "testapp";
static bool g_annibale = true, g_bo_ready = true, g_gpu_ready = true, g_idle_ok = true, g_faulted;
static uint32_t g_gmu_regs[16];
static uint32_t g_smmu_regs[0x40000 / 4];
static uint32_t g_sticky_fsr_mask;
static unsigned g_smmu_writes;

static uint32_t g_hfi_ids[32];
static uint32_t g_hfi_payloads[32][16];
static uint32_t g_hfi_counts[32];
static unsigned g_hfi_sent;
static unsigned g_doorbells[32];
static unsigned g_doorbell_count;

static int adreno8_say(const char *fmt, ...) { (void)fmt; return 0; }
#define printf adreno8_say
static void adreno8_settle(void) {}
static uint64_t mach_absolute_time(void) { return clock_ns; }
static void absolutetime_to_nanoseconds(uint64_t v, uint64_t *out) { *out = v; }
static void adreno8_sleep_us(uint32_t us) { clock_ns += (uint64_t)us * 1000ull; }
static void delay_for_interval(uint32_t n, uint64_t unit) { (void)n; (void)unit; }
static bool PE_parse_boot_argn(const char *k, void *out, int sz) { (void)k; (void)out; (void)sz; return false; }
static bool annibale_platform_is_annibale(void) { return g_annibale; }
static void proc_selfname(char *buf, int size) { snprintf(buf, (size_t)size, "%s", g_proc_name); }
static int proc_selfpid(void) { return strcmp(g_proc_name, "backboardd") == 0 ? 10 : 42; }
static void adreno8_to_memory(vm_offset_t s, uint64_t n) { (void)s; (void)n; }

static uint32_t adreno8_rd(vm_offset_t addr) {
    if (addr >= adreno8_smmu.base && addr < adreno8_smmu.base + sizeof(g_smmu_regs))
        return g_smmu_regs[(addr - adreno8_smmu.base) / 4];
    if (addr >= adreno8_gmu.base && addr < adreno8_gmu.base + sizeof(g_gmu_regs))
        return g_gmu_regs[(addr - adreno8_gmu.base) / 4];
    return *(const uint32_t *)addr;
}

static void auto_retire_context(uint32_t doorbell);

static void adreno8_wr(vm_offset_t addr, uint32_t val) {
    if (addr >= adreno8_smmu.base && addr < adreno8_smmu.base + sizeof(g_smmu_regs)) {
        uint32_t idx = (uint32_t)(addr - adreno8_smmu.base) / 4;
        if (((addr - adreno8_smmu.base) & 0xfff) == SMMU_CB_FSR) {
            g_smmu_regs[idx] &= ~(val & ~g_sticky_fsr_mask);
            g_smmu_writes++;
        } else {
            g_smmu_regs[idx] = val;
        }
        return;
    }
    if (addr == adreno8_gmu.base + GMU_HOST2GMU_SET) {
        g_gmu_regs[0] = val;
        assert(g_doorbell_count < 32);
        g_doorbells[g_doorbell_count++] = val;
        auto_retire_context(val);
        return;
    }
    *(uint32_t *)addr = val;
}

static void adreno8_put(struct adreno8_block *b, const char *name, uint32_t offset, uint32_t value) {
    (void)name;
    adreno8_wr(b->base + offset, value);
}

static uint64_t kvtophys(vm_offset_t va) { return (uint64_t)va; }
static vm_offset_t ml_io_map_wcomb(uint64_t pa, size_t sz) { (void)sz; return (vm_offset_t)pa; }
bool adreno8_gpu_commands_ready(void) { return g_gpu_ready; }
bool adreno8_hwsched_ready(void) { return g_gpu_ready; }
bool adreno8_gpu_memory_ready(void) { return adreno8_gpu_space.is_on; }
bool adreno8_gpu_faulted(void) { return g_faulted; }
void adreno8_hwsched_memory_failed(void) { g_gpu_ready = false; }
bool adreno8_gpu_wait_idle(const char *what) { (void)what; return g_idle_ok; }
void adreno8_gpu_hang_dump(void) {}
void adreno8_gpu_oob_release(void) {}
static uint32_t adreno8_pm4_scratch(uint32_t *ib, uint32_t max_w, uint32_t addr, uint32_t val) {
    (void)max_w; (void)addr;
    ib[0] = 0x70000001u;
    ib[1] = val;
    return 6;
}

static struct adreno8_memory g_mem[16];
static unsigned g_mem_count;
static bool g_auto_retire = true;

static struct adreno8_memory *alloc_mem(const char *name, uint32_t size, uint32_t align, bool gpu, bool gmu) {
    assert(g_mem_count < 16 && (align & (align - 1)) == 0);
    unsigned idx = g_mem_count++;
    struct adreno8_memory *m = &g_mem[idx];
    m->name = name;
    m->size = size;
    m->memory = (vm_offset_t)calloc(1, size);
    assert(m->memory);
    m->gpu_address = gpu ? 0x10000000u + idx * 0x100000u : 0;
    m->gmu_address = gmu ? 0x60000000u + idx * 0x100000u : 0;
    return m;
}
struct adreno8_memory *adreno8_gmu_alloc(const char *n, uint32_t s, uint32_t a, uint32_t p) {
    (void)p; return alloc_mem(n, s, a, false, true);
}
struct adreno8_memory *adreno8_gpu_alloc(const char *n, uint32_t s, uint32_t a, uint32_t p) {
    (void)p; return alloc_mem(n, s, a, true, false);
}
bool adreno8_memory_publish(const struct adreno8_memory *m, uint32_t off, uint32_t sz) {
    (void)m; (void)off; (void)sz; return true;
}
bool adreno8_memory_acquire(const struct adreno8_memory *m, uint32_t off, uint32_t sz) {
    (void)m; (void)off; (void)sz; return true;
}
bool adreno8_hfi_poll(void) { return true; }
bool adreno8_hfi_send(uint32_t id, const uint32_t *payload, uint32_t count, const char *what) {
    (void)what;
    assert(g_hfi_sent < 32 && count <= 16);
    g_hfi_ids[g_hfi_sent] = id;
    g_hfi_counts[g_hfi_sent] = count;
    if (count) memcpy(g_hfi_payloads[g_hfi_sent], payload, count * sizeof(uint32_t));
    g_hfi_sent++;
    return true;
}

bool adreno8_bo_enable(void) { return g_bo_ready; }
bool adreno8_bo_ready(void) { return g_bo_ready; }
bool adreno8_bo_enabled(void) { return g_bo_ready; }
void adreno8_bo_stop(void) { g_bo_ready = false; }
uint64_t adreno8_bo_reserve(uint64_t s, uint32_t *r) { (void)s; (void)r; return 0; }
bool adreno8_bo_release(uint64_t a, uint64_t s) { (void)a; (void)s; return true; }
bool adreno8_bo_map(uint64_t a, uint64_t p, bool ro) { (void)a; (void)p; (void)ro; return true; }
bool adreno8_bo_commit(void) { return true; }
bool adreno8_bo_unmap(uint64_t a, uint64_t s) { (void)a; (void)s; return true; }
static void adreno8_speed_tick(void) {}
static void adreno8_speed_state(uint32_t *m, uint32_t *c) { *m = 342; *c = 1; }
void adreno8_hfi_worked(uint64_t *u, uint32_t *s, uint32_t by[7]) { *u = 0; *s = 0; memset(by, 0, 28); }
void adreno8_hfi_irq_attach(void) {}
uint32_t adreno8_hfi_irq_count(void) { return 0; }
void adreno8_hfi_irq_pause(uint32_t seen, uint32_t pu, uint32_t lu) { (void)seen; (void)pu; (void)lu; }
static bool AnnibaleGPUStart(void) { return true; }
"""

HARNESS_TAIL = r"""
#undef printf
static void auto_retire_context(uint32_t doorbell) {
    if (!g_auto_retire) return;
    for (uint32_t i = 0; i < context_slot_count(); i++) {
        struct adreno8_context_slot *s = &context_slots[i];
        if (s->doorbell_bit != doorbell || !s->queue_view || !s->timestamp_view) continue;
        uint32_t *q = (uint32_t *)s->queue_view;
        uint32_t read = q[5], write = q[6];
        while (read != write) {
            uint32_t ts = q[CONTEXT_HEADER_WORDS + read + 3];
            uint32_t len = (q[CONTEXT_HEADER_WORDS + read] >> 8) & 0xff;
            uint32_t aligned = (len + 3) & ~3u;
            ((uint32_t *)s->timestamp_view)[0] = ts;
            ((uint32_t *)s->timestamp_view)[2] = ts;
            if (test_ib && test_result && test_ib->memory && test_result->memory) {
                uint32_t *ib = (uint32_t *)test_ib->memory;
                ((uint32_t *)test_result->memory)[0] = ib[1];
            }
            read = (read + aligned) % CONTEXT_QUEUE_WORDS;
        }
        q[5] = read;
    }
}

static void reset_all(bool preempt) {
    for (unsigned i = 0; i < g_mem_count; i++) {
        free((void *)g_mem[i].memory);
    }
    memset(g_mem, 0, sizeof(g_mem));
    g_mem_count = 0;
    memset(&context_slots, 0, sizeof(context_slots));
    context_slots[0] = (struct adreno8_context_slot){ .id = 1u, .flags = CONTEXT_FLAGS_RB0, .doorbell_bit = CONTEXT_DOORBELL_RB0, .needs_user_record = false };
    context_slots[1] = (struct adreno8_context_slot){ .id = 2u, .flags = CONTEXT_FLAGS_RB1, .doorbell_bit = CONTEXT_DOORBELL_RB1, .needs_user_record = true };
    test_ib = test_result = NULL;
    context_tested = context_failed = false;
    adreno8_gpu8_preempt = preempt;
    adreno8_gmu.base = (vm_offset_t)g_gmu_regs;
    adreno8_smmu.base = (vm_offset_t)g_smmu_regs;
    memset(g_gmu_regs, 0, sizeof(g_gmu_regs));
    memset(g_smmu_regs, 0, sizeof(g_smmu_regs));
    g_sticky_fsr_mask = 0;
    g_smmu_writes = 0;
    adreno8_gpu_space = (struct adreno8_gpu_space){ .name = "gpu", .context = 0, .table_count = 1, .is_on = true };
    adreno8_gpu_space.tables[0].phys = 0x300000000ULL;
    g_hfi_sent = 0;
    g_doorbell_count = 0;
    g_gpu_ready = g_bo_ready = g_idle_ok = true;
    g_faulted = false;
    g_auto_retire = true;
    adreno8_inflight = 4;
}

static uint32_t compute_depth_target(uint32_t depth, uint32_t last) {
    uint32_t target = 0;
    uint32_t raw = last & 0x7fffffffu;
    if (raw >= depth) target = (last & 0x80000000u) | (raw - depth + 1);
    return target;
}

int main(void) {
    /* 1. Default preempt=0 preserves single RB0 context (no slot 1, no user record, no bit31 tags). */
    reset_all(false);
    assert(adreno8_context_test());
    assert(g_hfi_sent == 3); /* HFI 128, 134, 129 for ctx 1 only */
    assert(g_hfi_ids[0] == 128 && g_hfi_payloads[0][0] == 1 && g_hfi_payloads[0][1] == CONTEXT_FLAGS_RB0);
    assert(g_hfi_ids[1] == 134 && g_hfi_payloads[1][5] == 0 && g_hfi_payloads[1][6] == 0);
    assert(g_hfi_ids[2] == 129 && g_hfi_payloads[2][0] == 1);
    assert(g_doorbell_count == 2 && g_doorbells[0] == (1u << 2) && g_doorbells[1] == (1u << 2));
    uint64_t val = 0;
    assert(annibale_gpu_parameter(7, &val) && val == 1);

    /* 2. preempt=1 enables dual RB0/RB1 registration, user_ctxt_record on slot 1, independent doorbells. */
    reset_all(true);
    assert(adreno8_context_test());
    assert(context_slots[1].user_record && context_slots[1].user_record->size == CONTEXT_USER_RECORD_BYTES);
    /* Expect: ctx1 reg(128)+ptr(134), ctx2 reg(128)+ptr(134), close ctx1(129)+ctx2(129) = 6 */
    assert(g_hfi_sent == 6);
    assert(g_hfi_ids[0] == 128 && g_hfi_payloads[0][0] == 1 && g_hfi_payloads[0][1] == CONTEXT_FLAGS_RB0);
    assert(g_hfi_ids[1] == 134 && g_hfi_payloads[1][5] == 0 && g_hfi_payloads[1][6] == 0);
    assert(g_hfi_ids[2] == 128 && g_hfi_payloads[2][0] == 2 && g_hfi_payloads[2][1] == CONTEXT_FLAGS_RB1);
    assert(g_hfi_ids[3] == 134 && g_hfi_payloads[3][0] == 2 &&
           g_hfi_payloads[3][5] == context_slots[1].user_record->gpu_address && g_hfi_payloads[3][6] == 0);
    assert(g_hfi_ids[4] == 129 && g_hfi_payloads[4][0] == 1);
    assert(g_hfi_ids[5] == 129 && g_hfi_payloads[5][0] == 2);
    assert(g_doorbell_count == 3);
    assert(g_doorbells[0] == (1u << 2) && g_doorbells[1] == (1u << 2) && g_doorbells[2] == (1u << 3));
    assert(annibale_gpu_parameter(7, &val) && val == 2);

    /* 3. Role routing in AnnibaleGPU: backboardd -> slot 0 (bit31=0), other apps -> slot 1 (bit31=1). */
    reset_all(true);
    assert(adreno8_context_test());
    uint64_t ib_addr = 0x10002000ull;
    uint32_t ib_words = 8;
    snprintf(g_proc_name, sizeof(g_proc_name), "MobileSafari");
    uint32_t app_f1 = annibale_gpu_submit(&ib_addr, &ib_words, 1);
    uint32_t app_f2 = annibale_gpu_submit(&ib_addr, &ib_words, 1);
    assert((app_f1 & CONTEXT_FENCE_SLOT_BIT) != 0 && (app_f2 & CONTEXT_FENCE_SLOT_BIT) != 0);
    assert(annibale_gpu_last() == app_f2);
    assert(annibale_gpu_done(app_f1) && annibale_gpu_done(app_f2));

    /* Check depth target stays on slot 1 even when bit31 is set. */
    uint32_t app_f3 = annibale_gpu_submit(&ib_addr, &ib_words, 1);
    uint32_t app_f4 = annibale_gpu_submit(&ib_addr, &ib_words, 1);
    uint32_t app_f5 = annibale_gpu_submit(&ib_addr, &ib_words, 1);
    assert(compute_depth_target(4, app_f5) == app_f2);
    (void)app_f3; (void)app_f4;

    /* backboardd routes to slot 0 independently and isn't throttled by slot 1's inflight fence count. */
    g_auto_retire = false;
    snprintf(g_proc_name, sizeof(g_proc_name), "MobileSafari");
    uint32_t app_f6 = annibale_gpu_submit(&ib_addr, &ib_words, 1); /* slot 1 ts=6 */
    snprintf(g_proc_name, sizeof(g_proc_name), "backboardd");
    uint32_t bb_f = annibale_gpu_submit(&ib_addr, &ib_words, 1);   /* slot 0 ts=3 */
    assert((bb_f & CONTEXT_FENCE_SLOT_BIT) == 0);
    assert(annibale_gpu_last() == bb_f);
    assert(!annibale_gpu_done(bb_f));
    /* Advancing slot 1 EOP to 6 (>= slot 0 ts=3) must NOT mark backboardd's slot 0 fence done. */
    ((uint32_t *)context_slots[1].timestamp_view)[2] = app_f6 & CONTEXT_FENCE_TS_MASK;
    ((uint32_t *)context_slots[1].queue_view)[5] = ((uint32_t *)context_slots[1].queue_view)[6];
    assert(annibale_gpu_done(app_f6) && !annibale_gpu_done(bb_f));
    assert(annibale_gpu_retired_for(app_f6) == app_f6);
    assert(annibale_gpu_retired_for(bb_f) == 2u);
    snprintf(g_proc_name, sizeof(g_proc_name), "MobileSafari");
    assert(((annibale_gpu_last() - annibale_gpu_retired_for(app_f5)) & 0x7fffffffu) == 0u);
    ((uint32_t *)context_slots[1].timestamp_view)[2] = 0u; /* slot 1 EOP=0 while last=7 -> inflight=7, not 0x80000007 */
    assert(annibale_gpu_retired_for(app_f6) == 0u);
    assert(((annibale_gpu_last() - annibale_gpu_retired_for(app_f5)) & 0x7fffffffu) == (app_f6 & CONTEXT_FENCE_TS_MASK));
    ((uint32_t *)context_slots[1].timestamp_view)[2] = app_f6 & CONTEXT_FENCE_TS_MASK;
    snprintf(g_proc_name, sizeof(g_proc_name), "backboardd");
    assert(!adreno8_context_drained("slot 0 still running", 200));
    /* Advancing slot 0 EOP retires bb_f and completes drained/wait_idle. */
    ((uint32_t *)context_slots[0].timestamp_view)[2] = bb_f & CONTEXT_FENCE_TS_MASK;
    ((uint32_t *)context_slots[0].queue_view)[5] = ((uint32_t *)context_slots[0].queue_view)[6];
    assert(annibale_gpu_done(bb_f));
    assert(adreno8_context_drained("both idle", 200));
    assert(adreno8_context_wait_idle("preempt wait idle"));

    /* 4. 31-bit wraparound on slot 1 keeps bit 31 set and preserves modular comparison. */
    context_slots[1].last = CONTEXT_FENCE_TS_MASK;
    ((uint32_t *)context_slots[1].timestamp_view)[2] = CONTEXT_FENCE_TS_MASK;
    ((uint32_t *)context_slots[1].queue_view)[5] = ((uint32_t *)context_slots[1].queue_view)[6];
    g_auto_retire = true;
    snprintf(g_proc_name, sizeof(g_proc_name), "Camera");
    uint32_t wrapped = annibale_gpu_submit(&ib_addr, &ib_words, 1);
    assert(wrapped == (CONTEXT_FENCE_SLOT_BIT | 1u));
    assert(annibale_gpu_done(wrapped));

    /* 5. adreno8_space_clear_fault W1C clears FSR_ALL bits, ignores non-fault 0x400, detects sticky bits. */
    uint32_t cb0_fsr_idx = (adreno8_smmu_pages * adreno8_smmu_page + SMMU_CB_FSR) / 4;
    g_smmu_regs[cb0_fsr_idx] = 0x400u; /* format bit only */
    g_smmu_writes = 0;
    assert(adreno8_space_clear_fault(&adreno8_gpu_space) && g_smmu_writes == 0 && g_smmu_regs[cb0_fsr_idx] == 0x400u);
    g_smmu_regs[cb0_fsr_idx] = 0x408u; /* translation fault + format bit */
    assert(adreno8_space_clear_fault(&adreno8_gpu_space) && g_smmu_writes == 1 && g_smmu_regs[cb0_fsr_idx] == 0x400u);
    g_sticky_fsr_mask = 0x2u;
    g_smmu_regs[cb0_fsr_idx] = 0x40au;
    assert(!adreno8_space_clear_fault(&adreno8_gpu_space) && g_smmu_regs[cb0_fsr_idx] == 0x402u);
    assert(!adreno8_space_clear_fault(NULL));

    printf("test-adreno8-preempt: ALL PASS\n");
    return 0;
}
"""


def main() -> int:
    ctx_src = (DEV / "adreno8_context.c").read_text()
    ctx_src = ctx_src[ctx_src.index("#define printf adreno8_say"):]
    disp_src = (ANN / "annibale_gpu_dispatch.c").read_text()
    disp_src = disp_src[disp_src.index("uint32_t gpu_va;"):]
    smmu_src = (DEV / "adreno8_smmu.c").read_text()
    clear_fn = re.search(
        r"bool\nadreno8_space_clear_fault\(struct adreno8_gpu_space \*space\)\n\{.*?\n\}",
        smmu_src, re.S,
    )
    assert clear_fn, "adreno8_space_clear_fault not found in adreno8_smmu.c"
    full_c = HARNESS_HEAD + "\n" + ctx_src + "\n" + disp_src + "\n" + clear_fn.group(0) + "\n" + HARNESS_TAIL

    with tempfile.TemporaryDirectory(prefix="adreno8-preempt-") as td:
        tdir = Path(td)
        (tdir / "sys").mkdir()
        (tdir / "adreno8_private.h").write_text("")
        (tdir / "sys/annibale_input.h").write_text("")
        for hdr in ("adreno8_wire_private.h", "adreno8_memory_private.h", "adreno8_hwsched_private.h",
                    "adreno8_context_private.h", "adreno8_bo_private.h", "adreno8_gpu_private.h"):
            (tdir / hdr).write_text((DEV / hdr).read_text())
        cfile = tdir / "check.c"
        binfile = tdir / "check"
        cfile.write_text(full_c)
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            f"-I{tdir}", str(cfile), "-o", str(binfile),
        ], check=True)
        res = subprocess.run([str(binfile)], capture_output=True, text=True)
        print(res.stdout, end="")
        if res.returncode != 0:
            print(res.stderr, end="")
            return res.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

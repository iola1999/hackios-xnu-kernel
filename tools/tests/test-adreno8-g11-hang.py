#!/usr/bin/env python3
"""Host ASan+UBSan tests for G11 GPU hang diagnostics, CP progress sampling, and G30 recovery reset."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEV = ROOT / "xnu/files/bsd/dev/adreno8"

SYS_STUBS = {
    "kern/clock.h": "",
    "kern/sched_prim.h": "typedef void *event_t; enum { THREAD_UNINT = 0, THREAD_AWAKENED = 0 };\n"
    "#define THREAD_CONTINUE_NULL 0\n"
    "static inline void thread_wakeup(event_t e) { (void)e; }\n"
    "static inline void assert_wait_timeout(event_t e, int i, uint32_t m, uint64_t u) { (void)e;(void)i;(void)m;(void)u; }\n"
    "static inline void clear_wait(void *t, int r) { (void)t;(void)r; }\n"
    "static inline void *current_thread(void) { return 0; }\n"
    "static inline void thread_block(int c) { (void)c; }\n",
    "libkern/libkern.h": "",
    "mach/mach_types.h": "#include <stdbool.h>\n#include <stdint.h>\n#include <stddef.h>\n"
    "typedef int boolean_t;\ntypedef uintptr_t vm_offset_t;\ntypedef size_t vm_size_t;\ntypedef int kern_return_t;\n"
    "#define KERN_SUCCESS 0\n",
    "mach/vm_param.h": "#ifndef PAGE_SIZE\n#define PAGE_SIZE 16384u\n#endif\n#define PAGE_MASK (PAGE_SIZE - 1u)\n",
    "pexpert/pexpert.h": "",
    "sys/stat.h": "",
    "sys/systm.h": "",
    "sys/proc.h": "static inline int proc_selfpid(void) { return 77; }\n"
    "static inline void proc_selfname(char *buf, int sz) { snprintf(buf, (size_t)sz, \"backboardd\"); }\n",
    "sys/annibale_input.h": "bool annibale_platform_is_annibale(void);\n",
    "vm/vm_kern.h": "#define KMA_KOBJECT 1\n#define KMA_ZERO 2\n#define KMA_DATA 4\n#define VM_KERN_MEMORY_IOKIT 0\n",
}

HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adreno8_private.h"
#include "adreno8_wire_private.h"
#include "adreno8_memory_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_context_private.h"
#include "adreno8_bo_private.h"
#include "adreno8_gpu_private.h"
#include <sys/proc.h>
#define A8_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define printf adreno8_say

static char log_buf[32768];
static size_t log_len;

static void reset_log(void) { log_len = 0; log_buf[0] = '\0'; }

int adreno8_say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (log_len < sizeof(log_buf)) {
        int n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
        if (n > 0) {
            size_t add = (size_t)n;
            log_len += (add < sizeof(log_buf) - log_len) ? add : (sizeof(log_buf) - log_len - 1);
        }
    }
    va_end(ap);
    return 0;
}

void loader_log_line(const char *line) { (void)line; }
void adreno8_settle(void) {}
void adreno8_sleep_us(uint32_t us) { (void)us; }
void adreno8_to_memory(vm_offset_t a, vm_size_t s) { (void)a; (void)s; }
static bool PE_parse_boot_argn(const char *k, void *v, unsigned s) { (void)k; (void)v; (void)s; return false; }

static uint32_t g_gpu_regs[8][0x10000];
static uint32_t g_gmu_regs[0x20000];
static uint32_t g_hfi_table[512];
static uint32_t g_pipe;
static bool gx_powered = true, oob_ack_enabled = true;
static unsigned gx_accesses, oob_set_writes, oob_clr_writes;
static unsigned gmu_fault_checks, gpu_fault_checks;

struct adreno8_block adreno8_gpu = { "gpu", 0, sizeof(g_gpu_regs[0]), (vm_offset_t)g_gpu_regs[0] };
struct adreno8_block adreno8_gmu = { "gmu", 0, sizeof(g_gmu_regs), (vm_offset_t)g_gmu_regs };
struct adreno8_block adreno8_smmu = { "smmu", 0, 0x1000, 0 };
struct adreno8_gpu_space adreno8_gpu_space = { .name = "gpu", .context = 0, .is_on = true };
struct adreno8_gpu_space adreno8_gmu_space = { .name = "gmu", .context = 2, .is_on = true };

bool adreno8_direct_ring = false;
bool adreno8_gpu8_keep = true;
bool adreno8_gpu8_ifpc = true;
bool adreno8_gpu8_preempt = true;
bool adreno8_gpu8_return = false;
bool adreno8_reset_required = false;
bool adreno8_hfi_tables_sent = true;
uint32_t adreno8_step = 23, adreno8_newest_step = 23, adreno8_pause_ms = 0, adreno8_skip_steps = 0, adreno8_inflight = 4;
vm_offset_t adreno8_hfi_view_table = (vm_offset_t)g_hfi_table;
void adreno8_hold(bool single_access) { (void)single_access; }
void adreno8_hwsched_memory_failed(void) {}
uint64_t kvtophys(vm_offset_t va) { return (uint64_t)va; }
vm_offset_t ml_io_map_wcomb(vm_offset_t pa, vm_size_t sz) { (void)sz; return pa; }

uint32_t adreno8_rd(vm_offset_t addr) {
    if (addr >= (vm_offset_t)g_gpu_regs[0] && addr < (vm_offset_t)g_gpu_regs[0] + sizeof(g_gpu_regs[0])) {
        assert(gx_powered && "GX read while collapsed");
        gx_accesses++;
        uint32_t reg = (uint32_t)(addr - (vm_offset_t)g_gpu_regs[0]) / 4u;
        if (reg == GEN8_CP_APERTURE_CNTL_HOST) return g_pipe << 12;
        return g_gpu_regs[g_pipe][reg];
    }
    if (addr >= (vm_offset_t)g_gmu_regs && addr < (vm_offset_t)g_gmu_regs + sizeof(g_gmu_regs)) {
        uint32_t off = (uint32_t)(addr - (vm_offset_t)g_gmu_regs);
        if (off == GMU_PWR_CLK_STATUS) return gx_powered ? 0u : 3u;
        return g_gmu_regs[off / 4u];
    }
    return *(volatile uint32_t *)addr;
}

void adreno8_wr(vm_offset_t addr, uint32_t val) {
    if (addr >= (vm_offset_t)g_gpu_regs[0] && addr < (vm_offset_t)g_gpu_regs[0] + sizeof(g_gpu_regs[0])) {
        assert(gx_powered && "GX write while collapsed");
        gx_accesses++;
        uint32_t reg = (uint32_t)(addr - (vm_offset_t)g_gpu_regs[0]) / 4u;
        if (reg == GEN8_CP_APERTURE_CNTL_HOST) { g_pipe = (val >> 12) & 7u; return; }
        g_gpu_regs[g_pipe][reg] = val;
        return;
    }
    if (addr >= (vm_offset_t)g_gmu_regs && addr < (vm_offset_t)g_gmu_regs + sizeof(g_gmu_regs)) {
        uint32_t off = (uint32_t)(addr - (vm_offset_t)g_gmu_regs);
        g_gmu_regs[off / 4u] = val;
        if (off == GMU_HOST2GMU_SET && val == (1u << 30)) {
            oob_set_writes++;
            if (oob_ack_enabled) { g_gmu_regs[GMU_GMU2HOST_INFO / 4u] |= (1u << 31); gx_powered = true; }
        } else if (off == GMU_HOST2GMU_SET && val == (1u << 31)) {
            oob_clr_writes++;
            gx_powered = false;
        } else if (off == GMU_GMU2HOST_CLR) {
            g_gmu_regs[GMU_GMU2HOST_INFO / 4u] &= ~val;
        }
        return;
    }
    *(volatile uint32_t *)addr = val;
}

uint32_t adreno8_get(struct adreno8_block *b, const char *n, uint32_t o) { (void)n; return adreno8_rd(b->base + o); }
void adreno8_put(struct adreno8_block *b, const char *n, uint32_t o, uint32_t v) { (void)n; adreno8_wr(b->base + o, v); }
bool adreno8_ordinary_ready(void) { return false; }
bool adreno8_hwsched_ready(void) { return true; }
bool adreno8_gpu_memory_ready(void) { return true; }
bool adreno8_gpu_faulted(void) { return false; }
bool adreno8_hfi_poll(void) { return true; }
bool adreno8_hfi_send(uint32_t id, const uint32_t *p, uint32_t n, const char *w) { (void)id;(void)p;(void)n;(void)w; return true; }
bool adreno8_hwsched_raw(const uint32_t *p, uint32_t n, const char *w) { (void)p;(void)n;(void)w; return true; }
void adreno8_announce(const char *w, ...) { (void)w; }
bool adreno8_space_faulted(struct adreno8_gpu_space *sp) {
    if (sp == &adreno8_gmu_space) gmu_fault_checks++;
    if (sp == &adreno8_gpu_space) gpu_fault_checks++;
    return false;
}
struct adreno8_memory *adreno8_gpu_alloc(const char *n, uint32_t s, uint32_t a, uint32_t f) {
    (void)n; (void)a; (void)f;
    struct adreno8_memory *m = calloc(1, sizeof(*m));
    m->size = s; m->memory = (vm_offset_t)calloc(1, s); m->gpu_address = 0x10000000u;
    return m;
}
struct adreno8_memory *adreno8_gmu_alloc(const char *n, uint32_t s, uint32_t a, uint32_t f) {
    return adreno8_gpu_alloc(n, s, a, f);
}
bool adreno8_memory_publish(const struct adreno8_memory *m, uint32_t o, uint32_t s) { (void)m;(void)o;(void)s; return true; }
bool adreno8_memory_acquire(const struct adreno8_memory *m, uint32_t o, uint32_t s) { (void)m;(void)o;(void)s; return true; }
bool adreno8_translation_walk(struct adreno8_gpu_space *sp, uint32_t va, vm_offset_t m, uint32_t s, uint32_t f) {
    (void)sp;(void)va;(void)m;(void)s;(void)f; return true;
}
const uint8_t *annibale_image_file(const char *d, const char *n, uint64_t *s, int *k) { (void)d;(void)n;(void)s;(void)k; return NULL; }
void adreno8_bo_dump_tree(uint64_t b, uint32_t i) { (void)b;(void)i; }
void adreno8_bo_describe_owner(const char *n, uint32_t a) { (void)n;(void)a; }
void adreno8_bo_dump_words(const char *w, uint64_t a, uint32_t c) { (void)w;(void)a;(void)c; }
static uint64_t mach_absolute_time(void) { return 1000000ULL; }
static void absolutetime_to_nanoseconds(uint64_t d, uint64_t *o) { *o = d; }
'''

TESTS = r'''
int main(void) {
    /* Prepare context slot 0 + submit history for hang dump & recovery checks. */
    static uint32_t ts_mem[4096];
    ts_mem[CONTEXT_SOP / 4] = 9u;
    ts_mem[CONTEXT_EOP / 4] = 8u;
    context_slots[0].id = 1u;
    context_slots[0].state = CONTEXT_REGISTERED;
    context_slots[0].last = 10u;
    context_slots[0].timestamp_view = (vm_offset_t)ts_mem;
    submit_history[0] = (struct submit_record){
        .context_id = 1u, .timestamp = 10u, .pid = 77, .count = 2u,
        .ib0_addr = 0x10001000u, .ib0_words = 16u, .ib_last_addr = 0x10002000u
    };
    submit_history_next = 1u;

    /* Populate always-on GMU & HFI queue registers with GX collapsed. */
    g_gmu_regs[GMU_RPMH_POWER_STATE / 4u] = 0x1234u;
    g_gmu_regs[GMU_FW_INIT_RESULT / 4u] = GMU_FW_STARTED;
    g_gmu_regs[GMU_GMU2HOST_INFO / 4u] = 0x80800001u;
    g_gmu_regs[GMU_GMU2HOST_MASK / 4u] = 0xfffffffeU;
    g_gmu_regs[GMU_CX_BUSY_STATUS / 4u] = 0x00800000u;
    g_gmu_regs[GMU_AO_INTERRUPT_STATUS / 4u] = 0x20u;
    g_gmu_regs[GMU_AHB_FENCE_STATUS / 4u] = 0x4u;
    g_gmu_regs[GMU_HFI_CTRL_STATUS / 4u] = 0x1u;
    g_hfi_table[ADRENO8_HFI_HEADER(0) + 10] = 11u;
    g_hfi_table[ADRENO8_HFI_HEADER(0) + 11] = 12u;
    g_hfi_table[ADRENO8_HFI_HEADER(1) + 10] = 21u;
    g_hfi_table[ADRENO8_HFI_HEADER(1) + 11] = 22u;
    g_hfi_table[ADRENO8_HFI_HEADER(2) + 10] = 31u;
    g_hfi_table[ADRENO8_HFI_HEADER(2) + 11] = 32u;

    /* 1. GX off at hang_dump: pre-OOB GMU2HOST_INFO, GMU/HFI queues, SMMU faults & inflight all print. */
    gpu_state = GPU_TESTED;
    oob_held = false;
    gx_powered = false;
    oob_ack_enabled = false;
    reset_log();
    adreno8_gpu_hang_dump();
    assert(gx_accesses == 0);
    assert(gmu_fault_checks == 1 && gpu_fault_checks == 1);
    assert(strstr(log_buf, "adreno8: hang GMU GMU_PWR_CLK_STATUS = 0x3") != NULL);
    assert(strstr(log_buf, "adreno8: hang GMU GMU_FW_INIT_RESULT = 0x100") != NULL);
    assert(strstr(log_buf, "adreno8: hang GMU GMU_GMU2HOST_INFO = 0x80800001") != NULL);
    assert(strstr(log_buf, "adreno8: hang GMU GMU_GMU2HOST_MASK = 0xfffffffe") != NULL);
    assert(strstr(log_buf, "adreno8: hang GMU GMU_CX_BUSY_STATUS = 0x800000") != NULL);
    assert(strstr(log_buf, "adreno8: hang HFI queues: command 11/12, response 21/22, debug 31/32") != NULL);
    assert(strstr(log_buf, "adreno8: hang dump: GX is off, no CP registers") != NULL);
    assert(strstr(log_buf, "adreno8: ctx 1 inflight: SOP 9 EOP 8 last 10") != NULL);
    assert(strstr(log_buf, "adreno8: submit ctx 1 ts 10 pid 77") != NULL);

    /* 2. G30 recovery resets gpu_hang_dumped AND context_inflight_described for a 2nd hang dump. */
    adreno8_gpu_reset_for_recovery();
    adreno8_context_reset_for_recovery();
    gpu_state = GPU_TESTED;
    context_slots[0].last = 11u;
    ts_mem[CONTEXT_SOP / 4] = 11u;
    ts_mem[CONTEXT_EOP / 4] = 10u;
    reset_log();
    adreno8_gpu_hang_dump();
    assert(gmu_fault_checks == 2 && gpu_fault_checks == 2);
    assert(strstr(log_buf, "adreno8: ctx 1 inflight: SOP 11 EOP 10 last 11") != NULL);
    assert(strstr(log_buf, "adreno8: submit ctx 1 ts 10 pid 77") != NULL);

    /* 3. CP progress sampling: GX off skips OOB and returns false; GX up samples and releases temp OOB. */
    uint64_t sig0 = 0, sig1 = 0, sig2 = 0, sig3 = 0, sig4 = 0, sig5 = 0;
    oob_set_writes = oob_clr_writes = gx_accesses = 0;
    oob_ack_enabled = true;
    gx_powered = false;
    oob_held = false;
    assert(!adreno8_gpu_cp_progress(&sig0) && oob_set_writes == 0 && gx_accesses == 0);

    gx_powered = true;
    g_gpu_regs[0][GEN8_CP_RB_RPTR_BR] = 0x10u;
    g_gpu_regs[0][GEN8_CP_RB_RPTR_BV] = 0x20u;
    g_gpu_regs[A8_PIPE_BR][GEN8_CP_IB1_BASE_PIPE + 2] = 100u;
    g_gpu_regs[A8_PIPE_BR][GEN8_CP_IB2_BASE_PIPE + 2] = 40u;
    g_gpu_regs[A8_PIPE_BV][GEN8_CP_IB1_BASE_PIPE + 2] = 80u;
    g_gpu_regs[A8_PIPE_BV][GEN8_CP_IB2_BASE_PIPE + 2] = 20u;
    assert(adreno8_gpu_cp_progress(&sig1));
    assert(oob_set_writes == 1 && oob_clr_writes == 1 && !oob_held && !gx_powered && g_pipe == 0);

    gx_powered = true;
    assert(adreno8_gpu_cp_progress(&sig2) && sig2 == sig1);
    gx_powered = true;
    g_gpu_regs[A8_PIPE_BR][GEN8_CP_IB1_BASE_PIPE + 2] = 99u;
    assert(adreno8_gpu_cp_progress(&sig3) && sig3 != sig1);
    gx_powered = true;
    g_gpu_regs[A8_PIPE_BR][GEN8_CP_IB2_BASE_PIPE + 2] = 39u;
    assert(adreno8_gpu_cp_progress(&sig4) && sig4 != sig3);
    gx_powered = true;
    g_gpu_regs[0][GEN8_CP_RB_RPTR_BR] = 0x14u;
    assert(adreno8_gpu_cp_progress(&sig5) && sig5 != sig4);

    puts("G11 hang dump GX-off, pre-OOB GMU/HFI, CP progress under IFPC and G30 re-dump: PASS");
    return 0;
}
'''


def fn(text, name):
    m = re.search(rf"^(?:void|bool)\n{name}\(", text, re.M)
    assert m, f"missing {name}"
    start = m.start()
    end = text.index("\n}\n", start) + 3
    return text[start:end]


def main():
    hfi = (DEV / "adreno8_hfi.c").read_text()
    ctx = (DEV / "adreno8_context.c").read_text()
    gpu = (DEV / "adreno8_gpu.c").read_text()
    body = "\n".join([
        HARNESS,
        fn(hfi, "adreno8_hfi_dump_queues"),
        fn(hfi, "adreno8_hfi_hang_dump"),
        ctx[ctx.index("#define CONTEXT_BYTES"):],
        gpu[gpu.index("enum gpu_state"):],
        TESTS,
    ])
    with tempfile.TemporaryDirectory(prefix="adreno8-g11-") as td:
        tdir = Path(td)
        for rel, content in SYS_STUBS.items():
            p = tdir / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        for hdr in DEV.glob("*.h"):
            (tdir / hdr.name).write_text(hdr.read_text())
        src = tdir / "g11_test.c"
        exe = tdir / "g11_test"
        src.write_text(body)
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            f"-I{tdir}", str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()

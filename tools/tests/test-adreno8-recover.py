#!/usr/bin/env python3
"""Exercise G30 Phase 2 GPU recovery, secure preservation, SMMU clear and monotonic fences."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEV = ROOT / "xnu/files/bsd/dev/adreno8"
ANN = ROOT / "xnu/files/bsd/dev/annibale"

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
    "sys/proc.h": "static inline int proc_selfpid(void) { return 42; }\n"
    "static inline void proc_selfname(char *buf, int sz) { snprintf(buf, (size_t)sz, \"backboardd\"); }\n",
    "sys/annibale_input.h": "bool annibale_platform_is_annibale(void);\n",
    "vm/vm_kern.h": "#define KMA_KOBJECT 1\n#define KMA_ZERO 2\n#define KMA_DATA 4\n#define VM_KERN_MEMORY_IOKIT 0\n",
}

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "adreno8_private.h"
#include "adreno8_wire_private.h"
#include "adreno8_memory_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_context_private.h"
#include "adreno8_bo_private.h"
#include "adreno8_gpu_private.h"
#include "adreno8_zap_private.h"
#include "adreno8_secure_private.h"
#include "adreno8_smmu_private.h"
#include "adreno8_clock_private.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NSEC_PER_USEC 1000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_SEC  1000000000ull
static inline int proc_selfpid(void) { return 42; }
static inline void proc_selfname(char *buf, int sz) { snprintf(buf, (size_t)sz, "backboardd"); }
unsigned int ml_phys_read_word(vm_offset_t p) { (void)p; return 0; }
bool adreno8_gpu_coherent;
bool adreno8_secure_preflight(void) { return false; }
bool adreno8_secure_translation_3a(struct adreno8_gpu_space *s) { (void)s; return true; }
bool adreno8_secure_translation_3b(struct adreno8_gpu_space *s) { (void)s; return true; }
bool adreno8_space_prepare_page(struct adreno8_gpu_space *s, uint32_t a) { (void)s;(void)a; return true; }
bool adreno8_space_publish_range(struct adreno8_gpu_space *s, uint64_t a, uint64_t sz) { (void)s;(void)a;(void)sz; return true; }
bool adreno8_translation_walk(struct adreno8_gpu_space *s, uint32_t i, vm_offset_t m, uint32_t sz, uint32_t p) {
    (void)s;(void)i;(void)m;(void)sz;(void)p; return true;
}

static uint64_t g_clock_ns;
static uint64_t mach_absolute_time(void) { return g_clock_ns; }
static void absolutetime_to_nanoseconds(uint64_t dt, uint64_t *out) { *out = dt; }
static void delay_for_interval(uint32_t n, uint64_t u) { g_clock_ns += (uint64_t)n * u; }
static int kernel_map;
static kern_return_t kmem_alloc(int m, vm_offset_t *va, vm_size_t sz, int f, int t) {
    (void)m; (void)f; (void)t;
    void *p = aligned_alloc(PAGE_SIZE, (sz + PAGE_MASK) & ~(size_t)PAGE_MASK);
    assert(p); memset(p, 0, sz); *va = (vm_offset_t)p; return KERN_SUCCESS;
}
static void kmem_free(int m, vm_offset_t va, vm_size_t sz) { (void)m; (void)sz; free((void *)va); }
uint64_t kvtophys(vm_offset_t va) { return (uint64_t)va; }
vm_offset_t ml_io_map_wcomb(vm_offset_t pa, vm_size_t sz) { (void)sz; return pa; }
static bool PE_parse_boot_argn(const char *k, void *v, unsigned s) {
    (void)k; (void)v; (void)s; return false;
}

static uint32_t g_gmu_regs[0x68000 / 4];
static uint32_t g_smmu_regs[0x40000 / 4];
struct adreno8_block adreno8_gmu = {"gmu", 0, 0x68000, 0};
struct adreno8_block adreno8_smmu = {"smmu", 0, 0x40000, 0};
struct adreno8_gpu_space adreno8_gpu_space = {.name = "gpu", .context = 0, .is_on = true};
struct adreno8_gpu_space adreno8_gmu_space = {.name = "gmu", .context = 2, .is_on = true};
struct adreno8_gpu_space adreno8_secure_space = {.name = "secure", .context = 3, .is_on = true};

bool adreno8_gpu8_keep = true, adreno8_gpu8_ifpc, adreno8_gpu8_preempt = true, adreno8_gpu8_return;
bool adreno8_gpu_recover_enabled, adreno8_direct_ring, adreno8_reset_required;
bool adreno8_fw_loaded = true, adreno8_gmu_runs = true, adreno8_gmu_talks = true;
uint32_t adreno8_gpu8_secure_mode = SECURE_MODE_TRANSLATION_B;
uint32_t adreno8_step = 23, adreno8_newest_step = 23, adreno8_inflight = 4;
uint32_t adreno8_gx_hz[17] = {0, 160000000, 222000000, 342000000};
unsigned adreno8_gx_count = 4;
uint32_t adreno8_smmu_page = 0x1000, adreno8_smmu_pages = 32;
struct adreno8_gpu_block adreno8_gmu_hfi = {.address = 0x60006000, .size = ADRENO8_HFI_ALLOC_BYTES};
vm_offset_t adreno8_hfi_view_table;
static uint8_t g_hfi_backing[ADRENO8_HFI_ALLOC_BYTES] __attribute__((aligned(4096)));
static struct adreno8_memory g_vrb_buf = {.name = "vrb", .size = 4096};
static const struct adreno8_memory *adreno8_gmu_vrb = &g_vrb_buf;
static uint8_t g_vrb_backing[4096] __attribute__((aligned(4096)));

static bool g_cx_ready = true, g_sec_lost = false, g_zap_ok = true, g_hwsched_ok = true;
static bool g_fw_ok = true, g_gmu_start_ok = true, g_queues_ok = true, g_tables_ok = true;
static bool g_gx_ok = true, g_cp_ok = true, g_sec_exit_ok = true, g_scratch_ok = true;
static bool g_sync_ok = true, g_sticky_fsr = false, g_hfi_fault = false, g_irq_on = true;
static uint32_t g_sec_state = SECURE_XFER_READY;
static unsigned g_sec_aborts, g_fw_calls, g_gmu_start_calls, g_cp_calls, g_scratch_calls, g_hang_dumps;
static unsigned g_cm3_resets, g_init_res_clears, g_irq_resumes, g_sync_calls;

bool annibale_platform_is_annibale(void) { return true; }
int adreno8_say(const char *fmt, ...) { (void)fmt; return 0; }
void adreno8_sleep_us(uint32_t us) { g_clock_ns += (uint64_t)us * 1000ull; }
void adreno8_settle(void) {}
void adreno8_to_memory(vm_offset_t a, vm_size_t s) { (void)a; (void)s; }
uint32_t adreno8_rd(vm_offset_t a) { return *(volatile uint32_t *)a; }
void adreno8_wr(vm_offset_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
void adreno8_put(struct adreno8_block *b, const char *n, uint32_t o, uint32_t v) {
    (void)n;
    if (b == &adreno8_gmu) {
        if (o == GMU_CM3_SYSRESET && v == 1) g_cm3_resets++;
        if (o == GMU_FW_INIT_RESULT && v == 0) g_init_res_clears++;
    }
    if (b == &adreno8_smmu && (o & 0xfff) == SMMU_CB_FSR) {
        if (!g_sticky_fsr) *(volatile uint32_t *)(b->base + o) &= ~v;
        return;
    }
    *(volatile uint32_t *)(b->base + o) = v;
}

bool adreno8_cx_ready(void) { return g_cx_ready; }
bool adreno8_secure_lost(void) { return g_sec_lost; }
uint32_t adreno8_secure_xfer_state(void) { return g_sec_state; }
void adreno8_secure_xfer_abort(void) { g_sec_aborts++; g_sec_state = SECURE_XFER_TERMINAL; }
void adreno8_secure_xfer_set(uint32_t s) { g_sec_state = s; }
bool adreno8_secure_xfer(struct adreno8_gpu_space *sp, const struct adreno8_memory *b) {
    (void)sp; (void)b; return false;
}
bool adreno8_secure_xfer_ready(void) { return g_sec_state == SECURE_XFER_READY; }
bool adreno8_zap_ready(void) { return g_zap_ok; }
bool adreno8_space_check(struct adreno8_gpu_space *sp, uint32_t s, uint32_t m) { (void)s;(void)m; return sp->is_on; }
bool adreno8_space_route_check(struct adreno8_gpu_space *sp, uint32_t s, uint32_t m) { (void)sp;(void)s;(void)m; return true; }
bool adreno8_space_on(struct adreno8_gpu_space *sp, uint32_t s, uint32_t m) { (void)s;(void)m; sp->is_on = true; return true; }
bool adreno8_space_route(struct adreno8_gpu_space *sp, uint32_t s, uint32_t m) { (void)sp;(void)s;(void)m; return true; }
bool adreno8_scm_gpu_aperture(void) { return true; }
bool adreno8_space_find_range(struct adreno8_gpu_space *sp, uint32_t st, uint32_t en, uint32_t sz, uint32_t al, uint32_t *out) {
    (void)sp;(void)en;(void)sz;(void)al; static uint32_t next = 0x10000000u; *out = next; next += 0x100000u; (void)st; return true;
}
bool adreno8_space_map_region(struct adreno8_gpu_space *sp, uint32_t a, vm_offset_t m, uint32_t sz, uint32_t pr) {
    (void)sp;(void)a;(void)m;(void)sz;(void)pr; return true;
}
bool adreno8_space_faulted(struct adreno8_gpu_space *sp) {
    uint32_t cb = (adreno8_smmu_pages + sp->context) * adreno8_smmu_page;
    return (g_smmu_regs[(cb + SMMU_CB_FSR) / 4] & FSR_ALL) != 0;
}
bool adreno8_space_sync(struct adreno8_gpu_space *sp) { (void)sp; g_sync_calls++; return g_sync_ok; }
vm_offset_t adreno8_space_new_table(struct adreno8_gpu_space *sp, unsigned l, uint32_t idx) { (void)sp;(void)l;(void)idx; return 1; }
uint64_t *adreno8_space_entry(struct adreno8_gpu_space *sp, uint32_t va) { (void)sp;(void)va; static uint64_t pte; return &pte; }
bool adreno8_smmu_tlb_invalidate_range(struct adreno8_gpu_space *sp, uint64_t va, uint64_t sz) { (void)sp;(void)va;(void)sz; return true; }
bool adreno8_clock_started(void) { return true; }
void adreno8_speed_tick(void) {}
void adreno8_speed_state(uint32_t *m, uint32_t *c) { *m = 160; *c = 0; }
bool adreno8_hfi_poll(void) { return !g_hfi_fault; }
static unsigned g_hfi_sends;
bool adreno8_hfi_send(uint32_t id, const uint32_t *p, uint32_t c, const char *w) {
    (void)p;(void)c;(void)w; g_hfi_sends++;
    if (id == 10 && !g_hwsched_ok) return false;
    return true;
}
void adreno8_hfi_worked(uint64_t *u, uint32_t *s, uint32_t b[7]) { *u = *s = 0; memset(b, 0, 28); }
void adreno8_hfi_irq_attach(void) {}
uint32_t adreno8_hfi_irq_count(void) { return 0; }
void adreno8_hfi_irq_pause(uint32_t s, uint32_t p, uint32_t l) { (void)s;(void)p;(void)l; }
void adreno8_hfi_irq_resume(void) {
    if (g_irq_on) { g_irq_resumes++; g_gmu_regs[GMU_GMU2HOST_MASK / 4] = ~1u; }
}
void adreno8_hfi_reset_for_recovery(void) { adreno8_gmu_talks = false; adreno8_hfi_tables_sent = false; }
bool adreno8_hfi_tables_sent = true;

bool adreno8_step_firmware(void) { g_fw_calls++; if (!g_fw_ok) return false; adreno8_fw_loaded = true; return true; }
bool adreno8_step_gmu_start(void) { g_gmu_start_calls++; if (!g_gmu_start_ok) return false; adreno8_gmu_runs = true; return true; }
bool adreno8_step_queues(void) { if (!g_queues_ok) return false; adreno8_gmu_talks = true; return true; }
bool adreno8_step_tables(void) { if (!g_tables_ok) return false; adreno8_hfi_tables_sent = true; return true; }
bool adreno8_step_gx_power(void) { return g_gx_ok; }

static bool g_gpu_cmds_ok = true, g_gpu_dumped = false;
bool adreno8_gpu_commands_ready(void) { return g_gpu_cmds_ok; }
bool adreno8_gpu_wait_idle(const char *w) { (void)w; return true; }
void adreno8_gpu_oob_release(void) {}
bool adreno8_gpu_oob_acquire(void) { return true; }
void adreno8_gpu_hang_dump(void) { if (!g_gpu_dumped) { g_gpu_dumped = true; g_hang_dumps++; } }
bool adreno8_gpu_cp_init(void) { g_cp_calls++; return g_cp_ok; }
bool adreno8_gpu_secure_exit(void) { return g_sec_exit_ok; }
bool adreno8_gpu_scratch_test(void) { g_scratch_calls++; return g_scratch_ok; }
void adreno8_gpu_reset_for_recovery(void) { g_gpu_dumped = false; g_gpu_cmds_ok = true; }
bool AnnibaleGPUStart(void) { return true; }
'''

CASES = r'''
static void setup_world(bool recover_on) {
    memset(g_gmu_regs, 0, sizeof(g_gmu_regs));
    memset(g_smmu_regs, 0, sizeof(g_smmu_regs));
    memset(g_hfi_backing, 0, sizeof(g_hfi_backing));
    memset(g_vrb_backing, 0, sizeof(g_vrb_backing));
    adreno8_gmu.base = (vm_offset_t)g_gmu_regs;
    adreno8_smmu.base = (vm_offset_t)g_smmu_regs;
    adreno8_hfi_view_table = (vm_offset_t)g_hfi_backing;
    g_vrb_buf.memory = (vm_offset_t)g_vrb_backing;
    adreno8_gpu_space.tables[0].phys = 0x40000000ULL;
    adreno8_gpu_space.table_count = 1;
    adreno8_gmu_space.tables[0].phys = 0x50000000ULL;
    adreno8_gmu_space.table_count = 1;
    adreno8_gpu_space.is_on = true;
    adreno8_gmu_space.is_on = true;
    adreno8_secure_space.is_on = true;
    adreno8_gpu_recover_enabled = recover_on;
    adreno8_fw_loaded = adreno8_gmu_runs = adreno8_gmu_talks = adreno8_hfi_tables_sent = true;
    adreno8_hwsched_attempted = adreno8_hwsched_started = true;
    adreno8_memory_failed = false;
    adreno8_memory_bytes = 0;
    adreno8_memory_count = 1;
    adreno8_memory_entries[0] = (struct adreno8_memory_entry){
        .buffer = {.name = "hwsched-secure", .memory = 0x1000, .size = 0x138000, .gpu_address = 0xf0000000u},
        .request = {.flags = 0x1007u, .kind = 5, .handle = 11, .size = 0x138000, .align = 0xc10},
        .from_hfi = true, .complete = true, .secure = true,
    };
    g_cx_ready = g_zap_ok = g_hwsched_ok = g_fw_ok = g_gmu_start_ok = g_queues_ok = g_tables_ok = true;
    g_gx_ok = g_cp_ok = g_sec_exit_ok = g_scratch_ok = g_sync_ok = g_gpu_cmds_ok = true;
    g_sec_lost = g_sticky_fsr = g_hfi_fault = g_gpu_dumped = false;
    g_sec_state = SECURE_XFER_READY;
    g_sec_aborts = g_fw_calls = g_gmu_start_calls = g_cp_calls = g_scratch_calls = g_hang_dumps = 0;
    g_cm3_resets = g_init_res_clears = g_irq_resumes = g_sync_calls = g_hfi_sends = 0;
    bo_guard = (vm_offset_t)g_vrb_backing;
    bo_state = BO_ENABLED;
    context_tested = true;
    context_failed = false;
    for (uint32_t i = 0; i < ADRENO8_CONTEXT_SLOTS; i++) {
        struct adreno8_context_slot *s = &context_slots[i];
        s->state = CONTEXT_PREPARED;
        if (!s->queue) {
            s->queue = adreno8_gmu_alloc("q", CONTEXT_BYTES, CONTEXT_BYTES, 11);
            s->timestamps = adreno8_gpu_alloc("ts", CONTEXT_BYTES, CONTEXT_BYTES, 11);
            s->queue_view = s->queue->memory;
            s->timestamp_view = s->timestamps->memory;
            adreno8_wr(s->queue_view + 4, s->queue->gmu_address + CONTEXT_HEADER_WORDS * 4);
            adreno8_wr(s->queue_view + 8, CONTEXT_QUEUE_WORDS);
        }
        adreno8_wr(s->queue_view + 20, 0);
        adreno8_wr(s->queue_view + 24, 0);
        adreno8_wr(s->timestamp_view + CONTEXT_SOP, 0);
        adreno8_wr(s->timestamp_view + CONTEXT_EOP, 0);
        s->last = 0;
        s->checked = 0;
    }
    assert(adreno8_step_service());
}

static void run_in_child(void (*fn)(void)) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) { fn(); _exit(0); }
    int st = 0;
    assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void case_disabled_default(void) {
    /* Default gpu_recover=0: report dumps without bo_stop; recover stops and aborts secure state. */
    setup_world(false);
    annibale_gpu_report("no answer");
    assert(g_hang_dumps == 1 && bo_state == BO_ENABLED && g_sec_state == SECURE_XFER_READY);
    assert(!annibale_gpu_recover());
    assert(bo_state == BO_FAILED && g_sec_state == SECURE_XFER_TERMINAL && g_sec_aborts == 1);
}

static void case_sticky_fsr_terminal(void) {
    /* Unclearable CB0 FSR refuses recovery and closes permanently. */
    setup_world(true);
    g_sticky_fsr = true;
    g_smmu_regs[(adreno8_smmu_pages * adreno8_smmu_page + SMMU_CB_FSR) / 4] = 0x408u;
    assert(!annibale_gpu_recover());
    assert(bo_state == BO_FAILED && g_sec_state == SECURE_XFER_TERMINAL && g_sec_aborts == 1);
    g_sticky_fsr = false;
    g_smmu_regs[(adreno8_smmu_pages * adreno8_smmu_page + SMMU_CB_FSR) / 4] = 0;
    assert(!annibale_gpu_recover());
}

static void case_secure_lost_terminal(void) {
    /* Lost secure state during faulted wait forces terminal abort without replaying recovery. */
    setup_world(true);
    g_sec_lost = true;
    assert(!annibale_gpu_recover());
    assert(bo_state == BO_FAILED && g_sec_state == SECURE_XFER_TERMINAL && g_sec_aborts == 1);
}

static void case_step_failure_terminal(void) {
    /* Mid-step firmware failure sets terminal abort and stays latched. */
    setup_world(true);
    g_fw_ok = false;
    assert(!annibale_gpu_recover());
    assert(bo_state == BO_FAILED && g_sec_state == SECURE_XFER_TERMINAL && g_sec_aborts == 1);
    g_fw_ok = true;
    assert(!annibale_gpu_recover());
}

int main(void) {
    run_in_child(case_disabled_default);

    /* gpu_recover=1: HFI 150 + CB0 SMMU fault preserves SECURE_XFER_READY across report, then recovers cleanly. */
    setup_world(true);
    uint64_t addr = 0x10000000u; uint32_t words = 4;
    uint32_t f0 = annibale_gpu_submit(&addr, &words, 1);
    uint32_t f1 = adreno8_context_submit_slot(ADRENO8_CONTEXT_SLOT_NORMAL, &addr, &words, 1);
    assert(f0 != 0 && f1 != 0);
    g_smmu_regs[(adreno8_smmu_pages * adreno8_smmu_page + SMMU_CB_FSR) / 4] = 0x408u;
    g_hfi_fault = true;
    assert(!adreno8_bo_ready() && bo_state == BO_FAILED);
    assert(g_sec_state == SECURE_XFER_READY && g_sec_aborts == 0);
    annibale_gpu_report("faulted");
    assert(g_hang_dumps == 1);
    g_hfi_fault = false;
    g_gmu_regs[GMU_FW_INIT_RESULT / 4] = GMU_FW_STARTED;
    assert(annibale_gpu_recover());
    assert(g_cm3_resets == 1 && g_init_res_clears == 1 && g_sync_calls == 1);
    assert(g_fw_calls == 1 && g_gmu_start_calls == 1 && g_cp_calls == 1 && g_scratch_calls == 1);
    assert(g_irq_resumes == 1 && g_gmu_regs[GMU_GMU2HOST_MASK / 4] == ~1u);
    assert(!adreno8_space_faulted(&adreno8_gpu_space) && bo_state == BO_ENABLED);
    assert(g_sec_state == SECURE_XFER_READY && g_sec_aborts == 0);
    struct adreno8_hfi_mem_result res = {0};
    assert(adreno8_hwsched_alloc(&adreno8_memory_entries[0].request, &res) && res.secure && res.gpu_address == 0xf0000000u);
    assert(annibale_gpu_done(f0) && annibale_gpu_done(f1));
    uint32_t f0_next = annibale_gpu_submit(&addr, &words, 1);
    assert(f0_next == f0 + 1u);

    /* Second hang dumps again (gpu_hang_dumped reset) and recovers a second time. */
    annibale_gpu_report("no answer");
    assert(g_hang_dumps == 2);
    assert(annibale_gpu_recover() && g_hang_dumps == 2 && bo_state == BO_ENABLED);

    run_in_child(case_sticky_fsr_terminal);
    run_in_child(case_secure_lost_terminal);
    run_in_child(case_step_failure_terminal);

    puts("G30 Phase 2 GPU recovery, secure preservation, SMMU clear and monotonic fences: PASS");
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
    gmu = (DEV / "adreno8_gmu.c").read_text()
    smmu = (DEV / "adreno8_smmu.c").read_text()
    mem = (DEV / "adreno8_memory.c").read_text()
    hw = (DEV / "adreno8_hwsched.c").read_text()
    ctx = (DEV / "adreno8_context.c").read_text()
    bo = (DEV / "adreno8_bo.c").read_text()
    disp = (ANN / "annibale_gpu_dispatch.c").read_text()

    body = "\n".join([
        HARNESS,
        fn(smmu, "adreno8_space_clear_fault"),
        mem[mem.index("#define MEMORY_GRANULE"):],
        hw[hw.index("static bool adreno8_hwsched_attempted"):],
        fn(gmu, "adreno8_gmu_reset_for_recovery"),
        ctx[ctx.index("#define CONTEXT_BYTES"):],
        bo[bo.index("static enum { BO_COLD"):],
        disp[disp.index("uint32_t gpu_va;"):],
        CASES,
    ])

    with tempfile.TemporaryDirectory(prefix="adreno8-recover-") as td:
        tdir = Path(td)
        for rel, content in SYS_STUBS.items():
            p = tdir / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        for hdr in DEV.glob("*.h"):
            (tdir / hdr.name).write_text(hdr.read_text())
        src = tdir / "recover_test.c"
        exe = tdir / "recover_test"
        src.write_text(body)
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            f"-I{tdir}", str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Host ASan+UBSan unit tests for psci_idle=1 (C4 0x00000004 retention idle) in psci_smp.c."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PSCI_C = ROOT / "xnu/files/osfmk/arm64/psci_smp.c"

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CPUS 8
#define NSEC_PER_USEC 1000ULL
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC  1000000000ULL
#define kSuccess 0
#define GIC_SIGNAL_KINDS 13
#define relaxed __ATOMIC_RELAXED
#define acquire __ATOMIC_ACQUIRE
#define release __ATOMIC_RELEASE
#define acq_rel __ATOMIC_ACQ_REL

typedef int boolean_t;
typedef void *DTEntry;

#define os_atomic_load(p, mo)       __atomic_load_n((p), (mo))
#define os_atomic_store(p, v, mo)   __atomic_store_n((p), (v), (mo))
#define os_atomic_xchg(p, v, mo)    __atomic_exchange_n((p), (v), (mo))

struct cpu_stat { uint64_t irq_ex_cnt, ipi_cnt, timer_cnt; };
typedef struct {
    uint32_t cpu_phys_id;
    uint32_t cpu_signal;
    uint32_t cpu_flags;
    uint64_t rtcPop;
    uint64_t cpu_idle_pop;
    uint64_t wfe_count;
    struct cpu_stat cpu_stat;
} cpu_data_t;

static cpu_data_t g_cpus[MAX_CPUS];
static int g_cur_cpu = 0;
static uint64_t g_now = 10000000ULL;
static uint32_t g_psci_idle_arg = 0;
static uint32_t g_psci_cl5_arg = 0;
static int64_t g_psci_ver = 0x00010001LL;
static int64_t g_psci_feat = 0x3LL;
static int64_t g_psci_mode_feat = 0;
static int64_t g_psci_set_mode_ret = 0;
static int64_t g_psci_suspend_ret = 0;
static int64_t (*g_suspend_hook)(uint64_t power_state);
static unsigned g_wfi_calls = 0;
static unsigned g_suspend_calls = 0;
static unsigned g_set_mode_calls = 0;
static uint64_t g_last_suspend_mode = 999;
static uint64_t g_last_power_state = 0;
static uint64_t g_last_entry = 999;
static uint64_t g_last_ctx = 999;
boolean_t idle_enable = 1;

static cpu_data_t *current_cpu_datap(void) { return &g_cpus[g_cur_cpu]; }
static cpu_data_t *cpu_datap(int cpu) { return (cpu >= 0 && cpu < 4) ? &g_cpus[cpu] : NULL; }
static int cpu_number(void) { return g_cur_cpu; }
static int ml_get_max_cpu_number(void) { return 3; }
static uint64_t mach_absolute_time(void) { return g_now; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *res) { *res = ns; } /* 1 tick = 1 ns in test */
static int SecureDTLookupEntry(void *b, const char *p, DTEntry *e) { (void)b; (void)p; *e = NULL; return 1; }
static int SecureDTGetProperty(DTEntry e, const char *n, void const **v, unsigned *s) { (void)e; (void)n; (void)v; (void)s; return 1; }
static bool PE_parse_boot_argn(const char *k, void *v, unsigned s) {
    if (strcmp(k, "psci_idle") == 0 && s == sizeof(uint32_t)) {
        *(uint32_t *)v = g_psci_idle_arg;
        return g_psci_idle_arg != 0;
    }
    if (strcmp(k, "psci_cl5") == 0 && s == sizeof(uint32_t)) {
        *(uint32_t *)v = g_psci_cl5_arg;
        return g_psci_cl5_arg != 0;
    }
    return false;
}
static void gic_v3_signal_kinds(int cpu, uint32_t kinds[GIC_SIGNAL_KINDS]) {
    (void)cpu;
    memset(kinds, 0, sizeof(uint32_t) * GIC_SIGNAL_KINDS);
}

#define __builtin_arm_wfi() do { g_wfi_calls++; } while (0)

#define PSCI_VERSION            0x84000000u
#define PSCI_FEATURES           0x8400000Au
#define PSCI_SET_SUSPEND_MODE   0x8400000Fu
#define PSCI_SUSPEND_MODE_OSI   1u
#define PSCI_CPU_SUSPEND        0xC4000001u
#define PSCI_RET_DENIED         (-3)
#define PSCI_C4_STATE_ID        0x00000004u
#define PSCI_CL5_STATE_ID       0x01000054u
#define PSCI_C4_MIN_RES_C0_US   560u
#define PSCI_C4_MIN_RES_C1_US   686u
#define PSCI_CL5_MIN_RES_C1_US  9144u

uint32_t psci_smp_hvc;
static bool     psci_idle_enabled;
static bool     psci_idle_disabled;
static bool     psci_cl5_enabled;
static bool     psci_cl5_disabled;
static uint64_t psci_c4_min_ticks[2];
static uint64_t psci_cl5_min_ticks;
static uint32_t psci_c1_lock;
static uint32_t psci_c1_idle_mask;
static uint64_t psci_c1_pop[2];
static uint64_t psci_c4_count[MAX_CPUS];
static uint64_t psci_c4_err_count[MAX_CPUS];
static uint64_t psci_cl5_count[MAX_CPUS];
static uint64_t psci_cl5_denied_count[MAX_CPUS];
static uint64_t psci_cl5_err_count[MAX_CPUS];

static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (fn == PSCI_VERSION) return g_psci_ver;
    if (fn == PSCI_FEATURES && a1 == PSCI_CPU_SUSPEND) return g_psci_feat;
    if (fn == PSCI_FEATURES && a1 == PSCI_SET_SUSPEND_MODE) return g_psci_mode_feat;
    if (fn == PSCI_SET_SUSPEND_MODE) {
        g_set_mode_calls++;
        g_last_suspend_mode = a1;
        return g_psci_set_mode_ret;
    }
    if (fn == PSCI_CPU_SUSPEND) {
        g_suspend_calls++;
        g_last_power_state = a1;
        g_last_entry = a2;
        g_last_ctx = a3;
        if (g_suspend_hook) return g_suspend_hook(a1);
        return g_psci_suspend_ret;
    }
    return -1;
}

__PSCI_FUNCS__

static void reset_state(void) {
    psci_idle_enabled = false;
    psci_idle_disabled = false;
    psci_cl5_enabled = false;
    psci_cl5_disabled = false;
    memset(psci_c4_min_ticks, 0, sizeof(psci_c4_min_ticks));
    psci_cl5_min_ticks = 0;
    psci_c1_lock = 0;
    psci_c1_idle_mask = 0;
    memset(psci_c1_pop, 0, sizeof(psci_c1_pop));
    memset(psci_c4_count, 0, sizeof(psci_c4_count));
    memset(psci_c4_err_count, 0, sizeof(psci_c4_err_count));
    memset(psci_cl5_count, 0, sizeof(psci_cl5_count));
    memset(psci_cl5_denied_count, 0, sizeof(psci_cl5_denied_count));
    memset(psci_cl5_err_count, 0, sizeof(psci_cl5_err_count));
    memset(g_cpus, 0, sizeof(g_cpus));
    g_cpus[0].cpu_phys_id = 0x10100u; /* cluster1 prime */
    g_cpus[1].cpu_phys_id = 0x10000u; /* cluster1 prime */
    g_cpus[2].cpu_phys_id = 0x00100u; /* cluster0 medium */
    g_cpus[3].cpu_phys_id = 0x00200u; /* cluster0 medium */
    g_cur_cpu = 0;
    g_now = 10000000ULL;
    g_wfi_calls = 0;
    g_suspend_calls = 0;
    g_set_mode_calls = 0;
    g_last_suspend_mode = 999;
    g_psci_ver = 0x00010001LL;
    g_psci_feat = 0x3LL;
    g_psci_mode_feat = 0;
    g_psci_set_mode_ret = 0;
    g_psci_suspend_ret = 0;
    g_suspend_hook = NULL;
    g_psci_cl5_arg = 0;
}

static unsigned g_hook_stage = 0;
static uint64_t g_hook_cpu1_pop_us = 10000ULL;
static int64_t g_hook_cl5_ret = 0;

static int64_t peer_enter_hook(uint64_t power_state) {
    if (g_hook_stage == 0 && power_state == PSCI_C4_STATE_ID) {
        g_hook_stage = 1;
        int saved = g_cur_cpu;
        g_cur_cpu = 1;
        g_cpus[1].rtcPop = g_now + g_hook_cpu1_pop_us * 1000ULL;
        psci_smp_cpu_idle();
        g_cur_cpu = saved;
        return 0;
    }
    if (power_state == PSCI_CL5_STATE_ID) {
        return g_hook_cl5_ret;
    }
    return 0;
}

int main(void) {
    char report[2048];

    /* 1. Default psci_idle=0 -> always WFI, zero PSCI_CPU_SUSPEND calls. */
    reset_state();
    g_psci_idle_arg = 0;
    psci_smp_init();
    g_cur_cpu = 2;
    g_cpus[2].rtcPop = g_now + 2000000ULL; /* 2 ms */
    psci_smp_cpu_idle();
    assert(g_suspend_calls == 0 && g_wfi_calls == 1);

    /* 2. psci_idle=1 + firmware unsupported (feat=-1) -> stays disabled, WFI only. */
    reset_state();
    g_psci_idle_arg = 1;
    g_psci_feat = -1;
    psci_smp_init();
    g_cur_cpu = 2;
    g_cpus[2].rtcPop = g_now + 2000000ULL;
    psci_smp_cpu_idle();
    assert(g_suspend_calls == 0 && g_wfi_calls == 1);

    /* 3. psci_idle=1 + PSCI 1.1: cluster0 (560 us) and cluster1 (686 us) via rtcPop (cpu_idle_pop == 0). */
    reset_state();
    g_psci_idle_arg = 1;
    psci_smp_init();
    /* cluster0: 559 us -> WFI; 560 us -> C4 */
    g_cur_cpu = 2;
    g_cpus[2].rtcPop = g_now + 559000ULL;
    psci_smp_cpu_idle();
    assert(g_suspend_calls == 0 && g_wfi_calls == 1);
    g_cpus[2].rtcPop = g_now + 560000ULL;
    psci_smp_cpu_idle();
    assert(g_suspend_calls == 1 && g_wfi_calls == 1);
    assert(g_last_power_state == 0x00000004u && g_last_entry == 0 && g_last_ctx == 0);
    assert(psci_c4_count[2] == 1 && psci_c4_err_count[2] == 0);

    /* Earlier cpu_idle_pop overrides longer rtcPop and keeps core in WFI when < min residency. */
    g_cpus[2].rtcPop = g_now + 2000000ULL;
    g_cpus[2].cpu_idle_pop = g_now + 400000ULL;
    psci_smp_cpu_idle();
    assert(g_suspend_calls == 1 && g_wfi_calls == 2);
    g_cpus[2].cpu_idle_pop = 0;

    /* cluster1: 685 us -> WFI; 686 us -> C4 */
    g_cur_cpu = 0;
    g_cpus[0].rtcPop = g_now + 685000ULL;
    psci_smp_cpu_idle();
    assert(g_suspend_calls == 1 && g_wfi_calls == 3);
    g_cpus[0].rtcPop = g_now + 686000ULL;
    psci_smp_cpu_idle();
    assert(g_suspend_calls == 2 && g_wfi_calls == 3);
    assert(psci_c4_count[0] == 1);

    /* Verify kern.annibale.cpus report includes psci_idle, psci_cl5, c4, cl5, cl5_denied, cl5_err */
    int n = psci_smp_report(report, sizeof(report));
    assert(n > 0 && strstr(report, "psci_idle 1 psci_cl5 0") != NULL);
    assert(strstr(report, "c4 1 c4_err 0 cl5 0 cl5_denied 0 cl5_err 0") != NULL);

    /* 4. Non-zero PSCI error latches psci_idle_disabled and falls back to WFI permanently. */
    g_psci_suspend_ret = -2; /* invalid parameters */
    g_cpus[0].rtcPop = g_now + 5000000ULL;
    psci_smp_cpu_idle();
    assert(g_suspend_calls == 3 && g_wfi_calls == 4 && psci_c4_err_count[0] == 1);
    assert(psci_idle_disabled == true);
    /* Subsequent long idle skips PSCI_CPU_SUSPEND completely */
    psci_smp_cpu_idle();
    assert(g_suspend_calls == 3 && g_wfi_calls == 5);
    n = psci_smp_report(report, sizeof(report));
    assert(n > 0 && strstr(report, "psci_idle 0") != NULL && strstr(report, "c4 1 c4_err 1") != NULL);

    /* 5. psci_cl5=1 without psci_idle=1 or without OSI support does not call SET_SUSPEND_MODE or CL5. */
    reset_state();
    g_psci_idle_arg = 0; g_psci_cl5_arg = 1;
    psci_smp_init();
    assert(g_set_mode_calls == 0 && !psci_idle_enabled && !psci_cl5_enabled);

    reset_state();
    g_psci_idle_arg = 1; g_psci_cl5_arg = 1; g_psci_feat = 0x2LL; /* osi=0 */
    psci_smp_init();
    assert(g_set_mode_calls == 0 && psci_idle_enabled && !psci_cl5_enabled);

    reset_state();
    g_psci_idle_arg = 1; g_psci_cl5_arg = 1; g_psci_set_mode_ret = 0xfffffffdLL; /* zero-extended w0 (-3) */
    psci_smp_init();
    assert(g_set_mode_calls == 1 && g_last_suspend_mode == PSCI_SUSPEND_MODE_OSI);
    assert(psci_idle_enabled && !psci_cl5_enabled);

    /* 6. psci_idle=1 && psci_cl5=1 with OSI: dual-core cluster1 coordination and residency gate (9144 us). */
    reset_state();
    g_psci_idle_arg = 1; g_psci_cl5_arg = 1;
    psci_smp_init();
    assert(g_set_mode_calls == 1 && g_last_suspend_mode == PSCI_SUSPEND_MODE_OSI && psci_cl5_enabled);

    /* Solo cluster1 core idle stays on C4 (other cluster1 core active). */
    g_cur_cpu = 0; g_cpus[0].rtcPop = g_now + 20000000ULL;
    psci_smp_cpu_idle();
    assert(g_last_power_state == PSCI_C4_STATE_ID && psci_c4_count[0] == 1 && psci_cl5_count[0] == 0);
    assert(psci_c1_idle_mask == 0);

    /* Both cluster1 cores idle, but cpu 0 has < 9144 us (9143 us): last core (cpu 1) must still send C4. */
    g_hook_stage = 0; g_hook_cpu1_pop_us = 20000ULL; g_hook_cl5_ret = 0;
    g_suspend_hook = peer_enter_hook;
    g_cur_cpu = 0; g_cpus[0].rtcPop = g_now + 9143000ULL;
    psci_smp_cpu_idle();
    assert(g_last_power_state == PSCI_C4_STATE_ID && psci_c4_count[1] == 1 && psci_cl5_count[1] == 0);
    assert(psci_c1_idle_mask == 0);

    /* Both cluster1 cores idle with >= 9144 us (exact boundary 9144 us): last core (cpu 1) enters CL5 0x01000054. */
    g_hook_stage = 0; g_hook_cpu1_pop_us = 9144ULL; g_hook_cl5_ret = 0;
    g_cur_cpu = 0; g_cpus[0].rtcPop = g_now + 20000000ULL;
    psci_smp_cpu_idle();
    assert(g_last_power_state == PSCI_CL5_STATE_ID && psci_cl5_count[1] == 1 && psci_c1_idle_mask == 0);

    /* Cluster0 core stays on C4 even when residency > 9144 us and psci_cl5_enabled is true. */
    g_suspend_hook = NULL;
    g_cur_cpu = 2; g_cpus[2].rtcPop = g_now + 20000000ULL;
    psci_smp_cpu_idle();
    assert(g_last_power_state == PSCI_C4_STATE_ID && psci_c4_count[2] == 1 && psci_cl5_count[2] == 0);

    /* 7. Transient PSCI_DENIED (-3, including zero-extended w0 0xfffffffd) on CL5 falls back to C4 without disabling CL5 or C4. */
    g_hook_stage = 0; g_hook_cpu1_pop_us = 15000ULL; g_hook_cl5_ret = 0xfffffffdLL;
    g_suspend_hook = peer_enter_hook;
    g_cur_cpu = 0; g_cpus[0].rtcPop = g_now + 20000000ULL;
    psci_smp_cpu_idle();
    assert(!psci_idle_disabled && !psci_cl5_disabled);
    assert(psci_cl5_denied_count[1] == 1 && psci_cl5_err_count[1] == 0 && psci_c4_count[1] == 2);
    /* Subsequent idle when peer stays asleep succeeds in CL5 again. */
    g_hook_stage = 0; g_hook_cl5_ret = 0;
    psci_smp_cpu_idle();
    assert(psci_cl5_count[1] == 2);

    n = psci_smp_report(report, sizeof(report));
    assert(n > 0 && strstr(report, "psci_idle 1 psci_cl5 1") != NULL);
    assert(strstr(report, "cl5 2 cl5_denied 1 cl5_err 0") != NULL);

    /* 8. Non-transient CL5 error (-2) disables only CL5 while leaving C4 active. */
    g_hook_stage = 0; g_hook_cl5_ret = -2;
    psci_smp_cpu_idle();
    assert(psci_cl5_disabled && !psci_idle_disabled && psci_cl5_err_count[1] == 1 && psci_c4_count[1] == 3);
    n = psci_smp_report(report, sizeof(report));
    assert(n > 0 && strstr(report, "psci_idle 1 psci_cl5 0") != NULL);

    puts("PSCI C4/CL5 OSI idle (default-off, residency gates, -3 backoff, error isolation, report): PASS");
    return 0;
}
'''


def extract_funcs(src: str) -> str:
    err_start = src.index("static const char *\npsci_error(")
    err_end = src.index("\n/* A processor other than the boot processor", err_start)
    rep_start = src.index("int\npsci_smp_report(")
    rep_end = src.index("\n/*\n * kern.annibale.prof", rep_start)
    return src[err_start:err_end] + "\n" + src[rep_start:rep_end] + "\n"


def main() -> None:
    code = HARNESS.replace("__PSCI_FUNCS__", extract_funcs(PSCI_C.read_text()))
    with tempfile.TemporaryDirectory(prefix="psci-idle-") as d:
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

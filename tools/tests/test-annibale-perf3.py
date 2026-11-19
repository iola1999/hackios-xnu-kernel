#!/usr/bin/env python3
"""Host tests for PERF-3 (IOGPU control userclient + perfcontrol hooks) and PERF-5 locore patch."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ANNIBALE = ROOT / "xnu/files/bsd/dev/annibale"
GPU_CPP = (ROOT / "xnu/files/iokit/Kernel/AnnibaleGPU.cpp").read_text()
CPUFREQ_C = (ANNIBALE / "annibale_cpufreq.c").read_text()
LOCORE_PATCH = (ROOT / "xnu26/patches/80-osfmk%arm64%locore.s.patch").read_text()
WORK_INTERVAL_PATCH = (ROOT / "xnu26/patches/14-bsd%kern%sys_work_interval.c.patch").read_text()

# Static contract checks on PERF-5 and PERF-3 XNU upstream patches.
for token in ("CPU_BASE_TIMEBASE", "CPU_PENDING_AST", "annibale_timebase_scale_125",
              "mach_absolutetime_asleep", "ERET_NO_STRAIGHT_LINE_SPECULATION"):
    assert token in LOCORE_PATCH, f"missing {token} in locore.s patch"
for token in ("annibale_wi_instance", "kern_work_interval_lookup_type",
              "annibale_perfctl_work_interval", "wi_next_instance_id"):
    assert token in WORK_INTERVAL_PATCH, f"missing {token} in sys_work_interval.c patch"
assert ".version = SCHED_PERFCONTROL_CALLBACKS_VERSION_7" in CPUFREQ_C
assert "SCHED_PERFCONTROL_CALLBACKS_VERSION_CURRENT" not in CPUFREQ_C
assert "class IOGPU : public IOService" in GPU_CPP
assert "class AnnibaleGPU : public IOGPU" in GPU_CPP

# Extract production role helpers + iogpuControlMethod from AnnibaleGPU.cpp
role_block = GPU_CPP[
    GPU_CPP.index("struct gpu_pid_role {") : GPU_CPP.index("/* What the last seconds")
]
ctrl_start = GPU_CPP.index("IOReturn\nAnnibaleGPUUserClient::iogpuControlMethod(")
ctrl_end = GPU_CPP.index("\nIOReturn\nAnnibaleGPUUserClient::externalMethod(", ctrl_start)
ctrl_fn = GPU_CPP[ctrl_start:ctrl_end]

# Extract production perfcontrol + perf_want logic from annibale_cpufreq.c
cpufreq_state = re.search(
    r"static uint64_t touch_at;.*?\nstatic bool governing;",
    CPUFREQ_C, flags=re.S,
).group()
pd_struct = re.search(
    r"/\* domain N is cluster N.*?\} perf_domain\[PERF_DOMAINS\];",
    CPUFREQ_C, flags=re.S,
).group()
perf_want_match = re.search(
    r"/\* The level the governor wants.*?\nstatic uint32_t\nperf_want\(.*?\n\}",
    CPUFREQ_C, flags=re.S,
)
assert perf_want_match is not None
perf_want_fn = perf_want_match.group()
perfctl_block = CPUFREQ_C[
    CPUFREQ_C.index("static void\natomic_max_u64(") :
    CPUFREQ_C.index("/* A touch (annibale_input.c's touch_post())")
]

cpp_harness = rf'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

using IOReturn = int;
using UInt32 = unsigned;
using pid_t = int;
using task_t = void *;
using IOSimpleLock = int;
enum {{ kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnUnsupported = 2 }};
static constexpr uint64_t NSEC_PER_MSEC = 1000000ULL;
static uint64_t mock_now;
static uint64_t mach_absolute_time(void) {{ return mock_now; }}
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *out) {{ *out = ns; }}
static void IOSimpleLockLock(IOSimpleLock *) {{}}
static void IOSimpleLockUnlock(IOSimpleLock *) {{}}
static IOSimpleLock dummy_lock = 1;
static struct {{ uint64_t id; uint64_t getRegistryEntryID() const {{ return id; }} }} dummy_gpu{{0x123456789abcdef0ULL}}, *gGPUService = &dummy_gpu;
struct IOExternalMethodArguments {{
    void *structureInput;
    uint32_t structureInputSize;
    void *structureOutput;
    uint32_t structureOutputSize;
    uint64_t *scalarInput;
    uint32_t scalarInputCount;
    uint64_t *scalarOutput;
    uint32_t scalarOutputCount;
}};

{role_block}

struct AnnibaleGPUUserClient {{
    uint32_t fClientType = 0;
    pid_t fPid = 0;
    bool renderCalled = false;
    IOReturn iogpuControlMethod(uint32_t selector, IOExternalMethodArguments * args);
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args) {{
        if (fClientType == 1u) return iogpuControlMethod(selector, args);
        renderCalled = true;
        return kIOReturnSuccess;
    }}
}};
{ctrl_fn}

int main() {{
    gGPURoleLock = &dummy_lock;
    AnnibaleGPUUserClient rb{{1u, 100}}, render{{0u, 200}};

    uint8_t out0[0x40]; memset(out0, 0x55, sizeof(out0));
    IOExternalMethodArguments a0{{nullptr, 0, out0, sizeof(out0), nullptr, 0, nullptr, 0}};
    assert(rb.externalMethod(0, &a0) == kIOReturnSuccess && a0.structureOutputSize == 0x40);
    for (uint8_t b : out0) assert(b == 0);
    assert(!rb.renderCalled);

    uint8_t out2[0x218]; memset(out2, 0x55, sizeof(out2));
    IOExternalMethodArguments a2{{nullptr, 0, out2, sizeof(out2), nullptr, 0, nullptr, 0}};
    assert(rb.externalMethod(2, &a2) == kIOReturnSuccess && a2.structureOutputSize == 0x218);
    uint64_t reg = 0; memcpy(&reg, out2, 8);
    uint32_t num_ch = 99; memcpy(&num_ch, out2 + 0x14, 4);
    assert(reg == 0x123456789abcdef0ULL && num_ch == 0);

    uint8_t out4[0x20]; memset(out4, 0x55, sizeof(out4));
    IOExternalMethodArguments a4{{nullptr, 0, out4, sizeof(out4), nullptr, 0, nullptr, 0}};
    assert(rb.externalMethod(4, &a4) == kIOReturnSuccess && a4.structureOutputSize == 0x20);
    for (uint8_t b : out4) assert(b == 0);

    // Too-small buffer and unknown selector rejected; type 0 isolated from type 1.
    IOExternalMethodArguments a_small{{nullptr, 0, out2, 0x200, nullptr, 0, nullptr, 0}};
    assert(rb.externalMethod(2, &a_small) == kIOReturnBadArgument);
    assert(rb.externalMethod(0x7e, &a0) == kIOReturnUnsupported);
    assert(render.externalMethod(0, &a0) == kIOReturnSuccess && render.renderCalled);

    // Default role is 2 (Background), set/get works and 64-slot LRU evicts oldest PID.
    uint64_t sc_in[2] = {{ 200, 0 }}, sc_out[1] = {{ 99 }};
    IOExternalMethodArguments a_get{{nullptr, 0, nullptr, 0, sc_in, 1, sc_out, 1}};
    assert(rb.externalMethod(0x22, &a_get) == kIOReturnSuccess && sc_out[0] == 2);
    IOExternalMethodArguments a_set{{nullptr, 0, nullptr, 0, sc_in, 2, nullptr, 0}};
    assert(rb.externalMethod(0x21, &a_set) == kIOReturnSuccess);
    assert(rb.externalMethod(0x22, &a_get) == kIOReturnSuccess && sc_out[0] == 0);

    mock_now = 1000000ULL;
    gpuNoteSubmit(200);
    assert(annibale_gpu_focal_active(1000000ULL + 10 * NSEC_PER_MSEC));
    assert(!annibale_gpu_focal_active(1000000ULL + 60 * NSEC_PER_MSEC));

    // Role clamping and 64-slot LRU eviction (fill remaining 62 slots, keep pid 200 hot, then insert 1062 -> evicts pid 300).
    sc_in[0] = 300; sc_in[1] = 9;
    assert(rb.externalMethod(0x21, &a_set) == kIOReturnSuccess);
    assert(rb.externalMethod(0x22, &a_get) == kIOReturnSuccess && sc_out[0] == 2);
    for (int pid = 1000; pid < 1062; pid++) {{
        sc_in[0] = (uint64_t)pid; sc_in[1] = 1;
        assert(rb.externalMethod(0x21, &a_set) == kIOReturnSuccess);
    }}
    sc_in[0] = 200;
    assert(rb.externalMethod(0x22, &a_get) == kIOReturnSuccess && sc_out[0] == 0);
    sc_in[0] = 1062; sc_in[1] = 1;
    assert(rb.externalMethod(0x21, &a_set) == kIOReturnSuccess);
    sc_in[0] = 200;
    assert(rb.externalMethod(0x22, &a_get) == kIOReturnSuccess && sc_out[0] == 0);
    sc_in[0] = 300; sc_in[1] = 1;
    assert(rb.externalMethod(0x21, &a_set) == kIOReturnSuccess); /* touches slot without crashing */
    sc_in[0] = 200; assert(rb.externalMethod(0x22, &a_get) == kIOReturnSuccess && sc_out[0] == 0);
    puts("ok  test-annibale-perf3 IOGPU RunningBoard control contract");
    return 0;
}}
'''

c_harness = rf'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PERF_DOMAINS 2
#define MAX_LEVELS 32
#define TSENS_SENSORS 15
#define BUSY_TARGET 800
#define HOT_TENTHS 700
#define RELEASE_TENTHS 650
#define CUT_TENTHS 750
#define RISE_TICKS 4
#define NO_READING (-10000)
#define MIN(a,b) ((a)<(b)?(a):(b))
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define os_atomic_load(p, m) __atomic_load_n((p), __ATOMIC_RELAXED)
#define os_atomic_inc(p, m) __atomic_add_fetch((p), 1, __ATOMIC_RELAXED)
#define os_atomic_xchg(p, v, m) __atomic_exchange_n((p), (v), __ATOMIC_RELAXED)
#define os_atomic_cmpxchg(p, o, n, m) ({{ __typeof__(*(p)) _o = (o); __atomic_compare_exchange_n((p), &_o, (n), false, __ATOMIC_RELAXED, __ATOMIC_RELAXED); }})

#define WORK_INTERVAL_TYPE_COREAUDIO        (0x1 << 28)
#define WORK_INTERVAL_TYPE_CA_RENDER_SERVER (0x2 << 28)
#define WORK_INTERVAL_TYPE_CA_CLIENT        (0x3 << 28)
#define WORK_INTERVAL_TYPE_HID_DELIVERY     (0x4 << 28)
#define WORK_INTERVAL_TYPE_FRAME_COMPOSITOR (0x7 << 28)
#define WORK_INTERVAL_TYPE_MASK             (0xF0000000u)
#define SCHED_PERFCONTROL_CALLBACKS_VERSION_7 7
#define QUANTUM_EXPIRY 1
#define PERFCONTROL_CLASS_USER_INITIATED 9

typedef void *event_t;
typedef void *perfcontrol_state_t;
typedef int perfcontrol_event;
struct perfcontrol_thread_data {{ unsigned perfctl_class; }};
struct perfcontrol_work_interval {{ uint64_t start, finish, deadline; uint32_t create_flags; }};
typedef struct perfcontrol_work_interval *perfcontrol_work_interval_t;
struct sched_perfcontrol_callbacks {{
    int version;
    void (*work_interval_notify)(perfcontrol_state_t, perfcontrol_work_interval_t);
    void (*state_update)(perfcontrol_event, uint32_t, uint64_t, uint32_t, struct perfcontrol_thread_data *, uint64_t *);
}};

static uint32_t mode = 1;
static uint64_t mock_now;
static unsigned wakeups;
static uint64_t mach_absolute_time(void) {{ return mock_now; }}
static void thread_wakeup(event_t e) {{ (void)e; wakeups++; }}

{cpufreq_state}
{pd_struct}
{perf_want_fn}
{perfctl_block}

int main(void) {{
    wi_min_boost_abs = 4000000ULL;
    wi_max_boost_abs = 25000000ULL;
    wi_overrun_abs = 32000000ULL;
    wi_wake_gap_abs = 8000000ULL;
    governing = true;
    for (int d = 0; d < 2; d++) {{
        perf_domain[d].ours = true;
        perf_domain[d].known = true;
        perf_domain[d].count = 4;
        perf_domain[d].khz[0] = 1000000;
        perf_domain[d].khz[1] = 1800000;
        perf_domain[d].khz[2] = 2400000;
        perf_domain[d].khz[3] = 3100000;
        perf_domain[d].boost = 2;
        perf_domain[d].ceiling = 3;
        perf_domain[d].limit = 3;
        perf_domain[d].now = 0;
    }}

    mock_now = 100000000ULL;
    // Huge deadline gets clamped to now + 25 ms; triggers wakeup.
    annibale_perfctl_work_interval(6u, WORK_INTERVAL_TYPE_CA_RENDER_SERVER, mock_now, 0, mock_now + 500000000ULL);
    assert(wi_boost_until == mock_now + wi_max_boost_abs);
    assert(wakeups == 1);
    assert(annibale_perfctl_gpu_interactive_active(mock_now + 24000000ULL));
    assert(!annibale_perfctl_gpu_interactive_active(mock_now + 26000000ULL));

    // CoreAudio start does NOT boost GPU or cluster ceiling; overrun DOES trigger overrun window.
    uint64_t prev_boost = wi_boost_until;
    annibale_perfctl_work_interval(6u, WORK_INTERVAL_TYPE_COREAUDIO, mock_now, 0, mock_now + 10000000ULL);
    assert(wi_boost_until == prev_boost);

    // 75% budget consumed -> overrun window -> ceiling requested (3), clamped by thermal limit!
    mock_now += 10000000ULL;
    annibale_perfctl_work_interval(8u, WORK_INTERVAL_TYPE_CA_RENDER_SERVER, 100000000ULL, 109000000ULL, 112000000ULL);
    assert(wi_overrun_until == mock_now + wi_overrun_abs);
    assert(wakeups == 2); /* > 8 ms since first wake */
    assert(perf_want(0, 100, mock_now, 300000000ULL, true, 500, true, true) == 3);
    // Thermal CUT_TENTHS (750) overrides overrun ceiling boost down to limit 0.
    assert(perf_want(0, 100, mock_now, 300000000ULL, true, 760, true, true) == 0);

    // IRQ-off state_update tracks UI quantum expiries cleanly.
    struct perfcontrol_thread_data ui_thr = {{ PERFCONTROL_CLASS_USER_INITIATED }};
    perfctl_state_update(QUANTUM_EXPIRY, 0, mock_now, 0, &ui_thr, NULL);
    perfctl_state_update(QUANTUM_EXPIRY, 0, mock_now, 0, &ui_thr, NULL);
    assert(ui_quantum_expiries == 2);
    assert(annibale_perfctl_cbs.version == SCHED_PERFCONTROL_CALLBACKS_VERSION_7);
    assert(annibale_perfctl_cbs.work_interval_notify != NULL && annibale_perfctl_cbs.state_update != NULL);

    printf("ok  test-annibale-perf3 perfcontrol + cpufreq governor\\n");
    return 0;
}}
'''

with tempfile.TemporaryDirectory() as tmp:
    cpp_src = Path(tmp) / "test_iogpu.cpp"
    cpp_exe = Path(tmp) / "test_iogpu"
    cpp_src.write_text(cpp_harness)
    subprocess.run([
        "c++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(cpp_src), "-o", str(cpp_exe),
    ], check=True)
    subprocess.run([str(cpp_exe)], check=True)

    c_src = Path(tmp) / "test_perfctl.c"
    c_exe = Path(tmp) / "test_perfctl"
    c_src.write_text(c_harness)
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(c_src), "-o", str(c_exe),
    ], check=True)
    subprocess.run([str(c_exe)], check=True)

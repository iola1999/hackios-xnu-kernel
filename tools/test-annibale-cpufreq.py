#!/usr/bin/env python3
"""Run annibale's SCMI governor against a fake CPUCP, fake TSENS and a fake scheduler clock on the host."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
text = (ROOT / "xnu/files/bsd/dev/annibale_cpufreq.c").read_text()
block = text[text.index("#define SCMI_SHMEM_PHYS"):]

head = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef unsigned long vm_size_t;
typedef int boolean_t, wait_result_t, kern_return_t;
typedef void *thread_t, *event_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
typedef struct { uint32_t phys_id; } ml_topology_cpu_t;
typedef struct { unsigned int num_cpus; ml_topology_cpu_t cpus[2]; } ml_topology_info_t;
#define FALSE 0
#define KERN_SUCCESS 0
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL NULL
#define NSEC_PER_USEC 1000ULL
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define __builtin_arm_rsr64(name) 0x10100ULL
#define __builtin_arm_dsb(kind) ((void)0)
#define os_atomic_load(p, order) (*(p))
#define os_atomic_store(p, v, order) (*(p) = (v))
#define os_atomic_xchg(p, v, order) ({ __typeof__(*(p)) _old = *(p); *(p) = (v); _old; })

static const uint32_t device_khz[16] = { 1017600, 1209600, 1401600, 1689600, 1958400, 2246400, 2438400, 2649600,
    2841600, 3072000, 3283200, 3513600, 3801600, 4089600, 4204800, 4320000 };
static uint32_t shmem[0x80], doorbell[0xc0], tsens[3][0x80];
static uint32_t version, indexing, batch, sustained_khz, descending, level_key[2];
static uint32_t sets, set_domain[256], set_key[256], deny_set, never_free;
static uint32_t cfg_mode, cap_mhz, cap_given, threads, wakeups;
static uint64_t now_ns, idle_ns[2];
static uint32_t busy_now;               /* thousandths busy of each of our cores from here on */
static int32_t temp_now;                /* cluster 1, tenths; < -1000: invalid */
static unsigned ticks, stop_after;
static jmp_buf out_of_thread;
static thread_continue_t thread_fn;
static ml_topology_info_t topology = { 2, { { 0x10100 }, { 0x10000 } } };

#define LINES 64
static char lines[LINES][512];
static unsigned nlines;
#define last_line lines[(nlines - 1) % LINES]

static void loader_log_putc(char c) { (void)c; }
static int printf_stub(const char *format, ...)
{
    va_list list;
    va_start(list, format);
    vsnprintf(lines[nlines++ % LINES], sizeof(lines[0]), format, list);
    va_end(list);
    return 0;
}
#define printf printf_stub
static const ml_topology_info_t *ml_get_topology_info(void) { return &topology; }
static boolean_t ml_set_interrupts_enabled(boolean_t on) { (void)on; return 1; }
static uint64_t mach_absolute_time(void) { return now_ns++; }
static void absolutetime_to_nanoseconds(uint64_t a, uint64_t *n) { *n = a; }
static void nanoseconds_to_absolutetime(uint64_t n, uint64_t *a) { *a = n; }
static void thread_deallocate(thread_t t) { (void)t; }
static kern_return_t kernel_thread_start(thread_continue_t f, void *arg, thread_t *t) { (void)arg; thread_fn = f; *t = 0; threads++; return 0; }
static void thread_wakeup(event_t e) { (void)e; wakeups++; }
static wait_result_t assert_wait_timeout(event_t e, int i, uint32_t interval, uint64_t scale) { (void)e; (void)i; assert(interval * scale == 50000000ULL); return 0; }
static bool ml_cpu_idle_time(int cpu, uint64_t *idle) { *idle = idle_ns[cpu]; return true; }
static bool PE_parse_boot_argn(const char *name, void *value, int size)
{
    (void)size;
    if (!strcmp(name, "cpufreq") && cfg_mode) { *(uint32_t *)value = cfg_mode; return true; }
    if (!strcmp(name, "cpufreq_mhz") && cap_given) { *(uint32_t *)value = cap_mhz; return true; }
    return false;
}
static vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size)
{
    (void)size;
    switch (phys) {
    case 0x17b4e000: return (vm_offset_t)shmem;
    case 0x17830000: return (vm_offset_t)doorbell;
    case 0x0c228000: return (vm_offset_t)tsens[0];
    case 0x0c229000: return (vm_offset_t)tsens[1];
    case 0x0c22a000: return (vm_offset_t)tsens[2];
    }
    assert(0);
    return 0;
}

/* Per-sensor state of the fake TSENS: own[g][s] overrides the group's temperature for that sensor,
   blank[g] bit s clears its valid bit. Both survive the refresh in thread_block(). */
#define OWN_NONE 0x7fffffff
static int32_t own[3][0x18];
static uint32_t blank[3];

/* What cluster 1 is to read from a given tick on, so that one run() can hold a whole 30 s window. */
typedef struct { unsigned tick; int32_t tenths; uint32_t blank; } temp_step;
static temp_step script[8];
static unsigned nscript;

static void put_reading(unsigned group, unsigned sensor, int32_t tenths)
{
    tsens[group][(0xa0 + 4 * sensor) / 4] = tenths < -1000 ? 0 : (1u << 21) | ((uint32_t)tenths & 0xfff);
}
static void set_temperature(unsigned group, int32_t tenths)
{
    for (unsigned s = 0; s < 0x18; s++)
        put_reading(group, s, blank[group] >> s & 1 ? -2000 : own[group][s] == OWN_NONE ? tenths : own[group][s]);
}
static void thread_block(void *continuation)
{
    (void)continuation;
    if (++ticks > stop_after)
        longjmp(out_of_thread, 1);
    now_ns += 50000000ULL;
    for (int i = 0; i < 2; i++)
        idle_ns[i] += 50000000ULL * (1000 - busy_now) / 1000;
    for (unsigned i = 0; i < nscript; i++)
        if (script[i].tick == ticks) {
            temp_now = script[i].tenths;
            blank[1] = script[i].blank;
        }
    set_temperature(1, temp_now);
}

/* The platform: answers whatever is in the channel once the doorbell rang. */
static void firmware(void)
{
    uint32_t message = shmem[6] & 0xff, *in = &shmem[7], *out = &shmem[8], words = 0;
    int32_t status = 0;

    if (never_free || (shmem[1] & 1) || doorbell[0x41] == 0)
        return;
    doorbell[0x41] = 0;
    assert(((shmem[6] >> 10) & 0xff) == 0x13 && shmem[4] == 0);
    switch (message) {
    case 0x0: out[0] = version; words = 1; break;
    case 0x1: out[0] = 2; words = 4; break;
    case 0x3:
        out[0] = 0x6a000000 & (indexing ? ~0u : ~(1u << 25)); out[1] = 0; out[2] = sustained_khz; out[3] = 15;
        memcpy(&out[4], in[0] ? "NCC1\0\0\0\0\0\0\0\0\0\0\0\0" : "NCC0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
        words = 8; break;
    case 0x4: {
        uint32_t entry = version >= 0x40000 ? 5 : 3, first = in[1], n = MIN(batch, 16 - first);
        out[0] = n | (16 - first - n) << 16;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k = descending ? 15 - (first + i) : first + i;
            out[1 + i * entry] = indexing ? k * 1000 : device_khz[k];   /* (with indices: values that are no frequency) */
            out[2 + i * entry] = 7;
            if (entry == 5) { out[4 + i * entry] = device_khz[k]; out[5 + i * entry] = k; }
        }
        words = 1 + n * entry; break;
    }
    case 0x7: {
        uint32_t ok = 0;
        for (uint32_t k = 0; k < 16; k++) ok |= in[1] == (indexing ? k : device_khz[k]);
        if (deny_set || !ok) { status = deny_set ? -3 : -2; break; }
        set_domain[sets] = in[0]; set_key[sets++] = in[1]; level_key[in[0]] = in[1]; break;
    }
    case 0x8: out[0] = level_key[in[0]]; words = 1; break;
    default: status = -1;
    }
    shmem[7] = (uint32_t)status;
    shmem[5] = 8 + 4 * words;
    shmem[1] = 1;
}
static void delay_for_interval(uint32_t interval, uint64_t scale) { (void)interval; (void)scale; firmware(); }
'''

tail = r'''
/* The most recent line printed that starts with prefix, or "" when there is none. */
static const char *last_of(const char *prefix)
{
    for (unsigned k = 0; k < LINES && k < nlines; k++) {
        char *line = lines[(nlines - 1 - k) % LINES];
        if (!strncmp(line, prefix, strlen(prefix)))
            return line;
    }
    return "";
}

/* A fresh phone (the 4.0 firmware as it is on annibale unless a test changes it after), not yet started. */
static void reset(uint32_t m)
{
    memset(shmem, 0, sizeof(shmem));
    shmem[1] = 1;
    version = 0x40000; indexing = 1; batch = 8; sustained_khz = 4089600; descending = 0;
    level_key[0] = 8; level_key[1] = 0;
    sets = deny_set = never_free = threads = wakeups = ticks = nlines = 0;
    busy_now = 50; temp_now = 400; touch_at = 0; governing = false;
    cfg_mode = m;
    nscript = 0; reports = 0;
    memset(perf_domain, 0, sizeof(perf_domain));
    memset(tsens_seen, 0, sizeof(tsens_seen));
    memset(blank, 0, sizeof(blank));
    for (unsigned g = 0; g < 3; g++)
        for (unsigned s = 0; s < 0x18; s++)
            own[g][s] = OWN_NONE;
    for (unsigned g = 0; g < 3; g++) set_temperature(g, 400);
}

/* The governor for `n` more ticks of 50 ms. A fresh call re-enters the thread, so its locals
   (report_ns, level_since) start over: a 30 s window is only consistent within one run(). */
static void run(unsigned n)
{
    stop_after = ticks + n;
    if (!setjmp(out_of_thread))
        thread_fn(NULL, 0);
}

static unsigned now_mhz(void) { return perf_domain[1].khz[perf_domain[1].now] / 1000; }

int main(void)
{
    /* idle: stays where the boot loader left it; only our cluster is governed */
    reset(1);
    annibale_cpufreq_init();
    assert(threads == 1 && perf_domain[1].ours && !perf_domain[0].ours);
    assert(perf_domain[1].boot == 0 && perf_domain[1].ceiling == 9 && perf_domain[1].boost == 6);
    run(40);
    assert(sets == 0 && now_mhz() == 1017);

    /* busy: a quarter more than it used each tick, up to the ceiling (3072 MHz, the cpufreq_mhz default) */
    busy_now = 1000;
    run(1); assert(sets == 1 && set_domain[0] == 1 && set_key[0] == 2);
    run(1); assert(set_key[1] == 4);
    run(1); assert(set_key[2] == 7);
    run(1); assert(set_key[3] == 9);
    run(5); assert(sets == 4 && now_mhz() == 3072);

    /* idle again: only after 200 ms of wanting less, then straight down */
    busy_now = 50;
    run(4); assert(sets == 4);
    run(1); assert(sets == 5 && set_key[4] == 0);
    run(20); assert(sets == 5);

    /* a touch: woken at once, at least 2438 MHz until 300 ms after it; a second touch within that wakes nothing */
    annibale_cpufreq_touch();
    assert(wakeups == 1);
    run(1); assert(sets == 6 && set_key[5] == 6);
    now_ns += 1000000; annibale_cpufreq_touch();
    assert(wakeups == 1);
    run(9); assert(sets == 6);
    run(2); assert(sets == 7 && set_key[6] == 0);

    /* 70 C: slowest at once, busy or not; 65 C: stays; under 60 C: up again */
    busy_now = 1000;
    run(6); assert(now_mhz() == 3072);
    temp_now = 700;
    run(1); assert(now_mhz() == 1017 && perf_domain[1].hot && perf_domain[1].hot_entries == 1);
    temp_now = 650;
    run(5); assert(now_mhz() == 1017);
    temp_now = 599;
    run(1); assert(now_mhz() == 1401 && !perf_domain[1].hot);

    /* no reading of its own: the last one stands in for TSENS_STALE_MS, then the domain goes to its slowest */
    temp_now = -2000;
    run(1); assert(now_mhz() == 1958 && !perf_domain[1].hot);           /* (50 ms after the last 59.9 C) */
    run(2); assert(now_mhz() == 3072 && !perf_domain[1].hot);           /* (100, 150 ms: it still stands in) */
    run(3); assert(now_mhz() == 1017 && perf_domain[1].hot && perf_domain[1].hot_by_no_reading == 1);

    /* the 30 s report */
    temp_now = 450; busy_now = 300;
    run(700);
    assert(strstr(last_line, "cluster 1 45.0 C") != NULL);

    /* a window of its own, so its figures come from one run(): the cap goes on by temperature at tick
       100 and off 500 ms later; the readings disappear at tick 300, the cap goes on without one 250 ms
       after that, and the readings come back at tick 320 */
    reset(1);
    annibale_cpufreq_init();
    temp_now = 450; busy_now = 1000;
    script[0] = (temp_step){ 100, 720, 0 };
    script[1] = (temp_step){ 110, 450, 0 };
    script[2] = (temp_step){ 300, -2000, 0 };
    script[3] = (temp_step){ 320, 450, 0 };
    nscript = 4;
    run(700);
    assert(strstr(last_of("cpufreq: domain 1 in 30 s:"), "hottest 72.0 C") != NULL);
    assert(strstr(last_of("cpufreq: domain 1 window"),
        "window 1: coolest 45.0 C; 1 by temperature, 1 without a reading; "
        "600 ticks, sensor sets changed 2; largest step 27.0 C, 2 ticks stepped 5.0 C or more, "
        "6 of 6 sensors spread 10.0 C or more") != NULL);
    assert(strstr(last_of("cpufreq: domain 1 hot ticks"),
        "hot ticks 10 of 600, 500 ms of 30000 ms: 1017 MHz ") != NULL);
    /* every sensor of the group read the same, so each of them produced the maximum on those ticks,
       and each of them moved between the window's two temperatures */
    assert(strstr(last_of("cpufreq: domain 1 sensors"), " 1 45.0-72.0 C") != NULL);
    assert(strstr(last_of("cpufreq: domain 1 sensors"), " 6 45.0-72.0 C") != NULL);
    /* a report the 256-byte buffer cut would come out exactly 255 characters long */
    for (unsigned k = 0; k < LINES && k < nlines; k++)
        assert(strlen(lines[(nlines - 1 - k) % LINES]) < 255);

    /* the same cap reached by a ramp instead: six moves of 4.5 C, so no tick stepped 5.0 C or more */
    reset(1);
    annibale_cpufreq_init();
    temp_now = 450; busy_now = 1000;
    for (unsigned k = 0; k < 6; k++)
        script[k] = (temp_step){ 100 + 10 * k, 495 + 45 * k, 0 };
    nscript = 6;
    run(700);
    assert(strstr(last_of("cpufreq: domain 1 window"),
        "window 1: coolest 45.0 C; 1 by temperature, 0 without a reading; "
        "600 ticks, sensor sets changed 0; largest step 4.5 C, 0 ticks stepped 5.0 C or more, "
        "6 of 6 sensors spread 10.0 C or more") != NULL);
    assert(strstr(last_of("cpufreq: domain 1 hot ticks"), "hot ticks 451 of 600") != NULL);

    /* a hot sensor whose valid bit drops out: its reading still holds the cap for TSENS_STALE_MS */
    reset(1);
    annibale_cpufreq_init();
    temp_now = 450; busy_now = 1000; own[1][1] = own[1][2] = 720;       /* (two of cluster 1's sensors) */
    run(1); assert(now_mhz() == 1017 && perf_domain[1].hot && perf_domain[1].hot_by_temp == 1);
    blank[1] = 1u << 1 | 1u << 2;
    run(1); assert(now_mhz() == 1017 && perf_domain[1].hot && perf_domain[1].hottest == 720);
    run(2); assert(now_mhz() == 1017 && perf_domain[1].hot && perf_domain[1].hot_entries == 1);
    run(4); assert(!perf_domain[1].hot && now_mhz() >= 1401);           /* (out of date: the 45.0 C decides) */

    /* a valid bit that flaps: the set of sensors with a reading of their own changes, the level does not */
    reset(1);
    annibale_cpufreq_init();
    temp_now = 450; busy_now = 300;
    run(1);
    blank[1] = 1u << 1; run(1);
    blank[1] = 0; run(1);
    blank[1] = 1u << 1; run(1);
    assert(sets == 0 && !perf_domain[1].hot && perf_domain[1].hot_ticks == 0);
    assert(perf_domain[1].ticks == 4 && perf_domain[1].mask_changes == 3);

    /* cpufreq=2: reports only, whatever the load and the touches */
    reset(2);
    annibale_cpufreq_init();
    busy_now = 1000;
    annibale_cpufreq_touch();
    run(50);
    assert(sets == 0 && wakeups == 0);

    /* cpufreq=3: the ceiling at once, the slowest when hot */
    reset(3);
    annibale_cpufreq_init();
    run(1); assert(sets == 1 && now_mhz() == 3072);
    temp_now = 720;
    run(1); assert(now_mhz() == 1017);

    /* cpufreq_mhz=2000: ceiling 1958 MHz and the boost no higher; cpufreq_mhz=0: the sustained frequency */
    reset(1);
    cap_given = 1; cap_mhz = 2000;
    annibale_cpufreq_init();
    assert(perf_domain[1].ceiling == 4 && perf_domain[1].boost == 4);
    reset(1);
    cap_mhz = 0;
    annibale_cpufreq_init();
    assert(perf_domain[1].ceiling == 13 && perf_domain[1].boost == 6);
    cap_given = 0;

    /* a refused request: not asked again */
    reset(1);
    annibale_cpufreq_init();
    deny_set = 1; busy_now = 1000;
    run(10);
    assert(sets == 0 && perf_domain[1].refused);

    /* SCMI 3.x levels by value, slowest last, one per answer: sorted, asked for by value */
    reset(1);
    version = 0x30000; indexing = 0; batch = 1; descending = 1; level_key[1] = device_khz[0];
    annibale_cpufreq_init();
    assert(perf_domain[1].count == 16 && perf_domain[1].khz[0] == 1017600 && perf_domain[1].ceiling == 9);
    busy_now = 1000;
    run(1);
    assert(sets == 1 && set_key[0] == device_khz[2]);

    /* a channel that never comes free: nothing set, no thread */
    reset(1);
    shmem[1] = 0; never_free = 1;
    annibale_cpufreq_init();
    assert(sets == 0 && threads == 0 && !perf_domain[1].known);

    puts("annibale cpufreq: ok");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "t.c"
    exe = Path(tmp) / "t"
    c.write_text(head + block + tail)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wno-unused-function", "-o", str(exe), str(c)], check=True)
    subprocess.run([str(exe)], check=True)

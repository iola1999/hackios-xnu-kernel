#!/usr/bin/env python3
"""
Test AudioReach APM Routing, Cold Calibration (150ms timeout),
MI2S1 BCLK Stabilization (4 zero-PCM periods), and Feeder Thread Survival.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[2]
REACH_SOURCE = Path(os.environ.get('AUDIO_REACH_SOURCE', ROOT / 'xnu/files/bsd/dev/annibale/annibale_audio_reach.c'))
AMP_SOURCE = Path(os.environ.get('AUDIO_AMP_SOURCE', ROOT / 'xnu/files/bsd/dev/annibale/annibale_audio_amp.c'))

reach_text = REACH_SOURCE.read_text()
amp_text = AMP_SOURCE.read_text()


def extract_function(source, name):
    match = re.search(r'(?:static )?(?:bool|void|uint32_t)\s+' + name + r'\([^;]*?\)\s*\{', source)
    assert match, f"Function {name} not found"
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


# -----------------------------------------------------------------------------
# Test 1: Amp Cold Calibration Timeout (150ms)
# -----------------------------------------------------------------------------
AMP_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TFA6X_REG_SYS_CONTROL0    0x00
#define TFA6X_REG_SYS_CONTROL1    0x01
#define TFA6X_REG_STATUS_FLAGS0   0x10
#define TFA6X_REG_STATUS_FLAGS1   0x11
#define TFA6X_REG_SYS_STATUS0     0x12
#define TFA6X_REG_AUDIO_CTRL      0x51
#define TFA6X_REG_MTP0            0x80
#define TFA6X_REG_SWPROFIL        0x04
#define TFA6X_REG_HIDDEN_KEY      0x0f

#define TFA6X_BF_PWDN             0x0001
#define TFA6X_BF_MANSCONF         0x0140
#define TFA6X_BF_AMPE             0x0300
#define TFA6X_BF_SWPROFIL         0x0400
#define TFA6X_BF_SWVSTEP          0x0410

#define NSEC_PER_MSEC             1000000

static bool current_muted = false;
static uint16_t tfa_ext_rev[2] = { 0x3a64u, 0x3a64u };
static unsigned poll_iterations = 0;
static unsigned ready_at_iteration = 0;

static void say(const char *fmt, ...) { (void)fmt; }
static void delay_for_interval(int interval, int unit) {
    (void)interval; (void)unit;
    poll_iterations++;
}

static bool amp_reg_read16(uint8_t addr, uint8_t reg, uint16_t *val) {
    (void)addr;
    if (reg == TFA6X_REG_SWPROFIL) { *val = 1u; return true; }
    if (reg == TFA6X_REG_MTP0) { *val = 0; return true; }
    if (reg == TFA6X_REG_STATUS_FLAGS1) {
        /* TFA9866 reports flg1=0x0881 (CLKS=1, NOCLK=0, bits 9/10 unset) in operating state */
        *val = 0x0881u;
        return true;
    }
    if (reg == TFA6X_REG_SYS_STATUS0) {
        if (poll_iterations >= ready_at_iteration) {
            *val = 0x1709u; /* MANSTATE == 9 (Operating), bit 13 clear */
        } else {
            *val = 0x1704u; /* MANSTATE == 4 (ramping), not yet operating */
        }
        return true;
    }
    *val = 0;
    return true;
}

static bool amp_reg_write16(uint8_t addr, uint8_t reg, uint16_t val) {
    (void)addr; (void)reg; (void)val;
    return true;
}

static bool tfa_set_bf(uint8_t addr, uint16_t bf, uint16_t val) {
    (void)addr; (void)bf; (void)val;
    return true;
}

static bool tfa986x_cold_startup_one(uint8_t addr, unsigned ch, uint32_t cal_mohm) {
    (void)addr; (void)ch; (void)cal_mohm;
    return true;
}

static void tfa986x_output_readback(uint8_t addr, unsigned ch) {
    (void)addr; (void)ch;
}

AMP_FUNCTION

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *mode = argv[1];

    if (!strcmp(mode, "fast-ready")) {
        ready_at_iteration = 10;
        poll_iterations = 0;
        bool ok = tfa986x_start_operating_one(0x36, 0, 7500);
        assert(ok);
        assert(poll_iterations == 10);
        printf("PASS: amp ready at 10ms (iterations=%u)\n", poll_iterations);
    } else if (!strcmp(mode, "slow-ready-50ms")) {
        /* Old 25ms timeout would fail this; 150ms timeout must succeed */
        ready_at_iteration = 50;
        poll_iterations = 0;
        bool ok = tfa986x_start_operating_one(0x36, 0, 7500);
        assert(ok);
        assert(poll_iterations == 50);
        printf("PASS: amp ready at 50ms (relaxed 150ms timeout succeeded, iterations=%u)\n", poll_iterations);
    } else if (!strcmp(mode, "timeout-150ms")) {
        ready_at_iteration = 999;
        poll_iterations = 0;
        bool ok = tfa986x_start_operating_one(0x36, 0, 7500);
        assert(!ok);
        assert(poll_iterations == 150);
        printf("PASS: amp timed out after exactly 150 iterations\n");
    } else {
        return 1;
    }
    return 0;
}
'''

# -----------------------------------------------------------------------------
# Test 2: BCLK Stabilization (4 Zero-PCM Periods) in build_audioreach_graphs
# -----------------------------------------------------------------------------
GRAPH_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define AUDIOREACH_ROUTING_TEST 1
#define RATE 48000u
#define CHANNELS 2u
#define PERIOD_FRAMES 960u
#define PERIOD_BYTES (PERIOD_FRAMES * CHANNELS * 2u)
#define BUFFER_BYTES 0x10000u

#define say(...) ((void)0)
#ifndef __DECONST
#define __DECONST(t, v) ((t)(uintptr_t)(const void *)(v))
#endif

static uint8_t mock_reach_buffer[BUFFER_BYTES];
static volatile uint8_t *reach_buffer = mock_reach_buffer;
static uint32_t zero_periods_sent = 0;
static bool amp_powered_history[4];
static unsigned amp_power_calls = 0;

static bool annibale_audio_amp_power(bool on) {
    if (amp_power_calls < 4) {
        amp_powered_history[amp_power_calls++] = on;
    }
    return true;
}

static bool send_audio_period(uint32_t slot) {
    assert(slot < 4);
    assert(zero_periods_sent == slot);
    /* Verify buffer content is all zero (muted) */
    for (uint32_t i = 0; i < PERIOD_BYTES; i++) {
        assert(reach_buffer[slot * PERIOD_BYTES + i] == 0);
    }
    zero_periods_sent++;
    return true;
}

struct gpr_pkt { uint8_t buf[1024]; uint32_t len; };
struct reach_module { uint32_t module, iid, inputs, outputs; };
struct reach_link { uint32_t from, to; };
static uint32_t gpr_token = 0, map_handle = 0x4321;
static uint64_t dsp_address = 0x1cf66f000ULL;
static unsigned commands = 0;

static bool annibale_audio_reach_send_cmd(const void *raw, uint32_t len, const char *descr,
    uint32_t *op, uint32_t *words, uint32_t count) {
    (void)descr; (void)count;
    const uint32_t *p = raw;
    assert((p[0] >> 8) == len);
    commands++;
    *op = 0x02001005u; /* GPR_BASIC_RSP_RESULT */
    if (p[5] == 0x01001021u) { /* APM_CMD_GET_SPF_STATE */
        *op = 0x02001007u; words[0] = 1;
    } else if (p[5] == 0x0100100cu) { /* OP_MEM_MAP */
        *op = 0x02001001u; words[0] = map_handle;
    }
    return true;
}

static void gpr_begin(struct gpr_pkt *p, uint32_t src, uint32_t dst, uint32_t opcode);
static uint32_t gpr_finish(struct gpr_pkt *p);
static bool wait_for_spf_ready(struct gpr_pkt *p) {
    gpr_begin(p, 0x1000u, 0x1001u, 0x01001021u);
    gpr_finish(p);
    uint32_t rop = 0, rwords[2] = { 0 };
    return annibale_audio_reach_send_cmd(p->buf, p->len, "GET_SPF_STATE", &rop, rwords, 2);
}

GRAPH_FUNCTIONS

int main(void) {
    /* Fill buffer with non-zero noise to ensure bzero actually clears it */
    memset(mock_reach_buffer, 0xaa, sizeof(mock_reach_buffer));
    commands = 0;
    zero_periods_sent = 0;
    amp_power_calls = 0;

    bool ok = build_audioreach_graphs();
    assert(ok);
    assert(commands == 11);
    /* Verify exactly 4 zero-PCM periods were sent */
    assert(zero_periods_sent == 4);
    /* Verify amp power sequence: power(true) then power(false) */
    assert(amp_power_calls == 2);
    assert(amp_powered_history[0] == true);
    assert(amp_powered_history[1] == false);

    printf("PASS: build_audioreach_graphs BCLK 4-period zero stabilization & calibration\n");
    return 0;
}
'''

# -----------------------------------------------------------------------------
# Test 3: Feeder Warmup & Thread Survival (No Suicide on Amp Startup Failure)
# -----------------------------------------------------------------------------
FEEDER_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#define PERIOD_FRAMES 960
#define QUIET_PERIODS 250
#define SLOTS 8
#define IN_FLIGHT 4
#define RING_FRAMES 32768u
#define RING_MASK (RING_FRAMES-1u)
#define NSEC_PER_MSEC 1000000
#define ANNIBALE_GUARD_SILENT 999
#define THREAD_UNINT 0
#define THREAD_AWAKENED 0
#define THREAD_CONTINUE_NULL 0
#define audio_spin_lock ((void *)1)
#ifndef __DECONST
#define __DECONST(t, v) ((t)(uintptr_t)(const void *)(v))
#endif

typedef int wait_result_t;
typedef void *event_t;

static uint32_t ring_head = 960, ring_tail = 0;
static uint32_t in_flight_mask = 0, reach_sent_count = 0, reach_done_count = 0;
static uint32_t reach_sync_mode = 0;
static bool reach_write_failed = false, reach_stream_active = false, locked = false;

#define CHANNELS 2u
#define PERIOD_BYTES (PERIOD_FRAMES * CHANNELS * 2u)
static int16_t ring_data[2 * RING_FRAMES];
static int16_t *ring_storage = ring_data;
static void clear_wait(void *thread, int reason) { (void)thread; (void)reason; }
static void thread_wakeup(event_t e) { (void)e; }
static uint8_t mock_buffer[0x10000];
static volatile uint8_t *reach_buffer = mock_buffer;

static unsigned amp_on_attempts = 0, amp_off_calls = 0;
static bool sim_amp_fail_start = false;
static bool sim_loud = true;
static unsigned iterations = 0;
static jmp_buf finished;

static void lck_spin_lock(void *p) { (void)p; assert(!locked); locked = true; }
static void lck_spin_unlock(void *p) { (void)p; assert(locked); locked = false; }
static void say(const char *format, ...) { (void)format; }
static bool annibale_adsp_gpr_ready(void) { return true; }
static bool build_audioreach_graphs(void) { return true; }
static void delay_for_interval(int a, int b) { (void)a; (void)b; }
static void *current_thread(void) { return NULL; }

static void thread_terminate(void *p) {
    (void)p;
    /* HARD CHECK: Feeder thread must NEVER call thread_terminate on amp startup failure! */
    if (sim_amp_fail_start) {
        fprintf(stderr, "FATAL REGRESSION: thread_terminate called on amp startup failure!\n");
        assert(false);
    }
    longjmp(finished, 1);
}

static bool annibale_audio_volume(uint32_t *a, bool *b, uint32_t *c) { (void)a; (void)b; (void)c; return false; }
static void annibale_guard_attenuation(uint32_t a) { (void)a; }
static void annibale_audio_amp_set_volume(uint32_t a, bool b) { (void)a; (void)b; }

static bool annibale_audio_amp_power(bool on) {
    if (on) {
        amp_on_attempts++;
        if (sim_amp_fail_start) return false;
        return true;
    }
    amp_off_calls++;
    return true;
}

static bool fill_period_slot(uint32_t slot, bool muted) {
    (void)slot; (void)muted;
    if (ring_tail != ring_head) {
        ring_tail = ring_head;
        return sim_loud;
    }
    return false;
}

static bool queue_audio_period_async(uint32_t slot) {
    in_flight_mask |= (1u << slot);
    reach_sent_count++;
    return true;
}

static bool send_audio_period(uint32_t slot) {
    (void)slot;
    reach_sent_count++;
    reach_done_count++;
    return true;
}

static void clock_interval_to_deadline(uint32_t ms, int scale, uint64_t *out) { (void)ms; (void)scale; *out = 0; }
static void assert_wait_deadline(event_t e, int mode, uint64_t deadline) { (void)e; (void)mode; (void)deadline; }
static void thread_block(int mode) {
    (void)mode;
    iterations++;
    reach_done_count += (unsigned)__builtin_popcount(in_flight_mask);
    in_flight_mask = 0;

    /* In startup failure test, stop after 5 iterations and assert thread stayed alive */
    if (sim_amp_fail_start && iterations >= 5) {
        longjmp(finished, 1);
    }
    /* In normal loud test, simulate draining */
    if (!sim_amp_fail_start && iterations >= 300) {
        longjmp(finished, 1);
    }
}

FEEDER_FUNCTIONS

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *case_name = argv[1];

    if (!strcmp(case_name, "test-warmup-and-poweron")) {
        sim_amp_fail_start = false;
        sim_loud = true;
        ring_head = 960; ring_tail = 0;
        amp_on_attempts = 0;
        iterations = 0;
        if (!setjmp(finished)) {
            reach_feeder_thread(NULL, 0);
        }
        assert(amp_on_attempts >= 1);
        printf("PASS: feeder delivered warmup periods and powered on amplifier\n");
    } else if (!strcmp(case_name, "test-amp-failure-survival")) {
        sim_amp_fail_start = true;
        sim_loud = true;
        ring_head = 960; ring_tail = 0;
        amp_on_attempts = 0;
        iterations = 0;
        reach_write_failed = false;

        if (!setjmp(finished)) {
            reach_feeder_thread(NULL, 0);
        }
        /* Verified: feeder survived 5 iterations despite amp failures, did not terminate */
        assert(amp_on_attempts >= 1);
        assert(!reach_write_failed);
        /* Verify sink still accepts audio! */
        const int16_t sample_frames[2] = { 100, -100 };
        uint32_t accepted = reach_audio_sink(sample_frames, 1);
        assert(accepted == 1);
        printf("PASS: feeder thread survived amp startup failure, no thread_terminate, sink active\n");
    } else {
        return 1;
    }
    return 0;
}
'''


def run_amp_test():
    amp_func = extract_function(amp_text, 'tfa986x_start_operating_one')
    src = AMP_HARNESS.replace('AMP_FUNCTION', amp_func)
    with tempfile.TemporaryDirectory(prefix='amp-test-') as td:
        c_path = Path(td) / 'test.c'
        bin_path = Path(td) / 'test'
        c_path.write_text(src)
        subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(c_path), '-o', str(bin_path)], check=True)
        for mode in ['fast-ready', 'slow-ready-50ms', 'timeout-150ms']:
            res = subprocess.run([str(bin_path), mode], capture_output=True, text=True, check=True)
            print(res.stdout.strip())


def run_graph_test():
    funcs = [
        extract_function(reach_text, 'gpr_put8'),
        extract_function(reach_text, 'gpr_put16'),
        extract_function(reach_text, 'gpr_put32'),
        extract_function(reach_text, 'gpr_set32'),
        extract_function(reach_text, 'gpr_pad_to'),
        extract_function(reach_text, 'gpr_begin'),
        extract_function(reach_text, 'gpr_finish'),
        extract_function(reach_text, 'gpr_cmd_begin'),
        extract_function(reach_text, 'gpr_cmd_end'),
        extract_function(reach_text, 'gpr_param_begin'),
        extract_function(reach_text, 'gpr_param_end'),
        extract_function(reach_text, 'gpr_graph_open'),
        extract_function(reach_text, 'gpr_send_command'),
        extract_function(reach_text, 'gpr_graph_cmd'),
        extract_function(reach_text, 'build_audioreach_graphs'),
    ]
    constants = '\n'.join(line for line in reach_text.splitlines() if line.startswith('#define ') and
                          re.match(r'#define \w+\s+(?:0x[0-9a-fA-F]+|[0-9]+)u?\s*(?:/\*.*)?$', line))
    src = constants + '\n' + GRAPH_HARNESS.replace('GRAPH_FUNCTIONS', ''.join(funcs))
    with tempfile.TemporaryDirectory(prefix='graph-test-') as td:
        c_path = Path(td) / 'test.c'
        bin_path = Path(td) / 'test'
        c_path.write_text(src)
        subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(c_path), '-o', str(bin_path)], check=True)
        res = subprocess.run([str(bin_path)], capture_output=True, text=True, check=True)
        print(res.stdout.strip())


def run_feeder_test():
    funcs = [
        extract_function(reach_text, 'reach_feeder_thread'),
        extract_function(reach_text, 'reach_audio_sink'),
        extract_function(reach_text, 'annibale_audio_reach_is_active'),
    ]
    src = FEEDER_HARNESS.replace('FEEDER_FUNCTIONS', ''.join(funcs))
    with tempfile.TemporaryDirectory(prefix='feeder-test-') as td:
        c_path = Path(td) / 'test.c'
        bin_path = Path(td) / 'test'
        c_path.write_text(src)
        subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(c_path), '-o', str(bin_path)], check=True)
        for mode in ['test-warmup-and-poweron', 'test-amp-failure-survival']:
            res = subprocess.run([str(bin_path), mode], capture_output=True, text=True, check=True)
            print(res.stdout.strip())


def main():
    print("=== Running AudioReach Routing & Timing Host Unit Tests ===")
    run_amp_test()
    run_graph_test()
    run_feeder_test()
    print("ALL AUDIOREACH ROUTING & TIMING TESTS PASSED (100%)")


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Host ASan+UBSan tests for async 4-slot mic capture, lifecycle recovery, and IO_NDELAY reads."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MIC = (ROOT / "xnu/files/bsd/dev/annibale/annibale_mic.c").read_text()
REACH = (ROOT / "xnu/files/bsd/dev/annibale/annibale_audio_reach.c").read_text()


def extract_function(source, name):
    match = re.search(r"(?:static\s+)?(?:bool|void|uint32_t|int)\s+" + name + r"\([^;]*?\)\s*\{", source)
    assert match, f"missing {name}"
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos] + "\n"


HEADER = r'''
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __dead2
#define __dead2
#endif
#ifndef __DEVOLATILE
#define __DEVOLATILE(type, var) ((type)(uintptr_t)(volatile void *)(var))
#endif
#define __builtin_arm_dsb(x) ((void)0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define say(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define NSEC_PER_SEC 1000000000u
#define NSEC_PER_MSEC 1000000u
#define NSEC_PER_USEC 1000u
#define IO_NDELAY 1
#define BUFFER_BYTES 65536u
#define CMD_OFFSET 0xc000u
#define APM_CMD_GET_SPF_STATE 0x01001021u
#define SLOTS 8u
#define PERIOD_BYTES 3840u
#define OUR_PORT 0x2001u
#define IID_PLAY_SHMEM 0x6011u
#define OP_WRITE_DONE 0x05001002u

struct proc { int unused; };
#ifndef __APPLE__
typedef intptr_t user_ssize_t;
#endif
typedef uintptr_t vm_offset_t;
typedef int wait_result_t;
typedef void *event_t;
typedef void *thread_t;
#define THREAD_INTERRUPTIBLE 1
#define THREAD_AWAKENED 0
#define THREAD_CONTINUE_NULL NULL

struct uio { int16_t data[4096]; user_ssize_t remaining, offset; };
static user_ssize_t uio_resid(struct uio *u) { return u->remaining; }
static unsigned moves;
static int uiomove(char *src, int len, struct uio *u) {
    assert(len >= 0 && (size_t)(u->offset + len) <= sizeof(u->data));
    memcpy((char *)u->data + u->offset, src, (size_t)len);
    u->offset += len;
    u->remaining -= len;
    moves++;
    return 0;
}

static uint64_t mock_time_ns = 1000000000ULL;
static uint64_t mach_absolute_time(void) { return mock_time_ns; }
static void absolutetime_to_nanoseconds(uint64_t a, uint64_t *n) { *n = a; }

static bool thread_test;
static int thread_stop_mode;
static jmp_buf thread_exit;
static void delay_for_interval(uint32_t n, uint32_t unit) {
    (void)n; (void)unit;
    if (thread_test) longjmp(thread_exit, 1);
}
static unsigned idle_waits, wakeups;
static int assert_wait_timeout(event_t e, int interruptible, uint32_t interval, uint32_t scale) {
    (void)e; (void)interruptible; (void)interval; (void)scale;
    idle_waits++;
    return 0;
}
static thread_t current_thread(void) { return NULL; }
static int clear_wait(thread_t t, int result) { (void)t; (void)result; return 0; }
static int thread_block(void *continuation) {
    (void)continuation;
    if (thread_test && thread_stop_mode > 0) longjmp(thread_exit, thread_stop_mode);
    return 0;
}
static void thread_wakeup(event_t e) { (void)e; wakeups++; }

static int amic_lock, cmd_lock, spin, audio_spin;
static int *amic_ring_lock = &spin, *cmd_oob_lock = &cmd_lock, *audio_spin_lock = &audio_spin;
static int locked, spin_locked;
static void lck_mtx_lock(void *p) { (void)p; locked++; }
static void lck_mtx_unlock(void *p) { (void)p; assert(locked > 0); locked--; }
static void lck_spin_lock(void *p) { (void)p; spin_locked++; }
static void lck_spin_unlock(void *p) { (void)p; assert(spin_locked > 0); spin_locked--; }

static uint32_t tlmm_memory[0x150000 / 4], lpi_memory[0x30000 / 4], va_memory[0x2000 / 4];
static vm_offset_t tlmm_base;
static bool hw_up = true, reach_active = true;
static unsigned map_calls, fail_map, commands, fail_command;
static unsigned async_sends, fail_async_send_at, graph_stops, graph_opens, graph_prepares;
static uint32_t stopped_sgs[8];
static uint32_t queued_slots[16];
static uint32_t queued_slot_count;
static uint32_t in_flight_mask, reach_done_count, reach_sent_count;
static bool reach_write_failed;

static bool annibale_adsp_is_hw_up(void) { return hw_up; }
static bool annibale_audio_reach_is_active(void) { return reach_active; }
static vm_offset_t ml_io_map(uintptr_t at, uintptr_t size) {
    (void)size;
    if (++map_calls == fail_map) return 0;
    if (at == 0x0f100000UL) return (uintptr_t)tlmm_memory;
    if (at == 0x07660000UL) return (uintptr_t)va_memory;
    return (uintptr_t)lpi_memory;
}

static int16_t storage[65536], *mic_ring = storage;
static uint32_t mic_ring_head, mic_ring_tail, mic_open_clients, mic_token = 500;
static uint32_t mic_graphs_opened;
static bool mic_graphs_started, mic_hw_prepared;
static int mic_capture_error;
static uint64_t last_blocking_read_ns;
static volatile uint32_t mic_inflight_mask;
static uint32_t mic_slot_map_handle;
static uint64_t mic_slot_dsp_base;
static volatile uint8_t *mic_slot_dma_va;
static volatile bool mic_async_failed;
static uint32_t mic_sent_count, mic_done_count, mic_last_bytes, mic_peak;

static bool recording = false, dma_available = true;
static uint32_t fed_frames;
static int16_t fed[1920];
static bool annibale_audio_recording_mic(void) { return recording; }
static void annibale_audio_record_fed(const int16_t *samples, uint32_t frames) {
    assert(frames <= 960);
    memcpy(fed, samples, (size_t)frames * 4u);
    fed_frames = frames;
}

struct gpr_pkt { uint8_t buf[1024]; uint32_t len; };
static uint8_t dma_buffer[65536];
static volatile uint8_t *reach_buffer = dma_buffer;
static uint32_t map_handle = 0x4321u;
static uint64_t dsp_address = 0x1cf66f000ULL;

static bool annibale_audio_reach_dma_info(volatile uint8_t **va, uint64_t *pa, uint64_t *dsp, uint32_t *handle) {
    *va = dma_buffer;
    *pa = 0xcf66f000ULL;
    *dsp = dsp_address;
    *handle = map_handle;
    return dma_available;
}

static bool annibale_adsp_gpr_send(const void *raw, uint32_t len, uint32_t wait,
    uint32_t *op, uint32_t *words, uint32_t count) {
    const uint32_t *p = raw;
    if (wait == 0) {
        assert(len == 56u && (p[0] >> 8) == len && p[2] == 0x2002u && p[3] == 0x6021u);
        assert(p[5] == 0x0400100bu && p[9] == 1920u);
        async_sends++;
        if (async_sends == fail_async_send_at) return false;
        uint64_t addr = ((uint64_t)p[7] << 32) | p[6];
        uint64_t cap_base = dsp_address + 0x8000u;
        assert(addr >= cap_base && (addr - cap_base) % 1920u == 0);
        if (queued_slot_count < 16) {
            queued_slots[queued_slot_count++] = (uint32_t)((addr - cap_base) / 1920u);
        }
        return true;
    }
    (void)count;
    commands++;
    if (commands == fail_command) return false;
    uint32_t pkt_op = p[5];
    if (pkt_op == 0x0100100fu) {
        assert(p[2] == 0x2002u && p[3] == 2u);
        assert(p[10] == 2u);
        if (p[11] == 0x08001032u) {
            assert(p[12] == 4u && p[13] == 0u && p[14] == 1u);
        } else if (p[11] == 0x0800102cu) {
            assert(p[12] == 20u && p[13] == 0u && p[14] == 1u &&
                   p[16] == 19200000u && p[17] == 1u && p[18] == 0u);
        } else {
            assert(0 && "unexpected PRM param_id");
        }
        if (op) *op = 0x02001002u;
        if (words && count >= 2) { words[0] = pkt_op; words[1] = 0; }
        return true;
    }
    if (pkt_op == 0x01001000u) graph_opens++;
    if (pkt_op == 0x01001001u) graph_prepares++;
    if (pkt_op == 0x01001003u) {
        if (graph_stops < 8) stopped_sgs[graph_stops] = p[15];
        graph_stops++;
    }
    if (op) *op = 0x02001005u;
    if (words && count >= 2) { words[0] = 0; words[1] = 0; }
    return true;
}

bool annibale_mic_gpr_event(uint32_t src_port, uint32_t dst_port, uint32_t opcode,
    const void *payload, uint32_t len);
'''

MAIN = r'''
static void reset_state(void) {
    assert(locked == 0 && spin_locked == 0);
    thread_test = false;
    thread_stop_mode = 2;
    tlmm_base = 0;
    hw_up = true;
    reach_active = true;
    map_calls = fail_map = commands = fail_command = 0;
    async_sends = fail_async_send_at = graph_stops = graph_opens = graph_prepares = 0;
    queued_slot_count = 0;
    in_flight_mask = reach_done_count = reach_sent_count = 0;
    reach_write_failed = false;
    mic_hw_prepared = mic_graphs_started = false;
    mic_graphs_opened = 0;
    memset(tlmm_memory, 0, sizeof(tlmm_memory));
    memset(lpi_memory, 0, sizeof(lpi_memory));
    memset(va_memory, 0, sizeof(va_memory));
    mic_capture_error = 0;
    mic_open_clients = 0;
    mic_inflight_mask = 0;
    mic_slot_map_handle = 0;
    mic_slot_dsp_base = 0;
    mic_slot_dma_va = NULL;
    mic_async_failed = false;
    mic_sent_count = mic_done_count = mic_last_bytes = mic_peak = 0;
    last_blocking_read_ns = 0;
    idle_waits = wakeups = 0;
    mic_ring = storage;
    mic_ring_head = mic_ring_tail = 0;
    moves = fed_frames = 0;
    recording = false;
    dma_available = true;
    memset(storage, 0x55, sizeof(storage));
    memset(dma_buffer, 0, sizeof(dma_buffer));
}

static int run_thread_once(int stop_mode) {
    thread_test = true;
    thread_stop_mode = stop_mode;
    int r = setjmp(thread_exit);
    if (r == 0) mic_capture_thread(NULL, 0);
    thread_test = false;
    assert(locked == 0 && spin_locked == 0);
    return r;
}

static void fill_done_payload(uint32_t words[14], uint32_t slot, uint32_t bytes) {
    memset(words, 0, 56);
    uint64_t addr = dsp_address + MIC_OFFSET + (uint64_t)slot * MIC_PERIOD_BYTES;
    words[0] = 0;
    words[1] = (uint32_t)addr;
    words[2] = (uint32_t)(addr >> 32);
    words[3] = map_handle;
    words[4] = bytes;
    words[8] = 1;
}

int main(void) {
    /* Boot race gate: do not send PRM/APM commands before AudioReach is active */
    reset_state();
    reach_active = false;
    assert(!mic_hw_prepare() && !mic_hw_prepared && commands == 0);

    /* 1. 4-slot async queuing + reach_gpr_event demux */
    reset_state();
    assert(mic_cdev_open(0, 0, 0, NULL) == 0 && mic_open_clients == 1 && mic_graphs_started);
    assert(va_memory[0x0000 / 4] == 1u && va_memory[0x0084 / 4] == 0x0bu && va_memory[0x0100 / 4] == 0x10u);
    assert((tlmm_memory[(183u * 0x1000u) / 4 + 1] & 2u) != 0);
    assert(graph_opens == 2 && graph_prepares == 2);
    uint32_t cmds_before = commands;
    assert(run_thread_once(2) == 2);
    assert(commands == cmds_before); /* zero sync OOB commands during async capture */
    assert(async_sends == 4 && mic_sent_count == 4 && mic_inflight_mask == 0x0fu);
    assert(queued_slots[0] == 0 && queued_slots[1] == 1 && queued_slots[2] == 2 && queued_slots[3] == 3);

    /* Playback completion still handled by reach_gpr_event without affecting mic */
    uint32_t play_words[8] = { (uint32_t)dsp_address, (uint32_t)(dsp_address >> 32), map_handle, 0, 0, 0, 0, 0 };
    in_flight_mask = 1u;
    assert(reach_gpr_event(IID_PLAY_SHMEM, OUR_PORT, OP_WRITE_DONE, 0, play_words, sizeof(play_words)));
    assert(in_flight_mask == 0 && reach_done_count == 1 && mic_done_count == 0);
    assert(!reach_gpr_event(0x9999u, OUR_MIC_PORT, OP_READ_BUF_DONE, 0, play_words, sizeof(play_words)));

    /* Capture completion on slot 2 feeds ring and clears only bit 2 */
    int16_t *slot2_va = (int16_t *)(dma_buffer + MIC_OFFSET + 2u * MIC_PERIOD_BYTES);
    slot2_va[0] = 321; slot2_va[1] = -654; slot2_va[2] = 987;
    recording = true;
    uint32_t cap_words[14];
    fill_done_payload(cap_words, 2, 6);
    assert(reach_gpr_event(IID_CAP_SHMEM, OUR_MIC_PORT, OP_READ_BUF_DONE, 0, cap_words, sizeof(cap_words)));
    assert(mic_inflight_mask == 0x0bu && mic_done_count == 1 && !mic_async_failed);
    assert((mic_ring_head - mic_ring_tail) == 3u && storage[0] == 321 && storage[1] == -654 && storage[2] == 987);
    assert(fed_frames == 3u && fed[0] == 321 && fed[1] == 321 && fed[4] == 987 && fed[5] == 987);

    /* Next feeder pass refills slot 2 cleanly without disturbing slots 0, 1, 3 */
    assert(run_thread_once(2) == 2);
    assert(async_sends == 5 && queued_slots[4] == 2 && mic_inflight_mask == 0x0fu);

    /* 2. IO_NDELAY short reads without zero-padding */
    struct uio u = { .remaining = 16 };
    memset(u.data, 0x77, sizeof(u.data));
    assert(annibale_mic_read(&u, IO_NDELAY) == 0);
    assert(u.remaining == 10 && u.offset == 6);
    assert(u.data[0] == 321 && u.data[1] == -654 && u.data[2] == 987 && u.data[3] == 0x7777);

    /* Empty ring + IO_NDELAY -> EAGAIN, zero uiomove calls */
    unsigned moves_before = moves;
    u = (struct uio){ .remaining = 16 };
    assert(annibale_mic_read(&u, IO_NDELAY) == EAGAIN && moves == moves_before && u.remaining == 16);

    /* Multi-chunk IO_NDELAY read (> 1024 samples) returns exact available samples */
    for (uint32_t i = 0; i < 1200u; i++) {
        storage[mic_ring_head & ((65536u) - 1u)] = (int16_t)(i + 1);
        mic_ring_head++;
    }
    u = (struct uio){ .remaining = 3000 };
    assert(annibale_mic_read(&u, IO_NDELAY) == 0);
    assert(u.offset == 2400 && u.remaining == 600);
    assert(u.data[0] == 1 && u.data[1199] == 1200);

    /* Blocking read (ioflag = 0) still pads silence to satisfy full request */
    storage[mic_ring_head & ((65536u) - 1u)] = 42;
    mic_ring_head++;
    u = (struct uio){ .remaining = 8 };
    assert(annibale_mic_read(&u, 0) == 0);
    assert(u.remaining == 0 && u.offset == 8 && u.data[0] == 42 && u.data[1] == 0 && u.data[3] == 0);

    /* Odd byte count and unsolicited slot completion are rejected */
    fill_done_payload(cap_words, 0, 5);
    assert(reach_gpr_event(IID_CAP_SHMEM, OUR_MIC_PORT, OP_READ_BUF_DONE, 0, cap_words, sizeof(cap_words)));
    assert(mic_async_failed);
    mic_async_failed = false;
    mic_inflight_mask = 0x05u; /* slot 1 not in flight */
    fill_done_payload(cap_words, 1, 1920);
    assert(reach_gpr_event(IID_CAP_SHMEM, OUR_MIC_PORT, OP_READ_BUF_DONE, 0, cap_words, sizeof(cap_words)));
    assert(mic_async_failed && mic_inflight_mask == 0x05u);
    mic_inflight_mask |= (1u << 1);

    /* 3. Malformed async completion latches error and recovers on close + reopen */
    fill_done_payload(cap_words, 1, 1920);
    cap_words[9] = 1; /* bad metadata status */
    assert(reach_gpr_event(IID_CAP_SHMEM, OUR_MIC_PORT, OP_READ_BUF_DONE, 0, cap_words, sizeof(cap_words)));
    assert(mic_async_failed && (mic_inflight_mask & (1u << 1)) == 0);
    assert(run_thread_once(1) == 1); /* stops graphs and sets EIO */
    assert(mic_capture_error == EIO && !mic_graphs_started && graph_stops == 2);
    assert(stopped_sgs[0] == SG_CAPTURE && stopped_sgs[1] == SG_MIC);
    u = (struct uio){ .remaining = 8 };
    assert(annibale_mic_read(&u, IO_NDELAY) == EIO);

    /* Last client close stops & clears error; second open increments refcount cleanly */
    recording = false;
    assert(mic_cdev_close(0, 0, 0, NULL) == 0 && mic_open_clients == 0 && mic_capture_error == 0);
    assert(mic_cdev_open(0, 0, 0, NULL) == 0 && mic_open_clients == 1 && mic_graphs_started);
    assert(graph_opens == 2 && graph_prepares == 4); /* prepared+started without re-opening */
    assert(mic_cdev_open(0, 0, 0, NULL) == 0 && mic_open_clients == 2);
    assert(mic_cdev_close(0, 0, 0, NULL) == 0 && mic_open_clients == 1 && mic_graphs_started);
    assert(mic_cdev_close(0, 0, 0, NULL) == 0 && mic_open_clients == 0 && !mic_graphs_started && graph_stops == 4);

    /* Stall detection after 25 consecutive wait ticks inside mic_capture_thread */
    reset_state();
    assert(mic_cdev_open(0, 0, 0, NULL) == 0);
    assert(run_thread_once(0) == 1);
    assert(idle_waits >= 24 && mic_capture_error == EIO && !mic_graphs_started);
    assert(mic_cdev_close(0, 0, 0, NULL) == 0 && mic_capture_error == 0);

    assert(locked == 0 && spin_locked == 0);
    puts("PASS: 4-slot async mic GPR demux, stop/recovery lifecycle, and IO_NDELAY short reads");
    return 0;
}
'''


def main():
    constants = MIC[MIC.index("#define TLMM_PHYS"):MIC.index("static LCK_GRP_DECLARE")]
    builders = MIC[MIC.index("struct mic_pkt {"):MIC.index("/* ---- LPASS PRM")]
    source = HEADER + constants
    source += extract_function(REACH, "gpr_set32") + extract_function(REACH, "gpr_finish")
    source += extract_function(REACH, "annibale_audio_reach_send_cmd") + builders
    for name in ("mic_prm_vote_hw_core", "mic_prm_set_clk", "mic_configure_dmic_pins",
                 "mic_configure_va_macro", "mic_hw_prepare", "open_capture_graphs",
                 "close_capture_graphs", "start_capture_graphs",
                 "stop_capture_graphs", "queue_capture_slot_async", "harvest_capture_period",
                 "annibale_mic_gpr_event", "mic_capture_thread", "annibale_mic_read",
                 "mic_cdev_open", "mic_cdev_close"):
        source += extract_function(MIC, name)
    source += extract_function(REACH, "reach_gpr_event")
    source += MAIN

    mutations = (
        ("skip mic demux in reach_gpr_event",
         "if (annibale_mic_gpr_event(src_port, dst_port, opcode, payload, len)) {",
         "if (false && annibale_mic_gpr_event(src_port, dst_port, opcode, payload, len)) {"),
        ("pad zeros on empty IO_NDELAY",
         "if (nonblock && take == 0) {\n\t\t\tif (!moved_any) {\n\t\t\t\terror = EAGAIN;\n\t\t\t}\n\t\t\tbreak;\n\t\t}",
         "if (false && nonblock && take == 0) {\n\t\t\tif (!moved_any) {\n\t\t\t\terror = EAGAIN;\n\t\t\t}\n\t\t\tbreak;\n\t\t}"),
        ("never recover mic_capture_error on close",
         "if (mic_open_clients == 0 && !annibale_audio_recording_mic()) {\n\t\tstop_capture_graphs();\n\t\tmic_capture_error = 0;\n\t}",
         "if (mic_open_clients == 0 && !annibale_audio_recording_mic()) {\n\t\tstop_capture_graphs();\n\t}"),
        ("reject odd byte count dropped",
         "(words[4] % sizeof(int16_t)) != 0",
         "false"),
        ("ignore unsolicited slot completion",
         "!addr_ok || (mask & (1u << slot)) == 0 || words[0] != 0",
         "!addr_ok || (mask & 0u) != 0 || words[0] != 0"),
    )

    with tempfile.TemporaryDirectory(prefix="mic-async-") as directory:
        cfile, binary = Path(directory) / "test.c", Path(directory) / "test"
        for name, old, new in [("production", "", ""), *mutations]:
            if old:
                assert old in source, f"mutation anchor missing: {name}"
            cfile.write_text(source.replace(old, new, 1) if old else source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            str(cfile), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if old:
                assert result.returncode != 0, f"mutation survived: {name}"
            else:
                assert result.returncode == 0, result.stderr + result.stdout
                print(result.stdout, end="")
        print(f"PASS: {len(mutations)} ASan+UBSan mic regressions rejected")


if __name__ == "__main__":
    main()

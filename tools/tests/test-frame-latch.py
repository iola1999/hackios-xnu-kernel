#!/usr/bin/env python3
"""Compile the real framebuffer time conversion and latch routines with a mocked 19.2 MHz clock."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FB = ROOT / "xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp"
FLIP = ROOT / "xnu/files/iokit/Kernel/MiniFramebufferFlip.inc"


def function(source, name):
    marker = source.index(f"\n{name}(")
    start = source.rfind("\nstatic ", 0, marker) + 1
    brace = source.index("{", marker)
    depth = 0
    for at in range(brace, len(source)):
        if source[at] == "{":
            depth += 1
        elif source[at] == "}":
            depth -= 1
            if depth == 0:
                return source[start:at + 1]
    raise AssertionError(f"unterminated function {name}")


fb = FB.read_text()
flip = FLIP.read_text()
routines = "\n\n".join([
    function(fb, "micros"),
    function(flip, "flip_note_taken"),
    function(flip, "flip_taken"),
    function(flip, "flip_latch"),
])

test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define FLIP_CTL(n) (0x15000u + 0x1000u * (n))
#define FLIP_CTL_FLUSH 0x018u
#define FLIP_INTF(n) (0x34000u + 0x1000u * (n))
#define FLIP_INTF_FRAME_COUNT 0xacu

static struct {
    uint32_t ctl, flush, intf, pending_swap, taken_frame;
    uint64_t taken;
    bool taken_valid;
} flip;
static uint64_t now_ticks;
static uint32_t hardware_flush, hardware_frame;
static int clear_after = -1;

uint64_t mach_absolute_time(void) { return now_ticks; }
void absolutetime_to_nanoseconds(uint64_t ticks, uint64_t *ns) { *ns = ticks * 625 / 12; }
void delay(uint32_t us) { now_ticks += us * 96 / 5; }
void IOSleep(uint32_t ms) { now_ticks += ms * 19200; }

static uint32_t flip_rd(uint32_t off)
{
    if (off == FLIP_INTF(flip.intf) + FLIP_INTF_FRAME_COUNT)
        return hardware_frame;
    if (clear_after > 0 && --clear_after == 0)
        hardware_flush = 0;
    return hardware_flush;
}

''' + routines + r'''

static void reset(void)
{
    flip = (typeof(flip)){ .flush = 1, .intf = 1 };
    now_ticks = 0;
    hardware_flush = 1;
    hardware_frame = 42;
    clear_after = -1;
}

int main(void)
{
    /* 19.2 MHz: 19200 ticks are 1 ms. The production helper must convert absolute time, then divide ns by 1000. */
    assert(micros(19200) == 1000);
    assert(micros(19) == 0 && micros(20) == 1);

    reset();
    flip.pending_swap = 77;
    clear_after = 3;
    assert(flip_taken(500, false));
    assert(flip.taken_valid && flip.taken_frame == 42);
    assert(micros(flip.taken) == 100);   /* two 50 us polls before the third read observed clear */
    uint32_t swap = 0, frame = 0;
    uint64_t at = 0;
    assert(flip_latch(&swap, &at, &frame));
    assert(swap == 77 && at == flip.taken && frame == 42);
    assert(!flip_latch(&swap, &at, &frame));

    reset();
    flip.pending_swap = 88;
    flip.taken = 123456;
    assert(!flip_taken(120, false));
    assert(!flip.taken_valid && flip.taken == 123456);   /* a timeout must not manufacture a latch */
    assert(!flip_latch(&swap, &at, &frame));

    reset();
    hardware_flush = 0;
    now_ticks = 19200;
    assert(flip_taken(50000, true));
    assert(micros(flip.taken) == 1000 && flip.taken_valid);

    puts("framebuffer latch: 19.2 MHz to us, CTL clear, counter capture and timeout: ok");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="frame-latch-") as directory:
    source = Path(directory) / "latch.c"
    executable = Path(directory) / "latch"
    source.write_text(test)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-O2", str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

#!/usr/bin/env python3
"""Compile Iris/camera CBCR helpers and fixed AHB calls against an MMIO fixture."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVERS = ROOT / "xnu/files/bsd/dev/annibale"

HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uintptr_t vm_offset_t;
#define NSEC_PER_USEC 1000u
static uint32_t active_offset, value, last_write;
static unsigned reads, writes, delays, elapsed_us;
static bool reject_write, unreadable;
static void require(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}
static uint32_t reg_rd(vm_offset_t base, uint32_t offset) {
    require(base == 1 && offset == active_offset, "MMIO read bounds");
    reads++;
    return unreadable && writes ? UINT32_MAX : value;
}
static void reg_wr(vm_offset_t base, uint32_t offset, uint32_t requested) {
    require(base == 1 && offset == active_offset, "MMIO write bounds");
    writes++;
    last_write = requested;
    if (!reject_write) value = requested;
}
static void iris_sleep_us(unsigned duration) {
    delays++;
    elapsed_us += duration;
}
static void delay_for_interval(unsigned duration, unsigned scale) {
    require(scale == NSEC_PER_USEC, "delay units");
    iris_sleep_us(duration);
}
static void iris_say(const char *format, ...) { (void)format; }
#define mmio_r32 reg_rd
#define mmio_w32 reg_wr
#define say iris_say
__HELPERS__
__CONSTANTS__
static struct { vm_offset_t gcc; } g_iris = {1};
static vm_offset_t gcc_base = 1;
__CALLS__

static void reset(uint32_t offset, uint32_t initial) {
    active_offset = offset;
    value = initial;
    reads = writes = delays = elapsed_us = last_write = 0;
    reject_write = unreadable = false;
}
typedef bool (*enable_fn)(vm_offset_t, uint32_t, bool);
static void check_helper(enable_fn enable, unsigned settle_us, unsigned polls,
                         unsigned poll_us) {
    reset(0x10u, 0x88000002u);
    require(enable(1, active_offset, false), "fixed idle must accept enabled bit0");
    require(reads == 2 && writes == 1 && last_write == 0x88000003u,
            "fixed enable must preserve bits and read back");
    require(delays == 1 && elapsed_us == settle_us, "fixed enable must not poll HALT");

    reset(0x10u, 0x88000002u);
    reject_write = true;
    require(!enable(1, active_offset, false), "rejected write must fail bit0 check");

    reset(0x10u, 0x88000002u);
    unreadable = true;
    require(!enable(1, active_offset, false), "unreadable readback must fail");

    reset(0x10u, 0x08000000u);
    require(enable(1, active_offset, true), "ordinary running branch must succeed");
    require(delays == 0, "running branch needs no HALT wait");

    reset(0x10u, 0xa0000000u);
    require(enable(1, active_offset, true), "ordinary FSM status 2 must succeed");

    reset(0x10u, 0x88000002u);
    require(!enable(1, active_offset, true), "ordinary HALT must still fail");
    require(writes == 1 && delays == polls && elapsed_us == polls * poll_us,
            "ordinary HALT polling budget must remain unchanged");
}
int main(void) {
    check_helper(iris_enable_cbcr, 20, 2000, 5);
    check_helper(cbcr_enable, 10, 500, 10);
    reset(0x10u, 0);
    require(!cbcr_enable(0, active_offset, false) && reads == 0 && writes == 0,
            "camera missing mapping must fail before MMIO");

    const struct { uint32_t offset; bool (*call)(void); } fixed[] = {
        {GCC_VIDEO_AHB_CBCR, fixed_0},
        {GCC_CAMERA_AHB_CBCR, fixed_1},
        {GCC_CAM_BIST_MCLK_AHB_CBCR, fixed_2},
    };
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        reset(fixed[i].offset, 0x88000002u);
        require(fixed[i].call(), "production fixed AHB call must accept idle");
        require(reads == 2 && writes == 1, "production fixed AHB must read back");
    }
    return 0;
}
'''


def helper(source, name):
    begin = source.index(f"static bool\n{name}(")
    end = source.index("\n}\n", begin) + 3
    return source[begin:end]


def call(source, function, base, register):
    pattern = rf"{function}\({re.escape(base)}, {register}, (?:true|false)\)"
    matches = re.findall(pattern, source)
    if len(matches) != 1:
        raise SystemExit(f"expected one production call for {register}")
    return matches[0]


def main():
    iris = (DRIVERS / "annibale_iris_hw.c").read_text()
    camera = (DRIVERS / "annibale_camera_hw.inc").read_text()
    camera_entry = (DRIVERS / "annibale_camera.c").read_text()
    iris_header = (DRIVERS / "annibale_iris_private.h").read_text()
    functions = [helper(iris, "iris_enable_cbcr"), helper(camera, "cbcr_enable")]
    names = ["GCC_VIDEO_AHB_CBCR", "GCC_CAMERA_AHB_CBCR", "GCC_CAM_BIST_MCLK_AHB_CBCR"]
    constants = []
    for name, source in zip(names, [iris_header, camera, camera]):
        match = re.search(rf"^#define\s+{name}\s+0x[0-9a-fA-F]+u\s*$", source, re.MULTILINE)
        if match is None:
            raise SystemExit(f"missing production constant {name}")
        constants.append(match.group())
    calls = [call(iris, "iris_enable_cbcr", "g_iris.gcc", names[0]),
             call(camera_entry, "cbcr_enable", "gcc_base", names[1]),
             call(camera_entry, "cbcr_enable", "gcc_base", names[2])]

    with tempfile.TemporaryDirectory(prefix="annibale-cbcr-") as folder:
        base = Path(folder)

        def execute(label, bodies, expressions):
            wrappers = "\n".join(f"static bool fixed_{i}(void) {{ return {expression}; }}"
                                  for i, expression in enumerate(expressions))
            unit = base / f"{label}.c"
            unit.write_text(HARNESS.replace("__HELPERS__", "\n".join(bodies))
                            .replace("__CONSTANTS__", "\n".join(constants))
                            .replace("__CALLS__", wrappers))
            binary = base / label
            result = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     str(unit), "-o", str(binary)],
                                    capture_output=True, text=True, timeout=20)
            if result.returncode:
                raise SystemExit(result.stderr)
            return subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)

        result = execute("production", functions, calls)
        if result.returncode:
            raise SystemExit(result.stderr)

        guards = [
            ("v != UINT32_MAX && (v & 1u) != 0", "v != UINT32_MAX", "rejected write"),
            ("v != UINT32_MAX && (v & 1u) != 0", "(v & 1u) != 0", "unreadable readback"),
            ("(v & (1u << 31)) == 0 || ((v >> 28) & 7u) == 2u",
             "(v & 1u) != 0", "ordinary HALT"),
        ]
        rejected = 0
        for index, function in enumerate(functions):
            for variant, (original, replacement, failure) in enumerate(guards):
                if function.count(original) != 1:
                    raise SystemExit(f"expected one guard: {original}")
                bodies = functions.copy()
                bodies[index] = function.replace(original, replacement)
                result = execute(f"helper-{index}-{variant}", bodies, calls)
                if result.returncode == 0 or failure not in result.stderr:
                    raise SystemExit(f"compiled helper mutation did not fail: {index}/{variant}")
                rejected += 1
        for index, expression in enumerate(calls):
            if not expression.endswith(", false)"):
                raise SystemExit("production fixed AHB call must use check_off=false")
            changed = calls.copy()
            changed[index] = expression.replace(", false)", ", true)")
            result = execute(f"call-{index}", functions, changed)
            if result.returncode == 0 or "production fixed AHB call" not in result.stderr:
                raise SystemExit(f"compiled fixed AHB call mutation did not fail: {index}")
            rejected += 1
    print("PASS: production Iris/camera CBCR readback, fixed AHB idle, and ordinary HALT checks")
    print(f"PASS: {rejected} compiled guard/call mutations fail")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Compile the Iris GDSC helper and calls against the native CFG status contract."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__CONSTANTS__
typedef uintptr_t vm_offset_t;
static uint32_t control_offset, state_offset, control_value, state_value, last_write;
static unsigned control_reads, state_reads, writes, delays, elapsed_us, ready_us;
static bool reject_write, invalid_control_after_write, invalid_state_after_write;
static char diagnostic[256];
static void require(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}
static void check_address(vm_offset_t base, uint32_t offset) {
    require(base == 1 && (offset & 3u) == 0 &&
            (uint64_t)offset + sizeof(uint32_t) <= IRIS_VIDCC_SIZE,
            "GDSC MMIO aperture bounds");
}
static uint32_t mmio_r32(vm_offset_t base, uint32_t offset) {
    check_address(base, offset);
    if (offset == control_offset) {
        control_reads++;
        return invalid_control_after_write && writes ? UINT32_MAX : control_value;
    }
    require(offset == state_offset, "GDSC status must read native GDSCR + 4");
    state_reads++;
    if (invalid_state_after_write && writes) return UINT32_MAX;
    return ready_us != UINT32_MAX && elapsed_us >= ready_us ? 0x10000u : state_value;
}
static void mmio_w32(vm_offset_t base, uint32_t offset, uint32_t requested) {
    check_address(base, offset);
    require(offset == control_offset, "GDSC writes must remain at control address");
    writes++;
    last_write = requested;
    if (!reject_write) control_value = requested;
}
static void iris_sleep_us(unsigned duration) {
    require(duration == 10, "GDSC polling delay must remain 10 us");
    delays++;
    elapsed_us += duration;
}
static void iris_say(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(diagnostic, sizeof(diagnostic), format, arguments);
    va_end(arguments);
}
__HELPER__
static struct { vm_offset_t vidcc; } g_iris = {1};
__CALLS__
static void reset(uint32_t control, uint32_t state) {
    control_value = control;
    state_value = state;
    control_reads = state_reads = writes = delays = elapsed_us = last_write = 0;
    ready_us = UINT32_MAX;
    reject_write = invalid_control_after_write = invalid_state_after_write = false;
    diagnostic[0] = '\0';
}
static void check_domain(uint32_t control, uint32_t state, bool (*enable)(void)) {
    control_offset = control;
    state_offset = state;
    require(control + 4u == state && state + sizeof(uint32_t) <= IRIS_VIDCC_SIZE,
            "production GDSCR + 4 must match native address within mapping");

    reset(0, 0x10000u);
    require(enable(), "cfg bit16 must accept power-on without control bit31");
    require(writes == 0 && delays == 0 && control_reads == 1 && state_reads == 1,
            "already powered GDSC must not write or wait");

    reset(0x80000000u, 0);
    require(!enable(), "control bit31 cannot signal CFG power-on");
    require(writes == 1 && last_write == 0x80000000u && delays == 5000 && elapsed_us == 50000,
            "GDSC off-state timeout budget must remain 5000 x 10 us");
    require(strstr(diagnostic, "power-on timeout") != NULL,
            "off-state timeout must use existing GDSC diagnostics");

    reset(0, 0x80008000u);
    require(!enable(), "CFG bit15 or bit31 cannot signal power-on");

    reset(0x80000203u, 0x10000u);
    reject_write = true;
    require(!enable(), "SW_COLLAPSE must reject stale CFG power-on after rejected write");
    require(writes == 1 && last_write == 0x80000200u && delays == 5000,
            "rejected collapse write must preserve control bits and stay bounded");

    reset(0x203u, 0x10000u);
    require(enable(), "cleared SW_COLLAPSE and CFG bit16 must complete power-on");
    require(writes == 1 && last_write == 0x200u && delays == 0,
            "control write must preserve v & ~3u");

    reset(UINT32_MAX, 0x10000u);
    require(!enable(), "invalid initial control must fail");
    require(writes == 0, "invalid initial control must not be written back");
    reset(0, UINT32_MAX);
    require(!enable(), "invalid initial CFG must fail");
    require(writes == 0, "invalid initial CFG must not start power-on");

    reset(3u, 0x10000u);
    invalid_control_after_write = true;
    require(!enable(), "invalid polled control must fail");
    reset(3u, 0x10000u);
    invalid_state_after_write = true;
    require(!enable(), "invalid polled CFG must fail");

    reset(3u, 0);
    ready_us = 30;
    require(enable(), "delayed CFG bit16 must complete power-on");
    require(writes == 1 && delays == 3 && elapsed_us == 30,
            "delayed CFG power-on must use original polling delay");

    reset(3u, 0);
    require(!enable(), "CFG remaining off must time out");
    require(writes == 1 && delays == 5000 && elapsed_us == 50000,
            "power-on timeout must remain bounded");
}
int main(void) {
    check_domain(VIDCC_MVS0C_GDSCR, 0x8038u, native_0);
    check_domain(VIDCC_MVS0_GDSCR, 0x806cu, native_1);
    return 0;
}
'''


def extract_helper(source):
    begin = source.index("static bool\niris_enable_gdsc(")
    end = source.index("\n}\n", begin) + 3
    return source[begin:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    drivers = args.source_root / "xnu/files/bsd/dev/annibale"
    source = (drivers / "annibale_iris_hw.c").read_text()
    header = (drivers / "annibale_iris_private.h").read_text()
    body = extract_helper(source)
    constants = []
    for name in ["IRIS_VIDCC_SIZE", "VIDCC_MVS0C_GDSCR", "VIDCC_MVS0_GDSCR"]:
        match = re.search(rf"^#define\s+{name}\s+0x[0-9a-fA-F]+(?:u|UL)\s*$",
                          header, re.MULTILINE)
        if match is None:
            raise SystemExit(f"missing production constant {name}")
        constants.append(match.group())
    calls = []
    for index, name in enumerate(["VIDCC_MVS0C_GDSCR", "VIDCC_MVS0_GDSCR"]):
        matches = re.findall(rf'iris_enable_gdsc\(g_iris\.vidcc, {name}, "[a-z0-9]+"\)', source)
        if len(matches) != 1:
            raise SystemExit(f"expected one production call for {name}")
        calls.append(f"static bool native_{index}(void) {{ return {matches[0]}; }}")

    with tempfile.TemporaryDirectory(prefix="annibale-iris-gdsc-") as folder:
        base = Path(folder)

        def execute(label, helper):
            unit = base / f"{label}.c"
            unit.write_text(HARNESS.replace("__CONSTANTS__", "\n".join(constants))
                            .replace("__HELPER__", helper).replace("__CALLS__", "\n".join(calls)))
            binary = base / label
            compiled = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                       str(unit), "-o", str(binary)],
                                      capture_output=True, text=True, timeout=20)
            if compiled.returncode:
                raise SystemExit(compiled.stderr)
            return subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)

        result = execute("production", body)
        if result.returncode:
            raise SystemExit(result.stderr)
        guards = [
            ("v == UINT32_MAX || cfg == UINT32_MAX", "cfg == UINT32_MAX",
             "invalid initial control"),
            ("v == UINT32_MAX || cfg == UINT32_MAX", "v == UINT32_MAX",
             "invalid initial CFG"),
            ("if ((cfg & (1u << 16)) != 0 && (v & 1u) == 0)",
             "if ((v & (1u << 31)) != 0 && (v & 1u) == 0)", "already powered"),
            ("if ((cfg & (1u << 16)) != 0 && (v & 1u) == 0)",
             "if ((cfg & (1u << 16)) != 0)", "SW_COLLAPSE"),
            ("cfg != UINT32_MAX && (cfg & (1u << 16)) != 0", "(cfg & (1u << 16)) != 0",
             "invalid polled CFG"),
            ("cfg != UINT32_MAX && (cfg & (1u << 16)) != 0",
             "cfg != UINT32_MAX && (v & (1u << 31)) != 0", "control bit31"),
            ("cfg != UINT32_MAX && (cfg & (1u << 16)) != 0",
             "cfg != UINT32_MAX && (cfg & (1u << 31)) != 0", "CFG bit15 or bit31"),
            ("cfg != UINT32_MAX && (cfg & (1u << 16)) != 0 && (v & 1u) == 0",
             "cfg != UINT32_MAX && (cfg & (1u << 16)) != 0", "SW_COLLAPSE"),
            ("i < 5000u", "i < 500u", "timeout budget"),
            ("v & ~3u", "v & ~1u", "preserve control bits"),
        ]
        rejected = 0
        for index, (original, replacement, failure) in enumerate(guards):
            if body.count(original) != 1:
                raise SystemExit(f"expected one guard: {original}")
            result = execute(f"guard-{index}", body.replace(original, replacement))
            if result.returncode == 0 or failure not in result.stderr:
                raise SystemExit(f"compiled guard mutation did not fail: {index}: {result.stderr}")
            rejected += 1
        if body.count("off + 4u") != 2:
            raise SystemExit("expected initial and polled GDSCR + 4 reads")
        for label, replacement, failure in [("wrong-status", "off + 12u", "native GDSCR + 4"),
                                             ("outside-map", "IRIS_VIDCC_SIZE", "aperture bounds")]:
            result = execute(label, body.replace("off + 4u", replacement))
            if result.returncode == 0 or failure not in result.stderr:
                raise SystemExit(f"compiled address mutation did not fail: {label}")
            rejected += 1
    print("PASS: actual Iris GDSC helper/calls, CFG bit16, SW_COLLAPSE, invalid reads, timeout, MMIO bounds")
    print(f"PASS: {rejected} compiled guard/address mutations fail")


if __name__ == "__main__":
    main()

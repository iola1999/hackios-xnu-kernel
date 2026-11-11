#!/usr/bin/env python3
"""Exercise production profiler control and table reads across start, stop and restart."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "xnu/files/osfmk/arm64/gic_v3_irq.c").read_text()
header = (ROOT / "xnu/files/osfmk/arm64/psci_smp.h").read_text()
slot = header[header.index("struct gic_prof_slot {"):]
slot = slot[:slot.index("};") + 2]
control = source[source.index("void\ngic_v3_prof_control("):]
control = control[:control.index("\nstatic void\ngic_irq_handler(")]

prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define MAX_CPUS 2
#define GIC_PROF_SLOTS 4
#define __builtin_arm_dmb(kind) ((void)0)
'''
globals = r'''
static volatile bool gic_prof_on;
static volatile int32_t gic_prof_only;
static struct gic_prof_slot gic_prof[MAX_CPUS][GIC_PROF_SLOTS];
static uint32_t gic_prof_lost[MAX_CPUS];
'''
checks = r'''
int main(void) {
    struct gic_prof_slot copy[GIC_PROF_SLOTS], zero[GIC_PROF_SLOTS] = {0};
    uint32_t lost;
    gic_v3_prof_control(true, 32);
    assert(gic_prof_on && gic_prof_only == 32);
    gic_prof[0][1] = (struct gic_prof_slot){ .pc = 0x1000, .pid = 32, .count = 17 };
    gic_prof[1][2] = (struct gic_prof_slot){ .pc = 0x2000, .pid = 32, .count = 29 };
    gic_prof_lost[0] = 3;
    gic_prof_lost[1] = 5;
    for (unsigned repeat = 0; repeat < 2; repeat++) {
        gic_v3_prof_control(false, 32);
        assert(!gic_prof_on);
        gic_v3_prof_table(0, copy, &lost);
        assert(copy[1].count == 17 && copy[1].pc == 0x1000 && lost == 3);
        gic_v3_prof_table(1, copy, &lost);
        assert(copy[2].count == 29 && copy[2].pc == 0x2000 && lost == 5);
    }
    /* Writing start again begins a new window, including a changed PID filter. */
    gic_v3_prof_control(true, 39);
    assert(gic_prof_on && gic_prof_only == 39);
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        gic_v3_prof_table(cpu, copy, &lost);
        assert(memcmp(copy, zero, sizeof(copy)) == 0 && lost == 0);
    }
    gic_prof[0][0].count = 1;
    gic_v3_prof_control(true, 0);
    assert(gic_prof_on && gic_prof_only == 0);
    gic_v3_prof_table(0, copy, &lost);
    assert(memcmp(copy, zero, sizeof(copy)) == 0 && lost == 0);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="hackios-prof-control-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prelude + slot + globals + control + checks)
    subprocess.run(["cc", "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("profiler stop preserves both CPU tables and lost counts; start/restart clears them")

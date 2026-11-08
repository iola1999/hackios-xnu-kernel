#!/usr/bin/env python3
"""Compile redwoodmetal_timing.inc on the host and check aggregation, status and log bounds."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST = r'''
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t fake_now;
static unsigned lines;
static char last[2048];
static uint64_t rw_now_us(void) { return fake_now; }
static void rw_print(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsnprintf(last, sizeof(last), format, ap);
    va_end(ap);
    lines++;
}

#include "redwoodmetal_timing.inc"

int main(void)
{
    for (unsigned i = 0; i < 10; i++)
        rw_timing_commit(i + 1, 9000, 2500, 100);
    rw_timing_completion(1, 1000, 9000, 2500, true);
    assert(lines == RW_TIMING_SLOW_LINES);   /* all ten commits and completion were slow; four lines is the cap */

    fake_now = 1000000;
    rw_timing_dump(fake_now);
    assert(lines == RW_TIMING_SLOW_LINES + 1);
    assert(strstr(last, "commits 10 encode 90000/9000 refresh 25000/2500 flush 1000/100"));
    assert(strstr(last, "completions 1 completed 1 errors 0"));
    assert(strstr(last, "queue 1000/1000 fence 9000/9000 writeback 2500/2500 total 12500/12500"));
    assert(strstr(last, "slow_lines 4"));
    assert(rw_timing.commits == 0 && rw_timing.completions == 0 && rw_timing.slow_lines == 0);

    rw_timing_completion(2, 0, 20, 2500, false);
    assert(strstr(last, "status error"));
    rw_timing_dump(2000000);
    assert(strstr(last, "commits 0") && strstr(last, "completions 1 completed 0 errors 1"));
    unsigned after = lines;
    rw_timing_dump(3000000);
    assert(lines == after);   /* an idle second has no aggregate line */

    puts("Metal timing: totals/max, real status, reset and four slow lines per second: ok");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="metal-timing-") as directory:
    source = Path(directory) / "timing.c"
    executable = Path(directory) / "timing"
    source.write_text(TEST)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-I",
                    str(ROOT / "tools/userland-driver"), str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

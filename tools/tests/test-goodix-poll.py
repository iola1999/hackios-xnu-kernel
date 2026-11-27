#!/usr/bin/env python3
"""Host ASan+UBSan test for Goodix two-tier active/idle absolute-deadline polling."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
GOODIX_SRC = ROOT.parent / "xnu" / "files" / "bsd" / "dev" / "annibale" / "annibale_goodix.c"


def extract_sched_code() -> str:
    text = GOODIX_SRC.read_text()
    start = text.index("#define GOODIX_ACTIVE_POLL_US")
    end = text.index("\nstatic bool\ngoodix_any_contact_down(void)")
    return text[start:end]


HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

__SCHED_CODE__

static void
check(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
}

int
main(void)
{
    struct goodix_poll_sched s;

    /* 1. Default init (no boot arg): active=2000, idle=8000. */
    goodix_poll_sched_init(&s, false, 0);
    check(s.active_us == 2000 && s.idle_us == 8000 && !s.in_active, "default init");

    /* 2. Boot arg touch_poll_us=4000 still uses 2000 active / 8000 idle. */
    goodix_poll_sched_init(&s, true, 4000);
    check(s.active_us == 2000 && s.idle_us == 8000, "boot_arg 4000 keeps 2000/8000");

    /* 3. Explicit fast active (1000) and slow idle (16000) bounds. */
    goodix_poll_sched_init(&s, true, 1000);
    check(s.active_us == 1000 && s.idle_us == 8000, "boot_arg 1000 lowers active");
    goodix_poll_sched_init(&s, true, 16000);
    check(s.active_us == 2000 && s.idle_us == 16000, "boot_arg 16000 raises idle");
    goodix_poll_sched_init(&s, true, 50);
    check(s.active_us == 2000 && s.idle_us == 8000, "out-of-range boot_arg ignored");

    /* 4. Idle starts at 8000 us. */
    goodix_poll_sched_init(&s, false, 0);
    uint32_t slp = goodix_poll_next_sleep_us(&s, 10000, 10000, false, false);
    check(!s.in_active && slp == 8000, "initial idle sleep 8000");

    /* 5. Touch event enters active tier and compensates 260 us SPI read duration. */
    slp = goodix_poll_next_sleep_us(&s, 18000, 18260, true, true);
    check(s.in_active && slp == 1740 && s.next_deadline_us == 20000,
          "active entry compensates 260 us SPI read");

    /* 6. Next active tick with 260 us SPI read stays phase-locked at 2000 us cadence. */
    slp = goodix_poll_next_sleep_us(&s, 20000, 20260, true, true);
    check(s.in_active && slp == 1740 && s.next_deadline_us == 22000,
          "phase-locked 2000 us cadence");

    /* 7. Small scheduler jitter (+300 us late wake) pulls back to exact deadline. */
    slp = goodix_poll_next_sleep_us(&s, 22300, 22560, true, true);
    check(slp == 1440 && s.next_deadline_us == 24000, "jitter pulls back to deadline");

    /* 8. Large preemption overrun resyncs cleanly without 0-sleep spin. */
    slp = goodix_poll_next_sleep_us(&s, 30000, 30260, true, true);
    check(slp == 2000 && s.next_deadline_us == 32260, "overrun resyncs cleanly");

    /* 9. Finger lift stays in active tier during 200 ms hold window, then drops to idle. */
    uint64_t lift_us = 30260;
    slp = goodix_poll_next_sleep_us(&s, lift_us + 100000, lift_us + 100000, false, false);
    check(s.in_active && slp == 2000, "stays active at 100 ms after lift");
    slp = goodix_poll_next_sleep_us(&s, lift_us + 200000, lift_us + 200000, false, false);
    check(!s.in_active && slp == 8000, "returns to 8000 us idle at 200 ms after lift");

    /* 10. Simulate 240 Hz (4167 us) touch stream over 100 reports: verify max gap <= 6200 us (no 9.3 ms skip). */
    goodix_poll_sched_init(&s, true, 4000);
    uint64_t now = 1000000;
    uint64_t next_report = now + 4167;
    uint64_t last_read = now;
    uint64_t max_gap = 0;
    int seen_reports = 0;
    while (seen_reports < 100) {
        bool ready = (now >= next_report);
        uint64_t done = now;
        if (ready) {
            done += 260;
            uint64_t gap = done - last_read;
            if (seen_reports > 0 && gap > max_gap) {
                max_gap = gap;
            }
            last_read = done;
            next_report += 4167;
            seen_reports++;
        }
        uint32_t wait = goodix_poll_next_sleep_us(&s, now, done, ready, true);
        now = done + wait + 150; /* simulate 150 us timer wake jitter */
    }
    check(max_gap <= 6500, "240 Hz stream must not skip to 9.3 ms");

    return 0;
}
"""


def main() -> int:
    code = HARNESS.replace("__SCHED_CODE__", extract_sched_code())
    with tempfile.TemporaryDirectory(prefix="goodix-poll-") as tmp:
        src = pathlib.Path(tmp) / "test.c"
        exe = pathlib.Path(tmp) / "test"
        src.write_text(code)
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                str(src),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
    print("test-goodix-poll: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

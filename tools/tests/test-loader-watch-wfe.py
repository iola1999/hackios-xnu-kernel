#!/usr/bin/env python3
"""Host ASan+UBSan test for xnuloader lc_watch_round EVNTEN+WFE dual-mode backoff."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
WATCH_INC = ROOT / "xnuloader/loader_03_ufs_block_watch.inc"
USB_INC = ROOT / "xnuloader/loader_06_usb_entry.inc"

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t g_cntvct = 100000000ULL;
static const uint64_t g_cntfrq = 19200000ULL;
#define READ_SYSREG(r) ((#r)[3] == 'F' ? g_cntfrq : g_cntvct)
#define START_SECONDS 10ULL
#define SILENCE_SECONDS 10ULL
#define OTHER_CPU_NOT_STARTED -2
#define OTHER_CPU_HUNG -3

struct kernel_live { volatile uint64_t heartbeat; };
enum { CTX_LOG_PA = 0, CTX_KSP = 10 };
static volatile uint64_t other_cpu_done, other_cpu_stage;
static uint64_t loader_ctx[16];
static int wdt_armed, charger_enabled, block_enabled = 1, uefi_list = 1;
static unsigned block_polls, usb_polls, wfe_calls;

static void flush_dcache(uint64_t a, uint64_t b) { (void)a; (void)b; }
static uint64_t seconds_since(uint64_t then) { return (g_cntvct - then) / g_cntfrq; }
static void held_irq_mask(void) {}
static void log_of_hung_kernel(const struct kernel_live *l, uint64_t s) { (void)l; (void)s; }
static void wdt_pet(void) {}
static void charger_poll(void) {}
static int hc_list_is_uefis(void) { return uefi_list; }
static void block_poll(void) { block_polls++; }
static void usb_poll_command(void) { usb_polls++; }
static void lc_watch_wfe_idle(void) { wfe_calls++; }

__USB_RECENT__
__LC_WATCH__

int main(void) {
    struct kernel_live live = { 1 };
    int arrived = 1;
    uint64_t started = g_cntvct, pet = g_cntvct, batt = g_cntvct, beat = 1, beat_at = g_cntvct;
    int64_t status = 0;

    /* 1. UEFI UFS mailbox active -> 1 + 64 block_poll(), 0 WFE. */
    uefi_list = 1; block_polls = usb_polls = wfe_calls = 0;
    assert(lc_watch_round(&live, 0, started, 0, &arrived, &pet, &batt, &beat, &beat_at, &status) == 0);
    assert(block_polls == 65u && usb_polls == 1u && wfe_calls == 0u);

    /* 2. Native UFS (!uefi_list) + recent USB activity (< 100 ms) -> 1 block_poll(), 0 WFE. */
    uefi_list = 0; usb_last_activity_ticks = g_cntvct - (g_cntfrq / 20ULL); /* 50 ms ago */
    block_polls = usb_polls = wfe_calls = 0;
    assert(lc_watch_round(&live, 0, started, 0, &arrived, &pet, &batt, &beat, &beat_at, &status) == 0);
    assert(block_polls == 1u && usb_polls == 1u && wfe_calls == 0u);

    /* 3. Native UFS (!uefi_list) + USB idle (>= 100 ms) -> 1 block_poll(), 1 WFE. */
    usb_last_activity_ticks = g_cntvct - (g_cntfrq / 5ULL); /* 200 ms ago */
    block_polls = usb_polls = wfe_calls = 0;
    assert(lc_watch_round(&live, 0, started, 0, &arrived, &pet, &batt, &beat, &beat_at, &status) == 0);
    assert(block_polls == 1u && usb_polls == 1u && wfe_calls == 1u);

    /* 4. Native UFS + never had USB activity -> 1 WFE. */
    usb_last_activity_ticks = 0;
    block_polls = usb_polls = wfe_calls = 0;
    assert(lc_watch_round(&live, 0, started, 0, &arrived, &pet, &batt, &beat, &beat_at, &status) == 0);
    assert(block_polls == 1u && usb_polls == 1u && wfe_calls == 1u);

    puts("xnuloader lc_watch_round dual-mode EVNTEN+WFE backoff: PASS");
    return 0;
}
'''


def main() -> None:
    watch = WATCH_INC.read_text()
    w0 = watch.index("/* LC-WATCH-BEGIN")
    w1 = watch.index("/* LC-WATCH-END */", w0)
    usb = USB_INC.read_text()
    u0 = usb.index("static uint64_t usb_last_activity_ticks;")
    u1 = usb.index("\n}\n", u0) + 3
    code = HARNESS.replace("__USB_RECENT__", usb[u0:u1]).replace("__LC_WATCH__", watch[w0:w1])
    with tempfile.TemporaryDirectory(prefix="loader-watch-wfe-") as d:
        src = Path(d) / "test.c"
        exe = Path(d) / "test"
        src.write_text(code)
        subprocess.run([
            "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Execute the loader menu recovery and boot fallback against owned resources."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnuloader/loader_06a_screen_ui.inc"

PRELUDE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct kernel_live { unsigned stop; };
static struct kernel_live live = { 17 };
static const struct kernel_live *running_live;
static int kernel_resources_held, firmware_fault, held_irq_masked;
static int usb_menu_interrupted, boot_fallback_active;
static uint64_t host_last_cmd;
static char last_result[64];
static char displayed[64];
static struct { unsigned log_seen; } UI;
static unsigned log_len = 37;
enum { CTX_LOG_PA, CTX_LOG_LEN };
static uint64_t loader_ctx[2];
#define READ_SYSREG(reg) 123
static void print(const char *format, const char *status) { (void)format; (void)status; }
static void ui_stage(const char *stage) { (void)stage; }
static void ui_draw_fault_banner(const char *status, const char *log, uint64_t length)
{ (void)log; (void)length; strcpy(displayed, status); }
"""

SCENARIOS = r"""
int main(void)
{
    /* Every combination models a command received before or after kernel handoff. */
    for (unsigned state = 0; state < 16; ++state) {
        running_live = state & 1 ? &live : NULL;
        kernel_resources_held = (state >> 1) & 1;
        firmware_fault = (state >> 2) & 1;
        held_irq_masked = (state >> 3) & 1;
        usb_menu_interrupted = 0;
        strcpy(last_result, state ? "returned-held 41 0" : "idle");
        const char *error = menu_enter_fastboot();
        assert((error == NULL) == (state == 0));
        assert(usb_menu_interrupted == (state == 0));
        assert(running_live == (state & 1 ? &live : NULL));
        assert(kernel_resources_held == ((state >> 1) & 1));
        assert(firmware_fault == ((state >> 2) & 1));
        assert(held_irq_masked == ((state >> 3) & 1));
        assert(live.stop == 17);
        assert(!strcmp(last_result, state ? "returned-held 41 0" : "idle"));
        boot_fallback_active = 0;
        boot_fallback_enter();
        assert(boot_fallback_active == 1 && host_last_cmd == 123 && UI.log_seen == 37);
        assert(!strcmp(last_result, displayed));
        assert(!strcmp(last_result, state ? "returned-held 41 0" : "idle"));
        assert(running_live == (state & 1 ? &live : NULL));
        assert(kernel_resources_held == ((state >> 1) & 1));
        assert(firmware_fault == ((state >> 2) & 1));
        assert(held_irq_masked == ((state >> 3) & 1));
        assert(live.stop == 17);
    }
    puts("loader recovery: 16 ownership combinations passed");
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static const char *\nmenu_enter_fastboot(void)")
    end = source.index("static void\nboot_fallback_check(void)", start)
    production = source[start:end]
    with tempfile.TemporaryDirectory(prefix="loader-recovery-") as directory:
        path = Path(directory)
        c = path / "test.c"
        binary = path / "test"
        c.write_text(PRELUDE + production + SCENARIOS)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        str(c), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()

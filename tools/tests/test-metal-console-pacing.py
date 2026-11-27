#!/usr/bin/env python3
"""Verify Metal console pacing and verbose log gating in annibalemetal."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRV = ROOT / "tools" / "userland-driver"


def extract_function(text: str, signature_prefix: str) -> str:
    idx = text.index(signature_prefix)
    prev_nl = text.rfind("\n", 0, idx)
    ret_nl = text.rfind("\n", 0, prev_nl) if prev_nl > 0 else -1
    start = 0 if ret_nl < 0 else ret_nl + 1
    brace = text.index("{", idx)
    depth = 1
    end = brace + 1
    while depth > 0:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


def main() -> None:
    console_src = (DRV / "annibalemetal_console.inc").read_text()
    metal_src = (DRV / "annibalemetal.m").read_text()
    enc_src = (DRV / "annibalemetal_renderencoder.inc").read_text()

    assert 'if (rw_log_verbose) {\n\t\trw_print("pipeline seen:' in metal_src
    assert 'if (rw_log_verbose) {\n\t\t\tstatic unsigned said_target;' in enc_src

    extracted = "\n".join([
        extract_function(console_src, "rw_env_flag_enabled(const char *name)"),
        extract_function(console_src, "rw_console_refresh_env(void)"),
        extract_function(console_src, "rw_is_noisy_line(const char *line, size_t length)"),
        extract_function(console_src, "rw_say_write(void *cookie, const char *bytes, int count)"),
        extract_function(metal_src, "rw_dealloc_print(void)"),
    ])

    harness = f"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool rw_log_verbose;
static bool rw_say_pace;
static uint64_t rw_dealloc_count;

static unsigned g_say_calls;
static char g_last_say[256];
static unsigned g_usleep_calls;
static unsigned g_last_usleep;

static int
sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
{{
    (void)oldp;
    (void)oldlenp;
    assert(strcmp(name, "kern.annibale.say") == 0);
    g_say_calls++;
    size_t n = newlen < sizeof(g_last_say) - 1 ? newlen : sizeof(g_last_say) - 1;
    memcpy(g_last_say, newp, n);
    g_last_say[n] = '\\0';
    return 0;
}}

static int
usleep(unsigned usec)
{{
    g_usleep_calls++;
    g_last_usleep = usec;
    return 0;
}}

{extracted}

static void
reset_counters(void)
{{
    g_say_calls = 0;
    g_last_say[0] = '\\0';
    g_usleep_calls = 0;
    g_last_usleep = 0;
}}

int
main(void)
{{
    unsetenv("ANNIBALE_METAL_LOG_VERBOSE");
    unsetenv("ANNIBALE_THUMBS");
    unsetenv("ANNIBALE_AIR_DUMP");
    rw_console_refresh_env();
    assert(!rw_log_verbose && !rw_say_pace);

    /* Default mode: core diagnostics emit without usleep(5000); noisy lines filtered. */
    reset_counters();
    const char *core = "tl: mt at 100 commits 24\\nAIRFAIL begin fragment\\n";
    rw_say_write(NULL, core, (int)strlen(core));
    assert(g_say_calls == 2);
    assert(g_usleep_calls == 0);

    reset_counters();
    const char *noisy = "fd tile ring create: clears ring 0x1 submit 0x2 seqno 3 tid 4\\n"
                        "  fd tile ring create: store ring 0x1\\n"
                        "rwa bind: texture 0\\n";
    rw_say_write(NULL, noisy, (int)strlen(noisy));
    assert(g_say_calls == 0);
    assert(g_usleep_calls == 0);
    assert(!rw_dealloc_print());
    assert(rw_dealloc_count == 0);

    /* Verbose mode: fd tile ring create and dealloc logs enabled, still zero usleep. */
    setenv("ANNIBALE_METAL_LOG_VERBOSE", "1", 1);
    rw_console_refresh_env();
    assert(rw_log_verbose && !rw_say_pace);
    reset_counters();
    const char *tile = "fd tile ring create: clears ring 0x1\\n";
    rw_say_write(NULL, tile, (int)strlen(tile));
    assert(g_say_calls == 1);
    assert(g_usleep_calls == 0);
    for (unsigned i = 1; i <= 32; i++)
        assert(rw_dealloc_print());
    assert(!rw_dealloc_print());
    for (unsigned i = 34; i < 256; i++)
        assert(!rw_dealloc_print());
    assert(rw_dealloc_print());

    /* Pacing preserved when ANNIBALE_THUMBS or ANNIBALE_AIR_DUMP is enabled. */
    unsetenv("ANNIBALE_METAL_LOG_VERBOSE");
    setenv("ANNIBALE_THUMBS", "1", 1);
    rw_console_refresh_env();
    assert(!rw_log_verbose && rw_say_pace);
    reset_counters();
    rw_say_write(NULL, tile, (int)strlen(tile));
    assert(g_say_calls == 0 && g_usleep_calls == 0);
    rw_say_write(NULL, "thumb line\\n", 11);
    assert(g_say_calls == 1 && g_usleep_calls == 1 && g_last_usleep == 5000);

    unsetenv("ANNIBALE_THUMBS");
    setenv("ANNIBALE_AIR_DUMP", "1", 1);
    rw_console_refresh_env();
    assert(rw_say_pace);
    reset_counters();
    rw_say_write(NULL, "air dump line\\n", 14);
    assert(g_say_calls == 1 && g_usleep_calls == 1 && g_last_usleep == 5000);

    setenv("ANNIBALE_AIR_DUMP", "0", 1);
    rw_console_refresh_env();
    assert(!rw_say_pace);

    puts("test-metal-console-pacing: PASS");
    return 0;
}}
"""

    with tempfile.TemporaryDirectory(prefix="metal-console-pacing-") as tmp:
        src_path = Path(tmp) / "test.c"
        bin_path = Path(tmp) / "test"
        src_path.write_text(harness)
        subprocess.run(
            [
                "xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-O1", "-g", "-fsanitize=address,undefined",
                str(src_path), "-o", str(bin_path),
            ],
            check=True,
        )
        res = subprocess.run([str(bin_path)], check=True, capture_output=True, text=True)
        print(res.stdout.strip())


if __name__ == "__main__":
    main()

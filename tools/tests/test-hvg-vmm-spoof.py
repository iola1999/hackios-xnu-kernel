#!/usr/bin/env python3
"""Verify 81-bsd%kern%hvg_sysctl.c.patch hunk structure and vmm_spoof_listed() default/override policy."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "xnu26/patches/81-bsd%kern%hvg_sysctl.c.patch"


def verify_hunk_counts(text: str) -> None:
    lines = text.splitlines()
    i = 0
    hunks = 0
    while i < len(lines):
        m = re.match(r"^@@ -(\d+),(\d+) \+(\d+),(\d+) @@", lines[i])
        if not m:
            i += 1
            continue
        hunks += 1
        old_expected, new_expected = int(m.group(2)), int(m.group(4))
        old_seen = new_seen = 0
        i += 1
        while i < len(lines) and not lines[i].startswith(("@@ ", "diff --git ")):
            ch = lines[i][:1]
            if ch == " ":
                old_seen += 1
                new_seen += 1
            elif ch == "-":
                old_seen += 1
            elif ch == "+":
                new_seen += 1
            i += 1
        assert old_seen == old_expected, f"hunk {hunks}: old {old_seen} != {old_expected}"
        assert new_seen == new_expected, f"hunk {hunks}: new {new_seen} != {new_expected}"
    assert hunks == 4, f"expected 4 hunks in {PATCH.name}, got {hunks}"


def extract_spoof_code(text: str) -> str:
    added = []
    in_block = False
    for line in text.splitlines():
        if line.startswith("+static char vmm_spoof_procs"):
            in_block = True
        if in_block:
            if line.startswith("+"):
                added.append(line[1:])
            if line == "+}":
                break
    code = "\n".join(added)
    assert "vmm_spoof_procs" in code and "vmm_spoof_listed" in code
    return code


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

__PRODUCTION__

int main(void) {
    /* Default list must include mediaanalysisd and exclude cameracaptured (BWFigCaptureDevice VM check). */
    assert(vmm_spoof_listed("mediaanalysisd"));
    assert(!vmm_spoof_listed("cameracaptured"));
    assert(!vmm_spoof_listed("bluetoothd"));
    assert(!vmm_spoof_listed("audiomxd"));
    assert(!vmm_spoof_listed("Camera"));
    assert(!vmm_spoof_listed(""));
    assert(!vmm_spoof_listed("mediaanalysisd2"));
    assert(!vmm_spoof_listed("mediaanalysis"));

    /* Boot-arg ane_vmm_procs override supports comma-separated lists and empty string. */
    snprintf(vmm_spoof_procs, sizeof(vmm_spoof_procs), "foo,mediaanalysisd,bar");
    assert(vmm_spoof_listed("foo"));
    assert(vmm_spoof_listed("mediaanalysisd"));
    assert(vmm_spoof_listed("bar"));
    assert(!vmm_spoof_listed("cameracaptured"));
    assert(!vmm_spoof_listed("fo"));

    vmm_spoof_procs[0] = '\0';
    assert(!vmm_spoof_listed("mediaanalysisd"));
    assert(!vmm_spoof_listed(""));

    puts("PASS: hvg_sysctl vmm_spoof_procs excludes cameracaptured and matches mediaanalysisd");
    return 0;
}
"""


def main() -> None:
    text = PATCH.read_text()
    verify_hunk_counts(text)
    code = extract_spoof_code(text)
    compiler = shlex.split(os.environ.get("CC", "clang"))
    with tempfile.TemporaryDirectory(prefix="hvg-vmm-spoof-") as tmp:
        cfile = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        cfile.write_text(HARNESS.replace("__PRODUCTION__", code))
        subprocess.run(
            compiler + ["-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(cfile), "-o", str(binary)],
            check=True,
        )
        result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
        print(result.stdout, end="")


if __name__ == "__main__":
    main()

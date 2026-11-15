#!/usr/bin/env python3
"""
Host test harness for Work Package 3B (WP-3B) Wi-Fi 7 / WCN7850 optimizations.

Executes genuine compiled C host unit test (test-wp3b-wifi-native.c) verifying:
1. DesignWare PCIe Root Complex MSI receiver W1C logic and per-vector masking.
2. EP MSI capability calculation (MME = MMC alignment) and pre/post-reset address alignment.
3. DMA coherent chunk recycling pool allocation and recycling.
4. Multithreaded bounded wakeup synchronization preventing lost wakeups.
Also enforces the repository constraint that all source files remain under 1200 lines.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = ROOT / "tools/tests"
TEST_C = TESTS_DIR / "test-wp3b-wifi-native.c"


def test_line_counts():
    """Verify that all modified and related source files remain strictly under 1200 lines."""
    files_to_check = [
        "hackios-xnu-kernel/wifi/lx/ath12k/lx_pci.c",
        "hackios-xnu-kernel/wifi/lx/src/lx_80211.c",
        "hackios-xnu-kernel/wifi/lx/src/lx_80211.h",
        "hackios-xnu-kernel/wifi/lx/src/lx_base.c",
        "hackios-xnu-kernel/wifi/lx/src/lx_join.c",
        "hackios-xnu-kernel/xnu/files/bsd/dev/annibale/annibale_apple80211.c",
        "hackios-xnu-kernel/xnu/files/bsd/dev/annibale/annibale_lx.c",
        "hackios-xnu-kernel/xnu/files/bsd/dev/annibale/annibale_net.c",
        "hackios-xnu-kernel/xnu/files/bsd/dev/annibale/annibale_sta.c",
        "hackios-xnu-kernel/xnu/files/bsd/dev/mhi/mhi_pcie.c",
        "hackios-xnu-kernel/xnu/files/bsd/dev/mhi/mhi_pcie_fw.c",
        "hackios-xnu-kernel/xnu/files/bsd/sys/annibale_sta.h",
        "hackios-xnu-kernel/tools/tests/test-wp3b-wifi-native.c",
    ]
    for rel_path in files_to_check:
        full_path = ROOT.parent / rel_path if not (ROOT / rel_path).exists() else (ROOT / rel_path)
        if not full_path.exists():
            full_path = ROOT / rel_path.replace("hackios-xnu-kernel/", "")
        assert full_path.exists(), f"File {full_path} not found"
        lines = len(full_path.read_text().splitlines())
        assert lines < 1200, f"File {rel_path} has {lines} lines (must be < 1200)"
        print(f"Line count check {rel_path}: {lines} lines < 1200 OK")


def run_real_c_host_test():
    """Compile and execute genuine C host test verifying MSI W1C, chunk pool recycling, and bounded wakeup."""
    assert TEST_C.exists(), f"Source file {TEST_C} not found"

    with tempfile.TemporaryDirectory() as tmpdir:
        bin_path = Path(tmpdir) / "test-wp3b-wifi-native"
        cmd_compile = [
            "xcrun", "clang",
            "-Wall", "-Werror", "-O2",
            str(TEST_C),
            "-o", str(bin_path)
        ]
        res_compile = subprocess.run(cmd_compile, capture_output=True, text=True)
        assert res_compile.returncode == 0, f"Compilation failed:\n{res_compile.stderr}"

        cmd_run = [str(bin_path)]
        res_run = subprocess.run(cmd_run, capture_output=True, text=True)
        print(res_run.stdout)
        if res_run.stderr:
            print(res_run.stderr, file=sys.stderr)
        assert res_run.returncode == 0, f"Test execution failed with code {res_run.returncode}"
        assert "ALL WP-3B UNIT TESTS PASSED SUCCESSFULLY!" in res_run.stdout, \
            "Expected completion banner missing from test execution"


if __name__ == "__main__":
    test_line_counts()
    run_real_c_host_test()
    print("\nALL WP-3B VALIDATION CHECKS PASSED SUCCESSFULLY!")

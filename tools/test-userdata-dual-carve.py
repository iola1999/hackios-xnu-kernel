#!/usr/bin/env python3
"""Unit tests for userdata-dual-carve.py partition carving and verification."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools/userdata-dual-carve.py"
BACKUP_GPT = ROOT.parent / "annibale-backup/20261022-2355/gpt"


def test_dual_carve_real_backup():
    print("Testing userdata-dual-carve with real annibale LUN 0 GPT backup...")
    assert TOOL.exists(), f"Tool not found: {TOOL}"
    assert BACKUP_GPT.exists(), f"Backup GPT dir not found: {BACKUP_GPT}"

    with tempfile.TemporaryDirectory() as td:
        shutil.copy(BACKUP_GPT / "sda-head.bin", td)
        shutil.copy(BACKUP_GPT / "sda-tail.bin", td)

        cmd = [sys.executable, str(TOOL), td, "--root-gib", "8", "--var-gib", "112"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        print(res.stdout)
        if res.stderr:
            print("STDERR:", res.stderr)
        assert res.returncode == 0, f"Tool failed with code {res.returncode}"

        out_head = Path(td) / "gpt-head.new"
        out_tail = Path(td) / "gpt-tail.new"
        assert out_head.exists(), "gpt-head.new was not created"
        assert out_tail.exists(), "gpt-tail.new was not created"
        assert out_head.stat().st_size == 32768, "gpt-head.new invalid size"
        assert out_tail.stat().st_size == 32768, "gpt-tail.new invalid size"

        # Verify output partitions
        assert "userdata:   LBA 3518464..32584698 (110.879 GiB" in res.stdout
        assert "darwin (root): LBA 32584704..34681855 (8.000 GiB" in res.stdout
        assert "darwin_var (var):  LBA 34681856..64041978 (112.000 GiB" in res.stdout
        assert "Total iOS Carved: 120.000 GiB" in res.stdout
        print("PASS: test_dual_carve_real_backup")


if __name__ == "__main__":
    test_dual_carve_real_backup()
    print("All userdata-dual-carve tests passed.")

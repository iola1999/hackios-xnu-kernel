#!/usr/bin/env python3
"""Run minicrypto's Cryptography Extensions code on the host and build it with kernel flags.

The host runs use the production mc_ce_arm64.c and mc_aes.c; the kernel build checks that
mc_ce_arm64.c, mc_aes.c and mc_digests.c compile with -mkernel -mgeneral-regs-only.
"""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MINICRYPTO_DIR = ROOT / "xnu/files/osfmk/minicrypto"
CE_C = MINICRYPTO_DIR / "mc_ce_arm64.c"
AES_C = MINICRYPTO_DIR / "mc_aes.c"
DIGESTS_C = MINICRYPTO_DIR / "mc_digests.c"
TEST_C = ROOT / "tools/tests/test_mc_ce_crypto.c"
XNU_SRC = ROOT / "xnu-src/xnu-12377.121.6"


def run_host_crypto_test():
    """Compile and execute test_mc_ce_crypto.c against mc_ce_arm64.c on host."""
    with tempfile.TemporaryDirectory() as tmpdir:
        bin_path = Path(tmpdir) / "test_mc_ce_crypto"
        cmd = [
            "clang", "-O3",
            f"-I{MINICRYPTO_DIR}",
            str(TEST_C),
            str(CE_C),
            "-o", str(bin_path),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0, f"Compilation failed:\n{res.stderr}"

        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        assert run_res.returncode == 0, f"Execution failed:\n{run_res.stdout}\n{run_res.stderr}"
        assert "ALL WP-4B VERIFICATION TESTS PASSED SUCCESSFULLY!" in run_res.stdout
        print("[PASS] Host crypto validation test passed successfully.")


def run_host_aes_selftest():
    """Compile and run mc_aes_selftest() on host to verify 80-byte CE cross-comparison."""
    if not XNU_SRC.exists():
        print("[SKIP] XNU source directory not found for host AES test.")
        return

    with tempfile.TemporaryDirectory() as tmpdir:
        test_main = Path(tmpdir) / "test_aes_main.c"
        test_main.write_text(
            '#include <stdio.h>\n'
            '#include "mc_ce_arm64.h"\n'
            'const char *mc_aes_selftest(void);\n'
            'int main(void) {\n'
            '    mc_ce_init();\n'
            '    const char *err = mc_aes_selftest();\n'
            '    if (err) {\n'
            '        printf("FAIL: %s\\n", err);\n'
            '        return 1;\n'
            '    }\n'
            '    printf("AES selftest 80-byte CE cross-comparison passed.\\n");\n'
            '    return 0;\n'
            '}\n'
        )
        bin_path = Path(tmpdir) / "test_aes_selftest"
        cmd = [
            "clang", "-O3",
            f"-I{MINICRYPTO_DIR}",
            f"-I{XNU_SRC}/EXTERNAL_HEADERS",
            str(test_main),
            str(AES_C),
            str(CE_C),
            "-o", str(bin_path),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0, f"Host AES compilation failed:\n{res.stderr}"

        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        assert run_res.returncode == 0, f"Host AES execution failed:\n{run_res.stdout}\n{run_res.stderr}"
        assert "AES selftest 80-byte CE cross-comparison passed." in run_res.stdout
        print("[PASS] Host AES selftest (including 80-byte hardware cross-comparison) passed.")


def run_kernel_mgeneral_regs_check():
    """Compile minicrypto sources with -mkernel -mgeneral-regs-only."""
    if not XNU_SRC.exists():
        print("[SKIP] XNU source directory not found for kernel compile check.")
        return

    with tempfile.TemporaryDirectory() as tmpdir:
        for src in [CE_C, AES_C, DIGESTS_C]:
            out_obj = Path(tmpdir) / f"{src.stem}.o"
            cmd = [
                "clang",
                "-target", "arm64-apple-darwin",
                "-mkernel",
                "-mgeneral-regs-only",
                "-DKERNEL",
                "-D__arm64__",
                f"-I{MINICRYPTO_DIR}",
                f"-I{XNU_SRC}/EXTERNAL_HEADERS",
                f"-I{XNU_SRC}/osfmk",
                f"-I{XNU_SRC}/bsd",
                f"-I{XNU_SRC}/libkern",
                "-c", str(src),
                "-o", str(out_obj),
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            assert res.returncode == 0, f"Kernel compilation of {src.name} failed:\n{res.stderr}"
        print("[PASS] Kernel -mgeneral-regs-only compilation check passed.")


def main():
    run_host_crypto_test()
    run_host_aes_selftest()
    run_kernel_mgeneral_regs_check()
    print("\nPASS: minicrypto host and kernel-flag checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())

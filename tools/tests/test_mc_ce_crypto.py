#!/usr/bin/env python3
"""Validation harness for WP-4B minicrypto ARMv8/v9 Cryptography Extensions.

Executes host unit tests and verifies vector register constraints,
FPU context save/restore state size, and interrupt context guards.
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MINICRYPTO_DIR = ROOT / "xnu/files/osfmk/minicrypto"
CE_H = MINICRYPTO_DIR / "mc_ce_arm64.h"
CE_C = MINICRYPTO_DIR / "mc_ce_arm64.c"
AES_C = MINICRYPTO_DIR / "mc_aes.c"
DIGESTS_C = MINICRYPTO_DIR / "mc_digests.c"
TEST_C = ROOT / "tools/tests/test_mc_ce_crypto.c"
XNU_SRC = ROOT / "xnu-src/xnu-12377.121.6"


def verify_fpu_state_layout():
    """Verify mc_fpu_state size and register save/restore in mc_ce_arm64.h."""
    content = CE_H.read_text()

    assert "uint8_t  q[12 * 16]" in content or "uint8_t q[12 * 16]" in content, \
        "mc_fpu_state.q must be 12 * 16 bytes (192 bytes)"
    assert "aligned(16)" in content, "mc_fpu_state.q must be 16-byte aligned"

    # Verify stp/ldp of q16..q19 (q0..q7 and q16..q19 saved, q8..q15 strictly untouched)
    assert 'stp q16, q17, [%2, #128]' in content, "kernel_fpu_begin must save q16, q17 at offset 128"
    assert 'stp q18, q19, [%2, #160]' in content, "kernel_fpu_begin must save q18, q19 at offset 160"
    assert 'ldp q16, q17, [%0, #128]' in content, "kernel_fpu_end must restore q16, q17 at offset 128"
    assert 'ldp q18, q19, [%0, #160]' in content, "kernel_fpu_end must restore q18, q19 at offset 160"

    # Verify AAPCS64 callee-saved registers q8..q15 are never saved/restored in code
    code_only = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)
    for r in range(8, 16):
        assert f"q{r}" not in code_only, f"kernel_fpu must not touch AAPCS64 callee-saved register q{r}"

    print("[PASS] mc_ce_arm64.h FPU state layout and register offsets verified.")


def audit_ce_vector_registers():
    """Audit inline assembly in mc_ce_arm64.c to ensure vector registers stay within {0..7, 16..19}."""
    content = CE_C.read_text()
    allowed_regs = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19}

    # Find all inline asm blocks
    asm_blocks = re.findall(r'__asm__\s+volatile\s*\((.*?)\);', content, re.DOTALL)
    assert len(asm_blocks) >= 8, f"Expected at least 8 asm blocks, found {len(asm_blocks)}"

    vector_pattern = re.compile(r'\b[vVqQdDsShHbB](\d+)\b')
    used_regs = set()

    for block in asm_blocks:
        matches = vector_pattern.findall(block)
        for m in matches:
            reg_idx = int(m)
            used_regs.add(reg_idx)
            assert reg_idx in allowed_regs, \
                f"Register {reg_idx} in inline assembly violates allowed set {allowed_regs}"

    assert used_regs.issubset(allowed_regs), f"Used registers {used_regs} exceed allowed set {allowed_regs}"
    for r in range(8, 16):
        assert r not in used_regs, f"Register {r} violates AAPCS64 callee-saved preservation!"
    print(f"[PASS] mc_ce_arm64.c vector register audit passed. Active registers: {sorted(used_regs)}")


def verify_rndr_and_subs_hardening():
    """Verify Part 3F Item 1 RNDR undefined instruction fix and subs clobbers."""
    ce_c_content = CE_C.read_text()
    provider_c_content = (MINICRYPTO_DIR / "mc_provider.c").read_text()
    aes_c_content = AES_C.read_text()

    # 1. Eliminate RNDR undefined instruction Panic
    assert "ID_AA64ISAR2_EL1" not in ce_c_content, "ID_AA64ISAR2_EL1 read must be removed"
    assert "(isar0 >> 60) & 0xf" in ce_c_content, "FEAT_RNG (RNDR) must be read from ID_AA64ISAR0_EL1[63:60]"
    assert "if (!mc_ce_has_rndr())" in ce_c_content, "mc_ce_rndr() must check mc_ce_has_rndr() at top"
    assert "mc_ce_has_rndr() && mc_ce_rndr(&hw_rnd)" in provider_c_content, \
        "mc_provider.c must guard mc_ce_rndr call with mc_ce_has_rndr()"
    assert 'printf("minicrypto: CE caps:' in provider_c_content, \
        "minicrypto_init() must print runtime capability bits"

    # 2. subs clobber and x7 check in 7 asm blocks
    asm_blocks = re.findall(r'__asm__\s+volatile\s*\((.*?)\);', ce_c_content, re.DOTALL)
    subs_blocks = [b for b in asm_blocks if "subs" in b]
    assert len(subs_blocks) == 7, f"Expected 7 assembly blocks with subs, found {len(subs_blocks)}"
    for idx, block in enumerate(subs_blocks):
        assert '"cc"' in block, f"Block {idx} using subs missing 'cc' in clobber list"
        assert '"w7"' not in block, f"Block {idx} must not use 'w7' in clobber list"

    # 3. AES boot self-test 80-byte coverage
    assert "plain80[80]" in aes_c_content, "mc_aes_selftest must define 80-byte test vector"
    assert "aes-256-ecb-80 hardware encrypt" in aes_c_content, "mc_aes_selftest must test 80-byte ECB hardware path"
    assert "aes-256-cbc-80 hardware encrypt" in aes_c_content, "mc_aes_selftest must test 80-byte CBC hardware path"
    assert "aes-256-ctr-80 hardware crypt" in aes_c_content, "mc_aes_selftest must test 80-byte CTR hardware path"

    print("[PASS] RNDR undefined instruction fix, subs clobbers, and 80-byte AES selftest verified.")


def verify_interrupt_context_guards():
    """Verify mc_aes.c and mc_digests.c guard CE hardware acceleration with !ml_at_interrupt_context()."""
    aes_content = AES_C.read_text()
    digests_content = DIGESTS_C.read_text()

    # In mc_aes.c: check that every CE invocation under #if defined(KERNEL) requires !ml_at_interrupt_context()
    assert aes_content.count("!ml_at_interrupt_context()") >= 5, \
        "mc_aes.c must guard all 5 CE paths with !ml_at_interrupt_context()"

    # In mc_digests.c: check that SHA-256 and SHA-512 CE paths require !ml_at_interrupt_context()
    assert digests_content.count("!ml_at_interrupt_context()") >= 2, \
        "mc_digests.c must guard SHA256 and SHA512 CE paths with !ml_at_interrupt_context()"

    print("[PASS] mc_aes.c and mc_digests.c interrupt context guards verified.")


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
    verify_fpu_state_layout()
    audit_ce_vector_registers()
    verify_rndr_and_subs_hardening()
    verify_interrupt_context_guards()
    run_host_crypto_test()
    run_host_aes_selftest()
    run_kernel_mgeneral_regs_check()
    print("\nALL WP-4B CE SIMD REGISTERS & CONTEXT GUARD CHECKS PASSED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

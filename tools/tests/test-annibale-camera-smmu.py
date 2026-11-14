#!/usr/bin/env python3
"""Exercise production SMMU functions with the native capture/read/close model."""
from pathlib import Path
import argparse
import importlib.util
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "xnu/files/bsd/dev/annibale"
spec = importlib.util.spec_from_file_location("camera_native", Path(__file__).with_name("test-annibale-camera-native.py"))
native_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native_test)


def run(directory, smmu, label, old=False):
    (directory / "annibale_camera_smmu.inc").write_text(smmu)
    binary = directory / label
    args = ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-I", str(directory), str(directory / "fixture.c"), "-o", str(binary)]
    if old:
        args += ["-DCAMERA_OLD_SMMU=1"]
    compiled = subprocess.run(args, capture_output=True, text=True, timeout=30)
    if compiled.returncode:
        raise AssertionError(compiled.stderr)
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reject-old", metavar="GIT_REV", help="also reject the recorded TYPE_BYPASS implementation")
    args = parser.parse_args()
    hardware = (DRIVER / "annibale_camera_hw.inc").read_text()
    smmu = (DRIVER / "annibale_camera_smmu.inc").read_text()
    native = (DRIVER / "annibale_camera_native.inc").read_text()
    camera = (DRIVER / "annibale_camera.c").read_text()
    profile = (DRIVER / "annibale_camera_profile.inc").read_text()
    constants = "\n".join(line for line in hardware.splitlines()
                          if line.startswith(("#define GPIO_", "#define FRONT_SENSOR_", "#define WIDE_SENSOR_", "#define GCC_",
                                              "#define MCLKCC_", "#define GDSC_", "#define CAMCC_TITAN_")))
    fixture = native_test.FIXTURE.read_text()
    for name in ("cam_smmu_configure_bypass", "cam_smmu_route", "cam_smmu_release"):
        fixture = fixture.replace(native_test.function(fixture, name), "")
    anchor = "static uint32_t reg_rd(vm_offset_t base, uint32_t offset)"
    fixture = fixture.replace(anchor, '#include "annibale-camera-smmu.fixture.c"\n\n' + anchor)
    fixture = fixture.replace("if (base != csid_lite_base) return 0;",
                              "if (base == 6) return camera_smmu_read(offset);\n    if (base != csid_lite_base) return 0;")
    fixture = fixture.replace("if (base != csid_lite_base) { shared_power_writes++; return; }",
                              "if (base == 6) { camera_smmu_write(offset, value); return; }\n    if (base != csid_lite_base) { shared_power_writes++; return; }")
    reset = native_test.function(fixture, "reset_fixture")
    fixture = fixture.replace(reset, reset[:-1] + "    camera_smmu_reset();\n}")
    fixture = fixture.replace('printf("%u native capture/read/close scenarios passed\\n", cases);',
                              'printf("%u native capture/read/close scenarios passed\\n", cases);\n    camera_smmu_cases();')
    read_close = "\n".join([
        native_test.function(profile, "cam_capture_resources_held"), native_test.function(profile, "cam_native_read"),
        native_test.function(camera, "cam_hardware_power_down"), native_test.function(camera, "annibale_cam_open"),
        native_test.function(camera, "annibale_cam_close"),
    ])
    mutations = {
        "tcr2-old-request": (
            "reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_TCR2, CAM_SMMU_S1_BYPASS_TCR2);",
            "reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_TCR2, CAM_SMMU_TCR2_AS);"),
        "tcr2-readback-mask": ("if (value != expected[i][1]) {",
            "uint32_t mask = expected[i][0] == CAM_SMMU_TCR2 ? ~0x60u : UINT32_MAX;\n\t\t"
            "if ((value & mask) != (expected[i][1] & mask)) {"),
        "asidpne-readback": ("(15u << 16) | ", "(15u << 16) | (1u << 12) | "),
        "bypass-type": ("allow ? (uint32_t)cam_smmu_bank : CAM_SMMU_FAULT", "allow ? (1u << 16) : CAM_SMMU_FAULT"),
        "cbar-attributes": ("#define CAM_SMMU_CBAR_S1                0x1f300u", "#define CAM_SMMU_CBAR_S1                0x10000u"),
        "cb-reference": ("!referenced[bank] && cam_smmu_bank_free(bank)", "cam_smmu_bank_free(bank)"),
        "gr1-ownership": ("!referenced[bank] && cam_smmu_bank_free(bank)", "!referenced[bank]"),
        "vmid-ownership": ("reg_rd(cam_smmu_base, cam_smmu_page_size + CAM_SMMU_GR1_CBA2R(cam_smmu_bank)) != cam_smmu_cba2r", "false"),
        "context-sync": ("return cam_smmu_sync(cam_smmu_cb_offset);", "return true;"),
        "route-sync": ("readback == value && cam_smmu_sync(0)", "readback == value"),
        "release-trans": ("reg_wr(cam_smmu_base, CAM_SMMU_SMR(cam_smmu_slot), 0);",
                          "reg_wr(cam_smmu_base, CAM_SMMU_SMR(cam_smmu_slot), 0);\n\t\treg_wr(cam_smmu_base, CAM_SMMU_S2CR(cam_smmu_slot), 0);"),
        "sticky-sync": ("if (cam_smmu_base == 0 || cam_smmu_sync_failed)", "if (cam_smmu_base == 0)"),
        "sid-acquire-cbar": ("reg_wr(cam_smmu_base, gr1 + CAM_SMMU_GR1_CBAR(cam_smmu_bank), CAM_SMMU_CBAR_S1);",
                             "if (initialize) reg_wr(cam_smmu_base, gr1 + CAM_SMMU_GR1_CBAR(cam_smmu_bank), CAM_SMMU_CBAR_S1);"),
        "context-before-claim": ("return reg_rd(cam_smmu_base, cam_smmu_page_size + CAM_SMMU_GR1_CBAR(bank)) == 0 &&",
                                "return reg_rd(cam_smmu_base, (128u + bank) * cam_smmu_page_size) == 0 &&"),
        "initial-read": ("uint32_t initial_sctlr = reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_SCTLR);",
                         "uint32_t initial_sctlr = 0;"),
        "active-bank": ("initial_sctlr == UINT32_MAX || (initial_sctlr & 1u) != 0", "initial_sctlr == UINT32_MAX"),
        "initial-disable": ("reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_SCTLR, 0);",
                            "/* omitted SCTLR initialization */"),
        "fault-clear": ("reg_wr(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FSR, CAM_SMMU_FSR_FAULT);",
                        "/* omitted FSR clear */"),
        "stop-ownership": ("done = cam_smmu_bank_valid(CAM_SMMU_S1_BYPASS_SCTLR);", "done = true;"),
        "capture-fault": ("cam_capture.error = EIO;", "cam_capture.error = 0;"),
    }
    with tempfile.TemporaryDirectory(prefix="annibale-camera-smmu-") as temp:
        directory = Path(temp)
        (directory / "sys").mkdir()
        (directory / "sys/ioccom.h").write_text("#define _IOR(g, n, t) 0\n")
        shutil.copyfile(ROOT / "xnu/files/bsd/sys/annibale_camera.h", directory / "annibale_camera.h")
        shutil.copyfile(DRIVER / "annibale_camera_exposure.inc", directory / "annibale_camera_exposure.inc")
        shutil.copyfile(Path(__file__).with_name("annibale-camera-smmu.fixture.c"), directory / "annibale-camera-smmu.fixture.c")
        (directory / "fixture.c").write_text("#include <errno.h>\n" + constants + "\n" + fixture)
        (directory / "annibale_camera_native.inc").write_text(native)
        (directory / "annibale_camera_clock.inc").write_text(native_test.function((DRIVER / "annibale_camera_clock.inc").read_text(), "cam_poll"))
        (directory / "annibale_camera_phy.inc").write_text("")
        (directory / "camera-production-read-close.inc").write_text(read_close)
        result = run(directory, smmu, "production")
        if result.returncode:
            raise AssertionError(result.stderr)
        print(result.stdout.strip())
        for name, (old, new) in mutations.items():
            if old not in smmu:
                raise AssertionError("mutation anchor missing: " + name)
            result = run(directory, smmu.replace(old, new), name)
            if result.returncode == 0:
                raise AssertionError("mutation was accepted: " + name)
        print(str(len(mutations)) + " SMMU permission/ownership/sync mutations rejected")
        if args.reject_old:
            old_hw = subprocess.check_output(["git", "show", args.reject_old + ":xnu/files/bsd/dev/annibale/annibale_camera_hw.inc"], cwd=ROOT, text=True)
            old_defines = "\n".join(line for line in old_hw.splitlines() if line.startswith("#define CAM_SMMU_"))
            old_state = "\n".join(line for line in old_hw.splitlines() if line.startswith("static ") and "cam_smmu" in line)
            old_functions = "\n".join(native_test.function(old_hw, name) for name in
                                      ("cam_smmu_sync", "cam_smmu_route", "cam_smmu_release", "cam_smmu_configure_bypass"))
            old_result = run(directory, old_defines + "\n" + old_state + "\n" + old_functions, "old-production", True)
            if old_result.returncode == 0:
                raise AssertionError("old SMMU implementation was accepted")
            expected = "SMMU TYPE_TRANS permission rejected: slot=12 bank=0 offset=0xc30 value=0 smr=0 bypass=1 address=0"
            if expected not in old_result.stderr:
                raise AssertionError("old implementation did not reach its saved S2CR restore: " + old_result.stderr)
            print("old TYPE_BYPASS/restore implementation rejected: " + old_result.stderr.strip())


if __name__ == "__main__":
    main()

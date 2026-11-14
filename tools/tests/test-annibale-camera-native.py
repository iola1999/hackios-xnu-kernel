#!/usr/bin/env python3
"""Run production capture/read/close against host MMIO and DMA ownership cases."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "xnu/files/bsd/dev/annibale"
FIXTURE = Path(__file__).with_name("annibale-camera-native.fixture.c")


def function(source, name):
    matches = list(re.finditer(r"static (?:bool|void|int)\s+" + name + r"\(", source))
    if len(matches) != 1:
        raise AssertionError("expected one production function: " + name)
    start = matches[0].start()
    at = source.index("{", matches[0].end()) + 1
    depth = 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return source[start:at]


def run(directory, native, read_close, label):
    (directory / "annibale_camera_native.inc").write_text(native)
    (directory / "camera-production-read-close.inc").write_text(read_close)
    binary = directory / label
    command = ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
               "-I", str(directory), str(directory / "fixture.c"), "-o", str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if compiled.returncode:
        raise AssertionError(compiled.stderr)
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)


def main():
    native = (DRIVER / "annibale_camera_native.inc").read_text()
    clock = (DRIVER / "annibale_camera_clock.inc").read_text()
    profile = (DRIVER / "annibale_camera_profile.inc").read_text()
    camera = (DRIVER / "annibale_camera.c").read_text()
    hardware = (DRIVER / "annibale_camera_hw.inc").read_text()
    constants = "\n".join(line for line in hardware.splitlines()
                          if line.startswith(("#define GPIO_", "#define FRONT_SENSOR_", "#define WIDE_SENSOR_", "#define GCC_",
                                              "#define MCLKCC_", "#define GDSC_", "#define CAMCC_TITAN_")))
    read_close = "\n".join([
        function(profile, "cam_capture_resources_held"), function(profile, "cam_native_read"),
        function(camera, "cam_hardware_power_down"), function(camera, "annibale_cam_open"),
        function(camera, "annibale_cam_close"),
    ])
    mutations = {
        "ordinary-group": ("#define CAM980_RDI0_DONE         2u", "#define CAM980_RDI0_DONE         1u"),
        "consumed-address": ("cam_capture.consumed_addr != (uint32_t)(cam_capture.dma_address >> 8)", "false"),
        "stale-buffer": ("!cam_poll(csid_lite_base, CAM980_BUF_STATUS, CAM980_RDI0_DONE, 0, 5000u)", "false"),
        "stale-reset": ("!cam_poll(csid_lite_base, CAM980_TOP_STATUS, 1u, 0, 5000u)", "false"),
        "reset-completion": ("if (!cam_native_reset(false)) {", "if (false) {"),
        "dma-range": ("cam_native_dma_bytes > (1ULL << 36) - first", "false"),
        "contiguity": ("kvtophys(cam_region_vbase + offset) != first + offset", "false"),
    }
    with tempfile.TemporaryDirectory(prefix="annibale-camera-native-") as temp:
        directory = Path(temp)
        (directory / "sys").mkdir()
        (directory / "sys/ioccom.h").write_text("#define _IOR(g, n, t) 0\n")
        shutil.copyfile(ROOT / "xnu/files/bsd/sys/annibale_camera.h", directory / "annibale_camera.h")
        shutil.copyfile(DRIVER / "annibale_camera_exposure.inc", directory / "annibale_camera_exposure.inc")
        (directory / "fixture.c").write_text("#include <errno.h>\n" + constants + "\n" + FIXTURE.read_text())
        (directory / "annibale_camera_clock.inc").write_text(function(clock, "cam_poll"))
        (directory / "annibale_camera_phy.inc").write_text("")
        baseline = run(directory, native, read_close, "production")
        if baseline.returncode:
            raise AssertionError(baseline.stderr)
        print(baseline.stdout.strip())
        rejected = []
        for name, (old, new) in mutations.items():
            if old not in native:
                raise AssertionError("mutation anchor missing: " + name)
            result = run(directory, native.replace(old, new), read_close, name)
            if result.returncode == 0:
                raise AssertionError("mutation was accepted: " + name)
            rejected.append(name)
        read_mutation = read_close.replace("(cam_capture.flags & ANNIBALE_CAM_CAPTURE_COMPLETE) != ANNIBALE_CAM_CAPTURE_COMPLETE",
                                          "false")
        result = run(directory, native, read_mutation, "incomplete-read")
        if result.returncode == 0:
            raise AssertionError("incomplete read mutation was accepted")
        rejected.append("incomplete-read")
        print(str(len(rejected)) + " ownership/IRQ/read guard mutations rejected")


if __name__ == "__main__":
    main()

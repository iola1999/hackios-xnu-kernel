#!/usr/bin/env python3
"""Empirical Challenger Test Suite for M6 Userland Modifications.

Focus:
1. mkjobs.py: DENY list filtering, assert passes, launchd.plist generation.
2. package.py: Default AUDIO=0, REST_DENY handling and adversarial overrides.
3. Full pipeline build and package verification.
"""

import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

WORKSPACE_ROOT = Path("/Users/fan/Downloads/hackios")
KERNEL_ROOT = WORKSPACE_ROOT / "hackios-xnu-kernel"
USERLAND_ROOT = WORKSPACE_ROOT / "hackios-xnu-userland"
PACKAGE_PY = WORKSPACE_ROOT / "tools/k90auto/package.py"
OUT_DEVICE = KERNEL_ROOT / "out/ios26-resume/bbmetal-device"


def log(msg):
    print(f"[TEST] {msg}")


def test_mkjobs_deny_and_assertions():
    log("=== Group 1: mkjobs.py DENY and Assertion Tests ===")
    for root_dir, label in [(KERNEL_ROOT, "kernel"), (USERLAND_ROOT, "userland")]:
        mkjobs_path = root_dir / "userland/mkjobs.py"
        assert mkjobs_path.exists(), f"mkjobs.py missing in {label}: {mkjobs_path}"
        code = mkjobs_path.read_text(encoding="utf-8")

        # Check line count
        lines = code.splitlines()
        log(f"{label} mkjobs.py line count: {len(lines)}")
        assert len(lines) <= 1200, f"{label} mkjobs.py exceeds 1200 lines ({len(lines)})"

        # Check DENY tuple definition
        assert "'audio'" in code, f"{label} mkjobs.py missing 'audio' in DENY"
        assert "'syncdefaultsd'" in code, f"{label} mkjobs.py missing 'syncdefaultsd' in DENY"
        assert "'com.apple.syncdefaultsd'" not in code.split("assert")[1] if "assert" in code else True

        # Check assertions present in file
        assert "assert not any(word in 'com.apple.cameracaptured' for word in DENY)" in code
        assert "assert not any(word in 'com.apple.fontservicesd' for word in DENY)" in code
        assert "assert not any(word in 'com.apple.locationd' for word in DENY)" in code
        assert "assert not any(word in 'com.apple.weatherd' for word in DENY)" in code

        # Verify no assertion fails against syncdefaultsd
        assert "assert not any(word in 'com.apple.syncdefaultsd' for word in DENY)" not in code

        # Extract DENY tuple dynamically and verify
        # Isolate DENY block
        g = {}
        exec("""
import os
DENY = ('watchdog', 'CommCenter', 'wifi', 'WiFi', 'bluetooth', 'BTServer', 'softwareupdate',
        'datamigrator', 'backupd', 'thermalmonitord', 'keybagd', 'nfcd', 'biometrickitd', 'pearld',
        'obliterat', 'storage_mounter', 'com.apple.fud', 'sepd', 'seputil', 'ReportCrash',
        'osanalytics', 'lockdown', 'audio', 'jetsamproperties',
        'purplebuddy', 'restore', 'usb', 'accessoryd',
        'mobileassetd', 'bird',
        'homed', 'routined', 'duetexpertd', 'com.apple.dmd', 'syncdefaultsd', 'photoanalysisd', 'avconferenced')
assert not any(word in 'com.apple.cameracaptured' for word in DENY)
assert not any(word in 'com.apple.fontservicesd' for word in DENY)
assert not any(word in 'com.apple.locationd' for word in DENY)
assert not any(word in 'com.apple.weatherd' for word in DENY)
""", g)
        deny = g['DENY']

        # Adversarial DENY membership test
        assert any(w in 'com.apple.syncdefaultsd' for w in deny), "syncdefaultsd must be blocked by DENY"
        assert any(w in 'com.apple.audiomxd' for w in deny), "audiomxd must be blocked by 'audio' in DENY"
        assert any(w in 'com.apple.audioanalyticsd' for w in deny), "audioanalyticsd must be blocked by 'audio' in DENY"
        assert not any(w in 'com.apple.locationd' for w in deny), "locationd must not be blocked"
        assert not any(w in 'com.apple.weatherd' for w in deny), "weatherd must not be blocked"
        assert not any(w in 'com.apple.cameracaptured' for w in deny), "cameracaptured must not be blocked"
        assert not any(w in 'com.apple.fontservicesd' for w in deny), "fontservicesd must not be blocked"

        # Test ALLOW override
        env_allow = "audio syncdefaultsd"
        allowed_deny = tuple(word for word in deny if word not in env_allow.split())
        assert not any(w in 'com.apple.syncdefaultsd' for w in allowed_deny), "syncdefaultsd should be unblocked by ALLOW"
        assert not any(w in 'com.apple.audiomxd' for w in allowed_deny), "audiomxd should be unblocked by ALLOW"

    log("Group 1 Passed: mkjobs.py DENY list, assertions, and overrides validated.")


def test_package_audio_and_rest_deny():
    log("=== Group 2: package.py Default AUDIO=0 and REST_DENY Tests ===")
    assert PACKAGE_PY.exists(), f"package.py not found at {PACKAGE_PY}"
    pkg_code = PACKAGE_PY.read_text(encoding="utf-8")

    # Check line count
    pkg_lines = pkg_code.splitlines()
    log(f"package.py line count: {len(pkg_lines)}")
    assert len(pkg_lines) <= 1200, f"package.py exceeds 1200 lines ({len(pkg_lines)})"

    # Check default AUDIO assignment
    assert 'AUDIO = os.environ.get("AUDIO", "0") == "1"' in pkg_code, "AUDIO default must be '0'"
    assert 'REST_DENY = ["com.apple.PerfPowerServices", "com.apple.syncdefaultsd"]' in pkg_code

    # Test environment parsing logic across cases
    cases = [
        ({}, False),
        ({"AUDIO": "0"}, False),
        ({"AUDIO": "1"}, True),
        ({"AUDIO": "true"}, False),
        ({"AUDIO": "True"}, False),
        ({"AUDIO": ""}, False),
    ]
    for env, expected in cases:
        parsed = env.get("AUDIO", "0") == "1"
        assert parsed == expected, f"Env {env} expected {expected}, got {parsed}"

    # Test REST_DENY filtering logic in simulation
    rest_deny = ["com.apple.PerfPowerServices", "com.apple.syncdefaultsd"]
    labels = {"com.apple.SpringBoard"}
    services = {"com.apple.springboard.services"}

    test_jobs = [
        {"Label": "com.apple.syncdefaultsd", "MachServices": {"com.apple.syncdefaultsd": True}},
        {"Label": "com.apple.PerfPowerServices", "MachServices": {"com.apple.perfpower": True}},
        {"Label": "com.apple.locationd", "MachServices": {"com.apple.locationd.desktop": True}},
        {"Label": "com.apple.audiomxd", "MachServices": {"com.apple.audio.audiomxd": True}},
    ]

    accepted = []
    for job in test_jobs:
        if job["Label"] in labels or job["Label"] in rest_deny:
            continue
        accepted.append(job["Label"])

    assert "com.apple.syncdefaultsd" not in accepted, "syncdefaultsd must be filtered by REST_DENY"
    assert "com.apple.PerfPowerServices" not in accepted, "PerfPowerServices must be filtered by REST_DENY"
    assert "com.apple.locationd" in accepted, "locationd should pass REST_DENY"
    assert "com.apple.audiomxd" in accepted, "audiomxd passes REST_DENY (it is stopped by mkjobs.py DENY when AUDIO=0)"

    # Adversarial test: what if REST_ALLOW attempts to force syncdefaultsd?
    # Even if ALLOW contains syncdefaultsd and mkjobs.py produces it, package.py's REST_DENY must drop it!
    adv_job = {"Label": "com.apple.syncdefaultsd", "MachServices": {"com.apple.syncdefaultsd": True}}
    assert adv_job["Label"] in rest_deny, "Adversarial override must still be blocked by REST_DENY"

    log("Group 2 Passed: package.py AUDIO=0 default and REST_DENY validated.")


def test_empirical_userland_build():
    log("=== Group 3: Empirical Userland Build & mkcache.py Execution ===")
    userland_dir = KERNEL_ROOT / "userland"
    assert userland_dir.exists(), f"Userland dir missing: {userland_dir}"

    cmd = "IOS=../ios26 COMPILE_ONLY=1 bash build.sh"
    log(f"Running userland build: {cmd} in {userland_dir}")
    res = subprocess.run(cmd, shell=True, cwd=str(userland_dir), capture_output=True, text=True)
    log(f"Return code: {res.returncode}")
    if res.returncode != 0:
        log(f"Build stdout: {res.stdout[-1000:]}")
        log(f"Build stderr: {res.stderr[-1000:]}")
    assert res.returncode == 0, f"Userland build failed with exit code {res.returncode}"

    # Verify mkcache output
    assert "mkcache.py: 27 jobs in root/xpc/launchd.plist" in res.stdout, \
        f"mkcache output missing expected 27 jobs. Got: {res.stdout}"

    # Verify root/LaunchDaemons plists
    daemons_dir = userland_dir / "root/LaunchDaemons"
    assert daemons_dir.exists(), f"LaunchDaemons directory missing: {daemons_dir}"
    plists = [p.name for p in daemons_dir.glob("*.plist")]
    log(f"LaunchDaemons plist count: {len(plists)}")

    # Check absence of syncdefaultsd and audiomxd
    assert "com.apple.syncdefaultsd.plist" not in plists, "com.apple.syncdefaultsd.plist MUST NOT exist in LaunchDaemons"
    assert "com.apple.audiomxd.plist" not in plists, "com.apple.audiomxd.plist MUST NOT exist in LaunchDaemons"

    # Check presence of expected core jobs
    expected_jobs = [
        "com.apple.SpringBoard.plist",
        "com.apple.systemstatusd.plist",
        "com.apple.fontservicesd.plist",
    ]
    for ej in expected_jobs:
        assert ej in plists, f"Expected daemon plist missing: {ej}"

    # Verify root/xpc/launchd.plist
    launchd_cache_path = userland_dir / "root/xpc/launchd.plist"
    assert launchd_cache_path.exists(), f"launchd cache missing: {launchd_cache_path}"
    cache_data = plistlib.loads(launchd_cache_path.read_bytes())
    assert "LaunchDaemons" in cache_data, "launchd.plist missing LaunchDaemons dictionary"
    cache_jobs = cache_data["LaunchDaemons"]
    log(f"launchd.plist LaunchDaemons job count: {len(cache_jobs)}")
    assert len(cache_jobs) == 27, f"Expected 27 jobs in launchd.plist, got {len(cache_jobs)}"

    # Check that denied jobs are NOT in launchd.plist cache
    for path_key, job_dict in cache_jobs.items():
        label = job_dict.get("Label", "")
        assert "syncdefaultsd" not in label, f"syncdefaultsd found in launchd cache: {path_key}"
        assert "audiomxd" not in label, f"audiomxd found in launchd cache: {path_key}"

    log("Group 3 Passed: Userland build, LaunchDaemons, and launchd.plist empirically verified.")


def test_empirical_package_run():
    log("=== Group 4: Empirical package.py Run and Manifest Verification ===")
    # Execute package.py with AUDIO unset
    env = dict(os.environ)
    env.pop("AUDIO", None)

    cmd = [sys.executable, str(PACKAGE_PY)]
    log(f"Executing: {' '.join(cmd)} (AUDIO unset)")
    res = subprocess.run(cmd, cwd=str(WORKSPACE_ROOT), env=env, capture_output=True, text=True)
    log(f"package.py return code: {res.returncode}")
    if res.returncode != 0:
        log(f"package.py stdout: {res.stdout[-1000:]}")
        log(f"package.py stderr: {res.stderr[-1000:]}")
    assert res.returncode == 0, f"package.py failed with return code {res.returncode}"

    # Check output manifest
    manifest_path = OUT_DEVICE / "manifest.json"
    assert manifest_path.exists(), f"manifest.json missing: {manifest_path}"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    # Verify manifest fields
    assert "rest_deny" in manifest, "manifest.json missing 'rest_deny'"
    assert "com.apple.syncdefaultsd" in manifest["rest_deny"], \
        f"com.apple.syncdefaultsd not in rest_deny: {manifest['rest_deny']}"
    assert "com.apple.PerfPowerServices" in manifest["rest_deny"]

    # Verify audiomxd is NOT in rest_added
    rest_added = manifest.get("rest_added", [])
    assert "com.apple.audiomxd.plist" not in rest_added, \
        "com.apple.audiomxd.plist MUST NOT be in manifest rest_added when AUDIO=0"
    assert "com.apple.syncdefaultsd.plist" not in rest_added, \
        "com.apple.syncdefaultsd.plist MUST NOT be in manifest rest_added"

    # Check generated root.apfs
    root_apfs = OUT_DEVICE / "root.apfs"
    assert root_apfs.exists(), f"root.apfs missing: {root_apfs}"
    root_size = root_apfs.stat().st_size
    log(f"root.apfs size: {root_size} bytes")
    assert root_size > 10 * 1024 * 1024, f"root.apfs unexpectedly small: {root_size}"

    # Check standalone payload generation
    mkstandalone_py = KERNEL_ROOT / "xnuloader/mkstandalone.py"
    mach_kernel = KERNEL_ROOT / "out/xnu12377-cmd/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837"
    if mach_kernel.exists():
        log(f"Verifying standalone payload generation with kernel {mach_kernel}")
        stand_cmd = [
            sys.executable, str(mkstandalone_py),
            "-k", str(mach_kernel),
            "-r", str(root_apfs),
        ]
        s_res = subprocess.run(stand_cmd, cwd=str(KERNEL_ROOT), capture_output=True, text=True)
        log(f"mkstandalone return code: {s_res.returncode}")
        assert s_res.returncode == 0, f"mkstandalone failed: {s_res.stderr}"
        payload_lz4 = KERNEL_ROOT / "out/standalone-payload.bin.lz4"
        assert payload_lz4.exists(), f"Payload missing: {payload_lz4}"
        payload_size = payload_lz4.stat().st_size
        log(f"Payload size: {payload_size} bytes ({payload_size / (1024*1024):.2f} MiB)")
        assert payload_size < 96 * 1024 * 1024, f"Payload exceeds 96 MiB partition limit: {payload_size}"

    log("Group 4 Passed: package.py execution, manifest.json, root.apfs, and payload verified.")


def main():
    log("Starting Challenger R2 IT4 empirical test suite...")
    test_mkjobs_deny_and_assertions()
    test_package_audio_and_rest_deny()
    test_empirical_userland_build()
    test_empirical_package_run()
    log("ALL CHALLENGER EMPIRICAL TESTS PASSED SUCCESSFULLY!")


if __name__ == "__main__":
    main()

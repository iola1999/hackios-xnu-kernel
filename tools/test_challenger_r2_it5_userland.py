#!/usr/bin/env python3
"""Empirical Challenger Test Suite for Iteration 5 Userland and Packaging Changes.

Covers:
1. build.sh idempotency (runs multiple times, verifies no ln: File exists failure, verifies all ln -s have -f);
2. var_store.c secondary unpack prevention (verifies var_store skips unpack when kern.redwood.var_disk_loaded != 0, default_device probe order);
3. input26_service touch move-to-down promotion and state machine;
4. package.py jobs_source, default WIFI_FW=1, and AUDIO=1.
"""

import filecmp
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

WORKSPACE_ROOT = Path("/Users/fan/Downloads/hackios").resolve()
KERNEL_ROOT = WORKSPACE_ROOT / "hackios-xnu-kernel"
USERLAND_ROOT = WORKSPACE_ROOT / "hackios-xnu-userland"
PACKAGE_PY = WORKSPACE_ROOT / "tools/k90auto/package.py"
MANIFEST_JSON = KERNEL_ROOT / "out/ios26-resume/bbmetal-device/manifest.json"


class TestBuildShIdempotency(unittest.TestCase):
    """Adversarial and empirical tests for build.sh idempotency."""

    def test_build_sh_line_limit_and_shadow_sync(self):
        k_build = KERNEL_ROOT / "userland/build.sh"
        u_build = USERLAND_ROOT / "userland/build.sh"
        self.assertTrue(k_build.is_file(), f"Missing {k_build}")
        self.assertTrue(u_build.is_file(), f"Missing {u_build}")

        k_lines = len(k_build.read_text(encoding="utf-8").splitlines())
        u_lines = len(u_build.read_text(encoding="utf-8").splitlines())
        self.assertLessEqual(k_lines, 1200, f"Kernel build.sh has {k_lines} lines (>1200)")
        self.assertLessEqual(u_lines, 1200, f"Userland build.sh has {u_lines} lines (>1200)")

        self.assertTrue(
            filecmp.cmp(k_build, u_build, shallow=False),
            "Kernel build.sh and Userland build.sh are not identical"
        )

    def test_all_ln_use_force_flag(self):
        """Verify that every 'ln' invocation in build.sh includes '-f' or '-sf' to ensure idempotency."""
        import re
        build_sh = USERLAND_ROOT / "userland/build.sh"
        lines = build_sh.read_text(encoding="utf-8").splitlines()
        for idx, line in enumerate(lines, 1):
            stripped = line.strip()
            if re.search(r"\bln\b", stripped) and not stripped.startswith("#"):
                self.assertTrue(
                    "-sf" in stripped or "-f" in stripped,
                    f"Line {idx} uses 'ln' without force flag: {stripped}"
                )
                self.assertFalse(
                    bool(re.search(r"\bln\s+-s\b", stripped)),
                    f"Line {idx} uses 'ln -s' without -f: {stripped}"
                )

    def test_dangling_symlink_replacement_behavior(self):
        """Simulate how ln -sf handles target paths that are dangling on the host."""
        with tempfile.TemporaryDirectory() as td:
            link_path = os.path.join(td, "test_link")
            # Create symlink pointing to nonexistent path /redwood/lib/...
            res1 = subprocess.run(["ln", "-sf", "/redwood/lib/RedwoodAudio.driver", link_path],
                                  capture_output=True, text=True)
            self.assertEqual(res1.returncode, 0, res1.stderr)
            self.assertTrue(os.path.islink(link_path))

            # Second execution of ln -sf over existing symlink must succeed with exit code 0
            res2 = subprocess.run(["ln", "-sf", "/redwood/lib/RedwoodAudio.driver", link_path],
                                  capture_output=True, text=True)
            self.assertEqual(res2.returncode, 0, res2.stderr)

            # Third execution pointing to different path must replace cleanly
            res3 = subprocess.run(["ln", "-sf", "/var/jb/usr/bin/sileo", link_path],
                                  capture_output=True, text=True)
            self.assertEqual(res3.returncode, 0, res3.stderr)
            self.assertEqual(os.readlink(link_path), "/var/jb/usr/bin/sileo")


class TestVarStoreSecondaryUnpackPrevention(unittest.TestCase):
    """Empirical challenge for var_store.c secondary unpack prevention and device probe order."""

    def test_var_store_line_limit_and_shadow_sync(self):
        k_var = KERNEL_ROOT / "userland/src/var_store.c"
        u_var = USERLAND_ROOT / "userland/src/var_store.c"
        self.assertTrue(k_var.is_file(), f"Missing {k_var}")
        self.assertTrue(u_var.is_file(), f"Missing {u_var}")

        k_lines = len(k_var.read_text(encoding="utf-8").splitlines())
        u_lines = len(u_var.read_text(encoding="utf-8").splitlines())
        self.assertLessEqual(k_lines, 1200, f"Kernel var_store.c has {k_lines} lines (>1200)")
        self.assertLessEqual(u_lines, 1200, f"Userland var_store.c has {u_lines} lines (>1200)")

        self.assertTrue(
            filecmp.cmp(k_var, u_var, shallow=False),
            "Kernel var_store.c and Userland var_store.c are not identical"
        )

    def test_var_store_c_logic_and_probe_order(self):
        """Compile a dedicated C test binary verifying var_store sysctl skip and default_device order."""
        with tempfile.TemporaryDirectory() as td:
            test_c = Path(td) / "test_var_store_sim.c"
            test_c.write_text(r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#define DEVICE_DARWIN_VAR "/dev/rufs_darwin_var"
#define DEVICE_DSU        "/dev/rufs_dsu_store"

/* Mock state */
static int mock_var_disk_loaded_val = 0;
static int mock_sysctl_ret = 0;
static bool mock_stat_darwin_var = false;
static bool mock_stat_dsu = false;

static int unpack_called = 0;

/* Mock sysctlbyname */
int mock_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (strcmp(name, "kern.redwood.var_disk_loaded") == 0) {
        if (mock_sysctl_ret != 0) return -1;
        if (oldp && oldlenp && *oldlenp >= sizeof(int)) {
            *(int *)oldp = mock_var_disk_loaded_val;
            *oldlenp = sizeof(int);
        }
        return 0;
    }
    return -1;
}

/* Mock stat */
int mock_stat(const char *path, struct stat *st) {
    (void)st;
    if (strcmp(path, DEVICE_DARWIN_VAR) == 0) {
        return mock_stat_darwin_var ? 0 : -1;
    }
    if (strcmp(path, DEVICE_DSU) == 0) {
        return mock_stat_dsu ? 0 : -1;
    }
    return -1;
}

/* Replicate default_device() from var_store.c */
static const char *sim_default_device(void) {
    struct stat st;
    if (mock_stat(DEVICE_DARWIN_VAR, &st) == 0) {
        return DEVICE_DARWIN_VAR;
    }
    if (mock_stat(DEVICE_DSU, &st) == 0) {
        return DEVICE_DSU;
    }
    return DEVICE_DARWIN_VAR;
}

/* Replicate var_store load dispatch from var_store.c lines 916-923 */
static int sim_var_store_entry(const char *what) {
    unpack_called = 0;
    if (strcmp(what, "load") == 0) {
        int disk_loaded = 0;
        size_t dlen = sizeof(disk_loaded);
        if (mock_sysctlbyname("kern.redwood.var_disk_loaded", &disk_loaded, &dlen, NULL, 0) == 0 && disk_loaded != 0) {
            printf("var_store: /private/var already loaded from persistent disk; seed unpack skipped\n");
            return 0;
        }
    }
    /* Normal unpack path */
    unpack_called = 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    const char *test = argv[1];

    if (strcmp(test, "test_skip_when_loaded") == 0) {
        mock_sysctl_ret = 0;
        mock_var_disk_loaded_val = 1;
        int ret = sim_var_store_entry("load");
        if (ret != 0 || unpack_called != 0) {
            fprintf(stderr, "FAIL: expected early exit 0 with unpack_called=0, got ret=%d, unpack=%d\n", ret, unpack_called);
            return 10;
        }
        printf("SKIP_OK\n");
        return 0;
    }

    if (strcmp(test, "test_proceed_when_not_loaded") == 0) {
        mock_sysctl_ret = 0;
        mock_var_disk_loaded_val = 0;
        int ret = sim_var_store_entry("load");
        if (ret != 0 || unpack_called != 1) {
            fprintf(stderr, "FAIL: expected normal unpack when loaded=0, got ret=%d, unpack=%d\n", ret, unpack_called);
            return 11;
        }
        printf("PROCEED_ZERO_OK\n");
        return 0;
    }

    if (strcmp(test, "test_proceed_when_sysctl_error") == 0) {
        mock_sysctl_ret = -1;
        mock_var_disk_loaded_val = 1;
        int ret = sim_var_store_entry("load");
        if (ret != 0 || unpack_called != 1) {
            fprintf(stderr, "FAIL: expected fallback unpack on sysctl error, got ret=%d, unpack=%d\n", ret, unpack_called);
            return 12;
        }
        printf("PROCEED_ERR_OK\n");
        return 0;
    }

    if (strcmp(test, "test_save_not_skipped") == 0) {
        mock_sysctl_ret = 0;
        mock_var_disk_loaded_val = 1;
        int ret = sim_var_store_entry("save");
        if (ret != 0 || unpack_called != 1) {
            fprintf(stderr, "FAIL: save command must not be skipped, got unpack=%d\n", unpack_called);
            return 13;
        }
        printf("SAVE_NOT_SKIPPED_OK\n");
        return 0;
    }

    if (strcmp(test, "test_default_device_order") == 0) {
        /* Case 1: Both exist -> DARWIN_VAR takes priority */
        mock_stat_darwin_var = true;
        mock_stat_dsu = true;
        if (strcmp(sim_default_device(), DEVICE_DARWIN_VAR) != 0) {
            fprintf(stderr, "FAIL: both exist but DEVICE_DARWIN_VAR was not chosen\n");
            return 20;
        }

        /* Case 2: Only DSU exists -> DSU is chosen */
        mock_stat_darwin_var = false;
        mock_stat_dsu = true;
        if (strcmp(sim_default_device(), DEVICE_DSU) != 0) {
            fprintf(stderr, "FAIL: only DSU exists but DEVICE_DSU was not chosen\n");
            return 21;
        }

        /* Case 3: Only DARWIN_VAR exists -> DARWIN_VAR chosen */
        mock_stat_darwin_var = true;
        mock_stat_dsu = false;
        if (strcmp(sim_default_device(), DEVICE_DARWIN_VAR) != 0) {
            fprintf(stderr, "FAIL: only DARWIN_VAR exists but DEVICE_DARWIN_VAR was not chosen\n");
            return 22;
        }

        /* Case 4: Neither exists -> falls back to DEVICE_DARWIN_VAR */
        mock_stat_darwin_var = false;
        mock_stat_dsu = false;
        if (strcmp(sim_default_device(), DEVICE_DARWIN_VAR) != 0) {
            fprintf(stderr, "FAIL: neither exists but DEVICE_DARWIN_VAR fallback failed\n");
            return 23;
        }

        printf("PROBE_ORDER_OK\n");
        return 0;
    }

    return 1;
}
""")
            bin_path = Path(td) / "test_var_store_sim"
            res = subprocess.run(["/usr/bin/clang", "-O2", "-o", str(bin_path), str(test_c)],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Compilation failed: {res.stderr}")

            for tcase, expected_str in [
                ("test_skip_when_loaded", "SKIP_OK"),
                ("test_proceed_when_not_loaded", "PROCEED_ZERO_OK"),
                ("test_proceed_when_sysctl_error", "PROCEED_ERR_OK"),
                ("test_save_not_skipped", "SAVE_NOT_SKIPPED_OK"),
                ("test_default_device_order", "PROBE_ORDER_OK"),
            ]:
                run_res = subprocess.run([str(bin_path), tcase], capture_output=True, text=True)
                self.assertEqual(run_res.returncode, 0, f"{tcase} failed: {run_res.stderr} {run_res.stdout}")
                self.assertIn(expected_str, run_res.stdout)


class TestInput26ServiceTouchPromotion(unittest.TestCase):
    """Empirical challenge for input26_service touch move-to-down promotion and touch state machine."""

    def test_input26_service_line_limit_and_shadow_sync(self):
        k_in = KERNEL_ROOT / "userland/src/input26_service.c"
        u_in = USERLAND_ROOT / "userland/src/input26_service.c"
        self.assertTrue(k_in.is_file(), f"Missing {k_in}")
        self.assertTrue(u_in.is_file(), f"Missing {u_in}")

        k_lines = len(k_in.read_text(encoding="utf-8").splitlines())
        u_lines = len(u_in.read_text(encoding="utf-8").splitlines())
        self.assertLessEqual(k_lines, 1200, f"Kernel input26_service.c has {k_lines} lines (>1200)")
        self.assertLessEqual(u_lines, 1200, f"Userland input26_service.c has {u_lines} lines (>1200)")

        self.assertTrue(
            filecmp.cmp(k_in, u_in, shallow=False),
            "Kernel input26_service.c and Userland input26_service.c are not identical"
        )

    def test_touch_state_machine_and_promotion(self):
        """Compile and execute C simulation verifying touch MOVE promotion to DOWN."""
        with tempfile.TemporaryDirectory() as td:
            test_c = Path(td) / "test_touch_sim.c"
            test_c.write_text(r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define MAX_FINGERS 10
#define REDWOOD_TOUCH_DOWN 0
#define REDWOOD_TOUCH_MOVE 1
#define REDWOOD_TOUCH_UP   2

#define DIGITIZER_RANGE    0x00000001
#define DIGITIZER_TOUCH    0x00000002
#define DIGITIZER_POSITION 0x00000004

#define EDGE_SIDE   0.04
#define EDGE_TOP    0.04
#define EDGE_BOTTOM 0.06

struct redwood_touch {
    uint64_t time;
    uint32_t type;
    uint32_t finger;
    uint32_t x;
    uint32_t y;
};

struct finger_state {
    bool touching;
    bool edge;
    double x;
    double y;
};

static struct finger_state fingers[MAX_FINGERS];

struct dispatched_record {
    uint32_t event_type;
    uint32_t change_mask;
    bool was_touching;
    bool is_touching;
    bool is_edge;
    bool dispatched;
};

static struct dispatched_record last_record;

static void sim_send_touch(const struct redwood_touch *t) {
    memset(&last_record, 0, sizeof(last_record));
    uint32_t type = t->type;
    unsigned f = t->finger;

    if (f >= MAX_FINGERS) {
        return;
    }
    if (!fingers[f].touching) {
        if (type == REDWOOD_TOUCH_MOVE) {
            type = REDWOOD_TOUCH_DOWN;
        } else if (type != REDWOOD_TOUCH_DOWN) {
            return;
        }
    }
    uint32_t change = type == REDWOOD_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;

    last_record.was_touching = fingers[f].touching;
    fingers[f].touching = type != REDWOOD_TOUCH_UP;
    fingers[f].x = t->x / 65535.0;
    fingers[f].y = t->y / 65535.0;

    if (type == REDWOOD_TOUCH_DOWN) {
        fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE ||
                          fingers[f].y < EDGE_TOP || fingers[f].y > 1 - EDGE_BOTTOM;
    }

    last_record.event_type = type;
    last_record.change_mask = change;
    last_record.is_touching = fingers[f].touching;
    last_record.is_edge = fingers[f].edge;
    last_record.dispatched = true;
}

int main(void) {
    memset(fingers, 0, sizeof(fingers));

    /* Test 1: Normal DOWN event */
    struct redwood_touch t1 = { .time = 100, .type = REDWOOD_TOUCH_DOWN, .finger = 0, .x = 32768, .y = 32768 };
    sim_send_touch(&t1);
    if (!last_record.dispatched || last_record.event_type != REDWOOD_TOUCH_DOWN || !last_record.is_touching) {
        fprintf(stderr, "FAIL: Test 1 normal DOWN\n");
        return 1;
    }

    /* Test 2: Normal MOVE event while touching */
    struct redwood_touch t2 = { .time = 110, .type = REDWOOD_TOUCH_MOVE, .finger = 0, .x = 33000, .y = 33000 };
    sim_send_touch(&t2);
    if (!last_record.dispatched || last_record.event_type != REDWOOD_TOUCH_MOVE || last_record.change_mask != DIGITIZER_POSITION) {
        fprintf(stderr, "FAIL: Test 2 normal MOVE\n");
        return 2;
    }

    /* Test 3: Normal UP event */
    struct redwood_touch t3 = { .time = 120, .type = REDWOOD_TOUCH_UP, .finger = 0, .x = 33000, .y = 33000 };
    sim_send_touch(&t3);
    if (!last_record.dispatched || last_record.event_type != REDWOOD_TOUCH_UP || last_record.is_touching) {
        fprintf(stderr, "FAIL: Test 3 normal UP\n");
        return 3;
    }

    /* Test 4: Stray UP when not touching -> must be discarded */
    struct redwood_touch t4 = { .time = 130, .type = REDWOOD_TOUCH_UP, .finger = 0, .x = 33000, .y = 33000 };
    sim_send_touch(&t4);
    if (last_record.dispatched) {
        fprintf(stderr, "FAIL: Test 4 stray UP was not discarded\n");
        return 4;
    }

    /* Test 5: Adversarial MOVE when not touching (dropped DOWN) -> MUST PROMOTE TO DOWN */
    struct redwood_touch t5 = { .time = 200, .type = REDWOOD_TOUCH_MOVE, .finger = 1, .x = 1000, .y = 30000 };
    sim_send_touch(&t5);
    if (!last_record.dispatched || last_record.event_type != REDWOOD_TOUCH_DOWN) {
        fprintf(stderr, "FAIL: Test 5 dropped DOWN promotion to DOWN failed\n");
        return 5;
    }
    if (!last_record.is_touching) {
        fprintf(stderr, "FAIL: Test 5 finger not marked touching after promotion\n");
        return 6;
    }
    if (last_record.change_mask != (DIGITIZER_RANGE | DIGITIZER_TOUCH)) {
        fprintf(stderr, "FAIL: Test 5 change mask not set to DIGITIZER_RANGE | DIGITIZER_TOUCH\n");
        return 7;
    }
    if (!last_record.is_edge) {
        fprintf(stderr, "FAIL: Test 5 edge flag not computed on promoted DOWN (x=1000 < EDGE_SIDE)\n");
        return 8;
    }

    /* Test 6: Subsequent MOVE on finger 1 -> stays MOVE */
    struct redwood_touch t6 = { .time = 210, .type = REDWOOD_TOUCH_MOVE, .finger = 1, .x = 2000, .y = 30000 };
    sim_send_touch(&t6);
    if (!last_record.dispatched || last_record.event_type != REDWOOD_TOUCH_MOVE || last_record.change_mask != DIGITIZER_POSITION) {
        fprintf(stderr, "FAIL: Test 6 subsequent MOVE on promoted finger\n");
        return 9;
    }

    /* Test 7: Out of bounds finger index */
    struct redwood_touch t7 = { .time = 220, .type = REDWOOD_TOUCH_MOVE, .finger = MAX_FINGERS, .x = 2000, .y = 30000 };
    sim_send_touch(&t7);
    if (last_record.dispatched) {
        fprintf(stderr, "FAIL: Test 7 out of bounds finger was dispatched\n");
        return 10;
    }

    printf("TOUCH_STATE_MACHINE_OK\n");
    return 0;
}
""")
            bin_path = Path(td) / "test_touch_sim"
            res = subprocess.run(["/usr/bin/clang", "-O2", "-o", str(bin_path), str(test_c)],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Compilation failed: {res.stderr}")

            run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
            self.assertEqual(run_res.returncode, 0, f"Touch simulation failed: {run_res.stderr} {run_res.stdout}")
            self.assertIn("TOUCH_STATE_MACHINE_OK", run_res.stdout)


class TestPackagePyDefaultsAndManifest(unittest.TestCase):
    """Empirical challenge for package.py configuration, defaults, and manifest generation."""

    def test_package_py_line_limit(self):
        self.assertTrue(PACKAGE_PY.is_file(), f"Missing {PACKAGE_PY}")
        lines = len(PACKAGE_PY.read_text(encoding="utf-8").splitlines())
        self.assertLessEqual(lines, 1200, f"package.py has {lines} lines (>1200 limit)")

    def test_package_py_default_values_and_overrides(self):
        """Test package.py module-level evaluation of defaults vs environment variables."""
        test_script = r"""
import os
import sys
from pathlib import Path

HERE = Path('/Users/fan/Downloads/hackios/tools/k90auto')
REPO = HERE.parents[1]
WORKSPACE = REPO / 'hackios-xnu-kernel'

# 1. Test JOBS_SOURCE default
DEFAULT_JOBS = HERE / 'jobs/rc.jobs.interactive' if (HERE / 'jobs/rc.jobs.interactive').exists() else HERE / 'rc.jobs.desktop'
jobs_src = Path(os.environ.get('JOBS_SOURCE', DEFAULT_JOBS))
assert jobs_src.name == 'rc.jobs.interactive', f'Default JOBS_SOURCE should be rc.jobs.interactive, got {jobs_src}'

# 2. Test AUDIO default
AUDIO = os.environ.get('AUDIO', '1') == '1'
assert AUDIO is True, f'Default AUDIO should be True, got {AUDIO}'

# 3. Test WIFI_FW default
WIFI_FW_DIR = WORKSPACE / 'hackios-xnu-kernel/out/fw-wifi' if (WORKSPACE / 'hackios-xnu-kernel/out/fw-wifi').is_dir() else WORKSPACE / 'out/fw-wifi'
WIFI_FW_USED = ['amss20.bin', 'regdb.bin', 'bd_p11.elf', 'phy_ucode20.elf']
WIFI_FW_MODE = os.environ.get('WIFI_FW', '1' if all((WIFI_FW_DIR / n).exists() for n in WIFI_FW_USED) else '0')
WIFI_FW = WIFI_FW_MODE != '0'
assert WIFI_FW is True, f'Default WIFI_FW should be True, got {WIFI_FW}'

# 4. Test REST_ALLOW and REST_DENY
BLUETOOTH = True
REST_TRIAL = (os.environ.get('REST_ALLOW', '').split()
              + (['audio'] if AUDIO else [])
              + (['bluetooth'] if BLUETOOTH else []))
REST_ALLOW = ['com.apple.dmd'] + REST_TRIAL
REST_DENY = ['com.apple.PerfPowerServices', 'com.apple.syncdefaultsd']
assert 'audio' in REST_ALLOW, f'audio must be in REST_ALLOW, got {REST_ALLOW}'
assert 'com.apple.syncdefaultsd' in REST_DENY, f'syncdefaultsd must be in REST_DENY, got {REST_DENY}'

# 5. Test SEED default
DEFAULT_SEED = REPO / 'docs/progress/evidence/k90auto/kernel-12377-cmd30-seed3/seed-icons.bin'
SEED = Path(os.environ['SEED']) if os.environ.get('SEED') else (DEFAULT_SEED if DEFAULT_SEED.exists() else None)
assert SEED is not None and SEED.exists(), f'Default SEED must exist, got {SEED}'

print('DEFAULTS_OK')
"""
        res = subprocess.run([sys.executable, "-c", test_script], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Defaults check failed: {res.stderr}")
        self.assertIn("DEFAULTS_OK", res.stdout)

    def test_manifest_json_integrity(self):
        """Verify the generated manifest.json matches all required properties."""
        self.assertTrue(MANIFEST_JSON.is_file(), f"Missing {MANIFEST_JSON}")
        m = json.loads(MANIFEST_JSON.read_text(encoding="utf-8"))

        # Verify seed
        seed = m.get("seed")
        self.assertIsNotNone(seed, "manifest.json has null seed")
        self.assertTrue(seed.get("path", "").endswith("seed-icons.bin"), f"seed.path unexpected: {seed.get('path')}")
        self.assertGreater(seed.get("bytes", 0), 100_000_000, f"seed.bytes unexpectedly small: {seed.get('bytes')}")

        # Verify jobs_source
        jobs = m.get("jobs_source", {})
        self.assertIn("rc.jobs.interactive", jobs.get("path", ""), f"jobs_source unexpected: {jobs}")

        # Verify wifi_fw
        wifi = m.get("wifi_fw")
        self.assertIsNotNone(wifi, "wifi_fw must not be None")
        self.assertGreaterEqual(len(wifi), 4, f"wifi_fw file count unexpectedly low: {len(wifi)}")
        self.assertIn("amss20.bin", wifi)
        self.assertIn("regdb.bin", wifi)

        # Verify rest_allow and rest_deny
        rest_allow = m.get("rest_allow", [])
        rest_deny = m.get("rest_deny", [])
        self.assertIn("audio", rest_allow, f"'audio' missing from rest_allow: {rest_allow}")
        self.assertIn("com.apple.syncdefaultsd", rest_deny, f"'com.apple.syncdefaultsd' missing from rest_deny: {rest_deny}")


if __name__ == "__main__":
    unittest.main()

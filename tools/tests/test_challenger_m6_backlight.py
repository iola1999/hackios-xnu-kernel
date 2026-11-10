#!/usr/bin/env python3
"""Empirical Challenger Test Suite for MiniAppleARMBacklight 1:1 Slider & Panel Sync."""

from pathlib import Path
import random

REPO_ROOT = Path(__file__).resolve().parents[2]
BACKLIGHT_CPP = REPO_ROOT / "xnu/files/iokit/Kernel/MiniAppleARMBacklight.cpp"
RAMFS_DISK_INC = REPO_ROOT / "xnu/files/bsd/miscfs/ramfs/ramfs_disk.inc"

kBLValueMin = 5
kBLValueMax = 0x1A5E
kBLMilliNitsMin = 5000
kBLMilliNitsMid = 140000
kBLMilliNitsMax = 400000
kBLLevelFull = 1024


def fixed_nits(millinits: int) -> int:
    return (millinits << 16) // 1000


class MiniAppleARMBacklightSimulator:
    def __init__(self, boot_v: int = kBLValueMax, boot_millinits: int = kBLMilliNitsMax):
        self.last_v = boot_v
        self.last_millinits = boot_millinits
        self.panel_v = boot_v
        self.saved = None
        self.active_level = (boot_millinits * kBLLevelFull) // kBLMilliNitsMax

    def bl_take(self, dict_keys: dict, value_key: str | None, nits_key: str | None):
        nits = dict_keys.get(nits_key) if nits_key else None
        value = dict_keys.get(value_key) if value_key else None
        db_dict = dict_keys.get("DisplayBrightness") if isinstance(dict_keys.get("DisplayBrightness"), dict) else None
        slider_moved = False
        slider_pos = 0

        if value_key is not None and nits is None and value is None:
            nits = dict_keys.get("DisplayBrightnessNitsNVRAM")
            if nits is None and db_dict is not None:
                nits = db_dict.get("NitsPhysical") or db_dict.get("Nits")

        if value_key is not None:
            pos_obj = dict_keys.get("DisplayBrightnessSliderPosition")
            if pos_obj is None and db_dict is not None:
                pos_obj = db_dict.get("DisplayBrightnessSliderPosition")
            if pos_obj is not None:
                slider_pos = min(65536, max(0, int(pos_obj)))
                if slider_pos > 0 or (nits is not None and nits > 0):
                    slider_moved = True

        if slider_moved:
            millinits = kBLMilliNitsMin + slider_pos * (kBLMilliNitsMax - kBLMilliNitsMin) // 65536
            v = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) // (kBLMilliNitsMax - kBLMilliNitsMin)
        elif nits is not None:
            s16 = int(nits)
            n16 = s16
            if s16 <= 0:
                self.panel_v = 0
                return ("PANEL_OFF", self.last_millinits)
            if n16 >= 65536:
                db = dict_keys.get("DisplayBrightness")
                if isinstance(db, dict):
                    db = db.get("Brightness", 65536)
                elif db is None:
                    db = 65536
                if n16 == fixed_nits(kBLMilliNitsMid) and db < 49398:
                    millinits = kBLMilliNitsMin + db * (kBLMilliNitsMid - kBLMilliNitsMin) // 49398
                else:
                    max_fixed = kBLMilliNitsMax << 16
                    clamped_n16 = n16 if n16 < max_fixed else max_fixed
                    millinits = (clamped_n16 * 1000) >> 16
                millinits = max(kBLMilliNitsMin, min(kBLMilliNitsMax, millinits))
                v = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) // (kBLMilliNitsMax - kBLMilliNitsMin)
            elif 1 <= n16 <= kBLLevelFull:
                millinits = kBLMilliNitsMin + (n16 - 1) * (kBLMilliNitsMax - kBLMilliNitsMin) // (kBLLevelFull - 1)
                v = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) // (kBLMilliNitsMax - kBLMilliNitsMin)
            else:
                return ("DISCARDED_TRANSIENT", self.last_millinits)
        elif value is not None:
            if value == 0:
                self.panel_v = 0
                return ("PANEL_OFF", self.last_millinits)
            v = max(kBLValueMin, min(kBLValueMax, int(value)))
            millinits = kBLMilliNitsMin + (v - kBLValueMin) * (kBLMilliNitsMax - kBLMilliNitsMin) // (kBLValueMax - kBLValueMin)
        else:
            return ("NOTHING_TAKEN", self.last_millinits)

        self.panel_v = v
        self.last_v = v
        self.last_millinits = millinits
        self.saved = (v, millinits)
        self.active_level = (millinits * kBLLevelFull) // kBLMilliNitsMax
        return ("APPLIED", self.last_millinits)


def run_tests():
    print("=== EMPIRICAL CHALLENGE: MiniAppleARMBacklight ===")

    cpp_src = BACKLIGHT_CPP.read_text(encoding="utf-8")
    ramfs_src = RAMFS_DISK_INC.read_text(encoding="utf-8")

    print("[1] Checking source invariants in MiniAppleARMBacklight.cpp and ramfs_disk.inc...")
    assert "outside slider window ignored" not in cpp_src, "Stale slider window filter still in MiniAppleARMBacklight.cpp"
    assert '"annibale-bl-restored"' in cpp_src, "Missing annibale-bl-restored property"
    assert 'OSDynamicCast(OSDictionary, dict->getObject("DisplayBrightness"))' in cpp_src, (
        "Missing nested DisplayBrightness dictionary parsing"
    )
    assert "0x424c5332U" in ramfs_src, "Expected ANNFS_BL_MAGIC 0x424c5332U ('BLS2') in ramfs_disk.inc"
    print("  ✓ Source invariants verified")

    print("[2] Testing calibration parameters...")
    assert fixed_nits(kBLMilliNitsMin) == 327680
    assert fixed_nits(kBLMilliNitsMid) == 9175040
    assert fixed_nits(kBLMilliNitsMax) == 26214400
    print("  ✓ Calibration parameters correct")

    sim = MiniAppleARMBacklightSimulator()

    print("[3] Testing 1:1 framebuffer and nested DisplayBrightness synchronization...")
    action, current_nits = sim.bl_take({"IOMFBBrightnessLevel": fixed_nits(200000)}, None, "IOMFBBrightnessLevel")
    assert action == "APPLIED" and current_nits == 200000
    action, current_nits = sim.bl_take(
        {"DisplayBrightness": {"Brightness": 32768, "Nits": fixed_nits(300000), "DisplayBrightnessSliderPosition": 32768}},
        "brightness",
        "brightness-nits",
    )
    assert action == "APPLIED" and 200000 <= current_nits <= 205000
    print("  ✓ Framebuffer and nested DisplayBrightness updates synchronize 1:1 with panel")

    print("[4] Testing screen-off (s16 <= 0) preserves last user brightness...")
    saved_before = sim.saved
    action, current_nits = sim.bl_take({"IOMFBBrightnessLevel": 0}, None, "IOMFBBrightnessLevel")
    assert action == "PANEL_OFF"
    assert sim.panel_v == 0
    assert sim.saved == saved_before
    print("  ✓ Screen-off turns off panel without overwriting persisted brightness")

    print("[5] Testing edge cases & 5,000 randomized events...")
    assert sim.bl_take({"brightness-nits": 30000}, "brightness", "brightness-nits")[0] == "DISCARDED_TRANSIENT"
    assert sim.bl_take({}, "brightness", "brightness-nits")[0] == "NOTHING_TAKEN"
    for _ in range(5000):
        target_nits = random.randint(5, 400)
        res, val = sim.bl_take({"IOMFBBrightnessLevel": fixed_nits(target_nits * 1000)}, None, "IOMFBBrightnessLevel")
        assert res == "APPLIED"
        assert kBLMilliNitsMin <= val <= kBLMilliNitsMax
    print("ALL MiniAppleARMBacklight TESTS PASSED!")


if __name__ == "__main__":
    run_tests()

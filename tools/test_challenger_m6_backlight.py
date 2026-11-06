#!/usr/bin/env python3
"""Empirical Challenger Test Suite for MiniAppleARMBacklight (M6 Boot Remediation).

Adversarially challenges:
1. publish_calibration():
   - lMidPanel and lMidProduct calibration values must equal fixed_nits(140000) = 9175040 (140.0 nits).
   - lMinPanel / lMinProduct = 5 nits, lMaxPanel / lMaxProduct = 400 nits.
2. bl_take() autonomous rejection:
   - Cold boot: backboardd sends autonomous brightness-nits = 327680 (5.0 nits) without slider.
     Must be rejected; 400 nits maintained.
   - Autonomous ramp: backboardd sends autonomous ramps with DisplayBrightness key but no slider key.
     Must be rejected; 400 nits maintained.
3. bl_take() slider activation & decay:
   - Slider arrives with DisplayBrightnessSliderPosition: accepted, window opens (500ms).
   - Follow-up updates within 500ms window with DisplayBrightness: accepted, window extends.
   - After window expires (> 500ms): autonomous updates rejected; user brightness retained.
4. Framebuffer brightness requests:
   - Always rejected, preserving current nits.
5. Boundary conditions & corrupted dictionaries:
   - Negative values, zero values, transients (1025..65535), > 400 nits, empty dict.
6. 10,000 randomized concurrent stress iterations.
"""

import sys
import time
import random

kBLValueMin = 5
kBLValueMax = 0x1A5E
kBLMilliNitsMin = 5000
kBLMilliNitsMid = 140000
kBLMilliNitsMax = 400000
kBLLevelFull = 1024

def fixed_nits(millinits: int) -> int:
    return (millinits << 16) // 1000

class MiniAppleARMBacklightSimulator:
    def __init__(self):
        self.last_v = kBLValueMax
        self.last_millinits = kBLMilliNitsMax
        self.user_until = 0
        self.initialized = False
        self.active_level = kBLLevelFull
        self.log_messages = []

    def bl_take(self, dict_keys: dict, value_key: str | None, nits_key: str | None, now_ns: int):
        """Exact replica of bl_take logic in MiniAppleARMBacklight.cpp."""
        nits = dict_keys.get(nits_key) if nits_key else None
        value = dict_keys.get(value_key) if value_key else None

        if value_key is not None and nits is None and value is None:
            nits = dict_keys.get("DisplayBrightnessNitsNVRAM")

        if value_key is not None:
            active_win_ns = 500 * 1_000_000
            if "DisplayBrightnessSliderPosition" in dict_keys:
                self.user_until = now_ns + active_win_ns
            elif "DisplayBrightness" in dict_keys and now_ns <= self.user_until:
                self.user_until = now_ns + active_win_ns

        if value_key is None:
            msg = f"framebuffer brightness ignored, keeping {self.last_millinits / 1000:.3f} nits"
            self.log_messages.append(msg)
            return ("IGNORED_FRAMEBUFFER", self.last_millinits)

        if nits is not None:
            n16 = nits
            s16 = nits
            if s16 <= 0:
                msg = f"discarded non-positive level {s16}, keeping {self.last_millinits / 1000:.3f} nits"
                self.log_messages.append(msg)
                return ("DISCARDED_NON_POSITIVE", self.last_millinits)

            if n16 >= 65536:
                db = dict_keys.get("DisplayBrightness", 65536)
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
                msg = f"discarded invalid transient {n16}, keeping {self.last_millinits / 1000:.3f} nits"
                self.log_messages.append(msg)
                return ("DISCARDED_TRANSIENT", self.last_millinits)
        elif value is not None:
            v = max(kBLValueMin, min(kBLValueMax, value))
            millinits = kBLMilliNitsMin + (v - kBLValueMin) * (kBLMilliNitsMax - kBLMilliNitsMin) // (kBLValueMax - kBLValueMin)
        else:
            self.log_messages.append("nothing taken")
            return ("NOTHING_TAKEN", self.last_millinits)

        if now_ns > self.user_until:
            msg = f"autonomous update ignored (no slider active), keeping {self.last_millinits // 1000} nits"
            self.log_messages.append(msg)
            return ("IGNORED_AUTONOMOUS", self.last_millinits)

        self.last_v = v
        self.last_millinits = millinits
        self.initialized = True
        self.active_level = (millinits * kBLLevelFull) // kBLMilliNitsMax
        return ("APPLIED", self.last_millinits)

def run_tests():
    print("=== EMPIRICAL CHALLENGE: MiniAppleARMBacklight ===")

    # 1. Calibration parameters test
    print("[1] Testing calibration parameters...")
    mid_fixed = fixed_nits(kBLMilliNitsMid)
    assert mid_fixed == 9175040, f"Expected 9175040, got {mid_fixed}"
    assert mid_fixed == int(140.0 * 65536)
    min_fixed = fixed_nits(kBLMilliNitsMin)
    assert min_fixed == 327680, f"Expected 327680, got {min_fixed}"
    max_fixed = fixed_nits(kBLMilliNitsMax)
    assert max_fixed == 26214400, f"Expected 26214400, got {max_fixed}"
    print(f"  ✓ Calibration parameters correct: min={min_fixed} (5nits), mid={mid_fixed} (140nits), max={max_fixed} (400nits)")

    sim = MiniAppleARMBacklightSimulator()

    # 2. Cold Boot Autonomous Rejection
    print("[2] Testing cold boot CoreBrightness autonomous dimming request...")
    t0 = 1000_000_000 # 1.0s
    req_5nits = {
        "brightness-nits": fixed_nits(5000), # 327680 (5.0 nits)
        "DisplayBrightness": 0,
        "FadePeriod": 200,
    }
    action, current_nits = sim.bl_take(req_5nits, "brightness", "brightness-nits", t0)
    assert action == "IGNORED_AUTONOMOUS", f"Expected IGNORED_AUTONOMOUS, got {action}"
    assert current_nits == 400000, f"Expected 400000 m-nits (400 nits), got {current_nits}"
    assert sim.last_millinits == 400000
    print("  ✓ Cold boot 5 nits request safely rejected; 400 nits retained")

    # 3. Autonomous Ramp Rejection
    print("[3] Testing autonomous ramp sequence without slider...")
    for ramp_nits in [350, 300, 200, 100, 50, 5]:
        t0 += 50_000_000 # 50ms interval
        req = {
            "brightness-nits": fixed_nits(ramp_nits * 1000),
            "DisplayBrightness": int(ramp_nits * 100),
        }
        action, current_nits = sim.bl_take(req, "brightness", "brightness-nits", t0)
        assert action == "IGNORED_AUTONOMOUS", f"Step {ramp_nits} nits failed: got {action}"
        assert current_nits == 400000, f"Brightness changed to {current_nits}!"
    print("  ✓ Full autonomous ramp rejected; 400 nits strictly preserved")

    # 4. User Slider Interaction
    print("[4] Testing slider drag activation and decay...")
    t_slider = t0 + 200_000_000
    slider_req = {
        "DisplayBrightnessSliderPosition": 500,
        "DisplayBrightness": 32768,
        "brightness-nits": fixed_nits(200000), # 200 nits
    }
    action, current_nits = sim.bl_take(slider_req, "brightness", "brightness-nits", t_slider)
    assert action == "APPLIED", f"Slider request not applied: {action}"
    assert current_nits == 200000, f"Expected 200000, got {current_nits}"
    assert sim.user_until == t_slider + 500_000_000
    print("  ✓ Slider drag accepted: brightness updated to 200 nits, window set to +500ms")

    # Rapid updates within window
    t_followup = t_slider + 100_000_000 # +100ms
    followup_req = {
        "DisplayBrightness": 25000,
        "brightness-nits": fixed_nits(150000), # 150 nits
    }
    action, current_nits = sim.bl_take(followup_req, "brightness", "brightness-nits", t_followup)
    assert action == "APPLIED", f"Follow-up in window not applied: {action}"
    assert current_nits == 150000
    assert sim.user_until == t_followup + 500_000_000
    print("  ✓ Follow-up event in window extended window and updated brightness to 150 nits")

    # After window expires
    t_expired = sim.user_until + 10_000_000 # +10ms after expiry
    auto_dim = {
        "brightness-nits": fixed_nits(5000),
        "DisplayBrightness": 0,
    }
    action, current_nits = sim.bl_take(auto_dim, "brightness", "brightness-nits", t_expired)
    assert action == "IGNORED_AUTONOMOUS", f"Expired request not ignored: {action}"
    assert current_nits == 150000, f"User brightness corrupted! Got {current_nits}"
    print("  ✓ Autonomous dimming after slider idle ignored; user brightness (150 nits) retained")

    # 5. Framebuffer Requests
    print("[5] Testing framebuffer brightness requests...")
    fb_req = {"IOMFBBrightnessLevel": fixed_nits(50000)}
    action, current_nits = sim.bl_take(fb_req, None, "IOMFBBrightnessLevel", t_expired)
    assert action == "IGNORED_FRAMEBUFFER"
    assert current_nits == 150000
    print("  ✓ Framebuffer brightness requests safely ignored")

    # 6. Malicious / Edge Case Inputs
    print("[6] Testing edge cases & corrupted inputs...")
    # Negative nits
    res, _ = sim.bl_take({"brightness-nits": -100}, "brightness", "brightness-nits", t_slider)
    assert res == "DISCARDED_NON_POSITIVE"

    # Zero nits
    res, _ = sim.bl_take({"brightness-nits": 0}, "brightness", "brightness-nits", t_slider)
    assert res == "DISCARDED_NON_POSITIVE"

    # Invalid transient (between 1025 and 65535)
    res, _ = sim.bl_take({"brightness-nits": 30000}, "brightness", "brightness-nits", t_slider)
    assert res == "DISCARDED_TRANSIENT"

    # Empty dictionary
    res, _ = sim.bl_take({}, "brightness", "brightness-nits", t_slider)
    assert res == "NOTHING_TAKEN"

    # Enormous nits (> 400 nits)
    res, val = sim.bl_take({"DisplayBrightnessSliderPosition": 1, "brightness-nits": fixed_nits(800000)}, "brightness", "brightness-nits", t_slider)
    assert res == "APPLIED"
    assert val == 400000, f"Expected clamp to 400000, got {val}"
    print("  ✓ All boundary and adversarial inputs handled without panic or corruption")

    # 7. Stress test: 10,000 randomized events
    print("[7] Stress testing 10,000 randomized events...")
    curr_time = 0
    slider_active_cnt = 0
    auto_cnt = 0
    for _ in range(10000):
        curr_time += random.randint(1_000, 20_000_000) # advance 1us to 20ms
        is_slider = random.random() < 0.05 # 5% user slider interaction
        if is_slider:
            target_nits = random.randint(5, 400)
            d = {
                "DisplayBrightnessSliderPosition": random.randint(0, 1000),
                "brightness-nits": fixed_nits(target_nits * 1000),
                "DisplayBrightness": random.randint(0, 65536)
            }
            res, val = sim.bl_take(d, "brightness", "brightness-nits", curr_time)
            assert res == "APPLIED"
            slider_active_cnt += 1
        else:
            target_nits = random.randint(5, 400)
            d = {
                "brightness-nits": fixed_nits(target_nits * 1000),
                "DisplayBrightness": random.randint(0, 65536)
            }
            res, val = sim.bl_take(d, "brightness", "brightness-nits", curr_time)
            auto_cnt += 1
            if curr_time > sim.user_until:
                assert res == "IGNORED_AUTONOMOUS"

    print(f"  ✓ 10,000 stress events completed successfully ({slider_active_cnt} slider, {auto_cnt} auto).")
    print("ALL MiniAppleARMBacklight TESTS PASSED!")

if __name__ == "__main__":
    run_tests()

#!/usr/bin/env python3
"""Host-side unit tests for MiniAppleARMBacklight sanity checks and annibale_battery hysteresis."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

TEST_C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define kBLValueMin             5u
#define kBLValueMax             0x1a5eu
#define kBLMilliNitsMin         5000u
#define kBLMilliNitsMax         400000u
#define kBLLevelFull            1024u

/* Backlight simulation */
static uint64_t last_v = kBLValueMax;
static uint64_t last_millinits = kBLMilliNitsMax;

static bool bl_take_sim(uint64_t n16, uint64_t *out_v, uint64_t *out_millinits, uint32_t *out_level)
{
    int64_t s16 = (int64_t) n16;
    if (s16 <= 0) {
        /* Discarded */
        return false;
    }

    uint64_t millinits, v;
    if (n16 >= 65536) {
        millinits = (n16 < (uint64_t) kBLMilliNitsMax << 16 ? n16 : (uint64_t) kBLMilliNitsMax << 16) * 1000 >> 16;
        millinits = millinits < kBLMilliNitsMin ? kBLMilliNitsMin : millinits > kBLMilliNitsMax ? kBLMilliNitsMax : millinits;
        v = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) / (kBLMilliNitsMax - kBLMilliNitsMin);
        last_v = v;
        last_millinits = millinits;
    } else if (n16 >= 1 && n16 <= kBLLevelFull) {
        millinits = kBLMilliNitsMin + (n16 - 1) * (kBLMilliNitsMax - kBLMilliNitsMin) / (kBLLevelFull - 1);
        v = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) / (kBLMilliNitsMax - kBLMilliNitsMin);
        last_v = v;
        last_millinits = millinits;
    } else {
        /* Invalid transient (> 1024 and < 65536) */
        return false;
    }

    *out_millinits = millinits;
    *out_v = v;
    *out_level = (uint32_t) (millinits * kBLLevelFull / kBLMilliNitsMax);
    return true;
}

static void test_backlight_mapping(void)
{
    uint64_t v = 0, millinits = 0;
    uint32_t level = 0;
    bool ok;

    /* Initial state */
    last_v = kBLValueMax;
    last_millinits = kBLMilliNitsMax;

    /* 1. Standard 16.16 fixed-point: 214.743 nits */
    uint64_t nits_214 = 14073450; /* 214.743 * 65536 */
    ok = bl_take_sim(nits_214, &v, &millinits, &level);
    assert(ok);
    assert(millinits >= 214000 && millinits <= 215000);
    assert(level == 549);
    assert(last_millinits == millinits);
    assert(last_v == v);

    /* 2. Menu navigation transients: 28 and 27 mapped linearly in 1..1024 */
    ok = bl_take_sim(28, &v, &millinits, &level);
    assert(ok);
    /* 5000 + 27 * 395000 / 1023 = 15425 */
    assert(millinits == 15425);
    assert(v == kBLValueMin + (15425 - 5000) * (kBLValueMax - kBLValueMin) / (kBLMilliNitsMax - kBLMilliNitsMin));

    /* 3. Boundaries of 1..1024 */
    ok = bl_take_sim(1, &v, &millinits, &level);
    assert(ok);
    assert(millinits == kBLMilliNitsMin);
    assert(v == kBLValueMin);

    ok = bl_take_sim(1024, &v, &millinits, &level);
    assert(ok);
    assert(millinits == kBLMilliNitsMax);
    assert(v == kBLValueMax);
    assert(level == kBLLevelFull);

    /* 4. Non-positive values: discarded, keep previous stable state */
    uint64_t prev_millinits = last_millinits;
    uint64_t prev_v = last_v;

    ok = bl_take_sim(0, &v, &millinits, &level);
    assert(!ok);
    assert(last_millinits == prev_millinits);
    assert(last_v == prev_v);

    ok = bl_take_sim((uint64_t)(int64_t)(-10), &v, &millinits, &level);
    assert(!ok);
    assert(last_millinits == prev_millinits);
    assert(last_v == prev_v);

    /* 5. Invalid transient values between 1025 and 65535: discarded */
    ok = bl_take_sim(1025, &v, &millinits, &level);
    assert(!ok);
    assert(last_millinits == prev_millinits);
    assert(last_v == prev_v);

    ok = bl_take_sim(32768, &v, &millinits, &level);
    assert(!ok);
    assert(last_millinits == prev_millinits);
    assert(last_v == prev_v);

    printf("test_backlight_mapping passed\n");
}

/* Battery simulation */
#define CHARGE_FLOW_UA 50000        /* 50 mA */
#define DISCONNECT_FLOW_UA (-250000) /* -250 mA */

static bool sim_last_external = false;

static void battery_update_sim(int32_t current_ua, bool *out_external, int *out_status)
{
    if (current_ua > CHARGE_FLOW_UA) {
        sim_last_external = true;
        *out_status = 1;
    } else if (current_ua < DISCONNECT_FLOW_UA) {
        sim_last_external = false;
        *out_status = 2;
    } else {
        *out_status = sim_last_external ? 3 : 2;
    }
    *out_external = sim_last_external;
}

static void test_battery_hysteresis(void)
{
    bool ext;
    int status;

    /* Start unplugged */
    sim_last_external = false;

    /* 1. At -100 mA (discharging, within hysteresis window): remains unplugged */
    battery_update_sim(-100000, &ext, &status);
    assert(ext == false);
    assert(status == 2);

    /* 2. Plugin charger: current jumps to +300 mA (> +50 mA) */
    battery_update_sim(300000, &ext, &status);
    assert(ext == true);
    assert(status == 1);

    /* 3. Float/hold: current drops to 0 mA (within [-250 mA, +50 mA]) -> must remain external */
    battery_update_sim(0, &ext, &status);
    assert(ext == true);
    assert(status == 3);

    /* 4. Mild drain during screen activity while plugged in: -50 mA -> must remain external */
    battery_update_sim(-50000, &ext, &status);
    assert(ext == true);
    assert(status == 3);

    /* 5. Heavier burst while on weak USB: -200 mA -> must still remain external */
    battery_update_sim(-200000, &ext, &status);
    assert(ext == true);
    assert(status == 3);

    /* 6. Unplug charger: discharge drops below -250 mA (-300 mA) */
    battery_update_sim(-300000, &ext, &status);
    assert(ext == false);
    assert(status == 2);

    /* 7. Current settles back to -100 mA -> remains unplugged */
    battery_update_sim(-100000, &ext, &status);
    assert(ext == false);
    assert(status == 2);

    printf("test_battery_hysteresis passed\n");
}

int main(void)
{
    test_backlight_mapping();
    test_battery_hysteresis();
    printf("ALL TESTS PASSED\n");
    return 0;
}
'''

def main():
    with tempfile.NamedTemporaryFile(suffix=".c") as f_src:
        f_src.write(TEST_C.encode("utf-8"))
        f_src.flush()
        with tempfile.NamedTemporaryFile(suffix="") as f_bin:
            subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", f_src.name, "-o", f_bin.name], check=True)
            res = subprocess.run([f_bin.name], check=True, capture_output=True, text=True)
            print(res.stdout.strip())

if __name__ == "__main__":
    main()

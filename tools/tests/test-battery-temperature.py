#!/usr/bin/env python3
"""Compile battery sampling/publication and check temperature, failures and recovery."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BATTERY = "xnu/files/bsd/dev/annibale/annibale_battery.c"
STATE = "xnu/files/bsd/dev/annibale/annibale_battery_state.c"
IOKIT = "xnu/files/iokit/Kernel/AnnibaleBattery.cpp"


def function(source, name):
    match = re.search(r"^" + re.escape(name) + r"\(", source, re.M)
    start = match.start()
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def source(path, revision):
    if revision:
        return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{revision}:{path}"], text=True)
    return (ROOT / path).read_text()


def build(revision):
    battery = source(BATTERY, revision)
    state = source(STATE, revision)
    iokit = source(IOKIT, revision)
    header = (ROOT / "xnu/files/bsd/sys/annibale_input.h").read_text()
    struct = re.search(r"struct annibale_battery_state \{.*?\n\};", header, re.S).group()
    globals_ = re.findall(r"^static (?:int|uint64_t) battery_.*?;", state, re.M)
    code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFUnserialize.h>
''' + struct + r'''
static std::map<std::string, uint32_t> properties;
struct OSObject { virtual ~OSObject() {} void release() const { delete this; } };
struct OSSymbol : OSObject {
    std::string value;
    static OSSymbol *withCString(const char *s) { auto p = new OSSymbol; p->value = s; return p; }
};
struct OSNumber : OSObject {
    uint32_t value;
    static OSNumber *withNumber(unsigned long long n, unsigned bits) {
        assert(bits == 32); auto p = new OSNumber; p->value = (uint32_t)n; return p;
    }
};
struct OSDictionary : OSObject {
    static OSDictionary *withCapacity(unsigned) { return new OSDictionary; }
    void setObject(const char *, OSObject *) {}
};
static OSObject false_object, true_object;
#define kOSBooleanFalse (&false_object)
#define kOSBooleanTrue (&true_object)
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
#define kIOPMPSAdapterDetailsWattsKey "Watts"
#define kIOPMPSAdapterDetailsAmperageKey "Amperage"
#define kIOPMPSAdapterDetailsVoltage "Voltage"
#define kIOPMPSAdapterDetailsFamilyKey "Family"
#define kIOPMPSAdapterDetailsKey "AdapterDetails"
class AnnibaleBattery {
public:
    void number(const char *, long long);
    void report(const annibale_battery_state *);
    void flag(const char *, bool) {}
    void object(const char *, OSObject *) {}
    void setPSProperty(const OSSymbol *key, OSNumber *n) { properties[key->value] = n->value; }
    void setBatteryInstalled(bool) {} void setExternalConnected(bool) {}
    void setExternalChargeCapable(bool) {} void setIsCharging(bool) {}
    void setCurrentCapacity(unsigned) {} void setMaxCapacity(unsigned) {}
    void setVoltage(unsigned) {} void setAmperage(int) {} void setCycleCount(unsigned) {}
    void updateStatus() {}
};
''' + "\nvoid\n" + function(iokit, "AnnibaleBattery::number")
    code += "\nstatic unsigned\n" + function(iokit, "family_code")
    code += "\nvoid\n" + function(iokit, "AnnibaleBattery::report")
    code += r'''
static annibale_battery_state published;
static unsigned reports;
static void AnnibaleBatteryReport(const void *p, unsigned size) {
    assert(size == sizeof(published));
    published = *(const annibale_battery_state *)p;
    assert(published.size == size);
    AnnibaleBattery battery; battery.report(&published); reports++;
}
''' + "\n".join(globals_)
    code += "\nint\n" + function(state, "annibale_battery_temperature")
    code += "\nvoid\n" + function(state, "annibale_battery_report")
    code += r'''
#define SDA 0
#define SCL 1
#define OUTPUT (1u << 9)
#define CHARGE_FLOW_UA 50000
static uint32_t saved_cfg[2], saved_io[2], input_cfg[2];
static uintptr_t dwc3_dev;
static bool last_external;
static struct { bool found; } usbin_mod;
static struct { bool active; int32_t vbat_mv, ibat_ma, vbus_uv, chg_status, chg_type; } pmic_glink_state;
static uint16_t readings[8] = {60, 3900, (uint16_t)-200, 2981, 3600, 6000, 6000, 10};
static unsigned completed = 8, reads, restores, charging_calls;
static bool qbg_ok;
static uint16_t qbg_soc = 65, qbg_mv = 4100;
static uint32_t rd(unsigned, unsigned off) { return off == 4 ? 1 : 0; }
static void wr(unsigned, unsigned, uint32_t) { restores++; }
static void line(unsigned, bool) {}
static bool read_word(uint8_t reg, uint16_t *value) {
    const uint8_t expected[] = {0x2c, 0x08, 0x0c, 0x06, 0x10, 0x12, 0x3c, 0x2a};
    assert(reads < 8 && reg == expected[reads]);
    if (reads == completed) return false;
    *value = readings[reads++]; return true;
}
static bool qbg_read_fallback(uint16_t *soc, uint16_t *mv, int16_t *ma) {
    *soc = qbg_soc; *mv = qbg_mv; *ma = -250; return qbg_ok;
}
static bool spmi_read_reg(void *, unsigned, uint8_t *) { return false; }
static bool pm8550b_configure_charging(bool enable) {
    assert(enable); charging_calls++; return true;
}
'''
    code += "\nstatic bool\n" + function(battery, "sample")
    torch = (ROOT / "xnu/files/bsd/dev/annibale/annibale_torch.c").read_text()
    code += r'''
#define FLASH_INT_RT_STS 0
#define FLASH_STATUS1 1
static bool lit = true;
static const char *spmi_read16(unsigned, uint8_t *, unsigned) { return nullptr; }
static void say(const char *, ...) {}
static void dark_locked() { lit = false; }
'''
    code += "\nstatic bool\n" + function(torch, "watch_locked")
    code += r'''
static int32_t iokit_temperature() {
    assert(properties.count("Temperature") == 1);
    char xml[100];
    snprintf(xml, sizeof(xml), "<integer size=\"32\">0x%x</integer>", properties["Temperature"]);
    CFStringRef error = nullptr;
    CFTypeRef number = IOCFUnserialize(xml, kCFAllocatorDefault, 0, &error);
    assert(number && !error && CFGetTypeID(number) == CFNumberGetTypeID());
    int result = 0;
    assert(CFNumberGetValue((CFNumberRef)number, kCFNumberIntType, &result));
    CFRelease(number); return result;
}
static void check_sample(int32_t expected, int degrees) {
    reads = restores = 0;
    const unsigned before = reports;
    const unsigned previous_charging_calls = charging_calls;
    assert(sample());
    assert(reports == before + 1 && restores == 8);
    assert(charging_calls == previous_charging_calls + 1);
    assert(published.temperature_hundredths == expected);
    assert(iokit_temperature() == expected);
    assert(annibale_battery_temperature() == degrees);
    lit = true;
    assert(watch_locked() == (degrees <= 45));
    assert(lit == (degrees <= 45));
}
static void check_failure(unsigned expected_charging_calls = 0) {
    reads = restores = 0;
    const unsigned before = reports;
    const unsigned previous_charging_calls = charging_calls;
    const int temperature = annibale_battery_temperature();
    assert(!sample());
    assert(reports == before && restores == 8);
    assert(charging_calls == previous_charging_calls + expected_charging_calls);
    assert(annibale_battery_temperature() == temperature);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const int which = atoi(argv[1]);
    const uint16_t raw[] = {2981, 3081, 3082, 3182, 3187, 2681, 2730, 2331, 3582};
    const int expected[] = {2495, 3495, 3505, 4505, 4555, -505, -15, -4005, 8505};
    const int degrees[] = {25, 35, 35, 45, 46, -5, 0, -40, 85};
    assert(annibale_battery_temperature() == -273 && reports == 0);
    assert(watch_locked());
    if (which < 9) {
        readings[3] = raw[which]; check_sample(expected[which], degrees[which]);
    } else if (which < 13) {
        const uint16_t invalid[] = {2330, 3583, 0, UINT16_MAX};
        readings[3] = invalid[which - 9]; check_failure();
        readings[3] = 3187; check_sample(4555, 46);
    } else if (which == 13 || which == 14) {
        completed = which == 13 ? 4 : 6; qbg_ok = true;
        readings[3] = 2681; check_sample(-505, -5);
        assert(published.percent_hundredths == 6500 && published.voltage_uv == 4100000);
    } else if (which == 15) {
        completed = 4; pmic_glink_state.active = true; pmic_glink_state.vbat_mv = 4200;
        readings[3] = 3187; check_sample(4555, 46);
        assert(published.voltage_uv == 4200000);
    } else if (which == 16 || which == 17) {
        completed = which == 16 ? 0 : 3; qbg_ok = true;
        pmic_glink_state.active = true; pmic_glink_state.vbat_mv = 4200;
        check_failure(1); assert(properties.count("Temperature") == 0);
        completed = 8; readings[3] = 3187; check_sample(4555, 46);
        completed = 0; check_failure(1); assert(iokit_temperature() == 4555);
        completed = 8; readings[3] = 2981; check_sample(2495, 25);
    } else if (which == 18) {
        completed = 4; check_failure();
        qbg_ok = true; readings[3] = 3583; check_failure();
    } else if (which == 19) {
        readings[0] = 101; check_failure();
        readings[0] = 60; readings[1] = 2499; check_failure();
        readings[1] = 5001; check_failure();
    } else if (which == 20) {
        const int hundredths[] = {-49, -50, -149, -150, 49, 50, 149, 150};
        const int rounded[] = {0, -1, -1, -2, 0, 1, 1, 2};
        for (unsigned i = 0; i < 8; i++) {
            annibale_battery_state b = {}; b.size = sizeof(b);
            b.temperature_hundredths = hundredths[i]; annibale_battery_report(&b);
            assert(annibale_battery_temperature() == rounded[i]);
            assert(iokit_temperature() == hundredths[i]);
        }
    } else if (which == 21) {
        completed = 0; qbg_ok = true; qbg_soc = 101; check_failure();
        qbg_soc = 65; qbg_mv = 5001; check_failure();
        qbg_ok = false; pmic_glink_state.active = true;
        pmic_glink_state.vbat_mv = 5001; check_failure();
        pmic_glink_state.vbat_mv = 4200; check_failure(1);
        assert(properties.count("Temperature") == 0);
    } else {
        completed = 3; check_failure();
        pmic_glink_state.active = true; pmic_glink_state.vbat_mv = 4200;
        check_failure(1); assert(properties.count("Temperature") == 0);
        completed = 4; readings[3] = 3583; check_failure();
        readings[3] = 3187; check_sample(4555, 46);
    }
}
'''
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="compile sampling and publication from this Git revision")
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="battery-temperature-") as directory:
        path = Path(directory)
        cpp, exe = path / "check.cpp", path / "check"
        cpp.write_text(build(args.revision))
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", str(cpp),
                        "-framework", "CoreFoundation", "-framework", "IOKit", "-o", str(exe)], check=True)
        for case in range(23):
            result = subprocess.run([str(exe), str(case)], capture_output=True, text=True)
            if result.returncode:
                failures.append(case)
                print(f"case {case}: FAIL: {result.stderr.strip()}")
            elif case == 6 and "T=-0.15 C" not in result.stdout:
                failures.append(case)
                print("case 6: FAIL: negative sub-degree temperature missing from log")
    print(f"battery temperature: {23 - len(failures)}/23 cases passed; failed={failures}")
    raise SystemExit(bool(failures))


if __name__ == "__main__":
    main()

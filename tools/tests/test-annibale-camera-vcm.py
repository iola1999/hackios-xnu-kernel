#!/usr/bin/env python3
"""Host ASan+UBSan tests for rear VCM power sequencing, soft-landing walk, and quiesce park."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

KERNEL_ROOT = Path(__file__).resolve().parents[2]
DRIVER = KERNEL_ROOT / "xnu/files/bsd/dev/annibale"
WORKER = KERNEL_ROOT.parent / "hackios-xnu-userland/userland/src/services/camera_device_worker.inc"


def extract_function(source, name):
    matches = list(re.finditer(r"static (?:bool|void|int|uint32_t)\s+" + name + r"\(", source))
    if len(matches) != 1:
        raise AssertionError("expected one production function: " + name)
    start = matches[0].start()
    at = source.index("{", matches[0].end()) + 1
    depth = 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return source[start:at]


HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "annibale_camera.h"
#include "vcm-defs.inc"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define NSEC_PER_USEC 1000u
#define NSEC_PER_MSEC 1000000u
#define ANNIBALE_RPMPD_LEVEL_NOM 1u
#define ANNIBALE_RPMH_MODE_NONE 0u
#define ANNIBALE_RPMH_MODE_AUTO 3u
#define ANNIBALE_RPMH_MODE_HPM  4u
static inline void say(const char *fmt, ...) { (void)fmt; }

typedef uintptr_t vm_offset_t;

static vm_offset_t mclkcc_base = 1, tlmm_base = 2, cci0_base = 3;
static uint32_t boot_cam_bcm;
static uint8_t wide_bus_mode = 1u;
static bool cci_ready = true;
static struct annibale_camera_profile *cam_profile;
static uint32_t cam_native_focus_steps;

struct reg_event {
    char name[16];
    uint32_t mv;
    bool enable;
};
static struct reg_event reg_events[64];
static unsigned reg_event_count;
static unsigned ldob8_on, ldob8_off;

struct bus_write {
    uint8_t slave, b0, b1, b2;
    uint32_t len;
};
static struct bus_write bus_writes[256];
static unsigned bus_write_count;
static uint32_t delay_calls, last_delay_us, spin_calls, last_spin_us;
static uint32_t cci_q_words[4], cci_q_count;

static uint32_t reg_rd(vm_offset_t base, uint32_t off)
{
    if (base == cci0_base && off == CCI_IRQ_STATUS_0) {
        return CCI_IRQ_M1_Q0_REPORT;
    }
    if (base == tlmm_base && (off & 0xfffu) == 4u) {
        return 2u;
    }
    return 0u;
}

static void reg_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
    if (base == cci0_base) {
        if (off == CCI_I2C_M1_Q0_LOAD_DATA) {
            CHECK(cci_q_count < 4u);
            cci_q_words[cci_q_count++] = val;
        } else if (off == CCI_QUEUE_START) {
            CHECK(cci_q_count == 3u && val == (1u << 2));
            CHECK(bus_write_count < 256u);
            uint32_t sp = cci_q_words[0], wr = cci_q_words[1];
            bus_writes[bus_write_count++] = (struct bus_write){
                .slave = (uint8_t)((sp >> 4) & 0x7fu),
                .b0 = (uint8_t)((wr >> 8) & 0xffu),
                .b1 = (uint8_t)((wr >> 16) & 0xffu),
                .b2 = (uint8_t)((wr >> 24) & 0xffu),
                .len = (wr >> 4) & 0xfu,
            };
            cci_q_count = 0u;
        }
    }
}

static void delay_for_interval(uint32_t n, uint32_t unit)
{
    delay_calls++;
    last_delay_us = unit == NSEC_PER_MSEC ? n * 1000u : n;
}

static void cam_return_spin_us(uint32_t us)
{
    spin_calls++;
    last_spin_us = us;
}

static bool annibale_rpmh_arc_vote(const char *n, uint32_t lvl) { (void)n; (void)lvl; return true; }
static bool annibale_rpmh_bcm_vote(const char *n, uint64_t bw, uint32_t f) { (void)n; (void)bw; (void)f; return true; }
static bool annibale_rpmh_regulator(const char *name, uint32_t mv, bool enable, uint32_t mode)
{
    (void)mode;
    CHECK(reg_event_count < 64u);
    snprintf(reg_events[reg_event_count].name, sizeof(reg_events[0].name), "%s", name);
    reg_events[reg_event_count].mv = mv;
    reg_events[reg_event_count].enable = enable;
    reg_event_count++;
    if (strcmp(name, "ldob8") == 0) {
        if (enable) ldob8_on++; else ldob8_off++;
    }
    return true;
}

static bool cam_native_rcg_write(vm_offset_t b, uint32_t c, uint32_t cfg, uint32_t r) { (void)b; (void)c; (void)cfg; (void)r; return true; }
static bool cbcr_enable(vm_offset_t b, uint32_t o, bool c) { (void)b; (void)o; (void)c; return true; }
static void tlmm_config_pin(uint32_t p, uint32_t f, uint32_t pl, uint32_t d, bool oe, bool oh) { (void)p; (void)f; (void)pl; (void)d; (void)oe; (void)oh; }
static void cam_mux_front_se9_od(void) {}
static void cam_spmi_front_ldo_probe(const char *tag) { (void)tag; }

static bool wide_bus_write(uint8_t slave7, uint8_t b0, uint8_t b1, uint8_t b2, uint32_t len)
{
    CHECK(bus_write_count < 256u);
    bus_writes[bus_write_count++] = (struct bus_write){ slave7, b0, b1, b2, len };
    return true;
}

static const struct annibale_camera_power *mock_up_seq, *mock_down_seq;
static const struct annibale_camera_power *cam_profile_power(bool up) { return up ? mock_up_seq : mock_down_seq; }

#include "annibale_camera_exposure.inc"
#include "vcm-prod.inc"

static void reset_state(void)
{
    reg_event_count = ldob8_on = ldob8_off = 0u;
    bus_write_count = delay_calls = last_delay_us = spin_calls = last_spin_us = 0u;
    cci_q_count = 0u;
    cam_vaf_powered = false;
    cam_native_lens_ready = false;
    cam_native_applied_focus = ANNIBALE_CAMERA_NO_FOCUS;
    cam_native_focus_steps = 0u;
    cci_ready = true;
    wide_bus_mode = 1u;
    cam_native_exposure_reset();
}

int main(void)
{
    static struct annibale_camera_profile back_prof = {
        .sensor = 1u, .mode = 24u, .width = 2560u, .height = 1920u,
        .up_count = 4u, .down_count = 4u,
    };
    static const struct annibale_camera_power back_up[4] = {
        { .type = 14u, .value = 3296000u, .delay_ms = 2u },
        { .type = 3u,  .value = 1u,       .delay_ms = 1u },
        { .type = 4u,  .value = 1u,       .delay_ms = 0u },
        { .type = 2u,  .value = 1u,       .delay_ms = 1u },
    };
    static const struct annibale_camera_power back_down[4] = {
        { .type = 2u,  .value = 0u,       .delay_ms = 1u },
        { .type = 4u,  .value = 0u,       .delay_ms = 1u },
        { .type = 3u,  .value = 0u,       .delay_ms = 1u },
        { .type = 14u, .value = 0u,       .delay_ms = 1u },
    };
    static struct annibale_camera_profile front_prof = {
        .sensor = 2u, .mode = 4u, .width = 2592u, .height = 1458u,
        .up_count = 2u, .down_count = 2u,
    };
    static const struct annibale_camera_power front_up[2] = {
        { .type = 3u, .value = 1u, .delay_ms = 1u },
        { .type = 1u, .value = 1u, .delay_ms = 1u },
    };
    static const struct annibale_camera_power front_down[2] = {
        { .type = 1u, .value = 0u, .delay_ms = 1u },
        { .type = 3u, .value = 0u, .delay_ms = 1u },
    };

    /* 1. Front camera session must never power ldob8 (rear VCM AF). */
    reset_state();
    cam_profile = &front_prof;
    mock_up_seq = front_up;
    mock_down_seq = front_down;
    cam_vote_rpmh_supplies(true);
    CHECK(ldob8_on == 0u && ldob8_off == 0u && !cam_vaf_powered);
    CHECK(cam_native_sensor_power(false));
    CHECK(cam_native_sensor_power(true));
    CHECK(cam_native_sensor_power(false));
    cam_vote_rpmh_supplies(false);
    CHECK(ldob8_on == 0u && ldob8_off == 0u && !cam_vaf_powered);

    /* 2. Rear camera session: ldob8 powers ON once and OFF once (no open->off->on glitch). */
    reset_state();
    cam_profile = &back_prof;
    mock_up_seq = back_up;
    mock_down_seq = back_down;
    cam_native_exposure_defaults(&back_prof);
    CHECK(cam_native_focus == 275u);
    cam_vote_rpmh_supplies(true);
    CHECK(ldob8_on == 0u && ldob8_off == 0u && !cam_vaf_powered);
    CHECK(cam_native_sensor_power(false));
    CHECK(ldob8_on == 0u && ldob8_off == 0u && !cam_vaf_powered);
    CHECK(cam_native_sensor_power(true));
    CHECK(ldob8_on == 1u && ldob8_off == 0u && cam_vaf_powered);

    /* 3. Lens up initializes GT9764V, syncs DAC=0, and walks in <=32-code steps to 275. */
    cam_native_lens_up();
    CHECK(cam_native_lens_ready && cam_native_applied_focus == 275u);
    CHECK(bus_write_count >= 5u);
    CHECK(bus_writes[0].b0 == 0x02u && bus_writes[0].b1 == 0x02u);
    CHECK(bus_writes[1].b0 == 0x06u && bus_writes[1].b1 == 0x40u);
    CHECK(bus_writes[2].b0 == 0x07u && bus_writes[2].b1 == 0x07u);
    CHECK(bus_writes[3].b0 == 0x0bu && bus_writes[3].b1 == 0x03u);
    CHECK(bus_writes[4].b0 == 0x03u && bus_writes[4].b1 == 0x00u && bus_writes[4].b2 == 0x00u);
    uint32_t prev = 0u;
    for (unsigned i = 5u; i < bus_write_count; i++) {
        CHECK(bus_writes[i].slave == OVX8000_LENS_SLAVE && bus_writes[i].b0 == 0x03u);
        uint32_t pos = ((uint32_t)bus_writes[i].b1 << 8) | bus_writes[i].b2;
        CHECK(pos > prev && (pos - prev) <= 32u);
        prev = pos;
    }
    CHECK(prev == 275u);

    /* 4. Focus set clamps at calibrated upper bound 700, and reset restores 275. */
    CHECK(cam_native_focus_set(&back_prof, 900u));
    CHECK(cam_native_focus == 700u);
    cam_native_apply_focus(false);
    CHECK(cam_native_applied_focus == 700u);

    /* 5. Lens down walks smoothly with <=16-code soft-landing below 64, then writes 0x02=0x01 + 200us. */
    bus_write_count = 0u;
    cam_native_lens_down();
    CHECK(!cam_native_lens_ready && cam_native_applied_focus == ANNIBALE_CAMERA_NO_FOCUS);
    CHECK(bus_write_count >= 2u);
    prev = 700u;
    for (unsigned i = 0u; i + 1u < bus_write_count; i++) {
        CHECK(bus_writes[i].b0 == 0x03u);
        uint32_t pos = ((uint32_t)bus_writes[i].b1 << 8) | bus_writes[i].b2;
        uint32_t max_step = prev <= 64u ? 16u : 32u;
        CHECK(pos < prev && (prev - pos) <= max_step);
        prev = pos;
    }
    CHECK(prev == 0u);
    CHECK(bus_writes[bus_write_count - 1u].b0 == 0x02u && bus_writes[bus_write_count - 1u].b1 == 0x01u);
    CHECK(last_delay_us == 200u);
    CHECK(cam_native_sensor_power(false));
    CHECK(ldob8_on == 1u && ldob8_off == 1u && !cam_vaf_powered);
    cam_vote_rpmh_supplies(false);
    CHECK(ldob8_on == 1u && ldob8_off == 1u && !cam_vaf_powered);

    /* Stale focus does not leak into next session. */
    cam_native_exposure_reset();
    CHECK(cam_native_focus == 275u);

    /* 6. Lockless return_quiesce lens park walks to 0 and writes standby 0x02=0x01 via spin delays. */
    reset_state();
    cam_profile = &back_prof;
    cam_native_lens_ready = true;
    cam_native_applied_focus = 96u;
    cam_return_lens_park();
    CHECK(!cam_native_lens_ready && cam_native_applied_focus == ANNIBALE_CAMERA_NO_FOCUS);
    CHECK(delay_calls == 0u && spin_calls >= 6u && last_spin_us == 200u);
    CHECK(bus_write_count == 6u);
    const uint32_t expected_park[5] = { 64u, 48u, 32u, 16u, 0u };
    for (unsigned i = 0u; i < 5u; i++) {
        CHECK(bus_writes[i].slave == OVX8000_LENS_SLAVE && bus_writes[i].b0 == 0x03u);
        uint32_t pos = ((uint32_t)bus_writes[i].b1 << 8) | bus_writes[i].b2;
        CHECK(pos == expected_park[i]);
    }
    CHECK(bus_writes[5].b0 == 0x02u && bus_writes[5].b1 == 0x01u);

    printf("all 6 VCM power/walk/clamp/reset/quiesce scenarios passed\n");
    return 0;
}
"""


def main():
    hw = (DRIVER / "annibale_camera_hw.inc").read_text()
    clock = (DRIVER / "annibale_camera_clock.inc").read_text()
    cam = (DRIVER / "annibale_camera.c").read_text()
    worker = WORKER.read_text()

    if "#define AF_FAR          700" not in worker:
        raise AssertionError("camera_device_worker.inc AF_FAR must be clamped to 700")
    if "#define AF_NEAR         16" not in worker:
        raise AssertionError("camera_device_worker.inc AF_NEAR must stay off 0 mechanical stop")

    hw_defs = "\n".join(
        line for line in hw.splitlines()
        if line.startswith(("#define GPIO_", "#define MCLKCC_", "#define CCI_"))
    )
    vrm_start = hw.index("static bool cam_vaf_powered")
    vrm_end = hw.index("static inline uint32_t")
    vrm_block = hw[vrm_start:vrm_end]
    clock_defs = "\n".join(
        line for line in clock.splitlines()
        if line.startswith(("#define CAM_RCG_FIELDS", "#define OVX8000_LENS_"))
    )
    defs = "\n".join([hw_defs, clock_defs])
    prod = "\n".join([
        vrm_block,
        "static bool cam_native_lens_ready;",
        extract_function(hw, "cam_vote_rpmh_supplies"),
        extract_function(clock, "cam_native_sensor_power"),
        extract_function(clock, "cam_native_lens_next_step"),
        extract_function(clock, "cam_native_lens_walk"),
        extract_function(clock, "cam_native_apply_focus"),
        extract_function(clock, "cam_native_lens_up"),
        extract_function(clock, "cam_native_lens_down"),
        extract_function(cam, "cam_return_cci0_write"),
        extract_function(cam, "cam_return_lens_park"),
    ])

    with tempfile.TemporaryDirectory(prefix="annibale-camera-vcm-") as tmp:
        d = Path(tmp)
        (d / "sys").mkdir()
        (d / "sys/ioccom.h").write_text("#define _IOR(g, n, t) 0\n")
        shutil.copyfile(KERNEL_ROOT / "xnu/files/bsd/sys/annibale_camera.h", d / "annibale_camera.h")
        shutil.copyfile(DRIVER / "annibale_camera_exposure.inc", d / "annibale_camera_exposure.inc")
        (d / "vcm-defs.inc").write_text(defs)
        (d / "vcm-prod.inc").write_text(prod)
        (d / "harness.c").write_text(HARNESS)
        binary = d / "test_vcm"
        cmd = [
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(d), str(d / "harness.c"), "-o", str(binary),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if res.returncode != 0:
            raise AssertionError(res.stderr)
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        if run.returncode != 0:
            raise AssertionError(run.stderr or run.stdout)
        print(run.stdout.strip())


if __name__ == "__main__":
    main()

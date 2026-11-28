#!/usr/bin/env python3
"""Host ASan+UBSan unit test for MiniFramebufferDpuScale.inc (SM8750 DPU 12.0 QSEED3LITE scaling).

Compiles MiniFramebufferDpuScale.inc directly against a mock MMIO register space and verifies:
  1. fb_dpu_scale=0 default leaves all registers untouched and rejects scaling.
  2. 1206x2622 -> 1156x2510 (BGRA8888): dual-VIG split (603+603 -> 578+578), QSEED3LITE
     phase_step_h/v, seam horizontal init_phase continuity, preload, op_mode (8-bit BIT(14)),
     SW_PIX_EXT_*_REQ_PIXELS, and fast-path 0-MMIO-write repeat frame.
  3. 1206x2622 -> 1156x2510 (l10r BGR10A2): SSPP_SRC_FORMAT DX bit (0x000277ff),
     QSEED3_OP_MODE 10-bit (BIT(14) cleared), unpack_pattern preserved.
  4. Fallback guards (upscaling, partial dst rect, oversized stride, unsupported fourcc).
  5. Quiesce and poweroff restore exact saved boot 1156x2510 1:1 registers and downscale/convert
     active l10r/BGRA frame into buffer A (0xfc800000).
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INC_DIR = ROOT / "xnu/files/iokit/Kernel"

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_DPU_SCALE_HOST_TEST 1

/* Mock constants matching MiniFramebufferFlip.inc */
#define FLIP_MDP                0x1000u
#define FLIP_SSPP_SRC_SIZE      0x00u
#define FLIP_SSPP_SRC_XY        0x08u
#define FLIP_SSPP_OUT_SIZE      0x0cu
#define FLIP_SSPP_OUT_XY        0x10u
#define FLIP_SSPP_SRC0_ADDR     0x14u
#define FLIP_SSPP_YSTRIDE0      0x24u
#define FLIP_SSPP_SRC_FORMAT    0x30u
#define FLIP_SSPP_OP_MODE       0x38u
#define FLIP_PIPES              2u

static const struct {
    const char *name;
    uint32_t base;
    uint32_t flush;
} flipPipe[FLIP_PIPES] = {
    { "vig0", 0x4000u, 1u << 0 },
    { "vig1", 0x6000u, 1u << 1 },
};

static struct {
    uint32_t pipes;
    uint32_t offset[FLIP_PIPES];
    uint8_t *va[2];
    uint64_t pa_mock;
    uint32_t bytes, slot_bytes, rows;
    int shown;
    bool ready, drew;
    int32_t band0, band1;
} flip;

static uint32_t gDpuScale = 0;

/* Mock 64 KiB MMIO space covering FLIP_MDP + 0x4000..0x7fff */
#define MOCK_MMIO_WORDS (0x10000u / 4u)
static uint32_t gMmio[MOCK_MMIO_WORDS];
static uint32_t gWriteCount = 0;
static int gPointCalls = 0;
static int gLastPointBuf = -1;

static uint32_t flip_rd(uint32_t off) {
    assert((off & 3u) == 0 && (off / 4u) < MOCK_MMIO_WORDS);
    return gMmio[off / 4u];
}

static void flip_wr(uint32_t off, uint32_t val) {
    assert((off & 3u) == 0 && (off / 4u) < MOCK_MMIO_WORDS);
    gMmio[off / 4u] = val;
    gWriteCount++;
}

static void flip_point(int which) {
    gPointCalls++;
    gLastPointBuf = which;
    flip.shown = which;
}

static bool flip_taken(uint32_t limit_us, bool may_sleep) {
    (void)limit_us;
    (void)may_sleep;
    return true;
}

static uint64_t gScanoutPa = ~0ULL;
static uint32_t gScanoutW = 99u, gScanoutH = 99u, gScanoutStride = 99u, gScanoutFmt = 99u;

static void loader_set_scanout_mode(uint64_t pa, uint32_t w, uint32_t h, uint32_t stride, uint32_t fmt) {
    gScanoutPa = pa;
    gScanoutW = w;
    gScanoutH = h;
    gScanoutStride = stride;
    gScanoutFmt = fmt;
}

static void loader_set_scanout_pa(uint64_t pa) {
    loader_set_scanout_mode(pa, 0, 0, 0, 0);
}

#include "MiniFramebufferDpuScale.inc"

static void seed_k90_boot_regs(void) {
    memset(gMmio, 0, sizeof(gMmio));
    gWriteCount = 0;
    gPointCalls = 0;
    gLastPointBuf = -1;

    /* Exact K90 SM8750 boot state from b5r61/run/cmd-1.log */
    const uint32_t sz_1156_half = (2510u << 16) | 578u; /* 0x09ce0242 */
    for (unsigned p = 0; p < 2; p++) {
        uint32_t b = FLIP_MDP + flipPipe[p].base;
        uint32_t q = b + FLIP_QSEED3_OFFSET;
        gMmio[(b + FLIP_SSPP_SRC_SIZE) / 4u] = sz_1156_half;
        gMmio[(b + FLIP_SSPP_SRC_XY) / 4u] = (p == 0) ? 0u : 578u;
        gMmio[(b + FLIP_SSPP_OUT_SIZE) / 4u] = sz_1156_half;
        gMmio[(b + FLIP_SSPP_OUT_XY) / 4u] = (p == 0) ? 0u : 578u;
        gMmio[(b + FLIP_SSPP_YSTRIDE0) / 4u] = 1156u * 4u; /* 0x1210 */
        gMmio[(b + FLIP_SSPP_SRC_FORMAT) / 4u] = 0x000237ffu;
        gMmio[(b + FLIP_SSPP_SRC_UNPACK_PATTERN) / 4u] = 0x03020001u;
        gMmio[(b + FLIP_SSPP_OP_MODE) / 4u] = 0x80000000u;
        gMmio[(b + FLIP_SSPP_SW_PIX_EXT_C0_REQ) / 4u] = sz_1156_half;
        gMmio[(b + FLIP_SSPP_SW_PIX_EXT_C1C2_REQ) / 4u] = sz_1156_half;
        gMmio[(b + FLIP_SSPP_SW_PIX_EXT_C3_REQ) / 4u] = sz_1156_half;
        gMmio[(q + FLIP_QSEED3_OP_MODE) / 4u] = 0x00004400u;
    }

    memset(&flip, 0, sizeof(flip));
    flip.pipes = 0x3u;
    flip.offset[0] = 0;
    flip.offset[1] = 0;
    flip.bytes = 1156u * 4u * 2510u;
    flip.slot_bytes = FLIP_DPU_SCALE_SLOT_BYTES;
    flip.rows = 2510u;
    flip.ready = true;
}

static void test_default_off(void) {
    seed_k90_boot_regs();
    uint32_t snapshot[MOCK_MMIO_WORDS];
    memcpy(snapshot, gMmio, sizeof(snapshot));

    gDpuScale = 0;
    dpu_scale_init();
    assert(!dpuScale.capable);
    assert(!dpuScale.active);
    assert(!flip_config_scale(1206, 2622, 1206 * 4, FLIP_FOURCC_BGRA));
    assert(gWriteCount == 0);
    assert(memcmp(snapshot, gMmio, sizeof(snapshot)) == 0);
    printf("  [PASS] default fb_dpu_scale=0 leaves MMIO untouched\n");
}

static void test_bgra_and_l10r_programming(void) {
    seed_k90_boot_regs();
    uint32_t boot_snapshot[MOCK_MMIO_WORDS];
    memcpy(boot_snapshot, gMmio, sizeof(boot_snapshot));

    gDpuScale = 1;
    dpu_scale_init();
    assert(dpuScale.capable);
    assert(dpuScale.dst_w[0] == 578 && dpuScale.dst_w[1] == 578 && dpuScale.dst_h == 2510);

    /* 1) BGRA8888 1206x2622 with 64-byte aligned stride 4864 -> 1156x2510 */
    uint32_t stride = 4864u;
    assert(flip_config_scale(1206, 2622, stride, FLIP_FOURCC_BGRA));
    assert(dpuScale.active);
    uint32_t writes_first = gWriteCount;
    assert(writes_first > 0);

    uint32_t expected_step_h = (uint32_t)(((uint64_t)1206 << 21) / 1156); /* 2187859 */
    uint32_t expected_step_v = (uint32_t)(((uint64_t)2622 << 21) / 2510); /* 2190730 */
    int64_t init_x0 = (int64_t)(expected_step_h - (1u << 21)) / 2;
    int64_t seam_pos = init_x0 + 578LL * (int64_t)expected_step_h;
    assert((seam_pos >> 21) == 603);
    uint32_t expected_init_x0 = (uint32_t)init_x0;
    uint32_t expected_init_x1 = (uint32_t)(seam_pos - (603LL << 21));
    uint32_t expected_init_y = (uint32_t)((int64_t)(expected_step_v - (1u << 21)) / 2);

    /* Verify vig0 (left 603 -> 578) and vig1 (right 603 -> 578) */
    for (unsigned p = 0; p < 2; p++) {
        uint32_t b = FLIP_MDP + flipPipe[p].base;
        uint32_t q = b + FLIP_QSEED3_OFFSET;
        uint32_t exp_src_sz = (2622u << 16) | 603u;
        uint32_t exp_dst_sz = (2510u << 16) | 578u;
        assert(flip_rd(b + FLIP_SSPP_SRC_SIZE) == exp_src_sz);
        assert(flip_rd(b + FLIP_SSPP_SRC_XY) == (p == 0 ? 0u : 603u));
        assert(flip_rd(b + FLIP_SSPP_OUT_SIZE) == exp_dst_sz);
        assert(flip_rd(b + FLIP_SSPP_OUT_XY) == (p == 0 ? 0u : 578u));
        assert(flip_rd(b + FLIP_SSPP_YSTRIDE0) == stride);
        assert(flip_rd(b + FLIP_SSPP_SRC_FORMAT) == 0x000237ffu);
        assert(flip_rd(b + FLIP_SSPP_SRC_UNPACK_PATTERN) == 0x03020001u);
        assert(flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C0_REQ) == exp_src_sz);
        assert(flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C1C2_REQ) == exp_src_sz);
        assert(flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C3_REQ) == exp_src_sz);
        assert(flip_rd(q + FLIP_QSEED3_PHASE_STEP_Y_H) == expected_step_h);
        assert(flip_rd(q + FLIP_QSEED3_PHASE_STEP_Y_V) == expected_step_v);
        assert(flip_rd(q + FLIP_QSEED3_PHASE_INIT_Y_H) == (p == 0 ? expected_init_x0 : expected_init_x1));
        assert(flip_rd(q + FLIP_QSEED3_PHASE_INIT_Y_V) == expected_init_y);
        assert(flip_rd(q + FLIP_QSEED3_PRELOAD) == 0x02040204u);
        assert(flip_rd(q + FLIP_QSEED3_SRC_SIZE_Y_RGB_A) == exp_src_sz);
        assert(flip_rd(q + FLIP_QSEED3_DST_SIZE) == exp_dst_sz);
        assert(flip_rd(q + FLIP_QSEED3_OP_MODE) == 0xa0034401u);
    }

    /* Verify seam step continuity: (603 + init_x1/2^21) - (init_x0/2^21 + 577*step_h/2^21) == step_h */
    int64_t left_last = (int64_t)expected_init_x0 + 577LL * (int64_t)expected_step_h;
    int64_t right_first = (603LL << 21) + (int64_t)expected_init_x1;
    assert(right_first - left_last == (int64_t)expected_step_h);

    /* 2) Repeat identical config must be a 0-MMIO-write no-op */
    assert(flip_config_scale(1206, 2622, stride, FLIP_FOURCC_BGRA));
    assert(gWriteCount == writes_first);

    /* 3) Switch to l10r (BGR10A2): DX bit set in SRC_FORMAT, 8-bit BIT(14) cleared in QSEED3_OP_MODE */
    assert(flip_config_scale(1206, 2622, stride, FLIP_FOURCC_L10R));
    assert(gWriteCount > writes_first);
    for (unsigned p = 0; p < 2; p++) {
        uint32_t b = FLIP_MDP + flipPipe[p].base;
        uint32_t q = b + FLIP_QSEED3_OFFSET;
        assert(flip_rd(b + FLIP_SSPP_SRC_FORMAT) == 0x000277ffu);
        assert(flip_rd(b + FLIP_SSPP_SRC_UNPACK_PATTERN) == 0x03020001u);
        assert(flip_rd(q + FLIP_QSEED3_OP_MODE) == 0xa0030401u);
    }

    /* 4) Restore boot registers bit-for-bit */
    flip_restore_boot_scale();
    assert(!dpuScale.active);
    assert(memcmp(boot_snapshot, gMmio, sizeof(boot_snapshot)) == 0);
    printf("  [PASS] BGRA + l10r QSEED3LITE programming, seam continuity, cache hit, boot restore\n");
}

static void test_guards_and_quiesce(void) {
    seed_k90_boot_regs();
    uint32_t boot_snapshot[MOCK_MMIO_WORDS];
    memcpy(boot_snapshot, gMmio, sizeof(boot_snapshot));

    /* Small synthetic geometry (12x8 -> 8x6, split 4+4) to exercise pixel copy + l10r quiesce */
    const uint32_t sz_small = (6u << 16) | 4u;
    for (unsigned p = 0; p < 2; p++) {
        uint32_t b = FLIP_MDP + flipPipe[p].base;
        gMmio[(b + FLIP_SSPP_SRC_SIZE) / 4u] = sz_small;
        gMmio[(b + FLIP_SSPP_SRC_XY) / 4u] = (p == 0) ? 0u : 4u;
        gMmio[(b + FLIP_SSPP_OUT_SIZE) / 4u] = sz_small;
        gMmio[(b + FLIP_SSPP_OUT_XY) / 4u] = (p == 0) ? 0u : 4u;
        gMmio[(b + FLIP_SSPP_YSTRIDE0) / 4u] = 8u * 4u;
        gMmio[(b + FLIP_SSPP_SW_PIX_EXT_C0_REQ) / 4u] = sz_small;
        gMmio[(b + FLIP_SSPP_SW_PIX_EXT_C1C2_REQ) / 4u] = sz_small;
        gMmio[(b + FLIP_SSPP_SW_PIX_EXT_C3_REQ) / 4u] = sz_small;
    }
    memcpy(boot_snapshot, gMmio, sizeof(boot_snapshot));

    uint8_t bufA[512], bufB[512], surf[12 * 8 * 4];
    memset(bufA, 0xaa, sizeof(bufA));
    memset(bufB, 0xbb, sizeof(bufB));
    flip.va[0] = bufA;
    flip.va[1] = bufB;
    flip.bytes = 8u * 4u * 6u;
    flip.slot_bytes = sizeof(bufA);
    flip.rows = 6u;
    flip.shown = 0;

    gDpuScale = 1;
    dpu_scale_init();
    assert(dpuScale.capable);

    /* Reject upscaling, partial rect, unsupported fourcc, oversized stride */
    assert(!flip_config_scale(6, 8, 6 * 4, FLIP_FOURCC_BGRA));
    assert(!flip_config_scale(12, 8, 12 * 4, 0x52474241u)); /* 'RGBA' unsupported */
    assert(!flip_config_scale(12, 8, 1024, FLIP_FOURCC_BGRA)); /* 8*1024 > slot_bytes */
    assert(!flip_dpu_scale_draw(bufB, surf, 12, 8, 12 * 4, FLIP_FOURCC_BGRA,
        0, 0, 12, 8, 1, 0, 8, 6, 8, 6)); /* partial px0 != 0 */

    /* Fill l10r surface: pure red (R=1023, G=0, B=0, A=3 -> 0xff0000ff? no: A[31:30]=3, R[29:20]=1023) */
    uint32_t l10r_red = (3u << 30) | (1023u << 20);
    for (unsigned i = 0; i < 12 * 8; i++) {
        ((uint32_t *)surf)[i] = l10r_red;
    }

    assert(flip_dpu_scale_draw(bufB, surf, 12, 8, 12 * 4, FLIP_FOURCC_L10R,
        0, 0, 12, 8, 0, 0, 8, 6, 8, 6));
    assert(flip.drew);
    assert(dpuScale.active);
    assert(memcmp(bufB, surf, sizeof(surf)) == 0);
    flip.shown = 1; /* simulate flip_present() switching scanout to B */

    /* Quiesce must convert l10r -> BGRA8888 (0xffff0000) into bufA and restore boot regs */
    assert(flip_dpu_scale_quiesce(false));
    assert(!dpuScale.active);
    assert(gLastPointBuf == 0);
    assert(memcmp(boot_snapshot, gMmio, sizeof(boot_snapshot)) == 0);
    for (unsigned i = 0; i < 8 * 6; i++) {
        assert(((uint32_t *)bufA)[i] == 0xffff0000u); /* B=0, G=0, R=255, A=255 */
    }

    /* Poweroff clears both buffers and restores boot regs */
    assert(flip_dpu_scale_draw(bufB, surf, 12, 8, 12 * 4, FLIP_FOURCC_BGRA,
        0, 0, 12, 8, 0, 0, 8, 6, 8, 6));
    assert(dpuScale.active);
    assert(flip_dpu_scale_quiesce(true));
    assert(!dpuScale.active);
    assert(memcmp(boot_snapshot, gMmio, sizeof(boot_snapshot)) == 0);
    for (unsigned i = 0; i < flip.bytes; i++) {
        assert(bufA[i] == 0 && bufB[i] == 0);
    }
    printf("  [PASS] fallback guards, raw copy, l10r quiesce conversion, poweroff clear\n");
}

static void test_scanout_mode_and_striped_copy(void) {
    seed_k90_boot_regs();
    const uint32_t sw = 1206u, sh = 2622u, stride = 4864u;
    size_t panel_bytes = (size_t)1156u * 4u * 2510u;
    size_t surf_bytes = (size_t)stride * sh;
    uint8_t *bufA = (uint8_t *)calloc(1, panel_bytes);
    uint8_t *bufB = (uint8_t *)calloc(1, surf_bytes);
    uint8_t *surf = (uint8_t *)calloc(1, surf_bytes);
    assert(bufA && bufB && surf);
    memset(bufA, 0x11, panel_bytes);
    for (size_t i = 0; i < surf_bytes; i++) {
        surf[i] = (uint8_t)((i * 37u + 13u) & 0xffu);
    }

    flip.va[0] = bufA;
    flip.va[1] = bufB;
    flip.pa_mock = 0;
    flip.bytes = (uint32_t)panel_bytes;
    flip.slot_bytes = (uint32_t)surf_bytes;
    flip.rows = 2510u;
    flip.shown = 0;

    gDpuScale = 1;
    dpu_scale_init();
    assert(dpuScale.capable);

    /* Whole-surface 1206x2622 padded stride (4864) striped across 3 worker ranges */
    uint32_t copy_stride = 0;
    bool whole = false;
    assert(flip_dpu_scale_prepare(bufB, surf, sw, sh, stride, FLIP_FOURCC_L10R,
        0, 0, (int32_t)sw, (int32_t)sh, 0, 0, 1156, 2510, 1156, 2510, &copy_stride, &whole));
    assert(whole && copy_stride == 4864u);
    dpu_scale_copy_rows(bufB, surf, stride, copy_stride, 0, 0, 0, 874, whole);
    dpu_scale_copy_rows(bufB, surf, stride, copy_stride, 0, 0, 874, 1748, whole);
    dpu_scale_copy_rows(bufB, surf, stride, copy_stride, 0, 0, 1748, (int32_t)sh, whole);
    flip_dpu_scale_finish();
    assert(memcmp(bufB, surf, surf_bytes) == 0);

    dpu_scale_report_scanout(0xfd400000ULL);
    assert(gScanoutPa == 0xfd400000ULL && gScanoutW == 1206u && gScanoutH == 2622u &&
           gScanoutStride == 4864u && gScanoutFmt == FLIP_FOURCC_L10R);

    /* Cropped sub-rect (sx=2, sy=10, sw=1180, sh=2600) striped copy -> packed stride 4720 */
    memset(bufB, 0x00, surf_bytes);
    assert(flip_dpu_scale_prepare(bufB, surf, sw, sh, stride, FLIP_FOURCC_BGRA,
        2, 10, 1180, 2600, 0, 0, 1156, 2510, 1156, 2510, &copy_stride, &whole));
    assert(!whole && copy_stride == 1180u * 4u);
    dpu_scale_copy_rows(bufB, surf, stride, copy_stride, 2, 10, 0, 1300, whole);
    dpu_scale_copy_rows(bufB, surf, stride, copy_stride, 2, 10, 1300, 2600, whole);
    flip_dpu_scale_finish();
    for (int y = 0; y < 2600; y++) {
        assert(memcmp(bufB + (size_t)y * copy_stride, surf + (size_t)(10 + y) * stride + 8u, copy_stride) == 0);
    }

    dpu_scale_report_scanout(0xfd400000ULL);
    assert(gScanoutW == 1180u && gScanoutH == 2600u && gScanoutStride == 4720u && gScanoutFmt == FLIP_FOURCC_BGRA);

    assert(flip_dpu_scale_fallback());
    assert(!dpuScale.active);
    dpu_scale_report_scanout(0xfc800000ULL);
    assert(gScanoutPa == 0xfc800000ULL && gScanoutW == 0u && gScanoutH == 0u &&
           gScanoutStride == 0u && gScanoutFmt == 0u);
    assert(!flip_dpu_scale_fallback());
    free(bufA);
    free(bufB);
    free(surf);
    printf("  [PASS] live scanout metadata + striped dpu_scale_copy_rows + fallback\n");
}

int main(void) {
    test_default_off();
    test_bgra_and_l10r_programming();
    test_guards_and_quiesce();
    test_scanout_mode_and_striped_copy();
    printf("MiniFramebufferDpuScale host tests: PASS (4/4)\n");
    return 0;
}
'''


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="fb-dpu-scale-test-") as td:
        src = Path(td) / "test_dpu_scale.c"
        exe = Path(td) / "test_dpu_scale"
        src.write_text(HARNESS)
        cmd = [
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-I",
            str(INC_DIR),
            str(src),
            "-o",
            str(exe),
        ]
        subprocess.run(cmd, check=True)
        env = dict(os.environ)
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        subprocess.run([str(exe)], check=True, env=env)
    return 0


if __name__ == "__main__":
    sys.exit(main())

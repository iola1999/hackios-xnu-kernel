#!/usr/bin/env python3
"""Host ASan+UBSan unit test for MiniFramebufferYuv.inc ('420v'/'420f' YUV scanout + srcOver compositing)."""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
YUV_INC = os.path.join(ROOT, "xnu", "files", "iokit", "Kernel", "MiniFramebufferYuv.inc")

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINI_FB_YUV_HOST_TEST 1
#include "MiniFramebufferYuv.inc"

static uint32_t px_bgra(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
}

static int abs_i(int x) { return x < 0 ? -x : x; }

static void check_rgb(uint32_t px, int er, int eg, int eb, int ea, int tol, const char *label) {
    int b = (int)(px & 0xffu);
    int g = (int)((px >> 8) & 0xffu);
    int r = (int)((px >> 16) & 0xffu);
    int a = (int)((px >> 24) & 0xffu);
    if (abs_i(r - er) > tol || abs_i(g - eg) > tol || abs_i(b - eb) > tol || abs_i(a - ea) > tol) {
        fprintf(stderr, "FAIL %s: got BGRA=(%d,%d,%d,%d) expected (%d,%d,%d,%d) tol=%d\n",
            label, r, g, b, a, er, eg, eb, ea, tol);
        exit(1);
    }
}

static void test_yuv_color_accuracy(void) {
    /* 1. '420v' BT.601 (matrix=6 and default 0): black (16,128,128), white (235,128,128), red/green/blue bars */
    check_rgb(fbYuvToBgra(16, 128, 128, false, 6), 0, 0, 0, 255, 1, "420v-601-black");
    check_rgb(fbYuvToBgra(235, 128, 128, false, 6), 255, 255, 255, 255, 1, "420v-601-white");
    check_rgb(fbYuvToBgra(81, 90, 240, false, 6), 255, 0, 0, 255, 3, "420v-601-red");
    check_rgb(fbYuvToBgra(145, 54, 34, false, 6), 0, 255, 0, 255, 3, "420v-601-green");
    check_rgb(fbYuvToBgra(41, 240, 110, false, 6), 0, 0, 255, 255, 3, "420v-601-blue");
    check_rgb(fbYuvToBgra(81, 90, 240, false, 0), 255, 0, 0, 255, 3, "420v-default-red");

    /* 2. '420v' BT.709 (matrix=1): standard BT.709 limited-range primaries */
    check_rgb(fbYuvToBgra(16, 128, 128, false, 1), 0, 0, 0, 255, 1, "420v-709-black");
    check_rgb(fbYuvToBgra(235, 128, 128, false, 1), 255, 255, 255, 255, 1, "420v-709-white");
    check_rgb(fbYuvToBgra(63, 102, 240, false, 1), 255, 0, 0, 255, 3, "420v-709-red");
    check_rgb(fbYuvToBgra(173, 42, 26, false, 1), 0, 255, 0, 255, 3, "420v-709-green");
    check_rgb(fbYuvToBgra(32, 240, 118, false, 1), 0, 0, 255, 255, 3, "420v-709-blue");

    /* Verify BT.709 vs BT.601 produce distinct chroma weights for same mid-tone YCbCr */
    uint32_t c601 = fbYuvToBgra(128, 60, 200, false, 6);
    uint32_t c709 = fbYuvToBgra(128, 60, 200, false, 1);
    assert(c601 != c709);

    /* 3. '420f' full-range BT.601 and BT.709 */
    check_rgb(fbYuvToBgra(0, 128, 128, true, 6), 0, 0, 0, 255, 1, "420f-601-black");
    check_rgb(fbYuvToBgra(255, 128, 128, true, 6), 255, 255, 255, 255, 1, "420f-601-white");
    check_rgb(fbYuvToBgra(76, 85, 255, true, 6), 255, 0, 0, 255, 3, "420f-601-red");
    check_rgb(fbYuvToBgra(54, 99, 255, true, 1), 255, 0, 0, 255, 3, "420f-709-red");
}

static void test_scaling_and_bounds(void) {
    /* Odd 7x5 Y plane, 4x3 UV plane with padded strides (ASan catches any OOB read) */
    const uint32_t yW = 7, yH = 5, yBPR = 16;
    const uint32_t uvW = 4, uvH = 3, uvBPR = 16;
    uint8_t *yBuf = (uint8_t *)malloc(yH * yBPR);
    uint8_t *uvBuf = (uint8_t *)malloc(uvH * uvBPR);
    memset(yBuf, 145, yH * yBPR);
    for (uint32_t r = 0; r < uvH; r++) {
        for (uint32_t c = 0; c < uvW; c++) {
            uvBuf[r * uvBPR + c * 2] = 54;
            uvBuf[r * uvBPR + c * 2 + 1] = 34;
        }
    }
    struct fb_yuv420_plane_view v = {
        .yBase = yBuf, .uvBase = uvBuf,
        .yW = yW, .yH = yH, .yBPR = yBPR,
        .uvW = uvW, .uvH = uvH, .uvBPR = uvBPR,
        .fullRange = false, .matrix = 6
    };

    const uint32_t pW = 20, pH = 12, pBPR = pW * 4;
    uint8_t *panel = (uint8_t *)calloc(pH, pBPR);

    /* Destination rect partially outside panel on all 4 edges + oversized src rect */
    bool ok = fbDrawYuv420Rect(panel, pW, pH, pBPR, &v, 0, 0, 100, 100, -4, -3, 24, 15);
    assert(ok);
    const uint32_t *p32 = (const uint32_t *)panel;
    check_rgb(p32[0], 0, 255, 0, 255, 3, "scaled-oob-top-left");
    check_rgb(p32[(pH - 1) * pW + (pW - 1)], 0, 255, 0, 255, 3, "scaled-oob-bot-right");

    /* Invalid negative src origin or completely offscreen dst must return false without touching memory */
    assert(!fbDrawYuv420Rect(panel, pW, pH, pBPR, &v, -1, 0, 4, 4, 0, 0, 10, 10));
    assert(!fbDrawYuv420Rect(panel, pW, pH, pBPR, &v, 0, 0, 4, 4, 30, 0, 40, 10));

    free(panel);
    free(yBuf);
    free(uvBuf);
}

static void test_multilayer_safari_composite(void) {
    /*
     * Simulates Safari b5r84 layers 0x6 (0 7 381 0):
     * - Panel: 32x24 BGRA8888
     * - Layer 2: '420v' 16x12 video (bright green: Y=145, Cb=54, Cr=34) scaled to dst=[4,4,24,16]
     * - Layer 1: 'BGRA' 32x24 UI with:
     *     - opaque dark chrome (20,20,20,255) outside video rect
     *     - transparent cutout (0,0,0,0) over video rect [4..28, 4..20)
     *     - semi-transparent overlay bar (premul 64,0,0,128) at y=6
     *     - opaque white play icon (255,255,255,255) at (16,12)
     */
    const uint32_t pW = 32, pH = 24, pBPR = pW * 4;
    uint8_t *panel = (uint8_t *)calloc(pH, pBPR);

    const uint32_t yW = 16, yH = 12, yBPR = 64;
    const uint32_t uvW = 8, uvH = 6, uvBPR = 64;
    uint8_t *yBuf = (uint8_t *)malloc(yH * yBPR);
    uint8_t *uvBuf = (uint8_t *)malloc(uvH * uvBPR);
    memset(yBuf, 145, yH * yBPR);
    for (uint32_t r = 0; r < uvH; r++) {
        for (uint32_t c = 0; c < uvW; c++) {
            uvBuf[r * uvBPR + c * 2] = 54;
            uvBuf[r * uvBPR + c * 2 + 1] = 34;
        }
    }
    struct fb_yuv420_plane_view v = {
        .yBase = yBuf, .uvBase = uvBuf,
        .yW = yW, .yH = yH, .yBPR = yBPR,
        .uvW = uvW, .uvH = uvH, .uvBPR = uvBPR,
        .fullRange = false, .matrix = 6
    };

    uint32_t *ui = (uint32_t *)malloc(pH * pBPR);
    for (uint32_t y = 0; y < pH; y++) {
        for (uint32_t x = 0; x < pW; x++) {
            if (x >= 4 && x < 28 && y >= 4 && y < 20) {
                ui[y * pW + x] = 0u; /* transparent cutout */
            } else {
                ui[y * pW + x] = px_bgra(20, 20, 20, 255); /* opaque Safari chrome */
            }
        }
    }
    /* Semi-transparent premultiplied red strip over video at y=6, x=10 */
    ui[6 * pW + 10] = px_bgra(64, 0, 0, 128);
    /* Opaque white play button over video center at (16, 12) */
    ui[12 * pW + 16] = px_bgra(255, 255, 255, 255);

#if defined(MUTATE_SKIP_YUV)
    /* Mutation 1: old MiniIOMobileFramebuffer behavior skips planar YUV layer 2 */
    (void)v;
    assert(fbCompositeOverRect(panel, pW, pH, pBPR, (const uint8_t *)ui, pW, pH, pBPR,
        FOURCC('B','G','R','A'), 0, 0, (int32_t)pW, (int32_t)pH, 0, 0, (int32_t)pW, (int32_t)pH));
#elif defined(MUTATE_REVERSE_ORDER)
    /* Mutation 2: draw UI layer 1 first, then overwrite with YUV layer 2 */
    assert(fbCompositeOverRect(panel, pW, pH, pBPR, (const uint8_t *)ui, pW, pH, pBPR,
        FOURCC('B','G','R','A'), 0, 0, (int32_t)pW, (int32_t)pH, 0, 0, (int32_t)pW, (int32_t)pH));
    assert(fbDrawYuv420Rect(panel, pW, pH, pBPR, &v, 0, 0, (int32_t)yW, (int32_t)yH, 4, 4, 28, 20));
#else
    /* Correct back-to-front: layer 2 YUV video first, then layer 1 premultiplied BGRA UI over it */
    assert(fbDrawYuv420Rect(panel, pW, pH, pBPR, &v, 0, 0, (int32_t)yW, (int32_t)yH, 4, 4, 28, 20));
    assert(fbCompositeOverRect(panel, pW, pH, pBPR, (const uint8_t *)ui, pW, pH, pBPR,
        FOURCC('B','G','R','A'), 0, 0, (int32_t)pW, (int32_t)pH, 0, 0, (int32_t)pW, (int32_t)pH));
#endif

    const uint32_t *out = (const uint32_t *)panel;
    /* 1. Chrome outside video rect */
    check_rgb(out[1 * pW + 1], 20, 20, 20, 255, 0, "safari-chrome");
    /* 2. Video cutout pixel (alpha=0) must show bright green video */
    check_rgb(out[10 * pW + 10], 0, 255, 0, 255, 3, "safari-video-cutout");
    /* 3. Semi-transparent overlay (64,0,0,128) over green (0,255,0,255):
     *    R = 64 + 0 = 64, G = 0 + (255 * 127 + 127)/255 = 127, B = 0, A = 255 */
    check_rgb(out[6 * pW + 10], 64, 127, 0, 255, 2, "safari-semitrans-overlay");
    /* 4. Opaque white play icon over video center must stay white */
    check_rgb(out[12 * pW + 16], 255, 255, 255, 255, 0, "safari-play-icon");

    free(ui);
    free(panel);
    free(yBuf);
    free(uvBuf);
}

int main(void) {
    assert(fbIsBiplanarYuv8(FOURCC('4','2','0','v'), 2));
    assert(fbIsBiplanarYuv8(FOURCC('4','2','0','f'), 2));
    assert(!fbIsBiplanarYuv8(FOURCC('B','G','R','A'), 0));
    assert(!fbIsBiplanarYuv8(FOURCC('4','2','0','v'), 0));
    test_yuv_color_accuracy();
    test_scaling_and_bounds();
    test_multilayer_safari_composite();
    puts("MiniFramebufferYuv host test: 420v (BT.601/BT.709), 420f, bounds/scaling, and Safari multi-layer srcOver passed");
    return 0;
}
'''


def build_and_run(tmpdir, name, extra_defines=()):
    src = os.path.join(tmpdir, f"{name}.cpp")
    exe = os.path.join(tmpdir, name)
    with open(src, "w", encoding="utf-8") as f:
        f.write(HARNESS)
    cmd = [
        "clang++", "-std=c++17", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        f"-I{os.path.dirname(YUV_INC)}",
        *[f"-D{d}" for d in extra_defines],
        src, "-o", exe,
    ]
    subprocess.run(cmd, check=True)
    return subprocess.run([exe], capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory(prefix="fb-yuv-test-") as tmp:
        res = build_and_run(tmp, "yuv_ok")
        if res.returncode != 0:
            sys.stderr.write(res.stdout + res.stderr)
            return 1
        print(res.stdout.strip())

        for mut in ("MUTATE_SKIP_YUV", "MUTATE_REVERSE_ORDER"):
            mres = build_and_run(tmp, f"yuv_{mut.lower()}", extra_defines=(mut,))
            if mres.returncode == 0:
                sys.stderr.write(f"ERROR: mutation {mut} unexpectedly passed!\n")
                return 1
            print(f"mutation {mut}: rejected as expected ({mres.stderr.strip().splitlines()[0]})")
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Verify double-buffered MiniIOMobileFramebuffer row tracking and rw_pipe_key graph hashing."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCALE_H = ROOT / "xnu/files/iokit/Kernel/MiniFramebufferScale.h"
DPU_SCALE_INC = ROOT / "xnu/files/iokit/Kernel/MiniFramebufferDpuScale.inc"
ROWS_INC = ROOT / "xnu/files/iokit/Kernel/MiniFramebufferRows.inc"
FB_CPP = ROOT / "xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp"
PIPECACHE_INC = ROOT / "tools/userland-driver/annibalemetal_pipecache.inc"
COPY_H = ROOT / "tools/userland-driver/air_metal_copy.h"
ADRENO8_PRIV_H = ROOT / "xnu/files/bsd/dev/adreno8/adreno8_private.h"
ADRENO8_SMMU_C = ROOT / "xnu/files/bsd/dev/adreno8/adreno8_smmu.c"
ADRENO8_BO_C = ROOT / "xnu/files/bsd/dev/adreno8/adreno8_bo.c"

FB_HARNESS = r"""
#include <assert.h>
#include <atomic>
#include <condition_variable>
#include <limits.h>
#include <mutex>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include "MiniFramebufferScale.h"

#define FOURCC(a, b, c, d) (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
    ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))
#define NSEC_PER_MSEC 1000000ULL
#define THREAD_UNINT 0
typedef int wait_result_t;
typedef int IOLock;
static std::atomic<uint32_t> gHelperWakeups{0};
static bool gRealHelperPool = false;
static std::atomic<bool> gStopHelpers{false};
static std::mutex gRealMtx;
static std::condition_variable gRealCv;
static inline void IOLockLock(IOLock *l) { (void)l; if (gRealHelperPool) gRealMtx.lock(); }
static inline void IOLockUnlock(IOLock *l) { (void)l; if (gRealHelperPool) gRealMtx.unlock(); }
static inline void IOLockSleep(IOLock *l, void *e, int m) {
    (void)l; (void)m;
    if (!gRealHelperPool) return;
    std::unique_lock<std::mutex> lk(gRealMtx, std::adopt_lock);
    if (gStopHelpers.load()) {
        lk.unlock();
        pthread_exit(nullptr);
    }
    gRealCv.wait(lk);
    if (gStopHelpers.load()) {
        lk.unlock();
        pthread_exit(nullptr);
    }
    lk.release();
    (void)e;
}
static inline void IOLockWakeup(IOLock *l, void *e, bool o) {
    (void)l; (void)e; (void)o;
    gHelperWakeups.fetch_add(1);
    if (gRealHelperPool) gRealCv.notify_all();
}
#define IONew(type, count) ((type *)calloc((count), sizeof(type)))
#define IODelete(ptr, type, count) free(ptr)
#define bzero(p, n) memset((p), 0, (n))

static uint64_t gNowNs = 1000000000ULL;
static inline uint64_t mach_absolute_time(void) { return gNowNs; }
static inline void absolutetime_to_nanoseconds(uint64_t a, uint64_t *out) { *out = a; }

static uint32_t gRows = 1;
static uint32_t gFilter = 1;
static bool fFilterFrame = true;

struct MiniSurfaceInfo {
    uint32_t width, height, bytesPerRow, pixelFormat;
};
struct MiniIOSurface {
    MiniSurfaceInfo info;
    uint8_t *buf;
    uint8_t *pixels(void) { return buf; }
};

struct FlipState {
    uint8_t *va[2];
    uint32_t rows;
    int shown;
    bool ready, drew;
    int32_t band0, band1, stale0, stale1;
} flip;

static uint8_t *flip_target(uint8_t *base) {
    return flip.ready ? flip.va[flip.shown ^ 1] : base;
}
static void flip_band(int32_t *by0, int32_t *by1, int32_t py0, int32_t py1) {
    if (!flip.ready) return;
    flip.band0 = *by0 < flip.band0 ? *by0 : flip.band0;
    flip.band1 = *by1 > flip.band1 ? *by1 : flip.band1;
    *by0 = flip.stale0 < *by0 ? (flip.stale0 > py0 ? flip.stale0 : py0) : *by0;
    *by1 = flip.stale1 > *by1 ? (flip.stale1 < py1 ? flip.stale1 : py1) : *by1;
    flip.drew = true;
}
static void flip_whole(void) {
    int32_t y0 = 0, y1 = (int32_t)flip.rows;
    flip_band(&y0, &y1, y0, y1);
}
static void flip_present(void) {
    if (!flip.ready || !flip.drew) return;
    flip.shown ^= 1;
    flip.stale0 = flip.band0;
    flip.stale1 = flip.band1;
    flip.band0 = INT32_MAX;
    flip.band1 = INT32_MIN;
    flip.drew = false;
}

struct RowJob;
class IOMobileFramebuffer {
public:
    uint8_t *fBase = NULL;
    uint32_t fPanelWidth = 0, fPanelHeight = 0, fRowBytes = 0;
    uint32_t fWidth = 0, fHeight = 0;
    bool fDiff = false, fSingleLayer = true, fSettling = false, fDrewNearest = false, fColumnValid = false;
    uint64_t fDiffPixels = 0;
    uint32_t fDiffLeft = ~0u, fDiffTop = ~0u, fDiffRight = 0, fDiffBottom = 0;
    uint64_t *fRowHash = NULL;
    uint32_t *fRowChanged = NULL;
    uint32_t fRowHashSize = 0, fRowHashRows = 0;
    uint32_t fRowSeq = 0, fAllChangedSeq = 0, fBufferSeq[2] = { 0, 0 };
    int32_t fRowHashGeometry[8] = { 0 };
    uint64_t fBandAt = 0;
    uint32_t fWholeFrames = 0, fUnhashed = 0, fStatUnhashed = 0, fStatUnchanged = 0, fLastChangedRows = 0;
    uint64_t fStatBandRows = 0;
    uint64_t fStatChangedRows = 0;
    int32_t fStatRowsFrom = -1, fStatRowsTo = -1;
    uint32_t *fColumn = NULL;
    uint16_t *fColumnWeight = NULL;
    int32_t fColumnGeometry[5] = { 0 };
    uint32_t *fBlendRow = NULL;
    uint32_t fBlendRowSize = 0;
    int32_t fLastBy0 = -1, fLastBy1 = -1;
    uint32_t fLastBufSeqBefore = 0, fLastBufSeqAfter = 0;
    bool fLastAllDirty = false;
    unsigned fThreads = 1, fHelpersBusy = 0, fHelpersStarted = 0;
    IOLock *fHelperLock = NULL;
    struct RowJob *fHelperJob = NULL;
    uint64_t fHelperGeneration = 0;
    uint32_t *fHelperBlend[7] = { NULL };

    void drawLayer(MiniIOSurface *s, const int32_t *src, const int32_t *dst);
    bool drawDpuScaled(MiniIOSurface *s, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                       int32_t px0, int32_t py0, int32_t px1, int32_t py1);
    void runHelperRows(struct RowJob *job, uint32_t *blendRow);
    void drawRows(const struct RowJob *job, int32_t y0, int32_t y1, uint32_t *blendRow);
    void hashRows(struct RowJob *job, unsigned share, unsigned shares);
    bool dirtyBand(struct RowJob *job);
    static void helperMain(void *self, wait_result_t result);
};

static bool gMockDpuCapable = false;
static bool gMockDpuActive = false;
static uint32_t gMockDpuFrames = 0;

static bool flip_dpu_scale_prepare(uint8_t *dst_buf, const uint8_t *pixels, uint32_t surf_w, uint32_t surf_h,
    uint32_t stride, uint32_t fmt, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
    int32_t px0, int32_t py0, int32_t px1, int32_t py1, uint32_t panel_w, uint32_t panel_h,
    uint32_t *copy_stride_out, bool *whole_surf_out) {
    (void)fmt;
    if (!gMockDpuCapable || !flip.ready || !dst_buf || !pixels ||
        px0 != 0 || py0 != 0 || px1 != (int32_t)panel_w || py1 != (int32_t)panel_h ||
        sw <= 0 || sh <= 0 || sx < 0 || sy < 0 ||
        (uint32_t)(sx + sw) > surf_w || (uint32_t)(sy + sh) > surf_h) {
        return false;
    }
    bool whole = (sx == 0 && sy == 0 && (uint32_t)sw == surf_w);
    *whole_surf_out = whole;
    *copy_stride_out = whole ? stride : (uint32_t)sw * 4u;
    gMockDpuActive = true;
    return true;
}

__DPU_COPY_ROWS__

static void flip_dpu_scale_finish(void) {
    flip.band0 = 0;
    flip.band1 = (int32_t)flip.rows;
    flip.drew = true;
    gMockDpuFrames++;
}

static bool flip_dpu_scale_fallback(void) {
    if (!gMockDpuActive) return false;
    gMockDpuActive = false;
    return true;
}

__ROWS_INC__
__DRAW_LAYER__

static void compute_expected(IOMobileFramebuffer *ref, MiniIOSurface *surf, const int32_t *src,
                             const int32_t *dst, uint8_t *expected) {
    bool saved = flip.ready;
    uint32_t savedRows = gRows;
    flip.ready = false;
    gRows = 0;
    memset(expected, 0, (size_t)ref->fRowBytes * ref->fPanelHeight);
    ref->fBase = expected;
    ref->fColumnValid = false;
    ref->drawLayer(surf, src, dst);
    gRows = savedRows;
    flip.ready = saved;
}

static void fill_band(MiniIOSurface *s, uint32_t y0, uint32_t y1, uint32_t color) {
    for (uint32_t y = y0; y < y1; y++) {
        uint32_t *row = (uint32_t *)(s->buf + (size_t)y * s->info.bytesPerRow);
        for (uint32_t x = 0; x < s->info.width; x++) {
            row[x] = color + x * 17u + y * 31u;
        }
    }
}

int main(void) {
#if defined(__aarch64__) || defined(__x86_64__)
    {
        /* Two rounds of the four hash streams, then both the 64- and 32-bit tails. */
        alignas(8) uint32_t pixels[36];
        for (unsigned i = 0; i < 36; i++) pixels[i] = 0x12340000u + 17u * i;
        uint64_t value = 0;
        uint32_t changed = 0;
        IOMobileFramebuffer test;
        test.fRowHash = &value;
        test.fRowChanged = &changed;
        struct RowJob job = {};
        job.pixels = (const uint8_t *)pixels;
        job.stride = sizeof(pixels);
        job.sw = 35;
        job.sh = 1;
        test.hashRows(&job, 0, 1);
        assert(value == 0x944451ce5f83876bull);
    }
#endif
    const uint32_t sw = 76, sh = 162, pw = 72, ph = 160;
    IOMobileFramebuffer fb, ref;
    fb.fWidth = ref.fWidth = sw;
    fb.fHeight = ref.fHeight = sh;
    fb.fPanelWidth = ref.fPanelWidth = pw;
    fb.fPanelHeight = ref.fPanelHeight = ph;
    fb.fRowBytes = ref.fRowBytes = pw * 4;
    fb.fColumn = IONew(uint32_t, pw);
    fb.fColumnWeight = IONew(uint16_t, pw);
    ref.fColumn = IONew(uint32_t, pw);
    ref.fColumnWeight = IONew(uint16_t, pw);

    uint8_t *bufA = (uint8_t *)calloc(ph, fb.fRowBytes);
    uint8_t *bufB = (uint8_t *)calloc(ph, fb.fRowBytes);
    uint8_t *expected = (uint8_t *)calloc(ph, fb.fRowBytes);
    memset(bufA, 0xaa, (size_t)ph * fb.fRowBytes);
    memset(bufB, 0x00, (size_t)ph * fb.fRowBytes);

    flip.va[0] = bufA;
    flip.va[1] = bufB;
    flip.rows = ph;
    flip.shown = 0;
    flip.ready = true;
    flip.drew = false;
    flip.stale0 = 0;
    flip.stale1 = (int32_t)ph;
    flip.band0 = INT32_MAX;
    flip.band1 = INT32_MIN;

    MiniIOSurface surf;
    surf.info.width = sw;
    surf.info.height = sh;
    surf.info.bytesPerRow = sw * 4;
    surf.buf = (uint8_t *)calloc(sh, surf.info.bytesPerRow);

    int32_t fullSrc[4] = { 0, 0, (int32_t)sw, (int32_t)sh };
    int32_t fullDst[4] = { 0, 0, (int32_t)sw, (int32_t)sh };

    auto step = [&](const int32_t *src, const int32_t *dst, uint64_t dtMs, const char *label) {
        gNowNs += dtMs * NSEC_PER_MSEC;
        fb.fBase = flip_target(fb.fBase);
        fb.drawLayer(&surf, src, dst);
        flip_present();
        compute_expected(&ref, &surf, src, dst, expected);
        const uint8_t *shown = flip.va[flip.shown];
        if (memcmp(shown, expected, (size_t)ph * fb.fRowBytes) != 0) {
            for (uint32_t y = 0; y < ph; y++) {
                if (memcmp(shown + (size_t)y * fb.fRowBytes, expected + (size_t)y * fb.fRowBytes, fb.fRowBytes) != 0) {
                    fprintf(stderr, "MISMATCH at %s panel row %u (shown buf=%d)\n", label, y, flip.shown);
                    exit(1);
                }
            }
        }
    };

    /* 1. Cold home screen frame -> va[1], then partial update (<90% rows, Files context menu / app icon lift) -> va[0] & va[1] */
    fill_band(&surf, 0, sh, 0xff224466u);
    step(fullSrc, fullDst, 16, "frame1-home");

    fill_band(&surf, 20, 60, 0xff88aa22u);
    step(fullSrc, fullDst, 16, "frame2-partial-to-bufA");

    fill_band(&surf, 100, 130, 0xffcc5533u);
    step(fullSrc, fullDst, 16, "frame3-partial-to-bufB");

    /* 2. Identical frame (0 rows changed) followed by another partial update */
    step(fullSrc, fullDst, 16, "frame4-unchanged");
    fill_band(&surf, 10, 18, 0xff1199eeu);
    step(fullSrc, fullDst, 16, "frame5-after-unchanged");

    /* 3. Whole-screen burst entering ROWS_UNHASHED (App Switcher -> app), then partial band on exit */
    for (int i = 0; i < 6; i++) {
        fill_band(&surf, 0, sh, 0xff300000u + (uint32_t)i * 0x050505u);
        step(fullSrc, fullDst, 8, "unhashed-burst");
    }
    /* Simulate external whole-screen state change (e.g. multi-layer / black clear resetting hashes) */
    fb.fSingleLayer = false;
    fill_band(&surf, 0, sh, 0xff776655u);
    step(fullSrc, fullDst, 16, "multilayer-whole");
    fb.fSingleLayer = true;
    fill_band(&surf, 0, 12, 0xffeeddccu);
    step(fullSrc, fullDst, 16, "post-multilayer-f1");
    fill_band(&surf, 140, 155, 0xff334455u);
    step(fullSrc, fullDst, 16, "post-multilayer-f2");

    /* 3b. Partial overlay draw while !fSingleLayer (updates flip.stale0..stale1 to only 40..80,
     * leaving rows 90..130 in off-screen buffer behind fAllChangedSeq unless job->allDirty checks bufferSeq) */
    {
        int32_t subDst[4] = { 0, 40, (int32_t)sw, 40 };
        /* Frame A: full-screen single-layer establishes hashes & paints rows 90..130 in shown buffer */
        fill_band(&surf, 0, sh, 0xff123456u);
        step(fullSrc, fullDst, 30, "overlay-prep-f1");
        /* Off-screen buffer still holds older pixels at rows 90..130; simulate multi-layer sub-band flip */
        fb.fBase = flip_target(fb.fBase);
        fb.fSingleLayer = false;
        fb.drawLayer(&surf, fullSrc, subDst);
        flip_present();
        /* Next frame returns to single-layer full-screen (!known -> draws whole into other buffer) */
        fb.fSingleLayer = true;
        fill_band(&surf, 0, sh, 0xff654321u);
        step(fullSrc, fullDst, 30, "overlay-recover-f1");
        /* Simulate narrow stale band from an intervening sub-band flip, then tiny 4-row edit */
        flip.stale0 = 0;
        flip.stale1 = 20;
        fill_band(&surf, 2, 6, 0xff998877u);
        step(fullSrc, fullDst, 30, "overlay-recover-f2");
    }

    /* 4. Inset single-layer card must zero uncovered margins in off-screen buffer */
    int32_t insetDst[4] = { 10, 20, 50, 110 };
    step(fullSrc, insetDst, 16, "inset-f1");
    fill_band(&surf, 30, 50, 0xffabcdefu);
    step(fullSrc, insetDst, 16, "inset-f2");

    /* 5. A few rows touched at both the top and the bottom: the first..last span covers the panel but
     * only a handful of rows changed, so hashing must stay on instead of copying every row unhashed. */
    fb.fWholeFrames = 0; fb.fUnhashed = 0; fb.fStatUnhashed = 0;
    fill_band(&surf, 0, sh, 0xff203040u);
    step(fullSrc, fullDst, 30, "sparse-prep");
    for (int i = 0; i < 6; i++) {
        fill_band(&surf, 0, 4, 0xff400000u + (uint32_t)i * 0x030303u);
        fill_band(&surf, sh - 4, sh, 0xff004000u + (uint32_t)i * 0x030303u);
        step(fullSrc, fullDst, 8, "sparse-top-bottom");
    }
    assert(fb.fWholeFrames == 0);
    assert(fb.fStatUnhashed == 0);

    /* 6. Idle 10 Hz small-band updates (>= ROWS_SOON_NS, < DRAW_MIN_PARALLEL_ROWS) must run inline
     * without waking helper threads, while keeping exact double-buffered pixel parity. */
    fb.fThreads = 1;
    fb.fHelpersStarted = 0;
    fb.fWholeFrames = 0; fb.fUnhashed = 0; fb.fStatUnhashed = 0;
    fill_band(&surf, 0, sh, 0xff102030u);
    step(fullSrc, fullDst, 100, "idle-prep-1");
    fill_band(&surf, 0, sh, 0xff102031u);
    step(fullSrc, fullDst, 100, "idle-prep-2");
    fill_band(&surf, 70, 82, 0xff501000u);
    step(fullSrc, fullDst, 100, "idle-prep-3");
    fb.fThreads = 3;
    fb.fHelpersStarted = 2;
    gHelperWakeups = 0;
    for (int i = 0; i < 4; i++) {
        fill_band(&surf, 70, 82, 0xff501010u + (uint32_t)i * 0x040404u);
        step(fullSrc, fullDst, 100, "idle-10hz-clock");
    }
    assert(gHelperWakeups == 0);
    /* Verify shouldParallelDraw flips to true once work reaches DRAW_MIN_PARALLEL_ROWS. */
    {
        RowJob bigJob = {};
        bigJob.by0 = 0; bigJob.by1 = 160; bigJob.drawWork = 128; bigJob.allDirty = false;
        assert(shouldParallelDraw(&bigJob));
        bigJob.drawWork = 127;
        assert(!shouldParallelDraw(&bigJob));
        bigJob.allDirty = true;
        assert(shouldParallelDraw(&bigJob));
    }

    /* 7. Post-animation settle: ROWS_UNHASHED is bounded to 4, so static frames after a whole-screen
     * burst re-hash within 4 frames and detect unchanged pixels. */
    fb.fThreads = 1;
    fb.fHelpersStarted = 0;
    fb.fWholeFrames = 0; fb.fUnhashed = 0; fb.fStatUnhashed = 0; fb.fStatUnchanged = 0;
    for (int i = 0; i < 3; i++) {
        fill_band(&surf, 0, sh, 0xff600000u + (uint32_t)i * 0x070707u);
        step(fullSrc, fullDst, 8, "settle-burst");
    }
    for (int i = 0; i < 6; i++) {
        step(fullSrc, fullDst, 8, "settle-static");
    }
    assert(fb.fStatUnhashed <= ROWS_UNHASHED);
    assert(fb.fStatUnchanged >= 1);

    /* 8. Parallel fb_threads copy in drawDpuScaled via real helperMain worker threads. */
    {
        const uint32_t dw = 240, dh = 260, dstride = 1024; /* padded stride > dw*4 */
        uint8_t *dbuf = (uint8_t *)calloc(dh, dstride);
        uint8_t *dsurf = (uint8_t *)calloc(dh, dstride);
        for (size_t i = 0; i < (size_t)dh * dstride; i++) {
            dsurf[i] = (uint8_t)((i * 73u + 19u) & 0xffu);
        }
        MiniIOSurface dpuSurf = {};
        dpuSurf.info.width = dw;
        dpuSurf.info.height = dh;
        dpuSurf.info.bytesPerRow = dstride;
        dpuSurf.info.pixelFormat = FOURCC('l', '1', '0', 'r');
        dpuSurf.buf = dsurf;

        fb.fBase = dbuf;
        fb.fThreads = 3;
        fb.fHelpersStarted = 0;
        fb.fHelperGeneration = 0;
        fb.fHelpersBusy = 0;
        gMockDpuCapable = true;
        gMockDpuActive = false;
        gMockDpuFrames = 0;
        gHelperWakeups.store(0);
        gStopHelpers.store(false);
        gRealHelperPool = true;

        {
            std::thread t0([&]() { IOMobileFramebuffer::helperMain(&fb, 0); });
            std::thread t1([&]() { IOMobileFramebuffer::helperMain(&fb, 0); });
            for (;;) {
                gRealMtx.lock();
                unsigned ready = fb.fHelpersStarted;
                gRealMtx.unlock();
                if (ready == 2) break;
                std::this_thread::yield();
            }
            int32_t dsrc[4] = { 0, 0, (int32_t)dw, (int32_t)dh };
            int32_t ddst[4] = { 0, 0, (int32_t)sw, (int32_t)sh };
            fb.drawLayer(&dpuSurf, dsrc, ddst);
            assert(gMockDpuActive && gMockDpuFrames == 1);
            assert(gHelperWakeups.load() >= 1);
            assert(memcmp(dbuf, dsurf, (size_t)dh * dstride) == 0);

            /* Cropped sub-rect parallel copy (px/py cover full panel, sx=8 sy=10 sw=200 sh=220 >= 128) */
            memset(dbuf, 0, (size_t)dh * dstride);
            int32_t csrc[4] = { 8, 10, 200, 220 };
            fb.drawLayer(&dpuSurf, csrc, ddst);
            for (int y = 0; y < 220; y++) {
                assert(memcmp(dbuf + (size_t)y * 200u * 4u, dsurf + (size_t)(10 + y) * dstride + 8u * 4u, 200u * 4u) == 0);
            }

            /* Partial dst triggers flip_dpu_scale_fallback() before software drawRows */
            fb.drawLayer(&surf, fullSrc, insetDst);
            assert(!gMockDpuActive);

            gRealMtx.lock();
            gStopHelpers.store(true);
            fb.fHelperGeneration++;
            gRealCv.notify_all();
            gRealMtx.unlock();
            t0.join();
            t1.join();
        }
        gRealHelperPool = false;
        gMockDpuCapable = false;
        free(dbuf);
        free(dsurf);
    }

    free(surf.buf);
    free(bufA);
    free(bufB);
    free(expected);
    printf("PASS: MiniIOMobileFramebuffer double-buffered row tracking\n");
    return 0;
}
"""

PIPE_HARNESS = r"""
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "air_metal_copy.h"

struct rwa_device { int dummy; };
struct rwa_render { int dummy; };
struct rwa_compute { int dummy; };
struct rwa_render_info { int dummy; };
struct rwa_compute_info { int dummy; };
struct rwa_render_shape { uint32_t color_format[8]; uint32_t depth_format; };
static void stub_render_destroy(struct rwa_render *r) { (void)r; }
static void stub_compute_destroy(struct rwa_compute *c) { (void)c; }
static void rw_print(const char *fmt, ...) { (void)fmt; }
static struct {
    void (*render_destroy)(struct rwa_render *);
    void (*compute_destroy)(struct rwa_compute *);
} mesa = { stub_render_destroy, stub_compute_destroy };

#include "pipecache_under_test.inc"

int main(void) {
    @autoreleasepool {
        NSData *vs = [NSData dataWithBytes:"VSBC" length:4];
        NSData *fs = [NSData dataWithBytes:"FSBC" length:4];
        struct rwa_render_shape shape = {};
        shape.color_format[0] = 80;

        int argsA[2] = { 0, 1 };
        int argsB[2] = { 1, 0 };
        unsigned afterA[1] = { 0 };
        unsigned afterB[1] = { 1 };

        struct rwa_stitch_node nodes1[2] = {
            { "N0", 2, "op", 0, NULL, 0, NULL },
            { "N1", 2, "mix", 2, argsA, 1, afterA },
        };
        struct rwa_stitch_node nodes2[2] = {
            { "N0", 2, "op", 0, NULL, 0, NULL },
            { "N1", 2, "mix", 2, argsB, 1, afterA },
        };
        struct rwa_stitch_node nodes3[2] = {
            { "N0", 2, "op", 0, NULL, 0, NULL },
            { "N1", 2, "mix", 2, argsA, 1, afterB },
        };
        struct rwa_linked_function l1 = { "stitched", 2, nodes1, 1 };
        struct rwa_linked_function l2 = { "stitched", 2, nodes2, 1 };
        struct rwa_linked_function l3 = { "stitched", 2, nodes3, 1 };

        unsigned char k1[CC_SHA256_DIGEST_LENGTH], k2[CC_SHA256_DIGEST_LENGTH], k3[CC_SHA256_DIGEST_LENGTH];
        rw_pipe_key(k1, vs, "vmain", fs, "fmain", &shape, &l1, 1);
        rw_pipe_key(k2, vs, "vmain", fs, "fmain", &shape, &l2, 1);
        rw_pipe_key(k3, vs, "vmain", fs, "fmain", &shape, &l3, 1);

        if (memcmp(k1, k2, sizeof(k1)) == 0) {
            fprintf(stderr, "COLLISION on node->args wiring\n");
            return 1;
        }
        if (memcmp(k1, k3, sizeof(k1)) == 0) {
            fprintf(stderr, "COLLISION on node->after dependencies\n");
            return 1;
        }
        printf("PASS: rw_pipe_key hashes stitched graph edges and dependencies\n");
        return 0;
    }
}
"""


def extract_draw_layer(cpp_text: str) -> str:
    start = cpp_text.index("void\nIOMobileFramebuffer::drawLayer(")
    end = cpp_text.index("\n/*\n * How evenly the frames", start)
    return cpp_text[start:end]


def extract_dpu_copy_rows(dpu_text: str) -> str:
    start = dpu_text.index("static void\ndpu_scale_copy_rows(")
    end = dpu_text.index("\nstatic void\nflip_dpu_scale_finish(", start)
    return dpu_text[start:end]


IOVA_HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
#define PAGE_SIZE 4096u
#define SMMU_GRANULE 4096u
#define SMMU_GRANULE_MASK 4095u
#define SMMU_PHYS_LIMIT (1ULL << 40)
#define PTE_TABLE 3ULL
#define PTE_ADDRESS 0x0000fffffffff000ULL
#define ADRENO8_PTE_EXTERNAL (1ULL << 55)
#define ADRENO8_MEM_READ 1u
#define ADRENO8_MEM_WRITE 2u
#define ADRENO8_MEM_EXEC 4u
#define ADRENO8_GPU_IOVA_START 0x10000000u
#define ADRENO8_GPU_IOVA_END   0xe0000000u
#define ADRENO8_BO_REFUSE_NOT_READY 1u
#define ADRENO8_BO_REFUSE_NO_RANGE  2u
#define ADRENO8_BO_REFUSE_NO_TABLE  3u
#define KERN_SUCCESS 0
#define KMA_KOBJECT 0
#define KMA_ZERO 0
#define KMA_DATA 0
#define VM_KERN_MEMORY_IOKIT 0
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL

static int kernel_map;
static unsigned adreno8_step = 23;
static bool adreno8_reset_required;
static uint64_t fake_phys_next = 0x100000000ULL;

static int kmem_alloc(int map, vm_offset_t *out, size_t sz, int flags, int tag) {
    (void)map; (void)flags; (void)tag;
    void *p = NULL;
    if (posix_memalign(&p, 4096, sz) != 0) return -1;
    memset(p, 0, sz);
    *out = (vm_offset_t)p;
    return KERN_SUCCESS;
}
static void kmem_free(int map, vm_offset_t addr, size_t sz) {
    (void)map; (void)sz;
    free((void *)addr);
}
static uint64_t kvtophys(vm_offset_t va) {
    (void)va;
    uint64_t pa = fake_phys_next;
    fake_phys_next += 4096ULL;
    return pa;
}
static void adreno8_to_memory(vm_offset_t va, size_t sz) { (void)va; (void)sz; }
static uint64_t mach_absolute_time(void) { return 1000000ULL; }
static int proc_selfpid(void) { return 38; }
static uint64_t adreno8_space_attributes(uint32_t prot) { return (uint64_t)prot << 6; }

__SPACE_STRUCT__

static struct adreno8_gpu_space adreno8_gpu_space = { "gpu", 0 };
static enum { BO_COLD, BO_ENABLED, BO_FAILED } bo_state = BO_ENABLED;
static uint64_t bo_guard_pte = 0x80000000ULL | ADRENO8_PTE_EXTERNAL;
static uint64_t bo_mapped_low = UINT64_MAX, bo_mapped_high;

static bool adreno8_bo_ready(void) { return bo_state == BO_ENABLED; }
static bool bo_fail(const char *w) { fprintf(stderr, "bo_fail: %s\n", w); return false; }
static bool bo_range(uint64_t address, uint64_t size) {
    return size != 0 && ((address | size) & SMMU_GRANULE_MASK) == 0 &&
        address >= ADRENO8_GPU_IOVA_START && address < ADRENO8_GPU_IOVA_END &&
        size <= ADRENO8_GPU_IOVA_END - address;
}

__SMMU_FUNCS__
__BO_CONSTS__

static void bo_note_reserve(uint32_t address, uint64_t size) { (void)address; (void)size; }
static void bo_note_release(uint64_t address, uint64_t size) { (void)address; (void)size; }

__BO_FUNCS__

static bool ref_find_range(struct adreno8_gpu_space *space, uint32_t start, uint32_t end,
    uint32_t size, uint32_t alignment, uint32_t *address) {
    uint64_t candidate = ((uint64_t)start + alignment - 1) & ~(uint64_t)(alignment - 1);
    while (candidate + size <= end) {
        uint32_t at;
        for (at = 0; at < size; at += SMMU_GRANULE) {
            uint64_t *entry = adreno8_space_entry(space, (uint32_t)candidate + at);
            if (entry != NULL && *entry != 0) break;
        }
        if (at == size) { *address = (uint32_t)candidate; return true; }
        candidate = (candidate + at + SMMU_GRANULE + alignment - 1) & ~(uint64_t)(alignment - 1);
    }
    return false;
}

int main(void) {
    assert(adreno8_space_add_table(&adreno8_gpu_space, 0, 0) != 0);
    adreno8_gpu_space.is_on = true;

    /* 1. Verify leaf-skipping adreno8_space_find_range matches 4 KB reference walk across partial leaves. */
    uint64_t a1 = adreno8_bo_reserve(12 * 4096, NULL);
    uint64_t a2 = adreno8_bo_reserve(600 * 4096, NULL);
    assert(a1 != 0 && a2 != 0);
    assert(adreno8_bo_release(a1, 12 * 4096));
    for (uint32_t sz_pages : (uint32_t[]){ 1, 12, 13, 512, 513, 1025 }) {
        uint32_t fast = 0, slow = 0;
        bool ok_f = adreno8_space_find_range(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START,
            0x20000000u, sz_pages * 4096u, 4096u, &fast);
        bool ok_s = ref_find_range(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START,
            0x20000000u, sz_pages * 4096u, 4096u, &slow);
        assert(ok_f == ok_s && fast == slow);
    }
    assert(adreno8_bo_release(a2, 600 * 4096));

    /* 2. Fragment > 2.0 GiB with 960 pinned 4 KB islands spaced every 2 MiB (allocated in waves larger
     * than BO_WINDOW so L2 tables across [0x10000000, 0x88000000) are populated, then pads released),
     * and allocate 16 full-screen 28 MiB RGBA16Float textures in backboardd. */
    uint64_t pads[960];
    for (int i = 0; i < 960; i++) {
        pads[i] = adreno8_bo_reserve(509 * 4096, NULL);
        uint64_t pin = adreno8_bo_reserve(4096, NULL);
        assert(pads[i] != 0 && pin != 0);
    }
    for (int i = 0; i < 960; i++) {
        assert(adreno8_bo_release(pads[i], 509 * 4096));
    }
    uint64_t big[16];
    for (int i = 0; i < 16; i++) {
        big[i] = adreno8_bo_reserve(28u << 20, NULL);
        if (big[i] == 0) {
            fprintf(stderr, "FAILED 28 MiB allocation #%d\n", i);
            return 1;
        }
    }
    for (int i = 0; i < 16; i++) {
        assert(adreno8_bo_release(big[i], 28u << 20));
    }
    printf("PASS: adreno8_space_find_range + 2064 SMMU tables + large-BO arena\n");
    return 0;
}
"""


def run_iova_case(priv_text: str, smmu_text: str, bo_text: str) -> subprocess.CompletedProcess:
    s0 = priv_text.index("struct adreno8_space_table {")
    s1 = priv_text.index("};", priv_text.index("struct adreno8_gpu_space {", s0)) + 2
    space_struct = priv_text[s0:s1]

    m0 = smmu_text.index("static struct adreno8_space_table *\nadreno8_space_table(")
    m1 = smmu_text.index("/* What publishing has cost", m0)
    m2 = smmu_text.index("bool\nadreno8_space_find_range(", m1)
    m3 = smmu_text.index("bool\nadreno8_space_map_region(", m2)
    smmu_funcs = smmu_text[m0:m1] + "\n" + smmu_text[m2:m3]

    c0 = bo_text.index("#define BO_WINDOW")
    c1 = bo_text.index("/*\n * The last BO_HISTORY", c0)
    b0 = bo_text.index("uint64_t\nadreno8_bo_reserve(")
    b1 = bo_text.index("/* One page of a buffer", b0)
    r0 = bo_text.index("bool\nadreno8_bo_release(", b1)
    bo_consts = bo_text[c0:c1]
    bo_funcs = bo_text[b0:b1] + "\n" + bo_text[r0:]

    code = (
        IOVA_HARNESS.replace("__SPACE_STRUCT__", space_struct)
        .replace("__SMMU_FUNCS__", smmu_funcs)
        .replace("__BO_CONSTS__", bo_consts)
        .replace("__BO_FUNCS__", bo_funcs)
    )
    with tempfile.TemporaryDirectory() as tmp:
        td = Path(tmp)
        src = td / "test_iova.cpp"
        exe = td / "test_iova"
        src.write_text(code)
        subprocess.run(["clang++", "-std=c++17", "-O2", str(src), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], capture_output=True, text=True)


def run_fb_case(rows_text: str, draw_text: str, dpu_copy_text: str) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as tmp:
        td = Path(tmp)
        (td / "MiniFramebufferScale.h").write_text(SCALE_H.read_text())
        code = (
            FB_HARNESS.replace("__DPU_COPY_ROWS__", dpu_copy_text)
            .replace("__ROWS_INC__", rows_text)
            .replace("__DRAW_LAYER__", draw_text)
        )
        src = td / "test_fb.cpp"
        exe = td / "test_fb"
        src.write_text(code)
        subprocess.run(
            ["clang++", "-std=c++17", "-O2", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             f"-I{td}", str(src), "-o", str(exe)],
            check=True,
        )
        return subprocess.run([str(exe)], capture_output=True, text=True,
                              env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


def run_pipe_case(pipecache_text: str) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as tmp:
        td = Path(tmp)
        (td / "air_metal_copy.h").write_text(COPY_H.read_text())
        (td / "pipecache_under_test.inc").write_text(pipecache_text)
        src = td / "test_pipe.m"
        exe = td / "test_pipe"
        src.write_text(PIPE_HARNESS)
        subprocess.run(
            ["clang", "-fobjc-arc", "-O2", "-framework", "Foundation", f"-I{td}", str(src), "-o", str(exe)],
            check=True,
        )
        return subprocess.run([str(exe)], capture_output=True, text=True)


def main() -> None:
    rows_text = ROWS_INC.read_text()
    draw_text = extract_draw_layer(FB_CPP.read_text())
    dpu_copy_text = extract_dpu_copy_rows(DPU_SCALE_INC.read_text())

    res = run_fb_case(rows_text, draw_text, dpu_copy_text)
    if res.returncode != 0:
        sys.stderr.write(res.stderr)
        sys.exit(1)
    print(res.stdout.strip())

    # Mutation 1: simulate uninitialized stack garbage in job.bufferSeq (the G23/G24 regression).
    mut1_draw = draw_text.replace("job.bufferSeq = fBufferSeq[targetBuf];", "job.bufferSeq = 0xfffffe00u;")
    assert mut1_draw != draw_text, "mutation 1 target missing"
    assert run_fb_case(rows_text, mut1_draw, dpu_copy_text).returncode != 0, "mutation 1 unexpectedly passed"

    # Mutation 2: drop fAllChangedSeq > job->bufferSeq check in dirtyBand.
    mut2_rows = rows_text.replace(
        "job->allDirty = !known || fAllChangedSeq > job->bufferSeq;",
        "job->allDirty = !known;",
    )
    assert mut2_rows != rows_text, "mutation 2 target missing"
    assert run_fb_case(mut2_rows, draw_text, dpu_copy_text).returncode != 0, "mutation 2 unexpectedly passed"

    pipecache_text = PIPECACHE_INC.read_text()
    pres = run_pipe_case(pipecache_text)
    if pres.returncode != 0:
        sys.stderr.write(pres.stderr)
        sys.exit(1)
    print(pres.stdout.strip())

    # Mutation 3: omit node->args from rw_pipe_key and verify collision is caught.
    mut3_pipe = pipecache_text.replace(
        "if (node->num_args != 0 && node->args != NULL)",
        "if (0)",
    )
    assert mut3_pipe != pipecache_text, "mutation 3 target missing"
    assert run_pipe_case(mut3_pipe).returncode != 0, "mutation 3 unexpectedly passed"

    priv_text = ADRENO8_PRIV_H.read_text()
    smmu_text = ADRENO8_SMMU_C.read_text()
    bo_text = ADRENO8_BO_C.read_text()
    ires = run_iova_case(priv_text, smmu_text, bo_text)
    if ires.returncode != 0:
        sys.stderr.write(ires.stderr)
        sys.exit(1)
    print(ires.stdout.strip())

    # Mutation 4: revert tables[2064] -> tables[1024] and verify 0x8f800000 refusal is caught.
    mut4_priv = priv_text.replace("tables[2064]", "tables[1024]")
    assert mut4_priv != priv_text, "mutation 4 target missing"
    assert run_iova_case(mut4_priv, smmu_text, bo_text).returncode != 0, "mutation 4 unexpectedly passed"
    print("PASS: all mutations caught")


if __name__ == "__main__":
    main()

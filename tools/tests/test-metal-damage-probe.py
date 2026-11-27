#!/usr/bin/env python3
"""Host ASan+UBSan test for S7 single-frame damage_probe height buckets and percentiles."""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SUBMIT_SRC = ROOT / "userland-driver" / "annibalemetal_commandbuffer_submit.inc"


def extract_damage_helpers() -> str:
    text = SUBMIT_SRC.read_text()
    start = text.index("#define RW_DAMAGE_RING 32u")
    end = text.index("\n/*\n * GPU YIELDING,")
    return text[start:end]


HARNESS = r"""
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MTLLoadActionDontCare = 0,
    MTLLoadActionLoad = 1,
    MTLLoadActionClear = 2,
};

enum RWCopyKind { RW_COPY_NONE, RW_COPY_CLEAR };

static char log_buf[16384];
static size_t log_len;

static void
rw_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);
    if (n > 0 && log_len + (size_t)n < sizeof(log_buf)) {
        log_len += (size_t)n;
    }
}

static uint32_t
ann_surface_id(void *sf)
{
    return (uint32_t)(uintptr_t)sf;
}

@interface FakeTex : NSObject {
    unsigned _w, _h;
    void *_sf;
}
- (instancetype)initWithW:(unsigned)w h:(unsigned)h sf:(uint32_t)sf;
- (NSUInteger)width;
- (NSUInteger)height;
- (void *)iosurface;
@end

@implementation FakeTex
- (instancetype)initWithW:(unsigned)w h:(unsigned)h sf:(uint32_t)sf
{
    self = [super init];
    if (self) { _w = w; _h = h; _sf = (void *)(uintptr_t)sf; }
    return self;
}
- (NSUInteger)width { return _w; }
- (NSUInteger)height { return _h; }
- (void *)iosurface { return _sf; }
@end

@interface AnnibaleMetalWork : NSObject {
@public
    enum RWCopyKind copyKind;
    id render;
    id color[8];
    unsigned colorCount;
    unsigned scissorRect[4];
    BOOL hasScissorRect;
}
@end
@implementation AnnibaleMetalWork
@end

__DAMAGE_HELPERS__

static void
check(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\nlog:\n%s\n", msg, log_buf);
        exit(1);
    }
}

int
main(void)
{
    @autoreleasepool {
        check(rw_damage_probe_enabled(), "ANNIBALE_DAMAGE_PROBE=1 must enable probe");
        FakeTex *surfA = [[FakeTex alloc] initWithW:1206 h:2600 sf:10];
        FakeTex *surfB = [[FakeTex alloc] initWithW:1206 h:2600 sf:11];
        NSObject *fakeRender = [[NSObject alloc] init];

        /* 30 frames: 10 frames <25% (h=260), 10 frames 25..50% (h=780),
         * 5 frames 50..90% (h=1820), 5 frames >90% (h=2600).
         * Sorted heights: [260 x10, 780 x10, 1820 x5, 2600 x5].
         * p50 = sorted[15] = 780, p90 = sorted[26] = 2600. */
        const unsigned heights[30] = {
            260, 260, 260, 260, 260, 260, 260, 260, 260, 260,
            780, 780, 780, 780, 780, 780, 780, 780, 780, 780,
            1820, 1820, 1820, 1820, 1820,
            2600, 2600, 2600, 2600, 2600,
        };
        for (unsigned i = 0; i < 30; i++) {
            const void *cb = (const void *)(uintptr_t)(0x1000u + i);
            rw_damage_note_pass(cb, i == 0 ? MTLLoadActionClear : MTLLoadActionLoad);
            AnnibaleMetalWork *w = [[AnnibaleMetalWork alloc] init];
            w->copyKind = RW_COPY_NONE;
            w->render = fakeRender;
            w->color[0] = (i & 1u) ? surfB : surfA;
            w->colorCount = 1;
            w->hasScissorRect = YES;
            w->scissorRect[0] = 0;
            w->scissorRect[1] = 0;
            w->scissorRect[2] = 1206;
            w->scissorRect[3] = heights[i];
            rw_damage_commit_sample(cb, @[w]);
            [w release];
        }
        check(strstr(log_buf, "damage_probe[1]: clear=1 load=29 dontcare=0 draws=30 "
                              "all_scissored=30/30 h_buckets=10/10/5/5 h_p50=780 h_p90=2600 "
                              "iosurface_id=11 sf_distinct=2") != NULL,
              "unexpected damage_probe summary line");

        [fakeRender release];
        [surfB release];
        [surfA release];
    }
    return 0;
}
"""


def main() -> int:
    code = HARNESS.replace("__DAMAGE_HELPERS__", extract_damage_helpers())
    with tempfile.TemporaryDirectory(prefix="metal-damage-probe-") as tmp:
        src = pathlib.Path(tmp) / "test.m"
        exe = pathlib.Path(tmp) / "test"
        src.write_text(code)
        subprocess.run([
            "clang", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-objc-arc",
            "-framework", "Foundation", str(src), "-o", str(exe),
        ], check=True)
        env = dict(os.environ, ANNIBALE_DAMAGE_PROBE="1")
        subprocess.run([str(exe)], env=env, check=True)
    print("test-metal-damage-probe: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

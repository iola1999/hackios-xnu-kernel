#!/usr/bin/env python3
"""Host ASan+UBSan test for the ANNIBALE_G31_TRACE env-gated RenderPass probe."""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENCODER_SRC = ROOT / "userland-driver" / "annibalemetal_renderencoder.inc"


def extract_g31_helpers() -> str:
    text = ENCODER_SRC.read_text()
    start = text.index("static unsigned\nrw_g31_trace_limit(void)")
    end = text.index("\n@implementation AnnibaleMetalRenderEncoder")
    return text[start:end]


HARNESS = r"""
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MTLLoadActionDontCare = 0,
    MTLLoadActionLoad = 1,
    MTLLoadActionClear = 2,
};

struct rwa_color_blend {
    unsigned blending_enabled;
    unsigned rgb_blend_operation, alpha_blend_operation;
    unsigned source_rgb_blend_factor, destination_rgb_blend_factor;
    unsigned source_alpha_blend_factor, destination_alpha_blend_factor;
    unsigned write_mask;
};

static char log_buf[16384];
static size_t log_len;
static unsigned log_lines;

static void
rw_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t add = (size_t)n;
        if (log_len + add < sizeof(log_buf)) {
            log_len += add;
        }
        log_lines++;
    }
}

static uint32_t
ann_surface_id(void *sf)
{
    return (uint32_t)(uintptr_t)sf;
}

@interface FakeTex : NSObject {
    unsigned _w, _h, _fmt;
    void *_sf;
}
- (instancetype)initWithW:(unsigned)w h:(unsigned)h fmt:(unsigned)fmt sf:(uint32_t)sf;
- (NSUInteger)width;
- (NSUInteger)height;
- (NSUInteger)pixelFormat;
- (void *)iosurface;
@end

@implementation FakeTex
- (instancetype)initWithW:(unsigned)w h:(unsigned)h fmt:(unsigned)fmt sf:(uint32_t)sf
{
    self = [super init];
    if (self) {
        _w = w; _h = h; _fmt = fmt;
        _sf = (void *)(uintptr_t)sf;
    }
    return self;
}
- (NSUInteger)width { return _w; }
- (NSUInteger)height { return _h; }
- (NSUInteger)pixelFormat { return _fmt; }
- (void *)iosurface { return _sf; }
@end

@interface AnnibaleMetalRenderPipeline : NSObject {
    struct rwa_color_blend _blend[8];
    NSString *_label;
    NSString *_fragmentName;
}
- (instancetype)initWithLabel:(NSString *)label fragmentName:(NSString *)frag blend:(struct rwa_color_blend)b;
- (const struct rwa_color_blend *)colorBlendAtIndex:(unsigned)index;
- (NSString *)label;
- (NSString *)fragmentFunctionName;
@end

@implementation AnnibaleMetalRenderPipeline
- (instancetype)initWithLabel:(NSString *)label fragmentName:(NSString *)frag blend:(struct rwa_color_blend)b
{
    self = [super init];
    if (self) {
        _label = [label retain];
        _fragmentName = [frag retain];
        for (unsigned i = 0; i < 8; i++) {
            _blend[i] = b;
        }
    }
    return self;
}
- (void)dealloc { [_label release]; [_fragmentName release]; [super dealloc]; }
- (const struct rwa_color_blend *)colorBlendAtIndex:(unsigned)index
{
    return index < 8 ? &_blend[index] : NULL;
}
- (NSString *)label { return _label; }
- (NSString *)fragmentFunctionName { return _fragmentName; }
@end

__G31_HELPERS__

static void
check(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\nlog:\n%s\n", msg, log_buf);
        exit(1);
    }
}

int
main(int argc, char **argv)
{
    @autoreleasepool {
        const char *mode = argc > 1 ? argv[1] : "off";
        struct rwa_color_blend b = {
            .blending_enabled = 1,
            .rgb_blend_operation = 0,
            .alpha_blend_operation = 0,
            .source_rgb_blend_factor = 1,
            .destination_rgb_blend_factor = 5,
            .source_alpha_blend_factor = 1,
            .destination_alpha_blend_factor = 5,
            .write_mask = 0x7,
        };
        AnnibaleMetalRenderPipeline *pipe =
            [[AnnibaleMetalRenderPipeline alloc] initWithLabel:@"safari.grid" fragmentName:@"grid_frag" blend:b];
        AnnibaleMetalRenderPipeline *pipe_nolabel =
            [[AnnibaleMetalRenderPipeline alloc] initWithLabel:nil fragmentName:@"maps_label_frag" blend:b];
        FakeTex *a8_96 = [[FakeTex alloc] initWithW:96 h:96 fmt:1 sf:0];
        FakeTex *r8_96 = [[FakeTex alloc] initWithW:96 h:96 fmt:10 sf:0];
        FakeTex *surf_bgra = [[FakeTex alloc] initWithW:1206 h:2622 fmt:80 sf:42];
        FakeTex *big_fp16 = [[FakeTex alloc] initWithW:1206 h:2622 fmt:115 sf:0];
        FakeTex *big_bgra = [[FakeTex alloc] initWithW:512 h:512 fmt:80 sf:0];
        FakeTex *vtex_64 = [[FakeTex alloc] initWithW:64 h:64 fmt:80 sf:0];

        if (strcmp(mode, "off") == 0) {
            check(rw_g31_trace_limit() == 0, "default off must return limit 0");
            id colors[1] = { a8_96 };
            uint8_t loads[1] = { MTLLoadActionClear };
            rw_g31_trace_draw(pipe, colors, 1, loads, NULL, 0, NULL, 0, NULL);
            check(log_lines == 0, "default off must emit 0 lines");
        } else if (strcmp(mode, "on") == 0) {
            check(rw_g31_trace_limit() == 256, "ANNIBALE_G31_TRACE=1 must default to 256");

            /* 1. 96x96 A8Unorm target, Clear, mask=0x7, premul blend */
            id c1[1] = { a8_96 };
            uint8_t l1[1] = { MTLLoadActionClear };
            unsigned sc1[4] = { 0, 0, 96, 96 };
            rw_g31_trace_draw(pipe, c1, 1, l1, NULL, 0, NULL, 0, sc1);
            check(log_lines == 1, "96x96 A8Unorm target must log");
            check(strstr(log_buf, "c0=96x96 fmt=1 sf=0 load=clear mask=0x7 blend=1 rgb=1/5/0 a=1/5/0 tex=none sc=0,0,96,96 pipe=safari.grid") != NULL,
                  "line 1 format mismatch");

            /* 2. BGRA8Unorm IOSurface target + 96x96 R8Unorm ftex */
            id c2[1] = { surf_bgra };
            uint8_t l2[1] = { MTLLoadActionLoad };
            id f2[1] = { r8_96 };
            rw_g31_trace_draw(pipe, c2, 1, l2, f2, 1, NULL, 0, NULL);
            check(log_lines == 2, "IOSurface BGRA + 96x96 R8 ftex must log");
            check(strstr(log_buf, "c0=1206x2622 fmt=80 sf=42 load=load") != NULL &&
                  strstr(log_buf, "tex=ftex[0]=96x96/fmt=10/sf=0 sc=none") != NULL,
                  "line 2 format mismatch");

            /* 3. Non-matching large non-IOSurface fp16 target + large 512x512 ftex */
            id c3[1] = { big_fp16 };
            uint8_t l3[1] = { MTLLoadActionDontCare };
            id f3[1] = { big_bgra };
            rw_g31_trace_draw(pipe, c3, 1, l3, f3, 1, NULL, 0, NULL);
            check(log_lines == 2, "large non-IOSurface fp16 + large ftex must not log");

            /* 4. Large non-IOSurface target + small 64x64 vtex */
            id v4[1] = { vtex_64 };
            rw_g31_trace_draw(pipe, c3, 1, l3, NULL, 0, v4, 1, NULL);
            check(log_lines == 3, "small vtex must trigger log");
            check(strstr(log_buf, "c0=1206x2622 fmt=115 sf=0 load=dontcare") != NULL &&
                  strstr(log_buf, "tex=vtex[0]=64x64/fmt=80/sf=0") != NULL,
                  "line 3 format mismatch");

            /* 5. Nil pipeline label falls back to fragmentFunctionName */
            rw_g31_trace_draw(pipe_nolabel, c1, 1, l1, NULL, 0, NULL, 0, sc1);
            check(log_lines == 4, "nil-label pipeline must log with fragmentFunctionName fallback");
            check(strstr(log_buf, "pipe=maps_label_frag") != NULL,
                  "line 4 fragmentFunctionName fallback mismatch");
        } else if (strcmp(mode, "cap2") == 0) {
            check(rw_g31_trace_limit() == 2, "ANNIBALE_G31_TRACE=2 must parse cap 2");
            id c[1] = { a8_96 };
            uint8_t l[1] = { MTLLoadActionClear };
            for (int i = 0; i < 5; i++) {
                rw_g31_trace_draw(pipe, c, 1, l, NULL, 0, NULL, 0, NULL);
            }
            check(log_lines == 2, "cap 2 must stop after 2 lines");
        }

        [vtex_64 release];
        [big_bgra release];
        [big_fp16 release];
        [surf_bgra release];
        [r8_96 release];
        [a8_96 release];
        [pipe_nolabel release];
        [pipe release];
    }
    return 0;
}
"""


def main() -> int:
    code = HARNESS.replace("__G31_HELPERS__", extract_g31_helpers())
    with tempfile.TemporaryDirectory(prefix="metal-g31-trace-") as tmp:
        src = pathlib.Path(tmp) / "test.m"
        exe = pathlib.Path(tmp) / "test"
        src.write_text(code)
        cmd = [
            "clang",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-objc-arc",
            "-framework",
            "Foundation",
            str(src),
            "-o",
            str(exe),
        ]
        subprocess.run(cmd, check=True)
        env_off = {k: v for k, v in os.environ.items() if k != "ANNIBALE_G31_TRACE"}
        subprocess.run([str(exe), "off"], env=env_off, check=True)
        env_zero = dict(env_off, ANNIBALE_G31_TRACE="0")
        subprocess.run([str(exe), "off"], env=env_zero, check=True)
        env_on = dict(env_off, ANNIBALE_G31_TRACE="1")
        subprocess.run([str(exe), "on"], env=env_on, check=True)
        env_cap = dict(env_off, ANNIBALE_G31_TRACE="2")
        subprocess.run([str(exe), "cap2"], env=env_cap, check=True)
    print("test-metal-g31-trace: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

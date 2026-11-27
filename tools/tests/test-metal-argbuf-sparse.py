#!/usr/bin/env python3
"""Host ASan+UBSan test for sparse [[id(n)]] argument buffer layout and encoder validation."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
ARGENC_SRC = ROOT / "userland-driver" / "annibalemetal_compute_argencoder.inc"
REFLECT_SRC = ROOT.parents[1] / "hackios-xnu-userland" / "tools" / "mesa" / "air" / "air_metal_reflect_vertex.inc"


def extract_ready_method() -> str:
    text = ARGENC_SRC.read_text()
    start = text.index("- (BOOL)annibaleReady:(NSUInteger)index for:(const char *)what")
    end = text.index("\n/*\n * The three that record.", start)
    return text[start:end]


def extract_layout_loop() -> str:
    text = REFLECT_SRC.read_text()
    start = text.index("   for (unsigned k = 0; k < n; k++) {\n      struct rwa_argbuf_layout *l = &out[k];")
    end = text.index("\n   if (count)", start)
    return text[start:end]


HARNESS = r"""
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RWA_MAX_ARGBUF_MEMBERS 64

struct rwa_argbuf_member {
    unsigned id;
    unsigned kind;
    unsigned index;
    unsigned write;
    char name[64];
};

struct rwa_argbuf_layout {
    unsigned metal_index;
    unsigned encoded_length;
    unsigned alignment;
    unsigned member_count;
    struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];
};

static char log_buf[4096];
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

static void
say(char *why, size_t why_size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (why && why_size) {
        vsnprintf(why, why_size, fmt, ap);
    }
    va_end(ap);
}

static void ralloc_free(void *p) { (void)p; }

static int
finalize_layouts(struct rwa_argbuf_layout *out, unsigned n, const char *name,
                 char *why, size_t why_size)
{
    void *mem_ctx = NULL;
__LAYOUT_LOOP__
    return 0;
}

@interface FakeArgEncoder : NSObject {
@public
    id _destination;
    BOOL _complained;
    BOOL _fromModule;
    struct rwa_argbuf_layout _layout;
}
- (BOOL)annibaleReady:(NSUInteger)index for:(const char *)what;
@end

@implementation FakeArgEncoder
__READY_METHOD__
@end

int
main(void)
{
    @autoreleasepool {
        char why[256] = { 0 };

        /* 1. Weather pass_constants_encoder: 3 members with sparse id 30 and encoded_length=0. */
        struct rwa_argbuf_layout l1 = {
            .metal_index = 0, .encoded_length = 0, .alignment = 8, .member_count = 3,
            .members = {
                { .id = 0, .name = "frame" },
                { .id = 1, .name = "camera" },
                { .id = 30, .name = "linearDepth" },
            },
        };
        assert(finalize_layouts(&l1, 1, "pass_constants_encoder", why, sizeof(why)) == 0);
        assert(l1.encoded_length == 8u * 31u);

        /* 2. Duplicate id must still be rejected. */
        struct rwa_argbuf_layout ldup = l1;
        ldup.members[1].id = 30;
        assert(finalize_layouts(&ldup, 1, "dup_test", why, sizeof(why)) == -1);
        assert(strstr(why, "both id 30") != NULL);

        /* 3. Encoder validation: sparse id 30 accepted, id 31 & 64 rejected. */
        NSObject *dest = [[NSObject alloc] init];
        FakeArgEncoder *enc1 = [[FakeArgEncoder alloc] init];
        enc1->_destination = dest;
        enc1->_fromModule = YES;
        enc1->_layout = l1;
        assert([enc1 annibaleReady:0 for:"a buffer"] == YES);
        assert([enc1 annibaleReady:30 for:"a texture"] == YES);
        assert([enc1 annibaleReady:31 for:"a texture"] == NO);
        assert([enc1 annibaleReady:64 for:"a texture"] == NO);

        /* 4. Weather 10-member set with encoded_length=200 (25 slots) encoding ids 10..11, 20..24. */
        FakeArgEncoder *enc2 = [[FakeArgEncoder alloc] init];
        enc2->_destination = dest;
        enc2->_fromModule = YES;
        enc2->_layout.member_count = 10;
        enc2->_layout.encoded_length = 200;
        for (unsigned i = 0; i < 10; i++) enc2->_layout.members[i].id = i;
        assert([enc2 annibaleReady:10 for:"a buffer"] == YES);
        assert([enc2 annibaleReady:11 for:"a buffer"] == YES);
        assert([enc2 annibaleReady:20 for:"a texture"] == YES);
        assert([enc2 annibaleReady:24 for:"a texture"] == YES);
        assert([enc2 annibaleReady:25 for:"a texture"] == NO);

        [enc2 release];
        [enc1 release];
        [dest release];
    }
    return 0;
}
"""


def main() -> int:
    code = (HARNESS
            .replace("__LAYOUT_LOOP__", extract_layout_loop())
            .replace("__READY_METHOD__", extract_ready_method()))
    with tempfile.TemporaryDirectory(prefix="metal-argbuf-sparse-") as tmp:
        src = pathlib.Path(tmp) / "test.m"
        exe = pathlib.Path(tmp) / "test"
        src.write_text(code)
        subprocess.run([
            "clang", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-objc-arc",
            "-framework", "Foundation", str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)
    print("test-metal-argbuf-sparse: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Host ASan+UBSan test for G6 ObjC setPurgeableState:/isPurgeable hookup in annibalemetal_texture.inc."""
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "tools/userland-driver"
body = runpy.run_path(str(Path(__file__).with_name("test-metal-format94.py")))["body"]

HARNESS = r"""
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct rwa_texture {
    unsigned state;
    unsigned set_calls;
    unsigned last_req;
};

static volatile unsigned commit_in_flight = 0;

static unsigned
fake_set_purgeable(struct rwa_texture *t, unsigned state, char *why, size_t n)
{
    (void)why; (void)n;
    unsigned prev = t->state ? t->state : 2;
    t->set_calls++;
    t->last_req = state;
    if (state == 2 || state == 3 || state == 4) {
        t->state = state;
    }
    return prev;
}

static bool
fake_is_purgeable(const struct rwa_texture *t)
{
    return t->state == 3 || t->state == 4;
}

static struct {
    unsigned (*texture_set_purgeable)(struct rwa_texture *, unsigned, char *, size_t);
    bool (*texture_is_purgeable)(const struct rwa_texture *);
} mesa;

@interface AnnibaleMetalTexture : NSObject {
@public
    struct rwa_texture *_texture;
    CFTypeRef _iosurface;
    id _parentTexture;
    id _buffer;
}
- (NSUInteger)setPurgeableState:(NSUInteger)state;
- (BOOL)isPurgeable;
@end

@implementation AnnibaleMetalTexture
__METHODS__
@end

int
main(void)
{
    @autoreleasepool {
        struct rwa_texture raw = { .state = 2 };
        AnnibaleMetalTexture *t = [AnnibaleMetalTexture new];
        t->_texture = &raw;

        /* Fallback when dlsym symbols are absent (older libannibaleair.dylib). */
        mesa.texture_set_purgeable = NULL;
        mesa.texture_is_purgeable = NULL;
        assert([t setPurgeableState:3] == 2);
        assert([t isPurgeable] == NO);

        /* Active delegation on a private texture. */
        mesa.texture_set_purgeable = fake_set_purgeable;
        mesa.texture_is_purgeable = fake_is_purgeable;
        assert([t setPurgeableState:1] == 2 && raw.last_req == 1);
        assert([t setPurgeableState:3] == 2 && raw.state == 3);
        assert([t isPurgeable] == YES);
        assert([t setPurgeableState:4] == 3 && raw.state == 4);
        assert([t setPurgeableState:2] == 4 && raw.state == 2);
        assert([t isPurgeable] == NO);

        /* ObjC ownership & in-flight CB guards block Volatile (3) and Empty (4). */
        int dummy = 1;
        t->_iosurface = (CFTypeRef)&dummy;
        assert([t setPurgeableState:3] == 2 && raw.last_req == 1 && raw.state == 2);
        assert([t setPurgeableState:4] == 2 && raw.last_req == 1 && raw.state == 2);
        t->_iosurface = NULL;

        t->_parentTexture = (id)&dummy;
        assert([t setPurgeableState:4] == 2 && raw.last_req == 1 && raw.state == 2);
        t->_parentTexture = NULL;

        t->_buffer = (id)&dummy;
        assert([t setPurgeableState:3] == 2 && raw.last_req == 1 && raw.state == 2);
        t->_buffer = NULL;

        commit_in_flight = 7;
        assert([t setPurgeableState:4] == 2 && raw.last_req == 1 && raw.state == 2);
        commit_in_flight = 0;

        /* Private texture works again once commit completes. */
        assert([t setPurgeableState:4] == 2 && raw.last_req == 4 && raw.state == 4);
        /* NonVolatile restore is allowed even while commit_in_flight != 0. */
        commit_in_flight = 9;
        assert([t setPurgeableState:2] == 4 && raw.last_req == 2 && raw.state == 2);
        commit_in_flight = 0;

        [t release];
        printf("test-metal-purgeable: PASS\n");
    }
    return 0;
}
"""


def main() -> int:
    tex = (DRIVER / "annibalemetal_texture.inc").read_text()
    methods = (
        body(tex, "- (NSUInteger)setPurgeableState:(NSUInteger)state")
        + "\n"
        + body(tex, "- (BOOL)isPurgeable")
    )
    code = HARNESS.replace("__METHODS__", methods)
    with tempfile.TemporaryDirectory(prefix="metal-purgeable-") as td:
        src = Path(td) / "test.m"
        exe = Path(td) / "test"
        src.write_text(code)
        subprocess.run(
            [
                "xcrun", "-sdk", "macosx", "clang",
                "-fno-objc-arc", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined",
                "-framework", "Foundation",
                str(src), "-o", str(exe),
            ],
            check=True,
        )
        return subprocess.run([str(exe)], check=True).returncode


if __name__ == "__main__":
    raise SystemExit(main())

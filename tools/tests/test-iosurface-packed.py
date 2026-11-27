#!/usr/bin/env python3
"""Check native plane storage, packed Metal views, M5 boundary cases and surface write tracking."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "tools/userland-driver"
SOURCE = Path(__file__).with_name("iosurface-packed-native.m")
COMPLETION = DRIVER / "annibalemetal_completion.inc"


def extract_surface_writes() -> str:
    text = COMPLETION.read_text()
    start = text.index("#define RW_SURFACE_WRITES")
    end = text.index("\nstatic dispatch_queue_t")
    return text[start:end]


SURFACE_WRITES_HARNESS = r"""
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t rw_completion_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rw_completion_done = PTHREAD_COND_INITIALIZER;
static pthread_t rw_completion_tid;

static uint32_t ann_surface_id(void *sf) { return (uint32_t)(uintptr_t)sf; }

@interface FakeSurfTex : NSObject {
    void *_sf;
}
- (instancetype)initWithSid:(uint32_t)sid;
- (void *)iosurface;
@end
@implementation FakeSurfTex
- (instancetype)initWithSid:(uint32_t)sid {
    self = [super init];
    if (self) _sf = (void *)(uintptr_t)sid;
    return self;
}
- (void *)iosurface { return _sf; }
@end

__SURFACE_WRITES__

struct release_arg {
    unsigned target_commit;
    unsigned delay_us;
};

static void *advance_worker(void *arg) {
    struct release_arg *r = (struct release_arg *)arg;
    usleep(r->delay_us);
    pthread_mutex_lock(&rw_completion_lock);
    rw_completed_commit = r->target_commit;
    pthread_cond_broadcast(&rw_completion_done);
    pthread_mutex_unlock(&rw_completion_lock);
    return NULL;
}

int main(void) { @autoreleasepool {
    /* Fill all RW_SURFACE_WRITES slots with in-flight commits 1..RW_SURFACE_WRITES. */
    for (unsigned i = 1; i <= RW_SURFACE_WRITES; i++) {
        FakeSurfTex *t = [[FakeSurfTex alloc] initWithSid:i];
        rw_surface_commit_mark(t, i);
        [t release];
    }

    /* Inserting slot RW_SURFACE_WRITES+1 must wait for commit 1 instead of silently evicting it. */
    struct release_arg r1 = { .target_commit = 1, .delay_us = 15000 };
    pthread_t th1;
    assert(pthread_create(&th1, NULL, advance_worker, &r1) == 0);
    FakeSurfTex *extra = [[FakeSurfTex alloc] initWithSid:RW_SURFACE_WRITES + 1];
    rw_surface_commit_mark(extra, RW_SURFACE_WRITES + 1);
    assert(rw_completed_commit >= 1);
    pthread_join(th1, NULL);

    /* annibale_surface_writes_wait must block until commit RW_SURFACE_WRITES+1 retires. */
    struct release_arg r2 = { .target_commit = RW_SURFACE_WRITES + 1, .delay_us = 15000 };
    pthread_t th2;
    assert(pthread_create(&th2, NULL, advance_worker, &r2) == 0);
    annibale_surface_writes_wait(RW_SURFACE_WRITES + 1);
    assert(rw_completed_commit >= RW_SURFACE_WRITES + 1);
    pthread_join(th2, NULL);

    [extra release];
    puts("surface write table overflow wait & completion sync passed");
    return 0;
} }
"""


def main():
    with tempfile.TemporaryDirectory(prefix="iosurface-packed-") as directory:
        binary = Path(directory) / "native-packed"
        subprocess.run(["clang", "-fblocks", "-Wall", "-Wextra", "-Werror",
                        "-Wno-deprecated-declarations", "-Wno-multichar", "-O1",
                        "-fsanitize=address,undefined", "-I", str(DRIVER),
                        str(SOURCE), str(DRIVER / "annibaledrawable.m"),
                        "-framework", "Foundation", "-framework", "IOKit",
                        "-framework", "IOSurface", "-framework", "Metal",
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

        sw_src = Path(directory) / "surface-writes.m"
        sw_bin = Path(directory) / "surface-writes"
        sw_src.write_text(SURFACE_WRITES_HARNESS.replace("__SURFACE_WRITES__",
                                                         extract_surface_writes()))
        subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fno-objc-arc",
                        "-fsanitize=address,undefined",
                        "-framework", "Foundation", str(sw_src), "-o", str(sw_bin)],
                       check=True)
        subprocess.run([str(sw_bin)], check=True)


if __name__ == "__main__":
    main()

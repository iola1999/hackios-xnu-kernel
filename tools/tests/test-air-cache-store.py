#!/usr/bin/env python3
"""Exercise AIR cache stores with Foundation and a temporary filesystem directory."""
from pathlib import Path
import argparse
import subprocess
import tempfile


KERNEL = Path(__file__).resolve().parents[2]
SOURCE = KERNEL / "tools/userland-driver/annibalemetal_stitch.inc"
NATIVE = r'''
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *cache_directory;
#define RW_AIR_CACHE_DIR cache_directory
@FUNCTIONS@

static void check(BOOL condition, NSString *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", [message UTF8String]);
        exit(1);
    }
}

static NSData *disk(NSString *path)
{
    return [NSData dataWithContentsOfFile:path options:0 error:nil];
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        check(argc == 2, @"cache directory required");
        cache_directory = argv[1];
        NSString *root = [NSString stringWithUTF8String:argv[1]];
        NSString *path = [root stringByAppendingPathComponent:@"entry.air"];
        NSData *first = [@"first bitcode" dataUsingEncoding:NSUTF8StringEncoding];
        NSData *second = [@"changed bitcode" dataUsingEncoding:NSUTF8StringEncoding];
        rw_air_cache_store(@"entry", first);
        check([disk(path) isEqualToData:first], @"first store did not persist bytes");
        int held = open([path fileSystemRepresentation], O_RDONLY);
        struct stat before, after;
        check(held >= 0 && fstat(held, &before) == 0, @"cannot retain initial inode");
        rw_air_cache_store(@"entry", first);
        check(stat([path fileSystemRepresentation], &after) == 0 &&
              before.st_ino == after.st_ino, @"identical bytes rewrote the cache file");
        [rw_air_memcache removeAllObjects];
        rw_air_cache_store(@"entry", first);
        check(stat([path fileSystemRepresentation], &after) == 0 &&
              before.st_ino == after.st_ino, @"memory eviction rewrote identical disk bytes");
        rw_air_cache_store(@"entry", second);
        check([disk(path) isEqualToData:second], @"changed bytes were not persisted");
        check(stat([path fileSystemRepresentation], &after) == 0 &&
              before.st_ino != after.st_ino, @"changed bytes did not use atomic replacement");
        close(held);

        check(unlink([path fileSystemRepresentation]) == 0, @"cannot remove test file");
        rw_air_cache_store(@"entry", second);
        check([disk(path) isEqualToData:second], @"memory hit prevented missing-file retry");

        NSString *blocked = [root stringByAppendingPathComponent:@"blocked.air"];
        check(mkdir([blocked fileSystemRepresentation], 0700) == 0,
              @"cannot create failed-store fixture");
        rw_air_cache_store(@"blocked", first);
        check(disk(blocked) == nil, @"failed-store fixture unexpectedly became a file");
        check(rmdir([blocked fileSystemRepresentation]) == 0, @"cannot clear failed-store fixture");
        rw_air_cache_store(@"blocked", first);
        check([disk(blocked) isEqualToData:first], @"failed atomic store could not be retried");

        rw_air_cache_store(nil, first);
        rw_air_cache_store(@"empty", [NSData data]);
        rw_air_cache_store(@"empty", nil);
        check(![[NSFileManager defaultManager] fileExistsAtPath:
                [root stringByAppendingPathComponent:@"empty.air"]], @"empty input created a file");
        check([disk(path) isEqualToData:second], @"invalid input changed existing content");
        puts("AIR cache filesystem cases passed");
    }
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    args = parser.parse_args()
    source = args.source.read_text()
    start = source.index("static NSCache *rw_air_memcache;")
    end = min(source.index(marker, start) for marker in
              ("static BOOL\nrw_air_key_bytes", "static NSString *\nrw_air_cache_key")
              if marker in source[start:])
    store = source.index("static void\nrw_air_cache_store", end)
    stop = source.index("static id (*rw_library_function", store)
    functions = source[start:end] + source[store:stop]
    with tempfile.TemporaryDirectory(prefix="air-cache-store-") as directory:
        root = Path(directory)
        code = root / "store.m"
        code.write_text(NATIVE.replace("@FUNCTIONS@", functions))
        binary = root / "store"
        subprocess.run(["xcrun", "clang", "-fno-objc-arc", "-fblocks", "-Wall", "-Wextra",
                        "-Werror", "-framework", "Foundation", str(code), "-o", str(binary)],
                       check=True, timeout=30)
        subprocess.run([str(binary), str(root / "cache")], check=True, timeout=15)


if __name__ == "__main__":
    main()

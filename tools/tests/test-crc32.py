#!/usr/bin/env python3
"""Compare the production CRC with zlib across seeds, segments and buffer boundaries."""
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "xnu/files/bsd/libkern/crc32.c").read_text()
source = source.replace("#include <sys/param.h>\n#include <sys/systm.h>",
                        "#include <stddef.h>\n#include <stdint.h>")
source = source.replace("crc32(uint32_t", "crc32_under_test(uint32_t")
hardware = False
if platform.system() == "Darwin" and platform.machine() == "arm64":
    hardware = subprocess.check_output(
        ["sysctl", "-n", "hw.optional.armv8_crc32"], text=True).strip() == "1"

checks = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <zlib.h>
#include <assert.h>

uint32_t crc32_under_test(uint32_t, const void *, size_t);
int gARM_FEAT_CRC32;
static uint64_t checks;
static uint32_t state = 0x5eedcafe;
static uint32_t next_random(void) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
static void check(uint32_t seed, const uint8_t *p, size_t n) {
    uint32_t expected = p ? (uint32_t)crc32(seed, p, (unsigned)n) : seed;
    for (int hardware = 0; hardware <= TEST_CRC_HARDWARE; hardware++) {
        gARM_FEAT_CRC32 = hardware;
        assert(crc32_under_test(seed, p, n) == expected);
        size_t split = n ? next_random() % (n + 1) : 0;
        uint32_t first = crc32_under_test(seed, p, split);
        assert(crc32_under_test(first, p ? p + split : NULL, n - split) == expected);
        checks += 2;
    }
}
int main(void) {
    const size_t cap = 32 * 1024 * 1024;
    uint8_t *data = malloc(cap + 64);
    assert(data);
    for (size_t i = 0; i < cap + 64; i++) data[i] = next_random();
    assert(crc32_under_test(0, "123456789", 9) == 0xcbf43926);
    const uint32_t seeds[] = {0, 1, 0xffffffff, 0x12345678, 0xdeadbeef};
    for (size_t k = 0; k < sizeof(seeds) / sizeof(seeds[0]); k++) {
        check(seeds[k], NULL, 0);
        for (size_t a = 0; a < 16; a++) {
            for (size_t n = 0; n <= 257; n++) check(seeds[k], data + a, n);
            const size_t lengths[] = {511, 512, 513, 4095, 4096, 4097, 65535, 65536, 65537};
            for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) check(seeds[k], data + a, lengths[i]);
        }
    }
    for (unsigned i = 0; i < 500; i++) check(next_random(), data + (next_random() & 15), next_random() % 65538);
    check(0, data, cap - 4096);
    check(0x12345678, data + 1, cap - 4097);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *guard = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(guard != MAP_FAILED && mprotect(guard + page, page, PROT_NONE) == 0);
    memcpy(guard, data, page);
    for (size_t n = 0; n <= 257; n++) check(0x5a5a5a5a, guard + page - n, n);
    assert(munmap(guard, page * 2) == 0);
    printf("%llu CRC checks passed (hardware path: %d)\n", (unsigned long long)checks, TEST_CRC_HARDWARE);
    free(data);
}
'''

with tempfile.TemporaryDirectory(prefix="hackios-crc32-") as tmp:
    path = Path(tmp)
    (path / "crc32.c").write_text(source)
    (path / "test.c").write_text(checks)
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
                    f"-DTEST_CRC_HARDWARE={int(hardware)}", str(path / "crc32.c"),
                    str(path / "test.c"), "-lz", "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)

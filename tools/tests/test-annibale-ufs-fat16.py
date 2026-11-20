#!/usr/bin/env python3
"""Host test of the FAT16 firmware reader (annibale_ufs_rw.inc): the production functions over a volume built here."""
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "xnu/files/bsd/dev/annibale/annibale_ufs_rw.inc").read_text()
FAT = SOURCE[SOURCE.index("static bool\nufs_fat83_name("):SOURCE.index("extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);")]

BPS, SPC, RESERVED, FATS, ROOT_ENTRIES, FAT_SECTORS, CLUSTERS = 512, 4, 1, 2, 64, 8, 900
CLUSTER = BPS * SPC


class Volume:
    def __init__(self):
        self.fat = [0xFFF8, 0xFFFF] + [0] * CLUSTERS
        self.data = bytearray(CLUSTERS * CLUSTER)
        self.root = bytearray()
        self.next = 2

    def chain(self, content, scattered=False):
        """Clusters for `content`; scattered leaves a gap after each so the chain is not contiguous."""
        count = max(1, -(-len(content) // CLUSTER))
        clusters = []
        for _ in range(count):
            clusters.append(self.next)
            self.next += 2 if scattered else 1
        for i, cluster in enumerate(clusters):
            self.fat[cluster] = clusters[i + 1] if i + 1 < count else 0xFFFF
            piece = content[i * CLUSTER:(i + 1) * CLUSTER]
            self.data[(cluster - 2) * CLUSTER:(cluster - 2) * CLUSTER + len(piece)] = piece
        return clusters[0]

    @staticmethod
    def entries(long_name, short11, attributes, cluster, size):
        out = bytearray()
        if long_name is not None:
            checksum = 0
            for byte in short11:
                checksum = ((checksum >> 1) | ((checksum & 1) << 7)) + byte & 0xFF
            units = [ord(c) for c in long_name] + [0]
            while len(units) % 13:
                units.append(0xFFFF)
            pieces = [units[i:i + 13] for i in range(0, len(units), 13)]
            for index in range(len(pieces), 0, -1):
                piece = pieces[index - 1]
                entry = bytearray(32)
                entry[0] = index | (0x40 if index == len(pieces) else 0)
                entry[11], entry[13] = 0x0F, checksum
                for k, at in enumerate([1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]):
                    struct.pack_into("<H", entry, at, piece[k])
                out += entry
        entry = bytearray(32)
        entry[0:11] = short11
        entry[11] = attributes
        struct.pack_into("<H", entry, 26, cluster)
        struct.pack_into("<I", entry, 28, size)
        return out + entry

    def image(self):
        boot = bytearray(BPS)
        struct.pack_into("<HBHBHHBH", boot, 11, BPS, SPC, RESERVED, FATS, ROOT_ENTRIES, 0, 0xF8, FAT_SECTORS)
        fat = b"".join(struct.pack("<H", value) for value in self.fat).ljust(FAT_SECTORS * BPS, b"\0")
        root = bytes(self.root).ljust(ROOT_ENTRIES * 32, b"\0")
        return bytes(boot) + fat * FATS + root + bytes(self.data)


def pattern(size, seed):
    return bytes((i * seed + 5) & 0xFF for i in range(size))


volume = Volume()
mdt = pattern(700, 3)
static = pattern(3 * CLUSTER + 123, 7)            # several clusters, scattered
dig = pattern(40, 11)
# image/modem_pr/mcfg/configs/mcfg_ddr/rf/ with long names, as on the device's volume
rf = Volume.entries("rf_static_940_00_00.mbn", b"RF_STA~1MBN", 0x20, volume.chain(static, scattered=True), len(static))
rf_dir = volume.chain(bytes(rf))
ddr_dir = volume.chain(bytes(Volume.entries(None, b"RF         ", 0x10, rf_dir, 0)))
# a directory of two clusters: 80 fillers before the entry looked for
fillers = b"".join(Volume.entries("filler_file_number_%03d.bin" % i, b"FILLER~1BIN", 0x20, 0, 0) for i in range(80))
configs = fillers + Volume.entries("mcfg_ddr", b"MCFG_DDR   ", 0x10, ddr_dir, 0)
assert CLUSTER < len(configs) <= 8 * CLUSTER
configs_dir = volume.chain(bytes(configs), scattered=True)
hw = Volume.entries("mbn_hw.dig", b"MBN_HW  DIG", 0x20, volume.chain(dig), len(dig))
mcfg_dir = volume.chain(bytes(Volume.entries(None, b"CONFIGS    ", 0x10, configs_dir, 0) + hw))
pr_dir = volume.chain(bytes(Volume.entries(None, b"MCFG       ", 0x10, mcfg_dir, 0)))
image_entries = (Volume.entries(None, b"MODEM   MDT", 0x20, volume.chain(mdt), len(mdt))
                 + b"\xe5" + bytes(31)
                 + Volume.entries("Modem_PR", b"MODEM_PR   ", 0x10, pr_dir, 0)
                 + Volume.entries("image", b"IMAGE~1    ", 0x20, 0, 0))     # a FILE called image: not the directory
image_dir = volume.chain(bytes(image_entries))
volume.root += Volume.entries(None, b"NON-HLOS   ", 0x08, 0, 0)                 # the volume label
volume.root += Volume.entries(None, b"IMAGE      ", 0x10, image_dir, 0)

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define Z_WAITOK 0
static void *kalloc_data(size_t size, int flags) { (void)flags; return malloc(size); }
static void kfree_data(void *p, size_t size) { (void)size; free(p); }
static uint8_t *disk; static size_t disk_size; static unsigned reads;
static int annibale_ufs_part_read(const char *part, uint64_t offset, void *buf, uint64_t len) {
    if (strcmp(part, "modemfirmware_a")) return ENOENT;
    if (offset + len > disk_size) return EINVAL;
    memcpy(buf, disk + offset, len); reads++; return 0;
}
@FAT@
static uint8_t out[65536];
static void expect(const char *path, const char *file) {
    static uint8_t want[65536]; uint64_t size = 0;
    FILE *f = fopen(file, "rb"); assert(f); size_t n = fread(want, 1, sizeof(want), f); fclose(f);
    assert(annibale_ufs_fat16_path_read(path, &size, NULL, 0) && size == n);
    memset(out, 0xee, sizeof(out));
    assert(annibale_ufs_fat16_path_read(path, &size, out, sizeof(out)) && size == n && !memcmp(out, want, n) && out[n] == 0xee);
    assert(!annibale_ufs_fat16_path_read(path, &size, out, n - 1));
}
int main(int argc, char **argv) {
    assert(argc == 5);
    FILE *f = fopen(argv[1], "rb"); assert(f); fseek(f, 0, SEEK_END); disk_size = (size_t)ftell(f); rewind(f);
    disk = malloc(disk_size); assert(fread(disk, 1, disk_size, f) == disk_size); fclose(f);
    uint64_t size = 0;
    expect("image/modem.mdt", argv[2]);
    expect("IMAGE/MODEM.MDT", argv[2]);
    expect("image/modem_pr/mcfg/configs/mcfg_ddr/rf/rf_static_940_00_00.mbn", argv[3]);
    expect("image/MODEM_PR/mcfg/configs/mcfg_ddr/rf/RF_STATIC_940_00_00.MBN", argv[3]);
    expect("image/modem_pr/mcfg/mbn_hw.dig", argv[4]);
    assert(annibale_ufs_fat16_fw_read("modem.mdt", &size, NULL, 0) && size == 700);
    assert(!annibale_ufs_fat16_fw_read("absent.mdt", &size, NULL, 0));
    assert(!annibale_ufs_fat16_path_read("image/modem_pr/mcfg/configs/mcfg_ddr/rf", &size, NULL, 0));      /* a directory */
    assert(!annibale_ufs_fat16_path_read("image/modem.mdt/x", &size, NULL, 0));                           /* a file as a directory */
    assert(!annibale_ufs_fat16_path_read("image/image", &size, NULL, 0));                                 /* empty file */
    assert(!annibale_ufs_fat16_path_read("image//modem.mdt", &size, NULL, 0) && !annibale_ufs_fat16_path_read("", &size, NULL, 0));
    assert(!annibale_ufs_fat16_path_read("image/modem_pr/mcfg/configs/filler_file_number_003.bin", &size, NULL, 0));
    assert(!annibale_ufs_fat16_path_read("non-hlos", &size, NULL, 0));
    puts("FAT16 firmware volume: 8.3 and long names, nested and multi-cluster directories, scattered files, refusals: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="ufs-fat16-") as directory:
    work = Path(directory)
    (work / "disk.img").write_bytes(volume.image())
    (work / "mdt").write_bytes(mdt)
    (work / "static").write_bytes(static)
    (work / "dig").write_bytes(dig)
    (work / "test.c").write_text(HARNESS.replace("@FAT@", FAT))
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-fsanitize=address,undefined", str(work / "test.c"), "-o", str(work / "test")], check=True)
    subprocess.run([str(work / "test"), str(work / "disk.img"), str(work / "mdt"), str(work / "static"),
                    str(work / "dig")], check=True)

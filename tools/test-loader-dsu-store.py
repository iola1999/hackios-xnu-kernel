#!/usr/bin/env python3
"""Host test of the loader's DSU store (xnuloader/loader_dsu.inc, dsu_store_open and dsu_store_io).

The functions are compiled as the loader has them, against a userdata stand-in that keeps the
blocks it was given in memory. Covered: the store is listed and writable over a fresh slot; it is
not listed without lp_metadata, with a slot smaller than 16 GiB, or when the slot does not begin
with the run's system image; it is read only when a header block holds something else; writes
across an extent boundary land in the right sectors; a request past the slot fails.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "xnuloader/loader_dsu.inc").read_text()


def function(name):
    match = re.search(r"static[^;{}]*\n" + name + r"\([^;]*?\)\n\{", SOURCE)
    assert match, name
    level, pos = 1, match.end()
    while level:
        level += (SOURCE[pos] == "{") - (SOURCE[pos] == "}")
        pos += 1
    return SOURCE[match.start():pos] + "\n"


def define(name):
    match = re.search(r"#define " + name + r"\s+(\([^)]*\)|\S+)", SOURCE)
    assert match, name
    return f"#define {name} {match.group(1)}\n"


PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EFIAPI
typedef uint64_t efi_status;
#define EFI_ERROR(x) ((int64_t)(x) < 0)
typedef struct { uint32_t media_id; uint8_t removable, present, logical, read_only; uint32_t block_size, io_align; uint64_t last_block; } efi_block_media;
typedef struct efi_block_io {
	uint64_t revision; efi_block_media *media; void *reset;
	efi_status (EFIAPI *read_blocks)(struct efi_block_io *, uint32_t, uint64_t, uint64_t, void *);
	efi_status (EFIAPI *write_blocks)(struct efi_block_io *, uint32_t, uint64_t, uint64_t, void *);
	efi_status (EFIAPI *flush_blocks)(struct efi_block_io *);
} efi_block_io;
struct loader_block_part { char name[36]; uint32_t block_size; uint64_t blocks, start_lba; uint16_t unit; uint8_t lun, flags; uint32_t reserved; };
#define DSU_MAX_EXTENTS 1024
static struct { uint64_t sector, count; } dsu_extent[DSU_MAX_EXTENTS];
static unsigned dsu_extents;
static uint64_t dsu_bytes;
static uint8_t ufs_buffer[1024 * 1024];
#define RESIDENT_RD_OFFSET 0
static uint8_t image[65536];
static uint64_t win_base, resident_rd_size;
static void print(const char *f, ...) { (void)f; }

/* userdata: 4096-byte blocks, only those written or set up kept (the rest read as zeros) */
#define BS 4096
#define KEPT 64
static struct { uint64_t lba; uint8_t data[BS]; } kept[KEPT];
static unsigned nkept;
static uint64_t writes_at[16]; static unsigned nwrites;
static uint8_t *block_of(uint64_t lba, int make) {
	for (unsigned i = 0; i < nkept; i++) if (kept[i].lba == lba) return kept[i].data;
	if (!make || nkept == KEPT) return NULL;
	kept[nkept].lba = lba; memset(kept[nkept].data, 0, BS); return kept[nkept++].data;
}
static efi_status rd(efi_block_io *b, uint32_t m, uint64_t lba, uint64_t n, void *buf) {
	for (uint64_t i = 0; i < n / BS; i++) { uint8_t *p = block_of(lba + i, 0); if (p) memcpy((uint8_t *)buf + i * BS, p, BS); else memset((uint8_t *)buf + i * BS, 0, BS); }
	return 0;
}
static efi_status wr(efi_block_io *b, uint32_t m, uint64_t lba, uint64_t n, void *buf) {
	if (nwrites < 16) writes_at[nwrites++] = lba;
	for (uint64_t i = 0; i < n / BS; i++) memcpy(block_of(lba + i, 1), (uint8_t *)buf + i * BS, BS);
	return 0;
}
static efi_status fl(efi_block_io *b) { return 0; }
static efi_block_media media = { .block_size = BS, .last_block = (1ULL << 40) / BS };
static efi_block_io userdata = { .media = &media, .read_blocks = rd, .write_blocks = wr, .flush_blocks = fl };
static int have_metadata = 1;
static int dsu_find(efi_block_io **data) { if (!have_metadata) return 0; *data = &userdata; return 1; }
"""

MAIN = r"""
static unsigned checks;
#define CK(x) do { checks++; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
/* two extents: 12 GiB + 1 MiB at sector 0x100000, the rest of 16 GiB at sector 0x9000000 */
static void slot(uint64_t total) {
	uint64_t first = (12ULL << 30) + (1ULL << 20);
	nkept = 0; nwrites = 0; have_metadata = 1;
	dsu_extents = 2;
	dsu_extent[0].sector = 0x100000; dsu_extent[0].count = first / 512;
	dsu_extent[1].sector = 0x9000000; dsu_extent[1].count = (total - first) / 512;
	dsu_bytes = total;
	for (unsigned i = 0; i < sizeof(image); i++) image[i] = (uint8_t)(i * 7);
	memcpy(image + 32, "NXSB", 4);
	win_base = (uint64_t)(uintptr_t)image; resident_rd_size = 5ULL << 30;
	for (unsigned b = 0; b < sizeof(image) / BS; b++) memcpy(block_of(0x100000 / 8 + b, 1), image + b * BS, BS);
}
static uint64_t store_lba(uint64_t at) {  /* userdata block of the slot's byte at */
	uint64_t first = dsu_extent[0].count * 512;
	return at < first ? (dsu_extent[0].sector * 512 + at) / BS : (dsu_extent[1].sector * 512 + at - first) / BS;
}
int main(void) {
	struct loader_block_part bp; int w; static uint8_t buf[3 * BS];

	slot(16ULL << 30);
	CK(dsu_store_open(&bp, 1, &w) == 1 && w == 1);
	CK(strcmp(bp.name, "dsu_store") == 0 && bp.block_size == BS && bp.blocks == (4ULL << 30) / BS && bp.unit == 0xffff);
	CK(dsu_store_open(&bp, 0, &w) == 1 && w == 0);

	/* three blocks from store block 255: 256 blocks (1 MiB) of the store are in the first extent */
	memset(buf, 0xa5, sizeof(buf));
	CK(dsu_store_io(1, 255, sizeof(buf), buf) == 0);
	CK(nwrites == 2 && writes_at[0] == store_lba((12ULL << 30) + 255 * BS) && writes_at[1] == store_lba((12ULL << 30) + 256 * BS));
	CK(writes_at[1] == 0x9000000 / 8);
	memset(buf, 0, sizeof(buf));
	CK(dsu_store_io(0, 255, sizeof(buf), buf) == 0 && buf[0] == 0xa5 && buf[sizeof(buf) - 1] == 0xa5);
	CK(EFI_ERROR(dsu_store_io(0, (4ULL << 30) / BS - 1, 2 * BS, buf)));

	/* a header block left as var_store writes it still takes writes */
	memcpy(block_of(store_lba(12ULL << 30), 1), "RDWVAR\0\0", 8);
	CK(dsu_store_open(&bp, 1, &w) == 1 && w == 1);
	/* something else in the second header: read only */
	block_of(store_lba(14ULL << 30), 1)[100] = 1;
	CK(dsu_store_open(&bp, 1, &w) == 1 && w == 0);

	slot(16ULL << 30); have_metadata = 0;
	CK(dsu_store_open(&bp, 1, &w) == 0 && w == 0);
	slot(15ULL << 30);
	CK(dsu_store_open(&bp, 1, &w) == 0);
	slot(16ULL << 30); image[40000] ^= 1;
	CK(dsu_store_open(&bp, 1, &w) == 0);
	slot(16ULL << 30); memset(block_of(0x100000 / 8, 1), 0, BS);
	CK(dsu_store_open(&bp, 1, &w) == 0);
	slot(16ULL << 30); resident_rd_size = (12ULL << 30) + 1;
	CK(dsu_store_open(&bp, 1, &w) == 0);
	slot(16ULL << 30); media.read_only = 1;
	CK(dsu_store_open(&bp, 1, &w) == 1 && w == 0);
	media.read_only = 0;
	printf("loader dsu_store: %u checks passed\n", checks);
	return 0;
}
"""


def main():
    names = ["dsu_slot_io", "dsu_store_io", "dsu_block_is_zero_or_head", "dsu_store_open"]
    defines = "".join(define(n) for n in ["DSU_STORE_AT", "DSU_STORE_BYTES", "DSU_STORE_SLOT", "DSU_HEAD_CHECK"])
    statics = re.search(r"static efi_block_io \*dsu_store_bio;.*?static unsigned dsu_store_extents;\n",
                        SOURCE, re.S).group(0)
    code = PRELUDE + defines + statics + "".join(function(n) for n in names) + MAIN
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d) / "t.c", Path(d) / "t"
        src.write_text(code)
        subprocess.run(["cc", "-Wall", "-Wno-unused-function", "-Wno-unused-parameter", "-o", exe, src], check=True)
        sys.exit(subprocess.run([exe]).returncode)


if __name__ == "__main__":
    main()

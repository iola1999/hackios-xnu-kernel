#!/usr/bin/env python3
"""Compile the loader's NVRAM bank reader (xnuloader/loader_04_fault_payload_nvram.inc) with host stubs.

The partition is a byte array behind a stub Block I/O. Cases: no darwin_nvram, empty banks, the
newer of two whole banks, a bank whose header block was not written yet (AnnibaleNVRAM writes it
last), a bank whose data was cut short, and a bad header checksum. Every image handed out is
checked here with zlib's Adler-32 and the CHRP checksum IONVRAMCHRPHandler.cpp uses.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'xnuloader/loader_04_fault_payload_nvram.inc').read_text()
BANK = 0x2000


def function(source, name):
    match = re.search(r'static[^;{}]*\n' + name + r'\([^;]*?\)\n\{', source)
    assert match, name
    level, pos = 1, match.end()
    while level:
        level += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NVRAM_BANK_SIZE 0x2000u
#define KERNEL_LIVE_MAGIC2 0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC3 0x334556494c554e58ULL
#define KERNEL_LIVE_MAGIC4 0x344556494c554e58ULL
#define KERNEL_LIVE_MAGIC5 0x354556494c554e58ULL
#define KERNEL_LIVE_MAGIC6 0x364556494c554e58ULL
#define EFI_ERROR(x) ((x) != 0)
typedef uint64_t efi_status;
struct kernel_live {
	uint64_t magic, heartbeat, log, log_len, log_ring_at, log_wrapped, log_head, log_ring;
	uint64_t recording, stop, magic2, sound, magic3, cmd_seq;
	char cmd[120];
	uint64_t magic4, inbox, inbox_size, inbox_len, inbox_seq;
	uint64_t magic5, scanout_pa;
	uint64_t magic6;
	uint32_t scanout_width, scanout_height, scanout_stride, scanout_format;
};
typedef struct { uint32_t media_id; int read_only; } efi_block_media;
typedef struct efi_block_io {
	efi_block_media *media;
	efi_status (*read_blocks)(struct efi_block_io *, uint32_t, uint64_t, uint64_t, void *);
	efi_status (*write_blocks)(struct efi_block_io *, uint32_t, uint64_t, uint64_t, void *);
	efi_status (*flush_blocks)(struct efi_block_io *);
} efi_block_io;
static struct { struct { uint32_t block_size; uint64_t blocks; } part[4]; } block_shared;
static efi_block_io *block_bios[4];
static const struct kernel_live *running_live;
static int kernel_resources_held, firmware_fault, flushed_count;
static int block_enabled = 1, block_darwin_nvram_part = -1, list_is_uefis = 1;
static uint8_t block_buffer[4 << 20], disk[64 << 10];
static unsigned str_len(const char *s) { return (unsigned)strlen(s); }
static int hc_list_is_uefis(void) { return list_is_uefis; }
static void flush_dcache(uint64_t a, uint64_t b) { (void)a; (void)b; }
static void print(const char *f, ...) { (void)f; }
static efi_status
read_disk(efi_block_io *b, uint32_t m, uint64_t lba, uint64_t n, void *to)
{
	(void)b; (void)m;
	if ((lba * 4096 + n) > sizeof(disk)) return 1;
	memcpy(to, disk + lba * 4096, n);
	return 0;
}
static efi_status
write_disk(efi_block_io *b, uint32_t m, uint64_t lba, uint64_t n, void *from)
{
	(void)b; (void)m;
	if ((lba * 4096 + n) > sizeof(disk)) return 1;
	memcpy(disk + lba * 4096, from, n);
	return 0;
}
static efi_status
flush_disk(efi_block_io *b)
{
	(void)b;
	flushed_count++;
	return 0;
}
static efi_block_media media;
static efi_block_io bio = { &media, read_disk, write_disk, flush_disk };
'''

MAIN = r'''
int
main(int argc, char **argv)
{
	static uint8_t image[NVRAM_BANK_SIZE];
	uint32_t banks = 0, bank;
	FILE *f;

	if (argc == 2 && strcmp(argv[1], "--self-extra") == 0) {
		struct kernel_live live;
		uint64_t fb = 0xfc800000ULL, len = 1156ULL * 2510ULL * 4ULL;

		memset(disk, 0x55, 0x4000);
		block_darwin_nvram_part = 0;
		block_shared.part[0].block_size = 4096;
		block_shared.part[0].blocks = sizeof(disk) / 4096;
		block_bios[0] = &bio;
		if (nvram_fastboot_consume() != 0) return 10;
		if (nvram_fastboot_set() != 1 || flushed_count != 1) return 11;
		for (int i = 0; i < 0x4000; i++) {
			if (disk[i] != 0x55) return 12; /* CHRP banks [0, 0x4000) untouched */
		}
		if (nvram_fastboot_consume() != 1 || flushed_count != 2) return 13;
		if (nvram_fastboot_consume() != 0) return 14;
		kernel_resources_held = 1;
		if (nvram_fastboot_set() != 0) return 15;
		kernel_resources_held = 0;
		firmware_fault = 1;
		if (nvram_fastboot_set() != 0) return 16;
		firmware_fault = 0;
		list_is_uefis = 0;
		if (nvram_fastboot_set() != 0) return 17;
		list_is_uefis = 1;

		memset(&live, 0, sizeof(live));
		if (screenshot_base(NULL, fb, len) != fb) return 20;
		if (screenshot_base(&live, fb, len) != fb) return 21;
		live.magic2 = KERNEL_LIVE_MAGIC2;
		live.magic3 = KERNEL_LIVE_MAGIC3;
		live.magic4 = KERNEL_LIVE_MAGIC4;
		live.magic5 = KERNEL_LIVE_MAGIC5;
		live.scanout_pa = 0;
		if (screenshot_base(&live, fb, len) != fb) return 22;
		live.scanout_pa = 0xfd400000ULL;
		if (screenshot_base(&live, fb, len) != 0xfd400000ULL) return 23;
		live.scanout_pa = 0xfd400001ULL; /* unaligned */
		if (screenshot_base(&live, fb, len) != fb) return 24;
		live.scanout_pa = fb + SPLASH_REGION_BYTES - len + 4096ULL; /* out of splash bounds */
		if (screenshot_base(&live, fb, len) != fb) return 25;

		/* Live geometry: missing magic6 or zero dimensions fall back to boot 1156x2510 BGRA */
		uint32_t sw = 0, sh = 0, ss = 0, sfmt = 0;
		uint64_t stage_bytes = 0;
		live.scanout_pa = 0xfd400000ULL;
		live.scanout_width = 1206;
		live.scanout_height = 2622;
		live.scanout_stride = 4864;
		live.scanout_format = 0x6c313072u; /* 'l10r' */
		screenshot_info(&live, fb, 1156, 2510, 4624, &sw, &sh, &ss, &sfmt);
		if (sw != 1156 || sh != 2510 || ss != 4624 || sfmt != 0x42475241u) return 26;

		live.magic6 = KERNEL_LIVE_MAGIC6;
		screenshot_info(&live, fb, 1156, 2510, 4624, &sw, &sh, &ss, &sfmt);
		if (sw != 1206 || sh != 2622 || ss != 4864 || sfmt != 0x6c313072u) return 27;
		if (screenshot_stage(&live, fb, 1156, 2510, 4624, &stage_bytes) != 0xfd400000ULL ||
		    stage_bytes != (uint64_t)4864 * 2622) return 28;

		live.scanout_format = 0; /* 0 = boot BGRA */
		screenshot_info(&live, fb, 1156, 2510, 4624, &sw, &sh, &ss, &sfmt);
		if (sfmt != 0x42475241u) return 29;

		live.scanout_format = 0x52474241u; /* unknown fourcc rejected */
		screenshot_info(&live, fb, 1156, 2510, 4624, &sw, &sh, &ss, &sfmt);
		if (sw != 1156 || sh != 2510 || ss != 4624 || sfmt != 0x42475241u) return 30;
		live.scanout_format = 0x42475241u;

		live.scanout_stride = 1206 * 4 - 4; /* narrower than width*4 */
		screenshot_info(&live, fb, 1156, 2510, 4624, &sw, &sh, &ss, &sfmt);
		if (sw != 1156) return 31;
		live.scanout_stride = 4864;

		live.scanout_pa = 0; /* scanout_pa == 0 uses boot SCREEN */
		screenshot_info(&live, fb, 1156, 2510, 4624, &sw, &sh, &ss, &sfmt);
		if (sw != 1156 || sh != 2510 || ss != 4624) return 32;
		return 0;
	}
	f = fopen(argv[1], "rb");
	if (f != NULL) {
		fread(disk, 1, sizeof(disk), f);
		fclose(f);
		block_darwin_nvram_part = 0;
		block_shared.part[0].block_size = 4096;
		block_shared.part[0].blocks = sizeof(disk) / 4096;
		block_bios[0] = &bio;
	}
	bank = nvram_load(image, &banks);
	printf("%u %u\n", bank, banks);
	fwrite(image, 1, sizeof(image), stdout);
	return argc < 2;
}
'''


def chrp_sum(h):
    s = h[0] + sum(h[2:16])
    while s > 0xff:
        s = (s & 0xff) + (s >> 8)
    return s


def bank(generation, payload=b''):
    """A whole bank as IONVRAMCHRPHandler writes it: the Apple header, then the common partition."""
    b = bytearray(BANK)
    b[0], b[2:4], b[4:9] = 0x5a, (2).to_bytes(2, 'little'), b'nvram'
    b[1] = chrp_sum(b)
    c = 0x20
    b[c], b[c + 2:c + 4], b[c + 4:c + 10] = 0x70, ((BANK - 0x20) // 16).to_bytes(2, 'little'), b'common'
    b[c + 1] = chrp_sum(b[c:c + 16])
    b[0x30:0x30 + len(payload)] = payload
    b[20:24] = generation.to_bytes(4, 'little')
    b[16:20] = zlib.adler32(bytes(b[20:])).to_bytes(4, 'little')
    return b


def check_image(img):
    assert img[0] == 0x5a and img[4:9] == b'nvram' and img[1] == chrp_sum(img), 'header'
    assert int.from_bytes(img[16:20], 'little') == zlib.adler32(img[20:]), 'adler'
    return int.from_bytes(img[20:24], 'little')


def main():
    funcs = ''.join(function(SOURCE, n) for n in (
        'nvram_chrp_sum', 'nvram_chrp_header', 'nvram_adler', 'build_nvram_image', 'nvram_bank_whole', 'nvram_load',
        'nvram_fastboot_set', 'nvram_fastboot_consume', 'screenshot_info', 'screenshot_base', 'screenshot_stage'))
    defines = '\n'.join(re.findall(r'#define (?:NVRAM_BANKS|NVRAM_FASTBOOT_AT|NVRAM_FASTBOOT_MAGIC|SPLASH_REGION_BYTES) [^\n]+', SOURCE))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 't.c').write_text(PRELUDE + defines + '\n' + funcs + MAIN)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-Wno-unused-function', '-o', str(tmp / 't'),
                        str(tmp / 't.c')], check=True)

        def run(disk):
            path = tmp / 'disk'
            if disk is None:
                path = tmp / 'none'
            else:
                path.write_bytes(bytes(disk) + bytes(64 * 1024 - len(disk)))
            out = subprocess.run([str(tmp / 't'), str(path)], capture_output=True).stdout
            line, img = out.split(b'\n', 1)
            chosen, banks = map(int, line.split())
            return chosen, banks, check_image(img), img

        old, new = bank(5, b'\x01a=1\x00'), bank(6, b'\x01a=2\x00')
        cases = []
        r = run(None)
        cases.append(('no darwin_nvram', r[:3] == (0, 1, 1)))
        r = run(bytes(2 * BANK))
        cases.append(('empty banks', r[:3] == (0, 2, 1)))
        r = run(old + new)
        cases.append(('newer bank 1', r[:3] == (1, 2, 6) and r[3] == bytes(new)))
        r = run(new + old)
        cases.append(('newer bank 0', r[:3] == (0, 2, 6)))
        torn = bytearray(bank(4)[:4096]) + new[4096:]      # header block still the old one
        r = run(old + torn)
        cases.append(('header block not written yet', r[:3] == (0, 2, 5) and r[3] == bytes(old)))
        cut = new[:4096] + b'\xaa' * 4096                  # header written, its second block not
        r = run(old + cut)
        cases.append(('data cut short', r[:3] == (0, 2, 5)))
        bad = bytearray(new)
        bad[1] ^= 0xff
        r = run(old + bad)
        cases.append(('bad header checksum', r[:3] == (0, 2, 5)))
        extra = subprocess.run([str(tmp / 't'), '--self-extra'], capture_output=True)
        cases.append(('fastboot one-shot marker & screenshot_base bounds', extra.returncode == 0))
    failed = [name for name, ok in cases if not ok]
    for name, ok in cases:
        print(('PASS ' if ok else 'FAIL ') + name)
    if failed:
        raise SystemExit('loader nvram: %d failed' % len(failed))
    print('loader nvram: %d cases passed' % len(cases))


if __name__ == '__main__':
    main()

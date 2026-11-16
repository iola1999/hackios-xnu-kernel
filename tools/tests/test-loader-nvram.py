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
#define EFI_ERROR(x) ((x) != 0)
typedef uint64_t efi_status;
typedef struct { uint32_t media_id; } efi_block_media;
typedef struct efi_block_io {
	efi_block_media *media;
	efi_status (*read_blocks)(struct efi_block_io *, uint32_t, uint64_t, uint64_t, void *);
} efi_block_io;
static struct { struct { uint32_t block_size; uint64_t blocks; } part[4]; } block_shared;
static efi_block_io *block_bios[4];
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
static efi_block_media media;
static efi_block_io bio = { &media, read_disk };
'''

MAIN = r'''
int
main(int argc, char **argv)
{
	static uint8_t image[NVRAM_BANK_SIZE];
	uint32_t banks = 0, bank;
	FILE *f = fopen(argv[1], "rb");

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
        'nvram_chrp_sum', 'nvram_chrp_header', 'nvram_adler', 'build_nvram_image', 'nvram_bank_whole', 'nvram_load'))
    define = re.search(r'#define NVRAM_BANKS [^\n]+', SOURCE).group(0)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 't.c').write_text(PRELUDE + define + '\n' + funcs + MAIN)
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
    failed = [name for name, ok in cases if not ok]
    for name, ok in cases:
        print(('PASS ' if ok else 'FAIL ') + name)
    if failed:
        raise SystemExit('loader nvram: %d failed' % len(failed))
    print('loader nvram: %d cases passed' % len(cases))


if __name__ == '__main__':
    main()

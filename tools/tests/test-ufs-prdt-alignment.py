#!/usr/bin/env python3
"""Check PRDT alignment and the slot-buffer path for block reads and writes."""
from pathlib import Path
import subprocess
import tempfile

DRIVER = Path(__file__).resolve().parents[2] / "xnu/files/bsd/dev/annibale"


def function(source, name):
    start = source.index("\n" + name + "(") + 1
    return source[start:source.index("\n}", start) + 2]


HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef char *caddr_t;
typedef uint32_t ppnum_t, upl_page_info_t, *upl_t;
typedef uintptr_t vm_offset_t;
#define PAGE_SHIFT 12
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1)
#define PRD_MAX (256 * 1024)
#define B_READ 1

static uint32_t pages[] = {3, 1, 5, 2, 4};
static _Alignas(PAGE_SIZE) char memory[5 * PAGE_SIZE];
static char disk[4 * PAGE_SIZE];
static unsigned maps, unmapped, direct, copied, completed;
struct buf {
    unsigned offset;
    bool upl, map, read;
};
typedef struct buf *buf_t;
#define buf_count(bp) ((uint32_t)sizeof(disk))
#define buf_device(bp) 0
#define minor(dev) 0
#define buf_blkno(bp) 0
#define buf_flags(bp) ((bp)->read ? B_READ : 0)
#define buf_seterror(bp, error) assert((error) == 0)
#define buf_setresid(bp, resid) assert((resid) == 0)
#define buf_biodone(bp) (++completed)
#define buf_upl(bp) ((bp)->upl ? pages : NULL)
#define buf_uploffset(bp) ((bp)->offset)
#define upl_get_internal_page_list(upl) (upl)
#define upl_phys_page(pl, i) ((pl)[i])
#define buf_dataptr(bp) ((bp)->map ? NULL : memory + (bp)->offset)
static uint64_t kvtophys(vm_offset_t va) {
    size_t off = va - (uintptr_t)memory;
    return (uint64_t)pages[off / PAGE_SIZE] * PAGE_SIZE + off % PAGE_SIZE;
}
static int buf_map(buf_t bp, caddr_t *data) {
    *data = memory + bp->offset;
    maps++;
    return 0;
}
static void buf_unmap(buf_t bp) { (void)bp; unmapped++; }
@SG_TYPE@
static int
@BUILD@

static const bool ufs_ready = true, ufs_native_on = true;
static const uint32_t ufs_parts = 1, ufs_bsize[] = { PAGE_SIZE };
static const uint8_t ufs_lun[] = {0};
static const uint64_t ufs_lun_offset[] = {0};
static const int ufs_darwin_var_part = 0;
#define ufs_part_bytes(part) sizeof(disk)

static void transfer(char *buf, bool write) {
    if (write) memcpy(disk, buf, sizeof(disk));
    else memcpy(buf, disk, sizeof(disk));
}
static int ufshc_strategy_async(buf_t bp, uint8_t lun, uint64_t off, bool write) {
    struct ufshc_sg sg[5];
    uint32_t n = 0, bytes = 0;
    assert(lun == 0 && off == 0);
    int error = ufs_build_sg(bp, sg, 5, &n);
    if (error) return error;
    for (uint32_t i = 0; i < n; i++) {
        assert(sg[i].pa == kvtophys((uintptr_t)memory + bp->offset + bytes));
        assert((sg[i].pa % 4) == 0 && (sg[i].len % 4) == 0);
        bytes += sg[i].len;
    }
    assert(bytes == sizeof(disk));
    transfer(memory + bp->offset, write);
    direct++;
    buf_biodone(bp);
    return 0;
}
static int ufs_read(uint32_t part, uint64_t off, uint8_t *dst, uint64_t len) {
    assert(!part && !off && len == sizeof(disk));
    transfer((char *)dst, false);
    copied++;
    return 0;
}
static int ufs_write(uint32_t part, uint64_t off, const uint8_t *src, uint64_t len) {
    assert(!part && !off && len == sizeof(disk));
    memcpy(disk, src, len);
    copied++;
    return 0;
}
static void
@STRATEGY@

int main(void) {
    const unsigned offsets[] = {0, 412, 413, 414, 415, 4092, 4095};
    char expected[sizeof(disk)];
    for (size_t i = 0; i < sizeof(expected); i++) expected[i] = (char)(i * 29 + i / 100);
    for (int mode = 0; mode < 3; mode++) {
        for (int read = 0; read < 2; read++) {
            for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
                unsigned off = offsets[i];
                struct buf bp = {off, mode == 2, mode != 0, read};
                memset(memory, 0x5a, sizeof(memory));
                memset(disk, 0, sizeof(disk));
                memcpy(read ? disk : memory + off, expected, sizeof(disk));
                direct = copied = completed = maps = unmapped = 0;
                ufs_strategy(&bp);
                assert(completed == 1 && maps == unmapped);
                assert(direct == (off % 4 == 0) && copied == (off % 4 != 0));
                assert(memcmp(read ? memory + off : disk, expected, sizeof(disk)) == 0);
                if (off != 0) assert(memory[off - 1] == 0x5a);
                assert(memory[off + sizeof(disk)] == 0x5a);
            }
        }
    }
}
"""

hc = (DRIVER / "annibale_ufshc_rw.inc").read_text()
sg = hc.index("struct ufshc_sg {")
source = HARNESS.replace("@SG_TYPE@", hc[sg:hc.index("};", sg) + 2])
source = source.replace("@BUILD@", function(hc, "ufs_build_sg"))
source = source.replace("@STRATEGY@", function((DRIVER / "annibale_ufs_rw.inc").read_text(), "ufs_strategy"))
with tempfile.TemporaryDirectory() as tmp:
    cfile = Path(tmp) / "test.c"
    cfile.write_text(source)
    prog = Path(tmp) / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(prog)], check=True)
    subprocess.run([str(prog)], check=True)
print("UFS PRDT alignment: 42 block transfers passed")

#!/usr/bin/env python3
"""Host ASan+UBSan regression test for APFS apfs_wresize and va_dataprotect_class handling."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
WRITE_INC = ROOT / "xnu/files/bsd/miscfs/apfs/apfs_vnops_write.inc"
DIR_INC = ROOT / "xnu/files/bsd/miscfs/apfs/apfs_vnops_dir.inc"
VNOPS_C = ROOT / "xnu/files/bsd/miscfs/apfs/apfs_vnops.c"

STUBS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#define PAGE_SIZE_64 16384ULL
#define round_page_64(x) (((uint64_t)(x) + PAGE_SIZE_64 - 1ULL) & ~(PAGE_SIZE_64 - 1ULL))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

enum {
    IO_HEADZEROFILL = 0x80,
    IO_NOZERODIRTY = 0x100,
    IO_SYNC = 0x10,
    PROTECTION_CLASS_DEFAULT = 0,
    PROTECTION_CLASS_B = 2,
    PROTECTION_CLASS_D = 4,
    APFS_WA_PCLASS = 1u << 0,
};

struct apfs_mount { int dummy; };
struct apfs_inode { uint32_t pclass; uint32_t nchildren; };
struct apfs_node {
    struct apfs_mount *an_mount;
    uint64_t an_id;
    uint64_t an_size;
    bool an_orphan;
    struct apfs_inode an_ino;
};
typedef struct vnode {
    off_t ubc_size;
    bool is_dir;
    struct apfs_node node;
} *vnode_t;

struct vnode_attr {
    bool active_pclass;
    bool supported_pclass;
    uint32_t va_dataprotect_class;
    uint64_t va_nlink;
};
struct apfs_wattr { uint32_t set; uint32_t pclass; };
struct apfs_wnew { uint32_t pclass; };

#define vnode_isdir(vp) ((vp)->is_dir)
#define VATTR_RETURN(vap, field, val) ((vap)->field = (val))
#define VATTR_IS_ACTIVE(vap, field) ((vap)->active_pclass)
#define VATTR_SET_SUPPORTED(vap, field) ((vap)->supported_pclass = true)
#define APFS_WCALL(am, an, other, call) ((void)(am), (void)(an), (void)(other), (call))

static int mock_truncate_err = 0;
static int mock_cluster_err = 0;
static int cluster_calls = 0;
static off_t last_cluster_old = 0;
static off_t last_cluster_limit = 0;
static off_t ubc_size_at_cluster = -1;
static int ubc_setsize_calls = 0;
static uint64_t stored_disk_size = 0;

static void *apfs_wtx_of(struct apfs_mount *am) { return am; }
static int apfs_w_truncate(void *w, uint64_t id, uint64_t size) {
    (void)w; (void)id;
    if (mock_truncate_err) return mock_truncate_err;
    stored_disk_size = size;
    return 0;
}
static int ubc_setsize(vnode_t vp, off_t size) {
    ubc_setsize_calls++;
    vp->ubc_size = size;
    return 1;
}
static off_t ubc_getsize(vnode_t vp) {
    return vp->ubc_size;
}
static int cluster_write(vnode_t vp, void *uio, off_t oldEOF, off_t newEOF,
    off_t headOffset, off_t tailOffset, int xflags) {
    (void)uio; (void)tailOffset;
    assert(headOffset == oldEOF);
    assert((xflags & (IO_HEADZEROFILL | IO_NOZERODIRTY)) == (IO_HEADZEROFILL | IO_NOZERODIRTY));
    cluster_calls++;
    last_cluster_old = oldEOF;
    last_cluster_limit = newEOF;
    ubc_size_at_cluster = ubc_getsize(vp);
    assert(ubc_size_at_cluster >= newEOF);
    return mock_cluster_err;
}
"""

TESTS = r"""
int main(void) {
    struct apfs_mount am = {0};
    struct vnode v = { .ubc_size = 0, .node = { .an_mount = &am, .an_id = 42, .an_size = 0 } };

    /* 1. Grow from 0 -> 500: no partial old page to zero, UBC updated to 500. */
    assert(apfs_wresize(&v, &v.node, 500, 0) == 0);
    assert(v.node.an_size == 500 && v.ubc_size == 500 && stored_disk_size == 500);
    assert(cluster_calls == 0);

    /* 2. Grow from unaligned 500 -> 20000: ubc_setsize MUST precede cluster_write. */
    assert(apfs_wresize(&v, &v.node, 20000, 0) == 0);
    assert(cluster_calls == 1);
    assert(last_cluster_old == 500 && last_cluster_limit == (off_t)PAGE_SIZE_64);
    assert(ubc_size_at_cluster == 20000);
    assert(v.node.an_size == 20000 && v.ubc_size == 20000 && stored_disk_size == 20000);

    /* 3. Grow past exact page boundary: 20000 -> 32768 -> 40000. */
    assert(apfs_wresize(&v, &v.node, 32768, 0) == 0);
    assert(cluster_calls == 2 && last_cluster_old == 20000 && last_cluster_limit == 32768);
    assert(apfs_wresize(&v, &v.node, 40000, 0) == 0);
    assert(cluster_calls == 2 && v.ubc_size == 40000);

    /* 4. Shrink error rollback restores ubc_size and an_size. */
    mock_truncate_err = EIO;
    assert(apfs_wresize(&v, &v.node, 1000, 0) == EIO);
    assert(v.ubc_size == 40000 && v.node.an_size == 40000);
    mock_truncate_err = 0;

    /* 5. Valid shrink updates ubc_size before truncate. */
    assert(apfs_wresize(&v, &v.node, 1000, 0) == 0);
    assert(v.ubc_size == 1000 && v.node.an_size == 1000 && stored_disk_size == 1000);

    /* 6. va_dataprotect_class: PROTECTION_CLASS_DEFAULT (0) inherits dir class on create
     *    and does not clobber explicit class on setattr. */
    struct apfs_node dir = { .an_ino = { .pclass = PROTECTION_CLASS_B } };
    struct vnode_attr create_default = { .active_pclass = true, .va_dataprotect_class = PROTECTION_CLASS_DEFAULT };
    struct vnode_attr create_explicit = { .active_pclass = true, .va_dataprotect_class = PROTECTION_CLASS_D };
    assert(harness_create_pclass(&dir, &create_default) == PROTECTION_CLASS_B);
    assert(harness_create_pclass(&dir, &create_explicit) == PROTECTION_CLASS_D);

    uint32_t cur_pclass = PROTECTION_CLASS_B;
    struct vnode_attr set_default = { .active_pclass = true, .va_dataprotect_class = PROTECTION_CLASS_DEFAULT };
    struct vnode_attr set_d = { .active_pclass = true, .va_dataprotect_class = PROTECTION_CLASS_D };
    assert(harness_setattr_pclass(&cur_pclass, &set_default) == PROTECTION_CLASS_B && set_default.supported_pclass);
    assert(harness_setattr_pclass(&cur_pclass, &set_d) == PROTECTION_CLASS_D && set_d.supported_pclass);

    /* 7. S12: unlinked open file (an_orphan) reports st_nlink == 0 via getattr. */
    struct vnode link_file = { .is_dir = false, .node = { .an_orphan = false, .an_ino = { .nchildren = 0 } } };
    assert(harness_getattr_nlink(&link_file) == 1);
    link_file.node.an_ino.nchildren = 3;
    assert(harness_getattr_nlink(&link_file) == 3);
    link_file.node.an_orphan = true;
    assert(harness_getattr_nlink(&link_file) == 0);
    struct vnode link_dir = { .is_dir = true, .node = { .an_orphan = true, .an_ino = { .nchildren = 0 } } };
    assert(harness_getattr_nlink(&link_dir) == 2);
    return 0;
}
"""


def main():
    write_src = WRITE_INC.read_text()
    dir_src = DIR_INC.read_text()
    vnops_src = VNOPS_C.read_text()

    wresize_start = write_src.index("static int\napfs_wresize(")
    wresize_end = write_src.index("\n}\n", wresize_start) + 3
    wresize_fn = write_src[wresize_start:wresize_end]

    setattr_blk_start = write_src.index("\tif (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {", write_src.index("apfs_vnop_setattr("))
    setattr_blk_end = write_src.index("\n\t/* kept nowhere", setattr_blk_start)
    setattr_blk = write_src[setattr_blk_start:setattr_blk_end]

    create_blk_start = dir_src.index("\tif (VATTR_IS_ACTIVE(vap, va_dataprotect_class)", dir_src.index("apfs_wmake("))
    create_blk_end = dir_src.index("\n\tif (VATTR_IS_ACTIVE(vap, va_flags))", create_blk_start)
    create_blk = dir_src[create_blk_start:create_blk_end]

    nlink_start = vnops_src.index("\tVATTR_RETURN(vap, va_nlink,", vnops_src.index("apfs_vnop_getattr("))
    nlink_end = vnops_src.index("\n", nlink_start)
    nlink_stmt = vnops_src[nlink_start:nlink_end]

    helpers = f"""
static uint32_t harness_create_pclass(struct apfs_node *dn, struct vnode_attr *vap) {{
    struct apfs_wnew a = {{ 0 }};
{create_blk}
    return a.pclass;
}}

static uint32_t harness_setattr_pclass(uint32_t *cur, struct vnode_attr *vap) {{
    struct apfs_wattr a = {{ 0 }};
{setattr_blk}
    if (a.set & APFS_WA_PCLASS) *cur = a.pclass;
    return *cur;
}}

static uint64_t harness_getattr_nlink(vnode_t vp) {{
    struct vnode_attr va = {{ 0 }};
    struct vnode_attr *vap = &va;
    struct apfs_node *an = &vp->node;
    struct apfs_inode ino = an->an_ino;
{nlink_stmt}
    return va.va_nlink;
}}
"""

    with tempfile.TemporaryDirectory() as directory:
        c_file = Path(directory) / "apfs_wresize_cprotect_test.c"
        binary = c_file.with_suffix("")
        c_file.write_text(STUBS + "\n" + wresize_fn + "\n" + helpers + "\n" + TESTS)
        subprocess.run([
            os.environ.get("CC", "cc"),
            "-std=c11", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-Werror",
            str(c_file), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    print("APFS apfs_wresize + va_dataprotect_class ASan+UBSan checks passed")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Exercise the APFS vnode sync operations with independently failing UBC and checkpoint writes."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/files/bsd/miscfs/apfs/apfs_vnops_write.inc"
STUBS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct apfs_mount { int commits; };
struct apfs_node { struct apfs_mount *an_mount; int an_wseq; };
typedef struct vnode { bool regular; off_t size; struct apfs_node node; } *vnode_t;
struct vnop_fsync_args { vnode_t a_vp; int a_waitfor; };
enum { MNT_WAIT = 1, UBC_SYNC = 2, UBC_PUSHDIRTY = 4, APFS_WHY_FSYNC = 8, F_FULLFSYNC = 10, F_BARRIERFSYNC = 11 };
#define APFS_VP(vp) (&(vp)->node)

static bool writable = true;
static int page_error, commit_error, pushes, flushes, flags;
static int vnode_isreg(vnode_t vp) { return vp->regular; }
static off_t ubc_getsize(vnode_t vp) { return vp->size; }
static int ubc_msync(vnode_t vp, off_t start, off_t end, void *resid, int requested) {
    assert(start == 0 && end == vp->size && resid == NULL);
    pushes++;
    flags = requested;
    return page_error;
}
static void *apfs_wtx_of(struct apfs_mount *am) { return writable ? am : NULL; }
static int apfs_wcommits(struct apfs_mount *am) { return am->commits; }
static int apfs_wflush(struct apfs_mount *am) { (void)am; flushes++; return commit_error; }
static int apfs_wcommit(struct apfs_mount *am, bool push, int seq, int why) {
    assert(!push && seq > am->commits && why == APFS_WHY_FSYNC);
    return apfs_wflush(am);
}
"""
TESTS = r"""
int main(void) {
    struct apfs_mount mount = {0};
    struct vnode vnode = {true, 65536, {&mount, 2}};
    struct vnop_fsync_args args = {&vnode, MNT_WAIT};
    const int errors[] = {EIO, ENOSPC, 0};
    for (size_t i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
        page_error = errors[i];
        flushes = pushes = 0;
        assert(apfs_vnop_fsync(&args) == page_error);
        assert(ioctl_sync(F_FULLFSYNC, &vnode) == page_error);
        assert(ioctl_sync(F_BARRIERFSYNC, &vnode) == page_error);
        assert(pushes == 3 && flags == (UBC_SYNC | UBC_PUSHDIRTY));
        assert(flushes == (page_error ? 0 : 3));
    }
    commit_error = EIO;
    assert(apfs_vnop_fsync(&args) == EIO);
    assert(ioctl_sync(F_FULLFSYNC, &vnode) == EIO);
    assert(ioctl_sync(F_BARRIERFSYNC, &vnode) == EIO);

    flushes = pushes = 0;
    args.a_waitfor = 0;
    assert(apfs_vnop_fsync(&args) == 0 && flags == UBC_PUSHDIRTY);
    assert(flushes == 0 && pushes == 1);

    args.a_waitfor = MNT_WAIT;
    commit_error = 0;
    mount.commits = vnode.node.an_wseq;
    page_error = EIO;
    assert(apfs_vnop_fsync(&args) == EIO && flushes == 0);

    writable = false;
    pushes = 0;
    assert(apfs_vnop_fsync(&args) == 0 && ioctl_sync(F_FULLFSYNC, &vnode) == 0);
    assert(pushes == 0 && flushes == 0);
    writable = true;
    vnode.regular = false;
    assert(apfs_vnop_fsync(&args) == 0 && pushes == 0);
    vnode.regular = true;
    vnode.size = 0;
    assert(ioctl_sync(F_BARRIERFSYNC, &vnode) == 0 && pushes == 0 && flushes == 1);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static int\napfs_sync_pages(")
    end = source.index("\n}", source.index("int\napfs_vnop_fsync(")) + 2
    fsync = source[start:end]
    start = source.index("\tcase F_FULLFSYNC:", source.index("apfs_vnop_ioctl("))
    end = source.index("\tcase FSIOC_CAS_BSDFLAGS:", start)
    ioctl = """static int ioctl_sync(int op, vnode_t vp) {
        struct apfs_mount *am = vp->node.an_mount;
        void *w = apfs_wtx_of(am);
        switch (op) {\n""" + source[start:end] + "default: return EINVAL; } }\n"
    with tempfile.TemporaryDirectory() as directory:
        c_file = Path(directory) / "sync.c"
        binary = c_file.with_suffix("")
        c_file.write_text(STUBS + fsync + "\n" + ioctl + TESTS)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("APFS fsync, F_FULLFSYNC and F_BARRIERFSYNC checks passed")


if __name__ == "__main__":
    main()

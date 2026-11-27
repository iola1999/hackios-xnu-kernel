#!/usr/bin/env python3
"""Host ASan+UBSan tests for APFS multi-file flush error propagation and MAP_SHARED in-place sync."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
APFS_H = ROOT / "xnu/files/bsd/miscfs/apfs/apfs.h"
VFSOPS_WRITE = ROOT / "xnu/files/bsd/miscfs/apfs/apfs_vfsops_write.inc"
VFSOPS_SYNC = ROOT / "xnu/files/bsd/miscfs/apfs/apfs_vfsops_sync.inc"

STUBS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define UBC_SYNC      0x02
#define UBC_PUSHDIRTY 0x04
#define MNT_WAIT      1
#define MNT_NOWAIT    2
#define MNT_RDONLY    0x1
#define LK_NOWAIT     0x10
#define VNODE_RETURNED 0
#define APFS_WHY_OTHER_FLAG 1

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#ifndef __unused
#define __unused __attribute__((unused))
#endif

typedef int lck_mtx_t;
typedef int lck_spin_t;
typedef void *thread_call_t;
typedef struct mount *mount_t;
typedef struct vnode *vnode_t;
typedef void *vfs_context_t;

struct apfs_wtx { bool open; bool broken; };
struct apfs_nx {
    struct {
        void *ctx;
        int (*flush)(void *ctx);
    } dev;
};

struct vnode {
    bool regular;
    off_t size;
    int inject_err;
    int msync_calls;
    int last_flags;
};

struct apfs_mount {
    mount_t am_mp;
    struct apfs_wmount *am_w;
    struct apfs_nx am_nx;
    struct { const char *name; } am_vol;
};

struct mount {
    struct apfs_mount *am;
    vnode_t *vnodes;
    size_t nvnodes;
    bool busy_fail;
};

static lck_mtx_t apfs_pool_lock;
static struct apfs_mount *apfs_data_mount;

static void lck_mtx_lock(lck_mtx_t *m) { (void)m; }
static void lck_mtx_unlock(lck_mtx_t *m) { (void)m; }
static void lck_spin_lock(lck_spin_t *s) { (void)s; }
static void lck_spin_unlock(lck_spin_t *s) { (void)s; }

static struct apfs_mount *APFS_MP(mount_t mp) { return mp->am; }
static void *vfs_fsprivate(mount_t mp) { return mp->am; }
static int vfs_busy(mount_t mp, int flags) { (void)flags; return mp->busy_fail ? EBUSY : 0; }
static void vfs_unbusy(mount_t mp) { (void)mp; }
static void vfs_setflags(mount_t mp, int flags) { (void)mp; (void)flags; }
static int vnode_isreg(vnode_t vp) { return vp->regular; }
static off_t ubc_getsize(vnode_t vp) { return vp->size; }
static int ubc_msync(vnode_t vp, off_t s, off_t e, void *r, int flags) {
    (void)s; (void)e; (void)r;
    vp->msync_calls++;
    vp->last_flags = flags;
    return vp->inject_err;
}
static int vnode_iterate(mount_t mp, int flags, int (*cb)(vnode_t, void *), void *arg) {
    (void)flags;
    for (size_t i = 0; i < mp->nvnodes; i++) {
        (void)cb(mp->vnodes[i], arg);
    }
    return 0;
}

static bool apfs_wtx_dirty(const struct apfs_wtx *w) { return w != NULL && w->open; }
static bool apfs_wtx_broken(const struct apfs_wtx *w) { return w == NULL || w->broken; }

static int mock_commit_err = 0;
static int mock_flush_err = 0;
static int mock_drain_calls = 0;
static int mock_flush_calls = 0;
static int mock_tx_commit_calls = 0;
static uint64_t mock_now = 100;

static uint64_t mach_absolute_time(void) { return mock_now += 5; }
static uint64_t apfs_wio_drain(struct apfs_mount *am);
static int apfs_wtx_commit(struct apfs_wtx *w) {
    mock_tx_commit_calls++;
    if (mock_commit_err == 0) w->open = false;
    return mock_commit_err;
}
static void apfs_lock_exclusive(struct apfs_mount *am) { (void)am; }
static void apfs_unlock_exclusive(struct apfs_mount *am) { (void)am; }
static void apfs_wpush(struct apfs_mount *am) { (void)am; }
static int dev_flush_cb(void *ctx) {
    (void)ctx;
    mock_flush_calls++;
    return mock_flush_err;
}
"""


def extract_production():
    apfs_h = APFS_H.read_text()
    w_src = VFSOPS_WRITE.read_text()
    s_src = VFSOPS_SYNC.read_text()

    enum_match = re.search(r"enum apfs_wwhy \{.*?\};", apfs_h, flags=re.S)
    ctx_match = re.search(r"struct apfs_wpush_ctx \{.*?\};", apfs_h, flags=re.S)
    assert enum_match and ctx_match

    wmount_match = re.search(r"#define APFS_WMAP_MAX.*?struct apfs_wmount \{.*?\};", w_src, flags=re.S)
    assert wmount_match

    data_dirty_start = w_src.index("bool\napfs_wdata_dirty(")
    data_dirty_end = w_src.index("\n/* A data write done:", data_dirty_start)
    data_dirty_fn = w_src[data_dirty_start:data_dirty_end]

    commit_locked_start = w_src.index("int\napfs_wcommit_locked(")
    commit_locked_end = w_src.index("\n/* A file's dirty pages", commit_locked_start)
    commit_locked_fn = w_src[commit_locked_start:commit_locked_end]

    wpush_start = w_src.index("int\napfs_wpush_vnode(")
    wpush_end = w_src.index("\nstatic void\napfs_wpush(", wpush_start)
    wpush_fn = w_src[wpush_start:wpush_end]

    wcommit_start = w_src.index("int\napfs_wcommit(")
    wcommit_end = w_src.index("\nuint64_t\napfs_wcommits(", wcommit_start)
    wcommit_fn = w_src[wcommit_start:wcommit_end]

    flush_start = w_src.index("int\napfs_data_flush(void)")
    flush_end = w_src.index("\n/* sysctl kern.apfs_wstats:", flush_start)
    flush_fn = w_src[flush_start:flush_end]

    sync_start = s_src.index("static int\napfs_sync(")
    sync_end = s_src.index("\nstatic int\napfs_init(", sync_start)
    sync_fn = s_src[sync_start:sync_end]

    drain_stub = """
static uint64_t
apfs_wio_drain(struct apfs_mount *am)
{
    mock_drain_calls++;
    am->am_w->wm_nmap = 0;
    am->am_w->wm_io = 0;
    return 0;
}
"""
    return "\n".join([
        enum_match.group(),
        ctx_match.group(),
        wmount_match.group(),
        drain_stub,
        data_dirty_fn,
        commit_locked_fn,
        wpush_fn,
        wcommit_fn,
        flush_fn,
        sync_fn,
    ])


TESTS = r"""
int main(void) {
    struct apfs_wtx wtx = { .open = false, .broken = false };
    struct apfs_wmount wm = { .wm_wtx = &wtx };
    struct apfs_mount am = {
        .am_w = &wm,
        .am_nx = { .dev = { .ctx = &am, .flush = dev_flush_cb } },
        .am_vol = { .name = "Data" },
    };
    struct vnode vnodes[16];
    vnode_t vptrs[16];
    for (int i = 0; i < 16; i++) {
        vnodes[i] = (struct vnode){ .regular = true, .size = 4096 * (off_t)(i + 1) };
        vptrs[i] = &vnodes[i];
    }
    struct mount mp = { .am = &am, .vnodes = vptrs, .nvnodes = 16 };
    am.am_mp = &mp;
    apfs_data_mount = &am;

    /* 1. Multi-file flush: error on file #3 and #11 returns first error (#3) while all 16 flush. */
    vnodes[3].inject_err = ENOSPC;
    vnodes[11].inject_err = EIO;
    wtx.open = true;
    assert(apfs_data_flush() == ENOSPC);
    for (int i = 0; i < 16; i++) {
        assert(vnodes[i].msync_calls == 1);
        assert((vnodes[i].last_flags & (UBC_PUSHDIRTY | UBC_SYNC)) == (UBC_PUSHDIRTY | UBC_SYNC));
    }
    assert(mock_tx_commit_calls == 1);

    /* 2. apfs_sync(MNT_WAIT) propagates error without dropping remaining vnodes. */
    vnodes[3].inject_err = 0;
    assert(apfs_sync(&mp, MNT_WAIT, NULL) == EIO);
    for (int i = 0; i < 16; i++) {
        assert(vnodes[i].msync_calls == 2);
    }

    /* 3. MAP_SHARED in-place overwrite (w->open == false, wm_data_dirty == true):
     *    apfs_data_flush() must drain I/O and invoke dev.flush, then clear wm_data_dirty. */
    vnodes[11].inject_err = 0;
    wtx.open = false;
    wm.wm_nmap = 1;
    wm.wm_io = 1;
    wm.wm_data_dirty = true;
    mock_drain_calls = 0;
    mock_flush_calls = 0;
    mock_tx_commit_calls = 0;
    assert(apfs_data_flush() == 0);
    assert(mock_drain_calls == 1 && mock_flush_calls == 1 && mock_tx_commit_calls == 0);
    assert(!apfs_wdata_dirty(&am));

    /* 4. No-op second apfs_data_flush does not needlessly re-flush device. */
    assert(apfs_data_flush() == 0);
    assert(mock_drain_calls == 1 && mock_flush_calls == 1);

    /* 5. Failed dev.flush keeps wm_data_dirty=true and propagates error. */
    wm.wm_data_dirty = true;
    mock_flush_err = EIO;
    assert(apfs_data_flush() == EIO);
    assert(apfs_wdata_dirty(&am));
    mock_flush_err = 0;
    assert(apfs_wflush(&am) == 0 && !apfs_wdata_dirty(&am));
    return 0;
}
"""


def main():
    prod = extract_production()
    with tempfile.TemporaryDirectory() as directory:
        c_file = Path(directory) / "apfs_flush_errors_test.c"
        binary = c_file.with_suffix("")
        c_file.write_text(STUBS + "\n" + prod + "\n" + TESTS)
        subprocess.run([
            os.environ.get("CC", "cc"),
            "-std=c11", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-Werror",
            str(c_file), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    print("APFS multi-file flush error propagation & MAP_SHARED sync ASan+UBSan checks passed")


if __name__ == "__main__":
    main()

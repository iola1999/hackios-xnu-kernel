#!/usr/bin/env python3
"""Compile the production exit flush with vnode lifetime and I/O failure models."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DISK = 'xnu/files/bsd/miscfs/ramfs/ramfs_disk.inc'
UFS = 'xnu/files/bsd/dev/annibale/annibale_ufs.c'


def function(text, name):
    start = text.index('\n' + name + '(')
    start = text.rfind('\n', 0, start) + 1
    return text[start:text.index('\n}\n', start) + 3]


def source_at(path, ref):
    if ref:
        return subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{ref}:{path}'], text=True)
    return (ROOT / path).read_text()


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define RAMFS_HASH_SIZE 4
#define VREG 1
#define NULLVP NULL
#define UBC_PUSHDIRTY 1
#define UBC_SYNC 2
struct vnode { unsigned id, refs, holds, calls; int error; bool dead; };
typedef struct vnode *vnode_t;
struct ramfs_node {
    uint64_t rn_ino;
    int rn_type, rn_nlink;
    bool rn_mapped_write;
    vnode_t rn_vp;
    uint32_t rn_vid;
    struct ramfs_node *rn_hnext;
};
struct ramfs_mount {
    int rm_lock;
    bool rm_disk_enabled, rm_seed_done, rm_disk_loading;
    uint64_t rm_next_ino;
    struct ramfs_node *rm_hash[RAMFS_HASH_SIZE];
};
static struct ramfs_mount mount_state, *ramfs_disk_rm;
static struct vnode vnodes[32];
static struct ramfs_node nodes[32];
static unsigned sync_calls, get_calls, warnings, mutations;
static int metadata_error;
static bool ufs_ready, mutate, recycle, growing;
static int ufs_darwin_var_part;
static void lck_mtx_lock(int *lock) { assert(*lock == 0); *lock = 1; }
static void lck_mtx_unlock(int *lock) {
    assert(*lock == 1); *lock = 0;
    if (recycle && get_calls == 0) {
        /* Reclaim can run after the filesystem releases its hash lock. */
        assert(vnodes[0].holds == 1);
        vnodes[0].id++;
    }
}
static void vnode_hold(vnode_t vp) {
    assert(mount_state.rm_lock == 1); vp->holds++;
}
static vnode_t vnode_drop(vnode_t vp) {
    assert(mount_state.rm_lock == 0 && vp->holds == 1); vp->holds--; return vp;
}
static int vnode_getwithvid(vnode_t vp, uint32_t vid) {
    assert(mount_state.rm_lock == 0);
    get_calls++;
    if (vp->dead || vp->id != vid) return ENOENT;
    vp->refs++; return 0;
}
static void vnode_put(vnode_t vp) {
    assert(mount_state.rm_lock == 0 && vp->refs == 1); vp->refs--;
}
static long ubc_getsize(vnode_t vp) { assert(vp->refs == 1); return 4096; }
static int ubc_msync(vnode_t vp, long off, long end, void *arg, int flags) {
    assert(mount_state.rm_lock == 0 && vp->refs == 1);
    assert(off == 0 && end == 4096 && arg == NULL && flags == (UBC_PUSHDIRTY | UBC_SYNC));
    assert(++vp->calls == 1);
    if (mutate && mutations++ == 0) {
        /* Remove an unvisited node and insert a new inode while the lock is dropped. */
        lck_mtx_lock(&mount_state.rm_lock);
        struct ramfs_node **at = &mount_state.rm_hash[1];
        while (*at != &nodes[1]) at = &(*at)->rn_hnext;
        *at = nodes[1].rn_hnext;
        nodes[1].rn_hnext = (void *)(uintptr_t)1;
        nodes[20] = (struct ramfs_node){ .rn_ino = mount_state.rm_next_ino + 1,
            .rn_type = VREG, .rn_nlink = 1, .rn_mapped_write = true,
            .rn_vp = &vnodes[20], .rn_vid = vnodes[20].id,
            .rn_hnext = mount_state.rm_hash[1] };
        mount_state.rm_hash[1] = &nodes[20];
        mount_state.rm_next_ino += 4;
        lck_mtx_unlock(&mount_state.rm_lock);
    }
    if (growing) {
        unsigned i = 20 + mutations++;
        assert(i < 32);
        lck_mtx_lock(&mount_state.rm_lock);
        nodes[i] = (struct ramfs_node){ .rn_ino = mount_state.rm_next_ino + 1,
            .rn_type = VREG, .rn_nlink = 1, .rn_mapped_write = true,
            .rn_vp = &vnodes[i], .rn_vid = vnodes[i].id,
            .rn_hnext = mount_state.rm_hash[1] };
        mount_state.rm_hash[1] = &nodes[i];
        mount_state.rm_next_ino += 4;
        lck_mtx_unlock(&mount_state.rm_lock);
    }
    return vp->error;
}
static int ramfs_disk_sync(struct ramfs_mount *rm) {
    assert(rm == &mount_state && rm->rm_lock == 0); sync_calls++; return metadata_error;
}
static int log_warning(const char *format, ...) {
    assert(strstr(format, "flush") != NULL); warnings++; return 0;
}
#define printf log_warning
'''

CASES = r'''
static void setup(unsigned count) {
    memset(&mount_state, 0, sizeof(mount_state));
    mount_state.rm_disk_enabled = mount_state.rm_seed_done = true;
    mount_state.rm_next_ino = 128;
    ramfs_disk_rm = &mount_state;
    ufs_ready = true; ufs_darwin_var_part = 0;
    for (unsigned i = 0; i < 32; i++) vnodes[i].id = 100 + i;
    /* Deliberately unsorted inode numbers, all in one hash bucket. */
    for (unsigned j = 0; j < count; j++) {
        unsigned i = (j * 7) % count;
        nodes[i] = (struct ramfs_node){ .rn_ino = 1 + 4 * i, .rn_type = VREG,
            .rn_nlink = 1, .rn_mapped_write = true, .rn_vp = &vnodes[i],
            .rn_vid = vnodes[i].id, .rn_hnext = mount_state.rm_hash[1] };
        mount_state.rm_hash[1] = &nodes[i];
    }
}
static void released(void) {
    assert(mount_state.rm_lock == 0);
    for (unsigned i = 0; i < 32; i++) assert(vnodes[i].refs == 0 && vnodes[i].holds == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    if (!strcmp(name, "overflow")) {
        setup(19); assert(run_flush() == 0);
        for (unsigned i = 0; i < 19; i++) assert(vnodes[i].calls == 1);
        assert(sync_calls == 1);
    } else if (!strcmp(name, "filter")) {
        setup(11); nodes[1].rn_type = 2; nodes[2].rn_nlink = 0;
        nodes[3].rn_mapped_write = false; nodes[4].rn_vp = NULL;
        assert(run_flush() == 0);
        for (unsigned i = 0; i < 11; i++) assert(vnodes[i].calls == (i < 1 || i > 4));
        assert(sync_calls == 1);
    } else if (!strcmp(name, "stale")) {
        setup(11); vnodes[0].dead = true;
        assert(run_flush() == ENOENT && sync_calls == 0);
        for (unsigned i = 0; i < 11; i++) assert(vnodes[i].calls == (i != 0));
    } else if (!strcmp(name, "recycled")) {
        setup(11); recycle = true;
        assert(run_flush() == ENOENT && sync_calls == 0 && vnodes[0].calls == 0);
        for (unsigned i = 1; i < 11; i++) assert(vnodes[i].calls == 1);
    } else if (!strcmp(name, "data_error")) {
        setup(11); vnodes[0].error = EIO; vnodes[5].error = ENOSPC;
        assert(run_flush() == EIO && sync_calls == 0);
        for (unsigned i = 0; i < 11; i++) assert(vnodes[i].calls == 1);
    } else if (!strcmp(name, "metadata_error")) {
        setup(11); metadata_error = ENOSPC;
        assert(run_flush() == ENOSPC && sync_calls == 1);
        for (unsigned i = 0; i < 11; i++) assert(vnodes[i].calls == 1);
    } else if (!strcmp(name, "mutation")) {
        setup(11); mutate = true; assert(run_flush() == 0);
        assert(vnodes[0].calls == 1 && vnodes[1].calls == 0 && vnodes[20].calls == 0);
        for (unsigned i = 2; i < 11; i++) assert(vnodes[i].calls == 1);
        assert(sync_calls == 1);
    } else if (!strcmp(name, "growth")) {
        setup(11); growing = true; assert(run_flush() == 0);
        assert(mutations == 11 && sync_calls == 1);
        for (unsigned i = 0; i < 11; i++) assert(vnodes[i].calls == 1);
        for (unsigned i = 20; i < 31; i++) assert(vnodes[i].calls == 0);
    } else if (!strcmp(name, "absent")) {
        setup(0); ramfs_disk_rm = NULL;
        assert(run_flush() == 0 && sync_calls == 0 && get_calls == 0);
    } else if (!strcmp(name, "disabled")) {
        setup(11); mount_state.rm_disk_enabled = false;
        assert(run_flush() == 0 && get_calls == 0);
    } else if (!strcmp(name, "unseeded")) {
        setup(11); mount_state.rm_seed_done = false;
        assert(run_flush() == 0 && get_calls == 0);
    } else if (!strcmp(name, "loading")) {
        setup(11); mount_state.rm_disk_loading = true;
        assert(run_flush() == 0 && get_calls == 0);
    } else if (!strcmp(name, "return_data_error")) {
        setup(11); vnodes[0].error = EIO;
        annibale_var_save_on_return();
        assert(warnings == 1 && sync_calls == 0);
        for (unsigned i = 0; i < 11; i++) assert(vnodes[i].calls == 1);
    } else if (!strcmp(name, "return_metadata_error")) {
        setup(11); metadata_error = ENOSPC;
        annibale_var_save_on_return(); assert(warnings == 1 && sync_calls == 1);
    } else if (!strcmp(name, "return_success")) {
        setup(11); annibale_var_save_on_return(); assert(warnings == 0 && sync_calls == 1);
    } else if (!strcmp(name, "return_no_ufs")) {
        setup(11); ufs_ready = false; annibale_var_save_on_return();
        assert(warnings == 0 && sync_calls == 0 && get_calls == 0);
    } else { assert(!"unknown case"); }
    released(); return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref', help='Read production functions from this Git revision.')
    parser.add_argument('--case', help='Run one scenario.')
    args = parser.parse_args()
    flush = function(source_at(DISK, args.source_ref), 'ramfs_disk_flush_all')
    save = function(source_at(UFS, args.source_ref), 'annibale_var_save_on_return')
    returns_void = flush.startswith('void\n')
    call = 'ramfs_disk_flush_all(); return 0;' if returns_void else 'return ramfs_disk_flush_all();'
    source = PRELUDE + flush + save + '\nstatic int run_flush(void) { ' + call + ' }\n' + CASES
    cases = [args.case] if args.case else [
        'overflow', 'filter', 'stale', 'recycled', 'data_error', 'metadata_error',
        'mutation', 'growth', 'absent', 'disabled', 'unseeded', 'loading',
        'return_data_error', 'return_metadata_error', 'return_success', 'return_no_ufs',
    ]
    with tempfile.TemporaryDirectory(prefix='ramfs-flush-') as tmp:
        c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
        c.write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined',
                        str(c), '-o', str(exe)], check=True)
        for case in cases:
            subprocess.run([str(exe), case], check=True, timeout=10)
            print(f'PASS {case}')


if __name__ == '__main__':
    main()

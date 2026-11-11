#!/usr/bin/env python3
"""Compile production vnode operations with failing mapped-page and metadata I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'xnu/files/bsd/miscfs/ramfs'


def function(text, name):
    start = text.index(f'static int\n{name}(')
    return text[start:text.index('\n}\n', start) + 3]


source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#define PAGE_SIZE 16384
#define PAGE_MASK (PAGE_SIZE - 1)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define Z_WAITOK 0
#define Z_NOFAIL 0
#define IO_APPEND 1
#define UBC_PUSHDIRTY 1
#define UBC_SYNC 2
#define F_FULLFSYNC 51
#define RAMFS_ROOT_INO 2
#define PROTECTION_CLASS_DEFAULT 0
#define _IOW(a,b,c) (2000 + (b))
#define _IOR(a,b,c) (3000 + (b))
#define _IOWR(a,b,c) (4000 + (b))
#define FSIOC_CAS_BSDFLAGS 5000
#define RAMFS_IF_SYNCROOT 1
#define NULLVP NULL
struct fsioc_cas_bsdflags { uint32_t expected_flags, new_flags, actual_flags; };
typedef int64_t user_ssize_t;
enum vtype { VREG, VDIR };
struct ramfs_mount { int rm_lock; bool rm_seed_done; };
struct ramfs_node {
    struct ramfs_mount *rn_mount;
    enum vtype rn_type;
    uint64_t rn_ino, rn_size;
    uint32_t rn_mode, rn_uid, rn_gid, rn_bsdflags, rn_dataprotect_class, rn_iflags;
    struct ramfs_node *rn_parent;
    struct timespec rn_btime, rn_atime, rn_mtime, rn_ctime;
    bool rn_mapped_write;
    char data[32];
};
typedef struct ramfs_node *vnode_t;
#define RAMFS_NODE(vp) (vp)
struct uio { off_t offset; user_ssize_t resid; char *data; bool read; };
struct vnode_attr {
    unsigned active, supported;
    uint64_t va_data_size;
    uint32_t va_mode, va_uid, va_gid, va_flags, va_dataprotect_class;
    struct timespec va_create_time, va_access_time, va_modify_time, va_change_time;
};
enum {
    ATTR_va_data_size = 1, ATTR_va_mode = 2, ATTR_va_uid = 4, ATTR_va_gid = 8,
    ATTR_va_flags = 16, ATTR_va_dataprotect_class = 32, ATTR_va_create_time = 64,
    ATTR_va_access_time = 128, ATTR_va_modify_time = 256, ATTR_va_change_time = 512
};
#define VATTR_IS_ACTIVE(vap, field) ((vap)->active & ATTR_##field)
#define VATTR_SET_SUPPORTED(vap, field) ((vap)->supported |= ATTR_##field)
struct vnop_setattr_args { vnode_t a_vp; struct vnode_attr *a_vap; };
struct vnop_read_args { vnode_t a_vp; struct uio *a_uio; };
struct vnop_write_args { vnode_t a_vp; struct uio *a_uio; int a_ioflag; };
struct vnop_ioctl_args { vnode_t a_vp; unsigned long a_command; void *a_data; };
struct vnop_fsync_args { vnode_t a_vp; };
static int mapped_error, metadata_error;
static unsigned mapped_calls, metadata_calls, changes, allocations;
static off_t ubc_getsize(vnode_t vp) { return (off_t)vp->rn_size; }
static int ubc_msync(vnode_t vp, off_t begin, off_t end, void *resid, int flags) {
    assert(begin == 0 && end == (off_t)vp->rn_size && resid == NULL);
    assert(flags == (UBC_PUSHDIRTY | UBC_SYNC));
    mapped_calls++; return mapped_error;
}
static int ramfs_disk_sync(struct ramfs_mount *rm) {
    assert(rm); metadata_calls++; return metadata_error;
}
static void ramfs_disk_mark_seed_done(struct ramfs_mount *rm) { rm->rm_seed_done = true; changes++; }
#define lck_mtx_lock(p) ((void)(p))
#define lck_mtx_unlock(p) ((void)(p))
static void *page_alloc(void) { void *p = calloc(1, PAGE_SIZE); assert(p); allocations++; return p; }
static void page_free(void *p) { assert(p && allocations); allocations--; free(p); }
#define kalloc_data(n, flags) page_alloc()
#define kfree_data(p, n) page_free(p)
#define ramfs_page_alloc(zero) page_alloc()
#define ramfs_page_free(p) page_free(p)
static off_t uio_offset(struct uio *uio) { return uio->offset; }
static user_ssize_t uio_resid(struct uio *uio) { return uio->resid; }
static void uio_setoffset(struct uio *uio, off_t off) { uio->offset = off; }
static void uio_setresid(struct uio *uio, user_ssize_t resid) { uio->resid = resid; }
static int uiomove(void *data, int count, struct uio *uio) {
    assert(count >= 0 && count <= uio->resid);
    if (uio->read) memcpy(uio->data, data, (size_t)count);
    else memcpy(data, uio->data, (size_t)count);
    uio->offset += count; uio->resid -= count; uio->data += count; return 0;
}
static void ramfs_resize(struct ramfs_node *node, uint64_t size) { node->rn_size = size; changes++; }
static void ramfs_touch(struct ramfs_node *node, bool data) { (void)node; (void)data; changes++; }
static void ramfs_track(struct ramfs_node *node) { (void)node; }
static vnode_t vnode_getparent(vnode_t vp) { (void)vp; return NULLVP; }
static void vnode_put(vnode_t vp) { (void)vp; }
static void ramfs_disk_dirty_locked(struct ramfs_mount *rm) { (void)rm; changes++; }
static int ubc_setsize(vnode_t vp, off_t size) { assert(size == (off_t)vp->rn_size); return 0; }
static bool ubc_pages_resident(vnode_t vp) { (void)vp; return false; }
static void ramfs_refresh_cache(vnode_t vp, struct ramfs_node *n, uint64_t a, uint64_t b) {
    (void)vp; (void)n; (void)a; (void)b; assert(false);
}
static void ramfs_reserve_slots(struct ramfs_mount *rm, struct ramfs_node *n, uint64_t index) {
    (void)rm; (void)n; assert(index == 0);
}
static void ramfs_fetch(struct ramfs_node *n, uint64_t off, void *to, size_t len) {
    assert(off + len <= sizeof(n->data)); memcpy(to, n->data + off, len);
}
static int ramfs_store(struct ramfs_node *n, uint64_t off, const void *data, size_t len,
                       void **spare) {
    (void)spare;
    assert(off + len <= sizeof(n->data)); memcpy(n->data + off, data, len);
    if (off + len > n->rn_size) n->rn_size = off + len;
    changes++; return 0;
}
static int ramfs_store_write_locked(struct ramfs_node *n, uint64_t index) {
    (void)n; assert(index == 0); return 0;
}
'''
source += function((SRC / 'ramfs.c').read_text(), 'ramfs_sync_mapping')
vnops = (SRC / 'ramfs_vnops.inc').read_text()
for name in ('ramfs_setattr', 'ramfs_read', 'ramfs_write'):
    source += function(vnops, name)
source += (SRC / 'ramfs_fsctl.inc').read_text().replace('#include <sys/fsctl.h>\n', '')
source += r'''
static void exercise(unsigned operation, int error, bool mapped) {
    struct ramfs_mount rm = { .rm_seed_done = true };
    struct ramfs_node node = {
        .rn_mount = &rm, .rn_type = VREG, .rn_ino = 3, .rn_size = 4,
        .rn_mapped_write = mapped, .data = "old!"
    };
    struct ramfs_node original = node;
    char data[4] = "new!", original_data[4];
    memcpy(original_data, data, sizeof(data));
    struct uio uio = { .resid = 4, .data = data, .read = operation == 1 };
    struct vnode_attr attr = { .active = ATTR_va_data_size, .va_data_size = 2 };
    struct vnop_setattr_args set = { &node, &attr };
    struct vnop_read_args read = { &node, &uio };
    struct vnop_write_args write = { &node, &uio, 0 };
    struct vnop_fsync_args fsync = { &node };
    struct vnop_ioctl_args ioctl = { &node, operation == 4 ? F_FULLFSYNC : 85 };
    mapped_error = error; metadata_error = 0;
    mapped_calls = metadata_calls = changes = 0;
    int result;
    switch (operation) {
    case 0: result = ramfs_setattr(&set); break;
    case 1: result = ramfs_read(&read); break;
    case 2: result = ramfs_write(&write); break;
    case 3: result = ramfs_fsync(&fsync); break;
    default: result = ramfs_ioctl(&ioctl); break;
    }
    assert(result == (mapped ? error : 0));
    assert(mapped_calls == (mapped ? 1u : 0u) && allocations == 0);
    if (mapped && error) {
        assert(memcmp(&node, &original, sizeof(node)) == 0);
        assert(memcmp(data, original_data, sizeof(data)) == 0);
        assert(uio.offset == 0 && uio.resid == 4 && attr.supported == 0);
        assert(changes == 0 && metadata_calls == 0);
    } else {
        assert(metadata_calls == (operation >= 3 ? 1u : 0u));
        if (operation == 0) assert(node.rn_size == 2 && attr.supported == ATTR_va_data_size);
        if (operation == 1) assert(uio.resid == 0 && memcmp(data, "old!", 4) == 0);
        if (operation == 2) assert(uio.resid == 0 && memcmp(node.data, "new!", 4) == 0);
    }
    if (operation >= 3) {
        mapped_error = 0; metadata_error = ENOSPC;
        assert((operation == 3 ? ramfs_fsync(&fsync) : ramfs_ioctl(&ioctl)) == ENOSPC);
    }
}
int main(void) {
    for (unsigned operation = 0; operation < 6; operation++) {
        exercise(operation, EIO, true);
        exercise(operation, ENOSPC, true);
        exercise(operation, 0, true);
        exercise(operation, EIO, false);
    }
    puts("ramfs mapped-page errors: setattr, read, write, fsync and full/barrier sync passed");
}
'''
with tempfile.TemporaryDirectory(prefix='ramfs-sync-errors-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(source)
    subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined',
                    str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

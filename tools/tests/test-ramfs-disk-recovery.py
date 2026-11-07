#!/usr/bin/env python3
"""Compile the production checkpoint reader and mount code against fault-injected UFS I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'xnu/files/bsd/miscfs/ramfs'
main = (SRC / 'ramfs.c').read_text()
disk = (SRC / 'ramfs_disk.inc').read_text()
structs = main[main.index('struct ramfs_mount;'):main.index('/* file pages in use')]
format_defs = disk[disk.index('#define RDWFS_MAGIC_STR'):disk.index('static void ramfs_disk_dirty_locked')]
find_ino = disk[disk.index('static struct ramfs_node *\nramfs_disk_find_ino'):disk.index('#include "ramfs_disk_restore.inc"')]
pack_sync = disk[disk.index('static int\nramfs_disk_pack_locked'):disk.index('static void\nramfs_disk_meta_thread')]
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <time.h>
#include <zlib.h>
#define PAGE_SIZE 16384
#define PAGE_MASK (PAGE_SIZE - 1)
#define RAMFS_ROOT_INO 2
#define RAMFS_MAX_NAME 255
#define RAMFS_MAX_XATTR (16u << 20)
#define XATTR_MAXNAMELEN 127
#define PROTECTION_CLASS_D 4
#define Z_WAITOK 0
#define Z_ZERO 0
#define PRIBIO 0
#define NSEC_PER_MSEC 1
#define relaxed 0
#define os_atomic_inc(p, order) (++*(p))
#define os_atomic_dec(p, order) (--*(p))
#define Z_NOFAIL 0
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define LCK_ATTR_NULL NULL
#define lck_mtx_init(...) ((void)0)
#define lck_mtx_destroy(...) ((void)0)
#ifndef __APPLE__
typedef uintptr_t user_addr_t;
#endif
typedef void *vfs_context_t;
typedef int lck_mtx_t;
typedef void *mount_t;
typedef void *vnode_t;
typedef void *thread_call_t;
typedef void *thread_call_param_t;
enum vtype { VNON, VREG, VDIR, VBLK, VCHR, VLNK, VSOCK, VFIFO, VBAD };
static size_t allocations, alloc_call, fail_alloc;
static void *allocate(size_t n) {
    if (++alloc_call == fail_alloc) return NULL;
    void *p = calloc(1, n); assert(p); allocations++; return p;
}
static void release(void *p) { if (p) { allocations--; free(p); } }
#define KT2(t, f) ((t *)allocate(sizeof(t)))
#define KT3(t, n, f) ((t *)allocate(sizeof(t) * (n)))
#define PICK(_1,_2,_3,NAME,...) NAME
#define kalloc_type(...) PICK(__VA_ARGS__, KT3, KT2)(__VA_ARGS__)
#define KF2(t,p) release(p)
#define KF3(t,n,p) release(p)
#define kfree_type(...) PICK(__VA_ARGS__, KF3, KF2)(__VA_ARGS__)
#define kalloc_data(n,f) allocate(n)
#define kfree_data(p,n) release(p)
#define ramfs_vm_alloc(n) allocate(n)
#define ramfs_vm_free(p,n) release(p)
#define ramfs_page_alloc(z) allocate(PAGE_SIZE)
#define ramfs_page_free(p) release(p)
#define lck_mtx_lock(p) ((void)0)
#define lck_mtx_unlock(p) ((void)0)
#define msleep(...) ((void)0)
#define wakeup(p) ((void)0)
#define clock_interval_to_deadline(a,b,p) (*(p) = 1)
#define thread_call_enter_delayed(a,b) ((void)0)
#define thread_call_cancel_wait(p) 0
#define thread_call_free(p) (release(p), 0)
static uint64_t ramfs_var_used_pages;
'''
source += structs + format_defs
source += r'''
static uint8_t *device;
static bool device_present = true;
static int64_t io_fail = -1;
static unsigned writes, panic_calls;
static uint64_t last_write;
static bool redwood_ufs_var_info(uint64_t *size) { *size = 1ULL << 30; return device_present; }
static int redwood_ufs_var_io(bool write, uint64_t offset, void *buf, uint64_t len) {
    assert(offset + len <= 64ULL << 20);
    if ((int64_t)offset == io_fail) return EIO;
    if (write) { writes++; last_write = offset; memcpy(device + offset, buf, len); }
    else memcpy(buf, device + offset, len);
    return 0;
}
static struct ramfs_node *ramfs_node_alloc(struct ramfs_mount *rm, enum vtype type, void *unused) {
    (void)unused;
    struct ramfs_node *n = calloc(1, sizeof(*n)); assert(n); allocations++;
    n->rn_mount = rm; n->rn_type = type; n->rn_mode = 0755;
    TAILQ_INIT(&n->rn_entries); TAILQ_INIT(&n->rn_xattrs); return n;
}
static void ramfs_hash_insert(struct ramfs_mount *rm, struct ramfs_node *n) {
    unsigned h = n->rn_ino & (RAMFS_HASH_SIZE - 1);
    n->rn_hnext = rm->rm_hash[h]; rm->rm_hash[h] = n; n->rn_hashed = true;
}
static void ramfs_disk_bmap_set(struct ramfs_mount *rm, uint32_t b) { rm->rm_bmap[b >> 5] |= 1u << (b & 31); }
static struct ramfs_dirent *ramfs_dirent_alloc(const char *s, size_t len) {
    struct ramfs_dirent *d = calloc(1, sizeof(*d)); assert(d); allocations++;
    d->rd_name = calloc(1, len + 1); assert(d->rd_name); allocations++;
    memcpy(d->rd_name, s, len); d->rd_namelen = (uint32_t)len; return d;
}
static void ramfs_dirent_free(struct ramfs_dirent *d) { release(d->rd_name); release(d); }
static void ramfs_node_free(struct ramfs_node *n) {
    struct ramfs_mount *rm = n->rn_mount;
    assert(TAILQ_EMPTY(&n->rn_entries));
    struct ramfs_node **p = &rm->rm_hash[n->rn_ino & (RAMFS_HASH_SIZE - 1)];
    while (*p != n) p = &(*p)->rn_hnext;
    *p = n->rn_hnext;
    struct ramfs_xattr *xa;
    while ((xa = TAILQ_FIRST(&n->rn_xattrs))) {
        TAILQ_REMOVE(&n->rn_xattrs, xa, rx_link);
        release(xa->rx_name); release(xa->rx_data); release(xa);
    }
    for (uint64_t i = 0; i < n->rn_nslots; i++) {
        if (n->rn_blocks && n->rn_blocks[i]) { rm->rm_pages--; ramfs_var_used_pages--; }
        if (n->rn_pages) release(n->rn_pages[i]);
    }
    release(n->rn_pages); release(n->rn_blocks); release(n->rn_target);
    rm->rm_nodes--; release(n);
}
static void ramfs_disk_meta_thread(void *a, void *b) { (void)a; (void)b; }
static void *thread_call_allocate(void (*f)(void *, void *), void *arg) {
    (void)f; (void)arg; return allocate(1);
}
static void ramfs_disk_restore_panic_log(struct ramfs_mount *rm) { (void)rm; panic_calls++; }
'''
source += find_ino + (SRC / 'ramfs_disk_restore.inc').read_text() + pack_sync
source += '#define ramfs_disk_sync(rm) ramfs_disk_sync_impl(rm)\n'
source += (SRC / 'ramfs_disk_mount.inc').read_text()
source += r'''
struct vfsstatfs { char f_mntonname[64]; };
struct test_mount { struct vfsstatfs stat; void *private; };
static uint64_t max_mem = 1ULL << 30;
static bool ufs_boot = true;
#define vfs_isupdate(mp) false
#define vfs_setfsprivate(mp, p) (((struct test_mount *)(mp))->private = (p))
#define vfs_setlocklocal(mp) ((void)0)
#define vfs_setextendedsecurity(mp) ((void)0)
#define vfs_getnewfsid(mp) ((void)0)
#define vfs_setfstypename(mp, name) ((void)0)
#define vfs_statfs(mp) (&((struct test_mount *)(mp))->stat)
#define ramfs_fill_statfs(rm, sp) ((void)(sp))
static bool PE_parse_boot_argn(const char *key, char *out, size_t size) {
    (void)key; if (ufs_boot) snprintf(out, size, "ufs_darwin"); return ufs_boot;
}
'''
source += main[main.index('static int\nramfs_mount('):main.index('/* Free a directory')]
source += r'''
static void setup(struct ramfs_mount *rm) {
    assert(allocations == 0 && ramfs_var_used_pages == 0);
    memset(device, 0, 64ULL << 20); memset(rm, 0, sizeof(*rm));
    device_present = true; io_fail = -1; writes = panic_calls = 0;
    ramfs_disk_claimed = false; ramfs_disk_rm = NULL;
    fail_alloc = alloc_call = 0;
    rm->rm_hash = calloc(RAMFS_HASH_SIZE, sizeof(*rm->rm_hash)); assert(rm->rm_hash);
    ramfs_disk_empty_root(rm);
}
static void cleanup(struct ramfs_mount *rm) {
    if (rm->rm_meta_call) release(rm->rm_meta_call);
    ramfs_disk_clear_nodes(rm); ramfs_disk_release_buffers(rm); free(rm->rm_hash);
    assert(allocations == 0 && ramfs_var_used_pages == 0 && rm->rm_pages == 0);
}
static void checkpoint(unsigned slot, uint64_t gen, bool partial) {
    uint8_t *base = device + slot * RDWFS_SLOT_MAX;
    struct rdwfs_super *s = (void *)base;
    struct rdwfs_inode_rec root = { .ino = 2, .type = VDIR, .nlink = 2, .parent_ino = 2 };
    memcpy(base + RDWFS_SUPER_BYTES, &root, sizeof(root));
    uint64_t bytes = sizeof(root);
    if (partial) {
        struct rdwfs_inode_rec file = { .ino = 3, .type = VREG, .nlink = 1, .size = 1, .nblocks = 1 };
        memcpy(base + RDWFS_SUPER_BYTES + bytes, &file, sizeof(file)); bytes += sizeof(file);
        uint32_t block = 1;
        memcpy(base + RDWFS_SUPER_BYTES + bytes, &block, sizeof(block)); bytes += 8;
        struct rdwfs_inode_rec bad = { .ino = 4, .type = VBAD, .nlink = 1 };
        memcpy(base + RDWFS_SUPER_BYTES + bytes, &bad, sizeof(bad)); bytes += sizeof(bad);
    }
    memcpy(s->magic, RDWFS_MAGIC_STR, 8); s->version = RDWFS_VERSION;
    s->page_size = PAGE_SIZE; s->generation = gen; s->next_ino = 5;
    s->node_count = partial ? 3 : 1; s->payload_bytes = bytes;
    s->payload_crc = (uint32_t)crc32(0, base + RDWFS_SUPER_BYTES, (uInt)bytes);
    s->super_crc = (uint32_t)crc32(0, (void *)s, __builtin_offsetof(struct rdwfs_super, super_crc));
}
int main(void) {
    device = calloc(1, 64ULL << 20); assert(device);
    struct ramfs_mount rm;
    setup(&rm); device_present = false;
    assert(ramfs_disk_mount(&rm) == ENXIO && writes == 0); cleanup(&rm);
    for (size_t fail = 1; fail <= 6; fail++) {
        setup(&rm); fail_alloc = fail;
        assert(ramfs_disk_mount(&rm) == ENOMEM && writes == 0 && !ramfs_disk_claimed);
        cleanup(&rm);
    }
    setup(&rm); io_fail = RDWFS_SLOT1_OFFSET;
    assert(ramfs_disk_mount(&rm) == EIO && writes == 0); cleanup(&rm);
    setup(&rm); device[65536] = 1;
    assert(ramfs_disk_mount(&rm) == EINVAL && writes == 0); cleanup(&rm);
    setup(&rm); device[0] = device[RDWFS_SLOT1_OFFSET] = 1;
    assert(ramfs_disk_mount(&rm) == EINVAL && writes == 0); cleanup(&rm);
    setup(&rm); assert(ramfs_disk_mount(&rm) == 0 && !rm.rm_seed_done && writes == 0); cleanup(&rm);
    setup(&rm); checkpoint(0, 3, false); checkpoint(1, 4, false);
    assert(ramfs_disk_mount(&rm) == 0 && rm.rm_nodes == 1 && rm.rm_meta_gen == 4 && writes == 0);
    cleanup(&rm);
    setup(&rm); checkpoint(0, 5, true); checkpoint(1, 3, false);
    assert(ramfs_disk_mount(&rm) == 0 && rm.rm_nodes == 1 && rm.rm_pages == 0);
    assert(rm.rm_meta_slot == 1 && rm.rm_meta_gen == 3 && ramfs_var_used_pages == 0 && writes == 0);
    rm.rm_meta_dirty = true;
    assert(ramfs_disk_sync_impl(&rm) == 0 && last_write == RDWFS_SLOT0_OFFSET && rm.rm_meta_gen == 4);
    cleanup(&rm);
    setup(&rm); checkpoint(0, 5, true); checkpoint(1, 3, false); fail_alloc = 7;
    assert(ramfs_disk_mount(&rm) == ENOMEM && writes == 0 && ramfs_var_used_pages == 0); cleanup(&rm);
    setup(&rm); checkpoint(1, 3, false);
    assert(ramfs_disk_mount(&rm) == 0 && rm.rm_meta_slot == 1);
    rm.rm_meta_dirty = true; io_fail = RDWFS_SUPER_BYTES;
    assert(ramfs_disk_sync_impl(&rm) == EIO && rm.rm_meta_gen == 3 && rm.rm_meta_slot == 1);
    assert(rm.rm_meta_dirty && writes == 0);
    io_fail = -1;
    assert(ramfs_disk_sync_impl(&rm) == 0 && rm.rm_meta_gen == 4 && last_write == RDWFS_SLOT0_OFFSET);
    cleanup(&rm);
    setup(&rm); checkpoint(0, 5, true); checkpoint(1, 3, true);
    assert(ramfs_disk_mount(&rm) == EINVAL && writes == 0 && ramfs_var_used_pages == 0); cleanup(&rm);
    setup(&rm); checkpoint(0, 1, false); io_fail = RDWFS_SUPER_BYTES;
    assert(ramfs_disk_mount(&rm) == EIO && writes == 0); cleanup(&rm);
    for (unsigned mode = 0; mode < 3; mode++) {
        assert(allocations == 0);
        struct test_mount mp = { 0 };
        strcpy(mp.stat.f_mntonname, mode == 2 ? "/tmp" : "/private/var");
        device_present = false; ufs_boot = mode != 1;
        ramfs_disk_claimed = false; alloc_call = fail_alloc = 0;
        int error = ramfs_mount(&mp, NULL, 0, NULL);
        assert(error == (mode == 0 ? ENXIO : 0));
        if (mode == 0) assert(mp.private == NULL);
        else {
            struct ramfs_mount *mounted = mp.private;
            assert(!mounted->rm_disk_backed && mounted->rm_nodes == 1);
            ramfs_disk_clear_nodes(mounted);
            release(mounted->rm_hash); release(mounted);
        }
        assert(allocations == 0);
    }
    free(device); puts("ramfs recovery: missing device, allocation/I/O faults, corrupt/empty slots, fallback and small checkpoints passed");
}
'''
with tempfile.TemporaryDirectory(prefix='ramfs-recovery-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(source)
    subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined', '-Wno-incompatible-pointer-types',
                    str(c), '-lz', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

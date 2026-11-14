#!/usr/bin/env python3
"""Test production ANNFS recovery, unmount retries, and callout lifetime with UFS faults."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'xnu/files/bsd/miscfs/ramfs'
main = (SRC / 'ramfs.c').read_text()
disk = (SRC / 'ramfs_disk.inc').read_text()
structs = main[main.index('struct ramfs_mount;'):main.index('/* file pages in use')]
format_defs = disk[disk.index('#define ANNFS_MAGIC_STR'):disk.index('static void ramfs_disk_dirty_locked')]
find_ino = disk[disk.index('static struct ramfs_node *\nramfs_disk_find_ino'):disk.index('#include "ramfs_disk_restore.inc"')]
pack_sync = disk[disk.index('static void\nramfs_disk_dirty_locked'):disk.index('int ramfs_disk_flush_all(void);')]
# Count calls without changing the extracted serialization or sync logic.
pack_sync = pack_sync.replace('uint8_t *base = (uint8_t *)rm->rm_meta_buf + ANNFS_SUPER_BYTES;',
                              'pack_calls++;\n\tuint8_t *base = (uint8_t *)rm->rm_meta_buf + ANNFS_SUPER_BYTES;', 1)
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
#include <pthread.h>
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
#define lck_mtx_init(p, ...) assert(pthread_mutex_init(p, NULL) == 0)
#define lck_mtx_destroy(p, ...) assert(pthread_mutex_destroy(p) == 0)
#ifndef __APPLE__
typedef uintptr_t user_addr_t;
#endif
typedef void *vfs_context_t;
typedef pthread_mutex_t lck_mtx_t;
typedef void *mount_t;
typedef void *vnode_t;
typedef struct test_call *thread_call_t;
typedef void *thread_call_param_t;
enum vtype { VNON, VREG, VDIR, VBLK, VCHR, VLNK, VSOCK, VFIFO, VBAD };
static size_t allocations, alloc_call, fail_alloc;
static void (*allocation_hook)(void);
static unsigned pack_calls, sync_waits;
static void *allocate(size_t n) {
    if (allocation_hook != NULL) {
        void (*hook)(void) = allocation_hook;
        allocation_hook = NULL; hook();
    }
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
#define lck_mtx_lock(p) assert(pthread_mutex_lock(p) == 0)
#define lck_mtx_unlock(p) assert(pthread_mutex_unlock(p) == 0)
static pthread_cond_t sync_cv = PTHREAD_COND_INITIALIZER;
static void wait_cond(pthread_cond_t *cv, pthread_mutex_t *lock) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    assert(pthread_cond_timedwait(cv, lock, &deadline) == 0);
}
static void sync_wait(pthread_mutex_t *lock);
#define msleep(p, lock, ...) sync_wait(lock)
#define wakeup(p) assert(pthread_cond_broadcast(&sync_cv) == 0)
#define clock_interval_to_deadline(a,b,p) (*(p) = 1)
#define THREAD_CALL_PRIORITY_HIGH 0
#define THREAD_CALL_OPTIONS_ONCE 1
struct test_call {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    void (*func)(void *, void *);
    void *arg;
    unsigned options;
    bool pending, running;
};
static void *thread_call_allocate_with_options(void (*func)(void *, void *), void *arg,
                                               unsigned priority, unsigned options) {
    (void)priority;
    struct test_call *call = allocate(sizeof(*call));
    if (call) {
        lck_mtx_init(&call->lock);
        assert(pthread_cond_init(&call->cv, NULL) == 0);
        call->func = func; call->arg = arg; call->options = options;
    }
    return call;
}
static void thread_call_enter_delayed(thread_call_t call, uint64_t deadline) {
    (void)deadline;
    lck_mtx_lock(&call->lock); call->pending = true; lck_mtx_unlock(&call->lock);
}
static bool thread_call_free(thread_call_t call) {
    lck_mtx_lock(&call->lock);
    assert(!call->pending && !call->running);
    lck_mtx_unlock(&call->lock);
    lck_mtx_destroy(&call->lock);
    assert(pthread_cond_destroy(&call->cv) == 0);
    release(call); return true;
}
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cv = PTHREAD_COND_INITIALIZER;
static bool pause_callback, callback_entered, release_callback;
static bool pause_io, io_entered, release_io, cancel_entered, unmount_done;
static uint64_t ramfs_var_used_pages;
static void nanotime(struct timespec *now) { assert(clock_gettime(CLOCK_REALTIME, now) == 0); }
'''
source += structs + format_defs
source += r'''
static void sync_wait(pthread_mutex_t *lock) {
    lck_mtx_lock(&event_lock); sync_waits++;
    pthread_cond_broadcast(&event_cv); lck_mtx_unlock(&event_lock);
    wait_cond(&sync_cv, lock);
}
/* Match XNU's cancellation contract; production ONCE selection must wait for both states. */
static bool thread_call_cancel_wait(thread_call_t call) {
    struct ramfs_mount *rm = call->arg;
    assert(pthread_mutex_trylock(&rm->rm_lock) == 0);
    assert(rm->rm_meta_call == NULL);
    lck_mtx_unlock(&rm->rm_lock);
    lck_mtx_lock(&call->lock);
    bool canceled = call->pending;
    call->pending = false;
    lck_mtx_lock(&event_lock); cancel_entered = true;
    pthread_cond_broadcast(&event_cv); lck_mtx_unlock(&event_lock);
    while (call->running && (!canceled || (call->options & THREAD_CALL_OPTIONS_ONCE))) {
        wait_cond(&call->cv, &call->lock);
    }
    lck_mtx_unlock(&call->lock);
    return canceled;
}
static void *run_call(void *arg) {
    thread_call_t call = arg;
    lck_mtx_lock(&call->lock);
    assert(call->pending && !call->running);
    call->pending = false; call->running = true;
    lck_mtx_unlock(&call->lock);
    lck_mtx_lock(&event_lock); callback_entered = true;
    pthread_cond_broadcast(&event_cv);
    while (pause_callback && !release_callback) wait_cond(&event_cv, &event_lock);
    lck_mtx_unlock(&event_lock);
    call->func(call->arg, NULL);
    lck_mtx_lock(&call->lock); call->running = false;
    pthread_cond_broadcast(&call->cv); lck_mtx_unlock(&call->lock);
    return NULL;
}
static uint8_t *device;
static bool device_present = true;
static int64_t io_fail = -1;
static uint64_t partial_write;
static unsigned writes, panic_calls;
static uint64_t last_write;
static bool annibale_ufs_var_info(uint64_t *size) { *size = 1ULL << 30; return device_present; }
static int annibale_ufs_var_io(bool write, uint64_t offset, void *buf, uint64_t len) {
    assert(offset + len <= 64ULL << 20);
    if (write) {
        lck_mtx_lock(&event_lock);
        if (pause_io) {
            io_entered = true; pthread_cond_broadcast(&event_cv);
            while (!release_io) wait_cond(&event_cv, &event_lock);
            pause_io = false;
        }
        lck_mtx_unlock(&event_lock);
    }
    if ((int64_t)offset == io_fail) {
        if (write && partial_write != 0) {
            assert(partial_write < len);
            memcpy(device + offset, buf, partial_write); writes++;
        }
        return EIO;
    }
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
static void ramfs_disk_restore_panic_log(struct ramfs_mount *rm) { (void)rm; panic_calls++; }
'''
source += '#define ramfs_disk_sync(rm) ramfs_disk_sync_impl(rm)\n'
source += find_ino + (SRC / 'ramfs_disk_restore.inc').read_text() + pack_sync
touch_at = main.index('static void\nramfs_touch(')
source += main[touch_at:main.index('\n}\n', touch_at) + 3]
source += (SRC / 'ramfs_disk_mount.inc').read_text()
source += r'''
struct vfsstatfs { char f_mntonname[64]; };
struct test_mount { struct vfsstatfs stat; void *private; };
static uint64_t max_mem = 1ULL << 30;
static bool ufs_boot = true;
#define vfs_isupdate(mp) false
#define vfs_setfsprivate(mp, p) (((struct test_mount *)(mp))->private = (p))
#define vfs_fsprivate(mp) (((struct test_mount *)(mp))->private)
#define RAMFS_MP(mp) ((struct ramfs_mount *)vfs_fsprivate(mp))
#define NULLVP NULL
#define MNT_FORCE 1
#define FORCECLOSE 1
#define vfs_setlocklocal(mp) ((void)0)
#define vfs_setextendedsecurity(mp) ((void)0)
#define vfs_getnewfsid(mp) ((void)0)
#define vfs_setfstypename(mp, name) ((void)0)
#define vfs_statfs(mp) (&((struct test_mount *)(mp))->stat)
#define ramfs_fill_statfs(rm, sp) ((void)(sp))
static bool PE_parse_boot_argn(const char *key, char *out, size_t size) {
    (void)key; if (ufs_boot) snprintf(out, size, "ufs_darwin"); return ufs_boot;
}
static int vflush_error;
static int vflush(mount_t mp, vnode_t skip, int flags) {
    (void)mp; (void)skip; (void)flags; return vflush_error;
}
'''
source += main[main.index('static int\nramfs_mount('):main.index('static int\nramfs_root(')]
source += r'''
static void setup(struct ramfs_mount *rm) {
    assert(allocations == 0 && ramfs_var_used_pages == 0);
    memset(device, 0, 64ULL << 20); memset(rm, 0, sizeof(*rm));
    lck_mtx_init(&rm->rm_lock); lck_mtx_init(&rm->rm_rename_lock);
    device_present = true; io_fail = -1; partial_write = 0; writes = panic_calls = 0;
    ramfs_disk_claimed = false; ramfs_disk_rm = NULL;
    fail_alloc = alloc_call = 0;
    rm->rm_hash = calloc(RAMFS_HASH_SIZE, sizeof(*rm->rm_hash)); assert(rm->rm_hash);
    ramfs_disk_empty_root(rm);
}
static void cleanup(struct ramfs_mount *rm) {
    if (rm->rm_meta_call) thread_call_free(rm->rm_meta_call);
    ramfs_disk_clear_nodes(rm); ramfs_disk_release_buffers(rm); free(rm->rm_hash);
    lck_mtx_destroy(&rm->rm_lock); lck_mtx_destroy(&rm->rm_rename_lock);
    assert(allocations == 0 && ramfs_var_used_pages == 0 && rm->rm_pages == 0);
}
static void checkpoint(unsigned slot, uint64_t gen, bool partial) {
    uint8_t *base = device + slot * ANNFS_SLOT_MAX;
    struct annfs_super *s = (void *)base;
    struct annfs_inode_rec root = { .ino = 2, .type = VDIR, .nlink = 2, .parent_ino = 2 };
    memcpy(base + ANNFS_SUPER_BYTES, &root, sizeof(root));
    uint64_t bytes = sizeof(root);
    if (partial) {
        struct annfs_inode_rec file = { .ino = 3, .type = VREG, .nlink = 1, .size = 1, .nblocks = 1 };
        memcpy(base + ANNFS_SUPER_BYTES + bytes, &file, sizeof(file)); bytes += sizeof(file);
        uint32_t block = 1;
        memcpy(base + ANNFS_SUPER_BYTES + bytes, &block, sizeof(block)); bytes += 8;
        struct annfs_inode_rec bad = { .ino = 4, .type = VBAD, .nlink = 1 };
        memcpy(base + ANNFS_SUPER_BYTES + bytes, &bad, sizeof(bad)); bytes += sizeof(bad);
    }
    memcpy(s->magic, ANNFS_MAGIC_STR, 8); s->version = ANNFS_VERSION;
    s->page_size = PAGE_SIZE; s->generation = gen; s->next_ino = 5;
    s->node_count = partial ? 3 : 1; s->payload_bytes = bytes;
    s->payload_crc = (uint32_t)crc32(0, base + ANNFS_SUPER_BYTES, (uInt)bytes);
    s->super_crc = (uint32_t)crc32(0, (void *)s, __builtin_offsetof(struct annfs_super, super_crc));
}
static struct ramfs_mount *mounted_checkpoint(struct test_mount *mp) {
    assert(allocations == 0 && ramfs_var_used_pages == 0);
    memset(device, 0, 64ULL << 20); memset(mp, 0, sizeof(*mp));
    checkpoint(0, 3, false); checkpoint(1, 4, false);
    ramfs_disk_claimed = false; ramfs_disk_rm = NULL;
    device_present = ufs_boot = true; io_fail = -1; partial_write = 0;
    fail_alloc = alloc_call = 0; writes = 0; vflush_error = 0;
    pause_callback = callback_entered = release_callback = false;
    pause_io = io_entered = release_io = cancel_entered = unmount_done = false;
    strcpy(mp->stat.f_mntonname, "/private/var");
    assert(ramfs_mount(mp, NULL, 0, NULL) == 0);
    return mp->private;
}
static void dirty_mode(struct ramfs_mount *rm, uint32_t mode) {
    lck_mtx_lock(&rm->rm_lock);
    rm->rm_root->rn_mode = mode; ramfs_disk_dirty_locked(rm);
    lck_mtx_unlock(&rm->rm_lock);
}
static void assert_released(struct test_mount *mp) {
    assert(mp->private == NULL && ramfs_disk_rm == NULL && !ramfs_disk_claimed);
    assert(allocations == 0 && ramfs_var_used_pages == 0);
}
static void unmount_retry_tests(void) {
    for (unsigned fault = 0; fault < 2; fault++) {
        struct test_mount mp;
        struct ramfs_mount *rm = mounted_checkpoint(&mp);
        thread_call_t call = rm->rm_meta_call;
        void *meta = rm->rm_meta_buf, *io_page = rm->rm_io_page, *bmap = rm->rm_bmap;
        struct ramfs_node *root = rm->rm_root;
        uint8_t committed[8192];
        memcpy(committed, device + ANNFS_SLOT1_OFFSET, sizeof(committed));
        dirty_mode(rm, 0700);
        assert(call->pending);
        vflush_error = EBUSY;
        assert(ramfs_unmount(&mp, 0, NULL) == EBUSY && writes == 0 && call->pending);
        vflush_error = 0;
        io_fail = fault == 0 ? ANNFS_SUPER_BYTES : ANNFS_SLOT0_OFFSET;
        partial_write = fault == 0 ? 128 : 0;
        assert(ramfs_unmount(&mp, 0, NULL) == EIO);
        assert(mp.private == rm && ramfs_disk_rm == rm && ramfs_disk_claimed);
        assert(rm->rm_disk_backed && rm->rm_meta_state == RAMFS_META_DIRTY && rm->rm_meta_gen == 4);
        assert(rm->rm_meta_call == call && rm->rm_meta_buf == meta && rm->rm_io_page == io_page);
        assert(rm->rm_bmap == bmap && rm->rm_root == root && !call->pending && !call->running);
        assert(memcmp(committed, device + ANNFS_SLOT1_OFFSET, sizeof(committed)) == 0);
        dirty_mode(rm, 0600);
        assert(call->pending);
        io_fail = -1; partial_write = 0;
        if (fault == 1) {
            pthread_t worker;
            assert(pthread_create(&worker, NULL, run_call, call) == 0);
            assert(pthread_join(worker, NULL) == 0);
            assert(rm->rm_meta_state == RAMFS_META_CLEAN && rm->rm_meta_gen == 5);
        }
        assert(ramfs_unmount(&mp, 0, NULL) == 0);
        assert_released(&mp);
        assert(((struct annfs_super *)device)->generation == 5);
        assert(((struct annfs_inode_rec *)(device + ANNFS_SUPER_BYTES))->mode == 0600);
    }
}
static int unmount_result;
static void *run_unmount(void *arg) {
    int error = ramfs_unmount(arg, 0, NULL);
    lck_mtx_lock(&event_lock);
    unmount_result = error; unmount_done = true;
    pthread_cond_broadcast(&event_cv); lck_mtx_unlock(&event_lock);
    return NULL;
}
static void wait_event(bool *flag) {
    lck_mtx_lock(&event_lock);
    while (!*flag) wait_cond(&event_cv, &event_lock);
    lck_mtx_unlock(&event_lock);
}
static void assert_unmount_waits(void) {
    wait_event(&cancel_entered);
    lck_mtx_lock(&event_lock);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_nsec += 100000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    int error = 0;
    while (!unmount_done && error == 0) {
        error = pthread_cond_timedwait(&event_cv, &event_lock, &deadline);
    }
    assert(error == ETIMEDOUT && !unmount_done);
    lck_mtx_unlock(&event_lock);
}
static void callout_tests(void) {
    for (unsigned stage = 0; stage < 3; stage++) {
        struct test_mount mp;
        struct ramfs_mount *rm = mounted_checkpoint(&mp);
        thread_call_t call = rm->rm_meta_call;
        pthread_t worker, unmount_worker;
        dirty_mode(rm, 0700);
        pause_callback = stage == 0; pause_io = stage != 0;
        assert(pthread_create(&worker, NULL, run_call, call) == 0);
        wait_event(stage == 0 ? &callback_entered : &io_entered);
        dirty_mode(rm, 0600);
        if (stage < 2) {
            assert(pthread_create(&unmount_worker, NULL, run_unmount, &mp) == 0);
            assert_unmount_waits();
        }
        lck_mtx_lock(&event_lock); release_callback = release_io = true;
        pthread_cond_broadcast(&event_cv); lck_mtx_unlock(&event_lock);
        assert(pthread_join(worker, NULL) == 0);
        if (stage < 2) {
            assert(pthread_join(unmount_worker, NULL) == 0 && unmount_result == 0);
        } else {
            /* A checkpoint changed during I/O must still schedule its next commit. */
            assert(call->pending && rm->rm_meta_state == RAMFS_META_DIRTY && rm->rm_meta_gen == 5);
            assert(pthread_create(&worker, NULL, run_call, call) == 0);
            assert(pthread_join(worker, NULL) == 0);
            assert(!call->pending && rm->rm_meta_state == RAMFS_META_CLEAN && rm->rm_meta_gen == 6);
            assert(ramfs_unmount(&mp, 0, NULL) == 0);
        }
        assert_released(&mp);
        unsigned slot = stage == 0 ? 0 : 1;
        struct annfs_inode_rec *root = (void *)(device + slot * ANNFS_SLOT_MAX + ANNFS_SUPER_BYTES);
        assert(root->mode == 0600);
    }
}
static struct ramfs_xattr *add_large_attribute(struct ramfs_mount *rm, const char *name) {
    struct ramfs_xattr *xa = allocate(sizeof(*xa));
    xa->rx_name = allocate(5); memcpy(xa->rx_name, name, 4); xa->rx_namelen = 4;
    xa->rx_data = allocate(RAMFS_MAX_XATTR); xa->rx_size = RAMFS_MAX_XATTR;
    TAILQ_INSERT_TAIL(&rm->rm_root->rn_xattrs, xa, rx_link);
    ramfs_touch(rm->rm_root, false); return xa;
}
static void remove_large_attribute(struct ramfs_mount *rm, struct ramfs_xattr *xa) {
    TAILQ_REMOVE(&rm->rm_root->rn_xattrs, xa, rx_link);
    release(xa->rx_name); release(xa->rx_data); release(xa);
    ramfs_touch(rm->rm_root, false);
}
static void *repeat_full_sync(void *arg) {
    for (unsigned i = 0; i < 25; i++) assert(ramfs_disk_sync_impl(arg) == ENOSPC);
    return NULL;
}
static void slot_full_tests(void) {
    struct test_mount mp; struct ramfs_mount *rm = mounted_checkpoint(&mp);
    thread_call_t call = rm->rm_meta_call;
    uint8_t committed[8192];
    memcpy(committed, device + ANNFS_SLOT1_OFFSET, sizeof(committed));
    lck_mtx_lock(&rm->rm_lock);
    struct ramfs_xattr *a = add_large_attribute(rm, "bigA");
    struct ramfs_xattr *b = add_large_attribute(rm, "bigB");
    lck_mtx_unlock(&rm->rm_lock);
    pack_calls = 0;
    assert(ramfs_disk_sync_impl(rm) == ENOSPC);
    unsigned first = pack_calls;
    assert(rm->rm_meta_state == RAMFS_META_SLOT_FULL && rm->rm_meta_gen == 4);
    pthread_t workers[8];
    for (unsigned i = 0; i < 8; i++) assert(pthread_create(&workers[i], NULL, repeat_full_sync, rm) == 0);
    for (unsigned i = 0; i < 8; i++) assert(pthread_join(workers[i], NULL) == 0);
    assert(pack_calls == first && writes == 0 && rm->rm_meta_slot == 1);
    assert(memcmp(committed, device + ANNFS_SLOT1_OFFSET, sizeof(committed)) == 0);
    assert(ramfs_unmount(&mp, 0, NULL) == ENOSPC && mp.private == rm);
    assert(rm->rm_meta_call == call && rm->rm_disk_backed && pack_calls == first);
    lck_mtx_lock(&rm->rm_lock);
    rm->rm_root->rn_mode = 0600; ramfs_touch(rm->rm_root, false);
    lck_mtx_unlock(&rm->rm_lock);
    assert(rm->rm_meta_state == RAMFS_META_DIRTY);
    assert(ramfs_disk_sync_impl(rm) == ENOSPC && pack_calls == first + 1 && writes == 0);
    lck_mtx_lock(&rm->rm_lock); remove_large_attribute(rm, b); lck_mtx_unlock(&rm->rm_lock);
    assert(ramfs_disk_sync_impl(rm) == 0 && pack_calls == first + 2 && writes == 2);
    assert(rm->rm_meta_gen == 5 && rm->rm_meta_state == RAMFS_META_CLEAN);
    assert(ramfs_disk_sync_impl(rm) == 0 && pack_calls == first + 2);
    lck_mtx_lock(&rm->rm_lock); remove_large_attribute(rm, a); lck_mtx_unlock(&rm->rm_lock);
    assert(ramfs_unmount(&mp, 0, NULL) == 0); assert_released(&mp);
    puts("slot-full: 200 unchanged sync requests preserve ENOSPC without repacking; mutation retries passed");
}
static void initial_slot_full_tests(void) {
    struct ramfs_mount rm; setup(&rm); assert(ramfs_disk_mount(&rm) == 0);
    rm.rm_seed_done = true; ramfs_var_seed_done = 1;
    lck_mtx_lock(&rm.rm_lock);
    struct ramfs_xattr *a = add_large_attribute(&rm, "bigA");
    struct ramfs_xattr *b = add_large_attribute(&rm, "bigB");
    lck_mtx_unlock(&rm.rm_lock);
    assert(ramfs_disk_sync_impl(&rm) == ENOSPC);
    unsigned first = pack_calls;
    assert(ramfs_disk_sync_impl(&rm) == ENOSPC && pack_calls == first);
    assert(rm.rm_meta_gen == 0 && writes == 0 && rm.rm_meta_state == RAMFS_META_SLOT_FULL);
    rm.rm_disk_loading = true;
    assert(ramfs_disk_sync_impl(&rm) == 0 && pack_calls == first);
    rm.rm_disk_loading = false; rm.rm_seed_done = false;
    assert(ramfs_disk_sync_impl(&rm) == 0 && pack_calls == first);
    rm.rm_seed_done = true; ramfs_var_seed_done = 0;
    assert(ramfs_disk_sync_impl(&rm) == 0 && pack_calls == first);
    ramfs_var_seed_done = 1;
    lck_mtx_lock(&rm.rm_lock); remove_large_attribute(&rm, b); lck_mtx_unlock(&rm.rm_lock);
    assert(ramfs_disk_sync_impl(&rm) == 0 && rm.rm_meta_gen == 1);
    lck_mtx_lock(&rm.rm_lock); remove_large_attribute(&rm, a); lck_mtx_unlock(&rm.rm_lock);
    assert(ramfs_disk_unmount(&rm) == 0); cleanup(&rm);
}
static struct ramfs_mount *sync_worker_rm;
static int sync_worker_results[2];
static void *sync_once(void *arg) {
    sync_worker_results[(uintptr_t)arg] = ramfs_disk_sync_impl(sync_worker_rm); return NULL;
}
static void initial_concurrent_sync_test(void) {
    struct ramfs_mount rm; setup(&rm); assert(ramfs_disk_mount(&rm) == 0);
    rm.rm_seed_done = true; ramfs_var_seed_done = 1;
    pack_calls = sync_waits = 0; pause_io = true; io_entered = release_io = false;
    sync_worker_rm = &rm;
    pthread_t a, b;
    assert(pthread_create(&a, NULL, sync_once, (void *)0) == 0);
    wait_event(&io_entered);
    assert(pthread_create(&b, NULL, sync_once, (void *)1) == 0);
    lck_mtx_lock(&event_lock);
    while (!sync_waits) wait_cond(&event_cv, &event_lock);
    release_io = true; pthread_cond_broadcast(&event_cv); lck_mtx_unlock(&event_lock);
    assert(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    assert(sync_worker_results[0] == 0 && sync_worker_results[1] == 0);
    assert(pack_calls == 1 && rm.rm_meta_gen == 1 && rm.rm_meta_state == RAMFS_META_CLEAN);
    assert(ramfs_disk_unmount(&rm) == 0); cleanup(&rm);
}
static struct ramfs_mount *allocation_rm;
static void change_during_allocation(void) { dirty_mode(allocation_rm, 0640); }
static void allocation_retry_tests(void) {
    struct test_mount mp; struct ramfs_mount *rm = mounted_checkpoint(&mp);
    lck_mtx_lock(&rm->rm_lock);
    struct ramfs_xattr *a = add_large_attribute(rm, "bigA");
    lck_mtx_unlock(&rm->rm_lock);
    size_t original_cap = rm->rm_meta_cap;
    fail_alloc = alloc_call + 1;
    assert(ramfs_disk_sync_impl(rm) == ENOMEM && writes == 0);
    assert(rm->rm_meta_state == RAMFS_META_DIRTY && rm->rm_meta_cap == original_cap);
    assert(!rm->rm_meta_syncing && rm->rm_meta_gen == 4);
    fail_alloc = 0; allocation_rm = rm; allocation_hook = change_during_allocation;
    assert(ramfs_disk_sync_impl(rm) == 0 && allocation_hook == NULL);
    assert(rm->rm_meta_state == RAMFS_META_CLEAN && rm->rm_meta_gen == 5);
    struct annfs_inode_rec *root = (void *)(device + ANNFS_SUPER_BYTES);
    assert(root->mode == 0640);
    lck_mtx_lock(&rm->rm_lock); remove_large_attribute(rm, a); lck_mtx_unlock(&rm->rm_lock);
    assert(ramfs_unmount(&mp, 0, NULL) == 0); assert_released(&mp);
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
    setup(&rm); io_fail = ANNFS_SLOT1_OFFSET;
    assert(ramfs_disk_mount(&rm) == EIO && writes == 0); cleanup(&rm);
    setup(&rm); device[65536] = 1;
    assert(ramfs_disk_mount(&rm) == EINVAL && writes == 0); cleanup(&rm);
    setup(&rm); device[0] = device[ANNFS_SLOT1_OFFSET] = 1;
    assert(ramfs_disk_mount(&rm) == EINVAL && writes == 0); cleanup(&rm);
    setup(&rm); assert(ramfs_disk_mount(&rm) == 0 && !rm.rm_seed_done && writes == 0); cleanup(&rm);
    setup(&rm); checkpoint(0, 3, false); checkpoint(1, 4, false);
    assert(ramfs_disk_mount(&rm) == 0 && rm.rm_nodes == 1 && rm.rm_meta_gen == 4 && writes == 0);
    cleanup(&rm);
    setup(&rm); checkpoint(0, 5, true); checkpoint(1, 3, false);
    assert(ramfs_disk_mount(&rm) == 0 && rm.rm_nodes == 1 && rm.rm_pages == 0);
    assert(rm.rm_meta_slot == 1 && rm.rm_meta_gen == 3 && ramfs_var_used_pages == 0 && writes == 0);
    rm.rm_meta_state = RAMFS_META_DIRTY;
    assert(ramfs_disk_sync_impl(&rm) == 0 && last_write == ANNFS_SLOT0_OFFSET && rm.rm_meta_gen == 4);
    cleanup(&rm);
    /* A failed target-slot write must preserve the last committed checkpoint. */
    for (unsigned stop = 0; stop < 2; stop++) {
        setup(&rm); checkpoint(0, 3, false); checkpoint(1, 4, false);
        assert(ramfs_disk_mount(&rm) == 0 && rm.rm_meta_slot == 1);
        uint8_t committed[8192];
        memcpy(committed, device + ANNFS_SLOT1_OFFSET, sizeof(committed));
        rm.rm_root->rn_mode = 0700; rm.rm_meta_state = RAMFS_META_DIRTY;
        io_fail = stop == 0 ? ANNFS_SUPER_BYTES : ANNFS_SLOT0_OFFSET;
        partial_write = stop == 0 ? 128 : 0;
        assert(ramfs_disk_sync_impl(&rm) == EIO && rm.rm_meta_state == RAMFS_META_DIRTY);
        assert(rm.rm_meta_gen == 4 && rm.rm_meta_slot == 1);
        assert(memcmp(committed, device + ANNFS_SLOT1_OFFSET, sizeof(committed)) == 0);
        io_fail = -1; partial_write = 0;
        struct annfs_super *old_header = (void *)device;
        assert(old_header->generation == 3 && ramfs_disk_super_valid(old_header));
        assert(ramfs_disk_read_slot(&rm, ANNFS_SLOT0_OFFSET, old_header) == EINVAL);
        cleanup(&rm);

        memset(&rm, 0, sizeof(rm));
        lck_mtx_init(&rm.rm_lock); lck_mtx_init(&rm.rm_rename_lock);
        rm.rm_hash = calloc(RAMFS_HASH_SIZE, sizeof(*rm.rm_hash)); assert(rm.rm_hash);
        ramfs_disk_empty_root(&rm); ramfs_disk_claimed = false; ramfs_disk_rm = NULL;
        io_fail = -1; partial_write = 0;
        assert(ramfs_disk_mount(&rm) == 0 && rm.rm_meta_gen == 4 && rm.rm_meta_slot == 1);
        assert(rm.rm_root->rn_mode == 0);
        rm.rm_root->rn_mode = 0600; rm.rm_meta_state = RAMFS_META_DIRTY;
        assert(ramfs_disk_sync_impl(&rm) == 0 && rm.rm_meta_gen == 5 && rm.rm_meta_slot == 0);
        assert(ramfs_disk_read_slot(&rm, ANNFS_SLOT0_OFFSET, (void *)device) == 0);
        cleanup(&rm);
    }
    setup(&rm); checkpoint(0, 5, true); checkpoint(1, 3, false); fail_alloc = 7;
    assert(ramfs_disk_mount(&rm) == ENOMEM && writes == 0 && ramfs_var_used_pages == 0); cleanup(&rm);
    setup(&rm); checkpoint(1, 3, false);
    assert(ramfs_disk_mount(&rm) == 0 && rm.rm_meta_slot == 1);
    rm.rm_meta_state = RAMFS_META_DIRTY; io_fail = ANNFS_SUPER_BYTES;
    assert(ramfs_disk_sync_impl(&rm) == EIO && rm.rm_meta_gen == 3 && rm.rm_meta_slot == 1);
    assert(rm.rm_meta_state == RAMFS_META_DIRTY && writes == 0);
    io_fail = -1;
    assert(ramfs_disk_sync_impl(&rm) == 0 && rm.rm_meta_gen == 4 && last_write == ANNFS_SLOT0_OFFSET);
    cleanup(&rm);
    setup(&rm); checkpoint(0, 5, true); checkpoint(1, 3, true);
    assert(ramfs_disk_mount(&rm) == EINVAL && writes == 0 && ramfs_var_used_pages == 0); cleanup(&rm);
    setup(&rm); checkpoint(0, 1, false); io_fail = ANNFS_SUPER_BYTES;
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
            assert(ramfs_unmount(&mp, 0, NULL) == 0 && mp.private == NULL);
        }
        assert(allocations == 0);
    }
    unmount_retry_tests(); callout_tests();
    slot_full_tests(); initial_slot_full_tests(); initial_concurrent_sync_test(); allocation_retry_tests();
    free(device); puts("ramfs recovery: recovery faults, unmount retries and callout lifetime passed");
}
'''
with tempfile.TemporaryDirectory(prefix='ramfs-recovery-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(source)
    subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined', '-Wno-incompatible-pointer-types',
                    str(c), '-lz', '-pthread', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

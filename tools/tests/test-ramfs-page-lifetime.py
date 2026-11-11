#!/usr/bin/env python3
"""Compile production RAMFS and UFS writes against concurrent eviction/truncation."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CASES = (
    'write-evict', 'write-truncate', 'pageout-evict', 'pageout-truncate',
    'partial-truncate', 'partial-reuse', 'pageout-allocate-truncate',
    'partial-preserve', 'write-read-error', 'write-write-error',
    'pageout-write-error', 'write-memory', 'pageout-memory',
)


def function(text, name, kind='static int'):
    start = text.index(f'{kind}\n{name}(')
    return text[start:text.index('\n}\n', start) + 3]


def harness(root):
    src = root / 'xnu/files/bsd/miscfs/ramfs'
    main = (src / 'ramfs.c').read_text()
    disk = (src / 'ramfs_disk.inc').read_text()
    vnops = (src / 'ramfs_vnops.inc').read_text()
    ufshc = (root / 'xnu/files/bsd/dev/annibale/annibale_ufshc_rw.inc').read_text()
    source = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#define PAGE_SIZE 16384
#define PAGE_MASK (PAGE_SIZE - 1)
#define BLOCK 4096
#define SLOTS 2
#define ANNFS_DATA_OFFSET (512ULL << 20)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define Z_WAITOK 0
#define Z_NOFAIL 0
#define IO_APPEND 1
#define UPL_NOCOMMIT 1
#define UPL_ABORT_FREE_ON_EMPTY 2
#define UPL_COMMIT_CLEAR_DIRTY 4
#define UPL_COMMIT_FREE_ON_EMPTY 8
#define relaxed memory_order_relaxed
#define os_atomic_inc(p,o) atomic_fetch_add_explicit(p, 1, o)
#define os_atomic_dec(p,o) atomic_fetch_sub_explicit(p, 1, o)
#define os_atomic_add(p,n,o) atomic_fetch_add_explicit(p, n, o)
#define os_atomic_load(p,o) atomic_load_explicit(p, o)
typedef pthread_mutex_t lck_mtx_t;
typedef uintptr_t vm_offset_t;
typedef int64_t user_ssize_t;
typedef size_t upl_size_t;
enum vtype { VREG, VDIR };
struct ramfs_mount {
    lck_mtx_t rm_lock;
    bool rm_disk_backed, rm_meta_dirty;
    uint64_t rm_pages, rm_max_pages;
    uint32_t rm_bmap_max;
};
struct ramfs_node {
    struct ramfs_mount *rn_mount;
    enum vtype rn_type;
    uint64_t rn_size, rn_nslots, rn_nalloc;
    void **rn_pages;
    uint32_t *rn_blocks;
};
typedef struct ramfs_node *vnode_t;
#define RAMFS_NODE(vp) (vp)
struct uio { off_t offset; user_ssize_t resid; const uint8_t *data; };
struct test_upl { uint8_t *data; unsigned commits, aborts; };
typedef struct test_upl *upl_t;
struct vnop_write_args { vnode_t a_vp; struct uio *a_uio; int a_ioflag; };
struct vnop_pageout_args {
    vnode_t a_vp; upl_t a_pl; size_t a_size, a_pl_offset; off_t a_f_offset; int a_flags;
};
static _Atomic uint64_t ramfs_pages_in_use, ramfs_var_used_pages;
static _Atomic unsigned mapped_pages;
static struct ramfs_mount mount_data;
static struct ramfs_node node;
static void *pages[1], *race_page;
static uint32_t blocks[1];
static bool block_used[3];
static uint8_t device[3 * PAGE_SIZE], input[PAGE_SIZE], written[PAGE_SIZE];
static bool written_valid;
static int read_error, write_error;
static struct uio last_uio;
static struct test_upl last_upl;
enum phase { NONE, READ_IO, WRITE_IO, ALLOC_PAGE };
enum action { EVICT, TRUNCATE, REUSE };
static enum phase pause_at;
static enum action release_action;
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cv = PTHREAD_COND_INITIALIZER;
static bool requested, attempted, blocked, finished;
static void race(enum phase phase);
static void lock(lck_mtx_t *p) { assert(pthread_mutex_lock(p) == 0); }
static void unlock(lck_mtx_t *p) { assert(pthread_mutex_unlock(p) == 0); }
#define lck_mtx_lock(p) lock(p)
#define lck_mtx_unlock(p) unlock(p)
static void *ramfs_page_alloc(bool wait) {
    (void)wait;
    race(ALLOC_PAGE);
    void *p = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(p != MAP_FAILED); mapped_pages++; return p;
}
static void ramfs_page_free(void *p) {
    assert(p && mapped_pages);
    assert(munmap(p, PAGE_SIZE) == 0); mapped_pages--;
}
static void *kalloc_data(size_t n, int flags) {
    (void)flags; void *p = calloc(1, n); assert(p); return p;
}
static void kfree_data(void *p, size_t n) { (void)n; free(p); }
static uint32_t ramfs_disk_bmap_alloc(struct ramfs_mount *rm) {
    for (uint32_t b = 1; b < rm->rm_bmap_max; b++) {
        if (!block_used[b]) { block_used[b] = true; return b; }
    }
    return 0;
}
static void ramfs_disk_bmap_free(struct ramfs_mount *rm, uint32_t b) {
    assert(b && b < rm->rm_bmap_max && block_used[b]); block_used[b] = false;
}
static int ramfs_grow_slots(struct ramfs_node *n, uint64_t i) {
    assert(n == &node && i == 0 && n->rn_nslots == 1); return 0;
}
static void ramfs_reserve_slots(struct ramfs_mount *rm, struct ramfs_node *n, uint64_t i) {
    assert(rm == &mount_data && n == &node && i == 0);
}
static void ramfs_disk_dirty_locked(struct ramfs_mount *rm) { rm->rm_meta_dirty = true; }
static off_t uio_offset(struct uio *u) { return u->offset; }
static user_ssize_t uio_resid(struct uio *u) { return u->resid; }
static void uio_setoffset(struct uio *u, off_t o) { u->offset = o; }
static void uio_setresid(struct uio *u, user_ssize_t n) { u->resid = n; }
static int uiomove(void *to, int n, struct uio *u) {
    assert(n >= 0 && n <= u->resid);
    memcpy(to, u->data, (size_t)n); u->offset += n; u->resid -= n; u->data += n; return 0;
}
static int ramfs_sync_mapping(vnode_t vp, struct ramfs_node *n) { assert(vp == n); return 0; }
static void ramfs_touch(struct ramfs_node *n, bool data) { (void)n; (void)data; }
static int ubc_setsize(vnode_t vp, off_t n) { (void)vp; (void)n; return 0; }
static bool ubc_pages_resident(vnode_t vp) { (void)vp; return false; }
static void ramfs_refresh_cache(vnode_t vp, struct ramfs_node *n, uint64_t a, uint64_t b) {
    (void)vp; (void)n; (void)a; (void)b; assert(false);
}
static int ubc_upl_map(upl_t upl, vm_offset_t *p) { *p = (uintptr_t)upl->data; return 0; }
#define KERN_SUCCESS 0
static void ubc_upl_unmap(upl_t upl) { (void)upl; }
static void ubc_upl_commit_range(upl_t upl, size_t off, size_t len, int flags) {
    assert(off == 0 && len == PAGE_SIZE);
    assert(flags == (UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY)); upl->commits++;
}
static void ubc_upl_abort_range(upl_t upl, size_t off, size_t len, int flags) {
    assert(off == 0 && len == PAGE_SIZE && flags == UPL_ABORT_FREE_ON_EMPTY); upl->aborts++;
}
struct hc_cmd { uint32_t lba, blocks; };
struct hc_slot { uint8_t data[PAGE_SIZE]; uint32_t number, lba, blocks; bool used; };
static struct hc_slot slots[SLOTS];
static uint32_t hc_slot_buffer = PAGE_SIZE;
static bool hc_usable = true, hc_in = true, hc_write_enabled = true, hc_dead, hc_closed;
static _Atomic uint64_t hc_write_requests, hc_write_bytes, hc_write_errors;
static struct hc_slot *hc_take(bool wait) {
    (void)wait;
    for (unsigned i = 0; i < SLOTS; i++) {
        if (!slots[i].used) { slots[i].used = true; slots[i].number = i; return &slots[i]; }
    }
    return NULL;
}
static void hc_give(struct hc_slot *s) { assert(s->used); s->used = false; }
static void hc_make_write(struct hc_cmd *c, uint8_t lun, uint8_t tag, uint32_t lba, uint32_t n) {
    assert(lun == 0 && tag < SLOTS); c->lba = lba; c->blocks = n;
}
static int hc_place(struct hc_slot *s, struct hc_cmd *c, uint32_t n) {
    assert(n == c->blocks * BLOCK); s->lba = c->lba; s->blocks = c->blocks; return 0;
}
static int hc_ring(struct hc_slot *s) {
    uint64_t off = (uint64_t)s->lba * BLOCK - ANNFS_DATA_OFFSET;
    assert(off >= PAGE_SIZE && off + (uint64_t)s->blocks * BLOCK <= sizeof(device));
    if (!write_error) {
        memcpy(device + off, s->data, (size_t)s->blocks * BLOCK);
        memcpy(written, s->data, PAGE_SIZE); written_valid = true;
    }
    return 0;
}
static int hc_complete(struct hc_slot *s, const char *what, bool tur) {
    assert(s->used && what && !tur); return write_error;
}
static void FlushPoC_DcacheRegion(vm_offset_t p, size_t n) { assert(p && n == PAGE_SIZE); }
'''
    source += function(ufshc, 'ufshc_write', 'int')
    source += r'''
static int annibale_ufs_var_io(bool write, uint64_t off, void *p, uint64_t len) {
    assert(off >= ANNFS_DATA_OFFSET + PAGE_SIZE && len == PAGE_SIZE);
    race(write ? WRITE_IO : READ_IO);
    if (write) return ufshc_write(0, off, p, len);
    if (read_error) return read_error;
    assert(off - ANNFS_DATA_OFFSET + len <= sizeof(device));
    memcpy(p, device + off - ANNFS_DATA_OFFSET, len); return 0;
}
'''
    begin = disk.index('static int\nramfs_disk_read_block(')
    end = disk.index('static void\nramfs_disk_dirty_locked(', begin)
    source += disk[begin:end]
    source += function(main, 'ramfs_free_pages', 'static void')
    source += function(main, 'ramfs_resize', 'static void')
    source += (src / 'ramfs_store.inc').read_text()
    source += function(vnops, 'ramfs_write') + function(vnops, 'ramfs_pageout')
    source += r'''
static void await(bool *flag) {
    while (!*flag) {
        struct timespec deadline;
        assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0); deadline.tv_sec += 5;
        assert(pthread_cond_timedwait(&event_cv, &event_lock, &deadline) == 0);
    }
}
static void release_node(void) {
    if (release_action == EVICT) {
        ramfs_disk_evict_page_locked(&mount_data, &node, 0, race_page);
    } else {
        ramfs_resize(&node, 0);
        if (release_action == REUSE) {
            /* Another owner reuses the released physical block. */
            assert(ramfs_disk_bmap_alloc(&mount_data) == 1);
            memset(device + PAGE_SIZE, 0xa7, PAGE_SIZE);
        }
    }
}
static void *competitor(void *unused) {
    (void)unused;
    assert(pthread_mutex_lock(&event_lock) == 0); await(&requested);
    assert(pthread_mutex_unlock(&event_lock) == 0);
    int status = pthread_mutex_trylock(&mount_data.rm_lock);
    assert(status == 0 || status == EBUSY);
    if (status == 0) release_node();
    assert(pthread_mutex_lock(&event_lock) == 0);
    blocked = status == EBUSY; attempted = true;
    if (status == 0) finished = true;
    assert(pthread_cond_broadcast(&event_cv) == 0);
    assert(pthread_mutex_unlock(&event_lock) == 0);
    if (status == EBUSY) { lock(&mount_data.rm_lock); release_node(); }
    unlock(&mount_data.rm_lock);
    assert(pthread_mutex_lock(&event_lock) == 0); finished = true;
    assert(pthread_cond_broadcast(&event_cv) == 0);
    assert(pthread_mutex_unlock(&event_lock) == 0); return NULL;
}
static void race(enum phase phase) {
    if (pause_at != phase || phase == NONE) return;
    pause_at = NONE;
    assert(pthread_mutex_lock(&event_lock) == 0);
    requested = true; assert(pthread_cond_broadcast(&event_cv) == 0);
    await(&attempted);
    if (!blocked) await(&finished);
    assert(pthread_mutex_unlock(&event_lock) == 0);
}
static void setup(bool resident, bool backed, bool pressure) {
    pthread_mutexattr_t attr;
    assert(pthread_mutexattr_init(&attr) == 0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) == 0);
    assert(pthread_mutex_init(&mount_data.rm_lock, &attr) == 0);
    assert(pthread_mutexattr_destroy(&attr) == 0);
    mount_data.rm_disk_backed = backed; mount_data.rm_bmap_max = 3;
    mount_data.rm_pages = 1; mount_data.rm_max_pages = pressure ? 2 : 128;
    node.rn_mount = &mount_data; node.rn_type = VREG; node.rn_size = PAGE_SIZE;
    node.rn_nslots = node.rn_nalloc = 1; node.rn_pages = pages;
    node.rn_blocks = backed ? blocks : NULL; blocks[0] = backed ? 1 : 0;
    block_used[1] = backed; ramfs_var_used_pages = backed ? 1 : 0;
    memset(input, 0x73, sizeof(input)); memset(device + PAGE_SIZE, 0x35, PAGE_SIZE);
    if (resident) {
        pages[0] = ramfs_page_alloc(false); memset(pages[0], 0x35, PAGE_SIZE);
        ramfs_pages_in_use = 1;
    }
    race_page = pages[0];
}
static int write_file(bool partial) {
    last_uio = (struct uio){ .offset = partial ? 8 : 0,
                            .resid = partial ? 4 : PAGE_SIZE, .data = input };
    struct vnop_write_args args = { &node, &last_uio, 0 }; return ramfs_write(&args);
}
static int pageout_file(void) {
    last_upl = (struct test_upl){ .data = input };
    struct vnop_pageout_args args = { .a_vp = &node, .a_pl = &last_upl, .a_size = PAGE_SIZE };
    return ramfs_pageout(&args);
}
static void check_written(bool partial) {
    assert(hc_write_requests == 1 && written_valid);
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        assert(written[i] == (!partial || (i >= 8 && i < 12) ? 0x73 : 0x35));
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    bool pageout = strncmp(name, "pageout", 7) == 0;
    bool partial = strncmp(name, "partial", 7) == 0 || strcmp(name, "write-read-error") == 0;
    bool allocation_race = strcmp(name, "pageout-allocate-truncate") == 0;
    bool backed = strstr(name, "memory") == NULL;
    bool racing = strstr(name, "evict") || strstr(name, "truncate") || strstr(name, "reuse");
    bool error_case = strstr(name, "error") != NULL;
    setup(!partial && !allocation_race, backed, strstr(name, "evict") || error_case);
    if (strcmp(name, "write-read-error") == 0) read_error = EIO;
    if (strstr(name, "write-error")) write_error = EIO;
    pthread_t worker;
    if (racing) {
        release_action = strstr(name, "evict") ? EVICT : strstr(name, "reuse") ? REUSE : TRUNCATE;
        pause_at = allocation_race ? ALLOC_PAGE : partial ? READ_IO : WRITE_IO;
        assert(pthread_create(&worker, NULL, competitor, NULL) == 0);
    }
    int result = pageout ? pageout_file() : write_file(partial);
    if (racing) assert(pthread_join(worker, NULL) == 0 && finished);
    assert(result == (error_case ? EIO : 0));
    if (allocation_race) {
        assert(!blocked && hc_write_requests == 0 && node.rn_size == 0 && blocks[0] == 0);
        assert(last_upl.commits == 1 && last_upl.aborts == 0);
    } else if (read_error) {
        assert(last_uio.offset == 8 && last_uio.resid == 4);
        assert(pages[0] == NULL && blocks[0] == 1 && !mount_data.rm_meta_dirty);
        assert(hc_write_requests == 0);
    } else if (write_error) {
        assert(pages[0] && blocks[0] == 1 && mount_data.rm_meta_dirty && !written_valid);
        if (pageout) assert(last_upl.aborts == 1 && last_upl.commits == 0);
        else assert(last_uio.offset == PAGE_SIZE && last_uio.resid == 0);
    } else if (backed) {
        if (racing) assert(blocked);
        check_written(partial);
        if (racing && release_action != EVICT) assert(node.rn_size == 0 && blocks[0] == 0);
    } else {
        assert(hc_write_requests == 0 && !mount_data.rm_meta_dirty && pages[0]);
        assert(memcmp(pages[0], input, PAGE_SIZE) == 0);
    }
    lock(&mount_data.rm_lock); ramfs_resize(&node, 0); unlock(&mount_data.rm_lock);
    assert(mapped_pages == 0 && ramfs_pages_in_use == 0 && ramfs_var_used_pages == 0);
    assert(pthread_mutex_destroy(&mount_data.rm_lock) == 0);
    puts(name); return 0;
}
'''
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--case', choices=CASES, action='append')
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix='ramfs-page-lifetime-') as tmp:
        c = Path(tmp) / 'test.c'
        exe = Path(tmp) / 'test'
        c.write_text(harness(args.source_root))
        subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined',
                        str(c), '-pthread', '-o', str(exe)], check=True)
        for case in args.case or CASES:
            result = subprocess.run([str(exe), case], text=True, capture_output=True, timeout=15)
            print(f'{"FAIL" if result.returncode else "PASS"} {case}', flush=True)
            if result.returncode:
                failures.append(case)
                print(result.stderr, end='')
    if failures:
        raise SystemExit(f'ramfs page lifetime: {len(failures)} failed: {", ".join(failures)}')
    print('ramfs page lifetime: concurrent release, partial reads and I/O errors passed')


if __name__ == '__main__':
    main()

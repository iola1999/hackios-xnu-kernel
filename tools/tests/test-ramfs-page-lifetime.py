#!/usr/bin/env python3
"""Compile production in-memory ramfs writes and pageout against concurrent truncation."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CASES = (
    'write-memory', 'pageout-memory', 'write-truncate', 'pageout-truncate',
    'pageout-allocate-truncate', 'partial-preserve',
)


def function(text, name, kind='static int'):
    start = text.index(f'{kind}\n{name}(')
    return text[start:text.index('\n}\n', start) + 3]


def harness(root):
    src = root / 'xnu/files/bsd/miscfs/ramfs'
    main = (src / 'ramfs.c').read_text()
    vnops = (src / 'ramfs_vnops.inc').read_text()
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
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define Z_WAITOK 0
#define Z_NOFAIL 0
#define IO_APPEND 1
#define UPL_NOCOMMIT 1
#define UPL_ABORT_FREE_ON_EMPTY 2
#define UPL_COMMIT_CLEAR_DIRTY 4
#define UPL_COMMIT_FREE_ON_EMPTY 8
#define KERN_SUCCESS 0
#define relaxed memory_order_relaxed
#define os_atomic_inc(p,o) atomic_fetch_add_explicit(p, 1, o)
#define os_atomic_dec(p,o) atomic_fetch_sub_explicit(p, 1, o)
typedef pthread_mutex_t lck_mtx_t;
typedef uintptr_t vm_offset_t;
typedef int64_t user_ssize_t;
typedef size_t upl_size_t;
enum vtype { VREG, VDIR };
struct ramfs_mount {
    lck_mtx_t rm_lock;
    uint64_t rm_pages, rm_max_pages;
};
struct ramfs_node {
    struct ramfs_mount *rn_mount;
    enum vtype rn_type;
    uint64_t rn_size, rn_nslots, rn_nalloc;
    void **rn_pages;
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
static _Atomic uint64_t ramfs_pages_in_use;
static _Atomic unsigned mapped_pages;
static struct ramfs_mount mount_data;
static struct ramfs_node node;
static void *pages[1];
static uint8_t input[PAGE_SIZE];
static struct uio last_uio;
static struct test_upl last_upl;
enum phase { NONE, ALLOC_PAGE, TOUCH_LOCKED };
static enum phase pause_at;
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
static int ramfs_grow_slots(struct ramfs_node *n, uint64_t i) {
    assert(n == &node && i == 0 && n->rn_nslots == 1); return 0;
}
static void ramfs_reserve_slots(struct ramfs_mount *rm, struct ramfs_node *n, uint64_t i) {
    assert(rm == &mount_data && n == &node && i == 0);
    race(ALLOC_PAGE);
}
static off_t uio_offset(struct uio *u) { return u->offset; }
static user_ssize_t uio_resid(struct uio *u) { return u->resid; }
static void uio_setoffset(struct uio *u, off_t o) { u->offset = o; }
static void uio_setresid(struct uio *u, user_ssize_t n) { u->resid = n; }
static int uiomove(void *to, int n, struct uio *u) {
    assert(n >= 0 && n <= u->resid);
    memcpy(to, u->data, (size_t)n); u->offset += n; u->resid -= n; u->data += n; return 0;
}
static int ramfs_sync_mapping(vnode_t vp, struct ramfs_node *n) { assert(vp == n); return 0; }
static void ramfs_touch(struct ramfs_node *n, bool data) { (void)n; (void)data; race(TOUCH_LOCKED); }
static int ubc_setsize(vnode_t vp, off_t n) { (void)vp; (void)n; return 0; }
static bool ubc_pages_resident(vnode_t vp) { (void)vp; return false; }
static void ramfs_refresh_cache(vnode_t vp, struct ramfs_node *n, uint64_t a, uint64_t b) {
    (void)vp; (void)n; (void)a; (void)b; assert(false);
}
static int ubc_upl_map(upl_t upl, vm_offset_t *p) { *p = (uintptr_t)upl->data; return 0; }
static void ubc_upl_unmap(upl_t upl) { (void)upl; }
static void ubc_upl_commit_range(upl_t upl, size_t off, size_t len, int flags) {
    assert(off == 0 && len == PAGE_SIZE);
    assert(flags == (UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY)); upl->commits++;
}
static void ubc_upl_abort_range(upl_t upl, size_t off, size_t len, int flags) {
    assert(off == 0 && len == PAGE_SIZE && flags == UPL_ABORT_FREE_ON_EMPTY); upl->aborts++;
}
'''
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
static void *competitor(void *unused) {
    (void)unused;
    assert(pthread_mutex_lock(&event_lock) == 0); await(&requested);
    assert(pthread_mutex_unlock(&event_lock) == 0);
    int status = pthread_mutex_trylock(&mount_data.rm_lock);
    assert(status == 0 || status == EBUSY);
    if (status == 0) ramfs_resize(&node, 0);
    assert(pthread_mutex_lock(&event_lock) == 0);
    blocked = status == EBUSY; attempted = true;
    if (status == 0) finished = true;
    assert(pthread_cond_broadcast(&event_cv) == 0);
    assert(pthread_mutex_unlock(&event_lock) == 0);
    if (status == EBUSY) { lock(&mount_data.rm_lock); ramfs_resize(&node, 0); }
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
static void setup(bool resident) {
    pthread_mutexattr_t attr;
    assert(pthread_mutexattr_init(&attr) == 0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) == 0);
    assert(pthread_mutex_init(&mount_data.rm_lock, &attr) == 0);
    assert(pthread_mutexattr_destroy(&attr) == 0);
    mount_data.rm_pages = resident ? 1 : 0; mount_data.rm_max_pages = 128;
    node.rn_mount = &mount_data; node.rn_type = VREG; node.rn_size = PAGE_SIZE;
    node.rn_nslots = 1; node.rn_nalloc = resident ? 1 : 0; node.rn_pages = pages;
    pages[0] = NULL; ramfs_pages_in_use = 0;
    memset(input, 0x73, sizeof(input));
    if (resident) {
        pages[0] = ramfs_page_alloc(false); memset(pages[0], 0x35, PAGE_SIZE);
        ramfs_pages_in_use = 1;
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    bool pageout = strncmp(name, "pageout", 7) == 0;
    bool partial = strncmp(name, "partial", 7) == 0;
    bool allocation_race = strcmp(name, "pageout-allocate-truncate") == 0;
    bool racing = strstr(name, "truncate") != NULL;
    setup(!allocation_race);
    pthread_t worker;
    if (racing) {
        pause_at = (pageout && !allocation_race) ? TOUCH_LOCKED : ALLOC_PAGE;
        assert(pthread_create(&worker, NULL, competitor, NULL) == 0);
    }
    int result;
    if (pageout) {
        last_upl = (struct test_upl){ .data = input };
        struct vnop_pageout_args args = { .a_vp = &node, .a_pl = &last_upl, .a_size = PAGE_SIZE };
        result = ramfs_pageout(&args);
    } else {
        last_uio = (struct uio){ .offset = partial ? 8 : 0,
                                .resid = partial ? 4 : PAGE_SIZE, .data = input };
        struct vnop_write_args args = { &node, &last_uio, 0 };
        result = ramfs_write(&args);
    }
    if (racing) assert(pthread_join(worker, NULL) == 0 && finished);
    assert(result == 0);
    if (allocation_race) {
        assert(!blocked && node.rn_size == 0 && pages[0] == NULL);
        assert(last_upl.commits == 1 && last_upl.aborts == 0);
    } else if (!racing) {
        assert(pages[0] != NULL);
        for (size_t i = 0; i < PAGE_SIZE; i++) {
            assert(((const uint8_t *)pages[0])[i] == (!partial || (i >= 8 && i < 12) ? 0x73 : 0x35));
        }
    }
    lock(&mount_data.rm_lock); ramfs_resize(&node, 0); unlock(&mount_data.rm_lock);
    assert(mapped_pages == 0 && ramfs_pages_in_use == 0 && mount_data.rm_pages == 0);
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
    print('ramfs page lifetime: concurrent truncation and partial writes passed')


if __name__ == '__main__':
    main()

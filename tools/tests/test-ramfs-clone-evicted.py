#!/usr/bin/env python3
"""Compile production RAMFS clonefile contents against evicted source pages."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CASES = ('evicted-over-cap', 'evicted-under-cap', 'hole', 'read-error')


def function(text, name, kind='static int'):
    start = text.index(f'{kind}\n{name}(')
    return text[start:text.index('\n}\n', start) + 3]


def harness(root):
    src = root / 'xnu/files/bsd/miscfs/ramfs'
    disk = (src / 'ramfs_disk.inc').read_text()
    clone = (src / 'ramfs_clonefile.inc').read_text()
    source = r'''
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#define PAGE_SIZE 16384
#define NPAGES 3
#define NBLOCKS 8
#define ANNFS_DATA_OFFSET (512ULL << 20)
#define CLONE_NOOWNERCOPY 2
#define Z_WAITOK 0
#define Z_ZERO 0
#define relaxed memory_order_relaxed
#define os_atomic_inc(p,o) atomic_fetch_add_explicit(p, 1, o)
#define os_atomic_dec(p,o) atomic_fetch_sub_explicit(p, 1, o)
#define os_atomic_load(p,o) atomic_load_explicit(p, o)
#define kalloc_data(n, f) calloc(1, (n))
#define kfree_data(p, n) free(p)
#define kalloc_type(t, f) calloc(1, sizeof(t))
#define kfree_type(t, p) free(p)
enum vtype { VNON, VREG, VDIR, VLNK };
struct ramfs_mount {
    bool rm_disk_backed;
    void *rm_io_page;
    uint64_t rm_pages, rm_max_pages, rm_nodes, rm_next_ino;
    uint32_t rm_bmap_max;
};
struct ramfs_node;
struct ramfs_dirent {
    TAILQ_ENTRY(ramfs_dirent) rd_link;
    struct ramfs_node *rd_node;
    char *rd_name;
    uint32_t rd_namelen;
};
struct ramfs_xattr {
    TAILQ_ENTRY(ramfs_xattr) rx_link;
    char *rx_name;
    void *rx_data;
    size_t rx_namelen, rx_size;
};
struct ramfs_node {
    struct ramfs_mount *rn_mount;
    enum vtype rn_type;
    uint64_t rn_size, rn_nslots, rn_nalloc, rn_ino;
    uint32_t rn_mode, rn_uid, rn_gid, rn_nlink, rn_dataprotect_class;
    void **rn_pages;
    uint32_t *rn_blocks;
    char *rn_target;
    struct ramfs_node *rn_parent;
    TAILQ_HEAD(, ramfs_dirent) rn_entries;
    TAILQ_HEAD(, ramfs_xattr) rn_xattrs;
};
static _Atomic uint64_t ramfs_pages_in_use, ramfs_var_used_pages;
static uint8_t device[NBLOCKS][PAGE_SIZE];
static bool block_used[NBLOCKS];
static int read_error;
static unsigned live_pages;
static void *ramfs_page_alloc(bool wait) { (void)wait; live_pages++; return calloc(1, PAGE_SIZE); }
static void ramfs_page_free(void *p) { assert(p && live_pages); live_pages--; free(p); }
static uint32_t ramfs_disk_bmap_alloc(struct ramfs_mount *rm) {
    for (uint32_t b = 1; b < rm->rm_bmap_max; b++)
        if (!block_used[b]) { block_used[b] = true; return b; }
    return 0;
}
static int ramfs_grow_slots(struct ramfs_node *n, uint64_t i) {
    n->rn_nslots = i + 1;
    n->rn_pages = calloc(n->rn_nslots, sizeof(void *));
    n->rn_blocks = calloc(n->rn_nslots, sizeof(uint32_t));
    return 0;
}
static int annibale_ufs_var_io(bool write, uint64_t off, void *p, uint64_t len) {
    uint64_t b = (off - ANNFS_DATA_OFFSET) / PAGE_SIZE;
    assert(len == PAGE_SIZE && b > 0 && b < NBLOCKS);
    if (write) { memcpy(device[b], p, PAGE_SIZE); return 0; }
    if (read_error) return read_error;
    memcpy(p, device[b], PAGE_SIZE); return 0;
}
/* Directory branches are not reached by these cases. */
static struct ramfs_node *ramfs_node_alloc(struct ramfs_mount *rm, enum vtype t, void *v) { (void)rm; (void)t; (void)v; abort(); }
static struct ramfs_dirent *ramfs_dirent_alloc(const char *n, size_t l) { (void)n; (void)l; abort(); }
static void ramfs_dirent_free(struct ramfs_dirent *d) { (void)d; abort(); }
static void ramfs_node_free(struct ramfs_node *n) { (void)n; abort(); }
static void ramfs_hash_insert(struct ramfs_mount *rm, struct ramfs_node *n) { (void)rm; (void)n; abort(); }
static void ramfs_dir_add(struct ramfs_node *d, struct ramfs_dirent *e, struct ramfs_node *n) { (void)d; (void)e; (void)n; abort(); }
'''
    begin = disk.index('static int\nramfs_disk_read_block(')
    end = disk.index('static void\nramfs_disk_dirty_locked(', begin)
    source += disk[begin:end]
    source += function(clone, 'ramfs_clone_contents')
    source += r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    bool over = strcmp(name, "evicted-under-cap") != 0;
    struct ramfs_mount rm = { .rm_disk_backed = true, .rm_bmap_max = NBLOCKS, .rm_max_pages = over ? 4 : 64 };
    struct ramfs_node src = { .rn_mount = &rm, .rn_type = VREG, .rn_size = NPAGES * PAGE_SIZE };
    struct ramfs_node dst = { .rn_mount = &rm, .rn_type = VREG };
    TAILQ_INIT(&src.rn_entries); TAILQ_INIT(&src.rn_xattrs);
    TAILQ_INIT(&dst.rn_entries); TAILQ_INIT(&dst.rn_xattrs);
    rm.rm_io_page = ramfs_page_alloc(true);
    ramfs_grow_slots(&src, NPAGES - 1);
    /* page 0 resident and on disk, page 1 evicted to disk, page 2 evicted or a hole */
    for (uint32_t i = 0; i < NPAGES; i++) {
        if (i == 2 && strcmp(name, "hole") == 0) continue;
        src.rn_blocks[i] = ramfs_disk_bmap_alloc(&rm);
        memset(device[src.rn_blocks[i]], 0x41 + (int)i, PAGE_SIZE);
    }
    src.rn_pages[0] = ramfs_page_alloc(true);
    memset(src.rn_pages[0], 0x41, PAGE_SIZE);
    ramfs_pages_in_use = over ? rm.rm_max_pages / 2 : 1;
    uint64_t in_use = ramfs_pages_in_use;
    if (strcmp(name, "read-error") == 0) read_error = EIO;

    int error = ramfs_clone_contents(&rm, &src, &dst, 0);

    if (read_error) { assert(error == EIO); puts(name); return 0; }
    assert(error == 0 && dst.rn_size == src.rn_size);
    for (uint32_t i = 0; i < NPAGES; i++) {
        bool hole = i == 2 && strcmp(name, "hole") == 0;
        uint32_t b = dst.rn_blocks[i];
        if (hole) { assert(b == 0 && dst.rn_pages[i] == NULL); continue; }
        assert(b != 0 && b != src.rn_blocks[i]);
        for (size_t k = 0; k < PAGE_SIZE; k++) assert(device[b][k] == 0x41 + i);
        if (dst.rn_pages[i] != NULL)
            for (size_t k = 0; k < PAGE_SIZE; k++) assert(((uint8_t *)dst.rn_pages[i])[k] == 0x41 + i);
        /* the clone keeps no source page resident that was evicted before */
        if (i > 0) assert(src.rn_pages[i] == NULL);
    }
    if (over) assert(ramfs_pages_in_use == in_use && dst.rn_pages[0] == NULL && dst.rn_pages[1] == NULL);
    else assert(ramfs_pages_in_use == in_use + 3 && dst.rn_pages[1] != NULL);
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
    with tempfile.TemporaryDirectory(prefix='ramfs-clone-evicted-') as tmp:
        c = Path(tmp) / 'test.c'
        exe = Path(tmp) / 'test'
        c.write_text(harness(args.source_root))
        subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined',
                        str(c), '-o', str(exe)], check=True)
        for case in args.case or CASES:
            result = subprocess.run([str(exe), case], text=True, capture_output=True, timeout=15)
            print(f'{"FAIL" if result.returncode else "PASS"} {case}', flush=True)
            if result.returncode:
                failures.append(case)
                print(result.stderr, end='')
    if failures:
        raise SystemExit(f'ramfs clone evicted: {len(failures)} failed: {", ".join(failures)}')
    print('ramfs clone evicted: evicted, resident and hole pages cloned with their data')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Compare ANNFS packing bytes with a selected revision; optionally time both functions."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REL = "xnu/files/bsd/miscfs/ramfs/ramfs_disk.inc"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--reference", default="a0e46547", help="pre-optimization Git revision")
parser.add_argument("--benchmark", action="store_true", help="use optimized native code without sanitizers")
args = parser.parse_args()
main = (ROOT / "xnu/files/bsd/miscfs/ramfs/ramfs.c").read_text()
disk = (ROOT / REL).read_text()
old = subprocess.check_output(["git", "show", f"{args.reference}:{REL}"], cwd=ROOT, text=True)


def pack(text):
    return text[text.index("static int\nramfs_disk_pack_locked"):text.index("static int\nramfs_disk_sync_impl")]


source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <time.h>
#define PAGE_SIZE 16384
#define PAGE_MASK (PAGE_SIZE - 1)
#define RAMFS_ROOT_INO 2
typedef void *mount_t;
typedef void *vnode_t;
typedef int lck_mtx_t;
enum vtype { VNON, VREG, VDIR, VBLK, VCHR, VLNK, VSOCK, VFIFO, VBAD };
'''
source += main[main.index("struct ramfs_mount;"):main.index("/* file pages in use")]
source += disk[disk.index("#define ANNFS_MAGIC_STR"):disk.index("static struct ramfs_mount *ramfs_disk_rm;")]
source += pack(old).replace("ramfs_disk_pack_locked", "reference_pack")
source += pack(disk)
source += r'''
typedef int (*pack_fn)(struct ramfs_mount *, uint64_t *, uint32_t *);
static unsigned comparisons;
static void compare(struct ramfs_mount *rm, size_t cap, unsigned char poison) {
    size_t allocation = cap + 64;
    unsigned char *a = malloc(allocation), *b = malloc(allocation);
    assert(a && b);
    memset(a, poison, allocation); memset(b, poison, allocation);
    uint64_t na = UINT64_MAX, nb = UINT64_MAX;
    uint32_t ca = UINT32_MAX, cb = UINT32_MAX;
    rm->rm_meta_cap = cap; rm->rm_meta_buf = a;
    int ea = reference_pack(rm, &na, &ca);
    rm->rm_meta_buf = b;
    int eb = ramfs_disk_pack_locked(rm, &nb, &cb);
    assert(ea == eb && na == nb && ca == cb);
    assert(ea == 0 || ea == ENOSPC);
    assert(memcmp(a, b, allocation) == 0);
    for (size_t i = cap; i < allocation; i++) assert(b[i] == poison);
    comparisons++;
    rm->rm_meta_buf = NULL;
    free(a); free(b);
}
static void init_node(struct ramfs_node *n, uint64_t ino, enum vtype type) {
    memset(n, 0, sizeof(*n));
    n->rn_ino = ino; n->rn_type = type; n->rn_nlink = 1;
    n->rn_uid = 501; n->rn_gid = 20; n->rn_mode = 0755;
    n->rn_bsdflags = 7; n->rn_dataprotect_class = 4;
    n->rn_docid = (uint32_t)ino + 100; n->rn_write_gen = 3;
    n->rn_atime = (struct timespec){1234, 567};
    n->rn_mtime = (struct timespec){2345, 678};
    n->rn_ctime = (struct timespec){3456, 789};
    n->rn_btime = (struct timespec){4567, 890};
    TAILQ_INIT(&n->rn_xattrs); TAILQ_INIT(&n->rn_entries);
}
static void correctness(void) {
    struct ramfs_node *hash[RAMFS_HASH_SIZE] = {0};
    struct ramfs_mount rm = {.rm_hash = hash};
    struct ramfs_node nodes[5];
    enum vtype types[] = {VDIR, VREG, VLNK, VSOCK, VREG};
    char text[256]; uint32_t blocks[32];
    for (unsigned i = 0; i < sizeof(text); i++) text[i] = (char)(i * 17 + 1);
    for (unsigned i = 0; i < 32; i++) blocks[i] = (i % 3) ? 100 + i : 0;
    for (unsigned i = 0; i < 5; i++) {
        init_node(&nodes[i], 2 + i, types[i]);
        nodes[i].rn_parent = &nodes[0];
        hash[i] = &nodes[i];
    }
    nodes[4].rn_nlink = 0;
    nodes[1].rn_blocks = blocks;
    nodes[2].rn_target = text;
    struct ramfs_xattr attrs[3] = {0};
    for (unsigned i = 0; i < 3; i++) {
        attrs[i].rx_name = text;
        attrs[i].rx_data = i == 1 ? NULL : text;
        TAILQ_INSERT_TAIL(&nodes[1].rn_xattrs, &attrs[i], rx_link);
    }
    struct ramfs_dirent entries[5] = {0};
    for (unsigned i = 0; i < 5; i++) {
        entries[i].rd_name = text;
        entries[i].rd_node = i == 0 ? NULL : &nodes[i];
        TAILQ_INSERT_TAIL(&nodes[0].rn_entries, &entries[i], rd_link);
    }
    unsigned char full[16384];
    for (unsigned len = 0; len <= 31; len++) {
        nodes[2].rn_size = len;
        nodes[1].rn_nslots = len;
        nodes[1].rn_size = len ? (len + (len % 2)) * PAGE_SIZE - 1 : 0;
        for (unsigned i = 0; i < 3; i++) {
            attrs[i].rx_namelen = (len + i) % 32;
            attrs[i].rx_size = (len * 7 + i) % sizeof(text);
        }
        for (unsigned i = 0; i < 5; i++) entries[i].rd_namelen = (len + i) % 32;
        rm.rm_meta_cap = sizeof(full); rm.rm_meta_buf = full;
        uint64_t bytes = 0; uint32_t count = 0;
        assert(reference_pack(&rm, &bytes, &count) == 0 && count == 3);
        for (size_t cap = ANNFS_SUPER_BYTES; cap <= ANNFS_SUPER_BYTES + bytes + 1; cap++)
            compare(&rm, cap, (unsigned char)(0xa5 ^ len));
    }
    attrs[1].rx_size = 0;
    for (unsigned len = 0; len <= 15; len++) {
        attrs[1].rx_namelen = len;
        compare(&rm, sizeof(full), 0xc7);
    }
    nodes[2].rn_target = NULL;
    nodes[1].rn_blocks = NULL;
    compare(&rm, sizeof(full), 0x5a);
    memset(hash, 0, sizeof(hash));
    compare(&rm, ANNFS_SUPER_BYTES, 0x5a);
    printf("PASS: %u byte-for-byte comparisons, including ENOSPC and padding\n", comparisons);
}
static double now(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static volatile uint64_t consumed;
static double measure(pack_fn fn, struct ramfs_mount *rm, unsigned loops) {
    double start = now();
    for (unsigned i = 0; i < loops; i++) {
        uint64_t bytes; uint32_t count;
        assert(fn(rm, &bytes, &count) == 0);
        consumed += bytes + count + ((unsigned char *)rm->rm_meta_buf)[ANNFS_SUPER_BYTES];
    }
    return (now() - start) * 1e6 / loops;
}
static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void benchmark(unsigned count, unsigned data_bytes, unsigned block_count) {
    struct ramfs_node *hash[RAMFS_HASH_SIZE] = {0};
    struct ramfs_mount rm = {.rm_hash = hash, .rm_meta_cap = 32u << 20};
    struct ramfs_node *nodes = calloc(count, sizeof(*nodes));
    struct ramfs_xattr *attrs = calloc(count, sizeof(*attrs));
    uint32_t blocks[256]; unsigned char data[4096];
    memset(blocks, 0x37, sizeof(blocks)); memset(data, 0x79, sizeof(data));
    assert(nodes && attrs && data_bytes <= sizeof(data) && block_count <= 256);
    for (unsigned i = 0; i < count; i++) {
        init_node(&nodes[i], i + 2, VREG);
        nodes[i].rn_size = block_count * PAGE_SIZE; nodes[i].rn_nslots = block_count;
        nodes[i].rn_blocks = blocks;
        unsigned h = i % RAMFS_HASH_SIZE;
        nodes[i].rn_hnext = hash[h]; hash[h] = &nodes[i];
        attrs[i].rx_name = "user.fixture"; attrs[i].rx_namelen = 12;
        attrs[i].rx_data = data; attrs[i].rx_size = data_bytes;
        TAILQ_INSERT_TAIL(&nodes[i].rn_xattrs, &attrs[i], rx_link);
    }
    compare(&rm, rm.rm_meta_cap, 0xa5);
    rm.rm_meta_buf = malloc(rm.rm_meta_cap);
    assert(rm.rm_meta_buf);
    unsigned loops = count < 1000 ? 1000 : 20;
    double before[9], after[9];
    measure(reference_pack, &rm, 2); measure(ramfs_disk_pack_locked, &rm, 2);
    for (unsigned i = 0; i < 9; i++) {
        if (i % 2) {
            after[i] = measure(ramfs_disk_pack_locked, &rm, loops);
            before[i] = measure(reference_pack, &rm, loops);
        } else {
            before[i] = measure(reference_pack, &rm, loops);
            after[i] = measure(ramfs_disk_pack_locked, &rm, loops);
        }
    }
    qsort(before, 9, sizeof(double), cmp); qsort(after, 9, sizeof(double), cmp);
    uint64_t bytes; uint32_t n;
    assert(ramfs_disk_pack_locked(&rm, &bytes, &n) == 0);
    printf("BENCH nodes=%u xattr=%u blocks=%u payload=%llu median_us=%.3f -> %.3f change=%.2f%%\n",
           count, data_bytes, block_count, (unsigned long long)bytes,
           before[4], after[4], (after[4] / before[4] - 1) * 100);
    free(rm.rm_meta_buf); free(attrs); free(nodes);
}
int main(int argc, char **argv) {
    (void)argv;
    correctness();
    if (argc > 1) {
        benchmark(256, 16, 4);
        benchmark(70000, 64, 4);
        benchmark(4096, 4096, 256);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="ramfs-pack-") as tmp:
    src = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    src.write_text(source)
    flags = ["-O2"] if args.benchmark else ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run(["clang", "-std=gnu11", *flags, str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe), *(["bench"] if args.benchmark else [])], check=True)

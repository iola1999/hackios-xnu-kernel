#!/usr/bin/env python3
"""Host ASan+UBSan tests for S9 APFS ENOSPC prechecks, clean 0-byte write rollback, clone xattr sizing, and IP pool reserve."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
APFS_DIR = ROOT / "xnu/files/bsd/miscfs/apfs"

STUBS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define WTX_HASH 16
#define WTX_MAXLEV 4
#define WCL_CHUNK (64 * 1024)
#define WCL_MAX_ITEMS 1024
#define WCL_NONE UINT32_MAX
#define WCL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define APFS_XATTR_MAX_EMBEDDED_SIZE 3804u
#define APFS_XATTR_DATA_STREAM 0x1u
#define APFS_S_IFMT 0170000
#define APFS_S_IFREG 0100000
#define APFS_INO_EXT_TYPE_DSTREAM 8
#define UF_COMPRESSED 0x20
#define IO_APPEND 0x02
#define IO_SYNC 0x10
#define IO_TAILZEROFILL 0x40
#define IO_HEADZEROFILL 0x80
#define IO_NOZEROVALID 0x100
#define IO_NOZERODIRTY 0x200
#define APFS_WA_MTIME 0x020
#define APFS_WHY_IOSYNC 2
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#ifndef __unused
#define __unused __attribute__((unused))
#endif

struct apfs_xattr;
typedef int (*apfs_xattr_cb)(void *, const char *, size_t, const struct apfs_xattr *);

struct apfs_xattr {
    uint16_t flags;
    const uint8_t *data;
    uint16_t data_len;
    uint64_t stream_id;
    uint64_t stream_size;
};

struct apfs_inode {
    uint64_t mod_time;
    uint32_t bsd_flags;
    uint32_t pclass;
};

struct apfs_vol {
    uint64_t alloc_count;
    struct { uint64_t root, root_oid; } tree;
    struct { struct { uint64_t root; } tree; } omap;
    uint64_t extentref_oid;
};

struct apfs_nx {
    uint32_t block_size;
    uint64_t view_xid;
    struct { struct { uint64_t root; } tree; } omap;
};

struct wblk {
    uint64_t addr;
    uint8_t *data;
    bool raw;
    struct wblk *next;
};

struct wtree { uint64_t root, root_oid; };
struct wfreed { uint64_t addr, count; bool vol_owned; };

struct wchunk {
    uint64_t addr;
    uint32_t blocks;
    uint32_t free;
    uint64_t xid;
    uint64_t bitmap_addr;
    uint8_t *bitmap;
    bool dirty;
};

struct wspace {
    uint8_t *sm;
    uint32_t sm_size;
    uint64_t sm_oid;
    uint32_t per_chunk, per_cib, per_cab;
    uint64_t block_count, chunk_count;
    uint32_t cib_count, cab_count;
    uint64_t free_count;
    struct wchunk *chunks;
    uint64_t *cib_addr;
    bool *cib_dirty;
    uint64_t *cab_addr;
    uint32_t hint;
    uint64_t ip_base, ip_blocks, ip_bm_base;
    uint32_t ip_bm_size, ip_bm_blocks;
    uint8_t *ip_bm;
    uint32_t ip_hint;
    struct wfreed *freed;
    uint32_t nfreed, freed_cap;
    uint64_t *ip_freed;
    uint32_t nip_freed, ip_freed_cap;
};

struct apfs_wtx {
    struct apfs_nx *nx;
    struct apfs_vol *vol;
    uint32_t bs;
    bool open;
    bool broken;
    uint64_t xid;
    struct wblk **hash;
    uint32_t nblk;
    struct wtree fs, omap, eref, nx_omap;
    uint64_t vol_omap_obj, nx_omap_obj, vsb_addr;
    uint8_t *vsb, *nxsb;
    uint64_t next_oid;
    struct wspace sp;
    struct apfs_vol saved_vol;
    uint64_t saved_nx_omap_root, saved_next_oid;
    uint8_t *saved_vsb;
    uint64_t ops, commits, commit_blocks, commit_ticks, cow_blocks, splits;
};

struct apfs_mount {
    struct apfs_nx am_nx;
    struct apfs_vol am_vol;
    struct apfs_wtx *am_w;
};

struct apfs_node {
    struct apfs_mount *an_mount;
    uint64_t an_id;
    uint64_t an_size;
    uint64_t an_wseq;
    struct apfs_inode an_ino;
    int an_rwlock;
};

typedef struct vnode {
    bool regular;
    off_t ubc_size;
    struct apfs_node node;
} *vnode_t;

struct uio {
    off_t offset;
    user_ssize_t resid;
};

struct vnop_write_args {
    vnode_t a_vp;
    struct uio *a_uio;
    int a_ioflag;
};

struct apfs_wattr { uint32_t set; uint64_t mtime; };
struct winode { uint64_t id; uint64_t size; };

#define APFS_VP(vp) (&(vp)->node)
#define APFS_WCALL(am, an, other, call) ((void)(am), (void)(an), (void)(other), (call))

static void *apfs_alloc(size_t n) { return calloc(1, n ? n : 1); }
static void apfs_free(void *p, size_t n) { (void)n; free(p); }
static uint64_t apfs_now_ns(void) { return 999999ULL; }

static void lck_rw_lock_exclusive(int *l) { (void)l; }
static void lck_rw_unlock_exclusive(int *l) { (void)l; }
static void apfs_lock(struct apfs_mount *am) { (void)am; }
static void apfs_unlock(struct apfs_mount *am) { (void)am; }
static void apfs_lock_exclusive(struct apfs_mount *am) { (void)am; }
static void apfs_unlock_exclusive(struct apfs_mount *am) { (void)am; }
static int vnode_isreg(vnode_t vp) { return vp->regular; }
static int vnode_isdir(vnode_t vp) { (void)vp; return 0; }
static off_t uio_offset(struct uio *u) { return u->offset; }
static void uio_setoffset(struct uio *u, off_t o) { u->offset = o; }
static user_ssize_t uio_resid(struct uio *u) { return u->resid; }
static bool os_add_overflow(off_t a, user_ssize_t b, off_t *out) {
    if (b < 0 || a > INT64_MAX - b) return true;
    *out = a + b;
    return false;
}
static int ubc_setsize(vnode_t vp, off_t sz) { vp->ubc_size = sz; return 1; }
static void apfs_nx_cache_flush(struct apfs_nx *nx) { (void)nx; }
static void apfs_omap_cache_clear(struct apfs_nx *nx) { (void)nx; }

static inline bool bit_get(const uint8_t *bm, uint32_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(uint8_t *bm, uint32_t i, bool on) {
    if (on) bm[i >> 3] |= (uint8_t)(1u << (i & 7));
    else bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static struct apfs_wtx *apfs_wtx_of(struct apfs_mount *am) { return am->am_w; }

static void wtx_drop_all(struct apfs_wtx *w) {
    for (uint32_t i = 0; i < WTX_HASH; i++) {
        while (w->hash[i] != NULL) {
            struct wblk *b = w->hash[i];
            w->hash[i] = b->next;
            apfs_free(b->data, w->bs);
            apfs_free(b, sizeof(*b));
            w->nblk--;
        }
    }
}

static int wsp_free_now(struct apfs_wtx *w, uint64_t addr, uint64_t count) {
    uint64_t c = addr / w->sp.per_chunk;
    if (c >= w->sp.chunk_count) return EIO;
    struct wchunk *ch = &w->sp.chunks[c];
    for (uint64_t i = 0; i < count; i++) {
        uint32_t idx = (uint32_t)(addr - ch->addr + i);
        if (!bit_get(ch->bitmap, idx)) return EIO;
        bit_set(ch->bitmap, idx, false);
        ch->free++;
        w->sp.free_count++;
    }
    return 0;
}

static void wtx_poison(struct apfs_wtx *w, int error) { (void)error; w->broken = true; }

static int mock_cluster_err = 0;
static user_ssize_t mock_cluster_consume = 0;
static int mock_setattr_calls = 0;
static int mock_wnote_calls = 0;
static int mock_wcommit_calls = 0;
static int mock_wresize_calls = 0;
static struct apfs_xattr mock_xattrs[4];
static uint32_t mock_nxattrs = 0;
static uint64_t mock_file_alloced = 4096;

static void apfs_wnote(struct apfs_mount *am, uint64_t id) { (void)am; (void)id; mock_wnote_calls++; }
static int apfs_wcommit(struct apfs_mount *am, bool p, uint64_t u, int why) {
    (void)am; (void)p; (void)u; (void)why;
    mock_wcommit_calls++;
    return 0;
}
static int wop_begin(struct apfs_wtx *w, uint64_t data_blocks);
static int apfs_w_setattr(struct apfs_wtx *w, uint64_t id, const struct apfs_wattr *a) {
    (void)id;
    int err = wop_begin(w, 0);
    if (err) return err;
    mock_setattr_calls++;
    w->open = true;
    (void)a;
    return 0;
}
static int apfs_wresize(vnode_t vp, struct apfs_node *an, uint64_t size, int ioflag) {
    (void)ioflag;
    mock_wresize_calls++;
    struct apfs_wtx *w = an->an_mount->am_w;
    if (size > an->an_size) {
        int err = wop_begin(w, 0);
        if (err) return err;
        /* Simulate allocating 1 metadata tree block in chunk 0. */
        if (!w->open) {
            w->saved_vol = w->vol ? *w->vol : (struct apfs_vol){0};
            w->saved_nx_omap_root = w->nx->omap.tree.root;
            w->saved_next_oid = w->next_oid;
            memcpy(w->saved_vsb, w->vsb, w->bs);
            w->open = true;
        }
        uint64_t blk_addr = 10 + w->nblk;
        bit_set(w->sp.chunks[0].bitmap, (uint32_t)blk_addr, true);
        w->sp.chunks[0].free--;
        w->sp.chunks[0].dirty = true;
        w->sp.cib_dirty[0] = true;
        w->sp.free_count--;
        struct wblk *b = apfs_alloc(sizeof(*b));
        b->addr = blk_addr;
        b->data = apfs_alloc(w->bs);
        b->next = w->hash[blk_addr % WTX_HASH];
        w->hash[blk_addr % WTX_HASH] = b;
        w->nblk++;
        w->vol->tree.root = blk_addr;
    }
    an->an_size = size;
    an->an_ino.mod_time = apfs_now_ns();
    an->an_wseq = w->commits + 1;
    ubc_setsize(vp, (off_t)size);
    return 0;
}
static int cluster_write(vnode_t vp, struct uio *uio, off_t old, off_t size, off_t h, off_t t, int flags) {
    (void)vp; (void)old; (void)size; (void)h; (void)t; (void)flags;
    if (mock_cluster_consume > 0) {
        user_ssize_t take = mock_cluster_consume <= uio->resid ? mock_cluster_consume : uio->resid;
        uio->offset += take;
        uio->resid -= take;
    }
    return mock_cluster_err;
}

static int wi_load(struct apfs_wtx *w, uint64_t id, struct winode *i) { (void)w; i->id = id; return 0; }
static bool wi_isdir(const struct winode *i) { (void)i; return false; }
static uint16_t wi_mode(const struct winode *i) { (void)i; return APFS_S_IFREG; }
static const void *wi_xf(const struct winode *i, int t) { (void)i; (void)t; return &mock_file_alloced; }
static void wi_dstream(const struct winode *i, uint64_t *s, uint64_t *a) { (void)i; *s = mock_file_alloced; *a = mock_file_alloced; }
static uint64_t wi_sparse(const struct winode *i) { (void)i; return 0; }
static int apfs_readdir(struct apfs_vol *v, uint64_t id, void *cb, void *ctx) { (void)v; (void)id; (void)cb; (void)ctx; return 0; }
static int apfs_xattr_list(struct apfs_vol *v, uint64_t id, apfs_xattr_cb cb, void *ctx) {
    (void)v; (void)id;
    for (uint32_t k = 0; k < mock_nxattrs; k++) {
        int err = cb(ctx, "user.big", 8, &mock_xattrs[k]);
        if (err) return err;
    }
    return 0;
}
struct wcl_item { uint64_t src, made; uint32_t parent; uint16_t namelen; uint8_t dtype; char name[256]; };
struct wcl { struct apfs_wtx *w; struct wcl_item *items; uint32_t n, cap, dir; uint64_t blocks; int error; };
static int wcl_entry(void *ctx, const char *n, size_t l, uint64_t id, uint8_t dt) {
    (void)ctx; (void)n; (void)l; (void)id; (void)dt; return 0;
}
"""


def extract():
    wspace = (APFS_DIR / "apfs_wspace.inc").read_text()
    wops = (APFS_DIR / "apfs_wops.inc").read_text()
    wtx = (APFS_DIR / "apfs_wtx.inc").read_text()
    wclone = (APFS_DIR / "apfs_wclone.inc").read_text()
    vnops_w = (APFS_DIR / "apfs_vnops_write.inc").read_text()

    wsp_fns = wspace[wspace.index("/* Free blocks, less"):wspace.index("\n/* ------------------------------------------------------------------ the internal pool */")]
    wop_defs = wops[wops.index("#define WOP_RESERVE"):wops.index("\n/* After the first change:")]
    wtx_fns = wops[wops.index("bool\napfs_wtx_dirty("):wops.index("\nuint32_t\napfs_wtx_dirty_blocks(")] + "\n" + \
              wops[wops.index("uint64_t\napfs_wtx_free_blocks("):wops.index("\nvoid\napfs_wtx_stats(")]
    discard_fn = wtx[wtx.index("bool\napfs_wtx_discard_solo("):wtx.index("\n/* ------------------------------------------------------------------ commit */")]

    wcl_collect = wclone[wclone.index("static int\nwcl_xattr_count("):wclone.index("\n/* The source's extents")]
    vnop_write = "#define APFS_WSPARE_BLOCKS 16384\n" + \
                 vnops_w[vnops_w.index("static int\napfs_vnop_write("):vnops_w.index("\nstatic int\napfs_vnop_pageout(")]

    return "\n".join([wsp_fns, wop_defs, wtx_fns, discard_fn, wcl_collect, vnop_write])


TESTS = r"""
int main(void) {
    uint8_t bm0[4096] = {0}, bm1[4096] = {0}, ip_bm[4096] = {0};
    uint8_t vsb[4096] = {0}, saved_vsb[4096] = {0};
    struct wchunk chunks[2] = {
        { .addr = 0, .blocks = 32768, .free = 20000 },
        { .addr = 32768, .blocks = 32768, .free = 20000 },
    };
    chunks[0].bitmap = bm0;
    chunks[1].bitmap = bm1;
    bool cib_dirty[1] = { false };
    uint64_t cib_addr[1] = { 1 };

    struct apfs_vol vol = { .alloc_count = 10, .tree = { .root = 100, .root_oid = 200 } };
    struct apfs_nx nx = { .block_size = 4096 };
    struct apfs_wtx w = {
        .nx = &nx, .vol = &vol, .bs = 4096,
        .hash = calloc(WTX_HASH, sizeof(struct wblk *)),
        .vsb = vsb, .saved_vsb = saved_vsb,
        .sp = {
            .per_chunk = 32768, .per_cib = 128, .block_count = 65536,
            .chunk_count = 2, .cib_count = 1, .cab_count = 0,
            .free_count = 40000, .chunks = chunks, .cib_addr = cib_addr,
            .cib_dirty = cib_dirty, .ip_blocks = 8, .ip_bm = ip_bm,
        },
    };
    struct apfs_mount am = { .am_nx = nx, .am_vol = vol, .am_w = &w };

    /* 1. apfs_vnop_write upward block rounding: unaligned write crossing block boundary checks 2 blocks. */
    w.sp.free_count = WOP_RESERVE + 16384 + 1; /* free_blocks = 16385 => 1 block above APFS_WSPARE_BLOCKS */
    struct vnode vp = { .regular = true, .ubc_size = 4096, .node = { .an_mount = &am, .an_id = 7, .an_size = 4096, .an_ino = { .mod_time = 111 } } };
    struct uio u_cross = { .offset = 2048, .resid = 4096 }; /* spans 2 blocks [0, 2) */
    struct vnop_write_args wap = { .a_vp = &vp, .a_uio = &u_cross, .a_ioflag = 0 };
    assert(apfs_vnop_write(&wap) == ENOSPC);
    assert(!w.open && !w.broken && vp.node.an_ino.mod_time == 111 && mock_setattr_calls == 0);

    /* 2. size <= old 0-byte failure leaves w->open=false, mtime=111, wnote=0. */
    w.sp.free_count = 40000;
    struct uio u_in = { .offset = 0, .resid = 100 };
    wap.a_uio = &u_in;
    mock_cluster_err = EFAULT;
    mock_cluster_consume = 0;
    assert(apfs_vnop_write(&wap) == EFAULT);
    assert(!w.open && vp.node.an_ino.mod_time == 111 && mock_setattr_calls == 0 && mock_wnote_calls == 0);

    /* 3. size > old 0-byte failure rolls back transaction cleanly (w->open == false, free_count restored). */
    uint64_t free_before = w.sp.free_count;
    struct uio u_grow = { .offset = 4096, .resid = 512 };
    wap.a_uio = &u_grow;
    mock_cluster_err = EFAULT;
    mock_cluster_consume = 0;
    assert(apfs_vnop_write(&wap) == EFAULT);
    assert(!w.open && !w.broken && w.nblk == 0);
    assert(w.sp.free_count == free_before && !chunks[0].dirty && !cib_dirty[0]);
    assert(vp.node.an_size == 4096 && vp.ubc_size == 4096 && vp.node.an_ino.mod_time == 111 && mock_wnote_calls == 0);

    /* 4. size > old partial write (256 of 512 written, then ENOSPC) shrinks to 4096+256 and updates mtime. */
    mock_cluster_err = ENOSPC;
    mock_cluster_consume = 256;
    assert(apfs_vnop_write(&wap) == ENOSPC);
    assert(vp.node.an_size == 4352 && vp.ubc_size == 4352 && mock_wnote_calls == 1 && !w.broken);
    w.open = false;
    wtx_drop_all(&w);

    /* 5. wcl_collect includes APFS_XATTR_DATA_STREAM blocks, and wop_begin catches ENOSPC without wtx_poison. */
    struct wcl_item items[1] = { { .src = 55 } };
    struct wcl clone_ctx = { .w = &w, .items = items, .n = 1 };
    mock_file_alloced = 4096; /* 1 data block */
    mock_nxattrs = 1;
    mock_xattrs[0] = (struct apfs_xattr){ .flags = APFS_XATTR_DATA_STREAM, .stream_size = 12289 }; /* +4 blocks */
    assert(wcl_collect(&clone_ctx) == 0);
    assert(clone_ctx.blocks == 5);
    w.sp.free_count = WOP_RESERVE + clone_ctx.blocks + 3; /* 1 block short of c.blocks + c.n * 4 */
    assert(wop_begin(&w, clone_ctx.blocks + (uint64_t)clone_ctx.n * 4) == ENOSPC);
    assert(!w.broken);

    /* 6. wsp_ip_alloc internal pool precheck: low ip_bm rejects cleanly with ENOSPC before wtx_poison. */
    w.sp.free_count = 40000;
    /* ip_blocks = 8, mark 6 bits used => 2 free IP blocks; 1 dirty chunk + 1 dirty cib + 2 new chunks + 0 new cib = 4 > 2 */
    for (uint32_t i = 0; i < 6; i++) bit_set(ip_bm, i, true);
    chunks[0].dirty = true;
    cib_dirty[0] = true;
    assert(wsp_ip_free_blocks(&w) == 2);
    assert(wop_begin(&w, 1) == ENOSPC);
    assert(!w.broken);

    free(w.hash);
    return 0;
}
"""


def main():
    prod = extract()
    with tempfile.TemporaryDirectory() as directory:
        c_file = Path(directory) / "apfs_enospc_rollback_test.c"
        binary = c_file.with_suffix("")
        c_file.write_text(STUBS + "\n" + prod + "\n" + TESTS)
        subprocess.run([
            os.environ.get("CC", "cc"),
            "-std=c11", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-Werror",
            str(c_file), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    print("APFS S9 ENOSPC precheck, 0-byte rollback, clone xattr & IP reserve ASan+UBSan checks passed")


if __name__ == "__main__":
    main()

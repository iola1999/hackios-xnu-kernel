/*
 * The writing side of APFS: one open transaction per container, built in memory and
 * committed as a checkpoint.  Internal to apfs_write.c and the .inc files it includes.
 *
 * A transaction collects every change made since the last checkpoint.  A metadata block it
 * changes is first copied to a block no committed checkpoint uses (copy on write), and from
 * then on changed in place in memory; the read path finds these copies through nx->overlay
 * before its cache, so reads see the transaction's state.  The commit writes the copies,
 * the space manager and a new checkpoint, with a cache flush before and after the container
 * superblock that makes it live.  A crash before that block lands leaves the previous
 * checkpoint live and none of the copies reachable.
 *
 * A block freed by the transaction stays allocated until the commit, so nothing the live
 * checkpoint refers to is overwritten; a block both allocated and freed inside one
 * transaction is released at once.  An operation that fails after it has changed anything
 * (a device error) poisons the transaction: it is discarded and the mount stops writing.
 */

#ifndef _MISCFS_APFS_APFS_WTX_H_
#define _MISCFS_APFS_APFS_WTX_H_

#include "apfs_lib.h"

#define WTX_HASH        4096            /* overlay buckets, a power of two */
#define WTX_MAXLEV      APFS_MAX_LEVELS
#define WTX_MAXREC      320             /* records a node may be taken apart into, with room to grow */
#define WTX_KEYMAX      320             /* the longest key: a hashed directory record is 12 + 256 */
#define WTX_MAXGROUPS   8               /* nodes one overfull node is spread over */

/* A metadata block changed by the open transaction: always at an address it allocated. */
struct wblk {
	struct wblk *next;              /* in its hash chain */
	uint64_t     addr;
	uint8_t     *data;              /* block_size bytes */
	bool         raw;               /* a bitmap: no object header to seal */
};

/* One record of a node taken apart: pointers into a copy the caller keeps. */
struct wrec {
	const uint8_t *k, *v;
	uint16_t       kl, vl;
};

/* Scratch for one level of one tree: a node's records and the memory they point into. */
struct wlevel {
	struct wrec r[WTX_MAXREC];
	uint8_t    *copy;               /* 2 * block_size: the node as read, and keys moved up */
	uint8_t     first[WTX_KEYMAX];  /* the node's first key before a change */
};

struct apfs_wtx;

/*
 * A B-tree being changed.  root is the root's block address, which moves when the root is
 * copied; a virtual tree's nodes are found through the volume's object map by identifier.
 */
struct wtree {
	uint16_t        subtype;        /* APFS_OBJECT_TYPE_FSTREE, _OMAP or _BLOCKREFTREE */
	bool            virt;
	bool            fixed;          /* object maps: 16-byte keys, 16-byte leaf values */
	bool            vol_owned;      /* its blocks count in the volume's apfs_fs_alloc_count */
	uint64_t        root;
	uint64_t        root_oid;       /* = root for a physical tree */
	bool            hashed;         /* directory-record keys carry a name hash */
	int           (*cmp)(const struct wtree *, const uint8_t *, uint16_t, const uint8_t *, uint16_t);
	struct wlevel  *lv[WTX_MAXLEV]; /* made when first needed */
};

/* A path from the root (lv[0]) to a leaf: each node's address, id and the entry taken. */
struct wpath {
	int depth;
	struct {
		uint64_t addr;
		uint64_t oid;
		uint32_t idx;
	} at[WTX_MAXLEV];
};

/* One chunk of the container's blocks, as its chunk-info entry says. */
struct wchunk {
	uint64_t  addr;                 /* its first block */
	uint32_t  blocks;
	uint32_t  free;
	uint64_t  xid;
	uint64_t  bitmap_addr;          /* in the internal pool; 0: no bitmap yet, all free */
	uint8_t  *bitmap;               /* NULL until first needed */
	bool      dirty;
};

struct wfreed {
	uint64_t addr;
	uint64_t count;
	bool     vol_owned;
};

/* The space manager: what the checkpoint's spaceman object and its chunk-info blocks say. */
struct wspace {
	uint8_t       *sm;              /* the object, sm_size bytes, rewritten each commit */
	uint32_t       sm_size;
	uint64_t       sm_oid;
	uint32_t       per_chunk, per_cib, per_cab;
	uint64_t       block_count, chunk_count;
	uint32_t       cib_count, cab_count;
	uint64_t       free_count;
	struct wchunk *chunks;
	uint64_t      *cib_addr;        /* cib_count */
	bool          *cib_dirty;
	uint64_t      *cab_addr;        /* cab_count, none when the spaceman holds the cib addresses */
	uint32_t       hint;            /* the chunk the next search begins at */
	/* the internal pool, where bitmaps, chunk-info and cib-address blocks live */
	uint64_t       ip_base, ip_blocks, ip_bm_base;
	uint32_t       ip_bm_size, ip_bm_blocks;
	uint8_t       *ip_bm;           /* ip_bm_size blocks: the current copy of its bitmap */
	uint32_t       ip_hint;
	/* freed in the open transaction, released at commit */
	struct wfreed *freed;
	uint32_t       nfreed, freed_cap;
	uint64_t      *ip_freed;
	uint32_t       nip_freed, ip_freed_cap;
};

struct apfs_wtx {
	struct apfs_nx  *nx;
	struct apfs_vol *vol;
	uint32_t         bs;
	bool             open;          /* something changed since the last checkpoint */
	bool             broken;        /* failed half way; no more writing on this mount */
	uint64_t         xid;           /* the transaction being built: nx->xid + 1 */

	struct wblk    **hash;
	uint32_t         nblk;

	struct wtree     fs, omap, eref, nx_omap;
	uint64_t         vol_omap_obj;  /* the volume's omap_phys block */
	uint64_t         nx_omap_obj;   /* the container's */
	uint64_t         vsb_addr;      /* the volume superblock the last checkpoint maps */
	uint8_t         *vsb;           /* a copy of it, updated in memory and written each commit */
	uint8_t         *nxsb;          /* the live container superblock */
	uint64_t         next_oid;      /* nx_next_oid */

	struct wspace    sp;

	/* what a discarded transaction puts back */
	struct apfs_vol  saved_vol;
	uint64_t         saved_nx_omap_root;
	uint64_t         saved_next_oid;
	uint8_t         *saved_vsb;

	/* counters */
	uint64_t         commits, commit_blocks, commit_ticks, cow_blocks, splits;

	/* the host's converter only (apfs_wadopt.inc); always 0 in the kernel */
	uint64_t         place;         /* the next allocation takes exactly this block on */
	uint64_t         alloc_end;     /* nothing else is allocated at or past this block */
};

/* apfs_wtx.inc */
static const uint8_t *wtx_find(struct apfs_wtx *w, uint64_t addr);
static int  wtx_begin(struct apfs_wtx *w);
static void wtx_poison(struct apfs_wtx *w, int error);
static int  wtx_new_block(struct apfs_wtx *w, bool vol_owned, uint64_t *addr, uint8_t **data);
static int  wtx_writable(struct apfs_wtx *w, uint64_t addr, bool vol_owned, uint64_t *new_addr, uint8_t **data);
static void wtx_release(struct apfs_wtx *w, uint64_t addr, bool vol_owned);
static int  wtx_read(struct apfs_wtx *w, uint64_t addr, const uint8_t **blk);

/* apfs_wspace.inc */
static int  wsp_load(struct apfs_wtx *w);
static void wsp_unload(struct apfs_wtx *w);
static int  wsp_alloc(struct apfs_wtx *w, uint64_t goal, uint64_t want, uint64_t *addr, uint64_t *got);
static int  wsp_free_now(struct apfs_wtx *w, uint64_t addr, uint64_t count);
static int  wsp_free_later(struct apfs_wtx *w, uint64_t addr, uint64_t count, bool vol_owned);
static uint64_t wsp_free_blocks(struct apfs_wtx *w);
static int  wsp_commit(struct apfs_wtx *w);

/* apfs_wbt.inc */
static int  wbt_get(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl,
    uint8_t *val, uint16_t *vl, uint16_t vcap);
static int  wbt_put(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl,
    const uint8_t *v, uint16_t vl);
static int  wbt_del(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl);
static int  wbt_next(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl, bool after,
    uint8_t *okey, uint16_t *okl, uint8_t *oval, uint16_t *ovl, uint16_t vcap);
static int  wbt_floor(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl,
    uint8_t *okey, uint16_t *okl, uint8_t *oval, uint16_t *ovl, uint16_t vcap);
static int  womap_get(struct apfs_wtx *w, struct wtree *t, uint64_t oid, uint64_t *addr);
static int  womap_set(struct apfs_wtx *w, struct wtree *t, uint64_t oid, uint64_t addr);
static int  womap_del(struct apfs_wtx *w, struct wtree *t, uint64_t oid);
static void wbt_free_scratch(struct apfs_wtx *w, struct wtree *t);

#endif /* _MISCFS_APFS_APFS_WTX_H_ */

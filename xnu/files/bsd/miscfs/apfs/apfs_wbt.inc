/*
 * B-trees as a transaction changes them (included by apfs_write.c).
 *
 * A change takes a node apart into its records (wl_load), edits the list and packs it back
 * (wn_pack).  A list that does not fit in one node is spread over as many as it needs and
 * the parent gets an entry for each new one; a root that does not fit hands its records to
 * new children and becomes their index one level up.  A node left empty is removed from its
 * parent, and a root left with a single child takes that child's records back.
 *
 * Every node is filled only to what a root could hold (the block less the 40-byte
 * btree_info), as mkapfs.py lays trees out, so a lone child always fits back into the root.
 * The first key of every non-root node equals its parent's entry for it, which apfsck
 * checks: whenever a node's first key changes, so does the parent's entry, upwards.
 *
 * Records always point into a level's scratch copy or into the caller's memory, never into a
 * live block, so a node can be rewritten while its old records are still being placed.
 */
#pragma once

#define WN_HDR   56
#define WN_INFO  40

struct wnode {
	const uint8_t *b;
	uint16_t       flags, level;
	uint32_t       n;
	uint32_t       toc, kbase, vend;
};

static inline bool
wt_fixed_leaf_vl(const struct wtree *t, bool leaf)
{
	return t->fixed && leaf;
}

static int
wn_parse(struct apfs_wtx *w, const uint8_t *b, struct wnode *n)
{
	uint32_t toff, tlen, esz;

	n->b = b;
	n->flags = w_le16(b + 32);
	n->level = w_le16(b + 34);
	n->n = w_le32(b + 36);
	toff = w_le16(b + 40);
	tlen = w_le16(b + 42);
	n->toc = WN_HDR + toff;
	n->kbase = n->toc + tlen;
	n->vend = w->bs - ((n->flags & APFS_BTNODE_ROOT) ? WN_INFO : 0);
	esz = (n->flags & APFS_BTNODE_FIXED_KV_SIZE) ? 4 : 8;
	if (n->kbase > n->vend || (uint64_t)n->n * esz > tlen || n->level >= WTX_MAXLEV ||
	    ((n->flags & APFS_BTNODE_LEAF) != 0) != (n->level == 0)) {
		return EIO;
	}
	return 0;
}

static int
wn_entry(const struct wnode *n, uint32_t i, const uint8_t **k, uint16_t *kl,
    const uint8_t **v, uint16_t *vl)
{
	const uint8_t *e;
	uint32_t koff, voff, klen, vlen;

	if (i >= n->n) {
		return EIO;
	}
	if (n->flags & APFS_BTNODE_FIXED_KV_SIZE) {
		e = n->b + n->toc + 4 * i;
		koff = w_le16(e);
		voff = w_le16(e + 2);
		klen = 16;
		vlen = (n->flags & APFS_BTNODE_LEAF) ? 16 : 8;
	} else {
		e = n->b + n->toc + 8 * i;
		koff = w_le16(e);
		klen = w_le16(e + 2);
		voff = w_le16(e + 4);
		vlen = w_le16(e + 6);
	}
	if ((uint64_t)n->kbase + koff + klen > n->vend || voff > n->vend - n->kbase ||
	    vlen > voff || klen == 0) {
		return EIO;
	}
	*k = n->b + n->kbase + koff;
	*kl = (uint16_t)klen;
	*v = n->b + n->vend - voff;
	*vl = (uint16_t)vlen;
	return 0;
}

/*
 * The first entry whose key is at or after k: *pos, and whether it is k itself.  In an index
 * node the child to follow is the one before *pos unless *exact (the first one if none is).
 */
static int
wn_search(struct wtree *t, const struct wnode *n, const uint8_t *k, uint16_t kl, uint32_t *pos,
    bool *exact)
{
	uint32_t lo = 0, hi = n->n;

	*exact = false;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const uint8_t *ek, *ev;
		uint16_t ekl, evl;
		int c, error = wn_entry(n, mid, &ek, &ekl, &ev, &evl);

		if (error) {
			return error;
		}
		c = t->cmp(t, ek, ekl, k, kl);
		if (c < 0) {
			lo = mid + 1;
		} else {
			if (c == 0) {
				*exact = true;
			}
			hi = mid;
		}
	}
	*pos = lo;
	return 0;
}

static struct wlevel *
wt_level(struct apfs_wtx *w, struct wtree *t, int lvl)
{
	if (t->lv[lvl] == NULL) {
		struct wlevel *l = apfs_alloc(sizeof(*l));

		if (l == NULL) {
			return NULL;
		}
		l->copy = apfs_alloc(2 * (size_t)w->bs);
		if (l->copy == NULL) {
			apfs_free(l, sizeof(*l));
			return NULL;
		}
		t->lv[lvl] = l;
	}
	return t->lv[lvl];
}

void
wbt_free_scratch(struct apfs_wtx *w, struct wtree *t)
{
	for (int i = 0; i < WTX_MAXLEV; i++) {
		if (t->lv[i] != NULL) {
			apfs_free(t->lv[i]->copy, 2 * (size_t)w->bs);
			apfs_free(t->lv[i], sizeof(*t->lv[i]));
			t->lv[i] = NULL;
		}
	}
}

/* A node's records, into level lvl's scratch: *n of them, and the node's level and flags. */
static int
wl_load(struct apfs_wtx *w, struct wtree *t, int lvl, uint64_t addr, uint32_t *count,
    uint16_t *level)
{
	struct wlevel *l = wt_level(w, t, lvl);
	const uint8_t *blk;
	struct wnode n;
	int error;

	if (l == NULL) {
		return ENOMEM;
	}
	error = wtx_read(w, addr, &blk);
	if (error) {
		return error;
	}
	memcpy(l->copy, blk, w->bs);
	error = wn_parse(w, l->copy, &n);
	if (error) {
		return error;
	}
	if (n.n > WTX_MAXREC - 8) {
		return EIO;
	}
	for (uint32_t i = 0; i < n.n; i++) {
		error = wn_entry(&n, i, &l->r[i].k, &l->r[i].kl, &l->r[i].v, &l->r[i].vl);
		if (error) {
			return error;
		}
	}
	*count = n.n;
	*level = n.level;
	return 0;
}

/* Room in a level's scratch for bytes a changed record needs (a key moved up, a child id). */
static uint8_t *
wl_room(struct apfs_wtx *w, struct wtree *t, int lvl, uint32_t *used, uint32_t len)
{
	struct wlevel *l = t->lv[lvl];

	if (l == NULL || *used + len > w->bs) {
		return NULL;
	}
	uint8_t *p = l->copy + w->bs + *used;
	*used += len;
	return p;
}

static uint32_t
wn_toc_len(struct apfs_wtx *w, struct wtree *t, uint32_t n, bool leaf)
{
	if (t->fixed) {
		return ((w->bs - WN_HDR) / (16 + (leaf ? 16 : 8) + 4)) * 4;
	}
	return ((n < 8 ? 8 : n) + 7) / 8 * 8 * 8;
}

/* Would records [from, from + n) fit in one node (as much as a root holds)? */
static bool
wn_fits(struct apfs_wtx *w, struct wtree *t, const struct wrec *r, uint32_t n, bool leaf)
{
	uint64_t used = WN_HDR + wn_toc_len(w, t, n, leaf);

	if (t->fixed && n > wn_toc_len(w, t, n, leaf) / 4) {
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		used += r[i].kl + r[i].vl;
	}
	return used <= w->bs - WN_INFO;
}

static void
wn_header(struct apfs_wtx *w, struct wtree *t, uint8_t *blk, uint64_t addr, uint64_t oid, bool root)
{
	uint32_t type = (root ? APFS_OBJECT_TYPE_BTREE : APFS_OBJECT_TYPE_BTREE_NODE) |
	    (t->virt ? APFS_OBJ_VIRTUAL : APFS_OBJ_PHYSICAL);

	w_put_le64(blk + 8, t->virt ? oid : addr);
	w_put_le64(blk + 16, w->xid);
	w_put_le32(blk + 24, type);
	w_put_le32(blk + 28, t->subtype);
}

/*
 * Pack records into the node at blk.  A root keeps the btree_info already at its end.  The
 * caller has checked that they fit.
 */
static void
wn_pack(struct apfs_wtx *w, struct wtree *t, uint8_t *blk, uint64_t addr, uint64_t oid, bool root,
    uint16_t level, const struct wrec *r, uint32_t n)
{
	bool leaf = level == 0;
	uint32_t toc_len = wn_toc_len(w, t, n, leaf);
	uint32_t vend = w->bs - (root ? WN_INFO : 0);
	uint32_t kbase = WN_HDR + toc_len, koff = 0, voff = 0;
	uint8_t info[WN_INFO];

	if (root) {
		memcpy(info, blk + w->bs - WN_INFO, WN_INFO);
	}
	memset(blk, 0, w->bs);
	for (uint32_t i = 0; i < n; i++) {
		memcpy(blk + kbase + koff, r[i].k, r[i].kl);
		voff += r[i].vl;
		memcpy(blk + vend - voff, r[i].v, r[i].vl);
		if (t->fixed) {
			w_put_le16(blk + WN_HDR + 4 * i, (uint16_t)koff);
			w_put_le16(blk + WN_HDR + 4 * i + 2, (uint16_t)voff);
		} else {
			w_put_le16(blk + WN_HDR + 8 * i, (uint16_t)koff);
			w_put_le16(blk + WN_HDR + 8 * i + 2, r[i].kl);
			w_put_le16(blk + WN_HDR + 8 * i + 4, (uint16_t)voff);
			w_put_le16(blk + WN_HDR + 8 * i + 6, r[i].vl);
		}
		koff += r[i].kl;
	}
	w_put_le16(blk + 32, (uint16_t)((root ? APFS_BTNODE_ROOT : 0) | (leaf ? APFS_BTNODE_LEAF : 0) |
	    (t->fixed ? APFS_BTNODE_FIXED_KV_SIZE : 0)));
	w_put_le16(blk + 34, level);
	w_put_le32(blk + 36, n);
	w_put_le16(blk + 40, 0);
	w_put_le16(blk + 42, (uint16_t)toc_len);
	w_put_le16(blk + 44, (uint16_t)koff);
	w_put_le16(blk + 46, (uint16_t)(vend - voff - (kbase + koff)));
	w_put_le16(blk + 48, APFS_BTOFF_INVALID);
	w_put_le16(blk + 50, 0);
	w_put_le16(blk + 52, APFS_BTOFF_INVALID);
	w_put_le16(blk + 54, 0);
	if (root) {
		memcpy(blk + w->bs - WN_INFO, info, WN_INFO);
	}
	wn_header(w, t, blk, addr, oid, root);
}

/* Where a child entry's value says the child is: through the object map for a virtual tree. */
static int
wt_child(struct apfs_wtx *w, struct wtree *t, const uint8_t *v, uint16_t vl, uint64_t *oid,
    uint64_t *addr)
{
	if (vl < 8) {
		return EIO;
	}
	*oid = w_le64(v);
	if (!t->virt) {
		*addr = *oid;
		return 0;
	}
	return womap_get(w, &w->omap, *oid, addr);
}

/* From the root to the leaf where k is or would go. */
static int
wt_walk(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl, struct wpath *p,
    bool *exact)
{
	uint64_t addr = t->root, oid = t->root_oid;

	for (int d = 0; d < WTX_MAXLEV; d++) {
		const uint8_t *blk, *ek, *ev;
		uint16_t ekl, evl;
		struct wnode n;
		uint32_t pos;
		int error = wtx_read(w, addr, &blk);

		if (error) {
			return error;
		}
		error = wn_parse(w, blk, &n);
		if (error) {
			return error;
		}
		if ((d == 0) != ((n.flags & APFS_BTNODE_ROOT) != 0)) {
			return EIO;
		}
		error = wn_search(t, &n, k, kl, &pos, exact);
		if (error) {
			return error;
		}
		p->at[d].addr = addr;
		p->at[d].oid = oid;
		if (n.level == 0) {
			p->at[d].idx = pos;
			p->depth = d + 1;
			return 0;
		}
		if (n.n == 0) {
			return EIO;
		}
		if (!*exact && pos > 0) {
			pos--;
		}
		p->at[d].idx = pos;
		error = wn_entry(&n, pos, &ek, &ekl, &ev, &evl);
		if (error) {
			return error;
		}
		error = wt_child(w, t, ev, evl, &oid, &addr);
		if (error) {
			return error;
		}
	}
	return EIO;
}

/*
 * Make every node on the path the transaction's own, from the root down.  A physical child
 * that moves gets its new address written into its parent's entry; a virtual one gets a new
 * object-map entry instead.
 */
static int
wt_own_path(struct apfs_wtx *w, struct wtree *t, struct wpath *p)
{
	for (int d = 0; d < p->depth; d++) {
		uint64_t old = p->at[d].addr, now;
		uint8_t *data;
		int error = wtx_writable(w, old, t->vol_owned, &now, &data);

		if (error) {
			return error;
		}
		if (now == old) {
			continue;
		}
		w->cow_blocks++;
		wn_header(w, t, data, now, p->at[d].oid, d == 0);
		p->at[d].addr = now;
		if (!t->virt) {
			p->at[d].oid = now;
		}
		if (d == 0) {
			t->root = now;
			if (!t->virt) {
				t->root_oid = now;
			}
		}
		if (t->virt) {
			error = womap_set(w, &w->omap, p->at[d].oid, now);
		} else if (d > 0) {
			const uint8_t *k, *v;
			uint16_t kl, vl;
			struct wnode n;
			uint8_t *parent;

			error = wtx_writable(w, p->at[d - 1].addr, t->vol_owned, &old, &parent);
			if (error == 0) {
				error = wn_parse(w, parent, &n);
			}
			if (error == 0) {
				error = wn_entry(&n, p->at[d - 1].idx, &k, &kl, &v, &vl);
			}
			if (error == 0) {
				w_put_le64((uint8_t *)(uintptr_t)v, now);
			}
		}
		if (error) {
			return error;
		}
	}
	return 0;
}

/* The tree's footer, by how many keys and nodes the change added (or took away). */
static int
wt_footer(struct apfs_wtx *w, struct wtree *t, int64_t dkeys, int64_t dnodes, uint16_t kl, uint16_t vl)
{
	uint64_t addr;
	uint8_t *root, *info;
	int error = wtx_writable(w, t->root, t->vol_owned, &addr, &root);

	if (error) {
		return error;
	}
	if (addr != t->root) {
		return EIO;                     /* the root is owned before any change */
	}
	info = root + w->bs - WN_INFO;
	w_put_le64(info + 24, w_le64(info + 24) + (uint64_t)dkeys);
	w_put_le64(info + 32, w_le64(info + 32) + (uint64_t)dnodes);
	if (!t->fixed) {
		if (kl > w_le32(info + 16)) {
			w_put_le32(info + 16, kl);
		}
		if (vl > w_le32(info + 20)) {
			w_put_le32(info + 20, vl);
		}
	}
	return 0;
}

/* A new node for the tree: its address and (virtual) object id, mapped. */
static int
wt_new_node(struct apfs_wtx *w, struct wtree *t, uint64_t *addr, uint64_t *oid, uint8_t **data)
{
	int error = wtx_new_block(w, t->vol_owned, addr, data);

	if (error) {
		return error;
	}
	if (t->virt) {
		*oid = w->next_oid++;
		return womap_set(w, &w->omap, *oid, *addr);
	}
	*oid = *addr;
	return 0;
}

/* A node no longer in the tree. */
static int
wt_drop_node(struct apfs_wtx *w, struct wtree *t, uint64_t addr, uint64_t oid)
{
	wtx_release(w, addr, t->vol_owned);
	if (t->virt) {
		return womap_del(w, &w->omap, oid);
	}
	return 0;
}

static int wt_store(struct apfs_wtx *w, struct wtree *t, struct wpath *p, int d,
    struct wrec *r, uint32_t n, int64_t *dnodes);

/*
 * The parent of the node at depth d, with entry idx's key set to key (the node's new first
 * key) and the entries in add[] for new siblings inserted after it -- or entry idx taken out
 * when the node is gone (gone).
 */
static int
wt_fix_parent(struct apfs_wtx *w, struct wtree *t, struct wpath *p, int d, const struct wrec *key,
    const struct wrec *add, uint32_t nadd, bool gone, int64_t *dnodes)
{
	struct wlevel *l;
	uint32_t n, idx = p->at[d - 1].idx, used = 0;
	uint16_t level;
	int error = wl_load(w, t, d - 1, p->at[d - 1].addr, &n, &level);

	if (error) {
		return error;
	}
	l = t->lv[d - 1];
	if (idx >= n || n + nadd > WTX_MAXREC) {
		return EIO;
	}
	if (gone) {
		memmove(&l->r[idx], &l->r[idx + 1], (n - idx - 1) * sizeof(l->r[0]));
		n--;
	} else {
		if (key != NULL) {
			uint8_t *kc = wl_room(w, t, d - 1, &used, key->kl);

			if (kc == NULL) {
				return EIO;
			}
			memcpy(kc, key->k, key->kl);
			l->r[idx].k = kc;
			l->r[idx].kl = key->kl;
		}
		memmove(&l->r[idx + 1 + nadd], &l->r[idx + 1], (n - idx - 1) * sizeof(l->r[0]));
		for (uint32_t i = 0; i < nadd; i++) {
			uint8_t *kc = wl_room(w, t, d - 1, &used, add[i].kl + 8);

			if (kc == NULL) {
				return EIO;
			}
			memcpy(kc, add[i].k, add[i].kl);
			memcpy(kc + add[i].kl, add[i].v, 8);
			l->r[idx + 1 + i].k = kc;
			l->r[idx + 1 + i].kl = add[i].kl;
			l->r[idx + 1 + i].v = kc + add[i].kl;
			l->r[idx + 1 + i].vl = 8;
		}
		n += nadd;
	}
	return wt_store(w, t, p, d - 1, l->r, n, dnodes);
}

/* Records for the node at depth d of the path (owned already): packed, split or removed. */
static int
wt_store(struct apfs_wtx *w, struct wtree *t, struct wpath *p, int d, struct wrec *r, uint32_t n,
    int64_t *dnodes)
{
	const uint8_t *blk, *ok = NULL, *ov;
	uint16_t okl = 0, ovl, level;
	struct wnode cur;
	uint8_t *data, *oldfirst = t->lv[d]->first;
	uint64_t addr = p->at[d].addr;
	bool root = d == 0, leaf, first_moved;
	int error = wtx_read(w, addr, &blk);

	if (error) {
		return error;
	}
	error = wn_parse(w, blk, &cur);
	if (error) {
		return error;
	}
	level = cur.level;
	leaf = level == 0;
	if (cur.n > 0) {
		error = wn_entry(&cur, 0, &ok, &okl, &ov, &ovl);
		if (error || okl > WTX_KEYMAX) {
			return error ? error : EIO;
		}
		memcpy(oldfirst, ok, okl);
		ok = oldfirst;
	}
	error = wtx_writable(w, addr, t->vol_owned, &addr, &data);
	if (error) {
		return error;
	}
	if (addr != p->at[d].addr) {
		return EIO;
	}

	if (n == 0 && !root) {
		error = wt_drop_node(w, t, addr, p->at[d].oid);
		if (error) {
			return error;
		}
		(*dnodes)--;
		return wt_fix_parent(w, t, p, d, NULL, NULL, 0, true, dnodes);
	}
	if (n == 0) {
		wn_pack(w, t, data, addr, p->at[d].oid, true, 0, r, 0);   /* an empty tree is one leaf */
		return 0;
	}

	if (wn_fits(w, t, r, n, leaf)) {
		if (root && level > 0 && n == 1) {
			/* a root with one child: the child's records come up into the root */
			uint64_t coid, caddr;
			uint32_t cn;
			uint16_t clevel;

			error = wt_child(w, t, r[0].v, r[0].vl, &coid, &caddr);
			if (error == 0) {
				error = wl_load(w, t, d + 1 < WTX_MAXLEV ? d + 1 : d, caddr, &cn, &clevel);
			}
			if (error) {
				return error;
			}
			struct wlevel *cl = t->lv[d + 1 < WTX_MAXLEV ? d + 1 : d];
			if (wn_fits(w, t, cl->r, cn, clevel == 0)) {
				error = wt_drop_node(w, t, caddr, coid);
				if (error) {
					return error;
				}
				(*dnodes)--;
				wn_pack(w, t, data, addr, p->at[d].oid, true, clevel, cl->r, cn);
				return 0;
			}
		}
		wn_pack(w, t, data, addr, p->at[d].oid, root, level, r, n);
		first_moved = !root && (okl != r[0].kl || memcmp(ok, r[0].k, okl) != 0);
		if (first_moved) {
			return wt_fix_parent(w, t, p, d, &r[0], NULL, 0, false, dnodes);
		}
		return 0;
	}

	/* Too many: groups of what fits, front to back. */
	uint32_t start[WTX_MAXGROUPS + 1], groups = 0, from = 0;

	while (from < n) {
		if (groups == WTX_MAXGROUPS) {
			return EFBIG;
		}
		uint32_t take = 1;

		while (from + take < n && wn_fits(w, t, r + from, take + 1, leaf)) {
			take++;
		}
		if (!wn_fits(w, t, r + from, take, leaf)) {
			return EFBIG;                   /* one record larger than a node */
		}
		start[groups++] = from;
		from += take;
	}
	start[groups] = n;
	w->splits++;

	struct wrec add[WTX_MAXGROUPS];
	uint8_t refs[WTX_MAXGROUPS][8];
	uint32_t first_new = root ? 0 : 1;

	for (uint32_t g = first_new; g < groups; g++) {
		uint64_t naddr, noid;
		uint8_t *ndata;

		error = wt_new_node(w, t, &naddr, &noid, &ndata);
		if (error) {
			return error;
		}
		(*dnodes)++;
		wn_pack(w, t, ndata, naddr, noid, false, level, r + start[g], start[g + 1] - start[g]);
		w_put_le64(refs[g], noid);
		add[g].k = r[start[g]].k;
		add[g].kl = r[start[g]].kl;
		add[g].v = refs[g];
		add[g].vl = 8;
	}
	if (root) {
		/* the root becomes an index over the new nodes, one level up */
		if (!wn_fits(w, t, add, groups, false) || level + 1 >= WTX_MAXLEV) {
			return EFBIG;
		}
		wn_pack(w, t, data, addr, p->at[d].oid, true, level + 1, add, groups);
		return 0;
	}
	wn_pack(w, t, data, addr, p->at[d].oid, false, level, r, start[1]);
	first_moved = okl != r[0].kl || memcmp(ok, r[0].k, okl) != 0;
	return wt_fix_parent(w, t, p, d, first_moved ? &r[0] : NULL, add + 1, groups - 1, false, dnodes);
}

/* ------------------------------------------------------------------ the operations */

/* The read path's view of a tree whose root may have moved. */
static void
wt_sync(struct apfs_wtx *w, struct wtree *t)
{
	if (t == &w->fs) {
		w->vol->tree.root = t->root;
	} else if (t == &w->omap) {
		w->vol->omap.tree.root = t->root;
	} else if (t == &w->eref) {
		w->vol->extentref_oid = t->root;
	} else if (t == &w->nx_omap) {
		w->nx->omap.tree.root = t->root;
	}
}

int
wbt_get(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl, uint8_t *val,
    uint16_t *vl, uint16_t vcap)
{
	struct wpath p;
	struct wnode n;
	const uint8_t *blk, *ek, *ev;
	uint16_t ekl, evl;
	bool exact;
	int error = wt_walk(w, t, k, kl, &p, &exact);

	if (error) {
		return error;
	}
	if (!exact) {
		return ENOENT;
	}
	error = wtx_read(w, p.at[p.depth - 1].addr, &blk);
	if (error == 0) {
		error = wn_parse(w, blk, &n);
	}
	if (error == 0) {
		error = wn_entry(&n, p.at[p.depth - 1].idx, &ek, &ekl, &ev, &evl);
	}
	if (error) {
		return error;
	}
	if (val != NULL && evl > vcap) {
		return E2BIG;
	}
	if (val != NULL) {
		memcpy(val, ev, evl);
	}
	if (vl != NULL) {
		*vl = evl;
	}
	return 0;
}

/* Insert a record, or replace the value (and key bytes) of the one with an equal key. */
int
wbt_put(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl, const uint8_t *v,
    uint16_t vl)
{
	struct wpath p;
	struct wlevel *l;
	uint32_t n, pos;
	uint16_t level;
	int64_t dnodes = 0;
	bool exact;
	int error;

	if (kl > WTX_KEYMAX || (t->fixed && (kl != 16 || vl != 16))) {
		return EINVAL;
	}
	error = wt_walk(w, t, k, kl, &p, &exact);
	if (error == 0) {
		error = wt_own_path(w, t, &p);
	}
	if (error == 0) {
		error = wl_load(w, t, p.depth - 1, p.at[p.depth - 1].addr, &n, &level);
	}
	if (error) {
		return error;
	}
	l = t->lv[p.depth - 1];
	pos = p.at[p.depth - 1].idx;
	if (!exact) {
		if (n + 1 > WTX_MAXREC) {
			return EIO;
		}
		memmove(&l->r[pos + 1], &l->r[pos], (n - pos) * sizeof(l->r[0]));
		n++;
	}
	l->r[pos].k = k;
	l->r[pos].kl = kl;
	l->r[pos].v = v;
	l->r[pos].vl = vl;
	error = wt_store(w, t, &p, p.depth - 1, l->r, n, &dnodes);
	if (error == 0) {
		error = wt_footer(w, t, exact ? 0 : 1, dnodes, kl, vl);
	}
	wt_sync(w, t);
	return error;
}

int
wbt_del(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl)
{
	struct wpath p;
	struct wlevel *l;
	uint32_t n, pos;
	uint16_t level;
	int64_t dnodes = 0;
	bool exact;
	int error = wt_walk(w, t, k, kl, &p, &exact);

	if (error) {
		return error;
	}
	if (!exact) {
		return ENOENT;
	}
	error = wt_own_path(w, t, &p);
	if (error == 0) {
		error = wl_load(w, t, p.depth - 1, p.at[p.depth - 1].addr, &n, &level);
	}
	if (error) {
		return error;
	}
	l = t->lv[p.depth - 1];
	pos = p.at[p.depth - 1].idx;
	memmove(&l->r[pos], &l->r[pos + 1], (n - pos - 1) * sizeof(l->r[0]));
	n--;
	error = wt_store(w, t, &p, p.depth - 1, l->r, n, &dnodes);
	if (error == 0) {
		error = wt_footer(w, t, -1, dnodes, 0, 0);
	}
	wt_sync(w, t);
	return error;
}

/*
 * The first record at k or after it (after: strictly after), copied out.  ENOENT at the end
 * of the tree; EOVERFLOW when its value is longer than vcap (the key is copied all the same).
 */
int
wbt_next(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl, bool after,
    uint8_t *okey, uint16_t *okl, uint8_t *oval, uint16_t *ovl, uint16_t vcap)
{
	struct wpath p;
	bool exact;
	int error = wt_walk(w, t, k, kl, &p, &exact);

	if (error) {
		return error;
	}
	if (exact && after) {
		p.at[p.depth - 1].idx++;
	}
	for (;;) {
		const uint8_t *blk, *ek, *ev;
		uint16_t ekl, evl;
		struct wnode n;
		int d = p.depth - 1;

		error = wtx_read(w, p.at[d].addr, &blk);
		if (error == 0) {
			error = wn_parse(w, blk, &n);
		}
		if (error) {
			return error;
		}
		if (p.at[d].idx < n.n) {
			error = wn_entry(&n, p.at[d].idx, &ek, &ekl, &ev, &evl);
			if (error) {
				return error;
			}
			if (ekl > WTX_KEYMAX) {
				return EIO;
			}
			memcpy(okey, ek, ekl);
			*okl = ekl;
			if (oval != NULL && evl > vcap) {
				return EOVERFLOW;       /* the key is there for the caller to look at */
			}
			if (oval != NULL) {
				memcpy(oval, ev, evl);
			}
			if (ovl != NULL) {
				*ovl = evl;
			}
			return 0;
		}
		/* past this leaf: up to the first ancestor with an entry to the right, then down its left edge */
		while (d > 0) {
			d--;
			error = wtx_read(w, p.at[d].addr, &blk);
			if (error == 0) {
				error = wn_parse(w, blk, &n);
			}
			if (error) {
				return error;
			}
			if (p.at[d].idx + 1 < n.n) {
				break;
			}
		}
		if (d == 0 && p.at[0].idx + 1 >= n.n) {
			return ENOENT;
		}
		p.at[d].idx++;
		while (d < p.depth - 1) {
			uint64_t coid, caddr;

			error = wtx_read(w, p.at[d].addr, &blk);
			if (error == 0) {
				error = wn_parse(w, blk, &n);
			}
			if (error == 0) {
				error = wn_entry(&n, p.at[d].idx, &ek, &ekl, &ev, &evl);
			}
			if (error == 0) {
				error = wt_child(w, t, ev, evl, &coid, &caddr);
			}
			if (error) {
				return error;
			}
			d++;
			p.at[d].addr = caddr;
			p.at[d].oid = coid;
			p.at[d].idx = 0;
		}
	}
}

/*
 * The last record at k or before it, copied out.  The walk takes the child whose first key
 * is at or before k, so the record is in the leaf it ends at, if anywhere.
 */
int
wbt_floor(struct apfs_wtx *w, struct wtree *t, const uint8_t *k, uint16_t kl, uint8_t *okey,
    uint16_t *okl, uint8_t *oval, uint16_t *ovl, uint16_t vcap)
{
	struct wpath p;
	struct wnode n;
	const uint8_t *blk, *ek, *ev;
	uint16_t ekl, evl;
	uint32_t idx;
	bool exact;
	int error = wt_walk(w, t, k, kl, &p, &exact);

	if (error) {
		return error;
	}
	idx = p.at[p.depth - 1].idx;
	if (!exact) {
		if (idx == 0) {
			return ENOENT;
		}
		idx--;
	}
	error = wtx_read(w, p.at[p.depth - 1].addr, &blk);
	if (error == 0) {
		error = wn_parse(w, blk, &n);
	}
	if (error == 0) {
		error = wn_entry(&n, idx, &ek, &ekl, &ev, &evl);
	}
	if (error) {
		return error;
	}
	if (ekl > WTX_KEYMAX) {
		return EIO;
	}
	memcpy(okey, ek, ekl);
	*okl = ekl;
	if (oval != NULL && evl > vcap) {
		return EOVERFLOW;               /* the key is there for the caller to look at */
	}
	if (oval != NULL) {
		memcpy(oval, ev, evl);
	}
	if (ovl != NULL) {
		*ovl = evl;
	}
	return 0;
}

/* ------------------------------------------------------------------ object maps */

static int
womap_cmp(const struct wtree *t, const uint8_t *a, uint16_t al, const uint8_t *b, uint16_t bl)
{
	uint64_t ao = w_le64(a), bo = w_le64(b), ax = w_le64(a + 8), bx = w_le64(b + 8);

	(void)t;
	(void)al;
	(void)bl;
	if (ao != bo) {
		return ao < bo ? -1 : 1;
	}
	if (ax != bx) {
		return ax < bx ? -1 : 1;
	}
	return 0;
}

/*
 * The one entry of oid (this port keeps a single version of every object: no snapshots).
 * Found as the last entry before (oid, all ones).
 */
static int
womap_find(struct apfs_wtx *w, struct wtree *t, uint64_t oid, struct wpath *p, uint8_t *key,
    uint8_t *val)
{
	uint8_t probe[16];
	const uint8_t *blk, *ek, *ev;
	uint16_t ekl, evl;
	struct wnode n;
	bool exact;
	int error;

	w_put_le64(probe, oid);
	w_put_le64(probe + 8, UINT64_MAX);
	error = wt_walk(w, t, probe, 16, p, &exact);
	if (error) {
		return error;
	}
	if (p->at[p->depth - 1].idx == 0) {
		return ENOENT;
	}
	p->at[p->depth - 1].idx--;
	error = wtx_read(w, p->at[p->depth - 1].addr, &blk);
	if (error == 0) {
		error = wn_parse(w, blk, &n);
	}
	if (error == 0) {
		error = wn_entry(&n, p->at[p->depth - 1].idx, &ek, &ekl, &ev, &evl);
	}
	if (error) {
		return error;
	}
	if (w_le64(ek) != oid) {
		return ENOENT;
	}
	memcpy(key, ek, 16);
	memcpy(val, ev, 16);
	return 0;
}

int
womap_get(struct apfs_wtx *w, struct wtree *t, uint64_t oid, uint64_t *addr)
{
	struct wpath p;
	uint8_t key[16], val[16];
	int error = womap_find(w, t, oid, &p, key, val);

	if (error) {
		return error;
	}
	*addr = w_le64(val + 8);
	return (*addr == 0 || *addr >= w->nx->block_count) ? EIO : 0;
}

int
womap_set(struct apfs_wtx *w, struct wtree *t, uint64_t oid, uint64_t addr)
{
	struct wpath p;
	uint8_t key[16], val[16], okey[16];
	int error = womap_find(w, t, oid, &p, okey, val);

	if (error != 0 && error != ENOENT) {
		return error;
	}
	if (error == 0 && w_le64(okey + 8) != w->xid) {
		error = wbt_del(w, t, okey, 16);       /* the old version's key: (oid, its xid) */
		if (error) {
			return error;
		}
	}
	w_put_le64(key, oid);
	w_put_le64(key + 8, w->xid);
	w_put_le32(val, 0);
	w_put_le32(val + 4, w->bs);
	w_put_le64(val + 8, addr);
	error = wbt_put(w, t, key, 16, val, 16);
	if (t == &w->omap) {
		apfs_omap_cache_forget(&w->vol->omap, oid);
	}
	return error;
}

int
womap_del(struct apfs_wtx *w, struct wtree *t, uint64_t oid)
{
	struct wpath p;
	uint8_t okey[16], val[16];
	int error = womap_find(w, t, oid, &p, okey, val);

	if (error) {
		return error;
	}
	error = wbt_del(w, t, okey, 16);
	if (t == &w->omap) {
		apfs_omap_cache_forget(&w->vol->omap, oid);
	}
	return error;
}

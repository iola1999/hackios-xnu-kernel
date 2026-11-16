/*
 * File-system records as a transaction changes them (included by apfs_write.c): the key
 * order, inodes and their extended fields, directory entries and the sibling records of hard
 * links, extended attributes, and the extents of data streams.
 *
 * What apfsck holds a volume to, and this code keeps: every inode has a NAME xfield naming
 * its primary link (the only one, or of several the one with the lowest sibling id) and the
 * parent_id of that link; exactly one of HAS_RSRC_FORK / NO_RSRC_FORK; the FINDER_INFO and
 * SECURITY flags exactly when those attributes exist; a stream's extents run from offset 0
 * without a gap (a hole is an extent with physical block 0) and add up to alloced_size,
 * the holes to the SPARSE_BYTES xfield, present with IS_SPARSE exactly when there are any.
 */
#pragma once

#define WXF_MAX          12
#define WXF_DATA_MAX     264             /* a NAME xfield: 255 bytes and the NUL, padded */
#define WVAL_MAX         4096            /* an embedded attribute is the longest value */

#define WXF_SYSTEM       0x20
#define WKIND_NEW        1ULL
#define WDOC_ID_MIN      3

#define WINO_PARENT      0
#define WINO_PRIVATE     8
#define WINO_CRTIME      16
#define WINO_MTIME       24
#define WINO_CTIME       32
#define WINO_ATIME       40
#define WINO_IFLAGS      48
#define WINO_NLINK       56
#define WINO_PCLASS      60
#define WINO_WGEN        64
#define WINO_BSDFLAGS    68
#define WINO_UID         72
#define WINO_GID         76
#define WINO_MODE        80
#define WINO_FIXED       92

#define VSB_NUM_OTHER    208
#define VSB_ALLOCED      224
#define VSB_FREED        232
#define VSB_NEXT_DOC_ID  960

#define WXATTR_RSRC      "com.apple.ResourceFork"
#define WXATTR_SECURITY  "com.apple.system.Security"
#define WXATTR_FINDER    "com.apple.FinderInfo"
#define WXATTR_SYMLINK   "com.apple.fs.symlink"

/* ------------------------------------------------------------------ key order */

static uint64_t wk_id(const uint8_t *k) { return w_le64(k) & APFS_OBJ_ID_MASK; }
static uint8_t wk_type(const uint8_t *k) { return (uint8_t)(w_le64(k) >> APFS_OBJ_TYPE_SHIFT); }

/* NUL-terminated names stored in keys, compared as apfsck does: strcmp */
static int
wk_strcmp(const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
	size_t i;

	for (i = 0; i < al && i < bl; i++) {
		if (a[i] != b[i]) {
			return a[i] < b[i] ? -1 : 1;
		}
		if (a[i] == 0) {
			return 0;
		}
	}
	return al == bl ? 0 : (al < bl ? -1 : 1);
}

/*
 * (object id, record type), then per type: a directory record by the hash part of its
 * name word and then the name; an extent or a sibling link by its 64-bit number; an
 * attribute by name.  The same order as apfsck's keycmp().
 */
static int
wfs_cmp(const struct wtree *t, const uint8_t *a, uint16_t al, const uint8_t *b, uint16_t bl)
{
	uint64_t ia = wk_id(a), ib = wk_id(b);
	uint8_t ta = wk_type(a), tb = wk_type(b);

	if (ia != ib) {
		return ia < ib ? -1 : 1;
	}
	if (ta != tb) {
		return ta < tb ? -1 : 1;
	}
	switch (ta) {
	case APFS_TYPE_DIR_REC:
		if (t->hashed && al >= 12 && bl >= 12) {
			uint32_t ha = w_le32(a + 8) & ~0x3ffu, hb = w_le32(b + 8) & ~0x3ffu;

			if (ha != hb) {
				return ha < hb ? -1 : 1;
			}
			return wk_strcmp(a + 12, al - 12u, b + 12, bl - 12u);
		}
		if (al >= 10 && bl >= 10) {
			return wk_strcmp(a + 10, al - 10u, b + 10, bl - 10u);
		}
		return 0;
	case APFS_TYPE_FILE_EXTENT:
	case APFS_TYPE_SIBLING_LINK:
		if (al >= 16 && bl >= 16) {
			uint64_t na = w_le64(a + 8), nb = w_le64(b + 8);

			if (na != nb) {
				return na < nb ? -1 : 1;
			}
		}
		return 0;
	case APFS_TYPE_XATTR:
	case APFS_TYPE_SNAP_NAME:
		if (al >= 10 && bl >= 10) {
			return wk_strcmp(a + 10, al - 10u, b + 10, bl - 10u);
		}
		return 0;
	default:
		return 0;
	}
}

static uint16_t
wk_simple(uint8_t *k, uint64_t id, uint8_t type)
{
	w_make_jkey(k, id, type);
	return 8;
}

static uint16_t
wk_num(uint8_t *k, uint64_t id, uint8_t type, uint64_t num)
{
	w_make_jkey(k, id, type);
	w_put_le64(k + 8, num);
	return 16;
}

static uint16_t
wk_xattr(uint8_t *k, uint64_t id, const char *name, size_t len)
{
	w_make_jkey(k, id, APFS_TYPE_XATTR);
	w_put_le16(k + 8, (uint16_t)(len + 1));
	memcpy(k + 10, name, len);
	k[10 + len] = 0;
	return (uint16_t)(10 + len + 1);
}

/* A directory record's key; EILSEQ for a name the volume cannot hash. */
static int
wk_drec(struct apfs_wtx *w, uint8_t *k, uint16_t *kl, uint64_t parent, const char *name, size_t len)
{
	w_make_jkey(k, parent, APFS_TYPE_DIR_REC);
	if (w->fs.hashed) {
		uint32_t hash;
		bool fold = (w->vol->incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;

		if (!apfs_name_hash(name, len, fold, &hash)) {
			return EILSEQ;
		}
		w_put_le32(k + 8, (hash << APFS_J_DREC_HASH_SHIFT) | (uint32_t)(len + 1));
		memcpy(k + 12, name, len);
		k[12 + len] = 0;
		*kl = (uint16_t)(12 + len + 1);
	} else {
		w_put_le16(k + 8, (uint16_t)(len + 1));
		memcpy(k + 10, name, len);
		k[10 + len] = 0;
		*kl = (uint16_t)(10 + len + 1);
	}
	return 0;
}

static int
wname_ok(const char *name, size_t len)
{
	if (len == 0 || (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) {
		return EINVAL;
	}
	if (len > 255) {
		return ENAMETOOLONG;
	}
	for (size_t i = 0; i < len; i++) {
		if (name[i] == '/' || name[i] == 0) {
			return EINVAL;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ inodes */

struct wxf {
	uint8_t  type, flags;
	uint16_t len;
	uint8_t  data[WXF_DATA_MAX];
};

struct winode {
	uint64_t   id;
	uint8_t    f[WINO_FIXED];
	uint32_t   nxf;
	struct wxf xf[WXF_MAX];
};

static inline uint16_t wi_mode(const struct winode *i) { return w_le16(i->f + WINO_MODE); }
static inline bool wi_isdir(const struct winode *i) { return (wi_mode(i) & APFS_S_IFMT) == APFS_S_IFDIR; }
static inline int32_t wi_nlink(const struct winode *i) { return (int32_t)w_le32(i->f + WINO_NLINK); }
static inline void wi_set_nlink(struct winode *i, int32_t n) { w_put_le32(i->f + WINO_NLINK, (uint32_t)n); }
static inline uint64_t wi_flags(const struct winode *i) { return w_le64(i->f + WINO_IFLAGS); }

static void
wi_set_flag(struct winode *i, uint64_t flag, bool on)
{
	uint64_t f = wi_flags(i);

	w_put_le64(i->f + WINO_IFLAGS, on ? (f | flag) : (f & ~flag));
}

static struct wxf *
wi_xf(struct winode *i, uint8_t type)
{
	for (uint32_t n = 0; n < i->nxf; n++) {
		if (i->xf[n].type == type) {
			return &i->xf[n];
		}
	}
	return NULL;
}

static int
wi_xf_set(struct winode *i, uint8_t type, uint8_t flags, const void *data, uint16_t len)
{
	struct wxf *x = wi_xf(i, type);

	if (len > WXF_DATA_MAX) {
		return EINVAL;
	}
	if (x == NULL) {
		uint32_t at = 0;

		if (i->nxf == WXF_MAX) {
			return EINVAL;
		}
		while (at < i->nxf && i->xf[at].type < type) {
			at++;
		}
		memmove(&i->xf[at + 1], &i->xf[at], (i->nxf - at) * sizeof(i->xf[0]));
		i->nxf++;
		x = &i->xf[at];
		x->type = type;
	}
	x->flags = flags;
	x->len = len;
	memcpy(x->data, data, len);
	return 0;
}

static void
wi_xf_del(struct winode *i, uint8_t type)
{
	struct wxf *x = wi_xf(i, type);

	if (x != NULL) {
		uint32_t at = (uint32_t)(x - i->xf);

		memmove(&i->xf[at], &i->xf[at + 1], (i->nxf - at - 1) * sizeof(i->xf[0]));
		i->nxf--;
	}
}

static int
wi_load(struct apfs_wtx *w, uint64_t id, struct winode *i)
{
	uint8_t k[8], *v = apfs_alloc(WVAL_MAX);
	uint16_t vl = 0;
	int error;

	if (v == NULL) {
		return ENOMEM;
	}
	memset(i, 0, sizeof(*i));
	i->id = id;
	error = wbt_get(w, &w->fs, k, wk_simple(k, id, APFS_TYPE_INODE), v, &vl, WVAL_MAX);
	if (error == 0 && vl < WINO_FIXED) {
		error = EIO;
	}
	if (error == 0) {
		memcpy(i->f, v, WINO_FIXED);
		if (vl > WINO_FIXED) {
			const uint8_t *blob = v + WINO_FIXED, *data;
			uint32_t num, used, off;

			if (vl < WINO_FIXED + 4) {
				error = EIO;
				goto out;
			}
			num = w_le16(blob);
			used = w_le16(blob + 2);
			off = 4 + 4 * num;
			if (num > WXF_MAX || WINO_FIXED + off + used != vl) {
				error = EIO;
				goto out;
			}
			data = blob + off;
			for (uint32_t n = 0; n < num; n++) {
				struct wxf *x = &i->xf[n];

				x->type = blob[4 + 4 * n];
				x->flags = blob[5 + 4 * n];
				x->len = w_le16(blob + 6 + 4 * n);
				if (x->len > WXF_DATA_MAX || (uint32_t)(data - blob) + x->len > (uint32_t)(vl - WINO_FIXED)) {
					error = EIO;
					goto out;
				}
				memcpy(x->data, data, x->len);
				data += (x->len + 7u) & ~7u;
			}
			i->nxf = num;
		}
	}
out:
	apfs_free(v, WVAL_MAX);
	return error;
}

static int
wi_store(struct apfs_wtx *w, const struct winode *i)
{
	uint8_t k[8], *v = apfs_alloc(WVAL_MAX);
	uint32_t vl = WINO_FIXED;
	int error;

	if (v == NULL) {
		return ENOMEM;
	}
	memcpy(v, i->f, WINO_FIXED);
	if (i->nxf > 0) {
		uint8_t *blob = v + WINO_FIXED, *data = blob + 4 + 4 * i->nxf;
		uint32_t used = 0;

		w_put_le16(blob, (uint16_t)i->nxf);
		for (uint32_t n = 0; n < i->nxf; n++) {
			const struct wxf *x = &i->xf[n];
			uint32_t pad = (x->len + 7u) & ~7u;

			blob[4 + 4 * n] = x->type;
			blob[5 + 4 * n] = x->flags;
			w_put_le16(blob + 6 + 4 * n, x->len);
			memcpy(data + used, x->data, x->len);
			memset(data + used + x->len, 0, pad - x->len);
			used += pad;
		}
		w_put_le16(blob + 2, (uint16_t)used);
		vl += 4 + 4 * i->nxf + used;
	}
	error = wbt_put(w, &w->fs, k, wk_simple(k, i->id, APFS_TYPE_INODE), v, (uint16_t)vl);
	apfs_free(v, WVAL_MAX);
	return error;
}

static void
wi_dstream(const struct winode *ci, uint64_t *size, uint64_t *alloced)
{
	struct wxf *x = wi_xf((struct winode *)(uintptr_t)ci, APFS_INO_EXT_TYPE_DSTREAM);

	*size = x != NULL && x->len >= 16 ? w_le64(x->data) : 0;
	*alloced = x != NULL && x->len >= 16 ? w_le64(x->data + 8) : 0;
}

static int
wi_set_dstream(struct winode *i, uint64_t size, uint64_t alloced)
{
	struct wxf *x = wi_xf(i, APFS_INO_EXT_TYPE_DSTREAM);
	uint8_t d[40];

	memset(d, 0, sizeof(d));
	if (x != NULL && x->len == 40) {
		memcpy(d, x->data, 40);
	}
	w_put_le64(d, size);
	w_put_le64(d + 8, alloced);
	return wi_xf_set(i, APFS_INO_EXT_TYPE_DSTREAM, WXF_SYSTEM, d, 40);
}

static uint64_t
wi_sparse(const struct winode *ci)
{
	struct wxf *x = wi_xf((struct winode *)(uintptr_t)ci, APFS_INO_EXT_TYPE_SPARSE_BYTES);

	return x != NULL && x->len == 8 ? w_le64(x->data) : 0;
}

static int
wi_set_sparse(struct winode *i, uint64_t bytes)
{
	uint8_t d[8];

	wi_set_flag(i, APFS_INODE_IS_SPARSE, bytes != 0);
	if (bytes == 0) {
		wi_xf_del(i, APFS_INO_EXT_TYPE_SPARSE_BYTES);
		return 0;
	}
	w_put_le64(d, bytes);
	return wi_xf_set(i, APFS_INO_EXT_TYPE_SPARSE_BYTES, WXF_SYSTEM | APFS_XF_CHILDREN_INHERIT, d, 8);
}

static void
wi_touch(struct winode *i, bool mtime)
{
	uint64_t now = apfs_now_ns();

	w_put_le64(i->f + WINO_CTIME, now);
	if (mtime) {
		w_put_le64(i->f + WINO_MTIME, now);
	}
}

static void
wvsb_add(struct apfs_wtx *w, uint32_t off, int64_t delta)
{
	w_put_le64(w->vsb + off, w_le64(w->vsb + off) + (uint64_t)delta);
}

/* ------------------------------------------------------------------ directory entries */

struct wdent {
	uint8_t  key[WTX_KEYMAX];
	uint16_t kl;
	uint64_t id;
	uint16_t dtype;
	uint64_t sibling;               /* 0: none */
	uint64_t added;
};

static void
wd_parse_val(struct wdent *d, const uint8_t *v, uint16_t vl)
{
	d->id = w_le64(v);
	d->added = w_le64(v + 8);
	d->dtype = w_le16(v + 16) & APFS_DREC_TYPE_MASK;
	d->sibling = 0;
	if (vl >= 18 + 4 + 4 + 8 && w_le16(v + 18) == 1 && v[22] == APFS_DREC_EXT_TYPE_SIBLING_ID) {
		d->sibling = w_le64(v + 26);
	}
}

/* The entry for name in dir, as stored (its exact key), however the name was spelled. */
static int
wd_lookup(struct apfs_wtx *w, uint64_t dir, const char *name, size_t len, struct wdent *d)
{
	uint8_t probe[WTX_KEYMAX], v[64];
	uint16_t pl, vl;
	int error = wk_drec(w, probe, &pl, dir, name, len);
	bool fold = (w->vol->incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;

	if (error) {
		return error;
	}
	if (!w->fs.hashed) {
		error = wbt_get(w, &w->fs, probe, pl, v, &vl, sizeof(v));
		if (error) {
			return error;
		}
		memcpy(d->key, probe, pl);
		d->kl = pl;
		wd_parse_val(d, v, vl);
		return 0;
	}
	uint32_t hash = w_le32(probe + 8) >> APFS_J_DREC_HASH_SHIFT;

	w_put_le32(probe + 8, (hash << APFS_J_DREC_HASH_SHIFT) | 1);
	probe[12] = 0;
	pl = 13;
	for (bool after = false;; after = true) {
		error = wbt_next(w, &w->fs, probe, pl, after, d->key, &d->kl, v, &vl, sizeof(v));
		if (error != 0 && error != EOVERFLOW) {
			return error;
		}
		if (wk_id(d->key) != dir || wk_type(d->key) != APFS_TYPE_DIR_REC || d->kl < 13 ||
		    (w_le32(d->key + 8) >> APFS_J_DREC_HASH_SHIFT) != hash) {
			return ENOENT;
		}
		if (error) {
			return EIO;             /* a directory record's value is never that long */
		}
		uint32_t nl = w_le32(d->key + 8) & APFS_J_DREC_LEN_MASK;

		if (nl >= 1 && 12u + nl == d->kl &&
		    apfs_names_equal((const char *)d->key + 12, nl - 1, name, len, fold)) {
			if (vl < 18) {
				return EIO;
			}
			wd_parse_val(d, v, vl);
			return 0;
		}
		memcpy(probe, d->key, d->kl);
		pl = d->kl;
	}
}

/* The name an entry is stored under (no NUL). */
static void
wd_name(struct apfs_wtx *w, const struct wdent *d, const char **name, size_t *len)
{
	uint32_t off = w->fs.hashed ? 12 : 10;

	*name = (const char *)d->key + off;
	*len = d->kl > off ? d->kl - off - 1u : 0;
}

static int
wd_put(struct apfs_wtx *w, const uint8_t *key, uint16_t kl, uint64_t id, uint16_t dtype,
    uint64_t sibling, uint64_t added)
{
	uint8_t v[34];
	uint16_t vl = 18;

	w_put_le64(v, id);
	w_put_le64(v + 8, added);
	w_put_le16(v + 16, dtype);
	if (sibling != 0) {
		w_put_le16(v + 18, 1);
		w_put_le16(v + 20, 8);
		v[22] = APFS_DREC_EXT_TYPE_SIBLING_ID;
		v[23] = 0;
		w_put_le16(v + 24, 8);
		w_put_le64(v + 26, sibling);
		vl = 34;
	}
	return wbt_put(w, &w->fs, key, kl, v, vl);
}

/* Any entry at all in dir? */
static int
wd_any(struct apfs_wtx *w, uint64_t dir, bool *any)
{
	uint8_t probe[8], k[WTX_KEYMAX];
	uint16_t kl;
	int error = wbt_next(w, &w->fs, probe, wk_simple(probe, dir, APFS_TYPE_DIR_REC), false, k, &kl,
	    NULL, NULL, WVAL_MAX);

	if (error == ENOENT) {
		*any = false;
		return 0;
	}
	if (error != 0 && error != EOVERFLOW) {
		return error;
	}
	*any = wk_id(k) == dir && wk_type(k) == APFS_TYPE_DIR_REC;
	return 0;
}

/* ------------------------------------------------------------------ records of one object */

/*
 * The first record of (id, type) at or after the probe's own position, copied out; ENOENT
 * when there are no more of that kind.  v holds vcap bytes; the next record may be of
 * another kind with a longer value, which is only looked at by its key.
 */
static int
wr_next(struct apfs_wtx *w, struct wtree *t, uint64_t id, uint8_t type, const uint8_t *probe,
    uint16_t pl, bool after, uint8_t *k, uint16_t *kl, uint8_t *v, uint16_t *vl, uint16_t vcap)
{
	int error = wbt_next(w, t, probe, pl, after, k, kl, v, vl, vcap);

	if (error != 0 && error != EOVERFLOW) {
		return error;
	}
	if (wk_id(k) != id || wk_type(k) != type) {
		return ENOENT;
	}
	return error == EOVERFLOW ? EIO : 0;
}

/* ------------------------------------------------------------------ extents */

struct wext {
	uint64_t logical;               /* bytes */
	uint64_t len;                   /* bytes */
	uint64_t phys;                  /* block; 0: a hole */
};

static int
we_get(struct apfs_wtx *w, const uint8_t *k, const uint8_t *v, uint16_t vl, struct wext *e)
{
	if (vl < 24) {
		return EIO;
	}
	e->logical = w_le64(k + 8);
	e->len = w_le64(v) & 0x00ffffffffffffffULL;
	e->phys = w_le64(v + 8);
	if (e->len == 0 || e->len % w->bs != 0 || e->logical % w->bs != 0) {
		return EIO;
	}
	return 0;
}

static int
we_put(struct apfs_wtx *w, uint64_t stream, const struct wext *e)
{
	uint8_t k[16], v[24];

	wk_num(k, stream, APFS_TYPE_FILE_EXTENT, e->logical);
	w_put_le64(v, e->len);
	w_put_le64(v + 8, e->phys);
	w_put_le64(v + 16, 0);
	return wbt_put(w, &w->fs, k, 16, v, 24);
}

static int
we_del(struct apfs_wtx *w, uint64_t stream, uint64_t logical)
{
	uint8_t k[16];

	return wbt_del(w, &w->fs, k, wk_num(k, stream, APFS_TYPE_FILE_EXTENT, logical));
}

/* The extent of stream that holds byte off: ENOENT past the last one. */
static int
we_find(struct apfs_wtx *w, uint64_t stream, uint64_t off, struct wext *e)
{
	uint8_t probe[16], k[WTX_KEYMAX], v[64];
	uint16_t kl, vl;
	int error;

	/* the extent at off, or the last one starting before it */
	wk_num(probe, stream, APFS_TYPE_FILE_EXTENT, off);
	error = wbt_floor(w, &w->fs, probe, 16, k, &kl, v, &vl, sizeof(v));
	if (error != 0 && error != EOVERFLOW) {
		return error;
	}
	if (wk_id(k) != stream || wk_type(k) != APFS_TYPE_FILE_EXTENT) {
		return ENOENT;
	}
	if (error) {
		return EIO;
	}
	error = we_get(w, k, v, vl, e);
	if (error) {
		return error;
	}
	return off < e->logical + e->len ? 0 : ENOENT;
}

/* The extent-reference record of a run of physical blocks the stream owns. */
static int
wpe_put(struct apfs_wtx *w, uint64_t phys, uint64_t blocks, uint64_t owner)
{
	uint8_t k[8], v[20];

	w_put_le64(v, blocks | (WKIND_NEW << APFS_PEXT_KIND_SHIFT));
	w_put_le64(v + 8, owner);
	w_put_le32(v + 16, 1);
	return wbt_put(w, &w->eref, k, wk_simple(k, phys, APFS_TYPE_EXTENT), v, 20);
}

static int
wpe_del(struct apfs_wtx *w, uint64_t phys)
{
	uint8_t k[8];

	return wbt_del(w, &w->eref, k, wk_simple(k, phys, APFS_TYPE_EXTENT));
}

/* Data blocks no longer in the stream: their reference record, and the blocks at commit. */
static int
we_release(struct apfs_wtx *w, uint64_t phys, uint64_t blocks, bool whole_record)
{
	int error = 0;

	if (whole_record) {
		error = wpe_del(w, phys);
	}
	if (error == 0) {
		error = wsp_free_later(w, phys, blocks, true);
	}
	if (error == 0) {
		w->vol->alloc_count -= blocks;
		wvsb_add(w, VSB_FREED, (int64_t)blocks);
	}
	return error;
}

/*
 * Cut the stream at byte `from` (a multiple of the block size): every extent after it goes,
 * the one across it is shortened.  Returns the bytes of holes removed in *holes.
 */
static int
we_cut(struct apfs_wtx *w, uint64_t stream, uint64_t from, uint64_t *holes)
{
	uint8_t probe[16], k[WTX_KEYMAX], v[64];
	uint16_t kl, vl;
	struct wext e;
	int error;

	*holes = 0;
	error = we_find(w, stream, from, &e);
	if (error == 0 && e.logical < from) {
		uint64_t keep = from - e.logical;

		if (e.phys != 0) {
			uint64_t kb = keep / w->bs;

			error = we_release(w, e.phys + kb, (e.len - keep) / w->bs, false);
			if (error == 0) {
				error = wpe_put(w, e.phys, kb, stream);
			}
		} else {
			*holes += e.len - keep;
		}
		e.len = keep;
		if (error == 0) {
			error = we_put(w, stream, &e);
		}
		if (error) {
			return error;
		}
	} else if (error != 0 && error != ENOENT) {
		return error;
	}
	wk_num(probe, stream, APFS_TYPE_FILE_EXTENT, from);
	for (;;) {
		error = wr_next(w, &w->fs, stream, APFS_TYPE_FILE_EXTENT, probe, 16, false, k, &kl, v, &vl, sizeof(v));
		if (error == ENOENT) {
			return 0;
		}
		if (error == 0) {
			error = we_get(w, k, v, vl, &e);
		}
		if (error == 0 && e.phys != 0) {
			error = we_release(w, e.phys, e.len / w->bs, true);
		} else if (error == 0) {
			*holes += e.len;
		}
		if (error == 0) {
			error = we_del(w, stream, e.logical);
		}
		if (error) {
			return error;
		}
	}
}

/*
 * Blocks for bytes [off, off + want) of a stream whose extents cover [0, *alloced): the
 * physical block where off lands and how many blocks run on from there (*got), allocating
 * what is a hole or past the end.  *alloced and *holes are updated.
 */
static int
we_map(struct apfs_wtx *w, uint64_t stream, uint64_t off, uint64_t want_blocks, bool alloc,
    uint64_t *alloced, uint64_t *holes, uint64_t *phys, uint64_t *got)
{
	uint64_t L = off / w->bs * w->bs, nb, p;
	struct wext e;
	int error;

	if (want_blocks == 0) {
		want_blocks = 1;
	}
	if (L < *alloced) {
		error = we_find(w, stream, L, &e);
		if (error) {
			return error == ENOENT ? EIO : error;
		}
		uint64_t left = (e.logical + e.len - L) / w->bs;

		if (e.phys != 0) {
			*phys = e.phys + (L - e.logical) / w->bs;
			*got = left < want_blocks ? left : want_blocks;
			return 0;
		}
		if (!alloc) {
			*phys = 0;
			*got = left < want_blocks ? left : want_blocks;
			return 0;
		}
		/*
		 * A hole: the part asked for becomes data, what is left of it on either side stays a
		 * hole.  Data right after a data extent, on the disk as well, makes that one longer.
		 */
		struct wext prev = { 0, 0, 0 };
		bool join = L == e.logical && L > 0 && we_find(w, stream, L - w->bs, &prev) == 0 && prev.phys != 0;

		error = wsp_alloc(w, join ? prev.phys + prev.len / w->bs : 0,
		    left < want_blocks ? left : want_blocks, &p, &nb);
		if (error) {
			return error;
		}
		struct wext pre = e, mid = { L, nb * w->bs, p }, post = { L + nb * w->bs, e.logical + e.len - L - nb * w->bs, 0 };

		pre.len = L - e.logical;
		join = join && prev.phys + prev.len / w->bs == p;
		if (join) {
			prev.len += nb * w->bs;
			error = we_put(w, stream, &prev);
			if (error == 0) {
				error = we_del(w, stream, e.logical);
			}
			if (error == 0) {
				error = wpe_put(w, prev.phys, prev.len / w->bs, stream);
			}
		} else {
			error = pre.len != 0 ? we_put(w, stream, &pre) : 0;     /* (else mid takes its key) */
			if (error == 0) {
				error = we_put(w, stream, &mid);
			}
			if (error == 0) {
				error = wpe_put(w, p, nb, stream);
			}
		}
		if (error == 0 && post.len > 0) {
			error = we_put(w, stream, &post);
		}
		if (error) {
			return error;
		}
		*holes -= nb * w->bs;
	} else {
		if (!alloc) {
			*phys = 0;
			*got = want_blocks;
			return 0;
		}
		if (L > *alloced) {
			/* the gap up to off: a hole, or the hole already last made longer */
			struct wext gap = { *alloced, L - *alloced, 0 };

			if (*alloced > 0 && we_find(w, stream, *alloced - w->bs, &e) == 0 && e.phys == 0) {
				gap.logical = e.logical;
				gap.len = L - e.logical;
			}
			error = we_put(w, stream, &gap);
			if (error) {
				return error;
			}
			*holes += L - *alloced;
			*alloced = L;
		}
		bool tail = L > 0 && we_find(w, stream, L - w->bs, &e) == 0 && e.phys != 0;

		error = wsp_alloc(w, tail ? e.phys + e.len / w->bs : 0, want_blocks, &p, &nb);
		if (error) {
			return error;
		}
		/* right after the last data extent, on the disk too: that extent just grows */
		if (tail && e.phys + e.len / w->bs == p) {
			uint64_t start = e.phys;

			e.len += nb * w->bs;
			error = we_put(w, stream, &e);
			if (error == 0) {
				error = wpe_put(w, start, e.len / w->bs, stream);
			}
		} else {
			struct wext ne = { L, nb * w->bs, p };

			error = we_put(w, stream, &ne);
			if (error == 0) {
				error = wpe_put(w, p, nb, stream);
			}
		}
		if (error) {
			return error;
		}
		*alloced = L + nb * w->bs;
	}
	w->vol->alloc_count += nb;
	wvsb_add(w, VSB_ALLOCED, (int64_t)nb);
	*phys = p;
	*got = nb;
	return 0;
}

/* Extend a stream with a hole to `to` bytes (a multiple of the block size). */
static int
we_grow_hole(struct apfs_wtx *w, uint64_t stream, uint64_t *alloced, uint64_t *holes, uint64_t to)
{
	struct wext e, gap = { *alloced, to - *alloced, 0 };
	int error;

	if (to <= *alloced) {
		return 0;
	}
	if (*alloced > 0 && we_find(w, stream, *alloced - w->bs, &e) == 0 && e.phys == 0) {
		gap.logical = e.logical;
		gap.len = to - e.logical;
	}
	error = we_put(w, stream, &gap);
	if (error == 0) {
		*holes += to - *alloced;
		*alloced = to;
	}
	return error;
}

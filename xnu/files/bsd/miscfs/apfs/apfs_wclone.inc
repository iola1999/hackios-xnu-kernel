/*
 * clonefile(2) on a writable volume (included by apfs_write.c after apfs_wxattr.inc): a copy of
 * a file, a symbolic link or a whole directory tree, made in one transaction.
 *
 * The copy has blocks of its own.  APFS shares the source's extents and copies a block only
 * when one side writes it; that needs reference counts on shared extents and copy-on-write in
 * vnop_blockmap, which this engine does not do yet, so the cost here is the copy of the data
 * and the space for it.  What the copy has of its source: the type, mode and owner (unless the
 * caller says otherwise for the top), the BSD flags, the times, the protection class, the
 * extended attributes and the contents.  Hard links inside a tree become separate files.
 *
 * Everything is looked up and the space counted before the first change; after it, a failure
 * poisons the transaction (wtx_poison), as for every other operation.
 */
#pragma once

#define WCL_MAX_ITEMS    262144          /* objects in one tree */
#define WCL_CHUNK        (1024 * 1024)   /* data copied at a time */
#define WCL_NONE         UINT32_MAX
#define WCL_MIN(a, b)    ((a) < (b) ? (a) : (b))

struct wcl_item {
	uint64_t src;
	uint64_t made;                  /* the copy, once made */
	uint32_t parent;                /* the index of its directory's item; WCL_NONE for the top */
	uint16_t namelen;
	uint8_t  dtype;
	char     name[256];
};

struct wcl {
	struct apfs_wtx *w;
	struct wcl_item *items;
	uint32_t         n, cap;
	uint32_t         dir;           /* the item readdir is filling in */
	uint64_t         blocks;        /* data blocks the copy needs */
	int              error;
};

static int
wcl_add(struct wcl *c, uint64_t src, uint32_t parent, const char *name, size_t len, uint8_t dtype)
{
	if (len > 255) {
		return ENAMETOOLONG;
	}
	if (c->n == c->cap) {
		uint32_t cap = c->cap ? c->cap * 2 : 64;
		struct wcl_item *more;

		if (cap > WCL_MAX_ITEMS) {
			return E2BIG;
		}
		more = apfs_alloc((size_t)cap * sizeof(*more));
		if (more == NULL) {
			return ENOMEM;
		}
		if (c->items != NULL) {
			memcpy(more, c->items, (size_t)c->n * sizeof(*more));
			apfs_free(c->items, (size_t)c->cap * sizeof(*more));
		}
		c->items = more;
		c->cap = cap;
	}
	struct wcl_item *it = &c->items[c->n++];

	it->src = src;
	it->made = 0;
	it->parent = parent;
	it->namelen = (uint16_t)len;
	it->dtype = dtype;
	memcpy(it->name, name, len);
	it->name[len] = 0;
	return 0;
}

static int
wcl_entry(void *ctx, const char *name, size_t len, uint64_t id, uint8_t dtype)
{
	struct wcl *c = ctx;

	c->error = wcl_add(c, id, c->dir, name, len, dtype);
	return c->error;
}

static int
wcl_xattr_count(void *ctx, const char *name, size_t len, const struct apfs_xattr *xa)
{
	struct wcl *c = ctx;

	if (len >= 128 || strncmp(name, "com.apple.fs.", 13) == 0) {
		return 0;
	}
	if (xa->flags & APFS_XATTR_DATA_STREAM) {
		c->blocks += (xa->stream_size + c->w->bs - 1) / c->w->bs;
	} else if (xa->data_len > APFS_XATTR_MAX_EMBEDDED_SIZE) {
		c->blocks += (xa->data_len + c->w->bs - 1) / c->w->bs;
	}
	return 0;
}

/* Every object of the tree, parents before children, and the data blocks they hold. */
static int
wcl_collect(struct wcl *c)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	int error = i == NULL ? ENOMEM : 0;

	for (uint32_t k = 0; error == 0 && k < c->n; k++) {
		error = wi_load(c->w, c->items[k].src, i);
		if (error) {
			break;
		}
		if (wi_isdir(i)) {
			c->dir = k;
			c->error = 0;
			error = apfs_readdir(c->w->vol, c->items[k].src, wcl_entry, c);
			if (error == 0) {
				error = c->error;
			}
		} else if ((wi_mode(i) & APFS_S_IFMT) == APFS_S_IFREG && wi_xf(i, APFS_INO_EXT_TYPE_DSTREAM) != NULL) {
			uint64_t size, alloced;

			wi_dstream(i, &size, &alloced);
			c->blocks += (alloced - WCL_MIN(alloced, wi_sparse(i))) / c->w->bs;
		}
		if (error == 0) {
			error = apfs_xattr_list(c->w->vol, c->items[k].src, wcl_xattr_count, c);
		}
	}
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

/* The source's extents into blocks of the copy's own, `buf` (WCL_CHUNK bytes) at a time. */
static int
wcl_data(struct apfs_wtx *w, struct winode *si, uint64_t dst, uint8_t *buf)
{
	uint64_t size, alloced, stream = w_le64(si->f + WINO_PRIVATE), off = 0;
	struct apfs_nx *nx = w->nx;
	int error;

	wi_dstream(si, &size, &alloced);
	if (size == 0) {
		return 0;
	}
	error = apfs_w_truncate(w, dst, size);
	while (error == 0 && off < alloced) {
		struct wext e;

		error = we_find(w, stream, off, &e);
		if (error) {
			break;
		}
		for (uint64_t at = off; error == 0 && e.phys != 0 && at < e.logical + e.len;) {
			uint64_t n = WCL_MIN((uint64_t)WCL_CHUNK, e.logical + e.len - at), phys, run;

			error = apfs_w_map(w, dst, at, n, true, &phys, &run);
			if (error == 0 && phys == 0) {
				error = EIO;
			}
			if (error == 0) {
				n = WCL_MIN(n, run);
				error = nx->dev.read(nx->dev.ctx, (e.phys + (at - e.logical) / w->bs) * w->bs, buf, n);
			}
			if (error == 0) {
				error = nx->dev.write(nx->dev.ctx, phys * w->bs, buf, n);
			}
			at += n;
		}
		off = e.logical + e.len;
	}
	return error;
}

struct wcl_xattrs {
	char     (*names)[128];
	uint32_t n, cap;
};

static int
wcl_xattr_name(void *ctx, const char *name, size_t len, __unused const struct apfs_xattr *xa)
{
	struct wcl_xattrs *x = ctx;

	if (len >= 128 || strncmp(name, "com.apple.fs.", 13) == 0) {
		return 0;                       /* the file system's own (a link's target) */
	}
	if (x->n == x->cap) {
		return E2BIG;
	}
	memcpy(x->names[x->n], name, len);
	x->names[x->n][len] = 0;
	x->n++;
	return 0;
}

/* The source's extended attributes onto the copy (`buf` holds WCL_CHUNK bytes). */
static int
wcl_xattrs(struct apfs_wtx *w, uint64_t src, uint64_t dst, uint8_t *buf)
{
	struct wcl_xattrs x = { .cap = 64 };
	int error;

	x.names = apfs_alloc(x.cap * sizeof(*x.names));
	if (x.names == NULL) {
		return ENOMEM;
	}
	error = apfs_xattr_list(w->vol, src, wcl_xattr_name, &x);
	for (uint32_t k = 0; error == 0 && k < x.n; k++) {
		size_t len = 0;

		error = apfs_xattr_read(w->vol, src, x.names[k], buf, WCL_CHUNK, &len);
		if (error == 0) {
			error = apfs_w_setxattr(w, dst, x.names[k], buf, len, 0);
		}
	}
	apfs_free(x.names, x.cap * sizeof(*x.names));
	return error;
}

/* One object of the tree, in the directory its parent's copy is. */
static int
wcl_make(struct wcl *c, uint32_t k, uint64_t top_dir, const struct apfs_wattr *top, uint32_t flags,
    struct winode *si, uint8_t *buf)
{
	struct apfs_wtx *w = c->w;
	struct wcl_item *it = &c->items[k];
	struct apfs_wnew a = { 0 };
	struct apfs_wattr t = { 0 };
	uint64_t dir = it->parent == WCL_NONE ? top_dir : c->items[it->parent].made;
	size_t tl = 0;
	int error = wi_load(w, it->src, si);

	if (error) {
		return error;
	}
	a.mode = wi_mode(si);
	a.uid = w_le32(si->f + WINO_UID);
	a.gid = w_le32(si->f + WINO_GID);
	a.pclass = w_le32(si->f + WINO_PCLASS);
	if (wi_xf(si, APFS_INO_EXT_TYPE_RDEV) != NULL) {
		a.rdev = w_le32(wi_xf(si, APFS_INO_EXT_TYPE_RDEV)->data);
	}
	if ((top->set & APFS_WA_UID) && (k == 0 || (flags & APFS_WCLONE_OWNER))) {
		a.uid = top->uid;
	}
	if ((top->set & APFS_WA_GID) && (k == 0 || (flags & APFS_WCLONE_OWNER))) {
		a.gid = top->gid;
	}
	if (k == 0 && (top->set & APFS_WA_MODE)) {
		a.mode = (uint16_t)((a.mode & APFS_S_IFMT) | (top->mode & 07777));
	}
	if ((a.mode & APFS_S_IFMT) == APFS_S_IFLNK) {
		error = apfs_readlink(w->vol, it->src, (char *)buf, WCL_CHUNK, &tl);
		a.target = (const char *)buf;
		a.target_len = tl;
	}
	if (error == 0) {
		error = apfs_w_create(w, dir, it->name, it->namelen, &a, &it->made);
	}
	if (error == 0 && (a.mode & APFS_S_IFMT) == APFS_S_IFREG) {
		error = wcl_data(w, si, it->made, buf);
	}
	if (error == 0) {
		error = wcl_xattrs(w, it->src, it->made, buf);
	}
	if (error == 0) {
		t.set = APFS_WA_FLAGS | APFS_WA_CRTIME | APFS_WA_MTIME | APFS_WA_ATIME;
		t.flags = k == 0 && (top->set & APFS_WA_FLAGS) ? top->flags : w_le32(si->f + WINO_BSDFLAGS);
		t.crtime = w_le64(si->f + WINO_CRTIME);
		t.mtime = w_le64(si->f + WINO_MTIME);
		t.atime = w_le64(si->f + WINO_ATIME);
		error = apfs_w_setattr(w, it->made, &t);
	}
	return error;
}

int
apfs_w_clone(struct apfs_wtx *w, uint64_t src, uint64_t dir, const char *name, size_t len,
    const struct apfs_wattr *top, uint32_t flags, uint64_t *id)
{
	struct wcl c = { .w = w };
	struct winode *si = apfs_alloc(sizeof(*si));
	uint8_t *buf = apfs_alloc(WCL_CHUNK);
	struct wdent d;
	int error = (si == NULL || buf == NULL) ? ENOMEM : wop_begin(w, 0);

	if (error == 0) {
		error = wname_ok(name, len);
	}
	if (error == 0) {
		error = wdir_check(w, dir);
	}
	if (error == 0) {
		error = wd_lookup(w, dir, name, len, &d);
		error = error == 0 ? EEXIST : (error == ENOENT ? 0 : error);
	}
	if (error == 0) {
		error = wcl_add(&c, src, WCL_NONE, name, len, 0);
	}
	if (error == 0) {
		error = wcl_collect(&c);
	}
	/* a directory copied into itself would never end */
	for (uint32_t k = 0; error == 0 && k < c.n; k++) {
		if (c.items[k].src == dir) {
			error = EINVAL;
		}
	}
	if (error == 0) {
		error = wop_begin(w, c.blocks + (uint64_t)c.n * 4);
	}
	/* from the first change on, a failure is the transaction's */
	for (uint32_t k = 0; error == 0 && k < c.n; k++) {
		error = wcl_make(&c, k, dir, top, flags, si, buf);
		if (error) {
			wtx_poison(w, error);
			error = EIO;
		}
	}
	if (error == 0) {
		*id = c.items[0].made;
	}
	if (c.items != NULL) {
		apfs_free(c.items, (size_t)c.cap * sizeof(*c.items));
	}
	if (si != NULL) {
		apfs_free(si, sizeof(*si));
	}
	if (buf != NULL) {
		apfs_free(buf, WCL_CHUNK);
	}
	return error;
}

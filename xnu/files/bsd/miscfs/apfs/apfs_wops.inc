/*
 * The operations of a writable volume (included by apfs_write.c; the API is in apfs_write.h).
 *
 * Each one looks everything up and checks it first, and makes sure of the space it may need,
 * before its first change; after that only a device error or an exhausted kernel heap can
 * stop it, and those poison the transaction (wtx_poison) instead of leaving half an operation
 * in it.
 */
#pragma once

/* Blocks one operation may need at most: new nodes on every level of every tree it changes. */
#define WOP_RESERVE 256

static int
wop_begin(struct apfs_wtx *w, uint64_t data_blocks)
{
	if (w->broken) {
		return EIO;
	}
	if (wsp_free_blocks(w) < WOP_RESERVE + data_blocks) {
		return ENOSPC;
	}
	return 0;
}

/* After the first change: any error from here on is the transaction's end. */
static int
wop_end(struct apfs_wtx *w, int error)
{
	if (error != 0) {
		wtx_poison(w, error);
		return EIO;
	}
	return 0;
}

bool
apfs_wtx_dirty(const struct apfs_wtx *w)
{
	return w != NULL && w->open;
}

bool
apfs_wtx_broken(const struct apfs_wtx *w)
{
	return w == NULL || w->broken;
}

uint32_t
apfs_wtx_dirty_blocks(const struct apfs_wtx *w)
{
	return w->nblk;
}

uint64_t
apfs_wtx_free_blocks(const struct apfs_wtx *w)
{
	return w->sp.free_count > WOP_RESERVE ? w->sp.free_count - WOP_RESERVE : 0;
}

void
apfs_wtx_stats(const struct apfs_wtx *w, uint64_t *commits, uint64_t *blocks, uint64_t *ticks)
{
	*commits = w->commits;
	*blocks = w->commit_blocks;
	*ticks = w->commit_ticks;
}

static uint16_t
wmode_dtype(uint16_t mode)
{
	return apfs_mode_to_dtype(mode);
}

static void
wcount(struct apfs_wtx *w, uint16_t mode, int delta)
{
	switch (mode & APFS_S_IFMT) {
	case APFS_S_IFREG:
		w->vol->num_files += (uint64_t)(int64_t)delta;
		break;
	case APFS_S_IFDIR:
		w->vol->num_dirs += (uint64_t)(int64_t)delta;
		break;
	case APFS_S_IFLNK:
		w->vol->num_symlinks += (uint64_t)(int64_t)delta;
		break;
	default:
		wvsb_add(w, VSB_NUM_OTHER, delta);
		break;
	}
}

static int
wi_set_name(struct winode *i, uint64_t parent, const char *name, size_t len)
{
	uint8_t buf[256];

	if (len > 255) {
		return ENAMETOOLONG;
	}
	memcpy(buf, name, len);
	buf[len] = 0;
	w_put_le64(i->f + WINO_PARENT, parent);
	return wi_xf_set(i, APFS_INO_EXT_TYPE_NAME, APFS_XF_DO_NOT_COPY, buf, (uint16_t)(len + 1));
}

/* A directory gains or loses an entry. */
static int
wdir_adjust(struct apfs_wtx *w, uint64_t dir, int delta)
{
	struct winode *d = apfs_alloc(sizeof(*d));
	int error;

	if (d == NULL) {
		return ENOMEM;
	}
	error = wi_load(w, dir, d);
	if (error == 0) {
		wi_set_nlink(d, wi_nlink(d) + delta);
		wi_touch(d, true);
		error = wi_store(w, d);
	}
	apfs_free(d, sizeof(*d));
	return error;
}

static int
wdir_check(struct apfs_wtx *w, uint64_t dir)
{
	struct winode *d = apfs_alloc(sizeof(*d));
	int error;

	if (d == NULL) {
		return ENOMEM;
	}
	error = wi_load(w, dir, d);
	if (error == 0 && !wi_isdir(d)) {
		error = ENOTDIR;
	}
	apfs_free(d, sizeof(*d));
	return error;
}

/* ------------------------------------------------------------------ creating */

int
apfs_w_create(struct apfs_wtx *w, uint64_t dir, const char *name, size_t len,
    const struct apfs_wnew *a, uint64_t *id)
{
	struct winode *i = NULL;
	struct wdent d;
	uint8_t key[WTX_KEYMAX];
	uint16_t kl, type = a->mode & APFS_S_IFMT;
	uint64_t now = apfs_now_ns();
	int error = wname_ok(name, len);

	if (error == 0 && type == 0) {
		error = EINVAL;
	}
	if (error == 0 && type == APFS_S_IFLNK && (a->target == NULL || a->target_len == 0 ||
	    a->target_len + 1 > APFS_XATTR_MAX_EMBEDDED_SIZE)) {
		error = a->target_len + 1 > APFS_XATTR_MAX_EMBEDDED_SIZE ? ENAMETOOLONG : EINVAL;
	}
	if (error == 0) {
		error = wop_begin(w, 0);
	}
	if (error == 0) {
		error = wdir_check(w, dir);
	}
	if (error == 0) {
		error = wd_lookup(w, dir, name, len, &d);
		error = error == 0 ? EEXIST : (error == ENOENT ? 0 : error);
	}
	if (error == 0) {
		error = wk_drec(w, key, &kl, dir, name, len);
	}
	if (error == 0) {
		i = apfs_alloc(sizeof(*i));
		error = i == NULL ? ENOMEM : 0;
	}
	if (error) {
		goto out;
	}

	i->id = w->vol->next_obj_id++;
	w_put_le64(i->f + WINO_PRIVATE, i->id);
	w_put_le64(i->f + WINO_CRTIME, now);
	w_put_le64(i->f + WINO_MTIME, now);
	w_put_le64(i->f + WINO_CTIME, now);
	w_put_le64(i->f + WINO_ATIME, now);
	w_put_le64(i->f + WINO_IFLAGS, APFS_INODE_NO_RSRC_FORK);
	wi_set_nlink(i, type == APFS_S_IFDIR ? 0 : 1);
	w_put_le32(i->f + WINO_PCLASS, a->pclass);
	w_put_le32(i->f + WINO_UID, a->uid);
	w_put_le32(i->f + WINO_GID, a->gid);
	w_put_le16(i->f + WINO_MODE, a->mode);
	error = wi_set_name(i, dir, name, len);
	if (error == 0 && (type == APFS_S_IFCHR || type == APFS_S_IFBLK)) {
		uint8_t rdev[4];

		w_put_le32(rdev, a->rdev);
		error = wi_xf_set(i, APFS_INO_EXT_TYPE_RDEV, 0, rdev, 4);
	}
	if (error) {
		goto out;
	}

	error = wi_store(w, i);
	if (error == 0 && type == APFS_S_IFLNK) {
		uint8_t xk[WTX_KEYMAX], *xv = apfs_alloc(WVAL_MAX);

		if (xv == NULL) {
			error = ENOMEM;
		} else {
			uint16_t xkl = wk_xattr(xk, i->id, WXATTR_SYMLINK, strlen(WXATTR_SYMLINK));

			w_put_le16(xv, APFS_XATTR_DATA_EMBEDDED | APFS_XATTR_FILE_SYSTEM_OWNED);
			w_put_le16(xv + 2, (uint16_t)(a->target_len + 1));
			memcpy(xv + 4, a->target, a->target_len);
			xv[4 + a->target_len] = 0;
			error = wbt_put(w, &w->fs, xk, xkl, xv, (uint16_t)(4 + a->target_len + 1));
			apfs_free(xv, WVAL_MAX);
		}
	}
	if (error == 0) {
		error = wd_put(w, key, kl, i->id, wmode_dtype(a->mode), 0, now);
	}
	if (error == 0) {
		error = wdir_adjust(w, dir, 1);
	}
	if (error == 0) {
		wcount(w, a->mode, 1);
		*id = i->id;
	}
	error = wop_end(w, error);
out:
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

/* ------------------------------------------------------------------ hard links */

static int
wsib_put(struct apfs_wtx *w, uint64_t id, uint64_t sib, uint64_t parent, const uint8_t *name,
    uint16_t nl)
{
	uint8_t k[16], v[8 + 2 + 256], mk[8], mv[8];
	int error;

	if (nl > 256) {
		return EIO;
	}
	w_put_le64(v, parent);
	w_put_le16(v + 8, nl);
	memcpy(v + 10, name, nl);
	error = wbt_put(w, &w->fs, k, wk_num(k, id, APFS_TYPE_SIBLING_LINK, sib), v, (uint16_t)(10 + nl));
	if (error == 0) {
		w_put_le64(mv, id);
		error = wbt_put(w, &w->fs, mk, wk_simple(mk, sib, APFS_TYPE_SIBLING_MAP), mv, 8);
	}
	return error;
}

static int
wsib_del(struct apfs_wtx *w, uint64_t id, uint64_t sib)
{
	uint8_t k[16], mk[8];
	int error = wbt_del(w, &w->fs, k, wk_num(k, id, APFS_TYPE_SIBLING_LINK, sib));

	if (error == 0) {
		error = wbt_del(w, &w->fs, mk, wk_simple(mk, sib, APFS_TYPE_SIBLING_MAP));
	}
	return error;
}

/* The inode's lowest-numbered sibling link: its id, parent and name. ENOENT if none. */
static int
wsib_first(struct apfs_wtx *w, uint64_t id, uint64_t *sib, uint64_t *parent, uint8_t *name,
    uint16_t *nl)
{
	uint8_t probe[16], k[WTX_KEYMAX], *v = apfs_alloc(WVAL_MAX);
	uint16_t kl, vl;
	int error;

	if (v == NULL) {
		return ENOMEM;
	}
	wk_num(probe, id, APFS_TYPE_SIBLING_LINK, 0);
	error = wr_next(w, &w->fs, id, APFS_TYPE_SIBLING_LINK, probe, 16, false, k, &kl, v, &vl, WVAL_MAX);
	if (error == 0 && (kl < 16 || vl < 10 || w_le16(v + 8) > 256 || 10u + w_le16(v + 8) > vl)) {
		error = EIO;
	}
	if (error == 0) {
		*sib = w_le64(k + 8);
		*parent = w_le64(v);
		*nl = w_le16(v + 8);
		memcpy(name, v + 10, *nl);
	}
	apfs_free(v, WVAL_MAX);
	return error;
}

int
apfs_w_link(struct apfs_wtx *w, uint64_t id, uint64_t dir, const char *name, size_t len)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	struct wdent d;
	uint8_t key[WTX_KEYMAX], sname[256];
	uint16_t kl, snl;
	uint64_t sib, sparent;
	int error = i == NULL ? ENOMEM : wname_ok(name, len);

	if (error == 0) {
		error = wop_begin(w, 0);
	}
	if (error == 0) {
		error = wi_load(w, id, i);
	}
	if (error == 0 && wi_isdir(i)) {
		error = EPERM;
	}
	if (error == 0 && wi_nlink(i) >= 0xffff) {
		error = EMLINK;
	}
	if (error == 0 && w_le64(i->f + WINO_PARENT) == APFS_PRIV_DIR_INO_NUM && wi_nlink(i) == 0) {
		error = ENOENT;                 /* an orphan: unlinked already */
	}
	if (error == 0) {
		error = wdir_check(w, dir);
	}
	if (error == 0) {
		error = wd_lookup(w, dir, name, len, &d);
		error = error == 0 ? EEXIST : (error == ENOENT ? 0 : error);
	}
	if (error == 0) {
		error = wk_drec(w, key, &kl, dir, name, len);
	}
	if (error) {
		goto out;
	}

	/* the first extra name: the existing one becomes a sibling too, and stays the primary */
	error = wsib_first(w, id, &sib, &sparent, sname, &snl);
	if (error == ENOENT) {
		struct wxf *nx = wi_xf(i, APFS_INO_EXT_TYPE_NAME);
		struct wdent cur;

		error = nx == NULL || nx->len == 0 ? EIO : 0;
		if (error == 0) {
			error = wd_lookup(w, w_le64(i->f + WINO_PARENT), (const char *)nx->data, nx->len - 1u, &cur);
		}
		if (error == 0) {
			sib = w->vol->next_obj_id++;
			error = wd_put(w, cur.key, cur.kl, id, cur.dtype, sib, cur.added);
		}
		if (error == 0) {
			error = wsib_put(w, id, sib, w_le64(i->f + WINO_PARENT), nx->data, nx->len);
		}
	}
	if (error == 0) {
		uint64_t mine = w->vol->next_obj_id++;
		uint8_t z[256];

		memcpy(z, name, len);
		z[len] = 0;
		error = wd_put(w, key, kl, id, wmode_dtype(wi_mode(i)), mine, apfs_now_ns());
		if (error == 0) {
			error = wsib_put(w, id, mine, dir, z, (uint16_t)(len + 1));
		}
	}
	if (error == 0) {
		wi_set_nlink(i, wi_nlink(i) + 1);
		wi_touch(i, false);
		error = wi_store(w, i);
	}
	if (error == 0) {
		error = wdir_adjust(w, dir, 1);
	}
	error = wop_end(w, error);
out:
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

/* ------------------------------------------------------------------ removing */

/* Every attribute of an object, and the streams of those kept out of line. */
static int
wx_drop_all(struct apfs_wtx *w, uint64_t id)
{
	uint8_t probe[8], k[WTX_KEYMAX], *v = apfs_alloc(WVAL_MAX);
	uint16_t kl, vl;
	int error = v == NULL ? ENOMEM : 0;

	while (error == 0) {
		wk_simple(probe, id, APFS_TYPE_XATTR);
		error = wr_next(w, &w->fs, id, APFS_TYPE_XATTR, probe, 8, false, k, &kl, v, &vl, WVAL_MAX);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error == 0 && vl >= 4 && (w_le16(v) & APFS_XATTR_DATA_STREAM) && vl >= 4 + 48) {
			uint64_t holes;

			error = we_cut(w, w_le64(v + 4), 0, &holes);
		}
		if (error == 0) {
			error = wbt_del(w, &w->fs, k, kl);
		}
	}
	if (v != NULL) {
		apfs_free(v, WVAL_MAX);
	}
	return error;
}

/* An object with no name left: everything it has goes. */
static int
wi_destroy(struct apfs_wtx *w, struct winode *i)
{
	uint8_t k[16], kk[WTX_KEYMAX];
	uint16_t kl;
	uint64_t holes, stream = w_le64(i->f + WINO_PRIVATE);
	int error = wx_drop_all(w, i->id);

	/* sibling records left over (an object can keep one with a single name) */
	while (error == 0) {
		uint8_t probe[16];

		wk_num(probe, i->id, APFS_TYPE_SIBLING_LINK, 0);
		error = wr_next(w, &w->fs, i->id, APFS_TYPE_SIBLING_LINK, probe, 16, false, kk, &kl, NULL, NULL, 0);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error == 0) {
			error = wsib_del(w, i->id, w_le64(kk + 8));
		}
	}
	if (error == 0 && wi_xf(i, APFS_INO_EXT_TYPE_DSTREAM) != NULL) {
		error = we_cut(w, stream, 0, &holes);
		if (error == 0) {
			error = wbt_del(w, &w->fs, k, wk_simple(k, stream, APFS_TYPE_DSTREAM_ID));
			if (error == ENOENT) {
				error = 0;
			}
		}
	}
	if (error == 0) {
		error = wbt_del(w, &w->fs, k, wk_simple(k, i->id, APFS_TYPE_INODE));
	}
	if (error == 0) {
		wcount(w, wi_mode(i), -1);
	}
	return error;
}

static int
worphan_key(struct apfs_wtx *w, uint64_t id, uint8_t *key, uint16_t *kl)
{
	char name[40];
	int n = snprintf(name, sizeof(name), "0x%llx-dead", (unsigned long long)id);

	return wk_drec(w, key, kl, APFS_PRIV_DIR_INO_NUM, name, (size_t)n);
}

/*
 * One name of i (the entry d, in dir) goes.  Its last name takes the object with it, or with
 * keep moves it to the private directory as an orphan.
 */
static int
wunlink(struct apfs_wtx *w, uint64_t dir, struct wdent *d, struct winode *i, bool keep, bool *gone)
{
	uint8_t sname[256];
	uint16_t snl;
	uint64_t sib, sparent;
	int error = wbt_del(w, &w->fs, d->key, d->kl);

	*gone = false;
	if (error == 0) {
		error = wdir_adjust(w, dir, -1);
	}
	if (error) {
		return error;
	}
	if (wi_isdir(i)) {
		*gone = true;
		return wi_destroy(w, i);
	}
	if (wi_nlink(i) > 1) {
		if (d->sibling != 0) {
			error = wsib_del(w, i->id, d->sibling);
		}
		/* the primary name may have been this one: the lowest sibling left is the primary now */
		if (error == 0) {
			error = wsib_first(w, i->id, &sib, &sparent, sname, &snl);
		}
		if (error == 0) {
			w_put_le64(i->f + WINO_PARENT, sparent);
			error = wi_xf_set(i, APFS_INO_EXT_TYPE_NAME, APFS_XF_DO_NOT_COPY, sname, snl);
		}
		if (error == 0) {
			wi_set_nlink(i, wi_nlink(i) - 1);
			wi_touch(i, false);
			error = wi_store(w, i);
		}
		return error;
	}
	if (!keep) {
		*gone = true;
		return wi_destroy(w, i);
	}
	/* still open: an orphan in the private directory, with no sibling records */
	uint8_t okey[WTX_KEYMAX];
	uint16_t okl;

	while (error == 0 && wsib_first(w, i->id, &sib, &sparent, sname, &snl) == 0) {
		error = wsib_del(w, i->id, sib);
	}
	if (error == 0) {
		error = worphan_key(w, i->id, okey, &okl);
	}
	if (error == 0) {
		error = wd_put(w, okey, okl, i->id, wmode_dtype(wi_mode(i)), 0, apfs_now_ns());
	}
	if (error == 0) {
		error = wdir_adjust(w, APFS_PRIV_DIR_INO_NUM, 1);
	}
	if (error == 0) {
		wi_set_nlink(i, 0);
		wi_touch(i, false);
		error = wi_store(w, i);
	}
	return error;
}

int
apfs_w_remove(struct apfs_wtx *w, uint64_t dir, const char *name, size_t len, bool want_dir,
    bool keep, uint64_t *id, bool *gone)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	struct wdent d;
	int error = i == NULL ? ENOMEM : 0;

	*gone = false;
	if (error == 0) {
		error = wop_begin(w, 0);
	}
	if (error == 0) {
		error = wd_lookup(w, dir, name, len, &d);
	}
	if (error == 0) {
		error = wi_load(w, d.id, i);
	}
	if (error == 0) {
		if (want_dir && !wi_isdir(i)) {
			error = ENOTDIR;
		} else if (!want_dir && wi_isdir(i)) {
			error = EISDIR;
		} else if (wi_isdir(i)) {
			bool any = false;

			error = wd_any(w, d.id, &any);
			if (error == 0 && (any || wi_nlink(i) != 0)) {
				error = ENOTEMPTY;
			}
		}
	}
	if (error == 0) {
		*id = d.id;
		error = wop_end(w, wunlink(w, dir, &d, i, keep, gone));
	}
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

/* The last close of an orphan: it goes, and its entry in the private directory with it. */
int
apfs_w_reap(struct apfs_wtx *w, uint64_t id)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	uint8_t key[WTX_KEYMAX];
	uint16_t kl;
	int error = i == NULL ? ENOMEM : 0;

	if (error == 0) {
		error = wop_begin(w, 0);
	}
	if (error == 0) {
		error = wi_load(w, id, i);
	}
	if (error == 0 && wi_nlink(i) != 0) {
		error = EBUSY;                  /* not an orphan */
	}
	if (error == 0) {
		error = worphan_key(w, id, key, &kl);
	}
	if (error == 0) {
		error = wbt_get(w, &w->fs, key, kl, NULL, NULL, WVAL_MAX);
	}
	if (error == 0) {
		error = wbt_del(w, &w->fs, key, kl);
		if (error == 0) {
			error = wdir_adjust(w, APFS_PRIV_DIR_INO_NUM, -1);
		}
		if (error == 0) {
			error = wi_destroy(w, i);
		}
		error = wop_end(w, error);
	}
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

int
apfs_w_reap_orphans(struct apfs_wtx *w)
{
	uint8_t probe[8], k[WTX_KEYMAX], v[64];
	uint16_t kl, vl;
	int error = 0;

	for (;;) {
		wk_simple(probe, APFS_PRIV_DIR_INO_NUM, APFS_TYPE_DIR_REC);
		error = wr_next(w, &w->fs, APFS_PRIV_DIR_INO_NUM, APFS_TYPE_DIR_REC, probe, 8, false, k, &kl, v, &vl,
		    sizeof(v));
		if (error == ENOENT) {
			return 0;
		}
		if (error) {
			return error;
		}
		if (vl < 8) {
			return EIO;
		}
		error = apfs_w_reap(w, w_le64(v));
		if (error) {
			return error;
		}
	}
}

/* ------------------------------------------------------------------ renaming */

/* Is `dir` the object `id` or somewhere below it? */
static int
wdir_below(struct apfs_wtx *w, uint64_t dir, uint64_t id, bool *below)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	int error = i == NULL ? ENOMEM : 0;

	*below = false;
	for (int depth = 0; error == 0 && depth < 4096; depth++) {
		if (dir == id) {
			*below = true;
			break;
		}
		if (dir == APFS_ROOT_DIR_INO_NUM || dir < APFS_MIN_USER_INO_NUM) {
			break;
		}
		error = wi_load(w, dir, i);
		if (error == 0) {
			dir = w_le64(i->f + WINO_PARENT);
		}
	}
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

/* An object now named (dir, name): its own record, or its sibling record if it has links. */
static int
wrename_name(struct apfs_wtx *w, struct winode *i, uint64_t sib, uint64_t dir, const char *name,
    size_t len)
{
	uint8_t z[256], sname[256];
	uint16_t snl;
	uint64_t first, fparent;
	int error = 0;

	memcpy(z, name, len);
	z[len] = 0;
	if (sib != 0) {
		error = wsib_put(w, i->id, sib, dir, z, (uint16_t)(len + 1));
		if (error == 0) {
			error = wsib_first(w, i->id, &first, &fparent, sname, &snl);
		}
		if (error == 0 && first == sib) {
			error = wi_set_name(i, dir, name, len);
		}
	} else {
		error = wi_set_name(i, dir, name, len);
	}
	if (error == 0) {
		wi_touch(i, false);
		error = wi_store(w, i);
	}
	return error;
}

int
apfs_w_rename(struct apfs_wtx *w, uint64_t fdir, const char *fname, size_t flen,
    uint64_t tdir, const char *tname, size_t tlen, uint32_t flags, bool keep_target,
    uint64_t *replaced, bool *replaced_gone)
{
	struct winode *fi = apfs_alloc(sizeof(*fi)), *ti = apfs_alloc(sizeof(*ti));
	struct wdent fd, td;
	uint8_t key[WTX_KEYMAX];
	uint16_t kl;
	bool have_target = false, below = false;
	int error = (fi == NULL || ti == NULL) ? ENOMEM : wname_ok(tname, tlen);

	*replaced = 0;
	*replaced_gone = false;
	if (error == 0) {
		error = wop_begin(w, 0);
	}
	if (error == 0) {
		error = wdir_check(w, tdir);
	}
	if (error == 0) {
		error = wd_lookup(w, fdir, fname, flen, &fd);
	}
	if (error == 0) {
		error = wi_load(w, fd.id, fi);
	}
	if (error == 0) {
		error = wd_lookup(w, tdir, tname, tlen, &td);
		have_target = error == 0;
		error = error == ENOENT ? 0 : error;
	}
	if (error == 0) {
		error = wk_drec(w, key, &kl, tdir, tname, tlen);
	}
	if (error == 0 && wi_isdir(fi) && fdir != tdir) {
		error = wdir_below(w, tdir, fd.id, &below);
		if (error == 0 && below) {
			error = EINVAL;
		}
	}
	if (error == 0 && have_target && (flags & APFS_WREN_EXCL)) {
		error = EEXIST;
	}
	if (error == 0 && !have_target && (flags & APFS_WREN_SWAP)) {
		error = ENOENT;
	}
	if (error == 0 && have_target) {
		error = wi_load(w, td.id, ti);
	}
	if (error) {
		goto out;
	}

	if (flags & APFS_WREN_SWAP) {
		if (td.id == fd.id) {
			goto out;
		}
		if (wi_isdir(ti) && tdir != fdir && wdir_below(w, fdir, td.id, &below) == 0 && below) {
			error = EINVAL;
			goto out;
		}
		/* each entry keeps its key and gets the other object, with that object's sibling id */
		const char *fn, *tn;
		size_t fnl, tnl;

		wd_name(w, &fd, &fn, &fnl);
		wd_name(w, &td, &tn, &tnl);
		error = wd_put(w, fd.key, fd.kl, td.id, td.dtype, td.sibling, fd.added);
		if (error == 0) {
			error = wd_put(w, td.key, td.kl, fd.id, fd.dtype, fd.sibling, td.added);
		}
		if (error == 0) {
			error = wrename_name(w, fi, fd.sibling, tdir, tn, tnl);
		}
		if (error == 0) {
			error = wrename_name(w, ti, td.sibling, fdir, fn, fnl);
		}
		if (error == 0 && fdir != tdir) {
			error = wdir_adjust(w, fdir, 0);
			if (error == 0) {
				error = wdir_adjust(w, tdir, 0);
			}
		}
		error = wop_end(w, error);
		goto out;
	}

	if (have_target && td.id == fd.id) {
		bool same_entry = fdir == tdir && td.kl == fd.kl && memcmp(td.key, fd.key, fd.kl) == 0;

		/* two names of one file (POSIX: nothing happens), or the very name it has */
		if (!same_entry || (kl == fd.kl && memcmp(key, fd.key, kl) == 0)) {
			goto out;
		}
		have_target = false;            /* the same entry spelled differently: it is renamed */
	}
	if (have_target) {
		if (wi_isdir(fi) && !wi_isdir(ti)) {
			error = ENOTDIR;
		} else if (!wi_isdir(fi) && wi_isdir(ti)) {
			error = EISDIR;
		} else if (wi_isdir(ti)) {
			bool any = false;

			error = wd_any(w, td.id, &any);
			if (error == 0 && (any || wi_nlink(ti) != 0)) {
				error = ENOTEMPTY;
			}
		}
		if (error) {
			goto out;
		}
		*replaced = td.id;
		error = wunlink(w, tdir, &td, ti, keep_target, replaced_gone);
	}
	if (error == 0) {
		error = wbt_del(w, &w->fs, fd.key, fd.kl);
	}
	if (error == 0) {
		error = wd_put(w, key, kl, fd.id, fd.dtype, fd.sibling, fd.added);
	}
	if (error == 0) {
		error = wrename_name(w, fi, fd.sibling, tdir, tname, tlen);
	}
	if (error == 0) {
		error = wdir_adjust(w, fdir, -1);
	}
	if (error == 0) {
		error = wdir_adjust(w, tdir, 1);
	}
	error = wop_end(w, error);
out:
	if (fi != NULL) {
		apfs_free(fi, sizeof(*fi));
	}
	if (ti != NULL) {
		apfs_free(ti, sizeof(*ti));
	}
	return error;
}

/* ------------------------------------------------------------------ attributes and sizes */

int
apfs_w_setattr(struct apfs_wtx *w, uint64_t id, const struct apfs_wattr *a)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	int error = i == NULL ? ENOMEM : wop_begin(w, 0);

	if (error == 0) {
		error = wi_load(w, id, i);
	}
	if (error == 0) {
		if (a->set & APFS_WA_MODE) {
			w_put_le16(i->f + WINO_MODE, (uint16_t)((wi_mode(i) & APFS_S_IFMT) | (a->mode & 07777)));
		}
		if (a->set & APFS_WA_UID) {
			w_put_le32(i->f + WINO_UID, a->uid);
		}
		if (a->set & APFS_WA_GID) {
			w_put_le32(i->f + WINO_GID, a->gid);
		}
		if (a->set & APFS_WA_FLAGS) {
			w_put_le32(i->f + WINO_BSDFLAGS, a->flags);
		}
		if (a->set & APFS_WA_PCLASS) {
			w_put_le32(i->f + WINO_PCLASS, a->pclass);
			wi_set_flag(i, APFS_INODE_PROT_CLASS_EXPLICIT, true);
		}
		if (a->set & APFS_WA_ATIME) {
			w_put_le64(i->f + WINO_ATIME, a->atime);
		}
		if (a->set & APFS_WA_MTIME) {
			w_put_le64(i->f + WINO_MTIME, a->mtime);
		}
		if (a->set & APFS_WA_CRTIME) {
			w_put_le64(i->f + WINO_CRTIME, a->crtime);
		}
		if (a->set & APFS_WA_IFLAGS) {
			w_put_le64(i->f + WINO_IFLAGS, (w_le64(i->f + WINO_IFLAGS) | a->iflags_on) & ~a->iflags_off);
		}
		w_put_le64(i->f + WINO_CTIME, (a->set & APFS_WA_CTIME) ? a->ctime : apfs_now_ns());
		error = wop_end(w, wi_store(w, i));
	}
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

/* A regular file's own stream, made the first time it needs one. */
static int
wi_stream(struct apfs_wtx *w, struct winode *i, bool make)
{
	uint8_t k[8], v[4];

	if (wi_xf(i, APFS_INO_EXT_TYPE_DSTREAM) != NULL || !make) {
		return 0;
	}
	w_put_le32(v, 1);
	int error = wbt_put(w, &w->fs, k, wk_simple(k, w_le64(i->f + WINO_PRIVATE), APFS_TYPE_DSTREAM_ID), v, 4);

	return error == 0 ? wi_set_dstream(i, 0, 0) : error;
}

/* Cut or grow a stream to `size` bytes; the inode's dstream and sparse fields follow. */
static int
wi_resize(struct apfs_wtx *w, struct winode *i, uint64_t size)
{
	uint64_t cur, alloced, holes = wi_sparse(i), cut_holes = 0, want;
	uint64_t stream = w_le64(i->f + WINO_PRIVATE);
	int error = wi_stream(w, i, size > 0);

	if (error || wi_xf(i, APFS_INO_EXT_TYPE_DSTREAM) == NULL) {
		return error;
	}
	wi_dstream(i, &cur, &alloced);
	want = (size + w->bs - 1) / w->bs * w->bs;
#ifndef KERNEL
	/*
	 * Bytes past the old end in its last block read as zeroes once the file is longer.  In the
	 * kernel the page cache owns file data and vnop_setattr / vnop_write zero that range through
	 * it; a write from here could cross a pageout of the same block.
	 */
	if (size > cur && cur % w->bs != 0) {
		struct wext e;

		error = we_find(w, stream, cur, &e);
		if (error == 0 && e.phys != 0) {
			uint8_t *blk = apfs_alloc(w->bs);
			uint64_t at = (e.phys + (cur - e.logical) / w->bs) * w->bs;

			error = blk == NULL ? ENOMEM : w->nx->dev.read(w->nx->dev.ctx, at, blk, w->bs);
			if (error == 0) {
				memset(blk + cur % w->bs, 0, w->bs - cur % w->bs);
				error = w->nx->dev.write(w->nx->dev.ctx, at, blk, w->bs);
			}
			if (blk != NULL) {
				apfs_free(blk, w->bs);
			}
		} else if (error == ENOENT) {
			error = 0;
		}
		if (error) {
			return error;
		}
	}
#endif
	if (want < alloced) {
		error = we_cut(w, stream, want, &cut_holes);
		holes -= cut_holes;
		alloced = want;
	} else if (want > alloced) {
		error = we_grow_hole(w, stream, &alloced, &holes, want);
	}
	if (error == 0) {
		error = wi_set_dstream(i, size, alloced);
	}
	if (error == 0) {
		error = wi_set_sparse(i, holes);
	}
	return error;
}

int
apfs_w_truncate(struct apfs_wtx *w, uint64_t id, uint64_t size)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	int error = i == NULL ? ENOMEM : wop_begin(w, 0);

	if (error == 0) {
		error = wi_load(w, id, i);
	}
	if (error == 0 && (wi_mode(i) & APFS_S_IFMT) != APFS_S_IFREG) {
		error = wi_isdir(i) ? EISDIR : EINVAL;
	}
	if (error == 0 && size >= (1ULL << 56)) {
		error = EFBIG;
	}
	if (error == 0) {
		error = wi_resize(w, i, size);
		if (error == 0) {
			wi_touch(i, true);
			error = wi_store(w, i);
		}
		error = wop_end(w, error);
	}
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

int
apfs_w_map(struct apfs_wtx *w, uint64_t id, uint64_t off, uint64_t len, bool alloc,
    uint64_t *phys, uint64_t *run)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	uint64_t size, alloced, holes, nb, got = 0, want = (len + (off % w->bs) + w->bs - 1) / w->bs;
	int error = i == NULL ? ENOMEM : wi_load(w, id, i);

	if (error == 0 && (wi_mode(i) & APFS_S_IFMT) != APFS_S_IFREG) {
		error = EINVAL;
	}
	if (error) {
		goto out;
	}
	wi_dstream(i, &size, &alloced);
	holes = wi_sparse(i);
	if (off >= alloced || !alloc) {
		/* past the stream, or only asked where: no change */
		if (off >= alloced) {
			*phys = 0;
			*run = len;
			goto out;
		}
		error = we_map(w, w_le64(i->f + WINO_PRIVATE), off, want, false, &alloced, &holes, &nb, &got);
	} else {
		error = wop_begin(w, want);
		if (error == 0) {
			error = we_map(w, w_le64(i->f + WINO_PRIVATE), off, want, true, &alloced, &holes, &nb, &got);
			if (error == 0) {
				error = wi_set_sparse(i, holes);
			}
			if (error == 0) {
				uint64_t s, a;

				wi_dstream(i, &s, &a);
				error = wi_set_dstream(i, s, alloced);
			}
			if (error == 0) {
				error = wi_store(w, i);
			}
			error = wop_end(w, error);
		}
	}
	if (error == 0) {
		uint64_t first = off % w->bs;

		*phys = nb;
		*run = got * w->bs - first;
	}
out:
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

#ifndef KERNEL
/* The host tool's file data (in the kernel the page cache writes it through vnop_blockmap). */
int
apfs_w_write(struct apfs_wtx *w, uint64_t id, uint64_t off, const void *buf, size_t len)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	uint8_t *blk = apfs_alloc(w->bs);
	uint64_t size, alloced, end = off + len;
	const uint8_t *src = buf;
	int error = (i == NULL || blk == NULL) ? ENOMEM : wop_begin(w, (len + w->bs - 1) / w->bs + 1);

	if (error == 0) {
		error = wi_load(w, id, i);
	}
	if (error == 0 && (wi_mode(i) & APFS_S_IFMT) != APFS_S_IFREG) {
		error = EINVAL;
	}
	if (error == 0 && end < off) {
		error = EFBIG;
	}
	if (error) {
		goto out;
	}
	wi_dstream(i, &size, &alloced);
	if (end > size) {
		/* the length first, with a hole to the end: then every block written is inside it */
		error = wi_resize(w, i, end);
		if (error == 0) {
			error = wi_store(w, i);
		}
		if (error) {
			error = wop_end(w, error);
			goto out;
		}
	}
	while (error == 0 && off < end) {
		uint64_t phys, run, in = off % w->bs, n;
		bool fresh;

		/* a block only partly written is mapped alone, so `fresh` is true of all it maps */
		uint64_t want = (in != 0 || end - off < w->bs) ? (w->bs - in < end - off ? w->bs - in : end - off) :
		    (end - off) / w->bs * w->bs;

		error = apfs_w_map(w, id, off, want, false, &phys, &run);
		fresh = error == 0 && phys == 0;
		if (error == 0 && fresh) {
			error = apfs_w_map(w, id, off, want, true, &phys, &run);
		}
		if (error) {
			break;
		}
		n = run < end - off ? run : end - off;
		if (in == 0 && n >= w->bs) {
			n -= n % w->bs;
			error = w->nx->dev.write(w->nx->dev.ctx, phys * w->bs, src, n);
		} else {
			uint64_t take = w->bs - in < end - off ? w->bs - in : end - off;

			/* part of a block: what is outside the write stays (zeroes in a hole or past the old end) */
			if (!fresh && off - in < size) {
				error = w->nx->dev.read(w->nx->dev.ctx, phys * w->bs, blk, w->bs);
			} else {
				memset(blk, 0, w->bs);
			}
			if (error == 0) {
				memcpy(blk + in, src, take);
				error = w->nx->dev.write(w->nx->dev.ctx, phys * w->bs, blk, w->bs);
			}
			n = take;
		}
		off += n;
		src += n;
	}
	if (error == 0) {
		error = wi_load(w, id, i);
		if (error == 0) {
			wi_touch(i, true);
			error = wi_store(w, i);
		}
		error = wop_end(w, error);
	}
out:
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	if (blk != NULL) {
		apfs_free(blk, w->bs);
	}
	return error;
}
#endif /* !KERNEL */

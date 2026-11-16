/*
 * Extended attributes and document ids of a writable volume (included by apfs_write.c after
 * apfs_wops.inc, whose wop_begin / wop_end and inode helpers these use).
 */
#pragma once

/* ------------------------------------------------------------------ extended attributes */

/* The inode flag an attribute stands for, kept in step with it (apfsck). */
static uint64_t
wx_flag(const char *name)
{
	if (strcmp(name, WXATTR_FINDER) == 0) {
		return APFS_INODE_HAS_FINDER_INFO;
	}
	if (strcmp(name, WXATTR_SECURITY) == 0) {
		return APFS_INODE_HAS_SECURITY_EA;
	}
	if (strcmp(name, WXATTR_RSRC) == 0) {
		return APFS_INODE_HAS_RSRC_FORK;
	}
	return 0;
}

static void
wx_set_flag(struct winode *i, const char *name, bool present)
{
	uint64_t flag = wx_flag(name);

	if (flag == 0) {
		return;
	}
	wi_set_flag(i, flag, present);
	if (flag == APFS_INODE_HAS_RSRC_FORK) {
		wi_set_flag(i, APFS_INODE_NO_RSRC_FORK, !present);
	}
}

int
apfs_w_setxattr(struct apfs_wtx *w, uint64_t id, const char *name, const void *data, size_t len,
    uint32_t flags)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	uint8_t key[WTX_KEYMAX], *v = apfs_alloc(WVAL_MAX);
	uint16_t kl, vl;
	size_t nl = name != NULL ? strnlen(name, 256) : 0;
	bool exists = false;
	int error = (i == NULL || v == NULL) ? ENOMEM : 0;

	if (error == 0 && (nl == 0 || nl > 127)) {
		error = nl == 0 ? EINVAL : ENAMETOOLONG;
	}
	if (error == 0 && (strcmp(name, WXATTR_SYMLINK) == 0 || strncmp(name, "com.apple.fs.", 13) == 0)) {
		error = EPERM;                  /* the file system's own */
	}
	if (error == 0 && strcmp(name, WXATTR_FINDER) == 0 && len != 32) {
		error = EINVAL;
	}
	if (error == 0) {
		error = wop_begin(w, (len + w->bs - 1) / w->bs);
	}
	if (error == 0) {
		error = wi_load(w, id, i);
	}
	if (error == 0) {
		kl = wk_xattr(key, id, name, nl);
		error = wbt_get(w, &w->fs, key, kl, v, &vl, WVAL_MAX);
		exists = error == 0;
		error = error == ENOENT ? 0 : error;
	}
	if (error == 0 && exists && (flags & APFS_WX_CREATE)) {
		error = EEXIST;
	}
	if (error == 0 && !exists && (flags & APFS_WX_REPLACE)) {
		error = ENOATTR;
	}
	if (error) {
		goto out;
	}

	/* the old value's own stream, if it had one, goes */
	if (exists && (w_le16(v) & APFS_XATTR_DATA_STREAM) && vl >= 4 + 48) {
		uint64_t holes;

		error = we_cut(w, w_le64(v + 4), 0, &holes);
	}
	if (error == 0 && len <= APFS_XATTR_MAX_EMBEDDED_SIZE) {
		w_put_le16(v, APFS_XATTR_DATA_EMBEDDED);
		w_put_le16(v + 2, (uint16_t)len);
		memcpy(v + 4, data, len);
		error = wbt_put(w, &w->fs, key, kl, v, (uint16_t)(4 + len));
	} else if (error == 0) {
		/* too long to embed: a stream of its own, written now to blocks nobody else has */
		uint64_t stream = w->vol->next_obj_id++, alloced = 0, holes = 0, done = 0;
		uint8_t *tail = apfs_alloc(w->bs);

		error = tail == NULL ? ENOMEM : 0;
		while (error == 0 && done < len) {
			uint64_t phys, got, n;

			error = we_map(w, stream, done, (len - done + w->bs - 1) / w->bs, true, &alloced, &holes,
			    &phys, &got);
			if (error) {
				break;
			}
			n = got * w->bs < len - done ? got * w->bs : len - done;
			if (n % w->bs == 0) {
				error = w->nx->dev.write(w->nx->dev.ctx, phys * w->bs, (const uint8_t *)data + done, n);
			} else {
				uint64_t whole = n - n % w->bs;

				if (whole > 0) {
					error = w->nx->dev.write(w->nx->dev.ctx, phys * w->bs, (const uint8_t *)data + done, whole);
				}
				if (error == 0) {
					memset(tail, 0, w->bs);
					memcpy(tail, (const uint8_t *)data + done + whole, n - whole);
					error = w->nx->dev.write(w->nx->dev.ctx, (phys + whole / w->bs) * w->bs, tail, w->bs);
				}
			}
			done += n;
		}
		if (tail != NULL) {
			apfs_free(tail, w->bs);
		}
		if (error == 0) {
			w_put_le16(v, APFS_XATTR_DATA_STREAM);
			w_put_le16(v + 2, 48);
			w_put_le64(v + 4, stream);
			memset(v + 12, 0, 40);
			w_put_le64(v + 12, len);
			w_put_le64(v + 20, alloced);
			error = wbt_put(w, &w->fs, key, kl, v, 4 + 48);
		}
	}
	if (error == 0) {
		wx_set_flag(i, name, true);
		wi_touch(i, false);
		error = wi_store(w, i);
	}
	error = wop_end(w, error);
out:
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	if (v != NULL) {
		apfs_free(v, WVAL_MAX);
	}
	return error;
}

int
apfs_w_rmxattr(struct apfs_wtx *w, uint64_t id, const char *name)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	uint8_t key[WTX_KEYMAX], *v = apfs_alloc(WVAL_MAX);
	uint16_t kl, vl;
	size_t nl = name != NULL ? strnlen(name, 256) : 0;
	int error = (i == NULL || v == NULL) ? ENOMEM : 0;

	if (error == 0 && (nl == 0 || nl > 127)) {
		error = nl == 0 ? EINVAL : ENAMETOOLONG;
	}
	if (error == 0 && strncmp(name, "com.apple.fs.", 13) == 0) {
		error = EPERM;
	}
	if (error == 0) {
		error = wop_begin(w, 0);
	}
	if (error == 0) {
		error = wi_load(w, id, i);
	}
	if (error == 0) {
		kl = wk_xattr(key, id, name, nl);
		error = wbt_get(w, &w->fs, key, kl, v, &vl, WVAL_MAX);
		error = error == ENOENT ? ENOATTR : error;
	}
	if (error == 0) {
		if ((w_le16(v) & APFS_XATTR_DATA_STREAM) && vl >= 4 + 48) {
			uint64_t holes;

			error = we_cut(w, w_le64(v + 4), 0, &holes);
		}
		if (error == 0) {
			error = wbt_del(w, &w->fs, key, kl);
		}
		if (error == 0) {
			wx_set_flag(i, name, false);
			wi_touch(i, false);
			error = wi_store(w, i);
		}
		error = wop_end(w, error);
	}
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	if (v != NULL) {
		apfs_free(v, WVAL_MAX);
	}
	return error;
}

int
apfs_w_docid(struct apfs_wtx *w, uint64_t id, uint32_t *docid)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	struct wxf *x;
	int error = i == NULL ? ENOMEM : wi_load(w, id, i);

	if (error == 0 && (x = wi_xf(i, APFS_INO_EXT_TYPE_DOCUMENT_ID)) != NULL && x->len == 4) {
		*docid = w_le32(x->data);
	} else if (error == 0) {
		uint32_t next = w_le32(w->vsb + VSB_NEXT_DOC_ID);
		uint8_t d[4];

		if (next < WDOC_ID_MIN) {
			next = WDOC_ID_MIN;
		}
		error = wop_begin(w, 0);
		if (error == 0) {
			w_put_le32(d, next);
			error = wi_xf_set(i, APFS_INO_EXT_TYPE_DOCUMENT_ID, 0, d, 4);
			if (error == 0) {
				error = wi_store(w, i);
			}
			if (error == 0) {
				w_put_le32(w->vsb + VSB_NEXT_DOC_ID, next + 1);
				*docid = next;
			}
			error = wop_end(w, error);
		}
	}
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

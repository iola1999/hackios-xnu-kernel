/*
 * The open transaction: its blocks, and the commit that makes them a checkpoint (included by
 * apfs_write.c; see apfs_wtx.h for the model).
 */
#pragma once

static inline uint32_t
wtx_bucket(uint64_t addr)
{
	return (uint32_t)((addr * 0x9E3779B97F4A7C15ULL) >> 40) & (WTX_HASH - 1);
}

static struct wblk *
wtx_lookup(struct apfs_wtx *w, uint64_t addr)
{
	struct wblk *b;

	if (w->hash == NULL) {
		return NULL;
	}
	for (b = w->hash[wtx_bucket(addr)]; b != NULL; b = b->next) {
		if (b->addr == addr) {
			return b;
		}
	}
	return NULL;
}

const uint8_t *
wtx_find(struct apfs_wtx *w, uint64_t addr)
{
	struct wblk *b = wtx_lookup(w, addr);

	return b != NULL ? b->data : NULL;
}

static const uint8_t *
wtx_overlay(void *ctx, uint64_t addr)
{
	return wtx_find((struct apfs_wtx *)ctx, addr);
}

static int
wtx_insert(struct apfs_wtx *w, uint64_t addr, bool raw, struct wblk **out)
{
	struct wblk *b = apfs_alloc(sizeof(*b));

	if (b == NULL) {
		return ENOMEM;
	}
	b->data = apfs_alloc(w->bs);
	if (b->data == NULL) {
		apfs_free(b, sizeof(*b));
		return ENOMEM;
	}
	b->addr = addr;
	b->raw = raw;
	b->next = w->hash[wtx_bucket(addr)];
	w->hash[wtx_bucket(addr)] = b;
	w->nblk++;
	*out = b;
	return 0;
}

static void
wtx_unlink(struct apfs_wtx *w, uint64_t addr)
{
	struct wblk **pp;

	for (pp = &w->hash[wtx_bucket(addr)]; *pp != NULL; pp = &(*pp)->next) {
		struct wblk *b = *pp;

		if (b->addr == addr) {
			*pp = b->next;
			apfs_free(b->data, w->bs);
			apfs_free(b, sizeof(*b));
			w->nblk--;
			return;
		}
	}
}

/* Everything the transaction holds in memory, dropped (after a commit, or to discard it). */
static void
wtx_drop_all(struct apfs_wtx *w)
{
	for (uint32_t i = 0; i < WTX_HASH && w->nblk > 0; i++) {
		while (w->hash[i] != NULL) {
			struct wblk *b = w->hash[i];

			w->hash[i] = b->next;
			apfs_free(b->data, w->bs);
			apfs_free(b, sizeof(*b));
			w->nblk--;
		}
	}
}

int
wtx_read(struct apfs_wtx *w, uint64_t addr, const uint8_t **blk)
{
	const uint8_t *mine = wtx_find(w, addr);

	if (mine != NULL) {
		*blk = mine;
		return 0;
	}
	return apfs_meta_block(w->nx, addr, blk);
}

/* The transaction begins with the first change: what a discard would put back is noted. */
int
wtx_begin(struct apfs_wtx *w)
{
	if (w->broken) {
		return EIO;
	}
	if (w->open) {
		return 0;
	}
	w->xid = w->nx->xid + 1;
	w->saved_vol = *w->vol;
	w->saved_nx_omap_root = w->nx->omap.tree.root;
	w->saved_next_oid = w->next_oid;
	memcpy(w->saved_vsb, w->vsb, w->bs);
	w->nx->view_xid = w->xid;
	w->open = true;
	return 0;
}

/* A new block for the transaction: allocated and zeroed in memory. */
int
wtx_new_block(struct apfs_wtx *w, bool vol_owned, uint64_t *addr, uint8_t **data)
{
	struct wblk *b;
	uint64_t got;
	int error = wtx_begin(w);

	if (error == 0) {
		error = wsp_alloc(w, 0, 1, addr, &got);
	}
	if (error) {
		return error;
	}
	error = wtx_insert(w, *addr, false, &b);
	if (error) {
		wsp_free_now(w, *addr, 1);
		return error;
	}
	if (vol_owned) {
		w->vol->alloc_count++;
	}
	*data = b->data;
	return 0;
}

/*
 * The block at addr, the transaction's own to change: itself if the transaction made it,
 * otherwise a copy at a new address (*new_addr) while the original is freed at commit.
 */
int
wtx_writable(struct apfs_wtx *w, uint64_t addr, bool vol_owned, uint64_t *new_addr, uint8_t **data)
{
	struct wblk *b = wtx_lookup(w, addr);
	const uint8_t *old;
	uint64_t naddr;
	uint8_t *ndata;
	int error;

	if (b != NULL) {
		*new_addr = addr;
		*data = b->data;
		return 0;
	}
	error = apfs_meta_block(w->nx, addr, &old);
	if (error) {
		return error;
	}
	error = wtx_new_block(w, vol_owned, &naddr, &ndata);
	if (error) {
		return error;
	}
	/* the cache may have handed old's memory to another block during the allocation */
	error = apfs_meta_block(w->nx, addr, &old);
	if (error) {
		return error;
	}
	memcpy(ndata, old, w->bs);
	error = wsp_free_later(w, addr, 1, vol_owned);
	if (error) {
		return error;
	}
	if (vol_owned) {
		w->vol->alloc_count--;
	}
	*new_addr = naddr;
	*data = ndata;
	return 0;
}

/* A block no longer referred to: free now if the transaction made it, else at commit. */
void
wtx_release(struct apfs_wtx *w, uint64_t addr, bool vol_owned)
{
	if (vol_owned) {
		w->vol->alloc_count--;
	}
	if (wtx_lookup(w, addr) != NULL) {
		wtx_unlink(w, addr);
		if (wsp_free_now(w, addr, 1) != 0) {
			wtx_poison(w, EIO);
		}
		return;
	}
	if (wsp_free_later(w, addr, 1, vol_owned) != 0) {
		wtx_poison(w, EIO);
	}
}

static int
wsp_ip_put(struct apfs_wtx *w, uint64_t addr, const uint8_t *data, bool raw)
{
	struct wblk *b = wtx_lookup(w, addr);
	int error = 0;

	if (b == NULL) {
		error = wtx_insert(w, addr, raw, &b);
	}
	if (error == 0) {
		memcpy(b->data, data, w->bs);
		b->raw = raw;
	}
	return error;
}

/*
 * A failure in the middle of a change: the transaction's blocks are thrown away, the volume
 * goes back to what the last checkpoint says, and nothing more is written on this mount.
 */
void
wtx_poison(struct apfs_wtx *w, int error)
{
	if (w->broken) {
		return;
	}
	apfs_log("write: transaction %llu failed (%d), discarded; the volume is read-only from now on\n",
	    (unsigned long long)w->xid, error);
	w->broken = true;
	if (w->open) {
		wtx_drop_all(w);
		*w->vol = w->saved_vol;
		w->nx->omap.tree.root = w->saved_nx_omap_root;
		w->next_oid = w->saved_next_oid;
		memcpy(w->vsb, w->saved_vsb, w->bs);
		w->nx->view_xid = 0;
		w->open = false;
		apfs_nx_cache_flush(w->nx);
		apfs_omap_cache_clear(w->nx);
	}
}

/*
 * Revert an open transaction only when it holds a single operation (w->ops == expected_ops)
 * and the caller verified it was closed beforehand: frees all dirty blocks back to bitmaps
 * and avoids leaving an empty checkpoint after a 0-byte write failure.
 */
bool
apfs_wtx_discard_solo(struct apfs_wtx *w, uint64_t expected_ops)
{
	if (w == NULL || w->broken || !w->open || w->ops != expected_ops) {
		return false;
	}
	for (uint32_t i = 0; i < WTX_HASH; i++) {
		for (struct wblk *b = w->hash[i]; b != NULL; b = b->next) {
			if (wsp_free_now(w, b->addr, 1) != 0) {
				wtx_poison(w, EIO);
				return false;
			}
		}
	}
	wtx_drop_all(w);
	w->sp.nfreed = 0;
	w->sp.nip_freed = 0;
	for (uint64_t c = 0; c < w->sp.chunk_count; c++) {
		w->sp.chunks[c].dirty = false;
	}
	for (uint32_t c = 0; c < w->sp.cib_count; c++) {
		w->sp.cib_dirty[c] = false;
	}
	*w->vol = w->saved_vol;
	w->nx->omap.tree.root = w->saved_nx_omap_root;
	w->next_oid = w->saved_next_oid;
	memcpy(w->vsb, w->saved_vsb, w->bs);
	w->fs.root = w->vol->tree.root;
	w->fs.root_oid = w->vol->tree.root_oid;
	w->omap.root = w->vol->omap.tree.root;
	w->omap.root_oid = w->vol->omap.tree.root;
	w->eref.root = w->vol->extentref_oid;
	w->eref.root_oid = w->vol->extentref_oid;
	w->nx_omap.root = w->nx->omap.tree.root;
	w->nx_omap.root_oid = w->nx->omap.tree.root;
	w->nx->view_xid = 0;
	w->open = false;
	apfs_nx_cache_flush(w->nx);
	apfs_omap_cache_clear(w->nx);
	return true;
}

/* ------------------------------------------------------------------ commit */

struct wwrite {
	uint64_t       addr;
	const uint8_t *data;
};

static int
wwrite_cmp(const void *a, const void *b)
{
	uint64_t x = ((const struct wwrite *)a)->addr, y = ((const struct wwrite *)b)->addr;

	return x < y ? -1 : (x > y ? 1 : 0);
}

static void
wtx_seal(uint8_t *blk, uint32_t bs)
{
	w_put_le64(blk, apfs_fletcher64(blk + 8, bs - 8));
}

/* Out to the device in address order, neighbours in one request. */
static int
wtx_write_all(struct apfs_wtx *w, struct wwrite *list, uint32_t n)
{
	struct apfs_nx *nx = w->nx;
	uint32_t max_run = 64, i = 0;
	uint8_t *run = apfs_alloc((size_t)max_run * w->bs);

	if (run == NULL) {
		return ENOMEM;
	}
	qsort(list, n, sizeof(*list), wwrite_cmp);
	while (i < n) {
		uint32_t k = 1;
		int error;

		memcpy(run, list[i].data, w->bs);
		while (i + k < n && k < max_run && list[i + k].addr == list[i].addr + k) {
			memcpy(run + (size_t)k * w->bs, list[i + k].data, w->bs);
			k++;
		}
		error = nx->dev.write(nx->dev.ctx, list[i].addr * (uint64_t)w->bs, run, (size_t)k * w->bs);
		if (error) {
			apfs_free(run, (size_t)max_run * w->bs);
			return error;
		}
		i += k;
	}
	apfs_free(run, (size_t)max_run * w->bs);
	return 0;
}

static int
wtx_flush(struct apfs_wtx *w)
{
	struct apfs_nx *nx = w->nx;

	return nx->dev.flush != NULL ? nx->dev.flush(nx->dev.ctx) : 0;
}

/* The volume's object map, superblock, and the container's map entry for it: new copies. */
static int
wtx_finish_volume(struct apfs_wtx *w)
{
	struct apfs_vol *vol = w->vol;
	uint8_t *data;
	uint64_t addr;
	const uint8_t *old;
	int error;

	error = wtx_read(w, w->vol_omap_obj, &old);
	if (error) {
		return error;
	}
	if (w_le64(old + 48) != w->omap.root) {
		error = wtx_new_block(w, true, &addr, &data);
		if (error) {
			return error;
		}
		error = wtx_read(w, w->vol_omap_obj, &old);
		if (error) {
			return error;
		}
		memcpy(data, old, w->bs);
		w_put_le64(data + 48, w->omap.root);
		w_put_le64(data + 8, addr);
		w_put_le64(data + 16, w->xid);
		wtx_release(w, w->vol_omap_obj, true);
		w->vol_omap_obj = addr;
	}

	uint8_t *v = w->vsb;

	w_put_le64(v + 88, vol->alloc_count);
	w_put_le64(v + 128, w->vol_omap_obj);
	w_put_le64(v + 144, w->eref.root);
	w_put_le64(v + 176, vol->next_obj_id);
	w_put_le64(v + 184, vol->num_files);
	w_put_le64(v + 192, vol->num_dirs);
	w_put_le64(v + 200, vol->num_symlinks);
	w_put_le64(v + 256, apfs_now_ns());
	error = wtx_new_block(w, false, &addr, &data);
	if (error) {
		return error;
	}
	memcpy(data, v, w->bs);
	w_put_le64(data + 8, vol->oid);
	w_put_le64(data + 16, w->xid);
	w_put_le32(data + 24, APFS_OBJ_VIRTUAL | APFS_OBJECT_TYPE_FS);
	w_put_le32(data + 28, 0);
	wtx_release(w, w->vsb_addr, false);
	w->vsb_addr = addr;
	error = womap_set(w, &w->nx_omap, vol->oid, addr);
	if (error) {
		return error;
	}

	error = wtx_read(w, w->nx_omap_obj, &old);
	if (error) {
		return error;
	}
	if (w_le64(old + 48) != w->nx_omap.root) {
		error = wtx_new_block(w, false, &addr, &data);
		if (error) {
			return error;
		}
		error = wtx_read(w, w->nx_omap_obj, &old);
		if (error) {
			return error;
		}
		memcpy(data, old, w->bs);
		w_put_le64(data + 48, w->nx_omap.root);
		w_put_le64(data + 8, addr);
		w_put_le64(data + 16, w->xid);
		wtx_release(w, w->nx_omap_obj, false);
		w->nx_omap_obj = addr;
	}
	return 0;
}

/* Fill in the checkpoint: the ephemeral objects into the data ring, its map and superblock. */
struct wckpt {
	uint8_t  *eph;                  /* ephemeral objects, one block each, in ring order */
	uint32_t  neph;
	uint32_t  data_index, data_next;
	uint8_t  *cpm;
	uint64_t  cpm_addr;
	uint64_t  sb_addr;
	uint32_t  desc_index;
};

static int
wtx_checkpoint(struct apfs_wtx *w, struct wckpt *ck)
{
	struct apfs_nx *nx = w->nx;
	uint8_t *oldmap = NULL;
	uint32_t count = 0;
	int error = 0;

	ck->eph = apfs_alloc((size_t)(w->bs / 40) * w->bs);
	ck->cpm = apfs_alloc(w->bs);
	oldmap = apfs_alloc(w->bs);
	if (ck->eph == NULL || ck->cpm == NULL || oldmap == NULL) {
		error = ENOMEM;
		goto out;
	}
	/* the spaceman first, then every other ephemeral object of the live checkpoint, re-stamped */
	memcpy(ck->eph, w->sp.sm, w->bs);
	ck->neph = 1;
	for (uint32_t i = 0; i + 1 < nx->xp_desc_len; i++) {
		uint64_t at = nx->xp_desc_base + (nx->xp_desc_index + i) % nx->xp_desc_blocks;

		error = nx->dev.read(nx->dev.ctx, at * (uint64_t)w->bs, oldmap, w->bs);
		if (error) {
			goto out;
		}
		count = w_le32(oldmap + 36);
		for (uint32_t m = 0; m < count && m < (w->bs - 40) / 40; m++) {
			const uint8_t *e = oldmap + 40 + 40 * m;
			uint8_t *dst;

			if ((w_le32(e) & APFS_OBJECT_TYPE_MASK) == APFS_OBJECT_TYPE_SPACEMAN) {
				continue;
			}
			if (w_le32(e + 8) != w->bs || ck->neph >= (w->bs - 40) / 40) {
				error = EOPNOTSUPP;
				goto out;
			}
			dst = ck->eph + (size_t)ck->neph * w->bs;
			error = nx->dev.read(nx->dev.ctx, w_le64(e + 32) * (uint64_t)w->bs, dst, w->bs);
			if (error) {
				goto out;
			}
			w_put_le64(dst + 16, w->xid);
			ck->neph++;
		}
	}
	if (ck->neph + nx->xp_data_len > nx->xp_data_blocks || 2 + nx->xp_desc_len > nx->xp_desc_blocks) {
		apfs_log("write: the checkpoint areas are too small for another checkpoint\n");
		error = ENOSPC;
		goto out;
	}
	ck->data_index = nx->xp_data_next % nx->xp_data_blocks;
	ck->desc_index = nx->xp_desc_next % nx->xp_desc_blocks;
	ck->cpm_addr = nx->xp_desc_base + ck->desc_index;
	ck->sb_addr = nx->xp_desc_base + (ck->desc_index + 1) % nx->xp_desc_blocks;

	memset(ck->cpm, 0, w->bs);
	w_put_le32(ck->cpm + 32, 1);           /* CHECKPOINT_MAP_LAST */
	w_put_le32(ck->cpm + 36, ck->neph);
	{
		uint32_t at = ck->data_index;

		/* the spaceman's mapping, then the others in the order the old map had them */
		for (uint32_t i = 0; i < ck->neph; i++) {
			uint8_t *e = ck->cpm + 40 + 40 * i, *obj = ck->eph + (size_t)i * w->bs;

			w_put_le32(e, w_le32(obj + 24));
			w_put_le32(e + 4, w_le32(obj + 28));
			w_put_le32(e + 8, w->bs);
			w_put_le64(e + 16, 0);
			w_put_le64(e + 24, w_le64(obj + 8));
			w_put_le64(e + 32, nx->xp_data_base + at);
			wtx_seal(obj, w->bs);
			at = (at + 1) % nx->xp_data_blocks;
		}
		ck->data_next = at;
	}
	w_put_le64(ck->cpm + 8, ck->cpm_addr);
	w_put_le64(ck->cpm + 16, w->xid);
	w_put_le32(ck->cpm + 24, APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_CHECKPOINT_MAP);
	w_put_le32(ck->cpm + 28, 0);
	wtx_seal(ck->cpm, w->bs);

	uint8_t *sb = w->nxsb;

	w_put_le64(sb + 88, w->next_oid);
	w_put_le64(sb + 96, w->xid + 1);
	w_put_le32(sb + 128, (ck->desc_index + 2) % nx->xp_desc_blocks);
	w_put_le32(sb + 132, ck->data_next);
	w_put_le32(sb + 136, ck->desc_index);
	w_put_le32(sb + 140, 2);
	w_put_le32(sb + 144, ck->data_index);
	w_put_le32(sb + 148, ck->neph);
	w_put_le64(sb + 160, w->nx_omap_obj);
	w_put_le64(sb + 8, APFS_OID_NX_SUPERBLOCK);
	w_put_le64(sb + 16, w->xid);
	w_put_le32(sb + 24, APFS_OBJ_EPHEMERAL | APFS_OBJECT_TYPE_NX_SUPERBLOCK);
	w_put_le32(sb + 28, 0);
	wtx_seal(sb, w->bs);
out:
	if (oldmap != NULL) {
		apfs_free(oldmap, w->bs);
	}
	return error;
}

/*
 * Make the open transaction a checkpoint.  The order on the device: every new block and the
 * checkpoint's objects and map; a flush; the container superblock in the descriptor ring,
 * which is what makes them live; a flush; the copy of it in block zero.
 */
int
apfs_wtx_commit(struct apfs_wtx *w)
{
	struct apfs_nx *nx = w->nx;
	struct wckpt ck;
	struct wwrite *list = NULL;
	uint32_t n = 0, cap;
	uint64_t t0 = apfs_ticks();
	int error;

	if (w->broken) {
		return EIO;
	}
	if (!w->open) {
		return 0;
	}
	memset(&ck, 0, sizeof(ck));
	error = wtx_finish_volume(w);
	if (error == 0) {
		error = wsp_commit(w);
	}
	if (error == 0) {
		error = wtx_checkpoint(w, &ck);
	}
	if (error) {
		goto fail;
	}
	cap = w->nblk + ck.neph + 1;
	list = apfs_alloc((size_t)cap * sizeof(*list));
	if (list == NULL) {
		error = ENOMEM;
		goto fail;
	}
	for (uint32_t i = 0; i < WTX_HASH; i++) {
		for (struct wblk *b = w->hash[i]; b != NULL; b = b->next) {
			if (!b->raw) {
				wtx_seal(b->data, w->bs);
			}
			list[n].addr = b->addr;
			list[n].data = b->data;
			n++;
		}
	}
	for (uint32_t i = 0; i < ck.neph; i++) {
		uint32_t at = (ck.data_index + i) % nx->xp_data_blocks;

		list[n].addr = nx->xp_data_base + at;
		list[n].data = ck.eph + (size_t)i * w->bs;
		n++;
	}
	list[n].addr = ck.cpm_addr;
	list[n].data = ck.cpm;
	n++;
	error = wtx_write_all(w, list, n);
	if (error == 0) {
		error = wtx_flush(w);
	}
	if (error == 0) {
		error = nx->dev.write(nx->dev.ctx, ck.sb_addr * (uint64_t)w->bs, w->nxsb, w->bs);
	}
	if (error == 0) {
		error = wtx_flush(w);
	}
	if (error) {
		goto fail;
	}
	/* live now; block zero is only where a reader starts looking */
	(void)nx->dev.write(nx->dev.ctx, 0, w->nxsb, w->bs);

	for (uint32_t i = 0; i < n; i++) {
		apfs_nx_cache_forget(nx, list[i].addr);
	}
	apfs_nx_cache_forget(nx, ck.sb_addr);
	apfs_omap_cache_clear(nx);
	nx->xid = w->xid;
	nx->next_oid = w->next_oid;
	nx->omap_addr = w->nx_omap_obj;
	nx->omap.omap_paddr = w->nx_omap_obj;
	nx->omap.tree.root = w->nx_omap.root;
	nx->xp_desc_next = (ck.desc_index + 2) % nx->xp_desc_blocks;
	nx->xp_desc_index = ck.desc_index;
	nx->xp_desc_len = 2;
	nx->xp_data_next = ck.data_next;
	nx->xp_data_index = ck.data_index;
	nx->xp_data_len = ck.neph;
	w->vol->omap.omap_paddr = w->vol_omap_obj;
	nx->view_xid = 0;
	w->commit_blocks += n + 1;
	wtx_drop_all(w);
	w->open = false;
	w->commits++;
	w->commit_ticks += apfs_ticks() - t0;
	apfs_free(list, (size_t)cap * sizeof(*list));
	apfs_free(ck.eph, (size_t)(w->bs / 40) * w->bs);
	apfs_free(ck.cpm, w->bs);
	return 0;
fail:
	if (list != NULL) {
		apfs_free(list, (size_t)cap * sizeof(*list));
	}
	if (ck.eph != NULL) {
		apfs_free(ck.eph, (size_t)(w->bs / 40) * w->bs);
	}
	if (ck.cpm != NULL) {
		apfs_free(ck.cpm, w->bs);
	}
	wtx_poison(w, error);
	return error;
}

/* ------------------------------------------------------------------ attach */

static int wfs_cmp(const struct wtree *t, const uint8_t *a, uint16_t al, const uint8_t *b, uint16_t bl);
static int womap_cmp(const struct wtree *t, const uint8_t *a, uint16_t al, const uint8_t *b, uint16_t bl);

/*
 * Ready a mounted volume for writing: the live superblocks, the space manager, and the four
 * trees a transaction changes.  The read path is pointed at the overlay from here on.
 */
int
apfs_wtx_attach(struct apfs_vol *vol, struct apfs_wtx **out)
{
	struct apfs_nx *nx = vol->nx;
	struct apfs_wtx *w;
	const char *why = NULL;
	const uint8_t *blk;
	int error;

	error = apfs_write_volume_ok(vol, &why);
	if (error) {
		apfs_log("write: volume %s is not writable: %s\n", vol->name, why);
		return error;
	}
	if (nx->block_size != 4096 || nx->xp_desc_blocks < 4 || nx->xp_data_blocks < 4 ||
	    vol->has_fext || (vol->fs_flags & APFS_FS_UNENCRYPTED) == 0) {
		return EOPNOTSUPP;
	}
	w = apfs_alloc(sizeof(*w));
	if (w == NULL) {
		return ENOMEM;
	}
	w->nx = nx;
	w->vol = vol;
	w->bs = nx->block_size;
	w->next_oid = nx->next_oid;
	w->hash = apfs_alloc(WTX_HASH * sizeof(*w->hash));
	w->vsb = apfs_alloc(w->bs);
	w->saved_vsb = apfs_alloc(w->bs);
	w->nxsb = apfs_alloc(w->bs);
	if (w->hash == NULL || w->vsb == NULL || w->saved_vsb == NULL || w->nxsb == NULL) {
		error = ENOMEM;
		goto fail;
	}
	/* the live container superblock: the last block of the live checkpoint's descriptors */
	{
		uint64_t at = nx->xp_desc_base + (nx->xp_desc_index + nx->xp_desc_len - 1) % nx->xp_desc_blocks;

		error = nx->dev.read(nx->dev.ctx, at * (uint64_t)w->bs, w->nxsb, w->bs);
		if (error) {
			goto fail;
		}
		if (!apfs_checksum_ok(w->nxsb, w->bs) || w_le64(w->nxsb + 16) != nx->xid ||
		    (w_le32(w->nxsb + 24) & APFS_OBJECT_TYPE_MASK) != APFS_OBJECT_TYPE_NX_SUPERBLOCK) {
			apfs_log("write: the live checkpoint's superblock is not where its descriptors say\n");
			error = EIO;
			goto fail;
		}
	}
	w->nx_omap_obj = w_le64(w->nxsb + 160);
	w->vol_omap_obj = vol->omap.omap_paddr;
	error = apfs_omap_lookup(&nx->omap, vol->oid, nx->xid, &w->vsb_addr);
	if (error == 0) {
		error = wtx_read(w, w->vsb_addr, &blk);
	}
	if (error) {
		goto fail;
	}
	memcpy(w->vsb, blk, w->bs);

	w->fs = (struct wtree){ .subtype = APFS_OBJECT_TYPE_FSTREE, .virt = true, .vol_owned = true,
		                .hashed = vol->hashed_names,
		                .root = vol->tree.root, .root_oid = vol->tree.root_oid, .cmp = wfs_cmp };
	w->omap = (struct wtree){ .subtype = APFS_OBJECT_TYPE_OMAP, .fixed = true, .vol_owned = true,
		                  .root = vol->omap.tree.root, .root_oid = vol->omap.tree.root, .cmp = womap_cmp };
	w->eref = (struct wtree){ .subtype = APFS_OBJECT_TYPE_BLOCKREFTREE, .vol_owned = true,
		                  .root = vol->extentref_oid, .root_oid = vol->extentref_oid, .cmp = wfs_cmp };
	w->nx_omap = (struct wtree){ .subtype = APFS_OBJECT_TYPE_OMAP, .fixed = true,
		                     .root = nx->omap.tree.root, .root_oid = nx->omap.tree.root, .cmp = womap_cmp };
	error = wsp_load(w);
	if (error) {
		goto fail;
	}
	nx->overlay = wtx_overlay;
	nx->overlay_ctx = w;
	nx->writable = true;
	*out = w;
	return 0;
fail:
	apfs_wtx_detach(w);
	return error;
}

/* The transaction must have been committed (or the mount be broken). */
void
apfs_wtx_detach(struct apfs_wtx *w)
{
	if (w == NULL) {
		return;
	}
	if (w->nx != NULL && w->nx->overlay_ctx == w) {
		w->nx->overlay = NULL;
		w->nx->overlay_ctx = NULL;
		w->nx->writable = false;
		w->nx->view_xid = 0;
	}
	if (w->hash != NULL) {
		wtx_drop_all(w);
		apfs_free(w->hash, WTX_HASH * sizeof(*w->hash));
	}
	wsp_unload(w);
	wbt_free_scratch(w, &w->fs);
	wbt_free_scratch(w, &w->omap);
	wbt_free_scratch(w, &w->eref);
	wbt_free_scratch(w, &w->nx_omap);
	if (w->vsb != NULL) {
		apfs_free(w->vsb, w->bs);
	}
	if (w->saved_vsb != NULL) {
		apfs_free(w->saved_vsb, w->bs);
	}
	if (w->nxsb != NULL) {
		apfs_free(w->nxsb, w->bs);
	}
	apfs_free(w, sizeof(*w));
}

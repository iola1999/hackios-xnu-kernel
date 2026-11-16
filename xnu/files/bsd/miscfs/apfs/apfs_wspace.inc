/*
 * The space manager as a transaction uses it (included by apfs_write.c).
 *
 * Every chunk of 8 * block_size blocks has a bitmap block in the internal pool; chunk-info
 * blocks (CIBs) say where each bitmap is and how many blocks are free, and the spaceman
 * object lists the CIBs (or, on a container with more of them than fit, cib-address blocks
 * that do).  At commit every changed bitmap and CIB goes to a fresh internal-pool block, the
 * internal pool's own bitmap to the next slot of its ring, and the spaceman object into the
 * checkpoint.  The old copies stay intact until the new checkpoint is live.
 *
 * Bitmaps are read when first needed and then kept for the life of the mount (one block per
 * 128 MiB of a 4 KiB-block container).  Blocks freed by the open transaction stay set in the
 * bitmaps until the commit, so nothing the live checkpoint uses is handed out again.
 *
 * The free queues are left as they are: this code frees blocks straight into the bitmaps at
 * the commit that frees them, which is safe because only the newest checkpoint is ever
 * mounted.  A container whose internal-pool bitmap takes more than one block, or whose
 * spaceman object more than one block, is refused.
 */
#pragma once

#define SM_CHUNK_COUNT   56
#define SM_CIB_COUNT     64
#define SM_CAB_COUNT     68
#define SM_FREE_COUNT    72
#define SM_ADDR_OFFSET   80
#define SM_IP_BLOCKS     152
#define SM_IP_BM_SIZE    160
#define SM_IP_BM_BLOCKS  164
#define SM_IP_BM_BASE    168
#define SM_IP_BASE       176
#define SM_IP_FREE_HEAD  320
#define SM_IP_FREE_TAIL  322
#define SM_IP_XID_OFF    324
#define SM_IP_BITMAP_OFF 328
#define SM_IP_NEXT_OFF   332

static int
wsp_read(struct apfs_wtx *w, uint64_t addr, uint8_t *buf, bool object)
{
	const uint8_t *blk;
	int error;

	if (object) {
		error = wtx_read(w, addr, &blk);
		if (error == 0) {
			memcpy(buf, blk, w->bs);
		}
		return error;
	}
	return w->nx->dev.read(w->nx->dev.ctx, addr * (uint64_t)w->bs, buf, w->bs);
}

/* The spaceman object of the live checkpoint, and everything it points to but the bitmaps. */
int
wsp_load(struct apfs_wtx *w)
{
	struct wspace *sp = &w->sp;
	struct apfs_nx *nx = w->nx;
	uint8_t *cpm = NULL, *blk = NULL;
	uint64_t sm_addr = 0;
	int error = 0;

	memset(sp, 0, sizeof(*sp));
	cpm = apfs_alloc(w->bs);
	blk = apfs_alloc(w->bs);
	if (cpm == NULL || blk == NULL) {
		error = ENOMEM;
		goto out;
	}
	/* the checkpoint map of the live checkpoint: the blocks before its superblock */
	for (uint32_t i = 0; i + 1 < nx->xp_desc_len && sm_addr == 0; i++) {
		uint64_t at = nx->xp_desc_base + (nx->xp_desc_index + i) % nx->xp_desc_blocks;
		uint32_t count;

		error = nx->dev.read(nx->dev.ctx, at * (uint64_t)w->bs, cpm, w->bs);
		if (error) {
			goto out;
		}
		if (!apfs_checksum_ok(cpm, w->bs) ||
		    (w_le32(cpm + 24) & APFS_OBJECT_TYPE_MASK) != APFS_OBJECT_TYPE_CHECKPOINT_MAP) {
			error = EIO;
			goto out;
		}
		count = w_le32(cpm + 36);
		if (count > (w->bs - 40) / 40) {
			error = EIO;
			goto out;
		}
		for (uint32_t m = 0; m < count; m++) {
			const uint8_t *e = cpm + 40 + 40 * m;

			if ((w_le32(e) & APFS_OBJECT_TYPE_MASK) == APFS_OBJECT_TYPE_SPACEMAN) {
				sp->sm_size = w_le32(e + 8);
				sp->sm_oid = w_le64(e + 24);
				sm_addr = w_le64(e + 32);
			}
		}
	}
	if (sm_addr == 0) {
		error = EIO;
		goto out;
	}
	if (sp->sm_size != w->bs) {
		apfs_log("write: a spaceman object of %u bytes is not handled\n", sp->sm_size);
		error = EOPNOTSUPP;
		goto out;
	}
	sp->sm = apfs_alloc(w->bs);
	if (sp->sm == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = nx->dev.read(nx->dev.ctx, sm_addr * (uint64_t)w->bs, sp->sm, w->bs);
	if (error) {
		goto out;
	}
	if (!apfs_checksum_ok(sp->sm, w->bs) || w_le32(sp->sm + 32) != w->bs) {
		error = EIO;
		goto out;
	}
	sp->per_chunk = w_le32(sp->sm + 36);
	sp->per_cib = w_le32(sp->sm + 40);
	sp->per_cab = w_le32(sp->sm + 44);
	sp->block_count = w_le64(sp->sm + 48);
	sp->chunk_count = w_le64(sp->sm + SM_CHUNK_COUNT);
	sp->cib_count = w_le32(sp->sm + SM_CIB_COUNT);
	sp->cab_count = w_le32(sp->sm + SM_CAB_COUNT);
	sp->free_count = w_le64(sp->sm + SM_FREE_COUNT);
	sp->ip_blocks = w_le64(sp->sm + SM_IP_BLOCKS);
	sp->ip_bm_size = w_le32(sp->sm + SM_IP_BM_SIZE);
	sp->ip_bm_blocks = w_le32(sp->sm + SM_IP_BM_BLOCKS);
	sp->ip_bm_base = w_le64(sp->sm + SM_IP_BM_BASE);
	sp->ip_base = w_le64(sp->sm + SM_IP_BASE);
	if (sp->per_chunk != w->bs * 8 || sp->per_cib == 0 || sp->per_cib > (w->bs - 40) / 32 ||
	    sp->block_count > nx->block_count || sp->chunk_count == 0 ||
	    sp->chunk_count != (sp->block_count + sp->per_chunk - 1) / sp->per_chunk ||
	    sp->cib_count != (sp->chunk_count + sp->per_cib - 1) / sp->per_cib ||
	    sp->ip_bm_size != 1 || sp->ip_blocks > (uint64_t)w->bs * 8 || sp->ip_bm_blocks == 0 ||
	    w_le32(sp->sm + SM_ADDR_OFFSET) > w->bs - 8 ||
	    w_le32(sp->sm + SM_IP_NEXT_OFF) > w->bs - 2 * sp->ip_bm_blocks ||
	    w_le32(sp->sm + SM_IP_BITMAP_OFF) > w->bs - 2 || w_le32(sp->sm + SM_IP_XID_OFF) > w->bs - 8 ||
	    w_le16(sp->sm + SM_IP_FREE_HEAD) >= sp->ip_bm_blocks ||
	    w_le16(sp->sm + SM_IP_FREE_TAIL) >= sp->ip_bm_blocks) {
		apfs_log("write: a space manager this code does not handle\n");
		error = EOPNOTSUPP;
		goto out;
	}
	sp->chunks = apfs_alloc(sp->chunk_count * sizeof(*sp->chunks));
	sp->cib_addr = apfs_alloc(sp->cib_count * sizeof(*sp->cib_addr));
	sp->cib_dirty = apfs_alloc(sp->cib_count * sizeof(*sp->cib_dirty));
	if (sp->chunks == NULL || sp->cib_addr == NULL || sp->cib_dirty == NULL) {
		error = ENOMEM;
		goto out;
	}
	if (sp->cab_count == 0) {
		uint32_t off = w_le32(sp->sm + SM_ADDR_OFFSET);

		if ((uint64_t)off + 8ull * sp->cib_count > w->bs) {
			error = EIO;
			goto out;
		}
		for (uint32_t i = 0; i < sp->cib_count; i++) {
			sp->cib_addr[i] = w_le64(sp->sm + off + 8 * i);
		}
	} else {
		uint32_t off = w_le32(sp->sm + SM_ADDR_OFFSET), at = 0;

		if (sp->cab_count != (sp->cib_count + sp->per_cab - 1) / sp->per_cab ||
		    (uint64_t)off + 8ull * sp->cab_count > w->bs) {
			error = EIO;
			goto out;
		}
		sp->cab_addr = apfs_alloc(sp->cab_count * sizeof(*sp->cab_addr));
		if (sp->cab_addr == NULL) {
			error = ENOMEM;
			goto out;
		}
		for (uint32_t c = 0; c < sp->cab_count; c++) {
			uint32_t n;

			sp->cab_addr[c] = w_le64(sp->sm + off + 8 * c);
			error = wsp_read(w, sp->cab_addr[c], blk, true);
			if (error) {
				goto out;
			}
			n = w_le32(blk + 36);
			if (w_le32(blk + 32) != c || n > (w->bs - 40) / 8 || at + n > sp->cib_count) {
				error = EIO;
				goto out;
			}
			for (uint32_t i = 0; i < n; i++) {
				sp->cib_addr[at++] = w_le64(blk + 40 + 8 * i);
			}
		}
		if (at != sp->cib_count) {
			error = EIO;
			goto out;
		}
	}
	for (uint32_t c = 0, ci = 0; c < sp->cib_count; c++) {
		uint32_t n;

		error = wsp_read(w, sp->cib_addr[c], blk, true);
		if (error) {
			goto out;
		}
		n = w_le32(blk + 36);
		if (!apfs_checksum_ok(blk, w->bs) || w_le32(blk + 32) != c || n > sp->per_cib ||
		    ci + n > sp->chunk_count) {
			error = EIO;
			goto out;
		}
		for (uint32_t i = 0; i < n; i++, ci++) {
			const uint8_t *e = blk + 40 + 32 * i;
			struct wchunk *ch = &sp->chunks[ci];

			ch->xid = w_le64(e);
			ch->addr = w_le64(e + 8);
			ch->blocks = w_le32(e + 16);
			ch->free = w_le32(e + 20);
			ch->bitmap_addr = w_le64(e + 24);
			if (ch->addr != (uint64_t)ci * sp->per_chunk || ch->blocks > sp->per_chunk ||
			    ch->free > ch->blocks || ch->addr + ch->blocks > sp->block_count) {
				error = EIO;
				goto out;
			}
		}
		if (c + 1 == sp->cib_count && ci != sp->chunk_count) {
			error = EIO;
			goto out;
		}
	}
	sp->ip_bm = apfs_alloc(w->bs);
	if (sp->ip_bm == NULL) {
		error = ENOMEM;
		goto out;
	}
	{
		uint16_t slot = w_le16(sp->sm + w_le32(sp->sm + SM_IP_BITMAP_OFF));

		if (slot >= sp->ip_bm_blocks) {
			error = EIO;
			goto out;
		}
		error = wsp_read(w, sp->ip_bm_base + slot, sp->ip_bm, false);
	}
out:
	if (cpm != NULL) {
		apfs_free(cpm, w->bs);
	}
	if (blk != NULL) {
		apfs_free(blk, w->bs);
	}
	if (error) {
		wsp_unload(w);
	}
	return error;
}

void
wsp_unload(struct apfs_wtx *w)
{
	struct wspace *sp = &w->sp;

	if (sp->chunks != NULL) {
		for (uint64_t i = 0; i < sp->chunk_count; i++) {
			if (sp->chunks[i].bitmap != NULL) {
				apfs_free(sp->chunks[i].bitmap, w->bs);
			}
		}
		apfs_free(sp->chunks, sp->chunk_count * sizeof(*sp->chunks));
	}
	if (sp->cib_addr != NULL) {
		apfs_free(sp->cib_addr, sp->cib_count * sizeof(*sp->cib_addr));
	}
	if (sp->cib_dirty != NULL) {
		apfs_free(sp->cib_dirty, sp->cib_count * sizeof(*sp->cib_dirty));
	}
	if (sp->cab_addr != NULL) {
		apfs_free(sp->cab_addr, sp->cab_count * sizeof(*sp->cab_addr));
	}
	if (sp->sm != NULL) {
		apfs_free(sp->sm, w->bs);
	}
	if (sp->ip_bm != NULL) {
		apfs_free(sp->ip_bm, w->bs);
	}
	if (sp->freed != NULL) {
		apfs_free(sp->freed, sp->freed_cap * sizeof(*sp->freed));
	}
	if (sp->ip_freed != NULL) {
		apfs_free(sp->ip_freed, sp->ip_freed_cap * sizeof(*sp->ip_freed));
	}
	memset(sp, 0, sizeof(*sp));
}

static int
wsp_bitmap(struct apfs_wtx *w, struct wchunk *ch)
{
	if (ch->bitmap != NULL) {
		return 0;
	}
	ch->bitmap = apfs_alloc(w->bs);
	if (ch->bitmap == NULL) {
		return ENOMEM;
	}
	if (ch->bitmap_addr == 0) {
		return 0;                       /* no bitmap yet: every block free */
	}
	int error = wsp_read(w, ch->bitmap_addr, ch->bitmap, false);

	if (error) {
		apfs_free(ch->bitmap, w->bs);
		ch->bitmap = NULL;
	}
	return error;
}

static inline bool
bit_get(const uint8_t *bm, uint32_t i)
{
	return (bm[i >> 3] >> (i & 7)) & 1;
}

static inline void
bit_set(uint8_t *bm, uint32_t i, bool on)
{
	if (on) {
		bm[i >> 3] |= (uint8_t)(1u << (i & 7));
	} else {
		bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
	}
}

static void
wsp_mark(struct apfs_wtx *w, struct wchunk *ch, uint32_t from, uint32_t count, bool used)
{
	for (uint32_t i = from; i < from + count; i++) {
		bit_set(ch->bitmap, i, used);
	}
	if (used) {
		ch->free -= count;
		w->sp.free_count -= count;
	} else {
		ch->free += count;
		w->sp.free_count += count;
	}
	ch->dirty = true;
	w->sp.cib_dirty[(ch - w->sp.chunks) / w->sp.per_cib] = true;
}

/* The first free bit at or after `from` in a chunk's bitmap, or blocks if none. */
static uint32_t
wsp_first_free(const uint8_t *bm, uint32_t from, uint32_t blocks)
{
	uint32_t i = from;

	while (i < blocks && (i & 63) != 0) {
		if (!bit_get(bm, i)) {
			return i;
		}
		i++;
	}
	for (; i + 64 <= blocks; i += 64) {
		uint64_t word;

		memcpy(&word, bm + i / 8, 8);   /* little-endian, as the bitmap is */
		if (word != UINT64_MAX) {
			return i + (uint32_t)__builtin_ctzll(~word);
		}
	}
	for (; i < blocks; i++) {
		if (!bit_get(bm, i)) {
			return i;
		}
	}
	return blocks;
}

/*
 * Blocks just given out may still be in the metadata cache from what they held before they
 * were freed; a stream read through it (an attribute's, apfs_read_stream) would get that.
 */
static void
wsp_taken(struct apfs_wtx *w, uint64_t addr, uint64_t count)
{
	for (uint64_t i = 0; i < count; i++) {
		apfs_nx_cache_forget(w->nx, addr + i);
	}
}

/*
 * Up to `want` free blocks in a row, the first run found from the hint on: *addr, *got.
 * A run does not cross from one chunk into the next.
 */
int
wsp_alloc(struct apfs_wtx *w, uint64_t goal, uint64_t want, uint64_t *addr, uint64_t *got)
{
	struct wspace *sp = &w->sp;

	if (want == 0 || sp->free_count == 0) {
		return ENOSPC;
	}
	/* right at the goal (where the caller's last run ended), if that block is free */
	if (goal != 0 && goal < sp->block_count) {
		struct wchunk *ch = &sp->chunks[goal / sp->per_chunk];
		uint32_t at = (uint32_t)(goal - ch->addr), run = 0;

		if (at < ch->blocks && ch->free > 0 && wsp_bitmap(w, ch) == 0) {
			while (run < want && at + run < ch->blocks && !bit_get(ch->bitmap, at + run)) {
				run++;
			}
			if (run > 0) {
				wsp_mark(w, ch, at, run, true);
				wsp_taken(w, goal, run);
				*addr = goal;
				*got = run;
				return 0;
			}
		}
	}
	for (uint64_t step = 0; step < sp->chunk_count; step++) {
		struct wchunk *ch = &sp->chunks[(sp->hint + step) % sp->chunk_count];
		uint32_t at;
		int error;

		if (ch->free == 0) {
			continue;
		}
		error = wsp_bitmap(w, ch);
		if (error) {
			return error;
		}
		at = wsp_first_free(ch->bitmap, 0, ch->blocks);
		while (at < ch->blocks) {
			uint32_t run = 1;

			/* block 0 of the container is the superblock's own */
			if (ch->addr + at == 0) {
				at = wsp_first_free(ch->bitmap, at + 1, ch->blocks);
				continue;
			}
			while (run < want && at + run < ch->blocks && !bit_get(ch->bitmap, at + run)) {
				run++;
			}
			wsp_mark(w, ch, at, run, true);
			wsp_taken(w, ch->addr + at, run);
			sp->hint = (uint32_t)(ch - sp->chunks);
			*addr = ch->addr + at;
			*got = run;
			return 0;
		}
	}
	return ENOSPC;
}

static struct wchunk *
wsp_chunk_of(struct apfs_wtx *w, uint64_t addr, uint64_t count)
{
	struct wspace *sp = &w->sp;
	uint64_t c = addr / sp->per_chunk;

	if (c >= sp->chunk_count || addr + count > sp->chunks[c].addr + sp->chunks[c].blocks) {
		return NULL;
	}
	return &sp->chunks[c];
}

/* Blocks the open transaction allocated and no longer needs: free again at once. */
int
wsp_free_now(struct apfs_wtx *w, uint64_t addr, uint64_t count)
{
	while (count > 0) {
		struct wchunk *ch = wsp_chunk_of(w, addr, 1);
		uint64_t here;
		int error;

		if (ch == NULL) {
			return EIO;
		}
		here = ch->addr + ch->blocks - addr;
		if (here > count) {
			here = count;
		}
		error = wsp_bitmap(w, ch);
		if (error) {
			return error;
		}
		for (uint64_t i = 0; i < here; i++) {
			if (!bit_get(ch->bitmap, (uint32_t)(addr - ch->addr + i))) {
				return EIO;             /* freeing what is free: a tree was misread */
			}
		}
		wsp_mark(w, ch, (uint32_t)(addr - ch->addr), (uint32_t)here, false);
		addr += here;
		count -= here;
	}
	return 0;
}

/* Blocks the live checkpoint still uses: freed when this transaction commits. */
int
wsp_free_later(struct apfs_wtx *w, uint64_t addr, uint64_t count, bool vol_owned)
{
	struct wspace *sp = &w->sp;

	if (count == 0) {
		return 0;
	}
	if (addr == 0 || addr + count > sp->block_count) {
		return EIO;
	}
	if (sp->nfreed > 0) {
		struct wfreed *last = &sp->freed[sp->nfreed - 1];

		if (last->addr + last->count == addr && last->vol_owned == vol_owned) {
			last->count += count;
			return 0;
		}
	}
	if (sp->nfreed == sp->freed_cap) {
		uint32_t cap = sp->freed_cap ? sp->freed_cap * 2 : 256;
		struct wfreed *n = apfs_alloc(cap * sizeof(*n));

		if (n == NULL) {
			return ENOMEM;
		}
		if (sp->freed != NULL) {
			memcpy(n, sp->freed, sp->nfreed * sizeof(*n));
			apfs_free(sp->freed, sp->freed_cap * sizeof(*sp->freed));
		}
		sp->freed = n;
		sp->freed_cap = cap;
	}
	sp->freed[sp->nfreed].addr = addr;
	sp->freed[sp->nfreed].count = count;
	sp->freed[sp->nfreed].vol_owned = vol_owned;
	sp->nfreed++;
	return 0;
}

/* Free blocks, less what the open transaction still has to release. */
uint64_t
wsp_free_blocks(struct apfs_wtx *w)
{
	return w->sp.free_count;
}

/* ------------------------------------------------------------------ the internal pool */

static int
wsp_ip_alloc(struct apfs_wtx *w, uint64_t *addr)
{
	struct wspace *sp = &w->sp;

	for (uint64_t step = 0; step < sp->ip_blocks; step++) {
		uint32_t i = (uint32_t)((sp->ip_hint + step) % sp->ip_blocks);

		if (!bit_get(sp->ip_bm, i)) {
			bit_set(sp->ip_bm, i, true);
			sp->ip_hint = i + 1;
			*addr = sp->ip_base + i;
			return 0;
		}
	}
	return ENOSPC;
}

static int
wsp_ip_free_later(struct apfs_wtx *w, uint64_t addr)
{
	struct wspace *sp = &w->sp;

	if (addr == 0) {
		return 0;
	}
	if (addr < sp->ip_base || addr >= sp->ip_base + sp->ip_blocks) {
		return EIO;
	}
	if (sp->nip_freed == sp->ip_freed_cap) {
		uint32_t cap = sp->ip_freed_cap ? sp->ip_freed_cap * 2 : 64;
		uint64_t *n = apfs_alloc(cap * sizeof(*n));

		if (n == NULL) {
			return ENOMEM;
		}
		if (sp->ip_freed != NULL) {
			memcpy(n, sp->ip_freed, sp->nip_freed * sizeof(*n));
			apfs_free(sp->ip_freed, sp->ip_freed_cap * sizeof(*n));
		}
		sp->ip_freed = n;
		sp->ip_freed_cap = cap;
	}
	sp->ip_freed[sp->nip_freed++] = addr;
	return 0;
}

/* An internal-pool block for the commit: written with the rest, raw or sealed. */
static int
wsp_ip_put(struct apfs_wtx *w, uint64_t addr, const uint8_t *data, bool raw);

/*
 * The commit's part: release what the transaction freed, then every changed bitmap and CIB
 * (and cib-address block) to new internal-pool blocks, the pool's bitmap to its next ring
 * slot, and the spaceman object updated in memory for the checkpoint.
 */
int
wsp_commit(struct apfs_wtx *w)
{
	struct wspace *sp = &w->sp;
	uint8_t *blk = NULL;
	int error = 0;

	for (uint32_t i = 0; i < sp->nfreed; i++) {
		uint64_t addr = sp->freed[i].addr, count = sp->freed[i].count;

		while (count > 0) {
			struct wchunk *ch = wsp_chunk_of(w, addr, 1);
			uint64_t here;

			if (ch == NULL) {
				return EIO;
			}
			here = ch->addr + ch->blocks - addr;
			if (here > count) {
				here = count;
			}
			error = wsp_bitmap(w, ch);
			if (error) {
				return error;
			}
			wsp_mark(w, ch, (uint32_t)(addr - ch->addr), (uint32_t)here, false);
			addr += here;
			count -= here;
		}
	}
	sp->nfreed = 0;

	blk = apfs_alloc(w->bs);
	if (blk == NULL) {
		return ENOMEM;
	}
	for (uint64_t c = 0; c < sp->chunk_count; c++) {
		struct wchunk *ch = &sp->chunks[c];
		uint64_t naddr;

		if (!ch->dirty) {
			continue;
		}
		error = wsp_ip_alloc(w, &naddr);
		if (error == 0) {
			error = wsp_ip_put(w, naddr, ch->bitmap, true);
		}
		if (error == 0) {
			error = wsp_ip_free_later(w, ch->bitmap_addr);
		}
		if (error) {
			goto out;
		}
		ch->bitmap_addr = naddr;
		ch->xid = w->xid;
		ch->dirty = false;
	}
	bool cabs_dirty = false;

	for (uint32_t c = 0; c < sp->cib_count; c++) {
		uint32_t first = c * sp->per_cib, n = sp->per_cib;
		uint64_t naddr;

		if (!sp->cib_dirty[c]) {
			continue;
		}
		if (first + n > sp->chunk_count) {
			n = (uint32_t)(sp->chunk_count - first);
		}
		memset(blk, 0, w->bs);
		w_put_le32(blk + 32, c);
		w_put_le32(blk + 36, n);
		for (uint32_t i = 0; i < n; i++) {
			const struct wchunk *ch = &sp->chunks[first + i];
			uint8_t *e = blk + 40 + 32 * i;

			w_put_le64(e, ch->xid);
			w_put_le64(e + 8, ch->addr);
			w_put_le32(e + 16, ch->blocks);
			w_put_le32(e + 20, ch->free);
			w_put_le64(e + 24, ch->bitmap_addr);
		}
		error = wsp_ip_alloc(w, &naddr);
		if (error) {
			goto out;
		}
		w_put_le64(blk + 8, naddr);
		w_put_le64(blk + 16, w->xid);
		w_put_le32(blk + 24, APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_CIB);
		w_put_le32(blk + 28, 0);
		error = wsp_ip_put(w, naddr, blk, false);
		if (error == 0) {
			error = wsp_ip_free_later(w, sp->cib_addr[c]);
		}
		if (error) {
			goto out;
		}
		sp->cib_addr[c] = naddr;
		sp->cib_dirty[c] = false;
		cabs_dirty = true;
	}
	uint32_t off = w_le32(sp->sm + SM_ADDR_OFFSET);

	if (sp->cab_count == 0) {
		for (uint32_t c = 0; c < sp->cib_count; c++) {
			w_put_le64(sp->sm + off + 8 * c, sp->cib_addr[c]);
		}
	} else if (cabs_dirty) {
		for (uint32_t a = 0; a < sp->cab_count; a++) {
			uint32_t first = a * sp->per_cab, n = sp->per_cab;
			uint64_t naddr;

			if (first + n > sp->cib_count) {
				n = sp->cib_count - first;
			}
			memset(blk, 0, w->bs);
			w_put_le32(blk + 32, a);
			w_put_le32(blk + 36, n);
			for (uint32_t i = 0; i < n; i++) {
				w_put_le64(blk + 40 + 8 * i, sp->cib_addr[first + i]);
			}
			error = wsp_ip_alloc(w, &naddr);
			if (error) {
				goto out;
			}
			w_put_le64(blk + 8, naddr);
			w_put_le64(blk + 16, w->xid);
			w_put_le32(blk + 24, APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_CAB);
			w_put_le32(blk + 28, 0);
			error = wsp_ip_put(w, naddr, blk, false);
			if (error == 0) {
				error = wsp_ip_free_later(w, sp->cab_addr[a]);
			}
			if (error) {
				goto out;
			}
			sp->cab_addr[a] = naddr;
			w_put_le64(sp->sm + off + 8 * a, naddr);
		}
	}

	/* the old copies are free in the pool's new bitmap: nothing allocated after this */
	for (uint32_t i = 0; i < sp->nip_freed; i++) {
		bit_set(sp->ip_bm, (uint32_t)(sp->ip_freed[i] - sp->ip_base), false);
	}
	sp->nip_freed = 0;

	/* the pool's bitmap into a free slot of its ring; the slot it was in joins the free list */
	{
		uint32_t next_off = w_le32(sp->sm + SM_IP_NEXT_OFF);
		uint32_t bm_off = w_le32(sp->sm + SM_IP_BITMAP_OFF);
		uint16_t head = w_le16(sp->sm + SM_IP_FREE_HEAD);
		uint16_t tail = w_le16(sp->sm + SM_IP_FREE_TAIL);
		uint16_t old = w_le16(sp->sm + bm_off);
		uint16_t after = w_le16(sp->sm + next_off + 2 * head);

		if (head == old) {
			error = EIO;
			goto out;
		}
		w_put_le16(sp->sm + next_off + 2 * head, 0xffff);
		w_put_le16(sp->sm + next_off + 2 * tail, old);
		w_put_le16(sp->sm + next_off + 2 * old, 0xffff);
		w_put_le16(sp->sm + SM_IP_FREE_HEAD, after);
		w_put_le16(sp->sm + SM_IP_FREE_TAIL, old);
		w_put_le16(sp->sm + bm_off, head);
		w_put_le64(sp->sm + w_le32(sp->sm + SM_IP_XID_OFF), w->xid);
		error = wsp_ip_put(w, sp->ip_bm_base + head, sp->ip_bm, true);
		if (error) {
			goto out;
		}
	}
	w_put_le64(sp->sm + SM_FREE_COUNT, sp->free_count);
	w_put_le64(sp->sm + 8, sp->sm_oid);
	w_put_le64(sp->sm + 16, w->xid);
	w_put_le32(sp->sm + 24, APFS_OBJ_EPHEMERAL | APFS_OBJECT_TYPE_SPACEMAN);
	w_put_le32(sp->sm + 28, 0);
out:
	apfs_free(blk, w->bs);
	return error;
}

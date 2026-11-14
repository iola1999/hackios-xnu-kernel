/*
 * APFS Transaction Engine & COW allocator
 */
#pragma once

/* ------------------------------------------------------------------ transaction engine */

struct apfs_txn {
	struct apfs_nx   *nx;
	struct apfs_vol  *vol;
	struct apfs_vol   saved_vol;
	bool             committed;
	uint64_t          old_xid;
	uint64_t          new_xid;

	uint64_t          cpm_bno;
	uint8_t          *cpm_buf;

	uint64_t          sm_bno;
	uint32_t          sm_size;
	uint64_t          sm_oid;
	uint8_t          *sm_buf;

	uint32_t          chunk_start;
	uint32_t          chunk_blocks;
	uint32_t          chunk_free;
	uint64_t          bmap_bno;
	uint8_t          *bmap_buf;

	uint64_t          cib_bno;
	uint8_t          *cib_buf;

	uint64_t          ip_base;
	uint32_t          ip_blocks;
	uint64_t          ip_bm_base;
	uint32_t          ip_bm_size;
	uint32_t          ip_bmap_blocks;
	uint16_t          ip_slot;
	uint8_t          *ip_bm_buf;

	uint16_t          ip_free_head;
	uint16_t          ip_free_tail;
	uint32_t          ip_xid_off;
	uint32_t          ip_bitmap_off;
	uint32_t          ip_free_next_off;
	uint32_t          cib_addr_off;

	struct {
		uint64_t bno;
		uint8_t *data;
		uint32_t size;
	} pending[128];
	uint32_t num_pending;

	struct {
		uint64_t bno;
		uint32_t count;
	} freed_ranges[128];
	uint32_t num_freed_ranges;

	struct {
		uint64_t oid;
		uint64_t paddr;
	} omap_records[64];
	uint32_t num_omap_records;
};

static int
txn_read_block(struct apfs_txn *txn, uint64_t bno, void *buf, uint32_t len)
{
	if (txn != NULL) {
		for (int32_t i = (int32_t)txn->num_pending - 1; i >= 0; i--) {
			if (txn->pending[i].bno == bno) {
				uint32_t sz = len < txn->pending[i].size ? len : txn->pending[i].size;
				memcpy(buf, txn->pending[i].data, sz);
				return 0;
			}
		}
	}
	struct apfs_nx *nx = txn ? txn->nx : NULL;
	if (nx == NULL || nx->dev.read == NULL) {
		return EIO;
	}
	uint64_t at = bno * (uint64_t)nx->block_size;
	return nx->dev.read(nx->dev.ctx, at, buf, len);
}

static int
txn_begin(struct apfs_vol *vol, struct apfs_txn *txn)
{
	struct apfs_nx *nx = vol->nx;
	uint32_t bs = nx->block_size;
	uint8_t *buf = NULL;
	bool found_cpm = false;
	int error = 0;

	memset(txn, 0, sizeof(*txn));
	txn->nx = nx;
	txn->vol = vol;
	txn->saved_vol = *vol;
	txn->old_xid = nx->xid;
	txn->new_xid = nx->xid + 1;
	const char *why = NULL;
	error = apfs_write_volume_ok(vol, &why);
	if (error) return error;
	if (nx->xid == UINT64_MAX || bs != 4096 || nx->xp_desc_blocks < 4 ||
	    nx->xp_data_blocks < 8) return EOPNOTSUPP;

	buf = apfs_alloc(bs);
	if (buf == NULL) {
		return ENOMEM;
	}

	for (uint32_t i = 0; i < nx->xp_desc_blocks; i++) {
		uint64_t b = nx->xp_desc_base + i;
		if (txn_read_block(txn, b, buf, bs) != 0) {
			continue;
		}
		if (!apfs_checksum_ok(buf, bs)) {
			continue;
		}
		uint32_t otype = w_le32(buf + 24) & APFS_OBJECT_TYPE_MASK;
		uint64_t xid = w_le64(buf + 16);
		if (otype == APFS_OBJECT_TYPE_CHECKPOINT_MAP && xid == txn->old_xid) {
			txn->cpm_bno = b;
			txn->cpm_buf = apfs_alloc(bs);
			if (txn->cpm_buf == NULL) {
				error = ENOMEM;
				goto fail;
			}
			memcpy(txn->cpm_buf, buf, bs);
			found_cpm = true;
			break;
		}
	}
	if (!found_cpm) {
		error = EIO;
		goto fail;
	}

	uint32_t cpm_count = w_le32(txn->cpm_buf + 36);
	if (cpm_count == 0 || cpm_count > (bs - 40) / 40 || cpm_count > 16) {
		error = EOPNOTSUPP;
		goto fail;
	}
	bool found_sm = false;
	for (uint32_t m = 0; m < cpm_count; m++) {
		const uint8_t *entry = txn->cpm_buf + 40 + 40 * m;
		uint32_t t = w_le32(entry);
		if ((t & APFS_OBJECT_TYPE_MASK) == APFS_OBJECT_TYPE_SPACEMAN) {
			txn->sm_size = w_le32(entry + 8);
			txn->sm_oid = w_le64(entry + 24);
			txn->sm_bno = w_le64(entry + 32);
			found_sm = true;
			break;
		}
	}
	if (!found_sm || txn->sm_size != bs) {
		error = EOPNOTSUPP;
		goto fail;
	}

	txn->sm_buf = apfs_alloc(txn->sm_size);
	if (txn->sm_buf == NULL) {
		error = ENOMEM;
		goto fail;
	}
	if (txn_read_block(txn, txn->sm_bno, txn->sm_buf, txn->sm_size) != 0) {
		error = EIO;
		goto fail;
	}
	/* The allocator currently handles one chunk and one internal-pool bitmap. */
	if (!apfs_checksum_ok(txn->sm_buf, bs) || w_le64(txn->sm_buf + 56) != 1 ||
	    w_le32(txn->sm_buf + 64) != 1 || w_le32(txn->sm_buf + 68) != 0 ||
	    w_le32(txn->sm_buf + 160) != 1) {
		error = EOPNOTSUPP;
		goto fail;
	}

	txn->cib_addr_off = w_le32(txn->sm_buf + 80);
	if (txn->cib_addr_off > bs - 8) {
		error = EIO;
		goto fail;
	}
	txn->cib_bno = w_le64(txn->sm_buf + txn->cib_addr_off);

	txn->ip_blocks = (uint32_t)w_le64(txn->sm_buf + 152);
	txn->ip_bm_size = w_le32(txn->sm_buf + 160);
	txn->ip_bmap_blocks = w_le32(txn->sm_buf + 164);
	txn->ip_bm_base = w_le64(txn->sm_buf + 168);
	txn->ip_base = w_le64(txn->sm_buf + 176);

	txn->ip_free_head = (uint16_t)w_le16(txn->sm_buf + 320);
	txn->ip_free_tail = (uint16_t)w_le16(txn->sm_buf + 322);
	txn->ip_xid_off = w_le32(txn->sm_buf + 324);
	txn->ip_bitmap_off = w_le32(txn->sm_buf + 328);
	txn->ip_free_next_off = w_le32(txn->sm_buf + 332);
	if (txn->ip_blocks > bs * 8 || txn->ip_bmap_blocks == 0 ||
	    txn->ip_bmap_blocks > bs / 2 || txn->ip_xid_off > bs - 8 ||
	    txn->ip_bitmap_off > bs - 2 || txn->ip_free_next_off > bs - 2 * txn->ip_bmap_blocks ||
	    txn->ip_free_head >= txn->ip_bmap_blocks || txn->ip_free_tail >= txn->ip_bmap_blocks) {
		error = EIO;
		goto fail;
	}

	txn->cib_buf = apfs_alloc(bs);
	if (txn->cib_buf == NULL) {
		error = ENOMEM;
		goto fail;
	}
	if (txn_read_block(txn, txn->cib_bno, txn->cib_buf, bs) != 0) {
		error = EIO;
		goto fail;
	}
	txn->chunk_start = (uint32_t)w_le64(txn->cib_buf + 48);
	txn->chunk_blocks = w_le32(txn->cib_buf + 56);
	txn->chunk_free = w_le32(txn->cib_buf + 60);
	txn->bmap_bno = w_le64(txn->cib_buf + 64);
	if (!apfs_checksum_ok(txn->cib_buf, bs) || w_le32(txn->cib_buf + 36) != 1 ||
	    txn->chunk_blocks > bs * 8 || txn->chunk_free > txn->chunk_blocks ||
	    (uint64_t)txn->chunk_start + txn->chunk_blocks > nx->block_count) {
		error = EOPNOTSUPP;
		goto fail;
	}

	txn->bmap_buf = apfs_alloc(bs);
	if (txn->bmap_buf == NULL) {
		error = ENOMEM;
		goto fail;
	}
	if (txn_read_block(txn, txn->bmap_bno, txn->bmap_buf, bs) != 0) {
		error = EIO;
		goto fail;
	}

	txn->ip_slot = (uint16_t)w_le16(txn->sm_buf + txn->ip_bitmap_off);
	if (txn->ip_slot >= txn->ip_bmap_blocks) {
		error = EIO;
		goto fail;
	}
	txn->ip_bm_buf = apfs_alloc(txn->ip_bm_size * bs);
	if (txn->ip_bm_buf == NULL) {
		error = ENOMEM;
		goto fail;
	}
	if (txn_read_block(txn, txn->ip_bm_base + txn->ip_slot, txn->ip_bm_buf, txn->ip_bm_size * bs) != 0) {
		error = EIO;
		goto fail;
	}

	apfs_free(buf, bs);
	return 0;

fail:
	if (buf != NULL) apfs_free(buf, bs);
	if (txn->cpm_buf != NULL) apfs_free(txn->cpm_buf, bs);
	if (txn->sm_buf != NULL) apfs_free(txn->sm_buf, txn->sm_size);
	if (txn->cib_buf != NULL) apfs_free(txn->cib_buf, bs);
	if (txn->bmap_buf != NULL) apfs_free(txn->bmap_buf, bs);
	if (txn->ip_bm_buf != NULL) apfs_free(txn->ip_bm_buf, txn->ip_bm_size * bs);
	return error;
}

static bool
txn_is_freed(const struct apfs_txn *txn, uint64_t bno, uint32_t count)
{
	for (uint32_t i = 0; i < txn->num_freed_ranges; i++) {
		uint64_t r_start = txn->freed_ranges[i].bno;
		uint64_t r_end = r_start + txn->freed_ranges[i].count;
		uint64_t q_end = bno + count;
		if (r_start < q_end && r_end > bno) {
			return true;
		}
	}
	return false;
}

static inline void
txn_add_omap_record(struct apfs_txn *txn, uint64_t oid, uint64_t paddr)
{
	for (uint32_t i = 0; i < txn->num_omap_records; i++) {
		if (txn->omap_records[i].oid == oid) {
			txn->omap_records[i].paddr = paddr;
			return;
		}
	}
	if (txn->num_omap_records < sizeof(txn->omap_records) / sizeof(txn->omap_records[0])) {
		txn->omap_records[txn->num_omap_records].oid = oid;
		txn->omap_records[txn->num_omap_records].paddr = paddr;
		txn->num_omap_records++;
	}
}

static inline int
txn_mark_freed_range(struct apfs_txn *txn, uint64_t bno, uint32_t count)
{
	if (bno == 0 || count == 0) return 0;
	for (uint32_t i = 0; i < txn->num_freed_ranges; i++) {
		if (txn->freed_ranges[i].bno + txn->freed_ranges[i].count == bno) {
			txn->freed_ranges[i].count += count;
			return 0;
		}
		if (bno + count == txn->freed_ranges[i].bno) {
			txn->freed_ranges[i].bno = bno;
			txn->freed_ranges[i].count += count;
			return 0;
		}
	}
	if (txn->num_freed_ranges >= sizeof(txn->freed_ranges) / sizeof(txn->freed_ranges[0])) {
		return ENOSPC;
	}
	txn->freed_ranges[txn->num_freed_ranges].bno = bno;
	txn->freed_ranges[txn->num_freed_ranges].count = count;
	txn->num_freed_ranges++;
	return 0;
}

static inline int
txn_mark_freed(struct apfs_txn *txn, uint64_t bno)
{
	return txn_mark_freed_range(txn, bno, 1);
}

static int
txn_alloc_blocks(struct apfs_txn *txn, uint32_t count, uint64_t *out_bno)
{
	uint32_t found = 0;
	uint32_t start_idx = 0;
	if (count == 0 || count > txn->chunk_free) return ENOSPC;

	for (uint32_t i = 0; i < txn->chunk_blocks; i++) {
		uint64_t bno = txn->chunk_start + i;
		if (!((txn->bmap_buf[i >> 3] >> (i & 7)) & 1) && !txn_is_freed(txn, bno, 1)) {
			if (found == 0) {
				start_idx = i;
			}
			found++;
			if (found == count) {
				for (uint32_t k = 0; k < count; k++) {
					uint32_t bit = start_idx + k;
					txn->bmap_buf[bit >> 3] |= (1 << (bit & 7));
				}
				txn->chunk_free -= count;
				*out_bno = txn->chunk_start + start_idx;
				return 0;
			}
		} else {
			found = 0;
		}
	}
	return ENOSPC;
}

static void
txn_free_block(struct apfs_txn *txn, uint64_t bno)
{
	if (bno >= txn->chunk_start && bno < txn->chunk_start + txn->chunk_blocks) {
		uint32_t idx = (uint32_t)(bno - txn->chunk_start);
		if ((txn->bmap_buf[idx >> 3] >> (idx & 7)) & 1) {
			txn->bmap_buf[idx >> 3] &= ~(1 << (idx & 7));
			txn->chunk_free++;
		}
	}
}

static int
txn_ip_alloc_block(struct apfs_txn *txn, uint64_t *out_bno)
{
	for (uint32_t i = 0; i < txn->ip_blocks; i++) {
		if (!((txn->ip_bm_buf[i >> 3] >> (i & 7)) & 1)) {
			txn->ip_bm_buf[i >> 3] |= (1 << (i & 7));
			*out_bno = txn->ip_base + i;
			return 0;
		}
	}
	return ENOSPC;
}

static void
txn_ip_free_block(struct apfs_txn *txn, uint64_t bno)
{
	if (bno >= txn->ip_base && bno < txn->ip_base + txn->ip_blocks) {
		uint32_t idx = (uint32_t)(bno - txn->ip_base);
		txn->ip_bm_buf[idx >> 3] &= ~(1 << (idx & 7));
	}
}

static int
txn_put(struct apfs_txn *txn, uint64_t bno, const void *data, uint32_t size)
{
	for (uint32_t i = 0; i < txn->num_pending; i++) {
		if (txn->pending[i].bno == bno) {
			if (txn->pending[i].size != size) {
				apfs_free(txn->pending[i].data, txn->pending[i].size);
				txn->pending[i].data = apfs_alloc(size);
				if (txn->pending[i].data == NULL) return ENOMEM;
				txn->pending[i].size = size;
			}
			memcpy(txn->pending[i].data, data, size);
			return 0;
		}
	}
	if (txn->num_pending >= sizeof(txn->pending)/sizeof(txn->pending[0])) {
		return ENOMEM;
	}
	uint8_t *copy = apfs_alloc(size);
	if (copy == NULL) {
		return ENOMEM;
	}
	memcpy(copy, data, size);
	txn->pending[txn->num_pending].bno = bno;
	txn->pending[txn->num_pending].data = copy;
	txn->pending[txn->num_pending].size = size;
	txn->num_pending++;
	return 0;
}

static inline uint64_t jkey_id_val(const uint8_t *k) { return w_le64(k) & APFS_OBJ_ID_MASK; }
static inline uint8_t jkey_type_val(const uint8_t *k) { return (uint8_t)(w_le64(k) >> APFS_OBJ_TYPE_SHIFT); }

static int
rec_key_cmp(const uint8_t *k1, uint16_t kl1, const uint8_t *k2, uint16_t kl2)
{
	uint64_t id1 = jkey_id_val(k1), id2 = jkey_id_val(k2);
	if (id1 != id2) return id1 < id2 ? -1 : 1;
	uint8_t t1 = jkey_type_val(k1), t2 = jkey_type_val(k2);
	if (t1 != t2) return t1 < t2 ? -1 : 1;

	if (t1 == APFS_TYPE_DIR_REC) {
		uint32_t nlh1 = (kl1 >= 12) ? w_le32(k1 + 8) & ~0x3ff : 0;
		uint32_t nlh2 = (kl2 >= 12) ? w_le32(k2 + 8) & ~0x3ff : 0;
		if (nlh1 != nlh2) return nlh1 < nlh2 ? -1 : 1;
		size_t off1 = (kl1 >= 12) ? 12 : 10;
		size_t off2 = (kl2 >= 12) ? 12 : 10;
		size_t l1 = kl1 > off1 ? kl1 - off1 : 0;
		size_t l2 = kl2 > off2 ? kl2 - off2 : 0;
		size_t min_l = l1 < l2 ? l1 : l2;
		int c = memcmp(k1 + off1, k2 + off2, min_l);
		if (c != 0) return c;
		return l1 < l2 ? -1 : (l1 > l2 ? 1 : 0);
	}
	if (t1 == APFS_TYPE_FILE_EXTENT) {
		uint64_t off1 = (kl1 >= 16) ? w_le64(k1 + 8) : 0;
		uint64_t off2 = (kl2 >= 16) ? w_le64(k2 + 8) : 0;
		if (off1 != off2) return off1 < off2 ? -1 : 1;
	}
	if (t1 == APFS_TYPE_XATTR) {
		size_t off = 10;
		size_t l1 = kl1 > off ? kl1 - off : 0;
		size_t l2 = kl2 > off ? kl2 - off : 0;
		size_t min_l = l1 < l2 ? l1 : l2;
		int c = memcmp(k1 + off, k2 + off, min_l);
		if (c != 0) return c;
		return l1 < l2 ? -1 : (l1 > l2 ? 1 : 0);
	}
	return 0;
}

#include "apfs_txn_btree.inc"

struct omap_entry {
	uint64_t oid;
	uint64_t xid;
	uint32_t flags;
	uint32_t size;
	uint64_t paddr;
};

static int
txn_cow_omap(struct apfs_txn *txn, uint64_t target_oid, uint64_t target_new_paddr,
    bool include_txn_records, uint64_t omap_obj_bno,
    uint64_t *out_new_omap_bno, uint64_t *out_new_tree_bno)
{
	struct apfs_nx *nx = txn->nx;
	uint32_t bs = nx->block_size;
	uint8_t *omap_obj = apfs_alloc(bs);
	uint8_t *omap_tree = apfs_alloc(bs);
	int error = 0;

	if (omap_obj == NULL || omap_tree == NULL) {
		error = ENOMEM;
		goto done;
	}

	if (txn_read_block(txn, omap_obj_bno, omap_obj, bs) != 0) {
		error = EIO;
		goto done;
	}
	uint64_t tree_bno = w_le64(omap_obj + 48);
	if (txn_read_block(txn, tree_bno, omap_tree, bs) != 0) {
		error = EIO;
		goto done;
	}

	uint32_t nkeys = w_le32(omap_tree + 36);
	uint32_t ts_off = w_le16(omap_tree + 40);
	uint32_t ts_len = w_le16(omap_tree + 42);
	uint32_t toc = 56 + ts_off;
	uint32_t key_base = toc + ts_len;
	uint32_t val_end = bs - 40;
	if (!apfs_checksum_ok(omap_tree, bs) || w_le16(omap_tree + 34) != 0 ||
	    w_le16(omap_tree + 32) != (APFS_BTNODE_ROOT | APFS_BTNODE_LEAF | APFS_BTNODE_FIXED_KV_SIZE) ||
	    toc > val_end || ts_len > val_end - toc || nkeys > ts_len / 4) {
		error = EOPNOTSUPP;
		goto done;
	}

	struct omap_entry entries[128];
	uint32_t total = 0;
	for (uint32_t i = 0; i < nkeys && total < 112; i++) {
		uint32_t koff = w_le16(omap_tree + toc + 4 * i);
		uint32_t voff = w_le16(omap_tree + toc + 4 * i + 2);
		if (koff > val_end - key_base || 16 > val_end - key_base - koff ||
		    voff > val_end || voff < 16 || val_end - voff < key_base) {
			error = EIO;
			goto done;
		}
		entries[total].oid = w_le64(omap_tree + key_base + koff);
		entries[total].xid = w_le64(omap_tree + key_base + koff + 8);
		entries[total].flags = w_le32(omap_tree + val_end - voff);
		entries[total].size = w_le32(omap_tree + val_end - voff + 4);
		entries[total].paddr = w_le64(omap_tree + val_end - voff + 8);
		total++;
	}

	/* Update or insert target entry */
	bool found = false;
	for (uint32_t i = 0; i < total; i++) {
		if (entries[i].oid == target_oid) {
			entries[i].xid = txn->new_xid;
			entries[i].paddr = target_new_paddr;
			found = true;
			break;
		}
	}
	if (!found && total < 112) {
		entries[total].oid = target_oid;
		entries[total].xid = txn->new_xid;
		entries[total].flags = 0;
		entries[total].size = bs;
		entries[total].paddr = target_new_paddr;
		total++;
	}

	/* Update or insert extra OMAP records from transaction if requested */
	if (include_txn_records) {
		for (uint32_t r = 0; r < txn->num_omap_records; r++) {
			uint64_t r_oid = txn->omap_records[r].oid;
			uint64_t r_paddr = txn->omap_records[r].paddr;
			bool r_found = false;
			for (uint32_t i = 0; i < total; i++) {
				if (entries[i].oid == r_oid) {
					entries[i].xid = txn->new_xid;
					entries[i].paddr = r_paddr;
					r_found = true;
					break;
				}
			}
			if (!r_found && total < 112) {
				entries[total].oid = r_oid;
				entries[total].xid = txn->new_xid;
				entries[total].flags = 0;
				entries[total].size = bs;
				entries[total].paddr = r_paddr;
				total++;
			}
		}
	}

	/* Sort entries by (oid, xid) ascending */
	for (uint32_t i = 1; i < total; i++) {
		struct omap_entry tmp = entries[i];
		uint32_t j = i;
		while (j > 0 && (entries[j - 1].oid > tmp.oid ||
		    (entries[j - 1].oid == tmp.oid && entries[j - 1].xid > tmp.xid))) {
			entries[j] = entries[j - 1];
			j--;
		}
		entries[j] = tmp;
	}

	uint8_t *new_tree = apfs_alloc(bs);
	if (new_tree == NULL) {
		error = ENOMEM;
		goto done;
	}
	memset(new_tree, 0, bs);

	uint32_t new_ts_len = 448;
	uint32_t new_toc = 56;
	uint32_t new_key_base = 56 + new_ts_len; /* 504 */
	uint32_t new_val_end = bs - 40;
	uint32_t cur_koff = 0;
	uint32_t cur_voff = 0;

	for (uint32_t i = 0; i < total; i++) {
		w_put_le64(new_tree + new_key_base + cur_koff, entries[i].oid);
		w_put_le64(new_tree + new_key_base + cur_koff + 8, entries[i].xid);

		cur_voff += 16;
		w_put_le32(new_tree + new_val_end - cur_voff, entries[i].flags);
		w_put_le32(new_tree + new_val_end - cur_voff + 4, entries[i].size ? entries[i].size : bs);
		w_put_le64(new_tree + new_val_end - cur_voff + 8, entries[i].paddr);

		w_put_le16(new_tree + new_toc + 4 * i, (uint16_t)cur_koff);
		w_put_le16(new_tree + new_toc + 4 * i + 2, (uint16_t)cur_voff);
		cur_koff += 16;
	}

	uint32_t free_sp = new_val_end - cur_voff - (new_key_base + cur_koff);
	w_put_le16(new_tree + 32, APFS_BTNODE_ROOT | APFS_BTNODE_LEAF | APFS_BTNODE_FIXED_KV_SIZE);
	w_put_le16(new_tree + 34, 0); /* level = 0 */
	w_put_le32(new_tree + 36, total);

	w_put_le16(new_tree + 40, 0);
	w_put_le16(new_tree + 42, (uint16_t)new_ts_len);
	w_put_le16(new_tree + 44, (uint16_t)cur_koff);
	w_put_le16(new_tree + 46, (uint16_t)free_sp);
	w_put_le16(new_tree + 48, 0xffff);
	w_put_le16(new_tree + 50, 0);
	w_put_le16(new_tree + 52, 0xffff);
	w_put_le16(new_tree + 54, 0);

	memcpy(new_tree + bs - 40, omap_tree + bs - 40, 40);
	w_put_le32(new_tree + bs - 40 + 8, 16);
	w_put_le32(new_tree + bs - 40 + 12, 16);
	w_put_le32(new_tree + bs - 40 + 16, 16);
	w_put_le32(new_tree + bs - 40 + 20, 16);
	w_put_le64(new_tree + bs - 40 + 24, total);
	w_put_le64(new_tree + bs - 40 + 32, 1);

	uint64_t new_tree_bno = 0;
	error = txn_alloc_blocks(txn, 1, &new_tree_bno);
	if (error) {
		apfs_free(new_tree, bs);
		goto done;
	}

	w_seal_obj(new_tree, bs, new_tree_bno, txn->new_xid,
	    APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_BTREE, APFS_OBJECT_TYPE_OMAP);
	error = txn_put(txn, new_tree_bno, new_tree, bs);
	apfs_free(new_tree, bs);
	if (error) goto done;
	txn_mark_freed(txn, tree_bno);

	uint64_t new_omap_bno = 0;
	error = txn_alloc_blocks(txn, 1, &new_omap_bno);
	if (error) goto done;

	w_put_le64(omap_obj + 48, new_tree_bno);
	w_seal_obj(omap_obj, bs, new_omap_bno, txn->new_xid,
	    APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_OMAP, 0);
	error = txn_put(txn, new_omap_bno, omap_obj, bs);
	if (error) goto done;
	txn_mark_freed(txn, omap_obj_bno);

	*out_new_omap_bno = new_omap_bno;
	if (out_new_tree_bno != NULL) *out_new_tree_bno = new_tree_bno;

done:
	if (omap_obj != NULL) apfs_free(omap_obj, bs);
	if (omap_tree != NULL) apfs_free(omap_tree, bs);
	return error;
}

static int
txn_cow_volume_sb(struct apfs_txn *txn, uint64_t new_vol_omap_bno, uint64_t *out_new_vol_sb)
{
	struct apfs_nx *nx = txn->nx;
	struct apfs_vol *vol = txn->vol;
	uint32_t bs = nx->block_size;
	uint8_t *vol_sb = apfs_alloc(bs);
	int error = 0;

	if (vol_sb == NULL) return ENOMEM;

	uint64_t old_vol_sb_bno = 0;
	error = apfs_omap_lookup(&nx->omap, vol->oid, nx->xid, &old_vol_sb_bno);
	if (error) goto done;

	if (txn_read_block(txn, old_vol_sb_bno, vol_sb, bs) != 0) {
		error = EIO;
		goto done;
	}

	w_put_le64(vol_sb + 128, new_vol_omap_bno);
	w_put_le64(vol_sb + 144, vol->extentref_oid);
	w_put_le64(vol_sb + 88, vol->alloc_count);
	w_put_le64(vol_sb + 176, vol->next_obj_id);
	w_put_le64(vol_sb + 184, vol->num_files);
	w_put_le64(vol_sb + 192, vol->num_dirs);
	w_put_le64(vol_sb + 64, apfs_ticks());

	uint64_t new_vol_sb_bno = 0;
	error = txn_alloc_blocks(txn, 1, &new_vol_sb_bno);
	if (error) goto done;

	w_seal_obj(vol_sb, bs, vol->oid, txn->new_xid,
	    APFS_OBJ_VIRTUAL | APFS_OBJECT_TYPE_FS, 0);
	error = txn_put(txn, new_vol_sb_bno, vol_sb, bs);
	if (error) goto done;
	txn_mark_freed(txn, old_vol_sb_bno);

	*out_new_vol_sb = new_vol_sb_bno;

done:
	if (vol_sb != NULL) apfs_free(vol_sb, bs);
	return error;
}

static int
txn_commit(struct apfs_txn *txn, uint64_t new_fs_bno)
{
	struct apfs_nx *nx = txn->nx;
	struct apfs_vol *vol = txn->vol;
	uint32_t bs = nx->block_size;
	int error = 0;

	uint64_t new_vol_omap_bno = 0, new_vol_tree_bno = 0;
	error = txn_cow_omap(txn, vol->tree.root_oid, new_fs_bno, true,
	    vol->omap.omap_paddr, &new_vol_omap_bno, &new_vol_tree_bno);
	if (error) return error;

	uint64_t new_vol_sb_bno = 0;
	error = txn_cow_volume_sb(txn, new_vol_omap_bno, &new_vol_sb_bno);
	if (error) return error;

	uint64_t new_nx_omap_bno = 0, new_nx_tree_bno = 0;
	error = txn_cow_omap(txn, vol->oid, new_vol_sb_bno, false,
	    nx->omap.omap_paddr ? nx->omap.omap_paddr : nx->omap_addr,
	    &new_nx_omap_bno, &new_nx_tree_bno);
	if (error) return error;

	for (uint32_t i = 0; i < txn->num_freed_ranges; i++) {
		for (uint32_t k = 0; k < txn->freed_ranges[i].count; k++) {
			txn_free_block(txn, txn->freed_ranges[i].bno + k);
		}
	}

	uint64_t new_bmap_bno = 0, new_cib_bno = 0;
	error = txn_ip_alloc_block(txn, &new_bmap_bno);
	if (error) return error;
	error = txn_ip_alloc_block(txn, &new_cib_bno);
	if (error) return error;

	txn_ip_free_block(txn, txn->bmap_bno);
	txn_ip_free_block(txn, txn->cib_bno);

	w_put_le64(txn->cib_buf + 40, txn->new_xid);
	w_put_le32(txn->cib_buf + 60, txn->chunk_free);
	w_put_le64(txn->cib_buf + 64, new_bmap_bno);
	w_seal_obj(txn->cib_buf, bs, new_cib_bno, txn->new_xid,
	    APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_CIB, 0);

	error = txn_put(txn, new_bmap_bno, txn->bmap_buf, bs);
	if (error) return error;
	error = txn_put(txn, new_cib_bno, txn->cib_buf, bs);
	if (error) return error;

	uint16_t slot = txn->ip_free_head;
	uint16_t nxt = (uint16_t)w_le16(txn->sm_buf + txn->ip_free_next_off + 2 * slot);
	w_put_le16(txn->sm_buf + txn->ip_free_next_off + 2 * slot, 0xffff);
	w_put_le16(txn->sm_buf + txn->ip_free_next_off + 2 * txn->ip_free_tail, txn->ip_slot);
	w_put_le16(txn->sm_buf + txn->ip_free_next_off + 2 * txn->ip_slot, 0xffff);
	txn->ip_free_head = nxt;
	txn->ip_free_tail = txn->ip_slot;
	w_put_le16(txn->sm_buf + 320, txn->ip_free_head);
	w_put_le16(txn->sm_buf + 322, txn->ip_free_tail);
	w_put_le16(txn->sm_buf + txn->ip_bitmap_off, slot);
	w_put_le64(txn->sm_buf + txn->ip_xid_off, txn->new_xid);

	error = txn_put(txn, txn->ip_bm_base + slot, txn->ip_bm_buf, txn->ip_bm_size * bs);
	if (error) return error;

	w_put_le64(txn->sm_buf + 72, txn->chunk_free);
	w_put_le64(txn->sm_buf + txn->cib_addr_off, new_cib_bno);
	w_seal_obj(txn->sm_buf, txn->sm_size, txn->sm_oid, txn->new_xid,
	    APFS_OBJ_EPHEMERAL | APFS_OBJECT_TYPE_SPACEMAN, 0);

	uint32_t data_index = nx->xp_data_next % nx->xp_data_blocks;
	uint32_t data_at = data_index;
	uint64_t sm_dest = nx->xp_data_base + data_at;
	error = txn_put(txn, sm_dest, txn->sm_buf, txn->sm_size);
	if (error) return error;
	data_at += txn->sm_size / bs;

	uint32_t old_cpm_count = w_le32(txn->cpm_buf + 36);
	struct {
		uint64_t paddr;
		uint32_t type;
		uint32_t subtype;
		uint32_t size;
		uint64_t oid;
	} eph[16];
	uint32_t eph_count = 0;

	eph[eph_count].paddr = sm_dest;
	eph[eph_count].type = APFS_OBJ_EPHEMERAL | APFS_OBJECT_TYPE_SPACEMAN;
	eph[eph_count].subtype = 0;
	eph[eph_count].size = txn->sm_size;
	eph[eph_count].oid = txn->sm_oid;
	eph_count++;

	uint8_t *eph_block = apfs_alloc(bs);
	if (eph_block == NULL) return ENOMEM;

	for (uint32_t m = 0; m < old_cpm_count && eph_count < 16; m++) {
		const uint8_t *entry = txn->cpm_buf + 40 + 40 * m;
		uint32_t t = w_le32(entry);
		if ((t & APFS_OBJECT_TYPE_MASK) == APFS_OBJECT_TYPE_SPACEMAN) {
			continue;
		}
		uint32_t st = w_le32(entry + 4);
		uint32_t sz = w_le32(entry + 8);
		uint64_t oid = w_le64(entry + 24);
		uint64_t paddr = w_le64(entry + 32);

		if (sz != bs || data_at >= nx->xp_data_blocks) {
			apfs_free(eph_block, bs);
			return EOPNOTSUPP;
		}
		error = txn_read_block(txn, paddr, eph_block, sz);
		if (error == 0) {
			uint64_t dest = nx->xp_data_base + data_at;
			w_seal_obj(eph_block, sz, oid, txn->new_xid, t, st);
			error = txn_put(txn, dest, eph_block, sz);
			if (error) {
				apfs_free(eph_block, bs);
				return error;
			}
			eph[eph_count].paddr = dest;
			eph[eph_count].type = t;
			eph[eph_count].subtype = st;
			eph[eph_count].size = sz;
			eph[eph_count].oid = oid;
			eph_count++;
			data_at += sz / bs;
		} else {
			apfs_free(eph_block, bs);
			return error;
		}
	}
	apfs_free(eph_block, bs);

	uint32_t desc_at = nx->xp_desc_next % nx->xp_desc_blocks;
	uint64_t new_cpm_bno = nx->xp_desc_base + desc_at;
	uint8_t *new_cpm = apfs_alloc(bs);
	if (new_cpm == NULL) return ENOMEM;

	w_put_le32(new_cpm + 32, 1);
	w_put_le32(new_cpm + 36, eph_count);
	for (uint32_t i = 0; i < eph_count; i++) {
		uint8_t *e = new_cpm + 40 + 40 * i;
		w_put_le32(e, eph[i].type);
		w_put_le32(e + 4, eph[i].subtype);
		w_put_le32(e + 8, eph[i].size);
		w_put_le64(e + 24, eph[i].oid);
		w_put_le64(e + 32, eph[i].paddr);
	}
	w_seal_obj(new_cpm, bs, new_cpm_bno, txn->new_xid,
	    APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_CHECKPOINT_MAP, 0);
	error = txn_put(txn, new_cpm_bno, new_cpm, bs);
	apfs_free(new_cpm, bs);
	if (error) return error;

	uint8_t *new_nx = apfs_alloc(bs);
	if (new_nx == NULL) return ENOMEM;
	if (txn_read_block(txn, 0, new_nx, bs) != 0) {
		apfs_free(new_nx, bs);
		return EIO;
	}

	uint64_t next_oid = nx->next_oid;
	if (next_oid < vol->next_obj_id + 10) next_oid = vol->next_obj_id + 10;
	if (next_oid < nx->fs_oid[0] + 100) next_oid = nx->fs_oid[0] + 100;
	if (next_oid < 1024) next_oid = 1024;
	nx->next_oid = next_oid + 1;
	w_put_le64(new_nx + 88, nx->next_oid);
	w_put_le64(new_nx + 96, txn->new_xid + 1);
	w_put_le64(new_nx + 160, new_nx_omap_bno);
	w_put_le32(new_nx + 128, (desc_at + 2) % nx->xp_desc_blocks);
	w_put_le32(new_nx + 132, data_at % nx->xp_data_blocks);
	w_put_le32(new_nx + 136, desc_at);
	w_put_le32(new_nx + 140, 2);
	w_put_le32(new_nx + 144, data_index);
	w_put_le32(new_nx + 148, data_at - data_index);

	uint64_t new_nx_bno = nx->xp_desc_base + (desc_at + 1) % nx->xp_desc_blocks;
	w_seal_obj(new_nx, bs, 1, txn->new_xid,
	    APFS_OBJ_EPHEMERAL | APFS_OBJECT_TYPE_NX_SUPERBLOCK, 0);

	error = txn_put(txn, new_nx_bno, new_nx, bs);
	if (error) {
		apfs_free(new_nx, bs);
		return error;
	}

	error = txn_put(txn, 0, new_nx, bs);
	apfs_free(new_nx, bs);
	if (error) return error;

	/*
	 * Dispatch pending writes in strict checkpoint barrier order:
	 * Phase 1: All data extents, B-trees, OMAP nodes, Spaceman bitmap,
	 *          ephemeral data ring objects, and checkpoint map (new_cpm_bno).
	 * Phase 2: Checkpoint descriptor ring container superblock (new_nx_bno).
	 * Phase 3: Primary container superblock at block 0.
	 */
	for (uint32_t i = 0; i < txn->num_pending; i++) {
		uint64_t b = txn->pending[i].bno;
		if (b == new_nx_bno || b == 0) {
			continue;
		}
		uint32_t sz = txn->pending[i].size;
		const uint8_t *d = txn->pending[i].data;
		error = nx->dev.write(nx->dev.ctx, b * (uint64_t)bs, d, sz);
		if (error) {
			return error;
		}
	}

	for (uint32_t i = 0; i < txn->num_pending; i++) {
		uint64_t b = txn->pending[i].bno;
		if (b != new_nx_bno) {
			continue;
		}
		uint32_t sz = txn->pending[i].size;
		const uint8_t *d = txn->pending[i].data;
		error = nx->dev.write(nx->dev.ctx, b * (uint64_t)bs, d, sz);
		if (error) {
			return error;
		}
	}

	for (uint32_t i = 0; i < txn->num_pending; i++) {
		uint64_t b = txn->pending[i].bno;
		if (b != 0) {
			continue;
		}
		uint32_t sz = txn->pending[i].size;
		const uint8_t *d = txn->pending[i].data;
		error = nx->dev.write(nx->dev.ctx, b * (uint64_t)bs, d, sz);
		if (error) {
			return error;
		}
	}

	nx->xid = txn->new_xid;
	nx->omap_addr = new_nx_omap_bno;
	nx->omap.omap_paddr = new_nx_omap_bno;
	nx->omap.tree.root = new_nx_tree_bno;
	vol->omap.omap_paddr = new_vol_omap_bno;
	vol->omap.tree.root = new_vol_tree_bno;
	vol->tree.root = new_fs_bno;
	nx->xp_desc_next = (desc_at + 2) % nx->xp_desc_blocks;
	nx->xp_data_next = data_at % nx->xp_data_blocks;
	nx->xp_desc_index = desc_at;
	nx->xp_desc_len = 2;
	nx->xp_data_index = data_index;
	nx->xp_data_len = data_at - data_index;
	txn->committed = true;
	apfs_nx_cache_flush(nx);

	return 0;
}

static void
txn_destroy(struct apfs_txn *txn)
{
	if (!txn->committed) *txn->vol = txn->saved_vol;
	uint32_t bs = txn->nx ? txn->nx->block_size : 4096;
	if (txn->cpm_buf != NULL) apfs_free(txn->cpm_buf, bs);
	if (txn->sm_buf != NULL) apfs_free(txn->sm_buf, txn->sm_size);
	if (txn->cib_buf != NULL) apfs_free(txn->cib_buf, bs);
	if (txn->bmap_buf != NULL) apfs_free(txn->bmap_buf, bs);
	if (txn->ip_bm_buf != NULL) apfs_free(txn->ip_bm_buf, txn->ip_bm_size * bs);
	for (uint32_t i = 0; i < txn->num_pending; i++) {
		if (txn->pending[i].data != NULL) {
			apfs_free(txn->pending[i].data, txn->pending[i].size);
		}
	}
	txn->num_pending = 0;
}


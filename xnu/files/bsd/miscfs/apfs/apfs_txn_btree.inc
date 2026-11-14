/*
 * APFS B-tree Node Splitting & Multi-level Traversal Engine
 */
#pragma once

struct btree_item {
	const uint8_t *key;
	uint16_t       key_len;
	const uint8_t *val;
	uint16_t       val_len;
};

static int
txn_build_leaf_node(struct apfs_txn *txn, const struct btree_item *items,
    uint32_t count, uint16_t flags, uint16_t level, uint64_t oid,
    uint32_t obj_type, uint32_t subtype, uint64_t *out_bno)
{
	struct apfs_nx *nx = txn->nx;
	uint32_t bs = nx->block_size;
	uint8_t *blk = apfs_alloc(bs);
	if (blk == NULL) return ENOMEM;
	memset(blk, 0, bs);

	bool is_root = (flags & APFS_BTNODE_ROOT) != 0;
	uint32_t val_end = bs - (is_root ? 40 : 0);
	uint32_t toc_len = count * 8;
	if (toc_len < 8) toc_len = 8;
	uint32_t key_base = 56 + toc_len;
	uint32_t cur_koff = 0;
	uint32_t cur_voff = 0;

	for (uint32_t i = 0; i < count; i++) {
		memcpy(blk + key_base + cur_koff, items[i].key, items[i].key_len);
		cur_voff += items[i].val_len;
		memcpy(blk + val_end - cur_voff, items[i].val, items[i].val_len);

		w_put_le16(blk + 56 + 8 * i, (uint16_t)cur_koff);
		w_put_le16(blk + 56 + 8 * i + 2, items[i].key_len);
		w_put_le16(blk + 56 + 8 * i + 4, (uint16_t)cur_voff);
		w_put_le16(blk + 56 + 8 * i + 6, items[i].val_len);

		cur_koff += items[i].key_len;
	}

	uint32_t free_sp = val_end - cur_voff - (key_base + cur_koff);
	w_put_le16(blk + 32, flags);
	w_put_le16(blk + 34, level);
	w_put_le32(blk + 36, count);

	w_put_le16(blk + 40, 0);
	w_put_le16(blk + 42, (uint16_t)toc_len);
	w_put_le16(blk + 44, (uint16_t)cur_koff);
	w_put_le16(blk + 46, (uint16_t)free_sp);
	w_put_le16(blk + 48, 0xffff);
	w_put_le16(blk + 50, 0);
	w_put_le16(blk + 52, 0xffff);
	w_put_le16(blk + 54, 0);

	uint64_t new_bno = 0;
	int error = txn_alloc_blocks(txn, 1, &new_bno);
	if (error) {
		apfs_free(blk, bs);
		return error;
	}

	uint32_t node_obj_type = (obj_type & ~APFS_OBJECT_TYPE_MASK) |
	    ((flags & APFS_BTNODE_ROOT) ? APFS_OBJECT_TYPE_BTREE : APFS_OBJECT_TYPE_BTREE_NODE);
	uint64_t seal_oid = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ? new_bno : oid;
	w_seal_obj(blk, bs, seal_oid, txn->new_xid, node_obj_type, subtype);

	error = txn_put(txn, new_bno, blk, bs);
	apfs_free(blk, bs);
	if (error == 0) {
		*out_bno = new_bno;
	}
	return error;
}

static int
txn_split_btree_root(struct apfs_txn *txn, uint64_t old_root_bno,
    uint64_t root_oid, uint32_t obj_type, uint32_t subtype,
    const struct btree_item *items, uint32_t rec_count,
    const uint8_t *old_blk, uint64_t *out_new_bno)
{
	struct apfs_nx *nx = txn->nx;
	uint32_t bs = nx->block_size;
	uint32_t mid = rec_count / 2;
	if (mid == 0 || mid >= rec_count) return ENOSPC;

	uint64_t left_bno = 0;
	uint64_t left_oid = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ?
	    0 : txn->nx->next_oid++;
	int error = txn_build_leaf_node(txn, items, mid,
	    APFS_BTNODE_LEAF, 0, left_oid, obj_type, subtype, &left_bno);
	if (error) return error;

	uint64_t right_bno = 0;
	uint64_t right_oid = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ?
	    0 : txn->nx->next_oid++;
	error = txn_build_leaf_node(txn, items + mid, rec_count - mid,
	    APFS_BTNODE_LEAF, 0, right_oid, obj_type, subtype, &right_bno);
	if (error) return error;

	if ((obj_type & APFS_OBJ_STORAGETYPE_MASK) != APFS_OBJ_PHYSICAL) {
		txn_add_omap_record(txn, left_oid, left_bno);
		txn_add_omap_record(txn, right_oid, right_bno);
	}

	/* Create the new index root node with btn_level = 1 */
	uint8_t *root_blk = apfs_alloc(bs);
	if (root_blk == NULL) return ENOMEM;
	memset(root_blk, 0, bs);

	uint32_t val_end = bs - 40; /* root retains btree_info */
	uint32_t toc_len = 2 * 8;
	uint32_t key_base = 56 + toc_len;
	uint32_t cur_koff = 0;
	uint32_t cur_voff = 0;

	/* Index Entry 0: first key of left child */
	uint64_t left_child_val = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ? left_bno : left_oid;
	memcpy(root_blk + key_base + cur_koff, items[0].key, items[0].key_len);
	cur_voff += 8;
	w_put_le64(root_blk + val_end - cur_voff, left_child_val);
	w_put_le16(root_blk + 56, (uint16_t)cur_koff);
	w_put_le16(root_blk + 58, items[0].key_len);
	w_put_le16(root_blk + 60, (uint16_t)cur_voff);
	w_put_le16(root_blk + 62, 8);
	cur_koff += items[0].key_len;

	/* Index Entry 1: first key of right child */
	uint64_t right_child_val = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ? right_bno : right_oid;
	memcpy(root_blk + key_base + cur_koff, items[mid].key, items[mid].key_len);
	cur_voff += 8;
	w_put_le64(root_blk + val_end - cur_voff, right_child_val);
	w_put_le16(root_blk + 64, (uint16_t)cur_koff);
	w_put_le16(root_blk + 66, items[mid].key_len);
	w_put_le16(root_blk + 68, (uint16_t)cur_voff);
	w_put_le16(root_blk + 70, 8);
	cur_koff += items[mid].key_len;

	uint32_t free_sp = val_end - cur_voff - (key_base + cur_koff);
	w_put_le16(root_blk + 32, APFS_BTNODE_ROOT);
	w_put_le16(root_blk + 34, 1); /* btn_level = 1 */
	w_put_le32(root_blk + 36, 2);

	w_put_le16(root_blk + 40, 0);
	w_put_le16(root_blk + 42, (uint16_t)toc_len);
	w_put_le16(root_blk + 44, (uint16_t)cur_koff);
	w_put_le16(root_blk + 46, (uint16_t)free_sp);
	w_put_le16(root_blk + 48, 0xffff);
	w_put_le16(root_blk + 50, 0);
	w_put_le16(root_blk + 52, 0xffff);
	w_put_le16(root_blk + 54, 0);

	/* Copy and update btree_info */
	memcpy(root_blk + bs - 40, old_blk + bs - 40, 40);
	uint32_t max_k = 0, max_v = 0;
	for (uint32_t i = 0; i < rec_count; i++) {
		if (items[i].key_len > max_k) max_k = items[i].key_len;
		if (items[i].val_len > max_v) max_v = items[i].val_len;
	}
	w_put_le32(root_blk + bs - 40 + 16, max_k);
	w_put_le32(root_blk + bs - 40 + 20, max_v);
	w_put_le64(root_blk + bs - 40 + 24, rec_count);
	w_put_le64(root_blk + bs - 40 + 32, 3); /* 3 nodes: root + 2 leaves */

	uint64_t new_root_bno = 0;
	error = txn_alloc_blocks(txn, 1, &new_root_bno);
	if (error) {
		apfs_free(root_blk, bs);
		return error;
	}

	uint64_t seal_oid = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ? new_root_bno : root_oid;
	w_seal_obj(root_blk, bs, seal_oid, txn->new_xid, APFS_OBJECT_TYPE_BTREE, subtype);

	error = txn_put(txn, new_root_bno, root_blk, bs);
	apfs_free(root_blk, bs);
	if (error) return error;

	txn_mark_freed(txn, old_root_bno);
	if ((obj_type & APFS_OBJ_STORAGETYPE_MASK) != APFS_OBJ_PHYSICAL && txn->vol != NULL) {
		txn->vol->alloc_count += 2;
	}
	*out_new_bno = new_root_bno;
	return 0;
}

#define APFS_MAX_BTREE_INDEX_KEYS 128

struct btree_index_slot {
	uint8_t  key[268];
	uint16_t key_len;
	uint64_t child_val;
};

static int
txn_modify_btree_index(struct apfs_txn *txn, uint64_t old_root_bno,
    uint64_t root_oid, uint32_t obj_type, uint32_t subtype,
    const uint8_t *mod_key, uint16_t mod_klen,
    const uint8_t *mod_val, uint16_t mod_vlen,
    bool is_delete, const uint8_t *old_root_blk, uint64_t *out_new_bno)
{
	struct apfs_nx *nx = txn->nx;
	uint32_t bs = nx->block_size;
	uint32_t nkeys = w_le32(old_root_blk + 36);
	uint32_t ts_off = w_le16(old_root_blk + 40);
	uint32_t ts_len = w_le16(old_root_blk + 42);
	uint32_t toc = 56 + ts_off;
	uint32_t key_base = toc + ts_len;
	uint32_t val_end = bs - 40; /* root has btree_info */

	if (nkeys == 0 || nkeys >= APFS_MAX_BTREE_INDEX_KEYS) return ENOSPC;

	struct btree_index_slot *idx_slots = apfs_alloc(sizeof(struct btree_index_slot) * APFS_MAX_BTREE_INDEX_KEYS);
	if (idx_slots == NULL) return ENOMEM;

	for (uint32_t i = 0; i < nkeys; i++) {
		uint32_t koff = w_le16(old_root_blk + toc + 8 * i);
		uint32_t kl = w_le16(old_root_blk + toc + 8 * i + 2);
		uint32_t voff = w_le16(old_root_blk + toc + 8 * i + 4);
		uint32_t vl = w_le16(old_root_blk + toc + 8 * i + 6);
		if (kl < 8 || kl > sizeof(idx_slots[0].key) || vl != 8) {
			apfs_free(idx_slots, sizeof(struct btree_index_slot) * APFS_MAX_BTREE_INDEX_KEYS);
			return EIO;
		}
		idx_slots[i].key_len = (uint16_t)kl;
		memcpy(idx_slots[i].key, old_root_blk + key_base + koff, kl);
		idx_slots[i].child_val = w_le64(old_root_blk + val_end - voff);
	}

	/* Find target child node: binary search largest index with key <= mod_key */
	uint32_t target_idx = 0;
	int low = 0, high = (int)nkeys - 1;
	while (low <= high) {
		int mid = low + (high - low) / 2;
		if (rec_key_cmp(mod_key, mod_klen, idx_slots[mid].key, idx_slots[mid].key_len) >= 0) {
			target_idx = (uint32_t)mid;
			low = mid + 1;
		} else {
			high = mid - 1;
		}
	}

	uint64_t target_child_val = idx_slots[target_idx].child_val;
	uint64_t child_bno = 0;
	if ((obj_type & APFS_OBJ_STORAGETYPE_MASK) != APFS_OBJ_PHYSICAL) {
		for (uint32_t j = 0; j < txn->num_omap_records; j++) {
			if (txn->omap_records[j].oid == target_child_val) {
				child_bno = txn->omap_records[j].paddr;
				break;
			}
		}
		if (child_bno == 0) {
			int err = apfs_omap_lookup(&txn->vol->omap, target_child_val, txn->old_xid, &child_bno);
			if (err) {
				apfs_free(idx_slots, sizeof(struct btree_index_slot) * APFS_MAX_BTREE_INDEX_KEYS);
				return err;
			}
		}
	} else {
		child_bno = target_child_val;
	}

	uint8_t *child_blk = apfs_alloc(bs);
	struct btree_item *child_items = apfs_alloc(sizeof(struct btree_item) * 512);
	if (child_blk == NULL || child_items == NULL) {
		if (child_blk) apfs_free(child_blk, bs);
		if (child_items) apfs_free(child_items, sizeof(struct btree_item) * 512);
		apfs_free(idx_slots, sizeof(struct btree_index_slot) * APFS_MAX_BTREE_INDEX_KEYS);
		return ENOMEM;
	}

	if (txn_read_block(txn, child_bno, child_blk, bs) != 0) {
		apfs_free(child_blk, bs);
		apfs_free(child_items, sizeof(struct btree_item) * 512);
		apfs_free(idx_slots, sizeof(struct btree_index_slot) * APFS_MAX_BTREE_INDEX_KEYS);
		return EIO;
	}

	uint32_t c_nkeys = w_le32(child_blk + 36);
	uint32_t c_ts_off = w_le16(child_blk + 40);
	uint32_t c_ts_len = w_le16(child_blk + 42);
	uint32_t c_toc = 56 + c_ts_off;
	uint32_t c_key_base = c_toc + c_ts_len;
	uint32_t c_val_end = bs; /* leaf has no btree_info */

	/* Binary search child TOC for insertion / modification point */
	int c_low = 0, c_high = (int)c_nkeys - 1;
	int found_idx = -1;
	uint32_t ins_pos = c_nkeys;
	while (c_low <= c_high) {
		int mid = c_low + (c_high - c_low) / 2;
		uint32_t koff = w_le16(child_blk + c_toc + 8 * mid);
		uint32_t kl = w_le16(child_blk + c_toc + 8 * mid + 2);
		int cmp = rec_key_cmp(child_blk + c_key_base + koff, (uint16_t)kl, mod_key, mod_klen);
		if (cmp == 0) {
			found_idx = mid;
			break;
		} else if (cmp < 0) {
			c_low = mid + 1;
		} else {
			ins_pos = (uint32_t)mid;
			c_high = mid - 1;
		}
	}
	if (found_idx >= 0) {
		ins_pos = (uint32_t)found_idx;
	} else if (c_low > (int)c_nkeys - 1) {
		ins_pos = c_nkeys;
	}

	/* Assemble child_items */
	uint32_t child_rec_count = 0;
	if (is_delete) {
		for (uint32_t i = 0; i < c_nkeys; i++) {
			if ((int)i == found_idx) continue;
			uint32_t koff = w_le16(child_blk + c_toc + 8 * i);
			uint32_t kl = w_le16(child_blk + c_toc + 8 * i + 2);
			uint32_t voff = w_le16(child_blk + c_toc + 8 * i + 4);
			uint32_t vl = w_le16(child_blk + c_toc + 8 * i + 6);
			child_items[child_rec_count].key = child_blk + c_key_base + koff;
			child_items[child_rec_count].key_len = (uint16_t)kl;
			child_items[child_rec_count].val = child_blk + c_val_end - voff;
			child_items[child_rec_count].val_len = (uint16_t)vl;
			child_rec_count++;
		}
	} else if (found_idx >= 0) {
		for (uint32_t i = 0; i < c_nkeys; i++) {
			if ((int)i == found_idx) {
				child_items[child_rec_count].key = mod_key;
				child_items[child_rec_count].key_len = mod_klen;
				child_items[child_rec_count].val = mod_val;
				child_items[child_rec_count].val_len = mod_vlen;
			} else {
				uint32_t koff = w_le16(child_blk + c_toc + 8 * i);
				uint32_t kl = w_le16(child_blk + c_toc + 8 * i + 2);
				uint32_t voff = w_le16(child_blk + c_toc + 8 * i + 4);
				uint32_t vl = w_le16(child_blk + c_toc + 8 * i + 6);
				child_items[child_rec_count].key = child_blk + c_key_base + koff;
				child_items[child_rec_count].key_len = (uint16_t)kl;
				child_items[child_rec_count].val = child_blk + c_val_end - voff;
				child_items[child_rec_count].val_len = (uint16_t)vl;
			}
			child_rec_count++;
		}
	} else {
		for (uint32_t i = 0; i < ins_pos; i++) {
			uint32_t koff = w_le16(child_blk + c_toc + 8 * i);
			uint32_t kl = w_le16(child_blk + c_toc + 8 * i + 2);
			uint32_t voff = w_le16(child_blk + c_toc + 8 * i + 4);
			uint32_t vl = w_le16(child_blk + c_toc + 8 * i + 6);
			child_items[child_rec_count].key = child_blk + c_key_base + koff;
			child_items[child_rec_count].key_len = (uint16_t)kl;
			child_items[child_rec_count].val = child_blk + c_val_end - voff;
			child_items[child_rec_count].val_len = (uint16_t)vl;
			child_rec_count++;
		}
		child_items[child_rec_count].key = mod_key;
		child_items[child_rec_count].key_len = mod_klen;
		child_items[child_rec_count].val = mod_val;
		child_items[child_rec_count].val_len = mod_vlen;
		child_rec_count++;
		for (uint32_t i = ins_pos; i < c_nkeys; i++) {
			uint32_t koff = w_le16(child_blk + c_toc + 8 * i);
			uint32_t kl = w_le16(child_blk + c_toc + 8 * i + 2);
			uint32_t voff = w_le16(child_blk + c_toc + 8 * i + 4);
			uint32_t vl = w_le16(child_blk + c_toc + 8 * i + 6);
			child_items[child_rec_count].key = child_blk + c_key_base + koff;
			child_items[child_rec_count].key_len = (uint16_t)kl;
			child_items[child_rec_count].val = child_blk + c_val_end - voff;
			child_items[child_rec_count].val_len = (uint16_t)vl;
			child_rec_count++;
		}
	}

	uint32_t new_c_toc_len = child_rec_count * 8;
	if (new_c_toc_len < 8) new_c_toc_len = 8;
	uint64_t occupied = 56 + new_c_toc_len;
	for (uint32_t i = 0; i < child_rec_count; i++) {
		occupied += child_items[i].key_len + child_items[i].val_len;
	}

	int error = 0;
	if (occupied <= c_val_end && child_rec_count < 512) {
		/* Leaf fits without splitting */
		uint64_t new_child_bno = 0;
		error = txn_build_leaf_node(txn, child_items, child_rec_count,
		    APFS_BTNODE_LEAF, 0, target_child_val, obj_type, subtype, &new_child_bno);
		if (error) goto cleanup;
		txn_mark_freed(txn, child_bno);
		if ((obj_type & APFS_OBJ_STORAGETYPE_MASK) != APFS_OBJ_PHYSICAL) {
			txn_add_omap_record(txn, target_child_val, new_child_bno);
		} else {
			idx_slots[target_idx].child_val = new_child_bno;
		}
		/* If first key changed, update index key */
		if (child_rec_count > 0) {
			idx_slots[target_idx].key_len = child_items[0].key_len;
			memcpy(idx_slots[target_idx].key, child_items[0].key, child_items[0].key_len);
		}
	} else {
		/* Leaf overflows: split into two child leaves */
		if (nkeys + 1 >= APFS_MAX_BTREE_INDEX_KEYS) {
			error = ENOSPC;
			goto cleanup;
		}

		uint32_t mid = child_rec_count / 2;
		uint64_t left_oid = target_child_val;
		uint64_t right_oid = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ?
		    0 : txn->nx->next_oid++;
		uint64_t left_bno = 0, right_bno = 0;

		error = txn_build_leaf_node(txn, child_items, mid,
		    APFS_BTNODE_LEAF, 0, left_oid, obj_type, subtype, &left_bno);
		if (error) goto cleanup;

		error = txn_build_leaf_node(txn, child_items + mid, child_rec_count - mid,
		    APFS_BTNODE_LEAF, 0, right_oid, obj_type, subtype, &right_bno);
		if (error) goto cleanup;

		txn_mark_freed(txn, child_bno);
		if ((obj_type & APFS_OBJ_STORAGETYPE_MASK) != APFS_OBJ_PHYSICAL) {
			txn_add_omap_record(txn, left_oid, left_bno);
			txn_add_omap_record(txn, right_oid, right_bno);
			if (txn->vol != NULL) {
				txn->vol->alloc_count += 1;
			}
		}

		/* Update left child entry in index */
		idx_slots[target_idx].key_len = child_items[0].key_len;
		memcpy(idx_slots[target_idx].key, child_items[0].key, child_items[0].key_len);
		idx_slots[target_idx].child_val = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ?
		    left_bno : left_oid;

		/* Insert right child entry after target_idx */
		for (uint32_t i = nkeys; i > target_idx + 1; i--) {
			idx_slots[i] = idx_slots[i - 1];
		}
		idx_slots[target_idx + 1].key_len = child_items[mid].key_len;
		memcpy(idx_slots[target_idx + 1].key, child_items[mid].key, child_items[mid].key_len);
		idx_slots[target_idx + 1].child_val = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ?
		    right_bno : right_oid;
		nkeys++;
	}

	/* Check index root node capacity before repacking */
	uint32_t root_toc_len = nkeys * 8;
	uint32_t root_key_base = 56 + root_toc_len;
	uint32_t cur_koff = 0;
	uint32_t cur_voff = 0;

	for (uint32_t i = 0; i < nkeys; i++) {
		cur_koff += idx_slots[i].key_len;
		cur_voff += 8;
	}
	if (root_key_base + cur_koff + cur_voff > val_end) {
		error = ENOSPC;
		goto cleanup;
	}

	/* Repack and write new root index node */
	uint8_t *new_root_blk = apfs_alloc(bs);
	if (new_root_blk == NULL) {
		error = ENOMEM;
		goto cleanup;
	}
	memset(new_root_blk, 0, bs);

	cur_koff = 0;
	cur_voff = 0;
	for (uint32_t i = 0; i < nkeys; i++) {
		memcpy(new_root_blk + root_key_base + cur_koff, idx_slots[i].key, idx_slots[i].key_len);
		cur_voff += 8;
		w_put_le64(new_root_blk + val_end - cur_voff, idx_slots[i].child_val);

		w_put_le16(new_root_blk + 56 + 8 * i, (uint16_t)cur_koff);
		w_put_le16(new_root_blk + 56 + 8 * i + 2, idx_slots[i].key_len);
		w_put_le16(new_root_blk + 56 + 8 * i + 4, (uint16_t)cur_voff);
		w_put_le16(new_root_blk + 56 + 8 * i + 6, 8);
		cur_koff += idx_slots[i].key_len;
	}

	uint32_t free_sp = val_end - cur_voff - (root_key_base + cur_koff);
	w_put_le16(new_root_blk + 32, APFS_BTNODE_ROOT);
	w_put_le16(new_root_blk + 34, 1); /* level = 1 */
	w_put_le32(new_root_blk + 36, nkeys);

	w_put_le16(new_root_blk + 40, 0);
	w_put_le16(new_root_blk + 42, (uint16_t)root_toc_len);
	w_put_le16(new_root_blk + 44, (uint16_t)cur_koff);
	w_put_le16(new_root_blk + 46, (uint16_t)free_sp);
	w_put_le16(new_root_blk + 48, 0xffff);
	w_put_le16(new_root_blk + 50, 0);
	w_put_le16(new_root_blk + 52, 0xffff);
	w_put_le16(new_root_blk + 54, 0);

	/* Copy btree_info from old root */
	memcpy(new_root_blk + bs - 40, old_root_blk + bs - 40, 40);
	if (!is_delete) {
		if (mod_klen > w_le32(new_root_blk + bs - 40 + 16)) {
			w_put_le32(new_root_blk + bs - 40 + 16, mod_klen);
		}
		if (mod_vlen > w_le32(new_root_blk + bs - 40 + 20)) {
			w_put_le32(new_root_blk + bs - 40 + 20, mod_vlen);
		}
	}
	uint64_t old_kc = w_le64(old_root_blk + bs - 40 + 24);
	if (is_delete) {
		if (found_idx >= 0 && old_kc > 0) old_kc--;
	} else if (found_idx < 0) {
		old_kc++;
	}
	w_put_le64(new_root_blk + bs - 40 + 24, old_kc);
	w_put_le64(new_root_blk + bs - 40 + 32, nkeys + 1); /* nodes = root + children */

	uint64_t new_root_bno = 0;
	error = txn_alloc_blocks(txn, 1, &new_root_bno);
	if (error) {
		apfs_free(new_root_blk, bs);
		goto cleanup;
	}

	uint64_t seal_oid = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ?
	    new_root_bno : root_oid;
	w_seal_obj(new_root_blk, bs, seal_oid, txn->new_xid, APFS_OBJECT_TYPE_BTREE, subtype);
	error = txn_put(txn, new_root_bno, new_root_blk, bs);
	apfs_free(new_root_blk, bs);
	if (error) goto cleanup;

	txn_mark_freed(txn, old_root_bno);
	*out_new_bno = new_root_bno;

cleanup:
	if (child_blk) apfs_free(child_blk, bs);
	if (child_items) apfs_free(child_items, sizeof(struct btree_item) * 512);
	if (idx_slots) apfs_free(idx_slots, sizeof(struct btree_index_slot) * APFS_MAX_BTREE_INDEX_KEYS);
	return error;
}

static int
txn_modify_btree_root(struct apfs_txn *txn, uint64_t old_root_bno,
    uint64_t root_oid, uint32_t obj_type, uint32_t subtype,
    const uint8_t *mod_key, uint16_t mod_klen,
    const uint8_t *mod_val, uint16_t mod_vlen,
    bool is_delete, uint64_t *out_new_bno)
{
	struct apfs_nx *nx = txn->nx;
	uint32_t bs = nx->block_size;
	uint8_t *old_blk = apfs_alloc(bs);
	uint8_t *new_blk = apfs_alloc(bs);
	struct btree_item *items = apfs_alloc(sizeof(struct btree_item) * 512);
	int error = 0;

	if (old_blk == NULL || new_blk == NULL || items == NULL) {
		error = ENOMEM;
		goto done;
	}

	if (txn_read_block(txn, old_root_bno, old_blk, bs) != 0) {
		error = EIO;
		goto done;
	}

	uint16_t flags = (uint16_t)w_le16(old_blk + 32);
	uint16_t level = (uint16_t)w_le16(old_blk + 34);
	if (level > 0) {
		error = txn_modify_btree_index(txn, old_root_bno,
		    root_oid, obj_type, subtype, mod_key, mod_klen,
		    mod_val, mod_vlen, is_delete, old_blk, out_new_bno);
		goto done;
	}

	uint32_t nkeys = w_le32(old_blk + 36);
	uint32_t ts_off = w_le16(old_blk + 40);
	uint32_t ts_len = w_le16(old_blk + 42);
	uint32_t toc = 56 + ts_off;
	uint32_t key_base = toc + ts_len;
	bool is_root = (flags & APFS_BTNODE_ROOT) != 0;
	uint32_t val_end = bs - (is_root ? 40 : 0);

	if (!apfs_checksum_ok(old_blk, bs) ||
	    (flags & (APFS_BTNODE_ROOT | APFS_BTNODE_LEAF)) == 0) {
		error = EOPNOTSUPP;
		goto done;
	}
	if (nkeys > 512 || toc > val_end || ts_len > val_end - toc ||
	    nkeys > ts_len / 8 || mod_klen < 8) {
		error = EIO;
		goto done;
	}

	/* Binary search root leaf TOC */
	int low = 0, high = (int)nkeys - 1;
	int found_idx = -1;
	uint32_t ins_pos = nkeys;
	while (low <= high) {
		int mid = low + (high - low) / 2;
		uint32_t koff = w_le16(old_blk + toc + 8 * mid);
		uint32_t kl = w_le16(old_blk + toc + 8 * mid + 2);
		int cmp = rec_key_cmp(old_blk + key_base + koff, (uint16_t)kl, mod_key, mod_klen);
		if (cmp == 0) {
			found_idx = mid;
			break;
		} else if (cmp < 0) {
			low = mid + 1;
		} else {
			ins_pos = (uint32_t)mid;
			high = mid - 1;
		}
	}
	if (found_idx >= 0) {
		ins_pos = (uint32_t)found_idx;
	} else if (low > (int)nkeys - 1) {
		ins_pos = nkeys;
	}

	/* Assemble items referencing old_blk and mod_key/mod_val */
	uint32_t rec_count = 0;
	if (is_delete) {
		for (uint32_t i = 0; i < nkeys; i++) {
			if ((int)i == found_idx) continue;
			uint32_t koff = w_le16(old_blk + toc + 8 * i);
			uint32_t kl = w_le16(old_blk + toc + 8 * i + 2);
			uint32_t voff = w_le16(old_blk + toc + 8 * i + 4);
			uint32_t vl = w_le16(old_blk + toc + 8 * i + 6);
			items[rec_count].key = old_blk + key_base + koff;
			items[rec_count].key_len = (uint16_t)kl;
			items[rec_count].val = old_blk + val_end - voff;
			items[rec_count].val_len = (uint16_t)vl;
			rec_count++;
		}
	} else if (found_idx >= 0) {
		for (uint32_t i = 0; i < nkeys; i++) {
			if ((int)i == found_idx) {
				items[rec_count].key = mod_key;
				items[rec_count].key_len = mod_klen;
				items[rec_count].val = mod_val;
				items[rec_count].val_len = mod_vlen;
			} else {
				uint32_t koff = w_le16(old_blk + toc + 8 * i);
				uint32_t kl = w_le16(old_blk + toc + 8 * i + 2);
				uint32_t voff = w_le16(old_blk + toc + 8 * i + 4);
				uint32_t vl = w_le16(old_blk + toc + 8 * i + 6);
				items[rec_count].key = old_blk + key_base + koff;
				items[rec_count].key_len = (uint16_t)kl;
				items[rec_count].val = old_blk + val_end - voff;
				items[rec_count].val_len = (uint16_t)vl;
			}
			rec_count++;
		}
	} else {
		if (nkeys >= 512) {
			error = ENOSPC;
			goto done;
		}
		for (uint32_t i = 0; i < ins_pos; i++) {
			uint32_t koff = w_le16(old_blk + toc + 8 * i);
			uint32_t kl = w_le16(old_blk + toc + 8 * i + 2);
			uint32_t voff = w_le16(old_blk + toc + 8 * i + 4);
			uint32_t vl = w_le16(old_blk + toc + 8 * i + 6);
			items[rec_count].key = old_blk + key_base + koff;
			items[rec_count].key_len = (uint16_t)kl;
			items[rec_count].val = old_blk + val_end - voff;
			items[rec_count].val_len = (uint16_t)vl;
			rec_count++;
		}
		items[rec_count].key = mod_key;
		items[rec_count].key_len = mod_klen;
		items[rec_count].val = mod_val;
		items[rec_count].val_len = mod_vlen;
		rec_count++;
		for (uint32_t i = ins_pos; i < nkeys; i++) {
			uint32_t koff = w_le16(old_blk + toc + 8 * i);
			uint32_t kl = w_le16(old_blk + toc + 8 * i + 2);
			uint32_t voff = w_le16(old_blk + toc + 8 * i + 4);
			uint32_t vl = w_le16(old_blk + toc + 8 * i + 6);
			items[rec_count].key = old_blk + key_base + koff;
			items[rec_count].key_len = (uint16_t)kl;
			items[rec_count].val = old_blk + val_end - voff;
			items[rec_count].val_len = (uint16_t)vl;
			rec_count++;
		}
	}

	memset(new_blk, 0, bs);
	uint32_t new_toc_len = rec_count * 8;
	if (new_toc_len < 8) new_toc_len = 8;
	uint32_t new_key_base = 56 + new_toc_len;
	uint32_t cur_koff = 0;
	uint32_t cur_voff = 0;
	uint64_t occupied = new_key_base;
	for (uint32_t i = 0; i < rec_count; i++) {
		occupied += items[i].key_len + items[i].val_len;
	}

	if (occupied > val_end || rec_count >= 512) {
		if (is_root) {
			error = txn_split_btree_root(txn, old_root_bno,
			    root_oid, obj_type, subtype, items, rec_count,
			    old_blk, out_new_bno);
			goto done;
		}
		error = ENOSPC;
		goto done;
	}

	for (uint32_t i = 0; i < rec_count; i++) {
		memcpy(new_blk + new_key_base + cur_koff, items[i].key, items[i].key_len);
		cur_voff += items[i].val_len;
		memcpy(new_blk + val_end - cur_voff, items[i].val, items[i].val_len);

		w_put_le16(new_blk + 56 + 8 * i, (uint16_t)cur_koff);
		w_put_le16(new_blk + 56 + 8 * i + 2, items[i].key_len);
		w_put_le16(new_blk + 56 + 8 * i + 4, (uint16_t)cur_voff);
		w_put_le16(new_blk + 56 + 8 * i + 6, items[i].val_len);

		cur_koff += items[i].key_len;
	}

	uint32_t free_sp = val_end - cur_voff - (new_key_base + cur_koff);
	w_put_le16(new_blk + 32, flags);
	w_put_le16(new_blk + 34, level);
	w_put_le32(new_blk + 36, rec_count);

	w_put_le16(new_blk + 40, 0);
	w_put_le16(new_blk + 42, (uint16_t)new_toc_len);
	w_put_le16(new_blk + 44, (uint16_t)cur_koff);
	w_put_le16(new_blk + 46, (uint16_t)free_sp);
	w_put_le16(new_blk + 48, 0xffff);
	w_put_le16(new_blk + 50, 0);
	w_put_le16(new_blk + 52, 0xffff);
	w_put_le16(new_blk + 54, 0);

	if (is_root) {
		uint32_t max_k = 0, max_v = 0;
		for (uint32_t i = 0; i < rec_count; i++) {
			if (items[i].key_len > max_k) max_k = items[i].key_len;
			if (items[i].val_len > max_v) max_v = items[i].val_len;
		}
		memcpy(new_blk + bs - 40, old_blk + bs - 40, 40);
		w_put_le32(new_blk + bs - 40 + 16, max_k);
		w_put_le32(new_blk + bs - 40 + 20, max_v);
		w_put_le64(new_blk + bs - 40 + 24, rec_count);
	}

	uint64_t new_fs_bno = 0;
	error = txn_alloc_blocks(txn, 1, &new_fs_bno);
	if (error) goto done;

	uint64_t seal_oid = (obj_type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL ? new_fs_bno : root_oid;
	w_seal_obj(new_blk, bs, seal_oid, txn->new_xid, obj_type, subtype);

	error = txn_put(txn, new_fs_bno, new_blk, bs);
	if (error) goto done;

	txn_mark_freed(txn, old_root_bno);
	*out_new_bno = new_fs_bno;

done:
	if (old_blk != NULL) apfs_free(old_blk, bs);
	if (new_blk != NULL) apfs_free(new_blk, bs);
	if (items != NULL) apfs_free(items, sizeof(struct btree_item) * 512);
	return error;
}

static int
txn_modify_fs_root(struct apfs_txn *txn,
    const uint8_t *mod_key, uint16_t mod_klen,
    const uint8_t *mod_val, uint16_t mod_vlen,
    bool is_delete, uint64_t *out_new_bno)
{
	struct apfs_vol *vol = txn->vol;
	uint64_t old_root_bno = vol->tree.root;
	return txn_modify_btree_root(txn, old_root_bno, vol->tree.root_oid,
	    APFS_OBJ_VIRTUAL | APFS_OBJECT_TYPE_BTREE, APFS_OBJECT_TYPE_FSTREE,
	    mod_key, mod_klen, mod_val, mod_vlen, is_delete, out_new_bno);
}

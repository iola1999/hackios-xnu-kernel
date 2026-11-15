/*
 * APFS Extended Attribute Transactional Operations
 */
#pragma once

int
apfs_set_xattr(struct apfs_vol *vol, uint64_t obj_id, const char *name,
    const void *data, size_t size)
{
	if (vol == NULL || vol->nx == NULL || name == NULL || (data == NULL && size > 0)) {
		return EINVAL;
	}
	size_t name_len = strnlen(name, 256);
	if (name_len == 0 || name_len > 255) return ENAMETOOLONG;

	uint8_t inobuf[512];
	uint16_t inolen = 0;
	int error = read_raw_inode(vol, obj_id, inobuf, &inolen);
	if (error) return error;

	/* Check if attribute already exists */
	struct apfs_xattr old_xa;
	bool old_exists = (apfs_xattr_lookup(vol, obj_id, name, &old_xa) == 0);

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	uint32_t bs = vol->nx->block_size;
	uint64_t fs_final = 0;

	/* If old attribute was stream-backed, free old stream extents */
	if (old_exists && (old_xa.flags & APFS_XATTR_DATA_STREAM)) {
		uint64_t old_stream_id = old_xa.stream_id;
		uint8_t dst_key[8];
		w_make_jkey(dst_key, old_stream_id, APFS_TYPE_DSTREAM_ID);
		uint64_t fs_s = 0;
		if (txn_modify_fs_root(&txn, dst_key, sizeof(dst_key), NULL, 0, true, &fs_s) == 0 && fs_s != 0) {
			vol->tree.root = fs_s;
		}

		/* Find and free extents */
		struct apfs_cursor cur;
		struct apfs_rec rec;
		if (apfs_seek(&vol->tree, old_stream_id, APFS_TYPE_FILE_EXTENT, &cur) == 0) {
			while (apfs_cursor_next(&cur, &rec) == 0) {
				if (rec.obj_id != old_stream_id || rec.type != APFS_TYPE_FILE_EXTENT) break;
				if (rec.val != NULL && rec.val_len >= 16) {
					uint64_t blkcnt = (w_le64(rec.val) & APFS_PEXT_LEN_MASK) / bs;
					if (blkcnt == 0) blkcnt = 1;
					uint64_t phys_bno = w_le64(rec.val + 8);
					if (phys_bno != 0) {
						txn_mark_freed_range(&txn, phys_bno, (uint32_t)blkcnt);
					}
				}
				uint64_t st = 0;
				if (txn_modify_fs_root(&txn, rec.key, rec.key_len, NULL, 0, true, &st) == 0 && st != 0) {
					vol->tree.root = st;
				}
			}
		}
	}

	uint8_t xkey[268];
	w_make_jkey(xkey, obj_id, APFS_TYPE_XATTR);
	w_put_le16(xkey + 8, (uint16_t)(name_len + 1));
	memcpy(xkey + 10, name, name_len + 1);
	uint16_t xklen = (uint16_t)(10 + name_len + 1);

	if (size <= APFS_XATTR_MAX_EMBEDDED_SIZE) {
		/* Embedded xattr */
		uint8_t xval[APFS_XATTR_MAX_EMBEDDED_SIZE + 4];
		w_put_le16(xval, APFS_XATTR_DATA_EMBEDDED);
		w_put_le16(xval + 2, (uint16_t)size);
		if (size > 0) {
			memcpy(xval + 4, data, size);
		}
		uint16_t xvlen = (uint16_t)(4 + size);

		error = txn_modify_fs_root(&txn, xkey, xklen, xval, xvlen, false, &fs_final);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		vol->tree.root = fs_final;
	} else {
		/* Stream xattr */
		if (vol->next_obj_id < 16) vol->next_obj_id = 16;
		uint64_t stream_id = vol->next_obj_id++;
		uint32_t add_blocks = (uint32_t)((size + bs - 1) / bs);
		uint64_t new_phys = 0;

		error = txn_alloc_blocks(&txn, add_blocks, &new_phys);
		if (error) {
			txn_destroy(&txn);
			return error;
		}

		uint8_t *zbuf = apfs_alloc(add_blocks * bs);
		if (zbuf == NULL) {
			txn_destroy(&txn);
			return ENOMEM;
		}
		memset(zbuf, 0, add_blocks * bs);
		memcpy(zbuf, data, size);

		for (uint32_t b = 0; b < add_blocks; b++) {
			uint64_t target_b = new_phys + b;
			error = txn_put(&txn, target_b, zbuf + b * bs, bs);
			if (error) {
				apfs_free(zbuf, add_blocks * bs);
				txn_destroy(&txn);
				return error;
			}
		}
		apfs_free(zbuf, add_blocks * bs);

		/* Insert file extent record */
		uint8_t ext_key[16];
		w_make_jkey(ext_key, stream_id, APFS_TYPE_FILE_EXTENT);
		w_put_le64(ext_key + 8, 0);

		uint8_t ext_val[24];
		w_put_le64(ext_val, (uint64_t)(add_blocks * bs));
		w_put_le64(ext_val + 8, new_phys);
		w_put_le64(ext_val + 16, 0);

		error = txn_modify_fs_root(&txn, ext_key, sizeof(ext_key), ext_val, sizeof(ext_val), false, &fs_final);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		vol->tree.root = fs_final;

		/* Insert dstream record */
		uint8_t dst_key[8];
		w_make_jkey(dst_key, stream_id, APFS_TYPE_DSTREAM_ID);
		uint32_t refc = 1;
		error = txn_modify_fs_root(&txn, dst_key, sizeof(dst_key), (const uint8_t *)&refc, sizeof(refc), false, &fs_final);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		vol->tree.root = fs_final;

		/* Insert xattr record */
		uint8_t xval[52];
		memset(xval, 0, sizeof(xval));
		w_put_le16(xval, APFS_XATTR_DATA_STREAM);
		w_put_le16(xval + 2, 48); /* sizeof(struct apfs_j_xattr_dstream) */
		w_put_le64(xval + 4, stream_id);
		w_put_le64(xval + 12, (uint64_t)size);
		w_put_le64(xval + 20, (uint64_t)(add_blocks * bs));

		error = txn_modify_fs_root(&txn, xkey, xklen, xval, 4 + 48, false, &fs_final);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		vol->tree.root = fs_final;
	}

	/* Update inode ctime */
	uint64_t now = apfs_ticks();
	w_put_le64(inobuf + 32, now);
	uint8_t inokey[8];
	w_make_jkey(inokey, obj_id, APFS_TYPE_INODE);
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey), inobuf, inolen, false, &fs_final);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_final;

	error = txn_commit(&txn, fs_final);
	txn_destroy(&txn);
	return error;
}

int
apfs_remove_xattr(struct apfs_vol *vol, uint64_t obj_id, const char *name)
{
	if (vol == NULL || vol->nx == NULL || name == NULL) return EINVAL;
	size_t name_len = strnlen(name, 256);
	if (name_len == 0 || name_len > 255) return ENAMETOOLONG;

	struct apfs_xattr xa;
	int error = apfs_xattr_lookup(vol, obj_id, name, &xa);
	if (error) return error;

	uint8_t inobuf[512];
	uint16_t inolen = 0;
	error = read_raw_inode(vol, obj_id, inobuf, &inolen);
	if (error) return error;

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	uint32_t bs = vol->nx->block_size;
	uint64_t fs_final = 0;

	/* Free stream extents if stream-backed */
	if (xa.flags & APFS_XATTR_DATA_STREAM) {
		uint64_t stream_id = xa.stream_id;
		uint8_t dst_key[8];
		w_make_jkey(dst_key, stream_id, APFS_TYPE_DSTREAM_ID);
		uint64_t fs_s = 0;
		if (txn_modify_fs_root(&txn, dst_key, sizeof(dst_key), NULL, 0, true, &fs_s) == 0 && fs_s != 0) {
			vol->tree.root = fs_s;
		}

		struct apfs_cursor cur;
		struct apfs_rec rec;
		if (apfs_seek(&vol->tree, stream_id, APFS_TYPE_FILE_EXTENT, &cur) == 0) {
			while (apfs_cursor_next(&cur, &rec) == 0) {
				if (rec.obj_id != stream_id || rec.type != APFS_TYPE_FILE_EXTENT) break;
				if (rec.val != NULL && rec.val_len >= 16) {
					uint64_t blkcnt = (w_le64(rec.val) & APFS_PEXT_LEN_MASK) / bs;
					if (blkcnt == 0) blkcnt = 1;
					uint64_t phys_bno = w_le64(rec.val + 8);
					if (phys_bno != 0) {
						txn_mark_freed_range(&txn, phys_bno, (uint32_t)blkcnt);
					}
				}
				uint64_t st = 0;
				if (txn_modify_fs_root(&txn, rec.key, rec.key_len, NULL, 0, true, &st) == 0 && st != 0) {
					vol->tree.root = st;
				}
			}
		}
	}

	/* Delete xattr record */
	uint8_t xkey[268];
	w_make_jkey(xkey, obj_id, APFS_TYPE_XATTR);
	w_put_le16(xkey + 8, (uint16_t)(name_len + 1));
	memcpy(xkey + 10, name, name_len + 1);
	uint16_t xklen = (uint16_t)(10 + name_len + 1);

	error = txn_modify_fs_root(&txn, xkey, xklen, NULL, 0, true, &fs_final);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_final;

	/* Update inode ctime */
	uint64_t now = apfs_ticks();
	w_put_le64(inobuf + 32, now);
	uint8_t inokey[8];
	w_make_jkey(inokey, obj_id, APFS_TYPE_INODE);
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey), inobuf, inolen, false, &fs_final);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_final;

	error = txn_commit(&txn, fs_final);
	txn_destroy(&txn);
	return error;
}

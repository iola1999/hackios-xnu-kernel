/*
 * APFS High-level Write & VFS File Operations
 */
#pragma once

/* ------------------------------------------------------------------ helpers */

static int
read_raw_inode(struct apfs_vol *vol, uint64_t obj_id, uint8_t *buf, uint16_t *out_len)
{
	struct apfs_cursor cur;
	struct apfs_rec rec;
	int error = apfs_seek(&vol->tree, obj_id, APFS_TYPE_INODE, &cur);
	if (error) return error;
	error = apfs_cursor_next(&cur, &rec);
	if (error) return error;
	if (rec.obj_id != obj_id || rec.type != APFS_TYPE_INODE || rec.val == NULL) return ENOENT;
	if (rec.val_len < sizeof(struct apfs_j_inode_val) || rec.val_len > 512) return EIO;
	memcpy(buf, rec.val, rec.val_len);
	*out_len = rec.val_len;
	return 0;
}

/* ------------------------------------------------------------------ file writes & vfs */

int
apfs_write_file(struct apfs_vol *vol, uint64_t obj_id, uint64_t off,
    const void *buf, size_t len, struct apfs_write_report *rep)
{
	struct apfs_inode ino;
	int error;
	if (vol == NULL || vol->nx == NULL || (buf == NULL && len > 0)) {
		return EINVAL;
	}
	if ((uint64_t)len > UINT64_MAX - off || off + len < off) return EFBIG;

	if (len == 0) {
		return 0;
	}
	error = apfs_inode_get(vol, obj_id, &ino);
	if (error) {
		return error;
	}

	if (off + len <= ino.size) {
		return apfs_write_in_place(vol, obj_id, off, buf, len, rep);
	}
	uint32_t bs = vol->nx->block_size;
	if (ino.size > UINT64_MAX - (bs - 1) || off + len > UINT64_MAX - (bs - 1)) return EFBIG;
	uint64_t old_blocks = (ino.size + bs - 1) / bs;
	uint64_t new_total_blocks = (off + len + bs - 1) / bs;
	if (new_total_blocks - old_blocks > 64) return EFBIG;

	uint8_t inobuf[512];
	uint16_t inobuf_len = 0;
	error = read_raw_inode(vol, obj_id, inobuf, &inobuf_len);
	if (error) {
		return error;
	}

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	uint32_t buf_pos = 0;
	uint64_t cur_off = off;
	uint64_t cur_len = len;

	if (cur_off < old_blocks * bs) {
		struct apfs_extent ext;
		error = apfs_extent_at(vol, ino.private_id, cur_off, &ext);
		if (error || ext.phys == 0) {
			txn_destroy(&txn);
			return error ? error : EIO;
		}
		uint64_t blk_idx = (cur_off - ext.logical) / bs;
		uint64_t phys_block = ext.phys + blk_idx;
		uint32_t off_in_blk = (uint32_t)(cur_off % bs);
		uint32_t to_write = (cur_len < (uint64_t)(bs - off_in_blk)) ?
		    (uint32_t)cur_len : (uint32_t)(bs - off_in_blk);

		uint8_t *blkbuf = apfs_alloc(bs);
		if (blkbuf == NULL) {
			txn_destroy(&txn);
			return ENOMEM;
		}
		if (txn_read_block(&txn, phys_block, blkbuf, bs) != 0) {
			apfs_free(blkbuf, bs);
			txn_destroy(&txn);
			return EIO;
		}
		memcpy(blkbuf + off_in_blk, (const uint8_t *)buf + buf_pos, to_write);
		error = txn_put(&txn, phys_block, blkbuf, bs);
		apfs_free(blkbuf, bs);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		buf_pos += to_write;
		cur_off += to_write;
		cur_len -= to_write;
	}

	uint32_t add_blocks = (uint32_t)(new_total_blocks - old_blocks);
	uint64_t new_phys = 0;
	if (add_blocks > 0 && cur_len > 0) {
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

		uint64_t write_off_in_new = cur_off - (old_blocks * bs);
		memcpy(zbuf + write_off_in_new, (const uint8_t *)buf + buf_pos, cur_len);

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
		w_make_jkey(ext_key, ino.private_id, APFS_TYPE_FILE_EXTENT);
		w_put_le64(ext_key + 8, old_blocks * bs);

		uint8_t ext_val[24];
		w_put_le64(ext_val, (uint64_t)(add_blocks * bs));
		w_put_le64(ext_val + 8, new_phys);
		w_put_le64(ext_val + 16, 0);

		uint64_t intermediate_fs = 0;
		error = txn_modify_fs_root(&txn, ext_key, sizeof(ext_key),
		    ext_val, sizeof(ext_val), false, &intermediate_fs);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		vol->tree.root = intermediate_fs;
		vol->alloc_count += add_blocks;

		if (vol->extentref_oid != 0) {
			uint8_t erf_key[8];
			w_make_jkey(erf_key, new_phys, APFS_TYPE_EXTENT);

			struct apfs_j_phys_ext_val erf_val;
			memset(&erf_val, 0, sizeof(erf_val));
			erf_val.len_and_kind = (uint64_t)add_blocks | (1ULL << APFS_PEXT_KIND_SHIFT);
			erf_val.owning_obj_id = ino.private_id;
			erf_val.refcnt = 1;

			uint64_t new_er_bno = 0;
			error = txn_modify_btree_root(&txn, vol->extentref_oid,
			    vol->extentref_oid, APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_BTREE,
			    APFS_OBJECT_TYPE_BLOCKREFTREE, erf_key, sizeof(erf_key),
			    (const uint8_t *)&erf_val, sizeof(erf_val), false, &new_er_bno);
			if (error) {
				txn_destroy(&txn);
				return error;
			}
			vol->extentref_oid = new_er_bno;
		}
	}

	/* Update inode record */
	uint8_t inokey[8];
	w_make_jkey(inokey, obj_id, APFS_TYPE_INODE);

	uint64_t now = apfs_ticks();
	w_put_le64(inobuf + 24, now);
	w_put_le64(inobuf + 32, now);
	uint64_t iflags = w_le64(inobuf + 48);
	if ((iflags & (0x4000 | 0x8000)) == 0) iflags |= 0x8000;
	w_put_le64(inobuf + 48, iflags);

	const uint8_t *xf = NULL;
	uint16_t xf_len = 0;
	if (inobuf_len >= 92 &&
	    apfs_xfields_find(inobuf + 92, inobuf_len - 92, APFS_INO_EXT_TYPE_DSTREAM, &xf, &xf_len) == 0 &&
	    xf_len >= 16) {
		uint32_t xf_off = (uint32_t)(xf - inobuf);
		w_put_le64(inobuf + xf_off, off + len);
		w_put_le64(inobuf + xf_off + 8, new_total_blocks * bs);
	}

	uint64_t final_fs = 0;
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey),
	    inobuf, inobuf_len, false, &final_fs);
	if (error) {
		txn_destroy(&txn);
		return error;
	}

	error = txn_commit(&txn, final_fs);
	txn_destroy(&txn);

	if (error == 0 && rep != NULL) {
		rep->bytes = len;
		rep->blocks = add_blocks;
		rep->first_block = new_phys;
	}
	return error;
}

int
apfs_create_file(struct apfs_vol *vol, uint64_t parent_id, const char *name,
    uint16_t mode, uint32_t uid, uint32_t gid, uint64_t *out_id)
{
	if (vol == NULL || vol->nx == NULL || name == NULL) return EINVAL;
	size_t name_len = strnlen(name, 256);
	if (name_len == 0 || strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return EINVAL;
	if (name_len > 255) return ENAMETOOLONG;
	uint64_t existing_id;
	uint8_t existing_type;
	int lookup_error = apfs_lookup(vol, parent_id, name, name_len, &existing_id, &existing_type);
	if (lookup_error == 0) return EEXIST;
	if (lookup_error != ENOENT) return lookup_error;
	uint8_t p_inobuf[512];
	uint16_t p_inolen = 0;
	int error = read_raw_inode(vol, parent_id, p_inobuf, &p_inolen);
	if (error) {
		return error;
	}
	if ((w_le16(p_inobuf + 80) & APFS_S_IFMT) != APFS_S_IFDIR) return ENOTDIR;

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) {
		return error;
	}

	if (vol->next_obj_id < 16) vol->next_obj_id = 16;
	uint64_t new_id = vol->next_obj_id++;

	/* Directory record */
	uint32_t nh = 0;
	apfs_name_hash(name, name_len, vol->hashed_names, &nh);

	uint8_t drec_key[268];
	w_make_jkey(drec_key, parent_id, APFS_TYPE_DIR_REC);
	w_put_le32(drec_key + 8, ((nh & 0x3fffff) << 10) | (uint32_t)(name_len + 1));
	memcpy(drec_key + 12, name, name_len + 1);
	uint16_t drec_klen = (uint16_t)(12 + name_len + 1);

	uint8_t drec_val[18];
	w_put_le64(drec_val, new_id);
	w_put_le64(drec_val + 8, apfs_ticks());
	w_put_le16(drec_val + 16, APFS_DT_REG);

	uint64_t fs_stage1 = 0;
	error = txn_modify_fs_root(&txn, drec_key, drec_klen,
	    drec_val, sizeof(drec_val), false, &fs_stage1);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage1;

	/* Inode record with NAME and DSTREAM xfields */
	uint8_t inokey[8];
	w_make_jkey(inokey, new_id, APFS_TYPE_INODE);

	uint8_t inobuf[512];
	memset(inobuf, 0, sizeof(inobuf));
	w_put_le64(inobuf, parent_id);
	w_put_le64(inobuf + 8, new_id);
	uint64_t now = apfs_ticks();
	w_put_le64(inobuf + 16, now);
	w_put_le64(inobuf + 24, now);
	w_put_le64(inobuf + 32, now);
	w_put_le64(inobuf + 40, now);
	w_put_le64(inobuf + 48, 0x8000); /* INODE_NO_RSRC_FORK */
	w_put_le32(inobuf + 56, 1);
	w_put_le32(inobuf + 68, 0);
	w_put_le32(inobuf + 72, uid);
	w_put_le32(inobuf + 76, gid);
	w_put_le16(inobuf + 80, (mode & 07777) | APFS_S_IFREG);

	size_t name_zlen = name_len + 1;
	size_t name_pad = (name_zlen + 7) & ~7;
	uint8_t *xf = inobuf + 92;
	w_put_le16(xf, 2);
	w_put_le16(xf + 2, (uint16_t)(name_pad + 40));

	xf[4] = 4;    /* APFS_INO_EXT_TYPE_NAME */
	xf[5] = 0x2;  /* XF_DO_NOT_COPY */
	w_put_le16(xf + 6, (uint16_t)name_zlen);

	xf[8] = 8;    /* APFS_INO_EXT_TYPE_DSTREAM */
	xf[9] = 0x20; /* XF_SYSTEM_FIELD */
	w_put_le16(xf + 10, 40);

	memcpy(xf + 12, name, name_zlen);
	memset(xf + 12 + name_zlen, 0, name_pad - name_zlen);

	uint8_t *dst = xf + 12 + name_pad;
	memset(dst, 0, 40);

	uint16_t inobuf_len = (uint16_t)(92 + 12 + name_pad + 40);

	uint64_t fs_stage2 = 0;
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey),
	    inobuf, inobuf_len, false, &fs_stage2);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage2;

	/* DStream record */
	uint8_t dst_key[8];
	w_make_jkey(dst_key, new_id, APFS_TYPE_DSTREAM_ID);
	uint32_t refc = 1;

	uint64_t fs_final = 0;
	error = txn_modify_fs_root(&txn, dst_key, sizeof(dst_key),
	    (const uint8_t *)&refc, sizeof(refc), false, &fs_final);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_final;

	vol->num_files++;

	/* Update parent directory inode nchildren */
	if (p_inolen >= 60) {
		int32_t nch = (int32_t)w_le32(p_inobuf + 56);
		w_put_le32(p_inobuf + 56, (uint32_t)(nch + 1));
		w_put_le64(p_inobuf + 24, now);
		w_put_le64(p_inobuf + 32, now);
		uint64_t ifl = w_le64(p_inobuf + 48);
		if ((ifl & (0x4000 | 0x8000)) == 0) ifl |= 0x8000;
		w_put_le64(p_inobuf + 48, ifl);

		uint8_t p_inokey[8];
		w_make_jkey(p_inokey, parent_id, APFS_TYPE_INODE);
		uint64_t fs_parent_stage = 0;
		error = txn_modify_fs_root(&txn, p_inokey, sizeof(p_inokey), p_inobuf, p_inolen, false, &fs_parent_stage);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		vol->tree.root = fs_parent_stage;
		fs_final = fs_parent_stage;
	}

	error = txn_commit(&txn, fs_final);
	txn_destroy(&txn);

	if (error == 0 && out_id != NULL) {
		*out_id = new_id;
	}
	return error;
}

int
apfs_remove_file(struct apfs_vol *vol, uint64_t parent_id, const char *name)
{
	if (vol == NULL || vol->nx == NULL || name == NULL) return EINVAL;
	size_t name_len = strnlen(name, 256);
	if (name_len == 0 || strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return EINVAL;
	if (name_len > 255) return ENAMETOOLONG;
	uint64_t file_id = 0;
	uint8_t type = 0;
	int error = apfs_lookup(vol, parent_id, name, name_len, &file_id, &type);
	if (error) return error;

	uint8_t p_inobuf[512];
	uint16_t p_inolen = 0;
	error = read_raw_inode(vol, parent_id, p_inobuf, &p_inolen);
	if (error) return error;

	struct apfs_inode file_ino;
	memset(&file_ino, 0, sizeof(file_ino));
	(void)apfs_inode_get(vol, file_id, &file_ino);
	uint64_t stream_id = file_ino.private_id ? file_ino.private_id : file_id;

	struct {
		uint8_t key[16];
		uint16_t key_len;
		uint64_t phys_bno;
		uint64_t blkcnt;
	} ext_to_del[32];
	int num_ext = 0;

	struct apfs_cursor cur;
	struct apfs_rec rec;
	if (apfs_seek(&vol->tree, stream_id, APFS_TYPE_FILE_EXTENT, &cur) == 0) {
		while (apfs_cursor_next(&cur, &rec) == 0 && num_ext < 32) {
			if (rec.obj_id != stream_id || rec.type != APFS_TYPE_FILE_EXTENT) break;
			ext_to_del[num_ext].key_len = rec.key_len < 16 ? rec.key_len : 16;
			memcpy(ext_to_del[num_ext].key, rec.key, ext_to_del[num_ext].key_len);
			if (rec.val != NULL && rec.val_len >= 16) {
				uint64_t blkcnt = (w_le64(rec.val) & APFS_PEXT_LEN_MASK) / vol->nx->block_size;
				if (blkcnt == 0) blkcnt = 1;
				ext_to_del[num_ext].blkcnt = blkcnt;
				ext_to_del[num_ext].phys_bno = w_le64(rec.val + 8);
			} else {
				ext_to_del[num_ext].blkcnt = 0;
				ext_to_del[num_ext].phys_bno = 0;
			}
			num_ext++;
		}
	}

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	uint32_t nh = 0;
	apfs_name_hash(name, name_len, vol->hashed_names, &nh);

	uint8_t drec_key[268];
	w_make_jkey(drec_key, parent_id, APFS_TYPE_DIR_REC);
	w_put_le32(drec_key + 8, ((nh & 0x3fffff) << 10) | (uint32_t)(name_len + 1));
	memcpy(drec_key + 12, name, name_len + 1);
	uint16_t drec_klen = (uint16_t)(12 + name_len + 1);

	uint64_t fs_stage1 = 0;
	error = txn_modify_fs_root(&txn, drec_key, drec_klen, NULL, 0, true, &fs_stage1);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage1;

	uint8_t inokey[8];
	w_make_jkey(inokey, file_id, APFS_TYPE_INODE);

	uint64_t fs_stage2 = 0;
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey), NULL, 0, true, &fs_stage2);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage2;
	uint64_t fs_final = fs_stage2;

	/* Delete DSTREAM_ID record */
	uint8_t dst_key[8];
	w_make_jkey(dst_key, stream_id, APFS_TYPE_DSTREAM_ID);
	uint64_t fs_stage3 = 0;
	if (txn_modify_fs_root(&txn, dst_key, sizeof(dst_key), NULL, 0, true, &fs_stage3) == 0 && fs_stage3 != 0) {
		vol->tree.root = fs_stage3;
		fs_final = fs_stage3;
	}

	/* Delete FILE_EXTENT records */
	for (int e = 0; e < num_ext; e++) {
		if (ext_to_del[e].blkcnt > 0) {
			if (vol->alloc_count >= ext_to_del[e].blkcnt) {
				vol->alloc_count -= ext_to_del[e].blkcnt;
			}
			if (vol->extentref_oid != 0 && ext_to_del[e].phys_bno != 0) {
				uint8_t erf_key[8];
				w_make_jkey(erf_key, ext_to_del[e].phys_bno, APFS_TYPE_EXTENT);
				uint64_t new_er_bno = 0;
				if (txn_modify_btree_root(&txn, vol->extentref_oid, vol->extentref_oid,
				    APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_BTREE, APFS_OBJECT_TYPE_BLOCKREFTREE,
				    erf_key, sizeof(erf_key), NULL, 0, true, &new_er_bno) == 0 && new_er_bno != 0) {
					vol->extentref_oid = new_er_bno;
				}
			}
			if (ext_to_del[e].phys_bno != 0) {
				txn_mark_freed_range(&txn, ext_to_del[e].phys_bno, (uint32_t)ext_to_del[e].blkcnt);
			}
		}
		uint64_t stage_ext = 0;
		if (txn_modify_fs_root(&txn, ext_to_del[e].key, ext_to_del[e].key_len, NULL, 0, true, &stage_ext) == 0 && stage_ext != 0) {
			vol->tree.root = stage_ext;
			fs_final = stage_ext;
		}
	}

	/* Update parent directory inode nchildren */
	if (p_inolen >= 60) {
		int32_t nch = (int32_t)w_le32(p_inobuf + 56);
		if (nch > 0) w_put_le32(p_inobuf + 56, (uint32_t)(nch - 1));
		uint64_t now = apfs_ticks();
		w_put_le64(p_inobuf + 24, now);
		w_put_le64(p_inobuf + 32, now);
		uint64_t ifl = w_le64(p_inobuf + 48);
		if ((ifl & (0x4000 | 0x8000)) == 0) ifl |= 0x8000;
		w_put_le64(p_inobuf + 48, ifl);

		uint8_t p_inokey[8];
		w_make_jkey(p_inokey, parent_id, APFS_TYPE_INODE);
		uint64_t fs_parent_stage = 0;
		if (txn_modify_fs_root(&txn, p_inokey, sizeof(p_inokey), p_inobuf, p_inolen, false, &fs_parent_stage) == 0 && fs_parent_stage != 0) {
			vol->tree.root = fs_parent_stage;
			fs_final = fs_parent_stage;
		}
	}

	if (vol->num_files > 0) vol->num_files--;
	error = txn_commit(&txn, fs_final);
	txn_destroy(&txn);
	return error;
}

int
apfs_set_file_size(struct apfs_vol *vol, uint64_t obj_id, uint64_t new_size)
{
	if (vol == NULL || vol->nx == NULL) return EINVAL;
	struct apfs_inode inode;
	int error = apfs_inode_get(vol, obj_id, &inode);
	if (error) return error;
	if (new_size == inode.size) return 0;
	if (new_size > inode.size) return EOPNOTSUPP;
	uint32_t bs = vol->nx->block_size;

	uint8_t inobuf[512];
	uint16_t inobuf_len = 0;
	error = read_raw_inode(vol, obj_id, inobuf, &inobuf_len);
	if (error) return error;

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	if (new_size == 0) {
		uint64_t stream_id = inode.private_id ? inode.private_id : obj_id;
		struct {
			uint8_t key[16];
			uint16_t key_len;
			uint64_t phys_bno;
			uint64_t blkcnt;
		} ext_to_del[32];
		int num_ext = 0;

		struct apfs_cursor cur;
		struct apfs_rec rec;
		if (apfs_seek(&vol->tree, stream_id, APFS_TYPE_FILE_EXTENT, &cur) == 0) {
			while (apfs_cursor_next(&cur, &rec) == 0 && num_ext < 32) {
				if (rec.obj_id != stream_id || rec.type != APFS_TYPE_FILE_EXTENT) break;
				ext_to_del[num_ext].key_len = rec.key_len < 16 ? rec.key_len : 16;
				memcpy(ext_to_del[num_ext].key, rec.key, ext_to_del[num_ext].key_len);
				if (rec.val != NULL && rec.val_len >= 16) {
					uint64_t blkcnt = (w_le64(rec.val) & APFS_PEXT_LEN_MASK) / bs;
					if (blkcnt == 0) blkcnt = 1;
					ext_to_del[num_ext].blkcnt = blkcnt;
					ext_to_del[num_ext].phys_bno = w_le64(rec.val + 8);
				} else {
					ext_to_del[num_ext].blkcnt = 0;
					ext_to_del[num_ext].phys_bno = 0;
				}
				num_ext++;
			}
		}

		for (int e = 0; e < num_ext; e++) {
			if (ext_to_del[e].blkcnt > 0) {
				if (vol->alloc_count >= ext_to_del[e].blkcnt) {
					vol->alloc_count -= ext_to_del[e].blkcnt;
				}
				if (vol->extentref_oid != 0 && ext_to_del[e].phys_bno != 0) {
					uint8_t erf_key[8];
					w_make_jkey(erf_key, ext_to_del[e].phys_bno, APFS_TYPE_EXTENT);
					uint64_t new_er_bno = 0;
					if (txn_modify_btree_root(&txn, vol->extentref_oid, vol->extentref_oid,
					    APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_BTREE, APFS_OBJECT_TYPE_BLOCKREFTREE,
					    erf_key, sizeof(erf_key), NULL, 0, true, &new_er_bno) == 0 && new_er_bno != 0) {
						vol->extentref_oid = new_er_bno;
					}
				}
				if (ext_to_del[e].phys_bno != 0) {
					txn_mark_freed_range(&txn, ext_to_del[e].phys_bno, (uint32_t)ext_to_del[e].blkcnt);
				}
			}
			uint64_t stage_ext = 0;
			if (txn_modify_fs_root(&txn, ext_to_del[e].key, ext_to_del[e].key_len, NULL, 0, true, &stage_ext) == 0 && stage_ext != 0) {
				vol->tree.root = stage_ext;
			}
		}
	} else if ((new_size - 1) / bs != (inode.size - 1) / bs) {
		txn_destroy(&txn);
		return EOPNOTSUPP;
	}

	uint8_t inokey[8];
	w_make_jkey(inokey, obj_id, APFS_TYPE_INODE);

	uint64_t now = apfs_ticks();
	w_put_le64(inobuf + 24, now);
	w_put_le64(inobuf + 32, now);
	uint64_t iflags = w_le64(inobuf + 48);
	if ((iflags & (0x4000 | 0x8000)) == 0) iflags |= 0x8000;
	w_put_le64(inobuf + 48, iflags);

	const uint8_t *xf = NULL;
	uint16_t xf_len = 0;
	if (inobuf_len >= 92 &&
	    apfs_xfields_find(inobuf + 92, inobuf_len - 92, APFS_INO_EXT_TYPE_DSTREAM, &xf, &xf_len) == 0 &&
	    xf_len >= 16) {
		uint32_t xf_off = (uint32_t)(xf - inobuf);
		w_put_le64(inobuf + xf_off, new_size);
		if (new_size == 0) {
			w_put_le64(inobuf + xf_off + 8, 0);
		}
	}

	uint64_t fs_final = 0;
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey),
	    inobuf, inobuf_len, false, &fs_final);
	if (error) {
		txn_destroy(&txn);
		return error;
	}

	error = txn_commit(&txn, fs_final);
	txn_destroy(&txn);
	return error;
}

int
apfs_set_file_attrs(struct apfs_vol *vol, uint64_t obj_id, uint16_t mode,
    uint32_t uid, uint32_t gid, uint64_t mtime_ns)
{
	if (vol == NULL || vol->nx == NULL) return EINVAL;
	uint8_t inobuf[512];
	uint16_t inobuf_len = 0;
	int error = read_raw_inode(vol, obj_id, inobuf, &inobuf_len);
	if (error) return error;

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	uint8_t inokey[8];
	w_make_jkey(inokey, obj_id, APFS_TYPE_INODE);

	uint64_t now = mtime_ns ? mtime_ns : apfs_ticks();
	w_put_le64(inobuf + 24, now);
	w_put_le64(inobuf + 32, now);
	uint64_t iflags = w_le64(inobuf + 48);
	if ((iflags & (0x4000 | 0x8000)) == 0) iflags |= 0x8000;
	w_put_le64(inobuf + 48, iflags);

	if (mode) w_put_le16(inobuf + 80, mode);
	if (uid) w_put_le32(inobuf + 72, uid);
	if (gid) w_put_le32(inobuf + 76, gid);

	uint64_t fs_final = 0;
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey),
	    inobuf, inobuf_len, false, &fs_final);
	if (error) {
		txn_destroy(&txn);
		return error;
	}

	error = txn_commit(&txn, fs_final);
	txn_destroy(&txn);
	return error;
}

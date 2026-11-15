/*
 * APFS Directory and Link Transactional Operations
 */
#pragma once

int
apfs_create_dir(struct apfs_vol *vol, uint64_t parent_id, const char *name,
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
	if (error) return error;
	if ((w_le16(p_inobuf + 80) & APFS_S_IFMT) != APFS_S_IFDIR) return ENOTDIR;

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	if (vol->next_obj_id < 16) vol->next_obj_id = 16;
	uint64_t new_id = vol->next_obj_id++;

	/* Directory record in parent */
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
	w_put_le16(drec_val + 16, APFS_DT_DIR);

	uint64_t fs_stage1 = 0;
	error = txn_modify_fs_root(&txn, drec_key, drec_klen,
	    drec_val, sizeof(drec_val), false, &fs_stage1);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage1;

	/* Inode record with NAME xfield */
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
	w_put_le64(inobuf + 48, 0x8000 | 0x0002); /* INODE_NO_RSRC_FORK | INODE_IS_APFS_DIR */
	w_put_le32(inobuf + 56, 0);               /* nchildren = 0 */
	w_put_le32(inobuf + 68, 0);
	w_put_le32(inobuf + 72, uid);
	w_put_le32(inobuf + 76, gid);
	w_put_le16(inobuf + 80, (mode & 07777) | APFS_S_IFDIR);

	size_t name_zlen = name_len + 1;
	size_t name_pad = (name_zlen + 7) & ~7;
	uint8_t *xf = inobuf + 92;
	w_put_le16(xf, 1);
	w_put_le16(xf + 2, (uint16_t)name_pad);

	xf[4] = 4;    /* APFS_INO_EXT_TYPE_NAME */
	xf[5] = 0x2;  /* XF_DO_NOT_COPY */
	w_put_le16(xf + 6, (uint16_t)name_zlen);

	memcpy(xf + 8, name, name_zlen);
	memset(xf + 8 + name_zlen, 0, name_pad - name_zlen);

	uint16_t inobuf_len = (uint16_t)(92 + 8 + name_pad);

	uint64_t fs_stage2 = 0;
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey),
	    inobuf, inobuf_len, false, &fs_stage2);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage2;
	uint64_t fs_final = fs_stage2;

	vol->num_dirs++;

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
apfs_remove_dir(struct apfs_vol *vol, uint64_t parent_id, const char *name)
{
	if (vol == NULL || vol->nx == NULL || name == NULL) return EINVAL;
	size_t name_len = strnlen(name, 256);
	if (name_len == 0 || strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return EINVAL;
	if (name_len > 255) return ENAMETOOLONG;

	uint64_t dir_id = 0;
	uint8_t type = 0;
	int error = apfs_lookup(vol, parent_id, name, name_len, &dir_id, &type);
	if (error) return error;
	if (type != APFS_DT_DIR) return ENOTDIR;

	uint8_t dir_inobuf[512];
	uint16_t dir_inolen = 0;
	error = read_raw_inode(vol, dir_id, dir_inobuf, &dir_inolen);
	if (error) return error;

	/* Verify directory is empty */
	uint32_t nchildren = w_le32(dir_inobuf + 56);
	if (nchildren > 0) return ENOTEMPTY;

	uint8_t p_inobuf[512];
	uint16_t p_inolen = 0;
	error = read_raw_inode(vol, parent_id, p_inobuf, &p_inolen);
	if (error) return error;

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	/* Delete DIR_REC from parent */
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

	/* Delete directory INODE */
	uint8_t inokey[8];
	w_make_jkey(inokey, dir_id, APFS_TYPE_INODE);

	uint64_t fs_stage2 = 0;
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey), NULL, 0, true, &fs_stage2);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage2;
	uint64_t fs_final = fs_stage2;

	if (vol->num_dirs > 0) vol->num_dirs--;

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
	return error;
}

int
apfs_create_symlink(struct apfs_vol *vol, uint64_t parent_id, const char *name,
    const char *target, uint32_t uid, uint32_t gid, uint64_t *out_id)
{
	if (vol == NULL || vol->nx == NULL || name == NULL || target == NULL) return EINVAL;
	size_t name_len = strnlen(name, 256);
	if (name_len == 0 || strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return EINVAL;
	if (name_len > 255) return ENAMETOOLONG;
	size_t target_len = strlen(target);
	if (target_len == 0 || target_len > 1024) return ENAMETOOLONG;

	uint64_t existing_id;
	uint8_t existing_type;
	int lookup_error = apfs_lookup(vol, parent_id, name, name_len, &existing_id, &existing_type);
	if (lookup_error == 0) return EEXIST;
	if (lookup_error != ENOENT) return lookup_error;

	uint8_t p_inobuf[512];
	uint16_t p_inolen = 0;
	int error = read_raw_inode(vol, parent_id, p_inobuf, &p_inolen);
	if (error) return error;
	if ((w_le16(p_inobuf + 80) & APFS_S_IFMT) != APFS_S_IFDIR) return ENOTDIR;

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	if (vol->next_obj_id < 16) vol->next_obj_id = 16;
	uint64_t new_id = vol->next_obj_id++;

	/* Directory record in parent */
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
	w_put_le16(drec_val + 16, APFS_DT_LNK);

	uint64_t fs_stage1 = 0;
	error = txn_modify_fs_root(&txn, drec_key, drec_klen,
	    drec_val, sizeof(drec_val), false, &fs_stage1);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage1;

	/* Inode record with NAME xfield */
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
	w_put_le32(inobuf + 56, 1);      /* nlink = 1 */
	w_put_le32(inobuf + 68, 0);
	w_put_le32(inobuf + 72, uid);
	w_put_le32(inobuf + 76, gid);
	w_put_le16(inobuf + 80, 0777 | APFS_S_IFLNK);

	size_t name_zlen = name_len + 1;
	size_t name_pad = (name_zlen + 7) & ~7;
	uint8_t *xf = inobuf + 92;
	w_put_le16(xf, 1);
	w_put_le16(xf + 2, (uint16_t)name_pad);

	xf[4] = 4;    /* APFS_INO_EXT_TYPE_NAME */
	xf[5] = 0x2;  /* XF_DO_NOT_COPY */
	w_put_le16(xf + 6, (uint16_t)name_zlen);

	memcpy(xf + 8, name, name_zlen);
	memset(xf + 8 + name_zlen, 0, name_pad - name_zlen);

	uint16_t inobuf_len = (uint16_t)(92 + 8 + name_pad);

	uint64_t fs_stage2 = 0;
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey),
	    inobuf, inobuf_len, false, &fs_stage2);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage2;

	/* Embedded xattr record for "com.apple.fs.symlink" */
	const char *ea_name = APFS_SYMLINK_EA_NAME;
	size_t ea_name_len = strlen(ea_name);
	uint8_t xkey[268];
	w_make_jkey(xkey, new_id, APFS_TYPE_XATTR);
	w_put_le16(xkey + 8, (uint16_t)(ea_name_len + 1));
	memcpy(xkey + 10, ea_name, ea_name_len + 1);
	uint16_t xklen = (uint16_t)(10 + ea_name_len + 1);

	uint8_t xval[1050];
	w_put_le16(xval, APFS_XATTR_DATA_EMBEDDED);
	w_put_le16(xval + 2, (uint16_t)(target_len + 1));
	memcpy(xval + 4, target, target_len + 1);
	uint16_t xvlen = (uint16_t)(4 + target_len + 1);

	uint64_t fs_stage3 = 0;
	error = txn_modify_fs_root(&txn, xkey, xklen, xval, xvlen, false, &fs_stage3);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage3;
	uint64_t fs_final = fs_stage3;

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
apfs_create_link(struct apfs_vol *vol, uint64_t target_id, uint64_t parent_id, const char *name)
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

	uint8_t t_inobuf[512];
	uint16_t t_inolen = 0;
	int error = read_raw_inode(vol, target_id, t_inobuf, &t_inolen);
	if (error) return error;

	uint16_t t_mode = w_le16(t_inobuf + 80);
	if ((t_mode & APFS_S_IFMT) == APFS_S_IFDIR) return EPERM;

	uint8_t p_inobuf[512];
	uint16_t p_inolen = 0;
	error = read_raw_inode(vol, parent_id, p_inobuf, &p_inolen);
	if (error) return error;
	if ((w_le16(p_inobuf + 80) & APFS_S_IFMT) != APFS_S_IFDIR) return ENOTDIR;

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	/* Increment target inode nlink */
	uint32_t t_nlink = w_le32(t_inobuf + 56);
	w_put_le32(t_inobuf + 56, t_nlink + 1);
	uint64_t now = apfs_ticks();
	w_put_le64(t_inobuf + 32, now); /* ctime */

	uint8_t t_inokey[8];
	w_make_jkey(t_inokey, target_id, APFS_TYPE_INODE);
	uint64_t fs_stage1 = 0;
	error = txn_modify_fs_root(&txn, t_inokey, sizeof(t_inokey), t_inobuf, t_inolen, false, &fs_stage1);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage1;

	/* Determine directory record file type */
	uint16_t dt = APFS_DT_REG;
	if ((t_mode & APFS_S_IFMT) == APFS_S_IFLNK) dt = APFS_DT_LNK;
	else if ((t_mode & APFS_S_IFMT) == APFS_S_IFSOCK) dt = APFS_DT_SOCK;
	else if ((t_mode & APFS_S_IFMT) == APFS_S_IFIFO) dt = APFS_DT_FIFO;
	else if ((t_mode & APFS_S_IFMT) == APFS_S_IFCHR) dt = APFS_DT_CHR;
	else if ((t_mode & APFS_S_IFMT) == APFS_S_IFBLK) dt = APFS_DT_BLK;

	/* Directory record in parent */
	uint32_t nh = 0;
	apfs_name_hash(name, name_len, vol->hashed_names, &nh);

	uint8_t drec_key[268];
	w_make_jkey(drec_key, parent_id, APFS_TYPE_DIR_REC);
	w_put_le32(drec_key + 8, ((nh & 0x3fffff) << 10) | (uint32_t)(name_len + 1));
	memcpy(drec_key + 12, name, name_len + 1);
	uint16_t drec_klen = (uint16_t)(12 + name_len + 1);

	uint8_t drec_val[18];
	w_put_le64(drec_val, target_id);
	w_put_le64(drec_val + 8, now);
	w_put_le16(drec_val + 16, dt);

	uint64_t fs_stage2 = 0;
	error = txn_modify_fs_root(&txn, drec_key, drec_klen,
	    drec_val, sizeof(drec_val), false, &fs_stage2);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage2;
	uint64_t fs_final = fs_stage2;

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
	return error;
}

int
apfs_create_mknod(struct apfs_vol *vol, uint64_t parent_id, const char *name,
    uint16_t mode, uint32_t rdev, uint32_t uid, uint32_t gid, uint64_t *out_id)
{
	if (vol == NULL || vol->nx == NULL || name == NULL) return EINVAL;
	size_t name_len = strnlen(name, 256);
	if (name_len == 0 || strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return EINVAL;
	if (name_len > 255) return ENAMETOOLONG;

	uint16_t fmt = mode & APFS_S_IFMT;
	uint16_t dt = APFS_DT_REG;
	if (fmt == APFS_S_IFSOCK) dt = APFS_DT_SOCK;
	else if (fmt == APFS_S_IFIFO) dt = APFS_DT_FIFO;
	else if (fmt == APFS_S_IFCHR) dt = APFS_DT_CHR;
	else if (fmt == APFS_S_IFBLK) dt = APFS_DT_BLK;
	else return EINVAL;

	uint64_t existing_id;
	uint8_t existing_type;
	int lookup_error = apfs_lookup(vol, parent_id, name, name_len, &existing_id, &existing_type);
	if (lookup_error == 0) return EEXIST;
	if (lookup_error != ENOENT) return lookup_error;

	uint8_t p_inobuf[512];
	uint16_t p_inolen = 0;
	int error = read_raw_inode(vol, parent_id, p_inobuf, &p_inolen);
	if (error) return error;
	if ((w_le16(p_inobuf + 80) & APFS_S_IFMT) != APFS_S_IFDIR) return ENOTDIR;

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	if (vol->next_obj_id < 16) vol->next_obj_id = 16;
	uint64_t new_id = vol->next_obj_id++;

	/* Directory record in parent */
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
	w_put_le16(drec_val + 16, dt);

	uint64_t fs_stage1 = 0;
	error = txn_modify_fs_root(&txn, drec_key, drec_klen,
	    drec_val, sizeof(drec_val), false, &fs_stage1);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage1;

	/* Inode record with NAME (and optional RDEV) xfield */
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
	w_put_le32(inobuf + 56, 1);      /* nlink = 1 */
	w_put_le32(inobuf + 68, 0);
	w_put_le32(inobuf + 72, uid);
	w_put_le32(inobuf + 76, gid);
	w_put_le16(inobuf + 80, mode);

	size_t name_zlen = name_len + 1;
	size_t name_pad = (name_zlen + 7) & ~7;
	uint8_t *xf = inobuf + 92;
	uint16_t inobuf_len;

	if (fmt == APFS_S_IFCHR || fmt == APFS_S_IFBLK) {
		w_put_le16(xf, 2);
		w_put_le16(xf + 2, (uint16_t)(name_pad + 8));

		xf[4] = 4;    /* APFS_INO_EXT_TYPE_NAME */
		xf[5] = 0x2;  /* XF_DO_NOT_COPY */
		w_put_le16(xf + 6, (uint16_t)name_zlen);

		xf[8] = 14;   /* APFS_INO_EXT_TYPE_RDEV */
		xf[9] = 0x0;
		w_put_le16(xf + 10, 4);

		memcpy(xf + 12, name, name_zlen);
		memset(xf + 12 + name_zlen, 0, name_pad - name_zlen);
		w_put_le32(xf + 12 + name_pad, rdev);
		inobuf_len = (uint16_t)(92 + 12 + name_pad + 8);
	} else {
		w_put_le16(xf, 1);
		w_put_le16(xf + 2, (uint16_t)name_pad);

		xf[4] = 4;    /* APFS_INO_EXT_TYPE_NAME */
		xf[5] = 0x2;  /* XF_DO_NOT_COPY */
		w_put_le16(xf + 6, (uint16_t)name_zlen);

		memcpy(xf + 8, name, name_zlen);
		memset(xf + 8 + name_zlen, 0, name_pad - name_zlen);
		inobuf_len = (uint16_t)(92 + 8 + name_pad);
	}

	uint64_t fs_stage2 = 0;
	error = txn_modify_fs_root(&txn, inokey, sizeof(inokey),
	    inobuf, inobuf_len, false, &fs_stage2);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_stage2;
	uint64_t fs_final = fs_stage2;

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
apfs_rename_file(struct apfs_vol *vol, uint64_t from_parent, const char *from_name,
    uint64_t to_parent, const char *to_name)
{
	if (vol == NULL || vol->nx == NULL || from_name == NULL || to_name == NULL) return EINVAL;
	size_t from_len = strnlen(from_name, 256);
	size_t to_len = strnlen(to_name, 256);
	if (from_len == 0 || to_len == 0 || from_len > 255 || to_len > 255) return EINVAL;
	if (strchr(from_name, '/') != NULL || strchr(to_name, '/') != NULL) return EINVAL;
	if (strcmp(from_name, ".") == 0 || strcmp(from_name, "..") == 0) return EINVAL;
	if (strcmp(to_name, ".") == 0 || strcmp(to_name, "..") == 0) return EINVAL;

	uint64_t src_id = 0;
	uint8_t src_type = 0;
	int error = apfs_lookup(vol, from_parent, from_name, from_len, &src_id, &src_type);
	if (error) return error;

	uint8_t src_inobuf[512];
	uint16_t src_inolen = 0;
	error = read_raw_inode(vol, src_id, src_inobuf, &src_inolen);
	if (error) return error;

	uint64_t dst_id = 0;
	uint8_t dst_type = 0;
	bool dst_exists = (apfs_lookup(vol, to_parent, to_name, to_len, &dst_id, &dst_type) == 0);

	if (dst_exists && src_id == dst_id) {
		return 0; /* Renaming to itself */
	}

	uint8_t dst_inobuf[512];
	uint16_t dst_inolen = 0;
	if (dst_exists) {
		error = read_raw_inode(vol, dst_id, dst_inobuf, &dst_inolen);
		if (error) return error;

		if (src_type == APFS_DT_DIR) {
			if (dst_type != APFS_DT_DIR) return ENOTDIR;
			uint32_t dst_nchildren = w_le32(dst_inobuf + 56);
			if (dst_nchildren > 0) return ENOTEMPTY;
		} else {
			if (dst_type == APFS_DT_DIR) return EISDIR;
		}
	}

	uint8_t from_p_inobuf[512];
	uint16_t from_p_inolen = 0;
	error = read_raw_inode(vol, from_parent, from_p_inobuf, &from_p_inolen);
	if (error) return error;

	uint8_t to_p_inobuf[512];
	uint16_t to_p_inolen = 0;
	if (from_parent == to_parent) {
		memcpy(to_p_inobuf, from_p_inobuf, from_p_inolen);
		to_p_inolen = from_p_inolen;
	} else {
		error = read_raw_inode(vol, to_parent, to_p_inobuf, &to_p_inolen);
		if (error) return error;
	}

	struct apfs_txn txn;
	error = txn_begin(vol, &txn);
	if (error) return error;

	uint64_t fs_final = 0;

	/* 1. If destination exists, remove it */
	if (dst_exists) {
		uint32_t dst_nh = 0;
		apfs_name_hash(to_name, to_len, vol->hashed_names, &dst_nh);

		uint8_t dst_drec_key[268];
		w_make_jkey(dst_drec_key, to_parent, APFS_TYPE_DIR_REC);
		w_put_le32(dst_drec_key + 8, ((dst_nh & 0x3fffff) << 10) | (uint32_t)(to_len + 1));
		memcpy(dst_drec_key + 12, to_name, to_len + 1);
		uint16_t dst_drec_klen = (uint16_t)(12 + to_len + 1);

		error = txn_modify_fs_root(&txn, dst_drec_key, dst_drec_klen, NULL, 0, true, &fs_final);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		vol->tree.root = fs_final;

		if (dst_type == APFS_DT_DIR) {
			uint8_t dst_inokey[8];
			w_make_jkey(dst_inokey, dst_id, APFS_TYPE_INODE);
			error = txn_modify_fs_root(&txn, dst_inokey, sizeof(dst_inokey), NULL, 0, true, &fs_final);
			if (error) {
				txn_destroy(&txn);
				return error;
			}
			vol->tree.root = fs_final;
			if (vol->num_dirs > 0) vol->num_dirs--;
		} else {
			uint32_t d_nlink = w_le32(dst_inobuf + 56);
			if (d_nlink <= 1) {
				uint8_t dst_inokey[8];
				w_make_jkey(dst_inokey, dst_id, APFS_TYPE_INODE);
				error = txn_modify_fs_root(&txn, dst_inokey, sizeof(dst_inokey), NULL, 0, true, &fs_final);
				if (error) {
					txn_destroy(&txn);
					return error;
				}
				vol->tree.root = fs_final;
				if (vol->num_files > 0) vol->num_files--;
			} else {
				w_put_le32(dst_inobuf + 56, d_nlink - 1);
				uint8_t dst_inokey[8];
				w_make_jkey(dst_inokey, dst_id, APFS_TYPE_INODE);
				error = txn_modify_fs_root(&txn, dst_inokey, sizeof(dst_inokey), dst_inobuf, dst_inolen, false, &fs_final);
				if (error) {
					txn_destroy(&txn);
					return error;
				}
				vol->tree.root = fs_final;
			}
		}
	}

	/* 2. Remove source DIR_REC from from_parent */
	uint32_t src_nh = 0;
	apfs_name_hash(from_name, from_len, vol->hashed_names, &src_nh);

	uint8_t src_drec_key[268];
	w_make_jkey(src_drec_key, from_parent, APFS_TYPE_DIR_REC);
	w_put_le32(src_drec_key + 8, ((src_nh & 0x3fffff) << 10) | (uint32_t)(from_len + 1));
	memcpy(src_drec_key + 12, from_name, from_len + 1);
	uint16_t src_drec_klen = (uint16_t)(12 + from_len + 1);

	error = txn_modify_fs_root(&txn, src_drec_key, src_drec_klen, NULL, 0, true, &fs_final);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_final;

	/* 3. Insert new DIR_REC under to_parent */
	uint32_t new_nh = 0;
	apfs_name_hash(to_name, to_len, vol->hashed_names, &new_nh);

	uint8_t new_drec_key[268];
	w_make_jkey(new_drec_key, to_parent, APFS_TYPE_DIR_REC);
	w_put_le32(new_drec_key + 8, ((new_nh & 0x3fffff) << 10) | (uint32_t)(to_len + 1));
	memcpy(new_drec_key + 12, to_name, to_len + 1);
	uint16_t new_drec_klen = (uint16_t)(12 + to_len + 1);

	uint8_t new_drec_val[18];
	w_put_le64(new_drec_val, src_id);
	uint64_t now = apfs_ticks();
	w_put_le64(new_drec_val + 8, now);
	w_put_le16(new_drec_val + 16, src_type);

	error = txn_modify_fs_root(&txn, new_drec_key, new_drec_klen,
	    new_drec_val, sizeof(new_drec_val), false, &fs_final);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_final;

	/* 4. Update source inode parent_id, ctime, and NAME xfield */
	w_put_le64(src_inobuf, to_parent);
	w_put_le64(src_inobuf + 32, now);

	size_t to_zlen = to_len + 1;
	size_t to_pad = (to_zlen + 7) & ~7;
	uint8_t *xf = src_inobuf + 92;
	uint16_t num_ext = w_le16(xf);
	if (num_ext >= 1 && xf[4] == 4) {
		w_put_le16(xf + 6, (uint16_t)to_zlen);
		if (num_ext == 1) {
			w_put_le16(xf + 2, (uint16_t)to_pad);
			memcpy(xf + 8, to_name, to_zlen);
			memset(xf + 8 + to_zlen, 0, to_pad - to_zlen);
			src_inolen = (uint16_t)(92 + 8 + to_pad);
		} else if (num_ext == 2 && xf[8] == 8) {
			uint8_t dst_copy[40];
			memcpy(dst_copy, xf + 12 + ((w_le16(xf + 6) + 7) & ~7), 40);
			w_put_le16(xf + 2, (uint16_t)(to_pad + 40));
			memcpy(xf + 12, to_name, to_zlen);
			memset(xf + 12 + to_zlen, 0, to_pad - to_zlen);
			memcpy(xf + 12 + to_pad, dst_copy, 40);
			src_inolen = (uint16_t)(92 + 12 + to_pad + 40);
		}
	}

	uint8_t src_inokey[8];
	w_make_jkey(src_inokey, src_id, APFS_TYPE_INODE);
	error = txn_modify_fs_root(&txn, src_inokey, sizeof(src_inokey), src_inobuf, src_inolen, false, &fs_final);
	if (error) {
		txn_destroy(&txn);
		return error;
	}
	vol->tree.root = fs_final;

	/* 5. Update directory nchildren */
	if (from_parent != to_parent) {
		int32_t from_nch = (int32_t)w_le32(from_p_inobuf + 56);
		if (from_nch > 0) w_put_le32(from_p_inobuf + 56, (uint32_t)(from_nch - 1));
		w_put_le64(from_p_inobuf + 24, now);
		w_put_le64(from_p_inobuf + 32, now);
		uint8_t f_pkey[8];
		w_make_jkey(f_pkey, from_parent, APFS_TYPE_INODE);
		error = txn_modify_fs_root(&txn, f_pkey, sizeof(f_pkey), from_p_inobuf, from_p_inolen, false, &fs_final);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		vol->tree.root = fs_final;

		if (!dst_exists) {
			int32_t to_nch = (int32_t)w_le32(to_p_inobuf + 56);
			w_put_le32(to_p_inobuf + 56, (uint32_t)(to_nch + 1));
			w_put_le64(to_p_inobuf + 24, now);
			w_put_le64(to_p_inobuf + 32, now);
			uint8_t t_pkey[8];
			w_make_jkey(t_pkey, to_parent, APFS_TYPE_INODE);
			error = txn_modify_fs_root(&txn, t_pkey, sizeof(t_pkey), to_p_inobuf, to_p_inolen, false, &fs_final);
			if (error) {
				txn_destroy(&txn);
				return error;
			}
			vol->tree.root = fs_final;
		}
	} else if (dst_exists) {
		int32_t nch = (int32_t)w_le32(from_p_inobuf + 56);
		if (nch > 0) w_put_le32(from_p_inobuf + 56, (uint32_t)(nch - 1));
		w_put_le64(from_p_inobuf + 24, now);
		w_put_le64(from_p_inobuf + 32, now);
		uint8_t pkey[8];
		w_make_jkey(pkey, from_parent, APFS_TYPE_INODE);
		error = txn_modify_fs_root(&txn, pkey, sizeof(pkey), from_p_inobuf, from_p_inolen, false, &fs_final);
		if (error) {
			txn_destroy(&txn);
			return error;
		}
		vol->tree.root = fs_final;
	}

	error = txn_commit(&txn, fs_final);
	txn_destroy(&txn);
	return error;
}

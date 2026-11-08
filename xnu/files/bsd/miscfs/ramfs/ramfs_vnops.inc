static int
ramfs_lookup(struct vnop_lookup_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct ramfs_node *dir = RAMFS_NODE(dvp);
	struct ramfs_mount *rm = dir->rn_mount;
	int error;

	*ap->a_vpp = NULLVP;
	if (dir->rn_type != VDIR) {
		return ENOTDIR;
	}
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		error = vnode_get(dvp);
		if (!error) {
			*ap->a_vpp = dvp;
		}
		return error;
	}

	do {
		struct ramfs_node *target;
		bool dotdot = (cnp->cn_flags & ISDOTDOT) != 0;

		lck_mtx_lock(&rm->rm_lock);
		if (dotdot) {
			target = dir->rn_parent;
		} else {
			struct ramfs_dirent *de = ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen);

			target = de ? de->rd_node : NULL;
		}
		if (target == NULL) {
			lck_mtx_unlock(&rm->rm_lock);
			/* a name that may be created: tell VFS to go ahead (unless the directory itself is gone) */
			if (!dotdot && dir->rn_nlink != 0 && (cnp->cn_flags & ISLASTCN) &&
			    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)) {
				return EJUSTRETURN;
			}
			return ENOENT;
		}
		error = ramfs_vget_locked(rm, target, dotdot ? NULLVP : dvp, dotdot ? NULL : cnp, ap->a_vpp);
	} while (error == EAGAIN);
	return error;
}

/* create, mkdir, symlink and mknod are one operation with different node types */
static int
ramfs_make(vnode_t dvp, vnode_t *vpp, struct componentname *cnp, struct vnode_attr *vap, enum vtype type, const char *target)
{
	struct ramfs_node *dir = RAMFS_NODE(dvp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_node *node;
	struct ramfs_dirent *de;
	int error;

	if (cnp->cn_namelen > RAMFS_MAX_NAME) {
		return ENAMETOOLONG;
	}
	node = ramfs_node_alloc(rm, type, vap);
	de = ramfs_dirent_alloc(cnp->cn_nameptr, (size_t)cnp->cn_namelen);
	if (type == VLNK) {
		node->rn_size = strlen(target);
		node->rn_target = kalloc_data(node->rn_size + 1, Z_WAITOK | Z_NOFAIL);
		memcpy(node->rn_target, target, node->rn_size + 1);
	}

	lck_mtx_lock(&rm->rm_lock);
	rm->rm_nodes++;
	if (dir->rn_nlink == 0) {
		error = ENOENT;         /* the directory has been removed */
	} else if (ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen) != NULL) {
		error = EEXIST;
	} else {
		error = 0;
	}
	if (error) {
		ramfs_node_free(node);
		lck_mtx_unlock(&rm->rm_lock);
		ramfs_dirent_free(de);
		return error;
	}
	node->rn_ino = rm->rm_next_ino++;
	if ((vap == NULL || !VATTR_IS_ACTIVE(vap, va_dataprotect_class) ||
	    vap->va_dataprotect_class == PROTECTION_CLASS_DEFAULT) && dir->rn_dataprotect_class != 0)
		node->rn_dataprotect_class = dir->rn_dataprotect_class;
	ramfs_hash_insert(rm, node);
	if (type == VDIR) {
		node->rn_nlink = 2;
		node->rn_parent = dir;
		dir->rn_nlink++;
	} else {
		node->rn_nlink = 1;
	}
	ramfs_dir_add(dir, de, node);

	/* a fresh node has no vnode and no other suitor: this cannot ask for a retry */
	error = ramfs_vget_locked(rm, node, dvp, cnp, vpp);
	if (error) {
		lck_mtx_lock(&rm->rm_lock);
		ramfs_dir_remove(dir, de);
		if (type == VDIR) {
			dir->rn_nlink--;
		}
		ramfs_node_free(node);
		lck_mtx_unlock(&rm->rm_lock);
	}
	return error;
}
static int
ramfs_create(struct vnop_create_args *ap)
{
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VREG, NULL);
}
static int
ramfs_mkdir(struct vnop_mkdir_args *ap)
{
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VDIR, NULL);
}
static int
ramfs_symlink(struct vnop_symlink_args *ap)
{
	if (strlen(ap->a_target) >= MAXPATHLEN) {
		return ENAMETOOLONG;
	}
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VLNK, ap->a_target);
}
/* Only the name of a local socket can be made here (bind); devices live in devfs. */
static int
ramfs_mknod(struct vnop_mknod_args *ap)
{
	if (ap->a_vap->va_type != VSOCK) {
		return ENOTSUP;
	}
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VSOCK, NULL);
}

static int
ramfs_remove(struct vnop_remove_args *ap)
{
	struct ramfs_node *dir = RAMFS_NODE(ap->a_dvp);
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_dirent *de;

	if (node->rn_type == VDIR) {
		return EPERM;
	}
	if ((ap->a_flags & VNODE_REMOVE_NODELETEBUSY) && vnode_isinuse(ap->a_vp, 0)) {
		return EBUSY;
	}
	lck_mtx_lock(&rm->rm_lock);
	de = ramfs_dir_find(dir, ap->a_cnp->cn_nameptr, (size_t)ap->a_cnp->cn_namelen);
	if (de == NULL || de->rd_node != node) {
		lck_mtx_unlock(&rm->rm_lock);
		return ENOENT;
	}
	ramfs_dir_remove(dir, de);
	node->rn_nlink--;
	ramfs_touch(node, false);
	lck_mtx_unlock(&rm->rm_lock);
	/* with no links left the node goes away when its vnode does: see ramfs_inactive() */
	return 0;
}

static int
ramfs_rmdir(struct vnop_rmdir_args *ap)
{
	struct ramfs_node *dir = RAMFS_NODE(ap->a_dvp);
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_dirent *de;
	int error = 0;

	if (node->rn_type != VDIR) {
		return ENOTDIR;
	}
	lck_mtx_lock(&rm->rm_lock);
	de = ramfs_dir_find(dir, ap->a_cnp->cn_nameptr, (size_t)ap->a_cnp->cn_namelen);
	if (de == NULL || de->rd_node != node) {
		error = ENOENT;
	} else if (node->rn_nentries != 0) {
		error = ENOTEMPTY;
	} else {
		ramfs_dir_remove(dir, de);
		dir->rn_nlink--;
		node->rn_nlink = 0;
		node->rn_parent = NULL;
	}
	lck_mtx_unlock(&rm->rm_lock);
	return error;
}

static int
ramfs_link(struct vnop_link_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_node *dir = RAMFS_NODE(ap->a_tdvp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct componentname *cnp = ap->a_cnp;
	struct ramfs_dirent *de;
	int error = 0;

	if (node->rn_type == VDIR) {
		return EPERM;
	}
	if (cnp->cn_namelen > RAMFS_MAX_NAME) {
		return ENAMETOOLONG;
	}
	de = ramfs_dirent_alloc(cnp->cn_nameptr, (size_t)cnp->cn_namelen);

	lck_mtx_lock(&rm->rm_lock);
	if (dir->rn_nlink == 0 || node->rn_nlink == 0) {
		error = ENOENT;
	} else if (ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen) != NULL) {
		error = EEXIST;
	} else {
		ramfs_dir_add(dir, de, node);
		node->rn_nlink++;
		ramfs_touch(node, false);
		de = NULL;
	}
	lck_mtx_unlock(&rm->rm_lock);
	if (de != NULL) {
		ramfs_dirent_free(de);
	}
	return error;
}

#include "ramfs_rename.inc"

static int
ramfs_open(__unused struct vnop_open_args *ap)
{
	return 0;
}

static int
ramfs_close(__unused struct vnop_close_args *ap)
{
	return 0;
}

static int
ramfs_getattr(struct vnop_getattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	uint64_t size, alloc;

	lck_mtx_lock(&rm->rm_lock);
	size = node->rn_type == VDIR ? (uint64_t)(node->rn_nentries + 2) * 32 : node->rn_size;
	alloc = node->rn_nalloc * PAGE_SIZE;

	VATTR_RETURN(vap, va_rdev, 0);
	VATTR_RETURN(vap, va_nlink, node->rn_nlink);
	VATTR_RETURN(vap, va_total_size, size);
	VATTR_RETURN(vap, va_total_alloc, alloc);
	VATTR_RETURN(vap, va_data_size, size);
	VATTR_RETURN(vap, va_data_alloc, alloc);
	VATTR_RETURN(vap, va_iosize, PAGE_SIZE);
	VATTR_RETURN(vap, va_uid, node->rn_uid);
	VATTR_RETURN(vap, va_gid, node->rn_gid);
	VATTR_RETURN(vap, va_mode, node->rn_mode);
	VATTR_RETURN(vap, va_flags, node->rn_bsdflags);
	VATTR_RETURN(vap, va_create_time, node->rn_btime);
	VATTR_RETURN(vap, va_addedtime, node->rn_btime);
	VATTR_RETURN(vap, va_access_time, node->rn_atime);
	VATTR_RETURN(vap, va_modify_time, node->rn_mtime);
	VATTR_RETURN(vap, va_change_time, node->rn_ctime);
	VATTR_RETURN(vap, va_fileid, node->rn_ino);
	VATTR_RETURN(vap, va_linkid, node->rn_ino);
	VATTR_RETURN(vap, va_fsid, vfs_statfs(vnode_mount(vp))->f_fsid.val[0]);
	VATTR_RETURN(vap, va_filerev, 0);
	VATTR_RETURN(vap, va_gen, 0);
	VATTR_RETURN(vap, va_dataprotect_class, node->rn_dataprotect_class ? node->rn_dataprotect_class : PROTECTION_CLASS_D);   /* "none", which PosterBoard checks */
	if (node->rn_type == VDIR && node->rn_parent != NULL) {
		VATTR_RETURN(vap, va_parentid, node->rn_parent->rn_ino);
		/*
		 * And its name, from its entry in the parent: what the kernel asks for to make a path out of a
		 * number alone (fsgetpath() of a vnode vfs_vget made, which has no name of its own).
		 */
		if (VATTR_IS_ACTIVE(vap, va_name) && vap->va_name != NULL && node != rm->rm_root) {
			struct ramfs_dirent *de;

			TAILQ_FOREACH(de, &node->rn_parent->rn_entries, rd_link) {
				if (de->rd_node == node) {
					strlcpy(vap->va_name, de->rd_name, MAXPATHLEN);
					VATTR_SET_SUPPORTED(vap, va_name);
					break;
				}
			}
		}
	}
	lck_mtx_unlock(&rm->rm_lock);
	return 0;
}

static int
ramfs_setattr(struct vnop_setattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	bool resized = false;
	uint64_t size = 0;

	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		if (node->rn_type == VDIR) {
			return EISDIR;
		}
		if (node->rn_type != VREG) {
			return EINVAL;
		}
		/* what a mapping changed must reach us before the size moves under it */
		int error = ramfs_sync_mapping(vp, node);
		if (error) {
			return error;
		}
	}

	lck_mtx_lock(&rm->rm_lock);
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		size = vap->va_data_size;
		ramfs_resize(node, size);
		resized = true;
		VATTR_SET_SUPPORTED(vap, va_data_size);
	}
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		node->rn_mode = vap->va_mode & ALLPERMS;
		VATTR_SET_SUPPORTED(vap, va_mode);
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		node->rn_uid = vap->va_uid;
		VATTR_SET_SUPPORTED(vap, va_uid);
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		node->rn_gid = vap->va_gid;
		VATTR_SET_SUPPORTED(vap, va_gid);
	}
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		node->rn_bsdflags = vap->va_flags;
		VATTR_SET_SUPPORTED(vap, va_flags);
	}
	if (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {
		if (vap->va_dataprotect_class != PROTECTION_CLASS_DEFAULT)
			node->rn_dataprotect_class = vap->va_dataprotect_class;
		VATTR_SET_SUPPORTED(vap, va_dataprotect_class);
	}
	if (VATTR_IS_ACTIVE(vap, va_create_time)) {
		node->rn_btime = vap->va_create_time;
		VATTR_SET_SUPPORTED(vap, va_create_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		node->rn_atime = vap->va_access_time;
		VATTR_SET_SUPPORTED(vap, va_access_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		node->rn_mtime = vap->va_modify_time;
		VATTR_SET_SUPPORTED(vap, va_modify_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_change_time)) {
		node->rn_ctime = vap->va_change_time;
		VATTR_SET_SUPPORTED(vap, va_change_time);
		ramfs_disk_dirty_locked(rm);
	} else {
		ramfs_touch(node, false);
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (resized) {
		(void)ubc_setsize(vp, (off_t)size);
	}
	return 0;
}

static int
ramfs_read(struct vnop_read_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	char *bounce;
	int error = 0;

	if (node->rn_type == VDIR) {
		return EISDIR;
	}
	if (node->rn_type != VREG || uio_offset(uio) < 0) {
		return EINVAL;
	}
	error = ramfs_sync_mapping(vp, node);
	if (error) {
		return error;
	}

	bounce = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);
	while (uio_resid(uio) > 0 && !error) {
		uint64_t off = (uint64_t)uio_offset(uio);
		size_t len;

		lck_mtx_lock(&rm->rm_lock);
		if (off >= node->rn_size) {
			lck_mtx_unlock(&rm->rm_lock);
			break;
		}
		len = MIN((size_t)MIN((uint64_t)uio_resid(uio), node->rn_size - off), PAGE_SIZE - (size_t)(off & PAGE_MASK));
		ramfs_fetch(node, off, bounce, len);
		lck_mtx_unlock(&rm->rm_lock);

		error = uiomove(bounce, (int)len, uio);
	}
	kfree_data(bounce, PAGE_SIZE);
	return error;
}

static int
ramfs_write(struct vnop_write_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	void *spare = NULL;
	char *bounce;
	uint64_t start, old_size;
	int error = 0;

	if (node->rn_type == VDIR) {
		return EISDIR;
	}
	if (node->rn_type != VREG || uio_offset(uio) < 0) {
		return EINVAL;
	}
	if (uio_resid(uio) == 0) {
		return 0;
	}
	error = ramfs_sync_mapping(vp, node);
	if (error) {
		return error;
	}

	lck_mtx_lock(&rm->rm_lock);
	old_size = node->rn_size;
	if (ap->a_ioflag & IO_APPEND) {
		uio_setoffset(uio, (off_t)old_size);
	}
	lck_mtx_unlock(&rm->rm_lock);
	start = (uint64_t)uio_offset(uio);

	bounce = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);
	while (uio_resid(uio) > 0) {
		uint64_t off = (uint64_t)uio_offset(uio);
		size_t len = MIN((size_t)uio_resid(uio), PAGE_SIZE - (size_t)(off & PAGE_MASK));

		error = uiomove(bounce, (int)len, uio);
		if (error) {
			break;
		}
		if (spare == NULL) {
			spare = ramfs_page_alloc(true);
		}
		uint32_t write_dblk = 0;
		void *write_page = NULL;

		ramfs_reserve_slots(rm, node, off / PAGE_SIZE);
		lck_mtx_lock(&rm->rm_lock);
		error = ramfs_store(node, off, bounce, len, &spare, &write_dblk, &write_page);
		lck_mtx_unlock(&rm->rm_lock);
		if (error) {
			/* nothing of this piece was stored: hand it back to the caller's count */
			uio_setoffset(uio, (off_t)off);
			uio_setresid(uio, uio_resid(uio) + (user_ssize_t)len);
			break;
		}
		error = ramfs_store_write(rm, node, off / PAGE_SIZE, write_dblk, write_page);
		if (error) {
			break;
		}
	}
	kfree_data(bounce, PAGE_SIZE);
	if (spare != NULL) {
		ramfs_page_free(spare);
	}

	if ((uint64_t)uio_offset(uio) > start) {
		uint64_t new_size;

		lck_mtx_lock(&rm->rm_lock);
		ramfs_touch(node, true);
		new_size = node->rn_size;
		lck_mtx_unlock(&rm->rm_lock);

		if (new_size != old_size) {
			(void)ubc_setsize(vp, (off_t)new_size);
		}
		/* cached pages of the range are stale now: brought up to date in place */
		if (ubc_pages_resident(vp)) {
			ramfs_refresh_cache(vp, node, start, (uint64_t)uio_offset(uio));
		}
		/* a partial write is a success */
		if (error == ENOSPC) {
			error = 0;
		}
	}
	return error;
}

/* Classic 32-bit-inode directory entries; the offset is a byte count of the entries before it. */
static int
ramfs_readdir(struct vnop_readdir_args *ap)
{
#define RAMFS_DIRSIZ(namlen)    ((sizeof(struct dirent) - (MAXNAMLEN + 1)) + (((namlen) + 1 + 3) & ~3))
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ramfs_node *dir = RAMFS_NODE(vp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_dirent *de;
	off_t start = uio_offset(uio), pos = 0;
	size_t room = (size_t)MIN((user_ssize_t)PAGE_SIZE, uio_resid(uio));
	size_t used = 0;
	uint32_t entry = 0;
	bool eof = false;
	int count = 0, error;
	char *buf;

	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) {
		return EINVAL;
	}
	if (dir->rn_type != VDIR) {
		return ENOTDIR;
	}
	/* the entries are collected under the lock and copied out after it is dropped */
	buf = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);

	lck_mtx_lock(&rm->rm_lock);
	de = TAILQ_FIRST(&dir->rn_entries);
	for (;; entry++) {
		struct dirent d;
		const char *name;

		bzero(&d, sizeof(d));
		if (entry == 0) {
			d.d_fileno = (ino_t)dir->rn_ino;
			d.d_type = DT_DIR;
			d.d_namlen = 1;
			name = ".";
		} else if (entry == 1) {
			d.d_fileno = (ino_t)(dir->rn_parent ? dir->rn_parent->rn_ino : dir->rn_ino);
			d.d_type = DT_DIR;
			d.d_namlen = 2;
			name = "..";
		} else {
			if (de == NULL) {
				eof = true;
				break;
			}
			d.d_fileno = (ino_t)de->rd_node->rn_ino;
			d.d_type = (uint8_t)IFTODT(VTTOIF(de->rd_node->rn_type));
			d.d_namlen = (uint8_t)de->rd_namelen;
			name = de->rd_name;
			de = TAILQ_NEXT(de, rd_link);
		}
		d.d_reclen = (uint16_t)RAMFS_DIRSIZ(d.d_namlen);
		if (pos >= start) {
			if (used + d.d_reclen > room) {
				break;
			}
			memcpy(d.d_name, name, d.d_namlen);
			memcpy(buf + used, &d, d.d_reclen);
			used += d.d_reclen;
			count++;
		}
		pos += d.d_reclen;
	}
	lck_mtx_unlock(&rm->rm_lock);

	error = used ? uiomove(buf, (int)used, uio) : 0;
	kfree_data(buf, PAGE_SIZE);
	if (!error) {
		uio_setoffset(uio, pos);
		if (ap->a_eofflag) {
			*ap->a_eofflag = eof;
		}
		if (ap->a_numdirent) {
			*ap->a_numdirent = count;
		}
	}
	return error;
#undef RAMFS_DIRSIZ
}

static int
ramfs_readlink(struct vnop_readlink_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);

	if (node->rn_type != VLNK) {
		return EINVAL;
	}
	/* a symlink never changes after it is made */
	return uiomove(node->rn_target, (int)node->rn_size, ap->a_uio);
}

#include "ramfs_fsctl.inc"

/* Last reference gone: a node without a name has no reason to linger in the vnode cache. */
static int
ramfs_inactive(struct vnop_inactive_args *ap)
{
	if (RAMFS_NODE(ap->a_vp)->rn_nlink == 0) {
		vnode_recycle(ap->a_vp);
	}
	return 0;
}

static int
ramfs_reclaim(struct vnop_reclaim_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;

	lck_mtx_lock(&rm->rm_lock);
	node->rn_vp = NULLVP;
	node->rn_vid = 0;
	if (node->rn_nlink == 0 && node != rm->rm_root) {
		ramfs_node_free(node);
	}
	lck_mtx_unlock(&rm->rm_lock);

	vnode_clearfsnode(vp);
	return 0;
}

static int
ramfs_pathconf(struct vnop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 32767;
		return 0;
	case _PC_NAME_MAX:
		*ap->a_retval = RAMFS_MAX_NAME;
		return 0;
	case _PC_PATH_MAX:
		*ap->a_retval = MAXPATHLEN;
		return 0;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		return 0;
	case _PC_CHOWN_RESTRICTED:
	case _PC_NO_TRUNC:
		*ap->a_retval = 200112;
		return 0;
	case _PC_CASE_SENSITIVE:
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		return 0;
	case _PC_FILESIZEBITS:
		*ap->a_retval = 64;
		return 0;
	default:
		return EINVAL;
	}
}

static int
ramfs_mmap(struct vnop_mmap_args *ap)
{
	if (ap->a_fflags & VM_PROT_WRITE) {
		RAMFS_NODE(ap->a_vp)->rn_mapped_write = true;
	}
	return 0;
}

static int
ramfs_mnomap(__unused struct vnop_mnomap_args *ap)
{
	return 0;
}

/* Fill pages of the page cache from the file. */
static int
ramfs_pagein(struct vnop_pagein_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	upl_t upl = ap->a_pl;
	upl_size_t size = (upl_size_t)ap->a_size;
	uint64_t off = (uint64_t)ap->a_f_offset;
	bool commit = (ap->a_flags & UPL_NOCOMMIT) == 0;
	upl_size_t valid = 0;
	vm_offset_t addr;

	if (upl == NULL || node->rn_type != VREG || ap->a_f_offset < 0 || (off & PAGE_MASK) != 0 ||
	    ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
		if (commit && upl != NULL) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
		return EINVAL;
	}
	addr += ap->a_pl_offset;

	lck_mtx_lock(&rm->rm_lock);
	for (upl_size_t done = 0; done < size && off + done < node->rn_size; done += PAGE_SIZE) {
		size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, node->rn_size - (off + done));

		ramfs_fetch(node, off + done, (void *)(addr + done), len);
		bzero((void *)(addr + done + len), PAGE_SIZE - len);
		valid = done + PAGE_SIZE;
	}
	lck_mtx_unlock(&rm->rm_lock);
	ubc_upl_unmap(upl);

	if (commit) {
		if (valid != 0) {
			ubc_upl_commit_range(upl, ap->a_pl_offset, valid, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
		/* pages wholly past the end of the file do not exist */
		if (valid < size) {
			ubc_upl_abort_range(upl, ap->a_pl_offset + valid, size - valid, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
	}
	return valid != 0 ? 0 : EINVAL;
}

/* Take back what a mapping changed: copy dirty pages of the page cache into the file. */
static int
ramfs_pageout(struct vnop_pageout_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	upl_t upl = ap->a_pl;
	upl_size_t size = (upl_size_t)ap->a_size;
	uint64_t off = (uint64_t)ap->a_f_offset;
	bool commit = (ap->a_flags & UPL_NOCOMMIT) == 0;
	vm_offset_t addr;
	int error = 0;

	if (upl == NULL) {
		return EINVAL;
	}
	if (node->rn_type != VREG || ap->a_f_offset < 0 || (off & PAGE_MASK) != 0 ||
	    ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
		if (commit) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		}
		return EINVAL;
	}
	addr += ap->a_pl_offset;

	for (upl_size_t done = 0; done < size && off + done < node->rn_size && !error; done += PAGE_SIZE) {
		size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, node->rn_size - (off + done));
		uint64_t index = (off + done) / PAGE_SIZE;
		uint32_t write_dblk = 0;
		void *write_page = NULL;
		/* this may be the pageout daemon looking for memory: never wait for more of it here */
		void *spare = index < node->rn_nslots && node->rn_pages[index] != NULL ? NULL : ramfs_page_alloc(false);

		lck_mtx_lock(&rm->rm_lock);
		error = ramfs_store(node, off + done, (const void *)(addr + done), len, &spare, &write_dblk, &write_page);
		lck_mtx_unlock(&rm->rm_lock);
		if (spare != NULL) {
			ramfs_page_free(spare);
		}
		if (!error) {
			error = ramfs_store_write(rm, node, index, write_dblk, write_page);
		}
	}
	if (!error) {
		lck_mtx_lock(&rm->rm_lock);
		ramfs_touch(node, true);
		lck_mtx_unlock(&rm->rm_lock);
	}
	ubc_upl_unmap(upl);

	if (commit) {
		if (error) {
			/* the pages stay dirty and will be offered again */
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		} else {
			ubc_upl_commit_range(upl, ap->a_pl_offset, size, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
	}
	return error;
}

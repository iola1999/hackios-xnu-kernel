/*
 * APFS VFS Operations: root, vget, getattr, sync, init, and vfsops table
 */
#pragma once

static int
apfs_root(mount_t mp, vnode_t *vpp, __unused vfs_context_t ctx)
{
	return apfs_node_vnode(APFS_MP(mp), APFS_MP(mp)->am_root_id, NULLVP, NULL, vpp);
}

static int
apfs_vget(mount_t mp, ino64_t ino, vnode_t *vpp, __unused vfs_context_t ctx)
{
	return apfs_node_vnode(APFS_MP(mp), (uint64_t)ino, NULLVP, NULL, vpp);
}

static int
apfs_vfs_getattr(mount_t mp, struct vfs_attr *fsap, __unused vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);
	uint64_t files = am->am_vol.num_files + am->am_vol.num_dirs;
	uint64_t free_blks = annibale_container_free_bytes() / am->am_nx.block_size;

	VFSATTR_RETURN(fsap, f_objcount, files);
	VFSATTR_RETURN(fsap, f_bsize, am->am_nx.block_size);
	VFSATTR_RETURN(fsap, f_iosize, am->am_nx.block_size);
	VFSATTR_RETURN(fsap, f_blocks, annibale_container_total_bytes() / am->am_nx.block_size);
	VFSATTR_RETURN(fsap, f_bfree, free_blks);
	VFSATTR_RETURN(fsap, f_bavail, free_blks);
	VFSATTR_RETURN(fsap, f_bused, am->am_vol.alloc_count);
	VFSATTR_RETURN(fsap, f_files, files);
	VFSATTR_RETURN(fsap, f_ffree, 0);

	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
		vol_capabilities_attr_t *cap = &fsap->f_capabilities;
		bool nocase = (am->am_vol.incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;

		bzero(cap, sizeof(*cap));
		/*
		 * An object's identifier never changes and apfs_vget() finds an object by it,
		 * which is what lets fsgetpath(2) and /.vol work for this volume: without it,
		 * what asks for a file's path by its identifier (LaunchServices among them) gets
		 * nothing.
		 */
		cap->capabilities[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_PATH_FROM_ID |
		    (nocase ? 0 : VOL_CAP_FMT_CASE_SENSITIVE);
		cap->capabilities[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR;
		cap->valid[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_JOURNAL |
		    VOL_CAP_FMT_JOURNAL_ACTIVE |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_ZERO_RUNS |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_HIDDEN_FILES |
		    VOL_CAP_FMT_PATH_FROM_ID;
		cap->valid[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR;
		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
		strlcpy(fsap->f_vol_name, am->am_vol.name, MAXPATHLEN);
		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_uuid)) {
		memcpy(fsap->f_uuid, am->am_nx.uuid, sizeof(fsap->f_uuid));
		VFSATTR_SET_SUPPORTED(fsap, f_uuid);
	}
	return 0;
}

/*
 * VFS sync: flush cached blocks, checkpoints, and issue DKIOCSYNCHRONIZECACHE
 * to the underlying block storage device.
 */
static int
apfs_sync(mount_t mp, __unused int waitfor, vfs_context_t ctx)
{
	struct apfs_mount *am;
	int error = 0;

	if (mp == NULL) {
		return EINVAL;
	}
	am = APFS_MP(mp);
	if (am == NULL) {
		return 0;
	}
	if (vfs_flags(mp) & MNT_RDONLY) {
		return 0;
	}

	apfs_lock_exclusive(am);

	if (am->am_devvp != NULLVP) {
		vfs_context_t ioctl_ctx = (ctx != NULL) ? ctx : vfs_context_kernel();
		int ioctl_err = VNOP_IOCTL(am->am_devvp, DKIOCSYNCHRONIZECACHE, (caddr_t)NULL, 0, ioctl_ctx);
		if (ioctl_err != 0 && ioctl_err != EROFS && ioctl_err != ENOTTY && ioctl_err != ENODEV) {
			error = ioctl_err;
		}
	}

	apfs_unlock_exclusive(am);

	return error;
}

static int
apfs_init(__unused struct vfsconf *vfsc)
{
	return 0;
}

struct vfsops apfs_vfsops = {
	.vfs_mount = apfs_mount,
	.vfs_unmount = apfs_unmount,
	.vfs_root = apfs_root,
	.vfs_getattr = apfs_vfs_getattr,
	.vfs_sync = apfs_sync,
	.vfs_vget = apfs_vget,
	.vfs_init = apfs_init,
};

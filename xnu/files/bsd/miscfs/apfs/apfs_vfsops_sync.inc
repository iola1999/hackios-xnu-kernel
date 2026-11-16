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

extern uint64_t ufs_darwin_bytes;      /* bsd/dev/annibale/annibale_ufs.c */

/*
 * The space of the one container iOS expects: on an iPhone the system and data volumes share
 * it, so every volume reports the same total and free space.  Here they are two partitions,
 * darwin (the system volume) and darwin_var (the data volume's own container); free space is
 * the data container's.  Before the data volume is APFS the figures come from ANNFS
 * (ramfs_getattr.inc).
 */
static void
apfs_container_space(uint64_t *total_bytes, uint64_t *avail_bytes)
{
	struct apfs_mount *data;

	lck_mtx_lock(&apfs_pool_lock);
	data = apfs_data_mount;
	if (data != NULL) {
		uint64_t bs = data->am_nx.block_size;

		*total_bytes = ufs_darwin_bytes + data->am_nx.block_count * bs;
		*avail_bytes = apfs_wtx_free_blocks(apfs_wtx_of(data)) * bs;
	}
	lck_mtx_unlock(&apfs_pool_lock);
	if (data == NULL) {
		*total_bytes = annibale_container_total_bytes();
		*avail_bytes = annibale_container_free_bytes();
	}
}

static void
apfs_fill_statfs(struct apfs_mount *am, struct vfsstatfs *sp)
{
	uint64_t space, avail, bs = am->am_nx.block_size;

	apfs_container_space(&space, &avail);
	sp->f_bsize = sp->f_iosize = (uint32_t)bs;
	sp->f_blocks = space / bs;
	sp->f_bfree = sp->f_bavail = avail / bs;
	sp->f_bused = sp->f_blocks - MIN(sp->f_blocks, sp->f_bfree);
	sp->f_files = am->am_vol.num_files + am->am_vol.num_dirs;
	sp->f_ffree = 0;
}

static int
apfs_vfs_getattr(mount_t mp, struct vfs_attr *fsap, __unused vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);
	uint64_t files = am->am_vol.num_files + am->am_vol.num_dirs;
	uint64_t space, avail, bs = am->am_nx.block_size;
	bool rw = am->am_w != NULL;

	apfs_container_space(&space, &avail);
	VFSATTR_RETURN(fsap, f_objcount, files);
	VFSATTR_RETURN(fsap, f_bsize, (uint32_t)bs);
	VFSATTR_RETURN(fsap, f_iosize, (size_t)bs);
	VFSATTR_RETURN(fsap, f_blocks, space / bs);
	VFSATTR_RETURN(fsap, f_bfree, avail / bs);
	VFSATTR_RETURN(fsap, f_bavail, avail / bs);
	VFSATTR_RETURN(fsap, f_bused, (space - MIN(space, avail)) / bs);
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
		    (rw ? VOL_CAP_FMT_JOURNAL | VOL_CAP_FMT_JOURNAL_ACTIVE | VOL_CAP_FMT_DOCUMENT_ID |
		    VOL_CAP_FMT_CLONE_MAPPING : 0) |
		    (nocase ? 0 : VOL_CAP_FMT_CASE_SENSITIVE);
		cap->capabilities[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_SECURITY |
		    VOL_CAP_INT_EXTENDED_ATTR |
		    (rw ? VOL_CAP_INT_CLONE | VOL_CAP_INT_RENAME_SWAP | VOL_CAP_INT_RENAME_EXCL : 0);
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
		    VOL_CAP_FMT_PATH_FROM_ID |
		    VOL_CAP_FMT_DOCUMENT_ID |
		    VOL_CAP_FMT_CLONE_MAPPING;
		cap->valid[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_SECURITY |
		    VOL_CAP_INT_EXTENDED_ATTR |
		    VOL_CAP_INT_CLONE |
		    VOL_CAP_INT_RENAME_SWAP |
		    VOL_CAP_INT_RENAME_EXCL;
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

/* sync(2) and the like: every file's dirty pages, then a commit, which ends with a flush. */
static int
apfs_sync(mount_t mp, __unused int waitfor, __unused vfs_context_t ctx)
{
	struct apfs_mount *am = mp != NULL ? APFS_MP(mp) : NULL;

	if (am == NULL || am->am_w == NULL) {
		return 0;
	}
	vnode_iterate(mp, 0, apfs_wpush_vnode, NULL);
	return apfs_wcommit(am, false, 0, APFS_WHY_SYNC);
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

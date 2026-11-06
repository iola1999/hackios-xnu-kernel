/*
 * Volume attributes and Apple 128 GB APFS Container capacity calculation
 * shared between the Data volume (/private/var) and the System volume (/).
 */

#define REDWOOD_CONTAINER_BYTES   128000000000ULL
#define REDWOOD_SYSTEM_USED_BYTES 15571353600ULL

uint64_t redwood_container_total_bytes(void);
uint64_t redwood_container_free_bytes(void);

uint64_t
redwood_container_total_bytes(void)
{
	return REDWOOD_CONTAINER_BYTES;
}

uint64_t
redwood_container_free_bytes(void)
{
	uint64_t data_bytes = os_atomic_load(&ramfs_var_used_pages, relaxed) * (uint64_t)PAGE_SIZE;
	uint64_t used_bytes = REDWOOD_SYSTEM_USED_BYTES + data_bytes;

	return used_bytes < REDWOOD_CONTAINER_BYTES ? (REDWOOD_CONTAINER_BYTES - used_bytes) : 0;
}

static void
ramfs_fill_statfs(struct ramfs_mount *rm, struct vfsstatfs *sp)
{
	uint64_t total_pages = redwood_container_total_bytes() / PAGE_SIZE;
	uint64_t free_pages = redwood_container_free_bytes() / PAGE_SIZE;

	sp->f_bsize = PAGE_SIZE;
	sp->f_iosize = PAGE_SIZE;
	sp->f_blocks = total_pages;
	sp->f_bfree = free_pages;
	sp->f_bavail = free_pages;
	sp->f_bused = total_pages - free_pages;
	sp->f_files = rm->rm_max_pages;
	sp->f_ffree = rm->rm_max_pages - MIN(rm->rm_nodes, rm->rm_max_pages);
	strlcpy(sp->f_fstypename, "apfs", sizeof(sp->f_fstypename));
	strlcpy(sp->f_mntfromname, "/dev/disk1s2", sizeof(sp->f_mntfromname));
}

static int
ramfs_vfs_getattr(mount_t mp, struct vfs_attr *fsap, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm = RAMFS_MP(mp);
	uint64_t nodes, total_pages, free_pages, used_pages;

	lck_mtx_lock(&rm->rm_lock);
	nodes = rm->rm_nodes;
	lck_mtx_unlock(&rm->rm_lock);

	total_pages = redwood_container_total_bytes() / PAGE_SIZE;
	free_pages = redwood_container_free_bytes() / PAGE_SIZE;
	used_pages = total_pages - free_pages;

	VFSATTR_RETURN(fsap, f_objcount, nodes);
	VFSATTR_RETURN(fsap, f_bsize, PAGE_SIZE);
	VFSATTR_RETURN(fsap, f_iosize, PAGE_SIZE);
	VFSATTR_RETURN(fsap, f_blocks, total_pages);
	VFSATTR_RETURN(fsap, f_bfree, free_pages);
	VFSATTR_RETURN(fsap, f_bavail, free_pages);
	VFSATTR_RETURN(fsap, f_bused, used_pages);
	VFSATTR_RETURN(fsap, f_files, rm->rm_max_pages);
	VFSATTR_RETURN(fsap, f_ffree, rm->rm_max_pages - MIN(nodes, rm->rm_max_pages));

	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
		vol_capabilities_attr_t *cap = &fsap->f_capabilities;

		bzero(cap, sizeof(*cap));
		cap->capabilities[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_PATH_FROM_ID |
		    VOL_CAP_FMT_CLONE_MAPPING |
		    (rm->rm_disk_backed ? (VOL_CAP_FMT_JOURNAL | VOL_CAP_FMT_JOURNAL_ACTIVE) : 0);
		cap->capabilities[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_SECURITY |
		    VOL_CAP_INT_EXTENDED_ATTR |
		    VOL_CAP_INT_CLONE;
		cap->valid[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_JOURNAL |
		    VOL_CAP_FMT_JOURNAL_ACTIVE |
		    VOL_CAP_FMT_NO_ROOT_TIMES |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_ZERO_RUNS |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_OPENDENYMODES |
		    VOL_CAP_FMT_HIDDEN_FILES |
		    VOL_CAP_FMT_PATH_FROM_ID |
		    VOL_CAP_FMT_CLONE_MAPPING;
		cap->valid[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_SEARCHFS |
		    VOL_CAP_INT_ATTRLIST |
		    VOL_CAP_INT_NFSEXPORT |
		    VOL_CAP_INT_READDIRATTR |
		    VOL_CAP_INT_EXCHANGEDATA |
		    VOL_CAP_INT_COPYFILE |
		    VOL_CAP_INT_ALLOCATE |
		    VOL_CAP_INT_VOL_RENAME |
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_SECURITY |
		    VOL_CAP_INT_USERACCESS |
		    VOL_CAP_INT_MANLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR |
		    VOL_CAP_INT_NAMEDSTREAMS |
		    VOL_CAP_INT_CLONE;
		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
	}
	VFSATTR_RETURN(fsap, f_filecount, nodes);
	VFSATTR_RETURN(fsap, f_dircount, 0);
	VFSATTR_RETURN(fsap, f_maxobjcount, rm->rm_max_pages);
	VFSATTR_RETURN(fsap, f_owner, 0);
	VFSATTR_RETURN(fsap, f_fssubtype, 0);
	VFSATTR_RETURN(fsap, f_signature, 0x4244);
	VFSATTR_RETURN(fsap, f_carbon_fsid, 0);
	VFSATTR_RETURN(fsap, f_quota, 0);
	VFSATTR_RETURN(fsap, f_reserved, 0);
	if (VFSATTR_IS_ACTIVE(fsap, f_fsid)) {
		fsap->f_fsid = vfs_statfs(mp)->f_fsid;
		VFSATTR_SET_SUPPORTED(fsap, f_fsid);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_create_time) || VFSATTR_IS_ACTIVE(fsap, f_modify_time) ||
	    VFSATTR_IS_ACTIVE(fsap, f_access_time) || VFSATTR_IS_ACTIVE(fsap, f_backup_time)) {
		struct timeval boot;
		struct timespec since = { .tv_sec = 0, .tv_nsec = 0 };

		boottime_timeval(&boot);
		since.tv_sec = boot.tv_sec;
		VFSATTR_RETURN(fsap, f_create_time, since);
		VFSATTR_RETURN(fsap, f_modify_time, since);
		VFSATTR_RETURN(fsap, f_access_time, since);
		since.tv_sec = 0;
		VFSATTR_RETURN(fsap, f_backup_time, since);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_uuid)) {
		static const uuid_t base = { 'r', 'e', 'd', 'w', 'o', 'o', 0x50, 'd', 0x80, 'R', 0, 0, 0, 0, 0, 0 };
		fsid_t fsid = vfs_statfs(mp)->f_fsid;

		memcpy(fsap->f_uuid, base, sizeof(uuid_t));
		memcpy(&fsap->f_uuid[10], &fsid.val[0], 4);
		fsap->f_uuid[14] = (uint8_t)fsid.val[1];
		VFSATTR_SET_SUPPORTED(fsap, f_uuid);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_attributes)) {
		vol_attributes_attr_t *attr = &fsap->f_attributes;

		attr->validattr.commonattr = ATTR_CMN_VALIDMASK;
		attr->validattr.volattr = ATTR_VOL_VALIDMASK;
		attr->validattr.dirattr = ATTR_DIR_VALIDMASK;
		attr->validattr.fileattr = ATTR_FILE_VALIDMASK;
		attr->validattr.forkattr = 0;
		attr->nativeattr = attr->validattr;
		VFSATTR_SET_SUPPORTED(fsap, f_attributes);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
		strlcpy(fsap->f_vol_name, "Data", MAXPATHLEN);
		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
	}
	return 0;
}

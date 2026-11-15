/*
 * APFS VFS Write Operations: vnop_write, vnop_pageout, vnop_fsync, vnop_create, vnop_remove, vnop_setattr
 */
#pragma once

#include <sys/disk.h>
#include "apfs_write.h"
#include "apfs_vnops_dir.inc"

static int
apfs_vnop_setxattr(struct vnop_setxattr_args *ap)
{
	if (ap == NULL || ap->a_vp == NULLVP || ap->a_name == NULL) {
		return EINVAL;
	}
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	if (an == NULL || an->an_mount == NULL) {
		return EINVAL;
	}
	struct apfs_mount *am = an->an_mount;
	int error = 0;

	if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY) {
		return EROFS;
	}

	size_t size = ap->a_uio != NULL ? (size_t)uio_resid(ap->a_uio) : 0;
	if (size > 3804) {
		return E2BIG;
	}
	void *data = NULL;
	if (size > 0) {
		data = kalloc_data(size, Z_WAITOK);
		if (data == NULL) {
			return ENOMEM;
		}
		error = uiomove(data, (int)size, ap->a_uio);
		if (error) {
			kfree_data(data, size);
			return error;
		}
	}

	apfs_lock_exclusive(am);
	error = apfs_set_xattr(&am->am_vol, an->an_id, ap->a_name, data, size);
	apfs_unlock_exclusive(am);

	if (data != NULL) {
		kfree_data(data, size);
	}
	return error;
}

static int
apfs_vnop_removexattr(struct vnop_removexattr_args *ap)
{
	if (ap == NULL || ap->a_vp == NULLVP || ap->a_name == NULL) {
		return EINVAL;
	}
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	if (an == NULL || an->an_mount == NULL) {
		return EINVAL;
	}
	struct apfs_mount *am = an->an_mount;
	int error = 0;

	if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY) {
		return EROFS;
	}

	apfs_lock_exclusive(am);
	error = apfs_remove_xattr(&am->am_vol, an->an_id, ap->a_name);
	apfs_unlock_exclusive(am);

	return error;
}

static int
apfs_vnop_write(struct vnop_write_args *ap)
{
	if (ap == NULL || ap->a_vp == NULLVP || ap->a_uio == NULL) {
		return EINVAL;
	}
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct apfs_node *an = APFS_VP(vp);
	if (an == NULL || an->an_mount == NULL) {
		return EINVAL;
	}
	struct apfs_mount *am = an->an_mount;
	int error = 0;

	if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY) {
		return EROFS;
	}
	if (!vnode_isreg(vp) || uio_offset(uio) < 0) {
		return EINVAL;
	}
	if (uio_resid(uio) == 0) {
		return 0;
	}

	lck_rw_lock_exclusive(&an->an_rwlock);
	if (ap->a_ioflag & IO_APPEND) {
		uio_setoffset(uio, (off_t)an->an_size);
	}
	off_t start = uio_offset(uio);
	off_t old_size = (off_t)an->an_size;

	/*
	 * Without vnop_blockmap, cluster_write returns ENOTSUP.
	 * Directly perform synchronous transactional extent writes and invalidate UBC pages.
	 */
	apfs_lock_exclusive(am);
	char *bounce = kalloc_data(PAGE_SIZE, Z_WAITOK);
	if (bounce == NULL) {
		apfs_unlock_exclusive(am);
		lck_rw_unlock_exclusive(&an->an_rwlock);
		return ENOMEM;
	}

	while (uio_resid(uio) > 0) {
		uint64_t off = (uint64_t)uio_offset(uio);
		size_t len = MIN((size_t)uio_resid(uio), PAGE_SIZE - (size_t)(off & PAGE_MASK));

		error = uiomove(bounce, (int)len, uio);
		if (error) {
			break;
		}

		struct apfs_write_report rep;
		memset(&rep, 0, sizeof(rep));
		error = apfs_write_file(&am->am_vol, an->an_id, off, bounce, len, &rep);
		if (error) {
			uio_setoffset(uio, (off_t)off);
			uio_setresid(uio, uio_resid(uio) + (user_ssize_t)len);
			break;
		}

		if (off + len > an->an_size) {
			an->an_size = off + len;
			an->an_ino.size = an->an_size;
		}
	}
	kfree_data(bounce, PAGE_SIZE);

	uint64_t end_off = (uint64_t)uio_offset(uio);
	uint64_t final_size = an->an_size;
	bool size_changed = (final_size != (uint64_t)old_size);
	apfs_unlock_exclusive(am);

	if (end_off > (uint64_t)start) {
		if (size_changed) {
			(void)ubc_setsize(vp, (off_t)final_size);
		}
		off_t inv_start = (off_t)(start & ~(uint64_t)PAGE_MASK);
		off_t inv_end = (off_t)((end_off + (uint64_t)PAGE_MASK) & ~(uint64_t)PAGE_MASK);
		(void)ubc_msync(vp, inv_start, inv_end, NULL, UBC_INVALIDATE);
	}
	lck_rw_unlock_exclusive(&an->an_rwlock);
	return error;
}

static int
apfs_vnop_pageout(struct vnop_pageout_args *ap)
{
	if (ap == NULL || ap->a_vp == NULLVP) {
		return EINVAL;
	}
	if (ap->a_pl != NULL && (ap->a_flags & UPL_NOCOMMIT) == 0) {
		ubc_upl_abort_range(ap->a_pl, ap->a_pl_offset, (upl_size_t)ap->a_size, UPL_ABORT_FREE_ON_EMPTY);
	}
	return ENOTSUP;
}

static int
apfs_vnop_fsync(struct vnop_fsync_args *ap)
{
	if (ap == NULL || ap->a_vp == NULLVP) {
		return EINVAL;
	}
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	if (an == NULL || an->an_mount == NULL) {
		return EINVAL;
	}
	struct apfs_mount *am = an->an_mount;
	if (vnode_isreg(vp)) {
		(void)cluster_push(vp, 0);
	}
	if (ap->a_waitfor == MNT_WAIT) {
		apfs_lock_exclusive(am);
		if (am->am_devvp != NULLVP) {
			vfs_context_t ioctl_ctx = (ap->a_context != NULL) ? ap->a_context : vfs_context_kernel();
			(void)VNOP_IOCTL(am->am_devvp, DKIOCSYNCHRONIZECACHE, (caddr_t)NULL, 0, ioctl_ctx);
		}
		apfs_unlock_exclusive(am);
	}
	return 0;
}

static int
apfs_vnop_create(struct vnop_create_args *ap)
{
	if (ap == NULL || ap->a_dvp == NULLVP || ap->a_vpp == NULL ||
	    ap->a_cnp == NULL || ap->a_vap == NULL) {
		return EINVAL;
	}
	vnode_t dvp = ap->a_dvp;
	vnode_t *vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_node *dn = APFS_VP(dvp);
	if (dn == NULL || dn->an_mount == NULL) {
		return EINVAL;
	}
	struct apfs_mount *am = dn->an_mount;
	int error;
	uint64_t new_id = 0;

	*vpp = NULLVP;
	if (vfs_flags(vnode_mount(dvp)) & MNT_RDONLY) {
		return EROFS;
	}
	if (cnp->cn_nameptr == NULL) {
		return EINVAL;
	}
	if (cnp->cn_namelen == 0 || cnp->cn_namelen > 255) {
		return ENAMETOOLONG;
	}

	uint16_t mode = (uint16_t)(vap->va_mode & 07777);
	uint32_t uid = VATTR_IS_ACTIVE(vap, va_uid) ? vap->va_uid : 0;
	uint32_t gid = VATTR_IS_ACTIVE(vap, va_gid) ? vap->va_gid : 0;

	char namebuf[256];
	memcpy(namebuf, cnp->cn_nameptr, cnp->cn_namelen);
	namebuf[cnp->cn_namelen] = '\0';

	apfs_lock_exclusive(am);
	error = apfs_create_file(&am->am_vol, dn->an_id, namebuf, mode, uid, gid, &new_id);
	apfs_unlock_exclusive(am);

	if (error == 0) {
		error = apfs_node_vnode(am, new_id, dvp, cnp, vpp);
		if (error == 0 && (cnp->cn_flags & MAKEENTRY)) {
			cache_enter(dvp, *vpp, cnp);
		}
	}
	return error;
}

static int
apfs_vnop_remove(struct vnop_remove_args *ap)
{
	if (ap == NULL || ap->a_dvp == NULLVP || ap->a_vp == NULLVP || ap->a_cnp == NULL) {
		return EINVAL;
	}
	vnode_t dvp = ap->a_dvp;
	vnode_t vp = ap->a_vp;
	struct componentname *cnp = ap->a_cnp;
	struct apfs_node *dn = APFS_VP(dvp);
	if (dn == NULL || dn->an_mount == NULL) {
		return EINVAL;
	}
	struct apfs_mount *am = dn->an_mount;
	int error;

	if (vfs_flags(vnode_mount(dvp)) & MNT_RDONLY) {
		return EROFS;
	}
	if (cnp->cn_nameptr == NULL) {
		return EINVAL;
	}
	if (cnp->cn_namelen == 0 || cnp->cn_namelen > 255) {
		return ENAMETOOLONG;
	}

	char namebuf[256];
	memcpy(namebuf, cnp->cn_nameptr, cnp->cn_namelen);
	namebuf[cnp->cn_namelen] = '\0';

	apfs_lock_exclusive(am);
	error = apfs_remove_file(&am->am_vol, dn->an_id, namebuf);
	apfs_unlock_exclusive(am);

	if (error == 0) {
		cache_purge(vp);
	}
	return error;
}

static int
apfs_vnop_setattr(struct vnop_setattr_args *ap)
{
	if (ap == NULL || ap->a_vp == NULLVP || ap->a_vap == NULL) {
		return EINVAL;
	}
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_node *an = APFS_VP(vp);
	if (an == NULL || an->an_mount == NULL) {
		return EINVAL;
	}
	struct apfs_mount *am = an->an_mount;
	int error = 0;

	if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY) {
		return EROFS;
	}

	apfs_lock_exclusive(am);
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		error = apfs_set_file_size(&am->am_vol, an->an_id, vap->va_data_size);
		if (error == 0) {
			an->an_size = vap->va_data_size;
			an->an_ino.size = an->an_size;
			ubc_setsize(vp, (off_t)an->an_size);
		}
	}

	if (error == 0 && (VATTR_IS_ACTIVE(vap, va_mode) || VATTR_IS_ACTIVE(vap, va_uid) ||
	    VATTR_IS_ACTIVE(vap, va_gid) || VATTR_IS_ACTIVE(vap, va_modify_time))) {
		uint16_t mode = VATTR_IS_ACTIVE(vap, va_mode) ? (uint16_t)vap->va_mode : 0;
		uint32_t uid = VATTR_IS_ACTIVE(vap, va_uid) ? vap->va_uid : (uint32_t)-1;
		uint32_t gid = VATTR_IS_ACTIVE(vap, va_gid) ? vap->va_gid : (uint32_t)-1;
		uint64_t mtime_ns = 0;
		if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
			mtime_ns = (uint64_t)vap->va_modify_time.tv_sec * 1000000000ULL +
			    (uint64_t)vap->va_modify_time.tv_nsec;
		}
		error = apfs_set_file_attrs(&am->am_vol, an->an_id, mode, uid, gid, mtime_ns);
		if (error == 0) {
			if (mode) an->an_ino.mode = (an->an_ino.mode & ~07777) | (mode & 07777);
			if (uid != (uint32_t)-1) an->an_ino.owner = uid;
			if (gid != (uint32_t)-1) an->an_ino.group = gid;
		}
	}
	apfs_unlock_exclusive(am);
	return error;
}

static int
apfs_vnop_ioctl(__unused struct vnop_ioctl_args *ap)
{
	return ENOTTY;
}

static int
apfs_vnop_pathconf(struct vnop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 32767;
		return 0;
	case _PC_NAME_MAX:
		*ap->a_retval = 255;
		return 0;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		return 0;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		return 0;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		return 0;
	case _PC_CASE_SENSITIVE:
		*ap->a_retval = (APFS_VP_MOUNT(ap->a_vp)->am_vol.incompat &
		    APFS_INCOMPAT_CASE_INSENSITIVE) ? 0 : 1;
		return 0;
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		return 0;
	case _PC_XATTR_SIZE_BITS:
		*ap->a_retval = 32;
		return 0;
	default:
		return EINVAL;
	}
}

static int
apfs_vnop_open(struct vnop_open_args *ap)
{
	if ((vfs_flags(vnode_mount(ap->a_vp)) & MNT_RDONLY) &&
	    (ap->a_mode & (FWRITE | O_TRUNC | O_CREAT))) {
		return EROFS;
	}
	return 0;
}

static int
apfs_vnop_close(__unused struct vnop_close_args *ap)
{
	return 0;
}

static int
apfs_vnop_mmap(__unused struct vnop_mmap_args *ap)
{
	return 0;
}

static int
apfs_vnop_mnomap(__unused struct vnop_mnomap_args *ap)
{
	return 0;
}

static int
apfs_vnop_inactive(struct vnop_inactive_args *ap)
{
	struct apfs_node *an = APFS_VP(ap->a_vp);
	if (an == NULL) {
		return 0;
	}
	if (os_atomic_load(&an->an_cf.piece_off, relaxed) == NULL &&
	    os_atomic_load(&an->an_cf.piece_len, relaxed) == NULL &&
	    os_atomic_load(&an->an_cf.held_data, relaxed) == NULL &&
	    os_atomic_load(&an->an_cf.ra_data, relaxed) == NULL) {
		os_atomic_inc(&an->an_mount->am_inactive_free, relaxed);
		return 0;
	}
	apfs_lock(an->an_mount);
	apfs_cfile_fini(&an->an_cf);
	apfs_unlock(an->an_mount);
	return 0;
}

static int
apfs_vnop_reclaim(struct vnop_reclaim_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	if (an != NULL) {
		apfs_lock(an->an_mount);
		apfs_cfile_fini(&an->an_cf);
		apfs_unlock(an->an_mount);
		apfs_node_detach(an);
		vnode_clearfsnode(vp);
		if (an->an_link != NULL) {
			kfree_data(an->an_link, sizeof(*an->an_link) + an->an_link->len);
		}
		kfree_type(struct apfs_node, an);
	}
	return 0;
}

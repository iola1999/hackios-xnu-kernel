/*
 * APFS VFS Write Operations: vnop_write, vnop_create, vnop_remove, vnop_setattr
 */
#pragma once

#include "apfs_write.h"

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

	apfs_lock(am);
	if (ap->a_ioflag & IO_APPEND) {
		uio_setoffset(uio, (off_t)an->an_size);
	}
	uint64_t start = (uint64_t)uio_offset(uio);
	uint64_t old_size = an->an_size;

	char *bounce = kalloc_data(PAGE_SIZE, Z_WAITOK);
	if (bounce == NULL) {
		apfs_unlock(am);
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
	uint64_t new_size = an->an_size;
	bool size_changed = (new_size != old_size);
	apfs_unlock(am);

	if (end_off > start) {
		if (size_changed) {
			(void)ubc_setsize(vp, (off_t)new_size);
		}
		off_t inv_start = (off_t)(start & ~(uint64_t)PAGE_MASK);
		off_t inv_end = (off_t)((end_off + (uint64_t)PAGE_MASK) & ~(uint64_t)PAGE_MASK);
		(void)ubc_msync(vp, inv_start, inv_end, NULL, UBC_INVALIDATE);
	}
	return error;
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

	apfs_lock(am);
	error = apfs_create_file(&am->am_vol, dn->an_id, cnp->cn_nameptr, mode, uid, gid, &new_id);
	apfs_unlock(am);

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

	apfs_lock(am);
	error = apfs_remove_file(&am->am_vol, dn->an_id, cnp->cn_nameptr);
	apfs_unlock(am);

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

	apfs_lock(am);
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
		uint32_t uid = VATTR_IS_ACTIVE(vap, va_uid) ? vap->va_uid : 0;
		uint32_t gid = VATTR_IS_ACTIVE(vap, va_gid) ? vap->va_gid : 0;
		uint64_t mtime_ns = 0;
		if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
			mtime_ns = (uint64_t)vap->va_modify_time.tv_sec * 1000000000ULL +
			    (uint64_t)vap->va_modify_time.tv_nsec;
		}
		error = apfs_set_file_attrs(&am->am_vol, an->an_id, mode, uid, gid, mtime_ns);
		if (error == 0) {
			if (mode) an->an_ino.mode = (an->an_ino.mode & ~07777) | (mode & 07777);
			if (uid) an->an_ino.owner = uid;
			if (gid) an->an_ino.group = gid;
		}
	}
	apfs_unlock(am);
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


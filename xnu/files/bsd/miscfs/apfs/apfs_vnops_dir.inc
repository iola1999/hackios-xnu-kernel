/*
 * APFS VFS Directory & Link Operations: mkdir, rmdir, symlink, link, mknod, rename
 */
#pragma once

static int
apfs_vnop_mkdir(struct vnop_mkdir_args *ap)
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

	apfs_lock_exclusive(am);
	error = apfs_create_dir(&am->am_vol, dn->an_id, cnp->cn_nameptr, mode, uid, gid, &new_id);
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
apfs_vnop_rmdir(struct vnop_rmdir_args *ap)
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

	apfs_lock_exclusive(am);
	error = apfs_remove_dir(&am->am_vol, dn->an_id, cnp->cn_nameptr);
	apfs_unlock_exclusive(am);

	if (error == 0) {
		cache_purge(vp);
	}
	return error;
}

static int
apfs_vnop_symlink(struct vnop_symlink_args *ap)
{
	if (ap == NULL || ap->a_dvp == NULLVP || ap->a_vpp == NULL ||
	    ap->a_cnp == NULL || ap->a_vap == NULL || ap->a_target == NULL) {
		return EINVAL;
	}
	vnode_t dvp = ap->a_dvp;
	vnode_t *vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	struct vnode_attr *vap = ap->a_vap;
	const char *target = ap->a_target;
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
	if (strlen(target) >= MAXPATHLEN) {
		return ENAMETOOLONG;
	}

	uint32_t uid = VATTR_IS_ACTIVE(vap, va_uid) ? vap->va_uid : 0;
	uint32_t gid = VATTR_IS_ACTIVE(vap, va_gid) ? vap->va_gid : 0;

	apfs_lock_exclusive(am);
	error = apfs_create_symlink(&am->am_vol, dn->an_id, cnp->cn_nameptr, target, uid, gid, &new_id);
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
apfs_vnop_link(struct vnop_link_args *ap)
{
	if (ap == NULL || ap->a_vp == NULLVP || ap->a_tdvp == NULLVP || ap->a_cnp == NULL) {
		return EINVAL;
	}
	vnode_t vp = ap->a_vp;
	vnode_t tdvp = ap->a_tdvp;
	struct componentname *cnp = ap->a_cnp;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_node *tdn = APFS_VP(tdvp);
	if (an == NULL || tdn == NULL || an->an_mount == NULL || tdn->an_mount != an->an_mount) {
		return EINVAL;
	}
	struct apfs_mount *am = an->an_mount;
	int error;

	if (vfs_flags(vnode_mount(tdvp)) & MNT_RDONLY) {
		return EROFS;
	}
	if (vnode_isreg(vp) == 0 && vnode_islnk(vp) == 0) {
		return EPERM;
	}
	if (cnp->cn_nameptr == NULL) {
		return EINVAL;
	}
	if (cnp->cn_namelen == 0 || cnp->cn_namelen > 255) {
		return ENAMETOOLONG;
	}

	apfs_lock_exclusive(am);
	error = apfs_create_link(&am->am_vol, an->an_id, tdn->an_id, cnp->cn_nameptr);
	apfs_unlock_exclusive(am);

	if (error == 0) {
		an->an_ino.nchildren++;
	}
	return error;
}

static int
apfs_vnop_mknod(struct vnop_mknod_args *ap)
{
	if (ap == NULL || ap->a_dvp == NULLVP || ap->a_cnp == NULL || ap->a_vap == NULL) {
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

	if (vpp != NULL) {
		*vpp = NULLVP;
	}
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
	switch (vap->va_type) {
	case VSOCK:
		mode |= S_IFSOCK;
		break;
	case VFIFO:
		mode |= S_IFIFO;
		break;
	case VCHR:
		mode |= S_IFCHR;
		break;
	case VBLK:
		mode |= S_IFBLK;
		break;
	default:
		return EINVAL;
	}

	uint32_t rdev = VATTR_IS_ACTIVE(vap, va_rdev) ? (uint32_t)vap->va_rdev : 0;
	uint32_t uid = VATTR_IS_ACTIVE(vap, va_uid) ? vap->va_uid : 0;
	uint32_t gid = VATTR_IS_ACTIVE(vap, va_gid) ? vap->va_gid : 0;

	apfs_lock_exclusive(am);
	error = apfs_create_mknod(&am->am_vol, dn->an_id, cnp->cn_nameptr, mode, rdev, uid, gid, &new_id);
	apfs_unlock_exclusive(am);

	if (error == 0 && vpp != NULL) {
		error = apfs_node_vnode(am, new_id, dvp, cnp, vpp);
		if (error == 0 && (cnp->cn_flags & MAKEENTRY)) {
			cache_enter(dvp, *vpp, cnp);
		}
	}
	return error;
}

static int
apfs_vnop_rename(struct vnop_rename_args *ap)
{
	if (ap == NULL || ap->a_fdvp == NULLVP || ap->a_fvp == NULLVP ||
	    ap->a_fcnp == NULL || ap->a_tdvp == NULLVP || ap->a_tcnp == NULL) {
		return EINVAL;
	}
	vnode_t fdvp = ap->a_fdvp;
	vnode_t fvp = ap->a_fvp;
	struct componentname *fcnp = ap->a_fcnp;
	vnode_t tdvp = ap->a_tdvp;
	vnode_t tvp = ap->a_tvp;
	struct componentname *tcnp = ap->a_tcnp;

	struct apfs_node *fdn = APFS_VP(fdvp);
	struct apfs_node *tdn = APFS_VP(tdvp);
	if (fdn == NULL || tdn == NULL || fdn->an_mount == NULL || tdn->an_mount != fdn->an_mount) {
		return EINVAL;
	}
	struct apfs_mount *am = fdn->an_mount;
	int error;

	if (vfs_flags(vnode_mount(fdvp)) & MNT_RDONLY) {
		return EROFS;
	}
	if (fcnp->cn_nameptr == NULL || tcnp->cn_nameptr == NULL) {
		return EINVAL;
	}
	if (fcnp->cn_namelen == 0 || fcnp->cn_namelen > 255 ||
	    tcnp->cn_namelen == 0 || tcnp->cn_namelen > 255) {
		return ENAMETOOLONG;
	}

	apfs_lock_exclusive(am);
	error = apfs_rename_file(&am->am_vol, fdn->an_id, fcnp->cn_nameptr,
	    tdn->an_id, tcnp->cn_nameptr);
	apfs_unlock_exclusive(am);

	if (error == 0) {
		cache_purge(fvp);
		if (tvp != NULLVP) {
			cache_purge(tvp);
		}
	}
	return error;
}

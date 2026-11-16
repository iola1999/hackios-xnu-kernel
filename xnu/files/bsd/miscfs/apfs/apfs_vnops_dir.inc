/*
 * The vnode operations that add, remove and move names on a writable mount: create, mkdir,
 * mknod, symlink, link, remove, rmdir, rename, renamex and clonefile (included by
 * apfs_vnops_write.inc).
 *
 * An object whose last name goes while it is still open (vnode_isinuse) stays in the private
 * directory as an orphan until vnop_inactive reaps it, as on APFS; one that is not open goes
 * with its name, and its vnode is recycled.
 */
#pragma once

static int
apfs_wname(struct componentname *cnp)
{
	if (cnp == NULL || cnp->cn_nameptr == NULL) {
		return EINVAL;
	}
	return (cnp->cn_namelen == 0 || cnp->cn_namelen > 255) ? ENAMETOOLONG : 0;
}

/*
 * A new object named cnp in dvp, of type `type`, from the attributes VFS hands over (those it
 * sets for itself afterwards are the ones not marked supported here).
 */
static int
apfs_wmake(vnode_t dvp, vnode_t *vpp, struct componentname *cnp, struct vnode_attr *vap,
    enum vtype type, const char *target)
{
	struct apfs_node *dn = APFS_VP(dvp);
	struct apfs_mount *am = dn->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	struct apfs_wnew a = { 0 };
	struct apfs_wattr t = { 0 };
	uint64_t id = 0;
	int error;

	if (vpp != NULL) {
		*vpp = NULLVP;
	}
	if (w == NULL) {
		return EROFS;
	}
	error = apfs_wname(cnp);
	if (error) {
		return error;
	}
	switch (type) {
	case VREG:
		a.mode = APFS_S_IFREG;
		break;
	case VDIR:
		a.mode = APFS_S_IFDIR;
		break;
	case VLNK:
		a.mode = APFS_S_IFLNK;
		break;
	case VSOCK:
		a.mode = APFS_S_IFSOCK;
		break;
	case VFIFO:
		a.mode = APFS_S_IFIFO;
		break;
	case VCHR:
		a.mode = APFS_S_IFCHR;
		break;
	case VBLK:
		a.mode = APFS_S_IFBLK;
		break;
	default:
		return EINVAL;
	}
	a.mode |= VATTR_IS_ACTIVE(vap, va_mode) ? (uint16_t)(vap->va_mode & 07777) :
	    (type == VLNK ? 0755 : 0);
	a.uid = VATTR_IS_ACTIVE(vap, va_uid) ? vap->va_uid : 0;
	a.gid = VATTR_IS_ACTIVE(vap, va_gid) ? vap->va_gid : 0;
	if ((type == VCHR || type == VBLK) && VATTR_IS_ACTIVE(vap, va_rdev)) {
		a.rdev = (uint32_t)vap->va_rdev;
	}
	if (target != NULL) {
		a.target = target;
		a.target_len = strlen(target);
	}
	if (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {
		a.pclass = vap->va_dataprotect_class;
	}
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		t.set |= APFS_WA_FLAGS;
		t.flags = vap->va_flags;
	}
	if (VATTR_IS_ACTIVE(vap, va_create_time)) {
		t.set |= APFS_WA_CRTIME;
		t.crtime = apfs_timespec_ns(vap->va_create_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		t.set |= APFS_WA_MTIME;
		t.mtime = apfs_timespec_ns(vap->va_modify_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		t.set |= APFS_WA_ATIME;
		t.atime = apfs_timespec_ns(vap->va_access_time);
	}
	apfs_lock_exclusive(am);
	error = apfs_w_create(w, dn->an_id, cnp->cn_nameptr, (size_t)cnp->cn_namelen, &a, &id);
	if (error == 0 && t.set != 0) {
		error = apfs_w_setattr(w, id, &t);
	}
	if (error == 0) {
		apfs_node_refresh(am, dn);
		apfs_wdirty(am, true);
	}
	apfs_unlock_exclusive(am);
	if (error) {
		return error;
	}
	VATTR_SET_SUPPORTED(vap, va_mode);
	VATTR_SET_SUPPORTED(vap, va_uid);
	VATTR_SET_SUPPORTED(vap, va_gid);
	VATTR_SET_SUPPORTED(vap, va_type);
	VATTR_SET_SUPPORTED(vap, va_rdev);
	VATTR_SET_SUPPORTED(vap, va_dataprotect_class);
	VATTR_SET_SUPPORTED(vap, va_flags);
	VATTR_SET_SUPPORTED(vap, va_create_time);
	VATTR_SET_SUPPORTED(vap, va_modify_time);
	VATTR_SET_SUPPORTED(vap, va_access_time);
	/* the negative entries the name cache has for dvp may include this name in another case */
	cache_purge_negatives(dvp);
	return vpp != NULL ? apfs_node_vnode(am, id, dvp, cnp, vpp) : 0;
}

static int
apfs_vnop_create(struct vnop_create_args *ap)
{
	enum vtype type = VATTR_IS_ACTIVE(ap->a_vap, va_type) ? ap->a_vap->va_type : VREG;

	return apfs_wmake(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, type, NULL);
}

static int
apfs_vnop_mkdir(struct vnop_mkdir_args *ap)
{
	return apfs_wmake(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VDIR, NULL);
}

static int
apfs_vnop_mknod(struct vnop_mknod_args *ap)
{
	enum vtype type = ap->a_vap->va_type;

	if (type != VSOCK && type != VFIFO && type != VCHR && type != VBLK) {
		return EINVAL;
	}
	return apfs_wmake(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, type, NULL);
}

static int
apfs_vnop_symlink(struct vnop_symlink_args *ap)
{
	if (ap->a_target == NULL || strlen(ap->a_target) >= MAXPATHLEN) {
		return ENAMETOOLONG;
	}
	return apfs_wmake(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VLNK, ap->a_target);
}

static int
apfs_vnop_link(struct vnop_link_args *ap)
{
	struct apfs_node *an = APFS_VP(ap->a_vp), *tdn = APFS_VP(ap->a_tdvp);
	struct apfs_mount *am = tdn->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	struct componentname *cnp = ap->a_cnp;
	int error;

	if (w == NULL) {
		return EROFS;
	}
	if (an->an_mount != am) {
		return EXDEV;
	}
	if (vnode_isdir(ap->a_vp)) {
		return EPERM;
	}
	error = apfs_wname(cnp);
	if (error == 0) {
		error = APFS_WCALL(am, an, tdn, apfs_w_link(w, an->an_id, tdn->an_id, cnp->cn_nameptr,
		    (size_t)cnp->cn_namelen));
	}
	if (error == 0) {
		cache_purge_negatives(ap->a_tdvp);
	}
	return error;
}

/* An object that lost its last name and is not open: its pages and its vnode go too. */
static void
apfs_wgone(vnode_t vp)
{
	if (vnode_isreg(vp)) {
		ubc_setsize(vp, 0);
	}
	vnode_recycle(vp);
}

static int
apfs_wremove(vnode_t dvp, vnode_t vp, struct componentname *cnp, bool dir)
{
	struct apfs_node *dn = APFS_VP(dvp), *an = APFS_VP(vp);
	struct apfs_mount *am = dn->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	bool keep = !dir && vnode_isinuse(vp, 0), gone = false;
	uint64_t id = 0;
	int error;

	if (w == NULL) {
		return EROFS;
	}
	error = apfs_wname(cnp);
	if (error) {
		return error;
	}
	apfs_lock_exclusive(am);
	error = apfs_w_remove(w, dn->an_id, cnp->cn_nameptr, (size_t)cnp->cn_namelen, dir, keep, &id, &gone);
	if (error == 0) {
		apfs_node_refresh(am, dn);
		if (!gone && id == an->an_id) {
			apfs_node_refresh(am, an);
			an->an_orphan = an->an_ino.nchildren == 0;
		}
		apfs_wdirty(am, true);
	}
	apfs_unlock_exclusive(am);
	if (error == 0) {
		cache_purge(vp);
		if (gone) {
			apfs_wgone(vp);
		}
	}
	return error;
}

static int
apfs_vnop_remove(struct vnop_remove_args *ap)
{
	return apfs_wremove(ap->a_dvp, ap->a_vp, ap->a_cnp, false);
}

static int
apfs_vnop_rmdir(struct vnop_rmdir_args *ap)
{
	return apfs_wremove(ap->a_dvp, ap->a_vp, ap->a_cnp, true);
}

static int
apfs_wrename(vnode_t fdvp, vnode_t fvp, struct componentname *fcnp, vnode_t tdvp, vnode_t tvp,
    struct componentname *tcnp, uint32_t flags)
{
	struct apfs_node *fdn = APFS_VP(fdvp), *tdn = APFS_VP(tdvp), *fn = APFS_VP(fvp);
	struct apfs_node *tn = tvp != NULLVP ? APFS_VP(tvp) : NULL;
	struct apfs_mount *am = fdn->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	bool swap = (flags & APFS_WREN_SWAP) != 0, gone = false;
	bool keep = tvp != NULLVP && !swap && !vnode_isdir(tvp) && vnode_isinuse(tvp, 0);
	uint64_t replaced = 0;
	int error;

	if (w == NULL) {
		return EROFS;
	}
	if (tdn->an_mount != am) {
		return EXDEV;
	}
	error = apfs_wname(fcnp);
	if (error == 0) {
		error = apfs_wname(tcnp);
	}
	if (error) {
		return error;
	}
	/*
	 * A file put in place of another (the write-then-rename way of replacing one) sends its
	 * pages first: the commit that names it then waits for them, and after a crash the name
	 * leads to the new contents or to the old file, never to a file of holes.
	 */
	if (tvp != NULLVP && !swap && vnode_isreg(fvp) && ubc_getsize(fvp) > 0) {
		(void)ubc_msync(fvp, 0, ubc_getsize(fvp), NULL, UBC_PUSHDIRTY);
	}
	apfs_lock_exclusive(am);
	error = apfs_w_rename(w, fdn->an_id, fcnp->cn_nameptr, (size_t)fcnp->cn_namelen, tdn->an_id,
	    tcnp->cn_nameptr, (size_t)tcnp->cn_namelen, flags, keep, &replaced, &gone);
	if (error == 0) {
		apfs_node_refresh(am, fdn);
		apfs_node_refresh(am, tdn);
		apfs_node_refresh(am, fn);
		if (tn != NULL && !gone) {
			apfs_node_refresh(am, tn);
			tn->an_orphan = !swap && !vnode_isdir(tvp) && tn->an_ino.nchildren == 0;
		}
		apfs_wdirty(am, true);
	}
	apfs_unlock_exclusive(am);
	if (error == 0) {
		cache_purge(fvp);
		if (tvp != NULLVP) {
			cache_purge(tvp);
			if (gone) {
				apfs_wgone(tvp);
			}
		}
		cache_purge_negatives(tdvp);
	}
	return error;
}

static int
apfs_vnop_rename(struct vnop_rename_args *ap)
{
	return apfs_wrename(ap->a_fdvp, ap->a_fvp, ap->a_fcnp, ap->a_tdvp, ap->a_tvp, ap->a_tcnp, 0);
}

static int
apfs_vnop_renamex(struct vnop_renamex_args *ap)
{
	uint32_t flags = 0;

	if (ap->a_flags & ~(VFS_RENAME_SWAP | VFS_RENAME_EXCL)) {
		return ENOTSUP;
	}
	if (ap->a_flags & VFS_RENAME_SWAP) {
		flags |= APFS_WREN_SWAP;
	}
	if (ap->a_flags & VFS_RENAME_EXCL) {
		flags |= APFS_WREN_EXCL;
	}
	return apfs_wrename(ap->a_fdvp, ap->a_fvp, ap->a_fcnp, ap->a_tdvp, ap->a_tvp, ap->a_tcnp, flags);
}

/* clonefile(2): a copy with blocks of its own, made from what is on the device (apfs_wclone.inc). */
static int
apfs_vnop_clonefile(struct vnop_clonefile_args *ap)
{
	vnode_t fvp = ap->a_fvp, dvp = ap->a_dvp;
	struct apfs_node *fn = APFS_VP(fvp), *dn = APFS_VP(dvp);
	struct apfs_mount *am = dn->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	struct vnode_attr *vap = ap->a_vap;
	struct componentname *cnp = ap->a_cnp;
	struct apfs_wattr top = { 0 };
	uint32_t flags = (ap->a_flags & VNODE_CLONEFILE_NOOWNERCOPY) ? APFS_WCLONE_OWNER : 0;
	uint64_t id = 0;
	int error;

	*ap->a_vpp = NULLVP;
	if (w == NULL) {
		return EROFS;
	}
	if (fn->an_mount != am) {
		return EXDEV;
	}
	error = apfs_wname(cnp);
	if (error) {
		return error;
	}
	if (vnode_isdir(fvp)) {
		vnode_iterate(am->am_mp, 0, apfs_wpush_vnode, am);
	} else {
		(void)apfs_wpush_vnode(fvp, am);
	}
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		top.set |= APFS_WA_MODE;
		top.mode = (uint16_t)(vap->va_mode & 07777);
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		top.set |= APFS_WA_UID;
		top.uid = vap->va_uid;
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		top.set |= APFS_WA_GID;
		top.gid = vap->va_gid;
	}
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		top.set |= APFS_WA_FLAGS;
		top.flags = vap->va_flags;
	}
	error = APFS_WCALL(am, dn, NULL, apfs_w_clone(w, fn->an_id, dn->an_id, cnp->cn_nameptr,
	    (size_t)cnp->cn_namelen, &top, flags, &id));
	if (error) {
		return error;
	}
	VATTR_SET_SUPPORTED(vap, va_type);
	VATTR_SET_SUPPORTED(vap, va_mode);
	VATTR_SET_SUPPORTED(vap, va_uid);
	VATTR_SET_SUPPORTED(vap, va_gid);
	VATTR_SET_SUPPORTED(vap, va_flags);
	cache_purge_negatives(dvp);
	return apfs_node_vnode(am, id, dvp, cnp, ap->a_vpp);
}

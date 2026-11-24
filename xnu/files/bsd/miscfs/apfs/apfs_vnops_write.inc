/*
 * The vnode operations that change one object of a writable mount (apfs_wtx_of(am) != NULL):
 * its contents, attributes and extended attributes, fsync and the fsctls, and the end of its
 * vnode.  Names are in apfs_vnops_dir.inc, commits in apfs_vfsops_write.inc.
 *
 * A file's contents go through the page cache and the cluster layer.  vnop_blockmap gives the
 * pages their blocks as they go out (a hole gets new blocks, the rest is written where it is)
 * and vnop_strategy counts every data write for the commit that has to wait for it.  Locks:
 * the node's an_rwlock round a change of its size, then the mount's exclusive lock round each
 * call into apfs_write.c.  The cluster layer is never entered with the mount's lock held,
 * since vnop_blockmap takes it.
 */
#pragma once

#include <sys/disk.h>
#include <sys/fsctl.h>
#include "apfs_write.h"

#define APFS_XATTR_MAX              (1024 * 1024)      /* the longest attribute set through here */
/*
 * Blocks a write leaves free for pages already accepted: those get their blocks only when
 * they go out (vnop_blockmap), and a pageout that finds no space loses them.
 */
#define APFS_WSPARE_BLOCKS          16384
/* APFS's own fsctls that iOS 26 sends to a local apfs volume (ramfs_fsctl.inc has the callers) */
#define APFS_IOC_MAINTAIN_DIR_STATS _IOW('J', 2, uint64_t)
#define APFS_IOC_MARK_PURGEABLE     _IOWR('J', 68, uint64_t)
#define APFS_IOC_SET_SYNCROOT       _IOW('J', 72, uint32_t)
#define APFS_IOC_GET_SYNCROOT       _IOR('J', 72, uint32_t)
#define APFS_IOC_PARENT_SYNCROOT    _IOR('J', 74, uint64_t)

/* The inode record again after a change to the object; with the exclusive lock held. */
static void
apfs_node_refresh(struct apfs_mount *am, struct apfs_node *an)
{
	struct apfs_inode ino;

	if (an != NULL && apfs_inode_get(&am->am_vol, an->an_id, &ino) == 0) {
		an->an_ino = ino;
		an->an_mode = ino.mode;
		an->an_parent = ino.parent_id;
		an->an_wseq = apfs_wcommits(am) + 1;
	}
}

static uint64_t
apfs_timespec_ns(struct timespec ts)
{
	return ts.tv_sec < 0 ? 0 : (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

/* One call into apfs_write.c that changes `an` (and `other`, when not NULL). */
#define APFS_WCALL(am, an, other, call) ({                              \
	int __e;                                                        \
	apfs_lock_exclusive(am);                                        \
	__e = (call);                                                   \
	if (__e == 0) {                                                 \
	        apfs_node_refresh((am), (an));                          \
	        apfs_node_refresh((am), (other));                       \
	        apfs_wdirty(am, true);                                  \
	}                                                               \
	apfs_unlock_exclusive(am);                                      \
	__e;                                                            \
})

/* ------------------------------------------------------------------ contents */

/* read(2) of a plain file of a writable volume: through the page cache, as it is written. */
static int
apfs_wread(struct vnop_read_args *ap)
{
	struct apfs_node *an = APFS_VP(ap->a_vp);
	int error;

	lck_rw_lock_shared(&an->an_rwlock);
	error = cluster_read(ap->a_vp, ap->a_uio, (off_t)an->an_size, ap->a_ioflag);
	lck_rw_unlock_shared(&an->an_rwlock);
	return error;
}

static int
apfs_wpagein(struct vnop_pagein_args *ap)
{
	return cluster_pagein(ap->a_vp, ap->a_pl, ap->a_pl_offset, ap->a_f_offset, (int)ap->a_size,
	           (off_t)APFS_VP(ap->a_vp)->an_size, ap->a_flags);
}

/*
 * A regular file's new length, with an_rwlock held exclusive.  Pages past a shorter end go
 * before its blocks do; past a longer one, the rest of the old last page is zeroed through the
 * page cache (the engine leaves file data to it).
 */
static int
apfs_wresize(vnode_t vp, struct apfs_node *an, uint64_t size, int ioflag)
{
	struct apfs_mount *am = an->an_mount;
	uint64_t old = an->an_size;
	int error;

	if (size < old) {
		ubc_setsize(vp, (off_t)size);
	}
	error = APFS_WCALL(am, an, NULL, apfs_w_truncate(apfs_wtx_of(am), an->an_id, size));
	if (error) {
		if (size < old) {
			ubc_setsize(vp, (off_t)old);
		}
		return error;
	}
	an->an_size = size;
	if (size > old) {
		off_t limit = MIN((off_t)round_page_64(old), (off_t)size);

		if (limit > (off_t)old) {
			error = cluster_write(vp, NULL, (off_t)old, limit, (off_t)old, 0,
			    IO_HEADZEROFILL | IO_NOZERODIRTY | (ioflag & IO_SYNC));
		}
		ubc_setsize(vp, (off_t)size);
	}
	return error;
}

static int
apfs_vnop_write(struct vnop_write_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	int ioflag = ap->a_ioflag, error = 0;
	user_ssize_t resid = uio_resid(uio);
	off_t old, end, size;

	if (w == NULL) {
		printf("apfs: write ino %llu refused: no write transaction (EROFS)\n", an->an_id);
		return EROFS;
	}
	if (!vnode_isreg(vp)) {
		printf("apfs: write ino %llu refused: not a regular file\n", an->an_id);
		return vnode_isdir(vp) ? EISDIR : EPERM;
	}
	if (uio_offset(uio) < 0) {
		printf("apfs: write ino %llu refused: negative offset\n", an->an_id);
		return EINVAL;
	}
	if (resid == 0) {
		return 0;
	}
	if (an->an_ino.bsd_flags & UF_COMPRESSED) {
		printf("apfs: write ino %llu refused: UF_COMPRESSED (EPERM)\n", an->an_id);
		return EPERM;                   /* decmpfs contents are never rewritten here */
	}
	lck_rw_lock_exclusive(&an->an_rwlock);
	old = (off_t)an->an_size;
	if (ioflag & IO_APPEND) {
		uio_setoffset(uio, old);
	}
	if (os_add_overflow(uio_offset(uio), resid, &end) || end >= (1LL << 56)) {
		error = EFBIG;
		goto out;
	}
	size = MAX(old, end);
	if (apfs_wtx_free_blocks(w) < (uint64_t)resid / am->am_nx.block_size + APFS_WSPARE_BLOCKS) {
		error = ENOSPC;
		goto out;
	}
	if (size > old) {
		error = apfs_wresize(vp, an, (uint64_t)size, 0);
	} else {
		struct apfs_wattr t = { .set = APFS_WA_MTIME, .mtime = apfs_now_ns() };

		error = APFS_WCALL(am, an, NULL, apfs_w_setattr(w, an->an_id, &t));
	}
	if (error) {
		goto out;
	}
	apfs_lock(am);
	apfs_wnote(am, an->an_id);
	apfs_unlock(am);
	error = cluster_write(vp, uio, old, size, 0, 0,
	    ioflag & ~(IO_TAILZEROFILL | IO_HEADZEROFILL | IO_NOZEROVALID | IO_NOZERODIRTY));
	if (error && size > old && uio_offset(uio) < size) {
		/* what was not written does not stay as a hole at the end */
		(void)apfs_wresize(vp, an, (uint64_t)MAX(old, uio_offset(uio)), 0);
	}
	if (error == 0 && (ioflag & IO_SYNC)) {
		error = apfs_wcommit(am, false, an->an_wseq, APFS_WHY_IOSYNC);
	}
out:
	lck_rw_unlock_exclusive(&an->an_rwlock);
	if (error) {
		printf("apfs: write ino %llu resid %lld ioflag 0x%x failed: %d (free %llu)\n",
		    an->an_id, (long long)resid, ioflag, error,
		    (unsigned long long)apfs_wtx_free_blocks(w));
	}
	return error;
}

static int
apfs_vnop_pageout(struct vnop_pageout_args *ap)
{
	struct apfs_node *an = APFS_VP(ap->a_vp);

	if (ap->a_pl == NULL) {
		return EINVAL;
	}
	if (apfs_wtx_of(an->an_mount) == NULL || !vnode_isreg(ap->a_vp)) {
		if (!(ap->a_flags & UPL_NOCOMMIT)) {
			ubc_upl_abort_range(ap->a_pl, ap->a_pl_offset, (upl_size_t)ap->a_size,
			    UPL_ABORT_FREE_ON_EMPTY);
		}
		return EROFS;
	}
	return cluster_pageout(ap->a_vp, ap->a_pl, ap->a_pl_offset, ap->a_f_offset, (int)ap->a_size,
	           (off_t)an->an_size, ap->a_flags);
}

/*
 * Where bytes [a_foffset, a_foffset + a_size) of a file are on the device.  For a write a hole
 * gets blocks of its own, and every mapping is counted until its buffer is sent (apfs_wmap_add).
 */
static int
apfs_vnop_blockmap(struct vnop_blockmap_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	uint64_t bs = am->am_nx.block_size, off = (uint64_t)ap->a_foffset, phys = 0, run = 0;
	int error;

	if (ap->a_poff != NULL) {
		*(int *)ap->a_poff = 0;
	}
	if (w == NULL || !vnode_isreg(vp) || (an->an_ino.bsd_flags & UF_COMPRESSED)) {
		return ENOTSUP;
	}
	if (ap->a_flags & VNODE_WRITE) {
		bool grew = false;

		apfs_lock_exclusive(am);
		error = apfs_w_map(w, an->an_id, off, ap->a_size, false, &phys, &run);
		if (error == 0 && phys == 0) {
			error = apfs_w_map(w, an->an_id, off, ap->a_size, true, &phys, &run);
			grew = error == 0;
		}
		if (error == 0 && phys != 0) {
			apfs_wmap_add(am, phys, (MIN(run, (uint64_t)ap->a_size) + off % bs + bs - 1) / bs, an->an_id, off);
		}
		if (grew) {             /* after apfs_wmap_add: no checkpoint names the blocks before they are sent */
			apfs_node_refresh(am, an);
			apfs_wdirty(am, false);
		}
		apfs_unlock_exclusive(am);
	} else {
		struct apfs_extent e;

		apfs_lock_shared(am);
		error = apfs_extent_at(&am->am_vol, an->an_ino.private_id, off, &e);
		apfs_unlock_shared(am);
		if (error == ENOENT) {
			error = 0;
			run = ap->a_size;
		} else if (error == 0) {
			run = e.logical + e.length - off;
			phys = e.phys != 0 ? e.phys + (off - e.logical) / bs : 0;
		}
	}
	if (error) {
		return error;
	}
	*ap->a_bpn = phys != 0 ? apfs_wdevblock(am, phys, off % bs) : (daddr64_t)-1;
	if (ap->a_run != NULL) {
		*ap->a_run = (size_t)MIN(run, (uint64_t)ap->a_size);
	}
	return 0;
}

static int
apfs_vnop_strategy(struct vnop_strategy_args *ap)
{
	buf_t bp = ap->a_bp;
	struct apfs_mount *am = APFS_VP_MOUNT(buf_vnode(bp));

	if (!(buf_flags(bp) & B_READ) && apfs_wtx_of(am) != NULL) {
		apfs_wio_start(am, bp);
	}
	return buf_strategy(am->am_devvp, ap);
}

static int
apfs_vnop_blktooff(struct vnop_blktooff_args *ap)
{
	*ap->a_offset = (off_t)ap->a_lblkno * PAGE_SIZE_64;
	return 0;
}

static int
apfs_vnop_offtoblk(struct vnop_offtoblk_args *ap)
{
	*ap->a_lblkno = (daddr64_t)(ap->a_offset / PAGE_SIZE_64);
	return 0;
}

/* ------------------------------------------------------------------ fsync and fsctl */

/* The file has a decmpfs header: its contents are compressed (apfs_lib's cfile_header). */
static bool
apfs_has_decmpfs(struct apfs_mount *am, struct apfs_node *an)
{
	uint8_t hdr[16];
	uint32_t magic = 0;
	size_t n = 0;
	int error;

	apfs_lock_shared(am);
	error = apfs_xattr_read(&am->am_vol, an->an_id, APFS_DECMPFS_XATTR_NAME, hdr, sizeof(hdr), &n);
	apfs_unlock_shared(am);
	if (error == 0 && n >= sizeof(hdr)) {
		memcpy(&magic, hdr, sizeof(magic));
	}
	return magic == APFS_DECMPFS_MAGIC;
}

/* fsync(2): the file's pages on the device, and a commit if anything of it is not committed. */
static int
apfs_vnop_fsync(struct vnop_fsync_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	bool wait = ap->a_waitfor == MNT_WAIT;

	if (apfs_wtx_of(am) == NULL) {
		return 0;
	}
	if (vnode_isreg(vp) && ubc_getsize(vp) > 0) {
		(void)ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | (wait ? UBC_SYNC : 0));
	}
	return wait && an->an_wseq > apfs_wcommits(am) ? apfs_wcommit(am, false, an->an_wseq, APFS_WHY_FSYNC) : 0;
}

/* The nearest proper ancestor of an_id's directory that is a sync root, 0 if none (shared lock). */
static uint64_t
apfs_parent_syncroot(struct apfs_mount *am, struct apfs_node *an)
{
	uint64_t at = an->an_parent, found = 0;
	struct apfs_inode ino;

	apfs_lock_shared(am);
	for (int depth = 0; depth < 512 && at >= APFS_ROOT_DIR_INO_NUM; depth++) {
		if (apfs_inode_get(&am->am_vol, at, &ino) != 0) {
			break;
		}
		if (ino.internal_flags & APFS_INODE_IS_SYNC_ROOT) {
			found = at;
			break;
		}
		if (at == APFS_ROOT_DIR_INO_NUM) {
			break;
		}
		at = ino.parent_id;
	}
	apfs_unlock_shared(am);
	return found;
}

static int
apfs_vnop_ioctl(struct vnop_ioctl_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	struct apfs_wattr a = { 0 };

	switch (ap->a_command) {
	case F_FULLFSYNC:
	case F_BARRIERFSYNC:
		if (w == NULL) {
			return 0;
		}
		if (vnode_isreg(vp) && ubc_getsize(vp) > 0) {
			(void)ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | UBC_SYNC);
		}
		return apfs_wflush(am);
	case FSIOC_CAS_BSDFLAGS: {
		struct fsioc_cas_bsdflags *cas = (struct fsioc_cas_bsdflags *)ap->a_data;
		/* VFS calls this without vnode_setattr; the decmpfs rule of apfs_vnop_setattr holds here too */
		bool compressible = !(cas->new_flags & UF_COMPRESSED) || apfs_has_decmpfs(am, an);
		int error = 0;

		if (w == NULL) {
			return EROFS;
		}
		apfs_lock_exclusive(am);
		apfs_node_refresh(am, an);
		cas->actual_flags = an->an_ino.bsd_flags;
		if (cas->actual_flags == cas->expected_flags) {
			uint32_t doc;

			a.set = APFS_WA_FLAGS;
			a.flags = cas->new_flags;
			if (!compressible && !(cas->actual_flags & UF_COMPRESSED)) {
				a.flags &= ~UF_COMPRESSED;
			}
			error = apfs_w_setattr(w, an->an_id, &a);
			if (error == 0 && (a.flags & UF_TRACKED)) {
				error = apfs_w_docid(w, an->an_id, &doc);
			}
			if (error == 0) {
				apfs_node_refresh(am, an);
				apfs_wdirty(am, true);
			}
		}
		apfs_unlock_exclusive(am);
		return error;
	}
	case APFS_IOC_MARK_PURGEABLE:
	case APFS_IOC_MAINTAIN_DIR_STATS:
		return 0;               /* nothing is purged and no directory stats are kept */
	case APFS_IOC_SET_SYNCROOT:
		if (!vnode_isdir(vp)) {
			return ENOTDIR;
		}
		if (w == NULL) {
			return EROFS;
		}
		a.set = APFS_WA_IFLAGS;
		if (*(uint32_t *)ap->a_data != 0) {
			a.iflags_on = APFS_INODE_IS_SYNC_ROOT;
		} else {
			a.iflags_off = APFS_INODE_IS_SYNC_ROOT;
		}
		return APFS_WCALL(am, an, NULL, apfs_w_setattr(w, an->an_id, &a));
	case APFS_IOC_GET_SYNCROOT:
		*(uint32_t *)ap->a_data = (an->an_ino.internal_flags & APFS_INODE_IS_SYNC_ROOT) != 0;
		return 0;
	case APFS_IOC_PARENT_SYNCROOT:
		*(uint64_t *)ap->a_data = apfs_parent_syncroot(am, an);
		return 0;
	default:
		return apfs_graft_ioctl(vp, ap->a_command, ap->a_data, ap->a_context);    /* else ENOTTY */
	}
}

/* ------------------------------------------------------------------ attributes */

static int
apfs_vnop_setattr(struct vnop_setattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_mount *am = an->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	struct apfs_wattr a = { 0 };
	bool track = false;
	int error = 0;

	if (w == NULL) {
		return EROFS;
	}
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		if (!vnode_isreg(vp)) {
			return vnode_isdir(vp) ? EISDIR : EINVAL;
		}
		if (an->an_ino.bsd_flags & UF_COMPRESSED) {
			return EPERM;
		}
		lck_rw_lock_exclusive(&an->an_rwlock);
		if (vap->va_data_size != an->an_size) {
			error = apfs_wresize(vp, an, vap->va_data_size, 0);
		}
		lck_rw_unlock_exclusive(&an->an_rwlock);
		if (error) {
			return error;
		}
		VATTR_SET_SUPPORTED(vap, va_data_size);
	}
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		a.set |= APFS_WA_MODE;
		a.mode = (uint16_t)(vap->va_mode & 07777);
		VATTR_SET_SUPPORTED(vap, va_mode);
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		a.set |= APFS_WA_UID;
		a.uid = vap->va_uid;
		VATTR_SET_SUPPORTED(vap, va_uid);
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		a.set |= APFS_WA_GID;
		a.gid = vap->va_gid;
		VATTR_SET_SUPPORTED(vap, va_gid);
	}
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		/*
		 * As decmpfs_update_attributes: UF_COMPRESSED goes on only over a decmpfs header set
		 * before it, and not with a new size in the same call.  (Else the file would read
		 * and write as compressed while its bytes are plain: libarchive restores the flag
		 * of every file of an IPA made on a Mac.)
		 */
		if ((vap->va_flags & UF_COMPRESSED) && !(an->an_ino.bsd_flags & UF_COMPRESSED) &&
		    (VATTR_IS_ACTIVE(vap, va_data_size) || !apfs_has_decmpfs(am, an))) {
			vap->va_flags &= ~UF_COMPRESSED;
		}
		a.set |= APFS_WA_FLAGS;
		a.flags = vap->va_flags;
		track = (vap->va_flags & UF_TRACKED) != 0;
		VATTR_SET_SUPPORTED(vap, va_flags);
	}
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		a.set |= APFS_WA_ATIME;
		a.atime = apfs_timespec_ns(vap->va_access_time);
		VATTR_SET_SUPPORTED(vap, va_access_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		a.set |= APFS_WA_MTIME;
		a.mtime = apfs_timespec_ns(vap->va_modify_time);
		VATTR_SET_SUPPORTED(vap, va_modify_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_change_time)) {
		a.set |= APFS_WA_CTIME;
		a.ctime = apfs_timespec_ns(vap->va_change_time);
		VATTR_SET_SUPPORTED(vap, va_change_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_create_time)) {
		a.set |= APFS_WA_CRTIME;
		a.crtime = apfs_timespec_ns(vap->va_create_time);
		VATTR_SET_SUPPORTED(vap, va_create_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {
		a.set |= APFS_WA_PCLASS;
		a.pclass = vap->va_dataprotect_class;
		VATTR_SET_SUPPORTED(vap, va_dataprotect_class);
	}
	/* kept nowhere, as on APFS */
	VATTR_SET_SUPPORTED(vap, va_backup_time);
	VATTR_SET_SUPPORTED(vap, va_encoding);
	if (a.set == 0) {
		return 0;
	}
	apfs_lock_exclusive(am);
	error = apfs_w_setattr(w, an->an_id, &a);
	if (error == 0 && track) {
		uint32_t doc;

		error = apfs_w_docid(w, an->an_id, &doc);
	}
	if (error == 0) {
		apfs_node_refresh(am, an);
		apfs_wdirty(am, true);
	}
	apfs_unlock_exclusive(am);
	return error;
}

static int
apfs_vnop_setxattr(struct vnop_setxattr_args *ap)
{
	struct apfs_node *an = APFS_VP(ap->a_vp);
	struct apfs_mount *am = an->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);
	size_t len = ap->a_uio != NULL ? (size_t)uio_resid(ap->a_uio) : 0;
	uint32_t flags = ((ap->a_options & XATTR_CREATE) ? APFS_WX_CREATE : 0) |
	    ((ap->a_options & XATTR_REPLACE) ? APFS_WX_REPLACE : 0);
	void *data;
	int error;

	if (w == NULL) {
		return EROFS;
	}
	if (ap->a_name == NULL) {
		return EINVAL;
	}
	if (len > APFS_XATTR_MAX) {
		return E2BIG;
	}
	data = kalloc_data(len ? len : 1, Z_WAITOK);
	if (data == NULL) {
		return ENOMEM;
	}
	error = len > 0 ? uiomove(data, (int)len, ap->a_uio) : 0;
	if (error == 0) {
		error = APFS_WCALL(am, an, NULL, apfs_w_setxattr(w, an->an_id, ap->a_name, data, len, flags));
	}
	kfree_data(data, len ? len : 1);
	return error;
}

static int
apfs_vnop_removexattr(struct vnop_removexattr_args *ap)
{
	struct apfs_node *an = APFS_VP(ap->a_vp);
	struct apfs_mount *am = an->an_mount;
	struct apfs_wtx *w = apfs_wtx_of(am);

	if (w == NULL) {
		return EROFS;
	}
	if (ap->a_name == NULL) {
		return EINVAL;
	}
	return APFS_WCALL(am, an, NULL, apfs_w_rmxattr(w, an->an_id, ap->a_name));
}

/* ------------------------------------------------------------------ the vnode's life */

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
	case _PC_NO_TRUNC:
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		return 0;
	case _PC_CASE_SENSITIVE:
		*ap->a_retval = (APFS_VP_MOUNT(ap->a_vp)->am_vol.incompat &
		    APFS_INCOMPAT_CASE_INSENSITIVE) ? 0 : 1;
		return 0;
	case _PC_XATTR_SIZE_BITS:
		*ap->a_retval = 32;
		return 0;
	case _PC_FILESIZEBITS:
		*ap->a_retval = 64;
		return 0;
	default:
		return EINVAL;
	}
}

static int
apfs_vnop_open(struct vnop_open_args *ap)
{
	if ((ap->a_mode & (FWRITE | O_TRUNC)) && apfs_wtx_of(APFS_VP_MOUNT(ap->a_vp)) == NULL) {
		printf("apfs: open ino %llu for write refused: no write transaction (EROFS)\n",
		    APFS_VP(ap->a_vp)->an_id);
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

/* The last use of a vnode: an object unlinked while it was open goes now. */
static int
apfs_vnop_inactive(struct vnop_inactive_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);

	if (an == NULL) {
		return 0;
	}
	if (an->an_orphan) {
		struct apfs_mount *am = an->an_mount;

		if (vnode_isreg(vp)) {
			ubc_setsize(vp, 0);
		}
		apfs_lock_exclusive(am);
		an->an_orphan = false;
		if (apfs_wtx_of(am) != NULL && apfs_w_reap(apfs_wtx_of(am), an->an_id) == 0) {
			apfs_wdirty(am, true);
		}
		apfs_unlock_exclusive(am);
		vnode_recycle(vp);
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

#include "apfs_vnops_dir.inc"

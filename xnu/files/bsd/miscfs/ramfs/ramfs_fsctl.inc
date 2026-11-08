/*
 * FSIOC_CAS_BSDFLAGS, the generic compare-and-swap of a file's BSD flags: vfs_syscalls.c checks the
 * caller's right to change the flags and leaves the swap to the file system, as HFS and APFS do it.
 * revisiond sets UF_TRACKED this way on every item FileProvider's LocalStorage extension creates (see
 * ramfs_track()); without an answer creating the "Downloads" folder fails with ENOTTY.
 *
 * The other fsctls answered are APFS's, because this volume reports itself as apfs and iOS 26 code
 * chooses its paths from that:
 *   - "mark purgeable" ('J' 68 with a uint64_t of flags), which PosterBoard sends for its data store under
 *     /var/mobile (flags 0 to unmark); without an answer it creates no poster store and the wallpaper
 *     stays black. Nothing here is ever purged, so the mark is accepted and kept nowhere.
 *   - the FileProvider sync-root calls ('J' 72 set and get, 'J' 74 for the nearest sync-root ancestor).
 *     FileProvider treats a local, writable apfs volume as able to host FPFS domains, so the
 *     LocalStorage domain behind "On My iPhone" marks its "File Provider Storage" directory as a sync
 *     root and checks every item against it; without an answer its root item does not exist and Safari
 *     downloads have no destination. The mark is saved with the node.
 * Everything else is ENOTTY.
 */
#include <sys/fsctl.h>

#define RAMFS_APFSIOC_MARK_PURGEABLE   _IOWR('J', 68, uint64_t)
#define RAMFS_APFSIOC_SET_SYNCROOT     _IOW('J', 72, uint32_t)
#define RAMFS_APFSIOC_GET_SYNCROOT     _IOR('J', 72, uint32_t)
#define RAMFS_APFSIOC_PARENT_SYNCROOT  _IOR('J', 74, uint64_t)

/* The number of the nearest proper ancestor of vp that is a sync root, 0 when there is none. */
static uint64_t
ramfs_parent_syncroot(vnode_t vp)
{
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	struct ramfs_node *dir = NULL;
	vnode_t parent_vp = NULLVP;
	uint64_t found = 0;

	if (node->rn_type != VDIR) {
		/* only directories keep their parent; a file's is the one it was reached through */
		parent_vp = vnode_getparent(vp);
		if (parent_vp == NULLVP) {
			return 0;
		}
		dir = RAMFS_NODE(parent_vp);
	}
	lck_mtx_lock(&rm->rm_lock);
	if (dir == NULL) {
		dir = node->rn_parent;
	}
	for (struct ramfs_node *at = dir; at != NULL; at = at->rn_parent) {
		if (at->rn_iflags & RAMFS_IF_SYNCROOT) {
			found = at->rn_ino;
			break;
		}
		if (at->rn_parent == at) {
			break;                  /* the root is its own parent */
		}
	}
	lck_mtx_unlock(&rm->rm_lock);
	if (parent_vp != NULLVP) {
		vnode_put(parent_vp);
	}
	return found;
}

static int
ramfs_syncroot_ioctl(struct vnop_ioctl_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;

	switch (ap->a_command) {
	case RAMFS_APFSIOC_SET_SYNCROOT:
		if (node->rn_type != VDIR) {
			return ENOTDIR;
		}
		lck_mtx_lock(&rm->rm_lock);
		if (*(uint32_t *)ap->a_data != 0) {
			node->rn_iflags |= RAMFS_IF_SYNCROOT;
		} else {
			node->rn_iflags &= ~RAMFS_IF_SYNCROOT;
		}
		ramfs_touch(node, false);
		lck_mtx_unlock(&rm->rm_lock);
		return 0;
	case RAMFS_APFSIOC_GET_SYNCROOT:
		*(uint32_t *)ap->a_data = (node->rn_iflags & RAMFS_IF_SYNCROOT) != 0;
		return 0;
	case RAMFS_APFSIOC_PARENT_SYNCROOT:
		*(uint64_t *)ap->a_data = ramfs_parent_syncroot(ap->a_vp);
		return 0;
	default:
		return ENOTTY;
	}
}

/* actual_flags reports the flags found; new_flags replaces them only when those equal expected_flags. */
static int
ramfs_cas_bsdflags(struct ramfs_node *node, struct fsioc_cas_bsdflags *cas)
{
	struct ramfs_mount *rm = node->rn_mount;

	lck_mtx_lock(&rm->rm_lock);
	cas->actual_flags = node->rn_bsdflags;
	if (cas->actual_flags == cas->expected_flags) {
		node->rn_bsdflags = cas->new_flags;
		ramfs_track(node);
		ramfs_touch(node, false);
	}
	lck_mtx_unlock(&rm->rm_lock);
	return 0;
}

static int
ramfs_ioctl(struct vnop_ioctl_args *ap)
{
	if (ap->a_command == FSIOC_CAS_BSDFLAGS) {
		return ramfs_cas_bsdflags(RAMFS_NODE(ap->a_vp), (struct fsioc_cas_bsdflags *)ap->a_data);
	}
	if (ap->a_command == F_FULLFSYNC || ap->a_command == 85 /* F_BARRIERFSYNC */) {
		struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
		int error = ramfs_sync_mapping(ap->a_vp, node);

		if (error) {
			return error;
		}
		return ramfs_disk_sync(node->rn_mount);
	}
	if (ap->a_command == RAMFS_APFSIOC_MARK_PURGEABLE) {
		return 0;
	}
	return ramfs_syncroot_ioctl(ap);
}

static int
ramfs_fsync(struct vnop_fsync_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;
	int error = ramfs_sync_mapping(ap->a_vp, node);

	if (error) {
		return error;
	}
	if (rm != NULL && node->rn_ino == RAMFS_ROOT_INO && !rm->rm_seed_done) {
		ramfs_disk_mark_seed_done(rm);
	}
	return ramfs_disk_sync(rm);
}

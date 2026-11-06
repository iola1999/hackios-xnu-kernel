/*
 * The one fsctl answered: APFS's "mark purgeable" ('J' 68 with a uint64_t of flags), which iOS 26's
 * PosterBoard sends for its data store under /var/mobile (ffsctl, flags 0 to unmark) and without
 * which it creates no poster store and the wallpaper stays black. Nothing here is ever purged, so
 * the mark is accepted and kept nowhere. Everything else is ENOTTY.
 */
#define RAMFS_APFSIOC_MARK_PURGEABLE _IOWR('J', 68, uint64_t)

static int
ramfs_ioctl(struct vnop_ioctl_args *ap)
{
	if (ap->a_command == F_FULLFSYNC || ap->a_command == 85 /* F_BARRIERFSYNC */) {
		struct ramfs_node *node = RAMFS_NODE(ap->a_vp);

		ramfs_sync_mapping(ap->a_vp, node);
		return ramfs_disk_sync(node->rn_mount);
	}
	return ap->a_command == RAMFS_APFSIOC_MARK_PURGEABLE ? 0 : ENOTTY;
}

static int
ramfs_fsync(struct vnop_fsync_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;

	ramfs_sync_mapping(ap->a_vp, node);
	if (rm != NULL && node->rn_ino == RAMFS_ROOT_INO && !rm->rm_seed_done) {
		ramfs_disk_mark_seed_done(rm);
	}
	return ramfs_disk_sync(rm);
}

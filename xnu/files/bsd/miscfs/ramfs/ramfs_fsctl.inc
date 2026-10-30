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
	return ap->a_command == RAMFS_APFSIOC_MARK_PURGEABLE ? 0 : ENOTTY;
}

/*
 * APFS VFS Write Support
 */
#pragma once

#define APFS_DATA_MAX (128 * 1024)

static int
apfs_dev_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	const char *in = buf;

	off += dc->dc_base;
	if ((off % dc->dc_devblock) != 0 || (len % dc->dc_devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		size_t n = len < APFS_DATA_MAX ? len : APFS_DATA_MAX;
		buf_t bp = buf_alloc(dc->dc_devvp);
		int error;

		if (bp == NULL) {
			return ENOMEM;
		}
		buf_reset(bp, B_WRITE);
		buf_setblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setlblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setcount(bp, (uint32_t)n);
		buf_setdataptr(bp, (uintptr_t)in);
		error = VNOP_STRATEGY(bp);
		if (error == 0) {
			error = (int)buf_biowait(bp);
		}
		if (error == 0 && (buf_resid(bp) != 0 || buf_count(bp) != n)) {
			error = EIO;
		}
		buf_free(bp);
		if (error) {
			return error;
		}
		in += n;
		off += n;
		len -= n;
	}
	return 0;
}

static inline void
apfs_mount_check_rw(mount_t mp, struct apfs_dev *dev)
{
	int rw_candidate = 0;
	(void)PE_parse_boot_argn("apfs_rw", &rw_candidate, sizeof(rw_candidate));
	if (rw_candidate != 1) {
		vfs_setflags(mp, MNT_RDONLY);
	}
	if (!(vfs_flags(mp) & MNT_RDONLY)) {
		dev->write = apfs_dev_write;
	}
}

static inline void
apfs_mount_set_rw_flags(mount_t mp, struct apfs_mount *am)
{
	if (vfs_flags(mp) & MNT_RDONLY) {
		vfs_setflags(mp, MNT_RDONLY | MNT_LOCAL);
	} else {
		vfs_setflags(mp, MNT_LOCAL);
		am->am_nx.dev.write = apfs_dev_write;
	}
}

/*
 * APFS VFS Mount Operations: free_mount, mount_common, mountroot, mount, unmount
 */
#pragma once

static void
apfs_free_mount(struct apfs_mount *am)
{
	if (am->am_synth != NULL) {
		kfree_data(am->am_synth, APFS_SYNTH_MAX * sizeof(*am->am_synth));
	}
	while (am->am_iobuf_kept > 0) {
		kfree_data(am->am_iobuf[--am->am_iobuf_kept], am->am_iobuf_size);
	}
	apfs_nx_close(&am->am_nx);
	if (am->am_procs != NULL) {
		kfree_data(am->am_procs, APFS_LOCK_PROCS * sizeof(am->am_procs[0]));
	}
	lck_spin_destroy(&am->am_iobuf_spin, &apfs_lck_grp);
	lck_spin_destroy(&am->am_procs_spin, &apfs_lck_grp);
	lck_rw_destroy(&am->am_rwlock, &apfs_lck_grp);
	lck_mtx_destroy(&am->am_lock, &apfs_lck_grp);
	kfree_type(struct apfs_mount, am);
}

/*
 * Which container of the device and which volume of it: containers lie back to back, each
 * saying in its superblock how big it is (nx_block_count blocks of nx_block_size), so the
 * N-th starts where the one before it ends -- the RAM disks of this port hold several (the
 * boot volume, a cryptex) and so may the phone's darwin partition.  volume < 0: the first
 * that has a root directory.
 */
static int
apfs_mount_common(mount_t mp, vnode_t devvp, vfs_context_t ctx, uint32_t container, int volume)
{
	struct apfs_mount *am;
	struct apfs_devctx *dc;
	struct apfs_dev dev;
	struct vfsstatfs *sp;
	uint32_t devblock = 0;
	uint64_t devblocks = 0, nx_size = 0;
	int error;

	vfs_context_t ioctl_ctx = (ctx != NULL) ? ctx : vfs_context_kernel();
	if (VNOP_IOCTL(devvp, DKIOCGETBLOCKSIZE, (caddr_t)&devblock, 0, ioctl_ctx) != 0 ||
	    devblock == 0 || (APFS_META_CHUNK % devblock) != 0) {
		return EINVAL;
	}
	(void)VNOP_IOCTL(devvp, DKIOCGETBLOCKCOUNT, (caddr_t)&devblocks, 0, ioctl_ctx);

	am = kalloc_type(struct apfs_mount, Z_WAITOK | Z_ZERO);
	if (am == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&am->am_lock, &apfs_lck_grp, LCK_ATTR_NULL);
	lck_rw_init(&am->am_rwlock, &apfs_lck_grp, LCK_ATTR_NULL);
	lck_spin_init(&am->am_iobuf_spin, &apfs_lck_grp, LCK_ATTR_NULL);
	lck_spin_init(&am->am_procs_spin, &apfs_lck_grp, LCK_ATTR_NULL);
	{
		uint32_t ubc = 1;

		(void)PE_parse_boot_argn("apfs_ubc_read", &ubc, sizeof(ubc));
		am->am_ubc_read = (ubc != 0);
	}
	am->am_procs = kalloc_data(APFS_LOCK_PROCS * sizeof(am->am_procs[0]), Z_WAITOK | Z_ZERO);
	am->am_mp = mp;
	am->am_devvp = devvp;
	am->am_devblock = devblock;

	dc = kalloc_type(struct apfs_devctx, Z_WAITOK | Z_ZERO);
	if (dc == NULL) {
		apfs_free_mount(am);
		return ENOMEM;
	}
	dc->dc_devvp = devvp;
	dc->dc_devblock = devblock;
	{
		uint8_t *sb = kalloc_data(APFS_META_CHUNK, Z_WAITOK);
		uint64_t devsize = devblocks * (uint64_t)devblock;
		uint32_t i;

		error = (sb == NULL) ? ENOMEM : 0;
		for (i = 0; error == 0; i++) {
			uint32_t bs;
			uint64_t bc;

			error = apfs_dev_read(dc, 0, sb, APFS_META_CHUNK);
			if (error == 0 && memcmp(sb + 32, "NXSB", 4) != 0) {
				error = EINVAL;         /* (no container here: as the root, the next filesystem's turn) */
			}
			if (error) {
				break;
			}
			bs = (uint32_t)sb[36] | (uint32_t)sb[37] << 8 | (uint32_t)sb[38] << 16 | (uint32_t)sb[39] << 24;
			bc = 0;
			for (int b = 7; b >= 0; b--) {
				bc = (bc << 8) | sb[40 + b];
			}
			nx_size = bc * bs;
			if (bs < 4096 || bc == 0 || (nx_size % devblock) != 0 ||
			    (devsize != 0 && dc->dc_base + nx_size > devsize)) {
				printf("apfs: container %u: %llu blocks of %u do not fit the device\n", i,
				    (unsigned long long)bc, bs);
				error = EINVAL;
				break;
			}
			if (i == container) {
				break;
			}
			dc->dc_base += nx_size;
		}
		if (sb != NULL) {
			kfree_data(sb, APFS_META_CHUNK);
		}
		if (error) {
			kfree_type(struct apfs_devctx, dc);
			apfs_free_mount(am);
			return error;
		}
	}

	bzero(&dev, sizeof(dev));
	dev.ctx = dc;
	dev.size = nx_size;
	dev.read = apfs_dev_read;
	dev.read_data = apfs_dev_read_data;
	apfs_mount_check_rw(mp, &dev);

	error = apfs_nx_open(&am->am_nx, &dev);
	if (error) {
		/*
		 * EINVAL means the magic was not there, so this is not an APFS container at
		 * all: as the root filesystem that lets vfs_mountroot() try the next one
		 * without a word.
		 */
		if (error != EINVAL) {
			printf("apfs: cannot read the container: %d\n", error);
		}
		kfree_type(struct apfs_devctx, dc);
		apfs_free_mount(am);
		return error;
	}

	/* apfs_nx_open() starts from a clean slate, so the lock is handed over only now */
	am->am_nx.unlock = apfs_lib_unlock;
	am->am_nx.relock = apfs_lib_relock;
	am->am_nx.lock_ctx = am;
	am->am_iobuf_size = APFS_IOBUF_DEFAULT;
	{
		uint32_t round = 0;

		if (PE_parse_boot_argn("apfs_round", &round, sizeof(round)) &&
		    round >= 64 * 1024 && round <= APFS_IOBUF_MAX && (round % (64 * 1024)) == 0) {
			am->am_iobuf_size = round;
		}
	}
	(void)PE_parse_boot_argn("apfs_meta_cluster", &am->am_nx.meta_cluster,
	    sizeof(am->am_nx.meta_cluster));
	/*
	 * The phone's flash and QEMU's virtio disk cost tens of microseconds a request; this
	 * port's other disks (md, the resident image; rsp, part of an image) answer with a
	 * memory copy.  Boot-arg apfs_cheap=0/1 says otherwise.
	 */
	{
		dev_t rdev = vnode_specrdev(devvp);
		uint32_t cheap;

		am->am_nx.cheap_requests = !annibale_ufs_dev(rdev) && !annibale_virtio_blk_dev(rdev);
		if (PE_parse_boot_argn("apfs_cheap", &cheap, sizeof(cheap))) {
			am->am_nx.cheap_requests = (cheap != 0);
		}
		printf("apfs: device %d,%d: requests to it %s\n", major(rdev), minor(rdev),
		    am->am_nx.cheap_requests ? "cost next to nothing (a disk in memory)" :
		    "are worth putting together");
	}
	apfs_pool_start();
	if (apfs_pool_threads > 0) {
		am->am_nx.parallel = apfs_parallel;
		am->am_nx.parallel_max = apfs_pool_threads + 1;
	}

	/* the first volume that has a root directory */
	{
		uint32_t i;
		bool got = false;

		/* with the lock, as every call into apfs_lib from now on (it may let go of it) */
		apfs_lock(am);
		for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS && !got; i++) {
			struct apfs_inode ino;

			if (am->am_nx.fs_oid[i] == 0 || (volume >= 0 && i != (uint32_t)volume)) {
				continue;
			}
			if (apfs_vol_open(&am->am_nx, i, &am->am_vol) != 0) {
				continue;
			}
			if (apfs_inode_get(&am->am_vol, APFS_ROOT_DIR_INO_NUM, &ino) == 0) {
				got = true;
			}
		}
		apfs_unlock(am);
		if (!got) {
			printf("apfs: no readable volume with a root directory\n");
			kfree_type(struct apfs_devctx, dc);
			apfs_free_mount(am);
			return EINVAL;
		}
	}
	am->am_root_id = APFS_ROOT_DIR_INO_NUM;

	vfs_setfsprivate(mp, am);
	vfs_setflags(mp, MNT_LOCAL);
	vfs_setlocklocal(mp);
	apfs_wmount_attach(am, dc);

	sp = vfs_statfs(mp);
	apfs_fill_statfs(am, sp);
	/* ID-based VFS lookups use fsid.val[0], so each mounted volume needs its own ID. */
	vfs_getnewfsid(mp);
	strlcpy(sp->f_fstypename, "apfs", sizeof(sp->f_fstypename));
	if (container == 1) {
		strlcpy(sp->f_mntfromname, "/dev/disk2s1", sizeof(sp->f_mntfromname));
	} else if (container == 2) {
		strlcpy(sp->f_mntfromname, "/dev/disk3s1", sizeof(sp->f_mntfromname));
	} else if (sp->f_mntfromname[0] == '\0') {
		strlcpy(sp->f_mntfromname, "/dev/disk1s1", sizeof(sp->f_mntfromname));
	}

	apfs_the_mount = am;
	am->am_rdev = vnode_specrdev(devvp);
	lck_mtx_lock(&apfs_pool_lock);
	am->am_next = apfs_mounts;
	apfs_mounts = am;
	lck_mtx_unlock(&apfs_pool_lock);
	printf("apfs: volume %u \"%s\" of container %u (at %llu MB): %llu files, %llu directories, %llu blocks of %u used\n",
	    am->am_vol.index, am->am_vol.name, container, (unsigned long long)(dc->dc_base >> 20), (unsigned long long)am->am_vol.num_files,
	    (unsigned long long)am->am_vol.num_dirs,
	    (unsigned long long)am->am_vol.alloc_count, am->am_nx.block_size);
	return 0;
}

/*
 * As the root filesystem: EINVAL lets vfs_mountroot() try the next quietly.  Boot-args
 * apfs_root_c=N and apfs_root_v=M say which container of the device and which volume of it
 * (default: the first container, its first volume with a root directory).
 */
int
apfs_mountroot(mount_t mp, vnode_t devvp, vfs_context_t ctx)
{
	uint32_t c = 0;
	int v = -1;

	(void)PE_parse_boot_argn("apfs_root_c", &c, sizeof(c));
	(void)PE_parse_boot_argn("apfs_root_v", &v, sizeof(v));
	return apfs_mount_common(mp, devvp, ctx, c, v);
}

/*
 * mount(2), and kernel_mount().  The mount data is the path of the block device holding
 * the container, as a string.  VFS does not resolve it for us the way it does for a
 * filesystem marked VFC_VFSLOCALARGS -- see the note in vfs_conf.c for why this one is
 * not -- so it is looked up and opened here, and closed again in apfs_unmount().
 */
static int
apfs_mount(mount_t mp, vnode_t devvp, user_addr_t data, vfs_context_t ctx)
{
	char *path;
	vnode_t vp = NULLVP;
	size_t len = 0;
	int mode = FREAD;
	uint32_t container = 0;
	int volume = -1;
	int error;

	if (vfs_isupdate(mp)) {
		/* nothing about a mounted volume can change, least of all to read-write */
		return vfs_iswriteupgrade(mp) ? EROFS : 0;
	}
	if (devvp != NULLVP) {
		/* mounted as the root: VFS found the device and owns the vnode */
		return apfs_mountroot(mp, devvp, ctx);
	}
	if (data == USER_ADDR_NULL) {
		return EINVAL;
	}
	path = kalloc_data(MAXPATHLEN, Z_WAITOK);
	if (path == NULL) {
		return ENOMEM;
	}
	if (vfs_iskernelmount(mp)) {
		/* kernel_mount() hands the pointer through untouched */
		error = (strlcpy(path, CAST_DOWN(char *, data), MAXPATHLEN) < MAXPATHLEN)
		    ? 0 : ENAMETOOLONG;
	} else {
		error = copyinstr(data, path, MAXPATHLEN, &len);
	}
	/* "DEVICE[,c=N][,v=M]": the container (from 0) and the volume (by index) */
	if (error == 0) {
		char *opt = strchr(path, ',');

		while (opt != NULL) {
			char *next = strchr(opt + 1, ',');

			*opt = '\0';
			if (strncmp(opt + 1, "c=", 2) == 0) {
				container = (uint32_t)strtoul(opt + 3, NULL, 10);
			} else if (strncmp(opt + 1, "v=", 2) == 0) {
				volume = (int)strtoul(opt + 3, NULL, 10);
			} else {
				error = EINVAL;
			}
			opt = next;
		}
	}
	if (error == 0) {
		error = vnode_lookup(path, 0, &vp, ctx);
	}
	if (error == 0 && !vnode_isblk(vp)) {
		error = ENOTBLK;
	}
	if (error == 0) {
		bool shared = false;

		mode = vfs_isrdonly(mp) ? FREAD : FREAD | FWRITE;
		error = VNOP_OPEN(vp, mode, ctx);
		if (error == EBUSY && apfs_device_held(vnode_specrdev(vp))) {
			/*
			 * Mounted already -- by us, another container of it (the root's RAM disk and the
			 * cryptex after the boot volume): read through it as it is open, not opened
			 * again.  Anything else mounted on a device stays EBUSY.
			 */
			shared = true;
			error = 0;
		}
		if (error == 0) {
			/*
			 * The iocount from the lookup is kept for as long as the volume is
			 * mounted; apfs_unmount() closes and drops it.
			 */
			error = apfs_mount_common(mp, vp, ctx, container, volume);
			if (error == 0) {
				APFS_MP(mp)->am_owns_devvp = true;
				APFS_MP(mp)->am_shares_devvp = shared;
				APFS_MP(mp)->am_open_mode = mode;
				/* what mount(8) and df show as where it came from */
				if (container == 1) {
					strlcpy(vfs_statfs(mp)->f_mntfromname, "/dev/disk2s1", MAXPATHLEN);
				} else if (container == 2) {
					strlcpy(vfs_statfs(mp)->f_mntfromname, "/dev/disk3s1", MAXPATHLEN);
				} else if (container == 0 && (volume == -1 || volume == 0)) {
					strlcpy(vfs_statfs(mp)->f_mntfromname, "/dev/disk1s1", MAXPATHLEN);
				} else if (vfs_statfs(mp)->f_mntfromname[0] == '\0' || container != 0 || volume >= 0) {
					char opts[40] = "";

					if (container != 0)
						snprintf(opts, sizeof(opts), ",c=%u", container);
					if (volume >= 0)
						snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), ",v=%d", volume);
					snprintf(vfs_statfs(mp)->f_mntfromname, MAXPATHLEN, "%s%s", path, opts);
				}
			} else if (!shared) {
				(void)VNOP_CLOSE(vp, mode, ctx);
			}
		}
		if (error != 0)
			vnode_put(vp);
	} else if (vp != NULLVP) {
		vnode_put(vp);
	}
	kfree_data(path, MAXPATHLEN);
	return error;
}

static int
apfs_unmount(mount_t mp, int mntflags, vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);
	int error = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE : 0);

	if (error) {
		return error;
	}
	error = apfs_wmount_detach(am);
	if (error) {
		printf("apfs: unmount of \"%s\": the last commit failed (%d)\n", am->am_vol.name, error);
	}
	if (apfs_the_mount == am) {
		apfs_the_mount = NULL;
	}
	lck_mtx_lock(&apfs_pool_lock);
	for (struct apfs_mount **pp = &apfs_mounts; *pp != NULL; pp = &(*pp)->am_next) {
		if (*pp == am) {
			*pp = am->am_next;
			break;
		}
	}
	lck_mtx_unlock(&apfs_pool_lock);
	vfs_setfsprivate(mp, NULL);
	if (am->am_owns_devvp && am->am_devvp != NULLVP) {
		if (!am->am_shares_devvp) {
			(void)VNOP_CLOSE(am->am_devvp, am->am_open_mode, ctx);
		}
		vnode_put(am->am_devvp);
	}
	apfs_free_mount(am);
	return 0;
}

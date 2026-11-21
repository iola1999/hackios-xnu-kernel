/*
 * Grafts: a volume of another container shown in a directory of a mounted volume, the way iOS
 * grafts each cryptex image into the Preboot volume (graftdmg, bsd/sys/fsctl.h).  On an iPhone
 * the OS cryptex's files are the Preboot volume's: statfs and the file system ID they report are
 * Preboot's, their inode numbers come from a range the host set aside for the graft, the graft's
 * root has the inode number of the directory it is shown in, and FSIOC_GET_GRAFT_INFO tells a
 * file in a graft from the rest.  LaunchServices depends on it: lsd drops at every boot the
 * bundles whose volume is none of the system, data, Preboot and user volumes ("Marking container
 * at ... for removal", _LSServerMain), and registers a bundle as a trusted cryptex's only when
 * the fsctl says it is in a graft (_LSServer_NodeIsOnTrustedCryptex).
 *
 * This port has no cryptex images to graft; init grafts a container of a block device (the RAM
 * disk or darwin) with APFS_IOC_GRAFT on the directory, naming it the way mount(2)'s data does.
 * A grafted volume is read only.
 */
#pragma once

#include <sys/fsctl.h>

/* This port's fsctl, on the host directory: graft the volume "DEVICE[,c=N][,v=M]" onto it. */
struct apfs_graft_req {
	char spec[128];
};
#define APFS_IOC_GRAFT          _IOW('J', 250, struct apfs_graft_req)

/*
 * The inode numbers of a host's N-th graft start at (N + 1) << 60, above every APFS object
 * identifier (60 bits), so that none can be one of the host's.  The range is as long as the
 * grafted volume's identifiers go, within the 56 bits gi_inum_len has.
 */
#define APFS_GRAFT_INUM_SHIFT   60
#define APFS_GRAFT_INUM_MAX     ((1ULL << 56) - 1)

uint64_t
apfs_graft_ino(const struct apfs_mount *am, uint64_t id)
{
	const struct apfs_graft *g = am->am_graft;

	if (g == NULL) {
		return id;
	}
	if (id == APFS_ROOT_DIR_INO_NUM) {
		return g->g_dir;
	}
	if (id == APFS_ROOT_DIR_PARENT) {
		return g->g_parent;
	}
	return g->g_inum_base + id;
}

/*
 * Where apfs_node_vnode finds object *id of am: the host directory a graft is shown in is the
 * graft's root, and the parent of a graft's root is that directory's parent in the host.
 */
static struct apfs_mount *
apfs_graft_route(struct apfs_mount *am, uint64_t *id)
{
	if (am->am_graft != NULL) {
		if (*id == APFS_ROOT_DIR_PARENT) {
			*id = am->am_graft->g_parent;
			return am->am_graft->g_host;
		}
		return am;
	}
	for (struct apfs_graft *g = os_atomic_load(&am->am_grafts, acquire); g != NULL; g = g->g_next) {
		if (g->g_dir == *id) {
			*id = APFS_ROOT_DIR_INO_NUM;
			return g->g_am;
		}
	}
	return am;
}

/* VFS_VGET of the host's mount: an inode number in a graft's range is that graft's object. */
static struct apfs_mount *
apfs_graft_vget(struct apfs_mount *am, uint64_t ino, uint64_t *id)
{
	*id = ino;
	for (struct apfs_graft *g = os_atomic_load(&am->am_grafts, acquire); g != NULL; g = g->g_next) {
		if (ino >= g->g_inum_base && ino - g->g_inum_base < g->g_inum_len) {
			*id = ino - g->g_inum_base;
			return g->g_am;
		}
	}
	return am;
}

static int
apfs_graft_attach(vnode_t dvp, struct apfs_graft_req *req, vfs_context_t ctx)
{
	struct apfs_mount *host = APFS_VP_MOUNT(dvp), *am = NULL;
	struct apfs_node *dn = APFS_VP(dvp);
	struct apfs_devctx *dc;
	struct apfs_graft *g, *older;
	char spec[sizeof(req->spec)];
	uint32_t container = 0, index = 0;
	int volume = -1, error = 0;
	vnode_t devvp = NULLVP;
	bool shared = false;

	if (!vfs_context_issuser(ctx)) {
		return EPERM;
	}
	if (!vnode_isdir(dvp) || host->am_graft != NULL) {
		return EINVAL;          /* a directory of a volume that is not itself grafted */
	}
	if (strnlen(req->spec, sizeof(req->spec)) == sizeof(req->spec)) {
		return ENAMETOOLONG;
	}
	strlcpy(spec, req->spec, sizeof(spec));
	error = apfs_parse_spec(spec, &container, &volume);
	if (error == 0) {
		error = apfs_open_device(spec, FREAD, ctx, &devvp, &shared);
	}
	if (error != 0) {
		return error;
	}
	error = apfs_volume_open(host->am_mp, devvp, ctx, container, volume, true, &am, &dc);
	if (error != 0) {
		if (!shared) {
			(void)VNOP_CLOSE(devvp, FREAD, ctx);
		}
		vnode_put(devvp);
		return error;
	}
	am->am_owns_devvp = true;
	am->am_shares_devvp = shared;
	am->am_open_mode = FREAD;
	am->am_rdev = vnode_specrdev(devvp);
	g = kalloc_type(struct apfs_graft, Z_WAITOK | Z_ZERO);
	if (g == NULL) {
		apfs_volume_close(am, ctx);
		return ENOMEM;
	}
	g->g_am = am;
	g->g_host = host;
	g->g_dir = dn->an_id;
	g->g_parent = dn->an_parent;
	g->g_inum_len = am->am_vol.next_obj_id != 0 && am->am_vol.next_obj_id < APFS_GRAFT_INUM_MAX ?
	    am->am_vol.next_obj_id : APFS_GRAFT_INUM_MAX;
	am->am_graft = g;

	/* apfs_pool_lock: one graft at a time, and the device in apfs_mounts for the next to share */
	lck_mtx_lock(&apfs_pool_lock);
	older = host->am_grafts;
	for (struct apfs_graft *o = older; o != NULL; o = o->g_next, index++) {
		if (o->g_dir == g->g_dir) {
			error = EEXIST;
		}
	}
	if (error == 0) {
		g->g_inum_base = (uint64_t)(index + 1) << APFS_GRAFT_INUM_SHIFT;
		g->g_next = older;
		am->am_next = apfs_mounts;
		apfs_mounts = am;
		os_atomic_store(&host->am_grafts, g, release);
	}
	lck_mtx_unlock(&apfs_pool_lock);
	if (error != 0) {
		apfs_volume_close(am, ctx);
		kfree_type(struct apfs_graft, g);
		return error;
	}
	/* names that led to the directory itself lead to the graft's root from now on */
	cache_purge(dvp);
	printf("apfs: volume \"%s\" of container %u of %s grafted onto directory %llu of \"%s\": "
	    "inode numbers %#llx and %llu after\n", am->am_vol.name, container, spec,
	    (unsigned long long)g->g_dir, host->am_vol.name, (unsigned long long)g->g_inum_base,
	    (unsigned long long)g->g_inum_len);
	return 0;
}

/* FSIOC_GET_GRAFT_INFO: whether the object is in a graft, and the host's grafts (newest first). */
static int
apfs_graft_info(struct apfs_mount *am, fsioc_get_graft_info_t *ggi)
{
	struct apfs_mount *host = am->am_graft != NULL ? am->am_graft->g_host : am;
	uint16_t count = 0, put = 0;
	int error = 0;

	ggi->ggi_is_in_graft = (am->am_graft != NULL);
	ggi->ggi_graft_index = 0;
	for (struct apfs_graft *g = os_atomic_load(&host->am_grafts, acquire); g != NULL; g = g->g_next) {
		fsioc_graft_info_t gi = { 0 };

		count++;
		if (ggi->ggi_buffer == 0 || put >= ggi->ggi_count || error != 0) {
			continue;
		}
		gi.gi_graft_dir = g->g_dir;
		gi.gi_inum_base = g->g_inum_base;
		gi.gi_inum_len = g->g_inum_len;
		error = copyout(&gi, ggi->ggi_buffer + put * sizeof(gi), sizeof(gi));
		if (g->g_am == am) {
			ggi->ggi_graft_index = put;
		}
		put++;
	}
	ggi->ggi_count = ggi->ggi_buffer == 0 ? count : put;
	return error;
}

int
apfs_graft_ioctl(vnode_t vp, u_long command, caddr_t data, vfs_context_t ctx)
{
	switch (command) {
	case APFS_IOC_GRAFT:
		return apfs_graft_attach(vp, (struct apfs_graft_req *)data, ctx);
	case FSIOC_GET_GRAFT_INFO:
		return apfs_graft_info(APFS_VP_MOUNT(vp), (fsioc_get_graft_info_t *)data);
	default:
		return ENOTTY;
	}
}

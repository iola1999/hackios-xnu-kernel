/*
 * A writable mount: the device's write and flush, and when the open transaction is committed.
 *
 * Changes collect in one transaction (apfs_write.c, apfs_wtx.h) and are committed
 *   - a second after the first change (wm_tick), once the files written since the last commit
 *     have sent their dirty pages to the device;
 *   - at once when the transaction holds more than APFS_WDIRTY_MAX metadata blocks;
 *   - by fsync(2), F_FULLFSYNC, F_BARRIERFSYNC, IO_SYNC writes, sync(2) and unmount.
 * A commit ends with flush, container superblock, flush (apfs_wtx_commit), so every commit is
 * a full sync of what it covers.
 *
 * Data blocks are given to a file when its pages go to the device (vnop_blockmap), and a
 * commit waits for every data write mapped before it (wm_map, then wm_io) before it writes the
 * checkpoint.  So a checkpoint never names a block whose contents are still on their way, and
 * the engine frees data blocks only at the commit that took them away from their file
 * (wsp_free_later), by which time no write to them is outstanding: a block cannot be written
 * for its old file and its new one at once.
 */
#pragma once

#include <sys/ubc.h>
#include "apfs_write.h"

static void apfs_fill_statfs(struct apfs_mount *am, struct vfsstatfs *sp);

#define APFS_WMAP_MAX     256           /* mappings for writing whose buffers are not yet sent */
#define APFS_WPUSH_MAX    512           /* files written since the last commit */
#define APFS_WDIRTY_MAX   4096          /* metadata blocks in one transaction */
#define APFS_WCOMMIT_MS   1000
#define APFS_WDRAIN_MS    2000          /* the longest a commit waits for data writes */

struct apfs_wmount {
	struct apfs_wtx   *wm_wtx;
	thread_call_t      wm_tick;
	bool               wm_armed;            /* wm_tick is entered; under am_lock */
	bool               wm_closing;          /* unmount: wm_tick is not entered again */
	uint64_t           wm_base;             /* the container's first byte on the device */
	uint32_t           wm_devblock;
	uint32_t           wm_bs;
	lck_spin_t         wm_spin;             /* the map, wm_io and wm_waiting */
	struct {
		uint64_t   addr, end;           /* container blocks [addr, end) */
		uint64_t   left;                /* blocks of it not yet sent */
	}                  wm_map[APFS_WMAP_MAX];
	uint32_t           wm_nmap;
	uint32_t           wm_io;               /* data writes sent and not yet done */
	bool               wm_waiting;
	uint64_t           wm_push[APFS_WPUSH_MAX];     /* object ids; under am_lock */
	uint32_t           wm_npush;
	bool               wm_push_all;         /* more files than wm_push holds */
	uint64_t           wm_commits, wm_errors, wm_slow_drains;
};

/* The writable data volume, for the container's space (apfs_container_space); apfs_pool_lock. */
static struct apfs_mount *apfs_data_mount;

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
		vnode_startwrite(dc->dc_devvp);
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

/* What the device has been sent is on the medium: the commit's barriers. */
static int
apfs_dev_flush(void *ctx)
{
	struct apfs_devctx *dc = ctx;
	int error = VNOP_IOCTL(dc->dc_devvp, DKIOCSYNCHRONIZECACHE, NULL, FWRITE, vfs_context_kernel());

	return (error == ENOTTY || error == ENOTSUP) ? 0 : error;
}

/* Before the container is opened: a mount without MNT_RDONLY writes, unless boot-arg apfs_rw=0. */
static void
apfs_mount_check_rw(mount_t mp, struct apfs_dev *dev)
{
	uint32_t rw = 1;

	(void)PE_parse_boot_argn("apfs_rw", &rw, sizeof(rw));
	if (rw == 0) {
		vfs_setflags(mp, MNT_RDONLY);
	}
	if (!vfs_isrdonly(mp)) {
		dev->write = apfs_dev_write;
		dev->flush = apfs_dev_flush;
	}
}

/* ------------------------------------------------------------------ data writes */

struct apfs_wtx *
apfs_wtx_of(struct apfs_mount *am)
{
	return am->am_w != NULL ? am->am_w->wm_wtx : NULL;
}

/* The device block vnop_blockmap gives for container block `addr` and byte `in` of it. */
daddr64_t
apfs_wdevblock(struct apfs_mount *am, uint64_t addr, uint64_t in)
{
	struct apfs_wmount *wm = am->am_w;

	return (daddr64_t)((addr * wm->wm_bs + in + wm->wm_base) / wm->wm_devblock);
}

/* A mapping vnop_blockmap gave for writing; with the exclusive lock held. */
void
apfs_wmap_add(struct apfs_mount *am, uint64_t addr, uint64_t blocks)
{
	struct apfs_wmount *wm = am->am_w;
	uint64_t deadline;

	clock_interval_to_deadline(APFS_WDRAIN_MS, NSEC_PER_MSEC, &deadline);
	lck_spin_lock(&wm->wm_spin);
	while (wm->wm_nmap == APFS_WMAP_MAX) {
		if (lck_spin_sleep_deadline(&wm->wm_spin, LCK_SLEEP_DEFAULT, (event_t)&wm->wm_nmap,
		    THREAD_UNINT, deadline) == THREAD_TIMED_OUT && wm->wm_nmap == APFS_WMAP_MAX) {
			wm->wm_map[0] = wm->wm_map[--wm->wm_nmap];      /* never sent: forgotten */
			wm->wm_slow_drains++;
		}
	}
	wm->wm_map[wm->wm_nmap].addr = addr;
	wm->wm_map[wm->wm_nmap].end = addr + blocks;
	wm->wm_map[wm->wm_nmap].left = blocks;
	wm->wm_nmap++;
	lck_spin_unlock(&wm->wm_spin);
}

/* A data write done: its count goes, then the cluster layer's own completion, if any. */
static void
apfs_wio_done(buf_t bp, void *arg)
{
	void (*next)(buf_t, void *) = (void (*)(buf_t, void *))arg;
	void *next_arg = buf_fsprivate(bp);
	struct apfs_wmount *wm = APFS_VP_MOUNT(buf_vnode(bp))->am_w;

	buf_setfsprivate(bp, NULL);
	lck_spin_lock(&wm->wm_spin);
	if (--wm->wm_io == 0 && wm->wm_waiting) {
		wakeup((caddr_t)&wm->wm_io);
	}
	lck_spin_unlock(&wm->wm_spin);
	if (next != NULL) {
		next(bp, next_arg);
	}
}

/*
 * vnop_strategy, a data write on its way to the device: what it covers of the mappings is
 * sent, and the write counts until it is done.
 */
void
apfs_wio_start(struct apfs_mount *am, buf_t bp)
{
	struct apfs_wmount *wm = am->am_w;
	uint64_t at = (uint64_t)buf_blkno(bp) * wm->wm_devblock;
	uint64_t a0 = at >= wm->wm_base ? (at - wm->wm_base) / wm->wm_bs : 0;
	uint64_t a1 = at >= wm->wm_base ? (at - wm->wm_base + buf_count(bp) + wm->wm_bs - 1) / wm->wm_bs : 0;
	void (*next)(buf_t, void *) = NULL;
	void *next_arg = NULL;

	lck_spin_lock(&wm->wm_spin);
	for (uint32_t i = 0; i < wm->wm_nmap;) {
		uint64_t lo = MAX(a0, wm->wm_map[i].addr), hi = MIN(a1, wm->wm_map[i].end);

		if (lo < hi) {
			wm->wm_map[i].left -= MIN(wm->wm_map[i].left, hi - lo);
		}
		if (wm->wm_map[i].left == 0) {
			wm->wm_map[i] = wm->wm_map[--wm->wm_nmap];
			wakeup((caddr_t)&wm->wm_nmap);
			continue;
		}
		i++;
	}
	wm->wm_io++;
	if (wm->wm_nmap == 0 && wm->wm_waiting) {
		wakeup((caddr_t)&wm->wm_io);
	}
	lck_spin_unlock(&wm->wm_spin);
	buf_setfilter(bp, apfs_wio_done, NULL, &next, &next_arg);
	buf_setfilter(bp, apfs_wio_done, (void *)next, NULL, NULL);
	buf_setfsprivate(bp, next_arg);
}

/* Before a checkpoint, with the exclusive lock held: every data write mapped so far is done. */
static void
apfs_wio_drain(struct apfs_mount *am)
{
	struct apfs_wmount *wm = am->am_w;
	uint64_t deadline;

	clock_interval_to_deadline(APFS_WDRAIN_MS, NSEC_PER_MSEC, &deadline);
	lck_spin_lock(&wm->wm_spin);
	while (wm->wm_nmap > 0 || wm->wm_io > 0) {
		wm->wm_waiting = true;
		if (lck_spin_sleep_deadline(&wm->wm_spin, LCK_SLEEP_DEFAULT, (event_t)&wm->wm_io,
		    THREAD_UNINT, deadline) == THREAD_TIMED_OUT) {
			/* a mapping that was never sent (the cluster layer gave up on it) */
			printf("apfs: commit: %u mappings and %u data writes still out after %u ms\n",
			    wm->wm_nmap, wm->wm_io, APFS_WDRAIN_MS);
			wm->wm_nmap = 0;
			wm->wm_slow_drains++;
			break;
		}
	}
	wm->wm_waiting = false;
	lck_spin_unlock(&wm->wm_spin);
}

/* ------------------------------------------------------------------ commits */

/* With the exclusive lock held. */
int
apfs_wcommit_locked(struct apfs_mount *am)
{
	struct apfs_wmount *wm = am->am_w;
	int error;

	wm->wm_armed = false;
	if (!apfs_wtx_dirty(wm->wm_wtx)) {
		return apfs_wtx_broken(wm->wm_wtx) ? EIO : 0;
	}
	apfs_wio_drain(am);
	error = apfs_wtx_commit(wm->wm_wtx);
	if (error) {
		wm->wm_errors++;
		printf("apfs: commit of volume \"%s\" failed (%d): the volume is read only from now on\n",
		    am->am_vol.name, error);
		vfs_setflags(am->am_mp, MNT_RDONLY);
	} else {
		wm->wm_commits++;
	}
	return error;
}

/* A file's dirty pages to the device; `wait` non-NULL: until they are there.  No lock held. */
int
apfs_wpush_vnode(vnode_t vp, void *wait)
{
	if (vnode_isreg(vp) && ubc_getsize(vp) > 0) {
		(void)ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | (wait != NULL ? UBC_SYNC : 0));
	}
	return VNODE_RETURNED;
}

static void
apfs_wpush(struct apfs_mount *am)
{
	struct apfs_wmount *wm = am->am_w;
	uint64_t *ids = kalloc_data(APFS_WPUSH_MAX * sizeof(*ids), Z_WAITOK);
	uint32_t n = 0;
	bool all;

	apfs_lock(am);
	all = wm->wm_push_all || ids == NULL;
	if (!all) {
		n = wm->wm_npush;
		memcpy(ids, wm->wm_push, n * sizeof(*ids));
	}
	wm->wm_npush = 0;
	wm->wm_push_all = false;
	apfs_unlock(am);
	if (all) {
		vnode_iterate(am->am_mp, 0, apfs_wpush_vnode, NULL);
	}
	for (uint32_t i = 0; i < n; i++) {
		vnode_t vp = NULLVP;
		uint32_t vid = 0;

		apfs_lock(am);
		for (struct apfs_node *an = am->am_hash[apfs_hash_index(ids[i])]; an != NULL; an = an->an_next) {
			if (an->an_id == ids[i] && an->an_attached) {
				vp = an->an_vp;
				vid = vnode_vid(vp);
				break;
			}
		}
		apfs_unlock(am);
		if (vp != NULLVP && vnode_getwithvid(vp, vid) == 0) {
			(void)apfs_wpush_vnode(vp, NULL);
			vnode_put(vp);
		}
	}
	if (ids != NULL) {
		kfree_data(ids, APFS_WPUSH_MAX * sizeof(*ids));
	}
}

/* push: send the written files' pages first (sync(2), the periodic commit) */
int
apfs_wcommit(struct apfs_mount *am, bool push)
{
	int error;

	if (am->am_w == NULL) {
		return 0;
	}
	if (push) {
		apfs_wpush(am);
	}
	apfs_lock_exclusive(am);
	error = apfs_wcommit_locked(am);
	apfs_unlock_exclusive(am);
	return error;
}

/* F_FULLFSYNC and F_BARRIERFSYNC: what is open is committed; with nothing open, a flush. */
int
apfs_wflush(struct apfs_mount *am)
{
	int error;

	if (am->am_w == NULL) {
		return 0;
	}
	apfs_lock_exclusive(am);
	if (apfs_wtx_dirty(am->am_w->wm_wtx)) {
		error = apfs_wcommit_locked(am);
	} else {
		apfs_wio_drain(am);
		error = am->am_nx.dev.flush != NULL ? am->am_nx.dev.flush(am->am_nx.dev.ctx) : 0;
	}
	apfs_unlock_exclusive(am);
	return error;
}

uint64_t
apfs_wcommits(struct apfs_mount *am)
{
	return am->am_w != NULL ? am->am_w->wm_commits : 0;
}

static void
apfs_wtick(thread_call_param_t p0, __unused thread_call_param_t p1)
{
	(void)apfs_wcommit((struct apfs_mount *)p0, true);
}

/* The periodic commit, a second from now unless it is due already; with am_lock held. */
static void
apfs_warm(struct apfs_wmount *wm)
{
	uint64_t deadline;

	if (!wm->wm_armed && !wm->wm_closing) {
		wm->wm_armed = true;
		clock_interval_to_deadline(APFS_WCOMMIT_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(wm->wm_tick, deadline);
	}
}

/* After a change, with the exclusive lock held: the commit is due in a second, or now. */
void
apfs_wdirty(struct apfs_mount *am)
{
	struct apfs_wmount *wm = am->am_w;

	if (apfs_wtx_dirty_blocks(wm->wm_wtx) > APFS_WDIRTY_MAX) {
		(void)apfs_wcommit_locked(am);
	} else if (apfs_wtx_dirty(wm->wm_wtx)) {
		apfs_warm(wm);
	}
}

/* A file whose pages the next commit sends first; with am_lock held. */
void
apfs_wnote(struct apfs_mount *am, uint64_t id)
{
	struct apfs_wmount *wm = am->am_w;

	uint32_t i;

	for (i = 0; i < wm->wm_npush && wm->wm_push[i] != id; i++) {
	}
	if (i == wm->wm_npush) {
		if (wm->wm_npush < APFS_WPUSH_MAX) {
			wm->wm_push[wm->wm_npush++] = id;
		} else {
			wm->wm_push_all = true;
		}
	}
	apfs_warm(wm);
}

/* ------------------------------------------------------------------ mount and unmount */

/*
 * After the volume is open: the transaction, and what a crash left in the private directory.
 * A volume the engine cannot write (apfs_wtx_attach says why) is mounted read only.
 */
static void
apfs_wmount_attach(struct apfs_mount *am, struct apfs_devctx *dc)
{
	struct apfs_wmount *wm;
	int error;

	if (vfs_isrdonly(am->am_mp)) {
		vfs_setflags(am->am_mp, MNT_RDONLY);
		return;
	}
	wm = kalloc_type(struct apfs_wmount, Z_WAITOK | Z_ZERO);
	if (wm == NULL) {
		vfs_setflags(am->am_mp, MNT_RDONLY);
		return;
	}
	lck_spin_init(&wm->wm_spin, &apfs_lck_grp, LCK_ATTR_NULL);
	wm->wm_base = dc->dc_base;
	wm->wm_devblock = dc->dc_devblock;
	wm->wm_bs = am->am_nx.block_size;
	wm->wm_tick = thread_call_allocate_with_priority(apfs_wtick, am, THREAD_CALL_PRIORITY_KERNEL);
	apfs_lock_exclusive(am);
	error = wm->wm_tick == NULL ? ENOMEM : apfs_wtx_attach(&am->am_vol, &wm->wm_wtx);
	if (error == 0) {
		am->am_w = wm;
		error = apfs_w_reap_orphans(wm->wm_wtx);
		if (error == 0) {
			error = apfs_wcommit_locked(am);
		}
		if (error) {
			apfs_wtx_detach(wm->wm_wtx);
			am->am_w = NULL;
		}
	}
	apfs_unlock_exclusive(am);
	if (error) {
		printf("apfs: volume \"%s\" is mounted read only: it cannot be written here (%d)\n",
		    am->am_vol.name, error);
		if (wm->wm_tick != NULL) {
			thread_call_free(wm->wm_tick);
		}
		lck_spin_destroy(&wm->wm_spin, &apfs_lck_grp);
		kfree_type(struct apfs_wmount, wm);
		vfs_setflags(am->am_mp, MNT_RDONLY);
		return;
	}
	(void)vfs_init_io_attributes(am->am_devvp, am->am_mp);
	lck_mtx_lock(&apfs_pool_lock);
	apfs_data_mount = am;
	lck_mtx_unlock(&apfs_pool_lock);
	printf("apfs: volume \"%s\" is writable: %llu blocks free\n", am->am_vol.name,
	    (unsigned long long)apfs_wtx_free_blocks(wm->wm_wtx));
}

/* At unmount, once vflush has pushed every file: the last commit, then the transaction goes. */
static int
apfs_wmount_detach(struct apfs_mount *am)
{
	struct apfs_wmount *wm = am->am_w;
	int error;

	if (wm == NULL) {
		return 0;
	}
	apfs_lock(am);
	wm->wm_closing = true;
	apfs_unlock(am);
	thread_call_cancel_wait(wm->wm_tick);
	error = apfs_wcommit(am, true);
	lck_mtx_lock(&apfs_pool_lock);
	if (apfs_data_mount == am) {
		apfs_data_mount = NULL;
	}
	lck_mtx_unlock(&apfs_pool_lock);
	apfs_lock_exclusive(am);
	apfs_wtx_detach(wm->wm_wtx);
	am->am_w = NULL;
	apfs_unlock_exclusive(am);
	thread_call_free(wm->wm_tick);
	lck_spin_destroy(&wm->wm_spin, &apfs_lck_grp);
	kfree_type(struct apfs_wmount, wm);
	return error;
}

/*
 * Everything written to the data volume onto the device, files' pages included and waited for:
 * the end of a run (bsd/dev/annibale/annibale_ufs.c, annibale_var_save_on_return).
 */
int apfs_data_flush(void);

int
apfs_data_flush(void)
{
	mount_t mp = NULL;
	int error = 0;

	lck_mtx_lock(&apfs_pool_lock);
	if (apfs_data_mount != NULL && vfs_busy(apfs_data_mount->am_mp, LK_NOWAIT) == 0) {
		mp = apfs_data_mount->am_mp;
	}
	lck_mtx_unlock(&apfs_pool_lock);
	if (mp != NULL) {
		struct apfs_mount *am = vfs_fsprivate(mp);

		vnode_iterate(mp, 0, apfs_wpush_vnode, am);
		error = apfs_wcommit(am, false);
		vfs_unbusy(mp);
	}
	return error;
}

/* sysctl kern.apfs_wstats: what the writable volume has done since it was mounted. */
static int
sysctl_apfs_wstats SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[256] = "no writable APFS volume";

	lck_mtx_lock(&apfs_pool_lock);
	if (apfs_data_mount != NULL) {
		struct apfs_wmount *wm = apfs_data_mount->am_w;
		uint64_t commits, blocks, ticks;

		apfs_wtx_stats(wm->wm_wtx, &commits, &blocks, &ticks);
		snprintf(line, sizeof(line), "\"%s\": %llu commits, %llu blocks written in %llu ms, %llu failed, "
		    "%llu slow drains, %llu blocks free", apfs_data_mount->am_vol.name, (unsigned long long)commits,
		    (unsigned long long)blocks, (unsigned long long)apfs_ticks_ms(ticks),
		    (unsigned long long)wm->wm_errors, (unsigned long long)wm->wm_slow_drains,
		    (unsigned long long)apfs_wtx_free_blocks(wm->wm_wtx));
	}
	lck_mtx_unlock(&apfs_pool_lock);
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_PROC(_kern, OID_AUTO, apfs_wstats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_apfs_wstats, "A", "the writable APFS volume: commits, blocks written, failures");

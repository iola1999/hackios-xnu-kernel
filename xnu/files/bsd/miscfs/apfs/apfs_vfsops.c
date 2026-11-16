/*
 * apfs mount-level operations.  See apfs.h; the format is in apfs_lib.c.
 */

#include <kern/assert.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <miscfs/apfs/apfs.h>
#include <sys/attr.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <uuid/uuid.h>
#include <sys/sysctl.h>
#include <sys/vnode_internal.h>
#include <sys/fcntl.h>
#include <sys/annibale_input.h>
#include <sys/annibale_virtio_blk.h>
#include <sys/proc.h>
#include <kern/thread.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

static LCK_GRP_DECLARE(apfs_lck_grp, "apfs");

/* Blocks are read from the device in pieces of this size; data, in requests of at most APFS_DATA_MAX. */
#define APFS_META_CHUNK 4096
#define APFS_DATA_MAX   (128 * 1024)

/*
 * The device, as apfs_lib wants it.  Offsets and lengths are whole container blocks, and
 * a container block is a multiple of the device's block size (both are powers of two and
 * the container's is at least 4096), so every read lines up.
 */
struct apfs_devctx {
	vnode_t  dc_devvp;
	uint32_t dc_devblock;
	uint64_t dc_base;               /* where on the device the container starts (bytes) */
};

static int
apfs_dev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	char *out = buf;

	off += dc->dc_base;
	if ((off % dc->dc_devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		buf_t bp = NULL;
		size_t chunk = len < APFS_META_CHUNK ? len : APFS_META_CHUNK;
		int error = (int)buf_meta_bread(dc->dc_devvp,
		    (daddr64_t)(off / dc->dc_devblock), APFS_META_CHUNK, NOCRED, &bp);

		if (error) {
			if (bp != NULL) {
				buf_brelse(bp);
			}
			return error;
		}
		memcpy(out, (const void *)buf_dataptr(bp), chunk);
		/*
		 * apfs_lib keeps its own cache of what it needs, and a volume this size would
		 * otherwise fill the buffer cache with blocks nobody asks for twice.
		 */
		buf_markinvalid(bp);
		buf_brelse(bp);

		out += chunk;
		off += chunk;
		len -= chunk;
	}
	return 0;
}
/*
 * A file's contents, straight into the caller's memory: one buffer header of our own and
 * one VNOP_STRATEGY a request, past the buffer cache.  apfs_lib calls this without the
 * volume's lock, from several threads at once; nothing here is shared but the device.
 * (Tried once before, in QEMU, in place of the block cache for the middle of a request and
 * with the lock held, it was slower there; here it is the device reads coming out from
 * under the lock, on a device where one costs a memory copy -- see apfs_lib.c.)
 */
static int
apfs_dev_read_data(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	char *out = buf;

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
		buf_reset(bp, B_READ);
		buf_setblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setlblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setcount(bp, (uint32_t)n);
		buf_setdataptr(bp, (uintptr_t)out);
		error = VNOP_STRATEGY(bp);
		if (error == 0) {
			error = (int)buf_biowait(bp);
		}
		/* read() must fill the whole buffer, and so must this (md cuts the count short) */
		if (error == 0 && (buf_resid(bp) != 0 || buf_count(bp) != n)) {
			error = EIO;
		}
		buf_free(bp);
		if (error) {
			return error;
		}
		out += n;
		off += n;
		len -= n;
	}
	return 0;
}

/* ------------------------------------------------------------- where the time goes */

/*
 * One mounted volume, for the sysctl below to look at.  This port mounts one APFS volume
 * and the counters are a bring-up aid, so a single pointer is enough; it is set when the
 * volume is mounted and cleared when it goes away.
 */
static struct apfs_mount *apfs_the_mount;
static LCK_MTX_DECLARE(apfs_pool_lock, &apfs_lck_grp);   /* the pool's threads, and apfs_mounts */

/*
 * Every APFS mount, for a mount of another container of the same device to find that the
 * device is open already (apfs_mount): the root's RAM disk holds the boot volume and the OS
 * cryptex, each its own container.  Under apfs_pool_lock.
 */
static struct apfs_mount *apfs_mounts;

static bool
apfs_device_held(dev_t rdev)
{
	struct apfs_mount *m;
	bool held = false;

	lck_mtx_lock(&apfs_pool_lock);
	for (m = apfs_mounts; m != NULL && !held; m = m->am_next) {
		held = (m->am_rdev == rdev);
	}
	lck_mtx_unlock(&apfs_pool_lock);
	return held;
}

/*
 * Which volume kern.apfs_stats is about: the root's, when the root is APFS (the iOS volume on
 * the flash, once init has switched to it); else the one mounted last.
 */
extern vnode_t rootvnode;

static struct apfs_mount *
apfs_stats_mount(void)
{
	mount_t mp = rootvnode != NULLVP ? vnode_mount(rootvnode) : NULL;

	if (mp != NULL && strcmp(vfs_statfs(mp)->f_fstypename, "apfs") == 0 && vfs_fsprivate(mp) != NULL) {
		return vfs_fsprivate(mp);
	}
	return apfs_the_mount;
}

/* The volume's lock by who takes it, and how many others waited, appended to buf */
static void
apfs_lock_report(struct apfs_mount *am, char *buf, size_t len)
{
	size_t at = strlen(buf);
	uint32_t i;

	at += scnprintf(buf + at, len - at, "lookups  %llu of names not there, now in the name cache as such; %llu "
	    "answered by it so\n", am->am_lookup_negative, am->am_lookup_negative_hits);
	at += scnprintf(buf + at, len - at, "inactive %llu with nothing to give back, without the lock\n",
	    am->am_inactive_free);
	at += scnprintf(buf + at, len - at, "vnodes   %llu found there already (%llu of them by a lookup, its name now in the "
	    "name cache), %llu made; %llu lookups the name cache answered with the vnode\n", am->am_node_found,
	    am->am_node_named, am->am_node_made, am->am_lookup_positive_hits);
	at += scnprintf(buf + at, len - at, "lock     waited by priority (all ms; over 1 ms: how many, ms): background (<=4) %llu; "
	    "%llu, %llu; utility (<=20) %llu; %llu, %llu; default (<=31) %llu; %llu, %llu; user-initiated (<=46) %llu; %llu, "
	    "%llu; interactive %llu; %llu, %llu\n",
	    apfs_ticks_ms(am->am_prio_wait[0]), am->am_prio_long[0], apfs_ticks_ms(am->am_prio_long_wait[0]),
	    apfs_ticks_ms(am->am_prio_wait[1]), am->am_prio_long[1], apfs_ticks_ms(am->am_prio_long_wait[1]),
	    apfs_ticks_ms(am->am_prio_wait[2]), am->am_prio_long[2], apfs_ticks_ms(am->am_prio_long_wait[2]),
	    apfs_ticks_ms(am->am_prio_wait[3]), am->am_prio_long[3], apfs_ticks_ms(am->am_prio_long_wait[3]),
	    apfs_ticks_ms(am->am_prio_wait[4]), am->am_prio_long[4], apfs_ticks_ms(am->am_prio_long_wait[4]));
	{
		static const char *const band[5] = { "background", "utility", "default", "user-initiated", "interactive" };

		at += scnprintf(buf + at, len - at, "lock     waits over 1 ms, where the time went (ms: blocked, the lock not "
		    "given / queued, runnable without a processor / on a processor):");
		for (i = 0; i < 5; i++) {
			at += scnprintf(buf + at, len - at, " %s %llu/%llu/%llu%s", band[i],
			    (unsigned long long)apfs_ticks_ms(am->am_long_blocked[i]),
			    (unsigned long long)apfs_ticks_ms(am->am_long_queued[i]),
			    (unsigned long long)apfs_ticks_ms(am->am_long_oncpu[i]), i < 4 ? ";" : "\n");
		}
		at += scnprintf(buf + at, len - at, "lock     waits over 1 ms, the processor each ended on:");
		for (i = 0; i < 8; i++) {
			at += scnprintf(buf + at, len - at, " cpu %u %llu%s", i, am->am_long_cpu[i], i < 7 ? "," : "\n");
		}
	}
	at += scnprintf(buf + at, len - at, "lock     takes with others waiting: none %llu, 1 %llu, 2-3 %llu, 4-7 %llu, "
	    "8 or more %llu\n", am->am_waiters_hist[0], am->am_waiters_hist[1], am->am_waiters_hist[2],
	    am->am_waiters_hist[3], am->am_waiters_hist[4]);
	{
		uint64_t max_ns;

		absolutetime_to_nanoseconds(am->am_lock_hold_max, &max_ns);
		at += scnprintf(buf + at, len - at, "lock     holds: %llu under 10 us, %llu to 100 us, %llu to 1 ms, %llu to "
		    "10 ms, %llu longer; longest %llu us\n", am->am_lock_hold_hist[0], am->am_lock_hold_hist[1],
		    am->am_lock_hold_hist[2], am->am_lock_hold_hist[3], am->am_lock_hold_hist[4], max_ns / 1000);
	}
	for (i = 0; i < APFS_LOCK_SITES && am->am_sites[i].name != NULL && at < len; i++) {
		const struct apfs_lock_site *ls = &am->am_sites[i];
		uint64_t wmax, hmax;

		absolutetime_to_nanoseconds(ls->wait_max, &wmax);
		absolutetime_to_nanoseconds(ls->hold_max, &hmax);
		at += scnprintf(buf + at, len - at, "lock by  %-22s %7llu takes %7llu again; waited %6llu ms (longest %6llu us, "
		    "%5llu over 1 ms); held %6llu ms (longest %6llu us, %5llu over 1 ms)\n", ls->name, ls->takes, ls->relocks,
		    (unsigned long long)apfs_ticks_ms(ls->wait), wmax / 1000, ls->wait_long,
		    (unsigned long long)apfs_ticks_ms(ls->hold), hmax / 1000, ls->hold_long);
	}
	for (i = 0; am->am_procs != NULL && i < APFS_LOCK_PROCS && i < am->am_nprocs && at < len; i++) {
		const struct apfs_lock_proc *lp = &am->am_procs[i];
		uint64_t wmax;

		/* (only who waited 1 ms or more in all, or read 1 MB or more) */
		if (apfs_ticks_ms(lp->wait) == 0 && lp->read_bytes + lp->pagein_bytes < (1 << 20)) {
			continue;
		}
		absolutetime_to_nanoseconds(lp->wait_max, &wmax);
		at += scnprintf(buf + at, len - at, "who      %-16.16s waited %6llu times over 10 us, %6llu ms (longest %7llu us, "
		    "%4llu over 1 ms); read(2) %6llu calls %10llu bytes %6llu ms; pagein %6llu calls %10llu bytes %6llu ms\n",
		    lp->name, lp->waits, (unsigned long long)apfs_ticks_ms(lp->wait), wmax / 1000, lp->wait_long,
		    lp->read_calls, lp->read_bytes, (unsigned long long)apfs_ticks_ms(lp->read_ticks), lp->pagein_calls,
		    lp->pagein_bytes, (unsigned long long)apfs_ticks_ms(lp->pagein_ticks));
	}
}

/*
 * kern.apfs_stats: reading it says where the time went, writing anything zeroes the
 * counters.  The point is to tell apart, on one road through the driver, the time spent
 * waiting for the device, the time spent decompressing, and the rest -- which is the
 * driver's own work and the paging machinery above it.
 */
static int
sysctl_apfs_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	struct apfs_mount *am = apfs_stats_mount();
	size_t cap = 16384;
	char *text = kalloc_data(cap, Z_WAITOK | Z_ZERO);
	int error;

	if (text == NULL) {
		return ENOMEM;
	}
	if (am == NULL) {
		strlcpy(text, "apfs: nothing mounted\n", cap);
	} else {
		struct apfs_nx *nx = &am->am_nx;

		snprintf(text, cap,
		    "pagein   %llu calls  %llu ms  %llu bytes\n"
		    "read     %llu calls  %llu ms  %llu bytes; from the page cache %llu, read into it %llu (%s)\n"
		    "device   %llu calls  %llu ms  %llu bytes\n"
		    "data     %llu calls  %llu ms  %llu bytes\n"
		    "inflate  %llu calls  %llu ms  %llu bytes in  %llu bytes out  %llu by zlib\n"
		    "blocks   %llu read  %llu cache hits\n"
		    "lookups  %llu by the name's hash  %llu by reading the directory\n"
		    "lock     %llu takes  %llu ms waiting  %llu ms held; metadata read without it %llu times\n",
		    (unsigned long long)am->am_pagein_calls,
		    (unsigned long long)apfs_ticks_ms(am->am_pagein_ticks),
		    (unsigned long long)am->am_pagein_bytes,
		    (unsigned long long)am->am_read_calls,
		    (unsigned long long)apfs_ticks_ms(am->am_read_ticks),
		    (unsigned long long)am->am_read_bytes,
		    (unsigned long long)am->am_read_cached,
		    (unsigned long long)am->am_read_filled,
		    am->am_ubc_read ? "read(2) through it" : "read(2) past it: apfs_ubc_read=0",
		    (unsigned long long)nx->dev_calls,
		    (unsigned long long)apfs_ticks_ms(nx->dev_ticks),
		    (unsigned long long)nx->dev_bytes,
		    (unsigned long long)nx->data_calls,
		    (unsigned long long)apfs_ticks_ms(nx->data_ticks),
		    (unsigned long long)nx->data_bytes,
		    (unsigned long long)nx->inflate_calls,
		    (unsigned long long)apfs_ticks_ms(nx->inflate_ticks),
		    (unsigned long long)nx->inflate_in,
		    (unsigned long long)nx->inflate_out,
		    (unsigned long long)nx->inflate_fallbacks,
		    (unsigned long long)nx->reads,
		    (unsigned long long)nx->hits,
		    (unsigned long long)nx->lookups_hashed,
		    (unsigned long long)nx->lookups_scanned,
		    (unsigned long long)am->am_lock_waits,
		    (unsigned long long)apfs_ticks_ms(am->am_lock_wait_ticks),
		    (unsigned long long)apfs_ticks_ms(am->am_lock_held_ticks),
		    (unsigned long long)nx->unlocked_fills);
		apfs_lock_report(am, text, cap);
	}
	error = SYSCTL_OUT(req, text, strlen(text) + 1);
	kfree_data(text, cap);
	if (error || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (am != NULL) {
		apfs_lock(am);
		am->am_pagein_calls = am->am_pagein_ticks = am->am_pagein_bytes = 0;
		am->am_read_calls = am->am_read_ticks = am->am_read_bytes = 0;
		am->am_nx.dev_calls = am->am_nx.dev_ticks = am->am_nx.dev_bytes = 0;
		am->am_nx.data_calls = am->am_nx.data_ticks = am->am_nx.data_bytes = 0;
		am->am_nx.inflate_calls = am->am_nx.inflate_ticks = 0;
		am->am_nx.inflate_in = am->am_nx.inflate_out = am->am_nx.inflate_fallbacks = 0;
		am->am_nx.reads = am->am_nx.hits = 0;
		am->am_lock_waits = am->am_lock_wait_ticks = am->am_lock_held_ticks = 0;
		memset(am->am_lock_wait_hist, 0, sizeof(am->am_lock_wait_hist));
		am->am_lock_wait_max = 0;
		memset(am->am_lock_hold_hist, 0, sizeof(am->am_lock_hold_hist));
		am->am_lock_hold_max = 0;
		memset(am->am_waiters_hist, 0, sizeof(am->am_waiters_hist));
		am->am_lookup_negative = 0;
		am->am_lookup_negative_hits = 0;
		am->am_inactive_free = 0;
		am->am_lookup_positive_hits = am->am_node_found = am->am_node_made = am->am_node_named = 0;
		memset(am->am_prio_wait, 0, sizeof(am->am_prio_wait));
		memset(am->am_prio_long, 0, sizeof(am->am_prio_long));
		memset(am->am_prio_long_wait, 0, sizeof(am->am_prio_long_wait));
		memset(am->am_long_blocked, 0, sizeof(am->am_long_blocked));
		memset(am->am_long_queued, 0, sizeof(am->am_long_queued));
		memset(am->am_long_oncpu, 0, sizeof(am->am_long_oncpu));
		memset(am->am_long_cpu, 0, sizeof(am->am_long_cpu));
		lck_spin_lock(&am->am_procs_spin);
		if (am->am_procs != NULL) {
			memset(am->am_procs, 0, APFS_LOCK_PROCS * sizeof(am->am_procs[0]));
		}
		am->am_nprocs = 0;
		lck_spin_unlock(&am->am_procs_spin);
		am->am_read_cached = am->am_read_filled = 0;
		for (uint32_t i = 0; i < APFS_LOCK_SITES; i++) {
			const char *name = am->am_sites[i].name;

			memset(&am->am_sites[i], 0, sizeof(am->am_sites[i]));
			am->am_sites[i].name = name;
		}
		apfs_unlock(am);
	}
	return 0;
}

/*
 * The same breakdown, straight to the log.  Needed where there is no user space to read
 * the sysctl: when the volume is the root, nothing else is running.
 */
void
apfs_stats_print(void)
{
	struct apfs_mount *am = apfs_stats_mount();
	struct apfs_nx *nx;

	if (am == NULL) {
		printf("apfs: nothing mounted\n");
		return;
	}
	nx = &am->am_nx;
	printf("apfs: pagein  %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)am->am_pagein_calls,
	    (unsigned long long)apfs_ticks_ms(am->am_pagein_ticks),
	    (unsigned long long)am->am_pagein_bytes);
	printf("apfs: read    %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)am->am_read_calls,
	    (unsigned long long)apfs_ticks_ms(am->am_read_ticks),
	    (unsigned long long)am->am_read_bytes);
	printf("apfs: device  %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)nx->dev_calls,
	    (unsigned long long)apfs_ticks_ms(nx->dev_ticks),
	    (unsigned long long)nx->dev_bytes);
	/* file contents, read without the lock (the line above is metadata, with it) */
	printf("apfs: data    %llu calls  %llu ms  %llu bytes\n",
	    (unsigned long long)nx->data_calls,
	    (unsigned long long)apfs_ticks_ms(nx->data_ticks),
	    (unsigned long long)nx->data_bytes);
	/* "by zlib": streams libdeflate refused (bad, or more out than the piece holds), zlib's then */
	printf("apfs: inflate %llu calls  %llu ms  %llu bytes in  %llu bytes out  %llu by zlib\n",
	    (unsigned long long)nx->inflate_calls,
	    (unsigned long long)apfs_ticks_ms(nx->inflate_ticks),
	    (unsigned long long)nx->inflate_in, (unsigned long long)nx->inflate_out,
	    (unsigned long long)nx->inflate_fallbacks);
	/* requests whose whole pieces went out together (nx->parallel), and how many pieces */
	printf("apfs: batch   %llu requests  %llu pieces  %u threads at most\n",
	    (unsigned long long)nx->batch_calls, (unsigned long long)nx->batch_pieces,
	    nx->parallel != NULL ? nx->parallel_max : 1);
	printf("apfs: ahead   %llu times  %llu pieces  %llu of them used\n",
	    (unsigned long long)nx->ra_batches, (unsigned long long)nx->ra_pieces,
	    (unsigned long long)nx->ra_hits);
	printf("apfs: blocks  %llu read  %llu cache hits  highest block %llu\n",
	    (unsigned long long)nx->reads, (unsigned long long)nx->hits,
	    (unsigned long long)nx->max_addr);
	printf("apfs: lock    %llu takes  %llu ms waiting  %llu ms held\n",
	    (unsigned long long)am->am_lock_waits,
	    (unsigned long long)apfs_ticks_ms(am->am_lock_wait_ticks),
	    (unsigned long long)apfs_ticks_ms(am->am_lock_held_ticks));
	{
		uint64_t max_ns;

		absolutetime_to_nanoseconds(am->am_lock_wait_max, &max_ns);
		printf("apfs: lock    waits: %llu under 2 us, %llu to 10 us, %llu to 100 us, %llu to 1 ms, %llu longer; "
		    "longest %llu us; metadata read from the device %llu ms\n",
		    am->am_lock_wait_hist[0], am->am_lock_wait_hist[1], am->am_lock_wait_hist[2], am->am_lock_wait_hist[3],
		    am->am_lock_wait_hist[4], max_ns / 1000, (unsigned long long)apfs_ticks_ms(nx->dev_ticks));
		absolutetime_to_nanoseconds(am->am_lock_hold_max, &max_ns);
		printf("apfs: lock    holds: %llu under 10 us, %llu to 100 us, %llu to 1 ms, %llu to 10 ms, %llu longer; "
		    "longest %llu us\n", am->am_lock_hold_hist[0], am->am_lock_hold_hist[1], am->am_lock_hold_hist[2],
		    am->am_lock_hold_hist[3], am->am_lock_hold_hist[4], max_ns / 1000);
		printf("apfs: lookups %llu by the name's hash, %llu by reading the directory\n", nx->lookups_hashed,
		    nx->lookups_scanned);
		{
			char *rep = kalloc_data(16384, Z_WAITOK | Z_ZERO);

			if (rep != NULL) {
				apfs_lock_report(am, rep, 16384);
				printf("%s", rep);
				kfree_data(rep, 16384);
			}
		}
		printf("apfs: metadata read without the lock: %llu times, %llu of them already in by another's read; "
		    "retired blocks %llu now, %llu at most; operations going on now %u\n", nx->unlocked_fills, nx->fill_races,
		    nx->retired_now, nx->retired_most, am->am_nops);
	}
}

/* kern.annibale_memory: the kernel's line about memory (osfmk/arm64/loader_return.c), for a job list to keep */
extern void annibale_memory_line(char *buf, size_t len);

static int
sysctl_annibale_memory SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[256];

	annibale_memory_line(line, sizeof(line));
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_PROC(_kern, OID_AUTO, annibale_memory, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_annibale_memory, "A", "where the memory is, in MB");

SYSCTL_PROC(_kern, OID_AUTO, apfs_stats, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_apfs_stats, "A", "APFS: where the time goes; write to zero it");

/* ------------------------------------------------------------------ vnodes */

static uint32_t
apfs_hash_index(uint64_t obj_id)
{
	/* the low bits of an identifier are the ones that differ between neighbours */
	return (uint32_t)((obj_id ^ (obj_id >> 17)) % APFS_HASH_BUCKETS);
}

/* The vnode for an object, making one if there is not one already. */
int
apfs_node_vnode(struct apfs_mount *am, uint64_t obj_id, vnode_t dvp,
    struct componentname *cnp, vnode_t *vpp)
{
	struct vnode_fsparam vfsp;
	struct apfs_node *an;
	struct apfs_inode ino;
	uint32_t b = apfs_hash_index(obj_id);
	uint64_t size = 0;
	enum vtype vtype;
	int error;

	/* already have one? */
	apfs_lock(am);
	for (an = am->am_hash[b]; an != NULL; an = an->an_next) {
		if (an->an_id == obj_id && an->an_attached) {
			vnode_t vp = an->an_vp;
			uint32_t vid = vnode_vid(vp);

			apfs_unlock(am);
			if (vnode_getwithvid(vp, vid) == 0) {
				os_atomic_inc(&am->am_node_found, relaxed);
				/*
				 * Found by a lookup: the name goes into the name cache, as vnode_create
				 * puts it there for a new vnode -- else every later lookup of this
				 * name comes back here for the lock.  (The volume does not change;
				 * a synthetic name is looked for only where there is no real one, and
				 * adding one purges its directory's entries.)  The name cache keeps
				 * one name per vnode and leaves one that has a name already as it is.
				 */
				if (dvp != NULLVP && cnp != NULL) {
					if (an->an_parent == APFS_VP(dvp)->an_id && vnode_parent(vp) != dvp) {
						vnode_update_identity(vp, dvp, cnp->cn_nameptr,
						    cnp->cn_namelen, cnp->cn_hash,
						    VNODE_UPDATE_PARENT | VNODE_UPDATE_NAME | VNODE_UPDATE_CACHE);
					}
					if (cnp->cn_flags & MAKEENTRY) {
						cache_enter(dvp, vp, cnp);
						os_atomic_inc(&am->am_node_named, relaxed);
					}
				}
				*vpp = vp;
				return 0;
			}
			/* it was going away under us: fall through and make a new one */
			apfs_lock(am);
			break;
		}
	}

	if (obj_id >= APFS_SYNTH_BASE) {
		/* a synthetic directory: an empty one, root's, made up here */
		const struct apfs_synth *sy = NULL;
		uint32_t i;

		for (i = 0; i < am->am_nsynth; i++) {
			if (am->am_synth[i].id == obj_id) {
				sy = &am->am_synth[i];
			}
		}
		bzero(&ino, sizeof(ino));
		if (sy == NULL) {
			error = ENOENT;
		} else {
			ino.obj_id = obj_id;
			ino.parent_id = sy->parent;
			ino.mode = APFS_S_IFDIR | 0755;
			error = 0;
		}
	} else {
		error = apfs_inode_get(&am->am_vol, obj_id, &ino);
		if (error == 0) {
			uint32_t i;

			error = apfs_inode_size(&am->am_vol, &ino, &size);
			if (error) {
				size = ino.size;       /* the stream's size is still better than nothing */
				error = 0;
			}
			/* a directory with another name: ".." is where that name is */
			for (i = 0; i < am->am_nsynth; i++) {
				if (am->am_synth[i].alias && am->am_synth[i].id == obj_id) {
					ino.parent_id = am->am_synth[i].parent;
				}
			}
		}
	}
	apfs_unlock(am);
	if (error) {
		return error;
	}

	switch (ino.mode & APFS_S_IFMT) {
	case APFS_S_IFREG:
		vtype = VREG;
		break;
	case APFS_S_IFDIR:
		vtype = VDIR;
		break;
	case APFS_S_IFLNK:
		vtype = VLNK;
		break;
	case APFS_S_IFCHR:
		vtype = VCHR;
		break;
	case APFS_S_IFBLK:
		vtype = VBLK;
		break;
	case APFS_S_IFIFO:
		vtype = VFIFO;
		break;
	case APFS_S_IFSOCK:
		vtype = VSOCK;
		break;
	default:
		return EIO;
	}

	an = kalloc_type(struct apfs_node, Z_WAITOK | Z_ZERO);
	if (an == NULL) {
		return ENOMEM;
	}
	an->an_mount = am;
	an->an_id = obj_id;
	an->an_parent = ino.parent_id;
	an->an_mode = ino.mode;
	an->an_size = size;
	an->an_ino = ino;
	lck_rw_init(&an->an_rwlock, &apfs_lck_grp, LCK_ATTR_NULL);

	bzero(&vfsp, sizeof(vfsp));
	vfsp.vnfs_mp = am->am_mp;
	vfsp.vnfs_vtype = vtype;
	vfsp.vnfs_str = "apfs";
	vfsp.vnfs_dvp = dvp;
	vfsp.vnfs_fsnode = an;
	vfsp.vnfs_vops = apfs_vnodeop_p;
	vfsp.vnfs_markroot = (obj_id == am->am_root_id);
	vfsp.vnfs_marksystem = 0;
	vfsp.vnfs_rdev = (vtype == VCHR || vtype == VBLK) ? (dev_t)ino.rdev : 0;
	vfsp.vnfs_filesize = size;
	vfsp.vnfs_cnp = cnp;
	/* let VFS cache the name only when it came from a lookup that has one */
	vfsp.vnfs_flags = (dvp != NULL && cnp != NULL) ? 0 : VNFS_NOCACHE;

	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &an->an_vp);
	if (error) {
		kfree_type(struct apfs_node, an);
		return error;
	}
	vnode_settag(an->an_vp, VT_OTHER);

	/*
	 * Another thread may have made a vnode for the object meanwhile.  Two of them would mean
	 * two page caches for one file, so the later one is thrown away and the search starts over.
	 */
	apfs_lock(am);
	for (struct apfs_node *o = am->am_hash[b]; o != NULL; o = o->an_next) {
		if (o->an_id == obj_id && o->an_attached) {
			apfs_unlock(am);
			vnode_recycle(an->an_vp);
			vnode_put(an->an_vp);
			return apfs_node_vnode(am, obj_id, dvp, cnp, vpp);
		}
	}
	an->an_next = am->am_hash[b];
	am->am_hash[b] = an;
	an->an_attached = true;
	apfs_unlock(am);
	os_atomic_inc(&am->am_node_made, relaxed);

	*vpp = an->an_vp;
	return 0;
}

/* A synthetic directory by where it is and its name; with am_lock held. */
const struct apfs_synth *
apfs_synth_find(struct apfs_mount *am, uint64_t parent, const char *name, size_t len)
{
	uint32_t i;

	for (i = 0; i < am->am_nsynth; i++) {
		if (am->am_synth[i].parent == parent && strlen(am->am_synth[i].name) == len &&
		    memcmp(am->am_synth[i].name, name, len) == 0) {
			return &am->am_synth[i];
		}
	}
	return NULL;
}

/*
 * sysctl kern.apfs_synthetic=PATH: a synthetic directory at PATH, whose parent must be a
 * directory of an APFS volume (real or synthetic) and which must not exist yet.
 * PATH=TARGET: at PATH, another name for the directory TARGET of the same volume.  Root
 * only; nothing ever takes one away again but unmounting.
 */
static int
apfs_sysctl_synthetic SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char path[MAXPATHLEN], name[APFS_SYNTH_NAMELEN], *target;
	vnode_t dvp = NULLVP, vp = NULLVP, tvp = NULLVP;
	struct apfs_mount *am;
	struct apfs_node *dn;
	uint64_t target_id = 0;
	const char *slash;
	size_t len;
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, "", 1);          /* reading it says nothing */
	}
	if ((error = suser(kauth_cred_get(), NULL)) != 0) {
		return error;
	}
	if (req->newlen == 0 || req->newlen >= sizeof(path)) {
		return EINVAL;
	}
	error = SYSCTL_IN(req, path, req->newlen);
	if (error) {
		return error;
	}
	path[req->newlen] = '\0';
	target = strchr(path, '=');
	if (target != NULL) {
		*target++ = '\0';
		error = vnode_lookup(target, 0, &tvp, vfs_context_current());
		if (error) {
			return error;
		}
		if (vnode_vtype(tvp) != VDIR || vnode_mount(tvp) == NULL ||
		    strcmp(vfs_statfs(vnode_mount(tvp))->f_fstypename, "apfs") != 0) {
			vnode_put(tvp);
			return ENOTDIR;
		}
		target_id = APFS_VP(tvp)->an_id;
	}
	while ((len = strlen(path)) > 1 && path[len - 1] == '/') {
		path[len - 1] = '\0';
	}
	slash = strrchr(path, '/');
	if (slash == NULL || slash[1] == '\0') {
		error = EINVAL;
		goto out;
	}
	len = strlen(slash + 1);
	if (len >= sizeof(name)) {
		error = EINVAL;
		goto out;
	}
	strlcpy(name, slash + 1, sizeof(name));        /* before the path is cut to its parent */
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		error = EINVAL;
		goto out;
	}
	if (vnode_lookup(path, 0, &vp, vfs_context_current()) == 0) {
		vnode_put(vp);
		error = EEXIST;
		goto out;
	}
	path[slash - path == 0 ? 1 : slash - path] = '\0';     /* the parent ("/" for "/name") */
	error = vnode_lookup(path, 0, &dvp, vfs_context_current());
	if (error) {
		goto out;
	}
	if (vnode_vtype(dvp) != VDIR || vnode_mount(dvp) == NULL ||
	    strcmp(vfs_statfs(vnode_mount(dvp))->f_fstypename, "apfs") != 0 ||
	    (tvp != NULLVP && vnode_mount(tvp) != vnode_mount(dvp))) {
		vnode_put(dvp);
		error = ENOTSUP;
		goto out;
	}
	dn = APFS_VP(dvp);
	am = dn->an_mount;
	apfs_lock(am);
	if (am->am_synth == NULL) {
		am->am_synth = kalloc_data(APFS_SYNTH_MAX * sizeof(*am->am_synth), Z_WAITOK | Z_ZERO);
	}
	if (am->am_synth == NULL) {
		error = ENOMEM;
	} else if (apfs_synth_find(am, dn->an_id, name, len) != NULL) {
		error = EEXIST;
	} else if (am->am_nsynth >= APFS_SYNTH_MAX) {
		error = ENOSPC;
	} else {
		uint32_t n0 = am->am_nsynth;
		uint64_t id = (tvp != NULLVP) ? target_id : APFS_SYNTH_BASE + n0;
		struct apfs_inode ino;

		/* an id the volume does not use itself, or nothing (reading the tree may let go of the lock) */
		if (tvp == NULLVP && apfs_inode_get(&am->am_vol, id, &ino) != ENOENT) {
			error = EEXIST;
		} else if (am->am_nsynth != n0 || apfs_synth_find(am, dn->an_id, name, len) != NULL) {
			error = EBUSY;          /* another was made meanwhile: try again */
		} else {
			struct apfs_synth *sy = &am->am_synth[am->am_nsynth];

			sy->parent = dn->an_id;
			sy->alias = (tvp != NULLVP);
			sy->id = id;
			strlcpy(sy->name, name, sizeof(sy->name));
			if (tvp != NULLVP) {
				struct apfs_node *tn = APFS_VP(tvp);

				tn->an_parent = dn->an_id;
				tn->an_ino.parent_id = dn->an_id;
			}
			am->am_nsynth++;
			error = 0;
		}
	}
	apfs_unlock(am);
	if (error == 0 && tvp != NULLVP) {
		/*
		 * Target lookup attached tvp under its backing directory; move its
		 * VFS identity to the synthetic parent so F_GETPATH resolves through
		 * the alias after the backing path is covered by another mount.
		 */
		vnode_update_identity(tvp, dvp, name, (int)len, 0,
		    VNODE_UPDATE_PARENT | VNODE_UPDATE_NAME | VNODE_UPDATE_CACHE);
	}
	/* the parent's entries changed: what the name cache has of it is stale */
	cache_purge(dvp);
	cache_purge_negatives(dvp);     /* (the new name was known not to be there) */
	vnode_put(dvp);
out:
	if (tvp != NULLVP) {
		vnode_put(tvp);
	}
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, apfs_synthetic, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, apfs_sysctl_synthetic, "A",
    "write PATH: an empty synthetic directory on an APFS volume (a mount point)");

/* Take a node out of the hash; called from vnop_reclaim. */
void
apfs_node_detach(struct apfs_node *an)
{
	struct apfs_mount *am = an->an_mount;
	uint32_t b = apfs_hash_index(an->an_id);
	struct apfs_node **pp;

	apfs_lock(am);
	for (pp = &am->am_hash[b]; *pp != NULL; pp = &(*pp)->an_next) {
		if (*pp == an) {
			*pp = an->an_next;
			break;
		}
	}
	an->an_attached = false;
	lck_rw_destroy(&an->an_rwlock, &apfs_lck_grp);
	apfs_unlock(am);
}

/* apfs_lib lets go of the mount's lock round decompression, through these */
static void
apfs_lib_unlock(void *ctx)
{
	apfs_unlock_inner((struct apfs_mount *)ctx);     /* the operation goes on */
}

static void
apfs_lib_relock(void *ctx)
{
	apfs_lock_inner((struct apfs_mount *)ctx);
}

/* ------------------------------------------------------- threads for one request */

/*
 * apfs_lib's nx->parallel: a few kernel threads that help a request of many compressed
 * pieces along, each reading and decompressing whole pieces into the caller's buffer while
 * the caller does the same.  A request posts a job and wakes as many helpers as it can
 * use; a helper that comes takes a worker number and then items until none are left.  The
 * caller works through items too, then takes its job off the list -- no helper can join
 * after that -- and waits for the helpers still inside it.  The job lives on the caller's
 * stack, which is why nothing may touch it once the caller is back.
 *
 * Boot-arg apfs_par=N: at most N threads on one request, the caller's included (default:
 * four, or the processors if fewer -- see apfs_pool_start(); 1 turns the helpers off).
 */
struct apfs_pjob {
	struct apfs_pjob *pj_next;
	int             (*pj_fn)(void *arg, uint32_t item, uint32_t worker);
	void             *pj_arg;
	uint32_t          pj_items;
	uint32_t          pj_workers;
	volatile SInt32   pj_taken;     /* items handed out so far (may run past pj_items) */
	volatile SInt32   pj_error;
	uint32_t          pj_joined;    /* worker numbers given out; under apfs_pool_lock */
	uint32_t          pj_active;    /* helpers inside the job now; under apfs_pool_lock */
};

static struct apfs_pjob *apfs_pool_jobs;        /* oldest first */
static uint32_t apfs_pool_threads;
static uint32_t apfs_par_max;                   /* threads on one request, the caller's included */

static void
apfs_pjob_work(struct apfs_pjob *pj, uint32_t worker)
{
	for (;;) {
		SInt32 item = OSIncrementAtomic(&pj->pj_taken);
		int error;

		if (item < 0 || (uint32_t)item >= pj->pj_items) {
			break;
		}
		error = pj->pj_fn(pj->pj_arg, (uint32_t)item, worker);
		if (error != 0) {
			OSCompareAndSwap(0, (UInt32)error, (volatile UInt32 *)&pj->pj_error);
		}
	}
}

static void
apfs_pool_thread(__unused void *arg, __unused wait_result_t wr)
{
	lck_mtx_lock(&apfs_pool_lock);
	for (;;) {
		struct apfs_pjob *pj;
		uint32_t worker;

		for (pj = apfs_pool_jobs; pj != NULL; pj = pj->pj_next) {
			if (pj->pj_joined < pj->pj_workers &&
			    (uint32_t)pj->pj_taken < pj->pj_items) {
				break;
			}
		}
		if (pj == NULL) {
			(void)msleep(&apfs_pool_jobs, &apfs_pool_lock, PRIBIO, "apfs_pool", NULL);
			continue;
		}
		worker = pj->pj_joined++;
		pj->pj_active++;
		lck_mtx_unlock(&apfs_pool_lock);
		apfs_pjob_work(pj, worker);
		lck_mtx_lock(&apfs_pool_lock);
		if (--pj->pj_active == 0) {
			wakeup(&pj->pj_active);
		}
	}
}

static int
apfs_parallel(__unused void *ctx, uint32_t items, uint32_t workers,
    int (*fn)(void *arg, uint32_t item, uint32_t worker), void *arg)
{
	struct apfs_pjob pj, **pp;
	uint32_t i;

	bzero(&pj, sizeof(pj));
	pj.pj_fn = fn;
	pj.pj_arg = arg;
	pj.pj_items = items;
	pj.pj_workers = workers;
	pj.pj_joined = 1;               /* the caller is worker 0 */

	lck_mtx_lock(&apfs_pool_lock);
	for (pp = &apfs_pool_jobs; *pp != NULL; pp = &(*pp)->pj_next) {
	}
	*pp = &pj;
	for (i = 1; i < workers; i++) {
		wakeup_one((caddr_t)&apfs_pool_jobs);
	}
	lck_mtx_unlock(&apfs_pool_lock);

	apfs_pjob_work(&pj, 0);

	lck_mtx_lock(&apfs_pool_lock);
	for (pp = &apfs_pool_jobs; *pp != &pj; pp = &(*pp)->pj_next) {
	}
	*pp = pj.pj_next;
	while (pj.pj_active > 0) {
		(void)msleep(&pj.pj_active, &apfs_pool_lock, PRIBIO, "apfs_pjob", NULL);
	}
	lck_mtx_unlock(&apfs_pool_lock);
	return pj.pj_error;
}

/*
 * The helpers, once, at the first mount.  Four threads on one request at most unless
 * apfs_par says otherwise: on this phone one reader with 1 MB reads took 765 ms over the
 * restore ramdisk with two, 612 with four or five, 714 with all eight -- the four small
 * cores take two and a half times as long over a piece, and a request waits for its
 * slowest one.
 */
static void
apfs_pool_start(void)
{
	uint32_t want = ml_get_topology_info()->num_cpus, i;

	if (want > 4) {
		want = 4;
	}
	if (want < 1) {
		want = 1;
	}
	PE_parse_boot_argn("apfs_par", &want, sizeof(want));
	if (want < 1) {
		want = 1;
	}
	lck_mtx_lock(&apfs_pool_lock);
	if (apfs_par_max != 0) {
		lck_mtx_unlock(&apfs_pool_lock);
		return;
	}
	apfs_par_max = want;
	lck_mtx_unlock(&apfs_pool_lock);
	for (i = 0; i + 1 < want; i++) {
		thread_t th;

		if (kernel_thread_start(apfs_pool_thread, NULL, &th) != KERN_SUCCESS) {
			break;
		}
		thread_deallocate(th);
		OSIncrementAtomic((volatile SInt32 *)&apfs_pool_threads);
	}
	printf("apfs: %u threads on a request of many pieces (%u helpers)\n", want,
	    apfs_pool_threads);
}

/* ------------------------------------------------------------------ mount & vfsops */

#include "apfs_vfsops_write.inc"
#include "apfs_vfsops_mount.inc"
#include "apfs_vfsops_sync.inc"

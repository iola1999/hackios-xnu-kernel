/*
 * ramfs: writable files with optional ANNFS001 backing for /private/var.
 *
 * UFS system boots require /private/var to mount its physical backing.
 * Other mount points retain temporary, memory-only storage.
 *
 * Nodes (files, directories, symlinks, socket names) are plain kernel
 * structures; a directory is a list of (name, node) entries, so hard links
 * are just two entries naming one node. File contents are kept in
 * individually allocated pages hanging off the node. Disk-backed pages are
 * read on demand and written through; the page cache is used for
 * mmap, with page-in and page-out copying between the two. After a write(),
 * the pages of the page cache that hold what was written are brought up to
 * date in place (ramfs_refresh_cache()).
 *
 * rm_lock protects mount data and is released before user memory accesses,
 * which may fault back into ramfs. Copies use a bounce buffer. A separate
 * rm_rename_lock serializes renames through their vnode identity updates.
 *
 * A node lives while it is named by a directory or has a vnode; after the
 * last unlink it is freed when its vnode is reclaimed.
 */

#include <kern/assert.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/crc.h>
#include <libkern/libkern.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/attr.h>
#include <sys/content_protection.h>
#include <sys/ioccom.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/ubc.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/vnode_internal.h>
#include <sys/xattr.h>
#include <uuid/uuid.h>
#include <vfs/vfs_support.h>
#include <vm/vm_kern.h>

#define RAMFS_MAX_NAME          255
#define RAMFS_MAX_XATTR         (16u * 1024 * 1024)
#define RAMFS_ROOT_INO          2

struct ramfs_mount;
struct ramfs_dirent {
	TAILQ_ENTRY(ramfs_dirent) rd_link;
	struct ramfs_node *rd_node;
	char              *rd_name;             /* rd_namelen bytes and a terminator */
	uint32_t           rd_namelen;
};

/* An extended attribute: iOS keeps container metadata, ACLs and much else in them. */
struct ramfs_xattr {
	TAILQ_ENTRY(ramfs_xattr) rx_link;
	char   *rx_name;                        /* rx_namelen bytes and a terminator */
	void   *rx_data;
	size_t  rx_namelen, rx_size;
};

struct ramfs_node {
	struct ramfs_mount *rn_mount;
	uint64_t            rn_ino, rn_size;    /* rn_size: file bytes or symlink target length */
	enum vtype          rn_type;
	uint32_t            rn_mode;            /* permission bits only */
	uid_t               rn_uid;
	gid_t               rn_gid;
	uint32_t            rn_bsdflags, rn_dataprotect_class, rn_nlink, rn_state, rn_vid, rn_nentries;
	uint32_t            rn_iflags;          /* RAMFS_IF_*: saved with the node, not visible to chflags */
	uint32_t            rn_docid;           /* document ID, given when UF_TRACKED is first set; 0 until then */
	uint32_t            rn_write_gen;       /* ATTR_CMN_GEN_COUNT of a non-directory: 0 until its contents first change */
	vnode_t             rn_vp;
	struct timespec     rn_atime, rn_mtime, rn_ctime, rn_btime;
	/* directory */
	struct ramfs_node  *rn_parent;          /* NULL once the directory has been removed */
	TAILQ_HEAD(, ramfs_dirent) rn_entries;
	/* regular file */
	void              **rn_pages;           /* rn_nslots slots; NULL is a hole that reads as zeros */
	uint32_t           *rn_blocks;          /* rn_nslots disk block numbers on darwin_var (0 = hole) */
	uint64_t            rn_nslots, rn_nalloc; /* rn_nalloc: pages actually allocated */
	bool                rn_mapped_write, rn_hashed; /* rn_mapped_write: page cache may be ahead */
	/* symlink */
	char               *rn_target;
	struct ramfs_node  *rn_hnext;           /* the next node in rm_hash's chain */
	TAILQ_HEAD(, ramfs_xattr) rn_xattrs;
};
#define RAMFS_IF_SYNCROOT       0x1         /* a FileProvider sync root (APFS fsctl 'J' 72) */
#define RAMFS_N_CREATING        0x1         /* a thread is in vnode_create() for this node */
#define RAMFS_N_WANTED          0x2         /* somebody sleeps on the node */

struct ramfs_mount {
	mount_t             rm_mp;
	lck_mtx_t           rm_lock, rm_rename_lock;
	struct ramfs_node  *rm_root;
	uint64_t            rm_next_ino, rm_nodes, rm_pages, rm_max_pages; /* rm_pages: file pages in use */
	uint32_t            rm_next_docid;
	struct ramfs_node **rm_hash;            /* every node by its number, for vfs_vget */
	bool                rm_disk_backed, rm_disk_loading, rm_meta_dirty, rm_meta_syncing, rm_seed_done;
	uint32_t            rm_bmap_words, rm_bmap_max, rm_bmap_next;
	uint32_t           *rm_bmap;
	void               *rm_io_page, *rm_meta_buf, *rm_meta_call;
	size_t              rm_meta_cap;
	uint64_t            rm_meta_gen;
	uint32_t            rm_meta_slot;
};

#define RAMFS_HASH_SIZE         4096u       /* chains, a power of two */

/* file pages in use on all the mounts together (each mount has its own lock): for the kernel's memory report */
_Atomic uint64_t ramfs_pages_in_use;
_Atomic uint64_t ramfs_var_used_pages;

#define RAMFS_MP(mp)    ((struct ramfs_mount *)vfs_fsprivate(mp))
#define RAMFS_NODE(vp)  ((struct ramfs_node *)vnode_fsnode(vp))

static LCK_GRP_DECLARE(ramfs_lck_grp, "ramfs");

extern int(**ramfs_vnodeop_p)(void *);

/* ------------------------------------------------------------------------ */
/* Nodes                                                                    */

static struct ramfs_node *
ramfs_node_alloc(struct ramfs_mount *rm, enum vtype type, struct vnode_attr *vap)
{
	struct ramfs_node *node = kalloc_type(struct ramfs_node, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	struct timespec now;

	nanotime(&now);
	node->rn_mount = rm;
	node->rn_type = type;
	node->rn_mode = type == VLNK ? 0755 : (type == VDIR ? 0755 : 0644);
	node->rn_dataprotect_class = PROTECTION_CLASS_D;
	node->rn_atime = node->rn_mtime = node->rn_ctime = node->rn_btime = now;
	TAILQ_INIT(&node->rn_entries);
	TAILQ_INIT(&node->rn_xattrs);

	if (vap != NULL) {
		if (VATTR_IS_ACTIVE(vap, va_mode)) {
			node->rn_mode = vap->va_mode & ALLPERMS;
			VATTR_SET_SUPPORTED(vap, va_mode);
		}
		if (VATTR_IS_ACTIVE(vap, va_uid)) {
			node->rn_uid = vap->va_uid;
			VATTR_SET_SUPPORTED(vap, va_uid);
		}
		if (VATTR_IS_ACTIVE(vap, va_gid)) {
			node->rn_gid = vap->va_gid;
			VATTR_SET_SUPPORTED(vap, va_gid);
		}
		if (VATTR_IS_ACTIVE(vap, va_flags)) {
			node->rn_bsdflags = vap->va_flags;
			VATTR_SET_SUPPORTED(vap, va_flags);
		}
		if (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {
			if (vap->va_dataprotect_class != PROTECTION_CLASS_DEFAULT)
				node->rn_dataprotect_class = vap->va_dataprotect_class;
			VATTR_SET_SUPPORTED(vap, va_dataprotect_class);
		}
		if (VATTR_IS_ACTIVE(vap, va_create_time)) {
			node->rn_btime = vap->va_create_time;
			VATTR_SET_SUPPORTED(vap, va_create_time);
		}
		if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
			node->rn_mtime = vap->va_modify_time;
			VATTR_SET_SUPPORTED(vap, va_modify_time);
		}
		if (VATTR_IS_ACTIVE(vap, va_access_time)) {
			node->rn_atime = vap->va_access_time;
			VATTR_SET_SUPPORTED(vap, va_access_time);
		}
		if (VATTR_IS_ACTIVE(vap, va_change_time)) {
			node->rn_ctime = vap->va_change_time;
			VATTR_SET_SUPPORTED(vap, va_change_time);
		}
		VATTR_SET_SUPPORTED(vap, va_type);
	}
	return node;
}

/*
 * A page of a file's contents, and giving it back. Not from a kalloc zone, as it once was: all the
 * kernel's zones share one ceiling, and a few hundred MB of files in data.kalloc.16384 met it --
 * first writes failing with ENOSPC ("there isn't enough space" while most of this filesystem's own
 * allowance was free: dpkg's database could not be written), then, on an allocation that may not
 * fail, the kernel panicked with the zone map exhausted. Straight from the VM instead, a wired page
 * of kernel_map each, as the contents of a RAM disk are. wait=false: never wait for memory (the
 * page-out path, which may be the pageout daemon looking for some).
 */
static void *
ramfs_page_alloc(bool wait)
{
	vm_offset_t addr = 0;
	kma_flags_t flags = KMA_KOBJECT | KMA_DATA | KMA_ZERO | (wait ? KMA_NONE : KMA_NOPAGEWAIT);

	return kmem_alloc(kernel_map, &addr, PAGE_SIZE, flags, VM_KERN_MEMORY_FILE) == KERN_SUCCESS ?
	       (void *)addr : NULL;
}

static void
ramfs_page_free(void *page)
{
	kmem_free(kernel_map, (vm_offset_t)page, PAGE_SIZE);
}

/*
 * Make the page array big enough for page `index`, before the mount lock is taken: under it the
 * array can only grow by an allocation that does not wait (ramfs_grow_slots), and one that failed
 * under memory pressure failed the write with ENOSPC. Nothing is promised: ramfs_store looks again.
 */
static void
ramfs_reserve_slots(struct ramfs_mount *rm, struct ramfs_node *node, uint64_t index)
{
	for (int tries = 0; tries < 4; tries++) {
		uint64_t have, want;
		void **slots;
		uint32_t *blks;

		lck_mtx_lock(&rm->rm_lock);
		have = node->rn_nslots;
		lck_mtx_unlock(&rm->rm_lock);
		if (index < have) {
			return;
		}
		for (want = have ? have : 4; want <= index; want *= 2) {
		}
		slots = kalloc_type(void *, want, Z_WAITOK | Z_ZERO);
		blks = kalloc_data(want * sizeof(uint32_t), Z_WAITOK | Z_ZERO);
		if (slots == NULL || blks == NULL) {
			if (slots != NULL) {
				kfree_type(void *, want, slots);
			}
			if (blks != NULL) {
				kfree_data(blks, want * sizeof(uint32_t));
			}
			return;
		}
		lck_mtx_lock(&rm->rm_lock);
		if (node->rn_nslots == have) {
			if (node->rn_pages != NULL) {
				memcpy(slots, node->rn_pages, have * sizeof(void *));
				kfree_type(void *, have, node->rn_pages);
			}
			if (node->rn_blocks != NULL) {
				memcpy(blks, node->rn_blocks, have * sizeof(uint32_t));
				kfree_data(node->rn_blocks, have * sizeof(uint32_t));
			}
			node->rn_pages = slots;
			node->rn_blocks = blks;
			node->rn_nslots = want;
			slots = NULL;
			blks = NULL;
		}
		lck_mtx_unlock(&rm->rm_lock);
		if (slots == NULL) {
			return;
		}
		kfree_type(void *, want, slots);
		kfree_data(blks, want * sizeof(uint32_t));
	}
}

static void
ramfs_hash_insert(struct ramfs_mount *rm, struct ramfs_node *node)
{
	struct ramfs_node **chain = &rm->rm_hash[node->rn_ino & (RAMFS_HASH_SIZE - 1)];

	node->rn_hnext = *chain;
	*chain = node;
	node->rn_hashed = true;
}

static void
ramfs_hash_remove(struct ramfs_mount *rm, struct ramfs_node *node)
{
	struct ramfs_node **link = &rm->rm_hash[node->rn_ino & (RAMFS_HASH_SIZE - 1)];

	while (*link != NULL && *link != node) {
		link = &(*link)->rn_hnext;
	}
	if (*link == node) {
		*link = node->rn_hnext;
	}
	node->rn_hashed = false;
}

static struct ramfs_dirent *ramfs_dirent_alloc(const char *name, size_t len);
static void ramfs_dirent_free(struct ramfs_dirent *de);
static void ramfs_node_free(struct ramfs_node *node);
#include "ramfs_disk.inc"

/* Drop file pages from slot `from` on. The mount lock is held. */
static void
ramfs_free_pages(struct ramfs_node *node, uint64_t from)
{
	struct ramfs_mount *rm = node->rn_mount;

	for (uint64_t i = from; i < node->rn_nslots; i++) {
		bool had_ram = node->rn_pages != NULL && node->rn_pages[i] != NULL;
		bool had_blk = node->rn_blocks != NULL && node->rn_blocks[i] != 0;

		if (had_ram) {
			ramfs_page_free(node->rn_pages[i]);
			node->rn_pages[i] = NULL;
			os_atomic_dec(&ramfs_pages_in_use, relaxed);
		}
		if (had_blk) {
			ramfs_disk_bmap_free(rm, node->rn_blocks[i]);
			node->rn_blocks[i] = 0;
			os_atomic_dec(&ramfs_var_used_pages, relaxed);
		}
		if (had_ram || had_blk) {
			node->rn_nalloc--;
			rm->rm_pages--;
		}
	}
}

static void
ramfs_xattr_free(struct ramfs_xattr *xa)
{
	kfree_data(xa->rx_name, xa->rx_namelen + 1);
	if (xa->rx_data != NULL) {
		kfree_data(xa->rx_data, xa->rx_size);
	}
	kfree_type(struct ramfs_xattr, xa);
}

/* The node is named by nothing and has no vnode. The mount lock is held. */
static void
ramfs_node_free(struct ramfs_node *node)
{
	struct ramfs_xattr *xa;

	if (node->rn_hashed) {
		ramfs_hash_remove(node->rn_mount, node);
	}

	assert(TAILQ_EMPTY(&node->rn_entries));
	while ((xa = TAILQ_FIRST(&node->rn_xattrs)) != NULL) {
		TAILQ_REMOVE(&node->rn_xattrs, xa, rx_link);
		ramfs_xattr_free(xa);
	}
	ramfs_free_pages(node, 0);
	if (node->rn_pages != NULL) {
		kfree_type(void *, node->rn_nslots, node->rn_pages);
	}
	if (node->rn_blocks != NULL) {
		kfree_data(node->rn_blocks, node->rn_nslots * sizeof(uint32_t));
	}
	if (node->rn_target != NULL) {
		kfree_data(node->rn_target, node->rn_size + 1);
	}
	node->rn_mount->rm_nodes--;
	kfree_type(struct ramfs_node, node);
}

static void
ramfs_touch(struct ramfs_node *node, bool modified)
{
	struct timespec now;

	nanotime(&now);
	node->rn_ctime = now;
	if (modified) {
		node->rn_mtime = now;
		/* the write generation count, as APFS and HFS keep it: every content change, skipping 0 on wrap */
		if (node->rn_type != VDIR && ++node->rn_write_gen == 0) {
			node->rn_write_gen = 1;
		}
	}
	ramfs_disk_dirty_locked(node->rn_mount);
}

/*
 * APFS and HFS give a file or directory a document ID the first time UF_TRACKED is set on it: unique on
 * the volume and kept with the node. revisiond's GSLibraryGetOrAllocateDocumentId sets UF_TRACKED and
 * reads ATTR_CMN_DOCUMENT_ID back, and FileProvider's LocalStorage domain does that for every item it
 * creates. Called with rm_lock held, after the flags changed.
 */
static void
ramfs_track(struct ramfs_node *node)
{
	if ((node->rn_bsdflags & UF_TRACKED) && node->rn_docid == 0) {
		node->rn_docid = node->rn_mount->rm_next_docid++;
	}
}

static struct ramfs_dirent *
ramfs_dirent_alloc(const char *name, size_t len)
{
	struct ramfs_dirent *de = kalloc_type(struct ramfs_dirent, Z_WAITOK | Z_ZERO | Z_NOFAIL);

	de->rd_name = kalloc_data(len + 1, Z_WAITOK | Z_NOFAIL);
	memcpy(de->rd_name, name, len);
	de->rd_name[len] = '\0';
	de->rd_namelen = (uint32_t)len;
	return de;
}

static void
ramfs_dirent_free(struct ramfs_dirent *de)
{
	kfree_data(de->rd_name, de->rd_namelen + 1);
	kfree_type(struct ramfs_dirent, de);
}

static struct ramfs_dirent *
ramfs_dir_find(struct ramfs_node *dir, const char *name, size_t len)
{
	struct ramfs_dirent *de;

	TAILQ_FOREACH(de, &dir->rn_entries, rd_link) {
		if (de->rd_namelen == len && memcmp(de->rd_name, name, len) == 0) {
			return de;
		}
	}
	return NULL;
}

static void
ramfs_dir_add(struct ramfs_node *dir, struct ramfs_dirent *de, struct ramfs_node *node)
{
	de->rd_node = node;
	TAILQ_INSERT_TAIL(&dir->rn_entries, de, rd_link);
	dir->rn_nentries++;
	ramfs_touch(dir, true);
}

static void
ramfs_dir_remove(struct ramfs_node *dir, struct ramfs_dirent *de)
{
	TAILQ_REMOVE(&dir->rn_entries, de, rd_link);
	dir->rn_nentries--;
	ramfs_touch(dir, true);
	ramfs_dirent_free(de);
}

/*
 * Get the vnode of a node that is known to exist (it was just found in a
 * directory, or is the root). Called with the mount lock held; returns with
 * it dropped. 0: *vpp carries an iocount. EAGAIN: the node could not be
 * pinned (its vnode is on the way out, or another thread is creating one)
 * and may be gone by now; the caller must find it again.
 */
static int
ramfs_vget_locked(struct ramfs_mount *rm, struct ramfs_node *node, vnode_t dvp, struct componentname *cnp, vnode_t *vpp)
{
	struct vnode_fsparam param;
	vnode_t vp;
	int error;

	if (node->rn_state & RAMFS_N_CREATING) {
		node->rn_state |= RAMFS_N_WANTED;
		msleep(node, &rm->rm_lock, PINOD | PDROP, "ramfs_create", NULL);
		return EAGAIN;
	}
	if (node->rn_vp != NULLVP) {
		uint32_t vid = node->rn_vid;

		vp = node->rn_vp;
		lck_mtx_unlock(&rm->rm_lock);
		if (vnode_getwithvid(vp, vid) == 0) {
			*vpp = vp;
			return 0;
		}
		/* being reclaimed: give it a moment */
		tsleep(rm, PINOD, "ramfs_reclaim", 1);
		return EAGAIN;
	}

	/* The CREATING mark keeps the node in place: nobody can unlink what they cannot get a vnode for. */
	node->rn_state |= RAMFS_N_CREATING;
	lck_mtx_unlock(&rm->rm_lock);

	bzero(&param, sizeof(param));
	param.vnfs_mp = rm->rm_mp;
	param.vnfs_vtype = node->rn_type;
	param.vnfs_str = "ramfs";
	param.vnfs_dvp = dvp;
	param.vnfs_fsnode = node;
	param.vnfs_vops = ramfs_vnodeop_p;
	param.vnfs_markroot = (node == rm->rm_root);
	param.vnfs_filesize = node->rn_type == VREG ? (off_t)node->rn_size : 0;
	param.vnfs_cnp = cnp;
	/* names change under rename and unlink at any time: keep them out of the name cache */
	param.vnfs_flags = VNFS_NOCACHE | VNFS_CANTCACHE;
	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &param, &vp);

	lck_mtx_lock(&rm->rm_lock);
	if (!error) {
		node->rn_vp = vp;
		node->rn_vid = vnode_vid(vp);
	}
	if (node->rn_state & RAMFS_N_WANTED) {
		wakeup(node);
	}
	node->rn_state &= ~(RAMFS_N_CREATING | RAMFS_N_WANTED);
	lck_mtx_unlock(&rm->rm_lock);

	if (!error) {
		*vpp = vp;
	}
	return error;
}

/* ------------------------------------------------------------------------ */
/* File contents                                                            */

/* Make sure the page array has a slot for page `index`. The mount lock is held. */
static int
ramfs_grow_slots(struct ramfs_node *node, uint64_t index)
{
	uint64_t want = node->rn_nslots ? node->rn_nslots : 4;
	void **slots;
	uint32_t *blks;

	if (index < node->rn_nslots) {
		return 0;
	}
	while (want <= index) {
		want *= 2;
	}
	slots = kalloc_type(void *, want, Z_NOWAIT | Z_ZERO);
	blks = kalloc_data(want * sizeof(uint32_t), Z_NOWAIT | Z_ZERO);
	if (slots == NULL || blks == NULL) {
		if (slots != NULL) {
			kfree_type(void *, want, slots);
		}
		if (blks != NULL) {
			kfree_data(blks, want * sizeof(uint32_t));
		}
		return ENOSPC;
	}
	if (node->rn_pages != NULL) {
		memcpy(slots, node->rn_pages, node->rn_nslots * sizeof(void *));
		kfree_type(void *, node->rn_nslots, node->rn_pages);
	}
	if (node->rn_blocks != NULL) {
		memcpy(blks, node->rn_blocks, node->rn_nslots * sizeof(uint32_t));
		kfree_data(node->rn_blocks, node->rn_nslots * sizeof(uint32_t));
	}
	node->rn_pages = slots;
	node->rn_blocks = blks;
	node->rn_nslots = want;
	return 0;
}

#include "ramfs_store.inc"

/* Copy len bytes (within one page) out of the file; holes read as zeros. The mount lock is held. */
static void
ramfs_fetch(struct ramfs_node *node, uint64_t off, void *dst, size_t len)
{
	uint64_t index = off / PAGE_SIZE;

	if (index < node->rn_nslots && node->rn_pages[index] == NULL &&
	    node->rn_blocks != NULL && node->rn_blocks[index] != 0) {
		ramfs_disk_fault_page(node, index);
		if (node->rn_pages[index] == NULL && node->rn_mount->rm_io_page != NULL &&
		    ramfs_disk_read_block(node->rn_mount, node->rn_blocks[index], node->rn_mount->rm_io_page) == 0) {
			memcpy(dst, (char *)node->rn_mount->rm_io_page + (off & PAGE_MASK), len);
			return;
		}
	}
	if (index < node->rn_nslots && node->rn_pages[index] != NULL) {
		memcpy(dst, (char *)node->rn_pages[index] + (off & PAGE_MASK), len);
	} else {
		bzero(dst, len);
	}
}

/* Change the file size; shrinking frees pages and clears the tail of the last one. The mount lock is held. */
static void
ramfs_resize(struct ramfs_node *node, uint64_t size)
{
	if (size < node->rn_size) {
		uint64_t keep = (size + PAGE_MASK) / PAGE_SIZE;

		ramfs_free_pages(node, keep);
		if ((size & PAGE_MASK) != 0 && keep - 1 < node->rn_nslots) {
			if (node->rn_pages[keep - 1] == NULL && node->rn_blocks != NULL && node->rn_blocks[keep - 1] != 0) {
				ramfs_disk_fault_page(node, keep - 1);
			}
			if (node->rn_pages[keep - 1] != NULL) {
				bzero((char *)node->rn_pages[keep - 1] + (size & PAGE_MASK), PAGE_SIZE - (size & PAGE_MASK));
				if (node->rn_mount->rm_disk_backed && node->rn_blocks != NULL && node->rn_blocks[keep - 1] != 0) {
					(void)ramfs_disk_write_block(node->rn_mount, node->rn_blocks[keep - 1],
					    node->rn_pages[keep - 1]);
				}
			}
		}
	}
	node->rn_size = size;
	ramfs_touch(node, true);
}

/*
 * What a write() has changed, into the pages of the page cache that hold it, where there are such
 * pages: each is filled anew from the file, in place. Throwing them away instead, for the next
 * page-in to fetch again, is not enough: the page cache cannot throw away a page that somebody
 * holds wired, and that page goes on showing the file as it was. A surface made around memory that
 * maps a file wires the pages under it (MiniIOSurface.cpp; CoreAnimation's window server makes one
 * around every picture it shows, and the pictures of the home screen's icons are memory that maps
 * IconServices' cache, a file here). When SpringBoard wrote the Calendar's new icon into that
 * cache, the pages of the icon that it shares with the icons next to it were still held by those
 * icons' surfaces: they kept the old icon, in the top 16 rows and the bottom 4 of the new one, and
 * SpringBoard's own mapping of the cache, being of those very pages, showed the same.
 */
static void
ramfs_refresh_cache(vnode_t vp, struct ramfs_node *node, uint64_t start, uint64_t end)
{
	struct ramfs_mount *rm = node->rn_mount;

	for (uint64_t page = trunc_page_64(start); page < end; page += PAGE_SIZE) {
		upl_t upl = NULL;
		upl_page_info_t *pl = NULL;
		vm_offset_t addr;
		int flags = 0;

		if (ubc_page_op(vp, (off_t)page, 0, NULL, &flags) != KERN_SUCCESS) {
			continue;                       /* not in the page cache: the next page-in fetches it */
		}
		if (ubc_create_upl_kernel(vp, (off_t)page, PAGE_SIZE, &upl, &pl, UPL_SET_LITE | UPL_WILL_MODIFY,
		    VM_KERN_MEMORY_FILE) != KERN_SUCCESS || upl == NULL) {
			continue;
		}
		if (!upl_page_present(pl, 0) || ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
			/* (gone meanwhile: the page the request made is thrown away again) */
			ubc_upl_abort_range(upl, 0, PAGE_SIZE, UPL_ABORT_FREE_ON_EMPTY);
			continue;
		}
		lck_mtx_lock(&rm->rm_lock);
		if (page < node->rn_size) {
			size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, node->rn_size - page);

			ramfs_fetch(node, page, (void *)addr, len);
			bzero((void *)(addr + len), PAGE_SIZE - len);
		} else {
			bzero((void *)addr, PAGE_SIZE);
		}
		lck_mtx_unlock(&rm->rm_lock);
		ubc_upl_unmap(upl);
		ubc_upl_commit_range(upl, 0, PAGE_SIZE, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
	}
}

/* A mapping may hold changes that have not been paged out to us yet: get them. */
static int
ramfs_sync_mapping(vnode_t vp, struct ramfs_node *node)
{
	if (node->rn_mapped_write) {
		return ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | UBC_SYNC);
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Vnode operations                                                         */

#include "ramfs_vnops.inc"

#include "ramfs_xattr.inc"

static int
ramfs_blktooff(struct vnop_blktooff_args *ap)
{
	*ap->a_offset = (off_t)ap->a_lblkno * PAGE_SIZE;
	return 0;
}

static int
ramfs_offtoblk(struct vnop_offtoblk_args *ap)
{
	*ap->a_lblkno = (daddr64_t)(ap->a_offset / PAGE_SIZE);
	return 0;
}

#define VOPFUNC int (*)(void *)

#include "ramfs_clonefile.inc"

int(**ramfs_vnodeop_p)(void *);
const struct vnodeopv_entry_desc ramfs_vnodeop_entries[] = {
	{ .opve_op = &vnop_default_desc, .opve_impl = (VOPFUNC)vn_default_error },
	{ .opve_op = &vnop_lookup_desc, .opve_impl = (VOPFUNC)ramfs_lookup },
	{ .opve_op = &vnop_create_desc, .opve_impl = (VOPFUNC)ramfs_create },
	{ .opve_op = &vnop_mknod_desc, .opve_impl = (VOPFUNC)ramfs_mknod },
	{ .opve_op = &vnop_open_desc, .opve_impl = (VOPFUNC)ramfs_open },
	{ .opve_op = &vnop_close_desc, .opve_impl = (VOPFUNC)ramfs_close },
	{ .opve_op = &vnop_getattr_desc, .opve_impl = (VOPFUNC)ramfs_getattr },
	{ .opve_op = &vnop_setattr_desc, .opve_impl = (VOPFUNC)ramfs_setattr },
	{ .opve_op = &vnop_read_desc, .opve_impl = (VOPFUNC)ramfs_read },
	{ .opve_op = &vnop_write_desc, .opve_impl = (VOPFUNC)ramfs_write },
	{ .opve_op = &vnop_ioctl_desc, .opve_impl = (VOPFUNC)ramfs_ioctl },
	{ .opve_op = &vnop_fsync_desc, .opve_impl = (VOPFUNC)ramfs_fsync },
	{ .opve_op = &vnop_remove_desc, .opve_impl = (VOPFUNC)ramfs_remove },
	{ .opve_op = &vnop_link_desc, .opve_impl = (VOPFUNC)ramfs_link },
	{ .opve_op = &vnop_rename_desc, .opve_impl = (VOPFUNC)ramfs_rename },
	{ .opve_op = &vnop_renamex_desc, .opve_impl = (VOPFUNC)ramfs_renamex },
	{ .opve_op = &vnop_mkdir_desc, .opve_impl = (VOPFUNC)ramfs_mkdir },
	{ .opve_op = &vnop_rmdir_desc, .opve_impl = (VOPFUNC)ramfs_rmdir },
	{ .opve_op = &vnop_symlink_desc, .opve_impl = (VOPFUNC)ramfs_symlink },
	{ .opve_op = &vnop_readdir_desc, .opve_impl = (VOPFUNC)ramfs_readdir },
	{ .opve_op = &vnop_readlink_desc, .opve_impl = (VOPFUNC)ramfs_readlink },
	{ .opve_op = &vnop_inactive_desc, .opve_impl = (VOPFUNC)ramfs_inactive },
	{ .opve_op = &vnop_reclaim_desc, .opve_impl = (VOPFUNC)ramfs_reclaim },
	{ .opve_op = &vnop_pathconf_desc, .opve_impl = (VOPFUNC)ramfs_pathconf },
	{ .opve_op = &vnop_mmap_desc, .opve_impl = (VOPFUNC)ramfs_mmap },
	{ .opve_op = &vnop_mnomap_desc, .opve_impl = (VOPFUNC)ramfs_mnomap },
	{ .opve_op = &vnop_pagein_desc, .opve_impl = (VOPFUNC)ramfs_pagein },
	{ .opve_op = &vnop_pageout_desc, .opve_impl = (VOPFUNC)ramfs_pageout },
	{ .opve_op = &vnop_getxattr_desc, .opve_impl = (VOPFUNC)ramfs_getxattr },
	{ .opve_op = &vnop_setxattr_desc, .opve_impl = (VOPFUNC)ramfs_setxattr },
	{ .opve_op = &vnop_removexattr_desc, .opve_impl = (VOPFUNC)ramfs_removexattr },
	{ .opve_op = &vnop_listxattr_desc, .opve_impl = (VOPFUNC)ramfs_listxattr },
	{ .opve_op = &vnop_blktooff_desc, .opve_impl = (VOPFUNC)ramfs_blktooff },
	{ .opve_op = &vnop_offtoblk_desc, .opve_impl = (VOPFUNC)ramfs_offtoblk },
	{ .opve_op = &vnop_clonefile_desc, .opve_impl = (VOPFUNC)ramfs_clonefile },
	{ .opve_op = (struct vnodeop_desc *)NULL, .opve_impl = (VOPFUNC)NULL }
};
const struct vnodeopv_desc ramfs_vnodeop_opv_desc = {
	.opv_desc_vector_p = &ramfs_vnodeop_p,
	.opv_desc_ops = ramfs_vnodeop_entries
};

/* ------------------------------------------------------------------------ */
/* Mount operations                                                         */

#include "ramfs_getattr.inc"

/* mount(2) with no arguments: an empty filesystem appears. */
static int
ramfs_mount(mount_t mp, __unused vnode_t devvp, __unused user_addr_t data, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm;
	struct vfsstatfs *sp;

	if (vfs_isupdate(mp)) {
		return 0;
	}
	rm = kalloc_type(struct ramfs_mount, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	lck_mtx_init(&rm->rm_lock, &ramfs_lck_grp, LCK_ATTR_NULL);
	lck_mtx_init(&rm->rm_rename_lock, &ramfs_lck_grp, LCK_ATTR_NULL);
	rm->rm_mp = mp;
	rm->rm_next_ino = RAMFS_ROOT_INO + 1;
	rm->rm_next_docid = 1;
	/* a runaway writer must not be able to take the whole machine: a quarter of memory at most */
	rm->rm_max_pages = (max_mem / 4) / PAGE_SIZE;

	rm->rm_hash = kalloc_type(struct ramfs_node *, RAMFS_HASH_SIZE, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	rm->rm_root = ramfs_node_alloc(rm, VDIR, NULL);
	rm->rm_root->rn_ino = RAMFS_ROOT_INO;
	ramfs_hash_insert(rm, rm->rm_root);
	rm->rm_root->rn_nlink = 2;
	rm->rm_root->rn_parent = rm->rm_root;
	rm->rm_nodes = 1;

	vfs_setfsprivate(mp, rm);
	vfs_setlocklocal(mp);
	/* ACLs and owner UUIDs: VFS keeps them in an extended attribute of the file */
	vfs_setextendedsecurity(mp);
	vfs_getnewfsid(mp);

	if (strcmp(vfs_statfs(mp)->f_mntonname, "/private/var") == 0) {
		char root_apfs[64] = { 0 };
		bool required = PE_parse_boot_argn("root_apfs", root_apfs, sizeof(root_apfs)) &&
		    strcmp(root_apfs, "ufs_darwin") == 0;
		int error = ramfs_disk_mount(rm);
		if (error != 0 && !(error == ENXIO && !required)) {
			/* Mount failure owns no vnode and must not invoke the syncing unmount path. */
			ramfs_disk_clear_nodes(rm);
			vfs_setfsprivate(mp, NULL);
			lck_mtx_destroy(&rm->rm_rename_lock, &ramfs_lck_grp);
			lck_mtx_destroy(&rm->rm_lock, &ramfs_lck_grp);
			kfree_type(struct ramfs_node *, RAMFS_HASH_SIZE, rm->rm_hash);
			kfree_type(struct ramfs_mount, rm);
			return error;
		}
	}
	vfs_setfstypename(mp, "apfs");
	sp = vfs_statfs(mp);
	ramfs_fill_statfs(rm, sp);
	return 0;
}

/* Free a directory's subtree; every vnode is gone, so nothing else can be looking. */
static void
ramfs_free_tree(struct ramfs_node *dir)
{
	struct ramfs_dirent *de;

	while ((de = TAILQ_FIRST(&dir->rn_entries)) != NULL) {
		struct ramfs_node *node = de->rd_node;

		TAILQ_REMOVE(&dir->rn_entries, de, rd_link);
		ramfs_dirent_free(de);
		if (node->rn_type == VDIR) {
			ramfs_free_tree(node);
			ramfs_node_free(node);
		} else if (--node->rn_nlink == 0) {
			ramfs_node_free(node);
		}
	}
}

static int
ramfs_unmount(mount_t mp, int mntflags, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm = RAMFS_MP(mp);
	int error = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE : 0);

	if (error) {
		return error;
	}
	error = ramfs_disk_unmount(rm);
	if (error) {
		return error;
	}
	lck_mtx_lock(&rm->rm_lock);
	ramfs_free_tree(rm->rm_root);
	ramfs_node_free(rm->rm_root);
	lck_mtx_unlock(&rm->rm_lock);

	vfs_setfsprivate(mp, NULL);
	lck_mtx_destroy(&rm->rm_rename_lock, &ramfs_lck_grp);
	lck_mtx_destroy(&rm->rm_lock, &ramfs_lck_grp);
	kfree_type(struct ramfs_node *, RAMFS_HASH_SIZE, rm->rm_hash);
	kfree_type(struct ramfs_mount, rm);
	return 0;
}

static int
ramfs_root(mount_t mp, vnode_t *vpp, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm = RAMFS_MP(mp);
	int error;

	do {
		lck_mtx_lock(&rm->rm_lock);
		error = ramfs_vget_locked(rm, rm->rm_root, NULLVP, NULL, vpp);
	} while (error == EAGAIN);
	return error;
}

/* A node by its number (fsgetpath, and whoever else asks by file ID): see ramfs_hash_insert(). */
static int
ramfs_vfs_vget(mount_t mp, ino64_t ino, vnode_t *vpp, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm = RAMFS_MP(mp);
	int error;

	do {
		struct ramfs_node *node;

		lck_mtx_lock(&rm->rm_lock);
		for (node = rm->rm_hash[ino & (RAMFS_HASH_SIZE - 1)]; node != NULL && node->rn_ino != ino;
		    node = node->rn_hnext) {
		}
		if (node == NULL || node->rn_nlink == 0) {
			lck_mtx_unlock(&rm->rm_lock);
			return ENOENT;
		}
		error = ramfs_vget_locked(rm, node, NULLVP, NULL, vpp);
	} while (error == EAGAIN);
	return error;
}

static int
ramfs_sync(mount_t mp, __unused int waitfor, __unused vfs_context_t ctx)
{
	return ramfs_disk_sync(RAMFS_MP(mp));
}

static int
ramfs_init(__unused struct vfsconf *vfsc)
{
	return 0;
}

struct vfsops ramfs_vfsops = {
	.vfs_mount = ramfs_mount,
	.vfs_unmount = ramfs_unmount,
	.vfs_root = ramfs_root,
	.vfs_vget = ramfs_vfs_vget,
	.vfs_getattr = ramfs_vfs_getattr,
	.vfs_sync = ramfs_sync,
	.vfs_init = ramfs_init,
};

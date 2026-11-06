/*
 * ramfs: a read-write filesystem that lives entirely in kernel memory.
 *
 * The open-source kernel has no writable filesystem of its own, and the
 * bring-up system has no storage driver, so /private/var and /tmp are kept
 * here. Everything is lost at shutdown.
 *
 * Nodes (files, directories, symlinks, socket names) are plain kernel
 * structures; a directory is a list of (name, node) entries, so hard links
 * are just two entries naming one node. File contents are kept in
 * individually allocated pages hanging off the node. That copy is the truth:
 * read() and write() go straight to it, and the page cache is used only for
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
#define RAMFS_N_CREATING        0x1         /* a thread is in vnode_create() for this node */
#define RAMFS_N_WANTED          0x2         /* somebody sleeps on the node */

struct ramfs_mount {
	mount_t             rm_mp;
	lck_mtx_t           rm_lock, rm_rename_lock;
	struct ramfs_node  *rm_root;
	uint64_t            rm_next_ino, rm_nodes, rm_pages, rm_max_pages; /* rm_pages: file pages in use */
	struct ramfs_node **rm_hash;            /* every node by its number, for vfs_vget */
	bool                rm_disk_backed, rm_disk_loading, rm_meta_dirty, rm_meta_syncing, rm_seed_done;
	uint32_t            rm_bmap_words, rm_bmap_max, rm_bmap_next;
	uint32_t           *rm_bmap;
	void               *rm_io_page, *rm_meta_buf, *rm_meta_call;
	size_t              rm_meta_cap;
	uint64_t            rm_meta_gen;
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
	}
	ramfs_disk_dirty_locked(node->rn_mount);
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
static void
ramfs_sync_mapping(vnode_t vp, struct ramfs_node *node)
{
	if (node->rn_mapped_write) {
		(void)ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | UBC_SYNC);
	}
}

/* ------------------------------------------------------------------------ */
/* Vnode operations                                                         */

static int
ramfs_lookup(struct vnop_lookup_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct ramfs_node *dir = RAMFS_NODE(dvp);
	struct ramfs_mount *rm = dir->rn_mount;
	int error;

	*ap->a_vpp = NULLVP;
	if (dir->rn_type != VDIR) {
		return ENOTDIR;
	}
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		error = vnode_get(dvp);
		if (!error) {
			*ap->a_vpp = dvp;
		}
		return error;
	}

	do {
		struct ramfs_node *target;
		bool dotdot = (cnp->cn_flags & ISDOTDOT) != 0;

		lck_mtx_lock(&rm->rm_lock);
		if (dotdot) {
			target = dir->rn_parent;
		} else {
			struct ramfs_dirent *de = ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen);

			target = de ? de->rd_node : NULL;
		}
		if (target == NULL) {
			lck_mtx_unlock(&rm->rm_lock);
			/* a name that may be created: tell VFS to go ahead (unless the directory itself is gone) */
			if (!dotdot && dir->rn_nlink != 0 && (cnp->cn_flags & ISLASTCN) &&
			    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)) {
				return EJUSTRETURN;
			}
			return ENOENT;
		}
		error = ramfs_vget_locked(rm, target, dotdot ? NULLVP : dvp, dotdot ? NULL : cnp, ap->a_vpp);
	} while (error == EAGAIN);
	return error;
}

/* create, mkdir, symlink and mknod are one operation with different node types */
static int
ramfs_make(vnode_t dvp, vnode_t *vpp, struct componentname *cnp, struct vnode_attr *vap, enum vtype type, const char *target)
{
	struct ramfs_node *dir = RAMFS_NODE(dvp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_node *node;
	struct ramfs_dirent *de;
	int error;

	if (cnp->cn_namelen > RAMFS_MAX_NAME) {
		return ENAMETOOLONG;
	}
	node = ramfs_node_alloc(rm, type, vap);
	de = ramfs_dirent_alloc(cnp->cn_nameptr, (size_t)cnp->cn_namelen);
	if (type == VLNK) {
		node->rn_size = strlen(target);
		node->rn_target = kalloc_data(node->rn_size + 1, Z_WAITOK | Z_NOFAIL);
		memcpy(node->rn_target, target, node->rn_size + 1);
	}

	lck_mtx_lock(&rm->rm_lock);
	rm->rm_nodes++;
	if (dir->rn_nlink == 0) {
		error = ENOENT;         /* the directory has been removed */
	} else if (ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen) != NULL) {
		error = EEXIST;
	} else {
		error = 0;
	}
	if (error) {
		ramfs_node_free(node);
		lck_mtx_unlock(&rm->rm_lock);
		ramfs_dirent_free(de);
		return error;
	}
	node->rn_ino = rm->rm_next_ino++;
	if ((vap == NULL || !VATTR_IS_ACTIVE(vap, va_dataprotect_class) ||
	    vap->va_dataprotect_class == PROTECTION_CLASS_DEFAULT) && dir->rn_dataprotect_class != 0)
		node->rn_dataprotect_class = dir->rn_dataprotect_class;
	ramfs_hash_insert(rm, node);
	if (type == VDIR) {
		node->rn_nlink = 2;
		node->rn_parent = dir;
		dir->rn_nlink++;
	} else {
		node->rn_nlink = 1;
	}
	ramfs_dir_add(dir, de, node);

	/* a fresh node has no vnode and no other suitor: this cannot ask for a retry */
	error = ramfs_vget_locked(rm, node, dvp, cnp, vpp);
	if (error) {
		lck_mtx_lock(&rm->rm_lock);
		ramfs_dir_remove(dir, de);
		if (type == VDIR) {
			dir->rn_nlink--;
		}
		ramfs_node_free(node);
		lck_mtx_unlock(&rm->rm_lock);
	}
	return error;
}
static int
ramfs_create(struct vnop_create_args *ap)
{
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VREG, NULL);
}
static int
ramfs_mkdir(struct vnop_mkdir_args *ap)
{
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VDIR, NULL);
}
static int
ramfs_symlink(struct vnop_symlink_args *ap)
{
	if (strlen(ap->a_target) >= MAXPATHLEN) {
		return ENAMETOOLONG;
	}
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VLNK, ap->a_target);
}
/* Only the name of a local socket can be made here (bind); devices live in devfs. */
static int
ramfs_mknod(struct vnop_mknod_args *ap)
{
	if (ap->a_vap->va_type != VSOCK) {
		return ENOTSUP;
	}
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VSOCK, NULL);
}

static int
ramfs_remove(struct vnop_remove_args *ap)
{
	struct ramfs_node *dir = RAMFS_NODE(ap->a_dvp);
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_dirent *de;

	if (node->rn_type == VDIR) {
		return EPERM;
	}
	if ((ap->a_flags & VNODE_REMOVE_NODELETEBUSY) && vnode_isinuse(ap->a_vp, 0)) {
		return EBUSY;
	}
	lck_mtx_lock(&rm->rm_lock);
	de = ramfs_dir_find(dir, ap->a_cnp->cn_nameptr, (size_t)ap->a_cnp->cn_namelen);
	if (de == NULL || de->rd_node != node) {
		lck_mtx_unlock(&rm->rm_lock);
		return ENOENT;
	}
	ramfs_dir_remove(dir, de);
	node->rn_nlink--;
	ramfs_touch(node, false);
	lck_mtx_unlock(&rm->rm_lock);
	/* with no links left the node goes away when its vnode does: see ramfs_inactive() */
	return 0;
}

static int
ramfs_rmdir(struct vnop_rmdir_args *ap)
{
	struct ramfs_node *dir = RAMFS_NODE(ap->a_dvp);
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_dirent *de;
	int error = 0;

	if (node->rn_type != VDIR) {
		return ENOTDIR;
	}
	lck_mtx_lock(&rm->rm_lock);
	de = ramfs_dir_find(dir, ap->a_cnp->cn_nameptr, (size_t)ap->a_cnp->cn_namelen);
	if (de == NULL || de->rd_node != node) {
		error = ENOENT;
	} else if (node->rn_nentries != 0) {
		error = ENOTEMPTY;
	} else {
		ramfs_dir_remove(dir, de);
		dir->rn_nlink--;
		node->rn_nlink = 0;
		node->rn_parent = NULL;
	}
	lck_mtx_unlock(&rm->rm_lock);
	return error;
}

static int
ramfs_link(struct vnop_link_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_node *dir = RAMFS_NODE(ap->a_tdvp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct componentname *cnp = ap->a_cnp;
	struct ramfs_dirent *de;
	int error = 0;

	if (node->rn_type == VDIR) {
		return EPERM;
	}
	if (cnp->cn_namelen > RAMFS_MAX_NAME) {
		return ENAMETOOLONG;
	}
	de = ramfs_dirent_alloc(cnp->cn_nameptr, (size_t)cnp->cn_namelen);

	lck_mtx_lock(&rm->rm_lock);
	if (dir->rn_nlink == 0 || node->rn_nlink == 0) {
		error = ENOENT;
	} else if (ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen) != NULL) {
		error = EEXIST;
	} else {
		ramfs_dir_add(dir, de, node);
		node->rn_nlink++;
		ramfs_touch(node, false);
		de = NULL;
	}
	lck_mtx_unlock(&rm->rm_lock);
	if (de != NULL) {
		ramfs_dirent_free(de);
	}
	return error;
}

#include "ramfs_rename.inc"

static int
ramfs_open(__unused struct vnop_open_args *ap)
{
	return 0;
}

static int
ramfs_close(__unused struct vnop_close_args *ap)
{
	return 0;
}

static int
ramfs_getattr(struct vnop_getattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	uint64_t size, alloc;

	lck_mtx_lock(&rm->rm_lock);
	size = node->rn_type == VDIR ? (uint64_t)(node->rn_nentries + 2) * 32 : node->rn_size;
	alloc = node->rn_nalloc * PAGE_SIZE;

	VATTR_RETURN(vap, va_rdev, 0);
	VATTR_RETURN(vap, va_nlink, node->rn_nlink);
	VATTR_RETURN(vap, va_total_size, size);
	VATTR_RETURN(vap, va_total_alloc, alloc);
	VATTR_RETURN(vap, va_data_size, size);
	VATTR_RETURN(vap, va_data_alloc, alloc);
	VATTR_RETURN(vap, va_iosize, PAGE_SIZE);
	VATTR_RETURN(vap, va_uid, node->rn_uid);
	VATTR_RETURN(vap, va_gid, node->rn_gid);
	VATTR_RETURN(vap, va_mode, node->rn_mode);
	VATTR_RETURN(vap, va_flags, node->rn_bsdflags);
	VATTR_RETURN(vap, va_create_time, node->rn_btime);
	VATTR_RETURN(vap, va_addedtime, node->rn_btime);
	VATTR_RETURN(vap, va_access_time, node->rn_atime);
	VATTR_RETURN(vap, va_modify_time, node->rn_mtime);
	VATTR_RETURN(vap, va_change_time, node->rn_ctime);
	VATTR_RETURN(vap, va_fileid, node->rn_ino);
	VATTR_RETURN(vap, va_linkid, node->rn_ino);
	VATTR_RETURN(vap, va_fsid, vfs_statfs(vnode_mount(vp))->f_fsid.val[0]);
	VATTR_RETURN(vap, va_filerev, 0);
	VATTR_RETURN(vap, va_gen, 0);
	VATTR_RETURN(vap, va_dataprotect_class, node->rn_dataprotect_class ? node->rn_dataprotect_class : PROTECTION_CLASS_D);   /* "none", which PosterBoard checks */
	if (node->rn_type == VDIR && node->rn_parent != NULL) {
		VATTR_RETURN(vap, va_parentid, node->rn_parent->rn_ino);
		/*
		 * And its name, from its entry in the parent: what the kernel asks for to make a path out of a
		 * number alone (fsgetpath() of a vnode vfs_vget made, which has no name of its own).
		 */
		if (VATTR_IS_ACTIVE(vap, va_name) && vap->va_name != NULL && node != rm->rm_root) {
			struct ramfs_dirent *de;

			TAILQ_FOREACH(de, &node->rn_parent->rn_entries, rd_link) {
				if (de->rd_node == node) {
					strlcpy(vap->va_name, de->rd_name, MAXPATHLEN);
					VATTR_SET_SUPPORTED(vap, va_name);
					break;
				}
			}
		}
	}
	lck_mtx_unlock(&rm->rm_lock);
	return 0;
}

static int
ramfs_setattr(struct vnop_setattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	bool resized = false;
	uint64_t size = 0;

	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		if (node->rn_type == VDIR) {
			return EISDIR;
		}
		if (node->rn_type != VREG) {
			return EINVAL;
		}
		/* what a mapping changed must reach us before the size moves under it */
		ramfs_sync_mapping(vp, node);
	}

	lck_mtx_lock(&rm->rm_lock);
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		size = vap->va_data_size;
		ramfs_resize(node, size);
		resized = true;
		VATTR_SET_SUPPORTED(vap, va_data_size);
	}
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
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		node->rn_atime = vap->va_access_time;
		VATTR_SET_SUPPORTED(vap, va_access_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		node->rn_mtime = vap->va_modify_time;
		VATTR_SET_SUPPORTED(vap, va_modify_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_change_time)) {
		node->rn_ctime = vap->va_change_time;
		VATTR_SET_SUPPORTED(vap, va_change_time);
		ramfs_disk_dirty_locked(rm);
	} else {
		ramfs_touch(node, false);
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (resized) {
		(void)ubc_setsize(vp, (off_t)size);
	}
	return 0;
}

static int
ramfs_read(struct vnop_read_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	char *bounce;
	int error = 0;

	if (node->rn_type == VDIR) {
		return EISDIR;
	}
	if (node->rn_type != VREG || uio_offset(uio) < 0) {
		return EINVAL;
	}
	ramfs_sync_mapping(vp, node);

	bounce = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);
	while (uio_resid(uio) > 0 && !error) {
		uint64_t off = (uint64_t)uio_offset(uio);
		size_t len;

		lck_mtx_lock(&rm->rm_lock);
		if (off >= node->rn_size) {
			lck_mtx_unlock(&rm->rm_lock);
			break;
		}
		len = MIN((size_t)MIN((uint64_t)uio_resid(uio), node->rn_size - off), PAGE_SIZE - (size_t)(off & PAGE_MASK));
		ramfs_fetch(node, off, bounce, len);
		lck_mtx_unlock(&rm->rm_lock);

		error = uiomove(bounce, (int)len, uio);
	}
	kfree_data(bounce, PAGE_SIZE);
	return error;
}

static int
ramfs_write(struct vnop_write_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	void *spare = NULL;
	char *bounce;
	uint64_t start, old_size;
	int error = 0;

	if (node->rn_type == VDIR) {
		return EISDIR;
	}
	if (node->rn_type != VREG || uio_offset(uio) < 0) {
		return EINVAL;
	}
	if (uio_resid(uio) == 0) {
		return 0;
	}
	ramfs_sync_mapping(vp, node);

	lck_mtx_lock(&rm->rm_lock);
	old_size = node->rn_size;
	if (ap->a_ioflag & IO_APPEND) {
		uio_setoffset(uio, (off_t)old_size);
	}
	lck_mtx_unlock(&rm->rm_lock);
	start = (uint64_t)uio_offset(uio);

	bounce = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);
	while (uio_resid(uio) > 0) {
		uint64_t off = (uint64_t)uio_offset(uio);
		size_t len = MIN((size_t)uio_resid(uio), PAGE_SIZE - (size_t)(off & PAGE_MASK));

		error = uiomove(bounce, (int)len, uio);
		if (error) {
			break;
		}
		if (spare == NULL) {
			spare = ramfs_page_alloc(true);
		}
		uint32_t write_dblk = 0;
		void *write_page = NULL;

		ramfs_reserve_slots(rm, node, off / PAGE_SIZE);
		lck_mtx_lock(&rm->rm_lock);
		error = ramfs_store(node, off, bounce, len, &spare, &write_dblk, &write_page);
		lck_mtx_unlock(&rm->rm_lock);
		if (error) {
			/* nothing of this piece was stored: hand it back to the caller's count */
			uio_setoffset(uio, (off_t)off);
			uio_setresid(uio, uio_resid(uio) + (user_ssize_t)len);
			break;
		}
		error = ramfs_store_write(rm, node, off / PAGE_SIZE, write_dblk, write_page);
		if (error) {
			break;
		}
	}
	kfree_data(bounce, PAGE_SIZE);
	if (spare != NULL) {
		ramfs_page_free(spare);
	}

	if ((uint64_t)uio_offset(uio) > start) {
		uint64_t new_size;

		lck_mtx_lock(&rm->rm_lock);
		ramfs_touch(node, true);
		new_size = node->rn_size;
		lck_mtx_unlock(&rm->rm_lock);

		if (new_size != old_size) {
			(void)ubc_setsize(vp, (off_t)new_size);
		}
		/* cached pages of the range are stale now: brought up to date in place */
		if (ubc_pages_resident(vp)) {
			ramfs_refresh_cache(vp, node, start, (uint64_t)uio_offset(uio));
		}
		/* a partial write is a success */
		if (error == ENOSPC) {
			error = 0;
		}
	}
	return error;
}

/* Classic 32-bit-inode directory entries; the offset is a byte count of the entries before it. */
static int
ramfs_readdir(struct vnop_readdir_args *ap)
{
#define RAMFS_DIRSIZ(namlen)    ((sizeof(struct dirent) - (MAXNAMLEN + 1)) + (((namlen) + 1 + 3) & ~3))
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ramfs_node *dir = RAMFS_NODE(vp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_dirent *de;
	off_t start = uio_offset(uio), pos = 0;
	size_t room = (size_t)MIN((user_ssize_t)PAGE_SIZE, uio_resid(uio));
	size_t used = 0;
	uint32_t entry = 0;
	bool eof = false;
	int count = 0, error;
	char *buf;

	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) {
		return EINVAL;
	}
	if (dir->rn_type != VDIR) {
		return ENOTDIR;
	}
	/* the entries are collected under the lock and copied out after it is dropped */
	buf = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);

	lck_mtx_lock(&rm->rm_lock);
	de = TAILQ_FIRST(&dir->rn_entries);
	for (;; entry++) {
		struct dirent d;
		const char *name;

		bzero(&d, sizeof(d));
		if (entry == 0) {
			d.d_fileno = (ino_t)dir->rn_ino;
			d.d_type = DT_DIR;
			d.d_namlen = 1;
			name = ".";
		} else if (entry == 1) {
			d.d_fileno = (ino_t)(dir->rn_parent ? dir->rn_parent->rn_ino : dir->rn_ino);
			d.d_type = DT_DIR;
			d.d_namlen = 2;
			name = "..";
		} else {
			if (de == NULL) {
				eof = true;
				break;
			}
			d.d_fileno = (ino_t)de->rd_node->rn_ino;
			d.d_type = (uint8_t)IFTODT(VTTOIF(de->rd_node->rn_type));
			d.d_namlen = (uint8_t)de->rd_namelen;
			name = de->rd_name;
			de = TAILQ_NEXT(de, rd_link);
		}
		d.d_reclen = (uint16_t)RAMFS_DIRSIZ(d.d_namlen);
		if (pos >= start) {
			if (used + d.d_reclen > room) {
				break;
			}
			memcpy(d.d_name, name, d.d_namlen);
			memcpy(buf + used, &d, d.d_reclen);
			used += d.d_reclen;
			count++;
		}
		pos += d.d_reclen;
	}
	lck_mtx_unlock(&rm->rm_lock);

	error = used ? uiomove(buf, (int)used, uio) : 0;
	kfree_data(buf, PAGE_SIZE);
	if (!error) {
		uio_setoffset(uio, pos);
		if (ap->a_eofflag) {
			*ap->a_eofflag = eof;
		}
		if (ap->a_numdirent) {
			*ap->a_numdirent = count;
		}
	}
	return error;
#undef RAMFS_DIRSIZ
}

static int
ramfs_readlink(struct vnop_readlink_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);

	if (node->rn_type != VLNK) {
		return EINVAL;
	}
	/* a symlink never changes after it is made */
	return uiomove(node->rn_target, (int)node->rn_size, ap->a_uio);
}

#include "ramfs_fsctl.inc"

/* Last reference gone: a node without a name has no reason to linger in the vnode cache. */
static int
ramfs_inactive(struct vnop_inactive_args *ap)
{
	if (RAMFS_NODE(ap->a_vp)->rn_nlink == 0) {
		vnode_recycle(ap->a_vp);
	}
	return 0;
}

static int
ramfs_reclaim(struct vnop_reclaim_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;

	lck_mtx_lock(&rm->rm_lock);
	node->rn_vp = NULLVP;
	node->rn_vid = 0;
	if (node->rn_nlink == 0 && node != rm->rm_root) {
		ramfs_node_free(node);
	}
	lck_mtx_unlock(&rm->rm_lock);

	vnode_clearfsnode(vp);
	return 0;
}

static int
ramfs_pathconf(struct vnop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 32767;
		return 0;
	case _PC_NAME_MAX:
		*ap->a_retval = RAMFS_MAX_NAME;
		return 0;
	case _PC_PATH_MAX:
		*ap->a_retval = MAXPATHLEN;
		return 0;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		return 0;
	case _PC_CHOWN_RESTRICTED:
	case _PC_NO_TRUNC:
		*ap->a_retval = 200112;
		return 0;
	case _PC_CASE_SENSITIVE:
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		return 0;
	case _PC_FILESIZEBITS:
		*ap->a_retval = 64;
		return 0;
	default:
		return EINVAL;
	}
}

static int
ramfs_mmap(struct vnop_mmap_args *ap)
{
	if (ap->a_fflags & VM_PROT_WRITE) {
		RAMFS_NODE(ap->a_vp)->rn_mapped_write = true;
	}
	return 0;
}

static int
ramfs_mnomap(__unused struct vnop_mnomap_args *ap)
{
	return 0;
}

/* Fill pages of the page cache from the file. */
static int
ramfs_pagein(struct vnop_pagein_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	upl_t upl = ap->a_pl;
	upl_size_t size = (upl_size_t)ap->a_size;
	uint64_t off = (uint64_t)ap->a_f_offset;
	bool commit = (ap->a_flags & UPL_NOCOMMIT) == 0;
	upl_size_t valid = 0;
	vm_offset_t addr;

	if (upl == NULL || node->rn_type != VREG || ap->a_f_offset < 0 || (off & PAGE_MASK) != 0 ||
	    ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
		if (commit && upl != NULL) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
		return EINVAL;
	}
	addr += ap->a_pl_offset;

	lck_mtx_lock(&rm->rm_lock);
	for (upl_size_t done = 0; done < size && off + done < node->rn_size; done += PAGE_SIZE) {
		size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, node->rn_size - (off + done));

		ramfs_fetch(node, off + done, (void *)(addr + done), len);
		bzero((void *)(addr + done + len), PAGE_SIZE - len);
		valid = done + PAGE_SIZE;
	}
	lck_mtx_unlock(&rm->rm_lock);
	ubc_upl_unmap(upl);

	if (commit) {
		if (valid != 0) {
			ubc_upl_commit_range(upl, ap->a_pl_offset, valid, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
		/* pages wholly past the end of the file do not exist */
		if (valid < size) {
			ubc_upl_abort_range(upl, ap->a_pl_offset + valid, size - valid, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
	}
	return valid != 0 ? 0 : EINVAL;
}

/* Take back what a mapping changed: copy dirty pages of the page cache into the file. */
static int
ramfs_pageout(struct vnop_pageout_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	upl_t upl = ap->a_pl;
	upl_size_t size = (upl_size_t)ap->a_size;
	uint64_t off = (uint64_t)ap->a_f_offset;
	bool commit = (ap->a_flags & UPL_NOCOMMIT) == 0;
	vm_offset_t addr;
	int error = 0;

	if (upl == NULL) {
		return EINVAL;
	}
	if (node->rn_type != VREG || ap->a_f_offset < 0 || (off & PAGE_MASK) != 0 ||
	    ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
		if (commit) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		}
		return EINVAL;
	}
	addr += ap->a_pl_offset;

	for (upl_size_t done = 0; done < size && off + done < node->rn_size && !error; done += PAGE_SIZE) {
		size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, node->rn_size - (off + done));
		uint64_t index = (off + done) / PAGE_SIZE;
		uint32_t write_dblk = 0;
		void *write_page = NULL;
		/* this may be the pageout daemon looking for memory: never wait for more of it here */
		void *spare = index < node->rn_nslots && node->rn_pages[index] != NULL ? NULL : ramfs_page_alloc(false);

		lck_mtx_lock(&rm->rm_lock);
		error = ramfs_store(node, off + done, (const void *)(addr + done), len, &spare, &write_dblk, &write_page);
		lck_mtx_unlock(&rm->rm_lock);
		if (spare != NULL) {
			ramfs_page_free(spare);
		}
		if (!error) {
			error = ramfs_store_write(rm, node, index, write_dblk, write_page);
		}
	}
	if (!error) {
		lck_mtx_lock(&rm->rm_lock);
		ramfs_touch(node, true);
		lck_mtx_unlock(&rm->rm_lock);
	}
	ubc_upl_unmap(upl);

	if (commit) {
		if (error) {
			/* the pages stay dirty and will be offered again */
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		} else {
			ubc_upl_commit_range(upl, ap->a_pl_offset, size, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
	}
	return error;
}

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

	ramfs_disk_mount(rm);
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
	ramfs_disk_unmount(rm);
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

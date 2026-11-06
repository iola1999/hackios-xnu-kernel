/*
 * Direct UFS persistence for /private/var (RDWFS001).
 *
 * File data pages are written straight through to 16 KB blocks on /dev/ufs_darwin_var
 * (or /dev/rufs_dsu_store) at RDWFS_DATA_OFFSET + dblk * PAGE_SIZE, and faulted in on
 * demand when read. Metadata (inodes, directories, symlinks, extended attributes, and
 * per-file block tables) is checkpointed to two alternating 32 MiB slots at the start
 * of the partition (0 MiB and 32 MiB), each protected by superblock and payload CRC32s.
 */

extern bool redwood_ufs_var_info(uint64_t *size_bytes_out);
extern int redwood_ufs_var_io(bool write, uint64_t offset, void *buf, uint64_t len);

#define RDWFS_MAGIC_STR          "RDWFS001"
#define RDWFS_VERSION            2u
#define rm_disk_enabled          rm_disk_backed
#define RDWFS_SUPER_BYTES        4096u
#define RDWFS_SLOT0_OFFSET       0ULL
#define RDWFS_SLOT1_OFFSET       (32ULL << 20)
#define RDWFS_SLOT_MAX           (32ULL << 20)
#define RDWFS_PANIC_OFFSET       (64ULL << 20)
#define RDWFS_PANIC_SIZE         (16ULL << 20)
#define RDWFS_DATA_OFFSET        (512ULL << 20)
#define RDWFS_INIT_META_CAP      (8ULL << 20)
#define RDWFS_ALIGN8(n)          (((uint64_t)(n) + 7ULL) & ~7ULL)
#define RDWFS_ALIGN4K(n)         (((uint64_t)(n) + 4095ULL) & ~4095ULL)

#define KLOG_MAGIC               0x474f4c4bU     /* 'KLOG' */
#define KLOG_MAGIC_ARCHIVED      0x474f4c44U     /* 'DLOG' */
#define KLOG_VERSION             1U

struct klog_head {
	uint32_t magic;
	uint32_t version;
	uint64_t payload_bytes;
	uint64_t timestamp;
	uint32_t boot_counter;
	uint32_t payload_crc;
	uint32_t head_crc;
	uint32_t flags;
	uint8_t  reserved[24];
};
_Static_assert(sizeof(struct klog_head) == 64, "klog_head must be 64 bytes");

struct rdwfs_super {
	char     magic[8];
	uint32_t version;
	uint32_t page_size;
	uint64_t generation;
	uint64_t payload_bytes;
	uint64_t next_ino;
	uint32_t node_count;
	uint32_t payload_crc;
	uint32_t super_crc;
	uint8_t  reserved[4096 - 52];
};
_Static_assert(sizeof(struct rdwfs_super) == 4096, "rdwfs_super must be 4096 bytes");

struct rdwfs_inode_rec {
	uint64_t ino;
	uint64_t size;
	uint64_t parent_ino;
	uint32_t type;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint32_t bsdflags;
	uint32_t dataprotect_class;
	uint32_t nlink;
	uint32_t target_len;
	uint32_t nblocks;
	uint32_t nxattrs;
	uint32_t ndirents;
	uint32_t reserved;
	int64_t  atime_sec, atime_nsec;
	int64_t  mtime_sec, mtime_nsec;
	int64_t  ctime_sec, ctime_nsec;
	int64_t  btime_sec, btime_nsec;
};

struct rdwfs_xattr_rec {
	uint32_t namelen;
	uint32_t size;
};

struct rdwfs_dirent_rec {
	uint64_t child_ino;
	uint32_t namelen;
	uint32_t reserved;
};

static struct ramfs_mount *ramfs_disk_rm;
static bool ramfs_disk_claimed;
static int ramfs_var_disk_loaded;
static int ramfs_var_seed_done = 1;

static void ramfs_disk_dirty_locked(struct ramfs_mount *rm);
static int ramfs_disk_sync_impl(struct ramfs_mount *rm);
#define ramfs_disk_sync(rm, ...) ramfs_disk_sync_impl(rm)

static void
ramfs_disk_mark_seed_done(struct ramfs_mount *rm)
{
	if (rm != NULL) {
		lck_mtx_lock(&rm->rm_lock);
		rm->rm_seed_done = true;
		ramfs_var_seed_done = 1;
		ramfs_disk_dirty_locked(rm);
		lck_mtx_unlock(&rm->rm_lock);
		(void)ramfs_disk_sync(rm, true);
	}
}

static int
sysctl_var_seed_done SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int val = ramfs_var_seed_done;
	int error = sysctl_io_number(req, val, sizeof(val), &val, NULL);

	if (error == 0 && req->newptr != USER_ADDR_NULL) {
		ramfs_var_seed_done = val;
		if (ramfs_disk_rm != NULL) {
			if (val != 0) {
				ramfs_disk_mark_seed_done(ramfs_disk_rm);
			} else {
				lck_mtx_lock(&ramfs_disk_rm->rm_lock);
				ramfs_disk_rm->rm_seed_done = false;
				lck_mtx_unlock(&ramfs_disk_rm->rm_lock);
			}
		}
	}
	return error;
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_INT(_kern_redwood, OID_AUTO, var_disk_loaded, CTLFLAG_RD | CTLFLAG_LOCKED,
    &ramfs_var_disk_loaded, 0, "1 when /private/var was loaded from RDWFS001 on disk");
SYSCTL_PROC(_kern_redwood, OID_AUTO, var_seed_done,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    &ramfs_var_seed_done, 0, sysctl_var_seed_done, "I",
    "1 when /private/var seed unpack has completed");

static void *
ramfs_vm_alloc(size_t bytes)
{
	vm_offset_t addr = 0;

	return kmem_alloc(kernel_map, &addr, bytes, KMA_KOBJECT | KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_FILE) == KERN_SUCCESS ? (void *)addr : NULL;
}

static void
ramfs_vm_free(void *ptr, size_t bytes)
{
	if (ptr != NULL && bytes != 0) {
		kmem_free(kernel_map, (vm_offset_t)ptr, bytes);
	}
}

static void
ramfs_disk_bmap_set(struct ramfs_mount *rm, uint32_t dblk)
{
	if (rm->rm_bmap != NULL && dblk > 0 && dblk < rm->rm_bmap_max) {
		rm->rm_bmap[dblk >> 5] |= (1u << (dblk & 31u));
	}
}

static void
ramfs_disk_bmap_free(struct ramfs_mount *rm, uint32_t dblk)
{
	if (rm->rm_bmap != NULL && dblk > 0 && dblk < rm->rm_bmap_max) {
		rm->rm_bmap[dblk >> 5] &= ~(1u << (dblk & 31u));
	}
}

static uint32_t
ramfs_disk_bmap_alloc(struct ramfs_mount *rm)
{
	uint32_t start_word, w, words = rm->rm_bmap_words;

	if (rm->rm_bmap == NULL || words == 0) {
		return 0;
	}
	start_word = rm->rm_bmap_next >> 5;
	if (start_word >= words) {
		start_word = 0;
	}
	for (uint32_t step = 0; step < words; step++) {
		uint32_t word;

		w = start_word + step;
		if (w >= words) {
			w -= words;
		}
		word = rm->rm_bmap[w];
		if (word == 0xffffffffu) {
			continue;
		}
		for (uint32_t bit = 0; bit < 32u; bit++) {
			uint32_t dblk = (w << 5) + bit;

			if (dblk == 0 || dblk >= rm->rm_bmap_max) {
				continue;
			}
			if ((word & (1u << bit)) == 0) {
				rm->rm_bmap[w] |= (1u << bit);
				rm->rm_bmap_next = (dblk + 1u < rm->rm_bmap_max) ? (dblk + 1u) : 1u;
				return dblk;
			}
		}
	}
	return 0;
}

static int
ramfs_disk_read_block(struct ramfs_mount *rm, uint32_t dblk, void *dst)
{
	if (!rm->rm_disk_backed || dblk == 0 || dblk >= rm->rm_bmap_max) {
		return EIO;
	}
	return redwood_ufs_var_io(false, RDWFS_DATA_OFFSET + (uint64_t)dblk * PAGE_SIZE, dst, PAGE_SIZE);
}

static int
ramfs_disk_write_block(struct ramfs_mount *rm, uint32_t dblk, const void *src)
{
	if (!rm->rm_disk_backed || dblk == 0 || dblk >= rm->rm_bmap_max) {
		return EIO;
	}
	return redwood_ufs_var_io(true, RDWFS_DATA_OFFSET + (uint64_t)dblk * PAGE_SIZE,
	    (void *)(uintptr_t)src, PAGE_SIZE);
}

static void
ramfs_disk_fault_page(struct ramfs_node *node, uint64_t index)
{
	struct ramfs_mount *rm = node->rn_mount;
	void *page;

	if (!rm->rm_disk_backed || index >= node->rn_nslots || node->rn_pages[index] != NULL ||
	    node->rn_blocks == NULL || node->rn_blocks[index] == 0) {
		return;
	}
	if (os_atomic_load(&ramfs_pages_in_use, relaxed) >= rm->rm_max_pages / 2) {
		return;
	}
	page = ramfs_page_alloc(false);
	if (page == NULL) {
		return;
	}
	if (ramfs_disk_read_block(rm, node->rn_blocks[index], page) == 0) {
		node->rn_pages[index] = page;
		os_atomic_inc(&ramfs_pages_in_use, relaxed);
	} else {
		ramfs_page_free(page);
	}
}

static void
ramfs_disk_evict_page_locked(struct ramfs_mount *rm, struct ramfs_node *node, uint64_t index, void *page)
{
	if (os_atomic_load(&ramfs_pages_in_use, relaxed) >= rm->rm_max_pages / 2) {
		if (node->rn_pages != NULL && index < node->rn_nslots && node->rn_pages[index] == page) {
			ramfs_page_free(page);
			node->rn_pages[index] = NULL;
			os_atomic_dec(&ramfs_pages_in_use, relaxed);
		}
	}
}

static void
ramfs_disk_evict_page(struct ramfs_mount *rm, struct ramfs_node *node, uint64_t index, void *page)
{
	if (os_atomic_load(&ramfs_pages_in_use, relaxed) >= rm->rm_max_pages / 2) {
		lck_mtx_lock(&rm->rm_lock);
		ramfs_disk_evict_page_locked(rm, node, index, page);
		lck_mtx_unlock(&rm->rm_lock);
	}
}

static void
ramfs_disk_dirty_locked(struct ramfs_mount *rm)
{
	if (rm == NULL || !rm->rm_disk_backed || rm->rm_disk_loading) {
		return;
	}
	rm->rm_meta_dirty = true;
	if (ramfs_var_seed_done == 0 || !rm->rm_seed_done) {
		return;
	}
	if (rm->rm_meta_gen > 0 && rm->rm_meta_call != NULL && !rm->rm_meta_syncing) {
		uint64_t deadline;

		clock_interval_to_deadline(1500, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed((thread_call_t)rm->rm_meta_call, deadline);
	}
}

/* Serialize all live nodes into rm->rm_meta_buf + RDWFS_SUPER_BYTES. rm->rm_lock is held. */
static int
ramfs_disk_pack_locked(struct ramfs_mount *rm, uint64_t *payload_bytes_out, uint32_t *node_count_out)
{
	uint8_t *base = (uint8_t *)rm->rm_meta_buf + RDWFS_SUPER_BYTES;
	uint64_t cap = rm->rm_meta_cap > RDWFS_SUPER_BYTES ? (rm->rm_meta_cap - RDWFS_SUPER_BYTES) : 0;
	uint64_t at = 0;
	uint32_t nodes = 0;

	for (uint32_t h = 0; h < RAMFS_HASH_SIZE; h++) {
		for (struct ramfs_node *node = rm->rm_hash[h]; node != NULL; node = node->rn_hnext) {
			struct rdwfs_inode_rec rec;
			struct ramfs_xattr *xa;
			struct ramfs_dirent *de;
			uint32_t nblocks = 0, nxattrs = 0, ndirents = 0, target_len = 0;
			uint64_t need;

			if (node->rn_nlink == 0 || node->rn_type == VSOCK) {
				continue;
			}
			if (node->rn_type == VLNK && node->rn_target != NULL) {
				target_len = (uint32_t)node->rn_size;
			}
			if (node->rn_type == VREG && node->rn_blocks != NULL && node->rn_size > 0) {
				uint64_t slots_used = (node->rn_size + PAGE_MASK) / PAGE_SIZE;

				if (slots_used > node->rn_nslots) {
					slots_used = node->rn_nslots;
				}
				nblocks = (uint32_t)slots_used;
			}
			TAILQ_FOREACH(xa, &node->rn_xattrs, rx_link) {
				nxattrs++;
			}
			if (node->rn_type == VDIR) {
				TAILQ_FOREACH(de, &node->rn_entries, rd_link) {
					if (de->rd_node != NULL && de->rd_node->rn_nlink > 0 &&
					    de->rd_node->rn_type != VSOCK) {
						ndirents++;
					}
				}
			}
			need = sizeof(rec) + RDWFS_ALIGN8(target_len) +
			    RDWFS_ALIGN8((uint64_t)nblocks * sizeof(uint32_t));
			if (at + need > cap) {
				return ENOSPC;
			}
			bzero(&rec, sizeof(rec));
			rec.ino = node->rn_ino;
			rec.size = node->rn_size;
			rec.parent_ino = (node->rn_type == VDIR && node->rn_parent != NULL) ?
			    node->rn_parent->rn_ino : RAMFS_ROOT_INO;
			rec.type = (uint32_t)node->rn_type;
			rec.mode = node->rn_mode;
			rec.uid = node->rn_uid;
			rec.gid = node->rn_gid;
			rec.bsdflags = node->rn_bsdflags;
			rec.dataprotect_class = node->rn_dataprotect_class;
			rec.nlink = node->rn_nlink;
			rec.target_len = target_len;
			rec.nblocks = nblocks;
			rec.nxattrs = nxattrs;
			rec.ndirents = ndirents;
			rec.atime_sec = node->rn_atime.tv_sec;
			rec.atime_nsec = node->rn_atime.tv_nsec;
			rec.mtime_sec = node->rn_mtime.tv_sec;
			rec.mtime_nsec = node->rn_mtime.tv_nsec;
			rec.ctime_sec = node->rn_ctime.tv_sec;
			rec.ctime_nsec = node->rn_ctime.tv_nsec;
			rec.btime_sec = node->rn_btime.tv_sec;
			rec.btime_nsec = node->rn_btime.tv_nsec;

			memcpy(base + at, &rec, sizeof(rec));
			at += sizeof(rec);

			if (target_len > 0) {
				uint64_t alen = RDWFS_ALIGN8(target_len);

				bzero(base + at, (size_t)alen);
				memcpy(base + at, node->rn_target, target_len);
				at += alen;
			}
			if (nblocks > 0) {
				uint64_t bbytes = (uint64_t)nblocks * sizeof(uint32_t);
				uint64_t alen = RDWFS_ALIGN8(bbytes);

				bzero(base + at, (size_t)alen);
				memcpy(base + at, node->rn_blocks, (size_t)bbytes);
				at += alen;
			}
			TAILQ_FOREACH(xa, &node->rn_xattrs, rx_link) {
				struct rdwfs_xattr_rec xrec = {
					.namelen = (uint32_t)xa->rx_namelen,
					.size = (uint32_t)xa->rx_size,
				};
				uint64_t raw = (uint64_t)xrec.namelen + (uint64_t)xrec.size;
				uint64_t alen = RDWFS_ALIGN8(raw);

				if (at + sizeof(xrec) + alen > cap) {
					return ENOSPC;
				}
				memcpy(base + at, &xrec, sizeof(xrec));
				at += sizeof(xrec);
				bzero(base + at, (size_t)alen);
				memcpy(base + at, xa->rx_name, xrec.namelen);
				if (xrec.size > 0 && xa->rx_data != NULL) {
					memcpy(base + at + xrec.namelen, xa->rx_data, xrec.size);
				}
				at += alen;
			}
			if (node->rn_type == VDIR) {
				TAILQ_FOREACH(de, &node->rn_entries, rd_link) {
					struct rdwfs_dirent_rec drec;
					uint64_t alen;

					if (de->rd_node == NULL || de->rd_node->rn_nlink == 0 ||
					    de->rd_node->rn_type == VSOCK) {
						continue;
					}
					drec.child_ino = de->rd_node->rn_ino;
					drec.namelen = de->rd_namelen;
					drec.reserved = 0;
					alen = RDWFS_ALIGN8(drec.namelen);
					if (at + sizeof(drec) + alen > cap) {
						return ENOSPC;
					}
					memcpy(base + at, &drec, sizeof(drec));
					at += sizeof(drec);
					bzero(base + at, (size_t)alen);
					memcpy(base + at, de->rd_name, drec.namelen);
					at += alen;
				}
			}
			nodes++;
		}
	}
	*payload_bytes_out = at;
	*node_count_out = nodes;
	return 0;
}

static int
ramfs_disk_sync_impl(struct ramfs_mount *rm)
{
	uint64_t payload_bytes = 0, write_bytes, gen, slot_off;
	uint32_t node_count = 0;
	struct rdwfs_super *sup;
	int error = 0;

	if (rm == NULL || !rm->rm_disk_enabled || !rm->rm_seed_done || ramfs_var_seed_done == 0 || rm->rm_disk_loading) {
		return 0;
	}
	lck_mtx_lock(&rm->rm_lock);
	if (rm->rm_meta_gen == 0) {
		rm->rm_meta_dirty = true;
	}
	while (rm->rm_meta_syncing) {
		msleep(&rm->rm_meta_syncing, &rm->rm_lock, PRIBIO, "rdwfssync", NULL);
	}
	if (!rm->rm_meta_dirty) {
		lck_mtx_unlock(&rm->rm_lock);
		return 0;
	}

	while (ramfs_disk_pack_locked(rm, &payload_bytes, &node_count) == ENOSPC) {
		size_t old_cap = rm->rm_meta_cap;
		size_t new_cap = old_cap ? old_cap * 2 : RDWFS_INIT_META_CAP;
		void *new_buf;

		if (new_cap > RDWFS_SLOT_MAX) {
			lck_mtx_unlock(&rm->rm_lock);
			return ENOSPC;
		}
		rm->rm_meta_syncing = true;
		lck_mtx_unlock(&rm->rm_lock);
		new_buf = ramfs_vm_alloc(new_cap);
		lck_mtx_lock(&rm->rm_lock);
		rm->rm_meta_syncing = false;
		wakeup(&rm->rm_meta_syncing);
		if (new_buf == NULL) {
			lck_mtx_unlock(&rm->rm_lock);
			return ENOMEM;
		}
		if (rm->rm_meta_cap == old_cap) {
			ramfs_vm_free(rm->rm_meta_buf, old_cap);
			rm->rm_meta_buf = new_buf;
			rm->rm_meta_cap = new_cap;
		} else {
			ramfs_vm_free(new_buf, new_cap);
		}
	}

	rm->rm_meta_dirty = false;
	rm->rm_meta_syncing = true;
	gen = ++rm->rm_meta_gen;
	write_bytes = RDWFS_ALIGN4K(RDWFS_SUPER_BYTES + payload_bytes);
	if (write_bytes > RDWFS_SUPER_BYTES + payload_bytes) {
		bzero((uint8_t *)rm->rm_meta_buf + RDWFS_SUPER_BYTES + payload_bytes,
		    (size_t)(write_bytes - (RDWFS_SUPER_BYTES + payload_bytes)));
	}
	sup = (struct rdwfs_super *)rm->rm_meta_buf;
	bzero(sup, sizeof(*sup));
	memcpy(sup->magic, RDWFS_MAGIC_STR, 8);
	sup->version = RDWFS_VERSION;
	sup->page_size = PAGE_SIZE;
	sup->generation = gen;
	sup->payload_bytes = payload_bytes;
	sup->next_ino = rm->rm_next_ino;
	sup->node_count = node_count;
	sup->payload_crc = crc32(0, (uint8_t *)rm->rm_meta_buf + RDWFS_SUPER_BYTES, (size_t)payload_bytes);
	sup->super_crc = crc32(0, sup, __builtin_offsetof(struct rdwfs_super, super_crc));

	slot_off = (gen & 1ULL) ? RDWFS_SLOT0_OFFSET : RDWFS_SLOT1_OFFSET;
	lck_mtx_unlock(&rm->rm_lock);

	if (write_bytes > RDWFS_SUPER_BYTES) {
		error = redwood_ufs_var_io(true, slot_off + RDWFS_SUPER_BYTES,
		    (uint8_t *)rm->rm_meta_buf + RDWFS_SUPER_BYTES, write_bytes - RDWFS_SUPER_BYTES);
	}
	if (error == 0) {
		error = redwood_ufs_var_io(true, slot_off, rm->rm_meta_buf, RDWFS_SUPER_BYTES);
	}
	if (error == 0 && gen == 1) {
		if (write_bytes > RDWFS_SUPER_BYTES) {
			(void)redwood_ufs_var_io(true, RDWFS_SLOT1_OFFSET + RDWFS_SUPER_BYTES,
			    (uint8_t *)rm->rm_meta_buf + RDWFS_SUPER_BYTES, write_bytes - RDWFS_SUPER_BYTES);
		}
		(void)redwood_ufs_var_io(true, RDWFS_SLOT1_OFFSET, rm->rm_meta_buf, RDWFS_SUPER_BYTES);
	}

	lck_mtx_lock(&rm->rm_lock);
	rm->rm_meta_syncing = false;
	wakeup(&rm->rm_meta_syncing);
	if (error != 0) {
		printf("ramfs: metadata write failed (%d)\n", error);
	} else if (rm->rm_meta_dirty && rm->rm_seed_done && ramfs_var_seed_done != 0 &&
	    rm->rm_meta_call != NULL) {
		uint64_t deadline;

		clock_interval_to_deadline(1500, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed((thread_call_t)rm->rm_meta_call, deadline);
	}
	lck_mtx_unlock(&rm->rm_lock);
	return error;
}

static void
ramfs_disk_meta_thread(thread_call_param_t param0, __unused thread_call_param_t param1)
{
	struct ramfs_mount *rm = (struct ramfs_mount *)param0;

	if (rm != NULL && rm->rm_disk_backed && rm->rm_meta_gen > 0) {
		(void)ramfs_disk_sync(rm);
	}
}

void ramfs_disk_flush_all(void);

void
ramfs_disk_flush_all(void)
{
	if (ramfs_disk_rm != NULL) {
		(void)ramfs_disk_sync(ramfs_disk_rm);
	}
}

static struct ramfs_node *
ramfs_disk_find_ino(struct ramfs_mount *rm, uint64_t ino)
{
	for (struct ramfs_node *node = rm->rm_hash[ino & (RAMFS_HASH_SIZE - 1)]; node != NULL; node = node->rn_hnext) {
		if (node->rn_ino == ino) {
			return node;
		}
	}
	return NULL;
}

static bool
ramfs_disk_read_slot(struct ramfs_mount *rm, uint64_t slot_off, struct rdwfs_super *sup_out)
{
	struct rdwfs_super *sup = (struct rdwfs_super *)rm->rm_meta_buf;
	uint64_t pay_aligned;

	if (redwood_ufs_var_io(false, slot_off, sup, RDWFS_SUPER_BYTES) != 0) {
		return false;
	}
	if (memcmp(sup->magic, RDWFS_MAGIC_STR, 8) != 0 ||
	    (sup->version != 1u && sup->version != RDWFS_VERSION) ||
	    sup->node_count < 1000 ||
	    sup->page_size != PAGE_SIZE || sup->payload_bytes > RDWFS_SLOT_MAX - RDWFS_SUPER_BYTES) {
		return false;
	}
	if (crc32(0, sup, __builtin_offsetof(struct rdwfs_super, super_crc)) != sup->super_crc) {
		return false;
	}
	pay_aligned = RDWFS_ALIGN4K(sup->payload_bytes);
	if (RDWFS_SUPER_BYTES + pay_aligned > rm->rm_meta_cap) {
		size_t new_cap = (size_t)RDWFS_ALIGN4K(RDWFS_SUPER_BYTES + pay_aligned);
		void *new_buf = ramfs_vm_alloc(new_cap);

		if (new_buf == NULL) {
			return false;
		}
		memcpy(new_buf, sup, RDWFS_SUPER_BYTES);
		ramfs_vm_free(rm->rm_meta_buf, rm->rm_meta_cap);
		rm->rm_meta_buf = new_buf;
		rm->rm_meta_cap = new_cap;
		sup = (struct rdwfs_super *)rm->rm_meta_buf;
	}
	if (pay_aligned > 0 && redwood_ufs_var_io(false, slot_off + RDWFS_SUPER_BYTES,
	    (uint8_t *)rm->rm_meta_buf + RDWFS_SUPER_BYTES, pay_aligned) != 0) {
		return false;
	}
	if (crc32(0, (uint8_t *)rm->rm_meta_buf + RDWFS_SUPER_BYTES, (size_t)sup->payload_bytes) != sup->payload_crc) {
		return false;
	}
	*sup_out = *sup;
	return true;
}

static bool
ramfs_disk_unpack(struct ramfs_mount *rm, const struct rdwfs_super *sup)
{
	const uint8_t *base = (const uint8_t *)rm->rm_meta_buf + RDWFS_SUPER_BYTES;
	uint64_t end = sup->payload_bytes, at = 0, max_ino = RAMFS_ROOT_INO;

	/* Pass 1: instantiate nodes, symlink targets, regular file block tables, and xattrs. */
	for (uint32_t i = 0; i < sup->node_count; i++) {
		struct rdwfs_inode_rec rec;
		struct ramfs_node *node;

		if (at + sizeof(rec) > end) {
			return false;
		}
		memcpy(&rec, base + at, sizeof(rec));
		at += sizeof(rec);
		if (rec.ino > max_ino) {
			max_ino = rec.ino;
		}
		if (rec.ino == RAMFS_ROOT_INO) {
			node = rm->rm_root;
		} else {
			node = ramfs_node_alloc(rm, (enum vtype)rec.type, NULL);
			node->rn_ino = rec.ino;
			ramfs_hash_insert(rm, node);
			rm->rm_nodes++;
		}
		node->rn_size = rec.size;
		node->rn_mode = rec.mode;
		node->rn_uid = rec.uid;
		node->rn_gid = rec.gid;
		node->rn_bsdflags = rec.bsdflags;
		node->rn_dataprotect_class = rec.dataprotect_class ? rec.dataprotect_class : PROTECTION_CLASS_D;
		node->rn_nlink = rec.nlink;
		node->rn_atime.tv_sec = (time_t)rec.atime_sec;
		node->rn_atime.tv_nsec = (long)rec.atime_nsec;
		node->rn_mtime.tv_sec = (time_t)rec.mtime_sec;
		node->rn_mtime.tv_nsec = (long)rec.mtime_nsec;
		node->rn_ctime.tv_sec = (time_t)rec.ctime_sec;
		node->rn_ctime.tv_nsec = (long)rec.ctime_nsec;
		node->rn_btime.tv_sec = (time_t)rec.btime_sec;
		node->rn_btime.tv_nsec = (long)rec.btime_nsec;

		if (rec.target_len > 0) {
			uint64_t alen = RDWFS_ALIGN8(rec.target_len);

			if (at + alen > end) {
				return false;
			}
			node->rn_target = kalloc_data(rec.target_len + 1, Z_WAITOK);
			if (node->rn_target == NULL) {
				return false;
			}
			memcpy(node->rn_target, base + at, rec.target_len);
			node->rn_target[rec.target_len] = '\0';
			at += alen;
		}
		if (rec.nblocks > 0) {
			uint64_t bbytes = (uint64_t)rec.nblocks * sizeof(uint32_t);
			uint64_t alen = RDWFS_ALIGN8(bbytes);
			uint64_t want = 4;

			if (at + alen > end) {
				return false;
			}
			while (want < rec.nblocks) {
				want *= 2;
			}
			node->rn_pages = kalloc_type(void *, want, Z_WAITOK | Z_ZERO);
			node->rn_blocks = kalloc_data(want * sizeof(uint32_t), Z_WAITOK | Z_ZERO);
			if (node->rn_pages == NULL || node->rn_blocks == NULL) {
				if (node->rn_pages != NULL) {
					kfree_type(void *, want, node->rn_pages);
					node->rn_pages = NULL;
				}
				if (node->rn_blocks != NULL) {
					kfree_data(node->rn_blocks, want * sizeof(uint32_t));
					node->rn_blocks = NULL;
				}
				return false;
			}
			node->rn_nslots = want;
			memcpy(node->rn_blocks, base + at, (size_t)bbytes);
			for (uint32_t b = 0; b < rec.nblocks; b++) {
				uint32_t dblk = node->rn_blocks[b];

				if (dblk != 0) {
					ramfs_disk_bmap_set(rm, dblk);
					node->rn_nalloc++;
					rm->rm_pages++;
					os_atomic_inc(&ramfs_var_used_pages, relaxed);
				}
			}
			at += alen;
		}
		for (uint32_t x = 0; x < rec.nxattrs; x++) {
			struct rdwfs_xattr_rec xrec;
			struct ramfs_xattr *xa;
			uint64_t alen;

			if (at + sizeof(xrec) > end) {
				return false;
			}
			memcpy(&xrec, base + at, sizeof(xrec));
			at += sizeof(xrec);
			alen = RDWFS_ALIGN8((uint64_t)xrec.namelen + (uint64_t)xrec.size);
			if (at + alen > end || xrec.namelen > XATTR_MAXNAMELEN || xrec.size > RAMFS_MAX_XATTR) {
				return false;
			}
			xa = kalloc_type(struct ramfs_xattr, Z_WAITOK | Z_ZERO);
			if (xa == NULL) {
				return false;
			}
			xa->rx_name = kalloc_data(xrec.namelen + 1, Z_WAITOK);
			if (xa->rx_name == NULL) {
				kfree_type(struct ramfs_xattr, xa);
				return false;
			}
			memcpy(xa->rx_name, base + at, xrec.namelen);
			xa->rx_name[xrec.namelen] = '\0';
			xa->rx_namelen = xrec.namelen;
			if (xrec.size > 0) {
				xa->rx_data = kalloc_data(xrec.size, Z_WAITOK);
				if (xa->rx_data == NULL) {
					kfree_data(xa->rx_name, xrec.namelen + 1);
					kfree_type(struct ramfs_xattr, xa);
					return false;
				}
				memcpy(xa->rx_data, base + at + xrec.namelen, xrec.size);
			}
			xa->rx_size = xrec.size;
			TAILQ_INSERT_TAIL(&node->rn_xattrs, xa, rx_link);
			at += alen;
		}
		for (uint32_t d = 0; d < rec.ndirents; d++) {
			struct rdwfs_dirent_rec drec;

			if (at + sizeof(drec) > end) {
				return false;
			}
			memcpy(&drec, base + at, sizeof(drec));
			at += sizeof(drec) + RDWFS_ALIGN8(drec.namelen);
		}
	}

	/* Pass 2: link directory parent pointers and directory entries. */
	at = 0;
	for (uint32_t i = 0; i < sup->node_count; i++) {
		struct rdwfs_inode_rec rec;
		struct ramfs_node *node;

		memcpy(&rec, base + at, sizeof(rec));
		at += sizeof(rec);
		if (rec.target_len > 0) {
			at += RDWFS_ALIGN8(rec.target_len);
		}
		if (rec.nblocks > 0) {
			at += RDWFS_ALIGN8((uint64_t)rec.nblocks * sizeof(uint32_t));
		}
		for (uint32_t x = 0; x < rec.nxattrs; x++) {
			struct rdwfs_xattr_rec xrec;

			memcpy(&xrec, base + at, sizeof(xrec));
			at += sizeof(xrec) + RDWFS_ALIGN8((uint64_t)xrec.namelen + (uint64_t)xrec.size);
		}
		node = ramfs_disk_find_ino(rm, rec.ino);
		if (node != NULL && node->rn_type == VDIR) {
			struct ramfs_node *parent = ramfs_disk_find_ino(rm, rec.parent_ino);

			node->rn_parent = parent != NULL ? parent : rm->rm_root;
		}
		for (uint32_t d = 0; d < rec.ndirents; d++) {
			struct rdwfs_dirent_rec drec;
			struct ramfs_node *child;
			uint64_t alen;

			memcpy(&drec, base + at, sizeof(drec));
			at += sizeof(drec);
			alen = RDWFS_ALIGN8(drec.namelen);
			child = ramfs_disk_find_ino(rm, drec.child_ino);
			if (node != NULL && node->rn_type == VDIR && child != NULL &&
			    drec.namelen > 0 && drec.namelen <= RAMFS_MAX_NAME) {
				struct ramfs_dirent *de = ramfs_dirent_alloc((const char *)(base + at), drec.namelen);

				de->rd_node = child;
				TAILQ_INSERT_TAIL(&node->rn_entries, de, rd_link);
				node->rn_nentries++;
			}
			at += alen;
		}
	}
	rm->rm_next_ino = sup->next_ino > max_ino ? sup->next_ino : (max_ino + 1);
	rm->rm_meta_gen = sup->generation;
	return true;
}

static struct ramfs_node *
ramfs_disk_find_child(struct ramfs_node *dir, const char *name, size_t len)
{
	struct ramfs_dirent *de;

	if (dir == NULL || dir->rn_type != VDIR) {
		return NULL;
	}
	TAILQ_FOREACH(de, &dir->rn_entries, rd_link) {
		if (de->rd_namelen == len && memcmp(de->rd_name, name, len) == 0) {
			return de->rd_node;
		}
	}
	return NULL;
}

static void
ramfs_disk_restore_panic_log(struct ramfs_mount *rm)
{
	uint8_t *head_buf;
	struct klog_head *kh;
	uint64_t pay_len, total_len, npages, nslots;
	uint8_t *full_buf;
	const uint8_t *payload;
	void **pages;
	uint32_t *blocks;
	struct ramfs_node *db_node, *log_node;
	struct ramfs_dirent *de;

	if (rm == NULL || !rm->rm_disk_backed || rm->rm_root == NULL) {
		return;
	}

	head_buf = (uint8_t *)ramfs_vm_alloc(PAGE_SIZE);
	if (head_buf == NULL) {
		return;
	}
	if (redwood_ufs_var_io(false, RDWFS_PANIC_OFFSET, head_buf, PAGE_SIZE) != 0) {
		ramfs_vm_free(head_buf, PAGE_SIZE);
		return;
	}

	kh = (struct klog_head *)head_buf;
	if (kh->magic != KLOG_MAGIC || kh->version != KLOG_VERSION) {
		ramfs_vm_free(head_buf, PAGE_SIZE);
		return;
	}
	if (crc32(0, kh, __builtin_offsetof(struct klog_head, head_crc)) != kh->head_crc) {
		printf("ramfs: panic log header crc mismatch\n");
		ramfs_vm_free(head_buf, PAGE_SIZE);
		return;
	}
	if (kh->payload_bytes == 0 || kh->payload_bytes > (RDWFS_PANIC_SIZE - sizeof(struct klog_head))) {
		ramfs_vm_free(head_buf, PAGE_SIZE);
		return;
	}

	pay_len = kh->payload_bytes;
	total_len = RDWFS_ALIGN4K(sizeof(struct klog_head) + pay_len);
	full_buf = (uint8_t *)ramfs_vm_alloc((size_t)total_len);
	if (full_buf == NULL) {
		ramfs_vm_free(head_buf, PAGE_SIZE);
		return;
	}
	if (redwood_ufs_var_io(false, RDWFS_PANIC_OFFSET, full_buf, total_len) != 0) {
		ramfs_vm_free(full_buf, (size_t)total_len);
		ramfs_vm_free(head_buf, PAGE_SIZE);
		return;
	}

	payload = full_buf + sizeof(struct klog_head);
	if (crc32(0, payload, (size_t)pay_len) != kh->payload_crc) {
		printf("ramfs: panic log payload crc mismatch\n");
		ramfs_vm_free(full_buf, (size_t)total_len);
		ramfs_vm_free(head_buf, PAGE_SIZE);
		return;
	}

	npages = (pay_len + PAGE_MASK) / PAGE_SIZE;
	nslots = 4;
	while (nslots < npages) {
		nslots *= 2;
	}

	pages = kalloc_type(void *, nslots, Z_WAITOK | Z_ZERO);
	blocks = kalloc_data((size_t)nslots * sizeof(uint32_t), Z_WAITOK | Z_ZERO);
	if (pages == NULL || blocks == NULL) {
		if (pages != NULL) {
			kfree_type(void *, nslots, pages);
		}
		if (blocks != NULL) {
			kfree_data(blocks, (size_t)nslots * sizeof(uint32_t));
		}
		ramfs_vm_free(full_buf, (size_t)total_len);
		ramfs_vm_free(head_buf, PAGE_SIZE);
		return;
	}

	for (uint64_t i = 0; i < npages; i++) {
		void *p = ramfs_page_alloc(true);
		if (p == NULL) {
			for (uint64_t j = 0; j < i; j++) {
				ramfs_page_free(pages[j]);
			}
			kfree_type(void *, nslots, pages);
			kfree_data(blocks, (size_t)nslots * sizeof(uint32_t));
			ramfs_vm_free(full_buf, (size_t)total_len);
			ramfs_vm_free(head_buf, PAGE_SIZE);
			return;
		}
		size_t chunk = (i + 1 == npages && (pay_len & PAGE_MASK)) ? (size_t)(pay_len & PAGE_MASK) : PAGE_SIZE;
		memcpy(p, payload + i * PAGE_SIZE, chunk);
		pages[i] = p;
	}

	lck_mtx_lock(&rm->rm_lock);
	db_node = ramfs_disk_find_child(rm->rm_root, "db", 2);
	if (db_node == NULL) {
		db_node = ramfs_node_alloc(rm, VDIR, NULL);
		db_node->rn_ino = rm->rm_next_ino++;
		db_node->rn_mode = 0755;
		db_node->rn_nlink = 2;
		db_node->rn_parent = rm->rm_root;
		ramfs_hash_insert(rm, db_node);
		rm->rm_nodes++;

		de = ramfs_dirent_alloc("db", 2);
		de->rd_node = db_node;
		TAILQ_INSERT_TAIL(&rm->rm_root->rn_entries, de, rd_link);
		rm->rm_root->rn_nentries++;
	}

	log_node = ramfs_disk_find_child(db_node, "panic.log", 9);
	if (log_node != NULL) {
		if (log_node->rn_pages != NULL) {
			for (uint64_t i = 0; i < log_node->rn_nslots; i++) {
				if (log_node->rn_pages[i] != NULL) {
					ramfs_page_free(log_node->rn_pages[i]);
					os_atomic_dec(&ramfs_pages_in_use, relaxed);
				}
				if (log_node->rn_blocks != NULL && log_node->rn_blocks[i] != 0) {
					ramfs_disk_bmap_free(rm, log_node->rn_blocks[i]);
					os_atomic_dec(&ramfs_var_used_pages, relaxed);
				}
			}
			kfree_type(void *, log_node->rn_nslots, log_node->rn_pages);
		}
		if (log_node->rn_blocks != NULL) {
			kfree_data(log_node->rn_blocks, (size_t)log_node->rn_nslots * sizeof(uint32_t));
		}
	} else {
		log_node = ramfs_node_alloc(rm, VREG, NULL);
		log_node->rn_ino = rm->rm_next_ino++;
		log_node->rn_mode = 0644;
		log_node->rn_nlink = 1;
		ramfs_hash_insert(rm, log_node);
		rm->rm_nodes++;

		de = ramfs_dirent_alloc("panic.log", 9);
		de->rd_node = log_node;
		TAILQ_INSERT_TAIL(&db_node->rn_entries, de, rd_link);
		db_node->rn_nentries++;
	}

	log_node->rn_pages = pages;
	log_node->rn_blocks = blocks;
	log_node->rn_nslots = nslots;
	log_node->rn_nalloc = (uint32_t)npages;
	log_node->rn_size = pay_len;
	rm->rm_pages += npages;
	os_atomic_add(&ramfs_pages_in_use, npages, relaxed);

	for (uint64_t i = 0; i < npages; i++) {
		uint32_t dblk = ramfs_disk_bmap_alloc(rm);
		if (dblk != 0) {
			blocks[i] = dblk;
			os_atomic_inc(&ramfs_var_used_pages, relaxed);
		}
	}
	ramfs_disk_dirty_locked(rm);
	lck_mtx_unlock(&rm->rm_lock);

	for (uint64_t i = 0; i < npages; i++) {
		if (blocks[i] != 0 && pages[i] != NULL) {
			(void)ramfs_disk_write_block(rm, blocks[i], pages[i]);
		}
	}

	kh->magic = KLOG_MAGIC_ARCHIVED;
	kh->head_crc = crc32(0, kh, __builtin_offsetof(struct klog_head, head_crc));
	bzero(head_buf, PAGE_SIZE);
	memcpy(head_buf, kh, sizeof(*kh));
	(void)redwood_ufs_var_io(true, RDWFS_PANIC_OFFSET, head_buf, PAGE_SIZE);

	ramfs_vm_free(full_buf, (size_t)total_len);
	ramfs_vm_free(head_buf, PAGE_SIZE);
	(void)ramfs_disk_sync(rm);
	printf("ramfs: restored %llu bytes of crash log to /private/var/db/panic.log\n", (unsigned long long)pay_len);
}

static void
ramfs_disk_mount(struct ramfs_mount *rm)
{
	uint64_t part_bytes = 0, avail_bytes, max_blocks;
	struct rdwfs_super *sup0 = NULL, *sup1 = NULL, *win_sup = NULL;
	bool ok0 = false, ok1 = false, loaded = false;
	uint32_t win_slot = 0;

	if (ramfs_disk_claimed || !redwood_ufs_var_info(&part_bytes) || part_bytes <= RDWFS_DATA_OFFSET) {
		return;
	}
	avail_bytes = part_bytes - RDWFS_DATA_OFFSET;
	max_blocks = avail_bytes / PAGE_SIZE;
	if (max_blocks > 8388608ULL) {
		max_blocks = 8388608ULL;
	}
	rm->rm_bmap_max = (uint32_t)max_blocks;
	rm->rm_bmap_words = (rm->rm_bmap_max + 31u) / 32u;
	rm->rm_bmap = kalloc_data((size_t)rm->rm_bmap_words * sizeof(uint32_t), Z_WAITOK | Z_ZERO);
	rm->rm_io_page = ramfs_page_alloc(true);
	rm->rm_meta_cap = RDWFS_INIT_META_CAP;
	rm->rm_meta_buf = ramfs_vm_alloc(rm->rm_meta_cap);
	if (rm->rm_bmap == NULL || rm->rm_io_page == NULL || rm->rm_meta_buf == NULL) {
		if (rm->rm_bmap != NULL) {
			kfree_data(rm->rm_bmap, (size_t)rm->rm_bmap_words * sizeof(uint32_t));
			rm->rm_bmap = NULL;
		}
		if (rm->rm_io_page != NULL) {
			ramfs_page_free(rm->rm_io_page);
			rm->rm_io_page = NULL;
		}
		ramfs_vm_free(rm->rm_meta_buf, rm->rm_meta_cap);
		rm->rm_meta_buf = NULL;
		return;
	}
	rm->rm_bmap[0] |= 1u;
	rm->rm_bmap_next = part_bytes >= (8ULL << 30) ? (uint32_t)((2048ULL << 20) / PAGE_SIZE) : 1u;
	rm->rm_disk_loading = true;

	sup0 = kalloc_type(struct rdwfs_super, Z_WAITOK | Z_ZERO);
	sup1 = kalloc_type(struct rdwfs_super, Z_WAITOK | Z_ZERO);
	win_sup = kalloc_type(struct rdwfs_super, Z_WAITOK | Z_ZERO);
	if (sup0 == NULL || sup1 == NULL || win_sup == NULL) {
		goto out_cleanup_supers;
	}

	/* Inspect both checkpoint slots and pick the newer valid generation. */
	if (redwood_ufs_var_io(false, RDWFS_SLOT0_OFFSET, sup0, sizeof(*sup0)) == 0 &&
	    memcmp(sup0->magic, RDWFS_MAGIC_STR, 8) == 0 &&
	    (sup0->version == 1u || sup0->version == RDWFS_VERSION) &&
	    sup0->node_count >= 1000 &&
	    sup0->page_size == PAGE_SIZE &&
	    crc32(0, sup0, __builtin_offsetof(struct rdwfs_super, super_crc)) == sup0->super_crc) {
		ok0 = true;
	}
	if (redwood_ufs_var_io(false, RDWFS_SLOT1_OFFSET, sup1, sizeof(*sup1)) == 0 &&
	    memcmp(sup1->magic, RDWFS_MAGIC_STR, 8) == 0 &&
	    (sup1->version == 1u || sup1->version == RDWFS_VERSION) &&
	    sup1->node_count >= 1000 &&
	    sup1->page_size == PAGE_SIZE &&
	    crc32(0, sup1, __builtin_offsetof(struct rdwfs_super, super_crc)) == sup1->super_crc) {
		ok1 = true;
	}
	if (ok0 && (!ok1 || sup0->generation >= sup1->generation)) {
		if (ramfs_disk_read_slot(rm, RDWFS_SLOT0_OFFSET, win_sup)) {
			loaded = ramfs_disk_unpack(rm, win_sup);
			win_slot = 0;
		} else if (ok1 && ramfs_disk_read_slot(rm, RDWFS_SLOT1_OFFSET, win_sup)) {
			loaded = ramfs_disk_unpack(rm, win_sup);
			win_slot = 1;
		}
	} else if (ok1) {
		if (ramfs_disk_read_slot(rm, RDWFS_SLOT1_OFFSET, win_sup)) {
			loaded = ramfs_disk_unpack(rm, win_sup);
			win_slot = 1;
		} else if (ok0 && ramfs_disk_read_slot(rm, RDWFS_SLOT0_OFFSET, win_sup)) {
			loaded = ramfs_disk_unpack(rm, win_sup);
			win_slot = 0;
		}
	}

out_cleanup_supers:
	rm->rm_disk_loading = false;
	rm->rm_disk_backed = true;
	rm->rm_meta_call = thread_call_allocate(ramfs_disk_meta_thread, (thread_call_param_t)rm);
	ramfs_disk_rm = rm;
	ramfs_disk_claimed = true;

	if (loaded) {
		ramfs_var_disk_loaded = 1;
		ramfs_var_seed_done = 1;
		rm->rm_seed_done = true;
		printf("ramfs: loaded /private/var from disk (slot %u, gen %llu, %llu nodes, %llu pages)\n",
		    win_slot, win_sup != NULL ? win_sup->generation : 0ULL, rm->rm_nodes, rm->rm_pages);
	} else {
		ramfs_var_disk_loaded = 0;
		ramfs_var_seed_done = 0;
		rm->rm_seed_done = false;
		printf("ramfs: /private/var backed by UFS (%llu MB data region, fresh checkpoint)\n",
		    avail_bytes >> 20);
	}
	if (sup0 != NULL) {
		kfree_type(struct rdwfs_super, 1, sup0);
	}
	if (sup1 != NULL) {
		kfree_type(struct rdwfs_super, 1, sup1);
	}
	if (win_sup != NULL) {
		kfree_type(struct rdwfs_super, 1, win_sup);
	}
	ramfs_disk_restore_panic_log(rm);
}

static void
ramfs_disk_unmount(struct ramfs_mount *rm)
{
	if (!rm->rm_disk_backed) {
		return;
	}
	if (rm->rm_meta_call != NULL) {
		(void)thread_call_cancel_wait((thread_call_t)rm->rm_meta_call);
		(void)thread_call_free((thread_call_t)rm->rm_meta_call);
		rm->rm_meta_call = NULL;
	}
	(void)ramfs_disk_sync(rm);
	if (rm == ramfs_disk_rm) {
		ramfs_disk_rm = NULL;
		ramfs_disk_claimed = false;
	}
	if (rm->rm_bmap != NULL) {
		kfree_data(rm->rm_bmap, (size_t)rm->rm_bmap_words * sizeof(uint32_t));
		rm->rm_bmap = NULL;
	}
	if (rm->rm_io_page != NULL) {
		ramfs_page_free(rm->rm_io_page);
		rm->rm_io_page = NULL;
	}
	ramfs_vm_free(rm->rm_meta_buf, rm->rm_meta_cap);
	rm->rm_meta_buf = NULL;
	rm->rm_disk_backed = false;
}

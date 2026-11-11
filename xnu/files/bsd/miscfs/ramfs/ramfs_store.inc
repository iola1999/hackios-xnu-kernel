/*
 * ramfs_store.inc: File page allocation, storage, and persistence write helpers.
 *
 * The caller holds rm_lock from ramfs_store() through the synchronous disk
 * write and eviction. This protects the page mapping, block ownership and
 * ordering against other writes and truncation.
 */

/*
 * Copy len bytes (within one page) into the file at offset off, extending
 * the file if needed. *spare is a zeroed page the caller allocated without
 * the lock; it is consumed if the slot was empty. The mount lock is held.
 */
static int
ramfs_store(struct ramfs_node *node, uint64_t off, const void *src, size_t len, void **spare)
{
	struct ramfs_mount *rm = node->rn_mount;
	uint64_t index = off / PAGE_SIZE;
	int error = ramfs_grow_slots(node, index);

	if (error) {
		return error;
	}
	if (node->rn_pages[index] == NULL) {
		uint32_t existing_blk = node->rn_blocks != NULL ? node->rn_blocks[index] : 0;

		if (*spare == NULL || (!rm->rm_disk_backed && rm->rm_pages >= rm->rm_max_pages)) {
			return ENOSPC;
		}
		if (existing_blk != 0) {
			if (len < PAGE_SIZE) {
				error = ramfs_disk_read_block(rm, existing_blk, *spare);
				if (error) {
					return error;
				}
			}
			node->rn_pages[index] = *spare;
			*spare = NULL;
			os_atomic_inc(&ramfs_pages_in_use, relaxed);
		} else {
			if (rm->rm_disk_backed) {
				uint32_t dblk = ramfs_disk_bmap_alloc(rm);

				if (dblk == 0) {
					return ENOSPC;
				}
				node->rn_blocks[index] = dblk;
				os_atomic_inc(&ramfs_var_used_pages, relaxed);
			}
			node->rn_pages[index] = *spare;
			*spare = NULL;
			node->rn_nalloc++;
			rm->rm_pages++;
			os_atomic_inc(&ramfs_pages_in_use, relaxed);
		}
	} else if (rm->rm_disk_backed && node->rn_blocks[index] == 0) {
		uint32_t dblk = ramfs_disk_bmap_alloc(rm);

		if (dblk == 0) {
			return ENOSPC;
		}
		node->rn_blocks[index] = dblk;
		os_atomic_inc(&ramfs_var_used_pages, relaxed);
	}
	memcpy((char *)node->rn_pages[index] + (off & PAGE_MASK), src, len);
	if (off + len > node->rn_size) {
		node->rn_size = off + len;
	}
	if (rm->rm_disk_backed && node->rn_blocks[index] != 0) {
		ramfs_disk_dirty_locked(rm);
	}
	return 0;
}

static int
ramfs_store_write_locked(struct ramfs_node *node, uint64_t index)
{
	struct ramfs_mount *rm = node->rn_mount;

	if (rm->rm_disk_backed && node->rn_blocks[index] != 0) {
		void *page = node->rn_pages[index];
		int error = ramfs_disk_write_block(rm, node->rn_blocks[index], page);
		if (error) {
			return error;
		}
		ramfs_disk_evict_page_locked(rm, node, index, page);
	}
	return 0;
}

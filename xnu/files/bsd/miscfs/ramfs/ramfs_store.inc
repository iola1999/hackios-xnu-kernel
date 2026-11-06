/*
 * ramfs_store.inc: File page allocation, storage, and persistence write helpers.
 *
 * ramfs_store() allocates and writes data to a node's in-memory page slots
 * and prepares disk block writes for disk-backed mounts (RDWFS001).
 * ramfs_store_write() performs synchronous UFS block writes and memory-pressure
 * page evictions outside rm->rm_lock to prevent lock contention during bulk writes
 * and seed unpack.
 */

/*
 * Copy len bytes (within one page) into the file at offset off, extending
 * the file if needed. *spare is a zeroed page the caller allocated without
 * the lock; it is consumed if the slot was empty. The mount lock is held.
 */
static int
ramfs_store(struct ramfs_node *node, uint64_t off, const void *src, size_t len, void **spare,
    uint32_t *dblk_out, void **page_out)
{
	struct ramfs_mount *rm = node->rn_mount;
	uint64_t index = off / PAGE_SIZE;
	int error = ramfs_grow_slots(node, index);

	if (dblk_out != NULL) {
		*dblk_out = 0;
	}
	if (page_out != NULL) {
		*page_out = NULL;
	}
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
				lck_mtx_unlock(&rm->rm_lock);
				error = ramfs_disk_read_block(rm, existing_blk, *spare);
				lck_mtx_lock(&rm->rm_lock);
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
		if (dblk_out != NULL && page_out != NULL) {
			*dblk_out = node->rn_blocks[index];
			*page_out = node->rn_pages[index];
		} else {
			error = ramfs_disk_write_block(rm, node->rn_blocks[index], node->rn_pages[index]);
			if (error) {
				return error;
			}
			ramfs_disk_evict_page_locked(rm, node, index, node->rn_pages[index]);
		}
		ramfs_disk_dirty_locked(rm);
	}
	return 0;
}

static int
ramfs_store_write(struct ramfs_mount *rm, struct ramfs_node *node, uint64_t index, uint32_t dblk, void *page)
{
	if (dblk != 0 && page != NULL) {
		int error = ramfs_disk_write_block(rm, dblk, page);
		if (error) {
			return error;
		}
		ramfs_disk_evict_page(rm, node, index, page);
	}
	return 0;
}

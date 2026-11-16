/*
 * ramfs_store.inc: in-memory file page allocation and write helper.
 *
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
		if (*spare == NULL || rm->rm_pages >= rm->rm_max_pages) {
			return ENOSPC;
		}
		node->rn_pages[index] = *spare;
		*spare = NULL;
		node->rn_nalloc++;
		rm->rm_pages++;
		os_atomic_inc(&ramfs_pages_in_use, relaxed);
	}
	memcpy((char *)node->rn_pages[index] + (off & PAGE_MASK), src, len);
	if (off + len > node->rn_size) {
		node->rn_size = off + len;
	}
	return 0;
}

/* Mount recovery never writes until one complete checkpoint or empty metadata is established. */
static void
ramfs_disk_release_buffers(struct ramfs_mount *rm)
{
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
}

/* Failed unpacking can leave unlinked nodes. Walk the hash rather than the directory tree. */
static void
ramfs_disk_clear_nodes(struct ramfs_mount *rm)
{
	for (uint32_t h = 0; h < RAMFS_HASH_SIZE; h++) {
		while (rm->rm_hash[h] != NULL) {
			struct ramfs_node *node = rm->rm_hash[h];
			struct ramfs_dirent *de;
			while ((de = TAILQ_FIRST(&node->rn_entries)) != NULL) {
				TAILQ_REMOVE(&node->rn_entries, de, rd_link);
				ramfs_dirent_free(de);
			}
			ramfs_node_free(node);
		}
	}
	rm->rm_root = NULL;
	rm->rm_meta_gen = 0;
	rm->rm_next_ino = RAMFS_ROOT_INO + 1;
	if (rm->rm_bmap != NULL) {
		bzero(rm->rm_bmap, (size_t)rm->rm_bmap_words * sizeof(uint32_t));
		rm->rm_bmap[0] = 1;
	}
}

static void
ramfs_disk_empty_root(struct ramfs_mount *rm)
{
	rm->rm_root = ramfs_node_alloc(rm, VDIR, NULL);
	rm->rm_root->rn_ino = RAMFS_ROOT_INO;
	rm->rm_root->rn_nlink = 2;
	rm->rm_root->rn_parent = rm->rm_root;
	ramfs_hash_insert(rm, rm->rm_root);
	rm->rm_nodes = 1;
}

/* This checks only the two metadata slots; nonzero older formats require explicit migration. */
static int
ramfs_disk_empty_metadata(struct ramfs_mount *rm)
{
	for (uint64_t off = RDWFS_SLOT0_OFFSET; off < RDWFS_SLOT1_OFFSET + RDWFS_SLOT_MAX;) {
		uint64_t len = RDWFS_SLOT1_OFFSET + RDWFS_SLOT_MAX - off;
		if (len > rm->rm_meta_cap) len = rm->rm_meta_cap;
		int error = redwood_ufs_var_io(false, off, rm->rm_meta_buf, len);
		if (error != 0) return error;
		const uint8_t *bytes = rm->rm_meta_buf;
		for (uint64_t i = 0; i < len; i++) {
			if (bytes[i] != 0) return EINVAL;
		}
		off += len;
	}
	return 0;
}

static int
ramfs_disk_mount(struct ramfs_mount *rm)
{
	uint64_t part_bytes = 0, max_blocks;
	struct rdwfs_super *sup[2] = { NULL, NULL };
	uint64_t offsets[2] = { RDWFS_SLOT0_OFFSET, RDWFS_SLOT1_OFFSET };
	bool valid[2] = { false, false }, loaded = false;
	int error = ENOMEM;
	unsigned win_slot = 0;

	if (ramfs_disk_claimed) return EBUSY;
	if (!redwood_ufs_var_info(&part_bytes)) return ENXIO;
	if (part_bytes < RDWFS_DATA_OFFSET + 2 * PAGE_SIZE) return EINVAL;
	max_blocks = (part_bytes - RDWFS_DATA_OFFSET) / PAGE_SIZE;
	if (max_blocks > 8388608ULL) max_blocks = 8388608ULL;
	rm->rm_bmap_max = (uint32_t)max_blocks;
	rm->rm_bmap_words = (rm->rm_bmap_max + 31u) / 32u;
	rm->rm_bmap = kalloc_data((size_t)rm->rm_bmap_words * sizeof(uint32_t), Z_WAITOK | Z_ZERO);
	rm->rm_io_page = ramfs_page_alloc(true);
	rm->rm_meta_cap = RDWFS_INIT_META_CAP;
	rm->rm_meta_buf = ramfs_vm_alloc(rm->rm_meta_cap);
	if (rm->rm_bmap == NULL || rm->rm_io_page == NULL || rm->rm_meta_buf == NULL) goto fail;
	rm->rm_bmap[0] = 1;
	rm->rm_bmap_next = part_bytes >= (8ULL << 30) ? (uint32_t)((2048ULL << 20) / PAGE_SIZE) : 1u;
	rm->rm_disk_loading = true;
	for (unsigned i = 0; i < 2; i++) {
		sup[i] = kalloc_type(struct rdwfs_super, Z_WAITOK | Z_ZERO);
		if (sup[i] == NULL) { error = ENOMEM; goto fail; }
		error = redwood_ufs_var_io(false, offsets[i], sup[i], sizeof(*sup[i]));
		if (error != 0) goto fail;
		valid[i] = ramfs_disk_super_valid(sup[i]);
	}
	if (!valid[0] && !valid[1]) {
		error = ramfs_disk_empty_metadata(rm);
		if (error != 0) goto fail;
	} else {
		unsigned first = valid[0] && (!valid[1] || sup[0]->generation >= sup[1]->generation) ? 0 : 1;
		for (unsigned attempt = 0; attempt < 2; attempt++) {
			unsigned slot = first ^ attempt;
			if (!valid[slot]) continue;
			error = ramfs_disk_read_slot(rm, offsets[slot], sup[slot]);
			if (error == 0) error = ramfs_disk_unpack(rm, sup[slot]);
			if (error == 0) { loaded = true; win_slot = slot; break; }
			ramfs_disk_clear_nodes(rm);
			if (error != EINVAL) goto fail;
			ramfs_disk_empty_root(rm);
		}
		if (!loaded) goto fail;
	}
	rm->rm_meta_call = thread_call_allocate(ramfs_disk_meta_thread, (thread_call_param_t)rm);
	if (rm->rm_meta_call == NULL) { error = ENOMEM; goto fail; }
	/* Publish only after every allocation and recovery check has succeeded. */
	rm->rm_disk_loading = false;
	rm->rm_disk_backed = true;
	rm->rm_seed_done = loaded;
	rm->rm_meta_slot = loaded ? win_slot : 1;
	ramfs_var_disk_loaded = loaded ? 1 : 0;
	ramfs_var_seed_done = loaded ? 1 : 0;
	ramfs_disk_rm = rm;
	ramfs_disk_claimed = true;
	if (loaded) {
		printf("ramfs: loaded /private/var from disk (slot %u, gen %llu, %llu nodes, %llu pages)\n",
		    win_slot, rm->rm_meta_gen, rm->rm_nodes, rm->rm_pages);
	} else {
		printf("ramfs: /private/var backed by UFS (%llu MB data region, empty metadata)\n",
		    (part_bytes - RDWFS_DATA_OFFSET) >> 20);
	}
	for (unsigned i = 0; i < 2; i++) kfree_type(struct rdwfs_super, sup[i]);
	ramfs_disk_restore_panic_log(rm);
	return 0;

fail:
	for (unsigned i = 0; i < 2; i++) {
		if (sup[i] != NULL) kfree_type(struct rdwfs_super, sup[i]);
	}
	ramfs_disk_clear_nodes(rm);
	ramfs_disk_release_buffers(rm);
	rm->rm_disk_loading = false;
	return error;
}

static void
ramfs_disk_unmount(struct ramfs_mount *rm)
{
	if (!rm->rm_disk_backed) return;
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
	ramfs_disk_release_buffers(rm);
	rm->rm_disk_backed = false;
}

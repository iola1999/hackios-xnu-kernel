/* Checkpoints are accepted by format and bounds, including a root-only filesystem. */
static bool
ramfs_disk_super_valid(const struct annfs_super *sup)
{
	return memcmp(sup->magic, ANNFS_MAGIC_STR, 8) == 0 &&
	    sup->version == ANNFS_VERSION &&
	    sup->page_size == PAGE_SIZE && sup->generation > 0 && sup->generation < ~0ULL &&
	    sup->node_count > 0 && sup->payload_bytes <= ANNFS_SLOT_MAX - ANNFS_SUPER_BYTES &&
	    sup->node_count <= sup->payload_bytes / sizeof(struct annfs_inode_rec) &&
	    crc32(0, sup, __builtin_offsetof(struct annfs_super, super_crc)) == sup->super_crc;
}

static int
ramfs_disk_read_slot(struct ramfs_mount *rm, uint64_t slot_off, struct annfs_super *sup_out)
{
	struct annfs_super *sup = (struct annfs_super *)rm->rm_meta_buf;
	uint64_t pay_aligned;

	int error = annibale_ufs_var_io(false, slot_off, sup, ANNFS_SUPER_BYTES);
	if (error != 0) {
		return error;
	}
	if (!ramfs_disk_super_valid(sup)) {
		return EINVAL;
	}
	pay_aligned = ANNFS_ALIGN4K(sup->payload_bytes);
	if (ANNFS_SUPER_BYTES + pay_aligned > rm->rm_meta_cap) {
		size_t new_cap = (size_t)ANNFS_ALIGN4K(ANNFS_SUPER_BYTES + pay_aligned);
		void *new_buf = ramfs_vm_alloc(new_cap);

		if (new_buf == NULL) {
			return ENOMEM;
		}
		memcpy(new_buf, sup, ANNFS_SUPER_BYTES);
		ramfs_vm_free(rm->rm_meta_buf, rm->rm_meta_cap);
		rm->rm_meta_buf = new_buf;
		rm->rm_meta_cap = new_cap;
		sup = (struct annfs_super *)rm->rm_meta_buf;
	}
	error = annibale_ufs_var_io(false, slot_off + ANNFS_SUPER_BYTES,
	    (uint8_t *)rm->rm_meta_buf + ANNFS_SUPER_BYTES, pay_aligned);
	if (error != 0) {
		return error;
	}
	if (crc32(0, (uint8_t *)rm->rm_meta_buf + ANNFS_SUPER_BYTES, (size_t)sup->payload_bytes) != sup->payload_crc) {
		return EINVAL;
	}
	*sup_out = *sup;
	return 0;
}

static int
ramfs_disk_unpack(struct ramfs_mount *rm, const struct annfs_super *sup)
{
	const uint8_t *base = (const uint8_t *)rm->rm_meta_buf + ANNFS_SUPER_BYTES;
	uint64_t end = sup->payload_bytes, at = 0, max_ino = RAMFS_ROOT_INO;
	bool root_seen = false;

	/* Pass 1: instantiate nodes, symlink targets, regular file block tables, and xattrs. */
	for (uint32_t i = 0; i < sup->node_count; i++) {
		struct annfs_inode_rec rec;
		struct ramfs_node *node;

		if (at + sizeof(rec) > end) {
			return EINVAL;
		}
		memcpy(&rec, base + at, sizeof(rec));
		at += sizeof(rec);
		if (rec.ino < RAMFS_ROOT_INO || rec.ino == ~0ULL ||
		    rec.type == VNON || rec.type >= VBAD || rec.nlink == 0 ||
		    (rec.type != VLNK && rec.target_len != 0) ||
		    (rec.type == VLNK && rec.target_len != rec.size) ||
		    (rec.type != VREG && rec.nblocks != 0) ||
		    (rec.type != VDIR && rec.ndirents != 0)) {
			return EINVAL;
		}
		if (rec.ino > max_ino) {
			max_ino = rec.ino;
		}
		if (rec.ino == RAMFS_ROOT_INO) {
			if (root_seen || rec.type != VDIR) return EINVAL;
			root_seen = true;
			node = rm->rm_root;
		} else {
			if (ramfs_disk_find_ino(rm, rec.ino) != NULL) return EINVAL;
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
		node->rn_iflags = rec.iflags & RAMFS_IF_SYNCROOT;
		node->rn_docid = rec.document_id;
		if (rec.document_id >= rm->rm_next_docid) {
			rm->rm_next_docid = rec.document_id + 1;
		}
		node->rn_atime.tv_sec = (time_t)rec.atime_sec;
		node->rn_atime.tv_nsec = (long)rec.atime_nsec;
		node->rn_mtime.tv_sec = (time_t)rec.mtime_sec;
		node->rn_mtime.tv_nsec = (long)rec.mtime_nsec;
		node->rn_ctime.tv_sec = (time_t)rec.ctime_sec;
		node->rn_ctime.tv_nsec = (long)rec.ctime_nsec;
		node->rn_btime.tv_sec = (time_t)rec.btime_sec;
		node->rn_btime.tv_nsec = (long)rec.btime_nsec;

		if (rec.target_len > 0) {
			uint64_t alen = ANNFS_ALIGN8(rec.target_len);

			if (at + alen > end) {
				return EINVAL;
			}
			node->rn_target = kalloc_data(rec.target_len + 1, Z_WAITOK);
			if (node->rn_target == NULL) {
				return ENOMEM;
			}
			memcpy(node->rn_target, base + at, rec.target_len);
			node->rn_target[rec.target_len] = '\0';
			at += alen;
		}
		if (rec.nblocks > 0) {
			uint64_t bbytes = (uint64_t)rec.nblocks * sizeof(uint32_t);
			uint64_t alen = ANNFS_ALIGN8(bbytes);
			uint64_t want = 4;

			if (at + alen > end) {
				return EINVAL;
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
				return ENOMEM;
			}
			node->rn_nslots = want;
			for (uint32_t b = 0; b < rec.nblocks; b++) {
				uint32_t dblk;
				memcpy(&dblk, base + at + b * sizeof(dblk), sizeof(dblk));
				if (dblk >= rm->rm_bmap_max) return EINVAL;
			}
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
			struct annfs_xattr_rec xrec;
			struct ramfs_xattr *xa;
			uint64_t alen;

			if (at + sizeof(xrec) > end) {
				return EINVAL;
			}
			memcpy(&xrec, base + at, sizeof(xrec));
			at += sizeof(xrec);
			alen = ANNFS_ALIGN8((uint64_t)xrec.namelen + (uint64_t)xrec.size);
			if (at + alen > end || xrec.namelen > XATTR_MAXNAMELEN || xrec.size > RAMFS_MAX_XATTR) {
				return EINVAL;
			}
			xa = kalloc_type(struct ramfs_xattr, Z_WAITOK | Z_ZERO);
			if (xa == NULL) {
				return ENOMEM;
			}
			xa->rx_name = kalloc_data(xrec.namelen + 1, Z_WAITOK);
			if (xa->rx_name == NULL) {
				kfree_type(struct ramfs_xattr, xa);
				return ENOMEM;
			}
			memcpy(xa->rx_name, base + at, xrec.namelen);
			xa->rx_name[xrec.namelen] = '\0';
			xa->rx_namelen = xrec.namelen;
			if (xrec.size > 0) {
				xa->rx_data = kalloc_data(xrec.size, Z_WAITOK);
				if (xa->rx_data == NULL) {
					kfree_data(xa->rx_name, xrec.namelen + 1);
					kfree_type(struct ramfs_xattr, xa);
					return ENOMEM;
				}
				memcpy(xa->rx_data, base + at + xrec.namelen, xrec.size);
			}
			xa->rx_size = xrec.size;
			TAILQ_INSERT_TAIL(&node->rn_xattrs, xa, rx_link);
			at += alen;
		}
		for (uint32_t d = 0; d < rec.ndirents; d++) {
			struct annfs_dirent_rec drec;

			if (at + sizeof(drec) > end) {
				return EINVAL;
			}
			memcpy(&drec, base + at, sizeof(drec));
			at += sizeof(drec) + ANNFS_ALIGN8(drec.namelen);
			if (at > end || drec.namelen == 0 || drec.namelen > RAMFS_MAX_NAME) return EINVAL;
		}
	}

	if (!root_seen || at != end) return EINVAL;

	/* Pass 2: link directory parent pointers and directory entries. */
	at = 0;
	for (uint32_t i = 0; i < sup->node_count; i++) {
		struct annfs_inode_rec rec;
		struct ramfs_node *node;

		memcpy(&rec, base + at, sizeof(rec));
		at += sizeof(rec);
		if (rec.target_len > 0) {
			at += ANNFS_ALIGN8(rec.target_len);
		}
		if (rec.nblocks > 0) {
			at += ANNFS_ALIGN8((uint64_t)rec.nblocks * sizeof(uint32_t));
		}
		for (uint32_t x = 0; x < rec.nxattrs; x++) {
			struct annfs_xattr_rec xrec;

			memcpy(&xrec, base + at, sizeof(xrec));
			at += sizeof(xrec) + ANNFS_ALIGN8((uint64_t)xrec.namelen + (uint64_t)xrec.size);
		}
		node = ramfs_disk_find_ino(rm, rec.ino);
		if (node != NULL && node->rn_type == VDIR) {
			struct ramfs_node *parent = ramfs_disk_find_ino(rm, rec.parent_ino);

			if (parent == NULL || parent->rn_type != VDIR) return EINVAL;
			node->rn_parent = parent;
		}
		for (uint32_t d = 0; d < rec.ndirents; d++) {
			struct annfs_dirent_rec drec;
			struct ramfs_node *child;
			uint64_t alen;

			memcpy(&drec, base + at, sizeof(drec));
			at += sizeof(drec);
			alen = ANNFS_ALIGN8(drec.namelen);
			child = ramfs_disk_find_ino(rm, drec.child_ino);
			if (node == NULL || node->rn_type != VDIR || child == NULL) return EINVAL;
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
	return 0;
}

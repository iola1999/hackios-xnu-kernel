/*
 * ramfs_clonefile.inc: vnop_clonefile implementation for ramfs.
 * Supports cloning regular files, symbolic links, and directories within the volume.
 */

#include <sys/clonefile.h>

static void ramfs_free_tree(struct ramfs_node *dir);
static int ramfs_clone_contents(struct ramfs_mount *rm, struct ramfs_node *src, struct ramfs_node *node, uint32_t flags);

static int
ramfs_clone_contents(struct ramfs_mount *rm, struct ramfs_node *src, struct ramfs_node *node, uint32_t flags)
{
	struct ramfs_xattr *rx;
	int error = 0;

	node->rn_size = src->rn_size;
	node->rn_dataprotect_class = src->rn_dataprotect_class;

	if (src->rn_type == VREG) {
		if (src->rn_nslots > 0) {
			error = ramfs_grow_slots(node, src->rn_nslots - 1);
			if (error != 0) {
				return error;
			}
			for (uint64_t i = 0; i < src->rn_nslots; i++) {
				if (src->rn_pages != NULL && src->rn_pages[i] == NULL &&
				    src->rn_blocks != NULL && src->rn_blocks[i] != 0) {
					ramfs_disk_fault_page(src, i);
				}
				if (src->rn_pages != NULL && src->rn_pages[i] != NULL) {
					void *new_page = ramfs_page_alloc(false);
					if (new_page == NULL) {
						return ENOSPC;
					}
					memcpy(new_page, src->rn_pages[i], PAGE_SIZE);
					node->rn_pages[i] = new_page;
					node->rn_nalloc++;
					rm->rm_pages++;
					os_atomic_inc(&ramfs_pages_in_use, relaxed);

					if (rm->rm_disk_backed) {
						uint32_t dblk = ramfs_disk_bmap_alloc(rm);
						if (dblk == 0) {
							return ENOSPC;
						}
						node->rn_blocks[i] = dblk;
						os_atomic_inc(&ramfs_var_used_pages, relaxed);
						error = ramfs_disk_write_block(rm, dblk, new_page);
						if (error != 0) {
							return error;
						}
					}
				}
			}
		}
	} else if (src->rn_type == VLNK) {
		if (src->rn_target != NULL) {
			node->rn_target = kalloc_data(node->rn_size + 1, Z_WAITOK);
			if (node->rn_target == NULL) {
				return ENOMEM;
			}
			memcpy(node->rn_target, src->rn_target, node->rn_size + 1);
		}
	} else if (src->rn_type == VDIR) {
		struct ramfs_dirent *sde;
		TAILQ_FOREACH(sde, &src->rn_entries, rd_link) {
			struct ramfs_node *cnode = ramfs_node_alloc(rm, sde->rd_node->rn_type, NULL);
			struct ramfs_dirent *cde = ramfs_dirent_alloc(sde->rd_name, sde->rd_namelen);

			rm->rm_nodes++;
			cnode->rn_ino = rm->rm_next_ino++;
			ramfs_hash_insert(rm, cnode);
			cnode->rn_mode = sde->rd_node->rn_mode;
			if (!(flags & CLONE_NOOWNERCOPY)) {
				cnode->rn_uid = sde->rd_node->rn_uid;
				cnode->rn_gid = sde->rd_node->rn_gid;
			}
			if (cnode->rn_type == VDIR) {
				cnode->rn_nlink = 2;
				cnode->rn_parent = node;
				node->rn_nlink++;
			} else {
				cnode->rn_nlink = 1;
			}
			error = ramfs_clone_contents(rm, sde->rd_node, cnode, flags);
			if (error != 0) {
				ramfs_dirent_free(cde);
				ramfs_node_free(cnode);
				return error;
			}
			ramfs_dir_add(node, cde, cnode);
		}
	}

	TAILQ_FOREACH(rx, &src->rn_xattrs, rx_link) {
		struct ramfs_xattr *new_rx = kalloc_type(struct ramfs_xattr, Z_WAITOK | Z_ZERO);
		if (new_rx == NULL) {
			return ENOMEM;
		}
		new_rx->rx_namelen = rx->rx_namelen;
		new_rx->rx_size = rx->rx_size;
		new_rx->rx_name = kalloc_data(rx->rx_namelen + 1, Z_WAITOK);
		if (new_rx->rx_name == NULL) {
			kfree_type(struct ramfs_xattr, new_rx);
			return ENOMEM;
		}
		memcpy(new_rx->rx_name, rx->rx_name, rx->rx_namelen + 1);
		if (rx->rx_size > 0 && rx->rx_data != NULL) {
			new_rx->rx_data = kalloc_data(rx->rx_size, Z_WAITOK);
			if (new_rx->rx_data == NULL) {
				kfree_data(new_rx->rx_name, rx->rx_namelen + 1);
				kfree_type(struct ramfs_xattr, new_rx);
				return ENOMEM;
			}
			memcpy(new_rx->rx_data, rx->rx_data, rx->rx_size);
		}
		TAILQ_INSERT_TAIL(&node->rn_xattrs, new_rx, rx_link);
	}
	return 0;
}

static int
ramfs_clonefile(struct vnop_clonefile_args *ap)
{
	vnode_t fvp = ap->a_fvp;
	vnode_t dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct ramfs_node *src, *dir, *node;
	struct ramfs_mount *rm;
	struct ramfs_dirent *de;
	vnode_t out_vp = NULLVP;
	vnode_t *vpp_target;
	int error;

	if (fvp == NULLVP || dvp == NULLVP || cnp == NULL) {
		return EINVAL;
	}
	if (vnode_mount(fvp) != vnode_mount(dvp)) {
		return EXDEV;
	}
	src = RAMFS_NODE(fvp);
	dir = RAMFS_NODE(dvp);
	rm = dir->rn_mount;

	if (dir->rn_type != VDIR) {
		return ENOTDIR;
	}
	if (src->rn_type != VREG && src->rn_type != VLNK && src->rn_type != VDIR) {
		return ENOTSUP;
	}
	if (cnp->cn_namelen > RAMFS_MAX_NAME) {
		return ENAMETOOLONG;
	}
	if (src->rn_mapped_write && fvp != NULLVP) {
		(void)ubc_msync(fvp, 0, ubc_getsize(fvp), NULL, UBC_PUSHDIRTY | UBC_SYNC);
	}

	node = ramfs_node_alloc(rm, src->rn_type, ap->a_vap);
	de = ramfs_dirent_alloc(cnp->cn_nameptr, (size_t)cnp->cn_namelen);

	lck_mtx_lock(&rm->rm_lock);
	rm->rm_nodes++;
	if (dir->rn_nlink == 0) {
		error = ENOENT;
	} else if (ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen) != NULL) {
		error = EEXIST;
	} else {
		error = 0;
	}
	if (error != 0) {
		ramfs_node_free(node);
		lck_mtx_unlock(&rm->rm_lock);
		ramfs_dirent_free(de);
		return error;
	}

	node->rn_ino = rm->rm_next_ino++;
	ramfs_hash_insert(rm, node);
	if (src->rn_type == VDIR) {
		node->rn_nlink = 2;
		node->rn_parent = dir;
		dir->rn_nlink++;
	} else {
		node->rn_nlink = 1;
	}
	if (ap->a_vap == NULL || !VATTR_IS_ACTIVE(ap->a_vap, va_mode)) {
		node->rn_mode = src->rn_mode;
	}
	if (!(ap->a_flags & CLONE_NOOWNERCOPY)) {
		node->rn_uid = src->rn_uid;
		node->rn_gid = src->rn_gid;
	}

	error = ramfs_clone_contents(rm, src, node, ap->a_flags);
	if (error != 0) {
		if (src->rn_type == VDIR) {
			dir->rn_nlink--;
			ramfs_free_tree(node);
		}
		ramfs_node_free(node);
		lck_mtx_unlock(&rm->rm_lock);
		ramfs_dirent_free(de);
		return error;
	}

	if (rm->rm_disk_backed) {
		ramfs_disk_dirty_locked(rm);
	}
	ramfs_dir_add(dir, de, node);

	vpp_target = (ap->a_vpp != NULL) ? ap->a_vpp : &out_vp;
	error = ramfs_vget_locked(rm, node, dvp, cnp, vpp_target);
	if (error != 0) {
		lck_mtx_lock(&rm->rm_lock);
		ramfs_dir_remove(dir, de);
		if (src->rn_type == VDIR) {
			dir->rn_nlink--;
			ramfs_free_tree(node);
		}
		ramfs_node_free(node);
		lck_mtx_unlock(&rm->rm_lock);
		return error;
	}
	if (ap->a_vpp == NULL && out_vp != NULLVP) {
		vnode_put(out_vp);
	}
	return 0;
}

/*
 * Extended attributes of ramfs nodes, kept in a list on the node (included by ramfs.c, which
 * defines struct ramfs_xattr, ramfs_xattr_free and the mount lock these functions take).
 */

static struct ramfs_xattr *
ramfs_xattr_find(struct ramfs_node *node, const char *name)
{
	struct ramfs_xattr *xa;
	size_t len = strlen(name);

	TAILQ_FOREACH(xa, &node->rn_xattrs, rx_link) {
		if (xa->rx_namelen == len && memcmp(xa->rx_name, name, len) == 0) {
			return xa;
		}
	}
	return NULL;
}

static int
ramfs_getxattr(struct vnop_getxattr_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;
	struct ramfs_xattr *xa;
	void *copy = NULL;
	size_t size = 0, held = 0;
	int error;

	/*
	 * The copy is made with the lock dropped, where the allocation may wait: a Z_NOWAIT one under
	 * the lock answered ENOMEM in bursts of reads (kernel-12377-cmd30-warm4, var_store restoring
	 * access lists). The attribute may change meanwhile, so it is looked up again after.
	 */
	for (;;) {
		lck_mtx_lock(&rm->rm_lock);
		xa = ramfs_xattr_find(node, ap->a_name);
		error = xa == NULL ? ENOATTR : 0;
		size = xa != NULL ? xa->rx_size : 0;
		if (error || ap->a_uio == NULL || size == 0 || (size_t)uio_resid(ap->a_uio) < size || held >= size) {
			break;
		}
		lck_mtx_unlock(&rm->rm_lock);
		if (copy != NULL) {
			kfree_data(copy, held);
		}
		held = size;
		copy = kalloc_data(held, Z_WAITOK);
		if (copy == NULL) {
			error = ENOMEM;
			lck_mtx_lock(&rm->rm_lock);
			break;
		}
	}
	if (!error && ap->a_uio != NULL && (size_t)uio_resid(ap->a_uio) < size) {
		error = ERANGE;
	} else if (!error && ap->a_uio != NULL && size != 0) {
		memcpy(copy, xa->rx_data, size);
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (!error && ap->a_uio == NULL) {
		*ap->a_size = size;
	} else if (!error && size != 0) {
		error = uiomove(copy, (int)size, ap->a_uio);
	}
	if (copy != NULL) {
		kfree_data(copy, held);
	}
	return error;
}

static int
ramfs_setxattr(struct vnop_setxattr_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;
	struct ramfs_xattr *xa, *fresh, *old = NULL;
	size_t size = (size_t)uio_resid(ap->a_uio);
	size_t namelen = strlen(ap->a_name);
	void *data = NULL;
	int error = 0;

	if (size > RAMFS_MAX_XATTR || namelen > XATTR_MAXNAMELEN) {
		return E2BIG;
	}
	if (size != 0) {
		data = kalloc_data(size, Z_WAITOK);
		if (data == NULL) {
			return ENOMEM;
		}
		error = uiomove(data, (int)size, ap->a_uio);
		if (error) {
			kfree_data(data, size);
			return error;
		}
	}
	fresh = kalloc_type(struct ramfs_xattr, Z_WAITOK | Z_ZERO);
	if (fresh == NULL) {
		if (data != NULL) {
			kfree_data(data, size);
		}
		return ENOMEM;
	}
	fresh->rx_name = kalloc_data(namelen + 1, Z_WAITOK);
	if (fresh->rx_name == NULL) {
		kfree_type(struct ramfs_xattr, fresh);
		if (data != NULL) {
			kfree_data(data, size);
		}
		return ENOMEM;
	}
	memcpy(fresh->rx_name, ap->a_name, namelen + 1);
	fresh->rx_namelen = namelen;
	fresh->rx_data = data;
	fresh->rx_size = size;

	lck_mtx_lock(&rm->rm_lock);
	xa = ramfs_xattr_find(node, ap->a_name);
	if (xa != NULL && (ap->a_options & XATTR_CREATE)) {
		error = EEXIST;
	} else if (xa == NULL && (ap->a_options & XATTR_REPLACE)) {
		error = ENOATTR;
	} else {
		if (xa != NULL) {
			TAILQ_REMOVE(&node->rn_xattrs, xa, rx_link);
			old = xa;
		}
		TAILQ_INSERT_TAIL(&node->rn_xattrs, fresh, rx_link);
		fresh = NULL;
		ramfs_touch(node, false);
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (fresh != NULL) {
		ramfs_xattr_free(fresh);
	}
	if (old != NULL) {
		ramfs_xattr_free(old);
	}
	return error;
}

static int
ramfs_removexattr(struct vnop_removexattr_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;
	struct ramfs_xattr *xa;

	lck_mtx_lock(&rm->rm_lock);
	xa = ramfs_xattr_find(node, ap->a_name);
	if (xa != NULL) {
		TAILQ_REMOVE(&node->rn_xattrs, xa, rx_link);
		ramfs_touch(node, false);
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (xa == NULL) {
		return ENOATTR;
	}
	ramfs_xattr_free(xa);
	return 0;
}

/* The names, each with its terminator, one after another; copied as ramfs_getxattr copies. */
static int
ramfs_listxattr(struct vnop_listxattr_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;
	struct ramfs_xattr *xa;
	char *copy = NULL;
	size_t total, at = 0, held = 0;
	int error = 0;

	for (;;) {
		lck_mtx_lock(&rm->rm_lock);
		total = 0;
		TAILQ_FOREACH(xa, &node->rn_xattrs, rx_link) {
			total += xa->rx_namelen + 1;
		}
		if (ap->a_uio == NULL || total == 0 || (size_t)uio_resid(ap->a_uio) < total || held >= total) {
			break;
		}
		lck_mtx_unlock(&rm->rm_lock);
		if (copy != NULL) {
			kfree_data(copy, held);
		}
		held = total;
		copy = kalloc_data(held, Z_WAITOK);
		if (copy == NULL) {
			error = ENOMEM;
			lck_mtx_lock(&rm->rm_lock);
			break;
		}
	}
	if (ap->a_uio != NULL && total != 0 && (size_t)uio_resid(ap->a_uio) < total) {
		error = ERANGE;
	} else if (ap->a_uio != NULL && total != 0) {
		TAILQ_FOREACH(xa, &node->rn_xattrs, rx_link) {
			memcpy(copy + at, xa->rx_name, xa->rx_namelen + 1);
			at += xa->rx_namelen + 1;
		}
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (!error && ap->a_uio == NULL) {
		*ap->a_size = total;
	} else if (!error && total != 0) {
		error = uiomove(copy, (int)total, ap->a_uio);
	}
	if (copy != NULL) {
		kfree_data(copy, held);
	}
	return error;
}

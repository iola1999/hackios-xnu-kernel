#ifndef _REDWOOD_UFS_RW_INC_
#define _REDWOOD_UFS_RW_INC_

/*
 * Bytes [offset, offset + len) of partition part into dst, whatever their alignment, through the
 * loader: the native blocks that cover them are read a buffer at a time and the wanted bytes
 * copied out.
 */
static int
ufs_read_mailbox(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	uint64_t nbs = ufs_page->part[part].block_size, per = ufs_buffer_size / nbs;
	static uint8_t *bounce;
	int error = 0;

	if (bounce == NULL) {
		bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
		if (bounce == NULL) {
			return ENOMEM;
		}
	}
	lck_mtx_lock(&ufs_lock);
	if (ufshc_list_in()) {
		/* this kernel's own transfer list is in the controller: UEFI's driver must not be asked to ring */
		if (ufs_mailbox_refused++ < 4) {
			printf("ufs: %s: not asked of the loader while this kernel's own transfer list is in\n",
			    ufs_page->part[part].name);
		}
		lck_mtx_unlock(&ufs_lock);
		return EIO;
	}
	while (len > 0 && error == 0) {
		uint64_t first = offset / nbs, skip = offset % nbs;
		uint64_t count = (skip + len + nbs - 1) / nbs, take;

		if (count > per) {
			count = per;
		}
		take = count * nbs - skip;
		if (take > len) {
			take = len;
		}
		if (skip == 0 && take == count * nbs) {
			error = ufs_request(BLOCK_OP_READ, part, first, count, dst, NULL);   /* whole blocks: straight in */
		} else {
			error = ufs_request(BLOCK_OP_READ, part, first, count, bounce, NULL);
			if (error == 0) {
				memcpy(dst, bounce + skip, (size_t)take);
			}
		}
		offset += take;
		dst += take;
		len -= take;
	}
	lck_mtx_unlock(&ufs_lock);
	return error;
}

/* The same, through this kernel's own driver: the partition's unit, from where it begins there */
static int
ufs_read_native(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	if (part >= ufs_parts || ufs_lun[part] == 0xff) {
		return ENXIO;
	}
	return ufshc_read(ufs_lun[part], ufs_lun_offset[part] + offset, dst, len);
}

/* The switches of lists, with the loader's mailbox held: nothing is asked of UEFI meanwhile */
static int
ufs_native_in(bool prove)
{
	int error;

	lck_mtx_lock(&ufs_lock);
	/* (a loader that did not answer may still be inside UEFI's driver) */
	error = ufs_dead ? EIO : ufshc_switch_in(prove);
	lck_mtx_unlock(&ufs_lock);
	return error;
}

static int
ufs_native_out(const char *why)
{
	int error;

	lck_mtx_lock(&ufs_lock);
	error = ufshc_switch_out(why);
	lck_mtx_unlock(&ufs_lock);
	return error;
}

/* The way chosen (ufs_native=, kern.redwood.ufs_native); if this kernel's own fails, the loader's */
static int
ufs_read(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	if (ufs_native_on && ufs_lun[part] != 0xff) {
		int error = ufs_read_native(part, offset, dst, len);

		if (error == 0) {
			return 0;
		}
		if (ufs_write_busy) {
			/* the list is out for a write of ours, not broken: this read goes the other way */
			return ufs_read_mailbox(part, offset, dst, len);
		}
		if (ufs_native_on) {
			ufs_native_on = false;
			ufs_native_fallbacks++;
			printf("ufs: %s: bytes %llu..%llu: this kernel's own read failed (%d); through the loader from now on\n",
			    ufs_page->part[part].name, offset, offset + len - 1, error);
		}
		ufs_native_out("a read failed");
	}
	return ufs_read_mailbox(part, offset, dst, len);
}

/*
 * Whole blocks [offset, offset + len) of the partition that takes writes, from src.
 * For ufs_darwin_var, writes are routed directly through ufshc_write to native hardware
 * multi-slot queues without dropping ufs_native_on.
 * For legacy DSU store, its transfer list goes out to UEFI driver as before.
 */
static int
ufs_write(uint32_t part, uint64_t offset, const uint8_t *src, uint64_t len)
{
	uint64_t nbs, per;
	int error = 0;

	if (!ufs_ready || part >= ufs_parts) {
		ufs_write_refused++;
		return EROFS;
	}

	if (ufs_darwin_var_part >= 0 && part == (uint32_t)ufs_darwin_var_part) {
		nbs = ufs_page->part[part].block_size;
		if (len == 0 || offset % nbs != 0 || len % nbs != 0) {
			return EINVAL;
		}
		if (offset > ufs_part_bytes(part) || len > ufs_part_bytes(part) - offset) {
			ufs_write_refused++;
			return EROFS;
		}
		if (ufs_native_on && ufs_lun[part] != 0xff) {
			return ufshc_write(ufs_lun[part], ufs_lun_offset[part] + offset, src, len);
		}
		per = ufs_buffer_size / nbs;
		lck_mtx_lock(&ufs_lock);
		while (len > 0 && error == 0) {
			uint64_t count = len / nbs;
			if (count > per) {
				count = per;
			}
			error = ufs_request(BLOCK_OP_WRITE, part, offset / nbs, count, NULL, src);
			offset += count * nbs;
			src += count * nbs;
			len -= count * nbs;
		}
		lck_mtx_unlock(&ufs_lock);
		if (error != 0) {
			ufs_write_errors++;
		}
		return error;
	}

	if (ufs_write_part < 0 || part != (uint32_t)ufs_write_part) {
		ufs_write_refused++;
		return EROFS;
	}
	nbs = ufs_page->part[part].block_size;
	if (len == 0 || offset % nbs != 0 || len % nbs != 0) {
		return EINVAL;          /* whole blocks only: nothing is read before a write */
	}
	if (offset > ufs_part_bytes(part) || len > ufs_part_bytes(part) - offset) {
		/* past the store's end: never */
		ufs_write_refused++;
		return EROFS;
	}
	per = ufs_buffer_size / nbs;
	lck_mtx_lock(&ufs_write_mtx);
	/*
	 * The list goes out once, at the first write, and comes back when the device is closed: a
	 * whole data volume is written a bufferful at a time, and switching the controller's list
	 * for each of those would say so a hundred times over and buy nothing. ufs_native_on is
	 * cleared first, so that a reader about to start goes the other way of its own accord; one
	 * already inside this kernel's driver gets an error and is sent the other way by
	 * ufs_write_busy.
	 */
	if (!ufs_write_out) {
		ufs_write_was_native = ufs_native_on;
		ufs_write_busy = true;
		ufs_write_out = true;
		if (ufs_write_was_native) {
			ufs_native_on = false;
			ufs_native_out("writing the data volume into the DSU store");
		}
	}
	while (len > 0 && error == 0) {
		uint64_t count = len / nbs;

		if (count > per) {
			count = per;
		}
		lck_mtx_lock(&ufs_lock);
		if (ufshc_list_in()) {
			/* it did not go out after all: UEFI's driver must not be asked to ring */
			error = EIO;
		} else {
			error = ufs_request(BLOCK_OP_WRITE, part, offset / nbs, count, NULL, src);
		}
		lck_mtx_unlock(&ufs_lock);
		offset += count * nbs;
		src += count * nbs;
		len -= count * nbs;
	}
	lck_mtx_unlock(&ufs_write_mtx);
	if (error != 0) {
		ufs_write_errors++;
	}
	return error;
}

/* The writing over: this kernel's own transfer list back in, and reads through it again. */
static void
ufs_write_restore(void)
{
	lck_mtx_lock(&ufs_write_mtx);
	if (ufs_write_out) {
		if (ufs_write_was_native && ufs_native_in(false) == 0) {
			ufs_native_on = true;
		}
		ufs_write_busy = false;
		ufs_write_out = false;
	}
	lck_mtx_unlock(&ufs_write_mtx);
}

/* ---- the devices ---- */

static int
ufs_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	uint32_t part = (uint32_t)minor(dev);

	if (!ufs_ready || part >= ufs_parts || (ufs_page->part[part].flags & BLOCK_PART_UNIT)) {
		return ENXIO;
	}
	if ((flags & FWRITE) && (ufs_darwin_var_part < 0 || part != (uint32_t)ufs_darwin_var_part)) {
		return EROFS;           /* block devices of this driver never write unless it's darwin_var */
	}
	return 0;
}

/* The same, for the character devices: the partitions that take writes may be opened for them. */
static int
ufs_copen(dev_t dev, int flags, int devtype, proc_t p)
{
	uint32_t part = (uint32_t)minor(dev);

	if (flags & FWRITE) {
		if (ufs_write_part >= 0 && part == (uint32_t)ufs_write_part) {
			flags &= ~FWRITE;
		} else if (ufs_darwin_var_part >= 0 && part == (uint32_t)ufs_darwin_var_part) {
			/* permit character write open for darwin_var */
		}
	}
	return ufs_open(dev, flags, devtype, p);
}

static int
ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

/* The last close of /dev/rufs_dsu_store ends the writing: this kernel's own transfer list goes back in. */
static int
ufs_cclose(dev_t dev, int flags, int devtype, proc_t p)
{
	if (ufs_write_part >= 0 && (uint32_t)minor(dev) == (uint32_t)ufs_write_part) {
		ufs_write_restore();
	}
	return ufs_close(dev, flags, devtype, p);
}

static void
ufs_strategy(buf_t bp)
{
	uint32_t part = (uint32_t)minor(buf_device(bp));
	uint64_t offset, len, size;
	int error;

	if (!ufs_ready || part >= ufs_parts) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	size = ufs_part_bytes(part);
	offset = (uint64_t)buf_blkno(bp) * ufs_bsize[part];
	len = buf_count(bp);
	if (offset >= size) {
		buf_seterror(bp, offset == size ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (offset + len > size) {
		len = size - offset;
	}
	if (!(buf_flags(bp) & B_READ)) {
		if (ufs_darwin_var_part >= 0 && part == (uint32_t)ufs_darwin_var_part) {
			error = ufs_write(part, offset, (const uint8_t *)buf_dataptr(bp), len);
			buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : len)));
			if (error) {
				buf_seterror(bp, error);
			}
			buf_biodone(bp);
			return;
		}
		buf_seterror(bp, EROFS);         /* this device never writes */
		buf_biodone(bp);
		return;
	}
	error = ufs_read(part, offset, (uint8_t *)buf_dataptr(bp), len);
	buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : len)));
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	uint32_t part = (uint32_t)minor(dev), native;

	if (!ufs_ready || part >= ufs_parts) {
		return ENXIO;
	}
	native = ufs_page->part[part].block_size;
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = ufs_bsize[part];
		return 0;
	case DKIOCSETBLOCKSIZE:
		/*
		 * spec_open() sets the block size to 512 to count the device in 512-byte blocks and
		 * then back, and treats a refusal as a broken device; reads are by byte here, so any
		 * power of two from 512 to the native block size will do.
		 */
		if (*(uint32_t *)data < 512 || *(uint32_t *)data > native ||
		    (*(uint32_t *)data & (*(uint32_t *)data - 1)) != 0) {
			return EINVAL;
		}
		ufs_bsize[part] = *(uint32_t *)data;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = native;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = ufs_part_bytes(part) / ufs_bsize[part];
		return 0;
	case DKIOCISWRITABLE:
		if (ufs_darwin_var_part >= 0 && part == (uint32_t)ufs_darwin_var_part) {
			*(uint32_t *)data = 1;
			return 0;
		}
		*(uint32_t *)data = 0;  /* read only for other partitions */
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = ufs_buffer_size / ufs_bsize[part];
		return 0;
	default:
		return ENOTTY;
	}
}

static int
ufs_psize(dev_t dev)
{
	uint32_t part = (uint32_t)minor(dev);

	return ufs_ready && part < ufs_parts ? (int)ufs_bsize[part] : -1;
}

/*
 * A read of /dev/rufs_NAME: a bufferful at a time into memory of this kernel's and out to the
 * reader with uiomove. Not physio, which hands the driver the reader's own address to copy to --
 * and this kernel may not touch user memory itself (PAN): the copy has to go through uiomove.
 * Nothing of this driver is held while it does, for a fault in the reader's memory can come back
 * here as a read of the storage.
 */
static int
ufs_cread(dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t part = (uint32_t)minor(dev);
	uint64_t size;
	uint8_t *bounce;
	int error = 0;

	if (!ufs_ready || part >= ufs_parts) {
		return ENXIO;
	}
	size = ufs_part_bytes(part);
	if (uio_offset(uio) < 0) {
		return EINVAL;
	}
	bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		uint64_t at = (uint64_t)uio_offset(uio), want = (uint64_t)uio_resid(uio);

		if (want > ufs_buffer_size) {
			want = ufs_buffer_size;
		}
		if (want > size - at) {
			want = size - at;
		}
		error = ufs_read(part, at, bounce, want);
		if (error == 0) {
			error = uiomove((caddr_t)bounce, (int)want, uio);
		}
	}
	kfree_data(bounce, ufs_buffer_size);
	return error;
}

/*
 * A write to /dev/rufs_darwin_var or /dev/rufs_dsu_store: whole blocks, out of user memory
 * a bufferful at a time and into the partition.
 */
static int
ufs_cwrite(dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t part = (uint32_t)minor(dev);
	uint64_t nbs;
	uint8_t *bounce;
	int error = 0;

	if (!ufs_ready || part >= ufs_parts) {
		return ENXIO;
	}

	if (ufs_darwin_var_part >= 0 && part == (uint32_t)ufs_darwin_var_part) {
		nbs = ufs_page->part[part].block_size;
		if (uio_offset(uio) < 0 || (uint64_t)uio_offset(uio) % nbs != 0 || (uint64_t)uio_resid(uio) % nbs != 0) {
			return EINVAL;
		}
		bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
		if (bounce == NULL) {
			return ENOMEM;
		}
		while (error == 0 && uio_resid(uio) > 0) {
			user_ssize_t want = uio_resid(uio);
			off_t at = uio_offset(uio);

			if (want > (user_ssize_t)ufs_buffer_size) {
				want = (user_ssize_t)ufs_buffer_size;
			}
			error = uiomove((caddr_t)bounce, (int)want, uio);
			if (error == 0) {
				error = ufs_write(part, (uint64_t)at, bounce, (uint64_t)want);
				if (error != 0) {
					uio_setoffset(uio, at);
					uio_setresid(uio, uio_resid(uio) + want);
				}
			}
		}
		kfree_data(bounce, ufs_buffer_size);
		return error;
	}

	if (ufs_write_part < 0 || part != (uint32_t)ufs_write_part) {
		return EROFS;
	}
	if (ufs_var_busy_pid != proc_selfpid()) {
		/* one writer of the store at a time, and it says so first (kern.redwood.var_busy) */
		ufs_write_refused++;
		return EPERM;
	}
	nbs = ufs_page->part[part].block_size;
	if (uio_offset(uio) < 0 || (uint64_t)uio_offset(uio) % nbs != 0 || (uint64_t)uio_resid(uio) % nbs != 0) {
		return EINVAL;
	}
	bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0) {
		user_ssize_t want = uio_resid(uio);
		off_t at = uio_offset(uio);

		if (want > (user_ssize_t)ufs_buffer_size) {
			want = (user_ssize_t)ufs_buffer_size;
		}
		error = uiomove((caddr_t)bounce, (int)want, uio);
		if (error == 0) {
			error = ufs_write(part, (uint64_t)at, bounce, (uint64_t)want);
			if (error != 0) {
				/* these bytes did not go in: the write is to say so, not to count them */
				uio_setoffset(uio, at);
				uio_setresid(uio, uio_resid(uio) + want);
			}
		}
	}
	kfree_data(bounce, ufs_buffer_size);
	return error;
}

bool
redwood_ufs_var_info(uint64_t *size_bytes_out)
{
	int part = ufs_darwin_var_part >= 0 ? ufs_darwin_var_part : ufs_write_part;

	if (!ufs_ready || part < 0 || (uint32_t)part >= ufs_parts) {
		return false;
	}
	if (size_bytes_out != NULL) {
		*size_bytes_out = ufs_part_bytes((uint32_t)part);
	}
	return true;
}

int
redwood_ufs_var_io(bool write, uint64_t offset, void *buf, uint64_t len)
{
	int part = ufs_darwin_var_part >= 0 ? ufs_darwin_var_part : ufs_write_part;

	if (!ufs_ready || part < 0 || (uint32_t)part >= ufs_parts) {
		return ENXIO;
	}
	if (write) {
		int error = ufs_write((uint32_t)part, offset, (const uint8_t *)buf, len);
		if (part == ufs_write_part) {
			ufs_write_restore();
		}
		return error;
	}
	return ufs_read((uint32_t)part, offset, (uint8_t *)buf, len);
}

static const struct bdevsw ufs_bdevsw = {
	.d_open     = ufs_open,
	.d_close    = ufs_close,
	.d_strategy = ufs_strategy,
	.d_ioctl    = ufs_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw ufs_cdevsw = {
	.d_open       = ufs_copen,
	.d_close      = ufs_cclose,
	.d_read       = ufs_cread,
	.d_write      = ufs_cwrite,
	.d_ioctl      = ufs_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

#endif /* _REDWOOD_UFS_RW_INC_ */

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

extern boolean_t ml_get_interrupts_enabled(void);

int
redwood_ufs_var_io(bool write, uint64_t offset, void *buf, uint64_t len)
{
	int part = ufs_darwin_var_part >= 0 ? ufs_darwin_var_part : ufs_write_part;

	if (!ml_get_interrupts_enabled()) {
		return EBUSY;
	}
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

int
redwood_ufs_part_read(const char *part_name, uint64_t offset, void *buf, uint64_t len)
{
	if (!ufs_ready || part_name == NULL || buf == NULL || len == 0) {
		return ENXIO;
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		if ((ufs_page->part[i].flags & BLOCK_PART_UNIT) == 0 &&
		    strcmp(ufs_page->part[i].name, part_name) == 0) {
			uint64_t pbytes = ufs_part_bytes(i);
			if (offset >= pbytes || len > pbytes - offset) {
				return EINVAL;
			}
			return ufs_read(i, offset, (uint8_t *)buf, len);
		}
	}
	return ENOENT;
}

static bool
ufs_fat83_name(const char *name, char out11[11])
{
	memset(out11, ' ', 11);
	const char *dot = strchr(name, '.');
	size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
	if (base_len == 0 || base_len > 8) return false;
	for (size_t i = 0; i < base_len; i++) {
		char c = name[i];
		out11[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
	}
	if (dot != NULL) {
		const char *ext = dot + 1;
		size_t ext_len = strlen(ext);
		if (ext_len > 3) return false;
		for (size_t i = 0; i < ext_len; i++) {
			char c = ext[i];
			out11[8 + i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
		}
	}
	return true;
}

static bool
ufs_lfn_eq(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
		if (ca != cb) return false;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static void
ufs_lfn_feed(const uint8_t *ent, char lfn[256])
{
	static const uint8_t offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	unsigned ord = ent[0] & 0x1fu;
	if ((ent[0] & 0x40u) != 0) {
		memset(lfn, 0, 256);
	}
	if (ord == 0 || ord > 19) return;
	unsigned base = (ord - 1u) * 13u;
	for (unsigned k = 0; k < 13u && base + k < 255u; k++) {
		uint16_t ch = (uint16_t)(ent[offs[k]] | (ent[offs[k] + 1] << 8));
		if (ch == 0x0000u || ch == 0xffffu) {
			lfn[base + k] = '\0';
			break;
		}
		lfn[base + k] = (ch < 128u) ? (char)ch : '?';
	}
}

bool
redwood_ufs_fat16_fw_read(const char *name, uint64_t *size_out, volatile void *dst_buf, uint64_t dst_cap)
{
	static const char *const parts[2] = { "modem_a", "modem_b" };
	char target11[11], image11[11] = { 'I','M','A','G','E',' ',' ',' ',' ',' ',' ' };
	bool has_83 = ufs_fat83_name(name, target11);

	uint8_t *sec = kalloc_data(4096, Z_WAITOK);
	if (sec == NULL) return false;

	for (unsigned p = 0; p < 2; p++) {
		if (redwood_ufs_part_read(parts[p], 0, sec, 512) != 0) continue;
		uint16_t bps = (uint16_t)(sec[11] | (sec[12] << 8));
		uint8_t spc = sec[13];
		uint16_t rsvd = (uint16_t)(sec[14] | (sec[15] << 8));
		uint8_t nfats = sec[16];
		uint16_t root_ents = (uint16_t)(sec[17] | (sec[18] << 8));
		uint16_t spf = (uint16_t)(sec[22] | (sec[23] << 8));
		if ((bps != 512 && bps != 4096) || spc == 0 || nfats == 0 || root_ents == 0 || spf == 0) continue;

		uint32_t fat_off = (uint32_t)rsvd * bps;
		uint32_t root_off = fat_off + (uint32_t)nfats * spf * bps;
		uint32_t root_sz = (uint32_t)root_ents * 32u;
		uint32_t data_off = root_off + ((root_sz + bps - 1u) & ~(bps - 1u));
		uint32_t csize = (uint32_t)spc * bps;

		uint16_t img_clus = 0;
		char lfn[256] = { 0 };
		for (uint32_t off = 0; off < root_sz && img_clus == 0; off += 4096) {
			uint32_t ask = root_sz - off > 4096 ? 4096 : root_sz - off;
			if (redwood_ufs_part_read(parts[p], root_off + off, sec, ask) != 0) break;
			for (uint32_t e = 0; e < ask; e += 32) {
				if (sec[e] == 0x00) break;
				if (sec[e] == 0xe5) { lfn[0] = '\0'; continue; }
				if (sec[e + 11] == 0x0f) { ufs_lfn_feed(sec + e, lfn); continue; }
				if ((sec[e + 11] & 0x10) &&
				    (memcmp(sec + e, image11, 11) == 0 || (lfn[0] != '\0' && ufs_lfn_eq(lfn, "image")))) {
					img_clus = (uint16_t)(sec[e + 26] | (sec[e + 27] << 8));
					break;
				}
				lfn[0] = '\0';
			}
		}
		if (img_clus < 2) continue;

		uint16_t file_clus = 0;
		uint32_t file_sz = 0;
		uint16_t cur = img_clus;
		memset(lfn, 0, sizeof(lfn));
		for (unsigned hop = 0; hop < 256 && cur >= 2 && cur < 0xfff8 && file_clus == 0; hop++) {
			uint64_t clus_off = data_off + (uint64_t)(cur - 2u) * csize;
			for (uint32_t off = 0; off < csize && file_clus == 0; off += 4096) {
				uint32_t ask = csize - off > 4096 ? 4096 : csize - off;
				if (redwood_ufs_part_read(parts[p], clus_off + off, sec, ask) != 0) break;
				for (uint32_t e = 0; e < ask; e += 32) {
					if (sec[e] == 0x00) { cur = 0xffff; break; }
					if (sec[e] == 0xe5) { lfn[0] = '\0'; continue; }
					if (sec[e + 11] == 0x0f) { ufs_lfn_feed(sec + e, lfn); continue; }
					if ((sec[e + 11] & 0x18) != 0) { lfn[0] = '\0'; continue; }
					if ((has_83 && memcmp(sec + e, target11, 11) == 0) ||
					    (lfn[0] != '\0' && ufs_lfn_eq(lfn, name))) {
						file_clus = (uint16_t)(sec[e + 26] | (sec[e + 27] << 8));
						memcpy(&file_sz, sec + e + 28, 4);
						break;
					}
					lfn[0] = '\0';
				}
			}
			if (file_clus != 0 || cur >= 0xfff8) break;
			uint8_t fe[2];
			if (redwood_ufs_part_read(parts[p], fat_off + (uint32_t)cur * 2u, fe, 2) != 0) break;
			cur = (uint16_t)(fe[0] | (fe[1] << 8));
		}
		if (file_clus < 2 || file_sz == 0) continue;
		if (size_out != NULL) *size_out = file_sz;
		if (dst_buf == NULL) {
			kfree_data(sec, 4096);
			return true;
		}
		if (file_sz > dst_cap) break;

		uint32_t rem = file_sz, written = 0;
		cur = file_clus;
		volatile uint8_t *out = (volatile uint8_t *)dst_buf;
		for (unsigned hop = 0; hop < 8192 && rem > 0 && cur >= 2 && cur < 0xfff8; hop++) {
			uint32_t run = 1;
			uint16_t nxt = cur;
			while (rem > run * csize && run < 64) {
				uint8_t fe[2];
				if (redwood_ufs_part_read(parts[p], fat_off + (uint32_t)nxt * 2u, fe, 2) != 0) break;
				uint16_t c2 = (uint16_t)(fe[0] | (fe[1] << 8));
				if (c2 != nxt + 1) { nxt = c2; break; }
				nxt = c2;
				run++;
			}
			if (run == 1 || nxt == cur + run - 1) {
				uint8_t fe[2];
				if (redwood_ufs_part_read(parts[p], fat_off + (uint32_t)nxt * 2u, fe, 2) == 0) {
					nxt = (uint16_t)(fe[0] | (fe[1] << 8));
				} else {
					nxt = 0xffff;
				}
			}
			uint32_t chunk = run * csize;
			if (chunk > rem) chunk = rem;
			uint64_t disk_off = data_off + (uint64_t)(cur - 2u) * csize;
			for (uint32_t off = 0; off < chunk; off += 4096) {
				uint32_t ask = chunk - off > 4096 ? 4096 : chunk - off;
				if (redwood_ufs_part_read(parts[p], disk_off + off, sec, ask) != 0) { rem = 1; cur = 0xffff; break; }
				for (uint32_t b = 0; b < ask; b++) out[written + off + b] = sec[b];
			}
			if (cur == 0xffff) break;
			written += chunk;
			rem -= chunk;
			cur = nxt;
		}
		if (rem == 0) {
			kfree_data(sec, 4096);
			return true;
		}
	}
	kfree_data(sec, 4096);
	return false;
}

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void annibale_spmi_lock(void);
extern void annibale_spmi_unlock(void);
extern void PESetUTCTimeOfDay(clock_sec_t secs, clock_usec_t usecs);
extern void clock_initialize_calendar(void);

static uint32_t
ufs_spmi_rtc_seconds(void)
{
	if (!redwood_platform_is_annibale()) return 0;
	vm_offset_t core = ml_io_map(0x0c400000UL, 0x3000UL);
	vm_offset_t obsrv = ml_io_map(0x0c440000UL, 0x80000UL);
	if (core == 0 || obsrv == 0) return 0;
	if ((*(volatile uint32_t *)core & 0xf0000000u) < 0x70000000u) return 0;
	uint32_t max_ch = *(volatile uint32_t *)(core + 4) & 0x7ffu;
	if (max_ch == 0 || max_ch > 1024u) max_ch = 1024u;
	uint32_t rtc_ch = max_ch;
	for (uint32_t n = 0; n < max_ch; n++) {
		uint32_t map = *(volatile uint32_t *)(core + 0x2000UL + 4UL * n);
		uint32_t ppid = (map >> 8) & 0xfffu;
		if (ppid == 0x061u || ppid == 0x060u) {
			rtc_ch = n;
			if (ppid == 0x061u) break;
		}
	}
	if (rtc_ch >= max_ch) return 0;
	vm_offset_t ch = obsrv + 0x20UL * rtc_ch;
	uint32_t rdata = 0, st = 0;
	annibale_spmi_lock();
	*(volatile uint32_t *)(ch + 0x00) = (13u << 27) | (0x48u << 4) | 3u;
	for (unsigned i = 0; i < 4000; i++) {
		st = *(volatile uint32_t *)(ch + 0x08);
		if (st & 1u) break;
	}
	if ((st & 0xfu) == 1u) {
		rdata = *(volatile uint32_t *)(ch + 0x18);
	}
	annibale_spmi_unlock();
	return rdata;
}

static uint32_t
ufs_ext4_block0(uint32_t ino, uint32_t inodes_per_grp, uint16_t inode_sz, uint16_t desc_sz)
{
	uint8_t gd[32], ibuf[256];
	uint32_t grp = (ino - 1u) / inodes_per_grp;
	uint32_t idx = (ino - 1u) % inodes_per_grp;
	if (redwood_ufs_part_read("persist", 4096ULL + (uint64_t)grp * desc_sz, gd, sizeof(gd)) != 0) return 0;
	uint32_t itable = 0;
	memcpy(&itable, gd + 8, 4);
	if (itable == 0) return 0;
	if (redwood_ufs_part_read("persist", (uint64_t)itable * 4096ULL + (uint64_t)idx * inode_sz, ibuf, sizeof(ibuf)) != 0) return 0;
	uint32_t flags = 0, blk = 0;
	memcpy(&flags, ibuf + 0x20, 4);
	if ((flags & 0x80000u) != 0) {
		uint16_t magic = 0, entries = 0, depth = 0;
		memcpy(&magic, ibuf + 0x28, 2);
		memcpy(&entries, ibuf + 0x28 + 2, 2);
		memcpy(&depth, ibuf + 0x28 + 6, 2);
		if (magic != 0xf30au || entries == 0 || depth != 0) return 0;
		memcpy(&blk, ibuf + 0x28 + 12 + 8, 4);
	} else {
		memcpy(&blk, ibuf + 0x28, 4);
	}
	return blk;
}

static uint32_t
ufs_ext4_lookup(uint8_t *sec, uint32_t dir_ino, const char *name,
    uint32_t inodes_per_grp, uint16_t inode_sz, uint16_t desc_sz)
{
	uint32_t blk = ufs_ext4_block0(dir_ino, inodes_per_grp, inode_sz, desc_sz);
	if (blk == 0 || redwood_ufs_part_read("persist", (uint64_t)blk * 4096ULL, sec, 4096) != 0) return 0;
	size_t nlen = strlen(name);
	for (uint32_t off = 0; off + 8u <= 4096u; ) {
		uint32_t entry_ino = 0;
		uint16_t rec_len = 0;
		memcpy(&entry_ino, sec + off, 4);
		memcpy(&rec_len, sec + off + 4, 2);
		uint8_t elen = sec[off + 6];
		if (rec_len < 8u || off + rec_len > 4096u) break;
		if (entry_ino != 0 && elen == nlen && memcmp(sec + off + 8, name, nlen) == 0) {
			return entry_ino;
		}
		off += rec_len;
	}
	return 0;
}

static void
redwood_ufs_sync_rtc(void)
{
	uint8_t *sec = kalloc_data(4096, Z_WAITOK);
	if (sec == NULL) return;
	if (redwood_ufs_part_read("persist", 0, sec, 4096) == 0) {
		uint16_t magic = 0, inode_sz = 0, desc_sz = 32;
		uint32_t log_bs = 0, inodes_per_grp = 0, incompat = 0;
		memcpy(&log_bs, sec + 1024 + 0x18, 4);
		memcpy(&inodes_per_grp, sec + 1024 + 0x28, 4);
		memcpy(&magic, sec + 1024 + 0x38, 2);
		memcpy(&inode_sz, sec + 1024 + 0x58, 2);
		memcpy(&incompat, sec + 1024 + 0x60, 4);
		if ((incompat & 0x80u) != 0) {
			memcpy(&desc_sz, sec + 1024 + 0xfe, 2);
		}
		if (magic == 0xef53u && log_bs == 2u && inodes_per_grp != 0 &&
		    inode_sz >= 128u && inode_sz <= 1024u && desc_sz >= 32u && desc_sz <= 1024u) {
			uint32_t time_ino = ufs_ext4_lookup(sec, 2u, "time", inodes_per_grp, inode_sz, desc_sz);
			uint32_t ats_ino = time_ino ? ufs_ext4_lookup(sec, time_ino, "ats_2", inodes_per_grp, inode_sz, desc_sz) : 0;
			if (ats_ino == 0 && time_ino != 0) {
				ats_ino = ufs_ext4_lookup(sec, time_ino, "ats_1", inodes_per_grp, inode_sz, desc_sz);
			}
			uint32_t ats_blk = ats_ino ? ufs_ext4_block0(ats_ino, inodes_per_grp, inode_sz, desc_sz) : 0;
			uint64_t ats_ms = 0;
			if (ats_blk != 0 && redwood_ufs_part_read("persist", (uint64_t)ats_blk * 4096ULL, &ats_ms, 8) == 0 &&
			    ats_ms > 1700000000000ULL) {
				uint32_t rtc_secs = ufs_spmi_rtc_seconds();
				clock_sec_t utc_sec = (clock_sec_t)(ats_ms / 1000ULL + (uint64_t)rtc_secs);
				clock_usec_t utc_usec = (clock_usec_t)((ats_ms % 1000ULL) * 1000ULL);
				PESetUTCTimeOfDay(utc_sec, utc_usec);
				clock_initialize_calendar();
				printf("ufs: RTC synced: PMK8550=%u s, persist /time/ats=%llu ms -> UTC %lu.%06u\n",
				    rtc_secs, ats_ms, (unsigned long)utc_sec, (unsigned)utc_usec);
			}
		}
	}
	kfree_data(sec, 4096);
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

/* ------------------------------------------------------------------------ */
/* The iOS system image as an Android DSU image ("oem dsu")                  */

/*
 * Android's Dynamic System Updates keep an installed image in userdata as a pinned file, written
 * beneath the metadata encryption (libfiemap maps it with dm-linear onto the raw partition), so on
 * the storage it is the image itself. Where it lies, libfiemap writes down in
 * /metadata/gsi/dsu/<slot>/lp_metadata: a liblp geometry block (4 KB), then a metadata header and
 * tables whose partition "system_gsi" lists the extents in 512-byte sectors of userdata. /metadata
 * is not encrypted, and the file is found by its magic in the raw partition, without reading ext4.
 * tools/k90auto/dsu.py installs the image from Android; the DSU is never enabled.
 *
 * "oem dsu"                    what was found, into the log (read only)
 * "oem dsu load TOTAL WINDOW"  read the first TOTAL bytes (hex) of system_gsi into the resident area
 *                              of a WINDOW-byte memory window, as "oem ramdisk begin TOTAL WINDOW"
 *                              and its blocks would have left it; getvar ramdisk-state then says ready
 * "oem dsu crc OFFSET LENGTH"  CRC-32 (zlib's) of that part of the resident image, for the host to
 *                              compare with its own copy before it boots a kernel on it; the host
 *                              reads it with "getvar dsu-crc" (fastboot shows no text of an oem OKAY)
 * The commands write nothing to the storage. The one write this port makes there goes to the last
 * part of the same slot, through the kernel's block service (dsu_store, at the end of this file).
 * Ported from hakios-arm64e-dada's loader (APSL 2.0).
 */
#define LP_GEOMETRY_MAGIC  0x616c4467u
#define LP_HEADER_MAGIC    0x414c5030u
/* a fresh 16 GiB DSU file on a used userdata had 166 extents (2026-11-05); 64 refused it */
#define DSU_MAX_EXTENTS    1024
#define DSU_READ_BYTES     (8u << 20)

static struct {
	uint64_t sector, count;
} dsu_extent[DSU_MAX_EXTENTS];
static unsigned dsu_extents;
static uint64_t dsu_bytes;

/* system_gsi's extents from one lp_metadata copy at p (geometry first); 0 when it is not one */
static int
dsu_parse(const uint8_t *p, uint64_t room, uint64_t userdata_sectors)
{
	const uint8_t *h = p + 4096, *t;
	uint32_t header_size, tables_size, po, pn, ps, eo, en, es;
	static const char want[] = "system_gsi";

	if (room < 4096 + 256 || *(const uint32_t *)h != LP_HEADER_MAGIC) {
		return 0;
	}
	header_size = *(const uint32_t *)(h + 8);
	tables_size = *(const uint32_t *)(h + 44);
	if (header_size < 128 || header_size > 256 || tables_size > room - 4096 - header_size) {
		return 0;
	}
	t = h + header_size;
	po = *(const uint32_t *)(h + 80), pn = *(const uint32_t *)(h + 84), ps = *(const uint32_t *)(h + 88);
	eo = *(const uint32_t *)(h + 92), en = *(const uint32_t *)(h + 96), es = *(const uint32_t *)(h + 100);
	if (ps < 52 || es < 24 || (uint64_t)po + (uint64_t)pn * ps > tables_size ||
	    (uint64_t)eo + (uint64_t)en * es > tables_size) {
		return 0;
	}
	for (uint32_t i = 0; i < pn; i++) {
		const uint8_t *e = t + po + (uint64_t)i * ps;
		uint32_t first, n;
		uint64_t bytes = 0;
		unsigned k = 0;

		while (k < sizeof(want) && e[k] == (uint8_t)want[k]) {
			k++;
		}
		if (k != sizeof(want)) {
			continue;
		}
		first = *(const uint32_t *)(e + 40);
		n = *(const uint32_t *)(e + 44);
		if (n > DSU_MAX_EXTENTS) {
			print("dsu: system_gsi has %d extents, more than the %d this loader keeps\n", (uint64_t)n,
			    (uint64_t)DSU_MAX_EXTENTS);
			return 0;
		}
		if (n == 0 || (uint64_t)first + n > en) {
			return 0;
		}
		for (uint32_t x = 0; x < n; x++) {
			const uint8_t *ex = t + eo + (uint64_t)(first + x) * es;
			uint64_t count = *(const uint64_t *)ex, sector = *(const uint64_t *)(ex + 12);

			if (*(const uint32_t *)(ex + 8) != 0 /* linear */ || sector + count > userdata_sectors) {
				return 0;
			}
			dsu_extent[x].sector = sector;
			dsu_extent[x].count = count;
			bytes += count * 512;
		}
		dsu_extents = n;
		dsu_bytes = bytes;
		return 1;
	}
	return 0;
}

static int
dsu_find_gsi(efi_block_io **userdata)
{
	static const char16 meta_name[] = u"metadata", data_name[] = u"userdata";
	efi_block_io *meta = find_partition_named(meta_name, sizeof(meta_name) / sizeof(meta_name[0]));
	efi_block_io *data = find_partition_named(data_name, sizeof(data_name) / sizeof(data_name[0]));
	uint64_t bs, meta_bytes, chunk = sizeof(ufs_buffer), found = 0, first_bytes = 0, first_sector = 0;
	unsigned first_extents = 0;

	if (meta == NULL || data == NULL) {
		print("dsu: no %s partition\n", meta == NULL ? "metadata" : "userdata");
		return 0;
	}
	bs = meta->media->block_size;
	meta_bytes = (meta->media->last_block + 1) * bs;
	/* windows of the whole buffer that overlap by 64 KB, so that a copy across a boundary is read whole */
	for (uint64_t at = 0; at < meta_bytes; at += chunk - 64 * 1024) {
		uint64_t n = meta_bytes - at < chunk ? meta_bytes - at : chunk;

		n -= n % bs;
		if (EFI_ERROR(meta->read_blocks(meta, meta->media->media_id, at / bs, n, ufs_buffer))) {
			print("dsu: metadata read failed at %x\n", at);
			return 0;
		}
		for (uint64_t b = 0; b + 4096 <= n && (b < chunk - 64 * 1024 || at + n == meta_bytes); b += 4096) {
			if (*(const uint32_t *)(ufs_buffer + b) != LP_GEOMETRY_MAGIC ||
			    !dsu_parse(ufs_buffer + b, n - b, (data->media->last_block + 1) * data->media->block_size / 512)) {
				continue;
			}
			print("dsu: lp_metadata at %x of metadata: system_gsi %d bytes in %d extents, the first at sector %x\n",
			    at + b, dsu_bytes, (uint64_t)dsu_extents, dsu_extent[0].sector);
			if (found++ == 0) {
				first_bytes = dsu_bytes, first_extents = dsu_extents, first_sector = dsu_extent[0].sector;
			} else if (dsu_bytes != first_bytes || dsu_extents != first_extents || dsu_extent[0].sector != first_sector) {
				print("dsu: two different copies of the metadata: not choosing\n");
				return 0;
			}
		}
		wdt_pet();
	}
	if (found == 0) {
		print("dsu: no lp_metadata with a system_gsi in the metadata partition\n");
		return 0;
	}
	*userdata = data;
	return 1;
}

/* system_gsi's extents; 1 if every copy of the metadata found agrees (a rename can leave an older one) */
static int
dsu_find(efi_block_io **userdata)
{
	static const char16 darwin_name[] = u"darwin";
	efi_block_io *darwin = find_partition_named(darwin_name, sizeof(darwin_name) / sizeof(darwin_name[0]));
	if (darwin != NULL) {
		uint64_t bytes = (darwin->media->last_block + 1) * darwin->media->block_size;
		dsu_extents = 1;
		dsu_extent[0].sector = 0;
		dsu_extent[0].count = bytes / 512;
		dsu_bytes = bytes;
		*userdata = darwin;
		print("dsu: darwin partition: %d bytes in 1 extent\n", dsu_bytes);
		return 1;
	}
	return dsu_find_gsi(userdata);
}

/* zlib's CRC-32 through the ARMv8 CRC32 instructions (the same reflected 0xedb88320 polynomial) */
static uint32_t
dsu_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (; n >= 8; p += 8, n -= 8) {
		__asm__ (".arch_extension crc\n\tcrc32x %w0, %w0, %x1" : "+r" (c) : "r" (*(const uint64_t *)p));
	}
	for (; n > 0; p++, n--) {
		__asm__ (".arch_extension crc\n\tcrc32b %w0, %w0, %w1" : "+r" (c) : "r" ((uint32_t)*p));
	}
	return ~c;
}

static void
dsu_load(efi_block_io *data, const char *arguments)
{
	const char *error = ramdisk_begin(arguments);
	uint64_t want = ramdisk_transfer.total, got = 0, bs = data->media->block_size, t0;
	uint8_t *dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);

	if (error != NULL || want > dsu_bytes) {
		ramdisk_fail();
		fb_reply("FAIL", error != NULL ? error : "the DSU image is smaller than that");
		return;
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	ui_stage("READING SYSTEM FROM STORAGE");
	for (unsigned x = 0; x < dsu_extents && got < want; x++) {
		uint64_t start = dsu_extent[x].sector * 512, left = dsu_extent[x].count * 512;

		if (bs == 0 || start % bs) {
			ramdisk_fail();
			fb_reply("FAIL", "an extent is not block aligned");
			return;
		}
		while (left > 0 && got < want) {
			uint64_t n = left < DSU_READ_BYTES ? left : DSU_READ_BYTES;

			if (want - got < n) {
				n = (want - got + bs - 1) / bs * bs;
			}
			if (EFI_ERROR(data->read_blocks(data, data->media->media_id, start / bs, n, dest + got))) {
				print("dsu: read failed at sector %x\n", start / 512);
				ramdisk_fail();
				fb_reply("FAIL", "read failed");
				return;
			}
			start += n;
			left -= n;
			got += n;
			ui_progress(got < want ? got : want, want);
			wdt_pet();
		}
	}
	ramdisk_transfer.received = want;
	resident_rd_size = want;
	print("dsu: %d bytes read into the resident area in %d ms\n", want,
	    (READ_SYSREG(CNTVCT_EL0) - t0) / (READ_SYSREG(CNTFRQ_EL0) / 1000));
	ui_stage("SYSTEM READY");
	fb_reply("OKAY", "");
}

/* "oem dsu crc": the CRC as eight hex digits, kept for "getvar dsu-crc" ("none" until there is one) */
static char dsu_crc_text[16] = "none";

static void
dsu_crc_reply(const char *arguments)
{
	uint64_t offset, length;
	char *p = dsu_crc_text;

	memcpy(dsu_crc_text, "none", 5);
	if (!ramdisk_hex(&arguments, &offset) || *arguments++ != ' ' || !ramdisk_hex(&arguments, &length) ||
	    *arguments || resident_rd_size == 0 || offset > resident_rd_size || length > resident_rd_size - offset) {
		fb_reply("FAIL", "expected OFFSET LENGTH inside the resident image");
		return;
	}
	uint32_t crc = dsu_crc32((const uint8_t *)(win_base + RESIDENT_RD_OFFSET + offset), length);
	for (int shift = 28; shift >= 0; shift -= 4) {
		*p++ = "0123456789abcdef"[(crc >> shift) & 15];
	}
	*p = 0;
	fb_reply("OKAY", dsu_crc_text);
}

/* Called after the shared held/fault/live command check, which refuses all of "oem dsu" while busy. */
static int
dsu_command(const char *cmd)
{
	efi_block_io *data;

	if (starts_with(cmd, "oem dsu crc ")) {
		dsu_crc_reply(cmd + 12);
		return 1;
	}
	if (str_eq(cmd, "getvar:dsu-crc")) {
		fb_reply("OKAY", dsu_crc_text);
		return 1;
	}
	if (!str_eq(cmd, "oem dsu") && !starts_with(cmd, "oem dsu load ")) {
		return 0;
	}
	log_len = 0;
	if (!dsu_find(&data)) {
		fb_reply("FAIL", "no DSU image (see oem log)");
	} else if (starts_with(cmd, "oem dsu load ")) {
		dsu_load(data, cmd + 13);
	} else {
		for (unsigned x = 0; x < dsu_extents; x++) {
			print("dsu: extent %d: sector %x, %d sectors\n", (uint64_t)x, dsu_extent[x].sector, dsu_extent[x].count);
		}
		fb_reply("OKAY", "");
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* dsu_store: where /private/var is kept between boots                      */

/*
 * tools/k90auto/dsu.py pads the system image with zeros to a 16 GiB slot, so the slot's tail is
 * this port's own: the store is its last 4 GiB, two slots of userland/src/var_store.c (2 GiB each,
 * a header at the start of each). The block service lists it as the partition "dsu_store", in
 * blocks of userdata's size, and the kernel sees /dev/rufs_dsu_store (bsd/dev/redwood_ufs.c).
 *
 * A write lands in userdata, where a wrong sector is Android's data and cannot be undone. So the
 * store is listed only when the DSU file found through lp_metadata is the one holding this run's
 * system image -- its first 64 KB are the APFS container this loader is about to hand over -- and
 * it takes writes only if besides each var_store header block holds zeros or a header. A
 * discarded DSU leaves an old lp_metadata in /metadata that dsu_find still finds; its extents are
 * then free space or another file of Android's, which fails one of the two checks.
 */
#define DSU_STORE_AT       (12ULL << 30)               /* past the system image (5.3 GB now) */
#define DSU_STORE_BYTES    (4ULL << 30)
#define DSU_STORE_SLOT     (2ULL << 30)                /* var_store.c SLOT_BYTES */
#define DSU_HEAD_CHECK     65536u

static efi_block_io *dsu_store_bio;
static uint64_t dsu_store_at = DSU_STORE_AT;
static struct {
	uint64_t sector, count;
} dsu_store_extent[DSU_MAX_EXTENTS];
static unsigned dsu_store_extents;

/* bytes at..at + len of the slot (whole blocks), read into or written from buf */
static efi_status
dsu_slot_io(int write, uint64_t at, uint64_t len, uint8_t *buf)
{
	efi_block_io *bio = dsu_store_bio;
	uint64_t bs = bio->media->block_size;

	for (unsigned x = 0; x < dsu_store_extents && len > 0; x++) {
		uint64_t extent = dsu_store_extent[x].count * 512, n, lba;
		efi_status s;

		if (at >= extent) {
			at -= extent;
			continue;
		}
		n = extent - at < len ? extent - at : len;
		lba = (dsu_store_extent[x].sector * 512 + at) / bs;
		s = write ? bio->write_blocks(bio, bio->media->media_id, lba, n, buf) :
		    bio->read_blocks(bio, bio->media->media_id, lba, n, buf);
		if (EFI_ERROR(s)) {
			return s;
		}
		buf += n;
		len -= n;
		at = 0;
	}
	return len == 0 ? 0 : 0x8000000000000002ULL;       /* EFI_INVALID_PARAMETER: past the slot */
}

/* the store's blocks from lba on, for the block service (which has bounded lba and bytes) */
static efi_status
dsu_store_io(int write, uint64_t lba, uint64_t bytes, uint8_t *buf)
{
	efi_status s;

	if (dsu_store_bio == NULL) {
		return 0x8000000000000002ULL;
	}
	s = dsu_slot_io(write, dsu_store_at + lba * dsu_store_bio->media->block_size, bytes, buf);
	if (write && !EFI_ERROR(s)) {
		dsu_store_bio->flush_blocks(dsu_store_bio);
	}
	return s;
}

static int
dsu_block_is_zero_or_head(const uint8_t *p, uint64_t bs)
{
	static const char magic[8] = "RDWVAR\0";
	static const char fs_magic[8] = {'R', 'D', 'W', 'F', 'S', '0', '0', '1'};

	if (memcmp(p, magic, sizeof(magic)) == 0 || memcmp(p, fs_magic, sizeof(fs_magic)) == 0) {
		return 1;
	}
	for (uint64_t i = 0; i < bs; i++) {
		if (p[i] != 0) {
			return 0;
		}
	}
	return 1;
}

/* List the store at bp if the DSU slot checks out; *writable when writes may go there too. */
static int
dsu_store_open(struct loader_block_part *bp, int want_write, int *writable)
{
	static const char name[] = "dsu_store";
	static const char16 darwin_var_name[] = u"darwin_var";
	const uint8_t *image = (const uint8_t *)(win_base + RESIDENT_RD_OFFSET);
	efi_block_io *data = NULL;
	uint64_t bs = 0;
	const char *why = NULL;

	*writable = 0;
	dsu_store_bio = NULL;
	dsu_store_at = DSU_STORE_AT;
	if (dsu_find_gsi(&data)) {
		bs = data->media->block_size;
		dsu_store_bio = data;
		dsu_store_extents = dsu_extents;
		for (unsigned x = 0; x < dsu_extents; x++) {
			dsu_store_extent[x].sector = dsu_extent[x].sector;
			dsu_store_extent[x].count = dsu_extent[x].count;
			if (bs == 0 || (dsu_extent[x].sector * 512) % bs || (dsu_extent[x].count * 512) % bs) {
				why = "an extent is not block aligned";
			}
		}
		if (why == NULL && (bs > DSU_HEAD_CHECK || DSU_STORE_AT % bs || DSU_STORE_SLOT % bs)) {
			why = "userdata's blocks do not divide the store";
		} else if (why == NULL && dsu_bytes < DSU_STORE_AT + DSU_STORE_BYTES) {
			why = "the slot is smaller than 16 GiB";
		} else if (why == NULL && (resident_rd_size < DSU_HEAD_CHECK || resident_rd_size > DSU_STORE_AT)) {
			why = "this run's system image is missing or reaches into the store";
		} else if (why == NULL && EFI_ERROR(dsu_slot_io(0, 0, DSU_HEAD_CHECK, ufs_buffer))) {
			why = "the slot's first blocks do not read";
		} else if (why == NULL && memcmp(ufs_buffer + 32, "NXSB", 4) != 0) {
			why = "the slot does not begin with an APFS container";
		} else if (why == NULL && memcmp(ufs_buffer, image, DSU_HEAD_CHECK) != 0) {
			why = "the slot does not begin with this run's system image";
		}
	} else {
		why = "no DSU slot";
	}
	if (why != NULL) {
		efi_block_io *dvar = find_partition_named(darwin_var_name,
		    sizeof(darwin_var_name) / sizeof(darwin_var_name[0]));
		if (dvar != NULL) {
			uint64_t vbytes = (dvar->media->last_block + 1) * dvar->media->block_size;
			bs = dvar->media->block_size;
			if (bs != 0 && bs <= DSU_HEAD_CHECK && DSU_STORE_SLOT % bs == 0 && vbytes >= DSU_STORE_BYTES) {
				data = dvar;
				dsu_store_bio = dvar;
				dsu_store_at = 0;
				dsu_store_extents = 1;
				dsu_store_extent[0].sector = 0;
				dsu_store_extent[0].count = vbytes / 512;
				why = NULL;
			}
		}
	}
	if (why != NULL) {
		print("dsu_store: not listed: %s\n", why);
		dsu_store_bio = NULL;
		return 0;
	}
	memset(bp, 0, sizeof(*bp));
	memcpy(bp->name, name, sizeof(name));
	bp->block_size = (uint32_t)bs;
	bp->blocks = DSU_STORE_BYTES / bs;
	bp->unit = 0xffff;
	bp->lun = 0xff;
	print("dsu_store: %d MB from %d MB of the DSU slot, %d blocks of %d\n", DSU_STORE_BYTES >> 20,
	    dsu_store_at >> 20, bp->blocks, bs);
	if (!want_write) {
		return 1;
	}
	for (uint64_t slot = 0; slot < DSU_STORE_BYTES / DSU_STORE_SLOT && why == NULL; slot++) {
		if (EFI_ERROR(dsu_slot_io(0, dsu_store_at + slot * DSU_STORE_SLOT, bs, ufs_buffer))) {
			why = "a header block does not read";
		} else if (slot == 0 && memcmp(ufs_buffer, "RDWFS001", 8) == 0) {
			break;
		} else if (!dsu_block_is_zero_or_head(ufs_buffer, bs)) {
			why = "a header block holds something else than zeros or a header";
		}
	}
	if (why == NULL && data->media->read_only) {
		why = "UEFI gives userdata out read only";
	}
	if (why != NULL) {
		print("dsu_store: read only this run: %s\n", why);
		return 1;
	}
	*writable = 1;
	print("dsu_store: takes writes this run\n");
	return 1;
}

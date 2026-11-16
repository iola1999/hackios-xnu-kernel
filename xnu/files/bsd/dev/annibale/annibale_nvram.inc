/*
 * darwin_nvram, the 32 MiB partition after darwin_var (included by annibale_ufs.c). It keeps what
 * an iPhone keeps in its NOR flash beside the file systems:
 *
 *   +0        the NVRAM banks, nvram-bank-count of nvram-bank-size bytes (/chosen); the loader
 *             reads them at boot (xnuloader/loader_04_fault_payload_nvram.inc), AnnibaleNVRAM
 *             (iokit/Kernel/AnnibaleNVRAM.cpp) writes them back through annibale_nvram_io()
 *   +16 MiB   the panic log, at most 16 MiB (struct var_klog_head, then the text; written by
 *             osfmk/arm64/loader_return.c, read back through kern.annibale.panic_log)
 *
 * Without darwin_nvram, NVRAM lasts one run. A darwin_var still in ANNFS001
 * (bsd/miscfs/ramfs/ramfs_disk.inc) then keeps the panic log at 64 MiB and the backlight setting
 * in its superblock, and its mount reads them back; an APFS darwin_var keeps neither. Which file
 * system darwin_var holds, its block 0 says (an APFS container superblock has "NXSB" at byte 32),
 * read once at the end of the driver's start.
 */

#define NVRAM_BANKS_MAX      (16ULL << 20)  /* the banks stay below the panic log */
#define NVRAM_KLOG_AT        (16ULL << 20)
#define NVRAM_KLOG_MAX       (16ULL << 20)
#define VAR_ANNFS_KLOG_AT    (64ULL << 20)
#define VAR_KLOG_MAGIC       0x474f4c4bU    /* 'KLOG' */
#define VAR_KLOG_ARCHIVED    0x474f4c44U    /* 'DLOG' */
#define VAR_KLOG_VERSION     1U

/* as in loader_return.c and ramfs_disk.inc */
struct var_klog_head {
	uint32_t magic;
	uint32_t version;
	uint64_t payload_bytes;
	uint64_t timestamp;
	uint32_t boot_counter;
	uint32_t payload_crc;
	uint32_t head_crc;
	uint32_t flags;
	uint8_t  reserved[24];
};
_Static_assert(sizeof(struct var_klog_head) == 64, "struct klog_head");

static bool var_probed;             /* block 0 of darwin_var has been read */
static bool var_apfs;               /* and it is an APFS container's */

static void
var_probe(void)
{
	uint8_t *blk;

	if (var_probed || !ufs_ready || ufs_darwin_var_part < 0 || (blk = kalloc_data(4096, Z_WAITOK)) == NULL) {
		return;
	}
	if (ufs_read((uint32_t)ufs_darwin_var_part, 0, blk, 4096) == 0) {
		var_apfs = memcmp(blk + 32, "NXSB", 4) == 0;
		var_probed = true;
		printf("ufs: darwin_var is %s; %s\n", var_apfs ? "an APFS container" : "not APFS (ANNFS001)",
		    ufs_darwin_nvram_part >= 0 ? "darwin_nvram keeps NVRAM and the panic log" :
		    var_apfs ? "no darwin_nvram: NVRAM and panic logs are not kept" :
		    "no darwin_nvram: ANNFS001 keeps the panic log and the backlight setting");
	}
	kfree_data(blk, 4096);
}

/* Where the panic log is: its partition and its offset there; false while that is not known. */
static bool
klog_place(uint32_t *part, uint64_t *at)
{
	if (!ufs_ready) {
		return false;
	}
	if (ufs_darwin_nvram_part >= 0) {
		*part = (uint32_t)ufs_darwin_nvram_part;
		*at = NVRAM_KLOG_AT;
		return true;
	}
	if (var_probed && !var_apfs && ufs_darwin_var_part >= 0) {
		*part = (uint32_t)ufs_darwin_var_part;
		*at = VAR_ANNFS_KLOG_AT;
		return true;
	}
	return false;
}

/* loader_return.c: a panic log (struct var_klog_head and the text, whole blocks); polled with interrupts off. */
int annibale_klog_write(const void *buf, uint64_t len);

int
annibale_klog_write(const void *buf, uint64_t len)
{
	uint32_t part;
	uint64_t at;

	if (!klog_place(&part, &at)) {
		return ENXIO;
	}
	if (len > NVRAM_KLOG_MAX) {
		return EFBIG;
	}
	return ufs_part_io(part, true, at, (void *)(uintptr_t)buf, len);
}

/* ---- the NVRAM banks, for AnnibaleNVRAM ---- */

/* The bytes darwin_nvram has for banks; 0 without the partition. */
uint64_t annibale_nvram_bytes(void);
/* Bytes [offset, offset + len) of the banks; writes in whole blocks. */
int annibale_nvram_io(bool write, uint64_t offset, void *buf, uint64_t len);
/* What was written is on the flash (SYNCHRONIZE CACHE); the loader's writes are already. */
int annibale_nvram_flush(void);

uint64_t
annibale_nvram_bytes(void)
{
	if (!ufs_ready || ufs_darwin_nvram_part < 0) {
		return 0;
	}
	return MIN(ufs_part_bytes((uint32_t)ufs_darwin_nvram_part), NVRAM_BANKS_MAX);
}

int
annibale_nvram_io(bool write, uint64_t offset, void *buf, uint64_t len)
{
	uint64_t room = annibale_nvram_bytes();

	if (room == 0) {
		return ENXIO;
	}
	if (len == 0 || offset > room || len > room - offset) {
		return EINVAL;
	}
	return ufs_part_io((uint32_t)ufs_darwin_nvram_part, write, offset, buf, len);
}

int
annibale_nvram_flush(void)
{
	uint8_t lun;

	if (annibale_nvram_bytes() == 0) {
		return ENXIO;
	}
	lun = ufs_lun[ufs_darwin_nvram_part];
	return ufs_native_on && lun != 0xff ? ufshc_sync_cache(lun) : 0;
}

/* ---- the panic log of the run before, for init to keep in /private/var/db/panic.log ---- */

/*
 * kern.annibale.panic_log: reading gives the text of a panic log not yet kept (nothing if there
 * is none, or it is ANNFS001's at 64 MiB, which its mount keeps itself); writing anything marks
 * it kept.
 */
static int
sysctl_var_panic_log SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	struct var_klog_head kh;
	uint32_t part;
	uint64_t at, span;
	uint8_t *buf;
	int error;

	if (!klog_place(&part, &at) || (int)part == ufs_darwin_var_part) {
		return SYSCTL_OUT(req, NULL, 0);
	}
	buf = kalloc_data(4096, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	error = ufs_read(part, at, buf, 4096);
	memcpy(&kh, buf, sizeof(kh));
	if (error != 0 || kh.magic != VAR_KLOG_MAGIC || kh.version != VAR_KLOG_VERSION ||
	    kh.head_crc != crc32(0, &kh, offsetof(struct var_klog_head, head_crc)) ||
	    kh.payload_bytes == 0 || kh.payload_bytes > NVRAM_KLOG_MAX - sizeof(kh)) {
		kfree_data(buf, 4096);
		return error != 0 ? error : SYSCTL_OUT(req, NULL, 0);
	}
	if (req->newptr != USER_ADDR_NULL) {
		kh.magic = VAR_KLOG_ARCHIVED;
		kh.head_crc = crc32(0, &kh, offsetof(struct var_klog_head, head_crc));
		memcpy(buf, &kh, sizeof(kh));
		error = ufs_write(part, at, buf, 4096);
		kfree_data(buf, 4096);
		return error;
	}
	kfree_data(buf, 4096);
	if (req->oldptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, NULL, (size_t)kh.payload_bytes);
	}
	span = (sizeof(kh) + kh.payload_bytes + 4095) & ~4095ULL;
	buf = kalloc_data((vm_size_t)span, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	error = ufs_read(part, at, buf, span);
	if (error == 0 && crc32(0, buf + sizeof(kh), (size_t)kh.payload_bytes) != kh.payload_crc) {
		error = EIO;
	}
	if (error == 0) {
		error = SYSCTL_OUT(req, buf + sizeof(kh), (size_t)kh.payload_bytes);
	}
	kfree_data(buf, (vm_size_t)span);
	return error;
}

SYSCTL_DECL(_kern_annibale);
SYSCTL_PROC(_kern_annibale, OID_AUTO, panic_log, CTLTYPE_OPAQUE | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_var_panic_log, "", "the panic log of the run before (darwin_nvram); write: kept");

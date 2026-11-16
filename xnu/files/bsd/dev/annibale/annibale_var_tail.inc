/*
 * The last 32 MiB of darwin_var once /private/var is APFS (included by annibale_ufs.c).  The
 * container ends there (tools/apfs/annfs-to-apfs.py makes it that much smaller than the
 * partition), so what is kept here is written without the file system:
 *
 *   +0        reserved for NVRAM
 *   +1 MiB    the backlight setting, one 4 KiB block (struct var_backlight)
 *   +16 MiB   the panic log, at most 16 MiB (struct klog_head, then the text; written by
 *             osfmk/arm64/loader_return.c, read back through kern.annibale.panic_log)
 *
 * A partition still in ANNFS001 (bsd/miscfs/ramfs/ramfs_disk.inc) keeps the backlight setting
 * in its superblock and the panic log at 64 MiB, and none of this is used.  Which one the
 * partition is, block 0 says (an APFS container superblock has "NXSB" at byte 32); it is read
 * once: at the end of the driver's start, or before that by the backlight driver if it starts
 * first.  Until then no panic log is written.
 */

#define VAR_TAIL_BYTES       (32ULL << 20)
#define VAR_BACKLIGHT_AT     (1ULL << 20)
#define VAR_KLOG_AT          (16ULL << 20)
#define VAR_KLOG_MAX         (16ULL << 20)
#define VAR_ANNFS_KLOG_AT    (64ULL << 20)
#define VAR_BACKLIGHT_MAGIC  0x424c5333U    /* 'BLS3' */
#define VAR_BACKLIGHT_MS     1000           /* a slider's run of changes is written once */
#define VAR_KLOG_MAGIC       0x474f4c4bU    /* 'KLOG' */
#define VAR_KLOG_ARCHIVED    0x474f4c44U    /* 'DLOG' */
#define VAR_KLOG_VERSION     1U

struct var_backlight {
	uint32_t magic;
	uint32_t panel_v;
	uint32_t millinits;
	uint32_t crc;                   /* crc32 of the three before it */
};

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

static bool var_probed;             /* block 0 has been read */
static bool var_apfs;               /* and it is an APFS container's */
static uint64_t var_tail;           /* where the last 32 MiB start; 0 when var_apfs is not set */
static LCK_MTX_DECLARE(var_probe_lock, &ufs_lck_grp);

static LCK_MTX_DECLARE(var_backlight_lock, &ufs_lck_grp);
static struct var_backlight var_backlight_saved;    /* on the partition, or about to be */
static bool var_backlight_read, var_backlight_pending;
static thread_call_t var_backlight_call;

bool ramfs_backlight_load(uint32_t *panel_v_out, uint32_t *millinits_out);
void ramfs_backlight_save(uint32_t panel_v, uint32_t millinits);

static void
var_tail_probe(void)
{
	uint8_t *blk = NULL;

	lck_mtx_lock(&var_probe_lock);
	if (var_probed || !ufs_ready || ufs_darwin_var_part < 0 || ufs_darwin_var_bytes < 2 * VAR_TAIL_BYTES ||
	    (blk = kalloc_data(4096, Z_WAITOK)) == NULL) {
		lck_mtx_unlock(&var_probe_lock);
		return;
	}
	if (ufs_read((uint32_t)ufs_darwin_var_part, 0, blk, 4096) == 0) {
		var_apfs = memcmp(blk + 32, "NXSB", 4) == 0;
		var_tail = var_apfs ? ufs_darwin_var_bytes - VAR_TAIL_BYTES : 0;
		var_probed = true;
		printf("ufs: darwin_var is %s\n", var_apfs ? "an APFS container; its last 32 MiB keep the panic log "
		    "and the backlight setting" : "not APFS (ANNFS001 keeps the panic log and the backlight setting)");
	}
	lck_mtx_unlock(&var_probe_lock);
	if (blk != NULL) {
		kfree_data(blk, 4096);
	}
}

/* Where loader_return.c writes the panic log: 0 while it is not known (nothing is written). */
uint64_t annibale_var_klog_offset(void);

uint64_t
annibale_var_klog_offset(void)
{
	if (!var_probed) {
		return 0;
	}
	return var_apfs ? var_tail + VAR_KLOG_AT : VAR_ANNFS_KLOG_AT;
}

/* ---- the backlight setting: MiniAppleARMBacklight.cpp, MiniFramebufferDCS.inc ---- */

static int
var_backlight_write(const struct var_backlight *rec)
{
	uint8_t *blk = kalloc_data(4096, Z_WAITOK | Z_ZERO);
	int error = blk == NULL ? ENOMEM : 0;

	if (error == 0) {
		memcpy(blk, rec, sizeof(*rec));
		error = ufs_write((uint32_t)ufs_darwin_var_part, var_tail + VAR_BACKLIGHT_AT, blk, 4096);
		kfree_data(blk, 4096);
	}
	return error;
}

static void
var_backlight_flush(void)
{
	struct var_backlight rec;
	bool go;

	lck_mtx_lock(&var_backlight_lock);
	go = var_backlight_pending;
	rec = var_backlight_saved;
	var_backlight_pending = false;
	lck_mtx_unlock(&var_backlight_lock);
	if (go && var_backlight_write(&rec) != 0) {
		printf("ufs: the backlight setting could not be written\n");
	}
}

static void
var_backlight_later(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	var_backlight_flush();
}

bool annibale_backlight_load(uint32_t *panel_v_out, uint32_t *millinits_out);
void annibale_backlight_save(uint32_t panel_v, uint32_t millinits);

bool
annibale_backlight_load(uint32_t *panel_v_out, uint32_t *millinits_out)
{
	struct var_backlight rec = { 0 };
	uint8_t *blk;

	var_tail_probe();
	if (!var_apfs) {
		return ramfs_backlight_load(panel_v_out, millinits_out);
	}
	lck_mtx_lock(&var_backlight_lock);
	if (!var_backlight_read && (blk = kalloc_data(4096, Z_WAITOK)) != NULL) {
		if (ufs_read((uint32_t)ufs_darwin_var_part, var_tail + VAR_BACKLIGHT_AT, blk, 4096) == 0) {
			memcpy(&var_backlight_saved, blk, sizeof(var_backlight_saved));
		}
		kfree_data(blk, 4096);
		var_backlight_read = true;
	}
	rec = var_backlight_saved;
	lck_mtx_unlock(&var_backlight_lock);
	if (rec.magic != VAR_BACKLIGHT_MAGIC || rec.crc != crc32(0, &rec, offsetof(struct var_backlight, crc)) ||
	    rec.panel_v == 0 || rec.millinits == 0) {
		return false;
	}
	if (panel_v_out != NULL) {
		*panel_v_out = rec.panel_v;
	}
	if (millinits_out != NULL) {
		*millinits_out = rec.millinits;
	}
	return true;
}

void
annibale_backlight_save(uint32_t panel_v, uint32_t millinits)
{
	struct var_backlight rec = { VAR_BACKLIGHT_MAGIC, panel_v, millinits, 0 };
	bool changed;

	var_tail_probe();
	if (!var_apfs) {
		ramfs_backlight_save(panel_v, millinits);
		return;
	}
	if (panel_v == 0 || millinits == 0) {
		return;
	}
	rec.crc = crc32(0, &rec, offsetof(struct var_backlight, crc));
	lck_mtx_lock(&var_backlight_lock);
	changed = memcmp(&rec, &var_backlight_saved, sizeof(rec)) != 0;
	if (changed) {
		var_backlight_saved = rec;
		var_backlight_pending = true;
		var_backlight_read = true;
		if (var_backlight_call == NULL) {
			var_backlight_call = thread_call_allocate(var_backlight_later, NULL);
		}
	}
	lck_mtx_unlock(&var_backlight_lock);
	if (changed && var_backlight_call != NULL) {
		thread_call_enter_delayed_with_leeway(var_backlight_call, NULL, 0, VAR_BACKLIGHT_MS * NSEC_PER_MSEC,
		    THREAD_CALL_DELAY_LEEWAY | THREAD_CALL_DELAY_USER_NORMAL);
	}
}

/* ---- the panic log of the run before, for init to keep in /private/var/db/panic.log ---- */

/*
 * kern.annibale.panic_log: reading gives the text of a panic log not yet kept (nothing if
 * there is none, or the partition is ANNFS001, whose mount keeps it itself); writing anything
 * marks it kept.
 */
static int
sysctl_var_panic_log SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	struct var_klog_head kh;
	uint64_t at = var_tail + VAR_KLOG_AT, span;
	uint8_t *buf;
	int error;

	if (!var_apfs) {
		return SYSCTL_OUT(req, NULL, 0);
	}
	buf = kalloc_data(4096, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	error = ufs_read((uint32_t)ufs_darwin_var_part, at, buf, 4096);
	memcpy(&kh, buf, sizeof(kh));
	if (error != 0 || kh.magic != VAR_KLOG_MAGIC || kh.version != VAR_KLOG_VERSION ||
	    kh.head_crc != crc32(0, &kh, offsetof(struct var_klog_head, head_crc)) ||
	    kh.payload_bytes == 0 || kh.payload_bytes > VAR_KLOG_MAX - sizeof(kh)) {
		kfree_data(buf, 4096);
		return error != 0 ? error : SYSCTL_OUT(req, NULL, 0);
	}
	if (req->newptr != USER_ADDR_NULL) {
		kh.magic = VAR_KLOG_ARCHIVED;
		kh.head_crc = crc32(0, &kh, offsetof(struct var_klog_head, head_crc));
		memcpy(buf, &kh, sizeof(kh));
		error = ufs_write((uint32_t)ufs_darwin_var_part, at, buf, 4096);
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
	error = ufs_read((uint32_t)ufs_darwin_var_part, at, buf, span);
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
    0, 0, sysctl_var_panic_log, "", "the panic log of the run before (APFS darwin_var); write: kept");

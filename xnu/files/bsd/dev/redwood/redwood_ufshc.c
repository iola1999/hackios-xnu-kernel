/*
 * The phone's UFS host controller, driven from this kernel -- read only, next to UEFI's driver.
 *
 * Reading the storage through the loader (redwood_ufs.c) is one request at a time: Qualcomm's
 * UFSDxe reads synchronously on core 0, and its transfer request list has room for one request
 * only (UTRLBA points at a single descriptor, with UFSDxe's command descriptor right behind it,
 * where slots 8.. would be). The controller takes 32 at once. So for the length of a run this
 * kernel puts a list of its own into the controller: with nothing in flight, the list is stopped
 * (UTRLRSR 0), UTRLBA/UTRLBAU are pointed at the new one, the list is started again; at the end
 * (redwood_ufs_quiesce, from loader_return), what is in flight is waited for, the list stopped,
 * UEFI's address written back, the list started. The list and its command descriptors are in
 * memory the loader owns (it offers it in its page, with UEFI's address); the loader checks the
 * address on every way back from a run and puts UEFI's back itself if the kernel did not
 * (xnuloader/loader.c, hc_take_back), and neither reads nor writes for the kernel or for its log
 * while another list is in. While this kernel's list is in, it asks the loader for nothing.
 *
 * Nothing else of the controller is changed: no reset, no link startup, no UIC command, no
 * interrupt enable, aggregation, clock or power setting; the task management list is not
 * touched; IS and UTRLCNR are only read. UTRLCLR is written only to clear this kernel's own
 * slots when they do not finish at the end.
 *
 * Read only by construction: a command is made in a buffer of its own, and only one that passes
 * ufshc_permitted() is ever written into a slot; what the slot then holds is read back and must
 * be the same and pass again before the doorbell is rung. The whitelist checks three things of
 * every command: the SCSI operation (READ(10)/READ(16), TEST UNIT READY, REQUEST SENSE, INQUIRY,
 * READ CAPACITY(10/16), nothing else -- no query, no task management, no NOP), the data
 * direction in the request descriptor (from the device or none, never to it), and the command
 * UPIU's flags (never W); and the logical unit: never a well-known one (REPORT LUNS, BOOT, RPMB,
 * the device), and only those reads are allowed to go to now (ufshc_set_luns: LUN 0, where the
 * iOS volume is, unless a comparison with the loader's reads asks for more). A read's length in
 * the CDB must be what the UPIU expects. It is tried at the probe on made-up commands that must
 * pass or be refused. This file builds READ(10) and TEST UNIT READY only.
 *
 * Every slot of this list holds a whitelisted command at all times: TEST UNIT READY until it is
 * used for a read. Slot 0 is kept for the proof that the controller does read this list: after
 * every switch, before any read, slot 0's TEST UNIT READY is rung and must come back in this
 * list (its status written here). Were the controller still reading UEFI's list, the doorbell
 * would run UEFI's slot 0 instead -- so first UEFI's slot 0 must hold a read too (it does: the
 * loader's last use of the storage before a run is reading back the log it wrote), and the
 * proof costs nothing either way. Reads go in slots 1..31.
 *
 * The doorbell is rung only with interrupts off, after looking that this kernel's list is the
 * one in the controller and running, that the slot is free, and that the loader is not taking
 * the controller back (it says so in its page, then waits before it does).
 *
 * The list and the command descriptors are mapped here without caching (nothing of them is ever
 * in a cache line of this kernel's to be written back over what the controller writes); the data
 * buffers are this kernel's memory, cleaned and invalidated around every read.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include "redwood_ufshc.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern boolean_t pmap_valid_address(uint64_t addr);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void IOSleep(unsigned int milliseconds);
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);

#define UFSHC_PHYS          0x1d84000ULL        /* ufshc@1d84000, "ufs_mem", in the phone's Android device tree */
#define UFSHC_SIZE          0x3000

/* UFSHCI registers (JESD223). 0x38..0x48, the UIC error codes, clear when read: never read here. */
#define UFSHC_CAP           0x00
#define UFSHC_VER           0x08
#define UFSHC_HCDDID        0x10
#define UFSHC_HCPMID        0x14
#define UFSHC_AHIT          0x18
#define UFSHC_IS            0x20
#define UFSHC_IE            0x24
#define UFSHC_HCS           0x30
#define UFSHC_HCE           0x34
#define UFSHC_UTRIACR       0x4c
#define UFSHC_UTRLBA        0x50
#define UFSHC_UTRLBAU       0x54
#define UFSHC_UTRLDBR       0x58
#define UFSHC_UTRLCLR       0x5c
#define UFSHC_UTRLRSR       0x60
#define UFSHC_UTRLCNR       0x64
#define UFSHC_UTMRLBA       0x70
#define UFSHC_UTMRLBAU      0x74
#define UFSHC_UTMRLDBR      0x78
#define UFSHC_UTMRLRSR      0x80

#define CAP_NUTRS(c)        (((c) & 0x1f) + 1)
#define CAP_64AS            (1u << 24)
#define HCS_DP              (1u << 0)
#define HCS_UTRLRDY         (1u << 1)
#define IS_FATAL            ((1u << 2) | (1u << 11) | (1u << 12) | (1u << 16) | (1u << 17))  /* UE, DFES, UTPES, HCFES, SBFES */

/* the request descriptor's first word: command type, data direction */
#define UTRD_CT_MASK        0xf0000000u
#define UTRD_CT_UFS         (1u << 28)
#define UTRD_DD_MASK        (3u << 25)
#define UTRD_DD_NONE        (0u << 25)
#define UTRD_DD_OUT         (1u << 25)          /* host to device: never */
#define UTRD_DD_IN          (2u << 25)
#define OCS_SUCCESS         0x0
#define OCS_INVALID         0xf

#define UPIU_COMMAND        0x01
#define UPIU_RESPONSE       0x21
#define UPIU_FLAG_READ      0x40
#define UPIU_FLAG_WRITE     0x20
#define LUN_WELL_KNOWN      0x80

#define SLOTS               32                  /* this kernel's list */
#define SLOT_PROOF          0
#define UCD_RESPONSE        512
#define UCD_PRDT            1024
#define PRD_MAX             (256u << 10)        /* the most one table entry describes */
#define SLOT_BUFFER_DEFAULT (256u << 10)
#define BLOCK               4096u               /* the logical block size of every unit here */

#define TIMEOUT_MS          10000

struct hc_cmd {
	uint8_t upiu[32];
	uint32_t dw0;
};

struct hc_slot {
	uint32_t number;                        /* in this kernel's list */
	volatile uint32_t *utrd;                /* its descriptor (the loader's memory, not cached here) */
	volatile uint32_t *ucd;                 /* its command descriptor (likewise) */
	uint64_t ucd_pa;
	uint8_t *data;                          /* this kernel's memory, cached */
	uint64_t data_pa;
};

static vm_offset_t hc;                          /* the registers */
static struct ufshc_loader hc_loader;
static uint32_t hc_list_lo, hc_list_hi;         /* this kernel's list's address */
static volatile uint32_t *hc_uefi_list;         /* UEFI's list, not cached here, only read */
static uint64_t hc_uefi_map_pa, hc_uefi_map_size;
static vm_offset_t hc_uefi_map_va;
static struct hc_slot hc_slots[SLOTS];
static uint32_t hc_nslots;                      /* slots 1..hc_nslots carry reads */
static uint32_t hc_free;                        /* bit i: hc_slots[i] free */
static uint32_t hc_slot_buffer = SLOT_BUFFER_DEFAULT;
static uint32_t hc_lun_mask = 1;                /* the logical units reads may go to: LUN 0 */
static bool hc_usable;
static uint32_t hc_leave;                       /* ufs_native_leave=1 (a test): the list left in at the end, for the loader */
static volatile bool hc_in, hc_closed, hc_dead;
static uint32_t hc_is_in, hc_cnr_in;            /* IS and UTRLCNR when this kernel's list went in */
static uint64_t hc_requests, hc_errors, hc_bytes, hc_wait_us, hc_refused, hc_most_in_flight, hc_proofs;
static uint64_t hc_write_requests, hc_write_errors, hc_write_bytes;
static uint64_t hc_switch_in_count, hc_switch_out_count;
static uint32_t hc_in_flight;
/* where the time goes: doorbell reads while waiting, waits for a free slot, taking hc_lock, how many in flight */
static uint64_t hc_polls, hc_slot_waits, hc_slot_wait_ticks, hc_lock_ticks, hc_flight_hist[6];

static LCK_GRP_DECLARE(hc_lck_grp, "redwood-ufshc");
static LCK_MTX_DECLARE(hc_lock, &hc_lck_grp);

SYSCTL_DECL(_kern_redwood);
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_requests, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_requests,
    "reads this kernel's own UFS driver has had answered");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_errors,
    "reads of this kernel's own UFS driver that failed");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_bytes,
    "bytes this kernel's own UFS driver has read");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_wait_us, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_wait_us,
    "microseconds spent waiting for the controller, summed over the reads");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_refused,
    "commands the whitelist refused (there should never be one)");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_most_in_flight, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_most_in_flight,
    "the most reads this kernel's own UFS driver has had in flight at once");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_writes, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_write_requests,
    "writes this kernel's own UFS driver has had answered");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_write_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_write_errors,
    "writes of this kernel's own UFS driver that failed");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, ufs_native_write_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_write_bytes,
    "bytes this kernel's own UFS driver has written");

/*
 * What this file says goes to the kernel's log, and the last lines are kept: the start of a long
 * run's log does not come back to the loader (the buffer wraps), so they are said again at the
 * end, into the loader's log directly (redwood_ufs_quiesce; printf is not for that moment).
 */
extern void loader_log_putc(char c);
#define KEPT_LINES          64
static char hc_kept[KEPT_LINES][208];
static uint32_t hc_kept_count;

static void
hc_say(const char *format, ...) __printflike(1, 2);
static void
hc_say(const char *format, ...)
{
	char line[208];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("%s", line);
	strlcpy(hc_kept[hc_kept_count++ % KEPT_LINES], line, sizeof(hc_kept[0]));
}

/* the same, at the end: into the loader's log only */
static void
hc_say_late(const char *format, ...) __printflike(1, 2);
static void
hc_say_late(const char *format, ...)
{
	char line[208];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

static inline uint32_t
hc_read(uint32_t reg)
{
	return *(volatile uint32_t *)(hc + reg);
}

static inline void
hc_write(uint32_t reg, uint32_t value)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(hc + reg) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_us_since(uint64_t start)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return ns / 1000;
}

/* Nothing more is sent from here on; whoever waits for a slot is told. */
static void
hc_off(void)
{
	hc_dead = true;
	lck_mtx_lock(&hc_lock);
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
}

#include "redwood_ufshc_rw.inc"

/* ---- the proof, and the switches ---- */

/* UEFI's slot 0 holds a read (what the controller would run were it to take no notice of this list) */
static const char *
hc_uefi_slot0(uint8_t *upiu, uint32_t *dw0)
{
	uint64_t ucd = (uint64_t)hc_uefi_list[5] << 32 | hc_uefi_list[4];
	volatile const uint8_t *u;

	*dw0 = hc_uefi_list[0];
	if (ucd < hc_uefi_map_pa || ucd + 32 > hc_uefi_map_pa + hc_uefi_map_size) {
		return "UEFI's command descriptor is not where it was";
	}
	u = (volatile const uint8_t *)(hc_uefi_map_va + (ucd - hc_uefi_map_pa));
	for (int i = 0; i < 32; i++) {
		upiu[i] = u[i];
	}
	return hc_refusal(upiu, *dw0, 0xff, true) == NULL ? NULL : "UEFI's slot 0 does not hold a read";
}

/* This kernel's slot 0: TEST UNIT READY rung, and its status must come back in this list */
static int
hc_prove(void)
{
	struct hc_slot *s = &hc_slots[SLOT_PROOF];
	struct hc_cmd c;
	uint32_t ocs;
	int e;

	hc_make_tur(&c, (uint8_t)__builtin_ctz(hc_lun_mask), SLOT_PROOF);
	e = hc_place(s, &c, 0);
	if (e == 0) {
		e = hc_ring(s);
	}
	if (e == 0) {
		e = hc_complete(s, "the proof's TEST UNIT READY", true);
	}
	ocs = s->utrd[2] & 0xff;
	hc_proofs++;
	if (e != 0 || ocs != OCS_SUCCESS) {
		hc_say("ufs: native: proof FAILED (%d): this list's slot 0 has OCS %x after its doorbell -- the controller "
		    "did not run it from this list\n", e, ocs);
		return e != 0 ? e : EIO;
	}
	hc_say("ufs: native: proof: TEST UNIT READY in this list's slot 0 came back here (OCS 0, response %02x, status %02x)\n",
	    ((volatile const uint8_t *)s->ucd)[UCD_RESPONSE + 6], ((volatile const uint8_t *)s->ucd)[UCD_RESPONSE + 7]);
	return 0;
}

struct hc_put_back_result {
	uint32_t db_before, db_after_clear, lo, hi, rsr, db, is, cnr, cnr_ours;
	bool cleared, ok;
};

/*
 * UEFI's list back into the controller: what is in flight waited for (1 s), cleared if it does
 * not finish (every slot of this list is this kernel's), the list stopped, UEFI's address
 * written, the list started, read back. Interrupts off, no locks, no printing (the quiesce).
 */
static void
hc_put_back(struct hc_put_back_result *r)
{
	uint64_t t;

	bzero(r, sizeof(*r));
	for (t = mach_absolute_time(); (r->db_before = hc_read(UFSHC_UTRLDBR)) != 0 && hc_us_since(t) < 1000000;) {
		__builtin_arm_yield();
	}
	if (r->db_before != 0 && hc_read(UFSHC_UTRLBA) == hc_list_lo && hc_read(UFSHC_UTRLBAU) == hc_list_hi) {
		r->cleared = true;
		hc_write(UFSHC_UTRLCLR, 0);
		for (t = mach_absolute_time(); (r->db_after_clear = hc_read(UFSHC_UTRLDBR)) != 0 && hc_us_since(t) < 1000000;) {
			__builtin_arm_yield();
		}
	}
	/* the completion notifications this kernel's requests left: exactly those bits, written with ones */
	r->cnr_ours = hc_read(UFSHC_UTRLCNR) & ~hc_cnr_in;
	if (r->cnr_ours != 0) {
		hc_write(UFSHC_UTRLCNR, r->cnr_ours);
	}
	hc_write(UFSHC_UTRLRSR, 0);
	for (t = mach_absolute_time(); (hc_read(UFSHC_UTRLRSR) & 1) && hc_us_since(t) < 5000;) {
		__builtin_arm_yield();
	}
	hc_write(UFSHC_UTRLBA, hc_loader.uefi_lo);
	hc_write(UFSHC_UTRLBAU, hc_loader.uefi_hi);
	hc_write(UFSHC_UTRLRSR, 1);
	r->lo = hc_read(UFSHC_UTRLBA);
	r->hi = hc_read(UFSHC_UTRLBAU);
	r->rsr = hc_read(UFSHC_UTRLRSR);
	r->db = hc_read(UFSHC_UTRLDBR);
	r->is = hc_read(UFSHC_IS);
	r->cnr = hc_read(UFSHC_UTRLCNR);
	r->ok = r->lo == hc_loader.uefi_lo && r->hi == hc_loader.uefi_hi && (r->rsr & 1);
	if (r->ok) {
		*hc_loader.list_in = 0;
		(*hc_loader.switched_out)++;
		hc_switch_out_count++;
	}
}

static void
hc_put_back_print(const char *why, const struct hc_put_back_result *r, bool late)
{
	void (*say)(const char *, ...) = late ? hc_say_late : hc_say;

	if (r->cleared) {
		say("ufs: native: %s: doorbell %08x did not clear in 1 s: this kernel's slots cleared (UTRLCLR), doorbell "
		    "then %08x\n", why, r->db_before, r->db_after_clear);
	}
	say("ufs: native: %s: transfer list %s UEFI's %08x:%08x (run %u, doorbell %08x; IS %08x, at the switch %08x; "
	    "completion %08x after clearing this kernel's %08x, at the switch %08x%s)\n", why, r->ok ? "back to" :
	    "NOT BACK TO", r->hi, r->lo, r->rsr, r->db, r->is, hc_is_in, r->cnr, r->cnr_ours, hc_cnr_in,
	    r->cnr == hc_cnr_in ? ", as it was" : ", NOT AS IT WAS");
}

/*
 * This kernel's list into the controller (the caller holds the loader's mailbox: nothing is asked
 * of UEFI meanwhile). prove: then slot 0's TEST UNIT READY, before anything is read.
 */
int
ufshc_switch_in(bool prove)
{
	uint32_t lo, hi, db, rsr, hcs, uefi_dw0 = 0;
	uint8_t uefi_upiu[32] = { 0 };
	const char *no = NULL;
	struct hc_put_back_result undo;
	boolean_t istate;
	bool undone = false;

	if (!hc_usable || hc_dead || hc_closed) {
		return ENXIO;
	}
	lck_mtx_lock(&hc_lock);
	if (hc_in) {
		lck_mtx_unlock(&hc_lock);
		return 0;
	}
	istate = ml_set_interrupts_enabled(FALSE);
	if (*hc_loader.take_back != 0) {
		no = "the loader has taken the controller back";
	} else {
		no = hc_uefi_slot0(uefi_upiu, &uefi_dw0);
	}
	lo = hc_read(UFSHC_UTRLBA);
	hi = hc_read(UFSHC_UTRLBAU);
	db = hc_read(UFSHC_UTRLDBR);
	rsr = hc_read(UFSHC_UTRLRSR);
	hcs = hc_read(UFSHC_HCS);
	if (no == NULL) {
		if (lo != hc_loader.uefi_lo || hi != hc_loader.uefi_hi) {
			no = "the controller's list is not UEFI's";
		} else if (db != 0) {
			no = "a request in flight";
		} else if ((rsr & 1) == 0 || (hcs & HCS_UTRLRDY) == 0) {
			no = "the list is not running, or not ready";
		}
	}
	if (no == NULL) {
		uint64_t t;

		hc_is_in = hc_read(UFSHC_IS);
		hc_cnr_in = hc_read(UFSHC_UTRLCNR);
		hc_write(UFSHC_UTRLRSR, 0);
		for (t = mach_absolute_time(); (hc_read(UFSHC_UTRLRSR) & 1) && hc_us_since(t) < 5000;) {
			__builtin_arm_yield();
		}
		if (hc_read(UFSHC_UTRLRSR) & 1) {
			no = "the list did not stop";
			hc_write(UFSHC_UTRLRSR, 1);     /* (UEFI's list, running, as it was) */
		} else {
			hc_write(UFSHC_UTRLBA, hc_list_lo);
			hc_write(UFSHC_UTRLBAU, hc_list_hi);
			hc_write(UFSHC_UTRLRSR, 1);
			lo = hc_read(UFSHC_UTRLBA);
			hi = hc_read(UFSHC_UTRLBAU);
			rsr = hc_read(UFSHC_UTRLRSR);
			if (lo != hc_list_lo || hi != hc_list_hi || (rsr & 1) == 0) {
				no = "the controller did not take this list";
				hc_put_back(&undo);
				undone = true;
			} else {
				hc_in = true;
				*hc_loader.cnr_at_switch = hc_cnr_in;
				*hc_loader.list_in = 1;
				(*hc_loader.switched_in)++;
				hc_switch_in_count++;
			}
		}
	}
	ml_set_interrupts_enabled(istate);
	lck_mtx_unlock(&hc_lock);
	if (undone) {
		hc_put_back_print("undoing", &undo, false);
	}
	if (no != NULL) {
		hc_say("ufs: native: this kernel's list not put in: %s (list %08x:%08x, doorbell %08x, run %08x, HCS %08x; UEFI's "
		    "slot 0 %08x, CDB %02x)\n", no, hi, lo, db, rsr, hcs, uefi_dw0, uefi_upiu[16]);
		return EIO;
	}
	hc_say("ufs: native: transfer list switched from UEFI's %08x:%08x to this kernel's %08x:%08x (doorbell 0, run %u; "
	    "IS %08x, completion %08x; UEFI's slot 0 holds a read: %08x, CDB %02x)\n", hc_loader.uefi_hi,
	    hc_loader.uefi_lo, hi, lo, rsr, hc_is_in, hc_cnr_in, uefi_dw0, uefi_upiu[16]);
	if (prove) {
		int e = hc_prove();

		if (e != 0) {
			ufshc_switch_out("after the failed proof");
			hc_off();
			return e;
		}
	}
	return 0;
}

/* UEFI's list back (the caller holds the loader's mailbox) */
int
ufshc_switch_out(const char *why)
{
	struct hc_put_back_result r;
	uint64_t started;
	boolean_t istate;

	lck_mtx_lock(&hc_lock);
	if (!hc_in) {
		lck_mtx_unlock(&hc_lock);
		return 0;
	}
	hc_in = false;                          /* nothing more is rung */
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
	/* what is in flight finishes (whoever waits for it sees it done) */
	for (started = mach_absolute_time(); hc_read(UFSHC_UTRLDBR) != 0 && hc_us_since(started) < 2000000;) {
		IOSleep(1);
	}
	lck_mtx_lock(&hc_lock);
	istate = ml_set_interrupts_enabled(FALSE);
	hc_put_back(&r);
	ml_set_interrupts_enabled(istate);
	lck_mtx_unlock(&hc_lock);
	hc_put_back_print(why, &r, false);
	if (!r.ok) {
		hc_off();
		return EIO;
	}
	return 0;
}

bool
ufshc_list_in(void)
{
	return hc_in;
}

bool
ufshc_available(void)
{
	return hc_usable && !hc_dead && !hc_closed;
}

void
ufshc_set_luns(uint32_t mask)
{
	hc_lun_mask = mask & 0xff;
}

static void
hc_report(const char *when, bool late)
{
	(late ? hc_say_late : hc_say)("ufs: native: %s: %llu reads, %llu errors, %llu bytes, %llu ms waiting (summed over reads), at most %llu "
	    "in flight, %llu refused, list in %llu times and out %llu times, %llu proofs%s\n", when, hc_requests, hc_errors,
	    hc_bytes, hc_wait_us / 1000, hc_most_in_flight, hc_refused, hc_switch_in_count, hc_switch_out_count, hc_proofs,
	    hc_dead ? "; THE PATH IS OFF" : "");
	{
		uint64_t wait_ns, lock_ns;

		absolutetime_to_nanoseconds(hc_slot_wait_ticks, &wait_ns);
		absolutetime_to_nanoseconds(hc_lock_ticks, &lock_ns);
		(late ? hc_say_late : hc_say)("ufs: native: %s: %llu doorbell reads while waiting (%llu a read), %llu waits for a "
		    "free slot (%llu ms), %llu ms taking the slots' lock; reads put in with 1 in flight %llu, 2 %llu, 3-4 %llu, "
		    "5-8 %llu, 9-16 %llu, more %llu\n", when, hc_polls, hc_requests ? hc_polls / hc_requests : 0, hc_slot_waits,
		    wait_ns / 1000000, lock_ns / 1000000, hc_flight_hist[0], hc_flight_hist[1], hc_flight_hist[2],
		    hc_flight_hist[3], hc_flight_hist[4], hc_flight_hist[5]);
		(late ? hc_say_late : hc_say)("ufs: native: %s: waiters spin %u us, then sleep: %llu slept; the poller %llu "
		    "rounds, %llu doorbell reads\n", when, hc_spin_us, hc_sleeps, hc_poller_rounds, hc_poller_polls);
	}
}

void
ufshc_report(const char *when)
{
	if (hc_usable) {
		hc_report(when, false);
	}
}

/* ---- the probe ---- */

/*
 * What the controller looks like as UEFI left it, and whether this kernel may put a list of its
 * own in (only == true: say so and do not use it).
 */
bool
ufshc_probe(const struct ufshc_loader *loader, bool only)
{
	uint32_t cap, ver, hcs, hce, is, ie, iacr, ahit, base, baseu, db, rsr, cnr, tm_base, tm_baseu, tm_db, tm_rsr;
	uint32_t uefi_dw0 = 0, slots = 31;
	uint8_t uefi_upiu[32] = { 0 };
	uint64_t uefi_pa;
	const char *no = NULL;
	vm_offset_t area;

	if (!hc_whitelist_selftest()) {
		return false;
	}
	hc_loader = *loader;
	PE_parse_boot_argn("ufs_native_buffer", &hc_slot_buffer, sizeof(hc_slot_buffer));
	if (hc_slot_buffer < BLOCK || hc_slot_buffer > (4u << 20) || hc_slot_buffer % BLOCK) {
		hc_slot_buffer = SLOT_BUFFER_DEFAULT;
	}
	PE_parse_boot_argn("ufs_native_slots", &slots, sizeof(slots));
	if (slots < 1 || slots > SLOTS - 1) {
		slots = SLOTS - 1;
	}
	PE_parse_boot_argn("ufs_native_leave", &hc_leave, sizeof(hc_leave));
	PE_parse_boot_argn("ufs_native_hang", &hc_hang_after, sizeof(hc_hang_after));
	if (loader->hc_pa != UFSHC_PHYS || loader->area_pa == 0 || loader->slots != SLOTS || (loader->area_pa & 0x3ff) ||
	    loader->ucd_size < UCD_PRDT + 16 * (hc_slot_buffer / PRD_MAX + 1) || (loader->ucd_offset & 0x7f) ||
	    (loader->ucd_size & 0x7f) || loader->ucd_offset < SLOTS * 32 ||
	    loader->area_size < loader->ucd_offset + SLOTS * loader->ucd_size) {
		hc_say("ufs: native: not used: the loader offers no area for a list (or not one as expected: controller %llx, "
		    "area %llx of %u, command descriptors at %u of %u)\n", loader->hc_pa, loader->area_pa, loader->area_size,
		    loader->ucd_offset, loader->ucd_size);
		return false;
	}
	if (pmap_valid_address(loader->area_pa) || pmap_valid_address(loader->area_pa + loader->area_size - 1)) {
		hc_say("ufs: native: not used: the loader's area %llx is memory this kernel manages\n", loader->area_pa);
		return false;
	}
	hc = ml_io_map(UFSHC_PHYS, UFSHC_SIZE);
	cap = hc_read(UFSHC_CAP);
	ver = hc_read(UFSHC_VER);
	hcs = hc_read(UFSHC_HCS);
	hce = hc_read(UFSHC_HCE);
	is = hc_read(UFSHC_IS);
	ie = hc_read(UFSHC_IE);
	iacr = hc_read(UFSHC_UTRIACR);
	ahit = hc_read(UFSHC_AHIT);
	base = hc_read(UFSHC_UTRLBA);
	baseu = hc_read(UFSHC_UTRLBAU);
	db = hc_read(UFSHC_UTRLDBR);
	rsr = hc_read(UFSHC_UTRLRSR);
	cnr = hc_read(UFSHC_UTRLCNR);
	tm_base = hc_read(UFSHC_UTMRLBA);
	tm_baseu = hc_read(UFSHC_UTMRLBAU);
	tm_db = hc_read(UFSHC_UTMRLDBR);
	tm_rsr = hc_read(UFSHC_UTMRLRSR);
	hc_say("ufs: native: controller at %llx: CAP %08x (%u slots, 64-bit %s), VER %08x, HCDDID %08x, HCPMID %08x\n",
	    UFSHC_PHYS, cap, CAP_NUTRS(cap), (cap & CAP_64AS) ? "yes" : "no", ver, hc_read(UFSHC_HCDDID),
	    hc_read(UFSHC_HCPMID));
	hc_say("ufs: native: HCE %08x, HCS %08x, IS %08x, IE %08x, UTRIACR %08x, AHIT %08x\n", hce, hcs, is, ie, iacr, ahit);
	hc_say("ufs: native: transfer list %08x:%08x (the loader's note of UEFI's: %08x:%08x), doorbell %08x, run %08x, "
	    "completion %08x; task management list %08x:%08x, doorbell %08x, run %08x\n", baseu, base, loader->uefi_hi,
	    loader->uefi_lo, db, rsr, cnr, tm_baseu, tm_base, tm_db, tm_rsr);
	uefi_pa = (uint64_t)baseu << 32 | base;

	if ((hce & 1) == 0) {
		no = "the controller is not enabled";
	} else if ((hcs & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY)) {
		no = "no device present or the transfer list not ready";
	} else if ((rsr & 1) == 0) {
		no = "the transfer list is not running";
	} else if (ver < 0x200 || ver >= 0x500) {
		no = "a controller version other than 2.x, 3.x or 4.x";
	} else if (CAP_NUTRS(cap) < SLOTS) {
		no = "fewer than 32 slots";
	} else if ((cap & CAP_64AS) == 0) {
		no = "no 64-bit addressing";
	} else if (is & IS_FATAL) {
		no = "an error status";
	} else if (base != loader->uefi_lo || baseu != loader->uefi_hi) {
		no = "the transfer list is not the one the loader noted as UEFI's";
	} else if (db != 0) {
		no = "a request in flight";
	} else if (pmap_valid_address(uefi_pa)) {
		no = "UEFI's transfer list is in memory this kernel manages";
	}
	if (no == NULL) {
		/* UEFI's list and command descriptor (the page they are in), not cached here, only read */
		hc_uefi_map_pa = uefi_pa & ~(uint64_t)PAGE_MASK;
		hc_uefi_map_size = PAGE_SIZE;
		hc_uefi_map_va = ml_io_map_unmappable((vm_offset_t)hc_uefi_map_pa, PAGE_SIZE, VM_WIMG_WCOMB);
		hc_uefi_list = (volatile uint32_t *)(hc_uefi_map_va + (uefi_pa - hc_uefi_map_pa));
		hc_say("ufs: native: UEFI's slot 0: %08x %08x %08x %08x %08x %08x %08x %08x\n", hc_uefi_list[0], hc_uefi_list[1],
		    hc_uefi_list[2], hc_uefi_list[3], hc_uefi_list[4], hc_uefi_list[5], hc_uefi_list[6], hc_uefi_list[7]);
		no = hc_uefi_slot0(uefi_upiu, &uefi_dw0);
		hc_say("ufs: native: UEFI's last command: UPIU %02x %02x %02x %02x, CDB %02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x\n", uefi_upiu[0], uefi_upiu[1], uefi_upiu[2], uefi_upiu[3], uefi_upiu[16], uefi_upiu[17],
		    uefi_upiu[18], uefi_upiu[19], uefi_upiu[20], uefi_upiu[21], uefi_upiu[22], uefi_upiu[23], uefi_upiu[24],
		    uefi_upiu[25]);
	}
	if (no != NULL) {
		hc_say("ufs: native: not used: %s; the storage is read through the loader\n", no);
		return false;
	}
	if (only) {
		hc_say("ufs: native: as expected; ufs_native=2 asks for no more than this look\n");
		return false;
	}

	/* this kernel's list and command descriptors, in the loader's area; every slot TEST UNIT READY to begin with */
	area = ml_io_map_unmappable((vm_offset_t)(loader->area_pa & ~(uint64_t)PAGE_MASK),
	    (vm_size_t)round_page((loader->area_pa & PAGE_MASK) + loader->area_size), VM_WIMG_WCOMB) +
	    (vm_offset_t)(loader->area_pa & PAGE_MASK);
	hc_list_lo = (uint32_t)loader->area_pa;
	hc_list_hi = (uint32_t)(loader->area_pa >> 32);
	for (uint32_t i = 0; i < SLOTS; i++) {
		struct hc_slot *s = &hc_slots[i];
		struct hc_cmd c;

		s->number = i;
		s->utrd = (volatile uint32_t *)(area + 32 * i);
		s->ucd = (volatile uint32_t *)(area + loader->ucd_offset + i * loader->ucd_size);
		s->ucd_pa = loader->area_pa + loader->ucd_offset + i * loader->ucd_size;
		if (i >= 1 && i <= slots) {
			vm_offset_t data = 0;

			if (kmem_alloc_contig(kernel_map, &data, round_page(hc_slot_buffer), PAGE_MASK, 0, 0,
			    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				hc_say("ufs: native: not used: no memory for the slots' buffers\n");
				return false;
			}
			s->data = (uint8_t *)data;
			s->data_pa = kvtophys(data);
			FlushPoC_DcacheRegion(data, hc_slot_buffer);
		}
		hc_make_tur(&c, 0, (uint8_t)i);
		if (hc_place(s, &c, 0) != 0) {
			hc_say("ufs: native: not used: slot %u would not take its TEST UNIT READY\n", i);
			return false;
		}
	}
	PE_parse_boot_argn("ufs_native_spin", &hc_spin_us, sizeof(hc_spin_us));
	if (hc_spin_us > 5000) {
		hc_spin_us = 5000;
	}
	{
		thread_t th;

		if (kernel_thread_start(hc_poller, NULL, &th) != KERN_SUCCESS) {
			hc_say("ufs: native: not used: no poller thread\n");
			return false;
		}
		thread_deallocate(th);
	}
	hc_nslots = slots;
	hc_free = (uint32_t)(((1ULL << (slots + 1)) - 1) & ~1ULL);       /* slots 1..n */
	hc_usable = true;
	hc_say("ufs: native: usable: a list of %u slots at %llx (the loader's), reads in %u of them, %u KB each (the logical units reads may go to are set by redwood_ufs.c)%s\n",
	    SLOTS, loader->area_pa, slots, hc_slot_buffer >> 10, hc_leave ? "; ufs_native_leave=1: the list is left in "
	    "at the end, for the loader to take back" : "");
	return true;
}

/*
 * Before the kernel gives the phone back to the loader (loader_return, the other processors
 * already stopped): nothing more is sent, what is in flight is waited for, UEFI's list put back.
 * No locks (a stopped processor may hold one).
 */
void
redwood_ufs_quiesce(void)
{
	struct hc_put_back_result r;

	if (hc_kept_count != 0) {
		hc_say_late("ufs: native: what happened in this run, again:\n");
		for (uint32_t i = hc_kept_count > KEPT_LINES ? hc_kept_count - KEPT_LINES : 0; i < hc_kept_count; i++) {
			const char *line = hc_kept[i % KEPT_LINES];

			hc_say_late("> %s%s", line, line[0] != 0 && line[strlen(line) - 1] == '\n' ? "" : "...\n");
		}
	}
	if (!hc_usable) {
		return;
	}
	hc_closed = true;
	__asm__ volatile ("dsb sy" ::: "memory");
	hc_say_late("ufs: native: at the end: entered with doorbell %08x, %u reads of this kernel's in flight, its list %s\n",
	    hc_read(UFSHC_UTRLDBR), hc_in_flight, hc_in ? "in" : "out");
	if (!hc_in && hc_read(UFSHC_UTRLBA) == hc_loader.uefi_lo && hc_read(UFSHC_UTRLBAU) == hc_loader.uefi_hi &&
	    (hc_read(UFSHC_UTRLRSR) & 1)) {
		hc_say_late("ufs: native: at the end: UEFI's list is in (doorbell %08x)\n", hc_read(UFSHC_UTRLDBR));
		hc_report("at the end", true);
		return;
	}
	hc_in = false;
	if (hc_leave) {
		hc_say_late("ufs: native: at the end: this kernel's list LEFT IN for the loader to take back "
		    "(ufs_native_leave=1; doorbell %08x)\n", hc_read(UFSHC_UTRLDBR));
		hc_report("at the end", true);
		return;
	}
	hc_put_back(&r);
	hc_put_back_print("at the end", &r, true);
	hc_report("at the end", true);
}

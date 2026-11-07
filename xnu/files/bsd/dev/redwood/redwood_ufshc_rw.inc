/*
 * redwood_ufshc_rw.inc - SCSI command formation, hardware whitelist, slot queue, and I/O execution.
 * Included by redwood_ufshc.c to keep translation units strictly under 1200 lines.
 */

#ifndef _REDWOOD_UFSHC_RW_INC_
#define _REDWOOD_UFSHC_RW_INC_

static uint64_t hc_write_lba_start, hc_write_lba_end;
static uint8_t hc_write_lun;
static bool hc_write_enabled;

void
ufshc_set_write_range(uint8_t lun, uint64_t start_lba, uint64_t end_lba)
{
	lck_mtx_lock(&hc_lock);
	hc_write_lun = lun;
	hc_write_lba_start = start_lba;
	hc_write_lba_end = end_lba;
	hc_write_enabled = (end_lba > start_lba);
	lck_mtx_unlock(&hc_lock);
	hc_say("ufs: native: write range registered for LUN %u: LBA %llu..%llu (%llu GiB)\n",
	    lun, start_lba, end_lba, (end_lba - start_lba) * BLOCK >> 30);
}

/* ---- the whitelist: the check every command passes before it goes into a slot ---- */

static const char *
hc_refusal(const uint8_t *upiu, uint32_t dw0, uint32_t lun_mask, bool any_command_type)
{
	const uint8_t *cdb = upiu + 16;
	uint32_t dd = dw0 & UTRD_DD_MASK;
	uint64_t expected = (uint64_t)upiu[12] << 24 | (uint64_t)upiu[13] << 16 | (uint64_t)upiu[14] << 8 | upiu[15];
	uint8_t lun = upiu[2];

	if (!any_command_type && (dw0 & UTRD_CT_MASK) != UTRD_CT_UFS) {
		return "not a UFS storage command type";
	}
	if ((upiu[0] & 0x3f) != UPIU_COMMAND) {
		return "not a SCSI command (a query, task management, NOP, ...)";
	}
	if (upiu[4] & 0x0f) {
		return "not the SCSI command set";
	}
	if (lun & LUN_WELL_KNOWN) {
		return "a well-known logical unit";
	}
	if (lun >= 8 || (lun_mask & (1u << lun)) == 0) {
		return "a logical unit operations do not go to now";
	}

	switch (cdb[0]) {
	case 0x28:      /* READ(10) */
	case 0x88: {    /* READ(16) */
		uint64_t blocks = cdb[0] == 0x28 ? ((uint64_t)cdb[7] << 8 | cdb[8]) :
		    ((uint64_t)cdb[10] << 24 | (uint64_t)cdb[11] << 16 | (uint64_t)cdb[12] << 8 | cdb[13]);

		if (dd != UTRD_DD_IN || (upiu[1] & UPIU_FLAG_READ) == 0 || (upiu[1] & UPIU_FLAG_WRITE) != 0) {
			return "a read without data from the device or with WRITE flag";
		}
		if (blocks == 0 || expected != blocks * BLOCK) {
			return "a read whose length is not what the UPIU expects";
		}
		return NULL;
	}
	case 0x2a:      /* WRITE(10) */
	case 0x8a: {    /* WRITE(16) */
		uint64_t lba = cdb[0] == 0x2a ?
		    ((uint64_t)cdb[2] << 24 | (uint64_t)cdb[3] << 16 | (uint64_t)cdb[4] << 8 | cdb[5]) :
		    ((uint64_t)cdb[2] << 56 | (uint64_t)cdb[3] << 48 | (uint64_t)cdb[4] << 40 | (uint64_t)cdb[5] << 32 |
		     (uint64_t)cdb[6] << 24 | (uint64_t)cdb[7] << 16 | (uint64_t)cdb[8] << 8 | cdb[9]);
		uint64_t blocks = cdb[0] == 0x2a ? ((uint64_t)cdb[7] << 8 | cdb[8]) :
		    ((uint64_t)cdb[10] << 24 | (uint64_t)cdb[11] << 16 | (uint64_t)cdb[12] << 8 | cdb[13]);

		if (dd != UTRD_DD_OUT || (upiu[1] & UPIU_FLAG_WRITE) == 0 || (upiu[1] & UPIU_FLAG_READ) != 0) {
			return "a write without data to the device or with READ flag";
		}
		if (blocks == 0 || expected != blocks * BLOCK) {
			return "a write whose length is not what the UPIU expects";
		}
		if (!hc_write_enabled || lun != hc_write_lun) {
			return "writes disabled or not targeted at writable logical unit";
		}
		if (lba < hc_write_lba_start || lba + blocks > hc_write_lba_end) {
			return "write outside permitted darwin_var LBA range";
		}
		return NULL;
	}
	case 0x00:      /* TEST UNIT READY */
		return dd == UTRD_DD_NONE && expected == 0 && (upiu[1] & (UPIU_FLAG_READ | UPIU_FLAG_WRITE)) == 0 ? NULL :
		       "TEST UNIT READY with data";
	case 0x03:      /* REQUEST SENSE */
	case 0x12:      /* INQUIRY */
	case 0x25:      /* READ CAPACITY(10) */
		return NULL;
	case 0x9e:      /* SERVICE ACTION IN(16): READ CAPACITY(16) only */
		return (cdb[1] & 0x1f) == 0x10 ? NULL : "a SERVICE ACTION IN other than READ CAPACITY(16)";
	default:
		return "a SCSI operation not on the list";
	}
}

static bool
ufshc_permitted(const uint8_t *upiu, uint32_t dw0)
{
	const char *why = hc_refusal(upiu, dw0, hc_lun_mask, false);

	if (why == NULL) {
		return true;
	}
	hc_refused++;
	hc_say("ufs: native: REFUSED (%s): UPIU %02x %02x %02x %02x, CDB %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, "
	    "descriptor %08x; it goes into no slot\n", why, upiu[0], upiu[1], upiu[2], upiu[3], upiu[16], upiu[17], upiu[18],
	    upiu[19], upiu[20], upiu[21], upiu[22], upiu[23], upiu[24], upiu[25], dw0);
	return false;
}

static void
hc_make_read(struct hc_cmd *c, uint8_t lun, uint8_t tag, uint32_t lba, uint32_t blocks)
{
	uint32_t bytes = blocks * BLOCK;

	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_COMMAND;
	c->upiu[1] = UPIU_FLAG_READ;
	c->upiu[2] = lun;
	c->upiu[3] = tag;
	c->upiu[12] = (uint8_t)(bytes >> 24);
	c->upiu[13] = (uint8_t)(bytes >> 16);
	c->upiu[14] = (uint8_t)(bytes >> 8);
	c->upiu[15] = (uint8_t)bytes;
	c->upiu[16] = 0x28;                     /* READ(10) */
	c->upiu[18] = (uint8_t)(lba >> 24);
	c->upiu[19] = (uint8_t)(lba >> 16);
	c->upiu[20] = (uint8_t)(lba >> 8);
	c->upiu[21] = (uint8_t)lba;
	c->upiu[23] = (uint8_t)(blocks >> 8);
	c->upiu[24] = (uint8_t)blocks;
	c->dw0 = UTRD_CT_UFS | UTRD_DD_IN;
}

static void
hc_make_write(struct hc_cmd *c, uint8_t lun, uint8_t tag, uint32_t lba, uint32_t blocks)
{
	uint32_t bytes = blocks * BLOCK;

	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_COMMAND;
	c->upiu[1] = UPIU_FLAG_WRITE;
	c->upiu[2] = lun;
	c->upiu[3] = tag;
	c->upiu[12] = (uint8_t)(bytes >> 24);
	c->upiu[13] = (uint8_t)(bytes >> 16);
	c->upiu[14] = (uint8_t)(bytes >> 8);
	c->upiu[15] = (uint8_t)bytes;
	c->upiu[16] = 0x2a;                     /* WRITE(10) */
	c->upiu[18] = (uint8_t)(lba >> 24);
	c->upiu[19] = (uint8_t)(lba >> 16);
	c->upiu[20] = (uint8_t)(lba >> 8);
	c->upiu[21] = (uint8_t)lba;
	c->upiu[23] = (uint8_t)(blocks >> 8);
	c->upiu[24] = (uint8_t)blocks;
	c->dw0 = UTRD_CT_UFS | UTRD_DD_OUT;
}

static void
hc_make_tur(struct hc_cmd *c, uint8_t lun, uint8_t tag)
{
	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_COMMAND;
	c->upiu[2] = lun;
	c->upiu[3] = tag;
	c->upiu[16] = 0x00;                     /* TEST UNIT READY */
	c->dw0 = UTRD_CT_UFS | UTRD_DD_NONE;
}

static bool
hc_whitelist_selftest(void)
{
	static const struct {
		uint8_t type, flags, lun, op, cdb1;
		uint16_t blocks;
		uint32_t expected;
		uint32_t dd;
		bool pass;
		const char *what;
	} cases[] = {
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x28, 0, 1, 4096, UTRD_DD_IN, true, "READ(10) of LUN 0" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x88, 0, 1, 4096, UTRD_DD_IN, true, "READ(16) of LUN 0" },
		{ UPIU_COMMAND, 0, 0, 0x00, 0, 0, 0, UTRD_DD_NONE, true, "TEST UNIT READY of LUN 0" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x25, 0, 0, 8, UTRD_DD_IN, true, "READ CAPACITY(10)" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x2a, 0, 1, 4096, UTRD_DD_OUT, false, "WRITE(10) when writes disabled" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x8a, 0, 1, 4096, UTRD_DD_OUT, false, "WRITE(16) when writes disabled" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x2a, 0, 1, 4096, UTRD_DD_IN, false, "WRITE(10) dressed as a read" },
		{ UPIU_COMMAND, 0, 0, 0x2a, 0, 0, 0, UTRD_DD_NONE, false, "WRITE(10) with no data" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x42, 0, 0, 24, UTRD_DD_OUT, false, "UNMAP" },
		{ UPIU_COMMAND, 0, 0, 0x42, 0, 0, 0, UTRD_DD_NONE, false, "UNMAP with no data" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x28, 0, 1, 4096, UTRD_DD_OUT, false, "READ(10) with data host to device" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x28, 0, 1, 4096, UTRD_DD_MASK, false, "READ(10) with direction 3" },
		{ UPIU_COMMAND, UPIU_FLAG_READ | UPIU_FLAG_WRITE, 0, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) with the W flag" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) with only the W flag" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x28, 0, 1, 8192, UTRD_DD_IN, false, "READ(10) length mismatch" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0xc4, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of RPMB" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0xb0, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of BOOT" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0xd0, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of device unit" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 0x81, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of REPORT LUNS" },
		{ UPIU_COMMAND, UPIU_FLAG_READ, 3, 0x28, 0, 1, 4096, UTRD_DD_IN, false, "READ(10) of unpermitted LUN 3" },
		{ UPIU_COMMAND, 0, 0, 0x04, 0, 0, 0, UTRD_DD_NONE, false, "FORMAT UNIT" },
		{ UPIU_COMMAND, 0, 0, 0x1b, 0, 0, 0, UTRD_DD_NONE, false, "START STOP UNIT" },
		{ UPIU_COMMAND, 0, 0, 0x35, 0, 0, 0, UTRD_DD_NONE, false, "SYNCHRONIZE CACHE(10)" },
		{ UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0x3b, 0, 0, 4096, UTRD_DD_OUT, false, "WRITE BUFFER" },
		{ 0x16, 0, 0, 0, 0, 0, 0, UTRD_DD_NONE, false, "a QUERY REQUEST" },
		{ 0x04, 0, 0, 0, 0, 0, 0, UTRD_DD_NONE, false, "TASK MANAGEMENT" },
		{ 0x00, 0, 0, 0, 0, 0, 0, UTRD_DD_NONE, false, "NOP OUT" },
	};
	bool right = true;

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint8_t upiu[32] = { 0 };
		bool passed;

		upiu[0] = cases[i].type;
		upiu[1] = cases[i].flags;
		upiu[2] = cases[i].lun;
		upiu[12] = (uint8_t)(cases[i].expected >> 24);
		upiu[13] = (uint8_t)(cases[i].expected >> 16);
		upiu[14] = (uint8_t)(cases[i].expected >> 8);
		upiu[15] = (uint8_t)cases[i].expected;
		upiu[16] = cases[i].op;
		upiu[17] = cases[i].cdb1;
		if (cases[i].op == 0x88 || cases[i].op == 0x8a) {
			upiu[16 + 12] = (uint8_t)(cases[i].blocks >> 8);
			upiu[16 + 13] = (uint8_t)cases[i].blocks;
		} else {
			upiu[16 + 7] = (uint8_t)(cases[i].blocks >> 8);
			upiu[16 + 8] = (uint8_t)cases[i].blocks;
		}
		passed = hc_refusal(upiu, UTRD_CT_UFS | cases[i].dd, 1u << 0, false) == NULL;
		if (passed != cases[i].pass) {
			hc_say("ufs: native: the whitelist %s %s\n", passed ? "LETS THROUGH" : "refuses", cases[i].what);
			right = false;
		}
	}
	/* verify write range whitelist check */
	{
		struct hc_cmd c;

		hc_write_enabled = true;
		hc_write_lun = 0;
		hc_write_lba_start = 10000;
		hc_write_lba_end = 20000;

		hc_make_write(&c, 0, 1, 12000, 16);
		right &= (hc_refusal(c.upiu, c.dw0, 1u << 0, false) == NULL);

		hc_make_write(&c, 0, 1, 9000, 16);   /* under start */
		right &= (hc_refusal(c.upiu, c.dw0, 1u << 0, false) != NULL);

		hc_make_write(&c, 0, 1, 19990, 20);  /* past end */
		right &= (hc_refusal(c.upiu, c.dw0, 1u << 0, false) != NULL);

		hc_make_write(&c, 1, 1, 12000, 16);  /* wrong LUN */
		right &= (hc_refusal(c.upiu, c.dw0, 1u << 0, false) != NULL);

		hc_write_enabled = false;
	}
	return right;
}

static void
hc_put(volatile uint32_t *to, const void *from, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes / 4; i++) {
		uint32_t w;

		memcpy(&w, (const uint8_t *)from + 4 * i, 4);
		to[i] = w;
	}
}

static int
hc_place(struct hc_slot *s, const struct hc_cmd *c, uint32_t bytes)
{
	uint32_t n = 0, back[8];

	if (!ufshc_permitted(c->upiu, c->dw0)) {
		return EPERM;
	}
	hc_put(s->ucd, c->upiu, sizeof(c->upiu));
	for (uint32_t i = sizeof(c->upiu) / 4; i < UCD_PRDT / 4; i++) {
		s->ucd[i] = 0;
	}
	for (uint32_t at = 0; at < bytes; at += PRD_MAX, n++) {
		volatile uint32_t *prd = s->ucd + (UCD_PRDT + 16 * n) / 4;
		uint64_t pa = s->data_pa + at;
		uint32_t len = bytes - at < PRD_MAX ? bytes - at : PRD_MAX;

		prd[0] = (uint32_t)pa;
		prd[1] = (uint32_t)(pa >> 32);
		prd[2] = 0;
		prd[3] = len - 1;
	}
	s->utrd[0] = c->dw0;
	s->utrd[1] = 0;
	s->utrd[2] = OCS_INVALID;
	s->utrd[3] = 0;
	s->utrd[4] = (uint32_t)s->ucd_pa;
	s->utrd[5] = (uint32_t)(s->ucd_pa >> 32);
	s->utrd[6] = ((UCD_RESPONSE / 4) << 16) | (UCD_RESPONSE / 4);
	s->utrd[7] = ((UCD_PRDT / 4) << 16) | n;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int i = 0; i < 8; i++) {
		back[i] = s->ucd[i];
	}
	if (memcmp(back, c->upiu, sizeof(back)) != 0 || s->utrd[0] != c->dw0 ||
	    !ufshc_permitted((const uint8_t *)back, s->utrd[0])) {
		hc_say("ufs: native: slot %u does not hold what was put in it: not rung\n", s->number);
		hc_refused++;
		return EPERM;
	}
	return 0;
}

static int
hc_ring(struct hc_slot *s)
{
	uint32_t bit = 1u << s->number;
	boolean_t istate;
	bool ok;

	lck_mtx_lock(&hc_lock);
	istate = ml_set_interrupts_enabled(FALSE);
	ok = hc_in && !hc_closed && !hc_dead && *hc_loader.take_back == 0 &&
	    hc_read(UFSHC_UTRLBA) == hc_list_lo && hc_read(UFSHC_UTRLBAU) == hc_list_hi &&
	    (hc_read(UFSHC_UTRLRSR) & 1) != 0 && (hc_read(UFSHC_UTRLDBR) & bit) == 0;
	if (ok) {
		hc_write(UFSHC_UTRLDBR, bit);
	}
	ml_set_interrupts_enabled(istate);
	lck_mtx_unlock(&hc_lock);
	return ok ? 0 : ENXIO;
}

extern unsigned int cpu_broadcast_xcall(uint32_t *synch, boolean_t self_xcall, void (*func)(void *), void *parm);
static uint32_t hc_hang_after;
static volatile uint32_t hc_hanging;

static void
hc_spin_forever(__unused void *parm)
{
	(void)ml_set_interrupts_enabled(FALSE);
	for (;;) {
		__builtin_arm_yield();
	}
}

static void
hc_hang(uint32_t slot)
{
	uint32_t synch = 0;

	hc_say("ufs: native: ufs_native_hang=%u: stopping the kernel with slot %u in flight\n", hc_hang_after, slot);
	cpu_broadcast_xcall(&synch, TRUE, hc_spin_forever, NULL);
}

static uint32_t hc_waiting, hc_done;
static bool hc_poller_asleep;
static uint64_t hc_poller_rounds, hc_poller_polls, hc_sleeps;
static uint32_t hc_spin_us = 100;

static void
hc_poller(__unused void *parm, __unused wait_result_t wr)
{
	for (;;) {
		uint32_t waiting, db, done;

		lck_mtx_lock(&hc_lock);
		while (hc_waiting == 0) {
			hc_poller_asleep = true;
			msleep((void *)&hc_waiting, &hc_lock, PRIBIO, "ufshc-poll", NULL);
		}
		hc_poller_asleep = false;
		waiting = hc_waiting;
		lck_mtx_unlock(&hc_lock);
		hc_poller_rounds++;
		while (((db = hc_read(UFSHC_UTRLDBR)) & waiting) == waiting && hc_waiting == waiting) {
			hc_poller_polls++;
			__builtin_arm_yield();
		}
		done = waiting & ~db;
		if (done != 0) {
			lck_mtx_lock(&hc_lock);
			hc_done |= done;
			hc_waiting &= ~done;
			for (uint32_t m = done; m != 0; m &= m - 1) {
				wakeup(&hc_slots[__builtin_ctz(m)]);
			}
			lck_mtx_unlock(&hc_lock);
		}
	}
}

static int
hc_complete(struct hc_slot *s, const char *what, bool tur)
{
	uint64_t started = mach_absolute_time(), us;
	volatile const uint8_t *r = (volatile const uint8_t *)s->ucd + UCD_RESPONSE;
	uint32_t ocs, bit = 1u << s->number;
	uint64_t polls = 1;

	while ((hc_read(UFSHC_UTRLDBR) & bit) && hc_us_since(started) < hc_spin_us) {
		polls++;
		__builtin_arm_yield();
	}
	if (hc_read(UFSHC_UTRLDBR) & bit) {
		struct timespec second = { 1, 0 };

		lck_mtx_lock(&hc_lock);
		hc_sleeps++;
		hc_waiting |= bit;
		if (hc_poller_asleep) {
			wakeup((void *)&hc_waiting);
		}
		while ((hc_done & bit) == 0) {
			(void)msleep(s, &hc_lock, PRIBIO, "ufshc-io", &second);
			us = hc_us_since(started);
			if ((hc_done & bit) == 0 && us > TIMEOUT_MS * 1000ULL) {
				hc_waiting &= ~bit;
				lck_mtx_unlock(&hc_lock);
				hc_say("ufs: native: slot %u (%s) not done in %u s (doorbell %08x, IS %08x, HCS %08x): "
				    "this path is off from now on\n", s->number, what, TIMEOUT_MS / 1000, hc_read(UFSHC_UTRLDBR),
				    hc_read(UFSHC_IS), hc_read(UFSHC_HCS));
				hc_off();
				return ETIMEDOUT;
			}
		}
		hc_done &= ~bit;
		lck_mtx_unlock(&hc_lock);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	os_atomic_add(&hc_polls, polls, relaxed);
	if (!tur) {
		os_atomic_add(&hc_wait_us, hc_us_since(started), relaxed);
	}
	ocs = s->utrd[2] & 0xff;
	if (ocs != OCS_SUCCESS || (r[0] & 0x3f) != UPIU_RESPONSE || r[3] != s->number || r[6] != 0 ||
	    (r[7] != 0 && !(tur && r[7] == 0x02))) {
		hc_say("ufs: native: slot %u (%s): OCS %x, response %02x tag %02x, response %02x status %02x, "
		    "sense key %x asc %02x ascq %02x\n", s->number, what, ocs, r[0], r[3], r[6], r[7],
		    r[34 + 2] & 0x0f, r[34 + 12], r[34 + 13]);
		if (!tur) {
			os_atomic_inc(&hc_errors, relaxed);
		}
		return EIO;
	}
	return 0;
}

static struct hc_slot *
hc_take(bool wait)
{
	struct hc_slot *s = NULL;
	uint64_t t0 = mach_absolute_time();

	lck_mtx_lock(&hc_lock);
	hc_lock_ticks += mach_absolute_time() - t0;
	if (hc_free == 0 && wait && !hc_dead && hc_in) {
		uint64_t w0 = mach_absolute_time();

		hc_slot_waits++;
		while (hc_free == 0 && wait && !hc_dead && hc_in) {
			msleep(&hc_free, &hc_lock, PRIBIO, "ufshc", NULL);
		}
		hc_slot_wait_ticks += mach_absolute_time() - w0;
	}
	if (hc_free != 0 && hc_in && !hc_dead) {
		uint32_t i = (uint32_t)__builtin_ctz(hc_free), f;

		hc_free &= ~(1u << i);
		s = &hc_slots[i];
		f = ++hc_in_flight;
		if (f > hc_most_in_flight) {
			hc_most_in_flight = f;
		}
		hc_flight_hist[f <= 1 ? 0 : f <= 2 ? 1 : f <= 4 ? 2 : f <= 8 ? 3 : f <= 16 ? 4 : 5]++;
	}
	lck_mtx_unlock(&hc_lock);
	return s;
}

static void
hc_give(struct hc_slot *s)
{
	lck_mtx_lock(&hc_lock);
	hc_free |= 1u << (s - hc_slots);
	hc_in_flight--;
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
}

int
ufshc_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len)
{
	struct {
		struct hc_slot *slot;
		uint32_t lba, blocks, skip, take;
		uint8_t *to;
	} q[SLOTS];
	uint32_t head = 0, tail = 0, per = hc_slot_buffer / BLOCK;
	int error = 0;

	if (!hc_usable || !hc_in || hc_dead || hc_closed) {
		return ENXIO;
	}
	while ((len > 0 && error == 0) || head != tail) {
		while (len > 0 && error == 0 && tail - head < SLOTS) {
			uint64_t first = offset / BLOCK, skip = offset % BLOCK;
			uint64_t blocks = (skip + len + BLOCK - 1) / BLOCK, take;
			struct hc_slot *s = hc_take(head == tail);
			struct hc_cmd c;
			int e;

			if (s == NULL) {
				if (head == tail) {
					error = ENXIO;
				}
				break;
			}
			if (blocks > per) {
				blocks = per;
			}
			if (first + blocks > 0xffffffffULL) {
				hc_give(s);
				error = EINVAL;
				break;
			}
			take = blocks * BLOCK - skip;
			if (take > len) {
				take = len;
			}
			hc_make_read(&c, lun, (uint8_t)s->number, (uint32_t)first, (uint32_t)blocks);
			FlushPoC_DcacheRegion((vm_offset_t)s->data, (size_t)blocks * BLOCK);
			e = hc_place(s, &c, (uint32_t)blocks * BLOCK);
			if (e == 0) {
				e = hc_ring(s);
			}
			if (e == 0 && hc_hang_after != 0 && hc_requests >= hc_hang_after &&
			    os_atomic_cmpxchg(&hc_hanging, 0, 1, relaxed)) {
				hc_hang(s->number);
			}
			if (e != 0) {
				hc_give(s);
				error = e;
				break;
			}
			q[tail % SLOTS].slot = s;
			q[tail % SLOTS].lba = (uint32_t)first;
			q[tail % SLOTS].blocks = (uint32_t)blocks;
			q[tail % SLOTS].skip = (uint32_t)skip;
			q[tail % SLOTS].take = (uint32_t)take;
			q[tail % SLOTS].to = dst;
			tail++;
			offset += take;
			dst += take;
			len -= take;
		}
		if (head != tail) {
			typeof(q[0]) *e = &q[head % SLOTS];
			char what[48];
			int r;

			snprintf(what, sizeof(what), "LUN %u, blocks %u..%u", lun, e->lba, e->lba + e->blocks - 1);
			r = hc_complete(e->slot, what, false);
			if (r == 0) {
				FlushPoC_DcacheRegion((vm_offset_t)e->slot->data, (size_t)e->blocks * BLOCK);
				os_atomic_inc(&hc_requests, relaxed);
				os_atomic_add(&hc_bytes, (uint64_t)e->blocks * BLOCK, relaxed);
				if (error == 0) {
					memcpy(e->to, e->slot->data + e->skip, e->take);
				}
			} else if (error == 0) {
				error = r;
			}
			if (r != ETIMEDOUT) {
				hc_give(e->slot);
			}
			head++;
		}
	}
	return error;
}

int
ufshc_write(uint8_t lun, uint64_t offset, const uint8_t *src, uint64_t len)
{
	struct {
		struct hc_slot *slot;
		uint32_t lba, blocks;
	} q[SLOTS];
	uint32_t head = 0, tail = 0, per = hc_slot_buffer / BLOCK;
	int error = 0;

	if (!hc_usable || !hc_in || hc_dead || hc_closed || !hc_write_enabled) {
		return ENXIO;
	}
	if (offset % BLOCK != 0 || len % BLOCK != 0) {
		return EINVAL;
	}

	while ((len > 0 && error == 0) || head != tail) {
		while (len > 0 && error == 0 && tail - head < SLOTS) {
			uint64_t first = offset / BLOCK;
			uint64_t blocks = len / BLOCK;
			struct hc_slot *s = hc_take(head == tail);
			struct hc_cmd c;
			int e;

			if (s == NULL) {
				if (head == tail) {
					error = ENXIO;
				}
				break;
			}
			if (blocks > per) {
				blocks = per;
			}
			if (first + blocks > 0xffffffffULL) {
				hc_give(s);
				error = EINVAL;
				break;
			}

			hc_make_write(&c, lun, (uint8_t)s->number, (uint32_t)first, (uint32_t)blocks);
			memcpy(s->data, src, (size_t)blocks * BLOCK);
			FlushPoC_DcacheRegion((vm_offset_t)s->data, (size_t)blocks * BLOCK);

			e = hc_place(s, &c, (uint32_t)blocks * BLOCK);
			if (e == 0) {
				e = hc_ring(s);
			}
			if (e != 0) {
				hc_give(s);
				error = e;
				break;
			}
			q[tail % SLOTS].slot = s;
			q[tail % SLOTS].lba = (uint32_t)first;
			q[tail % SLOTS].blocks = (uint32_t)blocks;
			tail++;
			offset += blocks * BLOCK;
			src += blocks * BLOCK;
			len -= blocks * BLOCK;
		}

		if (head != tail) {
			typeof(q[0]) *e = &q[head % SLOTS];
			char what[48];
			int r;

			snprintf(what, sizeof(what), "WRITE LUN %u, blocks %u..%u", lun, e->lba, e->lba + e->blocks - 1);
			r = hc_complete(e->slot, what, false);
			if (r == 0) {
				os_atomic_inc(&hc_write_requests, relaxed);
				os_atomic_add(&hc_write_bytes, (uint64_t)e->blocks * BLOCK, relaxed);
			} else if (error == 0) {
				error = r;
				os_atomic_inc(&hc_write_errors, relaxed);
			}
			if (r != ETIMEDOUT) {
				hc_give(e->slot);
			}
			head++;
		}
	}
	return error;
}

#endif /* _REDWOOD_UFSHC_RW_INC_ */

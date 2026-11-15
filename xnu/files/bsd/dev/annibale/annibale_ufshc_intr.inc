/*
 * annibale_ufshc_intr.inc - GICv3 SPI interrupt handling, aggregation, and asynchronous queue completion.
 * Included by annibale_ufshc.c to keep translation units strictly under 1200 lines.
 */

#ifndef _ANNIBALE_UFSHC_INTR_INC_
#define _ANNIBALE_UFSHC_INTR_INC_


#define UFSHC_SPI_INTID     297u       /* DT <0 0x109 4>: 32 + 265 = 297 */

/* UFSHC_IS & UFSHC_IE bit masks (JESD223) */
#define UFSHC_IS_UTRCS      (1u << 0)  /* UTP Transfer Request Completion Status */
#define UFSHC_IS_UDEPRI     (1u << 1)
#define UFSHC_IS_UE         (1u << 2)  /* UIC Error */
#define UFSHC_IS_UTMS       (1u << 3)
#define UFSHC_IS_UPMS       (1u << 4)  /* UIC Power Mode Status */
#define UFSHC_IS_UHXS       (1u << 5)
#define UFSHC_IS_UHES       (1u << 6)
#define UFSHC_IS_ULLS       (1u << 7)
#define UFSHC_IS_ULSS       (1u << 8)
#define UFSHC_IS_UTMRCS     (1u << 9)
#define UFSHC_IS_UCCS       (1u << 10)
#define UFSHC_IS_DFES       (1u << 11)
#define UFSHC_IS_UTPES      (1u << 12)
#define UFSHC_IS_HCFES      (1u << 16)
#define UFSHC_IS_SBFES      (1u << 17)

#define UFSHC_IE_UTRCE      (1u << 0)  /* UTP Transfer Request Completion Enable */
#define UFSHC_IE_UE         (1u << 2)  /* UIC Error Enable */
#define UFSHC_IE_UTPMS      (1u << 4)  /* UIC Power Mode Status Enable */

/* UFSHC_UTRIACR bit masks */
#define UTRIACR_IAEN        (1u << 31) /* Interrupt Aggregation Enable */
#define UTRIACR_IAPWEN      (1u << 24) /* Parameter Write Enable */
#define UTRIACR_CTR         (1u << 16) /* Counter and Timer Reset */
#define UTRIACR_IACTH(c)    (((c) & 0x1fu) << 8)  /* Counter threshold (4) */
#define UTRIACR_IATOVAL(t)  ((t) & 0xffu)          /* Timeout value (80 us, 2 ticks of 40 us) */

static uint32_t hc_completion_event;
static uint32_t hc_watchdog_event;
static thread_t hc_completion_thread_ref = THREAD_NULL;

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void thread_set_thread_name(thread_t thread, const char *name);

#ifndef _UFSHC_DCACHE_CLEAN_INVAL_DEFINED
#define _UFSHC_DCACHE_CLEAN_INVAL_DEFINED
static inline void
ufshc_dcache_clean_inval(vm_offset_t va, size_t length)
{
	FlushPoC_DcacheRegion(va, length);
}
#endif

static void
ufshc_process_completions(void)
{
	uint32_t pending = os_atomic_xchg(&hc_pending_slots, 0, acquire);
	uint32_t active = os_atomic_load(&hc_active_slots, acquire);
	uint32_t dbr = hc_read(UFSHC_UTRLDBR);
	uint32_t done = pending | (active & ~dbr);

	if (done == 0) {
		return;
	}

	for (uint32_t i = 0; i < SLOTS; i++) {
		if ((done & (1u << i)) == 0) {
			continue;
		}

		if ((hc_read(UFSHC_UTRLDBR) & (1u << i)) != 0) {
			continue;
		}

		if ((os_atomic_and_orig(&hc_active_slots, ~(1u << i), acq_rel) & (1u << i)) == 0) {
			continue;
		}

		struct hc_slot *s = &hc_slots[i];
		__asm__ volatile ("dsb sy" ::: "memory");

		uint32_t ocs = s->utrd[2] & 0xff;

		if (s->is_async && s->bp != NULL) {
			buf_t bp = s->bp;
			s->bp = NULL;
			bool is_write = s->is_write;
			uint32_t count = (uint32_t)s->blocks * BLOCK;

			if (ocs == OCS_SUCCESS) {
				buf_setresid(bp, 0);
				if (is_write) {
					os_atomic_inc(&hc_write_requests, relaxed);
					os_atomic_add(&hc_write_bytes, (uint64_t)count, relaxed);
				} else {
					os_atomic_inc(&hc_requests, relaxed);
					os_atomic_add(&hc_bytes, (uint64_t)count, relaxed);
				}
			} else {
				volatile const uint8_t *r = (volatile const uint8_t *)s->ucd + UCD_RESPONSE;

				/* The same fields as the synchronous path prints, so an async EIO can be traced to its slot. */
				hc_say("ufs: native: slot %u (async %s, %u blocks): OCS %x, response %02x tag %02x, "
				    "response %02x status %02x, sense key %x asc %02x ascq %02x\n", s->number,
				    is_write ? "write" : "read", s->blocks, ocs, r[0], r[3], r[6], r[7],
				    r[34 + 2] & 0x0f, r[34 + 12], r[34 + 13]);
				buf_seterror(bp, EIO);
				if (is_write) {
					os_atomic_inc(&hc_write_errors, relaxed);
				} else {
					os_atomic_inc(&hc_errors, relaxed);
				}
			}

			/*
			 * Non-coherent DMA async read completion:
			 * Flush CPU cache lines for PRDT physical segments before buf_biodone.
			 */
			if (!is_write && !hc_dma_coherent && ocs == OCS_SUCCESS) {
				uint32_t nsegs = s->utrd[7] & 0xffff;
				if (nsegs > UCD_PRDT_ENTRIES) {
					nsegs = UCD_PRDT_ENTRIES;
				}
				for (uint32_t k = 0; k < nsegs; k++) {
					volatile uint32_t *prd = s->ucd + (UCD_PRDT + 16 * k) / 4;
					uint64_t pa = (uint64_t)prd[0] | ((uint64_t)prd[1] << 32);
					uint32_t len = prd[3] + 1;
					ufshc_dcache_sg_poc(pa, len, false);
				}
			}

			os_atomic_inc(&hc_async_completions, relaxed);
			/* Executed in kernel thread context outside lock; buf_biodone is strictly forbidden in ISR */
			buf_biodone(bp);

			/* Slot return and wakeup strictly under hc_lock to eliminate lost-wakeup race with hc_take */
			lck_mtx_lock(&hc_lock);
			hc_give(s);
			lck_mtx_unlock(&hc_lock);
		}
	}
}

/*
 * Hardware interrupt top-half handler (GICv3 SPI 297).
 * STRICT INTEGRITY RULE: buf_biodone(bp) MUST NEVER BE CALLED FROM THIS ISR!
 * Top-half ONLY clears interrupt status (W1C), records completed slot bits into
 * hc_pending_slots, and wakes up hc_completion_thread via thread_wakeup().
 */
static void
ufshc_intr(__unused void *context, __unused uint32_t intid)
{
	uint32_t is = hc_read(UFSHC_IS);
	if (is == 0) {
		return;
	}

	/* Clear handled interrupt status bits (W1C) */
	hc_write(UFSHC_IS, is);
	os_atomic_inc(&hc_interrupts, relaxed);

	uint32_t active = os_atomic_load(&hc_active_slots, acquire);
	if (active != 0) {
		uint32_t done = active & ~hc_read(UFSHC_UTRLDBR);
		if (done != 0) {
			os_atomic_or(&hc_pending_slots, done, release);
			thread_wakeup((event_t)&hc_completion_event);
		}
	}

	if (os_atomic_load(&hc_sync_waiters, acquire) != 0) {
		thread_wakeup((event_t)&hc_sync_event);
	}
}

/*
 * High-priority dedicated kernel thread for asynchronous UFS buffer completion.
 * Executes in normal thread context so buf_biodone(bp) can acquire mutexes safely.
 */
static void
hc_completion_thread(__unused void *parm, __unused wait_result_t wr)
{
	thread_set_thread_name(current_thread(), "ufs-completion");

	for (;;) {
		assert_wait((event_t)&hc_completion_event, THREAD_UNINT);
		if (hc_dead || hc_closed) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		if (os_atomic_load(&hc_pending_slots, acquire) != 0) {
			clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			thread_block(THREAD_CONTINUE_NULL);
		}

		if (hc_dead || hc_closed) {
			break;
		}

		ufshc_process_completions();
	}
}

static bool
ufshc_intr_init(void)
{
	/* Start dedicated high-priority completion thread */
	if (kernel_thread_start(hc_completion_thread, NULL, &hc_completion_thread_ref) != KERN_SUCCESS) {
		hc_say("ufs: native: warning: failed to start UFS completion thread\n");
		return false;
	}
	thread_deallocate(hc_completion_thread_ref);

	/* Configure UTRIACR: threshold 4 completions, 80 us timeout (2 * 40 us ticks) */
	uint32_t iacr = UTRIACR_IAEN | UTRIACR_IAPWEN | UTRIACR_CTR | UTRIACR_IACTH(4) | UTRIACR_IATOVAL(2);
	hc_write(UFSHC_UTRIACR, iacr);

	/* Enable interrupts: UTRCE | UE | UTPMS */
	uint32_t ie = UFSHC_IE_UTRCE | UFSHC_IE_UE | UFSHC_IE_UTPMS;
	hc_write(UFSHC_IE, ie);

	/* Attach GICv3 SPI interrupt (level-sensitive) */
	if (gic_v3_spi_attach(UFSHC_SPI_INTID, false, ufshc_intr, NULL) != KERN_SUCCESS) {
		hc_say("ufs: native: warning: failed to attach GICv3 SPI %u\n", UFSHC_SPI_INTID);
		return false;
	}

	hc_say("ufs: native: attached GICv3 SPI %u, IE %08x, UTRIACR %08x (4 completions / 80 us)\n",
	    UFSHC_SPI_INTID, ie, iacr);
	return true;
}

static void
ufshc_intr_quiesce(void)
{
	hc_write(UFSHC_IE, 0);
	hc_write(UFSHC_UTRIACR, UTRIACR_IAPWEN | UTRIACR_CTR);
	gic_v3_spi_mask(UFSHC_SPI_INTID);
	thread_wakeup((event_t)&hc_completion_event);
	thread_wakeup((event_t)&hc_sync_event);
}

static void
hc_watchdog(__unused void *parm, __unused wait_result_t wr)
{
	for (;;) {
		assert_wait_timeout((event_t)&hc_watchdog_event, THREAD_UNINT, 500, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);

		if (hc_dead || hc_closed) {
			break;
		}

		uint32_t active = os_atomic_load(&hc_active_slots, acquire);
		if (active == 0) {
			continue;
		}

		uint32_t cnr = hc_read(UFSHC_UTRLCNR);
		uint32_t db = hc_read(UFSHC_UTRLDBR);
		uint32_t unwoken = active & (cnr | ~db);
		if (unwoken != 0) {
			os_atomic_or(&hc_pending_slots, unwoken, release);
			thread_wakeup((event_t)&hc_completion_event);
		}

		if ((active & db) != 0) {
			for (uint32_t i = 1; i <= hc_nslots; i++) {
				if ((active & db & (1u << i)) && hc_slots[i].start_time != 0) {
					if (hc_us_since(hc_slots[i].start_time) > TIMEOUT_MS * 1000ULL) {
						/* Re-check hardware registers before declaring fatal timeout */
						cnr = hc_read(UFSHC_UTRLCNR);
						db = hc_read(UFSHC_UTRLDBR);
						if ((db & (1u << i)) == 0 || (cnr & (1u << i)) != 0) {
							os_atomic_or(&hc_pending_slots, (1u << i), release);
							thread_wakeup((event_t)&hc_completion_event);
							continue;
						}
						hc_say("ufs: native: watchdog: slot %u hung past %u s (DBR %08x, CNR %08x, IS %08x, HCS %08x)\n",
						    i, TIMEOUT_MS / 1000, db, cnr, hc_read(UFSHC_IS), hc_read(UFSHC_HCS));
						hc_off();
						break;
					}
				}
			}
		}
	}
}

#endif /* _ANNIBALE_UFSHC_INTR_INC_ */

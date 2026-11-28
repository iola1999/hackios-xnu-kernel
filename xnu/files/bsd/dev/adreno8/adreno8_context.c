/* SPDX-License-Identifier: GPL-2.0-only */
/* Standard gen8 HWSCHED context path, following adreno_gen8_hwsched_hfi.c and
 * adreno_hfi.h at OnePlus graphics-kernel revision
 * d50b305f7da9e14715a25120a4ac7b1a4b8b97c3. When adreno8_preempt=0 (default),
 * only slot 0 (CONTEXT_ID=1, RB0) is registered; adreno8_preempt=1 boots slot 0
 * and slot 1 (CONTEXT_ID=2, RB1) and lazily prepares/registers slots 2..15
 * (CONTEXT_ID=3..16, RB1) on acquire with per-slot fault isolation. */
#include "adreno8_private.h"
#include "adreno8_memory_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_gpu_private.h"
#include "adreno8_context_private.h"
#ifdef KERNEL
#include <sys/proc.h>
#endif

#define printf adreno8_say
#define CONTEXT_BYTES 4096u
#define CONTEXT_USER_RECORD_BYTES (192u * 1024u)
#define CONTEXT_HEADER_WORDS 12u
#define CONTEXT_QUEUE_WORDS (CONTEXT_BYTES / 4 - CONTEXT_HEADER_WORDS)
#define CONTEXT_SOP 0u
#define CONTEXT_EOP 8u
/* NO_GMEM_ALLOC, PREAMBLE, PER_CONTEXT_TS, NOTIFY, priority 1 (RB0) / 4 (RB1), GL. */
#define CONTEXT_FLAGS_RB0 0x00101072u
#define CONTEXT_FLAGS_RB1 0x00104072u
#define CONTEXT_NOTIFY (1u << 5)
#define CONTEXT_DOORBELL_RB0 (1u << 2)
#define CONTEXT_DOORBELL_RB1 (1u << 3)
#define CONTEXT_FENCE_SHIFT  ADRENO8_CONTEXT_FENCE_SHIFT
#define CONTEXT_FENCE_SLOT_MASK ADRENO8_CONTEXT_FENCE_SLOT_MASK
#define CONTEXT_FENCE_TS_MASK   ADRENO8_CONTEXT_FENCE_TS_MASK
/* Every wait for a timestamp, including an external buffer's unmap. The GMU's own hang report
 * (HFI 150, with the timestamps and a CP dump) has arrived on device both before and after two
 * seconds; a wait that gives up sooner closes submissions with neither. */
#define CONTEXT_WAIT_NS 2000000000ULL
/* A submission may run for seconds (the camera's still-image passes at the lowest GPU speed), so
 * a wait for a timestamp fails only after the end-of-pipe timestamp has stood still this long.
 * A real hang is reported by the GMU (HFI 150) and ends the wait through the completion poll. */
#define CONTEXT_STALL_NS 30000000000ULL
#define CONTEXT_INFLIGHT_MAX 8u
#define CONTEXT_CONSUME_NS 20000000ULL
#define CONTEXT_CHECK_NS 1000000ull /* 1 ms */

enum context_state { CONTEXT_COLD, CONTEXT_PREPARED, CONTEXT_REGISTERED, CONTEXT_FAILED };

struct adreno8_context_slot {
	uint32_t id, flags, doorbell_bit;
	bool needs_user_record;
	bool in_use;
	int owner_pid;
	enum context_state state;
	struct adreno8_memory *queue, *timestamps, *user_record;
	vm_offset_t queue_view, timestamp_view;
	uint32_t last, sequence;
	uint32_t fault_count, last_fault_ts, last_fault_err;
	uint64_t checked;
};

static struct adreno8_context_slot context_slots[ADRENO8_CONTEXT_SLOTS] = {
	{
		.id = 1u,
		.flags = CONTEXT_FLAGS_RB0,
		.doorbell_bit = CONTEXT_DOORBELL_RB0,
		.needs_user_record = false,
	},
	{
		.id = 2u,
		.flags = CONTEXT_FLAGS_RB1,
		.doorbell_bit = CONTEXT_DOORBELL_RB1,
		.needs_user_record = true,
	},
};

static struct adreno8_memory *test_ib, *test_result;
static bool context_tested, context_failed;

#define SUBMIT_HISTORY 16u
struct submit_record {
	uint32_t context_id, timestamp, count, ib0_addr, ib0_words, ib_last_addr;
	int pid;
};
static struct submit_record submit_history[SUBMIT_HISTORY];
static uint32_t submit_history_next;

static inline uint32_t
context_slot_count(void)
{
	return adreno8_gpu8_preempt ? ADRENO8_CONTEXT_SLOTS : 1u;
}

static inline uint32_t
context_boot_slot_count(void)
{
	return adreno8_gpu8_preempt ? 2u : 1u;
}

static inline void
context_slot_ensure_meta(uint32_t slot)
{
	struct adreno8_context_slot *s = &context_slots[slot];

	if (s->id)
		return;
	s->id = slot + 1u;
	s->flags = slot == ADRENO8_CONTEXT_SLOT_HIGH ? CONTEXT_FLAGS_RB0 : CONTEXT_FLAGS_RB1;
	s->doorbell_bit = slot == ADRENO8_CONTEXT_SLOT_HIGH ? CONTEXT_DOORBELL_RB0 : CONTEXT_DOORBELL_RB1;
	s->needs_user_record = slot != ADRENO8_CONTEXT_SLOT_HIGH;
}

static inline uint32_t
context_encode_fence(uint32_t slot, uint32_t ts)
{
	if (!adreno8_gpu8_preempt) return ts;
	return ((slot & 0x1fu) << CONTEXT_FENCE_SHIFT) | (ts & CONTEXT_FENCE_TS_MASK);
}

static inline void
context_decode_fence(uint32_t fence, uint32_t *slot, uint32_t *ts)
{
	if (!adreno8_gpu8_preempt) {
		*slot = 0u;
		*ts = fence;
		return;
	}
	*slot = (fence & CONTEXT_FENCE_SLOT_MASK) >> CONTEXT_FENCE_SHIFT;
	*ts = fence & CONTEXT_FENCE_TS_MASK;
}

static inline bool
context_ts_before(uint32_t a, uint32_t b)
{
	if (!adreno8_gpu8_preempt) return (int32_t)(a - b) < 0;
	return (int32_t)((uint32_t)(a - b) << (32u - CONTEXT_FENCE_SHIFT)) < 0;
}

static bool
context_fail(const char *what)
{
	if (context_failed) return false;
	context_failed = true;
	for (uint32_t i = 0; i < ADRENO8_CONTEXT_SLOTS; i++)
		context_slots[i].state = CONTEXT_FAILED;
	adreno8_reset_required = true;
	adreno8_hwsched_memory_failed();
	printf("adreno8: context %s failed; submissions closed and pages retained until cold reset\n", what);
	return false;
}

static bool
context_slot_fail(uint32_t slot, const char *what)
{
	if (!adreno8_gpu8_preempt || slot == ADRENO8_CONTEXT_SLOT_HIGH || slot >= context_slot_count())
		return context_fail(what);
	context_slot_ensure_meta(slot);
	struct adreno8_context_slot *s = &context_slots[slot];

	if (s->state == CONTEXT_FAILED)
		return false;
	s->state = CONTEXT_FAILED;
	s->fault_count++;
	printf("adreno8: ctx %u (slot %u) %s failed; context isolated while other contexts stay alive\n",
	    s->id, slot, what);
	return false;
}

static bool
context_health(void)
{
	if (context_failed) return false;
	if (context_slots[ADRENO8_CONTEXT_SLOT_HIGH].state == CONTEXT_FAILED)
		return context_fail("slot state");
	if (!adreno8_gpu_commands_ready() || !adreno8_hwsched_ready() ||
	    !adreno8_gpu_memory_ready() || adreno8_gpu_faulted())
		return context_fail("execution prerequisites");
	return true;
}

static bool
context_slot_health(uint32_t slot)
{
	if (!context_health() || slot >= context_slot_count())
		return false;
	if (context_slots[slot].state == CONTEXT_FAILED) {
		if (!adreno8_gpu8_preempt || slot == ADRENO8_CONTEXT_SLOT_HIGH)
			return context_fail("slot state");
		return false;
	}
	return true;
}

static bool
context_prepare_slot(uint32_t slot)
{
	if (!context_slot_health(slot)) return false;
	if (!adreno8_gpu_space.is_on || adreno8_gpu_space.table_count == 0 || adreno8_gpu_space.context != 0)
		return context_fail("allocation or GPU page table");
	context_slot_ensure_meta(slot);
	struct adreno8_context_slot *s = &context_slots[slot];

	if (s->state != CONTEXT_COLD) return true;
	const char *q_name = slot == 0 ? "context queue" : (slot == 1 ? "context queue 1" : "context queue slot");
	const char *ts_name = slot == 0 ? "context timestamps" : (slot == 1 ? "context timestamps 1" : "context timestamps slot");
	bool isolate = adreno8_gpu8_preempt && slot >= 2u;
	s->queue = adreno8_gmu_alloc(q_name,
	    CONTEXT_BYTES, CONTEXT_BYTES, ADRENO8_MEM_READ | ADRENO8_MEM_WRITE | ADRENO8_MEM_PRIV);
	s->timestamps = adreno8_gpu_alloc(ts_name,
	    CONTEXT_BYTES, CONTEXT_BYTES, ADRENO8_MEM_READ | ADRENO8_MEM_WRITE | ADRENO8_MEM_PRIV);
	if (!s->queue || !s->timestamps)
		return isolate ? context_slot_fail(slot, "allocation or GPU page table")
		               : context_fail("allocation or GPU page table");
	if (s->needs_user_record) {
		s->user_record = adreno8_gpu_alloc("context user record",
		    CONTEXT_USER_RECORD_BYTES, CONTEXT_BYTES, ADRENO8_MEM_READ | ADRENO8_MEM_WRITE);
		if (!s->user_record || !adreno8_memory_publish(s->user_record, 0, CONTEXT_USER_RECORD_BYTES))
			return isolate ? context_slot_fail(slot, "user record")
			               : context_fail("user record");
	}
	if (!adreno8_memory_publish(s->queue, 0, CONTEXT_BYTES) ||
	    !adreno8_memory_publish(s->timestamps, 0, CONTEXT_BYTES))
		return isolate ? context_slot_fail(slot, "initial publication")
		               : context_fail("initial publication");
	s->queue_view = ml_io_map_wcomb(kvtophys(s->queue->memory), CONTEXT_BYTES);
	s->timestamp_view = ml_io_map_wcomb(kvtophys(s->timestamps->memory), CONTEXT_BYTES);
	if (!s->queue_view || !s->timestamp_view)
		return isolate ? context_slot_fail(slot, "uncached views")
		               : context_fail("uncached views");
	/* After publication, only these aliases access the two shared pages. The
	 * queue's host and GMU fields share cache lines; cached cleans can lose updates. */
	adreno8_wr(s->queue_view + 4, s->queue->gmu_address + CONTEXT_HEADER_WORDS * 4);
	adreno8_wr(s->queue_view + 8, CONTEXT_QUEUE_WORDS);
	adreno8_settle();
	s->state = CONTEXT_PREPARED;
	printf("adreno8: context %u pages ready: queue GMU %#x timestamps GPU %#x\n",
	    s->id, s->queue->gmu_address, s->timestamps->gpu_address);
	return true;
}

bool
adreno8_context_prepare(void)
{
	for (uint32_t i = 0; i < context_boot_slot_count(); i++) {
		if (!context_prepare_slot(i))
			return false;
	}
	return true;
}

static bool
context_register_slot(uint32_t slot)
{
	if (!context_prepare_slot(slot)) return false;
	struct adreno8_context_slot *s = &context_slots[slot];
	uint32_t registration[6], pointers[9] = {0};
	uint64_t root;

	if (s->state == CONTEXT_REGISTERED) return true;
	/* The existing CB0 uses ASID 0. Context IDR identifies this HFI context. */
	root = adreno8_gpu_space.tables[0].phys;
	registration[0] = s->id;
	registration[1] = s->flags;
	registration[2] = (uint32_t)root;
	registration[3] = (uint32_t)(root >> 32);
	registration[4] = s->id;
	registration[5] = adreno8_gpu_space.context;
	if (!adreno8_hfi_send(128, registration, 6, "register context"))
		return context_slot_fail(slot, "registration ACK");
	pointers[0] = s->id;
	pointers[1] = s->timestamps->gpu_address + CONTEXT_SOP;
	pointers[3] = s->timestamps->gpu_address + CONTEXT_EOP;
	if (s->user_record) {
		pointers[5] = s->user_record->gpu_address;
		pointers[6] = (uint32_t)((uint64_t)s->user_record->gpu_address >> 32);
	}
	pointers[8] = s->queue->gmu_address;
	if (!adreno8_hfi_send(134, pointers, 9, "context pointers"))
		return context_slot_fail(slot, "pointer ACK");
	s->state = CONTEXT_REGISTERED;
	return true;
}

static bool context_wait_slot(uint32_t slot, uint32_t raw_ts);

static bool
context_unregister_slot(uint32_t slot, bool isolate_on_fail)
{
	struct adreno8_context_slot *s = &context_slots[slot];
	uint32_t unregister[2] = {s->id, s->last};

	if (s->state != CONTEXT_REGISTERED)
		return s->state == CONTEXT_PREPARED || s->state == CONTEXT_COLD;
	if ((s->last && !context_wait_slot(slot, s->last)) ||
	    !adreno8_gpu_wait_idle("context unregister idle") ||
	    !adreno8_hfi_send(129, unregister, 2, "unregister context") ||
	    !adreno8_gpu_wait_idle("context unregister completion"))
		return isolate_on_fail ? context_slot_fail(slot, "unregister") : context_fail("unregister");
	/* Firmware no longer owns the queue. Reuse pages and monotonic timestamps
	 * after a later registration; retain all physical memory and aliases. */
	uint32_t write = adreno8_rd(s->queue_view + 24);
	adreno8_wr(s->queue_view + 20, write);
	adreno8_settle();
	s->state = CONTEXT_PREPARED;
	return true;
}

static bool
context_queue_indices(struct adreno8_context_slot *s, uint32_t slot, uint32_t *read, uint32_t *write)
{
	*read = adreno8_rd(s->queue_view + 20);
	*write = adreno8_rd(s->queue_view + 24);
	if (adreno8_rd(s->queue_view) != 0 ||
	    adreno8_rd(s->queue_view + 4) != s->queue->gmu_address + CONTEXT_HEADER_WORDS * 4 ||
	    adreno8_rd(s->queue_view + 8) != CONTEXT_QUEUE_WORDS ||
	    *read >= CONTEXT_QUEUE_WORDS || *write >= CONTEXT_QUEUE_WORDS || ((*read | *write) & 3))
		return context_slot_fail(slot, "queue indices or header");
	adreno8_settle();
	return true;
}

/*
 * Every wait asks here every 100 us, and so does every program that polls a fence. The health
 * checks behind an answer read the SMMU fault registers of three contexts, the GMU's interrupt
 * state, the HFI queues and the context queue; they run at most once per CONTEXT_CHECK_NS, while
 * the timestamp is read every time.
 */
bool
adreno8_context_done(uint32_t timestamp)
{
	uint64_t now = mach_absolute_time(), since;
	uint32_t slot, ts, read, write;

	if (!timestamp || context_failed) return false;
	context_decode_fence(timestamp, &slot, &ts);
	if (slot >= context_slot_count() || !ts) return false;
	struct adreno8_context_slot *s = &context_slots[slot];

	if (!s->last || context_ts_before(s->last, ts) || s->state == CONTEXT_FAILED) return false;
	absolutetime_to_nanoseconds(now - s->checked, &since);
	if (s->checked == 0 || since >= CONTEXT_CHECK_NS) {
		if (!context_slot_health(slot)) return false;
		if (!adreno8_hfi_poll()) return context_fail("completion transport");
		if (!context_queue_indices(s, slot, &read, &write)) return false;
		s->checked = now;
	}
	adreno8_settle();
	uint32_t eop = adreno8_rd(s->timestamp_view + CONTEXT_EOP);
	if (context_ts_before(s->last, eop)) return context_slot_fail(slot, "EOP exceeds submitted timestamp");
	return !context_ts_before(eop, ts);
}

static bool
context_wait_slot(uint32_t slot, uint32_t raw_ts)
{
	struct adreno8_context_slot *s = &context_slots[slot];
	uint32_t fence = context_encode_fence(slot, raw_ts);
	uint64_t start = mach_absolute_time(), elapsed;
	uint32_t seen = adreno8_rd(s->timestamp_view + CONTEXT_EOP);

	while (!adreno8_context_done(fence)) {
		if (context_failed || s->state == CONTEXT_FAILED) return false;
		uint32_t eop = adreno8_rd(s->timestamp_view + CONTEXT_EOP);
		if (eop != seen) {
			seen = eop;
			start = mach_absolute_time();
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= CONTEXT_STALL_NS) {
			printf("adreno8: ctx %u EOP timeout waiting for %u: last %u, EOP %u\n",
			    s->id, raw_ts, s->last, adreno8_rd(s->timestamp_view + CONTEXT_EOP));
			adreno8_gpu_hang_dump();
			return context_slot_fail(slot, "EOP timeout");
		}
		adreno8_sleep_us(100);
	}
	return true;
}

/* The GMU may advance the queue read index after EOP and GPU idle are already
 * visible (seen on device at test pass 2), so the read index gets a short wait. */
static bool
context_consumed(struct adreno8_context_slot *s, uint32_t slot)
{
	uint64_t start = mach_absolute_time(), elapsed;
	uint32_t read, write;

	for (;;) {
		if (!context_queue_indices(s, slot, &read, &write)) return false;
		if (read == write) return true;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= CONTEXT_CONSUME_NS) {
			printf("adreno8: ctx %u queue read %u, write %u after %llu us\n",
			    s->id, read, write, elapsed / 1000);
			return false;
		}
		adreno8_sleep_us(100);
	}
}

/* How many submissions may be outstanding at once: adreno8_inflight, 1 to CONTEXT_INFLIGHT_MAX. */
uint32_t
adreno8_context_depth(void)
{
	return adreno8_inflight >= 1 && adreno8_inflight <= CONTEXT_INFLIGHT_MAX ? adreno8_inflight : 1;
}

int32_t
adreno8_context_acquire(uint32_t prio, bool allow_high, int pid)
{
	(void)prio;
	if (!context_health())
		return -1;
	if (!adreno8_gpu8_preempt)
		return (int32_t)ADRENO8_CONTEXT_SLOT_HIGH;
	if (allow_high) {
		context_slot_ensure_meta(ADRENO8_CONTEXT_SLOT_HIGH);
		context_slots[ADRENO8_CONTEXT_SLOT_HIGH].in_use = true;
		context_slots[ADRENO8_CONTEXT_SLOT_HIGH].owner_pid = pid;
		return context_slot_health(ADRENO8_CONTEXT_SLOT_HIGH) ?
		    (int32_t)ADRENO8_CONTEXT_SLOT_HIGH : -1;
	}
	int32_t cold = -1;
	for (uint32_t i = 2u; i < ADRENO8_CONTEXT_SLOTS; i++) {
		context_slot_ensure_meta(i);
		struct adreno8_context_slot *s = &context_slots[i];

		if (s->in_use || s->state == CONTEXT_FAILED)
			continue;
		if (s->state == CONTEXT_PREPARED || s->state == CONTEXT_REGISTERED) {
			s->in_use = true;
			s->owner_pid = pid;
			return (int32_t)i;
		}
		if (cold < 0 && s->state == CONTEXT_COLD)
			cold = (int32_t)i;
	}
	if (cold >= 0) {
		context_slots[cold].in_use = true;
		context_slots[cold].owner_pid = pid;
		return cold;
	}
	if (context_slot_health(ADRENO8_CONTEXT_SLOT_NORMAL))
		return (int32_t)ADRENO8_CONTEXT_SLOT_NORMAL;
	return -1;
}

void
adreno8_context_release(uint32_t slot)
{
	if (!adreno8_gpu8_preempt || slot < 2u || slot >= context_slot_count())
		return;
	struct adreno8_context_slot *s = &context_slots[slot];

	if (s->state == CONTEXT_REGISTERED && context_health())
		context_unregister_slot(slot, true);
	s->in_use = false;
	s->owner_pid = 0;
}

bool
adreno8_context_healthy(uint32_t slot)
{
	return context_slot_health(slot);
}

uint32_t
adreno8_context_faults(uint32_t slot)
{
	if (slot >= context_slot_count())
		return 0u;
	return context_slots[slot].fault_count;
}

bool
adreno8_context_record_fault(uint32_t context_id, uint32_t timestamp, uint32_t error)
{
	if (!adreno8_gpu8_preempt)
		return false;
	for (uint32_t i = 0; i < context_slot_count(); i++) {
		context_slot_ensure_meta(i);
		struct adreno8_context_slot *s = &context_slots[i];

		if (s->id != context_id)
			continue;
		if (i == ADRENO8_CONTEXT_SLOT_HIGH)
			return false;
		s->last_fault_ts = timestamp;
		s->last_fault_err = error;
		printf("adreno8: ctx %u fault record ts %u err %#x\n", context_id, timestamp, error);
		context_slot_fail(i, "HFI fault");
		return true;
	}
	return false;
}

uint32_t
adreno8_context_depth_target(uint32_t slot, uint32_t depth)
{
	if (slot >= context_slot_count() || !depth)
		return 0u;
	uint32_t last = context_slots[slot].last;

	if (last < depth)
		return 0u;
	return context_encode_fence(slot, last - depth + 1u);
}

/* Waits until the GMU has read enough of the queue for `words` more; returns the write index. */
static bool
context_room(struct adreno8_context_slot *s, uint32_t slot, uint32_t words, uint32_t *write)
{
	uint64_t start = mach_absolute_time(), elapsed;
	uint32_t read, empty;

	for (;;) {
		if (!context_queue_indices(s, slot, &read, write)) return false;
		empty = *write >= read ? CONTEXT_QUEUE_WORDS - (*write - read) : read - *write;
		if (empty > words) return true;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= CONTEXT_WAIT_NS) return context_slot_fail(slot, "queue full");
		adreno8_sleep_us(100);
	}
}

bool
adreno8_context_wait_idle(const char *what)
{
	if (!context_tested) {
		printf("adreno8: context %s requires the standard context test first\n", what);
		return false;
	}
	if (!context_health()) return false;
	for (uint32_t i = 0; i < context_slot_count(); i++) {
		struct adreno8_context_slot *s = &context_slots[i];

		if (s->state == CONTEXT_COLD || (adreno8_gpu8_preempt && i >= 1u && s->state == CONTEXT_FAILED))
			continue;
		if (s->last && !context_wait_slot(i, s->last)) {
			if (!(adreno8_gpu8_preempt && i >= 1u && s->state == CONTEXT_FAILED))
				return context_fail("external buffer idle");
		}
	}
	if (!adreno8_gpu_wait_idle(what)) return context_fail("external buffer idle");
	return true;
}

/*
 * For a change that needs the pipeline empty and can be put off (the GPU's speed): waits at most
 * limit_us for what is outstanding, and false without closing anything when it is still running.
 */
bool
adreno8_context_drained(const char *what, uint32_t limit_us)
{
	if (!context_tested || !context_health()) return false;
	for (uint32_t waited = 0;; waited += 100) {
		bool all_done = true;

		for (uint32_t i = 0; i < context_slot_count(); i++) {
			struct adreno8_context_slot *s = &context_slots[i];

			if (s->state == CONTEXT_COLD || (adreno8_gpu8_preempt && i >= 1u && s->state == CONTEXT_FAILED))
				continue;
			if (s->last && !adreno8_context_done(context_encode_fence(i, s->last))) {
				all_done = false;
				break;
			}
		}
		if (all_done) break;
		if (context_failed || waited >= limit_us) return false;
		adreno8_sleep_us(100);
	}
	return adreno8_gpu_wait_idle(what) || context_fail("external buffer idle");
}

uint32_t
adreno8_context_submit_slot(uint32_t slot, const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t msg[8 + ADRENO8_CONTEXT_MAX_IBS * 3] = {0};
	uint32_t write, length, aligned, timestamp, depth;

	if (slot >= context_slot_count()) {
		context_fail("slot index");
		return 0;
	}
	struct adreno8_context_slot *s = &context_slots[slot];

	if (!context_slot_health(slot))
		return 0;

	/* Validate before registration or queue publication. IB_PRIV stays clear;
	 * the HFI descriptor encodes each IB length in bytes. */
	if (!addresses || !words || !count || count > ADRENO8_CONTEXT_MAX_IBS) {
		context_slot_fail(slot, "IB count");
		return 0;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!addresses[i] || (addresses[i] & 3) || !words[i] || words[i] > 0xfffff ||
		    addresses[i] >= (1ULL << 32) || addresses[i] + (uint64_t)words[i] * 4 > (1ULL << 32)) {
			context_slot_fail(slot, "IB range");
			return 0;
		}
	}
	/* Up to `depth` submissions are outstanding per slot: Mesa records and submits the next batch
	 * while the GPU executes earlier ones. With `depth` outstanding, the oldest retires first. */
	depth = adreno8_context_depth();
	if (!context_register_slot(slot) || (s->last >= depth && !context_wait_slot(slot, s->last - depth + 1)))
		return 0;
	length = 8 + count * 3;
	aligned = (length + 3) & ~3u;
	if (!context_room(s, slot, aligned, &write)) return 0;
	timestamp = s->last + 1;
	if (!timestamp || (adreno8_gpu8_preempt && timestamp > CONTEXT_FENCE_TS_MASK)) timestamp = 1;
	s->sequence = (s->sequence + 1) & 0xfff;
	msg[0] = 130u | length << 8 | s->sequence << 20;
	msg[1] = s->id;
	msg[2] = CONTEXT_NOTIFY;
	msg[3] = timestamp;
	msg[6] = count;
	for (uint32_t i = 0; i < count; i++) {
		msg[8 + i * 3] = (uint32_t)addresses[i];
		msg[9 + i * 3] = (uint32_t)(addresses[i] >> 32);
		msg[10 + i * 3] = words[i] * 4;
	}
	adreno8_reset_required = true;
	for (uint32_t i = 0; i < aligned; i++) {
		adreno8_wr(s->queue_view + CONTEXT_HEADER_WORDS * 4 + write * 4,
		    i < length ? msg[i] : 0xfafafafa);
		write = (write + 1) % CONTEXT_QUEUE_WORDS;
	}
	adreno8_settle();
	adreno8_wr(s->queue_view + 24, write);
	adreno8_settle();
	s->last = timestamp;
	submit_history[submit_history_next++ % SUBMIT_HISTORY] = (struct submit_record){
		.context_id = s->id,
		.timestamp = timestamp,
		.count = count,
		.ib0_addr = (uint32_t)addresses[0],
		.ib0_words = words[0],
		.ib_last_addr = (uint32_t)addresses[count - 1],
		.pid = proc_selfpid(),
	};
	adreno8_wr(adreno8_gmu.base + GMU_HOST2GMU_SET, s->doorbell_bit);
	return context_encode_fence(slot, timestamp);
}

uint32_t
adreno8_context_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	return adreno8_context_submit_slot(ADRENO8_CONTEXT_SLOT_HIGH, addresses, words, count);
}

uint32_t
adreno8_context_last_slot(uint32_t slot)
{
	if (slot >= context_slot_count() || !context_slots[slot].last) return 0u;
	return context_encode_fence(slot, context_slots[slot].last);
}

uint32_t
adreno8_context_last(void)
{
	return context_slots[0].last;
}

/* The timestamp the GPU last wrote at end of pipe: the submission after it is the one that did not
 * retire. Zero before the context exists. */
uint32_t
adreno8_context_retired(void)
{
	vm_offset_t view = context_slots[0].timestamp_view;

	return view ? adreno8_rd(view + CONTEXT_EOP) : 0;
}

uint32_t
adreno8_context_retired_for(uint32_t timestamp)
{
	uint32_t slot, target, eop;
	vm_offset_t view;

	context_decode_fence(timestamp, &slot, &target);
	view = slot < context_slot_count() ? context_slots[slot].timestamp_view : 0;
	eop = view ? adreno8_rd(view + CONTEXT_EOP) : 0;
	return eop ? context_encode_fence(slot, eop) : 0u;
}

static bool context_inflight_described;

void
adreno8_context_describe_inflight(void)
{
	if (context_inflight_described)
		return;
	context_inflight_described = true;
	for (uint32_t i = 0; i < context_slot_count(); i++) {
		const struct adreno8_context_slot *s = &context_slots[i];

		if (i >= context_boot_slot_count() && s->state == CONTEXT_COLD)
			continue;
		uint32_t sop = s->timestamp_view ? adreno8_rd(s->timestamp_view + CONTEXT_SOP) : 0;
		uint32_t eop = s->timestamp_view ? adreno8_rd(s->timestamp_view + CONTEXT_EOP) : 0;

		if (i == 0 && !adreno8_gpu8_preempt)
			printf("adreno8: context inflight: SOP %u EOP %u last %u\n", sop, eop, s->last);
		else
			printf("adreno8: ctx %u inflight: SOP %u EOP %u last %u\n", s->id, sop, eop, s->last);
	}
	for (uint32_t i = 1; i <= SUBMIT_HISTORY; i++) {
		const struct submit_record *r = &submit_history[(submit_history_next - i) % SUBMIT_HISTORY];

		if (r->timestamp == 0)
			continue;
		printf("adreno8: submit ctx %u ts %u pid %d count %u ib0 %#x+%#x ib_last %#x\n",
		    r->context_id, r->timestamp, r->pid, r->count, r->ib0_addr, r->ib0_words, r->ib_last_addr);
	}
}

bool
adreno8_context_close(void)
{
	if (!context_health()) return false;
	for (uint32_t i = 0; i < context_slot_count(); i++) {
		struct adreno8_context_slot *s = &context_slots[i];

		if (s->state == CONTEXT_COLD || s->state == CONTEXT_PREPARED)
			continue;
		if (adreno8_gpu8_preempt && i >= 1u && s->state == CONTEXT_FAILED)
			continue;
		if (s->state != CONTEXT_REGISTERED)
			return false;
		if (!context_unregister_slot(i, adreno8_gpu8_preempt && i >= 1u))
			return false;
	}
	return true;
}

static bool
context_test_pass(uint32_t slot, uint32_t pass)
{
	struct adreno8_context_slot *s = &context_slots[slot];
	uint32_t expected = 0x22c00000u | pass;
	uint32_t *ib = (uint32_t *)test_ib->memory;
	uint32_t count = adreno8_pm4_scratch(ib, CONTEXT_BYTES / 4, test_result->gpu_address, expected);
	uint64_t address = test_ib->gpu_address;

	if (!count || !adreno8_memory_publish(test_ib, 0, count * 4) ||
	    !adreno8_memory_publish(test_result, 0, CONTEXT_BYTES))
		return context_fail("test publication");
	uint32_t fence = adreno8_context_submit_slot(slot, &address, &count, 1);
	uint32_t dec_slot = 0, raw_ts = 0;
	context_decode_fence(fence, &dec_slot, &raw_ts);
	if (!fence || dec_slot != slot || !context_wait_slot(slot, raw_ts) ||
	    !adreno8_gpu_wait_idle("context test idle") ||
	    !adreno8_memory_acquire(test_result, 0, CONTEXT_BYTES))
		return context_fail("test completion");
	volatile uint32_t *result = (volatile uint32_t *)test_result->memory;
	if (result[0] != expected || adreno8_rd(s->timestamp_view + CONTEXT_SOP) != raw_ts ||
	    adreno8_rd(s->timestamp_view + CONTEXT_EOP) != raw_ts)
		return context_fail("test result or timestamps");
	for (unsigned i = 1; i < CONTEXT_BYTES / 4; i++)
		if (result[i] != 0) return context_fail("test result bounds");
	if (!context_health() || !adreno8_hfi_poll()) return context_fail("test fault check");
	if (!context_consumed(s, slot))
		return context_fail("test queue consumption");
	printf("adreno8: context %u pass %u: SOP/EOP %u result %#x; idle and fault checks passed\n",
	    s->id, pass, raw_ts, result[0]);
	return true;
}

bool
adreno8_context_test(void)
{
	if (test_ib || test_result || !adreno8_context_prepare())
		return context_fail("test prerequisites");
	test_ib = adreno8_gpu_alloc("context test IB", CONTEXT_BYTES, CONTEXT_BYTES,
	    ADRENO8_MEM_READ | ADRENO8_MEM_EXEC);
	test_result = adreno8_gpu_alloc("context test result", CONTEXT_BYTES, CONTEXT_BYTES,
	    ADRENO8_MEM_READ | ADRENO8_MEM_WRITE);
	if (!test_ib || !test_result) return context_fail("test allocation");
	for (uint32_t pass = 1; pass <= 2; pass++) {
		if (!context_test_pass(ADRENO8_CONTEXT_SLOT_HIGH, pass)) return false;
	}
	if (adreno8_gpu8_preempt && !context_test_pass(ADRENO8_CONTEXT_SLOT_NORMAL, 3))
		return false;
	if (!adreno8_context_close()) return false;
	context_tested = true;
	printf("adreno8: standard context reused for non-privileged IBs and unregistered; resources retained until cold reset\n");
	return true;
}

void
adreno8_context_reset_for_recovery(void)
{
	context_failed = false;
	context_inflight_described = false;
	for (uint32_t i = 0; i < context_slot_count(); i++) {
		struct adreno8_context_slot *s = &context_slots[i];

		s->fault_count = 0;
		s->last_fault_ts = 0;
		s->last_fault_err = 0;
		if (s->state == CONTEXT_COLD)
			continue;
		s->state = (s->queue_view && s->timestamp_view) ? CONTEXT_PREPARED : CONTEXT_COLD;
		if (s->state == CONTEXT_COLD)
			continue;
		s->checked = 0;
		if (s->queue_view) {
			adreno8_wr(s->queue_view + 20, 0);
			adreno8_wr(s->queue_view + 24, 0);
		}
		if (s->timestamp_view) {
			adreno8_wr(s->timestamp_view + CONTEXT_SOP, s->last);
			adreno8_wr(s->timestamp_view + CONTEXT_EOP, s->last);
		}
		adreno8_settle();
	}
}

bool
adreno8_context_rebind_for_recovery(void)
{
	for (uint32_t i = 0; i < context_slot_count(); i++) {
		struct adreno8_context_slot *s = &context_slots[i];

		if (i >= context_boot_slot_count() && (!s->in_use || s->state != CONTEXT_PREPARED))
			continue;
		if (!context_register_slot(i))
			return false;
	}
	return true;
}

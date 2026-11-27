/* gen8 HWSCHED context submission; callers serialize all operations. */
#ifndef _DEV_ADRENO8_CONTEXT_PRIVATE_H
#define _DEV_ADRENO8_CONTEXT_PRIVATE_H

#define ADRENO8_CONTEXT_MAX_IBS 82u

/* OOB and command initialization stay owned by adreno8_gpu.c. */
bool adreno8_gpu_commands_ready(void);
bool adreno8_gpu_wait_idle(const char *what);
void adreno8_gpu_oob_release(void);
bool adreno8_gpu_oob_acquire(void);
/* After a reported hang: the RBBM and CP status, and for each pipe the IB levels the CP is in
 * with the command words around its position. Reads only, once per boot; prints one line when GX
 * is off. */
void adreno8_gpu_hang_dump(void);

#define ADRENO8_CONTEXT_SLOTS 2u
#define ADRENO8_CONTEXT_SLOT_HIGH 0u
#define ADRENO8_CONTEXT_SLOT_NORMAL 1u

/* Pages and aliases remain allocated until cold reset, including after close.
 * A confirmed close permits registration again; any failure closes this boot. */
bool adreno8_context_prepare(void);
/* Addresses use the current 32-bit GPU VA space; lengths are dwords. Zero reports
 * failure and is never a fence. */
uint32_t adreno8_context_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
uint32_t adreno8_context_submit_slot(uint32_t slot, const uint64_t *addresses, const uint32_t *words, uint32_t count);
/* Completion requires EOP memory and healthy transport/device state. */
bool adreno8_context_done(uint32_t timestamp);
uint32_t adreno8_context_last(void);
uint32_t adreno8_context_last_slot(uint32_t slot);
uint32_t adreno8_context_retired(void);
uint32_t adreno8_context_retired_for(uint32_t timestamp);
uint32_t adreno8_context_depth(void);
void adreno8_context_describe_inflight(void);
bool adreno8_context_close(void);
bool adreno8_context_test(void);
/* Require the completed stage-22 self-test, latest EOP and GPU idle. The caller
 * keeps submissions serialized until its page-table operation has finished. */
bool adreno8_context_wait_idle(const char *what);
bool adreno8_context_drained(const char *what, uint32_t limit_us);

#endif

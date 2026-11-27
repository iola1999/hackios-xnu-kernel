/* AnnibaleGPU C ABI backed by the SM8750 Adreno 830 driver. */
#include "../adreno8/adreno8_private.h"
#include "../adreno8/adreno8_memory_private.h"
#include "../adreno8/adreno8_secure_private.h"
#include "../adreno8/adreno8_smmu_private.h"
#include "../adreno8/adreno8_bo_private.h"
#include "../adreno8/adreno8_context_private.h"
#include "../adreno8/adreno8_hwsched_private.h"
#include "../adreno8/adreno8_gpu_private.h"
#include "../adreno8/adreno8_zap_private.h"
#include <sys/annibale_input.h>
#ifdef KERNEL
#include <sys/proc.h>
#endif

extern bool AnnibaleGPUStart(void);

uint32_t gpu_va;
uint32_t gpu_log;

bool
adreno8_step_service(void)
{
	if (!annibale_platform_is_annibale() || !adreno8_gpu8_keep || adreno8_gpu8_return) {
		adreno8_say("adreno8: AnnibaleGPU requires the annibale platform, keep=1 and return=0\n");
		return false;
	}
	if (!adreno8_bo_enable()) {
		adreno8_say("adreno8: AnnibaleGPU external BO enable was refused\n");
		return false;
	}
	adreno8_hfi_irq_attach();
	gpu_va = 0; /* The shared address space has no per-client placement or BO-count query. */
	PE_parse_boot_argn("gpu_log", &gpu_log, sizeof(gpu_log));
	if (!AnnibaleGPUStart()) {
		adreno8_say("adreno8: AnnibaleGPU service registration failed\n");
		adreno8_bo_stop();
		return false;
	}
	adreno8_memory_set_recover_armed(adreno8_gpu_recover_enabled);
	if (adreno8_gpu8_ifpc) {
		adreno8_gpu_oob_release();
	}
	adreno8_say("adreno8: AnnibaleGPU registered with A830 BOs and standard HWSCHED submission\n");
	return true;
}

bool
annibale_gpu_ready(void)
{
	return annibale_platform_is_annibale() && adreno8_bo_ready();
}

/* Without the health check annibale_gpu_ready() runs: safe to ask with no lock held. */
bool
annibale_gpu_accepting(void)
{
	return annibale_platform_is_annibale() && adreno8_bo_enabled();
}

bool
annibale_gpu_parameter(uint32_t which, uint64_t *value)
{
	if (!annibale_platform_is_annibale() || !value || !adreno8_bo_ready()) {
		return false;
	}
	switch (which) {
	case 0x01: *value = 830; break;                 /* GPU_ID */
	case 0x02: *value = 0xc00000; break;            /* GMEM_SIZE: vendor gen8_0_1 */
	case 0x03: *value = 0x44050001; break;          /* CHIP_ID: running Android DT */
	case 0x04: *value = adreno8_gx_hz[adreno8_gx_count - 1]; break; /* MAX_FREQ: the fastest speed of the table */
	case 0x06: *value = 0x04000000; break;          /* GMEM_BASE: GPU setup */
	case 0x07: *value = adreno8_gpu8_preempt ? ADRENO8_CONTEXT_SLOTS : 1; break; /* PRIORITIES */
	case 0x08:                                      /* PP_PGTABLE */
	case 0x09:                                      /* FAULTS: unhealthy state is rejected above */
	case 0x0a:                                      /* SUSPENDS: no suspend transition */
	case 0x11: *value = 0; break;                   /* RAYTRACING: unavailable */
	case 0x10: *value = 16; break;                  /* HIGHEST_BANK_BIT: vendor gen8_0_1 */
	case 0x14: *value = 0x1fffffffff000ULL; break;  /* UCHE_TRAP_BASE: GPU setup */
	default: return false;
	}
	return true;
}

uint64_t
annibale_gpu_address(uint64_t size, uint32_t *refusal)
{
	if (!annibale_platform_is_annibale()) {
		*refusal = ADRENO8_BO_REFUSE_NOT_READY;
		return 0;
	}
	return adreno8_bo_reserve(size, refusal);
}

void
annibale_gpu_address_free(uint64_t address, uint64_t size)
{
	if (annibale_platform_is_annibale()) {
		(void)adreno8_bo_release(address, size);
	}
}

void
annibale_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes,
    unsigned *most, uint64_t *lost, uint32_t *refused)
{
	*used = *largest = *lost = 0;
	*live = *holes = *most = *refused = 0;
}

bool
annibale_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return annibale_platform_is_annibale() && adreno8_bo_map(address, phys, read_only);
}

bool
annibale_gpu_mapped(void)
{
	return annibale_platform_is_annibale() && adreno8_bo_commit();
}

bool
annibale_gpu_unmap(uint64_t address, uint64_t size)
{
	return annibale_platform_is_annibale() && adreno8_bo_unmap(address, size);
}

void
annibale_gpu_to_memory(void *start, uint64_t size)
{
	if (annibale_platform_is_annibale()) {
		adreno8_to_memory((vm_offset_t)start, size);
	}
}

static uint32_t
annibale_gpu_caller_slot(void)
{
	char who[24] = "";

	if (!adreno8_gpu8_preempt) {
		return ADRENO8_CONTEXT_SLOT_HIGH;
	}
	proc_selfname(who, sizeof(who));
	return strcmp(who, "backboardd") == 0 ?
	    ADRENO8_CONTEXT_SLOT_HIGH : ADRENO8_CONTEXT_SLOT_NORMAL;
}

uint32_t
annibale_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	if (!annibale_platform_is_annibale() || !adreno8_bo_ready()) {
		return 0;
	}
	adreno8_speed_tick();
	return adreno8_gpu8_preempt ?
	    adreno8_context_submit_slot(annibale_gpu_caller_slot(), addresses, words, count) :
	    adreno8_context_submit(addresses, words, count);
}

bool
annibale_gpu_done(uint32_t fence)
{
	return annibale_platform_is_annibale() && adreno8_bo_ready() && adreno8_context_done(fence);
}

uint32_t
annibale_gpu_irqs(void)
{
	return annibale_platform_is_annibale() ? adreno8_hfi_irq_count() : 0;
}

void
annibale_gpu_pause(uint32_t seen, uint32_t poll_us, uint32_t longest_us)
{
	if (annibale_platform_is_annibale()) {
		adreno8_hfi_irq_pause(seen, poll_us, longest_us);
	} else {
		delay_for_interval(poll_us, NSEC_PER_USEC);
	}
}

uint32_t
annibale_gpu_retired(void)
{
	return annibale_platform_is_annibale() ? adreno8_context_retired() : 0;
}

uint32_t
annibale_gpu_retired_for(uint32_t fence)
{
	return annibale_platform_is_annibale() ? adreno8_context_retired_for(fence) : 0;
}

uint32_t
annibale_gpu_last(void)
{
	if (!annibale_platform_is_annibale()) {
		return 0;
	}
	return adreno8_gpu8_preempt ?
	    adreno8_context_last_slot(annibale_gpu_caller_slot()) :
	    adreno8_context_last();
}

void
annibale_gpu_speed(uint32_t *mhz, uint32_t *changes)
{
	*mhz = *changes = 0;
	if (annibale_platform_is_annibale()) {
		adreno8_speed_state(mhz, changes);
	}
}

void
annibale_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	if (!annibale_platform_is_annibale()) {
		*microseconds = 0;
		*submissions = 0;
		for (unsigned i = 0; i < 7; i++) {
			by[i] = 0;
		}
		return;
	}
	adreno8_hfi_worked(microseconds, submissions, by);
}

void
annibale_gpu_report(const char *why)
{
	if (!annibale_platform_is_annibale()) {
		return;
	}
	adreno8_say("adreno8: AnnibaleGPU %s; last timestamp %u, retired %u; SMMU fault %u\n",
	    why, adreno8_context_last(), adreno8_context_retired(), adreno8_gpu_faulted());
	for (unsigned i = 0; i < 30 && adreno8_hfi_poll(); i++) {
		adreno8_sleep_us(100000);
	}
	adreno8_gpu_hang_dump();
}

static bool
annibale_gpu_recover_abort(bool *terminal)
{
	*terminal = true;
	adreno8_memory_set_recover_armed(false);
	adreno8_bo_stop();
	return false;
}

bool
annibale_gpu_recover(void)
{
	static bool recover_busy, recover_terminal;

	if (!annibale_platform_is_annibale()) {
		return false;
	}
	if (!adreno8_gpu_recover_enabled || recover_busy || recover_terminal) {
		return annibale_gpu_recover_abort(&recover_terminal);
	}
	recover_busy = true;
	if (!adreno8_cx_ready() || adreno8_secure_lost() || !adreno8_zap_ready() ||
	    (adreno8_secure_space.is_on && adreno8_space_faulted(&adreno8_secure_space)) ||
	    (adreno8_gmu_space.is_on && adreno8_space_faulted(&adreno8_gmu_space)) ||
	    (adreno8_gpu_space.is_on &&
	    (!adreno8_space_clear_fault(&adreno8_gpu_space) || !adreno8_space_sync(&adreno8_gpu_space))) ||
	    !adreno8_memory_reset_for_recovery()) {
		recover_busy = false;
		return annibale_gpu_recover_abort(&recover_terminal);
	}
	adreno8_hfi_reset_for_recovery();
	adreno8_hwsched_reset_for_recovery();
	adreno8_gpu_reset_for_recovery();
	adreno8_context_reset_for_recovery();
	adreno8_gmu_reset_for_recovery();
	if (!adreno8_step_firmware() || !adreno8_step_gmu_start() || !adreno8_step_queues() ||
	    !adreno8_step_tables() || !adreno8_hwsched_start() || !adreno8_step_gx_power() ||
	    !adreno8_gpu_memory_verify() || !adreno8_gpu_cp_init() ||
	    !adreno8_gpu_secure_exit() || !adreno8_gpu_scratch_test() ||
	    !adreno8_context_rebind_for_recovery() || !adreno8_bo_reset_for_recovery()) {
		recover_busy = false;
		return annibale_gpu_recover_abort(&recover_terminal);
	}
	adreno8_hfi_irq_resume();
	recover_busy = false;
	adreno8_say("adreno8: GPU recovery completed; submissions resumed\n");
	return true;
}

void
annibale_gpu_stop(void)
{
	if (!annibale_platform_is_annibale()) {
		return;
	}
	if (adreno8_bo_ready()) {
		(void)adreno8_context_close();
	}
	adreno8_bo_stop();
}

/* External physical BO pages in the existing non-secure GPU address space. */
#ifndef _DEV_ADRENO8_BO_PRIVATE_H
#define _DEV_ADRENO8_BO_PRIVATE_H
#include "adreno8_memory_private.h"

/* Stage 23 explicitly enables this path after the standard context self-test.
 * The caller serializes these operations with every GPU submission. */
bool adreno8_bo_enable(void);
bool adreno8_bo_ready(void);
/* bo_state alone, without the health check adreno8_bo_ready() runs: monotonic once the GPU is
 * gone, so a lock-free caller may ask it as a first refusal (AnnibaleGPU's ioctl entry check);
 * the hardware check stays inside the lock. */
bool adreno8_bo_enabled(void);
/* Close all BO admission without releasing pages or IOVAs. */
void adreno8_bo_stop(void);
bool adreno8_bo_reset_for_recovery(void);
/* Why a reservation was refused, written through adreno8_bo_reserve's `refusal` when it returns
 * 0, so the user client can answer with a return code that says which shortage it was instead of
 * one kIOReturnNoMemory for all of them. iokit/Kernel/AnnibaleGPU.cpp repeats these values (the
 * headers of the two sides are not shared). */
#define ADRENO8_BO_REFUSE_NOT_READY    1   /* the health check inside the reservation refused: the GPU is going or gone */
#define ADRENO8_BO_REFUSE_NO_RANGE     2   /* no free span in the GPU's address space */
#define ADRENO8_BO_REFUSE_NO_TABLE     3   /* no page table for the span: the table slots or the memory for one */
uint64_t adreno8_bo_reserve(uint64_t size, uint32_t *refusal);
/* The existing GEM interface identifies only read-only versus writable BOs.
 * Mesa uses it for shader, IB and data pages; all retain READ/EXEC permission. */
bool adreno8_bo_map(uint64_t address, uint64_t phys, bool read_only);
bool adreno8_bo_commit(void);
/* Use the address returned by reserve and the original requested size. Unmap confirms latest
 * EOP, GPU idle and TLB completion; only then may the caller release its pages.
 * Release returns the unmapped IOVA to the same page-table occupancy record.
 * Failures after enable close subsequent BO operations; any existing reservation,
 * mapping and backing pages remain owned until cold reset. */
bool adreno8_bo_unmap(uint64_t address, uint64_t size);
bool adreno8_bo_release(uint64_t address, uint64_t size);
/* Print `count` dwords from GPU address `address` through the CB0 page table, for a hang report.
 * Pages that are not mapped are named, not read. */
void adreno8_bo_dump_words(const char *what, uint64_t address, uint32_t count);
/* Print the command buffer at `base`, `count` dwords, and the IBs and draw-state groups it names,
 * three levels down; then the fragment stage's bindless descriptors in Mesa's fb-read slots. Once
 * per boot, for a hang report. */
void adreno8_bo_dump_tree(uint64_t base, uint32_t count);
/* After an SMMU fault in the GPU space, once per boot: the BOs that held the faulting address
 * in the last reservations, live or released, and the nearest live ones below and above it. */
void adreno8_bo_describe_fault(uint32_t fault);
/* Look up the owning BO (live or recently released) for a GPU VA such as CP IB1/IB2. */
void adreno8_bo_describe_owner(const char *what, uint32_t address);

#endif

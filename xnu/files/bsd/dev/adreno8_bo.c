/* External BO ownership stays in the caller; CB0 PTEs record IOVA ownership. */
#include "adreno8_bo_private.h"
#include "adreno8_smmu_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_context_private.h"

#define printf adreno8_say

static enum { BO_COLD, BO_ENABLED, BO_FAILED } bo_state;

void
adreno8_bo_stop(void)
{
	bo_state = BO_FAILED;
	adreno8_reset_required = true;
	adreno8_hwsched_memory_failed();
}

static bool
bo_fail(const char *what)
{
	adreno8_bo_stop();
	printf("adreno8: external BO %s failed; submissions closed and pages retained until cold reset\n", what);
	return false;
}

/*
 * The full health check reads some twenty GPU, GMU and SMMU registers and drains the HFI queues;
 * the dispatch layer asks several times per submission and on every poll of a wait. Within
 * BO_CHECK_NS of a check that passed, the state alone answers, so a fault is noticed by the
 * next check at most that much later. Completion polling drains the HFI queues on its own.
 */
#define BO_CHECK_NS 1000000ull              /* 1 ms */
static uint64_t bo_checked;

bool
adreno8_bo_ready(void)
{
	uint64_t now = mach_absolute_time(), since;

	if (bo_state != BO_ENABLED) return false;
	absolutetime_to_nanoseconds(now - bo_checked, &since);
	if (bo_checked != 0 && since < BO_CHECK_NS) return true;
	if (!adreno8_gpu_commands_ready() || !adreno8_gpu_memory_ready() ||
	    !adreno8_hwsched_ready() || !adreno8_hfi_poll() || adreno8_gpu_faulted())
		return bo_fail("health check");
	bo_checked = now;
	return true;
}

bool
adreno8_bo_enable(void)
{
	if (bo_state == BO_FAILED) return false;
	if (bo_state == BO_ENABLED) return adreno8_bo_ready();
	if (!adreno8_context_wait_idle("external BO enable")) return false;
	if (!adreno8_gpu_memory_verify() || !adreno8_hfi_poll()) return false;
	bo_state = BO_ENABLED;
	return adreno8_bo_ready();
}

static bool
bo_range(uint64_t address, uint64_t size)
{
	return size != 0 && ((address | size) & SMMU_GRANULE_MASK) == 0 &&
	    address >= ADRENO8_GPU_IOVA_START && address < ADRENO8_GPU_IOVA_END &&
	    size <= ADRENO8_GPU_IOVA_END - address;
}

/* Every page below this address was in use when last looked at: a search for room starts here. */
static uint32_t bo_free_from = ADRENO8_GPU_IOVA_START;

uint64_t
adreno8_bo_reserve(uint64_t size)
{
	uint32_t address, from = bo_free_from;
	if (!adreno8_bo_ready()) return 0;
	if (!bo_range(ADRENO8_GPU_IOVA_START, size) ||
	    (!adreno8_space_find_range(&adreno8_gpu_space, from, ADRENO8_GPU_IOVA_END, (uint32_t)size, SMMU_GRANULE, &address) &&
	    (from == ADRENO8_GPU_IOVA_START || !adreno8_space_find_range(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START,
	    ADRENO8_GPU_IOVA_END, (uint32_t)size, SMMU_GRANULE, &address)))) {
		bo_fail("IOVA reservation");
		return 0;
	}
	/* Populate tables before marking any leaf. A partial table allocation is
	 * retained on failure and no physical BO page has been exposed yet. */
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE) {
		if (!adreno8_space_prepare_page(&adreno8_gpu_space, address + at)) {
			bo_fail("reservation tables");
			return 0;
		}
	}
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE)
		*adreno8_space_entry(&adreno8_gpu_space, address + at) = ADRENO8_PTE_EXTERNAL;
	/* (the pages it passes over stay in use until a release brings it back below them) */
	for (uint64_t *entry; bo_free_from < ADRENO8_GPU_IOVA_END &&
	    (entry = adreno8_space_entry(&adreno8_gpu_space, bo_free_from)) != NULL && *entry != 0;
	    bo_free_from += SMMU_GRANULE) {
	}
	return address;
}

/* The pages entered since the last commit, which cleans just their entries out of the cache. */
static uint64_t bo_mapped_low = UINT64_MAX, bo_mapped_high;

/* One page of a buffer: the GPU's health is checked at its reservation and at its commit. */
bool
adreno8_bo_map(uint64_t address, uint64_t phys, bool read_only)
{
	if (bo_state != BO_ENABLED) return false;
	uint64_t *entry = bo_range(address, SMMU_GRANULE) ?
	    adreno8_space_entry(&adreno8_gpu_space, (uint32_t)address) : NULL;
	if (!entry || *entry != ADRENO8_PTE_EXTERNAL || !phys ||
	    (phys & SMMU_GRANULE_MASK) || phys >= SMMU_PHYS_LIMIT)
		return bo_fail("physical page");
	uint32_t prot = ADRENO8_MEM_READ | ADRENO8_MEM_EXEC | (read_only ? 0 : ADRENO8_MEM_WRITE);
	*entry = phys | adreno8_space_attributes(prot) | ADRENO8_PTE_EXTERNAL;
	if (address < bo_mapped_low) bo_mapped_low = address;
	if (address + SMMU_GRANULE > bo_mapped_high) bo_mapped_high = address + SMMU_GRANULE;
	return true;
}

bool
adreno8_bo_commit(void)
{
	uint64_t low = bo_mapped_low, high = bo_mapped_high;
	if (!adreno8_bo_ready()) return false;
	bo_mapped_low = UINT64_MAX;
	bo_mapped_high = 0;
	return adreno8_space_publish_range(&adreno8_gpu_space, low < high ? low : 0, low < high ? high - low : 0) ||
	    bo_fail("map TLB synchronization");
}

static bool
bo_owned(uint64_t pte)
{
	if (pte == ADRENO8_PTE_EXTERNAL) return true;
	uint64_t phys = pte & PTE_ADDRESS;
	uint64_t attributes = pte & ~(PTE_ADDRESS | ADRENO8_PTE_EXTERNAL);
	uint32_t prot = ADRENO8_MEM_READ | ADRENO8_MEM_EXEC;
	return (pte & ADRENO8_PTE_EXTERNAL) && phys && phys < SMMU_PHYS_LIMIT &&
	    (attributes == adreno8_space_attributes(prot) ||
	    attributes == adreno8_space_attributes(prot | ADRENO8_MEM_WRITE));
}

/* Each unmap waits for the GPU to be idle: how often, and how long, every 5 s while it goes on. */
static struct {
	uint32_t count;
	uint64_t since, waiting;
} unmap_cost;

static void
unmap_note(uint64_t began, uint64_t ended)
{
	uint64_t span, waiting;

	unmap_cost.count++;
	unmap_cost.waiting += ended - began;
	if (unmap_cost.since == 0) unmap_cost.since = began;
	absolutetime_to_nanoseconds(ended - unmap_cost.since, &span);
	if (span < 5 * NSEC_PER_SEC) return;
	absolutetime_to_nanoseconds(unmap_cost.waiting, &waiting);
	printf("adreno8: %u external buffers unmapped in %llu ms, %llu us waiting for the GPU to be idle in all\n",
	    unmap_cost.count, span / NSEC_PER_MSEC, waiting / 1000);
	memset(&unmap_cost, 0, sizeof(unmap_cost));
}

bool
adreno8_bo_unmap(uint64_t address, uint64_t size)
{
	if (!adreno8_bo_ready()) return false;
	if (!bo_range(address, size)) return bo_fail("unmap range");
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE) {
		uint64_t *entry = adreno8_space_entry(&adreno8_gpu_space, (uint32_t)(address + at));
		if (!entry || !bo_owned(*entry)) return bo_fail("unmap ownership");
	}
	uint64_t began = mach_absolute_time();
	if (!adreno8_context_wait_idle("external BO unmap")) return bo_fail("unmap idle");
	unmap_note(began, mach_absolute_time());
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE)
		*adreno8_space_entry(&adreno8_gpu_space, (uint32_t)(address + at)) = ADRENO8_PTE_EXTERNAL;
	return adreno8_space_publish_range(&adreno8_gpu_space, address, size) || bo_fail("unmap TLB synchronization");
}

extern unsigned int ml_phys_read_word(vm_offset_t paddr);

/* The dword at GPU address `at`, read through the CB0 page table; false for an unmapped page. */
static bool
bo_word(uint64_t at, uint32_t *word)
{
	uint64_t *entry = at < ADRENO8_GPU_IOVA_END ?
	    adreno8_space_entry(&adreno8_gpu_space, (uint32_t)(at & ~(uint64_t)SMMU_GRANULE_MASK)) : NULL;

	if (!entry || !(*entry & 1))
		return false;
	*word = ml_phys_read_word((vm_offset_t)((*entry & PTE_ADDRESS) | (at & SMMU_GRANULE_MASK)));
	return true;
}

void
adreno8_bo_dump_words(const char *what, uint64_t address, uint32_t count)
{
	address &= ~3ull;
	for (uint32_t i = 0; i < count; i += 8) {
		char line[160];
		int used = snprintf(line, sizeof(line), "adreno8: %s %#llx:", what, address + 4ull * i);

		for (uint32_t j = i; j < count && j < i + 8 && used < (int)sizeof(line) - 10; j++) {
			uint32_t word;

			used += bo_word(address + 4ull * j, &word) ?
			    snprintf(line + used, sizeof(line) - used, " %08x", word) :
			    snprintf(line + used, sizeof(line) - used, " unmapped");
		}
		printf("%s\n", line);
	}
}

/* Packet opcodes and registers the walk follows (Mesa adreno_pm4.xml and a6xx.xml, A8XX). */
#define PM4_INDIRECT_BUFFER 0x3f
#define PM4_INDIRECT_BUFFER_CHAIN 0x57
#define PM4_SET_DRAW_STATE 0x43
#define PM4_CONTEXT_REG_BUNCH 0x5c
#define DRAW_STATE_DISABLE (1u << 17)
/* SP_GFX_BINDLESS_BASE[4].DESCRIPTOR, the fragment stage's set; bits 0-1 are the descriptor size. */
#define BINDLESS_BASE_FS 0xab12
/* Mesa's fb-read descriptors: IR3_BINDLESS_DESC_COUNT - 1 - A6XX_MAX_RENDER_TARGETS, 16 dwords each. */
#define FB_READ_FIRST_SLOT 55
#define FB_READ_SLOTS 4
#define TREE_BUFFERS 96
#define TREE_WORDS 12288
#define TREE_DEPTH 3

struct bo_tree {
	uint64_t seen[TREE_BUFFERS];
	unsigned buffers;
	uint32_t words;
	uint64_t bindless_fs;
};

static void
bo_tree_reg(struct bo_tree *tree, uint32_t reg, uint32_t value)
{
	if (reg == BINDLESS_BASE_FS)
		tree->bindless_fs = (tree->bindless_fs & ~0xffffffffull) | (value & ~3u);
	else if (reg == BINDLESS_BASE_FS + 1)
		tree->bindless_fs = (tree->bindless_fs & 0xffffffffull) | (uint64_t)value << 32;
}

static void
bo_tree_walk(struct bo_tree *tree, uint64_t base, uint32_t count, unsigned depth)
{
	for (unsigned i = 0; i < tree->buffers; i++)
		if (tree->seen[i] == base)
			return;
	if (tree->buffers == TREE_BUFFERS || tree->words + count > TREE_WORDS) {
		printf("adreno8: tree %#llx (%u words) not printed: over the dump's size\n", base, count);
		return;
	}
	tree->seen[tree->buffers++] = base;
	tree->words += count;

	char what[32];
	snprintf(what, sizeof(what), "tree %u of %u", depth, count);
	adreno8_bo_dump_words(what, base, count);

	for (uint32_t at = 0; at < count;) {
		uint32_t header, size, a, b, c;

		if (!bo_word(base + 4ull * at, &header))
			return;
		if (header >> 28 == 7)
			size = header & 0x7fff;
		else if (header >> 28 == 4)
			size = header & 0x7f;
		else
			return;

		uint32_t opcode = (header >> 16) & 0x7f;
		uint64_t body = base + 4ull * (at + 1);
		if (header >> 28 == 4) {
			for (uint32_t i = 0; i < size && bo_word(body + 4ull * i, &a); i++)
				bo_tree_reg(tree, ((header >> 8) & 0x3ffff) + i, a);
		} else if (opcode == PM4_CONTEXT_REG_BUNCH) {
			for (uint32_t i = 0; i + 1 < size && bo_word(body + 4ull * i, &a) &&
			     bo_word(body + 4ull * (i + 1), &b); i += 2)
				bo_tree_reg(tree, a, b);
		} else if ((opcode == PM4_INDIRECT_BUFFER || opcode == PM4_INDIRECT_BUFFER_CHAIN) &&
		    size >= 3 && depth < TREE_DEPTH && bo_word(body, &a) && bo_word(body + 4, &b) &&
		    bo_word(body + 8, &c)) {
			bo_tree_walk(tree, a | (uint64_t)b << 32, c & 0xfffff, depth + 1);
		} else if (opcode == PM4_SET_DRAW_STATE && depth < TREE_DEPTH) {
			for (uint32_t i = 0; i + 3 <= size && bo_word(body + 4ull * i, &a) &&
			     bo_word(body + 4ull * (i + 1), &b) && bo_word(body + 4ull * (i + 2), &c); i += 3)
				if ((a & 0xffff) && !(a & DRAW_STATE_DISABLE))
					bo_tree_walk(tree, b | (uint64_t)c << 32, a & 0xffff, depth + 1);
		}
		at += size + 1;
	}
}

void
adreno8_bo_dump_tree(uint64_t base, uint32_t count)
{
	static struct bo_tree tree;

	bo_tree_walk(&tree, base, count, 0);
	if (!tree.bindless_fs) {
		printf("adreno8: tree: no write to SP_GFX_BINDLESS_BASE[4]\n");
		return;
	}
	for (unsigned slot = FB_READ_FIRST_SLOT; slot < FB_READ_FIRST_SLOT + FB_READ_SLOTS; slot++) {
		char what[48];

		snprintf(what, sizeof(what), "fs bindless slot %u", slot);
		adreno8_bo_dump_words(what, tree.bindless_fs + 64ull * slot, 16);
	}
}

bool
adreno8_bo_release(uint64_t address, uint64_t size)
{
	if (!adreno8_bo_ready()) return false;
	if (!bo_range(address, size)) return bo_fail("release range");
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE) {
		uint64_t *entry = adreno8_space_entry(&adreno8_gpu_space, (uint32_t)(address + at));
		if (!entry || *entry != ADRENO8_PTE_EXTERNAL) return bo_fail("release ownership");
	}
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE)
		*adreno8_space_entry(&adreno8_gpu_space, (uint32_t)(address + at)) = 0;
	if (address < bo_free_from) bo_free_from = (uint32_t)address;
	return true;
}

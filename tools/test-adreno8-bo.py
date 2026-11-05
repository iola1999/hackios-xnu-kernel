#!/usr/bin/env python3
"""Exercise external BO ownership with the production CB0 mapper under ASan/UBSan.

Each mode runs in its own process: the guard page after each BO, the search that starts after the
last reservation and wraps around, refusals that refuse one buffer, the failures that close every
BO operation, and the ownership checks. Each mutation of adreno8_bo.c below has to fail a mode.
Dropped: the unmap that waited for EOP and GPU idle, since RedwoodGPUMemory.inc's freeBuffer() now
unmaps only after the last submission retired (tools/test-redwood-gpu-lifetime.py checks that).
Dropped: the refill of a released hole from the lowest free page, replaced by the next-fit cursor.
"""
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEV = ROOT / "xnu/files/bsd/dev"

FENCES = r'''
/* adreno8_bo.c's clock: each reading BO_CHECK_NS or more after the last unless a case stops it */
static uint64_t bo_clock, bo_clock_step = 1ull << 40;
static uint64_t bo_test_clock(void) { return bo_clock += bo_clock_step; }
static bool commands_verified, context_verified, memory_healthy, hfi_healthy;
static bool adreno8_reset_required;
static int proc_selfpid(void) { return 1; }
/* Only the hang-report dumps read BO words; no case reaches them. */
unsigned int ml_phys_read_word(vm_offset_t paddr) { assert(false); return 0; }
static unsigned closed, idle_calls;
bool adreno8_gpu_commands_ready(void) { return commands_verified && memory_healthy; }
bool adreno8_gpu_memory_ready(void) { return adreno8_gpu_space.is_on && memory_healthy; }
bool adreno8_gpu_memory_verify(void) {
    return memory_healthy && adreno8_space_check(&adreno8_gpu_space, 0, 0);
}
bool adreno8_gpu_faulted(void) { return !memory_healthy || adreno8_space_faulted(&adreno8_gpu_space); }
bool adreno8_hwsched_ready(void) { return memory_healthy && hfi_healthy; }
bool adreno8_hfi_poll(void) { return hfi_healthy; }
void adreno8_hwsched_memory_failed(void) { closed++; memory_healthy = false; }
bool adreno8_context_wait_idle(const char *why) {
    assert(why != NULL);
    if (!context_verified || !memory_healthy) return false;
    idle_calls++;
    return true;
}
'''

CASES = r'''
enum { NORMAL, BAD_SIZE_0, BAD_SIZE_1, BAD_SIZE_4095, BAD_SIZE_MAX, BAD_SIZE_4G, TABLE_FAIL,
       TWO_LEAVES, PA_ZERO, PA_UNALIGNED, PA_LIMIT, PA_MAX, UNMAP_FOREIGN, RELEASE_FOREIGN, DOUBLE_MAP,
       COMMIT_SYNC, RELEASE_MAPPED, HEALTH, UNMAP_SYNC, CORRUPT_PTE, GMU_TAG, SECURE_TAG,
       REVERSE_MAP, CHECK_WINDOW, GUARD_ALLOC, GUARD_CHANGED, COHERENT, CURSOR, MODES };
static vm_offset_t startup_memory;
static uint64_t startup_pte;
static void fresh(void) {
    reset(&adreno8_gpu_space, false);
    adreno8_gpu_space.context = 0; adreno8_gpu_space.name = "gpu";
    memset(&adreno8_gmu_space, 0, sizeof(adreno8_gmu_space));
    adreno8_gmu_space.context = GMU_CONTEXT;
    memset(&adreno8_secure_space, 0, sizeof(adreno8_secure_space));
    adreno8_secure_space.context = SECURE_CONTEXT;
    bo_state = BO_COLD; bo_checked = 0; bo_clock_step = 1ull << 40;
    commands_verified = context_verified = memory_healthy = hfi_healthy = true;
    adreno8_reset_required = true; /* Secure transfer retains pages through the normal boot. */
    closed = idle_calls = 0;
    assert(kmem_alloc(kernel_map, &startup_memory, PAGE_SIZE, 0, 0) == KERN_SUCCESS);
    assert(adreno8_space_new_table(&adreno8_gpu_space, 0, 0));
    assert(adreno8_space_map_region(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START,
        startup_memory, PAGE_SIZE, ADRENO8_MEM_READ | ADRENO8_MEM_EXEC));
    startup_pte = *adreno8_space_entry(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START);
    assert(adreno8_space_on(&adreno8_gpu_space, 0, 0));
}
/* What every guard entry holds: the zeroed guard page, read-only and not executable, tagged external. */
static uint64_t guard_pte(void) {
    uint64_t pte = kvtophys(bo_guard) | adreno8_space_attributes(ADRENO8_MEM_READ) | ADRENO8_PTE_EXTERNAL;
    assert((pte & (1ULL << 7)) && (pte & SMMU_PTE_XN) == SMMU_PTE_XN);
    return pte;
}
/* Bytes cleaned since event `from`, each event less than a table; whether one held `value` at `entry`. */
static uint64_t cleaned_since(unsigned from, const uint64_t *entry, uint64_t value, bool *seen) {
    uint64_t bytes = 0;
    vm_offset_t at = (vm_offset_t)entry;
    *seen = false;
    for (unsigned i = from; i < clean_count; i++) {
        assert(cleaned[i].size < 4096);
        bytes += cleaned[i].size;
        if (cleaned[i].start <= at && at + 8 <= cleaned[i].start + cleaned[i].snap_len &&
            !memcmp(cleaned[i].snap + (at - cleaned[i].start), &value, 8)) *seen = true;
    }
    return bytes;
}
static void stays_closed(uint64_t address, uint64_t size) {
    unsigned old_writes = writes, old_allocations = allocation_count;
    assert(!adreno8_bo_ready() && closed && adreno8_reset_required);
    assert(!adreno8_bo_map(address, 0x50000000, false));
    assert(!adreno8_bo_commit() && !adreno8_bo_unmap(address, size));
    assert(!adreno8_bo_release(address, size) && !adreno8_bo_reserve(4096));
    sync_ok = hfi_healthy = memory_healthy = true;
    assert(!adreno8_bo_enable());
    assert(writes == old_writes && allocation_count == old_allocations);
}
int main(int argc, char **argv) {
    unsigned mode = argc > 1 ? (unsigned)atoi(argv[1]) : NORMAL;
    assert(mode < MODES);
    fresh();
    unsigned old_writes = writes, old_allocations = allocation_count;
    assert(!adreno8_bo_ready() && !adreno8_bo_map(ADRENO8_GPU_IOVA_START, 0x50000000, false));
    context_verified = false;
    assert(!adreno8_bo_enable() && !closed && !idle_calls);
    assert(writes == old_writes && allocation_count == old_allocations);
    context_verified = true;
    if (mode == GUARD_ALLOC) {
        /* without the guard page no BO may be placed */
        alloc_fail_after = 0;
        assert(!adreno8_bo_enable() && closed && !bo_guard);
        alloc_fail_after = -1;
        stays_closed(ADRENO8_GPU_IOVA_START + PAGE_SIZE, 4096); release_all(); return 0;
    }
    unsigned old_clean = clean_count;
    assert(adreno8_bo_enable() && adreno8_bo_ready() && idle_calls == 1);
    /* one zeroed guard page for every BO, cleaned to memory once; a second enable makes no other */
    assert(bo_guard && allocation_count == old_allocations + 1 && clean_count == old_clean + 1);
    assert(cleaned[old_clean].start == bo_guard && cleaned[old_clean].size == PAGE_SIZE);
    for (unsigned i = 0; i < PAGE_SIZE; i++) assert(((const uint8_t *)bo_guard)[i] == 0);
    assert(adreno8_bo_enable() && allocation_count == old_allocations + 1 && idle_calls == 1);
    if (mode == CHECK_WINDOW) {
        /* within BO_CHECK_NS of a check that passed the state answers; after it the check runs again */
        bo_clock_step = 0;
        assert(adreno8_bo_ready());
        hfi_healthy = false;
        assert(adreno8_bo_ready() && !closed);
        bo_clock_step = 1ull << 40;
        assert(!adreno8_bo_ready() && closed);
        stays_closed(ADRENO8_GPU_IOVA_START, 4096); release_all(); return 0;
    }
    if (mode >= BAD_SIZE_0 && mode <= BAD_SIZE_4G) {
        const uint64_t bad_sizes[] = {0, 1, 4095, UINT64_MAX, (1ULL << 32)};
        unsigned allocations = allocation_count;
        assert(!adreno8_bo_reserve(bad_sizes[mode - BAD_SIZE_0]));
        assert(allocation_count == allocations && adreno8_bo_ready() && !closed);
        assert(*adreno8_space_entry(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START) == startup_pte);
        assert(*adreno8_space_entry(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START + PAGE_SIZE) == 0);
        /* only this request is refused: the next one gets the first room */
        assert(adreno8_bo_reserve(4096) == ADRENO8_GPU_IOVA_START + PAGE_SIZE);
        release_all(); return 0;
    }
    if (mode == TABLE_FAIL) {
        /* 5 MB from the first free page needs two new leaf tables; the second cannot be had */
        unsigned tables = adreno8_gpu_space.table_count;
        alloc_fail_after = 1;
        assert(!adreno8_bo_reserve(5u << 20) && adreno8_bo_ready() && !closed);
        /* the table made before the failure stays, and no leaf of this buffer was marked */
        assert(adreno8_gpu_space.table_count == tables + 1);
        assert(*adreno8_space_entry(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START + PAGE_SIZE) == 0);
        alloc_fail_after = -1;
        assert(adreno8_bo_reserve(5u << 20) == ADRENO8_GPU_IOVA_START + PAGE_SIZE);
        assert(adreno8_gpu_space.table_count == tables + 2 && adreno8_bo_ready() && !closed);
        release_all(); return 0;
    }
    if (mode == CURSOR) {
        uint64_t a = adreno8_bo_reserve(4096), b = adreno8_bo_reserve(4096);
        assert(a == ADRENO8_GPU_IOVA_START + PAGE_SIZE && b == a + 8192);
        assert(adreno8_bo_release(a, 4096));
        /* the search starts after the last reservation, not in the room just given back */
        uint64_t c = adreno8_bo_reserve(4096);
        assert(c == b + 8192);
        /* one that ends past BO_WINDOW sends the next search back to the start, where it finds that room */
        assert(adreno8_bo_reserve(BO_WINDOW) == c + 8192);
        assert(adreno8_bo_reserve(4096) == a && adreno8_bo_ready() && !closed);
        release_all(); return 0;
    }
    adreno8_gpu_coherent = mode == COHERENT;
    uint64_t size = mode == TWO_LEAVES ? (2u << 20) + 4096 : 3 * 4096;
    uint64_t address = adreno8_bo_reserve(size);
    assert(address == ADRENO8_GPU_IOVA_START + PAGE_SIZE);
    /* each reservation is followed by its guard page, and the next one starts after that */
    uint64_t next = adreno8_bo_reserve(4096);
    assert(next == address + size + 4096);
    uint32_t unused;
    assert(adreno8_space_find_range(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START,
        ADRENO8_GPU_IOVA_END, 4096, 4096, &unused) && unused == next + 8192);
    for (uint64_t at = 0; at < size; at += 4096)
        assert(*adreno8_space_entry(&adreno8_gpu_space, address + at) == ADRENO8_PTE_EXTERNAL);
    uint64_t *guard = adreno8_space_entry(&adreno8_gpu_space, address + size);
    assert(*guard == guard_pte() && *adreno8_space_entry(&adreno8_gpu_space, next + 4096) == guard_pte());
    assert(adreno8_space_check(&adreno8_gpu_space, 0, 0));
    uint64_t *first = adreno8_space_entry(&adreno8_gpu_space, address);
    if (mode >= PA_ZERO && mode <= PA_MAX) {
        const uint64_t bad_pa[] = {0, 0x50000001, 1ULL << 36, UINT64_MAX};
        assert(!adreno8_bo_map(address, bad_pa[mode - PA_ZERO], false));
        assert(*first == ADRENO8_PTE_EXTERNAL);
        stays_closed(address, size); release_all(); return 0;
    }
    if (mode == UNMAP_FOREIGN || mode == RELEASE_FOREIGN) {
        bool result = mode == UNMAP_FOREIGN ? adreno8_bo_unmap(ADRENO8_GPU_IOVA_START, PAGE_SIZE) :
            adreno8_bo_release(ADRENO8_GPU_IOVA_START, PAGE_SIZE);
        assert(!result && *adreno8_space_entry(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START) == startup_pte);
        stays_closed(address, size); release_all(); return 0;
    }
    /* (REVERSE_MAP enters the pages in the other order: the commit's range takes both either way) */
    if (mode == REVERSE_MAP) assert(adreno8_bo_map(address + 4096, 0x60000000, true));
    assert(adreno8_bo_map(address, 0x50000000, false));
    if (mode != REVERSE_MAP) assert(adreno8_bo_map(address + 4096, 0x60000000, true));
    /* gpu_coherent=1: user pages cacheable and inner-shareable */
    uint64_t coherent = mode == COHERENT ? 1ULL << 2 | 3ULL << 8 : 0;
    assert((*first & ~PTE_ADDRESS) == (ADRENO8_PTE_EXTERNAL | adreno8_space_attributes(7) | coherent));
    assert((*adreno8_space_entry(&adreno8_gpu_space, address + 4096) & ~PTE_ADDRESS) ==
        (ADRENO8_PTE_EXTERNAL | adreno8_space_attributes(5) | coherent));
    uint64_t mapped = *first;
    if (mode == DOUBLE_MAP) {
        assert(!adreno8_bo_map(address, 0x70000000, false) && *first == mapped);
        stays_closed(address, size); release_all(); return 0;
    }
    if (mode == COMMIT_SYNC) {
        sync_ok = false;
        assert(!adreno8_bo_commit() && *first == mapped && adreno8_gpu_space.is_on);
        stays_closed(address, size); release_all(); return 0;
    }
    unsigned before_commit = clean_count;
    bool seen, guard_seen;
    assert(adreno8_bo_commit());
    /* the commit cleans the entries from the first page entered to the last guard page, not the tables */
    assert(cleaned_since(before_commit, first, mapped, &seen) == (next + 8192 - address) / 4096 * sizeof(uint64_t));
    /* (TWO_LEAVES's first event is too long for the fixture's snapshot) */
    assert(seen || mode == TWO_LEAVES);
    cleaned_since(before_commit, guard, guard_pte(), &guard_seen);
    assert(guard_seen);
    assert(adreno8_space_check(&adreno8_gpu_space, 0, 0));
    if (mode == RELEASE_MAPPED) {
        assert(!adreno8_bo_release(address, size) && *first == mapped);
        stays_closed(address, size); release_all(); return 0;
    }
    if (mode == HEALTH) {
        hfi_healthy = false;
        assert(!adreno8_bo_ready() && *first == mapped);
        stays_closed(address, size); release_all(); return 0;
    }
    if (mode == UNMAP_SYNC) {
        sync_ok = false;
        assert(!adreno8_bo_unmap(address, size) && *first == ADRENO8_PTE_EXTERNAL);
        assert(adreno8_gpu_space.is_on);
        stays_closed(address, size); release_all(); return 0;
    }
    if (mode == CORRUPT_PTE) {
        *adreno8_space_entry(&adreno8_gpu_space, address + 8192) |= 4;
        assert(!adreno8_space_check(&adreno8_gpu_space, 0, 0));
        assert(!adreno8_bo_unmap(address, size) && *first == mapped);
        stays_closed(address, size); release_all(); return 0;
    }
    if (mode == GMU_TAG || mode == SECURE_TAG) {
        struct adreno8_gpu_space *other = mode == GMU_TAG ? &adreno8_gmu_space : &adreno8_secure_space;
        map_one(other);
        uint64_t *entry = adreno8_space_entry(other, 0x60001000);
        uint64_t tagged = *entry | ADRENO8_PTE_EXTERNAL;
        *entry = tagged;
        assert(!adreno8_space_verify(other, false));
        *entry = ADRENO8_PTE_EXTERNAL;
        assert(!adreno8_space_verify(other, false));
        other->context = 0;
        assert(!adreno8_space_verify(other, false));
        *entry = tagged;
        assert(!adreno8_space_verify(other, false));
    }
    old_writes = writes; old_clean = clean_count;
    /* no wait for the GPU here: the caller unmaps once every submission that could read the BO retired */
    assert(adreno8_bo_unmap(address, size) && idle_calls == 1);
    assert(*first == ADRENO8_PTE_EXTERNAL && writes > old_writes && clean_count > old_clean);
    assert(cleaned_since(old_clean, first, ADRENO8_PTE_EXTERNAL, &seen) == size / 4096 * sizeof(uint64_t) &&
        (seen || mode == TWO_LEAVES));
    assert(*guard == guard_pte());
    if (mode == GUARD_CHANGED) {
        *guard = 0;
        assert(!adreno8_bo_release(address, size) && *first == ADRENO8_PTE_EXTERNAL);
        stays_closed(address, size); release_all(); return 0;
    }
    assert(adreno8_bo_release(address, size) && *first == 0 && *guard == 0);
    /* the next search starts after the last reservation, not in the room just given back */
    assert(adreno8_bo_reserve(4096) == next + 8192);
    assert(*adreno8_space_entry(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START) == startup_pte);
    assert(*adreno8_space_entry(&adreno8_gpu_space, next) == ADRENO8_PTE_EXTERNAL);
    assert(adreno8_bo_ready() && !closed);
    release_all();
    puts("external BO: guard pages, next-fit cursor, permissions, partial map, TLB and retention: PASS");
}
'''
MODES = CASES[CASES.index("enum {"):CASES.index("MODES };")].count(",")

MUTATIONS = (
    ("size 0 accepted", "if (!size || !bo_range(ADRENO8_GPU_IOVA_START, span) ||",
     "if (!bo_range(ADRENO8_GPU_IOVA_START, span) ||"),
    ("no guard page", "\t*adreno8_space_entry(&adreno8_gpu_space, address + size) = bo_guard_pte;\n", ""),
    ("guard page outside the commit",
     "if (address + size < bo_mapped_low) bo_mapped_low = address + size;\n"
     "\tif (address + span > bo_mapped_high) bo_mapped_high = address + span;",
     "if (address < bo_mapped_low) bo_mapped_low = address;\n"
     "\tif (address + size > bo_mapped_high) bo_mapped_high = address + size;"),
    ("release keeps the guard", "\t*guard = 0;\n", ""),
    ("search from the lowest page", "uint32_t address, from = bo_free_from;",
     "uint32_t address, from = ADRENO8_GPU_IOVA_START;"),
    ("cursor never wraps", "address + span < ADRENO8_GPU_IOVA_START + BO_WINDOW ?", "true ?"),
    ("missing table closes submissions", 'printf("adreno8: no page table for GPU address',
     'adreno8_bo_stop();\n\t\t\tprintf("adreno8: no page table for GPU address'),
    ("unmap of a page it does not own", 'if (!entry || !bo_owned(*entry)) return bo_fail("unmap ownership");',
     'if (!entry) return bo_fail("unmap ownership");'),
    ("map without the external tag", "*entry = phys | adreno8_bo_attributes(prot) | ADRENO8_PTE_EXTERNAL;",
     "*entry = phys | adreno8_bo_attributes(prot);"),
)


def build(target, page_size, label):
    binary = target / f"check-{label}-{page_size}"
    compiled = subprocess.run(["cc", "-std=c11", f"-DPAGE_SIZE={page_size}",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(target / "check.c"), "-o", str(binary)],
        capture_output=True, text=True)
    if compiled.returncode:
        raise RuntimeError(compiled.stderr)
    return binary


def main():
    fixture = runpy.run_path(str(ROOT / "tools/test-adreno8-smmu.py"))
    stub = fixture["stub"].replace("assert(size == 4096);", "assert(size != 0);")
    stub = stub.replace("void adreno8_bo_describe_fault(uint32_t fault) {}", "", 1)
    setup = fixture["tests"].split("int main(void) {")[0]
    production = (DEV / "adreno8_bo.c").read_text()
    with tempfile.TemporaryDirectory(prefix="adreno8-bo-") as directory:
        target = Path(directory)
        (target / "adreno8_private.h").write_text(stub)
        for name in ("adreno8_smmu_private.h", "adreno8_memory_private.h", "adreno8_context_private.h",
                "adreno8_hwsched_private.h", "adreno8_bo_private.h"):
            (target / name).write_text((DEV / name).read_text())
        (target / "adreno8_smmu.c").write_text(fixture["driver"])
        (target / "adreno8_bo.c").write_text(production)
        source = ('#include "adreno8_smmu.c"\n' + FENCES + '\n#define mach_absolute_time bo_test_clock\n'
            '#include "adreno8_bo.c"\n#undef mach_absolute_time\n#undef printf\n' + setup + CASES)
        (target / "check.c").write_text(source)
        for page_size in (4096, 16384):
            binary = build(target, page_size, "production")
            for mode in range(MODES):
                result = subprocess.run([str(binary), str(mode)], capture_output=True, text=True)
                if result.returncode:
                    raise RuntimeError(f"PAGE_SIZE={page_size}, mode={mode}:\n" + result.stdout + result.stderr)
            print(f"External BO: {MODES} ownership/lifecycle scenarios, PAGE_SIZE={page_size}: PASS")
        for label, before, after in MUTATIONS:
            assert production.count(before) == 1, label
            (target / "adreno8_bo.c").write_text(production.replace(before, after, 1))
            binary = build(target, 16384, "mutant")
            failed = [mode for mode in range(MODES)
                      if subprocess.run([str(binary), str(mode)], capture_output=True).returncode]
            assert failed, "mutation survived: " + label
        print(f"External BO mutations rejected: {len(MUTATIONS)}")


if __name__ == "__main__":
    main()

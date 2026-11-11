#!/usr/bin/env python3
"""Verify actual Iris DMA allocation and release retain pages after failed TLB synchronization."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path('xnu/files/bsd/dev/annibale/annibale_iris_hw.c')

HARNESS = r'''
#define ANNIBALE_IRIS_HOST_TEST 1
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "annibale_iris_private.h"
#define PAGE_SIZE 16384u
#define PAGE_MASK (PAGE_SIZE - 1u)
#define KMA_KOBJECT 1
#define KMA_ZERO 2
#define KMA_DATA 4
#define VM_KERN_MEMORY_BSD 1
#define KERN_SUCCESS 0
static const int kernel_map = 0;
__PTE_DEFINES__
struct iris_controller g_iris;
static uint64_t entries[512];
static unsigned allocated, freed, mapped, sleeps, sync_writes;
static unsigned fail_map_at = UINT32_MAX;
static bool busy, allocation_fails;
static vm_offset_t allocation;

static int kmem_alloc(int map, vm_offset_t *p, unsigned size, unsigned flags, unsigned tag)
{
    (void)map; (void)flags; (void)tag;
    assert(size == PAGE_SIZE);
    if (allocation_fails) return 1;
    allocation = *p = (uintptr_t)calloc(1, size);
    assert(allocation);
    allocated++;
    return 0;
}
static void kmem_free(int map, vm_offset_t p, unsigned size)
{
    (void)map;
    assert(p == allocation && size == PAGE_SIZE && !busy && g_iris.smmu_ready);
    freed++;
    free((void *)p);
    allocation = 0;
}
static uint64_t kvtophys(vm_offset_t p) { return 0x1000000u + p - allocation; }
void iris_cache_clean_inval(const void *p, size_t n) { (void)p; (void)n; }
static bool iris_smmu_map_page(uint32_t iova, uint64_t p, uint64_t attributes)
{
    assert(iova >= 0x26000000u && iova < 0x26004000u && (p & 4095u) == 0);
    if (mapped++ == fail_map_at) return false;
    entries[(iova >> 12) & 511u] = p | attributes;
    return true;
}
static void mmio_w32(vm_offset_t base, uint32_t off, uint32_t value)
{
    assert(base == 1);
    if (off == 209u * 4096u + 0x610u) assert(value == 81);
    else assert(off == 209u * 4096u + 0x7f0u && value == 0);
    sync_writes++;
}
static uint32_t mmio_r32(vm_offset_t base, uint32_t off)
{
    assert(base == 1 && off == 209u * 4096u + 0x7f4u);
    return busy ? 1 : 0;
}
void iris_sleep_us(uint32_t us) { assert(us == 2); sleeps++; }

__FUNCTIONS__

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *name = argv[1];
    bool freeing = !strncmp(name, "free_", 5) || !strcmp(name, "retry_free");
    bool failed_sync = strstr(name, "sync_fail") || !strcmp(name, "partial_held") || !strcmp(name, "retry_free");
    g_iris.smmu = 1;
    g_iris.smmu_ready = g_iris.smmu_owned = true;
    g_iris.smmu_page = 4096;
    g_iris.smmu_pages = 128;
    g_iris.smmu_cb = 81;
    g_iris.table_count = 1;
    g_iris.tables[0].kva = (uintptr_t)entries;
    g_iris.tables[0].key = 0x26000000u >> 21;
    g_iris.tables[0].level = 2;
    struct iris_dma_buf buffer = {0};
    if (!strcmp(name, "empty_free")) {
        assert(iris_dma_free_unmap(&buffer) && iris_dma_free_unmap(NULL));
        assert(!allocated && !freed && !sync_writes);
        return 0;
    }
    if (!strcmp(name, "zero_size")) {
        assert(!iris_dma_alloc_map(&buffer, 0x26000000u, 0, false));
        assert(!buffer.kva && !allocated && !freed);
        return 0;
    }
    if (!strcmp(name, "allocation_fail")) allocation_fails = true;
    if (!strcmp(name, "partial_synced") || !strcmp(name, "partial_held")) fail_map_at = 1;
    if (freeing) {
        assert(iris_dma_alloc_map(&buffer, 0x26000000u, PAGE_SIZE, false));
        memset((void *)buffer.kva, 0x5a, PAGE_SIZE);
    }
    busy = failed_sync;
    if (!strcmp(name, "free_not_ready")) g_iris.smmu_ready = false;
    bool result = freeing ? iris_dma_free_unmap(&buffer) :
        iris_dma_alloc_map(&buffer, 0x26000000u, PAGE_SIZE, true);
    bool expected = !failed_sync && strcmp(name, "partial_synced") &&
        strcmp(name, "allocation_fail") && strcmp(name, "free_not_ready");
    assert(result == expected);
    if (failed_sync || !strcmp(name, "free_not_ready")) {
        assert(allocated == 1 && freed == 0 && buffer.kva == allocation);
        assert(buffer.size == PAGE_SIZE && buffer.iova == 0x26000000u);
        if (freeing) for (unsigned i = 0; i < PAGE_SIZE; i++) assert(((uint8_t *)buffer.kva)[i] == 0x5a);
    } else if (freeing || !strcmp(name, "partial_synced")) {
        assert(allocated == 1 && freed == 1 && !buffer.kva && !buffer.size);
    } else if (allocation_fails) assert(!allocated && !buffer.kva && !freed);
    else assert(buffer.kva == allocation && !freed);
    if (!strcmp(name, "retry_free")) {
        busy = false;
        assert(iris_dma_free_unmap(&buffer));
        assert(freed == 1 && !buffer.kva && !buffer.size);
    }
    if (freeing || fail_map_at != UINT32_MAX) {
        for (unsigned i = 0; i < 4; i++) assert(entries[i] == 0);
    }
    if (allocation) free((void *)allocation);
    printf("PASS %s allocated=%u freed=%u sleeps=%u\n", name, allocated, freed, sleeps);
    return 0;
}
'''

CASES = ['allocate_success', 'allocate_sync_fail', 'partial_synced', 'partial_held',
         'free_success', 'free_sync_fail', 'free_not_ready', 'retry_free',
         'empty_free', 'zero_size', 'allocation_fail']


def extract(source):
    names = ['iris_smmu_tlb_sync', 'iris_smmu_unmap_range', 'iris_dma_alloc_map', 'iris_dma_free_unmap']
    bodies = []
    for name in names:
        match = re.search(r'(?:static )?bool\n' + name + r'\([^\n]*\)\n\{', source)
        if not match: raise AssertionError('DMA source boundary changed: ' + name)
        end = source.index('\n}\n', match.end()) + 3
        bodies.append(source[match.start():end])
    return '\n'.join(bodies)


def run(root, source, directory):
    directory.mkdir(parents=True, exist_ok=True)
    defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define IRIS_PTE_'))
    path, binary = directory / 'fixture.c', directory / 'fixture'
    path.write_text(HARNESS.replace('__PTE_DEFINES__', defines).replace('__FUNCTIONS__', extract(source)))
    result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-I' + str(root / 'xnu/files/bsd'), '-I' + str(root / SOURCE.parent),
        str(path), '-o', str(binary)], capture_output=True, text=True, timeout=60)
    (directory / 'compile.log').write_text(result.stdout + result.stderr)
    if result.returncode: raise AssertionError('Actual DMA functions did not compile: ' + str(directory / 'compile.log'))
    cases = []
    for name in CASES:
        result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=5)
        (directory / (name + '.log')).write_text(result.stdout + result.stderr)
        cases.append({'name': name, 'rc': result.returncode, 'passed': result.returncode == 0})
    return cases


def verify(args, directory):
    source = (args.source_root / SOURCE).read_text()
    results = {'functions_sha256': hashlib.sha256(extract(source).encode()).hexdigest(), 'variants': {}}
    cases = run(args.source_root, source, directory / 'candidate')
    results['variants']['candidate'] = cases
    failures = [c['name'] for c in cases if not c['passed']]
    if args.mutations:
        changes = {
            'release_without_sync': ('if (!iris_smmu_unmap_range(buf->iova, buf->size))', 'if (false)'),
            'allocate_without_sync': ('return iris_smmu_tlb_sync();\n}\n\nbool\niris_dma_free_unmap', 'return true;\n}\n\nbool\niris_dma_free_unmap'),
            'partial_release_without_sync': ('if (iris_smmu_unmap_range(iova, off))', 'if (true)'),
            'tlb_timeout_success': ('\treturn false;\n}\n\nstatic bool\niris_smmu_map_page', '\treturn true;\n}\n\nstatic bool\niris_smmu_map_page'),
        }
        for name, (old, new) in changes.items():
            if source.count(old) != 1: raise AssertionError('Mutation boundary changed: ' + name)
            cases = run(args.source_root, source.replace(old, new, 1), directory / name)
            results['variants'][name] = cases
            if all(c['passed'] for c in cases): failures.append(name + '_accepted')
    (directory / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    if failures: raise AssertionError('Iris DMA regression failed: ' + ', '.join(failures))
    print(f'PASS: {len(CASES)} actual DMA lifetime cases')
    if args.mutations: print('PASS: 4 compiled release/synchronization variants rejected')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if args.output_dir: verify(args, args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix='iris-dma-') as directory: verify(args, Path(directory))


if __name__ == '__main__':
    main()

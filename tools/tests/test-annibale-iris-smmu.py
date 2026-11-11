#!/usr/bin/env python3
"""Compile the actual Iris SMMU setup against context ownership and SCTLR fixtures."""
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
typedef uintptr_t vm_size_t;

__PTE_DEFINES__

struct iris_controller g_iris;
static uint32_t registers[0x100000 / 4], initial[0x100000 / 4];
static unsigned reads, writes, maps, tables, device_maps, syncs;
static unsigned cb_reads[128], cbar_writes[128], sctlr_writes[128];
static uint32_t pages, page, banks, target, reject_cbar;
static bool fail_mapping;

static void address(vm_offset_t base, uint32_t offset)
{
	assert(base == 1 && (offset & 3u) == 0 && offset <= 0x100000u - 4u);
}

static uint32_t mmio_r32(vm_offset_t base, uint32_t offset)
{
	address(base, offset);
	reads++;
	if (offset >= pages * page) {
		uint32_t cb = offset / page - pages;
		assert(cb < banks);
		assert((registers[(page + 4u * cb) / 4u] & 0x30000u) != 0x30000u);
		cb_reads[cb]++;
	}
	return registers[offset / 4u];
}

static void mmio_w32(vm_offset_t base, uint32_t offset, uint32_t value)
{
	address(base, offset);
	writes++;
	if (offset >= page && offset < page + 4u * banks) {
		uint32_t cb = (offset - page) / 4u;
		cbar_writes[cb]++;
		if (cb == reject_cbar) return;
	}
	if (offset >= pages * page) {
		uint32_t cb = offset / page - pages;
		assert(cb < banks);
		if (offset % page == 0) sctlr_writes[cb]++;
	}
	registers[offset / 4u] = value;
}

static vm_offset_t ml_io_map(vm_offset_t physical, vm_size_t size)
{
	assert(physical == 0x15000000u && size == 0x100000u);
	maps++;
	return fail_mapping ? 0 : 1;
}

static vm_offset_t iris_smmu_get_table(uint8_t level, uint32_t key, uint64_t *physical)
{
	assert(level == 0 && key == 0);
	tables++;
	*physical = 0x9000u;
	return 0x8000u;
}

static bool iris_smmu_map_page(uint32_t iova, uint64_t physical, uint64_t attributes)
{
	(void)attributes;
	assert((iova == 0xffade000u && physical == 0x17421000u) ||
	    (iova == 0xffadf000u && physical == 0x00508000u));
	device_maps++;
	return true;
}

static bool iris_smmu_tlb_sync(void) { syncs++; return true; }
void iris_sleep_us(uint32_t duration) { assert(duration == 2); }

__SETUP__

static void route(unsigned slot, uint32_t sid, unsigned cb)
{
	registers[(0x800u + 4u * slot) / 4u] = 0x80000000u | sid;
	registers[(0xc00u + 4u * slot) / 4u] = 0x300u | cb;
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	const char *name = argv[1];
	uint32_t idr1 = 0x60000053u;
	if (!strcmp(name, "64k_pages")) idr1 = 0x80000004u;
	if (!strcmp(name, "two_pages")) idr1 = 0x00000004u;
	page = idr1 >> 31 ? 0x10000u : 0x1000u;
	pages = 2u << ((idr1 >> 28) & 7u);
	banks = idr1 & 0xffu;
	target = banks - 2u;
	reject_cbar = UINT32_MAX;
	registers[0x20u / 4u] = 0x4c017e7fu;
	registers[0x24u / 4u] = idr1;
	for (unsigned cb = 0; cb < banks; cb++) {
		registers[(page + 4u * cb) / 4u] = 0x30000u;
	}
	registers[(page + 4u * target) / 4u] = 0;
	registers[((pages + target) * page) / 4u] = 0x3d22e706u;
	bool expected = true, used_target = false;
	if (!strcmp(name, "zero")) registers[((pages + target) * page) / 4u] = 0;
	if (!strcmp(name, "enabled") || !strcmp(name, "enabled_unrouted")) {
		registers[((pages + target) * page) / 4u] = 0x27u;
		expected = false;
	}
	if (!strcmp(name, "invalid_sctlr")) {
		registers[((pages + target) * page) / 4u] = UINT32_MAX;
		expected = false;
	}
	if (!strcmp(name, "used_high_bank")) {
		route(3, 0x1400u, target);
		used_target = true;
		expected = false;
	}
	if (!strcmp(name, "reserved_bypass")) {
		registers[(page + 4u * target) / 4u] = 0x30000u;
		registers[(page + 4u * (banks - 1u)) / 4u] = 0;
		expected = false;
	}
	if (!strcmp(name, "stage12") || !strcmp(name, "invalid_cbar")) {
		registers[(page + 4u * target) / 4u] = !strcmp(name, "stage12") ? 0x30000u : UINT32_MAX;
		expected = false;
	}
	if (!strcmp(name, "cbar_rejected")) {
		reject_cbar = target;
		expected = false;
	}
	if (!strcmp(name, "next_disabled")) {
		registers[((pages + target) * page) / 4u] = 0x27u;
		target--;
		registers[(page + 4u * target) / 4u] = 0;
		registers[((pages + target) * page) / 4u] = 0x426u;
	}
	if (!strcmp(name, "existing_iris")) {
		route(4, 0x1940u, target);
		route(5, 0x1947u, target);
	}
	if (!strcmp(name, "no_slots") || !strcmp(name, "one_slot")) {
		for (unsigned slot = 0; slot < 127; slot++) route(slot, 0x1000u + slot, banks - 1u);
		if (!strcmp(name, "one_slot")) registers[(0x800u + 4u * 12u) / 4u] = 0;
		expected = false;
	}
	if (!strcmp(name, "bad_idr")) {
		registers[0x20u / 4u] = UINT32_MAX;
		expected = false;
	}
	if (!strcmp(name, "map_failure")) {
		fail_mapping = true;
		expected = false;
	} else g_iris.smmu = 1;
	if (!strcmp(name, "already_ready")) g_iris.smmu_ready = true;
	memcpy(initial, registers, sizeof(registers));
	bool ready = iris_smmu_setup();
	printf("case=%s ready=%u target=%u reads=%u writes=%u\n", name, ready, target, reads, writes);
	assert(ready == expected);
	assert(g_iris.smmu_ready == expected);
	assert(cbar_writes[banks - 1u] == 0 && sctlr_writes[banks - 1u] == 0);
	if (!strcmp(name, "already_ready")) {
		assert(reads == 0 && writes == 0 && maps == 0);
		return 0;
	}
	if (used_target) assert(cbar_writes[target] == 0 && cb_reads[target] == 0);
	if (ready) {
		assert(g_iris.smmu_cb == target && tables == 1 && device_maps == 2 && syncs == 1);
		assert(registers[((pages + target) * page) / 4u] == 0x27u);
		unsigned non_pixel = !strcmp(name, "existing_iris") ? 4 : 0;
		unsigned pixel = !strcmp(name, "existing_iris") ? 5 : 1;
		assert(g_iris.smmu_smr_non_pix == non_pixel && g_iris.smmu_smr_pix == pixel);
		assert(registers[(0x800u + 4u * non_pixel) / 4u] == 0x80001940u);
		assert(registers[(0x800u + 4u * pixel) / 4u] == 0x80001947u);
		assert(registers[(0xc00u + 4u * non_pixel) / 4u] == (0x300u | target));
		assert(registers[(0xc00u + 4u * pixel) / 4u] == (0x300u | target));
	} else {
		assert(tables == 0 && device_maps == 0 && syncs == 0);
		for (unsigned cb = 0; cb < banks; cb++) assert(sctlr_writes[cb] == 0);
		assert(memcmp(initial + 0x800 / 4, registers + 0x800 / 4, 0x800) == 0);
	}
	if (!strcmp(name, "next_disabled")) {
		assert(sctlr_writes[banks - 2u] == 0);
		assert(registers[((pages + banks - 2u) * page) / 4u] == 0x27u);
	}
	return 0;
}
'''

CASES = ['reset_fields', 'zero', 'enabled', 'enabled_unrouted', 'invalid_sctlr',
         'used_high_bank', 'reserved_bypass', 'stage12', 'invalid_cbar', 'cbar_rejected',
         'next_disabled', 'existing_iris', 'no_slots', 'one_slot', 'bad_idr',
         'map_failure', 'already_ready', '64k_pages', 'two_pages']


def setup(source):
    match = re.search(r'static bool\niris_smmu_setup\(void\)\n\{', source)
    if not match:
        raise AssertionError('Iris SMMU source boundary changed')
    end = source.index('\n}\n', match.end()) + 3
    return source[match.start():end].replace(
        '__asm__ volatile("dsb sy" ::: "memory");',
        '__atomic_thread_fence(__ATOMIC_SEQ_CST);')


def run_variant(root, source, directory):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / 'harness.c'
    pte_defines = '\n'.join(line for line in source.splitlines()
                            if line.startswith('#define IRIS_PTE_'))
    path.write_text(HARNESS.replace('__PTE_DEFINES__', pte_defines)
                          .replace('__SETUP__', setup(source)))
    binary = directory / 'harness'
    compiled = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-I' + str(root / 'xnu/files/bsd'), '-I' + str(root / 'xnu/files/bsd/dev/annibale'),
        str(path), '-o', str(binary)], capture_output=True, text=True, timeout=60)
    (directory / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise AssertionError('Actual SMMU setup did not compile; inspect compile.log')
    cases = []
    for name in CASES:
        result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=5)
        (directory / (name + '.log')).write_text(result.stdout + result.stderr)
        cases.append({'name': name, 'rc': result.returncode, 'passed': result.returncode == 0})
    return cases


def verify(args, directory):
    source = (args.source_root / SOURCE).read_text()
    results = {'setup_sha256': hashlib.sha256(setup(source).encode()).hexdigest(), 'variants': {}}
    cases = run_variant(args.source_root, source, directory / 'candidate')
    results['variants']['candidate'] = cases
    failures = [case['name'] for case in cases if not case['passed']]
    if args.baseline_ref:
        baseline = subprocess.check_output(['git', '-C', str(args.source_root), 'show',
            args.baseline_ref + ':' + str(SOURCE)], text=True)
        cases = run_variant(args.source_root, baseline, directory / 'baseline')
        results['variants']['baseline'] = cases
        if all(case['passed'] for case in cases): failures.append('unfixed_baseline_accepted')
    if args.mutations:
        changes = {
            'require_whole_sctlr_zero': ('(sctlr & 1u) == 0', 'sctlr == 0'),
            'ignore_enabled_bit': ('(sctlr & 1u) == 0', 'true'),
            'wrong_enabled_bit': ('(sctlr & 1u) == 0', '(sctlr & 2u) == 0'),
            'ignore_existing_routes': ('(used_cbs[cb >> 6] & (1ULL << (cb & 63u))) != 0', 'false'),
            'include_reserved_bypass': ('uint32_t idx = banks - 1u', 'uint32_t idx = banks'),
            'ignore_stage12': ('(old_cbar & 0x30000u) == 0x30000u', '(old_cbar & 0x30000u) == 0x30000u && false'),
            'ignore_cbar_readback': ('(cbar & 0x30000u) != 0x10000u', 'false'),
        }
        for name, (old, new) in changes.items():
            if source.count(old) != 1: raise AssertionError('Mutation boundary changed: ' + name)
            cases = run_variant(args.source_root, source.replace(old, new, 1), directory / name)
            results['variants'][name] = cases
            if all(case['passed'] for case in cases): failures.append(name + '_accepted')
    (directory / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    if failures: raise AssertionError('Iris SMMU regression failed: ' + ', '.join(failures))
    print(f'PASS: {len(CASES)} actual SMMU setup cases')
    if args.baseline_ref:
        failed = sum(not c['passed'] for c in results['variants']['baseline'])
        print(f'PASS: original setup rejected in {failed} cases')
    if args.mutations: print(f'PASS: {len(changes)} compiled protection variants rejected')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--baseline-ref')
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        verify(args, args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix='iris-smmu-') as directory:
            verify(args, Path(directory))


if __name__ == '__main__':
    main()

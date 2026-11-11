#!/usr/bin/env python3
"""Run actual PAS authentication, SMMU teardown and loader return with injected failures."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HW = Path('xnu/files/bsd/dev/annibale/annibale_iris_hw.c')
RETURN = Path('xnu/files/osfmk/arm64/loader_return.c')

HARNESS = r'''
#define ANNIBALE_IRIS_HOST_TEST 1
#define GENERIC_PSCI_SMP 1
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "annibale_iris_private.h"
typedef uintptr_t vm_size_t;
#define PAGE_SIZE 16384u
#define KMA_KOBJECT 1
#define KMA_ZERO 2
#define KMA_DATA 4
#define VM_KERN_MEMORY_BSD 1
#define KERN_SUCCESS 0
#define VM_WIMG_WCOMB 0
#define IRIS_MBN_META_MAX 0x4000u
#define IRIS_MBN_MAX_SEGS 32u
static const int kernel_map = 0;
struct iris_controller g_iris;
static const char *case_name;
static uint32_t registers[0x100000u / 4u], initial[0x100000u / 4u];
static uint8_t firmware[64], carveout[4096], retained[64];
static unsigned shutdown_calls, mmio_writes, tlb_reads, sleeps;
static unsigned stopped, stage, save_calls, display_calls;
static uint64_t return_reason, return_code;
static int32_t transport;
static uint64_t firmware_result;
static bool missing_answer, auth_case;
static bool loader_return_entered, annibale_halting_poweroff;
static const char *debug_buf_base, *debug_buf_ptr;

__MBN_TYPES__

static bool named(const char *name) { return !strcmp(case_name, name); }
void iris_cache_clean_inval(const void *p, size_t n) { (void)p; (void)n; }
bool iris_hw_probe_firmware(void) { return true; }
static bool iris_mbn_parse(const uint8_t *p, size_t n, struct iris_mbn_image *image)
{
    assert(p == firmware && n == sizeof(firmware));
    memset(image, 0, sizeof(*image));
    image->header_size = 16;
    image->hash_off = 16;
    image->hash_size = 16;
    image->total_mem_size = sizeof(carveout);
    image->min_paddr = 0x800000u;
    image->seg_count = 1;
    image->segs[0] = (struct iris_mbn_seg){32, 16, 0x800000u, 32};
    return true;
}
static int kmem_alloc(int map, vm_offset_t *p, unsigned n, unsigned flags, unsigned tag)
{
    (void)map; (void)flags; (void)tag;
    *p = (uintptr_t)aligned_alloc(PAGE_SIZE, n);
    assert(*p != 0);
    return 0;
}
static uint64_t kvtophys(vm_offset_t p)
{
    return 0x400000u + p - g_iris.pas_meta;
}
static vm_offset_t ml_io_map_unmappable(vm_offset_t p, unsigned n, unsigned attr)
{
    assert(p == 0x9c300000u && n == sizeof(carveout));
    (void)attr;
    return (uintptr_t)carveout;
}
static void ml_io_unmap(vm_offset_t p, unsigned n)
{
    assert(p == (uintptr_t)carveout && n == sizeof(carveout));
}
static int32_t adreno8_scm_call(uint64_t function, uint64_t arginfo,
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t *answer)
{
    if (function == 0x42000c08u) { *answer = 0; return 0; }
    assert(a0 == 9 && a3 == 0);
    if (function == 0x42000206u) {
        assert(stopped == 1 && arginfo == 1 && a1 == 0 && a2 == 0);
        shutdown_calls++;
        stage = 2;
        if (!missing_answer) *answer = firmware_result;
        return transport;
    }
    assert(auth_case);
    if (function == 0x42000201u) {
        assert(arginfo == 0x82 || arginfo == 0x22);
        assert(a1 == 0x400000u && a2 == 0);
        assert(!g_iris.pas_shutdown_required && !g_iris.dma_shutdown_unconfirmed);
        *answer = 0;
        return named("auth_init_fail") ? -1 : 0;
    }
    if (function == 0x42000202u) {
        assert(arginfo == 3 && a1 == 0x9c300000u && a2 == sizeof(carveout));
        assert(!g_iris.pas_shutdown_required && !g_iris.dma_shutdown_unconfirmed);
        *answer = 0;
        return named("auth_mem_fail") ? -1 : 0;
    }
    assert(function == 0x42000205u && arginfo == 1 && a1 == 0 && a2 == 0);
    /* The request can have a side effect even if the reply reports failure. */
    assert(g_iris.pas_shutdown_required && g_iris.dma_shutdown_unconfirmed);
    *answer = named("auth_firmware_fail") ? 7 : 0;
    return named("auth_transport_fail") ? -1 : 0;
}
static uint32_t mmio_r32(vm_offset_t base, uint32_t off)
{
    assert(base == 1 && off < 0x100000u && (off & 3u) == 0);
    if (off == (128u + 81u) * 4096u + 0x7f4u) {
        tlb_reads++;
        if (named("tlb_timeout")) return 1;
        if (named("tlb_delayed") && tlb_reads < 3) return 1;
    }
    return registers[off / 4u];
}
static void mmio_w32(vm_offset_t base, uint32_t off, uint32_t value)
{
    assert(stopped == 1 && base == 1 && off < 0x100000u && (off & 3u) == 0);
    if (shutdown_calls) assert(stage == 2);
    mmio_writes++;
    if ((named("fault_non_pixel") && off == 0xc2cu) ||
        (named("fault_pixel") && off == 0xc30u) ||
        (named("smr_non_pixel") && off == 0x82cu) ||
        (named("smr_pixel") && off == 0x830u) ||
        (named("sctlr_rejected") && off == 209u * 4096u)) return;
    registers[off / 4u] = value;
}
void iris_sleep_us(uint32_t us) { assert(us == 2); sleeps++; }

__HW_FUNCTIONS__

static void psci_smp_stop_others(void) { stopped++; stage = 1; }
static void loader_log_linearize(void) { }
static void loader_log_append_msgbuf(void) { }
static void loader_log_puts(const char *p) { (void)p; }
static void loader_log_putc(char c) { (void)c; }
static void loader_panic_sync_ufs(const char *reason, unsigned flags)
{
    assert(!strcmp(reason, "loader_return") && flags == 1 && stopped == 1);
    save_calls++;
}
static void annibale_ufs_quiesce(void) { }
static void adreno8_return_quiesce(void) { }
static void annibale_wifi_quiesce(void) { }
static void annibale_haptic_quiesce(void) { }
static void annibale_torch_quiesce(void) { }
static void MiniIOMobileFramebufferPowerOff(void) { display_calls++; }
static void MiniIOMobileFramebufferQuiesce(void) { display_calls++; }
static void hv_wdt_pet_NOW(void) { }
static void loader_jump(uint64_t reason, uint64_t code)
{
    return_reason = reason;
    return_code = code;
}

__RETURN_FUNCTION__

int main(int argc, char **argv)
{
    assert(argc == 2);
    case_name = argv[1];
    memset(retained, 0x5a, sizeof(retained));
    g_iris.uc_region.kva = (uintptr_t)retained;
    g_iris.uc_region.size = sizeof(retained);
    g_iris.table_count = 1;
    g_iris.tables[0].kva = (uintptr_t)retained;
    g_iris.smmu = 1;
    g_iris.smmu_page = 4096;
    g_iris.smmu_pages = 128;
    g_iris.smmu_cb = 81;
    g_iris.smmu_smr_non_pix = 11;
    g_iris.smmu_smr_pix = 12;
    g_iris.smmu_owned = g_iris.smmu_ready = true;
    g_iris.pas_shutdown_required = g_iris.pas_authed = true;
    g_iris.dma_shutdown_unconfirmed = true;
    g_iris.hw_ready = true;
    registers[0x82cu / 4u] = 0x80001940u;
    registers[0x830u / 4u] = 0x80001947u;
    registers[0xc2cu / 4u] = registers[0xc30u / 4u] = 0x351u;
    registers[209u * 4096u / 4u] = 0x27u;
    bool expected = true;
    if (named("never_started")) {
        g_iris.pas_shutdown_required = g_iris.pas_authed = false;
        g_iris.smmu_owned = g_iris.smmu_ready = false;
        g_iris.dma_shutdown_unconfirmed = false;
    }
    if (named("smmu_only")) {
        g_iris.pas_shutdown_required = g_iris.pas_authed = false;
        g_iris.dma_shutdown_unconfirmed = false;
    }
    if (named("previously_shutdown")) {
        g_iris.pas_shutdown_required = g_iris.pas_authed = false;
        g_iris.smmu_owned = g_iris.smmu_ready = false;
    }
    if (named("shutdown_flag_recovery")) g_iris.dma_shutdown_unconfirmed = false;
    if (named("pending_auth")) g_iris.pas_authed = false;
    if (named("authed_only")) g_iris.pas_shutdown_required = false;
    if (named("partial_setup")) {
        g_iris.smmu_ready = false;
        registers[0x82cu / 4u] = registers[0x830u / 4u] = 0;
    }
    if (named("transport_fail")) { transport = -1; expected = false; }
    if (named("firmware_fail")) { firmware_result = 7; expected = false; }
    if (named("answer_missing")) { missing_answer = true; expected = false; }
    if (named("wrong_route")) { registers[0x830u / 4u] = 0x80001800u; expected = false; }
    if (named("bad_mapping")) { g_iris.smmu = 0; expected = false; }
    if (named("bad_geometry")) { g_iris.smmu_cb = 512; expected = false; }
    if (named("sctlr_invalid")) { registers[209u * 4096u / 4u] = UINT32_MAX; expected = false; }
    if (named("smr_non_pixel")) { registers[0x82cu / 4u] = 0; expected = false; }
    if (named("smr_pixel")) { registers[0x830u / 4u] = 0; expected = false; }
    if (named("fault_non_pixel") || named("fault_pixel") ||
        named("sctlr_rejected") || named("tlb_timeout")) expected = false;
    auth_case = !strncmp(case_name, "auth_", 5);
    bool want_shutdown = !named("never_started") && !named("smmu_only") &&
        !named("previously_shutdown");
    if (auth_case) {
        g_iris.pas_authed = g_iris.pas_shutdown_required = false;
        g_iris.dma_shutdown_unconfirmed = false;
        g_iris.fw_data = firmware;
        g_iris.fw_size = sizeof(firmware);
        bool authenticated = iris_pas_authenticate();
        bool want_auth = named("auth_success");
        want_shutdown = !named("auth_init_fail") && !named("auth_mem_fail");
        assert(authenticated == want_auth);
        assert(g_iris.pas_authed == want_auth);
        assert(g_iris.pas_shutdown_required == want_shutdown);
        assert(g_iris.dma_shutdown_unconfirmed == want_shutdown);
    }
    bool want_dma_retained = g_iris.dma_shutdown_unconfirmed || want_shutdown;
    bool want_normal_return = expected && !want_dma_retained;
    bool original_required = g_iris.pas_shutdown_required;
    memcpy(initial, registers, sizeof(registers));
    loader_return();
    assert(stopped == 1 && save_calls == 1 && display_calls == 1);
    assert(return_reason == (want_normal_return ? 0u : 3u));
    assert(return_code == (want_normal_return ? 0u : 9u));
    assert(shutdown_calls == (want_shutdown ? 1u : 0u));
    assert(g_iris.dma_shutdown_unconfirmed == want_dma_retained);
    assert(g_iris.table_count == 1 && g_iris.tables[0].kva == (uintptr_t)retained);
    assert(g_iris.uc_region.kva == (uintptr_t)retained);
    for (unsigned i = 0; i < sizeof(retained); i++) assert(retained[i] == 0x5a);
    if (transport || firmware_result || missing_answer) {
        assert(g_iris.pas_shutdown_required == original_required);
        assert(g_iris.pas_authed && g_iris.hw_ready && g_iris.smmu_owned);
        assert(mmio_writes == 0);
        assert(!memcmp(registers, initial, sizeof(registers)));
    } else {
        assert(!g_iris.pas_shutdown_required && !g_iris.pas_authed);
        assert(g_iris.smmu_owned == !expected);
        if (expected && !named("never_started") && !named("previously_shutdown")) {
            assert(registers[0x82cu / 4u] == 0x80001940u);
            assert(registers[0x830u / 4u] == 0x80001947u);
            assert(registers[0xc2cu / 4u] == 0x20300u);
            assert(registers[0xc30u / 4u] == 0x20300u);
            assert((registers[209u * 4096u / 4u] & 1u) == 0);
        }
    }
    if (named("never_started") || named("previously_shutdown")) assert(mmio_writes == 0);
    if (named("second_return")) {
        unsigned previous_writes = mmio_writes;
        assert(!annibale_iris_return_quiesce());
        assert(g_iris.dma_shutdown_unconfirmed && shutdown_calls == 1);
        assert(mmio_writes == previous_writes);
    }
    if (named("tlb_timeout")) assert(sleeps == 2000 && tlb_reads == 2000);
    if (named("tlb_delayed")) assert(sleeps == 2 && tlb_reads == 3);
    for (unsigned i = 0; i < sizeof(registers) / sizeof(registers[0]); i++) {
        unsigned off = i * 4u;
        if (off == 0x82cu || off == 0x830u || off == 0xc2cu || off == 0xc30u ||
            off == 209u * 4096u || off == 209u * 4096u + 0x610u ||
            off == 209u * 4096u + 0x7f0u) continue;
        assert(registers[i] == initial[i]);
    }
    free((void *)g_iris.pas_meta);
    printf("PASS %s reason=%llu code=%llu shutdown=%u writes=%u\n", case_name,
        (unsigned long long)return_reason, (unsigned long long)return_code, shutdown_calls, mmio_writes);
    return 0;
}
'''

CASES = ['never_started', 'smmu_only', 'pending_auth', 'authed_only', 'success',
         'previously_shutdown', 'second_return', 'shutdown_flag_recovery',
         'partial_setup', 'transport_fail', 'firmware_fail', 'answer_missing',
         'fault_non_pixel', 'fault_pixel', 'smr_non_pixel', 'smr_pixel',
         'wrong_route', 'bad_mapping', 'bad_geometry', 'sctlr_invalid',
         'sctlr_rejected', 'tlb_timeout', 'tlb_delayed', 'auth_success',
         'auth_init_fail', 'auth_mem_fail', 'auth_transport_fail', 'auth_firmware_fail']


def function(source, name):
    match = re.search(r'(?:static )?(?:bool|void)\n' + re.escape(name) + r'\(void\)\n\{', source)
    if not match:
        # iris_pas_call has arguments on one line.
        match = re.search(r'static bool\n' + re.escape(name) + r'\([^\n]*\)\n\{', source)
    if not match:
        raise AssertionError('Source boundary changed: ' + name)
    end = source.index('\n}\n', match.end()) + 3
    return source[match.start():end]


def run(root, hardware, returning, directory):
    directory.mkdir(parents=True, exist_ok=True)
    types = hardware[hardware.index('struct iris_mbn_seg {'):hardware.index('static inline uint32_t')]
    body = '\n'.join(function(hardware, name) for name in
        ['iris_pas_call', 'iris_pas_authenticate', 'iris_smmu_return_quiesce', 'annibale_iris_return_quiesce'])
    # ARM barriers are represented by host fences; instruction encoding is checked by the kernel build.
    body = body.replace('__asm__ volatile("dsb sy" ::: "memory");',
                        '__atomic_thread_fence(__ATOMIC_SEQ_CST);')
    source = HARNESS.replace('__MBN_TYPES__', types).replace('__HW_FUNCTIONS__', body)
    source = source.replace('__RETURN_FUNCTION__', function(returning, 'loader_return'))
    path, binary = directory / 'fixture.c', directory / 'fixture'
    path.write_text(source)
    compiled = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-I' + str(root / 'xnu/files/bsd'), '-I' + str(root / HW.parent),
        str(path), '-o', str(binary)], capture_output=True, text=True, timeout=60)
    (directory / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise AssertionError('Actual Iris return did not compile: ' + str(directory / 'compile.log'))
    cases = []
    for name in CASES:
        result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=5)
        (directory / (name + '.log')).write_text(result.stdout + result.stderr)
        cases.append({'name': name, 'rc': result.returncode, 'passed': result.returncode == 0})
    return cases


def verify(args, directory):
    hardware, returning = (args.source_root / HW).read_text(), (args.source_root / RETURN).read_text()
    results = {'hardware_sha256': hashlib.sha256(hardware.encode()).hexdigest(),
               'return_sha256': hashlib.sha256(returning.encode()).hexdigest(), 'variants': {}}
    results['variants']['candidate'] = run(args.source_root, hardware, returning, directory / 'candidate')
    failures = [c['name'] for c in results['variants']['candidate'] if not c['passed']]
    if args.baseline_ref:
        original = subprocess.check_output(['git', '-C', str(args.source_root), 'show',
            args.baseline_ref + ':' + str(RETURN)], text=True)
        results['variants']['original_return'] = run(args.source_root, hardware, original, directory / 'original_return')
        if all(c['passed'] for c in results['variants']['original_return']): failures.append('original_return_accepted')
    if args.mutations:
        variants = {
            'omit_shutdown': ('if (g_iris.pas_shutdown_required || g_iris.pas_authed)', 'if (false)'),
            'ignore_transport': ('if (st != 0 || ans != 0)', 'if (ans != 0)'),
            'ignore_firmware_result': ('if (st != 0 || ans != 0)', 'if (st != 0)'),
            'ready_only_cleanup': ('if (!g_iris.smmu_owned)', 'if (!g_iris.smmu_ready)'),
            'early_tlb_success': ('if ((mmio_r32(g_iris.smmu, cb_off + IRIS_CB_TLBSTATUS) & 1u) == 0)', 'if (true)'),
            'discard_dma_retention': ('if (g_iris.dma_shutdown_unconfirmed)', 'if (false)'),
            'clear_dma_after_pas': ('g_iris.pas_shutdown_required = false;',
                                    'g_iris.pas_shutdown_required = false; g_iris.dma_shutdown_unconfirmed = false;'),
        }
        for name, (old, new) in variants.items():
            if hardware.count(old) < 1: raise AssertionError('Mutation boundary changed: ' + name)
            if name == 'early_tlb_success':
                begin = hardware.index('iris_smmu_return_quiesce(void)')
                modified = hardware[:begin] + hardware[begin:].replace(old, new, 1)
            else:
                modified = hardware.replace(old, new, 1)
            cases = run(args.source_root, modified, returning, directory / name)
            results['variants'][name] = cases
            if all(c['passed'] for c in cases): failures.append(name + '_accepted')
        old = '\tg_iris.pas_shutdown_required = true;\n\tg_iris.dma_shutdown_unconfirmed = true;\n\t__asm__ volatile("dsb sy" ::: "memory");\n'
        if hardware.count(old) != 1: raise AssertionError('Authentication boundary changed')
        modified = hardware.replace(old, '', 1)
        name = 'auth_request_unrecorded'
        results['variants'][name] = run(args.source_root, modified, returning, directory / name)
        if all(c['passed'] for c in results['variants'][name]): failures.append(name + '_accepted')
        old = 'loader_jump(iris_quiesced ? 0 : 3, iris_quiesced ? 0 : 9);'
        if returning.count(old) != 1: raise AssertionError('Loader reason boundary changed')
        modified = returning.replace(old, '(void)iris_quiesced; loader_jump(0, 0);')
        name = 'discard_resource_failure'
        results['variants'][name] = run(args.source_root, hardware, modified, directory / name)
        if all(c['passed'] for c in results['variants'][name]): failures.append(name + '_accepted')
    (directory / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    if failures: raise AssertionError('Iris return regression failed: ' + ', '.join(failures))
    print(f'PASS: {len(CASES)} actual PAS/SMMU/loader return cases')
    if args.baseline_ref:
        count = sum(not c['passed'] for c in results['variants']['original_return'])
        print(f'PASS: original loader return rejected in {count} cases')
    if args.mutations: print('PASS: 9 compiled protection variants rejected')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--baseline-ref')
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if args.output_dir:
        verify(args, args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix='iris-return-') as directory:
            verify(args, Path(directory))


if __name__ == '__main__':
    main()

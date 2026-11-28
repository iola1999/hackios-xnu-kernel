#!/usr/bin/env python3
"""Host ASan+UBSan test for bcopy_phys_internal copy-window preemption safety."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PATCH_PATH = ROOT / 'xnu26/patches/84-osfmk%arm64%loose_ends.c.patch'
LOOSE_ENDS = ROOT / 'xnu-src/xnu-12377.121.6/osfmk/arm64/loose_ends.c'


def extract_bcopy_phys_internal(source_text):
    start = source_text.index('#define BCOPY_PHYS_SRC_IS_PHYS')
    end = source_text.index('\nvoid\nbcopy_phys(', start)
    return source_text[start:end]


HARNESS_PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t addr64_t;
typedef uint64_t pmap_paddr_t;
typedef uint64_t user_addr_t;
typedef size_t vm_size_t;
typedef uintptr_t vm_offset_t;
typedef uint32_t ppnum_t;
typedef int kern_return_t;
typedef bool boolean_t;

#define TRUE true
#define FALSE false
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
#define PAGE_SHIFT 14
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1u)
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_WIMG_MASK 0xffu
#define VM_WIMG_DEFAULT 0x2u
#define VM_WIMG_WCOMB 0x6u
#define cppvPsnk 0x1
#define cppvPsrc 0x2
#define cppvKmap 0x40
#define __assert_only __attribute__((unused))
#define __improbable(x) (x)
#define os_add_overflow(a, b, out) __builtin_add_overflow((a), (b), (out))

#define NUM_PAGES 4
static uint8_t g_ram[NUM_PAGES * PAGE_SIZE];
static int g_preemption_level = 0;
static int g_window_open = 0;
static int g_window_maps = 0;
static int g_copyin_calls = 0;
static int g_copyout_calls = 0;
static int g_fail_copyin_on = -1;
static int g_fail_copyout_on = -1;
static bool g_valid_addr = true;
static bool g_wpreflight_ok = true;
static unsigned int g_cache_attr = VM_WIMG_DEFAULT;

static void mp_disable_preemption(void) { g_preemption_level++; }
static void mp_enable_preemption(void) {
    assert(g_preemption_level > 0);
    g_preemption_level--;
}
static unsigned int cpu_number(void) { return 0; }
static bool pmap_valid_address(pmap_paddr_t pa) {
    return g_valid_addr && pa < sizeof(g_ram);
}
static uintptr_t phystokv(pmap_paddr_t pa) { return (uintptr_t)(g_ram + pa); }
static uintptr_t phystokv_range(pmap_paddr_t pa, vm_size_t *max_len) {
    vm_size_t rem = sizeof(g_ram) - (size_t)pa;
    if (*max_len > rem) *max_len = rem;
    return (uintptr_t)(g_ram + pa);
}
static int mmu_kvtop_wpreflight(uintptr_t va) {
    (void)va;
    return g_wpreflight_ok ? 1 : 0;
}
static unsigned int pmap_cache_attributes(ppnum_t pn) {
    (void)pn;
    return g_cache_attr;
}
static unsigned int pmap_map_cpu_windows_copy(ppnum_t pn, int prot, unsigned int wimg) {
    (void)prot;
    (void)wimg;
    assert(g_preemption_level > 0);
    assert(pn < NUM_PAGES);
    g_window_open++;
    g_window_maps++;
    return pn;
}
static uintptr_t pmap_cpu_windows_copy_addr(unsigned int cpu, unsigned int idx) {
    (void)cpu;
    assert(g_preemption_level > 0);
    assert(idx < NUM_PAGES);
    return (uintptr_t)(g_ram + ((size_t)idx << PAGE_SHIFT));
}
static void pmap_unmap_cpu_windows_copy(unsigned int idx) {
    (void)idx;
    assert(g_preemption_level > 0);
    assert(g_window_open > 0);
    g_window_open--;
}
static int copyin(user_addr_t src, void *dst, vm_size_t len) {
    g_copyin_calls++;
    /* Mirrors vm_fault_internal() returning KERN_FAILURE -> EFAULT when preemption is disabled. */
    if (g_preemption_level != 0) return EFAULT;
    if (g_fail_copyin_on == g_copyin_calls) return EFAULT;
    memcpy(dst, (const void *)(uintptr_t)src, len);
    return 0;
}
static int copyout(const void *src, user_addr_t dst, vm_size_t len) {
    g_copyout_calls++;
    if (g_preemption_level != 0) return EFAULT;
    if (g_fail_copyout_on == g_copyout_calls) return EFAULT;
    memcpy((void *)(uintptr_t)dst, src, len);
    return 0;
}
'''

HARNESS_MAIN = r'''
static void reset_state(bool valid, bool wpreflight, unsigned int wimg) {
    memset(g_ram, 0xaa, sizeof(g_ram));
    g_preemption_level = 0;
    g_window_open = 0;
    g_window_maps = 0;
    g_copyin_calls = 0;
    g_copyout_calls = 0;
    g_fail_copyin_on = -1;
    g_fail_copyout_on = -1;
    g_valid_addr = valid;
    g_wpreflight_ok = wpreflight;
    g_cache_attr = wimg;
}

int main(void) {
    uint8_t user_src[1024];
    uint8_t user_dst[1024];
    for (size_t i = 0; i < sizeof(user_src); i++) {
        user_src[i] = (uint8_t)((i * 37u + 13u) & 0xffu);
    }

    /* 1. User -> Phys (60 B, locationd case) with wpreflight=0 */
    reset_state(true, false, VM_WIMG_DEFAULT);
    assert(bcopy_phys_internal((addr64_t)(uintptr_t)user_src, 0, 60, cppvPsnk) == KERN_SUCCESS);
    assert(memcmp(g_ram, user_src, 60) == 0);
    assert(g_preemption_level == 0 && g_window_open == 0 && g_window_maps == 1);

    /* 2. User -> Phys multi-chunk + page boundary with non-default WIMG */
    reset_state(true, true, VM_WIMG_WCOMB);
    size_t cross_dst = PAGE_SIZE - 40;
    assert(bcopy_phys_internal((addr64_t)(uintptr_t)user_src, cross_dst, 700, cppvPsnk) == KERN_SUCCESS);
    assert(memcmp(g_ram + cross_dst, user_src, 700) == 0);
    assert(g_preemption_level == 0 && g_window_open == 0 && g_window_maps >= 3);

    /* 3. Phys -> User multi-chunk + page boundary with !pmap_valid_address */
    reset_state(false, true, VM_WIMG_DEFAULT);
    memcpy(g_ram + PAGE_SIZE - 40, user_src, 700);
    memset(user_dst, 0, sizeof(user_dst));
    assert(bcopy_phys_internal(PAGE_SIZE - 40, (addr64_t)(uintptr_t)user_dst, 700, cppvPsrc) == KERN_SUCCESS);
    assert(memcmp(user_dst, user_src, 700) == 0);
    assert(g_preemption_level == 0 && g_window_open == 0 && g_window_maps >= 3);

    /* 4. copyin EFAULT on 2nd chunk leaves preemption and windows clean */
    reset_state(true, false, VM_WIMG_DEFAULT);
    g_fail_copyin_on = 2;
    assert(bcopy_phys_internal((addr64_t)(uintptr_t)user_src, 0, 600, cppvPsnk) == EFAULT);
    assert(g_preemption_level == 0 && g_window_open == 0 && g_window_maps == 1);

    /* 5. copyout EFAULT on 2nd chunk leaves preemption and windows clean */
    reset_state(false, true, VM_WIMG_DEFAULT);
    g_fail_copyout_on = 2;
    assert(bcopy_phys_internal(0, (addr64_t)(uintptr_t)user_dst, 600, cppvPsrc) == EFAULT);
    assert(g_preemption_level == 0 && g_window_open == 0 && g_window_maps == 2);

    /* 6. Fast path (no copy window) for both directions */
    reset_state(true, true, VM_WIMG_DEFAULT);
    assert(bcopy_phys_internal((addr64_t)(uintptr_t)user_src, 128, 300, cppvPsnk) == KERN_SUCCESS);
    assert(memcmp(g_ram + 128, user_src, 300) == 0);
    assert(g_window_maps == 0 && g_preemption_level == 0);

    memset(user_dst, 0, sizeof(user_dst));
    assert(bcopy_phys_internal(128, (addr64_t)(uintptr_t)user_dst, 300, cppvPsrc) == KERN_SUCCESS);
    assert(memcmp(user_dst, user_src, 300) == 0);
    assert(g_window_maps == 0 && g_preemption_level == 0);

    /* 7. Phys -> Phys with copy window still works */
    reset_state(true, false, VM_WIMG_WCOMB);
    memcpy(g_ram + 64, user_src, 512);
    assert(bcopy_phys_internal(64, PAGE_SIZE + 64, 512, cppvPsrc | cppvPsnk) == KERN_SUCCESS);
    assert(memcmp(g_ram + PAGE_SIZE + 64, user_src, 512) == 0);
    assert(g_preemption_level == 0 && g_window_open == 0 && g_window_maps >= 2);

    printf("BCOPY_PHYS_PREEMPT_OK\n");
    return 0;
}
'''


class BcopyPhysCopyinPreemptTest(unittest.TestCase):
    def test_patch_exists_and_matches_xnu_src(self):
        self.assertTrue(PATCH_PATH.is_file(), msg=f'missing {PATCH_PATH}')
        patch_text = PATCH_PATH.read_text()
        self.assertIn('--- a/osfmk/arm64/loose_ends.c', patch_text)
        self.assertIn('+++ b/osfmk/arm64/loose_ends.c', patch_text)
        self.assertIn('use_copy_window_dst && BCOPY_PHYS_SRC_IS_USER(flags)', patch_text)
        self.assertIn('use_copy_window_src && BCOPY_PHYS_DST_IS_USER(flags)', patch_text)

    def test_bcopy_phys_internal_asan_ubsan(self):
        if not LOOSE_ENDS.is_file():
            self.skipTest('xnu-src not unpacked')
        body = extract_bcopy_phys_internal(LOOSE_ENDS.read_text())
        self.assertIn('use_copy_window_dst && BCOPY_PHYS_SRC_IS_USER(flags)', body)
        self.assertIn('use_copy_window_src && BCOPY_PHYS_DST_IS_USER(flags)', body)

        with tempfile.TemporaryDirectory(prefix='bcopy-phys-preempt-') as td:
            src = Path(td) / 'harness.c'
            exe = Path(td) / 'harness'
            src.write_text(HARNESS_PREFIX + '\n' + body + '\n' + HARNESS_MAIN)
            subprocess.run(
                ['xcrun', '--sdk', 'macosx', 'clang', '-O1', '-g',
                 '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 str(src), '-o', str(exe)],
                check=True,
            )
            out = subprocess.check_output([str(exe)], text=True, timeout=15)
            self.assertIn('BCOPY_PHYS_PREEMPT_OK', out)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Host ASan+UBSan test for adreno8_ifpc=1 Phase 2 OOB_GPU release and re-acquire."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEV = ROOT / "xnu/files/bsd/dev/adreno8"

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uintptr_t vm_offset_t;

#define GMU(r) ((r) * 4u)
#define GMU_GMU2HOST_CLR 0x100u
#define GMU_GMU2HOST_INFO 0x104u
#define GMU_HOST2GMU_SET 0x108u
#define GMU_PWR_CLK_STATUS 0x10cu
#define GMU_AHB_FENCE_STATUS 0x110u
#define SMMU_SGFSR 0x048u
#define GMU_PWR_GX_UP(v) (((v) & 1u) != 0)
#define ADRENO8_MEM_READ 1u
#define ADRENO8_MEM_WRITE 2u
#define ADRENO8_MEM_EXEC 4u
#define ADRENO8_MEM_PRIV 8u
#define ADRENO8_SECURE_IOVA_START 0xf0000000u

struct adreno8_block { vm_offset_t base; uint64_t phys; uint32_t size; };
struct adreno8_memory { vm_offset_t memory; uint32_t gpu_address; uint32_t size; };
struct adreno8_space { uint32_t context; bool is_on; };

static uint32_t gmu_regs[0x30000];
static uint32_t gx_regs[0x20000];
static bool gx_powered = true;
static bool oob_ack_enabled = true;
static unsigned gx_accesses, oob_set_writes, oob_clr_writes;

struct adreno8_block adreno8_gpu = { (vm_offset_t)gx_regs, 0x3d00000, sizeof(gx_regs) };
struct adreno8_block adreno8_gmu = { (vm_offset_t)gmu_regs, 0x3d6a000, sizeof(gmu_regs) };
struct adreno8_block adreno8_smmu;
struct adreno8_space adreno8_gpu_space;

bool adreno8_direct_ring = false;
bool adreno8_gpu8_keep = true;
bool adreno8_gpu8_ifpc = false;
bool adreno8_gpu8_return = false;
bool adreno8_reset_required = false;
unsigned adreno8_step = 17, adreno8_newest_step = 17, adreno8_pause_ms = 0, adreno8_skip_steps = 0;

static int adreno8_say(const char *fmt, ...) { (void)fmt; return 0; }
void loader_log_line(const char *line) { (void)line; }
static void adreno8_settle(void) {}
static void adreno8_sleep_us(unsigned us) { (void)us; }
static void adreno8_hold(bool x) { (void)x; }
static bool PE_parse_boot_argn(const char *k, void *v, unsigned s) { (void)k; (void)v; (void)s; return false; }

static uint32_t adreno8_rd(vm_offset_t addr) {
    if (addr >= (vm_offset_t)gx_regs && addr < (vm_offset_t)(gx_regs + 0x20000)) {
        assert(gx_powered && "GX register read while GX is collapsed!");
        gx_accesses++;
        return gx_regs[(addr - (vm_offset_t)gx_regs) / 4];
    }
    if (addr >= (vm_offset_t)gmu_regs && addr < (vm_offset_t)(gmu_regs + 0x30000)) {
        uint32_t off = (uint32_t)(addr - (vm_offset_t)gmu_regs);
        if (off == GMU_PWR_CLK_STATUS) return gx_powered ? 1u : 0u;
        return gmu_regs[off / 4];
    }
    assert(false);
    return 0;
}

static void adreno8_wr(vm_offset_t addr, uint32_t val) {
    if (addr >= (vm_offset_t)gx_regs && addr < (vm_offset_t)(gx_regs + 0x20000)) {
        assert(gx_powered && "GX register write while GX is collapsed!");
        gx_accesses++;
        gx_regs[(addr - (vm_offset_t)gx_regs) / 4] = val;
        return;
    }
    if (addr >= (vm_offset_t)gmu_regs && addr < (vm_offset_t)(gmu_regs + 0x30000)) {
        uint32_t off = (uint32_t)(addr - (vm_offset_t)gmu_regs);
        gmu_regs[off / 4] = val;
        if (off == GMU_HOST2GMU_SET && val == (1u << 30)) {
            oob_set_writes++;
            if (oob_ack_enabled) {
                gmu_regs[GMU_GMU2HOST_INFO / 4] |= (1u << 31);
                gx_powered = true;
            }
        } else if (off == GMU_HOST2GMU_SET && val == (1u << 31)) {
            oob_clr_writes++;
            gx_powered = false;
        } else if (off == GMU_GMU2HOST_CLR) {
            gmu_regs[GMU_GMU2HOST_INFO / 4] &= ~val;
        }
        return;
    }
    assert(false);
}

static uint32_t adreno8_get(const struct adreno8_block *b, const char *n, uint32_t o) { (void)b; (void)n; (void)o; return 0; }
static void adreno8_put(const struct adreno8_block *b, const char *n, uint32_t o, uint32_t v) { (void)b; (void)n; (void)o; (void)v; }
static bool adreno8_ordinary_ready(void) { return false; }
static bool adreno8_hwsched_ready(void) { return true; }
static bool adreno8_gpu_memory_ready(void) { return true; }
static bool adreno8_gpu_faulted(void) { return false; }
static bool adreno8_hfi_poll(void) { return true; }
static bool adreno8_hwsched_raw(const uint32_t *p, uint32_t n, const char *w) { (void)p; (void)n; (void)w; return true; }
static struct adreno8_memory *adreno8_gpu_alloc(const char *n, uint32_t s, uint32_t a, uint32_t f) { (void)n; (void)s; (void)a; (void)f; return NULL; }
static bool adreno8_memory_publish(struct adreno8_memory *m, uint32_t o, uint32_t s) { (void)m; (void)o; (void)s; return true; }
static bool adreno8_memory_acquire(struct adreno8_memory *m, uint32_t o, uint32_t s) { (void)m; (void)o; (void)s; return true; }
static bool adreno8_translation_walk(struct adreno8_space *sp, uint32_t va, vm_offset_t m, uint32_t s, uint32_t f) { (void)sp; (void)va; (void)m; (void)s; (void)f; return true; }
static const uint8_t *annibale_image_file(const char *d, const char *n, uint64_t *s, int *k) { (void)d; (void)n; (void)s; (void)k; return NULL; }
static bool adreno8_space_faulted(struct adreno8_space *sp) { (void)sp; return false; }
void adreno8_context_describe_inflight(void) {}
void adreno8_hfi_hang_dump(void) {}
void adreno8_bo_dump_tree(uint64_t b, uint32_t i) { (void)b; (void)i; }
void adreno8_bo_describe_owner(const char *n, uint32_t a) { (void)n; (void)a; }
void adreno8_bo_dump_words(const char *w, uint64_t a, uint32_t c) { (void)w; (void)a; (void)c; }

#define _DEV_ADRENO8_PRIVATE_H
#define _DEV_ADRENO8_MEMORY_PRIVATE_H
#define _DEV_ADRENO8_HWSCHED_PRIVATE_H
#define _DEV_ADRENO8_CONTEXT_PRIVATE_H
#define _DEV_ADRENO8_BO_PRIVATE_H
#include "adreno8_gpu.c"

int main(void) {
    /* 1. Default adreno8_ifpc=0: release is a no-op, GX stays powered and checked. */
    gpu_state = GPU_TESTED;
    oob_requested = oob_acked = oob_held = true;
    gx_powered = true;
    adreno8_gpu8_ifpc = false;
    adreno8_gpu_oob_release();
    assert(oob_clr_writes == 0 && oob_held == true && gx_powered == true);
    gx_accesses = 0;
    assert(adreno8_gpu_commands_ready());
    assert(gx_accesses > 0);

    /* 2. adreno8_ifpc=1: release clears OOB (bit 31); GX collapses; ready/wait_idle do NOT touch GX. */
    adreno8_gpu8_ifpc = true;
    adreno8_gpu_oob_release();
    assert(oob_clr_writes == 1 && oob_held == false && gx_powered == false);
    gx_accesses = 0;
    assert(adreno8_gpu_commands_ready());
    assert(adreno8_gpu_wait_idle("ifpc collapsed idle"));
    assert(gx_accesses == 0);

    /* 3. Re-acquire fails if GMU does not ACK: hang dump must not touch GX. */
    oob_ack_enabled = false;
    assert(!adreno8_gpu_oob_acquire());
    assert(oob_set_writes == 1 && oob_held == false && gx_powered == false);
    assert(gx_accesses == 0);

    /* 4. Re-acquire succeeds inside adreno8_gpu_hang_dump before touching GX registers. */
    oob_ack_enabled = true;
    adreno8_gpu_hang_dump();
    assert(oob_set_writes == 2 && oob_held == true && gx_powered == true);
    assert(gx_accesses > 0);

    puts("adreno8 IFPC Phase 2 OOB_GPU release/re-acquire/collapsed-GX guard: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="adreno8-ifpc-oob-") as d:
    src = Path(d) / "test.c"
    exe = Path(d) / "test"
    src.write_text(HARNESS)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-parameter",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(DEV), str(src), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)

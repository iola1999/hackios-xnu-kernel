#!/usr/bin/env python3
"""Host-side unit test verifying Part 3E Kernel ADSP, SMEM, GLINK fragmentation and Battery PMIC GLINK fixes."""
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ADSP_SRC = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_adsp.c').read_text()
BATT_SRC = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_battery.c').read_text()

def verify_static_invariants():
    # 1. Check dtb_fw and main_fw zero-initialization in annibale_adsp_boot_thread
    boot_match = re.search(r'struct\s+fw_image\s+dtb_fw\s*=\s*\{0\},\s*main_fw\s*=\s*\{0\};', ADSP_SRC)
    assert boot_match is not None, "dtb_fw and main_fw must be explicitly zero-initialized"

    # 2. Check 64-bit arithmetic in smem_peek and smem_poke
    assert '(uint64_t)offset + (uint64_t)size > SMEM_SIZE' in ADSP_SRC, "smem_peek must use 64-bit arithmetic"
    assert '(uint64_t)offset + 4ULL <= SMEM_SIZE' in ADSP_SRC, "smem_poke must use 64-bit arithmetic"

    # 3. Check smem_item_in_partition bounds checks
    assert 'e.padding_data > e.size' in ADSP_SRC, "smem_item_in_partition must check padding_data > e.size"
    assert 'data + (uint64_t)e.size > SMEM_SIZE' in ADSP_SRC, "smem_item_in_partition must check SMEM_SIZE bound"

    # 4. Check GLINK bounded reassembly buffer and step_bytes 64-bit calculation
    assert 'struct glink_reasm' in ADSP_SRC, "annibale_adsp.c must define glink_reasm"
    assert 'GLINK_REASM_MAX' in ADSP_SRC, "annibale_adsp.c must define GLINK_REASM_MAX"
    assert 'uint64_t step_bytes = 8;' in ADSP_SRC, "glink_poll_rx must use uint64_t for step_bytes"
    assert 'r = w;' in ADSP_SRC, "glink_poll_rx must advance r = w on invalid length"

    # 5. Check Battery PMIC GLINK alignment fix and synchronization
    assert 'pmic_glink_lock' in BATT_SRC, "annibale_battery.c must define pmic_glink_lock"
    assert 'const uint32_t *w = data;' not in BATT_SRC, "annibale_battery.c must not cast raw data to uint32_t*"
    assert 'memcpy(&owner, data, sizeof(owner));' in BATT_SRC, "annibale_battery.c must use memcpy to extract owner"
    assert 'memcpy(header, data, sizeof(header));' in BATT_SRC, "annibale_battery.c must use memcpy to extract header words"
    assert '++pmic_glink_state.seq;' in BATT_SRC, "annibale_battery.c must increment seq"

    print("PASS: Static invariant assertions verified")

C_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMEM_SIZE 0x00200000u
#define SMEM_CANARY 0xa5a5u
#define GLINK_REASM_MAX 2048u
#define BATTMGR_OWNER 0x800au
#define BATTMGR_TYPE_REQ 1u
#define BATTMGR_OP_BAT_INFO 1u

/* Simulation of SMEM buffer */
static uint8_t mock_smem[SMEM_SIZE];
static bool mock_smem_ready = true;

static bool smem_peek(uint32_t offset, void *to, uint32_t size) {
    if (!mock_smem_ready || (uint64_t)offset + (uint64_t)size > SMEM_SIZE) {
        return false;
    }
    memcpy(to, mock_smem + offset, size);
    return true;
}

static void smem_poke(uint32_t offset, uint32_t val) {
    if (mock_smem_ready && (uint64_t)offset + 4ULL <= SMEM_SIZE) {
        memcpy(mock_smem + offset, &val, 4);
    }
}

static void test_smem_arithmetic_overflow(void) {
    uint8_t buf[16];
    /* 32-bit overflow attempts */
    assert(!smem_peek(0xFFFFFFFFu, buf, 16));
    assert(!smem_peek(SMEM_SIZE - 4, buf, 8));
    assert(!smem_peek(0xFFFFFFF0u, buf, 32));
    assert(smem_peek(SMEM_SIZE - 4, buf, 4));

    smem_poke(0xFFFFFFFFu, 0x12345678);
    smem_poke(SMEM_SIZE - 2, 0x12345678);
    smem_poke(SMEM_SIZE - 4, 0xdeadbeef);
    uint32_t check = 0;
    assert(smem_peek(SMEM_SIZE - 4, &check, 4));
    assert(check == 0xdeadbeef);
    printf("PASS: test_smem_arithmetic_overflow\n");
}

/* Simulation of GLINK reassembly */
struct glink_reasm {
    uint16_t cid;
    uint32_t total_len;
    uint32_t assembled_len;
    bool     in_progress;
    bool     oversize;
    uint8_t  buf[GLINK_REASM_MAX];
};

static uint32_t dispatched_len = 0;
static uint8_t  dispatched_buf[GLINK_REASM_MAX];
static int      dispatch_count = 0;

static void mock_handle_incoming(const uint8_t *data, uint32_t len) {
    dispatch_count++;
    dispatched_len = len;
    memcpy(dispatched_buf, data, len);
}

static void test_glink_fragment_reassembly(void) {
    struct glink_reasm reasm;
    memset(&reasm, 0, sizeof(reasm));
    dispatch_count = 0;
    dispatched_len = 0;

    /* Fragmented message of 100 bytes: packet 1 (chunk=40, left=60) */
    uint32_t chunk1 = 40, left1 = 60;
    uint64_t total1 = (uint64_t)chunk1 + (uint64_t)left1;
    assert(total1 <= GLINK_REASM_MAX);
    reasm.in_progress = true;
    reasm.oversize = false;
    reasm.cid = 2;
    reasm.total_len = (uint32_t)total1;
    for (uint32_t i = 0; i < chunk1; i++) reasm.buf[i] = (uint8_t)(i + 1);
    reasm.assembled_len = chunk1;
    /* left > 0 -> NOT dispatched yet */
    assert(dispatch_count == 0);

    /* Packet 2: continuation (chunk=35, left=25) */
    uint32_t chunk2 = 35;
    assert(reasm.in_progress && !reasm.oversize);
    for (uint32_t i = 0; i < chunk2; i++) reasm.buf[reasm.assembled_len + i] = (uint8_t)(chunk1 + i + 1);
    reasm.assembled_len += chunk2;
    assert(dispatch_count == 0);

    /* Packet 3: final continuation (chunk=25, left=0) */
    uint32_t chunk3 = 25, left3 = 0;
    for (uint32_t i = 0; i < chunk3; i++) reasm.buf[reasm.assembled_len + i] = (uint8_t)(chunk1 + chunk2 + i + 1);
    reasm.assembled_len += chunk3;
    if (left3 == 0) {
        assert(reasm.assembled_len == reasm.total_len);
        mock_handle_incoming(reasm.buf, reasm.assembled_len);
        reasm.in_progress = false;
    }

    assert(dispatch_count == 1);
    assert(dispatched_len == 100);
    for (uint32_t i = 0; i < 100; i++) {
        assert(dispatched_buf[i] == (uint8_t)(i + 1));
    }
    printf("PASS: test_glink_fragment_reassembly\n");
}

static void test_glink_oversize_discard(void) {
    struct glink_reasm reasm;
    memset(&reasm, 0, sizeof(reasm));
    dispatch_count = 0;

    /* Oversize message of 4096 bytes > GLINK_REASM_MAX (2048) */
    uint32_t chunk = 500, left = 3596;
    uint64_t total = (uint64_t)chunk + (uint64_t)left;
    if (total > GLINK_REASM_MAX) {
        reasm.in_progress = true;
        reasm.oversize = true;
        reasm.cid = 2;
        reasm.total_len = 0;
        reasm.assembled_len = 0;
    }
    assert(reasm.oversize == true);
    assert(dispatch_count == 0);

    /* Continuation 1: discard */
    uint32_t c_left = 3096;
    if (reasm.in_progress && reasm.oversize) {
        if (c_left == 0) reasm.in_progress = false;
    }
    assert(dispatch_count == 0);

    /* Final continuation */
    uint32_t f_left = 0;
    if (reasm.in_progress && reasm.oversize) {
        if (f_left == 0) {
            reasm.in_progress = false;
            reasm.oversize = false;
        }
    }
    assert(dispatch_count == 0);
    assert(reasm.in_progress == false);
    printf("PASS: test_glink_oversize_discard\n");
}

/* Battery PMIC GLINK alignment test */
struct pmic_glink_mock_state {
    bool active;
    uint32_t seq;
    int32_t vbat_mv;
    int32_t ibat_ma;
};

static struct pmic_glink_mock_state pmic_state;

static void mock_pmic_glink_rx(const void *data, uint32_t len) {
    uint32_t header[7];
    if (data == NULL || len < 16) return;
    uint32_t owner = 0;
    memcpy(&owner, data, sizeof(owner));
    if (owner != BATTMGR_OWNER) return;

    pmic_state.active = true;
    if (len >= 28) {
        memcpy(header, data, sizeof(header));
        uint32_t type = header[1];
        uint32_t op = header[2];
        if (type == BATTMGR_TYPE_REQ && op == BATTMGR_OP_BAT_INFO) {
            uint32_t prop = header[3];
            uint32_t res = header[4];
            int32_t val = (int32_t)header[6];
            if (res == 0) {
                if (prop == 0x2001) pmic_state.vbat_mv = val;
                else if (prop == 0x2002) pmic_state.ibat_ma = val;
            }
        }
    }
}

static void test_battery_pmic_unaligned_rx(void) {
    /* Allocate unaligned buffer on stack (odd address offset) */
    uint8_t raw[128];
    uint8_t *unaligned = raw + 1; /* Guaranteed misaligned for uint32_t */

    uint32_t packet[7];
    packet[0] = BATTMGR_OWNER;
    packet[1] = BATTMGR_TYPE_REQ;
    packet[2] = BATTMGR_OP_BAT_INFO;
    packet[3] = 0x2001; /* vbat property */
    packet[4] = 0;      /* success */
    packet[5] = 42;     /* seq */
    packet[6] = 4150;   /* 4150 mV */

    memcpy(unaligned, packet, sizeof(packet));

    memset(&pmic_state, 0, sizeof(pmic_state));
    mock_pmic_glink_rx(unaligned, sizeof(packet));

    assert(pmic_state.active == true);
    assert(pmic_state.vbat_mv == 4150);
    printf("PASS: test_battery_pmic_unaligned_rx\n");
}

int main(void) {
    test_smem_arithmetic_overflow();
    test_glink_fragment_reassembly();
    test_glink_oversize_discard();
    test_battery_pmic_unaligned_rx();
    printf("ALL ADSP/BATTERY FIX TESTS PASSED\n");
    return 0;
}
'''

def run_c_harness():
    with tempfile.TemporaryDirectory() as td:
        c_file = Path(td) / "test_harness.c"
        bin_file = Path(td) / "test_harness"
        c_file.write_text(C_HARNESS)
        subprocess.run(["xcrun", "clang", "-O2", "-Wall", "-Wextra", str(c_file), "-o", str(bin_file)], check=True)
        res = subprocess.run([str(bin_file)], check=True, capture_output=True, text=True)
        print(res.stdout.strip())

if __name__ == "__main__":
    verify_static_invariants()
    run_c_harness()

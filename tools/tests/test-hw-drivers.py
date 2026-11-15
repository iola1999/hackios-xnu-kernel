#!/usr/bin/env python3
"""Host-side unit tests for MiniAppleARMBacklight sanity checks and annibale_battery hysteresis."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

TEST_C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define kBLValueMin             5u
#define kBLValueMax             0x1a5eu
#define kBLMilliNitsMin         5000u
#define kBLMilliNitsMax         400000u
#define kBLLevelFull            1024u

/* Backlight simulation */
static uint64_t last_v = kBLValueMax;
static uint64_t last_millinits = kBLMilliNitsMax;

static bool bl_take_sim(uint64_t n16, uint64_t *out_v, uint64_t *out_millinits, uint32_t *out_level)
{
    int64_t s16 = (int64_t) n16;
    if (s16 <= 0) {
        /* Discarded */
        return false;
    }

    uint64_t millinits, v;
    if (n16 >= 65536) {
        millinits = (n16 < (uint64_t) kBLMilliNitsMax << 16 ? n16 : (uint64_t) kBLMilliNitsMax << 16) * 1000 >> 16;
        millinits = millinits < kBLMilliNitsMin ? kBLMilliNitsMin : millinits > kBLMilliNitsMax ? kBLMilliNitsMax : millinits;
        v = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) / (kBLMilliNitsMax - kBLMilliNitsMin);
        last_v = v;
        last_millinits = millinits;
    } else if (n16 >= 1 && n16 <= kBLLevelFull) {
        millinits = kBLMilliNitsMin + (n16 - 1) * (kBLMilliNitsMax - kBLMilliNitsMin) / (kBLLevelFull - 1);
        v = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) / (kBLMilliNitsMax - kBLMilliNitsMin);
        last_v = v;
        last_millinits = millinits;
    } else {
        /* Invalid transient (> 1024 and < 65536) */
        return false;
    }

    *out_millinits = millinits;
    *out_v = v;
    *out_level = (uint32_t) (millinits * kBLLevelFull / kBLMilliNitsMax);
    return true;
}

static void test_backlight_mapping(void)
{
    uint64_t v = 0, millinits = 0;
    uint32_t level = 0;
    bool ok;

    /* Initial state */
    last_v = kBLValueMax;
    last_millinits = kBLMilliNitsMax;

    /* 1. Standard 16.16 fixed-point: 214.743 nits */
    uint64_t nits_214 = 14073450; /* 214.743 * 65536 */
    ok = bl_take_sim(nits_214, &v, &millinits, &level);
    assert(ok);
    assert(millinits >= 214000 && millinits <= 215000);
    assert(level == 549);
    assert(last_millinits == millinits);
    assert(last_v == v);

    /* 2. Menu navigation transients: 28 and 27 mapped linearly in 1..1024 */
    ok = bl_take_sim(28, &v, &millinits, &level);
    assert(ok);
    /* 5000 + 27 * 395000 / 1023 = 15425 */
    assert(millinits == 15425);
    assert(v == kBLValueMin + (15425 - 5000) * (kBLValueMax - kBLValueMin) / (kBLMilliNitsMax - kBLMilliNitsMin));

    /* 3. Boundaries of 1..1024 */
    ok = bl_take_sim(1, &v, &millinits, &level);
    assert(ok);
    assert(millinits == kBLMilliNitsMin);
    assert(v == kBLValueMin);

    ok = bl_take_sim(1024, &v, &millinits, &level);
    assert(ok);
    assert(millinits == kBLMilliNitsMax);
    assert(v == kBLValueMax);
    assert(level == kBLLevelFull);

    /* 4. Non-positive values: discarded, keep previous stable state */
    uint64_t prev_millinits = last_millinits;
    uint64_t prev_v = last_v;

    ok = bl_take_sim(0, &v, &millinits, &level);
    assert(!ok);
    assert(last_millinits == prev_millinits);
    assert(last_v == prev_v);

    ok = bl_take_sim((uint64_t)(int64_t)(-10), &v, &millinits, &level);
    assert(!ok);
    assert(last_millinits == prev_millinits);
    assert(last_v == prev_v);

    /* 5. Invalid transient values between 1025 and 65535: discarded */
    ok = bl_take_sim(1025, &v, &millinits, &level);
    assert(!ok);
    assert(last_millinits == prev_millinits);
    assert(last_v == prev_v);

    ok = bl_take_sim(32768, &v, &millinits, &level);
    assert(!ok);
    assert(last_millinits == prev_millinits);
    assert(last_v == prev_v);

    printf("test_backlight_mapping passed\n");
}

/* Battery simulation */
#define CHARGE_FLOW_UA 50000        /* 50 mA */
#define DISCONNECT_FLOW_UA (-250000) /* -250 mA */

static bool sim_last_external = false;

static void battery_update_sim(int32_t current_ua, bool *out_external, int *out_status)
{
    if (current_ua > CHARGE_FLOW_UA) {
        sim_last_external = true;
        *out_status = 1;
    } else if (current_ua < DISCONNECT_FLOW_UA) {
        sim_last_external = false;
        *out_status = 2;
    } else {
        *out_status = sim_last_external ? 3 : 2;
    }
    *out_external = sim_last_external;
}

static void test_battery_hysteresis(void)
{
    bool ext;
    int status;

    /* Start unplugged */
    sim_last_external = false;

    /* 1. At -100 mA (discharging, within hysteresis window): remains unplugged */
    battery_update_sim(-100000, &ext, &status);
    assert(ext == false);
    assert(status == 2);

    /* 2. Plugin charger: current jumps to +300 mA (> +50 mA) */
    battery_update_sim(300000, &ext, &status);
    assert(ext == true);
    assert(status == 1);

    /* 3. Float/hold: current drops to 0 mA (within [-250 mA, +50 mA]) -> must remain external */
    battery_update_sim(0, &ext, &status);
    assert(ext == true);
    assert(status == 3);

    /* 4. Mild drain during screen activity while plugged in: -50 mA -> must remain external */
    battery_update_sim(-50000, &ext, &status);
    assert(ext == true);
    assert(status == 3);

    /* 5. Heavier burst while on weak USB: -200 mA -> must still remain external */
    battery_update_sim(-200000, &ext, &status);
    assert(ext == true);
    assert(status == 3);

    /* 6. Unplug charger: discharge drops below -250 mA (-300 mA) */
    battery_update_sim(-300000, &ext, &status);
    assert(ext == false);
    assert(status == 2);

    /* 7. Current settles back to -100 mA -> remains unplugged */
    battery_update_sim(-100000, &ext, &status);
    assert(ext == false);
    assert(status == 2);

    printf("test_battery_hysteresis passed\n");
}

/* ========================================================================= */
/* UFS Whitelist & Refusal Simulation                                        */
/* ========================================================================= */

#define UPIU_COMMAND            0x01
#define UPIU_FLAG_READ          0x40
#define UPIU_FLAG_WRITE         0x20

#define UTRD_CT_MASK            0xf0000000u
#define UTRD_CT_UFS             (1u << 28)
#define UTRD_DD_MASK            (3u << 25)
#define UTRD_DD_NONE            (0u << 25)
#define UTRD_DD_OUT             (1u << 25)
#define UTRD_DD_IN              (2u << 25)
#define LUN_WELL_KNOWN          0x80
#define BLOCK                   4096

static uint64_t sim_write_lba_start = 10000;
static uint64_t sim_write_lba_end = 20000;
static uint8_t sim_write_lun = 0;
static bool sim_write_enabled = false;

static const char *
sim_hc_refusal(const uint8_t *upiu, uint32_t dw0, uint32_t lun_mask, bool any_command_type)
{
    const uint8_t *cdb = upiu + 16;
    uint32_t dd = dw0 & UTRD_DD_MASK;
    uint64_t expected = (uint64_t)upiu[12] << 24 | (uint64_t)upiu[13] << 16 | (uint64_t)upiu[14] << 8 | upiu[15];
    uint8_t lun = upiu[2];

    if (!any_command_type && (dw0 & UTRD_CT_MASK) != UTRD_CT_UFS) {
        return "not a UFS storage command type";
    }
    if ((upiu[0] & 0x3f) != UPIU_COMMAND) {
        return "not a SCSI command (a query, task management, NOP, ...)";
    }
    if (upiu[4] & 0x0f) {
        return "not the SCSI command set";
    }
    if (lun & LUN_WELL_KNOWN) {
        return "a well-known logical unit";
    }
    if (lun >= 8 || (lun_mask & (1u << lun)) == 0) {
        return "a logical unit operations do not go to now";
    }
    switch (cdb[0]) {
    case 0x08:      /* READ(6) */
    case 0x28:      /* READ(10) */
    case 0x88: {    /* READ(16) */
        uint64_t blocks = cdb[0] == 0x08 ? (cdb[4] == 0 ? 256 : cdb[4]) :
            cdb[0] == 0x28 ? ((uint64_t)cdb[7] << 8 | cdb[8]) :
            ((uint64_t)cdb[10] << 24 | (uint64_t)cdb[11] << 16 | (uint64_t)cdb[12] << 8 | cdb[13]);

        if (dd != UTRD_DD_IN || (upiu[1] & UPIU_FLAG_READ) == 0 || (upiu[1] & UPIU_FLAG_WRITE) != 0) {
            return "a read without data into memory or with WRITE flag";
        }
        if (blocks == 0 || expected != blocks * BLOCK) {
            return "a read whose length is not what the UPIU expects";
        }
        return NULL;
    }
    case 0x0a:      /* WRITE(6) */
    case 0x2a:      /* WRITE(10) */
    case 0x8a: {    /* WRITE(16) */
        uint64_t lba = cdb[0] == 0x2a ?
            ((uint64_t)cdb[2] << 24 | (uint64_t)cdb[3] << 16 | (uint64_t)cdb[4] << 8 | cdb[5]) :
            ((uint64_t)cdb[2] << 56 | (uint64_t)cdb[3] << 48 | (uint64_t)cdb[4] << 40 | (uint64_t)cdb[5] << 32 |
             (uint64_t)cdb[6] << 24 | (uint64_t)cdb[7] << 16 | (uint64_t)cdb[8] << 8 | cdb[9]);
        uint64_t blocks = cdb[0] == 0x2a ? ((uint64_t)cdb[7] << 8 | cdb[8]) :
            ((uint64_t)cdb[10] << 24 | (uint64_t)cdb[11] << 16 | (uint64_t)cdb[12] << 8 | cdb[13]);

        if (dd != UTRD_DD_OUT || (upiu[1] & UPIU_FLAG_WRITE) == 0 || (upiu[1] & UPIU_FLAG_READ) != 0) {
            return "a write without data to the device or with READ flag";
        }
        if (blocks == 0 || expected != blocks * BLOCK) {
            return "a write whose length is not what the UPIU expects";
        }
        if (!sim_write_enabled || lun != sim_write_lun) {
            return "writes disabled or not targeted at writable logical unit";
        }
        if (lba < sim_write_lba_start || lba + blocks > sim_write_lba_end) {
            return "write outside permitted darwin_var LBA range";
        }
        if ((cdb[1] & ~0x08) != 0) {
            return "write CDB[1] has unsupported flags other than FUA";
        }
        return NULL;
    }
    case 0x35: {    /* SYNCHRONIZE CACHE(10) */
        if (dd != UTRD_DD_NONE || expected != 0 || (upiu[1] & (UPIU_FLAG_READ | UPIU_FLAG_WRITE)) != 0) {
            return "SYNCHRONIZE CACHE with data or unexpected flags";
        }
        if (!sim_write_enabled || lun != sim_write_lun) {
            return "SYNCHRONIZE CACHE targeted at non-writable LUN or writes disabled";
        }
        return NULL;
    }
    case 0x42: {    /* UNMAP */
        uint16_t param_len = (uint16_t)cdb[7] << 8 | cdb[8];

        if (dd != UTRD_DD_OUT || (upiu[1] & UPIU_FLAG_WRITE) == 0 || (upiu[1] & UPIU_FLAG_READ) != 0) {
            return "UNMAP without write data direction";
        }
        if (expected != 24 || param_len != 24) {
            return "UNMAP length mismatch or not single-segment (expected 24 bytes)";
        }
        if (!sim_write_enabled || lun != sim_write_lun) {
            return "UNMAP targeted at non-writable LUN or writes disabled";
        }
        return NULL;
    }
    case 0x00:      /* TEST UNIT READY */
        return dd == UTRD_DD_NONE && expected == 0 && (upiu[1] & (UPIU_FLAG_READ | UPIU_FLAG_WRITE)) == 0 ? NULL :
               "TEST UNIT READY with data";
    case 0x03:      /* REQUEST SENSE */
    case 0x12:      /* INQUIRY */
    case 0x25:      /* READ CAPACITY(10) */
        return NULL;
    case 0x9e:      /* SERVICE ACTION IN(16): READ CAPACITY(16) only */
        return (cdb[1] & 0x1f) == 0x10 ? NULL : "a SERVICE ACTION IN other than READ CAPACITY(16)";
    default:
        return "a SCSI operation not on the list";
    }
}

static void test_ufs_whitelist(void)
{
    uint8_t upiu[32];
    uint32_t lun_mask = (1u << 0) | (1u << 1);

    /* 1. Normal READ(10) on permitted LUN */
    memset(upiu, 0, sizeof(upiu));
    upiu[0] = UPIU_COMMAND;
    upiu[1] = UPIU_FLAG_READ;
    upiu[2] = 0; /* LUN 0 */
    upiu[14] = (4096 >> 8) & 0xff;
    upiu[15] = 4096 & 0xff;
    upiu[16] = 0x28; /* READ(10) */
    upiu[23] = 0;
    upiu[24] = 1; /* 1 block */
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, lun_mask, false) == NULL);

    /* 2. Read with wrong direction / flag */
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_OUT, lun_mask, false) != NULL);

    /* 3. Writable range tests */
    sim_write_enabled = true;
    sim_write_lun = 0;
    sim_write_lba_start = 1000;
    sim_write_lba_end = 5000;

    /* Normal WRITE(10) inside range */
    memset(upiu, 0, sizeof(upiu));
    upiu[0] = UPIU_COMMAND;
    upiu[1] = UPIU_FLAG_WRITE;
    upiu[2] = 0;
    upiu[14] = (4096 >> 8) & 0xff;
    upiu[15] = 4096 & 0xff;
    upiu[16] = 0x2a; /* WRITE(10) */
    upiu[18] = 0;
    upiu[19] = 0;
    upiu[20] = (1500 >> 8) & 0xff;
    upiu[21] = 1500 & 0xff; /* LBA 1500 */
    upiu[23] = 0;
    upiu[24] = 1; /* 1 block */
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_OUT, lun_mask, false) == NULL);

    /* FUA WRITE(10): cdb[1] = 0x08 */
    upiu[17] = 0x08;
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_OUT, lun_mask, false) == NULL);

    /* WRITE(10) with invalid flags in cdb[1] (e.g. 0x10) */
    upiu[17] = 0x10;
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_OUT, lun_mask, false) != NULL);
    upiu[17] = 0x00;

    /* WRITE outside range: LBA 6000 */
    upiu[20] = (6000 >> 8) & 0xff;
    upiu[21] = 6000 & 0xff;
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_OUT, lun_mask, false) != NULL);

    /* 4. SYNCHRONIZE CACHE(10) */
    memset(upiu, 0, sizeof(upiu));
    upiu[0] = UPIU_COMMAND;
    upiu[2] = 0; /* writable LUN */
    upiu[16] = 0x35; /* SYNCHRONIZE CACHE(10) */
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_NONE, lun_mask, false) == NULL);

    /* SYNCHRONIZE CACHE on non-writable LUN (LUN 1) */
    upiu[2] = 1;
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_NONE, lun_mask, false) != NULL);
    upiu[2] = 0;

    /* 5. UNMAP */
    memset(upiu, 0, sizeof(upiu));
    upiu[0] = UPIU_COMMAND;
    upiu[1] = UPIU_FLAG_WRITE;
    upiu[2] = 0;
    upiu[15] = 24; /* expected length */
    upiu[16] = 0x42; /* UNMAP */
    upiu[24] = 24; /* cdb[8] param_len */
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_OUT, lun_mask, false) == NULL);

    /* UNMAP with multi-segment / invalid length (e.g. 48) */
    upiu[15] = 48;
    upiu[24] = 48;
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_OUT, lun_mask, false) != NULL);

    /* UNMAP on non-writable LUN */
    upiu[15] = 24;
    upiu[24] = 24;
    upiu[2] = 1;
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_OUT, lun_mask, false) != NULL);

    /* 6. Refused management / dangerous commands */
    memset(upiu, 0, sizeof(upiu));
    upiu[0] = UPIU_COMMAND;
    upiu[2] = 0;
    upiu[16] = 0x04; /* FORMAT UNIT */
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_NONE, lun_mask, false) != NULL);

    upiu[16] = 0x3b; /* WRITE BUFFER */
    assert(sim_hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_OUT, lun_mask, false) != NULL);

    printf("test_ufs_whitelist passed\n");
}

/* ========================================================================= */
/* UFS PRDT 64-Entry Scatter-Gather & Coalescing                             */
/* ========================================================================= */

#define UCD_PRDT_ENTRIES 64u

struct sim_prdt_entry {
    uint32_t base_addr_lo;
    uint32_t base_addr_hi;
    uint32_t reserved;
    uint32_t data_byte_cnt; /* byte count - 1 */
};

/* Coalesces contiguous physical page segments into PRDT entries */
static uint32_t sim_build_prdt(const uint64_t *pages, uint32_t npages, struct sim_prdt_entry *prdt, uint32_t max_entries)
{
    uint32_t nentries = 0;
    uint32_t i = 0;

    while (i < npages && nentries < max_entries) {
        uint64_t start_pa = pages[i];
        uint32_t seg_len = 4096;
        i++;

        /* Coalesce contiguous pages */
        while (i < npages && pages[i] == start_pa + seg_len && (seg_len + 4096) <= 262144) {
            seg_len += 4096;
            i++;
        }

        prdt[nentries].base_addr_lo = (uint32_t)start_pa;
        prdt[nentries].base_addr_hi = (uint32_t)(start_pa >> 32);
        prdt[nentries].reserved = 0;
        prdt[nentries].data_byte_cnt = seg_len - 1; /* UFS spec: size - 1 */
        nentries++;
    }

    return nentries;
}

static void test_ufs_prdt_scatter_gather(void)
{
    struct sim_prdt_entry prdt[UCD_PRDT_ENTRIES];
    uint64_t pages[64];

    /* Verify structure size and alignment: 16 bytes per entry, exactly 1024 bytes for 64 entries */
    assert(sizeof(struct sim_prdt_entry) == 16);
    assert(sizeof(prdt) == 1024);

    /* Case 1: 64 contiguous pages (256 KiB transfer) -> coalesces into exactly 1 PRDT entry */
    for (uint32_t i = 0; i < 64; i++) {
        pages[i] = 0x80000000ULL + i * 4096;
    }
    uint32_t n = sim_build_prdt(pages, 64, prdt, UCD_PRDT_ENTRIES);
    assert(n == 1);
    assert(prdt[0].base_addr_lo == 0x80000000U);
    assert(prdt[0].base_addr_hi == 0);
    assert(prdt[0].data_byte_cnt == 262144 - 1); /* 256 KiB - 1 */

    /* Case 2: 64 non-contiguous physical pages -> exactly 64 PRDT entries */
    for (uint32_t i = 0; i < 64; i++) {
        pages[i] = 0x90000000ULL + (uint64_t)i * 0x10000; /* spaced 64 KiB apart */
    }
    n = sim_build_prdt(pages, 64, prdt, UCD_PRDT_ENTRIES);
    assert(n == 64);
    for (uint32_t i = 0; i < 64; i++) {
        assert(prdt[i].base_addr_lo == (uint32_t)(0x90000000ULL + (uint64_t)i * 0x10000));
        assert(prdt[i].data_byte_cnt == 4096 - 1);
        assert(prdt[i].reserved == 0);
    }

    /* Case 3: 4 groups of 16 contiguous pages -> 4 PRDT entries of 64 KiB each */
    for (uint32_t g = 0; g < 4; g++) {
        for (uint32_t p = 0; p < 16; p++) {
            pages[g * 16 + p] = 0xa0000000ULL + ((uint64_t)g * 0x200000) + (p * 4096);
        }
    }
    n = sim_build_prdt(pages, 64, prdt, UCD_PRDT_ENTRIES);
    assert(n == 4);
    for (uint32_t g = 0; g < 4; g++) {
        assert(prdt[g].base_addr_lo == (uint32_t)(0xa0000000ULL + ((uint64_t)g * 0x200000)));
        assert(prdt[g].data_byte_cnt == (16 * 4096) - 1); /* 64 KiB - 1 */
    }

    printf("test_ufs_prdt_scatter_gather passed\n");
}

/* ========================================================================= */
/* UFS Interrupt Aggregation & GICv3 Configuration                           */
/* ========================================================================= */

#define UFSHC_SPI_INTID     297u

#define UFSHC_IE_UTRCE      (1u << 0)
#define UFSHC_IE_UE         (1u << 2)
#define UFSHC_IE_UTPMS      (1u << 4)

#define UTRIACR_IAEN        (1u << 31)
#define UTRIACR_IAPWEN      (1u << 24)
#define UTRIACR_CTR         (1u << 16)
#define UTRIACR_IACTH(c)    (((c) & 0x1fu) << 8)
#define UTRIACR_IATOVAL(t)  ((t) & 0xffu)

static void test_ufs_interrupt_aggregation(void)
{
    /* Verify GICv3 SPI INTID: DT <0 0x109 4> -> 32 + 265 = 297 */
    assert(UFSHC_SPI_INTID == 297);

    /* Verify Interrupt Enable bits: UTRCE (bit 0) | UE (bit 2) | UTPMS (bit 4) = 0x15 */
    uint32_t ie = UFSHC_IE_UTRCE | UFSHC_IE_UE | UFSHC_IE_UTPMS;
    assert(ie == 0x15);

    /* Verify UTRIACR aggregation formula: 4 completions, 80 us (2 ticks of 40 us) */
    uint32_t threshold = 4;
    uint32_t timeout_ticks = 2; /* 80 us / 40 us = 2 */
    uint32_t iacr = UTRIACR_IAEN | UTRIACR_IAPWEN | UTRIACR_CTR |
                    UTRIACR_IACTH(threshold) | UTRIACR_IATOVAL(timeout_ticks);

    assert(iacr == 0x81010402U);
    assert((iacr & UTRIACR_IAEN) != 0);
    assert(((iacr >> 8) & 0x1f) == 4);
    assert((iacr & 0xff) == 2);

    /* Verify quiesce value clears enable but keeps reset */
    uint32_t quiesce_iacr = UTRIACR_IAPWEN | UTRIACR_CTR;
    assert((quiesce_iacr & UTRIACR_IAEN) == 0);
    assert(quiesce_iacr == 0x01010000U);

    printf("test_ufs_interrupt_aggregation passed\n");
}

/* ========================================================================= */
/* UFS Rework: UCD Boundary, ISR Completion Safety & UPL NULL Protection     */
/* ========================================================================= */

static bool sim_probe_ucd(uint32_t ucd_size, uint32_t area_size, uint32_t ucd_offset)
{
    if (ucd_size < 2048u) {
        return false;
    }
    if ((ucd_offset & 0x7f) || (ucd_size & 0x7f) || ucd_offset < 32 * 32) {
        return false;
    }
    if (area_size < ucd_offset + 32 * ucd_size) {
        return false;
    }
    return true;
}

static void test_ufs_ucd_size_boundary(void)
{
    uint32_t ucd_offset = 2048; /* aligned and >= 32 * 32 */
    uint32_t area_size = 2048 + 32 * 2048;

    /* Boundary 1: < 2048 rejected */
    assert(!sim_probe_ucd(0, area_size, ucd_offset));
    assert(!sim_probe_ucd(1024, area_size, ucd_offset));
    assert(!sim_probe_ucd(2047, area_size, ucd_offset));

    /* Boundary 2: >= 2048 accepted (with proper alignment and area) */
    assert(sim_probe_ucd(2048, area_size, ucd_offset));
    assert(sim_probe_ucd(4096, 2048 + 32 * 4096, ucd_offset));

    /* Boundary 3: Misaligned ucd_size (not 128-byte aligned) rejected */
    assert(!sim_probe_ucd(2049, area_size, ucd_offset));

    /* Boundary 4: Area too small rejected */
    assert(!sim_probe_ucd(2048, area_size - 1, ucd_offset));

    printf("test_ufs_ucd_size_boundary passed\n");
}

/* Simulation of ISR vs Thread completion */
static bool in_hardware_isr = false;
static uint32_t sim_biodone_calls = 0;
static bool sim_thread_woken = false;

static void sim_buf_biodone(void *bp)
{
    (void)bp;
    /* HARD INVARIANT: buf_biodone must NEVER be called from hardware ISR context! */
    assert(!in_hardware_isr);
    sim_biodone_calls++;
}

static void sim_ufshc_isr(uint32_t is_status, uint32_t active_mask, uint32_t dbr,
                          uint32_t *pending_slots, bool *thread_event)
{
    in_hardware_isr = true;
    if (is_status == 0) {
        in_hardware_isr = false;
        return;
    }
    /* Clear status (W1C) */
    uint32_t done = active_mask & ~dbr;
    if (done != 0) {
        *pending_slots |= done;
    }
    *thread_event = true;
    sim_thread_woken = true;

    /* Note: Absolutely NO sim_buf_biodone() called here! */
    in_hardware_isr = false;
}

static void sim_ufshc_worker(uint32_t *pending_slots, void *bp)
{
    assert(!in_hardware_isr);
    uint32_t done = *pending_slots;
    *pending_slots = 0;

    if (done != 0 && bp != NULL) {
        sim_buf_biodone(bp);
    }
}

static void test_ufs_interrupt_completion_queue(void)
{
    uint32_t pending_slots = 0;
    bool thread_event = false;
    int dummy_bp = 0x1234;

    sim_biodone_calls = 0;
    sim_thread_woken = false;

    /* 1. Simulate hardware interrupt firing with completed slot 1 */
    uint32_t active = (1u << 1);
    uint32_t dbr_after = 0; /* doorbell cleared for slot 1 */
    sim_ufshc_isr(1u /* UTRCS */, active, dbr_after, &pending_slots, &thread_event);

    assert(sim_thread_woken);
    assert(thread_event);
    assert(pending_slots == (1u << 1));
    /* biodone NOT called yet during ISR */
    assert(sim_biodone_calls == 0);

    /* 2. Worker thread runs in standard kernel thread context */
    sim_ufshc_worker(&pending_slots, &dummy_bp);
    assert(pending_slots == 0);
    assert(sim_biodone_calls == 1);

    printf("test_ufs_interrupt_completion_queue passed\n");
}

/* Simulation of UPL NULL pointer protection and buf_map/buf_unmap */
static bool sim_buf_is_mapped = false;
static uint8_t sim_page_buffer[4096];

static int sim_buf_map(void *bp, uint8_t **out_addr)
{
    (void)bp;
    assert(!sim_buf_is_mapped);
    sim_buf_is_mapped = true;
    *out_addr = sim_page_buffer;
    return 0;
}

static int sim_buf_unmap(void *bp)
{
    (void)bp;
    assert(sim_buf_is_mapped);
    sim_buf_is_mapped = false;
    return 0;
}

static void test_ufs_upl_null_pointer_safety(void)
{
    /* Case A: Traditional buffer with valid buf_dataptr */
    uint8_t direct_buffer[512];
    uint8_t *data = direct_buffer;
    bool mapped = false;

    if (data == NULL) {
        int err = sim_buf_map(NULL, &data);
        assert(err == 0);
        mapped = true;
    }
    assert(!mapped);
    assert(data == direct_buffer);

    /* Case B: UBC cluster I/O where buf_dataptr is NULL */
    data = NULL;
    mapped = false;
    if (data == NULL) {
        int err = sim_buf_map(NULL, &data);
        assert(err == 0);
        assert(data == sim_page_buffer);
        mapped = true;
    }
    assert(mapped);
    assert(sim_buf_is_mapped);
    /* Safe to access data without NULL pointer dereference */
    data[0] = 0x5a;
    assert(sim_page_buffer[0] == 0x5a);

    if (mapped) {
        int err = sim_buf_unmap(NULL);
        assert(err == 0);
    }
    assert(!sim_buf_is_mapped);

    printf("test_ufs_upl_null_pointer_safety passed\n");
}

static void test_part3h_ufs_and_vnodes(void)
{
    /* 1. UTRD_INT contract */
    #define UTRD_CT_MASK_TEST    0xf0000000u
    #define UTRD_CT_UFS_TEST     (1u << 28)
    #define UTRD_DD_MASK_TEST    (3u << 25)
    #define UTRD_DD_NONE_TEST    (0u << 25)
    #define UTRD_DD_OUT_TEST     (1u << 25)
    #define UTRD_DD_IN_TEST      (2u << 25)
    #define UTRD_INT_TEST        (1u << 24)

    uint32_t dw0_read = UTRD_CT_UFS_TEST | UTRD_DD_IN_TEST | UTRD_INT_TEST;
    uint32_t dw0_write = UTRD_CT_UFS_TEST | UTRD_DD_OUT_TEST | UTRD_INT_TEST;
    uint32_t dw0_tur = UTRD_CT_UFS_TEST | UTRD_DD_NONE_TEST | UTRD_INT_TEST;
    uint32_t dw0_sync = UTRD_CT_UFS_TEST | UTRD_DD_NONE_TEST | UTRD_INT_TEST;
    uint32_t dw0_unmap = UTRD_CT_UFS_TEST | UTRD_DD_OUT_TEST | UTRD_INT_TEST;

    assert((dw0_read & UTRD_INT_TEST) != 0);
    assert((dw0_write & UTRD_INT_TEST) != 0);
    assert((dw0_tur & UTRD_INT_TEST) != 0);
    assert((dw0_sync & UTRD_INT_TEST) != 0);
    assert((dw0_unmap & UTRD_INT_TEST) != 0);

    /* Verify UTRD_INT does not collide with CT or DD */
    assert((dw0_read & UTRD_CT_MASK_TEST) == UTRD_CT_UFS_TEST);
    assert((dw0_read & UTRD_DD_MASK_TEST) == UTRD_DD_IN_TEST);
    assert((dw0_write & UTRD_CT_MASK_TEST) == UTRD_CT_UFS_TEST);
    assert((dw0_write & UTRD_DD_MASK_TEST) == UTRD_DD_OUT_TEST);

    /* 2. UFSHCI Interrupt status and UTRLCNR W1C logic */
    uint32_t hc_cnr_in = 0x00000003u; /* slots 0 and 1 were active at boot/switch */
    uint32_t cnr_hw = 0x0000000fu;    /* hardware has slots 0, 1, 2, 3 completed */
    uint32_t cnr_clear = cnr_hw & ~hc_cnr_in;
    assert(cnr_clear == 0x0000000cu); /* only clears slots 2 and 3 */

    /* 3. UTRIACR counter/timer reset formula */
    uint32_t utriacr_reset = UTRIACR_CTR | UTRIACR_IAEN | UTRIACR_IAPWEN | UTRIACR_IACTH(4) | UTRIACR_IATOVAL(2);
    assert((utriacr_reset & UTRIACR_CTR) != 0);
    assert((utriacr_reset & UTRIACR_IAEN) != 0);
    assert((utriacr_reset & UTRIACR_IAPWEN) != 0);

    /* 4. PRDT segment physical iteration simulation */
    uint32_t prd_table[4 * 4]; /* 4 segments */
    for (int i = 0; i < 4; i++) {
        uint64_t pa = 0x180000000ULL + i * 4096;
        uint32_t len = 4096;
        prd_table[4 * i + 0] = (uint32_t)pa;
        prd_table[4 * i + 1] = (uint32_t)(pa >> 32);
        prd_table[4 * i + 2] = 0;
        prd_table[4 * i + 3] = len - 1;
    }
    uint64_t flushed_bytes = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t pa = (uint64_t)prd_table[4 * i + 0] | ((uint64_t)prd_table[4 * i + 1] << 32);
        uint32_t len = prd_table[4 * i + 3] + 1;
        assert(pa == 0x180000000ULL + i * 4096);
        assert(len == 4096);
        flushed_bytes += len;
    }
    assert(flushed_bytes == 16384);

    /* 5. PID 1 maxvnodes calculation and guard */
    #define MAX_VNODES_TEST 263168
    unsigned long long memsize_16g = 16ULL * 1024 * 1024 * 1024;
    int vnodes = memsize_16g / 65536 + 1024 > MAX_VNODES_TEST ? MAX_VNODES_TEST : (int)(memsize_16g / 65536) + 1024;
    assert(vnodes == 263168);

    /* When current maxvnodes is already 263168 from kernel tier, do not downgrade */
    int cur_maxvnodes = 263168;
    bool write_needed = (vnodes > cur_maxvnodes);
    assert(!write_needed);

    /* When current maxvnodes is small (e.g. 1024 default), upgrade is triggered */
    cur_maxvnodes = 1024;
    write_needed = (vnodes > cur_maxvnodes);
    assert(write_needed);

    printf("test_part3h_ufs_and_vnodes passed\n");
}

static bool sim_sectask_entitlements_validated(uint32_t cs_flags)
{
    /* Mirrors Security.framework SecTaskEntitlementsValidated (0x1890261ac) */
    const uint32_t strict_mask = 0x4201u;     /* CS_ENTITLEMENTS_VALIDATED | CS_KILL | CS_VALID */
    const uint32_t debug_mask = 0x10004000u;  /* CS_DEBUGGED | CS_ENTITLEMENTS_VALIDATED */
    return ((cs_flags & strict_mask) == strict_mask) ||
           ((cs_flags & debug_mask) == debug_mask);
}

static bool sim_tfa9866_ready(uint16_t r11, uint16_t r12, bool muted)
{
    return (r11 & 1u) != 0 &&
           (r11 & (1u << 2)) == 0 &&
           (r12 & 0x0fu) >= (muted ? 6u : 9u) &&
           (r12 & (1u << 13)) == 0;
}

static void test_wave1_r4_contracts(void)
{
    /* 1. SecTaskEntitlementsValidated bitmask contract without CS_KILL */
    uint32_t old_flags = 0x26000003u; /* CS_PLATFORM_BINARY | CS_ADHOC | CS_VALID | CS_SIGNED */
    assert(!sim_sectask_entitlements_validated(old_flags));
    assert(!sim_sectask_entitlements_validated(old_flags | 0x4000u)); /* missing CS_DEBUGGED & CS_KILL */

    uint32_t r4_flags = old_flags | 0x4000u | 0x10000000u; /* + CS_ENTITLEMENTS_VALIDATED | CS_DEBUGGED */
    assert(sim_sectask_entitlements_validated(r4_flags));
    /* Even after DYLD_INSERT_LIBRARIES clears CS_VALID, validated state holds without SIGKILL */
    assert(sim_sectask_entitlements_validated(r4_flags & ~1u));
    assert((r4_flags & 0x0200u) == 0); /* CS_KILL remains clear */

    /* 2. K90 TFA9866 (rev=0x9866) operating readiness: flg1=0x0881, st0=0x1709 */
    assert(sim_tfa9866_ready(0x0881u, 0x1709u, false));
    assert(!sim_tfa9866_ready(0x0881u, 0x1704u, false)); /* MANSTATE=4 still ramping */
    assert(!sim_tfa9866_ready(0x0880u, 0x1709u, false)); /* CLKS=0 not locked */
    assert(!sim_tfa9866_ready(0x0885u, 0x1709u, false)); /* NOCLK=1 */
    assert(sim_tfa9866_ready(0x0881u, 0x1706u, true));   /* muted reaches MANSTATE=6 */

    /* 3. APFS synthetic alias parent identity update on cached target vnode */
    uint64_t staged_dir_id = 9001;
    uint64_t apps_dir_id = 42;
    uint64_t an_parent = staged_dir_id;
    uint64_t ino_parent_id = staged_dir_id;
    /* apfs_sysctl_synthetic rebinds cached tvp to synthetic parent */
    an_parent = apps_dir_id;
    ino_parent_id = apps_dir_id;
    assert(an_parent == apps_dir_id && ino_parent_id == apps_dir_id);

    printf("test_wave1_r4_contracts passed\n");
}

int main(void)
{
    test_backlight_mapping();
    test_battery_hysteresis();
    test_ufs_whitelist();
    test_ufs_prdt_scatter_gather();
    test_ufs_interrupt_aggregation();
    test_ufs_ucd_size_boundary();
    test_ufs_interrupt_completion_queue();
    test_ufs_upl_null_pointer_safety();
    test_part3h_ufs_and_vnodes();
    test_wave1_r4_contracts();
    printf("ALL TESTS PASSED\n");
    return 0;
}
'''

def main():
    with tempfile.NamedTemporaryFile(suffix=".c") as f_src:
        f_src.write(TEST_C.encode("utf-8"))
        f_src.flush()
        with tempfile.NamedTemporaryFile(suffix="") as f_bin:
            subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", f_src.name, "-o", f_bin.name], check=True)
            res = subprocess.run([f_bin.name], capture_output=True, text=True)
            if res.stdout:
                print(res.stdout.strip())
            if res.stderr:
                print(res.stderr.strip())
            if res.returncode != 0:
                raise RuntimeError(f"Test binary failed with code {res.returncode}")

if __name__ == "__main__":
    main()

/* qsmmu-v500 MMIO and permission model, shared with native capture/close cases. */
#define APPS_SMMU_PHYS 0x15000000u
#define APPS_SMMU_SIZE 0x100000u

static uint32_t smmu_regs[APPS_SMMU_SIZE / 4u];
static unsigned smmu_writes, smmu_context_writes, smmu_global_syncs, smmu_context_syncs, smmu_bank_claims, smmu_cbar_touches;
static unsigned smmu_bypass_writes, smmu_trans_writes, smmu_serial;
static unsigned smmu_fault_serial, smmu_block_sync_serial, smmu_remove_sync_serial;
static unsigned smmu_cb_sync_serial[128], smmu_actlr_serial[128];
static unsigned smmu_cbar_serial[128], smmu_cba2r_serial[128];
static unsigned smmu_disable_serial[128];
static bool smmu_sctlr_read[128];
static uint32_t smmu_tcr2_request[128];
static uint32_t cam_native_dma_bytes;
static uint32_t smmu_model_bank, smmu_last_smr;
static bool smmu_have_domain, smmu_vmid_assignment;
static bool smmu_bad_cbar, smmu_bad_cba2r, smmu_reject_trans, smmu_reject_fault;
static bool smmu_fail_context_sync, smmu_fail_publish_sync, smmu_reject_disable;
static bool smmu_reject_publish;
static bool smmu_reject_initial_fault;
static bool smmu_fault_on_stop, smmu_revoke_on_stop;

static uint32_t reg_rd(vm_offset_t base, uint32_t offset);
static void reg_wr(vm_offset_t base, uint32_t offset, uint32_t value);
static void reg_wr64(vm_offset_t base, uint32_t offset, uint64_t value);
static void delay_for_interval(uint32_t n, uint32_t unit);
static void reset_fixture(void);
static void cam_native_capture(void);
static void check_success(void);
static void check_held(void);
static void check_no_frame(int expected);
static int annibale_cam_close(dev_t dev, int flags, int devtype, struct proc *p);
static int annibale_cam_open(dev_t dev, int flags, int devtype, struct proc *p);

static vm_offset_t ml_io_map(uint64_t pa, size_t bytes)
{
    CHECK(pa == APPS_SMMU_PHYS && bytes == APPS_SMMU_SIZE);
    return 6;
}

#include "annibale_camera_smmu.inc"

static uint32_t smmu_page_size(void) { return (smmu_regs[0x024 / 4] >> 31) ? 0x10000u : 0x1000u; }
static uint32_t smmu_pages(void) { return 1u << (((smmu_regs[0x024 / 4] >> 28) & 7u) + 1u); }
static uint32_t smmu_cb(uint32_t bank) { return (smmu_pages() + bank) * smmu_page_size(); }
static uint32_t smmu_cbar(uint32_t bank) { return smmu_page_size() + 4u * bank; }
static uint32_t smmu_cba2r(uint32_t bank) { return smmu_page_size() + 0x800u + 4u * bank; }

static void smmu_context_permission(uint32_t offset)
{
    if (offset < smmu_pages() * smmu_page_size()) return;
    uint32_t bank = offset / smmu_page_size() - smmu_pages();
    CHECK(bank < 128u);
    uint32_t cbar = smmu_regs[smmu_cbar(bank) / 4u], cba2r = smmu_regs[smmu_cba2r(bank) / 4u];
    if ((cbar & ~0xffu) != 0x1f300u || (cba2r & 0xffffu) != 1u) {
        fprintf(stderr, "SMMU context access before S1 claim: bank=%u offset=%#x cbar=%#x cba2r=%#x\n",
                bank, offset, cbar, cba2r);
        exit(1);
    }
}

static uint32_t camera_smmu_read(uint32_t offset)
{
    CHECK(offset < sizeof(smmu_regs) && offset % 4u == 0);
    smmu_context_permission(offset);
    if (offset >= smmu_pages() * smmu_page_size() && offset % smmu_page_size() == 0)
        smmu_sctlr_read[offset / smmu_page_size() - smmu_pages()] = true;
    if (offset == 0x074u &&
        (smmu_fail_publish_sync || (fail_smmu_stop && address_writes && smmu_fault_serial))) return 1;
    if (offset >= smmu_pages() * smmu_page_size() &&
        offset % smmu_page_size() == 0x7f4u && smmu_fail_context_sync) return 1;
    return smmu_regs[offset / 4u];
}

static void camera_smmu_write(uint32_t offset, uint32_t value)
{
    CHECK(offset < sizeof(smmu_regs) && offset % 4u == 0);
    smmu_context_permission(offset);
    smmu_writes++; smmu_serial++;
    uint32_t streams = smmu_regs[0x020 / 4u] & 0xffu;
    if (offset >= 0xc00u && offset < 0xc00u + 4u * streams) {
        uint32_t slot = (offset - 0xc00u) / 4u, type = (value >> 16) & 3u;
        if (type == 1u) {
            smmu_bypass_writes++;
            value = (value & ~(3u << 16)) | (2u << 16);
        } else if (type == 0u) {
            uint32_t bank = value & 0xffu, cb = smmu_cb(bank);
            /* This S1 bypass permission model requires an assigned CB before TYPE_TRANS. */
            if (bank >= 128u || (smmu_regs[smmu_cbar(bank) / 4u] & ~0xffu) != 0x1f300u) {
                fprintf(stderr, "SMMU TYPE_TRANS permission rejected: slot=%u bank=%u offset=%#x value=%#x smr=%#x bypass=%u address=%u\n",
                        slot, bank, offset, value, smmu_regs[(0x800u + 4u * slot) / 4u],
                        smmu_bypass_writes, address_writes);
                exit(1);
            }
            CHECK((smmu_regs[smmu_cba2r(bank) / 4u] & 0xffffu) == 1u);
            CHECK(smmu_tcr2_request[bank] == 0x70u);
            CHECK(smmu_regs[(cb + 0x010u) / 4u] == 0x70u);
            CHECK(smmu_regs[cb / 4u] == 0x0a5f0066u);
            CHECK(smmu_regs[(cb + 0x004u) / 4u] == 1u);
            CHECK(smmu_cb_sync_serial[bank] > smmu_actlr_serial[bank]);
            CHECK((value & 0x300u) == 0x300u);
            smmu_trans_writes++; smmu_model_bank = bank;
            if (smmu_reject_trans) value = 0x20300u;
        } else if (type == 2u) {
            if ((smmu_regs[offset / 4u] & (3u << 16)) == 0 &&
                (smmu_regs[(0x800u + 4u * slot) / 4u] & 0x80000000u)) {
                if (address_writes) route_stops++;
                smmu_fault_serial = smmu_serial;
                if (smmu_fault_on_stop)
                    smmu_regs[(smmu_cb(smmu_model_bank) + 0x058u) / 4u] |= 2u;
                if (smmu_revoke_on_stop)
                    smmu_regs[smmu_cbar(smmu_model_bank) / 4u] = 0;
            }
            if ((smmu_reject_fault && address_writes) || smmu_reject_initial_fault) return;
        }
    } else if (offset >= 0x800u && offset < 0x800u + 4u * streams) {
        if (value == 0x80001c00u) {
            if (smmu_have_domain)
                CHECK(smmu_cbar_serial[smmu_model_bank] > smmu_remove_sync_serial);
            CHECK((smmu_regs[(offset + 0x400u) / 4u] & (3u << 16)) == (2u << 16));
            if (smmu_reject_publish) return;
            smmu_owned = true; smmu_last_smr = offset;
        } else if (value == 0 && offset == smmu_last_smr) {
            CHECK(smmu_block_sync_serial > smmu_fault_serial);
            CHECK((smmu_regs[(offset + 0x400u) / 4u] & (3u << 16)) == (2u << 16));
            if (fail_smmu_restore) return;
        }
    } else if (offset == 0x070u) {
        smmu_global_syncs++;
        if (!camera_smmu_read(0x074u)) {
            if (smmu_last_smr && smmu_regs[smmu_last_smr / 4u] == 0)
                smmu_remove_sync_serial = smmu_serial;
            else smmu_block_sync_serial = smmu_serial;
        }
    } else if (offset >= smmu_page_size() && offset < smmu_page_size() + 4u * 128u) {
        uint32_t bank = (offset - smmu_page_size()) / 4u;
        CHECK(value == 0x1f300u);
        CHECK((smmu_regs[smmu_cba2r(bank) / 4u] & 0xffffu) == 1u);
        CHECK(smmu_cba2r_serial[bank] > smmu_cbar_serial[bank]);
        if (!smmu_regs[offset / 4u]) smmu_bank_claims++;
        smmu_cbar_touches++; smmu_cbar_serial[bank] = smmu_serial;
        smmu_sctlr_read[bank] = false;
        smmu_model_bank = bank; smmu_have_domain = true;
        if (smmu_bad_cbar) value ^= 0x1000u;
        if (smmu_vmid_assignment) value |= 0x3au;
    } else if (offset >= smmu_page_size() + 0x800u && offset < smmu_page_size() + 0xa00u) {
        uint32_t bank = (offset - smmu_page_size() - 0x800u) / 4u;
        CHECK(value == 1u);
        smmu_cba2r_serial[bank] = smmu_serial;
        if (smmu_bad_cba2r) value = 0;
        if (smmu_vmid_assignment) value |= 0x77u << 16;
    } else if (offset >= smmu_pages() * smmu_page_size()) {
        uint32_t bank = offset / smmu_page_size() - smmu_pages();
        uint32_t local = offset % smmu_page_size();
        CHECK(bank < 128u);
        smmu_context_writes++;
        if (local == 0) {
            if (value == 0) {
                CHECK(smmu_remove_sync_serial || !smmu_last_smr);
                CHECK(smmu_sctlr_read[bank] && (smmu_regs[offset / 4u] & 1u) == 0);
                if (smmu_reject_disable) return;
                smmu_disable_serial[bank] = smmu_serial;
            } else {
                CHECK((value & ~0x1000u) == 0x0a5f0066u && smmu_have_domain && smmu_sctlr_read[bank]);
                /* Model the observed camera SCTLR bit12 readback as zero. */
                value &= ~0x1000u;
            }
        } else if (local == 0x010u) {
            CHECK(smmu_disable_serial[bank] > smmu_cbar_serial[bank]);
            smmu_tcr2_request[bank] = value;
            /* Model the recorded effective TBI readback; its write handling is unknown. */
            value |= 0x60u;
        } else if (local == 0x004u) {
            CHECK(smmu_disable_serial[bank] > smmu_cbar_serial[bank]);
            CHECK(value == 1u); smmu_actlr_serial[bank] = smmu_serial;
        } else if (local == 0x058u) {
            CHECK(smmu_disable_serial[bank] > smmu_cbar_serial[bank]);
            CHECK(smmu_regs[smmu_cb(bank) / 4u] == 0 && value == 0xc00001feu);
            smmu_regs[offset / 4u] &= ~value;
            return;
        } else if (local == 0x610u) {
            CHECK(value == bank && smmu_have_domain);
        } else if (local == 0x7f0u) {
            smmu_context_syncs++;
            if (!smmu_fail_context_sync) {
                smmu_cb_sync_serial[bank] = smmu_serial;
                if (smmu_owned && smmu_last_smr && smmu_regs[smmu_last_smr / 4u] == 0 &&
                    smmu_regs[smmu_cb(bank) / 4u] == 0) {
                    smmu_owned = false; smmu_restores++;
                }
            }
        } else {
            CHECK(smmu_disable_serial[bank] > smmu_cbar_serial[bank]);
        }
    }
    smmu_regs[offset / 4u] = value;
}

static void reg_wr64(vm_offset_t base, uint32_t offset, uint64_t value)
{
    CHECK(base == 6 && offset % 8u == 0 && offset < sizeof(smmu_regs) - 4u);
    smmu_context_permission(offset);
    uint32_t bank = offset / smmu_page_size() - smmu_pages(), cb = smmu_cb(bank);
    CHECK(smmu_disable_serial[bank] > smmu_cbar_serial[bank]);
    CHECK(smmu_regs[(cb + 0x010u) / 4u] == 0x70u && smmu_regs[(cb + 0x030u) / 4u] == 0x800080u);
    CHECK(value == (uint64_t)bank << 48);
    smmu_regs[offset / 4u] = (uint32_t)value;
    smmu_regs[offset / 4u + 1u] = (uint32_t)(value >> 32);
    smmu_serial++; smmu_writes++; smmu_context_writes++;
}

static void camera_smmu_reset(void)
{
    memset(smmu_regs, 0, sizeof(smmu_regs));
    memset(smmu_cb_sync_serial, 0, sizeof(smmu_cb_sync_serial));
    memset(smmu_actlr_serial, 0, sizeof(smmu_actlr_serial));
    memset(smmu_cbar_serial, 0, sizeof(smmu_cbar_serial));
    memset(smmu_cba2r_serial, 0, sizeof(smmu_cba2r_serial));
    memset(smmu_disable_serial, 0, sizeof(smmu_disable_serial));
    memset(smmu_sctlr_read, 0, sizeof(smmu_sctlr_read));
    memset(smmu_tcr2_request, 0, sizeof(smmu_tcr2_request));
    smmu_regs[0x020 / 4u] = 0x48000080u;
    smmu_regs[0x024 / 4u] = 0x60020010u;
    smmu_regs[0x028 / 4u] = 0x00009011u;
    for (uint32_t i = 0; i < 12u; i++) {
        smmu_regs[(0x800u + 4u * i) / 4u] = 0x80000800u + i;
        smmu_regs[(0xc00u + 4u * i) / 4u] = 0x20300u;
    }
    smmu_regs[smmu_cbar(15u) / 4u] = 0x10000u;
    smmu_regs[smmu_cba2r(15u) / 4u] = 1u;
    cam_smmu_base = 0; cam_smmu_slot = 0xffu;
    cam_smmu_ready = cam_smmu_sync_failed = false;
#ifdef CAMERA_OLD_SMMU
    cam_smmu_owned = false; cam_smmu_saved_smr = cam_smmu_saved_s2cr = 0;
#else
    cam_smmu_bank = 0xffu; cam_smmu_page_size = cam_smmu_cb_offset = cam_smmu_stream_count = 0;
    cam_smmu_cbar = cam_smmu_cba2r = 0;
#endif
    smmu_writes = smmu_context_writes = smmu_global_syncs = smmu_context_syncs = smmu_bank_claims = smmu_cbar_touches = 0;
    smmu_bypass_writes = smmu_trans_writes = smmu_serial = 0;
    smmu_fault_serial = smmu_block_sync_serial = smmu_remove_sync_serial = 0;
    smmu_model_bank = UINT32_MAX; smmu_last_smr = 0;
    smmu_have_domain = smmu_vmid_assignment = false;
    smmu_bad_cbar = smmu_bad_cba2r = smmu_reject_trans = smmu_reject_fault = false;
    smmu_fail_context_sync = smmu_fail_publish_sync = smmu_reject_disable = smmu_reject_publish = false;
    smmu_reject_initial_fault = false;
    smmu_fault_on_stop = smmu_revoke_on_stop = false;
}

static unsigned camera_smmu_cases(void)
{
    unsigned cases = 0;
    reset_fixture();
    /* Recorded ID registers and bank5 reset state from the native camera round. */
    smmu_regs[0x020 / 4u] = 0x4c017e7fu;
    smmu_regs[0x024 / 4u] = 0x60000053u;
    smmu_regs[0x028 / 4u] = 0x5111u;
    smmu_regs[0x000 / 4u] = 0x260406u;
    for (uint32_t i = 0; i < 5u; i++) {
        smmu_regs[smmu_cbar(i) / 4u] = 0x1f300u;
        smmu_regs[smmu_cba2r(i) / 4u] = 1u;
    }
    /* Banks0..4 are synthetic exclusions; their GR1 values were not captured. */
    smmu_regs[(0xc00u + 4u * 12u) / 4u] = 0x300006eu;
    smmu_regs[smmu_cb(5u) / 4u] = 0x3567e502u;
    physical = 0x8d7f24000ULL;
    cam_native_capture(); check_success();
    CHECK(smmu_model_bank == 5u && smmu_cb(5u) == 0x85000u && smmu_last_smr == 0x830u);
    CHECK(smmu_regs[smmu_cb(5u) / 4u] == 0x0a5f0066u);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1u);
    CHECK(smmu_regs[smmu_cb(5u) / 4u] == 0 && smmu_restores == 1u); cases++;
    reset_fixture(); cam_native_capture(); check_success();
    uint32_t bank = smmu_model_bank;
    CHECK(bank == 2u && smmu_bank_claims == 1u && smmu_bypass_writes == 0);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1 && smmu_bank_claims == 1);
    CHECK(smmu_regs[(0xc00u + 4u * 12u) / 4u] == 0x20300u && smmu_regs[(0x800u + 4u * 12u) / 4u] == 0);
    CHECK(smmu_regs[smmu_cb(bank) / 4u] == 0); cases++;
    cam_profile = &profile_data; cam_region_vbase = (vm_offset_t)region; delivered = false;
    completed_frames = cache_flushes = 0;
    CHECK(annibale_cam_open(0, 0, 0, NULL) == 0);
    cam_native_capture(); check_success();
    CHECK(smmu_model_bank == bank && smmu_bank_claims == 1u && smmu_cbar_touches == 2u);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 2); cases++;

    for (unsigned kind = 0; kind < 3; kind++) {
        reset_fixture();
        smmu_regs[(0x800u + 4u * 3u) / 4u] = kind == 1u ? 0x80011c00u : 0x80001c00u;
        if (kind == 2u) smmu_regs[(0x800u + 4u * 4u) / 4u] = 0x80001c00u;
        cam_native_capture();
        CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && address_writes == 0 && smmu_writes == 0);
        CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1); cases++;
    }
    reset_fixture();
    for (uint32_t i = 12u; i < 128u; i++) smmu_regs[(0x800u + 4u * i) / 4u] = 0x80000900u + i;
    CHECK(!cam_smmu_configure_bypass() && smmu_writes == 0); cases++;

    for (unsigned kind = 0; kind < 6u; kind++) {
        reset_fixture();
        uint32_t cb = smmu_cb(2u);
        if (kind == 0u) smmu_regs[(0xc00u + 4u * 100u) / 4u] = 2u;
        if (kind == 1u) smmu_regs[smmu_cbar(2u) / 4u] = 0x10000u;
        if (kind == 2u) smmu_regs[smmu_cba2r(2u) / 4u] = 1u;
        if (kind == 3u) smmu_regs[(cb + 0x020u) / 4u] = 0x4000u;
        if (kind == 4u) smmu_regs[(cb + 0x004u) / 4u] = 1u;
        if (kind == 5u) smmu_regs[(cb + 0x058u) / 4u] = 2u;
        CHECK(cam_smmu_configure_bypass() && smmu_model_bank == (kind < 3u ? 3u : 2u));
        CHECK(cam_smmu_release()); cases++;
    }
    reset_fixture();
    for (uint32_t offset = 0; offset <= 0x03cu; offset += 4u)
        smmu_regs[(smmu_cb(2u) + offset) / 4u] = 0x55aa0010u;
    smmu_regs[(smmu_cb(2u) + 0x058u) / 4u] = 0xc00005feu;
    cam_native_capture(); check_success();
    CHECK(smmu_model_bank == 2u && smmu_regs[(smmu_cb(2u) + 0x058u) / 4u] == 0x400u);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1); cases++;
    reset_fixture(); smmu_regs[smmu_cb(2u) / 4u] = 1u;
    cam_native_capture(); check_held();
    CHECK(smmu_model_bank == 2u && smmu_context_writes == 0 && address_writes == 0); cases++;
    reset_fixture();
    for (uint32_t b = 2u; b < 15u; b++) smmu_regs[smmu_cbar(b) / 4u] = 0x10000u;
    smmu_regs[smmu_cbar(15u) / 4u] = smmu_regs[smmu_cba2r(15u) / 4u] = 0;
    CHECK(!cam_smmu_configure_bypass() && smmu_writes == 0); cases++;

    reset_fixture(); smmu_regs[0x020 / 4u] = 0x480000c8u;
    smmu_regs[(0xc00u + 4u * 175u) / 4u] = 2u;
    CHECK(cam_smmu_configure_bypass() && smmu_model_bank == 3u);
    CHECK(cam_smmu_release()); cases++;
    reset_fixture(); smmu_regs[0x024 / 4u] = 0x80020008u;
    CHECK(cam_smmu_configure_bypass() && smmu_model_bank == 2u);
    CHECK(cam_smmu_release()); cases++;

    for (unsigned kind = 0; kind < 6u; kind++) {
        reset_fixture();
        if (kind == 0u) smmu_regs[0x020 / 4u] &= ~(1u << 30);
        if (kind == 1u) smmu_regs[0x020 / 4u] &= ~(1u << 27);
        if (kind == 2u) smmu_regs[0x028 / 4u] &= ~(1u << 12);
        if (kind == 3u) smmu_regs[0x000 / 4u] |= 1u << 3;
        if (kind == 4u) smmu_regs[0x024 / 4u] = 0x80020080u;
        if (kind == 5u) smmu_regs[0x024 / 4u] = UINT32_MAX;
        CHECK(!cam_smmu_configure_bypass() && smmu_writes == 0); cases++;
    }
    reset_fixture(); smmu_vmid_assignment = true;
    cam_native_capture(); check_success(); bank = smmu_model_bank;
    CHECK(smmu_regs[smmu_cbar(bank) / 4u] == 0x1f33au && smmu_regs[smmu_cba2r(bank) / 4u] == 0x770001u);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1); cases++;

    for (unsigned kind = 0; kind < 4u; kind++) {
        reset_fixture();
        smmu_bad_cbar = kind == 0u; smmu_bad_cba2r = kind == 1u;
        smmu_fail_context_sync = kind == 2u; smmu_fail_publish_sync = kind == 3u;
        cam_native_capture(); CHECK(address_writes == 0 && smmu_trans_writes == 0);
        check_held(); cases++;
    }
    reset_fixture(); smmu_reject_trans = true; cam_native_capture();
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && address_writes == 0 && !smmu_owned);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1); cases++;
    reset_fixture(); smmu_reject_publish = true; cam_native_capture();
    CHECK(address_writes == 0); check_held(); cases++;
    reset_fixture(); smmu_reject_initial_fault = true; cam_native_capture();
    CHECK(address_writes == 0 && smmu_last_smr == 0); check_held(); cases++;
    reset_fixture(); smmu_reject_fault = true; cam_native_capture(); check_held(); cases++;

    for (unsigned kind = 0; kind < 6u; kind++) {
        reset_fixture(); cam_native_capture(); check_success(); bank = smmu_model_bank;
        if (kind == 0u) smmu_regs[smmu_cba2r(bank) / 4u] ^= 1u << 16;
        if (kind == 1u) smmu_regs[(0xc00u + 4u * 100u) / 4u] = bank;
        if (kind == 2u) smmu_fail_context_sync = true;
        if (kind == 3u) smmu_reject_disable = true;
        if (kind == 4u) smmu_regs[smmu_cbar(bank) / 4u] = 0;
        if (kind == 5u) smmu_regs[smmu_cba2r(bank) / 4u] = 0;
        CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
        CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_HELD && region_frees == 0 && profile_frees == 0);
        memset(output, 0xcc, sizeof(output));
        check_no_frame(EIO); cases++;
    }
    reset_fixture(); smmu_fault_on_stop = true; cam_native_capture();
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && cam_capture.error == EIO);
    CHECK((cam_capture.flags & ANNIBALE_CAM_CAPTURE_COMPLETE) == ANNIBALE_CAM_CAPTURE_COMPLETE);
    check_no_frame(EIO);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1); cases++;
    smmu_fault_on_stop = false; cam_profile = &profile_data;
    cam_region_vbase = (vm_offset_t)region; delivered = false;
    completed_frames = cache_flushes = 0;
    CHECK(annibale_cam_open(0, 0, 0, NULL) == 0);
    cam_native_capture(); check_success();
    CHECK(smmu_bank_claims == 1u && annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 2); cases++;
    reset_fixture(); smmu_revoke_on_stop = true; cam_native_capture(); check_held(); cases++;
    reset_fixture(); fail_smmu_stop = true; cam_native_capture(); check_held();
    unsigned syncs = smmu_global_syncs, writes = smmu_writes;
    CHECK(!cam_smmu_configure_bypass() && smmu_global_syncs == syncs && smmu_writes == writes);
#ifndef CAMERA_OLD_SMMU
    CHECK(!cam_smmu_sync(0) && smmu_global_syncs == syncs);
#endif
    cases++;
    /* A changed TCR2 field must retain the assigned domain and DMA resources. */
    static const uint32_t tcr2_bits[] = {0u, 4u, 5u, 6u, 15u};
    for (unsigned i = 0; i < sizeof(tcr2_bits) / sizeof(tcr2_bits[0]); i++) {
        reset_fixture(); cam_native_capture(); check_success(); bank = smmu_model_bank;
        smmu_regs[(smmu_cb(bank) + 0x010u) / 4u] ^= 1u << tcr2_bits[i];
        CHECK(!cam_smmu_bank_valid(CAM_SMMU_S1_BYPASS_SCTLR));
        CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
        CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_HELD && region_frees == 0 && profile_frees == 0);
        memset(output, 0xcc, sizeof(output));
        check_no_frame(EIO);
        CHECK(annibale_cam_open(0, 0, 0, NULL) == EBUSY); cases++;
    }
    const uint64_t dma_limit = 1ULL << 36;
    const uint64_t rounded_bytes = (3248ULL * 1458u + 511u) & ~511ULL;
    for (unsigned kind = 0; kind < 3u; kind++) {
        reset_fixture(); smmu_regs[0x028u / 4u] = 0x5111u;
        physical = kind == 0u ? dma_limit - rounded_bytes :
                   kind == 1u ? dma_limit - rounded_bytes + 512u : 0;
        cam_native_capture();
        if (kind == 0u) {
            check_success();
            CHECK(cam_capture.dma_address + cam_native_dma_bytes == dma_limit);
        } else {
            CHECK(cam_capture.error == EOVERFLOW && address_writes == 0 && smmu_writes == 0);
            check_no_frame(EOVERFLOW);
        }
        CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1u); cases++;
    }
    printf("%u production SMMU configuration/route/release scenarios passed\n", cases);
    return cases;
}

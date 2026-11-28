/*
 * Host MMIO model for the production capture, stream and close paths. The writer is modelled as the
 * device showed it: it takes one queued address per frame at a register update (RUP_AUP) and writes
 * nothing when no address is queued.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "annibale_camera.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define NSEC_PER_USEC 1000u
#define NSEC_PER_MSEC 1000000u
#define PAGE_SIZE 16384u
#define CAMERA_SLOT_SIZE (8u * 1024u * 1024u)
#define CAMERA_REGION_SIZE (2u * CAMERA_SLOT_SIZE)
#define CAM_NATIVE_PROFILE_BYTES 6440u
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#undef bzero
#define bzero(p, n) memset((p), 0, (n))
#ifndef __unused
#define __unused __attribute__((unused))
#endif
static inline void say(const char *format, ...) { (void)format; }
#define cam_native_trace(...) ((void)0)
#ifndef __printflike
#define __printflike(a, b)
#endif
void annibale_tl(const char *format, ...) __printflike(1, 2);
void annibale_tl(const char *format, ...) { (void)format; }
static bool PE_parse_boot_argn(const char *name __unused, void *arg __unused, int size __unused) { return false; }
static void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result) { *result = abstime * 1000u; }
static void *current_proc(void) { return NULL; }
static int proc_pid(void *p __unused) { return 0; }
static void *current_thread(void) { return NULL; }
static uint64_t thread_tid(void *t __unused) { return 0; }

typedef uintptr_t vm_offset_t;
typedef int64_t user_ssize_t;
struct proc;
struct uio { uint8_t *next; user_ssize_t residual; };

static struct annibale_camera_profile profile_data, *cam_profile;
static struct annibale_camera_capture cam_capture;
static uint8_t region[CAMERA_REGION_SIZE], output[CAMERA_SLOT_SIZE];
static vm_offset_t cam_region_vbase, csid_lite_base = 1;
static vm_offset_t gcc_base = 2, mclkcc_base = 3, camcc_base = 4, tlmm_base = 5;
static void *cam_lock;
static uint16_t front_chip_id, wide_chip_id;
static uint8_t front_slave_addr, wide_slave_addr, wide_bus_mode;
static uint32_t cached_csid_ver, cached_ife_ver, cam_hw_mmio, current_sensor;
static uint32_t boot_cam_keep_stream, boot_cam_wm_prefetch;
static uint64_t cam_frame_seq;
static bool cam_is_open, cam_hw_dma_verified, cam_sensor_streaming, cci_ready, cam_session_probed, cam_mid_sampled;
static bool cam_native_phy_configured;
static uint32_t regs[0x2800 / 4];
static uint32_t top_clear, buf_clear, rx_clear, path_clear;
static uint64_t physical, second_physical;   /* second_physical: the second slot elsewhere, when set */
static unsigned address_writes, resets, table_writes[4], cache_flushes, vblank_polls;
static unsigned clock_starts, clock_stops, phy_starts, phy_stops;
static unsigned route_stops, smmu_restores, region_frees, profile_frees, power_stops;
static unsigned gpio_writes, shared_power_writes, completed_frames;
static bool clocks_owned, phy_owned, smmu_owned, delivered, power_up_ok, vblank_eof_seen;
static bool stale_reset, stale_buf, missing_finish_reset, fail_global_reset;
static bool wrong_address, stuck_fifo, fail_smmu_stop, fail_smmu_restore;
static bool fail_phy_stop, fail_clock_stop, bad_contiguity, invalid_status;
static bool wrong_group, fail_sensor_on, fail_sensor_off, fail_init, fatal_frame;
static bool bus_overflow, sticky_overflow;
static unsigned buf_polls, deliver_after;
static uint32_t queued[4], queued_count, taken_address, flushed_slots;
static bool taken, lose_update;
static uint32_t wm1_queued[4], wm1_queued_count, wm1_taken_address, pd_flushed_slots, last_rup_aup;
static bool wm1_taken, stuck_wm1_fifo, wm1_cache_written, fail_eeprom_read;
static unsigned cci_inits;
static uint8_t fake_eeprom[0x1840u];
static uint32_t cam_native_exp_writes, cam_native_focus_steps, cam_native_applied_frame_lines;

static bool cam_capture_resources_held(void);
static void fake_dma(void);

static uint32_t reg_rd(vm_offset_t base, uint32_t offset)
{
    if (base != csid_lite_base) return 0;
    CHECK(offset < sizeof(regs));
    if (offset == 0x08c && cam_sensor_streaming && regs[0x1700 / 4] == 1 && taken &&
        buf_polls++ >= deliver_after) fake_dma();
    if (invalid_status && cam_sensor_streaming && offset == 0x08c) return UINT32_MAX;
    if (stuck_fifo && address_writes && offset == 0x1798) return 1;
    if (stuck_wm1_fifo && offset == 0x1898) return 1;
    return regs[offset / 4];
}

static void reg_wr(vm_offset_t base, uint32_t offset, uint32_t value)
{
    if (base != csid_lite_base) { shared_power_writes++; return; }
    CHECK(offset < sizeof(regs));
    if (offset == 0x084) { top_clear = value; return; }
    if (offset == 0x094) { buf_clear = value; return; }
    if (offset == 0x0a4) { rx_clear = value; return; }
    if (offset == 0x0f4) { path_clear = value; return; }
    if (offset == 0x014) {
        CHECK(value == 1);
        if (!stale_reset) regs[0x07c / 4] &= ~top_clear;
        if (!stale_buf) regs[0x08c / 4] &= ~buf_clear;
        regs[0x09c / 4] &= ~rx_clear;
        regs[0x0ec / 4] &= ~path_clear;
        top_clear = buf_clear = rx_clear = path_clear = 0;
    }
    if (offset == 0x010) {
        resets++;
        /* A path reset waits for the end of a frame: none comes with the path off, or for a frame cut short. */
        bool no_eof = (value == 1 && !cam_sensor_streaming && (regs[0x500 / 4] == 0 || taken));
        if (!fail_global_reset && !no_eof && !(missing_finish_reset && resets > 1)) regs[0x07c / 4] |= 1;
    }
    if (offset == 0x1260) {
        if (!sticky_overflow) regs[0x1268 / 4] &= ~value;
        return;
    }
    if (offset == 0x1704) {
        CHECK(value == (uint32_t)(physical >> 8) || value == (uint32_t)((physical + CAMERA_SLOT_SIZE) >> 8));
        CHECK(queued_count < 4);
        queued[queued_count++] = value;
        address_writes++;
        regs[0x1798 / 4] = queued_count;
    }
    if (offset == 0x1868) wm1_cache_written = true;
    if (offset == 0x1804) {
        CHECK(value == (uint32_t)((physical + 0x600000u) >> 8) ||
              value == (uint32_t)((physical + CAMERA_SLOT_SIZE + 0x600000u) >> 8));
        CHECK(wm1_queued_count < 4);
        wm1_queued[wm1_queued_count++] = value;
        regs[0x1898 / 4] = wm1_queued_count;
    }
    if (offset == 0x018) {
        last_rup_aup = value;
        if (!taken && queued_count != 0 && !lose_update) {
            /* The register update hands the writer its next address. */
            taken_address = queued[0];
            memmove(queued, queued + 1, sizeof(queued) - sizeof(queued[0]));
            queued_count--; taken = true; buf_polls = 0;
        }
        if ((value & 0x200020u) == 0x200020u && !wm1_taken && wm1_queued_count != 0 && !lose_update) {
            wm1_taken_address = wm1_queued[0];
            memmove(wm1_queued, wm1_queued + 1, sizeof(wm1_queued) - sizeof(wm1_queued[0]));
            wm1_queued_count--; wm1_taken = true;
        }
    }
    if (offset == 0x010 || (offset == 0x1700 && value == 0)) {
        if (offset == 0x1700 || value == 2) { queued_count = 0; taken = false; }
    }
    if (offset == 0x010 || (offset == 0x1800 && value == 0)) {
        if (offset == 0x1800 || value == 2) { wm1_queued_count = 0; wm1_taken = false; }
    }
    regs[offset / 4] = value;
    if (offset == 0x1704 || offset == 0x018 || offset == 0x010 || offset == 0x1700)
        regs[0x1798 / 4] = stuck_fifo && address_writes ? 1 : queued_count;
    if (offset == 0x1804 || offset == 0x018 || offset == 0x010 || offset == 0x1800)
        regs[0x1898 / 4] = stuck_wm1_fifo ? 1 : wm1_queued_count;
}

static void csid_wr(uint32_t offset, uint32_t value) { reg_wr(csid_lite_base, offset, value); }
static void delay_for_interval(uint32_t n __unused, uint32_t unit __unused) { }
static uint64_t mach_absolute_time(void) { return 1000u + completed_frames; }
static uint64_t kvtophys(vm_offset_t va)
{
    uint64_t offset = va - cam_region_vbase;
    if (second_physical != 0 && offset >= CAMERA_SLOT_SIZE) return second_physical + offset - CAMERA_SLOT_SIZE;
    return physical + offset + (bad_contiguity && offset >= PAGE_SIZE ? PAGE_SIZE : 0);
}
void FlushPoC_DcacheRegion(vm_offset_t va, size_t bytes)
{
    if (va == cam_region_vbase + 0x600000u || va == cam_region_vbase + CAMERA_SLOT_SIZE + 0x600000u) {
        CHECK(bytes == 3200u * 480u);
        pd_flushed_slots |= (va == cam_region_vbase + 0x600000u) ? 1u : 2u;
        cache_flushes++;
        return;
    }
    CHECK((va == cam_region_vbase || va == cam_region_vbase + CAMERA_SLOT_SIZE) &&
          bytes >= profile_data.stride * profile_data.height);
    flushed_slots |= va == cam_region_vbase ? 1u : 2u;
    cache_flushes++;
}

static bool cci0_m1_read16(uint8_t slave, uint16_t reg, uint16_t *id)
{
    CHECK(slave == 0x10 && reg == 0x300a);
    *id = 0x5650;
    return true;
}

static bool cci0_m1_init(void)
{
    cci_inits++;
    cci_ready = true;
    return true;
}

static bool cci0_m1_read_bytes(uint8_t slave, uint16_t reg, uint8_t *out, uint32_t count)
{
    CHECK(slave == 0x51 && count > 0 && count <= 15u);
    if (fail_eeprom_read || (uint32_t)reg + count > sizeof(fake_eeprom)) {
        cci_ready = false;
        return false;
    }
    memcpy(out, fake_eeprom + reg, count);
    return true;
}

static bool od_i2c_read_bytes(uint32_t sda __unused, uint32_t scl __unused, uint8_t slave, uint16_t reg,
    uint8_t *out, uint32_t count)
{
    return cci0_m1_read_bytes(slave, reg, out, count);
}

static void cam_native_lens_up(void) { }
static void cam_native_lens_down(void) { }
static void cam_native_apply_focus(bool force __unused) { }

static bool od_i2c_read16_reg16(uint32_t sda, uint32_t scl, uint8_t slave, uint16_t reg, uint16_t *id)
{
    CHECK(sda == 4 && scl == 5 && slave == 0x10 && reg == 0x300b);
    *id = 0x2042;
    return true;
}

static bool cam_native_clocks_up(void) { clock_starts++; clocks_owned = true; return true; }
static bool cam_native_clocks_down(void)
{
    if (!clocks_owned) return true;
    if (fail_clock_stop) return false;
    clock_stops++; clocks_owned = false; return true;
}
static bool cam_native_phy_up(void) { phy_starts++; phy_owned = true; return true; }
static bool cam_native_phy_down(void)
{
    if (!phy_owned) return true;
    if (fail_phy_stop) return false;
    phy_stops++; phy_owned = false; return true;
}
static bool cam_native_sensor_power(bool up __unused) { return true; }
static bool cam_native_apply_exposure_regs(bool force __unused) { return true; }
static void cam_diag_bus_debug_enable(void) { }
static void cam_diag_midframe_sample(void) { }
static void cam_diag_capture_probe(const char *tag __unused) { }
static void cam_diag_capture_failure(uint32_t sof __unused, uint32_t overflow __unused, uint32_t waited __unused) { }
static bool cam_native_register_table(uint32_t table, uint32_t count)
{
    CHECK(table < 4 && count);
    table_writes[table]++;
    if (table == 0 && fail_init) return false;
    if (table == 2 && fail_sensor_on) return false;
    if (table == 3 && fail_sensor_off) return false;
    return true;
}

static bool cam_smmu_configure_bypass(void) { smmu_owned = true; return true; }
static bool cam_smmu_route(bool allow)
{
    CHECK(!allow && smmu_owned);
    CHECK(regs[0x200 / 4] == 0 && regs[0x204 / 4] == 0 && regs[0x500 / 4] == 0 && regs[0x1700 / 4] == 0);
    CHECK(regs[0x1798 / 4] == 0 && resets >= 2);
    route_stops++;
    return !fail_smmu_stop;
}
static bool cam_smmu_release(void)
{
    if (!smmu_owned) return true;
    if (fail_smmu_restore) return false;
    smmu_restores++; smmu_owned = false; return true;
}

static user_ssize_t uio_resid(struct uio *uio) { return uio->residual; }
static int uiomove(caddr_t data, int bytes, struct uio *uio)
{
    CHECK(bytes >= 0 && bytes <= uio->residual);
    memcpy(uio->next, data, (size_t)bytes);
    uio->next += bytes; uio->residual -= bytes;
    return 0;
}
static void lck_mtx_lock(void *lock __unused) { }
static void lck_mtx_unlock(void *lock __unused) { }
static void kfree_data(void *address, size_t bytes)
{
    CHECK(address == cam_profile && bytes == CAM_NATIVE_PROFILE_BYTES);
    CHECK(!cam_capture_resources_held());
    profile_frees++;
}
static void kmem_free(void *map __unused, vm_offset_t address, size_t bytes)
{
    CHECK(address == cam_region_vbase && bytes == CAMERA_REGION_SIZE);
    CHECK(!cam_capture_resources_held());
    region_frees++;
}
static void csid_lite_hardware_stop(void) { power_stops++; }
static void tlmm_set_out(uint32_t pin __unused, bool high __unused) { gpio_writes++; }
static void cam_vote_rpmh_supplies(bool enable) { CHECK(!enable); power_stops++; }
static bool cam_hardware_power_up(void) { return power_up_ok; }

#include "annibale_camera_exposure.inc"
#include "camera-production-slots.inc"
#include "annibale_camera_native.inc"
#include "camera-production-read-close.inc"

static void fake_dma(void)
{
    CHECK(taken && clocks_owned && phy_owned && smmu_owned);
    CHECK(regs[0x1700 / 4] == 1 && regs[0x504 / 4] == 1);
    CHECK(regs[0x510 / 4] == 0x8000 && regs[0x1718 / 4] == 0);
    CHECK(regs[0x170c / 4] == ((profile_data.height << 16) | (profile_data.stride / 16u)) &&
          regs[0x1714 / 4] == profile_data.stride);
    uint32_t slot = taken_address == (uint32_t)(physical >> 8) ? 0 : 1;
    /* The buffer the caller was last given is not written while it may be read. */
    CHECK(cam_capture.state != ANNIBALE_CAM_CAPTURE_STREAMING || slot != cam_native_ready);
    uint32_t active_bytes = (profile_data.width * 5u) / 4u;
    for (uint32_t y = 0; y < profile_data.height; y++) {
        uint8_t *line = region + slot * CAMERA_SLOT_SIZE + y * profile_data.stride;
        memset(line, (int)(0x30u + completed_frames), active_bytes);
        if (profile_data.stride > active_bytes) {
            memset(line + active_bytes, 0xa5, profile_data.stride - active_bytes);
        }
    }
    if (bus_overflow) {
        regs[0x1268 / 4] |= 1u;
    } else {
        regs[0x08c / 4] |= wrong_group ? 1u : 2u;
        regs[0x1790 / 4] = taken_address + (wrong_address ? 1u : 0u);
        if (wm1_taken && regs[0x1800 / 4] == 1 && regs[0x604 / 4] == 1) {
            uint32_t pd_slot = wm1_taken_address == (uint32_t)((physical + 0x600000u) >> 8) ? 0 : 1;
            memset(region + pd_slot * CAMERA_SLOT_SIZE + 0x600000u, (int)(0x70u + completed_frames), 3200u * 480u);
            regs[0x1890 / 4] = wm1_taken_address;
            regs[0x08c / 4] |= (1u << 2);
            wm1_taken = false;
        }
    }
    if (fatal_frame) regs[0x09c / 4] |= 0x800u;
    taken = false; flushed_slots = 0; delivered = true; completed_frames++;
}

static void reset_fixture(void)
{
    memset(&profile_data, 0, sizeof(profile_data));
    profile_data.sensor = 2; profile_data.mode = 4;
    profile_data.width = 2592; profile_data.height = 1458; profile_data.stride = 3248;
    profile_data.bayer = 2; profile_data.lane_count = 4; profile_data.lane_assign = 0x3210;
    profile_data.virtual_channel = 0; profile_data.data_type = 0x2b;
    profile_data.settle_count = 31; profile_data.frame_length = 1800;
    profile_data.init_count = 433; profile_data.mode_count = 81;
    profile_data.on_count = profile_data.off_count = 1;
    cam_profile = &profile_data;
    memset(&cam_capture, 0, sizeof(cam_capture));
    memset(regs, 0, sizeof(regs)); memset(region, 0xcc, sizeof(region));
    memset(output, 0xcc, sizeof(output));
    top_clear = buf_clear = rx_clear = path_clear = 0;
    cam_region_vbase = (vm_offset_t)region; physical = 0x123400000ULL; second_physical = 0;
    cam_is_open = power_up_ok = true; cam_hw_mmio = 1; current_sensor = 2;
    front_chip_id = 0x2042; front_slave_addr = 0x10;
    cached_csid_ver = 0x30000004; cached_ife_ver = 0x20050000;
    cam_sensor_streaming = cam_hw_dma_verified = cam_native_phy_configured = false;
    boot_cam_keep_stream = boot_cam_wm_prefetch = 0;
    boot_cam_pdaf = 1u;
    cam_native_initialized = cam_native_sensor_powered = cam_native_submitted = false;
    cam_native_path_configured = cam_native_reset_failed = false;
    cam_native_pd_configured = cam_native_pd_diag_logged = false;
    cam_native_pd_ready_flags = cam_native_pd_frame_flags = 0;
    cam_eeprom_valid = cam_eeprom_logged = false;
    wm1_queued_count = wm1_taken_address = pd_flushed_slots = last_rup_aup = 0;
    wm1_taken = stuck_wm1_fifo = wm1_cache_written = fail_eeprom_read = false;
    cci_inits = 0; cci_ready = true;
    memset(fake_eeprom, 0, sizeof(fake_eeprom));
    fake_eeprom[0x0001u] = 0x10u;
    fake_eeprom[0x002du] = 0x02u; fake_eeprom[0x002eu] = 0xa4u; /* macro 676 (BE) */
    fake_eeprom[0x0031u] = 0x01u; fake_eeprom[0x0032u] = 0x45u; /* inf 325 (BE) */
    fake_eeprom[0x0eefu] = 0x00u; fake_eeprom[0x0ef0u] = 17u;
    fake_eeprom[0x0ef1u] = 0x00u; fake_eeprom[0x0ef2u] = 13u;
    cam_native_dma_bytes = 0;
    clocks_owned = phy_owned = smmu_owned = delivered = vblank_eof_seen = false;
    address_writes = resets = cache_flushes = completed_frames = vblank_polls = 0;
    clock_starts = clock_stops = phy_starts = phy_stops = 0;
    route_stops = smmu_restores = region_frees = profile_frees = power_stops = 0;
    gpio_writes = shared_power_writes = 0;
    memset(table_writes, 0, sizeof(table_writes));
    stale_reset = stale_buf = missing_finish_reset = fail_global_reset = false;
    wrong_address = stuck_fifo = fail_smmu_stop = fail_smmu_restore = false;
    fail_phy_stop = fail_clock_stop = bad_contiguity = invalid_status = false;
    wrong_group = fail_sensor_on = fail_sensor_off = fail_init = fatal_frame = false;
    bus_overflow = sticky_overflow = false;
    buf_polls = deliver_after = 0;
    queued_count = taken_address = flushed_slots = 0; taken = lose_update = false;
    cam_native_writing = cam_native_ready = 0;
}

static void check_no_frame(int expected)
{
    struct uio uio = {output, sizeof(output)};
    memset(output, 0xcc, sizeof(struct annibale_camera_frame));
    CHECK(cam_native_read(&uio) == expected);
    CHECK(uio.residual == sizeof(output) && output[0] == 0xcc);
}

static void check_held(void)
{
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_HELD && !cam_hw_dma_verified);
    check_no_frame((int)cam_capture.error);
    unsigned old_addresses = address_writes, old_resets = resets;
    cam_native_capture();
    CHECK(address_writes == old_addresses && resets == old_resets);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(profile_frees == 0 && region_frees == 0 && cam_region_vbase && cam_profile);
    CHECK(annibale_cam_open(0, 0, 0, NULL) == EBUSY);
}

/* A frame without comp_done ends as FAILED with everything released, and the next capture starts over. */
static void check_recovered(uint32_t error, unsigned frames_before)
{
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && cam_capture.error == error);
    CHECK((cam_capture.flags & ANNIBALE_CAM_CAPTURE_BUF_DONE) == 0 && !cam_hw_dma_verified);
    CHECK(!cam_native_submitted && !cam_sensor_streaming && route_stops == frames_before + 1);
    CHECK(!clocks_owned && !phy_owned && !smmu_owned && regs[0x200 / 4] == 0);
    memset(output, 0xcc, sizeof(output));
    check_no_frame((int)error);
    wrong_group = bus_overflow = delivered = false;
    cam_native_capture();
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_STOPPED && cam_capture.error == 0);
    CHECK(cam_capture.flags == ANNIBALE_CAM_CAPTURE_COMPLETE && cam_hw_dma_verified);
    CHECK(cam_capture.sequence == frames_before + 1 && table_writes[0] == 2 && table_writes[1] == 2);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 0);
}

static void check_success(void)
{
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_STOPPED && cam_capture.error == 0);
    CHECK(cam_capture.flags == ANNIBALE_CAM_CAPTURE_COMPLETE && cam_hw_dma_verified);
    CHECK(cam_capture.dma_address == physical && cam_capture.consumed_addr == (physical >> 8));
    CHECK(cam_capture.sequence == completed_frames && (flushed_slots & 1u) != 0);
    struct uio uio = {output, sizeof(output)};
    CHECK(cam_native_read(&uio) == 0);
    struct annibale_camera_frame frame;
    memcpy(&frame, output, sizeof(frame));
    CHECK(memcmp(frame.magic, "RWFRAME1", 8) == 0 && frame.header_bytes == sizeof(frame));
    CHECK(frame.width == 2592 && frame.height == 1458 && frame.stride == 3248);
    CHECK(frame.format == 1 && frame.bayer == 2 && frame.sensor == 2);
    CHECK(frame.time == cam_capture.time && frame.sequence == completed_frames);
    CHECK(frame.black == 0 && frame.exposure_lines == 0 && frame.line_ns == 0);
    CHECK(frame.gain == 0 && frame.gain_unity == 0 && frame.gain_max == 0);
    CHECK(uio.residual == sizeof(output) - sizeof(frame) - 3248u * 1458u);
    CHECK(output[sizeof(frame)] == 0x2fu + completed_frames);
    CHECK(output[sizeof(frame) + 3240] == 0xa5 && output[sizeof(frame) + 3248] == 0x2fu + completed_frames);
}

/* A stream's frame: in the slot the writer just left, flushed, readable, and the other slot queued and taken. */
static void check_stream(unsigned frame)
{
    uint32_t slot = (frame - 1u) & 1u;
    uint64_t address = physical + slot * CAMERA_SLOT_SIZE, other = physical + (slot ^ 1u) * CAMERA_SLOT_SIZE;
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_STREAMING && cam_capture.error == 0 && cam_hw_dma_verified);
    CHECK(cam_capture.flags == (ANNIBALE_CAM_CAPTURE_BUF_DONE | ANNIBALE_CAM_CAPTURE_ADDR_MATCH));
    CHECK(ANNIBALE_CAM_CAPTURE_HAS_FRAME(&cam_capture) && !cam_capture_resources_held());
    CHECK(cam_capture.dma_address == address && cam_capture.consumed_addr == (uint32_t)(address >> 8));
    CHECK(cam_capture.sequence == frame && completed_frames == frame && (flushed_slots & (1u << slot)) != 0);
    CHECK(taken && taken_address == (uint32_t)(other >> 8) && queued_count == 0 && (regs[0x08c / 4] & 2u) == 0);
    CHECK(cam_sensor_streaming && regs[0x200 / 4] != 0 && regs[0x500 / 4] != 0 && regs[0x1700 / 4] == 1);
    CHECK(table_writes[2] >= 1 && cam_native_submitted);
    struct uio uio = {output, sizeof(output)};
    CHECK(cam_native_read(&uio) == 0);
    struct annibale_camera_frame header;
    memcpy(&header, output, sizeof(header));
    CHECK(header.sequence == frame && header.width == profile_data.width && header.stride == profile_data.stride);
    CHECK(output[sizeof(header)] == 0x2fu + frame && output[sizeof(header) + profile_data.stride] == 0x2fu + frame);
}

int main(void)
{
    unsigned cases = 0;
    reset_fixture(); cam_native_capture(); check_success(); cases++;
    CHECK(clock_starts == 1 && phy_starts == 1 && table_writes[0] == 1 && table_writes[1] == 1);
    delivered = false; cam_native_capture(); check_success(); cases++;
    CHECK(table_writes[0] == 1 && table_writes[1] == 1 && address_writes == 2 && table_writes[3] == 2);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(profile_frees == 1 && region_frees == 0 && power_stops == 2);
    CHECK(clock_stops == 1 && phy_stops == 1 && smmu_restores == 1); cases++;

    reset_fixture(); cam_capture.state = ANNIBALE_CAM_CAPTURE_READY;
    check_no_frame(EAGAIN); cases++;
    reset_fixture(); cam_native_capture();
    for (uint32_t bit = 1; bit <= ANNIBALE_CAM_CAPTURE_SMMU_SYNC; bit <<= 1) {
        cam_capture.flags = ANNIBALE_CAM_CAPTURE_COMPLETE & ~bit;
        check_no_frame(EAGAIN); cases++;
    }
    cam_capture.flags = ANNIBALE_CAM_CAPTURE_COMPLETE; cam_hw_dma_verified = false;
    check_no_frame(EAGAIN); cases++;
    cam_hw_dma_verified = true; cam_capture.state = ANNIBALE_CAM_CAPTURE_RUNNING;
    check_no_frame(EAGAIN); cases++;
    /* A stream's frame needs comp_done at the queued address; the stop flags belong to a single capture. */
    cam_capture.state = ANNIBALE_CAM_CAPTURE_STREAMING;
    cam_capture.flags = ANNIBALE_CAM_CAPTURE_COMPLETE & ~ANNIBALE_CAM_CAPTURE_ADDR_MATCH;
    check_no_frame(EAGAIN);
    cam_capture.flags = ANNIBALE_CAM_CAPTURE_COMPLETE & ~ANNIBALE_CAM_CAPTURE_BUF_DONE;
    check_no_frame(EAGAIN); cases++;

    reset_fixture(); wrong_address = true; cam_native_capture(); check_held();
    CHECK(route_stops == 0 && address_writes == 1); cases++;
    reset_fixture(); wrong_group = true; cam_native_capture(); check_recovered(ETIMEDOUT, 0); cases++;
    reset_fixture(); bus_overflow = true; cam_native_capture(); check_recovered(ENOBUFS, 0); cases++;
    /* A stream: frames alternate between the two slots without the sensor or the path stopping. */
    reset_fixture(); boot_cam_keep_stream = 1;
    for (unsigned frame = 1; frame <= 5; frame++) { cam_native_capture(); check_stream(frame); }
    CHECK(table_writes[2] == 1 && table_writes[3] == 0 && resets == 1 && address_writes == 6 && route_stops == 0);
    /* Close gives up the frame in flight: sensor off, global reset, everything released. */
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(!cam_sensor_streaming && table_writes[3] == 1 && regs[0x200 / 4] == 0 && regs[0x1700 / 4] == 0);
    CHECK(route_stops == 1 && smmu_restores == 1 && region_frees == 0 && profile_frees == 1 && !clocks_owned); cases++;
    /* ... and the camera opens and streams again. */
    cam_region_vbase = (vm_offset_t)region; completed_frames = 0;
    CHECK(annibale_cam_open(0, 0, 0, NULL) == 0);
    cam_profile = &profile_data;
    cam_native_capture(); check_stream(1); cam_native_capture(); check_stream(2);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0); cases++;
    /* A stream frame the writer drops, or that never completes: FAILED and released, the next call starts over. */
    for (unsigned kind = 0; kind < 2; kind++) {
        reset_fixture(); boot_cam_keep_stream = 1; cam_native_capture(); check_stream(1);
        if (kind == 0) bus_overflow = true; else wrong_group = true;
        cam_native_capture();
        CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && cam_capture.error == (kind == 0 ? ENOBUFS : ETIMEDOUT));
        CHECK(!cam_hw_dma_verified || !ANNIBALE_CAM_CAPTURE_HAS_FRAME(&cam_capture));
        CHECK(!cam_native_submitted && !cam_sensor_streaming && !clocks_owned && !smmu_owned && table_writes[3] == 1);
        check_no_frame((int)cam_capture.error);
        bus_overflow = wrong_group = false; regs[0x1268 / 4] = 0;
        cam_native_capture();
        /* The stream starts over in the first slot; the frame count goes on from the last good frame. */
        CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_STREAMING && ANNIBALE_CAM_CAPTURE_HAS_FRAME(&cam_capture));
        CHECK(cam_capture.dma_address == physical && cam_capture.sequence == 2 && cam_hw_dma_verified);
        CHECK(table_writes[0] == 2 && table_writes[2] == 2);
        CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 0); cases++;
    }
    /* A stream frame done at another address than the queued one keeps everything. */
    reset_fixture(); boot_cam_keep_stream = 1; cam_native_capture(); check_stream(1);
    wrong_address = true; cam_native_capture(); check_held(); CHECK(route_stops == 0); cases++;
    /* The register update lost: no frame follows, and the call fails instead of handing out an old buffer. */
    reset_fixture(); boot_cam_keep_stream = 1; cam_native_capture(); check_stream(1);
    lose_update = true; cam_native_capture();
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_STREAMING); lose_update = false;
    cam_native_capture(); CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && cam_capture.error == ETIMEDOUT);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 0); cases++;
    /* Close during a stream with the FIFO stuck: held, nothing freed. */
    reset_fixture(); boot_cam_keep_stream = 1; cam_native_capture(); check_stream(1);
    stuck_fifo = true; CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_HELD && region_frees == 0 && profile_frees == 0);
    CHECK(annibale_cam_open(0, 0, 0, NULL) == EBUSY); cases++;
    reset_fixture(); wrong_group = stuck_fifo = true; cam_native_capture(); check_held();
    CHECK(cam_capture.error == ETIMEDOUT && route_stops == 0); cases++;
    reset_fixture(); sticky_overflow = true; deliver_after = 3; regs[0x1268 / 4] = 1; cam_native_capture(); check_success(); cases++;
    reset_fixture(); missing_finish_reset = true; cam_native_capture(); check_held();
    CHECK(route_stops == 0 && cam_native_reset_failed); cases++;
    reset_fixture(); fail_global_reset = true; cam_native_capture(); check_held();
    CHECK(address_writes == 0 && resets == 1); cases++;
    reset_fixture(); stuck_fifo = true; cam_native_capture(); check_held();
    CHECK(route_stops == 0); cases++;
    reset_fixture(); fail_smmu_stop = true; cam_native_capture(); check_held();
    CHECK(route_stops == 1 && smmu_restores == 0); cases++;
    reset_fixture(); invalid_status = true; cam_native_capture(); check_held();
    CHECK((cam_capture.flags & ANNIBALE_CAM_CAPTURE_BUF_DONE) == 0); cases++;
    reset_fixture(); fail_sensor_on = true; cam_native_capture(); check_held(); cases++;

    reset_fixture(); stale_reset = true; regs[0x07c / 4] = 1;
    cam_native_capture(); check_held(); CHECK(address_writes == 0 && resets == 0); cases++;
    reset_fixture(); stale_buf = true; regs[0x08c / 4] = 2;
    cam_native_capture(); CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && address_writes == 0);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 0); cases++;

    reset_fixture(); fail_init = true; cam_native_capture();
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && address_writes == 0);
    CHECK(!clocks_owned && !phy_owned && !cam_native_sensor_powered);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 0); cases++;
    reset_fixture(); bad_contiguity = true; cam_native_capture();
    CHECK(cam_capture.error == EOVERFLOW && address_writes == 0 && clock_starts == 0); cases++;
    reset_fixture(); physical = 1ULL << 36; cam_native_capture();
    CHECK(cam_capture.error == EOVERFLOW && address_writes == 0); cases++;
    reset_fixture(); physical = (1ULL << 36) - 0x10000u; second_physical = 0x123400000ULL; cam_native_capture();
    CHECK(cam_capture.error == EOVERFLOW && address_writes == 0); cases++;
    /* The second slot is checked like the first. */
    reset_fixture(); second_physical = (1ULL << 36) - 0x10000u; cam_native_capture();
    CHECK(cam_capture.error == EOVERFLOW && address_writes == 0); cases++;
    reset_fixture(); second_physical = 0x200000001ULL; cam_native_capture();
    CHECK(cam_capture.error == EOVERFLOW && address_writes == 0); cases++;
    reset_fixture(); physical++; cam_native_capture();
    CHECK(cam_capture.error == EOVERFLOW && address_writes == 0); cases++;

    reset_fixture(); regs[0x1800 / 4] = 1; cam_native_capture();
    CHECK(cam_capture.error == EBUSY && clock_starts == 0 && phy_starts == 0 && address_writes == 0);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(power_stops == 0 && gpio_writes == 0 && shared_power_writes == 0); cases++;
    reset_fixture(); regs[0x1398 / 4] = 1; cam_native_capture();
    CHECK(cam_capture.error == EBUSY && clock_starts == 0); cases++;
    reset_fixture(); regs[0x500 / 4] = UINT32_MAX; cam_native_capture();
    CHECK(cam_capture.error == EBUSY && clock_starts == 0); cases++;

    reset_fixture(); fatal_frame = true; cam_native_capture();
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && cam_capture.error == EIO);
    CHECK(!cam_hw_dma_verified && !cam_native_submitted && route_stops == 1);
    check_no_frame(EIO); CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 0); cases++;
    reset_fixture(); fail_sensor_off = true; cam_native_capture();
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && !cam_hw_dma_verified);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 0); cases++;

    reset_fixture(); cam_native_capture(); fail_smmu_restore = true;
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_HELD && region_frees == 0 && profile_frees == 0); cases++;
    reset_fixture(); cam_native_capture(); fail_phy_stop = true;
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_HELD && region_frees == 0 && clocks_owned); cases++;
    reset_fixture(); cam_native_capture(); fail_clock_stop = true;
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_HELD && region_frees == 0); cases++;

    reset_fixture(); CHECK(annibale_cam_open(0, 0, 0, NULL) == EBUSY); cases++;
    reset_fixture(); cam_is_open = false; power_up_ok = false;
    CHECK(annibale_cam_open(0, 0, 0, NULL) == EIO && region_frees == 0); cases++;

    /* Verify rear wide mode 24 (2560x1920) exposure defaults, clamping, and frame black level. */
    reset_fixture();
    profile_data.sensor = 1; profile_data.mode = 24;
    profile_data.width = 2560; profile_data.height = 1920; profile_data.stride = 3200;
    cam_native_exposure_defaults(&profile_data);
    CHECK(cam_native_frame_lines == 6250u && cam_native_exposure_lines == 6218u && cam_native_gain == 256u);
    CHECK(cam_native_exposure_set(&profile_data, 3000u, 1024u, 6250u));
    cam_native_applied_exposure_lines = cam_native_exposure_lines;
    cam_native_applied_gain = cam_native_gain;
    cam_native_applied_frame_lines = cam_native_frame_lines;
    cam_native_exposure_shown(true);
    struct annibale_camera_frame back_frame = {0};
    cam_native_frame_exposure(&profile_data, &back_frame);
    CHECK(back_frame.black == 64u && back_frame.exposure_lines == 3000u && back_frame.line_ns == 5333u);
    CHECK(back_frame.gain == 1024u && back_frame.gain_unity == 256u && back_frame.gain_max == 4096u);
    cam_native_exposure_reset();
    cases++;

    /* The rear wide camera streams the same way. */
    reset_fixture();
    boot_cam_keep_stream = 1;
    wide_chip_id = WIDE_SENSOR_ID_EXPECTED; wide_slave_addr = 0x10; wide_bus_mode = 1;
    profile_data.sensor = 1; profile_data.mode = 24;
    profile_data.width = 2560; profile_data.height = 1920; profile_data.stride = 3200;
    for (unsigned frame = 1; frame <= 3; frame++) { cam_native_capture(); check_stream(frame); }
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(!cam_sensor_streaming && regs[0x200 / 4] == 0 && table_writes[3] == 1 && region_frees == 0);
    cases++;

    /* Verify cam_wm_prefetch=0 vs 1 writes IFE-lite0 WM0 0x1760/0x1764 accordingly. */
    reset_fixture();
    cam_native_capture();
    check_success();
    CHECK(regs[0x1760 / 4] == 0u && regs[0x1764 / 4] == 0u);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    reset_fixture();
    boot_cam_wm_prefetch = 1u;
    cam_native_capture();
    check_success();
    CHECK(regs[0x1760 / 4] == 1u && regs[0x1764 / 4] == UINT32_MAX);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    cases++;

    /* Verify rear wide Mode 24 PDAF RDI1+WM1 capture, legacy vs PD read, WM1 cache/FIFO guards, and paged EEPROM ioctl. */
    reset_fixture();
    boot_cam_keep_stream = 1;
    wide_chip_id = WIDE_SENSOR_ID_EXPECTED; wide_slave_addr = 0x10; wide_bus_mode = 1;
    profile_data.sensor = 1; profile_data.mode = 24;
    profile_data.width = 2560; profile_data.height = 1920; profile_data.stride = 3200;
    cam_native_capture();
    CHECK(ANNIBALE_CAM_CAPTURE_HAS_FRAME(&cam_capture));
    CHECK(last_rup_aup == 0x300030u && !wm1_cache_written);
    CHECK(regs[0x600 / 4] != 0 && regs[0x1800 / 4] == 1u && cam_eeprom_valid);
    CHECK((pd_flushed_slots & 1u) != 0);
    /* Legacy caller reads header + main_bytes only. */
    size_t main_len = sizeof(struct annibale_camera_frame) + 3200u * 1920u;
    struct uio u_legacy = { .next = output, .residual = (user_ssize_t)main_len };
    CHECK(cam_native_read(&u_legacy) == 0 && u_legacy.residual == 0);
    const struct annibale_camera_frame *pf = (const struct annibale_camera_frame *)output;
    CHECK((pf->pd_flags & ANNIBALE_CAM_PD_VALID) != 0 && pf->pd_width == 2560u &&
          pf->pd_height == 480u && pf->pd_stride == 3200u && pf->pd_bytes == 3200u * 480u);
    /* PD-aware caller reads header + main_bytes + pd_bytes. */
    uint8_t *big = malloc(main_len + 3200u * 480u);
    CHECK(big != NULL);
    struct uio u_pd = { .next = big, .residual = (user_ssize_t)(main_len + 3200u * 480u) };
    CHECK(cam_native_read(&u_pd) == 0 && u_pd.residual == 0);
    CHECK(big[main_len] == 0x70u && big[main_len + 3200u * 480u - 1u] == 0x70u);
    free(big);
    /* Paged CAMIOC_G_EEPROM ioctl returns cached bytes across pages. */
    struct annibale_camera_eeprom er = { .offset = 0, .length = ANNIBALE_CAM_EEPROM_CHUNK };
    CHECK(cam_native_eeprom_ioctl(&er) == 0 && (er.flags & ANNIBALE_CAM_EEPROM_FLAG_VALID) != 0);
    CHECK(er.total_bytes == CAM_EEPROM_CACHE_BYTES && er.length == ANNIBALE_CAM_EEPROM_CHUNK);
    CHECK(er.data[0x0001u] == 0x10u && er.data[0x002du] == 0x02u && er.data[0x002eu] == 0xa4u);
    er.offset = 0x1800u; er.length = ANNIBALE_CAM_EEPROM_CHUNK;
    CHECK(cam_native_eeprom_ioctl(&er) == 0 && er.length == 0x40u);
    /* Stuck WM1 FIFO blocks clean close and holds resources. */
    stuck_wm1_fifo = true;
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_HELD);
    cases++;

    /* EEPROM read failure on CCI0 M1 resets/recovers CCI so main stream still captures cleanly. */
    reset_fixture();
    boot_cam_keep_stream = 1;
    wide_chip_id = WIDE_SENSOR_ID_EXPECTED; wide_slave_addr = 0x10; wide_bus_mode = 1;
    profile_data.sensor = 1; profile_data.mode = 24;
    profile_data.width = 2560; profile_data.height = 1920; profile_data.stride = 3200;
    fail_eeprom_read = true;
    cam_native_capture();
    CHECK(ANNIBALE_CAM_CAPTURE_HAS_FRAME(&cam_capture));
    CHECK(!cam_eeprom_valid && cci_inits == 1u && cci_ready);
    struct annibale_camera_eeprom er_empty = { .offset = 0, .length = ANNIBALE_CAM_EEPROM_CHUNK };
    CHECK(cam_native_eeprom_ioctl(&er_empty) == 0 && er_empty.total_bytes == 0 && er_empty.flags == 0);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    cases++;

    printf("%u native capture/read/close scenarios passed\n", cases);
    return 0;
}

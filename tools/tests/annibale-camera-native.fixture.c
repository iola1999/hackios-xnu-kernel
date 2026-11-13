/* Host MMIO model for the production one-address capture and close paths. */
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
#define CAMERA_REGION_SIZE (8u * 1024u * 1024u)
#define CAM_NATIVE_PROFILE_BYTES 6440u
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#undef bzero
#define bzero(p, n) memset((p), 0, (n))
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define say(...) ((void)0)
#define cam_native_trace(...) ((void)0)

typedef uintptr_t vm_offset_t;
typedef int64_t user_ssize_t;
struct proc;
struct uio { uint8_t *next; user_ssize_t residual; };

static struct annibale_camera_profile profile_data, *cam_profile;
static struct annibale_camera_capture cam_capture;
static uint8_t region[CAMERA_REGION_SIZE], output[CAMERA_REGION_SIZE];
static vm_offset_t cam_region_vbase, csid_lite_base = 1;
static vm_offset_t gcc_base = 2, mclkcc_base = 3, camcc_base = 4, tlmm_base = 5;
static void *kernel_map, *cam_lock;
static uint16_t front_chip_id;
static uint8_t front_slave_addr, wide_bus_mode;
static uint32_t cached_csid_ver, cached_ife_ver, cam_hw_mmio, current_sensor;
static uint64_t cam_frame_seq;
static bool cam_is_open, cam_hw_dma_verified, cam_sensor_streaming, cci_ready;
static uint32_t regs[0x2800 / 4];
static uint32_t top_clear, buf_clear, rx_clear, path_clear;
static uint64_t physical;
static unsigned address_writes, resets, table_writes[4], cache_flushes;
static unsigned clock_starts, clock_stops, phy_starts, phy_stops;
static unsigned route_stops, smmu_restores, region_frees, profile_frees, power_stops;
static unsigned gpio_writes, shared_power_writes, completed_frames;
static bool clocks_owned, phy_owned, smmu_owned, delivered, power_up_ok;
static bool stale_reset, stale_buf, missing_finish_reset, fail_global_reset;
static bool wrong_address, stuck_fifo, fail_smmu_stop, fail_smmu_restore;
static bool fail_phy_stop, fail_clock_stop, bad_contiguity, invalid_status;
static bool wrong_group, fail_sensor_on, fail_sensor_off, fail_init, fatal_frame;

static bool cam_capture_resources_held(void);
static void fake_dma(void);

static uint32_t reg_rd(vm_offset_t base, uint32_t offset)
{
    if (base != csid_lite_base) return 0;
    CHECK(offset < sizeof(regs));
    if (offset == 0x08c && cam_sensor_streaming && !delivered) fake_dma();
    if (invalid_status && cam_sensor_streaming && offset == 0x08c) return UINT32_MAX;
    if (stuck_fifo && address_writes && offset == 0x1798) return 1;
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
        if (!fail_global_reset && !(missing_finish_reset && resets > 1)) regs[0x07c / 4] |= 1;
    }
    if (offset == 0x1704) {
        CHECK(!delivered);
        CHECK((physical >> 8) == value);
        address_writes++;
        regs[0x1798 / 4] = 1;
    }
    regs[offset / 4] = value;
}

static void csid_wr(uint32_t offset, uint32_t value) { reg_wr(csid_lite_base, offset, value); }
static void delay_for_interval(uint32_t n __unused, uint32_t unit __unused) { }
static uint64_t mach_absolute_time(void) { return 1000u + completed_frames; }
static uint64_t kvtophys(vm_offset_t va)
{
    uint64_t offset = va - cam_region_vbase;
    return physical + offset + (bad_contiguity && offset >= PAGE_SIZE ? PAGE_SIZE : 0);
}
void FlushPoC_DcacheRegion(vm_offset_t va, size_t bytes)
{
    CHECK(va == cam_region_vbase && bytes >= profile_data.stride * profile_data.height);
    cache_flushes++;
}

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
    CHECK(regs[0x200 / 4] == 0 && regs[0x500 / 4] == 0 && regs[0x1700 / 4] == 0);
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

#include "annibale_camera_native.inc"
#include "camera-production-read-close.inc"

static void fake_dma(void)
{
    CHECK(address_writes && clocks_owned && phy_owned && smmu_owned);
    CHECK(regs[0x1700 / 4] == 1 && regs[0x504 / 4] == 1);
    CHECK(regs[0x510 / 4] == 0x8000 && regs[0x1718 / 4] == 0);
    CHECK(regs[0x170c / 4] == ((1458u << 16) | 203u) && regs[0x1714 / 4] == 3248);
    CHECK(cache_flushes == 2u * completed_frames + 1u);
    CHECK(!cam_hw_dma_verified && cam_capture.state == ANNIBALE_CAM_CAPTURE_RUNNING);
    for (uint32_t y = 0; y < profile_data.height; y++) {
        uint8_t *line = region + y * profile_data.stride;
        memset(line, (int)(0x30u + completed_frames), 3240);
        memset(line + 3240, 0xa5, 8);
    }
    regs[0x08c / 4] |= wrong_group ? 1u : 2u;
    regs[0x1790 / 4] = regs[0x1704 / 4] + (wrong_address ? 1u : 0u);
    regs[0x1798 / 4] = 0;
    if (fatal_frame) regs[0x09c / 4] |= 0x800u;
    delivered = true; completed_frames++;
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
    cam_region_vbase = (vm_offset_t)region; physical = 0x123400000ULL;
    cam_is_open = power_up_ok = true; cam_hw_mmio = 1; current_sensor = 2;
    front_chip_id = 0x2042; front_slave_addr = 0x10;
    cached_csid_ver = 0x30000004; cached_ife_ver = 0x20050000;
    cam_sensor_streaming = cam_hw_dma_verified = false;
    cam_native_initialized = cam_native_sensor_powered = cam_native_submitted = false;
    cam_native_path_configured = cam_native_reset_failed = false;
    cam_native_dma_bytes = 0;
    clocks_owned = phy_owned = smmu_owned = delivered = false;
    address_writes = resets = cache_flushes = completed_frames = 0;
    clock_starts = clock_stops = phy_starts = phy_stops = 0;
    route_stops = smmu_restores = region_frees = profile_frees = power_stops = 0;
    gpio_writes = shared_power_writes = 0;
    memset(table_writes, 0, sizeof(table_writes));
    stale_reset = stale_buf = missing_finish_reset = fail_global_reset = false;
    wrong_address = stuck_fifo = fail_smmu_stop = fail_smmu_restore = false;
    fail_phy_stop = fail_clock_stop = bad_contiguity = invalid_status = false;
    wrong_group = fail_sensor_on = fail_sensor_off = fail_init = fatal_frame = false;
}

static void check_no_frame(int expected)
{
    struct uio uio = {output, sizeof(output)};
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

static void check_success(void)
{
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_STOPPED && cam_capture.error == 0);
    CHECK(cam_capture.flags == ANNIBALE_CAM_CAPTURE_COMPLETE && cam_hw_dma_verified);
    CHECK(cam_capture.dma_address == physical && cam_capture.consumed_addr == (physical >> 8));
    CHECK(cam_capture.sequence == completed_frames && cache_flushes == 2u * completed_frames);
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

int main(void)
{
    unsigned cases = 0;
    reset_fixture(); cam_native_capture(); check_success(); cases++;
    CHECK(clock_starts == 1 && phy_starts == 1 && table_writes[0] == 1 && table_writes[1] == 1);
    delivered = false; cam_native_capture(); check_success(); cases++;
    CHECK(table_writes[0] == 1 && table_writes[1] == 1 && address_writes == 2);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0);
    CHECK(profile_frees == 1 && region_frees == 1 && power_stops == 2);
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

    reset_fixture(); wrong_address = true; cam_native_capture(); check_held();
    CHECK(route_stops == 0 && address_writes == 1); cases++;
    reset_fixture(); wrong_group = true; cam_native_capture(); check_held();
    CHECK(cam_capture.error == ETIMEDOUT && route_stops == 0); cases++;
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
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1); cases++;

    reset_fixture(); fail_init = true; cam_native_capture();
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && address_writes == 0);
    CHECK(!clocks_owned && !phy_owned && !cam_native_sensor_powered);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1); cases++;
    reset_fixture(); bad_contiguity = true; cam_native_capture();
    CHECK(cam_capture.error == EOVERFLOW && address_writes == 0 && clock_starts == 0); cases++;
    reset_fixture(); physical = 1ULL << 36; cam_native_capture();
    CHECK(cam_capture.error == EOVERFLOW && address_writes == 0); cases++;
    reset_fixture(); physical = (1ULL << 36) - 0x10000u; cam_native_capture();
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
    check_no_frame(EIO); CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1); cases++;
    reset_fixture(); fail_sensor_off = true; cam_native_capture();
    CHECK(cam_capture.state == ANNIBALE_CAM_CAPTURE_FAILED && !cam_hw_dma_verified);
    CHECK(annibale_cam_close(0, 0, 0, NULL) == 0 && region_frees == 1); cases++;

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
    CHECK(annibale_cam_open(0, 0, 0, NULL) == EIO && region_frees == 1); cases++;
    printf("%u native capture/read/close scenarios passed\n", cases);
    return 0;
}

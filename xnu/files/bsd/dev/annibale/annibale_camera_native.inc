/*
 * Native frames through IFE-lite0 RDI0 and write master 0.
 *
 * cam_keep_stream=1 (the default): the path is configured once and stays enabled. The writer takes
 * one address per frame from its FIFO and writes nothing when the FIFO is empty, so each
 * CAMIOC_CAPTURE waits for the frame in flight, queues the other of two buffers with a register
 * update (RUP_AUP), and hands back the finished one. A caller slower than the sensor loses frames,
 * never a buffer it is reading. The stream ends at release, or at the first frame that fails.
 *
 * cam_keep_stream=0: one frame per call: one address, comp_done, consumed-address match, sensor off,
 * CSID reset, writer disabled, then SMMU fault/sync.
 *
 * A missing completion keeps DMA memory and hardware resources owned here.
 */
#include "annibale_camera_clock.inc"
#include "annibale_camera_phy.inc"

#define CAM980_RESET_CONFIG      0x00cu
#define CAM980_RESET_COMMAND     0x010u
#define CAM980_IRQ_COMMAND       0x014u
#define CAM980_RUP_AUP           0x018u
#define CAM980_TOP_STATUS        0x07cu
#define CAM980_TOP_MASK          0x080u
#define CAM980_TOP_CLEAR         0x084u
#define CAM980_BUF_STATUS        0x08cu
#define CAM980_BUF_MASK          0x090u
#define CAM980_BUF_CLEAR         0x094u
#define CAM980_RX_STATUS         0x09cu
#define CAM980_RX_MASK           0x0a0u
#define CAM980_RX_CLEAR          0x0a4u
#define CAM980_PATH_STATUS       0x0ecu
#define CAM980_PATH_MASK         0x0f0u
#define CAM980_PATH_CLEAR        0x0f4u
#define CAM980_RX_CFG0           0x200u
#define CAM980_RX_CFG1           0x204u
#define CAM980_PATH_CFG0         0x500u
#define CAM980_PATH_CONTROL      0x504u
#define CAM980_PATH_CFG1         0x510u
#define CAM980_PATH_OVERFLOW     0x514u
#define CAM980_WM_CFG            0x1700u
#define CAM980_WM_ADDRESS        0x1704u
#define CAM980_WM_FRAME_INCR     0x1708u
#define CAM980_WM_IMAGE_CFG0     0x170cu
#define CAM980_WM_IMAGE_CFG1     0x1710u
#define CAM980_WM_IMAGE_CFG2     0x1714u
#define CAM980_WM_PACKER         0x1718u
#define CAM980_WM_HEADER         0x1728u
#define CAM980_WM_LINE_DONE      0x172cu
#define CAM980_WM_IRQ_PERIOD     0x1730u
#define CAM980_WM_IRQ_PATTERN    0x1734u
#define CAM980_WM_PREFETCH       0x1760u
#define CAM980_WM_PREFETCH_MASK  0x1764u
#define CAM980_WM_CACHE          0x1768u
#define CAM980_WM_ADDR_CONFIG    0x1770u
#define CAM980_WM_CONSUMED       0x1790u
#define CAM980_WM_FIFO           0x1798u
#define CAM980_BUS_OVERFLOW_CLEAR 0x1260u
#define CAM980_BUS_OVERFLOW      0x1268u
#define CAM980_RDI0_DONE         2u
#define CAM980_PATH_INPUT_EOF    0x200u
#define CAM980_RX_FATAL          0x019fa800u
#define CAM980_PATH_FATAL        0x20186001u

extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void annibale_tl(const char *format, ...) __printflike(1, 2);
#ifdef KERNEL
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);
#endif
static bool cam_native_initialized, cam_native_sensor_powered, cam_native_submitted;
static bool cam_native_path_configured, cam_native_reset_failed;
static uint32_t cam_native_dma_bytes;
static uint64_t cam_native_slot_address[2];
static uint32_t cam_native_sof_ms, cam_native_overflow_ms, cam_native_overflow_stale;
static uint32_t cam_native_wait_spun;   /* the wait loop's own microseconds where it last looked */

#include "annibale_camera_pdaf.inc"

/*
 * Errors after which no comp_done can arrive. The sensor is stopped and CSID is reset globally;
 * the disabled path and writer and the empty FIFO then end the DMA in place of comp_done.
 */
static bool
cam_native_recoverable(uint32_t error)
{
	return error == ETIMEDOUT || error == ENOBUFS;
}

static bool
cam_native_front_mode4(void)
{
	return cam_hw_mmio != 0 && front_chip_id == FRONT_SENSOR_ID_EXPECTED &&
	       cached_csid_ver == 0x30000004u && cached_ife_ver == 0x20050000u;
}

static bool
cam_native_back_mode24(void)
{
	return cam_hw_mmio != 0 && wide_chip_id == WIDE_SENSOR_ID_EXPECTED &&
	       cached_csid_ver == 0x30000004u && cached_ife_ver == 0x20050000u;
}

static bool
cam_native_reset(bool global)
{
	if (cam_native_reset_failed) {
		return false;
	}
	csid_wr(CAM980_TOP_CLEAR, 1u);
	csid_wr(CAM980_IRQ_COMMAND, 1u);
	if (!cam_poll(csid_lite_base, CAM980_TOP_STATUS, 1u, 0, 5000u)) {
		return false;
	}
	csid_wr(CAM980_TOP_MASK, 1u);
	if (global) {
		csid_wr(CAM980_RX_CFG0, 0);
		csid_wr(CAM980_RX_CFG1, 0);
	}
	csid_wr(CAM980_RESET_CONFIG, global ? 0x11u : 1u);
	csid_wr(CAM980_RESET_COMMAND, global ? 2u : 1u);
	bool done = cam_poll(csid_lite_base, CAM980_TOP_STATUS, 1u, 1u, 100000u);
	cam_capture.reset_status = reg_rd(csid_lite_base, CAM980_TOP_STATUS);
	done = done && cam_capture.reset_status != UINT32_MAX &&
	    (cam_capture.reset_status & 1u) != 0;
	if (done) {
		csid_wr(CAM980_TOP_CLEAR, 1u);
		csid_wr(CAM980_IRQ_COMMAND, 1u);
	} else {
		/* The reset command may still be executing; retain its clocks and resources. */
		cam_native_reset_failed = true;
	}
	return done;
}

static bool
cam_native_idle(void)
{
	static const uint32_t paths[] = { 0x300u, 0x500u, 0x600u, 0x700u, 0x800u };
	static const uint32_t writers[] = { 0x1700u, 0x1800u, 0x1900u, 0x1a00u, 0x1300u, 0x1400u };
	for (size_t i = 0; i < sizeof(paths) / sizeof(uint32_t); i++) {
		uint32_t value = reg_rd(csid_lite_base, paths[i]);
		if (value == UINT32_MAX || (value & (1u << 31)) != 0) {
			return false;
		}
	}
	for (size_t i = 0; i < sizeof(writers) / sizeof(uint32_t); i++) {
		uint32_t value = reg_rd(csid_lite_base, writers[i]);
		if (value == UINT32_MAX || (value & 1u) != 0 ||
		    reg_rd(csid_lite_base, writers[i] + 0x98u) != 0) {
			return false;
		}
	}
	return true;
}

/* No address is published until both slots' complete DMA ranges have been checked. */
static bool
cam_native_buffer(void)
{
	cam_native_dma_bytes = (cam_capture.bytes + 511u) & ~511u;
	if (cam_region_vbase == 0 || cam_native_dma_bytes == 0 ||
	    cam_native_dma_bytes > CAMERA_SLOT_SIZE) {
		return false;
	}
	for (unsigned slot = 0; slot < 2u; slot++) {
		vm_offset_t base = cam_region_vbase + slot * CAMERA_SLOT_SIZE;
		uint64_t first = kvtophys(base);
		if (first == 0 || (first & 511u) != 0 ||
		    first >= (1ULL << 36) || cam_native_dma_bytes > (1ULL << 36) - first) {
			return false;
		}
		for (uint32_t offset = 0; offset < cam_native_dma_bytes; offset += PAGE_SIZE) {
			if (kvtophys(base + offset) != first + offset) {
				return false;
			}
		}
		cam_native_slot_address[slot] = first;
	}
	cam_capture.dma_address = cam_native_slot_address[0];
	return cam_native_pd_buffer_check();
}

static bool cam_native_finish(bool abandon);

static bool
cam_native_release(void)
{
	if (cam_capture.state == ANNIBALE_CAM_CAPTURE_STREAMING) {
		/* The frame in flight is given up: sensor off, CSID reset, writer and FIFO checked empty. */
		cam_capture.state = cam_native_finish(true) ? ANNIBALE_CAM_CAPTURE_STOPPED : ANNIBALE_CAM_CAPTURE_HELD;
	}
	if (cam_native_submitted || cam_native_reset_failed || cam_capture_resources_held()) {
		say("release blocked: submitted=%u reset_fail=%u state=%u\n",
		    cam_native_submitted ? 1u : 0u, cam_native_reset_failed ? 1u : 0u, cam_capture.state);
		return false;
	}
	bool had_stream = cam_sensor_streaming;
	if (cam_native_sensor_powered && cam_sensor_streaming && cam_profile != NULL) {
		(void)cam_native_register_table(3u, cam_profile->off_count);
		cam_sensor_streaming = false;
		delay_for_interval(40000, NSEC_PER_USEC);
	}
	if (cam_native_path_configured || had_stream) {
		/* Sensor MIPI clock is already stopped above; reset CSID immediately rather than waiting for EOF. */
		if (!cam_native_reset(true)) {
			say("release failed: global reset (top=%#x)\n", cam_capture.reset_status);
			return false;
		}
		csid_wr(CAM980_RX_CFG0, 0);
		csid_wr(CAM980_RX_CFG1, 0);
		csid_wr(CAM980_PATH_CFG0, 0);
		csid_wr(CAM980_WM_CFG, 0);
		cam_native_pd_disable_regs();
		cam_native_path_configured = false;
	}
	if (!cam_smmu_release()) {
		say("release failed: smmu\n");
		return false;
	}
	if (cam_native_sensor_powered) {
		cam_native_lens_down();
		if (!cam_native_sensor_power(false)) {
			say("release failed: sensor power off\n");
			return false;
		}
		cam_native_sensor_powered = false;
	}
	if (!cam_native_phy_down()) {
		say("release failed: phy down\n");
		return false;
	}
	if (!cam_native_clocks_down()) {
		say("release failed: clocks down\n");
		return false;
	}
	cam_native_initialized = false;
	return true;
}

static int
cam_native_prepare(void)
{
	bool back = cam_profile->sensor == 1u;
	if (back ? !cam_native_back_mode24() : !cam_native_front_mode4()) {
		return ENXIO;
	}
	if (!cam_native_buffer()) {
		return EOVERFLOW;
	}
	/* Shared clock rates are changed only after every path and writer is idle. */
	if (!cam_native_idle()) {
		return EBUSY;
	}
	if (!cam_native_clocks_up() || !cam_native_phy_up()) {
		return EIO;
	}
	if (cam_native_initialized) {
		return 0;
	}
	cam_native_sensor_powered = true;
	/* Open's identity probe is powered down before applying the module's sequence. */
	cam_native_lens_down();
	if (!cam_native_sensor_power(false) || !cam_native_sensor_power(true)) {
		return EIO;
	}
	cam_native_exposure_invalidate();
	uint16_t id = 0;
	bool identified = back ?
	    (wide_bus_mode == 1u ? cci0_m1_read16(wide_slave_addr, WIDE_SENSOR_ID_REG, &id) :
	    (wide_bus_mode == 2u && od_i2c_read16_reg16(GPIO_CCI0_M1_SDA, GPIO_CCI0_M1_SCL,
	    wide_slave_addr, WIDE_SENSOR_ID_REG, &id))) :
	    od_i2c_read16_reg16(GPIO_SE9_I2C_SDA, GPIO_SE9_I2C_SCL,
	    front_slave_addr, FRONT_SENSOR_ID_REG, &id);
	if (!identified || id != (back ? WIDE_SENSOR_ID_EXPECTED : FRONT_SENSOR_ID_EXPECTED)) {
		return ENXIO;
	}
	if (!cam_native_register_table(0u, cam_profile->init_count) ||
	    !cam_native_register_table(1u, cam_profile->mode_count)) {
		return EIO;
	}
	cam_native_eeprom_load();
	cam_native_lens_up();
	cam_native_initialized = true;
	return 0;
}

static bool
cam_native_configure(void)
{
	if (!cam_native_idle()) {
		return false;
	}
	cam_native_path_configured = true;
	if (!cam_native_reset(true)) {
		return false;
	}
	csid_wr(CAM980_TOP_CLEAR, UINT32_MAX);
	csid_wr(CAM980_BUF_CLEAR, UINT32_MAX);
	csid_wr(CAM980_RX_CLEAR, UINT32_MAX);
	csid_wr(CAM980_PATH_CLEAR, UINT32_MAX);
	csid_wr(CAM980_IRQ_COMMAND, 1u);
	if (!cam_poll(csid_lite_base, CAM980_BUF_STATUS, CAM980_RDI0_DONE, 0, 5000u)) {
		return false;
	}
	csid_wr(CAM980_BUF_MASK, CAM980_RDI0_DONE);
	csid_wr(CAM980_RX_MASK, CAM980_RX_FATAL);
	csid_wr(CAM980_PATH_MASK, CAM980_PATH_FATAL | 0x18u);
	csid_wr(CAM980_TOP_MASK, 1u | 0x2000u | 0x100u | 0x10u);
	csid_wr(0x1218u, 0);
	csid_wr(0x1220u, UINT32_MAX);
	csid_wr(0x1230u, 1u);
	bool back = cam_profile->sensor == 1u;
	uint32_t receive = (cam_profile->lane_count - 1u) | (cam_profile->lane_assign << 4) |
	    ((back ? 4u : 5u) << 20) | (back ? 1u << 24 : 0);
	csid_wr(CAM980_RX_CFG0, receive);
	csid_wr(CAM980_RX_CFG1, (1u << 6) | 1u);
	/* Native RAW10 -> MIPI RAW10: decode 2, packing 1; timestamp strobe 2. */
	uint32_t path = (cam_profile->virtual_channel << 22) | (cam_profile->data_type << 16) |
	    (2u << 12) | (1u << 6) | (2u << 8);
	csid_wr(CAM980_PATH_CFG0, path);
	csid_wr(CAM980_PATH_CONTROL, 0);
	csid_wr(0x50cu, 0);
	csid_wr(CAM980_PATH_CFG1, 1u << 15);
	csid_wr(CAM980_PATH_OVERFLOW, 9u);
	csid_wr(0x548u, 1u);
	csid_wr(0x54cu, 0);
	csid_wr(0x558u, 0);
	csid_wr(0x55cu, 0);
	csid_wr(0x560u, 0);
	csid_wr(0x564u, 0);
	csid_wr(CAM980_PATH_CFG0, path | (1u << 31));
	csid_wr(CAM980_WM_CFG, 0);
	csid_wr(CAM980_WM_IMAGE_CFG0, (cam_profile->height << 16) | (cam_profile->stride / 16u));
	csid_wr(CAM980_WM_IMAGE_CFG1, 0);
	csid_wr(CAM980_WM_IMAGE_CFG2, cam_profile->stride);
	csid_wr(CAM980_WM_PACKER, 0);
	csid_wr(CAM980_WM_FRAME_INCR, cam_native_dma_bytes >> 8);
	csid_wr(CAM980_WM_HEADER, 0);
	csid_wr(CAM980_WM_LINE_DONE, 0);
	csid_wr(CAM980_WM_IRQ_PERIOD, 0);
	csid_wr(CAM980_WM_IRQ_PATTERN, 1u);
	csid_wr(CAM980_WM_PREFETCH, boot_cam_wm_prefetch != 0 ? 1u : 0u);
	csid_wr(CAM980_WM_PREFETCH_MASK, boot_cam_wm_prefetch != 0 ? UINT32_MAX : 0u);
	/*
	 * system_cache_cfg (CAM980_WM_CACHE) keeps its reset value: the vendor bus driver
	 * (cam_vfe_bus_ver3.c) never writes it. With 0 written, every 256-byte block whose address
	 * has bit 8 set ended in a CAMNOC decode error and stayed unwritten.
	 */
	csid_wr(CAM980_WM_ADDR_CONFIG, (uint32_t)(cam_capture.dma_address & 0xffu));
	cam_diag_bus_debug_enable();
	/* Bits that survive the clear are an earlier frame's and do not fail this one. */
	csid_wr(CAM980_BUS_OVERFLOW_CLEAR, UINT32_MAX);
	cam_native_overflow_stale = reg_rd(csid_lite_base, CAM980_BUS_OVERFLOW);
	return reg_rd(csid_lite_base, CAM980_PATH_CFG0) == (path | (1u << 31)) &&
	       reg_rd(csid_lite_base, CAM980_WM_IMAGE_CFG0) ==
	       ((cam_profile->height << 16) | (cam_profile->stride / 16u)) &&
	       reg_rd(csid_lite_base, CAM980_WM_IMAGE_CFG2) == cam_profile->stride &&
	       cam_native_pd_configure();
}

static int
cam_native_wait_frame(void)
{
	cam_native_sof_ms = cam_native_overflow_ms = UINT32_MAX;
	for (uint32_t elapsed = 0; elapsed < 1000000u; elapsed += 1000u) {
		cam_native_wait_spun = elapsed;
		if (elapsed == 10000u) {
			cam_diag_midframe_sample();
		}
		uint32_t buf = reg_rd(csid_lite_base, CAM980_BUF_STATUS);
		uint32_t top = reg_rd(csid_lite_base, CAM980_TOP_STATUS);
		uint32_t rx = reg_rd(csid_lite_base, CAM980_RX_STATUS);
		uint32_t path = reg_rd(csid_lite_base, CAM980_PATH_STATUS);
		cam_capture.buf_done |= buf;
		cam_capture.top_irq |= top;
		cam_capture.rx_irq |= rx;
		cam_capture.path_irq |= path;
		if (buf == UINT32_MAX || top == UINT32_MAX || rx == UINT32_MAX || path == UINT32_MAX) {
			return EIO;
		}
		if ((cam_capture.buf_done & CAM980_RDI0_DONE) != 0) {
			cam_capture.flags |= ANNIBALE_CAM_CAPTURE_BUF_DONE;
			cam_capture.consumed_addr = reg_rd(csid_lite_base, CAM980_WM_CONSUMED);
			if (cam_capture.consumed_addr != (uint32_t)(cam_native_slot_address[cam_native_writing] >> 8)) {
				return EIO;
			}
			cam_capture.flags |= ANNIBALE_CAM_CAPTURE_ADDR_MATCH;
			cam_capture.time = mach_absolute_time();
			cam_native_pd_check_done(buf, top);
			return (cam_capture.rx_irq & CAM980_RX_FATAL) != 0 ||
			       (cam_capture.path_irq & CAM980_PATH_FATAL) != 0 ? EIO : 0;
		}
		if (cam_native_sof_ms == UINT32_MAX && (path & 0x10u) != 0) {
			cam_native_sof_ms = elapsed / 1000u;
		}
		uint32_t overflow = reg_rd(csid_lite_base, CAM980_BUS_OVERFLOW);
		if (overflow != UINT32_MAX && (overflow & ~cam_native_overflow_stale) != 0) {
			/* The writer dropped the frame: comp_done will not come. */
			cam_native_overflow_ms = elapsed / 1000u;
			return ENOBUFS;
		}
		/* Wait for WM0's RDI0_DONE output fence so transient C-PHY errors retire DMA cleanly instead of entering HELD. */
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	return ETIMEDOUT;
}

/*
 * Ends the DMA: sensor off, CSID reset, receiver, path and writer disabled, FIFO empty, SMMU stopped.
 * With `abandon`, or after an error no comp_done can follow, the frame in flight need not have finished.
 */
static bool
cam_native_finish(bool abandon)
{
	cam_diag_capture_probe("pre-finish");
	bool sensor_off = cam_native_register_table(3u, cam_profile->off_count);
	if (sensor_off) {
		cam_sensor_streaming = false;
		delay_for_interval(40000, NSEC_PER_USEC);
		cam_native_phy_configured = false;
	}
	bool stopped = sensor_off && (abandon || cam_native_recoverable(cam_capture.error));
	if (!cam_native_reset(stopped)) {
		return false;
	}
	cam_capture.flags |= ANNIBALE_CAM_CAPTURE_HALTED;
	csid_wr(CAM980_RX_CFG0, 0);
	csid_wr(CAM980_RX_CFG1, 0);
	csid_wr(CAM980_PATH_CFG0, 0);
	csid_wr(CAM980_WM_CFG, 0);
	cam_capture.wm_cfg = reg_rd(csid_lite_base, CAM980_WM_CFG);
	if (reg_rd(csid_lite_base, CAM980_RX_CFG0) != 0 || reg_rd(csid_lite_base, CAM980_RX_CFG1) != 0 ||
	    reg_rd(csid_lite_base, CAM980_PATH_CFG0) != 0 || cam_capture.wm_cfg != 0 ||
	    !cam_poll(csid_lite_base, CAM980_WM_FIFO, UINT32_MAX, 0, 100000u) ||
	    !cam_native_pd_stop_and_drain()) {
		return false;
	}
	cam_capture.fifo_count = reg_rd(csid_lite_base, CAM980_WM_FIFO);
	if (cam_capture.fifo_count != 0) {
		return false;
	}
	cam_capture.flags |= ANNIBALE_CAM_CAPTURE_FIFO_EMPTY;
	/* Ordinary comp_done is the native output fence; no later address or virtual frame was queued. */
	uint32_t completed = ANNIBALE_CAM_CAPTURE_BUF_DONE | ANNIBALE_CAM_CAPTURE_ADDR_MATCH;
	if (((cam_capture.flags & completed) != completed && !stopped) || !cam_smmu_route(false)) {
		return false;
	}
	cam_capture.flags |= ANNIBALE_CAM_CAPTURE_DMA_STOPPED | ANNIBALE_CAM_CAPTURE_SMMU_SYNC;
	cam_native_submitted = false;
	cam_native_path_configured = false;
	csid_wr(CAM980_BUF_MASK, 0);
	csid_wr(CAM980_BUF_CLEAR, CAM980_RDI0_DONE | CAM980_RDI1_DONE);
	csid_wr(CAM980_RX_MASK, 0);
	csid_wr(CAM980_PATH_MASK, 0);
	csid_wr(CAM980_TOP_MASK, 0);
	csid_wr(CAM980_IRQ_COMMAND, 1u);
	/* DMA was preceded by a clean/invalidate; CPU did not access these lines while hardware owned them. */
	FlushPoC_DcacheRegion(cam_native_slot_base(0), cam_native_dma_bytes);
	FlushPoC_DcacheRegion(cam_native_slot_base(1), cam_native_dma_bytes);
	if (!sensor_off && cam_capture.error == 0) {
		cam_capture.error = EIO;
	}
	return true;
}

static void
cam_native_report_failure(int error, uint64_t sequence, uint64_t began)
{
#ifdef CAM_SMMU_FSR
	uint32_t fsr = cam_smmu_bank != 0xffu ?
	    reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FSR) : 0u;
#else
	uint32_t fsr = 0u;
#endif
	(void)fsr;
	say("capture error=%d seq=%llu buf=%#x consumed=%#x top=%#x rx=%#x path=%#x fsr=%#x "
	    "bus_ovf=%#x bus_oob=%#x wm_cfg=%#x wm_fifo=%#x wm_dbg=%#x halt=%#x "
	    "fll=%u exp=%u gain=%u\n",
	    error, (unsigned long long)sequence, cam_capture.buf_done,
	    cam_capture.consumed_addr, cam_capture.top_irq, cam_capture.rx_irq,
	    cam_capture.path_irq, fsr,
	    reg_rd(csid_lite_base, 0x1268u), reg_rd(csid_lite_base, 0x1270u),
	    reg_rd(csid_lite_base, CAM980_WM_CFG), reg_rd(csid_lite_base, CAM980_WM_FIFO),
	    reg_rd(csid_lite_base, CAM980_WM_CONSUMED), reg_rd(csid_lite_base, 0x568u),
	    cam_native_frame_lines, cam_native_exposure_lines, cam_native_gain);
	cam_diag_capture_failure(cam_native_sof_ms, cam_native_overflow_ms,
	    (uint32_t)((mach_absolute_time() - began) / 24000u));
}

/* After a frame's wait: ends the DMA, then on an error gives the hardware up for a fresh start. */
static void
cam_native_end(int error)
{
	cam_capture.error = (uint32_t)error;
	if (!cam_native_finish(false)) {
		cam_capture.state = ANNIBALE_CAM_CAPTURE_HELD;
		if (cam_capture.error == 0) {
			cam_capture.error = EIO;
		}
		cam_native_trace("capture finish held flags=%#x error=%u reset=%#x wm=%#x fifo=%#x\n",
		    cam_capture.flags, cam_capture.error, cam_capture.reset_status, cam_capture.wm_cfg,
		    cam_capture.fifo_count);
		return;
	}
	if (cam_capture.error != 0) {
		cam_capture.state = ANNIBALE_CAM_CAPTURE_FAILED;
		if (!cam_native_release()) {
			cam_capture.state = ANNIBALE_CAM_CAPTURE_HELD;
		}
		return;
	}
	cam_capture.state = ANNIBALE_CAM_CAPTURE_STOPPED;
}

/*
 * The frame in the slot the writer has is done: the other slot is queued for the next frame, taken at
 * this register update, and the finished one is the caller's until the call after next.
 */
static void
cam_native_stream_advance(void)
{
	uint32_t finished = cam_native_writing;

	cam_native_writing = finished ^ 1u;
	csid_wr(CAM980_BUF_CLEAR, CAM980_RDI0_DONE | (cam_native_pd_configured ? CAM980_RDI1_DONE : 0u));
	csid_wr(CAM980_IRQ_COMMAND, 1u);
	csid_wr(CAM980_WM_ADDRESS, (uint32_t)(cam_native_slot_address[cam_native_writing] >> 8));
	cam_native_pd_queue_next(cam_native_writing);
	csid_wr(CAM980_RUP_AUP, cam_native_pd_rup_aup());
	FlushPoC_DcacheRegion(cam_native_slot_base(finished), cam_native_dma_bytes);
	cam_native_pd_finish_slot(finished);
	cam_native_ready = finished;
	cam_capture.dma_address = cam_native_slot_address[finished];
	cam_capture.state = ANNIBALE_CAM_CAPTURE_STREAMING;
	cam_capture.sequence++;
	cam_hw_dma_verified = true;
}

/*
 * "tl: C PID TID seq N exp W/E foc S/F wait W spin S adv A del D tot T fll F at US" -- one frame of
 * the streaming capture ioctl, under the same touch_lat=1 switch as the rest of the tl: lines, all
 * the times in microseconds:
 *
 * exp, foc    the frame's exposure/gain/frame-length and lens writes before the wait; W and S how
 *             many registers and lens positions were written (both zero when nothing changed). The
 *             lens time is the GT9764V's stepping, the positions 10.8 ms apart.
 * wait, spin  the poll for the frame, and the time the loop itself counted. wait past spin is time
 *             the poll was not running: the thread was woken late from its millisecond or kept off
 *             the processor.
 * adv         the slot advance, the finished buffer's cache flush inside.
 * del         the frame's completion (cam_capture.time, when the poll saw it) to the end of the
 *             ioctl's work: what the delivery added after the frame existed. adv is part of it, so
 *             del far past adv is the thread's rather than the flush's.
 * tot         the whole ioctl. fll the frame length applied to the sensor: wait is legitimately up
 *             to the frame time fll x line_ns (5333 ns back, 10000 ns front) makes.
 *
 * A line is written for a frame that wrote registers or moved the lens -- the metering window is
 * the shutter's own -- for one whose tot or del passed the notice level, for CAM_TL_ARM frames
 * after one did, and for one frame in CAM_TL_BASELINE otherwise: a quiet stream costs the log one
 * line in about three seconds.
 */
#define CAM_TL_TOTAL_NOTICE_US  40000u   /* one frame time at 30 fps is 33 ms */
#define CAM_TL_DEL_NOTICE_US    5000u
#define CAM_TL_ARM              8u
#define CAM_TL_BASELINE         90u

static uint32_t cam_native_tl_armed;

static void
cam_native_tl_frame(uint64_t began, uint64_t exp_at, uint64_t focus_at, uint64_t wait_at, uint64_t end)
{
	static int8_t tl_on = -1;
	uint64_t exp_us, focus_us, wait_us, adv_us, del_us, tot_us, at;

	if (tl_on < 0) {
		uint32_t on = 0;
		tl_on = PE_parse_boot_argn("touch_lat", &on, sizeof(on)) && on != 0;
	}
	if (!tl_on) {
		return;
	}
	absolutetime_to_nanoseconds(exp_at - began, &exp_us);
	absolutetime_to_nanoseconds(focus_at - exp_at, &focus_us);
	absolutetime_to_nanoseconds(wait_at - focus_at, &wait_us);
	absolutetime_to_nanoseconds(end - wait_at, &adv_us);
	absolutetime_to_nanoseconds(end - cam_capture.time, &del_us);
	absolutetime_to_nanoseconds(end - began, &tot_us);
	absolutetime_to_nanoseconds(end, &at);
	exp_us /= 1000;
	focus_us /= 1000;
	wait_us /= 1000;
	adv_us /= 1000;
	del_us /= 1000;
	tot_us /= 1000;
	bool notice = cam_native_exp_writes != 0 || cam_native_focus_steps != 0 ||
	    tot_us >= CAM_TL_TOTAL_NOTICE_US || del_us >= CAM_TL_DEL_NOTICE_US;
	bool line = notice || cam_native_tl_armed != 0 ||
	    cam_capture.sequence % CAM_TL_BASELINE == 0;
	if (tot_us >= CAM_TL_TOTAL_NOTICE_US || del_us >= CAM_TL_DEL_NOTICE_US) {
		cam_native_tl_armed = CAM_TL_ARM;
	} else if (cam_native_tl_armed != 0) {
		cam_native_tl_armed--;
	}
	if (!line) {
		return;
	}
	annibale_tl("tl: C %d %llu seq %llu exp %u/%llu foc %u/%llu wait %llu spin %llu adv %llu "
	    "del %llu tot %llu fll %u at %llu\n",
	    proc_pid(current_proc()), (unsigned long long)thread_tid(current_thread()),
	    (unsigned long long)cam_capture.sequence, cam_native_exp_writes, exp_us,
	    cam_native_focus_steps, focus_us, wait_us, (uint64_t)cam_native_wait_spun, adv_us,
	    del_us, tot_us, cam_native_applied_frame_lines, at / 1000);
}

/* One more frame of a running stream. */
static void
cam_native_stream_next(void)
{
	uint64_t began = mach_absolute_time();
	int error = 0;

	cam_capture.flags = cam_capture.buf_done = cam_capture.top_irq = cam_capture.rx_irq = cam_capture.path_irq = 0;
	cam_native_exp_writes = cam_native_focus_steps = 0;
	if (!cam_native_apply_exposure_regs(false)) {
		error = EIO;
	}
	uint64_t exp_at = mach_absolute_time();
	cam_native_apply_focus(false);
	cam_native_exposure_shown(false);
	uint64_t focus_at = mach_absolute_time();
	if (error == 0) {
		error = cam_native_wait_frame();
	}
	uint64_t wait_at = mach_absolute_time();
	if (error == 0) {
		cam_native_stream_advance();
		cam_native_tl_frame(began, exp_at, focus_at, wait_at, mach_absolute_time());
		return;
	}
	cam_native_report_failure(error, cam_capture.sequence, began);
	cam_hw_dma_verified = false;                    /* (the frame handed out before is no longer offered) */
	cam_capture.state = ANNIBALE_CAM_CAPTURE_RUNNING;
	cam_native_end(error);
}

static void
cam_native_capture(void)
{
	if (cam_capture_resources_held()) {
		return;
	}
	if (!cam_is_open || cam_profile == NULL) {
		cam_capture.state = ANNIBALE_CAM_CAPTURE_NO_PROFILE;
		cam_capture.error = ENXIO;
		return;
	}
	if (cam_capture.state == ANNIBALE_CAM_CAPTURE_STREAMING) {
		cam_native_stream_next();
		return;
	}
	uint64_t sequence = cam_capture.sequence;
	bzero(&cam_capture, sizeof(cam_capture));
	cam_capture.state = ANNIBALE_CAM_CAPTURE_RUNNING;
	cam_capture.width = cam_profile->width;
	cam_capture.height = cam_profile->height;
	cam_capture.stride = cam_profile->stride;
	cam_capture.bytes = cam_profile->stride * cam_profile->height;
	cam_capture.sequence = sequence;
	cam_hw_dma_verified = false;
	cam_native_trace("capture prepare bytes=%u dma=%#llx\n", cam_capture.bytes, cam_capture.dma_address);
	int error = cam_native_prepare();
	cam_native_trace("capture prepared error=%d dma=%#llx\n", error, cam_capture.dma_address);
	if (error == 0) {
		for (unsigned slot = 0; slot < 2u; slot++) {
			bzero((void *)cam_native_slot_base(slot), cam_native_dma_bytes);
			FlushPoC_DcacheRegion(cam_native_slot_base(slot), cam_native_dma_bytes);
		}
		cam_native_pd_zero_slots();
	}
	if (error == 0 && !cam_native_configure()) {
		error = EIO;
	}
	cam_native_trace("capture CSID configured error=%d reset=%#x\n", error, cam_capture.reset_status);
	if (error == 0 && !cam_smmu_configure_bypass()) {
		error = EIO;
	}
	cam_native_trace("capture SMMU configured error=%d\n", error);
	if (error != 0) {
		cam_capture.error = (uint32_t)error;
		cam_capture.state = ANNIBALE_CAM_CAPTURE_FAILED;
		if (!cam_native_release()) {
			cam_capture.state = ANNIBALE_CAM_CAPTURE_HELD;
		}
		cam_native_trace("capture prepare release state=%u error=%u\n", cam_capture.state, cam_capture.error);
		return;
	}
	cam_native_submitted = true;
	cam_native_writing = 0;
	uint64_t armed = mach_absolute_time();
#ifdef KERNEL
	boolean_t arm_istate = ml_set_interrupts_enabled(FALSE);
#endif
	csid_wr(CAM980_WM_ADDRESS, (uint32_t)(cam_native_slot_address[0] >> 8));
	csid_wr(CAM980_WM_CFG, 1u);
	cam_native_pd_arm_first();
	csid_wr(CAM980_RUP_AUP, cam_native_pd_rup_aup());
	csid_wr(CAM980_PATH_CONTROL, 1u);
#ifdef KERNEL
	ml_set_interrupts_enabled(arm_istate);
#endif
	cam_sensor_streaming = true;
	if (!cam_native_register_table(2u, cam_profile->on_count)) {
		error = EIO;
	} else {
		cam_native_exposure_shown(true);
		error = cam_native_wait_frame();
	}
	if (error != 0) {
		cam_native_report_failure(error, sequence, armed);
	}
	cam_native_trace("capture frame error=%d buf=%#x consumed=%#x top=%#x rx=%#x path=%#x\n",
	    error, cam_capture.buf_done, cam_capture.consumed_addr, cam_capture.top_irq,
	    cam_capture.rx_irq, cam_capture.path_irq);
	if (error == 0 && boot_cam_keep_stream != 0) {
		cam_native_stream_advance();
		return;
	}
	cam_native_end(error);
	if (cam_capture.state == ANNIBALE_CAM_CAPTURE_STOPPED) {
		cam_native_ready = 0;
		cam_native_pd_ready_flags = cam_native_pd_frame_flags;
		cam_capture.sequence = sequence + 1u;
		cam_hw_dma_verified = true;
	}
}

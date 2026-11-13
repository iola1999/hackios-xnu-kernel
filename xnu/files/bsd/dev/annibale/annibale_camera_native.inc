/*
 * One native front-mode frame: one WM0 FIFO address, ordinary comp_done,
 * consumed-address match, CSID path reset, WM disable, then SMMU fault/sync.
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
#define CAM980_WM_CACHE          0x1768u
#define CAM980_WM_ADDR_CONFIG    0x1770u
#define CAM980_WM_CONSUMED       0x1790u
#define CAM980_WM_FIFO           0x1798u
#define CAM980_RDI0_DONE         2u
#define CAM980_RX_FATAL          0x019fa800u
#define CAM980_PATH_FATAL        0x20186001u

extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
static bool cam_native_initialized, cam_native_sensor_powered, cam_native_submitted;
static bool cam_native_path_configured, cam_native_reset_failed;
static uint32_t cam_native_dma_bytes;

static bool
cam_native_front_mode4(void)
{
	return cam_hw_mmio != 0 && front_chip_id == FRONT_SENSOR_ID_EXPECTED &&
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

/* No address is published until the complete DMA range has been checked. */
static bool
cam_native_buffer(void)
{
	cam_native_dma_bytes = (cam_capture.bytes + 255u) & ~255u;
	if (cam_region_vbase == 0 || cam_native_dma_bytes == 0 ||
	    cam_native_dma_bytes > CAMERA_REGION_SIZE) {
		return false;
	}
	uint64_t first = kvtophys(cam_region_vbase);
	if (first == 0 || (first & 255u) != 0 ||
	    first >= (1ULL << 36) || cam_native_dma_bytes > (1ULL << 36) - first) {
		return false;
	}
	for (uint32_t offset = 0; offset < cam_native_dma_bytes; offset += PAGE_SIZE) {
		if (kvtophys(cam_region_vbase + offset) != first + offset) {
			return false;
		}
	}
	cam_capture.dma_address = first;
	return true;
}

static bool
cam_native_release(void)
{
	if (cam_native_submitted || cam_native_reset_failed || cam_capture_resources_held()) {
		return false;
	}
	if (cam_native_path_configured) {
		if (!cam_native_reset(false)) {
			return false;
		}
		csid_wr(CAM980_RX_CFG0, 0);
		csid_wr(CAM980_RX_CFG1, 0);
		csid_wr(CAM980_PATH_CFG0, 0);
		csid_wr(CAM980_WM_CFG, 0);
		cam_native_path_configured = false;
	}
	if (!cam_smmu_release()) {
		return false;
	}
	if (cam_native_sensor_powered) {
		if (cam_sensor_streaming) {
			(void)cam_native_register_table(3u, cam_profile->off_count);
		}
		if (!cam_native_sensor_power(false)) {
			return false;
		}
		cam_sensor_streaming = false;
		cam_native_sensor_powered = false;
	}
	if (!cam_native_phy_down() || !cam_native_clocks_down()) {
		return false;
	}
	cam_native_initialized = false;
	return true;
}

static int
cam_native_prepare(void)
{
	if (!cam_native_front_mode4()) {
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
	if (!cam_native_sensor_power(false) || !cam_native_sensor_power(true)) {
		return EIO;
	}
	uint16_t id = 0;
	if (!od_i2c_read16_reg16(GPIO_SE9_I2C_SDA, GPIO_SE9_I2C_SCL,
	    front_slave_addr, FRONT_SENSOR_ID_REG, &id) || id != FRONT_SENSOR_ID_EXPECTED) {
		return ENXIO;
	}
	if (!cam_native_register_table(0u, cam_profile->init_count) ||
	    !cam_native_register_table(1u, cam_profile->mode_count)) {
		return EIO;
	}
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
	uint32_t receive = (cam_profile->lane_count - 1u) | (cam_profile->lane_assign << 4) |
	    (5u << 20);
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
	csid_wr(CAM980_WM_PREFETCH, 0);
	csid_wr(CAM980_WM_CACHE, 0);
	csid_wr(CAM980_WM_ADDR_CONFIG, 0);
	return reg_rd(csid_lite_base, CAM980_PATH_CFG0) == (path | (1u << 31)) &&
	       reg_rd(csid_lite_base, CAM980_WM_IMAGE_CFG0) ==
	       ((cam_profile->height << 16) | (cam_profile->stride / 16u)) &&
	       reg_rd(csid_lite_base, CAM980_WM_IMAGE_CFG2) == cam_profile->stride;
}

static int
cam_native_wait_frame(void)
{
	for (uint32_t elapsed = 0; elapsed < 500000u; elapsed += 1000u) {
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
			if (cam_capture.consumed_addr != (uint32_t)(cam_capture.dma_address >> 8)) {
				return EIO;
			}
			cam_capture.flags |= ANNIBALE_CAM_CAPTURE_ADDR_MATCH;
			cam_capture.time = mach_absolute_time();
			return (cam_capture.rx_irq & CAM980_RX_FATAL) != 0 ||
			       (cam_capture.path_irq & CAM980_PATH_FATAL) != 0 ? EIO : 0;
		}
		if ((cam_capture.rx_irq & CAM980_RX_FATAL) != 0 ||
		    (cam_capture.path_irq & CAM980_PATH_FATAL) != 0) {
			return EIO;
		}
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	return ETIMEDOUT;
}

static bool
cam_native_finish(void)
{
	bool sensor_off = cam_native_register_table(3u, cam_profile->off_count);
	if (sensor_off) {
		cam_sensor_streaming = false;
	}
	if (!cam_native_reset(false)) {
		return false;
	}
	cam_capture.flags |= ANNIBALE_CAM_CAPTURE_HALTED;
	csid_wr(CAM980_RX_CFG0, 0);
	csid_wr(CAM980_RX_CFG1, 0);
	csid_wr(CAM980_PATH_CFG0, 0);
	csid_wr(CAM980_WM_CFG, 0);
	cam_capture.wm_cfg = reg_rd(csid_lite_base, CAM980_WM_CFG);
	if (reg_rd(csid_lite_base, CAM980_RX_CFG0) != 0 ||
	    reg_rd(csid_lite_base, CAM980_RX_CFG1) != 0 ||
	    reg_rd(csid_lite_base, CAM980_PATH_CFG0) != 0 || cam_capture.wm_cfg != 0 ||
	    !cam_poll(csid_lite_base, CAM980_WM_FIFO, UINT32_MAX, 0, 100000u)) {
		return false;
	}
	cam_capture.fifo_count = reg_rd(csid_lite_base, CAM980_WM_FIFO);
	if (cam_capture.fifo_count != 0) {
		return false;
	}
	cam_capture.flags |= ANNIBALE_CAM_CAPTURE_FIFO_EMPTY;
	/* Ordinary comp_done is the native output fence; no later address or virtual frame was queued. */
	uint32_t completed = ANNIBALE_CAM_CAPTURE_BUF_DONE | ANNIBALE_CAM_CAPTURE_ADDR_MATCH;
	if ((cam_capture.flags & completed) != completed || !cam_smmu_route(false)) {
		return false;
	}
	cam_capture.flags |= ANNIBALE_CAM_CAPTURE_DMA_STOPPED | ANNIBALE_CAM_CAPTURE_SMMU_SYNC;
	cam_native_submitted = false;
	cam_native_path_configured = false;
	csid_wr(CAM980_BUF_MASK, 0);
	csid_wr(CAM980_BUF_CLEAR, CAM980_RDI0_DONE);
	csid_wr(CAM980_RX_MASK, 0);
	csid_wr(CAM980_PATH_MASK, 0);
	csid_wr(CAM980_TOP_MASK, 0);
	csid_wr(CAM980_IRQ_COMMAND, 1u);
	/* DMA was preceded by a clean/invalidate; CPU did not access these lines while hardware owned them. */
	FlushPoC_DcacheRegion(cam_region_vbase, cam_native_dma_bytes);
	if (!sensor_off && cam_capture.error == 0) {
		cam_capture.error = EIO;
	}
	return true;
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
	bzero((void *)cam_region_vbase, cam_native_dma_bytes);
	FlushPoC_DcacheRegion(cam_region_vbase, cam_native_dma_bytes);
	cam_native_submitted = true;
	csid_wr(CAM980_WM_ADDRESS, (uint32_t)(cam_capture.dma_address >> 8));
	csid_wr(CAM980_WM_CFG, 1u);
	csid_wr(CAM980_RUP_AUP, 0x100010u);
	csid_wr(CAM980_PATH_CONTROL, 1u);
	cam_sensor_streaming = true;
	if (!cam_native_register_table(2u, cam_profile->on_count)) {
		error = EIO;
	} else {
		error = cam_native_wait_frame();
	}
	cam_native_trace("capture frame error=%d buf=%#x consumed=%#x top=%#x rx=%#x path=%#x\n",
	    error, cam_capture.buf_done, cam_capture.consumed_addr, cam_capture.top_irq,
	    cam_capture.rx_irq, cam_capture.path_irq);
	cam_native_trace("capture SMMU fault bank=%u fsr=%#x far=%#x:%#x\n", cam_smmu_bank,
	    reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FSR),
	    reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FAR + 4u),
	    reg_rd(cam_smmu_base, cam_smmu_cb_offset + CAM_SMMU_FAR));
	cam_capture.error = (uint32_t)error;
	if (!cam_native_finish()) {
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
	cam_capture.sequence = sequence + 1u;
	cam_hw_dma_verified = true;
	cam_native_trace("capture stopped flags=%#x sequence=%llu\n", cam_capture.flags, cam_capture.sequence);
}

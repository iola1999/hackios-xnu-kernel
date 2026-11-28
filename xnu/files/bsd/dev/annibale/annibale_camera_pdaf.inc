/*
 * Rear wide OVX8000 Mode 24 PDAF support:
 *   - IFE-lite0 RDI1 (0x600) + WM1 (0x1800) capturing VC=1 DT=0x2b (2560x480 MIPI RAW10)
 *     into CAMERA_SLOT_SIZE + CAM_NATIVE_PD_OFFSET (6 MiB offset inside each 8 MiB slot).
 *   - GT24P128F EEPROM (7-bit 0x51, 16-bit address) read cache (0x0000..0x183f) served
 *     through paged CAMIOC_G_EEPROM ioctl.
 *
 * Included by annibale_camera_native.inc after CAM980_* register definitions.
 */

#define CAM980_RDI1_PATH_STATUS   0x0fcu
#define CAM980_RDI1_PATH_MASK     0x100u
#define CAM980_RDI1_PATH_CLEAR    0x104u
#define CAM980_RDI1_PATH_CFG0     0x600u
#define CAM980_RDI1_PATH_CONTROL  0x604u
#define CAM980_RDI1_PATH_CFG1     0x610u
#define CAM980_RDI1_PATH_OVERFLOW 0x614u
#define CAM980_WM1_CFG            0x1800u
#define CAM980_WM1_ADDRESS        0x1804u
#define CAM980_WM1_FRAME_INCR     0x1808u
#define CAM980_WM1_IMAGE_CFG0     0x180cu
#define CAM980_WM1_IMAGE_CFG1     0x1810u
#define CAM980_WM1_IMAGE_CFG2     0x1814u
#define CAM980_WM1_PACKER         0x1818u
#define CAM980_WM1_HEADER         0x1828u
#define CAM980_WM1_LINE_DONE      0x182cu
#define CAM980_WM1_IRQ_PERIOD     0x1830u
#define CAM980_WM1_IRQ_PATTERN    0x1834u
#define CAM980_WM1_PREFETCH       0x1860u
#define CAM980_WM1_PREFETCH_MASK  0x1864u
/* CAM980_WM1_CACHE (0x1868) must keep its reset value; writing 0 causes CAMNOC decode errors. */
#define CAM980_WM1_ADDR_CONFIG    0x1870u
#define CAM980_WM1_CONSUMED       0x1890u
#define CAM980_WM1_FIFO           0x1898u

#define CAM980_RDI1_DONE          (1u << 2)
#define CAM980_RUP_AUP_RDI0       0x100010u
#define CAM980_RUP_AUP_RDI1       0x200020u

#define CAM_EEPROM_SLAVE7         0x51u
#define CAM_EEPROM_CACHE_BYTES    0x1840u   /* 6208 B: covers 0x0000..0x183f (AF + GainMap + H/V DCC) */

static uint32_t boot_cam_pdaf = 1u;
static uint64_t cam_native_pd_slot_address[2];
static bool cam_native_pd_configured;
static bool cam_native_pd_diag_logged;
static uint32_t cam_native_pd_frame_flags;

static uint8_t cam_eeprom_cache[CAM_EEPROM_CACHE_BYTES];
static bool cam_eeprom_valid;
static bool cam_eeprom_logged;

static bool
cam_native_pd_active(void)
{
	return boot_cam_pdaf != 0 && cam_profile != NULL &&
	       cam_profile->sensor == 1u && cam_profile->mode == 24u;
}

static bool
cam_native_pd_buffer_check(void)
{
	if (!cam_native_pd_active()) {
		cam_native_pd_slot_address[0] = 0;
		cam_native_pd_slot_address[1] = 0;
		return true;
	}
	if (cam_native_dma_bytes > CAM_NATIVE_PD_OFFSET ||
	    CAM_NATIVE_PD_OFFSET + CAM_NATIVE_PD_DMA_BYTES > CAMERA_SLOT_SIZE) {
		return false;
	}
	for (unsigned slot = 0; slot < 2u; slot++) {
		vm_offset_t base = cam_region_vbase + slot * CAMERA_SLOT_SIZE + CAM_NATIVE_PD_OFFSET;
		uint64_t first = kvtophys(base);
		if (first == 0 || (first & 511u) != 0 ||
		    first >= (1ULL << 36) || CAM_NATIVE_PD_DMA_BYTES > (1ULL << 36) - first) {
			return false;
		}
		for (uint32_t offset = 0; offset < CAM_NATIVE_PD_DMA_BYTES; offset += PAGE_SIZE) {
			if (kvtophys(base + offset) != first + offset) {
				return false;
			}
		}
		cam_native_pd_slot_address[slot] = first;
	}
	return true;
}

static void
cam_native_pd_zero_slots(void)
{
	if (!cam_native_pd_active()) {
		return;
	}
	for (unsigned slot = 0; slot < 2u; slot++) {
		vm_offset_t base = cam_native_slot_base(slot) + CAM_NATIVE_PD_OFFSET;
		bzero((void *)base, CAM_NATIVE_PD_DMA_BYTES);
		FlushPoC_DcacheRegion(base, CAM_NATIVE_PD_DMA_BYTES);
	}
}

static bool
cam_native_pd_configure(void)
{
	cam_native_pd_configured = false;
	cam_native_pd_diag_logged = false;
	cam_native_pd_frame_flags = 0;
	cam_native_pd_ready_flags = 0;
	if (!cam_native_pd_active()) {
		return true;
	}
	csid_wr(CAM980_RDI1_PATH_CLEAR, UINT32_MAX);
	csid_wr(CAM980_IRQ_COMMAND, 1u);
	/* Keep RDI0_DONE + RDI1_DONE unmasked in BUF_MASK; RDI0 remains the primary completion fence. */
	csid_wr(CAM980_BUF_MASK, CAM980_RDI0_DONE | CAM980_RDI1_DONE);
	csid_wr(CAM980_RDI1_PATH_MASK, CAM980_PATH_FATAL | 0x18u);
	/* VC=1, DT=0x2b (RAW10): decode 2, packing 1, timestamp strobe 2. */
	uint32_t path = (1u << 22) | (0x2bu << 16) | (2u << 12) | (1u << 6) | (2u << 8);
	csid_wr(CAM980_RDI1_PATH_CFG0, path);
	csid_wr(CAM980_RDI1_PATH_CONTROL, 0);
	csid_wr(0x60cu, 0);
	csid_wr(CAM980_RDI1_PATH_CFG1, 1u << 15);
	csid_wr(CAM980_RDI1_PATH_OVERFLOW, 9u);
	csid_wr(0x648u, 1u);
	csid_wr(0x64cu, 0);
	csid_wr(0x658u, 0);
	csid_wr(0x65cu, 0);
	csid_wr(0x660u, 0);
	csid_wr(0x664u, 0);
	csid_wr(CAM980_RDI1_PATH_CFG0, path | (1u << 31));
	csid_wr(CAM980_WM1_CFG, 0);
	csid_wr(CAM980_WM1_IMAGE_CFG0, (CAM_NATIVE_PD_HEIGHT << 16) | (CAM_NATIVE_PD_STRIDE / 16u));
	csid_wr(CAM980_WM1_IMAGE_CFG1, 0);
	csid_wr(CAM980_WM1_IMAGE_CFG2, CAM_NATIVE_PD_STRIDE);
	csid_wr(CAM980_WM1_PACKER, 0);
	csid_wr(CAM980_WM1_FRAME_INCR, CAM_NATIVE_PD_DMA_BYTES >> 8);
	csid_wr(CAM980_WM1_HEADER, 0);
	csid_wr(CAM980_WM1_LINE_DONE, 0);
	csid_wr(CAM980_WM1_IRQ_PERIOD, 0);
	csid_wr(CAM980_WM1_IRQ_PATTERN, 1u);
	csid_wr(CAM980_WM1_PREFETCH, boot_cam_wm_prefetch != 0 ? 1u : 0u);
	csid_wr(CAM980_WM1_PREFETCH_MASK, boot_cam_wm_prefetch != 0 ? UINT32_MAX : 0u);
	csid_wr(CAM980_WM1_ADDR_CONFIG, (uint32_t)(cam_native_pd_slot_address[0] & 0xffu));
	bool ok = reg_rd(csid_lite_base, CAM980_RDI1_PATH_CFG0) == (path | (1u << 31)) &&
	          reg_rd(csid_lite_base, CAM980_WM1_IMAGE_CFG0) ==
	          ((CAM_NATIVE_PD_HEIGHT << 16) | (CAM_NATIVE_PD_STRIDE / 16u)) &&
	          reg_rd(csid_lite_base, CAM980_WM1_IMAGE_CFG2) == CAM_NATIVE_PD_STRIDE;
	cam_native_pd_configured = ok;
	return ok;
}

static void
cam_native_pd_arm_first(void)
{
	if (!cam_native_pd_configured) {
		return;
	}
	csid_wr(CAM980_WM1_ADDRESS, (uint32_t)(cam_native_pd_slot_address[0] >> 8));
	csid_wr(CAM980_WM1_CFG, 1u);
	csid_wr(CAM980_RDI1_PATH_CONTROL, 1u);
}

static uint32_t
cam_native_pd_rup_aup(void)
{
	return CAM980_RUP_AUP_RDI0 | (cam_native_pd_configured ? CAM980_RUP_AUP_RDI1 : 0u);
}

static void
cam_native_pd_check_done(uint32_t buf_status, uint32_t top_status)
{
	cam_native_pd_frame_flags = 0;
	if (!cam_native_pd_configured) {
		return;
	}
	uint32_t wm1_cons = reg_rd(csid_lite_base, CAM980_WM1_CONSUMED);
	uint32_t wm1_fifo = reg_rd(csid_lite_base, CAM980_WM1_FIFO);
	uint32_t rdi1_path = reg_rd(csid_lite_base, CAM980_RDI1_PATH_STATUS);
	uint32_t ovf = reg_rd(csid_lite_base, CAM980_BUS_OVERFLOW);
	uint32_t expected = (uint32_t)(cam_native_pd_slot_address[cam_native_writing] >> 8);
	bool overflowed = ovf != UINT32_MAX && (ovf & ~cam_native_overflow_stale) != 0;
	if (!overflowed && wm1_cons == expected) {
		cam_native_pd_frame_flags |= ANNIBALE_CAM_PD_VALID;
	} else if (overflowed) {
		cam_native_pd_frame_flags |= ANNIBALE_CAM_PD_OVERFLOW;
	}
	if (!cam_native_pd_diag_logged) {
		cam_native_pd_diag_logged = true;
		say("pdaf: frame0 buf=%#x top=%#x rdi1_path=%#x wm1_cons=%#x exp=%#x wm1_fifo=%#x ovf=%#x valid=%u\n",
		    buf_status, top_status, rdi1_path, wm1_cons, expected, wm1_fifo, ovf,
		    (cam_native_pd_frame_flags & ANNIBALE_CAM_PD_VALID) != 0 ? 1u : 0u);
	}
}

static void
cam_native_pd_queue_next(uint32_t next_slot)
{
	if (!cam_native_pd_configured) {
		return;
	}
	csid_wr(CAM980_WM1_ADDRESS, (uint32_t)(cam_native_pd_slot_address[next_slot] >> 8));
}

static void
cam_native_pd_finish_slot(uint32_t finished_slot)
{
	cam_native_pd_ready_flags = cam_native_pd_frame_flags;
	if (cam_native_pd_configured) {
		FlushPoC_DcacheRegion(cam_native_slot_base(finished_slot) + CAM_NATIVE_PD_OFFSET,
		    CAM_NATIVE_PD_DMA_BYTES);
	}
}

static bool
cam_native_pd_stop_and_drain(void)
{
	if (!cam_native_pd_configured) {
		return true;
	}
	csid_wr(CAM980_RDI1_PATH_CFG0, 0);
	csid_wr(CAM980_WM1_CFG, 0);
	if (reg_rd(csid_lite_base, CAM980_RDI1_PATH_CFG0) != 0 ||
	    reg_rd(csid_lite_base, CAM980_WM1_CFG) != 0 ||
	    !cam_poll(csid_lite_base, CAM980_WM1_FIFO, UINT32_MAX, 0, 100000u)) {
		return false;
	}
	csid_wr(CAM980_RDI1_PATH_MASK, 0);
	csid_wr(CAM980_RDI1_PATH_CLEAR, UINT32_MAX);
	FlushPoC_DcacheRegion(cam_native_slot_base(0) + CAM_NATIVE_PD_OFFSET, CAM_NATIVE_PD_DMA_BYTES);
	FlushPoC_DcacheRegion(cam_native_slot_base(1) + CAM_NATIVE_PD_OFFSET, CAM_NATIVE_PD_DMA_BYTES);
	cam_native_pd_configured = false;
	return true;
}

static void
cam_native_pd_disable_regs(void)
{
	if (cam_native_pd_configured) {
		csid_wr(CAM980_RDI1_PATH_CFG0, 0);
		csid_wr(CAM980_WM1_CFG, 0);
		cam_native_pd_configured = false;
	}
	cam_native_pd_ready_flags = 0;
	cam_native_pd_frame_flags = 0;
}

static uint16_t
cam_eeprom_u16_be(uint32_t off)
{
	return (uint16_t)(((uint16_t)cam_eeprom_cache[off] << 8) | (uint16_t)cam_eeprom_cache[off + 1u]);
}

static void
cam_native_eeprom_load(void)
{
	if (cam_eeprom_valid || !cam_native_pd_active()) {
		return;
	}
	bool ok = true;
	uint32_t fail_off = 0;
	for (uint32_t off = 0; off < CAM_EEPROM_CACHE_BYTES; ) {
		uint32_t chunk = CAM_EEPROM_CACHE_BYTES - off;
		if (chunk > 15u) {
			chunk = 15u;
		}
		bool read_ok = wide_bus_mode == 1u ?
		    cci0_m1_read_bytes(CAM_EEPROM_SLAVE7, (uint16_t)off, cam_eeprom_cache + off, chunk) :
		    (wide_bus_mode == 2u &&
		    od_i2c_read_bytes(GPIO_CCI0_M1_SDA, GPIO_CCI0_M1_SCL, CAM_EEPROM_SLAVE7,
		    (uint16_t)off, cam_eeprom_cache + off, chunk));
		if (!read_ok) {
			ok = false;
			fail_off = off;
			break;
		}
		off += chunk;
	}
	if (!ok) {
		bool recovered = true;
		if (wide_bus_mode == 1u && !cci_ready) {
			recovered = cci0_m1_init();
		}
		if (!cam_eeprom_logged) {
			cam_eeprom_logged = true;
			say("eeprom: gt24p128f read failed at %#x (bus=%u cci_recovered=%u)\n",
			    fail_off, wide_bus_mode, recovered ? 1u : 0u);
		}
		return;
	}
	cam_eeprom_valid = true;
	if (!cam_eeprom_logged) {
		cam_eeprom_logged = true;
		say("eeprom: gt24p128f %u B vendor=%#02x af=%u/%u pd_hdr=%#x/%ux%u dcc_h0=%#x dcc_v0=%#x\n",
		    CAM_EEPROM_CACHE_BYTES, cam_eeprom_cache[0x0001u],
		    cam_eeprom_u16_be(0x002du), cam_eeprom_u16_be(0x0031u),
		    cam_eeprom_u16_be(0x0eedu), cam_eeprom_u16_be(0x0eefu), cam_eeprom_u16_be(0x0ef1u),
		    cam_eeprom_u16_be(0x126du), cam_eeprom_u16_be(0x1713u));
	}
}

static int
cam_native_eeprom_ioctl(struct annibale_camera_eeprom *req)
{
	if (req == NULL) {
		return EINVAL;
	}
	uint32_t req_off = req->offset;
	uint32_t req_len = req->length;
	bzero(req, sizeof(*req));
	req->offset = req_off;
	if (!cam_eeprom_valid) {
		req->total_bytes = 0;
		req->flags = 0;
		req->length = 0;
		return 0;
	}
	req->total_bytes = CAM_EEPROM_CACHE_BYTES;
	req->flags = ANNIBALE_CAM_EEPROM_FLAG_VALID;
	if (req_off >= CAM_EEPROM_CACHE_BYTES || req_len == 0) {
		req->length = 0;
		return 0;
	}
	if (req_len > ANNIBALE_CAM_EEPROM_CHUNK) {
		req_len = ANNIBALE_CAM_EEPROM_CHUNK;
	}
	uint32_t avail = CAM_EEPROM_CACHE_BYTES - req_off;
	if (req_len > avail) {
		req_len = avail;
	}
	memcpy(req->data, cam_eeprom_cache + req_off, req_len);
	req->length = req_len;
	return 0;
}

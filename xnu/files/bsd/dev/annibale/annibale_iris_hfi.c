/*
 * HFI Gen2 protocol & session engine for SM8750 Iris VPU v3.5.
 */
#include "annibale_iris_private.h"

static inline uint32_t
iris_align(uint32_t v, uint32_t a)
{
	return (v + a - 1u) & ~(a - 1u);
}

static inline uint32_t
mmio_r32(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile const uint32_t *)(base + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static inline void
mmio_w32(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static struct hfi_queue_header *
iris_qhdr(uint32_t idx)
{
	return (struct hfi_queue_header *)(g_iris.uc_region.kva + 0x120u + idx * sizeof(struct hfi_queue_header));
}

static uint32_t *
iris_qring(uint32_t off)
{
	return (uint32_t *)(g_iris.uc_region.kva + off);
}

static void
iris_drain_dbgq(void)
{
	struct hfi_queue_header *qh = iris_qhdr(2u);
	uint32_t *ring = iris_qring(IRIS_HFI_DBGQ_OFF);

	iris_cache_clean_inval(qh, sizeof(*qh));
	uint32_t rd = qh->read_idx;
	uint32_t wr = qh->write_idx;
	if (rd == wr || rd >= IRIS_HFI_Q_DWORDS || wr >= IRIS_HFI_Q_DWORDS) {
		return;
	}
	qh->read_idx = wr;
	iris_cache_clean_inval(qh, sizeof(*qh));
	(void)ring;

	iris_cache_clean_inval((const void *)g_iris.sfr_region.kva, 256u);
	const char *sfr = (const char *)(g_iris.sfr_region.kva + 4u);
	if (sfr[0] >= 0x20 && sfr[0] < 0x7f) {
		char msg[128];
		strlcpy(msg, sfr, sizeof(msg));
		iris_say("VPU SFR: %s\n", msg);
	}
}

/* ---- Packet Builder & Queue Ring I/O --------------------------------------------------------- */

struct iris_pkt_buf {
	uint8_t  bytes[1024];
	uint32_t used;
};

static void
pkt_begin(struct iris_pkt_buf *b, uint32_t session_id)
{
	memset(b, 0, sizeof(*b));
	struct hfi_header *hdr = (struct hfi_header *)b->bytes;
	hdr->size = sizeof(struct hfi_header);
	hdr->session_id = session_id;
	hdr->header_id = ++g_iris.next_header_id;
	hdr->num_packets = 0;
	b->used = sizeof(struct hfi_header);
}

static bool
pkt_add(struct iris_pkt_buf *b, uint32_t type, uint32_t flags, uint32_t info,
    uint32_t port, const void *payload, uint32_t payload_bytes)
{
	uint32_t aligned_payload = iris_align(payload_bytes, 4u);
	uint32_t pkt_sz = (uint32_t)sizeof(struct hfi_packet) + aligned_payload;
	if (b->used + pkt_sz > sizeof(b->bytes)) {
		return false;
	}
	struct hfi_packet *p = (struct hfi_packet *)(b->bytes + b->used);
	p->size = pkt_sz;
	p->type = type;
	p->flags = flags;
	p->payload_info = info;
	p->port = port;
	p->packet_id = ++g_iris.next_packet_id;
	if (payload != NULL && payload_bytes > 0) {
		memcpy(b->bytes + b->used + sizeof(struct hfi_packet), payload, payload_bytes);
	}
	b->used += pkt_sz;
	struct hfi_header *hdr = (struct hfi_header *)b->bytes;
	hdr->size = b->used;
	hdr->num_packets++;
	return true;
}

static bool
pkt_add_u32(struct iris_pkt_buf *b, uint32_t type, uint32_t flags, uint32_t info,
    uint32_t port, uint32_t val)
{
	return pkt_add(b, type, flags, info, port, &val, sizeof(val));
}

static bool
iris_cmdq_write(const struct iris_pkt_buf *b)
{
	struct hfi_queue_header *qh = iris_qhdr(0u);
	uint32_t *ring = iris_qring(IRIS_HFI_CMDQ_OFF);
	uint32_t dwords = b->used / 4u;
	const uint32_t *src = (const uint32_t *)b->bytes;

	iris_cache_clean_inval(qh, sizeof(*qh));
	uint32_t rd = qh->read_idx;
	uint32_t wr = qh->write_idx;
	if (rd >= IRIS_HFI_Q_DWORDS || wr >= IRIS_HFI_Q_DWORDS) {
		return false;
	}
	uint32_t free_dw = (rd > wr) ? (rd - wr - 1u) : (IRIS_HFI_Q_DWORDS - (wr - rd) - 1u);
	if (dwords == 0 || dwords > free_dw) {
		return false;
	}

	uint32_t first = IRIS_HFI_Q_DWORDS - wr;
	if (first > dwords) {
		first = dwords;
	}
	memcpy(&ring[wr], src, first * 4u);
	iris_cache_clean_inval(&ring[wr], first * 4u);
	if (dwords > first) {
		memcpy(&ring[0], src + first, (dwords - first) * 4u);
		iris_cache_clean_inval(&ring[0], (dwords - first) * 4u);
	}

	qh->write_idx = (wr + dwords) % IRIS_HFI_Q_DWORDS;
	iris_cache_clean_inval(qh, sizeof(*qh));
	mmio_w32(g_iris.mmio, VPU_HOST2XTENSA_INTR, 1u);
	return true;
}

static uint32_t
iris_msgq_read_one(uint8_t *dst, uint32_t dst_cap)
{
	struct hfi_queue_header *qh = iris_qhdr(1u);
	uint32_t *ring = iris_qring(IRIS_HFI_MSGQ_OFF);

	uint32_t intr = mmio_r32(g_iris.mmio, VPU_WRAPPER_INTR_STATUS);
	if (intr != 0) {
		mmio_w32(g_iris.mmio, VPU_WRAPPER_INTR_CLEAR, 1u);
	}

	iris_cache_clean_inval(qh, sizeof(*qh));
	uint32_t rd = qh->read_idx;
	uint32_t wr = qh->write_idx;
	if (rd == wr || rd >= IRIS_HFI_Q_DWORDS || wr >= IRIS_HFI_Q_DWORDS) {
		return 0;
	}

	iris_cache_clean_inval(&ring[rd], 64u);
	uint32_t byte_len = ring[rd];
	uint32_t dwords = iris_align(byte_len, 4u) / 4u;
	uint32_t avail = (wr >= rd) ? (wr - rd) : (IRIS_HFI_Q_DWORDS - rd + wr);
	if (byte_len < sizeof(struct hfi_header) || dwords > avail || dwords * 4u > dst_cap) {
		qh->read_idx = wr;
		iris_cache_clean_inval(qh, sizeof(*qh));
		return 0;
	}

	uint32_t first = IRIS_HFI_Q_DWORDS - rd;
	if (first > dwords) {
		first = dwords;
	}
	iris_cache_clean_inval(&ring[rd], first * 4u);
	memcpy(dst, &ring[rd], first * 4u);
	if (dwords > first) {
		iris_cache_clean_inval(&ring[0], (dwords - first) * 4u);
		memcpy(dst + first * 4u, &ring[0], (dwords - first) * 4u);
	}
	qh->read_idx = (rd + dwords) % IRIS_HFI_Q_DWORDS;
	iris_cache_clean_inval(qh, sizeof(*qh));
	return dwords * 4u;
}

/* ---- System Init & Buffer Sizing ------------------------------------------------------------- */

bool
iris_hfi_sys_init(void)
{
	struct iris_pkt_buf b;
	const uint32_t cmd_flags = HFI_PACKET_SELF_DEFINED | HFI_PACKET_RESPONSE_REQUIRED | HFI_PACKET_ORDERED;
	const uint32_t prop_flags = HFI_PACKET_SELF_DEFINED;

	pkt_begin(&b, 0);
	pkt_add_u32(&b, HFI_CMD_INIT, cmd_flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, 1u);
	pkt_add_u32(&b, HFI_PROP_UBWC_MAX_CHANNELS, prop_flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, 8u);
	pkt_add_u32(&b, HFI_PROP_UBWC_MAL_LENGTH, prop_flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, 32u);
	pkt_add_u32(&b, HFI_PROP_UBWC_HBB, prop_flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, 16u);
	pkt_add_u32(&b, HFI_PROP_UBWC_BANK_SWZL_LEVEL1, prop_flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, 0u);
	pkt_add_u32(&b, HFI_PROP_UBWC_BANK_SWZL_LEVEL2, prop_flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, 1u);
	pkt_add_u32(&b, HFI_PROP_UBWC_BANK_SWZL_LEVEL3, prop_flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, 1u);
	pkt_add_u32(&b, HFI_PROP_UBWC_BANK_SPREADING, prop_flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, 1u);

	if (!iris_cmdq_write(&b)) {
		return false;
	}

	uint8_t msg[2048];
	for (uint32_t i = 0; i < 1000u; i++) {
		uint32_t len = iris_msgq_read_one(msg, sizeof(msg));
		if (len >= sizeof(struct hfi_header)) {
			const struct hfi_header *hdr = (const struct hfi_header *)msg;
			uint32_t off = sizeof(struct hfi_header);
			for (uint32_t p = 0; p < hdr->num_packets && off + sizeof(struct hfi_packet) <= len; p++) {
				const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
				if (pkt->type == HFI_SYS_ERROR) {
					iris_drain_dbgq();
					return false;
				}
				off += pkt->size ? pkt->size : (uint32_t)sizeof(*pkt);
			}
			return true;
		}
		iris_sleep_us(250);
	}
	iris_drain_dbgq();
	/* Firmware may defer first message until HFI_CMD_OPEN on Gen2; check SMMU fault before accepting. */
	uint32_t cb_off = (g_iris.smmu_pages + g_iris.smmu_cb) * g_iris.smmu_page;
	return (mmio_r32(g_iris.smmu, cb_off + IRIS_CB_FSR) & 0x1feu) == 0;
}

static bool
iris_queue_buffer_ts(uint32_t session_id, uint32_t port, uint32_t buf_type, uint32_t idx,
    uint64_t iova, uint32_t buf_size, uint32_t data_size, uint64_t timestamp, uint32_t buf_flags)
{
	struct iris_pkt_buf pkt;
	struct hfi_buffer hb;

	memset(&hb, 0, sizeof(hb));
	hb.type = buf_type;
	hb.index = idx;
	hb.base_address = iova;
	hb.buffer_size = buf_size;
	hb.data_size = data_size;
	hb.timestamp = timestamp;
	hb.flags = buf_flags;

	pkt_begin(&pkt, session_id);
	pkt_add(&pkt, HFI_CMD_BUFFER, HFI_PACKET_SELF_DEFINED, HFI_PAYLOAD_STRUCTURE,
	    port, &hb, sizeof(hb));
	return iris_cmdq_write(&pkt);
}

static bool
iris_queue_buffer(uint32_t session_id, uint32_t port, uint32_t buf_type, uint32_t idx,
    uint64_t iova, uint32_t buf_size, uint32_t data_size, uint32_t buf_flags)
{
	return iris_queue_buffer_ts(session_id, port, buf_type, idx, iova, buf_size, data_size, 0, buf_flags);
}

static void
iris_free_session_buffers(struct iris_session *s)
{
	iris_dma_free_unmap(&s->bitstream);
	iris_dma_free_unmap(&s->intern_bin);
	iris_dma_free_unmap(&s->intern_comv);
	iris_dma_free_unmap(&s->intern_non_comv);
	iris_dma_free_unmap(&s->intern_line);
	iris_dma_free_unmap(&s->intern_persist);
	iris_dma_free_unmap(&s->intern_dpb);
	for (uint32_t i = 0; i < IRIS_RAW_POOL_COUNT; i++) {
		iris_dma_free_unmap(&s->raw[i]);
		s->raw_queued[i] = false;
	}
}

static bool
iris_alloc_session_buffers(struct iris_session *s)
{
	uint32_t slot = s->session_id - 1u;
	uint32_t mbs = ((s->width + 15u) / 16u) * ((s->height + 15u) / 16u);
	if (mbs < 14400u) {
		mbs = 14400u; /* Minimum 1080p headroom for internal scratch buffers */
	}

	s->y_stride = iris_align(s->width, 128u);
	s->y_scanlines = iris_align(s->height, 32u);
	s->uv_stride = s->y_stride;
	s->uv_scanlines = iris_align((s->height + 1u) / 2u, 16u);
	s->raw_frame_size = iris_align(s->y_stride * s->y_scanlines + s->uv_stride * s->uv_scanlines + 4096u, 4096u);

	uint32_t ibase = IRIS_IOVA_INTERNAL_BASE + slot * IRIS_IOVA_SESSION_STRIDE;
	uint32_t bin_sz = iris_align(mbs * 384u, 65536u);
	uint32_t comv_sz = iris_align(mbs * 128u * 8u, 65536u);
	uint32_t ncomv_sz = iris_align(mbs * 64u, 65536u);
	uint32_t line_sz = iris_align(s->y_stride * 256u, 65536u);
	uint32_t persist_sz = 0x100000u;
	uint32_t dpb_sz = iris_align(s->raw_frame_size * 6u, 65536u);

	uint32_t cur = ibase;
	if (!iris_dma_alloc_map(&s->intern_bin, cur, bin_sz, false)) goto fail;
	cur += s->intern_bin.size;
	if (!iris_dma_alloc_map(&s->intern_comv, cur, comv_sz, false)) goto fail;
	cur += s->intern_comv.size;
	if (!iris_dma_alloc_map(&s->intern_non_comv, cur, ncomv_sz, false)) goto fail;
	cur += s->intern_non_comv.size;
	if (!iris_dma_alloc_map(&s->intern_line, cur, line_sz, false)) goto fail;
	cur += s->intern_line.size;
	if (!iris_dma_alloc_map(&s->intern_persist, cur, persist_sz, false)) goto fail;
	cur += s->intern_persist.size;
	if (!iris_dma_alloc_map(&s->intern_dpb, cur, dpb_sz, false)) goto fail;

	uint32_t bs_iova = IRIS_IOVA_BITSTREAM_BASE + slot * IRIS_IOVA_BITSTREAM_STRIDE;
	if (!iris_dma_alloc_map(&s->bitstream, bs_iova, IRIS_MAX_BITSTREAM_BYTES, true)) {
		goto fail;
	}

	uint32_t raw_iova = IRIS_IOVA_RAW_BASE + slot * IRIS_IOVA_RAW_SESSION_STRIDE;
	for (uint32_t i = 0; i < IRIS_RAW_POOL_COUNT; i++) {
		if (!iris_dma_alloc_map(&s->raw[i], raw_iova, s->raw_frame_size, true)) {
			goto fail;
		}
		raw_iova += s->raw[i].size;
	}
	return true;

fail:
	iris_free_session_buffers(s);
	return false;
}

static bool
iris_wait_ack(uint32_t session_id, uint32_t timeout_ms)
{
	uint8_t msg[2048];
	uint32_t loops = timeout_ms * 4u;

	for (uint32_t i = 0; i < loops; i++) {
		uint32_t len = iris_msgq_read_one(msg, sizeof(msg));
		if (len >= sizeof(struct hfi_header)) {
			const struct hfi_header *hdr = (const struct hfi_header *)msg;
			uint32_t off = sizeof(struct hfi_header);
			for (uint32_t p = 0; p < hdr->num_packets && off + sizeof(struct hfi_packet) <= len; p++) {
				const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
				if (pkt->type == HFI_SYS_ERROR) {
					iris_drain_dbgq();
					return false;
				}
				off += pkt->size ? pkt->size : (uint32_t)sizeof(*pkt);
			}
			if (hdr->session_id == 0 || hdr->session_id == session_id) {
				return true;
			}
		}
		iris_sleep_us(250);
	}
	return false;
}

int
iris_hfi_session_open(struct iris_session *s)
{
	uint32_t hfi_codec = 0;
	if (s->codec_type == IRIS_CODEC_H264) {
		hfi_codec = 1u;
	} else if (s->codec_type == IRIS_CODEC_HEVC) {
		hfi_codec = 2u;
	} else {
		return EOPNOTSUPP;
	}

	if (!iris_alloc_session_buffers(s)) {
		return ENOMEM;
	}

	struct iris_pkt_buf b;
	const uint32_t cmd_resp = HFI_PACKET_SELF_DEFINED | HFI_PACKET_RESPONSE_REQUIRED;
	const uint32_t prop_f = HFI_PACKET_SELF_DEFINED;
	uint64_t stride_scan = ((uint64_t)((s->y_stride << 16) | s->y_scanlines)) |
	    (((uint64_t)((s->uv_stride << 16) | s->uv_scanlines)) << 32);

	pkt_begin(&b, s->session_id);
	pkt_add_u32(&b, HFI_CMD_OPEN, cmd_resp, HFI_PAYLOAD_U32, HFI_PORT_NONE, s->session_id);
	if (!iris_cmdq_write(&b) || !iris_wait_ack(s->session_id, 200u)) {
		iris_drain_dbgq();
		iris_free_session_buffers(s);
		return EIO;
	}

	pkt_begin(&b, s->session_id);
	pkt_add_u32(&b, HFI_PROP_CODEC, prop_f, HFI_PAYLOAD_U32_ENUM, HFI_PORT_NONE, hfi_codec);
	pkt_add_u32(&b, HFI_PROP_SECURE, prop_f, HFI_PAYLOAD_U32, HFI_PORT_NONE, 0u);
	pkt_add_u32(&b, HFI_PROP_BITSTREAM_RESOLUTION, prop_f, HFI_PAYLOAD_U32,
	    HFI_PORT_BITSTREAM, (s->width << 16) | s->height);
	pkt_add_u32(&b, HFI_PROP_COMV_BUFFER_COUNT, prop_f, HFI_PAYLOAD_U32, HFI_PORT_BITSTREAM, 8u);
	pkt_add_u32(&b, HFI_PROP_COLOR_FORMAT, prop_f, HFI_PAYLOAD_U32, HFI_PORT_RAW, 2u /* NV12 */);
	pkt_add(&b, HFI_PROP_LINEAR_STRIDE_SCANLINE, prop_f, HFI_PAYLOAD_U64,
	    HFI_PORT_RAW, &stride_scan, sizeof(stride_scan));
	pkt_add_u32(&b, HFI_CMD_SUBSCRIBE_MODE, prop_f, HFI_PAYLOAD_U32, HFI_PORT_BITSTREAM, 1u);
	pkt_add_u32(&b, HFI_CMD_SUBSCRIBE_MODE, prop_f, HFI_PAYLOAD_U32, HFI_PORT_RAW, 2u);
	if (!iris_cmdq_write(&b)) {
		goto fail_close;
	}

	(void)iris_queue_buffer(s->session_id, HFI_PORT_BITSTREAM, HFI_BUFFER_BIN, 0,
	    s->intern_bin.iova, s->intern_bin.size, 0, 0);
	(void)iris_queue_buffer(s->session_id, HFI_PORT_BITSTREAM, HFI_BUFFER_COMV, 0,
	    s->intern_comv.iova, s->intern_comv.size, 0, 0);
	(void)iris_queue_buffer(s->session_id, HFI_PORT_BITSTREAM, HFI_BUFFER_NON_COMV, 0,
	    s->intern_non_comv.iova, s->intern_non_comv.size, 0, 0);
	(void)iris_queue_buffer(s->session_id, HFI_PORT_BITSTREAM, HFI_BUFFER_LINE, 0,
	    s->intern_line.iova, s->intern_line.size, 0, 0);
	(void)iris_queue_buffer(s->session_id, HFI_PORT_BITSTREAM, HFI_BUFFER_PERSIST, 0,
	    s->intern_persist.iova, s->intern_persist.size, 0, 0);
	(void)iris_queue_buffer(s->session_id, HFI_PORT_RAW, HFI_BUFFER_DPB, 0,
	    s->intern_dpb.iova, s->intern_dpb.size, 0, 0);

	for (uint32_t i = 0; i < IRIS_RAW_POOL_COUNT; i++) {
		if (!iris_queue_buffer(s->session_id, HFI_PORT_RAW, HFI_BUFFER_RAW, i,
		    s->raw[i].iova, s->raw[i].size, 0, 0)) {
			goto fail_close;
		}
		s->raw_queued[i] = true;
	}

	pkt_begin(&b, s->session_id);
	pkt_add(&b, HFI_CMD_START, cmd_resp, HFI_PAYLOAD_NONE, HFI_PORT_BITSTREAM, NULL, 0);
	pkt_add(&b, HFI_CMD_START, cmd_resp, HFI_PAYLOAD_NONE, HFI_PORT_RAW, NULL, 0);
	if (!iris_cmdq_write(&b) || !iris_wait_ack(s->session_id, 200u)) {
		goto fail_close;
	}

	s->streaming = true;
	return 0;

fail_close:
	iris_hfi_session_close(s);
	return EIO;
}

static int
iris_copy_decoded_nv12(struct iris_session *s, uint32_t raw_idx, struct iris_decode_frame *req)
{
	const struct iris_dma_buf *rb = &s->raw[raw_idx];
	iris_cache_clean_inval((const void *)rb->kva, rb->size);

	uint32_t dst_stride = req->out_stride ? req->out_stride : s->y_stride;
	if (dst_stride < s->width) {
		return EINVAL;
	}

	const uint8_t *src_y = (const uint8_t *)rb->kva;
	const uint8_t *src_uv = src_y + (size_t)s->y_stride * s->y_scanlines;
	uint32_t copy_w = s->width;
	uint32_t uv_rows = (s->height + 1u) / 2u;

	if (req->out_user_y != 0) {
		for (uint32_t row = 0; row < s->height; row++) {
			int err = copyout(src_y + (size_t)row * s->y_stride,
			    (user_addr_t)(req->out_user_y + (uint64_t)row * dst_stride), copy_w);
			if (err != 0) {
				return err;
			}
		}
	}
	if (req->out_user_uv != 0) {
		for (uint32_t row = 0; row < uv_rows; row++) {
			int err = copyout(src_uv + (size_t)row * s->uv_stride,
			    (user_addr_t)(req->out_user_uv + (uint64_t)row * dst_stride), copy_w);
			if (err != 0) {
				return err;
			}
		}
	}

	req->out_stride = dst_stride;
	req->flags = IRIS_FRAME_DECODED;
	return 0;
}

int
iris_hfi_decode_frame(struct iris_session *s, struct iris_decode_frame *req)
{
	uint32_t in_flags = req->flags;
	bool drain_mode = (in_flags & IRIS_FRAME_DRAIN) != 0;
	bool poll_mode = (in_flags & IRIS_FRAME_POLL) != 0;
	int err = 0;

	req->flags = 0;
	req->out_tag = 0;
	if (!s->streaming) {
		return EINVAL;
	}

	if (drain_mode) {
		if (!s->draining) {
			struct iris_pkt_buf b;
			pkt_begin(&b, s->session_id);
			pkt_add(&b, HFI_CMD_DRAIN,
			    HFI_PACKET_SELF_DEFINED | HFI_PACKET_RESPONSE_REQUIRED | HFI_PACKET_ORDERED,
			    HFI_PAYLOAD_NONE, HFI_PORT_BITSTREAM, NULL, 0);
			if (!iris_cmdq_write(&b)) {
				return EIO;
			}
			s->draining = true;
		}
	} else if (!poll_mode) {
		if (req->input_len == 0 || req->input_len > IRIS_MAX_BITSTREAM_BYTES - 512u ||
		    req->input_user_addr == 0) {
			return EINVAL;
		}
		s->draining = false;
		err = copyin((user_addr_t)req->input_user_addr, (void *)s->bitstream.kva, req->input_len);
		if (err != 0) {
			return err;
		}
		uint32_t aligned_bs = iris_align(req->input_len, 256u);
		if (aligned_bs > req->input_len) {
			memset((void *)(s->bitstream.kva + req->input_len), 0, aligned_bs - req->input_len);
		}
		iris_cache_clean_inval((const void *)s->bitstream.kva, aligned_bs);

		if (!iris_queue_buffer_ts(s->session_id, HFI_PORT_BITSTREAM, HFI_BUFFER_BITSTREAM,
		    s->next_input_idx++, s->bitstream.iova, aligned_bs, req->input_len, req->in_tag, 0)) {
			return EIO;
		}
	}

	uint8_t msg[2048];
	bool input_released = poll_mode;
	uint32_t max_iters = poll_mode ? 4u : 1200u;
	for (uint32_t iter = 0; iter < max_iters; iter++) {
		uint32_t len = iris_msgq_read_one(msg, sizeof(msg));
		if (len < sizeof(struct hfi_header)) {
			if (poll_mode) {
				return 0;
			}
			if (!drain_mode && input_released && iter > 20u) {
				req->flags = IRIS_FRAME_NEED_MORE;
				return 0;
			}
			iris_sleep_us(250);
			continue;
		}
		const struct hfi_header *hdr = (const struct hfi_header *)msg;
		uint32_t off = sizeof(struct hfi_header);
		for (uint32_t p = 0; p < hdr->num_packets && off + sizeof(struct hfi_packet) <= len; p++) {
			const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
			uint32_t plen = pkt->size > sizeof(*pkt) ? pkt->size - (uint32_t)sizeof(*pkt) : 0;
			const uint8_t *pay = msg + off + sizeof(*pkt);

			if (pkt->type == HFI_SYS_ERROR) {
				iris_drain_dbgq();
				return EIO;
			}
			if (pkt->type == HFI_CMD_SETTINGS_CHANGE) {
				struct iris_pkt_buf ack;
				pkt_begin(&ack, s->session_id);
				pkt_add(&ack, HFI_CMD_SETTINGS_CHANGE,
				    HFI_PACKET_SELF_DEFINED | HFI_PACKET_RESPONSE_REQUIRED,
				    HFI_PAYLOAD_NONE, HFI_PORT_RAW, NULL, 0);
				(void)iris_cmdq_write(&ack);
			} else if (pkt->type == HFI_CMD_BUFFER && plen >= sizeof(struct hfi_buffer)) {
				const struct hfi_buffer *hb = (const struct hfi_buffer *)pay;
				if (hb->type == HFI_BUFFER_BITSTREAM) {
					input_released = true;
				} else if (hb->type == HFI_BUFFER_RAW) {
					uint32_t ridx = hb->index < IRIS_RAW_POOL_COUNT ? hb->index : 0u;
					if (hb->data_size > 0) {
						err = iris_copy_decoded_nv12(s, ridx, req);
						req->out_tag = hb->timestamp;
						(void)iris_queue_buffer(s->session_id, HFI_PORT_RAW,
						    HFI_BUFFER_RAW, ridx, s->raw[ridx].iova,
						    s->raw[ridx].size, 0, 0);
						if ((pkt->flags & HFI_INFORMATION_END_OF_DRAIN) != 0) {
							s->draining = false;
						}
						return err;
					}
					(void)iris_queue_buffer(s->session_id, HFI_PORT_RAW,
					    HFI_BUFFER_RAW, ridx, s->raw[ridx].iova,
					    s->raw[ridx].size, 0, 0);
				}
			}
			if (drain_mode && ((pkt->flags & HFI_INFORMATION_END_OF_DRAIN) != 0 ||
			    pkt->type == HFI_CMD_DRAIN)) {
				s->draining = false;
				return 0;
			}
			off += pkt->size ? pkt->size : (uint32_t)sizeof(*pkt);
		}
	}
	if (drain_mode) {
		s->draining = false;
		return 0;
	}
	iris_drain_dbgq();
	return ETIMEDOUT;
}

int
iris_hfi_session_flush(struct iris_session *s)
{
	if (!s->streaming) {
		return 0;
	}
	struct iris_pkt_buf b;
	pkt_begin(&b, s->session_id);
	pkt_add(&b, HFI_CMD_DRAIN,
	    HFI_PACKET_SELF_DEFINED | HFI_PACKET_RESPONSE_REQUIRED | HFI_PACKET_ORDERED,
	    HFI_PAYLOAD_NONE, HFI_PORT_BITSTREAM, NULL, 0);
	if (!iris_cmdq_write(&b)) {
		return EIO;
	}

	uint8_t msg[2048];
	for (uint32_t i = 0; i < 800u; i++) {
		uint32_t len = iris_msgq_read_one(msg, sizeof(msg));
		if (len >= sizeof(struct hfi_header)) {
			const struct hfi_header *hdr = (const struct hfi_header *)msg;
			uint32_t off = sizeof(struct hfi_header);
			for (uint32_t p = 0; p < hdr->num_packets && off + sizeof(struct hfi_packet) <= len; p++) {
				const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
				const uint8_t *pay = msg + off + sizeof(*pkt);
				uint32_t plen = pkt->size > sizeof(*pkt) ? pkt->size - (uint32_t)sizeof(*pkt) : 0;
				if (pkt->type == HFI_CMD_BUFFER && plen >= sizeof(struct hfi_buffer)) {
					const struct hfi_buffer *hb = (const struct hfi_buffer *)pay;
					if (hb->type == HFI_BUFFER_RAW && hb->index < IRIS_RAW_POOL_COUNT) {
						(void)iris_queue_buffer(s->session_id, HFI_PORT_RAW,
						    HFI_BUFFER_RAW, hb->index, s->raw[hb->index].iova,
						    s->raw[hb->index].size, 0, 0);
					}
				}
				if ((pkt->flags & HFI_INFORMATION_END_OF_DRAIN) != 0 ||
				    pkt->type == HFI_CMD_DRAIN) {
					return 0;
				}
				off += pkt->size ? pkt->size : (uint32_t)sizeof(*pkt);
			}
		}
		iris_sleep_us(250);
	}
	return 0;
}

void
iris_hfi_session_close(struct iris_session *s)
{
	struct iris_pkt_buf b;
	const uint32_t cmd_resp = HFI_PACKET_SELF_DEFINED | HFI_PACKET_RESPONSE_REQUIRED | HFI_PACKET_ORDERED;

	if (s->streaming) {
		pkt_begin(&b, s->session_id);
		pkt_add(&b, HFI_CMD_STOP, cmd_resp, HFI_PAYLOAD_NONE, HFI_PORT_BITSTREAM, NULL, 0);
		pkt_add(&b, HFI_CMD_STOP, cmd_resp, HFI_PAYLOAD_NONE, HFI_PORT_RAW, NULL, 0);
		if (iris_cmdq_write(&b)) {
			(void)iris_wait_ack(s->session_id, 100u);
		}
		s->streaming = false;
	}

	pkt_begin(&b, s->session_id);
	pkt_add(&b, HFI_CMD_CLOSE, cmd_resp, HFI_PAYLOAD_NONE, HFI_PORT_NONE, NULL, 0);
	if (iris_cmdq_write(&b)) {
		(void)iris_wait_ack(s->session_id, 100u);
	}
	iris_free_session_buffers(s);
}

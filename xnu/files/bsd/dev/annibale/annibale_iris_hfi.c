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

/* annibale_tl has a 320-byte line buffer; split firmware text without truncating it. */
static void
iris_diag_text(const char *kind, const char *text, uint32_t capacity)
{
	uint32_t length = (uint32_t)strnlen(text, capacity);
	for (uint32_t off = 0; off < length; ) {
		uint32_t count = length - off;
		if (count > 240u) count = 240u;
		iris_say("VPU %s +%u: %.*s\n", kind, off, (int)count, text + off);
		off += count;
	}
	if (length == capacity) iris_say("VPU %s: no terminator in %u bytes\n", kind, capacity);
}

static void
iris_dump_sfr(void)
{
	if (g_iris.sfr_region.kva == 0 || g_iris.sfr_region.size < 4u) return;
	uint32_t capacity = g_iris.sfr_region.size;
	if (capacity > IRIS_IOVA_SFR_SIZE) capacity = IRIS_IOVA_SFR_SIZE;
	iris_cache_clean_inval((const void *)g_iris.sfr_region.kva, capacity);
	const uint32_t *sfr = (const uint32_t *)g_iris.sfr_region.kva;
	uint32_t size = sfr[0];
	if (size <= 4u || size > capacity) {
		iris_say("VPU SFR: invalid size=%u capacity=%u\n", size, capacity);
		return;
	}
	iris_diag_text("SFR", (const char *)(sfr + 1), size - 4u);
}

static void
iris_drain_dbgq(void)
{
	/* iris_lock serializes the scratch buffer; keep 4 KiB off nested HFI receive stacks. */
	static uint32_t packet[1024];
	iris_dump_sfr();
	if (g_iris.uc_region.kva == 0 ||
	    g_iris.uc_region.size < IRIS_HFI_DBGQ_OFF + IRIS_HFI_Q_BYTES) return;
	struct hfi_queue_header *qh = iris_qhdr(2u);
	uint32_t *ring = iris_qring(IRIS_HFI_DBGQ_OFF);
	iris_cache_clean_inval(qh, sizeof(*qh));
	uint32_t rd = qh->read_idx, wr = qh->write_idx;
	if (rd >= IRIS_HFI_Q_DWORDS || wr >= IRIS_HFI_Q_DWORDS) {
		iris_say("VPU debug: invalid indices rd=%u wr=%u\n", rd, wr);
		return;
	}
	/* Freeze the producer limit; at most 16 complete records (under 64 KiB) per call. */
	uint32_t avail = wr >= rd ? wr - rd : IRIS_HFI_Q_DWORDS - rd + wr;
	for (uint32_t records = 0; avail != 0 && records < 16u; records++) {
		iris_cache_clean_inval(&ring[rd], sizeof(uint32_t));
		uint32_t bytes = ring[rd];
		/* Native __flush_debug_queue accepts 16..4095 bytes; __read_queue uses words. */
		if (bytes < 16u || bytes >= sizeof(packet) || (bytes & 3u) != 0) {
			iris_say("VPU debug: invalid length=%u rd=%u wr=%u\n", bytes, rd, wr);
			return;
		}
		uint32_t words = bytes / 4u;
		if (words > avail) {
			iris_say("VPU debug: incomplete length=%u available=%u rd=%u wr=%u\n",
			    bytes, avail * 4u, rd, wr);
			return;
		}
		uint32_t first = IRIS_HFI_Q_DWORDS - rd;
		if (first > words) first = words;
		iris_cache_clean_inval(&ring[rd], first * 4u);
		memcpy(packet, &ring[rd], first * 4u);
		if (words > first) {
			iris_cache_clean_inval(ring, (words - first) * 4u);
			memcpy((uint8_t *)packet + first * 4u, ring, (words - first) * 4u);
		}
		/* Firmware may reuse consumed words as soon as read_idx is published. */
		uint32_t start = rd;
		rd = (rd + words) % IRIS_HFI_Q_DWORDS;
		avail -= words;
		qh->read_idx = rd;
		iris_cache_clean_inval(qh, sizeof(*qh));
		iris_say("VPU debug: rd=%u bytes=%u type=0x%x\n", start, bytes, packet[1]);
		/* Native text starts at byte 0x11, after the 16-byte header and marker. */
		if (bytes > 17u) iris_diag_text("debug", (const char *)packet + 17u, bytes - 17u);
	}
	if (avail != 0) iris_say("VPU debug: deferred words=%u rd=%u wr=%u\n", avail, rd, wr);
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
	const struct hfi_header *hdr = (const struct hfi_header *)b->bytes;
	for (uint32_t off = sizeof(*hdr); off < b->used; ) {
		const struct hfi_packet *p = (const struct hfi_packet *)(b->bytes + off);
		if (p->type != HFI_CMD_BUFFER) {
			iris_say("tx sid=%u hdr=%u type=0x%08x id=%u port=%u flags=0x%x info=%u cmdq=%u/%u\n",
			    hdr->session_id, hdr->header_id, p->type, p->packet_id, p->port,
			    p->flags, p->payload_info, rd, qh->write_idx);
		}
		off += p->size;
	}
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

static bool
iris_validate_message(const uint8_t *msg, uint32_t len)
{
	if (len < sizeof(struct hfi_header) + sizeof(struct hfi_packet) || (len & 3u) != 0) {
		return false;
	}
	const struct hfi_header *hdr = (const struct hfi_header *)msg;
	if (hdr->size != len || hdr->num_packets == 0) {
		return false;
	}
	uint32_t off = sizeof(*hdr);
	for (uint32_t i = 0; i < hdr->num_packets; i++) {
		if (off > len || len - off < sizeof(struct hfi_packet)) {
			return false;
		}
		const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
		if (pkt->size < sizeof(*pkt) || pkt->size > len - off || (pkt->size & 3u) != 0) {
			return false;
		}
		uint32_t need = 0;
		switch (pkt->payload_info) {
		case HFI_PAYLOAD_U32:
		case HFI_PAYLOAD_U32_ENUM:
		case HFI_PAYLOAD_32_PACKED:
		case HFI_PAYLOAD_U32_ARRAY: need = 4u; break;
		case HFI_PAYLOAD_U64:
		case HFI_PAYLOAD_64_PACKED: need = 8u; break;
		case HFI_PAYLOAD_STRUCTURE:
			if (pkt->type == HFI_CMD_BUFFER) need = sizeof(struct hfi_buffer);
			break;
		default: break;
		}
		if (need > pkt->size - sizeof(*pkt)) {
			return false;
		}
		off += pkt->size;
	}
	return off == len;
}

bool
iris_hfi_sys_init(void)
{
	struct iris_pkt_buf b;
	const uint32_t cmd_flags = HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_NON_DISCARDABLE;
	const uint32_t prop_flags = HFI_HOST_FLAGS_NONE;

	pkt_begin(&b, 0);
	pkt_add_u32(&b, HFI_CMD_INIT, cmd_flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, 1u);
	/* UBWC packets have separate IDs; the core reply must match INIT. */
	uint32_t init_packet_id = g_iris.next_packet_id;
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
	iris_say("HFI INIT wait id=%u\n", init_packet_id);

	uint8_t msg[2048];
	for (uint32_t i = 0; i < 1000u; i++) {
		uint32_t len = iris_msgq_read_one(msg, sizeof(msg));
		if (len >= sizeof(struct hfi_header)) {
			const struct hfi_header *hdr = (const struct hfi_header *)msg;
			if (!iris_validate_message(msg, len)) {
				iris_say("HFI INIT invalid header size=%u len=%u\n", hdr->size, len);
				return false;
			}
			bool init_done = false;
			uint32_t off = sizeof(struct hfi_header);
			for (uint32_t p = 0; p < hdr->num_packets; p++) {
				const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
				if ((pkt->flags & HFI_FW_FLAGS_SYSTEM_ERROR) != 0 ||
				    (pkt->type > HFI_SYSTEM_ERROR_BEGIN && pkt->type < HFI_SYSTEM_ERROR_END)) {
					iris_say("HFI INIT system error type=0x%08x flags=0x%08x\n", pkt->type, pkt->flags);
					iris_drain_dbgq();
					return false;
				}
				if ((pkt->flags & HFI_FW_FLAGS_SESSION_ERROR) != 0 ||
				    (pkt->type > HFI_SESSION_ERROR_BEGIN && pkt->type < HFI_SESSION_ERROR_END)) {
					iris_say("HFI INIT session error type=0x%08x flags=0x%08x\n", pkt->type, pkt->flags);
					iris_drain_dbgq();
					return false;
				}
				if (pkt->type == HFI_CMD_INIT) {
					iris_say("HFI INIT reply session=%u id=%u flags=0x%08x expected=%u\n",
					    hdr->session_id, pkt->packet_id, pkt->flags, init_packet_id);
				}
				if (hdr->session_id == 0u && pkt->type == HFI_CMD_INIT &&
				    pkt->packet_id == init_packet_id && (pkt->flags & HFI_FW_FLAGS_SUCCESS) != 0) {
					init_done = true;
				}
				off += pkt->size;
			}
			/* A later packet in the same reply can report a system error. */
			if (init_done) {
				iris_say("HFI INIT done id=%u\n", init_packet_id);
				return true;
			}
		}
		iris_sleep_us(250);
	}
	iris_drain_dbgq();
	uint32_t cb_off = (g_iris.smmu_pages + g_iris.smmu_cb) * g_iris.smmu_page;
	uint32_t fsr = mmio_r32(g_iris.smmu, cb_off + IRIS_CB_FSR);
	iris_say("HFI INIT timeout id=%u fsr=0x%08x\n", init_packet_id, fsr);
	return false;
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
	hb.flags = buf_flags | HFI_BUF_HOST_FLAGS_CB_NON_SECURE;

	pkt_begin(&pkt, session_id);
	return pkt_add(&pkt, HFI_CMD_BUFFER, HFI_HOST_FLAGS_INTR_REQUIRED, HFI_PAYLOAD_STRUCTURE,
	    port, &hb, sizeof(hb)) && iris_cmdq_write(&pkt);
}

static bool
iris_queue_buffer(uint32_t session_id, uint32_t port, uint32_t buf_type, uint32_t idx,
    uint64_t iova, uint32_t buf_size, uint32_t data_size, uint32_t buf_flags)
{
	return iris_queue_buffer_ts(session_id, port, buf_type, idx, iova, buf_size, data_size, 0, buf_flags);
}

static bool
iris_free_session_buffers(struct iris_session *s)
{
	struct iris_dma_buf *bufs[] = { &s->bitstream, &s->intern_bin, &s->intern_comv,
		&s->intern_non_comv, &s->intern_line, &s->intern_persist, &s->intern_dpb };
	bool freed = true;
	for (uint32_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		if (!iris_dma_free_unmap(bufs[i])) freed = false;
	}
	for (uint32_t i = 0; i < IRIS_RAW_POOL_MAX; i++) {
		if (iris_dma_free_unmap(&s->raw[i])) s->raw_state[i] = IRIS_RAW_HOST;
		else freed = false;
	}
	return freed;
}

static uint32_t
iris_qc08c_size(const struct iris_session *s)
{
	uint32_t w = s->y_stride, h = s->y_scanlines;
	uint32_t y = iris_align(w * h, 4096u);
	uint32_t uv = iris_align(w * iris_align(h / 2u, 32u), 4096u);
	uint32_t ym = iris_align(iris_align((w + 31u) / 32u, 64u) *
	    iris_align((h + 7u) / 8u, 16u), 4096u);
	uint32_t uvm = iris_align(iris_align((w / 2u + 15u) / 16u, 64u) *
	    iris_align((h / 2u + 7u) / 8u, 16u), 4096u);
	return iris_align(y + uv + ym + uvm, 4096u);
}

/* SM8750 vpu35 uses the native vpu33 decoder sizing with four VPP pipes. */
static void
iris_internal_sizes(const struct iris_session *s, uint32_t sizes[6])
{
	uint32_t w = s->width, h = s->height;
	uint32_t mw = (w + 15u) / 16u, mh = (h + 15u) / 16u;
	uint32_t pixels = iris_align(w, 16u) * iris_align(h, 16u);
	uint32_t yuv = (pixels < 1280u * 736u ? 1280u * 736u : pixels) * 3u / 2u;
	uint32_t hdr_ratio = s->codec_type == IRIS_CODEC_H264 ? 1u : 2u;
	sizes[0] = (iris_align(yuv * hdr_ratio / 4u, 256u) +
	    iris_align(yuv * (4u - hdr_ratio) / 4u, 256u)) * 4u;
	uint32_t comv, non_comv, line, persist;
	uint32_t qp = iris_align(((w + 63u) / 64u) * ((h + 63u) / 64u) * 128u, 256u);
	uint32_t meta = iris_align(iris_align(h, 16u) * 32u, 256u);
	uint32_t top = iris_align(iris_align(w, 32u) / 32u * 256u, 256u) + 31u * 256u;
	if (top < 32u * iris_align(h, 8u)) top = 32u * iris_align(h, 8u);
	uint32_t vpss = 2u * top + 2u * iris_align(iris_align(h, 8u) / 2u * 64u, 32u);
	if (s->codec_type == IRIS_CODEC_H264) {
		uint32_t rows = (mh + 1u) / 2u;
		uint32_t zero = iris_align(2u * iris_align(iris_align(mw * 4u, 16u) * rows, 64u), 512u);
		uint32_t mv = iris_align(2u * iris_align(iris_align(mw * 128u, 16u) * rows, 64u), 512u);
		comv = (zero + mv + 512u) * 8u + 512u;
		uint32_t slices = iris_align(h, 32u) / 16u * 48u;
		if (slices > 1800u) slices = 1800u;
		non_comv = iris_align(slices * 128u, 256u) + iris_align(slices * 512u, 256u) + 32u * 2048u;
		line = iris_align(640u * iris_align(w, 16u) * 3u, 256u) +
		    iris_align(64u * mw, 256u) + iris_align(64u * mh, 256u) * 4u +
		    iris_align(16u * mw, 256u) + iris_align(16u * mh, 256u) * 4u +
		    iris_align(384u * mw, 256u) + iris_align(128u * mw, 256u) + 2u * meta + qp;
		persist = (256u + 32u) * 512u + 3328u * 66u + 32u * 4096u;
	} else {
		comv = iris_align(mw * mh * 256u, 512u) * 8u + 512u;
		uint32_t lcw = iris_align(w, 64u) / 16u, lch = iris_align(h, 64u) / 16u;
		uint32_t slices = iris_align(lcw * lch * 32u, 256u);
		if (slices > 3601u) slices = 3601u;
		uint32_t vpp = 2u * iris_align(slices, 4u) * 256u;
		if (vpp > 0x100000u) vpp = 0x100000u;
		non_comv = iris_align(2u * slices * 64u, 256u) + iris_align(vpp, 256u) +
		    iris_align(32u * 20u * 22u * 4u, 256u) + iris_align(4u * lcw * lch, 256u) +
		    32u * 2048u + 4096u;
		line = iris_align(640u * (iris_align(w, 64u) + 8u) * 2u, 256u) +
		    iris_align(64u * lcw, 256u) + iris_align(64u * lch, 256u) * 4u +
		    iris_align(((h + 63u) / 8u) * 16u, 256u) * 4u + iris_align(128u * mw, 256u) +
		    iris_align(384u * mw, 256u) + iris_align(((w + 63u) / 64u) * 128u, 256u) +
		    iris_align(((h + 63u) / 64u) * 128u, 256u) * 4u + 4u * meta + qp;
		persist = 100u * 1024u + 48u * 3072u + (128u * 32u + 1u) * 4u + 32u * 4096u;
	}
	sizes[1] = iris_align(comv, 65536u);
	sizes[2] = iris_align(non_comv, 65536u);
	sizes[3] = iris_align(line + vpss, 65536u);
	sizes[4] = iris_align(persist, 65536u);
	sizes[0] = iris_align(sizes[0], 65536u);
	sizes[5] = iris_align(s->dpb_frame_size * IRIS_DPB_POOL_COUNT, 65536u);
}

static int
iris_alloc_session_buffers(struct iris_session *s)
{
	if ((s->width & 1u) != 0 || (s->height & 1u) != 0) return EOPNOTSUPP;
	s->y_stride = iris_align(s->width, 128u);
	s->y_scanlines = iris_align(s->height, 32u);
	s->uv_stride = s->y_stride;
	s->uv_scanlines = iris_align(s->height / 2u, 16u);
	s->raw_frame_size = iris_align(s->y_stride * s->y_scanlines +
	    s->uv_stride * s->uv_scanlines, (uint32_t)PAGE_SIZE);
	s->dpb_frame_size = iris_qc08c_size(s);
	uint32_t sizes[6];
	iris_internal_sizes(s, sizes);
	uint64_t total = 0;
	for (uint32_t i = 0; i < 6u; i++) total += sizes[i];
	if (total > IRIS_IOVA_SESSION_STRIDE ||
	    (uint64_t)s->raw_frame_size * IRIS_RAW_POOL_COUNT > IRIS_IOVA_RAW_SESSION_STRIDE) {
		iris_say("static buffer range unsupported slot=%u internal=%llu raw=%u\n",
		    s->session_id, (unsigned long long)total, s->raw_frame_size);
		return EOPNOTSUPP;
	}
	struct iris_dma_buf *bufs[] = { &s->intern_bin, &s->intern_comv, &s->intern_non_comv,
		&s->intern_line, &s->intern_persist, &s->intern_dpb };
	uint32_t slot = s->session_id - 1u;
	uint32_t cur = IRIS_IOVA_INTERNAL_BASE + slot * IRIS_IOVA_SESSION_STRIDE;
	for (uint32_t i = 0; i < 6u; i++) {
		if (!iris_dma_alloc_map(bufs[i], cur, sizes[i], false)) return ENOMEM;
		cur += bufs[i]->size;
	}
	uint32_t bs = IRIS_IOVA_BITSTREAM_BASE + slot * IRIS_IOVA_BITSTREAM_STRIDE;
	if (!iris_dma_alloc_map(&s->bitstream, bs, IRIS_MAX_BITSTREAM_BYTES, true)) return ENOMEM;
	cur = IRIS_IOVA_RAW_BASE + slot * IRIS_IOVA_RAW_SESSION_STRIDE;
	for (uint32_t i = 0; i < IRIS_RAW_POOL_COUNT; i++) {
		if (!iris_dma_alloc_map(&s->raw[i], cur, s->raw_frame_size, true)) return ENOMEM;
		cur += s->raw[i].size;
		s->raw_count = i + 1u;
	}
	return 0;
}

enum {
	PSC_RES, PSC_CROP_LT, PSC_CROP_RB, PSC_DEPTH, PSC_CODED, PSC_MIN_COUNT,
	PSC_POC, PSC_PROFILE, PSC_LEVEL, PSC_TIER, PSC_COLOR
};
static int iris_psc_index(uint32_t type);

static const uint32_t iris_avc_change[] = {
	HFI_PROP_BITSTREAM_RESOLUTION, HFI_PROP_CROP_OFFSETS, HFI_PROP_LUMA_CHROMA_BIT_DEPTH,
	HFI_PROP_CODED_FRAMES, HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT, HFI_PROP_PIC_ORDER_CNT_TYPE,
	HFI_PROP_PROFILE, HFI_PROP_LEVEL, HFI_PROP_SIGNAL_COLOR_INFO
};
static const uint32_t iris_hevc_change[] = {
	HFI_PROP_BITSTREAM_RESOLUTION, HFI_PROP_CROP_OFFSETS, HFI_PROP_LUMA_CHROMA_BIT_DEPTH,
	HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT, HFI_PROP_PROFILE, HFI_PROP_LEVEL,
	HFI_PROP_TIER, HFI_PROP_SIGNAL_COLOR_INFO
};

static bool
iris_subscribe_port(struct iris_pkt_buf *b, const struct iris_session *s, uint32_t port)
{
	const uint32_t *props = s->codec_type == IRIS_CODEC_H264 ? iris_avc_change : iris_hevc_change;
	uint32_t count = s->codec_type == IRIS_CODEC_H264 ? 9u : 8u;
	uint32_t payload[10] = { HFI_MODE_PORT_SETTINGS_CHANGE };
	memcpy(payload + 1, props, count * sizeof(uint32_t));
	uint32_t flags = HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_RESPONSE_REQUIRED;
	if (!pkt_add(b, HFI_CMD_SUBSCRIBE_MODE, flags, HFI_PAYLOAD_U32_ARRAY,
	    port, payload, (count + 1u) * sizeof(uint32_t))) return false;
	/* Native AVC/HEVC input subscription publishes the initial output minimum. */
	if (port == HFI_PORT_BITSTREAM &&
	    !pkt_add_u32(b, HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT, HFI_HOST_FLAGS_NONE,
	    HFI_PAYLOAD_U32, port, 4u)) return false;
	if (port == HFI_PORT_RAW) {
		for (uint32_t i = 0; i < count; i++) {
			int index = iris_psc_index(props[i]);
			uint32_t info = index == PSC_CROP_LT ? HFI_PAYLOAD_64_PACKED : HFI_PAYLOAD_U32;
			uint32_t bytes = index == PSC_CROP_LT ? 8u : 4u;
			if (!pkt_add(b, props[i], HFI_HOST_FLAGS_NONE, info, port,
			    &s->psc_values[index], bytes)) return false;
		}
	}
	payload[0] = HFI_MODE_PROPERTY;
	payload[1] = port == HFI_PORT_BITSTREAM ? HFI_PROP_NO_OUTPUT : HFI_PROP_PICTURE_TYPE;
	count = 2u;
	if (port == HFI_PORT_RAW && s->codec_type == IRIS_CODEC_H264) {
		payload[count++] = HFI_PROP_CABAC_SESSION;
	}
	return pkt_add(b, HFI_CMD_SUBSCRIBE_MODE, flags, HFI_PAYLOAD_U32_ARRAY,
	    port, payload, count * sizeof(uint32_t));
}

static bool
iris_send_command(struct iris_session *s, uint32_t type, uint32_t port,
    uint32_t flags, uint32_t reason, uint32_t *id)
{
	struct iris_pkt_buf b;
	pkt_begin(&b, s->wire_session_id);
	bool added = type == HFI_CMD_RESUME ?
	    pkt_add_u32(&b, type, flags, HFI_PAYLOAD_U32, port, reason) :
	    pkt_add(&b, type, flags, HFI_PAYLOAD_NONE, port, NULL, 0);
	if (id != NULL) *id = g_iris.next_packet_id;
	return added && iris_cmdq_write(&b);
}

static bool
iris_start_port(struct iris_session *s, uint32_t port)
{
	struct iris_pkt_buf b;
	pkt_begin(&b, s->wire_session_id);
	if (!iris_subscribe_port(&b, s, port) || !iris_cmdq_write(&b) ||
	    !iris_send_command(s, HFI_CMD_START, port,
	    HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_RESPONSE_REQUIRED, 0, NULL)) return false;
	/* Gen2 START has no synchronous session completion in the native driver. */
	s->started_ports |= 1u << port;
	return true;
}

static int
iris_psc_index(uint32_t type)
{
	switch (type) {
	case HFI_PROP_BITSTREAM_RESOLUTION: return PSC_RES;
	case HFI_PROP_CROP_OFFSETS: return PSC_CROP_LT;
	case HFI_PROP_LUMA_CHROMA_BIT_DEPTH: return PSC_DEPTH;
	case HFI_PROP_CODED_FRAMES: return PSC_CODED;
	case HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT: return PSC_MIN_COUNT;
	case HFI_PROP_PIC_ORDER_CNT_TYPE: return PSC_POC;
	case HFI_PROP_PROFILE: return PSC_PROFILE;
	case HFI_PROP_LEVEL: return PSC_LEVEL;
	case HFI_PROP_TIER: return PSC_TIER;
	case HFI_PROP_SIGNAL_COLOR_INFO: return PSC_COLOR;
	default: return -1;
	}
}

static int
iris_read_property(struct iris_session *s, const struct hfi_packet *pkt)
{
	int index = iris_psc_index(pkt->type);
	if (index < 0 || pkt->port != HFI_PORT_BITSTREAM ||
	    (pkt->flags & HFI_FW_FLAGS_INFORMATION) != 0) return 0;
	uint32_t size = index == PSC_CROP_LT ? 8u : 4u;
	/* VPU replies use U64 crop and 32_PACKED depth, coded-frame and color fields. */
	bool info_ok = index == PSC_RES ? (pkt->payload_info == HFI_PAYLOAD_32_PACKED ||
	    pkt->payload_info == HFI_PAYLOAD_U32) :
	    index == PSC_CROP_LT ? (pkt->payload_info == HFI_PAYLOAD_U64 ||
	    pkt->payload_info == HFI_PAYLOAD_64_PACKED) :
	    (pkt->payload_info == HFI_PAYLOAD_U32 || pkt->payload_info == HFI_PAYLOAD_U32_ENUM ||
	    ((index == PSC_DEPTH || index == PSC_CODED || index == PSC_COLOR) &&
	    pkt->payload_info == HFI_PAYLOAD_32_PACKED));
	if (!info_ok || pkt->size - sizeof(*pkt) < size) return EIO;
	memcpy(&s->psc_values[index], (const uint8_t *)pkt + sizeof(*pkt), size);
	s->psc_seen |= 1u << index;
	return 0;
}

static int
iris_start_output(struct iris_session *s)
{
	uint32_t required = (1u << PSC_RES) | (1u << PSC_CROP_LT) | (1u << PSC_DEPTH) |
	    (1u << PSC_MIN_COUNT) | (1u << PSC_PROFILE) | (1u << PSC_LEVEL) | (1u << PSC_COLOR);
	required |= s->codec_type == IRIS_CODEC_H264 ?
	    (1u << PSC_CODED) | (1u << PSC_POC) : 1u << PSC_TIER;
	uint32_t count = s->psc_values[PSC_MIN_COUNT];
	iris_say("PSC sid=%u seen=0x%x required=0x%x resolution=0x%x depth=0x%x min=%u\n",
	    s->wire_session_id, s->psc_seen, required, s->psc_values[PSC_RES],
	    s->psc_values[PSC_DEPTH], count);
	if ((s->psc_seen & required) != required ||
	    s->psc_values[PSC_RES] != ((s->width << 16) | s->height) ||
	    s->psc_values[PSC_CROP_LT] != 0 || s->psc_values[PSC_CROP_RB] != 0 ||
	    s->psc_values[PSC_DEPTH] != 0x00080008u || count == 0 ||
	    count > IRIS_RAW_POOL_MAX || s->raw_frame_size == 0 ||
	    (uint64_t)s->raw_frame_size * count > IRIS_IOVA_RAW_SESSION_STRIDE ||
	    s->dpb_frame_size == 0 || count > s->intern_dpb.size / s->dpb_frame_size ||
	    (s->codec_type == IRIS_CODEC_H264 && (s->psc_values[PSC_CODED] & 1u) == 0)) {
		return EOPNOTSUPP;
	}
	/* Keep the initial RAW allocations and add the buffers required by input PSC. */
	while (s->raw_count < count) {
		uint32_t i = s->raw_count;
		uint32_t address = IRIS_IOVA_RAW_BASE + (s->session_id - 1u) *
		    IRIS_IOVA_RAW_SESSION_STRIDE + i * s->raw_frame_size;
		if (!iris_dma_alloc_map(&s->raw[i], address, s->raw_frame_size, true)) return ENOMEM;
		s->raw_count++;
	}
	struct iris_pkt_buf b;
	pkt_begin(&b, s->wire_session_id);
	uint32_t stride_scan[2] = { (s->y_stride << 16) | s->y_scanlines,
		(s->uv_stride << 16) | s->uv_scanlines };
	if (!pkt_add_u32(&b, HFI_PROP_OPB_ENABLE, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32, HFI_PORT_RAW, 1u) ||
	    !pkt_add_u32(&b, HFI_PROP_COLOR_FORMAT, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32_ENUM,
	    HFI_PORT_RAW, HFI_COLOR_FMT_NV12) ||
	    !pkt_add(&b, HFI_PROP_LINEAR_STRIDE_SCANLINE, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_64_PACKED,
	    HFI_PORT_RAW, stride_scan, sizeof(stride_scan)) || !iris_cmdq_write(&b)) return EIO;
	/* Initial PSC resumes input before RAW START; DPB is queued after RAW START. */
	if (!iris_send_command(s, HFI_CMD_RESUME, HFI_PORT_BITSTREAM,
	    HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_RESPONSE_REQUIRED, HFI_CMD_SETTINGS_CHANGE, NULL) ||
	    !iris_start_port(s, HFI_PORT_RAW)) return EIO;
	s->dpb_count = count;
	for (uint32_t i = 0; i < count; i++) {
		if (!iris_queue_buffer(s->wire_session_id, HFI_PORT_RAW, HFI_BUFFER_DPB, i,
		    s->intern_dpb.iova + i * s->dpb_frame_size, s->dpb_frame_size, 0, 0)) return EIO;
	}
	for (uint32_t i = 0; i < s->raw_count; i++) {
		if (!iris_queue_buffer(s->wire_session_id, HFI_PORT_RAW, HFI_BUFFER_RAW, i,
		    s->raw[i].iova, s->raw[i].size, 0, 0)) return EIO;
		s->raw_state[i] = IRIS_RAW_QUEUED;
	}
	iris_say("RAW published sid=%u ports=0x%x dpb=%u frame=%u raw=%u\n",
	    s->wire_session_id, s->started_ports, count, s->dpb_frame_size, s->raw_count);
	return 0;
}

struct iris_reply_wait {
	uint32_t session_id, type, packet_id, port;
	bool done;
};

static struct iris_session *
iris_wire_session(uint32_t session_id)
{
	for (uint32_t i = 0; i < IRIS_MAX_SESSIONS; i++) {
		struct iris_session *s = &g_iris.sessions[i];
		if (s->active && s->wire_session_id == session_id) return s;
	}
	return NULL;
}

static void
iris_protocol_fault(void)
{
	g_iris.hw_ready = false;
	for (uint32_t i = 0; i < IRIS_MAX_SESSIONS; i++) {
		if (g_iris.sessions[i].active) g_iris.sessions[i].error = EIO;
	}
	iris_drain_dbgq();
}

#include "annibale_iris_buffers.inc"

static int
iris_dispatch_message(const uint8_t *msg, uint32_t len, struct iris_session *target,
    struct iris_reply_wait *wait)
{
	if (!iris_validate_message(msg, len)) {
		iris_say("invalid response len=%u; DMA retained\n", len);
		iris_protocol_fault();
		return EIO;
	}
	const struct hfi_header *hdr = (const struct hfi_header *)msg;
	struct iris_session *s = iris_wire_session(hdr->session_id);
	/* Validate the entire reply and process errors before changing ownership or completing a wait. */
	for (uint32_t off = sizeof(*hdr); off < len; ) {
		const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
		if ((pkt->flags & HFI_FW_FLAGS_SYSTEM_ERROR) != 0 ||
		    (pkt->type > HFI_SYSTEM_ERROR_BEGIN && pkt->type < HFI_SYSTEM_ERROR_END)) {
			iris_say("system error sid=%u type=0x%x flags=0x%x\n", hdr->session_id, pkt->type, pkt->flags);
			iris_protocol_fault();
			return EIO;
		}
		if ((pkt->flags & HFI_FW_FLAGS_SESSION_ERROR) != 0 ||
		    (pkt->type > HFI_SESSION_ERROR_BEGIN && pkt->type < HFI_SESSION_ERROR_END)) {
			iris_say("session error sid=%u type=0x%x flags=0x%x\n", hdr->session_id, pkt->type, pkt->flags);
			iris_drain_dbgq();
			if (s != NULL) s->error = EIO;
		}
		off += pkt->size;
	}
	if (s == NULL) return 0;
	if (s->error != 0) return s == target ? s->error : 0;
	/* Native Gen2 clears DATA_CORRUPT before each message, then processes info before BUFFER. */
	bool psc = false, data_corrupt = false;
	for (uint32_t off = sizeof(*hdr); off < len; ) {
		const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
		psc |= pkt->type == HFI_CMD_SETTINGS_CHANGE && pkt->port == HFI_PORT_BITSTREAM;
		data_corrupt |= pkt->type == HFI_INFO_DATA_CORRUPT;
		off += pkt->size;
	}
	if (psc) {
		if (s->closing || (s->started_ports & (1u << HFI_PORT_RAW)) != 0) {
			s->error = EOPNOTSUPP;
			return s == target ? s->error : 0;
		}
		s->psc_seen = 0;
		memset(s->psc_values, 0, sizeof(s->psc_values));
	}
	for (uint32_t off = sizeof(*hdr); off < len; ) {
		const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
		if (s->response_logs < 32u || pkt->type == HFI_CMD_STOP || pkt->type == HFI_CMD_CLOSE) {
			struct hfi_queue_header *qh = iris_qhdr(0u);
			iris_cache_clean_inval(qh, sizeof(*qh));
			iris_say("rx sid=%u hdr=%u type=0x%08x id=%u port=%u flags=0x%x info=%u cmdq=%u/%u\n",
			    hdr->session_id, hdr->header_id, pkt->type, pkt->packet_id, pkt->port,
			    pkt->flags, pkt->payload_info, qh->read_idx, qh->write_idx);
			s->response_logs++;
		}
		int error = iris_read_property(s, pkt);
#if ANNIBALE_IRIS_BUFFER_DIAGNOSTICS
		if (pkt->type == HFI_INFO_DATA_CORRUPT)
			iris_say("data corrupt sid=%u hdr=%u port=%u flags=0x%x\n",
			    hdr->session_id, hdr->header_id, pkt->port, pkt->flags);
#endif
		if (pkt->type == HFI_INFO_BUFFER_OVERFLOW) error = EIO;
		if (pkt->type == HFI_INFO_HFI_FLAG_DRAIN_LAST && s->draining) s->drain_last = true;
		if (pkt->type == HFI_INFO_HFI_FLAG_PSC_LAST) error = EOPNOTSUPP;
		if (error != 0) {
			if (s->error == 0) iris_trace_reply_error(s, hdr, pkt, error, "property");
			s->error = error;
		}
		off += pkt->size;
	}
#if ANNIBALE_IRIS_BUFFER_DIAGNOSTICS
	if (data_corrupt) {
		for (uint32_t off = sizeof(*hdr); off < len; ) {
			const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
			iris_trace_buffer_identity(s, hdr, pkt);
			off += pkt->size;
		}
	}
#endif
	/* A malformed later property must also precede BUFFER ownership changes. */
	if (s->error != 0) return s == target ? s->error : 0;
	for (uint32_t off = sizeof(*hdr); off < len; ) {
		const struct hfi_packet *pkt = (const struct hfi_packet *)(msg + off);
		if (pkt->type == HFI_CMD_BUFFER) s->error = iris_return_buffer(s, pkt, data_corrupt);
		if (wait != NULL && hdr->session_id == wait->session_id && pkt->type == wait->type &&
		    pkt->packet_id == wait->packet_id && pkt->port == wait->port) {
			if ((pkt->flags & HFI_FW_FLAGS_SUCCESS) != 0) wait->done = true;
			else s->error = EIO;
		}
		if (s->draining && pkt->type == HFI_CMD_DRAIN && pkt->port == HFI_PORT_BITSTREAM &&
		    pkt->packet_id == s->drain_packet_id) {
			if ((pkt->flags & HFI_FW_FLAGS_SUCCESS) != 0) s->drain_ack = true;
			else s->error = EIO;
		}
		if (s->error != 0) {
			iris_trace_reply_error(s, hdr, pkt, s->error, "buffer-or-ack");
			break;
		}
		off += pkt->size;
	}
	if (s->error == 0 && psc) s->error = iris_start_output(s);
	return s == target ? s->error : 0;
}

static int
iris_receive(struct iris_session *s, struct iris_reply_wait *wait)
{
	uint8_t msg[2048];
	uint32_t len = iris_msgq_read_one(msg, sizeof(msg));
	if (len == 0) return 0;
	int error = iris_dispatch_message(msg, len, s, wait);
	return error != 0 ? -error : 1;
}

static int
iris_wait_reply(struct iris_session *s, uint32_t type, uint32_t id, uint32_t port, uint32_t timeout_ms)
{
	struct iris_reply_wait wait = { s->wire_session_id, type, id, port, false };
	for (uint32_t i = 0; i < timeout_ms * 4u; i++) {
		if (s->error != 0) return s->error;
		int received = iris_receive(s, &wait);
		if (received < 0) return -received;
		if (wait.done) return 0;
		iris_sleep_us(250u);
	}
	struct hfi_queue_header *qh = iris_qhdr(0u);
	iris_cache_clean_inval(qh, sizeof(*qh));
	iris_say("timeout sid=%u type=0x%x id=%u port=%u cmdq=%u/%u; DMA retained\n",
	    s->wire_session_id, type, id, port, qh->read_idx, qh->write_idx);
	return ETIMEDOUT;
}

int
iris_hfi_session_open(struct iris_session *s)
{
	uint32_t codec = s->codec_type == IRIS_CODEC_H264 ? HFI_CODEC_DECODE_AVC :
	    s->codec_type == IRIS_CODEC_HEVC ? HFI_CODEC_DECODE_HEVC : 0;
	if (codec == 0) return EOPNOTSUPP;
	if (g_iris.next_wire_session_id == UINT32_MAX) return EBUSY;
	s->wire_session_id = ++g_iris.next_wire_session_id;
	iris_say("open begin slot=%u wire=%u codec=%u width=%u height=%u\n",
	    s->session_id, s->wire_session_id, s->codec_type, s->width, s->height);
	int error = iris_alloc_session_buffers(s);
	if (error != 0) return error;
	struct iris_pkt_buf b;
	uint32_t flags = HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_RESPONSE_REQUIRED;
	/* OPEN addresses the core; the new wire session ID is its U32 payload. */
	pkt_begin(&b, 0u);
	if (!pkt_add_u32(&b, HFI_CMD_OPEN, flags, HFI_PAYLOAD_U32, HFI_PORT_NONE, s->wire_session_id) ||
	    !iris_cmdq_write(&b)) {
		return EIO;
	}
	s->opened = true;
	pkt_begin(&b, s->wire_session_id);
	/* Decoder resolution uses U32; the encoder uses the 32_PACKED payload type. */
	/* The synchronous single-input interface uses native vpu35's low-latency STAGE=1. */
	if (!pkt_add_u32(&b, HFI_PROP_CODEC, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32_ENUM, HFI_PORT_NONE, codec) ||
	    !pkt_add_u32(&b, HFI_PROP_DEC_DEFAULT_HEADER, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32,
	    HFI_PORT_BITSTREAM, 0u) ||
	    !pkt_add_u32(&b, HFI_PROP_BITSTREAM_RESOLUTION, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32,
	    HFI_PORT_BITSTREAM, (s->width << 16) | s->height) ||
	    !pkt_add_u32(&b, HFI_PROP_DECODE_ORDER_OUTPUT, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32,
	    HFI_PORT_BITSTREAM, 1u) ||
	    !pkt_add_u32(&b, HFI_PROP_STAGE, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32,
	    HFI_PORT_NONE, 1u) ||
	    !pkt_add_u32(&b, HFI_PROP_COMV_BUFFER_COUNT, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32,
	    HFI_PORT_BITSTREAM, 8u) || !iris_cmdq_write(&b)) goto failed;
	const struct iris_dma_buf *bufs[] = { &s->intern_persist, &s->intern_bin, &s->intern_comv,
		&s->intern_non_comv, &s->intern_line };
	const uint32_t types[] = { HFI_BUFFER_PERSIST, HFI_BUFFER_BIN, HFI_BUFFER_COMV,
		HFI_BUFFER_NON_COMV, HFI_BUFFER_LINE };
	for (uint32_t i = 0; i < 5u; i++) {
		uint32_t port = types[i] == HFI_BUFFER_PERSIST ? HFI_PORT_NONE : HFI_PORT_BITSTREAM;
		if (!iris_queue_buffer(s->wire_session_id, port, types[i], 0u,
		    bufs[i]->iova, bufs[i]->size, 0u, 0u)) goto failed;
		iris_say("internal published sid=%u type=%u port=%u bytes=%u\n",
		    s->wire_session_id, types[i], port, bufs[i]->size);
	}
	if (!iris_start_port(s, HFI_PORT_BITSTREAM)) goto failed;
	iris_say("open published slot=%u wire=%u ports=0x%x; waiting for input PSC\n",
	    s->session_id, s->wire_session_id, s->started_ports);
	return 0;
failed:
	iris_say("open failed slot=%u wire=%u; closing published session\n", s->session_id, s->wire_session_id);
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

static int
iris_take_output(struct iris_session *s, struct iris_decode_frame *req)
{
	for (uint32_t i = 0; i < s->raw_count; i++) {
		bool corrupt = s->raw_state[i] == IRIS_RAW_RETURNED_CORRUPT;
		if (s->raw_state[i] != IRIS_RAW_RETURNED && !corrupt) continue;
		struct hfi_buffer *hb = &s->raw_done[i];
		bool decoded = !corrupt && hb->data_size != 0;
		if (corrupt && req != NULL) {
			req->flags = IRIS_FRAME_CORRUPT;
			req->out_tag = hb->timestamp;
		}
		if (decoded && req != NULL) {
			int error = iris_copy_decoded_nv12(s, i, req);
			if (error != 0) return -error;
			req->out_tag = hb->timestamp;
		}
		s->raw_state[i] = IRIS_RAW_HOST;
		if (!s->closing && !s->drain_last) {
			if (!iris_queue_buffer(s->wire_session_id, HFI_PORT_RAW, HFI_BUFFER_RAW, i,
			    s->raw[i].iova, s->raw[i].size, 0, 0)) return -EIO;
			s->raw_state[i] = IRIS_RAW_QUEUED;
		}
		if ((decoded || corrupt) && req != NULL) return 1;
	}
	return 0;
}

static int
iris_begin_drain(struct iris_session *s)
{
	if (s->draining) return 0;
	if (!iris_send_command(s, HFI_CMD_DRAIN, HFI_PORT_BITSTREAM,
	    HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_RESPONSE_REQUIRED |
	    HFI_HOST_FLAGS_NON_DISCARDABLE, 0, &s->drain_packet_id)) return EIO;
	s->draining = true;
	s->drain_ack = false;
	s->drain_last = false;
	iris_say("drain published sid=%u id=%u\n", s->wire_session_id, s->drain_packet_id);
	return 0;
}

static int
iris_finish_drain(struct iris_session *s)
{
	if (!s->draining || !s->drain_ack || !s->drain_last || s->input_queued) return EBUSY;
	uint32_t flags = HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_RESPONSE_REQUIRED;
	/* Publish host-owned output buffers before firmware resumes decoding. */
	for (uint32_t i = 0; i < s->raw_count; i++) {
		if (s->raw_state[i] != IRIS_RAW_HOST || (s->started_ports & (1u << HFI_PORT_RAW)) == 0) continue;
		if (!iris_queue_buffer(s->wire_session_id, HFI_PORT_RAW, HFI_BUFFER_RAW, i,
		    s->raw[i].iova, s->raw[i].size, 0, 0)) return EIO;
		s->raw_state[i] = IRIS_RAW_QUEUED;
	}
	if (!iris_send_command(s, HFI_CMD_RESUME, HFI_PORT_BITSTREAM, flags, HFI_CMD_DRAIN, NULL)) return EIO;
	if ((s->started_ports & (1u << HFI_PORT_RAW)) != 0 &&
	    !iris_send_command(s, HFI_CMD_RESUME, HFI_PORT_RAW, flags, HFI_CMD_DRAIN, NULL)) return EIO;
	s->draining = false;
	s->drain_ack = false;
	s->drain_last = false;
	iris_say("drain done sid=%u id=%u\n", s->wire_session_id, s->drain_packet_id);
	return 0;
}

static int
iris_collect_output(struct iris_session *s, struct iris_decode_frame *req, bool draining, bool poll)
{
	uint32_t loops = poll ? 4u : 1200u;
	for (uint32_t i = 0; i < loops; i++) {
		int received = iris_receive(s, NULL);
		if (received < 0) return -received;
		/* Ordinary submits must observe the exact input return before reusing its DMA region. */
		int output = 0;
		if (draining || poll || !s->input_queued) output = iris_take_output(s, req);
		if (output < 0) return -output;
		if (output > 0 && req != NULL) return 0;
		if (draining && s->drain_ack && s->drain_last && !s->input_queued) {
			int error = iris_finish_drain(s);
			if (error != 0) s->error = error;
			return error;
		}
		if (!draining && !poll && !s->input_queued && i > 20u && received == 0) {
			req->flags = IRIS_FRAME_NEED_MORE;
			return 0;
		}
		if (poll && received == 0) return 0;
		iris_sleep_us(250u);
	}
	if (poll && !draining) return 0;
	iris_say("collect timeout sid=%u input=%u drain=%u ack=%u last=%u; DMA retained\n",
	    s->wire_session_id, s->input_queued, s->draining, s->drain_ack, s->drain_last);
	iris_drain_dbgq();
	return ETIMEDOUT;
}

int
iris_hfi_decode_frame(struct iris_session *s, struct iris_decode_frame *req)
{
	uint32_t flags = req->flags;
	bool drain = (flags & IRIS_FRAME_DRAIN) != 0, poll = (flags & IRIS_FRAME_POLL) != 0;
	req->flags = 0;
	req->out_tag = 0;
	if (s->error != 0) return s->error;
	if (s->closing || (s->started_ports & (1u << HFI_PORT_BITSTREAM)) == 0) return EINVAL;
	if (drain) {
		int error = iris_begin_drain(s);
		if (error != 0) return error;
	} else if (!poll) {
		/* The single input region remains immutable until its exact BUFFER return. */
		if (s->input_queued || s->draining) return EBUSY;
		if (req->input_len == 0 || req->input_len > IRIS_MAX_BITSTREAM_BYTES - 512u ||
		    req->input_user_addr == 0) return EINVAL;
		int error = copyin((user_addr_t)req->input_user_addr, (void *)s->bitstream.kva, req->input_len);
		if (error != 0) return error;
		uint32_t bytes = iris_align(req->input_len, 256u);
		memset((void *)(s->bitstream.kva + req->input_len), 0, bytes - req->input_len);
		iris_cache_clean_inval((const void *)s->bitstream.kva, bytes);
		uint32_t index = s->next_input_idx;
		if (!iris_queue_buffer_ts(s->wire_session_id, HFI_PORT_BITSTREAM, HFI_BUFFER_BITSTREAM,
		    index, s->bitstream.iova, s->bitstream.size, req->input_len, req->in_tag, 0)) return EIO;
		s->next_input_idx++;
		s->input_queued = true;
		if (index < 4u) iris_say("input published sid=%u index=%u bytes=%u tag=%llu\n",
		    s->wire_session_id, index, req->input_len, (unsigned long long)req->in_tag);
	}
	return iris_collect_output(s, req, drain, poll);
}

int
iris_hfi_session_flush(struct iris_session *s)
{
	if (s->error != 0) return s->error;
	if (s->closing) return EBUSY;
	if ((s->started_ports & (1u << HFI_PORT_BITSTREAM)) == 0) return 0;
	/* AVPlayer flushes before its first sample; without input PSC there is no DRAIN_LAST. */
	if (s->next_input_idx == 0) return 0;
	int error = iris_begin_drain(s);
	return error != 0 ? error : iris_collect_output(s, NULL, true, false);
}

int
iris_hfi_session_close(struct iris_session *s)
{
	s->closing = true;
	if (s->opened) {
		if (s->error != 0 || !g_iris.hw_ready) return s->error != 0 ? s->error : EIO;
		uint32_t flags = HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_RESPONSE_REQUIRED |
		    HFI_HOST_FLAGS_NON_DISCARDABLE;
		for (uint32_t port = HFI_PORT_BITSTREAM; port <= HFI_PORT_RAW; port++) {
			if ((s->started_ports & (1u << port)) == 0) continue;
			uint32_t id;
			if (!iris_send_command(s, HFI_CMD_STOP, port, flags, 0, &id)) return EIO;
			int error = iris_wait_reply(s, HFI_CMD_STOP, id, port, 100u);
			if (error != 0) return error;
			s->started_ports &= ~(1u << port);
		}
		uint32_t id;
		if (!iris_send_command(s, HFI_CMD_CLOSE, HFI_PORT_NONE, flags, 0, &id)) return EIO;
		int error = iris_wait_reply(s, HFI_CMD_CLOSE, id, HFI_PORT_NONE, 100u);
		if (error != 0) return error;
		s->opened = false;
		s->input_queued = false;
	}
	if (!iris_free_session_buffers(s)) {
		iris_say("close retained slot=%u wire=%u: TLB synchronization failed\n",
		    s->session_id, s->wire_session_id);
		return EIO;
	}
	iris_say("close done slot=%u wire=%u\n", s->session_id, s->wire_session_id);
	return 0;
}

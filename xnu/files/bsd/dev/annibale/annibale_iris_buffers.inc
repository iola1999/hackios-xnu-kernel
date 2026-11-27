/* HFI BUFFER identity, DMA returns, and failure diagnostics. Included by annibale_iris_hfi.c. */
static int
iris_return_buffer(struct iris_session *s, const struct hfi_packet *pkt, bool data_corrupt)
{
	/* Gen2 BUFFER replies without a payload do not return DMA ownership. */
	if (pkt->payload_info == HFI_PAYLOAD_NONE) return 0;
	if (pkt->payload_info != HFI_PAYLOAD_STRUCTURE ||
	    pkt->size - sizeof(*pkt) < sizeof(struct hfi_buffer)) return EIO;
	const struct hfi_buffer *hb = (const struct hfi_buffer *)((const uint8_t *)pkt + sizeof(*pkt));
	if (hb->addr_offset != 0 || hb->data_offset > hb->buffer_size ||
	    hb->data_size > hb->buffer_size - hb->data_offset) return EIO;
	if (hb->type == HFI_BUFFER_BITSTREAM) {
		if (pkt->port != HFI_PORT_BITSTREAM || !s->input_queued ||
		    hb->index != s->next_input_idx - 1u || hb->base_address != s->bitstream.iova ||
		    hb->data_offset != 0 || hb->buffer_size != s->bitstream.size) return EIO;
		s->input_queued = false;
		return 0;
	}
	if (hb->type == HFI_BUFFER_RAW) {
		if (pkt->port != HFI_PORT_RAW || hb->index >= s->raw_count ||
		    s->raw_state[hb->index] != IRIS_RAW_QUEUED || hb->data_offset != 0 ||
		    hb->base_address != s->raw[hb->index].iova || hb->buffer_size != s->raw[hb->index].size) return EIO;
		struct hfi_buffer returned = *hb;
		/* Native LAST/PSC_LAST handling precedes the internal-DPB READONLY check. */
		if ((returned.flags & (HFI_BUF_FW_FLAG_LAST | HFI_BUF_FW_FLAG_PSC_LAST)) != 0) {
			returned.data_size = 0;
			returned.flags &= ~HFI_BUF_FW_FLAG_READONLY;
		}
		if ((returned.flags & HFI_BUF_FW_FLAG_READONLY) != 0) return EIO;
		uint32_t row_bytes = s->width * (s->is_10bit ? 2u : 1u);
		uint32_t needed = s->y_stride * s->y_scanlines +
		    s->uv_stride * (s->height / 2u - 1u) + row_bytes;
		/* Damaged frames can be empty or short; their bytes are never copied to userland. */
		if (!data_corrupt && returned.data_size != 0 && returned.data_size < needed) return EIO;
		if ((returned.flags & HFI_BUF_FW_FLAG_PSC_LAST) != 0) return EOPNOTSUPP;
		s->raw_done[hb->index] = returned;
		s->raw_state[hb->index] = data_corrupt ? IRIS_RAW_RETURNED_CORRUPT : IRIS_RAW_RETURNED;
		if ((returned.flags & HFI_BUF_FW_FLAG_LAST) != 0 && s->draining) s->drain_last = true;
		return 0;
	}
	const struct iris_dma_buf *buf = NULL;
	uint32_t port = HFI_PORT_BITSTREAM, index = 0;
	switch (hb->type) {
	case HFI_BUFFER_BIN: buf = &s->intern_bin; break;
	case HFI_BUFFER_COMV: buf = &s->intern_comv; break;
	case HFI_BUFFER_NON_COMV: buf = &s->intern_non_comv; break;
	case HFI_BUFFER_LINE: buf = &s->intern_line; break;
	case HFI_BUFFER_PERSIST: buf = &s->intern_persist; port = HFI_PORT_NONE; break;
	case HFI_BUFFER_DPB:
		port = HFI_PORT_RAW;
		if (hb->index >= s->dpb_count || hb->base_address !=
		    (uint64_t)s->intern_dpb.iova + hb->index * s->dpb_frame_size ||
		    hb->buffer_size != s->dpb_frame_size) return EIO;
		return pkt->port == port ? 0 : EIO;
	default: return EIO;
	}
	return pkt->port == port && hb->index == index && hb->base_address == buf->iova &&
	    hb->buffer_size == buf->size ? 0 : EIO;
}

#if ANNIBALE_IRIS_BUFFER_DIAGNOSTICS
static void
iris_trace_buffer_identity(struct iris_session *s, const struct hfi_header *hdr,
    const struct hfi_packet *pkt)
{
	if (pkt->type != HFI_CMD_BUFFER || pkt->payload_info != HFI_PAYLOAD_STRUCTURE ||
	    pkt->size - sizeof(*pkt) < sizeof(struct hfi_buffer)) return;
	const struct hfi_buffer *hb = (const struct hfi_buffer *)((const uint8_t *)pkt + sizeof(*pkt));
	iris_say("buffer identity sid=%u hdr=%u port=%u type=%u index=%u base=0x%llx addr_offset=%u size=%u data_offset=%u data_size=%u tag=%llu flags=0x%x\n",
	    hdr->session_id, hdr->header_id, pkt->port, hb->type, hb->index,
	    (unsigned long long)hb->base_address, hb->addr_offset, hb->buffer_size,
	    hb->data_offset, hb->data_size, (unsigned long long)hb->timestamp, hb->flags);
	iris_say("expected input queued=%u index=%u base=0x%llx size=%u raw_count=%u drain=%u ack=%u last=%u\n",
	    s->input_queued, s->next_input_idx - 1u, (unsigned long long)s->bitstream.iova,
	    s->bitstream.size, s->raw_count, s->draining, s->drain_ack, s->drain_last);
	if (hb->type == HFI_BUFFER_RAW && hb->index < s->raw_count && hb->index < IRIS_RAW_POOL_MAX)
		iris_say("expected raw index=%u base=0x%llx size=%u state=%u\n", hb->index,
		    (unsigned long long)s->raw[hb->index].iova, s->raw[hb->index].size, s->raw_state[hb->index]);
}
#endif

static void
iris_trace_reply_error(struct iris_session *s, const struct hfi_header *hdr,
    const struct hfi_packet *pkt, int error, const char *stage)
{
	iris_say("reply rejected stage=%s sid=%u hdr=%u type=0x%x id=%u port=%u flags=0x%x info=%u size=%u error=%d\n",
	    stage, hdr->session_id, hdr->header_id, pkt->type, pkt->packet_id, pkt->port,
	    pkt->flags, pkt->payload_info, pkt->size, error);
	if (pkt->type == HFI_CMD_BUFFER && pkt->payload_info == HFI_PAYLOAD_STRUCTURE &&
	    pkt->size >= sizeof(*pkt) && pkt->size - sizeof(*pkt) >= sizeof(struct hfi_buffer)) {
		const struct hfi_buffer *hb = (const struct hfi_buffer *)((const uint8_t *)pkt + sizeof(*pkt));
		iris_say("rejected buf type=%u idx=%u base=0x%llx aoff=%u bsz=%u doff=%u dsz=%u bflags=0x%x\n",
		    hb->type, hb->index, (unsigned long long)hb->base_address, hb->addr_offset,
		    hb->buffer_size, hb->data_offset, hb->data_size, hb->flags);
	}
#if ANNIBALE_IRIS_BUFFER_DIAGNOSTICS
	iris_trace_buffer_identity(s, hdr, pkt);
#else
	(void)s;
#endif
}

static int
iris_reallocate_dpb(struct iris_session *s, uint32_t dpb_count)
{
	uint64_t dpb_bytes = (uint64_t)s->dpb_frame_size * dpb_count;
	uint32_t dpb_iova = s->intern_dpb.iova;
	uint32_t intern_base = IRIS_IOVA_INTERNAL_BASE + (s->session_id - 1u) * IRIS_IOVA_SESSION_STRIDE;
	if (s->dpb_frame_size == 0 || dpb_count == 0 || dpb_bytes > IRIS_IOVA_SESSION_STRIDE ||
	    dpb_iova < intern_base) {
		s->psc_rejected = true;
		return EOPNOTSUPP;
	}
	uint32_t dpb_size = iris_align((uint32_t)dpb_bytes, 65536u);
	if ((uint64_t)(dpb_iova - intern_base) + dpb_size > IRIS_IOVA_SESSION_STRIDE) {
		s->psc_rejected = true;
		return EOPNOTSUPP;
	}
	if (!iris_dma_free_unmap(&s->intern_dpb)) return EIO;
	return iris_dma_alloc_map(&s->intern_dpb, dpb_iova, dpb_size, false) ? 0 : ENOMEM;
}

static int
iris_reconfigure_p010(struct iris_session *s, uint32_t raw_target)
{
	uint32_t y_stride = iris_align(s->width * 2u, 256u);
	uint32_t raw_size = iris_align(y_stride * s->y_scanlines +
	    y_stride * s->uv_scanlines, (uint32_t)PAGE_SIZE);
	uint32_t old_stride = s->y_stride;
	s->y_stride = y_stride;
	uint32_t dpb_frame = iris_qc08c_size(s);
	s->y_stride = old_stride;
	uint32_t dpb_count = raw_target > IRIS_DPB_POOL_COUNT ? raw_target : IRIS_DPB_POOL_COUNT;
	uint64_t dpb_bytes = (uint64_t)dpb_frame * dpb_count;
	uint32_t dpb_iova = s->intern_dpb.iova;
	uint32_t intern_base = IRIS_IOVA_INTERNAL_BASE + (s->session_id - 1u) * IRIS_IOVA_SESSION_STRIDE;
	if (raw_size == 0 || dpb_frame == 0 || dpb_bytes > IRIS_IOVA_SESSION_STRIDE ||
	    (uint64_t)raw_size * raw_target > IRIS_IOVA_RAW_SESSION_STRIDE ||
	    dpb_iova < intern_base) {
		s->psc_rejected = true;
		return EOPNOTSUPP;
	}
	uint32_t dpb_size = iris_align((uint32_t)dpb_bytes, 65536u);
	if ((uint64_t)(dpb_iova - intern_base) + dpb_size > IRIS_IOVA_SESSION_STRIDE) {
		s->psc_rejected = true;
		return EOPNOTSUPP;
	}
	if (!iris_dma_free_unmap(&s->intern_dpb)) return EIO;
	for (uint32_t i = 0; i < s->raw_count; i++) {
		if (!iris_dma_free_unmap(&s->raw[i])) return EIO;
		s->raw_state[i] = IRIS_RAW_HOST;
	}
	s->raw_count = 0;
	s->y_stride = y_stride;
	s->uv_stride = y_stride;
	s->raw_frame_size = raw_size;
	s->dpb_frame_size = dpb_frame;
	s->is_10bit = true;
	return iris_dma_alloc_map(&s->intern_dpb, dpb_iova, dpb_size, false) ? 0 : ENOMEM;
}

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
		    hb->base_address != s->raw[hb->index].iova || hb->buffer_size != s->raw[hb->index].size ||
		    (hb->flags & HFI_BUF_FW_FLAG_READONLY) != 0) return EIO;
		uint32_t needed = s->y_stride * s->y_scanlines +
		    s->uv_stride * (s->height / 2u - 1u) + s->width;
		/* Damaged frames can be empty or short; their bytes are never copied to userland. */
		if (!data_corrupt && hb->data_size != 0 && hb->data_size < needed) return EIO;
		if ((hb->flags & HFI_BUF_FW_FLAG_PSC_LAST) != 0) return EOPNOTSUPP;
		s->raw_done[hb->index] = *hb;
		s->raw_state[hb->index] = data_corrupt ? IRIS_RAW_RETURNED_CORRUPT : IRIS_RAW_RETURNED;
		if ((hb->flags & HFI_BUF_FW_FLAG_LAST) != 0 && s->draining) s->drain_last = true;
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
#if ANNIBALE_IRIS_BUFFER_DIAGNOSTICS
	iris_trace_buffer_identity(s, hdr, pkt);
#else
	(void)s;
#endif
}

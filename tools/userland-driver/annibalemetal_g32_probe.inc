/*
 * G32 env-gated draw-state probe for VfxLlnXh / Tcma*Lln* / CAGradientLayer pipelines
 * (ANNIBALE_G32_PROBE=1 or =<max_lines>, off by default).
 *
 * When unset or "0", rw_g32_probe_limit() caches 0 on first call and recordDraw: takes zero
 * probe branches and emits zero log lines. Included by annibalemetal_renderencoder.inc (and
 * directly by host ASan+UBSan tests with ANNIBALE_G32_PROBE_HOST_TEST).
 */

struct rw_g32_vattr_meta {
	unsigned format;
	unsigned offset;
	unsigned buffer_index;
	unsigned stride;
};

struct rw_g32_draw_info {
	const char *vname;
	const char *fname;
	const char *label;
	unsigned primitive;
	unsigned vertex_start;
	unsigned vertex_count;
	int indexed;
	unsigned index_type; /* 0 = uint16, 1 = uint32 */
	const uint8_t *index_bytes;
	size_t index_len;
	size_t index_offset;
	int base_vertex;
	unsigned blend_enabled;
	unsigned write_mask;
	unsigned src_rgb, dst_rgb, op_rgb;
	unsigned src_a, dst_a, op_a;
	int has_scissor;
	unsigned scissor[4];
	int has_viewport;
	float viewport[4];
	const uint8_t *ubo1_bytes;
	size_t ubo1_len;
	size_t ubo1_offset;
	struct rw_g32_vattr_meta attr0;
	const uint8_t *attr0_bytes;
	size_t attr0_len;
	size_t attr0_buf_off;
	struct rw_g32_vattr_meta attr3;
	const uint8_t *attr3_bytes;
	size_t attr3_len;
	size_t attr3_buf_off;
};

static int rw_g32_limit_cache = -1;
static unsigned rw_g32_emitted = 0;

static unsigned
rw_g32_probe_limit(void)
{
	if (__builtin_expect(rw_g32_limit_cache < 0, 0)) {
		const char *val = getenv("ANNIBALE_G32_PROBE");
		if (val == NULL || val[0] == '\0' || strcmp(val, "0") == 0) {
			rw_g32_limit_cache = 0;
		} else {
			long n = strtol(val, NULL, 10);
			rw_g32_limit_cache = (n > 1 && n <= 1024) ? (int)n : 64;
		}
	}
	return (unsigned)rw_g32_limit_cache;
}

static int
rw_g32_name_matches(const char *vname, const char *fname, const char *label)
{
	if (vname != NULL && strstr(vname, "VfxLln") != NULL) {
		return 1;
	}
	if (fname != NULL &&
	    (strstr(fname, "Lln") != NULL || strstr(fname, "Tcma") != NULL ||
	     strstr(fname, "shading") != NULL)) {
		return 1;
	}
	if (label != NULL &&
	    (strstr(label, "Lln") != NULL || strstr(label, "Tcma") != NULL ||
	     strstr(label, "Gradient") != NULL)) {
		return 1;
	}
	return 0;
}

static float
rw_g32_f16_to_f32(uint16_t h)
{
	uint32_t sign = ((uint32_t)(h >> 15) & 1u) << 31;
	uint32_t exp = (h >> 10) & 0x1fu;
	uint32_t mant = h & 0x3ffu;
	uint32_t bits;

	if (exp == 0) {
		if (mant == 0) {
			bits = sign;
		} else {
			float f = (float)mant / 1024.0f;
			return sign ? -f * (1.0f / 16384.0f) : f * (1.0f / 16384.0f);
		}
	} else if (exp == 31u) {
		bits = sign | 0x7f800000u | (mant << 13);
	} else {
		bits = sign | ((exp + 112u) << 23) | (mant << 13);
	}
	float out;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

static void
rw_g32_fmt_attr(char *dst, size_t dst_sz, const struct rw_g32_vattr_meta *meta,
                const uint8_t *buf, size_t buf_len, size_t buf_off,
                const struct rw_g32_draw_info *d)
{
	if (meta->format == 0) {
		snprintf(dst, dst_sz, "none");
		return;
	}
	int n = snprintf(dst, dst_sz, "fmt=%u/b%u+%u/s%u[",
	                 meta->format, meta->buffer_index, meta->offset, meta->stride);
	if (n < 0 || (size_t)n >= dst_sz) {
		return;
	}
	size_t pos = (size_t)n;
	unsigned sample_n = d->vertex_count < 4u ? d->vertex_count : 4u;
	if (buf == NULL || meta->stride == 0 || sample_n == 0) {
		snprintf(dst + pos, dst_sz - pos, "%s]", buf == NULL ? "nobuf" : "empty");
		return;
	}
	for (unsigned i = 0; i < sample_n && pos + 24 < dst_sz; i++) {
		int64_t vidx = (int64_t)d->vertex_start + (int64_t)i;
		if (d->indexed) {
			if (d->index_bytes == NULL) {
				break;
			}
			size_t isz = d->index_type == 1u ? 4u : 2u;
			size_t ioff = d->index_offset + (size_t)i * isz;
			if (ioff + isz > d->index_len) {
				break;
			}
			uint32_t raw_i = 0;
			if (isz == 4u) {
				memcpy(&raw_i, d->index_bytes + ioff, 4);
			} else {
				uint16_t raw_16 = 0;
				memcpy(&raw_16, d->index_bytes + ioff, 2);
				raw_i = raw_16;
			}
			vidx = (int64_t)raw_i + (int64_t)d->base_vertex;
		}
		if (vidx < 0) {
			break;
		}
		size_t voff = buf_off + (size_t)vidx * (size_t)meta->stride + (size_t)meta->offset;
		if (i > 0 && pos + 2 < dst_sz) {
			dst[pos++] = ';';
		}
		if (voff + 8u <= buf_len && (meta->format == 27u || meta->format == 25u)) {
			/* MTLVertexFormatHalf4 (27) / Half2 (25) */
			uint16_t h[4] = { 0, 0, 0, 0 };
			memcpy(h, buf + voff, meta->format == 27u ? 8u : 4u);
			int k = snprintf(dst + pos, dst_sz - pos, "%04x,%04x,%04x,%04x",
			                 h[0], h[1], h[2], h[3]);
			if (k > 0) pos += (size_t)k;
		} else if (voff + 16u <= buf_len && (meta->format == 31u || meta->format == 30u)) {
			/* MTLVertexFormatFloat4 (31) / Float3 (30) */
			float f[4] = { 0, 0, 0, 0 };
			memcpy(f, buf + voff, meta->format == 31u ? 16u : 12u);
			int k = snprintf(dst + pos, dst_sz - pos, "%.1f,%.1f,%.1f,%.1f",
			                 f[0], f[1], f[2], f[3]);
			if (k > 0) pos += (size_t)k;
		} else if (voff + 8u <= buf_len && meta->format == 29u) {
			/* MTLVertexFormatFloat2 (29) */
			float f[2] = { 0, 0 };
			memcpy(f, buf + voff, 8u);
			int k = snprintf(dst + pos, dst_sz - pos, "%.1f,%.1f", f[0], f[1]);
			if (k > 0) pos += (size_t)k;
		} else if (voff + 4u <= buf_len) {
			uint32_t w = 0;
			memcpy(&w, buf + voff, 4u);
			int k = snprintf(dst + pos, dst_sz - pos, "%08x", w);
			if (k > 0) pos += (size_t)k;
		} else {
			int k = snprintf(dst + pos, dst_sz - pos, "oob");
			if (k > 0) pos += (size_t)k;
		}
	}
	if (pos + 2 <= dst_sz) {
		dst[pos++] = ']';
		dst[pos] = '\0';
	}
}

static int
rw_g32_format_line(char *out, size_t out_sz, const struct rw_g32_draw_info *d)
{
	unsigned limit = rw_g32_probe_limit();
	if (limit == 0 || d->primitive == 1u ||
	    !rw_g32_name_matches(d->vname, d->fname, d->label)) {
		return 0;
	}
	if (__atomic_load_n(&rw_g32_emitted, __ATOMIC_RELAXED) >= limit) {
		return 0;
	}
	unsigned seq = __atomic_fetch_add(&rw_g32_emitted, 1u, __ATOMIC_RELAXED);
	if (seq >= limit) {
		return 0;
	}
	char sc_buf[48], vp_buf[64], ubo_buf[32], a0_buf[160], a3_buf[160];
	if (d->has_scissor) {
		snprintf(sc_buf, sizeof(sc_buf), "%u,%u,%u,%u",
		         d->scissor[0], d->scissor[1], d->scissor[2], d->scissor[3]);
	} else {
		snprintf(sc_buf, sizeof(sc_buf), "none");
	}
	if (d->has_viewport) {
		snprintf(vp_buf, sizeof(vp_buf), "%.0f,%.0f,%.0f,%.0f",
		         d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3]);
	} else {
		snprintf(vp_buf, sizeof(vp_buf), "none");
	}
	if (d->ubo1_bytes != NULL && d->ubo1_offset + 0x96u <= d->ubo1_len) {
		uint16_t raw = 0;
		memcpy(&raw, d->ubo1_bytes + d->ubo1_offset + 0x94u, 2u);
		snprintf(ubo_buf, sizeof(ubo_buf), "0x%04x(%.3f)", raw, rw_g32_f16_to_f32(raw));
	} else {
		snprintf(ubo_buf, sizeof(ubo_buf), "none");
	}
	rw_g32_fmt_attr(a0_buf, sizeof(a0_buf), &d->attr0, d->attr0_bytes,
	                d->attr0_len, d->attr0_buf_off, d);
	rw_g32_fmt_attr(a3_buf, sizeof(a3_buf), &d->attr3, d->attr3_bytes,
	                d->attr3_len, d->attr3_buf_off, d);
	snprintf(out, out_sz,
	         "g32-probe #%u v=%s f=%s label=%s prim=%u start=%u count=%u idx=%s(%zu) "
	         "blend=%u mask=%#x rgb=%u/%u/%u a=%u/%u/%u sc=%s vp=%s ubo1_94=%s "
	         "attr0=%s attr3=%s\n",
	         seq, d->vname ? d->vname : "-", d->fname ? d->fname : "-",
	         d->label ? d->label : "-", d->primitive, d->vertex_start, d->vertex_count,
	         d->indexed ? (d->index_type == 1u ? "u32" : "u16") : "none",
	         d->indexed ? d->index_offset : (size_t)0,
	         d->blend_enabled, d->write_mask,
	         d->src_rgb, d->dst_rgb, d->op_rgb, d->src_a, d->dst_a, d->op_a,
	         sc_buf, vp_buf, ubo_buf, a0_buf, a3_buf);
	return 1;
}

#ifndef ANNIBALE_G32_PROBE_HOST_TEST
static void
rw_g32_probe_draw(AnnibaleMetalRenderPipeline *pipeline, const AnnibaleMetalWork *work)
{
	if (pipeline == nil || work == nil || work->primitive == 1u) {
		return;
	}
	NSString *vn = [pipeline vertexFunctionName];
	NSString *fn = [pipeline fragmentFunctionName];
	NSString *lb = [pipeline label];
	struct rw_g32_draw_info d;
	memset(&d, 0, sizeof(d));
	d.vname = (vn != nil && [vn length] > 0) ? [vn UTF8String] : NULL;
	d.fname = (fn != nil && [fn length] > 0) ? [fn UTF8String] : NULL;
	d.label = (lb != nil && [lb length] > 0) ? [lb UTF8String] : NULL;
	if (!rw_g32_name_matches(d.vname, d.fname, d.label)) {
		return;
	}
	d.primitive = work->primitive;
	d.vertex_start = work->vertexStart;
	d.vertex_count = work->vertexCount;
	d.indexed = work->indexBuffer != nil ? 1 : 0;
	d.index_type = work->indexType;
	d.index_offset = work->indexBufferOffset;
	d.base_vertex = work->baseVertex;
	if (work->indexBuffer != nil && [work->indexBuffer respondsToSelector:@selector(contents)]) {
		d.index_bytes = (const uint8_t *)[work->indexBuffer contents];
		d.index_len = (size_t)[work->indexBuffer length];
	}
	const struct rwa_color_blend *b = [pipeline colorBlendAtIndex:0];
	d.blend_enabled = b != NULL ? b->blending_enabled : 0u;
	d.write_mask = b != NULL ? b->write_mask : 0xfu;
	d.src_rgb = b != NULL ? b->source_rgb_blend_factor : 1u;
	d.dst_rgb = b != NULL ? b->destination_rgb_blend_factor : 0u;
	d.op_rgb = b != NULL ? b->rgb_blend_operation : 0u;
	d.src_a = b != NULL ? b->source_alpha_blend_factor : 1u;
	d.dst_a = b != NULL ? b->destination_alpha_blend_factor : 0u;
	d.op_a = b != NULL ? b->alpha_blend_operation : 0u;
	d.has_scissor = work->hasScissorRect ? 1 : 0;
	for (unsigned i = 0; i < 4; i++) {
		d.scissor[i] = work->scissorRect[i];
		d.viewport[i] = work->viewportRect[i];
	}
	d.has_viewport = work->hasViewportRect ? 1 : 0;
	if (work->count > 1u && work->buffers[1] != nil &&
	    [work->buffers[1] respondsToSelector:@selector(contents)]) {
		d.ubo1_bytes = (const uint8_t *)[work->buffers[1] contents];
		d.ubo1_len = (size_t)[work->buffers[1] length];
		d.ubo1_offset = work->bufferOffsets[1];
	}
	const struct rw_g32_vattr_meta *m0 = [pipeline vertexAttrMeta:0];
	if (m0 != NULL) {
		d.attr0 = *m0;
		if (m0->buffer_index < work->vertexBufferCount &&
		    work->vertexBuffers[m0->buffer_index] != nil &&
		    [work->vertexBuffers[m0->buffer_index] respondsToSelector:@selector(contents)]) {
			d.attr0_bytes = (const uint8_t *)[work->vertexBuffers[m0->buffer_index] contents];
			d.attr0_len = (size_t)[work->vertexBuffers[m0->buffer_index] length];
			d.attr0_buf_off = work->vertexBufferOffsets[m0->buffer_index];
		}
	}
	const struct rw_g32_vattr_meta *m3 = [pipeline vertexAttrMeta:3];
	if (m3 != NULL) {
		d.attr3 = *m3;
		if (m3->buffer_index < work->vertexBufferCount &&
		    work->vertexBuffers[m3->buffer_index] != nil &&
		    [work->vertexBuffers[m3->buffer_index] respondsToSelector:@selector(contents)]) {
			d.attr3_bytes = (const uint8_t *)[work->vertexBuffers[m3->buffer_index] contents];
			d.attr3_len = (size_t)[work->vertexBuffers[m3->buffer_index] length];
			d.attr3_buf_off = work->vertexBufferOffsets[m3->buffer_index];
		}
	}
	char line[640];
	if (rw_g32_format_line(line, sizeof(line), &d)) {
		rw_print("%s", line);
	}
}
#endif

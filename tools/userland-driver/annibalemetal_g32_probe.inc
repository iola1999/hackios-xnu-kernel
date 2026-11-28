/*
 * G32 env-gated draw-state probe for VfxLlnXh / Tcma*Lln* / CAGradientLayer pipelines
 * (ANNIBALE_G32_PROBE=1 or =<max_lines>, off by default).
 *
 * When unset or "0", rw_g32_probe_limit() caches 0 on first call and recordDraw: takes zero
 * probe branches and emits zero log lines. Included by annibalemetal_renderencoder.inc (and
 * directly by host ASan+UBSan tests with ANNIBALE_G32_PROBE_HOST_TEST).
 */

#define RW_G32_MAX_TEX_SLOTS 16u

struct rw_g32_vattr_meta {
	unsigned format;
	unsigned offset;
	unsigned buffer_index;
	unsigned stride;
};

struct rw_g32_tex_meta {
	unsigned fmt;
	unsigned w;
	unsigned h;
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
	struct rw_g32_vattr_meta attr1;
	const uint8_t *attr1_bytes;
	size_t attr1_len;
	size_t attr1_buf_off;
	struct rw_g32_vattr_meta attr3;
	const uint8_t *attr3_bytes;
	size_t attr3_len;
	size_t attr3_buf_off;
	struct rw_g32_tex_meta tex[RW_G32_MAX_TEX_SLOTS];
	unsigned tex_count;
	int has_lut_buf;
	unsigned lut_slot;
	unsigned lut_fmt;
	unsigned lut_w;
	const uint8_t *lut_bytes;
	size_t lut_len;
	size_t lut_offset;
	const uint8_t *fbuf2_bytes;
	size_t fbuf2_len;
	size_t fbuf2_offset;
	const uint8_t *fbuf4_bytes;
	size_t fbuf4_len;
	size_t fbuf4_offset;
	const uint8_t *fbuf5_bytes;
	size_t fbuf5_len;
	size_t fbuf5_offset;
};

static int rw_g32_limit_cache = -1;
static int rw_g32_filter_init = 0;
static const char *rw_g32_filter_cache = NULL;
static unsigned rw_g32_emitted = 0;
static unsigned rw_g32_tex_emitted = 0;

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

static const char *
rw_g32_probe_filter(void)
{
	if (__builtin_expect(!rw_g32_filter_init, 0)) {
		const char *val = getenv("ANNIBALE_G32_FILTER");
		rw_g32_filter_cache = (val != NULL && val[0] != '\0') ? val : NULL;
		rw_g32_filter_init = 1;
	}
	return rw_g32_filter_cache;
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

static int
rw_g32_tex_matches(unsigned pixel_format, unsigned width, unsigned height)
{
	if (width == 0u || height == 0u) {
		return 0;
	}
	if (height == 1u) {
		return 1;
	}
	if (pixel_format == 115u && width == 256u && height <= 4u) {
		return 1;
	}
	return 0;
}

static int
rw_g32_find_lut_slot(const struct rw_g32_draw_info *d)
{
	unsigned n = d->tex_count < RW_G32_MAX_TEX_SLOTS ? d->tex_count : RW_G32_MAX_TEX_SLOTS;
	for (unsigned i = 0; i < n; i++) {
		if (rw_g32_tex_matches(d->tex[i].fmt, d->tex[i].w, d->tex[i].h)) {
			return (int)i;
		}
	}
	return -1;
}

static int
rw_g32_draw_matches(const struct rw_g32_draw_info *d)
{
	const char *flt = rw_g32_probe_filter();
	if (flt != NULL) {
		if ((d->vname != NULL && strstr(d->vname, flt) != NULL) ||
		    (d->fname != NULL && strstr(d->fname, flt) != NULL) ||
		    (d->label != NULL && strstr(d->label, flt) != NULL)) {
			return 1;
		}
		return 0;
	}
	if (rw_g32_name_matches(d->vname, d->fname, d->label)) {
		return 1;
	}
	if (rw_g32_find_lut_slot(d) >= 0) {
		return 1;
	}
	if (d->fbuf5_len > 0u && d->fbuf5_len <= 16u) {
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

static void
rw_g32_fmt_half4(char *dst, size_t dst_sz, const uint8_t *p)
{
	uint16_t h[4];
	memcpy(h, p, sizeof(h));
	snprintf(dst, dst_sz, "(%.3f,%.3f,%.3f,%.3f)",
	         rw_g32_f16_to_f32(h[0]), rw_g32_f16_to_f32(h[1]),
	         rw_g32_f16_to_f32(h[2]), rw_g32_f16_to_f32(h[3]));
}

static void
rw_g32_scan_lut_row(unsigned fmt, unsigned width, const uint8_t *bytes, size_t avail_bytes,
                    unsigned *out_nz, unsigned *out_nan, unsigned *out_scan,
                    char *first_nz, size_t first_sz,
                    char *mid_buf, size_t mid_sz,
                    char *last_buf, size_t last_sz)
{
	snprintf(first_nz, first_sz, "none");
	snprintf(mid_buf, mid_sz, "none");
	snprintf(last_buf, last_sz, "none");
	*out_nz = 0u;
	*out_nan = 0u;
	*out_scan = 0u;
	unsigned scan = width > 256u ? 256u : width;
	if (bytes == NULL || scan == 0u) {
		return;
	}
	*out_scan = scan;
	if (fmt == 115u) {
		unsigned max_px = (unsigned)(avail_bytes / 8u);
		if (scan > max_px) {
			scan = max_px;
		}
		*out_scan = scan;
		if (scan == 0u) {
			return;
		}
		int got_nz = 0;
		for (unsigned i = 0; i < scan; i++) {
			const uint8_t *px = bytes + (size_t)i * 8u;
			uint16_t h[4];
			memcpy(h, px, sizeof(h));
			if ((h[0] | h[1] | h[2] | h[3]) != 0u) {
				(*out_nz)++;
				if (!got_nz) {
					rw_g32_fmt_half4(first_nz, first_sz, px);
					got_nz = 1;
				}
			}
			if (((h[0] & 0x7c00u) == 0x7c00u) || ((h[1] & 0x7c00u) == 0x7c00u) ||
			    ((h[2] & 0x7c00u) == 0x7c00u) || ((h[3] & 0x7c00u) == 0x7c00u)) {
				(*out_nan)++;
			}
		}
		rw_g32_fmt_half4(mid_buf, mid_sz, bytes + (size_t)(scan / 2u) * 8u);
		rw_g32_fmt_half4(last_buf, last_sz, bytes + (size_t)(scan - 1u) * 8u);
	} else if (fmt == 80u || fmt == 70u || fmt == 81u || fmt == 71u) {
		unsigned max_px = (unsigned)(avail_bytes / 4u);
		if (scan > max_px) {
			scan = max_px;
		}
		*out_scan = scan;
		if (scan == 0u) {
			return;
		}
		int got_nz = 0;
		for (unsigned i = 0; i < scan; i++) {
			uint32_t raw = 0;
			memcpy(&raw, bytes + (size_t)i * 4u, 4u);
			if (raw != 0u) {
				(*out_nz)++;
				if (!got_nz) {
					snprintf(first_nz, first_sz, "0x%08x", raw);
					got_nz = 1;
				}
			}
		}
		uint32_t mid_v = 0, last_v = 0;
		memcpy(&mid_v, bytes + (size_t)(scan / 2u) * 4u, 4u);
		memcpy(&last_v, bytes + (size_t)(scan - 1u) * 4u, 4u);
		snprintf(mid_buf, mid_sz, "0x%08x", mid_v);
		snprintf(last_buf, last_sz, "0x%08x", last_v);
	}
}

static int
rw_g32_format_tex_create(char *out, size_t out_sz, unsigned fmt, unsigned type,
                         unsigned w, unsigned h, int has_surface)
{
	unsigned limit = rw_g32_probe_limit();
	if (limit == 0u || !rw_g32_tex_matches(fmt, w, h)) {
		return 0;
	}
	if (__atomic_load_n(&rw_g32_tex_emitted, __ATOMIC_RELAXED) >= limit) {
		return 0;
	}
	unsigned seq = __atomic_fetch_add(&rw_g32_tex_emitted, 1u, __ATOMIC_RELAXED);
	if (seq >= limit) {
		return 0;
	}
	snprintf(out, out_sz, "g32-tex-new #%u fmt=%u type=%u %ux%u surf=%d\n",
	         seq, fmt, type, w, h, has_surface);
	return 1;
}

static int
rw_g32_format_tex_write(char *out, size_t out_sz, unsigned fmt, unsigned tex_w, unsigned tex_h,
                        unsigned w, unsigned h, const void *bytes, size_t stride, int rc)
{
	unsigned limit = rw_g32_probe_limit();
	if (limit == 0u || !rw_g32_tex_matches(fmt, tex_w, tex_h)) {
		return 0;
	}
	if (__atomic_load_n(&rw_g32_tex_emitted, __ATOMIC_RELAXED) >= limit) {
		return 0;
	}
	unsigned seq = __atomic_fetch_add(&rw_g32_tex_emitted, 1u, __ATOMIC_RELAXED);
	if (seq >= limit) {
		return 0;
	}
	char first_nz[48], mid_buf[48], last_buf[48];
	unsigned nz = 0u, nan_inf = 0u, scan = 0u;
	size_t bpp = (fmt == 115u) ? 8u : 4u;
	size_t avail = stride > 0u ? stride : (size_t)w * bpp;
	rw_g32_scan_lut_row(fmt, w, (const uint8_t *)bytes, avail, &nz, &nan_inf, &scan,
	                    first_nz, sizeof(first_nz), mid_buf, sizeof(mid_buf),
	                    last_buf, sizeof(last_buf));
	snprintf(out, out_sz,
	         "g32-tex-wr #%u fmt=%u tex=%ux%u wr=%ux%u stride=%zu rc=%d nz=%u/%u nan=%u "
	         "nz0=%s mid=%s last=%s\n",
	         seq, fmt, tex_w, tex_h, w, h, stride, rc, nz, scan, nan_inf,
	         first_nz, mid_buf, last_buf);
	return 1;
}

static int
rw_g32_format_buf_tex(char *out, size_t out_sz, unsigned fmt, unsigned w, unsigned h,
                      const void *buf_bytes, size_t buf_len, size_t offset,
                      size_t row_bytes, int ok)
{
	unsigned limit = rw_g32_probe_limit();
	if (limit == 0u || !rw_g32_tex_matches(fmt, w, h)) {
		return 0;
	}
	if (__atomic_load_n(&rw_g32_tex_emitted, __ATOMIC_RELAXED) >= limit) {
		return 0;
	}
	unsigned seq = __atomic_fetch_add(&rw_g32_tex_emitted, 1u, __ATOMIC_RELAXED);
	if (seq >= limit) {
		return 0;
	}
	char first_nz[48], mid_buf[48], last_buf[48];
	unsigned nz = 0u, nan_inf = 0u, scan = 0u;
	const uint8_t *base = NULL;
	size_t avail = 0u;
	if (buf_bytes != NULL && offset < buf_len) {
		base = (const uint8_t *)buf_bytes + offset;
		avail = buf_len - offset;
	}
	rw_g32_scan_lut_row(fmt, w, base, avail, &nz, &nan_inf, &scan,
	                    first_nz, sizeof(first_nz), mid_buf, sizeof(mid_buf),
	                    last_buf, sizeof(last_buf));
	snprintf(out, out_sz,
	         "g32-buf-tex #%u fmt=%u tex=%ux%u off=%zu/%zu stride=%zu ok=%d nz=%u/%u nan=%u "
	         "nz0=%s mid=%s last=%s\n",
	         seq, fmt, w, h, offset, buf_len, row_bytes, ok, nz, scan, nan_inf,
	         first_nz, mid_buf, last_buf);
	return 1;
}

static void
rw_g32_fmt_textures(char *dst, size_t dst_sz, const struct rw_g32_draw_info *d)
{
	unsigned n = d->tex_count < RW_G32_MAX_TEX_SLOTS ? d->tex_count : RW_G32_MAX_TEX_SLOTS;
	size_t pos = 0;
	int any = 0;
	for (unsigned i = 0; i < n && pos + 24 < dst_sz; i++) {
		if (d->tex[i].w == 0u || d->tex[i].h == 0u) {
			continue;
		}
		int k = snprintf(dst + pos, dst_sz - pos, "%st%u=fmt%u:%ux%u",
		                 any ? "," : "", i, d->tex[i].fmt, d->tex[i].w, d->tex[i].h);
		if (k > 0) {
			pos += (size_t)k;
			any = 1;
		}
	}
	if (!any) {
		snprintf(dst, dst_sz, "none");
	}
}

static int
rw_g32_format_line(char *out, size_t out_sz, const struct rw_g32_draw_info *d)
{
	unsigned limit = rw_g32_probe_limit();
	if (limit == 0 || d->primitive == 1u || !rw_g32_draw_matches(d)) {
		return 0;
	}
	if (__atomic_load_n(&rw_g32_emitted, __ATOMIC_RELAXED) >= limit) {
		return 0;
	}
	unsigned seq = __atomic_fetch_add(&rw_g32_emitted, 1u, __ATOMIC_RELAXED);
	if (seq >= limit) {
		return 0;
	}
	char sc_buf[48], vp_buf[64], ubo_buf[32], tex_buf[160], fb2_buf[48], fb4_buf[704], fb5_buf[64], lut_buf[200];
	char a0_buf[160], a1_buf[160], a3_buf[160];
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
	rw_g32_fmt_textures(tex_buf, sizeof(tex_buf), d);
	if (d->fbuf2_bytes != NULL && d->fbuf2_offset + 4u <= d->fbuf2_len) {
		float lod = 0.0f;
		memcpy(&lod, d->fbuf2_bytes + d->fbuf2_offset, 4u);
		snprintf(fb2_buf, sizeof(fb2_buf), "len%zu+off%zu:%.3f",
		         d->fbuf2_len, d->fbuf2_offset, lod);
	} else if (d->fbuf2_len > 0u) {
		snprintf(fb2_buf, sizeof(fb2_buf), "len%zu+off%zu:oob",
		         d->fbuf2_len, d->fbuf2_offset);
	} else {
		snprintf(fb2_buf, sizeof(fb2_buf), "none");
	}
	if (d->fbuf4_bytes != NULL && d->fbuf4_offset + 8u <= d->fbuf4_len) {
		size_t avail = d->fbuf4_len - d->fbuf4_offset;
		unsigned n_h4 = (unsigned)(avail / 8u);
		if (n_h4 > 21u) {
			n_h4 = 21u;
		}
		int p = snprintf(fb4_buf, sizeof(fb4_buf), "len%zu+off%zu[",
		                 d->fbuf4_len, d->fbuf4_offset);
		size_t pos = (p > 0 && (size_t)p < sizeof(fb4_buf)) ? (size_t)p : sizeof(fb4_buf) - 1u;
		for (unsigned i = 0; i < n_h4 && pos + 32u < sizeof(fb4_buf); i++) {
			char h4[48];
			rw_g32_fmt_half4(h4, sizeof(h4), d->fbuf4_bytes + d->fbuf4_offset + (size_t)i * 8u);
			int k = snprintf(fb4_buf + pos, sizeof(fb4_buf) - pos, "%s%s", i ? "," : "", h4);
			if (k > 0) pos += (size_t)k;
		}
		if (pos + 2u <= sizeof(fb4_buf)) {
			fb4_buf[pos++] = ']';
			fb4_buf[pos] = '\0';
		}
	} else if (d->fbuf4_len > 0u) {
		snprintf(fb4_buf, sizeof(fb4_buf), "len%zu+off%zu:oob",
		         d->fbuf4_len, d->fbuf4_offset);
	} else {
		snprintf(fb4_buf, sizeof(fb4_buf), "none");
	}
	if (d->fbuf5_bytes != NULL && d->fbuf5_offset + 2u <= d->fbuf5_len) {
		uint16_t raw = 0;
		memcpy(&raw, d->fbuf5_bytes + d->fbuf5_offset, 2u);
		snprintf(fb5_buf, sizeof(fb5_buf), "len%zu+off%zu:0x%04x(%.4f)",
		         d->fbuf5_len, d->fbuf5_offset, raw, rw_g32_f16_to_f32(raw));
	} else if (d->fbuf5_len > 0u) {
		snprintf(fb5_buf, sizeof(fb5_buf), "len%zu+off%zu:oob",
		         d->fbuf5_len, d->fbuf5_offset);
	} else {
		snprintf(fb5_buf, sizeof(fb5_buf), "none");
	}
	if (d->has_lut_buf) {
		char first_nz[48], mid_buf[48], last_buf[48];
		unsigned nz = 0u, nan_inf = 0u, scan = 0u;
		const uint8_t *base = NULL;
		size_t avail = 0u;
		if (d->lut_bytes != NULL && d->lut_offset < d->lut_len) {
			base = d->lut_bytes + d->lut_offset;
			avail = d->lut_len - d->lut_offset;
		}
		rw_g32_scan_lut_row(d->lut_fmt, d->lut_w, base, avail, &nz, &nan_inf, &scan,
		                    first_nz, sizeof(first_nz), mid_buf, sizeof(mid_buf),
		                    last_buf, sizeof(last_buf));
		snprintf(lut_buf, sizeof(lut_buf), "t%u:nz=%u/%u,nan=%u,nz0=%s,mid=%s,last=%s",
		         d->lut_slot, nz, scan, nan_inf, first_nz, mid_buf, last_buf);
	} else {
		snprintf(lut_buf, sizeof(lut_buf), "none");
	}
	rw_g32_fmt_attr(a0_buf, sizeof(a0_buf), &d->attr0, d->attr0_bytes,
	                d->attr0_len, d->attr0_buf_off, d);
	rw_g32_fmt_attr(a1_buf, sizeof(a1_buf), &d->attr1, d->attr1_bytes,
	                d->attr1_len, d->attr1_buf_off, d);
	rw_g32_fmt_attr(a3_buf, sizeof(a3_buf), &d->attr3, d->attr3_bytes,
	                d->attr3_len, d->attr3_buf_off, d);
	snprintf(out, out_sz,
	         "g32-probe #%u v=%s f=%s label=%s prim=%u start=%u count=%u idx=%s(%zu) "
	         "blend=%u mask=%#x rgb=%u/%u/%u a=%u/%u/%u sc=%s vp=%s tex=%s fbuf2=%s fbuf4=%s fbuf5=%s "
	         "lut=%s ubo1_94=%s attr0=%s attr1=%s attr3=%s\n",
	         seq, d->vname ? d->vname : "-", d->fname ? d->fname : "-",
	         d->label ? d->label : "-", d->primitive, d->vertex_start, d->vertex_count,
	         d->indexed ? (d->index_type == 1u ? "u32" : "u16") : "none",
	         d->indexed ? d->index_offset : (size_t)0,
	         d->blend_enabled, d->write_mask,
	         d->src_rgb, d->dst_rgb, d->op_rgb, d->src_a, d->dst_a, d->op_a,
	         sc_buf, vp_buf, tex_buf, fb2_buf, fb4_buf, fb5_buf, lut_buf, ubo_buf, a0_buf, a1_buf, a3_buf);
	return 1;
}

#ifndef ANNIBALE_G32_PROBE_HOST_TEST
static void
rw_g32_probe_tex_create(const struct rwa_texture_shape *shape, int has_surface)
{
	char line[256];
	if (shape == NULL) {
		return;
	}
	if (rw_g32_format_tex_create(line, sizeof(line), shape->pixel_format, shape->type,
	                             shape->width, shape->height, has_surface)) {
		rw_print("%s", line);
	}
}

static void
rw_g32_probe_tex_write(const struct rwa_texture_shape *shape, unsigned w, unsigned h,
                       const void *bytes, size_t stride, int rc)
{
	char line[384];
	if (shape == NULL) {
		return;
	}
	if (rw_g32_format_tex_write(line, sizeof(line), shape->pixel_format,
	                            shape->width, shape->height, w, h, bytes, stride, rc)) {
		rw_print("%s", line);
	}
}

static void
rw_g32_probe_buf_tex(const struct rwa_texture_shape *shape, const void *buf_bytes,
                     size_t buf_len, size_t offset, size_t row_bytes, int ok)
{
	char line[384];
	if (shape == NULL) {
		return;
	}
	if (rw_g32_format_buf_tex(line, sizeof(line), shape->pixel_format,
	                          shape->width, shape->height, buf_bytes, buf_len,
	                          offset, row_bytes, ok)) {
		rw_print("%s", line);
	}
}

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
	unsigned tcnt = work->textureCount < RW_G32_MAX_TEX_SLOTS
	                    ? work->textureCount : RW_G32_MAX_TEX_SLOTS;
	d.tex_count = tcnt;
	for (unsigned i = 0; i < tcnt; i++) {
		if (work->textures[i] != nil &&
		    [work->textures[i] respondsToSelector:@selector(pixelFormat)]) {
			d.tex[i].fmt = (unsigned)[work->textures[i] pixelFormat];
			d.tex[i].w = (unsigned)[work->textures[i] width];
			d.tex[i].h = (unsigned)[work->textures[i] height];
		}
	}
	if (work->count > 2u && work->buffers[2] != nil &&
	    [work->buffers[2] respondsToSelector:@selector(contents)]) {
		d.fbuf2_bytes = (const uint8_t *)[work->buffers[2] contents];
		d.fbuf2_len = (size_t)[work->buffers[2] length];
		d.fbuf2_offset = work->bufferOffsets[2];
	}
	if (work->count > 4u && work->buffers[4] != nil &&
	    [work->buffers[4] respondsToSelector:@selector(contents)]) {
		d.fbuf4_bytes = (const uint8_t *)[work->buffers[4] contents];
		d.fbuf4_len = (size_t)[work->buffers[4] length];
		d.fbuf4_offset = work->bufferOffsets[4];
	}
	if (work->count > 5u && work->buffers[5] != nil &&
	    [work->buffers[5] respondsToSelector:@selector(contents)]) {
		d.fbuf5_bytes = (const uint8_t *)[work->buffers[5] contents];
		d.fbuf5_len = (size_t)[work->buffers[5] length];
		d.fbuf5_offset = work->bufferOffsets[5];
	}
	if (!rw_g32_draw_matches(&d)) {
		return;
	}
	int lut_slot = rw_g32_find_lut_slot(&d);
	if (lut_slot >= 0 && work->textures[lut_slot] != nil &&
	    [work->textures[lut_slot] respondsToSelector:@selector(buffer)]) {
		id lbuf = [work->textures[lut_slot] buffer];
		if (lbuf != nil && [lbuf respondsToSelector:@selector(contents)]) {
			d.has_lut_buf = 1;
			d.lut_slot = (unsigned)lut_slot;
			d.lut_fmt = d.tex[lut_slot].fmt;
			d.lut_w = d.tex[lut_slot].w;
			d.lut_bytes = (const uint8_t *)[lbuf contents];
			d.lut_len = (size_t)[lbuf length];
			d.lut_offset = [work->textures[lut_slot] respondsToSelector:@selector(bufferOffset)]
			                   ? (size_t)[work->textures[lut_slot] bufferOffset] : 0u;
		}
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
	const struct rw_g32_vattr_meta *m1 = [pipeline vertexAttrMeta:1];
	if (m1 != NULL) {
		d.attr1 = *m1;
		if (m1->buffer_index < work->vertexBufferCount &&
		    work->vertexBuffers[m1->buffer_index] != nil &&
		    [work->vertexBuffers[m1->buffer_index] respondsToSelector:@selector(contents)]) {
			d.attr1_bytes = (const uint8_t *)[work->vertexBuffers[m1->buffer_index] contents];
			d.attr1_len = (size_t)[work->vertexBuffers[m1->buffer_index] length];
			d.attr1_buf_off = work->vertexBufferOffsets[m1->buffer_index];
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
	char line[2048];
	if (rw_g32_format_line(line, sizeof(line), &d)) {
		rw_print("%s", line);
	}
}
#endif

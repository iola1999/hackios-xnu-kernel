/*
 * DPU 12.0 hardware scaling for single-layer full-screen swaps (boot-arg fb_dpu_scale=1, default 0).
 *
 * When enabled on SM8750 dual-VIG split scanout (vig0 + vig1 -> lm0 + lm1), the presenter copies
 * the raw 1206x2622 BGRA8888 or l10r (BGR10A2) surface into splash_region A/B buffers and programs
 * vig0/vig1 QSEED3LITE scalers (scaler_blk at +0xa00, mainline Linux dpu_12_0_sm8750.h /
 * dpu_hw_util.c / dpu_hw_sspp.c) to scale 603+603 -> 578+578 horizontally and 2622 -> 2510
 * vertically in hardware. Any multi-layer swap, partial-rect swap, unsupported format, quiesce, or
 * poweroff restores the exact boot 1:1 1156x2510 register snapshot.
 *
 * Included by MiniFramebufferFlip.inc (and directly by host unit tests with FB_DPU_SCALE_HOST_TEST).
 */

#ifndef FB_DPU_SCALE_HOST_TEST
static TUNABLE(uint32_t, gDpuScale, "fb_dpu_scale", 0);
#endif

#define FLIP_SSPP_SRC_UNPACK_PATTERN    0x034u
#define FLIP_SSPP_SW_PIX_EXT_C0_LR      0x100u
#define FLIP_SSPP_SW_PIX_EXT_C0_TB      0x104u
#define FLIP_SSPP_SW_PIX_EXT_C0_REQ     0x108u
#define FLIP_SSPP_SW_PIX_EXT_C1C2_LR    0x110u
#define FLIP_SSPP_SW_PIX_EXT_C1C2_TB    0x114u
#define FLIP_SSPP_SW_PIX_EXT_C1C2_REQ   0x118u
#define FLIP_SSPP_SW_PIX_EXT_C3_LR      0x120u
#define FLIP_SSPP_SW_PIX_EXT_C3_TB      0x124u
#define FLIP_SSPP_SW_PIX_EXT_C3_REQ     0x128u

#define FLIP_QSEED3_OFFSET              0xa00u
#define FLIP_QSEED3_OP_MODE             0x04u
#define FLIP_QSEED3_PHASE_STEP_Y_H      0x10u
#define FLIP_QSEED3_PHASE_STEP_Y_V      0x14u
#define FLIP_QSEED3_PHASE_STEP_UV_H     0x18u
#define FLIP_QSEED3_PHASE_STEP_UV_V     0x1cu
#define FLIP_QSEED3_PRELOAD             0x20u
#define FLIP_QSEED3_SRC_SIZE_Y_RGB_A    0x40u
#define FLIP_QSEED3_SRC_SIZE_UV         0x44u
#define FLIP_QSEED3_DST_SIZE            0x48u
#define FLIP_QSEED3_PHASE_INIT_Y_H      0x90u
#define FLIP_QSEED3_PHASE_INIT_Y_V      0x94u
#define FLIP_QSEED3_PHASE_INIT_UV_H     0x98u
#define FLIP_QSEED3_PHASE_INIT_UV_V     0x9cu

#define FLIP_SRC_FORMAT_DX_BIT          (1u << 14)
#define FLIP_QSEED3_PHASE_SHIFT         21u
#define FLIP_QSEED3_PHASE_UNIT          (1u << FLIP_QSEED3_PHASE_SHIFT)
#define FLIP_QSEED3_PHASE_INIT_MASK     0x1fffffu
#define FLIP_QSEED3_PHASE_STEP_MASK     0xffffffu
#define FLIP_QSEED3_OP_ENABLE           (1u << 0)
#define FLIP_QSEED3_OP_ALPHA_EN         (1u << 10)
#define FLIP_QSEED3_OP_8BIT             (1u << 14)
#define FLIP_QSEED3_OP_FILTER_BIL       (3u << 16)
#define FLIP_QSEED3_OP_ALPHA_BIL        (1u << 29)
#define FLIP_QSEED3_OP_BLEND_CFG        (1u << 31)
#define FLIP_QSEED3_PRELOAD_QSEED4      ((4u << 0) | (2u << 8) | (4u << 16) | (2u << 24))
#define FLIP_DPU_SCALE_SLOT_BYTES       0x1000000u      /* 16 MiB per A/B slot inside 43 MiB splash_region */
#define FLIP_FOURCC_BGRA                0x42475241u     /* 'BGRA' */
#define FLIP_FOURCC_L10R                0x6c313072u     /* 'l10r' (BGR10A2Unorm) */

struct dpu_pipe_saved {
	uint32_t src_size, src_xy, out_size, out_xy, ystride0;
	uint32_t src_format, unpack_pattern, op_mode;
	uint32_t pe_lr[3], pe_tb[3], pe_req[3];
	uint32_t q_op_mode, q_step_yh, q_step_yv, q_step_uvh, q_step_uvv;
	uint32_t q_preload, q_src_y, q_src_uv, q_dst;
	uint32_t q_init_yh, q_init_yv, q_init_uvh, q_init_uvv;
};

struct dpu_scale_plan_t {
	uint32_t src_x[2], src_w[2], dst_x[2], dst_w[2];
	uint32_t src_h, dst_h, stride;
	uint32_t src_format, unpack_pattern;
	uint32_t phase_step_h, phase_step_v;
	uint32_t init_phase_x[2], init_phase_y;
	uint32_t preload, qseed_op_mode;
	bool     scale_enable;
};

static struct {
	bool                    capable;
	bool                    active;
	uint32_t                dst_w[2], dst_h;
	uint32_t                src_w, src_h, stride, fmt;
	struct dpu_pipe_saved   boot[2];
	uint64_t                frames;
} dpuScale;

static inline uint32_t
dpu_scale_l10r_to_bgra(uint32_t p)
{
	uint32_t b = ((p & 0x3ffu) * 255u + 511u) / 1023u;
	uint32_t g = (((p >> 10) & 0x3ffu) * 255u + 511u) / 1023u;
	uint32_t r = (((p >> 20) & 0x3ffu) * 255u + 511u) / 1023u;
	uint32_t a = ((p >> 30) & 0x3u) * 85u;

	return b | (g << 8) | (r << 16) | (a << 24);
}

static void
dpu_pipe_save(unsigned p, struct dpu_pipe_saved * s)
{
	uint32_t b = FLIP_MDP + flipPipe[p].base;
	uint32_t q = b + FLIP_QSEED3_OFFSET;

	s->src_size = flip_rd(b + FLIP_SSPP_SRC_SIZE);
	s->src_xy = flip_rd(b + FLIP_SSPP_SRC_XY);
	s->out_size = flip_rd(b + FLIP_SSPP_OUT_SIZE);
	s->out_xy = flip_rd(b + FLIP_SSPP_OUT_XY);
	s->ystride0 = flip_rd(b + FLIP_SSPP_YSTRIDE0);
	s->src_format = flip_rd(b + FLIP_SSPP_SRC_FORMAT);
	s->unpack_pattern = flip_rd(b + FLIP_SSPP_SRC_UNPACK_PATTERN);
	s->op_mode = flip_rd(b + FLIP_SSPP_OP_MODE);
	s->pe_lr[0] = flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C0_LR);
	s->pe_tb[0] = flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C0_TB);
	s->pe_req[0] = flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C0_REQ);
	s->pe_lr[1] = flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C1C2_LR);
	s->pe_tb[1] = flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C1C2_TB);
	s->pe_req[1] = flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C1C2_REQ);
	s->pe_lr[2] = flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C3_LR);
	s->pe_tb[2] = flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C3_TB);
	s->pe_req[2] = flip_rd(b + FLIP_SSPP_SW_PIX_EXT_C3_REQ);
	s->q_op_mode = flip_rd(q + FLIP_QSEED3_OP_MODE);
	s->q_step_yh = flip_rd(q + FLIP_QSEED3_PHASE_STEP_Y_H);
	s->q_step_yv = flip_rd(q + FLIP_QSEED3_PHASE_STEP_Y_V);
	s->q_step_uvh = flip_rd(q + FLIP_QSEED3_PHASE_STEP_UV_H);
	s->q_step_uvv = flip_rd(q + FLIP_QSEED3_PHASE_STEP_UV_V);
	s->q_preload = flip_rd(q + FLIP_QSEED3_PRELOAD);
	s->q_src_y = flip_rd(q + FLIP_QSEED3_SRC_SIZE_Y_RGB_A);
	s->q_src_uv = flip_rd(q + FLIP_QSEED3_SRC_SIZE_UV);
	s->q_dst = flip_rd(q + FLIP_QSEED3_DST_SIZE);
	s->q_init_yh = flip_rd(q + FLIP_QSEED3_PHASE_INIT_Y_H);
	s->q_init_yv = flip_rd(q + FLIP_QSEED3_PHASE_INIT_Y_V);
	s->q_init_uvh = flip_rd(q + FLIP_QSEED3_PHASE_INIT_UV_H);
	s->q_init_uvv = flip_rd(q + FLIP_QSEED3_PHASE_INIT_UV_V);
}

static void
dpu_pipe_restore(unsigned p, const struct dpu_pipe_saved * s)
{
	uint32_t b = FLIP_MDP + flipPipe[p].base;
	uint32_t q = b + FLIP_QSEED3_OFFSET;

	flip_wr(b + FLIP_SSPP_SRC_SIZE, s->src_size);
	flip_wr(b + FLIP_SSPP_SRC_XY, s->src_xy);
	flip_wr(b + FLIP_SSPP_OUT_SIZE, s->out_size);
	flip_wr(b + FLIP_SSPP_OUT_XY, s->out_xy);
	flip_wr(b + FLIP_SSPP_YSTRIDE0, s->ystride0);
	flip_wr(b + FLIP_SSPP_SRC_FORMAT, s->src_format);
	flip_wr(b + FLIP_SSPP_SRC_UNPACK_PATTERN, s->unpack_pattern);
	flip_wr(b + FLIP_SSPP_OP_MODE, s->op_mode);
	flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C0_LR, s->pe_lr[0]);
	flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C0_TB, s->pe_tb[0]);
	flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C0_REQ, s->pe_req[0]);
	flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C1C2_LR, s->pe_lr[1]);
	flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C1C2_TB, s->pe_tb[1]);
	flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C1C2_REQ, s->pe_req[1]);
	flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C3_LR, s->pe_lr[2]);
	flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C3_TB, s->pe_tb[2]);
	flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C3_REQ, s->pe_req[2]);
	flip_wr(q + FLIP_QSEED3_PHASE_INIT_Y_H, s->q_init_yh);
	flip_wr(q + FLIP_QSEED3_PHASE_INIT_Y_V, s->q_init_yv);
	flip_wr(q + FLIP_QSEED3_PHASE_INIT_UV_H, s->q_init_uvh);
	flip_wr(q + FLIP_QSEED3_PHASE_INIT_UV_V, s->q_init_uvv);
	flip_wr(q + FLIP_QSEED3_PHASE_STEP_Y_H, s->q_step_yh);
	flip_wr(q + FLIP_QSEED3_PHASE_STEP_Y_V, s->q_step_yv);
	flip_wr(q + FLIP_QSEED3_PHASE_STEP_UV_H, s->q_step_uvh);
	flip_wr(q + FLIP_QSEED3_PHASE_STEP_UV_V, s->q_step_uvv);
	flip_wr(q + FLIP_QSEED3_PRELOAD, s->q_preload);
	flip_wr(q + FLIP_QSEED3_SRC_SIZE_Y_RGB_A, s->q_src_y);
	flip_wr(q + FLIP_QSEED3_SRC_SIZE_UV, s->q_src_uv);
	flip_wr(q + FLIP_QSEED3_DST_SIZE, s->q_dst);
	flip_wr(q + FLIP_QSEED3_OP_MODE, s->q_op_mode);
}

static bool
dpu_scale_plan(uint32_t src_w, uint32_t src_h, uint32_t dst_w0, uint32_t dst_w1, uint32_t dst_h,
    uint32_t stride, uint32_t fmt, uint32_t boot_format, uint32_t boot_unpack, struct dpu_scale_plan_t * out)
{
	bool is_l10r = fmt == FLIP_FOURCC_L10R;
	uint32_t dst_w = dst_w0 + dst_w1;

	if ((fmt != FLIP_FOURCC_BGRA && !is_l10r) || dst_w0 == 0 || dst_w1 == 0 || dst_h == 0) {
		return false;
	}
	if (src_w < dst_w || src_h < dst_h || src_w > dst_w * 4u || src_h > dst_h * 4u) {
		return false;
	}
	if (stride < src_w * 4u || (stride & 3u) != 0) {
		return false;
	}

	uint32_t step_h = (uint32_t) (((uint64_t) src_w << FLIP_QSEED3_PHASE_SHIFT) / dst_w);
	uint32_t step_v = (uint32_t) (((uint64_t) src_h << FLIP_QSEED3_PHASE_SHIFT) / dst_h);
	int64_t init_x0 = (int64_t) (step_h - FLIP_QSEED3_PHASE_UNIT) / 2;
	int64_t init_y = (int64_t) (step_v - FLIP_QSEED3_PHASE_UNIT) / 2;
	int64_t seam_pos = init_x0 + (int64_t) dst_w0 * (int64_t) step_h;
	uint32_t split_x = (uint32_t) (seam_pos >> FLIP_QSEED3_PHASE_SHIFT);
	int64_t init_x1 = seam_pos - ((int64_t) split_x << FLIP_QSEED3_PHASE_SHIFT);

	if (split_x == 0 || split_x >= src_w) {
		return false;
	}

	out->src_x[0] = 0;
	out->src_w[0] = split_x;
	out->src_x[1] = split_x;
	out->src_w[1] = src_w - split_x;
	out->dst_x[0] = 0;
	out->dst_w[0] = dst_w0;
	out->dst_x[1] = dst_w0;
	out->dst_w[1] = dst_w1;
	out->src_h = src_h;
	out->dst_h = dst_h;
	out->stride = stride;
	out->src_format = (boot_format & ~FLIP_SRC_FORMAT_DX_BIT) | (is_l10r ? FLIP_SRC_FORMAT_DX_BIT : 0u);
	out->unpack_pattern = boot_unpack;
	out->phase_step_h = step_h & FLIP_QSEED3_PHASE_STEP_MASK;
	out->phase_step_v = step_v & FLIP_QSEED3_PHASE_STEP_MASK;
	out->init_phase_x[0] = (uint32_t) init_x0 & FLIP_QSEED3_PHASE_INIT_MASK;
	out->init_phase_x[1] = (uint32_t) init_x1 & FLIP_QSEED3_PHASE_INIT_MASK;
	out->init_phase_y = (uint32_t) init_y & FLIP_QSEED3_PHASE_INIT_MASK;
	out->preload = FLIP_QSEED3_PRELOAD_QSEED4;
	out->scale_enable = (src_w != dst_w || src_h != dst_h);
	out->qseed_op_mode = FLIP_QSEED3_OP_ALPHA_EN | FLIP_QSEED3_OP_ALPHA_BIL |
	    (is_l10r ? 0u : FLIP_QSEED3_OP_8BIT);
	if (out->scale_enable) {
		out->qseed_op_mode |= FLIP_QSEED3_OP_ENABLE | FLIP_QSEED3_OP_FILTER_BIL | FLIP_QSEED3_OP_BLEND_CFG;
	}
	return true;
}

static void
dpu_scale_init(void)
{
	dpuScale.capable = false;
	dpuScale.active = false;
	dpuScale.frames = 0;
	if (gDpuScale == 0 || flip.pipes != 0x3u || flip.offset[0] != 0 || flip.offset[1] != 0) {
		return;
	}
	dpu_pipe_save(0, &dpuScale.boot[0]);
	dpu_pipe_save(1, &dpuScale.boot[1]);
	uint32_t w0 = dpuScale.boot[0].out_size & 0xffffu;
	uint32_t h0 = dpuScale.boot[0].out_size >> 16;
	uint32_t w1 = dpuScale.boot[1].out_size & 0xffffu;
	uint32_t h1 = dpuScale.boot[1].out_size >> 16;
	if (w0 == 0 || w1 == 0 || h0 == 0 || h0 != h1 ||
	    dpuScale.boot[0].out_xy != 0 || dpuScale.boot[1].out_xy != w0) {
		return;
	}
	dpuScale.dst_w[0] = w0;
	dpuScale.dst_w[1] = w1;
	dpuScale.dst_h = h0;
	dpuScale.capable = true;
#ifndef FB_DPU_SCALE_HOST_TEST
	IOLog("MiniIOMobileFramebuffer: flip: DPU QSEED3LITE scaling ready (%u+%ux%u, slot %u MiB, boot fmt %08x unpack %08x)\n",
	    w0, w1, h0, flip.slot_bytes >> 20, dpuScale.boot[0].src_format, dpuScale.boot[0].unpack_pattern);
#endif
}

static bool
flip_config_scale(uint32_t src_w, uint32_t src_h, uint32_t stride, uint32_t fmt)
{
	struct dpu_scale_plan_t plan;

	if (gDpuScale == 0 || !flip.ready || !dpuScale.capable) {
		return false;
	}
	if ((uint64_t) src_h * (uint64_t) stride > (uint64_t) flip.slot_bytes) {
		return false;
	}
	if (dpuScale.active && dpuScale.src_w == src_w && dpuScale.src_h == src_h &&
	    dpuScale.stride == stride && dpuScale.fmt == fmt) {
		return true;
	}
	if (!dpu_scale_plan(src_w, src_h, dpuScale.dst_w[0], dpuScale.dst_w[1], dpuScale.dst_h,
	    stride, fmt, dpuScale.boot[0].src_format, dpuScale.boot[0].unpack_pattern, &plan)) {
		return false;
	}
	for (unsigned p = 0; p < 2; p++) {
		uint32_t b = FLIP_MDP + flipPipe[p].base;
		uint32_t q = b + FLIP_QSEED3_OFFSET;
		uint32_t src_sz = (plan.src_h << 16) | plan.src_w[p];
		uint32_t dst_sz = (plan.dst_h << 16) | plan.dst_w[p];

		flip_wr(b + FLIP_SSPP_SRC_SIZE, src_sz);
		flip_wr(b + FLIP_SSPP_SRC_XY, plan.src_x[p]);
		flip_wr(b + FLIP_SSPP_OUT_SIZE, dst_sz);
		flip_wr(b + FLIP_SSPP_OUT_XY, plan.dst_x[p]);
		flip_wr(b + FLIP_SSPP_YSTRIDE0, plan.stride);
		flip_wr(b + FLIP_SSPP_SRC_FORMAT, plan.src_format);
		flip_wr(b + FLIP_SSPP_SRC_UNPACK_PATTERN, plan.unpack_pattern);
		flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C0_LR, 0);
		flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C0_TB, 0);
		flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C0_REQ, src_sz);
		flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C1C2_LR, 0);
		flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C1C2_TB, 0);
		flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C1C2_REQ, src_sz);
		flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C3_LR, 0);
		flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C3_TB, 0);
		flip_wr(b + FLIP_SSPP_SW_PIX_EXT_C3_REQ, src_sz);
		if (plan.scale_enable) {
			flip_wr(q + FLIP_QSEED3_PHASE_INIT_Y_H, plan.init_phase_x[p]);
			flip_wr(q + FLIP_QSEED3_PHASE_INIT_Y_V, plan.init_phase_y);
			flip_wr(q + FLIP_QSEED3_PHASE_INIT_UV_H, plan.init_phase_x[p]);
			flip_wr(q + FLIP_QSEED3_PHASE_INIT_UV_V, plan.init_phase_y);
			flip_wr(q + FLIP_QSEED3_PHASE_STEP_Y_H, plan.phase_step_h);
			flip_wr(q + FLIP_QSEED3_PHASE_STEP_Y_V, plan.phase_step_v);
			flip_wr(q + FLIP_QSEED3_PHASE_STEP_UV_H, plan.phase_step_h);
			flip_wr(q + FLIP_QSEED3_PHASE_STEP_UV_V, plan.phase_step_v);
			flip_wr(q + FLIP_QSEED3_PRELOAD, plan.preload);
			flip_wr(q + FLIP_QSEED3_SRC_SIZE_Y_RGB_A, src_sz);
			flip_wr(q + FLIP_QSEED3_SRC_SIZE_UV, src_sz);
			flip_wr(q + FLIP_QSEED3_DST_SIZE, dst_sz);
		}
		flip_wr(q + FLIP_QSEED3_OP_MODE, plan.qseed_op_mode);
	}
	dpuScale.active = true;
	dpuScale.src_w = src_w;
	dpuScale.src_h = src_h;
	dpuScale.stride = stride;
	dpuScale.fmt = fmt;
	return true;
}

static void
flip_restore_boot_scale(void)
{
	if (!dpuScale.active) {
		return;
	}
	dpu_pipe_restore(0, &dpuScale.boot[0]);
	dpu_pipe_restore(1, &dpuScale.boot[1]);
	dpuScale.active = false;
	dpuScale.src_w = dpuScale.src_h = dpuScale.stride = dpuScale.fmt = 0;
}

static bool
flip_dpu_scale_draw(uint8_t * dst_buf, const uint8_t * pixels, uint32_t surf_w, uint32_t surf_h,
    uint32_t stride, uint32_t fmt, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
    int32_t px0, int32_t py0, int32_t px1, int32_t py1, uint32_t panel_w, uint32_t panel_h)
{
	if (gDpuScale == 0 || !flip.ready || !dpuScale.capable || dst_buf == NULL || pixels == NULL) {
		return false;
	}
	if (px0 != 0 || py0 != 0 || px1 != (int32_t) panel_w || py1 != (int32_t) panel_h) {
		return false;
	}
	if (sw <= 0 || sh <= 0 || sx < 0 || sy < 0 ||
	    (uint32_t) (sx + sw) > surf_w || (uint32_t) (sy + sh) > surf_h) {
		return false;
	}
	bool whole_surf = (sx == 0 && sy == 0 && (uint32_t) sw == surf_w);
	uint32_t copy_stride = whole_surf ? stride : (uint32_t) sw * 4u;

	if (!flip_config_scale((uint32_t) sw, (uint32_t) sh, copy_stride, fmt)) {
		return false;
	}
	if (whole_surf) {
		memcpy(dst_buf, pixels, (size_t) sh * (size_t) stride);
	} else {
		for (int32_t y = 0; y < sh; y++) {
			memcpy(dst_buf + (size_t) y * copy_stride,
			    pixels + (size_t) (sy + y) * stride + (size_t) sx * 4u, copy_stride);
		}
	}
	flip.band0 = 0;
	flip.band1 = (int32_t) flip.rows;
	flip.drew = true;
	dpuScale.frames++;
	return true;
}

static bool
flip_dpu_scale_quiesce(bool poweroff)
{
	if (!dpuScale.active) {
		return false;
	}
	if (poweroff) {
		flip_restore_boot_scale();
		bzero(flip.va[0], flip.bytes);
		bzero(flip.va[1], flip.bytes);
		flip_point(0);
		flip_taken(40000, false);
		return true;
	}
	uint32_t panel_w = dpuScale.dst_w[0] + dpuScale.dst_w[1];
	uint32_t panel_h = dpuScale.dst_h;
	uint32_t dst_stride = dpuScale.boot[0].ystride0;
	const uint8_t * src_buf = flip.va[flip.shown];
	uint8_t * tmp_buf = flip.va[flip.shown ^ 1];
	bool is_l10r = dpuScale.fmt == FLIP_FOURCC_L10R;

	if (src_buf != NULL && tmp_buf != NULL && dst_stride >= panel_w * 4u &&
	    (uint64_t) panel_h * dst_stride <= flip.bytes) {
		for (uint32_t y = 0; y < panel_h; y++) {
			uint32_t sy = (uint32_t) ((uint64_t) y * dpuScale.src_h / panel_h);
			const uint32_t * srow = (const uint32_t *) (src_buf + (size_t) sy * dpuScale.stride);
			uint32_t * drow = (uint32_t *) (tmp_buf + (size_t) y * dst_stride);

			for (uint32_t x = 0; x < panel_w; x++) {
				uint32_t sx = (uint32_t) ((uint64_t) x * dpuScale.src_w / panel_w);
				drow[x] = is_l10r ? dpu_scale_l10r_to_bgra(srow[sx]) : srow[sx];
			}
		}
		if (flip.shown == 0) {
			memcpy(flip.va[0], tmp_buf, flip.bytes);
		}
	}
	flip_restore_boot_scale();
	flip_point(0);
	if (!flip_taken(40000, false)) {
#ifndef FB_DPU_SCALE_HOST_TEST
		IOLog("MiniIOMobileFramebuffer: boot framebuffer flush still pending at handoff\n");
#endif
	}
	return true;
}

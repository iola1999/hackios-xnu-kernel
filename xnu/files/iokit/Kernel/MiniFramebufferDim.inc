/*
 * The picture darkened by the display controller, for the brightness iOS asks for while the panel's
 * own brightness stays what the loader set (MiniAppleARMBacklight.cpp turns iOS's values into a level).
 *
 * Boot-arg fb_dim=1: each layer mixer the panel's CTL uses blends the one stage that holds the picture
 * with constant alphas: the picture's alpha follows the level, the border colour's is none, and so the
 * mixer's output is the picture times that alpha. At full level, and when the kernel returns to the
 * loader, the stage gets back what the loader had in it. A mixer laid out otherwise (the picture as its
 * background, or more than one stage with a pipe) is left alone and the picture is not darkened.
 * Without fb_dim the mixers are not touched.
 *
 * The layout is DPU 12's, as mainline Linux has it (drivers/gpu/drm/msm/disp/dpu1): dpu_hw_lm.c for the
 * v12 registers (LM_BG_SRC_SEL_V12 0x14; per stage LM_BLEND0_OP 0,
 * LM_BLEND0_FG_SRC_SEL_V12 4, LM_BLEND0_CONST_ALPHA_V12 8 with the foreground's 10-bit alpha in bits
 * 25:16 and the background's in 9:0), catalog/dpu_hw_catalog.c's sm8750_lm_sblk for where the 11
 * stages are (0x40 + 0x30 n), dpu_hw_mdss.h for the blend operations, dpu_hw_ctl.c for CTL_LAYER_ACTIVE
 * 0x130 and the mixers' bits in CTL_FLUSH. The mixers are at FLIP_LM(n), as in catalog/dpu_12_0_sm8750.h.
 * On the K90 (kernel-12377-rate11-w3) the loader leaves ctl0 driving INTF_1 with lm0 and lm1 active, each
 * with the picture in stage 5 (src 0x4140, op 0x400, alpha 0xffff0000). Included by MiniFramebufferFlip.inc.
 */

static TUNABLE(uint32_t, gDim, "fb_dim", 0);

#define DIM_LMS                 8u
#define DIM_LM_BG_SRC_SEL       0x14
#define DIM_LM_STAGES           11u
#define DIM_LM_STAGE(s)         (0x40u + 0x30u * (s))
#define DIM_STAGE_OP            0x00
#define DIM_STAGE_SRC_SEL       0x04
#define DIM_STAGE_CONST_ALPHA   0x08
#define DIM_CTL_LAYER_ACTIVE    0x130
#define DIM_OP_BLEND            0x3f3fu         /* DPU_BLEND_FG_* (bits 5:0) and DPU_BLEND_BG_* (13:8) */
#define DIM_OP_CONSTANT         0x100u          /* DPU_BLEND_FG_ALPHA_FG_CONST | DPU_BLEND_BG_ALPHA_BG_CONST */
#define DIM_ALPHA_FULL          0x3ffu

static const uint32_t dimFlush[DIM_LMS] = { 1u << 6, 1u << 7, 1u << 8, 1u << 9, 1u << 10, 1u << 20, 1u << 21, 1u << 27 };

static struct {
	bool            ready, dimmed, tried;   /* the mixers found; darkened now; written once */
	uint32_t        ctl, count;
	uint32_t        flush;                  /* the mixers' bits in CTL_FLUSH */
	uint32_t        lm[DIM_LMS];
	uint32_t        stage[DIM_LMS];         /* each mixer's picture stage, from the start of MDSS */
	uint32_t        op[DIM_LMS], alpha[DIM_LMS];    /* ... and what the loader had in it */
	uint32_t        late;                   /* writes that found the one before not yet taken */
} dim;

/* A pipe in either byte of a source selector (the pipe's type in bits 7:6, 3 for none: reset value 0xc0c0) */
static inline bool
dim_has_pipe(uint32_t sel)
{
	return (sel & 0xc0) != 0xc0 || (sel >> 8 & 0xc0) != 0xc0;
}

/* The largest r with r * r <= n. */
static uint32_t
dim_sqrt(uint32_t n)
{
	uint32_t r = 0;

	for (uint32_t bit = 1u << 30; bit != 0; bit >>= 2) {
		if (n >= r + bit) {
			n -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
	}
	return r;
}

/* fb_dim: the mixers of the CTL that drives the panel, each with the stage its picture is in. */
static void
dim_find(void)
{
	uint32_t active;

	for (dim.ctl = 0; dim.ctl < 6 && !(flip_rd(FLIP_CTL(dim.ctl) + FLIP_CTL_INTF_ACTIVE) & 0x6); dim.ctl++) {
	}
	active = dim.ctl < 6 ? flip_rd(FLIP_CTL(dim.ctl) + DIM_CTL_LAYER_ACTIVE) & 0xff : 0;
	if (active == 0) {
		IOLog("MiniIOMobileFramebuffer: dim: %s: the picture is not darkened\n",
		    dim.ctl < 6 ? "the panel's CTL has no layer mixer active" : "no CTL drives a DSI interface");
		return;
	}
	for (unsigned n = 0; n < DIM_LMS; n++) {
		uint32_t m = FLIP_LM(n), staged = 0, stage = 0;

		if (!(active & (1u << n))) {
			continue;
		}
		for (unsigned s = 0; s < DIM_LM_STAGES; s++) {
			if (dim_has_pipe(flip_rd(m + DIM_LM_STAGE(s) + DIM_STAGE_SRC_SEL))) {
				staged++;
				stage = s;
			}
		}
		if (dim_has_pipe(flip_rd(m + DIM_LM_BG_SRC_SEL)) || staged != 1) {
			IOLog("MiniIOMobileFramebuffer: dim: lm%u: background %08x and %u stages with a pipe, not one picture over "
			    "the border colour: the picture is not darkened\n", n, flip_rd(m + DIM_LM_BG_SRC_SEL), staged);
			dim.count = 0;
			dim.flush = 0;
			return;
		}
		dim.lm[dim.count] = n;
		dim.stage[dim.count] = m + DIM_LM_STAGE(stage);
		dim.op[dim.count] = flip_rd(dim.stage[dim.count] + DIM_STAGE_OP);
		dim.alpha[dim.count] = flip_rd(dim.stage[dim.count] + DIM_STAGE_CONST_ALPHA);
		if ((dim.alpha[dim.count] >> 16 & DIM_ALPHA_FULL) == 0) {
			dim.op[dim.count] = (dim.op[dim.count] & ~DIM_OP_CONSTANT) | DIM_OP_BLEND;
			dim.alpha[dim.count] = 0xffff0000u;
			flip_wr(dim.stage[dim.count] + DIM_STAGE_CONST_ALPHA, dim.alpha[dim.count]);
			flip_wr(dim.stage[dim.count] + DIM_STAGE_OP, dim.op[dim.count]);
		}
		dim.flush |= dimFlush[n];
		IOLog("MiniIOMobileFramebuffer: dim: lm%u: the picture in stage %u (src %08x), op %08x alpha %08x\n", n, stage,
		    flip_rd(dim.stage[dim.count] + DIM_STAGE_SRC_SEL), dim.op[dim.count], dim.alpha[dim.count]);
		dim.count++;
	}
	flip_wr(FLIP_CTL(dim.ctl) + FLIP_CTL_FLUSH, dim.flush | FLIP_CTL_FLUSH_CTL);
	dim.ready = true;
	IOLog("MiniIOMobileFramebuffer: dim: ctl%u, %u mixers: brightness below full darkens the picture (flush %#x)\n",
	    dim.ctl, dim.count, dim.flush | FLIP_CTL_FLUSH_CTL);
}


/* Until CTL has taken the mixers' flush (at the next refresh), LIMIT_US at most: how long it took, or -1. */
static int32_t
dim_taken(uint32_t limit_us, bool may_sleep)
{
	for (uint32_t waited = 0; waited <= limit_us; waited += may_sleep ? 1000 : 50) {
		if ((flip_rd(FLIP_CTL(dim.ctl) + FLIP_CTL_FLUSH) & dim.flush) == 0) {
			return (int32_t) waited;
		}
		if (may_sleep) {
			IOSleep(1);
		} else {
			delay(50);
		}
	}
	return -1;
}

static void
dim_write(uint32_t alpha)
{
	bool full = alpha >= DIM_ALPHA_FULL;

	for (unsigned i = 0; i < dim.count; i++) {
		flip_wr(dim.stage[i] + DIM_STAGE_CONST_ALPHA, full ? dim.alpha[i] : alpha << 16);
		flip_wr(dim.stage[i] + DIM_STAGE_OP, full ? dim.op[i] : (dim.op[i] & ~DIM_OP_BLEND) | DIM_OP_CONSTANT);
	}
	flip_wr(FLIP_CTL(dim.ctl) + FLIP_CTL_FLUSH, dim.flush | FLIP_CTL_FLUSH_CTL);
	dim.dimmed = !full;
}

/*
 * LEVEL is the fraction of the panel's brightness to show, in 1024ths; the mixer scales pixel values, which
 * the panel shows to the power of about 2.2 (inferred, sRGB's), so the alpha is the square root of the level
 * (2.0: near enough, and no table). Nothing with fb_dim off or no mixers found. Callers one at a time
 * (MiniAppleARMBacklight.cpp's lock).
 */
void
MiniIOMobileFramebufferDim(uint32_t level)
{
	uint32_t alpha = dim_sqrt((level < 1024 ? level : 1024) << 10);
	int32_t took;

	if (!dim.ready) {
		return;
	}
	alpha = alpha < DIM_ALPHA_FULL ? alpha : DIM_ALPHA_FULL;
	if ((flip_rd(FLIP_CTL(dim.ctl) + FLIP_CTL_FLUSH) & dim.flush) != 0) {
		dim.late++;
		if ((dim.late & (dim.late - 1)) == 0) {
			IOLog("MiniIOMobileFramebuffer: dim: %u writes found the one before not yet taken (flush %08x)\n", dim.late,
			    flip_rd(FLIP_CTL(dim.ctl) + FLIP_CTL_FLUSH));
		}
	}
	dim_write(alpha);
	if (!dim.tried) {
		dim.tried = true;
		took = dim_taken(50000, true);
		IOLog("MiniIOMobileFramebuffer: dim: the first level, alpha %u of 1023, %s %d ms; lm%u's stage now op %08x "
		    "alpha %08x\n", alpha, took >= 0 ? "taken after" : "not taken in", took >= 0 ? took / 1000 : 50, dim.lm[0],
		    flip_rd(dim.stage[0] + DIM_STAGE_OP), flip_rd(dim.stage[0] + DIM_STAGE_CONST_ALPHA));
	}
}

/* The picture alpha lm0's stage holds now (of 1023), or -1 with fb_dim off or no mixers found. */
int
MiniIOMobileFramebufferDimShown(void)
{
	return dim.ready ? (int) (flip_rd(dim.stage[0] + DIM_STAGE_CONST_ALPHA) >> 16 & DIM_ALPHA_FULL) : -1;
}

/* The loader's blending back before the kernel returns to it (MiniIOMobileFramebufferQuiesce: nothing sleeps). */
static void
dim_quiesce(void)
{
	if (!dim.ready || !dim.dimmed) {
		return;
	}
	dim_taken(40000, false);
	dim_write(DIM_ALPHA_FULL);
	dim_taken(40000, false);
}

/* Darken the panel to black on OS shutdown and keep dim_quiesce() from restoring full alpha on loader_return(). */
static void
dim_poweroff(void)
{
	if (!dim.ready) {
		return;
	}
	dim_taken(40000, false);
	dim_write(0);
	dim_taken(40000, false);
	dim.ready = false;
}

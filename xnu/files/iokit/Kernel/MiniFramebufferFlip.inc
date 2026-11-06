/*
 * The display controller showing frames from two buffers in turn (boot-arg fb_flip). Without it the
 * presenter scales every frame into the boot framebuffer while the controller reads the same memory
 * out to the panel, with nothing between the two, so a frame that changes much (a page swipe, an
 * application opening) can show torn. With fb_flip=2 the boot framebuffer (A) and a buffer of the
 * same layout placed after it in the continuous-splash region (B) take turns: a frame is drawn into
 * the one not on screen, the source pipes are pointed at it, and the controller takes the new
 * address at the start of a refresh. The loader's screenshots read A, so while B is on screen they
 * show the frame before it, or one being drawn.
 *
 * fb_flip=1 only looks: the register clocks turned on where they are off, then what the loader left
 * running (CTL paths, source pipes, layer mixers, interfaces) and how far the interfaces' frame
 * counters move in 200 ms, in "MiniIOMobileFramebuffer: flip:" lines. fb_flip=0 (the default) leaves
 * the display controller alone, unless fb_dim asks for its layer mixers (MiniFramebufferDim.inc) or
 * fb_dcs for the register clocks and INTF1's frame count (MiniFramebufferDCS.inc).
 *
 * SM8750's MDSS (0x0ae00000) has a DPU 12.0; the offsets are mainline Linux's (drivers/gpu/drm/msm/
 * disp/dpu1, dpu_hw_sspp.c, dpu_hw_ctl.c, dpu_hw_intf.c), counted from the MDP block at MDSS + 0x1000.
 * On the Xiaomi 15 the display could fetch from the splash region only, which is why B is there.
 * Ported from hakios-arm64e-dada's bsd/dev/hakios_dada_dpu.c (APSL 2.0). Included by
 * MiniIOMobileFramebuffer.cpp.
 */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern "C" boolean_t pmap_valid_address(uint64_t pa);

static TUNABLE(uint32_t, gFlip, "fb_flip", 0);

#define FLIP_MDSS_PHYS          0x0ae00000UL
#define FLIP_MDSS_SIZE          0x94000UL
#define FLIP_MDP                0x1000u
#define FLIP_CTL(n)             (FLIP_MDP + 0x15000u + 0x1000u * (n))
#define FLIP_CTL_LAYER(n)       (4u * (n))
#define FLIP_CTL_TOP            0x014
#define FLIP_CTL_FLUSH          0x018
#define FLIP_CTL_START          0x01c
#define FLIP_CTL_INTF_ACTIVE    0x0f4
#define FLIP_CTL_FETCH_ACTIVE   0x0fc
#define FLIP_CTL_FLUSH_CTL      (1u << 17)
#define FLIP_LM(n)              (FLIP_MDP + 0x44000u + 0x1000u * (n))
#define FLIP_INTF(n)            (FLIP_MDP + 0x34000u + 0x1000u * (n))
#define FLIP_SSPP_SRC_SIZE      0x00
#define FLIP_SSPP_SRC_XY        0x08
#define FLIP_SSPP_OUT_SIZE      0x0c
#define FLIP_SSPP_OUT_XY        0x10
#define FLIP_SSPP_SRC0_ADDR     0x14
#define FLIP_SSPP_YSTRIDE0      0x24
#define FLIP_SSPP_SRC_FORMAT    0x30
#define FLIP_SSPP_OP_MODE       0x38
#define FLIP_SPLASH_PHYS        0xfc800000UL    /* DT splash_region, 43 MiB */
#define FLIP_SPLASH_SIZE        0x2b00000UL
#define FLIP_PIPES              10u

static const struct {
	const char *    name;
	uint32_t        base;   /* from FLIP_MDP */
	uint32_t        flush;  /* its bit in CTL_FLUSH */
} flipPipe[FLIP_PIPES] = {
	{ "vig0", 0x4000, 1u << 0 }, { "vig1", 0x6000, 1u << 1 }, { "vig2", 0x8000, 1u << 2 }, { "vig3", 0xa000, 1u << 18 },
	{ "dma0", 0x24000, 1u << 11 }, { "dma1", 0x26000, 1u << 12 }, { "dma2", 0x28000, 1u << 24 },
	{ "dma3", 0x2a000, 1u << 25 }, { "dma4", 0x2c000, 1u << 13 }, { "dma5", 0x2e000, 1u << 14 },
};

static struct {
	vm_offset_t     mdss;
	uint32_t        ctl, pipes, flush;
	uint32_t        offset[FLIP_PIPES];     /* where each pipe reads, from the start of a buffer */
	uint64_t        pa[2];                  /* A, the boot framebuffer, and B */
	uint8_t *       va[2];
	uint32_t        bytes, rows;
	int             shown;                  /* the buffer the pipes were last pointed at */
	bool            ready, needStart, pending, drew;
	int32_t         band0, band1;           /* the panel rows this frame changed */
	int32_t         stale0, stale1;         /* the rows the buffer off screen lacks: what the last flip changed */
	uint64_t        flips, late;
} flip;

static inline uint32_t
flip_rd(uint32_t off)
{
	return *(volatile uint32_t *) (flip.mdss + off);
}

static inline void
flip_wr(uint32_t off, uint32_t value)
{
	*(volatile uint32_t *) (flip.mdss + off) = value;
	__builtin_arm_dsb(0xf);
}

/*
 * The register clocks, as Linux's dispcc-sm8750.c and gcc-sm8750.c name them. With them off a read
 * of the display controller never completes and the board resets (hakios-arm64e-dada, runs scan1
 * and scan2). A branch that is off is turned on; under hardware control (bit 1) it reads off while
 * nothing asks for it and is on when something does.
 */
static bool
flip_branch_on(vm_offset_t cbcr, const char * name)
{
	uint32_t was = *(volatile uint32_t *) cbcr;

	if (!(was & 1)) {
		*(volatile uint32_t *) cbcr = was | 1;
		__builtin_arm_dsb(0xf);
	}
	for (int i = 0; i < 200 && (*(volatile uint32_t *) cbcr >> 31); i++) {
		delay(5);
	}
	IOLog("MiniIOMobileFramebuffer: flip: %s %08x -> %08x\n", name, was, *(volatile uint32_t *) cbcr);
	return !(*(volatile uint32_t *) cbcr >> 31) || (*(volatile uint32_t *) cbcr & 2);
}

static bool
flip_clocks(void)
{
	static const struct {
		uint32_t        off;
		const char *    name;
		bool            needed;
	} branches[] = {
		{ 0x80b0, "disp_cc_mdss_ahb_clk", true }, { 0xc004, "disp_cc_mdss_non_gdsc_ahb_clk", false },
		{ 0xa028, "disp_cc_mdss_ahb1_clk", false }, { 0x8010, "disp_cc_mdss_mdp_clk", true },
		{ 0x8030, "disp_cc_mdss_vsync_clk", false },
	};
	vm_offset_t gcc = ml_io_map(0x127000UL, 0x1000), dispcc;
	uint32_t gdsc;

	if (gcc == 0 || !flip_branch_on(gcc + 0x004, "gcc_disp_ahb_clk")) {
		return false;
	}
	dispcc = ml_io_map(0x0af00000UL, 0x10000);
	if (dispcc == 0) {
		return false;
	}
	gdsc = *(volatile uint32_t *) (dispcc + 0x9000);
	IOLog("MiniIOMobileFramebuffer: flip: mdss_gdsc %08x (%s)\n", gdsc, (gdsc >> 31) ? "on" : "off");
	if (!(gdsc >> 31)) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		if (!flip_branch_on(dispcc + branches[i].off, branches[i].name) && branches[i].needed) {
			return false;
		}
	}
	return true;
}

/* What the loader left running, and how far the interfaces count in 200 ms (a refresh rate). */
static void
flip_look(uint64_t fb)
{
	uint32_t frames[3], reads[3];

	IOLog("MiniIOMobileFramebuffer: flip: MDSS %08x, DPU %08x; the boot framebuffer at %#llx\n", flip_rd(0),
	    flip_rd(FLIP_MDP), fb);
	for (unsigned n = 0; n < 6; n++) {
		uint32_t c = FLIP_CTL(n);

		if (flip_rd(c + FLIP_CTL_INTF_ACTIVE) == 0 && flip_rd(c + FLIP_CTL_FETCH_ACTIVE) == 0 &&
		    flip_rd(c + FLIP_CTL_LAYER(0)) == 0) {
			continue;
		}
		IOLog("MiniIOMobileFramebuffer: flip: ctl%u: layers %08x %08x top %08x flush %08x start %08x intf %08x "
		    "fetch %08x\n", n, flip_rd(c + FLIP_CTL_LAYER(0)), flip_rd(c + FLIP_CTL_LAYER(1)), flip_rd(c + FLIP_CTL_TOP),
		    flip_rd(c + FLIP_CTL_FLUSH), flip_rd(c + FLIP_CTL_START), flip_rd(c + FLIP_CTL_INTF_ACTIVE),
		    flip_rd(c + FLIP_CTL_FETCH_ACTIVE));
	}
	for (unsigned p = 0; p < FLIP_PIPES; p++) {
		uint32_t b = FLIP_MDP + flipPipe[p].base;

		if (flip_rd(b + FLIP_SSPP_SRC0_ADDR) == 0 && flip_rd(b + FLIP_SSPP_SRC_SIZE) == 0) {
			continue;
		}
		IOLog("MiniIOMobileFramebuffer: flip: %s: src %08x at %08x, out %08x at %08x, addr %08x stride %08x "
		    "format %08x op %08x\n", flipPipe[p].name, flip_rd(b + FLIP_SSPP_SRC_SIZE), flip_rd(b + FLIP_SSPP_SRC_XY),
		    flip_rd(b + FLIP_SSPP_OUT_SIZE), flip_rd(b + FLIP_SSPP_OUT_XY), flip_rd(b + FLIP_SSPP_SRC0_ADDR),
		    flip_rd(b + FLIP_SSPP_YSTRIDE0), flip_rd(b + FLIP_SSPP_SRC_FORMAT), flip_rd(b + FLIP_SSPP_OP_MODE));
	}
	for (unsigned n = 0; n < 4; n++) {
		uint32_t m = FLIP_LM(n);

		if (flip_rd(m) != 0 || flip_rd(m + 4) != 0) {
			IOLog("MiniIOMobileFramebuffer: flip: lm%u: op %08x out %08x\n", n, flip_rd(m), flip_rd(m + 4));
		}
	}
	/*
	 * INTF_FRAME_COUNT counts the timing engine's frames (video mode), INTF_TEAR_INT_COUNT_VAL the read
	 * pointer's (command mode). Only INTF1 and INTF2, the DSI ones, as the Xiaomi 15's look did.
	 */
	for (unsigned n = 1; n <= 2; n++) {
		frames[n] = flip_rd(FLIP_INTF(n) + 0xac);
		reads[n] = flip_rd(FLIP_INTF(n) + 0x298);
	}
	IOSleep(200);
	for (unsigned n = 1; n <= 2; n++) {
		uint32_t i = FLIP_INTF(n), r = flip_rd(i + 0x298);

		IOLog("MiniIOMobileFramebuffer: flip: intf%u: status %08x engine %08x config %08x tear check %08x autorefresh "
		    "%08x write line %u; frames %u -> %u, read pointer frame %u line %u -> frame %u line %u in 200 ms\n", n,
		    flip_rd(i + 0x26c), flip_rd(i + 0x0), flip_rd(i + 0x4), flip_rd(i + 0x284), flip_rd(i + 0x2b4),
		    flip_rd(i + 0x2b0), frames[n], flip_rd(i + 0xac), reads[n] >> 16, reads[n] & 0xffff, r >> 16, r & 0xffff);
	}
}

/* Until CTL has taken the last flush (its bits clear at the refresh that latches them), LIMIT_US at most. */
static bool
flip_taken(uint32_t limit_us, bool may_sleep)
{
	for (uint32_t waited = 0; waited < limit_us; waited += may_sleep && waited >= 200 ? 1000 : 50) {
		if ((flip_rd(FLIP_CTL(flip.ctl) + FLIP_CTL_FLUSH) & flip.flush) == 0) {
			return true;
		}
		if (may_sleep && waited >= 200) {
			IOSleep(1);
		} else {
			delay(50);
		}
	}
	return (flip_rd(FLIP_CTL(flip.ctl) + FLIP_CTL_FLUSH) & flip.flush) == 0;
}

/* The pipes onto buffer WHICH, taken at the next refresh. */
static void
flip_point(int which)
{
	__builtin_arm_dsb(0xf);         /* (the frame's writes out of the write buffers before the display reads them) */
	for (unsigned p = 0; p < FLIP_PIPES; p++) {
		if (flip.pipes & (1u << p)) {
			flip_wr(FLIP_MDP + flipPipe[p].base + FLIP_SSPP_SRC0_ADDR, (uint32_t) (flip.pa[which] + flip.offset[p]));
		}
	}
	flip_wr(FLIP_CTL(flip.ctl) + FLIP_CTL_FLUSH, flip.flush);
	if (flip.needStart) {
		flip_wr(FLIP_CTL(flip.ctl) + FLIP_CTL_START, 1);
	}
	flip.shown = which;
}

#include "MiniFramebufferDim.inc"
#include "MiniFramebufferDCS.inc"

static void
flip_start(uint64_t fb, uint8_t * fbBase, uint32_t bytes, uint32_t rows)
{
	uint64_t b;

	if ((gFlip == 0 && gDim == 0 && gDcs == 0) || !flip_clocks() ||
	    (flip.mdss = ml_io_map(FLIP_MDSS_PHYS, FLIP_MDSS_SIZE)) == 0) {
		return;
	}
	if (gDim) {
		dim_find();
	}
	dcs_start();
	if (gFlip == 0) {
		return;
	}
	flip_look(fb);
	b = (fb + bytes + 0xfffff) & ~0xfffffULL;
	if (gFlip < 2 || fb < FLIP_SPLASH_PHYS || b + bytes > FLIP_SPLASH_PHYS + FLIP_SPLASH_SIZE) {
		return;
	}
	/* the pipes that read A, or B where a kernel before this one left them */
	for (unsigned p = 0; p < FLIP_PIPES; p++) {
		uint32_t at = flip_rd(FLIP_MDP + flipPipe[p].base + FLIP_SSPP_SRC0_ADDR);

		if (flip_rd(FLIP_MDP + flipPipe[p].base + FLIP_SSPP_SRC_SIZE) != 0 && at != 0 &&
		    ((at >= fb && at < fb + bytes) || (at >= b && at < b + bytes))) {
			flip.pipes |= 1u << p;
			flip.flush |= flipPipe[p].flush;
			flip.offset[p] = (uint32_t) (at - (at >= b ? b : fb));
		}
	}
	for (flip.ctl = 0; flip.ctl < 6 && !(flip_rd(FLIP_CTL(flip.ctl) + FLIP_CTL_INTF_ACTIVE) & 0x6); flip.ctl++) {
	}
	if (flip.pipes == 0 || flip.ctl == 6) {
		IOLog("MiniIOMobileFramebuffer: flip: %s: the processor draws on the panel's memory\n",
		    flip.pipes == 0 ? "no pipe reads the boot framebuffer" : "no CTL drives a DSI interface");
		return;
	}
	for (uint64_t at = b; at < b + bytes; at += 0x100000) {
		if (pmap_valid_address(at)) {
			IOLog("MiniIOMobileFramebuffer: flip: %#llx is the kernel's memory: no second buffer\n", at);
			return;
		}
	}
	flip.flush |= FLIP_CTL_FLUSH_CTL;
	flip.pa[0] = fb;
	flip.pa[1] = b;
	flip.va[0] = fbBase;
	flip.va[1] = (uint8_t *) ml_io_map_wcomb((vm_offset_t) b, bytes);
	flip.bytes = bytes;
	flip.rows = rows;
	if (flip.va[1] == NULL) {
		return;
	}
	/* A on screen, as the loader had it; does a flush alone get taken, or does each refresh need a start? */
	flip_point(0);
	if (!flip_taken(50000, true)) {
		flip.needStart = true;
		flip_point(0);
		if (!flip_taken(50000, true)) {
			IOLog("MiniIOMobileFramebuffer: flip: CTL does not take a flush (flush %08x)\n",
			    flip_rd(FLIP_CTL(flip.ctl) + FLIP_CTL_FLUSH));
			return;
		}
	}
	flip.stale0 = 0;
	flip.stale1 = (int32_t) rows;
	flip.band0 = INT32_MAX;
	flip.band1 = INT32_MIN;
	flip.ready = true;
	IOLog("MiniIOMobileFramebuffer: flip: ctl%u, pipes %#x (flush %#x%s); frames flip between %#llx and %#llx\n",
	    flip.ctl, flip.pipes, flip.flush, flip.needStart ? ", a start each" : "", fb, b);
}

/* Where the next frame is drawn: the buffer off screen, once the last flip has been taken. */
static uint8_t *
flip_target(uint8_t * base)
{
	if (!flip.ready) {
		return base;
	}
	if (flip.pending && !flip_taken(40000, true)) {
		flip.late++;
	}
	flip.pending = false;
	return flip.va[flip.shown ^ 1];
}

/* A band of rows drawn into the buffer off screen: widened by the rows that buffer missed. */
static void
flip_band(int32_t * by0, int32_t * by1, int32_t py0, int32_t py1)
{
	if (!flip.ready) {
		return;
	}
	flip.band0 = *by0 < flip.band0 ? *by0 : flip.band0;
	flip.band1 = *by1 > flip.band1 ? *by1 : flip.band1;
	*by0 = flip.stale0 < *by0 ? (flip.stale0 > py0 ? flip.stale0 : py0) : *by0;
	*by1 = flip.stale1 > *by1 ? (flip.stale1 < py1 ? flip.stale1 : py1) : *by1;
	flip.drew = true;
}

/* The whole panel drawn (cleared) into the buffer off screen. */
static void
flip_whole(void)
{
	int32_t y0 = 0, y1 = (int32_t) flip.rows;

	flip_band(&y0, &y1, y0, y1);
}

/* The frame drawn: the pipes onto it. A frame that drew nothing leaves the screen as it is. */
static void
flip_present(void)
{
	if (!flip.ready || !flip.drew) {
		return;
	}
	flip_point(flip.shown ^ 1);
	flip.pending = true;
	flip.stale0 = flip.band0;
	flip.stale1 = flip.band1;
	flip.band0 = INT32_MAX;
	flip.band1 = INT32_MIN;
	flip.drew = false;
	if (++flip.flips % 4096 == 1) {
		IOLog("MiniIOMobileFramebuffer: flip: %llu frames flipped, %llu flips not taken within 40 ms\n", flip.flips,
		    flip.late);
	}
}

/*
 * Before the kernel gives the machine back to the loader (loader_return): the picture on screen into
 * the boot framebuffer and the pipes onto it, and the mixers' blending as the loader had it (fb_dim),
 * for the loader's screen and screenshots and for the next kernel. Other processors are stopped by
 * then; nothing here sleeps.
 */
extern "C" void
MiniIOMobileFramebufferQuiesce(void)
{
	dim_quiesce();
	if (!flip.ready || flip.shown == 0) {
		return;
	}
	flip_taken(40000, false);
	memcpy(flip.va[0], flip.va[1], flip.bytes);
	flip_point(0);
	flip_taken(40000, false);
	flip.ready = false;
}

extern "C" void
MiniIOMobileFramebufferPowerOff(void)
{
	dim_poweroff();
	if (flip.ready) {
		flip_taken(40000, false);
		bzero(flip.va[0], flip.bytes);
		bzero(flip.va[1], flip.bytes);
		flip_point(0);
		flip_taken(40000, false);
		flip.ready = false;
	} else if (vinfo.v_baseaddr != 0 && vinfo.v_height != 0 && vinfo.v_rowbytes != 0) {
		bzero((void *) vinfo.v_baseaddr, (size_t) vinfo.v_rowbytes * vinfo.v_height);
	}
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size, in the buffer on screen.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;
	uint8_t * base = flip.ready ? flip.va[flip.shown] : (uint8_t *) vinfo.v_baseaddr;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy(base + (size_t) y * vinfo.v_rowbytes, pixels + (size_t) y * rowBytes, (size_t) columns * 4);
	}
}

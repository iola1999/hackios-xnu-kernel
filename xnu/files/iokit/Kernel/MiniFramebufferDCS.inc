/*
 * The panel's own brightness, DCS 0x51 over DSI0, for the brightness iOS asks for
 * (MiniAppleARMBacklight.cpp turns IOMFBBrightnessLevel into a panel value of 5 to 0x1a5e).
 *
 * Boot-arg fb_dcs, DCS_DEFAULT without it:
 *   1  read only: DSI0's registers as the loader left them, and INTF1's frame count over 100 ms
 *   2  1, then two DCS reads, 0x0A (power mode) and 0x52 (brightness); the panel's state is not changed
 *   3  2, then one write 51 03 5A, read back after 100 ms; when the panel already read 0x35a, also
 *      0x200 and 0x35a again, so that the read-back tells a write that took effect from one that did not
 *   4  2, then every value iOS asks for: at most one write per refresh (120 Hz), the last value always
 *      written; the first DCS_CHECKS times the requests stop for 300 ms the panel is read back
 * The first thing that is not as expected (DSI0 laid out otherwise, the controller busy or erroring,
 * no DMA_DONE, INTF1's frames stopped) ends the commands for the rest of the boot: a panel driver chip
 * that hangs comes back only with a restart. A read that gets no answer only ends the reads. Only
 * these packets are sent: 51 with two bytes, and for the reads Set Maximum Return Packet Size (0x37)
 * and a DCS read (0x06) of 0x52 or 0x0A.
 *
 * Packets go through the command FIFO of DSI's test pattern generator, so the command DMA reads no
 * memory and needs no display SMMU mapping; DMA_CMD_OFFSET (the boot loader's buffer) is only printed.
 * The sequence is the one of ShueiTsuki/data-a-live 0139-0141 (SM8850, Xiaomi command mode panel:
 * 0x51 took effect at the next frame, and 0x52 read it back, 1370 written, 0x055a read 100 ms later)
 * and of Shad0w23333/UniXNU-OnePlus13T's PeripheralIO.h (SM8750, DSI0 at the same address, its
 * offsets from Qualcomm's dsi_ctrl_hw_cmn.c), which also gives the interrupt enables and error masks.
 * Offsets marked "unchecked" have no other source here; fb_dcs=1 prints them before anything is sent.
 *   0x000 HW_VERSION       0x2009xxxx expected (dsi-ctrl-hw-v2.9; PeripheralIO.h's check)
 *   0x004 CTRL             bit 0 DSI_EN, bit 2 CMD_MODE_EN
 *   0x008 STATUS           bit 1 command DMA busy, bit 2 MDP stream busy (PeripheralIO.h)
 *   0x00c FIFO_STATUS      error bits 0xcccc0480
 *   0x03c COMMAND_MODE_DMA_CTRL  bit 28 embedded, bit 26 low power, bits 31:30 broadcast
 *   0x048 DMA_CMD_OFFSET   unchecked (downstream dsi_ctrl_reg.h, from memory); read only
 *   0x04c DMA_CMD_LENGTH   bytes of the packet, padding not counted
 *   0x068 ACK_ERR_STATUS   0x06c RDBK_DATA0 (DI, data0, data1, ECC from bit 31 down; data-a-live)
 *   0x07c RDBK_DATATYPE0, 0x1d4 RDBK_DATA_CTRL: unchecked names, printed only (data-a-live prints them)
 *   0x084 TRIG_CTRL        bits 3:0 DMA trigger, 4 = software; bit 12 BLOCK_DMA_WITHIN_FRAME unchecked
 *                          (mainline msm dsi.xml.h, from memory: mainline counts 6G offsets 4 lower)
 *   0x090 CMD_MODE_DMA_SW_TRIGGER
 *   0x0c0 TIMEOUT_STATUS   bit 0 HS TX, bit 4 LP RX, bit 8 BTA
 *   0x110 INT_CTRL         bit 0 DMA_DONE (write 1 to clear), bit 24 DSI_ERROR; enables 0xa222aa02
 *   0x15c TEST_PATTERN_GEN_CTRL 0x30006 while the FIFO holds a packet; 0x17c the FIFO; 0x1ec its reset
 *   0x390 name unknown: both ports clear bit 1 for a FIFO packet; put back afterwards
 * INTF1's INTF_FRAME_COUNT (0xac) is MiniFramebufferFlip.inc's; it went up 25 in 200 ms (bo53).
 * Included by MiniFramebufferFlip.inc, after MiniFramebufferDim.inc.
 */

#define DCS_DEFAULT             4u              /* fb_dcs without the boot-arg */

static TUNABLE(uint32_t, gDcs, "fb_dcs", DCS_DEFAULT);

#define DCS_DSI0_PHYS           0x0ae94000UL
#define DCS_DSI_SIZE            0x1000UL

#define DSI_HW_VERSION          0x000
#define DSI_CTRL                0x004
#define DSI_STATUS              0x008
#define DSI_FIFO_STATUS         0x00c
#define DSI_DMA_CTRL            0x03c
#define DSI_DMA_CMD_OFFSET      0x048
#define DSI_DMA_CMD_LENGTH      0x04c
#define DSI_ACK_ERR_STATUS      0x068
#define DSI_RDBK_DATA0          0x06c
#define DSI_RDBK_DATATYPE0      0x07c
#define DSI_TRIG_CTRL           0x084
#define DSI_DMA_SW_TRIGGER      0x090
#define DSI_LANE_STATUS         0x0a8
#define DSI_TIMEOUT_STATUS      0x0c0
#define DSI_INT_CTRL            0x110
#define DSI_CLK_CTRL            0x11c
#define DSI_TPG_CTRL            0x15c
#define DSI_TPG_FIFO            0x17c
#define DSI_RDBK_DATA_CTRL      0x1d4
#define DSI_TPG_FIFO_RESET      0x1ec
#define DSI_REG_390             0x390

#define DSI_CTRL_ON             0x5u
#define DSI_STATUS_DMA_BUSY     0x2u
#define DSI_FIFO_ERRORS         0xcccc0480u
#define DSI_DMA_CLEAR           0xc4000000u     /* broadcast, master, low power: off, so a single high speed packet */
#define DSI_DMA_EMBEDDED        (1u << 28)
#define DSI_TRIG_DMA_SW         0x4u
#define DSI_TRIG_BLOCK_IN_FRAME (1u << 12)
#define DSI_TIMEOUT_HS_TX       0x001u
#define DSI_TIMEOUT_READ        0x110u          /* LP RX, BTA */
#define DSI_INT_DMA_DONE        0x1u
#define DSI_INT_ERROR           (1u << 24)
#define DSI_INT_ENABLES         0xa222aa02u
#define DSI_TPG_CMD_FIFO        0x30006u
#define DSI_390_FIFO_CLEAR      0x2u
#define DSI_INTF_FRAME_COUNT    0xac

#define DCS_PANEL_MIN           5u              /* qcom,mdss-dsi-bl-min-level */
#define DCS_PANEL_MAX           0x1a5eu         /* mi,mdss-fac-brightness-max-level: above it taken as high brightness */
#define DCS_TEST_LEVEL          0x35au          /* qcom,mdss-brightness-init-level */
#define DCS_TEST_OTHER          0x200u
#define DCS_PERIOD_US           8333u           /* one refresh at 120 Hz (qcom,mdss-dsi-panel-framerate) */
#define DCS_IDLE_POLLS          200u            /* x 100 us */
#define DCS_DONE_SPINS          40u             /* x 50 us, then 1 ms sleeps up to DCS_DONE_MS */
#define DCS_DONE_MS             50u
#define DCS_SETTLE_MS           300u
#define DCS_CHECKS              8u
#define DCS_LOGGED_WRITES       16u

enum { DCS_OFF, DCS_START, DCS_READY, DCS_FAILED };      /* OFF also once the tests of fb_dcs 1 to 3 are done */

static struct {
	vm_offset_t     dsi;
	IOLock *        lock;           /* one sender; around everything below but want and sentAt */
	thread_call_t   call;           /* the start's checks, then the writes */
	thread_call_t   check;          /* the read-back after a burst */
	uint64_t        period;
	int             state;
	bool            canRead;
	uint32_t        sent;           /* the last value written, 0 none */
	uint32_t        want;           /* the last value asked for, 0 none (atomic) */
	uint64_t        sentAt;         /* (atomic) */
	uint32_t        writes, checks;
} dcs;

struct dcs_regs {
	uint32_t ctrl, status, fifo, dma, trig, ints, timeout, ack, tpg;
};

static inline uint32_t
dcs_rd(uint32_t off)
{
	return *(volatile uint32_t *) (dcs.dsi + off);
}

static inline void
dcs_wr(uint32_t off, uint32_t value)
{
	*(volatile uint32_t *) (dcs.dsi + off) = value;
	__builtin_arm_dsb(0xf);
}

static void
dcs_snap(struct dcs_regs * r)
{
	r->ctrl = dcs_rd(DSI_CTRL);
	r->status = dcs_rd(DSI_STATUS);
	r->fifo = dcs_rd(DSI_FIFO_STATUS);
	r->dma = dcs_rd(DSI_DMA_CTRL);
	r->trig = dcs_rd(DSI_TRIG_CTRL);
	r->ints = dcs_rd(DSI_INT_CTRL);
	r->timeout = dcs_rd(DSI_TIMEOUT_STATUS);
	r->ack = dcs_rd(DSI_ACK_ERR_STATUS);
	r->tpg = dcs_rd(DSI_TPG_CTRL);
}

/* What one packet left wrong, or NULL; the error bits count when new, as the loader may have left some. */
static const char *
dcs_problem(const struct dcs_regs * b, const struct dcs_regs * a)
{
	if (a->ctrl != b->ctrl || a->trig != b->trig || a->dma != b->dma) {
		return "CTRL, TRIG_CTRL or DMA_CTRL changed";
	}
	if (a->tpg != 0) {
		return "the test pattern generator left on";
	}
	if (a->status & DSI_STATUS_DMA_BUSY) {
		return "command DMA still busy";
	}
	if (a->timeout & ~b->timeout & DSI_TIMEOUT_HS_TX) {
		return "a new HS TX timeout";
	}
	if (a->fifo & ~b->fifo & DSI_FIFO_ERRORS) {
		return "a new FIFO error";
	}
	if (a->ints & ~b->ints & DSI_INT_ERROR) {
		return "a new DSI_ERROR";
	}
	return NULL;
}

static uint32_t
dcs_frames(void)
{
	return flip_rd(FLIP_INTF(1) + DSI_INTF_FRAME_COUNT);
}

/*
 * One packet of N words (1 or 2) through the FIFO, LENGTH bytes of it, with dcs.lock held: NULL once
 * DMA_DONE, else why not. The MDP's frames go on meanwhile; the controller puts the packet between
 * them (data-a-live sent the same way at every step of a fade). Without DMA_DONE nothing is put back:
 * the engine may still own the packet.
 */
static const char *
dcs_send(const uint32_t * words, unsigned n, uint32_t length)
{
	uint32_t dma, reg390, saved, enables;
	unsigned polls;

	if ((dcs_rd(DSI_CTRL) & DSI_CTRL_ON) != DSI_CTRL_ON) {
		return "DSI_EN or CMD_MODE_EN clear";
	}
	if (dcs_rd(DSI_TPG_CTRL) != 0) {
		return "the test pattern generator in use";
	}
	for (polls = 0; dcs_rd(DSI_STATUS) & DSI_STATUS_DMA_BUSY; polls++) {
		if (polls == DCS_IDLE_POLLS) {
			return "command DMA busy for 20 ms before the packet";
		}
		IODelay(100);
	}
	dma = dcs_rd(DSI_DMA_CTRL);
	reg390 = dcs_rd(DSI_REG_390);
	saved = dcs_rd(DSI_DMA_CMD_LENGTH);
	enables = dcs_rd(DSI_INT_CTRL) & DSI_INT_ENABLES;
	dcs_wr(DSI_INT_CTRL, enables | DSI_INT_DMA_DONE);
	dcs_wr(DSI_TPG_CTRL, DSI_TPG_CMD_FIFO);
	for (unsigned i = 0; i < n; i++) {
		dcs_wr(DSI_TPG_FIFO, words[i]);
	}
	if (n & 1) {
		dcs_wr(DSI_TPG_FIFO, 0);        /* the FIFO takes words in pairs */
	}
	dcs_wr(DSI_DMA_CTRL, (dma & ~DSI_DMA_CLEAR) | DSI_DMA_EMBEDDED);
	dcs_wr(DSI_REG_390, reg390 & ~DSI_390_FIFO_CLEAR);
	dcs_wr(DSI_DMA_CMD_LENGTH, length);
	dcs_wr(DSI_DMA_SW_TRIGGER, 1);
	/* up to a frame's transfer (7.3 ms, qcom,mdss-mdp-transfer-time-us) before the gap the packet goes in */
	for (polls = 0; !(dcs_rd(DSI_INT_CTRL) & DSI_INT_DMA_DONE); polls++) {
		if (polls >= DCS_DONE_SPINS + DCS_DONE_MS) {
			return "no DMA_DONE in 50 ms";
		}
		if (polls < DCS_DONE_SPINS) {
			IODelay(50);
		} else {
			IOSleep(1);
		}
	}
	dcs_wr(DSI_INT_CTRL, enables | DSI_INT_DMA_DONE);
	dcs_wr(DSI_TPG_CTRL, 0);
	dcs_wr(DSI_TPG_FIFO_RESET, 1);
	IODelay(1);
	dcs_wr(DSI_TPG_FIFO_RESET, 0);
	dcs_wr(DSI_DMA_CTRL, dma);
	dcs_wr(DSI_REG_390, reg390);
	dcs_wr(DSI_DMA_CMD_LENGTH, saved);
	return NULL;
}

static void
dcs_fail(const char * why)
{
	struct dcs_regs r;

	dcs_snap(&r);
	dcs.state = DCS_FAILED;
	IOLog("MiniIOMobileFramebuffer: dcs: %s: no DCS for the rest of the boot (ctrl %08x status %08x fifo %08x dma %08x "
	    "trig %08x int %08x timeout %08x ack %08x tpg %08x)\n", why, r.ctrl, r.status, r.fifo, r.dma, r.trig, r.ints,
	    r.timeout, r.ack, r.tpg);
}

/*
 * DCS read of CMD (0x0A or 0x52): Set Maximum Return Packet Size 2, then the read with a bus turnaround.
 * The panel's answer, a short read response, is in RDBK_DATA0. The value read, or -1: no answer ends the
 * reads, anything else wrong ends the commands.
 */
static int32_t
dcs_read(uint8_t cmd)
{
	static const uint32_t maxret = 0x80370002u;     /* last packet; 0x37; 2, 0 */
	uint32_t read = 0xa0060000u | cmd;              /* last packet, BTA; 0x06; CMD, 0 */
	uint32_t old = dcs_rd(DSI_RDBK_DATA0), d0, type;
	struct dcs_regs b, a;
	const char * why;
	int32_t value = -1;

	dcs_snap(&b);
	why = dcs_send(&maxret, 1, 4);
	if (why == NULL) {
		why = dcs_send(&read, 1, 4);
	}
	if (why != NULL) {
		dcs_fail(why);
		return -1;
	}
	IODelay(200);
	dcs_snap(&a);
	d0 = dcs_rd(DSI_RDBK_DATA0);
	type = d0 >> 24;
	if (type == 0x22) {
		value = (int32_t) (d0 >> 8 & 0xffff);
	} else if (type == 0x21) {
		value = (int32_t) (d0 >> 16 & 0xff);
	}
	IOLog("MiniIOMobileFramebuffer: dcs: read %02x: data0 %08x (was %08x) -> %d; datatype0 %08x rdbk ctrl %08x ack %08x->%08x "
	    "timeout %08x->%08x int %08x\n", cmd, d0, old, value, dcs_rd(DSI_RDBK_DATATYPE0), dcs_rd(DSI_RDBK_DATA_CTRL), b.ack,
	    a.ack, b.timeout, a.timeout, a.ints);
	why = dcs_problem(&b, &a);
	if (why != NULL) {
		dcs_fail(why);
		return -1;
	}
	if (value < 0 || (a.timeout & ~b.timeout & DSI_TIMEOUT_READ)) {
		dcs.canRead = false;
		IOLog("MiniIOMobileFramebuffer: dcs: read %02x got no short read response: no more reads this boot\n", cmd);
		return -1;
	}
	return value;
}

/* 51 hi lo (qcom,mdss-dsi-bl-inverted-dbv: high byte first), VALUE clamped to the panel's range. */
static bool
dcs_write(uint32_t value, bool log)
{
	uint32_t words[2];
	struct dcs_regs b, a;
	uint64_t began, us;
	const char * why;

	value = value < DCS_PANEL_MIN ? DCS_PANEL_MIN : value > DCS_PANEL_MAX ? DCS_PANEL_MAX : value;
	words[0] = 0xc0390003u;                         /* last, long packet; 0x39; 3 bytes */
	words[1] = 0xff000000u | (value & 0xff) << 16 | (value >> 8 & 0xff) << 8 | 0x51u;
	dcs_snap(&b);
	began = mach_absolute_time();
	why = dcs_send(words, 2, 8);
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &us);
	us /= NSEC_PER_USEC;
	dcs_snap(&a);
	if (why == NULL) {
		why = dcs_problem(&b, &a);
	}
	dcs.writes++;
	if (why != NULL || log) {
		IOLog("MiniIOMobileFramebuffer: dcs: write #%u 51 %02x %02x %s in %llu us; status %08x fifo %08x timeout %08x int %08x "
		    "ack %08x\n", dcs.writes, value >> 8 & 0xff, value & 0xff, why != NULL ? "FAILED" : "done", us, a.status, a.fifo,
		    a.timeout, a.ints, a.ack);
	}
	if (why != NULL) {
		dcs_fail(why);
		return false;
	}
	dcs.sent = value;
	__atomic_store_n(&dcs.sentAt, mach_absolute_time(), __ATOMIC_RELEASE);
	return true;
}

/* VALUE written, then after 100 ms of frames: did INTF1 count on, and does the panel read VALUE? */
static bool
dcs_write_checked(uint32_t value)
{
	uint32_t frames;
	int32_t read;

	if (!dcs_write(value, true)) {
		return false;
	}
	frames = dcs_frames();
	IOSleep(100);
	frames = dcs_frames() - frames;
	read = dcs.canRead ? dcs_read(0x52) : -1;
	IOLog("MiniIOMobileFramebuffer: dcs: after 51 %04x: intf1 frames +%u in 100 ms, panel reads %d (%s)\n", value, frames, read,
	    read < 0 ? "no read-back" : (uint32_t) read == value ? "the value written" : "NOT the value written");
	if (frames == 0) {
		dcs_fail("INTF1's frames stopped");
		return false;
	}
	return dcs.state != DCS_FAILED;
}

/* fb_dcs 1 and up, once: the controller as the loader left it; NULL when it is the one these packets are for. */
static const char *
dcs_look(void)
{
	uint32_t version = dcs_rd(DSI_HW_VERSION), ctrl = dcs_rd(DSI_CTRL), trig = dcs_rd(DSI_TRIG_CTRL);
	uint32_t tpg = dcs_rd(DSI_TPG_CTRL), frames = dcs_frames();

	IOLog("MiniIOMobileFramebuffer: dcs: DSI0 version %08x ctrl %08x status %08x fifo %08x lanes %08x clk %08x\n", version, ctrl,
	    dcs_rd(DSI_STATUS), dcs_rd(DSI_FIFO_STATUS), dcs_rd(DSI_LANE_STATUS), dcs_rd(DSI_CLK_CTRL));
	IOLog("MiniIOMobileFramebuffer: dcs: dma ctrl %08x offset %08x length %08x; trig %08x (dma trigger %u, block dma within "
	    "frame %u); reg390 %08x tpg %08x\n", dcs_rd(DSI_DMA_CTRL), dcs_rd(DSI_DMA_CMD_OFFSET), dcs_rd(DSI_DMA_CMD_LENGTH), trig,
	    trig & 0xf, (trig & DSI_TRIG_BLOCK_IN_FRAME) != 0, dcs_rd(DSI_REG_390), tpg);
	IOSleep(100);
	frames = dcs_frames() - frames;
	IOLog("MiniIOMobileFramebuffer: dcs: int %08x timeout %08x ack %08x rdbk ctrl %08x data0 %08x datatype0 %08x; intf1 "
	    "frames +%u in 100 ms\n", dcs_rd(DSI_INT_CTRL), dcs_rd(DSI_TIMEOUT_STATUS), dcs_rd(DSI_ACK_ERR_STATUS),
	    dcs_rd(DSI_RDBK_DATA_CTRL), dcs_rd(DSI_RDBK_DATA0), dcs_rd(DSI_RDBK_DATATYPE0), frames);
	if (version >> 16 != 0x2009) {
		return "DSI0 is not version 2.9";
	}
	if ((ctrl & DSI_CTRL_ON) != DSI_CTRL_ON) {
		return "DSI0 is not on in command mode";
	}
	if ((trig & 0xf) != DSI_TRIG_DMA_SW) {
		return "the command DMA trigger is not software";
	}
	if (tpg != 0) {
		return "the test pattern generator is in use";
	}
	if (frames == 0) {
		return "INTF1 counts no frames";
	}
	return NULL;
}

/* The start's checks, with dcs.lock held. */
static void
dcs_test(void)
{
	const char * why = dcs_look();
	int32_t level;
	uint32_t boot;

	if (why != NULL) {
		dcs_fail(why);
		return;
	}
	if (gDcs < 2) {
		IOLog("MiniIOMobileFramebuffer: dcs: DSI0 as expected; fb_dcs=1 sends nothing\n");
		dcs.state = DCS_OFF;
		return;
	}
	dcs.canRead = true;
	(void) dcs_read(0x0a);
	level = dcs.state != DCS_FAILED && dcs.canRead ? dcs_read(0x52) : -1;
	boot = level > 0 ? (uint32_t) level : 0;
	if (dcs.state == DCS_FAILED) {
		return;
	}
	IOLog("MiniIOMobileFramebuffer: dcs: the panel's brightness at the start %#x\n", boot);
	if (gDcs == 3) {
		if (dcs_write_checked(DCS_TEST_LEVEL) && boot == DCS_TEST_LEVEL && dcs_write_checked(DCS_TEST_OTHER)) {
			(void) dcs_write_checked(DCS_TEST_LEVEL);
		}
	}
	if (dcs.state == DCS_FAILED) {
		return;
	}
	dcs.state = gDcs >= 4 ? DCS_READY : DCS_OFF;
	if (gDcs >= 4) {
		dcs_write(0xae4, true);
	}
	IOLog("MiniIOMobileFramebuffer: dcs: %s\n", gDcs >= 4 ? "brightness goes to the panel" : "tests done, nothing more sent");
}

static void
dcs_run(thread_call_param_t, thread_call_param_t)
{
	uint64_t now, next, settle;
	uint32_t want;

	IOLockLock(dcs.lock);
	if (dcs.state == DCS_START) {
		dcs_test();
	}
	want = __atomic_load_n(&dcs.want, __ATOMIC_ACQUIRE);
	now = mach_absolute_time();
	next = __atomic_load_n(&dcs.sentAt, __ATOMIC_ACQUIRE) + dcs.period;
	if (dcs.state == DCS_READY && want != 0 && want != dcs.sent) {
		if (now < next) {
			thread_call_enter_delayed(dcs.call, next);
		} else if (dcs_write(want, dcs.writes < DCS_LOGGED_WRITES || dcs.writes % 256 == 0) && dcs.canRead &&
		    dcs.checks < DCS_CHECKS) {
			clock_interval_to_deadline(DCS_SETTLE_MS, kMillisecondScale, &settle);
			thread_call_enter_delayed(dcs.check, settle);
		}
	}
	IOLockUnlock(dcs.lock);
}

/* DCS_SETTLE_MS after the last write of a burst: does the panel read what was written? */
static void
dcs_check(thread_call_param_t, thread_call_param_t)
{
	int32_t read;

	IOLockLock(dcs.lock);
	if (dcs.state == DCS_READY && dcs.canRead && dcs.checks < DCS_CHECKS) {
		dcs.checks++;
		read = dcs_read(0x52);
		IOLog("MiniIOMobileFramebuffer: dcs: check %u of %u: panel reads %d, %#x written last (%s)\n", dcs.checks, DCS_CHECKS,
		    read, dcs.sent, read < 0 ? "no read-back" : (uint32_t) read == dcs.sent ? "same" : "DIFFERENT");
	}
	IOLockUnlock(dcs.lock);
}

/*
 * MiniAppleARMBacklight.cpp's bl_take, its lock held: VALUE (the panel's 5 to 0x1a5e) for the panel.
 * True when the panel takes the brightness (fb_dcs=4, the start's checks running or passed): then the
 * mixers leave the picture whole.
 */
bool
MiniIOMobileFramebufferDCS(uint32_t value)
{
	uint64_t now, next;

	if (gDcs < 4 || (dcs.state != DCS_START && dcs.state != DCS_READY)) {
		return false;
	}
	__atomic_store_n(&dcs.want, value, __ATOMIC_RELEASE);
	now = mach_absolute_time();
	next = __atomic_load_n(&dcs.sentAt, __ATOMIC_ACQUIRE) + dcs.period;
	thread_call_enter_delayed(dcs.call, next > now ? next : now);
	return true;
}

/* From flip_start, MDSS mapped and its register clocks on. The checks run on a thread call: boot goes on. */
static void
dcs_start(void)
{
	if (gDcs == 0) {
		return;
	}
	dcs.dsi = ml_io_map(DCS_DSI0_PHYS, DCS_DSI_SIZE);
	dcs.lock = IOLockAlloc();
	dcs.call = thread_call_allocate(dcs_run, NULL);
	dcs.check = thread_call_allocate(dcs_check, NULL);
	if (dcs.dsi == 0 || dcs.lock == NULL || dcs.call == NULL || dcs.check == NULL) {
		IOLog("MiniIOMobileFramebuffer: dcs: cannot map DSI0 or allocate: no DCS\n");
		return;
	}
	nanoseconds_to_absolutetime((uint64_t) DCS_PERIOD_US * NSEC_PER_USEC, &dcs.period);
	dcs.state = DCS_START;
	IOLog("MiniIOMobileFramebuffer: dcs: fb_dcs=%u\n", gDcs);
	thread_call_enter(dcs.call);
}

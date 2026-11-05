/*
 * How fast frames are asked for and how they reach the panel: the refresh rate CoreAnimation is told
 * (fb_ca_hz), the priority of the threads that put frames on the panel (fb_pri), and the presenter
 * loop with its second, filtered drawing of a frame that stands still (fb_filter=2). Included by
 * MiniIOMobileFramebuffer.cpp after setNumber(). All three are off unless their boot-arg is given, so
 * one kernel can run the rounds that compare them.
 */

/*
 * Boot-arg fb_ca_hz=N (24 to 480): publish IOMFBDisplayRefresh, the refresh interval the window server
 * clocks its frames by (QuartzCore's displayMinRefreshInterval; 23G90's QuartzCore has the same keys).
 * Without it CoreAnimation takes the built-in panel for a fixed 60 Hz one whatever TimingElements say.
 * The three intervals are in 2^-32 s (32.32 fixed point), and only displayRefreshStepMachTime is in
 * mach_absolute_time ticks: given ticks, CoreAnimation on 16.5 took a 120 Hz panel for a 26844 Hz one
 * (oosexgang/hakios-xnu-kernel 0dedaec). Minimum and maximum are the same interval: the panel does not
 * vary its rate. The refreshes this driver paces itself by still come at fb_hz.
 */
static TUNABLE(uint32_t, gCARefreshHz, "fb_ca_hz", 0);

static void
publish_refresh(IOService * fb)
{
	uint32_t told = gCARefreshHz;
	uint64_t interval, ticks;
	OSDictionary * refresh;

	if (told < 24 || told > 480) {
		return;
	}
	refresh = OSDictionary::withCapacity(4);
	if (refresh == NULL) {
		return;
	}
	interval = (1ULL << 32) / told;                 /* below 2^32 for every allowed rate: setNumber() is 32-bit */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / told, &ticks);
	setNumber(refresh, "displayMinRefreshInterval", interval);
	setNumber(refresh, "displayMaxRefreshInterval", interval);
	setNumber(refresh, "displayRefreshStep", interval);
	setNumber(refresh, "displayRefreshStepMachTime", ticks);
	fb->setProperty("IOMFBDisplayRefresh", refresh);
	refresh->release();
	IOLog("MiniIOMobileFramebuffer: IOMFBDisplayRefresh: %u Hz (frames paced at %u: fb_hz), an interval of %llu "
	    "(2^-32 s), %llu ticks\n", told, refreshHz(), interval, ticks);
}

/*
 * Boot-arg fb_pri=N (1 to 95): the scheduling priority of the presenter and its helpers; without it the
 * kernel's default for its threads. On the SM7325 phone at the default priority these threads lost the
 * processors to the window server's real-time threads under load, and one frame in ten took 10.9 ms to
 * put on the panel against 3.6 as a rule (oosexgang/hakios-xnu-kernel cd8fdbb, which uses 92,
 * BASEPRI_PREEMPT).
 */
static TUNABLE(uint32_t, gThreadPriority, "fb_pri", 0);
extern "C" kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void * parameter, integer_t priority,
    thread_t * new_thread);

static kern_return_t
panel_thread_start(thread_continue_t continuation, void * parameter, thread_t * thread)
{
	if (gThreadPriority >= 1 && gThreadPriority <= 95) {
		return kernel_thread_start_priority(continuation, parameter, (integer_t) gThreadPriority, thread);
	}
	return kernel_thread_start(continuation, parameter, thread);
}

/*
 * fb_filter=2: nearest neighbour while frames come, and the frame on the panel drawn again bilinear once
 * no other has come for SETTLE_MS, so what stands still on the screen is the filtered picture. During a
 * page swipe every row changes and fb_rows saves nothing: scaling took 5.4 ms a frame bilinear
 * (kernel-12377-cmd38-bo14). The frame's surfaces stay held until it is settled or replaced; the settle
 * is skipped when a swap is already waiting, but one that arrives while it draws can find the surface
 * being drawn into by CoreAnimation again, for the length of that one drawing.
 */
#define SETTLE_MS       40
#define FAST_FRAMES     2

static unsigned fFastFrames = 0;
static bool fFilterFrame = false;
static uint32_t fStatSettleReused = 0;
static uint32_t shownDrawn[SWAP_LAYERS] = { 0 };
static uint64_t lastFrame = 0;

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

static void
drop_surfaces(MiniIOSurface ** surfaces)
{
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL) {
			MiniIOSurfaceDrop(surfaces[layer]);
			surfaces[layer] = NULL;
		}
	}
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS] = { NULL };
	bool unsettled = false;                 /* the panel shows rows drawn nearest neighbour; `surfaces` are the last frame's */
	AbsoluteTime deadline = 0;
	uint64_t settleTicks;

	nanoseconds_to_absolutetime(SETTLE_MS * NSEC_PER_MSEC, &settleTicks);

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			if (!unsettled) {
				IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
			} else if (IOLockSleepDeadline(fSwapLock, fSwap, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
			    fSwapsWaiting == 0) {
				IOLockUnlock(fSwapLock);
				bool reused = false;
				for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
					reused = reused || (surfaces[layer] != NULL && surfaces[layer]->drawn != shownDrawn[layer]);
				}
				if (reused) {
					fStatSettleReused++;
				} else {
					settle(swap, surfaces);
				}
				drop_surfaces(surfaces);
				unsettled = false;
				IOLockLock(fSwapLock);
			}
		}
		drop_surfaces(surfaces);                /* the frame before, settled or not, is done with */
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		{
			uint64_t now = mach_absolute_time();

			fFastFrames = (lastFrame != 0 && now - lastFrame <= settleTicks) ? fFastFrames + 1 : 0;
			fFilterFrame = fFastFrames < FAST_FRAMES;
			for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
				shownDrawn[layer] = surfaces[layer] != NULL ? surfaces[layer]->drawn : 0;
			}
			fDrewNearest = false;
			show(swap, surfaces);
			lastFrame = mach_absolute_time();
		}
		/* (a frame that drew nothing leaves the rows of the one before, so an unsettled panel stays so) */
		unsettled = gFilter == 2 && !fFilterFrame && (unsettled || fDrewNearest);
		if (unsettled) {
			clock_interval_to_deadline(SETTLE_MS, kMillisecondScale, &deadline);
		} else {
			drop_surfaces(surfaces);
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}


/*
 * The frame on the panel once more, filtered: the same layers from the same surfaces, every row (the row
 * hashes would find nothing changed), nothing waited for or counted as a frame.
 */
void
IOMobileFramebuffer::settle(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint64_t began = mach_absolute_time(), bandRows = fStatBandRows;
	int32_t rowsFrom = fStatRowsFrom, rowsTo = fStatRowsTo;

	if (!fGraphics) {
		return;
	}
	fSettling = true;
	fRowHashRows = 0;
	fBase = flip_target(fBase);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		MiniIOSurface * s = surfaces[layer];
		int32_t src[4], dst[4];

		if (s == NULL || s->info.bytesPerElement != 4 || s->info.planeCount != 0) {
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		drawLayer(s, src, dst);
	}
	flip_present();
	fSettling = false;
	fStatBandRows = bandRows;
	fStatRowsFrom = rowsFrom;
	fStatRowsTo = rowsTo;
	fStatSettled++;
	fStatSettleTime += mach_absolute_time() - began;
}

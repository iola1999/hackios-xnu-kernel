/*
 * THE SLOT EVERY COMMAND BUFFER TAKES FROM THE QUEUE, AND WHY IT HAS TO BE GIVEN BACK.
 *
 * -[_MTLCommandBuffer initWithQueue:retainedReferences:synchronousDebugMode:] -- the
 * designated initialiser this driver's command buffer calls, because docs/metal.md 10.13
 * says to call the real one -- does this, at 0x181a1ea14 and on:
 *
 *     ldr  x8, [x19, _MTLCommandBuffer._queue]
 *     ldr  x0, [x8, _MTLCommandQueue._commandBufferSemaphore]
 *     mov  x1, #-1                       ; DISPATCH_TIME_FOREVER
 *     bl   dispatch_semaphore_wait
 *
 * and just before it sets _MTLCommandBuffer._needsCommandBufferSemaphoreSignal to 1. The
 * semaphore is made in -[_MTLCommandQueue initWithDevice:descriptor:] (0x181a1d1fc), which
 * reads maxCommandBufferCount off the descriptor (0x181a1d268) and refuses zero; the
 * default for MTLCommandQueueDescriptor is 64. Every ivar name above is the cache's own
 * (`ipsw dyld a2s`: OBJC_IVAR_$__MTLCommandQueue._commandBufferSemaphore,
 * OBJC_IVAR_$__MTLCommandBuffer._needsCommandBufferSemaphoreSignal), not a guess.
 *
 * So each command buffer takes one of 64 slots AND OWES A SIGNAL -- that ivar is Apple's
 * own name for the debt. In Apple's driver something on the completion path pays it. This
 * driver has no completion path at all: it overrides -commit and -waitUntilCompleted and
 * never calls -enqueueCommandBuffer:, -commitCommandBuffer:wake: or any of the rest, so
 * nothing in Metal will ever pay it for us. The 65th -commandBuffer then waits for ever.
 *
 * MEASURED, not reasoned: logs/114823-run.log. A test that made 64 command buffers (one
 * per shader of an 8x8 mosaic) blocked on the 65th inside -[MTLCommandQueue commandBuffer]
 * and stayed blocked for four minutes, with a heartbeat thread proving the process alive.
 * It is not a test bug: an application makes a command buffer per frame, so this driver
 * would hang anything that drew for a couple of seconds.
 *
 * The debt is paid at -commit, and that is a choice with a reason. Metal holds the slot
 * until the buffer completes; here -commit IS the completion -- the recorded work runs
 * inside it and -waitUntilCompleted only fences -- so holding it any longer would throttle
 * an application for a wait this driver does not have. A buffer that is never committed
 * pays at -dealloc instead, which is the case Apple's flag exists for.
 *
 * Both are done through the runtime by ivar NAME, and a name that is not there is said out
 * loud rather than worked around: on an iOS whose Metal keeps this differently, the right
 * answer is to find out, not to run with a queue that silently blocks at 64.
 */
static dispatch_semaphore_t
annibale_queue_semaphore(id queue)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandQueue");

		looked = YES;
		ivar = base != Nil ? class_getInstanceVariable(base, "_commandBufferSemaphore")
				   : NULL;
		if (ivar == NULL) {
			rw_print("_MTLCommandQueue has no "
			         "_commandBufferSemaphore in this Metal, so the slot every "
			         "command buffer takes cannot be given back and this queue "
			         "will block on its maxCommandBufferCount'th buffer\n");
		}
	}
	return ivar != NULL ? (dispatch_semaphore_t)object_getIvar(queue, ivar) : nil;
}

/* Where a queue's completion queue lives: Apple's own _MTLCommandQueue._completionQueue,
 * found by NAME for the same reason the semaphore above is -- the base class already has
 * the getter for it, and a second copy in this file would mean a queue built from a
 * descriptor that carried one reading back as nil. A Metal without the ivar is said out
 * loud once and falls back to this driver's own field. */
static Ivar
annibale_completion_ivar(void)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandQueue");

		looked = YES;
		ivar = base != Nil ? class_getInstanceVariable(base, "_completionQueue") : NULL;
		if (ivar == NULL) {
			rw_print("_MTLCommandQueue has no _completionQueue in this "
			         "Metal, so -setCompletionQueue: keeps the queue in this driver's "
			         "own field instead of the one Apple's getter reads\n");
		}
	}
	return ivar;
}

@implementation AnnibaleMetalCommandQueue {
	AnnibaleMetalDevice *_owner;
	NSUInteger _outstanding;        /* buffers of this queue that still hold a slot */
	NSUInteger _made, _returned;
	/* What <MTLCommandQueueSPI> keeps about a queue and this driver cannot act on: the
	 * two priorities are remembered so that a caller reading back what it set is not
	 * lied to, and -setCompletionQueue: explains the third below. */
	NSUInteger _gpuPriority, _backgroundGPUPriority;
	dispatch_queue_t _completionQueue;   /* only used when Apple's own ivar is missing */
}

- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device descriptor:(id)descriptor
{
	/* The initialiser IOGPUMetalCommandQueue itself sends to super (0x1ec16dabc). The
	 * descriptor is not optional: the base validates it at its own line 137 and the first
	 * version of this passed nil and was killed by that assertion. */
	self = [super initWithDevice:device descriptor:descriptor];
	if (self == nil) {
		return nil;
	}
	_owner = device;
	/* Said at birth and once: this number is how many command buffers may exist at a
	 * time before -commandBuffer blocks, and until this driver returned the slot it was
	 * also how many this process could ever make. */
	rw_print("this queue takes %lu command buffers at a time "
	         "(maxCommandBufferCount), and each one waits on the queue's semaphore in "
	         "-[_MTLCommandBuffer initWithQueue:...]; this driver gives the slot back at "
	         "-commit%s\n", (unsigned long)[self maxCommandBufferCount],
	         annibale_queue_semaphore(self) != nil ? "" :
	         ", EXCEPT that the semaphore was not found and it cannot");
	return self;
}

- (id)device { return _owner; }

/* What the queue itself thinks, for a test that wants the number rather than the
 * inference: how many command buffers of this queue still hold one of those slots. */
- (NSUInteger)annibaleOutstandingCommandBuffers { return _outstanding; }
- (NSUInteger)annibaleCommandBuffersMade        { return _made; }

- (void)annibaleTookSlot
{
	_outstanding++;
	_made++;
}

- (void)annibaleReturnedSlot
{
	if (_outstanding > 0) {
		_outstanding--;
	}
	_returned++;
}

- (id)commandBuffer
{
	return [[[AnnibaleMetalCommandBuffer alloc] initWithAnnibaleQueue:self] autorelease];
}

- (id)commandBufferWithUnretainedReferences
{
	return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary { }

/*
 * THE TWO METHODS THE SYSTEM NAMED, AND WHAT THEY REALLY ARE.
 *
 * With metal=1 and launchd running, backboardd and mediaserverd sent this queue
 * -setGPUPriority: 58 times and -setCompletionQueue: 30 times in a hundred seconds, and
 * every one of them killed the daemon that sent it (logs/130132-run.log). Neither is in any
 * public header: both are required members of <MTLCommandQueueSPI>, which is declared in
 * AGXMetalA11.bundle and therefore does not exist in any process on this phone -- so
 * objc_getProtocol cannot be asked what this class owes, and the list was read out of the
 * cache instead (tools/metal/protometh.py, tools/metal/owes.py).
 *
 * -setGPUPriority: (B24@0:8Q16 -- NSUInteger in, BOOL out) is NOT bookkeeping.
 * -[IOGPUMetalCommandQueue setGPUPriority:] (0x1ec174cb8) calls _validateGPUPriority
 * (0x1ec178f74) and then -_setGPUPriority:backgroundPriority: (0x1ec178eec), which packs
 * the foreground and background numbers into a three-word request and hands it to the
 * kernel's own command-queue object (_IOGPUCommandQueueSetPriorityAndBackground); only if
 * the kernel accepts does it write the two ivars that -getGPUPriority reads back. So the
 * number is read by the GPU's scheduler, and the ivar is just a cache of what the scheduler
 * agreed to. The validation is worth copying exactly: anything above 5 is refused and 3 is
 * refused for a foreground queue -- "priority (%d) is not valid for foreground" is the
 * cache's own string -- and it only REPORTS, it does not stop the call, which is why
 * Apple's BOOL is the kernel's answer and not the validator's.
 *
 * This driver has one ring and one priority. Every command buffer goes to the kernel's
 * single msm ring in the order it was committed, and there is no second queue to be ahead
 * of; so there is nothing here to tell, and the honest answer is to remember the number,
 * report it back, and say once that it changes nothing.
 */
- (BOOL)setGPUPriority:(NSUInteger)priority
{
	if (priority > 5 || priority == 3) {
		RW_ONCE("GPU priority %lu is outside what Apple's own "
			"_validateGPUPriority allows (0..5, and 3 is the background-only "
			"one); it is remembered as asked, like Apple's own path, which "
			"reports such a value and carries on\n", (unsigned long)priority);
	}
	RW_ONCE("a GPU priority of %lu was asked for; this driver has one ring "
		"and one priority, so the number is remembered and reported back but nothing "
		"schedules by it\n", (unsigned long)priority);
	_gpuPriority = priority;
	return YES;
}

/* The offset is a place WITHIN a priority band (Apple's second argument is an unsigned
 * short, S24 in the encoding). With one ring there is no band to be placed in. */
- (BOOL)setGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
	if (offset != 0) {
		RW_ONCE("a GPU priority offset of %u is ignored: this driver has "
			"one ring, so a place inside a priority band means nothing here\n",
			(unsigned)offset);
	}
	return [self setGPUPriority:priority];
}

/* The background priority is the one a queue gets while its process is not frontmost. Same
 * answer, kept separately so that the two reads do not lie about each other. */
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority
{
	RW_ONCE("a background GPU priority of %lu was asked for; remembered, "
		"not honoured, for the same reason as the foreground one\n",
		(unsigned long)priority);
	_backgroundGPUPriority = priority;
	return YES;
}

- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
	(void)offset;
	return [self setBackgroundGPUPriority:priority];
}

- (NSUInteger)getGPUPriority           { return _gpuPriority; }
- (NSUInteger)getBackgroundGPUPriority { return _backgroundGPUPriority; }

/*
 * -setCompletionQueue: (v24@0:8@16) names the dispatch queue on which this command queue
 * delivers what finishes. -[IOGPUMetalCommandQueue setCompletionQueue:] (0x1ec178e88)
 * releases the queue it held, retains the new one into _MTLCommandQueue._completionQueue --
 * the ivar the base class already has a getter for -- and re-targets the queue's own
 * dispatch source at it (0x1ec1765e8), so that every command buffer's completion handler
 * runs there instead of on whichever thread the kernel's notification arrived on. That is
 * exactly why backboardd sets one: a completion handler that runs on a daemon's render
 * thread is a completion handler that can stall a frame.
 *
 * So this one is SERVED, not declined: the queue is kept, and -[AnnibaleMetalCommandBuffer
 * commit] delivers the scheduled and completed handlers on it (see the note there -- before
 * this they ran inline, at the moment they were added, which is before the work they are
 * about). It is kept in Apple's ivar rather than in one of ours so that a queue built from
 * a descriptor that already carried a completion queue (there is a
 * -[MTLCommandQueueDescriptorInternal setCompletionQueue:]) reads back as what it is; the
 * retain is balanced by -[_MTLCommandQueue dealloc], the ivar's owner.
 */
- (void)setCompletionQueue:(dispatch_queue_t)queue
{
	Ivar ivar = annibale_completion_ivar();
	dispatch_queue_t held = ivar != NULL ?
	    (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;

	if (held == queue) {
		return;
	}
	[queue retain];
	[held release];
	if (ivar != NULL) {
		object_setIvar(self, ivar, queue);
	} else {
		_completionQueue = queue;
	}
}

/* What this driver reads when it has something to deliver. Not an override of -completionQueue
 * (the base class's getter reads the same ivar): this is here for the Metal where that ivar
 * is not, so that one missing ivar does not quietly send every handler to the wrong thread. */
- (dispatch_queue_t)annibaleCompletionQueue
{
	Ivar ivar = annibale_completion_ivar();

	return ivar != NULL ? (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;
}

- (void)dealloc
{
	/* Only the copy this driver keeps itself: the one in Apple's ivar belongs to
	 * -[_MTLCommandQueue dealloc], and releasing it twice would be worse than leaking
	 * one dispatch queue in the Metal where that dealloc does not. */
	[_completionQueue release];
	[super dealloc];
}

@end

/*
 * MTLCommandBufferStatus, AND WHAT A LOG FROM BEFORE THIS CHANGE SAYS.
 *
 * These are Metal's own numbers (MTLCommandBuffer.h): NotEnqueued 0, Enqueued 1, Committed 2,
 * Scheduled 3, Completed 4, Error 5. -[MTLCommandBuffer status] is how real code decides that a
 * buffer is done, so they are not cosmetic.
 *
 * Until 2026-10-21 this file answered 3 for committed, 4 for scheduled and 6 for completed -- and
 * 6 is not a status at all. Under those numbers an application comparing with
 * MTLCommandBufferStatusCompleted (4) saw completion the moment -commit returned, BEFORE the GPU
 * fence of -waitUntilCompleted was paid, and after the fence it saw a number Metal does not
 * define. Error was 5 by coincidence and has not moved.
 *
 * Every boot log under logs/ was written by the old numbering and none of them is going to be
 * regenerated, so those numbers have to be DATED rather than reinterpreted. That is what the
 * banner in -initWithAnnibaleQueue: below is for: it names the numbering of the log that carries
 * it, and it is printed out of these constants, so the line cannot drift from what -status
 * answers. A log without that line is older than this change, and in it "status 6" is this
 * completion, "status 4" is a buffer that was only submitted, and 2 and 3 do not occur at all.
 * That is the rule tools/air/metalcheck.py reads a log by, and the measurement that makes it
 * safe: over all 141 logs in logs/ the only statuses the test ever printed are 5 (128 times) and
 * 6 (990 times) -- never 4.
 *
 * One file more keys on the old numbers and is NOT part of this change: userland/src/
 * metal_drv_test.m compares [cmd status] with the literal 6 in five places (its lines 1200, 1526,
 * 2041, 2293, 2795) and prints the literal "status 6" in a sixth (2308). Until those move, a boot
 * with this driver has the test calling every completed buffer a failure: the sweep prints
 * "drvtest: drawall <name> status 4" with nothing after it and reads back no pixel, and the single
 * draw returns before its own readback. metalcheck.py names that shape when it sees it.
 */
enum {
	RW_CB_NOT_ENQUEUED = 0,
	RW_CB_ENQUEUED     = 1,
	RW_CB_COMMITTED    = 2,
	RW_CB_SCHEDULED    = 3,
	RW_CB_COMPLETED    = 4,
	RW_CB_ERROR        = 5,
};


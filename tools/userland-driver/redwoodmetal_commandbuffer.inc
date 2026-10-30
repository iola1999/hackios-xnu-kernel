#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <mach/vm_map.h>
#include <pthread.h>
#include <ptrauth.h>
#include <time.h>

/* Where a command buffer's encode time goes, by kind of work. The draws and the copies are
 * fixed in different places -- a draw is this library's state and the GPU driver's
 * commands, a copy is a read of one surface and a write of another -- so the window line
 * says which of them a frame is spent in rather than one number that averages them.
 */
enum { RW_WORK_DRAW, RW_WORK_COPY, RW_WORK_DISPATCH, RW_WORK_KINDS };
static struct {
	unsigned long count;
	uint64_t us;
} rw_work_stats[RW_WORK_KINDS];

/* A commit is the frame, and a commit that never returns is what stops the compositor. These
 * two are set for the length of one commit so the work loop can say it is still inside one.
 * Below the threshold the line is silent: 20 ms is longer than a whole frame at 50 Hz, so a
 * commit that crosses it is already a frame that was missed. Microseconds, and every comparison
 * against it converts through rw_ticks_us() first -- mach_absolute_time's ticks are 52.08 ns on
 * this device, so a raw tick count compared to this number is a millisecond, not twenty.
 */
#define RW_COMMIT_SLOW_US 20000ULL
#define RW_TRACE_COMMITS 16

/* One flush longer than this is said on its own line; the window average hides it. */
#define RW_FLUSH_SLOW_US 50000ULL
/* One draw longer than this is said on its own line: the first draw of a process carries the
 * library's setup and is seconds, while the window line reports the microseconds of all. */
#define RW_DRAW_SLOW_US 50000ULL
static uint64_t commit_started;
static unsigned commit_count;

/* The stall that stops the compositor lasts seconds, and a check placed between works fires only
 * after the work that blocked has returned -- which is after the moment a dump is worth taking. So
 * a thread of this process watches instead, at 250 ms, and asks the kernel to print the threads of
 * this process while a commit is still inside one call. The kernel side is kern.redwood.thread_dump
 * (bsd/dev/redwood_threads.c); one pid written there prints one line per thread. One dump per
 * stalled commit, so a run that stalls seven times prints seven dumps.
 */
#define RW_STALL_US 2500000ULL
#define RW_WATCH_NAP_US 250000ULL
static volatile uint64_t commit_progress_us;    /* when the work loop last moved */
static volatile unsigned commit_in_flight;      /* the commit running, 0 when none */
static volatile unsigned commit_phase;          /* 0 in a recorded work, 1 inside mesa.flush */
static unsigned commit_dumped;                  /* the commit already dumped for */

/* A draw that runs for a third of a second leaves the process asleep somewhere, and a timing line
 * only says how long: the dump says which call. rw_draw_began_us is 0 between draws, and
 * rw_draw_dumped names the draw already dumped for, so a run prints one dump per stuck draw. */
#define RW_DRAW_STALL_US 300000ULL
static void rw_print_threads(void);
static volatile uint64_t rw_draw_began_us;
static volatile unsigned rw_draw_serial;
static unsigned rw_draw_dumped;
static pthread_once_t rw_watch_once = PTHREAD_ONCE_INIT;

/* A wait for the GPU is the one stall a check between calls cannot see: the thread is inside
 * the call, so nothing after it runs and no line is printed. The wait records itself here on
 * entry and clears the record when it returns, and the watch thread above says a line when a
 * record has been outstanding for RW_FENCE_SLOW_US. The frame number is the commit ordinal,
 * and the fence is the command buffer whose fence it is -- mesa.finish takes the device, so
 * there is no handle to name.
 */
#define RW_FENCE_SLOW_US 2000000ULL
static volatile unsigned fence_wait;            /* the wait running, 0 when none */
static unsigned fence_waits;                    /* the serial handed to each wait */
static volatile uint64_t fence_began_us;        /* when it entered */
static volatile unsigned fence_frame;           /* the commit it belongs to */
static volatile unsigned fence_tid;             /* the thread waiting */
static const void *volatile fence_owner;        /* the command buffer */
static unsigned fence_reported;                 /* the wait already said */

static uint64_t hb_due_us;                      /* when the next heartbeat line is due */

static uint64_t
rw_now_us(void)
{
	static struct mach_timebase_info tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

/* Microseconds of a mach_absolute_time interval. A draw is measured from three of these, so
 * the arithmetic is in one place rather than repeated at each pair. */
static uint64_t
rw_ticks_us(uint64_t ticks)
{
	static struct mach_timebase_info tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return ticks * tb.numer / tb.denom / 1000;
}

static void *
rw_stall_watch(void *ignored)
{
	(void)ignored;
	for (;;) {
		/* The two readings taken whatever the commit is doing: the fence wait (a line after 2 s)
		 * and the heartbeat (a line every 1 s). Both sat below the stall checks until this point,
		 * and those `continue` whenever no commit is running or the running one has moved within
		 * 2.5 s -- so neither line could be printed in the state either exists to report.
		 */
		/* The fence wait, which the commit stall check cannot see: the waiting thread is
		 * inside mesa.finish, and the check below only reports a commit that stops moving.
		 * Said once per wait, and only after the wait has outlasted RW_FENCE_SLOW_US. */
		{
			unsigned waiting = __atomic_load_n(&fence_wait, __ATOMIC_ACQUIRE);

			if (waiting != 0 && waiting != fence_reported) {
				uint64_t waited = rw_now_us() - fence_began_us;

				if (waited >= RW_FENCE_SLOW_US) {
					fence_reported = waiting;
					rw_print("tl: fw %u %p %u %llu %llu\n", fence_frame,
					    fence_owner, fence_tid,
					    (unsigned long long)(waited / 1000),
					    (unsigned long long)rw_now_us());
				}
			}
		}

		/* The heartbeat, printed whether or not anything moved: a line of zeros is the
		 * reading that says the frame flow stopped. The counts are exchanged for zero
		 * rather than read and cleared, so a call landing between the two is counted in
		 * the next second and not lost. */
		{
			uint64_t now = rw_now_us();

			if (hb_due_us == 0)
				hb_due_us = now + RW_HEARTBEAT_US;
			if (now >= hb_due_us) {
				rw_print("tl: cnt %u 0 %u %u 0 %llu\n",
				    __atomic_exchange_n(&hb_commit, 0, __ATOMIC_RELAXED),
				    __atomic_exchange_n(&hb_wait, 0, __ATOMIC_RELAXED),
				    __atomic_exchange_n(&hb_texwrite, 0, __ATOMIC_RELAXED),
				    (unsigned long long)now);
				while (hb_due_us <= now)
					hb_due_us += RW_HEARTBEAT_US;
			}
		}

		/* A draw that has outlasted RW_DRAW_STALL_US: dump the process's threads while it is
		 * still inside the call. rw_print_threads reads them from this process and prints one
		 * line per thread, so the one wanted is the render thread's PC, which names the call
		 * it is asleep in. */
		{
			uint64_t began = __atomic_load_n(&rw_draw_began_us, __ATOMIC_ACQUIRE);
			unsigned serial = __atomic_load_n(&rw_draw_serial, __ATOMIC_ACQUIRE);

			if (began != 0 && serial != rw_draw_dumped && rw_now_us() - began >= RW_DRAW_STALL_US) {
				rw_draw_dumped = serial;
				rw_print("draw %u has run for %llu ms; reading this process's threads\n", serial,
				    (unsigned long long)((rw_now_us() - began) / 1000));
				rw_print_threads();
			}
		}

		struct timespec nap = { 0, (long)(RW_WATCH_NAP_US * 1000) };
		unsigned watched = commit_in_flight;
		uint64_t moved = commit_progress_us;

		nanosleep(&nap, NULL);
		if (watched == 0 || watched == commit_dumped || watched != commit_in_flight) {
			continue;
		}
		if (rw_now_us() - moved < RW_STALL_US) {
			continue;
		}
		commit_dumped = watched;
		/* A commit that stops moving is either inside a recorded work or inside the
		 * library's flush, and the two have different owners: the work is this file's
		 * loop, the flush is freedreno's. Said before the dump, because a flush that
		 * never returns leaves no line after it. */
		rw_print("commit %u has not moved for %llu ms inside %s\n", watched,
		    (unsigned long long)((rw_now_us() - moved) / 1000),
		    commit_phase != 0 ? "mesa.flush" : "a recorded work");
		{
			pid_t self = getpid();
			int refused;

			refused = sysctlbyname("kern.redwood.thread_dump", NULL, NULL, &self,
			    sizeof(self));
			if (refused != 0) {
				rw_print("thread dump for pid %d refused (errno %d); reading this process's threads\n",
				    (int)self, errno);
				rw_print_threads();
			}
		}

	}
	return NULL;
}

static void
rw_start_watch(void)
{
	pthread_attr_t attr;
	pthread_t thread;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attr, rw_stall_watch, NULL) != 0) {
		rw_print("no watch thread: a stalled commit will not dump the thread states\n");
	}
	pthread_attr_destroy(&attr);
}

static void
rw_work_time(int kind, uint64_t began)
{
	static struct mach_timebase_info tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	rw_work_stats[kind].count++;
	rw_work_stats[kind].us += (mach_absolute_time() - began) * tb.numer / tb.denom / 1000;
}

@implementation RedwoodMetalCommandBuffer {
	RedwoodMetalCommandQueue *_queue;
	NSMutableArray *_work;
	NSError *_error;
	NSUInteger _status;
	BOOL _owesSlot;                 /* this buffer holds one of the queue's slots */
	NSMutableArray *_scheduled, *_completed;   /* handler blocks, copied to the heap */
	NSUInteger _protectionOptions;  /* what the window server asked for; see -setProtectionOptions: */
}

/* MPSCommandBufferImageCache checks this protocol before creating its cache. */
+ (void)load
{
    Protocol *protocol = objc_getProtocol("MTLCommandBuffer");
    if (protocol == NULL || (!class_conformsToProtocol(self, protocol) &&
        !class_addProtocol(self, protocol))) {
        rw_print("class_addProtocol(MTLCommandBuffer) unavailable or refused\n");
    }
}

/* 23G90 supplies task identity token send rights. Mesa/RedwoodGPU has no cross-task attribution
 * path, so no identity here can be used for anything: this counts them and releases the temporary
 * name right it is handed. A token that is not this process is the ordinary case in the render
 * server, which submits for the applications it composites -- refusing one killed backboardd
 * fifteen times in a 300 s run on 2026-10-27, and with it SpringBoard's remote render context.
 * This does not implement the IOGPU opcode 14 contract. */
- (void)setResponsibleTaskIDs:(const unsigned int *)taskIDs count:(unsigned int)count
{
    unsigned int selfVerified = 0, other = 0, unassigned = 0, failed = 0;
    if (taskIDs == NULL) {
        count = 0;
    }
    for (unsigned int i = 0; i < count; i++) {
        mach_port_t token = taskIDs[i];
        if (token == MACH_PORT_NULL || token == MACH_PORT_DEAD) {
            unassigned++;
            continue;
        }
        mach_port_t name = MACH_PORT_NULL;
        if (task_identity_token_get_task_port(token, TASK_FLAVOR_NAME, &name) != KERN_SUCCESS) {
            failed++;
            continue;
        }
        boolean_t isSelf = mach_task_is_self(name);
        kern_return_t kr = mach_port_deallocate(mach_task_self(), name);
        if (kr != KERN_SUCCESS) {
            RW_ONCE("setResponsibleTaskIDs: a temporary task name right was not released, kr=%d\n", kr);
        }
        if (isSelf) {
            selfVerified++;
        } else {
            other++;
        }
    }
    RW_ONCE("setResponsibleTaskIDs: count=%u self=%u other_task=%u unassigned=%u unreadable=%u; "
        "cross-task attribution unavailable\n", count, selfVerified, other, unassigned, failed);
}

/*
 * The window server sends this to every command buffer it makes, and until it was answered the
 * boot of 22 October 01:51 ended it 78 times with
 *
 *   NSInvalidArgumentException: -[RedwoodMetalCommandBuffer setProtectionOptions:]:
 *   unrecognized selector sent to instance
 *
 * -- an uncaught Objective-C exception out of CA::WindowServer::IOMFBServer::thread_body, which
 * is terminate() and not a refusal we could read.
 *
 * It was missed by owes.py for a reason worth keeping: that tool counts what the PROTOCOLS
 * require, and this pair is declared on a CLASS -- IOGPUMetalCommandBuffer, whose own
 * _protectionOptions ivar it is. Our command buffer descends from _MTLCommandBuffer and not
 * from IOGPU's, so nothing in the chain answers it and nothing in the protocol lists says it is
 * missing. Every selector Apple's own counterpart class declares is a selector a caller may
 * send, and the protocols are only part of that list.
 *
 * The value is kept and not honoured. Protected content means memory the GPU can read and the
 * processor cannot, which this port has nothing to provide: a buffer of ours is ordinary memory
 * on both sides. So a non-zero request says so once rather than being refused -- the window
 * server asks for it on buffers that carry no protected content at all, and refusing would stop
 * a composite that is in no danger.
 */
- (void)setProtectionOptions:(NSUInteger)options
{
	if (options != 0 && _protectionOptions != options) {
		RW_ONCE("a command buffer was asked for protection options %lu, and this "
			"driver has no protected memory to give: the request is kept and not honoured, "
			"because nothing it would protect is protected here either\n",
			(unsigned long)options);
	}
	_protectionOptions = options;
}

- (NSUInteger)protectionOptions
{
	return _protectionOptions;
}

- (instancetype)initWithRedwoodQueue:(RedwoodMetalCommandQueue *)queue
{
	/* what IOGPUMetalCommandBuffer sends to super, retained references and no debug mode.
	 * THIS CALL BLOCKS when the queue's semaphore is empty -- see the long note above
	 * RedwoodMetalCommandQueue -- so by the time it returns, a slot is held and owed. */
	self = [super initWithQueue:queue retainedReferences:YES synchronousDebugMode:NO];
	if (self == nil) {
		return nil;
	}
	_queue = [queue retain];
    RW_ONCE("MTLCommandBuffer conformance=%d device_matches_queue=%d\n",
        [self conformsToProtocol:objc_getProtocol("MTLCommandBuffer")],
        [self device] == [queue device]);
	_work = [[NSMutableArray alloc] init];
	/* The numbering this log is written in, said once per process and printed out of the
	 * constants that answer -status: a log that does NOT carry this line was written before
	 * the numbers became Metal's, and says committed 3, scheduled 4, completed 6 (see the
	 * note above this class). */
	RW_ONCE("MTLCommandBufferStatus: committed %u, scheduled %u, completed %u, "
		"error %u -- Metal's own numbers; a log without this line was written by the old "
		"ones (committed 3, scheduled 4, completed 6)\n",
		(unsigned)RW_CB_COMMITTED, (unsigned)RW_CB_SCHEDULED,
		(unsigned)RW_CB_COMPLETED, (unsigned)RW_CB_ERROR);
	_status = RW_CB_ENQUEUED;   /* nothing is pending yet */
	_owesSlot = YES;
	[queue redwoodTookSlot];
	return self;
}

/*
 * Give the queue's slot back, exactly once. Called at the end of -commit, where this
 * driver's work is already done, and at -dealloc for a buffer that was never committed --
 * which is the case Apple's _needsCommandBufferSemaphoreSignal exists for and the one that
 * would otherwise lose a slot for the life of the process.
 */
- (void)redwoodReturnSlot
{
	dispatch_semaphore_t semaphore;

	if (!_owesSlot) {
		return;
	}
	_owesSlot = NO;
	redwood_clear_semaphore_debt(self);
	[_queue redwoodReturnedSlot];
	semaphore = redwood_queue_semaphore(_queue);
	if (semaphore != nil) {
		dispatch_semaphore_signal(semaphore);
	}
}

- (void)dealloc
{
	[self redwoodReturnSlot];
	[_work release];
	[_error release];
	[_scheduled release];
	[_completed release];
	[_queue release];
	[super dealloc];
}

- (id)device       { return [_queue device]; }
- (id)commandQueue { return _queue; }
- (NSUInteger)status { return _status; }
- (NSError *)error { return _error; }
- (BOOL)retainedReferences { return YES; }

- (id)computeCommandEncoder
{
	return [[[RedwoodMetalComputeEncoder alloc] initWithRedwoodCommandBuffer:self]
		   autorelease];
}

- (id)renderCommandEncoderWithDescriptor:(id)descriptor
{
	if (descriptor == nil) {
		rw_print("a render pass needs a descriptor\n");
		return nil;
	}
	return [[[RedwoodMetalRenderEncoder alloc] initWithRedwoodCommandBuffer:self
								    descriptor:descriptor]
		   autorelease];
}

- (id)parallelRenderCommandEncoderWithDescriptor:(id)descriptor
{
	(void)descriptor;
	return nil;   /* one encoder at a time here; a parallel pass needs the split */
}

/* Encoder creation and event entry points used by MTLCommandBuffer clients. */
- (id)blitCommandEncoder
{
	return [[[RedwoodMetalBlitEncoder alloc] initWithRedwoodCommandBuffer:self] autorelease];
}
- (id)resourceStateCommandEncoder { return nil; }
- (id)accelerationStructureCommandEncoder { return nil; }
- (void)encodeSignalEvent:(id)event value:(uint64_t)value { (void)event; (void)value; }
- (void)encodeWaitForEvent:(id)event value:(uint64_t)value { (void)event; (void)value; }

- (id)computeCommandEncoderWithDispatchType:(NSUInteger)type
{
	(void)type;   /* serial and concurrent dispatch are the same here: one queue, in order */
	return [self computeCommandEncoder];
}

- (void)recordWork:(RedwoodMetalWork *)work
{
	[work retainResources];
	[_work addObject:work];
}

- (void)enqueue { }

/* One Mesa context serves every command buffer of the process, and it is not thread safe. Each of
 * the four backboardd crashes on 2026-10-30 (SIGSEGV in fd6_vsc_update_sizes and nearby fd6 code)
 * came right after "commit N started while commit N-1 was still running"; the first was on a swipe
 * up out of Settings. Recursive because a completion handler may commit the next buffer from inside
 * this one. */
static pthread_mutex_t rw_commit_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

/* Metal's own rule, kept: the recorded dispatches run here and not when they were encoded. */
- (void)commit
{
	char why[512] = { 0 };
	uint64_t started = mach_absolute_time();

	pthread_mutex_lock(&rw_commit_lock);
	__atomic_add_fetch(&hb_commit, 1, __ATOMIC_RELAXED);

	if (commit_started != 0) {
		/* Two commits at once means two threads in one driver with one ring. Said out loud
		 * because it changes what a slow commit means: the wait may be the other thread's. */
		rw_print("commit %u started while commit %u was still running\n", commit_count + 1,
			 commit_count);
	}
	commit_started = started;
	commit_count++;
	pthread_once(&rw_watch_once, rw_start_watch);
	commit_progress_us = rw_now_us();
	commit_in_flight = commit_count;
	_status = RW_CB_COMMITTED;
	/* Textures laid over buffers get the callers' bytes copied into them again: the
	 * processor may have written those buffers since the last -commit, and Metal's own
	 * promise is that a texture over a buffer reads what is there now.
	 */
	if (mesa.buffer_textures_refresh != NULL) {
		mesa.buffer_textures_refresh([[_queue device] mesaDevice]);
	}
	uint64_t say_after_us = RW_COMMIT_SLOW_US;

	/* The first commits of a process name the pipelines they use. A GPU hang is reported by
	 * the kernel as a process and its submission number, and this is what matches that
	 * submission to shaders (the handles are in the "render pipelines" and "compute
	 * pipeline" lines). */
	if (commit_count <= RW_TRACE_COMMITS) {
		void *seen[16];
		unsigned distinct = 0;
		char list[16 * 20 + 1] = "";
		size_t at = 0;

		for (RedwoodMetalWork *work in _work) {
			void *handle = work->render != nil ? (void *)[work->render render]
				     : work->pipeline != nil ? (void *)[work->pipeline compute] : NULL;
			unsigned k = 0;

			while (k < distinct && seen[k] != handle)
				k++;
			if (handle == NULL || k < distinct || distinct == 16)
				continue;
			seen[distinct++] = handle;
			at += (size_t)snprintf(list + at, sizeof(list) - at, " %p", handle);
		}
		rw_print("trace commit %u: %lu works, pipelines%s\n", commit_count,
			 (unsigned long)[_work count], list);
	}

	unsigned work_index = 0;

	for (RedwoodMetalWork *work in _work) {
		uint64_t work_began = mach_absolute_time();

		work_index++;

		commit_progress_us = rw_now_us();

		/* A commit that is slow says so while it is still running, naming the work it is
		 * on. The compositor stops making frames when a commit never returns, and the log
		 * otherwise ends at the last completed frame -- which says the run stopped without
		 * saying where. One line per work of a slow commit is a trail that ends at the call
		 * that blocked.
		 */
		if (commit_started != 0) {
			uint64_t spent_us = rw_ticks_us(mach_absolute_time() - commit_started);

			/* Each time the commit's age doubles past the limit, not once per work: a
			 * 46 ms commit of 121 works wrote 121 lines, and 2489 of them filled a third
			 * of the log ring in round lock3cpu4. */
			if (spent_us > say_after_us) {
				rw_print("commit %u has run for %llu ms and is on work kind %d "
				         "(%u binds, %s)\n", commit_count,
				         (unsigned long long)(spent_us / 1000), (int)work->copyKind,
				         work->count, work->render != nil ? "a draw" : "a dispatch");
				say_after_us = spent_us * 2;
			}
		}

		if (work->copyKind != RW_COPY_NONE) {
			if (rw_run_copy(work, why, sizeof(why)) != 0) {
				[self failWith:why];
				goto done;
			}
			rw_work_time(RW_WORK_COPY, work_began);
			continue;
		}
		if (work->render != nil) {
			if (![self runDraw:work why:why size:sizeof(why)]) {
				goto done;
			}
			rw_work_time(RW_WORK_DRAW, work_began);
			if (rw_sync_draws() && ![self syncDraw:work index:work_index why:why
							  size:sizeof(why)]) {
				goto done;
			}
			/* The window line reports draws by the microsecond on average, which is
			 * the wrong shape for the first draw of a process: that one carries the
			 * library's own setup and can take seconds. Said on its own line. */
			{
				struct mach_timebase_info tb;
				uint64_t draw_us;

				mach_timebase_info(&tb);
				draw_us = (mach_absolute_time() - work_began) * tb.numer / tb.denom / 1000;
				if (draw_us > RW_DRAW_SLOW_US) {
					rw_print("draw of commit %u took %llu ms (%u binds, %u textures)\n",
					    commit_count, (unsigned long long)(draw_us / 1000),
					    work->count, work->textureCount);
				}
			}
			continue;
		}
		struct rwa_buffer *buffers[RW_MAX_BUFFERS] = { NULL };
		struct rwa_texture *textures[RW_MAX_BUFFERS] = { NULL };
		struct rwa_sampler *samplers[RW_MAX_BUFFERS] = { NULL };
		/* Metal's one [[buffer(n)]] table, passed BOTH ways: `buffers` is what is
		 * bound at each index, `table` is the same thing with Metal's own offset
		 * beside it, and the library takes from each side what that side owns -- an
		 * argument buffer's encoding offset and a buffer argument's own offset come
		 * from the table, and which an index is nobody but the shader knows. */
		struct rwa_buffer_bind table[RW_MAX_BUFFERS] = { { NULL, 0 } };

		for (unsigned i = 0; i < work->count; i++) {
			buffers[i] = [work->buffers[i] mesaBuffer];
			table[i].buffer = buffers[i];
			table[i].offset = work->bufferOffsets[i];
			if (buffers[i] == NULL && work->buffers[i] != nil) {
				rw_print("buffer %u of this dispatch did not come "
				         "from the compiler library, so it cannot be bound\n", i);
			}
		}
		for (unsigned i = 0; i < work->textureCount; i++) {
			textures[i] = [work->textures[i] mesaTexture];
			if (textures[i] == NULL && work->textures[i] != nil) {
				rw_print("texture %u of this dispatch is a %s with no texture in the "
				         "compiler library, so it cannot be bound\n", i,
				         object_getClassName(work->textures[i]));
			}
		}
		for (unsigned i = 0; i < work->samplerCount; i++) {
			samplers[i] = [work->samplers[i] mesaSampler];
		}
		struct rwa_bindings bindings = {
			.buffers = buffers, .buffer_count = work->count,
			.textures = textures, .texture_count = work->textureCount,
			.samplers = samplers, .sampler_count = work->samplerCount,
		};

		/* rwa_dispatch_buffers when the library has it, which is the same call with
		 * Metal's offsets passed as well; a library too old to have it is used
		 * through rwa_dispatch, and then a kernel with a set is refused by name
		 * there and every offset is read as zero -- which is what an older library
		 * did with one in any case, out loud. */
		int rc = mesa.dispatch_threadgroup != NULL
			    ? mesa.dispatch_threadgroup([work->pipeline compute], &bindings,
						        table, work->count, work->groups,
						        work->size, work->threadgroupBytes, why,
						        sizeof(why))
			    : mesa.dispatch_buffers != NULL
			    ? mesa.dispatch_buffers([work->pipeline compute], &bindings,
						    table, work->count, work->groups,
						    work->size, why, sizeof(why))
			    : mesa.dispatch([work->pipeline compute], &bindings, work->groups,
					    work->size, why, sizeof(why));
		rw_work_time(RW_WORK_DISPATCH, work_began);
		if (rc != 0) {
			rw_print("the dispatch failed: %s\n", why);
			_error = [[NSError errorWithDomain:@"RedwoodMetal" code:2
						  userInfo:@{ NSLocalizedDescriptionKey:
							      [NSString stringWithUTF8String:why] }]
				     retain];
			_status = RW_CB_ERROR;
			goto done;
		}
	}
	_status = RW_CB_SCHEDULED;   /* submitted; the fence below is what makes it completed */
done:
	/* The draws this buffer recorded are all in the library's batch by now -- a draw no
	 * longer waits for the GPU (rwa_render_draw) -- and this hands them over.
	 *
	 * Submitting at every -commit was better than submitting at every draw and still cost
	 * a frame: the timeline put a frame with one submission at 16.6 ms and one with 7.7
	 * at 62 ms, and the kernel saw about one fence wait per frame. Batching several of a
	 * frame's commits into one submission was tried and never engaged: every buffer the
	 * compositor commits carries a handler, and a buffer with a handler has to be
	 * submitted at once, because the present handler must not register a surface the GPU
	 * has not been given yet.
	 */
	if (mesa.flush != NULL) {
		char flushed[256] = { 0 };
		static unsigned long commits, window_commits, window_works, window_flushes;
		static unsigned window_tables;
		static uint64_t window_encode, window_flush;
		struct mach_timebase_info tb;
		uint64_t encode;

		mach_timebase_info(&tb);
		encode = (mach_absolute_time() - started) * tb.numer / tb.denom / 1000;
		commits++;
		window_commits++;
		window_works += [_work count];
		window_encode += encode;
		uint64_t before = mach_absolute_time();

		/* The window average above cannot show one flush that took a second: 64 commits
		 * divide it away, and a commit that never returns prints nothing at all. The phase
		 * lets the stall watcher name this call while it is still in it, and a flush long
		 * enough to be a missed frame says its own length when it returns. */
		commit_phase = 1;
		commit_progress_us = rw_now_us();
		mesa.flush([[_queue device] mesaDevice], flushed, sizeof(flushed));
		commit_phase = 0;
		commit_progress_us = rw_now_us();
		window_flush += (mach_absolute_time() - before) * tb.numer / tb.denom / 1000;
		window_flushes++;
		{
			uint64_t flushed_us = (mach_absolute_time() - before) * tb.numer / tb.denom / 1000;

			if (flushed_us > RW_FLUSH_SLOW_US) {
				rw_print("flush %u took %llu ms over %lu recorded works\n", commit_count,
				    (unsigned long long)(flushed_us / 1000), (unsigned long)[_work count]);
			}
		}
		if (window_commits == 64) {
			rw_print("command buffers: %lu committed, %.2f works each, encode %llu us, "
			         "%lu flushes of %llu us each; per work: draw %llu us of %lu, copy "
			         "%llu us of %lu, dispatch %llu us of %lu (last 64)\n", commits,
			         (double)window_works / (double)window_commits,
			         window_encode / window_commits, window_flushes,
			         window_flushes ? window_flush / window_flushes : 0,
			         rw_work_stats[RW_WORK_DRAW].count ? rw_work_stats[RW_WORK_DRAW].us /
			             rw_work_stats[RW_WORK_DRAW].count : 0,
			         rw_work_stats[RW_WORK_DRAW].count,
			         rw_work_stats[RW_WORK_COPY].count ? rw_work_stats[RW_WORK_COPY].us /
			             rw_work_stats[RW_WORK_COPY].count : 0,
			         rw_work_stats[RW_WORK_COPY].count,
			         rw_work_stats[RW_WORK_DISPATCH].count ? rw_work_stats[RW_WORK_DISPATCH].us /
			             rw_work_stats[RW_WORK_DISPATCH].count : 0,
			         rw_work_stats[RW_WORK_DISPATCH].count);
			memset(rw_work_stats, 0, sizeof(rw_work_stats));
			/* The pipeline table is asked for far less often than the line above it: a
			 * window is about a tenth of a second here, and every window's table would
			 * be tens of thousands of lines over a run. */
			if (++window_tables == 128) {
				window_tables = 0;
				rw_pipeline_dump();
			}
			window_commits = 0;
			window_works = 0;
			window_flushes = 0;
			window_encode = 0;
			window_flush = 0;
		}
	}
	/* The queue's slot goes back here and on every way out of this method, a failed draw
	 * included: a buffer that errored is as finished as one that drew, and a slot kept
	 * because of an error would turn one bad draw into a queue that blocks for ever. */
	[self redwoodReturnSlot];
	commit_started = 0;
	commit_in_flight = 0;
	/* The handlers run outside the lock: one that waits for another thread's commit would
	 * otherwise wait for ever. */
	pthread_mutex_unlock(&rw_commit_lock);
	/* and only then the handlers, so that a handler which makes the next command buffer
	 * is not waiting for a slot this one still holds */
	[self redwoodDeliverHandlers];
	/* How long the whole commit took, said only when it was long enough to be a missed
	 * frame. A commit that never returns leaves the "has run for" line above as the last
	 * thing this process said, which names the work it was inside. */
	{
		uint64_t us = rw_ticks_us(mach_absolute_time() - started);

		if (us > RW_COMMIT_SLOW_US) {
			rw_print("commit %u took %llu ms over %lu recorded works\n", commit_count,
			         (unsigned long long)(us / 1000),
			         (unsigned long)[_work count]);
		}
	}
}

/* A draw, built out of what the encoder recorded. Separate from the dispatch loop because the
 * two have different bindings -- a draw has them per stage -- and folding them together would
 * mean a struct whose fields mean different things depending on a flag. */
#define RW_COMMIT_SLOW_FOR_IN 500000ULL  /* a commit this old says its draws on entry */

/* rw_sync_draws: hand this draw to the GPU and wait for it, naming it when it does not finish. */
- (BOOL)syncDraw:(RedwoodMetalWork *)work index:(unsigned)index why:(char *)why size:(size_t)size
{
	struct rwa_device *device = [[_queue device] mesaDevice];

	if (mesa.flush == NULL || mesa.finish == NULL)
		return YES;
	rw_describe_draw(work, [work->render render], commit_count, index);
	mesa.flush(device, why, size);
	if (mesa.finish(device, why, size) != 0) {
		rw_print("sync: commit %u work %u did not finish: pipeline %p, %u colour attachments, "
			 "%u textures, %u buffers: %s\n", commit_count, index,
			 (void *)[work->render render], work->colorCount, work->textureCount, work->count,
			 why);
		rw_dump_kept_air([work->render render], "hung");
		[self failWith:why];
		return NO;
	}
	return YES;
}

- (BOOL)runDraw:(RedwoodMetalWork *)work why:(char *)why size:(size_t)size
{
	uint64_t rd_t0 = mach_absolute_time();
	struct rwa_texture *color[8] = { NULL };
	struct rwa_texture *resolve[8] = { NULL };
	struct rwa_buffer *vertex_buffers[RW_MAX_BUFFERS] = { NULL };
	struct rwa_vertex_stream vertex_streams[RW_MAX_BUFFERS] = { { NULL, 0 } };
	struct rwa_buffer *fragment_buffers[RW_MAX_BUFFERS] = { NULL };
	struct rwa_texture *fragment_textures[RW_MAX_BUFFERS] = { NULL };
	struct rwa_sampler *fragment_samplers[RW_MAX_BUFFERS] = { NULL };
	/* and Metal's own table for each stage: the same entries as the arrays above with
	 * the OFFSET beside each, which rwa_bindings has nowhere to put. One array for
	 * both of the things an offset means -- which encoding of an argument buffer, and
	 * which block of a buffer argument -- because Metal has one call behind both. */
	struct rwa_buffer_bind vertex_table[RW_MAX_BUFFERS] = { { NULL, 0 } };
	struct rwa_buffer_bind fragment_table[RW_MAX_BUFFERS] = { { NULL, 0 } };

	/* Pairs with the `rd out` line below: a draw that entered and did not come back is the
	 * one worth reading, so entering is recorded here and its own exit always says so. The
	 * condition is on the commit, not on this draw, so the line appears only where the frame
	 * is already slow. */
	bool rd_said_in = false;

	if (rw_ticks_us(mach_absolute_time() - commit_started) > RW_COMMIT_SLOW_FOR_IN) {
		rd_said_in = true;
		rw_print("rd in %u %u %u %p\n", commit_count, work->count, work->textureCount,
		    (void *)[work->render render]);
	}

	if (mesa.render_draw == NULL) {
		snprintf(why, size, "this compiler library can compute but not yet draw");
		[self failWith:why];
		return NO;
	}
	for (unsigned i = 0; i < work->colorCount; i++) {
		color[i] = [work->color[i] mesaTexture];
		resolve[i] = work->resolve[i] != nil ? [work->resolve[i] mesaTexture] : NULL;
		if (work->resolve[i] != nil && resolve[i] == NULL) {
			snprintf(why, size, "the resolve texture of colour attachment %u did "
					    "not come from the compiler library, so there is "
					    "nothing to resolve into", i);
			[self failWith:why];
			return NO;
		}
	}
	/*
	 * Metal's one vertex buffer table, split in two by the pipeline's own mask: an index
	 * the vertex descriptor fetches attributes from is a STREAM, with its offset, and
	 * every other index is a [[buffer(n)]] ARGUMENT of the vertex stage. The library
	 * refuses a descriptor that claims an index the module also reads as a buffer
	 * (air_metal.h), so the two sets cannot overlap and this split cannot drop a
	 * binding.
	 *
	 * The offset is honoured on the stream side and still unimplemented on the argument
	 * side, and that asymmetry is said out loud rather than left in a comment: the
	 * library takes a stream's offset as Metal's own `offset:` and rwa_bindings has
	 * nowhere to put one.
	 */
	unsigned stream_mask = [work->render vertexStreamMask];

	for (unsigned i = 0; i < work->vertexBufferCount; i++) {
		/* Metal's own offset, for whichever of the other two things index i is: an
		 * argument buffer (which encoding to read) or a buffer argument (which
		 * block). The library takes it from here and judges it there, where the
		 * shader's own address space is in hand; a stream's offset is the stream
		 * array's below and an index is never both. */
		if (!(stream_mask & (1u << i))) {
			vertex_table[i].buffer = [work->vertexBuffers[i] mesaBuffer];
			vertex_table[i].offset = work->vertexBufferOffsets[i];
		}
		if (i < RWA_MAX_VERTEX_BUFFERS && (stream_mask & (1u << i))) {
			vertex_streams[i].buffer = [work->vertexBuffers[i] mesaBuffer];
			vertex_streams[i].offset = work->vertexBufferOffsets[i];
			if (work->vertexBuffers[i] != nil &&
			    vertex_streams[i].buffer == NULL) {
				snprintf(why, size, "the buffer bound as vertex stream %u did "
						    "not come from the compiler library, so "
						    "there is nothing to fetch vertices out "
						    "of", i);
				[self failWith:why];
				return NO;
			}
			continue;
		}
		vertex_buffers[i] = [work->vertexBuffers[i] mesaBuffer];
	}
	if ([work->render vertexStreamCount] > RW_MAX_BUFFERS) {
		snprintf(why, size, "this pipeline fetches from %u vertex stream slots and "
				    "this driver's table holds %u", [work->render
				    vertexStreamCount], RW_MAX_BUFFERS);
		[self failWith:why];
		return NO;
	}
	for (unsigned i = 0; i < work->count; i++) {
		fragment_buffers[i] = [work->buffers[i] mesaBuffer];
		fragment_table[i].buffer = fragment_buffers[i];
		fragment_table[i].offset = work->bufferOffsets[i];
		if (work->buffers[i] != nil && fragment_buffers[i] == NULL) {
			snprintf(why, size, "the buffer bound at fragment buffer index %u did "
					    "not come from the compiler library, so there is "
					    "nothing to bind", i);
			[self failWith:why];
			return NO;
		}
	}
	for (unsigned i = 0; i < work->textureCount; i++) {
		fragment_textures[i] = [work->textures[i] mesaTexture];
		if (fragment_textures[i] == NULL && work->textures[i] != nil) {
			rw_print("texture %u of this draw is a %s with no texture in the compiler "
			         "library, so it cannot be bound\n", i,
			         object_getClassName(work->textures[i]));
		}
	}
	for (unsigned i = 0; i < work->samplerCount; i++) {
		fragment_samplers[i] = [work->samplers[i] mesaSampler];
	}
	struct rwa_draw draw = {
		.color = color,
		.color_count = work->colorCount,
		.clear_color = work->clears ? work->clearColor : NULL,
		/* The depth attachment, its two load actions, the state that was bound and
		 * the depth range of the viewport -- which is where the quad's own depth
		 * comes from, the generated vertex stage putting every corner at clip z = 0
		 * (-setViewport: has the whole of it). Without a viewport the contract's
		 * default [0, 1] applies and the quad sits at 0, the near plane. */
		.depth = [work->depth mesaTexture],
		.clear_depth = work->clearsDepth ? &work->clearDepth : NULL,
		.clear_stencil = work->clearsStencil ? &work->clearStencil : NULL,
		.depth_stencil = [work->depthStencilState mesaDepthStencil],
		.stencil_reference = { work->stencilReference[0], work->stencilReference[1] },
		.depth_range = work->hasDepthRange ? work->depthRange : NULL,
		.viewport = work->hasViewportRect ? work->viewportRect : NULL,
		.scissor = work->hasScissorRect ? work->scissorRect : NULL,
		.vertex = { .buffers = vertex_buffers, .buffer_count = work->vertexBufferCount },
		/* and the streams the layout fetches from, at Metal's own indices with
		 * Metal's own offsets; the count is what the library needs covered, which
		 * is the highest index the pipeline fetches from plus one. */
		.vertex_streams = vertex_streams,
		.vertex_stream_count = [work->render vertexStreamCount],
		.fragment = {
			.buffers = fragment_buffers, .buffer_count = work->count,
			.textures = fragment_textures, .texture_count = work->textureCount,
			.samplers = fragment_samplers, .sampler_count = work->samplerCount,
		},
		.primitive = work->primitive,
		.vertex_start = work->vertexStart,
		.vertex_count = work->vertexCount,
		/* The index buffer, by the same rule as every other resource here: it has to
		 * be one of ours, because what the library needs is the Gallium resource
		 * behind it and a buffer from the kernel path (or from another driver) has
		 * none. -mesaBuffer is nil for such a buffer, and nil here would be a draw
		 * the library reads as non-indexed -- the vertices in the wrong order, which
		 * is the one outcome worse than an error. So it is caught where the mapping
		 * fails and not left to the library, which has no way to tell the two apart. */
		.index_buffer = [work->indexBuffer mesaBuffer],
		.index_type = work->indexType,
		.index_buffer_offset = work->indexBufferOffset,
		.base_vertex = work->baseVertex,
		.instance_count = work->instanceCount,
		.base_instance = work->baseInstance,
		/* Metal's own table for each stage, at Metal's own indices: the offsets the
		 * arrays above have nowhere to put. The library takes an argument buffer's
		 * whole binding from here and a buffer argument's offset only, so the two
		 * readings cannot disagree -- and where both name a buffer it checks that
		 * they name the same one. */
		.vertex_buffer_table = vertex_table,
		.vertex_buffer_table_count = work->vertexBufferCount,
		.fragment_buffer_table = fragment_table,
		.fragment_buffer_table_count = work->count,
		/* One entry per colour attachment, NULL where the pass named no resolve target:
		 * the library reads the array and skips the holes, so an ordinary pass costs it
		 * one loop over its attachments and nothing else. */
		.resolve = resolve,
	};

	if (work->indexBuffer != nil && draw.index_buffer == NULL) {
		snprintf(why, size, "the index buffer of this draw did not come from the "
		         "compiler library, so there is nothing to read the indices out of");
		[self failWith:why];
		return NO;
	}

	/* The call the watch thread watches: a draw that stops moving is inside this one, and the
	 * dump it asks for names the kernel call that thread is asleep in. One dump per draw comes
	 * from the serial, which is bumped here and compared there. */
	__atomic_store_n(&rw_draw_serial, __atomic_add_fetch(&rw_draw_serial, 1, __ATOMIC_RELAXED),
	    __ATOMIC_RELEASE);
	__atomic_store_n(&rw_draw_began_us, rw_now_us(), __ATOMIC_RELEASE);
	uint64_t rd_t1 = mach_absolute_time();
	int rd_rc = mesa.render_draw([work->render render], &draw, why, size);
	uint64_t rd_t2 = mach_absolute_time();
	__atomic_store_n(&rw_draw_began_us, 0, __ATOMIC_RELEASE);

	/* Said when this draw announced itself above, and also when it was slow without doing so:
	 * the pair is what makes a missing `rd out` mean a draw that did not return. `binds` is the
	 * time spent filling the bindings before the call, `draw` is the call itself, `total` is both
	 * plus this method's own entry, and `slow` repeats the 50 ms test so the line says why it is
	 * here without the reader comparing two thresholds. */
	uint64_t rd_draw_us = rw_ticks_us(rd_t2 - rd_t1);

	if (rd_said_in || rd_draw_us > RW_DRAW_SLOW_US) {
		rw_print("rd out %u %u %u %u %u %p binds=%llu draw=%llu total=%llu slow=%u\n",
		    commit_count, work->count, work->textureCount, work->vertexBufferCount,
		    work->colorCount, (void *)[work->render render],
		    (unsigned long long)rw_ticks_us(rd_t1 - rd_t0),
		    (unsigned long long)rd_draw_us, (unsigned long long)rw_ticks_us(rd_t2 - rd_t0),
		    rd_draw_us > RW_DRAW_SLOW_US ? 1u : 0u);
	}

	if (rd_rc != 0) {
		rw_print("the draw failed: %s\n", why);
		rw_dump_refused_air([work->render render]);
		[self failWith:why];
		return NO;
	}
	return YES;
}

- (void)failWith:(const char *)reason
{
    rw_print("command buffer failed: %s\n", reason);
	_error = [[NSError errorWithDomain:@"RedwoodMetal" code:4
				  userInfo:@{ NSLocalizedDescriptionKey:
					      [NSString stringWithUTF8String:reason] }] retain];
	_status = RW_CB_ERROR;
}

/* The fence is the whole point: a dispatch that the GPU does not finish must not read as a
 * result. air_run learned that the expensive way (docs/air.md section 5). */
- (void)waitUntilCompleted
{
	char why[512] = { 0 };

	if (_status == RW_CB_ERROR) {
		return;
	}

	/* Recorded before the call and cleared after it, so a wait that never returns leaves
	 * its record standing and the watch thread says so. The fields go in before the slot,
	 * because the slot is what the watch thread reads to decide the record is complete. */
	__atomic_add_fetch(&hb_wait, 1, __ATOMIC_RELAXED);

	unsigned slot = __atomic_add_fetch(&fence_waits, 1, __ATOMIC_ACQ_REL);

	fence_frame = commit_count;
	fence_owner = (const void *)self;
	fence_tid = (unsigned)pthread_mach_thread_np(pthread_self());
	fence_began_us = rw_now_us();
	__atomic_store_n(&fence_wait, slot, __ATOMIC_RELEASE);

	if (mesa.finish([[_queue device] mesaDevice], why, sizeof(why)) != 0) {
		__atomic_store_n(&fence_wait, 0, __ATOMIC_RELEASE);
		rw_print("THE GPU DID NOT FINISH: %s\n", why);
		_error = [[NSError errorWithDomain:@"RedwoodMetal" code:3
					  userInfo:@{ NSLocalizedDescriptionKey:
						      [NSString stringWithUTF8String:why] }] retain];
		_status = RW_CB_ERROR;
		return;
	}
	__atomic_store_n(&fence_wait, 0, __ATOMIC_RELEASE);
	_status = RW_CB_COMPLETED;   /* the fence has been paid, and only now */
}

- (void)waitUntilScheduled { }

/*
 * HANDLERS, AND WHERE THEY RUN.
 *
 * Both of these used to call the block the moment it was added -- which is before the work
 * the block is about, on whichever thread encoded it. That was defensible while the only
 * caller was this port's own test; it is not once backboardd is the caller, and it is the
 * other half of why the system sends -[MTLCommandQueueSPI setCompletionQueue:]: a daemon
 * says WHERE its completion handlers may run precisely because it must not have them on its
 * own thread.
 *
 * So handlers are kept and delivered where the work happens: -commit. A scheduled handler
 * is delivered as soon as the recorded work has been submitted, a completed handler after
 * the GPU fence has been paid for -- the fence is only taken when somebody is actually
 * waiting on one, because this driver's -commit is otherwise asynchronous and a fence
 * nobody asked for would cost every frame. A handler added after the fact is delivered at
 * once rather than dropped: Metal calls that undefined, and losing it would be the kind of
 * silence this driver is trying to stop having.
 */
- (void)redwoodDeliver:(void (^)(id))handler
{
	dispatch_queue_t queue = [_queue redwoodCompletionQueue];

	if (queue != nil) {
		/* dispatch_async copies the block, which retains both this buffer and the
		 * handler it carries, so the buffer outlives its own completion. */
		dispatch_async(queue, ^{ handler(self); });
		return;
	}
	handler(self);
}

- (void)addScheduledHandler:(void (^)(id))handler
{
	if (handler == NULL) {
		return;
	}
	/* scheduled or further, an error included (3, 4, 5): the work this handler is about has
	 * already run, so it is delivered rather than kept */
	if (_status >= RW_CB_SCHEDULED) {
		[self redwoodDeliver:handler];
		return;
	}
	if (_scheduled == nil) {
		_scheduled = [[NSMutableArray alloc] init];
	}
	[_scheduled addObject:[[handler copy] autorelease]];
}

- (void)addCompletedHandler:(void (^)(id))handler
{
	if (handler == NULL) {
		return;
	}
	if (_status >= RW_CB_SCHEDULED) {
		[self redwoodDeliver:handler];
		return;
	}
	if (_completed == nil) {
		_completed = [[NSMutableArray alloc] init];
	}
	[_completed addObject:[[handler copy] autorelease]];
}

/* Called from the one place the work is done. The order is Metal's: scheduled first, then
 * the fence, then completed. */
- (void)redwoodDeliverHandlers
{
	NSArray *scheduled = _scheduled;
	NSArray *completed = _completed;

	_scheduled = nil;
	_completed = nil;
	for (id handler in scheduled) {
		[self redwoodDeliver:(void (^)(id))handler];
	}
	if ([completed count] > 0 && _status != RW_CB_ERROR) {
		/* A completed handler is a promise about the GPU, so it is made true before it
		 * is kept: -waitUntilCompleted is this driver's fence and sets the status the
		 * handler will read. */
		[self waitUntilCompleted];
	}
	for (id handler in completed) {
		[self redwoodDeliver:(void (^)(id))handler];
	}
	[scheduled release];
	[completed release];
}

/* -commitWithDeadline: is an abstract entry of _MTLCommandBuffer, so inheriting it aborts
 * rather than defaulting. The deadline is for work that is worthless if it is late (a frame
 * that misses its vsync); Apple's drivers hand it to the GPU's scheduler. There is one
 * scheduler here and one priority, so the deadline cannot be honoured -- but the COMMIT
 * must happen, and a deadline that is merely unmet is a late frame while a commit that
 * never happens is a daemon waiting for ever. */
- (void)commitWithDeadline:(NSUInteger)deadline
{
	(void)deadline;
	RW_ONCE("a command buffer was committed with a deadline; this driver "
		"has one ring and no scheduler to tell, so the work is committed and the "
		"deadline is not promised\n");
	[self commit];
}

- (void)addPurgedResource:(id)resource { (void)resource; [self redwoodPurgedNote]; }
- (void)addPurgedHeap:(id)heap         { (void)heap; [self redwoodPurgedNote]; }

/* Metal records on a command buffer every resource of its that was purged while the buffer
 * referenced it, so that -commit can fail the buffer instead of reading pages that are
 * gone. Nothing of this driver's is ever volatile -- -setPurgeableState: answers
 * MTLPurgeableStateNonVolatile for every resource, which is what they are -- so a report
 * here means Metal and this driver disagree about a resource, and that is worth a line
 * rather than a silent buffer that fails later for no stated reason. */
- (void)redwoodPurgedNote
{
	RW_ONCE("Metal reported a purged resource on a command buffer, but "
		"nothing this driver allocates is volatile; the report is noted and the "
		"buffer is not failed\n");
}


@end

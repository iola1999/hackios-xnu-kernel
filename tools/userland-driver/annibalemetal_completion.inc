/*
 * Completion on a thread of its own, as Metal does it: -commit hands the work to the GPU and
 * returns, and the command buffer completes when the GPU has done it.
 *
 * -commit used to wait for the GPU itself whenever a completed handler was attached, and every
 * buffer CoreAnimation and RenderBox commit carries one, so encoding and the GPU never overlapped;
 * and a handler with no completion queue ran on the committing thread inside -commit, before the
 * caller had finished its own bookkeeping for the buffer.
 *
 * Now -commit takes the fence of its submission (rwa_fence_take) and queues the buffer here. One
 * thread per process waits for the fences in queue order, without the library's lock, writes back
 * the textures laid over buffers by copy, marks the buffer completed and wakes -waitUntilCompleted.
 * Handlers run on the queue the caller named (-[MTLCommandQueueSPI setCompletionQueue:]) or on one
 * serial queue of this driver's, never on the completion thread: a handler that waits for a later
 * buffer would otherwise wait for itself. Scheduled handlers go to the same queue at -commit, so
 * every buffer's scheduled handlers come before its completed ones.
 */

struct rw_completion {
	struct rw_completion *next;
	AnnibaleMetalCommandBuffer *buffer;      /* retained until it has completed */
	struct rwa_fence *fence;                /* NULL: no retained asynchronous fence */
	unsigned commit;
	uint64_t queued_us;
};

static pthread_mutex_t rw_completion_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rw_completion_more = PTHREAD_COND_INITIALIZER;   /* a buffer was queued */
static pthread_cond_t rw_completion_done = PTHREAD_COND_INITIALIZER;   /* a buffer completed */
static struct rw_completion *rw_completion_head, **rw_completion_tail = &rw_completion_head;
static pthread_t rw_completion_tid;

/* A processor read of an IOSurface must see what the GPU wrote into it. The commit path records
 * which commit touched every surface-bound texture it had (reads over-marked: waiting on one is
 * a fence that has already fired); a CPU reader asks annibale_surface_writes_wait to stand on
 * the far side of that commit before it maps the surface. This is how the still pipeline's first
 * photograph came out black: the encoder read the engine's output surface while the commit that
 * wrote it was still in flight (kernel-12377-say16-ubo12). */
#define RW_SURFACE_WRITES 256
static struct { uint32_t sid; unsigned commit; } rw_surface_writes[RW_SURFACE_WRITES];
static unsigned rw_completed_commit;

static void
rw_surface_commit_mark(id texture, unsigned commit)
{
	if (texture == nil) {
		return;
	}
	void *sf = [texture respondsToSelector:sel_registerName("iosurface")] ?
	    ((void *(*)(id, SEL))objc_msgSend)(texture, sel_registerName("iosurface")) : NULL;
	if (sf == NULL) {
		return;
	}
	uint32_t sid = ann_surface_id(sf);
	pthread_mutex_lock(&rw_completion_lock);
	for (;;) {
		unsigned slot = RW_SURFACE_WRITES, reclaim = RW_SURFACE_WRITES, oldest = 0;
		for (unsigned i = 0; i < RW_SURFACE_WRITES; i++) {
			if (rw_surface_writes[i].sid == sid) {
				slot = i;
				break;
			}
			if (reclaim == RW_SURFACE_WRITES &&
			    (rw_surface_writes[i].sid == 0 ||
			     rw_surface_writes[i].commit <= rw_completed_commit)) {
				reclaim = i;
			}
			if (rw_surface_writes[i].commit < rw_surface_writes[oldest].commit) {
				oldest = i;
			}
		}
		if (slot == RW_SURFACE_WRITES) {
			slot = reclaim;
		}
		if (slot < RW_SURFACE_WRITES) {
			rw_surface_writes[slot].sid = sid;
			rw_surface_writes[slot].commit = commit;
			break;
		}
		/* Table full of genuinely in-flight writes: wait for the oldest before reusing it. */
		unsigned wait_target = rw_surface_writes[oldest].commit;
		if (pthread_equal(pthread_self(), rw_completion_tid) ||
		    rw_completed_commit >= wait_target) {
			rw_surface_writes[oldest].sid = sid;
			rw_surface_writes[oldest].commit = commit;
			break;
		}
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 4;
		while (rw_completed_commit < wait_target) {
			if (pthread_cond_timedwait(&rw_completion_done, &rw_completion_lock,
			                           &deadline) != 0) {
				rw_surface_writes[oldest].sid = sid;
				rw_surface_writes[oldest].commit = commit;
				goto done;
			}
		}
	}
done:
	pthread_mutex_unlock(&rw_completion_lock);
}

void
annibale_surface_writes_wait(uint32_t sid)
{
	if (pthread_equal(pthread_self(), rw_completion_tid)) {
		return;   /* the completion thread cannot wait on the work it is announcing */
	}
	pthread_mutex_lock(&rw_completion_lock);
	unsigned target = 0;
	for (unsigned i = 0; i < RW_SURFACE_WRITES; i++) {
		if (rw_surface_writes[i].sid == sid) {
			target = rw_surface_writes[i].commit;
			break;
		}
	}
	if (target != 0) {
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 4;
		while (rw_completed_commit < target) {
			if (pthread_cond_timedwait(&rw_completion_done, &rw_completion_lock, &deadline) != 0) {
				break;
			}
		}
	}
	pthread_mutex_unlock(&rw_completion_lock);
}

static dispatch_queue_t
rw_handler_queue(void)
{
	static dispatch_queue_t queue;
	static dispatch_once_t once;

	dispatch_once(&once, ^{ queue = dispatch_queue_create("annibale.metal.handlers", DISPATCH_QUEUE_SERIAL); });
	return queue;
}

static void *rw_completion_main(void *unused);

@implementation AnnibaleMetalCommandBuffer (Completion)

- (void)annibaleDeliverLater:(NSArray *)handlers
{
	dispatch_queue_t queue = [_queue annibaleCompletionQueue];

	if ([handlers count] == 0) {
		return;
	}
	dispatch_async(queue != nil ? queue : rw_handler_queue(), ^{
		for (id handler in handlers) {
			((void (^)(id))handler)(self);
		}
	});
}

/* Takes the retained submission fence from -commit. Handlers remain asynchronous. */
- (void)annibaleCompleteLater:(unsigned)commit fence:(struct rwa_fence *)fence
{
	static dispatch_once_t once;
	static BOOL worker_ready;
	struct rw_completion *item = calloc(1, sizeof(*item));
	NSArray *scheduled;

	dispatch_once(&once, ^{
		pthread_t thread;
		if (pthread_create(&thread, NULL, rw_completion_main, NULL) == 0) {
			pthread_detach(thread);
			worker_ready = YES;
		}
	});
	pthread_mutex_lock(&rw_completion_lock);
	if (__atomic_load_n(&_status, __ATOMIC_ACQUIRE) < RW_CB_SCHEDULED) {
		__atomic_store_n(&_status, RW_CB_SCHEDULED, __ATOMIC_RELEASE);
	}
	scheduled = _scheduled;
	_scheduled = nil;
	__atomic_store_n(&_phase, RW_CB_PHASE_SCHEDULED, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&rw_completion_lock);
	[self annibaleDeliverLater:scheduled];
	[scheduled release];
	if (item == NULL || !worker_ready) {
		AnnibaleMetalCommandBuffer *buffer = [self retain];

		rw_print("cannot queue a command buffer for completion; waiting for its submission\n");
		free(item);
		[buffer annibaleCompleteWithFence:fence commit:commit queued:rw_now_us()];
		[buffer release];
		return;
	}
	item->buffer = [self retain];
	item->commit = commit;
	item->queued_us = rw_now_us();
	item->fence = fence;
	pthread_mutex_lock(&rw_completion_lock);
	*rw_completion_tail = item;
	rw_completion_tail = &item->next;
	pthread_cond_signal(&rw_completion_more);
	pthread_mutex_unlock(&rw_completion_lock);
}

/* On the completion thread. */
- (void)annibaleCompleteWithFence:(struct rwa_fence *)fence commit:(unsigned)commit queued:(uint64_t)queued_us
{
	struct rwa_device *device = [[_queue device] mesaDevice];
	NSArray *completed;
	uint64_t began_us = rw_now_us(), fence_us, writeback_us = 0;

	char why[256] = { 0 };
	BOOL wait_failed = NO;
	bool waiting_gpu = (fence == NULL && mesa.fence_take == NULL && mesa.finish != NULL) ||
	    fence != NULL;

	if (waiting_gpu) {
		unsigned slot = __atomic_add_fetch(&fence_waits, 1, __ATOMIC_ACQ_REL);
		__atomic_add_fetch(&hb_wait, 1, __ATOMIC_RELAXED);
		fence_frame = commit;
		fence_owner = (const void *)self;
		fence_tid = (unsigned)pthread_mach_thread_np(pthread_self());
		fence_began_us = began_us;
		__atomic_store_n(&fence_wait, slot, __ATOMIC_RELEASE);
	}
	/* A compiler library without the fence calls: wait for everything, as -commit used to. */
	if (fence == NULL && mesa.fence_take == NULL && mesa.finish != NULL &&
	    mesa.finish(device, why, sizeof(why)) != 0) {
		rw_print("THE GPU DID NOT FINISH: %s\n", why);
		wait_failed = YES;
	}
	if (fence != NULL && mesa.fence_wait(device, fence) != 0) {
		rw_print("THE GPU DID NOT FINISH: a command buffer's fence did not signal in time\n");
		wait_failed = YES;
	}
	if (wait_failed) {
		if (_error == nil) {
			_error = [[NSError errorWithDomain:@"AnnibaleMetal" code:3
					  userInfo:@{ NSLocalizedDescriptionKey:
						      @"the GPU did not finish this command buffer in time" }] retain];
		}
		_failed = YES;
	}
	if (waiting_gpu) {
		__atomic_store_n(&fence_wait, 0, __ATOMIC_RELEASE);
	}
	fence_us = rw_now_us() - began_us;
	if (fence != NULL) {
		mesa.fence_release(device, fence);
	}
	if (!_failed && mesa.buffer_textures_writeback != NULL) {
		uint64_t writeback_began = rw_now_us();

		mesa.buffer_textures_writeback(device);
		writeback_us = rw_now_us() - writeback_began;
	}
	/* Still-capture triage (A1 first-shot dark): after the fence, a compute work that
	 * touched an IOSurface gets every internal texture it bound sampled at the centre
	 * texel -- written-vs-unwritten separates "the fusion read an empty input" from
	 * "the fusion read a populated input and still wrote zeros". */
	static unsigned probe_budget;
	if (!wait_failed && probe_budget < 128) {
		for (AnnibaleMetalWork *work in _work) {
			if (work->pipeline == nil || work->textureCount == 0)
				continue;
			BOOL touches_surface = NO;
			for (unsigned i = 0; i < work->textureCount && !touches_surface; i++) {
				id t = work->textures[i];
				void *sf = [t respondsToSelector:sel_registerName("iosurface")] ?
				    ((void *(*)(id, SEL))objc_msgSend)(t, sel_registerName("iosurface")) : NULL;
				touches_surface = sf != NULL;
			}
			if (!touches_surface)
				continue;
			const char *pname = "?";
			if ([work->pipeline respondsToSelector:sel_registerName("annibaleKernelName")]) {
				NSString *kn = ((id (*)(id, SEL))objc_msgSend)(
				    work->pipeline, sel_registerName("annibaleKernelName"));
				if ([kn isKindOfClass:[NSString class]] && [kn length] != 0)
					pname = [kn UTF8String];
			}
			for (unsigned i = 0; i < work->textureCount && probe_budget < 128; i++) {
				id t = work->textures[i];
				if (t == nil || ![t respondsToSelector:@selector(getBytes:bytesPerRow:fromRegion:mipmapLevel:)])
					continue;
				unsigned w = (unsigned)[t width], h = (unsigned)[t height];
				if (w == 0 || h == 0)
					continue;
				unsigned char sample[256] = { 0 };
				MTLRegion region = MTLRegionMake3D(w / 2, h / 2, 0, 4, 8, 1);
				probe_budget++;
				[t getBytes:sample bytesPerRow:32 fromRegion:region
				mipmapLevel:0];
				unsigned nz = 0, mx = 0, sum = 0;
				for (unsigned b = 0; b < sizeof(sample); b++) {
					nz += sample[b] != 0;
					mx = sample[b] > mx ? sample[b] : mx;
					sum += sample[b];
				}
				void *sf = [t respondsToSelector:sel_registerName("iosurface")] ?
				    ((void *(*)(id, SEL))objc_msgSend)(t, sel_registerName("iosurface")) : NULL;
				rw_print("probe %s commit %u tex%u %s%u %ux%u nz=%u mx=%u sum=%u\n", pname, commit, i,
					 sf != NULL ? "s" : "t",
					 sf != NULL ? ann_surface_id(sf) : (unsigned)(uintptr_t)t,
					 w, h, nz, mx, sum);
			}
		}
	}
	if (!wait_failed) {
		for (AnnibaleMetalWork *work in _work) {
			if (work->copyKind == RW_COPY_SIGNAL_EVENT &&
			    [work->sharedEvent respondsToSelector:@selector(setSignaledValue:)]) {
				[(id<AnnibaleSharedEventSPI>)work->sharedEvent
				    setSignaledValue:work->sharedEventValue];
			}
		}
	}
	pthread_mutex_lock(&rw_completion_lock);
	__atomic_store_n(&_status, _failed ? RW_CB_ERROR : RW_CB_COMPLETED, __ATOMIC_RELEASE);
	completed = _completed;
	_completed = nil;
	__atomic_store_n(&_phase, RW_CB_PHASE_FINISHED, __ATOMIC_RELEASE);
	rw_completed_commit = commit;
	pthread_cond_broadcast(&rw_completion_done);
	pthread_mutex_unlock(&rw_completion_lock);
	rw_timing_completion(commit, began_us - queued_us, fence_us, writeback_us, !_failed);
	[self annibaleDeliverLater:completed];
	[completed release];
}

/* -addScheduledHandler:. Kept until -commit finishes encoding; delivered at once afterwards. */
- (void)annibaleAddScheduled:(void (^)(id))handler
{
	pthread_mutex_lock(&rw_completion_lock);
	if (__atomic_load_n(&_phase, __ATOMIC_ACQUIRE) < RW_CB_PHASE_SCHEDULED) {
		if (_scheduled == nil) {
			_scheduled = [[NSMutableArray alloc] init];
		}
		[_scheduled addObject:[[handler copy] autorelease]];
		handler = nil;
	}
	pthread_mutex_unlock(&rw_completion_lock);
	if (handler != nil) {
		[self annibaleDeliverLater:@[ [[handler copy] autorelease] ]];
	}
}

/* -addCompletedHandler:. Kept until completion; delivered at once to a buffer already complete. */
- (void)annibaleAddCompleted:(void (^)(id))handler
{
	pthread_mutex_lock(&rw_completion_lock);
	if (__atomic_load_n(&_phase, __ATOMIC_ACQUIRE) < RW_CB_PHASE_FINISHED) {
		if (_completed == nil) {
			_completed = [[NSMutableArray alloc] init];
		}
		[_completed addObject:[[handler copy] autorelease]];
		handler = nil;
	}
	pthread_mutex_unlock(&rw_completion_lock);
	if (handler != nil) {
		[self annibaleDeliverLater:@[ [[handler copy] autorelease] ]];
	}
}

/* -waitUntilCompleted for a committed buffer: YES once it has completed or failed. NO for one
 * never committed, which the caller waits for the old way. */
- (BOOL)annibaleAwaitCompletion
{
	pthread_mutex_lock(&rw_completion_lock);
	if (__atomic_load_n(&_phase, __ATOMIC_ACQUIRE) < RW_CB_PHASE_COMMITTED) {
		pthread_mutex_unlock(&rw_completion_lock);
		return NO;
	}
	while (__atomic_load_n(&_phase, __ATOMIC_ACQUIRE) < RW_CB_PHASE_FINISHED) {
		pthread_cond_wait(&rw_completion_done, &rw_completion_lock);
	}
	pthread_mutex_unlock(&rw_completion_lock);
	return YES;
}

@end

static void *
rw_completion_main(void *unused)
{
	(void)unused;
	pthread_setname_np("annibale.metal.completion");
	rw_completion_tid = pthread_self();
	for (;;) {
		struct rw_completion *item;

		pthread_mutex_lock(&rw_completion_lock);
		while (rw_completion_head == NULL) {
			pthread_cond_wait(&rw_completion_more, &rw_completion_lock);
		}
		item = rw_completion_head;
		pthread_mutex_unlock(&rw_completion_lock);
		@autoreleasepool {
			[item->buffer annibaleCompleteWithFence:item->fence commit:item->commit queued:item->queued_us];
		}
		pthread_mutex_lock(&rw_completion_lock);
		rw_completion_head = item->next;
		if (rw_completion_head == NULL) {
			rw_completion_tail = &rw_completion_head;
		}
		pthread_mutex_unlock(&rw_completion_lock);
		[item->buffer release];
		free(item);
	}
	return NULL;
}

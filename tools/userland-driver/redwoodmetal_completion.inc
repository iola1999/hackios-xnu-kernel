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
 * thread per process waits for the fences in commit order, without the library's lock, writes back
 * the textures laid over buffers by copy, marks the buffer completed and wakes -waitUntilCompleted.
 * Handlers run on the queue the caller named (-[MTLCommandQueueSPI setCompletionQueue:]) or on one
 * serial queue of this driver's, never on the completion thread: a handler that waits for a later
 * buffer would otherwise wait for itself. Scheduled handlers go to the same queue at -commit, so
 * every buffer's scheduled handlers come before its completed ones.
 */

struct rw_completion {
	struct rw_completion *next;
	RedwoodMetalCommandBuffer *buffer;      /* retained until it has completed */
	struct rwa_fence *fence;                /* NULL: nothing was handed to the GPU */
};

static pthread_mutex_t rw_completion_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rw_completion_more = PTHREAD_COND_INITIALIZER;   /* a buffer was queued */
static pthread_cond_t rw_completion_done = PTHREAD_COND_INITIALIZER;   /* a buffer completed */
static struct rw_completion *rw_completion_head, **rw_completion_tail = &rw_completion_head;

static dispatch_queue_t
rw_handler_queue(void)
{
	static dispatch_queue_t queue;
	static dispatch_once_t once;

	dispatch_once(&once, ^{ queue = dispatch_queue_create("redwood.metal.handlers", DISPATCH_QUEUE_SERIAL); });
	return queue;
}

static void *rw_completion_main(void *unused);

@implementation RedwoodMetalCommandBuffer (Completion)

- (void)redwoodDeliverLater:(NSArray *)handlers
{
	dispatch_queue_t queue = [_queue redwoodCompletionQueue];

	if ([handlers count] == 0) {
		return;
	}
	dispatch_async(queue != nil ? queue : rw_handler_queue(), ^{
		for (id handler in handlers) {
			((void (^)(id))handler)(self);
		}
	});
}

/* From -commit, after the submission: the scheduled handlers now, the rest when the GPU is done. */
- (void)redwoodCompleteLater
{
	static dispatch_once_t once;
	struct rw_completion *item = calloc(1, sizeof(*item));
	NSArray *scheduled;

	dispatch_once(&once, ^{
		pthread_t thread;
		if (pthread_create(&thread, NULL, rw_completion_main, NULL) == 0) {
			pthread_detach(thread);
		}
	});
	pthread_mutex_lock(&rw_completion_lock);
	scheduled = _scheduled;
	_scheduled = nil;
	pthread_mutex_unlock(&rw_completion_lock);
	[self redwoodDeliverLater:scheduled];
	[scheduled release];
	if (item == NULL) {
		rw_print("no memory to queue a command buffer for completion\n");
		return;
	}
	item->buffer = [self retain];
	item->fence = _status == RW_CB_ERROR || mesa.fence_take == NULL ? NULL :
	    mesa.fence_take([[_queue device] mesaDevice]);
	pthread_mutex_lock(&rw_completion_lock);
	*rw_completion_tail = item;
	rw_completion_tail = &item->next;
	pthread_cond_signal(&rw_completion_more);
	pthread_mutex_unlock(&rw_completion_lock);
}

/* On the completion thread. */
- (void)redwoodCompleteWithFence:(struct rwa_fence *)fence
{
	struct rwa_device *device = [[_queue device] mesaDevice];
	NSArray *completed;

	char why[256] = { 0 };

	/* A compiler library without the fence calls: wait for everything, as -commit used to. */
	if (fence == NULL && mesa.fence_take == NULL && _status != RW_CB_ERROR && mesa.finish != NULL &&
	    mesa.finish(device, why, sizeof(why)) != 0) {
		rw_print("THE GPU DID NOT FINISH: %s\n", why);
		_status = RW_CB_ERROR;
	}
	if (fence != NULL && mesa.fence_wait(device, fence) != 0) {
		rw_print("THE GPU DID NOT FINISH: a command buffer's fence did not signal in time\n");
		_error = [[NSError errorWithDomain:@"RedwoodMetal" code:3
					  userInfo:@{ NSLocalizedDescriptionKey:
						      @"the GPU did not finish this command buffer in time" }] retain];
		_status = RW_CB_ERROR;
	}
	if (fence != NULL) {
		mesa.fence_release(device, fence);
	}
	if (_status != RW_CB_ERROR && mesa.buffer_textures_writeback != NULL) {
		mesa.buffer_textures_writeback(device);
	}
	pthread_mutex_lock(&rw_completion_lock);
	if (_status != RW_CB_ERROR) {
		_status = RW_CB_COMPLETED;
	}
	completed = _completed;
	_completed = nil;
	pthread_cond_broadcast(&rw_completion_done);
	pthread_mutex_unlock(&rw_completion_lock);
	[self redwoodDeliverLater:completed];
	[completed release];
}

/* -addCompletedHandler:. Kept until completion; delivered at once to a buffer already complete. */
- (void)redwoodAddCompleted:(void (^)(id))handler
{
	pthread_mutex_lock(&rw_completion_lock);
	if (_status < RW_CB_COMPLETED) {
		if (_completed == nil) {
			_completed = [[NSMutableArray alloc] init];
		}
		[_completed addObject:[[handler copy] autorelease]];
		handler = nil;
	}
	pthread_mutex_unlock(&rw_completion_lock);
	if (handler != nil) {
		[self redwoodDeliverLater:@[ [[handler copy] autorelease] ]];
	}
}

/* -waitUntilCompleted for a committed buffer: YES once it has completed or failed. NO for one
 * never committed, which the caller waits for the old way. */
- (BOOL)redwoodAwaitCompletion
{
	if (_status < RW_CB_COMMITTED) {
		return NO;
	}
	pthread_mutex_lock(&rw_completion_lock);
	while (_status < RW_CB_COMPLETED) {
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
	pthread_setname_np("redwood.metal.completion");
	for (;;) {
		struct rw_completion *item;

		pthread_mutex_lock(&rw_completion_lock);
		while (rw_completion_head == NULL) {
			pthread_cond_wait(&rw_completion_more, &rw_completion_lock);
		}
		item = rw_completion_head;
		pthread_mutex_unlock(&rw_completion_lock);
		@autoreleasepool {
			[item->buffer redwoodCompleteWithFence:item->fence];
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

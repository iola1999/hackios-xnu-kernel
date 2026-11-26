/* Commit execution retains the existing work order and completion path. */
static void rw_surface_commit_mark(id texture, unsigned commit);   /* annibalemetal_completion.inc */

/*
 * GPU YIELDING, the submission half (the grid cutting is the encoder's half,
 * annibalemetal_compute_argencoder.inc; rw_gpu_yield in annibalemetal_diag.inc says what
 * the pair is for, with the measurements). The kernel's context queue is first-in
 * first-out and a submission in execution cannot be interrupted, so a piece only bounds
 * the compositor's wait if the queue in front of it is short. This is the shortness: a
 * commit that paces waits for the piece it last handed over before it hands over the next
 * one, so at most one piece of this process is ever in the queue or in execution, and a
 * compositor submission landing meanwhile starts behind that one piece rather than behind
 * the whole commit. The fence carries across commits, because the camera's chain is one
 * dispatch a commit as often as it is several: the first piece of the next commit waits
 * for the last piece of this one exactly as the pieces inside it do.
 *
 * The wait holds rw_commit_lock, so a second commit of this process lines up behind the
 * whole paced commit rather than behind one piece of it. That is the price the shape
 * already paid before yielding (the one flush of a commit serialised the same way) plus
 * the piece round trips, and it is only paid by a process whose commit is both large and
 * compute-only -- the compositor's own commits carry draws and never pace. The fence
 * itself completes off the kernel's interrupt path, so nothing the lock holds back can be
 * part of what the wait is for.
 *
 * One fence reference is held here between commits. It is released when the next paced
 * flush replaces or consumes it, and the last one of a process simply stays: one fence.
 */
static struct rwa_fence *rw_yield_fence;
static struct rwa_device *rw_yield_fence_device;

/* Waits out the piece handed over last, if the wait is for this device at all, and
 * answers how long it took. A fence that does not signal is the GPU stuck, which the
 * completion path reports for its own waits; here the piece boundary is simply lost and
 * the commit goes on, no worse than before yielding. */
static uint64_t
rw_yield_wait(struct rwa_device *device)
{
	uint64_t began;

	if (rw_yield_fence == NULL || rw_yield_fence_device != device) {
		return 0;
	}
	began = rw_now_us();
	if (mesa.fence_wait(device, rw_yield_fence) != 0) {
		rw_print("yield: a piece's fence did not signal in time; the pieces of "
		         "this commit go on without the boundary\n");
	}
	mesa.fence_release(device, rw_yield_fence);
	rw_yield_fence = NULL;
	rw_yield_fence_device = NULL;
	return rw_now_us() - began;
}

/* Adopts the fence of the flush just made as the piece the next flush waits for. The
 * reference is the take's own, independent of the one the completion thread holds. */
static void
rw_yield_adopt(struct rwa_device *device)
{
	struct rwa_fence *fence = mesa.fence_take(device);

	if (rw_yield_fence != NULL) {
		mesa.fence_release(rw_yield_fence_device, rw_yield_fence);
	}
	rw_yield_fence = fence;
	rw_yield_fence_device = fence != NULL ? device : NULL;
}

- (void)commit
{
	char why[512] = { 0 };
	uint64_t started = mach_absolute_time();
	uint64_t refresh_us = 0, encode_us = 0, flush_us = 0;
	struct rwa_fence *submission_fence = NULL;
	static dispatch_queue_t defer_q;
	static dispatch_once_t defer_once;
	static unsigned defer_pending;
	static _Thread_local BOOL on_defer_q;

	if (!on_defer_q && __atomic_load_n(&_phase, __ATOMIC_ACQUIRE) == RW_CB_PHASE_RECORDING) {
		BOOL needs_defer = __atomic_load_n(&defer_pending, __ATOMIC_ACQUIRE) != 0;
		for (NSUInteger i = 0; !needs_defer && i < [_work count]; i++) {
			AnnibaleMetalWork *w = [_work objectAtIndex:i];
			if (w->copyKind != RW_COPY_WAIT_EVENT ||
			    ![w->sharedEvent respondsToSelector:@selector(signaledValue)] ||
			    [(id<AnnibaleSharedEventSPI>)w->sharedEvent signaledValue] >= w->sharedEventValue) {
				continue;
			}
			BOOL signaled_earlier = NO;
			for (NSUInteger j = 0; j < i; j++) {
				AnnibaleMetalWork *prev = [_work objectAtIndex:j];
				if (prev->copyKind == RW_COPY_SIGNAL_EVENT &&
				    prev->sharedEvent == w->sharedEvent &&
				    prev->sharedEventValue >= w->sharedEventValue) {
					signaled_earlier = YES;
					break;
				}
			}
			if (!signaled_earlier) {
				needs_defer = YES;
			}
		}
		if (needs_defer) {
			pthread_mutex_lock(&rw_commit_lock);
			if (__atomic_load_n(&_phase, __ATOMIC_ACQUIRE) != RW_CB_PHASE_RECORDING) {
				pthread_mutex_unlock(&rw_commit_lock);
				return;
			}
			__atomic_store_n(&_status, RW_CB_COMMITTED, __ATOMIC_RELEASE);
			__atomic_store_n(&_phase, RW_CB_PHASE_COMMITTED, __ATOMIC_RELEASE);
			[self annibaleReturnSlot];
			__atomic_add_fetch(&defer_pending, 1, __ATOMIC_ACQ_REL);
			pthread_mutex_unlock(&rw_commit_lock);
			dispatch_once(&defer_once, ^{
				defer_q = dispatch_queue_create("annibale.metal.eventwait", DISPATCH_QUEUE_SERIAL);
			});
			AnnibaleMetalCommandBuffer *retained = [self retain];
			dispatch_async(defer_q, ^{
				@autoreleasepool {
					on_defer_q = YES;
					[retained commit];
					on_defer_q = NO;
					__atomic_sub_fetch(&defer_pending, 1, __ATOMIC_ACQ_REL);
					[retained release];
				}
			});
			return;
		}
	}
	if (on_defer_q) {
		for (NSUInteger i = 0; i < [_work count]; i++) {
			AnnibaleMetalWork *w = [_work objectAtIndex:i];
			if (w->copyKind != RW_COPY_WAIT_EVENT ||
			    ![w->sharedEvent respondsToSelector:@selector(waitUntilSignaledValue:timeoutMS:)]) {
				continue;
			}
			BOOL signaled_earlier = NO;
			for (NSUInteger j = 0; j < i; j++) {
				AnnibaleMetalWork *prev = [_work objectAtIndex:j];
				if (prev->copyKind == RW_COPY_SIGNAL_EVENT &&
				    prev->sharedEvent == w->sharedEvent &&
				    prev->sharedEventValue >= w->sharedEventValue) {
					signaled_earlier = YES;
					break;
				}
			}
			if (!signaled_earlier) {
				[(id<AnnibaleSharedEventSPI>)w->sharedEvent
				    waitUntilSignaledValue:w->sharedEventValue timeoutMS:5000];
			}
		}
	}

	pthread_mutex_lock(&rw_commit_lock);
	if (__atomic_load_n(&_phase, __ATOMIC_ACQUIRE) !=
	    (on_defer_q ? RW_CB_PHASE_COMMITTED : RW_CB_PHASE_RECORDING)) {
		pthread_mutex_unlock(&rw_commit_lock);
		return;
	}
	__atomic_add_fetch(&hb_commit, 1, __ATOMIC_RELAXED);

	if (commit_started != 0) {
		/* Two commits at once means two threads in one driver with one ring. Said out loud
		 * because it changes what a slow commit means: the wait may be the other thread's. */
		rw_print("commit %u started while commit %u was still running\n", commit_count + 1,
			 commit_count);
	}
	uint64_t prev_started = commit_started;
	unsigned prev_in_flight = commit_in_flight;
	commit_started = started;
	unsigned this_commit = ++commit_count;
	/* Every surface-bound texture this commit touches is recorded so a processor-side reader
	 * can wait for the write (annibale_surface_writes_wait): the still pipeline's encoder
	 * reads the engine's IOSurface output on the CPU, and without this the read can land
	 * while the write is still in flight -- the first shot of a session came out black. */
	for (AnnibaleMetalWork *mark in _work) {
		for (unsigned i = 0; i < mark->textureCount; i++)
			rw_surface_commit_mark(mark->textures[i], this_commit);
		for (unsigned i = 0; i < mark->vertexTextureCount; i++)
			rw_surface_commit_mark(mark->vertexTextures[i], this_commit);
		for (unsigned i = 0; i < mark->colorCount; i++)
			rw_surface_commit_mark(mark->color[i], this_commit);
		for (unsigned i = 0; i < 8; i++)
			rw_surface_commit_mark(mark->resolve[i], this_commit);
		rw_surface_commit_mark(mark->depth, this_commit);
	}
	pthread_once(&rw_watch_once, rw_start_watch);
	commit_progress_us = rw_now_us();
	commit_in_flight = commit_count;
	__atomic_store_n(&_status, RW_CB_COMMITTED, __ATOMIC_RELEASE);
	__atomic_store_n(&_phase, RW_CB_PHASE_COMMITTED, __ATOMIC_RELEASE);
	/* Textures laid over buffers get the callers' bytes copied into them again: the
	 * processor may have written those buffers since the last -commit, and Metal's own
	 * promise is that a texture over a buffer reads what is there now.
	 */
	if (mesa.buffer_textures_refresh != NULL) {
		uint64_t refresh_began = mach_absolute_time();

		mesa.buffer_textures_refresh([[_queue device] mesaDevice]);
		refresh_us = rw_ticks_us(mach_absolute_time() - refresh_began);
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

		for (AnnibaleMetalWork *work in _work) {
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
	NSMutableArray *rgb2pOpen = nil;

	/* Whether this commit paces (the rw_yield_* notes above): it does when it carries no
	 * draw at all and its compute dispatches total at least the size one piece is cut at.
	 * A draw in the mix is the compositor's shape of commit, which is exactly the work
	 * the yield makes room for; pacing it would be the yield backwards. The total is over
	 * the pieces of cut dispatches as they are recorded, which sums back to the grid
	 * they were cut from. */
	struct rwa_device *yield_device = [[_queue device] mesaDevice];
	BOOL pacing = rw_gpu_yield() && mesa.flush != NULL && mesa.fence_take != NULL &&
	    mesa.fence_wait != NULL && mesa.fence_release != NULL;
	uint64_t commit_threads = 0, yield_waited_us = 0;
	unsigned yield_flushes = 0;

	if (pacing) {
		for (AnnibaleMetalWork *scan in _work) {
			if (scan->render != nil) {
				pacing = NO;
				break;
			}
			if (scan->copyKind == RW_COPY_NONE && scan->pipeline != nil) {
				uint64_t t = 1;

				for (unsigned i = 0; i < 3; i++) {
					t *= (uint64_t)scan->groups[i] * scan->size[i];
				}
				commit_threads += t;
			}
		}
		pacing = pacing && commit_threads >= RW_YIELD_THREADS;
	}

	rw_thumb_commit(_work, commit_count);
	rw_audit_commit_begin(_work, commit_count);
	for (AnnibaleMetalWork *work in _work) {
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

		if (work->copyKind == RW_COPY_SIGNAL_EVENT ||
		    work->copyKind == RW_COPY_WAIT_EVENT) {
			continue;
		}
		if (work->copyKind != RW_COPY_NONE) {
			if (rw_run_copy(work, why, sizeof(why)) != 0) {
				[self failWith:why];
				goto done;
			}
			if (work->copyKind == RW_COPY_RGB2P_BEGIN) {
				if (!rgb2pOpen) rgb2pOpen = [NSMutableArray array];
				[rgb2pOpen addObject:work->textures[0]];
			} else if (work->copyKind == RW_COPY_RGB2P_END) {
				[rgb2pOpen removeObjectIdenticalTo:work->textures[0]];
			}
			rw_work_time(RW_WORK_COPY, work_began);
			continue;
		}
		if (work->render != nil) {
			bool matched = rw_take_matched_draw(work, commit_count, work_index);
			bool sync = matched || rw_sync_draws();

			if (matched && getenv("ANNIBALE_REPLAY_VERTEX") != NULL) {
				/* rw_take_matched_draw requires both synchronization methods. */
				struct rwa_device *device = [[_queue device] mesaDevice];
				char reason[256] = {0};
				if (mesa.flush(device, reason, sizeof(reason)) != 0)
					rw_print("vertex capture: flush failed: %s\n", reason);
				else if (mesa.finish(device, reason, sizeof(reason)) != 0)
					rw_print("vertex capture: preceding work did not finish: %s\n", reason);
				else
					rw_capture_vertex(work, [work->render render], commit_count, work_index);
			}
			bool sample = !rw_matched_sync() ||
			    (sync && rw_thumb_group_fits(work, commit_count, work_index));
			if (sample)
				[self thumbWork:work index:work_index after:NO];
			if (![self runDraw:work why:why size:sizeof(why)]) {
				if (sync && rw_matched_sync())
					rw_print("sync matched: commit %u work %u incomplete, draw failed\n",
					    commit_count, work_index);
				goto done;
			}
			rw_audit_render_work(work, commit_count, work_index);
			rw_work_time(RW_WORK_DRAW, work_began);
			if (sync && ![self syncDraw:work index:work_index sample:sample why:why size:sizeof(why)]) {
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
		/* Without the wait: the dispatch goes with this buffer's flush below, and the buffer
		 * completes on that fence (annibalemetal_completion.inc). In the Today view of
		 * kernel-12377-cmd43-bo19 the waiting dispatch took 1.4 to 3.2 ms each, 16 to 52 of
		 * them per 64 command buffers. ANNIBALE_SYNC_DRAWS keeps the wait, which names the
		 * dispatch the GPU did not finish. */
		bool has_ib = (work->imageblockSize[0] != 0 || work->imageblockSize[1] != 0) &&
			      mesa.dispatch_nowait_imageblock != NULL;
		unsigned split[12];

		if (work->split) {
			memcpy(split, work->splitBaseGroup, sizeof(work->splitBaseGroup));
			memcpy(split + 3, work->splitNominal, sizeof(work->splitNominal));
			memcpy(split + 6, work->splitGridThreads, sizeof(work->splitGridThreads));
			memcpy(split + 9, work->splitGridGroups, sizeof(work->splitGridGroups));
		}
		int rc = work->split
			    ? mesa.dispatch_split([work->pipeline compute], &bindings, table, work->count,
						  work->groups, work->size, work->threadgroupBytes,
						  work->imageblockSize, split, rw_sync_draws(), why, sizeof(why))
			    : has_ib
			    ? mesa.dispatch_nowait_imageblock([work->pipeline compute], &bindings,
							      table, work->count, work->groups,
							      work->size, work->threadgroupBytes,
							      work->imageblockSize, why, sizeof(why))
			    : mesa.dispatch_nowait != NULL && !rw_sync_draws()
			    ? mesa.dispatch_nowait([work->pipeline compute], &bindings, table,
						   work->count, work->groups, work->size,
						   work->threadgroupBytes, why, sizeof(why))
			    : mesa.dispatch_threadgroup != NULL
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
		if (rc == 0 && has_ib && rw_sync_draws() && mesa.finish != NULL) {
			rc = mesa.finish([[_queue device] mesaDevice], why, sizeof(why));
		}
		rw_work_time(RW_WORK_DISPATCH, work_began);
		if (rc != 0) {
			rw_print("the dispatch failed: %s\n", why);
			[self failWith:why];
			goto done;
		}
		/* One dispatch is one piece: hand it over before encoding the next, and wait for
		 * the piece before it first (rw_yield_wait). The encode of the next dispatch
		 * happens ahead of that wait, so what the GPU idles between pieces is the wake
		 * and the flush rather than the encoding. An open RGB2P pass is not a boundary
		 * the batch may break at, so the piece goes on until it closes. */
		if (pacing && [rgb2pOpen count] == 0) {
			char piece_why[256] = { 0 };

			yield_waited_us += rw_yield_wait(yield_device);
			mesa.flush(yield_device, piece_why, sizeof(piece_why));
			rw_yield_adopt(yield_device);
			yield_flushes++;
		}
	}
	if (rw_sync_draws() && rw_thumbs_wanted(commit_count) && !rw_thumb_draws) {
		NSMutableSet *seen = [NSMutableSet set];

		/* "res" is a colour attachment's resolve target: a CAMetalLayer drawn with
		 * multisampling shows that texture, and the multisample attachment cannot be sampled. */
		for (AnnibaleMetalWork *work in _work) {
			const unsigned colors = work->colorCount;

			for (unsigned i = 0; i < 2 * colors + work->textureCount; i++) {
				unsigned slot = i < colors ? i : i < 2 * colors ? i - colors : i - 2 * colors;
				id texture = i < colors ? work->color[slot] : i < 2 * colors ? work->resolve[slot]
										 : work->textures[slot];

				if (texture == nil || [seen containsObject:texture])
					continue;
				[seen addObject:texture];
				rw_thumb([texture mesaTexture], texture, commit_count, 0,
					 i < colors ? "out" : i < 2 * colors ? "res" : "in", slot);
			}
		}
	}
done:
	/* Failed work can leave an attachment between its load and store. Close those
	 * passes before flushing; _work retains the resources through completion. */
	for (id texture in rgb2pOpen) {
		char ended[256] = {0};
		if (mesa.texture_rgb2p_pass([texture mesaTexture], false, ended, sizeof(ended))) {
			rw_print("RGB2P pass cleanup failed: %s\n", ended);
			[self failWith:ended];
		}
	}
	rw_audit_commit_end(commit_count);
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
	encode_us = rw_ticks_us(mach_absolute_time() - started);
	if (mesa.flush != NULL) {
		char flushed[256] = { 0 };
		static unsigned long commits, window_commits, window_works, window_flushes;
		static unsigned window_tables;
		static uint64_t window_encode, window_flush;
		struct mach_timebase_info tb;
		uint64_t encode;

		mach_timebase_info(&tb);
		encode = encode_us;
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
		if (pacing) {
			yield_waited_us += rw_yield_wait(yield_device);
		}
		mesa.flush([[_queue device] mesaDevice], flushed, sizeof(flushed));
		/* Retain this submission before another commit or finish changes pending. */
		if (mesa.fence_take != NULL) {
			submission_fence = mesa.fence_take([[_queue device] mesaDevice]);
		}
		/* The carry for the next commit's first piece: this commit's last piece, which a
		 * non-pacing commit in between leaves untouched. A second take of the same
		 * fence, owned apart from the completion thread's. */
		if (pacing) {
			rw_yield_adopt(yield_device);
		}
		/* A texture over a buffer the library could not lay over the buffer's memory is a copy
		 * (rwa_texture_from_buffer); what this command buffer wrote into one goes back into the
		 * buffer here, before the next commit, and not later on the completion thread. Metal's
		 * texture and buffer are one memory, so the next command buffer and the processor read
		 * the result through the buffer; with the copy left to completion, Deep Fusion's next
		 * command buffer read the old bytes and the photograph came out dark green
		 * (kernel-12377-lens2-w26; waiting for each command buffer, w28, was right). Returns at
		 * once when no such texture was written; otherwise the download waits for the GPU. */
		if (mesa.buffer_textures_writeback != NULL) {
			mesa.buffer_textures_writeback([[_queue device] mesaDevice]);
		}
		commit_phase = 0;
		commit_progress_us = rw_now_us();
		window_flush += (mach_absolute_time() - before) * tb.numer / tb.denom / 1000;
		window_flushes++;
		{
			uint64_t flushed_us = (mach_absolute_time() - before) * tb.numer / tb.denom / 1000;

			flush_us = flushed_us;
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
	rw_timing_commit(commit_count, encode_us, refresh_us, flush_us);
	/* The queue's slot goes back here and on every way out of this method, a failed draw
	 * included: a buffer that errored is as finished as one that drew, and a slot kept
	 * because of an error would turn one bad draw into a queue that blocks for ever. */
	[self annibaleReturnSlot];
	commit_started = prev_started;
	commit_in_flight = prev_in_flight;
	/* The handlers run outside the lock: one that waits for another thread's commit would
	 * otherwise wait for ever. */
	pthread_mutex_unlock(&rw_commit_lock);
	/* and only then the handlers, so that a handler which makes the next command buffer
	 * is not waiting for a slot this one still holds (annibalemetal_completion.inc) */
	[self annibaleCompleteLater:this_commit fence:submission_fence];
	/* How long the whole commit took, said only when it was long enough to be a missed
	 * frame. A commit that never returns leaves the "has run for" line above as the last
	 * thing this process said, which names the work it was inside. */
	{
		uint64_t us = rw_ticks_us(mach_absolute_time() - started);

		if (us > RW_COMMIT_SLOW_US) {
			rw_print("commit %u took %llu ms over %lu recorded works%s\n", commit_count,
			         (unsigned long long)(us / 1000),
			         (unsigned long)[_work count],
			         pacing ? " (paced)" : "");
		}
		if (pacing) {
			rw_print("yield: commit %u: %u piece flushes over %llu threads, "
			         "%llu ms of piece waits in it\n", commit_count, yield_flushes,
			         (unsigned long long)commit_threads,
			         (unsigned long long)(yield_waited_us / 1000));
		}
	}
}

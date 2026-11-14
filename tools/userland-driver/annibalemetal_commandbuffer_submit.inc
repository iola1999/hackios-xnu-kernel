/* Commit execution retains the existing work order and completion path. */
- (void)commit
{
	char why[512] = { 0 };
	uint64_t started = mach_absolute_time();
	uint64_t refresh_us = 0, encode_us = 0, flush_us = 0;
	struct rwa_fence *submission_fence = NULL;

	pthread_mutex_lock(&rw_commit_lock);
	if (__atomic_load_n(&_phase, __ATOMIC_ACQUIRE) != RW_CB_PHASE_RECORDING) {
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
			bool sync = rw_sync_draws() || rw_take_matched_draw(work, commit_count, work_index);
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
		int rc = mesa.dispatch_nowait != NULL && !rw_sync_draws()
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
		rw_work_time(RW_WORK_DISPATCH, work_began);
		if (rc != 0) {
			rw_print("the dispatch failed: %s\n", why);
			[self failWith:why];
			goto done;
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
		mesa.flush([[_queue device] mesaDevice], flushed, sizeof(flushed));
		/* Retain this submission before another commit or finish changes pending. */
		if (mesa.fence_take != NULL) {
			submission_fence = mesa.fence_take([[_queue device] mesaDevice]);
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
			rw_print("commit %u took %llu ms over %lu recorded works\n", commit_count,
			         (unsigned long long)(us / 1000),
			         (unsigned long)[_work count]);
		}
	}
}

/* Submission validation precedes cache maintenance and backend publication. */
IOReturn
AnnibaleGPUUserClient::submit(struct msm_gem_submit * request, struct gpu_io_stat * io)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t entered = mach_absolute_time(), began, nanos, again, roomed, acq;
	/*
	 * Segment boundaries inside the lock, for "tl: is" when the whole span is slow: after the
	 * depth wait, the validation, the cache maintenance, the backend's own submit, the "tl: Q"
	 * line's making, the per-buffer fence bookkeeping, reap(), and the statistics. The tail of
	 * a submit -- everything after "tl: Q" -- reached 16 ms in batch 8 (tlL-strict-comparison
	 * §7) without any line saying which part of it that was; these say it.
	 */
	uint64_t seg_room, seg_val, seg_flush, seg_sub, seg_qlog, seg_book, seg_reap, seg_end;
	IOReturn ret = kIOReturnBadArgument;
	const bool a830 = annibale_platform_is_annibale();

	if (a830 && ((request->flags & ~MSM_SUBMIT_NO_IMPLICIT) != MSM_PIPE_3D0 ||
	    request->in_syncobjs || request->out_syncobjs || request->nr_in_syncobjs ||
	    request->nr_out_syncobjs || request->syncobj_stride)) {
		return kIOReturnBadArgument;
	}
	if (request->nr_cmds == 0 || request->nr_cmds > (a830 ? GPU_A830_SUBMIT_CMDS : GPU_SUBMIT_CMDS) || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("AnnibaleGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	acq = ioLock(io, kMsmGemSubmit);
	roomed = mach_absolute_time();
	/*
	 * The A830's queue holds adreno8_context_depth() submissions ahead of the GPU. With that many
	 * outstanding, the backend would wait for the oldest with this lock held, and every other
	 * buffer request and the display's driver with it; gpuWait() lets go of the lock meanwhile.
	 *
	 * The fence waited for is fixed once, here: the core retires in order, so `target` retiring
	 * means at most depth - 1 of the submissions this one was behind are still outstanding --
	 * the room this one needs. Re-reading annibale_gpu_last() on every round instead (as this
	 * loop did) made the target advance with every submission another program made while
	 * gpuWait() slept: with work arriving faster than the core retires it, the wait outlasted
	 * the flood itself and one ioctl could be held for tens of milliseconds. Submissions made
	 * meanwhile are throttled by their own waits, and should more of them crowd in anyway, the
	 * backend's own depth check (adreno8_context_submit) is the backstop.
	 */
	{ /* (a scope of its own: the early exits above jump to the end of the function) */
		uint32_t depth = a830 ? adreno8_context_depth() : 0, target = 0;

		if (depth != 0 && !gGPUReported && annibale_gpu_ready()) {
			uint32_t last = annibale_gpu_last();

			if (last >= depth) {
				target = last - depth + 1;
			}
		}
		uint64_t depthBegan = mach_absolute_time();
		bool depthWaited = target != 0 && !annibale_gpu_done(target);

		if (depthWaited) {
			depthWaitEntry(target, depth);
		}
		while (target != 0 && !gGPUReported && annibale_gpu_ready() && !annibale_gpu_done(target)) {
			if (!gpuWait(target, GPU_PATIENCE_MS)) {
				break;                          /* (the backend's own wait says what is wrong) */
			}
		}
		if (depthWaited) {
			depthWaitExit(target, depthBegan);
		}
	}
	gGPUStat.rooming += mach_absolute_time() - roomed;
	seg_room = mach_absolute_time();
	if (gGPUReported || !annibale_gpu_ready()) {
		ioUnlock(io, acq);
		ret = kIOReturnNotReady;
		goto out;
	}
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		uint64_t address = b ? b->address + cmds[i].submit_offset : 0;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size ||
		    (a830 && (cmds[i].size == 0 || cmds[i].size / 4 > 0xfffff || b->address >= (1ull << 32) ||
		    address >= (1ull << 32) || cmds[i].size > (1ull << 32) - address))) {
			IOLog("AnnibaleGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			ioUnlock(io, acq);
			goto out;
		}
		addresses[count] = address;
		words[count++] = cmds[i].size / 4;
	}
	if (count == 0) {
		ioUnlock(io, acq);
		goto out;
	}
	seg_val = mach_absolute_time();
	/*
	 * The GPU reads memory and writes memory. What this processor's cache holds of an IOSurface
	 * (somebody has written it, or has read it, as the display's driver does, and the GPU is to
	 * draw into it again) goes out of the cache now: when the surface is said to have changed
	 * (its seed: who writes one is to lock it, and so does a program that has had the GPU draw
	 * into one: userland/src/eagl.c), and in any case once in a few milliseconds, which is once
	 * a frame and not once a submission: not everybody who draws into a surface says so (an
	 * application drawing a picture that is on the screen meanwhile). A surface this program
	 * has mapped, every time. The buffers made here need none of that, unless they are
	 * ordinary memory (gpu_cached=1). With gpu_coherent=1 none of it is done: the GPU maps
	 * those pages cacheable and inner-shareable and snoops the caches itself (adreno8_bo_attributes).
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	for (uint32_t i = 0; i < request->nr_bos && !gGPUCoherent; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b == NULL) {
			continue;
		}
		if (b->surface != NULL) {
			uint32_t seed = b->surface->shared()->seed;

			if (seed == b->seed && !b->seen && began - b->pushed < again) {
				continue;
			}
			b->seed = seed;
			b->pushed = began;
		} else if (!b->seen || !gGPUCached) {
			continue;
		}
		annibale_gpu_to_memory(b->kernel, b->size);
		gGPUStat.flushed += b->size;
	}
	annibale_gpu_to_memory(NULL, 0);                 /* (and what was written last is in memory: a barrier) */
	gGPUStat.flushing += mach_absolute_time() - began;
	seg_flush = mach_absolute_time();
	gGPUStat.buffers += request->nr_bos;
	request->fence = annibale_gpu_submit(addresses, words, count);
	if (request->fence != 0) {
		gpuNoteSubmit(fPid);
	}
	/*
	 * The ring is full and the core is not reading it: it has stopped. With gpu_recover=1 it is
	 * reset and given this submission again -- after a reset the ring is empty, so there is
	 * room for it. (A wait of a second or more notices a stopped core first, as a rule; this is
	 * for a program that submits without waiting.)
	 */
	if (request->fence == 0 && !gGPUReported) {
		annibale_gpu_report("the ring is full");
		gGPUReported = !annibale_gpu_recover();
		if (!gGPUReported) {
			request->fence = annibale_gpu_submit(addresses, words, count);
			if (request->fence != 0) {
				gpuNoteSubmit(fPid);
			}
		}
	}
	if (request->fence == 0) {
		/* The backend may have published commands before the failure. Keep every mapped
		 * descriptor, including buffers still carrying their previous fence. */
		gGPUReported = true;
	}
	seg_sub = mach_absolute_time();
	if (gpu_log && request->fence != 0) {
		/* "tl: Q FENCE WHO at US: N buffers, B of 4 MB or more, W words of commands": what was given
		 * (tl: q: what it took). WHO is the process that gave it, because several share the GPU and
		 * the sum alone does not say which of them is the one that submits a hundred times a frame. */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;
		char who[24] = "?";

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		proc_selfname(who, sizeof(who));
		annibale_tl("tl: Q %u %s at %llu: %u buffers, %u big, %u words\n", request->fence, who, ns / 1000,
		    request->nr_bos, big, all_words);
	}
	seg_qlog = mach_absolute_time();
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * The surfaces named are being drawn into, or read, until this submission is done: the
	 * display's driver waits for that before it puts one on the panel (MiniIOSurfaceDrawn()).
	 * (freedreno names every buffer for reading and writing alike, so which it is cannot be
	 * told; the last submission of a frame names the surface the frame is drawn into.)
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL) {
				b->surface->drawn = request->fence;
			}
		}
	}
	seg_book = mach_absolute_time();
	reap();
	seg_reap = mach_absolute_time();
	if (fSubmits++ < 4) {
		IOLog("AnnibaleGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    addresses[0], words[0]);
	}
	/* every five seconds in which something was submitted: how much, and what it cost here */
	gGPUStat.submissions++;
	gGPUStat.submitting += mach_absolute_time() - entered;
	gGPUStat.commands += count;
	if (gGPUStat.since == 0) {
		gGPUStat.since = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked, submitting, rooming;
		uint64_t statsBegan = mach_absolute_time();
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.submitting, &submitting);
		absolutetime_to_nanoseconds(gGPUStat.rooming, &rooming);
		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		annibale_gpu_worked(&worked, &timed, by);
		uint64_t making, allocating, locking;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		absolutetime_to_nanoseconds(gGPUStat.allocating, &allocating);
		absolutetime_to_nanoseconds(gGPUStat.locking, &locking);
		uint32_t kept_count = 0;

		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			kept_count += fKept[i].memory != NULL;
		}
		IOLog("AnnibaleGPU: buffers: %u made in %llu us each (%llu getting the memory, %llu waiting for the GPU lock; %u under "
		    "64 KB, %u under 1 MB, %u under 8 MB, %u larger), %u kept ones given again (%llu MB kept), hits by size %u/%u/%u/%u, "
		    "%u in the pool; misses %u for size, %u for flags; evicted %u entry-cap, %u byte-cap, %u expired\n",
		    gGPUStat.made, gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0,
		    gGPUStat.made ? allocating / NSEC_PER_USEC / gGPUStat.made : 0,
		    gGPUStat.made ? locking / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.made_by_size[0], gGPUStat.made_by_size[1],
		    gGPUStat.made_by_size[2], gGPUStat.made_by_size[3], gGPUStat.given_again, fKeptBytes >> 20,
		    gGPUStat.hit_by_size[0], gGPUStat.hit_by_size[1], gGPUStat.hit_by_size[2], gGPUStat.hit_by_size[3], kept_count,
		    gGPUStat.miss_size, gGPUStat.miss_flags, gGPUStat.evict_entrycap, gGPUStat.evict_bytecap, gGPUStat.expire_drop);
		/* Three messages, one record each: what a window's submissions cost here, what the GPU did
		 * with them, and what they named. */
		IOLog("AnnibaleGPU: %u submissions in %llu ms: %llu us each in the kernel, %llu of them waiting for room in the queue; "
		    "%u command buffers and %u buffers named, %llu KB out of the cache in %llu us a submission\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, submitting / NSEC_PER_USEC / gGPUStat.submissions,
		    rooming / NSEC_PER_USEC / gGPUStat.submissions, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions);
		IOLog("AnnibaleGPU: the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all)\n",
		    timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000);
		IOLog("AnnibaleGPU: %u waits of %llu us, %u over before they began; %u IOSurfaces taken as buffers in %llu us each; "
		    "%u buffers, %llu MB; %u completion interrupts\n",
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20, annibale_gpu_irqs() - gGPUIrqsSeen);
		gGPUIrqsSeen = annibale_gpu_irqs();
		uint32_t mhz, changes;

		annibale_gpu_speed(&mhz, &changes);
		if (mhz != 0) {
			IOLog("AnnibaleGPU: GPU at %u MHz, %u speed changes since it started\n", mhz, changes);
		}
		if (gpu_va != 0) {
			/*
			 * The address space itself, because nothing else said anything about it: what
			 * argument buffers and heaps are built on is that this allocator holds, and
			 * "lost" and "refused" are the two ways it stops holding (bsd/dev/
			 * annibale_adreno.c, annibale_gpu_address_free).
			 */
			uint64_t used, largest, lost;
			uint32_t live, refused;
			unsigned holes, most;

			annibale_gpu_space(&used, &live, &largest, &holes, &most, &lost, &refused);
			IOLog("AnnibaleGPU: addresses: %llu MB in %u buffers, longest run free %llu MB, %u holes "
			    "(%u at most, of 1024), %llu KB lost, %u allocations refused\n", used >> 20, live,
			    largest >> 20, holes, most, lost >> 10, refused);
		}
		/*
		 * What printing the statistics themselves cost, with the lock held throughout: the
		 * suspect for whole-ioctl stalls that no other line can see (kernel-submit-blocking
		 * report, batch 5). Into the ring rather than the log, which is the suspect itself.
		 */
		if (gWaitLines) {
			uint64_t cost, at;

			absolutetime_to_nanoseconds(mach_absolute_time() - statsBegan, &cost);
			absolutetime_to_nanoseconds(mach_absolute_time(), &at);
			annibale_tl("tl: I %d %llu %llu at %llu\n", proc_pid(current_proc()),
			    (unsigned long long)thread_tid(current_thread()), cost / 1000, at / 1000);
		}
		bzero(&gGPUStat, sizeof(gGPUStat));
	}
	seg_end = mach_absolute_time();
	ioUnlock(io, acq);
	/*
	 * The whole span in the lock, with what it was spent on, when it passed the notice
	 * threshold: room is the depth wait for space in the queue, val the command buffers'
	 * validation, flush the cache maintenance (nothing with gpu_coherent=1), sub the backend's
	 * own submit including a recovery retry, qlog the counting and printing of "tl: Q", book
	 * the per-buffer fence bookkeeping, reap the freeing of what the GPU is done with, stat
	 * the five-second statistics. After ioUnlock so the line itself holds nothing.
	 */
	if (gGPUIoLog) {
		uint64_t span_ns, at;

		absolutetime_to_nanoseconds(seg_end - roomed, &span_ns);
		if (span_ns >= (uint64_t) GPU_LOCK_NOTICE_US * NSEC_PER_USEC) {
			uint64_t us[8];

			absolutetime_to_nanoseconds(seg_room - roomed, &us[0]);
			absolutetime_to_nanoseconds(seg_val - seg_room, &us[1]);
			absolutetime_to_nanoseconds(seg_flush - seg_val, &us[2]);
			absolutetime_to_nanoseconds(seg_sub - seg_flush, &us[3]);
			absolutetime_to_nanoseconds(seg_qlog - seg_sub, &us[4]);
			absolutetime_to_nanoseconds(seg_book - seg_qlog, &us[5]);
			absolutetime_to_nanoseconds(seg_reap - seg_book, &us[6]);
			absolutetime_to_nanoseconds(seg_end - seg_reap, &us[7]);
			absolutetime_to_nanoseconds(roomed, &at);
			annibale_tl("tl: is %d %llu fence %u room %llu val %llu flush %llu sub %llu qlog %llu book %llu reap %llu stat %llu at %llu\n",
			    proc_pid(current_proc()), (unsigned long long)thread_tid(current_thread()), request->fence,
			    us[0] / 1000, us[1] / 1000, us[2] / 1000, us[3] / 1000, us[4] / 1000, us[5] / 1000, us[6] / 1000,
			    us[7] / 1000, at / 1000);
		}
	}
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

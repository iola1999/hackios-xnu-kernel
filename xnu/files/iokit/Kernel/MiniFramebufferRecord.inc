/*
 * Screen recording into a physically contiguous buffer shared with xnuloader (RWREC003).
 * Supports boot-time capture (rec_secs=N [rec_start=S] [rec_mb=M] [rec_ring=1] [rec_scanout=1]),
 * live runtime capture via sysctl kern.annibale.rec_arm / kern.annibale.rec_info, and a stream the host
 * reads while the kernel runs via kern.annibale.rec_stream (tools/k90auto/screenrec.py).
 * Included by MiniIOMobileFramebuffer.cpp.
 */

extern "C" void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

static TUNABLE(uint32_t, gRecScanout, "rec_scanout", 0);
static IOMobileFramebuffer * gFBInstance;

void
IOMobileFramebuffer::flushRecCache(void)
{
	if (fRecBase != NULL && loader_recording[1] != 0) {
		FlushPoC_DcacheRegion((vm_offset_t) fRecBase, (size_t) loader_recording[1]);
		FlushPoC_DcacheRegion((vm_offset_t) loader_recording, sizeof(loader_recording));
	}
}

bool
IOMobileFramebuffer::ensureRecBuffer(uint32_t wantMB)
{
	static const uint32_t fallbacks[] = { 96, 64, 32, 16, 8, 4 };

	if (fRecBase != NULL) {
		return true;
	}
	for (unsigned i = 0; i <= sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
		uint32_t mb = i == 0 ? wantMB : fallbacks[i - 1];
		IOBufferMemoryDescriptor * memory;

		if (mb == 0 || (i > 0 && mb >= wantMB)) {
			continue;
		}
		memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) mb << 20, ~0ULL);
		if (memory != NULL) {
			fRecDesc = memory;
			fRecBase = (uint8_t *) memory->getBytesNoCopy();
			fRecSize = (uint64_t) mb << 20;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			if (fRecRow == NULL) {
				fRecRow = IONew(uint32_t, fPanelWidth);
			}
			return true;
		}
	}
	return false;
}

void
IOMobileFramebuffer::resetRecHeader(uint64_t durationNs, uint64_t startDelayNs, bool scanout)
{
	struct rec_header * header = (struct rec_header *) fRecBase;

	fRecUsed = sizeof(*header);
	fRecFirst = 0;
	fRecDone = false;
	fRecWrapped = false;
	fRecScanout = scanout;
	fRecLastAt = 0;
	fRecHeld = false;
	fRecDurationNs = durationNs;
	fRecStartDelayNs = startDelayNs;
	bzero(header, sizeof(*header));
	memcpy(header->magic, "RWREC003", 8);
	header->width = fPanelWidth / REC_STEP;
	header->height = fPanelHeight / REC_STEP;
	loader_recording[1] = fRecUsed;
	flushRecCache();
}

/*
 * Every REC_STEP-th pixel of every REC_STEP-th row of the buffer the panel shows. That buffer is
 * mapped write-combining: a whole row is copied out first, as reading its pixels one by one from
 * there takes about ten times as long.
 */
void
IOMobileFramebuffer::recordScanout(uint8_t * to, uint32_t width, uint32_t height)
{
	const uint8_t * scanout = flip.ready ? flip.va[flip.shown] : (const uint8_t *) vinfo.v_baseaddr;

	for (uint32_t y = 0; y < height; y++) {
		const uint32_t * from = (const uint32_t *) (scanout + (size_t) (y * REC_STEP + 1) * fRowBytes);

		if (fRecRow != NULL) {
			memcpy(fRecRow, from, (size_t) fPanelWidth * 4);
			from = fRecRow;
		}
		for (uint32_t x = 0; x < width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
}

/*
 * swap == NULL: recTailMain() asks for the panel as it is, because the last frame shown came too soon
 * after the one before it to be recorded (rec_stream) and no frame has come since.
 */
void
IOMobileFramebuffer::recordFrame(const uint8_t * swap, MiniIOSurface * s, bool clearedBlack,
    bool drewPanel, uint64_t now)
{
	bool tail = swap == NULL;
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t since_first = 0, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint32_t rec_idx;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || fRecDurationNs == 0) {
		return;
	}
	if (fRecStartDelayNs != 0) {
		absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
		if (since_first < fRecStartDelayNs) {
			return;
		}
	}
	if (!tail && s == NULL && !clearedBlack && !fRecScanout && fSingleLayer) {
		return;
	}
	if (fRecStreamFps != 0 && !tail) {
		uint64_t interval;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / fRecStreamFps, &interval);
		if (fRecLastAt != 0 && now - fRecLastAt < interval) {
			fRecHeld = true;
			thread_call_enter_delayed(fRecTail, fRecLastAt + interval);
			return;
		}
	}
	if (fRecFirst == 0) {
		uint64_t began = 0;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: recording begins at %llu.%03llu s (%ux%u, scanout=%u)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000, header->width, header->height,
		    fRecScanout ? 1u : 0u);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > fRecDurationNs || (fRecUsed + frame_size > fRecSize && !gRecRing && fRecStreamFps == 0)) {
		fRecDone = true;
		flushRecCache();
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu KB at %#llx\n",
		    header->frames, loader_recording[1] >> 10, loader_recording[0]);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	rec_idx = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	if (clearedBlack && !fRecScanout && !tail) {
		bzero(to, (size_t) header->width * header->height * 3);
	} else if (!tail && !fRecScanout && fSingleLayer && s != NULL &&
	    s->info.width >= header->width && s->info.height >= header->height) {
		uint32_t sw = s->info.width, sh = s->info.height, bpr = s->info.bytesPerRow;
		const uint8_t * base = s->pixels();

		for (uint32_t y = 0; y < header->height; y++) {
			uint32_t sy = (uint32_t) (((uint64_t) (y * REC_STEP + 1) * sh) / fPanelHeight);
			const uint32_t * from = (const uint32_t *) (base + (size_t) (sy < sh ? sy : sh - 1) * bpr);

			for (uint32_t x = 0; x < header->width; x++) {
				uint32_t sx = (uint32_t) (((uint64_t) (x * REC_STEP + 1) * sw) / fPanelWidth);
				uint32_t pixel = from[sx < sw ? sx : sw - 1];

				*to++ = (uint8_t) (pixel >> 16);
				*to++ = (uint8_t) (pixel >> 8);
				*to++ = (uint8_t) pixel;
			}
		}
	} else {
		recordScanout(to, header->width, header->height);
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
	fRecLastAt = now;
	fRecHeld = false;
	if (fRecStreamFps != 0) {
		/* the host reads it from memory while the kernel runs; a "tl: rec" line a frame would only crowd the log */
		FlushPoC_DcacheRegion((vm_offset_t) frame, (size_t) frame_size);
		FlushPoC_DcacheRegion((vm_offset_t) header, sizeof(*header));
		return;
	}
	annibale_tl("tl: rec %u swap %u us %llu buf %d drew %u set 0x%x clr 0x%x sids %u,%u,%u,%u band %d..%d bseq %u->%u all %u\n",
	    rec_idx, swapWord(swap, SWAP_ID), (unsigned long long) micros(now),
	    flip.ready ? flip.shown : 0, drewPanel ? 1u : 0u,
	    swapWord(swap, SWAP_LAYERS_SET), swapWord(swap, SWAP_LAYERS_CLEARED),
	    swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
	    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)),
	    fLastBy0, fLastBy1, fLastBufSeqBefore, fLastBufSeqAfter, fLastAllDirty ? 1u : 0u);
}

extern "C" int
annibale_fb_rec_arm(int value, int * currentOut)
{
	IOMobileFramebuffer * fb = gFBInstance;

	if (fb == NULL) {
		if (currentOut != NULL) {
			*currentOut = 0;
		}
		return ENXIO;
	}
	if (currentOut == NULL && value != 0 && fb->fRecBase == NULL) {
		(void) fb->ensureRecBuffer(96);
	}
	IOLockLock(fb->fLock);
	if (currentOut != NULL) {
		bool active = fb->fRecBase != NULL && !fb->fRecDone && fb->fRecDurationNs != 0;
		*currentOut = active ? (fb->fRecScanout ? -1 : 1) : 0;
	} else if (value == 0) {
		fb->fRecDone = true;
		fb->fRecStreamFps = 0;
		fb->flushRecCache();
	} else {
		bool scanout = value < 0 || gRecScanout != 0;
		uint32_t secs = (uint32_t) (value < 0 ? -value : value);

		if (secs > 120) {
			secs = 120;
		}
		if (fb->fRecBase == NULL) {
			IOLockUnlock(fb->fLock);
			return ENOMEM;
		}
		fb->fRecStreamFps = 0;
		fb->resetRecHeader((uint64_t) secs * NSEC_PER_SEC, 0, scanout);
		IOLog("MiniIOMobileFramebuffer: armed runtime recording for %u s into %llu MB at %#llx (scanout=%u)\n",
		    secs, fb->fRecSize >> 20, loader_recording[0], scanout ? 1u : 0u);
	}
	IOLockUnlock(fb->fLock);
	return 0;
}

extern "C" void
annibale_fb_rec_info(char * buf, size_t size)
{
	IOMobileFramebuffer * fb = gFBInstance;

	if (fb == NULL) {
		snprintf(buf, size, "0x0 0 0 0 0 0");
		return;
	}
	IOLockLock(fb->fLock);
	if (fb->fRecBase == NULL) {
		snprintf(buf, size, "0x0 0 0 0 0 0");
	} else {
		struct rec_header * header = (struct rec_header *) fb->fRecBase;
		bool active = !fb->fRecDone && fb->fRecDurationNs != 0;

		if (!active) {
			fb->flushRecCache();
		}
		snprintf(buf, size, "0x%llx %llu %u %u %u %u %llu",
		    loader_recording[0], loader_recording[1],
		    header->frames, header->width, header->height, active ? 1u : 0u, fb->fRecSize);
	}
	IOLockUnlock(fb->fLock);
}

void
IOMobileFramebuffer::recTailMain(thread_call_param_t self, thread_call_param_t unused __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;

	IOLockLock(fb->fLock);
	if (fb->fRecHeld && fb->fRecStreamFps != 0) {
		fb->recordFrame(NULL, NULL, false, true, mach_absolute_time());
	}
	IOLockUnlock(fb->fLock);
}

/*
 * kern.annibale.rec_stream=N (1 to 30): record at most N frames a second, without end, over the oldest
 * once the buffer is full. Each frame and the header are flushed as written, so the host can read new
 * frames with "oem stage" while the kernel runs; the header's frames (until the buffer is first full)
 * or oldest (after) is where the next frame goes. A frame shown too soon after the last recorded one
 * is skipped; when no frame follows it, the panel is recorded once the interval has passed, so the
 * end of an animation is in the stream. 0 stops it, as kern.annibale.rec_arm=0 does.
 */
extern "C" int
annibale_fb_rec_stream(int value, int * currentOut)
{
	IOMobileFramebuffer * fb = gFBInstance;

	if (fb == NULL) {
		if (currentOut != NULL) {
			*currentOut = 0;
		}
		return ENXIO;
	}
	if (currentOut != NULL) {
		IOLockLock(fb->fLock);
		*currentOut = fb->fRecDone ? 0 : (int) fb->fRecStreamFps;
		IOLockUnlock(fb->fLock);
		return 0;
	}
	if (value < 0 || value > 30) {
		return EINVAL;
	}
	if (value != 0 && fb->fRecBase == NULL && !fb->ensureRecBuffer(96)) {
		return ENOMEM;
	}
	if (value != 0 && fb->fRecTail == NULL) {
		fb->fRecTail = thread_call_allocate(&IOMobileFramebuffer::recTailMain, fb);
		if (fb->fRecTail == NULL) {
			return ENOMEM;
		}
	}
	IOLockLock(fb->fLock);
	if (value == 0) {
		fb->fRecDone = true;
		fb->fRecStreamFps = 0;
		fb->flushRecCache();
	} else {
		fb->fRecStreamFps = (uint32_t) value;
		fb->resetRecHeader(UINT64_MAX, 0, false);
		IOLog("MiniIOMobileFramebuffer: streaming the screen at up to %d frames a second (%ux%u) into %llu MB at %#llx\n",
		    value, fb->fPanelWidth / REC_STEP, fb->fPanelHeight / REC_STEP, fb->fRecSize >> 20, loader_recording[0]);
	}
	IOLockUnlock(fb->fLock);
	return 0;
}

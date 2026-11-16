/*
 * Screen recording into a physically contiguous buffer shared with xnuloader (RWREC003).
 * Supports boot-time capture (rec_secs=N [rec_start=S] [rec_mb=M] [rec_ring=1] [rec_scanout=1])
 * and live runtime capture via sysctl kern.annibale.rec_arm / kern.annibale.rec_info.
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
	fRecDurationNs = durationNs;
	fRecStartDelayNs = startDelayNs;
	bzero(header, sizeof(*header));
	memcpy(header->magic, "RWREC003", 8);
	header->width = fPanelWidth / REC_STEP;
	header->height = fPanelHeight / REC_STEP;
	loader_recording[1] = fRecUsed;
	flushRecCache();
}

void
IOMobileFramebuffer::recordFrame(const uint8_t * swap, MiniIOSurface * s, bool clearedBlack,
    bool drewPanel, uint64_t now)
{
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
	if (s == NULL && !clearedBlack && !fRecScanout && fSingleLayer) {
		return;
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
	if (since_start > fRecDurationNs || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
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
	if (clearedBlack && !fRecScanout) {
		bzero(to, (size_t) header->width * header->height * 3);
	} else if (!fRecScanout && fSingleLayer && s != NULL &&
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
		const uint8_t * scanout = flip.ready ? flip.va[flip.shown] : (const uint8_t *) vinfo.v_baseaddr;

		for (uint32_t y = 0; y < header->height; y++) {
			const uint32_t * from = (const uint32_t *) (scanout + (size_t) (y * REC_STEP + 1) * fRowBytes);

			for (uint32_t x = 0; x < header->width; x++) {
				uint32_t pixel = from[x * REC_STEP + 1];

				*to++ = (uint8_t) (pixel >> 16);
				*to++ = (uint8_t) (pixel >> 8);
				*to++ = (uint8_t) pixel;
			}
		}
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
		snprintf(buf, size, "0x%llx %llu %u %u %u %u",
		    loader_recording[0], loader_recording[1],
		    header->frames, header->width, header->height, active ? 1u : 0u);
	}
	IOLockUnlock(fb->fLock);
}

/*
 * The rows of a frame on the panel: where a panel pixel samples the surface, the scaling of a band
 * of rows (drawRows) shared out between fb_threads processors (helperMain), and which rows a frame
 * has to draw at all (dirtyBand). Included by MiniIOMobileFramebuffer.cpp.
 */

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	int32_t         by0, by1;       /* the panel rows this frame draws (dirtyBand), within py0..py1 */
	bool            filter, unscaled;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				mixRows(upper, lower, blendRow, first, last, weight);
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			scaleRow(from, to + cx0, fColumn + cx0, fColumnWeight + cx0, (uint32_t) (cx1 - cx0), (uint32_t) sw);
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->by1 - job->by0;
		y0 = job->by0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->by0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}


/*
 * Boot-arg fb_rows=0 draws every frame whole. By default a frame that is one surface on the whole
 * panel draws only the panel rows whose source rows changed since the frame before: every source
 * row is hashed (reading the surface costs a fraction of scaling it), and a frame whose rows all
 * match draws nothing. The home screen sends ten frames a second while it sits still and each one
 * cost a whole scaling, 5 to 11 ms of two processors (kernel-12377-cmd37-bo13). False: nothing to
 * draw; otherwise the band is in job->by0..by1.
 *
 * An animation of the whole picture changes every row, and hashing them costs the presenter alone
 * up to 1.6 ms a frame before the scaling can start (kernel-12377-rate6-sk1: a window in which 489
 * of 567 frames changed nothing took 1623 us a frame in the driver). So after two frames in
 * a row that changed nine tenths of the rows or more, each less than ROWS_SOON_NS after the one
 * before, the next ROWS_UNHASHED frames are drawn whole unhashed; then one is hashed to know the
 * rows again (it draws whole, the hashes being old) and one to see whether the frames still change
 * whole. A pause of ROWS_SOON_NS or more goes back to hashing every frame.
 */
#define ROWS_SOON_NS    (25 * NSEC_PER_MSEC)
#define ROWS_UNHASHED   6

bool
IOMobileFramebuffer::dirtyBand(RowJob * job)
{
	int32_t geometry[8] = { job->sx, job->sy, job->sw, job->sh, job->px0, job->py0, job->px1, job->py1 };
	int32_t first = -1, last = -1, words = job->sw / 2;
	int64_t span = job->py1 - job->py0;
	uint64_t now = mach_absolute_time(), gap;
	bool known;

	job->by0 = job->py0;
	job->by1 = job->py1;
	if (!gRows || !fSingleLayer || fDiff || (job->sx & 1) != 0 || (job->stride & 7) != 0) {
		fRowHashRows = 0;
		return true;
	}
	absolutetime_to_nanoseconds(now - fBandAt, &gap);
	fBandAt = now;
	if (gap >= ROWS_SOON_NS) {
		fWholeFrames = 0;
	}
	if (fWholeFrames >= 2 && fUnhashed < ROWS_UNHASHED) {
		fUnhashed++;
		fStatUnhashed++;
		fRowHashRows = 0;
		return true;
	}
	if (fRowHashSize != (uint32_t) job->sh) {
		if (fRowHash != NULL) {
			IODelete(fRowHash, uint64_t, fRowHashSize);
			fRowHash = NULL;
			fRowHashSize = 0;
		}
		fRowHash = IONew(uint64_t, (uint32_t) job->sh);
		fRowHashSize = fRowHash != NULL ? (uint32_t) job->sh : 0;
		fRowHashRows = 0;
		if (fRowHash == NULL) {
			return true;
		}
	}
	known = fRowHashRows == (uint32_t) job->sh && memcmp(geometry, fRowHashGeometry, sizeof(geometry)) == 0;
	for (int32_t r = 0; r < job->sh; r++) {
		const uint64_t * w = (const uint64_t *) (job->pixels + (size_t) (job->sy + r) * job->stride + (size_t) job->sx * 4);
		/* four separate sums, so that the multiplications do not wait for each other */
		uint64_t h[4] = { 0xcbf29ce484222325ULL, 1, 2, 3 };
		int32_t i;

		for (i = 0; i + 4 <= words; i += 4) {
			h[0] = (h[0] ^ w[i]) * 0x100000001b3ULL;
			h[1] = (h[1] ^ w[i + 1]) * 0x100000001b3ULL;
			h[2] = (h[2] ^ w[i + 2]) * 0x100000001b3ULL;
			h[3] = (h[3] ^ w[i + 3]) * 0x100000001b3ULL;
		}
		for (; i < words; i++) {
			h[0] = (h[0] ^ w[i]) * 0x100000001b3ULL;
		}
		if (job->sw & 1) {
			h[0] = (h[0] ^ ((const uint32_t *) w)[job->sw - 1]) * 0x100000001b3ULL;
		}
		h[0] ^= (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
		if (!known || h[0] != fRowHash[r]) {
			first = first < 0 ? r : first;
			last = r;
		}
		fRowHash[r] = h[0];
	}
	fRowHashRows = (uint32_t) job->sh;
	memcpy(fRowHashGeometry, geometry, sizeof(geometry));
	if (known) {
		fWholeFrames = first >= 0 && (int64_t) (last - first + 1) * 10 >= (int64_t) job->sh * 9 ? fWholeFrames + 1 : 0;
		fUnhashed = 0;
	}
	if (first < 0) {
		return false;
	}
	fStatRowsFrom = fStatRowsFrom < 0 || first < fStatRowsFrom ? first : fStatRowsFrom;
	fStatRowsTo = last > fStatRowsTo ? last : fStatRowsTo;
	/* the panel rows that sample source rows first .. last, with a row to spare for the filter */
	job->by0 = job->py0 + (int32_t) ((int64_t) (first - 1) * span / job->sh) - 1;
	job->by1 = job->py0 + (int32_t) ((int64_t) (last + 2) * span / job->sh) + 2;
	job->by0 = job->by0 < job->py0 ? job->py0 : job->by0;
	job->by1 = job->by1 > job->py1 ? job->py1 : job->by1;
	return true;
}

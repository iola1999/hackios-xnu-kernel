static void
gpuProgressSample(uint32_t fence, uint64_t began, uint64_t now, uint32_t *stalledAt, uint64_t *stalledSince,
    uint64_t *cpSig, bool *stalledArmed, bool *cpValid)
{
	uint64_t sig = 0;
	uint32_t retired = annibale_gpu_retired_for(fence);
	bool ok = annibale_gpu_progress(&sig);

	if (!*stalledArmed || retired != *stalledAt) {
		*stalledAt = retired;
		*stalledSince = began;
		*stalledArmed = true;
		*cpSig = sig;
		*cpValid = ok;
	} else if (ok && (!*cpValid || sig != *cpSig)) {
		if (*cpValid) {
			*stalledSince = now;
		}
		*cpSig = sig;
		*cpValid = true;
	}
}

/* Wait for the 3D core to be done with submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	static uint32_t stalledAt;
	static uint64_t stalledSince, cpSig;
	static bool stalledArmed, cpValid;
	uint64_t began = mach_absolute_time(), now, patience, step, nextSample, stalled;
	bool wasReady = !gGPUReported && annibale_gpu_ready();

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	nanoseconds_to_absolutetime(5000ULL * NSEC_PER_MSEC, &step);
	nextSample = began + step;
	for (bool waited = false;; waited = true) {
		uint32_t seen = annibale_gpu_irqs();

		now = mach_absolute_time();
		if (!gGPUReported && annibale_gpu_ready() && annibale_gpu_done(fence)) {
			if (waited) {
				gGPUStat.waits++;
				gGPUStat.waiting += now - began;
			}
			stalledArmed = false;
			stalledSince = 0;
			cpValid = false;
			return true;
		}
		if (now - began >= patience || gGPUReported || !annibale_gpu_ready()) {
			break;                  /* (a GPU that has stopped answering is not waited for again) */
		}
		if (milliseconds >= 1000 && now >= nextSample) {
			gpuProgressSample(fence, began, now, &stalledAt, &stalledSince, &cpSig,
			    &stalledArmed, &cpValid);
			nextSample = now + step;
		}
		IOLockUnlock(gGPULock);
		annibale_gpu_pause(seen, GPU_ASK_AGAIN_US, GPU_ASK_LONGEST_US);
		IOLockLock(gGPULock);
	}
	/*
	 * A faulted wait or a multi-second wait whose retired timestamp and CP IB1/IB2 progress
	 * stood still for GPU_STALL_MS triggers diagnostics and, with gpu_recover=1, resets the core.
	 */
	if (wasReady && !gGPUReported && !annibale_gpu_ready()) {
		annibale_gpu_report("faulted");
		gGPUReported = !annibale_gpu_recover();
		stalledAt = 0;
		stalledSince = 0;
		stalledArmed = false;
		cpValid = false;
	} else if (milliseconds >= 1000 && !gGPUReported && annibale_gpu_ready()) {
		now = mach_absolute_time();
		gpuProgressSample(fence, began, now, &stalledAt, &stalledSince, &cpSig,
		    &stalledArmed, &cpValid);
		absolutetime_to_nanoseconds(now - stalledSince, &stalled);
		if (stalled >= (uint64_t) GPU_STALL_MS * NSEC_PER_MSEC) {
			annibale_gpu_report("no answer");
			gGPUReported = !annibale_gpu_recover();
			stalledAt = 0;
			stalledSince = 0;
			stalledArmed = false;
			cpValid = false;
		}
	}
	return false;
}

/* The descriptor and IOVA remain owned until unmap and TLB invalidation succeed.
 * A failed backend may still DMA into any submitted buffer, even with no returned fence. */
static bool
gpuReleaseMapping(uint64_t address, uint64_t size)
{
	if (gGPUReported || !annibale_gpu_ready() || !annibale_gpu_unmap(address, size)) {
		gGPUReported = true;
		return false;
	}
	annibale_gpu_address_free(address, size);
	return true;
}

/*
 * With the lock held. Any submission made before the buffer was given back may still read it, named
 * in it or not (an argument buffer holds bare GPU addresses), so its pages leave the GPU's address
 * space once the last submission of that moment has retired: until then it waits in fLeft and reap()
 * lets it go. adreno8_bo_unmap() used to wait for the whole GPU to go idle here, with this lock held,
 * which stopped every other program's submissions for up to a frame of GPU work. A failed unmap
 * retains its existing record.
 */
void
AnnibaleGPUUserClient::freeBuffer(struct gpu_buffer * b)
{
	uint32_t last = annibale_gpu_ready() ? annibale_gpu_last() : 0;
	bool busy = gGPUReported || !annibale_gpu_ready() || (last != 0 && !annibale_gpu_done(last));

	/*
	 * Nowhere to keep it: wait until `last` retires. One of the camera's still-image passes runs
	 * for seconds, so a wait that runs out here is tried again; gpuWait() reports a core whose
	 * retired timestamp stood still for GPU_STALL_MS and closes submissions, which ends this loop.
	 * The wait lets go of the lock, so a reap() meanwhile may make room.
	 */
	if (busy && fLeftCount == GPU_LEFT && !gGPUReported && annibale_gpu_ready()) {
		uint64_t began = mach_absolute_time(), ns;

		do {
			busy = !gpuWait(last, GPU_PATIENCE_MS);
		} while (busy && fLeftCount == GPU_LEFT && !gGPUReported && annibale_gpu_ready());
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		IOLog("AnnibaleGPU: %u buffers given back wait for the GPU; one more waited %llu ms for submission %u: %s\n",
		    GPU_LEFT, ns / NSEC_PER_MSEC, last, busy ? "not done" : "done");
	}
	if (busy && fLeftCount < GPU_LEFT) {
		struct gpu_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->surface = b->surface;
		left->address = b->address;
		left->size = b->size;
		left->fence = last;
		left->flags = b->flags;
	} else {
		if (!busy && b->surface == NULL && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
			b->memory = NULL;
		} else if (busy || !gpuReleaseMapping(b->address, b->size)) {
			gGPUReported = true;
			IOLog("AnnibaleGPU: retaining %llu bytes until reset after unconfirmed completion or unmap\n", b->size);
			return;
		}
		if (b->memory != NULL) {
			b->memory->release();
		}
		if (b->surface != NULL) {
			MiniIOSurfaceDrop(b->surface);
		}
	}
	b->memory = NULL;
	b->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;
}

/* The buffers that were given back early and that the GPU is done with now: freed. With the lock held. */
void
AnnibaleGPUUserClient::reap(void)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct gpu_left * left = &fLeft[i];

		if (gGPUReported || !annibale_gpu_ready() || !annibale_gpu_done(left->fence)) {
			i++;
			continue;
		}
		if (left->surface == NULL && keep(left->memory, NULL, left->address, left->size, left->flags)) {
			left->memory = NULL;
		} else if (!gpuReleaseMapping(left->address, left->size)) {
			i++;
			continue;
		}
		if (left->memory != NULL) {
			left->memory->release();
		}
		if (left->surface != NULL) {
			MiniIOSurfaceDrop(left->surface);
		}
		*left = fLeft[--fLeftCount];
	}
}

IOReturn
AnnibaleGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/* what the client had the GPU do last may still be going on, in these buffers */
		if (annibale_gpu_ready()) {
			gpuWait(annibale_gpu_last(), 1000);
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap();
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		if (fLeftCount != 0) {
			IOLog("AnnibaleGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
		}
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
AnnibaleGPUUserClient::free(void)
{
	if (fKept != NULL) {
		IODelete(fKept, struct gpu_kept, GPU_KEPT);
		fKept = NULL;
	}
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
AnnibaleGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* (a program that names a buffer it has given back draws into somebody else's next: worth knowing) */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("AnnibaleGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * The submission a processor access to buffer `handle` has to wait for: the last one that named
 * the buffer here, or, for a surface's pixels, the last one in any program that named the surface
 * (MiniIOSurface::drawn). Waiting for annibale_gpu_last() instead, as CPU_PREP did, made a program
 * that writes one buffer wait for all the work every other program had given the GPU. A handle
 * that is not there waits for everything, as before. With the lock held.
 */
uint32_t
AnnibaleGPUUserClient::lastUse(uint32_t handle)
{
	struct gpu_buffer * b = buffer(handle);
	uint32_t fence;

	if (b == NULL) {
		return annibale_gpu_last();
	}
	fence = b->fence;
	if (b->surface != NULL && b->surface->drawn != 0 &&
	    (fence == 0 || (int32_t) (b->surface->drawn - fence) > 0)) {
		fence = b->surface->drawn;
	}
	return fence;
}

/*
 * What a buffer of this client's is, without the GPU lock (batch 8). Everything asked here is
 * fixed when the buffer is made and never written again while it lives (address, flags), or is
 * no state at all (offset, name), so the only thing the read can catch mid-write is the making
 * itself. Against that, every path that fills a slot writes the memory reference last and with
 * release ordering, and the read of it here is acquire: a slot either reads as empty or with
 * every field of its making visible, never half of it. Two residues, both accepted:
 *
 * - A close that races the query can read as the old buffer still there (fine: the answer is
 *   right about the buffer as it was) or, if the slot has since been given out again, as the
 *   new one. The lock never made that race meaningful either -- it only picked one order --
 *   and a program that closes a buffer while asking about it gets nothing useful in any order.
 * - The "not there" note buffer() writes (and its fStrangers count) is a write of shared
 *   state, so it is not repeated here; a bad handle is still a bad argument, only silent.
 *
 * MSM_INFO_SET_IOVA's once-a-run note writes gGPUSaidSetIova, one boolean everyone writes the
 * same value into: the worst a race does is print the refusal twice.
 */
IOReturn
AnnibaleGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	if (request->handle == 0 || request->handle > GPU_BUFFERS) {
		return kIOReturnBadArgument;
	}
	b = &fBuffers[request->handle - 1];
	if (__atomic_load_n(&b->memory, __ATOMIC_ACQUIRE) == NULL) {
		return kIOReturnBadArgument;
	}
	if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		/*
		 * The address the buffer was given when it was made, and the buffer's until it is
		 * given back. Nothing here ever moves a live buffer, so a caller may ask once and
		 * keep the answer -- which is what freedreno does (fd_bo_init_common).
		 */
		request->value = b->address;
	} else if (request->info == MSM_INFO_SET_IOVA) {
		/*
		 * Where a buffer goes is not the program's to choose here: every client is in the
		 * one SMMU context 0, so an address one program picked would be a place in every
		 * other program's space as well. Linux draws the line in the same place for the
		 * two parameters that describe a space -- VA_START and VA_SIZE are refused unless
		 * the client has page tables of its own (adreno_gpu.c, adreno_get_param) -- and
		 * placing a buffer is the stronger request of the two. Said once, by name, rather
		 * than returned as a bare code nobody can look up.
		 */
		if (!gGPUSaidSetIova) {
			gGPUSaidSetIova = true;
			IOLog("AnnibaleGPU: a program asked to place buffer %u at %#llx itself: refused. There is one "
			    "page table here for every client, so addresses are the kernel's to give and "
			    "MSM_INFO_GET_IOVA is how to learn them\n", request->handle, request->value);
		}
		ret = kIOReturnUnsupported;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* a name for somebody's debugger: not kept */
		ret = kIOReturnUnsupported;
	}
	return ret;
}

/*
 * A handle and addresses of the GPU's for `size` bytes of a memory descriptor, from byte `from`
 * of it on (wired memory; both in whole pages). With the lock held; NULL: allocation or mapping
 * failed, and `error` then says which shortage it was, so a caller's answer to the program can
 * say more than "no memory" for every one of them (kIOReturnNoResources: this client's handles
 * are all in use; kIOReturnNoSpace: no free span in the GPU's address space; kIOReturnVMError: no
 * page table for the span; kIOReturnNotReady: the GPU is going or gone, whenever in the making
 * that came to be known).
 */
struct gpu_buffer *
AnnibaleGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle, IOReturn * error)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	if (gGPUReported || !annibale_gpu_ready()) {
		*error = kIOReturnNotReady;
		return NULL;
	}
	if (size == 0 || from > memory->getLength() ||
	    size > memory->getLength() - from || ((from | size) & 4095) != 0) {
		*error = kIOReturnBadArgument;
		return NULL;
	}
	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL) {
		IOLog("AnnibaleGPU: no handles left for a buffer of %llu bytes\n", size);
		*error = kIOReturnNoResources;
		return NULL;
	}
	uint32_t refusal = 0;

	if ((b->address = annibale_gpu_address(size, &refusal)) == 0) {
		/* adreno8's own lines say the same refusal in words */
		*error = refusal == GPU_REFUSE_NO_RANGE ? kIOReturnNoSpace :
		    refusal == GPU_REFUSE_NO_TABLE ? kIOReturnVMError : kIOReturnNotReady;
		return NULL;
	}
	/* one entry for each 4 KB, one lookup for each physically contiguous run of them */
	addr64_t phys = 0;
	IOByteCount length = 0;

	for (uint64_t offset = 0; offset < size && mapped; offset += 4096, phys += 4096, length -= 4096) {
		if (length < 4096) {
			phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);
			mapped = phys != 0 && (phys & 4095) == 0 && length >= 4096;
		}
		mapped = mapped && annibale_gpu_map(b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	mapped = annibale_gpu_mapped() && mapped;
	if (!mapped) {
		if (!gpuReleaseMapping(b->address, size)) {
			/* The caller releases its reference on failure; this record retains the backing pages. */
			memory->retain();
			b->surface = NULL;
			b->size = size;
			b->flags = flags;
			b->fence = 0;
			/* the memory reference is the slot's validity, published last for gemInfo's lock-free read */
			__atomic_store_n(&b->memory, memory, __ATOMIC_RELEASE);
			fBufferCount++;
			fBufferBytes += size;
		}
		IOLog("AnnibaleGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		/* mapping and its commit fail on the GPU's health, never on how full anything is */
		*error = kIOReturnNotReady;
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->surface = NULL;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* With the lock held. */
bool
AnnibaleGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return true;
	}
	if (!gpuReleaseMapping(k->address, k->size)) {
		return false;
	}
	k->memory->release();
	k->memory = NULL;
	fKeptBytes -= k->size;
	return true;
}

#define GPU_KEPT_BYTES          (160ull << 20)
#define GPU_KEPT_SECONDS        60              /* a buffer not asked for again in so long is let go */
#define GPU_BUCKET_TO           (256u << 10)    /* smaller sizes are made in power-of-two buckets, this one and up exact */

/* The size classes of GPU_KEPT_SMALL/MEDIUM/LARGE/HUGE_TARGET and of the statistics' size counters. */
static uint32_t
gpuKeptClass(uint64_t size)
{
	return size < (64u << 10) ? 0 : size < (1u << 20) ? 1 : size < (8u << 20) ? 2 : 3;
}
static const uint32_t gGPUKeptTarget[4] = {
	GPU_KEPT_SMALL_TARGET, GPU_KEPT_MEDIUM_TARGET, GPU_KEPT_LARGE_TARGET, GPU_KEPT_HUGE_TARGET
};

/* With the lock held, the GPU done with the buffer: kept if it is worth it. */
bool
AnnibaleGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	struct gpu_kept * place = NULL;

	if (gGPUKeep == 0 || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;
		struct gpu_kept * oldest_by_class[4] = { NULL, NULL, NULL, NULL };
		uint32_t count[4] = { 0, 0, 0, 0 };

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
				continue;
			}
			if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
			uint32_t keptClass = gpuKeptClass(fKept[i].size);

			count[keptClass]++;
			if (oldest_by_class[keptClass] == NULL || fKept[i].since < oldest_by_class[keptClass]->since) {
				oldest_by_class[keptClass] = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		struct gpu_kept * victim = oldest;

		if (place == NULL) {
			/*
			 * Entry slots are shared in soft proportions when full. A size class may use spare
			 * slots beyond its target. Once no slot is free, the victim is the oldest entry of
			 * a class above target, or of the new entry's class when every class is at target.
			 * Byte-capacity pressure keeps using the global oldest entry.
			 */
			victim = NULL;
			for (uint32_t keptClass = 0; keptClass < 4; keptClass++) {
				if (count[keptClass] > gGPUKeptTarget[keptClass] && oldest_by_class[keptClass] != NULL &&
				    (victim == NULL || oldest_by_class[keptClass]->since < victim->since)) {
					victim = oldest_by_class[keptClass];
				}
			}
			if (victim == NULL) {
				victim = oldest_by_class[gpuKeptClass(size)];
			}
			victim = victim != NULL ? victim : oldest;
		}
		if (!dropKept(victim)) {
			return false;
		}
		/* no free place: the entry cap is what makes room; a free place and the bytes over: the byte cap */
		gGPUStat.evict_entrycap += place == NULL;
		gGPUStat.evict_bytecap += place != NULL;
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

IOReturn
AnnibaleGPUUserClient::gemNew(struct msm_gem_new * request, uint64_t * iova, struct gpu_io_stat * io)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/*
	 * Small sizes in power-of-two buckets, so that near sizes share one kept buffer: an icon's
	 * texture of 15 pages and another of 10 are both 64 KB here, where exact sizes would sit in
	 * the pool side by side, each waiting for its own repetition. The program is never told a
	 * size -- nothing in the interface hands it back -- so only the making and the matching see
	 * the bucket, and the spare pages are mapped and unused, as a quarter-larger kept buffer's
	 * always were. Sizes of GPU_BUCKET_TO and more are left as asked.
	 */
	if (size < GPU_BUCKET_TO) {
		uint64_t bucket = PAGE_SIZE;

		while (bucket < size) {
			bucket <<= 1;
		}
		size = bucket;
	}
	/*
	 * One kept of this kind and of this size or up to a quarter larger, if there is one: the smallest,
	 * and of those the one kept last (likeliest still in the caches' way). CoreAnimation's blur
	 * targets change size from frame to frame of an animation, and with exact sizes only the 160 MB
	 * kept were given again in none of most 30 s windows of kernel-12377-idle1-w1's gestures while
	 * backboardd made 218 buffers of 8 MB or more. The program sees the size it asked for; the rest
	 * is mapped and unused.
	 */
	uint64_t acq = ioLock(io, kMsmGemNew);

	if (gGPUReported || !annibale_gpu_ready()) {
		ioUnlock(io, acq);
		return kIOReturnNotReady;
	}
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	bool size_seen = false;         /* the pool has an entry of the size asked, whatever its flags: a miss is for the flags */

	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old) {
			if (!dropKept(k)) {
				ioUnlock(io, acq);
				return kIOReturnNotReady;
			}
			gGPUStat.expire_drop++;
		}

		if (k->memory != NULL && k->size >= size && k->size - size <= size / 4) {
			size_seen = true;
			if (k->flags == request->flags &&
			    (found == NULL || k->size < found->size || (k->size == found->size && k->since > found->since))) {
				found = k;
			}
		}
	}
	if (found == NULL) {
		gGPUStat.miss_size += !size_seen;
		gGPUStat.miss_flags += size_seen;
	}
	for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
		uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[h].memory == NULL) {
			b = &fBuffers[h];
			bzero(b, sizeof(*b));
			b->kernel = found->kernel;
			b->address = found->address;
			b->size = found->size;
			b->flags = found->flags;
			/* the memory reference is the slot's validity, published last for gemInfo's lock-free read */
			__atomic_store_n(&b->memory, found->memory, __ATOMIC_RELEASE);
			fNextBuffer = h + 1;
			fBufferCount++;
			fBufferBytes += found->size;
			request->handle = h + 1;
			if (iova) *iova = b->address;
			found->memory = NULL;
			fKeptBytes -= found->size;
			gGPUStat.given_again++;
			gGPUStat.hit_by_size[b->size < (64u << 10) ? 0 : b->size < (1u << 20) ? 1 : b->size < (8u << 20) ? 2 : 3]++;
			ioUnlock(io, acq);
			if (gGPUKeep == 2) {
				bzero(b->kernel, (size_t) b->size);
			}
			return kIOReturnSuccess;
		}
	}
	ioUnlock(io, acq);
	/* (the memory comes zeroed, and making it write-combined pushes the zeroes out of the cache) */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("AnnibaleGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	if (gGPUCached && !gGPUCoherent) {
		annibale_gpu_to_memory(memory->getBytesNoCopy(), size);
	}
	uint64_t allocated = mach_absolute_time();

	acq = ioLock(io, kMsmGemNew);
	gGPUStat.locking += mach_absolute_time() - allocated;
	IOReturn error = kIOReturnNoMemory;

	b = newBuffer(memory, 0, size, request->flags, &request->handle, &error);
	if (b != NULL) {
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		if (iova) *iova = b->address;
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
		gGPUStat.allocating += allocated - began;
		gGPUStat.made_by_size[size < (64u << 10) ? 0 : size < (1u << 20) ? 1 : size < (8u << 20) ? 2 : 3]++;
		/* the memory reference is the slot's validity, published last for gemInfo's lock-free read */
		__atomic_store_n(&b->memory, memory, __ATOMIC_RELEASE);
	}
	ioUnlock(io, acq);
	if (b == NULL) {
		memory->release();
		return error;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared
 * state on (MiniIOSurface.h). The surface is held for as long as the buffer is there. A surface
 * that is a buffer of this client's already is that buffer again, as with Linux's handles: so
 * the client must not be giving that buffer back at the same moment (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe (kAnnibaleDescribe), no buffer is made.
 */
IOReturn
AnnibaleGPUUserClient::surfaceBuffer(struct annibale_surface * request, struct gpu_io_stat * io)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct gpu_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size, began = mach_absolute_time();

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		/* (memory of a program's own with a surface made around it: not in whole pages, and seldom in rows the GPU takes) */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	IOMemoryDescriptor * pixelsMemory = s->backing != NULL ? s->backing : s->memory;
	uint64_t pixelsOffset = s->backing != NULL ? s->backingOffset : PAGE_SIZE;
	memory = IOSubMemoryDescriptor::withSubRange(pixelsMemory, pixelsOffset, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kAnnibaleDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	uint64_t acq = ioLock(io, kAnnibaleSurface);
	uint64_t inLock = mach_absolute_time();

	if (gGPUReported || !annibale_gpu_ready()) {
		ioUnlock(io, acq);
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNotReady;
	}
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	IOReturn error = kIOReturnNoMemory;

	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else if ((b = newBuffer(pixelsMemory, pixelsOffset, size, 0, &request->handle, &error)) != NULL) {
		b->kernel = s->kpixels;
		b->surface = s;
		gGPUStat.surfaces++;
		/* the memory reference is the slot's validity, published last for gemInfo's lock-free read */
		__atomic_store_n(&b->memory, memory, __ATOMIC_RELEASE);
	}
	gGPUStat.taking += mach_absolute_time() - began;
	if (gWaitLines) {
		uint64_t cost, at;

		absolutetime_to_nanoseconds(mach_absolute_time() - inLock, &cost);
		if (cost >= (uint64_t) GPU_IMPORT_NOTICE_US * NSEC_PER_USEC) {
			absolutetime_to_nanoseconds(mach_absolute_time(), &at);
			annibale_tl("tl: S %d %llu surface %u %llu %llu at %llu\n", proc_pid(current_proc()),
			    (unsigned long long)thread_tid(current_thread()), request->surface, size >> 10, cost / 1000, at / 1000);
		}
	}
	ioUnlock(io, acq);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return error;
	}
	return kIOReturnSuccess;
}

/*
 * The caller's own memory as a buffer, for Metal's -newBufferWithBytesNoCopy:: the GPU reads and
 * writes the pages the program writes and reads, and nothing is copied. The pages are wired for as
 * long as the buffer is there (IOGeneralMemoryDescriptor::free completes the prepare), so a program
 * that unmaps them first takes nothing from the GPU. Memory the program may only read (backboardd's
 * images: cmd35-bo8) is wired for reading and is read-only to the GPU and in the second mapping.
 * Only with gpu_coherent=1: these pages have no kernel address submit() could push out of the
 * cache, and there the GPU snoops the caches itself.
 */
IOReturn
AnnibaleGPUUserClient::memoryBuffer(struct annibale_memory * request, struct gpu_io_stat * io)
{
	static uint32_t refused;
	IOMemoryDescriptor * memory;
	struct gpu_buffer * b = NULL;
	uint32_t flags = 0;
	IOReturn ret;

	if (!gGPUCoherent) {
		return kIOReturnUnsupported;
	}
	if (request->size == 0 || request->size > GPU_BUFFER_MAX || ((request->address | request->size) & 4095) != 0) {
		return kIOReturnBadArgument;
	}
	memory = IOMemoryDescriptor::withAddressRange(request->address, request->size, kIODirectionInOut, fTask);
	if (memory == NULL) {
		return kIOReturnNoMemory;
	}
	if ((ret = memory->prepare()) == KERN_PROTECTION_FAILURE) {
		memory->release();
		memory = IOMemoryDescriptor::withAddressRange(request->address, request->size, kIODirectionOut, fTask);
		if (memory == NULL) {
			return kIOReturnNoMemory;
		}
		ret = memory->prepare();
		flags = MSM_BO_GPU_READONLY;
	}
	if (ret != kIOReturnSuccess) {
		if (refused++ < 16) {
			IOLog("AnnibaleGPU: %llu bytes at %#llx cannot be wired: %#x\n", request->size, request->address, ret);
		}
		memory->release();
		return ret;
	}
	uint64_t acq = ioLock(io, kAnnibaleMemory);
	IOReturn error = kIOReturnNoMemory;

	if (gGPUReported || !annibale_gpu_ready()) {
		error = kIOReturnNotReady;
	} else if ((b = newBuffer(memory, 0, request->size, flags, &request->handle, &error)) != NULL) {
		b->kernel = NULL;
		/* the memory reference is the slot's validity, published last for gemInfo's lock-free read */
		__atomic_store_n(&b->memory, memory, __ATOMIC_RELEASE);
	}
	ioUnlock(io, acq);
	if (b == NULL) {
		memory->release();
		return error;
	}
	return kIOReturnSuccess;
}

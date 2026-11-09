/* The serialized object in a request, or NULL. */
static OSObject *
unserialize(const uint8_t * data, size_t size)
{
	if (data == NULL || size < 4) {
		return NULL;
	}
	return OSUnserializeXML((const char *) data, size, NULL);
}

IOReturn
IOSurfaceRootUserClient::methodCreate(IOExternalMethodArguments * args)
{
	IOReturn ret;
	IOMemoryMap * inMap = NULL;
	const uint8_t * in = (const uint8_t *) args->structureInput;
	size_t inSize = args->structureInputSize;
	OSObject * obj;
	OSDictionary * props;
	MiniIOSurface * s;
	struct minisurf_client_ref * ref;
	uint64_t began = mach_absolute_time();

	if (args->scalarInputCount >= 1 && args->scalarInput[0] != 0) {
		IOLog("MiniIOSurface: child surfaces are not supported (parent %llu)\n", args->scalarInput[0]);
		return kIOReturnUnsupported;
	}
	if (args->structureInputDescriptor != NULL) {
		inMap = args->structureInputDescriptor->map(kIOMapReadOnly);
		if (inMap == NULL) {
			return kIOReturnNoMemory;
		}
		in = (const uint8_t *) inMap->getVirtualAddress();
		inSize = inMap->getLength();
	}
	obj = unserialize(in, inSize);
	OSSafeReleaseNULL(inMap);
	props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	uint64_t made_at = mach_absolute_time();
	s = surfaceCreate(props, &ret);
	made_at = mach_absolute_time() - made_at;
	if (s == NULL) {
		IOLog("MiniIOSurface: create failed 0x%x: %ux%u format 0x%x alloc %u\n", ret,
		    dictNumber(props, "IOSurfaceWidth", 0), dictNumber(props, "IOSurfaceHeight", 0),
		    dictNumber(props, "IOSurfacePixelFormat", 0), dictNumber(props, "IOSurfaceAllocSize", 0));
		props->release();
		return ret;
	}
	{
		char procname[32] = "?";
		static unsigned keysSaid;

		proc_selfname(procname, sizeof(procname));
		/* what a traced program asks for, key by key (the first few surfaces) */
		if (gSurfaceTraceOf[0] != 0 && strncmp(procname, gSurfaceTraceOf, strlen(gSurfaceTraceOf)) == 0 && keysSaid++ < 6) {
			OSCollectionIterator * keys = OSCollectionIterator::withCollection(props);
			char line[256];
			size_t used = 0;
			OSSymbol * key;

			line[0] = 0;
			while (keys != NULL && (key = OSDynamicCast(OSSymbol, keys->getNextObject())) != NULL && used < sizeof(line)) {
				OSNumber * n = OSDynamicCast(OSNumber, props->getObject(key));

				used += (size_t) snprintf(line + used, sizeof(line) - used, " %s=%lld", key->getCStringNoCopy(),
				    n ? (long long) n->unsigned64BitValue() : -1LL);
			}
			OSSafeReleaseNULL(keys);
			traceLine("MiniIOSurface: %s asks for a surface with%s\n", procname, line);
		}
	}
	props->release();
	{
		char procname[32] = "?";

		proc_selfname(procname, sizeof(procname));
		if (gSurfaceTraceOf[0] != 0 && strncmp(procname, gSurfaceTraceOf, strlen(gSurfaceTraceOf)) != 0) {
			procname[0] = 0;        /* (not one of those asked about: not counted) */
		}
		if (procname[0] != 0 && gSurfaceTraced < gSurfaceTrace) {
			s->traced = gSurfaceTraceOf[0] != 0;
		}
	}
	if (s->traced || (gSurfaceTraceOf[0] == 0 && gSurfaceTraced < gSurfaceTrace)) {
		char procname[32] = "?";

		gSurfaceTraced++;
		proc_selfname(procname, sizeof(procname));
		traceLine("MiniIOSurface: %s[%d] created surface %u: %ux%u '%c%c%c%c', %u bytes per row, %u planes, %u bytes\n",
		    procname, proc_selfpid(), s->sid, s->info.width, s->info.height, (int) (s->info.pixelFormat >> 24 & 0xff),
		    (int) (s->info.pixelFormat >> 16 & 0xff), (int) (s->info.pixelFormat >> 8 & 0xff),
		    (int) (s->info.pixelFormat & 0xff), s->info.bytesPerRow, s->info.planeCount, s->info.allocSize);
	}

	uint64_t mapped_at = mach_absolute_time();
	IOLockLock(gSurfaceLock);
	ref = addRef(s, &ret);
	if (ref != NULL) {
		ret = returnSurface(ref, false, args);
	}
	if (ref != NULL && ret != kIOReturnSuccess) {
		dropRef(ref);
	}
	uint32_t allocSize = s->info.allocSize;
	surfaceUnhold(s);                       /* the creator's hold; the client's own is in the ref */
	IOLockUnlock(gSurfaceLock);
	mapped_at = mach_absolute_time() - mapped_at;
	OSAddAtomic64((SInt64) (mach_absolute_time() - began), (SInt64 *) &gSurfaceMaking);
	OSIncrementAtomic(&gSurfacesMade);
	if (allocSize >= SURFACE_KEEP_FROM) {
		/* the big ones (the window server's of the whole display): where the time to make one goes, every 5 s */
		static uint64_t since, all, making, mapping;
		static uint32_t count;
		uint64_t now = mach_absolute_time(), ns;

		count++;
		all += now - began;
		making += made_at;
		mapping += mapped_at;
		absolutetime_to_nanoseconds(now - since, &ns);
		if (since == 0) {
			since = now;
		} else if (ns >= 5 * NSEC_PER_SEC) {
			uint64_t a, m, p;

			absolutetime_to_nanoseconds(all, &a);
			absolutetime_to_nanoseconds(making, &m);
			absolutetime_to_nanoseconds(mapping, &p);
			IOLog("MiniIOSurface: %u big surfaces made, %llu us each: %llu getting the memory and the surface, %llu mapping it "
			    "for its maker; memory not kept as somebody else had it %u times, a piece kept longer put out for it %u times\n",
			    count, a / 1000 / count, m / 1000 / count, p / 1000 / count, OSBitAndAtomic(0, &gSurfacesNotKept[0]),
			    OSBitAndAtomic(0, &gSurfacesNotKept[1]));
			since = now;
			count = 0;
			all = making = mapping = 0;
		}
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::methodLookup(IOExternalMethodArguments * args)
{
	IOReturn ret = kIOReturnNotFound;
	uint32_t sid;
	struct minisurf_client_ref * ref;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	sid = (uint32_t) args->scalarInput[0];
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	bool known = ref != NULL;
	if (ref == NULL && sid != 0 && sid < MINISURF_TABLE_SIZE && gSurfaceTable[sid] != NULL) {
		ref = addRef(gSurfaceTable[sid], &ret);
	}
	if (ref != NULL) {
		ret = returnSurface(ref, false, args);
		if (!known && ret != kIOReturnSuccess) {
			dropRef(ref);
		}
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::methodLookupFromPort(IOExternalMethodArguments * args)
{
	IOReturn ret;
	OSObject * obj = NULL;
	IOSurfaceSendRight * right;
	struct minisurf_client_ref * ref;
	bool known;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	ret = copyObjectForPortNameInTask(fTask, (mach_port_name_t) args->scalarInput[0], &obj);
	right = OSDynamicCast(IOSurfaceSendRight, obj);
	if (ret != kIOReturnSuccess || right == NULL || right->surface == NULL) {
		static unsigned said;

		if (said++ < 16) {
			char procname[32] = "?";

			proc_selfname(procname, sizeof(procname));
			traceLine("MiniIOSurface: %s[%d] looked up a surface from port %#llx: %s\n", procname, proc_selfpid(),
			    args->scalarInput[0], ret != kIOReturnSuccess ? "no object behind the port" : obj ? "not a surface's port" :
			    "no surface");
		}
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(right->surface->sid, false);
	known = ref != NULL;
	if (ref == NULL) {
		ref = addRef(right->surface, &ret);
	}
	if (ref != NULL) {
		ret = returnSurface(ref, known, args);
		if (!known && ret != kIOReturnSuccess) {
			dropRef(ref);
		}
	}
	IOLockUnlock(gSurfaceLock);
	if (right->surface->traced) {
		char procname[32] = "?";
		uint32_t format = right->surface->info.pixelFormat, nonzero = 0, first = 0, middle = 0;
		const uint8_t * pixels = right->surface->kpixels;
		uint32_t size = right->surface->info.allocSize;

		/* what is in it by now: how many of its bytes are not zero, its first pixel and one in the middle */
		for (uint32_t at = 0; pixels != NULL && at < size; at += 64) {
			nonzero += pixels[at] != 0 || pixels[at + 1] != 0 || pixels[at + 2] != 0 || pixels[at + 3] != 0;
		}
		if (pixels != NULL && size >= 8) {
			memcpy(&first, pixels, 4);
			memcpy(&middle, pixels + (size / 2 & ~3u), 4);
		}
		proc_selfname(procname, sizeof(procname));
		traceLine("MiniIOSurface: %s[%d] looked up surface %u (%ux%u '%c%c%c%c') from a port: %#x; of every 64 bytes %u of %u "
		    "begin with a pixel that is not 0, the first %#010x, one in the middle %#010x\n", procname, proc_selfpid(),
		    right->surface->sid, right->surface->info.width, right->surface->info.height, (int) (format >> 24 & 0xff),
		    (int) (format >> 16 & 0xff), (int) (format >> 8 & 0xff), (int) (format & 0xff), ret, nonzero, size / 64, first, middle);
	}
	obj->release();
	return ret;
}

mach_port_name_t
IOSurfaceRootUserClient::makeSendRight(task_t task, MiniIOSurface * s)
{
	IOSurfaceSendRight * right;
	mach_port_name_t name = 0;

	right = OSTypeAlloc(IOSurfaceSendRight);
	if (right == NULL || !right->init()) {
		OSSafeReleaseNULL(right);
		return 0;
	}
	IOLockLock(gSurfaceLock);
	surfaceHold(s);                         /* for as long as the send right exists */
	IOLockUnlock(gSurfaceLock);
	right->surface = s;
	copyPortNameForObjectInTask(task, right, &name);
	right->release();
	return name;
}

mach_port_name_t
MiniIOSurfaceMakeSendRight(task_t task, MiniIOSurface * surface)
{
	return IOSurfaceRootUserClient::makeSendRight(task, surface);
}

IOReturn
IOSurfaceRootUserClient::methodCreatePort(IOExternalMethodArguments * args)
{
	MiniIOSurface * s;
	mach_port_name_t name;

	if (args->scalarInputCount < 1 || args->scalarOutputCount < 1) {
		return kIOReturnBadArgument;
	}
	s = MiniIOSurfaceCopy((uint32_t) args->scalarInput[0]);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	name = makeSendRight(fTask, s);
	if (s->traced) {
		char procname[32] = "?";

		proc_selfname(procname, sizeof(procname));
		traceLine("MiniIOSurface: %s[%d] made a port for surface %u: %#x\n", procname, proc_selfpid(), s->sid, name);
	}
	MiniIOSurfaceDrop(s);
	args->scalarOutput[0] = name;
	args->scalarOutputCount = 1;
	return name != 0 ? kIOReturnSuccess : kIOReturnNoMemory;
}

#include "MiniIOSurfaceValues.inc"

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint8_t * in = (const uint8_t *) args->structureInput;
	size_t inSize = args->structureInputSize;
	IOMemoryMap * inMap = NULL;
	struct minisurf_client_ref * ref;
	IOReturn ret;

	switch (selector) {
	case kSurfaceMethodInit:
		return methodInit(args);
	case kSurfaceMethodCreate:
		return methodCreate(args);
	case kSurfaceMethodLookup:
		return methodLookup(args);
	case kSurfaceMethodLookupFromPort:
		return methodLookupFromPort(args);
	case kSurfaceMethodCreatePort:
		return methodCreatePort(args);

	case kSurfaceMethodSetValue:
	case kSurfaceMethodCopyValue:
	case kSurfaceMethodRemoveValue:
	case kSurfaceMethodSetBulkAttachments:
	case kSurfaceMethodSetCoreVideoBridgedKeys:
		if (args->structureInputDescriptor != NULL) {
			inMap = args->structureInputDescriptor->map(kIOMapReadOnly);
			if (inMap == NULL) {
				return kIOReturnNoMemory;
			}
			in = (const uint8_t *) inMap->getVirtualAddress();
			inSize = inMap->getLength();
		}
		if (in == NULL) {
			ret = kIOReturnBadArgument;
		} else if (selector == kSurfaceMethodSetValue) {
			ret = methodSetValue(args, in, inSize);
		} else if (selector == kSurfaceMethodCopyValue) {
			ret = methodCopyValue(args, in, inSize);
		} else if (selector == kSurfaceMethodRemoveValue) {
			ret = methodRemoveValue(args, in, inSize);
		} else if (selector == kSurfaceMethodSetCoreVideoBridgedKeys) {
			ret = methodSetCoreVideoBridgedKeys(in, inSize);
		} else {
			ret = methodSetBulkAttachments(in, inSize);
		}
		OSSafeReleaseNULL(inMap);
		return ret;

	case kSurfaceMethodSetPurgeable:                /* in: surface, new state; out: old state */
		if (args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		ret = kIOReturnNotFound;
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			if (args->scalarOutputCount >= 1) {
				args->scalarOutput[0] = ref->surface->purgeable;
				args->scalarOutputCount = 1;
			}
			if (args->scalarInput[1] != 3) {        /* 3 = keep current */
				ref->surface->purgeable = (uint32_t) args->scalarInput[1];
			}
			ret = kIOReturnSuccess;
		}
		IOLockUnlock(gSurfaceLock);
		return ret;

	case kSurfaceMethodBindAccel:                   /* in: surface, accelerator, sub-id */
		if (args->scalarInputCount < 3) {
			return kIOReturnBadArgument;
		}
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			ref->surface->shared()->accelID = (uint16_t) args->scalarInput[1];
			ref->surface->shared()->accelSubID = (uint16_t) args->scalarInput[2];
		}
		IOLockUnlock(gSurfaceLock);
		return ref ? kIOReturnSuccess : kIOReturnNotFound;

	case kSurfaceMethodSetYCbCrMatrix:              /* in: surface, matrix */
		if (args->scalarInputCount < 2) {
			break;
		}
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			minisurfSetYCbCrMatrix(ref->surface->shared(), fABI, (uint32_t) args->scalarInput[1]);
		}
		IOLockUnlock(gSurfaceLock);
		return ref ? kIOReturnSuccess : kIOReturnNotFound;

	case kSurfaceMethodSetOwnership:
	case kSurfaceMethodSetOwnershipIdentity:
	case kSurfaceMethodSetTimestamp:
	case kSurfaceMethodSetIndexedTimestamp:
	case kSurfaceMethodDetachModeCode:
	case kSurfaceMethodLog:
	/*
	 * iOS 26's IOSurfaceClientQueryTransactionList: in surface, list buffer, entry count and two
	 * more; out three scalars from which IOSurface builds an IOSurfaceTransactionList. No surface
	 * here has transactions, so the answer is an empty list. While it was refused, backboardd's
	 * Metal logged "Failed to allocate MTLTexture from IOSurface" right after each failed query
	 * (2026-10-30 rounds).
	 */
	case kSurfaceMethodQueryTransactionList:
		/* accounting and diagnostics: nothing depends on them here */
		if (args->scalarOutputCount != 0) {
			bzero(args->scalarOutput, args->scalarOutputCount * sizeof(args->scalarOutput[0]));
		}
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gSurfaceLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gSurfaceLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOSurface: selector %u not implemented (scalars %u in, %u out; structure %u in, %u out)\n", selector,
		    args->scalarInputCount, args->scalarOutputCount, args->structureInputSize, args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService ** targetP, UInt32 index)
{
	if (index >= kSurfaceTrapCount || fTraps[index].func == NULL) {
		return NULL;
	}
	*targetP = this;
	return &fTraps[index];
}

IOReturn
IOSurfaceRootUserClient::useCountCommon(uint32_t sid, uint32_t category, int delta)
{
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (category > 1) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		MiniIOSurface * s = ref->surface;

		if (delta > 0 || ref->use[category] != 0) {
			ref->use[category] += delta;
			s->useCount[category] += delta;
			s->shared()->useCount = (uint64_t) s->useCount[1] << 32 | s->useCount[0];
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapIncrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return useCountCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, 1);
}

IOReturn
IOSurfaceRootUserClient::trapDecrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return useCountCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, -1);
}

/*
 * Lock and unlock do not exclude anybody: the memory is shared and always mapped.
 * What matters is the seed, which tells readers that the pixels may have changed.
 */
IOReturn
IOSurfaceRootUserClient::lockCommon(uint32_t sid, uint32_t options, user_addr_t seedOut)
{
	struct minisurf_client_ref * ref;
	uint32_t seed = 0;

	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		struct minisurf_shared * sh = ref->surface->shared();

		if (!(options & kSurfaceLockReadOnly)) {
			sh->seed++;
		}
		seed = sh->seed;
	}
	IOLockUnlock(gSurfaceLock);
	if (ref == NULL) {
		return kIOReturnNotFound;
	}
	if (seedOut != 0) {
		copyout(&seed, seedOut, sizeof(seed));
	}
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return lockCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, (user_addr_t) (uintptr_t) p3);
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return lockCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, (user_addr_t) (uintptr_t) p3);
}

/*
 * A client drops a surface in two steps: "detach" while it still holds its own lock, and
 * "release" after it. A lookup of the same surface in between must make a fresh view.
 */
IOReturn
IOSurfaceRootUserClient::trapDetach(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	struct minisurf_client_ref * ref;

	IOLockLock(gSurfaceLock);
	ref = findRef((uint32_t) (uintptr_t) p1, false);
	if (ref != NULL) {
		ref->detached = true;
	}
	IOLockUnlock(gSurfaceLock);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	struct minisurf_client_ref * ref;
	uint32_t sid = (uint32_t) (uintptr_t) p1;

	IOLockLock(gSurfaceLock);
	ref = findRef(sid, true);
	if (ref == NULL) {
		ref = findRef(sid, false);
	}
	if (ref != NULL) {
		dropRef(ref);
	}
	IOLockUnlock(gSurfaceLock);
	return ref ? kIOReturnSuccess : kIOReturnNotFound;
}

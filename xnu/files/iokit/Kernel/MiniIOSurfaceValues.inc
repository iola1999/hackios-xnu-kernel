/* Requests about values start with the surface number in 8 bytes; the reply to a change is the new seed. */
static IOReturn
returnSeed(IOExternalMethodArguments * args, uint32_t seed)
{
	if (args->structureOutput != NULL && args->structureOutputSize >= sizeof(seed)) {
		memcpy(args->structureOutput, &seed, sizeof(seed));
		args->structureOutputSize = sizeof(seed);
	}
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodSetValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, seed = 0;
	OSObject * obj;
	OSArray * array;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnBadArgument;

	if (inSize < 12) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	obj = unserialize(in + 8, inSize - 8);
	array = OSDynamicCast(OSArray, obj);
	if (array == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref == NULL) {
		ret = kIOReturnNotFound;
	} else if (ref->surface->values != NULL) {
		MiniIOSurface * s = ref->surface;
		OSString * key = OSDynamicCast(OSString, array->getObject(1));
		OSDictionary * all = OSDynamicCast(OSDictionary, array->getObject(0));

		if (array->getCount() == 2 && key != NULL) {
			s->values->setObject(key, array->getObject(0));         /* [value, key] */
			ret = kIOReturnSuccess;
		} else if (array->getCount() == 1 && all != NULL) {
			s->values->merge(all);                                   /* [all the values] */
			ret = kIOReturnSuccess;
		}
		if (ret == kIOReturnSuccess) {
			s->updateValuesState();
			seed = s->shared()->valuesSeed;
		}
	}
	IOLockUnlock(gSurfaceLock);
	obj->release();
	return ret == kIOReturnSuccess ? returnSeed(args, seed) : ret;
}

IOReturn
IOSurfaceRootUserClient::methodRemoveValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, seed = 0;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (inSize < 9 || in[inSize - 1] != 0) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL && ref->surface->values != NULL) {
		MiniIOSurface * s = ref->surface;
		const char * key = (const char *) in + 8;

		if (key[0] == 0) {
			s->values->flushCollection();
		} else {
			s->values->removeObject(key);
		}
		s->updateValuesState();
		seed = s->shared()->valuesSeed;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret == kIOReturnSuccess ? returnSeed(args, seed) : ret;
}

/* The reply: 4 bytes unused, the seed, then the serialized value (a dictionary when all were asked for). */
IOReturn
IOSurfaceRootUserClient::methodCopyValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, header[2] = { 0, 0 };
	struct minisurf_client_ref * ref;
	OSSerialize * s = NULL;
	IOReturn ret = kIOReturnNotFound;
	uint32_t length = 0, total;

	if (inSize < 9 || in[inSize - 1] != 0) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL && ref->surface->values != NULL) {
		const char * key = (const char *) in + 8;
		OSObject * value = key[0] ? ref->surface->values->getObject(key) : ref->surface->values;

		header[1] = ref->surface->shared()->valuesSeed;
		ret = kIOReturnSuccess;
		if (value != NULL && (key[0] != 0 || ref->surface->values->getCount() != 0)) {
			s = OSSerialize::binaryWithCapacity(4096);
			if (s == NULL || !value->serialize(s)) {
				ret = kIOReturnNoMemory;
			} else {
				length = s->getLength();
			}
		}
	}
	IOLockUnlock(gSurfaceLock);

	total = (uint32_t) sizeof(header) + length;
	if (ret == kIOReturnSuccess) {
		if (args->structureOutputDescriptor != NULL) {
			IOMemoryDescriptor * out = args->structureOutputDescriptor;

			if (out->getLength() < total) {
				ret = kIOReturnNoSpace;
			} else if ((ret = out->prepare()) == kIOReturnSuccess) {
				out->writeBytes(0, header, sizeof(header));
				if (length != 0) {
					out->writeBytes(sizeof(header), s->text(), length);
				}
				out->complete();
				args->structureOutputDescriptorSize = total;
			}
		} else if (args->structureOutput == NULL || args->structureOutputSize < total) {
			ret = kIOReturnNoSpace;
		} else {
			memcpy(args->structureOutput, header, sizeof(header));
			if (length != 0) {
				memcpy((uint8_t *) args->structureOutput + sizeof(header), s->text(), length);
			}
			args->structureOutputSize = total;
		}
	}
	OSSafeReleaseNULL(s);
	return ret;
}

/* The 12-byte prefix is unspecified in 23G90; only the serialized array is meaningful. */
IOReturn
IOSurfaceRootUserClient::methodSetCoreVideoBridgedKeys(const uint8_t * in, size_t inSize)
{
	if (in == NULL || inSize < 16) {
		return kIOReturnBadArgument;
	}
	OSObject * obj = unserialize(in + 12, inSize - 12);
	OSArray * keys = OSDynamicCast(OSArray, obj);
	if (keys == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	if (fABI != MINISURF_ABI26) {
		IOLockUnlock(gSurfaceLock);
		obj->release();
		return kIOReturnUnsupported;
	}
	OSArray * old = fCoreVideoBridgedKeys;
	fCoreVideoBridgedKeys = keys;
	IOLockUnlock(gSurfaceLock);
	OSSafeReleaseNULL(old);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapRemoveCoreVideoBridgedValues(void * p1, void * p2,
    void * p3, void * p4, void * p5, void * p6)
{
	uint32_t seed = 0;
	IOLockLock(gSurfaceLock);
	if (fABI != MINISURF_ABI26) {
		IOLockUnlock(gSurfaceLock);
		return kIOReturnUnsupported;
	}
	struct minisurf_client_ref * ref = findRef((uint32_t) (uintptr_t) p1, false);
	if (ref == NULL || fCoreVideoBridgedKeys == NULL) {
		IOLockUnlock(gSurfaceLock);
		return kIOReturnBadArgument;
	}
	MiniIOSurface * s = ref->surface;
	bool changed = false;
	if (s->values != NULL) {
		for (unsigned i = 0; i < fCoreVideoBridgedKeys->getCount(); ++i) {
			OSString * key = OSDynamicCast(OSString, fCoreVideoBridgedKeys->getObject(i));
			if (key != NULL && s->values->getObject(key) != NULL) {
				s->values->removeObject(key);
				changed = true;
			}
		}
	}
	if (changed) {
		s->updateValuesState();
		seed = s->shared()->valuesSeed;
	}
	/* Bulk fields have no dictionary seed; a dictionary no-op returns zero. */
	minisurfClearBulk(s->shared(), fABI, MINISURF_BULK_MASK26);
	IOLockUnlock(gSurfaceLock);
	return copyout(&seed, (user_addr_t) (uintptr_t) p2, sizeof(seed));
}

IOReturn
IOSurfaceRootUserClient::trapRemoveBulkAttachments(void * p1, void * p2,
    void * p3, void * p4, void * p5, void * p6)
{
	IOLockLock(gSurfaceLock);
	if (fABI != MINISURF_ABI26) {
		IOLockUnlock(gSurfaceLock);
		return kIOReturnUnsupported;
	}
	struct minisurf_client_ref * ref = findRef((uint32_t) (uintptr_t) p1, false);
	bool valid = ref != NULL && minisurfClearBulk(ref->surface->shared(), fABI, (uint64_t) (uintptr_t) p2);
	IOLockUnlock(gSurfaceLock);
	return valid ? kIOReturnSuccess : kIOReturnBadArgument;
}

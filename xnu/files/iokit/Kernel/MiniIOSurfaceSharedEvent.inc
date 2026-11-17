/*
 * IOSurfaceSharedEvent, IOSurfaceSharedEventListener, and IOSurfaceMemoryPool support for
 * IOSurfaceRootUserClient (23G90 selectors 36, 38..41, 46..48 and traps 6..7).
 */

#define MINISURF_SHARED_EVENT_SLOTS 2048u

struct minisurf_event_slot {
	volatile uint64_t signaledValue;
	uint64_t          reserved;
};

struct minisurf_event_listener {
	struct minisurf_event_listener * next;
	uint64_t                         listenerID;
	OSAsyncReference64               asyncRef;
	uint32_t                         refs;
	bool                             removed;
};

class IOSurfaceSharedEventRight;

struct minisurf_event_notify {
	struct minisurf_event_notify *   next;
	IOSurfaceSharedEventRight *      right;
	struct minisurf_event_listener * listener;
	uint64_t                         eventObjPtr;
	uint64_t                         blockPtr;
	uint64_t                         targetValue;
	bool                             shouldSend;
};

class IOSurfaceSharedEventRight : public OSObject
{
	OSDeclareDefaultStructors(IOSurfaceSharedEventRight);

public:
	uint32_t                       slot;
	uint64_t                       traceID;
	uint64_t                       options;
	struct minisurf_event_notify * notifyHead;

	virtual bool
	init(void) APPLE_KEXT_OVERRIDE
	{
		return OSObject::init();
	}
	virtual void free(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOSurfaceSharedEventRight, OSObject);

class IOSurfacePoolRight : public OSObject
{
	OSDeclareDefaultStructors(IOSurfacePoolRight);

public:
	uint64_t poolID;

	virtual bool
	init(void) APPLE_KEXT_OVERRIDE
	{
		return OSObject::init();
	}
};

OSDefineMetaClassAndStructors(IOSurfacePoolRight, OSObject);

static IOLock *                    gSharedEventLock;
static IOBufferMemoryDescriptor *  gSharedEventMemory;
static IOSurfaceSharedEventRight * gSharedEventTable[MINISURF_SHARED_EVENT_SLOTS];
static uint64_t                    gSharedEventNextID = 1;
static uint64_t                    gMemoryPoolNextID = 1;

static void
minisurfSharedEventInit(void)
{
	vm_size_t bytes = MINISURF_SHARED_EVENT_SLOTS * sizeof(struct minisurf_event_slot);

	gSharedEventLock = IOLockAlloc();
	gSharedEventMemory = IOBufferMemoryDescriptor::inTaskWithOptions(
	    kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, bytes, PAGE_SIZE);
	if (gSharedEventMemory != NULL) {
		bzero(gSharedEventMemory->getBytesNoCopy(), bytes);
	}
}

static inline volatile uint64_t *
minisurfEventSlotPtr(uint32_t slot)
{
	struct minisurf_event_slot * base =
	    (struct minisurf_event_slot *) gSharedEventMemory->getBytesNoCopy();
	return &base[slot].signaledValue;
}

static void
minisurfListenerReleaseLocked(struct minisurf_event_listener * listener)
{
	if (listener == NULL || --listener->refs != 0) {
		return;
	}
	IOUserClient::releaseAsyncReference64(listener->asyncRef);
	IODelete(listener, struct minisurf_event_listener, 1);
}

void
IOSurfaceSharedEventRight::free(void)
{
	if (slot != 0 && slot < MINISURF_SHARED_EVENT_SLOTS && gSharedEventLock != NULL) {
		IOLockLock(gSharedEventLock);
		if (gSharedEventTable[slot] == this) {
			gSharedEventTable[slot] = NULL;
		}
		if (gSharedEventMemory != NULL) {
			__atomic_store_n(minisurfEventSlotPtr(slot), 0, __ATOMIC_RELEASE);
		}
		IOLockUnlock(gSharedEventLock);
		slot = 0;
	}
	OSObject::free();
}

IOMemoryMap *
IOSurfaceRootUserClient::ensureSharedEventMap(void)
{
	if (gSharedEventLock == NULL || gSharedEventMemory == NULL) {
		return NULL;
	}
	IOLockLock(gSharedEventLock);
	if (fSharedEventMap == NULL) {
		fSharedEventMap = gSharedEventMemory->createMappingInTask(
		    fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
	}
	IOMemoryMap * map = fSharedEventMap;
	IOLockUnlock(gSharedEventLock);
	return map;
}

static IOSurfaceSharedEventRight *
minisurfCopySharedEventRightInTask(task_t task, mach_port_name_t portName)
{
	OSObject * obj = NULL;

	if (portName == 0 || !task ||
	    IOUserClient::copyObjectForPortNameInTask(task, portName, &obj) != kIOReturnSuccess) {
		return NULL;
	}
	IOSurfaceSharedEventRight * right = OSDynamicCast(IOSurfaceSharedEventRight, obj);
	if (right == NULL || right->slot == 0 || right->slot >= MINISURF_SHARED_EVENT_SLOTS) {
		OSSafeReleaseNULL(obj);
		return NULL;
	}
	return right;
}

IOSurfaceSharedEventRight *
IOSurfaceRootUserClient::copySharedEventRight(mach_port_name_t portName)
{
	return minisurfCopySharedEventRightInTask(fTask, portName);
}

void
IOSurfaceRootUserClient::dropEventListeners(uint64_t listenerID, bool all)
{
	struct minisurf_event_notify * cancelled = NULL;

	if (gSharedEventLock == NULL) {
		return;
	}
	IOLockLock(gSharedEventLock);
	struct minisurf_event_listener ** prev = &fEventListeners;
	while (*prev != NULL) {
		struct minisurf_event_listener * cur = *prev;
		if (all || cur->listenerID == listenerID) {
			*prev = cur->next;
			cur->next = NULL;
			cur->removed = true;
			minisurfListenerReleaseLocked(cur);
		} else {
			prev = &cur->next;
		}
	}
	if (all) {
		OSSafeReleaseNULL(fSharedEventMap);
	}
	/* Reclaim queued notifications belonging to removed listeners immediately. */
	for (uint32_t s = 1; s < MINISURF_SHARED_EVENT_SLOTS; s++) {
		IOSurfaceSharedEventRight * right = gSharedEventTable[s];
		if (right == NULL) {
			continue;
		}
		struct minisurf_event_notify ** np = &right->notifyHead;
		while (*np != NULL) {
			struct minisurf_event_notify * n = *np;
			if (n->listener->removed) {
				*np = n->next;
				minisurfListenerReleaseLocked(n->listener);
				n->listener = NULL;
				n->next = cancelled;
				cancelled = n;
			} else {
				np = &n->next;
			}
		}
	}
	IOLockUnlock(gSharedEventLock);
	while (cancelled != NULL) {
		struct minisurf_event_notify * next = cancelled->next;
		OSSafeReleaseNULL(cancelled->right);
		IODelete(cancelled, struct minisurf_event_notify, 1);
		cancelled = next;
	}
}

IOReturn
IOSurfaceRootUserClient::methodCreateSharedEvent(IOExternalMethodArguments * args)
{
	if (args->scalarInputCount < 1 || args->scalarOutputCount < 2) {
		return kIOReturnBadArgument;
	}
	IOMemoryMap * map = ensureSharedEventMap();
	if (map == NULL) {
		return kIOReturnNoMemory;
	}
	IOSurfaceSharedEventRight * right = OSTypeAlloc(IOSurfaceSharedEventRight);
	if (right == NULL || !right->init()) {
		OSSafeReleaseNULL(right);
		return kIOReturnNoMemory;
	}
	uint32_t slot = 0;
	IOLockLock(gSharedEventLock);
	for (uint32_t i = 1; i < MINISURF_SHARED_EVENT_SLOTS; i++) {
		if (gSharedEventTable[i] == NULL) {
			slot = i;
			gSharedEventTable[i] = right;
			__atomic_store_n(minisurfEventSlotPtr(i), 0, __ATOMIC_RELEASE);
			break;
		}
	}
	if (slot != 0) {
		right->slot = slot;
		right->traceID = gSharedEventNextID++;
		right->options = args->scalarInput[0];
		right->notifyHead = NULL;
	}
	IOLockUnlock(gSharedEventLock);
	if (slot == 0) {
		right->release();
		return kIOReturnNoResources;
	}
	mach_port_name_t name = 0;
	copyPortNameForObjectInTask(fTask, right, &name);
	right->release();
	if (name == 0) {
		return kIOReturnNoMemory;
	}
	args->scalarOutput[0] = name;
	args->scalarOutput[1] = map->getVirtualAddress() +
	    (mach_vm_address_t) slot * sizeof(struct minisurf_event_slot);
	args->scalarOutputCount = 2;
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodOpenSharedEvent(IOExternalMethodArguments * args)
{
	if (args->scalarInputCount < 1 || args->scalarOutputCount < 4) {
		return kIOReturnBadArgument;
	}
	IOMemoryMap * map = ensureSharedEventMap();
	if (map == NULL) {
		return kIOReturnNoMemory;
	}
	IOSurfaceSharedEventRight * right = copySharedEventRight((mach_port_name_t) args->scalarInput[0]);
	if (right == NULL) {
		return kIOReturnNotFound;
	}
	args->scalarOutput[0] = 0;
	args->scalarOutput[1] = map->getVirtualAddress() +
	    (mach_vm_address_t) right->slot * sizeof(struct minisurf_event_slot);
	args->scalarOutput[2] = right->traceID;
	args->scalarOutput[3] = right->options;
	args->scalarOutputCount = 4;
	right->release();
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodAddEventListener(IOExternalMethodArguments * args)
{
	if (gSharedEventLock == NULL || args->asyncWakePort == MACH_PORT_NULL ||
	    args->asyncReference == NULL || args->asyncReferenceCount < 3) {
		return kIOReturnBadArgument;
	}
	struct minisurf_event_listener * l = IONew(struct minisurf_event_listener, 1);
	if (l == NULL) {
		IOUserClient::releaseNotificationPort(args->asyncWakePort);
		return kIOReturnNoMemory;
	}
	bzero(l, sizeof(*l));
	bcopy(args->asyncReference, l->asyncRef, sizeof(OSAsyncReference64));
	l->listenerID = l->asyncRef[kIOAsyncCalloutRefconIndex];
	l->refs = 1;
	l->removed = false;

	dropEventListeners(l->listenerID, false);
	IOLockLock(gSharedEventLock);
	l->next = fEventListeners;
	fEventListeners = l;
	IOLockUnlock(gSharedEventLock);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodRemoveEventListener(IOExternalMethodArguments * args)
{
	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	dropEventListeners(args->scalarInput[0], false);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodNotifyEventListener(IOExternalMethodArguments * args)
{
	if (gSharedEventLock == NULL || args->scalarInputCount < 5) {
		return kIOReturnBadArgument;
	}
	IOSurfaceSharedEventRight * right = copySharedEventRight((mach_port_name_t) args->scalarInput[0]);
	if (right == NULL) {
		return kIOReturnNotFound;
	}
	uint64_t listenerID = args->scalarInput[1];
	uint64_t eventObjPtr = args->scalarInput[2];
	uint64_t blockPtr = args->scalarInput[3];
	uint64_t targetValue = args->scalarInput[4];

	struct minisurf_event_notify * pending = IONew(struct minisurf_event_notify, 1);
	if (pending == NULL) {
		right->release();
		return kIOReturnNoMemory;
	}
	bzero(pending, sizeof(*pending));

	struct minisurf_event_listener * fireListener = NULL;
	OSAsyncReference64 fireRef;
	uint64_t curValue = 0;

	IOLockLock(gSharedEventLock);
	struct minisurf_event_listener * l = fEventListeners;
	while (l != NULL && (l->removed || l->listenerID != listenerID)) {
		l = l->next;
	}
	if (l == NULL) {
		IOLockUnlock(gSharedEventLock);
		IODelete(pending, struct minisurf_event_notify, 1);
		right->release();
		return kIOReturnNotFound;
	}
	curValue = __atomic_load_n(minisurfEventSlotPtr(right->slot), __ATOMIC_ACQUIRE);
	if (curValue >= targetValue) {
		l->refs++;
		fireListener = l;
		bcopy(l->asyncRef, fireRef, sizeof(fireRef));
	} else {
		right->retain();
		l->refs++;
		pending->right = right;
		pending->listener = l;
		pending->eventObjPtr = eventObjPtr;
		pending->blockPtr = blockPtr;
		pending->targetValue = targetValue;
		struct minisurf_event_notify ** tail = &right->notifyHead;
		while (*tail != NULL) {
			tail = &(*tail)->next;
		}
		*tail = pending;
		pending = NULL;
	}
	IOLockUnlock(gSharedEventLock);

	if (pending != NULL) {
		IODelete(pending, struct minisurf_event_notify, 1);
	}
	if (fireListener != NULL) {
		io_user_reference_t msgArgs[3] = { blockPtr, eventObjPtr, curValue };
		IOUserClient::sendAsyncResult64(fireRef, kIOReturnSuccess, msgArgs, 3);
		IOLockLock(gSharedEventLock);
		minisurfListenerReleaseLocked(fireListener);
		IOLockUnlock(gSharedEventLock);
	}
	right->release();
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::waitSharedEventInTask(task_t task, mach_port_name_t portName, uint64_t target, uint64_t timeoutMS)
{
	if (gSharedEventLock == NULL || gSharedEventMemory == NULL) {
		return kIOReturnNotReady;
	}
	IOSurfaceSharedEventRight * right = minisurfCopySharedEventRightInTask(task, portName);
	if (right == NULL) {
		return kIOReturnNotFound;
	}
	volatile uint64_t * slotPtr = minisurfEventSlotPtr(right->slot);

	if (__atomic_load_n(slotPtr, __ATOMIC_ACQUIRE) >= target) {
		right->release();
		return kIOReturnSuccess;
	}
	if (timeoutMS == 0) {
		right->release();
		return kIOReturnTimeout;
	}
	AbsoluteTime deadline = 0;
	bool bounded = timeoutMS < 3600000ULL;
	if (bounded) {
		clock_interval_to_deadline((uint32_t) timeoutMS, kMillisecondScale, &deadline);
	}
	IOLockLock(gSharedEventLock);
	while (__atomic_load_n(slotPtr, __ATOMIC_ACQUIRE) < target) {
		int w = bounded ? IOLockSleepDeadline(gSharedEventLock, right, deadline, THREAD_ABORTSAFE) :
		    IOLockSleep(gSharedEventLock, right, THREAD_ABORTSAFE);
		if (w == THREAD_TIMED_OUT || w == THREAD_INTERRUPTED) {
			break;
		}
	}
	bool ok = __atomic_load_n(slotPtr, __ATOMIC_ACQUIRE) >= target;
	IOLockUnlock(gSharedEventLock);
	right->release();
	return ok ? kIOReturnSuccess : kIOReturnTimeout;
}

IOReturn
IOSurfaceRootUserClient::signalSharedEventInTask(task_t task, mach_port_name_t portName, uint64_t value)
{
	if (gSharedEventLock == NULL || gSharedEventMemory == NULL) {
		return kIOReturnNotReady;
	}
	IOSurfaceSharedEventRight * right = minisurfCopySharedEventRightInTask(task, portName);
	if (right == NULL) {
		return kIOReturnNotFound;
	}
	volatile uint64_t * slotPtr = minisurfEventSlotPtr(right->slot);
	struct minisurf_event_notify * fired = NULL, ** firedTail = &fired;
	uint64_t cur;

	IOLockLock(gSharedEventLock);
	cur = __atomic_load_n(slotPtr, __ATOMIC_ACQUIRE);
	if (value > cur) {
		__atomic_store_n(slotPtr, value, __ATOMIC_RELEASE);
		cur = value;
	}
	IOLockWakeup(gSharedEventLock, right, false);

	struct minisurf_event_notify ** np = &right->notifyHead;
	while (*np != NULL) {
		struct minisurf_event_notify * n = *np;
		if (n->listener->removed || n->targetValue <= cur) {
			*np = n->next;
			n->next = NULL;
			n->shouldSend = !n->listener->removed;
			*firedTail = n;
			firedTail = &n->next;
		} else {
			np = &n->next;
		}
	}
	IOLockUnlock(gSharedEventLock);

	while (fired != NULL) {
		struct minisurf_event_notify * n = fired;
		fired = n->next;
		if (n->shouldSend) {
			io_user_reference_t msgArgs[3] = { n->blockPtr, n->eventObjPtr, cur };
			IOUserClient::sendAsyncResult64(n->listener->asyncRef, kIOReturnSuccess, msgArgs, 3);
		}
		IOLockLock(gSharedEventLock);
		minisurfListenerReleaseLocked(n->listener);
		IOLockUnlock(gSharedEventLock);
		OSSafeReleaseNULL(n->right);
		IODelete(n, struct minisurf_event_notify, 1);
	}
	right->release();
	return kIOReturnSuccess;
}

IOReturn
MiniIOSurfaceWaitSharedEvent(task_t task, mach_port_name_t portName, uint64_t target, uint64_t timeoutMS)
{
	return IOSurfaceRootUserClient::waitSharedEventInTask(task, portName, target, timeoutMS);
}

IOReturn
MiniIOSurfaceSignalSharedEvent(task_t task, mach_port_name_t portName, uint64_t value)
{
	return IOSurfaceRootUserClient::signalSharedEventInTask(task, portName, value);
}

IOReturn
IOSurfaceRootUserClient::trapWaitSharedEvent(void * p1, void * p2, void * p3,
    void * p4 __unused, void * p5 __unused, void * p6 __unused)
{
	return waitSharedEventInTask(fTask, (mach_port_name_t) (uintptr_t) p1,
	    (uint64_t) (uintptr_t) p2, (uint64_t) (uintptr_t) p3);
}

IOReturn
IOSurfaceRootUserClient::trapSignalSharedEvent(void * p1, void * p2,
    void * p3 __unused, void * p4 __unused, void * p5 __unused, void * p6 __unused)
{
	return signalSharedEventInTask(fTask, (mach_port_name_t) (uintptr_t) p1,
	    (uint64_t) (uintptr_t) p2);
}

IOReturn
IOSurfaceRootUserClient::methodCreateMemoryPool(IOExternalMethodArguments * args)
{
	if (args->scalarOutputCount < 2) {
		return kIOReturnBadArgument;
	}
	IOSurfacePoolRight * right = OSTypeAlloc(IOSurfacePoolRight);
	if (right == NULL || !right->init()) {
		OSSafeReleaseNULL(right);
		return kIOReturnNoMemory;
	}
	IOLockLock(gSurfaceLock);
	uint64_t poolID = gMemoryPoolNextID++;
	IOLockUnlock(gSurfaceLock);
	right->poolID = poolID;

	mach_port_name_t name = 0;
	copyPortNameForObjectInTask(fTask, right, &name);
	right->release();
	if (name == 0) {
		return kIOReturnNoMemory;
	}
	args->scalarOutput[0] = name;
	args->scalarOutput[1] = poolID;
	args->scalarOutputCount = 2;
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodMemoryPoolCommand(IOExternalMethodArguments * args)
{
	OSObject * obj = NULL;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	if (copyObjectForPortNameInTask(fTask, (mach_port_name_t) args->scalarInput[0], &obj) != kIOReturnSuccess) {
		return kIOReturnNotFound;
	}
	bool valid = OSDynamicCast(IOSurfacePoolRight, obj) != NULL;
	obj->release();
	return valid ? kIOReturnSuccess : kIOReturnBadArgument;
}

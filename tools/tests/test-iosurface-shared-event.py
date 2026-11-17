#!/usr/bin/env python3
"""Compile and exercise MiniIOSurfaceSharedEvent.inc under ASan/UBSan on macOS."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
INC = ROOT / "xnu/files/iokit/Kernel/MiniIOSurfaceSharedEvent.inc"

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using IOReturn = int;
using mach_port_t = uint32_t;
using mach_port_name_t = uint32_t;
using mach_vm_address_t = uint64_t;
using vm_size_t = size_t;
using task_t = int;
using AbsoluteTime = uint64_t;
using UInt32 = uint32_t;
using SInt64 = int64_t;
using io_user_reference_t = uint64_t;
using OSAsyncReference64 = io_user_reference_t[8];

constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnNoMemory = -1;
constexpr IOReturn kIOReturnNoResources = -2;
constexpr IOReturn kIOReturnBadArgument = -3;
constexpr IOReturn kIOReturnNotFound = -4;
constexpr IOReturn kIOReturnNotReady = -5;
constexpr IOReturn kIOReturnTimeout = -6;

constexpr mach_port_t MACH_PORT_NULL = 0;
constexpr unsigned kIOAsyncCalloutFuncIndex = 1;
constexpr unsigned kIOAsyncCalloutRefconIndex = 2;
constexpr unsigned kIODirectionInOut = 1;
constexpr unsigned kIOMemoryKernelUserShared = 2;
constexpr unsigned kIOMapAnywhere = 1;
constexpr unsigned kIOMapReadOnly = 2;
constexpr vm_size_t PAGE_SIZE = 16384;
constexpr task_t kernel_task = 0;
constexpr uint32_t kMillisecondScale = 1000000u;
constexpr int THREAD_ABORTSAFE = 1;
constexpr int THREAD_AWAKENED = 0;
constexpr int THREAD_TIMED_OUT = 1;
constexpr int THREAD_INTERRUPTED = 2;

#define APPLE_KEXT_OVERRIDE override
#define OSDeclareDefaultStructors(cls)
#define OSDefineMetaClassAndStructors(cls, super)
#define OSTypeAlloc(cls) (new cls())
#define IONew(type, count) (new type[count])
#define IODelete(ptr, type, count) (delete[] (ptr))
#define OSDynamicCast(cls, obj) dynamic_cast<cls *>(obj)
#define OSSafeReleaseNULL(ptr) do { if (ptr) { (ptr)->release(); (ptr) = nullptr; } } while (0)
#undef bzero
#define bzero(p, n) memset((p), 0, (n))
#undef bcopy
#define bcopy(src, dst, n) memcpy((dst), (src), (n))
#ifndef __unused
#define __unused __attribute__((unused))
#endif

static std::atomic<int> gLiveObjects{0};
static std::atomic<int> gLiveSendRights{0};

struct OSObject {
    std::atomic<int> refcount{1};
    OSObject() { ++gLiveObjects; }
    virtual ~OSObject() { --gLiveObjects; }
    virtual bool init() { return true; }
    virtual void free() { delete this; }
    void retain() {
        int prev = refcount.fetch_add(1, std::memory_order_acq_rel);
        assert(prev >= 1);
    }
    void release() {
        int next = refcount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        assert(next >= 0);
        if (next == 0) free();
    }
};

struct IOLock {
    std::mutex mtx;
    std::condition_variable cv;
    uint64_t epoch = 0;
};

static std::vector<IOLock *> gAllocatedLocks;
static IOLock *gSurfaceLock = nullptr;

static IOLock *IOLockAlloc(void) {
    auto *l = new IOLock();
    gAllocatedLocks.push_back(l);
    return l;
}
static void IOLockLock(IOLock *l) { l->mtx.lock(); }
static void IOLockUnlock(IOLock *l) { l->mtx.unlock(); }

static void clock_interval_to_deadline(uint32_t interval, uint32_t scale, AbsoluteTime *deadline) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    *deadline = ns + (uint64_t)interval * (uint64_t)scale;
}

static int IOLockSleep(IOLock *l, void * /*event*/, UInt32 /*inter*/) {
    std::unique_lock<std::mutex> ul(l->mtx, std::adopt_lock);
    uint64_t seen = l->epoch;
    l->cv.wait(ul, [&] { return l->epoch != seen; });
    ul.release();
    return THREAD_AWAKENED;
}

static int IOLockSleepDeadline(IOLock *l, void * /*event*/, AbsoluteTime deadline, UInt32 /*inter*/) {
    std::unique_lock<std::mutex> ul(l->mtx, std::adopt_lock);
    uint64_t seen = l->epoch;
    auto tp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline));
    bool woke = l->cv.wait_until(ul, tp, [&] { return l->epoch != seen; });
    ul.release();
    return woke ? THREAD_AWAKENED : THREAD_TIMED_OUT;
}

static void IOLockWakeup(IOLock *l, void * /*event*/, bool oneThread) {
    ++l->epoch;
    if (oneThread) l->cv.notify_one();
    else l->cv.notify_all();
}

struct IOMemoryMap : OSObject {
    std::vector<uint8_t> *backing = nullptr;
    uintptr_t bias = 0;
    mach_vm_address_t getVirtualAddress() const {
        return (mach_vm_address_t)(uintptr_t)backing->data();
    }
};

struct IOBufferMemoryDescriptor : OSObject {
    std::vector<uint8_t> bytes;
    static IOBufferMemoryDescriptor *inTaskWithOptions(task_t, unsigned, vm_size_t sz, vm_size_t) {
        auto *d = new IOBufferMemoryDescriptor();
        d->bytes.resize(sz, 0);
        return d;
    }
    void *getBytesNoCopy() { return bytes.data(); }
    IOMemoryMap *createMappingInTask(task_t task, mach_vm_address_t, unsigned) {
        auto *m = new IOMemoryMap();
        m->backing = &bytes;
        m->bias = (uintptr_t)task;
        return m;
    }
};

struct IOExternalMethodArguments {
    mach_port_t asyncWakePort = 0;
    io_user_reference_t *asyncReference = nullptr;
    uint32_t asyncReferenceCount = 0;
    const uint64_t *scalarInput = nullptr;
    uint32_t scalarInputCount = 0;
    uint64_t *scalarOutput = nullptr;
    uint32_t scalarOutputCount = 0;
};

struct AsyncDelivery {
    mach_port_t port;
    io_user_reference_t callback;
    io_user_reference_t refcon;
    IOReturn result;
    std::vector<io_user_reference_t> args;
};

static std::mutex gPortMutex;
static std::map<std::pair<task_t, mach_port_name_t>, OSObject *> gTaskPorts;
static mach_port_name_t gNextPortName = 100;
static std::vector<AsyncDelivery> gDeliveries;

static mach_port_t allocWakePort() {
    ++gLiveSendRights;
    return ++gNextPortName;
}

struct IOUserClient : OSObject {
    static void releaseNotificationPort(mach_port_t port) {
        if (port != MACH_PORT_NULL) {
            int left = --gLiveSendRights;
            assert(left >= 0);
        }
    }
    static void releaseAsyncReference64(OSAsyncReference64 ref) {
        mach_port_t port = (mach_port_t)(ref[0] & 0xffffffffu);
        releaseNotificationPort(port);
    }
    static IOReturn sendAsyncResult64(OSAsyncReference64 ref, IOReturn result,
                                      io_user_reference_t args[], UInt32 numArgs) {
        std::lock_guard<std::mutex> lk(gPortMutex);
        assert(gLiveSendRights.load() > 0);
        AsyncDelivery d;
        d.port = (mach_port_t)(ref[0] & 0xffffffffu);
        d.callback = ref[kIOAsyncCalloutFuncIndex];
        d.refcon = ref[kIOAsyncCalloutRefconIndex];
        d.result = result;
        d.args.assign(args, args + numArgs);
        gDeliveries.push_back(d);
        return kIOReturnSuccess;
    }
    static IOReturn copyPortNameForObjectInTask(task_t task, OSObject *object, mach_port_name_t *name) {
        std::lock_guard<std::mutex> lk(gPortMutex);
        object->retain();
        mach_port_name_t p = ++gNextPortName;
        gTaskPorts[{task, p}] = object;
        *name = p;
        return kIOReturnSuccess;
    }
    static IOReturn copyObjectForPortNameInTask(task_t task, mach_port_name_t name, OSObject **out) {
        std::lock_guard<std::mutex> lk(gPortMutex);
        auto it = gTaskPorts.find({task, name});
        if (it == gTaskPorts.end()) return kIOReturnNotFound;
        it->second->retain();
        *out = it->second;
        return kIOReturnSuccess;
    }
};

static mach_port_name_t dupPortToTask(task_t fromTask, mach_port_name_t name, task_t toTask) {
    std::lock_guard<std::mutex> lk(gPortMutex);
    auto it = gTaskPorts.find({fromTask, name});
    assert(it != gTaskPorts.end());
    it->second->retain();
    mach_port_name_t dst = ++gNextPortName;
    gTaskPorts[{toTask, dst}] = it->second;
    return dst;
}

static void closeTaskPort(task_t task, mach_port_name_t name) {
    OSObject *obj = nullptr;
    {
        std::lock_guard<std::mutex> lk(gPortMutex);
        auto it = gTaskPorts.find({task, name});
        assert(it != gTaskPorts.end());
        obj = it->second;
        gTaskPorts.erase(it);
    }
    obj->release();
}

class IOSurfaceSharedEventRight;
struct minisurf_event_listener;

class IOSurfaceRootUserClient : public IOUserClient {
public:
    task_t fTask;
    IOMemoryMap *fSharedEventMap = nullptr;
    struct minisurf_event_listener *fEventListeners = nullptr;

    explicit IOSurfaceRootUserClient(task_t t) : fTask(t) {}
    IOMemoryMap *ensureSharedEventMap(void);
    IOSurfaceSharedEventRight *copySharedEventRight(mach_port_name_t portName);
    static IOReturn waitSharedEventInTask(task_t task, mach_port_name_t portName, uint64_t target, uint64_t timeoutMS);
    static IOReturn signalSharedEventInTask(task_t task, mach_port_name_t portName, uint64_t value);
    void dropEventListeners(uint64_t listenerID, bool all);
    IOReturn methodCreateSharedEvent(IOExternalMethodArguments *args);
    IOReturn methodOpenSharedEvent(IOExternalMethodArguments *args);
    IOReturn methodNotifyEventListener(IOExternalMethodArguments *args);
    IOReturn methodAddEventListener(IOExternalMethodArguments *args);
    IOReturn methodRemoveEventListener(IOExternalMethodArguments *args);
    IOReturn methodCreateMemoryPool(IOExternalMethodArguments *args);
    IOReturn methodMemoryPoolCommand(IOExternalMethodArguments *args);
    IOReturn trapWaitSharedEvent(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6);
    IOReturn trapSignalSharedEvent(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6);
};

#include "MiniIOSurfaceSharedEvent.inc"

int main() {
    gSurfaceLock = IOLockAlloc();
    minisurfSharedEventInit();

    IOSurfaceRootUserClient clientA(1), clientB(2);

    // 1. Create shared event in task 1 and open viaselector 38 in task 1 & task 2.
    uint64_t createIn[1] = {0x5a};
    uint64_t createOut[2] = {};
    IOExternalMethodArguments args{};
    args.scalarInput = createIn;
    args.scalarInputCount = 1;
    args.scalarOutput = createOut;
    args.scalarOutputCount = 2;
    assert(clientA.methodCreateSharedEvent(&args) == kIOReturnSuccess);
    mach_port_name_t portA = (mach_port_name_t)createOut[0];
    auto *valPtrA = (const volatile uint64_t *)(uintptr_t)createOut[1];
    assert(portA != 0 && valPtrA != nullptr && *valPtrA == 0);

    uint64_t openInA[1] = {portA};
    uint64_t openOutA[4] = {};
    args = {};
    args.scalarInput = openInA;
    args.scalarInputCount = 1;
    args.scalarOutput = openOutA;
    args.scalarOutputCount = 4;
    assert(clientA.methodOpenSharedEvent(&args) == kIOReturnSuccess);
    assert(openOutA[1] == createOut[1]);
    assert(openOutA[2] >= 1 && openOutA[3] == 0x5a);

    mach_port_name_t portB = dupPortToTask(1, portA, 2);
    uint64_t openInB[1] = {portB};
    uint64_t openOutB[4] = {};
    args = {};
    args.scalarInput = openInB;
    args.scalarInputCount = 1;
    args.scalarOutput = openOutB;
    args.scalarOutputCount = 4;
    assert(clientB.methodOpenSharedEvent(&args) == kIOReturnSuccess);
    auto *valPtrB = (const volatile uint64_t *)(uintptr_t)openOutB[1];
    assert(valPtrB == valPtrA);
    assert(openOutB[2] == openOutA[2] && openOutB[3] == 0x5a);

    // 2. Monotonic signal via trap 7.
    assert(clientA.trapSignalSharedEvent((void *)(uintptr_t)portA, (void *)5, nullptr, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(*valPtrB == 5);
    assert(clientB.trapSignalSharedEvent((void *)(uintptr_t)portB, (void *)3, nullptr, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(*valPtrA == 5);

    // 3. Wait via trap 6: immediate hit, zero-timeout miss, bounded timeout miss, and multi-waiter wakeup.
    assert(clientB.trapWaitSharedEvent((void *)(uintptr_t)portB, (void *)5, (void *)10, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(clientB.trapWaitSharedEvent((void *)(uintptr_t)portB, (void *)6, (void *)0, nullptr, nullptr, nullptr) == kIOReturnTimeout);
    assert(clientB.trapWaitSharedEvent((void *)(uintptr_t)portB, (void *)6, (void *)15, nullptr, nullptr, nullptr) == kIOReturnTimeout);

    std::atomic<int> woke12{0}, woke20{0};
    std::thread waiter1([&] {
        IOReturn r = clientB.trapWaitSharedEvent((void *)(uintptr_t)portB, (void *)12, (void *)2000, nullptr, nullptr, nullptr);
        assert(r == kIOReturnSuccess);
        woke12 = 1;
    });
    std::thread waiter2([&] {
        IOReturn r = clientB.trapWaitSharedEvent((void *)(uintptr_t)portB, (void *)20, (void *)2000, nullptr, nullptr, nullptr);
        assert(r == kIOReturnSuccess);
        woke20 = 1;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    assert(clientA.trapSignalSharedEvent((void *)(uintptr_t)portA, (void *)15, nullptr, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    waiter1.join();
    assert(woke12 == 1 && woke20 == 0);
    assert(clientA.trapSignalSharedEvent((void *)(uintptr_t)portA, (void *)20, nullptr, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    waiter2.join();
    assert(woke20 == 1);

    // 4. Listener registration (40), immediate & deferred notify (39), and removal (41).
    mach_port_t wakePort = allocWakePort();
    OSAsyncReference64 asyncRef = {wakePort, 0x1111, 0x2222};
    args = {};
    args.asyncWakePort = wakePort;
    args.asyncReference = asyncRef;
    args.asyncReferenceCount = 3;
    assert(clientB.methodAddEventListener(&args) == kIOReturnSuccess);

    // Immediate fire (20 >= 20).
    uint64_t notifyImmediate[5] = {portB, 0x2222, 0xaaaa, 0xbbbb, 20};
    args = {};
    args.scalarInput = notifyImmediate;
    args.scalarInputCount = 5;
    assert(clientB.methodNotifyEventListener(&args) == kIOReturnSuccess);
    assert(gDeliveries.size() == 1);
    assert(gDeliveries[0].args[0] == 0xbbbb && gDeliveries[0].args[1] == 0xaaaa && gDeliveries[0].args[2] == 20);

    // Queued fire (target 30) + queued cancelled (target 50).
    uint64_t notify30[5] = {portB, 0x2222, 0xcccc, 0xdddd, 30};
    uint64_t notify50[5] = {portB, 0x2222, 0xeeee, 0xffff, 50};
    args.scalarInput = notify30;
    assert(clientB.methodNotifyEventListener(&args) == kIOReturnSuccess);
    args.scalarInput = notify50;
    assert(clientB.methodNotifyEventListener(&args) == kIOReturnSuccess);
    assert(gDeliveries.size() == 1);

    assert(clientA.trapSignalSharedEvent((void *)(uintptr_t)portA, (void *)35, nullptr, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(gDeliveries.size() == 2);
    assert(gDeliveries[1].args[0] == 0xdddd && gDeliveries[1].args[1] == 0xcccc && gDeliveries[1].args[2] == 35);

    uint64_t removeIn[1] = {0x2222};
    args = {};
    args.scalarInput = removeIn;
    args.scalarInputCount = 1;
    assert(clientB.methodRemoveEventListener(&args) == kIOReturnSuccess);
    assert(gLiveSendRights == 0);

    // Signaling 60 after listener removal must not deliver cancelled notify50.
    assert(clientA.trapSignalSharedEvent((void *)(uintptr_t)portA, (void *)60, nullptr, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(gDeliveries.size() == 2);

    // 5. Memory pool (46, 47, 48) and wrong-port-class rejection.
    uint64_t poolOut[2] = {};
    args = {};
    args.scalarOutput = poolOut;
    args.scalarOutputCount = 2;
    assert(clientA.methodCreateMemoryPool(&args) == kIOReturnSuccess);
    mach_port_name_t poolPort = (mach_port_name_t)poolOut[0];
    assert(poolPort != 0 && poolOut[1] >= 1);

    uint64_t poolCmd[1] = {poolPort};
    args = {};
    args.scalarInput = poolCmd;
    args.scalarInputCount = 1;
    assert(clientA.methodMemoryPoolCommand(&args) == kIOReturnSuccess);
    uint64_t wrongCmd[1] = {portA};
    args.scalarInput = wrongCmd;
    assert(clientA.methodMemoryPoolCommand(&args) == kIOReturnBadArgument);

    // 6. Teardown and zero-leak verification.
    closeTaskPort(1, poolPort);
    closeTaskPort(1, portA);
    closeTaskPort(2, portB);
    clientA.dropEventListeners(0, true);
    clientB.dropEventListeners(0, true);
    clientA.release(); // wait: stack objects! release only gSharedEventMemory
    return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "shared_event.cc"
        exe = Path(tmp) / "shared_event"
        # Replace stack clientA.release() with clean teardown check
        code = HARNESS.replace(
            "clientA.release(); // wait: stack objects! release only gSharedEventMemory",
            "OSSafeReleaseNULL(gSharedEventMemory); assert(gLiveSendRights == 0 && gLiveObjects == 2);"
        )
        src.write_text(code)
        subprocess.run([
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-O1", "-fsanitize=address,undefined",
            "-I", str(INC.parent), str(src), "-o", str(exe)
        ], check=True)
        subprocess.run([str(exe)], check=True)
        print("PASS: MiniIOSurfaceSharedEvent create/open, monotonic signal, bounded wait, async listener, and memory pool")

if __name__ == "__main__":
    main()

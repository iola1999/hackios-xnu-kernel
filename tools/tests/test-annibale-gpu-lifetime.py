#!/usr/bin/env python3
"""Execute AnnibaleGPU's production BO methods with checked descriptor ownership."""

from pathlib import Path
import argparse
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/files/iokit/Kernel/AnnibaleGPU.cpp"
MEMORY = SOURCE.with_name("AnnibaleGPUMemory.inc")
SUBMIT = SOURCE.with_name("AnnibaleGPUSubmit.inc")


def function(source, name):
    position = source.index("\n" + name + "(")
    start = source.rfind("\n", 0, position - 1) + 1
    return source[start : source.index("\n}\n", position) + 3]


PRELUDE = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
using IOByteCount = uint64_t;
using addr64_t = uint64_t;
using user_addr_t = uint64_t;
using vm_size_t = uint64_t;
using task_t = void *;
using pid_t = int;
using IOReturn = int;
struct IOExternalMethodArguments;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNoMemory,
       kIOReturnNotReady, kIOReturnNotFound, kIOReturnUnsupported,
       kIOReturnVMError, kIOReturnTimeout, kIOReturnNoResources, kIOReturnNoSpace };
enum { GPU_REFUSE_NOT_READY = 1, GPU_REFUSE_NO_RANGE = 2, GPU_REFUSE_NO_TABLE = 3 };
#define GPU_KEEP_FROM (256u << 10)
enum { kIOMemoryMapperNone, kIODirectionInOut = 1,
       kIOMemoryKernelUserShared = 2, kIOMapWriteCombineCache = 4, kIODirectionOut = 8 };
constexpr IOReturn KERN_PROTECTION_FAILURE = 100;
#ifndef HOST_PAGE_SIZE
#define HOST_PAGE_SIZE 16384
#endif
constexpr uint64_t PAGE_SIZE = HOST_PAGE_SIZE;
constexpr uint64_t NSEC_PER_USEC = 1000, NSEC_PER_MSEC = 1000000,
                   NSEC_PER_SEC = 1000000000;
static void *kernel_task;
static uint64_t now;
static uint64_t mach_absolute_time() { return now; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *t) { *t = ns; }
static void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) { *ns = t; }
static void delay_for_interval(uint64_t n, uint64_t unit) { now += n * unit; }
static uint64_t round_page_64(uint64_t n) { return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }
static void IOLog(const char *, ...) {}
static void annibale_tl(const char *, ...) {}
static void proc_selfname(char *name, int size) { snprintf(name, size, "test"); }
static int copyin(user_addr_t from, void *to, size_t size) {
    memcpy(to, reinterpret_cast<void *>(from), size); return 0;
}
#define TUNABLE(type, name, key, value) type name = value
#define IONew(type, count) new type[count]
#define IODelete(ptr, type, count) delete[] ptr
#define OSDynamicCast(type, ptr) dynamic_cast<type *>(ptr)
#define OSSafeReleaseNULL(ptr) do { if (ptr) { (ptr)->release(); (ptr) = nullptr; } } while (0)
struct IOLock { unsigned depth = 0; };
static IOLock lock; static IOLock *gGPULock = &lock;
static unsigned releases_in_lock, drops_in_lock;
static void IOLockLock(IOLock *l) { assert(l && l->depth++ == 0); }
static void IOLockUnlock(IOLock *l) { assert(l && l->depth-- == 1); }
struct gpu_io_stat { uint64_t queueing, span; };
static void gpuLockIoctl(uint32_t) { IOLockLock(gGPULock); }
static uint64_t ioLock(struct gpu_io_stat *, uint32_t selector) { gpuLockIoctl(selector); return 0; }
static void ioUnlock(struct gpu_io_stat *, uint64_t) { IOLockUnlock(gGPULock); }
static void depthWaitEntry(uint32_t, uint32_t) {}
static void depthWaitExit(uint32_t, uint64_t) {}
static void *current_proc() { return nullptr; }
static int proc_pid(void *) { return 1; }
static void *current_thread() { return nullptr; }
static uint64_t thread_tid(void *) { return 1; }
#define GPU_LOCK_NOTICE_US 2000
#define GPU_IMPORT_NOTICE_US 2000
static void gpuNoteSubmit(pid_t) {}

class IOMemoryDescriptor {
public:
    unsigned refs = 1;
    bool freed = false;
    uint64_t size, segment_limit = UINT64_MAX;
    uint8_t *bytes;
    IOMemoryDescriptor *parent = nullptr;
    static std::vector<IOMemoryDescriptor *> allocated;
    explicit IOMemoryDescriptor(uint64_t n) : size(n), bytes((uint8_t *)calloc(1, n)) {
        assert(bytes); allocated.push_back(this);
    }
    virtual ~IOMemoryDescriptor() { free(bytes); }
    void retain() { assert(!freed && refs); ++refs; }
    void release() {
        assert(!freed && refs);
        if (gGPULock->depth != 0) ++releases_in_lock;
        if (--refs == 0) { freed = true; if (parent) parent->release(); }
    }
    uint64_t getLength() const { assert(!freed); return size; }
    /* memoryBuffer() wires program memory only with gpu_coherent=1, which no case sets */
    static IOMemoryDescriptor *withAddressRange(uint64_t, uint64_t, int, task_t) { abort(); }
    IOReturn prepare() { abort(); }
    void *getBytesNoCopy() { assert(!freed); return bytes; }
    addr64_t getPhysicalSegment(uint64_t from, IOByteCount *length, int) {
        assert(!freed && from < size);
        *length = std::min(size - from, segment_limit);
        return 0x100000000ull + from;
    }
};
std::vector<IOMemoryDescriptor *> IOMemoryDescriptor::allocated;
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    using IOMemoryDescriptor::IOMemoryDescriptor;
    static uint64_t next_segment_limit;
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *, unsigned, uint64_t size, uint64_t) {
        auto *m = new IOBufferMemoryDescriptor(size);
        m->segment_limit = next_segment_limit; return m;
    }
};
uint64_t IOBufferMemoryDescriptor::next_segment_limit = UINT64_MAX;
class IOSubMemoryDescriptor : public IOMemoryDescriptor {
public:
    using IOMemoryDescriptor::IOMemoryDescriptor;
    static IOSubMemoryDescriptor *withSubRange(IOMemoryDescriptor *p, uint64_t from, uint64_t n, int) {
        if (from > p->size || n > p->size - from) return nullptr;
        auto *m = new IOSubMemoryDescriptor(n); p->retain(); m->parent = p; return m;
    }
};
struct MiniIOSurface {
    IOMemoryDescriptor *memory;
    IOMemoryDescriptor *backing = nullptr;
    uint64_t backingOffset = 0;
    uint8_t *kbase;
    uint8_t *kpixels = nullptr;
    unsigned held = 1;
    uint32_t drawn = 0;
    uint32_t cpuSyncedFence = 0;
    void *client = nullptr;
    struct { uint64_t allocSize; uint32_t offset, bytesPerRow, width, height, pixelFormat, planeCount; } info{};
    struct Shared { uint32_t seed = 0; } state;
    Shared *shared() { return &state; }
};
static MiniIOSurface *surface_lookup;
static MiniIOSurface *MiniIOSurfaceCopy(uint32_t) {
    if (surface_lookup) ++surface_lookup->held;
    return surface_lookup;
}
static void MiniIOSurfaceDrop(MiniIOSurface *s) {
    if (gGPULock->depth != 0) ++drops_in_lock;
    assert(s->held); if (--s->held == 0) s->memory->release();
}
struct IOUserClient {
    bool inactive = false;
    bool isInactive() { return inactive; }
    void terminate() { inactive = true; }
    void free() {}
};
static bool a830;
static bool annibale_platform_is_annibale() { return a830; }
static bool ready, mapped_ok, unmap_ok, fence_done, recover_ok;
static unsigned map_calls, unmap_calls, freed_addresses, done_calls, submit_calls, map_fail_at;
static uint32_t submit_answer;
static uint64_t next_address, last_base, base_phys, wrong_pages;
static uint32_t gpu_log, gpu_va;
static bool gGPUSaidSetIova, gWaitLines, gGPUIoLog;
static uint32_t annibale_gpu_last();
static bool gpuWait(uint32_t fence, uint64_t milliseconds);
static bool annibale_gpu_ready() { return ready; }
static uint64_t annibale_gpu_address(uint64_t size, uint32_t *refusal) {
    if (!ready) { if (refusal) *refusal = GPU_REFUSE_NOT_READY; return 0; }
    uint64_t result = next_address; next_address += size; last_base = result; return result;
}
static void annibale_gpu_address_free(uint64_t, uint64_t) { ++freed_addresses; }
static bool annibale_gpu_map(uint64_t address, uint64_t phys, bool) {
    /* every 4 KB of a buffer is entered at its own physical page */
    if (address == last_base) base_phys = phys;
    else wrong_pages += phys - base_phys != address - last_base;
    return ++map_calls != map_fail_at;
}
static bool annibale_gpu_mapped() { return mapped_ok; }
static bool annibale_gpu_unmap(uint64_t, uint64_t) { ++unmap_calls; return unmap_ok; }
static unsigned busy_for;                       /* annibale_gpu_done() says no so many times first */
static bool (*done_hook)(uint32_t);
static bool annibale_gpu_done(uint32_t f) { ++done_calls; return done_hook ? done_hook(f) : (fence_done && done_calls > busy_for); }
static uint32_t annibale_gpu_last() { return 7; }
static uint32_t retired_now, retired_slot1;
static uint32_t annibale_gpu_retired() { return retired_now; }
static uint32_t annibale_gpu_retired_for(uint32_t f) { return (f & 0x80000000u) ? retired_slot1 : retired_now; }
static uint32_t context_depth = 4;
static uint32_t adreno8_context_depth() { return context_depth; }
static uint32_t annibale_gpu_submit(const uint64_t *, const uint32_t *, uint32_t count) {
    assert(count); ++submit_calls; return submit_answer;
}
static unsigned flush_calls;
static void *last_flush_ptr;
static uint64_t last_flush_size;
static void annibale_gpu_to_memory(void *start, uint64_t size) {
    if (start != nullptr || size != 0) {
        ++flush_calls;
        last_flush_ptr = start;
        last_flush_size = size;
    }
}
static void annibale_gpu_report(const char *) {}
static bool annibale_gpu_recover() { return recover_ok; }
static void annibale_gpu_worked(uint64_t *time, uint32_t *count, uint32_t by[7]) {
    *time = *count = 0; memset(by, 0, 7 * sizeof(*by));
}
static void annibale_gpu_space(uint64_t *, uint32_t *, uint64_t *, unsigned *, unsigned *, uint64_t *, uint32_t *) {}
static void annibale_gpu_speed(uint32_t *mhz, uint32_t *changes) { *mhz = *changes = 0; }
static uint32_t annibale_gpu_irqs() { return 0; }
static void annibale_gpu_pause(uint32_t, uint32_t poll_us, uint32_t) { delay_for_interval(poll_us, NSEC_PER_USEC); }
'''


CASES = r'''
static void reset() {
    assert(gGPULock->depth == 0);
    for (auto *m : IOMemoryDescriptor::allocated) delete m;
    IOMemoryDescriptor::allocated.clear();
    a830 = false; ready = mapped_ok = unmap_ok = fence_done = true;
    recover_ok = false;
    map_calls = unmap_calls = freed_addresses = done_calls = submit_calls = map_fail_at = 0;
    releases_in_lock = drops_in_lock = 0;
    wrong_pages = busy_for = 0; context_depth = 4; done_hook = nullptr;
    retired_now = retired_slot1 = 0;
    submit_answer = 17; next_address = 0x40000000; now = 0;
    gGPUReported = gGPUCached = gGPUCoherent = false; gGPUKeep = 0;
    flush_calls = 0; last_flush_ptr = nullptr; last_flush_size = 0;
    IOBufferMemoryDescriptor::next_segment_limit = UINT64_MAX;
    surface_lookup = nullptr; memset(&gGPUStat, 0, sizeof(gGPUStat));
}
static AnnibaleGPUUserClient client() {
    AnnibaleGPUUserClient c{}; c.fBuffers = new gpu_buffer[GPU_BUFFERS]{}; c.fKept = new gpu_kept[GPU_KEPT]{}; return c;
}
static gpu_buffer *allocate(AnnibaleGPUUserClient &c, uint64_t size = PAGE_SIZE) {
    msm_gem_new request{size, 0, 0}; struct gpu_io_stat io{};
    assert(c.gemNew(&request, nullptr, &io) == kIOReturnSuccess);
    return &c.fBuffers[request.handle - 1];
}
static void healthy() {
    reset(); auto c = client(); auto *b = allocate(c); auto *m = b->memory;
    c.freeBuffer(b); assert(m->freed && unmap_calls == 1 && freed_addresses == 1); c.free();
}
static void stopped() {
    reset(); auto c = client(); auto *b = allocate(c); auto *m = b->memory;
    b->fence = 7; ready = false; c.freeBuffer(b);
    assert(c.fLeftCount == 1 && !m->freed && done_calls == 0);
    c.reap(); c.clientClose(); assert(!m->freed && done_calls == 0 && freed_addresses == 0); c.free();
}
static void failed_reap() {
    reset(); auto c = client(); auto *b = allocate(c); auto *m = b->memory;
    b->fence = 7; fence_done = false; c.freeBuffer(b); assert(c.fLeftCount == 1);
    fence_done = true; unmap_ok = false; c.reap();
    assert(c.fLeftCount == 1 && !m->freed && gGPUReported && freed_addresses == 0);
    c.reap(); assert(unmap_calls == 1); c.clientClose(); assert(!m->freed); c.free();
}
static void completed_reap() {
    for (bool cached : {false, true}) {
        reset(); auto c = client(); gGPUKeep = cached; auto *b = allocate(c, GPU_KEEP_FROM); auto *m = b->memory;
        b->fence = 7; fence_done = false; c.freeBuffer(b); assert(c.fLeftCount == 1);
        fence_done = true; c.reap(); assert(c.fLeftCount == 0);
        if (cached) {
            assert(!m->freed && c.fKeptBytes == GPU_KEEP_FROM && unmap_calls == 0);
            assert(c.dropKept(&c.fKept[0]) && m->freed);
        } else assert(m->freed && unmap_calls == 1 && freed_addresses == 1);
        c.free();
    }
}
static void failed_free() {
    reset(); auto c = client(); auto *b = allocate(c); auto *m = b->memory;
    unmap_ok = false; c.freeBuffer(b);
    assert(!m->freed && m->refs == 1 && b->memory == m && c.fBufferCount == 1);
    assert(gGPUReported && freed_addresses == 0);
    msm_gem_new request{PAGE_SIZE, 0, 0}; struct gpu_io_stat io{}; assert(c.gemNew(&request, nullptr, &io) == kIOReturnNotReady); c.free();
}
static void partial_map() {
    for (bool commit_failure : {false, true}) for (bool cleanup : {false, true}) {
        reset(); auto c = client(); map_fail_at = commit_failure ? 0 : 2;
        mapped_ok = !commit_failure; unmap_ok = cleanup;
        msm_gem_new request{2 * PAGE_SIZE, 0, 0}; struct gpu_io_stat io{};
        assert(c.gemNew(&request, nullptr, &io) == kIOReturnNotReady);
        auto *m = IOMemoryDescriptor::allocated.back();
        assert(m->freed == cleanup && freed_addresses == unsigned(cleanup));
        assert(unmap_calls == 1 && (cleanup || (m->refs == 1 && gGPUReported))); c.free();
    }
    reset(); auto c = client(); IOBufferMemoryDescriptor::next_segment_limit = 1024;
    msm_gem_new request{PAGE_SIZE, 0, 0}; struct gpu_io_stat io{};
    assert(c.gemNew(&request, nullptr, &io) == kIOReturnNotReady);
    assert(map_calls == 0 && IOMemoryDescriptor::allocated.back()->freed); c.free();
}
static void segments() {
    for (uint64_t limit : {uint64_t(4096), uint64_t(8192), uint64_t(PAGE_SIZE), UINT64_MAX}) {
        reset(); auto c = client(); IOBufferMemoryDescriptor::next_segment_limit = limit;
        allocate(c, 3 * PAGE_SIZE);
        assert(map_calls == 4 * PAGE_SIZE / 4096 && wrong_pages == 0); c.free();
    }
}
static void kept() {
    reset(); auto c = client(); gGPUKeep = 1; auto *b = allocate(c, GPU_KEEP_FROM); auto *m = b->memory;
    c.freeBuffer(b); assert(!m->freed && c.fKeptBytes == GPU_KEEP_FROM);
    unmap_ok = false; assert(!c.dropKept(&c.fKept[0]));
    assert(!m->freed && c.fKept[0].memory == m && c.fKeptBytes == GPU_KEEP_FROM);
    c.clientClose(); assert(!m->freed); c.free();
    reset(); c = client(); gGPUKeep = 1;
    for (unsigned i = 0; i < GPU_KEPT; ++i) {
        auto *memory = new IOBufferMemoryDescriptor(GPU_KEEP_FROM);
        assert(c.keep(memory, memory->bytes, i * GPU_KEEP_FROM, GPU_KEEP_FROM, 0));
    }
    auto *extra = new IOBufferMemoryDescriptor(GPU_KEEP_FROM); unmap_ok = false;
    assert(!c.keep(extra, extra->bytes, 0xf0000000, GPU_KEEP_FROM, 0));
    assert(unmap_calls == 1 && !extra->freed); extra->release(); c.free();
    reset(); c = client(); gGPUKeep = 1; b = allocate(c, GPU_KEEP_FROM); m = b->memory;
    c.freeBuffer(b); b = allocate(c, GPU_KEEP_FROM);
    assert(b->memory == m && c.fKeptBytes == 0 && unmap_calls == 0);
    c.freeBuffer(b); assert(c.dropKept(&c.fKept[0]) && m->freed); c.free();
    reset(); c = client(); gGPUKeep = 1; b = allocate(c, GPU_KEEP_FROM); m = b->memory;
    c.freeBuffer(b); unmap_ok = false; now = (GPU_KEPT_SECONDS + 1ull) * NSEC_PER_SEC;
    msm_gem_new request{GPU_KEEP_FROM, 0, 0}; struct gpu_io_stat io{};
    assert(c.gemNew(&request, nullptr, &io) == kIOReturnNotReady && !m->freed && c.fKept[0].memory == m); c.free();
}
static void failed_surface() {
    reset(); auto c = client(); auto *root = new IOBufferMemoryDescriptor(3 * PAGE_SIZE);
    MiniIOSurface s{}; s.memory = root; s.kbase = root->bytes; s.info.allocSize = 2 * PAGE_SIZE;
    surface_lookup = &s; map_fail_at = 2; unmap_ok = false;
    annibale_surface request{}; struct gpu_io_stat io{};
    assert(c.surfaceBuffer(&request, &io) == kIOReturnNotReady);
    assert(c.fBufferCount == 1 && c.fBuffers[request.handle - 1].memory == root && !c.fBuffers[request.handle - 1].pooled);
    MiniIOSurfaceDrop(&s); assert(!root->freed && root->refs == 1); c.free();
}
static void surface_ownership() {
    for (bool cleanup : {false, true}) {
        reset(); auto c = client(); auto *root = new IOBufferMemoryDescriptor(2 * PAGE_SIZE);
        MiniIOSurface s{}; s.memory = root; s.kbase = root->bytes; s.info.allocSize = PAGE_SIZE;
        surface_lookup = &s; annibale_surface request{}; struct gpu_io_stat io{};
        assert(c.surfaceBuffer(&request, &io) == kIOReturnSuccess && s.held == 2);
        auto *b = &c.fBuffers[request.handle - 1]; auto *sub = b->memory;
        unmap_ok = cleanup; c.freeBuffer(b); MiniIOSurfaceDrop(&s);
        assert(root->freed == cleanup && sub->freed == cleanup);
        if (!cleanup) {
            assert(b->memory == sub && b->surface == &s && s.held == 1);
            c.clientClose(); assert(!root->freed && !sub->freed);
        }
        c.free();
    }
}
static void full_pending() {
    reset(); auto c = client(); std::vector<IOMemoryDescriptor *> backing;
    for (unsigned i = 0; i <= GPU_LEFT; ++i) {
        auto *b = allocate(c); b->fence = 7; backing.push_back(b->memory);
    }
    gGPUReported = true;
    for (unsigned i = 0; i <= GPU_LEFT; ++i) c.freeBuffer(&c.fBuffers[i]);
    assert(c.fLeftCount == GPU_LEFT && c.fBufferCount == 1 && c.fBuffers[GPU_LEFT].memory == backing.back());
    c.clientClose(); c.free();
    for (auto *m : backing) assert(!m->freed && m->refs == 1);
    assert(done_calls == 0 && freed_addresses == 0);
}
static void long_pass() {
    /* fLeft is full while one submission runs for about 7 s (a camera still-image pass): the next
       free waits it out across more than one GPU_PATIENCE_MS wait and nothing is reported */
    reset(); auto c = client(); std::vector<IOMemoryDescriptor *> backing;
    for (unsigned i = 0; i <= GPU_LEFT; ++i) {
        auto *b = allocate(c); b->fence = 7; backing.push_back(b->memory);
    }
    busy_for = 7 * 5000;                        /* gpuWait() looks every 200 us */
    IOLockLock(gGPULock);
    for (unsigned i = 0; i <= GPU_LEFT; ++i) c.freeBuffer(&c.fBuffers[i]);
    c.reap();
    IOLockUnlock(gGPULock);
    assert(!gGPUReported && !MiniIOSurfaceGPUStopped() && now > GPU_PATIENCE_MS * NSEC_PER_MSEC);
    assert(c.fLeftCount == 0 && c.fBufferCount == 0);
    for (auto *m : backing) assert(m->freed);
    c.free();

    /* a core whose retired timestamp stands still: reported after GPU_STALL_MS, the last buffer kept */
    reset(); c = client(); backing.clear(); retired_now++;
    for (unsigned i = 0; i <= GPU_LEFT; ++i) {
        auto *b = allocate(c); b->fence = 7; backing.push_back(b->memory);
    }
    fence_done = false;
    IOLockLock(gGPULock);
    for (unsigned i = 0; i <= GPU_LEFT; ++i) c.freeBuffer(&c.fBuffers[i]);
    IOLockUnlock(gGPULock);
    assert(gGPUReported && MiniIOSurfaceGPUStopped() && now >= GPU_STALL_MS * NSEC_PER_MSEC);
    assert(c.fLeftCount == GPU_LEFT && c.fBufferCount == 1 && !backing.back()->freed);
    c.clientClose(); c.free();
}
static void failed_submit() {
    for (bool recovered : {false, true}) {
        reset(); auto c = client(); auto *b = allocate(c); auto *m = b->memory;
        msm_gem_submit_bo bo{0, 1, 0}; msm_gem_submit_cmd cmd{};
        cmd.type = MSM_SUBMIT_CMD_BUF; cmd.size = 4;
        msm_gem_submit request{}; request.nr_cmds = request.nr_bos = 1;
        request.bos = reinterpret_cast<uint64_t>(&bo); request.cmds = reinterpret_cast<uint64_t>(&cmd);
        submit_answer = 0; recover_ok = recovered; struct gpu_io_stat io{};
        assert(c.submit(&request, &io) == kIOReturnTimeout && gGPUReported && b->fence == 0);
        assert(submit_calls == (recovered ? 2u : 1u)); c.freeBuffer(b);
        assert(!m->freed && freed_addresses == 0);
        MiniIOSurface surface{}; assert(!MiniIOSurfaceDrawn(&surface, 0)); c.free();
    }
}
static void queue_room() {
    /* A830: with the queue full, submit waits for the oldest through gpuWait() before the backend is called */
    for (bool a830_queue : {false, true}) {
        reset(); a830 = a830_queue; auto c = client(); allocate(c);
        msm_gem_submit_bo bo{0, 1, 0}; msm_gem_submit_cmd cmd{};
        cmd.type = MSM_SUBMIT_CMD_BUF; cmd.size = 4;
        msm_gem_submit request{}; request.nr_cmds = request.nr_bos = 1; request.flags = MSM_PIPE_3D0;
        request.bos = reinterpret_cast<uint64_t>(&bo); request.cmds = reinterpret_cast<uint64_t>(&cmd);
        busy_for = 3; uint64_t before = now; struct gpu_io_stat io{};
        assert(c.submit(&request, &io) == kIOReturnSuccess && submit_calls == 1);
        assert(a830_queue ? done_calls >= 4 && now > before && gGPUStat.waits == 1 : done_calls == 0 && now == before);
        c.free();
    }
    reset(); a830 = true; context_depth = 8; auto c = client(); allocate(c);   /* last 7 < depth 8: room, no wait */
    msm_gem_submit_bo bo{0, 1, 0}; msm_gem_submit_cmd cmd{};
    cmd.type = MSM_SUBMIT_CMD_BUF; cmd.size = 4;
    msm_gem_submit request{}; request.nr_cmds = request.nr_bos = 1; request.flags = MSM_PIPE_3D0;
    request.bos = reinterpret_cast<uint64_t>(&bo); request.cmds = reinterpret_cast<uint64_t>(&cmd);
    busy_for = 3; struct gpu_io_stat io{}; assert(c.submit(&request, &io) == kIOReturnSuccess && done_calls == 0); c.free();
}
static void display() {
    reset(); MiniIOSurface s{}; s.drawn = 7; ready = false;
    assert(!MiniIOSurfaceDrawn(&s, 0) && done_calls == 0 && MiniIOSurfaceGPUStopped());
    ready = true; assert(MiniIOSurfaceDrawn(&s, 0) && !MiniIOSurfaceGPUStopped());
    gGPUReported = true; assert(!MiniIOSurfaceDrawn(&s, 0) && MiniIOSurfaceGPUStopped());

    reset();
    auto *m = new IOMemoryDescriptor(2 * PAGE_SIZE);
    MiniIOSurface s2{}; s2.memory = m; s2.kbase = m->bytes;
    s2.info.allocSize = PAGE_SIZE; s2.drawn = 7;
    assert(MiniIOSurfaceDrawn(&s2, 0) && flush_calls == 1);
    assert(last_flush_ptr == m->bytes + PAGE_SIZE && last_flush_size == PAGE_SIZE);
    assert(s2.cpuSyncedFence == 7);
    assert(MiniIOSurfaceDrawn(&s2, 0) && flush_calls == 1);

    gGPUCoherent = true; s2.drawn = 8;
    assert(MiniIOSurfaceDrawn(&s2, 0) && flush_calls == 1);
}
static void out_of_lock_teardown() {
    reset(); auto c = client(); gGPUKeep = 0;
    std::vector<IOMemoryDescriptor *> deferred;
    fence_done = false;
    for (unsigned i = 0; i < 40; ++i) {
        auto *b = allocate(c, PAGE_SIZE);
        deferred.push_back(b->memory);
        uint32_t handle = (uint32_t)(b - c.fBuffers) + 1;
        assert(c.gemClose(handle) == kIOReturnSuccess);
    }
    assert(c.fLeftCount == 40 && releases_in_lock == 0);
    auto *live = allocate(c, PAGE_SIZE);
    uint32_t live_handle = (uint32_t)(live - c.fBuffers) + 1;
    msm_gem_submit_bo bo{0, live_handle, 0}; msm_gem_submit_cmd cmd{};
    cmd.type = MSM_SUBMIT_CMD_BUF; cmd.size = 4;
    msm_gem_submit req{}; req.nr_cmds = req.nr_bos = 1; req.flags = MSM_PIPE_3D0;
    req.bos = reinterpret_cast<uint64_t>(&bo); req.cmds = reinterpret_cast<uint64_t>(&cmd);
    fence_done = true;
    struct gpu_io_stat io{};
    assert(c.submit(&req, &io) == kIOReturnSuccess);
    assert(c.fLeftCount == 40 - GPU_REAP_SUBMIT_BUDGET);
    assert(releases_in_lock == 0);
    while (c.fLeftCount != 0) {
        assert(c.submit(&req, &io) == kIOReturnSuccess);
    }
    assert(releases_in_lock == 0);
    for (auto *m : deferred) assert(m->freed);
    auto *root = new IOBufferMemoryDescriptor(2 * PAGE_SIZE);
    MiniIOSurface s{}; s.memory = root; s.kbase = root->bytes; s.info.allocSize = PAGE_SIZE;
    surface_lookup = &s; annibale_surface sreq{};
    assert(c.surfaceBuffer(&sreq, &io) == kIOReturnSuccess);
    fence_done = false;
    assert(c.gemClose(sreq.handle) == kIOReturnSuccess);
    assert(c.fLeftCount == 1);
    fence_done = true;
    assert(c.submit(&req, &io) == kIOReturnSuccess);
    assert(c.fLeftCount == 0 && releases_in_lock == 0 && drops_in_lock == 0 && s.held == 1);
    MiniIOSurfaceDrop(&s);
    assert(root->freed);
    gGPUKeep = 1;
    std::vector<uint32_t> keep_handles;
    for (unsigned i = 0; i < 20; ++i) {
        auto *b = allocate(c, PAGE_SIZE);
        keep_handles.push_back((uint32_t)(b - c.fBuffers) + 1);
    }
    for (uint32_t h : keep_handles) {
        assert(c.gemClose(h) == kIOReturnSuccess);
    }
    now = (GPU_KEPT_SECONDS + 1ull) * NSEC_PER_SEC;
    msm_gem_new nreq{2 * PAGE_SIZE, 0, 0};
    assert(c.gemNew(&nreq, nullptr, &io) == kIOReturnSuccess);
    assert(gGPUStat.expire_drop == GPU_EXPIRE_BUDGET && releases_in_lock == 0);
    assert(c.clientClose() == kIOReturnSuccess);
    assert(releases_in_lock == 0 && drops_in_lock == 0);
    c.free();
}
static void keep_evict_bounded() {
    reset(); auto c = client(); gGPUKeep = 1;
    for (unsigned i = 0; i < 32; ++i) {
        auto *m = new IOBufferMemoryDescriptor(GPU_KEPT_BYTES / 32);
        assert(c.keep(m, m->bytes, 0x50000000ull + i * (GPU_KEPT_BYTES / 32), GPU_KEPT_BYTES / 32, 0));
    }
    assert(c.fKeptBytes == GPU_KEPT_BYTES);
    struct gpu_garbage garbage{};
    auto *big = new IOBufferMemoryDescriptor(GPU_KEPT_BYTES / 4);
    /* Replacing 8 entries needs <= 16 evictions -> succeeds and defers releases to garbage. */
    assert(c.keep(big, big->bytes, 0x90000000ull, GPU_KEPT_BYTES / 4, 0, &garbage));
    assert(garbage.memory_count == 8 && releases_in_lock == 0);
    gpuGarbageFlush(&garbage);
    /* Fill pool with many tiny entries plus large entries so > GPU_KEEP_EVICT_MAX evictions would be required. */
    reset(); c = client(); gGPUKeep = 1;
    for (unsigned i = 0; i < 200; ++i) {
        auto *m = new IOBufferMemoryDescriptor(PAGE_SIZE);
        assert(c.keep(m, m->bytes, 0x60000000ull + i * PAGE_SIZE, PAGE_SIZE, 0));
    }
    for (unsigned i = 0; i < 3; ++i) {
        auto *m = new IOBufferMemoryDescriptor(GPU_KEPT_BYTES / 4);
        assert(c.keep(m, m->bytes, 0x70000000ull + i * (GPU_KEPT_BYTES / 4), GPU_KEPT_BYTES / 4, 0));
    }
    auto *giant = new IOBufferMemoryDescriptor(GPU_KEPT_BYTES / 4);
    assert(!c.keep(giant, giant->bytes, 0x80000000ull, GPU_KEPT_BYTES / 4, 0, &garbage));
    assert(garbage.memory_count == GPU_KEEP_EVICT_MAX && releases_in_lock == 0);
    gpuGarbageFlush(&garbage); giant->release();
    c.clientClose(); c.free();
}
static void surface_fail_retain_not_pooled() {
    reset(); auto c = client();
    auto *root = new IOBufferMemoryDescriptor(3 * PAGE_SIZE);
    MiniIOSurface s{}; s.memory = root; s.kbase = root->bytes; s.info.allocSize = 2 * PAGE_SIZE;
    surface_lookup = &s; map_fail_at = 2; unmap_ok = false;
    annibale_surface request{}; struct gpu_io_stat io{};
    assert(c.surfaceBuffer(&request, &io) == kIOReturnNotReady);
    assert(c.fBufferCount == 1 && !c.fBuffers[request.handle - 1].pooled);
    /* Even if recovery clears gGPUReported later, retained surface backing must never enter fKept. */
    ready = true; gGPUReported = false; fence_done = true; unmap_ok = true; gGPUKeep = 1;
    assert(c.gemClose(request.handle) == kIOReturnSuccess);
    assert(c.fKeptBytes == 0 && c.fKept[0].memory == nullptr);
    assert(c.clientClose() == kIOReturnSuccess);
    MiniIOSurfaceDrop(&s);
    assert(root->freed); c.free();
}
static void full_fleft_reap_before_wait() {
    reset(); auto c = client();
    for (unsigned i = 0; i <= GPU_LEFT; ++i) {
        auto *b = allocate(c); b->fence = 7;
    }
    fence_done = false;
    for (unsigned i = 0; i < GPU_LEFT; ++i) {
        assert(c.gemClose(i + 1) == kIOReturnSuccess);
    }
    assert(c.fLeftCount == GPU_LEFT);
    fence_done = true;
    uint64_t before = now;
    assert(c.gemClose(GPU_LEFT + 1) == kIOReturnSuccess);
    assert(now == before && c.fLeftCount < GPU_LEFT && releases_in_lock == 0);
    c.clientClose(); c.free();
}
static void cross_slot_last_use_and_stall() {
    static uint32_t s0_done, s1_done;
    auto check_done = [](uint32_t f) -> bool {
        return (f & 0x80000000u) ? ((f & 0x7fffffffu) <= s1_done) : (f <= s0_done);
    };
    reset();
    auto c = client();
    auto *root = new IOBufferMemoryDescriptor(2 * PAGE_SIZE);
    MiniIOSurface s{}; s.memory = root; s.kbase = root->bytes; s.info.allocSize = PAGE_SIZE;
    surface_lookup = &s;
    annibale_surface req{}; struct gpu_io_stat io{};
    assert(c.surfaceBuffer(&req, &io) == kIOReturnSuccess);
    auto *b = &c.fBuffers[req.handle - 1];
    done_hook = check_done;

    /* Slot 0 pending (16) vs slot 1 retired (0x80000005) must not pick retired slot 1 fence. */
    s0_done = 10; s1_done = 10;
    b->fence = 16u; s.drawn = 0x80000005u;
    assert(c.lastUse(req.handle) == 16u);

    /* Slot 1 retired (0x80000005) vs slot 0 pending (16) must pick pending slot 0 drawn. */
    b->fence = 0x80000005u; s.drawn = 16u;
    assert(c.lastUse(req.handle) == 16u);

    /* Slot 1 pending (0x80000012) vs slot 0 retired (10) must pick pending slot 1 fence. */
    b->fence = 0x80000012u; s.drawn = 10u;
    assert(c.lastUse(req.handle) == 0x80000012u);

    /* Same-slot 31-bit wraparound still picks newer drawn timestamp. */
    s1_done = 0;
    b->fence = 0xffffffffu; s.drawn = 0x80000002u;
    assert(c.lastUse(req.handle) == 0x80000002u);

    /* Per-slot stall detection: slot 1 advancing prevents false stall even when slot 0 retired_now is frozen. */
    done_hook = nullptr;
    fence_done = false;
    retired_now = 99u; retired_slot1 = 0x80000001u;
    IOLockLock(gGPULock);
    assert(!gpuWait(0x80000005u, 1000));
    assert(!gGPUReported);
    now += (uint64_t)(GPU_STALL_MS + 100) * NSEC_PER_MSEC;
    retired_slot1 = 0x80000002u; /* slot 1 made progress -> resets stall timer */
    assert(!gpuWait(0x80000005u, 1000));
    assert(!gGPUReported);
    /* Slot 0 advancing must NOT mask a stalled slot 1 past GPU_STALL_MS. */
    now += (uint64_t)(GPU_STALL_MS + 100) * NSEC_PER_MSEC;
    retired_now = 105u;
    assert(!gpuWait(0x80000005u, 1000));
    assert(gGPUReported);
    IOLockUnlock(gGPULock);

    gGPUReported = false; s.drawn = 0; b->fence = 0; fence_done = true;
    assert(c.gemClose(req.handle) == kIOReturnSuccess);
    MiniIOSurfaceDrop(&s);
    assert(root->freed); c.clientClose(); c.free();
}
int main() {
    healthy(); stopped(); failed_reap(); completed_reap(); failed_free(); partial_map(); segments(); kept();
    failed_surface(); surface_ownership(); full_pending(); long_pass(); failed_submit(); queue_room(); display();
    out_of_lock_teardown(); keep_evict_bounded(); surface_fail_retain_not_pooled(); full_fleft_reap_before_wait();
    cross_slot_last_use_and_stall(); reset();
    printf("AnnibaleGPU production lifecycle: 36 scenarios at %llu-byte pages passed\n", (unsigned long long)PAGE_SIZE);
}
'''


def unit(source, memory):
    uapi = source[source.index("/* Linux's include/uapi") : source.index("static IOLock * gGPULock;")]
    stats = re.search(r"static struct \{.*?\} gGPUStat;", source, re.S).group()
    records = source[source.index("struct gpu_buffer {") : source.index("class AnnibaleGPUUserClient :")]
    fields = source[source.index("\ttask_t                  fTask;") : source.index("\npublic:\n\tvirtual bool initWithTask")]
    return (PRELUDE + uapi + "\n"
            "static bool gGPUReported, gGPUCached, gGPUCoherent;\n" + stats + "\nstatic uint32_t gGPUIrqsSeen;\n" + records
            + "\nstruct AnnibaleGPUUserClient : IOUserClient {\n" + fields
            + "\nIOReturn clientClose(); void free();\n};\n"
            + function(source, "gpuWait") + function(source, "MiniIOSurfaceDrawn")
            + function(source, "MiniIOSurfaceGPUStopped")
            + memory + function(source, "AnnibaleGPUUserClient::submit") + CASES)


LEGACY_MODEL = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
struct { unsigned context = 3; } core_space;
struct { uint64_t base = 0x100000; } smmu;
enum { SMMU_CB_TLBIASID = 1, SMMU_CB_TLBSYNC = 2, SMMU_CB_TLBSTATUS = 3 };
static bool pte_ok = true, tlb_busy;
static unsigned commits, maps, status_reads;
static uint32_t smmu_context(void *) { return 0x1000; }
static void space_commit(void *) { ++commits; }
static bool space_map(void *, uint64_t, uint64_t phys, unsigned prot) {
    assert(phys == 0 && prot == 0); ++maps; return pte_ok;
}
static void settle() {}
static void wr(uint64_t, uint64_t) {}
static unsigned rd(uint64_t address) {
    assert(address == smmu.base + 0x1000 + SMMU_CB_TLBSTATUS);
    ++status_reads; return tlb_busy;
}
'''
LEGACY_CASES = r'''
int main() {
    assert(annibale_gpu_mapped() && commits == 1);
    assert(annibale_gpu_unmap(0x20000000, 8192) && maps == 2 && commits == 2);
    pte_ok = false; assert(!annibale_gpu_unmap(0x20000000, 8192));
    pte_ok = true; tlb_busy = true; status_reads = 0;
    assert(!annibale_gpu_unmap(0x20000000, 8192) && status_reads == 1001);
    status_reads = 0;
    assert(!annibale_gpu_mapped() && status_reads == 1001);
    puts("legacy production unmap: PTE and TLB failure checks passed");
}
'''


def execute(text, folder, label, expect_success=True, page_size=16384):
    source = folder / (label + ".cpp")
    binary = folder / label
    source.write_text(text)
    compile_result = subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DHOST_PAGE_SIZE=" + str(page_size),
                                     "-Werror=return-type", str(source), "-o", str(binary)], capture_output=True, text=True)
    if compile_result.returncode:
        raise RuntimeError(label + " compilation failed:\n" + compile_result.stderr)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    if expect_success:
        if run.returncode:
            raise RuntimeError(label + " failed:\n" + run.stdout + run.stderr)
        print(run.stdout, end="")
    elif run.returncode == 0:
        raise RuntimeError("mutation survived: " + label)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    source = SOURCE.read_text().replace('#include "AnnibaleGPUSubmit.inc"', SUBMIT.read_text())
    memory = MEMORY.read_text()
    with tempfile.TemporaryDirectory(prefix="annibale-gpu-lifetime-") as temporary:
        folder = Path(temporary)
        execute(unit(source, memory), folder, "lifecycle_4k", page_size=4096)
        execute(unit(source, memory), folder, "lifecycle_16k")
        if args.mutations:
            changes = [
                ("release_stopped", memory, "busy = gGPUReported || !annibale_gpu_ready() || (last", "busy = gGPUReported || (last", False),
                ("ignore_unmap", memory, "!annibale_gpu_unmap(address, size)", "(annibale_gpu_unmap(address, size), false)", False),
                ("partial_release", memory, "\n\t\t\tmemory->retain();", "\n\t\t\t/* removed retain */", False),
                ("reuse_failed_kept", memory, "if (!gpuReleaseMapping(k->address, k->size))", "if ((gpuReleaseMapping(k->address, k->size), false))", False),
                ("short_segment", memory, " && length >= 4096;", ";", False),
                ("one_lookup", memory, "phys += 4096, length -= 4096", "length -= 4096", False),
                ("queue_wait_held", source, "if (!gpuWait(target, GPU_PATIENCE_MS)) {", "if (true) {", True),
                ("submit_zero", source, "gGPUReported = true;\n\t}\n\tseg_sub = mach_absolute_time();\n\tif (gpu_log", "gGPUReported = false;\n\t}\n\tseg_sub = mach_absolute_time();\n\tif (gpu_log", True),
                ("one_free_wait", memory, "} while (busy && fLeftCount == GPU_LEFT && !gGPUReported && annibale_gpu_ready());", "} while (false);", False),
                ("stopped_ignores_reported", source, "stopped = stopped || gGPUReported;", "stopped = stopped;", True),
                ("display_stopped", source, "(annibale_gpu_ready() && gpuWait(fence, milliseconds))", "(!annibale_gpu_ready() || gpuWait(fence, milliseconds))", True),
                ("surface_pooled_leak", memory, "b->surface == NULL && b->pooled &&", "b->surface == NULL &&", False),
                ("reap_release_in_lock", source, "gpuGarbageFlush(&garbage);", "/* leaked or in-lock */", True),
            ]
            for name, original, old, new, in_source in changes:
                assert old in original, name
                changed = original.replace(old, new, 1)
                execute(unit(changed if in_source else source, memory if in_source else changed), folder, name, False)
            print(f"{len(changes)} lifecycle mutations detected")


if __name__ == "__main__":
    main()

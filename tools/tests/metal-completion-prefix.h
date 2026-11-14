#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "air_metal.h"
#include "air_metal_rgb2p.h"
#include "annibalemetal_work_private.h"

static unsigned checks;
static void require(BOOL value, const char *message) {
    if (!value) { fprintf(stderr, "FAIL %s\n", message); abort(); }
    __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED);
}
static void rw_print(const char *format, ...) { (void)format; }
#define RW_ONCE(...) rw_print(__VA_ARGS__)
static void say(char *why, size_t size, const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(why, size, format, args); va_end(args);
}
static uint64_t rw_now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static uint64_t rw_ticks_us(uint64_t ticks) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); return ticks * tb.numer / tb.denom / 1000;
}
static void pause_ms(unsigned ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000}; nanosleep(&t, NULL);
}
static void await(dispatch_semaphore_t semaphore, const char *message) {
    require(dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 3000000000ll)) == 0, message);
}
static BOOL wait_count(unsigned *value, unsigned target) {
    uint64_t deadline = rw_now_us() + 2500000;
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < target && rw_now_us() < deadline) pause_ms(1);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE) >= target;
}

struct pipe_fence_handle {
    unsigned refs, id, waits, releases;
    bool signaled, fail;
    pthread_mutex_t lock;
    pthread_cond_t changed;
};
struct pipe_context { void (*flush)(struct pipe_context *, struct pipe_fence_handle **, unsigned); };
struct pipe_screen {
    void (*fence_reference)(struct pipe_screen *, struct pipe_fence_handle **, struct pipe_fence_handle *);
    bool (*fence_finish)(struct pipe_screen *, struct pipe_context *, struct pipe_fence_handle *, uint64_t);
};
struct rwa_device { struct pipe_context *pipe; struct pipe_screen *screen; struct pipe_fence_handle *pending; };
static struct pipe_fence_handle fences[32];
static unsigned submitted, recorded, wait_entries, finish_entries, raw_destroyed, cb_destroyed;
static unsigned texture_alive, device_alive, writebacks;
static void reference(struct pipe_screen *screen, struct pipe_fence_handle **slot, struct pipe_fence_handle *source) {
    (void)screen;
    if (source) require(__atomic_add_fetch(&source->refs, 1, __ATOMIC_ACQ_REL) > 1, "retain live fence");
    if (*slot) {
        require(__atomic_fetch_sub(&(*slot)->refs, 1, __ATOMIC_ACQ_REL) > 0, "release owned fence");
        __atomic_add_fetch(&(*slot)->releases, 1, __ATOMIC_RELAXED);
    }
    *slot = source;
}
static void flush(struct pipe_context *pipe, struct pipe_fence_handle **output, unsigned flags) {
    (void)pipe; (void)flags;
    *output = NULL;
    if (!recorded) return;
    struct pipe_fence_handle *fence = &fences[++submitted];
    require(submitted < 32, "bounded fence storage");
    fence->id = submitted; fence->refs = 1;
    pthread_mutex_init(&fence->lock, NULL); pthread_cond_init(&fence->changed, NULL);
    recorded = 0; *output = fence;
}
static bool finish(struct pipe_screen *screen, struct pipe_context *pipe, struct pipe_fence_handle *fence, uint64_t timeout) {
    (void)screen; (void)pipe;
    pthread_mutex_lock(&fence->lock);
    if (!timeout) { bool done = fence->signaled; pthread_mutex_unlock(&fence->lock); return done; }
    __atomic_add_fetch(&wait_entries, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&fence->waits, 1, __ATOMIC_RELEASE);
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 3;
    while (!fence->signaled && !fence->fail) {
        int result = pthread_cond_timedwait(&fence->changed, &fence->lock, &deadline);
        require(result != ETIMEDOUT, "host supplies bounded GPU outcome");
    }
    bool done = fence->signaled && !fence->fail;
    pthread_mutex_unlock(&fence->lock); return done;
}
static void resolve_fence(unsigned id, BOOL success) {
    struct pipe_fence_handle *fence = &fences[id];
    require(id && id <= submitted, "resolve submitted fence");
    pthread_mutex_lock(&fence->lock);
    fence->signaled = success; fence->fail = !success;
    pthread_cond_broadcast(&fence->changed); pthread_mutex_unlock(&fence->lock);
}
static struct pipe_context pipe_backend = {.flush = flush};
static struct pipe_screen screen_backend = {.fence_reference = reference, .fence_finish = finish};
static struct rwa_device backend = {.pipe = &pipe_backend, .screen = &screen_backend};

#include "air-extracts.inc"

static pthread_mutex_t rw_commit_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static dispatch_semaphore_t unlocked, resume_commit, taking, resume_take;
static _Thread_local BOOL pause_unlock, pause_take, fail_calloc;
static BOOL fail_worker;
static void fixture_unlock(pthread_mutex_t *lock) {
    pthread_mutex_unlock(lock);
    if (lock == &rw_commit_lock && pause_unlock) {
        pause_unlock = NO; dispatch_semaphore_signal(unlocked); await(resume_commit, "resume paused commit");
    }
}
static void *fixture_calloc(size_t count, size_t size) {
    if (fail_calloc) { fail_calloc = NO; return NULL; }
    return calloc(count, size);
}
static int fixture_create(pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg) {
    if (fail_worker) return EAGAIN;
    return pthread_create(thread, attr, entry, arg);
}
static struct rwa_fence *take(struct rwa_device *device) {
    if (pause_take) {
        pause_take = NO; dispatch_semaphore_signal(taking); await(resume_take, "resume fence capture");
    }
    return rwa_fence_take(device);
}
static int legacy_finish(struct rwa_device *device, char *why, size_t size) {
    __atomic_add_fetch(&finish_entries, 1, __ATOMIC_RELEASE); return rwa_finish(device, why, size);
}
static void writeback(struct rwa_device *device) { (void)device; __atomic_add_fetch(&writebacks, 1, __ATOMIC_RELAXED); }
struct rwa_texture { unsigned unused; };
static void destroy_texture(struct rwa_texture *texture) {
    require(__atomic_load_n(&device_alive, __ATOMIC_ACQUIRE) != 0, "device owns texture through destruction");
    __atomic_add_fetch(&raw_destroyed, 1, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&texture_alive, 1, __ATOMIC_RELEASE); free(texture);
}
static int pass(struct rwa_texture *texture, bool begin, char *why, size_t size) {
    (void)texture; (void)begin; (void)why; (void)size; return 0;
}
static struct {
    __typeof__(&rwa_flush) flush;
    __typeof__(&rwa_fence_take) fence_take;
    __typeof__(&rwa_fence_wait) fence_wait;
    __typeof__(&rwa_fence_release) fence_release;
    __typeof__(&rwa_finish) finish;
    __typeof__(&rwa_buffer_textures_refresh) buffer_textures_refresh;
    __typeof__(&rwa_buffer_textures_writeback) buffer_textures_writeback;
    __typeof__(&rwa_texture_rgb2p_pass) texture_rgb2p_pass;
    __typeof__(&rwa_texture_destroy) texture_destroy;
    __typeof__(&rwa_dispatch_nowait) dispatch_nowait;
    __typeof__(&rwa_dispatch_threadgroup) dispatch_threadgroup;
    __typeof__(&rwa_dispatch_buffers) dispatch_buffers;
    __typeof__(&rwa_dispatch) dispatch;
} mesa = {.flush = rwa_flush, .fence_take = take, .fence_wait = rwa_fence_wait,
          .fence_release = rwa_fence_release, .finish = legacy_finish,
          .buffer_textures_writeback = writeback, .texture_rgb2p_pass = pass, .texture_destroy = destroy_texture};

#define RW_TRACE_COMMITS 0
#define RW_COMMIT_SLOW_US 1000000000ull
#define RW_DRAW_SLOW_US 1000000000ull
#define RW_FLUSH_SLOW_US 1000000000ull
enum {RW_WORK_COPY, RW_WORK_DRAW, RW_WORK_DISPATCH};
static struct { unsigned count; uint64_t us; } rw_work_stats[3];
static unsigned commit_count, commit_in_flight, commit_phase, fence_waits, fence_frame, fence_tid, fence_wait;
static const void *fence_owner;
static uint64_t commit_started, commit_progress_us, fence_began_us;
static pthread_once_t rw_watch_once = PTHREAD_ONCE_INIT;
static unsigned rw_thumb_draws;
static void rw_start_watch(void) {}
static void rw_work_time(int kind, uint64_t began) { (void)began; rw_work_stats[kind].count++; }
static BOOL rw_sync_draws(void) { return NO; }
static BOOL rw_matched_sync(void) { return NO; }
static BOOL rw_thumbs_wanted(unsigned n) { (void)n; return NO; }
static BOOL rw_take_matched_draw(id work, unsigned c, unsigned i) { (void)work; (void)c; (void)i; return NO; }
static BOOL rw_thumb_group_fits(id work, unsigned c, unsigned i) { (void)work; (void)c; (void)i; return YES; }
static void rw_thumb_commit(id works, unsigned c) { (void)works; (void)c; }
static void rw_audit_commit_begin(id works, unsigned c) { (void)works; (void)c; }
static void rw_audit_render_work(id work, unsigned c, unsigned i) { (void)work; (void)c; (void)i; }
static void rw_audit_commit_end(unsigned c) { (void)c; }
static void rw_thumb(void *raw, id object, unsigned c, unsigned i, const char *label, unsigned slot) {
    (void)raw; (void)object; (void)c; (void)i; (void)label; (void)slot;
}
static void rw_timing_commit(unsigned c, uint64_t e, uint64_t r, uint64_t f) { (void)c; (void)e; (void)r; (void)f; }
static void rw_timing_completion(unsigned c, uint64_t q, uint64_t f, uint64_t w, BOOL success) {
    (void)c; (void)q; (void)f; (void)w; (void)success;
}
static void rw_pipeline_dump(void) {}
/* The host superclass has no _MTLCommandBuffer private semaphore ivar. */
static void annibale_clear_semaphore_debt(id object) { (void)object; }

@interface AnnibaleMetalDevice : NSObject
- (struct rwa_device *)mesaDevice;
@end
@implementation AnnibaleMetalDevice
- (id)init { self = [super init]; __atomic_add_fetch(&device_alive, 1, __ATOMIC_RELEASE); return self; }
- (struct rwa_device *)mesaDevice { return &backend; }
- (void)dealloc { __atomic_sub_fetch(&device_alive, 1, __ATOMIC_RELEASE); [super dealloc]; }
@end
@interface AnnibaleMetalCommandQueue : NSObject {
@public AnnibaleMetalDevice *owner; dispatch_queue_t handlerQueue; dispatch_semaphore_t slots; unsigned returned, took;
}
- (id)initWithDevice:(AnnibaleMetalDevice *)device;
- (id)device;
- (dispatch_queue_t)annibaleCompletionQueue;
- (void)annibaleTookSlot;
- (void)annibaleReturnedSlot;
@end
@implementation AnnibaleMetalCommandQueue
- (id)initWithDevice:(AnnibaleMetalDevice *)device {
    self = [super init]; owner = [device retain];
    handlerQueue = dispatch_queue_create("fixture.handlers", DISPATCH_QUEUE_SERIAL);
    slots = dispatch_semaphore_create(128); return self;
}
- (id)device { return owner; }
- (dispatch_queue_t)annibaleCompletionQueue { return handlerQueue; }
- (void)annibaleTookSlot { __atomic_add_fetch(&took, 1, __ATOMIC_RELEASE); }
- (void)annibaleReturnedSlot { __atomic_add_fetch(&returned, 1, __ATOMIC_RELEASE); }
- (void)dealloc { [owner release]; dispatch_release(handlerQueue); dispatch_release(slots); [super dealloc]; }
@end
static dispatch_semaphore_t annibale_queue_semaphore(AnnibaleMetalCommandQueue *queue) { return queue->slots; }
@interface RWFixtureCommandBufferBase : NSObject
- (id)initWithQueue:(id)queue retainedReferences:(BOOL)retained synchronousDebugMode:(BOOL)debug;
@end
@implementation RWFixtureCommandBufferBase
- (id)initWithQueue:(id)queue retainedReferences:(BOOL)retained synchronousDebugMode:(BOOL)debug {
    (void)queue; (void)retained; (void)debug; return [super init];
}
- (void)dealloc { __atomic_add_fetch(&cb_destroyed, 1, __ATOMIC_RELEASE); [super dealloc]; }
@end
@interface AnnibaleMetalRenderPipeline : NSObject
- (struct rwa_render *)render;
@end
@implementation AnnibaleMetalRenderPipeline
- (struct rwa_render *)render { return (void *)self; }
@end
@interface AnnibaleMetalComputePipeline : NSObject
- (struct rwa_compute *)compute;
@end
@implementation AnnibaleMetalComputePipeline
- (struct rwa_compute *)compute { return (void *)self; }
@end
@interface AnnibaleMetalTexture : NSObject {
    AnnibaleMetalDevice *_owner; struct rwa_texture *_texture; struct rwa_texture_shape _shape;
    CFTypeRef _iosurface; NSUInteger _iosurfacePlane; id _parentTexture, _buffer;
}
- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device texture:(struct rwa_texture *)texture
                               shape:(const struct rwa_texture_shape *)shape iosurface:(CFTypeRef)surface plane:(NSUInteger)plane;
- (struct rwa_texture *)mesaTexture;
@end
@implementation AnnibaleMetalTexture
#include "texture-extracts.inc"
- (struct rwa_texture *)mesaTexture { return _texture; }
@end
#include "work-extract.inc"

@interface AnnibaleMetalCommandBuffer : RWFixtureCommandBufferBase
- (id)initWithAnnibaleQueue:(AnnibaleMetalCommandQueue *)queue;
- (void)commit;
- (void)recordWork:(AnnibaleMetalWork *)work;
- (void)annibaleReturnSlot;
- (void)failWith:(const char *)why;
- (void)annibaleCompleteLater:(unsigned)commit fence:(struct rwa_fence *)fence;
- (void)annibaleCompleteWithFence:(struct rwa_fence *)fence commit:(unsigned)commit queued:(uint64_t)queued_us;
- (void)annibaleDeliverLater:(NSArray *)handlers;
- (void)annibaleAddScheduled:(void (^)(id))handler;
- (void)annibaleAddCompleted:(void (^)(id))handler;
- (BOOL)annibaleAwaitCompletion;
- (void)waitUntilCompleted;
- (NSUInteger)status;
- (unsigned)fixturePhase;
- (NSError *)error;
- (id)device;
@end
static int rw_run_copy(AnnibaleMetalWork *work, char *why, size_t size) {
    if (work->copyKind == RW_COPY_ERROR) { snprintf(why, size, "original encoding failure"); return -1; }
    recorded++; return 0;
}

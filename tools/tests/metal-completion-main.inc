static AnnibaleMetalDevice *owner;
static AnnibaleMetalCommandQueue *queue;
static unsigned callback_count, scheduled_count, commit_returned, waiter_returned;
static pthread_t commit_thread;
static dispatch_semaphore_t completed_callback;

static unsigned phase(AnnibaleMetalCommandBuffer *buffer) { return [buffer fixturePhase]; }
static void callbacks(AnnibaleMetalCommandBuffer *buffer) {
    [buffer annibaleAddScheduled:^(id raw) {
        require([raw status] >= RW_CB_SCHEDULED, "Scheduled handler observes scheduled status");
        require(!pthread_equal(pthread_self(), commit_thread), "Scheduled handler runs asynchronously");
        __atomic_add_fetch(&scheduled_count, 1, __ATOMIC_RELEASE);
    }];
    [buffer annibaleAddCompleted:^(id raw) {
        require([raw status] == RW_CB_COMPLETED || [raw status] == RW_CB_ERROR, "Completed handler observes terminal status");
        require(__atomic_load_n(&scheduled_count, __ATOMIC_ACQUIRE) != 0, "Scheduled handler precedes Completed handler");
        require(!pthread_equal(pthread_self(), commit_thread), "Completed handler runs asynchronously");
        __atomic_add_fetch(&callback_count, 1, __ATOMIC_RELEASE); dispatch_semaphore_signal(completed_callback);
    }];
}
static AnnibaleMetalCommandBuffer *make_buffer(BOOL fails) {
    AnnibaleMetalCommandBuffer *buffer = [[AnnibaleMetalCommandBuffer alloc] initWithAnnibaleQueue:queue];
    AnnibaleMetalWork *work = [AnnibaleMetalWork new];
    AnnibaleMetalRenderPipeline *pipeline = [AnnibaleMetalRenderPipeline new];
    work->render = pipeline; [buffer recordWork:work]; [work release]; [pipeline release];
    if (fails) {
        work = [AnnibaleMetalWork new]; work->copyKind = RW_COPY_ERROR;
        [buffer recordWork:work]; [work release];
    }
    return buffer;
}
struct request { AnnibaleMetalCommandBuffer *buffer; BOOL pause_unlock, pause_take, oom, wait_only; };
static void *perform(void *raw) {
    struct request *request = raw;
    @autoreleasepool {
        pause_unlock = request->pause_unlock; pause_take = request->pause_take; fail_calloc = request->oom;
        if (request->wait_only) {
            [request->buffer waitUntilCompleted]; __atomic_add_fetch(&waiter_returned, 1, __ATOMIC_RELEASE);
        } else {
            commit_thread = pthread_self(); [request->buffer commit];
            __atomic_add_fetch(&commit_returned, 1, __ATOMIC_RELEASE);
        }
    }
    return NULL;
}
static pthread_t launch(struct request *request) {
    pthread_t thread; require(pthread_create(&thread, NULL, perform, request) == 0, "create host task"); return thread;
}
static void drain(AnnibaleMetalCommandBuffer *buffer) {
    [buffer waitUntilCompleted]; await(completed_callback, "completion callback arrives");
    dispatch_sync(queue->handlerQueue, ^{});
}
static void pending_release(void) {
    pthread_mutex_lock(&rwa_lock); reference(backend.screen, &backend.pending, NULL); pthread_mutex_unlock(&rwa_lock);
    for (unsigned i = 1; i <= submitted; i++) require(fences[i].refs == 0, "all submission fence references released");
}
static void asynchronous(BOOL fails, BOOL timeout, BOOL legacy) {
    if (legacy) mesa.fence_take = NULL;
    AnnibaleMetalCommandBuffer *buffer = make_buffer(fails); callbacks(buffer);
    commit_thread = pthread_self(); [buffer commit];
    require(wait_count(&wait_entries, 1), "completion waits on submitted work");
    require(phase(buffer) < RW_CB_PHASE_FINISHED, "pending GPU work has no completion");
    NSError *original = [[buffer error] retain];
    if (fails) require([original code] == 4, "encoding error recorded before completion");
    resolve_fence(1, !timeout); drain(buffer);
    require([buffer status] == (fails || timeout ? RW_CB_ERROR : RW_CB_COMPLETED), "real GPU outcome sets command status");
    if (fails) require([buffer error] == original && [[buffer error] code] == 4, "original error survives fence timeout");
    if (timeout && !fails) require([[buffer error] code] == 3, "GPU timeout supplies an error");
    require(callback_count == 1 && scheduled_count == 1, "each initial handler delivered once");
    require(queue->returned == 1, "queue slot returned exactly once");
    require(writebacks == (fails || timeout ? 0 : 1), "failed work skips writeback");
    [buffer commit]; require(submitted == 1, "duplicate commit makes no submission");
    [original release]; [buffer release]; require(queue->returned == 1, "dealloc preserves paid queue slot");
    pending_release();
}
static void unavailable(BOOL worker_failure) {
    fail_worker = worker_failure;
    AnnibaleMetalCommandBuffer *buffer = make_buffer(NO); callbacks(buffer);
    struct request request = {.buffer = buffer, .oom = !worker_failure};
    pthread_t thread = launch(&request);
    require(wait_count(&wait_entries, 1), "allocation or worker failure waits for GPU");
    require(commit_returned == 0 && phase(buffer) < RW_CB_PHASE_FINISHED, "fallback cannot finish pending work");
    resolve_fence(1, YES); pthread_join(thread, NULL); drain(buffer);
    require(commit_returned == 1 && [buffer status] == RW_CB_COMPLETED, "bounded synchronous fallback finishes real work");
    require(callback_count == 1 && scheduled_count == 1, "fallback preserves async handler order");
    [buffer release]; pending_release();
}
static void interleave(BOOL clear_pending) {
    AnnibaleMetalCommandBuffer *a = make_buffer(NO), *b = make_buffer(NO);
    struct request request = {.buffer = a, .pause_unlock = YES}; pthread_t thread = launch(&request);
    await(unlocked, "A unlocks after flush");
    if (!clear_pending) require(fences[1].refs == 2, "A owns its fence before unlocking");
    [b commit];
    if (!clear_pending) require(fences[1].refs == 1, "B replacement preserves A independent fence");
    if (clear_pending) {
        resolve_fence(2, NO);
        AnnibaleMetalCommandBuffer *other = [[AnnibaleMetalCommandBuffer alloc] initWithAnnibaleQueue:queue];
        [other waitUntilCompleted];
        require(backend.pending == NULL, "B timeout clears AIR pending");
        [other release];
    } else resolve_fence(2, YES);
    dispatch_semaphore_signal(resume_commit); pthread_join(thread, NULL);
    require(wait_count(&fences[1].waits, 1), "A still waits on its own earlier fence");
    require(phase(a) < RW_CB_PHASE_FINISHED, "later finish cannot complete A early");
    resolve_fence(1, YES); [a waitUntilCompleted]; [b waitUntilCompleted];
    require([a status] == RW_CB_COMPLETED, "A completes on its own hardware outcome");
    [a release]; [b release]; pending_release();
}
static void uncommitted_serialization(void) {
    AnnibaleMetalCommandBuffer *a = make_buffer(NO);
    AnnibaleMetalCommandBuffer *other = [[AnnibaleMetalCommandBuffer alloc] initWithAnnibaleQueue:queue];
    struct request first = {.buffer = a, .pause_take = YES}, second = {.buffer = other, .wait_only = YES};
    pthread_t at = launch(&first); await(taking, "A retains fence while holding commit lock");
    pthread_t bt = launch(&second); pause_ms(50);
    require(finish_entries == 0, "uncommitted finish cannot interleave flush and fence retain");
    dispatch_semaphore_signal(resume_take);
    require(wait_count(&finish_entries, 1), "uncommitted fallback runs after submission snapshot");
    resolve_fence(1, NO); pthread_join(at, NULL); pthread_join(bt, NULL);
    [a waitUntilCompleted]; require([a status] == RW_CB_ERROR, "retained fence reports real timeout");
    require(waiter_returned == 1, "fallback waiter returns after bounded outcome");
    [a release]; [other release]; pending_release();
}
static void resources(void) {
    CFMutableDataRef surface = CFDataCreateMutable(NULL, 128);
    struct rwa_texture_shape shape = {.width = 4, .height = 4};
    __atomic_add_fetch(&texture_alive, 1, __ATOMIC_RELEASE);
    AnnibaleMetalTexture *texture = [[AnnibaleMetalTexture alloc] initWithAnnibaleDevice:owner
        texture:calloc(1, sizeof(struct rwa_texture)) shape:&shape iosurface:surface plane:0];
    AnnibaleMetalCommandBuffer *buffer = make_buffer(NO);
    AnnibaleMetalWork *work = [AnnibaleMetalWork new]; work->copyKind = RW_COPY_FILL;
    work->textures[0] = texture; work->textures[1] = texture; work->textureCount = 2;
    [buffer recordWork:work]; [work release]; [texture release]; callbacks(buffer);
    require(CFGetRetainCount(surface) == 2, "texture owns IOSurface reference");
    commit_thread = pthread_self(); [buffer commit]; [buffer release];
    require(wait_count(&wait_entries, 1), "resource case waits for hardware");
    require(texture_alive == 1 && raw_destroyed == 0, "work and resources remain held after caller release");
    resolve_fence(1, YES); await(completed_callback, "resource case completes");
    dispatch_sync(queue->handlerQueue, ^{});
    require(wait_count(&raw_destroyed, 1), "work releases texture after completion ownership ends");
    require(texture_alive == 0 && raw_destroyed == 1 && CFGetRetainCount(surface) == 1,
            "repeated binding slots release texture and IOSurface once");
    CFRelease(surface); pending_release();
}
static void *observe(void *raw) {
    AnnibaleMetalCommandBuffer *buffer = raw;
    while (phase(buffer) < RW_CB_PHASE_FINISHED) {
        NSUInteger status = [buffer status]; require(status <= RW_CB_ERROR, "concurrent public status stays in range");
    }
    require([buffer status] == RW_CB_COMPLETED, "finished phase publishes completed status"); return NULL;
}
static void phase_reads(void) {
    AnnibaleMetalCommandBuffer *buffer = make_buffer(NO); pthread_t readers[4];
    for (unsigned i = 0; i < 4; i++) require(pthread_create(&readers[i], NULL, observe, buffer) == 0, "create status reader");
    commit_thread = pthread_self(); [buffer commit]; require(wait_count(&wait_entries, 1), "phase test awaits GPU");
    resolve_fence(1, YES); [buffer waitUntilCompleted];
    for (unsigned i = 0; i < 4; i++) pthread_join(readers[i], NULL);
    [buffer release]; pending_release();
}
int main(int argc, char **argv) { @autoreleasepool {
    require(argc == 2, "one bounded scenario per process");
    owner = [AnnibaleMetalDevice new]; queue = [[AnnibaleMetalCommandQueue alloc] initWithDevice:owner];
    unlocked = dispatch_semaphore_create(0); resume_commit = dispatch_semaphore_create(0);
    taking = dispatch_semaphore_create(0); resume_take = dispatch_semaphore_create(0);
    completed_callback = dispatch_semaphore_create(0);
    if (!strcmp(argv[1], "normal")) asynchronous(NO, NO, NO);
    else if (!strcmp(argv[1], "error")) asynchronous(YES, NO, NO);
    else if (!strcmp(argv[1], "error-timeout")) asynchronous(YES, YES, NO);
    else if (!strcmp(argv[1], "timeout")) asynchronous(NO, YES, NO);
    else if (!strcmp(argv[1], "legacy-error")) asynchronous(YES, NO, YES);
    else if (!strcmp(argv[1], "legacy-timeout")) asynchronous(YES, YES, YES);
    else if (!strcmp(argv[1], "legacy-gpu-timeout")) asynchronous(NO, YES, YES);
    else if (!strcmp(argv[1], "oom")) unavailable(NO);
    else if (!strcmp(argv[1], "worker-failure")) unavailable(YES);
    else if (!strcmp(argv[1], "interleave")) interleave(NO);
    else if (!strcmp(argv[1], "clear-timeout")) interleave(YES);
    else if (!strcmp(argv[1], "fallback-lock")) uncommitted_serialization();
    else if (!strcmp(argv[1], "resources")) resources();
    else if (!strcmp(argv[1], "phase")) phase_reads();
    else require(NO, "known scenario");
    dispatch_sync(queue->handlerQueue, ^{});
    require(wait_count(&cb_destroyed, queue->took), "every completion item and callback releases command ownership");
    [queue release]; [owner release]; require(device_alive == 0, "device releases after all command resources");
    printf("%s: %u checks passed\n", argv[1], checks); return 0;
} }

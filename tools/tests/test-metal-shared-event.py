#!/usr/bin/env python3
"""Exercise AnnibaleMetal encodeSignalEvent:value: and encodeWaitForEvent:value: under ASan/UBSan."""
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = ROOT / "tools/userland-driver"
AIR = ROOT.parent / "hackios-xnu-userland/tools/mesa/air"


def extract(path, marker):
    text = path.read_text()
    begin = text.index(marker)
    opening = text.index("{", begin)
    depth = 0
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]',
                             text[opening:], re.S):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return text[begin:opening + token.end()]
    raise RuntimeError(marker)


MAIN = r'''
@interface MockSharedEvent : NSObject <AnnibaleSharedEventSPI> {
    pthread_mutex_t _lock;
    pthread_cond_t _cv;
    uint64_t _value;
}
@end
@implementation MockSharedEvent
- (id)init {
    self = [super init];
    pthread_mutex_init(&_lock, NULL);
    pthread_cond_init(&_cv, NULL);
    _value = 0;
    return self;
}
- (void)dealloc {
    pthread_cond_destroy(&_cv);
    pthread_mutex_destroy(&_lock);
    [super dealloc];
}
- (uint64_t)signaledValue {
    pthread_mutex_lock(&_lock);
    uint64_t v = _value;
    pthread_mutex_unlock(&_lock);
    return v;
}
- (void)setSignaledValue:(uint64_t)value {
    pthread_mutex_lock(&_lock);
    if (value > _value) {
        _value = value;
    }
    pthread_cond_broadcast(&_cv);
    pthread_mutex_unlock(&_lock);
}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)timeoutMS {
    pthread_mutex_lock(&_lock);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeoutMS / 1000;
    deadline.tv_nsec += (long)(timeoutMS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    while (_value < value) {
        if (pthread_cond_timedwait(&_cv, &_lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    BOOL ok = _value >= value;
    pthread_mutex_unlock(&_lock);
    return ok;
}
@end

static AnnibaleMetalWork *make_draw(AnnibaleMetalRenderPipeline *p) {
    AnnibaleMetalWork *w = [[AnnibaleMetalWork alloc] init];
    w->render = p;
    return [w autorelease];
}

int main(void) {
    @autoreleasepool {
        unlocked = dispatch_semaphore_create(0);
        resume_commit = dispatch_semaphore_create(0);
        taking = dispatch_semaphore_create(0);
        resume_take = dispatch_semaphore_create(0);

        AnnibaleMetalDevice *dev = [[AnnibaleMetalDevice alloc] init];
        AnnibaleMetalCommandQueue *q = [[AnnibaleMetalCommandQueue alloc] initWithDevice:dev];
        AnnibaleMetalRenderPipeline *pipe = [[AnnibaleMetalRenderPipeline alloc] init];

        /* 1. Same-thread CPU->GPU kickoff: commit with unsatisfied wait must NOT block caller. */
        MockSharedEvent *kick = [[MockSharedEvent alloc] init];
        AnnibaleMetalCommandBuffer *cb1 = [[AnnibaleMetalCommandBuffer alloc] initWithAnnibaleQueue:q];
        [cb1 encodeWaitForEvent:kick value:7];
        [cb1 recordWork:make_draw(pipe)];
        [cb1 encodeSignalEvent:kick value:10];
        [cb1 commit];
        require(__atomic_load_n(&submitted, __ATOMIC_ACQUIRE) == 0, "unsatisfied wait defers GPU submission");
        require(q->returned == 1, "deferred commit returns queue slot immediately");

        /* Signal from same thread after -commit; deferred worker submits and completion signals 10. */
        [kick setSignaledValue:7];
        require(wait_count(&submitted, 1), "deferred buffer submits once event reaches 7");
        require([kick signaledValue] == 7, "encodeSignalEvent waits until GPU fence completes");
        resolve_fence(1, YES);
        [cb1 waitUntilCompleted];
        require([cb1 status] == RW_CB_COMPLETED, "cb1 completed");
        require([kick signaledValue] == 10, "GPU completion signals 10");
        [cb1 release];

        /* 2. Cross-buffer GPU->GPU chaining: cb2 signals 20 on fence 2, cb3 waits for 20 before submitting. */
        MockSharedEvent *chain = [[MockSharedEvent alloc] init];
        AnnibaleMetalCommandBuffer *cb2 = [[AnnibaleMetalCommandBuffer alloc] initWithAnnibaleQueue:q];
        [cb2 recordWork:make_draw(pipe)];
        [cb2 encodeSignalEvent:chain value:20];
        [cb2 commit];
        require(__atomic_load_n(&submitted, __ATOMIC_ACQUIRE) == 2, "cb2 submits immediately");

        AnnibaleMetalCommandBuffer *cb3 = [[AnnibaleMetalCommandBuffer alloc] initWithAnnibaleQueue:q];
        [cb3 encodeWaitForEvent:chain value:20];
        [cb3 recordWork:make_draw(pipe)];
        [cb3 commit];
        pause_ms(15);
        require(__atomic_load_n(&submitted, __ATOMIC_ACQUIRE) == 2, "cb3 waits for cb2 GPU completion");

        resolve_fence(2, YES);
        [cb2 waitUntilCompleted];
        require(wait_count(&submitted, 3), "cb3 submits after cb2 signals chain=20");
        resolve_fence(3, YES);
        [cb3 waitUntilCompleted];
        require([cb3 status] == RW_CB_COMPLETED, "cb3 completed");

        /* 3. Intra-buffer signal followed by wait on same event does not self-defer. */
        MockSharedEvent *intra = [[MockSharedEvent alloc] init];
        AnnibaleMetalCommandBuffer *cb4 = [[AnnibaleMetalCommandBuffer alloc] initWithAnnibaleQueue:q];
        [cb4 encodeSignalEvent:intra value:5];
        [cb4 encodeWaitForEvent:intra value:5];
        [cb4 recordWork:make_draw(pipe)];
        [cb4 commit];
        require(__atomic_load_n(&submitted, __ATOMIC_ACQUIRE) == 4, "intra-buffer signal satisfies wait");
        resolve_fence(4, YES);
        [cb4 waitUntilCompleted];
        require([intra signaledValue] == 5, "intra-buffer event reaches 5 on completion");

        [cb2 release];
        [cb3 release];
        [cb4 release];
        [intra release];
        [chain release];
        [kick release];
        [pipe release];
        [q release];
        [dev release];
    }
    printf("PASS: AnnibaleMetal shared event wait/signal (%u checks)\n", checks);
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="metal-shared-event-") as tmp:
        folder = Path(tmp)
        (folder / "fixture-prefix.h").write_text((HERE / "metal-completion-prefix.h").read_text())
        air = (AIR / "air_metal.c").read_text()
        lock_begin = air.index("static pthread_mutex_t rwa_lock")
        lock_end = air.index("\n\n", air.index("#define RWA_LOCKED"))
        locks = air[lock_begin:lock_end]
        timeout = re.search(r"#define RWA_FENCE_TIMEOUT_NS[^\n]+", air).group()
        fence = (AIR / "air_metal_fence.inc").read_text()
        (folder / "air-extracts.inc").write_text("\n\n".join([
            locks, timeout,
            extract(AIR / "air_metal_bind.inc", "static int\nfinish_or_say_why"),
            extract(AIR / "air_metal_bind.inc", "int\nrwa_flush("),
            extract(AIR / "air_metal_reflect_vertex.inc", "int\nrwa_finish("),
            fence,
        ]) + "\n")
        (folder / "texture-extracts.inc").write_text("\n\n".join([
            extract(SOURCE / "annibalemetal_texture.inc", "- (instancetype)initWithAnnibaleDevice:"),
            extract(SOURCE / "annibalemetal_texture.inc", "- (void)dealloc"),
        ]) + "\n")
        work = (SOURCE / "annibalemetal_work.inc").read_text()
        (folder / "work-extract.inc").write_text(work[:work.index("@end") + len("@end")] + "\n")
        queue = (SOURCE / "annibalemetal_queue.inc").read_text()
        at = queue.index("RW_CB_NOT_ENQUEUED")
        enum = queue[queue.rfind("enum {", 0, at):queue.index("};", at) + 2]
        (folder / "submit-extract.inc").write_text((SOURCE / "annibalemetal_commandbuffer_submit.inc").read_text())
        (folder / "completion-extract.inc").write_text((SOURCE / "annibalemetal_completion.inc").read_text())
        blocks = [
            '#include "fixture-prefix.h"', enum,
            extract(SOURCE / "annibalemetal_commandbuffer.inc", "@implementation AnnibaleMetalCommandBuffer"),
        ]
        for marker in [
            "- (instancetype)initWithAnnibaleQueue:", "- (void)annibaleReturnSlot", "- (void)dealloc",
            "- (void)encodeSignalEvent:", "- (void)encodeWaitForEvent:",
            "- (void)recordWork:", "- (void)failWith:", "- (NSUInteger)status",
            "- (NSError *)error", "- (id)device",
        ]:
            blocks.append(extract(SOURCE / "annibalemetal_commandbuffer.inc", marker))
        blocks.append("""
- (unsigned)fixturePhase { return __atomic_load_n(&_phase, __ATOMIC_ACQUIRE); }
- (BOOL)runDraw:(AnnibaleMetalWork *)work why:(char *)why size:(size_t)size {
    (void)work; (void)why; (void)size; recorded++; return YES;
}
- (void)thumbWork:(AnnibaleMetalWork *)work index:(unsigned)index after:(BOOL)after {
    (void)work; (void)index; (void)after;
}
- (BOOL)syncDraw:(AnnibaleMetalWork *)work index:(unsigned)index sample:(BOOL)sample why:(char *)why size:(size_t)size {
    (void)work; (void)index; (void)sample; (void)why; (void)size; return YES;
}
#include "submit-extract.inc"
""")
        blocks.append(extract(SOURCE / "annibalemetal_commandbuffer.inc", "- (void)waitUntilCompleted"))
        blocks.append('@end\n#include "completion-extract.inc"\n' + MAIN)
        src = folder / "fixture.m"
        exe = folder / "fixture"
        src.write_text("\n\n".join(blocks) + "\n")
        subprocess.run([
            "/usr/bin/xcrun", "clang", "-fno-objc-arc", "-O1", "-g", "-Werror=return-type",
            "-Wno-deprecated-declarations", "-fsanitize=address,undefined",
            "-I" + str(SOURCE), "-I" + str(AIR), str(src),
            "-framework", "Foundation", "-framework", "Metal", "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()

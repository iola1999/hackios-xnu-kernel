/*
 * AnnibaleGPU: the phone's Adreno for programs to draw with.
 *
 * bsd/dev/annibale_adreno.c wakes the GPU and, with boot-arg adreno_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's msm DRM driver,
 * the one Mesa's freedreno knows: every request of that interface is a call of the user client
 * with the request's number as the selector and the request's structure passed in and back
 * (userland: tools/mesa/annibale-drm, which stands in for libdrm inside Mesa). What Linux does
 * with mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * What there is of the interface is what freedreno uses to draw: parameters, buffers (made,
 * told their address in the GPU's address space, mapped, given back), submission of command
 * buffers, and waiting for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context; it is zero when made, and write-combined: what the program writes goes to
 * memory, where the GPU reads it, and not into this processor's cache (as Linux maps them for
 * freedreno; boot-arg gpu_cached=1 makes them ordinary memory, pushed out of the cache whenever
 * the GPU is given something to do: see submit()).
 *
 * One request is not Linux's: a buffer that is the memory of an IOSurface (surfaceBuffer()), for
 * drawing into what the display shows and for reading pictures where other programs have drawn
 * them. Linux hands such memory around as file descriptors; here it has a name already. That
 * memory is ordinary, cached memory, which everybody else reads and writes as such. Another is a
 * buffer that is the calling program's own memory (memoryBuffer()), for Metal's no-copy buffers.
 *
 * Not here yet: interrupts (waiting is asking again five times a millisecond), more than one
 * program's worth of care (every client's buffers are in the one address space; a client that
 * goes takes its own away), fences and buffers as file descriptors. A GPU that has stopped
 * answering is reported (annibale_gpu_report()) and, with boot-arg gpu_recover=1, reset and
 * started again, so that the submission that hung is all that is lost: what that does and
 * which source says so is at annibale_gpu_recover() in bsd/dev/annibale_adreno.c. Without the
 * boot-arg it stays stopped and nothing is waited for again.
 */

#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "MiniIOSurface.h"

extern "C" void annibale_tl(const char * format, ...) __printflike(1, 2);

extern "C" {
/* bsd/dev/annibale_adreno.c ("the GPU kept on"; declared in bsd/sys/annibale_input.h, which is not seen from here) */
bool annibale_platform_is_annibale(void);
bool annibale_gpu_ready(void);
bool annibale_gpu_accepting(void);              /* annibale_gpu_ready() without its health check: safe with no lock held */
bool annibale_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t annibale_gpu_address(uint64_t size, uint32_t * refusal);
/* What `refusal` of annibale_gpu_address says when no address is given: the values of
 * ADRENO8_BO_REFUSE_* in bsd/dev/adreno8/adreno8_bo_private.h, repeated here because the
 * headers of the two sides are not shared. */
#define GPU_REFUSE_NOT_READY    1    /* the GPU is going or gone (its health check inside the reservation refused) */
#define GPU_REFUSE_NO_RANGE     2    /* no free span in the GPU's address space */
#define GPU_REFUSE_NO_TABLE     3    /* no page table for the span: the table slots or the memory for one */
void annibale_gpu_address_free(uint64_t address, uint64_t size);
void annibale_gpu_space(uint64_t * used, uint32_t * live, uint64_t * largest, unsigned * holes, unsigned * most,
    uint64_t * lost, uint32_t * refused);
extern uint32_t gpu_va;                 /* boot-arg gpu_va: 0 as before, 1 describe the addresses, 2 also guard them */
bool annibale_gpu_map(uint64_t address, uint64_t phys, bool read_only);
bool annibale_gpu_mapped(void);
bool annibale_gpu_unmap(uint64_t address, uint64_t size);
void annibale_gpu_to_memory(void * start, uint64_t size);
uint32_t annibale_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
extern uint32_t gpu_log;
bool annibale_gpu_done(uint32_t fence);
uint32_t annibale_gpu_retired(void);
uint32_t annibale_gpu_retired_for(uint32_t fence);
bool annibale_gpu_progress(uint64_t * sig);
uint32_t annibale_gpu_irqs(void);
void annibale_gpu_pause(uint32_t seen, uint32_t poll_us, uint32_t longest_us);
uint32_t annibale_gpu_last(void);
uint32_t adreno8_context_depth(void);
void annibale_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void annibale_gpu_speed(uint32_t * mhz, uint32_t * changes);
void annibale_gpu_report(const char * why);
bool annibale_gpu_recover(void);
void annibale_gpu_stop(void);

bool AnnibaleGPUStart(void);
void AnnibaleGPUStop(void);
bool annibale_gpu_focal_active(uint64_t now);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kAnnibaleMemory          = 0x7c,         /* ours: the caller's own memory as a buffer */
	kAnnibaleGPUWorked       = 0x7d,         /* ours: how long the 3D core itself has worked, and on how many */
	kAnnibaleSurface         = 0x7e,         /* ours: an IOSurface's memory as a buffer */
	kAnnibaleShow            = 0x7f,         /* ours: pixels of the caller's onto the panel, to see a test's result */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct annibale_gem_new {
	struct msm_gem_new request;
	uint64_t iova;
};
static_assert(sizeof(struct annibale_gem_new) == 24, "GEM_NEW with an address");
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct annibale_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kAnnibaleDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
struct annibale_memory {
	uint64_t address, size;                 /* the caller's, in whole 4 KB pages */
	uint32_t handle, pad;                   /* back: the buffer */
};
struct annibale_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};
/*
 * kAnnibaleGPUWorked: the GPU's own side of a measurement a program makes with its own clock.
 *
 * A program timing this driver (userland/src/metal_drv_test.m --time) can measure only wall
 * time: the whole of a draw, which is its own work, this ioctl, the submission, the core's
 * execution and the wait. Nothing it can reach says how much of that was the core -- and the
 * kernel has known all along, because bsd/dev/annibale_adreno.c times every submission off
 * CP_ALWAYS_ON_COUNTER (19.2 ticks a microsecond) and until now only ever printed it. This
 * request hands the same three numbers to the program, so the subtraction happens where the
 * two halves are both in hand instead of in whoever reads the log afterwards.
 *
 * It RESETS what it reports, which is annibale_gpu_worked()'s own contract ("since this was
 * last asked") and is what makes it useful: a caller reads once to zero the counters, does the
 * work it wants to price, and reads again. Two consequences, and they are the whole of the
 * sharp edge: two programs asking at once each get a part of the truth, and the kernel's own
 * five-second report in submit() below shares the same counters -- so a boot that reads this
 * will see gaps in those lines, and that is expected rather than a fault.
 */
struct annibale_worked {
	uint64_t microseconds;                  /* the core's own time on the submissions counted here */
	uint32_t submissions;                   /* how many of them it timed; 0 means nothing finished since */
	uint32_t by[7];                         /* ... of which under 0.1, 0.25, 0.5, 1, 2, 4 ms, and longer */
};

#define kAnnibaleDescribe        0xffffffffu

#define MSM_PARAM_COMM          0x0c
#define MSM_PARAM_CMDLINE       0x0d
#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_SET_IOVA       4
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_PIPE_3D0            0x10
#define MSM_SUBMIT_NO_IMPLICIT  0x80000000u
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             8192            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_A830_SUBMIT_CMDS    82
#define GPU_SUBMIT_BOS          8192
#define GPU_PATIENCE_MS         5000            /* for a buffer or a submission that somebody waits for without a limit */
#define GPU_STALL_MS            30000           /* no submission retired for this long with one waited for: the core has stopped */
#define GPU_ASK_AGAIN_US        200             /* while waiting for the core: how long between two looks at how far it is */
#define GPU_ASK_LONGEST_US      2000            /* ... and with its completion interrupt (gpu_irq): the longest sleep between two */
#define GPU_PUSH_AGAIN_MS       4               /* a surface is pushed out of the cache again for a submission this long after the last */

static IOLock * gGPULock;                       /* around everything that is the GPU's (annibale_gpu_...) */
static bool gGPUReported;
static bool gGPUCached;                         /* boot-arg gpu_cached=1 */
static bool gGPUCoherent;                       /* boot-arg gpu_coherent=1: the GPU snoops the caches (adreno8.c) */
static bool gGPUSaidSetIova;                    /* MSM_INFO_SET_IOVA has been refused out loud once */

/* PID -> IOGPU role table updated by RunningBoard (`type = 1`) and consulted on 3D submit. */
struct gpu_pid_role {
	pid_t    pid;
	uint8_t  role;
	uint32_t gen;
};
#define GPU_PID_ROLES           64
#define GPU_ROLE_BACKGROUND     2u
static IOSimpleLock * gGPURoleLock;
static struct gpu_pid_role gGPURoles[GPU_PID_ROLES];
static uint32_t gGPURoleGen;
static uint64_t gGPUFocalActiveUntil;

static void
gpuRoleSet(pid_t pid, uint8_t role)
{
	uint8_t clamped = role > GPU_ROLE_BACKGROUND ? GPU_ROLE_BACKGROUND : role;
	int empty = -1, oldest = 0;

	if (pid <= 0 || gGPURoleLock == NULL) {
		return;
	}
	IOSimpleLockLock(gGPURoleLock);
	gGPURoleGen++;
	for (int i = 0; i < GPU_PID_ROLES; i++) {
		if (gGPURoles[i].pid == pid) {
			gGPURoles[i].role = clamped;
			gGPURoles[i].gen = gGPURoleGen;
			IOSimpleLockUnlock(gGPURoleLock);
			return;
		}
		if (gGPURoles[i].pid == 0 && empty < 0) {
			empty = i;
		}
		if (gGPURoles[i].gen < gGPURoles[oldest].gen) {
			oldest = i;
		}
	}
	int slot = empty >= 0 ? empty : oldest;
	gGPURoles[slot].pid = pid;
	gGPURoles[slot].role = clamped;
	gGPURoles[slot].gen = gGPURoleGen;
	IOSimpleLockUnlock(gGPURoleLock);
}

static uint8_t
gpuRoleGet(pid_t pid)
{
	uint8_t role = GPU_ROLE_BACKGROUND;

	if (pid <= 0 || gGPURoleLock == NULL) {
		return role;
	}
	IOSimpleLockLock(gGPURoleLock);
	for (int i = 0; i < GPU_PID_ROLES; i++) {
		if (gGPURoles[i].pid == pid) {
			role = gGPURoles[i].role;
			gGPURoles[i].gen = ++gGPURoleGen;
			break;
		}
	}
	IOSimpleLockUnlock(gGPURoleLock);
	return role;
}

static void
gpuNoteSubmit(pid_t pid)
{
	uint64_t boost_ticks, until, cur;

	if (gpuRoleGet(pid) > 1u) {
		return;
	}
	nanoseconds_to_absolutetime(50ULL * NSEC_PER_MSEC, &boost_ticks);
	until = mach_absolute_time() + boost_ticks;
	cur = __atomic_load_n(&gGPUFocalActiveUntil, __ATOMIC_RELAXED);
	while (cur < until &&
	    !__atomic_compare_exchange_n(&gGPUFocalActiveUntil, &cur, until, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

bool
annibale_gpu_focal_active(uint64_t now)
{
	return now < __atomic_load_n(&gGPUFocalActiveUntil, __ATOMIC_RELAXED);
}

/* What the last seconds' submissions were and cost here, for the log. */
static struct {
	uint64_t since, flushing, waiting, taking;      /* times of the processor's clock */
	uint64_t flushed;                       /* bytes sent out of the cache */
	uint32_t submissions, commands, buffers, surfaces, waits, over;
	uint32_t made, given_again;             /* buffers of their own made for programs, and of those kept ones given again */
	uint64_t making;                        /* ... and the time it took to make the others */
	uint64_t allocating;                    /* ... of which getting the memory, before entering it into the GPU's address space */
	uint32_t made_by_size[4];               /* ... how many of them under 64 KB, 1 MB, 8 MB, and larger */
	uint64_t locking;                       /* ... and waiting for gGPULock to enter them */
	uint64_t submitting, rooming;           /* time in submit() from its start, and in it waiting for room in the A830's queue */
	uint32_t hit_by_size[4];                /* kept ones given again, in the size classes of made_by_size */
	uint32_t miss_size, miss_flags;         /* a gemNew the pool had nothing the size of; the size there, the flags not */
	uint32_t evict_entrycap, evict_bytecap; /* keep() drops: no free place of GPU_KEPT; over GPU_KEPT_BYTES */
	uint32_t expire_drop;                   /* pool entries gemNew's scan let go for being kept GPU_KEPT_SECONDS */
} gGPUStat;
static uint32_t gGPUIrqsSeen;                   /* annibale_gpu_irqs() at the last statistics */

#include "AnnibaleGPUWait.inc"

bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	uint32_t fence = surface->drawn;
	bool done = true;
	bool syncCache = false;

	if (gGPULock != NULL) {
		IOLockLock(gGPULock);
		done = !gGPUReported && (fence == 0 || (annibale_gpu_ready() && gpuWait(fence, milliseconds)));
		if (done && fence != 0 && surface->cpuSyncedFence != fence) {
			surface->cpuSyncedFence = fence;
			syncCache = !gGPUCoherent && surface->client == NULL && surface->kbase != NULL &&
			    surface->info.allocSize != 0;
		}
		IOLockUnlock(gGPULock);
		if (syncCache) {
			/* Clean/invalidate stale CPU cache lines before CPU reads GPU-rendered DRAM pixels. */
			annibale_gpu_to_memory(surface->kbase + PAGE_SIZE, round_page_64(surface->info.allocSize));
		}
	}
	return done;
}

bool
MiniIOSurfaceGPUStopped(void)
{
	bool stopped = !annibale_gpu_ready();

	if (gGPULock != NULL) {
		IOLockLock(gGPULock);
		stopped = stopped || gGPUReported;
		IOLockUnlock(gGPULock);
	}
	return stopped;
}

/*
 * Boot-arg touch_lat=1: a line for every wait of a program's for the GPU that took a millisecond or
 * more ("tl: f KIND US at US"), for telling apart what holds a frame up (tools/frames.py).
 */
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);

static void
slowWait(const char * kind, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	if (ns >= NSEC_PER_MSEC) {
		absolutetime_to_nanoseconds(now, &at);
		annibale_tl("tl: f %d %llu %s %llu at %llu\n", proc_pid(current_proc()),
		    (unsigned long long)thread_tid(current_thread()), kind, ns / 1000, at / 1000);
	}
}

/*
 * The rest of a wait's story, under the same touch_lat=1 switch: "tl: f" exists only when the
 * ioctl returned, so a wait the thread never comes back from -- or one that keeps failing --
 * leaves nothing. "tl: F PID TID KIND FENCE PATIENCE_MS at US" is written before the wait
 * begins, and "tl: X PID TID KIND FENCE retired R last L at US" when it fails, with R the
 * newest timestamp the GPU retired and L the newest it was ever given (annibale_gpu_retired/
 * annibale_gpu_last, which count in the same timestamps as the fence). R at or past FENCE with
 * the wait failed is a completion the wait could not see; FENCE beyond L is userspace waiting
 * for work that was never submitted. annibale_tl writes a memory ring, so these are safe with
 * the GPU lock held; the entry line must precede the wait itself or it is worthless.
 */
static void
fenceWaitEntry(const char * kind, uint32_t fence, uint64_t patience)
{
	uint64_t at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &at);
	annibale_tl("tl: F %d %llu %s %u %llu at %llu\n", proc_pid(current_proc()),
	    (unsigned long long)thread_tid(current_thread()), kind, fence,
	    (unsigned long long)patience, at / 1000);
}

static void
fenceWaitFailed(const char * kind, uint32_t fence)
{
	uint64_t at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &at);
	annibale_tl("tl: X %d %llu %s %u retired %u last %u at %llu\n", proc_pid(current_proc()),
	    (unsigned long long)thread_tid(current_thread()), kind, fence,
	    annibale_gpu_retired_for(fence), annibale_gpu_last(), at / 1000);
}

/*
 * The same switch over the rest of what a WriteRead ioctl can be kept by (kernel-submit-blocking
 * report, batch 5; the WriteRead paths have no per-event lines of their own at all):
 *
 * "tl: D PID TID target F inflight N depth M at US" -- submit() is about to wait for room in the
 * queue: F is the fence whose retirement makes room, fixed here rather than advancing with later
 * submissions (submit() says why), N the submissions outstanding at this moment. "tl: d PID TID
 * target F US at US" -- the wait is over and took US microseconds.
 *
 * "tl: L PID TID sel SEL US at US" -- acquiring the GPU lock for request SEL took at least
 * GPU_LOCK_NOTICE_US: the queueing a long-held lock makes every other request stand in.
 *
 * "tl: S PID TID surface ID KB US at US" -- an IOSurface's import as a buffer took at least
 * GPU_IMPORT_NOTICE_US with the lock held. KB is the surface's size in kilobytes: the cost is in
 * the pages.
 *
 * "tl: I PID TID US at US" -- printing the five-second statistics in submit() itself took US
 * microseconds, lock held throughout: the block is the leading suspect for the flips-window
 * stalls, so its cost is measured where nothing else can see it. This one has no threshold --
 * the distribution is the answer.
 */
#define GPU_LOCK_NOTICE_US      2000
#define GPU_IMPORT_NOTICE_US    2000

static void
depthWaitEntry(uint32_t target, uint32_t depth)
{
	uint64_t at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &at);
	annibale_tl("tl: D %d %llu target %u inflight %u depth %u at %llu\n", proc_pid(current_proc()),
	    (unsigned long long)thread_tid(current_thread()), target,
	    (annibale_gpu_last() - annibale_gpu_retired_for(target)) & 0x7fffffffu, depth, at / 1000);
}

static void
depthWaitExit(uint32_t target, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	absolutetime_to_nanoseconds(now, &at);
	annibale_tl("tl: d %d %llu target %u %llu at %llu\n", proc_pid(current_proc()),
	    (unsigned long long)thread_tid(current_thread()), target, ns / 1000, at / 1000);
}

/* IOLockLock(gGPULock) for a program's request, with the queueing for it measured. */
static void
gpuLockIoctl(uint32_t selector)
{
	uint64_t began = 0;

	if (gWaitLines) {
		began = mach_absolute_time();
	}
	IOLockLock(gGPULock);
	if (gWaitLines) {
		uint64_t now = mach_absolute_time(), ns, at;

		absolutetime_to_nanoseconds(now - began, &ns);
		if (ns >= (uint64_t) GPU_LOCK_NOTICE_US * NSEC_PER_USEC) {
			absolutetime_to_nanoseconds(now, &at);
			annibale_tl("tl: L %d %llu sel %#x %llu at %llu\n", proc_pid(current_proc()),
			    (unsigned long long)thread_tid(current_thread()), selector, ns / 1000, at / 1000);
		}
	}
}

/*
 * Boot-arg gpu_io_log=1: one line for every WriteRead ioctl of the five that make up the
 * desktop's traffic (gemNew, gemInfo, submit, surface, memory), however fast it was -- "tl: L"
 * answers only how often the queueing passed 2 ms, and a distribution needs the fast ones too.
 *
 * "tl: io PID TID sel SEL queue Q held H total T at US", all of Q, H, T and US in microseconds:
 * Q is what acquiring the GPU lock cost in all, summed when a request takes it more than once
 * (gemNew does, once for the pool and once for the making); H is the span from each acquisition
 * to its release, summed the same way -- for submit() the depth wait is part of the span because
 * the wait starts and ends inside it (gpuWait() lets go of the lock meanwhile; the "tl: D/d"
 * lines price the wait itself), and gemInfo takes no lock at all since batch 8, so both stay 0
 * there, which is what the lines are for; T is the whole ioctl and US when it began.
 */
static TUNABLE(uint32_t, gGPUIoLog, "gpu_io_log", 0);

struct gpu_io_stat {
	uint64_t queueing;                      /* of the processor's clock, as mach_absolute_time() counts */
	uint64_t span;
};

/* gpuLockIoctl() with the queueing added to the stat; the acquisition time is the answer, for ioUnlock(). */
static uint64_t
ioLock(struct gpu_io_stat * io, uint32_t selector)
{
	uint64_t pre = 0, acq;

	if (gGPUIoLog) {
		pre = mach_absolute_time();
	}
	gpuLockIoctl(selector);
	if (!gGPUIoLog) {
		return 0;
	}
	acq = mach_absolute_time();
	io->queueing += acq - pre;
	return acq;
}

static void
ioUnlock(struct gpu_io_stat * io, uint64_t acq)
{
	if (gGPUIoLog) {
		io->span += mach_absolute_time() - acq;
	}
	IOLockUnlock(gGPULock);
}

static void
ioLine(uint32_t selector, struct gpu_io_stat * io, uint64_t began)
{
	uint64_t now = mach_absolute_time(), queueing, span, total, at;

	absolutetime_to_nanoseconds(io->queueing, &queueing);
	absolutetime_to_nanoseconds(io->span, &span);
	absolutetime_to_nanoseconds(now - began, &total);
	absolutetime_to_nanoseconds(began, &at);
	annibale_tl("tl: io %d %llu sel %#x queue %llu held %llu total %llu at %llu\n", proc_pid(current_proc()),
	    (unsigned long long)thread_tid(current_thread()), selector, queueing / 1000, span / 1000, total / 1000, at / 1000);
}

/* How long from now a time of msm's is, in milliseconds; 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when->seconds * (int64_t) NSEC_PER_SEC + when->nanoseconds - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/*
 * A wait that was to end at a time that had passed when it was asked for, and the GPU was not
 * done: whoever asked goes on as if it were (a picture is shown half drawn). That is what comes
 * of a caller whose clock is not this one, so the first few are told.
 */
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
	static uint32_t told;
	uint64_t now;

	gGPUStat.over++;
	if (told < 4) {
		told++;
		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		IOLog("AnnibaleGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
		    "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
	}
}

static bool
waitTimedFence(const char * kind, uint32_t fence, const struct msm_timespec * timeout, bool nosync,
    IOReturn failRet, IOReturn * ret)
{
	uint64_t patience = nosync ? 0 : millisecondsUntil(timeout);

	fenceWaitEntry(kind, fence, patience);
	if (gpuWait(fence, patience)) {
		return true;
	}
	fenceWaitFailed(kind, fence);
	*ret = failRet;
	if (patience == 0 && !nosync) {
		waitOver(timeout, fence);
	}
	return false;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOGPU : public IOService
{
	OSDeclareDefaultStructors(IOGPU);
};

OSDefineMetaClassAndStructors(IOGPU, IOService);

class AnnibaleGPU : public IOGPU
{
	OSDeclareDefaultStructors(AnnibaleGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AnnibaleGPU, IOGPU);

bool
AnnibaleGPU::start(IOService * provider)
{
	if (!IOGPU::start(provider)) {
		return false;
	}
	if (!setProperty(kIOUserClientClassKey, "AnnibaleGPUUserClient")) return false;
	setName("AnnibaleGPU");
	registerService(kIOServiceSynchronous);
	return (getState() & (kIOServiceRegisteredState | kIOServiceInactiveState)) == kIOServiceRegisteredState;
}

static AnnibaleGPU * gGPUService;

bool
AnnibaleGPUStart(void)
{
	if (gGPUService != NULL) {
		return (gGPUService->getState() & (kIOServiceRegisteredState | kIOServiceInactiveState)) == kIOServiceRegisteredState;
	}
	IOService * provider = IOService::getPlatform();
	AnnibaleGPU * gpu = NULL;
	uint32_t cached = 0;

	if (provider == NULL || (gGPULock = IOLockAlloc()) == NULL || (gGPURoleLock = IOSimpleLockAlloc()) == NULL) {
		if (gGPULock != NULL) {
			IOLockFree(gGPULock);
			gGPULock = NULL;
		}
		IOLog("AnnibaleGPU: cannot allocate the service lock or find its provider\n");
		return false;
	}
	gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
	cached = 0;
	gGPUCoherent = PE_parse_boot_argn("gpu_coherent", &cached, sizeof(cached)) && cached == 1;
	gGPUCached = gGPUCached || gGPUCoherent;
	gpu = OSTypeAlloc(AnnibaleGPU);
	if (gpu == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOSimpleLockFree(gGPURoleLock);
		gGPURoleLock = NULL;
		IOLockFree(gGPULock);
		gGPULock = NULL;
		IOLog("AnnibaleGPU: cannot create the service\n");
		return false;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		gpu->release();
		IOSimpleLockFree(gGPURoleLock);
		gGPURoleLock = NULL;
		IOLockFree(gGPULock);
		gGPULock = NULL;
		IOLog("AnnibaleGPU: the service did not register\n");
		return false;
	}
	gGPUService = gpu; /* Keep the actual registered service for repeated start checks. */
	return true;
}

/* The run is ending (annibale_adreno.c): nobody is in the middle of anything when the GPU goes. */
void
AnnibaleGPUStop(void)
{
	if (gGPULock == NULL) return;
	IOLockLock(gGPULock);
	annibale_gpu_stop();
	IOLockUnlock(gGPULock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *            memory;         /* NULL: this handle is free */
	uint8_t *                       kernel;         /* where it is in the kernel */
	MiniIOSurface *                 surface;        /* whose memory it is, when not its own */
	uint64_t                        address;        /* the GPU's for it */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        seed;           /* the surface's when it was last pushed out of the cache, */
	uint64_t                        pushed;         /* ... and when that was */
	bool                            seen, pooled;   /* seen: mapped by program; pooled: came from gemNew and not shared */
	uint32_t                        fence;          /* the last submission that names it (0: none has) */
};

/*
 * A buffer given back while the GPU is still at it: its memory and its place in the GPU's
 * address space are kept until the submission is done, and nobody waits for that. (A window
 * server that makes a surface for every frame gives the last frame's back while the GPU draws
 * this one: waiting there, for all the GPU had been given, cost it some five milliseconds a
 * frame.)
 */
struct gpu_left {
	IOMemoryDescriptor *            memory;
	MiniIOSurface *                 surface;
	uint64_t                        address, size;
	uint32_t                        fence, flags;
	bool                            pooled;
};
#define GPU_LEFT                256     /* given back while the GPU is busy: about a frame of an animation's worth */
struct gpu_garbage;

/*
 * A buffer of the program's own given back, and done with: kept, memory and place in the GPU's
 * address space, for the next one of the same size and kind. CoreAnimation makes a new texture to
 * draw a blur into for every frame of an animation with one (Search: 1082 x 1226, 6 MB with Mesa's
 * rounding), and gives them back only later, a score at a time; Mesa keeps what is given back for
 * a second and then lets it go. Each made afresh here took 2 to 3.5 ms of the window server's frame
 * (memory got, cleared, made write-combined, and entered page by page into the GPU's address
 * space). A buffer kept is given again as it is, not cleared: it was the same program's (Mesa's own
 * cache gives buffers back the same way). At most GPU_KEPT_BYTES are kept, none for longer than
 * GPU_KEPT_SECONDS; a full entry array keeps soft size shares (keep()). rc.jobs.fps, Search: 8 such
 * textures made slowly instead of 77, the window server's frames drawn in 6.8 ms instead of 9.3-10 (median).
 *
 * Small buffers are kept since batch 8: the making of one is the same allocation, address space
 * reservation, page table writes and lock round trips whatever its size, and the desktop's icon
 * textures ran the whole lifecycle hundreds of times a frame while only buffers of 256 KB or
 * more were kept. Sizes under 256 KB are rounded up to a power of two when made (gemNew()), so
 * near sizes share one entry. A kept small buffer costs at most its bucket of wired memory; the byte
 * cap bounds the pool as before. None of this applies to a surface's or the program's own memory, which keep() never takes.
 */
struct gpu_kept {
	IOMemoryDescriptor *            memory;         /* NULL: this place is free */
	uint8_t *                       kernel;
	uint64_t                        address, size, since;
	uint32_t                        flags;
};
#define GPU_KEPT_SMALL_TARGET   320             /* soft share when full: sizes under 64 KB are most gemNew requests */
#define GPU_KEPT_MEDIUM_TARGET  128             /* 64 KB to 1 MB */
#define GPU_KEPT_LARGE_TARGET   48              /* 1 MB to 8 MB */
#define GPU_KEPT_HUGE_TARGET    16              /* 8 MB and up: few requests, but costly to make again */
#define GPU_KEPT                (GPU_KEPT_SMALL_TARGET + GPU_KEPT_MEDIUM_TARGET + GPU_KEPT_LARGE_TARGET + GPU_KEPT_HUGE_TARGET)
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);      /* boot-arg gpu_keep=0 (a test): none kept; gpu_keep=2: kept ones cleared when given again */

class AnnibaleGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AnnibaleGPUUserClient);

	task_t                  fTask;
	uint32_t                fClientType;
	pid_t                   fPid;
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextQueue;
	uint32_t                fNextBuffer;            /* where the search for a free handle starts: one given back is not the next given */
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	uint32_t                fStrangers;             /* requests that named a buffer the client does not have */
	struct gpu_left         fLeft[GPU_LEFT];
	uint32_t                fLeftCount;
	struct gpu_kept *       fKept;                  /* allocated with fBuffers: GPU_KEPT of them would not fit the object itself */
	uint64_t                fKeptBytes;

	struct gpu_buffer * buffer(uint32_t handle);
	uint32_t lastUse(uint32_t handle);
	struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle, IOReturn * error);
	void freeBuffer(struct gpu_buffer * b, struct gpu_garbage * garbage = NULL);
	bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags, struct gpu_garbage * garbage = NULL);
	bool dropKept(struct gpu_kept * k, struct gpu_garbage * garbage = NULL);
	void reap(struct gpu_garbage * garbage = NULL, uint32_t budget = 0);
	IOReturn gemClose(uint32_t handle);
	IOReturn gemNew(struct msm_gem_new * request, uint64_t * iova, struct gpu_io_stat * io);
	IOReturn surfaceBuffer(struct annibale_surface * request, struct gpu_io_stat * io);
	IOReturn memoryBuffer(struct annibale_memory * request, struct gpu_io_stat * io);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request, struct gpu_io_stat * io);
	IOReturn show(const struct annibale_show * request);
	IOReturn iogpuControlMethod(uint32_t selector, IOExternalMethodArguments * args);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AnnibaleGPUUserClient, IOUserClient);

bool
AnnibaleGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	fClientType = type & 0xffffu;
	fPid = proc_selfpid();
	return true;
}

bool
AnnibaleGPUUserClient::start(IOService * provider)
{
	if (fClientType == 1u) {
		return IOUserClient::start(provider);
	}
	if (gGPULock == NULL) return false;
	IOLockLock(gGPULock);
	bool available = !gGPUReported && annibale_gpu_ready();
	IOLockUnlock(gGPULock);
	if (!available || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	if (fBuffers == NULL) {
		return false;
	}
	fKept = IONewZero(struct gpu_kept, GPU_KEPT);
	if (fKept == NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
		return false;
	}
	return true;
}

#include "AnnibaleGPUMemory.inc"

#include "AnnibaleGPUSubmit.inc"

/* Pixels of the caller's (32 bits each, blue first) copied to the panel: for a test to show what it has drawn. */
IOReturn
AnnibaleGPUUserClient::show(const struct annibale_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
AnnibaleGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	if (fBuffers == NULL) {
		return kIOReturnUnsupported;
	}
	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		b->seen = true;
		*memory = b->memory;
		/*
		 * A program's own memory (memoryBuffer(): no kernel address, no surface) is mapped again,
		 * not handed back at the program's own address, which IOKit does for a mapping into the
		 * task the memory is in: the program may unmap that address while the buffer is alive.
		 * Guard pages are what makes IOKit map it again. kIOMapUnique does too, but then
		 * IOConnectUnmapMemory64 cannot look the mapping up and panics in doMap (cmd35-bo8).
		 */
		*options = 0;
		if (b->kernel == NULL && b->surface == NULL) {
			*options = kIOMapGuardedSmall | ((b->flags & MSM_BO_GPU_READONLY) ? kIOMapReadOnly : 0);
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
AnnibaleGPUUserClient::iogpuControlMethod(uint32_t selector, IOExternalMethodArguments * args)
{
	switch (selector) {
	case 0:
		if (args->structureOutput == NULL || args->structureOutputSize < 0x40u) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, 0x40u);
		args->structureOutputSize = 0x40u;
		return kIOReturnSuccess;
	case 2: {
		uint64_t reg_id = gGPUService != NULL ? gGPUService->getRegistryEntryID() : 1ULL;

		if (args->structureOutput == NULL || args->structureOutputSize < 0x218u) {
			return kIOReturnBadArgument;
		}
		if (reg_id == 0) {
			reg_id = 1ULL;
		}
		bzero(args->structureOutput, 0x218u);
		memcpy(args->structureOutput, &reg_id, sizeof(reg_id));
		args->structureOutputSize = 0x218u;
		return kIOReturnSuccess;
	}
	case 4:
		if (args->structureOutput == NULL || args->structureOutputSize < 0x20u) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, 0x20u);
		args->structureOutputSize = 0x20u;
		return kIOReturnSuccess;
	case 0x21:
		if (args->scalarInput == NULL || args->scalarInputCount < 2u) {
			return kIOReturnBadArgument;
		}
		gpuRoleSet((pid_t)(int32_t)args->scalarInput[0], (uint8_t)args->scalarInput[1]);
		return kIOReturnSuccess;
	case 0x22:
		if (args->scalarInput == NULL || args->scalarInputCount < 1u ||
		    args->scalarOutput == NULL || args->scalarOutputCount < 1u) {
			return kIOReturnBadArgument;
		}
		args->scalarOutput[0] = gpuRoleGet((pid_t)(int32_t)args->scalarInput[0]);
		args->scalarOutputCount = 1u;
		return kIOReturnSuccess;
	default:
		return kIOReturnUnsupported;
	}
}

IOReturn
AnnibaleGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct msm_param param;
		struct annibale_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct annibale_surface surface;
		struct annibale_memory memory;
		struct annibale_show show;
		struct annibale_worked worked;
	} r;
	size_t size = args->structureInputSize;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience, waitBegan;
	uint32_t fence;
	struct gpu_io_stat io = { 0, 0 };
	uint64_t ioBegan = gGPUIoLog ? mach_absolute_time() : 0;

	if (fClientType == 1u) {
		return iogpuControlMethod(selector, args);
	}
	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	/*
	 * The availability check without the lock (batch 8): it used to take gGPULock here, once
	 * per request and again in the case itself, so even a request that needs no lock queued
	 * behind whatever held it. gGPUReported and the backend's state both move one way only --
	 * once stopped, nothing is started again in this run -- so a stale read at worst lets a
	 * request through that the case's own locked check then refuses (every case that touches
	 * the GPU asks annibale_gpu_ready() again with the lock held, and the health check stays
	 * there: adreno8_bo_ready() drains the HFI queues, which only the lock may). gemInfo does
	 * not ask again, and does not need to: it reads this client's own bookkeeping, never the
	 * GPU.
	 */
	if (selector != kAnnibaleShow &&
	    (gGPULock == NULL || __atomic_load_n(&gGPUReported, __ATOMIC_RELAXED) || !annibale_gpu_accepting())) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		gpuLockIoctl(kMsmGetParam);
		ret = annibale_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		IOLockUnlock(gGPULock);
		break;
	case kMsmSetParam:
		/*
		 * Only the program's name and command line, for Linux's lists of who uses the GPU. Turnip
		 * reads success of any other, MSM_PARAM_EN_VM_BIND above all, as the parameter taking effect.
		 */
		if (r.param.param != MSM_PARAM_COMM && r.param.param != MSM_PARAM_CMDLINE) {
			ret = kIOReturnBadArgument;
		}
		break;
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew.request, size == sizeof(r.gemNew) ? &r.gemNew.iova : NULL, &io);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit, &io);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with a buffer */
	case kMsmWaitFence:
		waitBegan = mach_absolute_time();
		gpuLockIoctl(selector);
		if (selector == kMsmWaitFence) {
			waitTimedFence("fence", r.waitFence.fence, &r.waitFence.timeout, false, kIOReturnTimeout, &ret);
		} else {
			for (int pass = 0; pass < 2 && (fence = lastUse(r.cpuPrep.handle)) != 0; pass++) {
				if (!waitTimedFence("cpu-prep", fence, &r.cpuPrep.timeout, r.cpuPrep.op & MSM_PREP_NOSYNC,
				    kIOReturnBusy, &ret) || lastUse(r.cpuPrep.handle) == fence) {
					break;
				}
			}
		}
		IOLockUnlock(gGPULock);
		slowWait(selector == kMsmWaitFence ? "fence" : "cpu-prep", waitBegan);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		if (annibale_platform_is_annibale()) {
			uint64_t prios = 1;
			gpuLockIoctl(kMsmSubmitQueueNew);
			ret = !annibale_gpu_parameter(0x07, &prios) ? kIOReturnNotReady
			    : (r.queue.flags != 0 || r.queue.prio >= prios) ? kIOReturnBadArgument : kIOReturnSuccess;
			IOLockUnlock(gGPULock);
		}
		if (ret == kIOReturnSuccess) { r.queue.id = ++fNextQueue; }
		break;
	case kMsmSubmitQueueQuery:                      /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		ret = gemClose(r.close.handle);
		break;
	case kAnnibaleGPUWorked:
		if (annibale_platform_is_annibale()) {
			ret = kIOReturnUnsupported;
			break;
		}
		/*
		 * Under gGPULock because annibale_gpu_worked() calls gpu_times(), which reads the
		 * scratch page and advances the same counters the submit path advances -- and
		 * submit() holds this lock while it does exactly that. The output goes back
		 * through the common copy at the end of this method, so a caller that passes a
		 * struct of this size in gets it filled on the way out; one that passes fewer
		 * bytes gets as many as it asked for, and the rest of the numbers are lost with
		 * the reset, which is the one way to misuse this request.
		 */
		gpuLockIoctl(kAnnibaleGPUWorked);
		annibale_gpu_worked(&r.worked.microseconds, &r.worked.submissions, r.worked.by);
		IOLockUnlock(gGPULock);
		break;
	case kAnnibaleSurface:
		ret = surfaceBuffer(&r.surface, &io);
		break;
	case kAnnibaleMemory:
		ret = memoryBuffer(&r.memory, &io);
		break;
	case kAnnibaleShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("AnnibaleGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (gGPUIoLog && (selector == kMsmGemNew || selector == kMsmGemInfo || selector == kMsmGemSubmit ||
	    selector == kAnnibaleSurface || selector == kAnnibaleMemory)) {
		ioLine(selector, &io, ioBegan);
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}

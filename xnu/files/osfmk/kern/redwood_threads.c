/*
 * One thread dump of a task, for kern.redwood.thread_dump (bsd/dev/redwood_threads.c).
 *
 * The compositor stops making frames for seconds at a time, and the display side can see that it
 * stopped but not which thread held the others. This prints one line per thread of the task: the
 * thread id, its state, the event it waits on, its scheduled priority, and the user pc its saved
 * state holds; then, per thread, the return addresses of its user stack ("bt" lines).
 *
 * The user side calls this from a watcher thread of its own (see redwoodmetal_commandbuffer.inc)
 * while a commit is inside one long call, so the dump is taken during the stall. That is also why
 * the thread fields are read without the thread lock: a dump that waits for the thread's lock
 * arrives after the stall ends, which is the one moment it is worthless. The values can therefore
 * be a little stale, and the saved pc points into a kernel stack that may be moving; both are
 * enough to tell "every thread waits on the same event" from "they wait on different ones".
 *
 * This half is osfmk's because `struct task`, `task->threads` and task_lock sit behind
 * MACH_KERNEL_PRIVATE (osfmk/kern/task.h), which only an osfmk build sees. It takes no sysctl:
 * the BSD half finds the process by pid, reads the image base the pcs below are offsets from, and
 * calls here, so neither side includes the other's headers.
 *
 * The user stacks are walked after the task lock is let go of, through the task's map
 * (vm_map_read_user), because the reads may fault pages in. A thread can move between the
 * collection of its registers and the walk; for a thread waiting in the kernel, which is the case
 * this is for (a main thread asleep in mach_msg inside some run loop), the frames do not change.
 *
 * Nothing is enabled unless asked: no boot argument, no timer, no work while idle.
 */

#include <mach/mach_types.h>
#include <kern/kern_types.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/queue.h>
#include <mach/arm/thread_status.h>
#include <kern/backtrace.h>
#include <vm/vm_map_xnu.h>
#include <sys/errno.h>

#define REDWOOD_TD_THREADS      48
#define REDWOOD_TD_FRAMES       32

extern void redwood_tl(const char *format, ...) __printflike(1, 2);

void redwood_thread_dump(task_t task, int pid, uint64_t exec_base, const char *exec_image);

static errno_t
redwood_td_copy(void *map, void *dst, user_addr_t src, size_t size)
{
	return vm_map_read_user((vm_map_t)map, src, dst, size) == KERN_SUCCESS ? 0 : EFAULT;
}

/*
 * One "bt" line per thread with up to REDWOOD_TD_LINE_FRAMES return addresses, innermost first, and
 * "bt+" lines after it with the rest, up to REDWOOD_TD_FRAMES: redwood_tl() keeps 191 characters
 * of a line, and a single line used to end after 16 or 17 frames.
 */
#define REDWOOD_TD_LINE_FRAMES  12

static void
redwood_td_backtrace(int pid, vm_map_t map, thread_t thread)
{
	uintptr_t frames[REDWOOD_TD_FRAMES];
	struct backtrace_control ctl = {
		.btc_user_thread = thread,
		.btc_user_copy = redwood_td_copy,
		.btc_user_copy_context = map,
	};
	struct backtrace_user_info info = BTUINFO_INIT;
	unsigned int count = backtrace_user(frames, REDWOOD_TD_FRAMES, &ctl, &info);
	unsigned int at = 0;

	do {
		char line[REDWOOD_TD_LINE_FRAMES * 12 + 64];
		int n = snprintf(line, sizeof(line), "tl: td %d tid %llu bt%s", pid,
		    (unsigned long long)thread_tid(thread), at == 0 ? "" : "+");

		for (unsigned int i = 0; i < REDWOOD_TD_LINE_FRAMES && at < count && n > 0 && (size_t)n < sizeof(line); i++, at++) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %lx", (unsigned long)frames[at]);
		}
		redwood_tl("%s%s\n", line, at >= count && (info.btui_info & BTI_TRUNCATED) ? " +" : "");
	} while (at < count);
}

void
redwood_thread_dump(task_t task, int pid, uint64_t exec_base, const char *exec_image)
{
	thread_t thread, held[REDWOOD_TD_THREADS];
	unsigned printed = 0, kept = 0;
	vm_map_t map;

	task_lock(task);
	redwood_tl("tl: td pid %d threads %d\n", pid, task->thread_count);
	/* The pcs below live in the shared cache, which is slid per boot, so a pc alone does not say
	 * which image it belongs to. The task carries the slide; printing it turns each pc into an
	 * offset into a known cache image without another source of truth. 0xffffffff means the
	 * field was never set for this task, and then the pcs stay unreadable as before. */
	redwood_tl("tl: td %d slide %#x\n", pid, task->task_shared_region_slide);
	/* The slide does not cover the images dyld mapped per process, so a pc above the cache still
	 * has no name. Both values come from the BSD half, which walked the map for the first
	 * vnode-backed executable region before this dump was asked for; a base of 0 means the walk
	 * found none and the low pcs stay unnamed. */
	redwood_tl("tl: td %d exec %#llx %s\n", pid,
	    (unsigned long long)exec_base, exec_image);
	queue_iterate(&task->threads, thread, thread_t, task_threads) {
		uint64_t pc = 0;

		if (thread->machine.upcb != NULL) {
			pc = get_saved_state_pc(thread->machine.upcb);
		}
		redwood_tl("tl: td %d tid %llu state %d wait %p pri %d pc %#llx\n", pid,
		    (unsigned long long)thread_tid(thread), thread->state,
		    (void *)(uintptr_t)thread->wait_event, (int)thread->sched_pri, pc);
		printed++;
		if (kept < REDWOOD_TD_THREADS && thread->machine.upcb != NULL) {
			thread_reference(thread);
			held[kept++] = thread;
		}
	}
	map = task->map;
	vm_map_reference(map);
	task_unlock(task);

	for (unsigned i = 0; i < kept; i++) {
		redwood_td_backtrace(pid, map, held[i]);
		thread_deallocate(held[i]);
	}
	vm_map_deallocate(map);

	redwood_tl("tl: td pid %d printed %u\n", pid, printed);
}

/*
 * kern.redwood.thread_dump: what every thread of one process is waiting on, on demand.
 *
 * Write a pid; one line per thread of that process goes into the loader's log (the format and the
 * reading of it are in osfmk/kern/redwood_threads.c, which does the walk). This half is the sysctl
 * front: it finds the process by pid and hands its task over.
 *
 * The two halves are split because neither build can include the other's headers. `struct task`,
 * `task->threads` and task_lock sit behind MACH_KERNEL_PRIVATE in osfmk/kern/task.h, which a BSD
 * file does not see; the sysctl plumbing is in bsd/sys/sysctl.h, whose types collide with osfmk's
 * when both are included. Only task_t and plain values cross the boundary, and task_t stays
 * incomplete on both sides.
 *
 * This half also answers "which image is this user address in", for two readers: the exec base of
 * the dump above, and the bad-access pc printed from osfmk/arm64/sleh.c. A pc on its own says
 * nothing because the shared cache is slid per boot and the images dyld mapped per process sit
 * above it, so both readers get the map entry from the vm layer and the file name from the VFS
 * namecache. The name is a basename; the entry bounds printed next to it give the offset when no
 * name is available.
 *
 * kern.redwood.cmd is the other way in: the last command the host sent through the loader with
 * "oem sh TEXT" while this kernel runs, read by /redwood/bin/rwsh, which runs it as root and so may
 * also write thread_dump for a process that cannot do it for itself.
 *
 * Nothing is enabled unless asked: no boot argument, no timer, no work while idle.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <mach/mach_types.h>
#include <mach/vm_prot.h>
#include <kern/task.h>
#include <sys/bsdtask_info.h>
#include <kern/kalloc.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>

SYSCTL_DECL(_kern_redwood);

/* Regions probed while looking for the first executable one. A task maps a handful of files and
 * the main binary's __TEXT is executable, so this only has to outlast the read-only mappings that
 * come before it; the bound keeps a malformed entry from turning the walk into a spin. */
#define REDWOOD_EXEC_IMAGE_PROBES 64

void redwood_thread_dump(task_t task, int pid, uint64_t exec_base, const char *exec_image);
int redwood_task_region_image(task_t task, uint64_t addr, char *name, uint32_t namelen,
    uint64_t *start, uint64_t *end);
uint64_t redwood_task_exec_image(task_t task, char *name, uint32_t namelen);
uint64_t loader_live_command(char *text, size_t size);         /* osfmk/arm64/loader_return.c */
uint64_t loader_live_inbox(const char **data, uint64_t *len);  /* the same */
int psci_smp_report(char *buf, size_t size);                  /* osfmk/arm64/psci_smp.c */
int psci_smp_prof_report(char *buf, size_t size);             /* the same */
void gic_v3_prof_control(bool on, int only_pid);              /* osfmk/arm64/gic_v3_irq.c */

/*
 * fill_procregioninfo* hands the vnode over as a bare address because it cannot hold a reference
 * across the copyout it was written for, so the vid is what makes taking one here safe: the vnode
 * may have been recycled between the map walk and this call. Both helpers leave `name` non-empty,
 * "-" when the region has no vnode or the namecache has no name for it, so the printed line keeps
 * the same number of fields either way.
 */
static bool
redwood_vnode_name(uintptr_t vnodeaddr, uint32_t vid, char *name, uint32_t namelen)
{
	vnode_t vp = (vnode_t)(uintptr_t)vnodeaddr;
	const char *cached;
	bool have = false;

	strlcpy(name, "-", namelen);
	if (vnodeaddr == 0) {
		return false;
	}
	if (vnode_getwithvid(vp, vid) != 0) {
		return false;
	}
	cached = vnode_getname(vp);
	if (cached != NULL) {
		strlcpy(name, cached, namelen);
		vnode_putname(cached);
		have = true;
	}
	vnode_put(vp);
	return have;
}

/*
 * The image backing `addr`, or 0 when no map entry covers it. fill_procregioninfo reports the next
 * region when the address falls in a hole, which answers "what is mapped above this" and not "what
 * is this address in", so the containment check is done here rather than trusted to the callee.
 */
int
redwood_task_region_image(task_t task, uint64_t addr, char *name, uint32_t namelen,
    uint64_t *start, uint64_t *end)
{
	struct proc_regioninfo_internal info = {};
	uintptr_t vnodeaddr = 0;
	uint32_t vid = 0;

	if (fill_procregioninfo(task, addr, &info, &vnodeaddr, &vid) == 0) {
		strlcpy(name, "-", namelen);
		return 0;
	}
	if (addr < info.pri_address || addr >= info.pri_address + info.pri_size) {
		strlcpy(name, "-", namelen);
		return 0;
	}

	if (start != NULL) {
		*start = info.pri_address;
	}
	if (end != NULL) {
		*end = info.pri_address + info.pri_size;
	}
	redwood_vnode_name(vnodeaddr, vid, name, namelen);
	return 1;
}

/*
 * Start of the first vnode-backed region of the task whose permissions include execute, which on
 * iOS is the main binary's __TEXT, and its file name. 0 when the task maps no such region.
 */
uint64_t
redwood_task_exec_image(task_t task, char *name, uint32_t namelen)
{
	uint64_t addr = 0;
	int probes;

	strlcpy(name, "-", namelen);
	for (probes = 0; probes < REDWOOD_EXEC_IMAGE_PROBES; probes++) {
		struct proc_regioninfo_internal info = {};
		uintptr_t vnodeaddr = 0;
		uint32_t vid = 0;
		uint64_t next;

		if (fill_procregioninfo_onlymappedvnodes(task, addr, &info, &vnodeaddr, &vid) == 0) {
			return 0;
		}
		next = info.pri_address + info.pri_size;
		if (info.pri_protection & VM_PROT_EXECUTE) {
			redwood_vnode_name(vnodeaddr, vid, name, namelen);
			return info.pri_address;
		}
		if (next <= addr) {
			return 0;
		}
		addr = next;
	}
	return 0;
}

static int
redwood_thread_dump_pid(int pid)
{
	proc_t p = proc_find(pid);

	if (p == NULL) {
		return ESRCH;
	}

	task_t task = proc_task(p);
	uint64_t exec_base;
	char exec_image[64];

	/* Read before the dump, not inside it: fill_procregioninfo* takes task_lock itself, and the
	 * osfmk half holds it across the thread walk. */
	exec_base = redwood_task_exec_image(task, exec_image, sizeof(exec_image));
	redwood_thread_dump(task, pid, exec_base, exec_image);
	proc_rele(p);
	return 0;
}

static int
redwood_thread_dump_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int pid = 0;
	int error;

	error = sysctl_io_number(req, 0, sizeof(pid), &pid, NULL);
	if (error != 0 || req->newptr == NULL) {
		return error;
	}
	if (pid <= 0) {
		return EINVAL;
	}
	return redwood_thread_dump_pid(pid);
}

/*
 * With boot-arg touch_lat=1, on every tick of the processor governor (annibale_cpufreq.c, 50 ms), the
 * threads of backboardd, SpringBoard and the input service go into the log as thread_dump writes
 * them in two cases:
 *   - a gesture with no answer: a touch began (the first after GESTURE_GAP_MS without one), and
 *     QUIET_MS later the panel has had fewer than QUIET_FRAMES new frames. In kernel-12377-cmd40-bo16
 *     and cmd41-bo17 the swipes that moved the screen had 9 to 26 frames by then, the others 0 to 4
 *     (the clock icon's second hand draws at 10 Hz); in bo16 neither backboardd nor SpringBoard
 *     submitted anything after those until the next gesture, about four seconds later;
 *   - an animation that stops while the screen is being touched: the panel's frame count had been
 *     moving since the gesture began (two changes at most STALL_FLOWING_MS apart, the last after the
 *     gesture's first touch), has not moved for STALL_MS since, and the last finger sample is under
 *     STALL_TOUCH_MS old, so the finger is still on the glass.
 * Each case dumps once per gesture or stall, and at most STALL_DUMPS times a boot. Only finger
 * samples count as touches (redwood_touch_last). In kernel-12377-cmd43-bo20, with button presses
 * counted and a stall allowed up to a second after the last touch, the six home presses on the
 * first page, which draw nothing, used most of the first case's dumps, and all eight of the second
 * case's came 445 to 954 ms after the finger had lifted, once a page swipe had settled.
 */
#define GESTURE_GAP_MS          300
#define QUIET_MS                400
#define QUIET_FRAMES            8
#define STALL_MS                300
#define STALL_FLOWING_MS        100
#define STALL_TOUCH_MS          100
#define STALL_DUMPS             8

static void
freeze_dump(void)
{
	static const char *const names[] = { "backboardd", "SpringBoard", "input26-service" };

	for (int pid = 1; pid < 2000; pid++) {
		char name[32] = "";

		proc_name(pid, name, sizeof(name));
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			if (strcmp(name, names[i]) == 0) {
				printf("freeze: %s pid %d\n", name, pid);
				redwood_thread_dump_pid(pid);
			}
		}
	}
}

void
redwood_freeze_check(uint64_t touched)
{
	static int enabled = -1;
	static uint32_t frames, dumps, began_frames, quiet_dumps;
	static uint64_t moved, moved_before, touch_seen, began;
	static bool dumped, began_dumped;
	uint64_t now = mach_absolute_time(), touch_ns, still_ns, flowing_ns;

	if (enabled < 0) {
		uint32_t value = 0;

		enabled = PE_parse_boot_argn("touch_lat", &value, sizeof(value)) && value != 0;
	}
	if (!enabled || touched > now) {
		return;
	}
	if (touched != touch_seen) {
		absolutetime_to_nanoseconds(touched - touch_seen, &touch_ns);
		if (touch_seen == 0 || touch_ns > GESTURE_GAP_MS * NSEC_PER_MSEC) {
			began = touched;
			began_frames = redwood_frames_shown;
			began_dumped = false;
		}
		touch_seen = touched;
	}
	if (began != 0 && !began_dumped && quiet_dumps < STALL_DUMPS && redwood_frames_shown - began_frames < QUIET_FRAMES) {
		absolutetime_to_nanoseconds(now - began, &touch_ns);
		if (touch_ns >= QUIET_MS * NSEC_PER_MSEC && touch_ns < NSEC_PER_SEC) {
			began_dumped = true;
			quiet_dumps++;
			printf("freeze: %u frames %llu ms after a touch began (frame %u), %u touch samples unread; the "
			    "threads of backboardd, SpringBoard and input26-service follow\n", redwood_frames_shown - began_frames,
			    touch_ns / NSEC_PER_MSEC, began_frames, redwood_touch_unread());
			freeze_dump();
		}
	}
	if (redwood_frames_shown != frames || moved == 0) {
		frames = redwood_frames_shown;
		moved_before = moved;
		moved = now;
		dumped = false;
		return;
	}
	if (dumped || dumps >= STALL_DUMPS || began == 0 || moved_before == 0 || moved < began) {
		return;         /* (a screen that stopped before this gesture began is the other case's) */
	}
	absolutetime_to_nanoseconds(now - touched, &touch_ns);
	absolutetime_to_nanoseconds(now - moved, &still_ns);
	absolutetime_to_nanoseconds(moved - moved_before, &flowing_ns);
	if (touch_ns > STALL_TOUCH_MS * NSEC_PER_MSEC || still_ns < STALL_MS * NSEC_PER_MSEC ||
	    flowing_ns > STALL_FLOWING_MS * NSEC_PER_MSEC) {
		return;
	}
	dumped = true;
	dumps++;
	printf("freeze: no frame for %llu ms after frames %llu ms apart, touched %llu ms ago (frame %u); "
	    "the threads of backboardd, SpringBoard and input26-service follow\n", still_ns / NSEC_PER_MSEC, flowing_ns / NSEC_PER_MSEC,
	    touch_ns / NSEC_PER_MSEC, frames);
	freeze_dump();
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, thread_dump, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, redwood_thread_dump_sysctl, "I",
    "write a pid: one line per thread of it (id, state, wait event, priority, user pc)");

/* "SEQ TEXT": SEQ is 0 and TEXT empty until the host has sent a command. */
static int
redwood_cmd_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	char text[128], line[160];
	uint64_t seq = loader_live_command(text, sizeof(text));
	int n = snprintf(line, sizeof(line), "%llu %s", (unsigned long long)seq, text);

	return SYSCTL_OUT(req, line, (size_t)n + 1);
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, cmd, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, redwood_cmd_sysctl, "A",
    "the last command the host sent through the loader (oem sh), as SEQ TEXT");

/* kern.redwood.inbox: the last file the host pushed through the loader ("oem push"), preceded by
 * its sequence number (8 bytes, 0 before the first push); rwsh "pull" writes it to a file. */
static int
redwood_inbox_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	const char *data;
	uint64_t len, seq = loader_live_inbox(&data, &len);
	int error = SYSCTL_OUT(req, &seq, sizeof(seq));

	return error != 0 ? error : SYSCTL_OUT(req, data, (size_t)len);
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, inbox, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, redwood_inbox_sysctl, "S",
    "the last file the host pushed through the loader (oem push): an 8-byte sequence number, then the file");

/*
 * kern.redwood.outbox: a file for the host, handed over the way a screen recording is. Each write is
 * a header { total bytes, offset } and then that piece of the file. The piece at offset 0 takes a
 * buffer of the total size in one physical piece, and the piece that completes the file publishes
 * it in loader_recording: after "oem stop" the loader stages it ("oem recording") and the host
 * reads it with "fastboot get_staged" (tools/k90auto/run-auto.py). This replaces a screen
 * recording of the same run. Reading answers { total bytes, bytes written so far }.
 *
 * The loaded file system has no other way off the phone faster than the log (rwsh "cat", 48 bytes a
 * line). The buffer is allocated on the first write, which finds no 328 MB in one piece once iOS
 * runs (kernel-12377-cmd29-seed2); boot-arg outbox_mb=N takes N MB at boot instead, while memory
 * is still in large pieces (redwood_outbox_init, called with the other drivers' inits).
 */
extern uint64_t loader_recording[3];    /* osfmk/arm64/loader_return.c */
extern uint64_t kvtophys(vm_offset_t va);
static LCK_GRP_DECLARE(redwood_outbox_grp, "redwood-outbox");
static LCK_MTX_DECLARE(redwood_outbox_lock, &redwood_outbox_grp);
static vm_offset_t redwood_outbox;
static uint64_t redwood_outbox_size, redwood_outbox_total, redwood_outbox_filled;

#define REDWOOD_OUTBOX_MAX (1ULL << 30)  /* what the loader takes as a recording */

void
redwood_outbox_init(void)
{
	uint32_t mb = 0;

	if (!PE_parse_boot_argn("outbox_mb", &mb, sizeof(mb)) || mb == 0) {
		return;
	}
	redwood_outbox_size = (uint64_t)MIN(mb, (uint32_t)(REDWOOD_OUTBOX_MAX >> 20)) << 20;
	if (kmem_alloc_contig(kernel_map, &redwood_outbox, redwood_outbox_size, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		redwood_outbox = 0;
		redwood_outbox_size = 0;
	}
	printf("outbox: %llu MB in one piece taken at boot%s\n", (uint64_t)mb,
	    redwood_outbox != 0 ? "" : ": none to be had");
}

static int
redwood_outbox_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	uint64_t head[2], piece;
	int error = 0;

	lck_mtx_lock(&redwood_outbox_lock);
	if (req->newptr == USER_ADDR_NULL) {
		head[0] = redwood_outbox_total;
		head[1] = redwood_outbox_filled;
		goto out;
	}
	if (req->newlen < sizeof(head) || (error = SYSCTL_IN(req, head, sizeof(head))) != 0) {
		error = error != 0 ? error : EINVAL;
		goto out;
	}
	piece = req->newlen - sizeof(head);
	if (head[0] == 0 || head[0] > REDWOOD_OUTBOX_MAX || head[1] > head[0] || piece > head[0] - head[1]) {
		error = EINVAL;
		goto out;
	}
	if (head[1] == 0) {
		loader_recording[0] = loader_recording[1] = loader_recording[2] = 0;
		if (redwood_outbox != 0 && redwood_outbox_size < round_page(head[0])) {
			kmem_free(kernel_map, redwood_outbox, redwood_outbox_size);
			redwood_outbox = 0;
		}
		if (redwood_outbox == 0 && kmem_alloc_contig(kernel_map, &redwood_outbox, round_page(head[0]), PAGE_MASK,
		    0, 0, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("outbox: no memory in one piece for %llu MB\n", head[0] >> 20);
			redwood_outbox = 0;
			error = ENOMEM;
			goto out;
		}
		redwood_outbox_size = MAX(redwood_outbox_size, round_page(head[0]));
		redwood_outbox_total = head[0];
		redwood_outbox_filled = 0;
	}
	/* pieces come in order: one out of order means the writer lost track, and the file has a hole */
	if (redwood_outbox == 0 || head[0] != redwood_outbox_total || head[1] != redwood_outbox_filled) {
		error = ESPIPE;
		goto out;
	}
	if (piece != 0 && (error = SYSCTL_IN(req, (void *)(redwood_outbox + head[1]), (size_t)piece)) != 0) {
		goto out;
	}
	redwood_outbox_filled += piece;
	if (redwood_outbox_filled == redwood_outbox_total) {
		loader_recording[0] = kvtophys(redwood_outbox);
		loader_recording[1] = redwood_outbox_total;
		loader_recording[2] = (uint64_t)redwood_outbox;
		printf("outbox: %llu bytes for the host at %#llx (oem recording after the run)\n",
		    redwood_outbox_total, loader_recording[0]);
	}
	head[0] = redwood_outbox_total;
	head[1] = redwood_outbox_filled;
out:
	lck_mtx_unlock(&redwood_outbox_lock);
	return error != 0 ? error : SYSCTL_OUT(req, head, sizeof(head));
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, outbox, CTLTYPE_OPAQUE | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, redwood_outbox_sysctl, "S",
    "a file for the host, staged by the loader after the run: write { total, offset } and a piece");

/*
 * kern.redwood.env: settings for the processes that load libredwoodmetal, written by rwsh
 * ("setenv", "addenv") while iOS runs. The words are PROCESS:NAME=VALUE or PROCESS:-NAME; the
 * driver applies its process's words when the process loads it, so a probe can be changed and the
 * process restarted ("restart NAME") without a new image. Empty until written; writing needs root.
 */
static char redwood_env[1024];
SYSCTL_STRING(_kern_redwood, OID_AUTO, env, CTLFLAG_RW | CTLFLAG_LOCKED, redwood_env,
    sizeof(redwood_env), "settings for libredwoodmetal, as PROCESS:NAME=VALUE words");

/* kern.redwood.cpus: psci_smp_report(), one line per processor. */
static int
redwood_cpus_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	char text[2048];
	int n = psci_smp_report(text, sizeof(text));

	return SYSCTL_OUT(req, text, (size_t)n + 1 <= sizeof(text) ? (size_t)n + 1 : sizeof(text));
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, cpus, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, redwood_cpus_sysctl, "A", "per processor: signal word, interrupt, IPI, timer and WFE counts");

/*
 * kern.redwood.prof_on: 1 clears the interrupt-sampled profile and starts it, 0 stops it; a pid
 * written to kern.redwood.prof_pid first keeps that process's samples only (0: every process).
 * kern.redwood.prof: the profile so far (psci_smp_prof_report): samples per pid, then the most
 * frequent (pid, pc, lr). Two reads a few seconds apart after a 1 say where the processors were.
 */
static int redwood_prof_on, redwood_prof_pid;

SYSCTL_INT(_kern_redwood, OID_AUTO, prof_pid, CTLFLAG_RW | CTLFLAG_LOCKED, &redwood_prof_pid, 0,
    "the process kern.redwood.prof_on samples, 0 for all");

static int
redwood_prof_on_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int value = redwood_prof_on, changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);

	if (error == 0 && changed) {
		redwood_prof_on = value != 0;
		gic_v3_prof_control(redwood_prof_on, redwood_prof_pid);
	}
	return error;
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, prof_on, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, redwood_prof_on_sysctl, "I", "1: clear the interrupt-sampled profile and start it, 0: stop");

static int
redwood_prof_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	const size_t size = 96 * 1024;  /* PROF_TOP lines of about 80 bytes */
	char *text = kalloc_data(size, Z_WAITOK);
	int n, error;

	if (text == NULL) {
		return ENOMEM;
	}
	n = psci_smp_prof_report(text, size);
	error = SYSCTL_OUT(req, text, (size_t)n + 1 <= size ? (size_t)n + 1 : size);
	kfree_data(text, size);
	return error;
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, prof, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, redwood_prof_sysctl, "A", "interrupt-sampled profile: samples per pid, then count pid u|k pc lr");

/*
 * kern.redwood.say: a line any process writes here goes into the kernel log as
 * "say PID NAME: TEXT". It is for libraries inserted into processes whose standard error goes
 * nowhere (an XPC service under xpcproxy, a sandboxed extension), which is where the loops that
 * keep the processors busy run. At most REDWOOD_SAY_PER_SECOND lines a second are printed and the
 * rest counted, so a library that says too much costs the log one line a second.
 */
#define REDWOOD_SAY_PER_SECOND  200

extern void redwood_tl(const char *format, ...) __printflike(1, 2);

static int
redwood_say_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	static uint64_t window;
	static uint32_t said, dropped;
	char text[256], name[32];
	size_t length = req->newlen < sizeof(text) - 1 ? req->newlen : sizeof(text) - 1;
	uint64_t now = mach_absolute_time(), second;
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	error = SYSCTL_IN(req, text, length);
	if (error != 0) {
		return error;
	}
	text[length] = '\0';
	while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\0')) {
		text[--length] = '\0';
	}
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	if (now - window >= second) {
		if (dropped != 0) {
			redwood_tl("say: %u lines over %u a second not printed\n", dropped, REDWOOD_SAY_PER_SECOND);
		}
		window = now;
		said = dropped = 0;
	}
	if (++said > REDWOOD_SAY_PER_SECOND) {
		dropped++;
		return 0;
	}
	proc_selfname(name, sizeof(name));
	redwood_tl("say %d %s: %s\n", proc_selfpid(), name, text);
	return 0;
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, say, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    NULL, 0, redwood_say_sysctl, "A", "a line for the kernel log, from any process");

extern void halt_all_cpus(boolean_t reboot) __attribute__((noreturn));

static int
redwood_halt_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);

	if (error == 0 && changed && value != 0) {
		redwood_tl("halt requested by pid %d\n", proc_selfpid());
		halt_all_cpus(FALSE);
	}
	return error;
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, halt, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    NULL, 0, redwood_halt_sysctl, "I", "1: blank the panel, save /private/var and return to the loader");

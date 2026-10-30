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

/*
 * kern.redwood.env: settings for the processes that load libredwoodmetal, written by rwsh
 * ("setenv", "addenv") while iOS runs. The words are PROCESS:NAME=VALUE or PROCESS:-NAME; the
 * driver applies its process's words when the process loads it, so a probe can be changed and the
 * process restarted ("restart NAME") without a new image. Empty until written; writing needs root.
 */
static char redwood_env[1024];
SYSCTL_STRING(_kern_redwood, OID_AUTO, env, CTLFLAG_RW | CTLFLAG_LOCKED, redwood_env,
    sizeof(redwood_env), "settings for libredwoodmetal, as PROCESS:NAME=VALUE words");

/*
 * Character device /dev/annibale-iris for SM8750 Iris VPU v3.5.
 * Registration and open() perform zero hardware or firmware accesses; hardware
 * bring-up runs lazily on ioctl under iris_lock.
 */
#ifndef ANNIBALE_IRIS_HOST_TEST
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#endif

#include "annibale_iris_private.h"

struct iris_controller g_iris;

#ifndef ANNIBALE_IRIS_HOST_TEST
static LCK_GRP_DECLARE(iris_lck_grp, "annibale-iris");
static LCK_MTX_DECLARE(iris_lock, &iris_lck_grp);

static void
iris_lock_acquire(void)
{
	lck_mtx_lock(&iris_lock);
}

static void
iris_lock_release(void)
{
	lck_mtx_unlock(&iris_lock);
}

static int
iris_caller_pid(struct proc *p)
{
	return p != NULL ? proc_pid(p) : 0;
}
#endif /* !ANNIBALE_IRIS_HOST_TEST */

static int
iris_dev_open(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	return 0;
}

static int
iris_dev_close(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p)
{
	int pid = iris_caller_pid(p);

	iris_lock_acquire();
	for (uint32_t i = 0; i < IRIS_MAX_SESSIONS; i++) {
		struct iris_session *s = &g_iris.sessions[i];
		if (s->active && (pid == 0 || s->owner_pid == pid)) {
			iris_hfi_session_close(s);
			memset(s, 0, sizeof(*s));
		}
	}
	iris_lock_release();
	return 0;
}

static int
iris_ioctl_get_info(struct iris_device_info *info)
{
	struct iris_device_info out;

	memset(&out, 0, sizeof(out));
	out.version = IRIS_ABI_VERSION;
	out.flags = IRIS_INFO_DEV_PRESENT;
	out.max_width = IRIS_MAX_WIDTH;
	out.max_height = IRIS_MAX_HEIGHT;

	if (iris_hw_probe_firmware()) {
		out.flags |= IRIS_INFO_FW_PRESENT;
		out.fw_size = g_iris.fw_size;
	}
	if (iris_hw_ensure_ready()) {
		out.flags |= IRIS_INFO_HW_READY;
		out.codecs_mask = IRIS_CODEC_MASK_H264 | IRIS_CODEC_MASK_HEVC;
	}
	*info = out;
	return 0;
}

static int
iris_ioctl_open_session(struct iris_session_params *params, int pid)
{
	if ((params->codec_type != IRIS_CODEC_H264 && params->codec_type != IRIS_CODEC_HEVC) ||
	    params->width < IRIS_MIN_DIM || params->width > IRIS_MAX_WIDTH ||
	    params->height < IRIS_MIN_DIM || params->height > IRIS_MAX_HEIGHT) {
		return EINVAL;
	}
	if (!iris_hw_ensure_ready()) {
		return EOPNOTSUPP;
	}

	struct iris_session *slot = NULL;
	uint32_t sid = 0;
	for (uint32_t i = 0; i < IRIS_MAX_SESSIONS; i++) {
		if (!g_iris.sessions[i].active) {
			slot = &g_iris.sessions[i];
			sid = i + 1u;
			break;
		}
	}
	if (slot == NULL) {
		return EBUSY;
	}

	memset(slot, 0, sizeof(*slot));
	slot->session_id = sid;
	slot->codec_type = params->codec_type;
	slot->width = params->width;
	slot->height = params->height;
	slot->owner_pid = pid;

	int err = iris_hfi_session_open(slot);
	if (err != 0) {
		memset(slot, 0, sizeof(*slot));
		return err;
	}
	slot->active = true;
	params->session_id = sid;
	return 0;
}

static struct iris_session *
iris_find_session(uint32_t session_id)
{
	if (session_id == 0 || session_id > IRIS_MAX_SESSIONS) {
		return NULL;
	}
	struct iris_session *s = &g_iris.sessions[session_id - 1u];
	return s->active ? s : NULL;
}

static int
iris_dev_ioctl(dev_t dev __unused, u_long cmd, caddr_t data, int flags __unused, struct proc *p)
{
	if (data == NULL) {
		switch (cmd) {
		case IRIS_IOC_GET_INFO:
		case IRIS_IOC_OPEN_SESSION:
		case IRIS_IOC_DECODE_FRAME:
		case IRIS_IOC_FLUSH_SESSION:
		case IRIS_IOC_CLOSE_SESSION:
			return EINVAL;
		default:
			return ENOTTY;
		}
	}

	int err = 0;
	iris_lock_acquire();
	switch (cmd) {
	case IRIS_IOC_GET_INFO:
		err = iris_ioctl_get_info((struct iris_device_info *)data);
		break;
	case IRIS_IOC_OPEN_SESSION:
		err = iris_ioctl_open_session((struct iris_session_params *)data, iris_caller_pid(p));
		break;
	case IRIS_IOC_DECODE_FRAME: {
		struct iris_decode_frame *req = (struct iris_decode_frame *)data;
		struct iris_session *s = iris_find_session(req->session_id);
		err = (s != NULL) ? iris_hfi_decode_frame(s, req) : EINVAL;
		break;
	}
	case IRIS_IOC_FLUSH_SESSION: {
		uint32_t sid = *(const uint32_t *)data;
		struct iris_session *s = iris_find_session(sid);
		err = (s != NULL) ? iris_hfi_session_flush(s) : EINVAL;
		break;
	}
	case IRIS_IOC_CLOSE_SESSION: {
		uint32_t sid = *(const uint32_t *)data;
		struct iris_session *s = iris_find_session(sid);
		if (s == NULL) {
			err = EINVAL;
		} else {
			iris_hfi_session_close(s);
			memset(s, 0, sizeof(*s));
		}
		break;
	}
	default:
		err = ENOTTY;
		break;
	}
	iris_lock_release();
	return err;
}

static const struct cdevsw iris_cdevsw = {
	.d_open = iris_dev_open,
	.d_close = iris_dev_close,
	.d_read = eno_rdwrt,
	.d_write = eno_rdwrt,
	.d_ioctl = iris_dev_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0
};

void
annibale_iris_init(void)
{
	static bool initialized = false;
	if (initialized) {
		return;
	}
	initialized = true;

	iris_say("registering SM8750 Iris VPU v3.5 (/dev/annibale-iris)...\n");

	int major = cdevsw_add(-1, &iris_cdevsw);
	if (major >= 0) {
		if (devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "annibale-iris") != NULL) {
			iris_say("/dev/annibale-iris created successfully\n");
		} else {
			iris_say("warning: failed to create devfs node /dev/annibale-iris\n");
		}
	} else {
		iris_say("warning: failed to allocate cdevsw major for iris\n");
	}
}

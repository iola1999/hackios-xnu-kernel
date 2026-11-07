/*
 * Iris device ABI for REDMI K90 (annibale / SM8750).
 * HFI session handling and the native VideoToolbox decoder are pending.
 * Registration only publishes the device node; open does not access hardware.
 */

#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/proc.h>

#define say(...) printf("iris: " __VA_ARGS__)

/* IOCTL interface for /dev/annibale-iris */
struct iris_session_params {
	uint32_t codec_type;    /* 1: H.264 (avc1), 2: HEVC (hvc1), 3: VP9 (vp09) */
	uint32_t width;
	uint32_t height;
	uint32_t session_id;    /* Out */
};

struct iris_decode_frame {
	uint32_t session_id;
	uint64_t input_user_addr;
	uint32_t input_len;
	uint64_t out_user_y;    /* In: 用户态 CVPixelBuffer 平面 0 (Y) 目标地址 */
	uint64_t out_user_uv;   /* In: 用户态 CVPixelBuffer 平面 1 (UV) 目标地址 */
	uint64_t out_phys_y;    /* Out: 硬件解码平面 0 物理基址 */
	uint64_t out_phys_uv;   /* Out: 硬件解码平面 1 物理基址 */
	uint32_t out_stride;    /* In/Out: 跨距 (对齐至 128 字节) */
	uint32_t flags;         /* Out: 状态标志位 (0x01: 成功) */
};

#define IRIS_IOC_OPEN_SESSION   _IOWR('V', 1, struct iris_session_params)
#define IRIS_IOC_DECODE_FRAME   _IOWR('V', 2, struct iris_decode_frame)
#define IRIS_IOC_CLOSE_SESSION  _IOW('V', 3, uint32_t)

/* Character device callbacks */

static int
iris_dev_open(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	return EOPNOTSUPP;
}

static int
iris_dev_close(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	return 0;
}

static int
iris_dev_ioctl(dev_t dev __unused, u_long cmd, caddr_t data __unused, int flags __unused, struct proc *p __unused)
{
	int error = 0;

	switch (cmd) {
	case IRIS_IOC_OPEN_SESSION:
	case IRIS_IOC_DECODE_FRAME:
	case IRIS_IOC_CLOSE_SESSION:
		/* HFI gen2 session and completion packets are not implemented yet. */
		say("Iris HFI session ioctl %#lx is unavailable until native packet handling is implemented\n", cmd);
		error = EOPNOTSUPP;
		break;
	default:
		error = ENOTTY;
		break;
	}
	return error;
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

	say("registering SM8750 Iris VPU v3.5 (/dev/annibale-iris)...\n");

	int major = cdevsw_add(-1, &iris_cdevsw);
	if (major >= 0) {
		if (devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "annibale-iris") != NULL) {
			say("/dev/annibale-iris created successfully\n");
		} else {
			say("warning: failed to create devfs node /dev/annibale-iris\n");
		}
	} else {
		say("warning: failed to allocate cdevsw major for iris\n");
	}
}

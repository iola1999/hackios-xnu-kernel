/*
 * Public ioctl ABI for SM8750 Iris VPU v3.5 (/dev/annibale-iris).
 */
#ifndef _SYS_ANNIBALE_IRIS_H_
#define _SYS_ANNIBALE_IRIS_H_

#include <stdint.h>
#include <sys/ioccom.h>

#define IRIS_ABI_VERSION        1u

#define IRIS_CODEC_H264         1u      /* 'avc1' */
#define IRIS_CODEC_HEVC         2u      /* 'hvc1' / 'hev1' */

#define IRIS_CODEC_MASK_H264    (1u << IRIS_CODEC_H264)
#define IRIS_CODEC_MASK_HEVC    (1u << IRIS_CODEC_HEVC)

#define IRIS_INFO_DEV_PRESENT   0x01u
#define IRIS_INFO_FW_PRESENT    0x02u
#define IRIS_INFO_HW_READY      0x04u

#define IRIS_FRAME_DECODED      0x01u
#define IRIS_FRAME_NEED_MORE    0x04u
#define IRIS_FRAME_DRAIN        0x08u
#define IRIS_FRAME_POLL         0x10u

struct iris_device_info {
	uint32_t version;
	uint32_t flags;
	uint32_t codecs_mask;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t fw_size;
};

struct iris_session_params {
	uint32_t codec_type;    /* IRIS_CODEC_* */
	uint32_t width;
	uint32_t height;
	uint32_t session_id;    /* Out: 1..IRIS_MAX_SESSIONS */
};

struct iris_decode_frame {
	uint32_t session_id;
	uint32_t input_len;
	uint64_t input_user_addr;
	uint64_t in_tag;        /* In: opaque caller frame tag echoed into HFI timestamp */
	uint64_t out_tag;       /* Out: caller frame tag from emitted HFI_BUFFER_RAW */
	uint64_t out_user_y;    /* Userland CVPixelBuffer plane 0 (Y) */
	uint64_t out_user_uv;   /* Userland CVPixelBuffer plane 1 (UV) */
	uint32_t out_stride;    /* In: userland destination bytesPerRow; Out: active row stride */
	uint32_t flags;         /* In: IRIS_FRAME_DRAIN / POLL; Out: IRIS_FRAME_DECODED / NEED_MORE */
};

#define IRIS_IOC_OPEN_SESSION   _IOWR('V', 1, struct iris_session_params)
#define IRIS_IOC_DECODE_FRAME   _IOWR('V', 2, struct iris_decode_frame)
#define IRIS_IOC_CLOSE_SESSION  _IOW('V', 3, uint32_t)
#define IRIS_IOC_GET_INFO       _IOR('V', 4, struct iris_device_info)
#define IRIS_IOC_FLUSH_SESSION  _IOW('V', 5, uint32_t)

#ifdef KERNEL
void annibale_iris_init(void);
#endif

#endif /* _SYS_ANNIBALE_IRIS_H_ */

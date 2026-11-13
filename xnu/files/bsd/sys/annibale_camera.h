/*
 * /dev/annibale-camera accepts annibale_camera_command followed by cmd.bytes.
 * ANNIBALE_CAMERA_PROFILE installs one checked native sensor mode. CAMIOC_CAPTURE
 * acquires one frame and returns its completion/stop status. read() copies a frame
 * header and up to stride * height bytes only after that capture has stopped.
 * A header-sized read returns geometry without copying pixels. Unknown native
 * exposure/gain/black-level fields are zero. Native capture failures retain any
 * resources whose DMA ownership has not ended; close() cannot release them.
 * cam_hw=0 selects the existing software diagnostic path without a profile.
 */
#ifndef _SYS_ANNIBALE_CAMERA_H_
#define _SYS_ANNIBALE_CAMERA_H_

#include <stdint.h>
#include <sys/ioccom.h>

#define ANNIBALE_CAMERA_DEVICE           "/dev/annibale-camera"
#define ANNIBALE_CAMERA_MAGIC            "RWFRAME1"

struct annibale_camera_frame {
	char            magic[8];       /* ANNIBALE_CAMERA_MAGIC */
	uint32_t        header_bytes;   /* this structure's size: the frame's bytes follow */
	uint32_t        width, height;  /* pixels */
	uint32_t        stride;         /* bytes from one line to the next */
	uint32_t        format;         /* 1: MIPI RAW10, four pixels in five bytes */
	uint32_t        bayer;          /* the order of a 2x2 cell: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        black;          /* the black level, in 10-bit steps */
	uint64_t        sequence;       /* the frame's number since the sensor started, from 1 */
	uint64_t        time;           /* when the frame was done, in mach_absolute_time()'s units */
	uint32_t        exposure_lines; /* the exposure it was taken with, in lines ... */
	uint32_t        line_ns;        /* ... of this many nanoseconds */
	uint32_t        gain;           /* the sensor's analog gain register ... */
	uint32_t        gain_unity;     /* ... which is this for 1x */
	uint32_t        gain_max;       /* the most it may be given */
	uint32_t        frame_lines;    /* the frame's length in lines (frames a second: 1e9 / (frame_lines * line_ns)) */
	uint32_t        frame_lines_min; /* the mode's own length: the shortest frame */
	uint32_t        frame_lines_max; /* the longest frame it may be given */
	uint32_t        margin;         /* exposure_lines is at most frame_lines - margin */
	uint32_t        sensor;         /* which: 0 the macro, 1 the wide, 2 the front, 3 the ultra-wide camera */
	uint32_t        focus;          /* the lens's position as last set, 0 (the furthest) to 1023: ANNIBALE_CAMERA_NO_FOCUS where it
	                                 * has no motor (or it does not answer) */
	uint32_t        reserved[5];
};

#define ANNIBALE_CAMERA_NO_FOCUS 0xffffffffu

enum {
	ANNIBALE_CAMERA_EXPOSURE = 1,    /* a: lines, b: gain, c: frame lines (0: the shortest that holds the exposure) */
	ANNIBALE_CAMERA_KEEP = 2,        /* a picture for the host: a width, b height, c stride, d format (1 the frame's RAW10, 2 NV12,
	                                 * 3 BGRA); bytes follow */
	ANNIBALE_CAMERA_SENSOR = 3,      /* another sensor: a which (as in the frames' header), b its mode (~0: the usual one);
	                                 * the frames stop, everything goes down, and the next read starts the other one */
	ANNIBALE_CAMERA_FOCUS = 4,       /* the lens to a: 0 (the furthest) to 1023, from the next frame or so */
	ANNIBALE_CAMERA_PROFILE = 5,     /* bytes: one RWCTAB03 profile; a/b/c/d must be zero */
};

struct annibale_camera_command {
	uint32_t        command;
	uint32_t        bytes;          /* of what follows this structure */
	uint32_t        a, b, c, d;
};

#define ANNIBALE_CAM_INFO_FLAG_TPG_FALLBACK     (1u << 0)
#define ANNIBALE_CAM_INFO_FLAG_SMMU_BYPASS      (1u << 1)
#define ANNIBALE_CAM_INFO_FLAG_WIDE_DETECTED    (1u << 2)
#define ANNIBALE_CAM_INFO_FLAG_FRONT_DETECTED   (1u << 3)
#define ANNIBALE_CAM_INFO_FLAG_HW_DMA_VERIFIED  (1u << 4)
#define ANNIBALE_CAM_INFO_FLAG_NATIVE_PROFILE   (1u << 5)
#define ANNIBALE_CAM_INFO_FLAG_FRONT_MODE4      (1u << 6)

struct annibale_camera_info {
	uint32_t        flags;          /* ANNIBALE_CAM_INFO_FLAG_*; native completion is reported separately */
	uint16_t        wide_chip_id;   /* Rear Wide OVX8000 chip ID (0x5650) or 0 */
	uint16_t        front_chip_id;  /* Front OV20B40 chip ID (0x2042) or 0 */
	uint8_t         wide_slave;     /* 7-bit I2C slave on CCI0 M1 */
	uint8_t         front_slave;    /* 7-bit I2C slave on QUPv3 SE9 I2C */
	uint8_t         active_sensor;  /* 1 = Rear Wide, 2 = Front */
	uint8_t         smmu_slot;      /* apps_smmu SMR slot for SID 0x1c00 (0xff if unmapped) */
	uint32_t        csid_hw_ver;    /* CSID-lite0 HW_VERSION (0x0ad6d000) */
	uint32_t        ife_hw_ver;     /* IFE-lite0 HW_VERSION (0x0ad6f800) */
};

#define CAMIOC_G_INFO           _IOR('C', 1, struct annibale_camera_info)

/* Little-endian profile: header, up/down power entries, then init/mode/on/off registers. */
#define ANNIBALE_CAMERA_PROFILE_MAGIC "RWCTAB03"
struct annibale_camera_profile {
	char magic[8];
	uint32_t bytes, sensor, mode;
	uint32_t width, height, stride, bayer;
	uint32_t lane_assign, lane_count, settle_count, virtual_channel, data_type;
	uint32_t output_pixel_clock, line_length, frame_length, fps_milli;
	uint32_t init_count, mode_count, on_count, off_count, up_count, down_count;
	uint8_t source_sha256[32];
};

struct annibale_camera_power {
	uint32_t type, value, delay_ms;
};

struct annibale_camera_register {
	uint16_t address, value;
	uint8_t address_bytes, data_bytes, operation, reserved;
	uint32_t delay_us;
};

enum {
	ANNIBALE_CAM_CAPTURE_NO_PROFILE = 0,
	ANNIBALE_CAM_CAPTURE_READY,
	ANNIBALE_CAM_CAPTURE_RUNNING,
	ANNIBALE_CAM_CAPTURE_STOPPED,
	ANNIBALE_CAM_CAPTURE_FAILED,
	ANNIBALE_CAM_CAPTURE_HELD,
};

#define ANNIBALE_CAM_CAPTURE_BUF_DONE   (1u << 0)
#define ANNIBALE_CAM_CAPTURE_ADDR_MATCH (1u << 1)
#define ANNIBALE_CAM_CAPTURE_FIFO_EMPTY (1u << 2)
#define ANNIBALE_CAM_CAPTURE_HALTED     (1u << 3)
#define ANNIBALE_CAM_CAPTURE_DMA_STOPPED (1u << 4)
#define ANNIBALE_CAM_CAPTURE_SMMU_SYNC  (1u << 5)
#define ANNIBALE_CAM_CAPTURE_COMPLETE  (ANNIBALE_CAM_CAPTURE_BUF_DONE | ANNIBALE_CAM_CAPTURE_ADDR_MATCH | \
    ANNIBALE_CAM_CAPTURE_FIFO_EMPTY | ANNIBALE_CAM_CAPTURE_HALTED | ANNIBALE_CAM_CAPTURE_DMA_STOPPED | \
    ANNIBALE_CAM_CAPTURE_SMMU_SYNC)

/* CAMIOC_CAPTURE has no input. error is a positive errno; the ioctl copies status on failure too. */
struct annibale_camera_capture {
	uint32_t state, error, flags;
	uint32_t buf_done, top_irq, rx_irq, path_irq;
	uint32_t consumed_addr, fifo_count, reset_status, wm_cfg;
	uint32_t width, height, stride, bytes;
	uint64_t dma_address, sequence, time;
};

#define CAMIOC_CAPTURE          _IOR('C', 2, struct annibale_camera_capture)

#endif /* _SYS_ANNIBALE_CAMERA_H_ */

#!/usr/bin/env python3
"""Compile and test SM8750 Iris VPU (/dev/annibale-iris) and VideoToolbox plugin safety."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
KERNEL_SRC = ROOT / 'xnu/files/bsd/dev/annibale/annibale_iris.c'
VT_SRC = ROOT.parent / 'hackios-xnu-userland/userland/src/inject/videotoolbox_iris.c'
PACKAGE = (ROOT.parent / 'tools/k90auto/package.py').read_text()

KERNEL_HARNESS = r'''
#define ANNIBALE_IRIS_HOST_TEST 1
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define DEVFS_CHAR 1
#define UID_ROOT 0
#define GID_WHEEL 0

struct proc { int pid; };
struct cdevsw {
	int (*d_open)(dev_t, int, int, struct proc *);
	int (*d_close)(dev_t, int, int, struct proc *);
	void (*d_read)(void), (*d_write)(void);
	int (*d_ioctl)(dev_t, u_long, caddr_t, int, struct proc *);
	void (*d_stop)(void), (*d_reset)(void), (*d_select)(void), (*d_mmap)(void), (*d_strategy)(void);
	int d_type;
};
static void no_operation(void) {}
#define eno_rdwrt no_operation
#define eno_stop no_operation
#define eno_reset no_operation
#define eno_select no_operation
#define eno_mmap no_operation
#define eno_strat no_operation

static int registry_calls, node_calls, hardware_accesses, lock_depth;
static int open_sessions_hw, decode_calls_hw, flush_calls_hw, close_calls_hw;
static bool sim_fw_present, sim_hw_ready;
static int sim_open_error, sim_close_error;

static void iris_lock_acquire(void) { assert(lock_depth == 0); lock_depth++; }
static void iris_lock_release(void) { assert(lock_depth == 1); lock_depth--; }
static int iris_caller_pid(struct proc *p) { return p ? p->pid : 0; }

static int cdevsw_add(int major, const struct cdevsw *ops) {
	assert(major == -1 && ops->d_open && ops->d_ioctl && ops->d_close);
	registry_calls++;
	return 42;
}
static void *devfs_make_node(dev_t dev, int type, int uid, int gid, int mode, const char *name) {
	(void)dev;
	assert(type == DEVFS_CHAR && uid == 0 && gid == 0 && mode == 0666);
	assert(strcmp(name, "annibale-iris") == 0);
	node_calls++;
	return (void *)1;
}

#include "annibale_iris_private.h"

bool iris_hw_probe_firmware(void) {
	hardware_accesses++;
	if (sim_fw_present) {
		g_iris.fw_size = 2413256u;
		return true;
	}
	return false;
}
bool iris_hw_ensure_ready(void) {
	hardware_accesses++;
	return sim_hw_ready;
}
int iris_hfi_session_open(struct iris_session *s) {
	assert(s->active && s->owner_pid && s->session_id);
	hardware_accesses++;
	open_sessions_hw++;
	s->opened = true;
	s->started_ports = 1u << HFI_PORT_BITSTREAM;
	s->bitstream.kva = 0x1000u + s->session_id;
	return sim_open_error;
}
int iris_hfi_decode_frame(struct iris_session *s, struct iris_decode_frame *req) {
	(void)s;
	hardware_accesses++;
	decode_calls_hw++;
	req->out_tag = req->in_tag;
	req->flags = IRIS_FRAME_DECODED;
	return 0;
}
int iris_hfi_session_flush(struct iris_session *s) {
	(void)s;
	hardware_accesses++;
	flush_calls_hw++;
	return 0;
}
int iris_hfi_session_close(struct iris_session *s) {
	hardware_accesses++;
	close_calls_hw++;
	if (sim_close_error) return sim_close_error;
	s->opened = false;
	s->started_ports = 0;
	memset(&s->bitstream, 0, sizeof(s->bitstream));
	return 0;
}

__IRIS_KERNEL_BODY__

int main(void) {
	annibale_iris_init();
	annibale_iris_init();
	assert(registry_calls == 1 && node_calls == 1 && hardware_accesses == 0);

	struct proc p1 = { .pid = 101 }, p2 = { .pid = 202 };
	assert(iris_cdevsw.d_open(0, 0, 0, &p1) == 0);
	assert(iris_cdevsw.d_close(0, 0, 0, &p1) == 0);
	assert(hardware_accesses == 0 && lock_depth == 0);

	/* NULL data and unknown command safety without touching hardware */
	const u_long cmds[] = {
		IRIS_IOC_GET_INFO, IRIS_IOC_OPEN_SESSION, IRIS_IOC_DECODE_FRAME,
		IRIS_IOC_FLUSH_SESSION, IRIS_IOC_CLOSE_SESSION, 0xdeadbeefUL
	};
	unsigned char scratch[128], orig[128];
	memset(scratch, 0xa5, sizeof(scratch));
	memcpy(orig, scratch, sizeof(orig));
	for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		int rc = iris_cdevsw.d_ioctl(0, cmds[i], NULL, 0, &p1);
		assert(rc == (i < 5 ? EINVAL : ENOTTY));
	}
	assert(iris_cdevsw.d_ioctl(0, 0xdeadbeefUL, (caddr_t)scratch, 0, &p1) == ENOTTY);
	assert(memcmp(scratch, orig, sizeof(scratch)) == 0);
	assert(hardware_accesses == 0);

	/* Hardware offline: GET_INFO omits HW_READY, OPEN_SESSION returns EOPNOTSUPP */
	sim_fw_present = false;
	sim_hw_ready = false;
	struct iris_device_info info;
	memset(&info, 0xff, sizeof(info));
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_GET_INFO, (caddr_t)&info, 0, &p1) == 0);
	assert(info.version == IRIS_ABI_VERSION);
	assert(info.flags == IRIS_INFO_DEV_PRESENT);
	assert(info.codecs_mask == 0);

	struct iris_session_params sp = { .codec_type = IRIS_CODEC_H264, .width = 1920, .height = 1080 };
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, &p1) == EOPNOTSUPP);
	sp.width = 16;
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, &p1) == EINVAL);

	/* Hardware ready: session table capacity, decode, flush, close, and PID cleanup */
	sim_fw_present = true;
	sim_hw_ready = true;
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_GET_INFO, (caddr_t)&info, 0, &p1) == 0);
	assert((info.flags & (IRIS_INFO_DEV_PRESENT | IRIS_INFO_FW_PRESENT | IRIS_INFO_HW_READY)) ==
	    (IRIS_INFO_DEV_PRESENT | IRIS_INFO_FW_PRESENT | IRIS_INFO_HW_READY));
	assert(info.codecs_mask == (IRIS_CODEC_MASK_H264 | IRIS_CODEC_MASK_HEVC));

	const unsigned half = IRIS_MAX_SESSIONS / 2u;
	uint32_t sids[IRIS_MAX_SESSIONS];
	for (unsigned i = 0; i < IRIS_MAX_SESSIONS; i++) {
		sp.codec_type = (i & 1) ? IRIS_CODEC_HEVC : IRIS_CODEC_H264;
		sp.width = 1280;
		sp.height = 720;
		sp.session_id = 0;
		assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, (i < half ? &p1 : &p2)) == 0);
		assert(sp.session_id == i + 1u);
		sids[i] = sp.session_id;
	}
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, &p1) == EBUSY);

	struct iris_decode_frame df;
	memset(&df, 0, sizeof(df));
	df.session_id = sids[0];
	df.in_tag = 77;
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_DECODE_FRAME, (caddr_t)&df, 0, &p1) == 0);
	assert(df.flags == IRIS_FRAME_DECODED && df.out_tag == 77);
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_FLUSH_SESSION, (caddr_t)&sids[0], 0, &p1) == 0);
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_CLOSE_SESSION, (caddr_t)&sids[0], 0, &p1) == 0);
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_CLOSE_SESSION, (caddr_t)&sids[0], 0, &p1) == EINVAL);

	/* Closing p1 reclaims only p1's remaining sessions; p2's stay active */
	assert(iris_cdevsw.d_close(0, 0, 0, &p1) == 0);
	assert(close_calls_hw == (int)half);
	assert(iris_cdevsw.d_close(0, 0, 0, &p2) == 0);
	assert(close_calls_hw == (int)IRIS_MAX_SESSIONS);
	assert(lock_depth == 0);

	sim_open_error = EIO;
	sp.session_id = 0;
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, &p1) == EIO);
	assert(!g_iris.sessions[0].active && sp.session_id == 0);
	sim_close_error = ETIMEDOUT;
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, &p1) == ETIMEDOUT);
	assert(g_iris.sessions[0].active && g_iris.sessions[0].opened && g_iris.sessions[0].bitstream.kva);
	assert(g_iris.sessions[0].owner_pid == p1.pid && sp.session_id == 0);
	sim_open_error = 0;
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, &p1) == 0 && sp.session_id == 2);
	uint32_t retained_sid = 1;
	assert(iris_cdevsw.d_ioctl(0, IRIS_IOC_CLOSE_SESSION, (caddr_t)&retained_sid, 0, &p1) == ETIMEDOUT);
	assert(g_iris.sessions[0].active && g_iris.sessions[0].bitstream.kva);
	assert(iris_cdevsw.d_close(0, 0, 0, &p1) == ETIMEDOUT);
	assert(g_iris.sessions[0].active && g_iris.sessions[1].active);
	assert(g_iris.sessions[0].owner_pid == p1.pid && g_iris.sessions[1].owner_pid == p1.pid);
	sim_close_error = 0;
	assert(iris_cdevsw.d_close(0, 0, 0, &p1) == 0);
	assert(!g_iris.sessions[0].active && !g_iris.sessions[1].active && lock_depth == 0);
	puts("PASS: Iris kernel cdevsw safety, lazy init, and session lifecycle verified");
	return 0;
}
'''

VT_HARNESS = r'''
#define IRIS_VT_HOST_TEST 1
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t OSStatus;
typedef uint8_t Boolean;
typedef unsigned long CFHashCode;
typedef unsigned long CFTypeID;
typedef intptr_t CFIndex;
typedef uint32_t FourCharCode;
typedef FourCharCode CMVideoCodecType;
typedef uint32_t VTDecodeFrameFlags;
typedef uint32_t VTDecodeInfoFlags;
typedef int32_t CVReturn;

enum {
	noErr = 0,
	kCVReturnSuccess = 0,
	kCMBlockBufferNoErr = 0,
	kVTDecodeInfo_FrameDropped = 1u << 2,
	kVTCouldNotFindVideoDecoderErr = -12906,
	kVTVideoDecoderBadDataErr = -12909,
	kVTVideoDecoderUnsupportedDataFormatErr = -12910,
	kVTVideoDecoderMalfunctionErr = -12911,
	kCMVideoCodecType_H264 = 0x61766331u,
	kCMVideoCodecType_HEVC = 0x68766331u,
	kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange = 0x34323076u,
	kCFNumberSInt32Type = 3,
};

typedef const void *CFTypeRef;
typedef const void *CFAllocatorRef;
typedef const char *CFStringRef;
typedef const void *CFDictionaryRef;
typedef const void *CFNumberRef;
typedef const void *CFBooleanRef;
typedef const void *CFDataRef;
typedef const void *CFArrayRef;
typedef void *CMFormatDescriptionRef;
typedef void *CMVideoFormatDescriptionRef;
typedef void *CMSampleBufferRef;
typedef void *CMBlockBufferRef;
typedef void *CVPixelBufferRef;
typedef void *CVImageBufferRef;
typedef void *VTDecoderSessionRef;
typedef void *VTDecodeFrameRef;
typedef struct { int32_t width, height; } CMVideoDimensions;

#define kCFAllocatorDefault ((CFAllocatorRef)NULL)
#define CFSTR(s) (s)
static const int k_true_obj = 1, k_false_obj = 0;
#define kCFBooleanTrue ((CFBooleanRef)&k_true_obj)
static const int kCFTypeDictionaryKeyCallBacks = 0;
static const int kCFTypeDictionaryValueCallBacks = 0;
static const char *kVTVideoDecoder_IsHardwareAccelerated = "IsHardwareAccelerated";
static const char *kVTVideoDecoderInfo_Rating = "Rating";
static const char *kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms = "Atoms";
static const char *kCVPixelBufferPixelFormatTypeKey = "PixelFormatType";
static const char *kCVPixelBufferWidthKey = "Width";
static const char *kCVPixelBufferHeightKey = "Height";
static const char *kCVPixelBufferIOSurfacePropertiesKey = "IOSurfaceProperties";
static const char *kCMSampleAttachmentKey_NotSync = "NotSync";

struct fake_cf_obj {
	int refcnt;
	int kind;
	uint8_t storage[512];
};

static VTDecoderSessionRef borrowed_session;
static int cf_live_objects, pixbuf_allocations;

static void require_cf_object(CFTypeRef p) {
	assert(p != borrowed_session);
	assert((uintptr_t)p >= 4096);
}

static CFTypeRef CFRetain(CFTypeRef p) {
	if (p == NULL || p == kCFBooleanTrue || p == (CFTypeRef)&k_false_obj) return p;
	require_cf_object(p);
	((struct fake_cf_obj *)p)->refcnt++;
	return p;
}
static void CFRelease(CFTypeRef p) {
	if (p == NULL || p == kCFBooleanTrue || p == (CFTypeRef)&k_false_obj) return;
	require_cf_object(p);
	struct fake_cf_obj *o = (struct fake_cf_obj *)p;
	assert(o->refcnt > 0);
	if (--o->refcnt == 0) { cf_live_objects--; free(o); }
}
static Boolean CFEqual(CFTypeRef a, CFTypeRef b) {
	return a && b && strcmp((const char *)a, (const char *)b) == 0;
}
static Boolean CFBooleanGetValue(CFBooleanRef b) { return b == kCFBooleanTrue; }
static CFNumberRef CFNumberCreate(CFAllocatorRef a, int t, const void *v) {
	(void)a; (void)t; (void)v;
	struct fake_cf_obj *o = calloc(1, sizeof(*o)); o->refcnt = 1; cf_live_objects++; return o;
}
static CFDictionaryRef CFDictionaryCreate(CFAllocatorRef a, const void **k, const void **v, CFIndex n, const int *kc, const int *vc) {
	(void)a; (void)k; (void)v; (void)n; (void)kc; (void)vc;
	struct fake_cf_obj *o = calloc(1, sizeof(*o)); o->refcnt = 1; cf_live_objects++; return o;
}
static CFStringRef CFStringCreateWithFormat(CFAllocatorRef a, CFDictionaryRef o, CFStringRef f, ...) {
	(void)a; (void)o; (void)f;
	struct fake_cf_obj *s = calloc(1, sizeof(*s)); s->refcnt = 1; cf_live_objects++; return (CFStringRef)s;
}

struct fake_data { const uint8_t *bytes; size_t len; };
struct fake_fmt { CMVideoCodecType sub; int32_t w, h; struct fake_data cfg; };
struct fake_sample { const uint8_t *bytes; size_t len; bool not_sync; };
struct fake_pixbuf { struct fake_cf_obj base; uint8_t y[256 * 128]; uint8_t uv[256 * 64]; };

static const void *CFDictionaryGetValue(CFDictionaryRef d, const void *key) {
	if (d == (CFDictionaryRef)0x99) {
		const struct fake_sample *s = (const struct fake_sample *)key;
		(void)s;
	}
	const struct fake_fmt *f = (const struct fake_fmt *)d;
	if (strcmp((const char *)key, "avcC") == 0 || strcmp((const char *)key, "hvcC") == 0) {
		return f->cfg.bytes ? &f->cfg : NULL;
	}
	if (strcmp((const char *)key, "NotSync") == 0) {
		const struct fake_sample *s = (const struct fake_sample *)d;
		return s->not_sync ? kCFBooleanTrue : (CFBooleanRef)&k_false_obj;
	}
	return NULL;
}
static const uint8_t *CFDataGetBytePtr(CFDataRef d) { return ((const struct fake_data *)d)->bytes; }
static CFIndex CFDataGetLength(CFDataRef d) { return (CFIndex)((const struct fake_data *)d)->len; }
static CFIndex CFArrayGetCount(CFArrayRef a) { return a ? 1 : 0; }
static const void *CFArrayGetValueAtIndex(CFArrayRef a, CFIndex i) { (void)i; return a; }

static CMVideoDimensions CMVideoFormatDescriptionGetDimensions(CMVideoFormatDescriptionRef f) {
	const struct fake_fmt *ff = f;
	return (CMVideoDimensions){ ff->w, ff->h };
}
static FourCharCode CMFormatDescriptionGetMediaSubType(CMFormatDescriptionRef f) {
	return ((const struct fake_fmt *)f)->sub;
}
static CFTypeRef CMFormatDescriptionGetExtension(CMFormatDescriptionRef f, CFStringRef k) {
	(void)k; return f;
}
static CMBlockBufferRef CMSampleBufferGetDataBuffer(CMSampleBufferRef s) { return s; }
static size_t CMBlockBufferGetDataLength(CMBlockBufferRef b) { return ((const struct fake_sample *)b)->len; }
static OSStatus CMBlockBufferGetDataPointer(CMBlockBufferRef b, size_t off, size_t *lat, size_t *tot, char **p) {
	const struct fake_sample *s = b;
	(void)off; *lat = s->len; *tot = s->len; *p = (char *)(uintptr_t)s->bytes;
	return kCMBlockBufferNoErr;
}
static OSStatus CMBlockBufferCopyDataBytes(CMBlockBufferRef b, size_t off, size_t len, void *dst) {
	const struct fake_sample *s = b;
	memcpy(dst, s->bytes + off, len);
	return kCMBlockBufferNoErr;
}
static CFArrayRef CMSampleBufferGetSampleAttachmentsArray(CMSampleBufferRef s, Boolean create) {
	(void)create; return s;
}

static CFTypeID VTVideoDecoderGetClassID(void) { return 77; }
static const void *last_created_vtable;
static OSStatus CMDerivedObjectCreate(CFAllocatorRef a, const void *vtable, CFTypeID cid, CFTypeRef *out) {
	(void)a; assert(cid == 77 && vtable != NULL);
	last_created_vtable = vtable;
	struct fake_cf_obj *o = calloc(1, sizeof(*o) + 2048);
	o->refcnt = 1;
	cf_live_objects++;
	*out = o;
	return noErr;
}
static void *CMBaseObjectGetDerivedStorage(CFTypeRef obj) {
	return ((struct fake_cf_obj *)obj)->storage;
}

static int vt_register_calls;
static CMVideoCodecType vt_registered_codecs[8];
static OSStatus (*vt_create_cb)(CMVideoCodecType, CFAllocatorRef, CFTypeRef *);
static OSStatus VTRegisterVideoDecoderWithInfo(CMVideoCodecType codec, CFDictionaryRef info,
    OSStatus (*cb)(CMVideoCodecType, CFAllocatorRef, CFTypeRef *)) {
	assert(info != NULL && cb != NULL);
	vt_registered_codecs[vt_register_calls++] = codec;
	vt_create_cb = cb;
	return noErr;
}
static int pb_attr_calls;
static OSStatus VTDecoderSessionSetPixelBufferAttributes(VTDecoderSessionRef s, CFDictionaryRef a) {
	assert(s == borrowed_session && a != NULL); pb_attr_calls++; return noErr;
}
static OSStatus VTDecoderSessionCreatePixelBuffer(VTDecoderSessionRef s, VTDecodeFrameRef fr, CVPixelBufferRef *out) {
	assert(s == borrowed_session); (void)fr;
	struct fake_pixbuf *pb = calloc(1, sizeof(*pb));
	pb->base.refcnt = 1;
	cf_live_objects++;
	pixbuf_allocations++;
	*out = pb;
	return noErr;
}
static CVReturn CVPixelBufferLockBaseAddress(CVPixelBufferRef pb, uint64_t f) { (void)pb; (void)f; return kCVReturnSuccess; }
static CVReturn CVPixelBufferUnlockBaseAddress(CVPixelBufferRef pb, uint64_t f) { (void)pb; (void)f; errno = ENOSPC; return kCVReturnSuccess; }
static void *CVPixelBufferGetBaseAddressOfPlane(CVPixelBufferRef pb, size_t idx) {
	struct fake_pixbuf *p = pb;
	return idx == 0 ? p->y : p->uv;
}
static size_t CVPixelBufferGetBytesPerRowOfPlane(CVPixelBufferRef pb, size_t idx) { (void)pb; (void)idx; return 256; }

static int emit_calls;
static OSStatus sim_emit_status;
static VTDecodeFrameRef emitted_frames[64];
static OSStatus emitted_statuses[64];
static VTDecodeInfoFlags emitted_flags[64];
static bool emitted_images[64];
static OSStatus VTDecoderSessionEmitDecodedFrame(VTDecoderSessionRef s, VTDecodeFrameRef fr,
    OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img) {
	assert(s == borrowed_session && fr != NULL);
	assert(emit_calls < 64);
	if (img != NULL) {
		assert(((struct fake_pixbuf *)img)->base.refcnt == 1);
		CFRetain(img);
		CFRelease(img);
	}
	emitted_frames[emit_calls] = fr;
	emitted_statuses[emit_calls] = st;
	emitted_flags[emit_calls] = fl;
	emitted_images[emit_calls] = img != NULL;
	emit_calls++;
	return sim_emit_status;
}

static bool sim_dev_exists;
static uint32_t sim_info_flags;
static int sim_delay_mode;
static bool sim_fail_decode, sim_fail_poll, sim_fail_flush, sim_fail_drain;
static uint32_t sim_output_flags, sim_poll_flags, sim_drain_flags;
static uint64_t sim_output_tag;
static unsigned sim_decode_calls, sim_flush_calls;
static uint64_t delayed_tag;
static uint8_t last_submitted_bs[256];
static uint32_t last_submitted_len;

static int iris_sys_open(const char *path, int flags) {
	(void)flags;
	assert(strcmp(path, "/dev/annibale-iris") == 0);
	return sim_dev_exists ? 10 : -1;
}
static int iris_sys_close(int fd) { assert(fd == 10); return 0; }

#include "__VT_SOURCE__"

static int iris_sys_ioctl(int fd, unsigned long cmd, void *arg) {
	assert(fd == 10 && arg != NULL);
	if (cmd == IRIS_IOC_GET_INFO) {
		struct iris_device_info *info = arg;
		info->version = IRIS_ABI_VERSION;
		info->flags = sim_info_flags;
		info->codecs_mask = (sim_info_flags & IRIS_INFO_HW_READY) ?
		    (IRIS_CODEC_MASK_H264 | IRIS_CODEC_MASK_HEVC) : 0;
		return 0;
	}
	if (cmd == IRIS_IOC_OPEN_SESSION) {
		struct iris_session_params *sp = arg;
		sp->session_id = 1;
		return 0;
	}
	if (cmd == IRIS_IOC_DECODE_FRAME) {
		sim_decode_calls++;
		struct iris_decode_frame *req = arg;
		if (sim_fail_decode) { errno = EIO; return -1; }
		if ((req->flags & IRIS_FRAME_POLL) != 0) {
			if (sim_fail_poll) return -1;
			req->flags = sim_poll_flags;
			sim_poll_flags = 0;
			req->out_tag = delayed_tag;
			if (req->flags != 0) delayed_tag = 0;
			return 0;
		}
		if ((req->flags & IRIS_FRAME_DRAIN) != 0) {
			if (sim_fail_drain) { errno = EIO; return -1; }
			if (delayed_tag != 0) {
				req->out_tag = delayed_tag;
				delayed_tag = 0;
				req->flags = sim_drain_flags ? sim_drain_flags : IRIS_FRAME_DECODED;
				sim_drain_flags = 0;
			} else {
				req->flags = 0;
			}
			return 0;
		}
		assert(req->input_len <= sizeof(last_submitted_bs));
		memcpy(last_submitted_bs, (const void *)(uintptr_t)req->input_user_addr, req->input_len);
		last_submitted_len = req->input_len;
		if (sim_output_flags != 0) {
			req->flags = sim_output_flags;
			req->out_tag = sim_output_tag;
			sim_output_flags = 0;
			return 0;
		}
		memset((void *)(uintptr_t)req->out_user_y, 0x5a, 128);
		if (sim_delay_mode == 1) {
			delayed_tag = req->in_tag;
			sim_delay_mode = 2;
			req->flags = IRIS_FRAME_NEED_MORE;
			return 0;
		}
		req->out_tag = req->in_tag;
		req->flags = IRIS_FRAME_DECODED;
		return 0;
	}
	if (cmd == IRIS_IOC_FLUSH_SESSION || cmd == IRIS_IOC_CLOSE_SESSION) {
		if (cmd == IRIS_IOC_FLUSH_SESSION) {
			sim_flush_calls++;
			if (sim_fail_flush) { errno = EIO; return -1; }
		}
		return 0;
	}
	return -1;
}

static void test_pending_tokens(void) {
	struct iris_vt_decoder dec = { .session = borrowed_session, .fd = -1 };
	const int live_before = cf_live_objects;
	emit_calls = 0;
	for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) {
		dec.next_tag = i + 1;
		assert(iris_pending_push(&dec, 100 + i, (VTDecodeFrameRef)(uintptr_t)(i + 1)));
	}
	assert(!iris_pending_push(&dec, 999, (VTDecodeFrameRef)(uintptr_t)999));
	assert(iris_pending_pop(&dec, 999) == NULL);
	assert(iris_pending_pop(&dec, 103) == (VTDecodeFrameRef)(uintptr_t)4);
	assert(iris_pending_pop(&dec, 0) == NULL);
	iris_pending_abort_all(&dec, kVTVideoDecoderMalfunctionErr);
	assert(emit_calls == (int)IRIS_VT_PENDING_MAX - 1);
	int emitted = 0;
	for (uint32_t token = 1; token <= IRIS_VT_PENDING_MAX; token++) {
		if (token == 4) continue;
		assert(emitted_frames[emitted] == (VTDecodeFrameRef)(uintptr_t)token);
		assert(emitted_statuses[emitted] == kVTVideoDecoderMalfunctionErr);
		assert(emitted_flags[emitted++] == kVTDecodeInfo_FrameDropped);
	}
	for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) assert(!dec.pending[i].in_use);
	iris_pending_abort_all(&dec, kVTVideoDecoderMalfunctionErr);
	assert(emit_calls == emitted && cf_live_objects == live_before);
}

static unsigned pending_count(const struct iris_vt_decoder *dec) {
	unsigned count = 0;
	for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) count += dec->pending[i].in_use;
	return count;
}

static void test_corrupt_outputs(CFTypeRef obj) {
	struct iris_vt_decoder *dec = CMBaseObjectGetDerivedStorage(obj);
	const uint8_t nal[] = {0, 0, 0, 2, 1, 0x9a};
	struct fake_sample sample = {nal, sizeof(nal), true};
	const int live_before = cf_live_objects;
	sim_delay_mode = 1;
	assert(iris_vt_decode_frame(obj, (void *)(uintptr_t)101, &sample, 0, NULL) == noErr);
	uint64_t older = dec->next_tag;
	sim_output_flags = 2; sim_output_tag = older;
	assert(iris_vt_decode_frame(obj, (void *)(uintptr_t)102, &sample, 0, NULL) == noErr);
	assert(emit_calls == 1 && emitted_frames[0] == (void *)(uintptr_t)101);
	assert(emitted_statuses[0] == kVTVideoDecoderBadDataErr && emitted_flags[0] == kVTDecodeInfo_FrameDropped);
	assert(!emitted_images[0] && pending_count(dec) == 1 && cf_live_objects == live_before);
	delayed_tag = dec->next_tag; sim_poll_flags = 1;
	assert(iris_vt_decode_frame(obj, (void *)(uintptr_t)103, &sample, 0, NULL) == noErr);
	assert(emit_calls == 3 && emitted_frames[1] == (void *)(uintptr_t)103 && emitted_frames[2] == (void *)(uintptr_t)102);
	assert(emitted_statuses[1] == noErr && emitted_statuses[2] == noErr && emitted_images[1] && emitted_images[2]);
	assert(pending_count(dec) == 0);

	/* A damaged submit output and a damaged POLL output each complete their exact token. */
	sim_delay_mode = 1;
	assert(iris_vt_decode_frame(obj, (void *)(uintptr_t)104, &sample, 0, NULL) == noErr);
	sim_output_flags = 2; sim_output_tag = dec->next_tag;
	delayed_tag = dec->next_tag + 1; sim_poll_flags = 2;
	assert(iris_vt_decode_frame(obj, (void *)(uintptr_t)105, &sample, 0, NULL) == noErr);
	assert(emit_calls == 5 && emitted_frames[3] == (void *)(uintptr_t)104 && emitted_frames[4] == (void *)(uintptr_t)105);
	for (unsigned i = 3; i < 5; i++)
		assert(emitted_statuses[i] == kVTVideoDecoderBadDataErr && emitted_flags[i] == kVTDecodeInfo_FrameDropped && !emitted_images[i]);
	assert(pending_count(dec) == 0);

	assert(iris_pending_push(dec, 0xf01, (void *)(uintptr_t)201));
	assert(iris_pending_push(dec, 0xf02, (void *)(uintptr_t)202));
	const uint64_t unknown_tags[] = {0, 0xfff, older};
	pthread_mutex_lock(&dec->lock);
	for (unsigned i = 0; i < 3; i++) {
		bool emitted = true;
		delayed_tag = unknown_tags[i]; sim_poll_flags = 2;
		assert(iris_run_decode_step_Locked(dec, NULL, NULL, 0, 0, IRIS_FRAME_POLL, &emitted) == kVTVideoDecoderMalfunctionErr);
		assert(!emitted && emit_calls == 5 && pending_count(dec) == 2 && cf_live_objects == live_before);
	}
	bool emitted = true;
	delayed_tag = 0xf01; sim_poll_flags = 3;
	assert(iris_run_decode_step_Locked(dec, NULL, NULL, 0, 0, IRIS_FRAME_POLL, &emitted) == kVTVideoDecoderMalfunctionErr);
	assert(!emitted && emit_calls == 5 && pending_count(dec) == 2);
	delayed_tag = 0xf01; sim_poll_flags = 2;
	assert(iris_run_decode_step_Locked(dec, NULL, NULL, 0, 0, IRIS_FRAME_POLL, &emitted) == noErr && emitted);
	delayed_tag = 0xf02; sim_poll_flags = 1;
	assert(iris_run_decode_step_Locked(dec, NULL, NULL, 0, 0, IRIS_FRAME_POLL, &emitted) == noErr && emitted);
	pthread_mutex_unlock(&dec->lock);
	assert(emit_calls == 7 && emitted_frames[5] == (void *)(uintptr_t)201 && emitted_frames[6] == (void *)(uintptr_t)202);
	assert(emitted_statuses[5] == kVTVideoDecoderBadDataErr && !emitted_images[5] && emitted_statuses[6] == noErr && emitted_images[6]);
	assert(pending_count(dec) == 0);

	sim_delay_mode = 1;
	assert(iris_vt_decode_frame(obj, (void *)(uintptr_t)106, &sample, 0, NULL) == noErr);
	sim_drain_flags = 2;
	unsigned flush_before = sim_flush_calls;
	assert(iris_vt_finish_delayed_frames(obj) == noErr && sim_flush_calls == flush_before + 1);
	assert(emit_calls == 8 && emitted_frames[7] == (void *)(uintptr_t)106 && emitted_statuses[7] == kVTVideoDecoderBadDataErr && !emitted_images[7]);
	assert(pending_count(dec) == 0);
	unsigned calls_before = sim_decode_calls;
	assert(iris_vt_finish_delayed_frames(obj) == noErr && sim_decode_calls == calls_before && sim_flush_calls == flush_before + 1);
	assert(iris_vt_decode_frame(obj, (void *)(uintptr_t)107, &sample, 0, NULL) == noErr);
	assert(emit_calls == 9 && emitted_statuses[8] == noErr && emitted_images[8]);

	sim_delay_mode = 1;
	assert(iris_vt_decode_frame(obj, (void *)(uintptr_t)108, &sample, 0, NULL) == noErr);
	sim_fail_flush = true;
	assert(iris_vt_finish_delayed_frames(obj) == kVTVideoDecoderMalfunctionErr);
	sim_fail_flush = false;
	assert(emit_calls == 10 && emitted_frames[9] == (void *)(uintptr_t)108 && pending_count(dec) == 0);

	sim_delay_mode = 1;
	assert(iris_vt_decode_frame(obj, (void *)(uintptr_t)109, &sample, 0, NULL) == noErr);
	sim_fail_drain = true;
	assert(iris_vt_finish_delayed_frames(obj) == kVTVideoDecoderBadDataErr);
	sim_fail_drain = false; delayed_tag = 0;
	assert(emit_calls == 11 && emitted_frames[10] == (void *)(uintptr_t)109 && emitted_statuses[10] == kVTVideoDecoderBadDataErr && !emitted_images[10]);
	assert(pending_count(dec) == 0 && cf_live_objects == live_before);
	assert(iris_vt_finish_delayed_frames(obj) == noErr && emit_calls == 11);
}

int main(int argc, char **argv) {
	uintptr_t session_words[4] = { 0x101, 0x202, 0x303, 0 };
	const uintptr_t original_words[4] = { 0x101, 0x202, 0x303, 0 };
	VTDecoderSessionRef sess = session_words;
	borrowed_session = sess;
	if (argc == 2 && strcmp(argv[1], "tokens") == 0) {
		test_pending_tokens();
		puts("PASS: integer frame tokens survive queue completion and cancellation");
		return 0;
	}
	/* 1. Missing device & unready VPU must NOT register hardware decoders */
	sim_dev_exists = false;
	register_iris_videotoolbox_decoders();
	assert(vt_register_calls == 0);

	sim_dev_exists = true;
	sim_info_flags = IRIS_INFO_DEV_PRESENT | IRIS_INFO_FW_PRESENT;
	register_iris_videotoolbox_decoders();
	assert(vt_register_calls == 0);

	/* 2. Ready VPU registers avc1, hvc1, hev1 */
	sim_info_flags = IRIS_INFO_DEV_PRESENT | IRIS_INFO_FW_PRESENT | IRIS_INFO_HW_READY;
	register_iris_videotoolbox_decoders();
	assert(vt_register_calls == 3 && vt_create_cb != NULL);

	/* 3. Create instance, check vtable versions & UsingHardwareAcceleratedVideoDecoder */
	CFTypeRef dec_obj = NULL;
	assert(vt_create_cb(kCMVideoCodecType_H264, NULL, &dec_obj) == noErr && dec_obj != NULL);
	const struct iris_cm_vtable *vt = last_created_vtable;
	assert(vt->base_class->version == 1 && vt->derived_class->version == 2);

	CFTypeRef hw_prop = NULL;
	assert(vt->base_class->copy_property(dec_obj, "UsingHardwareAcceleratedVideoDecoder", NULL, &hw_prop) == noErr);
	assert(hw_prop == kCFBooleanTrue);

	/* 4. Start session with avcC (SPS={0x67,0x42,0x00,0x1e}, PPS={0x68,0xce,0x3c,0x80}) */
	static const uint8_t avcc[] = {
		0x01, 0x42, 0x00, 0x1e, 0xff,
		0xe1, 0x00, 0x04, 0x67, 0x42, 0x00, 0x1e,
		0x01, 0x00, 0x04, 0x68, 0xce, 0x3c, 0x80
	};
	struct fake_fmt fmt = { .sub = kCMVideoCodecType_H264, .w = 160, .h = 96, .cfg = { avcc, sizeof(avcc) } };
	assert(vt->derived_class->start_session(dec_obj, sess, &fmt) == noErr);
	assert(pb_attr_calls == 1);
	assert(cf_live_objects == 1 && memcmp(session_words, original_words, sizeof(session_words)) == 0);
	assert(vt->derived_class->can_accept_format(dec_obj, &fmt));
	for (unsigned repeat = 0; repeat < 3; repeat++) {
		assert(vt->derived_class->finish_delayed_frames(dec_obj) == noErr);
	}
	assert(sim_decode_calls == 0 && sim_flush_calls == 0);
	if (argc == 2 && strcmp(argv[1], "corrupt") == 0) {
		test_corrupt_outputs(dec_obj);
		assert(vt->base_class->invalidate(dec_obj) == noErr && emit_calls == 11);
		vt->base_class->finalize(dec_obj);
		CFRelease(dec_obj);
		assert(cf_live_objects == 0);
		puts("PASS: exact damaged output tokens, NULL images, continued decode, drain errors, and cleanup");
		return 0;
	}

	/* 5. Corrupt zero-length NALU is rejected with BadDataErr (-12909) */
	static const uint8_t bad_nal[] = { 0x00, 0x00, 0x00, 0x00 };
	struct fake_sample bad_sample = { bad_nal, sizeof(bad_nal), false };
	VTDecodeFrameRef fref_bad = (VTDecodeFrameRef)(uintptr_t)1;
	VTDecodeInfoFlags failure_flags = 0;
	assert(vt->derived_class->decode_frame(dec_obj, fref_bad, &bad_sample, 0, &failure_flags) == kVTVideoDecoderBadDataErr);
	assert(emit_calls == 1 && emitted_frames[0] == fref_bad);
	assert(emitted_statuses[0] == kVTVideoDecoderBadDataErr && emitted_flags[0] == kVTDecodeInfo_FrameDropped);
	assert(failure_flags == kVTDecodeInfo_FrameDropped);
	emit_calls = 0;

	/* 6. Valid AVCC IDR + B-frame reorder + drain */
	static const uint8_t idr_nal[] = { 0x00, 0x00, 0x00, 0x03, 0x65, 0x88, 0x84 };
	static const uint8_t b_nal[]   = { 0x00, 0x00, 0x00, 0x02, 0x01, 0x9a };
	struct fake_sample s1 = { idr_nal, sizeof(idr_nal), false };
	struct fake_sample s2 = { b_nal, sizeof(b_nal), true };
	VTDecodeFrameRef fref1 = (VTDecodeFrameRef)(uintptr_t)1;
	VTDecodeFrameRef fref2 = (VTDecodeFrameRef)(uintptr_t)2;

	sim_delay_mode = 1;
	assert(vt->derived_class->decode_frame(dec_obj, fref1, &s1, 0, NULL) == noErr);
	/* First sync sample prepended Annex-B SPS (8 B) + PPS (8 B) + IDR (7 B) = 23 B */
	assert(last_submitted_len == 23);
	assert(memcmp(last_submitted_bs, "\x00\x00\x00\x01\x67\x42\x00\x1e\x00\x00\x00\x01\x68\xce\x3c\x80\x00\x00\x00\x01\x65\x88\x84", 23) == 0);
	assert(emit_calls == 0);

	assert(vt->derived_class->decode_frame(dec_obj, fref2, &s2, 0, NULL) == noErr);
	/* Non-sync sample omits SPS/PPS: 4 + 2 = 6 B, emits fref2 while fref1 stays in DPB */
	assert(last_submitted_len == 6);
	assert(emit_calls == 1 && emitted_frames[0] == fref2 && emitted_statuses[0] == noErr);

	/* Drain flushes delayed fref1 */
	assert(vt->derived_class->finish_delayed_frames(dec_obj) == noErr);
	assert(emit_calls == 2 && emitted_frames[1] == fref1 && emitted_statuses[1] == noErr);
	assert(cf_live_objects == 1 && pixbuf_allocations > 0);
	assert(sim_flush_calls == 1);
	unsigned decode_calls = sim_decode_calls, flush_calls = sim_flush_calls;
	for (unsigned repeat = 0; repeat < 3; repeat++) {
		assert(vt->derived_class->finish_delayed_frames(dec_obj) == noErr);
	}
	assert(sim_decode_calls == decode_calls && sim_flush_calls == flush_calls);

	/* The native wrapper consumes the return code; the frame still needs completion. */
	sim_fail_decode = true;
	assert(vt->derived_class->decode_frame(dec_obj, (VTDecodeFrameRef)(uintptr_t)3, &s2, 0, NULL) == kVTVideoDecoderBadDataErr);
	sim_fail_decode = false;
	struct iris_vt_decoder *storage = CMBaseObjectGetDerivedStorage(dec_obj);
	for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) assert(!storage->pending[i].in_use);
	assert(emit_calls == 3 && cf_live_objects == 1);
	assert(emitted_frames[2] == (VTDecodeFrameRef)(uintptr_t)3);
	assert(emitted_statuses[2] == kVTVideoDecoderBadDataErr && emitted_flags[2] == kVTDecodeInfo_FrameDropped);

	/* Invalidation completes a delayed token once before clearing its handle. */
	sim_delay_mode = 1;
	assert(vt->derived_class->decode_frame(dec_obj, (VTDecodeFrameRef)(uintptr_t)4, &s2, 0, NULL) == noErr);
	assert(emit_calls == 3);
	/* A rejected completion must not consume the older delayed token or emit twice. */
	sim_emit_status = kVTVideoDecoderMalfunctionErr;
	assert(vt->derived_class->decode_frame(dec_obj, (VTDecodeFrameRef)(uintptr_t)5, &s2, 0, NULL) == sim_emit_status);
	sim_emit_status = noErr;
	assert(emit_calls == 4 && emitted_frames[3] == (VTDecodeFrameRef)(uintptr_t)5);

	/* Poll failure completes the older delayed frame, preserving the current completion. */
	sim_fail_poll = true;
	assert(vt->derived_class->decode_frame(dec_obj, (VTDecodeFrameRef)(uintptr_t)6, &s2, 0, NULL) == kVTVideoDecoderBadDataErr);
	sim_fail_poll = false;
	assert(emit_calls == 6 && emitted_frames[4] == (VTDecodeFrameRef)(uintptr_t)6);
	assert(emitted_statuses[4] == noErr && emitted_frames[5] == (VTDecodeFrameRef)(uintptr_t)4);
	assert(emitted_statuses[5] == kVTVideoDecoderBadDataErr && emitted_flags[5] == kVTDecodeInfo_FrameDropped);
	for (uint32_t i = 0; i < IRIS_VT_PENDING_MAX; i++) assert(!storage->pending[i].in_use);
	sim_delay_mode = 1;
	assert(vt->derived_class->decode_frame(dec_obj, (VTDecodeFrameRef)(uintptr_t)7, &s2, 0, NULL) == noErr);

	/* HVCC parser sanity */
	uint8_t hvcc[32] = { 0 };
	hvcc[0] = 1; hvcc[21] = 0xff; hvcc[22] = 1;
	hvcc[23] = 32; hvcc[24] = 0; hvcc[25] = 1; hvcc[26] = 0; hvcc[27] = 3; hvcc[28] = 0x40; hvcc[29] = 0x01; hvcc[30] = 0x0c;
	uint8_t nls = 0, ab[64]; size_t ab_len = 0;
	assert(iris_parse_hvcc(hvcc, 31, &nls, ab, sizeof(ab), &ab_len) && nls == 4 && ab_len == 7);

	assert(vt->base_class->invalidate(dec_obj) == noErr);
	assert(emit_calls == 7 && emitted_frames[6] == (VTDecodeFrameRef)(uintptr_t)7);
	assert(emitted_statuses[6] == kVTVideoDecoderMalfunctionErr);
	assert(emitted_flags[6] == kVTDecodeInfo_FrameDropped);
	assert(storage->session == NULL && storage->fd == -1 && storage->session_id == 0);
	assert(vt->base_class->invalidate(dec_obj) == noErr && emit_calls == 7);
	vt->base_class->finalize(dec_obj);
	CFRelease(dec_obj);
	assert(cf_live_objects == 0 && memcmp(session_words, original_words, sizeof(session_words)) == 0);
	test_pending_tokens();
	puts("PASS: Iris VideoToolbox gating, vtable layout, borrowed handles, frame tokens, Annex-B conversion, and B-frame drain verified");
	return 0;
}
'''


def run_kernel(kernel_body: str, directory: Path, label: str) -> subprocess.CompletedProcess:
    src = directory / f'{label}.c'
    filtered = '\n'.join(
        line for line in kernel_body.splitlines()
        if not line.startswith('#include "annibale_iris_private.h"')
    )
    src.write_text(KERNEL_HARNESS.replace('__IRIS_KERNEL_BODY__', filtered))
    binary = directory / label
    subprocess.run([
        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(ROOT / 'xnu/files/bsd'),
        '-I' + str(ROOT / 'xnu/files/bsd/dev/annibale'),
        str(src), '-o', str(binary),
    ], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def run_vt(directory: Path, vt_code=None, args=(), label='vt_test') -> subprocess.CompletedProcess:
    src = directory / f'{label}.c'
    # Include videotoolbox_iris.c without iris_sys_ioctl forward issue by splitting before iris_sys_ioctl's callers
    if vt_code is None:
        vt_code = VT_SRC.read_text()
    vt_header = 'static int iris_sys_ioctl(int fd, unsigned long cmd, void *arg);\n'
    patched_vt = directory / f'{label}_source.h'
    patched_vt.write_text(vt_header + vt_code)
    src.write_text(VT_HARNESS.replace('__VT_SOURCE__', str(patched_vt)))
    binary = directory / label
    subprocess.run([
        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-four-char-constants',
        str(src), '-lpthread', '-o', str(binary),
    ], check=True)
    return subprocess.run([str(binary), *args], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='iris-safety-') as tmp:
    directory = Path(tmp)
    kernel_text = KERNEL_SRC.read_text()
    res = run_kernel(kernel_text, directory, 'production')
    assert res.returncode == 0, res.stderr
    print(res.stdout.strip())

    mutations = {
        'init_hardware': ('initialized = true;', 'initialized = true; hardware_accesses++;'),
        'open_hardware': ('return 0;\n}', 'hardware_accesses++; return 0;\n}'),
        'null_ioctl_pass': ('return EINVAL;', 'return 0;'),
        'unknown_ioctl_pass': ('return ENOTTY;', 'return 0;'),
        'reserve_before_open': ('slot->active = true;', 'slot->active = false;'),
        'open_retained': ('if (cleanup == 0) memset(slot, 0, sizeof(*slot));',
                          'if (true) memset(slot, 0, sizeof(*slot));'),
        'close_retained': ('if (err == 0) memset(s, 0, sizeof(*s));',
                           'if (true) memset(s, 0, sizeof(*s));'),
        'device_retained': ('if (result == 0) memset(s, 0, sizeof(*s));',
                            'if (true) memset(s, 0, sizeof(*s));'),
    }
    for label, (old, new) in mutations.items():
        assert old in kernel_text, label
        mres = run_kernel(kernel_text.replace(old, new, 1), directory, label)
        assert mres.returncode != 0, label
    print(f'PASS: {len(mutations)} compiled kernel regressions are rejected')

    vt_res = run_vt(directory)
    assert vt_res.returncode == 0, vt_res.stderr
    for stage in ('create codec=', 'create class=', 'create result=', 'start decoder=', 'start result=', 'decode session_id='):
        assert any(line.startswith('iris_vt[') and line.split(']', 1)[0][8:].isdigit() and stage in line
                   for line in vt_res.stderr.splitlines()), stage
    failures = [line for line in vt_res.stderr.splitlines() if 'decode ioctl failed ' in line]
    assert len(failures) == 1, failures
    assert 'sid=1 mode=0x0 tag=3 bytes=6 errno=5' in failures[0], failures
    print(vt_res.stdout.strip())

    vt_code = VT_SRC.read_text()
    public_code = (ROOT / 'xnu/files/bsd/sys/annibale_iris.h').read_text()
    for name in ('DECODED', 'CORRUPT', 'NEED_MORE', 'DRAIN', 'POLL'):
        definition = rf'^#define IRIS_FRAME_{name}\s+(0x[0-9a-f]+u)'
        assert re.search(definition, vt_code, re.M).group(1) == re.search(definition, public_code, re.M).group(1)
    corrupt_res = run_vt(directory, args=('corrupt',), label='vt_corrupt')
    assert corrupt_res.returncode == 0, corrupt_res.stderr
    print(corrupt_res.stdout.strip())
    vt_mutations = {
        'corrupt_ignored': ('bool corrupt = (req.flags & IRIS_FRAME_CORRUPT) != 0;', 'bool corrupt = false;'),
        'corrupt_success': ('corrupt ? kVTVideoDecoderBadDataErr : noErr,', 'noErr,'),
        'corrupt_image': ('corrupt ? NULL : pixbuf', 'pixbuf'),
        'corrupt_current_tag': ('iris_pending_pop(dec, req.out_tag)', 'iris_pending_pop(dec, in_tag)'),
        'zero_oldest_tag': ('if (tag == 0) return NULL;', 'if (tag == 0) return iris_pending_pop(dec, dec->pending[0].tag);'),
        'corrupt_step_error': ('*out_emitted = true;', '*out_emitted = true; if (corrupt) status = kVTVideoDecoderBadDataErr;'),
        'flush_error_ignored': ('if (iris_sys_ioctl(dec->fd, IRIS_IOC_FLUSH_SESSION, &sid) != 0 && status == noErr)\n\t\t\tstatus = kVTVideoDecoderMalfunctionErr;',
                                '(void)iris_sys_ioctl(dec->fd, IRIS_IOC_FLUSH_SESSION, &sid);'),
        'drain_error_ignored': ('status = iris_run_decode_step_Locked(dec, NULL, NULL, 0, 0, IRIS_FRAME_DRAIN, &emitted);',
                                '(void)iris_run_decode_step_Locked(dec, NULL, NULL, 0, 0, IRIS_FRAME_DRAIN, &emitted);'),
    }
    for label, (old, new) in vt_mutations.items():
        assert old in vt_code, label
        result = run_vt(directory, vt_code.replace(old, new, 1), args=('corrupt',), label=label)
        assert result.returncode != 0, label
    print(f'PASS: {len(vt_mutations)} compiled per-frame error regressions are rejected')

assert 'videotoolbox_iris.c' in PACKAGE
assert 'libannibale-vt.dylib' in PACKAGE
print('PASS: package.py wires libannibale-vt.dylib and records videotoolbox_iris.c')

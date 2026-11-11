#!/usr/bin/env python3
"""Compile and test SM8750 Iris VPU (/dev/annibale-iris) and VideoToolbox plugin safety."""
from pathlib import Path
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
	hardware_accesses++;
	open_sessions_hw++;
	s->streaming = true;
	return 0;
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
void iris_hfi_session_close(struct iris_session *s) {
	(void)s;
	hardware_accesses++;
	close_calls_hw++;
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

static CFTypeRef CFRetain(CFTypeRef p) {
	if (p == NULL || p == kCFBooleanTrue || p == (CFTypeRef)&k_false_obj) return p;
	((struct fake_cf_obj *)p)->refcnt++;
	return p;
}
static void CFRelease(CFTypeRef p) {
	if (p == NULL || p == kCFBooleanTrue || p == (CFTypeRef)&k_false_obj) return;
	struct fake_cf_obj *o = (struct fake_cf_obj *)p;
	assert(o->refcnt > 0);
	if (--o->refcnt == 0) free(o);
}
static Boolean CFEqual(CFTypeRef a, CFTypeRef b) {
	return a && b && strcmp((const char *)a, (const char *)b) == 0;
}
static Boolean CFBooleanGetValue(CFBooleanRef b) { return b == kCFBooleanTrue; }
static CFNumberRef CFNumberCreate(CFAllocatorRef a, int t, const void *v) {
	(void)a; (void)t; (void)v;
	struct fake_cf_obj *o = calloc(1, sizeof(*o)); o->refcnt = 1; return o;
}
static CFDictionaryRef CFDictionaryCreate(CFAllocatorRef a, const void **k, const void **v, CFIndex n, const int *kc, const int *vc) {
	(void)a; (void)k; (void)v; (void)n; (void)kc; (void)vc;
	struct fake_cf_obj *o = calloc(1, sizeof(*o)); o->refcnt = 1; return o;
}
static CFStringRef CFStringCreateWithFormat(CFAllocatorRef a, CFDictionaryRef o, CFStringRef f, ...) {
	(void)a; (void)o; (void)f;
	struct fake_cf_obj *s = calloc(1, sizeof(*s)); s->refcnt = 1; return (CFStringRef)s;
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
	(void)s; assert(a != NULL); pb_attr_calls++; return noErr;
}
static OSStatus VTDecoderSessionCreatePixelBuffer(VTDecoderSessionRef s, VTDecodeFrameRef fr, CVPixelBufferRef *out) {
	(void)s; (void)fr;
	struct fake_pixbuf *pb = calloc(1, sizeof(*pb));
	pb->base.refcnt = 1;
	*out = pb;
	return noErr;
}
static CVReturn CVPixelBufferLockBaseAddress(CVPixelBufferRef pb, uint64_t f) { (void)pb; (void)f; return kCVReturnSuccess; }
static CVReturn CVPixelBufferUnlockBaseAddress(CVPixelBufferRef pb, uint64_t f) { (void)pb; (void)f; return kCVReturnSuccess; }
static void *CVPixelBufferGetBaseAddressOfPlane(CVPixelBufferRef pb, size_t idx) {
	struct fake_pixbuf *p = pb;
	return idx == 0 ? p->y : p->uv;
}
static size_t CVPixelBufferGetBytesPerRowOfPlane(CVPixelBufferRef pb, size_t idx) { (void)pb; (void)idx; return 256; }

static int emit_calls;
static VTDecodeFrameRef emitted_frames[16];
static OSStatus emitted_statuses[16];
static OSStatus VTDecoderSessionEmitDecodedFrame(VTDecoderSessionRef s, VTDecodeFrameRef fr,
    OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img) {
	(void)s; (void)fl; (void)img;
	emitted_frames[emit_calls] = fr;
	emitted_statuses[emit_calls] = st;
	emit_calls++;
	return noErr;
}

static bool sim_dev_exists;
static uint32_t sim_info_flags;
static int sim_delay_mode;
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
		struct iris_decode_frame *req = arg;
		if ((req->flags & IRIS_FRAME_POLL) != 0) {
			req->flags = 0;
			return 0;
		}
		if ((req->flags & IRIS_FRAME_DRAIN) != 0) {
			if (delayed_tag != 0) {
				req->out_tag = delayed_tag;
				delayed_tag = 0;
				req->flags = IRIS_FRAME_DECODED;
			} else {
				req->flags = 0;
			}
			return 0;
		}
		assert(req->input_len <= sizeof(last_submitted_bs));
		memcpy(last_submitted_bs, (const void *)(uintptr_t)req->input_user_addr, req->input_len);
		last_submitted_len = req->input_len;
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
		return 0;
	}
	return -1;
}

int main(void) {
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
	struct fake_cf_obj sess = { .refcnt = 1 };
	assert(vt->derived_class->start_session(dec_obj, &sess, &fmt) == noErr);
	assert(pb_attr_calls == 1);
	assert(vt->derived_class->can_accept_format(dec_obj, &fmt));

	/* 5. Corrupt zero-length NALU is rejected with BadDataErr (-12909) */
	static const uint8_t bad_nal[] = { 0x00, 0x00, 0x00, 0x00 };
	struct fake_sample bad_sample = { bad_nal, sizeof(bad_nal), false };
	struct fake_cf_obj fref_bad = { .refcnt = 1 };
	assert(vt->derived_class->decode_frame(dec_obj, &fref_bad, &bad_sample, 0, NULL) == kVTVideoDecoderBadDataErr);

	/* 6. Valid AVCC IDR + B-frame reorder + drain */
	static const uint8_t idr_nal[] = { 0x00, 0x00, 0x00, 0x03, 0x65, 0x88, 0x84 };
	static const uint8_t b_nal[]   = { 0x00, 0x00, 0x00, 0x02, 0x01, 0x9a };
	struct fake_sample s1 = { idr_nal, sizeof(idr_nal), false };
	struct fake_sample s2 = { b_nal, sizeof(b_nal), true };
	struct fake_cf_obj fref1 = { .refcnt = 1 }, fref2 = { .refcnt = 1 };

	sim_delay_mode = 1;
	assert(vt->derived_class->decode_frame(dec_obj, &fref1, &s1, 0, NULL) == noErr);
	/* First sync sample prepended Annex-B SPS (8 B) + PPS (8 B) + IDR (7 B) = 23 B */
	assert(last_submitted_len == 23);
	assert(memcmp(last_submitted_bs, "\x00\x00\x00\x01\x67\x42\x00\x1e\x00\x00\x00\x01\x68\xce\x3c\x80\x00\x00\x00\x01\x65\x88\x84", 23) == 0);
	assert(emit_calls == 0);

	assert(vt->derived_class->decode_frame(dec_obj, &fref2, &s2, 0, NULL) == noErr);
	/* Non-sync sample omits SPS/PPS: 4 + 2 = 6 B, emits fref2 while fref1 stays in DPB */
	assert(last_submitted_len == 6);
	assert(emit_calls == 1 && emitted_frames[0] == &fref2 && emitted_statuses[0] == noErr);

	/* Drain flushes delayed fref1 */
	assert(vt->derived_class->finish_delayed_frames(dec_obj) == noErr);
	assert(emit_calls == 2 && emitted_frames[1] == &fref1 && emitted_statuses[1] == noErr);
	assert(fref1.refcnt == 1 && fref2.refcnt == 1);

	/* HVCC parser sanity */
	uint8_t hvcc[32] = { 0 };
	hvcc[0] = 1; hvcc[21] = 0xff; hvcc[22] = 1;
	hvcc[23] = 32; hvcc[24] = 0; hvcc[25] = 1; hvcc[26] = 0; hvcc[27] = 3; hvcc[28] = 0x40; hvcc[29] = 0x01; hvcc[30] = 0x0c;
	uint8_t nls = 0, ab[64]; size_t ab_len = 0;
	assert(iris_parse_hvcc(hvcc, 31, &nls, ab, sizeof(ab), &ab_len) && nls == 4 && ab_len == 7);

	assert(vt->base_class->invalidate(dec_obj) == noErr);
	vt->base_class->finalize(dec_obj);
	CFRelease(dec_obj);
	puts("PASS: Iris VideoToolbox gating, PAC vtable, Annex-B conversion, and B-frame drain verified");
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


def run_vt(directory: Path) -> subprocess.CompletedProcess:
    src = directory / 'vt_test.c'
    # Include videotoolbox_iris.c without iris_sys_ioctl forward issue by splitting before iris_sys_ioctl's callers
    vt_code = VT_SRC.read_text()
    vt_header = 'static int iris_sys_ioctl(int fd, unsigned long cmd, void *arg);\n'
    patched_vt = directory / 'videotoolbox_iris_under_test.h'
    patched_vt.write_text(vt_header + vt_code)
    src.write_text(VT_HARNESS.replace('__VT_SOURCE__', str(patched_vt)))
    binary = directory / 'vt_test'
    subprocess.run([
        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-four-char-constants',
        str(src), '-lpthread', '-o', str(binary),
    ], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


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
    }
    for label, (old, new) in mutations.items():
        assert old in kernel_text, label
        mres = run_kernel(kernel_text.replace(old, new, 1), directory, label)
        assert mres.returncode != 0, label
    print(f'PASS: {len(mutations)} compiled kernel regressions are rejected')

    vt_res = run_vt(directory)
    assert vt_res.returncode == 0, vt_res.stderr
    print(vt_res.stdout.strip())

assert 'videotoolbox_iris.c' in PACKAGE
assert 'libannibale-vt.dylib' in PACKAGE
print('PASS: package.py wires libannibale-vt.dylib and records videotoolbox_iris.c')

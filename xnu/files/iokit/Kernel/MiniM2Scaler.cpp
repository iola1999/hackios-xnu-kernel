/*
 * MiniM2Scaler: stand-in for AppleM2ScalerCSCDriver (IOSurfaceAccelerator.framework).
 *
 * Camera (BWPhotonicEngineNode / NRFV4 PyramidStage_NRF / CMIPost), AppleJPEG, CMPhoto and
 * VideoToolbox open AppleM2ScalerCSCDriver to crop, scale, rotate and colour-convert IOSurfaces
 * synchronously or with IONotificationPort async completion and IOSurfaceSharedEvent fences.
 * All arithmetic uses integer fixed-point so it compiles cleanly under -mgeneral-regs-only.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <sys/systm.h>

#include "MiniDrivers.h"
#include "MiniIOSurface.h"

#define FOURCC(a, b, c, d) \
	((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

enum {
	kM2MethodCaptureSurface         = 0,
	kM2MethodTransformSurface       = 1,
	kM2MethodAbortTransfers         = 2,
	kM2MethodAbortCaptures          = 3,
	kM2MethodSetCustomFilter        = 4,
	kM2MethodKernelTests            = 6,
	kM2MethodGetHistogram           = 7,
	kM2MethodGetTransformEstimation = 9,
	kM2MethodSetProperty            = 10,
};

enum {
	kM2FlagFlipH             = 1ULL << 0,
	kM2FlagFlipV             = 1ULL << 1,
	kM2FlagRotate90          = 1ULL << 2, /* kIOSurfaceAcceleratorSymmetricTransformKey: CW 90° */
	kM2FlagNearestFilter     = 1ULL << 7,
	kM2FlagSkipAsyncCallback = 1ULL << 10,
	kM2FlagAlphaOverride     = 1ULL << 18,
	kM2FlagZeroFill          = 1ULL << 30,
	kM2FlagWriteOnly         = 1ULL << 49,
};

struct m2scaler_req {
	uint32_t srcSurfaceID;      /* 0x00 */
	uint32_t dstSurfaceID;      /* 0x04 */
	uint64_t callback;          /* 0x08 */
	uint64_t refcon1;           /* 0x10 */
	uint64_t refcon2;           /* 0x18 */
	uint64_t flags;             /* 0x20 */
	uint64_t srcXFixed16;       /* 0x28 */
	uint64_t srcYFixed16;       /* 0x30 */
	uint64_t srcWFixed16;       /* 0x38 */
	uint64_t srcHFixed16;       /* 0x40 */
	uint32_t fullSrcW;          /* 0x48 */
	uint32_t fullSrcH;          /* 0x4c */
	uint64_t reserved50;        /* 0x50 */
	uint64_t reserved58;        /* 0x58 */
	uint32_t dstX;              /* 0x60 */
	uint32_t dstY;              /* 0x64 */
	uint32_t dstW;              /* 0x68 */
	uint32_t dstH;              /* 0x6c */
	uint32_t fullDstW;          /* 0x70 */
	uint32_t fullDstH;          /* 0x74 */
	uint32_t srcEventPort;      /* 0x78 */
	uint32_t pad7c;             /* 0x7c */
	uint64_t srcEventWait;      /* 0x80 */
	uint64_t srcEventSignal;    /* 0x88 */
	uint32_t dstEventPort;      /* 0x90 */
	uint32_t pad94;             /* 0x94 */
	uint64_t dstEventWait;      /* 0x98 */
	uint64_t dstEventSignal;    /* 0xa0 */
	uint32_t alphaOverride;     /* 0xa8 */
	uint8_t  reservedAC[0x30];  /* 0xac..0xdb */
	uint32_t histBinMode;       /* 0xdc */
	uint32_t histOffsetX;       /* 0xe0 */
	uint32_t histOffsetY;       /* 0xe4 */
	uint32_t histWidth;         /* 0xe8 */
	uint32_t histHeight;        /* 0xec */
};

static_assert(offsetof(struct m2scaler_req, dstX) == 0x60, "m2scaler dstX offset");
static_assert(offsetof(struct m2scaler_req, srcEventPort) == 0x78, "m2scaler srcEvent offset");
static_assert(offsetof(struct m2scaler_req, alphaOverride) == 0xa8, "m2scaler alpha offset");
static_assert(offsetof(struct m2scaler_req, histBinMode) == 0xdc, "m2scaler histBinMode offset");
static_assert(offsetof(struct m2scaler_req, histHeight) == 0xec, "m2scaler histHeight offset");

static inline int32_t
clampS32(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline bool
m2IsBiplanarYUV(uint32_t fmt, uint32_t * hsub, uint32_t * vsub, bool * wide, bool * full)
{
	switch (fmt) {
	case FOURCC('4', '2', '0', 'v'):
		*hsub = 2; *vsub = 2; *wide = false; *full = false; return true;
	case FOURCC('4', '2', '0', 'f'):
		*hsub = 2; *vsub = 2; *wide = false; *full = true; return true;
	case FOURCC('4', '2', '2', 'v'):
		*hsub = 2; *vsub = 1; *wide = false; *full = false; return true;
	case FOURCC('4', '2', '2', 'f'):
		*hsub = 2; *vsub = 1; *wide = false; *full = true; return true;
	case FOURCC('x', '4', '2', '0'):
		*hsub = 2; *vsub = 2; *wide = true; *full = false; return true;
	case FOURCC('x', 'f', '2', '0'):
		*hsub = 2; *vsub = 2; *wide = true; *full = true; return true;
	case FOURCC('x', '4', '2', '2'):
		*hsub = 2; *vsub = 1; *wide = true; *full = false; return true;
	case FOURCC('x', 'f', '2', '2'):
		*hsub = 2; *vsub = 1; *wide = true; *full = true; return true;
	default:
		return false;
	}
}

static uint8_t *
m2PlaneBase(MiniIOSurface * s, uint32_t p, uint32_t * outW, uint32_t * outH,
    uint32_t * outBPR, uint32_t * outBPE)
{
	if (s == NULL || s->kpixels == NULL || p >= MINISURF_MAX_PLANES) {
		return NULL;
	}
	uint32_t pcount = s->info.planeCount ? s->info.planeCount : 1;
	if (p >= pcount) {
		return NULL;
	}
	const struct minisurf_plane * pl = &s->planes[p];
	uint32_t w = pl->width ? pl->width : s->info.width;
	uint32_t h = pl->height ? pl->height : s->info.height;
	uint32_t bpe = pl->bytesPerElement ? pl->bytesPerElement : s->info.bytesPerElement;
	uint32_t bpr = pl->bytesPerRow ? pl->bytesPerRow : s->info.bytesPerRow;
	uint32_t off = (s->info.planeCount == 0 ? s->info.offset : 0) + pl->offset;

	if (w == 0 || h == 0 || bpe == 0 || bpr < w * bpe) {
		return NULL;
	}
	uint64_t end = (uint64_t) off + (uint64_t) (h - 1) * bpr + (uint64_t) w * bpe;
	if (end > s->info.allocSize) {
		return NULL;
	}
	*outW = w;
	*outH = h;
	*outBPR = bpr;
	*outBPE = bpe;
	return s->kpixels + off;
}

/* Pure-integer IEEE-754 binary16 <-> [0..65535] unorm helpers for L00h/1C0h/RGhA surfaces. */
static inline uint16_t
halfToUnorm16(uint16_t h)
{
	if ((h & 0x8000u) != 0 || (h & 0x7fffu) == 0) {
		return 0;
	}
	uint32_t exp = (h >> 10) & 0x1fu;
	uint32_t mant = h & 0x3ffu;
	if (exp >= 15u) {
		return 65535u;
	}
	uint32_t sig = (exp == 0) ? (mant << 1) : (0x400u | mant);
	uint32_t shift = 15u - (exp == 0 ? 1u : exp);
	uint32_t v = ((sig * 65535u) + (1u << (9u + shift))) >> (10u + shift);
	return (uint16_t) (v > 65535u ? 65535u : v);
}

static inline uint16_t
unorm16ToHalf(uint16_t u)
{
	if (u == 0) {
		return 0;
	}
	if (u >= 65535u) {
		return 0x3c00u;
	}
	uint32_t v = ((uint32_t) u << 16) / 65535u;
	uint32_t exp = 14u;
	while ((v & 0x8000u) == 0 && exp > 0) {
		v <<= 1;
		exp--;
	}
	uint32_t mant = (v >> 5) & 0x3ffu;
	if ((v & 0x10u) != 0) {
		mant++;
		if (mant == 0x400u) {
			mant = 0;
			exp++;
		}
	}
	return (uint16_t) ((exp << 10) | mant);
}

static inline bool
m2Aborted(const volatile uint32_t * abortGen, uint32_t startGen)
{
	return abortGen != NULL && __atomic_load_n(abortGen, __ATOMIC_ACQUIRE) != startGen;
}

/* Fast integer bilinear/nearest plane resampler for 1- or 2-component 8/16-bit YUV planes. */
static bool
m2ScalePlane(const uint8_t * srcBase, uint32_t srcW, uint32_t srcH, uint32_t srcBPR, bool srcWide,
    uint8_t * dstBase, uint32_t dstW, uint32_t dstH, uint32_t dstBPR, bool dstWide,
    uint32_t channels, uint64_t sx16, uint64_t sy16, uint64_t sw16, uint64_t sh16,
    uint32_t dx0, uint32_t dy0, uint32_t dw, uint32_t dh, bool nearest, uint32_t xform,
    const volatile uint32_t * abortGen, uint32_t startGen)
{
	if (dw == 0 || dh == 0 || srcW == 0 || srcH == 0 || dx0 >= dstW || dy0 >= dstH) {
		return true;
	}
	if (dw > dstW - dx0) {
		dw = dstW - dx0;
	}
	if (dh > dstH - dy0) {
		dh = dstH - dy0;
	}

	bool rot90 = (xform & kM2FlagRotate90) != 0;
	bool flipH = (xform & kM2FlagFlipH) != 0;
	bool flipV = (xform & kM2FlagFlipV) != 0;
	uint32_t denomX = rot90 ? dh : dw;
	uint32_t denomY = rot90 ? dw : dh;

	/* Fast 1:1 rotated/flipped plane copy without per-pixel division or RGB round-trip. */
	if (xform != 0 && srcWide == dstWide &&
	    (sx16 & 0xffffu) == 0 && (sy16 & 0xffffu) == 0 &&
	    (sw16 & 0xffffu) == 0 && (sh16 & 0xffffu) == 0 &&
	    (sw16 >> 16) == denomX && (sh16 >> 16) == denomY) {
		uint32_t sx0 = (uint32_t) (sx16 >> 16);
		uint32_t sy0 = (uint32_t) (sy16 >> 16);
		if (sx0 + denomX <= srcW && sy0 + denomY <= srcH) {
			uint32_t elemBytes = channels * (srcWide ? 2u : 1u);
			for (uint32_t y = 0; y < dh; y++) {
				if (m2Aborted(abortGen, startGen)) {
					return false;
				}
				uint32_t uy = flipV ? (dh - 1 - y) : y;
				uint8_t * drow = dstBase + (size_t) (dy0 + y) * dstBPR + (size_t) dx0 * elemBytes;
				if (!rot90) {
					const uint8_t * srow = srcBase + (size_t) (sy0 + uy) * srcBPR;
					if (!flipH) {
						bcopy(srow + (size_t) sx0 * elemBytes, drow, (size_t) dw * elemBytes);
					} else if (elemBytes == 1u) {
						const uint8_t * s = srow + sx0 + dw - 1;
						for (uint32_t x = 0; x < dw; x++) drow[x] = s[-(intptr_t) x];
					} else if (elemBytes == 2u) {
						const uint16_t * s = (const uint16_t *) (srow + (size_t) (sx0 + dw - 1) * 2u);
						uint16_t * d = (uint16_t *) drow;
						for (uint32_t x = 0; x < dw; x++) d[x] = s[-(intptr_t) x];
					} else {
						const uint32_t * s = (const uint32_t *) (srow + (size_t) (sx0 + dw - 1) * 4u);
						uint32_t * d = (uint32_t *) drow;
						for (uint32_t x = 0; x < dw; x++) d[x] = s[-(intptr_t) x];
					}
				} else {
					/* CW 90° maps dst(x,y) -> src(uy, dw-1-ux); flipH reverses ux. */
					const uint8_t * scol = srcBase + (size_t) (sx0 + uy) * elemBytes;
					bool revY = !flipH;
					intptr_t step = revY ? -(intptr_t) srcBPR : (intptr_t) srcBPR;
					const uint8_t * sp = scol + (size_t) (sy0 + (revY ? (dw - 1) : 0)) * srcBPR;
					if (elemBytes == 1u) {
						for (uint32_t x = 0; x < dw; x++, sp += step) drow[x] = *sp;
					} else if (elemBytes == 2u) {
						uint16_t * d = (uint16_t *) drow;
						for (uint32_t x = 0; x < dw; x++, sp += step) d[x] = *(const uint16_t *) sp;
					} else {
						uint32_t * d = (uint32_t *) drow;
						for (uint32_t x = 0; x < dw; x++, sp += step) d[x] = *(const uint32_t *) sp;
					}
				}
			}
			return true;
		}
	}

	uint64_t stepX = sw16 / denomX;
	uint64_t stepY = sh16 / denomY;

	if (xform != 0) {
		for (uint32_t y = 0; y < dh; y++) {
			if (m2Aborted(abortGen, startGen)) {
				return false;
			}
			uint32_t uy = flipV ? (dh - 1 - y) : y;
			uint8_t * drow = dstBase + (size_t) (dy0 + y) * dstBPR;
			for (uint32_t x = 0; x < dw; x++) {
				uint32_t ux = flipH ? (dw - 1 - x) : x;
				uint32_t tx = rot90 ? uy : ux;
				uint32_t ty = rot90 ? (dw - 1 - ux) : uy;
				uint32_t ix = (uint32_t) ((sx16 + tx * stepX) >> 16);
				uint32_t iy = (uint32_t) ((sy16 + ty * stepY) >> 16);
				if (ix >= srcW) ix = srcW - 1;
				if (iy >= srcH) iy = srcH - 1;
				const uint8_t * srow = srcBase + (size_t) iy * srcBPR;
				for (uint32_t c = 0; c < channels; c++) {
					uint32_t val16 = srcWide ?
					    ((const uint16_t *) srow)[ix * channels + c] :
					    ((uint32_t) srow[ix * channels + c] << 8);
					if (dstWide) {
						((uint16_t *) drow)[(dx0 + x) * channels + c] = (uint16_t) val16;
					} else {
						drow[(dx0 + x) * channels + c] = (uint8_t) (val16 >> 8);
					}
				}
			}
		}
		return true;
	}

	for (uint32_t y = 0; y < dh; y++) {
		if (m2Aborted(abortGen, startGen)) {
			return false;
		}
		uint64_t fy = sy16 + (nearest ? (y * stepY + (stepY >> 1)) : (y * stepY));
		uint32_t y0 = (uint32_t) (fy >> 16);
		if (y0 >= srcH) {
			y0 = srcH - 1;
		}
		uint32_t y1 = (!nearest && y0 + 1 < srcH) ? (y0 + 1) : y0;
		uint32_t wy = nearest ? 0 : (uint32_t) ((fy >> 8) & 0xffu);
		const uint8_t * srow0 = srcBase + (size_t) y0 * srcBPR;
		const uint8_t * srow1 = srcBase + (size_t) y1 * srcBPR;
		uint8_t * drow = dstBase + (size_t) (dy0 + y) * dstBPR;

		for (uint32_t x = 0; x < dw; x++) {
			uint64_t fx = sx16 + (nearest ? (x * stepX + (stepX >> 1)) : (x * stepX));
			uint32_t x0 = (uint32_t) (fx >> 16);
			if (x0 >= srcW) {
				x0 = srcW - 1;
			}
			uint32_t x1 = (!nearest && x0 + 1 < srcW) ? (x0 + 1) : x0;
			uint32_t wx = nearest ? 0 : (uint32_t) ((fx >> 8) & 0xffu);
			uint32_t w00 = (256u - wx) * (256u - wy);
			uint32_t w10 = wx * (256u - wy);
			uint32_t w01 = (256u - wx) * wy;
			uint32_t w11 = wx * wy;

			for (uint32_t c = 0; c < channels; c++) {
				uint32_t v00, v10, v01, v11;
				if (srcWide) {
					v00 = ((const uint16_t *) srow0)[x0 * channels + c];
					v10 = ((const uint16_t *) srow0)[x1 * channels + c];
					v01 = ((const uint16_t *) srow1)[x0 * channels + c];
					v11 = ((const uint16_t *) srow1)[x1 * channels + c];
				} else {
					v00 = (uint32_t) srow0[x0 * channels + c] << 8;
					v10 = (uint32_t) srow0[x1 * channels + c] << 8;
					v01 = (uint32_t) srow1[x0 * channels + c] << 8;
					v11 = (uint32_t) srow1[x1 * channels + c] << 8;
				}
				uint32_t val16 = (uint32_t) (((uint64_t) v00 * w00 + (uint64_t) v10 * w10 +
				    (uint64_t) v01 * w01 + (uint64_t) v11 * w11 + 32768ULL) >> 16);
				if (val16 > 65535u) {
					val16 = 65535u;
				}
				if (dstWide) {
					((uint16_t *) drow)[(dx0 + x) * channels + c] = (uint16_t) val16;
				} else {
					drow[(dx0 + x) * channels + c] = (uint8_t) ((val16 + 128u) >> 8);
				}
			}
		}
	}
	return true;
}

/* Read one RGBA16 [0..65535] pixel from any supported single-plane or biplanar IOSurface. */
static void
m2SampleRGBA16(uint32_t fmt, const uint8_t * p0, uint32_t w0, uint32_t h0, uint32_t bpr0,
    const uint8_t * p1, uint32_t w1, uint32_t h1, uint32_t bpr1,
    uint32_t x, uint32_t y, uint16_t out[4])
{
	if (x >= w0) x = w0 - 1;
	if (y >= h0) y = h0 - 1;

	uint32_t hsub = 1, vsub = 1;
	bool wide = false, full = true;
	if (m2IsBiplanarYUV(fmt, &hsub, &vsub, &wide, &full) && p1 != NULL) {
		uint32_t cx = (x / hsub < w1) ? (x / hsub) : (w1 - 1);
		uint32_t cy = (y / vsub < h1) ? (y / vsub) : (h1 - 1);
		int32_t y8, cb8, cr8;
		if (wide) {
			y8 = ((const uint16_t *) (p0 + (size_t) y * bpr0))[x] >> 8;
			cb8 = ((const uint16_t *) (p1 + (size_t) cy * bpr1))[cx * 2] >> 8;
			cr8 = ((const uint16_t *) (p1 + (size_t) cy * bpr1))[cx * 2 + 1] >> 8;
		} else {
			y8 = p0[(size_t) y * bpr0 + x];
			cb8 = p1[(size_t) cy * bpr1 + cx * 2];
			cr8 = p1[(size_t) cy * bpr1 + cx * 2 + 1];
		}
		int32_t r8, g8, b8;
		if (full) {
			int32_t cb = cb8 - 128, cr = cr8 - 128;
			r8 = clampS32(y8 + ((359 * cr + 128) >> 8), 0, 255);
			g8 = clampS32(y8 - ((88 * cb + 183 * cr + 128) >> 8), 0, 255);
			b8 = clampS32(y8 + ((454 * cb + 128) >> 8), 0, 255);
		} else {
			int32_t yy = 298 * (y8 - 16);
			int32_t cb = cb8 - 128, cr = cr8 - 128;
			r8 = clampS32((yy + 409 * cr + 128) >> 8, 0, 255);
			g8 = clampS32((yy - 100 * cb - 208 * cr + 128) >> 8, 0, 255);
			b8 = clampS32((yy + 516 * cb + 128) >> 8, 0, 255);
		}
		out[0] = (uint16_t) (r8 << 8);
		out[1] = (uint16_t) (g8 << 8);
		out[2] = (uint16_t) (b8 << 8);
		out[3] = 65280u;
		return;
	}

	const uint8_t * px = p0 + (size_t) y * bpr0;
	switch (fmt) {
	case FOURCC('R', 'G', 'B', 'A'): {
		const uint8_t * s = px + x * 4u;
		out[0] = (uint16_t) (s[0] << 8);
		out[1] = (uint16_t) (s[1] << 8);
		out[2] = (uint16_t) (s[2] << 8);
		out[3] = (uint16_t) (s[3] << 8);
		break;
	}
	case FOURCC('L', '0', '0', '8'):
	case FOURCC('A', '0', '0', '8'):
	case 0x00000008: {
		uint16_t v = (uint16_t) (px[x] << 8);
		out[0] = out[1] = out[2] = v;
		out[3] = (fmt == FOURCC('A', '0', '0', '8')) ? v : 65280u;
		break;
	}
	case FOURCC('L', 'A', '0', '8'): {
		const uint8_t * s = px + x * 2u;
		out[0] = (uint16_t) (s[0] << 8);
		out[1] = (uint16_t) (s[1] << 8);
		out[2] = 0;
		out[3] = 65280u;
		break;
	}
	case FOURCC('L', '0', '1', '6'): {
		uint16_t v = ((const uint16_t *) px)[x];
		out[0] = out[1] = out[2] = v;
		out[3] = 65535u;
		break;
	}
	case FOURCC('L', '0', '0', 'h'):
	case FOURCC('1', 'C', '0', 'h'): {
		uint16_t v = halfToUnorm16(((const uint16_t *) px)[x]);
		out[0] = out[1] = out[2] = v;
		out[3] = 65535u;
		break;
	}
	case FOURCC('R', 'G', 'h', 'A'): {
		const uint16_t * s = ((const uint16_t *) px) + x * 4u;
		out[0] = halfToUnorm16(s[0]);
		out[1] = halfToUnorm16(s[1]);
		out[2] = halfToUnorm16(s[2]);
		out[3] = halfToUnorm16(s[3]);
		break;
	}
	default: { /* BGRA and compatible 4-byte RGB formats */
		const uint8_t * s = px + x * 4u;
		out[0] = (uint16_t) (s[2] << 8);
		out[1] = (uint16_t) (s[1] << 8);
		out[2] = (uint16_t) (s[0] << 8);
		out[3] = (uint16_t) (s[3] << 8);
		break;
	}
	}
}

static void
m2StoreSinglePlaneRGBA16(uint32_t fmt, uint8_t * p0, uint32_t bpr0, uint32_t x, uint32_t y,
    const uint16_t rgba[4])
{
	uint8_t * px = p0 + (size_t) y * bpr0;
	switch (fmt) {
	case FOURCC('R', 'G', 'B', 'A'): {
		uint8_t * d = px + x * 4u;
		d[0] = (uint8_t) ((rgba[0] + 128u) >> 8);
		d[1] = (uint8_t) ((rgba[1] + 128u) >> 8);
		d[2] = (uint8_t) ((rgba[2] + 128u) >> 8);
		d[3] = (uint8_t) ((rgba[3] + 128u) >> 8);
		break;
	}
	case FOURCC('L', '0', '0', '8'):
	case FOURCC('A', '0', '0', '8'):
	case 0x00000008:
		px[x] = (uint8_t) ((rgba[0] + 128u) >> 8);
		break;
	case FOURCC('L', 'A', '0', '8'): {
		uint8_t * d = px + x * 2u;
		d[0] = (uint8_t) ((rgba[0] + 128u) >> 8);
		d[1] = (uint8_t) ((rgba[1] + 128u) >> 8);
		break;
	}
	case FOURCC('L', '0', '1', '6'):
		((uint16_t *) px)[x] = rgba[0];
		break;
	case FOURCC('L', '0', '0', 'h'):
	case FOURCC('1', 'C', '0', 'h'):
		((uint16_t *) px)[x] = unorm16ToHalf(rgba[0]);
		break;
	case FOURCC('R', 'G', 'h', 'A'): {
		uint16_t * d = ((uint16_t *) px) + x * 4u;
		d[0] = unorm16ToHalf(rgba[0]);
		d[1] = unorm16ToHalf(rgba[1]);
		d[2] = unorm16ToHalf(rgba[2]);
		d[3] = unorm16ToHalf(rgba[3]);
		break;
	}
	default: {
		uint8_t * d = px + x * 4u;
		d[0] = (uint8_t) ((rgba[2] + 128u) >> 8);
		d[1] = (uint8_t) ((rgba[1] + 128u) >> 8);
		d[2] = (uint8_t) ((rgba[0] + 128u) >> 8);
		d[3] = (uint8_t) ((rgba[3] + 128u) >> 8);
		break;
	}
	}
}

static IOReturn
m2ExecuteTransfer(const struct m2scaler_req * req, MiniIOSurface * src, MiniIOSurface * dst,
    const volatile uint32_t * abortGen, uint32_t startGen)
{
	uint32_t sw0 = 0, sh0 = 0, sbpr0 = 0, sbpe0 = 0;
	uint32_t dw0 = 0, dh0 = 0, dbpr0 = 0, dbpe0 = 0;
	uint8_t * sp0 = m2PlaneBase(src, 0, &sw0, &sh0, &sbpr0, &sbpe0);
	uint8_t * dp0 = m2PlaneBase(dst, 0, &dw0, &dh0, &dbpr0, &dbpe0);
	if (sp0 == NULL || dp0 == NULL) {
		return kIOReturnBadArgument;
	}
	if ((req->flags & kM2FlagZeroFill) != 0) {
		uint32_t pcount = dst->info.planeCount ? dst->info.planeCount : 1;
		for (uint32_t p = 0; p < pcount; p++) {
			uint32_t pw, ph, pbpr, pbpe;
			uint8_t * base = m2PlaneBase(dst, p, &pw, &ph, &pbpr, &pbpe);
			if (base != NULL) {
				bzero(base, (size_t) (ph - 1) * pbpr + (size_t) pw * pbpe);
			}
		}
		return kIOReturnSuccess;
	}

	uint64_t maxSX = (uint64_t) sw0 << 16;
	uint64_t maxSY = (uint64_t) sh0 << 16;
	uint64_t sx16 = req->srcXFixed16 < maxSX ? req->srcXFixed16 : 0;
	uint64_t sy16 = req->srcYFixed16 < maxSY ? req->srcYFixed16 : 0;
	uint64_t sw16 = req->srcWFixed16 ? req->srcWFixed16 : (maxSX - sx16);
	uint64_t sh16 = req->srcHFixed16 ? req->srcHFixed16 : (maxSY - sy16);
	if (sw16 > maxSX - sx16) sw16 = maxSX - sx16;
	if (sh16 > maxSY - sy16) sh16 = maxSY - sy16;

	uint32_t dx0 = req->dstX, dy0 = req->dstY;
	if (dx0 >= dw0 || dy0 >= dh0 || sw16 == 0 || sh16 == 0) {
		return kIOReturnSuccess;
	}
	uint32_t dw = req->dstW ? req->dstW : (dw0 - dx0);
	uint32_t dh = req->dstH ? req->dstH : (dh0 - dy0);
	if (dw > dw0 - dx0) dw = dw0 - dx0;
	if (dh > dh0 - dy0) dh = dh0 - dy0;

	uint32_t sfmt = src->info.pixelFormat, dfmt = dst->info.pixelFormat;
	uint32_t shsub = 1, svsub = 1, dhsub = 1, dvsub = 1;
	bool swide = false, sfull = true, dwide = false, dfull = true;
	bool sbi = m2IsBiplanarYUV(sfmt, &shsub, &svsub, &swide, &sfull);
	bool dbi = m2IsBiplanarYUV(dfmt, &dhsub, &dvsub, &dwide, &dfull);
	uint32_t sw1p = 0, sh1p = 0, sbpr1 = 0, sbpe1 = 0;
	uint32_t dw1p = 0, dh1p = 0, dbpr1 = 0, dbpe1 = 0;
	uint8_t * sp1 = sbi ? m2PlaneBase(src, 1, &sw1p, &sh1p, &sbpr1, &sbpe1) : NULL;
	uint8_t * dp1 = dbi ? m2PlaneBase(dst, 1, &dw1p, &dh1p, &dbpr1, &dbpe1) : NULL;
	if ((sbi && sp1 == NULL) || (dbi && dp1 == NULL)) {
		return kIOReturnBadArgument;
	}

	bool nearest = (req->flags & kM2FlagNearestFilter) != 0;
	uint32_t xform = (uint32_t) (req->flags & 7ULL);

	/* Fast biplanar YUV -> YUV path (covers 420f/420v/xf20/x420 pyramid, rotation & encoder transfers). */
	if (sbi && dbi && sfull == dfull) {
		if (!m2ScalePlane(sp0, sw0, sh0, sbpr0, swide, dp0, dw0, dh0, dbpr0, dwide, 1,
		    sx16, sy16, sw16, sh16, dx0, dy0, dw, dh, nearest, xform, abortGen, startGen)) {
			return kIOReturnAborted;
		}
		uint32_t cdx0 = dx0 / dhsub, cdy0 = dy0 / dvsub;
		uint32_t cdw = (dw + dhsub - 1) / dhsub, cdh = (dh + dvsub - 1) / dvsub;
		if (cdx0 < dw1p && cdy0 < dh1p) {
			if (cdw > dw1p - cdx0) cdw = dw1p - cdx0;
			if (cdh > dh1p - cdy0) cdh = dh1p - cdy0;
			if (!m2ScalePlane(sp1, sw1p, sh1p, sbpr1, swide, dp1, dw1p, dh1p, dbpr1, dwide, 2,
			    sx16 / shsub, sy16 / svsub, sw16 / shsub, sh16 / svsub,
			    cdx0, cdy0, cdw, cdh, nearest, xform, abortGen, startGen)) {
				return kIOReturnAborted;
			}
		}
		return kIOReturnSuccess;
	}

	/* Fast exact 1:1 copy for identical single-plane formats without rotation. */
	if (!sbi && !dbi && sfmt == dfmt && sbpe0 == dbpe0 && xform == 0 &&
	    (req->flags & kM2FlagAlphaOverride) == 0 &&
	    (sx16 & 0xffffu) == 0 && (sy16 & 0xffffu) == 0 &&
	    (sw16 >> 16) == dw && (sh16 >> 16) == dh) {
		uint32_t sx0 = (uint32_t) (sx16 >> 16), sy0 = (uint32_t) (sy16 >> 16);
		size_t rowBytes = (size_t) dw * dbpe0;
		for (uint32_t y = 0; y < dh; y++) {
			bcopy(sp0 + (size_t) (sy0 + y) * sbpr0 + (size_t) sx0 * sbpe0,
			    dp0 + (size_t) (dy0 + y) * dbpr0 + (size_t) dx0 * dbpe0, rowBytes);
		}
		return kIOReturnSuccess;
	}

	/* General path: handles CSC (YUV <-> BGRA/RGBA), half-float, flips, and CW rotations. */
	bool rot90 = (xform & kM2FlagRotate90) != 0;
	bool flipH = (xform & kM2FlagFlipH) != 0;
	bool flipV = (xform & kM2FlagFlipV) != 0;
	uint16_t alphaOv = (uint16_t) ((req->alphaOverride & 0xffu) << 8);
	bool hasAlphaOv = (req->flags & kM2FlagAlphaOverride) != 0;

	for (uint32_t y = 0; y < dh; y++) {
		if (m2Aborted(abortGen, startGen)) {
			return kIOReturnAborted;
		}
		for (uint32_t x = 0; x < dw; x++) {
			uint32_t ux = flipH ? (dw - 1 - x) : x;
			uint32_t uy = flipV ? (dh - 1 - y) : y;
			uint32_t tx = rot90 ? uy : ux;
			uint32_t ty = rot90 ? (dw - 1 - ux) : uy;
			uint32_t denomX = rot90 ? dh : dw;
			uint32_t denomY = rot90 ? dw : dh;
			uint64_t fx = sx16 + ((sw16 * tx) / (denomX ? denomX : 1));
			uint64_t fy = sy16 + ((sh16 * ty) / (denomY ? denomY : 1));
			uint32_t ix = (uint32_t) (fx >> 16);
			uint32_t iy = (uint32_t) (fy >> 16);
			uint16_t rgba[4];

			m2SampleRGBA16(sfmt, sp0, sw0, sh0, sbpr0, sp1, sw1p, sh1p, sbpr1, ix, iy, rgba);
			if (hasAlphaOv) {
				rgba[3] = alphaOv;
			}
			if (!dbi) {
				m2StoreSinglePlaneRGBA16(dfmt, dp0, dbpr0, dx0 + x, dy0 + y, rgba);
			} else {
				int32_t r8 = (rgba[0] + 128u) >> 8;
				int32_t g8 = (rgba[1] + 128u) >> 8;
				int32_t b8 = (rgba[2] + 128u) >> 8;
				int32_t y8, cb8, cr8;
				if (dfull) {
					y8 = clampS32((77 * r8 + 150 * g8 + 29 * b8 + 128) >> 8, 0, 255);
					cb8 = clampS32(((-43 * r8 - 85 * g8 + 128 * b8 + 128) >> 8) + 128, 0, 255);
					cr8 = clampS32(((128 * r8 - 107 * g8 - 21 * b8 + 128) >> 8) + 128, 0, 255);
				} else {
					y8 = clampS32(((66 * r8 + 129 * g8 + 25 * b8 + 128) >> 8) + 16, 16, 235);
					cb8 = clampS32(((-38 * r8 - 74 * g8 + 112 * b8 + 128) >> 8) + 128, 16, 240);
					cr8 = clampS32(((112 * r8 - 94 * g8 - 18 * b8 + 128) >> 8) + 128, 16, 240);
				}
				uint32_t ox = dx0 + x, oy = dy0 + y;
				if (dwide) {
					((uint16_t *) (dp0 + (size_t) oy * dbpr0))[ox] = (uint16_t) ((y8 << 8) | y8);
				} else {
					dp0[(size_t) oy * dbpr0 + ox] = (uint8_t) y8;
				}
				if ((ox % dhsub) == 0 && (oy % dvsub) == 0) {
					uint32_t cx = ox / dhsub, cy = oy / dvsub;
					if (cx < dw1p && cy < dh1p) {
						if (dwide) {
							uint16_t * cptr = (uint16_t *) (dp1 + (size_t) cy * dbpr1);
							cptr[cx * 2] = (uint16_t) ((cb8 << 8) | cb8);
							cptr[cx * 2 + 1] = (uint16_t) ((cr8 << 8) | cr8);
						} else {
							dp1[(size_t) cy * dbpr1 + cx * 2] = (uint8_t) cb8;
							dp1[(size_t) cy * dbpr1 + cx * 2 + 1] = (uint8_t) cr8;
						}
					}
				}
			}
		}
	}
	return kIOReturnSuccess;
}

static void
m2ComputeHistogram(const struct m2scaler_req * req, MiniIOSurface * dst, uint32_t outHist[3][128])
{
	bzero(outHist, sizeof(uint32_t) * 3 * 128);
	uint32_t dw0 = 0, dh0 = 0, dbpr0 = 0, dbpe0 = 0;
	uint8_t * dp0 = m2PlaneBase(dst, 0, &dw0, &dh0, &dbpr0, &dbpe0);
	if (dp0 == NULL || dw0 == 0 || dh0 == 0) {
		return;
	}
	uint32_t hx0 = req->histOffsetX < dw0 ? req->histOffsetX : 0;
	uint32_t hy0 = req->histOffsetY < dh0 ? req->histOffsetY : 0;
	uint32_t hw = req->histWidth ? req->histWidth : (dw0 - hx0);
	uint32_t hh = req->histHeight ? req->histHeight : (dh0 - hy0);
	if (hw > dw0 - hx0) hw = dw0 - hx0;
	if (hh > dh0 - hy0) hh = dh0 - hy0;

	uint32_t dhsub = 1, dvsub = 1;
	bool dwide = false, dfull = true;
	bool dbi = m2IsBiplanarYUV(dst->info.pixelFormat, &dhsub, &dvsub, &dwide, &dfull);
	uint32_t dw1p = 0, dh1p = 0, dbpr1 = 0, dbpe1 = 0;
	uint8_t * dp1 = dbi ? m2PlaneBase(dst, 1, &dw1p, &dh1p, &dbpr1, &dbpe1) : NULL;

	for (uint32_t y = hy0; y < hy0 + hh; y++) {
		for (uint32_t x = hx0; x < hx0 + hw; x++) {
			if (dbi && dp1 != NULL) {
				uint8_t y8 = dwide ?
				    (uint8_t) (((const uint16_t *) (dp0 + (size_t) y * dbpr0))[x] >> 8) :
				    dp0[(size_t) y * dbpr0 + x];
				uint32_t cx = (x / dhsub < dw1p) ? (x / dhsub) : (dw1p - 1);
				uint32_t cy = (y / dvsub < dh1p) ? (y / dvsub) : (dh1p - 1);
				uint8_t cb8, cr8;
				if (dwide) {
					const uint16_t * cptr = (const uint16_t *) (dp1 + (size_t) cy * dbpr1);
					cb8 = (uint8_t) (cptr[cx * 2] >> 8);
					cr8 = (uint8_t) (cptr[cx * 2 + 1] >> 8);
				} else {
					cb8 = dp1[(size_t) cy * dbpr1 + cx * 2];
					cr8 = dp1[(size_t) cy * dbpr1 + cx * 2 + 1];
				}
				outHist[0][y8 >> 1]++;
				outHist[1][cb8 >> 1]++;
				outHist[2][cr8 >> 1]++;
			} else {
				uint16_t rgba[4];
				m2SampleRGBA16(dst->info.pixelFormat, dp0, dw0, dh0, dbpr0,
				    NULL, 0, 0, 0, x, y, rgba);
				outHist[0][(rgba[0] >> 9) & 0x7fu]++;
				outHist[1][(rgba[1] >> 9) & 0x7fu]++;
				outHist[2][(rgba[2] >> 9) & 0x7fu]++;
			}
		}
	}
}

class AppleM2ScalerCSCDriver : public IOService
{
	OSDeclareDefaultStructors(AppleM2ScalerCSCDriver);
};

OSDefineMetaClassAndStructors(AppleM2ScalerCSCDriver, IOService);

class AppleM2ScalerCSCDriverUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleM2ScalerCSCDriverUserClient);

private:
	task_t          fTask;
	IOLock *        fLock;
	mach_port_t     fNotifyPort;
	bool            fClosed;
	volatile uint32_t fAbortGen;
	uint32_t        fLastHist[3][128];

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleM2ScalerCSCDriverUserClient, IOUserClient);

bool
AppleM2ScalerCSCDriverUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type,
    OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
AppleM2ScalerCSCDriverUserClient::start(IOService * provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	return fLock != NULL;
}

IOReturn
AppleM2ScalerCSCDriverUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	__atomic_add_fetch(&fAbortGen, 1u, __ATOMIC_RELEASE);
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
AppleM2ScalerCSCDriverUserClient::free(void)
{
	if (fNotifyPort != MACH_PORT_NULL) {
		releaseNotificationPort(fNotifyPort);
		fNotifyPort = MACH_PORT_NULL;
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
AppleM2ScalerCSCDriverUserClient::registerNotificationPort(mach_port_t port, UInt32 type,
    io_user_reference_t refCon __unused)
{
	if (type >= 4) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	mach_port_t old = fNotifyPort;
	fNotifyPort = port;
	IOLockUnlock(fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

IOReturn
AppleM2ScalerCSCDriverUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch __unused, OSObject * target __unused, void * reference __unused)
{
	static uint64_t sLoggedSelectors;

	switch (selector) {
	case kM2MethodAbortTransfers:
		__atomic_add_fetch(&fAbortGen, 1u, __ATOMIC_RELEASE);
		return kIOReturnSuccess;

	case kM2MethodCaptureSurface:
	case kM2MethodAbortCaptures:
	case kM2MethodSetCustomFilter:
	case kM2MethodGetTransformEstimation:
	case kM2MethodSetProperty:
		return kIOReturnSuccess;

	case kM2MethodGetHistogram: {
		if (args->structureInput == NULL || args->structureInputSize < sizeof(uint64_t)) {
			return kIOReturnBadArgument;
		}
		uint64_t userHistPtr = *(const uint64_t *) args->structureInput;
		if (userHistPtr == 0) {
			return kIOReturnBadArgument;
		}
		uint32_t tmpHist[3][128];
		IOLockLock(fLock);
		bcopy(fLastHist, tmpHist, sizeof(tmpHist));
		IOLockUnlock(fLock);
		return copyout(tmpHist, (user_addr_t) userHistPtr, sizeof(tmpHist)) == 0 ?
		    kIOReturnSuccess : kIOReturnVMError;
	}

	case kM2MethodTransformSurface: {
		if (args->structureInput == NULL || args->structureInputSize < sizeof(struct m2scaler_req)) {
			return kIOReturnBadArgument;
		}
		uint32_t startGen = __atomic_load_n(&fAbortGen, __ATOMIC_ACQUIRE);
		const struct m2scaler_req * req = (const struct m2scaler_req *) args->structureInput;
		if (!m2Aborted(&fAbortGen, startGen) && req->srcEventPort != 0 && req->srcEventWait != 0) {
			(void) MiniIOSurfaceWaitSharedEvent(fTask, req->srcEventPort, req->srcEventWait, 1000);
		}
		if (!m2Aborted(&fAbortGen, startGen) && req->dstEventPort != 0 && req->dstEventWait != 0) {
			(void) MiniIOSurfaceWaitSharedEvent(fTask, req->dstEventPort, req->dstEventWait, 1000);
		}
		IOReturn ret = kIOReturnAborted;
		if (!m2Aborted(&fAbortGen, startGen)) {
			MiniIOSurface * src = MiniIOSurfaceCopy(req->srcSurfaceID);
			MiniIOSurface * dst = MiniIOSurfaceCopy(req->dstSurfaceID);
			if (src == NULL || dst == NULL) {
				if (src != NULL) MiniIOSurfaceDrop(src);
				if (dst != NULL) MiniIOSurfaceDrop(dst);
				return kIOReturnBadArgument;
			}
			if (!m2Aborted(&fAbortGen, startGen)) {
				(void) MiniIOSurfaceDrawn(src, 250);
			}
			if (!m2Aborted(&fAbortGen, startGen)) {
				ret = m2ExecuteTransfer(req, src, dst, &fAbortGen, startGen);
			}
			if (ret == kIOReturnSuccess) {
				if (req->histBinMode != 0) {
					uint32_t hist[3][128];
					m2ComputeHistogram(req, dst, hist);
					IOLockLock(fLock);
					bcopy(hist, fLastHist, sizeof(fLastHist));
					IOLockUnlock(fLock);
				}
				__atomic_add_fetch(&dst->shared()->seed, 1u, __ATOMIC_RELEASE);
			}
			MiniIOSurfaceDrop(src);
			MiniIOSurfaceDrop(dst);
		}

		if (req->srcEventPort != 0 && req->srcEventSignal != 0) {
			(void) MiniIOSurfaceSignalSharedEvent(fTask, req->srcEventPort, req->srcEventSignal);
		}
		if (req->dstEventPort != 0 && req->dstEventSignal != 0) {
			(void) MiniIOSurfaceSignalSharedEvent(fTask, req->dstEventPort, req->dstEventSignal);
		}
		if (req->callback != 0 && (req->flags & kM2FlagSkipAsyncCallback) == 0) {
			OSAsyncReference64 ref;
			bool shouldSend = false;

			IOLockLock(fLock);
			if (!fClosed && fNotifyPort != MACH_PORT_NULL) {
				setAsyncReference64(ref, fNotifyPort, req->callback, req->refcon1, fTask);
				shouldSend = true;
			}
			IOLockUnlock(fLock);
			if (shouldSend) {
				io_user_reference_t arg = req->refcon2;
				IOUserClient::sendAsyncResult64(ref, ret, &arg, 1);
			}
		}
		return ret;
	}

	default:
		if (selector < 64 && (sLoggedSelectors & (1ULL << selector)) == 0) {
			sLoggedSelectors |= 1ULL << selector;
			IOLog("MiniM2Scaler: selector %u not implemented (in %u/%u out %u/%u)\n",
			    selector, args->scalarInputCount, args->structureInputSize,
			    args->scalarOutputCount, args->structureOutputSize);
		}
		return kIOReturnUnsupported;
	}
}

void
MiniM2ScalerStart(IOService * provider)
{
	AppleM2ScalerCSCDriver * svc = OSTypeAlloc(AppleM2ScalerCSCDriver);
	OSDictionary * caps = OSDictionary::withCapacity(48);

	if (svc == NULL || caps == NULL || !svc->init() || !svc->attach(provider)) {
		OSSafeReleaseNULL(svc);
		OSSafeReleaseNULL(caps);
		IOLog("MiniM2Scaler: cannot create AppleM2ScalerCSCDriver\n");
		return;
	}
	setNumber(caps, "IOSurfaceAcceleratorScalerVersion", 0x40007);
	setNumber(caps, "IOSurfaceAcceleratorFilterVerticalTapsCount", 4);
	setNumber(caps, "IOSurfaceAcceleratorFilterHorizontalTapsCount", 8);
	setNumber(caps, "IOSurfaceAcceleratorFilterVerticalPhasesCount", 16);
	setNumber(caps, "IOSurfaceAcceleratorFilterHorizontalPhasesCount", 16);
	setNumber(caps, "IOSurfaceAcceleratorFilterCoefficientsPrePointBits", 2);
	setNumber(caps, "IOSurfaceAcceleratorFilterCoefficientsPostPointBits", 14);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesSymmetricScaling", 1);
	setNumber(caps, "kSurfaceAcceleratorCapabilitiesHistogramValues", 128);
	setNumber(caps, "IOSurfaceAcceleratorFormatIn2Planes420", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatOut2Planes420", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatIn2Planes422", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatOut2Planes422", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatIn2Planes42010", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatOut2Planes42010", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatIn2Planes42210", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatOut2Planes42210", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatInRGBA8888", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatInLumaOnlyL008", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatOutLumaOnlyL008", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatInLumaOnlyL016", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatOutLumaOnlyL016", 1);
	setNumber(caps, "IOSurfaceAcceleratorFormatInARGB1555", 0);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMinSourceWidth", 2);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMinSourceHeight", 2);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMaxSourceWidth", 16384);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMaxSourceHeight", 16384);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMinDestWidth", 2);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMinDestHeight", 2);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMaxDestWidth", 16384);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMaxDestHeight", 16384);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMaxHUpscale", 16);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMaxHDownscale", 16);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMaxVUpscale", 16);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesMaxVDownscale", 16);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesFractionalDimensions", 1);
	setNumber(caps, "IOSurfaceAcceleratorCapabilitiesIdentityCSC", 1);
	setNumber(caps, "IOSurfaceAcceleratorHCUApiVersion", 1);
	setNumber(caps, "IOSurfaceAcceleratorHDRApiVersion", 1);
	setNumber(caps, "IOSurfaceAcceleratorASEApiVersion", 1);

	svc->setProperty(kIOUserClientClassKey, "AppleM2ScalerCSCDriverUserClient");
	svc->setProperty("IOSurfaceAcceleratorCapabilitiesDict", caps);
	caps->release();
	svc->registerService();
	svc->release();
}

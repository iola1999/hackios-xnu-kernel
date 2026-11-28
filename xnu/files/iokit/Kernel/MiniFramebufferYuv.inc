/*
 * Biplanar 8-bit YUV ('420v'/'420f') overlay scanout and multi-layer premultiplied srcOver
 * compositing for MiniIOMobileFramebuffer. Included by MiniIOMobileFramebuffer.cpp and by the
 * host unit test (MINI_FB_YUV_HOST_TEST).
 */

#ifndef _MINI_FRAMEBUFFER_YUV_H
#define _MINI_FRAMEBUFFER_YUV_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef FOURCC
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#endif

static inline int32_t
fbClampU8(int32_t v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static inline bool
fbIsBiplanarYuv8(uint32_t pixelFormat, uint32_t planeCount)
{
	return planeCount == 2 &&
	    (pixelFormat == FOURCC('4', '2', '0', 'v') || pixelFormat == FOURCC('4', '2', '0', 'f'));
}

/*
 * Convert one 8-bit YCbCr sample to opaque BGRA8888.
 * matrix uses Apple/CICP IOSurfaceYCbCrMatrix values: 1 = BT.709, 9/10 = BT.2020, 0/5/6 = BT.601.
 */
static inline uint32_t
fbYuvToBgra(uint8_t y8, uint8_t cb8, uint8_t cr8, bool fullRange, uint32_t matrix)
{
	int32_t cb = (int32_t) cb8 - 128;
	int32_t cr = (int32_t) cr8 - 128;
	int32_t r, g, b;

	if (fullRange) {
		int32_t y = (int32_t) y8;
		if (matrix == 1u) {
			r = fbClampU8(y + ((403 * cr + 128) >> 8));
			g = fbClampU8(y - ((48 * cb + 120 * cr + 128) >> 8));
			b = fbClampU8(y + ((475 * cb + 128) >> 8));
		} else if (matrix == 9u || matrix == 10u) {
			r = fbClampU8(y + ((378 * cr + 128) >> 8));
			g = fbClampU8(y - ((42 * cb + 146 * cr + 128) >> 8));
			b = fbClampU8(y + ((482 * cb + 128) >> 8));
		} else {
			r = fbClampU8(y + ((359 * cr + 128) >> 8));
			g = fbClampU8(y - ((88 * cb + 183 * cr + 128) >> 8));
			b = fbClampU8(y + ((454 * cb + 128) >> 8));
		}
	} else {
		int32_t yy = 298 * ((int32_t) y8 - 16);
		if (matrix == 1u) {
			r = fbClampU8((yy + 459 * cr + 128) >> 8);
			g = fbClampU8((yy - 55 * cb - 136 * cr + 128) >> 8);
			b = fbClampU8((yy + 541 * cb + 128) >> 8);
		} else if (matrix == 9u || matrix == 10u) {
			r = fbClampU8((yy + 430 * cr + 128) >> 8);
			g = fbClampU8((yy - 48 * cb - 166 * cr + 128) >> 8);
			b = fbClampU8((yy + 548 * cb + 128) >> 8);
		} else {
			r = fbClampU8((yy + 409 * cr + 128) >> 8);
			g = fbClampU8((yy - 100 * cb - 208 * cr + 128) >> 8);
			b = fbClampU8((yy + 516 * cb + 128) >> 8);
		}
	}
	return (uint32_t) b | ((uint32_t) g << 8) | ((uint32_t) r << 16) | 0xff000000u;
}

/* Premultiplied srcOver: out = clamp255(src + dst * (255 - srcA) / 255), BGRA8888 little-endian. */
static inline uint32_t
fbBlendPremulSrcOver(uint32_t dst, uint32_t src)
{
	uint32_t sa = src >> 24;
	if (src == 0u) {
		return dst;
	}
	if (sa == 255u) {
		return src;
	}
	uint32_t inv = 255u - sa;
	uint32_t sb = src & 0xffu, sg = (src >> 8) & 0xffu, sr = (src >> 16) & 0xffu;
	uint32_t db = dst & 0xffu, dg = (dst >> 8) & 0xffu, dr = (dst >> 16) & 0xffu, da = dst >> 24;
	uint32_t ob = sb + ((db * inv + 127u) / 255u);
	uint32_t og = sg + ((dg * inv + 127u) / 255u);
	uint32_t or_ = sr + ((dr * inv + 127u) / 255u);
	uint32_t oa = sa + ((da * inv + 127u) / 255u);
	if (ob > 255u) ob = 255u;
	if (og > 255u) og = 255u;
	if (or_ > 255u) or_ = 255u;
	if (oa > 255u) oa = 255u;
	return ob | (og << 8) | (or_ << 16) | (oa << 24);
}

struct fb_yuv420_plane_view {
	const uint8_t * yBase;
	const uint8_t * uvBase;
	uint32_t        yW, yH, yBPR;
	uint32_t        uvW, uvH, uvBPR;
	bool            fullRange;
	uint32_t        matrix;
};

/*
 * Draw a scaled biplanar 4:2:0 YUV rectangle [sx,sy,sw,sh] into BGRA8888 framebuffer
 * [px0,py0)..[px1,py1) with strict bounds clamping on both planes and destination panel.
 */
static bool
fbDrawYuv420Rect(uint8_t * dstBase, uint32_t dstW, uint32_t dstH, uint32_t dstRowBytes,
    const struct fb_yuv420_plane_view * v,
    int32_t sx, int32_t sy, int32_t sw, int32_t sh,
    int32_t px0, int32_t py0, int32_t px1, int32_t py1)
{
	if (dstBase == NULL || v == NULL || v->yBase == NULL || v->uvBase == NULL ||
	    dstW == 0 || dstH == 0 || dstRowBytes < dstW * 4u ||
	    v->yW == 0 || v->yH == 0 || v->uvW == 0 || v->uvH == 0 ||
	    v->yBPR < v->yW || v->uvBPR < v->uvW * 2u) {
		return false;
	}
	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) v->yW;
		sh = (int32_t) v->yH;
	}
	if (sx < 0 || sy < 0 || (uint32_t) sx >= v->yW || (uint32_t) sy >= v->yH) {
		return false;
	}
	if ((uint64_t) (uint32_t) sx + (uint64_t) (uint32_t) sw > v->yW) {
		sw = (int32_t) (v->yW - (uint32_t) sx);
	}
	if ((uint64_t) (uint32_t) sy + (uint64_t) (uint32_t) sh > v->yH) {
		sh = (int32_t) (v->yH - (uint32_t) sy);
	}
	if (sw <= 0 || sh <= 0 || px1 <= px0 || py1 <= py0) {
		return false;
	}

	int32_t cx0 = px0 < 0 ? 0 : px0;
	int32_t cy0 = py0 < 0 ? 0 : py0;
	int32_t cx1 = (uint32_t) px1 > dstW ? (int32_t) dstW : px1;
	int32_t cy1 = (uint32_t) py1 > dstH ? (int32_t) dstH : py1;
	if (cx1 <= cx0 || cy1 <= cy0) {
		return false;
	}

	int64_t dw = (int64_t) px1 - px0;
	int64_t dh = (int64_t) py1 - py0;

	for (int32_t y = cy0; y < cy1; y++) {
		uint32_t syRow = (uint32_t) (sy + (int64_t) (y - py0) * sh / dh);
		if (syRow >= v->yH) {
			syRow = v->yH - 1u;
		}
		uint32_t uvRow = syRow >> 1;
		if (uvRow >= v->uvH) {
			uvRow = v->uvH - 1u;
		}
		const uint8_t * yPtr = v->yBase + (size_t) syRow * v->yBPR;
		const uint8_t * uvPtr = v->uvBase + (size_t) uvRow * v->uvBPR;
		uint32_t * to = (uint32_t *) (dstBase + (size_t) y * dstRowBytes);

		for (int32_t x = cx0; x < cx1; x++) {
			uint32_t sxCol = (uint32_t) (sx + (int64_t) (x - px0) * sw / dw);
			if (sxCol >= v->yW) {
				sxCol = v->yW - 1u;
			}
			uint32_t uvCol = sxCol >> 1;
			if (uvCol >= v->uvW) {
				uvCol = v->uvW - 1u;
			}
			uint8_t y8 = yPtr[sxCol];
			uint8_t cb8 = uvPtr[uvCol * 2u];
			uint8_t cr8 = uvPtr[uvCol * 2u + 1u];
			to[x] = fbYuvToBgra(y8, cb8, cr8, v->fullRange, v->matrix);
		}
	}
	return true;
}

/*
 * Composite a single-plane 32-bit BGRA8888 (or 'l10r') rectangle over an existing BGRA8888
 * framebuffer using premultiplied srcOver.
 */
static bool
fbCompositeOverRect(uint8_t * dstBase, uint32_t dstW, uint32_t dstH, uint32_t dstRowBytes,
    const uint8_t * srcBase, uint32_t srcW, uint32_t srcH, uint32_t srcRowBytes, uint32_t pixelFormat,
    int32_t sx, int32_t sy, int32_t sw, int32_t sh,
    int32_t px0, int32_t py0, int32_t px1, int32_t py1)
{
	if (dstBase == NULL || srcBase == NULL || dstW == 0 || dstH == 0 ||
	    dstRowBytes < dstW * 4u || srcW == 0 || srcH == 0 || srcRowBytes < srcW * 4u) {
		return false;
	}
	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) srcW;
		sh = (int32_t) srcH;
	}
	if (sx < 0 || sy < 0 || (uint32_t) sx >= srcW || (uint32_t) sy >= srcH) {
		return false;
	}
	if ((uint64_t) (uint32_t) sx + (uint64_t) (uint32_t) sw > srcW) {
		sw = (int32_t) (srcW - (uint32_t) sx);
	}
	if ((uint64_t) (uint32_t) sy + (uint64_t) (uint32_t) sh > srcH) {
		sh = (int32_t) (srcH - (uint32_t) sy);
	}
	if (sw <= 0 || sh <= 0 || px1 <= px0 || py1 <= py0) {
		return false;
	}

	int32_t cx0 = px0 < 0 ? 0 : px0;
	int32_t cy0 = py0 < 0 ? 0 : py0;
	int32_t cx1 = (uint32_t) px1 > dstW ? (int32_t) dstW : px1;
	int32_t cy1 = (uint32_t) py1 > dstH ? (int32_t) dstH : py1;
	if (cx1 <= cx0 || cy1 <= cy0) {
		return false;
	}

	bool isL10r = pixelFormat == FOURCC('l', '1', '0', 'r');
	int64_t dw = (int64_t) px1 - px0;
	int64_t dh = (int64_t) py1 - py0;

	for (int32_t y = cy0; y < cy1; y++) {
		uint32_t syRow = (uint32_t) (sy + (int64_t) (y - py0) * sh / dh);
		if (syRow >= srcH) {
			syRow = srcH - 1u;
		}
		const uint32_t * from = (const uint32_t *) (srcBase + (size_t) syRow * srcRowBytes);
		uint32_t * to = (uint32_t *) (dstBase + (size_t) y * dstRowBytes);

		for (int32_t x = cx0; x < cx1; x++) {
			uint32_t sxCol = (uint32_t) (sx + (int64_t) (x - px0) * sw / dw);
			if (sxCol >= srcW) {
				sxCol = srcW - 1u;
			}
			uint32_t sp = from[sxCol];
			if (isL10r) {
				uint32_t b = ((sp & 0x3ffu) * 255u + 511u) / 1023u;
				uint32_t g = (((sp >> 10) & 0x3ffu) * 255u + 511u) / 1023u;
				uint32_t r = (((sp >> 20) & 0x3ffu) * 255u + 511u) / 1023u;
				uint32_t a = ((sp >> 30) & 0x3u) * 85u;
				sp = b | (g << 8) | (r << 16) | (a << 24);
			}
			if (sp == 0u) {
				continue;
			}
			uint32_t sa = sp >> 24;
			to[x] = (sa == 255u) ? sp : fbBlendPremulSrcOver(to[x], sp);
		}
	}
	return true;
}

#ifndef MINI_FB_YUV_HOST_TEST

static inline uint32_t
fbSurfaceYCbCrMatrix(const MiniIOSurface * s)
{
	if (s == NULL || s->kbase == NULL) {
		return 0;
	}
	if (s->sharedABI == MINISURF_ABI26) {
		return ((const uint8_t *) s->shared())[0x6e];
	}
	uint32_t v = 0;
	memcpy(&v, (const uint8_t *) s->shared() + 0x24, sizeof(v));
	return v;
}

static bool
fbValidateYuv420Surface(const MiniIOSurface * s, struct fb_yuv420_plane_view * out)
{
	if (s == NULL || s->kpixels == NULL || !fbIsBiplanarYuv8(s->info.pixelFormat, s->info.planeCount)) {
		return false;
	}
	const struct minisurf_plane * p0 = &s->planes[0];
	const struct minisurf_plane * p1 = &s->planes[1];
	uint32_t yW = p0->width ? p0->width : s->info.width;
	uint32_t yH = p0->height ? p0->height : s->info.height;
	uint32_t yBPR = p0->bytesPerRow ? p0->bytesPerRow : s->info.bytesPerRow;
	uint32_t yBPE = p0->bytesPerElement ? p0->bytesPerElement : 1u;
	uint32_t uvW = p1->width;
	uint32_t uvH = p1->height;
	uint32_t uvBPR = p1->bytesPerRow;
	uint32_t uvBPE = p1->bytesPerElement ? p1->bytesPerElement : 2u;

	if (yW == 0 || yH == 0 || uvW == 0 || uvH == 0 || yBPE != 1u || uvBPE != 2u ||
	    yBPR < yW || uvBPR < uvW * 2u) {
		return false;
	}
	uint64_t yEnd = (uint64_t) p0->offset + (uint64_t) (yH - 1u) * yBPR + yW;
	uint64_t uvEnd = (uint64_t) p1->offset + (uint64_t) (uvH - 1u) * uvBPR + (uint64_t) uvW * 2u;
	if (yEnd > s->info.allocSize || uvEnd > s->info.allocSize) {
		return false;
	}
	out->yBase = s->kpixels + p0->offset;
	out->uvBase = s->kpixels + p1->offset;
	out->yW = yW;
	out->yH = yH;
	out->yBPR = yBPR;
	out->uvW = uvW;
	out->uvH = uvH;
	out->uvBPR = uvBPR;
	out->fullRange = s->info.pixelFormat == FOURCC('4', '2', '0', 'f');
	out->matrix = fbSurfaceYCbCrMatrix(s);
	return true;
}

void
IOMobileFramebuffer::drawYuvLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst, bool clearBg)
{
	struct fb_yuv420_plane_view view;
	if (!fbValidateYuv420Surface(s, &view)) {
		return;
	}
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) view.yW;
		sh = (int32_t) view.yH;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	int32_t px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	int32_t py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	int32_t px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	int32_t py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);

	flip_dpu_scale_fallback();
	fRowHashRows = 0;
	if (clearBg && (px0 > 0 || py0 > 0 || px1 < (int32_t) fPanelWidth || py1 < (int32_t) fPanelHeight)) {
		for (uint32_t y = 0; y < fPanelHeight; y++) {
			bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4u);
		}
	}
	if (fbDrawYuv420Rect(fBase, fPanelWidth, fPanelHeight, fRowBytes, &view, sx, sy, sw, sh, px0, py0, px1, py1)) {
		flip_whole();
		fAllChangedSeq = ++fRowSeq;
		fBufferSeq[flip.ready ? (flip.shown ^ 1) : 0] = fRowSeq;
		fLastBy0 = 0; fLastBy1 = (int32_t) fPanelHeight; fLastBufSeqAfter = fRowSeq; fLastAllDirty = true;
	}
}

void
IOMobileFramebuffer::compositeLayerOver(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	if (s == NULL || s->kpixels == NULL || s->info.bytesPerElement != 4 || s->info.planeCount != 0) {
		return;
	}
	uint64_t srcEnd = (uint64_t) s->info.offset + (uint64_t) (s->info.height > 0 ? s->info.height - 1u : 0u) *
	    s->info.bytesPerRow + (uint64_t) s->info.width * 4u;
	if (s->info.width == 0 || s->info.height == 0 || srcEnd > s->info.allocSize) {
		return;
	}
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	if (sw <= 0 || sh <= 0) { sx = sy = 0; sw = (int32_t) s->info.width; sh = (int32_t) s->info.height; }
	if (dw <= 0 || dh <= 0) { dx = dy = 0; dw = (int32_t) fWidth; dh = (int32_t) fHeight; }
	int32_t px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	int32_t py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	int32_t px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	int32_t py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);

	flip_dpu_scale_fallback();
	fRowHashRows = 0;
	if (fbCompositeOverRect(fBase, fPanelWidth, fPanelHeight, fRowBytes,
	    s->pixels(), s->info.width, s->info.height, s->info.bytesPerRow, s->info.pixelFormat,
	    sx, sy, sw, sh, px0, py0, px1, py1)) {
		flip_whole();
		fAllChangedSeq = ++fRowSeq;
		fBufferSeq[flip.ready ? (flip.shown ^ 1) : 0] = fRowSeq;
		fLastBy0 = 0; fLastBy1 = (int32_t) fPanelHeight; fLastBufSeqAfter = fRowSeq; fLastAllDirty = true;
	}
}

/* Single-layer 32-bit swaps take drawLayer(); multi-layer swaps composite back-to-front (3 -> 0). */
void
IOMobileFramebuffer::showLayers(const uint8_t * swap, MiniIOSurface ** surfaces, uint32_t set,
    bool report, uint32_t * layersOut, MiniIOSurface ** recordedOut, bool * clearedBlackOut)
{
	uint32_t layers = 0;
	bool hasUnderlay = false;
	static bool loggedPlanar = false;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		if (sid != 0) layers |= 1u << layer;
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) { fStatSurfaces[i] = sid; break; }
		}
	}
	*layersOut = layers;

	if (layers == 0) {
		if ((set & 1) && fGraphics) {
			flip_dpu_scale_fallback();
			for (uint32_t y = 0; y < fPanelHeight; y++) {
				bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4u);
			}
			flip_whole();
			fAllChangedSeq = ++fRowSeq;
			fBufferSeq[flip.ready ? (flip.shown ^ 1) : 0] = fRowSeq;
			*clearedBlackOut = true;
			fLastBy0 = 0; fLastBy1 = (int32_t) fPanelHeight; fLastBufSeqAfter = fRowSeq; fLastAllDirty = true;
		}
		return;
	}

	/* Iterate back-to-front (3 -> 0): IOMFB places YUV video in higher indices under UI. */
	for (int layer = SWAP_LAYERS - 1; layer >= 0; layer--) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE((unsigned) layer));
		if (sid == 0) {
			continue;
		}
		int32_t src[4], dst[4];
		memcpy(src, swap + SWAP_SOURCE((unsigned) layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST((unsigned) layer), sizeof(dst));
		MiniIOSurface * s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %d: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (fbIsBiplanarYuv8(s->info.pixelFormat, s->info.planeCount)) {
			if (!fGraphics) {
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			if (!loggedPlanar || layers != fLayersShown) {
				loggedPlanar = true;
				IOLog("MiniIOMobileFramebuffer: planar layer %d surface %u '%c%c%c%c' %ux%u "
				    "src=%d,%d+%dx%d dst=%d,%d+%dx%d yBPR=%u uvBPR=%u ycbcr=%u layers=0x%x\n",
				    layer, sid, (int) (s->info.pixelFormat >> 24 & 0xff),
				    (int) (s->info.pixelFormat >> 16 & 0xff), (int) (s->info.pixelFormat >> 8 & 0xff),
				    (int) (s->info.pixelFormat & 0xff), s->info.width, s->info.height,
				    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3],
				    s->planes[0].bytesPerRow, s->planes[1].bytesPerRow, fbSurfaceYCbCrMatrix(s), layers);
			}
			drawYuvLayer(s, src, dst, !hasUnderlay);
			hasUnderlay = true;
		} else if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			if (hasUnderlay) {
				compositeLayerOver(s, src, dst);
			} else {
				drawLayer(s, src, dst);
			}
			hasUnderlay = true;
			if (layer == 0) {
				*recordedOut = s;
			}
		}
	}
}

#endif /* !MINI_FB_YUV_HOST_TEST */
#endif /* _MINI_FRAMEBUFFER_YUV_H */

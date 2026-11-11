/*
 * The drawable side of the Metal driver: an IOSurface of CoreAnimation's as a texture.
 *
 * Why this is a file of its own. Everything in src/annibalemetal.m answers Metal; this
 * answers CoreAnimation, and the two meet in exactly one method --
 * -newTextureWithDescriptor:iosurface:plane:. Keeping it apart means the surface bridge can
 * be read, and refused, without reading the 865 answers next to it.
 *
 * That method is NOT only a drawable's, and believing it was cost a wrong sentence in a real
 * boot (docs/metal.md 10.46). Five call sites in QuartzCore send it; one is
 * allocate_drawable_texture, the other four are the window server's Metal compositor
 * (CA::OGL::MetalContext::set_destination_iosurface, ::update_surface, ::get_surface,
 * ::create_surface_from_iosurface). So this file serves "a texture over an IOSurface",
 * whoever asks, and what it can and cannot lay over one is what its refusals are about.
 *
 * What this does NOT contain: a texture class. The object Metal gets back has to be the
 * driver's own AnnibaleMetalTexture, because that is what the render encoder binds, so
 * this side stops at `struct rwa_texture *` and src/annibalemetal.m wraps it. The split is
 * deliberate: the surface, its geometry and every refusal are here, and the Objective-C
 * object stays where its class is.
 */

#ifndef ANNIBALE_DRAWABLE_H
#define ANNIBALE_DRAWABLE_H

#include <IOKit/IOKitLib.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rwa_device;
struct rwa_texture;

/*
 * What the kernel answers about an IOSurface, and the request it answers in: kAnnibaleSurface
 * (0x7e) on AnnibaleGPUUserClient, `struct annibale_surface` in
 * xnu/files/iokit/Kernel/AnnibaleGPU.cpp. Field for field the same, and declared here rather
 * than included for the reason src/annibalemetal.m gives for the Mesa contract: userland/build.sh
 * knows nothing of the kernel tree.
 *
 * The struct goes in and comes back out of the same call, so `handle` is both the question
 * (ANN_DESCRIBE: do not make a buffer) and the answer (which buffer was made).
 */
struct ann_surface {
	uint32_t surface;                 /* the IOSurface's ID, which is what the kernel looks up */
	uint32_t handle;                  /* in: ANN_DESCRIBE or 0; out: the GPU buffer, 1-based */
	uint64_t size;                    /* out: how big that buffer is, in whole pages */
	uint32_t offset, rowBytes;        /* out: where the pixels start in it, and a row's length */
	uint32_t width, height;           /* out: and what they are -- read back, not assumed, */
	uint32_t pixelFormat, planes;     /* because MiniIOSurface substitutes some formats */
};

#define ANN_DESCRIBE 0xffffffffu          /* kAnnibaleDescribe: answer, allocate nothing */

/*
 * The shape, in the same fields and the same order as `struct rwa_texture_shape`
 * (tools/mesa/air/air_metal.h). Declared again under a name of this file's own for the
 * reason the struct above is: this file is built by userland/build.sh, which has neither
 * tree's headers on its include path, and a second declaration of the same layout is
 * cheaper than making it have them.
 */
struct ann_texture_shape {
	unsigned pixel_format, type, width, height, depth;
	unsigned array_length, mip_levels, sample_count, usage;
};

/*
 * The GPU service, opened and closed. This is the same service and the same user client
 * src/annibalemetal.m already opens for the device; a second connection is cheap and keeps
 * this file from reaching into that one's statics.
 *
 * 0, or -1 and a sentence in `why`. A kernel booted without gpu=1 has no such service, and
 * that is the ordinary case rather than a fault.
 */
int ann_gpu_open(io_connect_t *connection, char *why, size_t why_size);
void ann_gpu_close(io_connect_t connection);

/*
 * What a surface is, and the same with its memory taken as a GPU buffer. `describe`
 * allocates nothing and may be asked of any surface; `take` makes the buffer (or finds the
 * one this client already has for that surface) and must be paired with kDrmGemClose on
 * `handle` when the drawable is released.
 *
 * Both fill every out field on success. describe returns 0, a kernel IOReturn, or -1 for
 * a local validation failure; take returns 0 or -1. Failures include a sentence in why.
 * AnnibaleGPU surfaceBuffer returns kIOReturnUnsupported only for IOSurfaceAddress
 * storage, including describe requests. The texture caller uses that result to select
 * its existing buffer-backed storage path.
 */
int ann_surface_describe(io_connect_t connection, uint32_t surfaceID,
			 struct ann_surface *surface, char *why, size_t why_size);
int ann_surface_take(io_connect_t connection, uint32_t surfaceID,
		     struct ann_surface *surface, char *why, size_t why_size);
int ann_surface_give_back(io_connect_t connection, uint32_t handle, char *why, size_t why_size);

/*
 * An IOSurface's ID without linking IOSurface.framework.
 *
 * The driver is loaded as a bundle into whatever process asks Metal, and a hard dependency
 * on a framework would fail that load instead of failing this one call. Every process that
 * has a CAMetalLayer has IOSurface.framework loaded already -- CAMetalDrawable's private
 * structure holds an `__IOSurface *` -- so the symbol is there exactly when it is needed.
 *
 * 0 is "no ID", which is also what IOSurfaceGetID answers for a surface that was never
 * made shareable; either way there is nothing for the kernel to look up.
 */
uint32_t ann_surface_id(void *iosurface);

/*
 * Whether Adreno can render into this surface as it stands, and if not, which number says
 * so. This is freedreno's own precondition, not a guess:
 * fd_resource_from_handle (mesa-25.0.7 gallium/drivers/freedreno/freedreno_resource.c:1555)
 * fails the import unless
 *
 *     stride >= width * bytes_per_pixel        and        align(stride, 1 << pitchalign) == stride
 *
 * and on a6xx pitchalign is at least 6, so the row has to be a multiple of 64 bytes. The
 * kernel's surfaces already are: MINISURF_ROW_ALIGN is 64 (MiniIOSurface.cpp:80), and
 * IOSurfaceRootUserClient::methodInit tells IOSurface.framework rowAlignMask = 63
 * (MiniIOSurface.cpp:1090) so that a bytesPerRow asked for by a caller is rounded the same
 * way before it ever reaches surfaceCreate. The two numbers agree, which is why this check
 * is expected to pass -- and it is still made, because the agreement is between two trees
 * that change separately.
 *
 * 0, or -1 and a sentence naming the stride, the width and the two numbers it failed.
 */
int ann_surface_fits_adreno(const struct ann_surface *surface, unsigned bytes_per_pixel,
			    char *why, size_t why_size);

/*
 * The pixel format, in all three vocabularies at once: Metal's number, the IOSurface FourCC
 * CoreAnimation makes the surface of, and the bytes a pixel takes.
 *
 * The table is read out of -[CAMetalLayer setPixelFormat:] (0x1882a3d38), which is the only
 * place that decides it: the formats it accepts are 70, 71 ('RGBA'), 80, 81 ('BGRA'), 94,
 * 115 ('RGhA') and 550-555 (the XR family, 'b3a8' / 'w40a' / 'w30r'), and it raises an
 * NSException on anything else -- so a format outside this table cannot reach a drawable at
 * all, and this function answers for the ones that can.
 *
 * Note which of those the kernel then substitutes: surfaceCreate turns 'w30r', 'l10r',
 * 'w30R' and 'b3a8' into 'BGRA' (MiniIOSurface.cpp:466-479), so the FourCC read back from a
 * surface need not be the one asked for. That is why the caller compares against what the
 * kernel reported rather than against what it would have asked for.
 *
 * 0, or -1 and a sentence. `fourcc` and `bytes_per_pixel` may be NULL.
 */
int ann_format_of_metal(unsigned metal_pixel_format, uint32_t *fourcc,
			unsigned *bytes_per_pixel, char *why, size_t why_size);

/* Select and validate one IOSurface plane. allocation_base is the start of its allocation. */
int ann_surface_plane(void *iosurface, unsigned long plane, const struct ann_texture_shape *want,
    struct ann_surface *selected, void **allocation_base, char *why, size_t why_size);

/* client_memory is set only for the kernel's process-owned-storage refusal. */
struct rwa_texture *ann_texture_for_surface(struct rwa_device *mesa_device,
    const struct ann_surface *selected, unsigned long plane, const struct ann_texture_shape *want,
    int *client_memory, char *why, size_t why_size);

/*
 * There is deliberately NO release call here. The buffer a surface's memory becomes belongs
 * to freedreno's table of buffers from the moment the library imports it, and
 * rwa_texture_destroy gives it back -- so a second release on this side would be a second
 * owner of one surface. `ann_surface_take` / `ann_surface_give_back` are for a caller that wants a buffer of its
 * own, which the drawable path is not.
 */

/* Whether the compiler library has the import call at all, which is the one thing that
 * decides whether a drawable can exist in this boot. Said once, by the first caller. */
int ann_import_available(void);

static inline unsigned
ann_canonical_pixel_format(unsigned fmt)
{
	if (fmt == 94 || (fmt >= 550 && fmt <= 555)) {
		return (fmt == 551 || fmt == 553 || fmt == 555) ? 81u : 80u;
	}
	return fmt;
}

#ifdef __cplusplus
}
#endif

#endif /* ANNIBALE_DRAWABLE_H */

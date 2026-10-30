#ifndef AIR_METAL_COPY_H
#define AIR_METAL_COPY_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rwa_device;
struct rwa_buffer;
struct rwa_texture;
struct rwa_texture_shape;

/* Memory a shader can read and write, and the processor can too. The mapping is write
 * combined, as the kernel makes it, so a driver has nothing to flush.
 *
 * It is also what an index buffer is (rwa_draw.index_buffer): Metal has one buffer
 * object and the application decides at the draw what the bytes in it are, so this makes
 * memory that can be any of them rather than asking what it will be used for. */
struct rwa_buffer *rwa_buffer_create(struct rwa_device *device, size_t size,
				     char *why, size_t why_size);
void *rwa_buffer_map(struct rwa_buffer *buffer);
size_t rwa_buffer_size(struct rwa_buffer *buffer);
void rwa_buffer_destroy(struct rwa_buffer *buffer);

/* Copies execute on the resources' context and wait for its fence. Nonzero regions
 * only; resources must belong to one device. Buffer copies reject argument-buffer
 * encodings and overlapping ranges. Texture copies accept matching-format,
 * single-sample 2D or 2D-array color resources, with disjoint regions if aliased.
 * Returns 0 after completion, or -1 with a diagnostic in why. */
int rwa_buffer_copy(struct rwa_buffer *src, size_t src_offset,
                    struct rwa_buffer *dst, size_t dst_offset, size_t size,
                    char *why, size_t why_size);
int rwa_texture_copy(struct rwa_texture *src, unsigned src_level, unsigned src_slice,
                     const unsigned src_origin[3], struct rwa_texture *dst,
                     unsigned dst_level, unsigned dst_slice, const unsigned dst_origin[3],
                     const unsigned size[3], char *why, size_t why_size);

/* Downsamples level 0 into each level above it, for every layer. The blits are recorded on
 * the device's context in the order they are asked for, which is what makes the level 0
 * content visible to them; returns 0, or -1 with a diagnostic in why. */
int rwa_texture_generate_mipmaps(struct rwa_texture *texture, char *why, size_t why_size);

/* A texture laid over a buffer the caller holds, as -newTextureWithDescriptor:offset:
 * bytesPerRow: asks for: the bytes are copied out of the buffer at this call and again at
 * every rwa_buffer_textures_refresh; once the texture has been written they are copied back
 * into the buffer, at the next refresh or at rwa_buffer_textures_writeback. */
struct rwa_texture *rwa_texture_from_buffer(struct rwa_device *device,
                                            const struct rwa_texture_shape *shape,
                                            struct rwa_buffer *source, size_t offset,
                                            unsigned row_bytes, char *why, size_t why_size);
void rwa_buffer_textures_refresh(struct rwa_device *device);
void rwa_buffer_textures_writeback(struct rwa_device *device);

/* A w x h nearest-texel sample of the first level as 8-bit RGBA, clamped; -1 for a depth,
 * stencil, multisample or compressed texture. Waits for the work that writes the texture. */
int rwa_texture_thumbnail(struct rwa_texture *texture, unsigned w, unsigned h, uint8_t *rgba);

/* The caller's memory of a no-copy buffer: textures over the buffer read their bytes from it and
 * write their results back into it as well as into the buffer. NULL turns it off. */
void rwa_buffer_set_mirror(struct rwa_buffer *buffer, void *mirror);

/* A function linked into the fragment stage (MTLLinkedFunctions), node by node: the same
 * layout as air_metal.h's, see rwa_render_create_linked there. */
struct rwa_stitch_node {
	const void *bitcode;
	size_t size;
	const char *function;
	unsigned num_args;
	const int *args;
	unsigned num_after;
	const unsigned *after;
};
struct rwa_linked_function {
	const char *name;
	unsigned num_nodes;
	const struct rwa_stitch_node *nodes;
	unsigned output;
};

/* A second texture over the same resource, read and rendered as mtl_format: Metal's
 * -newTextureViewWithPixelFormat:. Every level and slice; texels of the same size. */
struct rwa_texture *rwa_texture_view(struct rwa_texture *texture, unsigned mtl_format,
                                     unsigned first_level, unsigned num_levels,
                                     char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
#endif

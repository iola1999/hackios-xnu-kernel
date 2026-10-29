#include "air_metal_copy.h"

static struct {
	void *handle;
	__typeof__(&rwa_buffer_copy) buffer_copy;
	__typeof__(&rwa_texture_copy) texture_copy;
	__typeof__(&rwa_texture_generate_mipmaps) texture_generate_mipmaps;
	__typeof__(&rwa_texture_from_buffer) texture_from_buffer;
	__typeof__(&rwa_buffer_textures_refresh) buffer_textures_refresh;
	struct rwa_device *(*device_open)(char *, size_t);
	void (*device_close)(struct rwa_device *);
	const char *(*device_name)(struct rwa_device *);
	struct rwa_buffer *(*buffer_create)(struct rwa_device *, size_t, char *, size_t);
	void *(*buffer_map)(struct rwa_buffer *);
	size_t (*buffer_size)(struct rwa_buffer *);
	void (*buffer_destroy)(struct rwa_buffer *);
	struct rwa_compute *(*compute_create)(struct rwa_device *, const void *, size_t,
					      const char *, struct rwa_compute_info *, char *,
					      size_t);
	void (*compute_destroy)(struct rwa_compute *);
	struct rwa_texture *(*texture_create)(struct rwa_device *,
					      const struct rwa_texture_shape *, char *, size_t);
	int (*texture_write)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			     const unsigned[3], const void *, size_t, char *, size_t);
	int (*texture_read)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			    const unsigned[3], void *, size_t, char *, size_t);
	int (*depth_read)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			  const unsigned[3], float *, uint8_t *, char *, size_t);
	void (*texture_destroy)(struct rwa_texture *);
	struct rwa_sampler *(*sampler_create)(struct rwa_device *,
					      const struct rwa_sampler_state *, char *, size_t);
	void (*sampler_destroy)(struct rwa_sampler *);
	int (*dispatch)(struct rwa_compute *, const struct rwa_bindings *,
			const unsigned[3], const unsigned[3], char *, size_t);
	int (*dispatch_buffers)(struct rwa_compute *, const struct rwa_bindings *,
				const struct rwa_buffer_bind *, unsigned, const unsigned[3],
				const unsigned[3], char *, size_t);
	int (*module_argbufs)(struct rwa_device *, const void *, size_t, const char *,
			      struct rwa_argbuf_layout *, unsigned, unsigned *, char *,
			      size_t);
	int (*argbuf_encode_buffer)(struct rwa_buffer *, size_t, unsigned,
				    struct rwa_buffer *, size_t, char *, size_t);
	int (*argbuf_encode_texture)(struct rwa_buffer *, size_t, unsigned,
				     struct rwa_texture *, char *, size_t);
	int (*argbuf_encode_sampler)(struct rwa_buffer *, size_t, unsigned,
				     struct rwa_sampler *, char *, size_t);
	unsigned (*argbuf_encoded_members)(const struct rwa_buffer *, size_t);
	struct rwa_render *(*render_create)(struct rwa_device *, const void *, size_t,
					    const char *, const void *, size_t, const char *,
					    const struct rwa_render_shape *,
					    struct rwa_render_info *, char *, size_t);
	void (*render_destroy)(struct rwa_render *);
	int (*render_draw)(struct rwa_render *, const struct rwa_draw *, char *, size_t);
	struct rwa_depth_stencil *(*depth_stencil_create)(struct rwa_device *,
							  const struct rwa_depth_stencil_state *,
							  char *, size_t);
	void (*depth_stencil_destroy)(struct rwa_depth_stencil *);
	int (*finish)(struct rwa_device *, char *, size_t);
	/* Optional: a library without it is one whose draws still finish themselves. */
	int (*flush)(struct rwa_device *, char *, size_t);
} mesa;

/* Loaded once, and its absence is an answer rather than a crash: without the GPU there is
 * no library worth loading, and the device object still has to exist. */
static bool
mesa_available(void)
{
	static bool tried;

	if (tried) {
		return mesa.handle != NULL;
	}
	tried = true;
	mesa.handle = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW);
	if (mesa.handle == NULL) {
		rw_print("no compiler library: %s\n", dlerror());
		return false;
	}
	*(void **)&mesa.buffer_copy = dlsym(mesa.handle, "rwa_buffer_copy");
	*(void **)&mesa.texture_copy = dlsym(mesa.handle, "rwa_texture_copy");
	*(void **)&mesa.texture_generate_mipmaps = dlsym(mesa.handle, "rwa_texture_generate_mipmaps");
	*(void **)&mesa.texture_from_buffer = dlsym(mesa.handle, "rwa_texture_from_buffer");
	*(void **)&mesa.buffer_textures_refresh = dlsym(mesa.handle, "rwa_buffer_textures_refresh");
	*(void **)&mesa.device_open = dlsym(mesa.handle, "rwa_device_open");
	*(void **)&mesa.device_close = dlsym(mesa.handle, "rwa_device_close");
	*(void **)&mesa.device_name = dlsym(mesa.handle, "rwa_device_name");
	*(void **)&mesa.buffer_create = dlsym(mesa.handle, "rwa_buffer_create");
	*(void **)&mesa.buffer_map = dlsym(mesa.handle, "rwa_buffer_map");
	*(void **)&mesa.buffer_size = dlsym(mesa.handle, "rwa_buffer_size");
	*(void **)&mesa.buffer_destroy = dlsym(mesa.handle, "rwa_buffer_destroy");
	*(void **)&mesa.compute_create = dlsym(mesa.handle, "rwa_compute_create");
	*(void **)&mesa.compute_destroy = dlsym(mesa.handle, "rwa_compute_destroy");
	*(void **)&mesa.texture_create = dlsym(mesa.handle, "rwa_texture_create");
	*(void **)&mesa.texture_write = dlsym(mesa.handle, "rwa_texture_write");
	*(void **)&mesa.texture_read = dlsym(mesa.handle, "rwa_texture_read");
	*(void **)&mesa.depth_read = dlsym(mesa.handle, "rwa_depth_read");
	*(void **)&mesa.texture_destroy = dlsym(mesa.handle, "rwa_texture_destroy");
	*(void **)&mesa.sampler_create = dlsym(mesa.handle, "rwa_sampler_create");
	*(void **)&mesa.sampler_destroy = dlsym(mesa.handle, "rwa_sampler_destroy");
	*(void **)&mesa.dispatch = dlsym(mesa.handle, "rwa_dispatch");
	/* The argument-buffer calls, looked up but NOT required, for the reason drawing is
	 * not required: a library without them is a real state of this work, and the
	 * encoder says so when an application asks for one rather than the driver refusing
	 * to load. What must not happen is answering an encoder and then binding nothing,
	 * so -newArgumentEncoder... checks for these before it makes one. */
	*(void **)&mesa.dispatch_buffers = dlsym(mesa.handle, "rwa_dispatch_buffers");
	*(void **)&mesa.module_argbufs = dlsym(mesa.handle, "rwa_module_argbufs");
	*(void **)&mesa.argbuf_encode_buffer = dlsym(mesa.handle, "rwa_argbuf_encode_buffer");
	*(void **)&mesa.argbuf_encode_texture = dlsym(mesa.handle, "rwa_argbuf_encode_texture");
	*(void **)&mesa.argbuf_encode_sampler = dlsym(mesa.handle, "rwa_argbuf_encode_sampler");
	*(void **)&mesa.argbuf_encoded_members = dlsym(mesa.handle,
						       "rwa_argbuf_encoded_members");
	/* Drawing is looked up but not required: a compiler library that can compute and not
	 * yet draw is a real state of this work, and the driver says so when asked to draw
	 * rather than refusing to exist. */
	*(void **)&mesa.render_create = dlsym(mesa.handle, "rwa_render_create");
	*(void **)&mesa.render_destroy = dlsym(mesa.handle, "rwa_render_destroy");
	*(void **)&mesa.render_draw = dlsym(mesa.handle, "rwa_render_draw");
	/* Depth likewise: a library that can draw and not test depth is a real state of
	 * this work, and -newDepthStencilStateWithDescriptor: says so when it is asked
	 * rather than the driver refusing to load. */
	*(void **)&mesa.depth_stencil_create = dlsym(mesa.handle, "rwa_depth_stencil_create");
	*(void **)&mesa.depth_stencil_destroy = dlsym(mesa.handle, "rwa_depth_stencil_destroy");
	*(void **)&mesa.finish = dlsym(mesa.handle, "rwa_finish");
	*(void **)&mesa.flush = dlsym(mesa.handle, "rwa_flush");
	if (mesa.device_open == NULL || mesa.compute_create == NULL || mesa.dispatch == NULL ||
	    mesa.texture_create == NULL || mesa.sampler_create == NULL) {
		/* rwa_dispatch's arguments changed when textures were added to the contract, so
		 * an older library would be called with the wrong shape of argument and do
		 * something arbitrary. Refusing it outright is the only safe reading of a
		 * missing rwa_texture_create. */
		rw_print("the compiler library is missing calls the contract names, "
		         "so it is older than this driver and will not be used\n");
		dlclose(mesa.handle);
		mesa.handle = NULL;
		return false;
	}
	return true;
}

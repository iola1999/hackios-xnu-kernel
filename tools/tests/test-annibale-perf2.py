#!/usr/bin/env python3
"""Host tests for Metal compute materialization, compute pipe-cache keys, and blit slice bounds."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
AIR = ROOT.parent / "hackios-xnu-userland/tools/mesa/air"
DRV = ROOT / "tools/userland-driver"
METAL_M = (DRV / "annibalemetal.m").read_text()
PIPE_INC = (DRV / "annibalemetal_buffer_pipeline.inc").read_text()
CACHE_INC = (DRV / "annibalemetal_pipecache.inc").read_text()
BLIT_INC = (DRV / "annibalemetal_blit.inc").read_text()
AIR_PRIV_H = (AIR / "air_metal_resource_private.h").read_text()
AIR_TEX_INC = (AIR / "air_metal_texture.inc").read_text()
AIR_COMP_INC = (AIR / "air_metal_compute.inc").read_text()
AIR_METAL_C = (AIR / "air_metal.c").read_text()

# Static guards against regressing the global spinlock / race in AnnibaleMetalComputePipeline.
assert "rw_compute_limits_lock" not in PIPE_INC, "global spinlock must not wrap compute_materialize"
assert "uint32_t _limitsMaterialized;" in METAL_M, "_limitsMaterialized must be 32-bit atomic-ready"

# Static guards that air_to_nir runs outside rwa_lock on deferred compute and render/module queries.
comp_create = AIR_COMP_INC[AIR_COMP_INC.index("compute_create(") : AIR_COMP_INC.index("struct rwa_compute *\nrwa_compute_create(")]
assert comp_create.index("air_to_nir(") < comp_create.index("RWA_LOCKED"), "compute_create must run air_to_nir outside rwa_lock"
mod_tex = AIR_METAL_C[AIR_METAL_C.index("rwa_module_textures(") : AIR_METAL_C.index("rwa_module_argbufs(")]
assert "RWA_LOCKED" not in mod_tex, "rwa_module_textures must not take rwa_lock"
mod_arg = AIR_METAL_C[AIR_METAL_C.index("rwa_module_argbufs(") : AIR_METAL_C.index('#include "air_metal_depth_stencil.inc"')]
assert "RWA_LOCKED" not in mod_arg, "rwa_module_argbufs must not take rwa_lock"
render_create = AIR_METAL_C[AIR_METAL_C.index("rwa_render_create_linked(") : AIR_METAL_C.index("rwa_render_destroy(")]
assert render_create.index("air_to_nir(", render_create.index("fragment_bitcode")) < render_create.index("create_stage_cso("), "render_create must run air_to_nir before stage CSO lock"
assert render_create.index("prepare_stage_nir(") < render_create.index("{\n   RWA_LOCKED;"), "render_create must run prepare_stage_nir outside rwa_lock"
assert "make_stage_cso(" not in render_create, "render_create must use lock-free prepare_stage_nir + locked create_stage_cso"
assert "rwa_quad_vbuf_ref(" in render_create, "render_create must share device->quad_vbuf"
assert "RWA_LOCKED;\n   if (!shape)" not in render_create, "rwa_render_create_linked must not lock the entire function"
assert "{\n   RWA_LOCKED;\n   if (fetched)" in render_create and "return render;\n   }\n\nfail:" in render_create, "render_create final RWA_LOCKED must be scoped in a block before fail:"

# Extract production Mesa refresh + writeback helpers.
air_wb_helpers = AIR_PRIV_H[
    AIR_PRIV_H.index("static inline void\nbuffer_texture_clear_written") :
    AIR_PRIV_H.index("static inline struct rwa_texture *")
]
air_forget = AIR_TEX_INC[
    AIR_TEX_INC.index("static void\nbuffer_textures_forget") :
    AIR_TEX_INC.index("static bool\ntexture_ensure_sampler_view")
]

# Extract production AnnibaleMetalComputePipeline methods.
cp_start = PIPE_INC.index("- (void)ensureLimitsMaterialized")
cp_end = PIPE_INC.index("- (NSUInteger)staticThreadgroupMemoryLength", cp_start)
cp_methods = PIPE_INC[cp_start:cp_end]

# Extract production compute pipe-cache functions from annibalemetal_pipecache.inc.
cache_add = CACHE_INC[
    CACHE_INC.index("static void\nrw_pipe_add_bytes") :
    CACHE_INC.index("static void\nrw_pipe_key(")
]
cache_compute = CACHE_INC[
    CACHE_INC.index("struct rw_compute_pipe_entry {") :
    CACHE_INC.index("/* The device is closing:")
]

# Extract production copyFromTexture:toTexture: from annibalemetal_blit.inc.
blit_start = BLIT_INC.index("- (void)copyFromTexture:(id)source toTexture:(id)destination")
blit_end = BLIT_INC.index("\n- (void)generateMipmapsForTexture:", blit_start)
blit_method = BLIT_INC[blit_start:blit_end]

OBJC_HARNESS = rf'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <dispatch/dispatch.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rwa_device {{ int dummy; }};
struct rwa_compute {{ int id; }};
struct rwa_compute_info {{
	unsigned max_threads_per_group;
	unsigned thread_execution_width;
	unsigned static_shared_bytes;
	unsigned buffers;
	unsigned textures;
	unsigned samplers;
	unsigned argbuf_mask;
	unsigned group_free;
}};

static void rw_print(const char *fmt, ...) {{ (void)fmt; }}

static pthread_mutex_t mock_mesa_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned materialize_calls;
static _Atomic unsigned materialize_fail_remaining;
static _Atomic unsigned destroyed_count;

static int
mock_compute_materialize(struct rwa_compute *c, struct rwa_compute_info *info, char *why, size_t why_size)
{{
	(void)c;
	pthread_mutex_lock(&mock_mesa_lock);
	atomic_fetch_add(&materialize_calls, 1);
	unsigned rem = atomic_load(&materialize_fail_remaining);
	if (rem > 0) {{
		atomic_store(&materialize_fail_remaining, rem - 1);
		snprintf(why, why_size, "transient mock failure");
		pthread_mutex_unlock(&mock_mesa_lock);
		return -1;
	}}
	info->max_threads_per_group = 512;
	info->thread_execution_width = 64;
	info->static_shared_bytes = 9999; /* must not overwrite immutable pipeline fields */
	info->group_free = 0;
	pthread_mutex_unlock(&mock_mesa_lock);
	return 0;
}}

static void
mock_compute_destroy(struct rwa_compute *c)
{{
	(void)c;
	atomic_fetch_add(&destroyed_count, 1);
}}

static struct {{
	int (*compute_materialize)(struct rwa_compute *, struct rwa_compute_info *, char *, size_t);
	void (*compute_destroy)(struct rwa_compute *);
}} mesa = {{
	.compute_materialize = mock_compute_materialize,
	.compute_destroy = mock_compute_destroy,
}};

#define RW_COMPUTE_PIPE_IDLE_MAX 4
#define RW_COMPUTE_BUCKETS       256

{cache_add}
{cache_compute}

@interface MockComputePipeline : NSObject {{
	struct rwa_compute *_compute;
	struct rwa_compute_info _info;
	uint32_t _limitsMaterialized;
}}
- (instancetype)initWithCompute:(struct rwa_compute *)compute info:(const struct rwa_compute_info *)info;
- (BOOL)groupFree;
- (NSUInteger)staticThreadgroupMemoryLength;
- (void)ensureLimitsMaterialized;
- (NSUInteger)maxTotalThreadsPerThreadgroup;
- (NSUInteger)threadExecutionWidth;
@end

@implementation MockComputePipeline
- (instancetype)initWithCompute:(struct rwa_compute *)compute info:(const struct rwa_compute_info *)info
{{
	self = [super init];
	if (self != nil) {{
		_compute = compute;
		_info = *info;
	}}
	return self;
}}
- (BOOL)groupFree {{ return _info.group_free != 0; }}
- (NSUInteger)staticThreadgroupMemoryLength {{ return _info.static_shared_bytes; }}
{cp_methods}
@end

@interface AnnibaleMetalTexture : NSObject
@property(nonatomic) MTLTextureType textureType;
@property(nonatomic) NSUInteger arrayLength;
@property(nonatomic) NSUInteger mipmapLevelCount;
@end
@implementation AnnibaleMetalTexture
@end

@interface MockBlitEncoder : NSObject
@property(nonatomic) NSUInteger lastSliceCount;
@property(nonatomic) NSUInteger lastLevelCount;
@property(nonatomic) BOOL called;
- (BOOL)acceptSource:(id)source destination:(id)destination resourceClass:(Class)cls;
- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
              toTexture:(id)destination destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl
             sliceCount:(NSUInteger)sc levelCount:(NSUInteger)lc;
- (void)copyFromTexture:(id)source toTexture:(id)destination;
@end

@implementation MockBlitEncoder
- (BOOL)acceptSource:(id)source destination:(id)destination resourceClass:(Class)cls
{{
	return [source isKindOfClass:cls] && [destination isKindOfClass:cls];
}}
- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
              toTexture:(id)destination destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl
             sliceCount:(NSUInteger)sc levelCount:(NSUInteger)lc
{{
	(void)source; (void)ss; (void)sl; (void)destination; (void)ds; (void)dl;
	_called = YES;
	_lastSliceCount = sc;
	_lastLevelCount = lc;
}}
{blit_method}
@end

static void
test_materialize_contract(void)
{{
	struct rwa_compute comp = {{ .id = 1 }};
	struct rwa_compute_info init_info = {{
		.max_threads_per_group = 1024,
		.thread_execution_width = 32,
		.static_shared_bytes = 2048,
		.group_free = 1,
	}};
	MockComputePipeline *p = [[MockComputePipeline alloc] initWithCompute:&comp info:&init_info];

	/* Transient failure must NOT latch _limitsMaterialized; retry must succeed. */
	atomic_store(&materialize_calls, 0);
	atomic_store(&materialize_fail_remaining, 1);
	assert([p maxTotalThreadsPerThreadgroup] == 1024);
	assert([p threadExecutionWidth] == 64);
	assert([p maxTotalThreadsPerThreadgroup] == 512);
	assert(atomic_load(&materialize_calls) == 2);
	/* Immutable fields must stay untouched. */
	assert([p groupFree] == YES);
	assert([p staticThreadgroupMemoryLength] == 2048);

	/* Subsequent calls hit lock-free fast path without invoking mesa.compute_materialize. */
	for (unsigned i = 0; i < 64; i++) {{
		assert([p maxTotalThreadsPerThreadgroup] == 512);
		assert([p threadExecutionWidth] == 64);
	}}
	assert(atomic_load(&materialize_calls) == 2);
	[p release];

	/* Concurrent readers + materializers under TSan/ASan. */
	MockComputePipeline *cp = [[MockComputePipeline alloc] initWithCompute:&comp info:&init_info];
	atomic_store(&materialize_calls, 0);
	atomic_store(&materialize_fail_remaining, 0);
	dispatch_apply(8, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t idx) {{
		(void)idx;
		for (unsigned j = 0; j < 200; j++) {{
			assert([cp maxTotalThreadsPerThreadgroup] == 512);
			assert([cp threadExecutionWidth] == 64);
			assert([cp groupFree] == YES);
			assert([cp staticThreadgroupMemoryLength] == 2048);
		}}
	}});
	assert(atomic_load(&materialize_calls) >= 1 && atomic_load(&materialize_calls) <= 8);
	[cp release];
}}

static void
test_compute_pipecache_keys(void)
{{
	const char raw[] = "AIR-BITCODE-PAYLOAD-1234567890";
	NSData *imm = [NSData dataWithBytes:raw length:sizeof(raw) - 1];
	NSMutableData *mut = [NSMutableData dataWithBytes:raw length:sizeof(raw) - 1];
	unsigned char k_raw[CC_SHA256_DIGEST_LENGTH];
	unsigned char k_imm1[CC_SHA256_DIGEST_LENGTH];
	unsigned char k_imm2[CC_SHA256_DIGEST_LENGTH];
	unsigned char k_mut[CC_SHA256_DIGEST_LENGTH];
	unsigned char k_other[CC_SHA256_DIGEST_LENGTH];

	rw_compute_pipe_key(k_raw, raw, sizeof(raw) - 1, "kernelA");
	rw_compute_pipe_key_nsdata(k_imm1, imm, "kernelA");
	rw_compute_pipe_key_nsdata(k_imm2, imm, "kernelA"); /* exercises associated-object hit */
	rw_compute_pipe_key_nsdata(k_mut, mut, "kernelA");
	rw_compute_pipe_key_nsdata(k_other, imm, "kernelB");

	assert(memcmp(k_raw, k_imm1, CC_SHA256_DIGEST_LENGTH) == 0);
	assert(memcmp(k_imm1, k_imm2, CC_SHA256_DIGEST_LENGTH) == 0);
	assert(memcmp(k_imm1, k_mut, CC_SHA256_DIGEST_LENGTH) == 0);
	assert(memcmp(k_imm1, k_other, CC_SHA256_DIGEST_LENGTH) != 0);

	/* Modifying mutable data must change the key (no stale associated SHA). */
	((char *)[mut mutableBytes])[0] = 'X';
	rw_compute_pipe_key_nsdata(k_mut, mut, "kernelA");
	assert(memcmp(k_imm1, k_mut, CC_SHA256_DIGEST_LENGTH) != 0);

	/* Bucket insert, lookup, idle eviction past RW_COMPUTE_PIPE_IDLE_MAX (4), and forget. */
	struct rwa_device dev = {{ 1 }};
	struct rwa_compute pipes[6];
	struct rwa_compute_info info = {{ .max_threads_per_group = 512, .thread_execution_width = 64 }};
	atomic_store(&destroyed_count, 0);
	for (int i = 0; i < 6; i++) {{
		pipes[i].id = i + 1;
		char name[16];
		snprintf(name, sizeof(name), "k%d", i);
		unsigned char k[CC_SHA256_DIGEST_LENGTH];
		rw_compute_pipe_key_nsdata(k, imm, name);
		rw_compute_pipe_keep(&dev, k, &pipes[i], &info);
		struct rwa_compute_info found_info = {{ 0 }};
		assert(rw_compute_pipe_find(&dev, k, &found_info) == &pipes[i]);
		rw_compute_pipe_release(&pipes[i]); /* users 2 -> 1 */
		rw_compute_pipe_release(&pipes[i]); /* users 1 -> 0 (idle) */
	}}
	assert(atomic_load(&destroyed_count) == 2); /* 6 idle with max 4 -> oldest 2 evicted */
	rw_compute_pipe_forget(&dev);
	assert(atomic_load(&destroyed_count) == 6);
}}

static void
test_blit_cube_slices(void)
{{
	MockBlitEncoder *enc = [[MockBlitEncoder alloc] init];
	AnnibaleMetalTexture *cube = [[AnnibaleMetalTexture alloc] init];
	cube.textureType = MTLTextureTypeCube;
	cube.arrayLength = 1;
	cube.mipmapLevelCount = 4;

	AnnibaleMetalTexture *arr2 = [[AnnibaleMetalTexture alloc] init];
	arr2.textureType = MTLTextureType2DArray;
	arr2.arrayLength = 2;
	arr2.mipmapLevelCount = 3;

	/* Cube (6 slices) -> 2DArray (2 slices): must copy MIN(6, 2) = 2 slices, NOT MIN(1, 2) * 6 = 6. */
	[enc copyFromTexture:cube toTexture:arr2];
	assert(enc.called && enc.lastSliceCount == 2 && enc.lastLevelCount == 3);

	/* 2DArray (8 slices) -> Cube (6 slices): must copy MIN(8, 6) = 6 slices, NOT MIN(8, 1) = 1. */
	arr2.arrayLength = 8;
	enc.called = NO;
	[enc copyFromTexture:arr2 toTexture:cube];
	assert(enc.called && enc.lastSliceCount == 6 && enc.lastLevelCount == 3);

	/* CubeArray (2 * 6 = 12) -> Cube (6): must copy 6. */
	AnnibaleMetalTexture *cubeArr = [[AnnibaleMetalTexture alloc] init];
	cubeArr.textureType = MTLTextureTypeCubeArray;
	cubeArr.arrayLength = 2;
	cubeArr.mipmapLevelCount = 5;
	enc.called = NO;
	[enc copyFromTexture:cubeArr toTexture:cube];
	assert(enc.called && enc.lastSliceCount == 6 && enc.lastLevelCount == 4);

	[cubeArr release];
	[arr2 release];
	[cube release];
	[enc release];
}}

int main(void)
{{
	@autoreleasepool {{
		test_materialize_contract();
		test_compute_pipecache_keys();
		test_blit_cube_slices();
	}}
	puts("ok  test-annibale-perf2 compute materialize, pipecache keys, and blit cube slices");
	return 0;
}}
'''

AIR_WB_HARNESS = rf'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t rwa_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static void rwa_unlock_scope(int *unused) {{ (void)unused; pthread_mutex_unlock(&rwa_lock); }}
#define RWA_LOCKED int rwa_locked_ __attribute__((cleanup(rwa_unlock_scope))) = (pthread_mutex_lock(&rwa_lock), 0); (void)rwa_locked_

struct rwa_buffer;
struct rwa_texture;
struct rwa_buffer_texture {{
	struct rwa_buffer_texture *next;
	struct rwa_texture *texture;
	struct rwa_buffer *source;
	size_t offset;
	unsigned row_bytes;
	bool written;
}};
struct rwa_device {{
	struct rwa_buffer_texture *buffer_textures;
	unsigned written_buffer_textures, buffer_textures_count;
}};
struct rwa_buffer {{
	struct rwa_device *device;
	uint32_t payload;
}};
struct rwa_texture_shape {{ unsigned pixel_format; }};
struct rwa_texture {{
	struct rwa_texture *storage_owner;
	struct rwa_texture *chroma;
	bool yuv_full_range, yuv_bt709, yuv_bt2020;
	uint64_t write_seq;
	struct rwa_device *device;
	struct rwa_texture_shape shape;
	struct rwa_buffer_texture *backing;
	uint32_t tex_value;
}};
static bool rgb2p_format(unsigned fmt) {{ (void)fmt; return false; }}
static atomic_uint downloads, uploads;
static struct {{ unsigned refreshes, uploads, unchanged; }} upload_count;

{air_wb_helpers}

static void buffer_mirror_pull(struct rwa_buffer *buffer) {{ (void)buffer; }}

static int buffer_texture_upload(struct rwa_buffer_texture *b, char *why, size_t why_size)
{{
	(void)why; (void)why_size;
	atomic_fetch_add(&uploads, 1);
	buffer_texture_clear_written(b->texture->device, b);
	return 0;
}}

static int buffer_texture_download(struct rwa_buffer_texture *b, char *why, size_t why_size)
{{
	(void)why; (void)why_size;
	atomic_fetch_add(&downloads, 1);
	b->source->payload = b->texture->tex_value;
	buffer_texture_clear_written(b->texture->device, b);
	return 0;
}}

{air_forget}

struct lock_probe_ctx {{
	struct rwa_device *dev;
	atomic_uint holder_locked;
	atomic_uint writeback_done;
}};

static void *lock_holder_thread(void *arg)
{{
	struct lock_probe_ctx *ctx = arg;
	pthread_mutex_lock(&rwa_lock);
	atomic_store_explicit(&ctx->holder_locked, 1, memory_order_release);
	/* Spin-wait until writeback returns; if writeback tried to take rwa_lock this would deadlock. */
	while (!atomic_load_explicit(&ctx->writeback_done, memory_order_acquire))
		;
	pthread_mutex_unlock(&rwa_lock);
	return NULL;
}}

static void test_writeback_fastpath(void)
{{
	struct rwa_device dev = {{ 0 }};
	struct rwa_buffer bufs[4] = {{ 0 }};
	struct rwa_texture texs[4] = {{ 0 }};
	struct rwa_buffer_texture *bks[4] = {{ 0 }};
	for (int i = 0; i < 4; i++) {{
		bufs[i].device = &dev;
		texs[i].device = &dev;
		bks[i] = calloc(1, sizeof(*bks[i]));
		bks[i]->texture = &texs[i];
		bks[i]->source = &bufs[i];
		bks[i]->next = dev.buffer_textures;
		dev.buffer_textures = bks[i];
		texs[i].backing = bks[i];
		__atomic_fetch_add(&dev.buffer_textures_count, 1u, __ATOMIC_RELEASE);
	}}

	/* Empty device: rwa_buffer_textures_refresh must skip rwa_lock completely. */
	struct rwa_device empty_dev = {{ 0 }};
	struct lock_probe_ctx rctx = {{ .dev = &empty_dev }};
	pthread_t rth;
	assert(pthread_create(&rth, NULL, lock_holder_thread, &rctx) == 0);
	while (!atomic_load_explicit(&rctx.holder_locked, memory_order_acquire))
		;
	atomic_store(&uploads, 0);
	rwa_buffer_textures_refresh(&empty_dev);
	atomic_store_explicit(&rctx.writeback_done, 1, memory_order_release);
	assert(pthread_join(rth, NULL) == 0);
	assert(atomic_load(&uploads) == 0);

	/* With 4 read-only buffer textures, writeback must bypass rwa_lock completely. */
	struct lock_probe_ctx ctx = {{ .dev = &dev }};
	pthread_t th;
	assert(pthread_create(&th, NULL, lock_holder_thread, &ctx) == 0);
	while (!atomic_load_explicit(&ctx.holder_locked, memory_order_acquire))
		;
	atomic_store(&downloads, 0);
	rwa_buffer_textures_writeback(&dev);
	atomic_store_explicit(&ctx.writeback_done, 1, memory_order_release);
	assert(pthread_join(th, NULL) == 0);
	assert(atomic_load(&downloads) == 0);
	assert(__atomic_load_n(&dev.written_buffer_textures, __ATOMIC_RELAXED) == 0);

	/* Repeated writes to the same texture keep count == 1; view forwards to storage_owner. */
	struct rwa_texture view = {{ .storage_owner = &texs[0], .device = &dev }};
	{{
		RWA_LOCKED;
		texs[0].tex_value = 0xCAFEBABEu;
		texture_written(&view);
		texture_written(&texs[0]);
	}}
	assert(__atomic_load_n(&dev.written_buffer_textures, __ATOMIC_RELAXED) == 1);
	rwa_buffer_textures_writeback(&dev);
	assert(atomic_load(&downloads) == 1);
	assert(bufs[0].payload == 0xCAFEBABEu);
	assert(__atomic_load_n(&dev.written_buffer_textures, __ATOMIC_RELAXED) == 0);

	/* buffer_textures_forget on a written texture restores written_buffer_textures to 0. */
	{{
		RWA_LOCKED;
		texture_written(&texs[1]);
		assert(__atomic_load_n(&dev.written_buffer_textures, __ATOMIC_RELAXED) == 1);
		buffer_textures_forget(&bufs[1]);
	}}
	assert(__atomic_load_n(&dev.written_buffer_textures, __ATOMIC_RELAXED) == 0);
	rwa_buffer_textures_writeback(&dev);
	assert(atomic_load(&downloads) == 1);

	/* Clean up remaining backings and verify buffer_textures_count drops to 0. */
	for (int i = 0; i < 4; i++)
		if (i != 1) {{
			RWA_LOCKED;
			buffer_textures_forget(&bufs[i]);
		}}
	assert(dev.buffer_textures == NULL);
	assert(__atomic_load_n(&dev.buffer_textures_count, __ATOMIC_RELAXED) == 0);
	/* Once buffer_textures_count drops to 0, refresh bypasses rwa_lock again. */
	struct lock_probe_ctx rctx2 = {{ .dev = &dev }};
	pthread_t rth2;
	assert(pthread_create(&rth2, NULL, lock_holder_thread, &rctx2) == 0);
	while (!atomic_load_explicit(&rctx2.holder_locked, memory_order_acquire))
		;
	rwa_buffer_textures_refresh(&dev);
	atomic_store_explicit(&rctx2.writeback_done, 1, memory_order_release);
	assert(pthread_join(rth2, NULL) == 0);
}}

static int render_create_scope_probe(int mode)
{{
	/* Mirrors rwa_render_create_linked: gotos before the lock block never enter RWA_LOCKED scope. */
	int result = -1;
	if (mode == 0)
		goto fail;
	{{
		RWA_LOCKED;
		if (mode == 1)
			goto fail;
		result = 42;
		return result;
	}}
fail:
	{{
		RWA_LOCKED; /* rwa_render_destroy re-acquires rwa_lock */
		return result;
	}}
}}

static void test_lock_scope_goto_balance(void)
{{
	assert(render_create_scope_probe(0) == -1);
	assert(render_create_scope_probe(1) == -1);
	assert(render_create_scope_probe(2) == 42);
	/* Verify rwa_lock is cleanly unlocked after all three return/goto paths. */
	assert(pthread_mutex_trylock(&rwa_lock) == 0);
	pthread_mutex_unlock(&rwa_lock);
}}

int main(void)
{{
	test_writeback_fastpath();
	test_lock_scope_goto_balance();
	puts("ok  test-annibale-perf2 mesa writeback fast-path and forget accounting");
	return 0;
}}
'''

with tempfile.TemporaryDirectory(prefix="perf2-test-") as tmp:
    src = Path(tmp) / "test_perf2.m"
    src.write_text(OBJC_HARNESS)
    wb_src = Path(tmp) / "test_air_wb.c"
    wb_src.write_text(AIR_WB_HARNESS)
    for tag, san in (("asan+ubsan", "address,undefined"), ("tsan", "thread")):
        exe = Path(tmp) / f"test_perf2_{tag}"
        subprocess.run([
            "xcrun", "clang", "-fno-objc-arc", "-fblocks", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
            f"-fsanitize={san}", "-fno-sanitize-recover=all",
            "-framework", "Foundation", "-framework", "Metal",
            str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)
        wb_exe = Path(tmp) / f"test_air_wb_{tag}"
        subprocess.run([
            "xcrun", "clang", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            f"-fsanitize={san}", "-fno-sanitize-recover=all",
            str(wb_src), "-o", str(wb_exe),
        ], check=True)
        subprocess.run([str(wb_exe)], check=True)

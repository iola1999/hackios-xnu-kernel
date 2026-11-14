/*
 * The device object of a Metal driver for this port, as a real library.
 *
 * What Metal demands of a plugin's device class was measured rather than guessed
 * (docs/metal.md section 10): `MTLAddDevice` (0x181a1875c) wants an object that comes back
 * non-nil from -initWithAcceleratorPort:, conforms to MTLDevice and MTLDeviceSPI -- both
 * asserted, both fatal -- and answers -featureProfile, -maxBufferLength and
 * -supportsVertexAmplificationCount:. Four methods. Everything else in the registration
 * path, including the 64-field limits table and the 42 feature queries, _MTLDevice fills
 * in itself once -featureProfile answers.
 *
 * Three decisions in how this is written, each for a measured reason:
 *
 *   - The class is declared in SOURCE, not built with objc_allocateClassPair as the probes
 *     did. That needs the private class symbol at link time, and it is bindable: IOGPU
 *     itself imports `_OBJC_CLASS_$__MTLDevice` together with its metaclass. Only the name
 *     is declared here -- nothing about Apple's 36 ivars or their offsets, which belong to
 *     Metal 306.6.4 and are nowhere written down. Our own ivars are placed by the runtime
 *     after Apple's, which is what the non-fragile ABI is for.
 *   - `<MTLDeviceSPI>` is declared at RUNTIME, with class_addProtocol. The protocol is
 *     private and requires 337 instance methods; writing that declaration out would mean
 *     copying 337 signatures we do not implement, and the only thing the two assertions in
 *     MTLAddDevice read is conformsToProtocol:. One call says the same thing truthfully.
 *     MTLDeviceSPI adopts MTLDevice, so one declaration satisfies both.
 *   - -initWithAcceleratorPort: opens this port's OWN gpu service, `AnnibaleGPU`, the way
 *     Mesa's shim does (tools/mesa/annibale-drm/annibale_drm.c: IOServiceOpen with type 0,
 *     then msm-shaped requests through IOConnectCallStructMethod). That is the road the
 *     resource layer will take, and it already carries Mesa's own dispatches today. The
 *     IOGPU road was measured shut: IOServiceOpen on our IOAcceleratorES service with
 *     Apple's type 1 answers kIOReturnUnsupported.
 *
 * The first resource is here too. A Metal buffer on this port is a GEM allocation of the
 * kernel's own gpu driver, made and mapped exactly the way Mesa's shim makes and maps one
 * (tools/mesa/annibale-drm/annibale_drm.c): kMsmGemNew (0x42) for the memory, MSM_INFO_GET_IOVA
 * through kMsmGemInfo (0x43) for the address the GPU sees, IOConnectMapMemory64 for the
 * address the processor sees, kDrmGemClose (0x09) to give it back. The kernel makes that
 * memory write-combined, with no cache of this processor between the program and what the
 * GPU reads, which is also why -didModifyRange: has nothing to do.
 *
 * Metal.framework has no `_MTLBuffer` to inherit -- only `_MTLResource`, a thin base over
 * `_MTLObjectWithLabel` -- so the buffer class is ours, as it is in Apple's own drivers
 * (`IOGPUMetalBuffer`, `AGXA11FamilyBuffer`).
 *
 * The compute path is here too, and it is where subsystem (1) finally carries something.
 * Metal hands a driver one function's LLVM module through -[_MTLFunction bitcodeData], and
 * those bytes were checked against our own AIR reader over all eleven functions of
 * AV1SW.metallib -- same lengths, same checksums (docs/metal.md 10.12). The front end that
 * reads them lives in Mesa, so the compiling and the dispatching are asked of
 * /annibale/lib/libannibaleair.dylib through the contract in tools/mesa/air/air_metal.h.
 *
 * That library is loaded with dlopen rather than linked, for a measured reason: the device
 * object must exist even when there is no GPU. The port starts the Adreno only with the
 * boot argument gpu=1, and a device that refuses to be created at all would take Metal's
 * whole discovery down with it; a device that answers its name and its limits and refuses
 * to make a pipeline is the truthful object in that case.
 *
 * Ordering is Metal's, not Gallium's: an encoder RECORDS its dispatches and the command
 * buffer runs them when it is committed. Dispatching as they are encoded would be less
 * code and would quietly reorder work an application committed in a particular order.
 *
 * Depth and stencil are here, and what that amounts to is four things: a texture of a depth
 * pixel format, an MTLDepthStencilState over the private _MTLDepthStencilState (whose SPI
 * has four methods Apple leaves answering NO, overridden here with the truth about the
 * descriptor), the render pass descriptor's depth and stencil attachments, and the
 * depth-range half of -setViewport:, which is the only thing in Metal that moves this
 * driver's quad in z at all.
 *
 * ARGUMENT BUFFERS are here, and what they are here is a RECORDER: Metal's encoder writes
 * 8-byte handles into a buffer and those are an Apple GPU's descriptors, so this driver
 * records (resource, Metal id) against (destination buffer, destination offset) instead
 * and the draw binds every member the pipeline's shaders declare. The buffer's contents
 * are never read, and an argument buffer the application did not fill through an encoder
 * is refused BY NAME (docs/air.md 8.2, and the MTLArgumentEncoder implementation at the
 * end of this file).
 *
 * BUFFER OFFSETS are honoured, which they were not: Metal's own `offset:` is recorded at
 * the encoder and judged at the draw, where the pipeline says whether an index is a
 * stream, a set or a buffer argument. A `constant` one is served at any offset -- the
 * compiler library uploads the block when the GPU cannot fetch from that offset -- and a
 * `device` one the GPU cannot fetch from is refused by name, because the shader may write
 * it and a copy would lose the writes.
 *
 * Heaps make separate allocations (annibalemetal_heap.inc); events, multisample resolve and
 * viewport rectangles remain unimplemented (docs/metal.md 10.8). Blit currently supports buffer and matching-format 2D copies.
 *
 * Every line this file writes carries the PROCESS it was written by
 * (`annibalemetal[progname pid]:`, see rw_print): a driver is a library, four or five
 * Metal clients are alive in one boot of this port, and a refusal nobody can attribute
 * is half a refusal.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>          /* for MTLSize and the public enumerations, nothing private */
#import <objc/message.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <dlfcn.h>

#include "annibale_drawable.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* The contract with Mesa's side, copied here rather than included: this file is built by
 * userland/build.sh, which knows nothing of Mesa's tree, and the declarations are few.
 * tools/mesa/air/air_metal.h is the original and explains each call. */
struct rwa_device;
struct rwa_buffer;
struct rwa_texture;
struct rwa_sampler;
struct rwa_compute;
struct rwa_compute_info {
	unsigned max_threads_per_group, thread_execution_width, static_shared_bytes;
	unsigned buffers, textures, samplers;
	/* which of those buffer indices are ARGUMENT BUFFERS, one bit each: Metal has one
	 * buffer table and an index in it is either a set or an argument. Appended, so
	 * every field above keeps the offset this file already reads -- and this file and
	 * the library still have to be rebuilt together. */
	unsigned argbuf_mask;
	unsigned group_free;   /* any threadgroup size that divides the grid serves alike */
};
struct rwa_texture_shape {
	unsigned pixel_format, type, width, height, depth;
	unsigned array_length, mip_levels, sample_count, usage;
};
struct rwa_sampler_state {
	unsigned address[3];
	unsigned min_filter, mag_filter, mip_filter, compare_function, max_anisotropy;
	unsigned normalized_coordinates;
	float lod_min, lod_max;
};
struct rwa_bindings {
	struct rwa_buffer *const *buffers;
	unsigned buffer_count;
	struct rwa_texture *const *textures;
	unsigned texture_count;
	struct rwa_sampler *const *samplers;
	unsigned sampler_count;
};
/* ARGUMENT BUFFERS. There is no argument buffer to write on this driver: the frontend
 * resolves every member of a set to a binding while translating, and Metal's 8-byte
 * handles are an Apple GPU's descriptors, which a Gallium driver cannot read. So
 * MTLArgumentEncoder RECORDS (resource, Metal id) against (destination buffer,
 * destination offset) and the draw binds what it finds -- air_metal.h carries the whole
 * of why, including why an argument buffer nobody encoded has to be refused by name. */
#define RWA_MAX_ARGBUF_MEMBERS 64
enum rwa_argbuf_kind {
	RWA_ARGBUF_BUFFER = 0,
	RWA_ARGBUF_TEXTURE = 1,
	RWA_ARGBUF_SAMPLER = 2,
};
struct rwa_argbuf_member {
	unsigned id;             /* Metal's [[id(n)]], which the application names */
	unsigned kind;           /* enum rwa_argbuf_kind */
	unsigned index;          /* the flat index the frontend allocated */
	unsigned write;
	char name[48];
};
struct rwa_argbuf_layout {
	unsigned metal_index;    /* the [[buffer(n)]] the set is bound at */
	unsigned encoded_length; /* 8 bytes per member the set declares: Apple's own number */
	unsigned alignment;
	unsigned member_count;
	struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];
};
/* Metal's buffer table for one stage: what is bound at each index AND AT WHAT OFFSET,
 * which `rwa_bindings.buffers` has no room for. One array for both of the things an
 * offset means, because Metal has one -setFragmentBuffer:offset:atIndex: behind both: for
 * an argument buffer it is WHICH ENCODING in that buffer to read, and for a buffer
 * argument it is which block of it the shader's pointer starts at. The library serves a
 * `constant` one at any offset (uploading when the GPU cannot fetch from it) and refuses
 * a `device` one the GPU cannot fetch from, by name -- air_metal.h says why. */
struct rwa_buffer_bind {
	struct rwa_buffer *buffer;   /* what the application bound at this Metal index */
	size_t offset;               /* Metal's own `offset:` */
};
struct rwa_render;
struct rwa_depth_stencil;
/* MTLVertexDescriptor's two arrays, indexed by Metal's own indices: attributes[n] by the
 * shader's [[attribute(n)]] and layouts[k] by the buffer index an attribute names. A
 * format of MTLVertexFormatInvalid (0) is "not configured", which is what a fresh
 * MTLVertexDescriptor holds everywhere, so a zeroed shape carries no layout at all. */
#define RWA_MAX_VERTEX_ATTRIBUTES 31
#define RWA_MAX_VERTEX_BUFFERS    31
struct rwa_vertex_attribute {
	unsigned format, offset, buffer_index;
};
struct rwa_vertex_layout {
	unsigned stride, step_function, step_rate;
};
/* MTLRenderPipelineColorAttachmentDescriptor's blend half, per colour attachment, as
 * Metal's own numbers: the library maps MTLBlendFactor, MTLBlendOperation and
 * MTLColorWriteMask by table and refuses what it cannot serve by name. `write_mask` is
 * MTLColorWriteMask, whose bits run the OTHER WAY from Gallium's colormask (Metal's Red is
 * 1 << 3) -- which is the library's business and not this driver's: this passes the number
 * Metal gave it and reverses nothing. */
struct rwa_color_blend {
	unsigned blending_enabled;
	unsigned rgb_blend_operation, alpha_blend_operation;
	unsigned source_rgb_blend_factor, destination_rgb_blend_factor;
	unsigned source_alpha_blend_factor, destination_alpha_blend_factor;
	unsigned write_mask;
};
struct rwa_render_shape {
	unsigned color_format[8];
	unsigned color_count;
	unsigned depth_format;
	unsigned stencil_format;
	unsigned sample_count;
	struct rwa_vertex_attribute vertex_attribute[RWA_MAX_VERTEX_ATTRIBUTES];
	struct rwa_vertex_layout vertex_layout[RWA_MAX_VERTEX_BUFFERS];
	/* APPENDED, 260 bytes at the END: every field above keeps the offset this driver
	 * already writes. A field inserted in the MIDDLE of a struct shared with the library
	 * has cost this port twice -- a primitive type that arrived as 1871292816, and
	 * struct air_resource growing by eight bytes so that every resource after the first
	 * was read eight bytes early. `color_blend_count` of 0 means this shape carries NO
	 * blend state, which is blending off and all four channels written; a zeroed entry
	 * cannot stand in for that, because MTLColorWriteMaskNone is 0 and Metal's default
	 * writeMask is All. */
	struct rwa_color_blend color_blend[8];
	unsigned color_blend_count;
};
struct rwa_render_info {
	unsigned vertex_buffers, fragment_buffers, fragment_textures, fragment_samplers;
	unsigned generated_vertex_stage;
	unsigned fragment_writes_depth;
	/* what the vertex descriptor became: how many elements were built, the Metal
	 * attribute index of each (in driver_location order, which is the shader's and not
	 * Metal's), how many stream slots a draw must cover and which of them are fetched
	 * from. The mask is what splits Metal's one buffer table at the draw. */
	unsigned vertex_attribute_count;
	unsigned vertex_attribute_index[16];
	unsigned vertex_streams;
	unsigned vertex_stream_mask;
	/* and which buffer indices of each stage are argument buffers. Metal's one table per
	 * stage now holds three things -- a vertex stream, a buffer argument and a set --
	 * and only the shader and the vertex descriptor together say which an index is. */
	unsigned vertex_argbuf_mask;
	unsigned fragment_argbuf_mask;
	/* and what the blend state became: one bit per attachment that ends up blending, and
	 * four bits per attachment of colour write mask in GALLIUM's order (R 0x1 .. A 0x8),
	 * which is MTLColorWriteMask's reversed. Printed rather than trusted: for any mask
	 * but All, None, green|blue and red|alpha these four bits MUST differ from the
	 * writeMask this driver passed, so a log line where they agree is a log line saying
	 * the bits were never reversed. Filled in BY the library, so this struct must not be
	 * shorter than the library's. */
	unsigned blend_enabled_mask;
	unsigned color_write_masks;
};
struct rwa_vertex_stream {
	struct rwa_buffer *buffer;
	size_t offset;
};
struct rwa_stencil_face {
	unsigned compare_function;
	unsigned stencil_fail_operation;
	unsigned depth_fail_operation;
	unsigned depth_stencil_pass_operation;
	unsigned read_mask, write_mask;
};
struct rwa_depth_stencil_state {
	unsigned depth_compare_function;
	unsigned depth_write_enabled;
	struct rwa_stencil_face front, back;
};
struct rwa_draw {
	struct rwa_texture *const *color;
	unsigned color_count;
	const float *clear_color;
	struct rwa_texture *depth;
	const float *clear_depth;
	const unsigned *clear_stencil;
	const struct rwa_depth_stencil *depth_stencil;
	unsigned stencil_reference[2];
	const float *depth_range;
	struct rwa_bindings vertex;
	struct rwa_bindings fragment;
	unsigned primitive;
	unsigned vertex_start, vertex_count;
	/* the indexed forms: -drawIndexedPrimitives: and its instanced ones. Appended, as
	 * air_metal.h says, so that every field above keeps the offset this driver already
	 * reads -- but the library and this file still have to be rebuilt together, and
	 * `vertex_count` is the INDEX count for an indexed draw (there is deliberately no
	 * second count field for the two to disagree in). */
	struct rwa_buffer *index_buffer;
	unsigned index_type;
	size_t index_buffer_offset;
	int base_vertex;
	unsigned instance_count;
	unsigned base_instance;
	/* the vertex streams: -setVertexBuffer:offset:atIndex:, indexed by that index, and
	 * a second array beside `vertex.buffers` because a stream carries an OFFSET and is
	 * checked against the layout rather than against the shader's [[buffer(n)]]. Both
	 * come off the one table Metal has, and the pipeline's vertex_stream_mask says
	 * which index belongs to which side. */
	const struct rwa_vertex_stream *vertex_streams;
	unsigned vertex_stream_count;
	/* Metal's buffer table for each stage: the offsets `vertex.buffers` and
	 * `fragment.buffers` have nowhere to put -- an argument buffer's encoding offset
	 * and a buffer argument's own offset alike. APPENDED rather than put inside
	 * rwa_bindings, which is embedded by value above: a field added there would move
	 * `primitive` and every index field after it, which is how a primitive type once
	 * arrived as 1871292816. */
	const struct rwa_buffer_bind *vertex_buffer_table;
	unsigned vertex_buffer_table_count;
	const struct rwa_buffer_bind *fragment_buffer_table;
	unsigned fragment_buffer_table_count;
	/* { originX, originY, width, height }, or NULL for the whole attachment. Appended
	 * here for the same reason it is appended in air_metal.h. */
	const float *viewport;
	/* { x, y, width, height }, or NULL for no clip. Appended with it. */
	const unsigned *scissor;
	/* the resolveTexture of each colour attachment, or NULL where there is none.
	 * Appended with the two above: air_metal.h says what the library does with one. */
	struct rwa_texture *const *resolve;
	unsigned discard;   /* colour attachments the pass does not load (bit i) */
};

/*
 * The library's MTLVertexFormat table, checked against Apple's own header HERE, where the
 * SDK is in hand: tools/mesa/air/air_metal.c carries those numbers as literals (it has no
 * Metal headers to read them from), this file is compiled against
 * iPhoneOS16.5.sdk/System/Library/Frameworks/Metal.framework/Headers/MTLVertexDescriptor.h,
 * and a disagreement between the two would be a vertex fetched in the wrong format rather
 * than anything that fails. So it is a build failure of this driver instead.
 *
 * Nothing in this file switches on these names; they are asserted and otherwise the
 * numbers pass through as the application gave them, exactly as MTLPixelFormat does.
 */
_Static_assert((int)MTLVertexFormatInvalid == 0 && (int)MTLVertexFormatUChar2 == 1 &&
		   (int)MTLVertexFormatChar2 == 4 &&
		   (int)MTLVertexFormatUChar2Normalized == 7 &&
		   (int)MTLVertexFormatChar2Normalized == 10,
	       "MTLVertexFormat's 8-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatUShort2 == 13 && (int)MTLVertexFormatShort2 == 16 &&
		   (int)MTLVertexFormatUShort2Normalized == 19 &&
		   (int)MTLVertexFormatShort2Normalized == 22 &&
		   (int)MTLVertexFormatHalf2 == 25,
	       "MTLVertexFormat's 16-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatFloat == 28 && (int)MTLVertexFormatFloat4 == 31 &&
		   (int)MTLVertexFormatInt == 32 && (int)MTLVertexFormatUInt == 36 &&
		   (int)MTLVertexFormatInt1010102Normalized == 40 &&
		   (int)MTLVertexFormatUInt1010102Normalized == 41 &&
		   (int)MTLVertexFormatUChar4Normalized_BGRA == 42,
	       "MTLVertexFormat's 32-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatUChar == 45 && (int)MTLVertexFormatChar == 46 &&
		   (int)MTLVertexFormatUCharNormalized == 47 &&
		   (int)MTLVertexFormatCharNormalized == 48 &&
		   (int)MTLVertexFormatUShort == 49 && (int)MTLVertexFormatShort == 50 &&
		   (int)MTLVertexFormatUShortNormalized == 51 &&
		   (int)MTLVertexFormatShortNormalized == 52 &&
		   (int)MTLVertexFormatHalf == 53,
	       "MTLVertexFormat's one-component forms are not where air_metal.c's table "
	       "has them");
_Static_assert((int)MTLVertexStepFunctionConstant == 0 &&
		   (int)MTLVertexStepFunctionPerVertex == 1 &&
		   (int)MTLVertexStepFunctionPerInstance == 2 &&
		   (int)MTLVertexStepFunctionPerPatch == 3 &&
		   (int)MTLVertexStepFunctionPerPatchControlPoint == 4,
	       "MTLVertexStepFunction is not what air_metal.c's refusals name");

/*
 * THE BLEND ENUMERATIONS, asserted for the same reason and with more riding on it: the
 * compiler library maps all three BY TABLE, and if Apple's numbering were not what those
 * tables assume the result would be a composite with the wrong arithmetic in it rather
 * than anything that fails. MTLBlendFactor and pipe_blendfactor agree on only two of
 * nineteen values, so there is no cast to fall back on and no accident that would be
 * harmless.
 *
 * This is one of the two independent sources for the numbering, and it is the one that
 * can be made into a BUILD FAILURE: the SDK header compiled against here is
 * iPhoneOS16.5.sdk's own MTLRenderPipeline.h, the release this driver runs on.
 *
 * MTLColorWriteMask is the one to look at twice. Its bits run from Alpha at 1 << 0 up to
 * Red at 1 << 3, which is the OPPOSITE ORDER from Gallium's colormask, where red is 0x1.
 * Four of the sixteen masks are palindromes and survive the confusion -- None, All,
 * green|blue and red|alpha -- so a driver that got this backwards would still pass any
 * test built out of "write everything" and "write nothing".
 */
_Static_assert((int)MTLBlendFactorZero == 0 && (int)MTLBlendFactorOne == 1 &&
		   (int)MTLBlendFactorSourceColor == 2 &&
		   (int)MTLBlendFactorOneMinusSourceColor == 3 &&
		   (int)MTLBlendFactorSourceAlpha == 4 &&
		   (int)MTLBlendFactorOneMinusSourceAlpha == 5 &&
		   (int)MTLBlendFactorDestinationColor == 6 &&
		   (int)MTLBlendFactorOneMinusDestinationColor == 7 &&
		   (int)MTLBlendFactorDestinationAlpha == 8 &&
		   (int)MTLBlendFactorOneMinusDestinationAlpha == 9 &&
		   (int)MTLBlendFactorSourceAlphaSaturated == 10,
	       "MTLBlendFactor's served values are not where air_metal.c's table has them");
_Static_assert((int)MTLBlendFactorBlendColor == 11 &&
		   (int)MTLBlendFactorOneMinusBlendColor == 12 &&
		   (int)MTLBlendFactorBlendAlpha == 13 &&
		   (int)MTLBlendFactorOneMinusBlendAlpha == 14 &&
		   (int)MTLBlendFactorSource1Color == 15 &&
		   (int)MTLBlendFactorOneMinusSource1Color == 16 &&
		   (int)MTLBlendFactorSource1Alpha == 17 &&
		   (int)MTLBlendFactorOneMinusSource1Alpha == 18,
	       "MTLBlendFactor's refused values are not where air_metal.c's refusals name "
	       "them, so a refusal would name the wrong factor");
_Static_assert((int)MTLBlendOperationAdd == 0 && (int)MTLBlendOperationSubtract == 1 &&
		   (int)MTLBlendOperationReverseSubtract == 2 &&
		   (int)MTLBlendOperationMin == 3 && (int)MTLBlendOperationMax == 4,
	       "MTLBlendOperation is not the identity air_metal.c's table states it is");
_Static_assert((int)MTLColorWriteMaskNone == 0 && (int)MTLColorWriteMaskAlpha == 1 &&
		   (int)MTLColorWriteMaskBlue == 2 && (int)MTLColorWriteMaskGreen == 4 &&
		   (int)MTLColorWriteMaskRed == 8 && (int)MTLColorWriteMaskAll == 0xf,
	       "MTLColorWriteMask's bits are not the reverse of Gallium's colormask, which "
	       "is what air_metal.c's table is");

/*
 * AND THE SHAPE OF THE TWO STRUCTS THIS FILE TYPES OUT BY HAND, which is the bug this
 * port has paid for twice: a primitive type that arrived as 1871292816, and struct
 * air_resource growing by eight bytes so that every resource after the first was read
 * eight bytes early.
 *
 * The numbers below are the same numbers asserted in tools/mesa/air/air_metal.c against
 * the library's own definitions. Neither file can see the other's structs, so this is
 * not a comparison -- it is two independent statements of one agreement, and an edit to
 * one side alone fails to build on that side. The offsets are asserted as well as the
 * sizes, because the whole discipline here is that a new field goes on the END: a field
 * inserted in the middle keeps the size and moves everything after it, which is exactly
 * the failure that is invisible.
 */
_Static_assert(sizeof(struct rwa_color_blend) == 32 &&
		   sizeof(struct rwa_render_shape) == 1052 &&
		   offsetof(struct rwa_render_shape, vertex_attribute) == 48 &&
		   offsetof(struct rwa_render_shape, vertex_layout) == 420 &&
		   offsetof(struct rwa_render_shape, color_blend) == 792 &&
		   offsetof(struct rwa_render_shape, color_blend_count) == 1048,
	       "struct rwa_render_shape here is not the shape air_metal.c asserts: a field "
	       "was added in the middle, or only one of the two copies was rebuilt");
_Static_assert(sizeof(struct rwa_render_info) == 116 &&
		   offsetof(struct rwa_render_info, vertex_argbuf_mask) == 100 &&
		   offsetof(struct rwa_render_info, blend_enabled_mask) == 108,
	       "struct rwa_render_info here is not the shape air_metal.c asserts, and this "
	       "one the LIBRARY writes into: too short is a buffer overrun");

/*
 * WHOSE SENTENCE THIS IS. Every line this driver writes goes through here, and the reason
 * is an attribution that could not be made: a boot's log held
 *
 *     annibalemetal: fragment buffer 2 at offset 1164: ...
 *     annibalemetal: no drawable: pixel format 500 is not one ...
 *
 * one after the other, and four or five Metal clients were alive in that boot (backboardd,
 * SpringBoard, chronod and this port's own application). The drawable refusal was pinned on
 * backboardd only by reading QuartzCore's own os_log line beside it, which is a guess, and
 * the offset refusals could not be attributed at all. A driver is a library: its lines are
 * written by somebody else's process, and the process is the first thing a reader needs.
 *
 * So the prefix is `annibalemetal[progname pid]:`, built once per process -- getprogname()
 * is the name launchd or the shell gave it, which is what the log's other lines carry too.
 *
 * It flushes, for the reason RW_ONCE below flushes: stdio to a console is block-buffered,
 * the processes that reach these sites are the ones that then abort, and a line written and
 * not flushed dies with the process. One flush per sentence costs nothing next to the draw
 * that provoked it.
 */
/* Instrumentation for the frozen-desktop round: the buffer teardown order below is unchanged, these
 * lines only record it, so a later analysis can match a bad access address against the memory of a
 * buffer this process already destroyed. Bounded to the first 32 buffers of a process and then every
 * 256th, because the log ring holds about 13,500 lines. */
static uint64_t rw_dealloc_count;
static bool
rw_dealloc_print(void)
{
	return ++rw_dealloc_count <= 32 || (rw_dealloc_count % 256) == 0;
}

static void rw_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* and the one method this driver installs into Metal's own function class, whose reason
 * is a measurement and is written where it is done (the end of this file) */
static void ensure_function_encoder(void);

#include "annibalemetal_console.inc"

static void
rw_print(const char *fmt, ...)
{
	static char who[96];
	FILE *out = rw_console != NULL ? rw_console : stdout;
	va_list ap;

	if (who[0] == '\0') {
		snprintf(who, sizeof(who), "annibalemetal[%s %d]", getprogname(), getpid());
	}
	flockfile(out);
	fprintf(out, "%s: ", who);
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fflush(out);
	funlockfile(out);
}

/*
 * Which pipeline state drew into what, as a table with one row per (pipeline, pass shape),
 * printed with the command buffer statistics.
 *
 * Two classes of layer carry a rounded corner here, and one of them shows a line where its
 * corner is tessellated while the other does not. Whether a multisample resolve would fix
 * both or only one depends on whether the two are drawn by the same pipeline state, and that
 * is a fact about a table rather than about any single draw. The handle is Metal's own object
 * pointer, stable for the life of the process, and the pass shape is the colour attachment's
 * size with the count of attachments and whether a depth one is bound -- enough for a reader
 * to tell a layer-sized pass from a screen-sized one without knowing which layer it was.
 *
 * The rows are counted per window and cleared after each print, so a row's draws are the
 * draws since the last line rather than since the process started.
 */
struct rw_pipeline_row {
	const void *pipeline;
	unsigned long draws;
	unsigned width, height;
	unsigned color_count, depth, sample_count;
};

#define RW_PIPELINE_ROWS 96

static struct rw_pipeline_row rw_pipeline_rows[RW_PIPELINE_ROWS];
static unsigned rw_pipeline_row_count;

static void
rw_pipeline_note(const void *pipeline, unsigned color_count, unsigned depth,
                 unsigned width, unsigned height, unsigned sample_count)
{
	unsigned i;

	if (pipeline == NULL) {
		return;
	}
	for (i = 0; i < rw_pipeline_row_count; i++) {
		struct rw_pipeline_row *row = &rw_pipeline_rows[i];

		if (row->pipeline == pipeline && row->width == width && row->height == height &&
		    row->color_count == color_count && row->depth == depth) {
			row->draws++;
			return;
		}
	}
	if (rw_pipeline_row_count == RW_PIPELINE_ROWS) {
		return;
	}
	rw_pipeline_rows[rw_pipeline_row_count].pipeline = pipeline;
	rw_pipeline_rows[rw_pipeline_row_count].draws = 1;
	rw_pipeline_rows[rw_pipeline_row_count].width = width;
	rw_pipeline_rows[rw_pipeline_row_count].height = height;
	rw_pipeline_rows[rw_pipeline_row_count].color_count = color_count;
	rw_pipeline_rows[rw_pipeline_row_count].depth = depth;
	rw_pipeline_rows[rw_pipeline_row_count].sample_count = sample_count;
	rw_pipeline_row_count++;
	/* Said once, when the shape is first drawn: a handle and the attachment it went into,
	 * which is the pair the reader needs and the one thing that cannot be recovered later
	 * from a count. Bounded by RW_PIPELINE_ROWS lines for the life of the process. */
	rw_print("pipeline seen: %p attachment %ux%u colour %u depth %u samples %u\n", pipeline,
	         width, height, color_count, depth, sample_count);
}

static void
rw_pipeline_dump(void)
{
	unsigned i, j, shown;

	if (rw_pipeline_row_count == 0) {
		return;
	}
	/* the busiest first: a row with one draw in it is noise beside one with a thousand */
	for (i = 1; i < rw_pipeline_row_count; i++) {
		struct rw_pipeline_row held = rw_pipeline_rows[i];

		for (j = i; j > 0 && rw_pipeline_rows[j - 1].draws < held.draws; j--) {
			rw_pipeline_rows[j] = rw_pipeline_rows[j - 1];
		}
		rw_pipeline_rows[j] = held;
	}
	shown = rw_pipeline_row_count < 24 ? rw_pipeline_row_count : 24;
	rw_print("pipelines: %u in this window, %u shown (state, attachment, draws, colours, "
	         "depth, samples)\n", rw_pipeline_row_count, shown);
	for (i = 0; i < shown; i++) {
		const struct rw_pipeline_row *row = &rw_pipeline_rows[i];

		rw_print("  %p %ux%u draws %lu colour %u depth %u samples %u\n", row->pipeline,
		         row->width, row->height, row->draws, row->color_count, row->depth,
		         row->sample_count);
	}
	rw_pipeline_row_count = 0;
	memset(rw_pipeline_rows, 0, sizeof(rw_pipeline_rows));
}

#include "annibalemetal_mesa.inc"

/*
 * Say a thing once, per place that says it.
 *
 * Every method of this driver that declines to do something is called by a daemon that will
 * call it again on the next frame, so a line per call buries the log that matters and a
 * silent return tells us nothing at all. One line the first time a site is reached is what
 * names the feature the system actually wanted -- which is the whole value of answering
 * "no" instead of being absent: an absent selector aborts the daemon and says nothing, an
 * answer that refuses keeps it running AND leaves its name in the log.
 *
 * And it is flushed, because the measurement says it has to be: searching every log in
 * `logs/` for four different RW_ONCE strings -- the GPU priority, the one ring, the texture
 * view, the resource ID -- finds them **zero** times, while plain printf lines from these
 * same files are in every boot. The processes that reach these sites are the ones that then
 * abort (backboardd aborted 43 times in logs/214841-run.log), and stdio to a console is
 * block-buffered, so a line written and not flushed dies with the process. That is a
 * suspicion with evidence rather than a proof -- the sites may also simply never have been
 * reached -- and the flush is what makes the next boot able to tell the two apart: a line
 * that is now still missing was never reached. One flush per site per process costs nothing;
 * a diagnostic that has never once been seen costs the whole point of writing it.
 */
#define RW_ONCE(...)							\
	do {								\
		static BOOL rw_said_;					\
									\
		if (!rw_said_) {					\
			rw_said_ = YES;					\
			rw_print(__VA_ARGS__);				\
		}							\
	} while (0)

/*
 * A sentence that is true every time it is reached and says nothing new after the first: print
 * it, then only on powers of two, carrying the count. The window server asks this driver for a
 * two-plane YCbCr drawable 862 times in a boot, and 862 identical lines pushed the rest of that
 * boot out of the loader's log, which keeps the first 256 KB and the last megabyte. A refusal
 * that drowns the evidence of what it refused is a worse instrument than a quiet one.
 */
#define RW_AGAIN(...)							\
	do {								\
		static unsigned long rw_times_;				\
									\
		if ((++rw_times_ & (rw_times_ - 1)) == 0) {		\
			rw_print(__VA_ARGS__);				\
			if (rw_times_ > 1) {				\
				rw_print("  ... and that is the %lu'th "\
					 "time\n", rw_times_);		\
			}						\
		}							\
	} while (0)

/*
 * The refused module itself, for the first four that fail. The refusal names the construct the
 * frontend declined and, since the stage is in the sentence, which module declined it; fixing the
 * frontend needs the bytes, and the only road out of a running daemon is this log, which the loader
 * holds and the host pulls every 20 s. Base64 in 72-character lines, 16 KB a module, four modules
 * a boot: the loader keeps the first 256 KB and the last megabyte, a run writes about 21 KB a
 * second, and a diagnostic that evicts the frames it explains is a worse instrument than a quiet
 * one. tools/mesa/air/airfail-extract.py reads these lines back into .metallib files. The modules
 * of a draw the GPU did not finish (rw_dump_kept_air, ANNIBALE_SYNC_DRAWS) are not counted against
 * the four and may be up to AIR_HUNG_BYTES: that happens once a boot, as the GPU stops with it.
 */
#define AIR_DUMP_MAX 4
#define AIR_DUMP_BYTES (16 * 1024)
#define AIR_HUNG_BYTES (96 * 1024)
static void
air_dump(const void *bytes, NSUInteger length, const char *stage, const char *name, bool counted)
{
	static const char b64[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	static unsigned dumped;
	static unsigned seen[8];
	const unsigned char *p = bytes;
	unsigned hash = 2166136261u;

	if (bytes == NULL || length == 0 || (counted && dumped >= AIR_DUMP_MAX)) {
		return;
	}
	for (NSUInteger i = 0; i < length; i++) {
		hash = (hash ^ p[i]) * 16777619u;
	}
	for (unsigned i = 0; i < sizeof(seen) / sizeof(seen[0]); i++) {
		if (seen[i] == hash) {
			return;
		}
		if (seen[i] == 0) {
			seen[i] = hash;
			break;
		}
	}
	if (length > (counted ? AIR_DUMP_BYTES : AIR_HUNG_BYTES)) {
		rw_print("AIRFAIL over-cap %s %s %lu bytes, the cap is %d\n", stage, name,
		    (unsigned long)length, counted ? AIR_DUMP_BYTES : AIR_HUNG_BYTES);
		return;
	}
	dumped += counted;
	rw_print("AIRFAIL begin %s %s %lu bytes fnv=%08x\n", stage, name,
	    (unsigned long)length, hash);
	for (NSUInteger at = 0; at < length; at += 54) {
		char line[76];
		unsigned n = (unsigned)MIN(length - at, 54u), o = 0;
		for (unsigned i = 0; i < n; i += 3) {
			unsigned v = (unsigned)p[at + i] << 16 |
			    (i + 1 < n ? (unsigned)p[at + i + 1] : 0) << 8 |
			    (i + 2 < n ? (unsigned)p[at + i + 2] : 0);
			line[o++] = b64[v >> 18 & 63];
			line[o++] = b64[v >> 12 & 63];
			line[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
			line[o++] = i + 2 < n ? b64[v & 63] : '=';
		}
		line[o] = '\0';
		rw_print("AIRFAIL %s\n", line);
	}
	rw_print("AIRFAIL end %s %s\n", stage, name);
}

static void
air_fail_dump(const void *bytes, NSUInteger length, const char *stage, const char *name)
{
	air_dump(bytes, length, stage, name, true);
}

#include "annibalemetal_diag.inc"


/* The private base class. Only what is used of it is declared, and nothing of its layout.
 * -init is its own (its method list has it); -initWithAcceleratorPort: is abstract there,
 * which is precisely the invitation this file accepts. */
@interface _MTLDevice : NSObject
- (instancetype)init;
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format;
- (NSUInteger)iosurfaceReadOnlyTextureAlignmentBytes;
- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes;
@end

/* The private bases this driver stands on. Only what is used of each is declared, and
 * nothing of any layout. The designated initialisers are the ones Apple's own driver sends
 * to super (read out of IOGPUMetalCommandQueue 0x1ec16da90, IOGPUMetalCommandEncoder
 * 0x1ec171d14 and AGXA11FamilyComputePipeline 0x2092477e0), which is a better authority
 * than a guess at what an init of this kind ought to be called. */
@interface _MTLObjectWithLabel : NSObject
- (instancetype)init;
@end

@interface _MTLResource : _MTLObjectWithLabel
@end

@interface _MTLCommandQueue : _MTLObjectWithLabel
- (instancetype)initWithDevice:(id)device descriptor:(id)descriptor;
@end

@interface _MTLCommandBuffer : _MTLObjectWithLabel
- (instancetype)initWithQueue:(id)queue
	   retainedReferences:(BOOL)retained
	 synchronousDebugMode:(BOOL)synchronous;
@end

@interface _MTLCommandEncoder : _MTLObjectWithLabel
- (instancetype)initWithCommandBuffer:(id)commandBuffer;
/* Declared because it is CALLED: that initialiser retains the command buffer and this is the
 * only thing that gives it back (see -[AnnibaleMetalRenderEncoder endEncoding]). */
- (void)endEncoding;
@end

@interface _MTLComputePipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

@interface _MTLRenderPipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

/*
 * The depth/stencil state's base class, and it is one of the good ones: everything but four
 * methods is Apple's and works for us. Read out of the cache the way the rest of this driver
 * was (tools/metal/absmeth.py over `_MTLDepthStencilState`, 10 selectors):
 *
 *   - -initWithDevice:depthStencilDescriptor: (0x181a29b38) is REAL and is the designated
 *     initialiser. It validates the descriptor field by field -- depthCompareFunction and
 *     each face's stencilCompareFunction, stencilFailureOperation, depthFailureOperation and
 *     depthStencilPassOperation are each compared against 8 and reported through
 *     MTLReportFailure when out of range -- then sends `init` to super
 *     (objc_msgSendSuper2), retains the device into _device and copies the descriptor's
 *     label into _label. It also asserts isKindOfClass: on both arguments (_MTLDevice for
 *     the device, MTLDepthStencilDescriptorInternal for the descriptor) and REPORTS rather
 *     than refuses: the failure paths branch back into the chain and the object is built
 *     either way.
 *   - -device and -label are ivar reads, so they work once that initialiser has run;
 *     -description, -formattedDescription: and -dealloc are Apple's.
 *   - -readsDepth, -writesDepth, -readsStencil, -writesStencil (0x181b1b5e0 and its three
 *     neighbours) are two-instruction NIL-STUBS: they answer NO. Those four are the whole of
 *     <MTLDepthStencilStateSPI> beyond the public protocol, and for a state that does write
 *     depth, NO is a false statement about this object -- so they are the four this driver
 *     overrides, out of the descriptor it was given.
 */
@interface _MTLDepthStencilState : NSObject
- (instancetype)initWithDevice:(id)device depthStencilDescriptor:(id)descriptor;
@end

/* The sampler has a real base class that owes its protocol nothing, and the initialiser
 * AGXA11FamilySampler sends to super (0x20930f0f0 -> 0x181a2d414). */
@interface _MTLSamplerState : NSObject
- (instancetype)initWithDevice:(id)device samplerDescriptor:(id)descriptor;
@end

/*
 * The one struct Metal reads out of a device by raw pointer, and the one answer on this
 * device that cannot be a refusal. Nothing here is guessed: the name and all three field
 * types are Apple's own, out of the type encoding MTLDeviceSPI records for the method
 * (`tools/metal/protometh.py`, joined to `absmeth.py` by implementation address):
 *
 *     -targetDeviceInfo    r^{MTLTargetDeviceArch=QI*}16@0:8
 *
 * so this declaration makes our method's recorded encoding the same string as Apple's,
 * rather than a `^v` that happens to pass in the same register. Which field means what was
 * read separately, off the readers -- see -targetDeviceInfo.
 */
struct MTLTargetDeviceArch {
	uint64_t target;      /* +0x00: nothing on this path reads it; Apple's own branch writes 0 */
	uint32_t version;     /* +0x08: the second half of MTLCompilerCache::Version{1, version} */
	char *name;           /* +0x10: formatted into "<pipelinelib>/pipelines.%s", never nil */
};

/* The offsets are the point of the struct, so they are checked rather than trusted: 0x10 is
 * the offset in the instruction that crashed without this answer. */
_Static_assert(sizeof(struct MTLTargetDeviceArch) == 24 &&
	       __builtin_offsetof(struct MTLTargetDeviceArch, version) == 0x8 &&
	       __builtin_offsetof(struct MTLTargetDeviceArch, name) == 0x10,
	       "Metal reads the name at +0x10 (0x181a3ca3c) and the version at +0x8");

@interface AnnibaleMetalDevice : _MTLDevice {
	io_service_t _acceleratorService;   /* the IOAcceleratorES service Metal handed us */
	io_connect_t _gpu;                  /* this port's own gpu client, or IO_OBJECT_NULL */
	struct rwa_device *_mesa;           /* Mesa's screen and context, or NULL */
	BOOL _assertions;                   /* Metal's own assertion switch, stored for it */
	struct MTLTargetDeviceArch _target;     /* answered by pointer; see -targetDeviceInfo */
	char _targetName[96];               /* the bytes _target.name points at, inside the object */
}
/* Not part of any Metal protocol: how this driver's own objects reach the compiler. */
- (struct rwa_device *)mesaDevice;
/* and the argument encoder of one function's `[[buffer(n)]]` set, which the method this
 * driver installs into Metal's own _MTLFunction forwards to (the end of this file says
 * why it has to): the layout is read out of the function's own bitcode, which is the one
 * thing Metal has here and its reflection has not. */
- (id)annibaleArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index;
@end

/* Declaring the protocol is the gate both assertions in MTLAddDevice read. Done at +load
 * for the bundle case (Metal loads us, so Metal is already there) and again before the
 * first use, because a library can also be brought in by hand, before Metal. */
static void
ensure_spi(Class cls)
{
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");

	if (spi == NULL) {
		rw_print("MTLDeviceSPI is not in this process yet; "
		         "MTLAddDevice would abort\n");
		return;
	}
	if (!class_conformsToProtocol(cls, spi) && !class_addProtocol(cls, spi)) {
		rw_print("class_addProtocol(MTLDeviceSPI) refused\n");
	}
	/* and the one method of Metal's OWN function class this driver has to answer: it is
	 * installed here for the same reason the protocol is declared here, and the whole of
	 * why it is installed at all is at the end of this file. */
	ensure_function_encoder();
}

/* One request of this port's own gpu client, shaped like the msm ioctl it stands for
 * (iokit/Kernel/AnnibaleGPU.cpp: kMsmGetParam = 0x40, struct msm_param). The parameters
 * worth asking for are 0x03 CHIP_ID (with the speed bin in the high half), 0x02 GMEM_SIZE
 * and 0x04 MAX_FREQ. Mesa reaches the same client the same way. */
static bool
gpu_param(io_connect_t gpu, uint32_t which, uint64_t *value)
{
	struct {
		uint32_t pipe, param;
		uint64_t value;
		uint32_t len, pad;
	} request = { .pipe = 0, .param = which, .value = 0, .len = 0, .pad = 0 };
	size_t size = sizeof(request);

	if (gpu == IO_OBJECT_NULL) {
		return false;
	}
	if (IOConnectCallStructMethod(gpu, 0x40, &request, sizeof(request), &request,
				      &size) != KERN_SUCCESS) {
		return false;
	}
	*value = request.value;
	return true;
}

/* The kernel's gpu interface, by the numbers its own header gives
 * (iokit/Kernel/AnnibaleGPU.cpp). */
enum {
	kDrmGemClose  = 0x09,
	kMsmGetParam  = 0x40,
	kMsmGemNew    = 0x42,
	kMsmGemInfo   = 0x43,
};
#define MSM_INFO_GET_IOVA  1

struct gem_new   { uint64_t size; uint32_t flags, handle; };
struct gem_info  { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct gem_close { uint32_t handle, pad; };

#include "annibalemetal_work_private.h"

@class AnnibaleMetalCommandQueue;
@class AnnibaleMetalWork;

@interface AnnibaleMetalComputePipeline : _MTLComputePipelineState {
	AnnibaleMetalDevice *_owner;
	struct rwa_compute *_compute;
	struct rwa_compute_info _info;
}
@end

/* One recorded dispatch. Metal's rule is that nothing runs until the command buffer is
 * committed, so an encoder records and the command buffer runs; dispatching at encode time
 * would be less code and would quietly reorder work. */
@interface AnnibaleMetalComputePipeline (AnnibaleMetalInternal)
- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device
			     compute:(struct rwa_compute *)compute
				info:(const struct rwa_compute_info *)info
			  descriptor:(id)descriptor;
- (struct rwa_compute *)compute;
- (BOOL)groupFree;
@end

@interface AnnibaleMetalCommandQueue : _MTLCommandQueue
- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device descriptor:(id)descriptor;
@end

@interface AnnibaleMetalCommandBuffer : _MTLCommandBuffer
- (instancetype)initWithAnnibaleQueue:(AnnibaleMetalCommandQueue *)queue;
- (void)recordWork:(AnnibaleMetalWork *)work;
- (id)device;
@end

@interface AnnibaleMetalCommandBuffer (Completion)
- (void)annibaleCompleteLater:(unsigned)commit fence:(struct rwa_fence *)fence;
- (void)annibaleCompleteWithFence:(struct rwa_fence *)fence commit:(unsigned)commit queued:(uint64_t)queued_us;
- (BOOL)annibaleAwaitCompletion;
@end

@interface AnnibaleMetalComputeEncoder : _MTLCommandEncoder
- (instancetype)initWithAnnibaleCommandBuffer:(AnnibaleMetalCommandBuffer *)commandBuffer;
@end

@interface AnnibaleMetalRenderEncoder : _MTLCommandEncoder
- (instancetype)initWithAnnibaleCommandBuffer:(AnnibaleMetalCommandBuffer *)commandBuffer
				  descriptor:(id)descriptor;
/* Every form of every draw method records through this one, indexed or not: the thirteen
 * lines that copy the encoder's state into the work item are the same either way, and two
 * copies of them are how one form ends up carrying state the other does not. */
- (void)recordPassClear;
- (void)recordDraw:(NSUInteger)primitive
       vertexStart:(NSUInteger)start
	     count:(NSUInteger)count
       indexBuffer:(id)indexBuffer
	 indexType:(NSUInteger)indexType
       indexOffset:(NSUInteger)indexOffset
	baseVertex:(NSInteger)baseVertex
     instanceCount:(NSUInteger)instances
      baseInstance:(NSUInteger)baseInstance;
@end

@interface AnnibaleMetalRenderPipeline : _MTLRenderPipelineState {
	AnnibaleMetalDevice *_owner;
	struct rwa_render *_render;
	struct rwa_render_info _info;
}
- (struct rwa_render *)render;
/* Not part of any Metal protocol: whether this pipeline's fragment shader stores to
 * [[depth]], which only whoever read the shader knows. A pass without a depth attachment
 * drops those writes -- Metal's own behaviour -- and a draw that says so is the difference
 * between a measurement and a colour attachment holding its clear value. */
- (BOOL)writesDepth;
/* Which buffer indices this pipeline's vertex stage FETCHES ATTRIBUTES from, one bit
 * each, and how many slots that covers. Metal has ONE table behind
 * -setVertexBuffer:offset:atIndex: and both the vertex descriptor's layouts and the
 * stage's [[buffer(n)]] arguments read out of it, so the encoder cannot know which an
 * index is until it knows the pipeline -- and this is what the draw splits the table
 * with. Zero for a stage that fetches nothing, which is every pipeline this driver drew
 * before the layout existed. */
- (unsigned)vertexStreamMask;
- (unsigned)vertexStreamCount;
/* And the element array the compiler library built out of the descriptor and the module's
 * own interface: how many, and which Metal [[attribute(n)]] each one is. The order is the
 * shader's driver_location order and not Metal's attribute order (docs/metal.md 10.44), so
 * these two are the only way a caller can read that mapping back instead of trusting it --
 * which is what metal_drv_test --draw-attrib prints beside its pixels. Not part of any
 * Metal protocol; Metal's own pipeline reflection would report the same thing. */
- (unsigned)vertexElementCount;
- (unsigned)vertexElementAttribute:(unsigned)index;
@end

@interface AnnibaleMetalTexture : _MTLResource {
	AnnibaleMetalDevice *_owner;
	struct rwa_texture *_texture;
	struct rwa_texture_shape _shape;
	CFTypeRef _iosurface;
	NSUInteger _iosurfacePlane;
	int _responsibleProcess;   /* as on the buffer: accounting, not behaviour */
	id _parentTexture;         /* the texture this one is a view of, retained */
	id _buffer;                /* the buffer this texture was laid over, retained */
	NSUInteger _bufferOffset, _bufferBytesPerRow;
	NSUInteger _parentRelativeLevel;
	NSUInteger _parentRelativeSlice;
	MTLTextureSwizzleChannels _swizzle;
}
- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device
	texture:(struct rwa_texture *)texture shape:(const struct rwa_texture_shape *)shape
	iosurface:(CFTypeRef)surface plane:(NSUInteger)plane;
@property (readonly) struct rwa_texture *mesaTexture;
/* The buffer a texture made by -[MTLBuffer newTextureWithDescriptor:offset:bytesPerRow:]
 * lies over: kept alive by the texture and answered by -buffer, as Metal does. */
- (void)annibaleSetBuffer:(id)buffer offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytesPerRow;
/* Not part of MTLTexture: the depth or stencil aspect of this texture, one float and one
 * byte per texel. Metal's own -getBytes: has no aspect to name and the compiler library
 * refuses a depth texture through it by name, so a caller that wants to know what a depth
 * buffer holds asks for an aspect here instead. Either pointer may be NULL. */
- (BOOL)annibaleReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region;
@end

@interface AnnibaleMetalSampler : _MTLSamplerState {
	struct rwa_sampler *_sampler;
}
@property (readonly) struct rwa_sampler *mesaSampler;
@end

@interface AnnibaleMetalDepthStencilState : _MTLDepthStencilState {
	struct rwa_depth_stencil *_state;
	BOOL _readsDepth, _writesDepth, _readsStencil, _writesStencil;
	unsigned _depthCompare;   /* MTLCompareFunction, for the sync log */
}
@property (readonly) struct rwa_depth_stencil *mesaDepthStencil;
- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor;
@end

/*
 * MTLArgumentEncoder. Not a writer: the records live in the compiler library with the
 * destination buffer, because the frontend has already resolved every member of every set
 * to a binding and Metal's 8-byte handles are an Apple GPU's descriptors (air_metal.h).
 * The implementation below carries the whole of why.
 */
@interface AnnibaleMetalArgumentEncoder : NSObject
- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule;
/* Not part of MTLArgumentEncoder: the mapping between Metal's [[id(n)]] and the index the
 * GPU is programmed with, so a test can read it back instead of trusting it. */
- (NSUInteger)annibaleMemberCount;
- (NSUInteger)annibaleMetalIndex;
- (BOOL)annibaleMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index;
@end

@interface AnnibaleMetalBuffer : _MTLResource {
	AnnibaleMetalDevice *_owner;
	io_connect_t _gpu;        /* not owned: the device holds it */
	uint32_t _handle;         /* set on the kernel path only */
	uint64_t _iova;
	void *_contents;
	NSUInteger _length;
	struct rwa_buffer *_mesa; /* set instead when the memory came from Mesa */
	int _responsibleProcess;  /* what <MTLResourceSPI> calls the owner, for accounting */
	void (^_callerDeallocator)(void *, NSUInteger); /* copied: CoreAnimation's is on its stack */
	void *_callerBytes;       /* what that block expects back, not what we allocated */
}
@property (readonly) struct rwa_buffer *mesaBuffer;
- (instancetype)initWithDevice:(AnnibaleMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length;
- (instancetype)initWithDevice:(AnnibaleMetalDevice *)device
			   gpu:(io_connect_t)gpu
			 bytes:(const void *)bytes
			length:(NSUInteger)length
		   deallocator:(void (^)(void *, NSUInteger))deallocator;
@end

#include "annibalemetal_stitch.inc"
#include "annibalemetal_pipecache.inc"
#include "annibalemetal_reflection.inc"


#include "annibalemetal_device.inc"
#include "annibalemetal_device_caps.inc"
#include "annibalemetal_heap.inc"   /* -newHeapWithDescriptor: and the two size answers */
#include "annibalemetal_buffer_pipeline.inc"
#include "annibalemetal_work.inc"
#include "annibalemetal_blit.inc"
#include "annibalemetal_texture_layout.inc"   /* ATX/KTX images: texture layouts and textures over their memory */
#include "annibalemetal_queue.inc"
#include "annibalemetal_commandbuffer.inc"
#include "annibalemetal_completion.inc"   /* where command buffers complete */
#include "annibalemetal_renderencoder.inc"
#include "annibalemetal_renderencoder_spi.inc"
#include "annibalemetal_compute_argencoder.inc"

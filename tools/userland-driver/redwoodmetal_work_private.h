#ifndef REDWOOD_METAL_WORK_PRIVATE_H
#define REDWOOD_METAL_WORK_PRIVATE_H

#import <Foundation/Foundation.h>

#define RW_MAX_BUFFERS 32

@class RedwoodMetalComputePipeline;
@class RedwoodMetalRenderPipeline;

enum RWCopyKind { RW_COPY_NONE, RW_COPY_BUFFER, RW_COPY_TEXTURE, RW_COPY_MIPMAP,
                   RW_COPY_ERROR };

@interface RedwoodMetalWork : NSObject {
@private
	BOOL _ownsResources;
@public
	/* Copies use buffers[0..1] or textures[0..1], owned like shader bindings. */
	enum RWCopyKind copyKind;
	size_t copyOffsets[2], copyLength;
	unsigned copyLevels[2], copySlices[2], copyOrigins[2][3], copySize[3];
	char copyError[128];
	RedwoodMetalComputePipeline *pipeline;
	RedwoodMetalRenderPipeline *render;      /* set instead for a draw */
	id color[8];
	unsigned colorCount;
	/* the resolveTexture of each colour attachment, or nil; struct rwa_draw says what the
	 * library does with one and why it is resolved per draw rather than per pass */
	id resolve[8];
	float clearColor[4];
	BOOL clears;
	/* the depth/stencil attachment of the pass, its two load actions and the state
	 * bound when the draw was encoded. Separate flags per aspect because Metal keeps
	 * two attachment descriptors over one texture and a pass that clears the depth and
	 * keeps the stencil is ordinary. */
	id depth;
	id depthStencilState;
	float clearDepth;
	unsigned clearStencil;
	BOOL clearsDepth, clearsStencil;
	unsigned stencilReference[2];
	float depthRange[2];
	BOOL hasDepthRange;
	/* { originX, originY, width, height } of -setViewport:, in pixels, and whether the
	 * encoder was given one at all; zeroed and NO when it was not, which is the whole
	 * attachment. */
	float viewportRect[4];
	BOOL hasViewportRect;
	/* { x, y, width, height } of -setScissorRect:, in pixels from the top left, and
	 * whether the encoder was given one; without one the draw does not clip. */
	unsigned scissorRect[4];
	BOOL hasScissorRect;
	unsigned primitive, vertexStart, vertexCount;
	/* the index buffer of an indexed draw and the three numbers that come with it in
	 * the same Metal call; indexBuffer nil is -drawPrimitives:, and then vertexCount
	 * counts vertices rather than indices */
	id indexBuffer;
	unsigned indexType;
	size_t indexBufferOffset;
	int baseVertex;
	unsigned instanceCount, baseInstance;
	id vertexBuffers[RW_MAX_BUFFERS];
	size_t vertexBufferOffsets[RW_MAX_BUFFERS];
	unsigned vertexBufferCount;
	id buffers[RW_MAX_BUFFERS];
	/* Metal's own `offset:` for each of them, which until argument buffers existed was
	 * refused at the encoder as unimplemented. It is RECORDED here and judged at the
	 * draw instead, because which indices may carry one is the pipeline's answer: a
	 * member of an argument buffer has its own offset and a buffer argument still has
	 * none. */
	size_t bufferOffsets[RW_MAX_BUFFERS];
	id textures[RW_MAX_BUFFERS];
	id samplers[RW_MAX_BUFFERS];
	unsigned count, textureCount, samplerCount;
	unsigned groups[3], size[3];
	unsigned threadgroupBytes;   /* -setThreadgroupMemoryLength: for threadgroup buffer 0 */
}
/* Call once the encoder has finished filling this immutable work item. */
- (void)retainResources;
@end

/* One line a second of what this process is still doing, because a thread inside a call runs
 * nothing else and the counts are the only thing left that can tell a stopped frame flow from a
 * stopped submission. The line is printed by the watch thread in redwoodmetal_commandbuffer.inc;
 * the counters are written from there, from redwoodmetal_texture.inc and from the command buffer.
 * `present` and `swap` are printed as zero by construction: this driver does not implement
 * -presentDrawable: (Apple's own method sends -commit), and the swap into the panel happens in
 * MiniIOMobileFramebuffer, in the kernel, where this library has no view. `texwrite` counts
 * -replaceRegion:...bytesPerImage:, the one caller of rwa_texture_write.
 */
#define RW_HEARTBEAT_US 1000000ULL
static unsigned hb_commit;                      /* -commit entries this second */
static unsigned hb_wait;                        /* -waitUntilCompleted entries this second */
static unsigned hb_texwrite;                    /* -replaceRegion: entries this second */

#endif

/*
 * We always store what was drawn, so these record nothing; saying it here is cheaper than
 * leaving the next reader to wonder whether a discarded attachment would be honoured.
 *
 * That is now also true of the depth attachment, and there it is more than tidiness: the
 * only thing MTLStoreActionDontCare permits that storing does not is leaving the contents
 * undefined, so keeping them is a superset of every store action Metal has. A pass whose
 * depth the next pass reads back therefore works whichever action it asked for -- and the
 * depth test inside one pass never depended on this at all.
 */
- (void)setColorStoreAction:(NSUInteger)action atIndex:(NSUInteger)index
{
	(void)action; (void)index;
}
- (void)setDepthStoreAction:(NSUInteger)action                 { (void)action; }
- (void)setStencilStoreAction:(NSUInteger)action               { (void)action; }
- (void)setColorStoreActionOptions:(NSUInteger)options atIndex:(NSUInteger)index
{
	(void)options; (void)index;
}
- (void)setDepthStoreActionOptions:(NSUInteger)options         { (void)options; }
- (void)setStencilStoreActionOptions:(NSUInteger)options       { (void)options; }

/* Residency: every allocation here is resident for its lifetime, so there is nothing to
 * make resident. */
/*
 * RESIDENCY, which on this driver is nothing to arrange, and that is worth saying rather
 * than implementing in silence.
 *
 * In Metal these calls exist because an argument buffer holds raw handles: the GPU reads
 * them, so the resources they name have to be made resident separately. Here every member
 * of every set is bound explicitly from the encoder's record (air_metal.h), so a resource
 * that is encoded is bound and a resource that is not encoded is a refusal at the draw --
 * there is no third state for residency to be about. What the application still owes is
 * what Metal makes it owe: keeping the resource alive, and this driver turns a resource
 * destroyed after it was encoded into a sentence rather than a read of freed memory.
 */
- (void)useResource:(id)resource usage:(NSUInteger)usage stages:(NSUInteger)stages
{
	(void)resource; (void)usage; (void)stages;
}
- (void)useResource:(id)resource usage:(NSUInteger)usage
{
	(void)resource; (void)usage;
	RW_ONCE("-useResource: is a no-op here: every member of an argument "
		"buffer is bound explicitly from what the encoder recorded, so there is no "
		"residency to arrange and no handle for the GPU to follow\n");
}
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(NSUInteger)usage
{
	(void)resources; (void)count; (void)usage;
}
- (void)useResources:(const id *)resources
	       count:(NSUInteger)count
	       usage:(NSUInteger)usage
	      stages:(NSUInteger)stages
{
	(void)resources; (void)count; (void)usage; (void)stages;
}
- (void)useHeap:(id)heap
{
	(void)heap;
	RW_ONCE("-useHeap: is a no-op here, for -useResource:'s reason, and "
		"a heap's resources are separate allocations, resident like any other\n");
}
- (void)useHeap:(id)heap stages:(NSUInteger)stages             { (void)heap; (void)stages; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count     { (void)heaps; (void)count; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count stages:(NSUInteger)stages
{
	(void)heaps; (void)count; (void)stages;
}

/*
 * THE REST OF WHAT <MTLRenderCommandEncoder> AND <MTLRenderCommandEncoderSPI> REQUIRE.
 *
 * 125 + 34 required methods between them, read out of the cache rather than the runtime
 * (MTLRenderCommandEncoderSPI is declared in AGXMetalA11.bundle, so no process on this
 * phone has it; tools/metal/owes.py does the subtraction). 48 of them had no answer
 * anywhere in this chain before this section, and an answer is the difference between a
 * daemon that keeps running and one launchd restarts for ever.
 *
 * Each one either serves, or refuses and says so once. Nothing here claims.
 */

/* Served, by forwarding: Metal's own shorthands for what this encoder already does. */
- (void)setFragmentTexture:(id)texture
	   atTextureIndex:(NSUInteger)textureIndex
	     samplerState:(id)sampler
	   atSamplerIndex:(NSUInteger)samplerIndex
{
	[self setFragmentTexture:texture atIndex:textureIndex];
	[self setFragmentSamplerState:sampler atIndex:samplerIndex];
}
- (void)setFragmentSamplerStates:(const id *)samplers
		    lodMinClamps:(const float *)lodMin
		    lodMaxClamps:(const float *)lodMax
		       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentSamplerState:samplers[i] lodMinClamp:lodMin[i]
				  lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}
- (void)setVertexSamplerStates:(const id *)samplers
		  lodMinClamps:(const float *)lodMin
		  lodMaxClamps:(const float *)lodMax
		     withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexSamplerState:samplers[i] lodMinClamp:lodMin[i]
				lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}
/* The SPI spelling of -setDepthClipMode:, which already says what it cannot do. */
- (void)setDepthClipModeSPI:(NSUInteger)mode { [self setDepthClipMode:mode]; }

/*
 * Served by construction, which is a different thing from ignored.
 *
 * A barrier and a fence both say "do not start this until that has finished". Everything
 * this driver submits goes to the kernel's single msm ring in the order it was committed,
 * and a command buffer's recorded work runs inside one -commit; there is no second queue
 * and no reordering, so the ordering a barrier asks for is already the only ordering there
 * is. If that ever stops being true -- a second ring, or work submitted from -endEncoding
 * -- these stop being correct, which is why it is written down here and not assumed.
 */
- (void)memoryBarrierWithScope:(NSUInteger)scope
		   afterStages:(NSUInteger)after
		  beforeStages:(NSUInteger)before
{
	(void)scope; (void)after; (void)before;
}
- (void)memoryBarrierWithResources:(const id *)resources
			     count:(NSUInteger)count
		       afterStages:(NSUInteger)after
		      beforeStages:(NSUInteger)before
{
	(void)resources; (void)count; (void)after; (void)before;
}
- (void)textureBarrier { }
- (void)updateFence:(id)fence afterStages:(NSUInteger)stages
{
	(void)fence; (void)stages;
}
- (void)waitForFence:(id)fence beforeStages:(NSUInteger)stages
{
	(void)fence; (void)stages;
}

/* Answers about this pass, and they are answers rather than placeholders: the attachments
 * are real textures in real memory (nothing memoryless), and there is no tile pipeline at
 * all, so there is no tile size to report. Zero is the only honest number for a feature
 * that does not exist -- a plausible 32x32 would be a claim, and the device already answers
 * no to every tile-shading query, so a caller that asks this has already been told. */
- (BOOL)isMemorylessRender { return NO; }
- (NSUInteger)tileWidth    { return 0; }
- (NSUInteger)tileHeight   { return 0; }

/* Refused, each with its name in the log once.
 *
 * Tessellation: the library has no tessellator, so a patch draw records nothing. Said out
 * loud because a draw that silently does nothing is the one failure this driver has already
 * paid for twice (docs/metal.md 10.21, 10.26). */
- (void)annibaleNoTessellation
{
	RW_ONCE("tessellation is not implemented; this patch draw records "
		"nothing\n");
}
- (void)drawPatches:(NSUInteger)controlPoints
	 patchStart:(NSUInteger)start
	 patchCount:(NSUInteger)count
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
      instanceCount:(NSUInteger)instances
       baseInstance:(NSUInteger)baseInstance
{
	(void)controlPoints; (void)start; (void)count; (void)indexBuffer; (void)offset;
	(void)instances; (void)baseInstance;
	[self annibaleNoTessellation];
}
- (void)drawPatches:(NSUInteger)controlPoints
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
     indirectBuffer:(id)indirectBuffer
indirectBufferOffset:(NSUInteger)indirectOffset
{
	(void)controlPoints; (void)indexBuffer; (void)offset; (void)indirectBuffer;
	(void)indirectOffset;
	[self annibaleNoTessellation];
}
- (void)drawIndexedPatches:(NSUInteger)controlPoints
		patchStart:(NSUInteger)start
		patchCount:(NSUInteger)count
	  patchIndexBuffer:(id)indexBuffer
    patchIndexBufferOffset:(NSUInteger)offset
   controlPointIndexBuffer:(id)controlPointBuffer
controlPointIndexBufferOffset:(NSUInteger)controlPointOffset
	     instanceCount:(NSUInteger)instances
	      baseInstance:(NSUInteger)baseInstance
{
	(void)controlPoints; (void)start; (void)count; (void)indexBuffer; (void)offset;
	(void)controlPointBuffer; (void)controlPointOffset; (void)instances;
	(void)baseInstance;
	[self annibaleNoTessellation];
}
- (void)drawIndexedPatches:(NSUInteger)controlPoints
	  patchIndexBuffer:(id)indexBuffer
    patchIndexBufferOffset:(NSUInteger)offset
   controlPointIndexBuffer:(id)controlPointBuffer
controlPointIndexBufferOffset:(NSUInteger)controlPointOffset
	    indirectBuffer:(id)indirectBuffer
      indirectBufferOffset:(NSUInteger)indirectOffset
{
	(void)controlPoints; (void)indexBuffer; (void)offset; (void)controlPointBuffer;
	(void)controlPointOffset; (void)indirectBuffer; (void)indirectOffset;
	[self annibaleNoTessellation];
}
- (void)setTessellationFactorBuffer:(id)buffer
			     offset:(NSUInteger)offset
		     instanceStride:(NSUInteger)stride
{
	(void)buffer; (void)offset; (void)stride;
	[self annibaleNoTessellation];
}
- (void)setTessellationFactorScale:(float)scale
{
	(void)scale;
	[self annibaleNoTessellation];
}

/* Indirect command buffers: a buffer of commands the GPU reads for itself. The device
 * answers no to every indirect-command-buffer query, so this is a path a correct caller
 * does not take; the draws in it would be lost silently if it did. */
- (void)annibaleNoIndirectCommands
{
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
- (void)executeCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	[self annibaleNoIndirectCommands];
}
- (void)executeCommandsInBuffer:(id)buffer
		 indirectBuffer:(id)indirectBuffer
	   indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)indirectBuffer; (void)offset;
	[self annibaleNoIndirectCommands];
}

/* Tile shading and imageblocks: a whole second pipeline kind (a tile function that runs over
 * the pass's imageblock), which this driver does not have. Every binding of it is refused
 * together, because one of them answering and the rest not would be worse than all of them
 * refusing: a tile dispatch would then run with half its bindings. */
- (void)annibaleNoTileShading
{
	RW_ONCE("tile shading and imageblocks are not implemented; this tile "
		"binding or dispatch is dropped\n");
}
- (void)dispatchThreadsPerTile:(MTLSize)size
{
	(void)size;
	[self annibaleNoTileShading];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length
			    offset:(NSUInteger)offset
			   atIndex:(NSUInteger)index
{
	(void)length; (void)offset; (void)index;
	[self annibaleNoTileShading];
}
- (void)setTileBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self annibaleNoTileShading];
}
- (void)setTileBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self annibaleNoTileShading];
}
- (void)setTileBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self annibaleNoTileShading];
}
- (void)setTileBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self annibaleNoTileShading];
}
- (void)setTileTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self annibaleNoTileShading];
}
- (void)setTileTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self annibaleNoTileShading];
}
- (void)setTileSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self annibaleNoTileShading];
}
- (void)setTileSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self annibaleNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self annibaleNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self annibaleNoTileShading];
}

/* Function tables: pointers to shader functions, which an AIR module of ours never names
 * and the device answers no to (no ray tracing, no function pointers). A table bound here
 * would be a table a shader cannot call. */
- (void)annibaleNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
- (void)setVertexVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoFunctionTables];
}
- (void)setVertexVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoFunctionTables];
}
- (void)setFragmentVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoFunctionTables];
}
- (void)setFragmentVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoFunctionTables];
}
- (void)setTileVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoFunctionTables];
}
- (void)setTileVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTables:(const id *)tables
			      withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoFunctionTables];
}
- (void)setTileIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoFunctionTables];
}
- (void)setTileIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoFunctionTables];
}

/* A line width other than 1 would change the picture, so it is said; the rasterizer this
 * driver drives has one width. */
- (void)setLineWidth:(float)width
{
	if (width != 1.0f) {
		RW_ONCE("a line width of %g is not implemented; lines are one "
			"pixel wide\n", (double)width);
	}
}

/* Vertex amplification: one vertex shader run writing several positions (what stereo
 * rendering uses). The device answers 1 to -supportsVertexAmplificationCount:, so the only
 * mode a correct caller sets is the one that amplifies nothing. */
- (void)setVertexAmplificationMode:(NSUInteger)mode value:(NSUInteger)value
{
	if (value > 1) {
		RW_ONCE("vertex amplification to %lu views is not implemented; "
			"this pass draws one\n", (unsigned long)value);
	}
	(void)mode;
}

/* Counter sampling: the device has no counter sets (every query answers nil or zero), so
 * there is no sample buffer this could write into. */
- (void)sampleCountersInBuffer:(id)buffer
		  atSampleIndex:(NSUInteger)index
		    withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;
}

/*
 * MESH AND OBJECT STAGE BINDINGS, AND WHY THEY ARE HERE AT ALL.
 *
 * These 26 are not missing from the chain: _MTLCommandEncoder has an entry for every one of
 * them. Each entry is one of Apple's ABSTRACT methods -- a body that calls
 * doesNotRecognizeSelector: -- and that is the trap docs/metal.md 10.1 was written about:
 * `respondsToSelector:` answers YES, the method list lists it, and the abort when it is
 * called names the RECEIVER, so the log would read "-[AnnibaleMetalRenderEncoder
 * setMeshTexture:atIndex:]: unrecognized selector" and send the next reader looking for a
 * method that is not the problem. An entry in a method table is not behaviour, and the only
 * way to tell these apart from working methods is to read the bodies out of the cache
 * (tools/metal/protometh.py --verdicts, which found exactly 28 of them under this driver's
 * classes: these, -commitWithDeadline: and -setThreadgroupPackingDisabled:).
 *
 * A mesh pipeline is two stages before the fragment one -- an object stage that decides how
 * much geometry to make and a mesh stage that makes it. This driver compiles a vertex and a
 * fragment function and nothing else, and the device answers no when asked whether mesh
 * shaders are supported, so there is no stage for any of this to bind to. They are refused
 * together for the same reason the tile bindings are: half of a stage's bindings arriving
 * would be worse than none.
 */
- (void)annibaleNoMeshStages
{
	RW_ONCE("mesh and object shader stages are not implemented (the device "
		"answers no to them); this binding is dropped\n");
}
- (void)setObjectBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectBuffers:(const id *)buffers
		 offsets:(const NSUInteger *)offsets
	       withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setObjectBytes:(const void *)bytes
		length:(NSUInteger)length
	       atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler
		  lodMinClamp:(float)lodMin
		  lodMaxClamp:(float)lodMax
		      atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers
		  lodMinClamps:(const float *)lodMin
		  lodMaxClamps:(const float *)lodMax
		     withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)length; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setObjectIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setMeshBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setMeshBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setMeshBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setMeshBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setMeshTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setMeshTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setMeshVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setMeshVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoMeshStages];
}
- (void)setMeshIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoMeshStages];
}
/*
 * The last four of the mesh group are OPTIONAL members of <MTLRenderCommandEncoderSPI>, and
 * optional is the worse case rather than the lighter one: an optional method is the one
 * Apple asks about with respondsToSelector: before sending it, and an abstract entry in
 * _MTLCommandEncoder makes that question answer YES. So the probe passes, the call goes
 * out, and the abort names our class for a method we were never asked to have. Overriding
 * them turns the yes into a yes that is true.
 */
- (void)setMeshIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setMeshAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self annibaleNoMeshStages];
}
- (void)setObjectAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self annibaleNoMeshStages];
}

/* A split handler is called when Metal splits one render pass into several submissions,
 * which this driver never does: a pass is one draw list and one submission. */
- (void)addSplitHandler:(void (^)(id, NSUInteger, NSUInteger))handler
{
	(void)handler;
	RW_ONCE("a render-pass split handler was installed; this driver never "
		"splits a pass, so it will not be called\n");
}

/* _MTLCommandEncoder releases its command-buffer reference in endEncoding.
 * Pending load actions and resolves must be recorded before that release. */
- (void)endEncoding
{
	if (_ended) return;
	[self recordPassClear];
	[self recordRGB2PPass:NO];
	rw_record_resolve_blits(_commandBuffer, _colorCount, _color, _resolveTo, _colorLevel, _colorSlice, _resolveLevel, _resolveSlice);
	_ended = YES;
	[super endEncoding];
	_commandBuffer = nil;
}

- (void)dealloc
{
	for (unsigned i = 0; i < 8; i++) {
		[_color[i] release];
		[_resolve[i] release];
		[_resolveTo[i] release];
	}
	[_depth release];
	[super dealloc];
}

@end

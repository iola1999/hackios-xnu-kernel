@implementation AnnibaleMetalRenderEncoder {
	AnnibaleMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder */
	AnnibaleMetalRenderPipeline *_pipeline;
	id _color[8];
	unsigned _colorCount;
	id _resolve[8];   /* the resolveTexture of each colour attachment, or nil */
	id _resolveTo[8];
	unsigned _resolveLevel[8], _resolveSlice[8];
	unsigned _colorLevel[8], _colorSlice[8];
	float _clearColor[8][4];
	BOOL _clears;
	unsigned _discards;   /* MTLLoadActionDontCare (bit i), MTLLoadActionClear (bit 8 + i) */
	id _depth;
	id _depthStencilState;
	float _clearDepth;
	unsigned _clearStencil;
	BOOL _clearsDepth, _clearsStencil;
	unsigned _stencilReference[2];
	float _depthRange[2];
	BOOL _hasDepthRange;
	float _viewportRect[4];
	BOOL _hasViewportRect;
	unsigned _scissorRect[4];
	BOOL _hasScissorRect;
	id _vertexBuffers[RW_MAX_BUFFERS];
	NSUInteger _vertexBufferOffsets[RW_MAX_BUFFERS];
	id _fragmentBuffers[RW_MAX_BUFFERS];
	NSUInteger _fragmentBufferOffsets[RW_MAX_BUFFERS];
	id _fragmentTextures[RW_MAX_BUFFERS], _vertexTextures[RW_MAX_BUFFERS];
	id _fragmentSamplers[RW_MAX_BUFFERS], _vertexSamplers[RW_MAX_BUFFERS];
	unsigned _vertexBufferCount, _fragmentBufferCount;
	unsigned _fragmentTextureCount, _fragmentSamplerCount, _vertexTextureCount, _vertexSamplerCount;
	BOOL _ended, _complained;
}

- (instancetype)initWithAnnibaleCommandBuffer:(AnnibaleMetalCommandBuffer *)commandBuffer
				  descriptor:(MTLRenderPassDescriptor *)descriptor
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	/* What the pass is drawing into, and whether it begins by clearing. Metal keeps this in
	 * the pass descriptor rather than in the pipeline, so it belongs to the encoder. */
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *attachment =
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		id texture = attachment != nil ? [attachment texture] : nil;
		if (texture == nil) {
			continue;
		}
		_color[i] = rw_attachment_level(texture, [attachment level]);
		_colorCount = i + 1;
		_colorLevel[i] = (unsigned)[attachment level];
		_colorSlice[i] = (unsigned)[attachment slice];
		_resolve[i] = [attachment resolveTexture];
		NSUInteger store = [attachment storeAction];
		if (_resolve[i] != nil && rw_one_sample() && [texture sampleCount] > 1) {
			_color[i] = rw_attachment_level(_resolve[i], [attachment resolveLevel]);
			_resolve[i] = nil;
		} else if (store == MTLStoreActionMultisampleResolve || store == MTLStoreActionStoreAndMultisampleResolve) {
			if (_resolve[i] != nil) {
				if ([texture sampleCount] <= 1 && [(id<MTLTexture>)_resolve[i] sampleCount] <= 1 &&
				    [_resolve[i] pixelFormat] == [texture pixelFormat]) {
					_resolveTo[i] = [_resolve[i] retain];
					_resolveLevel[i] = (unsigned)[attachment resolveLevel];
					_resolveSlice[i] = (unsigned)[attachment resolveSlice];
					_resolve[i] = nil;
				}
			} else {
				rw_print("colour attachment %u asks for storeAction %lu with no "
				    "resolveTexture: attachment stored without resolve\n",
				    i, (unsigned long)store);
			}
		}
		[_color[i] retain];
		[_resolve[i] retain];
		if ([attachment loadAction] == MTLLoadActionDontCare) _discards |= 1u << i;
		if ([attachment loadAction] == MTLLoadActionClear) {
			_discards |= 1u << (8 + i);
			MTLClearColor c = [attachment clearColor];
			_clearColor[i][0] = (float)c.red; _clearColor[i][1] = (float)c.green;
			_clearColor[i][2] = (float)c.blue; _clearColor[i][3] = (float)c.alpha;
			_clears = YES;
		}
	}
	/*
	 * The depth/stencil attachment, which is two descriptors over one texture: for
	 * MTLPixelFormatDepth32Float_Stencil8 an application sets the same texture on
	 * both, and for a depth-only format it sets only the first. So whichever of the
	 * two carries a texture is the attachment, and both carrying DIFFERENT textures is
	 * refused out loud -- a Gallium framebuffer has one zsbuf, and quietly taking the
	 * depth one would drop the stencil half of what was asked for.
	 *
	 * Each aspect keeps its own load action, because the one struct Metal has behind
	 * both descriptors holds clearDepth and clearStencil separately
	 * (MTLRenderPassAttachmentDescriptorPrivate) and a pass that clears the depth and
	 * loads the stencil is an ordinary pass.
	 */
	MTLRenderPassDepthAttachmentDescriptor *za = [descriptor depthAttachment];
	MTLRenderPassStencilAttachmentDescriptor *sa = [descriptor stencilAttachment];
	id ztex = [za texture];
	id stex = [sa texture];

	if (ztex != nil && stex != nil && ztex != stex) {
		rw_print("the depth attachment and the stencil attachment are two "
		         "different textures; one texture carries both aspects here, so this "
		         "pass is encoded with neither\n");
	} else {
		_depth = [(ztex != nil ? ztex : stex) retain];
		if (ztex != nil && [za loadAction] == MTLLoadActionClear) {
			_clearDepth = (float)[za clearDepth];
			_clearsDepth = YES;
		}
		if (stex != nil && [sa loadAction] == MTLLoadActionClear) {
			_clearStencil = (unsigned)[sa clearStencil];
			_clearsStencil = YES;
		}
	}
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)setRenderPipelineState:(id)state { _pipeline = state; }

/*
 * One table, two meanings, and the offset is no longer thrown away.
 *
 * Metal keeps vertex streams and the vertex stage's [[buffer(n)]] arguments in the SAME
 * table, and which an index is depends on the pipeline's vertex descriptor -- which may
 * not even be set yet when this is called. So the offset is RECORDED here and judged at
 * the draw: for a stream it is Metal's own `offset:` and goes to the library, and for a
 * buffer argument it is still unimplemented and the draw says so by name, having the
 * pipeline's own mask to tell the two apart. Dropping it here is what would draw the
 * wrong mesh out of a buffer that holds several.
 */
- (void)setVertexBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("vertex buffer %lu is past this driver's limit of %u\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		_complained = YES;
		return;
	}
	_vertexBuffers[index] = buffer;
	_vertexBufferOffsets[index] = offset;
	if (index + 1 > _vertexBufferCount) {
		_vertexBufferCount = (unsigned)index + 1;
	}
}

/*
 * And the fragment stage's table, which is now the same shape as the vertex one: the
 * offset is RECORDED and judged at the draw rather than refused here.
 *
 * It used to be refused on the spot, because the contract bound whole buffers and
 * ignoring an offset would read the wrong bytes quietly. An ARGUMENT BUFFER is bound
 * through this very call with an offset of its own -- Metal lets one allocation hold many
 * encodings and the offset is which one -- and whether index n is a set or a plain buffer
 * argument is the pipeline's answer and may not be known yet when this is called. So the
 * draw decides: a set takes the offset, a buffer argument with a non-zero offset is still
 * refused there, by name.
 */
- (void)setFragmentBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("fragment buffer %lu is past this driver's limit of %u\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		_complained = YES;
		return;
	}
	_fragmentBuffers[index] = buffer;
	_fragmentBufferOffsets[index] = offset;
	if (index + 1 > _fragmentBufferCount) {
		_fragmentBufferCount = (unsigned)index + 1;
	}
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setVertexBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setFragmentBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

/* A texture or sampler slot of either stage; the count is the highest bound index plus one. */
static void
rw_bind_slot(id *slots, unsigned *count, id object, NSUInteger index)
{
	if (index >= RW_MAX_BUFFERS)
		return;
	slots[index] = object;
	*count = MAX(*count, (unsigned)index + 1);
}

- (void)setFragmentTexture:(id)texture atIndex:(NSUInteger)index
{
	rw_bind_slot(_fragmentTextures, &_fragmentTextureCount, texture, index);
}
- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	rw_bind_slot(_fragmentSamplers, &_fragmentSamplerCount, sampler, index);
}

/*
 * The viewport: the rectangle and the depth range, both recorded here and both applied by the
 * library's draw.
 *
 * The rectangle is what places a layer's content. CoreAnimation renders a layer into an
 * attachment larger than the layer and puts the layer in it with this rectangle; a draw that
 * covers the whole attachment instead puts the content at the wrong size and position. The
 * encoder does not check the rectangle against the attachment -- the library clamps it, since
 * the attachment it needs is the one the draw is bound to at commit time.
 *
 * The depth range is the only thing in Metal that moves a quad in z through this driver -- the
 * generated vertex stage puts all four corners at clip z = 0, and Metal's NDC z starts at 0 on
 * the near plane, so the window depth of what is drawn is znear exactly. Without it every draw
 * here would be at depth 0 and no depth test could ever be measured on the device.
 */
- (void)setViewport:(MTLViewport)viewport
{
	_viewportRect[0] = (float)viewport.originX;
	_viewportRect[1] = (float)viewport.originY;
	_viewportRect[2] = (float)viewport.width;
	_viewportRect[3] = (float)viewport.height;
	_hasViewportRect = YES;
	_depthRange[0] = (float)viewport.znear;
	_depthRange[1] = (float)viewport.zfar;
	_hasDepthRange = YES;
}
/* Metal's clip rectangle, recorded like the viewport. A pass that never sets one has no
 * clip, which is Metal's default; the library turns the rectangle into the rasterizer
 * state's scissor and the same pixels from the same corner.
 */
- (void)setScissorRect:(MTLScissorRect)rect
{
	_scissorRect[0] = (unsigned)rect.x;
	_scissorRect[1] = (unsigned)rect.y;
	_scissorRect[2] = (unsigned)rect.width;
	_scissorRect[3] = (unsigned)rect.height;
	_hasScissorRect = YES;
}
- (void)setCullMode:(NSUInteger)mode                  { (void)mode; }
- (void)setFrontFacingWinding:(NSUInteger)winding     { (void)winding; }
- (void)setTriangleFillMode:(NSUInteger)mode          { (void)mode; }

/* The depth/stencil state, recorded like the pipeline state: nothing runs at encode time,
 * so what a draw tests against is whatever was bound when that draw was encoded. A state
 * from another driver -- or nil, which is Metal's "no test, no write" -- is passed on as
 * nil, and the library draws with its own zeroed state, which is the same thing. */
- (void)setDepthStencilState:(id)state
{
	if (state != nil && ![state isKindOfClass:[AnnibaleMetalDepthStencilState class]]) {
		rw_print("this depth/stencil state is a %s and not one of ours, so "
		         "it cannot be bound; this pass draws with no depth test\n",
		         class_getName(object_getClass(state)));
		state = nil;
	}
	_depthStencilState = state;
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
	(void)r; (void)g; (void)b; (void)a;
}

- (void)recordPassClear
{
	if (_ended || (!_clears && !_clearsDepth && !_clearsStencil)) return;
	AnnibaleMetalWork *work = [[AnnibaleMetalWork alloc] init];
	work->copyKind = RW_COPY_CLEAR;
	work->colorCount = _colorCount;
	for (unsigned i = 0; i < _colorCount; i++) work->color[i] = _color[i];
	work->clears = _clears;
	work->discards = _discards;
	for (unsigned i = 0; i < 8; i++) {
		for (unsigned c = 0; c < 4; c++)
			work->clearColor[i][c] = _clearColor[i][c];
	}
	work->depth = _depth;
	work->depthStencilState = _depthStencilState;
	work->clearDepth = _clearDepth;
	work->clearStencil = _clearStencil;
	work->clearsDepth = _clearsDepth;
	work->clearsStencil = _clearsStencil;
	_clears = _clearsDepth = _clearsStencil = NO;
	_discards = 0;
	[_commandBuffer recordWork:work];
	[work release];
}

- (void)recordDraw:(NSUInteger)primitive
       vertexStart:(NSUInteger)start
	     count:(NSUInteger)count
       indexBuffer:(id)indexBuffer
	 indexType:(NSUInteger)indexType
       indexOffset:(NSUInteger)indexOffset
	baseVertex:(NSInteger)baseVertex
     instanceCount:(NSUInteger)instances
      baseInstance:(NSUInteger)baseInstance
{
	if (_ended) return;
	[self recordPassClear];
	if (count == 0 || instances == 0) return;
	AnnibaleMetalWork *work = [[AnnibaleMetalWork alloc] init];

	if (_pipeline == nil || (_colorCount == 0 && _depth == nil)) {
		rw_print("this draw has %s and is dropped\n",
		         _pipeline == nil ? "no pipeline state" : "nothing to draw into");
		[work release];
		return;
	}
	/*
	 * What the contract carries in 32 bits and Metal hands over in 64. These are not
	 * limits of this driver's choosing: `index_bias` in Gallium is a signed int and
	 * instance counts there are unsigned, so a number that does not fit is a number
	 * that would be recorded as a DIFFERENT number -- a base vertex of 2^32 as 0, and
	 * an instance count of 2^32 + 3 as 3. Saying so is the only answer that is not a
	 * wrong picture.
	 */
	if (baseVertex < INT32_MIN || baseVertex > INT32_MAX || instances > UINT32_MAX ||
	    baseInstance > UINT32_MAX || count > UINT32_MAX || start > UINT32_MAX) {
		rw_print("this draw does not fit the contract's 32 bits (count %lu, "
		         "start %lu, base vertex %ld, %lu instances from %lu) and is dropped "
		         "rather than wrapped\n", (unsigned long)count, (unsigned long)start,
		         (long)baseVertex, (unsigned long)instances, (unsigned long)baseInstance);
		[work release];
		return;
	}
	/* Said at the draw and not at the pass, because it is the pipeline that knows:
	 * Metal drops a [[depth]] write when there is no depth attachment, and so does
	 * this -- but a caller measuring such a shader would otherwise be left reading a
	 * colour attachment that holds its clear value and nothing else. */
	if ([_pipeline writesDepth] && _depth == nil && !_complained) {
		rw_print("this pipeline's fragment shader stores to [[depth]] and "
		         "this pass has no depth attachment, so those writes go nowhere\n");
		_complained = YES;
	}
	work->render = _pipeline;
	work->discards = _discards;
	_discards = 0;
	work->colorCount = _colorCount;
	for (unsigned i = 0; i < _colorCount; i++) {
		work->color[i] = _color[i];
		work->resolve[i] = _resolve[i];
	}
	work->depth = _depth;
	work->depthStencilState = _depthStencilState;
	work->stencilReference[0] = _stencilReference[0];
	work->stencilReference[1] = _stencilReference[1];
	work->depthRange[0] = _depthRange[0];
	work->depthRange[1] = _depthRange[1];
	work->hasDepthRange = _hasDepthRange;
	for (unsigned i = 0; i < 4; i++) {
		work->viewportRect[i] = _viewportRect[i];
	}
	work->hasViewportRect = _hasViewportRect;
	for (unsigned i = 0; i < 4; i++) {
		work->scissorRect[i] = _scissorRect[i];
	}
	work->hasScissorRect = _hasScissorRect;
	work->primitive = (unsigned)primitive;
	work->vertexStart = (unsigned)start;
	work->vertexCount = (unsigned)count;
	work->indexBuffer = indexBuffer;
	work->indexType = (unsigned)indexType;
	work->indexBufferOffset = indexOffset;
	work->baseVertex = (int)baseVertex;
	work->instanceCount = (unsigned)instances;
	work->baseInstance = (unsigned)baseInstance;
	work->vertexBufferCount = _vertexBufferCount;
	for (unsigned i = 0; i < _vertexBufferCount; i++) {
		work->vertexBuffers[i] = _vertexBuffers[i];
		work->vertexBufferOffsets[i] = (size_t)_vertexBufferOffsets[i];
	}
	work->count = _fragmentBufferCount;
	for (unsigned i = 0; i < _fragmentBufferCount; i++) {
		work->buffers[i] = _fragmentBuffers[i];
		work->bufferOffsets[i] = (size_t)_fragmentBufferOffsets[i];
	}
	work->textureCount = _fragmentTextureCount;
	memcpy(work->textures, _fragmentTextures, _fragmentTextureCount * sizeof(id));
	work->samplerCount = _fragmentSamplerCount;
	memcpy(work->samplers, _fragmentSamplers, _fragmentSamplerCount * sizeof(id));
	work->vertexTextureCount = _vertexTextureCount;
	memcpy(work->vertexTextures, _vertexTextures, _vertexTextureCount * sizeof(id));
	work->vertexSamplerCount = _vertexSamplerCount;
	memcpy(work->vertexSamplers, _vertexSamplers, _vertexSamplerCount * sizeof(id));
	rw_pipeline_note(_pipeline, _colorCount, _depth != nil ? 1u : 0u,
	                 _colorCount > 0 ? (unsigned)[_color[0] width] : 0u,
	                 _colorCount > 0 ? (unsigned)[_color[0] height] : 0u,
	                 _colorCount > 0 ? (unsigned)[_color[0] sampleCount] : 0u);
	[_commandBuffer recordWork:work];
	[work release];
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:1 baseInstance:0];
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
	 instanceCount:(NSUInteger)instances
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:instances baseInstance:0];
}

/*
 * The rest of what MTLRenderCommandEncoder requires. Each is here for the reason the
 * device's own 44 were: a selector that is absent aborts the caller, and an application's
 * encoder sends a good many of these without thinking about it.
 *
 * The division is deliberate. A form that can be served by the single-binding path forwards
 * to it. A form that cannot be served -- the INDIRECT draws, whose whole argument list is
 * read out of a buffer the GPU may be writing -- DECLINES out loud, because drawing the
 * wrong primitives is worse than drawing none. A form that is merely state we do not vary
 * yet (store actions: we always store; residency: everything is resident) is a no-op, and
 * that is said here rather than guessed at by the next reader.
 */
- (void)setVertexBuffers:(const id *)buffers
		 offsets:(const NSUInteger *)offsets
	       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

- (void)setFragmentBuffers:(const id *)buffers
		   offsets:(const NSUInteger *)offsets
		 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

/* The offset alone, which an application changes between draws out of one buffer. It is
 * the same table entry -setVertexBuffer:offset:atIndex: filled, so it is the same field:
 * for a stream the library honours it, and for a buffer argument the draw says it cannot
 * (it has the pipeline's mask and this method has not). */
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_vertexBufferOffsets[index] = offset;
	if (index + 1 > _vertexBufferCount) {
		_vertexBufferCount = (unsigned)index + 1;
	}
}

/* The same for the fragment stage, and now with the same answer as the vertex one: the
 * offset is recorded against the index already bound there, and the draw judges it. An
 * application that moves an argument buffer's offset between draws -- which is the
 * ordinary way to walk a table of encodings -- is doing nothing this driver has to
 * refuse. */
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_fragmentBufferOffsets[index] = offset;
	if (index + 1 > _fragmentBufferCount) {
		_fragmentBufferCount = (unsigned)index + 1;
	}
}

/* The vertex stage's textures, which the Space wallpaper's mesh vertex shader samples. */
- (void)setVertexTexture:(id)texture atIndex:(NSUInteger)index
{
	rw_bind_slot(_vertexTextures, &_vertexTextureCount, texture, index);
}
- (void)setVertexTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++)
		[self setVertexTexture:textures[i] atIndex:range.location + i];
}
- (void)setVertexSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	rw_bind_slot(_vertexSamplers, &_vertexSamplerCount, sampler, index);
}
- (void)setVertexSamplerState:(id)sampler
		  lodMinClamp:(float)lodMin
		  lodMaxClamp:(float)lodMax
		      atIndex:(NSUInteger)index
{
	(void)lodMin; (void)lodMax;
	[self setVertexSamplerState:sampler atIndex:index];
}
- (void)setVertexSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++)
		[self setVertexSamplerState:samplers[i] atIndex:range.location + i];
}
- (void)setFragmentTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++)
		[self setFragmentTexture:textures[i] atIndex:range.location + i];
}
- (void)setFragmentSamplerState:(id)sampler
		    lodMinClamp:(float)lodMin
		    lodMaxClamp:(float)lodMax
			atIndex:(NSUInteger)index
{
	(void)lodMin; (void)lodMax;
	[self setFragmentSamplerState:sampler atIndex:index];
}
- (void)setFragmentSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++)
		[self setFragmentSamplerState:samplers[i] atIndex:range.location + i];
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
	 instanceCount:(NSUInteger)instances
	  baseInstance:(NSUInteger)base
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:instances baseInstance:base];
}

/*
 * Indexed drawing, which is what every drawing call in a real application is. All three
 * direct forms are one call here, because that is what they are in Metal: the shorter two
 * are the longest with instanceCount 1, baseVertex 0 and baseInstance 0, and Metal's own
 * header documents them that way (the parameter lists are identical as far as they go).
 *
 * Nothing is checked here except what the LIBRARY cannot check: that there is an index
 * buffer at all, and that it is one of ours (its Gallium resource is what the draw needs,
 * and a buffer allocated on the kernel path has none -- -mesaBuffer is nil for it, and
 * nil in the draw would read as "not indexed", which is the vertices in the wrong order).
 * The index type, the offset against the index size, the count against the buffer's
 * length and the indices against the quad are all refused BY NAME in air_metal.c, and
 * that refusal reaches the application as the command buffer's NSError, with the numbers
 * in it. Two readers of the same rule is how the two drift apart.
 */
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
		instanceCount:(NSUInteger)instances
		   baseVertex:(NSInteger)baseVertex
		 baseInstance:(NSUInteger)baseInstance
{
	if (buffer == nil) {
		rw_print("an indexed draw with no index buffer is refused rather "
		         "than drawn as if the indices were 0, 1, 2, ...\n");
		return;
	}
	[self recordDraw:primitive vertexStart:0 count:count indexBuffer:buffer
		indexType:type indexOffset:offset baseVertex:baseVertex
	    instanceCount:instances baseInstance:baseInstance];
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
{
	[self drawIndexedPrimitives:primitive indexCount:count indexType:type
			indexBuffer:buffer indexBufferOffset:offset instanceCount:1
			 baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
		instanceCount:(NSUInteger)instances
{
	[self drawIndexedPrimitives:primitive indexCount:count indexType:type
			indexBuffer:buffer indexBufferOffset:offset
		      instanceCount:instances baseVertex:0 baseInstance:0];
}

/*
 * The INDIRECT forms, which were absent from this class altogether until now -- and
 * absent is the one answer that aborts the application, with our class's name on the
 * selector (10.1). They are the fourth -drawIndexedPrimitives: overload and the second
 * -drawPrimitives:, and what they ask for is the draw's whole argument list out of a
 * buffer: MTLDrawPrimitivesIndirectArguments and MTLDrawIndexedPrimitivesIndirect-
 * Arguments, five 32-bit words at an offset.
 *
 * They decline, and the reason is not that the words are hard to read -- the processor
 * can read them, this buffer is mapped. It is that the point of an indirect draw is a
 * count the GPU writes, and this library validates an indexed draw ON THE PROCESSOR
 * before it makes it: the index count against the buffer's length, the indices against
 * the quad. A count read here would be the count at encode time and not the one the GPU
 * will have written, so what came out would be a picture of the wrong number of
 * primitives -- and it would read as a result.
 *
 * Nothing in Metal makes this unreachable for an application, so this says so out loud
 * rather than drawing nothing silently.
 */
- (void)drawPrimitives:(NSUInteger)primitive
	indirectBuffer:(id)indirectBuffer
  indirectBufferOffset:(NSUInteger)offset
{
	rw_print("an indirect draw is not implemented (the argument list would "
	         "be read on the processor at encode time, and the GPU may not have written it "
	         "yet), so this draw is refused rather than drawn from the wrong count\n");
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
	       indirectBuffer:(id)indirectBuffer
	 indirectBufferOffset:(NSUInteger)indirectOffset
{
	[self drawPrimitives:primitive indirectBuffer:indirectBuffer
	       indirectBufferOffset:indirectOffset];
}

/* Raster state the contract does not carry yet. Collected here with one line each so that
 * the next person can see at a glance what the draw ignores. */
- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count
{
	if (count > 1 && !_complained) {
		rw_print("%lu viewports are not implemented; the first one is "
		         "taken\n", (unsigned long)count);
		_complained = YES;
	}
	if (count >= 1 && viewports != NULL) {
		[self setViewport:viewports[0]];
	}
}
- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count
{
	if (count > 1 && !_complained) {
		rw_print("%lu scissor rects are not implemented; the first one is "
		         "taken\n", (unsigned long)count);
		_complained = YES;
	}
	if (count >= 1 && rects != NULL) {
		[self setScissorRect:rects[0]];
	}
}
/* MTLDepthClipModeClip is what the library's rasterizer state does (depth_clip_near and
 * _far are both on); MTLDepthClipModeClamp, which keeps a primitive outside the depth range
 * and clamps its z instead, is a different picture and the contract has nowhere to ask for
 * it. Said once rather than ignored. */
- (void)setDepthClipMode:(NSUInteger)mode
{
	if (mode != MTLDepthClipModeClip && !_complained) {
		rw_print("MTLDepthClipModeClamp is not implemented; this pass clips "
		         "against the depth range instead of clamping to it\n");
		_complained = YES;
	}
}
- (void)setDepthBias:(float)bias slopeScale:(float)scale clamp:(float)clamp
{
	(void)bias; (void)scale; (void)clamp;
}
/* The stencil reference, which Metal keeps on the encoder and not in the state object, so
 * it is recorded with the draw like the clear colour. */
- (void)setStencilReferenceValue:(uint32_t)value
{
	_stencilReference[0] = value;
	_stencilReference[1] = value;
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
	_stencilReference[0] = front;
	_stencilReference[1] = back;
}
- (void)setVisibilityResultMode:(NSUInteger)mode offset:(NSUInteger)offset
{
	(void)mode; (void)offset;
}
- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const void *)mappings
{
	(void)count; (void)mappings;
}

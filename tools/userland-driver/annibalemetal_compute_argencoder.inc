@implementation AnnibaleMetalComputeEncoder {
	AnnibaleMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder's life */
	AnnibaleMetalComputePipeline *_pipeline;
	id _buffers[RW_MAX_BUFFERS];
	NSUInteger _offsets[RW_MAX_BUFFERS];
	id _textures[RW_MAX_BUFFERS];
	id _samplers[RW_MAX_BUFFERS];
	unsigned _highest, _highestTexture, _highestSampler;
	unsigned _threadgroupBytes;
	BOOL _ended, _complained, _warnedLodClamp;
}

- (instancetype)initWithAnnibaleCommandBuffer:(AnnibaleMetalCommandBuffer *)commandBuffer
{
	/* the initialiser IOGPUMetalCommandEncoder sends to super (0x1ec171d14 -> 0x181a1ee6c) */
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)setComputePipelineState:(id)state
{
	_pipeline = state;
}

- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("buffer index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	/* The offset is RECORDED and judged at the dispatch, as the render encoder's is.
	 * An argument buffer is bound through this call with an offset of its own -- which
	 * encoding in that buffer to read -- and a plain buffer argument with one is an
	 * application packing several blocks into an allocation, which is served: the
	 * library takes a `constant` binding at any offset (uploading the block when the
	 * GPU cannot fetch from that offset) and refuses a `device` one the GPU cannot
	 * fetch from, by name, where the shader's own address space is in hand. */
	_buffers[index] = buffer;
	_offsets[index] = offset;
	if (index + 1 > _highest) {
		_highest = (unsigned)index + 1;
	}
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("texture index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	_textures[index] = texture;
	if (index + 1 > _highestTexture) {
		_highestTexture = (unsigned)index + 1;
	}
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("sampler index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	_samplers[index] = sampler;
	if (index + 1 > _highestSampler) {
		_highestSampler = (unsigned)index + 1;
	}
}

/* Metal lets a binding narrow the sampler's lod range. The compiler library takes the range
 * when the sampler is made, so honouring it here would need a second sampler object; until
 * it does, saying nothing would silently sample the wrong level. */
- (void)setSamplerState:(id)sampler
	    lodMinClamp:(float)lodMin
	    lodMaxClamp:(float)lodMax
		atIndex:(NSUInteger)index
{
	if (!_warnedLodClamp && (lodMin != 0.0f || lodMax < 1000.0f)) {
		rw_print("a per-binding lod clamp (%g..%g) is not implemented; the "
		         "sampler's own range is used instead\n", (double)lodMin, (double)lodMax);
		_warnedLodClamp = YES;
	}
	[self setSamplerState:sampler atIndex:index];
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] atIndex:range.location + i];
	}
}

/* Metal's own shorthand for small constant data; it is a buffer here, because that is what
 * the shader's binding is. */
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)size
{
	if (_ended) return;
	AnnibaleMetalWork *work = [[AnnibaleMetalWork alloc] init];

	if (_pipeline == nil || _complained) {
		rw_print("this dispatch has %s and is dropped\n",
		         _pipeline == nil ? "no pipeline state" : "a binding this driver refused");
		[work release];
		return;
	}
	work->pipeline = _pipeline;
	work->count = _highest;
	work->textureCount = _highestTexture;
	work->samplerCount = _highestSampler;
	for (unsigned i = 0; i < _highest; i++) {
		work->buffers[i] = _buffers[i];
		work->bufferOffsets[i] = (size_t)_offsets[i];
	}
	for (unsigned i = 0; i < _highestTexture; i++) {
		work->textures[i] = _textures[i];
	}
	for (unsigned i = 0; i < _highestSampler; i++) {
		work->samplers[i] = _samplers[i];
	}
	work->groups[0] = (unsigned)groups.width;
	work->groups[1] = (unsigned)groups.height;
	work->groups[2] = (unsigned)groups.depth;
	work->size[0] = (unsigned)size.width;
	work->size[1] = (unsigned)size.height;
	work->size[2] = (unsigned)size.depth;
	work->threadgroupBytes = _threadgroupBytes;
	[_commandBuffer recordWork:work];
	[work release];
}

/* Metal counts threads here and threadgroups above. Rounding the grid up would run threads the
 * application did not ask for, and freedreno has no partial threadgroups, so a grid the size does
 * not divide is served only for a kernel any threadgroup size serves alike (-groupFree): per
 * dimension the size becomes the largest divisor of the grid not above the one asked for. */
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)size
{
	NSUInteger t[3] = { threads.width, threads.height, threads.depth };
	NSUInteger g[3] = { size.width, size.height, size.depth };

	for (unsigned i = 0; i < 3; i++)
		while (g[i] > 1 && t[i] % g[i] != 0 && [_pipeline groupFree])
			g[i]--;
	if (g[0] == 0 || g[1] == 0 || g[2] == 0 || t[0] % g[0] || t[1] % g[1] || t[2] % g[2]) {
		rw_print("%lux%lux%lu threads do not divide into groups of %lux%lux%lu, and the kernel "
		         "reads its threadgroup\n", (unsigned long)t[0], (unsigned long)t[1],
		         (unsigned long)t[2], (unsigned long)g[0], (unsigned long)g[1],
		         (unsigned long)g[2]);
		return;
	}
	[self dispatchThreadgroups:MTLSizeMake(t[0] / g[0], t[1] / g[1], t[2] / g[2])
	     threadsPerThreadgroup:MTLSizeMake(g[0], g[1], g[2])];
}

- (void)useResource:(id)resource usage:(NSUInteger)usage { (void)resource; (void)usage; }
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(NSUInteger)usage
{
	(void)resources; (void)count; (void)usage;
}
- (void)memoryBarrierWithScope:(NSUInteger)scope { (void)scope; }
/* The compiler library takes one threadgroup buffer, at index 0 (air_to_nir.c refuses a
 * second), so a length for any other index has nothing to size. */
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	if (index == 0) {
		_threadgroupBytes = (unsigned)length;
	} else if (!_complained) {
		rw_print("a threadgroup memory length for index %lu is not implemented; only "
		         "index 0 is sized\n", (unsigned long)index);
		_complained = YES;
	}
}

/*
 * THE REST OF WHAT <MTLComputeCommandEncoder> AND <MTLComputeCommandEncoderSPI> REQUIRE:
 * 20 methods that had no answer in this chain, each of which was an abort waiting for the
 * first daemon to encode a kernel. Counted by tools/metal/owes.py off the cache, because
 * MTLComputeCommandEncoderSPI is declared in AGXMetalA11.bundle and is in no process here.
 */

/* Served, by forwarding to the singular form -- which is where this driver's refusal of a
 * non-zero buffer offset already lives, so a plural bind inherits it instead of slipping
 * past it. */
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("buffer index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	/* Metal's rule is that this moves a buffer that is already bound at that index, so
	 * with nothing bound there is nothing to move -- and binding nil here would add an
	 * empty slot to the dispatch's count and make it complain about a buffer nobody
	 * asked for. */
	if (_buffers[index] != nil) {
		[self setBuffer:_buffers[index] offset:offset atIndex:index];
	}
}
- (void)setBuffers:(const id *)buffers
	   offsets:(const NSUInteger *)offsets
	 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}
- (void)setSamplerStates:(const id *)samplers
	    lodMinClamps:(const float *)lodMin
	    lodMaxClamps:(const float *)lodMax
	       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] lodMinClamp:lodMin[i]
			  lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}

/* Served by construction: one ring, one submission per command buffer, in commit order, so
 * the ordering a barrier or a fence asks for is the only ordering there is. Residency is
 * the same answer as on the render encoder: every allocation is resident for its lifetime. */
- (void)memoryBarrierWithResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
}
- (void)updateFence:(id)fence  { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }
- (void)useHeap:(id)heap       { (void)heap; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count { (void)heaps; (void)count; }
- (void)sampleCountersInBuffer:(id)buffer
		  atSampleIndex:(NSUInteger)index
		    withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;   /* the device has no counter sets */
}

/* Declined: there is no debug information for a kernel of ours. nil is what Metal's own
 * driver returns when a pipeline was not built with it, so the caller has this path. */
- (id)newKernelDebugInfo { return nil; }

/* Declined, and each says its name once. An indirect dispatch reads its grid out of GPU
 * memory, which means the size is not known when the command is recorded -- this driver
 * records sizes, so there is nothing to record. A dispatch that vanishes without a word is
 * the worst outcome available, hence the line. */
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)buffer
			  indirectBufferOffset:(NSUInteger)offset
			 threadsPerThreadgroup:(MTLSize)size
{
	(void)buffer; (void)offset; (void)size;
	RW_ONCE("an indirect dispatch is not implemented (the grid lives in GPU "
		"memory and this driver records the grid); this dispatch is dropped\n");
}
- (void)executeCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
- (void)executeCommandsInBuffer:(id)buffer
		 indirectBuffer:(id)indirectBuffer
	   indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)indirectBuffer; (void)offset;
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
/* The stage-in region is for a kernel whose arguments arrive as [[stage_in]] -- the
 * non-uniform-grid feature. Nothing in the AIR this driver compiles declares one. */
- (void)annibaleNoStageIn
{
	RW_ONCE("a kernel stage-in region is not implemented; the dispatch "
		"covers the whole grid it was given\n");
}
- (void)setStageInRegion:(MTLRegion)region
{
	(void)region;
	[self annibaleNoStageIn];
}
- (void)setStageInRegionWithIndirectBuffer:(id)buffer
		      indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)offset;
	[self annibaleNoStageIn];
}
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height
{
	(void)width; (void)height;
	RW_ONCE("imageblocks are not implemented; this kernel's imageblock "
		"size is ignored\n");
}
/* An entry for this exists in _MTLCommandEncoder and it is one of Apple's abstract ones
 * (tools/metal/protometh.py --verdicts), so inheriting it is an abort rather than a
 * default. It is a hint about how the hardware packs threadgroups together; the compiler
 * library decides that for itself and there is nothing here to turn off. */
- (void)setThreadgroupPackingDisabled:(BOOL)disabled { (void)disabled; }

- (void)annibaleNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
- (void)setVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoFunctionTables];
}
- (void)setVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoFunctionTables];
}
- (void)setIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self annibaleNoFunctionTables];
}
- (void)setIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self annibaleNoFunctionTables];
}

/* The same as the render encoder's, for the same measured reason: the superclass retains the
 * command buffer in the initialiser and releases it only here, and its -dealloc aborts on an
 * encoder released without this. Nothing has deallocated a compute encoder of ours on the
 * phone yet -- the one that aborted 43 times was the render one, out of warmup_shaders -- so
 * this half is the same fix made before it is measured, not after. */
- (void)endEncoding
{
	if (_ended) return;
	_ended = YES;
	[super endEncoding];
	_commandBuffer = nil;
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * MTLArgumentEncoder, which on this driver is a RECORDER and not a writer.
 *
 * Metal's encoder writes one 8-byte handle per member into a plain buffer, and those
 * handles are an Apple GPU's descriptors: a Gallium driver, which binds by index, cannot
 * read them. It also does not need to -- the AIR frontend resolves every member of every
 * set to a binding while translating (docs/air.md 8.2) -- so each -setBuffer:atIndex:
 * here RECORDS (resource, Metal id) against (the destination buffer, the destination
 * offset) in the compiler library, and the draw binds every member the pipeline's shaders
 * declare from those records. The buffer's contents are never read, and an argument
 * buffer the application filled by any other means is REFUSED BY NAME at the draw.
 *
 * Two things are deliberate about this object:
 *
 *   - IT HOLDS NO RECORDS ITSELF. Its state is (buffer, offset), and the records live
 *     with the destination buffer in the library, because that is what Metal's own object
 *     model allows: one buffer holds several encodings at several offsets, each reused
 *     across many draws, and an application may release the encoder and go on drawing
 *     with what it encoded.
 *   - -setArgumentBuffer:offset: DOES NOT CLEAR the encoding at that offset, which is
 *     Metal's behaviour: the bytes an earlier encoding wrote there are still there, so a
 *     second encoding that sets three of five members leaves the other two as they were.
 *
 * The layout comes from the module (`air.indirect_buffer`), so `encodedLength` and
 * `alignment` are Apple's own numbers -- 8 bytes per member the set declares and 8 --
 * which matters because the application allocates its buffer from them and then encodes
 * at multiples of them.
 */
@implementation AnnibaleMetalArgumentEncoder {
	AnnibaleMetalDevice *_owner;         /* not retained: the device owns everything here */
	NSString *_label;
	struct rwa_argbuf_layout _layout;
	BOOL _fromModule;                   /* the layout came from a shader, not descriptors */
	AnnibaleMetalBuffer *_destination;   /* what -setArgumentBuffer:offset: named */
	NSUInteger _offset;
	BOOL _complained;
}

/* Conformance declared at runtime, as the device's MTLDeviceSPI is and for the same
 * reason: an application (or Metal's own validation) may ask conformsToProtocol: before
 * it sends anything, and the protocol is only in the process once Metal is. Every method
 * it requires is answered below -- some of them by declining out loud, which is an answer
 * and not an absence. */
+ (void)load
{
	Protocol *p = objc_getProtocol("MTLArgumentEncoder");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLArgumentEncoder) refused\n");
	}
}

- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_owner = device;
	_layout = *layout;
	_fromModule = fromModule;
	return self;
}

- (void)dealloc
{
	[_label release];
	[_destination release];
	[super dealloc];
}

- (id)device       { return _owner; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label
{
	[_label release];
	_label = [label copy];
}

/* The two numbers an application allocates from, and they are the module's own: 8 bytes
 * per member the set declares, used by this shader or not (air_metal.h). A length of ours
 * that was too small would have the application overlapping its own encodings. */
- (NSUInteger)encodedLength { return _layout.encoded_length; }
- (NSUInteger)alignment     { return _layout.alignment; }

/* Not part of MTLArgumentEncoder: what this encoder is for, so a test can read back the
 * mapping between Metal's [[id(n)]] and the index the GPU is programmed with instead of
 * trusting it. Metal's own pipeline reflection would report the same thing. */
- (NSUInteger)annibaleMemberCount { return _layout.member_count; }
- (NSUInteger)annibaleMetalIndex  { return _layout.metal_index; }
- (BOOL)annibaleMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index
{
	if (i >= _layout.member_count) {
		return NO;
	}
	if (mid != NULL) {
		*mid = _layout.members[i].id;
	}
	if (kind != NULL) {
		*kind = _layout.members[i].kind;
	}
	if (index != NULL) {
		*index = _layout.members[i].index;
	}
	return YES;
}

/*
 * The destination. Metal checks the offset against the buffer's length and the encoder's
 * alignment, and so does this -- an application that encodes at an unaligned offset has
 * allocated its buffer from something other than -encodedLength, and every record made
 * afterwards would be keyed by an offset the draw will never look up.
 */
- (void)setArgumentBuffer:(id)buffer offset:(NSUInteger)offset
{
	if (buffer != nil && ![buffer isKindOfClass:[AnnibaleMetalBuffer class]]) {
		rw_print("the argument buffer is a %s and not one of ours, so "
		         "nothing can be encoded into it\n",
		         class_getName(object_getClass(buffer)));
		[_destination release];
		_destination = nil;
		return;
	}
	if (buffer != nil && [buffer mesaBuffer] == NULL) {
		rw_print("the argument buffer did not come from the compiler "
		         "library, so nothing can be encoded into it\n");
		[_destination release];
		_destination = nil;
		return;
	}
	if (buffer != nil && _layout.alignment != 0 && offset % _layout.alignment != 0) {
		rw_print("an argument buffer offset of %lu is not a multiple of "
		         "this encoder's alignment (%u), so the draw would look for the "
		         "encoding somewhere else\n", (unsigned long)offset, _layout.alignment);
		[_destination release];
		_destination = nil;
		return;
	}
	/* Retained, unlike the other back-pointers in this file, and for a measured
	 * reason: the records live in the compiler library WITH that buffer, so an
	 * application that released it while this encoder still pointed at it would have
	 * rwa_buffer_destroy free the records (and scrub them out of everyone else's) and
	 * leave this object holding a freed MTLBuffer to send -mesaBuffer to. Metal's own
	 * encoder outliving its argument buffer is an application's mistake; a crash in
	 * the driver is ours. */
	[buffer retain];
	[_destination release];
	_destination = buffer;
	_offset = offset;
}

/* Metal's array form: the same destination with an element's worth of offset added. The
 * arithmetic is Metal's own -- encodedLength per element -- and it is here rather than
 * declined because an application that encodes an ARRAY of sets is doing nothing this
 * driver cannot record. */
- (void)setArgumentBuffer:(id)buffer startOffset:(NSUInteger)offset
	     arrayElement:(NSUInteger)element
{
	[self setArgumentBuffer:buffer offset:offset + element * _layout.encoded_length];
}

/* Whether this encoder is in a state to record, said once per site: an id past the set's
 * own members is the mistake worth naming, because Metal would write a handle into the
 * buffer and the shader would read a descriptor that is not there. */
- (BOOL)annibaleReady:(NSUInteger)index for:(const char *)what
{
	if (_destination == nil) {
		if (!_complained) {
			rw_print("%s was encoded at id %lu with no argument buffer "
			         "set (Metal's -setArgumentBuffer:offset: has to name one "
			         "first)\n", what, (unsigned long)index);
			_complained = YES;
		}
		return NO;
	}
	if (index >= _layout.member_count) {
		/* Which of the two layouts this is matters to whoever reads the line: a
		 * shader's set is the shader's business and an application cannot change
		 * it, while a set described with MTLArgumentDescriptors is the
		 * application's own and the id it just used is one it did not describe. */
		rw_print("%s was encoded at id %lu of %s that declares %u member%s\n", what,
		         (unsigned long)index,
		         _fromModule ? "a set a shader declares" : "an encoder this "
		                       "application described",
		         _layout.member_count, _layout.member_count == 1 ? "" : "s");
		return NO;
	}
	return YES;
}

/*
 * The three that record. The KIND is the call, as it is in Metal; a member encoded with
 * the wrong one is refused at the draw, by name, where the shader's own declaration is in
 * hand -- not here, because one buffer may be encoded for a set this driver has never
 * seen a shader for.
 *
 * The library's sentence is passed through rather than replaced: it names the id and what
 * went wrong with it.
 */
- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	char why[512] = { 0 };

	if (![self annibaleReady:index for:"a buffer"]) {
		return;
	}
	if (buffer != nil && ![buffer isKindOfClass:[AnnibaleMetalBuffer class]]) {
		rw_print("the buffer encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(buffer)));
		return;
	}
	if (mesa.argbuf_encode_buffer([_destination mesaBuffer], _offset, (unsigned)index,
				      [buffer mesaBuffer], offset, why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
}

- (void)setBuffers:(const id *)buffers
	   offsets:(const NSUInteger *)offsets
	 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setBuffer:buffers[i] offset:offsets ? offsets[i] : 0
			atIndex:range.location + i];
	}
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	char why[512] = { 0 };

	if (![self annibaleReady:index for:"a texture"]) {
		return;
	}
	if (texture != nil && ![texture isKindOfClass:[AnnibaleMetalTexture class]]) {
		rw_print("the texture encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(texture)));
		return;
	}
	if (mesa.argbuf_encode_texture([_destination mesaBuffer], _offset, (unsigned)index,
				       [texture mesaTexture], why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	char why[512] = { 0 };

	if (![self annibaleReady:index for:"a sampler"]) {
		return;
	}
	if (sampler != nil && ![sampler isKindOfClass:[AnnibaleMetalSampler class]]) {
		rw_print("the sampler encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(sampler)));
		return;
	}
	if (mesa.argbuf_encode_sampler([_destination mesaBuffer], _offset, (unsigned)index,
				       [sampler mesaSampler], why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] atIndex:range.location + i];
	}
}

/*
 * -constantDataAtIndex: is where an application writes a VALUE member -- an `int` or a
 * `float4` inside the set rather than a descriptor -- and it is answered with the real
 * bytes of the argument buffer at that member's place, which is what Metal answers.
 *
 * Nothing of this driver ever reads those bytes, and that is said here because it is the
 * kind of silence that costs a day: a shader that reads a value out of a set is refused
 * when it is TRANSLATED ("a value read out of the set instead of a descriptor",
 * docs/air.md 8.2.1), so an application writing here is writing into memory no shader of
 * ours will be compiled to read. Answering a real pointer is still the right answer --
 * the write lands in the application's own buffer and nothing is corrupted -- while
 * answering NULL would be a crash in the application's memcpy.
 */
- (void *)constantDataAtIndex:(NSUInteger)index
{
	if (![self annibaleReady:index for:"constant data"]) {
		return NULL;
	}
	void *bytes = [_destination contents];

	if (bytes == NULL) {
		return NULL;
	}
	RW_ONCE("-constantDataAtIndex: hands back the argument buffer's own "
		"bytes, and nothing in this driver reads them: a shader that reads a VALUE "
		"out of an argument buffer is refused when it is translated, because every "
		"member this frontend binds is a descriptor\n");
	return (char *)bytes + _offset + 8 * index;
}

/*
 * A SET INSIDE A SET, declined by name. The frontend refuses a nested `air.indirect_buffer`
 * when it translates the shader -- one module of Resident Evil 4's 7 620 has one, the
 * kernel that encodes draws on the GPU -- so an encoder for one would be an object whose
 * records no pipeline could ever ask for.
 */
- (id)newArgumentEncoderForBufferAtIndex:(NSUInteger)index
{
	rw_print("-newArgumentEncoderForBufferAtIndex:%lu asks for an encoder for "
	         "a set INSIDE a set, and this port's frontend refuses a nested argument buffer "
	         "when it translates the shader, so there is nothing to encode for\n",
	         (unsigned long)index);
	return nil;
}

/*
 * The members that are not a buffer, a texture or a sampler. Every one of them is declined
 * BY NAME and for the same reason: the frontend refuses such a member when it translates
 * the shader ("a member that is not a buffer, a texture or a sampler"), so a record here
 * would be a record no draw could use -- and a silent no-op would leave the application
 * believing its set was encoded.
 */
- (void)annibaleNoSuchMember:(const char *)what atIndex:(NSUInteger)index
{
	rw_print("%s was encoded at id %lu of an argument buffer, and this port "
	         "binds a buffer, a texture or a sampler member and refuses the rest when the "
	         "shader is translated; this encoding is dropped\n", what, (unsigned long)index);
}

- (void)setRenderPipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self annibaleNoSuchMember:"a render pipeline state" atIndex:index];
}
- (void)setRenderPipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self annibaleNoSuchMember:"render pipeline states" atIndex:range.location];
}
- (void)setComputePipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self annibaleNoSuchMember:"a compute pipeline state" atIndex:index];
}
- (void)setComputePipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self annibaleNoSuchMember:"compute pipeline states" atIndex:range.location];
}
- (void)setIndirectCommandBuffer:(id)buffer atIndex:(NSUInteger)index
{
	(void)buffer;
	[self annibaleNoSuchMember:"an indirect command buffer" atIndex:index];
}
- (void)setIndirectCommandBuffers:(const id *)buffers withRange:(NSRange)range
{
	(void)buffers;
	[self annibaleNoSuchMember:"indirect command buffers" atIndex:range.location];
}
- (void)setAccelerationStructure:(id)structure atIndex:(NSUInteger)index
{
	(void)structure;
	[self annibaleNoSuchMember:"an acceleration structure" atIndex:index];
}
- (void)setVisibleFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self annibaleNoSuchMember:"a visible function table" atIndex:index];
}
- (void)setVisibleFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self annibaleNoSuchMember:"visible function tables" atIndex:range.location];
}
- (void)setIntersectionFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self annibaleNoSuchMember:"an intersection function table" atIndex:index];
}
- (void)setIntersectionFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self annibaleNoSuchMember:"intersection function tables" atIndex:range.location];
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * -[MTLFunction newArgumentEncoderWithBufferIndex:], which is a method of METAL'S OWN
 * class and is therefore installed at RUNTIME here rather than written as a category.
 *
 * Two decisions, and the first one is measured. Metal's own implementation cannot work on
 * this port:
 *
 *   -[_MTLFunction newArgumentEncoderWithBufferIndex:] (0x181aef7f4) forwards to
 *   ...reflection:binaryArchives: (0x181aef85c), which asks
 *   -[_MTLFunction reflectionWithOptions:binaryArchives:] (0x181aef458) for the
 *   function's reflection -- and that one calls -compiler, which on this port is
 *   -[_MTLDevice compiler] (0x181aac128), a two-instruction stub answering nil
 *   (docs/metal.md 10.7: everything behind Metal's own compiler is nil here). The
 *   builder then walks `[reflection arguments]`, finds a count of zero, and takes its
 *   failure path (0x181aef924):
 *
 *     MTLReportFailure(0, "-[_MTLFunction newArgumentEncoderWithBufferIndex:reflection:"
 *                      "functionReflection:]", 0x22fc,
 *                      @"bufferIndex %lu does not identify an argument buffer")
 *
 *   and returns nil.
 *
 * So the choice is not between Apple's implementation and ours; it is between nil and an
 * encoder. And what Metal was missing is what this port HAS: -[_MTLFunction bitcodeData]
 * works (docs/metal.md 10.12, checked over all eleven functions of AV1SW.metallib), and
 * the frontend reads the set's layout straight out of those bytes. Resident Evil 4 sends
 * this selector; so does any application that writes `[[id(n)]]` structs in MSL instead
 * of building MTLArgumentDescriptors.
 *
 * The second decision is HOW, and a category is the obvious way and the wrong one: a
 * category on `_MTLFunction` makes this dylib import `_OBJC_CLASS_$__MTLFunction`, and a
 * private class symbol that does not bind is a dylib that does not LOAD -- which would
 * take the whole driver down for every Metal client in the process, not just this
 * selector. The one private class symbol this driver does import,
 * `_OBJC_CLASS_$__MTLDevice`, is imported by IOGPU.framework too and has been measured
 * loading on the phone; `ipsw dyld symaddr -b` finds nothing in the cache that imports
 * _MTLFunction's, so nothing says it would bind. class_replaceMethod needs no symbol at
 * all, and it is the same move ensure_spi already makes for MTLDeviceSPI.
 *
 * What it costs, said plainly: the method is replaced for every MTLFunction in the
 * process, Apple's daemons included. Nothing is lost by that -- the implementation it
 * replaces returns nil for all of them, for the reason above -- and nothing else of
 * _MTLFunction is touched.
 */
static id
rw_function_argument_encoder(id self, SEL cmd, NSUInteger index)
{
	id device = ((id (*)(id, SEL))objc_msgSend)(self, sel_getUid("device"));

	(void)cmd;
	if (![device isKindOfClass:[AnnibaleMetalDevice class]]) {
		rw_print("-newArgumentEncoderWithBufferIndex: on a function whose device is "
		         "a %s and not ours\n", class_getName(object_getClass(device)));
		return nil;
	}
	return [(AnnibaleMetalDevice *)device annibaleArgumentEncoderForFunction:self
								  bufferIndex:index];
}

/* The same with Metal's out-parameter, which is answered nil: a reflection object needs
 * Metal's own compiler, and every pipeline path of this driver already says so. */
static id
rw_function_argument_encoder_reflection(id self, SEL cmd, NSUInteger index, id *reflection)
{
	if (reflection != NULL) {
		*reflection = nil;
	}
	return rw_function_argument_encoder(self, cmd, index);
}

/* Installed once per process, from ensure_spi -- which runs at +load (Metal loaded us, so
 * Metal is there) and again before the first device is made (a library can also be
 * brought in by hand, before Metal). A process without Metal's class simply has no
 * MTLFunction to answer for. */
static void
ensure_function_encoder(void)
{
	static BOOL installed;
	Class fn = objc_lookUpClass("_MTLFunction");

	if (installed || fn == NULL) {
		return;
	}
	installed = YES;
	class_replaceMethod(fn, sel_getUid("newArgumentEncoderWithBufferIndex:"),
			    (IMP)rw_function_argument_encoder, "@@:Q");
	class_replaceMethod(fn, sel_getUid("newArgumentEncoderWithBufferIndex:reflection:"),
			    (IMP)rw_function_argument_encoder_reflection, "@@:Q^@");
}

/* _MTLDevice's own newCommandQueue and newCommandQueueWithMaxCommandBufferCount: are real
 * and both end here, where its body is `mov x0, #0; ret` (0x181aac688) -- a refusal that
 * announces itself as nothing at all. */
/*
 * -[MTLDevice newArgumentEncoderWithArguments:], the path an application takes when it
 * describes the set itself instead of taking it from a shader.
 *
 * Everything this encoder needs is in the descriptors -- how many members, which kind
 * each is, and therefore the length and the alignment -- and that is the whole of the
 * layout, because the records are keyed by Metal's `[[id(n)]]` and the FLAT index each
 * member is bound at comes from the pipeline's own shader at the draw. So an encoder made
 * this way records exactly as one made from a module does.
 *
 * Three shapes are refused BY NAME rather than encoded into something, each because the
 * frontend refuses the matching shader when it translates it (docs/air.md 8.2.1), so a
 * record of ours could never be asked for:
 *
 *   - a member that is not a pointer, a texture or a sampler: a VALUE inside the set,
 *     which this frontend binds nothing for;
 *   - an array of descriptors (arrayLength above 1), which no module in reach declares
 *     and which a shader could index at run time;
 *   - indices with a gap in them. Metal's own encodedLength is over the highest index, so
 *     a set declaring [[id(0)]] and [[id(5)]] is 48 bytes and not 16 -- and this library
 *     answers 8 bytes per member. Saying so is better than handing back a length the
 *     application would allocate too little memory from.
 */
- (id)newArgumentEncoderWithArguments:(NSArray *)arguments
{
	struct rwa_argbuf_layout layout = { 0 };
	NSUInteger count = [arguments count];

	if (mesa.argbuf_encode_buffer == NULL || mesa.module_argbufs == NULL) {
		rw_print("this compiler library has no argument buffers in it, so "
		         "an encoder would record what no draw could read\n");
		return nil;
	}
	if (count == 0 || count > RWA_MAX_ARGBUF_MEMBERS) {
		rw_print("an argument encoder of %lu members: this driver records "
		         "1 to %u\n", (unsigned long)count, RWA_MAX_ARGBUF_MEMBERS);
		return nil;
	}
	layout.metal_index = ~0u;      /* the application binds it where it likes */
	layout.alignment = 8;
	layout.member_count = (unsigned)count;
	layout.encoded_length = 8 * (unsigned)count;

	uint64_t seen = 0;
	for (NSUInteger i = 0; i < count; i++) {
		MTLArgumentDescriptor *d = [arguments objectAtIndex:i];
		NSUInteger index = [d index];
		NSUInteger type = [d dataType];
		NSUInteger array = [d arrayLength];
		unsigned kind;

		switch (type) {
		case MTLDataTypePointer: kind = RWA_ARGBUF_BUFFER; break;
		case MTLDataTypeTexture: kind = RWA_ARGBUF_TEXTURE; break;
		case MTLDataTypeSampler: kind = RWA_ARGBUF_SAMPLER; break;
		default:
			rw_print("member %lu of this argument encoder is "
			         "MTLDataType %lu, and this port binds a buffer, a texture or a "
			         "sampler member: a value inside a set is refused when the "
			         "shader is translated\n", (unsigned long)index,
			         (unsigned long)type);
			return nil;
		}
		if (array > 1) {
			rw_print("member %lu of this argument encoder is an array "
			         "of %lu descriptors, and this port's frontend refuses a member "
			         "spanning more than one descriptor index\n",
			         (unsigned long)index, (unsigned long)array);
			return nil;
		}
		if (index >= count || (seen & (1ull << index)) != 0) {
			rw_print("this argument encoder has %lu members and one of "
			         "them is index %lu: Metal's encodedLength is over the highest "
			         "index, so this driver will not answer a length for indices "
			         "with a gap or a repeat in them\n", (unsigned long)count,
			         (unsigned long)index);
			return nil;
		}
		seen |= 1ull << index;
		layout.members[i].id = (unsigned)index;
		layout.members[i].kind = kind;
		/* The flat index is the SHADER's to allocate, and there is no shader here:
		 * the draw takes it from the pipeline's own resource list, where the member
		 * is named by (set, id) exactly as the application named it. */
		layout.members[i].index = ~0u;
	}
	return [[AnnibaleMetalArgumentEncoder alloc] initWithAnnibaleDevice:self layout:&layout
							      fromModule:NO];
}

/* Direct buffer reflection supplies a declared type and byte layout. An argument
 * encoder also needs argument-buffer member IDs, read through the function path.
 */
- (id)newArgumentEncoderWithBufferBinding:(id)binding
{
	(void)binding;
	rw_print("-newArgumentEncoderWithBufferBinding: requires argument-buffer member IDs; "
	         "ask a function for an encoder (-newArgumentEncoderWithBufferIndex:) or "
	         "describe the set with MTLArgumentDescriptors\n");
	return nil;
}

/* The function path reads the argument-buffer layout from the function's bitcode. */
- (id)annibaleArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (_mesa == NULL || mesa.module_argbufs == NULL) {
		rw_print("no compiler library, so the argument buffer layout of "
		         "this function cannot be read\n");
		return nil;
	}
	if (function == nil || ![function respondsToSelector:bitcode]) {
		rw_print("this function does not answer bitcodeData, so its "
		         "argument buffers cannot be read\n");
		return nil;
	}
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);

	if (air == nil || [air length] == 0) {
		rw_print("this function has no bitcode\n");
		return nil;
	}

	char why[512] = { 0 };
	struct rwa_argbuf_layout sets[8];
	unsigned count = 0;

	if (mesa.module_argbufs(_mesa, [air bytes], [air length],
				[[function name] UTF8String], sets,
				(unsigned)(sizeof(sets) / sizeof(sets[0])), &count, why,
				sizeof(why)) != 0) {
		/* the frontend's own sentence, passed through: it names what it refused */
		rw_print("%s\n", why);
		return nil;
	}
	for (unsigned i = 0; i < count; i++) {
		if (sets[i].metal_index != (unsigned)index) {
			continue;
		}
		return [[AnnibaleMetalArgumentEncoder alloc] initWithAnnibaleDevice:self
									  layout:&sets[i]
								      fromModule:YES];
	}
	/* Metal's own words for this, and its own reason: there is no argument buffer at
	 * that index. Ours says which indices there ARE, because an application that asked
	 * for the wrong one has no other way to find out. */
	char which[96] = "";

	for (unsigned i = 0; i < count; i++) {
		char one[16];

		snprintf(one, sizeof(one), "%s%u", i ? ", " : " at ", sets[i].metal_index);
		strlcat(which, one, sizeof(which));
	}
	rw_print("bufferIndex %lu does not identify an argument buffer: this function "
	         "declares %u%s\n", (unsigned long)index, count, which);
	return nil;
}

- (id)newCommandQueueWithDescriptor:(id)descriptor
{
	/* Apple's own -newCommandQueue and -newCommandQueueWithMaxCommandBufferCount: build a
	 * descriptor and end here, so one usually arrives; when this is called directly with
	 * nil, one is made, because the base class validates it and refuses nil. */
	id use = descriptor != nil ? [[descriptor retain] autorelease] :
	    [[[MTLCommandQueueDescriptor alloc] init] autorelease];

	return [[AnnibaleMetalCommandQueue alloc] initWithAnnibaleDevice:self descriptor:use];
}

/* Compute pipelines compile the function's AIR with libannibaleair. Requested
 * reflection uses the same entry point's argument declarations and byte layouts.
 */
- (id)newComputePipelineStateWithDescriptor:(id)descriptor
				    options:(NSUInteger)options
				 reflection:(id *)reflection
				      error:(NSError **)error
{
	if (reflection != NULL)
		*reflection = nil;
	id state = [self pipelineForFunction:[descriptor computeFunction] descriptor:descriptor
				      error:error];
	if (state == nil || reflection == NULL ||
	    !(options & (MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo)))
		return state;
	if (mesa.compute_get_reflection == NULL) {
		[state release];
		return [self fail:error with:@"the compiler library has no compute reflection query"];
	}
	const struct rwa_compute_reflection *metadata = NULL;
	char why[512] = { 0 };
	if (mesa.compute_get_reflection([state compute], &metadata, why, sizeof(why)) != 0) {
		[state release];
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	NSString *reason = nil;
	id result = rw_compute_reflection(metadata, &reason);
	if (result == nil) {
		[state release];
		return [self fail:error with:reason ?: @"Metal could not construct compute reflection"];
	}
	*reflection = [result autorelease];
	return state;
}

- (id)newComputePipelineStateWithDescriptor:(id)descriptor error:(NSError **)error
{
	return [self pipelineForFunction:[descriptor computeFunction] descriptor:descriptor
				  error:error];
}

/* _MTLDevice's own version of this builds an MTLComputePipelineDescriptorInternal and
 * re-sends to self, so overriding it is not strictly needed -- but a descriptor is what the
 * pipeline state's base class demands, and making one here keeps the three entry points
 * reaching the same code with the same arguments. */
/* The same hole on the compute side, and the same reason. */
- (void)newComputePipelineStateWithFunction:(id)function
			  completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id state = [self newComputePipelineStateWithFunction:function error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newComputePipelineStateWithDescriptor:(id)descriptor
				      options:(NSUInteger)options
			    completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;
	id reflection = nil;
	id state = [self newComputePipelineStateWithDescriptor:descriptor
						      options:options
						   reflection:&reflection
							error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

- (id)newComputePipelineStateWithFunction:(id)function error:(NSError **)error
{
	Class internal = objc_getClass("MTLComputePipelineDescriptorInternal");
	id descriptor = internal != Nil ? [[internal alloc] init] :
	    (id)[[MTLComputePipelineDescriptor alloc] init];

	if (descriptor == nil) {
		return [self fail:error with:@"no pipeline descriptor class to build one from"];
	}
	[descriptor setComputeFunction:function];
	id state = [self pipelineForFunction:function descriptor:descriptor error:error];

	[descriptor release];
	return state;
}

/* One place where a function becomes a pipeline, so that the three entry points above
 * cannot drift apart. */
- (id)pipelineForFunction:(id)function descriptor:(id)descriptor error:(NSError **)error
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (function == nil) {
		return [self fail:error with:@"the descriptor has no compute function"];
	}
	if (_mesa == NULL) {
		return [self fail:error with:@"there is no GPU compiler here: either the Adreno "
					     @"was not started (gpu=1) or libannibaleair.dylib "
					     @"is missing"];
	}
	char why[512] = { 0 };
	struct rwa_compute_info info = { 0 };
	struct rwa_compute *compute = NULL;
	if ([function respondsToSelector:@selector(annibaleGraph)]) {
		struct rwa_stitched_kernel kernel = { 0 };
		struct rwa_linked_function graph = { 0 };
		NSMutableData *scratch = [NSMutableData data];
		BOOL valid = rw_stitched_nodes(function, &graph, scratch, &kernel);
		if (valid && mesa.compute_create_stitched != NULL)
			compute = mesa.compute_create_stitched(_mesa, &kernel, &info, why, sizeof(why));
		else
			snprintf(why, sizeof(why), "%s", valid ?
				 "the AIR library has no stitched compute entry point" :
				 "invalid stitched compute graph, input type, or dependency order");
		rw_linked_free(scratch);
	} else {
		if (![function respondsToSelector:bitcode])
			return [self fail:error with:@"the compute function has no bitcodeData"];
		NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);
		if ([air length] == 0)
			return [self fail:error with:@"the compute function has no bitcode"];
		compute = mesa.compute_create(_mesa, [air bytes], [air length],
			[[function name] UTF8String], &info, why, sizeof(why));
	}
	if (compute == NULL) {
		/* The front end's own sentence, passed through rather than replaced: it says
		 * which construct it refused, and an application can act on that. */
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	rw_print("compute pipeline %p is %s\n", (void *)compute, [[function name] UTF8String]);
	return [[AnnibaleMetalComputePipeline alloc] initWithAnnibaleDevice:self
								 compute:compute
								    info:&info
							      descriptor:descriptor];
}

- (id)fail:(NSError **)error with:(NSString *)reason
{
	if (error != NULL) {
		*error = [NSError errorWithDomain:@"AnnibaleMetal" code:1
					 userInfo:@{ NSLocalizedDescriptionKey: reason }];
	}
	return nil;
}

/*
 * The rest of what MTLDevice and MTLDeviceSPI require.
 *
 * Measured on the phone: 44 of the methods those two protocols require are answered by
 * nothing in the _MTLDevice chain (docs/metal.md 10.8). Until they are answered, any
 * application that asks one gets "unrecognized selector" and dies -- and that matters more
 * than it looks, because the gates inside Metal are assertions, so a process that asks a
 * question this device cannot answer does not degrade, it aborts, and launchd restarts it.
 *
 * So each of these answers, and each answers HONESTLY: a capability we have not got is
 * declined rather than claimed, a quantity we do not track says so in its comment rather
 * than inventing a number that something would then rely on. Where a number is a placeholder
 * it is marked as one; where the right authority is the compiler library rather than this
 * file, that is written down, because the next person to touch it will want to know.
 */

- (uint64_t)registryID
{
	/* Not invented: the registry's own identifier for the service Metal handed us. */
	uint64_t id = 0;

	if (_acceleratorService != IO_OBJECT_NULL &&
	    IORegistryEntryGetRegistryEntryID(_acceleratorService, &id) == KERN_SUCCESS) {
		return id;
	}
	return 0;
}

/* The a635 runs up to 1024 threads in a threadgroup, which is also what every pipeline the
 * compiler library has compiled reports back (10.15). The per-dimension limits are Metal's
 * usual shape for that; the authority should become the library itself once the contract has
 * a call for it. */
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 64); }

/* The number _MTLDevice's own limits table was filled with for this feature profile. Also a
 * placeholder in the same sense: the hardware's figure belongs to Mesa, not here. */
- (NSUInteger)maxThreadgroupMemoryLength { return 16384; }

- (BOOL)hasUnifiedMemory { return YES; }   /* one memory on this SoC, shared with the CPU */

/* Nothing here tracks allocation yet, and two of these are read by applications deciding how
 * much to allocate, so they answer a figure that is plausible for this phone rather than 0,
 * which would read as "allocate nothing". That they are not measured is the point of this
 * comment. */
- (uint64_t)recommendedMaxWorkingSetSize { return 512ULL << 20; }
- (uint64_t)currentAllocatedSize         { return 0; }
- (uint64_t)sharedMemorySize             { return 512ULL << 20; }
- (uint64_t)dedicatedMemorySize          { return 0; }   /* none: the memory is shared */
- (uint64_t)maxBufferLengthInternal      { return 256ULL << 20; }

- (BOOL)isLowPower      { return NO; }
- (BOOL)isHeadless      { return NO; }
- (BOOL)isRemovable     { return NO; }
- (BOOL)supportPriorityBand { return NO; }

/* Declined rather than claimed: no sparse textures, and a yes here would be acted on.
 *
 * Depth24Unorm_Stencil8 stays NO now that there IS a depth path, and it is not a leftover:
 * Metal's own header marks that format API_UNAVAILABLE(ios), the compiler library refuses it
 * by name for the same reason, and iOS depth is Depth16Unorm, Depth32Float or
 * Depth32Float_Stencil8 -- which this device does answer for. A yes here would offer a
 * format no texture could be made of. */
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }

/*
 * One sample by default, and four when ANNIBALE_MSAA asks for it -- the switch exists
 * because the answer changes what every application that asks it does.
 *
 * The compiler library makes multisample textures since docs/metal.md 10.34, and resolves
 * one into the attachment's resolve texture with a blit at the end of every draw that names
 * one (struct rwa_draw.resolve). Four and not two: four is what CoreAnimation and the GL
 * path ask for. Eight is declined by the back end (a6xx/fd6_screen.cc, valid_sample_count).
 * The variable is read on every call: applications call this a handful of times at startup.
 *
 * -supportsTextureSampleCount: is not a second answer to keep in step. It is one
 * instruction in _MTLDevice (0x181a17890) that tail-calls -supportsSampleCount:, which
 * _MTLDevice itself does not implement at all -- so this method is the whole of both, and
 * without it that forward would die on an unrecognized selector naming OUR class
 * (docs/metal.md 10.1's trap).
 */
- (BOOL)supportsSampleCount:(NSUInteger)count
{
	if (count == 1) {
		return YES;
	}
	return count == 4 && getenv("ANNIBALE_MSAA") != NULL;
}
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)type
			     pixelFormat:(NSUInteger)format
			     sampleCount:(NSUInteger)samples
{
	(void)type; (void)format; (void)samples;
	return MTLSizeMake(0, 0, 0);
}
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)type
			     pixelFormat:(NSUInteger)format
			     sampleCount:(NSUInteger)samples
			  sparsePageSize:(NSInteger)page
{
	(void)page;
	return [self sparseTileSizeWithTextureType:type pixelFormat:format sampleCount:samples];
}

/* These are bitfields of floating-point behaviours a device claims. Claiming none is the
 * honest answer while nothing has verified any of them on this hardware -- and docs/air.md
 * is precisely the document that would have to grow the evidence first. */
- (uint64_t)singleFPConfig { return 0; }
- (uint64_t)halfFPConfig   { return 0; }
- (uint64_t)doubleFPConfig { return 0; }

/* Everything else the two protocols require and this driver has not built. Each returns the
 * API's own failure value, so a caller finds out by the rule Metal already has, rather than
 * by a selector that does not exist. */
- (id)newFence                                      { return nil; }
/*
 * The one method -[CAMetalLayer nextDrawable] asks of a device (docs/metal.md 10.46):
 * allocate_drawable_texture builds an ordinary MTLTextureDescriptor, sets its private
 * rotation/isDrawable/framebufferOnly/usage and sends this. The surface, its geometry and
 * every refusal live in src/annibaledrawable.m, because they answer CoreAnimation rather than
 * Metal; what is left here is the object, which has to be of this class.
 *
 * The refusal is printed rather than returned silently, and that is not a style choice:
 * CoreAnimation does not test what it gets back, so a bare nil becomes a CAMetalDrawable
 * whose -texture is nil, and the application finds out later and somewhere else.
 */
- (id)newBufferWithIOSurface:(void *)surface        { (void)surface; return nil; }
- (id)newBufferWithBytesNoCopy:(void *)bytes
			length:(NSUInteger)length
		       options:(NSUInteger)options
		   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	/* 16 MiB covers what the window server composites: a full-screen BGRA image is 11 MB, about a
	 * millisecond to copy when the kernel does not take the pages. The first limit, 64 KiB, left
	 * backboardd's 114688-byte buffer over a 640x155 image undrawn (lock3cpu4, 2026-10-30). */
	enum { NOCOPY_LIMIT = 16 << 20 };

	(void)options;
	if (bytes == NULL || length == 0 || length > NOCOPY_LIMIT) {
		if (length > NOCOPY_LIMIT) {
			rw_print("rwa buffer: %lu bytes left nil, over the %u this path copies\n",
				 (unsigned long)length, (unsigned)NOCOPY_LIMIT);
		}
		return nil;
	}
	return [[AnnibaleMetalBuffer alloc] initWithDevice:self
						    gpu:_gpu
						   bytes:bytes
						  length:length
					     deallocator:deallocator];
}
- (id)newIndirectCommandBufferWithDescriptor:(id)d
			      maxCommandCount:(NSUInteger)count
				      options:(NSUInteger)options
{
	(void)d; (void)count; (void)options;
	return nil;
}
- (id)newIndirectCommandBufferWithDescriptor:(id)d
				    maxCount:(NSUInteger)count
				     options:(NSUInteger)options
{
	(void)d; (void)count; (void)options;
	return nil;
}
- (id)newIndirectComputeCommandEncoderWithBuffer:(id)b { (void)b; return nil; }
- (id)newIndirectRenderCommandEncoderWithBuffer:(id)b  { (void)b; return nil; }
- (id)newIndirectArgumentBufferLayoutWithStructType:(id)t { (void)t; return nil; }
- (id)newVisibleFunctionTableWithDescriptor:(id)d      { (void)d; return nil; }
- (id)newIntersectionFunctionTableWithDescriptor:(id)d { (void)d; return nil; }

/*
 * The last paths in the chain that would still abort.
 *
 * Asking the device whether it responds to these says yes -- they are real entries in
 * _MTLDevice's method list -- and calling one aborts, because each body is
 * [self doesNotRecognizeSelector:_cmd]. Seven methods of _MTLDevice are abstract in that
 * way (tools/metal/absmeth.py), one of them is -initWithAcceleratorPort: which this driver
 * implements, and the other six are these: ray tracing. Overriding them to decline turns the
 * last aborting paths into ordinary nil, which is what the "0 unanswered" measurement needs
 * in order to mean anything.
 */
- (id)newAccelerationStructureWithSize:(NSUInteger)size { (void)size; return nil; }
- (id)newAccelerationStructureWithSize:(NSUInteger)size resourceIndex:(NSUInteger)index
{
	(void)size; (void)index;
	return nil;
}
- (id)newAccelerationStructureWithSize:(NSUInteger)size withDescriptor:(id)descriptor
{
	(void)size; (void)descriptor;
	return nil;
}
- (id)newAccelerationStructureWithBuffer:(id)buffer offset:(NSUInteger)offset
{
	(void)buffer; (void)offset;
	return nil;
}
- (id)newAccelerationStructureWithBuffer:(id)buffer
				  offset:(NSUInteger)offset
			   resourceIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	return nil;
}
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithSize:(NSUInteger)size
{
	(void)size;
	return (MTLSizeAndAlign){ 0, 0 };
}

- (BOOL)metalAssertionsEnabled { return _assertions; }
- (void)setMetalAssertionsEnabled:(BOOL)enabled { _assertions = enabled; }

/* Without this the device inherits _MTLDevice's own answer, which is "Apple GPU" -- true of
 * the code and false of the hardware. The name is Mesa's renderer string for this chip
 * (docs/air.md: `Adreno 7c+ Gen 3, chip 0x06030500`), and the identifier beside it is asked
 * of the kernel rather than written down, so a different phone says something different.
 * Nothing in Metal's registration reads this; a person reading a log does. */
- (NSString *)name
{
	uint64_t chip = 0;

	if (gpu_param(_gpu, 0x03, &chip)) {
		return [NSString stringWithFormat:@"Adreno 7c+ Gen 3 (chip %#x, Annibale)",
				 (unsigned)(chip & 0xffffffffu)];
	}
	return @"Adreno (Annibale; the GPU is not started)";
}

#include "annibalemetal_archive.inc"

/* The two buffer entry points of MTLDevice that nothing in the chain answers. Anything
 * Metal would ask about storage modes is answered by the buffer itself. */
- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options
{
	(void)options;   /* every allocation here is CPU-visible; the buffer says so honestly */
	return [[AnnibaleMetalBuffer alloc] initWithDevice:self gpu:_gpu length:length];
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(NSUInteger)options
{
	AnnibaleMetalBuffer *buffer = [self newBufferWithLength:length options:options];

	if (buffer != nil && bytes != NULL && [buffer contents] != NULL) {
		memcpy([buffer contents], bytes, length);
	}
	return buffer;
}

/* Profile 0's own table says maxVertexAmplificationCount is 1, so anything above one is
 * not supported, and saying otherwise would be a lie Metal would act on. */
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count
{
	return count <= 1;
}

@end

@implementation AnnibaleMetalBuffer

+ (void)load
{
	Protocol *p = objc_getProtocol("MTLBuffer");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLBuffer) refused\n");
	}
}

- (instancetype)initWithDevice:(AnnibaleMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length
{
	struct gem_new request = { .size = length, .flags = 0, .handle = 0 };
	size_t size = sizeof(request);

	self = [super init];
	if (self == nil) {
		return nil;
	}

	/* Two ways to the same kernel. When the compiler library is loaded, the memory comes
	 * from Mesa, because only such a buffer can be handed to a dispatch; without it the
	 * kernel's GEM interface is used directly, so that a device with no GPU library still
	 * gives memory a program can write. Which one happened is visible in -mesaBuffer. */
	if (mesa_available() && [device mesaDevice] != NULL) {
		char why[256] = { 0 };

		_mesa = mesa.buffer_create([device mesaDevice], length, why, sizeof(why));
		if (_mesa == NULL) {
			rw_print("Mesa would not allocate %lu bytes: %s\n",
			         (unsigned long)length, why);
			[self release];
			return nil;
		}
		_owner = device;
		_length = length;
		_contents = mesa.buffer_map(_mesa);
		return self;
	}
	if (gpu == IO_OBJECT_NULL || length == 0) {
		rw_print("a buffer of %lu bytes needs a gpu client\n",
		         (unsigned long)length);
		[self release];
		return nil;
	}
	kern_return_t made = IOConnectCallStructMethod(gpu, kMsmGemNew, &request, sizeof(request),
						       &request, &size);
	if (made != KERN_SUCCESS || request.handle == 0) {
		/* the IOReturn says which shortage it was (AnnibaleGPUMemory.inc): handles all in use,
		 * no span of GPU addresses, no page table, wired memory, or the GPU going or gone */
		rw_print("the kernel would not allocate %lu bytes: IOReturn %#x\n",
		         (unsigned long)length, made);
		[self release];
		return nil;
	}
	_owner = device;
	_gpu = gpu;
	_handle = request.handle;
	_length = length;

	/* the address the GPU will see, which is what a command stream carries */
	struct gem_info info = { .handle = _handle, .info = MSM_INFO_GET_IOVA };

	size = sizeof(info);
	if (IOConnectCallStructMethod(gpu, kMsmGemInfo, &info, sizeof(info), &info,
				      &size) == KERN_SUCCESS) {
		_iova = info.value;
	}

	/* and the address this processor will see: the mapping is as the memory is made,
	 * write-combined, exactly as Mesa takes it */
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	if (IOConnectMapMemory64(gpu, _handle, mach_task_self(), &address, &mapped,
				 kIOMapAnywhere) == KERN_SUCCESS && address != 0) {
		_contents = (void *)(uintptr_t)address;
	} else {
		rw_print("buffer %u is allocated but could not be mapped\n", _handle);
		[self release];
		return nil;
	}
	return self;
}

- (struct rwa_buffer *)mesaBuffer
{
	return _mesa;
}

/* Memory the caller owns: the GPU's own if the kernel takes the pages (rwa_buffer_from_memory), else a
 * copy, whose textures read and write back the caller's memory (rwa_buffer_set_mirror) and whose -contents does not. */
- (instancetype)initWithDevice:(AnnibaleMetalDevice *)device
			   gpu:(io_connect_t)gpu
			 bytes:(const void *)bytes
			length:(NSUInteger)length
		   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	char why[256] = { 0 };
	struct rwa_buffer *own = mesa_available() && [device mesaDevice] != NULL && mesa.buffer_from_memory != NULL ?
	    mesa.buffer_from_memory([device mesaDevice], (void *)bytes, (length + 4095) & ~(NSUInteger)4095, why, sizeof(why)) : NULL;

	self = own != NULL ? [super init] : [self initWithDevice:device gpu:gpu length:length];
	if (self == nil) {
		if (own != NULL) mesa.buffer_destroy(own);
		return nil;
	}
	if (own != NULL)
		_mesa = own, _owner = device, _length = length, _contents = (void *)bytes;
	else if (bytes != NULL && _contents != NULL)
		memcpy(_contents, bytes, length);
	if (own == NULL && _mesa != NULL && mesa.buffer_set_mirror != NULL) mesa.buffer_set_mirror(_mesa, (void *)bytes);
	_callerBytes = (void *)bytes;
	if (deallocator != NULL) _callerDeallocator = [deallocator copy];
	static unsigned long made;
	unsigned long made_n = __sync_add_and_fetch(&made, 1);
	if (made_n <= 32 || made_n % 256 == 0)
		rw_print("rwa buffer: %s %p of %lu bytes, buffer at %p (%lu)%s%s\n", own ? "took" : "copied", bytes,
			 (unsigned long)length, _contents, made_n, own ? "" : ": ", why);
	return self;
}

- (instancetype)initWithDevice:(AnnibaleMetalDevice *)device
		     iosurface:(void *)surface
{
	static size_t (*get_alloc_size)(void *);
	static void *(*get_base_address)(void *);
	static dispatch_once_t once;
	char why[256] = { 0 };
	struct rwa_buffer *own = NULL;
	size_t alloc_size;
	void *base;
	uint32_t surface_id;

	self = [super init];
	if (self == nil) {
		return nil;
	}
	dispatch_once(&once, ^{
		*(void **)&get_alloc_size = dlsym(RTLD_DEFAULT, "IOSurfaceGetAllocSize");
		*(void **)&get_base_address = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
	});
	if (surface == NULL || get_alloc_size == NULL || !mesa_available() ||
	    [device mesaDevice] == NULL) {
		[self release];
		return nil;
	}
	alloc_size = get_alloc_size(surface);
	if (alloc_size == 0 || alloc_size > UINT32_MAX) {
		[self release];
		return nil;
	}
	base = get_base_address != NULL ? get_base_address(surface) : NULL;
	surface_id = ann_surface_id(surface);
	if (surface_id != 0 && mesa.buffer_from_surface != NULL) {
		own = mesa.buffer_from_surface([device mesaDevice], (int)surface_id, alloc_size,
					       why, sizeof(why));
	}
	if (own == NULL && base != NULL && mesa.buffer_from_memory != NULL) {
		NSUInteger page_size = (alloc_size + 4095) & ~(NSUInteger)4095;
		own = mesa.buffer_from_memory([device mesaDevice], base, page_size,
					      why, sizeof(why));
	}
	if (own == NULL) {
		rw_print("newBufferWithIOSurface %p (%zu bytes, id %u): %s\n", surface, alloc_size,
			 surface_id, why[0] ? why : "surface import unavailable");
		[self release];
		return nil;
	}
	_mesa = own;
	_owner = device;
	_length = alloc_size;
	_contents = base != NULL ? base : mesa.buffer_map(own);
	_iosurface = CFRetain(surface);
	return self;
}

- (void)dealloc
{
	/* The caller handed us its own memory and an expectation to be told when we are done with it:
	 * the deallocator runs once, here, with _callerBytes, after the library stopped mirroring it. */
	uint64_t dealloc_n = rw_dealloc_count + 1;
	bool dealloc_print = rw_dealloc_print();
	if (dealloc_print) {
		rw_print("rwa dealloc: begin %llu pid %d self %p mesa %p contents %p bytes %p len %lu\n",
			 (unsigned long long)dealloc_n, getpid(), self, _mesa, (void *)self->_contents,
			 _callerBytes, (unsigned long)self->_length);
	}
	if (_mesa != NULL && mesa.buffer_set_mirror != NULL) mesa.buffer_set_mirror(_mesa, NULL);
	if (_callerDeallocator != NULL) {
		void (^block)(void *, NSUInteger) = _callerDeallocator;

		_callerDeallocator = NULL;
		block(_callerBytes, _length);
		[block release];
	}
	if (dealloc_print) {
		rw_print("rwa dealloc: freed %llu bytes %p\n",
			 (unsigned long long)dealloc_n, _callerBytes);
	}
	if (_mesa != NULL) {
		mesa.buffer_destroy(_mesa);
		if (dealloc_print) {
			rw_print("rwa dealloc: destroyed %llu mesa %p\n",
				 (unsigned long long)dealloc_n, _mesa);
		}
		if (_iosurface != NULL) {
			CFRelease(_iosurface);
			_iosurface = NULL;
		}
		[super dealloc];
		return;
	}
	if (_contents != NULL) {
		IOConnectUnmapMemory64(_gpu, _handle, mach_task_self(),
				       (mach_vm_address_t)(uintptr_t)_contents);
	}
	if (_handle != 0) {
		struct gem_close request = { .handle = _handle, .pad = 0 };
		size_t size = sizeof(request);

		IOConnectCallStructMethod(_gpu, kDrmGemClose, &request, sizeof(request), &request,
					  &size);
	}
	if (_iosurface != NULL) {
		CFRelease(_iosurface);
		_iosurface = NULL;
	}
	[super dealloc];
}

- (void *)contents          { return _contents; }
- (NSUInteger)length        { return _length; }
- (id)device                { return _owner; }
- (uint64_t)gpuAddress      { return _iova; }
- (NSUInteger)allocatedSize { return _length; }
- (IOSurfaceRef)iosurface   { return (IOSurfaceRef)_iosurface; }

/* The memory is write-combined and the GPU reads it through no cache of ours, so there is
 * nothing to push out; saying so is not the same as doing nothing by accident. */
- (void)didModifyRange:(NSRange)range    { (void)range; }

/* What was asked for does not change what was given: every allocation here is one kind of
 * memory, and the honest answer is the kind it is. */
- (NSUInteger)storageMode        { return 0; }   /* MTLStorageModeShared */
- (NSUInteger)cpuCacheMode       { return 1; }   /* MTLCPUCacheModeWriteCombined */
- (NSUInteger)resourceOptions    { return 0; }
- (NSUInteger)hazardTrackingMode { return 0; }   /* MTLHazardTrackingModeDefault */

/* MTLPurgeableStateNonVolatile (2), whatever was asked for, and that is the answer and not
 * a refusal: this memory is never volatile -- the kernel holds it from kMsmGemNew to
 * kDrmGemClose -- so there is no state for a caller to change and none to report but this
 * one. (Zero, which this used to answer, is not an MTLPurgeableState at all, and Metal has
 * a path that then believes the resource was purged: see -addPurgedResource: on the command
 * buffer.) */
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }
- (id)heap                      { return rw_resource_heap(self); }
- (NSUInteger)heapOffset        { return rw_resource_heap_offset(self); }
- (BOOL)isAliasable             { return NO; }
- (void)makeAliasable           { }

/*
 * What <MTLResourceSPI> and <MTLBuffer> require and nothing in this chain answered: ten
 * methods, every one of them an abort in whatever daemon asked first. They are answers, and
 * most of them are true rather than evasive -- which is the point of counting what a
 * protocol demands instead of what our own tests happen to send.
 *
 * Aliasing: nothing here aliases anything. Every buffer is its own GEM allocation (or its
 * own Mesa allocation), a heap's too, and -makeAliasable does nothing, so "does this
 * overlap that" is NO for every pair, for certain.
 *
 * Completion: this driver's -commit runs the recorded work and -waitUntilCompleted fences
 * it, and nothing hands a resource back to a caller before the submission it was part of
 * has been made. There is no per-resource completion to wait for, so YES and a return are
 * the honest pair -- and if a second ring ever appears, these two are where it has to be
 * answered again.
 */
- (BOOL)doesAliasResource:(id)resource { (void)resource; return NO; }
- (BOOL)doesAliasAnyResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)doesAliasAllResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)isComplete        { return YES; }
- (void)waitUntilComplete { }
- (BOOL)isPurgeable       { return NO; }
/* Protected (DRM) memory is a kernel capability this port does not have; zero is "none of
 * the protection options", which is what this memory has. */
- (NSUInteger)protectionOptions { return 0; }
/* The options before the device filtered them: the device filters nothing, so this is the
 * same answer -resourceOptions gives. */
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
/* Whose allocation this is for the kernel's accounting. We do not tell the kernel, so the
 * only true answer is this process, and a setter that remembers what it was told. */
- (int)responsibleProcess
{
	return _responsibleProcess != 0 ? _responsibleProcess : getpid();
}
- (void)setResponsibleProcess:(int)pid { _responsibleProcess = pid; }
- (void)addDebugMarker:(NSString *)marker range:(NSRange)range { (void)marker; (void)range; }
- (void)removeAllDebugMarkers { }

@end

#include "annibalemetal_texture.inc"

#include "annibalemetal_state.inc"

@implementation AnnibaleMetalComputePipeline

- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device
			     compute:(struct rwa_compute *)compute
				info:(const struct rwa_compute_info *)info
			  descriptor:(id)descriptor
{
	/* _MTLComputePipelineState's designated initialiser is the one AGX sends to super
	 * (0x2092477e0 -> 0x181a2e068). The descriptor must not be nil: the base asserts on
	 * that at its own line 1504, and the first version of this passed nil and was killed
	 * by it. */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		mesa.compute_destroy(compute);
		return nil;
	}
	_owner = [device retain];
	_compute = compute;
	_info = *info;
	_imageblockDataSize = mesa.compute_imageblock_data_size != NULL
	    ? mesa.compute_imageblock_data_size(compute) : 0;
	if ([descriptor respondsToSelector:sel_registerName("computeFunction")]) {
		id fn = ((id (*)(id, SEL))objc_msgSend)(descriptor,
						  sel_registerName("computeFunction"));
		if (fn != nil && [fn respondsToSelector:@selector(name)])
			_kernelName = [[fn name] retain];
	}
	/* and the same for a kernel with a set, for the same reason */
	if (_info.argbuf_mask != 0) {
		RW_ONCE("a compute pipeline declares argument buffers at Metal buffer indices "
			"%#x; their members are bound from what an MTLArgumentEncoder "
			"recorded\n", _info.argbuf_mask);
	}
	return self;
}

- (void)dealloc
{
	if (_compute != NULL) {
		rw_compute_pipe_release(_compute);
		_compute = NULL;
	}
	[_kernelName release];
	[_owner release];
	[super dealloc];
}

- (struct rwa_compute *)compute { return _compute; }
- (NSString *)annibaleKernelName { return _kernelName; }
- (BOOL)groupFree { return _info.group_free != 0; }

- (void)ensureLimitsMaterialized
{
	if (__atomic_load_n(&_limitsMaterialized, __ATOMIC_ACQUIRE) != 0)
		return;
	if (_compute == NULL || mesa.compute_materialize == NULL)
		return;
	char why[512] = { 0 };
	struct rwa_compute_info mat = { 0 };
	if (mesa.compute_materialize(_compute, &mat, why, sizeof(why)) != 0) {
		static unsigned materialize_logged;
		if (__atomic_fetch_add(&materialize_logged, 1, __ATOMIC_RELAXED) < 16)
			rw_print("compute materialize refused: %s\n", why);
		return;
	}
	__atomic_store_n(&_info.max_threads_per_group, mat.max_threads_per_group, __ATOMIC_RELAXED);
	__atomic_store_n(&_info.thread_execution_width, mat.thread_execution_width, __ATOMIC_RELAXED);
	__atomic_store_n(&_limitsMaterialized, 1u, __ATOMIC_RELEASE);
}

/* The three the base class answers with nothing at all (docs/metal.md 10.8), so they can
 * only come from whoever compiled the shader -- which is why the contract carries them. */
- (NSUInteger)maxTotalThreadsPerThreadgroup
{
	[self ensureLimitsMaterialized];
	return __atomic_load_n(&_info.max_threads_per_group, __ATOMIC_ACQUIRE);
}
- (NSUInteger)threadExecutionWidth
{
	[self ensureLimitsMaterialized];
	return __atomic_load_n(&_info.thread_execution_width, __ATOMIC_ACQUIRE);
}
- (NSUInteger)staticThreadgroupMemoryLength { return _info.static_shared_bytes; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions
{
	if (_imageblockDataSize == 0)
		return 0;
	NSUInteger w = MAX(dimensions.width, (NSUInteger)1);
	NSUInteger h = MAX(dimensions.height, (NSUInteger)1);
	return (w * h * (NSUInteger)_imageblockDataSize + 15) & ~(NSUInteger)15;
}

- (id)device { return _owner; }

@end

@implementation AnnibaleMetalRenderPipeline

- (instancetype)initWithAnnibaleDevice:(AnnibaleMetalDevice *)device
			       render:(struct rwa_render *)render
				 info:(const struct rwa_render_info *)info
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilyRenderPipeline sends to super (0x181a294dc), and a real
	 * descriptor, for the reason the compute one learned at its own line 1504 */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		rw_pipe_release(render);
		return nil;
	}
	_owner = [device retain];
	_render = render;
	_info = *info;
	id vert = descriptor != nil ? [descriptor vertexFunction] : nil;
	_vertexName = vert != nil ? [[vert name] copy] : nil;
	id frag = descriptor != nil ? [descriptor fragmentFunction] : nil;
	_fragmentName = frag != nil ? [[frag name] copy] : nil;
	id vdesc = descriptor != nil ? [descriptor vertexDescriptor] : nil;
	if (vdesc != nil) {
		id attrs = [vdesc attributes];
		id layouts = [vdesc layouts];
		for (unsigned i = 0; i < 4; i++) {
			id a = attrs != nil ? [attrs objectAtIndexedSubscript:i] : nil;
			if (a == nil) {
				continue;
			}
			unsigned fmt = (unsigned)[a format];
			unsigned off = (unsigned)[a offset];
			unsigned bi = (unsigned)[a bufferIndex];
			unsigned st = 0;
			if (fmt != 0 && layouts != nil && bi < RW_MAX_BUFFERS) {
				id l = [layouts objectAtIndexedSubscript:bi];
				if (l != nil) {
					st = (unsigned)[l stride];
				}
			}
			_vattrMeta[i][0] = fmt;
			_vattrMeta[i][1] = off;
			_vattrMeta[i][2] = bi;
			_vattrMeta[i][3] = st;
		}
	}
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *att = descriptor != nil ?
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i] : nil;
		if (att == nil) {
			_colorBlend[i].write_mask = 0xf;
			continue;
		}
		_colorBlend[i].blending_enabled = [att isBlendingEnabled] ? 1u : 0u;
		_colorBlend[i].rgb_blend_operation = (unsigned)[att rgbBlendOperation];
		_colorBlend[i].alpha_blend_operation = (unsigned)[att alphaBlendOperation];
		_colorBlend[i].source_rgb_blend_factor = (unsigned)[att sourceRGBBlendFactor];
		_colorBlend[i].destination_rgb_blend_factor = (unsigned)[att destinationRGBBlendFactor];
		_colorBlend[i].source_alpha_blend_factor = (unsigned)[att sourceAlphaBlendFactor];
		_colorBlend[i].destination_alpha_blend_factor = (unsigned)[att destinationAlphaBlendFactor];
		_colorBlend[i].write_mask = (unsigned)[att writeMask];
	}
	if (info->generated_vertex_stage) {
		rw_print("this pipeline's vertex stage was generated, so what it "
		         "draws is the fragment shader over the whole attachment\n");
	}
	/* The first pipeline of this process whose shaders declare an ARGUMENT BUFFER, named
	 * once: every measurement this port has of argument buffers is on the host, and this
	 * line is how a boot says a pipeline with a set was built at all. The indices matter
	 * because the draw binds nothing of that set out of the buffer table -- it binds what
	 * the encoder recorded for whatever buffer is bound there. */
	if (info->vertex_argbuf_mask != 0 || info->fragment_argbuf_mask != 0) {
		RW_ONCE("a render pipeline declares argument buffers: the vertex stage at "
			"Metal buffer indices %#x and the fragment stage at %#x. Their members "
			"are bound from what an MTLArgumentEncoder recorded, and a buffer "
			"bound there that nobody encoded is refused by name\n",
			info->vertex_argbuf_mask, info->fragment_argbuf_mask);
	}
	return self;
}

- (void)dealloc
{
	if (_render != NULL) {
		rw_pipe_release(_render);
	}
	[_vertexName release];
	[_fragmentName release];
	[_owner release];
	[super dealloc];
}

- (struct rwa_render *)render { return _render; }
- (const struct rwa_color_blend *)colorBlendAtIndex:(unsigned)index
{
	return index < 8 ? &_colorBlend[index] : NULL;
}
- (NSString *)vertexFunctionName { return _vertexName; }
- (NSString *)fragmentFunctionName { return _fragmentName; }
- (const struct rw_g32_vattr_meta *)vertexAttrMeta:(unsigned)index
{
	return index < 4 ? (const struct rw_g32_vattr_meta *)_vattrMeta[index] : NULL;
}
- (BOOL)writesDepth           { return _info.fragment_writes_depth != 0; }
- (unsigned)vertexStreamMask  { return _info.vertex_stream_mask; }
- (unsigned)vertexStreamCount { return _info.vertex_streams; }
- (unsigned)vertexElementCount { return _info.vertex_attribute_count; }
- (unsigned)vertexElementAttribute:(unsigned)index
{
	unsigned n = sizeof(_info.vertex_attribute_index) /
		     sizeof(_info.vertex_attribute_index[0]);

	/* ~0u and not 0 for an index past the end: attribute 0 is a real answer, and a
	 * caller that asked for element 5 of a pipeline with two would otherwise read the
	 * mapping as if it had five. */
	return index < _info.vertex_attribute_count && index < n
	    ? _info.vertex_attribute_index[index] : ~0u;
}
- (id)device                  { return _owner; }

/* The base class answers this one with `mov x0, #0; ret` (0x181b0eee0), and a zero would read
 * as "no threads"; the number is the one the compiler library reports for a compiled stage. */
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (BOOL)threadgroupSizeMatchesTileSize      { return NO; }
- (NSUInteger)imageblockSampleLength        { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions
{
	(void)dimensions;
	return 0;
}

/* The last two of <MTLRenderPipelineStateSPI> that nothing in this chain answered. Shader
 * debug information is what Metal's own debugger reads out of a pipeline built with it;
 * there is none here, and nil is the answer Apple's driver gives for a pipeline that was
 * built without it, so the caller already has this path. */
- (id)newVertexShaderDebugInfo   { return nil; }
- (id)newFragmentShaderDebugInfo { return nil; }

@end

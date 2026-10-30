/* Texture ownership, IOSurface properties and Metal texture operations, and the textures a buffer
 * makes over its own memory. */
/* `-replaceRegion:` receives `MTLRegion` by value and reads six NSUInteger fields out of it. The
 * region is printed as six machine words before anything reads a field, because whether those six
 * words hold `origin.{x,y,z}` then `size.{width,height,depth}` is the question: a caller compiled
 * against a different width or packing still lands exactly 48 bytes here, and the only sign would be
 * zeros where a size should be. Eight lines per process, so a run that calls this often still
 * leaves room for everything else in the log ring.
 */
_Static_assert(sizeof(MTLRegion) == 48, "MTLRegion is six 64-bit fields: three origin, three size");

static uint64_t
region_words_now(void)
{
	struct mach_timebase_info tb = { 0, 0 };

	mach_timebase_info(&tb);
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

static void
region_words(const void *texture, const uint64_t *word, const void *bytes)
{
	char line[192];
	int n = snprintf(line, sizeof(line),
			 "tl: q %p %llx %llx %llx %llx %llx %llx %llu %p\n", texture,
			 (unsigned long long)word[0], (unsigned long long)word[1],
			 (unsigned long long)word[2], (unsigned long long)word[3],
			 (unsigned long long)word[4], (unsigned long long)word[5],
			 (unsigned long long)region_words_now(), bytes);

	if (n > 0) {
		(void)write(STDOUT_FILENO, line,
			    (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	}
}

/* The same six words as `tl: q`, printed only when the caller's byte pointer cannot be a normal
 * address: heap and stack addresses on this target leave the top 16 bits clear, and the value that
 * reaches `rwa_texture_write` as `bytes` in the faulting round has them set. A null pointer is
 * printed too, since that is the other way this call is wrong. `stride` rides along because the
 * pointer and the stride together decide whether the write is a whole level or a bad argument.
 */
static void
region_words_suspect(const void *texture, const uint64_t *word, const void *bytes, size_t stride)
{
	char line[224];
	int n = snprintf(line, sizeof(line),
			 "tl: q2 %p %llx %llx %llx %llx %llx %llx %p %zu %llu\n", texture,
			 (unsigned long long)word[0], (unsigned long long)word[1],
			 (unsigned long long)word[2], (unsigned long long)word[3],
			 (unsigned long long)word[4], (unsigned long long)word[5], bytes, stride,
			 (unsigned long long)region_words_now());

	if (n > 0) {
		(void)write(STDOUT_FILENO, line,
			    (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	}
}

@implementation RedwoodMetalTexture

+ (void)load
{
	/* MTLTexture is what an application sees. MTLTextureImplementation is what Metal
	 * itself checks: -[MTLRenderPassColorAttachmentDescriptorInternal setTexture:]
	 * (0x181a350a0) asks conformsToProtocol: for it and reports "texture is not a
	 * MTLTextureImplementation." Only the conformance is tested on that path -- nothing
	 * sends its 68 members -- so declaring it is honest and is what lets a texture of ours
	 * be a render target at all. */
	const char *names[] = { "MTLTexture", "MTLTextureImplementation" };

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Protocol *p = objc_getProtocol(names[i]);

		if (p == NULL) {
			continue;
		}
		if (!class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
			rw_print("class_addProtocol(%s) refused\n", names[i]);
		}
	}
}

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
			      texture:(struct rwa_texture *)texture
				shape:(const struct rwa_texture_shape *)shape
			    iosurface:(CFTypeRef)surface plane:(NSUInteger)plane
{
	self = [super init];
	if (self == nil) {
		mesa.texture_destroy(texture);
		return nil;
	}
	_owner = device;
	_texture = texture;
	_shape = *shape;
	_iosurface = surface ? CFRetain(surface) : NULL;
	_iosurfacePlane = plane;
	return self;
}

- (void)dealloc
{
	if (_texture != NULL) {
		mesa.texture_destroy(_texture);
		_texture = NULL;
	}
	if (_iosurface != NULL) {
		CFRelease(_iosurface);
	}
	[_parentTexture release];
	[_buffer release];   /* after the texture, whose copy-back reads the buffer */
	[super dealloc];
}

- (void)redwoodSetBuffer:(id)buffer offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytesPerRow
{
	[_buffer release];
	_buffer = [buffer retain];
	_bufferOffset = offset;
	_bufferBytesPerRow = bytesPerRow;
}

/* The surface a texture read through a copy stands for (rw_surface_through_copy), so that
 * -iosurface answers it. */
- (void)redwoodSetSurface:(CFTypeRef)surface plane:(NSUInteger)plane
{
	if (_iosurface != NULL)
		CFRelease(_iosurface);
	_iosurface = surface ? CFRetain(surface) : NULL;
	_iosurfacePlane = plane;
}

- (struct rwa_texture *)mesaTexture { return _texture; }

- (id)device                       { return _owner; }
- (NSUInteger)width                { return _shape.width; }
- (NSUInteger)height               { return _shape.height; }
- (NSUInteger)depth                { return _shape.depth; }
- (NSUInteger)mipmapLevelCount     { return _shape.mip_levels; }
- (NSUInteger)arrayLength          { return _shape.array_length; }
- (NSUInteger)sampleCount          { return _shape.sample_count; }
- (NSUInteger)pixelFormat          { return _shape.pixel_format; }
- (NSUInteger)textureType          { return _shape.type; }
- (NSUInteger)usage                { return _shape.usage; }
- (BOOL)isFramebufferOnly          { return NO; }
- (BOOL)allowGPUOptimizedContents  { return NO; }
- (id)parentTexture                { return _parentTexture; }
- (NSUInteger)parentRelativeLevel  { return 0; }
- (NSUInteger)parentRelativeSlice  { return 0; }
- (id)buffer                       { return _buffer; }
- (NSUInteger)bufferOffset         { return _bufferOffset; }
- (NSUInteger)bufferBytesPerRow    { return _bufferBytesPerRow; }
- (IOSurfaceRef)iosurface          { return (IOSurfaceRef)_iosurface; }
- (NSUInteger)iosurfacePlane       { return _iosurfacePlane; }
/* MTLStorageModeShared, except for a multisample texture, which Metal does not allow to be
 * one: "MTLTextureDescriptor: Multisample textures cannot be allocated with
 * MTLStorageModeShared on this device" is Metal's own sentence, in the framework's own
 * __cstring and not in the validation layer. Answering Private for one is not bookkeeping
 * -- Shared is what makes -replaceRegion: and -getBytes: legal, and those are exactly the
 * two calls the compiler library refuses on a multisample texture (and refuses because
 * neither side can give the samples of a texel a defined order through a map). So the
 * storage mode this reports and the calls that work on it say the same thing. */
- (NSUInteger)storageMode
{
	return _shape.sample_count > 1 ? 2 : 0;   /* MTLStorageModePrivate : Shared */
}
- (NSUInteger)cpuCacheMode         { return 1; }
- (NSUInteger)resourceOptions      { return 0; }
- (NSUInteger)hazardTrackingMode   { return 0; }
- (NSUInteger)allocatedSize        { return 0; }   /* not accounted for yet, and says so */
/* MTLPurgeableStateNonVolatile, for the reason written over the buffer's own: a texture of
 * the compiler library's is held until it is destroyed, so there is no volatile state to
 * report and zero was not a state at all. */
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }
- (id)heap                         { return nil; }
- (NSUInteger)heapOffset           { return 0; }
- (BOOL)isAliasable                { return NO; }
- (void)makeAliasable              { }

- (void)replaceRegion:(MTLRegion)region
	  mipmapLevel:(NSUInteger)level
		slice:(NSUInteger)slice
	    withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)stride
	bytesPerImage:(NSUInteger)image
{
	char why[256] = { 0 };
	{
		static unsigned printed;

		if (printed < 8) {
			printed++;
			region_words(_texture, (const uint64_t *)&region, bytes);
		}
	}
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	if (bytes == NULL || (uintptr_t)bytes >> 48)
		region_words_suspect(_texture, (const uint64_t *)&region, bytes, (size_t)stride);

	(void)image;
	/* Counted here because rwa_texture_write lives in the compiler library, which this round
	 * does not touch; this method is its only caller. */
	__atomic_add_fetch(&hb_texwrite, 1, __ATOMIC_RELAXED);
	if (mesa.texture_write(_texture, (unsigned)level, (unsigned)slice, origin, size, bytes,
			       stride, why, sizeof(why)) != 0) {
		rw_print("writing texels: %s\n", why);
	}
}

- (void)replaceRegion:(MTLRegion)region
	  mipmapLevel:(NSUInteger)level
	    withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)stride
{
	[self replaceRegion:region mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:stride
	      bytesPerImage:0];
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)stride
   bytesPerImage:(NSUInteger)image
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
	   slice:(NSUInteger)slice
{
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	(void)image;
	if (mesa.texture_read(_texture, (unsigned)level, (unsigned)slice, origin, size, bytes,
			      stride, why, sizeof(why)) != 0) {
		rw_print("reading texels: %s\n", why);
	}
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)stride
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
{
	[self getBytes:bytes bytesPerRow:stride bytesPerImage:0 fromRegion:region
	   mipmapLevel:level slice:0];
}

- (BOOL)redwoodReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region
{
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	if (mesa.depth_read == NULL) {
		rw_print("this compiler library has no rwa_depth_read, so it is "
		         "older than this driver and cannot say what a depth buffer holds\n");
		return NO;
	}
	if (mesa.depth_read(_texture, 0, 0, origin, size, depth, stencil, why,
			    sizeof(why)) != 0) {
		rw_print("reading the depth of a texture: %s\n", why);
		return NO;
	}
	return YES;
}

/* A view of a texture is another texture over the same resource (rwa_texture_view), read
 * and rendered in its own format, over a run of the texture's levels. It keeps the texture it
 * came from, which -parentTexture answers. */
- (id)redwoodViewWithPixelFormat:(NSUInteger)format levels:(NSRange)levels
{
	char why[256] = { 0 };
	struct rwa_texture_shape shape = _shape;
	struct rwa_texture *view;
	RedwoodMetalTexture *texture;
	unsigned first = (unsigned)levels.location;

	if (mesa.texture_view == NULL) {
		RW_ONCE("this compiler library has no rwa_texture_view; a texture view is nil\n");
		return nil;
	}
	view = mesa.texture_view(_texture, (unsigned)format, first, (unsigned)levels.length, why,
				 sizeof(why));
	if (view == NULL) {
		rw_print("texture view: %s\n", why);
		return nil;
	}
	shape.pixel_format = (unsigned)format;
	shape.width = MAX(shape.width >> first, 1u);
	if (shape.height)
		shape.height = MAX(shape.height >> first, 1u);
	if (shape.depth)
		shape.depth = MAX(shape.depth >> first, 1u);
	shape.mip_levels = (unsigned)levels.length;
	texture = [[RedwoodMetalTexture alloc] initWithRedwoodDevice:_owner texture:view
							       shape:&shape iosurface:NULL plane:0];
	if (texture != nil) {
		texture->_parentTexture = [self retain];
	}
	return texture;
}

- (id)newTextureViewWithPixelFormat:(NSUInteger)format
{
	return [self redwoodViewWithPixelFormat:format
					 levels:NSMakeRange(0, MAX(_shape.mip_levels, 1))];
}

/*
 * THE REST OF WHAT <MTLTexture>, <MTLTextureSPI>, <MTLResource> AND <MTLResourceSPI>
 * REQUIRE: 27 methods with no answer anywhere in this chain before this section. The same
 * three kinds as everywhere else -- served, true by construction, or refused out loud.
 *
 * A view over part of the slices, or as another type, is refused out loud: rwa_texture_view
 * covers every slice, and a view over fewer would read the wrong ones.
 */
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
			textureType:(NSUInteger)type
			     levels:(NSRange)levels
			     slices:(NSRange)slices
{
	NSUInteger layers = _shape.type == 3 || _shape.type == 4 ? 6 * MAX(_shape.array_length, 1)
								  : MAX(_shape.array_length, 1);

	if (type == _shape.type && slices.location == 0 && slices.length == layers) {
		return [self redwoodViewWithPixelFormat:format levels:levels];
	}
	rw_print("texture view: type %lu levels %lu+%lu slices %lu+%lu of a type %u texture "
		 "of %u levels and %lu slices; only a view of every slice, of the same type, "
		 "is made\n",
		 (unsigned long)type, (unsigned long)levels.location,
		 (unsigned long)levels.length, (unsigned long)slices.location,
		 (unsigned long)slices.length, _shape.type, _shape.mip_levels,
		 (unsigned long)layers);
	return nil;
}
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
			textureType:(NSUInteger)type
			     levels:(NSRange)levels
			     slices:(NSRange)slices
			    swizzle:(MTLTextureSwizzleChannels)swizzle
{
	(void)swizzle;
	return [self newTextureViewWithPixelFormat:format textureType:type levels:levels
					    slices:slices];
}
- (id)newSharedTextureHandle { return nil; }   /* nothing here is shared between processes */

/* Served out of the shape this texture was made with, which is the only place that
 * knowledge exists: a cube is six faces (MTLTextureTypeCube is 3, CubeArray is 4),
 * everything else is one. */
- (NSUInteger)numFaces
{
	return (_shape.type == 3 || _shape.type == 4) ? 6 : 1;
}
/* The identity swizzle, and it is the truth rather than a default: a texture of this driver
 * is made from a descriptor's format alone and nothing reorders its channels, so red reads
 * as red. MTLTextureSwizzleChannelsDefault is that same quadruple. */
- (MTLTextureSwizzleChannels)swizzle
{
	return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
					     MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
}
- (unsigned int)swizzleKey { return 0; }   /* the key of "nothing is swizzled" */
/* Nothing here is a view, so every texture is its own root, and nil is what Apple's own
 * resource answers for one that was allocated rather than derived. */
- (id)rootResource { return nil; }

/*
 * True by construction, and each is a sentence about this driver rather than a shrug:
 *
 *   - nothing aliases: each texture is its own allocation of the compiler library's, there
 *     are no heaps, and -makeAliasable does nothing, so "does this overlap that" is NO for
 *     every pair, for certain;
 *   - nothing is compressed: the library allocates surfaces with no lossless-compression
 *     metadata, so there is no compression and nothing to feed back;
 *   - nothing is sparse: there is no tail, so its size and its first mipmap are zero;
 *   - nothing is a drawable: a texture of this driver never comes from a CAMetalLayer, it is
 *     the attachment a pass draws into and -getBytes: reads back;
 *   - nothing is purgeable or incomplete, for the reason written over the buffer's pair.
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
- (BOOL)isCompressed             { return NO; }
- (NSInteger)compressionFeedback { return 0; }
- (BOOL)isSparse                 { return NO; }
- (NSUInteger)firstMipmapInTail  { return 0; }
- (NSUInteger)tailSizeInBytes    { return 0; }
- (BOOL)isShareable              { return NO; }
- (BOOL)isDrawable               { return NO; }
- (BOOL)isComplete               { return YES; }
- (void)waitUntilComplete        { }
- (BOOL)isPurgeable              { return NO; }
- (NSUInteger)protectionOptions  { return 0; }
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
/* Zero is "not rotated": this texture is not a rotated view of a display surface, which is
 * the only thing in Metal that sets this. */
- (NSUInteger)rotation { return 0; }
/*
 * The identifier an argument buffer would hold for this texture. This driver has no
 * argument buffers -- the device answers no to every tier of them -- and the compiler
 * library hands out no bindless handle, so there is no identifier to give; zero is the one
 * value that cannot be mistaken for a resource, and it is said once because a caller that
 * asks is a caller that was about to write it into a buffer a shader reads.
 */
- (MTLResourceID)gpuResourceID
{
	MTLResourceID resourceID = { 0 };

	RW_ONCE("a GPU resource ID was asked of a texture; this driver has no "
		"argument buffers and no bindless handles, so it answers zero\n");
	return resourceID;
}
- (NSUInteger)gpuHandle { return 0; }
/* Whose allocation this is for the kernel's accounting. We never tell the kernel, so the
 * true answer is this process until somebody says otherwise, and then it is what they said. */
- (int)responsibleProcess
{
	return _responsibleProcess != 0 ? _responsibleProcess : getpid();
}
- (void)setResponsibleProcess:(int)pid { _responsibleProcess = pid; }

@end

@implementation RedwoodMetalBuffer (Texture)

/*
 * A texture laid over this buffer's memory, which CoreAnimation asks for: 120x120 RGBA8 at
 * 512 bytes a row, once per process.
 *
 * The bytes are copied into a texture of its own rather than shared, because Gallium cannot
 * lay one resource over another's memory. The copies happen when the texture is made and
 * again at every -commit (rwa_buffer_textures_refresh), since the processor may have written
 * the buffer in between and nothing tells this driver when. What the GPU writes into the
 * texture goes back into the buffer at -waitUntilCompleted (rwa_buffer_textures_writeback).
 */
- (id)newTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
		   bytesPerRow:(NSUInteger)bytesPerRow
{
	char why[512] = { 0 };
	struct rwa_texture_shape shape;
	struct rwa_texture *texture;

	if (_mesa == NULL || mesa.texture_from_buffer == NULL) {
		rw_print("a texture over a buffer needs a buffer the compiler library made\n");
		return nil;
	}
	memset(&shape, 0, sizeof(shape));
	shape.pixel_format = (unsigned)[descriptor pixelFormat];
	shape.type = (unsigned)[descriptor textureType];
	shape.width = (unsigned)[descriptor width];
	shape.height = (unsigned)[descriptor height];
	shape.depth = (unsigned)[descriptor depth];
	shape.array_length = (unsigned)[descriptor arrayLength];
	shape.mip_levels = (unsigned)[descriptor mipmapLevelCount];
	shape.sample_count = (unsigned)[descriptor sampleCount];
	shape.usage = (unsigned)[descriptor usage];
	static unsigned said;
	if (said < 48)
		rw_print("texture over buffer %u: format %u %ux%u, %lu bytes a row at offset %lu\n",
			 said++, shape.pixel_format, shape.width, shape.height,
			 (unsigned long)bytesPerRow, (unsigned long)offset);
	texture = mesa.texture_from_buffer([_owner mesaDevice], &shape, _mesa, offset,
					   (unsigned)bytesPerRow, why, sizeof(why));
	if (texture == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	RedwoodMetalTexture *made = [[RedwoodMetalTexture alloc] initWithRedwoodDevice:_owner
										  texture:texture
										    shape:&shape
										iosurface:NULL plane:0];
	[made redwoodSetBuffer:self offset:offset bytesPerRow:bytesPerRow];
	return made;
}

- (id)newLinearTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
			 bytesPerRow:(NSUInteger)bytesPerRow
		       bytesPerImage:(NSUInteger)bytesPerImage
{
	(void)bytesPerImage;
	return [self newTextureWithDescriptor:descriptor offset:offset bytesPerRow:bytesPerRow];
}

@end

/*
 * An IOSurface as a texture. A surface whose rows lie a whole number of 64 bytes apart is imported
 * where it lies (rwd_texture_for_surface). One whose rows do not cannot be sampled there: the A830
 * reads a linear texture at a pitch of whole 64-byte units, and SpringBoard writes its glyph
 * surfaces at the 224 or 416 bytes a row it asked for, whatever pitch the surface reports (render84
 * sampled them sheared; render88, with the kernel padding the pitch, read the rows sheared on the
 * processor side as well). Such a surface is read the way a caller's own memory is: through a
 * no-copy buffer over the surface's memory and a texture over that buffer, whose bytes are copied
 * in before each commit and written back once the GPU has written them (rwa_texture_from_buffer,
 * rwa_buffer_set_mirror). nil leaves the surface to the import in place.
 */
static id
rw_surface_through_copy(RedwoodMetalDevice *device, MTLTextureDescriptor *d, void *surface,
			NSUInteger plane)
{
	static size_t (*row_bytes)(void *), (*alloc_size)(void *), (*plane_count)(void *);
	static void *(*base_address)(void *);
	static unsigned said;
	RedwoodMetalTexture *texture;
	id buffer;
	size_t row;

	if (row_bytes == NULL) {
		*(void **)&alloc_size = dlsym(RTLD_DEFAULT, "IOSurfaceGetAllocSize");
		*(void **)&plane_count = dlsym(RTLD_DEFAULT, "IOSurfaceGetPlaneCount");
		*(void **)&base_address = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
		*(void **)&row_bytes = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
	}
	if (surface == NULL || row_bytes == NULL || alloc_size == NULL || plane_count == NULL ||
	    base_address == NULL || plane != 0 || plane_count(surface) > 1 ||
	    [d textureType] != MTLTextureType2D || [d mipmapLevelCount] > 1 || [d sampleCount] > 1)
		return nil;
	row = row_bytes(surface);
	if (row == 0 || row % 64 == 0)
		return nil;
	/* the buffer reads and writes the surface's memory for as long as it lives */
	CFRetain(surface);
	buffer = [device newBufferWithBytesNoCopy:base_address(surface) length:alloc_size(surface)
					  options:0 deallocator:^(void *bytes, NSUInteger length) {
		(void)bytes; (void)length;
		CFRelease(surface);
	}];
	if (buffer == nil) {
		CFRelease(surface);
		return nil;
	}
	texture = [buffer newTextureWithDescriptor:d offset:0 bytesPerRow:row];
	[buffer release];   /* the texture holds it (redwoodSetBuffer) */
	if (texture == nil)
		return nil;
	[texture redwoodSetSurface:surface plane:plane];
	if (said < 16)
		rw_print("surface %p of %lux%lu at %zu bytes a row is sampled through a copy (%u)\n",
			 surface, (unsigned long)[d width], (unsigned long)[d height], row, said++);
	return texture;
}

@implementation RedwoodMetalDevice (Surface)

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(void *)s plane:(NSUInteger)p
{
	char why[768] = { 0 };
	struct rwd_surface took;
	struct rwa_texture_shape shape;   /* this file's own declaration; rwd_texture_shape is the
					   * same layout under the other file's name, and the cast
					   * below is the one place the two meet */
	struct rwa_texture *texture;
	id copy;

	if (_mesa == NULL) {
		rw_print("a drawable's texture needs the compiler library\n");
		return nil;
	}
	if ((copy = rw_surface_through_copy(self, d, s, p)) != nil)
		return copy;
	memset(&shape, 0, sizeof(shape));
	shape.pixel_format = (unsigned)[d pixelFormat];
	shape.type = (unsigned)[d textureType];
	shape.width = (unsigned)[d width];
	shape.height = (unsigned)[d height];
	shape.depth = (unsigned)[d depth];
	shape.array_length = (unsigned)[d arrayLength];
	shape.mip_levels = (unsigned)[d mipmapLevelCount];
	shape.sample_count = (unsigned)[d sampleCount];
	shape.usage = (unsigned)[d usage];
	texture = rwd_texture_for_surface(_mesa, s, p, (const struct rwd_texture_shape *)&shape,
					  &took, why, sizeof(why));
	if (texture == NULL) {
		static unsigned long refusals;
		unsigned long n = __sync_add_and_fetch(&refusals, 1);

		/* Every one of the first 64, then powers of two: the refusals of one boot have
		 * several reasons, and a count shared by all of them hides which one recurs. */
		if (n <= 64 || (n & (n - 1)) == 0)
			rw_print("no drawable (%lu): format %u usage %#x: %s\n", n,
				 shape.pixel_format, shape.usage, why);
		return nil;
	}
	return [[RedwoodMetalTexture alloc] initWithRedwoodDevice:self
							 texture:texture
							   shape:&shape iosurface:s plane:p];
}

@end

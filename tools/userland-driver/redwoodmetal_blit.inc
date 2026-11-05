/* Blit calls only record work. The command buffer executes these entries in the
 * same list as draws and dispatches, with the same resource ownership. */
static int
rw_run_copy(RedwoodMetalWork *work, char *why, size_t why_size)
{
	if (work->copyKind == RW_COPY_ERROR) {
		snprintf(why, why_size, "%s", work->copyError);
		return -1;
	}
	if (work->copyKind == RW_COPY_BUFFER && mesa.buffer_copy) {
		return mesa.buffer_copy([work->buffers[0] mesaBuffer], work->copyOffsets[0],
			[work->buffers[1] mesaBuffer], work->copyOffsets[1], work->copyLength,
			why, why_size);
	}
	if (work->copyKind == RW_COPY_MIPMAP) {
		if (mesa.texture_generate_mipmaps == NULL) {
			snprintf(why, why_size, "this compiler library does not generate mipmaps");
			return -1;
		}
		return mesa.texture_generate_mipmaps([work->textures[0] mesaTexture], why,
						     why_size);
	}
	/* Written by the processor from the buffer's memory when the command buffer reaches it,
	 * which is how -replaceRegion: fills a texture; MercuryPosterExtension uploads its KTX
	 * textures this way. */
	if (work->copyKind == RW_COPY_BUFFER_TEXTURE && mesa.texture_write) {
		const char *bytes = [work->buffers[0] contents];

		return mesa.texture_write([work->textures[0] mesaTexture], work->copyLevels[1],
			work->copySlices[1], work->copyOrigins[1], work->copySize,
			bytes ? bytes + work->copyOffsets[0] : NULL, work->copyLength, why, why_size);
	}
	if (work->copyKind == RW_COPY_TEXTURE && mesa.texture_copy) {
		return mesa.texture_copy([work->textures[0] mesaTexture], work->copyLevels[0],
			work->copySlices[0], work->copyOrigins[0], [work->textures[1] mesaTexture],
			work->copyLevels[1], work->copySlices[1], work->copyOrigins[1],
			work->copySize, why, why_size);
	}
	snprintf(why, why_size, "this compiler library does not implement the requested copy");
	return -1;
}

#include <objc/runtime.h>

/* A selector nobody answers aborts the process, and backboardd died fifteen times in one run on
 * -generateMipmapsForTexture: before anything was drawn. What a protocol of this encoder declares
 * is answered by forwarding: one log line each, so a single run shows every gap instead of only the
 * first. A selector no protocol declares still aborts. The protocols are looked up by name because
 * this file also builds into a host test that does not link Metal. */
static BOOL
protocol_declares(const char *name, SEL selector, struct objc_method_description *out)
{
	Protocol *protocol = objc_getProtocol(name);

	if (protocol == NULL) {
		return NO;
	}
	*out = protocol_getMethodDescription(protocol, selector, YES, YES);
	if (out->types == NULL) {
		*out = protocol_getMethodDescription(protocol, selector, NO, YES);
	}
	return out->types != NULL;
}

@interface RedwoodMetalBlitEncoder : _MTLCommandEncoder {
	RedwoodMetalCommandBuffer *_commandBuffer;
	BOOL _ended;
}
- (instancetype)initWithRedwoodCommandBuffer:(RedwoodMetalCommandBuffer *)commandBuffer;
@end

@implementation RedwoodMetalBlitEncoder
- (instancetype)initWithRedwoodCommandBuffer:(RedwoodMetalCommandBuffer *)commandBuffer
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self) _commandBuffer = commandBuffer;
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)rejectCopy:(const char *)reason
{
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];
	work->copyKind = RW_COPY_ERROR;
	snprintf(work->copyError, sizeof(work->copyError), "%s", reason);
	[_commandBuffer recordWork:work];
	[work release];
}

- (BOOL)acceptSource:(id)source destination:(id)destination resourceClass:(Class)cls
{
	if (_ended) {
		[NSException raise:NSInternalInconsistencyException format:@"copy after endEncoding"];
		return NO;
	}
	if (![source isKindOfClass:cls] || ![destination isKindOfClass:cls] ||
	    [source device] != [self device] || [destination device] != [self device]) {
		[self rejectCopy:"copy requires an active encoder and resources from its device"];
		return NO;
	}
	return YES;
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset
	      toBuffer:(id)destination destinationOffset:(NSUInteger)destinationOffset
		  size:(NSUInteger)size
{
	if (![self acceptSource:source destination:destination resourceClass:[RedwoodMetalBuffer class]])
		return;
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];
	work->copyKind = RW_COPY_BUFFER;
	work->count = 2;
	work->buffers[0] = source;
	work->buffers[1] = destination;
	work->copyOffsets[0] = sourceOffset;
	work->copyOffsets[1] = destinationOffset;
	work->copyLength = size;
	[_commandBuffer recordWork:work];
	[work release];
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset
     sourceBytesPerRow:(NSUInteger)bytesPerRow sourceBytesPerImage:(NSUInteger)bytesPerImage
	    sourceSize:(MTLSize)size toTexture:(id)destination
      destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)origin
{
	(void)bytesPerImage;   /* rwa_texture_write takes a 3D region's slices as packed rows */
	if (_ended || ![source isKindOfClass:[RedwoodMetalBuffer class]] ||
	    ![destination isKindOfClass:[RedwoodMetalTexture class]] ||
	    [source device] != [self device] || [destination device] != [self device]) {
		[self rejectCopy:"buffer-to-texture copy requires an active encoder and a buffer and "
				 "a texture of its device"];
		return;
	}
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];
	work->copyKind = RW_COPY_BUFFER_TEXTURE;
	work->count = 1;
	work->buffers[0] = source;
	work->textureCount = 1;
	work->textures[0] = destination;
	work->copyOffsets[0] = sourceOffset;
	work->copyLength = bytesPerRow;
	work->copyLevels[1] = (unsigned)level;
	work->copySlices[1] = (unsigned)slice;
	unsigned to[3] = { origin.x, origin.y, origin.z };
	unsigned extent[3] = { size.width, size.height, size.depth };
	memcpy(work->copyOrigins[1], to, sizeof(to));
	memcpy(work->copySize, extent, sizeof(extent));
	[_commandBuffer recordWork:work];
	[work release];
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset
     sourceBytesPerRow:(NSUInteger)bytesPerRow sourceBytesPerImage:(NSUInteger)bytesPerImage
	    sourceSize:(MTLSize)size toTexture:(id)destination
      destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)origin options:(NSUInteger)options
{
	(void)options;
	[self copyFromBuffer:source sourceOffset:sourceOffset sourceBytesPerRow:bytesPerRow
	 sourceBytesPerImage:bytesPerImage sourceSize:size toTexture:destination
	    destinationSlice:slice destinationLevel:level destinationOrigin:origin];
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)sourceSlice
	   sourceLevel:(NSUInteger)sourceLevel sourceOrigin:(MTLOrigin)sourceOrigin
	    sourceSize:(MTLSize)size toTexture:(id)destination
      destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
     destinationOrigin:(MTLOrigin)destinationOrigin
{
	if (![self acceptSource:source destination:destination resourceClass:[RedwoodMetalTexture class]])
		return;
	if (sourceSlice > UINT32_MAX || sourceLevel > UINT32_MAX ||
	    destinationSlice > UINT32_MAX || destinationLevel > UINT32_MAX ||
	    sourceOrigin.x > UINT32_MAX || sourceOrigin.y > UINT32_MAX || sourceOrigin.z > UINT32_MAX ||
	    destinationOrigin.x > UINT32_MAX || destinationOrigin.y > UINT32_MAX || destinationOrigin.z > UINT32_MAX ||
	    size.width > UINT32_MAX || size.height > UINT32_MAX || size.depth > UINT32_MAX) {
		[self rejectCopy:"texture copy parameters exceed the 32-bit resource contract"];
		return;
	}
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];
	work->copyKind = RW_COPY_TEXTURE;
	work->textureCount = 2;
	work->textures[0] = source;
	work->textures[1] = destination;
	work->copyLevels[0] = sourceLevel;
	work->copyLevels[1] = destinationLevel;
	work->copySlices[0] = sourceSlice;
	work->copySlices[1] = destinationSlice;
	unsigned from[3] = { sourceOrigin.x, sourceOrigin.y, sourceOrigin.z };
	unsigned to[3] = { destinationOrigin.x, destinationOrigin.y, destinationOrigin.z };
	unsigned extent[3] = { size.width, size.height, size.depth };
	memcpy(work->copyOrigins[0], from, sizeof(from));
	memcpy(work->copyOrigins[1], to, sizeof(to));
	memcpy(work->copySize, extent, sizeof(extent));
	[_commandBuffer recordWork:work];
	[work release];
}

/* Whole levels and slices, recorded as one region copy each. */
- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)sourceSlice
	   sourceLevel:(NSUInteger)sourceLevel toTexture:(id)destination
      destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
	    sliceCount:(NSUInteger)sliceCount levelCount:(NSUInteger)levelCount
{
	if (![self acceptSource:source destination:destination resourceClass:[RedwoodMetalTexture class]])
		return;
	id<MTLTexture> from = source;

	for (NSUInteger l = 0; l < levelCount; l++) {
		NSUInteger level = sourceLevel + l;
		MTLSize size = MTLSizeMake(MAX([from width] >> level, 1), MAX([from height] >> level, 1),
					   MAX([from depth] >> level, 1));

		for (NSUInteger s = 0; s < sliceCount; s++)
			[self copyFromTexture:source sourceSlice:sourceSlice + s sourceLevel:level
				 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size toTexture:destination
			     destinationSlice:destinationSlice + s destinationLevel:destinationLevel + l
			    destinationOrigin:MTLOriginMake(0, 0, 0)];
	}
}

- (void)generateMipmapsForTexture:(id)texture
{
	if (![texture isKindOfClass:[RedwoodMetalTexture class]] || [texture device] != [self device]) {
		[self rejectCopy:"mipmap generation requires a texture of this device"];
		return;
	}
	/* Recorded, not run: level 0 is written by whatever this command buffer drew into it
	 * earlier, and the downsample has to happen after that and before whatever samples the
	 * higher levels. The command buffer runs its work in the order it was recorded.
	 */
	RedwoodMetalWork *work = [[RedwoodMetalWork alloc] init];
	work->copyKind = RW_COPY_MIPMAP;
	work->textures[0] = texture;
	[_commandBuffer recordWork:work];
	[work release];
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
	NSMethodSignature *signature = [super methodSignatureForSelector:selector];
	struct objc_method_description description;

	if (signature == NULL &&
	    (protocol_declares("MTLBlitCommandEncoder", selector, &description) ||
	     protocol_declares("MTLCommandEncoder", selector, &description))) {
		signature = [NSMethodSignature signatureWithObjCTypes:description.types];
	}
	return signature;
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
	static const char none[32];
	NSUInteger length = invocation.methodSignature.methodReturnLength;

	rw_print("blit selector %s is not implemented\n", sel_getName(invocation.selector));
	if (length > 0 && length <= sizeof(none)) {
		[invocation setReturnValue:(void *)none];
	}
}

- (void)endEncoding
{
	if (_ended) {
		[NSException raise:NSInternalInconsistencyException format:@"blit encoder already ended"];
		return;
	}
	_ended = YES;
	[super endEncoding];
	_commandBuffer = nil;
}
@end

static void
rw_record_resolve_blits(RedwoodMetalCommandBuffer *cb, unsigned colorCount, id color[8],
    id resolveTo[8], const unsigned colorLevel[8], const unsigned colorSlice[8],
    const unsigned resolveLevel[8], const unsigned resolveSlice[8])
{
	for (unsigned i = 0; i < colorCount && i < 8; i++) {
		id from = color[i], to = resolveTo[i];
		if (from == nil || to == nil) {
			[resolveTo[i] release];
			resolveTo[i] = nil;
			continue;
		}
		RedwoodMetalWork *w = [[RedwoodMetalWork alloc] init];
		unsigned fw = (unsigned)MAX([(id<MTLTexture>)from width], 1ul);
		unsigned fh = (unsigned)MAX([(id<MTLTexture>)from height], 1ul);
		unsigned tw = (unsigned)MAX([(id<MTLTexture>)to width] >> resolveLevel[i], 1ul);
		unsigned th = (unsigned)MAX([(id<MTLTexture>)to height] >> resolveLevel[i], 1ul);
		BOOL hasParent = [from respondsToSelector:@selector(parentTexture)] && [from parentTexture] != nil;

		w->copyKind = RW_BLIT_TEXTURE;
		w->textureCount = 2;
		w->textures[0] = from;
		w->textures[1] = to;
		w->copyLevels[0] = hasParent ? 0 : colorLevel[i];
		w->copyLevels[1] = resolveLevel[i];
		w->copySlices[0] = hasParent ? 0 : colorSlice[i];
		w->copySlices[1] = resolveSlice[i];
		w->copySize[0] = MIN(fw, tw);
		w->copySize[1] = MIN(fh, th);
		w->copySize[2] = 1;
		[cb recordWork:w];
		[w release];
		[resolveTo[i] release];
		resolveTo[i] = nil;
	}
}

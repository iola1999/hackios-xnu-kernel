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

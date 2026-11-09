/* Encoders fill borrowed references; recording owns them until the command buffer
 * releases this work item. A discarded, unrecorded item owns no resources. Each slot
 * owns its reference independently, including repeated objects in different slots. */
@implementation AnnibaleMetalWork

- (void)retainResources
{
	if (_ownsResources) {
		return;
	}
	_ownsResources = YES;
	[(id)pipeline retain];
	[(id)render retain];
	[depth retain];
	[depthStencilState retain];
	[indexBuffer retain];
	for (unsigned i = 0; i < colorCount; i++) {
		[color[i] retain];
		[resolve[i] retain];
	}
	for (unsigned i = 0; i < vertexBufferCount; i++) {
		[vertexBuffers[i] retain];
	}
	for (unsigned i = 0; i < count; i++) {
		[buffers[i] retain];
	}
	for (unsigned i = 0; i < textureCount; i++) {
		[textures[i] retain];
	}
	for (unsigned i = 0; i < samplerCount; i++) {
		[samplers[i] retain];
	}
	for (unsigned i = 0; i < vertexTextureCount; i++) {
		[vertexTextures[i] retain];
	}
	for (unsigned i = 0; i < vertexSamplerCount; i++) {
		[vertexSamplers[i] retain];
	}
}

- (void)dealloc
{
	if (_ownsResources) {
		[(id)pipeline release];
		[(id)render release];
		[depth release];
		[depthStencilState release];
		[indexBuffer release];
		for (unsigned i = 0; i < colorCount; i++) {
			[color[i] release];
			[resolve[i] release];
		}
		for (unsigned i = 0; i < vertexBufferCount; i++) {
			[vertexBuffers[i] release];
		}
		for (unsigned i = 0; i < count; i++) {
			[buffers[i] release];
		}
		for (unsigned i = 0; i < textureCount; i++) {
			[textures[i] release];
		}
		for (unsigned i = 0; i < samplerCount; i++) {
			[samplers[i] release];
		}
		for (unsigned i = 0; i < vertexTextureCount; i++) {
			[vertexTextures[i] release];
		}
		for (unsigned i = 0; i < vertexSamplerCount; i++) {
			[vertexSamplers[i] release];
		}
	}
	[super dealloc];
}

@end

/* Apple's own "this buffer still owes a signal" flag, cleared when we pay, so that its
 * bookkeeping and ours cannot disagree about a debt that has been settled. */
static void
annibale_clear_semaphore_debt(id commandBuffer)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandBuffer");

		looked = YES;
		ivar = base != Nil ?
		    class_getInstanceVariable(base, "_needsCommandBufferSemaphoreSignal") : NULL;
	}
	if (ivar != NULL) {
		*(unsigned char *)((char *)commandBuffer + ivar_getOffset(ivar)) = 0;
	}
}

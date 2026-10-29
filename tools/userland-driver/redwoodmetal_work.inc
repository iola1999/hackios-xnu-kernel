/* Encoders fill borrowed references; recording owns them until the command buffer
 * releases this work item. A discarded, unrecorded item owns no resources. Each slot
 * owns its reference independently, including repeated objects in different slots. */
@implementation RedwoodMetalWork

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
	}
	[super dealloc];
}

@end

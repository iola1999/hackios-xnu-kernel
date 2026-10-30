/*
 * Sampler and depth/stencil state objects, included into redwoodmetal.m.
 */

@implementation RedwoodMetalSampler

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
			      sampler:(struct rwa_sampler *)sampler
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilySampler sends to super; the descriptor is passed on
	 * because the base keeps it and the pipeline-state assertions taught us not to send
	 * nil to one of these */
	self = [super initWithDevice:device samplerDescriptor:descriptor];
	if (self == nil) {
		mesa.sampler_destroy(sampler);
		return nil;
	}
	_sampler = sampler;
	return self;
}

- (void)dealloc
{
	if (_sampler != NULL) {
		mesa.sampler_destroy(_sampler);
	}
	[super dealloc];
}

- (struct rwa_sampler *)mesaSampler { return _sampler; }

@end

@implementation RedwoodMetalDepthStencilState

- (instancetype)initWithRedwoodDevice:(RedwoodMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor
{
	/* The base class's own designated initialiser, which validates the descriptor,
	 * sends `init` to super and fills _device and _label (0x181a29b38). The descriptor
	 * is handed on for the same reason the sampler's is: the base reads it, and a nil
	 * there is reported by Apple's own validation. */
	self = [super initWithDevice:device depthStencilDescriptor:descriptor];
	if (self == nil) {
		mesa.depth_stencil_destroy(state);
		return nil;
	}
	_state = state;

	/*
	 * The four answers <MTLDepthStencilStateSPI> requires and the base class gives as
	 * NO (nil-stubs at 0x181b1b5e0 and its neighbours). They are the only methods of
	 * this object that are a statement about what it will do, so they are derived from
	 * the descriptor rather than left at a default that would be false for every state
	 * that writes anything.
	 *
	 * "Reads" means the test consults the buffer: MTLCompareFunctionAlways consults
	 * nothing, anything else does. For the stencil an operation other than Keep also
	 * reads it, because incrementing or inverting a value means fetching it first.
	 */
	MTLStencilDescriptor *faces[2] = { [descriptor frontFaceStencil],
					   [descriptor backFaceStencil] };

	_writesDepth = [descriptor isDepthWriteEnabled];
	_readsDepth = [descriptor depthCompareFunction] != MTLCompareFunctionAlways;
	for (unsigned i = 0; i < 2; i++) {
		MTLStencilDescriptor *f = faces[i];
		BOOL keeps;

		if (f == nil) {
			continue;   /* null_resettable, so this is Metal's default: no stencil */
		}
		keeps = [f stencilFailureOperation] == MTLStencilOperationKeep &&
			[f depthFailureOperation] == MTLStencilOperationKeep &&
			[f depthStencilPassOperation] == MTLStencilOperationKeep;
		if ([f stencilCompareFunction] != MTLCompareFunctionAlways || !keeps) {
			_readsStencil = YES;
		}
		if (!keeps && [f writeMask] != 0) {
			_writesStencil = YES;
		}
	}
	return self;
}

- (void)dealloc
{
	if (_state != NULL) {
		mesa.depth_stencil_destroy(_state);
	}
	[super dealloc];
}

- (struct rwa_depth_stencil *)mesaDepthStencil { return _state; }

- (BOOL)readsDepth    { return _readsDepth; }
- (BOOL)writesDepth   { return _writesDepth; }
- (BOOL)readsStencil  { return _readsStencil; }
- (BOOL)writesStencil { return _writesStencil; }

@end

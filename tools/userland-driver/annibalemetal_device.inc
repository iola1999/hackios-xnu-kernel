@implementation AnnibaleMetalDevice

+ (void)load
{
	ensure_spi(self);
	/*
	 * Diagnostic only, paired with the kernel's `annibale: <comm> bad access at ... pc ...` line
	 * (e933479): without this address the pc cannot be attributed to an image, because the dylib is
	 * position independent and its slide differs per process. Kept while the nil-buffer line is
	 * suspended.
	 *
	 * Behind ANNIBALE_DIAG like every other print that exists only for one investigation: with the
	 * variable unset the load prints nothing, so turning a diagnostic on does not change the library
	 * the round's baseline describes. package.py puts the variable in these processes' environment
	 * and records it in the manifest.
	 */
	if (getenv("ANNIBALE_DIAG") != NULL) {
		rw_print("rwa metal: driver code at %p\n", (void *)&rw_print);
	}
}

- (instancetype)initWithAcceleratorPort:(io_service_t)port
{
	ensure_spi([self class]);
	rw_stitch_install_hooks();

	self = [super init];
	if (self == nil) {
		return nil;
	}
	_acceleratorService = port;
	if (port != IO_OBJECT_NULL) {
		IOObjectRetain(port);
	}

	/* The gpu this driver would actually drive. Not fatal if it is absent: the port
	 * starts the Adreno only with the boot argument gpu=1, and a device object that
	 * cannot allocate is still the thing under test until the resource layer exists. */
	io_service_t gpu = IOServiceGetMatchingService(0, IOServiceMatching("AnnibaleGPU"));

	_gpu = IO_OBJECT_NULL;
	if (gpu == IO_OBJECT_NULL) {
		rw_print("no AnnibaleGPU service (the gpu was not started)\n");
	} else {
		kern_return_t kr = IOServiceOpen(gpu, mach_task_self(), 0, &_gpu);

		rw_print("AnnibaleGPU client %s (%#x)\n",
		         kr == KERN_SUCCESS ? "opened" : "refused", kr);
		IOObjectRelease(gpu);
	}

	/* And the compiler, if it is there. Its absence is not fatal: the device still has to
	 * exist and answer, it just cannot make a pipeline (and says so when asked). */
	if (mesa_available()) {
		char why[256] = { 0 };

		_mesa = mesa.device_open(why, sizeof(why));
		rw_print("the compiler library %s\n",
		         _mesa != NULL ? mesa.device_name(_mesa) : why);
		/* freedreno reads its debug switches without saying so; these lines are the only
		 * record that a job's FD_MESA_DEBUG or IR3_SHADER_DEBUG (package.py JOB_ENV) reached
		 * the process. */
		static const char *const switches[] = { "FD_MESA_DEBUG", "IR3_SHADER_DEBUG" };
		for (unsigned i = 0; i < sizeof(switches) / sizeof(switches[0]); i++)
			if (getenv(switches[i]) != NULL)
				rw_print("%s=%s\n", switches[i], getenv(switches[i]));
	}

	/* The struct Metal reads out of this device by raw pointer, filled here rather than on
	 * demand: the pointer is handed out without a lock, from whichever thread a renderer
	 * happens to be on, and here is the one moment when nobody can have asked yet. The
	 * name is Apple's own rule for a device that declines -- its -name with the spaces
	 * taken out (0x181a3ca0c) -- but the bytes are COPIED, which Apple's branch does not
	 * need to do: it keeps the autoreleased -UTF8String in a struct on its own stack and is
	 * done with it nine instructions later, while this one is answered to callers for as
	 * long as the device lives. See -targetDeviceInfo. */
	NSString *plain = [[self name] stringByReplacingOccurrencesOfString:@" "
								 withString:@""];
	const char *bytes = [plain UTF8String];

	snprintf(_targetName, sizeof(_targetName), "%s", bytes != NULL ? bytes : "");
	_target.target = 0;
	_target.version = 0;
	_target.name = _targetName;
	rw_print("targetDeviceInfo answers {0, 0, \"%s\"} -- _MTLDevice's own entry "
	         "answers NULL and Metal reads it without looking, so the precompiled pipelines "
	         "this device will be asked for are pipelines.%s\n", _target.name, _target.name);
	return self;
}

- (struct rwa_device *)mesaDevice
{
	return _mesa;
}

- (void)dealloc
{
	if (_mesa != NULL) {
		rw_pipe_forget(_mesa);
		mesa.device_close(_mesa);
	}
	if (_gpu != IO_OBJECT_NULL) {
		IOServiceClose(_gpu);
	}
	if (_acceleratorService != IO_OBJECT_NULL) {
		IOObjectRelease(_acceleratorService);
	}
	[super dealloc];
}

/* The profile is what -[_MTLDevice initLimits] switches on, and it was swept on the device:
 * 0, 1 and 4 to 9 fill the limits table, while 2, 3 and 10 to 12 abort inside Apple on
 * `_limits.maxColorAttachments > 0`. 0 is taken because it is the lowest that works and
 * because its table (31 buffers, 16 samplers, 512 threads per threadgroup, 16 KB of
 * threadgroup memory, 8192 textures) is a plausible floor for this Adreno.
 *
 * "Which profile the hardware deserves is a separate question, to be answered when something
 * is drawn" -- and something has now been drawn, so here is the answer so far. This number is
 * the ONLY input Apple has for what this device supports: -[_MTLDevice initGPUFamilySupport]
 * switches on it and pushes a list of MTLGPUFamily into the vector that -supportsFamily:
 * searches. Profile 0 pushes Apple1 and Common1 and nothing else.
 *
 * That is why SpringBoard cannot start. Siri's orb prewarms an MPSImageGaussianBlur, and
 * MPSDevice::GetMPSDevice is a descending ladder of -supportsFamily: from Apple8 (1008) down
 * to Apple2 (1002) which returns NULL when none answers. It never asks about Apple1 -- the
 * constant 0x3e9 is nowhere in that function -- so the one family we claim is the one family
 * it does not want, and the process aborts in MPSKernel with "Failed to get MPSDevice"
 * (docs/metal.md 10.47). Profile 1 is the lowest that pushes Apple2.
 *
 * So the profile is a boot-arg (metal_profile=N), not because it should stay configurable but
 * because the cost of each step up has to be measured rather than argued: every family claimed
 * is a promise, and some of those promises this driver cannot keep. Answering LESS is not the
 * safe direction either -- an empty family vector makes
 * +[MTLFXSpatialScalerDescriptor supportsDevice:] take this for a discrete GPU and say yes
 * unconditionally -- so there is no answer here that is safe by default, only one that is
 * measured. Apple's own A11 driver answers 5.
 */
- (NSUInteger)featureProfile
{
	static NSUInteger profile = (NSUInteger)-1;

	if (profile == (NSUInteger)-1) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		profile = 0;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "metal_profile=")) != NULL && (at == args || at[-1] == ' ')) {
			profile = (NSUInteger)strtoul(at + strlen("metal_profile="), NULL, 10);
		}
		rw_print("feature profile %lu, which is what Metal reads this device's "
		         "whole capability surface out of (metal_profile= to change it; 0 claims Apple1 "
		         "alone, 1 adds Apple2, which is the lowest rung MPS asks about)\n",
		         (unsigned long)profile);
	}
	return profile;
}

/* A placeholder, and named as one: nothing has yet measured what the kernel's gemNew will
 * give in one allocation. It only has to be non-zero for initLimits to accept it. */
- (uint64_t)maxBufferLength
{
	return 256ULL << 20;
}

/*
 * The render pipeline, by the same rule as the compute one: both of _MTLDevice's entry points
 * go through its nil -compiler, so a driver that leaves them alone gets nil and no reason.
 *
 * One asymmetry is deliberate and is the measured road rather than a shortcut. When the
 * descriptor names no vertex function, the compiler library supplies a pass-through vertex
 * stage over the whole attachment -- which is exactly how air_run has drawn every fragment
 * shader of the corpus, 255 of them compared pixel by pixel (docs/air.md). A descriptor that
 * does name one passes both modules and nothing is generated.
 */
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
				     error:(NSError **)error
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (descriptor == nil) {
		return [self fail:error with:@"no render pipeline descriptor"];
	}
	if (_mesa == NULL) {
		return [self fail:error with:@"there is no GPU compiler here: either the Adreno "
					     @"was not started (gpu=1) or libannibaleair.dylib is "
					     @"missing or older than this driver"];
	}
	if (mesa.render_create == NULL) {
		/* The order of these two matters: the first version asked about render_create
		 * first and would have told a caller that a library which never loaded "can
		 * compute but not yet draw", which is a sentence about a library that is not
		 * there. */
		return [self fail:error with:@"this compiler library can compute but not yet "
					     @"draw"];
	}

	id vertex = [descriptor vertexFunction];
	id fragment = [descriptor fragmentFunction];

	/* a specialization proxy (annibalemetal_stitch.inc) answers to the specialized name; the
	 * module's entry point is the function underneath */
	if ([vertex respondsToSelector:@selector(annibaleUnspecializedFunction)]) {
		vertex = [vertex annibaleUnspecializedFunction];
	}
	if ([fragment respondsToSelector:@selector(annibaleUnspecializedFunction)]) {
		fragment = [fragment annibaleUnspecializedFunction];
	}
	rw_stitch_dump_linked(descriptor);
	rw_dump_pipeline_air(descriptor);
	/* No fragment function is a depth/stencil-only pipeline (RenderBox's path_edges); the
	 * library gives it an empty fragment stage and writes no colour. */
	if (fragment == nil && vertex == nil) {
		return [self fail:error with:@"the descriptor has neither a vertex nor a fragment "
					     @"function"];
	}
	if ((fragment != nil && ![fragment respondsToSelector:bitcode]) ||
	    (vertex != nil && ![vertex respondsToSelector:bitcode])) {
		return [self fail:error with:@"a function in this descriptor does not answer "
					     @"bitcodeData, so its AIR cannot be read"];
	}
	NSData *fragment_air = fragment != nil ?
	    ((NSData *(*)(id, SEL))objc_msgSend)(fragment, bitcode) : nil;
	NSData *vertex_air = vertex != nil ?
	    ((NSData *(*)(id, SEL))objc_msgSend)(vertex, bitcode) : nil;
	const char *fragment_name = fragment != nil ? [[fragment name] UTF8String] : "(none)";

	if (fragment != nil && (fragment_air == nil || [fragment_air length] == 0)) {
		return [self fail:error with:@"the fragment function has no bitcode"];
	}

	struct rwa_render_shape shape = { .sample_count = rw_one_sample() ? 1u : (unsigned)[descriptor sampleCount] };

	/*
	 * THE COLOUR ATTACHMENTS, each one's format AND ITS BLEND STATE.
	 *
	 * The blend half is what the home screen was missing: with no blend state in the
	 * contract the library made every pipeline opaque, and CoreAnimation composites with
	 * PREMULTIPLIED ALPHA -- so a label layer whose background is transparent black was
	 * drawn as opaque black, which is the black box behind every icon name in the
	 * photographs.
	 *
	 * All eight are read, as the formats are, and for the same reason: Apple's array
	 * answers -objectAtIndexedSubscript: for every index in range and an attachment an
	 * application never touched holds Apple's own defaults -- blendingEnabled NO,
	 * writeMask All, the factors One and Zero, both operations Add (MTLRenderPipeline.h).
	 * Nothing is interpreted here. Every number goes across as Metal's own and the
	 * library maps the three enumerations by table, because a cast between them is wrong:
	 * MTLBlendFactor and pipe_blendfactor agree on two of nineteen values and
	 * MTLColorWriteMask's bits run in the opposite order to Gallium's colormask.
	 *
	 * `color_blend_count` is set to eight -- all eight entries filled in -- and NOT to
	 * the attachment count: a zero there means "this shape carries no blend state at
	 * all", which is the old opaque behaviour, and that is a thing this driver must never
	 * say by accident.
	 */
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *attachment =
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		unsigned format = attachment != nil ? ann_canonical_pixel_format((unsigned)[attachment pixelFormat]) : 0;

		shape.color_format[i] = format;
		if (format != 0) {
			shape.color_count = i + 1;
		}
		if (attachment == nil) {
			/* an array that answered nil has no defaults to pass on, so this
			 * entry says what Metal's own untouched descriptor says: no
			 * blending, every channel written */
			shape.color_blend[i].write_mask = 0xf;   /* MTLColorWriteMaskAll */
			continue;
		}
		shape.color_blend[i].blending_enabled =
		    [attachment isBlendingEnabled] ? 1 : 0;
		shape.color_blend[i].rgb_blend_operation =
		    (unsigned)[attachment rgbBlendOperation];
		shape.color_blend[i].alpha_blend_operation =
		    (unsigned)[attachment alphaBlendOperation];
		shape.color_blend[i].source_rgb_blend_factor =
		    (unsigned)[attachment sourceRGBBlendFactor];
		shape.color_blend[i].destination_rgb_blend_factor =
		    (unsigned)[attachment destinationRGBBlendFactor];
		shape.color_blend[i].source_alpha_blend_factor =
		    (unsigned)[attachment sourceAlphaBlendFactor];
		shape.color_blend[i].destination_alpha_blend_factor =
		    (unsigned)[attachment destinationAlphaBlendFactor];
		shape.color_blend[i].write_mask = (unsigned)[attachment writeMask];
	}
	shape.color_blend_count = 8;
	/*
	 * And the measurement this was built to make: what the window server actually asks
	 * for, one line per attachment it has, printed BEFORE the library can refuse any of
	 * it. These are Metal's own numbers, so `blend 1 rgb 1/5 op 0 alpha 1/5 op 0` is
	 * premultiplied source-over (One, OneMinusSourceAlpha, Add) and `blend 1 rgb 4/5` is
	 * straight alpha instead -- the one distinction a boot log has to be able to make.
	 *
	 * THE LABEL IS HERE BECAUSE IT IS THE ANSWER. CoreAnimation builds a pipeline's
	 * label out of the specification it compiled: `encode_segment` emits a `B<mnemonic>`
	 * segment IF AND ONLY IF the fixed-function blend mode is not its "none" value
	 * (0x36), so `com.apple.coreanimation.draw.PBGRABsovXm_...` is a pipeline whose
	 * blending this descriptor carries and `com.apple.coreanimation.draw.PBGRAXm_...` is
	 * one that blends INSIDE ITS FRAGMENT SHADER and leaves this descriptor at Apple's
	 * defaults. One grep over a boot log then says which of the two the home screen is
	 * made of, which no amount of reading the enumerations can.
	 */
	bool blend_is_new = false;

	for (unsigned i = 0; i < shape.color_count; i++) {
		blend_is_new |= rw_blend_line(i, shape.color_format[i],
		                              &shape.color_blend[i]);
	}
	/* and the label, on a pipeline whose blend configuration has not been seen before:
	 * one per distinct configuration is the whole of what the label has to say, and the
	 * compositor has over a hundred of them per boot */
	if (blend_is_new) {
		NSString *label = [descriptor label];

		rw_print("blend: ... and the first such pipeline's label is %s\n",
		         label != nil ? [label UTF8String] : "(none)");
	}
	/* Both aspect formats, as Metal's descriptor has them, and the library requires
	 * them to name one texture: an application with a combined format sets the same
	 * MTLPixelFormat on both, because there is one attachment behind the two
	 * properties. Passing them through separately rather than folding them here is
	 * what lets the library refuse a descriptor that names two different formats by
	 * name instead of this driver picking one of them. */
	shape.depth_format = (unsigned)[descriptor depthAttachmentPixelFormat];
	shape.stencil_format = (unsigned)[descriptor stencilAttachmentPixelFormat];

	/*
	 * THE VERTEX DESCRIPTOR, which is what the window server asked this driver for and
	 * did not get: one pipeline specification was refused 80 times with "reads vertex
	 * attributes ([[stage_in]]), and this contract carries no vertex attribute layout"
	 * (docs/metal.md 10.44, 10.45), because the layout lives here and nowhere in the AIR.
	 *
	 * Both of Metal's arrays are read whole, index by index, and passed through as
	 * Metal's own numbers -- the library decides what it can honour and refuses the
	 * rest BY NAME, which is the half this driver must not do for it: a format a6xx
	 * cannot fetch and an offset past twelve bits are both things freedreno would take
	 * silently (air_metal.h has the register fields).
	 *
	 * There is no count to read on either side. Apple's arrays answer
	 * -objectAtIndexedSubscript: for every index in range and a descriptor an
	 * application never touched holds MTLVertexFormatInvalid in every attribute, which
	 * is what the library reads as "not configured" -- so the loop is the whole array
	 * and the formats decide. The range is Apple's 31 of each, from the feature tables,
	 * and asking past it would raise.
	 */
	MTLVertexDescriptor *vd = [descriptor vertexDescriptor];

	if (vd != nil) {
		unsigned configured = 0, highest = 0;

		for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++) {
			MTLVertexAttributeDescriptor *a =
			    [[vd attributes] objectAtIndexedSubscript:i];
			unsigned format = a != nil ? (unsigned)[a format] : 0;

			if (format == 0) {
				continue;
			}
			shape.vertex_attribute[i].format = format;
			shape.vertex_attribute[i].offset = (unsigned)[a offset];
			shape.vertex_attribute[i].buffer_index = (unsigned)[a bufferIndex];
			configured++;
			if ((unsigned)[a bufferIndex] > highest) {
				highest = (unsigned)[a bufferIndex];
			}
		}
		for (unsigned k = 0; k < RWA_MAX_VERTEX_BUFFERS; k++) {
			MTLVertexBufferLayoutDescriptor *l =
			    [[vd layouts] objectAtIndexedSubscript:k];

			if (l == nil) {
				continue;
			}
			shape.vertex_layout[k].stride = (unsigned)[l stride];
			shape.vertex_layout[k].step_function = (unsigned)[l stepFunction];
			shape.vertex_layout[k].step_rate = (unsigned)[l stepRate];
		}
		if (configured > 0) {
			rw_print("this pipeline's vertex descriptor configures %u "
			         "attribute%s, the highest from buffer index %u\n", configured,
			         configured == 1 ? "" : "s", highest);
		}
	}

	char why[512] = { 0 };
	struct rwa_render_info info = { 0 };
	unsigned char key[CC_SHA256_DIGEST_LENGTH];

	/* How long a compile takes, and how many there have been: CoreAnimation builds these
	 * while it animates -- ten a second in one Control Center pull -- and if that is where
	 * a frame's time goes, the answer is a pipeline it can reuse rather than a faster one. */
	uint64_t compile_began = mach_absolute_time();
	struct rwa_linked_function linked[RW_LINKED_MAX];
	NSMutableData *linked_scratch = [NSMutableData data];
	unsigned num_linked = mesa.render_create_linked != NULL
	    ? rw_linked_fragment_functions(descriptor, linked, RW_LINKED_MAX, linked_scratch) : 0;
	const char *vertex_name = vertex != nil ? [[vertex name] UTF8String] : NULL;
	struct rwa_render *render = NULL;
	rw_pipe_key(key, vertex_air, vertex_name, fragment_air, fragment != nil ? fragment_name : NULL,
		    &shape, linked, num_linked);
	render = rw_pipe_find(_mesa, key, &info);
	bool reused = render != NULL;
	if (!reused) {
		render = mesa.render_create_linked != NULL
		    ? mesa.render_create_linked(_mesa, [vertex_air bytes], [vertex_air length], vertex_name,
			  [fragment_air bytes], [fragment_air length], fragment != nil ? fragment_name : NULL,
			  linked, num_linked, &shape, &info, why, sizeof(why))
		    : mesa.render_create(_mesa, [vertex_air bytes], [vertex_air length], vertex_name,
			  [fragment_air bytes], [fragment_air length], fragment != nil ? fragment_name : NULL,
			  &shape, &info, why, sizeof(why));
		if (render != NULL) {
			rw_pipe_keep(_mesa, key, render, &info);
		}
	}
	rw_linked_free(linked_scratch);
	{
		static unsigned long built, reuses;
		struct mach_timebase_info tb;
		uint64_t us;

		mach_timebase_info(&tb);
		us = (mach_absolute_time() - compile_began) * tb.numer / tb.denom / 1000;
		if (reused) {
			++reuses;
		} else {
			++built;
		}
		/* The entry names are here because a picture that is wrong needs to be traced back to
		 * the pipeline that drew it, and Apple's own names for the compiler library's
		 * functions are the only handle this process has on them: "air.fragment.blur" says
		 * what an address does not. Every pipeline, with its handle, which is what the first
		 * commits' trace lines name. */
		rw_print("render pipelines: %lu built, this one %llu us (%s %p) vertex %s "
		         "fragment %s; %lu reused\n", built, us,
		         reused ? "reused" : render != NULL ? "ok" : "refused", (void *)render,
		         vertex_name != NULL ? vertex_name : "(none)", fragment_name, reuses);
	}

	if (render == NULL) {
		/* Both modules and the functions linked into them go out, whichever of them the
		 * sentence names: the driver cannot tell which one the frontend gave up on. */
		if (fragment_air != nil)
			air_fail_dump([fragment_air bytes], [fragment_air length], "fragment", fragment_name);
		if (vertex_air != nil)
			air_fail_dump([vertex_air bytes], [vertex_air length], "vertex", [[vertex name] UTF8String]);
		rw_dump_linked(descriptor);
		RW_AGAIN("this render pipeline is refused: %s\n", why);
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	rw_keep_air(render, vertex_air, vertex != nil ? [[vertex name] UTF8String] : NULL, fragment_air,
		    fragment_name);
	/* and what the library made of it, in GALLIUM's bits: for any mask but All, None,
	 * green|blue and red|alpha these nibbles must DIFFER from the writeMask printed
	 * above, because the two enumerations number their channels in opposite directions.
	 * A log where 0xe went in and 0xe came back is a log saying nothing reversed them. */
	if (shape.color_count > 0) {
		rw_blend_result(info.blend_enabled_mask, info.color_write_masks);
	}
	{
		return [[AnnibaleMetalRenderPipeline alloc] initWithAnnibaleDevice:self
									  render:render
									    info:&info
								      descriptor:descriptor];
	}
}

- (id)newRenderPipelineStateWithDescriptor:(id)descriptor
				   options:(NSUInteger)options
				reflection:(id *)reflection
				     error:(NSError **)error
{
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;   /* reflection is Metal's compiler's, and ours is nil */
	}
	return [self newRenderPipelineStateWithDescriptor:descriptor error:error];
}

/*
 * The asynchronous forms, and they are not a convenience: without them Metal's own versions
 * send the pipeline request to the nil compiler, return, and NEVER CALL THE HANDLER -- so an
 * application that builds a pipeline asynchronously waits for ever instead of being told no.
 * Reversing the four entry points is what turned that up (0x181aabc48 and 0x181aabccc both
 * end in a stack block handed to the compiler), and a hang is the one failure mode that
 * cannot be read out of a log.
 *
 * Building synchronously and calling the handler at once is allowed: Metal does not promise
 * the handler runs later, only that it runs.
 */
- (void)newRenderPipelineStateWithDescriptor:(id)descriptor
			   completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newRenderPipelineStateWithDescriptor:(id)descriptor
				     options:(NSUInteger)options
			   completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;
	id reflection = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor
						     options:options
						  reflection:&reflection
						       error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

/* The inherited mesh methods convert the descriptor into a regular render
 * pipeline descriptor. Reject them until this driver implements mesh shaders.
 */
- (id)newRenderPipelineStateWithMeshDescriptor:(id)descriptor
				       options:(NSUInteger)options
				    reflection:(id *)reflection
					 error:(NSError **)error
{
	(void)descriptor; (void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	return [self fail:error with:@"mesh pipelines are not implemented, and the inherited "
				     @"path would have built this one as a vertex pipeline"];
}

- (void)newRenderPipelineStateWithMeshDescriptor:(id)descriptor
					 options:(NSUInteger)options
			       completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;

	(void)[self newRenderPipelineStateWithMeshDescriptor:descriptor options:options
						 reflection:NULL error:&error];
	if (handler != NULL) {
		handler(nil, nil, error);
	}
}

/*
 * A resource group names resources whose cache lines the GPU may drop at the end of a pass -- an
 * AGX cache-management optimisation this port has no control over. nil here is not a guess: the
 * caller tests it (cbz x0 in CA::OGL::MetalContext::mark_cache_drop_hints) and composites without
 * it, and a nil also leaves the counter at zero, which makes -encodeCacheHintTag:... and
 * -encodeCacheHintFinalize:... unreachable. One nil answers three selectors, which is the right
 * shape: refuse where the decision is made rather than at each leaf.
 */
- (id)newResourceGroupFromResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources;
	(void)count;
	return nil;
}

/*
 * Overriding this keeps _MTLDevice's own body from running on our device, and that body is what
 * sends -copyIOSurfaceSharedTextureProperties: and -sparseSurfaceDefaultValue, neither of which
 * we answer. So one override closes three. It agrees with -newSharedTextureHandle below, which
 * answers nil for the same reason: nothing here is shared between processes.
 */
- (id)newSharedTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	(void)descriptor;
	return nil;
}

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (_mesa == NULL) {
		rw_print("a texture needs the compiler library\n");
		return nil;
	}
	struct rwa_texture_shape shape = {
		.pixel_format = ann_canonical_pixel_format((unsigned)[descriptor pixelFormat]),
		.type = (unsigned)[descriptor textureType],
		.width = (unsigned)[descriptor width],
		.height = (unsigned)[descriptor height],
		.depth = (unsigned)[descriptor depth],
		.array_length = (unsigned)[descriptor arrayLength],
		.mip_levels = (unsigned)[descriptor mipmapLevelCount],
		.sample_count = (unsigned)[descriptor sampleCount],
		.usage = (unsigned)[descriptor usage],
	};
	struct rwa_texture_shape made = shape;   /* rw_one_sample */

	if (rw_one_sample() && made.sample_count > 1) {
		made.sample_count = 1;
		made.type = made.type == MTLTextureType2DMultisampleArray ? MTLTextureType2DArray : MTLTextureType2D;
	}
	struct rwa_texture *texture = mesa.texture_create(_mesa, &made, why, sizeof(why));
	if (texture == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[AnnibaleMetalTexture alloc] initWithAnnibaleDevice:self
							 texture:texture
							   shape:&shape iosurface:NULL plane:0];
}

/* The sampler's own base class exists and owes its protocol nothing, so this is the cheapest
 * object in the driver: the state goes to the compiler library, which maps it the same way it
 * maps a shader's own `constexpr sampler` word -- one vocabulary, so the two can be checked
 * against each other (docs/air.md 3.3). */
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (_mesa == NULL) {
		rw_print("a sampler needs the compiler library\n");
		return nil;
	}
	struct rwa_sampler_state state = {
		.address = { (unsigned)[descriptor sAddressMode],
			     (unsigned)[descriptor tAddressMode],
			     (unsigned)[descriptor rAddressMode] },
		.min_filter = (unsigned)[descriptor minFilter],
		.mag_filter = (unsigned)[descriptor magFilter],
		.mip_filter = (unsigned)[descriptor mipFilter],
		.compare_function = (unsigned)[descriptor compareFunction],
		.max_anisotropy = (unsigned)[descriptor maxAnisotropy],
		.normalized_coordinates = [descriptor normalizedCoordinates] ? 1 : 0,
		.lod_min = [descriptor lodMinClamp],
		.lod_max = [descriptor lodMaxClamp],
	};
	struct rwa_sampler *sampler = mesa.sampler_create(_mesa, &state, why, sizeof(why));

	if (sampler == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[AnnibaleMetalSampler alloc] initWithAnnibaleDevice:self
							 sampler:sampler
						      descriptor:descriptor];
}

/*
 * The depth/stencil state, which _MTLDevice does not implement at all: unlike -compiler,
 * whose body is `mov x0, #0; ret`, this selector is simply absent from the base class's
 * method list, so the whole of it is a driver's. (Apple's own is AGXA11FamilyDepthStencilState
 * over the same _MTLDepthStencilState this one inherits.)
 *
 * MTLDepthStencilDescriptor is read here, in Metal's words, and translated by the compiler
 * library -- which is where the two enumerations that do NOT line up are turned over:
 * MTLStencilOperationInvert is 5 and Gallium's INVERT is 7, so a cast would turn "invert the
 * stencil" into "increment it and wrap", and that is a different picture rather than a
 * failure. This side only reads properties and names them.
 */
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (descriptor == nil) {
		rw_print("a depth/stencil state needs a descriptor\n");
		return nil;
	}
	if (_mesa == NULL) {
		rw_print("a depth/stencil state needs the compiler library\n");
		return nil;
	}
	if (mesa.depth_stencil_create == NULL) {
		rw_print("this compiler library has no depth/stencil state, so it "
		         "is older than this driver and cannot test depth\n");
		return nil;
	}

	/* frontFaceStencil and backFaceStencil are null_resettable: the setter takes nil
	 * and the getter hands back Metal's default descriptor, so both are read without a
	 * nil check -- and when one is nil anyway, the zeroed face left behind is compare
	 * Always with every operation Keep, which is that same default. */
	MTLStencilDescriptor *faces[2] = { [descriptor frontFaceStencil],
					   [descriptor backFaceStencil] };
	struct rwa_depth_stencil_state state = {
		.depth_compare_function = (unsigned)[descriptor depthCompareFunction],
		.depth_write_enabled = [descriptor isDepthWriteEnabled] ? 1 : 0,
	};
	struct rwa_stencil_face *out[2] = { &state.front, &state.back };

	for (unsigned i = 0; i < 2; i++) {
		if (faces[i] == nil) {
			*out[i] = (struct rwa_stencil_face){
				.compare_function = MTLCompareFunctionAlways,
				.read_mask = 0xffffffff, .write_mask = 0xffffffff,
			};
			continue;
		}
		*out[i] = (struct rwa_stencil_face){
			.compare_function = (unsigned)[faces[i] stencilCompareFunction],
			.stencil_fail_operation = (unsigned)[faces[i] stencilFailureOperation],
			.depth_fail_operation = (unsigned)[faces[i] depthFailureOperation],
			.depth_stencil_pass_operation =
			    (unsigned)[faces[i] depthStencilPassOperation],
			.read_mask = (unsigned)[faces[i] readMask],
			.write_mask = (unsigned)[faces[i] writeMask],
		};
	}

	struct rwa_depth_stencil *made = mesa.depth_stencil_create(_mesa, &state, why,
								   sizeof(why));

	if (made == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[AnnibaleMetalDepthStencilState alloc] initWithAnnibaleDevice:self
								     state:made
								descriptor:descriptor];
}

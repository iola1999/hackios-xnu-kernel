/* Native binary archives require a backend archive reader/writer. Mesa compiles
 * AIR pipelines at runtime; this device does not implement that archive contract. */
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor
                                                error:(NSError **)error
{
    (void)descriptor;
    if (error != NULL) {
        *error = [NSError errorWithDomain:MTLBinaryArchiveDomain
            code:MTLBinaryArchiveErrorInternalError
            userInfo:@{ NSLocalizedDescriptionKey:
                @"RedwoodMetal does not support native binary archives; AIR pipelines are compiled at runtime." }];
    }
    return nil;
}

/*
 * The answer that could not be "nothing", and the first place in this driver where that is
 * true. logs/210311-run.log is backboardd dying every ten seconds on
 *
 *   signal 11 at 0x10, pc 0x181a3ca3c, called from 0x18847f6dc
 *
 * which is MTLPipelineLibraryBuilder::newLibraryWithFile + 836, reached from
 * CA::OGL::new_metal_context + 1808 -- QuartzCore asking this device for the precompiled
 * pipelines of its own default.pipelinelib. Nothing of ours is on that stack:
 * -[_MTLDevice newPipelineLibraryWithFilePath:error:] is three instructions (0x181a3d9b0,
 * `mov x1, x0; ldr x0, [x0, #0x180]; b`) that hand the device and its own builder ivar to
 * Apple's C++, which gets as far as parsing pipelines.desc out of the image and then asks us
 * one question. The three instructions that matter:
 *
 *   0x181a3c9ec  objc_opt_respondsToSelector(device, "targetDeviceInfo")   -> YES
 *   0x181a3c9f8  objc_msgSend$targetDeviceInfo                             -> NULL
 *   0x181a3ca3c  ldr x8, [x23, #0x10]                                      -> 0x10
 *
 * Both halves of that are Apple's. `_MTLDevice` HAS an entry for -targetDeviceInfo and its
 * body is `mov x0, #0; ret` (0x181aa8d98 -- tools/metal/absmeth.py calls that a nil-stub),
 * so the runtime answers the question YES on our behalf and the pointer is NULL; and the
 * caller treats a YES as a promise and dereferences what comes back. Declining is therefore
 * not on the table: returning nil IS what crashes, and the only way to make the runtime say
 * NO would be to lie in -respondsToSelector:, which would also lie to every caller that
 * does not ask first. Of the eight objc_opt_respondsToSelector sites in all of
 * Metal.framework this is the only one whose answer is a raw pointer rather than a scalar or
 * an object -- the other seven ask about -shaderDebugInfoCaching, -maximumComputeSubstreams
 * (twice), -presentWithOptions: (twice), -boolValue and one selector held in a register.
 *
 * The shape can recur, and the population is counted rather than feared: of `_MTLDevice`'s
 * 522 instance methods 74 are nil-stubs, and 14 of those are declared to return a raw
 * pointer (absmeth.py joined to protometh.py by implementation address). Thirteen of the
 * fourteen are the pipeline-cache key family -- `*FunctionKeyWith...:keySize:` and
 * `get*FunctionId:compilerOptions:compilerOptionsSize:` -- which is reached only once a
 * pipeline cache actually holds something; this is the fourteenth, and the first one
 * anything asked for.
 *
 * So the struct is real, and its contents are the ones Apple's own code writes in the branch
 * it takes for a device that does not respond (0x181a3ca04): zero, zero, and the device's
 * -name with the spaces removed. Its shape is known twice over, from two sources that agree
 * -- that branch, which builds one on its own stack, and the type MTLDeviceSPI records for
 * this method, `r^{MTLTargetDeviceArch=QI*}16@0:8`. Each field by its reader:
 *
 *   +0x00  nobody on this path reads it, and Apple's branch writes 0.
 *   +0x08  read by -[_MTLPipelineCache initWithFilePath:readOnly:deviceInfo:] (0x181a3b720),
 *          which pairs it with 1 into an MTLCompilerCache::Version for
 *          MTLCompilerCache::createPipelineDataCache. That version decides whether a cache
 *          FILE is stale; this cache is opened read-only (w3=1 at 0x181a3ca64) over a file
 *          no image here has, so there is nothing for it to invalidate, and 0 is the number
 *          Apple's own branch uses. It becomes a real question the day this driver WRITES a
 *          pipeline cache, and not before.
 *   +0x10  formatted into "%s/pipelines.%s" (0x181a3ca48) -- the per-GPU cache file inside
 *          QuartzCore.framework/default.pipelinelib. It must be a C string and not NULL:
 *          Metal's second cache, the one for `functions`, it builds with a zeroed struct and
 *          an EMPTY name itself (0x181a3ca7c), which is the measurement that says a zeroed
 *          struct is acceptable to _MTLPipelineCache.
 *
 * What this buys is a refusal with a reason instead of a crash, and the refusal is the
 * honest one: that image ships `functions` and `pipelines.desc` and no pipelines.* for any
 * GPU at all (not even the A11 this phone was born with), so the cache comes back nil,
 * Metal fills the NSError with "Could not open pipeline cache file: %s" (code 6,
 * 0x181a3cb2c), and QuartzCore logs "Metal failed to load pipeline library: %s" and carries
 * on -- 0x18847f6f0 is a `cbnz`, not an assert. Nothing is lost by it: a precompiled
 * pipeline in that cache would be A11 machine code, which this driver could not run anyway.
 * The pipelines CoreAnimation needs get compiled the ordinary way, through
 * -newRenderPipelineStateWithDescriptor: and AIR.
 */
- (const struct MTLTargetDeviceArch *)targetDeviceInfo
{
	/* The whole point of this method is that a NULL here is fatal, so it must not depend on
	 * an initialiser having run: an object built with plain -init rather than Metal's
	 * -initWithAcceleratorPort: has a zeroed ivar, and an empty name is what Apple's own
	 * second cache passes for itself anyway. The array is zeroed by the runtime, so it
	 * already spells "". */
	if (_target.name == NULL) {
		_target.name = _targetName;
	}
	RW_ONCE("Metal asked for targetDeviceInfo (pipelines.%s); it is answered "
		"with a real struct because Apple's own answer here is a NULL this driver does "
		"not get to decline\n", _target.name);
	return &_target;
}

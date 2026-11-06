/*
 * Function specialization, stitched libraries and linked functions; included into redwoodmetal.m
 * after the class declarations.
 *
 * In iOS 26 RenderBox draws IconRendering's [[stitchable]] functions (sdfFill, glow,
 * glassHighlight, ...) through a fragment shader that calls an extern visible function
 * `custom_fn`. RenderBox (StitchShader) asks a library for functions through
 * -newFunctionWithDescriptor:error: with a specializedName, makes a library of a stitching graph
 * with -newLibraryWithStitchedDescriptor:error:, and puts the result in the pipeline's
 * fragmentLinkedFunctions.privateFunctions. _MTLLibrary finds most specialized variants in the
 * library itself and sends the rest to MTLCompilerService, which is killed on this system.
 *
 * What is here so far: a specialization _MTLLibrary cannot make in process is answered with a
 * proxy over the unspecialized function that carries the requested name and constant values,
 * and every descriptor of the three steps is printed in full, so that the stitching is built on
 * what RenderBox sends.
 */

#define RW_STITCH_DUMPS 64
#define RW_STITCH_DEPTH 6

static unsigned rw_stitch_dumps;

static BOOL
rw_stitch_skip_property(const char *name)
{
	static const char *const skip[] = { "description", "debugDescription", "hash", "superclass",
					    "device", "retainCount" };

	for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
		if (strcmp(name, skip[i]) == 0) {
			return YES;
		}
	}
	return NO;
}

/* One line per object and per property: class, then NSString/NSNumber values as they are,
 * arrays and dictionaries entry by entry, functions by name only, any other object
 * recursively. */
static void
rw_stitch_dump(const char *label, id object, unsigned depth)
{
	char indent[2 * RW_STITCH_DEPTH + 1];
	unsigned n = depth < RW_STITCH_DEPTH ? depth : RW_STITCH_DEPTH;

	memset(indent, ' ', 2 * n);
	indent[2 * n] = '\0';
	if (object == nil) {
		rw_print("stitch: %s%s: nil\n", indent, label);
		return;
	}
	if ([object isKindOfClass:[NSString class]] || [object isKindOfClass:[NSNumber class]] ||
	    [object isKindOfClass:[NSValue class]] || [object isKindOfClass:[NSData class]]) {
		rw_print("stitch: %s%s: %s\n", indent, label,
			 [[[object description] substringToIndex:MIN([[object description] length],
								      (NSUInteger)160)] UTF8String]);
		return;
	}
	if ([object isKindOfClass:[NSArray class]]) {
		NSArray *array = object;

		rw_print("stitch: %s%s: %lu entries\n", indent, label, (unsigned long)[array count]);
		for (NSUInteger i = 0; i < [array count] && i < 32; i++) {
			char item[32];

			snprintf(item, sizeof(item), "[%lu]", (unsigned long)i);
			rw_stitch_dump(item, [array objectAtIndex:i], depth + 1);
		}
		return;
	}
	if ([object isKindOfClass:[NSDictionary class]]) {
		NSDictionary *dictionary = object;

		rw_print("stitch: %s%s: %lu keys\n", indent, label,
			 (unsigned long)[dictionary count]);
		for (id key in dictionary) {
			rw_stitch_dump([[key description] UTF8String], [dictionary objectForKey:key],
				       depth + 1);
		}
		return;
	}
	if ([object respondsToSelector:@selector(functionType)]) {
		rw_print("stitch: %s%s: %s function \"%s\"\n", indent, label,
			 class_getName(object_getClass(object)),
			 [[object respondsToSelector:@selector(name)] ? [object name] : @"?"
			     UTF8String]);
		return;
	}
	rw_print("stitch: %s%s: %s\n", indent, label, class_getName(object_getClass(object)));
	if (depth >= RW_STITCH_DEPTH) {
		return;
	}
	for (Class c = object_getClass(object); c != Nil && c != [NSObject class];
	     c = class_getSuperclass(c)) {
		unsigned count = 0;
		objc_property_t *properties = class_copyPropertyList(c, &count);

		for (unsigned i = 0; i < count; i++) {
			const char *name = property_getName(properties[i]);
			id value = nil;

			if (rw_stitch_skip_property(name)) {
				continue;
			}
			@try {
				value = [object valueForKey:[NSString stringWithUTF8String:name]];
			} @catch (NSException *e) {
				rw_print("stitch: %s  %s: (%s)\n", indent, name, [[e reason] UTF8String]);
				continue;
			}
			rw_stitch_dump(name, value, depth + 1);
		}
		free(properties);
	}
}

static BOOL
rw_stitch_dump_begin(const char *what)
{
	if (rw_stitch_dumps >= RW_STITCH_DUMPS) {
		return NO;
	}
	rw_stitch_dumps++;
	rw_print("stitch: begin %s (%u of at most %u)\n", what, rw_stitch_dumps, RW_STITCH_DUMPS);
	return YES;
}

/* Called from -newRenderPipelineStateWithDescriptor:error: for every descriptor that links
 * functions into either stage. RenderBox fills privateFunctions. */
static void
rw_stitch_dump_linked(id descriptor)
{
	static const char *const stages[] = { "vertexLinkedFunctions", "fragmentLinkedFunctions" };

	for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
		SEL sel = sel_getUid(stages[i]);
		id linked;

		if (![descriptor respondsToSelector:sel]) {
			continue;
		}
		linked = ((id (*)(id, SEL))objc_msgSend)(descriptor, sel);
		if (linked == nil || ([[linked functions] count] == 0 &&
				      [[linked binaryFunctions] count] == 0 &&
				      [[linked privateFunctions] count] == 0 &&
				      [[linked groups] count] == 0)) {
			continue;
		}
		if (rw_stitch_dump_begin(stages[i])) {
			rw_stitch_dump(stages[i], linked, 0);
		}
	}
}

/*
 * A function as -newFunctionWithDescriptor: or -newFunctionWithName:constantValues: asked for
 * it: the unspecialized function underneath, answering to the specialized name, with the
 * constant values kept for the translator. Everything else goes to the function underneath,
 * bitcodeData included.
 */
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>

#define RW_AIR_CACHE_DIR "/private/var/db/redwood-aircache"

@interface RedwoodMetalSpecializedFunction : NSProxy {
	id _function;
	NSString *_name;
	id _constantValues;
	NSData *_cachedBitcode;
}
- (instancetype)initWithFunction:(id)function name:(NSString *)name constantValues:(id)values;
- (instancetype)initWithFunction:(id)function name:(NSString *)name constantValues:(id)values
		  cachedBitcode:(NSData *)bitcode;
- (id)redwoodConstantValues;
- (id)redwoodUnspecializedFunction;
- (NSData *)bitcodeData;
@end

@implementation RedwoodMetalSpecializedFunction

- (instancetype)initWithFunction:(id)function name:(NSString *)name constantValues:(id)values
		  cachedBitcode:(NSData *)bitcode
{
	_function = [function retain];
	_name = [name copy];
	_constantValues = [values retain];
	_cachedBitcode = [bitcode retain];
	return self;
}

- (instancetype)initWithFunction:(id)function name:(NSString *)name constantValues:(id)values
{
	return [self initWithFunction:function name:name constantValues:values cachedBitcode:nil];
}

- (void)dealloc
{
	[_function release];
	[_name release];
	[_constantValues release];
	[_cachedBitcode release];
	[super dealloc];
}

- (NSString *)name { return _name; }
- (id)redwoodConstantValues { return _constantValues; }
- (id)redwoodUnspecializedFunction { return _cachedBitcode != nil ? self : _function; }
- (NSData *)bitcodeData
{
	if (_cachedBitcode != nil)
		return _cachedBitcode;
	SEL sel = sel_getUid("bitcodeData");
	if ([_function respondsToSelector:sel])
		return ((NSData *(*)(id, SEL))objc_msgSend)(_function, sel);
	return nil;
}
- (id)forwardingTargetForSelector:(SEL)sel
{
	if (sel == sel_getUid("bitcodeData") && _cachedBitcode != nil)
		return nil;
	return _function;
}
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	if (sel == sel_getUid("bitcodeData"))
		return [NSMethodSignature signatureWithObjCTypes:"@@:"];
	return [_function methodSignatureForSelector:sel];
}
- (void)forwardInvocation:(NSInvocation *)invocation
{
	if ([invocation selector] == sel_getUid("bitcodeData") && _cachedBitcode != nil) {
		[invocation setReturnValue:&_cachedBitcode];
		return;
	}
	[invocation invokeWithTarget:_function];
}
- (BOOL)respondsToSelector:(SEL)sel
{
	if (sel == @selector(redwoodUnspecializedFunction))
		return _cachedBitcode == nil;
	if (sel == sel_getUid("bitcodeData"))
		return _cachedBitcode != nil || [_function respondsToSelector:sel];
	return sel == @selector(redwoodConstantValues) || [_function respondsToSelector:sel];
}
- (BOOL)isKindOfClass:(Class)cls { return [_function isKindOfClass:cls]; }
- (BOOL)conformsToProtocol:(Protocol *)protocol { return [_function conformsToProtocol:protocol]; }

@end

static NSCache *rw_air_memcache;
static dispatch_once_t rw_air_cache_once;

static void
rw_air_cache_init(void)
{
	dispatch_once(&rw_air_cache_once, ^{
		rw_air_memcache = [[NSCache alloc] init];
		[rw_air_memcache setCountLimit:256];
		mkdir(RW_AIR_CACHE_DIR, 0777);
	});
}

static NSString *
rw_air_cache_key(id library, NSString *name, NSString *specialized, id constantValues)
{
	CC_SHA256_CTX ctx;
	CC_SHA256_Init(&ctx);

	NSString *lib_id = nil;
	SEL sel_inst = sel_registerName("installName");
	SEL sel_path = sel_registerName("filePath");
	SEL sel_lbl = sel_registerName("label");
	if ([library respondsToSelector:sel_inst]) {
		lib_id = ((id (*)(id, SEL))objc_msgSend)(library, sel_inst);
	} else if ([library respondsToSelector:sel_path]) {
		lib_id = ((id (*)(id, SEL))objc_msgSend)(library, sel_path);
	} else if ([library respondsToSelector:sel_lbl]) {
		lib_id = ((id (*)(id, SEL))objc_msgSend)(library, sel_lbl);
	}
	if ([lib_id length] == 0) {
		lib_id = [NSString stringWithFormat:@"%p", (void *)library];
	}
	const char *lib_str = [lib_id UTF8String];
	if (lib_str) CC_SHA256_Update(&ctx, lib_str, strlen(lib_str));

	uint8_t sep = 0;
	CC_SHA256_Update(&ctx, &sep, 1);

	const char *fn_str = [name UTF8String];
	if (fn_str) CC_SHA256_Update(&ctx, fn_str, strlen(fn_str));
	CC_SHA256_Update(&ctx, &sep, 1);

	const char *spec_str = [specialized UTF8String];
	if (spec_str) CC_SHA256_Update(&ctx, spec_str, strlen(spec_str));
	CC_SHA256_Update(&ctx, &sep, 1);

	if (constantValues != nil) {
		NSData *data = nil;
		@try {
			data = [NSKeyedArchiver archivedDataWithRootObject:constantValues
						     requiringSecureCoding:NO
								     error:nil];
		} @catch (id ex) {
			data = nil;
		}
		if (data == nil) {
			data = [[constantValues description] dataUsingEncoding:NSUTF8StringEncoding];
		}
		if (data != nil && [data length] > 0) {
			CC_SHA256_Update(&ctx, [data bytes], [data length]);
		}
	}

	unsigned char digest[CC_SHA256_DIGEST_LENGTH];
	CC_SHA256_Final(digest, &ctx);

	char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
		snprintf(&hex[i * 2], 3, "%02x", digest[i]);
	}
	hex[CC_SHA256_DIGEST_LENGTH * 2] = '\0';
	return [NSString stringWithUTF8String:hex];
}

static NSData *
rw_air_cache_lookup(NSString *key)
{
	rw_air_cache_init();
	if (key == nil)
		return nil;
	NSData *data = [rw_air_memcache objectForKey:key];
	if (data != nil)
		return data;

	NSString *path = [NSString stringWithFormat:@"%s/%@.air", RW_AIR_CACHE_DIR, key];
	data = [NSData dataWithContentsOfFile:path options:0 error:nil];
	if (data != nil && [data length] > 0) {
		[rw_air_memcache setObject:data forKey:key];
		return data;
	}
	return nil;
}

static void
rw_air_cache_store(NSString *key, NSData *bitcode)
{
	rw_air_cache_init();
	if (key == nil || bitcode == nil || [bitcode length] == 0)
		return;

	[rw_air_memcache setObject:bitcode forKey:key];

	NSString *path = [NSString stringWithFormat:@"%s/%@.air", RW_AIR_CACHE_DIR, key];
	mkdir(RW_AIR_CACHE_DIR, 0777);
	NSError *err = nil;
	[bitcode writeToFile:path options:NSDataWritingAtomic error:&err];
}

static id (*rw_library_function_with_name)(id, SEL, NSString *);
static id (*rw_library_function_with_descriptor)(id, SEL, id, NSError **);
static id (*rw_library_function_with_constants)(id, SEL, NSString *, id, NSError **);

static id
rw_specialize_by_proxy(id library, NSString *name, NSString *specialized, id values, id descriptor,
		       NSError *why, NSData *cached_bitcode)
{
	id function;
	id proxy;

	if (cached_bitcode == nil && rw_stitch_dump_begin("a specialization _MTLLibrary refused")) {
		rw_stitch_dump("descriptor", descriptor, 0);
		rw_print("stitch: _MTLLibrary said %s; the proxy runs the function with every "
			 "constant zero\n", why != nil ? [[why description] UTF8String] : "nothing");
	}
	function = name != nil ? rw_library_function_with_name(library, @selector(newFunctionWithName:),
								name) : nil;
	if (function == nil) {
		return nil;
	}
	if ([specialized length] == 0 && values == nil && cached_bitcode == nil) {
		return function;
	}
	proxy = [[RedwoodMetalSpecializedFunction alloc]
	    initWithFunction:function name:[specialized length] != 0 ? specialized : name
	      constantValues:values cachedBitcode:cached_bitcode];
	[function release];
	return proxy;
}

static id
rw_library_new_function_with_descriptor(id self, SEL _cmd, id descriptor, NSError **error)
{
	NSString *name = [descriptor name];
	NSString *specialized = [descriptor respondsToSelector:@selector(specializedName)]
	    ? [descriptor specializedName] : nil;
	id values = [descriptor respondsToSelector:@selector(constantValues)]
	    ? [descriptor constantValues] : nil;
	NSString *cache_key = rw_air_cache_key(self, name, specialized, values);
	NSError *why = nil;
	id function = rw_library_function_with_descriptor(self, _cmd, descriptor, &why);

	if (function != nil) {
		SEL bitcode = sel_getUid("bitcodeData");
		if ([function respondsToSelector:bitcode]) {
			NSData *bc = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);
			if (bc != nil && [bc length] > 0)
				rw_air_cache_store(cache_key, bc);
		}
	} else {
		NSData *cached = rw_air_cache_lookup(cache_key);
		function = rw_specialize_by_proxy(self, name, specialized, values, descriptor, why, cached);
	}
	if (function == nil && error != NULL) {
		*error = why;
	}
	return function;
}

static id
rw_library_new_function_with_constants(id self, SEL _cmd, NSString *name, id values,
				       NSError **error)
{
	NSString *cache_key = rw_air_cache_key(self, name, nil, values);
	NSError *why = nil;
	id function = rw_library_function_with_constants(self, _cmd, name, values, &why);

	if (function != nil) {
		SEL bitcode = sel_getUid("bitcodeData");
		if ([function respondsToSelector:bitcode]) {
			NSData *bc = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);
			if (bc != nil && [bc length] > 0)
				rw_air_cache_store(cache_key, bc);
		}
	} else {
		NSData *cached = rw_air_cache_lookup(cache_key);
		function = rw_specialize_by_proxy(self, name, nil, values, name, why, cached);
	}
	if (function == nil && error != NULL) {
		*error = why;
	}
	return function;
}

/* Once per process, on the first device: _MTLLibrary's two specializing calls, which send what
 * the library cannot answer itself to the compiler service. */
static void
rw_stitch_install_hooks(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		Class library = NSClassFromString(@"_MTLLibrary");
		Method named = library != Nil
		    ? class_getInstanceMethod(library, @selector(newFunctionWithName:)) : NULL;
		Method described = library != Nil
		    ? class_getInstanceMethod(library, @selector(newFunctionWithDescriptor:error:)) : NULL;
		Method constants = library != Nil
		    ? class_getInstanceMethod(library, @selector(newFunctionWithName:constantValues:error:))
		    : NULL;

		if (named == NULL || described == NULL || constants == NULL) {
			rw_print("stitch: _MTLLibrary lacks one of its function calls; "
				 "specialization stays with the compiler service\n");
			return;
		}
		rw_library_function_with_name = (id (*)(id, SEL, NSString *))method_getImplementation(named);
		rw_library_function_with_descriptor =
		    (id (*)(id, SEL, id, NSError **))method_setImplementation(
			described, (IMP)rw_library_new_function_with_descriptor);
		rw_library_function_with_constants =
		    (id (*)(id, SEL, NSString *, id, NSError **))method_setImplementation(
			constants, (IMP)rw_library_new_function_with_constants);
	});
}

/*
 * A library made of stitching graphs (-newLibraryWithStitchedDescriptor:). Nothing is compiled
 * here: each graph becomes a RedwoodMetalStitchedFunction that keeps the graph and the functions
 * its nodes call, and a pipeline that links one hands the graph to the compiler library node by
 * node (rw_linked_fragment_functions).
 *
 * The function is a proxy over the MTLFunction of the graph's output node, which answers the
 * reflection RenderBox reads after making the library (returnType, arguments); its name, type and
 * graph are its own.
 */
@interface RedwoodMetalStitchedFunction : NSProxy {
	id _output;
	NSString *_name;
	id _graph;
	NSArray *_functions;
}
- (instancetype)initWithGraph:(id)graph functions:(NSArray *)functions output:(id)output;
- (id)redwoodGraph;
- (NSArray *)redwoodGraphFunctions;
@end

@implementation RedwoodMetalStitchedFunction

- (instancetype)initWithGraph:(id)graph functions:(NSArray *)functions output:(id)output
{
	_graph = [graph retain];
	_functions = [functions retain];
	_output = [output retain];
	_name = [[graph functionName] copy];
	return self;
}

- (void)dealloc
{
	[_graph release];
	[_functions release];
	[_output release];
	[_name release];
	[super dealloc];
}

- (NSString *)name { return _name; }
- (NSUInteger)functionType { return 5; }   /* MTLFunctionTypeVisible */
- (id)redwoodGraph { return _graph; }
- (NSArray *)redwoodGraphFunctions { return _functions; }
- (id)forwardingTargetForSelector:(SEL)sel { (void)sel; return _output; }
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	return [_output methodSignatureForSelector:sel];
}
- (void)forwardInvocation:(NSInvocation *)invocation { [invocation invokeWithTarget:_output]; }
- (BOOL)respondsToSelector:(SEL)sel
{
	return sel == @selector(redwoodGraph) || sel == @selector(redwoodGraphFunctions) ||
	       [_output respondsToSelector:sel];
}
- (BOOL)isKindOfClass:(Class)cls { return [_output isKindOfClass:cls]; }
- (BOOL)conformsToProtocol:(Protocol *)protocol { return [_output conformsToProtocol:protocol]; }

@end

@interface RedwoodMetalStitchedLibrary : NSObject {
	id _device;
	NSArray *_functions;
	NSString *_label;
}
- (instancetype)initWithDevice:(id)device functions:(NSArray *)functions;
@end

@implementation RedwoodMetalStitchedLibrary

+ (void)load
{
	Protocol *p = objc_getProtocol("MTLLibrary");

	if (p != NULL && !class_conformsToProtocol(self, p)) {
		class_addProtocol(self, p);
	}
}

- (instancetype)initWithDevice:(id)device functions:(NSArray *)functions
{
	self = [super init];
	if (self != nil) {
		_device = [device retain];
		_functions = [functions retain];
	}
	return self;
}

- (void)dealloc
{
	[_device release];
	[_functions release];
	[_label release];
	[super dealloc];
}

- (id)device { return _device; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { [_label release]; _label = [label copy]; }
- (NSInteger)type { return 0; }   /* MTLLibraryTypeExecutable */
- (NSString *)installName { return nil; }
- (NSArray *)functionNames { return [_functions valueForKey:@"name"]; }
- (id)newFunctionWithName:(NSString *)name
{
	for (id function in _functions) {
		if ([[function name] isEqualToString:name]) {
			return [function retain];
		}
	}
	return nil;
}
- (id)newFunctionWithDescriptor:(id)descriptor error:(NSError **)error
{
	id function = [self newFunctionWithName:[descriptor name]];

	if (function == nil && error != NULL) {
		*error = [NSError errorWithDomain:@"RedwoodMetal" code:1 userInfo:@{
			NSLocalizedDescriptionKey: @"no stitched function of that name" }];
	}
	return function;
}
- (id)newFunctionWithName:(NSString *)name constantValues:(id)values error:(NSError **)error
{
	id function = [self newFunctionWithName:name];

	(void)values;
	if (function == nil && error != NULL) {
		*error = [NSError errorWithDomain:@"RedwoodMetal" code:1 userInfo:@{
			NSLocalizedDescriptionKey: @"no stitched function of that name" }];
	}
	return function;
}

@end

/* The function of the stitched descriptor's list that a node names, or nil. */
static id
rw_stitch_function_named(NSArray *functions, NSString *name)
{
	for (id function in functions) {
		if ([[function name] isEqualToString:name]) {
			return function;
		}
	}
	return nil;
}

#define RW_LINKED_MAX 8

/* The bitcode and module function name behind an MTLFunction of this process: the function
 * itself, or the one under a specialization proxy. */
static NSData *
rw_function_bitcode(id function, const char **module_name)
{
	SEL bitcode = sel_getUid("bitcodeData");
	id body = [function respondsToSelector:@selector(redwoodUnspecializedFunction)]
	    ? [function redwoodUnspecializedFunction] : function;

	if (module_name != NULL)
		*module_name = [[body name] UTF8String];
	return [body respondsToSelector:bitcode] ? ((NSData *(*)(id, SEL))objc_msgSend)(body, bitcode)
						 : nil;
}

/* A stitched function as rwa_render_create_linked takes it: one node per function node of the
 * graph, the output node appended when the graph's list does not hold it. Input nodes become the
 * call's argument numbers. The arrays live in `scratch`. */
static BOOL
rw_stitched_nodes(id function, struct rwa_linked_function *out, NSMutableData *scratch)
{
	id graph = [function redwoodGraph];
	NSArray *functions = [function redwoodGraphFunctions];
	NSMutableArray *list = [NSMutableArray arrayWithArray:[graph nodes]];
	id output = [graph outputNode];

	if (output != nil && [list indexOfObjectIdenticalTo:output] == NSNotFound) {
		[list addObject:output];
	}
	NSUInteger count = [list count];
	struct rwa_stitch_node *nodes = calloc(count ? count : 1, sizeof(*nodes));
	[scratch appendBytes:&nodes length:sizeof(nodes)];   /* freed by the caller */

	for (NSUInteger k = 0; k < count; k++) {
		id node = [list objectAtIndex:k];
		id callee = rw_stitch_function_named(functions, [node name]);
		const char *module_name = NULL;
		NSData *air = callee != nil ? rw_function_bitcode(callee, &module_name) : nil;
		NSArray *arguments = [node arguments];
		NSArray *after = [node respondsToSelector:@selector(controlDependencies)]
		    ? [node controlDependencies] : nil;
		int *args = calloc([arguments count] + 1, sizeof(int));
		unsigned *deps = calloc([after count] + 1, sizeof(unsigned));

		[scratch appendBytes:&args length:sizeof(args)];
		[scratch appendBytes:&deps length:sizeof(deps)];
		if ([air length] == 0) {
			rw_print("stitched function %s: node %s has no function with bitcode\n",
				 [[function name] UTF8String], [[node name] UTF8String]);
			return NO;
		}
		for (NSUInteger a = 0; a < [arguments count]; a++) {
			id from = [arguments objectAtIndex:a];
			NSUInteger at = [list indexOfObjectIdenticalTo:from];

			if ([from respondsToSelector:@selector(argumentIndex)]) {
				args[a] = (int)[from argumentIndex];
			} else if (at != NSNotFound) {
				args[a] = -1 - (int)at;
			} else {
				rw_print("stitched function %s: argument %lu of node %s is a %s\n",
					 [[function name] UTF8String], (unsigned long)a,
					 [[node name] UTF8String], class_getName(object_getClass(from)));
				return NO;
			}
		}
		for (NSUInteger d = 0; d < [after count]; d++) {
			NSUInteger at = [list indexOfObjectIdenticalTo:[after objectAtIndex:d]];

			deps[d] = at != NSNotFound ? (unsigned)at : (unsigned)count;
		}
		nodes[k] = (struct rwa_stitch_node){
			.bitcode = [air bytes], .size = [air length], .function = module_name,
			.num_args = (unsigned)[arguments count], .args = args,
			.num_after = (unsigned)[after count], .after = deps,
		};
	}
	*out = (struct rwa_linked_function){
		.name = [[function name] UTF8String], .num_nodes = (unsigned)count, .nodes = nodes,
		.output = output != nil ? (unsigned)[list indexOfObjectIdenticalTo:output] : 0,
	};
	return YES;
}

/* The functions a descriptor links into the fragment stage, as rwa_render_create_linked takes
 * them. A visible function is one node taking the call's own arguments; a stitched function is
 * its graph. A function this cannot describe is left out, and the pipeline then refuses the call
 * it would have answered. The node arrays are recorded in `scratch` for rw_linked_free. */
static unsigned
rw_linked_fragment_functions(id descriptor, struct rwa_linked_function *out, unsigned max,
			     NSMutableData *scratch)
{
	id linked = [descriptor respondsToSelector:@selector(fragmentLinkedFunctions)]
	    ? [descriptor fragmentLinkedFunctions] : nil;
	NSMutableArray *all = [NSMutableArray array];
	unsigned n = 0;

	if ([linked functions] != nil) {
		[all addObjectsFromArray:[linked functions]];
	}
	if ([linked privateFunctions] != nil) {
		[all addObjectsFromArray:[linked privateFunctions]];
	}
	for (id function in all) {
		if (n >= max) {
			rw_print("linked functions: more than %u; the rest are left out\n", max);
			break;
		}
		if ([function respondsToSelector:@selector(redwoodGraph)]) {
			n += rw_stitched_nodes(function, &out[n], scratch) ? 1 : 0;
			continue;
		}
		const char *module_name = NULL;
		NSData *air = rw_function_bitcode(function, &module_name);
		if ([air length] == 0) {
			rw_print("linked function %s (%s) has no bitcode\n", [[function name] UTF8String],
				 class_getName(object_getClass(function)));
			continue;
		}
		struct rwa_stitch_node *node = calloc(1, sizeof(*node));
		[scratch appendBytes:&node length:sizeof(node)];
		*node = (struct rwa_stitch_node){
			.bitcode = [air bytes], .size = [air length], .function = module_name,
		};
		out[n++] = (struct rwa_linked_function){
			.name = [[function name] UTF8String], .num_nodes = 1, .nodes = node,
		};
	}
	return n;
}

/* The modules of the functions a refused pipeline links into its fragment stage, as
 * air_fail_dump prints them ("linked" in tools/mesa/air/airfail-extract.py): a stitched
 * fragment calls them, and the instruction the frontend refuses can be in one of them. */
static void
rw_dump_linked(id descriptor)
{
	id linked = [descriptor respondsToSelector:@selector(fragmentLinkedFunctions)]
	    ? [descriptor fragmentLinkedFunctions] : nil;
	NSMutableArray *all = [NSMutableArray array];

	if ([linked functions] != nil)
		[all addObjectsFromArray:[linked functions]];
	if ([linked privateFunctions] != nil)
		[all addObjectsFromArray:[linked privateFunctions]];
	for (id function in all) {
		const char *module_name = NULL;
		NSData *air = [function respondsToSelector:@selector(redwoodGraph)]
		    ? nil : rw_function_bitcode(function, &module_name);

		if ([air length] > 0)
			air_fail_dump([air bytes], [air length], "linked", [[function name] UTF8String]);
	}
}

/* Frees what rw_linked_fragment_functions allocated. */
static void
rw_linked_free(NSMutableData *scratch)
{
	void *const *blocks = [scratch bytes];

	for (NSUInteger i = 0; i < [scratch length] / sizeof(void *); i++) {
		free(blocks[i]);
	}
}

@implementation RedwoodMetalDevice (RedwoodStitch)

/* The stitched library itself: one RedwoodMetalStitchedFunction per graph. Refused, with the
 * graph printed, when a node names a function the descriptor does not hold. */
- (id)newLibraryWithStitchedDescriptor:(id)descriptor error:(NSError **)error
{
	NSArray *functions = [descriptor functions];
	NSMutableArray *made = [NSMutableArray array];

	if (rw_stitch_dump_begin("newLibraryWithStitchedDescriptor:error:")) {
		rw_stitch_dump("descriptor", descriptor, 0);
	}
	for (id graph in [descriptor functionGraphs]) {
		id output = rw_stitch_function_named(functions, [[graph outputNode] name]);

		if (output == nil) {
			NSString *why = [NSString stringWithFormat:@"stitched function %@: the output "
					 @"node calls %@, which the descriptor's functions do not hold",
					 [graph functionName], [[graph outputNode] name]];
			rw_print("stitch: %s\n", [why UTF8String]);
			if (error != NULL) {
				*error = [NSError errorWithDomain:@"RedwoodMetal" code:2
							 userInfo:@{ NSLocalizedDescriptionKey: why }];
			}
			return nil;
		}
		id function = [[RedwoodMetalStitchedFunction alloc] initWithGraph:graph
									 functions:functions
									    output:output];
		[made addObject:function];
		[function release];
	}
	rw_print("stitch: a library of %lu stitched function(s), the first %s\n",
		 (unsigned long)[made count],
		 [made count] ? [[[made objectAtIndex:0] name] UTF8String] : "-");
	return [[RedwoodMetalStitchedLibrary alloc] initWithDevice:self functions:made];
}

- (id)newLibraryWithStitchedDescriptorSPI:(id)descriptor error:(NSError **)error
{
	return [self newLibraryWithStitchedDescriptor:descriptor error:error];
}

- (void)newLibraryWithStitchedDescriptor:(id)descriptor
		       completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id library = [self newLibraryWithStitchedDescriptor:descriptor error:&error];

	handler(library, error);
	[library release];
}

@end

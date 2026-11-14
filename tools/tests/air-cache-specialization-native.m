#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/stat.h>

static const char *cache_directory;
static const char *config_mode;
static bool statistics, internal_build;
static unsigned config_lookups, buffer_frees, checks;
static atomic_uint live_functions, named_calls, descriptor_calls, constants_calls;
static atomic_uint concurrent_errors;

static bool fixture_statistics(void) { return statistics; }
static bool fixture_internal(void) { return internal_build; }
static void *config_lookup(void *handle, const char *name)
{
    if (strcmp(name, "_MTLCompilePerformanceStatisticsEnabled") == 0) {
        config_lookups++;
        return strcmp(config_mode, "missing-stats") == 0 ? NULL : (void *)fixture_statistics;
    }
    if (strcmp(name, "_MTLIsInternalBuild") == 0) {
        config_lookups++;
        return strcmp(config_mode, "missing-internal") == 0 ? NULL : (void *)fixture_internal;
    }
    return dlsym(handle, name);
}
static void key_free(void *bytes)
{
    if (bytes != NULL) buffer_frees++;
    free(bytes);
}
static BOOL rw_stitch_dump_begin(const char *label) { (void)label; return NO; }
static void rw_stitch_dump(const char *label, id object, unsigned depth)
{ (void)label; (void)object; (void)depth; }
static void fixture_print(const char *format, ...) { (void)format; }
#define rw_print(...) fixture_print(__VA_ARGS__)
#define RW_AIR_CACHE_DIR cache_directory
#define dlsym config_lookup
#define free key_free
#include "air-cache-specialization-production.inc"
#undef free
#undef dlsym

static void check(BOOL condition, const char *message)
{
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
    checks++;
}

static BOOL different(NSString *left, NSString *right)
{
    return left.length == 64 && right.length == 64 && ![left isEqualToString:right];
}

@interface CacheFunction : NSObject { uint8_t _hash[32]; atomic_uint _reads; }
@property BOOL missingHash;
@property BOOL throwBitcode;
@property(retain) NSData *bytes;
- (unsigned)reads;
@end
@implementation CacheFunction
- (instancetype)init
{
    self = [super init];
    if (self) { memset(_hash, 0x44, sizeof(_hash)); atomic_fetch_add(&live_functions, 1); }
    return self;
}
- (const uint8_t *)bitCodeHash
{
    static const uint8_t empty[32];
    return _missingHash ? empty : _hash;
}
- (id)device { return nil; }
- (NSString *)name { return @"kernel"; }
- (NSData *)bitcodeData
{
    atomic_fetch_add(&_reads, 1);
    if (_throwBitcode) [NSException raise:@"BitcodeRead" format:@"fixture"];
    return _bytes;
}
- (unsigned)reads { return atomic_load(&_reads); }
- (void)dealloc
{ [_bytes release]; atomic_fetch_sub(&live_functions, 1); [super dealloc]; }
@end

@interface SerializedValues : NSObject <NSCopying>
@property NSUInteger failure;
@end
@implementation SerializedValues
- (id)copyWithZone:(NSZone *)zone
{
    SerializedValues *copy = [[SerializedValues allocWithZone:zone] init];
    copy.failure = _failure;
    return copy;
}
- (void *)serializedConstantDataForFunction:(id)function dataSize:(NSUInteger *)size
                              errorMessage:(NSString **)error
{
    (void)function;
    if (_failure == 3) [NSException raise:@"Serialize" format:@"fixture"];
    if (_failure == 1) { *error = @"fixture failure"; return NULL; }
    uint32_t *bytes = malloc(sizeof(*bytes));
    check(bytes != NULL, "cannot allocate serializer fixture");
    *bytes = 0;
    *size = _failure == 4 ? 2 : _failure == 5 ? (NSUInteger)UINT32_MAX + 1 : sizeof(*bytes);
    if (_failure == 2) *error = @"fixture failure with buffer";
    return bytes;
}
@end

@interface HookLibrary : NSObject
@property(nonatomic, retain) CacheFunction *base;
@property(nonatomic, retain) CacheFunction *nativeFunction;
@property(nonatomic, retain) NSError *failure;
@property BOOL throwNative;
@property BOOL throwBase;
@end
@implementation HookLibrary
- (void)dealloc
{ [_base release]; [_nativeFunction release]; [_failure release]; [super dealloc]; }
@end

static id named(id object, SEL selector, NSString *name)
{
    (void)selector; (void)name;
    atomic_fetch_add(&named_calls, 1);
    HookLibrary *library = object;
    if (library.throwBase) [NSException raise:@"BaseLookup" format:@"fixture"];
    return [library.base retain];
}
static id descriptor(id object, SEL selector, id value, NSError **error)
{
    (void)selector; (void)value;
    atomic_fetch_add(&descriptor_calls, 1);
    HookLibrary *library = object;
    if (library.throwNative) [NSException raise:@"NativeSpecialization" format:@"fixture"];
    if (error != NULL) *error = library.failure;
    return [library.nativeFunction retain];
}
static id constants(id object, SEL selector, NSString *name, id value, NSError **error)
{
    (void)selector; (void)name; (void)value;
    atomic_fetch_add(&constants_calls, 1);
    HookLibrary *library = object;
    if (library.throwNative) [NSException raise:@"NativeSpecialization" format:@"fixture"];
    if (error != NULL) *error = library.failure;
    return [library.nativeFunction retain];
}
static void reset_calls(void)
{
    atomic_store(&named_calls, 0);
    atomic_store(&descriptor_calls, 0);
    atomic_store(&constants_calls, 0);
}
static id invoke_request(HookLibrary *library, MTLFunctionDescriptor *request,
                         NSString *name, id values, NSError **error)
{
    if (request != nil)
        return rw_library_new_function_with_descriptor(library,
            @selector(newFunctionWithDescriptor:error:), request, error);
    return rw_library_new_function_with_constants(library,
        @selector(newFunctionWithName:constantValues:error:), name, values, error);
}
static id invoke(HookLibrary *library, BOOL useDescriptor, NSString *name, id values, NSError **error)
{
    MTLFunctionDescriptor *request = nil;
    if (useDescriptor) {
        request = [[[MTLFunctionDescriptor alloc] init] autorelease];
        request.name = name;
        request.constantValues = values;
    }
    return invoke_request(library, request, name, values, error);
}
static void call_counts(BOOL useDescriptor)
{
    check(atomic_load(&named_calls) == 1, "base lookup count changed");
    check(atomic_load(&descriptor_calls) == (unsigned)useDescriptor &&
          atomic_load(&constants_calls) == (unsigned)!useDescriptor, "native specialization count changed");
}

@interface CopyFailure : NSString
@end
@implementation CopyFailure
- (NSUInteger)length { return 1; }
- (unichar)characterAtIndex:(NSUInteger)index { (void)index; return 'k'; }
- (id)copyWithZone:(NSZone *)zone
{ (void)zone; [NSException raise:@"NameCopy" format:@"fixture"]; return nil; }
@end

static void config_cases(CacheFunction *base)
{
    unsetenv("MTL_REPLACE_FAST_MATH_WITH_RELAXED_MATH");
    unsetenv("MTL_DISABLE_MULTITHREADED_COMPILATION");
    NSString *initial = rw_air_cache_key(base, @"kernel", nil, nil, nil);
    check(initial != nil, "fixture key unavailable");
    statistics = true;
    check(different(rw_air_cache_key(base, @"kernel", nil, nil, nil), initial),
          "native statistics setting was lost");
    statistics = false;
    setenv("MTL_REPLACE_FAST_MATH_WITH_RELAXED_MATH", "1", 1);
    check([rw_air_cache_key(base, @"kernel", nil, nil, nil) isEqualToString:initial],
          "external build reads internal fast-math setting");
    internal_build = true;
    NSString *relaxed = rw_air_cache_key(base, @"kernel", nil, nil, nil);
    unsetenv("MTL_REPLACE_FAST_MATH_WITH_RELAXED_MATH");
    check(different(rw_air_cache_key(base, @"kernel", nil, nil, nil), relaxed),
          "internal fast-math setting was lost");
    internal_build = false;
    setenv("MTL_DISABLE_MULTITHREADED_COMPILATION", "0", 1);
    NSString *single = rw_air_cache_key(base, @"kernel", nil, nil, nil);
    setenv("MTL_DISABLE_MULTITHREADED_COMPILATION", "1", 1);
    check(different(rw_air_cache_key(base, @"kernel", nil, nil, nil), single),
          "native thread setting was lost");
    unsetenv("MTL_DISABLE_MULTITHREADED_COMPILATION");
    check(config_lookups == 2, "configuration exports were resolved repeatedly");
}

static void compatibility_cases(HookLibrary *library, CacheFunction *base,
                                CacheFunction *native, NSError *sentinel, BOOL keyAvailable)
{
    NSData *legacyBytes = [@"legacy specialized AIR" dataUsingEncoding:NSUTF8StringEncoding];
    check(![legacyBytes isEqualToData:native.bytes], "legacy and native fixture bytes must differ");
    for (unsigned route = 0; route < 2; route++) {
        BOOL useDescriptor = route != 0;
        NSString *name = [NSString stringWithFormat:@"legacy_%u_%@", route, [NSUUID UUID].UUIDString];
        SerializedValues *values = [[[SerializedValues alloc] init] autorelease];
        MTLFunctionDescriptor *request = useDescriptor
            ? [[[MTLFunctionDescriptor alloc] init] autorelease] : nil;
        request.name = name; request.constantValues = (id)values;
        /* Native descriptors copy constants; the old key uses that copy's identity. */
        id legacyValues = useDescriptor ? request.constantValues : values;
        NSString *legacyKey = rw_air_cache_legacy_key(library, name, nil, legacyValues);
        NSString *v2Key = rw_air_cache_key(base, name, nil, legacyValues, request);
        check(legacyKey.length == 64 && (keyAvailable ? different(legacyKey, v2Key) : v2Key == nil),
              "compatibility fixture cache identities unavailable");
        NSString *legacyPath = [NSString stringWithFormat:@"%s/%@.air", cache_directory, legacyKey];
        NSString *v2Path = v2Key != nil
            ? [NSString stringWithFormat:@"%s/%@.air", cache_directory, v2Key] : nil;
        check(mkdir(cache_directory, 0777) == 0 || errno == EEXIST, "cannot create compatibility cache");
        check([legacyBytes writeToFile:legacyPath atomically:NO], "cannot prepare existing legacy AIR");
        check(v2Path == nil || ![[NSFileManager defaultManager] fileExistsAtPath:v2Path],
              "compatibility fixture unexpectedly has v2 AIR");

        library.nativeFunction = nil;
        [rw_air_memcache removeAllObjects];
        NSUInteger heldBase = [base retainCount];
        unsigned reads = base.reads;
        NSError *error = sentinel;
        reset_calls();
        id result = invoke_request(library, request, name, values, &error);
        check(result != nil && result != base && error == sentinel, "legacy hit changed return or caller error");
        check([[result bitcodeData] isEqualToData:legacyBytes] && base.reads == reads,
              "legacy hit lost specialized AIR or read base bytes");
        check([base retainCount] == heldBase + 1, "legacy proxy did not retain base");
        [result release]; call_counts(useDescriptor);
        check([base retainCount] == heldBase, "legacy proxy retained base after release");
        check([[rw_air_memcache objectForKey:legacyKey] isEqualToData:legacyBytes],
              "legacy hit was not cached under its existing identity");
        check(v2Key == nil || [rw_air_memcache objectForKey:v2Key] == nil,
              "legacy hit populated v2 memory cache");
        check(v2Path == nil || ![[NSFileManager defaultManager] fileExistsAtPath:v2Path],
              "legacy hit created v2 AIR without native success");

        base.missingHash = YES;
        [rw_air_memcache removeAllObjects];
        reset_calls(); result = invoke_request(library, request, name, values, &error);
        check(result != nil && [[result bitcodeData] isEqualToData:legacyBytes] && error == sentinel,
              "nil v2 key lost existing legacy AIR");
        [result release]; call_counts(useDescriptor);
        check([base retainCount] == heldBase && base.reads == reads, "nil-key legacy hit changed base ownership");
        base.missingHash = NO;

        library.nativeFunction = native;
        NSUInteger heldNative = [native retainCount];
        unsigned nativeReads = native.reads;
        reset_calls(); result = invoke_request(library, request, name, values, &error);
        check(result == native && error == sentinel && [native retainCount] == heldNative + 1,
              "native success with legacy AIR changed return ownership or caller error");
        [result release]; call_counts(useDescriptor);
        check([base retainCount] == heldBase && [native retainCount] == heldNative,
              "native success with legacy AIR leaked function ownership");
        check([[NSData dataWithContentsOfFile:legacyPath] isEqualToData:legacyBytes],
              "native success overwrote existing legacy AIR");
        if (keyAvailable) {
            check([[NSData dataWithContentsOfFile:v2Path] isEqualToData:native.bytes],
                  "native success did not store v2 AIR");
            library.nativeFunction = nil;
            [rw_air_memcache removeAllObjects];
            reset_calls(); result = invoke_request(library, request, name, values, &error);
            check(result != nil && [[result bitcodeData] isEqualToData:native.bytes] && error == sentinel,
                  "native failure did not prefer v2 AIR over legacy AIR");
            [result release]; call_counts(useDescriptor);
            check([base retainCount] == heldBase && [rw_air_memcache objectForKey:legacyKey] == nil,
                  "v2 hit changed base ownership or read legacy AIR");

            check([[NSData data] writeToFile:v2Path atomically:NO], "cannot prepare empty v2 AIR");
            [rw_air_memcache removeAllObjects];
            reset_calls(); result = invoke_request(library, request, name, values, &error);
            check(result != nil && [[result bitcodeData] isEqualToData:legacyBytes] && error == sentinel,
                  "empty v2 AIR did not permit a legacy hit");
            [result release]; call_counts(useDescriptor);
            check([base retainCount] == heldBase && [rw_air_memcache objectForKey:v2Key] == nil,
                  "empty-v2 legacy hit changed base ownership or populated v2 memory cache");
            check([[NSData dataWithContentsOfFile:v2Path] length] == 0,
                  "legacy hit promoted existing bytes into empty v2 AIR");
        } else {
            check(native.reads == nativeReads, "missing configuration read native bytes with legacy AIR");
        }
        library.nativeFunction = native;
    }
}

int main(int argc, char **argv)
{
    check(argc == 3, "cache directory and configuration mode required");
    cache_directory = argv[1]; config_mode = argv[2];
    rw_library_function_with_name = named;
    rw_library_function_with_descriptor = descriptor;
    rw_library_function_with_constants = constants;
    @autoreleasepool {
        CacheFunction *base = [[[CacheFunction alloc] init] autorelease];
        CacheFunction *native = [[[CacheFunction alloc] init] autorelease];
        native.bytes = [@"native specialized AIR" dataUsingEncoding:NSUTF8StringEncoding];
        HookLibrary *library = [[[HookLibrary alloc] init] autorelease];
        library.base = base; library.nativeFunction = native;
        library.failure = [NSError errorWithDomain:@"fixture" code:17 userInfo:nil];
        NSError *sentinel = [NSError errorWithDomain:@"caller" code:9 userInfo:nil];
        BOOL unavailable = strcmp(config_mode, "fixture") != 0;
        if (unavailable)
            check(rw_air_cache_key(base, @"kernel", nil, nil, nil) == nil, "missing export generated a key");
        else
            config_cases(base);
        for (unsigned route = 0; route < 2; route++) {
            BOOL useDescriptor = route != 0;
            NSError *error = sentinel;
            NSUInteger heldBase = [base retainCount], heldNative = [native retainCount];
            reset_calls();
            id result = invoke(library, useDescriptor, @"kernel", nil, &error);
            check(result == native && error == sentinel, "cache changed native success or caller error");
            call_counts(useDescriptor);
            check([base retainCount] == heldBase && [native retainCount] == heldNative + 1,
                  "native return ownership changed");
            [result release];
            check([native retainCount] == heldNative, "native return leaked");
            if (unavailable) continue;

            library.nativeFunction = base;
            heldBase = [base retainCount];
            reset_calls(); result = invoke(library, useDescriptor, @"kernel", nil, &error);
            check(result == base && [base retainCount] == heldBase + 1,
                  "shared native/base object ownership changed");
            [result release]; call_counts(useDescriptor);
            check([base retainCount] == heldBase, "shared native/base return leaked");
            library.nativeFunction = native;
            heldBase = [base retainCount];

            unsigned reads = native.reads;
            base.missingHash = YES;
            reset_calls(); result = invoke(library, useDescriptor, @"kernel", nil, &error);
            check(result == native && native.reads == reads, "rejected key read native bitcode");
            [result release]; call_counts(useDescriptor); base.missingHash = NO;

            for (NSUInteger failure = 1; failure <= 5; failure++) {
                SerializedValues *values = [[[SerializedValues alloc] init] autorelease];
                values.failure = failure;
                unsigned freed = buffer_frees;
                reset_calls(); result = invoke(library, useDescriptor, @"kernel", values, &error);
                check(result == native && native.reads == reads, "serializer failure changed native result");
                if (failure == 2 || failure >= 4)
                    check(buffer_frees == freed + 1, "rejected serializer buffer was not released");
                [result release]; call_counts(useDescriptor);
            }

            library.throwBase = YES;
            reset_calls(); result = invoke(library, useDescriptor, @"kernel", nil, &error);
            check(result == native && error == sentinel, "base exception changed native result");
            [result release]; call_counts(useDescriptor); library.throwBase = NO;
            native.throwBitcode = YES;
            reset_calls(); result = invoke(library, useDescriptor, @"kernel", nil, &error);
            check(result == native && [base retainCount] == heldBase,
                  "bitcode exception lost native result or retained base");
            [result release]; call_counts(useDescriptor); native.throwBitcode = NO;

            library.throwNative = YES;
            reset_calls(); BOOL propagated = NO;
            @try { result = invoke(library, useDescriptor, @"kernel", nil, &error); [result release]; }
            @catch (NSException *exception) { propagated = [exception.name isEqualToString:@"NativeSpecialization"]; }
            check(propagated && atomic_load(&named_calls) == 0, "native exception entered cache handling");
            library.throwNative = NO;

            SerializedValues *values = [[[SerializedValues alloc] init] autorelease];
            reset_calls(); result = invoke(library, useDescriptor, @"kernel", values, &error);
            check(result == native, "valid serialized constants lost native result");
            [result release]; call_counts(useDescriptor);
            library.nativeFunction = nil;
            [rw_air_memcache removeAllObjects];
            heldBase = [base retainCount]; reads = base.reads;
            reset_calls(); result = invoke(library, useDescriptor, @"kernel", values, &error);
            check(result != nil, "cached specialization unavailable");
            check([[result bitcodeData] isEqualToData:native.bytes] && base.reads == reads,
                  "cached proxy read base bitcode or changed bytes");
            check([base retainCount] == heldBase + 1, "cached proxy did not retain base");
            [result release]; call_counts(useDescriptor);
            check([base retainCount] == heldBase, "cached proxy retained base after release");

            values.failure = 1;
            reset_calls(); result = invoke(library, useDescriptor, @"kernel", values, &error);
            check(result != nil && [result annibaleUnspecializedFunction] == base,
                  "uncached fallback lost base function");
            [result release]; call_counts(useDescriptor);
            check([base retainCount] == heldBase, "uncached proxy leaked base");

            base.missingHash = YES;
            reset_calls(); result = invoke(library, useDescriptor, @"kernel", nil, &error);
            check(result == base && [base retainCount] == heldBase + 1, "direct fallback ownership changed");
            [result release]; call_counts(useDescriptor); base.missingHash = NO;
            library.base = nil;
            error = sentinel;
            reset_calls(); result = invoke(library, useDescriptor, @"kernel", nil, &error);
            check(result == nil && error == library.failure, "missing base lost native failure");
            call_counts(useDescriptor);
            library.base = base; library.nativeFunction = native;
        }
        if (unavailable) {
            check(native.reads == 0, "missing configuration read native bitcode");
            check(![[NSFileManager defaultManager] fileExistsAtPath:
                [NSString stringWithUTF8String:cache_directory]], "missing key created a cache directory");
        } else {
            NSUInteger heldBase = [base retainCount];
            CopyFailure *badName = [[[CopyFailure alloc] init] autorelease];
            BOOL thrown = NO;
            @try {
                id proxy = [[AnnibaleMetalSpecializedFunction alloc]
                    initWithFunction:base name:badName constantValues:nil cachedBitcode:nil];
                [proxy release];
            } @catch (NSException *exception) { thrown = [exception.name isEqualToString:@"NameCopy"]; }
            check(thrown && [base retainCount] == heldBase, "partial proxy initialization retained base");
            NSUInteger heldNative = [native retainCount];
            reset_calls();
            dispatch_apply(8, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t index) {
                (void)index;
                @autoreleasepool {
                    for (unsigned j = 0; j < 20; j++) {
                        NSError *error = sentinel;
                        id result = invoke(library, NO, @"kernel", nil, &error);
                        if (result != native || error != sentinel) atomic_fetch_add(&concurrent_errors, 1);
                        [result release];
                    }
                }
            });
            check(atomic_load(&concurrent_errors) == 0 && atomic_load(&named_calls) == 160 &&
                  atomic_load(&constants_calls) == 160 && atomic_load(&descriptor_calls) == 0,
                  "concurrent hook call counts changed");
            check([base retainCount] == heldBase && [native retainCount] == heldNative,
                  "concurrent hooks changed ownership");
        }
        compatibility_cases(library, base, native, sentinel, !unavailable);
        [rw_air_memcache release]; rw_air_memcache = nil;
    }
    check(atomic_load(&live_functions) == 0, "function lifetime did not end with callers");
    NSDictionary *result = @{@"status": @"passed", @"checks": @(checks),
        @"configuration": [NSString stringWithUTF8String:config_mode], @"configuration_lookups": @(config_lookups),
        @"boundary": @"Production hooks, Foundation cache and native descriptor objects; saved IMPs, functions and configuration exports are fixtures. Configuration mutation checks cover digest inputs only. Device and native runtime environment mutation remain untested."};
    NSData *json = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
    fwrite(json.bytes, 1, json.length, stdout); fputc('\n', stdout);
    return 0;
}

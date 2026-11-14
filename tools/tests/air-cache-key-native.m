#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "air-cache-key-production.inc"

static unsigned checks;
static void check(BOOL result, const char *message)
{
    if (!result) { fprintf(stderr, "%s\n", message); exit(1); }
    checks++;
}

@interface ConstantMetadata : NSObject
@property(copy) NSString *name;
@property NSUInteger index;
@property MTLDataType type;
@end
@implementation ConstantMetadata
- (BOOL)required { return NO; }
- (void)dealloc { [_name release]; [super dealloc]; }
@end

@interface VectorFunction : NSObject {
    uint8_t _hash[32];
}
@property(retain) NSArray *functionConstants;
@property(retain) id compileDevice;
@property NSUInteger hashMode;
@end
@implementation VectorFunction
- (instancetype)init
{
    self = [super init];
    if (self) memset(_hash, 0x55, sizeof(_hash));
    return self;
}
- (const void *)bitCodeHash
{
    static const uint8_t zero[32];
    if (_hashMode == 1) return NULL;
    if (_hashMode == 2) return zero;
    if (_hashMode == 3) [NSException raise:@"HashRead" format:@"fixture"];
    return _hash;
}
- (NSString *)name { return @"vector_fn"; }
- (id)device { return _compileDevice; }
- (void)dealloc { [_functionConstants release]; [_compileDevice release]; [super dealloc]; }
@end

@interface KeyCompiler : NSObject
@property NSUInteger compilerFlags;
@end
@implementation KeyCompiler
@end
@interface KeyDevice : NSObject
@property(retain) id compiler;
@end
@implementation KeyDevice
- (void)dealloc { [_compiler release]; [super dealloc]; }
@end
@interface UncheckedDescriptor : MTLFunctionDescriptor
@end
@implementation UncheckedDescriptor
@end

static void scalar(id object, const char *selector, NSUInteger value)
{
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(object, sel_getUid(selector), value);
}
static void objectValue(id object, const char *selector, id value)
{
    ((void (*)(id, SEL, id))objc_msgSend)(object, sel_getUid(selector), value);
}
static atomic_uint concurrent_errors;

static MTLFunctionConstantValues *values(BOOL named, BOOL reverse, uint32_t k, float f)
{
    MTLFunctionConstantValues *result = [[[MTLFunctionConstantValues alloc] init] autorelease];
    if (named) {
        [result setConstantValue:&k type:MTLDataTypeUInt withName:@"k"];
        [result setConstantValue:&f type:MTLDataTypeFloat withName:@"f"];
    } else {
        if (reverse) [result setConstantValue:&f type:MTLDataTypeFloat atIndex:7];
        [result setConstantValue:&k type:MTLDataTypeUInt atIndex:3];
        if (!reverse) [result setConstantValue:&f type:MTLDataTypeFloat atIndex:7];
    }
    return result;
}

static NSString *key(id function, id constants)
{
    return rw_air_cache_key(function, @"constant_kernel", @"specialized_kernel", constants, nil);
}

static BOOL different(NSString *left, NSString *right)
{
    return left.length == 64 && right.length == 64 && ![left isEqualToString:right];
}

static NSData *serialize(id constants, id function)
{
    NSUInteger size = 0;
    NSString *error = nil;
    void *bytes = ((void *(*)(id, SEL, id, NSUInteger *, NSString **))objc_msgSend)
        (constants, sel_getUid("serializedConstantDataForFunction:dataSize:errorMessage:"),
         function, &size, &error);
    check(bytes != NULL && error == nil, "vector serializer failed");
    NSData *result = [NSData dataWithBytes:bytes length:size];
    free(bytes);
    return result;
}

int main(void)
{
    @autoreleasepool {
        NSError *error = nil;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "constant uint k [[function_constant(3)]];\n"
            "constant float f [[function_constant(7)]];\n"
            "kernel void constant_kernel(device uint *out [[buffer(0)]]) {\n"
            "uint v=0; if (is_function_constant_defined(k)) v+=k;\n"
            "if (is_function_constant_defined(f)) v+=uint(f); out[0]=v; }\n";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        check(library != nil, "native library unavailable");
        id<MTLFunction> function = [library newFunctionWithName:@"constant_kernel"];
        check(function != nil, "native function unavailable");
        NSMutableSet *keys = [NSMutableSet set];
        NSMutableSet *descriptions = [NSMutableSet set];
        NSMutableArray *instances = [NSMutableArray array];
        for (unsigned i = 0; i < 8; i++) {
            id constants = values(NO, NO, 7, 1.25f);
            [instances addObject:constants];
            NSString *value = key(function, constants);
            check(value != nil && value.length == 64, "key unavailable");
            [keys addObject:value];
            [descriptions addObject:[constants description]];
        }
        check(keys.count == 1 && descriptions.count == 8, "same values did not produce one key");
        NSString *initial = keys.anyObject;
        check([key(function, values(YES, NO, 7, 1.25f)) isEqualToString:initial],
              "named and indexed constants differ");
        check([key(function, values(NO, YES, 7, 1.25f)) isEqualToString:initial],
              "insertion order changes key");
        MTLFunctionConstantValues *both = values(YES, NO, 9, 1.25f);
        uint32_t seven = 7;
        [both setConstantValue:&seven type:MTLDataTypeUInt atIndex:3];
        check([key(function, both) isEqualToString:initial], "index precedence changed");
        check(different(key(function, values(NO, NO, 9, 1.25f)), initial),
              "value change kept the key");
        MTLFunctionConstantValues *mutated = instances[0];
        uint32_t eleven = 11;
        [mutated setConstantValue:&eleven type:MTLDataTypeUInt atIndex:3];
        check(different(key(function, mutated), initial), "mutation kept the key");
        MTLFunctionConstantValues *mismatch = values(NO, NO, 7, 1.25f);
        float wrong = 7.0f;
        [mismatch setConstantValue:&wrong type:MTLDataTypeFloat atIndex:3];
        check(key(function, mismatch) == nil, "type error generated a key");
        check(key(nil, instances[1]) == nil, "nil function generated a key");
        check(key([[[NSObject alloc] init] autorelease], instances[1]) == nil, "missing hash generated a key");
        check(key(function, [[[NSObject alloc] init] autorelease]) == nil, "missing serializer generated a key");
        id empty1 = [[[MTLFunctionConstantValues alloc] init] autorelease];
        id empty2 = [[[MTLFunctionConstantValues alloc] init] autorelease];
        check([key(function, empty1) isEqualToString:key(function, empty2)],
              "empty objects changed key");
        check(different(key(function, nil), key(function, empty1)),
              "nil and explicit constants must retain separate identities");
        NSString *changedSource = [source stringByReplacingOccurrencesOfString:@"out[0]=v;"
                                                                    withString:@"out[0]=v+1;"];
        id<MTLLibrary> library2 = [device newLibraryWithSource:changedSource options:nil error:&error];
        check(library2 != nil, "second native library unavailable");
        id<MTLFunction> function2 = [library2 newFunctionWithName:@"constant_kernel"];
        check(different(key(function2, instances[1]), initial),
              "different function content kept the key");
        check(different(rw_air_cache_key(function, @"constant_kernel", @"other", instances[1], nil),
                        initial), "specialized name change kept the key");
        unichar withNull[] = { 'a', 0, 'b' };
        NSString *embedded = [NSString stringWithCharacters:withNull length:3];
        check(different(rw_air_cache_key(function, @"a", @"b", nil, nil),
                        rw_air_cache_key(function, embedded, nil, nil, nil)),
              "UTF8 field boundaries collide");

        NSArray *types = @[@(MTLDataTypeFloat3), @(MTLDataTypeHalf3),
                           @(MTLDataTypeBool3), @(MTLDataTypeLong3)];
        NSArray *packedSizes = @[@12, @6, @3, @24];
        NSMutableArray *vectors = [NSMutableArray array];
        for (unsigned i = 0; i < types.count; i++) {
            NSUInteger size = [packedSizes[i] unsignedIntegerValue];
            ConstantMetadata *metadata = [[[ConstantMetadata alloc] init] autorelease];
            metadata.name = @"vector";
            metadata.index = 17;
            metadata.type = [types[i] unsignedIntegerValue];
            VectorFunction *vectorFunction = [[[VectorFunction alloc] init] autorelease];
            vectorFunction.functionConstants = @[metadata];
            uint8_t input[32], padding[32], changed[32];
            memset(input, 0x11, sizeof(input));
            memcpy(padding, input, sizeof(input)); padding[size] = 0x22;
            memcpy(changed, input, sizeof(input)); changed[size - 1] = 0x33;
            id a = [[[MTLFunctionConstantValues alloc] init] autorelease];
            id b = [[[MTLFunctionConstantValues alloc] init] autorelease];
            id c = [[[MTLFunctionConstantValues alloc] init] autorelease];
            [a setConstantValue:input type:metadata.type atIndex:17];
            [b setConstantValue:padding type:metadata.type atIndex:17];
            [c setConstantValue:changed type:metadata.type atIndex:17];
            NSData *bytes = serialize(a, vectorFunction);
            check(bytes.length == 4 + 3 + size, "native packed size changed");
            check([key(vectorFunction, a) isEqualToString:key(vectorFunction, b)],
                  "padding affected packed key");
            check(different(key(vectorFunction, a), key(vectorFunction, c)),
                  "packed value change kept key");
            [vectors addObject:@{@"type": types[i], @"serialized_bytes": @(bytes.length)}];
        }
        CC_SHA256_CTX ctx;
        CC_SHA256_Init(&ctx);
        check(!rw_air_key_bytes(&ctx, "x", (NSUInteger)UINT32_MAX + 1),
              "oversized digest field accepted");
        check(!rw_air_key_bytes(&ctx, NULL, 1), "missing digest bytes accepted");
        VectorFunction *input = [[[VectorFunction alloc] init] autorelease];
        NSString *plain = key(input, nil);
        check(plain != nil, "fixture base hash unavailable");
        for (NSUInteger mode = 1; mode <= 3; mode++) {
            input.hashMode = mode;
            check(key(input, nil) == nil, "invalid hash generated a key");
        }
        input.hashMode = 0;
        check(rw_air_cache_key(input, nil, nil, nil, nil) == nil, "nil name generated a key");
        KeyDevice *compileDevice = [[[KeyDevice alloc] init] autorelease];
        KeyCompiler *compiler = [[[KeyCompiler alloc] init] autorelease];
        input.compileDevice = compileDevice;
        check([key(input, nil) isEqualToString:plain], "nil compiler changed default inputs");
        compileDevice.compiler = compiler;
        NSString *zeroFlags = key(input, nil);
        check(zeroFlags != nil && ![zeroFlags isEqualToString:plain], "compiler presence was lost");
        compiler.compilerFlags = (NSUInteger)1 << 40;
        check(different(key(input, nil), zeroFlags), "upper compiler flags were lost");
        compileDevice.compiler = [[[NSObject alloc] init] autorelease];
        check(key(input, nil) == nil, "missing compiler flags generated a key");
        input.compileDevice = [[[NSObject alloc] init] autorelease];
        check(key(input, nil) == nil, "missing device compiler generated a key");
        input.compileDevice = nil;
        MTLFunctionDescriptor *descriptor = [[[MTLFunctionDescriptor alloc] init] autorelease];
        NSMutableArray *descriptorSelectors = [NSMutableArray array];
        for (NSString *selectorName in @[@"options", @"pipelineOptions", @"applyFunctionConstants",
                                         @"privateFunctions", @"binaryArchives", @"pluginData"]) {
            SEL sel = sel_getUid(selectorName.UTF8String);
            Method method = class_getInstanceMethod(object_getClass(descriptor), sel);
            NSMutableDictionary *row = [@{@"selector": selectorName,
                @"available": @([descriptor respondsToSelector:sel])} mutableCopy];
            if (method) {
                row[@"types"] = [NSString stringWithUTF8String:method_getTypeEncoding(method)];
                if ([selectorName isEqualToString:@"options"] || [selectorName isEqualToString:@"pipelineOptions"])
                    row[@"default"] = @(((NSUInteger (*)(id, SEL))objc_msgSend)(descriptor,sel));
                else if ([selectorName isEqualToString:@"applyFunctionConstants"])
                    row[@"default"] = @(((BOOL (*)(id, SEL))objc_msgSend)(descriptor,sel));
                else {
                    id object = ((id (*)(id, SEL))objc_msgSend)(descriptor,sel);
                    row[@"nil"] = @(object == nil);
                    if ([object respondsToSelector:@selector(count)]) row[@"count"] = @([object count]);
                }
            }
            [descriptorSelectors addObject:row]; [row release];
        }
        NSString *defaults = rw_air_cache_key(input, @"constant_kernel", @"specialized_kernel", nil, descriptor);
        check([defaults isEqualToString:plain], "native descriptor defaults differ from constants overload");
        scalar(descriptor, "setOptions:", (NSUInteger)1 << 39);
        check(different(rw_air_cache_key(input, @"constant_kernel", @"specialized_kernel", nil, descriptor),
                        defaults), "upper descriptor options were lost");
        scalar(descriptor, "setOptions:", 0);
        scalar(descriptor, "setPipelineOptions:", (NSUInteger)1 << 38);
        check(different(rw_air_cache_key(input, @"constant_kernel", @"specialized_kernel", nil, descriptor),
                        defaults), "upper pipeline options were lost");
        scalar(descriptor, "setPipelineOptions:", 0);
        ((void (*)(id, SEL, BOOL))objc_msgSend)(descriptor, sel_getUid("setApplyFunctionConstants:"), NO);
        check(different(rw_air_cache_key(input, @"constant_kernel", @"specialized_kernel", nil, descriptor),
                        defaults), "applyFunctionConstants was lost");
        ((void (*)(id, SEL, BOOL))objc_msgSend)(descriptor, sel_getUid("setApplyFunctionConstants:"), YES);
        objectValue(descriptor, "setPrivateFunctions:", @[]);
        objectValue(descriptor, "setBinaryArchives:", @[]);
        check([rw_air_cache_key(input, @"constant_kernel", @"specialized_kernel", nil, descriptor)
                isEqualToString:defaults], "empty compiler collections changed inputs");
        objectValue(descriptor, "setPrivateFunctions:", @[function]);
        check(rw_air_cache_key(input, @"constant_kernel", nil, nil, descriptor) == nil,
              "private functions generated an unchecked key");
        objectValue(descriptor, "setPrivateFunctions:", @[]);
        objectValue(descriptor, "setBinaryArchives:", @[function]);
        check(rw_air_cache_key(input, @"constant_kernel", nil, nil, descriptor) == nil,
              "binary archives generated an unchecked key");
        objectValue(descriptor, "setBinaryArchives:", @[]);
        objectValue(descriptor, "setPluginData:", @{});
        check(rw_air_cache_key(input, @"constant_kernel", nil, nil, descriptor) == nil,
              "empty plugin data generated an unchecked key");
        objectValue(descriptor, "setPluginData:", nil);
        id intersection = [[[NSClassFromString(@"MTLIntersectionFunctionDescriptor") alloc] init] autorelease];
        check(different(rw_air_cache_key(input, @"constant_kernel", @"specialized_kernel", nil, intersection),
                        defaults), "descriptor kind was lost");
        id unchecked = [[[UncheckedDescriptor alloc] init] autorelease];
        check(rw_air_cache_key(input, @"constant_kernel", nil, nil, unchecked) == nil,
              "descriptor subclass generated an unchecked key");
        dispatch_apply(8, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t index) {
            (void)index;
            @autoreleasepool {
                for (unsigned j = 0; j < 32; j++) {
                    if (![key(function, values(NO, YES, 7, 1.25f)) isEqualToString:initial])
                        atomic_fetch_add(&concurrent_errors, 1);
                }
            }
        });
        check(atomic_load(&concurrent_errors) == 0, "concurrent native serialization changed the key");
        NSDictionary *result = @{@"status": @"passed", @"checks": @(checks),
            @"os": NSProcessInfo.processInfo.operatingSystemVersionString,
            @"equivalent_instances": @8, @"unique_keys": @(keys.count),
            @"unique_descriptions": @(descriptions.count), @"packed_vectors": vectors, @"descriptor_selectors": descriptorSelectors,
            @"concurrent_keys": @256,
            @"boundary": @"Native macOS Metal functions and constant serializer; vector metadata is a local fixture. iOS implementation and hook behavior are separate checks."};
        NSData *json = [NSJSONSerialization dataWithJSONObject:result
                                  options:NSJSONWritingPrettyPrinted error:nil];
        fwrite(json.bytes, 1, json.length, stdout); fputc('\n', stdout);
        [function2 release]; [library2 release]; [function release];
        [library release]; [device release];
    }
    return 0;
}

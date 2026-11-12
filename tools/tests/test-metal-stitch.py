#!/usr/bin/env python3
"""Exercise the production graph converter with native Metal stitching objects."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--stitch-source", type=Path,
                    default=ROOT / "tools/userland-driver/annibalemetal_stitch.inc")
args = parser.parse_args()
source = args.stitch_source.read_text()
specialized = source[source.index("@interface AnnibaleMetalSpecializedFunction"):
                     source.index("static NSCache *rw_air_memcache")]
production = source[source.index("@interface AnnibaleMetalStitchedFunction"):]
reflection = (ROOT / "tools/userland-driver/annibalemetal_reflection.inc").read_text()
preamble = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "air_metal_copy.h"
#include "air_metal_reflection.h"
#define rw_print(...) ((void)0)
static BOOL rw_stitch_dump_begin(const char *s) { return NO; }
static void rw_stitch_dump(const char *s, id v, unsigned d) {}
static void air_fail_dump(const void *p, size_t n, const char *s, const char *f) {}
@interface AnnibaleMetalDevice : NSObject @end
@implementation AnnibaleMetalDevice @end
@interface TestFunction : NSObject
@property(copy) NSString *name;
- (NSData *)bitcodeData;
@end
@implementation TestFunction
- (NSData *)bitcodeData { return [NSData dataWithBytes:"BCxx" length:4]; }
@end
'''
fixture = r'''
static unsigned live_labels;
@interface OwnedLabel : NSString { NSString *_text; }
@end
@implementation OwnedLabel
- (instancetype)init {
    self = [super init];
    _text = [@"owned function label with enough text for heap storage" retain];
    ++live_labels; return self;
}
- (NSUInteger)length { return _text.length; }
- (unichar)characterAtIndex:(NSUInteger)i { return [_text characterAtIndex:i]; }
- (void)getCharacters:(unichar *)buffer range:(NSRange)range { [_text getCharacters:buffer range:range]; }
- (id)copyWithZone:(NSZone *)zone { (void)zone; return [self retain]; }
- (void)dealloc { --live_labels; [_text release]; [super dealloc]; }
@end
static void set(id object, const char *selector, NSUInteger value) {
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(object, sel_getUid(selector), value);
}
static id make(NSString *name) { return [[[NSClassFromString(name) alloc] init] autorelease]; }
static void check(BOOL value, const char *message) {
    if (!value) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
}
static void check_label(id<MTLFunction> function) {
    check([function respondsToSelector:@selector(label)] &&
          [function respondsToSelector:@selector(setLabel:)], "function label selectors");
    check(function.label == nil, "default nil label");
    @autoreleasepool {
        NSMutableString *label = [[NSMutableString alloc] initWithString:
            @"mutable function label with enough text for heap storage"];
        NSString *expected = [label copy];
        function.label = label;
        [label appendString:@" changed"];
        check([function.label isEqualToString:expected], "label copies mutable strings");
        [label release];
        function.label = function.label;
        check([function.label isEqualToString:expected], "label self assignment");
        [expected release];
        function.label = @"replacement";
        function.label = @"replacement";
        check([function.label isEqualToString:@"replacement"], "repeated label assignment");
        function.label = nil;
        check(function.label == nil, "nil clears label");
        OwnedLabel *owned = [[OwnedLabel alloc] init];
        function.label = owned;
        [owned release];
        check(live_labels == 1, "function owns copied label");
        function.label = @"replacement";
    }
    check(live_labels == 0, "replacement releases old label");
    function.label = nil;
}
static void check_label_dealloc(id<MTLFunction> owned_function) {
    @autoreleasepool {
        OwnedLabel *owned = [[OwnedLabel alloc] init];
        owned_function.label = owned;
        [owned release];
        check(live_labels == 1, "label survives caller release");
        [owned_function release];
    }
    check(live_labels == 0, "function dealloc releases label");
}
static id graph(BOOL kernel, NSArray *nodes, id output) {
    Class cls = NSClassFromString(kernel ? @"MTLFunctionStitchingGraphSPI" : @"MTLFunctionStitchingGraph");
    return [[[cls alloc] initWithFunctionName:@"test" nodes:nodes outputNode:output attributes:@[]] autorelease];
}
static id call(NSArray *args, NSArray *after) {
    return [[[MTLFunctionStitchingFunctionNode alloc] initWithName:@"f" arguments:args
        controlDependencies:after] autorelease];
}
int main(void) { @autoreleasepool {
    id<MTLDevice> native_device = MTLCreateSystemDefaultDevice();
    NSError *native_error = nil;
    id<MTLLibrary> native_library = [native_device newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal; kernel void label_test() {}"
        options:nil error:&native_error];
    check(native_library != nil, "native function library");
    id<MTLFunction> native_function = [native_library newFunctionWithName:@"label_test"];
    check_label(native_function);
    id specialized_function = [[AnnibaleMetalSpecializedFunction alloc]
        initWithFunction:native_function name:@"specialized" constantValues:nil];
    check_label(specialized_function);
    [specialized_function setLabel:@"forwarded label"];
    check([native_function.label isEqualToString:@"forwarded label"], "specialized setter forwards");
    native_function.label = @"underlying label";
    check([[specialized_function label] isEqualToString:@"underlying label"], "specialized getter forwards");
    [specialized_function release];
    native_function.label = nil;
    check(NSClassFromString(@"MTLFunctionStitchingGraphSPI") != Nil, "GraphSPI runtime class");
    id position = make(@"MTLFunctionStitchingBuiltinThreadPositionInGrid");
    id buffer = make(@"MTLFunctionStitchingInputBufferAddress");
    set(buffer, "setBindIndex:", 2); set(buffer, "setByteOffset:", 0x100000008ull);
    set(buffer, "setDereference:", YES);
    id texture = make(@"MTLFunctionStitchingInputTexture"); set(texture, "setBindIndex:", 3);
    id sampler = make(@"MTLFunctionStitchingInputSampler"); set(sampler, "setBindIndex:", 5);
    id first = call(@[position, buffer, texture, sampler], @[]);
    id early = [((id (*)(id, SEL, id, id))objc_msgSend)(
        [NSClassFromString(@"MTLFunctionStitchingEarlyReturnNode") alloc],
        sel_getUid("initWithCondition:controlDependencies:"), first, @[]) autorelease];
    id last = call(@[first], @[early]);
    TestFunction *f = [[[TestFunction alloc] init] autorelease]; f.name = @"f";
    MTLStitchedLibraryDescriptor *desc = [[[MTLStitchedLibraryDescriptor alloc] init] autorelease];
    desc.functions = (id)@[f]; desc.functionGraphs = @[graph(YES, @[first, early, last], nil)];
    AnnibaleMetalDevice *device = [[[AnnibaleMetalDevice alloc] init] autorelease];
    NSError *error = nil;
    id library = [device newLibraryWithStitchedDescriptor:desc error:&error];
    check(library != nil, "kernel library");
    id function = [library newFunctionWithName:@"test"];
    check([function functionType] == 3, "GraphSPI kernel classification");
    check_label(function);
    check_label_dealloc((id)[[AnnibaleMetalStitchedFunction alloc]
        initWithGraph:desc.functionGraphs[0] functions:desc.functions output:nil]);
    id independent = [[AnnibaleMetalStitchedFunction alloc]
        initWithGraph:desc.functionGraphs[0] functions:desc.functions output:native_function];
    [independent setLabel:@"graph label"];
    check(native_function.label == nil, "graph label belongs to graph function");
    check([[independent label] isEqualToString:@"graph label"], "graph stores independent label");
    [independent release];
    MTLComputePipelineDescriptor *pipeline = [[[MTLComputePipelineDescriptor alloc] init] autorelease];
    pipeline.computeFunction = function;
    MTLComputePipelineDescriptor *pipeline_copy = [pipeline copy];
    check(pipeline_copy.computeFunction == function, "pipeline descriptor retains graph function");
    [pipeline_copy release];
    struct rwa_reflection_type opaque = {.name = "", .alignment = 4};
    struct rwa_reflection_argument bindings[] = {
        {.name = "", .type = MTLBindingTypeBuffer, .index = 2, .active = 1,
         .array_length = 1, .constant_buffer = 1, .buffer_type = &opaque},
        {.name = "", .type = MTLBindingTypeTexture, .index = 3, .active = 1,
         .array_length = 1, .access = MTLBindingAccessWriteOnly,
         .texture_type = MTLTextureType2D, .texture_data_type = MTLDataTypeFloat},
    };
    struct rwa_compute_reflection metadata = {.argument_count = 2, .arguments = bindings};
    NSString *why = nil;
    MTLComputePipelineReflection *converted = rw_compute_reflection(&metadata, &why);
    check(converted != nil && why == nil, "opaque constant buffer reflection");
    MTLArgument *ref_buffer = converted.arguments[0], *ref_texture = converted.arguments[1];
    check(ref_buffer.bufferDataType == MTLDataTypeNone && ref_buffer.bufferAlignment == 4 &&
          ref_buffer.bufferDataSize == 0 && ref_buffer.access == MTLArgumentAccessReadOnly,
          "native opaque constant buffer fields");
    check(ref_texture.access == MTLArgumentAccessWriteOnly, "native write-only access");
    [converted release];
    opaque.data_size = 4;
    check(rw_compute_reflection(&metadata, &why) == nil, "nonempty opaque type rejected");
    opaque.data_size = 0; bindings[0].constant_buffer = 0;
    check(rw_compute_reflection(&metadata, &why) == nil, "nonconstant opaque type rejected");
    struct rwa_linked_function linked;
    struct rwa_stitched_kernel kernel;
    NSMutableData *scratch = [NSMutableData data];
    check(rw_stitched_nodes(function, &linked, scratch, &kernel), "kernel conversion");
    check(kernel.num_inputs == 4 && linked.num_nodes == 3 && linked.output == UINT_MAX, "graph shape");
    check(kernel.inputs[0].kind == RWA_STITCH_THREAD_POSITION, "thread position");
    check(kernel.inputs[1].kind == RWA_STITCH_BUFFER_ADDRESS && kernel.inputs[1].binding == 2 &&
          kernel.inputs[1].byte_offset == 0x100000008ull && kernel.inputs[1].dereference == 1,
          "buffer binding, full-width byte offset and dereference");
    check(kernel.inputs[2].kind == RWA_STITCH_TEXTURE && kernel.inputs[2].binding == 3, "texture binding");
    check(kernel.inputs[3].kind == RWA_STITCH_SAMPLER && kernel.inputs[3].binding == 5, "sampler binding");
    check(!linked.nodes[1].function && !linked.nodes[1].bitcode && linked.nodes[1].args[0] == -1,
          "EarlyReturn condition");
    check(linked.nodes[2].after[0] == 1 && linked.nodes[2].args[0] == -1, "control and value references");
    rw_linked_free(scratch); [function release]; [library release];

    id input = [[[MTLFunctionStitchingInputNode alloc] initWithArgumentIndex:0] autorelease];
    id node = call(@[input], @[]);
    for (unsigned has_output = 0; has_output < 2; has_output++) {
        desc.functionGraphs = @[graph(NO, @[node], has_output ? node : nil)];
        library = [device newLibraryWithStitchedDescriptor:desc error:&error];
        check(library != nil, "visible library");
        function = [library newFunctionWithName:@"test"];
        check([function functionType] == 5, "visible classification with value or void");
        scratch = [NSMutableData data];
        check(rw_stitched_nodes(function, &linked, scratch, NULL), "visible conversion");
        check(linked.output == (has_output ? 0 : UINT_MAX), "visible output convention");
        rw_linked_free(scratch); [function release]; [library release];
    }
    desc.functionGraphs = @[graph(NO, @[first], first)];
    check([device newLibraryWithStitchedDescriptor:desc error:&error] == nil, "special inputs in visible graph");
    desc.functionGraphs = @[graph(YES, @[last, first, early], nil)];
    check([device newLibraryWithStitchedDescriptor:desc error:&error] == nil, "forward references");
    id dependency = call(@[], @[]);
    id missing = call(@[], @[dependency]);
    desc.functionGraphs = @[graph(YES, @[missing], nil)];
    check([device newLibraryWithStitchedDescriptor:desc error:&error] == nil, "missing dependency");
    [native_function release]; [native_library release]; [native_device release];
    puts("PASS native function label copy/lifetime, specialized forwarding, GraphSPI and visible graph contracts");
} }
'''
with tempfile.TemporaryDirectory(prefix="metal-stitch-") as tmp:
    path = Path(tmp)
    (path / "test.m").write_text(preamble + reflection + specialized + production + fixture)
    subprocess.run(["xcrun", "-sdk", "macosx", "clang", "-Wno-deprecated-declarations",
                    "-Wno-objc-method-access", "-fno-objc-arc", "-fblocks",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-framework", "Foundation",
                    "-framework", "Metal", "-I" + str(ROOT / "tools/userland-driver"),
                    str(path / "test.m"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=30)

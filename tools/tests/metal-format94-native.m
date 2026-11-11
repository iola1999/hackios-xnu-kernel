/* Native Metal reference for packed BGR10A2 sampling and render-target writes.
 * Tests ordinary, buffer-backed and l10r IOSurface sources with the same pixels.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <mach/kern_return.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t packed(unsigned r, unsigned g, unsigned b, unsigned a)
{
    return b | (g << 10) | (r << 20) | (a << 30);
}

static int copy_pixels(id<MTLDevice> device, id<MTLCommandQueue> queue,
    id<MTLRenderPipelineState> pipeline, id<MTLTexture> source,
    const uint32_t expected[8], const char *name)
{
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGR10A2Unorm
        width:4 height:2 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> output = [device newTextureWithDescriptor:descriptor];
    if (!source || !output) {
        printf("FAIL %s texture creation\n", name);
        return 1;
    }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = output;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        printf("FAIL %s command error=%s\n", name, command.error.description.UTF8String);
        return 1;
    }
    uint32_t got[8] = {0};
    [output getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
    unsigned failures = 0;
    for (unsigned i = 0; i < 8; i++) {
        printf("%s pixel=%u expected=%08x actual=%08x\n", name, i, expected[i], got[i]);
        failures += expected[i] != got[i];
    }
    printf("%s %s pixels=8 mismatches=%u format=%lu\n", failures ? "FAIL" : "PASS",
        name, failures, (unsigned long)source.pixelFormat);
    return failures != 0;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { puts("SKIP native Metal device unavailable"); return 77; }
        printf("native_device=%s\n", device.name.UTF8String);
        NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 full(uint v [[vertex_id]]) {\n"
            "  const float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
            "  return float4(p[v],0,1); }\n"
            "fragment float4 sample94(float4 p [[position]], texture2d<float> t [[texture(0)]]) {\n"
            "  return t.read(uint2(p.xy)); }\n";
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:msl options:nil error:&error];
        if (!library) { printf("FAIL library %s\n", error.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *pipelineDescriptor = [MTLRenderPipelineDescriptor new];
        pipelineDescriptor.vertexFunction = [library newFunctionWithName:@"full"];
        pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"sample94"];
        pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGR10A2Unorm;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        if (!pipeline) { printf("FAIL pipeline %s\n", error.description.UTF8String); return 1; }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        const uint32_t words[8] = {
            packed(341, 682, 17, 0), packed(342, 683, 18, 1),
            packed(343, 684, 19, 2), packed(344, 685, 20, 3),
            packed(1023, 0, 1, 3), packed(0, 1023, 511, 2),
            packed(1, 512, 1023, 1), packed(513, 514, 515, 0),
        };
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatBGR10A2Unorm width:4 height:2 mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> ordinary = [device newTextureWithDescriptor:d];
        [ordinary replaceRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0 withBytes:words bytesPerRow:16];
        int failures = copy_pixels(device, queue, pipeline, ordinary, words, "ordinary");

        NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGR10A2Unorm];
        if (!alignment) { puts("FAIL linear texture alignment"); return 1; }
        NSUInteger row = ((16 + alignment - 1) / alignment) * alignment;
        id<MTLBuffer> buffer = [device newBufferWithLength:row * 2 options:MTLResourceStorageModeShared];
        if (!buffer) { puts("FAIL source buffer allocation"); return 1; }
        memcpy(buffer.contents, words, 16);
        memcpy((char *)buffer.contents + row, words + 4, 16);
        id<MTLTexture> backed = [buffer newTextureWithDescriptor:d offset:0 bytesPerRow:row];
        failures += copy_pixels(device, queue, pipeline, backed, words, "buffer");

        NSDictionary *properties = @{
            (id)kIOSurfaceWidth: @4, (id)kIOSurfaceHeight: @2,
            (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @256,
            (id)kIOSurfaceAllocSize: @512, (id)kIOSurfacePixelFormat: @((uint32_t)'l10r'),
        };
        IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)properties);
        if (!surface) { puts("FAIL l10r IOSurface allocation"); return 1; }
        kern_return_t locked = IOSurfaceLock(surface, 0, NULL);
        if (locked != KERN_SUCCESS) { printf("FAIL surface lock=%x\n", locked); CFRelease(surface); return 1; }
        void *base = IOSurfaceGetBaseAddress(surface);
        size_t stride = IOSurfaceGetBytesPerRow(surface);
        memcpy(base, words, 16);
        memcpy((char *)base + stride, words + 4, 16);
        kern_return_t unlocked = IOSurfaceUnlock(surface, 0, NULL);
        if (unlocked != KERN_SUCCESS) { printf("FAIL surface unlock=%x\n", unlocked); CFRelease(surface); return 1; }
        printf("surface fourcc=%08x bpe=%zu stride=%zu planes=%zu\n",
            IOSurfaceGetPixelFormat(surface), IOSurfaceGetBytesPerElement(surface),
            stride, IOSurfaceGetPlaneCount(surface));
        id<MTLTexture> imported = [device newTextureWithDescriptor:d iosurface:surface plane:0];
        failures += copy_pixels(device, queue, pipeline, imported, words, "surface");
        CFRelease(surface);
        printf("RESULT native_paths=3 failures=%d\n", failures);
        return failures ? 1 : 0;
    }
}

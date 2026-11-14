#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "annibale_drawable.h"

extern size_t IOSurfaceGetOffsetOfPlane(IOSurfaceRef, size_t);

static IOSurfaceRef make_surface(unsigned fourcc, unsigned element0, unsigned element1)
{
    NSDictionary *properties = @{
        (id)kIOSurfaceWidth:@64, (id)kIOSurfaceHeight:@8,
        (id)kIOSurfaceBytesPerElement:@(element0),
        (id)kIOSurfaceBytesPerRow:@128, (id)kIOSurfacePixelFormat:@(fourcc),
        (id)kIOSurfaceAllocSize:@8192,
        (id)kIOSurfacePlaneInfo:@[
            @{(id)kIOSurfacePlaneWidth:@64, (id)kIOSurfacePlaneHeight:@8,
              (id)kIOSurfacePlaneBytesPerElement:@(element0),
              (id)kIOSurfacePlaneBytesPerRow:@128, (id)kIOSurfacePlaneOffset:@128,
              (id)kIOSurfacePlaneSize:@1024},
            @{(id)kIOSurfacePlaneWidth:@32, (id)kIOSurfacePlaneHeight:@4,
              (id)kIOSurfacePlaneBytesPerElement:@(element1),
              (id)kIOSurfacePlaneBytesPerRow:@128, (id)kIOSurfacePlaneOffset:@4096,
              (id)kIOSurfacePlaneSize:@512}
        ]
    };
    IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)properties);
    return surface;
}

static id<MTLRenderPipelineState> pipeline(id<MTLDevice> device, unsigned format, unsigned kind)
{
    /* Output order is taken from the original Affine AIR return values. BGRA8 stores
     * those components as B,G,R,A in the IOSurface plane. */
    NSString *fragment = @[
        @"return float4(v(4*x+2,y),v(4*x+1,y),v(4*x,y),v(4*x+3,y));",
        @"return float4(v(4*x+2,y),v(4*x+1,y),v(4*x,y),v(4*x+3,y));",
        @"return float4(v(2*x,y),v(2*x+1,y),0,1);",
        @"return float4(v(2*x,y),v(2*x+1,y),0,1);",
        @"return float4(v(x,y),0,0,1);",
        @"return float4(v(2*x,y),v(2*x+1,y),0,1);"
    ][kind];
    NSString *program = [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "vertex float4 vert(uint i [[vertex_id]]) { float2 p[]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
         "float v(uint x,uint y) { return float(((17*x+29*y+64) %% 800)*64)/65535.0; }\n"
         "fragment float4 frag(float4 p [[position]]) { uint x=uint(p.x),y=uint(p.y); %@ }\n", fragment];
    if (format == 80 || format == 10 || format == 30)
        program = [program stringByReplacingOccurrencesOfString:
            @"float(((17*x+29*y+64) % 800)*64)/65535.0"
            withString:@"float((17*x+29*y+64) % 240)/255.0"];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:program options:nil error:&error];
    if (!library) { fprintf(stderr,"native library: %s\n",[[error description] UTF8String]); exit(3); }
    id<MTLFunction> vertex = [library newFunctionWithName:@"vert"];
    id<MTLFunction> pixel = [library newFunctionWithName:@"frag"];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex; descriptor.fragmentFunction = pixel;
    descriptor.colorAttachments[0].pixelFormat = (MTLPixelFormat)format;
    id<MTLRenderPipelineState> result = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!result) { fprintf(stderr,"native format %u pipeline: %s\n",format,[[error description] UTF8String]); exit(4); }
    [descriptor release]; [pixel release]; [vertex release]; [library release];
    return result;
}

static void run_case(id<MTLDevice> device, unsigned fourcc, unsigned plane, unsigned format,
                     unsigned view_width, unsigned kind, unsigned element)
{
    bool p010 = fourcc == 'x420' || fourcc == 'xf20';
    IOSurfaceRef surface = make_surface(fourcc,p010?2:1,p010?4:2);
    assert(surface);
    unsigned height = plane ? 4 : 8;
    struct ann_texture_shape shape = {format,2,view_width,height,1,1,1,1,5};
    struct ann_surface selected = {};
    char why[768] = {};
    void *base = NULL;
    int accepted = ann_surface_plane(surface,plane,&shape,&selected,&base,why,sizeof(why));
    if (accepted) { fprintf(stderr,"selector rejected %08x plane %u format %u: %s\n",fourcc,plane,format,why); exit(2); }
    assert(selected.width == IOSurfaceGetWidthOfPlane(surface,plane));
    assert(selected.height == IOSurfaceGetHeightOfPlane(surface,plane));
    assert(selected.rowBytes == IOSurfaceGetBytesPerRowOfPlane(surface,plane));
    assert(selected.offset == IOSurfaceGetOffsetOfPlane(surface,plane));
    assert((char *)base+selected.offset == IOSurfaceGetBaseAddressOfPlane(surface,plane));
    assert(IOSurfaceGetBytesPerElementOfPlane(surface,plane) == element);
    shape.width = view_width+1;
    assert(ann_surface_plane(surface,plane,&shape,&selected,NULL,why,sizeof(why)) != 0);
    shape.width = view_width; shape.height = height+1;
    assert(ann_surface_plane(surface,plane,&shape,&selected,NULL,why,sizeof(why)) != 0);
    shape.height = height; shape.pixel_format = 70;
    assert(ann_surface_plane(surface,plane,&shape,&selected,NULL,why,sizeof(why)) != 0);
    shape.pixel_format = format; shape.sample_count = 2;
    assert(ann_surface_plane(surface,plane,&shape,&selected,NULL,why,sizeof(why)) != 0);
    shape.sample_count = 1;
    assert(ann_surface_plane(surface,2,&shape,&selected,NULL,why,sizeof(why)) != 0);
    assert(ann_surface_plane(surface,plane,&shape,&selected,&base,why,sizeof(why)) == 0);
    assert(IOSurfaceLock(surface,0,NULL) == 0);
    memset(base,0xa5,IOSurfaceGetAllocSize(surface));
    assert(IOSurfaceUnlock(surface,0,NULL) == 0);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format width:view_width height:height mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:td iosurface:surface plane:plane];
    assert(target);
    id<MTLRenderPipelineState> state = pipeline(device,format,kind);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:state];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    assert(command.status == MTLCommandBufferStatusCompleted);
    assert(IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL) == 0);
    const uint8_t *bytes = base;
    size_t offset = selected.offset, row = selected.rowBytes;
    unsigned components = plane ? 2 : 1;
    unsigned words = (plane ? 32 : 64)*components;
    unsigned active = words*(p010?2:1);
    for (unsigned y=0;y<height;y++) for(unsigned x=0;x<words;x++) {
        unsigned expected = p010 ? ((17*x+29*y+64)%800)*64 : (17*x+29*y+64)%240;
        unsigned actual = p010 ? ((const uint16_t *)(bytes+offset+y*row))[x] : bytes[offset+y*row+x];
        if (actual != expected) { fprintf(stderr,"plane %u format %u word(%u,%u) %u != %u\n",plane,format,x,y,actual,expected); exit(5); }
    }
    for(size_t i=0;i<IOSurfaceGetAllocSize(surface);i++) {
        bool active_byte = i >= offset && i < offset+row*height && ((i-offset)%row) < active;
        assert(active_byte || bytes[i] == 0xa5);
    }
    assert(IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL) == 0);
    printf("native packed case FourCC=%08x plane=%u format=%u view=%ux%u source=%ux%u element=%u row=%zu offset=%zu bytes=%u passed\n",
        fourcc,plane,format,view_width,height,selected.width,selected.height,element,row,offset,active*height);
    [queue release]; [state release]; [target release]; CFRelease(surface);
}

int main(void) { @autoreleasepool {
    setvbuf(stdout,NULL,_IOLBF,0);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); assert(device);
    printf("native device=%s OS=%s\n",[[device name] UTF8String],[[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String]);
    for(unsigned f=0;f<4;f++) {
        unsigned fourcc = (unsigned[]){'420v','420f','x420','xf20'}[f];
        bool ten = f >= 2;
        run_case(device,fourcc,0,ten?20:10,64,4,ten?2:1);
        run_case(device,fourcc,1,ten?60:30,32,5,ten?4:2);
        run_case(device,fourcc,0,ten?60:80,ten?32:16,ten?2:0,ten?2:1);
        if (!ten) run_case(device,fourcc,1,80,16,1,2);
        IOSurfaceRef wrong = make_surface(fourcc,4,4);
        struct ann_texture_shape shape = {ten?60:80,2,ten?32:16,8,1,1,1,1,5};
        struct ann_surface selected; char why[768]={};
        if (wrong) {
            assert(ann_surface_plane(wrong,0,&shape,&selected,NULL,why,sizeof(why)) != 0);
            CFRelease(wrong);
        } else {
            printf("native surface rejected FourCC=%08x with incompatible element sizes\n",fourcc);
        }
    }
    [device release];
    puts("native packed views: 14 GPU plane writes, byte order, physical geometry, guards and rejection passed");
} }

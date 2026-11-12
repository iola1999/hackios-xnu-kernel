/* Compile the complete production heap with NSObject replacing its private
 * superclass. Allocation methods delegate to the host's native Metal device. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "heap contract:%d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

@interface AnnibaleMetalDevice : NSObject {
    id<MTLDevice> _native;
@public
    NSUInteger allocations;
}
- (instancetype)initWithNative:(id<MTLDevice>)device;
- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor;
@end

@implementation AnnibaleMetalDevice
- (instancetype)initWithNative:(id<MTLDevice>)device
{
    if ((self = [super init])) _native = [device retain];
    return self;
}
- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options
{
    allocations++;
    return [_native newBufferWithLength:length options:options];
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
    allocations++;
    return [_native newTextureWithDescriptor:descriptor];
}
- (void)dealloc { [_native release]; [super dealloc]; }
@end

static void rw_print(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

#define _MTLObjectWithLabel NSObject
#ifndef HEAP_INCLUDE
#define HEAP_INCLUDE "../userland-driver/annibalemetal_heap.inc"
#endif
#include HEAP_INCLUDE

static NSUInteger protection(id object)
{
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(object, sel_getUid("protectionOptions"));
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        CHECK(argc == 2);
        id<MTLDevice> native = MTLCreateSystemDefaultDevice();
        CHECK(native);
        AnnibaleMetalDevice *device = [[AnnibaleMetalDevice alloc] initWithNative:native];
        MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
        SEL getter = sel_getUid("protectionOptions"), setter = sel_getUid("setProtectionOptions:");
        CHECK([descriptor respondsToSelector:getter] && [descriptor respondsToSelector:setter]);
        CHECK(protection(descriptor) == 0);
        descriptor.size = 1024 * 1024;
        descriptor.storageMode = MTLStorageModePrivate;

        if (!strcmp(argv[1], "ordinary")) {
            id<MTLHeap> nativeHeap = [native newHeapWithDescriptor:descriptor];
            CHECK(nativeHeap && [nativeHeap respondsToSelector:getter]);
            CHECK(protection(nativeHeap) == protection(descriptor));
            id<MTLBuffer> nativeBuffer = [nativeHeap newBufferWithLength:256 options:MTLResourceStorageModePrivate];
            CHECK(nativeBuffer && [nativeBuffer respondsToSelector:getter] && protection(nativeBuffer) == 0);
            AnnibaleMetalHeap *heap = [device newHeapWithDescriptor:descriptor];
            CHECK(heap && [heap respondsToSelector:getter]);
            CHECK(protection(heap) == protection(nativeHeap));
            @autoreleasepool {
                id buffer = [heap newBufferWithLength:256 options:MTLResourceStorageModePrivate];
                CHECK(buffer && protection(buffer) == protection(heap));
                MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                    MTLPixelFormatR8Unorm width:16 height:16 mipmapped:NO];
                textureDescriptor.storageMode = MTLStorageModePrivate;
                id texture = [heap newTextureWithDescriptor:textureDescriptor];
                CHECK(texture && protection(texture) == protection(heap));
                CHECK(device->allocations == 2 && [heap usedSize] > 0);
                CHECK(rw_resource_heap(buffer) == heap && rw_resource_heap(texture) == heap);
                [buffer release];
                [texture release];
            }
            CHECK([heap usedSize] == 0);
            [heap release];
            [nativeBuffer release];
            [nativeHeap release];
            puts("PASS native default descriptor/heap/resource protection and production ordinary resources");
        } else {
            CHECK(!strcmp(argv[1], "protected"));
            const NSUInteger values[] = {1, 2, 3, (NSUInteger)1 << 32};
            for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
                ((void (*)(id, SEL, NSUInteger))objc_msgSend)(descriptor, setter, values[i]);
                CHECK(protection(descriptor) == values[i]);
                CHECK([device newHeapWithDescriptor:descriptor] == nil);
                CHECK(device->allocations == 0);
            }
            puts("PASS nonzero native descriptors rejected before any resource allocation");
        }
        [descriptor release];
        [device release];
        [native release];
    }
    return 0;
}

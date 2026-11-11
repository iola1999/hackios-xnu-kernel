#!/usr/bin/env python3
"""Validate the production IOSurface selector against native macOS surfaces and Metal."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "tools/userland-driver/annibaledrawable.m"

TEST = r'''
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "annibale_drawable.h"
#define BPP (COMPONENTS * 2)
#define ROW_BYTES (17 * BPP)
/* Production resolves this exported IOSurface SPI through dlsym. */
extern size_t IOSurfaceGetOffsetOfPlane(IOSurfaceRef, size_t);

static IOSurfaceRef surface(unsigned format, size_t row, size_t offset, BOOL explicitPlane) {
    NSMutableDictionary *d = [@{
        (id)kIOSurfaceWidth:@17, (id)kIOSurfaceHeight:@5,
        (id)kIOSurfaceBytesPerElement:@(BPP), (id)kIOSurfaceBytesPerRow:@(row),
        (id)kIOSurfacePixelFormat:@(format), (id)kIOSurfaceAllocSize:@4096,
        (id)kIOSurfaceOffset:@(offset),
    } mutableCopy];
    if (explicitPlane) d[(id)kIOSurfacePlaneInfo] = @[@{
        (id)kIOSurfacePlaneWidth:@17, (id)kIOSurfacePlaneHeight:@5,
        (id)kIOSurfacePlaneBytesPerElement:@(BPP), (id)kIOSurfacePlaneBytesPerRow:@(row),
        (id)kIOSurfacePlaneOffset:@(offset), (id)kIOSurfacePlaneSize:@(5 * row),
    }];
    IOSurfaceRef s = IOSurfaceCreate((CFDictionaryRef)d);
    [d release]; assert(s); return s;
}

int main(void) { @autoreleasepool {
    uint32_t fourcc=0; unsigned bpp=0; char why[768]={};
    char actual_format[32], expected_format[32], element[32];
    snprintf(actual_format,sizeof(actual_format),"FourCC 0x%08x",FOURCC);
    snprintf(expected_format,sizeof(expected_format),"expected 0x%08x",FOURCC);
    snprintf(element,sizeof(element),"element %u",BPP);
    if (ann_format_of_metal(METAL_FORMAT,&fourcc,&bpp,why,sizeof(why))) {
        fprintf(stderr,"%s mapping rejected: %s\n",FORMAT_NAME,why); return 1;
    }
    assert(fourcc==FOURCC && bpp==BPP);
    assert(ann_format_of_metal(METAL_FORMAT,NULL,NULL,NULL,0)==0);
    id<MTLDevice> device=MTLCreateSystemDefaultDevice(); assert(device);
    unsigned cases=0;
    for (unsigned explicitPlane=0;explicitPlane<2;++explicitPlane) {
        for (unsigned offset=0;offset<=128;offset+=128) {
            for (unsigned row=128;row<=256;row+=128) {
                IOSurfaceRef s=surface(FOURCC,row,offset,explicitPlane);
                struct ann_texture_shape shape={METAL_FORMAT,2,17,5,1,1,1,1,3};
                struct ann_surface selected; void *base=NULL;
                assert(ann_surface_plane(s,0,&shape,&selected,&base,why,sizeof(why))==0);
                assert(selected.pixelFormat==FOURCC && selected.width==17 && selected.height==5);
                assert(selected.rowBytes==IOSurfaceGetBytesPerRowOfPlane(s,0));
                assert(selected.offset==IOSurfaceGetOffsetOfPlane(s,0));
                assert(selected.planes==IOSurfaceGetPlaneCount(s));
                assert((char *)base+selected.offset==IOSurfaceGetBaseAddressOfPlane(s,0));
                assert(IOSurfaceGetBytesPerElementOfPlane(s,0)==BPP);
                MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:METAL_FORMAT width:17 height:5 mipmapped:NO];
                td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
                td.storageMode=MTLStorageModeShared;
                id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:s plane:0];
                assert(texture);
                uint16_t input[5][17][COMPONENTS],output[5][17][COMPONENTS];
                const uint16_t half[]={0x0000,0x8000,0x3c00,0xbc00,0x3800,0x7bff,0x0400,0x0001};
                for(unsigned y=0;y<5;++y) for(unsigned x=0;x<17;++x)
                    for(unsigned c=0;c<COMPONENTS;++c) input[y][x][c]=half[(y*17+x+c)%8];
                [texture replaceRegion:MTLRegionMake2D(0,0,17,5) mipmapLevel:0 withBytes:input bytesPerRow:ROW_BYTES];
                [texture getBytes:output bytesPerRow:ROW_BYTES fromRegion:MTLRegionMake2D(0,0,17,5) mipmapLevel:0];
                assert(memcmp(input,output,sizeof(input))==0);
                assert(IOSurfaceLock(s,kIOSurfaceLockReadOnly,NULL)==0);
                char *pixels=IOSurfaceGetBaseAddressOfPlane(s,0);
                for(unsigned y=0;y<5;++y) assert(memcmp(input[y],pixels+y*selected.rowBytes,ROW_BYTES)==0);
                assert(IOSurfaceUnlock(s,kIOSurfaceLockReadOnly,NULL)==0);
                shape.width=18;
                assert(ann_surface_plane(s,0,&shape,&selected,&base,why,sizeof(why))!=0);
                assert(strstr(why,actual_format) && strstr(why,"17x5") && strstr(why,element) && strstr(why,"row "));
                shape.width=17;shape.sample_count=2;
                assert(ann_surface_plane(s,0,&shape,&selected,&base,why,sizeof(why))!=0);
                shape.sample_count=1;
                assert(ann_surface_plane(s,1,&shape,&selected,&base,why,sizeof(why))!=0);
                printf("surface %u: planes=%u row=%zu offset=%zu native %s read/write passed\n",cases++,explicitPlane,IOSurfaceGetBytesPerRowOfPlane(s,0),IOSurfaceGetOffsetOfPlane(s,0),FORMAT_NAME);
                [texture release];CFRelease(s);
            }
        }
    }
    IOSurfaceRef wrong=surface('BGRA',128,0,NO);
    struct ann_texture_shape shape={METAL_FORMAT,2,17,5,1,1,1,1,3};struct ann_surface selected;
    assert(ann_surface_plane(wrong,0,&shape,&selected,NULL,why,sizeof(why))!=0);
    assert(strstr(why,"0x42475241") && strstr(why,expected_format) && strstr(why,element));
    CFRelease(wrong);
    struct ann_surface bad={.surface=1,.size=4096,.rowBytes=16*BPP,.width=17,.height=5};
    assert(ann_surface_fits_adreno(&bad,BPP,why,sizeof(why))!=0);
    bad.rowBytes=ROW_BYTES+1;assert(ann_surface_fits_adreno(&bad,BPP,why,sizeof(why))!=0);
    bad.rowBytes=128;bad.offset=128;bad.size=767;
    assert(ann_surface_fits_adreno(&bad,BPP,why,sizeof(why))!=0);
    bad.size=768;assert(ann_surface_fits_adreno(&bad,BPP,why,sizeof(why))==0);
    [device release];
    printf("%s surface: native planes, row/offset layouts, half data, bounds and format rejection passed\n",FORMAT_NAME);
} }
'''


def main():
    with tempfile.TemporaryDirectory(prefix="rg16-surface-") as directory:
        source = Path(directory) / "test.m"
        binary = Path(directory) / "test"
        for label, metal, fourcc, components in [("RG16", 65, 0x32433068, 2),
                                               ("R16", 25, 0x4c303068, 1)]:
            print(label, flush=True)
            source.write_text(TEST)
            subprocess.run(["clang", f"-DMETAL_FORMAT={metal}", f"-DFOURCC={fourcc}",
                            f"-DCOMPONENTS={components}", f'-DFORMAT_NAME="{label}"', "-fblocks",
                            "-Wall", "-Wextra", "-Werror",
                            "-Wno-deprecated-declarations", "-Wno-multichar", "-O1",
                            "-fsanitize=address,undefined", "-I", str(DRIVER.parent),
                        str(source), str(DRIVER), "-framework", "Foundation",
                            "-framework", "IOKit", "-framework", "IOSurface",
                            "-framework", "Metal", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

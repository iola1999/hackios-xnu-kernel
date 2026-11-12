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
#define ROW_BYTES (WIDTH * BPP)
/* Production resolves this exported IOSurface SPI through dlsym. */
extern size_t IOSurfaceGetOffsetOfPlane(IOSurfaceRef, size_t);

static IOSurfaceRef surface(unsigned format, size_t row, size_t offset, BOOL explicitPlane) {
    NSMutableDictionary *d = [@{
        (id)kIOSurfaceWidth:@WIDTH, (id)kIOSurfaceHeight:@HEIGHT,
        (id)kIOSurfaceBytesPerElement:@(BPP), (id)kIOSurfaceBytesPerRow:@(row),
        (id)kIOSurfacePixelFormat:@(format),
        (id)kIOSurfaceAllocSize:@((offset + row * HEIGHT + 4095) & ~(size_t)4095),
        (id)kIOSurfaceOffset:@(offset),
    } mutableCopy];
    if (explicitPlane) d[(id)kIOSurfacePlaneInfo] = @[@{
        (id)kIOSurfacePlaneWidth:@WIDTH, (id)kIOSurfacePlaneHeight:@HEIGHT,
        (id)kIOSurfacePlaneBytesPerElement:@(BPP), (id)kIOSurfacePlaneBytesPerRow:@(row),
        (id)kIOSurfacePlaneOffset:@(offset), (id)kIOSurfacePlaneSize:@(HEIGHT * row),
    }];
    IOSurfaceRef s = IOSurfaceCreate((CFDictionaryRef)d);
    [d release]; assert(s); return s;
}

int main(void) { @autoreleasepool {
    uint32_t fourcc=0; unsigned bpp=0; char why[768]={};
    char actual_format[32], expected_format[32], element[32], geometry[32];
    snprintf(actual_format,sizeof(actual_format),"FourCC 0x%08x",FOURCC);
    snprintf(expected_format,sizeof(expected_format),"expected 0x%08x",CANONICAL_FOURCC);
    snprintf(element,sizeof(element),"element %u",BPP);
    snprintf(geometry,sizeof(geometry),"%ux%u",WIDTH,HEIGHT);
    if (ann_format_of_metal(METAL_FORMAT,&fourcc,&bpp,why,sizeof(why))) {
        fprintf(stderr,"%s mapping rejected: %s\n",FORMAT_NAME,why); return 1;
    }
    assert(fourcc==CANONICAL_FOURCC && bpp==BPP);
    assert(ann_format_of_metal(METAL_FORMAT,NULL,NULL,NULL,0)==0);
    id<MTLDevice> device=MTLCreateSystemDefaultDevice(); assert(device);
    unsigned cases=0;
    for (unsigned explicitPlane=0;explicitPlane<2;++explicitPlane) {
        for (unsigned offset=0;offset<=128;offset+=128) {
            for (unsigned row=BASE_ROW;row<=BASE_ROW+128;row+=128) {
                IOSurfaceRef s=surface(FOURCC,row,offset,explicitPlane);
                struct ann_texture_shape shape={METAL_FORMAT,2,WIDTH,HEIGHT,1,1,1,1,3};
                struct ann_surface selected; void *base=NULL;
                assert(ann_surface_plane(s,0,&shape,&selected,&base,why,sizeof(why))==0);
                assert(selected.pixelFormat==FOURCC && selected.width==WIDTH && selected.height==HEIGHT);
                assert(selected.rowBytes==IOSurfaceGetBytesPerRowOfPlane(s,0));
                assert(selected.offset==IOSurfaceGetOffsetOfPlane(s,0));
                assert(selected.planes==IOSurfaceGetPlaneCount(s));
                assert((char *)base+selected.offset==IOSurfaceGetBaseAddressOfPlane(s,0));
                assert(IOSurfaceGetBytesPerElementOfPlane(s,0)==BPP);
                MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:METAL_FORMAT width:WIDTH height:HEIGHT mipmapped:NO];
                td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
                td.storageMode=MTLStorageModeShared;
                id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:s plane:0];
                assert(texture);
                uint16_t input[HEIGHT][WIDTH][COMPONENTS],output[HEIGHT][WIDTH][COMPONENTS];
                const uint16_t half[]={0x0000,0x8000,0x3c00,0xbc00,0x3800,0x7bff,0x0400,0x0001};
                for(unsigned y=0;y<HEIGHT;++y) for(unsigned x=0;x<WIDTH;++x)
                    for(unsigned c=0;c<COMPONENTS;++c) input[y][x][c]=half[(y*WIDTH+x+c)%8];
                [texture replaceRegion:MTLRegionMake2D(0,0,WIDTH,HEIGHT) mipmapLevel:0 withBytes:input bytesPerRow:ROW_BYTES];
                [texture getBytes:output bytesPerRow:ROW_BYTES fromRegion:MTLRegionMake2D(0,0,WIDTH,HEIGHT) mipmapLevel:0];
                assert(memcmp(input,output,sizeof(input))==0);
                assert(IOSurfaceLock(s,kIOSurfaceLockReadOnly,NULL)==0);
                char *pixels=IOSurfaceGetBaseAddressOfPlane(s,0);
                for(unsigned y=0;y<HEIGHT;++y) assert(memcmp(input[y],pixels+y*selected.rowBytes,ROW_BYTES)==0);
                assert(IOSurfaceUnlock(s,kIOSurfaceLockReadOnly,NULL)==0);
                shape.width=WIDTH+1;
                assert(ann_surface_plane(s,0,&shape,&selected,&base,why,sizeof(why))!=0);
                assert(strstr(why,actual_format) && strstr(why,geometry) && strstr(why,element) && strstr(why,"row "));
                shape.width=WIDTH;shape.sample_count=2;
                assert(ann_surface_plane(s,0,&shape,&selected,&base,why,sizeof(why))!=0);
                shape.sample_count=1;
                assert(ann_surface_plane(s,1,&shape,&selected,&base,why,sizeof(why))!=0);
                if (FOURCC == 0x31433068) {
                    shape.pixel_format=30;
                    assert(ann_surface_plane(s,0,&shape,&selected,&base,why,sizeof(why))!=0);
                    shape.pixel_format=METAL_FORMAT;
                }
                printf("surface %u: planes=%u row=%zu offset=%zu native %s read/write passed\n",cases++,explicitPlane,IOSurfaceGetBytesPerRowOfPlane(s,0),IOSurfaceGetOffsetOfPlane(s,0),FORMAT_NAME);
                [texture release];CFRelease(s);
            }
        }
    }
    IOSurfaceRef wrong=surface('BGRA',BASE_ROW,0,NO);
    struct ann_texture_shape shape={METAL_FORMAT,2,WIDTH,HEIGHT,1,1,1,1,3};struct ann_surface selected;
    assert(ann_surface_plane(wrong,0,&shape,&selected,NULL,why,sizeof(why))!=0);
    assert(strstr(why,"0x42475241") && strstr(why,expected_format) && strstr(why,element));
    CFRelease(wrong);
    struct ann_surface bad={.surface=1,.size=4096,.rowBytes=(WIDTH-1)*BPP,.width=WIDTH,.height=5};
    assert(ann_surface_fits_adreno(&bad,BPP,why,sizeof(why))!=0);
    bad.rowBytes=ROW_BYTES+1;assert(ann_surface_fits_adreno(&bad,BPP,why,sizeof(why))!=0);
    bad.rowBytes=BASE_ROW;bad.offset=128;bad.size=128+5*BASE_ROW-1;
    assert(ann_surface_fits_adreno(&bad,BPP,why,sizeof(why))!=0);
    ++bad.size;assert(ann_surface_fits_adreno(&bad,BPP,why,sizeof(why))==0);
    [device release];
    printf("%s surface: native planes, row/offset layouts, half data, bounds and format rejection passed\n",FORMAT_NAME);
} }
'''


def main():
    with tempfile.TemporaryDirectory(prefix="rg16-surface-") as directory:
        source = Path(directory) / "test.m"
        binary = Path(directory) / "test"
        for label, metal, fourcc, components, width, height, row in [
                ("RG16", 65, 0x32433068, 2, 17, 5, 128),
                ("R16-L00h", 25, 0x4c303068, 1, 17, 5, 128),
                ("R16-1C0h", 25, 0x31433068, 1, 17, 5, 128),
                ("R16-1C0h-380", 25, 0x31433068, 1, 380, 380, 768),
                ("R16-1C0h-128", 25, 0x31433068, 1, 128, 128, 256),
                ("R16-1C0h-256", 25, 0x31433068, 1, 256, 380, 512)]:
            print(label, flush=True)
            source.write_text(TEST)
            subprocess.run(["clang", f"-DMETAL_FORMAT={metal}", f"-DFOURCC={fourcc}",
                            f"-DCOMPONENTS={components}", f"-DCANONICAL_FOURCC={0x32433068 if components == 2 else 0x4c303068}",
                            f"-DWIDTH={width}", f"-DHEIGHT={height}", f"-DBASE_ROW={row}", f'-DFORMAT_NAME="{label}"', "-fblocks",
                            "-Wall", "-Wextra", "-Werror",
                            "-Wno-deprecated-declarations", "-Wno-multichar", "-O1",
                            "-fsanitize=address,undefined", "-I", str(DRIVER.parent),
                        str(source), str(DRIVER), "-framework", "Foundation",
                            "-framework", "IOKit", "-framework", "IOSurface",
                            "-framework", "Metal", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run production texture/pipeline/import entrypoints and report format 94 gaps.

Default mode checks the named current failures as a diagnostic baseline.
--require-correct exits nonzero while any required contract fails.
--preserve94 evaluates a temporary canonicalizer-only candidate in the harness.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "tools/userland-driver"


def body(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    # Skip comments and string literals while counting C/Objective-C braces.
    tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
                         text[opening:], re.S)
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return text[start:opening + token.end()]
    raise ValueError(marker)


PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "annibale_drawable.h"
@interface NSObject (TestFunction)
- (id)annibaleUnspecializedFunction;
@end
#define RWA_MAX_VERTEX_ATTRIBUTES 31
#define RWA_MAX_VERTEX_BUFFERS 31
struct rwa_device { int unused; };
struct rwa_buffer { int unused; };
struct rwa_texture { int unused; };
struct rwa_render { int unused; };
struct rwa_linked_function { int unused; };
'''

HARNESS = r'''
static struct rwa_device device_token;
static struct rwa_buffer buffer_token;
static struct rwa_texture texture_token;
static struct rwa_render render_token;
static struct rwa_texture_shape ordinary_shape, buffer_shape;
static struct rwa_render_shape pipeline_shape;
static unsigned import_format, calls, failures, checks;
static bool reject_backend;
static struct ann_surface described;
static void check(const char *name, bool ok) {
    checks++; failures += !ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
static void rw_print(const char *format, ...) { (void)format; }
static bool rw_one_sample(void) { return false; }
static struct rwa_texture *create_texture(struct rwa_device *d,
    const struct rwa_texture_shape *s, char *why, size_t n) {
    (void)d; ordinary_shape=*s; calls++;
    if (reject_backend) { snprintf(why,n,"test allocation failure"); return NULL; }
    return &texture_token;
}
static struct rwa_texture *from_buffer(struct rwa_device *d,
    const struct rwa_texture_shape *s, struct rwa_buffer *b, size_t offset,
    unsigned stride, char *why, size_t n) {
    (void)d; (void)b; (void)offset; (void)stride; buffer_shape=*s; calls++;
    if (reject_backend) { snprintf(why,n,"test buffer failure"); return NULL; }
    return &texture_token;
}
static struct rwa_render *create_render(struct rwa_device *d,
    const void *v, size_t vn, const char *ve, const void *f, size_t fn,
    const char *fe, const struct rwa_render_shape *s,
    struct rwa_render_info *info, char *why, size_t n) {
    (void)d;(void)v;(void)vn;(void)ve;(void)f;(void)fn;(void)fe;(void)info;
    pipeline_shape=*s; calls++;
    if (reject_backend) { snprintf(why,n,"test pipeline failure"); return NULL; }
    return &render_token;
}
static struct {
    __typeof__(&create_texture) texture_create;
    __typeof__(&from_buffer) texture_from_buffer;
    __typeof__(&create_render) render_create;
    struct rwa_render *(*render_create_linked)(struct rwa_device *,const void *,size_t,
        const char *,const void *,size_t,const char *,const struct rwa_linked_function *,
        unsigned,const struct rwa_render_shape *,struct rwa_render_info *,char *,size_t);
} mesa = {create_texture, from_buffer, create_render, NULL};
#define RW_LINKED_MAX 1
#define rw_stitch_dump_linked(...) ((void)0)
#define rw_dump_pipeline_air(...) ((void)0)
#define rw_blend_line(...) false
#define rw_pipe_key(...) ((void)0)
#define rw_pipe_find(...) NULL
#define rw_pipe_keep(...) ((void)0)
#define rw_linked_fragment_functions(...) 0u
#define rw_linked_free(...) ((void)0)
#define air_fail_dump(...) ((void)0)
#define rw_dump_linked(...) ((void)0)
#define RW_AGAIN(...) ((void)0)
#define rw_keep_air(...) ((void)0)
#define rw_blend_result(...) ((void)0)
#define rw_combined_yuv(...) nil
#define rw_surface_through_copy(...) nil
@interface AnnibaleMetalDevice : NSObject { @public struct rwa_device *_mesa; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(void *)s plane:(NSUInteger)p;
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error;
- (struct rwa_device *)mesaDevice;
- (id)fail:(NSError **)error with:(NSString *)message;
@end
@interface AnnibaleMetalTexture : NSObject {
    struct rwa_texture_shape _shape;
    id _buffer;
    NSUInteger _bufferOffset, _bufferBytesPerRow;
}
- (id)initWithAnnibaleDevice:(id)d texture:(struct rwa_texture *)t
    shape:(const struct rwa_texture_shape *)s iosurface:(CFTypeRef)surface plane:(NSUInteger)plane;
- (void)annibaleSetBuffer:(id)b offset:(NSUInteger)o bytesPerRow:(NSUInteger)r;
- (NSUInteger)pixelFormat;
@end
@interface AnnibaleMetalBuffer : NSObject {
@public struct rwa_buffer *_mesa; AnnibaleMetalDevice *_owner;
}
- (id)newTextureWithDescriptor:(id)d offset:(NSUInteger)o bytesPerRow:(NSUInteger)r;
@end
@interface AnnibaleMetalRenderPipeline : NSObject
- (id)initWithAnnibaleDevice:(id)d render:(struct rwa_render *)r
    info:(const struct rwa_render_info *)i descriptor:(id)descriptor;
@end
@implementation AnnibaleMetalRenderPipeline
- (id)initWithAnnibaleDevice:(id)d render:(struct rwa_render *)r
    info:(const struct rwa_render_info *)i descriptor:(id)descriptor {
    (void)d;(void)r;(void)i;(void)descriptor; return [super init];
}
@end
@interface TestFunction : NSObject
- (NSData *)bitcodeData;
- (NSString *)name;
@end
@implementation TestFunction
- (NSData *)bitcodeData { return [NSData dataWithBytes:"test" length:4]; }
- (NSString *)name { return @"test.fragment"; }
@end
@interface TestPipelineDescriptor : NSObject { @public MTLRenderPipelineDescriptor *native; }
@end
@implementation TestPipelineDescriptor
- (id)fragmentFunction { return [[[TestFunction alloc] init] autorelease]; }
- (id)vertexFunction { return nil; }
- (id)forwardingTargetForSelector:(SEL)s { (void)s; return native; }
@end
static void ann_say(char *why,size_t n,const char *format,...) {
    va_list ap; va_start(ap,format); vsnprintf(why,n,format,ap); va_end(ap);
}
static io_connect_t ann_shared_connection(void) { return 1; }
int ann_surface_describe(io_connect_t c,uint32_t id,struct ann_surface *out,char *why,size_t n) {
    (void)c;(void)id;(void)why;(void)n; *out=described; return 0;
}
typedef struct rwa_texture *(*ann_import_call)(struct rwa_device *,
    const struct ann_texture_shape *,int,unsigned,unsigned,char *,size_t);
static struct rwa_texture *import_texture(struct rwa_device *d,const struct ann_texture_shape *s,
    int id,unsigned offset,unsigned stride,char *why,size_t n) {
    (void)d;(void)id;(void)offset;(void)stride;
    import_format=s->pixel_format; calls++;
    if (reject_backend) { snprintf(why,n,"test import failure"); return NULL; }
    return &texture_token;
}
static ann_import_call ann_import(void) { return import_texture; }
static IOSurfaceRef make_surface(uint32_t fourcc) {
    IOSurfaceRef s=IOSurfaceCreate((CFDictionaryRef)@{
        (id)kIOSurfaceWidth:@4, (id)kIOSurfaceHeight:@2,
        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfaceBytesPerRow:@64,
        (id)kIOSurfaceAllocSize:@128, (id)kIOSurfacePixelFormat:@(fourcc)});
    if (!s) { puts("FAIL test IOSurface allocation"); exit(2); }
    described=(struct ann_surface){.surface=IOSurfaceGetID(s),
        .size=IOSurfaceGetAllocSize(s),.rowBytes=IOSurfaceGetBytesPerRow(s),
        .width=IOSurfaceGetWidth(s),.height=IOSurfaceGetHeight(s),
        .pixelFormat=IOSurfaceGetPixelFormat(s),.planes=IOSurfaceGetPlaneCount(s)};
    return s;
}
'''

MAIN = r'''
int main(void) { @autoreleasepool {
    AnnibaleMetalDevice *device=[AnnibaleMetalDevice new]; device->_mesa=&device_token;
    AnnibaleMetalBuffer *buffer=[AnnibaleMetalBuffer new];
    buffer->_mesa=&buffer_token; buffer->_owner=device;
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatBGR10A2Unorm width:4 height:2 mipmapped:NO];
    d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
    id ordinary=[device newTextureWithDescriptor:d];
    id backed=[buffer newTextureWithDescriptor:d offset:64 bytesPerRow:64];
    check("ordinary preserves BGR10A2", ordinary && ordinary_shape.pixel_format==94 && [ordinary pixelFormat]==94);
    check("buffer preserves BGR10A2", backed && buffer_shape.pixel_format==94 && [backed pixelFormat]==94);
    check("ordinary and buffer geometry", ordinary_shape.width==4 && ordinary_shape.height==2 &&
        memcmp(&ordinary_shape.type,&buffer_shape.type,sizeof(ordinary_shape)-sizeof(unsigned))==0);
    TestPipelineDescriptor *p=[TestPipelineDescriptor new]; p->native=[MTLRenderPipelineDescriptor new];
    p->native.colorAttachments[0].pixelFormat=MTLPixelFormatBGR10A2Unorm;
    NSError *error=nil;
    id pipeline=[device newRenderPipelineStateWithDescriptor:(id)p error:&error];
    check("pipeline preserves BGR10A2", pipeline && !error && pipeline_shape.color_format[0]==94);
    struct rw_block block;
    check("BGR10A2 texel size", rw_format_block(94,&block) && block.width==1 && block.height==1 && block.bytes==4);
    uint32_t fourcc=0; unsigned bytes=0; char why[512]={0};
    int rc=ann_format_of_metal(94,&fourcc,&bytes,why,sizeof(why));
    check("native l10r surface format", rc==0 && fourcc=='l10r' && bytes==4);
    IOSurfaceRef bgra=make_surface(fourcc);
    id surface=[device newTextureWithDescriptor:d iosurface:bgra plane:0];
    check("surface import preserves BGR10A2", surface && [surface pixelFormat]==94 && import_format==94);
    check("pipeline matches imported attachment", surface && import_format==pipeline_shape.color_format[0]);
    described.pixelFormat='RGBA'; calls=0;
    check("kernel surface mismatch rejects import", [device newTextureWithDescriptor:d iosurface:bgra plane:0]==nil && calls==0);
    IOSurfaceRef native=make_surface('l10r');
    surface=[device newTextureWithDescriptor:d iosurface:native plane:0];
    check("native l10r surface entrypoint", surface && [surface pixelFormat]==94 && import_format==94);
    CFRelease(native); CFRelease(bgra);
    reject_backend=true; calls=0;
    check("ordinary allocation failure", [device newTextureWithDescriptor:d]==nil && calls==1);
    check("buffer allocation failure", [buffer newTextureWithDescriptor:d offset:64 bytesPerRow:64]==nil && calls==2);
    error=nil;
    check("pipeline failure reaches NSError", [device newRenderPipelineStateWithDescriptor:(id)p error:&error]==nil && error!=nil);
    bgra=make_surface(fourcc); calls=0;
    check("surface allocation failure", [device newTextureWithDescriptor:d iosurface:bgra plane:0]==nil && calls==1);
    CFRelease(bgra);
    reject_backend=false; d.pixelFormat=MTLPixelFormatBGRA8Unorm;
    id control=[device newTextureWithDescriptor:d];
    check("BGRA8 control", control && ordinary_shape.pixel_format==80 && [control pixelFormat]==80);
    printf("RESULT checks=%u failures=%u\n",checks,failures);
    return failures ? 1 : 0;
} }
'''


def build(preserve, revision):
    def read(name):
        if revision:
            return subprocess.check_output(["git", "-C", str(ROOT), "show",
                f"{revision}:tools/userland-driver/{name}"], text=True)
        return (DRIVER / name).read_text()
    main = read("annibalemetal.m")
    device = read("annibalemetal_device.inc")
    texture = read("annibalemetal_texture.inc")
    drawable = read("annibaledrawable.m")
    header = read("annibale_drawable.h")
    if preserve:
        assert "fmt == 94 || " in header
        header = header.replace("fmt == 94 || ", "", 1)
    types = "\n".join(body(main, "struct " + name + " {") + ";" for name in [
        "rwa_texture_shape", "rwa_vertex_attribute", "rwa_vertex_layout", "rwa_color_blend",
        "rwa_render_shape", "rwa_render_info"])
    code = PREFIX + types + HARNESS
    code += "\n@implementation AnnibaleMetalDevice\n"
    code += "- (struct rwa_device *)mesaDevice { return _mesa; }\n"
    code += '- (id)fail:(NSError **)e with:(NSString *)s { if(e) *e=[NSError errorWithDomain:s code:1 userInfo:nil]; return nil; }\n'
    code += body(device, "- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor\n")
    code += body(device, "- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor")
    code += body(texture, "- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:")
    code += "\n@end\n@implementation AnnibaleMetalBuffer\n"
    code += body(texture, "- (id)newTextureWithDescriptor:(id)descriptor offset:")
    code += "\n@end\n@implementation AnnibaleMetalTexture\n"
    code += "- (id)initWithAnnibaleDevice:(id)d texture:(struct rwa_texture *)t shape:(const struct rwa_texture_shape *)s iosurface:(CFTypeRef)surface plane:(NSUInteger)plane { (void)d;(void)t;(void)surface;(void)plane; self=[super init]; if(self) _shape=*s; return self; }\n"
    code += body(texture, "- (void)annibaleSetBuffer:")
    code += body(texture, "- (NSUInteger)pixelFormat") + "\n@end\n"
    layout = read("annibalemetal_texture_layout.inc")
    code += body(layout, "struct rw_block {") + ";\nstatic BOOL\n" + body(layout, "rw_format_block(")
    code += drawable[drawable.index("struct ann_format {"):drawable.index("/* IOSurface offsets")]
    code += "\nuint32_t\n" + body(drawable, "ann_surface_id(")
    code += "\nint\n" + body(drawable, "ann_surface_fits_adreno(")
    code += "\nint\n" + body(drawable, "ann_surface_plane(")
    code += "\nstruct rwa_texture *\n" + body(drawable, "ann_texture_for_surface(")
    return header, code + MAIN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision")
    parser.add_argument("--preserve94", action="store_true")
    parser.add_argument("--require-correct", action="store_true")
    args = parser.parse_args()
    header, code = build(args.preserve94, args.revision)
    with tempfile.TemporaryDirectory(prefix="metal-format94-") as tmp:
        directory = Path(tmp)
        (directory / "annibale_drawable.h").write_text(header)
        source = directory / "contract.m"
        source.write_text(code)
        binary = directory / "contract"
        subprocess.run(["xcrun", "-sdk", "macosx", "clang", "-fno-objc-arc",
            "-fsanitize=address,undefined", "-Wno-unused-variable", "-Wno-unused-function",
            "-framework", "Foundation", "-framework", "Metal", "-framework", "IOKit",
            "-framework", "IOSurface",
            str(source), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True)
        print(result.stdout, end="")
        print(result.stderr, end="")
        if args.require_correct:
            return result.returncode
        match = re.search(r"RESULT checks=(\d+) failures=(\d+)", result.stdout)
        expected = {"native l10r surface format", "surface import preserves BGR10A2",
                    "native l10r surface entrypoint"}
        expected |= ({"pipeline matches imported attachment"} if args.preserve94 else
                     {"ordinary preserves BGR10A2", "pipeline preserves BGR10A2"})
        failed = set(re.findall(r"^FAIL (.+)$", result.stdout, re.M))
        if not match or failed != expected or int(match[2]) != len(expected) or result.returncode != 1:
            print(f"Diagnostic baseline changed: expected={sorted(expected)}, actual={sorted(failed)}")
            return 1
        print(f"Known failures reproduced: {len(expected)}; required contracts remain incomplete.")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())

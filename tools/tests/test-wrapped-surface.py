#!/usr/bin/env python3
"""Run actual IOSurface import/cache and AIR mirror functions with a modeled GPU.

The IOConnect result and GPU pixel transfers are controlled; this does not exercise
A830 execution. --driver selects a frozen source directory for reverse checks.
"""
import argparse
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
body = runpy.run_path(str(Path(__file__).with_name('test-metal-format94.py')))['body']

PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "annibale_drawable.h"
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define RWA_LOCKED
struct rwa_buffer_texture;
struct rwa_device { struct rwa_buffer_texture *buffer_textures; };
struct rwa_buffer { struct rwa_device *device; void *map,*mirror; size_t size; uint64_t mirror_hash; bool mirror_hashed; };
struct pipe_resource { unsigned height0,width0; };
struct rwa_texture { struct pipe_resource *res; struct rwa_buffer_texture *backing; char *pixels; const char *format_name; };
struct rwa_buffer_texture { struct rwa_texture *texture; struct rwa_buffer *source; size_t offset; unsigned row_bytes; uint64_t hash; bool hashed,written; struct rwa_buffer_texture *next; };
static unsigned checks, failures, imports, buffers, released_buffers, released_textures;
static bool reject_buffer, reject_texture, reject_import, no_import;
static int kernel_result;
static bool short_reply, mismatch;
static io_connect_t connection=1;
static struct ann_surface described;
static void check(const char *name,bool ok) { checks++; failures+=!ok; printf("%s %s\n",ok?"PASS":"FAIL",name); }
static void ann_say(char *why,size_t size,const char *fmt,...) { va_list ap;va_start(ap,fmt);vsnprintf(why,size,fmt,ap);va_end(ap); }
#define say ann_say
static void rw_print(const char *fmt,...) { (void)fmt; }
static struct { unsigned refreshes,uploads,unchanged; } upload_count;
static int rwa_texture_write(struct rwa_texture *t,unsigned a,unsigned b,const void *c,const void *d,const void *p,unsigned row,char *why,size_t n) {
 (void)a;(void)b;(void)c;(void)d;(void)why;(void)n;
 for(unsigned y=0;y<t->res->height0;y++) memcpy(t->pixels+y*t->res->width0*4,(const char *)p+y*row,t->res->width0*4);return 0;
}
static int rwa_texture_read(struct rwa_texture *t,unsigned a,unsigned b,const void *c,const void *d,void *p,unsigned row,char *why,size_t n) {
 (void)a;(void)b;(void)c;(void)d;(void)why;(void)n;
 for(unsigned y=0;y<t->res->height0;y++) memcpy((char *)p+y*row,t->pixels+y*t->res->width0*4,t->res->width0*4);return 0;
}
static kern_return_t fake_call(io_connect_t c,uint32_t sel,const void *in,size_t ins,void *out,size_t *size) {
 (void)c;(void)sel;(void)in;(void)ins;
 if(kernel_result) return kernel_result;
 memcpy(out,&described,sizeof(described));
 if(mismatch)((struct ann_surface *)out)->pixelFormat='BGRA';
 if(short_reply)*size=1;
 return 0;
}
#define IOConnectCallStructMethod fake_call
#define ANN_REQUEST_SURFACE 0x7e
static io_connect_t ann_shared_connection(void) { return connection; }
static struct rwa_texture direct_texture;
typedef struct rwa_texture *(*ann_import_call)(struct rwa_device *,const struct ann_texture_shape *,int,unsigned,unsigned,char *,size_t);
static struct rwa_texture *test_import(struct rwa_device *d,const struct ann_texture_shape *s,int id,unsigned off,unsigned row,char *why,size_t n) {
 (void)d;(void)s;(void)id;(void)off;(void)row;(void)why;(void)n;imports++;return reject_import?NULL:&direct_texture;
}
static ann_import_call ann_import(void) { return no_import?NULL:test_import; }
'''

CLASSES = r'''
static void texture_destroy(struct rwa_texture *t) {
 if(t==&direct_texture)return;
 if(t->backing) {
  struct rwa_buffer_texture **p=&t->backing->source->device->buffer_textures;
  while(*p && *p!=t->backing)p=&(*p)->next;
  if(*p)*p=(*p)->next;
  free(t->backing);
 }
 free(t->res);free(t->pixels);free(t);released_textures++;
}
static struct { void (*texture_destroy)(struct rwa_texture *); } mesa={texture_destroy};
#define rw_combined_yuv(...) nil
@interface AnnibaleMetalDevice:NSObject { @public struct rwa_device *_mesa; }
- (id)newBufferWithBytesNoCopy:(void *)bytes length:(NSUInteger)n options:(NSUInteger)o deallocator:(void(^)(void *,NSUInteger))block;
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(void *)s plane:(NSUInteger)p;
@end
@interface AnnibaleMetalBuffer:NSObject { @public struct rwa_buffer storage; id owner; void(^deallocator)(void *,NSUInteger); }
- (id)newTextureWithDescriptor:(id)d offset:(NSUInteger)o bytesPerRow:(NSUInteger)r;
@end
@interface AnnibaleMetalTexture:NSObject { @public id _owner,_parentTexture,_buffer; struct rwa_texture *_texture; struct rwa_texture_shape _shape; CFTypeRef _iosurface; NSUInteger _iosurfacePlane,_bufferOffset,_bufferBytesPerRow; }
- (id)initWithAnnibaleDevice:(id)d texture:(struct rwa_texture *)t shape:(const struct rwa_texture_shape *)s iosurface:(CFTypeRef)surface plane:(NSUInteger)p;
- (void)annibaleSetBuffer:(id)b offset:(NSUInteger)o bytesPerRow:(NSUInteger)r;
- (void)annibaleSetSurface:(CFTypeRef)s plane:(NSUInteger)p;
- (NSUInteger)usage;
@end
@implementation AnnibaleMetalBuffer
- (id)newTextureWithDescriptor:(id)d offset:(NSUInteger)o bytesPerRow:(NSUInteger)r {
 if(reject_texture)return nil;
 struct rwa_texture *t=calloc(1,sizeof(*t)); t->res=calloc(1,sizeof(*t->res));
 t->res->height0=[d height];t->res->width0=[d width];t->format_name="RG16Float";
 t->pixels=calloc(t->res->width0*t->res->height0,4);
 struct rwa_buffer_texture *b=calloc(1,sizeof(*b));
 *b=(struct rwa_buffer_texture){.texture=t,.source=&storage,.offset=o,.row_bytes=r,.next=storage.device->buffer_textures};
 storage.device->buffer_textures=b;t->backing=b;
 char why[256];if(buffer_texture_upload(b,why,sizeof(why))) {texture_destroy(t);return nil;}
 struct rwa_texture_shape s={.pixel_format=[d pixelFormat],.width=[d width],.height=[d height],.usage=[d usage]};
 AnnibaleMetalTexture *tex=[[AnnibaleMetalTexture alloc] initWithAnnibaleDevice:owner texture:t shape:&s iosurface:NULL plane:0];
 [tex annibaleSetBuffer:self offset:o bytesPerRow:r];return tex;
}
- (void)dealloc {
 rwa_buffer_set_mirror(&storage,NULL);buffer_textures_forget(&storage);
 if(deallocator){deallocator(storage.mirror,storage.size);[deallocator release];}
 free(storage.map);released_buffers++;[super dealloc];
}
@end
'''

MAIN = r'''
static IOSurfaceRef surface(size_t row,size_t offset) {
 IOSurfaceRef s=IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth:@17,(id)kIOSurfaceHeight:@5,
  (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@(row),(id)kIOSurfaceOffset:@(offset),
  (id)kIOSurfacePixelFormat:@((uint32_t)'2C0h'),(id)kIOSurfaceAllocSize:@(offset+row*5+32)});
 if(!s)abort();
 described=(struct ann_surface){.surface=IOSurfaceGetID(s),.size=IOSurfaceGetAllocSize(s),
 .pixelFormat=IOSurfaceGetPixelFormat(s),.planes=IOSurfaceGetPlaneCount(s)};return s;
}
static void sweep(AnnibaleMetalDevice *dev,MTLTextureDescriptor *d) {
 @autoreleasepool {
 IOSurfaceRef s=surface(256,0);id t=[dev newTextureWithDescriptor:d iosurface:s plane:0];[t release];CFRelease(s);
 }
}
int main(void) { @autoreleasepool {
 struct rwa_device gpu={0};AnnibaleMetalDevice *dev=[AnnibaleMetalDevice new];dev->_mesa=&gpu;
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:65 width:17 height:5 mipmapped:NO];
 d.usage=MTLTextureUsageShaderRead;
 IOSurfaceRef s=surface(128,64);char why[512];struct ann_surface selected;
 check("describe keeps exact Unsupported",(kernel_result=kIOReturnUnsupported,ann_surface_describe(1,1,&selected,why,sizeof(why)))==(int)kIOReturnUnsupported);
 check("take keeps failure contract",ann_surface_take(1,1,&selected,why,sizeof(why))==-1);
 unsigned old=buffers;
 id a=[dev newTextureWithDescriptor:d iosurface:s plane:0];
 check("aligned wrapped import uses buffer",a!=nil && buffers==old+1 && imports==0);
 if(!a)return 1;
 id b=[dev newTextureWithDescriptor:d iosurface:s plane:0];
 check("same surface wrappers share storage",a==b && buffers==old+1);
 AnnibaleMetalTexture *t=a;struct rwa_buffer_texture *back=t->_texture->backing;
 uint8_t *base=(uint8_t *)IOSurfaceGetBaseAddress(s)-64;size_t n=IOSurfaceGetAllocSize(s);
 memset(base,0x5a,n);rwa_buffer_textures_refresh(&gpu);
 check("CPU edits refresh active pixels",t->_texture->pixels[0]==0x5a && t->_texture->pixels[339]==0x5a);
 memset(t->_texture->pixels,0x26,340);back->written=true;rwa_buffer_textures_writeback(&gpu);
 bool pixels=true,guards=true;
 for(size_t i=0;i<n;i++) {
  bool active=i>=64 && i<64+128*5 && (i-64)%128<68;
  if(active)pixels&=base[i]==0x26;else guards&=base[i]==0x5a;
 }
 check("GPU completion writes active pixels",pixels);check("writeback preserves prefix suffix padding in model",guards);
 check("second wrapper sees writes",((AnnibaleMetalTexture *)b)->_texture->pixels[0]==0x26);
 d.usage=MTLTextureUsageShaderWrite;
 id c=[dev newTextureWithDescriptor:d iosurface:s plane:0];
 check("wider usage creates a supported wrapper",c!=nil && ([c usage]&MTLTextureUsageShaderWrite));
 struct rwa_texture *other=((AnnibaleMetalTexture *)c)->_texture;
 memset(other->pixels,0x39,340);other->backing->written=true;
 rwa_buffer_textures_writeback(&gpu);rwa_buffer_textures_refresh(&gpu);
 check("different usage wrapper observes completed writes",t->_texture->pixels[0]==0x39 && t->_texture->pixels[339]==0x39);
 [c release];d.usage=MTLTextureUsageShaderRead;
 size_t off=back->offset;back->offset=n;
 check("past-end rows reject",buffer_texture_upload(back,why,sizeof(why))==-1);back->offset=off;
 check("surface retained by live texture",CFGetRetainCount(s)>1);
 [a release];[b release];sweep(dev,d);
 check("unused cache releases buffer once",released_buffers==2 && released_textures==2);
 check("surface retains returned after cache sweep",CFGetRetainCount(s)==1);
 kernel_result=0;imports=0;old=buffers;
 a=[dev newTextureWithDescriptor:d iosurface:s plane:0];
 check("regular aligned import unchanged",a!=nil && imports==1 && buffers==old);[a release];
 const int errors[]={kIOReturnNotFound,kIOReturnNoMemory,kIOReturnNotReady,kIOReturnBadArgument};
 for(unsigned i=0;i<4;i++){kernel_result=errors[i];old=buffers;
  a=[dev newTextureWithDescriptor:d iosurface:s plane:0];
  check("other kernel failure rejects without copy",a==nil && buffers==old);[a release];}
 kernel_result=0;short_reply=true;old=buffers;
 check("malformed reply rejects without copy",[dev newTextureWithDescriptor:d iosurface:s plane:0]==nil && buffers==old);short_reply=false;
 mismatch=true;check("kernel geometry mismatch rejects",[dev newTextureWithDescriptor:d iosurface:s plane:0]==nil);mismatch=false;
 connection=0;check("missing connection rejects",[dev newTextureWithDescriptor:d iosurface:s plane:0]==nil);connection=1;
 no_import=true;check("missing backend rejects",[dev newTextureWithDescriptor:d iosurface:s plane:0]==nil);no_import=false;
 reject_import=true;check("backend allocation failure rejects",[dev newTextureWithDescriptor:d iosurface:s plane:0]==nil);reject_import=false;
 kernel_result=kIOReturnUnsupported;d.width=18;old=buffers;
 check("oversized texture rejects before copy",[dev newTextureWithDescriptor:d iosurface:s plane:0]==nil && buffers==old);d.width=17;
 check("invalid plane rejects before copy",[dev newTextureWithDescriptor:d iosurface:s plane:1]==nil && buffers==old);
 d.pixelFormat=80;check("wrong format rejects",[dev newTextureWithDescriptor:d iosurface:s plane:0]==nil);d.pixelFormat=65;
 reject_buffer=true;check("buffer allocation failure rejects",[dev newTextureWithDescriptor:d iosurface:s plane:0]==nil);reject_buffer=false;
 reject_texture=true;check("texture allocation failure rejects",[dev newTextureWithDescriptor:d iosurface:s plane:0]==nil);reject_texture=false;
 CFRelease(s);
 printf("RESULT checks=%u failures=%u\n",checks,failures);return failures?1:0;
} }
'''


def source(driver, air):
    drawable = (driver / 'annibaledrawable.m').read_text()
    texture = (driver / 'annibalemetal_texture.inc').read_text()
    metal = (driver / 'annibalemetal.m').read_text()
    mir = (air / 'air_metal_texture.inc').read_text()
    code = PREFIX + body(metal, 'struct rwa_texture_shape {') + ';\n'
    for ret, marker in [('static bool','mirror_read('), ('static void','buffer_mirror_pull('),
        ('void','rwa_buffer_set_mirror(struct rwa_buffer *buffer, void *mirror)\n{'),
        ('static char *','buffer_texture_bytes('), ('static uint64_t','rows_hash('),
        ('static int','buffer_texture_upload('), ('static int','buffer_texture_download('),
        ('static void','buffer_textures_forget('), ('void','rwa_buffer_textures_refresh('),
        ('void','rwa_buffer_textures_writeback(')]:
        code += '\n' + ret + '\n' + body(mir, marker) + '\n'
    code += CLASSES
    for marker in ['ann_surface_call(', 'ann_surface_describe(', 'ann_surface_take(']:
        code += '\nint\n' + body(drawable, marker) + '\n'
    code += drawable[drawable.index('struct ann_format {'):drawable.index('/* IOSurface offsets')]
    for ret, marker in [('uint32_t','ann_surface_id('), ('int','ann_surface_fits_adreno('),
        ('int','ann_surface_plane('), ('struct rwa_texture *','ann_texture_for_surface(')]:
        code += '\n' + ret + '\n' + body(drawable,marker) + '\n'
    code += '\nstatic id\n' + body(texture,'rw_surface_through_copy(') + '\n'
    code += '@implementation AnnibaleMetalDevice\n'
    code += r'''
- (id)newBufferWithBytesNoCopy:(void *)bytes length:(NSUInteger)n options:(NSUInteger)o deallocator:(void(^)(void *,NSUInteger))block {
 (void)o;if(reject_buffer)return nil;buffers++;
 AnnibaleMetalBuffer *b=[AnnibaleMetalBuffer new];b->owner=self;
 b->storage=(struct rwa_buffer){.device=_mesa,.size=n,.map=malloc(n)};
 memcpy(b->storage.map,bytes,n);rwa_buffer_set_mirror(&b->storage,bytes);b->deallocator=[block copy];return b;
}
'''
    code += body(texture,'- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:') + '\n@end\n'
    code += '@implementation AnnibaleMetalTexture\n'
    for marker in ['- (instancetype)initWithAnnibaleDevice:', '- (void)dealloc', '- (void)annibaleSetBuffer:',
        '- (void)annibaleSetSurface:', '- (NSUInteger)usage']:
        code += body(texture,marker) + '\n'
    return code + '@end\n' + MAIN


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver',type=Path,default=ROOT/'tools/userland-driver')
    parser.add_argument('--air',type=Path,default=ROOT.parent/'hackios-xnu-userland/tools/mesa/air')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='wrapped-surface-') as tmp:
        p=Path(tmp);(p/'test.m').write_text(source(args.driver,args.air))
        subprocess.run(['xcrun','-sdk','macosx','clang','-fno-objc-arc','-fblocks','-fsanitize=address,undefined',
            '-Wno-unused-function','-Wno-unused-variable','-framework','Foundation','-framework','Metal',
            '-framework','IOSurface','-framework','IOKit','-I'+str(args.driver),str(p/'test.m'),'-o',str(p/'test')],check=True)
        return subprocess.run([str(p/'test')],timeout=30).returncode

if __name__=='__main__':
    raise SystemExit(main())

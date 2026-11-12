#!/usr/bin/env python3
"""Execute production commit, sync, thumbnail and matching code with a host Mesa fixture."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=Path, default=ROOT)
args, unittest_args = parser.parse_known_args()
DRIVER = args.source_root / 'tools/userland-driver'


def body(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    depth = 0
    for token in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', text[opening:], re.S):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return text[start:opening + token.end()]
    raise ValueError(marker)


PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "annibalemetal_work_private.h"
struct rwa_device { int unused; };
struct rwa_texture { int unused; };
struct rwa_buffer { int unused; };
struct rwa_sampler { int unused; };
struct rwa_render { int unused; };
struct rwa_compute { int unused; };
struct rwa_buffer_bind { struct rwa_buffer *buffer; size_t offset; };
struct rwa_bindings {
 struct rwa_buffer **buffers; unsigned buffer_count;
 struct rwa_texture **textures; unsigned texture_count;
 struct rwa_sampler **samplers; unsigned sampler_count;
};
static char events[10000];
static unsigned at, draw_calls, waits, thumbs, dumps, copies, async_calls, sync_calls;
static unsigned returned_slots, completions;
static bool draw_error, finish_error, read_error;
static bool partial_read, sparse;
static void event(char c) { events[at++]=c; events[at]=0; }
static void rw_print(const char *format, ...) {
 va_list ap; va_start(ap,format); vprintf(format,ap); va_end(ap);
}
#define RW_ONCE(...) rw_print(__VA_ARGS__)
static void air_dump(const void *p, NSUInteger n, const char *s, const char *f, bool c) {
 (void)p;(void)n;(void)s;(void)f;(void)c; dumps++;
}
static int flush(struct rwa_device *d,char *why,size_t n) { (void)d;(void)why;(void)n;event('F');return 0; }
static int finish(struct rwa_device *d,char *why,size_t n) {
 (void)d; waits++;event('W');snprintf(why,n,"fixture finish");return finish_error ? -1:0;
}
static int thumbnail(struct rwa_texture *t,unsigned w,unsigned h,uint8_t *p) {
 (void)t; thumbs++;event('T');memset(p,0,w*h*4);return read_error || (partial_read && thumbs==2) ? -1:0;
}
static int dispatch_async_mock(struct rwa_compute *p,const struct rwa_bindings *b,
 const struct rwa_buffer_bind *t,unsigned n,const unsigned g[3],const unsigned s[3],unsigned bytes,char *why,size_t z) {
 (void)p;(void)b;(void)t;(void)n;(void)g;(void)s;(void)bytes;(void)why;(void)z;async_calls++;event('A');return 0;
}
static int dispatch_sync_mock(struct rwa_compute *p,const struct rwa_bindings *b,
 const struct rwa_buffer_bind *t,unsigned n,const unsigned g[3],const unsigned s[3],unsigned bytes,char *why,size_t z) {
 sync_calls++;event('S');return 0;
}
static struct {
 int (*flush)(struct rwa_device *,char *,size_t);
 int (*finish)(struct rwa_device *,char *,size_t);
 int (*texture_thumbnail)(struct rwa_texture *,unsigned,unsigned,uint8_t *);
 void (*buffer_textures_refresh)(struct rwa_device *);
 __typeof__(&dispatch_async_mock) dispatch_nowait,dispatch_threadgroup;
 int (*dispatch_buffers)(struct rwa_compute *,const struct rwa_bindings *,const struct rwa_buffer_bind *,unsigned,const unsigned *,const unsigned *,char *,size_t);
 int (*dispatch)(struct rwa_compute *,const struct rwa_bindings *,const unsigned *,const unsigned *,char *,size_t);
} mesa={.flush=flush,.finish=finish,.texture_thumbnail=thumbnail,
 .dispatch_nowait=dispatch_async_mock,.dispatch_threadgroup=dispatch_sync_mock};
@interface FixtureResource : NSObject
- (struct rwa_render *)render;
- (struct rwa_compute *)compute;
- (struct rwa_texture *)mesaTexture;
- (struct rwa_buffer *)mesaBuffer;
- (struct rwa_sampler *)mesaSampler;
@end
@implementation FixtureResource
- (struct rwa_render *)render { return (void *)self; }
- (struct rwa_compute *)compute { return (void *)self; }
- (struct rwa_texture *)mesaTexture { return (void *)self; }
- (struct rwa_buffer *)mesaBuffer { return (void *)self; }
- (struct rwa_sampler *)mesaSampler { return (void *)self; }
- (NSUInteger)width { return 194; }
- (NSUInteger)height { return 194; }
- (NSUInteger)pixelFormat { return 80; }
- (NSUInteger)sampleCount { return 1; }
- (id)buffer { return nil; }
- (void *)iosurface { return NULL; }
@end
@interface FixtureData : NSObject @end
@implementation FixtureData
- (id)copyWithZone:(NSZone *)z { (void)z; copies++;return [self retain]; }
- (const void *)bytes { return "abcd"; }
- (NSUInteger)length { return 4; }
@end
static int no_sleep(unsigned n) { (void)n;return 0; }
#define usleep no_sleep
'''
SUPPORT = r'''
static char rw_last_draw[128];
static void rw_describe_draw(AnnibaleMetalWork *w,const void *r,unsigned c,unsigned i,bool say) {
 (void)w;snprintf(rw_last_draw,sizeof(rw_last_draw),"draw %u/%u %p",c,i,r);
 if(say) rw_print("%s\n",rw_last_draw);
}
static bool rw_audit_frame_active(unsigned c) { (void)c;return false; }
#define rw_audit_commit_begin(...) ((void)0)
#define rw_audit_render_work(...) ((void)0)
#define rw_audit_commit_end(...) ((void)0)
#define rw_work_time(...) ((void)0)
#define rw_timing_commit(...) ((void)0)
#define rw_pipeline_dump(...) ((void)0)
#define RW_COMMIT_SLOW_US 20000
#define RW_DRAW_SLOW_US 50000
#define RW_FLUSH_SLOW_US 50000
#define RW_TRACE_COMMITS 0
static struct { unsigned count; uint64_t us; } rw_work_stats[3];
enum { RW_WORK_DRAW,RW_WORK_COPY,RW_WORK_DISPATCH };
enum { RW_CB_COMMITTED=2,RW_CB_COMPLETED=4,RW_CB_ERROR=5 };
static unsigned commit_count,commit_in_flight,commit_phase;
static uint64_t commit_started,commit_progress_us;
static pthread_once_t rw_watch_once=PTHREAD_ONCE_INIT;
static void rw_start_watch(void) {}
static uint64_t rw_now_us(void) { return 1; }
static uint64_t rw_ticks_us(uint64_t t) { (void)t;return 1; }
static int rw_run_copy(AnnibaleMetalWork *w,char *why,size_t n) { (void)w;(void)why;(void)n;event('C');return 0; }
@interface FixtureQueue : NSObject
- (id)device;
- (struct rwa_device *)mesaDevice;
@end
@implementation FixtureQueue
- (id)device { return self; }
- (struct rwa_device *)mesaDevice { return (void *)self; }
@end
@interface AnnibaleMetalCommandBuffer : NSObject {
 @public FixtureQueue *_queue; NSMutableArray *_work; unsigned _phase,_status; bool _failed;
}
- (void)commit;
- (BOOL)runDraw:(AnnibaleMetalWork *)w why:(char *)why size:(size_t)n;
- (void)thumbWork:(AnnibaleMetalWork *)w index:(unsigned)i after:(BOOL)after;
@end
@implementation AnnibaleMetalCommandBuffer
- (id)init { self=[super init];_queue=[FixtureQueue new];_work=[NSMutableArray new];return self; }
- (void)dealloc { [_queue release];[_work release];[super dealloc]; }
- (void)annibaleReturnSlot { returned_slots++; }
- (void)annibaleCompleteLater:(unsigned)c { (void)c;completions++; }
- (void)failWith:(const char *)why { (void)why;_failed=true; }
- (BOOL)runDraw:(AnnibaleMetalWork *)w why:(char *)why size:(size_t)n {
 draw_calls++;event('D');snprintf(why,n,"fixture draw");
 if(draw_error) { rw_dump_refused_air([w->render render]);[self failWith:why];return NO; }return YES;
}
'''
FIXTURE = r'''
@end
static FixtureResource *matching,*other,*texture;
static FixtureData *data;
static void keep(FixtureResource *p,const char *name) { rw_keep_air([p render],(id)data,"vertex",(id)data,name); }
static AnnibaleMetalWork *draw(FixtureResource *p) {
 AnnibaleMetalWork *w=[AnnibaleMetalWork new];w->render=(id)p;w->colorCount=1;
 w->color[0]=texture;w->textures[0]=texture;w->textureCount=1;
 if(sparse) {
  w->color[0]=nil;w->textures[0]=nil;w->colorCount=3;w->textureCount=3;
  w->color[2]=texture;w->textures[2]=texture;w->depth=texture;
 }
 [w retainResources];return [w autorelease];
}
static AnnibaleMetalCommandBuffer *buffer(NSArray *works) {
 AnnibaleMetalCommandBuffer *b=[AnnibaleMetalCommandBuffer new];[b->_work addObjectsFromArray:works];return [b autorelease];
}
static void *commit_thread(void *b) { @autoreleasepool { [(id)b commit]; }return NULL; }
static void *publish_thread(void *unused) {
 (void)unused;@autoreleasepool { for(unsigned i=0;i<1000;i++) keep(matching,(i&1)?"target":"other"); }return NULL;
}
int main(int argc,char **argv) { @autoreleasepool {
 const char *scenario=argc>1?argv[1]:"ordinary";
 matching=[FixtureResource new];other=[FixtureResource new];texture=[FixtureResource new];data=[FixtureData new];
 keep(matching,"target");keep(other,"other");
 if(!strcmp(scenario,"reuse-other")) keep(matching,"other");
 if(!strcmp(scenario,"reuse-match")) {keep(matching,"other");keep(matching,"target");}
 if(!strcmp(scenario,"reuse-different")) {keep(matching,"other");keep(other,"target");}
 if(!strcmp(scenario,"finish-error")) finish_error=true;
 if(!strcmp(scenario,"draw-error")) draw_error=true;
 if(!strcmp(scenario,"read-error")) read_error=true;
 if(!strcmp(scenario,"partial-read")) partial_read=true;
 if(!strcmp(scenario,"sparse")) sparse=true;
 if(!strcmp(scenario,"long-name")) {
  char name[200];memset(name,'x',sizeof(name));memcpy(name+180,"target",7);keep(matching,name);
 }
 if(!strcmp(scenario,"no-finish")) mesa.finish=NULL;
 if(!strcmp(scenario,"no-flush")) mesa.flush=NULL;
 if(!strcmp(scenario,"many")) {
  NSMutableArray *works=[NSMutableArray array];for(unsigned i=0;i<20;i++) [works addObject:draw(matching)];
  [buffer(works) commit];
 } else if(!strcmp(scenario,"table-full")) {
  for(unsigned i=0;i<256;i++) rw_keep_air((void *)(uintptr_t)(i+1),(id)data,"v",(id)data,"target");
  keep(other,"target");[buffer(@[draw(other)]) commit];
 } else if(!strcmp(scenario,"compute-copy")) {
  AnnibaleMetalWork *c=[AnnibaleMetalWork new];c->pipeline=(id)matching;
  AnnibaleMetalWork *copy=[AnnibaleMetalWork new];copy->copyKind=RW_COPY_CLEAR;
  [buffer(@[copy,c,draw(other),draw(matching)]) commit];[c release];[copy release];
 } else if(!strcmp(scenario,"threads")) {
  id a=buffer(@[draw(matching),draw(matching)]),b=buffer(@[draw(matching),draw(matching)]);
  pthread_t t1,t2;pthread_create(&t1,NULL,commit_thread,a);pthread_create(&t2,NULL,commit_thread,b);
  pthread_join(t1,NULL);pthread_join(t2,NULL);
 } else if(!strcmp(scenario,"publish")) {
  pthread_t thread;pthread_create(&thread,NULL,publish_thread,NULL);
  for(unsigned i=0;i<1000;i++) (void)rw_thumb_match([matching render]);pthread_join(thread,NULL);
  keep(matching,"target");[buffer(@[draw(matching)]) commit];
 } else {
  id a=buffer(@[draw(other),draw(matching),draw(matching)]);[a commit];[a commit];
  [buffer(@[draw(matching)]) commit];
 }
 rw_dump_kept_air([matching render],"fixture");
 printf("RESULT draws=%u waits=%u thumbs=%u dumps=%u copies=%u async=%u sync=%u slots=%u complete=%u events=%s\n",
  draw_calls,waits,thumbs,dumps,copies,async_calls,sync_calls,returned_slots,completions,events);
 [matching release];[other release];[texture release];[data release];
 }return 0; }
'''


class MatchedSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        diag = (DRIVER / 'annibalemetal_diag.inc').read_text()
        command = (DRIVER / 'annibalemetal_commandbuffer.inc').read_text()
        sync = body(diag, 'static bool\nrw_sync_draws(')
        if 'static unsigned rw_matched_left;' in diag:
            sync += '\n' + diag[diag.index('static unsigned rw_matched_left;'):diag.index('/*\n * Without ANNIBALE_MSAA')]
        helpers = diag[diag.index('#define RW_KEPT_PIPELINES'):diag.index('/* One attachment:')]
        helpers += '\n' + body(diag, 'static bool\nrw_sync_log_wanted(')
        helpers += '\n' + body(diag, 'static void\nrw_dump_kept_air(')
        helpers += '\n' + body(diag, 'static void\nrw_dump_refused_air(')
        methods = body(command, '- (void)commit\n')
        methods += '\n' + body(command, '- (BOOL)syncDraw:')
        methods += '\n' + body(command, '- (void)thumbWork:')
        source = PREFIX + '\n' + sync + '\n' + helpers + '\n' + SUPPORT
        source += '\nstatic pthread_mutex_t rw_commit_lock=PTHREAD_RECURSIVE_MUTEX_INITIALIZER;\n' + methods + '\n' + FIXTURE
        # Use the actual work ownership methods; the test command buffer has no Metal private superclass.
        work = (DRIVER / 'annibalemetal_work.inc').read_text().split('/* Apple\'s own')[0]
        source = source.replace(SUPPORT, work + '\n' + SUPPORT)
        cls.temp = tempfile.TemporaryDirectory(prefix='metal-matched-sync-')
        cls.exe = Path(cls.temp.name) / 'test'
        path = cls.exe.with_suffix('.m'); path.write_text(source)
        result = subprocess.run(['xcrun','clang','-fno-objc-arc','-fblocks','-fsanitize=address,undefined',
            '-Wno-incompatible-pointer-types','-Wno-objc-method-access','-framework','Foundation','-framework','Metal',
            '-I',str(DRIVER),str(path),'-o',str(cls.exe)],capture_output=True,text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, scenario='ordinary', matched='2', thumbs='6', **settings):
        env = {k:v for k,v in os.environ.items() if not k.startswith('ANNIBALE_')}
        if matched is not None: env['ANNIBALE_SYNC_MATCHED_DRAWS']=matched
        env.update(ANNIBALE_THUMB_MATCH='target',ANNIBALE_THUMB_DRAWS='1',ANNIBALE_THUMBS=thumbs,
                   ANNIBALE_THUMB_SIZE='8',ANNIBALE_THUMB_ONLY='pre0,out0,in0')
        env.update(settings)
        env={k:v for k,v in env.items() if v is not None}
        result = subprocess.run([str(self.exe),scenario],env=env,capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr+result.stdout)
        line=next(line for line in result.stdout.splitlines() if line.startswith('RESULT '))
        fields=dict(re.findall(r'(\w+)=(\w*)',line))
        return {k:int(v) if k!='events' else v for k,v in fields.items()},result.stdout

    def test_default(self):
        r,_=self.run_case(matched=None);self.assertEqual((r['draws'],r['waits'],r['thumbs'],r['copies']),(4,0,0,0))
    def test_old_global(self):
        r,_=self.run_case(matched=None,ANNIBALE_SYNC_DRAWS='0')
        self.assertEqual((r['waits'],r['thumbs'],r['copies']),(4,6,4))
    def test_old_precedence(self):
        r,out=self.run_case(ANNIBALE_SYNC_DRAWS='1');self.assertEqual(r['waits'],4);self.assertIn('takes precedence',out)
    def test_two_attempts(self):
        r,_=self.run_case();self.assertEqual((r['draws'],r['waits'],r['thumbs'],r['copies'],r['dumps']),(4,2,6,0,0))
        self.assertEqual(r['events'],'DTDFWTTTDFWTTFDF')
    def test_budget_across_commits(self):
        r,_=self.run_case(matched='3',thumbs='9');self.assertEqual((r['waits'],r['thumbs'],r['slots']),(3,9,2))
    def test_handle_reuse(self):
        for scenario,waits in [('reuse-other',0),('reuse-match',2),('reuse-different',1)]:
            with self.subTest(scenario=scenario):
                r,_=self.run_case(scenario);self.assertEqual((r['waits'],r['copies']),(waits,0))
    def test_budget_insufficient(self):
        r,out=self.run_case(thumbs='2');self.assertEqual((r['waits'],r['thumbs']),(2,0));self.assertIn('need 3 samples have 2',out)
    def test_budget_for_last_group(self):
        r,out=self.run_case(thumbs='4');self.assertEqual((r['waits'],r['thumbs']),(2,3));self.assertIn('need 3 samples have 1',out)
    def test_read_failure(self):
        r,out=self.run_case('read-error');self.assertEqual((r['waits'],r['thumbs']),(2,6));self.assertEqual(out.count('read failed'),6)
    def test_partial_read_failure(self):
        r,out=self.run_case('partial-read');self.assertEqual((r['waits'],r['thumbs']),(2,6));self.assertEqual(out.count('read failed'),1)
    def test_sparse_roles_and_depth(self):
        roles='pre2,out2,in2,pre8,out8'
        r,out=self.run_case('sparse',matched='1',thumbs='5',ANNIBALE_THUMB_ONLY=roles)
        self.assertEqual((r['waits'],r['thumbs']),(1,5));self.assertNotIn('incomplete',out)
        r,out=self.run_case('sparse',matched='1',thumbs='4',ANNIBALE_THUMB_ONLY=roles)
        self.assertEqual((r['waits'],r['thumbs']),(1,0));self.assertIn('need 5 samples have 4',out)
    def test_selected_roles(self):
        r,_=self.run_case(thumbs='4',ANNIBALE_THUMB_ONLY='out0,in0');self.assertEqual((r['waits'],r['thumbs']),(2,4))
    def test_no_draw_thumbnails(self):
        r,_=self.run_case(ANNIBALE_THUMB_DRAWS=None);self.assertEqual((r['waits'],r['thumbs']),(2,0))
    def test_maximum_attempts(self):
        r,_=self.run_case('many',matched='16',thumbs='0');self.assertEqual((r['draws'],r['waits']),(20,16))
    def test_table_capacity(self):
        r,out=self.run_case('table-full');self.assertEqual((r['waits'],r['copies']),(0,0));self.assertIn('table full',out)
    def test_long_name_matches_before_log_truncation(self):
        r,_=self.run_case('long-name');self.assertEqual(r['waits'],2)
    def test_draw_failure(self):
        r,out=self.run_case('draw-error');self.assertEqual((r['draws'],r['waits'],r['dumps']),(2,0,0));self.assertIn('incomplete, draw failed',out)
    def test_finish_failure(self):
        r,out=self.run_case('finish-error');self.assertEqual((r['draws'],r['waits'],r['dumps']),(3,2,0));self.assertIn('did not finish',out)
    def test_compute_copy(self):
        r,_=self.run_case('compute-copy');self.assertEqual((r['async'],r['sync'],r['waits']),(1,0,1));self.assertTrue(r['events'].startswith('CAD'))
        r,_=self.run_case('compute-copy',matched=None,ANNIBALE_SYNC_DRAWS='1');self.assertEqual((r['async'],r['sync'],r['waits']),(0,1,2))
    def test_missing_interfaces(self):
        for scenario in ['no-finish','no-flush']:
            r,_=self.run_case(scenario);self.assertEqual((r['waits'],r['thumbs']),(0,0))
    def test_invalid_configuration(self):
        for value in ['0','17','-1','1x','','999999999999999999999',' 2','+2']:
            with self.subTest(value=value):
                r,_=self.run_case(matched=value);self.assertEqual((r['waits'],r['thumbs'],r['copies']),(0,0,0))
        r,_=self.run_case(ANNIBALE_THUMB_MATCH='');self.assertEqual(r['waits'],0)
    def test_zero_thumbnail_budget(self):
        r,_=self.run_case(thumbs='0');self.assertEqual((r['waits'],r['thumbs']),(2,0))
    def test_whole_commit_does_not_expand(self):
        r,_=self.run_case(ANNIBALE_THUMB_WHOLE='1');self.assertEqual((r['waits'],r['thumbs']),(2,6))
    def test_from_texture_scissor_filters(self):
        r,_=self.run_case(ANNIBALE_THUMB_FROM='2');self.assertEqual(r['waits'],1)
        r,_=self.run_case(ANNIBALE_THUMB_TEXTURE='1x1');self.assertEqual(r['waits'],0)
        r,_=self.run_case(ANNIBALE_THUMB_SCISSOR='0,0,100,100');self.assertEqual(r['waits'],0)
    def test_two_submitters(self):
        r,_=self.run_case('threads');self.assertEqual((r['draws'],r['waits'],r['thumbs']),(4,2,6))
    def test_concurrent_publication(self):
        r,_=self.run_case('publish');self.assertEqual((r['waits'],r['copies']),(1,0))


if __name__ == '__main__':
    unittest.main(argv=[__file__,*unittest_args])

#!/usr/bin/env python3
"""Execute production microphone and reply-copy functions with host hardware stubs."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MIC = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_mic.c').read_text()
REACH = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_audio_reach.c').read_text()


def function(source, name):
    match = re.search(r'(?:static\s+)?(?:bool|void|uint32_t|int)\s+' + name + r'\([^;]*?\)\s*\{', source)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/types.h>
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define __builtin_arm_dsb(x) ((void)0)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define say(...) ((void)0)
#define NSEC_PER_SEC 1000000000u
#define NSEC_PER_MSEC 1000000u
#define NSEC_PER_USEC 1000u
#define IO_NDELAY 1
#define BUFFER_BYTES 65536
#define CMD_OFFSET 0xc000
struct proc { int unused; };
#ifndef __APPLE__
typedef intptr_t user_ssize_t;
#endif
typedef uintptr_t vm_offset_t;
typedef int wait_result_t;
struct uio { int16_t data[2048]; user_ssize_t remaining, offset; };
static user_ssize_t uio_resid(struct uio *u) { return u->remaining; }
static unsigned moves;
static int uiomove(char *src, int len, struct uio *u) {
    memcpy((char *)u->data+u->offset,src,len); u->offset+=len;u->remaining-=len;moves++;return 0;
}
static uint64_t mach_absolute_time(void) { return 1000000000; }
static void absolutetime_to_nanoseconds(uint64_t a,uint64_t *n) { *n=a; }
static bool thread_test;
static jmp_buf thread_exit;
static void delay_for_interval(uint32_t n,uint32_t unit) {
    if(thread_test && n==20 && unit==NSEC_PER_MSEC) longjmp(thread_exit,1);
}
static int amic_lock, cmd_lock, spin;
static int *amic_ring_lock=&spin, *cmd_oob_lock=&cmd_lock;
static int locked;
static void lck_mtx_lock(void *p) { (void)p;locked++; }
static void lck_mtx_unlock(void *p) { (void)p;assert(locked>0);locked--; }
static void lck_spin_lock(void *p) { (void)p; }
static void lck_spin_unlock(void *p) { (void)p; }
static uint32_t tlmm_memory[0x150000/4], lpi_memory[0x30000/4];
static vm_offset_t tlmm_base;
static bool hw_up=true;
static unsigned rail_calls,map_calls,fail_rail,fail_map,commands,fail_command;
static bool annibale_adsp_is_hw_up(void) { return hw_up; }
static bool annibale_rpmh_regulator(const char *name,uint32_t mv,bool on,uint32_t mode) {
    (void)name;(void)mv;(void)on;(void)mode;return ++rail_calls!=fail_rail;
}
static vm_offset_t ml_io_map(uintptr_t at,uintptr_t size) {
    (void)size;if(++map_calls==fail_map)return 0;
    return (uintptr_t)(at==0x0f100000UL ? tlmm_memory:lpi_memory);
}
static int16_t storage[65536], *mic_ring=storage;
static uint32_t mic_ring_head,mic_ring_tail,mic_open_clients,mic_token;
static bool mic_graphs_started,mic_wcd_powered;
static int mic_capture_error;
static uint64_t last_blocking_read_ns;
static bool recording=true,dma_available=true;
static uint32_t fed_frames;
static int16_t fed[1920];
static bool annibale_audio_recording_mic(void) { return recording; }
static void annibale_audio_record_fed(const int16_t *samples,uint32_t frames) {
    assert(frames<=960);memcpy(fed,samples,frames*4);fed_frames=frames;
}
struct gpr_pkt { uint8_t buf[1024]; uint32_t len; };
static uint8_t dma_buffer[65536];
static volatile uint8_t *reach_buffer=dma_buffer;
static uint32_t map_handle=0x4321;
static uint64_t dsp_address=0x1cf66f000ULL;
static unsigned fault,read_requests;
static uint32_t reply_bytes=1920;
static bool annibale_audio_reach_dma_info(volatile uint8_t **va,uint64_t *pa,uint64_t *dsp,uint32_t *handle) {
    *va=dma_buffer;*pa=0xcf66f000;*dsp=dsp_address;*handle=map_handle;return dma_available;
}
static bool annibale_adsp_gpr_send(const void *raw,uint32_t len,uint32_t wait,
    uint32_t *op,uint32_t *words,uint32_t count) {
    const uint32_t *p=raw;(void)wait;
    commands++;if(commands==fail_command)return false;
    if(p[5]!=0x0400100b) { *op=0x02001005;return true; }
    read_requests++;
    assert(len==56 && (p[0]>>8)==len && p[2]==0x2002 && p[3]==0x6021);
    assert(count>=14 && p[9]==1920);
    for(unsigned i=10;i<14;i++)assert(p[i]==0);
    *op=0x05001005;
    words[0]=0;words[1]=p[6];words[2]=p[7];words[3]=p[8];words[4]=reply_bytes;
    words[8]=1; /* num_frames is not a status code */
    words[9]=0;words[13]=0;
    if(fault==1)return false;
    if(fault==2)*op=0x02001005;
    if(fault==3)words[0]=1;
    if(fault==4)words[1]++;
    if(fault==5)words[2]++;
    if(fault==6)words[3]++;
    if(fault==7)words[4]=1922;
    if(fault==8)words[4]=3;
    if(fault==9)words[9]=1;
    if(fault==10)words[13]=4;
    return true;
}
'''
MAIN = r'''
static void reset(void) {
    assert(locked==0);thread_test=false;tlmm_base=0;hw_up=true;
    rail_calls=map_calls=fail_rail=fail_map=commands=fail_command=read_requests=0;
    mic_wcd_powered=mic_graphs_started=false;mic_capture_error=0;mic_open_clients=0;
    mic_ring=storage;mic_ring_head=mic_ring_tail=0;moves=fed_frames=0;
    fault=0;reply_bytes=1920;recording=dma_available=true;
    memset(storage,0x55,sizeof(storage));memset(dma_buffer,0,sizeof(dma_buffer));
}
static void once(void) {
    thread_test=true;if(setjmp(thread_exit)==0)mic_capture_thread(NULL,0);thread_test=false;
    assert(locked==0);
}
int main(void) {
    for(unsigned failure=1;failure<=3;failure++) {
        reset();fail_rail=failure;
        assert(!wcd9395_power_and_reset());assert(!mic_wcd_powered);
        assert(rail_calls==failure && commands==0 && map_calls==0);
    }
    for(unsigned failure=1;failure<=2;failure++) {
        reset();fail_map=failure;
        assert(!wcd9395_power_and_reset() && !mic_wcd_powered);
    }
    reset();hw_up=false;assert(!wcd9395_power_and_reset() && !mic_wcd_powered && rail_calls==0);
    reset();fail_command=1;assert(!wcd9395_power_and_reset() && !mic_wcd_powered);
    reset();assert(wcd9395_power_and_reset() && mic_wcd_powered);
    unsigned saved=rail_calls;assert(wcd9395_power_and_reset() && rail_calls==saved);
    for(unsigned failure=1;failure<=9;failure++) {
        reset();fail_command=failure;
        assert(!start_capture_graphs() && !mic_graphs_started && commands==failure);
    }
    reset();assert(start_capture_graphs() && mic_graphs_started && commands==9);
    for(unsigned i=0;i<=10;i++) {
        reset();fault=i;uint32_t bytes=0;
        assert(submit_capture_slot(2,dsp_address,map_handle,&bytes)==(i==0));
        assert(bytes==(i==0 ? 1920u:0u));
    }
    reset();reply_bytes=4;uint32_t bytes=0;
    assert(submit_capture_slot(2,dsp_address,map_handle,&bytes) && bytes==4);
    int16_t *hw=(int16_t *)(dma_buffer+MIC_OFFSET+2*MIC_PERIOD_BYTES);
    hw[0]=123;hw[1]=-456;hw[2]=789;
    harvest_capture_period(2,dma_buffer,bytes);
    assert(mic_ring_head==2 && storage[0]==123 && storage[1]==-456 && storage[2]==0x5555);
    assert(fed_frames==2 && fed[0]==123 && fed[1]==123 && fed[2]==-456 && fed[3]==-456);
    reset();harvest_capture_period(0,dma_buffer,1920);
    assert(mic_ring_head==960);for(unsigned i=0;i<960;i++)assert(storage[i]==0);
    reset();fail_command=1;once();assert(read_requests==0 && mic_ring_head==0);
    reset();mic_graphs_started=true;mic_wcd_powered=true;fault=9;once();
    assert(mic_capture_error==EIO && mic_ring_head==0 && read_requests==1);
    once();assert(read_requests==1);
    struct uio u={.remaining=8};
    assert(annibale_mic_read(&u,IO_NDELAY)==EIO && moves==0);
    assert(mic_cdev_open(0,0,0,NULL)==EIO && mic_open_clients==0);
    reset();fail_command=1;u=(struct uio){.remaining=8};
    assert(annibale_mic_read(&u,IO_NDELAY)==EIO && moves==0);
    reset();fail_command=1;
    assert(mic_cdev_open(0,0,0,NULL)==EIO && mic_open_clients==0);
    reset();assert(mic_cdev_open(0,0,0,NULL)==0 && mic_open_clients==1);
    u=(struct uio){.remaining=8};assert(annibale_mic_read(&u,IO_NDELAY)==0);
    for(unsigned i=0;i<4;i++)assert(u.data[i]==0);
    reset();mic_ring=NULL;u=(struct uio){.remaining=8};assert(annibale_mic_read(&u,0)==ENXIO);
    assert(locked==0);
    puts("PASS: production power/graph failures, READ_V2 replies, exact PCM extent and open/read errors");
}
'''


def main():
    constants = MIC[MIC.index('#define TLMM_PHYS'):MIC.index('static LCK_GRP_DECLARE')]
    builders = MIC[MIC.index('struct mic_pkt {'):MIC.index('/* ---- WCD9395')]
    source = HEADER + constants
    source += function(REACH, 'gpr_set32') + function(REACH, 'gpr_finish')
    source += function(REACH, 'annibale_audio_reach_send_cmd') + builders
    source += ''.join(function(MIC, name) for name in (
        'wcd9395_power_and_reset', 'start_capture_graphs', 'submit_capture_slot',
        'harvest_capture_period', 'mic_capture_thread', 'annibale_mic_read', 'mic_cdev_open'))
    source += MAIN
    mutations = (
        ('wrong metadata status', 'words[9] != 0', 'words[8] != 0'),
        ('truncate reply copy', 'MIN(max_words, 16u)', 'MIN(max_words, 8u)'),
        ('accept odd byte count', '(words[4] % sizeof(int16_t)) != 0', 'false'),
        ('publish stale DMA tail', 'i < valid_samples; i++) {\n\t\tif ((mic_ring_head', 'i < MIC_PERIOD_FRAMES; i++) {\n\t\tif ((mic_ring_head'),
        ('ignore power failure', 'if (!wcd9395_power_and_reset()) return false;', '(void)wcd9395_power_and_reset();'),
    )
    with tempfile.TemporaryDirectory(prefix='mic-production-') as directory:
        cfile, binary = Path(directory)/'test.c', Path(directory)/'test'
        for name, old, new in [('production', '', ''), *mutations]:
            if old:
                assert old in source, name
            cfile.write_text(source.replace(old, new, 1) if old else source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if old:
                assert result.returncode != 0, name
            else:
                assert result.returncode == 0, result.stderr
                print(result.stdout, end='')
        print(f'PASS: {len(mutations)} compiled production regressions rejected')


if __name__ == '__main__':
    main()

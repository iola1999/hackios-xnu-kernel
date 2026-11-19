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
#ifndef __dead2
#define __dead2
#endif
#ifndef __DEVOLATILE
#define __DEVOLATILE(type, var) ((type)(uintptr_t)(volatile void *)(var))
#endif
#define __builtin_arm_dsb(x) ((void)0)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define say(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define NSEC_PER_SEC 1000000000u
#define NSEC_PER_MSEC 1000000u
#define NSEC_PER_USEC 1000u
#define IO_NDELAY 1
#define BUFFER_BYTES 65536
#define CMD_OFFSET 0xc000
#define APM_CMD_GET_SPF_STATE 0x01001021
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
typedef void *event_t;
typedef void *thread_t;
#define THREAD_INTERRUPTIBLE 1
#define THREAD_AWAKENED 0
#define THREAD_CONTINUE_NULL NULL
static unsigned idle_waits,wakeups;
static int assert_wait_timeout(event_t e,int interruptible,uint32_t interval,uint32_t scale) {
    (void)e;(void)interruptible;(void)interval;(void)scale;idle_waits++;return 0;
}
static thread_t current_thread(void) { return NULL; }
static int clear_wait(thread_t t,int result) { (void)t;(void)result;return 0; }
/* an idle feeder blocks until the next open: the host run leaves the thread here */
static int thread_block(void *continuation) {
    (void)continuation;if(thread_test) longjmp(thread_exit,2);return 0;
}
static void thread_wakeup(event_t e) { (void)e;wakeups++; }
static int amic_lock, cmd_lock, spin;
static int *amic_ring_lock=&spin, *cmd_oob_lock=&cmd_lock;
static int locked;
static void lck_mtx_lock(void *p) { (void)p;locked++; }
static void lck_mtx_unlock(void *p) { (void)p;assert(locked>0);locked--; }
static void lck_spin_lock(void *p) { (void)p; }
static void lck_spin_unlock(void *p) { (void)p; }
static uint32_t tlmm_memory[0x150000/4], lpi_memory[0x30000/4], va_memory[0x2000/4];
static vm_offset_t tlmm_base;
static bool hw_up=true, reach_active=true;
static unsigned map_calls,fail_map,commands,fail_command;
static bool annibale_adsp_is_hw_up(void) { return hw_up; }
static bool annibale_audio_reach_is_active(void) { return reach_active; }
static vm_offset_t ml_io_map(uintptr_t at,uintptr_t size) {
    (void)size;if(++map_calls==fail_map)return 0;
    if(at==0x0f100000UL)return (uintptr_t)tlmm_memory;
    if(at==0x07660000UL)return (uintptr_t)va_memory;
    return (uintptr_t)lpi_memory;
}
static int16_t storage[65536], *mic_ring=storage;
static uint32_t mic_ring_head,mic_ring_tail,mic_open_clients,mic_token;
static uint32_t mic_graphs_opened;static bool mic_graphs_started,mic_hw_prepared;
static int mic_capture_error;
static uint64_t last_blocking_read_ns;
static volatile uint32_t mic_inflight_mask;
static uint32_t mic_slot_map_handle,mic_sent_count,mic_done_count,mic_last_bytes,mic_peak;
static uint64_t mic_slot_dsp_base;
static volatile uint8_t *mic_slot_dma_va;
static volatile bool mic_async_failed;
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
bool annibale_mic_gpr_event(uint32_t src_port,uint32_t dst_port,uint32_t opcode,
    const void *payload,uint32_t len);
static bool annibale_adsp_gpr_send(const void *raw,uint32_t len,uint32_t wait,
    uint32_t *op,uint32_t *words,uint32_t count) {
    const uint32_t *p=raw;
    if(wait==0) {
        read_requests++;
        assert(len==56 && (p[0]>>8)==len && p[2]==0x2002 && p[3]==0x6021 && p[5]==0x0400100b);
        assert(p[9]==1920);
        for(unsigned i=10;i<14;i++)assert(p[i]==0);
        return fault!=1;
    }
    commands++;if(commands==fail_command)return false;
    (void)count;
    if(p[5]==0x0100100f) {
        if(op) *op=0x02001002;
        if(words && count>=2) { words[0]=p[5];words[1]=0; }
        return true;
    }
    if(op) *op=0x02001005;
    if(words && count>=2) { words[0]=0;words[1]=0; }
    return true;
}
'''
MAIN = r'''
static void reset(void) {
    assert(locked==0);thread_test=false;tlmm_base=0;hw_up=true;reach_active=true;
    map_calls=fail_map=commands=fail_command=read_requests=0;
    mic_hw_prepared=mic_graphs_started=false;mic_graphs_opened=0;mic_capture_error=0;mic_open_clients=0;
    memset(tlmm_memory,0,sizeof(tlmm_memory));memset(lpi_memory,0,sizeof(lpi_memory));memset(va_memory,0,sizeof(va_memory));
    mic_inflight_mask=0;mic_slot_map_handle=0;mic_slot_dsp_base=0;mic_slot_dma_va=NULL;
    mic_async_failed=false;mic_sent_count=mic_done_count=mic_last_bytes=mic_peak=0;last_blocking_read_ns=0;
    mic_ring=storage;mic_ring_head=mic_ring_tail=0;moves=fed_frames=0;
    fault=0;reply_bytes=1920;recording=dma_available=true;
    memset(storage,0x55,sizeof(storage));memset(dma_buffer,0,sizeof(dma_buffer));
}
static void once(void) {
    thread_test=true;if(setjmp(thread_exit)==0)mic_capture_thread(NULL,0);thread_test=false;
    assert(locked==0);
}
static bool submit_async_case(uint32_t slot, uint32_t *bytes_out) {
    if(!queue_capture_slot_async(slot, dma_buffer, dsp_address, map_handle)) return false;
    uint64_t addr=dsp_address+0x8000u+(uint64_t)slot*1920u;
    uint32_t words[14]={0,(uint32_t)addr,(uint32_t)(addr>>32),map_handle,reply_bytes,0,0,0,1,0,0,0,0,0};
    if(fault==3)words[0]=1;
    if(fault==4)words[1]++;
    if(fault==5)words[2]++;
    if(fault==6)words[3]++;
    if(fault==7)words[4]=1922;
    if(fault==8)words[4]=3;
    if(fault==9)words[9]=1;
    if(fault==10)words[13]=4;
    uint32_t opcode = (fault==2) ? 0x02001005u : 0x05001005u;
    if(opcode!=0x05001005u) {
        assert(!annibale_mic_gpr_event(0x6021u, 0x2002u, opcode, words, sizeof(words)));
        return false;
    }
    assert(annibale_mic_gpr_event(0x6021u, 0x2002u, opcode, words, sizeof(words)));
    if(mic_async_failed) return false;
    if(bytes_out) *bytes_out=words[4];
    return true;
}
int main(void) {
    for(unsigned failure=1;failure<=2;failure++) {
        reset();fail_map=failure;
        assert(!mic_hw_prepare() && !mic_hw_prepared);
    }
    reset();hw_up=false;assert(!mic_hw_prepare() && !mic_hw_prepared && commands==0);
    reset();reach_active=false;assert(!mic_hw_prepare() && !mic_hw_prepared && commands==0);
    for(unsigned failure=1;failure<=2;failure++) {
        reset();fail_command=failure;
        assert(!mic_hw_prepare() && !mic_hw_prepared);
    }
    reset();assert(mic_hw_prepare() && mic_hw_prepared);
    unsigned saved=commands;assert(mic_hw_prepare() && commands==saved);
    for(unsigned failure=1;failure<=10;failure++) {
        reset();fail_command=failure;
        /* A failure after an OPEN closes what was opened: one CLOSE after SG_MIC, two after SG_CAPTURE. */
        unsigned closes=failure==4||failure==5 ? 1u : failure==6 ? 2u : 0u;
        assert(!start_capture_graphs() && !mic_graphs_started && commands==failure+closes);
        assert(mic_graphs_opened==(failure<=6 ? 0u : 3u));
        fail_command=0;commands=0;
        assert(start_capture_graphs() && mic_graphs_started && commands==(failure<=2 ? 10u : failure<=6 ? 8u : 4u));
    }
    reset();assert(start_capture_graphs() && mic_graphs_started && commands==10);
    for(unsigned i=0;i<=10;i++) {
        reset();fault=i;uint32_t bytes=0;
        assert(submit_async_case(2,&bytes)==(i==0));
        assert(bytes==(i==0 ? 1920u:0u));
    }
    reset();reply_bytes=4;
    int16_t *hw=(int16_t *)(dma_buffer+MIC_OFFSET+2*MIC_PERIOD_BYTES);
    hw[0]=123;hw[1]=-456;hw[2]=789;
    uint32_t bytes=0;assert(submit_async_case(2,&bytes) && bytes==4);
    assert(mic_ring_head==2 && storage[0]==123 && storage[1]==-456 && storage[2]==0x5555);
    assert(fed_frames==2 && fed[0]==123 && fed[1]==123 && fed[2]==-456 && fed[3]==-456);
    reset();harvest_capture_period(0,dma_buffer,1920);
    assert(mic_ring_head==960);for(unsigned i=0;i<960;i++)assert(storage[i]==0);
    reset();fail_command=1;once();assert(read_requests==0 && mic_ring_head==0);
    reset();mic_graphs_started=true;mic_hw_prepared=true;mic_open_clients=1;
    mic_async_failed=true;once();
    assert(mic_capture_error==EIO && mic_ring_head==0 && !mic_graphs_started);
    struct uio u={.remaining=8};
    assert(annibale_mic_read(&u,IO_NDELAY)==EIO && moves==0);
    recording=false;assert(mic_cdev_close(0,0,0,NULL)==0 && mic_capture_error==0);
    reset();fail_command=1;u=(struct uio){.remaining=8};
    assert(annibale_mic_read(&u,IO_NDELAY)==EIO && moves==0);
    reset();fail_command=1;
    assert(mic_cdev_open(0,0,0,NULL)==EIO && mic_open_clients==0);
    reset();assert(mic_cdev_open(0,0,0,NULL)==0 && mic_open_clients==1);
    u=(struct uio){.remaining=8};assert(annibale_mic_read(&u,IO_NDELAY)==EAGAIN && moves==0);
    storage[0]=111;storage[1]=-222;mic_ring_head=2;
    assert(annibale_mic_read(&u,IO_NDELAY)==0 && u.offset==4 && u.remaining==4);
    assert(u.data[0]==111 && u.data[1]==-222);
    reset();mic_ring=NULL;u=(struct uio){.remaining=8};assert(annibale_mic_read(&u,0)==ENXIO);
    assert(locked==0);
    puts("PASS: production power/graph failures, async READ_BUF_DONE replies, exact PCM extent and open/read/close recovery");
}
'''


def main():
    constants = MIC[MIC.index('#define TLMM_PHYS'):MIC.index('static LCK_GRP_DECLARE')]
    builders = MIC[MIC.index('struct mic_pkt {'):MIC.index('/* ---- LPASS PRM')]
    source = HEADER + constants
    source += function(REACH, 'gpr_set32') + function(REACH, 'gpr_finish')
    source += function(REACH, 'annibale_audio_reach_send_cmd') + builders
    source += ''.join(function(MIC, name) for name in (
        'mic_prm_vote_hw_core', 'mic_prm_set_clk', 'mic_configure_dmic_pins',
        'mic_configure_va_macro', 'mic_hw_prepare', 'open_capture_graphs', 'close_capture_graphs',
        'start_capture_graphs', 'stop_capture_graphs',
        'queue_capture_slot_async', 'harvest_capture_period', 'annibale_mic_gpr_event',
        'mic_capture_thread', 'annibale_mic_read', 'mic_cdev_open', 'mic_cdev_close'))
    source += MAIN
    mutations = (
        ('wrong metadata status', 'words[9] != 0', 'words[8] != 0'),
        ('truncate reply copy', 'memcpy(words, payload, sizeof(words));', 'memcpy(words, payload, 32);'),
        ('accept odd byte count', '(words[4] % sizeof(int16_t)) != 0', 'false'),
        ('publish stale DMA tail', 'i < valid_samples; i++) {\n\t\tif ((mic_ring_head', 'i < MIC_PERIOD_FRAMES; i++) {\n\t\tif ((mic_ring_head'),
        ('ignore power failure', 'if (!mic_hw_prepare()) return false;', '(void)mic_hw_prepare();'),
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

#!/usr/bin/env python3
"""Exercise production GLINK intent ownership and receive handling on the host."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATH = Path(os.environ.get('ADSP_SOURCE', ROOT / 'xnu/files/bsd/dev/annibale/annibale_adsp.c'))
SOURCE = SOURCE_PATH.read_text()


def function(name):
    match = re.search(r'(?:static )?(?:bool|void|int|uint32_t)\s+' + name + r'\([^;]*?\)\s*\{', SOURCE)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[match.start():end] + '\n'


HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define __builtin_arm_dmb(x) ((void)0)
#define say(...) fprintf(stderr, __VA_ARGS__)
static pthread_mutex_t aadsp_gpr_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t aadsp_tx_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t glink_tx_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t glink_rx_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t glink_intent_lock=PTHREAD_MUTEX_INITIALIZER;
static void lck_mtx_lock(pthread_mutex_t *p) { assert(!pthread_mutex_lock(p)); }
static void lck_mtx_unlock(pthread_mutex_t *p) { assert(!pthread_mutex_unlock(p)); }
static uint8_t smem[8192];
static uint32_t glink_our_q=1024, glink_our_q_size=1024, glink_their_q=4096, glink_their_q_size=2048;
static uint32_t glink_our_read_at=4, glink_our_write_at=8, glink_their_read_at=12, glink_their_write_at=16;
static uint32_t glink_tx_write_pos;
static uint16_t glink_gpr_our_cid=2, glink_gpr_their_cid=3, glink_pmic_our_cid=4, glink_pmic_their_cid=5;
static bool adsp_hw_up=true, waited_done;
static uint32_t waited_token, waited_opcode, waited_words[32], waited_word_count;
static uint32_t waited_response_src, waited_response_dst;
typedef bool (*annibale_gpr_event_fn)(uint32_t,uint32_t,uint32_t,uint32_t,const void *,uint32_t);
static annibale_gpr_event_fn gpr_event_handler;
static uint64_t ticks;
static unsigned event_count;
static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv=PTHREAD_COND_INITIALIZER;
static bool gate_reads, release_reads;
static unsigned read_entries;
static bool smem_peek(uint32_t at, void *out, uint32_t size) {
    assert(at+size<=sizeof(smem));
    lck_mtx_lock(&gate_lock);
    memcpy(out,smem+at,size);
    if(gate_reads && at==glink_their_read_at) {
        read_entries++;
        assert(!pthread_cond_broadcast(&gate_cv));
        while(!release_reads) assert(!pthread_cond_wait(&gate_cv,&gate_lock));
    }
    lck_mtx_unlock(&gate_lock);
    return true;
}
static void smem_poke(uint32_t at, uint32_t value) {
    assert(at+4<=sizeof(smem));
    lck_mtx_lock(&gate_lock); memcpy(smem+at,&value,4); lck_mtx_unlock(&gate_lock);
}
static uint32_t smem_item(uint32_t item,uint32_t *size) { (void)item;*size=0;return 0; }
static uint64_t now_us(void) { return __atomic_add_fetch(&ticks,1,__ATOMIC_RELAXED); }
static void sleep_us(unsigned us) { __atomic_add_fetch(&ticks,us,__ATOMIC_RELAXED); }
static void ipcc_ring(unsigned client,unsigned signal) { (void)client;(void)signal; }
static void annibale_pmic_glink_set_tx(int (*fn)(const void *,uint32_t)) { (void)fn; }
static void annibale_pmic_glink_rx(const void *data,uint32_t len) { (void)data;(void)len; }
static bool event(uint32_t src,uint32_t dst,uint32_t op,uint32_t token,const void *data,uint32_t len) {
    (void)src;(void)dst;(void)op;(void)token;(void)data;(void)len;
    __atomic_add_fetch(&event_count,1,__ATOMIC_RELAXED);return true;
}
static void enqueue(const uint32_t *words,unsigned count) {
    uint32_t w;smem_peek(glink_their_write_at,&w,4);
    for(unsigned i=0;i<count;i++) smem_poke(glink_their_q+(w+4*i)%glink_their_q_size,words[i]);
    smem_poke(glink_their_write_at,(w+((4*count+7)&~7u))%glink_their_q_size);
}
static void command(uint16_t cmd,uint16_t cid,uint32_t id) {
    const uint32_t words[]={cmd|((uint32_t)cid<<16),id};enqueue(words,2);
}
static void advertise(uint16_t cid,uint32_t id,uint32_t size) {
    const uint32_t words[]={GLINK_INTENT|((uint32_t)cid<<16),1,size,id};enqueue(words,4);
}
'''

MAIN = r'''
static void receive_done(uint16_t cmd,uint16_t cid,uint32_t id) {
    command(cmd,cid,id);glink_poll_rx();
}
static void receive_intent(uint16_t cid,uint32_t id,uint32_t size) {
    advertise(cid,id,size);glink_poll_rx();
}
static bool take(unsigned bytes,uint32_t *id) {
    return glink_take_their_intent(&glink_gpr_intents,0,bytes,id);
}
static void *poll_thread(void *unused) { (void)unused;glink_poll_rx();return NULL; }
static void response(uint32_t token,uint32_t src,uint32_t status,unsigned words) {
    const uint32_t data[]={GLINK_TX_DATA|((uint32_t)glink_gpr_their_cid<<16),1,24+words*4,0,
        ((24+words*4)<<8)|0x60,0x203,src,0x2001,token,GPR_BASIC_RSP_RESULT,0x01001000,status};
    enqueue(data,10+words);
}
int main(int argc,char **argv) {
    assert(argc==2);unsigned scenario=(unsigned)strtoul(argv[1],NULL,10);uint32_t id=0;
    gpr_event_handler=event;
    if(scenario==0) {
        receive_intent(glink_gpr_their_cid,10,64);assert(take(24,&id)&&id==10);
        receive_done(GLINK_DONE_REUSE,glink_gpr_their_cid,10);
        assert(!take(296,&id));assert(take(44,&id)&&id==10);
        receive_intent(glink_gpr_their_cid,11,512);assert(take(296,&id)&&id==11);
    } else if(scenario==1) {
        receive_intent(glink_gpr_their_cid,10,64);assert(take(24,&id));
        receive_intent(glink_gpr_their_cid,11,512);assert(take(296,&id)&&id==11);
        receive_done(GLINK_DONE_REUSE,glink_gpr_their_cid,10);
        assert(take(44,&id)&&id==10);assert(!take(296,&id));
    } else if(scenario==2) {
        receive_intent(glink_gpr_their_cid,10,64);assert(take(24,&id));
        receive_intent(glink_gpr_their_cid,10,512);assert(!take(24,&id));
        receive_done(GLINK_DONE_REUSE,glink_gpr_their_cid,10);
        assert(!take(296,&id));assert(take(24,&id)&&id==10);
    } else if(scenario==3) {
        receive_done(GLINK_DONE_REUSE,glink_gpr_their_cid,99);assert(!take(24,&id));
        receive_intent(glink_gpr_their_cid,10,64);assert(take(24,&id));
        receive_done(GLINK_RX_DONE,glink_gpr_their_cid,10);assert(!take(24,&id));
        receive_done(GLINK_DONE_REUSE,glink_gpr_their_cid,10);assert(!take(24,&id));
        receive_intent(glink_gpr_their_cid,10,512);assert(take(296,&id)&&id==10);
    } else if(scenario==4) {
        for(unsigned i=0;i<GLINK_MAX_INTENTS;i++) { receive_intent(glink_gpr_their_cid,i+1,64);assert(take(24,&id)&&id==i+1); }
        receive_intent(glink_gpr_their_cid,99,512);
        for(unsigned i=0;i<GLINK_MAX_INTENTS;i++) {
            receive_done(GLINK_DONE_REUSE,glink_gpr_their_cid,i+1);assert(take(24,&id)&&id==i+1);
        }
        assert(!take(296,&id));
    } else if(scenario==5) {
        receive_intent(glink_gpr_their_cid,10,64);
        uint32_t pkt[]={0x1860,0x302,0x2001,1,7,0x01001000};
        glink_tx_write_pos=1000;
        assert(!annibale_adsp_gpr_send(pkt,sizeof(pkt),1,NULL,NULL,0));assert(waited_token==0);
        glink_tx_write_pos=0;
        assert(annibale_adsp_gpr_send(pkt,sizeof(pkt),0,NULL,NULL,0));
        assert(!take(24,&id));
    } else if(scenario==6) {
        receive_intent(glink_gpr_their_cid,10,64);assert(take(24,&id));
        receive_intent(glink_pmic_their_cid,10,512);
        receive_done(GLINK_DONE_REUSE,glink_pmic_their_cid,10);assert(!take(24,&id));
        assert(glink_take_their_intent(&glink_pmic_intents,0,296,&id)&&id==10);
        receive_done(GLINK_DONE_REUSE,glink_gpr_their_cid,10);assert(!take(296,&id));
        assert(take(24,&id)&&id==10);
    } else if(scenario==7) {
        response(9,1,0,2);gate_reads=true;pthread_t first,second;
        assert(!pthread_create(&first,NULL,poll_thread,NULL));
        lck_mtx_lock(&gate_lock);
        while(read_entries==0) assert(!pthread_cond_wait(&gate_cv,&gate_lock));
        lck_mtx_unlock(&gate_lock);
        assert(!pthread_create(&second,NULL,poll_thread,NULL));
        lck_mtx_lock(&gate_lock);
        struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));
        deadline.tv_nsec+=100000000;if(deadline.tv_nsec>=1000000000) {deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
        while(read_entries<2) {
            int err=pthread_cond_timedwait(&gate_cv,&gate_lock,&deadline);
            assert(err==0||err==ETIMEDOUT);if(err==ETIMEDOUT) break;
        }
        release_reads=true;assert(!pthread_cond_broadcast(&gate_cv));lck_mtx_unlock(&gate_lock);
        assert(!pthread_join(first,NULL));assert(!pthread_join(second,NULL));assert(event_count==1);
    } else if(scenario==13) {
        receive_intent(glink_pmic_their_cid,10,512);uint32_t payload[70]={0};
        glink_tx_write_pos=1000;assert(adsp_pmic_glink_tx(payload,sizeof(payload))==-1);
        glink_tx_write_pos=0;assert(adsp_pmic_glink_tx(payload,sizeof(payload))==0);
        assert(!glink_take_their_intent(&glink_pmic_intents,0,sizeof(payload),&id));
    } else if(scenario==14) {
        advertise(glink_gpr_their_cid,10,512);
        assert(glink_take_their_intent(&glink_gpr_intents,glink_gpr_our_cid,296,&id)&&id==10);
        assert(glink_tx_write_pos==8);
        assert(glink_q_read32(glink_our_q,glink_our_q_size,0)==(GLINK_RX_INTENT_REQ|((uint32_t)glink_gpr_our_cid<<16)));
        assert(glink_q_read32(glink_our_q,glink_our_q_size,4)==296);
    } else if(scenario==15) {
        assert(!glink_take_their_intent(&glink_gpr_intents,glink_gpr_our_cid,296,&id));
        assert(ticks>=50000&&ticks<51000&&glink_tx_write_pos==8);
        receive_intent(glink_gpr_their_cid,10,512);
        uint32_t pkt[]={0x1860,0x302,0x2001,1,7,0x01001000};
        assert(annibale_adsp_gpr_send(pkt,sizeof(pkt),0,NULL,NULL,0));
    } else if(scenario==16) {
        receive_intent(glink_gpr_their_cid,10,64);
        receive_intent(glink_gpr_their_cid,11,512);
        receive_done(GLINK_RX_DONE,glink_gpr_their_cid,10);
        assert(annibale_adsp_gpr_ready());
        receive_done(GLINK_RX_DONE,glink_gpr_their_cid,11);
        assert(!annibale_adsp_gpr_ready());
    } else if(scenario==17) {
        receive_intent(glink_gpr_their_cid,10,64);response(7,1,1,2);
        uint32_t pkt[]={0x1860,0x302,0x2001,1,7,0x01001000},opcode=0,payload[2]={0};
        assert(!annibale_adsp_gpr_send(pkt,sizeof(pkt),1,&opcode,payload,2));
        assert(waited_token==0&&opcode==GPR_BASIC_RSP_RESULT&&payload[1]==1);
        receive_done(GLINK_DONE_REUSE,glink_gpr_their_cid,10);
        response(7,1,1,2);response(8,1,0,2);pkt[4]=8;
        assert(annibale_adsp_gpr_send(pkt,sizeof(pkt),1,&opcode,payload,2));
        assert(waited_token==0&&payload[1]==0&&event_count==1);
    } else {
        unsigned mode=scenario-8;
        receive_intent(glink_gpr_their_cid,10,64);
        response(mode==2?8:7,mode==3?2:1,mode==1?1:0,mode==4?1:2);
        uint32_t pkt[]={0x1860,0x302,0x2001,1,7,0x01001000},opcode=0,payload[2]={0};
        bool ok=annibale_adsp_gpr_send(pkt,sizeof(pkt),1,&opcode,payload,2);
        assert(ok==(mode==0));assert(waited_token==0);
        assert(opcode==((mode==2||mode==3)?0:GPR_BASIC_RSP_RESULT));
        if(mode==1) assert(payload[0]==0x01001000&&payload[1]==1);
    }
    puts("PASS");
}
'''


class IntentTests(unittest.TestCase):
    def test_production_receive_and_ownership(self):
        constants = '\n'.join(line for line in SOURCE.splitlines() if re.match(
            r'#define (?:GLINK_|GPR_|IPCC_|ITEM_GLINK_THEIR_Q|APM_CMD_|OP_)\w*\s+(?:0x[\da-fA-F]+|\d+)u?$', line))
        pool = re.search(r'struct glink_intent_pool \{.*?\n\};', SOURCE, re.S).group()
        source = HEADER.replace('static uint8_t smem', constants + '\n' + pool +
            '\nstatic struct glink_intent_pool glink_gpr_intents,glink_pmic_intents;\nstatic uint8_t smem')
        source += 'static void glink_poll_rx(void);\n'
        source += ''.join(function(name) for name in ('glink_q_read32','glink_q_write','glink_send_cmd2',
            'glink_send_open','glink_queue_intents','handle_incoming_gpr'))
        if '#include "annibale_adsp_intents.inc"' in SOURCE:
            source += (SOURCE_PATH.parent / 'annibale_adsp_intents.inc').read_text()
        else:
            source += function('glink_add_their_intent') + function('glink_take_their_intent')
        source += ''.join(function(name) for name in ('adsp_pmic_glink_tx','glink_poll_rx','annibale_adsp_gpr_send',
                                                     'annibale_adsp_gpr_ready'))
        source += MAIN
        names = ('capacity','in-flight','duplicate','retirement','full-pool','unsent-retry',
                 'channel-isolation','concurrent-receive','basic-success','basic-error','token','port','short-result',
                 'pmic-unsent-retry','requested-intent','request-timeout-recovery','ready-after-retirement',
                 'error-then-success')
        with tempfile.TemporaryDirectory(prefix='audio-intents-') as directory:
            cfile,binary=Path(directory)/'test.c',Path(directory)/'test'
            cfile.write_text(source)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-pthread',
                            str(cfile),'-o',str(binary)],check=True)
            for scenario,name in enumerate(names):
                with self.subTest(name=name):
                    result=subprocess.run([str(binary),str(scenario)],capture_output=True,text=True,timeout=10)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    print(f'{name}: PASS', flush=True)


if __name__ == '__main__':
    unittest.main()

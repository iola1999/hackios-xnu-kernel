#!/usr/bin/env python3
"""Run production GPR send, GLINK queue and PCM V2 functions with host I/O stubs."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ADSP = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_adsp.c').read_text()
REACH = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_audio_reach.c').read_text()


def function(source, name):
    match = re.search(r'(?:static )?(?:bool|void|uint32_t)\s+' + name + r'\([^;]*?\)\s*\{', source)
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
#define MIN(a,b) ((a)<(b)?(a):(b))
#define __builtin_arm_dmb(x) ((void)0)
#define __builtin_arm_dsb(x) ((void)0)
#define IPCC_CLIENT_LPASS 3
#define IPCC_SIGNAL_GLINK 0
#define GLINK_TX_DATA 9
#define GPR_BASIC_RSP_RESULT 0x02001005u
#define APM_CMD_GET_SPF_STATE 0x01001021u
#define APM_CMD_RSP_GET_SPF_STATE 0x02001007u
#define OP_MEM_MAP 0x0100100cu
#define OP_MEM_MAP_DONE 0x02001001u
#define OP_WRITE 0x0400100au
#define OP_WRITE_DONE 0x05001004u
#define OP_READ_BUF 0x0400100bu
#define OP_READ_BUF_DONE 0x05001005u
static bool adsp_hw_up = true, waited_done;
static uint32_t glink_our_q = 16, glink_our_q_size = 1024, glink_our_read_at = 4;
static uint32_t glink_our_write_at = 8, glink_tx_write_pos, glink_gpr_our_cid = 2, glink_gpr_their_cid = 3;
static uint32_t waited_token, waited_opcode, waited_word_count, waited_words[32];
static int aadsp_gpr_lock, aadsp_tx_lock, glink_tx_lock, glink_gpr_intents, response_mode, writes, doorbells, intents = 1;
static uint32_t waited_response_src, waited_response_dst;
static uint32_t reader;
static uint64_t ticks;
static bool advance_on_lock;
static void lck_mtx_lock(void *p) {
    if(p==&glink_tx_lock && advance_on_lock) { glink_tx_write_pos=128;advance_on_lock=false; }
}
static void lck_mtx_unlock(void *p) { (void)p; }
static uint64_t now_us(void) { return ticks++; }
static void sleep_us(unsigned n) { ticks += n; }
static void ipcc_ring(int a, int b) { (void)a; (void)b; doorbells++; }
static void smem_peek(uint32_t at, void *out, unsigned size) { (void)at; assert(size==4); memcpy(out,&reader,4); }
static void smem_poke(uint32_t at, uint32_t value) { (void)at; (void)value; writes++; }
static bool glink_take_their_intent(void *p, uint16_t cid, uint32_t len, uint32_t *id) {
    (void)p; (void)cid; (void)len; *id=1; return intents;
}
static void glink_poll_rx(void) {
    if (!response_mode) return;
    waited_done=true; waited_opcode=GPR_BASIC_RSP_RESULT; waited_response_src=0x2001; waited_response_dst=0x302;
    waited_word_count=response_mode==3 ? 1 : 2;
    waited_words[0]=response_mode==4 ? 0 : 0x01001000;
    waited_words[1]=response_mode==2 ? 1 : 0;
    if(response_mode>=5) {
        waited_opcode=OP_WRITE_DONE; waited_word_count=response_mode==5 ? 3 : 8;
    }
    if(response_mode>=7) {
        waited_opcode=OP_READ_BUF_DONE; waited_word_count=response_mode==7 ? 13 : 14;
    }
}
'''
TRANSPORT_MAIN = r'''
int main(void) {
    uint32_t pkt[6]={0x1860,0x302,0x2001,1,7,0x01001000}, op=0, words[2]={0};
    for (int mode=0; mode<5; mode++) {
        response_mode=mode; glink_tx_write_pos=0;
        assert(annibale_adsp_gpr_send(pkt,sizeof(pkt),1,&op,words,2)==(mode==1));
        assert(waited_token==0);
    }
    pkt[5]=OP_WRITE;
    for (response_mode=5;response_mode<=6;response_mode++) {
        glink_tx_write_pos=0;
        assert(annibale_adsp_gpr_send(pkt,sizeof(pkt),1,&op,words,2)==(response_mode==6));
    }
    pkt[5]=OP_READ_BUF;
    for (response_mode=7;response_mode<=8;response_mode++) {
        glink_tx_write_pos=0;
        assert(annibale_adsp_gpr_send(pkt,sizeof(pkt),1,&op,words,2)==(response_mode==8));
    }
    adsp_hw_up=false; int prior=writes;
    assert(!annibale_adsp_gpr_send(pkt,sizeof(pkt),0,0,0,0)); assert(writes==prior);
    adsp_hw_up=true; intents=0;
    assert(!annibale_adsp_gpr_send(pkt,sizeof(pkt),0,0,0,0)); assert(writes==prior);
    intents=1; glink_tx_write_pos=1000; reader=0;
    assert(!annibale_adsp_gpr_send(pkt,sizeof(pkt),0,0,0,0)); assert(writes==prior);
    glink_tx_write_pos=0; assert(annibale_adsp_gpr_send(pkt,sizeof(pkt),0,0,0,0));
    assert(writes>prior);
    glink_tx_write_pos=0;advance_on_lock=true;
    assert(glink_q_write(glink_our_q,glink_our_q_size,&glink_tx_write_pos,pkt,8));
    assert(glink_tx_write_pos==136);
    puts("PASS: GPR response lengths/errors and GLINK lock acquisition/write position");
}
'''
PCM_HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define __builtin_arm_dsb(x) ((void)0)
#define GPR_DOMAIN_ADSP 2
#define GPR_DOMAIN_APPS 3
#define OUR_PORT 0x2001
#define IID_PLAY_SHMEM 0x6001
#define OP_WRITE 0x0400100a
#define OP_WRITE_DONE 0x05001004
#define PERIOD_BYTES 3840
struct gpr_pkt { uint8_t buf[1024]; uint32_t len; };
static uint32_t gpr_token, map_handle=0x1234, reach_sent_count, reach_done_count;
static uint64_t dsp_address=0x1cf66f000ULL;
static int fault;
static bool annibale_audio_reach_send_cmd(const void *raw,uint32_t len,const char *descr,
 uint32_t *op,uint32_t *words,uint32_t count) {
    (void)descr; assert(len==24+11*4 && count==8);
    const uint32_t *p=raw; assert(p[5]==OP_WRITE);
    assert(p[6]==(uint32_t)(dsp_address+2*PERIOD_BYTES) && p[7]==1 && p[8]==map_handle);
    assert(p[9]==PERIOD_BYTES);
    for (unsigned i=10;i<17;i++) assert(p[i]==0);
    *op=OP_WRITE_DONE; words[0]=p[6];words[1]=p[7];words[2]=p[8];
    if(fault==1) return false;
    if(fault==2) *op=0x02001005;
    if(fault==3) words[0]++;
    if(fault==4) words[2]++;
    if(fault==5) words[3]=1;
    if(fault==6) words[7]=1;
    return true;
}
'''
PCM_MAIN = r'''
int main(void) {
 for(fault=0;fault<7;fault++) {
    reach_sent_count=reach_done_count=0;
    assert(send_audio_period(2)==(fault==0));
    assert(reach_done_count==(fault==0));
 }
 puts("PASS: PCM V2 wire layout and matched completion required");
}
'''


GRAPH_HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define say(...) ((void)0)
struct gpr_pkt { uint8_t buf[1024]; uint32_t len; };
struct reach_module { uint32_t module, iid, inputs, outputs; };
struct reach_link { uint32_t from, to; };
static uint32_t gpr_token, map_handle;
static uint64_t dsp_address=0x1cf66f000ULL;
static unsigned commands, fail_at, input_formats, output_formats;
static bool bad_map, bad_state, bad_opcode;
static bool annibale_audio_reach_send_cmd(const void *raw, uint32_t len, const char *descr,
 uint32_t *op, uint32_t *words, uint32_t count) {
    (void)descr; (void)count;
    const uint32_t *p=raw;
    assert((p[0] >> 8)==len);
    commands++;
    if(commands==fail_at) return false;
    *op=GPR_BASIC_RSP_RESULT;
    if(p[5]==APM_CMD_GET_SPF_STATE) {
        *op=APM_CMD_RSP_GET_SPF_STATE; words[0]=!bad_state;
    } else if(p[5]==OP_MEM_MAP) {
        assert(len==44 && p[6]==0x10003 && p[7]==0);
        assert(p[8]==(uint32_t)dsp_address && p[9]==1 && p[10]==BUFFER_BYTES);
        *op=OP_MEM_MAP_DONE; words[0]=bad_map ? 0 : 0x4321;
    } else if(p[5]==OP_SET_CFG) {
        assert(p[9]==len-40);
        for(unsigned off=40;off<len;) {
            const uint32_t *param=(const uint32_t *)((const uint8_t *)raw+off);
            assert(param[3]==0 && off+16+param[2]<=len);
            if(param[1]==0x0800100c) {
                assert(p[3]==IID_PLAY_SHMEM && param[0]==IID_PLAY_SHMEM);
                assert(param[2]==32 && param[4]==1 && param[5]==0x09001000 && param[6]==18);
                assert(param[7]==48000 && param[8]==0x10010 && param[9]==0xf0010);
                assert(param[10]==0x20001 && param[11]==0x201);
                input_formats++;
            } else if(param[1]==0x08001008) {
                assert(p[3]==APM_IID && (param[0]==IID_PLAY_DEC || param[0]==IID_PLAY_CNV));
                assert(param[2]==32 && param[4]==1 && param[5]==0x09001000 && param[6]==18);
                assert(param[7]==0x10010 && param[8]==0xf0010 && param[9]==0x10001);
                assert(param[10]==0x20000 && param[11]==0x201);
                output_formats++;
            }
            off+=16+param[2];
        }
    } else if(p[5]==OP_GRAPH_PREPARE || p[5]==OP_GRAPH_START) {
        assert(input_formats==1 && output_formats==2);
    }
    if(bad_opcode && commands==3) *op=OP_WRITE_DONE;
    return true;
}
'''
GRAPH_MAIN = r'''
int main(void) {
    for(fail_at=0;fail_at<=11;fail_at++) {
        commands=input_formats=output_formats=map_handle=0;
        assert(build_audioreach_graphs()==(fail_at==0));
        assert(commands==(fail_at ? fail_at : 11));
        if(fail_at==1 || fail_at==2) assert(map_handle==0);
    }
    fail_at=0; commands=0;bad_map=true;map_handle=0;
    assert(!build_audioreach_graphs() && commands==2 && map_handle==0);
    bad_map=false;commands=0;bad_state=true;
    assert(!build_audioreach_graphs() && commands==1);
    bad_state=false;commands=0;bad_opcode=true;
    assert(!build_audioreach_graphs() && commands==3);
    puts("PASS: graph formats and stop at all eleven failed command stages");
}
'''


def main():
    transport = HEADER + function(ADSP,'glink_q_write') + function(ADSP,'annibale_adsp_gpr_send') + TRANSPORT_MAIN
    pcm = PCM_HEADER + ''.join(function(REACH,n) for n in ('gpr_put32','gpr_set32','gpr_begin','gpr_finish','send_audio_period')) + PCM_MAIN
    constants = '\n'.join(line for line in REACH.splitlines() if line.startswith('#define ') and
                          re.match(r'#define \w+\s+(?:0x[0-9a-fA-F]+|[0-9]+)u?\s*(?:/\*.*)?$', line))
    graph = GRAPH_HEADER.replace('static bool annibale_audio_reach_send_cmd', constants + '\nstatic bool annibale_audio_reach_send_cmd')
    graph += ''.join(function(REACH,n) for n in ('gpr_put8','gpr_put16','gpr_put32','gpr_set32',
        'gpr_pad_to','gpr_begin','gpr_finish','gpr_cmd_begin','gpr_cmd_end','gpr_param_begin',
        'gpr_param_end','gpr_graph_open','gpr_send_command','gpr_graph_cmd','build_audioreach_graphs')) + GRAPH_MAIN
    with tempfile.TemporaryDirectory(prefix='audio-transport-') as directory:
        cfile, binary = Path(directory)/'test.c', Path(directory)/'test'
        for source in (transport, pcm, graph):
            cfile.write_text(source)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
        mutations = (
            (transport, 'waited_words[1] == 0', 'true'),
            (transport, 'waited_word_count >= 14', 'waited_word_count >= 13'),
            (transport, 'lck_mtx_lock(&glink_tx_lock);\n\tuint32_t pos = *write_at;',
             'uint32_t pos = *write_at;\n\tlck_mtx_lock(&glink_tx_lock);'),
            (transport, 'if (wait == 199) {\n\t\t\t\tlck_mtx_unlock(&glink_tx_lock);\n\t\t\t\treturn false;\n\t\t\t}', 'if (wait == 199) break;'),
            (pcm, 'i < 4', 'i < 3'),
            (graph, 'if (!gpr_send_command(&p, "OPEN SG_PLAY (0x4001)")) return false;',
             '(void)gpr_send_command(&p, "OPEN SG_PLAY (0x4001)");'),
        )
        for source, old, new in mutations:
            assert old in source
            cfile.write_text(source.replace(old,new,1))
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True)
            assert result.returncode != 0, old
        print(f'PASS: {len(mutations)} production regressions rejected')


if __name__ == '__main__':
    main()

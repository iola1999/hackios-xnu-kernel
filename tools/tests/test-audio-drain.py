#!/usr/bin/env python3
"""Run the production feeder with simulated ring data and DSP completions."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get('AUDIO_REACH_SOURCE', ROOT / 'xnu/files/bsd/dev/annibale/annibale_audio_reach.c'))
text = SOURCE.read_text()
match = re.search(r'static void\s+reach_feeder_thread\([^;]*?\)\s*\{', text)
assert match
end, depth = match.end(), 1
while depth:
    depth += (text[end] == '{') - (text[end] == '}')
    end += 1
function = text[match.start():end]
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define PERIOD_FRAMES 960
#define QUIET_PERIODS 250
#define SLOTS 8
#define IN_FLIGHT 4
#define NSEC_PER_MSEC 1000000
#define ANNIBALE_GUARD_SILENT 999
#define THREAD_UNINT 0
#define THREAD_AWAKENED 0
#define THREAD_CONTINUE_NULL 0
#define audio_spin_lock ((void *)1)
typedef int wait_result_t;
typedef void *event_t;
static uint32_t ring_head=960, ring_tail, in_flight_mask, reach_sent_count=100, reach_done_count=100;
static uint32_t reach_sync_mode;
static bool reach_write_failed, reach_stream_active, locked;
static unsigned reports, iterations, feeds, amp_on, amp_off;
static bool loud, fail, second;
static jmp_buf finished;
static void lck_spin_lock(void *p) { (void)p;assert(!locked);locked=true; }
static void lck_spin_unlock(void *p) { (void)p;assert(locked);locked=false; }
static void say(const char *format,...) {
    if(strncmp(format,"PCM drained:",12)) return;
    assert(!locked);
    va_list ap;va_start(ap,format);
    unsigned sent=va_arg(ap,unsigned),done=va_arg(ap,unsigned),backlog=va_arg(ap,unsigned);
    unsigned inflight=va_arg(ap,unsigned),error=va_arg(ap,unsigned);va_end(ap);
    assert(sent==reach_sent_count && done==reach_done_count && sent==done && sent>100);
    assert(!backlog && !inflight && !error && !reach_stream_active);
    reports++;
}
static bool annibale_adsp_gpr_ready(void) { return true; }
static bool build_audioreach_graphs(void) { return true; }
static void delay_for_interval(int a,int b) { (void)a;(void)b; }
static void *current_thread(void) { return NULL; }
static void thread_terminate(void *p) { (void)p;assert(fail && reports==0);longjmp(finished,1); }
static bool annibale_audio_volume(uint32_t *a,bool *b,uint32_t *c) { (void)a;(void)b;(void)c;return false; }
static void annibale_guard_attenuation(uint32_t a) { (void)a; }
static void annibale_audio_amp_set_volume(uint32_t a,bool b) { (void)a;(void)b; }
static void annibale_audio_amp_power(bool on) { if(on) amp_on++;else amp_off++; }
static bool fill_period_slot(uint32_t slot,bool muted) {
    (void)slot;(void)muted;
    ring_tail=ring_head;feeds++;return loud && feeds==1;
}
static bool queue_audio_period_async(uint32_t slot) {
    assert(!(in_flight_mask&(1u<<slot)));in_flight_mask|=1u<<slot;reach_sent_count++;return true;
}
static bool send_audio_period(uint32_t slot) { (void)slot;reach_sent_count++;reach_done_count++;return true; }
static void clock_interval_to_deadline(uint32_t ms,int scale,uint64_t *out) { (void)ms;(void)scale;*out=0; }
static void assert_wait_deadline(event_t e,int mode,uint64_t deadline) { (void)e;(void)mode;(void)deadline; }
static void clear_wait(void *thread,int reason) { (void)thread;(void)reason; }
static void thread_block(int mode) {
    (void)mode;assert(++iterations<1000);
    if(fail) {reach_write_failed=true;return;}
    reach_done_count+=(unsigned)__builtin_popcount(in_flight_mask);in_flight_mask=0;
    if(reports==1 && !second) {ring_head+=960;second=true;}
    if(reports==2 && iterations>300) longjmp(finished,1);
}
PRODUCTION_FUNCTION
int main(int argc,char **argv) {
    assert(argc==2);
    reach_sync_mode=!strcmp(argv[1],"sync");loud=!strcmp(argv[1],"loud");fail=!strcmp(argv[1],"error");
    if(!setjmp(finished)) reach_feeder_thread(NULL,0);
    assert(reports==(fail ? 0u : 2u));
    if(!fail) assert(amp_on==(loud ? 1u : 0u) && amp_off==amp_on);
    puts("PASS: final drain snapshot, repeated bursts, idle suppression and completion failure");
}
'''
with tempfile.TemporaryDirectory(prefix='audio-drain-') as temp:
    folder = Path(temp)
    src = folder / 'test.c'
    binary = folder / 'test'
    src.write_text(HARNESS.replace('PRODUCTION_FUNCTION', function))
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(binary)], check=True)
    for case in ['async', 'sync', 'loud', 'error']:
        result = subprocess.run([binary, case], capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, (case, result.stdout, result.stderr)
        print(f'PASS {case}: {result.stdout.strip()}')

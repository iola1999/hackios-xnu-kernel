#!/usr/bin/env python3
"""Exercise the production RAM-upload state and download branches without USB hardware.

The C harness uses the real choose_window, command dispatch through download, and
command-loop interruption path. EFI allocation and USB transfers are host stubs.
The host-script tests execute stage-system.sh's Python with a virtual file and size-refusing loader.
"""
import contextlib
import io
import os
from pathlib import Path
import re
import subprocess
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
USB_SOURCE = (ROOT / 'xnuloader/loader_06_usb_entry.inc').read_text()
BASE_SOURCE = (ROOT / 'xnuloader/loader_01_base_log_dt.inc').read_text()
LAYOUT_SOURCE = (ROOT / 'xnuloader/loader_05_layout_run_kernel.inc').read_text()


def function(source, name):
    match = re.search(r'static[^;{}]*\n' + name + r'\([^;]*?\)\n\{', source)
    assert match, name
    start = match.start()
    level = 1
    pos = match.end()
    while level:
        level += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos] + '\n'


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
static const int platform_annibale, platform_qemu;
static const int *PLAT = &platform_annibale;
static unsigned checks;
#define CK(x) do { checks++; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static uint8_t payload[4096], target[256], receive_buffer[1024 * 1024];
#define PAYLOAD_BASE ((uint64_t)(uintptr_t)payload)
#define PAYLOAD_RESERVE_SIZE 0x4000000ULL
#define PAYLOAD_CONTAINER_MAX 0x3700000ULL
#define USB_CHUNK (1024 * 1024)
struct payload_header { uint32_t magic, size; };
struct payload_info { uint64_t max_mem, forced_base; };
typedef uint64_t efi_status;
typedef struct { uint32_t type; uint64_t phys_start, pages; } efi_memory_descriptor;
#define EfiConventionalMemory 7
#define EfiLoaderData 2
#define AllocateAddress 2
#define EFI_ERROR(x) ((x) != 0)
static uint64_t free_base = 0x8c0000000ULL, free_size = 0x340000000ULL;
static unsigned allocations;
static efi_status allocation_error;
static efi_status get_map(uint64_t *size, void *buffer, uint64_t *key, uint64_t *stride, uint32_t *version)
{
    (void)key; (void)version;
    efi_memory_descriptor d = { 7, free_base, free_size >> 12 };
    memcpy(buffer, &d, sizeof(d)); *size = *stride = sizeof(d); return 0;
}
static efi_status allocate(int policy, int type, uint64_t pages, uint64_t *address)
{ (void)policy; (void)type; (void)pages; (void)address; allocations++; return allocation_error; }
static struct { efi_status (*get_memory_map)(uint64_t *, void *, uint64_t *, uint64_t *, uint32_t *);
    efi_status (*allocate_pages)(int, int, uint64_t, uint64_t *); } services = { get_map, allocate }, *BS = &services;
static uint64_t win_base, win_size, resident_rd_size, payload_bytes;
static int download_to_ramdisk, kernel_resources_held, firmware_fault, usb_connected = 1;
static int usb_out_lost, usb_in_lost, screen_quiet;
static int usb_out_owned = 71, usb_in_owned = 72;
struct kernel_live { int stop; };
static struct kernel_live *running_live;
static struct { uint64_t base; uint32_t width, height, stride; } SCREEN;
static uint32_t log_len;
static char log_buf[512], last_result[64] = "idle", answer[80];
static uint8_t *usb_rx_buf = receive_buffer;
static unsigned rx_calls, data_replies;
static uint64_t rx_left, rx_position;
static jmp_buf loop_escape;
static int escape_loop;
static void print(const char *format, ...) { (void)format; }
/* the panel status of loader_06a_screen_ui.inc: drawing is not what this test checks */
static inline void ui_init(void) {}
static void boot_fallback_check(void) {}
static inline void ui_stage(const char *word) { (void)word; }
static inline void ui_progress(uint64_t done, uint64_t total) { (void)done; (void)total; }
/* "oem sh": tools/test-loader-shell.py */
static const char *menu_enter_fastboot(void) { assert(0); return "unexpected recovery"; }
static void oem_shell(const char *text) { (void)text; }
/* the host-silence watchdog clock, "oem push" into a running kernel's inbox and "oem dsu": not what this test checks */
static uint64_t host_last_cmd;
#define READ_SYSREG(name) 0ULL
static int wdt_armed, download_to_inbox;
static struct kernel_live *inbox_live(void) { return NULL; }
static void inbox_download(const char *size) { (void)size; assert(0); }
static int dsu_command(const char *cmd) { (void)cmd; return 0; }
static int str_eq(const char *a, const char *b) { return !strcmp(a, b); }
static uint64_t parse_number(const char *p, int base) { uint64_t n = 0; (void)base; sscanf(p, "%llx", (unsigned long long *)&n); return n; }
static void fb_reply(const char *status, const char *message)
{ snprintf(answer, 64, "%s%s", status, message); }
static void fb_reply_data(uint64_t size) { assert(size && size <= 0xffffffffULL); data_replies++; }
static int fb_getvar_identity(const char *var) { (void)var; return 0; }
static int parse_payload(struct payload_info *pi, int verbose) { (void)verbose; memset(pi, 0, sizeof(*pi)); return 1; }
enum { LC_STOP_HELD, LC_STOP_NOTIFY, LC_STOP_IDLE };
static int lc_stop_action(int hold, int live) { return live ? LC_STOP_NOTIFY : hold ? LC_STOP_HELD : LC_STOP_IDLE; }
static int64_t usb_wait(uint8_t endpoint, int deliver)
{ (void)endpoint; (void)deliver; assert(escape_loop); longjmp(loop_escape, 1); }
static int64_t usb_rx(uint8_t *buffer, uint64_t length, int command)
{
    if (command && escape_loop) longjmp(loop_escape, 1);
    rx_calls++;
    if (!rx_left) return -1;
    if (length > rx_left) length = rx_left;
    for (uint64_t i = 0; i < length; i++) buffer[i] = (uint8_t)(rx_position++ + 17);
    rx_left -= length;
    return (int64_t)length;
}
'''

SCENARIOS = r'''
static void clean(void)
{
    ramdisk_forget(); win_base = win_size = resident_rd_size = payload_bytes = 0;
    download_to_ramdisk = kernel_resources_held = firmware_fault = usb_out_lost = usb_in_lost = 0;
    running_live = NULL; usb_connected = 1; rx_calls = data_replies = allocations = 0;
    allocation_error = 0; free_base = 0x8c0000000ULL; free_size = 0x340000000ULL;
    answer[0] = 0; PLAT = &platform_annibale;
}
static void backing(uint64_t window)
{
    win_base = (uint64_t)(uintptr_t)target - RESIDENT_RD_OFFSET; win_size = window;
    memset(target, 0xa5, sizeof(target)); rx_position = 0;
}
int main(void)
{
    uint64_t length;
    clean(); fb_command("getvar:ramdisk-protocol"); CK(!strcmp(answer, "OKAY1") && allocations == 0);
    fb_command("oem ramdisk begin 4000000 2c0000000");
    CK(!strcmp(answer, "OKAY") && win_size == 0x2c0000000ULL && allocations == 1);
    CK(ramdisk_pending() && resident_rd_size == 0);
    fb_command("getvar:ramdisk-state"); CK(!strcmp(answer, "OKAYreceiving 0 4000000 2c0000000"));
    fb_command("oem run"); CK(!strncmp(answer, "FAIL", 4));
    fb_command("oem selftest"); CK(!strncmp(answer, "FAIL", 4));
    fb_command("oem ramdisk"); CK(!strncmp(answer, "FAIL", 4));
    CK(ramdisk_block_size("2000000", &length) && length == 0x2000000ULL);
    ramdisk_block_done(length, length); CK(resident_rd_size == 0 && ramdisk_pending());
    CK(ramdisk_block_size("2000000", &length)); ramdisk_block_done(length, length);
    CK(resident_rd_size == 0x4000000ULL && !ramdisk_pending());
    CK(!ramdisk_command("oem run"));
    fb_command("getvar:ramdisk-state"); CK(!strcmp(answer, "OKAYready 4000000 4000000 2c0000000"));

    const char *oversized[] = { "4000001 2c0000000", "100000000 2c0000000", "140000000 2c0000000" };
    for (unsigned i = 0; i < sizeof(oversized)/sizeof(oversized[0]); i++) {
        clean(); memset(target, 0xa5, sizeof(target));
        CK(ramdisk_begin(oversized[i]) != NULL);
        CK(!allocations && !rx_calls && !data_replies && !resident_rd_size);
        for (unsigned j = 0; j < sizeof(target); j++) CK(target[j] == 0xa5);
    }
    clean(); PLAT = &platform_qemu;
    CK(!ramdisk_begin("140000000 2c0000000"));
    CK(ramdisk_transfer.total == 0x140000000ULL && allocations == 1);
    CK(ramdisk_begin("288000001 2c0000000") != NULL);
    clean(); CK(!ramdisk_begin("4000000 2c0000000"));
    ramdisk_block_done(0x4000000, 0x4000000);
    const char *bad[] = { "0 2c0000000", "1 0", "1 2c0000001", "1 400000000",
        "10000000000000000 2c0000000", "ffffffffffffffff 2c0000000", "1 10000000000000000",
        "1", "1 2c0000000 trailing", "1x 2c0000000", "1  2c0000000", "288000001 2c0000000" };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        CK(ramdisk_begin(bad[i]) != NULL); CK(resident_rd_size == 0x4000000ULL);
    }
    CK(ramdisk_begin("8 180000000") != NULL && allocations == 1);
    CK(!ramdisk_begin("4000000 2c0000000") && ramdisk_pending() && !resident_rd_size);
    clean(); free_size = 0x180000000ULL;
    CK(ramdisk_begin("4000000 2c0000000") != NULL && resident_rd_size == 0);
    clean(); allocation_error = 1; CK(ramdisk_begin("8 2c0000000") != NULL && win_size == 0);
    clean(); win_base = ~0ULL - 15; win_size = 0x2c0000000ULL;
    CK(ramdisk_begin("8 2c0000000") != NULL);

    clean(); backing(0x2c0000000ULL);
    CK(!ramdisk_begin("8 2c0000000")); rx_left = 4; fb_command("download:4");
    CK(resident_rd_size == 0 && ramdisk_transfer.received == 4 && target[3] == 20 && target[4] == 0xa5);
    CK(!ramdisk_begin("8 2c0000000") && ramdisk_transfer.received == 0);
    rx_left = 4; rx_position = 0; fb_command("download:4");
    rx_left = 2; fb_command("download:4");
    CK(!strncmp(answer, "FAIL", 4) && ramdisk_transfer.failed && resident_rd_size == 0);
    CK(ramdisk_transfer.received == 4 && target[5] == 22 && target[6] == 0xa5);
    unsigned calls = rx_calls; rx_left = 4; fb_command("download:4"); CK(rx_calls == calls && !strncmp(answer, "FAIL", 4));
    fb_command("oem run"); CK(!strncmp(answer, "FAIL", 4));
    fb_command("oem ramdisk none"); CK(!ramdisk_pending() && !resident_rd_size && !download_to_ramdisk);
    CK(!ramdisk_begin("8 2c0000000")); rx_left = 4; rx_position = 0; fb_command("download:4");
    rx_left = 4; fb_command("download:4");
    CK(!strcmp(answer, "OKAY") && resident_rd_size == 8 && target[0] == 17 && target[7] == 24 && target[8] == 0xa5);
    CK(!ramdisk_begin("8 2c0000000")); fb_command("download:100000000"); CK(ramdisk_transfer.failed && !resident_rd_size);
    CK(!ramdisk_begin("8 2c0000000")); fb_command("download:10000000000000008"); CK(ramdisk_transfer.failed);
    CK(!ramdisk_begin("8 2c0000000")); fb_command("download:9"); CK(ramdisk_transfer.failed);

    const char *commands[] = { "oem ramdisk begin 8 2c0000000", "oem ramdisk none", "download:4", "oem run" };
    for (unsigned mode = 0; mode < 3; mode++) {
        clean(); backing(0x2c0000000ULL); CK(!ramdisk_begin("8 2c0000000"));
        struct kernel_live live = {0};
        kernel_resources_held = mode == 0; firmware_fault = mode == 1; running_live = mode == 2 ? &live : NULL;
        for (unsigned i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
            fb_command((char *)commands[i]); CK(!strncmp(answer, "FAIL", 4));
            CK(ramdisk_transfer.total == 8 && ramdisk_transfer.received == 0 && rx_calls == 0);
        }
        fb_command("getvar:ramdisk-state"); CK(!strncmp(answer, "OKAY", 4));
    }
    for (unsigned mode = 0; mode < 4; mode++) {
        clean(); backing(0x2c0000000ULL); CK(!ramdisk_begin("8 2c0000000"));
        usb_connected = mode != 0; firmware_fault = mode == 1; usb_out_lost = mode == 2; usb_in_lost = mode == 3;
        escape_loop = 1; if (!setjmp(loop_escape)) usb_command_loop(); escape_loop = 0;
        CK(ramdisk_transfer.failed && resident_rd_size == 0);
        CK(usb_out_owned == 71 && usb_in_owned == 72);
    }
    clean(); backing(MAX_MEM_SIZE); fb_command("oem ramdisk"); CK(download_to_ramdisk && !strcmp(answer, "OKAY"));
    rx_left = 8; fb_command("download:8"); CK(resident_rd_size == 8 && ramdisk_transfer.total == 0 && target[7] == 24);
    fb_command("oem ramdisk"); rx_left = 2; fb_command("download:8"); CK(!resident_rd_size && !strncmp(answer, "FAIL", 4));
    fb_command("oem ramdisk"); fb_command("download:100000000"); CK(!strncmp(answer, "FAIL", 4));
    fb_command("oem ramdisk");
    unsigned before_rx = rx_calls, before_data = data_replies;
    uint8_t saved[sizeof(target)]; memcpy(saved, target, sizeof(target));
    fb_command("download:4000001");
    CK(!strncmp(answer, "FAIL", 4) && rx_calls == before_rx && data_replies == before_data);
    CK(!memcmp(saved, target, sizeof(target)));
    fb_command("oem ramdisk none"); CK(!download_to_ramdisk && !resident_rd_size);
    clean(); PLAT = &platform_qemu; backing(MAX_MEM_SIZE);
    fb_command("oem ramdisk"); rx_left = 0;
    fb_command("download:4000001");
    CK(data_replies == 1 && rx_calls == 1 && !resident_rd_size);
    before_rx = rx_calls; before_data = data_replies;
    char over_window[48];
    snprintf(over_window, sizeof(over_window), "download:%llx",
        (unsigned long long)(win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE + 1));
    fb_command("oem ramdisk"); fb_command(over_window);
    CK(!strncmp(answer, "FAIL", 4) && rx_calls == before_rx && data_replies == before_data);
    printf("loader ramdisk C checks: %u passed\n", checks);
    return 0;
}
'''


def test_loader():
    definitions = '\n'.join(re.search(r'^#define ' + key + r'\s+\S+', BASE_SOURCE, re.M).group()
                            for key in ('BASE_ALIGN', 'MAX_MEM_SIZE', 'MAX_MEM_SIZE_LIMIT',
                                        'RESIDENT_RD_OFFSET', 'RESIDENT_RD_RESERVE', 'MAX_RESIDENT_BOOT_SIZE'))
    command = function(USB_SOURCE, 'fb_command')
    command = command[:command.index('\t\t} else if (starts_with(cmd, "upload"))')] + '\t\t}\n\t}\n}\n'
    assert command.index('lc_cmd_denied(') < command.index('ramdisk_command(cmd)')
    source = PRELUDE + definitions + '\n' + function(LAYOUT_SOURCE, 'choose_window')
    for name in ('lc_starts', 'lc_blocked_cmd', 'lc_busy', 'lc_cmd_denied'):
        source += function(BASE_SOURCE, name)
    source += function(USB_SOURCE, 'starts_with')
    source += (ROOT / 'xnuloader/loader_ramdisk.inc').read_text()
    source += command + function(USB_SOURCE, 'usb_command_loop') + SCENARIOS
    with tempfile.TemporaryDirectory(prefix='hackios-ramdisk-test-') as directory:
        cfile = Path(directory) / 'test.c'
        binary = Path(directory) / 'test'
        cfile.write_text(source)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        mutations = {
            'missing streaming boot limit': ('PLAT == &platform_annibale && total > MAX_RESIDENT_BOOT_SIZE', '0'),
            'missing legacy boot limit': ('limit = MAX_RESIDENT_BOOT_SIZE;', 'limit = MAX_MEM_SIZE_LIMIT;'),
            'partial publication': ('if (ramdisk_transfer.received == ramdisk_transfer.total) {',
                                    'if (ramdisk_transfer.received != 0) {'),
            'short block accepted': ('ramdisk_transfer.failed || received != wanted ||',
                                     'ramdisk_transfer.failed ||'),
            'failed session receives data': ('ramdisk_transfer.failed || !ramdisk_hex(&argument, size)',
                                              '!ramdisk_hex(&argument, size)'),
            'missing run refusal': ('ramdisk_pending() && (starts_with(cmd, "oem run")',
                                    '0 && (starts_with(cmd, "oem run")'),
        }
        for name, (before, after) in mutations.items():
            assert source.count(before) == 1, name
            cfile.write_text(source.replace(before, after))
            subprocess.run(['cc', '-std=c11', str(cfile), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True)
            assert result.returncode, name
            print('mutation rejected:', name)


def test_resident_layout():
    payload_source = (ROOT / 'xnuloader/loader_04_fault_payload_nvram.inc').read_text()
    macros = '\n'.join(re.search(r'^#define ' + key + r'\s+\S+', BASE_SOURCE, re.M).group()
                       for key in ('PAYLOAD_MAGIC', 'PAYLOAD_CONTAINER_MAX', 'MAX_RESIDENT_BOOT_SIZE',
                                   'KERNEL_VIRT_BASE', 'PAGE_SIZE_MAX', 'RESIDENT_RD_OFFSET', 'BASE_ALIGN', 'MAX_MEM_SIZE_LIMIT'))
    info = payload_source[payload_source.index('struct payload_info {'):payload_source.index('static uint64_t\nparse_number')]
    layout = LAYOUT_SOURCE[LAYOUT_SOURCE.index('struct run_layout {'):LAYOUT_SOURCE.index('/* F10-PLAN-END */')]
    # Keep the production guard and its immediate I/O consumers in their actual order.
    start = LAYOUT_SOURCE.index('\tuint64_t phys_base = win_base;')
    end = LAYOUT_SOURCE.index('\n\t/* --- load segments', start)
    dispatch = LAYOUT_SOURCE[start:end]
    source = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static uint8_t payload[4096];
#define PAYLOAD_BASE ((uint64_t)(uintptr_t)payload)
static uint64_t payload_bytes, resident_rd_size, win_base = 0x80000000ULL, win_size = 0x180000000ULL;
static const int platform_annibale, platform_qemu;
static const int *PLAT = &platform_annibale;
static unsigned disk_reads, memory_writes;
static void print(const char *format, ...) { (void)format; }
static int str_eq(const char *a, const char *b) { return !strcmp(a, b); }
struct payload_header { uint32_t magic, size; };
struct payload_entry { char name[16]; uint32_t size, reserved; };
''' + macros + '\n' + info
    for name in ('parse_number', 'nul_within', 'parse_payload'):
        source += function(payload_source, name)
    source += layout + r'''
static int rdfile_load(const char *part, uint64_t off, uint64_t size, uint64_t target)
{ (void)part; (void)off; (void)size; (void)target; disk_reads++; return 1; }
static void *count_clear(void *p, int c, size_t n)
{ (void)c; (void)n; memory_writes++; return p; }
static void *count_copy(void *p, const void *q, size_t n)
{ (void)q; (void)n; memory_writes++; return p; }
#undef memset
#undef memcpy
#define memset count_clear
#define memcpy count_copy
static int deliver(struct payload_info pi) {
    uint64_t vm_lo = KERNEL_VIRT_BASE + 0x3000000, vm_hi = vm_lo + 0x1000000;
''' + dispatch + r'''
    memory_writes++; /* The next production step clears and copies kernel segments. */
    return 1;
}
#undef memset
#undef memcpy
static void make_payload(const char *name, const char *text) {
    memset(payload, 0, sizeof(payload));
    struct payload_header *h = (void *)payload;
    struct payload_entry *e = (void *)(h + 1);
    strcpy(e->name, name); e->size = (uint32_t)strlen(text) + 1;
    memcpy(e + 1, text, e->size);
    h->magic = PAYLOAD_MAGIC;
    h->size = sizeof(*e) * 2 + ((e->size + 15) & ~15u);
    payload_bytes = sizeof(*h) + h->size;
    resident_rd_size = disk_reads = memory_writes = 0;
}
int main(void) {
    struct payload_info pi;
    const char *names[] = {"rdres", "rdfile"};
    const char *values[] = {"4000001", "darwin:67108865"};
    for (unsigned i = 0; i < 2; i++) {
        make_payload(names[i], values[i]); assert(parse_payload(&pi, 0));
        assert(!deliver(pi)); assert(!disk_reads && !memory_writes);
    }
    make_payload("rdres", "4000000"); assert(parse_payload(&pi, 0));
    assert(deliver(pi)); assert(!disk_reads && memory_writes == 1);
    make_payload("rdfile", "darwin:67108864"); assert(parse_payload(&pi, 0));
    assert(deliver(pi)); assert(disk_reads == 1 && memory_writes == 1);
    make_payload("rdimage", "boot bytes"); assert(parse_payload(&pi, 0));
    assert(pi.resident_image_size == 11 && !memcmp(pi.resident_image, "boot bytes", 11));
    assert(deliver(pi)); assert(!disk_reads && memory_writes == 3);
    struct run_layout L;
    assert(!plan_layout(win_base, win_size, KERNEL_VIRT_BASE + 0x3000000,
        KERNEL_VIRT_BASE + 0x4000000, 0, 64, MAX_RESIDENT_BOOT_SIZE + 1, &L));
    PLAT = &platform_qemu;
    for (unsigned i = 0; i < 2; i++) {
        make_payload(names[i], values[i]); assert(parse_payload(&pi, 0));
        assert(deliver(pi)); assert(disk_reads == i && memory_writes == 1);
    }
    make_payload("rdres", "180000000"); assert(parse_payload(&pi, 0));
    assert(!deliver(pi)); assert(!disk_reads && !memory_writes);
    puts("resident payload parsing and pre-I/O layout checks: passed");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='hackios-layout-test-') as directory:
        cfile, binary = Path(directory) / 'test.c', Path(directory) / 'test'
        for mutation in (False, True):
            content = source
            if mutation:
                content = content.replace('resident_rd > MAX_RESIDENT_BOOT_SIZE || resident_source_size > MAX_RESIDENT_BOOT_SIZE', '0')
            cfile.write_text(content)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                            str(cfile), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if mutation:
                assert result.returncode, 'missing layout size limit was not detected'
                print('mutation rejected: missing pre-I/O resident size limit')
            else:
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout, end='')


HOST_CODE = (ROOT / 'tools/stage-system.sh').read_text().split("<<'PYTHON'\n", 1)[1].rsplit('\nPYTHON', 1)[0]


def test_host(total, *, capability='1', window=True, held=False):
    calls = []

    class VirtualPath:
        def __init__(self, value): self.value = str(value)
        def __str__(self): return self.value
        def __truediv__(self, name): return VirtualPath(self.value + '/' + name)
        def is_file(self): return True
        def stat(self): return SimpleNamespace(st_size=total, st_mtime_ns=1)
        def open(self, mode): raise AssertionError('rejected image must not be opened')

    def run(command, **kwargs):
        calls.append(command)
        args = command[3:] if command[1:3] == ['-s', 'xnuloader'] else command[1:]
        output = ''
        code = 0
        if args == ['devices']: output = 'xnuloader\tfastboot\n'
        elif args == ['getvar', 'product']: output = 'product: xnuloader\n'
        elif args == ['getvar', 'result']: output = 'result: hung-held\n' if held else 'result: idle\n'
        elif args == ['getvar', 'ramdisk-protocol']: output = 'ramdisk-protocol: ' + capability + '\n'
        elif args[:3] == ['oem', 'ramdisk', 'begin']:
            requested, requested_window = (int(v, 16) for v in args[3:])
            if requested > 64 * 1024 * 1024:
                code = 1; output = 'FAILED invalid ramdisk total/window\n'
        elif args == ['stage', '/virtual/system.apfs'] and total > 64 * 1024 * 1024:
            code = 1; output = 'FAILED bad size\n'
        elif args not in (['oem', 'ramdisk'], ['stage', '/virtual/system.apfs']):
            raise AssertionError(args)
        return SimpleNamespace(returncode=code, stdout=output)

    env = {'STAGE_SYSTEM_IMAGE': '/virtual/system.apfs', 'STAGE_SYSTEM_FASTBOOT': 'fake-fastboot'}
    if window: env['STAGE_SYSTEM_WINDOW'] = '2c0000000'
    status = 0
    output = io.StringIO()
    with patch.dict(os.environ, env, clear=True), patch('pathlib.Path', VirtualPath), \
            patch('subprocess.run', run), \
            contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        try:
            exec(compile(HOST_CODE, 'stage-system.sh', 'exec'), {})
        except SystemExit as error:
            status = error.code
    return status, calls


def main():
    test_loader()
    test_resident_layout()
    status, calls = test_host(1024, capability='', window=False)
    assert status == 0 and not any('ramdisk-protocol' in c or 'ramdisk-state' in c for c in calls)
    assert calls[-2:] == [['fake-fastboot', '-s', 'xnuloader', 'oem', 'ramdisk'],
                         ['fake-fastboot', '-s', 'xnuloader', 'stage', '/virtual/system.apfs']]
    for total in (0x4000001, 0x100000001):
        status, calls = test_host(total)
        assert status == 1
    status, calls = test_host(0x100000001, window=False)
    assert status == 1 and not calls
    status, calls = test_host(0x100000001, capability='')
    assert status == 1
    status, calls = test_host(1024, held=True)
    assert status == 1 and not any('stage' in c or 'oem' in c for c in calls)
    print('stage-system: legacy upload, size refusal, window, capability and held cases passed')


if __name__ == '__main__':
    main()

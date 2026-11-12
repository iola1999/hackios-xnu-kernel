#!/usr/bin/env python3
"""Execute Fastboot selection, menu recovery, fallback and watchdog with a controlled clock."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UI = 'xnuloader/loader_06a_screen_ui.inc'
USB = 'xnuloader/loader_06_usb_entry.inc'
WDT = 'xnuloader/loader_04_fault_payload_nvram.inc'


def function(text, name):
    start = text.index('\n' + name + '(')
    start = text.rfind('\nstatic ', 0, start) + 1
    return text[start:text.index('\n}\n', start) + 3]


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
struct kernel_live { unsigned stop; };
static struct kernel_live live;
static const struct kernel_live *running_live;
static int kernel_resources_held, firmware_fault, held_irq_masked;
static int usb_menu_interrupted, boot_fallback_active, usb_out_lost, usb_in_lost;
static int usb_connected = 1, wdt_armed = 1, usb_run_requested;
static uint64_t host_last_cmd, ticks = 1000, frequency = 100;
static unsigned pets, resets, kernels, command_loops, poll_calls;
static char last_result[64] = "idle", displayed[64];
static struct { unsigned log_seen; int ready, pct, usb_chrome_drawn; uint64_t spin_at; } UI;
static struct { int active; } UI_BOOT;
static struct { uint64_t base; unsigned width, height; } SCREEN = { 1, 1, 1 };
static unsigned log_len = 37;
static uint64_t loader_ctx[2];
enum { CTX_LOG_PA, CTX_LOG_LEN };
#define READ_SYSREG(reg) (!strcmp(#reg, "CNTFRQ_EL0") ? frequency : ticks)
#define HOST_SILENCE_SECONDS 180
#define VIRT_WDT_PET 0x86000007u
#define MENU_CHOICE_IOS 0
#define MENU_CHOICE_ANDROID 1
#define MENU_CHOICE_FASTBOOT 2
#define UI_BG 0
#define EFI_ERROR(s) ((s) != 0)
typedef unsigned efi_status;
typedef struct { unsigned scan_code, unicode_char; } efi_input_key;
typedef struct efi_simple_text_input {
    efi_status (*read_key_stroke)(struct efi_simple_text_input *, efi_input_key *);
} efi_simple_text_input;
static struct { void *con_in; } system_table, *ST = &system_table;
struct spmi_btn { int usable, live, last, down; };
struct spmi_btn_ctx { int pon_ch, pin_ch; struct spmi_btn pwr, vol_up, vol_down; };
static int loader_slot = 'a', android_slot = 'b', interrupt_draw, interrupt_poll, entry_active;
static jmp_buf entry_return;
static const char *menu_enter_fastboot(void);
static void print(const char *format, const char *status) { (void)format; (void)status; }
static void ui_stage(const char *stage) { (void)stage; }
static void ui_draw_fault_banner(const char *status, const char *log, uint64_t length)
{ (void)log; (void)length; strcpy(displayed, status); }
static uint64_t smc_call(uint64_t id, uint64_t a, uint64_t b, void *d, void *e)
{ assert(id == VIRT_WDT_PET); (void)a; (void)b; (void)d; (void)e; pets++; return 0; }
static void reboot(void) { resets++; if (entry_active) longjmp(entry_return, 1); }
static void slot_detect(void) {}
static void slot_set_active_other(void) {}
static void slot_maintain_current(void) {}
static void spmi_buttons_init(struct spmi_btn_ctx *ctx) { memset(ctx, 0, sizeof(*ctx)); }
static int spmi_obsrv_read(int ch, uint8_t reg, uint8_t *buf, unsigned n)
{ (void)ch; (void)reg; (void)buf; (void)n; return 0; }
static int spmi_btn_edge(struct spmi_btn *b, int pressed) { (void)b; (void)pressed; return 0; }
static uint64_t ui_now(void) { return ticks++; }
static uint64_t ui_freq(void) { return frequency; }
static void ui_fill(int x, int y, int w, int h, unsigned color)
{ (void)x; (void)y; (void)w; (void)h; (void)color; }
static void ui_draw_menu(int choice, int countdown, int interacted)
{ (void)choice; (void)countdown; (void)interacted; if (interrupt_draw) assert(!menu_enter_fastboot()); }
static void ui_draw_menu_footer(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
static void ui_menu_select(int *choice, int next, int countdown, int *interacted)
{ *choice = next; *interacted = 1; (void)countdown; }
static void ui_draw_ios_boot_splash(void) {}
static void usb_poll_command(void)
{ poll_calls++; if (interrupt_poll && poll_calls == 1) assert(!menu_enter_fastboot()); }
static void *USB = &live;
static int usb_start(void) { return 1; }
static void flush_log(void) {}
static void usb_command_loop(void) { command_loops++; longjmp(entry_return, 1); }
static int run_kernel(int cp, uint64_t n) { (void)cp; (void)n; kernels++; return 1; }
'''

SCENARIOS = r'''
static void elapsed(unsigned seconds)
{
    ticks = host_last_cmd + (uint64_t)seconds * frequency;
    pets = resets = 0;
    boot_fallback_check();
    wdt_pet();
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *name = argv[1];
    if (!strcmp(name, "recovery")) {
        boot_fallback_enter();
        assert(!menu_enter_fastboot());
        for (unsigned sec = 121; sec <= 86400; sec *= 2) {
            elapsed(sec); assert(!resets && pets == 1);
        }
        usb_connected = 0;
        elapsed(86400); assert(!resets && pets == 1);
        wdt_armed = 0; elapsed(86400); assert(!pets);
    } else if (!strcmp(name, "fallback")) {
        assert(!menu_enter_fastboot());
        boot_fallback_enter();
        assert(!usb_menu_interrupted);
        elapsed(121); assert(resets == 1 && pets == 1);
        elapsed(181); assert(resets == 1 && !pets);
        usb_menu_interrupted = 1;
        assert(!fastboot_idle_resident());
    } else if (!strcmp(name, "unselected")) {
        elapsed(181); assert(!resets && !pets);
    } else if (!strcmp(name, "ownership")) {
        for (unsigned state = 1; state < 64; ++state) {
            running_live = state & 1 ? &live : NULL;
            kernel_resources_held = (state >> 1) & 1;
            firmware_fault = (state >> 2) & 1;
            held_irq_masked = (state >> 3) & 1;
            usb_out_lost = (state >> 4) & 1;
            usb_in_lost = (state >> 5) & 1;
            usb_menu_interrupted = 1;
            assert(!fastboot_idle_resident());
            if (state & 15) assert(menu_enter_fastboot() != NULL);
            elapsed(181);
            assert(pets == (running_live != NULL));
        }
    } else if (!strcmp(name, "menu-draw") || !strcmp(name, "menu-poll")) {
        interrupt_draw = !strcmp(name, "menu-draw");
        interrupt_poll = !strcmp(name, "menu-poll");
        assert(ui_boot_menu() == MENU_CHOICE_FASTBOOT);
        assert(usb_menu_interrupted && fastboot_idle_resident());
    } else if (!strcmp(name, "run-revokes")) {
        assert(!menu_enter_fastboot());
        run_selected();
        assert(kernels == 1 && !usb_menu_interrupted);
        elapsed(181); assert(!pets);
    } else {
        int choice = !strcmp(name, "manual") ? MENU_CHOICE_FASTBOOT :
            !strcmp(name, "android") ? MENU_CHOICE_ANDROID : MENU_CHOICE_IOS;
        int have = !strcmp(name, "standalone"), usb_mode = !strcmp(name, "usb-mode");
        entry_active = 1;
        if (!setjmp(entry_return)) entry_after_menu(choice, have || usb_mode, usb_mode);
        entry_active = 0;
        if (!strcmp(name, "android")) {
            assert(resets == 1 && !command_loops && !usb_menu_interrupted);
        } else if (have) {
            assert(kernels == 1 && command_loops == 1 && boot_fallback_active);
            elapsed(181); assert(resets == 1 && !pets);
        } else {
            assert(!kernels && command_loops == 1 && fastboot_idle_resident());
            elapsed(86400); assert(!resets && pets == 1);
        }
    }
    puts(name);
}
'''


def harness(sources):
    ui, usb, wdt = (sources[p] for p in (UI, USB, WDT))
    residency = function(usb, 'fastboot_idle_resident') if '\nfastboot_idle_resident(' in usb else (
        'static int fastboot_idle_resident(void) { return 0; }\n')
    body = '\n'.join(function(ui, n) for n in (
        'menu_enter_fastboot', 'boot_fallback_enter', 'boot_fallback_check', 'ui_boot_menu'))
    body += residency + function(wdt, 'wdt_pet')
    start = usb.index('\tif (menu_choice == MENU_CHOICE_ANDROID)')
    end = usb.index('\n\tfor (;;) {', start)
    body += ('static void entry_after_menu(int menu_choice, int have_payload, int usb_mode)\n{\n'
             'struct { int usb_mode; } pi = { usb_mode };\n' + usb[start:end] + '\n}\n')
    start = usb.index('\t\t\tusb_run_requested = 1;')
    end = usb.index('\n\t\t\tif (!ok)', start)
    body += ('static void run_selected(void) { int have_cp = 0; uint64_t cp = 0;\n' +
             usb[start:end] + '\n(void)ok;\n}\n')
    return PRELUDE + body + SCENARIOS


CASES = ['recovery', 'fallback', 'unselected', 'ownership', 'menu-draw', 'menu-poll',
         'run-revokes', 'manual', 'android', 'standalone', 'loader-only', 'usb-mode']


def run(sources, path):
    path.mkdir(exist_ok=True)
    c, binary = path / 'test.c', path / 'test'
    c.write_text(harness(sources))
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(c), '-o', str(binary)], check=True)
    results = []
    for name in CASES:
        result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=10)
        results.append({'name': name, 'passed': result.returncode == 0,
                        'returncode': result.returncode, 'stderr': result.stderr[-500:]})
    return results


def verify(args, directory):
    sources = {p: (ROOT / p).read_text() for p in (UI, USB, WDT)}
    results = {'candidate': run(sources, directory / 'candidate')}
    assert all(row['passed'] for row in results['candidate']), results['candidate']
    if args.baseline_ref:
        old = {p: subprocess.check_output(['git', 'show', args.baseline_ref + ':' + p],
                                         cwd=ROOT, text=True) for p in sources}
        results['baseline'] = run(old, directory / 'baseline')
        for name in ('recovery', 'menu-draw', 'run-revokes', 'manual', 'loader-only', 'usb-mode'):
            assert not next(row for row in results['baseline'] if row['name'] == name)['passed'], name
    if args.mutations:
        mutations = {
            'fallback_not_cleared': (UI, '\tboot_fallback_active = 0;', ''),
            'fallback_keeps_residency': (UI, '\tboot_fallback_active = 1;\n\tusb_menu_interrupted = 0;', '\tboot_fallback_active = 1;'),
            'watchdog_silence': (WDT, 'fastboot_idle_resident() ||', ''),
            'manual_not_resident': (USB, '} else if (menu_choice == MENU_CHOICE_FASTBOOT) {\n\t\t(void)menu_enter_fastboot();', '} else if (menu_choice == MENU_CHOICE_FASTBOOT) {'),
            'loader_only_not_resident': (USB, '\t\tif (!boot_fallback_active) (void)menu_enter_fastboot();', ''),
            'run_keeps_residency': (USB, '\t\t\tusb_menu_interrupted = 0;', ''),
            'menu_draw_clears': (UI, '\tusb_menu_interrupted = 0;\n\tui_fill', '\tui_fill'),
        }
        for gate in ('usb_menu_interrupted', '!boot_fallback_active', 'running_live == NULL',
                     '!kernel_resources_held', '!firmware_fault', '!held_irq_masked',
                     '!usb_out_lost', '!usb_in_lost'):
            mutations['gate_' + gate.replace('!', '').replace(' ', '_')] = (USB, gate, '1')
        for name, (path, old, new) in mutations.items():
            changed = dict(sources)
            if name.startswith('gate_'):
                original = function(changed[path], 'fastboot_idle_resident')
                changed[path] = changed[path].replace(original, original.replace(old, new, 1), 1)
            elif name == 'menu_draw_clears':
                changed[path] = changed[path].replace(old, new, 1).replace(
                    '\tui_draw_menu(choice, countdown, user_interacted);',
                    '\tui_draw_menu(choice, countdown, user_interacted);\n\tusb_menu_interrupted = 0;', 1)
            else:
                assert old in changed[path], name
                changed[path] = changed[path].replace(old, new, 1)
            results[name] = run(changed, directory / name)
            assert any(not row['passed'] for row in results[name]), name
    (directory / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'PASS: {len(CASES)} Fastboot residency scenarios; {len(results) - 1} comparison variants')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref')
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        verify(args, args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix='loader-residency-') as directory:
            verify(args, Path(directory))


if __name__ == '__main__':
    main()

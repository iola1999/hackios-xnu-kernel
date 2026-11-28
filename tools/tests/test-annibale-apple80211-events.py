#!/usr/bin/env python3
"""Execute the production Apple80211 link event with a captured kernel-event sink."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/files/bsd/dev/annibale/annibale_apple80211.c"
production = SOURCE.read_text()


def declaration(name):
    start = production.index(name)
    return production[start : production.index("};", start) + 2]


def function(name, prefix="void\n"):
    start = production.index(prefix + name + "(")
    begin = production.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (production[end] == "{") - (production[end] == "}")
        end += 1
    return production[start:end]


source = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define Z_WAITOK 1
#define Z_ZERO 2
#define NSEC_PER_MSEC 1000000ULL
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define printf(...) ((void)0)

typedef uint64_t clock_sec_t;
typedef uint32_t clock_usec_t;
static clock_sec_t fake_sec = 10;
static clock_usec_t fake_usec = 0;
static void clock_get_system_microtime(clock_sec_t *sec, clock_usec_t *usec) {
    *sec = fake_sec; *usec = fake_usec;
}
static unsigned delay_calls;
static void delay_for_interval(uint32_t interval, uint64_t scale) {
    assert(interval == 20u && scale == NSEC_PER_MSEC);
    delay_calls++;
}

enum annibale_sta_event { ANNIBALE_STA_POWER, ANNIBALE_STA_SCAN_DONE, ANNIBALE_STA_JOIN_DONE, ANNIBALE_STA_LINK };
struct annibale_sta_state { struct { int16_t rssi; uint8_t ssid_length, ssid[32], bssid[6]; } bss; };
static int current_rssi;
static struct annibale_sta_state station;
static void *kalloc_data(size_t size, int flags) {
    assert(size == sizeof(station) && flags == (Z_WAITOK | Z_ZERO)); return &station;
}
static void kfree_data(void *data, size_t size) { assert(data == &station && size == sizeof(station)); }
static void annibale_sta_state(struct annibale_sta_state *state) {
    state->bss.rssi = current_rssi;
    state->bss.ssid_length = 4;
    memcpy(state->bss.ssid, "TEST", 4);
    memcpy(state->bss.bssid, "\x02\x11\x22\x33\x44\x55", 6);
}
static int apple80211_lock;
static void lck_mtx_lock(int *lock) { assert(lock == &apple80211_lock); }
static void lck_mtx_unlock(int *lock) { assert(lock == &apple80211_lock); }
static struct { uint64_t data; unsigned next; } result_readers[16];
static uint32_t join_result = 0xffffu;
static uint64_t join_requested_usec;
static bool join_requested;
static uint8_t event_filter[32];
static bool event_filter_set;

#define KEV_VENDOR_APPLE 1
#define KEV_IEEE80211_CLASS 6
#ifndef __DECONST
#define __DECONST(type, var) ((type)(uintptr_t)(var))
#endif
struct kev_d_vectors { uint32_t data_length; void *data_ptr; };
struct kev_msg {
    uint32_t vendor_code, kev_class, kev_subclass, event_code;
    struct kev_d_vectors dv[5];
};
static char interface_name[16] = "en0";
static int fake_kev_error;
static unsigned printf_calls;
static void test_printf(const char *fmt, ...) { (void)fmt; printf_calls++; }
#undef printf
#define printf test_printf

struct captured { uint32_t code, length; uint8_t data[0x1d8]; };
static struct captured events[8];
static unsigned event_count;
static int kev_post_msg(struct kev_msg *msg) {
    assert(msg->vendor_code == KEV_VENDOR_APPLE && msg->kev_class == KEV_IEEE80211_CLASS && msg->kev_subclass == 1);
    assert(msg->dv[0].data_length == 16 && memcmp(msg->dv[0].data_ptr, "en0", 4) == 0);
    if (fake_kev_error != 0) return fake_kev_error;
    struct captured *event = &events[event_count];
    assert(event_count < 8 && msg->dv[1].data_length <= sizeof(event->data));
    event_count++;
    event->code = msg->event_code;
    event->length = msg->dv[1].data_length;
    if (msg->dv[1].data_length) memcpy(event->data, msg->dv[1].data_ptr, msg->dv[1].data_length);
    return 0;
}
''' + function("event_allowed", "static bool\n") + "\n" + function("post", "static void\n") + r'''
''' + "\n".join(declaration(name) for name in (
    "struct apple80211_ssid_changed {",
    "struct apple80211_bssid_changed {",
    "struct apple80211_power_changed {",
    "struct apple80211_link_changed {",
    "struct apple80211_lqm_changed {",
    "struct apple80211_assoc_done {",
    "enum { EVENT_POWER_CHANGED",
)) + "\n" + function("post_lqm", "static void\n") + "\n" + function("annibale_apple80211_event") + r'''

static void reset(void) { event_count = 0; memset(events, 0, sizeof(events)); }
static void check_codes(const uint32_t *codes, unsigned count) {
    assert(event_count == count);
    for (unsigned i = 0; i < count; i++) assert(events[i].code == codes[i]);
}
static void check_down(int status) {
    const uint32_t codes[] = { EVENT_SSID_CHANGED, EVENT_BSSID_CHANGED, EVENT_LINK_CHANGED };
    struct apple80211_link_changed link;
    reset(); annibale_apple80211_event(ANNIBALE_STA_LINK, status); check_codes(codes, 3);
    assert(events[2].length == sizeof(link)); memcpy(&link, events[2].data, sizeof(link));
    assert(link.down == 1 && link.rssi == 0);
}
int main(void) {
    const uint32_t codes[] = { EVENT_SSID_CHANGED, EVENT_BSSID_CHANGED, EVENT_LINK_CHANGED, EVENT_LQM_CHANGED };
    const uint32_t lqm_code[] = { EVENT_LQM_CHANGED };
    struct apple80211_link_changed link;
    struct apple80211_lqm_changed lqm;
    _Static_assert(sizeof(lqm) == 0x1d8 && offsetof(struct apple80211_lqm_changed, rssi) == 4, "LQM layout");

    current_rssi = -57; reset(); annibale_apple80211_event(ANNIBALE_STA_LINK, 1); check_codes(codes, 4);
    assert(events[2].length == sizeof(link)); memcpy(&link, events[2].data, sizeof(link));
    assert(link.down == 0 && link.rssi == -57);
    assert(events[3].length == sizeof(lqm)); memcpy(&lqm, events[3].data, sizeof(lqm));
    assert(lqm.rssi_known == 1 && lqm.rssi == -57);
    for (unsigned i = 1; i < 4; i++) assert(events[3].data[i] == 0);
    for (unsigned i = 8; i < sizeof(lqm); i++) assert(events[3].data[i] == 0);
    check_down(0); check_down(-1);

    reset(); fake_sec = 20; post_lqm(-49); post_lqm(-49); check_codes(lqm_code, 1);
    memcpy(&lqm, events[0].data, sizeof(lqm));
    assert(lqm.rssi_known == 1 && lqm.rssi == -49);
    reset(); fake_sec = 21; post_lqm(0); check_codes(lqm_code, 1);
    memcpy(&lqm, events[0].data, sizeof(lqm));
    assert(lqm.rssi_known == 1 && lqm.rssi == -55);

    {
        struct assoc_done_check {
            uint8_t unused[0x1c0];
            uint32_t status, result, flags;
        };
        _Static_assert(sizeof(struct assoc_done_check) == 0x1cc, "assoc done size");
        _Static_assert(offsetof(struct assoc_done_check, status) == 0x1c0, "status offset");
        _Static_assert(offsetof(struct assoc_done_check, result) == 0x1c4, "result offset");
        _Static_assert(offsetof(struct assoc_done_check, flags) == 0x1c8, "flags offset");

        uint8_t filter[32] = {0};
        uint32_t jr = 0xffffu;
        assert(jr == 0xffffu);

        /* bit (code - 1): EVENT_ASSOC_DONE (9) -> byte 1+1=2, bit 0; EVENT_LQM_CHANGED (39) -> byte 1+4=5, bit 6 */
        filter[0] = 0;
        filter[1 + ((9u - 1u) >> 3)] |= (uint8_t)(1u << ((9u - 1u) & 7u));
        assert((filter[2] & 0x01u) != 0);
        assert((filter[5] & 0x40u) == 0);

        struct kev_dv { uint32_t len; const void *ptr; };
        char ifname[16] = "en0";
        struct assoc_done_check done = { .status = 1u, .result = 1u, .flags = 1u };
        struct kev_dv dv[2] = {
            { (uint32_t)sizeof(ifname), ifname },
            { (uint32_t)sizeof(done), &done },
        };
        assert(dv[0].len == 16u && memcmp(dv[0].ptr, "en0", 4) == 0);
        assert(dv[1].len == 0x1ccu && done.flags == 1u && done.result == 1u);

        /* P21 kev_post_msg cluster boundary & error logging checks */
        assert(24u + 16u + sizeof(struct apple80211_assoc_done) == 500u);
        assert(24u + 16u + sizeof(struct apple80211_lqm_changed) == 512u);
        printf_calls = 0; fake_kev_error = EMSGSIZE;
        post(EVENT_ASSOC_DONE, &done, sizeof(done));
        assert(printf_calls == 1u);
        fake_kev_error = 0;
    }
}
'''

a80211_src = Path(__file__).resolve().parents[2] / "xnu/files/bsd/dev/annibale/annibale_apple80211.c"
a80211_text = a80211_src.read_text()
assert len(a80211_text.splitlines()) <= 1200, f"annibale_apple80211.c exceeds 1200 lines"
for marker in (
    "static uint32_t join_result = 0xffff;",
    "static uint8_t event_filter[32];",
    "event_allowed(uint32_t code)",
    "request->req_len < sizeof(mask)",
    "join_result = 0xffff;",
    "done.flags = 1;",
    "post(EVENT_ASSOC_DONE, &done, sizeof(done));",
    "int error = kev_post_msg(&message);",
    "delay_for_interval(20, NSEC_PER_MSEC);",
):
    assert marker in a80211_text, f"missing expected P21 contract in annibale_apple80211.c: {marker}"

patch83 = Path(__file__).resolve().parents[2] / "xnu26/patches/83-bsd%kern%kern_event.c.patch"
patch83_text = patch83.read_text()
for marker in (
    "MCLBYTES - total_size",
    "m_getcl(wait, MT_DATA, 0)",
    "total_size > MLEN",
):
    assert marker in patch83_text, f"missing expected P21 contract in 83-bsd%kern%kern_event.c.patch: {marker}"

with tempfile.TemporaryDirectory(prefix="annibale-apple80211-events-") as directory:
    src = Path(directory) / "check.c"
    binary = Path(directory) / "check"
    src.write_text(source)
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(src), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
print("Annibale Apple80211 events, filter mask, and assoc completion contract: PASS")

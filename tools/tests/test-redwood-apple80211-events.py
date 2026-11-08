#!/usr/bin/env python3
"""Execute the production Apple80211 link event with a captured kernel-event sink."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/files/bsd/dev/redwood/redwood_apple80211.c"
production = SOURCE.read_text()


def declaration(name):
    start = production.index(name)
    return production[start : production.index("};", start) + 2]


def function(name):
    start = production.index("void\n" + name + "(")
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
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define Z_WAITOK 1
#define Z_ZERO 2
#define printf(...) ((void)0)

enum redwood_sta_event { REDWOOD_STA_POWER, REDWOOD_STA_SCAN_DONE, REDWOOD_STA_JOIN_DONE, REDWOOD_STA_LINK };
struct redwood_sta_state { struct { int16_t rssi; } bss; };
static int current_rssi;
static struct redwood_sta_state station;
static void *kalloc_data(size_t size, int flags) {
    assert(size == sizeof(station) && flags == (Z_WAITOK | Z_ZERO)); return &station;
}
static void kfree_data(void *data, size_t size) { assert(data == &station && size == sizeof(station)); }
static void redwood_sta_state(struct redwood_sta_state *state) { state->bss.rssi = current_rssi; }
static int apple80211_lock;
static void lck_mtx_lock(int *lock) { assert(lock == &apple80211_lock); }
static void lck_mtx_unlock(int *lock) { assert(lock == &apple80211_lock); }
static unsigned result_next;
static uint32_t join_result;

struct captured { uint32_t code, length; uint8_t data[0x1d8]; };
static struct captured events[8];
static unsigned event_count;
static void post(uint32_t code, const void *data, uint32_t length) {
    struct captured *event = &events[event_count];
    assert(event_count < 8 && length <= sizeof(event->data));
    event_count++;
    event->code = code; event->length = length;
    if (length) memcpy(event->data, data, length);
}
''' + "\n".join(declaration(name) for name in (
    "struct apple80211_link_changed {",
    "struct apple80211_lqm_changed {",
    "struct apple80211_assoc_done {",
    "enum { EVENT_POWER_CHANGED",
)) + "\n" + function("redwood_apple80211_event") + r'''

static void reset(void) { event_count = 0; memset(events, 0, sizeof(events)); }
static void check_codes(const uint32_t *codes, unsigned count) {
    assert(event_count == count);
    for (unsigned i = 0; i < count; i++) assert(events[i].code == codes[i]);
}
static void check_down(int status) {
    const uint32_t codes[] = { EVENT_SSID_CHANGED, EVENT_BSSID_CHANGED, EVENT_LINK_CHANGED };
    struct apple80211_link_changed link;
    reset(); redwood_apple80211_event(REDWOOD_STA_LINK, status); check_codes(codes, 3);
    assert(events[2].length == sizeof(link)); memcpy(&link, events[2].data, sizeof(link));
    assert(link.down == 1 && link.rssi == 0);
}
int main(void) {
    const uint32_t codes[] = { EVENT_SSID_CHANGED, EVENT_BSSID_CHANGED, EVENT_LINK_CHANGED, EVENT_LQM_CHANGED };
    struct apple80211_link_changed link;
    struct apple80211_lqm_changed lqm;
    _Static_assert(sizeof(lqm) == 0x1d8 && offsetof(struct apple80211_lqm_changed, rssi) == 4, "LQM layout");

    current_rssi = -57; reset(); redwood_apple80211_event(REDWOOD_STA_LINK, 1); check_codes(codes, 4);
    assert(events[2].length == sizeof(link)); memcpy(&link, events[2].data, sizeof(link));
    assert(link.down == 0 && link.rssi == -57);
    assert(events[3].length == sizeof(lqm)); memcpy(&lqm, events[3].data, sizeof(lqm));
    assert(lqm.rssi_known == 1 && lqm.rssi == -57);
    for (unsigned i = 1; i < 4; i++) assert(events[3].data[i] == 0);
    for (unsigned i = 8; i < sizeof(lqm); i++) assert(events[3].data[i] == 0);
    check_down(0); check_down(-1);
}
'''

with tempfile.TemporaryDirectory(prefix="redwood-apple80211-events-") as directory:
    src = Path(directory) / "check.c"
    binary = Path(directory) / "check"
    src.write_text(source)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Redwood Apple80211 LQM event layout and ordering: PASS")

#!/usr/bin/env python3
"""Read ordinary H4 event fragments through the boot-time receiver and the production event parser."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/files/bsd/dev/annibale/annibale_bt_hci.c"
STUBS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define H4_EVENT 4
#define H4_ACL 2
#define H4_SCO 3
#define QCA_DEBUG_HANDLE 0x2edc
#define BT_UART_RX_BYTES 4096
#define NSEC_PER_MSEC 1000000
#define NSEC_PER_USEC 1000
#define bt_say(...) ((void)0)
#define annibale_bt_hex(...) ((void)0)

static uint8_t buffer[BT_UART_RX_BYTES], later[BT_UART_RX_BYTES];
static uint32_t available, remaining;
static uint64_t clock_now;
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *out) { *out = ns; }
static uint64_t mach_absolute_time(void) { return ++clock_now; }
static void delay_for_interval(uint64_t n, uint64_t units) { clock_now += n * units; }
static const uint8_t *annibale_bt_uart_rx(uint32_t *n) { *n = available; return buffer; }
static void annibale_bt_uart_take(uint32_t n) {
    assert(n <= available);
    available -= n;
    memmove(buffer, buffer + n, available);
}
static uint32_t annibale_bt_uart_poll(void) {
    uint32_t n = remaining;
    assert(n <= sizeof(buffer) - available);
    memcpy(buffer + available, later, n);
    available += n;
    remaining = 0;
    return n;
}
static bool annibale_bt_uart_write(const uint8_t *data, uint32_t n) {
    (void)data; (void)n;
    assert(0);
    return false;
}
"""
TEST = r"""
int main(void) {
    uint8_t vendor[258] = {4, 0xff}, reply[] = {4, 0x0e, 4, 1, 0x00, 0xfc, 0};
    uint8_t out[260];
    const uint8_t lengths[] = {0, 198, 246, 255};
    for (unsigned j = 0; j < sizeof(lengths); j++) {
        vendor[2] = lengths[j];
        uint32_t n = 3u + vendor[2];
        for (uint32_t prefix = 0; prefix <= 2; prefix++) {
            for (uint32_t split = 0; split <= n; split++) {
                available = 0;
                for (uint32_t i = 0; i < prefix; i++) {
                    memcpy(buffer + available, vendor, n);
                    available += n;
                }
                memcpy(buffer + available, vendor, split);
                available += split;
                discard_startup_rx();
                assert(available == (split == n ? 0 : split));
                memcpy(later, vendor + split, n - split);
                memcpy(later + n - split, reply, sizeof(reply));
                remaining = n - split + sizeof(reply);
                if (split < n) {
                    assert(hci_event(out, sizeof(out), 500) == n);
                    assert(memcmp(out, vendor, n) == 0);
                }
                assert(hci_event(out, sizeof(out), 500) == sizeof(reply));
                assert(memcmp(out, reply, sizeof(reply)) == 0 && available == 0);
            }
        }
    }
    return 0;
}
"""


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


source = SOURCE.read_text()
parts = [STUBS, function(source, "static uint32_t\nhci_event("),
         function(source, "static void\ndiscard_startup_rx("), TEST]
with tempfile.TemporaryDirectory() as directory:
    cfile = Path(directory) / "startup.c"
    binary = cfile.with_suffix("")
    cfile.write_text("\n".join(parts))
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("H4 startup event reassembly and the following command response passed")

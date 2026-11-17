#!/usr/bin/env python3
"""Verify that annibale_bt_hci.c's hci_event() frames H4 packets by their length.

After the rampatch the WCN7850 sends its log as ACL packets on handle 0x2edc. Their payload holds 04 bytes; a reader
that takes any 04 for an event's start reads a wrong length and swallows the HCI Reset's Command Complete behind it
(kernel log: "bt: stop: reset: HCI Reset after the firmware failed (0 bytes, status 0xff)").

hci_event() and the defines it uses are cut out of the kernel source and run against a fake UART that hands over the
chip's bytes four at a time. --source FILE runs the cases against another copy of annibale_bt_hci.c.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HCI_C = ROOT / "xnu/files/bsd/dev/annibale/annibale_bt_hci.c"
PRIVATE_H = ROOT / "xnu/files/bsd/dev/annibale/annibale_bt_private.h"

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_USEC 1000ULL
#ifndef BT_UART_RX_BYTES
#define BT_UART_RX_BYTES 4096u
#endif
@DEFINES@

static uint8_t chip[8192];
static uint32_t chip_len, chip_pos;
static uint8_t rx[BT_UART_RX_BYTES];
static uint32_t rx_len, acks;
static uint64_t now_ns;

static uint64_t mach_absolute_time(void) { return now_ns; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *t) { *t = ns; }
static void delay_for_interval(uint64_t n, uint64_t unit) { now_ns += n * unit; }
static uint32_t annibale_bt_uart_poll(void)
{
	uint32_t got = 0;

	while (got < 4 && chip_pos < chip_len && rx_len < sizeof(rx)) {
		rx[rx_len++] = chip[chip_pos++];
		got++;
	}
	return got;
}
static const uint8_t *annibale_bt_uart_rx(uint32_t *length) { *length = rx_len; return rx; }
static void annibale_bt_uart_take(uint32_t n)
{
	n = n < rx_len ? n : rx_len;
	memmove(rx, rx + n, rx_len - n);
	rx_len -= n;
}
static bool annibale_bt_uart_write(const uint8_t *data, uint32_t n) { acks += n == 1 && data[0] == 0xfc; return true; }
static void annibale_bt_hex(const char *what, const uint8_t *data, uint32_t n) { (void)data; printf("  %s: %u bytes\n", what, n); }

static void feed(const uint8_t *bytes, uint32_t n) { memcpy(chip + chip_len, bytes, n); chip_len += n; }
static void reset_chip(void) { chip_len = chip_pos = rx_len = acks = 0; now_ns = 0; }

@FUNCTION@

static const uint8_t reset_done[7] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };

static void expect_reset_done(const char *name)
{
	uint8_t ev[64];
	uint32_t n = hci_event(ev, sizeof(ev), 3000);

	printf("%s: %u bytes\n", name, n);
	assert(n == sizeof(reset_done));
	assert(memcmp(ev, reset_done, sizeof(reset_done)) == 0);
}

int main(void)
{
	/* 1. two SoC log packets (ACL 0x2edc) whose payload holds 04, then the Command Complete */
	static const uint8_t log1[] = { 0x02, 0xdc, 0x2e, 0x18, 0x00, 0x14, 0x00, 0xff, 0x01, 0x00, 0x19, 0x33, 0x07,
	    0x04, 0x0c, 0x40, 0x00, 0x60, 0x14, 0x00, 0x80, 0x00, 0x00, 0x00, 0x70, 0x0f, 0x00, 0x04, 0x3c };
	static const uint8_t log2[] = { 0x02, 0xdc, 0x2e, 0x08, 0x00, 0x04, 0xff, 0x00, 0x00, 0x89, 0x00, 0x04, 0x00 };
	reset_chip();
	feed(log1, sizeof(log1));
	feed(log2, sizeof(log2));
	feed(reset_done, sizeof(reset_done));
	expect_reset_done("log packets ahead");

	/* 2. in-band sleep bytes and a wake indication between packets: dropped, the wake acknowledged */
	static const uint8_t ibs[] = { 0xfe, 0xfd, 0xfc };
	reset_chip();
	feed(ibs, sizeof(ibs));
	feed(log2, sizeof(log2));
	feed(reset_done, sizeof(reset_done));
	expect_reset_done("in-band sleep bytes");
	assert(acks == 1);

	/* 3. a 02 of line noise announcing more than the buffer holds: dropped as one byte */
	static const uint8_t noise[] = { 0x02, 0x00, 0x00, 0xff, 0xff };
	reset_chip();
	feed(noise, sizeof(noise));
	feed(reset_done, sizeof(reset_done));
	expect_reset_done("noise with a huge length");

	/* 4. a log packet still arriving when the time is up: no event */
	reset_chip();
	feed(log1, 10);
	{
		uint8_t ev[64];
		assert(hci_event(ev, sizeof(ev), 5) == 0);
	}
	puts("PASS");
	return 0;
}
"""


def cut_function(text: str, name: str) -> str:
    m = re.search(r"^static uint32_t\n" + name + r"\(.*?^}\n", text, re.S | re.M)
    if not m:
        sys.exit(f"{name} not found")
    return m.group(0)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, default=HCI_C)
    args = ap.parse_args()
    text = args.source.read_text()
    defines = "\n".join(re.findall(r"^#define (?:H4_\w+|QCA_DEBUG_HANDLE)\s+.*$", text, re.M))
    rx_bytes = re.search(r"^#define BT_UART_RX_BYTES\s+.*$", PRIVATE_H.read_text(), re.M)
    if rx_bytes:
        defines = rx_bytes.group(0) + "\n" + defines
    source = HARNESS.replace("@DEFINES@", defines).replace("@FUNCTION@", cut_function(text, "hci_event"))
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "bt_hci_event.c"
        exe = Path(td) / "bt_hci_event"
        c.write_text(source)
        subprocess.run(["clang", "-std=c11", "-O1", "-Wall", "-Wno-unused-function", str(c), "-o", str(exe)],
                       check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())

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
IOS_C = ROOT / "xnu/files/bsd/dev/annibale/annibale_bt_ios.c"
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


IOS_HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BT_RING        (16u << 10)
#define IBS_WAKE_ACK   0xfc
#define IBS_WAKE_IND   0xfd
#define IBS_SLEEP_IND  0xfe
#define NSEC_PER_MSEC  1000000ULL
#define NVM_TAG_BD_ADDR    2
#define NVM_TAG_HCI        17
#define NVM_TAG_DEEP_SLEEP 27
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define bzero(p, n) memset((p), 0, (n))
#define bt_say(...) ((void)0)
#define BTSK_SAY(...) ((void)0)

typedef int errno_t;
typedef void *lck_mtx_t;
typedef void *kern_nexus_provider_t;
typedef void *kern_nexus_t;
typedef void *kern_channel_t;
typedef void *kern_pbufpool_t;
typedef void *proc_t;
typedef uint32_t nexus_port_t;
struct selinfo { int dummy; };
struct edit { uint32_t at; uint8_t value; };

static uint8_t bt_ring_mem[BT_RING];
static uint8_t *bt_ring = bt_ring_mem;
static uint32_t bt_ring_in, bt_ring_out, bt_rx_lost;
static lck_mtx_t bt_ring_lock_obj;
static lck_mtx_t *bt_ring_lock = &bt_ring_lock_obj;
static struct selinfo bt_sel;
static int bt_serving = 1;
static bool bt_ibs_asleep;
static kern_pbufpool_t btsk_rx_pp;
static kern_channel_t btsk_active_ch;
static volatile int btsk_up, bt_kctl_up;

static uint8_t mock_uart_rx[8192];
static uint32_t mock_uart_rx_len;
static bool mock_uart_write_ok = true;
static uint32_t mock_uart_writes;
static uint32_t mock_baud = 115200u;
static uint8_t mock_bd_addr[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

static void lck_mtx_lock(lck_mtx_t *l) { (void)l; }
static void lck_mtx_unlock(lck_mtx_t *l) { (void)l; }
static void wakeup(void *ch) { (void)ch; }
static void selwakeup(struct selinfo *s) { (void)s; }
static void delay_for_interval(uint64_t n, uint64_t u) { (void)n; (void)u; }
static void btsk_kick(void) {}
static bool PE_parse_boot_argn(const char *k, void *v, int sz) { (void)k; (void)v; (void)sz; return false; }
static bool PE_parse_boot_arg_str(const char *k, char *v, int sz) { (void)k; (void)v; (void)sz; return false; }
static uint32_t annibale_bt_uart_poll(void) { return mock_uart_rx_len; }
static const uint8_t *annibale_bt_uart_rx(uint32_t *len) { *len = mock_uart_rx_len; return mock_uart_rx; }
static void annibale_bt_uart_take(uint32_t n) {
    if (n > mock_uart_rx_len) n = mock_uart_rx_len;
    memmove(mock_uart_rx, mock_uart_rx + n, mock_uart_rx_len - n);
    mock_uart_rx_len -= n;
}
static bool annibale_bt_uart_write(const uint8_t *d, uint32_t n) {
    (void)d; (void)n; mock_uart_writes++; return mock_uart_write_ok;
}
static uint32_t annibale_bt_uart_get_baud(void) { return mock_baud; }
static bool annibale_bt_uart_set_baud(uint32_t b) { mock_baud = b; return true; }
static errno_t kern_nexus_get_pbufpool(kern_nexus_t nx, kern_pbufpool_t *tx, kern_pbufpool_t *rx) {
    (void)nx; *tx = (void *)0x1001; *rx = (void *)0x1002; return 0;
}
static void AnnibaleBTGetAddress(uint8_t out[6]) { memcpy(out, mock_bd_addr, 6); }

static uint32_t bt_ring_used(void) { return bt_ring_in - bt_ring_out; }
static void bt_ring_put_byte(uint8_t b) {
    if (bt_ring_used() < BT_RING) bt_ring[bt_ring_in++ % BT_RING] = b;
    else bt_rx_lost++;
}

@IOS_FUNCS@
@NVM_EDITS@

int main(void) {
    /* 1. Old Skywalk channel disconnect must not clobber new live session. */
    void *ch_ctx = NULL;
    kern_channel_t ch1 = (kern_channel_t)0x1111, ch2 = (kern_channel_t)0x2222;
    assert(btsk_pre_connect(NULL, NULL, NULL, 0, ch1, &ch_ctx) == 0);
    assert(btsk_connected(NULL, NULL, ch1) == 0 && btsk_up == 1 && btsk_rx_pp != NULL && btsk_active_ch == ch1);
    assert(btsk_pre_connect(NULL, NULL, NULL, 0, ch2, &ch_ctx) == 0);
    assert(btsk_connected(NULL, NULL, ch2) == 0 && btsk_active_ch == ch2);
    btsk_pre_disconnect(NULL, NULL, ch1);
    btsk_disconnected(NULL, NULL, ch1);
    assert(btsk_up == 1 && btsk_rx_pp != NULL && btsk_active_ch == ch2);
    btsk_pre_disconnect(NULL, NULL, ch2);
    assert(btsk_up == 0);
    btsk_disconnected(NULL, NULL, ch2);
    assert(btsk_up == 0 && btsk_rx_pp == NULL && btsk_active_ch == NULL);

    /* 2. H4 ACL full 16-bit length (4096 bytes = 0x1000) preserves embedded IBS bytes. */
    bt_ring_in = bt_ring_out = bt_rx_lost = 0;
    mock_uart_rx[0] = 0x02; mock_uart_rx[1] = 0x40; mock_uart_rx[2] = 0x20;
    mock_uart_rx[3] = 0x00; mock_uart_rx[4] = 0x10; /* 4096 bytes */
    memset(mock_uart_rx + 5, 0xfd, 4096); /* IBS_WAKE_IND inside ACL payload */
    mock_uart_rx_len = 5u + 4096u;
    mock_uart_writes = 0;
    bt_ring_fill();
    assert(bt_ring_used() == 4101u && bt_rx_lost == 0 && mock_uart_writes == 0);

    /* 3. bt_answer_vendor framing guard: 512-byte chunk not swallowed; exact command answered. */
    uint8_t chunk[512];
    memset(chunk, 0, sizeof(chunk));
    chunk[0] = 0x01; chunk[1] = 0x01; chunk[2] = 0xfc; chunk[3] = 0x06;
    assert(!bt_answer_vendor(chunk, sizeof(chunk)));
    assert(bt_answer_vendor(chunk, 10u));

    /* 4. 0xFC48 failure returns true (prevents double-send to UART) and queues error event. */
    bt_ring_in = bt_ring_out = 0;
    uint8_t baud_cmd[5] = { 0x01, 0x48, 0xfc, 0x01, 0x0e };
    mock_uart_write_ok = false; mock_uart_writes = 0;
    assert(bt_answer_vendor(baud_cmd, sizeof(baud_cmd)));
    assert(mock_uart_writes == 1u && bt_ring_used() == 7u && bt_ring[6] == 0x12);
    mock_uart_write_ok = true;

    /* 5. nvm_edits patches tag 2 with AnnibaleBTGetAddress() in little-endian wire order. */
    uint8_t nvm_blob[4 + 12 + 2 + 12 + 6] = {
        0x02, 32, 0, 0,
        17, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x0e,
        2, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6,
    };
    struct edit edits[12];
    unsigned edit_cnt = 0;
    assert(nvm_edits(nvm_blob, sizeof(nvm_blob), edits, &edit_cnt) && edit_cnt == 8u);
    for (unsigned b = 0; b < 6; b++) {
        assert(edits[2 + b].value == mock_bd_addr[5 - b]);
    }

    puts("PASS: BT Skywalk session isolation, 16-bit ACL, vendor framing, 0xFC48 & BD_ADDR (ASan+UBSan)");
    return 0;
}
"""


def cut_function(text: str, name: str) -> str:
    m = re.search(r"^static [^\n]+\n" + name + r"\(.*?^}\n", text, re.S | re.M)
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
    ios_text = IOS_C.read_text()
    ios_funcs = "\n".join(cut_function(ios_text, fn) for fn in (
        "bt_ring_fill", "bt_uart_send_locked", "btsk_pre_connect", "btsk_connected",
        "btsk_pre_disconnect", "btsk_disconnected", "bt_answer_vendor"))
    ios_source = IOS_HARNESS.replace("@IOS_FUNCS@", ios_funcs).replace("@NVM_EDITS@", cut_function(text, "nvm_edits"))
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "bt_hci_event.c"
        exe = Path(td) / "bt_hci_event"
        c.write_text(source)
        subprocess.run(["clang", "-std=c11", "-O1", "-Wall", "-Wno-unused-function", str(c), "-o", str(exe)],
                       check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode != 0:
            return r.returncode
        ic = Path(td) / "bt_ios_session.c"
        iexe = Path(td) / "bt_ios_session"
        ic.write_text(ios_source)
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-parameter", "-fsanitize=address,undefined", str(ic), "-o", str(iexe)],
                       check=True)
        r2 = subprocess.run([str(iexe)], capture_output=True, text=True)
        sys.stdout.write(r2.stdout)
        sys.stderr.write(r2.stderr)
        return r2.returncode


if __name__ == "__main__":
    sys.exit(main())

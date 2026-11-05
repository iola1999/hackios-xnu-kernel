/*
 * boot-arg bt=N: the WCN7850's Bluetooth half brought up by the kernel alone, over SE14 (annibale_bt_uart.c). It
 * starts from mhi_pcie.c once mhi_pcie_power.c has the chip's supplies on and BT_EN high (pcie=N pcie_power=1
 * pcie_bt_en=1), in a thread of its own, so the Wi-Fi bring-up goes on beside it. Each step is on top of the one
 * before:
 *
 *   bt=1  SE14 checked (UART protocol, FIFO interface), clocked and set up for 115200 baud
 *   bt=2  the ROM answers: Qualcomm's version request (product, ROM and SoC versions); oversampling 16, then 32
 *   bt=3  Linux btqca.c's qca_uart_setup for WCN7850: rampatch hmtbtfw<rom>.tlv, board id, NVM hmtnv<rom>.b<board>
 *         (hmtnv<rom>.bin without a match) from fw/ of the RAM disk (package.py BT_FW=1), SoC logging off, HCI Reset,
 *         the firmware's build string, Read_BD_ADDR, Read_Local_Version, a 10 s LE scan
 *   bt=4  instead of the kernel's LE scan, hand the initialized chip to iOS's bluetoothd (annibale_bt_ios.c)
 *
 * Every line starts with "bt: "; a step that fails says what it read, then "bt: stop: ...", and the thread ends.
 * Nothing here touches BT_EN, the supplies or the Wi-Fi side. The link stays at 115200: the NVM's HCI transport tag
 * (17) asks for 3.2 Mbaud and in-band sleep, its tag 27 for deep sleep, and both are changed on the way to the chip,
 * as dada does, since there is no baud switch and no IBS handling here.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

#include "annibale_bt_private.h"

extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern kern_return_t thread_terminate(thread_t thread);

#define H4_COMMAND              0x01
#define H4_EVENT                0x04
#define EV_COMMAND_COMPLETE     0x0e
#define EV_COMMAND_STATUS       0x0f
#define EV_LE_META              0x3e
#define LE_ADVERTISING_REPORT   0x02

#define OP_RESET                0x0c03
#define OP_SET_EVENT_MASK       0x0c01
#define OP_READ_LOCAL_VERSION   0x1001
#define OP_READ_BD_ADDR         0x1009
#define OP_LE_SCAN_PARAMETERS   0x200b
#define OP_LE_SCAN_ENABLE       0x200c
#define OP_QCA_LOGGING          0xfc17
#define OP_QCA_SET_BAUDRATE     0xfc48
#define QCA_BAUDRATE_115200     0x00
#define QCA_BAUDRATE_3000000    0x0e

/* Qualcomm's EDL commands, all behind opcode 0xfc00; WCN3991 and later answer with Command Complete, whose return
 * parameters (ev[6] on) are cresp (0 = done) and the request type again */
#define OP_EDL                  0xfc00
#define EDL_PATCH_TLV           0x1e
#define EDL_VERSION             0x19
#define EDL_BUILD_INFO          0x20
#define EDL_BOARD_ID            0x23
#define EDL_SEGMENT             243

#define NVM_TAG_BD_ADDR         2
#define NVM_TAG_HCI             17
#define NVM_TAG_DEEP_SLEEP      27

#define SCAN_SECONDS            10
#define SCAN_KEPT               32

static struct {
	uint32_t product, soc;
	uint16_t patch, rom, board;
	uint8_t rom_ver;
} chip;

static void
ms(uint32_t n)
{
	delay_for_interval(n, NSEC_PER_MSEC);
}

/* The next HCI event (04, code, length, parameters) into ev, its length; 0 when none came within wait_ms. Bytes
 * before an event's 04 are not H4 (in-band sleep bytes FC/FD/FE, line noise): the first few runs are shown, all
 * dropped. */
static uint32_t
hci_event(uint8_t *ev, uint32_t max, uint32_t wait_ms)
{
	uint64_t end, span;
	uint32_t shown = 0;

	nanoseconds_to_absolutetime((uint64_t)wait_ms * NSEC_PER_MSEC, &span);
	end = mach_absolute_time() + span;
	for (;;) {
		uint32_t have, skip = 0;
		const uint8_t *rx = annibale_bt_uart_rx(&have);

		while (skip < have && rx[skip] != H4_EVENT) {
			if (rx[skip] == 0xfd) {
				uint8_t ack = 0xfc;

				(void)annibale_bt_uart_write(&ack, 1);
			}
			skip++;
		}
		if (skip) {
			if (shown++ < 4) {
				annibale_bt_hex("not an event, dropped", rx, skip);
			}
			annibale_bt_uart_take(skip);
			rx = annibale_bt_uart_rx(&have);
		}
		if (have >= 3 && have >= 3u + rx[2]) {
			uint32_t n = 3u + rx[2], kept = n < max ? n : max;

			memcpy(ev, rx, kept);
			annibale_bt_uart_take(n);
			return kept;
		}
		if (mach_absolute_time() > end) {
			return 0;
		}
		if (annibale_bt_uart_poll() == 0) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
}

static bool
hci_send(uint16_t op, const uint8_t *param, uint32_t n)
{
	uint8_t packet[4 + 255];

	packet[0] = H4_COMMAND;
	packet[1] = (uint8_t)op;
	packet[2] = (uint8_t)(op >> 8);
	packet[3] = (uint8_t)n;
	if (n) {
		memcpy(packet + 4, param, n);
	}
	if (!annibale_bt_uart_write(packet, 4 + n)) {
		bt_say("bt: command %#06x was not sent\n", op);
		return false;
	}
	return true;
}

/* An HCI command and the events until its Command Complete or Command Status, which ends in ev: its length, 0 when
 * none came in wait_ms. Other events on the way are shown. */
static uint32_t
hci_cmd(uint16_t op, const uint8_t *param, uint32_t n, uint8_t *ev, uint32_t max, uint32_t wait_ms)
{
	if (!hci_send(op, param, n)) {
		return 0;
	}
	for (int others = 0; others < 16; others++) {
		uint32_t got = hci_event(ev, max, wait_ms);

		if (got == 0) {
			bt_say("bt: command %#06x: no answer in %u ms\n", op, wait_ms);
			return 0;
		}
		if ((ev[1] == EV_COMMAND_COMPLETE && got >= 6 && (ev[4] | ev[5] << 8) == op) ||
		    (ev[1] == EV_COMMAND_STATUS && got >= 7 && (ev[5] | ev[6] << 8) == op)) {
			return got;
		}
		annibale_bt_hex("an event on the way", ev, got);
	}
	return 0;
}

/* status 0 in a Command Complete of at least `least` bytes */
static bool
hci_ok(const uint8_t *ev, uint32_t got, uint32_t least)
{
	return got >= least && ev[1] == EV_COMMAND_COMPLETE && ev[6] == 0;
}

/* EDL_PATCH_VER_REQ: the file names come from the ROM version (btqca.c: ((soc_ver & 0xf00) >> 4) | (soc_ver & 0xf)) */
static bool
rom_version(const char *when)
{
	uint8_t cmd = EDL_VERSION, ev[64];
	uint32_t n = hci_cmd(OP_EDL, &cmd, 1, ev, sizeof(ev), 500), soc_ver;
	const uint8_t *v = ev + 9;

	if (n < 9 + 12 || ev[1] != EV_COMMAND_COMPLETE || ev[6] != 0 || ev[7] != EDL_VERSION) {
		if (n) {
			annibale_bt_hex("version: unexpected answer", ev, n);
		}
		return false;
	}
	chip.product = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
	chip.patch = (uint16_t)(v[4] | v[5] << 8);
	chip.rom = (uint16_t)(v[6] | v[7] << 8);
	chip.soc = v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24;
	soc_ver = chip.soc << 16 | chip.rom;
	chip.rom_ver = (uint8_t)(((soc_ver & 0xf00) >> 4) | (soc_ver & 0xf));
	bt_say("bt: version %s: product %#x, SoC %#x, ROM %#x, patch %#x; SoC version %#x, files of ROM %02x\n", when,
	    chip.product, chip.soc, chip.rom, chip.patch, soc_ver, chip.rom_ver);
	return chip.soc != 0 && chip.rom != 0;
}

/* EDL_GET_BID_REQ: the board id, which names the NVM file */
static void
board_id(void)
{
	uint8_t cmd = EDL_BOARD_ID, ev[64];
	uint32_t n = hci_cmd(OP_EDL, &cmd, 1, ev, sizeof(ev), 500);

	if (n >= 11 && ev[1] == EV_COMMAND_COMPLETE && ev[6] == 0 && ev[7] == EDL_BOARD_ID) {
		chip.board = (uint16_t)(ev[9] << 8 | ev[10]);
		bt_say("bt: board: id %#x\n", chip.board);
	} else {
		chip.board = 0;
		bt_say("bt: board: no id (%u bytes); the default NVM goes\n", n);
		if (n) {
			annibale_bt_hex("board: answer", ev, n);
		}
	}
}

struct edit {
	uint32_t at;
	uint8_t value;
};

/*
 * The NVM's changes, by file offset (the file itself may lie in the RAM disk, read-only): btqca.c's
 * qca_tlv_check_data layout, a type 2 TLV (inside a type 4 header when there is one) holding tags of
 * {u16 id, u16 length, u32, u32, data}. Without tag 17 the chip would take the file's baud rate: refused.
 */
static bool
nvm_edits(const uint8_t *data, uint32_t size, struct edit *edit, unsigned *count)
{
	uint32_t at = 0, tl = data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24, length, i = 0;
	bool hci = false;

	*count = 0;
	if ((tl & 0xff) == 4 && size > 8) {
		at = 4;
		tl = data[4] | data[5] << 8 | data[6] << 16 | (uint32_t)data[7] << 24;
	}
	length = tl >> 8;
	if ((tl & 0xff) != 2 || at + 4 + length > size) {
		bt_say("bt: nvm: TLV type %u, length %u of a %u-byte file: not an NVM this reads\n", tl & 0xff, length, size);
		return false;
	}
	at += 4;
	while (i + 12 <= length) {
		uint32_t id = data[at + i] | data[at + i + 1] << 8, len = data[at + i + 2] | data[at + i + 3] << 8;
		uint32_t d = at + i + 12;

		if (i + 12 + len > length) {
			break;
		}
		if (id == NVM_TAG_HCI && len >= 2 && *count + 2 <= 12) {
			bt_say("bt: nvm: tag 17 (HCI transport) %02x %02x -> %02x 00: in-band sleep off, 115200\n", data[d],
			    data[d + 1], data[d] & 0x7f);
			edit[(*count)++] = (struct edit){ d, (uint8_t)(data[d] & 0x7f) };
			edit[(*count)++] = (struct edit){ d + 1, 0 };
			hci = true;
		} else if (id == NVM_TAG_DEEP_SLEEP && len >= 2 && *count + 2 <= 12) {
			bt_say("bt: nvm: tag 27 (deep sleep) %02x %02x -> %02x 00\n", data[d], data[d + 1], data[d] & 0xfe);
			edit[(*count)++] = (struct edit){ d, (uint8_t)(data[d] & 0xfe) };
			edit[(*count)++] = (struct edit){ d + 1, 0 };
		} else if (id == NVM_TAG_BD_ADDR && len == 6 && *count + 6 <= 12) {
			static const uint8_t bd[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };

			bt_say("bt: nvm: tag 2 (address) %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x\n",
			    data[d + 5], data[d + 4], data[d + 3], data[d + 2], data[d + 1], data[d],
			    bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
			for (unsigned b = 0; b < 6; b++) {
				edit[(*count)++] = (struct edit){ d + b, bd[5 - b] };
			}
		}
		i += 12 + len;
	}
	if (!hci) {
		bt_say("bt: stop: nvm: no tag 17; sent as it is the chip would leave 115200\n");
	}
	return hci;
}

/*
 * One file to the chip in 243-byte EDL_PATCH_TLV pieces (btqca.c qca_download_firmware). The rampatch's header says
 * whether each piece is answered (download mode 0) or none is (1, 3: the chip sends at most a vendor event at the
 * end); the NVM's pieces are always answered.
 */
static bool
download(const char *name, bool nvm)
{
	uint64_t size64 = 0;
	const uint8_t *data = redwood_image_file("fw", name, &size64, NULL);
	uint32_t size = (uint32_t)size64, mode = 0, sent = 0, pieces = 0, next = 25;
	uint8_t cmd[2 + EDL_SEGMENT], ev[64];
	struct edit edit[12];
	unsigned edits = 0;

	if (data == NULL || size64 < 16 || size64 > (1u << 20)) {
		bt_say("bt: stop: %s: %s in fw/\n", name, data == NULL ? "not" : "wrong size");
		return false;
	}
	if (nvm) {
		if (!nvm_edits(data, size, edit, &edits)) {
			return false;
		}
	} else {
		if (data[0] != 1) {
			bt_say("bt: stop: %s: TLV type %u, not a patch (1)\n", name, data[0]);
			return false;
		}
		mode = data[14];
	}
	bt_say("bt: %s: sending %u bytes in %u pieces, %s\n", name, size, (size + EDL_SEGMENT - 1) / EDL_SEGMENT,
	    nvm || mode == 0 ? "each answered" : "unanswered");
	cmd[0] = EDL_PATCH_TLV;
	while (sent < size) {
		uint32_t piece = size - sent < EDL_SEGMENT ? size - sent : EDL_SEGMENT, n;
		bool answer = nvm || mode == 0;

		cmd[1] = (uint8_t)piece;
		memcpy(cmd + 2, data + sent, piece);
		for (unsigned e = 0; e < edits; e++) {
			if (edit[e].at >= sent && edit[e].at < sent + piece) {
				cmd[2 + edit[e].at - sent] = edit[e].value;
			}
		}
		if (!answer) {
			if (!hci_send(OP_EDL, cmd, 2 + piece)) {
				bt_say("bt: stop: %s: piece %u (offset %u) not sent\n", name, pieces, sent);
				return false;
			}
		} else if ((n = hci_cmd(OP_EDL, cmd, 2 + piece, ev, sizeof(ev), 2000)) < 8 || ev[1] != EV_COMMAND_COMPLETE ||
		    ev[6] != 0 || ev[7] != EDL_PATCH_TLV) {
			bt_say("bt: stop: %s: piece %u (offset %u) %s\n", name, pieces, sent, n ? "refused" : "not answered");
			if (n) {
				annibale_bt_hex("its answer", ev, n);
			}
			return false;
		}
		sent += piece;
		pieces++;
		if (sent * 100 / size >= next) {
			bt_say("bt: %s: %u%%\n", name, sent * 100 / size);
			next += 25;
		}
	}
	if (!nvm && mode != 0) {
		uint32_t got = hci_event(ev, sizeof(ev), 1000);

		if (got) {
			annibale_bt_hex("after the last piece", ev, got);
		} else {
			bt_say("bt: %s: no event after the last piece (download mode %u)\n", name, mode);
		}
	}
	bt_say("bt: %s: taken, %u pieces\n", name, pieces);
	return true;
}

/* btqca.c qca_get_nvm_name_by_board, plus the 4-digit form some files of the partition use (hmtnv20.b0202) */
static bool
nvm(char *used, size_t size)
{
	char names[3][32];
	unsigned count = 0;

	if (chip.board != 0) {
		snprintf(names[count++], sizeof(names[0]), (chip.board & 0xff00) ? "hmtnv%02x.b%x" : "hmtnv%02x.b%02x",
		    chip.rom_ver, chip.board);
		snprintf(names[count], sizeof(names[0]), "hmtnv%02x.b%04x", chip.rom_ver, chip.board);
		count += strcmp(names[count], names[0]) != 0;
	}
	snprintf(names[count++], sizeof(names[0]), "hmtnv%02x.bin", chip.rom_ver);
	for (unsigned i = 0; i < count; i++) {
		uint64_t length = 0;

		if (redwood_image_file("fw", names[i], &length, NULL) == NULL) {
			bt_say("bt: nvm: %s is not in fw/\n", names[i]);
			continue;
		}
		bt_say("bt: nvm: board %#x -> %s\n", chip.board, names[i]);
		strlcpy(used, names[i], size);
		return download(names[i], true);
	}
	bt_say("bt: stop: nvm: no file for board %#x\n", chip.board);
	return false;
}

/* the firmware's build string (EDL_GET_BUILD_INFO), set against hmtbtfw<rom>.ver when fw/ has it */
static void
build_info(void)
{
	uint8_t cmd = EDL_BUILD_INFO, ev[260];
	uint32_t n = hci_cmd(OP_EDL, &cmd, 1, ev, sizeof(ev), 500), len;
	char build[200], name[32];
	const uint8_t *ver;
	uint64_t ver_size = 0;

	if (n < 9 || ev[1] != EV_COMMAND_COMPLETE || ev[6] != 0 || ev[7] != EDL_BUILD_INFO) {
		bt_say("bt: build: no build string (%u bytes)\n", n);
		return;
	}
	len = ev[8] < n - 9 ? ev[8] : n - 9;
	len = len < sizeof(build) - 1 ? len : sizeof(build) - 1;
	memcpy(build, ev + 9, len);
	build[len] = 0;
	while (len > 0 && (build[len - 1] == '\n' || build[len - 1] == '\r' || build[len - 1] == 0)) {
		build[--len] = 0;
	}
	snprintf(name, sizeof(name), "hmtbtfw%02x.ver", chip.rom_ver);
	ver = redwood_image_file("fw", name, &ver_size, NULL);
	if (ver != NULL) {
		uint64_t vlen = ver_size;

		while (vlen > 0 && (ver[vlen - 1] == '\n' || ver[vlen - 1] == '\r')) {
			vlen--;
		}
		bt_say("bt: build: %s (%s %s)\n", build, name, vlen == len && memcmp(ver, build, len) == 0 ? "matches" : "DIFFERS");
	} else {
		bt_say("bt: build: %s (%s not in fw/)\n", build, name);
	}
}

/* LE scanning for SCAN_SECONDS: devices told apart by address; only the first three bytes are shown */
static uint32_t
le_scan(void)
{
	static const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f };     /* LE meta events too */
	static const uint8_t params[7] = { 0x01, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00 };   /* active, 30 ms of 30 ms */
	uint8_t on[2] = { 1, 0 }, ev[260], seen[SCAN_KEPT][6];
	uint32_t n, unique = 0, reports = 0;
	uint64_t end, span;

	n = hci_cmd(OP_SET_EVENT_MASK, mask, sizeof(mask), ev, sizeof(ev), 500);
	if (!hci_ok(ev, n, 7)) {
		bt_say("bt: scan: Set_Event_Mask refused (%u bytes, status %#x)\n", n, n >= 7 ? ev[6] : 0xff);
	}
	n = hci_cmd(OP_LE_SCAN_PARAMETERS, params, sizeof(params), ev, sizeof(ev), 500);
	if (!hci_ok(ev, n, 7)) {
		bt_say("bt: scan: LE_Set_Scan_Parameters refused (%u bytes, status %#x)\n", n, n >= 7 ? ev[6] : 0xff);
	}
	n = hci_cmd(OP_LE_SCAN_ENABLE, on, sizeof(on), ev, sizeof(ev), 500);
	if (!hci_ok(ev, n, 7)) {
		bt_say("bt: stop: scan: LE_Set_Scan_Enable refused (%u bytes, status %#x)\n", n, n >= 7 ? ev[6] : 0xff);
		return 0;
	}
	bt_say("bt: scan: on for %u s\n", SCAN_SECONDS);
	nanoseconds_to_absolutetime((uint64_t)SCAN_SECONDS * NSEC_PER_SEC, &span);
	end = mach_absolute_time() + span;
	while (mach_absolute_time() < end) {
		uint32_t got = hci_event(ev, sizeof(ev), 200), at = 5;

		if (got < 5 || ev[1] != EV_LE_META || ev[3] != LE_ADVERTISING_REPORT) {
			continue;
		}
		/* per report: event type, address type, address (6), data length, data, RSSI */
		for (uint32_t r = 0; r < ev[4] && at + 9 <= got; r++) {
			const uint8_t *a = ev + at + 2;
			uint32_t len = ev[at + 8];
			int8_t rssi = at + 9 + len < got ? (int8_t)ev[at + 9 + len] : 0;
			bool known = false;

			reports++;
			for (uint32_t i = 0; i < unique && !known; i++) {
				known = memcmp(seen[i], a, 6) == 0;
			}
			if (!known && unique < SCAN_KEPT) {
				memcpy(seen[unique++], a, 6);
				bt_say("bt: scan: device %u: %02x:%02x:%02x:..:..:.. (%s), %d dBm\n", unique, a[5], a[4], a[3],
				    ev[at + 1] ? "random" : "public", rssi);
			}
			at += 10 + len;
		}
	}
	on[0] = 0;
	(void)hci_cmd(OP_LE_SCAN_ENABLE, on, sizeof(on), ev, sizeof(ev), 500);
	bt_say("bt: scan: done: %u reports, %u devices%s\n", reports, unique, unique == SCAN_KEPT ? " (or more)" : "");
	return unique;
}

/* bt=2: the UART up and the ROM answering, at oversampling 16 (QUP 2.5 and later), else 32 */
static bool
rom_answers(void)
{
	static const uint32_t oversampling[2] = { 16, 32 };

	for (unsigned i = 0; i < 2; i++) {
		uint32_t have;
		const uint8_t *rx;
		uint8_t ev[64];
		uint32_t n;

		if (!annibale_bt_uart_config(oversampling[i])) {
			return false;
		}
		ms(50);
		(void)annibale_bt_uart_poll();
		rx = annibale_bt_uart_rx(&have);
		if (have) {
			annibale_bt_hex("before anything was sent", rx, have);
			annibale_bt_uart_take(have);
		}
		if (rom_version("from ROM")) {
			return true;
		}
		/* whether anything at all comes back: an HCI Reset is answered by any HCI controller */
		n = hci_cmd(OP_RESET, NULL, 0, ev, sizeof(ev), 500);
		if (n) {
			annibale_bt_hex("HCI Reset answer", ev, n);
		}
		annibale_bt_uart_status("after no version");
	}
	bt_say("bt: stop: rom: no version answer at oversampling 16 or 32\n");
	return false;
}

/*
 * Switch the HCI UART transport baud rate via Qualcomm vendor command 0xFC48.
 * Parameter is 1 byte: 0x0E for 3,000,000 baud (3 Mbaud), 0x00 for 115,200 baud.
 * The chip returns Command Complete at current baud, then switches its UART.
 * The host pauses 10 ms for line settle, then switches SE14 S6 RCG clock to 48 MHz (divider 1).
 */
bool
annibale_bt_hci_set_baudrate(uint32_t baud)
{
	uint8_t param = (baud == 3000000u) ? QCA_BAUDRATE_3000000 : QCA_BAUDRATE_115200;
	uint8_t ev[32];
	uint32_t n;

	bt_say("bt: baud transition: sending vendor command 0xFC48 (param %#x for %u baud)\n", param, baud);
	n = hci_cmd(OP_QCA_SET_BAUDRATE, &param, 1, ev, sizeof(ev), 1000);
	if (!hci_ok(ev, n, 7)) {
		bt_say("bt: stop: 0xFC48 refused or timed out (%u bytes, status %#x)\n", n, n >= 7 ? ev[6] : 0xff);
		if (n) {
			annibale_bt_hex("0xFC48 answer", ev, n);
		}
		return false;
	}
	bt_say("bt: chip accepted 0xFC48, settling before host UART switch\n");
	ms(10);
	if (!annibale_bt_uart_set_baud(baud)) {
		bt_say("bt: stop: host UART baud switch to %u failed\n", baud);
		return false;
	}
	ms(10);
	bt_say("bt: baud transition to %u completed successfully\n", baud);
	return true;
}

static void
bring_up(uint32_t steps)
{
	uint8_t ev[64], addr[6] = { 0 };
	char patch[32], nvm_used[32] = "";
	uint32_t n, manufacturer = 0, devices;
	static const uint8_t logging_off[2] = { 0x14, 0x00 };

	if (!annibale_bt_uart_open() || steps < 2) {
		bt_say("bt: end after step 1\n");
		return;
	}
	if (!rom_answers() || steps < 3) {
		bt_say("bt: end after step 2\n");
		return;
	}
	snprintf(patch, sizeof(patch), "hmtbtfw%02x.tlv", chip.rom_ver);
	if (!download(patch, false)) {
		return;
	}
	ms(10);
	board_id();
	if (!nvm(nvm_used, sizeof(nvm_used))) {
		return;
	}
	n = hci_cmd(OP_QCA_LOGGING, logging_off, sizeof(logging_off), ev, sizeof(ev), 500);
	if (!hci_ok(ev, n, 7)) {
		bt_say("bt: SoC logging not turned off (%u bytes, status %#x)\n", n, n >= 7 ? ev[6] : 0xff);
	}
	n = hci_cmd(OP_RESET, NULL, 0, ev, sizeof(ev), 3000);
	if (!hci_ok(ev, n, 7)) {
		bt_say("bt: stop: reset: HCI Reset after the firmware failed (%u bytes, status %#x)\n", n, n >= 7 ? ev[6] : 0xff);
		annibale_bt_uart_status("after reset");
		return;
	}
	bt_say("bt: reset: done\n");
	build_info();
	(void)rom_version("after patch");
	n = hci_cmd(OP_READ_BD_ADDR, NULL, 0, ev, sizeof(ev), 500);
	if (hci_ok(ev, n, 13)) {
		for (unsigned i = 0; i < 6; i++) {
			addr[i] = ev[12 - i];
		}
		bt_say("bt: bdaddr: %02x:%02x:%02x:%02x:%02x:%02x\n", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	} else {
		bt_say("bt: bdaddr: Read_BD_ADDR failed (%u bytes)\n", n);
	}
	n = hci_cmd(OP_READ_LOCAL_VERSION, NULL, 0, ev, sizeof(ev), 500);
	if (hci_ok(ev, n, 15)) {
		manufacturer = ev[11] | ev[12] << 8;
		bt_say("bt: version: HCI %u rev %#x, LMP %u subversion %#x, manufacturer %#x%s\n", ev[7], ev[8] | ev[9] << 8,
		    ev[10], ev[13] | ev[14] << 8, manufacturer, manufacturer == 0x1d ? " (Qualcomm)" : "");
	} else {
		bt_say("bt: version: Read_Local_Version failed (%u bytes)\n", n);
	}
	{
		uint32_t target_baud = 0;
		if (PE_parse_boot_argn("bt_baud", &target_baud, sizeof(target_baud)) && target_baud == 3000000u) {
			(void)annibale_bt_hci_set_baudrate(3000000u);
		}
	}
	if (steps >= 4) {
		annibale_bt_ios_serve(addr);
		bt_say("bt: done: ROM %02x board %#x, %s + %s, address %02x:%02x:%02x:%02x:%02x:%02x, manufacturer %#x, "
		    "handed to bluetoothd\n", chip.rom_ver, chip.board, patch, nvm_used, addr[0], addr[1], addr[2], addr[3],
		    addr[4], addr[5], manufacturer);
		return;
	}
	devices = le_scan();
	bt_say("bt: done: ROM %02x board %#x, %s + %s, address %02x:%02x:%02x:%02x:%02x:%02x, manufacturer %#x, "
	    "%u LE devices in %u s\n", chip.rom_ver, chip.board, patch, nvm_used, addr[0], addr[1], addr[2], addr[3], addr[4],
	    addr[5], manufacturer, devices, SCAN_SECONDS);
}

static void
bt_thread(void *parameter, __unused wait_result_t result)
{
	bring_up((uint32_t)(uintptr_t)parameter);
	thread_terminate(current_thread());
}

void
annibale_bt_start(void)
{
	uint32_t steps = 0, bt_en = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("bt", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (!PE_parse_boot_argn("pcie_bt_en", &bt_en, sizeof(bt_en)) || bt_en == 0) {
		bt_say("bt: stop: bt=%u needs pcie_bt_en=1, BT_EN stays low without it\n", steps);
		return;
	}
	if (steps > 4) {
		bt_say("bt: bt=%u: this kernel has steps 1 to 4; running 4\n", steps);
		steps = 4;
	}
	bt_say("bt: start: steps 1 to %u on SE14\n", steps);
	if (kernel_thread_start(bt_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

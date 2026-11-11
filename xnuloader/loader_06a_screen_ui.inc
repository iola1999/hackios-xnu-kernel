/* ------------------------------------------------------------------------ */
/* Status and Boot Menu UI on the panel.                                    */
/*                                                                          */
/* The firmware console font is fixed at 8x16 and the command loop turns    */
/* the console off (screen_quiet), so once the loader waits for USB nothing */
/* on the panel moves. This paints an iBoot-style interface on the panel:   */
/* a rounded-card multi-boot selector with hardware-key navigation, a       */
/* smooth rotating ring indicator while waiting on USB, a rounded transfer  */
/* progress bar with megabytes and percentage, and a monospace console card */
/* showing the tail of the loader log.                                      */
/*                                                                          */
/* The panel belongs to the kernel from "oem run" on: nothing is drawn      */
/* while a kernel is live or may still hold the hardware, after a firmware  */
/* fault, or after a started kernel came back (its last frame is what       */
/* oem screenshot is for).                                                  */
/*                                                                          */
/* Included by loader_06 ahead of the LC-USB block: usb_wait() calls        */
/* ui_tick from its poll loop, and the host tests stub that one call.       */

#include "ui_font_private.h"

#define UI_MARGIN       70
#define UI_TITLE_SCALE  3
#define UI_AMOUNT_SCALE 2
#define UI_LOG_SCALE    2
#define UI_LOG_LINES    10
#define UI_LOG_STEP     42

static struct {
	int ready;
	int usb_chrome_drawn;
	const char *stage;      /* what ui_stage last drew; compared by text */
	int pct;                /* fill already on the bar, -1 before the first draw */
	int spin;               /* ring phase, 0..15 */
	uint64_t spin_at, amount_at, log_seen;
	uint64_t done, total;
} UI;

static int boot_fallback_active;

static uint64_t
ui_now(void)
{
	return READ_SYSREG(CNTVCT_EL0);
}

static uint64_t
ui_freq(void)
{
	uint64_t f = READ_SYSREG(CNTFRQ_EL0);

	return f ? f : 1000000;   /* every board here sets CNTFRQ; the fallback only keeps the ring moving */
}

static int
ui_on(void)
{
	return UI.ready && running_live == NULL && (!kernel_resources_held || boot_fallback_active) && !firmware_fault;
}

static void
ui_spin_draw(void)
{
	int cx = (int)SCREEN.width / 2;
	int cy = 368;

	ui_draw_spinner_ring(cx, cy, 42, 8, UI.spin, UI_WHITE, UI_TRACK_BG, UI_CARD_BG);
}

static void
ui_bar(int pct)
{
	int card_w = (int)SCREEN.width - 2 * UI_MARGIN;
	int bx = UI_MARGIN + 64;
	int bw = card_w - 128;
	int by = 548;
	int bh = 22;

	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	ui_round_rect(bx, by, bw, bh, 11, 0, UI_TRACK_BG, UI_TRACK_BG, UI_CARD_BG);
	if (pct > 0) {
		int fill = bw * pct / 100;
		if (fill < 22) fill = 22;
		ui_round_rect(bx, by, fill, bh, 11, 0, UI_WHITE, UI_WHITE, UI_TRACK_BG);
	}
}

static void
ui_amount(void)
{
	char buf[64];
	int card_w = (int)SCREEN.width - 2 * UI_MARGIN;
	int y = 594;
	int n = 0, w;

	if (UI.total == 0) {
		const char *idle_msg = "Ready for Fastboot commands over USB";
		w = ui_measure_text_ex(idle_msg, 48, UI_AMOUNT_SCALE, 0);
		ui_fill(UI_MARGIN + 20, y - 4, card_w - 40, 40, UI_CARD_BG);
		ui_draw_text_ex(((int)SCREEN.width - w) / 2, y, idle_msg, UI_AMOUNT_SCALE, 48, UI_TEXT_SEC, UI_CARD_BG, 0, 0);
		return;
	}

	n += ui_dec(buf + n, UI.done >> 20);
	buf[n++] = ' ';
	buf[n++] = '/';
	buf[n++] = ' ';
	n += ui_dec(buf + n, UI.total >> 20);
	buf[n++] = ' ';
	buf[n++] = 'M';
	buf[n++] = 'B';
	buf[n++] = ' ';
	buf[n++] = ' ';
	buf[n++] = '(';
	n += ui_dec(buf + n, UI.pct >= 0 ? (uint64_t)UI.pct : 0);
	buf[n++] = '%';
	buf[n++] = ')';
	buf[n] = 0;

	w = ui_measure_text_ex(buf, n, UI_AMOUNT_SCALE, 0);
	ui_fill(UI_MARGIN + 20, y - 4, card_w - 40, 40, UI_CARD_BG);
	ui_draw_text_ex(((int)SCREEN.width - w) / 2, y, buf, UI_AMOUNT_SCALE, n, UI_TEXT_SEC, UI_CARD_BG, 0, 0);
}

static void
ui_log(void)
{
	int card_x = UI_MARGIN;
	int card_w = (int)SCREEN.width - 2 * UI_MARGIN;
	int body_y = 838;
	int max_h = boot_fallback_active ? 460 : 620;
	int max_lines = (max_h - 90) / UI_LOG_STEP;
	int maxc = 96;
	uint32_t end = log_len, p, start = 0, line;
	int seen = 0;

	if (max_lines > UI_LOG_LINES) max_lines = UI_LOG_LINES;
	ui_fill(card_x + 12, body_y, card_w - 24, max_lines * UI_LOG_STEP + 16, UI_TERM_BG);
	UI.log_seen = log_len;

	while (end > 0 && (log_buf[end - 1] == '\n' || log_buf[end - 1] == '\r')) {
		end--;
	}
	for (p = end; p > 0; ) {
		if (log_buf[--p] == '\n' && ++seen == max_lines) {
			start = p + 1;
			break;
		}
	}
	int y = body_y + 6;
	for (line = start; line < end && y + 32 <= body_y + max_lines * UI_LOG_STEP + 12; line = p + 1) {
		int n, last;

		for (p = line; p < end && log_buf[p] != '\n'; p++) {
		}
		n = (int)(p - line);
		if (n > maxc) {
			n = maxc;
		}
		for (last = n - 1; last >= 0 && log_buf[line + last] == '\r'; last--) {
			n = last;
		}
		ui_draw_text_ex(card_x + 32, y, log_buf + line, UI_LOG_SCALE, n, UI_TEXT_SEC, UI_TERM_BG, 1, 0);
		y += UI_LOG_STEP;
	}
}

static void
ui_ensure_usb_chrome(void)
{
	int card_w = (int)SCREEN.width - 2 * UI_MARGIN;
	int term_h = boot_fallback_active ? 460 : 620;

	if (UI.usb_chrome_drawn) {
		return;
	}
	UI.usb_chrome_drawn = 1;
	if (!boot_fallback_active) {
		ui_fill(0, 0, (int)SCREEN.width, (int)SCREEN.height, UI_BG);
	} else {
		ui_fill(0, 0, (int)SCREEN.width, 1220, UI_BG);
	}

	ui_draw_top_pill("USB Fastboot  -  annibale", UI_AMBER);
	/* Main status and progress card */
	ui_round_rect(UI_MARGIN, 240, card_w, 430, 36, 2, UI_CARD_BG, UI_CARD_BORDER, UI_BG);
	ui_spin_draw();
	ui_bar(UI.pct >= 0 ? UI.pct : 0);
	ui_amount();

	/* Console log card */
	ui_round_rect(UI_MARGIN, 730, card_w, term_h, 32, 2, UI_TERM_BG, UI_TERM_BORDER, UI_BG);
	ui_round_rect(UI_MARGIN + 28, 762, 16, 16, 8, 0, UI_RED, UI_RED, UI_TERM_BG);
	ui_round_rect(UI_MARGIN + 54, 762, 16, 16, 8, 0, UI_AMBER, UI_AMBER, UI_TERM_BG);
	ui_round_rect(UI_MARGIN + 80, 762, 16, 16, 8, 0, UI_GREEN, UI_GREEN, UI_TERM_BG);
	ui_draw_text_ex(UI_MARGIN + 116, 754, "LOADER CONSOLE", 2, 32, UI_TEXT_DIM, UI_TERM_BG, 0, 1);
	ui_fill(UI_MARGIN + 4, 810, card_w - 8, 2, UI_TERM_BORDER);
	ui_log();
}

static void
ui_stage(const char *word)
{
	int card_w = (int)SCREEN.width - 2 * UI_MARGIN;
	int y = 456;
	int w;
	uint32_t dot_col = UI_AMBER;

	if (!ui_on() || (UI.stage != 0 && str_eq(UI.stage, word) && UI.usb_chrome_drawn)) {
		return;
	}
	UI.stage = word;
	ui_ensure_usb_chrome();

	if (starts_with(word, "PAYLOAD READY") || starts_with(word, "RAMDISK READY") || starts_with(word, "SYSTEM READY")) {
		dot_col = UI_GREEN;
	} else if (starts_with(word, "TRANSFER FAILED") || starts_with(word, "BOOT FAILED") ||
	           starts_with(word, "BOOT FALLBACK") || starts_with(word, "NO KERNEL")) {
		dot_col = UI_RED;
	} else if (usb_connected) {
		dot_col = UI_GREEN;
	}
	ui_draw_top_pill(usb_connected ? "USB Fastboot Connected" : "USB Fastboot  -  annibale", dot_col);

	w = ui_measure_text_ex(word, 64, UI_TITLE_SCALE, 0);
	ui_fill(UI_MARGIN + 16, y - 6, card_w - 32, 64, UI_CARD_BG);
	ui_draw_text_ex(((int)SCREEN.width - w) / 2, y, word, UI_TITLE_SCALE, 64, UI_WHITE, UI_CARD_BG, 0, 1);
}

static void
ui_progress(uint64_t done, uint64_t total)
{
	int pct, moved;

	if (!ui_on() || total == 0) {
		return;
	}
	ui_ensure_usb_chrome();
	if (done > total) {
		done = total;
	}
	UI.done = done;
	UI.total = total;
	pct = (int)(done * 100 / total);
	moved = pct != UI.pct || done == total;
	if (moved) {
		UI.pct = pct;
		ui_bar(pct);
	}
	/* the megabytes move between whole percents too, five times a second at most */
	if (moved || ui_now() - UI.amount_at >= ui_freq() / 5) {
		UI.amount_at = ui_now();
		ui_amount();
	}
}

static void
ui_flush_splash_fb(void)
{
	if (skip_ufs_arbitration && SCREEN.base == 0xfc800000ULL) {
		uint64_t bytes = (uint64_t)SCREEN.height * SCREEN.stride;
		clean_dcache(SCREEN.base, SCREEN.base + bytes);
		memcpy((void *)(uintptr_t)0xfd400000ULL, (const void *)(uintptr_t)SCREEN.base, bytes);
		clean_dcache(0xfd400000ULL, 0xfd400000ULL + bytes);
	}
}

static void
ui_init(void)
{
	if (SCREEN.base == 0) return;
	const char *prev_stage = UI.stage;
	memset(&UI, 0, sizeof(UI));
	memset(&UI_BOOT, 0, sizeof(UI_BOOT));
	UI.pct = -1;
	UI.ready = 1;
	UI.spin_at = ui_now();
	if (!ui_on()) return;
	ui_ensure_usb_chrome();
	if (boot_fallback_active) {
		ui_stage(prev_stage ? prev_stage : "BOOT FALLBACK - FASTBOOT MODE");
		ui_flush_splash_fb();
		return;
	}
	ui_stage("WAITING FOR HOST");
}

/* Called from usb_wait's poll loop: two counter reads when nothing is due. */
static void
ui_tick(void)
{
	if (!ui_on()) {
		return;
	}
	if (!UI.usb_chrome_drawn) {
		ui_ensure_usb_chrome();
	}
	if (log_len != UI.log_seen) {
		ui_log();
	}
	if (ui_now() - UI.spin_at >= ui_freq() / 16) {
		UI.spin_at = ui_now();
		UI.spin = (UI.spin + 1) & 15;
		ui_spin_draw();
	}
}

/* ------------------------------------------------------------------------ */
/* Standalone Boot Menu & Slot Management                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
	uint16_t scan_code;
	char16 unicode_char;
} efi_input_key;

typedef struct efi_simple_text_input {
	efi_status (EFIAPI *reset)(struct efi_simple_text_input *self, uint8_t extended_verification);
	efi_status (EFIAPI *read_key_stroke)(struct efi_simple_text_input *self, efi_input_key *key);
	void *wait_for_key;
} efi_simple_text_input;

#define MENU_CHOICE_IOS       0
#define MENU_CHOICE_ANDROID   1
#define MENU_CHOICE_FASTBOOT  2

#include "slot_choice.h"

/* 'a' or 'b' once slot_detect has found the slots, 0 while unknown (slot_choice.h) */
static char loader_slot;
static char android_slot;
static char menu_item_ios[40] = "iOS 26";
static char menu_item_android[40] = "Android";
static const char *const menu_item_fastboot = "Fastboot Diagnostics";
static char menu_badge_ios[16] = "RAM";
static char menu_badge_android[16] = "ACTIVE";

static const char *
menu_item_str(int i)
{
	if (i == 0) return menu_item_ios;
	if (i == 1) return menu_item_android;
	return menu_item_fastboot;
}

static const char *
menu_item_sub(int i)
{
	if (i == 0) return "Darwin XNU Kernel  -  APFS System";
	if (i == 1) return "Qualcomm ABL  -  UFS LUN 4";
	return "USB Command & Log Channel";
}

static const char *
menu_item_badge(int i)
{
	if (i == 0) return menu_badge_ios;
	if (i == 1) return menu_badge_android;
	return "USB";
}

static const uint64_t *
menu_item_icon(int i)
{
	if (i == 0) return ui_icon_apple;
	if (i == 1) return ui_icon_android;
	return ui_icon_usb;
}

static void
menu_compose(char *dst, const char *label, char slot)
{
	static const char tail[] = " (SLOT ";
	int n = 0;
	while (label[n] != 0 && n < 30) {
		dst[n] = label[n];
		n++;
	}
	if (slot == 'a' || slot == 'b') {
		for (int i = 0; tail[i] != 0; i++) {
			dst[n++] = tail[i];
		}
		dst[n++] = (char)(slot - 'a' + 'A');
		dst[n++] = ')';
	}
	dst[n] = 0;
}

static void
menu_compose_badge(char *dst, char slot, const char *fallback)
{
	if (slot == 'a' || slot == 'b') {
		dst[0] = 'S'; dst[1] = 'L'; dst[2] = 'O'; dst[3] = 'T'; dst[4] = ' ';
		dst[5] = (char)(slot - 'a' + 'A');
		dst[6] = 0;
	} else {
		int i = 0;
		while (fallback[i] && i < 15) {
			dst[i] = fallback[i];
			i++;
		}
		dst[i] = 0;
	}
}

static void
slot_update_menu_text(void)
{
	menu_compose(menu_item_ios, "iOS 26", loader_slot);
	menu_compose(menu_item_android, "Android", android_slot);
	menu_compose_badge(menu_badge_ios, loader_slot, "RAM");
	menu_compose_badge(menu_badge_android, android_slot, "ACTIVE");
}

static efi_block_io *
lun_get_bio(uint16_t target_lun, const char *probe_part)
{
	uint16_t target_unit = target_lun;
	for (uint32_t i = 0; i < block_shared.parts; i++) {
		if (!(block_shared.part[i].flags & BLOCK_PART_UNIT) &&
		    str_eq(block_shared.part[i].name, probe_part) &&
		    block_shared.part[i].unit != 0xffff) {
			target_unit = block_shared.part[i].unit;
			break;
		}
	}
	for (uint32_t i = 0; i < block_shared.parts; i++) {
		if ((block_shared.part[i].flags & BLOCK_PART_UNIT) &&
		    (block_shared.part[i].lun == target_lun || block_shared.part[i].unit == target_unit)) {
			return block_bios[i];
		}
	}
	return NULL;
}

static efi_block_io *
lun4_get_bio(void)
{
	return lun_get_bio(4, "boot_a");
}

static int
gpt_match_name(const uint8_t *entry, const char *ascii_name)
{
	const uint8_t *pname = entry + 56;
	int len = 0;
	while (ascii_name[len]) {
		if (pname[len * 2] != (uint8_t)ascii_name[len] || pname[len * 2 + 1] != 0) {
			return 0;
		}
		len++;
	}
	if (pname[len * 2] != 0 || pname[len * 2 + 1] != 0) {
		return 0;
	}
	return 1;
}

static uint8_t *
gpt_find_entry(uint8_t *entries, uint32_t num_entries, uint32_t entry_size, const char *name)
{
	for (uint32_t i = 0; i < num_entries; i++) {
		uint8_t *entry = entries + i * entry_size;
		if (gpt_match_name(entry, name)) {
			return entry;
		}
	}
	return NULL;
}

static efi_status
safe_read_blocks(efi_block_io *bio, uint64_t lba, uint64_t num_blocks, uint32_t bs, void *buf)
{
	efi_status s = bio->read_blocks(bio, bio->media->media_id, lba, num_blocks * bs, buf);
	if (EFI_ERROR(s)) {
		uint8_t *p = (uint8_t *)buf;
		for (uint64_t b = 0; b < num_blocks; b++) {
			s = bio->read_blocks(bio, bio->media->media_id, lba + b, bs, p + b * bs);
			if (EFI_ERROR(s)) break;
		}
	}
	return s;
}

static efi_status
safe_write_blocks(efi_block_io *bio, uint64_t lba, uint64_t num_blocks, uint32_t bs, const void *buf)
{
	efi_status s = bio->write_blocks(bio, bio->media->media_id, lba, num_blocks * bs, (void *)buf);
	if (EFI_ERROR(s)) {
		const uint8_t *p = (const uint8_t *)buf;
		for (uint64_t b = 0; b < num_blocks; b++) {
			s = bio->write_blocks(bio, bio->media->media_id, lba + b, bs, (void *)(p + b * bs));
			if (EFI_ERROR(s)) break;
		}
	}
	return s;
}

static int
slot_holds_loader(char slot)
{
	static uint8_t head[SLOT_HEAD_BYTES] __attribute__((aligned(4096)));
	char16 name[] = u"boot_a";
	name[5] = (char16)slot;
	efi_block_io *bio = find_partition_named(name, sizeof(name) / sizeof(name[0]));
	if (bio == NULL || bio->media == NULL) {
		return 0;
	}
	uint32_t bs = bio->media->block_size;
	if (bs < 512 || bs > sizeof(head) || sizeof(head) % bs != 0 ||
	    EFI_ERROR(safe_read_blocks(bio, 0, sizeof(head) / bs, bs, head))) {
		return 0;
	}
	return boot_image_is_loader(head, sizeof(head));
}

static void
slot_detect(void)
{
	int in_a = slot_holds_loader('a');
	int in_b = slot_holds_loader('b');
	char loader_name[2] = { 0, 0 }, android_name[2] = { 0, 0 };

	slot_choose(in_a, in_b, &loader_slot, &android_slot);
	loader_name[0] = loader_slot ? loader_slot : '-';
	android_name[0] = android_slot ? android_slot : '-';
	print("slot: loader image in boot_a=%d boot_b=%d: loader_slot=%s android_slot=%s\n",
	    (uint64_t)in_a, (uint64_t)in_b, loader_name, android_name);
	slot_update_menu_text();
}

static void
ui_draw_menu_card(int i, int selected)
{
	int card_x = UI_MARGIN;
	int card_w = (int)SCREEN.width - 2 * UI_MARGIN;
	int card_h = 204;
	int card_y = 620 + i * 236;

	uint32_t bg = selected ? UI_CARD_SEL_BG : UI_CARD_BG;
	uint32_t brd = selected ? UI_CARD_SEL_BORDER : UI_CARD_BORDER;
	uint32_t icon_box_bg = selected ? UI_ICON_SEL_BG : UI_ICON_BG;
	uint32_t icon_fg = selected ? UI_TEXT_SEL_PRI : UI_WHITE;
	uint32_t title_col = selected ? UI_TEXT_SEL_PRI : UI_TEXT_PRI;
	uint32_t sub_col = selected ? UI_TEXT_SEL_SEC : UI_TEXT_SEC;
	uint32_t badge_bg = selected ? UI_BADGE_SEL_BG : UI_BADGE_BG;
	uint32_t badge_fg = selected ? UI_TEXT_SEL_PRI : UI_TEXT_SEC;

	ui_round_rect(card_x, card_y, card_w, card_h, 36, selected ? 0 : 2, bg, brd, UI_BG);

	/* Left icon badge */
	int ib_x = card_x + 36;
	int ib_y = card_y + (card_h - 96) / 2;
	ui_round_rect(ib_x, ib_y, 96, 96, 24, 0, icon_box_bg, icon_box_bg, bg);
	ui_draw_icon24(ib_x + 24, ib_y + 24, menu_item_icon(i), 2, icon_fg, icon_box_bg);

	/* Title and subtitle */
	const char *title = (i == 0) ? "iOS 26" : (i == 1) ? "Android" : menu_item_str(i);
	const char *sub = menu_item_sub(i);
	ui_draw_text_ex(ib_x + 128, card_y + 52, title, 3, 28, title_col, bg, 0, 1);
	ui_draw_text_ex(ib_x + 128, card_y + 116, sub, 2, 40, sub_col, bg, 0, 0);

	/* Right slot pill badge */
	const char *badge = menu_item_badge(i);
	int bw = ui_measure_text_ex(badge, 12, 2, 0) + 48;
	int bh = 56;
	int bx = card_x + card_w - bw - 36;
	int by = card_y + (card_h - bh) / 2;
	ui_round_rect(bx, by, bw, bh, 18, 0, badge_bg, badge_bg, bg);
	ui_draw_text_ex(bx + 24, by + 12, badge, 2, 12, badge_fg, badge_bg, 0, 1);
}

static void
ui_draw_menu_footer(int countdown, int pct_remaining, int user_interacted, int redraw_text)
{
	int card_x = UI_MARGIN;
	int card_w = (int)SCREEN.width - 2 * UI_MARGIN;
	int bar_y = 1420;
	int text_y = 1464;

	if (!user_interacted && countdown > 0) {
		if (pct_remaining < 0) pct_remaining = 0;
		if (pct_remaining > 100) pct_remaining = 100;
		ui_round_rect(card_x, bar_y, card_w, 14, 7, 0, UI_TRACK_BG, UI_TRACK_BG, UI_BG);
		int fill = card_w * pct_remaining / 100;
		if (fill > 0) {
			if (fill < 14) fill = 14;
			ui_round_rect(card_x, bar_y, fill, 14, 7, 0, UI_WHITE, UI_WHITE, UI_TRACK_BG);
		}
		if (redraw_text) {
			char cmsg[64] = "Auto-boot in 5s   -   Vol: Select   Pwr: Confirm";
			cmsg[13] = (char)('0' + countdown);
			int cw = ui_measure_text_ex(cmsg, 56, 2, 0);
			ui_fill(0, text_y - 4, (int)SCREEN.width, 44, UI_BG);
			ui_draw_text_ex(((int)SCREEN.width - cw) / 2, text_y, cmsg, 2, 56, UI_TEXT_SEC, UI_BG, 0, 0);
		}
	} else if (redraw_text) {
		ui_fill(card_x, bar_y - 2, card_w, 20, UI_BG);
		const char *hint = "Volume Keys: Select Target   -   Power Key: Confirm";
		int hw = ui_measure_text_ex(hint, 56, 2, 0);
		ui_fill(0, text_y - 4, (int)SCREEN.width, 44, UI_BG);
		ui_draw_text_ex(((int)SCREEN.width - hw) / 2, text_y, hint, 2, 56, UI_TEXT_SEC, UI_BG, 0, 0);
	}
}

static int usb_menu_interrupted;
static void usb_poll_command(void);

static void
ui_draw_menu(int choice, int countdown, int user_interacted)
{
	/* Top Apple/HackiOS emblem and header */
	int emb_sz = 104;
	int emb_x = ((int)SCREEN.width - emb_sz) / 2;
	int emb_y = 240;
	ui_round_rect(emb_x, emb_y, emb_sz, emb_sz, 52, 2, UI_CARD_BG, UI_CARD_BORDER, UI_BG);
	ui_draw_icon24(emb_x + 28, emb_y + 26, ui_icon_apple, 2, UI_WHITE, UI_CARD_BG);

	const char *title = "HackiOS Startup";
	int tw = ui_measure_text_ex(title, 32, 3, 0);
	ui_draw_text_ex(((int)SCREEN.width - tw) / 2, 376, title, 3, 32, UI_WHITE, UI_BG, 0, 1);

	const char *sub = "REDMI K90  -  Select Boot System";
	int sw = ui_measure_text_ex(sub, 40, 2, 0);
	ui_draw_text_ex(((int)SCREEN.width - sw) / 2, 442, sub, 2, 40, UI_TEXT_SEC, UI_BG, 0, 0);
	usb_poll_command();

	for (int i = 0; i < 3; i++) {
		ui_draw_menu_card(i, i == choice);
		usb_poll_command();
	}
	ui_draw_menu_footer(countdown, countdown * 20, user_interacted, 1);
}

#define SPMI_ARB_CORE   ((uintptr_t)0x0c400000ULL)
#define SPMI_ARB_OBSRV  ((uintptr_t)0x0c440000ULL)

struct spmi_btn { int usable, live, last, down; };
struct spmi_btn_ctx { int pon_ch, pin_ch; struct spmi_btn pwr, vol_up, vol_down; };

static int
spmi_obsrv_read(int ch, uint8_t reg, uint8_t *buf, uint32_t count)
{
	if (ch < 0 || count == 0 || count > 8) return 0;
	uintptr_t base = SPMI_ARB_OBSRV + (uintptr_t)0x20ULL * (uintptr_t)ch;
	uint32_t opc = (count > 4) ? 1u : 13u, status = 0;
	*(volatile uint32_t *)(base + 0x00) = (opc << 27) | (((uint32_t)reg & 0xffu) << 4) | (count - 1);
	for (int i = 0; i < 4000; i++) {
		status = *(volatile uint32_t *)(base + 0x08);
		if (status & 0x1u) break;
	}
	if (!(status & 0x1u) || (status & 0xeu)) return 0;
	uint32_t d[2] = { *(volatile uint32_t *)(base + 0x18), (count > 4) ? *(volatile uint32_t *)(base + 0x1c) : 0 };
	for (uint32_t i = 0; i < count; i++) buf[i] = (uint8_t)(d[i / 4] >> (8 * (i % 4)));
	return 1;
}

#include "loader_spmi_reboot_private.h"

static void
spmi_buttons_init(struct spmi_btn_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->pon_ch = -1;
	ctx->pin_ch = -1;
	if (SCREEN.base != platform_annibale.fb_base) return;

	uint32_t ver = *(volatile uint32_t *)(SPMI_ARB_CORE + 0x000);
	if (ver < 0x70000000u) return;

	uint32_t channels = *(volatile uint32_t *)(SPMI_ARB_CORE + 0x004) & 0x7ffu;
	if (channels == 0 || channels > 1024) channels = 1024;

	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = *(volatile uint32_t *)(SPMI_ARB_CORE + (uintptr_t)0x2000ULL + (uintptr_t)(4 * n));
		uint16_t ppid = (uint16_t)((map >> 8) & 0xfffu);
		if (ppid == 0x013u && ctx->pon_ch < 0) ctx->pon_ch = (int)n;
		if (ppid == 0x18du && ctx->pin_ch < 0) ctx->pin_ch = (int)n;
	}

	uint8_t id[2], rt = 0, cfg[8];
	if (ctx->pon_ch >= 0 && spmi_obsrv_read(ctx->pon_ch, 0x04, id, 2) &&
	    id[0] == 0x01 && id[1] == 0x09 && spmi_obsrv_read(ctx->pon_ch, 0x10, &rt, 1)) {
		ctx->pwr.usable = 1;
		ctx->pwr.last = (rt & 0x80u) != 0;
		ctx->pwr.live = !ctx->pwr.last;
		ctx->vol_down.usable = 1;
		ctx->vol_down.last = (rt & 0x40u) != 0;
		ctx->vol_down.live = !ctx->vol_down.last;
	}
	if (ctx->pin_ch >= 0 && spmi_obsrv_read(ctx->pin_ch, 0x04, id, 2) &&
	    id[0] == 0x10 && id[1] >= 0x10 && id[1] <= 0x13 &&
	    spmi_obsrv_read(ctx->pin_ch, 0x40, cfg, 8) &&
	    (cfg[0] & 3u) == 0 && (cfg[2] & 7u) < 4 && (cfg[6] & 0x80u) != 0 &&
	    spmi_obsrv_read(ctx->pin_ch, 0x10, &rt, 1)) {
		ctx->vol_up.usable = 1;
		ctx->vol_up.last = (rt & 0x01u) == 0;
		ctx->vol_up.live = !ctx->vol_up.last;
	}
	print("menu: spmi buttons pon_ch=%d(usable=%d) pin_ch=%d(usable=%d)\n",
	    (uint64_t)ctx->pon_ch, (uint64_t)ctx->vol_down.usable,
	    (uint64_t)ctx->pin_ch, (uint64_t)ctx->vol_up.usable);
}

static int
spmi_btn_edge(struct spmi_btn *b, int pressed)
{
	int steady = (pressed == b->last);
	b->last = pressed;
	if (!b->usable || !steady) return 0;
	if (!b->live) {
		if (!pressed) b->live = 1;
		return 0;
	}
	if (pressed == b->down) return 0;
	b->down = pressed;
	return pressed ? 1 : 0;
}

static void
ui_menu_select(int *choice, int next_choice, int countdown, int *user_interacted)
{
	int prev = *choice;
	int first_interact = (*user_interacted == 0);
	*user_interacted = 1;
	*choice = next_choice;
	if (prev != next_choice) {
		ui_draw_menu_card(prev, 0);
		ui_draw_menu_card(next_choice, 1);
	}
	if (first_interact) {
		ui_draw_menu_footer(countdown, 0, 1, 1);
	}
}

static int
ui_boot_menu(void)
{
	efi_simple_text_input *cin = (efi_simple_text_input *)ST->con_in;
	int choice = MENU_CHOICE_IOS, countdown = 5, last_pct = 100, user_interacted = 0;
	uint64_t start_time, last_spmi_poll, freq, poll_ticks;
	struct spmi_btn_ctx spmi_btns;

	if (SCREEN.base == 0) return MENU_CHOICE_IOS;
	if (loader_slot == 0 && android_slot == 0) slot_detect();
	spmi_buttons_init(&spmi_btns);

	UI.ready = 1;
	UI.pct = -1;
	UI.spin_at = ui_now();
	UI.usb_chrome_drawn = 0;
	ui_fill(0, 0, (int)SCREEN.width, (int)SCREEN.height, UI_BG);
	ui_draw_menu(choice, countdown, user_interacted);
	usb_menu_interrupted = 0;

	freq = ui_freq();
	poll_ticks = freq / 100;
	if (poll_ticks == 0) poll_ticks = 1;
	start_time = ui_now();
	while (!usb_connected && !usb_menu_interrupted && ui_now() - start_time < (freq * 3) / 2) {
		usb_poll_command();
	}
	start_time = ui_now();
	last_spmi_poll = start_time;

	while (1) {
		for (int u = 0; u < 32 && !usb_menu_interrupted; u++) {
			usb_poll_command();
		}
		if (usb_menu_interrupted) {
			choice = MENU_CHOICE_FASTBOOT;
			break;
		}
		if (cin != NULL) {
			efi_input_key key;
			efi_status status = cin->read_key_stroke(cin, &key);
			if (!EFI_ERROR(status)) {
				if (key.scan_code == 0x0001 || key.unicode_char == '+' || key.unicode_char == 'k' || key.unicode_char == '1') {
					ui_menu_select(&choice, (choice + 2) % 3, countdown, &user_interacted);
				} else if (key.scan_code == 0x0002 || key.unicode_char == '-' || key.unicode_char == 'j' || key.unicode_char == '2') {
					ui_menu_select(&choice, (choice + 1) % 3, countdown, &user_interacted);
				} else if (key.unicode_char == '3') {
					ui_menu_select(&choice, MENU_CHOICE_FASTBOOT, countdown, &user_interacted);
				} else if (key.scan_code == 0x0017 || key.scan_code == 0x0003 || key.unicode_char == '\r' || key.unicode_char == '\n' || key.unicode_char == ' ') {
					break;
				}
			}
		}

		uint64_t now = ui_now();
		if (now - last_spmi_poll >= poll_ticks) {
			last_spmi_poll = now;
			uint8_t rt = 0;
			int pwr_edge = 0, up_edge = 0, down_edge = 0;
			if ((spmi_btns.pwr.usable || spmi_btns.vol_down.usable) &&
			    spmi_obsrv_read(spmi_btns.pon_ch, 0x10, &rt, 1)) {
				pwr_edge = spmi_btn_edge(&spmi_btns.pwr, (rt & 0x80u) != 0);
				down_edge = spmi_btn_edge(&spmi_btns.vol_down, (rt & 0x40u) != 0);
			}
			if (spmi_btns.vol_up.usable &&
			    spmi_obsrv_read(spmi_btns.pin_ch, 0x10, &rt, 1)) {
				up_edge = spmi_btn_edge(&spmi_btns.vol_up, (rt & 0x01u) == 0);
			}
			if (up_edge) {
				ui_menu_select(&choice, (choice + 2) % 3, countdown, &user_interacted);
			} else if (down_edge) {
				ui_menu_select(&choice, (choice + 1) % 3, countdown, &user_interacted);
			} else if (pwr_edge) {
				break;
			}
		}

		if (!user_interacted) {
			uint64_t elapsed = now - start_time;
			uint64_t total_ticks = 5 * freq;
			if (elapsed >= total_ticks) {
				choice = MENU_CHOICE_IOS;
				break;
			}
			int sec_left = 5 - (int)(elapsed / freq);
			int pct_left = (int)((total_ticks - elapsed) * 100 / (total_ticks ? total_ticks : 1));
			if (sec_left != countdown || pct_left / 5 != last_pct / 5) {
				int redraw_text = (sec_left != countdown);
				countdown = sec_left;
				last_pct = pct_left;
				ui_draw_menu_footer(countdown, pct_left, 0, redraw_text);
			}
		}

		for (volatile int d = 0; d < 200; d++) {
		}
	}

	if (choice == MENU_CHOICE_IOS) {
		UI.usb_chrome_drawn = 0;
		ui_draw_ios_boot_splash();
		uint64_t splash_t0 = ui_now();
		while (usb_connected && !usb_menu_interrupted && ui_now() - splash_t0 < freq / 2) {
			usb_poll_command();
		}
		if (usb_menu_interrupted) {
			choice = MENU_CHOICE_FASTBOOT;
			UI_BOOT.active = 0;
			ui_fill(0, 0, (int)SCREEN.width, (int)SCREEN.height, UI_BG);
		}
	} else {
		ui_fill(0, 0, (int)SCREEN.width, (int)SCREEN.height, UI_BG);
	}
	return choice;
}

static void
gpt_sync_lun(efi_block_io *bio)
{
	if (!bio || !bio->media) return;
	uint32_t bs = bio->media->block_size;
	if (bs < 512 || bs > 4096) return;

	static uint8_t primary_hdr[4096] __attribute__((aligned(4096)));
	static uint8_t backup_hdr[4096] __attribute__((aligned(4096)));

	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, primary_hdr)) ||
	    memcmp(primary_hdr, "EFI PART", 8) != 0) {
		return;
	}

	uint32_t hdr_size = *(uint32_t *)(primary_hdr + 12);
	if (hdr_size < 92 || hdr_size > bs) hdr_size = 92;

	uint64_t alt_lba = *(uint64_t *)(primary_hdr + 32);
	uint32_t num_entries = *(uint32_t *)(primary_hdr + 80);
	uint32_t entry_size = *(uint32_t *)(primary_hdr + 84);
	uint64_t total_bytes = (uint64_t)num_entries * entry_size;
	uint64_t num_blocks = (total_bytes + bs - 1) / bs;
	if (total_bytes == 0 || total_bytes > sizeof(ufs_buffer) || alt_lba == 0) return;

	uint32_t entries_crc = ufs_crc32(ufs_buffer, total_bytes);
	*(uint32_t *)(primary_hdr + 88) = entries_crc;
	*(uint32_t *)(primary_hdr + 16) = 0;
	uint32_t primary_crc = ufs_crc32(primary_hdr, hdr_size);
	*(uint32_t *)(primary_hdr + 16) = primary_crc;

	(void)bio->write_blocks(bio, bio->media->media_id, 1, bs, primary_hdr);
	uint64_t backup_entries_lba = alt_lba - num_blocks;
	(void)safe_write_blocks(bio, backup_entries_lba, num_blocks, bs, ufs_buffer);

	memcpy(backup_hdr, primary_hdr, bs);
	*(uint64_t *)(backup_hdr + 24) = alt_lba;
	*(uint64_t *)(backup_hdr + 32) = 1;
	*(uint64_t *)(backup_hdr + 72) = backup_entries_lba;
	*(uint32_t *)(backup_hdr + 88) = entries_crc;
	*(uint32_t *)(backup_hdr + 16) = 0;
	uint32_t backup_crc = ufs_crc32(backup_hdr, hdr_size);
	*(uint32_t *)(backup_hdr + 16) = backup_crc;

	(void)bio->write_blocks(bio, bio->media->media_id, alt_lba, bs, backup_hdr);
	bio->flush_blocks(bio);
	print("slot: GPT CRC-32 synchronized (entries=%x primary=%x backup=%x at LBA %x)\n",
	    (uint64_t)entries_crc, (uint64_t)primary_crc, (uint64_t)backup_crc, alt_lba);
}

static uint32_t
gpt_swap_ab_type_guids(uint8_t *entries, uint32_t num_entries, uint32_t entry_size, char target_slot)
{
	/* Qualcomm inactive slot PartitionTypeGUID: 77036cd4-03d5-42bb-8ed1-37e5a88baa34 */
	static const uint8_t inactive_guid[16] = {
		0xd4, 0x6c, 0x03, 0x77, 0xd5, 0x03, 0xbb, 0x42,
		0x8e, 0xd1, 0x37, 0xe5, 0xa8, 0x8b, 0xaa, 0x34
	};
	uint32_t swapped = 0;
	for (uint32_t i = 0; i < num_entries; i++) {
		uint8_t *ei = entries + i * entry_size;
		const uint8_t *pi = ei + 56;
		uint32_t len = 0;
		while (len < 35 && (pi[len * 2] != 0 || pi[len * 2 + 1] != 0)) len++;
		if (len < 3 || pi[(len - 2) * 2] != '_' || pi[(len - 2) * 2 + 1] != 0 ||
		    pi[(len - 1) * 2] != 'a' || pi[(len - 1) * 2 + 1] != 0) {
			continue;
		}
		for (uint32_t j = 0; j < num_entries; j++) {
			uint8_t *ej = entries + j * entry_size;
			const uint8_t *pj = ej + 56;
			if (pj[len * 2] != 0 || pj[len * 2 + 1] != 0 ||
			    pj[(len - 2) * 2] != '_' || pj[(len - 2) * 2 + 1] != 0 ||
			    pj[(len - 1) * 2] != 'b' || pj[(len - 1) * 2 + 1] != 0) {
				continue;
			}
			if (memcmp(pi, pj, (len - 2) * 2) == 0) {
				uint8_t *et = (target_slot == 'b') ? ej : ei;
				uint8_t *eo = (target_slot == 'b') ? ei : ej;
				/* Only swap A/B pairs where the target slot currently holds the inactive GUID. */
				if (memcmp(et, inactive_guid, 16) == 0 && memcmp(eo, inactive_guid, 16) != 0) {
					uint8_t tmp[16];
					memcpy(tmp, ei, 16);
					memcpy(ei, ej, 16);
					memcpy(ej, tmp, 16);
					swapped++;
					int is_boot = (len == 6 && pi[0] == 'b' && pi[2] == 'o' && pi[4] == 'o' && pi[6] == 't');
					uint64_t keep_t = *(uint64_t *)(et + 48) & 0x1000000000000001ULL;
					uint64_t keep_o = *(uint64_t *)(eo + 48) & 0x1000000000000001ULL;
					*(uint64_t *)(et + 48) = is_boot ? 0x006f000000000000ULL : (keep_t | 0x007f000000000000ULL);
					*(uint64_t *)(eo + 48) = is_boot ? 0x006a000000000000ULL : (keep_o | 0x00c4000000000000ULL);
				}
				break;
			}
		}
	}
	return swapped;
}

static void
slot_apply_active(char target_slot)
{
	if (target_slot != 'a' && target_slot != 'b') {
		return;
	}
	efi_block_io *bio = lun4_get_bio();
	if (!bio || !bio->media) return;
	uint32_t bs = bio->media->block_size;
	if (bs < 512 || bs > 4096) return;

	static uint8_t hdr[4096] __attribute__((aligned(4096)));
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, hdr)) ||
	    memcmp(hdr, "EFI PART", 8) != 0) {
		return;
	}

	uint64_t entries_lba = *(uint64_t *)(hdr + 72);
	uint32_t num_entries = *(uint32_t *)(hdr + 80);
	uint32_t entry_size = *(uint32_t *)(hdr + 84);
	uint64_t total_bytes = (uint64_t)num_entries * entry_size;
	uint64_t num_blocks = (total_bytes + bs - 1) / bs;
	if (total_bytes == 0 || total_bytes > sizeof(ufs_buffer)) return;
	if (EFI_ERROR(safe_read_blocks(bio, entries_lba, num_blocks, bs, ufs_buffer))) return;

	const char *target_name = (target_slot == 'b') ? "boot_b" : "boot_a";
	const char *other_name = (target_slot == 'b') ? "boot_a" : "boot_b";
	uint8_t *entry = gpt_find_entry(ufs_buffer, num_entries, entry_size, target_name);
	uint8_t *entry_other = gpt_find_entry(ufs_buffer, num_entries, entry_size, other_name);
	if (!entry) return;

	uint32_t n4 = gpt_swap_ab_type_guids(ufs_buffer, num_entries, entry_size, target_slot);
	uint64_t want_t = 0x006f000000000000ULL, want_o = 0x006a000000000000ULL;
	if (n4 > 0 || *(uint64_t *)(entry + 48) != want_t ||
	    (entry_other && *(uint64_t *)(entry_other + 48) != want_o)) {
		*(uint64_t *)(entry + 48) = want_t;
		if (entry_other) *(uint64_t *)(entry_other + 48) = want_o;
		if (!EFI_ERROR(safe_write_blocks(bio, entries_lba, num_blocks, bs, ufs_buffer))) {
			gpt_sync_lun(bio);
			print("slot: LUN 4 Slot %s (%s) active=0x006f000000000000 (swapped=%d)\n",
			    target_slot == 'b' ? "b" : "a", target_name, (uint64_t)n4);
		}
	}

	efi_block_io *bio0 = lun_get_bio(0, "super");
	if (bio0 && bio0->media && bio0->media->block_size == bs &&
	    !EFI_ERROR(bio0->read_blocks(bio0, bio0->media->media_id, 1, bs, hdr)) &&
	    memcmp(hdr, "EFI PART", 8) == 0) {
		uint64_t lba0 = *(uint64_t *)(hdr + 72);
		uint32_t ne0 = *(uint32_t *)(hdr + 80), es0 = *(uint32_t *)(hdr + 84);
		uint64_t tb0 = (uint64_t)ne0 * es0, nb0 = (tb0 + bs - 1) / bs;
		if (tb0 > 0 && tb0 <= sizeof(ufs_buffer) &&
		    !EFI_ERROR(safe_read_blocks(bio0, lba0, nb0, bs, ufs_buffer))) {
			uint32_t n0 = gpt_swap_ab_type_guids(ufs_buffer, ne0, es0, target_slot);
			if (n0 > 0 && !EFI_ERROR(safe_write_blocks(bio0, lba0, nb0, bs, ufs_buffer))) {
				gpt_sync_lun(bio0);
				print("slot: swapped %d A/B PartitionTypeGUID pairs on LUN 0 for Slot %s\n",
				    (uint64_t)n0, target_slot == 'b' ? "b" : "a");
			}
		}
	}
}

static void
slot_maintain_current(void)
{
	slot_apply_active(loader_slot);
}

static void
slot_set_active_other(void)
{
	slot_apply_active(android_slot);
}

/* Compatibility wrappers */
static void __attribute__((unused)) slot_maintain_a(void) { slot_maintain_current(); }
static void __attribute__((unused)) slot_set_active_b(void) { slot_set_active_other(); }

/* Only the boot partition that already holds the loader is ever written (slot_choice.h). */
static const char *
slot_flash_boot_image(char slot, uint64_t bytes)
{
	char16 name[] = u"boot_a";
	if (loader_slot == 0) {
		return "no single boot partition holds the loader: flash it from LK";
	}
	if (slot != loader_slot) {
		return "that boot partition does not hold the loader";
	}
	name[5] = (char16)slot;
	efi_block_io *bio = find_partition_named(name, sizeof(name) / sizeof(name[0]));
	if (bio == NULL || bio->media == NULL || bio->media->block_size == 0) {
		return "boot partition not found";
	}
	if (bytes < 4096 || memcmp((const void *)PAYLOAD_BASE, "ANDROID!", 8) != 0) {
		return "staged data is not an ANDROID! boot image";
	}
	uint64_t bs = bio->media->block_size;
	uint64_t total = (bytes + bs - 1) / bs * bs;
	uint64_t max_bytes = (bio->media->last_block + 1) * bs;
	if (total > max_bytes || total > PAYLOAD_CONTAINER_MAX) {
		return "boot image exceeds partition or payload buffer";
	}
	for (uint64_t off = 0; off < total; off += (4u << 20)) {
		uint64_t chunk = total - off < (4u << 20) ? total - off : (4u << 20);
		efi_status ws = bio->write_blocks(bio, bio->media->media_id, off / bs, chunk,
		    (void *)(uintptr_t)(PAYLOAD_BASE + off));
		if (EFI_ERROR(ws)) {
			return "write_blocks failed";
		}
		wdt_pet();
	}
	bio->flush_blocks(bio);
	slot_maintain_current();
	return NULL;
}

static void
ui_draw_fault_banner(const char *status_line, const char *klog, uint64_t klog_len)
{
	if (!ui_on()) {
		return;
	}
	int card_x = UI_MARGIN;
	int card_w = (int)SCREEN.width - 2 * UI_MARGIN;
	int top_y = 1250;
	int card_h = (int)SCREEN.height - top_y - 60;
	if (card_h < 400) {
		top_y = (int)SCREEN.height - 920;
		if (top_y < 0) top_y = 0;
		card_h = (int)SCREEN.height - top_y - 40;
	}

	ui_round_rect(card_x, top_y, card_w, card_h, 36, 3, UI_FAULT_BG, UI_FAULT_BORDER, UI_BG);
	ui_round_rect(card_x + 20, top_y + 20, card_w - 40, 68, 22, 0, UI_FAULT_HDR_BG, UI_FAULT_HDR_BG, UI_FAULT_BG);

	const char *hdr = "KERNEL FAULT / HUNG - FASTBOOT RECOVERY";
	int hw = ui_measure_text_ex(hdr, 44, UI_AMOUNT_SCALE, 0);
	ui_draw_text_ex(((int)SCREEN.width - hw) / 2, top_y + 38, hdr, UI_AMOUNT_SCALE, 44, UI_RED, UI_FAULT_HDR_BG, 0, 1);

	int maxc = 96;
	int y = top_y + 112;
	ui_draw_text_ex(card_x + 32, y, status_line, UI_LOG_SCALE, maxc, UI_AMBER, UI_FAULT_BG, 1, 1);
	y += UI_LOG_STEP + 8;

	const char *src = (klog != NULL && klog_len > 0 && klog_len <= (16u << 20)) ? klog : log_buf;
	uint64_t slen = (src == klog) ? klog_len : log_len;
	while (slen > 0 && (src[slen - 1] == 0 || src[slen - 1] == '\n' || src[slen - 1] == '\r')) {
		slen--;
	}
	int max_y = top_y + card_h - 36;
	/* First show any line containing "panic" */
	for (uint64_t i = 0; i + 5 < slen && y + 32 < max_y - 120; i++) {
		if ((src[i] == 'p' || src[i] == 'P') &&
		    (src[i + 1] == 'a' || src[i + 1] == 'A') &&
		    (src[i + 2] == 'n' || src[i + 2] == 'N') &&
		    (src[i + 3] == 'i' || src[i + 3] == 'I') &&
		    (src[i + 4] == 'c' || src[i + 4] == 'C')) {
			uint64_t ls = i, le = i;
			while (ls > 0 && src[ls - 1] != '\n') ls--;
			while (le < slen && src[le] != '\n' && src[le] != '\r') le++;
			int n = (int)(le - ls);
			if (n > maxc) n = maxc;
			ui_draw_text_ex(card_x + 32, y, src + ls, UI_LOG_SCALE, n, 0x00ff8a80u, UI_FAULT_BG, 1, 1);
			y += UI_LOG_STEP;
			i = le;
		}
	}
	/* Then show the last lines of the log */
	int rem_rows = (max_y - y) / UI_LOG_STEP;
	if (rem_rows > 0 && slen > 0) {
		uint64_t start = slen;
		for (int lines = 0; start > 0 && lines < rem_rows; start--) {
			if (src[start - 1] == '\n') lines++;
		}
		for (uint64_t line = start, p; line < slen && y + 32 <= max_y; line = p + 1) {
			for (p = line; p < slen && src[p] != '\n'; p++) {}
			int n = (int)(p - line);
			while (n > 0 && src[line + n - 1] == '\r') n--;
			if (n > maxc) n = maxc;
			ui_draw_text_ex(card_x + 32, y, src + line, UI_LOG_SCALE, n, 0x00e5c5c8u, UI_FAULT_BG, 1, 0);
			y += UI_LOG_STEP;
		}
	}
	ui_flush_splash_fb();
}

static void reboot(void);

static const char *
menu_enter_fastboot(void)
{
	/* The menu and kernel watcher both service USB commands. Preserve ownership
	 * if the countdown has already handed control to the kernel. */
	if (running_live != NULL || kernel_resources_held || firmware_fault || held_irq_masked) {
		return "kernel resources are unavailable for recovery";
	}
	usb_menu_interrupted = 1;
	return NULL;
}

static void
boot_fallback_enter(void)
{
	char saved_status[64];
	int i = 0;
	while (i < 63 && last_result[i]) {
		saved_status[i] = last_result[i];
		i++;
	}
	saved_status[i] = 0;
	print("boot fallback: entering Fastboot debug mode (%s)\n", saved_status);
	boot_fallback_active = 1;
	if (SCREEN.base != 0) UI.ready = 1;
	host_last_cmd = READ_SYSREG(CNTVCT_EL0);
	ui_stage("BOOT FALLBACK - FASTBOOT MODE");
	ui_draw_fault_banner(saved_status, (const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
	UI.log_seen = log_len;
}

static void
boot_fallback_check(void)
{
	if (boot_fallback_active &&
	    READ_SYSREG(CNTVCT_EL0) - host_last_cmd > 120 * READ_SYSREG(CNTFRQ_EL0)) {
		reboot();
	}
}

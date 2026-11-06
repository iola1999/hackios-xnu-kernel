/* ------------------------------------------------------------------------ */
/* Status on the panel.                                                     */
/*                                                                          */
/* The firmware console font is fixed at 8x16 and the command loop turns    */
/* the console off (screen_quiet), so once the loader waits for USB nothing */
/* on the panel moves. This paints the status where it can be read from a   */
/* distance: the stage, a bar with the megabytes received, a block that     */
/* moves while the loader polls USB, and the tail of the log. The pixels go */
/* into the same framebuffer SCREEN names; the fourth byte stays zero,      */
/* which is what the panel and the raw screenshots already carry.           */
/*                                                                          */
/* The panel belongs to the kernel from "oem run" on: nothing is drawn      */
/* while a kernel is live or may still hold the hardware, after a firmware  */
/* fault, or after a started kernel came back (its last frame is what       */
/* oem screenshot is for).                                                  */
/*                                                                          */
/* Included by loader_06 ahead of the LC-USB block: usb_wait() calls       */
/* ui_tick from its poll loop, and the host tests stub that one call.       */

#define UI_WHITE        0x00ffffffu
#define UI_BLACK        0x00000000u
#define UI_LEFT         78
#define UI_TITLE_SCALE  10              /* a 3x5 glyph is 30x50 pixels */
#define UI_AMOUNT_SCALE 8
#define UI_LOG_SCALE    4
#define UI_LOG_LINES    8
#define UI_LOG_STEP     30

/* 3x5 glyphs, rows top to bottom; '#' is a dot that lights up. */
#define G(a, b, c, d, e) a b c d e
static const char *const ui_fonts[128] = {
	[' '] = G("...", "...", "...", "...", "..."),
	['-'] = G("...", "...", "###", "...", "..."),
	['_'] = G("...", "...", "...", "...", "###"),
	['.'] = G("...", "...", "...", "...", ".#."),
	[','] = G("...", "...", "...", ".#.", "#.."),
	['/'] = G("..#", "..#", ".#.", "#..", "#.."),
	[':'] = G("...", ".#.", "...", ".#.", "..."),
	[';'] = G("...", ".#.", "...", ".#.", "#.."),
	['='] = G("...", "###", "...", "###", "..."),
	['+'] = G("...", ".#.", "###", ".#.", "..."),
	['*'] = G("#.#", ".#.", "#.#", "...", "..."),
	['('] = G("..#", ".#.", ".#.", ".#.", "..#"),
	[')'] = G("#..", ".#.", ".#.", ".#.", "#.."),
	['['] = G("##.", "#..", "#..", "#..", "##."),
	[']'] = G(".##", "..#", "..#", "..#", ".##"),
	['<'] = G("..#", ".#.", "#..", ".#.", "..#"),
	['>'] = G("#..", ".#.", "..#", ".#.", "#.."),
	['!'] = G(".#.", ".#.", ".#.", "...", ".#."),
	['\''] = G(".#.", ".#.", "...", "...", "..."),
	['"'] = G("#.#", "#.#", "...", "...", "..."),
	['#'] = G("#.#", "###", "#.#", "###", "#.#"),
	['|'] = G(".#.", ".#.", ".#.", ".#.", ".#."),
	['%'] = G("#.#", "..#", ".#.", "#..", "#.#"),
	['?'] = G("##.", "..#", ".#.", "...", ".#."),
	['0'] = G("###", "#.#", "#.#", "#.#", "###"),
	['1'] = G(".#.", "##.", ".#.", ".#.", "###"),
	['2'] = G("##.", "..#", ".#.", "#..", "###"),
	['3'] = G("##.", "..#", ".#.", "..#", "##."),
	['4'] = G("#.#", "#.#", "###", "..#", "..#"),
	['5'] = G("###", "#..", "##.", "..#", "##."),
	['6'] = G(".##", "#..", "###", "#.#", "###"),
	['7'] = G("###", "..#", ".#.", ".#.", ".#."),
	['8'] = G("###", "#.#", "###", "#.#", "###"),
	['9'] = G("###", "#.#", "###", "..#", "##."),
	['A'] = G(".#.", "#.#", "###", "#.#", "#.#"),
	['B'] = G("##.", "#.#", "##.", "#.#", "##."),
	['C'] = G(".##", "#..", "#..", "#..", ".##"),
	['D'] = G("##.", "#.#", "#.#", "#.#", "##."),
	['E'] = G("###", "#..", "##.", "#..", "###"),
	['F'] = G("###", "#..", "##.", "#..", "#.."),
	['G'] = G(".##", "#..", "#.#", "#.#", ".##"),
	['H'] = G("#.#", "#.#", "###", "#.#", "#.#"),
	['I'] = G("###", ".#.", ".#.", ".#.", "###"),
	['J'] = G("..#", "..#", "..#", "#.#", ".#."),
	['K'] = G("#.#", "#.#", "##.", "#.#", "#.#"),
	['L'] = G("#..", "#..", "#..", "#..", "###"),
	['M'] = G("#.#", "###", "###", "#.#", "#.#"),
	['N'] = G("##.", "#.#", "#.#", "#.#", "#.#"),
	['O'] = G(".#.", "#.#", "#.#", "#.#", ".#."),
	['P'] = G("##.", "#.#", "##.", "#..", "#.."),
	['Q'] = G(".#.", "#.#", "#.#", "##.", ".##"),
	['R'] = G("##.", "#.#", "##.", "#.#", "#.#"),
	['S'] = G(".##", "#..", ".#.", "..#", "##."),
	['T'] = G("###", ".#.", ".#.", ".#.", ".#."),
	['U'] = G("#.#", "#.#", "#.#", "#.#", "###"),
	['V'] = G("#.#", "#.#", "#.#", "#.#", ".#."),
	['W'] = G("#.#", "#.#", "###", "###", "#.#"),
	['X'] = G("#.#", "#.#", ".#.", "#.#", "#.#"),
	['Y'] = G("#.#", "#.#", ".#.", ".#.", ".#."),
	['Z'] = G("###", "..#", ".#.", "#..", "###"),
};
#undef G

static struct {
	int ready;
	const char *stage;      /* what ui_stage last drew; compared by text */
	int pct;                /* fill already on the bar, -1 before the first draw */
	int spin;               /* block position, 0..3 */
	uint64_t spin_at, amount_at, log_seen;
	uint64_t done, total;
} UI;

static uint64_t
ui_now(void)
{
	return READ_SYSREG(CNTVCT_EL0);
}

static uint64_t
ui_freq(void)
{
	uint64_t f = READ_SYSREG(CNTFRQ_EL0);

	return f ? f : 1000000;   /* every board here sets CNTFRQ; the fallback only keeps the block moving */
}

static int
ui_on(void)
{
	return UI.ready && running_live == NULL && !kernel_resources_held && !firmware_fault;
}

/* One solid rectangle, clipped to the panel. */
static void
ui_fill(int x, int y, int w, int h, uint32_t color)
{
	volatile uint32_t *fb;
	unsigned stride;
	int x1 = x + w, y1 = y + h;

	if (SCREEN.base == 0) {
		return;
	}
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > (int)SCREEN.width) x1 = (int)SCREEN.width;
	if (y1 > (int)SCREEN.height) y1 = (int)SCREEN.height;
	fb = (volatile uint32_t *)(uintptr_t)SCREEN.base;
	stride = SCREEN.stride / 4;
	for (int row = y; row < y1; row++) {
		for (int col = x; col < x1; col++) {
			fb[(unsigned)row * stride + (unsigned)col] = color;
		}
	}
}

static const char *
ui_glyph(char c)
{
	unsigned char u = (unsigned char)c;

	if (u >= 'a' && u <= 'z') {
		u = (unsigned char)(u - 'a' + 'A');
	}
	if (u >= 128 || ui_fonts[u] == 0) {
		return ui_fonts['?'];
	}
	return ui_fonts[u];
}

/* Draw at most max_chars of s (which need not be terminated there). */
static void
ui_text(int x, int y, const char *s, int scale, int max_chars)
{
	int cx = x;

	for (int n = 0; n < max_chars && s[n]; n++) {
		const char *g = ui_glyph(s[n]);

		for (int r = 0; r < 5; r++) {
			for (int c = 0; c < 3; c++) {
				if (g[r * 3 + c] == '#') {
					ui_fill(cx + c * scale, y + r * scale, scale, scale, UI_WHITE);
				}
			}
		}
		cx += 4 * scale;
	}
}

static int
ui_text_width(const char *s, int len, int scale)
{
	int n = 0;

	while (n < len && s[n]) {
		n++;
	}
	return n ? n * 4 * scale - scale : 0;
}

static int
ui_dec(char *out, uint64_t v)
{
	char tmp[20];
	int n = 0, i;

	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	for (i = 0; i < n; i++) {
		out[i] = tmp[n - 1 - i];
	}
	return n;
}

static void
ui_stage(const char *word)
{
	int y = (int)SCREEN.height - 380;
	int w;

	if (!ui_on() || (UI.stage != 0 && str_eq(UI.stage, word))) {
		return;
	}
	UI.stage = word;
	w = ui_text_width(word, 64, UI_TITLE_SCALE);
	ui_fill(0, y - 10, (int)SCREEN.width, 70, UI_BLACK);
	ui_text(((int)SCREEN.width - w) / 2, y, word, UI_TITLE_SCALE, 64);
}

static void
ui_bar(int pct)
{
	int x = UI_LEFT, y = (int)SCREEN.height - 270;
	int w = (int)SCREEN.width - 2 * UI_LEFT, h = 48, t = 4;
	int fill = (w - 2 * t) * pct / 100;

	ui_fill(x, y, w, h, UI_BLACK);
	ui_fill(x, y, w, t, UI_WHITE);
	ui_fill(x, y + h - t, w, t, UI_WHITE);
	ui_fill(x, y, t, h, UI_WHITE);
	ui_fill(x + w - t, y, t, h, UI_WHITE);
	if (fill > 0) {
		ui_fill(x + t, y + t, fill, h - 2 * t, UI_WHITE);
	}
}

static void
ui_amount(void)
{
	char buf[48];
	int n = ui_dec(buf, UI.done >> 20);
	int y = (int)SCREEN.height - 180;
	int w;

	buf[n++] = ' ';
	buf[n++] = '/';
	buf[n++] = ' ';
	n += ui_dec(buf + n, UI.total >> 20);
	buf[n++] = ' ';
	buf[n++] = 'M';
	buf[n++] = 'B';
	w = ui_text_width(buf, n, UI_AMOUNT_SCALE);
	ui_fill(0, y - 6, (int)SCREEN.width, 5 * UI_AMOUNT_SCALE + 12, UI_BLACK);
	ui_text(((int)SCREEN.width - w) / 2, y, buf, UI_AMOUNT_SCALE, n);
}

static void
ui_progress(uint64_t done, uint64_t total)
{
	int pct, moved;

	if (!ui_on() || total == 0) {
		return;
	}
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

/* The moving block: four positions in a row, one step every 250 ms. */
static void
ui_spin_draw(void)
{
	int x = UI_LEFT, y = (int)SCREEN.height - 480;
	int prev = (UI.spin + 3) & 3;

	ui_fill(x + prev * 60, y, 40, 40, UI_BLACK);
	ui_fill(x + UI.spin * 60, y, 40, 40, UI_WHITE);
}

static void
ui_log(void)
{
	int y = (int)SCREEN.height - 760;
	int maxc = ((int)SCREEN.width - 2 * UI_LEFT) / (4 * UI_LOG_SCALE);
	uint32_t end = log_len, p, start = 0, line;
	int seen = 0;

	ui_fill(0, y - 10, (int)SCREEN.width, UI_LOG_LINES * UI_LOG_STEP + 20, UI_BLACK);
	UI.log_seen = log_len;
	while (end > 0 && (log_buf[end - 1] == '\n' || log_buf[end - 1] == '\r')) {
		end--;
	}
	for (p = end; p > 0; ) {
		if (log_buf[--p] == '\n' && ++seen == UI_LOG_LINES) {
			start = p + 1;
			break;
		}
	}
	for (line = start; line < end && y < (int)SCREEN.height - 500; line = p + 1) {
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
		ui_text(UI_LEFT, y, log_buf + line, UI_LOG_SCALE, n);
		y += UI_LOG_STEP;
	}
}

static void
ui_init(void)
{
	if (SCREEN.base == 0) {
		return;
	}
	memset(&UI, 0, sizeof(UI));
	UI.pct = -1;
	UI.ready = 1;
	UI.spin_at = ui_now();
	if (!ui_on()) {
		return;
	}
	ui_fill(0, (int)SCREEN.height - 820, (int)SCREEN.width, 820, UI_BLACK);
	ui_stage("WAITING FOR HOST");
	ui_spin_draw();
	ui_log();
}

/* Called from usb_wait's poll loop: two counter reads when nothing is due. */
static void
ui_tick(void)
{
	if (!ui_on()) {
		return;
	}
	if (log_len != UI.log_seen) {
		ui_log();
	}
	if (ui_now() - UI.spin_at >= ui_freq() / 4) {
		UI.spin_at = ui_now();
		UI.spin = (UI.spin + 1) & 3;
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

static char loader_slot = 'a';
static char android_slot = 'b';
static char menu_item_ios[40] = "[1] BOOT IOS 26 (SLOT A)";
static char menu_item_android[40] = "[2] BOOT ANDROID (SLOT B)";
static const char *const menu_item_fastboot = "[3] FASTBOOT USB DIAGNOSTICS";

static const char *
menu_item_str(int i)
{
	if (i == 0) return menu_item_ios;
	if (i == 1) return menu_item_android;
	return menu_item_fastboot;
}

static void
slot_update_menu_text(void)
{
	char up_loader = (loader_slot >= 'a' && loader_slot <= 'z') ? (loader_slot - 32) : loader_slot;
	char up_android = (android_slot >= 'a' && android_slot <= 'z') ? (android_slot - 32) : android_slot;

	menu_item_ios[0] = '['; menu_item_ios[1] = '1'; menu_item_ios[2] = ']'; menu_item_ios[3] = ' ';
	menu_item_ios[4] = 'B'; menu_item_ios[5] = 'O'; menu_item_ios[6] = 'O'; menu_item_ios[7] = 'T';
	menu_item_ios[8] = ' '; menu_item_ios[9] = 'I'; menu_item_ios[10] = 'O'; menu_item_ios[11] = 'S';
	menu_item_ios[12] = ' '; menu_item_ios[13] = '2'; menu_item_ios[14] = '6'; menu_item_ios[15] = ' ';
	menu_item_ios[16] = '('; menu_item_ios[17] = 'S'; menu_item_ios[18] = 'L'; menu_item_ios[19] = 'O';
	menu_item_ios[20] = 'T'; menu_item_ios[21] = ' '; menu_item_ios[22] = up_loader; menu_item_ios[23] = ')';
	menu_item_ios[24] = 0;

	menu_item_android[0] = '['; menu_item_android[1] = '2'; menu_item_android[2] = ']'; menu_item_android[3] = ' ';
	menu_item_android[4] = 'B'; menu_item_android[5] = 'O'; menu_item_android[6] = 'O'; menu_item_android[7] = 'T';
	menu_item_android[8] = ' '; menu_item_android[9] = 'A'; menu_item_android[10] = 'N'; menu_item_android[11] = 'D';
	menu_item_android[12] = 'R'; menu_item_android[13] = 'O'; menu_item_android[14] = 'I'; menu_item_android[15] = 'D';
	menu_item_android[16] = ' '; menu_item_android[17] = '('; menu_item_android[18] = 'S'; menu_item_android[19] = 'L';
	menu_item_android[20] = 'O'; menu_item_android[21] = 'T'; menu_item_android[22] = ' ';
	menu_item_android[23] = up_android; menu_item_android[24] = ')'; menu_item_android[25] = 0;
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

static void
slot_detect(void)
{
	efi_block_io *bio = lun4_get_bio();
	if (!bio || !bio->media) {
		slot_update_menu_text();
		return;
	}
	uint32_t bs = bio->media->block_size;
	if (bs < 512 || bs > 4096) {
		slot_update_menu_text();
		return;
	}

	static uint8_t hdr[4096] __attribute__((aligned(4096)));
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, hdr))) {
		slot_update_menu_text();
		return;
	}
	if (memcmp(hdr, "EFI PART", 8) != 0) {
		slot_update_menu_text();
		return;
	}

	uint64_t entries_lba = *(uint64_t *)(hdr + 72);
	uint32_t num_entries = *(uint32_t *)(hdr + 80);
	uint32_t entry_size = *(uint32_t *)(hdr + 84);
	uint64_t total_bytes = (uint64_t)num_entries * entry_size;
	uint64_t num_blocks = (total_bytes + bs - 1) / bs;

	if (total_bytes == 0 || total_bytes > sizeof(ufs_buffer)) {
		slot_update_menu_text();
		return;
	}

	if (EFI_ERROR(safe_read_blocks(bio, entries_lba, num_blocks, bs, ufs_buffer))) {
		slot_update_menu_text();
		return;
	}

	uint8_t *ea = gpt_find_entry(ufs_buffer, num_entries, entry_size, "boot_a");
	uint8_t *eb = gpt_find_entry(ufs_buffer, num_entries, entry_size, "boot_b");

	uint64_t attr_a = ea ? *(uint64_t *)(ea + 48) : 0;
	uint64_t attr_b = eb ? *(uint64_t *)(eb + 48) : 0;

	int a_unbootable = (attr_a & 0x0080000000000000ULL) != 0;
	int b_unbootable = (attr_b & 0x0080000000000000ULL) != 0;
	int a_active = (attr_a & 0x0004000000000000ULL) != 0;
	int b_active = (attr_b & 0x0004000000000000ULL) != 0;

	if (!b_unbootable && (a_unbootable || (b_active && !a_active))) {
		loader_slot = 'b';
		android_slot = 'a';
	} else {
		loader_slot = 'a';
		android_slot = 'b';
	}

	print("slot: detected loader_slot=%s, android_slot=%s (attr_a=%x attr_b=%x)\n",
	    loader_slot == 'b' ? "b" : "a", android_slot == 'b' ? "b" : "a", attr_a, attr_b);
	slot_update_menu_text();
}

static void
ui_draw_menu(int choice, int countdown, int user_interacted)
{
	int mid_y = (int)SCREEN.height / 3;

	const char *title = "HACKIOS K90 BOOT MENU";
	int tw = ui_text_width(title, 32, UI_TITLE_SCALE);
	ui_text(((int)SCREEN.width - tw) / 2, mid_y - 60, title, UI_TITLE_SCALE, 32);

	for (int i = 0; i < 3; i++) {
		const char *item = menu_item_str(i);
		int y = mid_y + 100 + i * 80;
		int w = ui_text_width(item, 32, UI_AMOUNT_SCALE);
		int x = ((int)SCREEN.width - w) / 2;
		ui_fill(x - 20, y - 6, w + 40, 5 * UI_AMOUNT_SCALE + 12, UI_BLACK);
		if (i == choice) {
			ui_fill(x - 16, y - 4, w + 32, 5 * UI_AMOUNT_SCALE + 8, UI_WHITE);
			for (int n = 0; n < 32 && item[n]; n++) {
				const char *g = ui_glyph(item[n]);
				for (int r = 0; r < 5; r++) {
					for (int c = 0; c < 3; c++) {
						if (g[r * 3 + c] == '#') {
							ui_fill(x + n * 4 * UI_AMOUNT_SCALE + c * UI_AMOUNT_SCALE,
							    y + r * UI_AMOUNT_SCALE, UI_AMOUNT_SCALE, UI_AMOUNT_SCALE, UI_BLACK);
						}
					}
				}
			}
		} else {
			ui_text(x, y, item, UI_AMOUNT_SCALE, 32);
		}
	}

	int cy = mid_y + 360;
	ui_fill(0, cy - 6, (int)SCREEN.width, 5 * UI_LOG_SCALE + 12, UI_BLACK);
	if (!user_interacted && countdown > 0) {
		char cmsg[48];
		cmsg[0] = 'A'; cmsg[1] = 'U'; cmsg[2] = 'T'; cmsg[3] = 'O'; cmsg[4] = ' ';
		cmsg[5] = 'B'; cmsg[6] = 'O'; cmsg[7] = 'O'; cmsg[8] = 'T'; cmsg[9] = ' ';
		cmsg[10] = 'I'; cmsg[11] = 'N'; cmsg[12] = ' ';
		cmsg[13] = (char)('0' + countdown);
		cmsg[14] = ' '; cmsg[15] = 'S'; cmsg[16] = 0;
		int cw = ui_text_width(cmsg, 16, UI_LOG_SCALE);
		ui_text(((int)SCREEN.width - cw) / 2, cy, cmsg, UI_LOG_SCALE, 16);
	} else {
		const char *hint = "VOL KEYS: SELECT   PWR: CONFIRM";
		int hw = ui_text_width(hint, 32, UI_LOG_SCALE);
		ui_text(((int)SCREEN.width - hw) / 2, cy, hint, UI_LOG_SCALE, 32);
	}
}

#define SPMI_ARB_CORE   ((uintptr_t)0x0c400000ULL)
#define SPMI_ARB_OBSRV  ((uintptr_t)0x0c440000ULL)

struct spmi_btn {
	int usable;
	int live;
	int last;
	int down;
};

struct spmi_btn_ctx {
	int pon_ch;
	int pin_ch;
	struct spmi_btn pwr;
	struct spmi_btn vol_up;
	struct spmi_btn vol_down;
};

static int
spmi_obsrv_read(int ch, uint8_t reg, uint8_t *buf, uint32_t count)
{
	if (ch < 0 || count == 0 || count > 8) return 0;
	uintptr_t base = SPMI_ARB_OBSRV + (uintptr_t)0x20ULL * (uintptr_t)ch;
	uint32_t opc = (count > 4) ? 1u : 13u;
	*(volatile uint32_t *)(base + 0x00) = (opc << 27) | (((uint32_t)reg & 0xffu) << 4) | (count - 1);
	uint32_t status = 0;
	for (int i = 0; i < 4000; i++) {
		status = *(volatile uint32_t *)(base + 0x08);
		if (status & 0x1u) break;
	}
	if (!(status & 0x1u) || (status & 0xeu)) return 0;
	uint32_t d[2];
	d[0] = *(volatile uint32_t *)(base + 0x18);
	d[1] = (count > 4) ? *(volatile uint32_t *)(base + 0x1c) : 0;
	for (uint32_t i = 0; i < count; i++) {
		buf[i] = (uint8_t)(d[i / 4] >> (8 * (i % 4)));
	}
	return 1;
}

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

static int usb_menu_interrupted;
static int boot_fallback_active;
static void usb_poll_command(void);

static int
ui_boot_menu(void)
{
	efi_simple_text_input *cin = (efi_simple_text_input *)ST->con_in;
	int choice = MENU_CHOICE_IOS;
	uint64_t start_time, last_spmi_poll, freq, poll_ticks;
	int countdown = 5;
	int user_interacted = 0;
	struct spmi_btn_ctx spmi_btns;

	if (SCREEN.base == 0) {
		return MENU_CHOICE_IOS;
	}

	slot_detect();
	spmi_buttons_init(&spmi_btns);

	freq = ui_freq();
	poll_ticks = freq / 100;
	if (poll_ticks == 0) poll_ticks = 1;
	start_time = ui_now();
	last_spmi_poll = start_time;

	int mid_y = (int)SCREEN.height / 3;
	ui_fill(0, mid_y - 80, (int)SCREEN.width, 600, UI_BLACK);
	ui_draw_menu(choice, countdown, user_interacted);
	usb_menu_interrupted = 0;

	while (1) {
		usb_poll_command();
		if (usb_menu_interrupted) {
			choice = MENU_CHOICE_FASTBOOT;
			break;
		}
		if (cin != NULL) {
			efi_input_key key;
			efi_status status = cin->read_key_stroke(cin, &key);
			if (!EFI_ERROR(status)) {
				user_interacted = 1;
				if (key.scan_code == 0x0001 || key.unicode_char == '+' || key.unicode_char == 'k' || key.unicode_char == '1') {
					choice = (choice + 2) % 3;
					ui_draw_menu(choice, countdown, user_interacted);
				} else if (key.scan_code == 0x0002 || key.unicode_char == '-' || key.unicode_char == 'j' || key.unicode_char == '2') {
					choice = (choice + 1) % 3;
					ui_draw_menu(choice, countdown, user_interacted);
				} else if (key.unicode_char == '3') {
					choice = MENU_CHOICE_FASTBOOT;
					ui_draw_menu(choice, countdown, user_interacted);
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
				user_interacted = 1;
				choice = (choice + 2) % 3;
				ui_draw_menu(choice, countdown, user_interacted);
			} else if (down_edge) {
				user_interacted = 1;
				choice = (choice + 1) % 3;
				ui_draw_menu(choice, countdown, user_interacted);
			} else if (pwr_edge) {
				break;
			}
		}

		if (!user_interacted) {
			uint64_t elapsed = now - start_time;
			int sec_left = 5 - (int)(elapsed / freq);
			if (sec_left != countdown) {
				countdown = sec_left;
				if (countdown <= 0) {
					choice = MENU_CHOICE_IOS;
					break;
				}
				ui_draw_menu(choice, countdown, user_interacted);
			}
		}

		for (volatile int d = 0; d < 10000; d++) {
		}
	}

	ui_fill(0, mid_y - 80, (int)SCREEN.width, 600, UI_BLACK);
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
gpt_swap_ab_type_guids(uint8_t *entries, uint32_t num_entries, uint32_t entry_size)
{
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
				uint8_t tmp[16];
				memcpy(tmp, ei, 16);
				memcpy(ei, ej, 16);
				memcpy(ej, tmp, 16);
				swapped++;
				break;
			}
		}
	}
	return swapped;
}

static void
slot_apply_active(char target_slot)
{
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
	const char *target_abl = (target_slot == 'b') ? "abl_b" : "abl_a";
	const char *other_name = (target_slot == 'b') ? "boot_a" : "boot_b";
	uint8_t *entry = gpt_find_entry(ufs_buffer, num_entries, entry_size, target_name);
	uint8_t *abl = gpt_find_entry(ufs_buffer, num_entries, entry_size, target_abl);
	uint8_t *entry_other = gpt_find_entry(ufs_buffer, num_entries, entry_size, other_name);
	if (!entry) return;

	/* Qualcomm LinuxLoader ValidateSlotGuids requires active slot partitions to hold distinct
	 * PartitionTypeGUIDs rather than the shared inactive GUID (77036cd4-03d5-42bb-8ed1-37e5a88baa34). */
	int need_guid_swap = (abl != NULL && memcmp(entry, abl, 16) == 0);
	if (need_guid_swap) {
		uint32_t n4 = gpt_swap_ab_type_guids(ufs_buffer, num_entries, entry_size);
		print("slot: swapped %d A/B PartitionTypeGUID pairs on LUN 4 for Slot %s\n",
		    (uint64_t)n4, target_slot == 'b' ? "b" : "a");
	}

	*(uint64_t *)(entry + 48) = 0x006f000000000000ULL;
	if (entry_other) {
		*(uint64_t *)(entry_other + 48) = 0x006a000000000000ULL;
	}
	if (EFI_ERROR(safe_write_blocks(bio, entries_lba, num_blocks, bs, ufs_buffer))) return;
	gpt_sync_lun(bio);

	if (need_guid_swap) {
		efi_block_io *bio0 = lun_get_bio(0, "userdata");
		if (bio0 && bio0->media && bio0->media->block_size == bs &&
		    !EFI_ERROR(bio0->read_blocks(bio0, bio0->media->media_id, 1, bs, hdr)) &&
		    memcmp(hdr, "EFI PART", 8) == 0) {
			uint64_t elba0 = *(uint64_t *)(hdr + 72);
			uint32_t nent0 = *(uint32_t *)(hdr + 80);
			uint32_t esz0 = *(uint32_t *)(hdr + 84);
			uint64_t tbytes0 = (uint64_t)nent0 * esz0;
			uint64_t nblk0 = (tbytes0 + bs - 1) / bs;
			if (tbytes0 > 0 && tbytes0 <= sizeof(ufs_buffer) &&
			    !EFI_ERROR(safe_read_blocks(bio0, elba0, nblk0, bs, ufs_buffer))) {
				uint32_t n0 = gpt_swap_ab_type_guids(ufs_buffer, nent0, esz0);
				if (n0 > 0 && !EFI_ERROR(safe_write_blocks(bio0, elba0, nblk0, bs, ufs_buffer))) {
					gpt_sync_lun(bio0);
				}
			}
		}
	}
	print("slot: Slot %s (%s) active=0x006f000000000000 (%s=0x006a000000000000)\n",
	    target_slot == 'b' ? "b" : "a", target_name, other_name);
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

static const char *
slot_flash_boot_image(uint64_t bytes)
{
	static const char16 want_a[] = u"boot_a", want_b[] = u"boot_b";
	const char16 *want = (loader_slot == 'b') ? want_b : want_a;
	efi_block_io *bio = find_partition_named(want, 7);
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

static int recovery_pending_after_return;

static void
ui_draw_fault_banner(const char *status_line, const char *klog, uint64_t klog_len)
{
	if (!ui_on()) {
		return;
	}
	int top_y = (int)SCREEN.height - 920;
	if (top_y < 0) {
		top_y = 0;
	}
	ui_fill(0, top_y, (int)SCREEN.width, (int)SCREEN.height - top_y, 0xff180000u);
	ui_fill(0, top_y, (int)SCREEN.width, 64, 0xffcc1010u);
	const char *hdr = "KERNEL FAULT / HUNG - FASTBOOT RECOVERY";
	int hw = ui_text_width(hdr, 40, UI_AMOUNT_SCALE);
	ui_text(((int)SCREEN.width - hw) / 2, top_y + 18, hdr, UI_AMOUNT_SCALE, 40);

	int maxc = ((int)SCREEN.width - 2 * UI_LEFT) / (4 * UI_LOG_SCALE);
	int y = top_y + 82;
	ui_text(UI_LEFT, y, status_line, UI_LOG_SCALE, maxc);
	y += UI_LOG_STEP + 6;

	const char *src = (klog != NULL && klog_len > 0 && klog_len <= (16u << 20)) ? klog : log_buf;
	uint64_t slen = (src == klog) ? klog_len : log_len;
	while (slen > 0 && (src[slen - 1] == 0 || src[slen - 1] == '\n' || src[slen - 1] == '\r')) {
		slen--;
	}
	/* First show any line containing "panic" */
	for (uint64_t i = 0; i + 5 < slen && y < (int)SCREEN.height - 460; i++) {
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
			ui_text(UI_LEFT, y, src + ls, UI_LOG_SCALE, n);
			y += UI_LOG_STEP;
			i = le;
		}
	}
	/* Then show the last lines of the log */
	int rem_rows = ((int)SCREEN.height - 420 - y) / UI_LOG_STEP;
	if (rem_rows > 0 && slen > 0) {
		uint64_t start = slen;
		for (int lines = 0; start > 0 && lines < rem_rows; start--) {
			if (src[start - 1] == '\n') lines++;
		}
		for (uint64_t line = start, p; line < slen && y < (int)SCREEN.height - 420; line = p + 1) {
			for (p = line; p < slen && src[p] != '\n'; p++) {}
			int n = (int)(p - line);
			while (n > 0 && src[line + n - 1] == '\r') n--;
			if (n > maxc) n = maxc;
			ui_text(UI_LEFT, y, src + line, UI_LOG_SCALE, n);
			y += UI_LOG_STEP;
		}
	}
	if (skip_ufs_arbitration && SCREEN.base == 0xfc800000ULL) {
		uint64_t off = (uint64_t)top_y * SCREEN.stride;
		uint64_t bytes = (uint64_t)((int)SCREEN.height - top_y) * SCREEN.stride;
		memcpy((void *)(uintptr_t)(0xfd400000ULL + off), (const void *)(uintptr_t)(SCREEN.base + off), bytes);
		clean_dcache(0xfd400000ULL + off, 0xfd400000ULL + off + bytes);
	}
}

static const char *
slot_unlock_held_recovery(void)
{
	usb_menu_interrupted = 1;
	if (!skip_ufs_arbitration) {
		return "only supported on ramdisk platforms";
	}
	if (running_live != NULL) {
		((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
		other_cpu_done = 1;
		clean_dcache((uint64_t)(uintptr_t)&other_cpu_done,
		    (uint64_t)(uintptr_t)&other_cpu_done + sizeof(other_cpu_done));
		active_watch = 0;
		running_live = NULL;
		recovery_pending_after_return = 1;
		set_result("idle", 0, 0);
		return NULL;
	}
	other_cpu_done = 1;
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done,
	    (uint64_t)(uintptr_t)&other_cpu_done + sizeof(other_cpu_done));
	active_watch = 0;
	recovery_pending_after_return = 0;
	if (held_irq_masked) {
		__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (held_group1));
		__asm__ volatile ("msr daif, %0" : : "r" (held_daif));
		held_irq_masked = 0;
	}
	kernel_resources_held = 0;
	firmware_fault = 0;
	hc_take_back("recovery unlock");
	set_result("idle", 0, 0);
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
	(void)slot_unlock_held_recovery();
	boot_fallback_active = 1;
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
		print("boot fallback: no host command for 120s, switching to Android slot\n");
		slot_set_active_other();
		reboot();
	}
}



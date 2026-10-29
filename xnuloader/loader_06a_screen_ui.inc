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

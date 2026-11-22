/*
 * Sensor exposure, analog gain, and frame metadata helpers for SM8750 native camera modes:
 *   - Front sensor (sensor=2, OV20B40 mode 4, 2592x1458@30):
 *     FLL=3333, vertOffset=4, line_ns=10000, gain_unity=256, gain_max=4088, black=64 (10-bit)
 *   - Rear wide sensor (sensor=1, OVX8000/OV50E40 mode 24, 2560x1920@30):
 *     FLL=6250, vertOffset=32, line_ns=5333, gain_unity=256, gain_max=4096, black=64 (10-bit)
 */
#ifndef ANNIBALE_CAMERA_EXPOSURE_INC
#define ANNIBALE_CAMERA_EXPOSURE_INC

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define OV20B_M4_FLL_MIN        3333u
#define OV20B_M4_FLL_MAX        6666u
#define OV20B_M4_MARGIN         4u
#define OV20B_M4_LINE_NS        10000u
#define OV20B_M4_GAIN_UNITY     256u
#define OV20B_M4_GAIN_DEFAULT   1024u
#define OV20B_M4_GAIN_MAX       4088u
#define OV20B_M4_BLACK_10BIT    64u

#define OVX8000_M24_FLL_MIN     6250u
#define OVX8000_M24_FLL_MAX     12500u
#define OVX8000_M24_MARGIN      32u
#define OVX8000_M24_LINE_NS     5333u
#define OVX8000_M24_GAIN_UNITY  256u
#define OVX8000_M24_GAIN_MAX    4096u
#define OVX8000_M24_BLACK_10BIT 64u
#define OVX8000_FOCUS_DEFAULT   512u
#define OVX8000_FOCUS_MAX       1023u

static uint32_t cam_native_exposure_lines;
static uint32_t cam_native_gain;
static uint32_t cam_native_frame_lines;
static uint32_t cam_native_applied_exposure_lines;
static uint32_t cam_native_applied_gain;
static uint32_t cam_native_applied_frame_lines;
static uint32_t cam_native_focus = OVX8000_FOCUS_DEFAULT;      /* back lens position asked for */
static uint32_t cam_native_applied_focus = ANNIBALE_CAMERA_NO_FOCUS;   /* ... written; no lens yet */
/*
 * The exposure and lens position the frames show, newest first: lines, gain, frame lines, focus as
 * written before each of the last three frames. A frame's pixels follow the exposure written two
 * frames before it (measured on the back camera, docs/progress/2026-11-21-user-test-issues.md) and
 * the lens position written one frame before it: the lens moves as soon as it is written, and on
 * kernel-12377-idle4-w7 (camera_test focus) the frame after the write had the new position's
 * sharpness (within 5%) at all 96 positions, and the frame exposed during the write was in between
 * where the sharpness changes steeply. The metadata reports these, so auto
 * exposure and focus wait for a change to be in the frames before they measure again.
 */
static uint32_t cam_native_shown[3][4];

static bool
cam_native_is_ov20b_m4(const struct annibale_camera_profile *p)
{
	return p != NULL && p->sensor == 2u && p->mode == 4u &&
	       p->width == 2592u && p->height == 1458u;
}

static bool
cam_native_is_ovx8000_m24(const struct annibale_camera_profile *p)
{
	return p != NULL && p->sensor == 1u && p->mode == 24u &&
	       p->width == 2560u && p->height == 1920u;
}

static void
cam_native_exposure_invalidate(void)
{
	cam_native_applied_exposure_lines = 0;
	cam_native_applied_gain = 0;
	cam_native_applied_frame_lines = 0;
}

/* After each frame's register writes; `restart` when the stream starts with them. */
static void
cam_native_exposure_shown(bool restart)
{
	const uint32_t now[4] = {
		cam_native_applied_exposure_lines, cam_native_applied_gain, cam_native_applied_frame_lines,
		cam_native_applied_focus,
	};

	for (uint32_t i = 2; i > 0; i--) {
		memcpy(cam_native_shown[i], restart ? now : cam_native_shown[i - 1], sizeof(now));
	}
	memcpy(cam_native_shown[0], now, sizeof(now));
}

static void
cam_native_exposure_reset(void)
{
	cam_native_exposure_lines = 0;
	cam_native_gain = 0;
	cam_native_frame_lines = 0;
	cam_native_exposure_invalidate();
}

static void
cam_native_exposure_defaults(const struct annibale_camera_profile *p)
{
	if (cam_native_is_ov20b_m4(p)) {
		cam_native_frame_lines = OV20B_M4_FLL_MIN;
		cam_native_exposure_lines = OV20B_M4_FLL_MIN - OV20B_M4_MARGIN;
		cam_native_gain = OV20B_M4_GAIN_DEFAULT;
	} else if (cam_native_is_ovx8000_m24(p)) {
		cam_native_frame_lines = OVX8000_M24_FLL_MIN;
		cam_native_exposure_lines = OVX8000_M24_FLL_MIN - OVX8000_M24_MARGIN;
		cam_native_gain = OVX8000_M24_GAIN_UNITY;
	} else {
		cam_native_exposure_reset();
	}
}

static bool
cam_native_exposure_set(const struct annibale_camera_profile *p,
    uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	if (lines == 0 || gain == 0) {
		return false;
	}
	if (cam_native_is_ov20b_m4(p)) {
		uint32_t fll = frame_lines >= OV20B_M4_FLL_MIN ?
		    MIN(frame_lines, OV20B_M4_FLL_MAX) : OV20B_M4_FLL_MIN;
		uint32_t max_lines = fll - OV20B_M4_MARGIN;
		cam_native_frame_lines = fll;
		cam_native_exposure_lines = MIN(MAX(lines, 4u), max_lines);
		cam_native_gain = MIN(MAX(gain, OV20B_M4_GAIN_UNITY), OV20B_M4_GAIN_MAX);
		return true;
	}
	if (cam_native_is_ovx8000_m24(p)) {
		uint32_t fll = frame_lines >= OVX8000_M24_FLL_MIN ?
		    MIN(frame_lines, OVX8000_M24_FLL_MAX) : OVX8000_M24_FLL_MIN;
		uint32_t max_lines = fll - OVX8000_M24_MARGIN;
		cam_native_frame_lines = fll;
		cam_native_exposure_lines = MIN(MAX(lines, 8u), max_lines);
		cam_native_gain = MIN(MAX(gain, OVX8000_M24_GAIN_UNITY), OVX8000_M24_GAIN_MAX);
		return true;
	}
	return false;
}

/* Back camera only: the front one is fixed focus (its module has no actuator). */
static bool
cam_native_focus_set(const struct annibale_camera_profile *p, uint32_t position)
{
	if (!cam_native_is_ovx8000_m24(p)) {
		return false;
	}
	cam_native_focus = MIN(position, OVX8000_FOCUS_MAX);
	return true;
}

static void
cam_native_frame_exposure(const struct annibale_camera_profile *p,
    struct annibale_camera_frame *f)
{
	uint32_t lines = cam_native_shown[2][0], gain = cam_native_shown[2][1], frame_lines = cam_native_shown[2][2];

	if (f == NULL || lines == 0 || gain == 0) {
		return;
	}
	if (cam_native_is_ov20b_m4(p)) {
		f->black = OV20B_M4_BLACK_10BIT;
		f->exposure_lines = lines;
		f->line_ns = OV20B_M4_LINE_NS;
		f->gain = gain;
		f->gain_unity = OV20B_M4_GAIN_UNITY;
		f->gain_max = OV20B_M4_GAIN_MAX;
		f->frame_lines = frame_lines;
		f->frame_lines_min = OV20B_M4_FLL_MIN;
		f->frame_lines_max = OV20B_M4_FLL_MAX;
		f->margin = OV20B_M4_MARGIN;
	} else if (cam_native_is_ovx8000_m24(p)) {
		f->black = OVX8000_M24_BLACK_10BIT;
		f->exposure_lines = lines;
		f->line_ns = OVX8000_M24_LINE_NS;
		f->gain = gain;
		f->gain_unity = OVX8000_M24_GAIN_UNITY;
		f->gain_max = OVX8000_M24_GAIN_MAX;
		f->frame_lines = frame_lines;
		f->frame_lines_min = OVX8000_M24_FLL_MIN;
		f->frame_lines_max = OVX8000_M24_FLL_MAX;
		f->margin = OVX8000_M24_MARGIN;
		f->focus = cam_native_shown[1][3];
	}
}

#endif /* ANNIBALE_CAMERA_EXPOSURE_INC */

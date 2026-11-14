/*
 * Sensor exposure, analog gain, and frame metadata helpers for SM8750 native camera modes:
 *   - Front sensor (sensor=2, OV20B40 mode 4, 2592x1458@30):
 *     FLL=3333, vertOffset=4, line_ns=10000, gain_unity=256, gain_max=4088, black=64 (10-bit)
 *   - Rear wide sensor (sensor=1, OVX8000/OV50E40 mode 24, 4096x3072@30):
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

static uint32_t cam_native_exposure_lines;
static uint32_t cam_native_gain;
static uint32_t cam_native_frame_lines;

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
	       p->width == 4096u && p->height == 3072u;
}

static void
cam_native_exposure_reset(void)
{
	cam_native_exposure_lines = 0;
	cam_native_gain = 0;
	cam_native_frame_lines = 0;
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

static void
cam_native_frame_exposure(const struct annibale_camera_profile *p,
    struct annibale_camera_frame *f)
{
	if (f == NULL || cam_native_exposure_lines == 0 || cam_native_gain == 0) {
		return;
	}
	if (cam_native_is_ov20b_m4(p)) {
		f->black = OV20B_M4_BLACK_10BIT;
		f->exposure_lines = cam_native_exposure_lines;
		f->line_ns = OV20B_M4_LINE_NS;
		f->gain = cam_native_gain;
		f->gain_unity = OV20B_M4_GAIN_UNITY;
		f->gain_max = OV20B_M4_GAIN_MAX;
		f->frame_lines = cam_native_frame_lines;
		f->frame_lines_min = OV20B_M4_FLL_MIN;
		f->frame_lines_max = OV20B_M4_FLL_MAX;
		f->margin = OV20B_M4_MARGIN;
	} else if (cam_native_is_ovx8000_m24(p)) {
		f->black = OVX8000_M24_BLACK_10BIT;
		f->exposure_lines = cam_native_exposure_lines;
		f->line_ns = OVX8000_M24_LINE_NS;
		f->gain = cam_native_gain;
		f->gain_unity = OVX8000_M24_GAIN_UNITY;
		f->gain_max = OVX8000_M24_GAIN_MAX;
		f->frame_lines = cam_native_frame_lines;
		f->frame_lines_min = OVX8000_M24_FLL_MIN;
		f->frame_lines_max = OVX8000_M24_FLL_MAX;
		f->margin = OVX8000_M24_MARGIN;
	}
}

#endif /* ANNIBALE_CAMERA_EXPOSURE_INC */

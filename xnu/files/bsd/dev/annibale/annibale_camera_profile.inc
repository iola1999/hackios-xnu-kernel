/* RWCTAB03 layout and the complete front mode 4 exported by tools/camsensor_profile.py. */
#define CAM_NATIVE_PROFILE_BYTES 6440u

static struct annibale_camera_profile *cam_profile;

_Static_assert(sizeof(struct annibale_camera_profile) == 128, "RWCTAB03 header");
_Static_assert(sizeof(struct annibale_camera_power) == 12, "RWCTAB03 power entry");
_Static_assert(sizeof(struct annibale_camera_register) == 12, "RWCTAB03 register entry");

static int
cam_profile_validate(const void *data, size_t bytes)
{
	static const uint8_t expected[SHA256_DIGEST_LENGTH] = {
		0x9a, 0xb0, 0xef, 0xeb, 0x01, 0xb3, 0xfe, 0x86,
		0x82, 0x2f, 0x8d, 0x5e, 0x4a, 0x18, 0xea, 0xf9,
		0x15, 0x67, 0xfd, 0xc9, 0x45, 0xc6, 0x40, 0x91,
		0xbd, 0x44, 0x3b, 0x0a, 0x52, 0x21, 0xe4, 0x5e,
	};
	const struct annibale_camera_profile *profile = data;
	uint8_t digest[SHA256_DIGEST_LENGTH];
	SHA256_CTX context;

	if (data == NULL || bytes != CAM_NATIVE_PROFILE_BYTES || profile->bytes != bytes ||
	    memcmp(profile->magic, ANNIBALE_CAMERA_PROFILE_MAGIC, sizeof(profile->magic)) != 0) {
		return EINVAL;
	}
	/* Includes mode geometry, all operations/delays, power sequences and the source-module hash. */
	SHA256_Init(&context);
	SHA256_Update(&context, data, bytes);
	SHA256_Final(digest, &context);
	return memcmp(digest, expected, sizeof(digest)) == 0 ? 0 : EINVAL;
}

static const struct annibale_camera_power *
cam_profile_power(bool up)
{
	const struct annibale_camera_power *power = (const void *)(cam_profile + 1);
	return power + (up ? 0 : cam_profile->up_count);
}

static const struct annibale_camera_register *
cam_profile_registers(uint32_t table)
{
	const struct annibale_camera_register *registers = (const void *)(cam_profile_power(false) +
	    cam_profile->down_count);
	if (table > 0) {
		registers += cam_profile->init_count;
	}
	if (table > 1) {
		registers += cam_profile->mode_count;
	}
	if (table > 2) {
		registers += cam_profile->on_count;
	}
	return registers;
}

static bool
cam_capture_resources_held(void)
{
	return cam_capture.state == ANNIBALE_CAM_CAPTURE_RUNNING ||
	       cam_capture.state == ANNIBALE_CAM_CAPTURE_HELD;
}

/* The caller holds cam_lock throughout installation and frame copying. */
static void
cam_profile_install(struct annibale_camera_profile *profile)
{
	if (cam_profile != NULL) {
		kfree_data(cam_profile, CAM_NATIVE_PROFILE_BYTES);
	}
	cam_profile = profile;
	current_sensor = profile->sensor;
	cam_hw_dma_verified = false;
	bzero(&cam_capture, sizeof(cam_capture));
	cam_capture.state = ANNIBALE_CAM_CAPTURE_READY;
	cam_capture.width = profile->width;
	cam_capture.height = profile->height;
	cam_capture.stride = profile->stride;
	cam_capture.bytes = profile->stride * profile->height;
}

static int
cam_native_read(struct uio *uio)
{
	struct annibale_camera_frame frame;
	if (cam_capture.state != ANNIBALE_CAM_CAPTURE_STOPPED || !cam_hw_dma_verified ||
	    (cam_capture.flags & ANNIBALE_CAM_CAPTURE_COMPLETE) != ANNIBALE_CAM_CAPTURE_COMPLETE) {
		return cam_capture.error != 0 ? (int)cam_capture.error : EAGAIN;
	}
	if (uio_resid(uio) < (user_ssize_t)sizeof(frame)) {
		return EINVAL;
	}
	bzero(&frame, sizeof(frame));
	memcpy(frame.magic, ANNIBALE_CAMERA_MAGIC, sizeof(frame.magic));
	frame.header_bytes = sizeof(frame);
	frame.width = cam_profile->width;
	frame.height = cam_profile->height;
	frame.stride = cam_profile->stride;
	frame.format = 1;
	frame.bayer = cam_profile->bayer;
	frame.sequence = cam_capture.sequence;
	frame.time = cam_capture.time;
	frame.frame_lines = cam_profile->frame_length;
	frame.sensor = cam_profile->sensor;
	frame.focus = ANNIBALE_CAMERA_NO_FOCUS;
	int error = uiomove((caddr_t)&frame, sizeof(frame), uio);
	user_ssize_t bytes = MIN(uio_resid(uio), (user_ssize_t)cam_capture.bytes);
	if (error == 0 && bytes > 0) {
		error = uiomove((caddr_t)cam_region_vbase, (int)bytes, uio);
	}
	return error;
}

/* Check volume through the public guard API without hardware access. */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/annibale_audio.h>

#define FRAMES 960

static void
check_amplitude(bool pcm)
{
	const uint32_t levels[] = { 0, 23 * 8, pcm ? 90 * 8 : ANNIBALE_GUARD_SILENT };
	const int16_t values[] = { -4096, -1024, -1, 0, 1, 1024, 4096 };
	int16_t frames[2 * FRAMES];
	annibale_guard_configure(false, 6 * 8);
	if (pcm) {
		annibale_guard_pcm_volume();
	}
	for (unsigned level = 0; level < sizeof(levels) / sizeof(levels[0]); level++) {
		annibale_guard_attenuation(levels[level]);
		for (unsigned i = 0; i < 2 * FRAMES; i++) {
			frames[i] = values[i % (sizeof(values) / sizeof(values[0]))];
		}
		annibale_guard_process(frames, FRAMES);
		double gain = pow(10.0, (6.0 - (pcm ? levels[level] / 8.0 : 0.0)) / 20.0);
		for (unsigned i = 0; i < 2 * FRAMES; i++) {
			int expected = (int)lrint(values[i % (sizeof(values) / sizeof(values[0]))] * gain);
			/* Fixed-point stages can change the final integer by one sample unit. */
			assert(abs(frames[i] - expected) <= 1);
		}
	}
}

static void
check_silence(void)
{
	int16_t frames[2 * FRAMES] = { 0 };
	struct annibale_guard_report report;
	annibale_guard_configure(true, 6 * 8);
	annibale_guard_attenuation(0);
	frames[2 * (FRAMES - 1)] = 1024;
	frames[2 * (FRAMES - 1) + 1] = -1024;
	annibale_guard_process(frames, FRAMES);
	assert(frames[2 * (FRAMES - 1)] != 0);
	annibale_guard_take_report(&report);
	annibale_guard_attenuation(ANNIBALE_GUARD_SILENT);
	for (unsigned block = 0; block < 2; block++) {
		for (unsigned i = 0; i < 2 * FRAMES; i++) {
			frames[i] = i & 1 ? -1234 : 1234;
		}
		annibale_guard_process(frames, FRAMES);
		for (unsigned i = 0; i < 2 * FRAMES; i++) {
			assert(frames[i] == 0);
		}
	}
	annibale_guard_take_report(&report);
	assert(report.peak_out == 0);
}

int
main(int argc, char **argv)
{
	assert(argc == 2);
	bool pcm = strcmp(argv[1], "pcm") == 0;
	assert(pcm || strcmp(argv[1], "hardware") == 0);
	check_amplitude(pcm);
	if (pcm) {
		check_silence();
	}
	puts(pcm ? "PASS: PCM attenuation once and silence with filter history" :
	    "PASS: existing hardware attenuation model preserves PCM amplitude");
	return 0;
}

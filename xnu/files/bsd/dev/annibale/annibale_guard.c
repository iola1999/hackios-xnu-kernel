/*
 * Stereo PCM protection for the speaker feeder. A second-order Butterworth high-pass filter
 * at 200 Hz precedes three power estimates (0.2, 3 and 60 seconds) and the peak limiter.
 * The monitor can request an additional reduction. Configurable gain is limited to 12 dB.
 *
 * Limits use a full-scale signal at the configured amplifier gain as their reference. TFA986x
 * user volume is applied to PCM before the filter and power estimates; its hardware gain stays
 * at the vendor profile. The other amplifier path retains its hardware attenuation model. The
 * reference has no measured conversion to acoustic output or watts. Arithmetic uses Q30
 * fractions unless stated otherwise.
 *
 * The feeder is the sole sample-processing caller. Volume and monitor updates can come from
 * another thread. The amplifier selects PCM volume before the feeder starts.
 */
#include <stdbool.h>
#include <stdint.h>
#include <sys/annibale_audio.h>

#define ONE                     (1ll << 30)
#define FULL_SCALE              32768

/* The limits, in eighths of a dB under full scale at the configured amplifier gain (powers: under a full-scale square wave's) */
#define PEAK_LIMIT              (3 * 8)
#define RMS_LIMIT               (6 * 8)         /* a full-scale sine is 3 dB under a square wave: these are 3, 6, 9 dB under it */
#define FAST_LIMIT              (9 * 8)
#define SLOW_LIMIT              (12 * 8)
/* ... and the averages' time constants, in frames (48 kHz) */
#define RMS_FRAMES              9600            /* 0.2 s */
#define FAST_FRAMES             144000          /* 3 s */
#define SLOW_FRAMES             2880000         /* 60 s */
#define RELEASE_SHIFT           11              /* the peak limiter lets go with a time constant of 2048 frames, 43 ms */
#define MAX_FRAMES              1024            /* the most frames processed at once (the feeder's period: 960) */

/* 10^(-n/160), n = 0..47, in Q30: an attenuation of n eighths of a dB, as a factor of amplitude */
static const uint32_t eighths[48] = {
	1073741824, 1058400094, 1043277569, 1028371116, 1013677647, 999194121, 984917536, 970844937, 956973408, 943300077,
	929822112, 916536722, 903441154, 890532697, 877808678, 865266461, 852903448, 840717079, 828704830, 816864213,
	805192776, 793688102, 782347808, 771169545, 760150998, 749289885, 738583957, 728030996, 717628817, 707375265,
	697268218, 687305580, 677485290, 667805313, 658263644, 648858308, 639587356, 630448869, 621440953, 612561743,
	603809400, 595182112, 586678091, 578295576, 570032831, 561888146, 553859832, 545946228,
};
#define SIX_DB                  538145694       /* 10^(-6/20) in Q30 */

/* the high-pass filter, 200 Hz at 48 kHz, Q = 1/sqrt(2): b0, b1, b2, a1, a2 in Q30 (RBJ's cookbook) */
static const int64_t hpf_b0 = 1054047540, hpf_b1 = -2108095079, hpf_b2 = 1054047540;
static const int64_t hpf_a1 = -2107733822, hpf_a2 = 1034714513;

static struct {
	bool hpf;
	bool pcm_volume;                       /* selected before the feeder starts for TFA986x */
	uint64_t gain;                          /* the gain before everything, Q30 (1 to 4) */
	volatile uint32_t attenuation;          /* requested volume, eighths of a dB; ANNIBALE_GUARD_SILENT: off */
	volatile uint32_t monitor;              /* the monitor's reduction, eighths of a dB */
	int64_t x1[2], x2[2], y1[2], y2[2];     /* the filter's past, in 1/256 of a sample */
	uint64_t rms[2], fast[2], slow[2];      /* average powers at each speaker, Q30 of a full-scale square wave at the configured amplifier gain */
	uint64_t level;                         /* the power guard's and the monitor's gain at the end of the last block */
	uint64_t envelope;                      /* the peak limiter's, 1/65536 of a sample */
	struct annibale_guard_report report;
} guard = { .hpf = true, .attenuation = ANNIBALE_GUARD_SILENT, .gain = ONE, .level = ONE, .report = { .least_gain = ONE, .least_peak_gain = 65536 } };

static int32_t work[2 * MAX_FRAMES];

/* 10^(-n/160) in Q30: n eighths of a dB under, as a factor of amplitude */
static uint64_t
amplitude(uint32_t n)
{
	uint64_t value = eighths[n % 48];

	for (uint32_t i = 0; i < n / 48 && value != 0; i++) {
		value = value * SIX_DB >> 30;
	}
	return value;
}

static uint64_t
isqrt(uint64_t value)
{
	uint64_t root = 0, bit = 1ull << 62;

	while (bit > value) {
		bit >>= 2;
	}
	while (bit != 0) {
		if (value >= root + bit) {
			value -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	return root;
}

/* how much of `limit' a power is, as the square of the gain that would bring it down to it (Q30, at most 1) */
static uint64_t
headroom(uint64_t power, uint64_t limit)
{
	if (power <= limit) {
		return ONE;
	}
	return (limit << 30) / power;
}

void
annibale_guard_configure(bool hpf, uint32_t gain)
{
	guard.hpf = hpf;
	/* 10^(gain/160): one over the attenuation by as much */
	guard.gain = (1ull << 60) / amplitude(gain > ANNIBALE_GUARD_MAX_GAIN ? ANNIBALE_GUARD_MAX_GAIN : gain);
}

void
annibale_guard_pcm_volume(void)
{
	guard.pcm_volume = true;
}

void
annibale_guard_attenuation(uint32_t below)
{
	guard.attenuation = below;
}

void
annibale_guard_monitor(uint32_t below)
{
	guard.monitor = below;
}

static void
block(int16_t *frames, uint32_t count)
{
	uint32_t attenuation = guard.attenuation;
	uint32_t hardware_attenuation = guard.pcm_volume ? 0 : attenuation;
	bool pcm_muted = guard.pcm_volume && attenuation == ANNIBALE_GUARD_SILENT;
	uint64_t input_gain = guard.gain;
	if (guard.pcm_volume) {
		input_gain = pcm_muted ? 0 : input_gain * amplitude(attenuation) >> 30;
	}
	struct annibale_guard_report *r = &guard.report;
	uint64_t to_speaker = hardware_attenuation == ANNIBALE_GUARD_SILENT ? 0 : amplitude(2 * hardware_attenuation);
	uint64_t sum[2] = { 0, 0 }, g2 = ONE, g, start = guard.level, ceiling;
	uint32_t binding = 0;

	/* the high-pass filter, and what comes out of it: its power, per channel */
	for (uint32_t i = 0; i < 2 * count; i++) {
		unsigned c = i & 1;
		int64_t x = (int64_t)frames[i] * (int64_t)input_gain >> 22, y;    /* (the gain, and 1/256 of a sample) */
		uint32_t level = frames[i] < 0 ? (uint32_t)-frames[i] : (uint32_t)frames[i];

		if (level > r->peak_in) {
			r->peak_in = level;
		}
		if (guard.hpf) {
			y = (hpf_b0 * x + hpf_b1 * guard.x1[c] + hpf_b2 * guard.x2[c] - hpf_a1 * guard.y1[c] - hpf_a2 * guard.y2[c]) >> 30;
			guard.x2[c] = guard.x1[c];
			guard.x1[c] = x;
			guard.y2[c] = guard.y1[c];
			guard.y1[c] = y;
		} else {
			y = x;
		}
		work[i] = (int32_t)((y + 128) >> 8);
		sum[c] += (uint64_t)((int64_t)work[i] * work[i]);
	}

	/* the averages at the speakers, and the gain that keeps all three under their limits */
	for (unsigned c = 0; c < 2; c++) {
		/* the block's power, Q30 of full scale (a square wave at 32768), then as it reaches the speaker */
		uint64_t power = (uint64_t)((unsigned __int128)(sum[c] / count) * to_speaker >> 30), h;

		guard.rms[c] = (uint64_t)((int64_t)guard.rms[c] + ((int64_t)power - (int64_t)guard.rms[c]) * count / RMS_FRAMES);
		guard.fast[c] = (uint64_t)((int64_t)guard.fast[c] + ((int64_t)power - (int64_t)guard.fast[c]) * count / FAST_FRAMES);
		guard.slow[c] = (uint64_t)((int64_t)guard.slow[c] + ((int64_t)power - (int64_t)guard.slow[c]) * count / SLOW_FRAMES);
		if ((h = headroom(guard.rms[c], amplitude(2 * RMS_LIMIT))) < g2) {
			g2 = h;
			binding = 1;
		}
		if ((h = headroom(guard.fast[c], amplitude(2 * FAST_LIMIT))) < g2) {
			g2 = h;
			binding = 2;
		}
		if ((h = headroom(guard.slow[c], amplitude(2 * SLOW_LIMIT))) < g2) {
			g2 = h;
			binding = 3;
		}
	}
	g = isqrt(g2 << 30) * amplitude(guard.monitor) >> 30;
	if (g > ONE) {
		g = ONE;
	}
	r->blocks++;
	if (binding == 1) {
		r->rms_blocks++;
	} else if (binding == 2) {
		r->fast_blocks++;
	} else if (binding == 3) {
		r->slow_blocks++;
	}
	if (g < r->least_gain) {
		r->least_gain = g;
	}

	/* the peaks: no sample over the limit at the speaker (and none over full scale) */
	ceiling = FULL_SCALE - 1;
	if (hardware_attenuation != ANNIBALE_GUARD_SILENT && hardware_attenuation < PEAK_LIMIT) {
		ceiling = (uint64_t)FULL_SCALE * amplitude(PEAK_LIMIT - hardware_attenuation) >> 30;
	}
	for (uint32_t f = 0; f < count; f++) {
		/* (the power guard's gain moves from the last block's to this one's across the block) */
		uint64_t gain = (uint64_t)((int64_t)start + ((int64_t)g - (int64_t)start) * (int64_t)(f + 1) / (int64_t)count);
		int64_t y[2];
		uint64_t level, peak_gain = 65536;

		for (unsigned c = 0; c < 2; c++) {
			y[c] = (int64_t)work[2 * f + c] * (int64_t)gain >> 30;
		}
		level = (uint64_t)(y[0] < 0 ? -y[0] : y[0]);
		if ((uint64_t)(y[1] < 0 ? -y[1] : y[1]) > level) {
			level = (uint64_t)(y[1] < 0 ? -y[1] : y[1]);
		}
		guard.envelope -= guard.envelope >> RELEASE_SHIFT;
		if (level << 16 > guard.envelope) {
			guard.envelope = level << 16;
		}
		if (guard.envelope > ceiling << 16) {
			peak_gain = (ceiling << 32) / guard.envelope;
			r->limited_frames++;
			if (peak_gain < r->least_peak_gain) {
				r->least_peak_gain = (uint32_t)peak_gain;
			}
		}
		for (unsigned c = 0; c < 2; c++) {
			int64_t out = pcm_muted ? 0 : y[c] * (int64_t)peak_gain >> 16;
			uint32_t magnitude;

			out = out > FULL_SCALE - 1 ? FULL_SCALE - 1 : out < -FULL_SCALE ? -FULL_SCALE : out;
			frames[2 * f + c] = (int16_t)out;
			magnitude = (uint32_t)(out < 0 ? -out : out);
			if (magnitude > r->peak_out) {
				r->peak_out = magnitude;
			}
		}
	}
	guard.level = g;
	r->gain = g;
	r->rms = guard.rms[0] > guard.rms[1] ? guard.rms[0] : guard.rms[1];
	r->fast = guard.fast[0] > guard.fast[1] ? guard.fast[0] : guard.fast[1];
	r->slow = guard.slow[0] > guard.slow[1] ? guard.slow[0] : guard.slow[1];
}

void
annibale_guard_process(int16_t *frames, uint32_t count)
{
	while (count > 0) {
		uint32_t n = count < MAX_FRAMES ? count : MAX_FRAMES;

		block(frames, n);
		frames += 2 * n;
		count -= n;
	}
}

/* What has happened since the last call (and the averages and the gain as they are now). */
void
annibale_guard_take_report(struct annibale_guard_report *report)
{
	*report = guard.report;
	guard.report = (struct annibale_guard_report){ .least_gain = ONE, .least_peak_gain = 65536 };
}

/* 10 log10 of a Q30 ratio, in tenths of a dB (for what is said) */
int
annibale_guard_decibels(uint64_t ratio)
{
	static const uint16_t fraction[17] = {
		0, 5732, 11136, 16248, 21098, 25711, 30109, 34312, 38336, 42196, 45904, 49472, 52911, 56229, 59434, 62534, 65535,
	};
	int64_t log2;                           /* Q16 */
	unsigned top = 63;

	if (ratio == 0) {
		return -999;
	}
	while (!(ratio >> top)) {
		top--;
	}
	{
		/* the 4 bits under the top one choose a segment, the next 12 where in it */
		uint64_t mantissa = top >= 16 ? ratio >> (top - 16) : ratio << (16 - top);
		unsigned index = (unsigned)(mantissa >> 12) & 15, within = (unsigned)mantissa & 4095;

		log2 = ((int64_t)top - 30) * 65536 + fraction[index] + (int64_t)(fraction[index + 1] - fraction[index]) * within / 4096;
	}
	return (int)(log2 * 30103 / 65536 / 1000);
}

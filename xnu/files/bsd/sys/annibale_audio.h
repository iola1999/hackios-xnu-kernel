/*
 * /dev/annibale-audio: the sound iOS plays, on its way to whatever can make it heard.
 *
 * iOS expects its output devices from Apple's audio drivers through IOAudio2; neither exists
 * here. Instead the audio server (mediaserverd) is given a plug-in of ours that presents one
 * stereo output device, and the plug-in writes what the server has mixed for that device
 * here. In the kernel the samples go to a sink -- a driver for something that plays them --
 * and, if asked for, into a recording that outlives the kernel (see dev/annibale_audio.c).
 *
 *   write(): whole frames in the format below. Never blocks: the writer is the audio server's
 *            real-time thread, which paces itself (48000 frames a second while anything
 *            plays, nothing at all otherwise). What the sink cannot take is dropped.
 */
#ifndef _SYS_ANNIBALE_AUDIO_H_
#define _SYS_ANNIBALE_AUDIO_H_

#include <stdint.h>

#define ANNIBALE_AUDIO_DEVICE    "/dev/annibale-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define ANNIBALE_AUDIO_RATE      48000
#define ANNIBALE_AUDIO_CHANNELS  2
#define ANNIBALE_AUDIO_FRAME     4       /* bytes */

/*
 * ioctl(ANNIBALE_AUDIO_SET_VOLUME): the speakers' volume, as iOS sets it on the plug-in's volume
 * control -- how many eighths of a dB under the loudest they may play, and whether they are
 * muted (iOS's volume at 0). The loudest is the kernel's to say; the plug-in only says how far
 * under it.
 */
#include <sys/ioccom.h>

struct annibale_audio_volume {
	uint32_t below;                 /* eighths of a dB */
	uint32_t muted;
};
#define ANNIBALE_AUDIO_SET_VOLUME _IOW('r', 1, struct annibale_audio_volume)

#ifdef KERNEL
#include <stdbool.h>

/*
 * For drivers. A sink is offered frames as they are written and says how many it took (the
 * rest are lost); it is called from the writer's thread and must not wait.
 */
typedef uint32_t (*annibale_audio_sink_t)(const int16_t *frames, uint32_t count);

void annibale_audio_set_sink(const char *name, annibale_audio_sink_t sink);
/* The volume iOS has set last (ANNIBALE_AUDIO_SET_VOLUME): false if it has set none; *generation counts the settings. */
bool annibale_audio_volume(uint32_t *below, bool *muted, uint32_t *generation);
void annibale_audio_init(void);
void annibale_virtio_sound_init(void);   /* boot-arg virtio_sound=1: QEMU's virtio sound device */
void annibale_haptic_init(void);        /* boot-arg hv_haptic=N: annibale's vibration motor, the PMIC's haptics block (bsd/dev/annibale_haptic.c) */
void annibale_haptic_quiesce(void);     /* its driver module disabled: at the run's end and on a panic's way out */
void annibale_buttons_init(void);       /* boot-arg buttons=1: annibale's keys, the PMIC's (bsd/dev/annibale_buttons.c) */
void annibale_reboot_probe(void);       /* Explicit LK target preparation; enabled path returns to the loader. */
void annibale_battery_init(void);       /* boot-arg battery=1: annibale's BQ27Z561 battery gauge (bsd/dev/annibale_battery.c) */
void annibale_torch_init(void);         /* boot-arg torch_secs=N: annibale's flashlight, the PMIC's flash module (bsd/dev/annibale_torch.c) */
void annibale_torch_quiesce(void);      /* its light off: at the run's end and on a panic's way out */
bool annibale_torch_low_on(uint32_t milliamps);   /* the camera's (task L1b): the torch lit, up to the device tree's maximum */
void annibale_torch_low_off(void);      /* ... and put out */
void adreno8_init(void);                /* boot-arg adreno8=N (1..23): annibale's Adreno 830, step by step (bsd/dev/adreno8.c) */
void adreno8_quiesce(void);             /* its GMU's processor back in reset: at the run's end and on a panic's way out */
void mhi_pcie_init(void);               /* boot-arg pcie=N (1..4): the Wi-Fi card on PCIe root complex 0, survey (bsd/dev/mhi_pcie.c) */
void annibale_outbox_init(void);         /* boot-arg outbox_mb=N: memory in one piece for kern.annibale.outbox (bsd/dev/annibale_threads.c) */
void annibale_spmi_lock(void);          /* the annibale drivers' one transfer at a time through the v7 arbiter (annibale_buttons.c) */
void annibale_spmi_unlock(void);

/*
 * The speakers' guard (bsd/dev/annibale_guard.c): what the samples go through in the feeder on their
 * way to the amplifiers, for want of the DSP's speaker protection. Levels in eighths of a dB.
 */
#define ANNIBALE_GUARD_SILENT    0xffffffffu     /* the amplifiers are off: nothing reaches the speakers */
struct annibale_guard_report {
	uint32_t blocks, rms_blocks, fast_blocks, slow_blocks;  /* blocks done, and in how many each average held the level down */
	uint32_t limited_frames;        /* frames in which the peak limiter turned the level down */
	uint32_t peak_in, peak_out;     /* the largest sample that came in and that went out */
	uint32_t least_peak_gain;       /* the peak limiter's least gain, in 1/65536 */
	uint64_t least_gain, gain;      /* the power guard's (with the monitor's) least gain and the latest, Q30 */
	uint64_t rms, fast, slow;       /* the averages now, the louder speaker's: Q30 of a full-scale square wave at the configured amplifier gain */
};
#define ANNIBALE_GUARD_MAX_GAIN  (12 * 8)        /* the most gain before the guard: 12 dB */
void annibale_guard_configure(bool hpf, uint32_t gain);         /* the high-pass filter; `gain' before everything */
/* Select PCM volume for a fixed amplifier profile before starting the feeder. */
void annibale_guard_pcm_volume(void);
/* Requested attenuation in eighths of a dB, or ANNIBALE_GUARD_SILENT. */
void annibale_guard_attenuation(uint32_t below);
void annibale_guard_process(int16_t *frames, uint32_t count);    /* stereo frames, in place */
void annibale_guard_monitor(uint32_t below);                     /* a further reduction, for the amplifiers' own protection */
void annibale_guard_take_report(struct annibale_guard_report *report);   /* what happened since the last call */
int annibale_guard_decibels(uint64_t ratio);                     /* 10 log10 of a Q30 ratio, in tenths of a dB */
/* boot-arg rec_sound_fed=1: the recording (rec_sound=N) has what the guard let through instead of what iOS wrote */
bool annibale_audio_recording_fed(void);
void annibale_audio_record_fed(const int16_t *frames, uint32_t count);

/* boot-arg rec_sound_mic=1: the recording (rec_sound=N) has what the microphone hears (given with annibale_audio_record_fed()) */
bool annibale_audio_recording_mic(void);
#endif

#endif /* _SYS_ANNIBALE_AUDIO_H_ */

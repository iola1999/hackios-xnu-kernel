/*
 * /dev/annibale-touch: touch samples on their way from whoever produces them to the
 * program that turns them into HID events for the window server.
 *
 * iOS expects touches from Apple's multitouch driver through IOHIDFamily; neither exists
 * here. Instead a producer -- a kernel driver for a real touch controller or for an emulated
 * pointer, or a user program replaying a script -- posts samples, and code of ours running
 * inside backboardd reads them and synthesizes the events. Both sides use the same record.
 *
 *   write(): one or more records (time is ignored; the kernel stamps them)
 *   read():  blocks until there is at least one record, returns as many whole ones as fit
 *
 * The queue holds TOUCH_QUEUE records and, when it is full, the write throws the oldest one away
 * and succeeds. So write() cannot report "nobody is reading": a producer whose reader has gone
 * still gets the bytes accepted, and the samples are gone without an error anywhere. Reading
 * that back from the log means comparing what the producer sent with what `tl: p` shows the
 * driver posting -- 2026-10-28 two conclusions about lost input were drawn from a silent write()
 * that had in fact succeeded. A producer that wants to know whether anyone listens has to ask by
 * another route; nothing in this interface says it.
 */
#ifndef _SYS_ANNIBALE_INPUT_H_
#define _SYS_ANNIBALE_INPUT_H_

#include <stdbool.h>
#include <stdint.h>

#define ANNIBALE_TOUCH_DEVICE    "/dev/annibale-touch"

/* Frames put on the panel since boot, kept by the framebuffer and read as kern.annibale.frames.
 * A control run needs it: when a daemon is frozen to see whether the compositor stalls, "no new
 * frame" has to be told from "the counter was never moving", and only the number can say which. */
extern uint32_t annibale_frames_shown;

#define ANNIBALE_TOUCH_DOWN      1       /* a finger has landed */
#define ANNIBALE_TOUCH_MOVE      2
#define ANNIBALE_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */
#define ANNIBALE_BUTTON_DOWN     4       /* a hardware button; `finger` says which (ANNIBALE_BUTTON_*), x and y are 0 */
#define ANNIBALE_BUTTON_UP       5

#define ANNIBALE_BUTTON_LOCK     1       /* the side button */
#define ANNIBALE_BUTTON_VOLUME_UP 2
#define ANNIBALE_BUTTON_VOLUME_DOWN 3
#define ANNIBALE_BUTTON_HOME     4

struct annibale_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* ANNIBALE_TOUCH_* or ANNIBALE_BUTTON_DOWN/UP */
	uint32_t finger;                /* which finger, from 0 (or which button) */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
/* For drivers; not from interrupt context. */
void annibale_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);

/* The loader's GIC description of annibale, whose EPSS and RPMh blocks an annibale driver does drive. */
bool annibale_platform_is_annibale(void);
void annibale_input_init(void);
void annibale_virtio_input_init(void);   /* boot-arg virtio_input=1: QEMU's virtio pointer and keyboard */
void annibale_goodix_init(void);         /* boot-arg touchscreen=1: the phone's Goodix touch screen */
void annibale_ufs_init(void);           /* ufs=2 to xnuloader: the phone's storage, read only, through the loader (ufs_shared_pa=) */
void annibale_ufs_report(const char *when); /* where reading the storage has spent its time so far */
bool annibale_ufs_dev(dev_t dev);           /* one of the storage's devices */
int annibale_battery_temperature(void);  /* the battery's temperature in C, -273 while it is not known */
/*
 * How the battery is, as the audio DSP's battery manager says (bsd/dev/annibale_adsp.c, with adsp=1):
 * handed to bsd/dev/annibale/annibale_battery_state.c, which keeps it for the sysctls kern.annibale.battery_* and
 * passes it on to iokit/Kernel/AnnibaleBattery.cpp, the power source that iOS reads. The structure
 * is repeated there (this header is not seen from iokit/), and `size` tells whether the two agree.
 */
struct annibale_battery_state {
	uint32_t size;                  /* sizeof(struct annibale_battery_state) */
	int32_t percent_hundredths;     /* the charge, 0..10000 */
	int32_t voltage_uv;
	int32_t current_ua;             /* more than 0 while the battery takes charge */
	int32_t temperature_hundredths; /* of a degree Celsius */
	int32_t present;                /* 1 if there is a battery */
	int32_t external;               /* 1 if a charger or a computer is on the USB port */
	int32_t status;                 /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	int32_t charge_uah, full_uah, design_uah, cycles;      /* 0 when not known (yet) */
	int32_t usb_type;               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 a computer's port, 2 a charger, ... */
	int32_t usb_voltage_uv, usb_current_ua;                 /* what the port gives */
};
void annibale_battery_report(const struct annibale_battery_state *state);
void annibale_net_init(void);            /* en0, if there is something to carry its frames: sys/annibale_net.h */
void annibale_bt_probe_init(void);       /* boot-arg bt_probe=1: the control sockets BlueTool looks for, answered with a log only */
void annibale_cpufreq_init(void);       /* ... on annibale: cpufreq=1 a governor, 2 a report, 3 the ceiling (bsd/dev/annibale_cpufreq.c) */
void annibale_cpufreq_touch(void);      /* a touch: the governor's boost */
bool annibale_gpu_temperature(int32_t *tenths); /* the hottest GPU sensor; false without cpufreq=N or a valid reading */
uint64_t annibale_cpufreq_touched(void);        /* mach_absolute_time() of the last touch, 0 for none */
void annibale_freeze_check(uint64_t touched);    /* bsd/dev/annibale_threads.c: thread dumps when a touched screen gets no frame */
uint32_t annibale_touch_unread(void);              /* bsd/dev/annibale_input.c: touch samples the input service has not read */
uint64_t annibale_touch_last(void);                /* ... mach_absolute_time() of the last finger sample (no buttons), 0 for none */
void annibale_wifi_stop(void);           /* where a run ends in order */
int annibale_wifi_say(const char *format, ...) __printflike(1, 2);      /* a line of the Wi-Fi steps' log */
int annibale_wifi_tell(const char *format, ...) __printflike(1, 2);     /* ... one that is said even with wifi=1 */
uint32_t annibale_wifi_option(const char *name);                         /* a boot-arg's number */
void annibale_wifi_quiesce(void);        /* on the way back to the loader */
/* ath12k's object and the kernel's side of it (bsd/dev/annibale_lx.c), and what that asks of the phone's radio */
int annibale_lx_start(void);             /* the driver brought up on the radio as far as it is built: 0 or an errno */
void annibale_lx_stop(void);             /* ... and down again, if it stayed up (a network joined) */
int annibale_lx_prepare(void);           /* before all that: the memory the radio will be shown, taken; 0 or an errno */
void annibale_lx_net_early(void);        /* ... and en0 made on the radio now, if the kernel was given its address */
bool annibale_wifi_radio_address(uint8_t *address);      /* the radio's own address (boot-arg wifi_mac), if one was given */

/*
 * The GPU kept on, for iokit/Kernel/AnnibaleGPU.cpp (implemented in bsd/dev/annibale/annibale_gpu_dispatch.c).
 */
bool annibale_gpu_ready(void);
bool annibale_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t annibale_gpu_address(uint64_t size);
void annibale_gpu_address_free(uint64_t address, uint64_t size);
void annibale_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes, unsigned *most,
    uint64_t *lost, uint32_t *refused);
extern uint32_t gpu_va;                 /* boot-arg gpu_va: 0 as before, 1 describe the addresses, 2 also guard them */
bool annibale_gpu_map(uint64_t address, uint64_t phys, bool read_only);
/* Commit prior map writes. Failure retains ownership of any entries already installed. */
bool annibale_gpu_mapped(void);
/* True only after all requested PTEs are removed and TLB synchronization completes. */
bool annibale_gpu_unmap(uint64_t address, uint64_t size);
void annibale_gpu_to_memory(void *start, uint64_t size);
uint32_t annibale_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool annibale_gpu_done(uint32_t fence);
uint32_t annibale_gpu_irqs(void);                /* the GPU's completion interrupts so far; 0 where there are none */
/* Between two looks at annibale_gpu_done(): until the next interrupt after `seen`, at most longest_us; without interrupts poll_us. */
void annibale_gpu_pause(uint32_t seen, uint32_t poll_us, uint32_t longest_us);
uint32_t annibale_gpu_last(void);
void annibale_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void annibale_gpu_speed(uint32_t *mhz, uint32_t *changes);      /* the A830's speed and its changes; 0 and 0 elsewhere */
void annibale_gpu_report(const char *why);
bool annibale_gpu_recover(void);
void annibale_gpu_stop(void);
#endif

#endif /* _SYS_ANNIBALE_INPUT_H_ */

/*
 * /dev/annibale-touch: a queue of touch samples (see sys/annibale_input.h).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/annibale_input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

#define TOUCH_QUEUE     512             /* records; a power of two */

/*
 * Boot-arg touch_lat=1: for measuring how long a touch takes to reach the panel, a line for every
 * sample posted ("tl: p TYPE FINGER X Y at US", the time in microseconds of mach_absolute_time),
 * to be matched with the window server's lines about reading it (userland/src/inject.c), drawing
 * (eagl.c) and the display driver's about the frame (MiniIOMobileFramebuffer.cpp): see
 * tools/latency.py.
 */
static uint32_t touch_lat = 2;          /* 2: not yet looked up */

uint32_t annibale_frames_shown;          /* (bsd/sys/annibale_input.h: the framebuffer's count) */

SYSCTL_DECL(_kern_annibale);
SYSCTL_INT(_kern_annibale, OID_AUTO, frames, CTLFLAG_RD, &annibale_frames_shown, 0,
    "frames put on the panel since boot");

extern void loader_log_line(const char *line);

/*
 * A "tl:" line, for the kernel's drivers: into the loader's log as it is written -- the kernel's own
 * log keeps only the last of a run, and a live look at the loader's (XL_FOLLOW) then misses nothing.
 * The buffer holds a kern.annibale.say line whole (255 bytes of text after "say PID NAME: "); a longer
 * line is cut and still ends with its newline, so it does not run into the next one.
 */
void annibale_tl(const char *format, ...) __printflike(1, 2);

void
annibale_tl(const char *format, ...)
{
	char line[320];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (length >= (int)sizeof(line)) {
		line[sizeof(line) - 2] = '\n';
	}
	loader_log_line(line);
}

static bool
latency_lines(void)
{
	if (touch_lat == 2) {
		uint32_t on = 0;

		touch_lat = PE_parse_boot_argn("touch_lat", &on, sizeof(on)) && on != 0;
	}
	return touch_lat != 0;
}

/*
 * The answer below is worked out once and kept, the first time by annibale_input_init(),
 * before the GPU paths that ask on every frame and every submission are up: pe_arm_get_soc_base_phys()
 * looks through the device tree with SecureDTFindEntry(), whose walk keeps its place in one static
 * variable, and two threads asking at the same time move each other's walk out of the tree (the
 * panic "Device tree pointer outside of device tree region").
 */
static int8_t is_annibale = -1;

static bool platform_is_annibale(void);

bool
annibale_platform_is_annibale(void)
{
	if (is_annibale < 0) {
		is_annibale = platform_is_annibale();
	}
	return is_annibale;
}

/* annibale's GIC distributor is at 0x16000000 and its redistributors at 0x16080000
 * (span 0x200000) inside a block that starts at 0x00400000, so its reg entry reads
 * {0x15C00000, 0x10000, 0x15C80000, 0x200000}. Taken from the loader's device tree,
 * so nothing is read from hardware to answer this. */
static bool
platform_is_annibale(void)
{
	DTEntry entry;
	const void *property;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "reg", &property, &size) != kSuccess || size != 4 * sizeof(uint64_t)) {
		return false;
	}
	const uint64_t *reg = property;

	return pe_arm_get_soc_base_phys() == 0x00400000 && reg[1] == 0x10000 && reg[3] == 0x200000 &&
	    reg[0] == 0x15C00000 && reg[2] == 0x15C80000;
}

static LCK_GRP_DECLARE(touch_lck_grp, "annibale-touch");
static LCK_MTX_DECLARE(touch_lock, &touch_lck_grp);
static struct annibale_touch touch_queue[TOUCH_QUEUE];
static uint32_t touch_head, touch_tail;        /* read from head, add at tail */
static uint64_t screen_touched;                 /* mach_absolute_time() of the last finger sample, buttons not counted */

/*
 * The way out when nothing else is at hand: five fingers held on the screen for a second and a
 * half end the run -- control goes back to the loader, as when loader_return_after expires.
 * Only a driver's fingers count, not samples a program writes to the device.
 */
#define EXIT_FINGERS    5
#define EXIT_HOLD_MS    1500

extern void loader_return(void);
extern void annibale_var_save_on_return(void);
extern void annibale_wifi_stop(void);

static thread_call_t exit_call;
static uint32_t fingers_down;           /* one bit for each finger a driver says is down */

static void
exit_hold_elapsed(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	printf("\n--- %d fingers held on the screen for %d ms: back to the loader ---\n", EXIT_FINGERS, EXIT_HOLD_MS);
	annibale_var_save_on_return();
	annibale_wifi_stop();
	loader_return();
}

/* With touch_lock held. */
static void
count_fingers(uint32_t type, uint32_t finger)
{
	int before = __builtin_popcount(fingers_down), now;
	uint64_t deadline;

	if (finger >= 32 || exit_call == NULL || (type != ANNIBALE_TOUCH_DOWN && type != ANNIBALE_TOUCH_UP)) {
		return;
	}
	if (type == ANNIBALE_TOUCH_DOWN) {
		fingers_down |= 1u << finger;
	} else {
		fingers_down &= ~(1u << finger);
	}
	now = __builtin_popcount(fingers_down);
	if (now >= EXIT_FINGERS && before < EXIT_FINGERS) {
		clock_interval_to_deadline(EXIT_HOLD_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(exit_call, deadline);
	} else if (now < EXIT_FINGERS && before >= EXIT_FINGERS) {
		thread_call_cancel(exit_call);
	}
}

/*
 * What the fingers' samples were like, said every 8 strokes: the lines of touch_lat=1 are many, and a long run's log keeps only its
 * beginning and its end. From the first move after a finger came down, between moves, and from the last move to its lift.
 */
static struct {
	uint64_t last;
	uint32_t strokes, moves, firsts, slow_moves, longest_move, first_sum, first_longest, lift_sum, lift_longest;
	uint32_t finger;
	bool down, moved;
} touch_feel;

static void
touch_feel_add(uint32_t type, uint32_t finger, uint64_t now)
{
	uint64_t nanos;
	uint32_t ms;

	if (type == ANNIBALE_TOUCH_DOWN && !touch_feel.down) {
		touch_feel.down = true;
		touch_feel.moved = false;
		touch_feel.finger = finger;
		touch_feel.last = now;
		return;
	}
	if (!touch_feel.down || finger != touch_feel.finger) {
		return;
	}
	absolutetime_to_nanoseconds(now - touch_feel.last, &nanos);
	ms = (uint32_t)(nanos / 1000000);
	touch_feel.last = now;
	if (type == ANNIBALE_TOUCH_UP) {
		touch_feel.down = false;
		touch_feel.strokes++;
		touch_feel.lift_sum += ms;
		touch_feel.lift_longest = ms > touch_feel.lift_longest ? ms : touch_feel.lift_longest;
		if (touch_feel.strokes % 8 == 0) {
			printf("touch: %u strokes: first move after %u ms on average, %u at most; %u moves, %u of them over 13 ms apart, %u at most; "
			    "the lift %u ms after the last move on average, %u at most\n", touch_feel.strokes,
			    touch_feel.firsts != 0 ? touch_feel.first_sum / touch_feel.firsts : 0, touch_feel.first_longest, touch_feel.moves,
			    touch_feel.slow_moves, touch_feel.longest_move, touch_feel.lift_sum / touch_feel.strokes, touch_feel.lift_longest);
		}
	} else if (!touch_feel.moved) {
		touch_feel.moved = true;
		touch_feel.firsts++;
		touch_feel.first_sum += ms;
		touch_feel.first_longest = ms > touch_feel.first_longest ? ms : touch_feel.first_longest;
	} else {
		touch_feel.moves++;
		touch_feel.slow_moves += ms > 13;
		touch_feel.longest_move = ms > touch_feel.longest_move ? ms : touch_feel.longest_move;
	}
}

static void
touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, bool from_driver)
{
	struct annibale_touch *t;

	lck_mtx_lock(&touch_lock);
	if (touch_tail - touch_head == TOUCH_QUEUE) {
		touch_head++;                   /* nobody reads: drop the oldest */
	}
	t = &touch_queue[touch_tail++ % TOUCH_QUEUE];
	t->time = mach_absolute_time();
	if (from_driver) {
		touch_feel_add(type, finger, t->time);
	}
	if (type <= ANNIBALE_TOUCH_UP) {
		__atomic_store_n(&screen_touched, t->time, __ATOMIC_RELAXED);
	}
	if (latency_lines()) {
		uint64_t ns;

		absolutetime_to_nanoseconds(t->time, &ns);
		annibale_tl("tl: p %u %u %u %u at %llu\n", type, finger, x, y, ns / 1000);
	}
	t->type = type;
	t->finger = finger;
	t->x = x > 65535 ? 65535 : x;
	t->y = y > 65535 ? 65535 : y;
	if (from_driver) {
		count_fingers(type, finger);
	}
	lck_mtx_unlock(&touch_lock);
	wakeup(&touch_queue);
	annibale_cpufreq_touch();
}

void
annibale_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	touch_post(type, finger, x, y, true);
}

/* Samples posted that the input service has not read yet (for the freeze watchdog's line; unlocked). */
uint32_t
annibale_touch_unread(void)
{
	return __atomic_load_n(&touch_tail, __ATOMIC_RELAXED) - __atomic_load_n(&touch_head, __ATOMIC_RELAXED);
}

uint64_t
annibale_touch_last(void)
{
	return __atomic_load_n(&screen_touched, __ATOMIC_RELAXED);
}

static int
touch_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct annibale_touch t;
	int error = 0, moved = 0;

	lck_mtx_lock(&touch_lock);
	while (touch_head == touch_tail) {
		error = msleep(&touch_queue, &touch_lock, PZERO | PCATCH, "annibale-touch", NULL);
		if (error) {
			lck_mtx_unlock(&touch_lock);
			return error;
		}
	}
	while (touch_head != touch_tail && uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		t = touch_queue[touch_head++ % TOUCH_QUEUE];
		lck_mtx_unlock(&touch_lock);
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		lck_mtx_lock(&touch_lock);
		if (error) {
			break;
		}
		moved++;
	}
	lck_mtx_unlock(&touch_lock);
	return moved ? 0 : error;
}

static int
touch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct annibale_touch t;
	int error = 0;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		if (error) {
			break;
		}
		if (t.type < ANNIBALE_TOUCH_DOWN || t.type > ANNIBALE_BUTTON_UP) {
			return EINVAL;
		}
		touch_post(t.type, t.finger, t.x, t.y, false);
	}
	return error;
}

static const struct cdevsw touch_cdevsw = {
	.d_open = touch_open, .d_close = touch_close, .d_read = touch_read, .d_write = touch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
annibale_input_init(void)
{
	int major = cdevsw_add(-1, &touch_cdevsw);

	(void)annibale_platform_is_annibale();
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "annibale-touch") == NULL) {
		printf("annibale-touch: cannot create the device\n");
	}
	exit_call = thread_call_allocate(exit_hold_elapsed, NULL);
	if (annibale_platform_is_annibale()) {
		annibale_cpufreq_init();
	}
	annibale_virtio_input_init();
	annibale_goodix_init();
	annibale_ufs_init();
	annibale_net_init();
	annibale_bt_probe_init();
}

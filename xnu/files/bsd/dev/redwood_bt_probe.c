/*
 * Boot-arg bt_probe=1: the control sockets BlueTool looks for, answered only as far as is needed
 * to see what it does.
 *
 * iOS's Bluetooth transport is not a tty. BlueTool makes a PF_SYSTEM SOCK_STREAM socket on
 * SYSPROTO_CONTROL, resolves "com.apple.uart.bluetooth" with CTLIOCGINFO, sets 8N1 and RTS/CTS
 * flow control through the TIOCGETA/TIOCSETA socket options, and then speaks HCI over it. What
 * is registered here is that socket with nothing behind it: connects, the options it is given
 * and the bytes the client writes are logged, and nothing is ever written back, so the
 * client stops at its first read. One run of that answers three questions that decide the rest
 * of the Bluetooth work -- which transport iOS 26 picks, which platform/chipset/manufacturer
 * triple it asks for, and where it looks for its override scripts.
 *
 * Off unless the boot-arg is set: no control is registered and nothing else changes.
 *
 * The socket type and the name lookup were read out of BlueTool rather than assumed:
 * socket(PF_SYSTEM=0x20, SOCK_STREAM=1, SYSPROTO_CONTROL=2), then
 * ioctl(fd, CTLIOCGINFO=0xc0644e03, &ctl_info) for the ID, then connect() with that ID and
 * sc_unit 0.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/redwood_input.h>
#include <sys/socket.h>
#include <sys/systm.h>

#define BT_CTL_NAME_BLUETOOTH   "com.apple.uart.bluetooth"
#define BT_CTL_NAME_SKYWALK     "com.apple.uart.sk.bluetooth"
#define BT_SAY_FRAMES           64              /* sends logged in full, then only counted */
#define BT_SAY_FAST             30              /* summary lines 10 s apart, then one a minute */
#define BT_SAY_BYTES            32              /* of each send */

static kern_ctl_ref bt_ctl_bluetooth, bt_ctl_skywalk;

/* What has been seen, for the summary a later round may want. */
static struct {
	uint64_t connects, disconnects, sends, send_bytes, setopts, getopts;
} bt_count;

static const char *
bt_name(kern_ctl_ref kctl)
{
	if (kctl == bt_ctl_bluetooth) {
		return BT_CTL_NAME_BLUETOOTH;
	}
	if (kctl == bt_ctl_skywalk) {
		return BT_CTL_NAME_SKYWALK;
	}
	return "unknown control";
}

static errno_t
bt_connect(kern_ctl_ref kctl, struct sockaddr_ctl *sac, void **unitinfo)
{
	*unitinfo = NULL;                       /* nothing per connection: this control carries nothing */
	bt_count.connects++;
	printf("bt: %s: client connected, unit %u, control %u (so far: %llu connects, %llu sends, %llu bytes, %llu setopts)\n",
	    bt_name(kctl), (unsigned)sac->sc_unit, (unsigned)sac->sc_id,
	    (unsigned long long)bt_count.connects, (unsigned long long)bt_count.sends,
	    (unsigned long long)bt_count.send_bytes, (unsigned long long)bt_count.setopts);
	return 0;
}

static errno_t
bt_disconnect(kern_ctl_ref kctl, u_int32_t unit, __unused void *unitinfo)
{
	bt_count.disconnects++;
	printf("bt: %s: client disconnected, unit %u (so far: %llu sends, %llu bytes)\n", bt_name(kctl),
	    (unsigned)unit, (unsigned long long)bt_count.sends, (unsigned long long)bt_count.send_bytes);
	return 0;
}

/* NUL-terminated hex of the first BT_SAY_BYTES bytes, for a log line. */
static void
bt_hex(const uint8_t *data, size_t length, char *out)
{
	static const char digits[] = "0123456789abcdef";
	size_t i, taken = 0;

	if (length > BT_SAY_BYTES) {
		length = BT_SAY_BYTES;
	}
	for (i = 0; i < length; i++) {
		out[taken++] = digits[data[i] >> 4];
		out[taken++] = digits[data[i] & 0x0f];
		out[taken++] = ' ';
	}
	out[taken > 0 ? taken - 1 : 0] = 0;
}

/* The client has bytes for the chip: log them and answer nothing. */
static errno_t
bt_send(kern_ctl_ref kctl, u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	size_t length = mbuf_pkthdr_len(m);
	char line[BT_SAY_BYTES * 3 + 1];

	if (length == 0) {
		return 0;
	}
	bt_count.sends++;
	bt_count.send_bytes += length;
	if (bt_count.sends <= BT_SAY_FRAMES) {
		bt_hex(mbuf_data(m), mbuf_len(m), line);
		printf("bt: %s: unit %u: %llu bytes from the client: %s%s\n", bt_name(kctl), (unsigned)unit,
		    (unsigned long long)length, line, length > BT_SAY_BYTES ? " ..." : "");
	} else if (bt_count.sends == BT_SAY_FRAMES + 1) {
		printf("bt: %s: further sends are counted only\n", bt_name(kctl));
	}
	return 0;
}

/* setsockopt on the control socket: TIOCSETA and friends, 8N1 and flow control live here. */
static errno_t
bt_setopt(kern_ctl_ref kctl, u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t len)
{
	bt_count.setopts++;
	printf("bt: %s: setopt %#x on unit %u, %llu bytes\n", bt_name(kctl), opt, (unsigned)unit,
	    (unsigned long long)len);
	if (len >= 8 && data != NULL) {
		const uint32_t *word = data;
		printf("bt: %s: setopt %#x first words %#x %#x\n", bt_name(kctl), opt, word[0], word[1]);
	}
	return 0;
}

/* getsockopt: hand back a zeroed termios, which the client reads to set what it asked for. */
static errno_t
bt_getopt(kern_ctl_ref kctl, u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	size_t given = len != NULL ? *len : 0;

	bt_count.getopts++;
	printf("bt: %s: getopt %#x on unit %u, %llu bytes\n", bt_name(kctl), opt, (unsigned)unit,
	    (unsigned long long)given);
	if (data != NULL && given > 0) {
		memset(data, 0, given);         /* no termios state yet: what the client sets, it sets on nothing */
	}
	return 0;
}

static bool
bt_register(const char *name, kern_ctl_ref *kctl)
{
	struct kern_ctl_reg reg;
	errno_t error;

	memset(&reg, 0, sizeof(reg));
	strlcpy(reg.ctl_name, name, sizeof(reg.ctl_name));
	/*
	 * A stream control, because BlueTool's socket() asks for SOCK_STREAM. No bind callback: the
	 * struct this header declares has no ctl_bind (the full one is in kern_control_private.h,
	 * behind KERNEL_PRIVATE), and bind() on a control socket is not on the path this probe is
	 * watching -- the client resolves the name with CTLIOCGINFO and then connects.
	 */
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_connect = bt_connect;
	reg.ctl_disconnect = bt_disconnect;
	reg.ctl_send = bt_send;
	reg.ctl_setopt = bt_setopt;
	reg.ctl_getopt = bt_getopt;
	/*
	 * No enable step: this XNU has no ctl_enable(), a control is connectable once
	 * ctl_register() returns 0.
	 */
	error = ctl_register(&reg, kctl);
	if (error != 0) {
		printf("bt: %s cannot be registered: error %d\n", name, error);
		return false;
	}
	return true;
}

/*
 * A line every ten seconds, then one a minute, saying the probe is up and what it has counted.
 *
 * The reason is the log ring: it holds about fifteen seconds of a boot that is busy, and snapshots
 * are twenty seconds apart, so a line printed once when the probe starts can be gone before
 * anything reads it. With a client that never connects there would be nothing else at all, and the
 * run would look like the probe never came up. These lines repeat the same facts, so a later
 * snapshot still shows them.
 */
static void __attribute__((noreturn))
bt_summary_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t tick = 0, seconds = 0, step;

	for (;;) {
		step = tick < BT_SAY_FAST ? 10 : 60;
		delay_for_interval(step, NSEC_PER_SEC);
		seconds += step;
		tick++;
		printf("bt: probe up %u s: %llu connects, %llu sends, %llu bytes, %llu setopts, %llu getopts\n",
		    seconds, (unsigned long long)bt_count.connects,
		    (unsigned long long)bt_count.sends, (unsigned long long)bt_count.send_bytes,
		    (unsigned long long)bt_count.setopts, (unsigned long long)bt_count.getopts);
	}
}

/* Boot-arg bt_probe=1: register the two names BlueTool looks for and keep the log to them. */
void
redwood_bt_probe_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("bt_probe", &on, sizeof(on)) || !on) {
		return;
	}
	if (!bt_register(BT_CTL_NAME_BLUETOOTH, &bt_ctl_bluetooth)) {
		return;
	}
	if (!bt_register(BT_CTL_NAME_SKYWALK, &bt_ctl_skywalk)) {
		printf("bt: %s is not offered; %s alone will do for the first run\n", BT_CTL_NAME_SKYWALK,
		    BT_CTL_NAME_BLUETOOTH);
	}
	printf("bt: probe on: %s and %s answer nothing; bytes the clients write go to this log\n",
	    BT_CTL_NAME_BLUETOOTH, bt_ctl_skywalk != NULL ? BT_CTL_NAME_SKYWALK : "(second name failed)");
	thread = THREAD_NULL;
	if (kernel_thread_start(bt_summary_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

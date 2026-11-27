/*
 * sensors_scan boot-arg pin-localization scan for the annibale ALS/prox buses, one-shot at boot.
 *
 * The gpio16-conflict investigation (out/goal-1124/batch7/gpio16-conflict-investigation.md) proved
 * the original SE12 = GPIO16/17 assignment wrong: those pins carry the WCN7850 WLAN enable and
 * bitbanging them only breaks Wi-Fi. gpio-offline-evidence.md then refuted the bus_instance =
 * global SE reading and bounded the physical candidate space to the idle bus pool: qupv3_2
 * SE10/11/13, qupv3_1 SE1/SE4 and the i2c hub pairs, none of which has an in-use owner in the
 * vendor device tree. This scan is the experiment that settles the pinout: open-drain ACK probes
 * of the sensor slave addresses on each candidate group, starting with the zero-conflict
 * gpio20/21, gpio12/13 and gpio8/9 triple.
 *
 *   sensors_scan=1               scan the default groups (20,21), (12,13) plus the (8,9) control
 *   sensors_scan=20,21,12,13     scan the given sda,scl pairs instead, up to 16 groups
 *
 * The default set is the priority-1 surface of gpio-offline-evidence.md section 6; the wider idle
 * surface (gpio36/37, gpio48/49, the i2c hub pairs) is reachable through the custom form without a
 * rebuild. Each group is probed at 0x31 (glsx) and 0x47 (stk6a2x), plus 0x2c: the sx937x SAR
 * sensor shares its bus with stk6a2x in the persist registry, so 0x47 and 0x2c answering on the
 * same pair is the signature of the stk6a2x bus (0x2c alone is ambiguous, qmc630x uses it too).
 *
 * While the boot-arg is present the scan replaces the hardware paths: sensors_hw_i2c and
 * sensors_probe are forced off in annibale_sensors_init(), so GPIO16/17 is never bitbanged.
 * Entry pin mux configurations are always restored before returning; the scan leaves no trace.
 * With sensors_rails= candidates present, a rail phase re-scans after each additive vote, so one
 * round also answers whether a silent bus was unpowered rather than wrongly pinned.
 */

#define SNS_SCAN_GROUPS_MAX     16      /* the full P1+P2+P3 idle surface of gpio-offline-evidence.md */
#define SNS_SCAN_ADDR_COUNT     3       /* 0x31 glsx, 0x47 stk6a2x, 0x2c sx937x/qmc630x */
#define SNS_SCAN_CLOCK_SAMPLES  2000    /* 2 s at 1 kHz before any pin is touched */

static struct {
	uint32_t sda, scl;
	int gate_bit;               /* bit in the clock-activity mask of this pair's engine, else -1 */
} scan_groups[SNS_SCAN_GROUPS_MAX];
static uint32_t scan_group_count;
static uint32_t scan_acks[SNS_SCAN_GROUPS_MAX][SNS_SCAN_ADDR_COUNT];   /* per group, per address index */
static uint32_t scan_tries[SNS_SCAN_GROUPS_MAX][SNS_SCAN_ADDR_COUNT];

/* mainline gcc-sm8750.c engine branches, exact halt offsets (the wrap1 stride is not uniform):
 * halt status is read-only, the AP votes live in GCC_QUPV3_WRAP1_VOTE/WRAP2_VOTE. In the chip-wide
 * numbering wrap1 sN is SE(N) and wrap2 sN is SE(8+N); wrap1_s2 is cross-checked against the
 * touch driver's A_SE2_HALT. The activity mask uses bits 0-7 for wrap2 s0-s7, 8-15 for wrap1. */
static const uint32_t scan_halt_wrap2[8] = {
	0x1e004, 0x1e140, 0x1e27c, 0x1e3b8, 0x1e4f4, 0x1e630, 0x1e76c, 0x1e8a8,
};
static const uint32_t scan_halt_wrap1[8] = {
	0x18004, 0x18140, 0x1827c, 0x18290, 0x183cc, 0x18508, 0x18644, 0x18780,
};
#define GCC_QUPV3_WRAP1_VOTE            0x52008UL

/* wrap1 s2 is the touch SPI engine: its activity is expected in every round and a bitbang of
 * other pins cannot disturb it, so unknown-SE pairs are not gated on it. */
#define SCAN_TOUCH_ENGINE_BIT           (8 + 2)

static uint64_t
scan_now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

/*
 * A fatal-phase marker: the scan of the 36/37 and 48/49 pairs kills the kernel one or two
 * seconds into the boot (kernel-12377-flipwd14-b5r25 RESULTS section 3) and nothing after the
 * reset can say where it died, so the scan's phases and every first pin write are written with
 * loader_log_line_sync, which pushes them to the point of coherence at once. Each one costs a
 * clean of the log so far; the scan is a one-shot boot diagnostic of a handful of groups, so
 * the total is tens of milliseconds, and the bitbang waveform itself is never interrupted --
 * the marks bracket it, they do not interrupt it.
 */
static void
scan_mark(const char *what, uint32_t sda, uint32_t scl)
{
	char line[96];

	snprintf(line, sizeof(line), "sensors-scan: mark %s gpio%u/%u t=%lluus\n", what, sda, scl, scan_now_us());
	loader_log_line_sync(line);
}

/* The activity-mask bit of the engine a known pin pair belongs to, from the Android dts pinmux
 * groups: qupv3_se13 = gpio20/21 (qup2_se5), qupv3_se11 = gpio12/13 (qup2_se3), qupv3_se10 =
 * gpio8/9 (qup2_se2) on wrapper 2; qupv3_1 SE1 = gpio36/37, SE4 = gpio48/49 on wrapper 1
 * (gpio-offline-evidence.md section 3.4). */
static int
scan_gate_bit_of_pair(uint32_t sda, uint32_t scl)
{
	if (sda == 20 && scl == 21) {
		return 5;       /* wrap2 s5 = SE13 */
	}
	if (sda == 12 && scl == 13) {
		return 3;       /* wrap2 s3 = SE11 */
	}
	if (sda == 8 && scl == 9) {
		return 2;       /* wrap2 s2 = SE10 */
	}
	if (sda == 36 && scl == 37) {
		return 8 + 1;   /* wrap1 s1 = SE1 */
	}
	if (sda == 48 && scl == 49) {
		return 8 + 4;   /* wrap1 s4 = SE4 */
	}
	return -1;
}

static void
scan_add_group(uint32_t sda, uint32_t scl)
{
	scan_groups[scan_group_count].sda = sda;
	scan_groups[scan_group_count].scl = scl;
	scan_groups[scan_group_count].gate_bit = scan_gate_bit_of_pair(sda, scl);
	scan_group_count++;
}

/*
 * Parse the scan groups. Numbers pair up as (sda, scl); pairs with a pin outside the TLMM range
 * or inside the WCN7850 connectivity-owned SE12 group (gpio16-19) are dropped, a trailing
 * unpaired number is ignored. Anything that yields no valid pair, the plain enable form
 * sensors_scan=1 included, falls back to the default candidate set.
 */
static bool
scan_pin_forbidden(uint32_t pin)
{
	/* WCN7850 connectivity (gpio16-19) and DTB qcom,gpios-reserved TZ/HYP XPU pins (36-39, 48-51, 74). */
	return (pin >= 16u && pin <= 19u) || (pin >= 36u && pin <= 39u) ||
	    (pin >= 48u && pin <= 51u) || pin == 74u;
}
static void
parse_scan_boot_arg(void)
{
	char arg[128] = { 0 };
	uint32_t nums[SNS_SCAN_GROUPS_MAX * 2] = { 0 };
	uint32_t count = 0, pairs = 0;
	const char *p = arg;
	bool custom;

	if (!PE_parse_boot_arg_str("sensors_scan", arg, sizeof(arg))) {
		return;
	}
	while (*p != '\0' && count < SNS_SCAN_GROUPS_MAX * 2) {
		if (*p >= '0' && *p <= '9') {
			uint32_t v = 0;
			while (*p >= '0' && *p <= '9') {
				v = v * 10 + (uint32_t)(*p - '0');
				p++;
			}
			nums[count++] = v;
		} else if (*p == ',') {
			p++;
		} else {
			break;
		}
	}
	for (uint32_t i = 0; i + 1 < count && scan_group_count < SNS_SCAN_GROUPS_MAX; i += 2) {
		if (nums[i] < 220 && nums[i + 1] < 220 &&
		    !scan_pin_forbidden(nums[i]) && !scan_pin_forbidden(nums[i + 1])) {
			scan_add_group(nums[i], nums[i + 1]);
			pairs++;
		} else {
			say("sensors: sensors_scan pair (%u,%u) invalid or connectivity-owned, dropped\n",
			    nums[i], nums[i + 1]);
		}
	}
	custom = pairs != 0;
	if (!custom) {
		scan_add_group(20, 21);         /* gls6151 hypothesis: SE13, slave 0x31 */
		scan_add_group(12, 13);         /* stk6a2x hypothesis: SE11, slave 0x47 */
		scan_add_group(8, 9);           /* control: the retired SE10 assignment */
	}
	say("sensors: sensors_scan=%s: %u pin group(s)%s\n", arg, scan_group_count,
	    custom ? "" : " (default set)");
}

/*
 * Sample all sixteen wrapper-1/wrapper-2 engine halt registers plus the AP vote registers.
 * The bitmask of engines seen running gates the bitbang per group: a clocked engine means the
 * SSC or another master may be driving that bus and the probe of it is meaningless anyway.
 */
static uint32_t
scan_clock_sample(void)
{
	uint32_t on_count[16] = { 0 };
	uint32_t active = 0;
	uint32_t vote1_first = 0, vote1_last = 0, vote2_first = 0, vote2_last = 0;

	if (gcc_base == 0) {
		say("sensors-scan: no GCC mapping, clock check unread, bitbang proceeds unverified\n");
		return 0;
	}
	for (uint32_t i = 0; i < SNS_SCAN_CLOCK_SAMPLES; i++) {
		for (uint32_t s = 0; s < 8; s++) {
			if (sns_clk_running(sns_reg_rd(gcc_base, scan_halt_wrap2[s]))) {
				on_count[s]++;
			}
			if (sns_clk_running(sns_reg_rd(gcc_base, scan_halt_wrap1[s]))) {
				on_count[8 + s]++;
			}
		}
		vote2_last = sns_reg_rd(gcc_base, GCC_QUPV3_WRAP2_VOTE);
		vote1_last = sns_reg_rd(gcc_base, GCC_QUPV3_WRAP1_VOTE);
		if (i == 0) {
			vote2_first = vote2_last;
			vote1_first = vote1_last;
		}
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	for (uint32_t s = 0; s < 16; s++) {
		if (on_count[s] == 0) {
			continue;
		}
		active |= 1u << s;
		uint32_t engine = s % 8;
		say("sensors-scan: clock wrap%u s%u/SE%u (halt %#x) on in %u/%u samples\n",
		    s < 8 ? 2u : 1u, engine, s < 8 ? engine + 8 : engine,
		    s < 8 ? scan_halt_wrap2[engine] : scan_halt_wrap1[engine],
		    on_count[s], SNS_SCAN_CLOCK_SAMPLES);
	}
	say("sensors-scan: clocks %u samples over 2 s: vote1 first=%#x last=%#x, vote2 first=%#x last=%#x, active-mask=%#x\n",
	    SNS_SCAN_CLOCK_SAMPLES, vote1_first, vote1_last, vote2_first, vote2_last, active);
	return active;
}

static bool
scan_group_clocked(uint32_t gi, uint32_t active)
{
	int bit = scan_groups[gi].gate_bit;

	if (bit >= 0) {
		return (active & (1u << (uint32_t)bit)) != 0;
	}
	/* A pair with no known SE mapping is probed only while no unaccounted engine runs. */
	return (active & ~(1u << SCAN_TOUCH_ENGINE_BIT)) != 0;
}

/*
 * One pass over every ungated group: bus idle, then a bare start/address-write/stop probe of each
 * sensor address. The pin levels in each line are read in the idle state right before the probe;
 * a low line there means the bus has no working pull-up and the nack says nothing about the chip.
 */
static uint32_t
scan_probe_pass(const char *tag, uint32_t active, uint32_t found, bool marks)
{
	static const uint8_t addrs[SNS_SCAN_ADDR_COUNT] = {
		ANNIBALE_GLS6151_I2C_ADDR, ANNIBALE_STK6A2X_I2C_ADDR, 0x2c,
	};

	for (uint32_t gi = 0; gi < scan_group_count; gi++) {
		if (scan_group_clocked(gi, active)) {
			continue;
		}
		uint32_t sda = scan_groups[gi].sda, scl = scan_groups[gi].scl;
		if (marks) {
			scan_mark("busidle-pre", sda, scl);
		}
		sns_bus_idle(sda, scl);
		if (marks) {
			scan_mark("busidle-post", sda, scl);
		}
		for (uint32_t ai = 0; ai < SNS_SCAN_ADDR_COUNT; ai++) {
			uint32_t sda_in = sns_tlmm_get_in(sda) ? 1u : 0u;
			uint32_t scl_in = sns_tlmm_get_in(scl) ? 1u : 0u;
			uint64_t t = scan_now_us();
			bool ack;

			if (marks && ai == 0) {
				scan_mark("probe-pre", sda, scl);
			}
			ack = od_i2c_ack_probe(sda, scl, addrs[ai]);
			if (marks && ai == 0) {
				scan_mark("probe-post", sda, scl);
			}

			say("sensors-scan: %s gpio%u/%u 0x%02x %s t=%lluus sda=%u scl=%u\n",
			    tag, sda, scl, addrs[ai], ack ? "ack" : "nack", t, sda_in, scl_in);
			scan_tries[gi][ai]++;
			if (ack) {
				scan_acks[gi][ai]++;
				found |= 1u << gi;
			}
		}
	}
	return found;
}

static void
sensors_scan_run(void)
{
	static const char *addr_names[SNS_SCAN_ADDR_COUNT] = { "0x31", "0x47", "0x2c" };
	uint32_t snap_pins[SNS_SCAN_GROUPS_MAX * 2];
	uint32_t snap_cfg[SNS_SCAN_GROUPS_MAX * 2];
	uint32_t snap_io[SNS_SCAN_GROUPS_MAX * 2];
	uint32_t snap_count = 0;
	uint32_t active, found = 0, full_mask;

	say("sensors-scan: starting, %u group(s), 0x31/0x47/0x2c probed per group\n", scan_group_count);
	scan_mark("start", 0, 0);
	if (tlmm_base == 0) {
		say("sensors-scan: no TLMM mapping, scan aborted\n");
		return;
	}

	/* Pristine pin state first: the cfg and input level before this boot touches anything.
	 * The two IRQ lines are attribution evidence: glsx drives gpio163, stk6a2x gpio35. */
	say("sensors-scan: pre irq gpio%d cfg=%#x in=%u, gpio%d cfg=%#x in=%u\n",
	    ANNIBALE_GLS6151_IRQ_GPIO, sns_reg_rd(tlmm_base, ANNIBALE_GLS6151_IRQ_GPIO * 0x1000u),
	    sns_tlmm_get_in(ANNIBALE_GLS6151_IRQ_GPIO) ? 1u : 0u,
	    ANNIBALE_STK6A2X_IRQ_GPIO, sns_reg_rd(tlmm_base, ANNIBALE_STK6A2X_IRQ_GPIO * 0x1000u),
	    sns_tlmm_get_in(ANNIBALE_STK6A2X_IRQ_GPIO) ? 1u : 0u);
	for (uint32_t gi = 0; gi < scan_group_count; gi++) {
		uint32_t sda = scan_groups[gi].sda, scl = scan_groups[gi].scl;
		say("sensors-scan: pre gpio%u cfg=%#x in=%u, gpio%u cfg=%#x in=%u\n",
		    sda, sns_reg_rd(tlmm_base, sda * 0x1000u), sns_tlmm_get_in(sda) ? 1u : 0u,
		    scl, sns_reg_rd(tlmm_base, scl * 0x1000u), sns_tlmm_get_in(scl) ? 1u : 0u);
		if (scan_groups[gi].gate_bit < 0) {
			say("sensors-scan: gpio%u/%u SE unknown, gated on any non-touch engine activity\n",
			    sda, scl);
		}
	}
	scan_mark("clocksample-pre", 0, 0);
	active = scan_clock_sample();
	scan_mark("clocksample-post", 0, 0);
	for (uint32_t gi = 0; gi < scan_group_count; gi++) {
		if (scan_group_clocked(gi, active)) {
			say("sensors-scan: gpio%u/%u skipped, its SE clock is active\n",
			    scan_groups[gi].sda, scan_groups[gi].scl);
		}
		for (uint32_t pin = 0; pin < 2; pin++) {
			snap_pins[snap_count] = pin == 0 ? scan_groups[gi].sda : scan_groups[gi].scl;
			snap_cfg[snap_count] = sns_reg_rd(tlmm_base, snap_pins[snap_count] * 0x1000u);
			snap_io[snap_count] = sns_reg_rd(tlmm_base, snap_pins[snap_count] * 0x1000u + 0x04u);
			snap_count++;
		}
	}

	for (uint32_t rep = 1; rep <= 3; rep++) {
		char tag[8];
		snprintf(tag, sizeof(tag), "r%u", rep);
		found = scan_probe_pass(tag, active, found, rep == 1);
		delay_for_interval(10, NSEC_PER_MSEC);
	}

	/* Rail phase: additive votes from sensors_rails=, re-scanning after each. The vote lines
	 * carry the sensors-probe P1b prefix because the helper is shared with the probe suite. */
	full_mask = (1u << scan_group_count) - 1u;
	for (uint32_t gi = 0; gi < scan_group_count; gi++) {
		if (scan_group_clocked(gi, active)) {
			found |= 1u << gi;      /* unprobeable groups do not hold the rail phase open */
		}
	}
	if (probe_rail_count != 0 && found != full_mask) {
		say("sensors-scan: entering rail phase, %u candidate(s)\n", probe_rail_count);
		for (uint32_t i = 0; i < probe_rail_count && found != full_mask; i++) {
			char tag[24];
			sns_rail_vote(probe_rails[i].name, probe_rails[i].mv);
			delay_for_interval(20, NSEC_PER_MSEC);
			snprintf(tag, sizeof(tag), "rail-%.8s", probe_rails[i].name);
			found = scan_probe_pass(tag, active, found, true);
		}
	}

	for (uint32_t i = 0; i < snap_count; i++) {
		/* The scan leaves every pin output-disabled, so the output value bit goes back first
		 * and the cfg (which may re-enable the output) second: no driven glitch in between.
		 * Bit0 of the in_out readback is the read-only input sense, writes to it are ignored. */
		scan_mark("restore-pre", snap_pins[i], 0);
		sns_reg_wr(tlmm_base, snap_pins[i] * 0x1000u + 0x04u, snap_io[i]);
		sns_reg_wr(tlmm_base, snap_pins[i] * 0x1000u, snap_cfg[i]);
		scan_mark("restore-post", snap_pins[i], 0);
	}
	for (uint32_t gi = 0; gi < scan_group_count; gi++) {
		say("sensors-scan: summary gpio%u/%u: %s ack %u/%u, %s ack %u/%u, %s ack %u/%u\n",
		    scan_groups[gi].sda, scan_groups[gi].scl,
		    addr_names[0], scan_acks[gi][0], scan_tries[gi][0],
		    addr_names[1], scan_acks[gi][1], scan_tries[gi][1],
		    addr_names[2], scan_acks[gi][2], scan_tries[gi][2]);
	}
	say("sensors-scan: done, pin mux snapshot restored\n");
	scan_mark("done", 0, 0);
}

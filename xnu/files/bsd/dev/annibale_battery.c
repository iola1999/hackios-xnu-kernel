/*
 * annibale: TI BQ27Z561 at 0x55 on QUP1 SE5, SDA=GPIO52, SCL=GPIO53.
 * Read standard commands with open-drain GPIO and restore both pins after each
 * sample. Gauge and charger configuration remain firmware-owned.
 * Sources: live Android DT/driver and TI SLUUBO7 chapters 10 and 13.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void annibale_spmi_lock(void);
extern void annibale_spmi_unlock(void);

#define ARB_CORE_PHYS           0x0c400000UL
#define ARB_CORE_SIZE           0x003000UL
#define ARB_CNFG_PHYS           0x0c42d000UL
#define ARB_CNFG_SIZE           0x004000UL
#define ARB_CHNLS_PHYS          0x0c500000UL
#define ARB_CHANNEL_SIZE        0x1000UL
#define ARB_OBSRV_PHYS          0x0c440000UL
#define ARB_OBSRV_SIZE          0x0080000UL

#define ARB_VERSION_7           0x70000000u
#define ARB_SYSTEM              0
#define APID_MAX                ((ARB_CORE_SIZE - 0x2000) / 4)
#define APID_MAP(n)             (0x2000UL + 4 * (n))
#define APID_OWNER(n)           (4 * (n))

#define CH_CMD                  0x00
#define CH_STATUS               0x08
#define CH_WDATA0               0x10
#define CH_RDATA0               0x18
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define OPC_EXT_READ            13
#define OPC_EXT_WRITEL          0
#define CMD_OPC(opc)            ((uint32_t)(opc) << 27)
#define CMD_REG(reg)            (((uint32_t)(reg) & 0xff) << 4)

struct charger_module {
	const char *name;
	uint8_t func;
	uint16_t ppid;
	uint16_t apid;
	uint8_t slave;
	uint8_t owner;
	bool found;
	bool ours;
	vm_offset_t chnl_wr;
	vm_offset_t chnl_rd;
};

static struct charger_module chgr_mod = { .name = "CHGR", .func = 0x10, .found = false };
static struct charger_module usbin_mod = { .name = "USBIN", .func = 0x13, .found = false };
static struct charger_module typec_mod = { .name = "TYPEC", .func = 0x15, .found = false };
static vm_offset_t arb_core, arb_cnfg, arb_obsrvr;

#define SDA 0
#define SCL 1
#define OUTPUT (1u << 9)
#define GAUGE_ADDRESS 0x55
#define CHARGE_FLOW_UA 50000        /* 50 mA */
#define DISCONNECT_FLOW_UA (-150000) /* -150 mA */
static vm_offset_t pins, dwc3_dev;
static uint32_t saved_cfg[2], saved_io[2], input_cfg[2];
static bool bus_ok;
static bool last_external = false;

static uint32_t
rd(unsigned pin, unsigned offset)
{
	return *(volatile uint32_t *)(pins + pin * 0x1000 + offset);
}

static void
wr(unsigned pin, unsigned offset, uint32_t value)
{
	*(volatile uint32_t *)(pins + pin * 0x1000 + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
pause_bus(void)
{
	delay_for_interval(5, NSEC_PER_USEC);
}

/* A high bit releases the line; the driver only drives low. */
static void
line(unsigned pin, bool high)
{
	wr(pin, 0, input_cfg[pin] | (high ? 0 : OUTPUT));
	pause_bus();
}

static bool
clock_high(void)
{
	line(SCL, true);
	/* The gauge may stretch SCL for 40.08 ms during its own flash update. */
	for (unsigned i = 0; i < 10000; i++) {
		if (rd(SCL, 4) & 1) { return true; }
		pause_bus();
	}
	bus_ok = false;
	return false;
}

static bool
start(void)
{
	line(SDA, true);
	if (!clock_high() || !(rd(SDA, 4) & 1)) { bus_ok = false; return false; }
	line(SDA, false);
	line(SCL, false);
	return true;
}

static void
stop(void)
{
	line(SCL, false);
	line(SDA, false);
	clock_high();
	line(SDA, true);
}

static bool
send_byte(uint8_t value)
{
	for (unsigned bit = 0; bit < 8; bit++) {
		line(SDA, (value & 0x80) != 0);
		if (!clock_high()) { return false; }
		line(SCL, false);
		value <<= 1;
	}
	line(SDA, true);
	if (!clock_high()) { return false; }
	bool acknowledged = !(rd(SDA, 4) & 1);
	line(SCL, false);
	return acknowledged;
}

static uint8_t
read_byte(bool acknowledge)
{
	uint8_t value = 0;
	line(SDA, true);
	for (unsigned bit = 0; bit < 8; bit++) {
		if (!clock_high()) { return 0; }
		value = (value << 1) | (rd(SDA, 4) & 1);
		line(SCL, false);
	}
	line(SDA, !acknowledge);
	clock_high();
	line(SCL, false);
	line(SDA, true);
	return value;
}

static bool
read_word(uint8_t reg, uint16_t *value)
{
	uint8_t low = 0, high = 0;
	bus_ok = true;
	bool acknowledged = start() && send_byte(GAUGE_ADDRESS << 1) && send_byte(reg) &&
	    start() && send_byte((GAUGE_ADDRESS << 1) | 1);
	if (acknowledged) {
		low = read_byte(true);
		high = read_byte(false);
	}
	stop();
	*value = low | ((uint16_t)high << 8);
	return acknowledged && bus_ok;
}

static uint32_t
spmi_wait(vm_offset_t channel)
{
	uint32_t status = 0;
	for (unsigned i = 0; i < 2000; i++) {
		status = *(volatile uint32_t *)(channel + CH_STATUS);
		if (status & (STATUS_DONE | STATUS_DENIED | STATUS_FAILURE | STATUS_DROPPED)) {
			break;
		}
		delay_for_interval(1, NSEC_PER_USEC);
	}
	return status;
}

static bool
spmi_read_reg(struct charger_module *mod, uint8_t reg, uint8_t *val)
{
	if (!mod->found || mod->chnl_rd == 0) {
		return false;
	}
	annibale_spmi_lock();
	*(volatile uint32_t *)(mod->chnl_rd + CH_CMD) = CMD_OPC(OPC_EXT_READ) | CMD_REG(reg) | 0;
	uint32_t status = spmi_wait(mod->chnl_rd);
	uint32_t data = *(volatile uint32_t *)(mod->chnl_rd + CH_RDATA0);
	annibale_spmi_unlock();

	if ((status & STATUS_DONE) && !(status & (STATUS_DENIED | STATUS_FAILURE | STATUS_DROPPED))) {
		*val = (uint8_t)(data & 0xff);
		return true;
	}
	return false;
}

static bool
spmi_write_reg(struct charger_module *mod, uint8_t reg, uint8_t val)
{
	if (!mod->found || !mod->ours || mod->chnl_wr == 0) {
		return false;
	}
	annibale_spmi_lock();
	for (unsigned i = 0; i < 1000; i++) {
		uint32_t st = *(volatile uint32_t *)(mod->chnl_wr + CH_STATUS);
		if ((st & (STATUS_DONE | STATUS_DROPPED | STATUS_FAILURE | STATUS_DENIED)) != 0 || st == 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_USEC);
	}
	*(volatile uint32_t *)(mod->chnl_wr + CH_WDATA0) = val;
	*(volatile uint32_t *)(mod->chnl_wr + CH_CMD) = CMD_OPC(OPC_EXT_WRITEL) | CMD_REG(reg) | 0;
	uint32_t status = spmi_wait(mod->chnl_wr);
	annibale_spmi_unlock();

	if (!(status & STATUS_DONE) || (status & (STATUS_DENIED | STATUS_FAILURE | STATUS_DROPPED))) {
		return false;
	}
	return true;
}

static bool
spmi_write_verify_reg(struct charger_module *mod, uint8_t reg, uint8_t val)
{
	if (!mod->found) {
		printf("battery: cannot write %s reg %#02x: module not found on SPMI bus\n", mod->name, reg);
		return false;
	}
	if (!mod->ours || mod->chnl_wr == 0) {
		printf("battery: cannot write %s reg %#02x: channel %u owned by system %u (not writable by APPS)\n",
		       mod->name, reg, mod->apid, mod->owner);
		return false;
	}
	if (!spmi_write_reg(mod, reg, val)) {
		printf("battery: %s reg %#02x write %#02x failed (arbiter status error or timeout)\n",
		       mod->name, reg, val);
		return false;
	}
	uint8_t readback = 0;
	if (!spmi_read_reg(mod, reg, &readback)) {
		printf("battery: %s reg %#02x write %#02x readback failed\n", mod->name, reg, val);
		return false;
	}
	if (readback != val) {
		delay_for_interval(1, NSEC_PER_MSEC);
		if (!spmi_read_reg(mod, reg, &readback) || readback != val) {
			printf("battery: %s reg %#02x write %#02x readback mismatch: expected %#02x, got %#02x\n",
			       mod->name, reg, val, val, readback);
			return false;
		}
	}
	return true;
}

static void
pm8550b_charger_probe(void)
{
	if (arb_core == 0) {
		arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
		arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
		arb_obsrvr = ml_io_map(ARB_OBSRV_PHYS, ARB_OBSRV_SIZE);
	}
	if (arb_core == 0 || arb_cnfg == 0 || arb_obsrvr == 0) {
		printf("battery: failed to map SPMI arbiter windows\n");
		return;
	}
	uint32_t version = *(volatile uint32_t *)(arb_core + 0);
	if (version < ARB_VERSION_7) {
		printf("battery: SPMI arbiter version %#x not supported\n", version);
		return;
	}
	uint32_t channels = *(volatile uint32_t *)(arb_core + 4) & 0x7ff;
	if (channels > APID_MAX) {
		channels = APID_MAX;
	}

	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = *(volatile uint32_t *)(arb_core + APID_MAP(n));
		if (map == 0) {
			continue;
		}
		uint16_t ppid = (map >> 8) & 0xfff;
		uint8_t slave = (ppid >> 8) & 0x0f;
		uint8_t func = ppid & 0xff;
		uint8_t owner = *(volatile uint32_t *)(arb_cnfg + APID_OWNER(n)) & 7;

		/*
		 * Exclude PMK8550/PM8550 power-on block (0x013 and 0x113) and PON PBS (0x015)
		 * which are power-on reset peripherals, not charger peripherals.
		 */
		if (ppid == 0x013 || ppid == 0x113 || ppid == 0x015) {
			continue;
		}

		if (func == chgr_mod.func) {
			if (!chgr_mod.found || (!chgr_mod.ours && owner == ARB_SYSTEM)) {
				chgr_mod.apid = (uint16_t)n;
				chgr_mod.ppid = ppid;
				chgr_mod.slave = slave;
				chgr_mod.owner = owner;
				chgr_mod.ours = (owner == ARB_SYSTEM);
				chgr_mod.found = true;
				chgr_mod.chnl_rd = arb_obsrvr + 0x8000UL * ARB_SYSTEM + 0x20UL * n;
				if (chgr_mod.ours && chgr_mod.chnl_wr == 0) {
					chgr_mod.chnl_wr = ml_io_map(ARB_CHNLS_PHYS + ARB_CHANNEL_SIZE * n, ARB_CHANNEL_SIZE);
				}
				printf("battery: found CHGR at channel %u (ppid %#05x, slave %u, owner %u, writable=%d)\n",
				       n, ppid, slave, owner, chgr_mod.ours);
			}
		} else if (func == usbin_mod.func) {
			if (!usbin_mod.found || (!usbin_mod.ours && owner == ARB_SYSTEM)) {
				usbin_mod.apid = (uint16_t)n;
				usbin_mod.ppid = ppid;
				usbin_mod.slave = slave;
				usbin_mod.owner = owner;
				usbin_mod.ours = (owner == ARB_SYSTEM);
				usbin_mod.found = true;
				usbin_mod.chnl_rd = arb_obsrvr + 0x8000UL * ARB_SYSTEM + 0x20UL * n;
				if (usbin_mod.ours && usbin_mod.chnl_wr == 0) {
					usbin_mod.chnl_wr = ml_io_map(ARB_CHNLS_PHYS + ARB_CHANNEL_SIZE * n, ARB_CHANNEL_SIZE);
				}
				printf("battery: found USBIN at channel %u (ppid %#05x, slave %u, owner %u, writable=%d)\n",
				       n, ppid, slave, owner, usbin_mod.ours);
			}
		} else if (func == typec_mod.func) {
			if (!typec_mod.found || (!typec_mod.ours && owner == ARB_SYSTEM)) {
				typec_mod.apid = (uint16_t)n;
				typec_mod.ppid = ppid;
				typec_mod.slave = slave;
				typec_mod.owner = owner;
				typec_mod.ours = (owner == ARB_SYSTEM);
				typec_mod.found = true;
				typec_mod.chnl_rd = arb_obsrvr + 0x8000UL * ARB_SYSTEM + 0x20UL * n;
				if (typec_mod.ours && typec_mod.chnl_wr == 0) {
					typec_mod.chnl_wr = ml_io_map(ARB_CHNLS_PHYS + ARB_CHANNEL_SIZE * n, ARB_CHANNEL_SIZE);
				}
				printf("battery: found TYPEC at channel %u (ppid %#05x, slave %u, owner %u, writable=%d)\n",
				       n, ppid, slave, owner, typec_mod.ours);
			}
		}
	}
	if (!chgr_mod.found) {
		printf("battery: CHGR peripheral (func %#02x) not found on SPMI bus\n", chgr_mod.func);
	}
	if (!usbin_mod.found) {
		printf("battery: USBIN peripheral (func %#02x) not found on SPMI bus\n", usbin_mod.func);
	}
	if (!typec_mod.found) {
		printf("battery: TYPEC peripheral (func %#02x) not found on SPMI bus\n", typec_mod.func);
	}
}

static bool
pm8550b_configure_charging(bool enable)
{
	static int current_state = -1;
	static int last_attempt = -1;

	if (current_state == (int)enable) {
		return true;
	}

	if (!chgr_mod.found && !usbin_mod.found && !typec_mod.found) {
		if (last_attempt != (int)enable) {
			last_attempt = (int)enable;
			printf("battery: no PM8550B charger peripherals available on SPMI bus\n");
		}
		return false;
	}

	bool success = true;

	if (enable) {
		if (usbin_mod.found) {
			if (!spmi_write_verify_reg(&usbin_mod, 0x40, 0x00) ||
			    !spmi_write_verify_reg(&usbin_mod, 0x70, 0x3c)) {
				success = false;
			}
		}
		if (chgr_mod.found) {
			if (!spmi_write_verify_reg(&chgr_mod, 0x42, 0x01) ||
			    !spmi_write_verify_reg(&chgr_mod, 0x61, 0x32)) {
				success = false;
			}
		}
		if (typec_mod.found) {
			if (!spmi_write_verify_reg(&typec_mod, 0x44, 0x01)) {
				success = false;
			}
		}
		if (success) {
			current_state = 1;
			last_attempt = 1;
			printf("battery: PM8550B charging enabled (ICL 3000 mA, FastChg 2500 mA)\n");
		} else {
			if (last_attempt != 1) {
				last_attempt = 1;
				printf("battery: PM8550B charging enable failed (writes unverified or denied)\n");
			}
		}
	} else {
		if (chgr_mod.found) {
			if (!spmi_write_verify_reg(&chgr_mod, 0x42, 0x00)) {
				success = false;
			}
		}
		if (usbin_mod.found) {
			if (!spmi_write_verify_reg(&usbin_mod, 0x70, 0x0a)) {
				success = false;
			}
		}
		if (success) {
			current_state = 0;
			last_attempt = 0;
			printf("battery: PM8550B charging disabled (standby 500 mA)\n");
		} else {
			if (last_attempt != 0) {
				last_attempt = 0;
				printf("battery: PM8550B charging disable failed\n");
			}
		}
	}
	return success;
}

static bool
sample(void)
{
	static const uint8_t registers[] = {0x2c, 0x08, 0x0c, 0x06, 0x10, 0x12, 0x3c, 0x2a};
	uint16_t values[sizeof(registers)] = {0};
	unsigned count = 0;
	for (unsigned pin = 0; pin < 2; pin++) {
		saved_cfg[pin] = rd(pin, 0);
		saved_io[pin] = rd(pin, 4) & 2;
		input_cfg[pin] = (saved_cfg[pin] & ~(0x3fu | OUTPUT)) | 3;
		wr(pin, 0, input_cfg[pin]);
		wr(pin, 4, 0);
	}
	/* Only take an idle bus; the charge pump at 0x6e shares these pins. */
	if ((rd(SDA, 4) & 1) && (rd(SCL, 4) & 1)) {
		for (; count < sizeof(registers); count++) {
			if (!read_word(registers[count], &values[count])) { break; }
		}
	}
	line(SDA, true);
	line(SCL, true);
	for (unsigned pin = 0; pin < 2; pin++) {
		wr(pin, 4, saved_io[pin]);
		wr(pin, 0, saved_cfg[pin]);
	}
	if (count != sizeof(registers)) {
		printf("battery: BQ27Z561 read failed at command %#x (%u completed)\n", registers[count], count);
		return false;
	}
	if (values[0] > 100 || values[1] < 2500 || values[1] > 5000 || values[3] < 2331 || values[3] > 3582) {
		printf("battery: BQ27Z561 invalid sample: SOC=%u voltage=%u temperature=%u\n", values[0], values[1], values[3]);
		return false;
	}
	struct redwood_battery_state state = {0};
	state.size = sizeof(state);
	state.percent_hundredths = values[0] * 100;
	state.voltage_uv = values[1] * 1000;
	state.current_ua = (int16_t)values[2] * 1000;
	state.temperature_hundredths = values[3] * 10 - 27315;
	state.present = 1;
	/* Current confirms energy flow; USB connection and limits need a separate source. On USB
	 * near full charge the gauge reads within a few tens of mA of 0 either way (+17 mA once in
	 * kernel-12377-cmd47-bo42), so flow under CHARGE_FLOW_UA counts as none. Running iOS draws
	 * hundreds of mA from the battery, so a battery that is not being drained is on external power:
	 * a charger that holds a full battery shows as plugged in and not charging (status 3), which
	 * RedwoodBattery reports as charging, as an iPhone shows it. A supply weaker than the phone's
	 * draw (a computer's USB port) still reads as discharging. */
	/* Hysteresis window [-250 mA, +50 mA] to eliminate toggling around 0 mA:
	 * > +50 mA: battery is actively charging from external power
	 * < -250 mA: battery is actively discharging, external power disconnected
	 * [-250 mA, +50 mA]: maintain current external state */
	bool usb_active = false;
	if (dwc3_dev != 0) {
		uint32_t dctl = *(volatile uint32_t *)(dwc3_dev + 0x704);
		uint32_t dsts = *(volatile uint32_t *)(dwc3_dev + 0x70c);
		usb_active = ((dctl >> 31) != 0) && ((dsts & (1u << 22)) == 0) && (((dsts >> 18) & 0xf) < 4);
	}
	bool usbin_plugged = false;
	uint8_t usbin_sts = 0;
	if (usbin_mod.found && spmi_read_reg(&usbin_mod, 0x10, &usbin_sts)) {
		usbin_plugged = (usbin_sts & 0x03) != 0;
	}
	if (usb_active || usbin_plugged || state.current_ua >= DISCONNECT_FLOW_UA) {
		last_external = true;
		state.status = (values[0] >= 100 && state.current_ua <= CHARGE_FLOW_UA) ? 4 : 1;
		pm8550b_configure_charging(true);
	} else {
		last_external = false;
		state.status = 2;
		pm8550b_configure_charging(false);
	}
	state.external = last_external;
	if (state.external) {
		state.usb_type = (usb_active || usbin_plugged) ? 1 : 2;
		state.usb_voltage_uv = 5000000;
		state.usb_current_ua = (usb_active || usbin_plugged) && state.current_ua <= CHARGE_FLOW_UA ? 500000 : 2000000;
	}
	state.charge_uah = values[4] * 1000;
	state.full_uah = values[5] * 1000;
	state.design_uah = values[6] * 1000;
	state.cycles = values[7];
	redwood_battery_report(&state);
	int fraction = state.temperature_hundredths % 100;
	printf("battery: BQ27Z561 SOC=%u%% V=%u mV I=%d mA T=%d.%02d C\n", values[0], values[1],
	    (int16_t)values[2], state.temperature_hundredths / 100, fraction < 0 ? -fraction : fraction);
	return true;
}

static void
bus_reinit(void)
{
	for (unsigned pin = 0; pin < 2; pin++) {
		saved_cfg[pin] = rd(pin, 0);
		saved_io[pin] = rd(pin, 4) & 2;
		input_cfg[pin] = (saved_cfg[pin] & ~(0x3fu | OUTPUT)) | 3;
		wr(pin, 0, input_cfg[pin]);
		wr(pin, 4, 0);
	}
	line(SDA, true);
	for (unsigned i = 0; i < 9; i++) {
		line(SCL, false);
		pause_bus();
		line(SCL, true);
		pause_bus();
		if (rd(SDA, 4) & 1) {
			break;
		}
	}
	stop();
	for (unsigned pin = 0; pin < 2; pin++) {
		wr(pin, 4, saved_io[pin]);
		wr(pin, 0, saved_cfg[pin]);
	}
	delay_for_interval(100, NSEC_PER_MSEC);
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("battery: BQ27Z561 on GPIO52/53, address %#x; reading standard commands\n", GAUGE_ADDRESS);
	pins = ml_io_map(0x0f134000UL, 0x2000);
	dwc3_dev = ml_io_map(0x0a60c000UL, 0x1000);
	pm8550b_charger_probe();
	if (pins) {
		for (;;) {
			if (!sample()) {
				bus_reinit();
				if (sample()) {
					delay_for_interval(5000, NSEC_PER_MSEC);
					continue;
				}
			}
			delay_for_interval(5000, NSEC_PER_MSEC);
		}
	}
	printf("battery: failed to map pins\n");
	thread_terminate(current_thread());
}

void
annibale_battery_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;
	DTEntry entry;
	const void *property;
	unsigned size;
	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) { return; }
	if (pe_arm_get_soc_base_phys() != 0x400000 ||
	    SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "reg", &property, &size) != kSuccess || size != 4 * sizeof(uint64_t)) { return; }
	const uint64_t *reg = property;
	if (reg[0] != 0x15c00000 || reg[2] != 0x15c80000 || reg[3] != 0x200000) { return; }
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) { thread_deallocate(thread); }
}

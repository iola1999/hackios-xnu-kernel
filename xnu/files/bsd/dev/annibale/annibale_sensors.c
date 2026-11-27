/*
 * Ambient light sensor (ALS) and proximity sensor driver for REDMI K90 (annibale, SM8750 "sun").
 *
 * Hardware topology from the phone's persist/sensors/registry configuration:
 *   - Front ALS + Proximity: gls6151 (sm8750_gls6151.json.glsx_platform.config)
 *     bus_type = 0 (I2C), bus_instance = 13 (QUPv3 SE12 at 0x00890000), slave = 0x31 (49),
 *     400 kHz, DRI IRQ = TLMM GPIO 163, reg_addr_type = 1.
 *     Factory proximity calibration (PSensor_ct1.txt & fac_cal):
 *       crosstalk offset = 585, near_threshold = 1041, far_threshold = 747.
 *     Factory ALS calibration (Lux_F_channel_data.txt):
 *       channel scale factor ~ 1.034x (1034 milli-units).
 *   - Rear ALS: stk6a2x_back (sm8750_stk6a2x_back.json.stk6a2x_back_0_platform.config)
 *     bus_type = 0 (I2C), bus_instance = 11 (QUPv3 SE10 at 0x00888000), slave = 0x47 (71),
 *     400 kHz, DRI IRQ = TLMM GPIO 35, reg_addr_type = 0.
 *
 * Bus access: open-drain GPIO bitbang (bitbang) on the SE pin pairs, the same technique the front
 * camera driver validated on-device on SE9 (annibale_camera_hw.inc). It needs neither the GCC
 * branch clocks nor the GENI firmware: the AP QUPv3 wrapper-2 controllers are disabled in the
 * Android device tree and the buses belong to the sensor island, whose clocks stay off while no
 * SSC client subscribes. The SE clock halt registers are still observed before any bitbang; seen
 * activity means another master may own the bus and the hardware path stays disengaged.
 *
 * Clock offsets are the SM8750 voting-model values of mainline gcc-sm8750.c (BRANCH_HALT_VOTED):
 * the branch registers are halt-status-only, the enable votes live in the AP voting register.
 *
 * Userland contract:
 *   - /dev/annibale-sensors character device (read, write, ioctl)
 *   - sysctl kern.annibale.als_lux (RW) and kern.annibale.prox_near (RW)
 *   - annibale_sensors_report() for ADSP SSC / kernel sensor updates
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/annibale_input.h>
#include <sys/annibale_rpmh.h>
#include <sys/annibale_sensors.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void loader_log_line(const char *line);
extern void loader_log_line_sync(const char *line);
extern bool disable_serial_output;
extern bool disable_kprintf_output;

SYSCTL_DECL(_kern_annibale);

/* ---- SM8750 GCC and TLMM physical addresses ------------------------------------------------------ */
/* mainline gcc-sm8750.c: the wrapper-2 branches are voted clocks. The halt registers below are
 * read-only status (bit 31 CLK_OFF, bits 30:28 NoC FSM status); the AP's enable votes are in
 * GCC_QUPV3_WRAP2_VOTE (bit1 s_ahb, bit2 m_ahb, bit0 core, bit3 core_2x, bit6 SE10/s2, bit8 SE12/s4). */

#define GCC_BASE_PHYS                   0x00100000UL
#define GCC_MAP_SIZE                    0x00060000UL
#define GCC_QUPV3_WRAP2_VOTE            0x52010UL       /* the application processor's votes */
#define GCC_QUPV3_WRAP2_M_AHB_HALT      0x23298UL
#define GCC_QUPV3_WRAP2_S_AHB_HALT      0x2329cUL
#define GCC_QUPV3_WRAP2_S2_HALT         0x1e27cUL       /* SE10 (rear ALS stk6a2x) */
#define GCC_QUPV3_WRAP2_S4_HALT         0x1e4f4UL       /* SE12 (front ALS+Prox gls6151) */

#define TLMM_BASE_PHYS                  0x0f100000UL
#define TLMM_MAP_SIZE                   0x00100000UL
#define TLMM_GPIO_CFG(pin)              (0x1000UL * (uint32_t)(pin))
#define TLMM_GPIO_IN_OUT(pin)           (0x1000UL * (uint32_t)(pin) + 0x04UL)

/* gls6151 register offsets, 8-bit register numbers and not yet verified on-device; the chip's
 * persist registry reg_addr_type = 1 hints at 16-bit register addressing, which the driver and
 * the probes both try. Layout matches a low-byte-first Sensortek-family arrangement. */
#define GLS6151_REG_SYSM_CTRL           0x00
#define GLS6151_REG_INT_FLAG            0x01
#define GLS6151_REG_PS_DATA_LOW         0x02
#define GLS6151_REG_ALS_CLEAR_LOW       0x04
#define GLS6151_REG_ALS_IR_LOW          0x06
#define GLS6151_REG_PRODUCT_ID          0x3e
#define GLS6151_STATE_EN_PS_ALS         0x03            /* Sensortek family: bit0 PS, bit1 ALS */

/* stk6a2x register offsets (mainline drivers/iio/light/stk3310.c) */
#define STK6A2X_REG_STATE               0x00
#define STK6A2X_REG_PS_DATA_MSB         0x11
#define STK6A2X_REG_ALS_DATA_MSB        0x13
#define STK6A2X_REG_PDT_ID              0x3e
#define STK6A2X_STATE_EN_PS_ALS         0x03

static LCK_GRP_DECLARE(sensors_lck_grp, "annibale-sensors");
static LCK_MTX_DECLARE(sensors_lck, &sensors_lck_grp);

static vm_offset_t gcc_base, tlmm_base;
static bool initialized;
static int hw_i2c_mode;
static int probe_mode;

/* Bitbang driver state, written once at init and then read-only in the poll loop. */
static bool front_online, back_online;
static bool front_reg16, back_reg16;
static bool hw_refused;         /* SE clock activity seen: another master may own the buses */

static struct annibale_sensors_state sensor_state = {
	.sample = {
		.timestamp = 0,
		.als_lux = 150,
		.clear_raw = 600,
		.ir_raw = 120,
		.prox_raw = ANNIBALE_GLS6151_PROX_OFFSET,
		.prox_distance_mm = 50,
		.prox_near = 0,
		.source_flags = ANNIBALE_SENSOR_SRC_DEFAULT,
	},
	.back_als_lux = 120,
	.prox_crosstalk = ANNIBALE_GLS6151_PROX_OFFSET,
	.prox_near_thresh = ANNIBALE_GLS6151_PROX_NEAR,
	.prox_far_thresh = ANNIBALE_GLS6151_PROX_FAR,
	.als_scale_milli = 1034,
	.front_irq_gpio = 1,
	.back_irq_gpio = 1,
	.poll_count = 0,
};

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (!ml_get_interrupts_enabled() ||
	    disable_serial_output || disable_kprintf_output) {
		loader_log_line(line);
	} else {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

#include "annibale_sensors_i2c.inc"
#include "annibale_sensors_probe.inc"
#include "annibale_sensors_scan.inc"

/* ---- bitbang hardware driver ------------------------------------------------------------------------ */

static bool
sns_read_regs(uint32_t sda, uint32_t scl, uint8_t slave, bool reg16, uint8_t reg,
    uint8_t *buf, uint32_t len)
{
	uint8_t reg8[1] = { reg };
	uint8_t reg16b[2] = { 0x00, reg };

	return od_i2c_read_regs(sda, scl, slave, reg16 ? reg16b : reg8, reg16 ? 2 : 1, buf, len);
}

static bool
sns_write_reg(uint32_t sda, uint32_t scl, uint8_t slave, bool reg16, uint8_t reg, uint8_t value)
{
	uint8_t write8[2] = { reg, value };
	uint8_t write16[3] = { 0x00, reg, value };

	return od_i2c_write_bytes(sda, scl, slave, reg16 ? write16 : write8, reg16 ? 3 : 2);
}

/*
 * Detect one chip: bare address ACK, then the product-ID read decides the register addressing
 * width (8-bit first, then 16-bit). True when the ID read succeeds in either width.
 */
static bool
sns_detect(uint32_t sda, uint32_t scl, uint8_t slave, const char *name, bool *reg16_out, bool verbose)
{
	uint8_t id = 0;

	if (!od_i2c_ack_probe(sda, scl, slave)) {
		if (verbose) {
			say("sensors: %s 0x%02x NACK\n", name, slave);
		}
		return false;
	}
	for (uint32_t width = 0; width < 2; width++) {
		bool reg16 = width != 0;
		if (sns_read_regs(sda, scl, slave, reg16, 0x3e, &id, 1)) {   /* both chips keep their id at 0x3e */
			*reg16_out = reg16;
			say("sensors: %s 0x%02x ACK, id@0x3e=%#x (%u-bit addressing)\n",
			    name, slave, id, reg16 ? 16u : 8u);
			return true;
		}
	}
	if (verbose) {
		say("sensors: %s 0x%02x ACKs its address but the id read fails in both widths\n", name, slave);
	}
	return false;
}

/* Detect one chip and write its Sensortek-family enable sequence. Power-on after a rail vote
 * takes time the registry does not specify, so the poll loop reruns this for missing chips. */
static bool
sns_bring_up(uint32_t sda, uint32_t scl, uint8_t slave, bool *reg16_io, const char *name,
    uint8_t state_reg, uint8_t state_enable, bool verbose)
{
	if (!sns_detect(sda, scl, slave, name, reg16_io, verbose)) {
		return false;
	}
	bool ok = sns_write_reg(sda, scl, slave, *reg16_io, state_reg, state_enable);
	if (verbose || ok) {
		say("sensors: %s enable ALS+PS %s\n", name, ok ? "written" : "WRITE NACK");
	}
	return ok;
}

/*
 * Engage the hardware path: refuse when an SE clock was seen running, vote any boot-arg rails,
 * let the rails settle, then bring up both chips. Chips missing here are retried in the poll loop.
 */
static void
sensors_hw_init(void)
{
	if (sns_se_clock_active_scan(1000, 1000)) {
		hw_refused = true;
		say("sensors: SE10/SE12 clock activity seen, bitbang refused (another master may own the bus)\n");
		return;
	}
	sns_bus_idle(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO);
	sns_bus_idle(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO);

	for (uint32_t i = 0; i < probe_rail_count; i++) {
		sns_rail_vote(probe_rails[i].name, probe_rails[i].mv);
	}
	if (probe_rail_count != 0) {
		delay_for_interval(20, NSEC_PER_MSEC);
	}

	front_online = sns_bring_up(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO,
	    ANNIBALE_GLS6151_I2C_ADDR, &front_reg16, "gls6151", GLS6151_REG_SYSM_CTRL, GLS6151_STATE_EN_PS_ALS, true);
	back_online = sns_bring_up(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO,
	    ANNIBALE_STK6A2X_I2C_ADDR, &back_reg16, "stk6a2x", STK6A2X_REG_STATE, STK6A2X_STATE_EN_PS_ALS, true);
	say("sensors: bitbang driver front=%u back=%u\n", front_online ? 1u : 0u, back_online ? 1u : 0u);
}

static void
update_proximity_from_raw_locked(uint32_t prox_raw, uint8_t src_flag)
{
	uint8_t near = sensor_state.sample.prox_near;

	sensor_state.sample.prox_raw = prox_raw;
	if (prox_raw >= sensor_state.prox_near_thresh) {
		near = 1;
	} else if (prox_raw <= sensor_state.prox_far_thresh) {
		near = 0;
	}
	sensor_state.sample.prox_near = near;
	if (near) {
		sensor_state.sample.prox_distance_mm = 0;
	} else if (prox_raw <= sensor_state.prox_crosstalk) {
		sensor_state.sample.prox_distance_mm = 50;
	} else {
		uint32_t span = sensor_state.prox_near_thresh > sensor_state.prox_crosstalk ?
		    sensor_state.prox_near_thresh - sensor_state.prox_crosstalk : 1;
		uint32_t delta = prox_raw - sensor_state.prox_crosstalk;
		uint32_t mm = delta >= span ? 1 : 50 - (delta * 49 / span);
		sensor_state.sample.prox_distance_mm = (uint16_t)mm;
	}
	sensor_state.sample.source_flags |= src_flag;
	sensor_state.sample.timestamp = mach_absolute_time();
}

static void
poll_hardware_sensors(void)
{
	uint32_t front_in = 1, back_in = 1;
	uint8_t raw[6] = { 0 };

	if (tlmm_base != 0) {
		front_in = rd(tlmm_base + TLMM_GPIO_IN_OUT(ANNIBALE_GLS6151_IRQ_GPIO)) & 0x1;
		back_in = rd(tlmm_base + TLMM_GPIO_IN_OUT(ANNIBALE_STK6A2X_IRQ_GPIO)) & 0x1;
	}
	lck_mtx_lock(&sensors_lck);
	sensor_state.front_irq_gpio = front_in;
	sensor_state.back_irq_gpio = back_in;
	sensor_state.poll_count++;
	if (sensor_state.sample.timestamp == 0) {
		sensor_state.sample.timestamp = mach_absolute_time();
	}
	lck_mtx_unlock(&sensors_lck);

	if (hw_i2c_mode <= 0 || hw_refused) {
		return;
	}
	/* Chips that missed the boot bring-up (late rail power, slow power-on) get a retry every
	 * two seconds; an ACK probe of a missing chip costs under a millisecond. */
	if ((!front_online || !back_online) && (sensor_state.poll_count % 8u) == 1u) {
		if (!front_online) {
			front_online = sns_bring_up(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO,
			    ANNIBALE_GLS6151_I2C_ADDR, &front_reg16, "gls6151", GLS6151_REG_SYSM_CTRL, GLS6151_STATE_EN_PS_ALS, false);
		}
		if (!back_online) {
			back_online = sns_bring_up(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO,
			    ANNIBALE_STK6A2X_I2C_ADDR, &back_reg16, "stk6a2x", STK6A2X_REG_STATE, STK6A2X_STATE_EN_PS_ALS, false);
		}
	}
	if (front_online && sns_read_regs(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO,
	    ANNIBALE_GLS6151_I2C_ADDR, front_reg16, GLS6151_REG_PS_DATA_LOW, raw, 6)) {
		uint32_t ps = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8);
		uint32_t clr = (uint32_t)raw[2] | ((uint32_t)raw[3] << 8);
		uint32_t ir = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8);
		uint32_t lux = (clr * sensor_state.als_scale_milli) / 4000;

		lck_mtx_lock(&sensors_lck);
		sensor_state.sample.clear_raw = clr;
		sensor_state.sample.ir_raw = ir;
		sensor_state.sample.als_lux = lux;
		update_proximity_from_raw_locked(ps, ANNIBALE_SENSOR_SRC_HW_I2C | ANNIBALE_SENSOR_SRC_FRONT_OK);
		lck_mtx_unlock(&sensors_lck);
	}
	if (back_online && sns_read_regs(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO,
	    ANNIBALE_STK6A2X_I2C_ADDR, back_reg16, STK6A2X_REG_ALS_DATA_MSB, raw, 2)) {
		uint32_t back_raw = ((uint32_t)raw[0] << 8) | (uint32_t)raw[1];

		lck_mtx_lock(&sensors_lck);
		sensor_state.back_als_lux = (back_raw * 678) / 1000;
		sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_BACK_OK;
		lck_mtx_unlock(&sensors_lck);
	}
}

void
annibale_sensors_report(uint32_t als_lux, uint32_t clear_raw, uint32_t ir_raw,
    uint32_t prox_raw, uint8_t prox_near, uint8_t source_flags)
{
	lck_mtx_lock(&sensors_lck);
	sensor_state.sample.als_lux = als_lux;
	sensor_state.sample.clear_raw = clear_raw;
	sensor_state.sample.ir_raw = ir_raw;
	if (prox_raw != 0) {
		update_proximity_from_raw_locked(prox_raw, source_flags);
	} else {
		sensor_state.sample.prox_near = prox_near ? 1 : 0;
		sensor_state.sample.prox_distance_mm = prox_near ? 0 : 50;
		sensor_state.sample.prox_raw = prox_near ? sensor_state.prox_near_thresh + 200 : sensor_state.prox_crosstalk;
		sensor_state.sample.source_flags |= source_flags;
		sensor_state.sample.timestamp = mach_absolute_time();
	}
	lck_mtx_unlock(&sensors_lck);
}

/* ---- sysctl handlers: kern.annibale.als_lux and kern.annibale.prox_near ---------------------- */

static int
sysctl_als_lux(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value, error;

	lck_mtx_lock(&sensors_lck);
	value = (int)sensor_state.sample.als_lux;
	lck_mtx_unlock(&sensors_lck);

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (value < 0 || value > 100000) {
		return EINVAL;
	}
	lck_mtx_lock(&sensors_lck);
	sensor_state.sample.als_lux = (uint32_t)value;
	sensor_state.sample.clear_raw = (uint32_t)value * 4;
	sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_INJECTED;
	sensor_state.sample.timestamp = mach_absolute_time();
	lck_mtx_unlock(&sensors_lck);
	say("sensors: sysctl kern.annibale.als_lux set to %d lux\n", value);
	return 0;
}

static int
sysctl_prox_near(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value, error;

	lck_mtx_lock(&sensors_lck);
	value = (int)sensor_state.sample.prox_near;
	lck_mtx_unlock(&sensors_lck);

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	lck_mtx_lock(&sensors_lck);
	sensor_state.sample.prox_near = value ? 1 : 0;
	sensor_state.sample.prox_distance_mm = value ? 0 : 50;
	sensor_state.sample.prox_raw = value ? (sensor_state.prox_near_thresh + 200) : sensor_state.prox_crosstalk;
	/* INJECTED stays clear: it covers ALS data only (annibale_sensors.h). */
	sensor_state.sample.timestamp = mach_absolute_time();
	lck_mtx_unlock(&sensors_lck);
	say("sensors: sysctl kern.annibale.prox_near set to %d\n", value ? 1 : 0);
	return 0;
}

SYSCTL_PROC(_kern_annibale, OID_AUTO, als_lux, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    NULL, 0, sysctl_als_lux, "I", "ambient light sensor illuminance in lux");
SYSCTL_PROC(_kern_annibale, OID_AUTO, prox_near, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    NULL, 0, sysctl_prox_near, "I", "proximity sensor state (0 = far, 1 = near)");

/* ---- /dev/annibale-sensors character device ------------------------------------------------- */

static int
sensors_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
sensors_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct annibale_sensors_state snapshot;
	user_ssize_t resid = uio_resid(uio);

	if (resid <= 0) {
		return 0;
	}
	lck_mtx_lock(&sensors_lck);
	if (sensor_state.sample.timestamp == 0) {
		sensor_state.sample.timestamp = mach_absolute_time();
	}
	snapshot = sensor_state;
	lck_mtx_unlock(&sensors_lck);

	if (resid >= (user_ssize_t)sizeof(struct annibale_sensors_state)) {
		return uiomove((caddr_t)&snapshot, sizeof(struct annibale_sensors_state), uio);
	}
	return uiomove((caddr_t)&snapshot.sample,
	    (int)MIN(sizeof(struct annibale_sensor_sample), (size_t)resid), uio);
}

static bool
parse_uint(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	bool any = false;

	while (*s == ' ' || *s == '\t' || *s == '=') {
		s++;
	}
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (uint32_t)(*s - '0');
		s++;
		any = true;
	}
	if (any) {
		*out = v;
	}
	return any;
}

static int
sensors_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	user_ssize_t resid = uio_resid(uio);
	char buf[64] = { 0 };
	int error;

	if (resid == (user_ssize_t)sizeof(struct annibale_sensor_sample)) {
		struct annibale_sensor_sample in_sample;

		if ((error = uiomove((caddr_t)&in_sample, sizeof(in_sample), uio)) != 0) {
			return error;
		}
		lck_mtx_lock(&sensors_lck);
		sensor_state.sample = in_sample;
		if (sensor_state.sample.timestamp == 0) {
			sensor_state.sample.timestamp = mach_absolute_time();
		}
		sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_INJECTED;
		lck_mtx_unlock(&sensors_lck);
		return 0;
	}
	if (resid <= 0 || resid >= (user_ssize_t)sizeof(buf)) {
		return EINVAL;
	}
	if ((error = uiomove(buf, (int)resid, uio)) != 0) {
		return error;
	}
	lck_mtx_lock(&sensors_lck);
	if (strncmp(buf, "near", 4) == 0) {
		sensor_state.sample.prox_near = 1;
		sensor_state.sample.prox_distance_mm = 0;
		sensor_state.sample.prox_raw = sensor_state.prox_near_thresh + 200;
	} else if (strncmp(buf, "far", 3) == 0) {
		sensor_state.sample.prox_near = 0;
		sensor_state.sample.prox_distance_mm = 50;
		sensor_state.sample.prox_raw = sensor_state.prox_crosstalk;
	} else if (strncmp(buf, "prox", 4) == 0) {
		uint32_t v = 0;
		if (parse_uint(buf + 4, &v)) {
			if (v <= 1) {
				sensor_state.sample.prox_near = (uint8_t)v;
				sensor_state.sample.prox_distance_mm = v ? 0 : 50;
				sensor_state.sample.prox_raw = v ? (sensor_state.prox_near_thresh + 200) : sensor_state.prox_crosstalk;
			} else {
				update_proximity_from_raw_locked(v, 0);
			}
		}
	} else if (strncmp(buf, "raw", 3) == 0) {
		uint32_t v = 0;
		if (parse_uint(buf + 3, &v)) {
			update_proximity_from_raw_locked(v, 0);
		}
	} else if (strncmp(buf, "lux", 3) == 0) {
		uint32_t v = 0;
		if (parse_uint(buf + 3, &v) && v <= 100000) {
			sensor_state.sample.als_lux = v;
			sensor_state.sample.clear_raw = v * 4;
			sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_INJECTED;
		}
	} else {
		uint32_t v = 0;
		if (parse_uint(buf, &v) && v <= 100000) {
			sensor_state.sample.als_lux = v;
			sensor_state.sample.clear_raw = v * 4;
			sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_INJECTED;
		}
	}
	/* INJECTED is asserted inside the lux-carrying branches only (annibale_sensors.h):
	 * proximity-only and unparsable writes leave the ALS publication contract untouched. */
	sensor_state.sample.timestamp = mach_absolute_time();
	lck_mtx_unlock(&sensors_lck);
	return 0;
}

static int
sensors_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	switch (cmd) {
	case ANNIBALE_SENSORS_GET_STATE:
		lck_mtx_lock(&sensors_lck);
		if (sensor_state.sample.timestamp == 0) {
			sensor_state.sample.timestamp = mach_absolute_time();
		}
		*(struct annibale_sensors_state *)data = sensor_state;
		lck_mtx_unlock(&sensors_lck);
		return 0;
	case ANNIBALE_SENSORS_SET_STATE:
		lck_mtx_lock(&sensors_lck);
		sensor_state.sample = *(const struct annibale_sensor_sample *)data;
		if (sensor_state.sample.timestamp == 0) {
			sensor_state.sample.timestamp = mach_absolute_time();
		}
		sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_INJECTED;
		lck_mtx_unlock(&sensors_lck);
		return 0;
	case ANNIBALE_SENSORS_CALIBRATE: {
		struct annibale_sensors_state *cal = (struct annibale_sensors_state *)data;

		lck_mtx_lock(&sensors_lck);
		if (cal->prox_crosstalk != 0) {
			sensor_state.prox_crosstalk = cal->prox_crosstalk;
		}
		if (cal->prox_near_thresh != 0) {
			sensor_state.prox_near_thresh = cal->prox_near_thresh;
		}
		if (cal->prox_far_thresh != 0) {
			sensor_state.prox_far_thresh = cal->prox_far_thresh;
		}
		if (cal->als_scale_milli != 0) {
			sensor_state.als_scale_milli = cal->als_scale_milli;
		}
		*cal = sensor_state;
		lck_mtx_unlock(&sensors_lck);
		return 0;
	}
	default:
		return ENOTTY;
	}
}

static const struct cdevsw sensors_cdevsw = {
	.d_open = sensors_open, .d_close = sensors_open, .d_read = sensors_read, .d_write = sensors_write,
	.d_ioctl = sensors_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* sensors_rails=ldob5@1800,bob1@3000: comma-separated cmd-db names with an optional millivolt. */
static void
parse_rails_boot_arg(void)
{
	char arg[96] = { 0 };
	uint32_t count = 0;
	char *cursor = arg;

	if (!PE_parse_boot_arg_str("sensors_rails", arg, sizeof(arg)) || arg[0] == '\0') {
		return;
	}
	while (*cursor != '\0' && count < SNS_PROBE_RAILS_MAX) {
		uint32_t len = 0, mv = 1800;

		while (cursor[len] != '\0' && cursor[len] != ',' && cursor[len] != '@' && len < 8) {
			len++;
		}
		if (len == 0) {
			break;
		}
		memcpy(probe_rails[count].name, cursor, len);
		probe_rails[count].name[len] = '\0';
		cursor += len;
		if (*cursor == '@') {
			uint32_t parsed = 0;
			cursor++;
			if (parse_uint(cursor, &parsed) && parsed >= 100 && parsed <= 4000) {
				mv = parsed;
			}
			while (*cursor >= '0' && *cursor <= '9') {
				cursor++;
			}
		}
		probe_rails[count].mv = mv;
		count++;
		if (*cursor == ',') {
			cursor++;
		}
	}
	probe_rail_count = count;
	if (count != 0) {
		say("sensors: sensors_rails: %u candidate(s), first %s @ %u mV\n",
		    count, probe_rails[0].name, probe_rails[0].mv);
	}
}

static void
sensors_thread(__unused void *parameter, __unused wait_result_t result)
{
	tlmm_base = ml_io_map(TLMM_BASE_PHYS, TLMM_MAP_SIZE);
	if (hw_i2c_mode > 0 || probe_mode > 0 || scan_group_count != 0) {
		gcc_base = ml_io_map(GCC_BASE_PHYS, GCC_MAP_SIZE);
	}
	uint32_t gpio163_cfg = tlmm_base ? rd(tlmm_base + TLMM_GPIO_CFG(ANNIBALE_GLS6151_IRQ_GPIO)) : 0;
	uint32_t gpio35_cfg = tlmm_base ? rd(tlmm_base + TLMM_GPIO_CFG(ANNIBALE_STK6A2X_IRQ_GPIO)) : 0;

	say("sensors: gls6151 (0x%02x, SE12/GPIO16-17, GPIO%d cfg=%#x) & stk6a2x (0x%02x, SE10/GPIO8-9, GPIO%d cfg=%#x); "
	    "hw_i2c=%d probe=%d scan=%u cal offset=%u near=%u far=%u lux=%u\n",
	    ANNIBALE_GLS6151_I2C_ADDR, ANNIBALE_GLS6151_IRQ_GPIO, gpio163_cfg,
	    ANNIBALE_STK6A2X_I2C_ADDR, ANNIBALE_STK6A2X_IRQ_GPIO, gpio35_cfg,
	    hw_i2c_mode, probe_mode, scan_group_count,
	    sensor_state.prox_crosstalk, sensor_state.prox_near_thresh, sensor_state.prox_far_thresh,
	    sensor_state.sample.als_lux);

	if (scan_group_count != 0) {
		sensors_scan_run();
	}
	if (probe_mode > 0) {
		sensors_probe_run();
	}
	if (hw_i2c_mode > 0) {
		sensors_hw_init();
	}

	for (;;) {
		poll_hardware_sensors();
		delay_for_interval(250, NSEC_PER_MSEC);
	}
}

void
annibale_sensors_init(void)
{
	thread_t thread;
	uint32_t boot_lux = 0, boot_prox = 0;
	int major;

	if (!annibale_platform_is_annibale() || initialized) {
		return;
	}
	initialized = true;

	if (PE_parse_boot_argn("als_lux", &boot_lux, sizeof(boot_lux)) && boot_lux <= 100000) {
		sensor_state.sample.als_lux = boot_lux;
		sensor_state.sample.clear_raw = boot_lux * 4;
		sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_INJECTED;
	}
	if (PE_parse_boot_argn("prox_near", &boot_prox, sizeof(boot_prox))) {
		sensor_state.sample.prox_near = boot_prox ? 1 : 0;
		sensor_state.sample.prox_distance_mm = boot_prox ? 0 : 50;
		sensor_state.sample.prox_raw = boot_prox ? (ANNIBALE_GLS6151_PROX_NEAR + 200) : ANNIBALE_GLS6151_PROX_OFFSET;
		/* INJECTED stays clear: it covers ALS data only (annibale_sensors.h). */
	}
	PE_parse_boot_argn("sensors_hw_i2c", &hw_i2c_mode, sizeof(hw_i2c_mode));
	PE_parse_boot_argn("sensors_probe", &probe_mode, sizeof(probe_mode));
	parse_scan_boot_arg();
	parse_rails_boot_arg();
	/* The scan replaces the hardware paths for the boot: its pin-localization verdict decides
	 * where the driver points next, and the gpio16/17 bitbang of the retired assignment must not
	 * run in a scan round (it breaks the WCN7850 WLAN enable, see the scan's header comment). */
	if (scan_group_count != 0) {
		if (hw_i2c_mode > 0 || probe_mode > 0) {
			say("sensors: sensors_scan present, sensors_hw_i2c/sensors_probe disengaged\n");
		}
		hw_i2c_mode = 0;
		probe_mode = 0;
	}
	sensor_state.sample.timestamp = mach_absolute_time();

	major = cdevsw_add(-1, &sensors_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666,
	    "annibale-sensors") == NULL) {
		say("sensors: /dev/annibale-sensors cannot be made\n");
		return;
	}
	say("sensors: /dev/annibale-sensors ready (kern.annibale.als_lux=%u, kern.annibale.prox_near=%u)\n",
	    sensor_state.sample.als_lux, sensor_state.sample.prox_near);

	if (kernel_thread_start(sensors_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

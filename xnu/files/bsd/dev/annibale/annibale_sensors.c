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
 *     400 kHz, DRI IRQ = TLMM GPIO 35.
 *
 * Bus ownership and non-faulting MMIO invariant:
 *   On SM8750, sensor I2C buses can be routed to the ADSP/SLPI Sensor Subsystem (SSC) island
 *   or left with their AP QUPv3 wrapper 2 branch clocks gated off (CLK_OFF bit 31 = 1).
 *   Reading any QUP GENI SE register while its GCC AHB or SE branch clock is off triggers a
 *   synchronous external abort. Therefore this driver checks GCC_QUPV3_WRAP_2_M_AHB_CBCR,
 *   GCC_QUPV3_WRAP_2_S_AHB_CBCR, and the SE's CBCR before any SE MMIO read, and only touches
 *   the SE registers when the clocks report CLK_OFF == 0.
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
#include <sys/annibale_sensors.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

SYSCTL_DECL(_kern_annibale);

/* ---- SM8750 GCC, TLMM, and QUPv3 Wrapper 2 (I2C Hub) physical addresses ------------------- */

#define GCC_BASE_PHYS                   0x00100000UL
#define GCC_MAP_SIZE                    0x00040000UL
#define GCC_QUPV3_WRAP2_M_AHB_CBCR      0x1f008UL
#define GCC_QUPV3_WRAP2_S_AHB_CBCR      0x1f00cUL
#define GCC_QUPV3_WRAP2_S2_CBCR         0x1f288UL       /* SE10 (rear ALS stk6a2x) */
#define GCC_QUPV3_WRAP2_S4_CBCR         0x1f504UL       /* SE12 (front ALS+Prox gls6151) */
#define GCC_CBCR_CLK_ENABLE             (1u << 0)
#define GCC_CBCR_CLK_OFF                (1u << 31)

#define QUPV3_SE10_PHYS                 0x00888000UL
#define QUPV3_SE12_PHYS                 0x00890000UL
#define QUPV3_SE_SIZE                   0x00004000UL

#define GENI_STATUS                     0x0040UL
#define GENI_FW_REVISION_RO             0x0068UL
#define GENI_I2C_TX_TRANS_LEN           0x026cUL
#define GENI_I2C_RX_TRANS_LEN           0x0270UL
#define GENI_M_CMD0                     0x0600UL
#define GENI_M_CMD_CTRL_REG             0x0604UL
#define GENI_M_IRQ_STATUS               0x0610UL
#define GENI_M_IRQ_CLEAR                0x0618UL
#define GENI_TX_FIFOn                   0x0700UL
#define GENI_RX_FIFOn                   0x0780UL
#define GENI_RX_FIFO_STATUS             0x0804UL

#define GENI_M_GENI_ACTIVE              (1u << 0)
#define GENI_FW_PROTO_MASK              0x0000ff00u
#define GENI_FW_PROTO_I2C               0x00000300u

#define M_CMD_I2C_WRITE                 0x1u
#define M_CMD_I2C_READ                  0x2u
#define M_CMD_I2C_WRITE_READ            0x3u
#define M_CMD_SLV_ADDR_SHIFT            9
#define M_IRQ_CMD_DONE                  (1u << 0)
#define M_IRQ_CMD_OVERRUN               (1u << 1)
#define M_IRQ_ILLEGAL_CMD               (1u << 2)
#define M_IRQ_CMD_FAILURE               (1u << 3)
#define M_IRQ_CMD_CANCEL                (1u << 4)
#define M_IRQ_CMD_ABORT                 (1u << 5)
#define M_IRQ_GP_ERR_MASK               (0x7fu << 6)
#define M_IRQ_NACK                      (1u << 26)
#define M_IRQ_ERR_ALL                   (M_IRQ_CMD_OVERRUN | M_IRQ_ILLEGAL_CMD | M_IRQ_CMD_FAILURE | \
                                         M_IRQ_CMD_CANCEL | M_IRQ_CMD_ABORT | M_IRQ_GP_ERR_MASK | M_IRQ_NACK)

#define TLMM_BASE_PHYS                  0x0f100000UL
#define TLMM_MAP_SIZE                   0x00100000UL
#define TLMM_GPIO_CFG(pin)              (0x1000UL * (uint32_t)(pin))
#define TLMM_GPIO_IN_OUT(pin)           (0x1000UL * (uint32_t)(pin) + 0x04UL)

/* gls6151 register offsets (standard 8-bit register addresses) */
#define GLS6151_REG_SYSM_CTRL           0x00
#define GLS6151_REG_INT_FLAG            0x01
#define GLS6151_REG_PS_DATA_LOW         0x02
#define GLS6151_REG_ALS_CLEAR_LOW       0x04
#define GLS6151_REG_ALS_IR_LOW          0x06
#define GLS6151_REG_PRODUCT_ID          0x3e

/* stk6a2x register offsets */
#define STK6A2X_REG_STATE               0x00
#define STK6A2X_REG_ALS_DATA_MSB        0x13
#define STK6A2X_REG_PDT_ID              0x3e

static LCK_GRP_DECLARE(sensors_lck_grp, "annibale-sensors");
static LCK_MTX_DECLARE(sensors_lck, &sensors_lck_grp);

static vm_offset_t gcc_base, tlmm_base, se10_base, se12_base;
static bool initialized;
static int hw_i2c_mode;

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
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

/*
 * Check whether a QUPv3 Wrapper 2 SE has both wrapper AHB clocks and its own branch clock running.
 * Never read SE MMIO unless CLK_OFF (bit 31) is 0 on all three CBCR registers.
 */
static bool
qup_se_clock_ready(uint32_t se_cbcr_offset)
{
	uint32_t m_ahb, s_ahb, se_cbcr;

	if (gcc_base == 0) {
		return false;
	}
	m_ahb = rd(gcc_base + GCC_QUPV3_WRAP2_M_AHB_CBCR);
	s_ahb = rd(gcc_base + GCC_QUPV3_WRAP2_S_AHB_CBCR);
	se_cbcr = rd(gcc_base + se_cbcr_offset);
	if ((m_ahb & GCC_CBCR_CLK_OFF) != 0 || (s_ahb & GCC_CBCR_CLK_OFF) != 0 || (se_cbcr & GCC_CBCR_CLK_OFF) != 0) {
		return false;
	}
	if ((m_ahb & GCC_CBCR_CLK_ENABLE) == 0 || (s_ahb & GCC_CBCR_CLK_ENABLE) == 0 || (se_cbcr & GCC_CBCR_CLK_ENABLE) == 0) {
		return false;
	}
	return true;
}

/*
 * Perform a bounded QUP GENI I2C write-then-read transaction when the SE clock is verified active
 * and the GENI engine is loaded with I2C firmware.
 */
static bool
geni_i2c_read_regs(vm_offset_t se_base, uint32_t se_cbcr_offset, uint8_t slave_addr,
    uint8_t reg_addr, uint8_t *buf, uint32_t len)
{
	uint32_t fw_rev, gstatus, irq, rx_cnt, word;
	uint64_t start, timeout;

	if (se_base == 0 || len == 0 || len > 8 || !qup_se_clock_ready(se_cbcr_offset)) {
		return false;
	}
	fw_rev = rd(se_base + GENI_FW_REVISION_RO);
	if ((fw_rev & GENI_FW_PROTO_MASK) != GENI_FW_PROTO_I2C) {
		return false;
	}
	gstatus = rd(se_base + GENI_STATUS);
	if ((gstatus & GENI_M_GENI_ACTIVE) != 0) {
		return false;
	}
	nanoseconds_to_absolutetime(2000ULL * NSEC_PER_USEC, &timeout);

	/* Clear sticky master IRQ status */
	irq = rd(se_base + GENI_M_IRQ_STATUS);
	if (irq != 0) {
		wr(se_base + GENI_M_IRQ_CLEAR, irq);
	}
	wr(se_base + GENI_I2C_TX_TRANS_LEN, 1);
	wr(se_base + GENI_I2C_RX_TRANS_LEN, len);
	wr(se_base + GENI_M_CMD0, (M_CMD_I2C_WRITE_READ << 27) | ((uint32_t)(slave_addr & 0x7f) << M_CMD_SLV_ADDR_SHIFT));
	wr(se_base + GENI_TX_FIFOn, (uint32_t)reg_addr);

	start = mach_absolute_time();
	do {
		irq = rd(se_base + GENI_M_IRQ_STATUS);
		if ((irq & (M_IRQ_CMD_DONE | M_IRQ_ERR_ALL)) != 0) {
			break;
		}
		spin_us(5);
	} while (mach_absolute_time() - start < timeout);

	if ((irq & M_IRQ_CMD_DONE) == 0 || (irq & M_IRQ_ERR_ALL) != 0) {
		if ((rd(se_base + GENI_STATUS) & GENI_M_GENI_ACTIVE) != 0) {
			wr(se_base + GENI_M_CMD_CTRL_REG, 0x2); /* ABORT */
			spin_us(50);
		}
		wr(se_base + GENI_M_IRQ_CLEAR, rd(se_base + GENI_M_IRQ_STATUS));
		return false;
	}
	rx_cnt = rd(se_base + GENI_RX_FIFO_STATUS) & 0x3f;
	if (rx_cnt == 0) {
		wr(se_base + GENI_M_IRQ_CLEAR, irq);
		return false;
	}
	for (uint32_t i = 0; i < len; i += 4) {
		word = rd(se_base + GENI_RX_FIFOn);
		for (uint32_t b = 0; b < 4 && (i + b) < len; b++) {
			buf[i + b] = (uint8_t)((word >> (8 * b)) & 0xff);
		}
	}
	wr(se_base + GENI_M_IRQ_CLEAR, irq);
	return true;
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

	if (hw_i2c_mode <= 0) {
		return;
	}
	if (geni_i2c_read_regs(se12_base, GCC_QUPV3_WRAP2_S4_CBCR, ANNIBALE_GLS6151_I2C_ADDR,
	    GLS6151_REG_PS_DATA_LOW, raw, 6)) {
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
	if (geni_i2c_read_regs(se10_base, GCC_QUPV3_WRAP2_S2_CBCR, ANNIBALE_STK6A2X_I2C_ADDR,
	    STK6A2X_REG_ALS_DATA_MSB, raw, 2)) {
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
	sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_INJECTED;
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
				update_proximity_from_raw_locked(v, ANNIBALE_SENSOR_SRC_INJECTED);
			}
		}
	} else if (strncmp(buf, "raw", 3) == 0) {
		uint32_t v = 0;
		if (parse_uint(buf + 3, &v)) {
			update_proximity_from_raw_locked(v, ANNIBALE_SENSOR_SRC_INJECTED);
		}
	} else if (strncmp(buf, "lux", 3) == 0) {
		uint32_t v = 0;
		if (parse_uint(buf + 3, &v) && v <= 100000) {
			sensor_state.sample.als_lux = v;
			sensor_state.sample.clear_raw = v * 4;
		}
	} else {
		uint32_t v = 0;
		if (parse_uint(buf, &v) && v <= 100000) {
			sensor_state.sample.als_lux = v;
			sensor_state.sample.clear_raw = v * 4;
		}
	}
	sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_INJECTED;
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

static void
sensors_thread(__unused void *parameter, __unused wait_result_t result)
{
	tlmm_base = ml_io_map(TLMM_BASE_PHYS, TLMM_MAP_SIZE);
	if (hw_i2c_mode > 0) {
		gcc_base = ml_io_map(GCC_BASE_PHYS, GCC_MAP_SIZE);
		se10_base = ml_io_map(QUPV3_SE10_PHYS, QUPV3_SE_SIZE);
		se12_base = ml_io_map(QUPV3_SE12_PHYS, QUPV3_SE_SIZE);
	}

	bool se12_clk = (hw_i2c_mode > 0) ? qup_se_clock_ready(GCC_QUPV3_WRAP2_S4_CBCR) : false;
	bool se10_clk = (hw_i2c_mode > 0) ? qup_se_clock_ready(GCC_QUPV3_WRAP2_S2_CBCR) : false;
	uint32_t gpio163_cfg = tlmm_base ? rd(tlmm_base + TLMM_GPIO_CFG(ANNIBALE_GLS6151_IRQ_GPIO)) : 0;
	uint32_t gpio35_cfg = tlmm_base ? rd(tlmm_base + TLMM_GPIO_CFG(ANNIBALE_STK6A2X_IRQ_GPIO)) : 0;

	say("sensors: gls6151 (0x%02x, SE12 clk=%d, GPIO%d cfg=%#x) & stk6a2x (0x%02x, SE10 clk=%d, GPIO%d cfg=%#x); "
	    "cal offset=%u near=%u far=%u lux=%u\n",
	    ANNIBALE_GLS6151_I2C_ADDR, se12_clk, ANNIBALE_GLS6151_IRQ_GPIO, gpio163_cfg,
	    ANNIBALE_STK6A2X_I2C_ADDR, se10_clk, ANNIBALE_STK6A2X_IRQ_GPIO, gpio35_cfg,
	    sensor_state.prox_crosstalk, sensor_state.prox_near_thresh, sensor_state.prox_far_thresh,
	    sensor_state.sample.als_lux);

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
	}
	if (PE_parse_boot_argn("prox_near", &boot_prox, sizeof(boot_prox))) {
		sensor_state.sample.prox_near = boot_prox ? 1 : 0;
		sensor_state.sample.prox_distance_mm = boot_prox ? 0 : 50;
		sensor_state.sample.prox_raw = boot_prox ? (ANNIBALE_GLS6151_PROX_NEAR + 200) : ANNIBALE_GLS6151_PROX_OFFSET;
	}
	PE_parse_boot_argn("sensors_hw_i2c", &hw_i2c_mode, sizeof(hw_i2c_mode));
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

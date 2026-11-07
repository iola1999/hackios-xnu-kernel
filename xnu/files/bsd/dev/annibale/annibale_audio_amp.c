/*
 * Smart amplifier driver for REDMI K90 (annibale):
 * Dual NXP TFA98xx amplifiers on qupv3_hub_i2c0 (0x00980000, SDA GPIO64, SCL GPIO65):
 *   - Top amplifier (CH[0] T):    I2C 0x36, Reset GPIO108, IRQ GPIO66, Rdc = 5885 mOhm
 *   - Bottom amplifier (CH[1] B): I2C 0x34, Reset GPIO107, IRQ GPIO84, Rdc = 5827 mOhm
 *   - Secondary vendor fallback:  SIA9197x at 0x6c (top), 0x6e (bottom)
 *   - Speaker/receiver switch:    GPIO67 (low = speaker, high = receiver/mute)
 *
 * Calibration values (5885 mOhm top, 5827 mOhm bottom) come from K90 persist/audio/tfa_calr.txt.
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define say(...) printf("damp: " __VA_ARGS__)

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x00150000UL

#define PIN_SDA                 64u
#define PIN_SCL                 65u
#define PIN_IRQ_TOP             66u
#define PIN_SPK_SW              67u
#define PIN_IRQ_BOT             84u
#define PIN_RST_BOT             107u
#define PIN_RST_TOP             108u

#define GPIO_CFG(p)             (tlmm_base + 0x1000UL * (p))
#define GPIO_IO(p)              (tlmm_base + 0x1000UL * (p) + 4UL)

/* Known I2C 7-bit slave addresses */
#define TFA_ADDR_TOP            0x36u
#define TFA_ADDR_BOT            0x34u
#define SIA_ADDR_TOP            0x6cu
#define SIA_ADDR_BOT            0x6eu

/* Factory calibrated DC impedance from tfa_calr.txt (in mOhm) */
#define TFA_CAL_TOP_MOHM        5885u
#define TFA_CAL_BOT_MOHM        5827u

/* TFA98xx registers */
#define TFA_REG_STATUS          0x00u
#define TFA_REG_BATTERY         0x01u
#define TFA_REG_TEMP            0x02u
#define TFA_REG_REV             0x03u
#define TFA_REG_I2S             0x04u
#define TFA_REG_AUDIO_CTR       0x06u
#define TFA_REG_SYS_CTRL        0x09u
#define TFA_REG_MTP_KEY2        0x0bu
#define TFA_REG_TDM_CFG0        0x20u
#define TFA_REG_TDM_CFG1        0x21u
#define TFA_REG_TDM_CFG2        0x22u
#define TFA_REG_VOL_CTRL        0x89u
#define TFA_REG_MTP0            0xf0u
#define TFA_REG_MTP_R25C        0xf5u

/* TFA_REG_SYS_CTRL (0x09) bits */
#define TFA_SYS_PWDN            (1u << 0)
#define TFA_SYS_I2CR            (1u << 1)
#define TFA_SYS_CFE             (1u << 2)
#define TFA_SYS_AMPE            (1u << 3)
#define TFA_SYS_DCA             (1u << 4)
#define TFA_SYS_SBSL            (1u << 5)
#define TFA_SYS_AMPC            (1u << 6)
#define TFA_SYS_INTPAD_OD       (1u << 7)
#define TFA_SYS_IPLL            (1u << 13)

enum amp_type {
	AMP_TYPE_UNKNOWN = 0,
	AMP_TYPE_TFA98XX,
	AMP_TYPE_SIA9197X,
};

static vm_offset_t tlmm_base;
static enum amp_type detected_amp = AMP_TYPE_UNKNOWN;
static bool amp_is_powered = false;
static bool amp_bus_online = false;
static uint8_t addr_top = TFA_ADDR_TOP;
static uint8_t addr_bot = TFA_ADDR_BOT;
static uint32_t current_below_eighths = 18 * 8; /* -18 dB default safe headroom */
static bool current_muted = false;

static lck_grp_t *amp_lock_grp;
static lck_mtx_t *amp_lock;

bool annibale_i2c_xfer(uint8_t addr, const uint8_t *wbuf, size_t wlen, uint8_t *rbuf, size_t rlen);
void annibale_audio_amp_set_volume(uint32_t below_eighths_db, bool muted);
bool annibale_audio_amp_probe(void);
void annibale_audio_amp_power(bool enable);
void annibale_audio_amp_status(const char *when);

static inline uint32_t
tlmm_rd(vm_offset_t reg)
{
	return *(volatile uint32_t *)reg;
}

static inline void
tlmm_wr(vm_offset_t reg, uint32_t val)
{
	*(volatile uint32_t *)reg = val;
	__builtin_arm_dsb(0xf);
}

/* Open-drain input configuration: function 0 (GPIO), OE=0, internal pull-up (3) */
static uint32_t
od_input_cfg(unsigned pin)
{
	uint32_t cfg = tlmm_rd(GPIO_CFG(pin));
	cfg = (cfg & ~0x3ffu) | 3u;
	if (cfg & (1u << 11)) {
		cfg |= (1u << 12); /* AP owns eGPIO pad */
	}
	return cfg;
}

/* Open-drain active-low configuration: function 0 (GPIO), OE=1, 6 mA drive, pull-up (3) */
static uint32_t
od_low_cfg(unsigned pin)
{
	return (od_input_cfg(pin) & ~0x1c0u) | (1u << 9) | (2u << 6);
}

static void
gpio_push_pull(unsigned pin, bool high)
{
	uint32_t cfg = tlmm_rd(GPIO_CFG(pin));
	cfg = (cfg & ~0x3ffu) | (1u << 9) | (2u << 6);
	if (cfg & (1u << 11)) {
		cfg |= (1u << 12);
	}
	tlmm_wr(GPIO_IO(pin), high ? 2u : 0u);
	tlmm_wr(GPIO_CFG(pin), cfg);
}

static bool
gpio_in(unsigned pin)
{
	return (tlmm_rd(GPIO_IO(pin)) & 1u) != 0;
}

static void
gpio_init_once(void)
{
	if (tlmm_base == 0) {
		tlmm_base = ml_io_map(TLMM_PHYS, TLMM_SIZE);
		if (amp_lock_grp == NULL) {
			amp_lock_grp = lck_grp_alloc_init("annibale-amp", LCK_GRP_ATTR_NULL);
			amp_lock = lck_mtx_alloc_init(amp_lock_grp, LCK_ATTR_NULL);
		}
		tlmm_wr(GPIO_IO(PIN_SDA), 0);
		tlmm_wr(GPIO_IO(PIN_SCL), 0);
		tlmm_wr(GPIO_CFG(PIN_SDA), od_input_cfg(PIN_SDA));
		tlmm_wr(GPIO_CFG(PIN_SCL), od_input_cfg(PIN_SCL));
	}
}

/* ---- Open-drain Software I2C with bus-idle and clock-stretch verification -------------------- */

static void
i2c_delay(void)
{
	delay_for_interval(4, NSEC_PER_USEC);
}

static bool
i2c_scl_high(void)
{
	tlmm_wr(GPIO_CFG(PIN_SCL), od_input_cfg(PIN_SCL));
	for (unsigned i = 0; i < 250; i++) {
		if (gpio_in(PIN_SCL)) {
			i2c_delay();
			return true;
		}
		delay_for_interval(1, NSEC_PER_USEC);
	}
	return gpio_in(PIN_SCL);
}

static void
i2c_scl_low(void)
{
	tlmm_wr(GPIO_IO(PIN_SCL), 0);
	tlmm_wr(GPIO_CFG(PIN_SCL), od_low_cfg(PIN_SCL));
	i2c_delay();
}

static void
i2c_sda_set(bool high)
{
	if (high) {
		tlmm_wr(GPIO_CFG(PIN_SDA), od_input_cfg(PIN_SDA));
	} else {
		tlmm_wr(GPIO_IO(PIN_SDA), 0);
		tlmm_wr(GPIO_CFG(PIN_SDA), od_low_cfg(PIN_SDA));
	}
}

/* Ensure both SDA and SCL float high when released; pulse 9 clocks if a slave is mid-byte */
static bool
i2c_bus_recover_and_check(void)
{
	i2c_sda_set(true);
	(void)i2c_scl_high();
	i2c_delay();
	if (gpio_in(PIN_SDA) && gpio_in(PIN_SCL)) {
		return true;
	}
	for (unsigned i = 0; i < 9; i++) {
		i2c_scl_low();
		i2c_sda_set(true);
		(void)i2c_scl_high();
		if (gpio_in(PIN_SDA)) {
			break;
		}
	}
	i2c_sda_set(false);
	i2c_delay();
	(void)i2c_scl_high();
	i2c_sda_set(true);
	i2c_delay();
	return gpio_in(PIN_SDA) && gpio_in(PIN_SCL);
}

static bool
i2c_start(void)
{
	i2c_sda_set(true);
	if (!i2c_scl_high()) {
		return false;
	}
	if (!gpio_in(PIN_SDA)) {
		/* Bus is stuck low; do not mistake a grounded SDA line for slave ACKs */
		return false;
	}
	i2c_sda_set(false);
	i2c_delay();
	i2c_scl_low();
	return true;
}

static void
i2c_stop(void)
{
	i2c_sda_set(false);
	i2c_delay();
	(void)i2c_scl_high();
	i2c_sda_set(true);
	i2c_delay();
}

static bool
i2c_write_byte(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_sda_set(((byte >> bit) & 1) != 0);
		i2c_delay();
		if (!i2c_scl_high()) {
			i2c_scl_low();
			return false;
		}
		i2c_scl_low();
	}
	i2c_sda_set(true);
	i2c_delay();
	if (!i2c_scl_high()) {
		i2c_scl_low();
		return false;
	}
	bool ack = !gpio_in(PIN_SDA);
	i2c_scl_low();
	return ack;
}

static uint8_t
i2c_read_byte(bool send_ack)
{
	uint8_t byte = 0;
	i2c_sda_set(true);
	for (int bit = 7; bit >= 0; bit--) {
		i2c_delay();
		(void)i2c_scl_high();
		if (gpio_in(PIN_SDA)) {
			byte |= (uint8_t)(1u << bit);
		}
		i2c_scl_low();
	}
	i2c_sda_set(!send_ack);
	i2c_delay();
	(void)i2c_scl_high();
	i2c_scl_low();
	i2c_sda_set(true);
	return byte;
}

bool
annibale_i2c_xfer(uint8_t addr, const uint8_t *wbuf, size_t wlen, uint8_t *rbuf, size_t rlen)
{
	bool ok = false;

	gpio_init_once();
	lck_mtx_lock(amp_lock);
	if (!i2c_start()) {
		goto out;
	}
	ok = true;
	if (wlen > 0 || rlen == 0) {
		ok = i2c_write_byte((uint8_t)(addr << 1));
		for (size_t i = 0; ok && i < wlen; i++) {
			ok = i2c_write_byte(wbuf[i]);
		}
		if (ok && rlen > 0) {
			if (!i2c_start()) {
				ok = false;
			}
		}
	}
	if (ok && rlen > 0) {
		ok = i2c_write_byte((uint8_t)((addr << 1) | 1u));
		for (size_t i = 0; ok && i < rlen; i++) {
			rbuf[i] = i2c_read_byte(i + 1 < rlen);
		}
	}
	i2c_stop();
out:
	lck_mtx_unlock(amp_lock);
	return ok;
}

static bool
amp_reg_read8(uint8_t addr, uint8_t reg, uint8_t *val)
{
	return annibale_i2c_xfer(addr, &reg, 1, val, 1);
}

static bool
amp_reg_write8(uint8_t addr, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return annibale_i2c_xfer(addr, buf, 2, NULL, 0);
}

static bool
amp_reg_read16(uint8_t addr, uint8_t reg, uint16_t *val)
{
	uint8_t buf[2] = { 0, 0 };
	if (!annibale_i2c_xfer(addr, &reg, 1, buf, 2)) {
		return false;
	}
	*val = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
	return true;
}

static bool
amp_reg_write16(uint8_t addr, uint8_t reg, uint16_t val)
{
	uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xffu) };
	return annibale_i2c_xfer(addr, buf, 3, NULL, 0);
}

/* ---- TFA98xx & SIA9197x Initialization and Volume Control ----------------------------------- */

static uint8_t
eighths_db_to_tfa_atten(uint32_t below_eighths)
{
	/*
	 * TFA98xx volume step is 0.5 dB per LSB (4 eighths of a dB).
	 * Enforce a minimum 6 dB (12 half-dB steps) hardware protection ceiling.
	 */
	uint32_t half_db = (below_eighths + 2u) / 4u + 12u;
	if (half_db > 0xffu) {
		half_db = 0xffu;
	}
	return (uint8_t)half_db;
}

static void
tfa98xx_init_one(uint8_t addr, unsigned ch, uint32_t cal_mohm)
{
	uint16_t rev = 0, status = 0;
	if (amp_reg_read16(addr, TFA_REG_REV, &rev)) {
		(void)amp_reg_read16(addr, TFA_REG_STATUS, &status);
		say("TFA98xx ch%u at %#x: rev=%#x status=%#x cal=%u mOhm\n",
		    ch, addr, rev, status, cal_mohm);
	}

	/* Soft reset I2C registers then enter powerdown for configuration */
	(void)amp_reg_write16(addr, TFA_REG_SYS_CTRL, TFA_SYS_I2CR);
	delay_for_interval(1, NSEC_PER_MSEC);
	(void)amp_reg_write16(addr, TFA_REG_SYS_CTRL, TFA_SYS_PWDN);

	/*
	 * Program factory speaker coil resistance (tfa_calr.txt: 5885 mOhm top, 5827 mOhm bottom):
	 * Unlock MTP key2 (0x5a6b), write R25C in ohms * 1024 (or mOhm), and mark OTC=1, MTPEX=1.
	 */
	(void)amp_reg_write16(addr, TFA_REG_MTP_KEY2, 0x5a6bu);
	(void)amp_reg_write16(addr, TFA_REG_MTP_R25C, (uint16_t)cal_mohm);
	(void)amp_reg_write16(addr, TFA_REG_MTP0, 0x0003u);

	/*
	 * Configure I2S/TDM interface:
	 * 48 kHz (FS=8), 16-bit standard I2S slave, ch 0 = left slot, ch 1 = right slot.
	 */
	uint16_t i2s_cfg = (uint16_t)(0x8803u | ((ch == 0 ? 1u : 2u) << 3));
	(void)amp_reg_write16(addr, TFA_REG_I2S, i2s_cfg);
	(void)amp_reg_write16(addr, TFA_REG_TDM_CFG0, 0x0010u | (ch & 1u));
	(void)amp_reg_write16(addr, TFA_REG_TDM_CFG1, 0x01f0u);
	(void)amp_reg_write16(addr, TFA_REG_TDM_CFG2, (uint16_t)(ch & 1u));

	/* Set attenuation and unmute */
	uint8_t atten = current_muted ? 0xffu : eighths_db_to_tfa_atten(current_below_eighths);
	(void)amp_reg_write16(addr, TFA_REG_AUDIO_CTR, current_muted ? 0x0020u : 0x0000u);
	(void)amp_reg_write16(addr, TFA_REG_VOL_CTRL, (uint16_t)atten);

	/* Power up PLL (on BCK), DC-DC boost converter, Class-D stage, and mark configured (SBSL) */
	uint16_t sys_up = TFA_SYS_CFE | TFA_SYS_AMPE | TFA_SYS_DCA | TFA_SYS_SBSL | TFA_SYS_IPLL;
	(void)amp_reg_write16(addr, TFA_REG_SYS_CTRL, sys_up);
}

static void
sia9197x_init_one(uint8_t addr, unsigned ch)
{
	uint8_t id = 0;
	if (amp_reg_read8(addr, 0x00, &id)) {
		say("SIA9197x ch%u at %#x: chip_id=%#x\n", ch, addr, id);
	}
	(void)amp_reg_write8(addr, 0x05, current_muted ? 0x3fu : 0x18u);
	(void)amp_reg_write8(addr, 0x06, 0x02u);
}

void
annibale_audio_amp_set_volume(uint32_t below_eighths_db, bool muted)
{
	gpio_init_once();
	current_below_eighths = below_eighths_db;
	current_muted = muted;

	if (!amp_is_powered || !amp_bus_online) {
		return;
	}
	if (detected_amp == AMP_TYPE_TFA98XX) {
		uint8_t atten = muted ? 0xffu : eighths_db_to_tfa_atten(below_eighths_db);
		uint16_t audio_ctr = muted ? 0x0020u : 0x0000u;
		(void)amp_reg_write16(addr_top, TFA_REG_VOL_CTRL, atten);
		(void)amp_reg_write16(addr_bot, TFA_REG_VOL_CTRL, atten);
		(void)amp_reg_write16(addr_top, TFA_REG_AUDIO_CTR, audio_ctr);
		(void)amp_reg_write16(addr_bot, TFA_REG_AUDIO_CTR, audio_ctr);
	} else if (detected_amp == AMP_TYPE_SIA9197X) {
		uint8_t vol = muted ? 0x3fu : (uint8_t)MIN(0x3fu, 0x10u + below_eighths_db / 8u);
		(void)amp_reg_write8(addr_top, 0x05, vol);
		(void)amp_reg_write8(addr_bot, 0x05, vol);
	}
}

bool
annibale_audio_amp_probe(void)
{
	gpio_init_once();

	/* Configure IRQ pins as inputs with pull-up */
	tlmm_wr(GPIO_CFG(PIN_IRQ_TOP), od_input_cfg(PIN_IRQ_TOP));
	tlmm_wr(GPIO_CFG(PIN_IRQ_BOT), od_input_cfg(PIN_IRQ_BOT));

	/* Assert hardware reset on both amplifiers (active high reset pulse then low/high check) */
	gpio_push_pull(PIN_RST_TOP, false);
	gpio_push_pull(PIN_RST_BOT, false);
	delay_for_interval(2, NSEC_PER_MSEC);
	gpio_push_pull(PIN_RST_TOP, true);
	gpio_push_pull(PIN_RST_BOT, true);
	delay_for_interval(10, NSEC_PER_MSEC);

	/* Route speaker switch GPIO67 to speaker path (low) */
	gpio_push_pull(PIN_SPK_SW, false);

	amp_bus_online = i2c_bus_recover_and_check();
	say("hub_i2c0 (SDA GPIO64=%u, SCL GPIO65=%u) bus_online=%d\n",
	    gpio_in(PIN_SDA), gpio_in(PIN_SCL), amp_bus_online);

	if (amp_bus_online) {
		/* Try active-low reset polarity first; if TFA98xx (reset active-high) does not ACK, invert */
		if (!annibale_i2c_xfer(TFA_ADDR_TOP, NULL, 0, NULL, 0) &&
		    !annibale_i2c_xfer(TFA_ADDR_BOT, NULL, 0, NULL, 0)) {
			gpio_push_pull(PIN_RST_TOP, false);
			gpio_push_pull(PIN_RST_BOT, false);
			delay_for_interval(5, NSEC_PER_MSEC);
		}

		char responding[128];
		int pos = 0;
		for (uint8_t a = 0x08; a < 0x78 && pos < (int)sizeof(responding) - 8; a++) {
			if (annibale_i2c_xfer(a, NULL, 0, NULL, 0)) {
				pos += snprintf(responding + pos, sizeof(responding) - (size_t)pos, " %#x", a);
			}
		}
		responding[pos] = '\0';
		say("I2C bus scan responses:%s\n", pos > 0 ? responding : " none");

		if (annibale_i2c_xfer(TFA_ADDR_TOP, NULL, 0, NULL, 0) ||
		    annibale_i2c_xfer(TFA_ADDR_BOT, NULL, 0, NULL, 0)) {
			detected_amp = AMP_TYPE_TFA98XX;
			addr_top = TFA_ADDR_TOP;
			addr_bot = TFA_ADDR_BOT;
			say("detected dual NXP TFA98xx amplifiers (top %#x [%u mOhm], bottom %#x [%u mOhm])\n",
			    addr_top, TFA_CAL_TOP_MOHM, addr_bot, TFA_CAL_BOT_MOHM);
			tfa98xx_init_one(addr_top, 0, TFA_CAL_TOP_MOHM);
			tfa98xx_init_one(addr_bot, 1, TFA_CAL_BOT_MOHM);
			return true;
		}

		if (annibale_i2c_xfer(SIA_ADDR_TOP, NULL, 0, NULL, 0) ||
		    annibale_i2c_xfer(SIA_ADDR_BOT, NULL, 0, NULL, 0)) {
			detected_amp = AMP_TYPE_SIA9197X;
			addr_top = SIA_ADDR_TOP;
			addr_bot = SIA_ADDR_BOT;
			say("detected dual SIA9197x amplifiers (top %#x, bottom %#x)\n", addr_top, addr_bot);
			sia9197x_init_one(addr_top, 0);
			sia9197x_init_one(addr_bot, 1);
			return true;
		}
	}

	/* Configure K90 default TFA98xx profile with factory calibration (5885 / 5827 mOhm) */
	detected_amp = AMP_TYPE_TFA98XX;
	addr_top = TFA_ADDR_TOP;
	addr_bot = TFA_ADDR_BOT;
	say("configured default TFA98xx profile (top %#x [%u mOhm], bottom %#x [%u mOhm])\n",
	    addr_top, TFA_CAL_TOP_MOHM, addr_bot, TFA_CAL_BOT_MOHM);
	return false;
}

void
annibale_audio_amp_power(bool on)
{
	gpio_init_once();
	if (amp_is_powered == on) {
		return;
	}
	amp_is_powered = on;
	if (on) {
		gpio_push_pull(PIN_SPK_SW, false);
		if (!amp_bus_online) {
			amp_bus_online = i2c_bus_recover_and_check();
		}
		if (amp_bus_online) {
			if (detected_amp == AMP_TYPE_TFA98XX) {
				tfa98xx_init_one(addr_top, 0, TFA_CAL_TOP_MOHM);
				tfa98xx_init_one(addr_bot, 1, TFA_CAL_BOT_MOHM);
			} else if (detected_amp == AMP_TYPE_SIA9197X) {
				sia9197x_init_one(addr_top, 0);
				sia9197x_init_one(addr_bot, 1);
			}
		}
		say("smart amplifier powered on (top=%#x, bot=%#x, below=%u.%03u dB, muted=%d)\n",
		    addr_top, addr_bot, current_below_eighths / 8, (current_below_eighths % 8) * 125, current_muted);
	} else {
		if (amp_bus_online && detected_amp == AMP_TYPE_TFA98XX) {
			(void)amp_reg_write16(addr_top, TFA_REG_AUDIO_CTR, 0x0020u);
			(void)amp_reg_write16(addr_bot, TFA_REG_AUDIO_CTR, 0x0020u);
			(void)amp_reg_write16(addr_top, TFA_REG_SYS_CTRL, TFA_SYS_PWDN);
			(void)amp_reg_write16(addr_bot, TFA_REG_SYS_CTRL, TFA_SYS_PWDN);
		}
		gpio_push_pull(PIN_SPK_SW, true);
		say("smart amplifier powered off\n");
	}
}

void
annibale_audio_amp_status(const char *when)
{
	gpio_init_once();
	say("status (%s): type=%d powered=%d bus_online=%d top=%#x(%u mOhm) bot=%#x(%u mOhm)\n",
	    when, detected_amp, amp_is_powered, amp_bus_online,
	    addr_top, TFA_CAL_TOP_MOHM, addr_bot, TFA_CAL_BOT_MOHM);
}

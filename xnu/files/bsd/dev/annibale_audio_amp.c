/*
 * Smart amplifier driver for REDMI K90 (annibale):
 * Dual-vendor design on qupv3_hub_i2c0 (SDA GPIO64, SCL GPIO65):
 *   - TFA98xx: 0x36 (amp 1 / left-top), 0x34 (amp 2 / right-bottom)
 *   - SIA9197x: 0x6c (amp 1), 0x6e (amp 2)
 *
 * Pin assignments:
 *   - Reset: GPIO108 (amp 1), GPIO107 (amp 2)
 *   - Interrupts: GPIO66 (amp 1), GPIO84 (amp 2)
 *   - Speaker/receiver path switch: GPIO67
 *
 * Safe initial volume: -40 dBFS / 18 dB fixed gain to protect hardware.
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define say(...) printf("damp: " __VA_ARGS__)

#define TLMM_PHYS       0x0f100000UL
#define TLMM_SIZE       0x150000UL

#define PIN_SDA         64
#define PIN_SCL         65
#define PIN_RST_BOT     107
#define PIN_RST_TOP     108
#define PIN_IRQ_TOP     66
#define PIN_IRQ_BOT     84
#define PIN_SPK_SW      67

#define GPIO_CFG(p)     (tlmm_base + 0x1000UL * (p))
#define GPIO_IO(p)      (tlmm_base + 0x1000UL * (p) + 4)

/* Known device addresses */
#define TFA_ADDR_TOP    0x36
#define TFA_ADDR_BOT    0x34
#define SIA_ADDR_TOP    0x6c
#define SIA_ADDR_BOT    0x6e

enum amp_type {
	AMP_TYPE_UNKNOWN = 0,
	AMP_TYPE_TFA98XX,
	AMP_TYPE_SIA9197X,
};

static vm_offset_t tlmm_base;
static enum amp_type detected_amp = AMP_TYPE_UNKNOWN;
static bool amp_is_powered = false;
static uint8_t addr_top = 0;
static uint8_t addr_bot = 0;

static lck_grp_t *amp_lock_grp;
static lck_mtx_t *amp_lock;

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

static void
gpio_init_once(void)
{
	if (tlmm_base == 0) {
		tlmm_base = ml_io_map(TLMM_PHYS, TLMM_SIZE);
		if (amp_lock_grp == NULL) {
			amp_lock_grp = lck_grp_alloc_init("annibale-amp", LCK_GRP_ATTR_NULL);
			amp_lock = lck_mtx_alloc_init(amp_lock_grp, LCK_ATTR_NULL);
		}
	}
}

static void
gpio_set_dir(unsigned pin, bool output)
{
	uint32_t cfg = tlmm_rd(GPIO_CFG(pin));
	/* Function 0 = GPIO, 2mA drive, keep pull state */
	cfg &= ~(0x0f << 2);
	if (cfg & (1u << 11)) {
		cfg |= (1u << 12); /* AP owns pad */
	}
	if (output) {
		cfg |= (1u << 9);  /* Output enable */
	} else {
		cfg &= ~(1u << 9); /* Input mode */
	}
	tlmm_wr(GPIO_CFG(pin), cfg);
}

static void
gpio_write(unsigned pin, bool high)
{
	gpio_set_dir(pin, true);
	tlmm_wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_read(unsigned pin)
{
	uint32_t cfg = tlmm_rd(GPIO_CFG(pin));
	bool was_output = (cfg & (1u << 9)) != 0;
	if (was_output) {
		gpio_set_dir(pin, false);
	}
	bool val = (tlmm_rd(GPIO_IO(pin)) & 1) != 0;
	if (was_output) {
		gpio_set_dir(pin, true);
	}
	return val;
}

/* ---- Software I2C implementation ------------------------------------------------------------- */

static void
i2c_delay(void)
{
	delay_for_interval(4, NSEC_PER_USEC);
}

static void
i2c_scl(bool high)
{
	if (high) {
		/* 开漏高电平：释放引脚由外部上拉电阻拉高，支持从机时钟延展 */
		gpio_set_dir(PIN_SCL, false);
		for (int i = 0; i < 1000 && !gpio_read(PIN_SCL); i++) {
			i2c_delay();
		}
	} else {
		/* 开漏低电平：配置输出主动下拉至 0 */
		gpio_write(PIN_SCL, false);
	}
}

static void
i2c_sda(bool high)
{
	if (high) {
		gpio_set_dir(PIN_SDA, false); /* Float high via pull-up */
	} else {
		gpio_write(PIN_SDA, false);
	}
}

static void
i2c_start(void)
{
	i2c_sda(true);
	i2c_scl(true);
	i2c_delay();
	i2c_sda(false);
	i2c_delay();
	i2c_scl(false);
	i2c_delay();
}

static void
i2c_stop(void)
{
	i2c_sda(false);
	i2c_delay();
	i2c_scl(true);
	i2c_delay();
	i2c_sda(true);
	i2c_delay();
}

static bool
i2c_write_byte(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_sda((byte >> bit) & 1);
		i2c_delay();
		i2c_scl(true);
		i2c_delay();
		i2c_scl(false);
		i2c_delay();
	}
	/* Read ACK bit */
	i2c_sda(true);
	i2c_delay();
	i2c_scl(true);
	i2c_delay();
	bool ack = !gpio_read(PIN_SDA);
	i2c_scl(false);
	i2c_delay();
	return ack;
}

static uint8_t
i2c_read_byte(bool ack)
{
	uint8_t byte = 0;
	i2c_sda(true);
	for (int bit = 7; bit >= 0; bit--) {
		i2c_delay();
		i2c_scl(true);
		i2c_delay();
		if (gpio_read(PIN_SDA)) {
			byte |= (1u << bit);
		}
		i2c_scl(false);
		i2c_delay();
	}
	/* Send ACK or NAK */
	i2c_sda(!ack);
	i2c_delay();
	i2c_scl(true);
	i2c_delay();
	i2c_scl(false);
	i2c_delay();
	i2c_sda(true);
	return byte;
}

bool
annibale_i2c_xfer(uint8_t addr, const uint8_t *wbuf, size_t wlen, uint8_t *rbuf, size_t rlen)
{
	bool ok = true;

	lck_mtx_lock(amp_lock);
	i2c_start();
	if (wlen > 0 || rlen == 0) {
		ok = i2c_write_byte((uint8_t)(addr << 1));
		for (size_t i = 0; ok && i < wlen; i++) {
			ok = i2c_write_byte(wbuf[i]);
		}
		if (ok && rlen > 0) {
			/* Repeated start */
			i2c_sda(true);
			i2c_scl(true);
			i2c_delay();
			i2c_sda(false);
			i2c_delay();
			i2c_scl(false);
			i2c_delay();
		}
	}
	if (ok && rlen > 0) {
		ok = i2c_write_byte((uint8_t)((addr << 1) | 1));
		for (size_t i = 0; ok && i < rlen; i++) {
			rbuf[i] = i2c_read_byte(i + 1 < rlen);
		}
	}
	i2c_stop();
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
	uint8_t buf[2];
	if (!annibale_i2c_xfer(addr, &reg, 1, buf, 2)) {
		return false;
	}
	*val = ((uint16_t)buf[0] << 8) | buf[1];
	return true;
}

static bool
amp_reg_write16(uint8_t addr, uint8_t reg, uint16_t val)
{
	uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	return annibale_i2c_xfer(addr, buf, 3, NULL, 0);
}

/* ---- Smart Amp hardware initialization and gain setup ---------------------------------------- */

static void
tfa98xx_init(uint8_t addr)
{
	/*
	 * TFA98xx initial sequence:
	 * Safe gain: -40 dBFS (or 18 dB), un-mute speaker path.
	 * Reg 0x04: I2S control (48 kHz slave, 16-bit format).
	 * Reg 0x09: System control (power up DSP bypass).
	 */
	uint16_t rev = 0;
	if (amp_reg_read16(addr, 0x03, &rev)) {
		say("TFA98xx at %#x revision: %#x\n", addr, rev);
	}
	/* Set safe gain (-40 dBFS attenuator) */
	(void)amp_reg_write16(addr, 0x89, 0x0028); /* 40 dB attenuation */
	(void)amp_reg_write16(addr, 0x04, 0x088b); /* 48 kHz 16-bit I2S */
}

static void
sia9197x_init(uint8_t addr)
{
	/*
	 * SIA9197x initial sequence:
	 * Reg 0x00: chip id
	 * Reg 0x05: volume control (-40 dB safe gain)
	 * Reg 0x06: audio format I2S 16-bit
	 */
	uint8_t id = 0;
	if (amp_reg_read8(addr, 0x00, &id)) {
		say("SIA9197x at %#x chip id: %#x\n", addr, id);
	}
	(void)amp_reg_write8(addr, 0x05, 0x28); /* safe gain */
	(void)amp_reg_write8(addr, 0x06, 0x02); /* 16-bit stereo */
}

bool
annibale_audio_amp_probe(void)
{
	gpio_init_once();

	say("probing smart amplifier on hub_i2c0 (SDA GPIO64, SCL GPIO65)...\n");

	/* Reset pulse: low for 2 ms, high for 50 ms */
	gpio_write(PIN_RST_TOP, false);
	gpio_write(PIN_RST_BOT, false);
	delay_for_interval(2, NSEC_PER_MSEC);
	gpio_write(PIN_RST_TOP, true);
	gpio_write(PIN_RST_BOT, true);
	delay_for_interval(50, NSEC_PER_MSEC);

	/* Set speaker switch GPIO67 low (speaker mode) */
	gpio_write(PIN_SPK_SW, false);

	/* Scan I2C addresses on bus */
	char responding[128];
	int pos = 0;
	for (uint8_t a = 0x08; a < 0x78 && pos < (int)sizeof(responding) - 8; a++) {
		if (annibale_i2c_xfer(a, NULL, 0, NULL, 0)) {
			pos += snprintf(responding + pos, sizeof(responding) - (size_t)pos, " %#x", a);
		}
	}
	responding[pos] = '\0';
	say("I2C bus scan responses:%s\n", pos > 0 ? responding : " none");

	/* Check TFA98xx addresses (0x36, 0x34) */
	if (annibale_i2c_xfer(TFA_ADDR_TOP, NULL, 0, NULL, 0) ||
	    annibale_i2c_xfer(TFA_ADDR_BOT, NULL, 0, NULL, 0)) {
		detected_amp = AMP_TYPE_TFA98XX;
		addr_top = TFA_ADDR_TOP;
		addr_bot = TFA_ADDR_BOT;
		say("detected amplifier: TFA98xx (top %#x, bottom %#x)\n", addr_top, addr_bot);
		tfa98xx_init(addr_top);
		tfa98xx_init(addr_bot);
		return true;
	}

	/* Check SIA9197x addresses (0x6c, 0x6e) */
	if (annibale_i2c_xfer(SIA_ADDR_TOP, NULL, 0, NULL, 0) ||
	    annibale_i2c_xfer(SIA_ADDR_BOT, NULL, 0, NULL, 0)) {
		detected_amp = AMP_TYPE_SIA9197X;
		addr_top = SIA_ADDR_TOP;
		addr_bot = SIA_ADDR_BOT;
		say("detected amplifier: SIA9197x (top %#x, bottom %#x)\n", addr_top, addr_bot);
		sia9197x_init(addr_top);
		sia9197x_init(addr_bot);
		return true;
	}

	/* Default fallback for board bringup */
	detected_amp = AMP_TYPE_TFA98XX;
	addr_top = TFA_ADDR_TOP;
	addr_bot = TFA_ADDR_BOT;
	say("no amplifier acknowledged on I2C; configuring default TFA98xx profiles\n");
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
		gpio_write(PIN_SPK_SW, false); /* Speaker path active */
		gpio_write(PIN_RST_TOP, true);
		gpio_write(PIN_RST_BOT, true);
		delay_for_interval(5, NSEC_PER_MSEC);
		if (detected_amp == AMP_TYPE_TFA98XX) {
			tfa98xx_init(addr_top);
			tfa98xx_init(addr_bot);
		} else if (detected_amp == AMP_TYPE_SIA9197X) {
			sia9197x_init(addr_top);
			sia9197x_init(addr_bot);
		}
		say("smart amplifier: powered on (safe initial gain -40 dBFS)\n");
	} else {
		/* Mute and power down */
		gpio_write(PIN_SPK_SW, true);  /* Mute speaker switch */
		say("smart amplifier: powered off\n");
	}
}

void
annibale_audio_amp_status(const char *when)
{
	gpio_init_once();
	say("status (%s): type %d, powered %d, top addr %#x, bot addr %#x\n",
	    when, detected_amp, amp_is_powered, addr_top, addr_bot);
}

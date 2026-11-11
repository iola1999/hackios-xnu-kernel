/*
 * Smart amplifier driver for REDMI K90 (annibale):
 * Dual NXP/Goodix TFA9865/TFA9866 (tfa_family = 2, CoolPWM without CoolFlux DSP)
 * amplifiers on qupv3_hub_i2c0 (0x00980000, SDA GPIO64, SCL GPIO65):
 *   - Top amplifier (CH[0] 'RCV'):    I2C 0x36, Reset GPIO108, IRQ GPIO66, TDM slot 0, Rdc = 5885 mOhm
 *   - Bottom amplifier (CH[1] 'SPK'): I2C 0x34, Reset GPIO107, IRQ GPIO84, TDM slot 1, Rdc = 5827 mOhm
 *   - Secondary vendor option:        SIA9197x at 0x6c (top), 0x6e (bottom)
 *   - Speaker/receiver switch:        GPIO67 (low = speaker 'stereo' mode)
 *   - 1.8 V digital supply:           PMIC pm_humu_l15 -> RPMh VRM "ldob15" (1800 mV, HPM mode 7)
 *
 * Register initialization and bitfield tables match K90 vendor tfa98xx_dlkm.ko
 * (tfa986x_specific / tfa9865_specific / tfaRunStartup / tfa_dev_set_state) and
 * firmware container tfa98xx.cnt ('stereo' profile, 0b65_08).
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/annibale_rpmh.h>
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

/* Factory calibrated DC impedance from persist/audio/tfa_calr.txt (in mOhm) */
#define TFA_CAL_TOP_MOHM        5885u
#define TFA_CAL_BOT_MOHM        5827u

/* TFA9865 / TFA9866 (tfa_family = 2) registers & bitfields (bf = (reg<<8) | (pos<<4) | (width-1)) */
#define TFA6X_REG_SYS_CONTROL0  0x00u
#define TFA6X_REG_SYS_CONTROL1  0x01u
#define TFA6X_REG_SYS_CONTROL2  0x02u
#define TFA6X_REG_REV           0x03u
#define TFA6X_REG_CLOCK_GATING  0x04u
#define TFA6X_REG_TYPE_BITS     0x06u
#define TFA6X_REG_HIDDEN_KEY    0x0fu
#define TFA6X_REG_STATUS_FLAGS0 0x10u
#define TFA6X_REG_STATUS_FLAGS1 0x11u
#define TFA6X_REG_SYS_STATUS0   0x12u
#define TFA6X_REG_VOL_CTRL      0x51u
#define TFA6X_REG_MTP_KEY2_A0   0xa0u
#define TFA6X_REG_MTP_KEY2_A1   0xa1u
#define TFA6X_REG_SWPROFIL      0xe0u
#define TFA6X_REG_MTP0          0xf0u
#define TFA6X_REG_KEY1_CHALLENGE 0xfbu

#define TFA6X_BF_PWDN           0x0000u /* reg 0x00[0]: 1 = powerdown, 0 = powerup */
#define TFA6X_BF_I2CR           0x0010u /* reg 0x00[1]: I2C register reset */
#define TFA6X_BF_AMPE           0x0030u /* reg 0x00[3]: Amplifier enable */
#define TFA6X_BF_DCA            0x0040u /* reg 0x00[4]: DC-DC boost enable */
#define TFA6X_BF_MANSCONF       0x0120u /* reg 0x01[2]: I2C configuration complete */
#define TFA6X_BF_AUDFS          0x0203u /* reg 0x02[3:0]: sample rate (8 = 48 kHz) */
#define TFA6X_BF_CSKIP_SAVE     0x0255u /* reg 0x02[10:5]: clock skip field preserved on reset */
#define TFA6X_BF_OPEN_PROTECT   0x0460u /* reg 0x04[6]: internal clock gate during OTP load */
#define TFA6X_BF_REV_HI2        0x0687u /* reg 0x06[15:8]: extended silicon revision bits */
#define TFA6X_BF_CLKS           0x1100u /* reg 0x11[0]: PLL & clocks stable on BCLK */
#define TFA6X_BF_MANSTATE       0x1203u /* reg 0x12[3:0]: manager state (0=pwdn, 1=wait_cfg, 6=mute, 9=op) */
#define TFA6X_BF_VOL            0x5187u /* reg 0x51[15:8]: 0.5 dB/step attenuation (0xff = mute) */
#define TFA6X_BF_INIT_DONE      0x9000u /* reg 0x90[0]: post-I2CR init latch */
#define TFA6X_BF_SWPROFIL       0xe00fu /* reg 0xe0[15:0]: software profile marker */
#define TFA6X_BF_SWVSTEP        0xe10fu /* reg 0xe1[15:0]: software vstep marker */
#define TFA6X_BF_OTP_F0_9       0xf090u /* reg 0xf0[9] */
#define TFA6X_BF_OTP_F3_11      0xf3b1u /* reg 0xf3[12:11] */
#define TFA6X_BF_OTP_F4_15      0xf4f0u /* reg 0xf4[15] */

enum amp_type {
	AMP_TYPE_UNKNOWN = 0,
	AMP_TYPE_TFA986X,
	AMP_TYPE_SIA9197X,
};

struct tfa_reg_val {
	uint8_t  reg;
	uint16_t val;
};

struct tfa_bf_val {
	uint16_t bf;
	uint16_t val;
};

static vm_offset_t tlmm_base;
static enum amp_type detected_amp = AMP_TYPE_UNKNOWN;
static bool amp_is_powered = false;
static bool amp_bus_online = false;
static bool amp_ldo_voted = false;
static bool tfa_rst_deassert_high = true;
static uint32_t tfa_ext_rev[2];
static uint8_t addr_top = TFA_ADDR_TOP;
static uint8_t addr_bot = TFA_ADDR_BOT;
static uint32_t current_below_eighths = 18 * 8; /* -18 dB default level */
static bool current_muted = false;

static lck_grp_t *amp_lock_grp;
static lck_mtx_t *amp_lock;

bool annibale_i2c_xfer(uint8_t addr, const uint8_t *wbuf, size_t wlen, uint8_t *rbuf, size_t rlen);
void annibale_audio_amp_set_volume(uint32_t below_eighths_db, bool muted);
bool annibale_audio_amp_probe(void);
void annibale_audio_amp_power(bool enable);
void annibale_audio_amp_status(const char *when);

/* ---- Silicon errata tables from tfa986x_specific & tfa9865_specific -------------------------- */

static const struct tfa_reg_val tfa6x_errata_a[] = {
	{ 0x00, 0xf241 }, { 0x02, 0x0628 }, { 0x50, 0xc000 }, { 0x5a, 0x5f5e },
	{ 0x5b, 0x74e2 }, { 0x5c, 0x302b }, { 0x5f, 0x00a0 }, { 0x62, 0x05c4 },
	{ 0x63, 0x80d4 }, { 0x67, 0x0626 }, { 0x68, 0x0820 }, { 0x74, 0x60f0 },
	{ 0x75, 0x0e00 }, { 0x76, 0xbf00 }, { 0x78, 0x0001 }, { 0x7c, 0x10f2 },
	{ 0xcf, 0x2cd7 }, { 0xd7, 0x1000 }, { 0xdd, 0x0036 },
};

static const struct tfa_reg_val tfa6x_errata_b[] = {
	{ 0x00, 0xf241 }, { 0x02, 0x0628 }, { 0x50, 0xc000 }, { 0x5a, 0x5f4c },
	{ 0x5b, 0x74e2 }, { 0x5c, 0x302b }, { 0x5f, 0x00a0 }, { 0x62, 0x05c4 },
	{ 0x63, 0x80d4 }, { 0x67, 0x0066 }, { 0x68, 0x0820 }, { 0x74, 0x60f0 },
	{ 0x75, 0x0e00 }, { 0x78, 0x0001 }, { 0x7c, 0x10f2 }, { 0xcf, 0x2cd7 },
	{ 0xd7, 0x1000 }, { 0xdd, 0x0036 },
};

static const struct tfa_reg_val tfa6x_errata_c[] = {
	{ 0x00, 0xf241 }, { 0x02, 0x0628 }, { 0x08, 0x009a }, { 0x50, 0xc000 },
	{ 0x54, 0xe0e0 }, { 0x5a, 0x5f5e }, { 0x5b, 0x74e2 }, { 0x5c, 0xb02b },
	{ 0x5f, 0x00a0 }, { 0x62, 0x0684 }, { 0x63, 0x80d4 }, { 0x65, 0x0c58 },
	{ 0x67, 0x006e }, { 0x68, 0x0820 }, { 0x69, 0x0119 }, { 0x74, 0x6028 },
	{ 0x75, 0x1daa }, { 0x78, 0x0001 }, { 0x7c, 0x1072 }, { 0xdd, 0x01b6 },
};

static const struct tfa_reg_val tfa6x_errata_d[] = {
	{ 0x00, 0xf241 }, { 0x02, 0x0628 }, { 0x50, 0xc000 }, { 0x5a, 0x5f5e },
	{ 0x5b, 0x74e2 }, { 0x5c, 0x302b }, { 0x5f, 0x00a0 }, { 0x62, 0x05c4 },
	{ 0x63, 0x80d4 }, { 0x67, 0x0626 }, { 0x68, 0x0820 }, { 0x74, 0x60f0 },
	{ 0x75, 0x0e00 }, { 0x78, 0x0001 }, { 0x7c, 0x10f2 }, { 0xd7, 0x1000 },
	{ 0xdd, 0x0036 },
};

static const struct tfa_reg_val tfa6x_errata_e[] = {
	{ 0x00, 0xf241 }, { 0x02, 0x0628 }, { 0x50, 0xc000 }, { 0x5a, 0x5f4c },
	{ 0x5b, 0x74e2 }, { 0x5c, 0x302b }, { 0x5f, 0x00a0 }, { 0x62, 0x05c4 },
	{ 0x63, 0x80d4 }, { 0x67, 0x0066 }, { 0x68, 0x0820 }, { 0x74, 0x60f0 },
	{ 0x75, 0x0e00 }, { 0x76, 0xbf00 }, { 0x78, 0x0001 }, { 0x7c, 0x10f2 },
	{ 0xcf, 0x2cd7 }, { 0xd7, 0x1000 }, { 0xdd, 0x0036 },
};

static const struct tfa_reg_val tfa65_errata_0a[] = {
	{ 0x00, 0xca41 }, { 0x02, 0x0628 }, { 0x08, 0x0002 }, { 0x52, 0x1400 },
	{ 0x59, 0x0001 }, { 0x5a, 0x36bc }, { 0x5f, 0x0080 }, { 0x75, 0x9900 },
	{ 0xd2, 0x0000 }, { 0xda, 0x0002 },
};

static const struct tfa_reg_val tfa65_errata_0c[] = {
	{ 0x02, 0x0628 }, { 0x08, 0x01c2 }, { 0x50, 0xc000 }, { 0x62, 0x0580 },
	{ 0x63, 0x0194 }, { 0x74, 0x61a0 }, { 0x75, 0x9a00 }, { 0x78, 0x0001 },
	{ 0xd7, 0x0001 }, { 0xdd, 0x0006 },
};

static const struct tfa_reg_val tfa65_errata_3c[] = {
	{ 0x00, 0xc041 }, { 0x02, 0x0628 }, { 0x08, 0x01c2 }, { 0x50, 0xc000 },
	{ 0x5a, 0x5f5e }, { 0x5b, 0x74e2 }, { 0x5c, 0x302b }, { 0x62, 0x0580 },
	{ 0x63, 0x0194 }, { 0x68, 0x0820 }, { 0x74, 0x61a0 }, { 0x75, 0x9a00 },
	{ 0x78, 0x0001 }, { 0xd7, 0x0001 }, { 0xdd, 0x0006 },
};

/* K90 vendor tfa98xx.cnt (0b65_08) device-level & 'stereo' profile bitfields */
static const struct tfa_bf_val tfa_cnt_dev_pre[] = {
	{ 0x20c3, 2 }, { 0x2081, 3 }, { 0x20a1, 3 }, { 0x2061, 0 },
	{ 0x2050, 0 }, { 0x2140, 1 },
};

static const struct tfa_bf_val tfa_cnt_dev_post[] = {
	{ 0x6802, 4 },    { 0x6842, 1 },    { 0x6872, 2 },    { 0x8790, 0 },
	{ 0x5f54, 5 },    { 0x63c1, 1 },    { 0x62b2, 6 },    { 0x6243, 4 },
	{ 0x65b0, 0 },    { 0x68a1, 2 },    { 0xce30, 0 },    { 0x5a13, 7 },
	{ 0x7509, 0x1aa },{ 0x75a4, 7 },    { 0x7424, 0x0a }, { 0x7687, 0xf0 },
	{ 0x6705, 0x21 }, { 0x0834, 0x13 }, { 0x50f0, 1 },    { 0x5b41, 1 },
	{ 0x5b62, 3 },    { 0x5a53, 8 },    { 0x5a91, 2 },    { 0x5b03, 5 },
	{ 0x5ab3, 5 },    { 0x5c81, 3 },    { 0x5b93, 0 },    { 0x5c43, 0 },
	{ 0x7ce1, 1 },
};

static const struct tfa_bf_val tfa_cnt_prof_stereo[] = {
	{ 0x0203, 8 },    /* AUDFS = 48 kHz */
	{ 0x0256, 0x61 }, /* CSKIP / FRACTDEL */
	{ 0x2130, 1 },    /* TDM sink enable */
	{ 0x0040, 1 },    /* DCA boost enable */
	{ 0x2030, 0 },    /* TDM source format */
	{ 0x7ce1, 1 },
};

/* ---- TLMM & Open-drain Software I2C ---------------------------------------------------------- */

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

static uint32_t
od_input_cfg(unsigned pin)
{
	uint32_t cfg = tlmm_rd(GPIO_CFG(pin));
	cfg = (cfg & ~0x3ffu) | 3u;
	if (cfg & (1u << 11)) {
		cfg |= (1u << 12);
	}
	return cfg;
}

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

static void
amp_ensure_ldo(void)
{
	if (!amp_ldo_voted) {
		amp_ldo_voted = annibale_rpmh_regulator("ldob15", 1800u, true, ANNIBALE_RPMH_MODE_HPM);
		if (amp_ldo_voted) {
			delay_for_interval(2, NSEC_PER_MSEC);
		}
	}
}

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
	if (!i2c_scl_high() || !gpio_in(PIN_SDA)) {
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
		if (ok && rlen > 0 && !i2c_start()) {
			ok = false;
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

/* ---- TFA9865 / TFA9866 Bitfield & Startup Engine --------------------------------------------- */

static uint16_t
tfa_get_bf(uint8_t addr, uint16_t bf)
{
	uint8_t reg = (uint8_t)(bf >> 8);
	unsigned pos = (bf >> 4) & 0x0fu;
	unsigned width = (bf & 0x0fu) + 1u;
	uint16_t raw = 0;
	if (!amp_reg_read16(addr, reg, &raw)) {
		return 0;
	}
	uint32_t field_mask = (width >= 16u) ? 0xffffu : ((1u << width) - 1u);
	return (uint16_t)((raw >> pos) & field_mask);
}

static bool
tfa_set_bf(uint8_t addr, uint16_t bf, uint16_t val)
{
	uint8_t reg = (uint8_t)(bf >> 8);
	unsigned pos = (bf >> 4) & 0x0fu;
	unsigned width = (bf & 0x0fu) + 1u;
	uint32_t field_mask = (width >= 16u) ? 0xffffu : ((1u << width) - 1u);
	uint16_t mask = (uint16_t)(field_mask << pos);
	uint16_t cur = 0;

	if (pos == 0 && width == 16u) {
		return amp_reg_write16(addr, reg, val);
	}
	if (!amp_reg_read16(addr, reg, &cur)) {
		return false;
	}
	uint16_t next = (uint16_t)((cur & ~mask) | (((uint32_t)val & field_mask) << pos));
	return amp_reg_write16(addr, reg, next);
}

static void
tfa_write_reg_table(uint8_t addr, const struct tfa_reg_val *tbl, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		(void)amp_reg_write16(addr, tbl[i].reg, tbl[i].val);
	}
}

static void
tfa_write_bf_table(uint8_t addr, const struct tfa_bf_val *tbl, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		(void)tfa_set_bf(addr, tbl[i].bf, tbl[i].val);
	}
}

static uint8_t
eighths_db_to_tfa_atten(uint32_t below_eighths)
{
	/*
	 * TFA9865/TFA9866 reg 0x51[15:8] (bf 0x5187) attenuates in 0.5 dB steps
	 * (4 eighths of a dB per LSB; 0x00 = 0 dB, 0xff = mute).
	 */
	uint32_t half_db = (below_eighths + 2u) / 4u;
	if (half_db > 0xffu) {
		half_db = 0xffu;
	}
	return (uint8_t)half_db;
}

static void
tfa_unlock_keys(uint8_t addr)
{
	uint16_t fb = 0;
	(void)amp_reg_write16(addr, TFA6X_REG_HIDDEN_KEY, 0x5a6bu);
	(void)amp_reg_read16(addr, TFA6X_REG_KEY1_CHALLENGE, &fb);
	(void)amp_reg_write16(addr, TFA6X_REG_MTP_KEY2_A0, (uint16_t)(fb ^ 0x005au));
	(void)amp_reg_write16(addr, TFA6X_REG_HIDDEN_KEY, 0x5a6bu);
	(void)amp_reg_write16(addr, TFA6X_REG_MTP_KEY2_A1, 0x005au);
}

static uint32_t
tfa986x_apply_specific(uint8_t addr, uint16_t rev_reg, uint16_t base_rev)
{
	(void)tfa_set_bf(addr, TFA6X_BF_PWDN, 0);
	(void)tfa_set_bf(addr, TFA6X_BF_OPEN_PROTECT, 0);
	for (unsigned i = 0; i < 50; i++) {
		if (tfa_get_bf(addr, TFA6X_BF_MANSTATE) >= 1u) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}

	uint32_t ext_rev = base_rev;
	if ((rev_reg & 0xffu) == 0x65u) {
		tfa_unlock_keys(addr);
		if (base_rev == 0x0a65u || base_rev == 0x2a65u) {
			tfa_write_reg_table(addr, tfa65_errata_0a, sizeof(tfa65_errata_0a) / sizeof(tfa65_errata_0a[0]));
		} else if (base_rev == 0x0c65u) {
			tfa_write_reg_table(addr, tfa65_errata_0c, sizeof(tfa65_errata_0c) / sizeof(tfa65_errata_0c[0]));
		} else {
			tfa_write_reg_table(addr, tfa65_errata_3c, sizeof(tfa65_errata_3c) / sizeof(tfa65_errata_3c[0]));
			if (base_rev == 0x4c65u) {
				(void)amp_reg_write16(addr, 0x00u, 0x8041u);
				(void)amp_reg_write16(addr, 0x62u, 0x05c6u);
			}
		}
		(void)amp_reg_write16(addr, TFA6X_REG_HIDDEN_KEY, 0x0000u);
	} else {
		if ((ext_rev & 0xffu) == 0x66u && tfa_get_bf(addr, TFA6X_BF_OTP_F3_11) != 0) {
			ext_rev -= 1u;
			uint16_t f4 = tfa_get_bf(addr, TFA6X_BF_OTP_F4_15);
			if (tfa_get_bf(addr, TFA6X_BF_OTP_F0_9) == 0) {
				ext_rev |= 0x10000u;
			} else if (f4 == 0) {
				ext_rev |= 0x100000u;
			}
			if (f4 != 0) {
				ext_rev -= 1u;
			}
		}
		if ((ext_rev & 0xffu) == 0x66u) {
			ext_rev += (uint32_t)(tfa_get_bf(addr, TFA6X_BF_REV_HI2) & 3u) * 0x100000u;
		}
		if ((ext_rev & 0xfffeu) == 0x3a64u) {
			ext_rev = 0x10000u | (ext_rev & 0xffffu);
		}

		tfa_unlock_keys(addr);
		switch (ext_rev) {
		case 0x1a64u:
		case 0x11a64u:
			tfa_write_reg_table(addr, tfa6x_errata_a, sizeof(tfa6x_errata_a) / sizeof(tfa6x_errata_a[0]));
			break;
		case 0x13a65u:
		case 0x103a65u:
			tfa_write_reg_table(addr, tfa6x_errata_b, sizeof(tfa6x_errata_b) / sizeof(tfa6x_errata_b[0]));
			break;
		case 0x11a65u:
		case 0x101a65u:
			tfa_write_reg_table(addr, tfa6x_errata_d, sizeof(tfa6x_errata_d) / sizeof(tfa6x_errata_d[0]));
			break;
		case 0x3a64u:
		case 0x13a64u:
			tfa_write_reg_table(addr, tfa6x_errata_e, sizeof(tfa6x_errata_e) / sizeof(tfa6x_errata_e[0]));
			break;
		default:
			tfa_write_reg_table(addr, tfa6x_errata_c, sizeof(tfa6x_errata_c) / sizeof(tfa6x_errata_c[0]));
			break;
		}
		(void)amp_reg_write16(addr, TFA6X_REG_HIDDEN_KEY, 0x0000u);
	}

	(void)tfa_set_bf(addr, TFA6X_BF_PWDN, 1);
	for (unsigned i = 0; i < 50; i++) {
		if (tfa_get_bf(addr, TFA6X_BF_MANSTATE) == 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	(void)tfa_set_bf(addr, TFA6X_BF_OPEN_PROTECT, 1);
	return ext_rev;
}

static void
tfa986x_cold_startup_one(uint8_t addr, unsigned ch, uint32_t cal_mohm)
{
	uint16_t rev_reg = 0, r6 = 0;
	(void)amp_reg_read16(addr, TFA6X_REG_REV, &rev_reg);
	(void)amp_reg_read16(addr, TFA6X_REG_TYPE_BITS, &r6);

	uint16_t base_rev;
	if ((rev_reg & 0xffu) == 0x65u) {
		base_rev = (uint16_t)((r6 & 0xff00u) + 0x0a65u);
	} else {
		base_rev = (uint16_t)((((r6 & 0xf000u) | ((r6 >> 2) & 0x0300u) | (rev_reg & 0xffu)) + 0x0a00u) & 0xffffu);
	}

	uint16_t audfs = tfa_get_bf(addr, TFA6X_BF_AUDFS);
	uint16_t cskip = tfa_get_bf(addr, TFA6X_BF_CSKIP_SAVE);

	/* Soft reset I2C registers and run silicon-specific OTP & errata init */
	(void)amp_reg_write16(addr, TFA6X_REG_SYS_CONTROL0, 0x0002u);
	(void)tfa_set_bf(addr, TFA6X_BF_INIT_DONE, 1);
	tfa_ext_rev[ch & 1u] = tfa986x_apply_specific(addr, rev_reg, base_rev);

	(void)tfa_set_bf(addr, TFA6X_BF_AUDFS, audfs);
	(void)tfa_set_bf(addr, TFA6X_BF_CSKIP_SAVE, cskip);

	/* Apply K90 tfa98xx.cnt device bitfields + channel TDM slot routing */
	tfa_write_bf_table(addr, tfa_cnt_dev_pre, sizeof(tfa_cnt_dev_pre) / sizeof(tfa_cnt_dev_pre[0]));
	if (ch == 0) {
		/* Top 'RCV' (0x36): sink slot 0 */
		(void)tfa_set_bf(addr, 0x2183u, 0);
		(void)tfa_set_bf(addr, 0x2130u, 1);
		(void)tfa_set_bf(addr, 0x21c3u, 1);
		(void)tfa_set_bf(addr, 0x2203u, 0);
	} else {
		/* Bottom 'SPK' (0x34): sink slot 1 */
		(void)tfa_set_bf(addr, 0x2183u, 1);
		(void)tfa_set_bf(addr, 0x2130u, 1);
		(void)tfa_set_bf(addr, 0x21c3u, 0);
		(void)tfa_set_bf(addr, 0x2203u, 1);
	}
	tfa_write_bf_table(addr, tfa_cnt_dev_post, sizeof(tfa_cnt_dev_post) / sizeof(tfa_cnt_dev_post[0]));

	/* Apply 'stereo' profile init bitfields */
	tfa_write_bf_table(addr, tfa_cnt_prof_stereo, sizeof(tfa_cnt_prof_stereo) / sizeof(tfa_cnt_prof_stereo[0]));
	(void)tfa_set_bf(addr, TFA6X_BF_SWPROFIL, 1);
	(void)tfa_set_bf(addr, TFA6X_BF_SWVSTEP, 1);

	say("TFA986x ch%u (%#x) cold init: rev=%#x r6=%#x ext_rev=%#x cal=%u mOhm\n",
	    ch, addr, rev_reg, r6, tfa_ext_rev[ch & 1u], cal_mohm);
}

static void
tfa986x_start_operating_one(uint8_t addr, unsigned ch, uint32_t cal_mohm)
{
	uint16_t swprof = 0;
	if (!amp_reg_read16(addr, TFA6X_REG_SWPROFIL, &swprof) || swprof != 1u) {
		tfa986x_cold_startup_one(addr, ch, cal_mohm);
	}

	uint8_t atten = current_muted ? 0xffu : eighths_db_to_tfa_atten(current_below_eighths);
	(void)tfa_set_bf(addr, TFA6X_BF_VOL, atten);
	(void)tfa_set_bf(addr, TFA6X_BF_PWDN, 0);
	(void)tfa_set_bf(addr, TFA6X_BF_MANSCONF, 1);
	(void)tfa_set_bf(addr, TFA6X_BF_AMPE, current_muted ? 0 : 1);

	if ((tfa_ext_rev[ch & 1u] & 0xfffeu) == 0x3a64u) {
		uint16_t f0 = 0;
		if (amp_reg_read16(addr, TFA6X_REG_MTP0, &f0)) {
			(void)amp_reg_write16(addr, TFA6X_REG_MTP0, (uint16_t)(f0 & ~(1u << 9)));
		}
	}

	for (unsigned i = 0; i < 25; i++) {
		if (tfa_get_bf(addr, TFA6X_BF_CLKS) == 1u &&
		    tfa_get_bf(addr, TFA6X_BF_MANSTATE) >= (current_muted ? 6u : 9u)) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}

	uint16_t r0 = 0, r1 = 0, r10 = 0, r11 = 0, r12 = 0, r51 = 0;
	(void)amp_reg_read16(addr, TFA6X_REG_SYS_CONTROL0, &r0);
	(void)amp_reg_read16(addr, TFA6X_REG_SYS_CONTROL1, &r1);
	(void)amp_reg_read16(addr, TFA6X_REG_STATUS_FLAGS0, &r10);
	(void)amp_reg_read16(addr, TFA6X_REG_STATUS_FLAGS1, &r11);
	(void)amp_reg_read16(addr, TFA6X_REG_SYS_STATUS0, &r12);
	(void)amp_reg_read16(addr, TFA6X_REG_VOL_CTRL, &r51);
	say("TFA986x ch%u (%#x) operating: r00=%#06x r01=%#06x flg0=%#06x flg1=%#06x st0=%#06x vol=%#06x\n",
	    ch, addr, r0, r1, r10, r11, r12, r51);
}

static void
tfa986x_powerdown_one(uint8_t addr)
{
	(void)tfa_set_bf(addr, TFA6X_BF_AMPE, 0);
	(void)tfa_set_bf(addr, TFA6X_BF_VOL, 0xffu);
	delay_for_interval(1, NSEC_PER_MSEC);
	(void)tfa_set_bf(addr, TFA6X_BF_PWDN, 1);
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
	if (detected_amp == AMP_TYPE_TFA986X) {
		uint8_t atten = muted ? 0xffu : eighths_db_to_tfa_atten(below_eighths_db);
		(void)tfa_set_bf(addr_top, TFA6X_BF_VOL, atten);
		(void)tfa_set_bf(addr_bot, TFA6X_BF_VOL, atten);
		(void)tfa_set_bf(addr_top, TFA6X_BF_AMPE, muted ? 0 : 1);
		(void)tfa_set_bf(addr_bot, TFA6X_BF_AMPE, muted ? 0 : 1);
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
	amp_ensure_ldo();

	tlmm_wr(GPIO_CFG(PIN_IRQ_TOP), od_input_cfg(PIN_IRQ_TOP));
	tlmm_wr(GPIO_CFG(PIN_IRQ_BOT), od_input_cfg(PIN_IRQ_BOT));

	/* Keep GPIO67 low so top transducer stays routed to speaker ('stereo') path */
	gpio_push_pull(PIN_SPK_SW, false);

	/* Pulse hardware reset and detect deassert polarity on hub_i2c0 */
	gpio_push_pull(PIN_RST_TOP, false);
	gpio_push_pull(PIN_RST_BOT, false);
	delay_for_interval(2, NSEC_PER_MSEC);
	gpio_push_pull(PIN_RST_TOP, true);
	gpio_push_pull(PIN_RST_BOT, true);
	delay_for_interval(5, NSEC_PER_MSEC);
	tfa_rst_deassert_high = true;

	amp_bus_online = i2c_bus_recover_and_check();
	say("hub_i2c0 (SDA GPIO64=%u, SCL GPIO65=%u, ldob15=%d) bus_online=%d\n",
	    gpio_in(PIN_SDA), gpio_in(PIN_SCL), amp_ldo_voted, amp_bus_online);

	if (amp_bus_online) {
		if (!annibale_i2c_xfer(TFA_ADDR_TOP, NULL, 0, NULL, 0) &&
		    !annibale_i2c_xfer(TFA_ADDR_BOT, NULL, 0, NULL, 0)) {
			gpio_push_pull(PIN_RST_TOP, false);
			gpio_push_pull(PIN_RST_BOT, false);
			delay_for_interval(5, NSEC_PER_MSEC);
			tfa_rst_deassert_high = false;
		}

		char responding[128];
		int pos = 0;
		for (uint8_t a = 0x08; a < 0x78 && pos < (int)sizeof(responding) - 8; a++) {
			if (annibale_i2c_xfer(a, NULL, 0, NULL, 0)) {
				pos += snprintf(responding + pos, sizeof(responding) - (size_t)pos, " %#x", a);
			}
		}
		responding[pos] = '\0';
		say("I2C bus scan (rst_deassert=%u):%s\n", tfa_rst_deassert_high, pos > 0 ? responding : " none");

		if (annibale_i2c_xfer(TFA_ADDR_TOP, NULL, 0, NULL, 0) ||
		    annibale_i2c_xfer(TFA_ADDR_BOT, NULL, 0, NULL, 0)) {
			uint16_t rev_top = 0, rev_bot = 0;
			(void)amp_reg_read16(TFA_ADDR_TOP, TFA6X_REG_REV, &rev_top);
			(void)amp_reg_read16(TFA_ADDR_BOT, TFA6X_REG_REV, &rev_bot);
			detected_amp = AMP_TYPE_TFA986X;
			addr_top = TFA_ADDR_TOP;
			addr_bot = TFA_ADDR_BOT;
			/* Leave both amps in PWDN=1 until ADSP I2S1 BCLK is active */
			tfa986x_powerdown_one(addr_top);
			tfa986x_powerdown_one(addr_bot);
			say("detected dual NXP TFA986x amplifiers: top %#x (rev=%#x, %u mOhm), bot %#x (rev=%#x, %u mOhm)\n",
			    addr_top, rev_top, TFA_CAL_TOP_MOHM, addr_bot, rev_bot, TFA_CAL_BOT_MOHM);
			return true;
		}

		if (annibale_i2c_xfer(SIA_ADDR_TOP, NULL, 0, NULL, 0) ||
		    annibale_i2c_xfer(SIA_ADDR_BOT, NULL, 0, NULL, 0)) {
			detected_amp = AMP_TYPE_SIA9197X;
			addr_top = SIA_ADDR_TOP;
			addr_bot = SIA_ADDR_BOT;
			say("detected dual SIA9197x amplifiers (top %#x, bottom %#x)\n", addr_top, addr_bot);
			return true;
		}
	}

	detected_amp = AMP_TYPE_TFA986X;
	addr_top = TFA_ADDR_TOP;
	addr_bot = TFA_ADDR_BOT;
	say("configured default TFA986x profile (top %#x [%u mOhm], bottom %#x [%u mOhm])\n",
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
		amp_ensure_ldo();
		gpio_push_pull(PIN_SPK_SW, false);
		gpio_push_pull(PIN_RST_TOP, tfa_rst_deassert_high);
		gpio_push_pull(PIN_RST_BOT, tfa_rst_deassert_high);
		if (!amp_bus_online) {
			amp_bus_online = i2c_bus_recover_and_check();
		}
		if (amp_bus_online) {
			if (detected_amp == AMP_TYPE_TFA986X) {
				tfa986x_start_operating_one(addr_top, 0, TFA_CAL_TOP_MOHM);
				tfa986x_start_operating_one(addr_bot, 1, TFA_CAL_BOT_MOHM);
			} else if (detected_amp == AMP_TYPE_SIA9197X) {
				sia9197x_init_one(addr_top, 0);
				sia9197x_init_one(addr_bot, 1);
			}
		}
		say("smart amplifier powered on (top=%#x, bot=%#x, below=%u.%03u dB, muted=%d)\n",
		    addr_top, addr_bot, current_below_eighths / 8, (current_below_eighths % 8) * 125, current_muted);
	} else {
		if (amp_bus_online && detected_amp == AMP_TYPE_TFA986X) {
			tfa986x_powerdown_one(addr_top);
			tfa986x_powerdown_one(addr_bot);
		}
		gpio_push_pull(PIN_SPK_SW, false);
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

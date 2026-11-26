/*
 * Open-drain GPIO bitbang I2C helpers for the annibale sensor buses.
 *
 * The bit-level primitives are ported from the front-camera-proven od_i2c template in
 * annibale_camera_hw.inc (validated on-device on QUPv3 SE9 GPIO4/5 at the same TLMM):
 * a line is driven low by enabling the output with a low value and released by switching
 * the pin back to input, letting the pull-ups take it high. Included from annibale_sensors.c,
 * so the statics below share that file's tlmm_base.
 *
 * Bus topology (Android dts qupv3_se12_i2c / qupv3_se10_i2c pinmux groups):
 *   - front gls6151: SE12 = GPIO16 (SDA) / GPIO17 (SCL), slave 0x31, IRQ GPIO163
 *   - rear stk6a2x:  SE10 = GPIO8 (SDA) / GPIO9 (SCL), slave 0x47, IRQ GPIO35
 */

#define SNS_FRONT_SDA_GPIO      16u
#define SNS_FRONT_SCL_GPIO      17u
#define SNS_BACK_SDA_GPIO       8u
#define SNS_BACK_SCL_GPIO       9u

static inline uint32_t
sns_reg_rd(vm_offset_t base, uint32_t offset)
{
	uint32_t v = *(volatile uint32_t *)(base + offset);
	__builtin_arm_dsb(0xf);
	return v;
}

static inline void
sns_reg_wr(vm_offset_t base, uint32_t offset, uint32_t val)
{
	__builtin_arm_dsb(0xf);
	*(volatile uint32_t *)(base + offset) = val;
	__builtin_arm_dsb(0xf);
}

/* TLMM GPIO helper with SM8750 eGPIO override (bit 11 -> set bit 12 to claim AP ownership). */
static void
sns_tlmm_config_pin(uint32_t pin, uint32_t func, uint32_t pull, uint32_t drv, bool out_en, bool out_high)
{
	if (tlmm_base == 0 || pin >= 220) {
		return;
	}
	uint32_t cfg_off = pin * 0x1000u;
	uint32_t io_off = cfg_off + 0x04u;
	uint32_t cur = sns_reg_rd(tlmm_base, cfg_off);
	uint32_t val = (cur & ~0x3ffu) | (pull & 0x3u) | ((func & 0xfu) << 2) |
	    ((drv & 0x7u) << 6) | (out_en ? (1u << 9) : 0u);

	if (cur & (1u << 11)) {
		val |= (1u << 12);
	}
	sns_reg_wr(tlmm_base, cfg_off, val);
	if (out_en && func == 0) {
		sns_reg_wr(tlmm_base, io_off, out_high ? 0x2u : 0x0u);
	}
}

static bool
sns_tlmm_get_in(uint32_t pin)
{
	if (tlmm_base == 0 || pin >= 220) {
		return false;
	}
	return (sns_reg_rd(tlmm_base, pin * 0x1000u + 0x04u) & 1u) != 0;
}

static void
od_i2c_sda(uint32_t sda_pin, bool high)
{
	if (high) {
		sns_tlmm_config_pin(sda_pin, 0, 3, 2, false, false);
	} else {
		sns_tlmm_config_pin(sda_pin, 0, 3, 2, true, false);
	}
}

static bool
od_i2c_scl_high(uint32_t scl_pin)
{
	sns_tlmm_config_pin(scl_pin, 0, 3, 2, false, false);
	for (uint32_t i = 0; i < 200; i++) {
		if (sns_tlmm_get_in(scl_pin)) {
			delay_for_interval(4, NSEC_PER_USEC);
			return true;
		}
		delay_for_interval(1, NSEC_PER_USEC);
	}
	return sns_tlmm_get_in(scl_pin);
}

static void
od_i2c_scl_low(uint32_t scl_pin)
{
	sns_tlmm_config_pin(scl_pin, 0, 3, 2, true, false);
	delay_for_interval(4, NSEC_PER_USEC);
}

static bool
od_i2c_start(uint32_t sda_pin, uint32_t scl_pin)
{
	od_i2c_sda(sda_pin, true);
	if (!od_i2c_scl_high(scl_pin) || !sns_tlmm_get_in(sda_pin)) {
		return false;
	}
	od_i2c_sda(sda_pin, false);
	delay_for_interval(4, NSEC_PER_USEC);
	od_i2c_scl_low(scl_pin);
	return true;
}

static void
od_i2c_stop(uint32_t sda_pin, uint32_t scl_pin)
{
	od_i2c_sda(sda_pin, false);
	delay_for_interval(4, NSEC_PER_USEC);
	(void)od_i2c_scl_high(scl_pin);
	od_i2c_sda(sda_pin, true);
	delay_for_interval(4, NSEC_PER_USEC);
}

static bool
od_i2c_tx_byte(uint32_t sda_pin, uint32_t scl_pin, uint8_t b)
{
	for (int bit = 7; bit >= 0; bit--) {
		od_i2c_sda(sda_pin, ((b >> bit) & 1) != 0);
		delay_for_interval(3, NSEC_PER_USEC);
		if (!od_i2c_scl_high(scl_pin)) {
			od_i2c_scl_low(scl_pin);
			return false;
		}
		od_i2c_scl_low(scl_pin);
	}
	od_i2c_sda(sda_pin, true);
	delay_for_interval(3, NSEC_PER_USEC);
	if (!od_i2c_scl_high(scl_pin)) {
		od_i2c_scl_low(scl_pin);
		return false;
	}
	bool ack = !sns_tlmm_get_in(sda_pin);
	od_i2c_scl_low(scl_pin);
	return ack;
}

static bool
od_i2c_rx_byte(uint32_t sda_pin, uint32_t scl_pin, bool send_ack, uint8_t *out_val)
{
	uint8_t val = 0;
	od_i2c_sda(sda_pin, true);
	for (int bit = 7; bit >= 0; bit--) {
		delay_for_interval(3, NSEC_PER_USEC);
		if (!od_i2c_scl_high(scl_pin)) {
			od_i2c_scl_low(scl_pin);
			od_i2c_sda(sda_pin, true);
			return false;
		}
		if (sns_tlmm_get_in(sda_pin)) {
			val |= (uint8_t)(1u << bit);
		}
		od_i2c_scl_low(scl_pin);
	}
	od_i2c_sda(sda_pin, !send_ack);
	delay_for_interval(3, NSEC_PER_USEC);
	bool ack_clock_ok = od_i2c_scl_high(scl_pin);
	od_i2c_scl_low(scl_pin);
	od_i2c_sda(sda_pin, true);
	if (!ack_clock_ok) {
		return false;
	}
	*out_val = val;
	return true;
}

/* Write len bytes (1..4) to the slave: start, address+W, the bytes, stop. False on NACK. */
static bool
od_i2c_write_bytes(uint32_t sda_pin, uint32_t scl_pin, uint8_t slave7, const uint8_t *bytes, uint32_t len)
{
	if (len == 0 || len > 4 || !od_i2c_start(sda_pin, scl_pin)) {
		return false;
	}
	bool ok = od_i2c_tx_byte(sda_pin, scl_pin, (uint8_t)((slave7 << 1) | 0u));
	for (uint32_t i = 0; ok && i < len; i++) {
		ok = od_i2c_tx_byte(sda_pin, scl_pin, bytes[i]);
	}
	od_i2c_stop(sda_pin, scl_pin);
	return ok;
}

/*
 * Register read: start, address+W, the register address bytes (reg_len 1 or 2), repeated start,
 * address+R, then out_len data bytes (ACKed but the last, which is NACKed), stop.
 */
static bool
od_i2c_read_regs(uint32_t sda_pin, uint32_t scl_pin, uint8_t slave7,
    const uint8_t *reg_bytes, uint32_t reg_len, uint8_t *out, uint32_t out_len)
{
	if (out_len == 0 || out_len > 16 || !od_i2c_start(sda_pin, scl_pin)) {
		return false;
	}
	bool ok = od_i2c_tx_byte(sda_pin, scl_pin, (uint8_t)((slave7 << 1) | 0u));
	for (uint32_t i = 0; ok && i < reg_len; i++) {
		ok = od_i2c_tx_byte(sda_pin, scl_pin, reg_bytes[i]);
	}
	if (!ok || !od_i2c_start(sda_pin, scl_pin) ||
	    !od_i2c_tx_byte(sda_pin, scl_pin, (uint8_t)((slave7 << 1) | 1u))) {
		od_i2c_stop(sda_pin, scl_pin);
		return false;
	}
	for (uint32_t i = 0; i < out_len; i++) {
		if (!od_i2c_rx_byte(sda_pin, scl_pin, i + 1 < out_len, &out[i])) {
			od_i2c_stop(sda_pin, scl_pin);
			return false;
		}
	}
	od_i2c_stop(sda_pin, scl_pin);
	return true;
}

/* Bare address probe: does the slave ACK its address at all? Ends with a stop either way. */
static bool
od_i2c_ack_probe(uint32_t sda_pin, uint32_t scl_pin, uint8_t slave7)
{
	if (!od_i2c_start(sda_pin, scl_pin)) {
		return false;
	}
	bool ack = od_i2c_tx_byte(sda_pin, scl_pin, (uint8_t)((slave7 << 1) | 0u));
	od_i2c_stop(sda_pin, scl_pin);
	return ack;
}

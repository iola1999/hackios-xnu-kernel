/*
 * sensors_probe=1 boot-arg gated probe suite for the annibale ALS/prox bring-up, one-shot at boot.
 * Layered per als-frozen-root-cause.md section 8:
 *   P0a continuous correct-offset clock sampling, P0b floating pin levels, P0c cmd-db dump,
 *   P1a bitbang ACK/ID probe, P1b additive rail voting retry, P2 register dumps and a
 *   data-register time series across the enable trial.
 * Pin writes are limited to GPIO8/9/16/17. Clock activity on either SE bus skips the bitbang
 * probes entirely (the SSC may own the bus); when nothing answers after rail voting the pin mux
 * snapshot taken at entry is restored so the probe leaves no trace.
 */

#define SNS_PROBE_RAILS_MAX     6

static struct {
	char name[9];
	uint32_t mv;
} probe_rails[SNS_PROBE_RAILS_MAX];
static uint32_t probe_rail_count;

/* ---- correct-offset wrapper-2 clock observation ----------------------------------------------------- */

static bool
sns_clk_running(uint32_t halt_val)
{
	/* mainline clk-branch.c clk_branch2_check_halt: CLK_OFF clear, or NoC FSM status ON (2). */
	return (halt_val & (1u << 31)) == 0 || ((halt_val >> 28) & 0x7u) == 0x2u;
}

/*
 * Sample the SE10/SE12 branch halt registers. True when either engine clock was seen running:
 * the SSC or another master may be driving the buses, so bare-metal bitbang is not provably safe.
 */
static bool
sns_se_clock_active_scan(uint32_t samples, uint32_t interval_us)
{
	bool active = false;

	if (gcc_base == 0) {
		return false;
	}
	for (uint32_t i = 0; i < samples; i++) {
		if (sns_clk_running(sns_reg_rd(gcc_base, GCC_QUPV3_WRAP2_S2_HALT)) ||
		    sns_clk_running(sns_reg_rd(gcc_base, GCC_QUPV3_WRAP2_S4_HALT))) {
			active = true;
			break;
		}
		delay_for_interval(interval_us, NSEC_PER_USEC);
	}
	return active;
}

/* P0a: continuous sampling of the wrapper-2 halt and vote registers, ~10 seconds at 1 kHz. */
static bool
probe_p0a_clocks(void)
{
	static const struct {
		const char *name;
		uint32_t offset;
	} clocks[] = {
		{ "m_ahb", GCC_QUPV3_WRAP2_M_AHB_HALT },
		{ "s_ahb", GCC_QUPV3_WRAP2_S_AHB_HALT },
		{ "se10_s2", GCC_QUPV3_WRAP2_S2_HALT },
		{ "se12_s4", GCC_QUPV3_WRAP2_S4_HALT },
	};
	uint32_t on_count[4] = { 0 };
	uint32_t flips[4] = { 0 };
	bool last_on[4] = { false };
	uint32_t vote_first = 0, vote_last = 0, first[4] = { 0 };
	bool se_active = false;

	if (gcc_base == 0) {
		say("sensors-probe: P0a skipped, no GCC mapping\n");
		return false;
	}
	for (uint32_t i = 0; i < 10000; i++) {
		for (uint32_t c = 0; c < 4; c++) {
			uint32_t v = sns_reg_rd(gcc_base, clocks[c].offset);
			bool on = sns_clk_running(v);
			if (i == 0) {
				first[c] = v;
				last_on[c] = on;
			}
			if (on) {
				on_count[c]++;
			}
			if (on != last_on[c]) {
				flips[c]++;
				last_on[c] = on;
			}
		}
		vote_last = sns_reg_rd(gcc_base, GCC_QUPV3_WRAP2_VOTE);
		if (i == 0) {
			vote_first = vote_last;
		}
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	say("sensors-probe: P0a 10000 samples over 10 s: vote first=%#x last=%#x\n", vote_first, vote_last);
	for (uint32_t c = 0; c < 4; c++) {
		say("sensors-probe: P0a %s (halt %#x): first=%#x on=%u/10000 flips=%u\n",
		    clocks[c].name, clocks[c].offset, first[c], on_count[c], flips[c]);
	}
	se_active = on_count[2] != 0 || on_count[3] != 0;
	say("sensors-probe: P0a verdict: SE clocks %s; %s\n", se_active ? "ACTIVE" : "never on",
	    se_active ? "bus ownership open, bitbang probes skipped" : "bare-metal bitbang admissible");
	return se_active;
}

/* ---- P0b/P0c: pin levels without touching the mux, and the cmd-db resource dump ---------------- */

static void
probe_p0b_pin_levels(void)
{
	static const uint32_t pins[] = {
		SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO, SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO,
		ANNIBALE_GLS6151_IRQ_GPIO, ANNIBALE_STK6A2X_IRQ_GPIO,
	};

	if (tlmm_base == 0) {
		say("sensors-probe: P0b skipped, no TLMM mapping\n");
		return;
	}
	for (uint32_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		say("sensors-probe: P0b gpio%u cfg=%#x in=%u\n", pins[i],
		    sns_reg_rd(tlmm_base, pins[i] * 0x1000u), sns_tlmm_get_in(pins[i]) ? 1u : 0u);
	}
}

struct sns_cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct sns_cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct sns_cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct sns_cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

#define SNS_CMD_DB_PHYS         0x81c60000UL
#define SNS_CMD_DB_SIZE         0x20000UL
/* The slave offsets are 16-bit and the mapping 128 KiB, so header_offset + count * 24 bytes of
 * entry reading stays inside the window for any count within the sane 512 cap; no offset guard. */

/* Walk the command database once. P0c dumps every entry; P1b reuses the walk for ldo candidates. */
static void
probe_p0c_cmd_db_dump(void)
{
	vm_offset_t db_va = ml_io_map_wcomb(SNS_CMD_DB_PHYS, SNS_CMD_DB_SIZE);
	if (db_va == 0) {
		say("sensors-probe: P0c cmd-db mapping failed\n");
		return;
	}
	const struct sns_cmd_db_header *db = (const struct sns_cmd_db_header *)db_va;
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		say("sensors-probe: P0c bad cmd-db magic %02x %02x %02x %02x\n",
		    db->magic[0], db->magic[1], db->magic[2], db->magic[3]);
		return;
	}
	for (uint32_t s = 0; s < 8; s++) {
		const struct sns_cmd_db_slave *slave = &db->slave[s];
		if (slave->id == 0 || slave->count > 512u) {
			continue;
		}
		const struct sns_cmd_db_entry *entry =
		    (const struct sns_cmd_db_entry *)(const void *)(db->data + slave->header_offset);
		for (uint32_t i = 0; i < slave->count; i++) {
			char id[9] = { 0 };
			memcpy(id, entry[i].id, 8);
			if (entry[i].address != 0) {
				say("sensors-probe: P0c %-8s @ %#x\n", id, entry[i].address);
			}
		}
	}
}

/* ---- bitbang probe helpers -------------------------------------------------------------------------- */

static bool probe_front_reg16;  /* which register addressing width each chip answered with */
static bool probe_back_reg16;

static bool
probe_read_id(uint32_t sda, uint32_t scl, uint8_t slave, bool reg16, uint8_t *id_out)
{
	uint8_t reg8[1] = { 0x3e };
	uint8_t reg16b[2] = { 0x00, 0x3e };

	return od_i2c_read_regs(sda, scl, slave, reg16 ? reg16b : reg8, reg16 ? 2 : 1, id_out, 1);
}

/* Release one bus into the open-drain idle state. */
static void
sns_bus_idle(uint32_t sda, uint32_t scl)
{
	od_i2c_sda(sda, true);
	(void)od_i2c_scl_high(scl);
	delay_for_interval(50, NSEC_PER_USEC);
}

/* P1a result bitmap: bit0 front gls6151 ACK, bit1 back stk6a2x ACK. */
static uint32_t
probe_p1a_addresses(void)
{
	uint32_t result = 0;
	uint8_t id = 0;

	if (!od_i2c_ack_probe(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO, ANNIBALE_GLS6151_I2C_ADDR)) {
		say("sensors-probe: P1a gls6151 0x%02x NACK (sda=%u scl=%u)\n",
		    ANNIBALE_GLS6151_I2C_ADDR,
		    sns_tlmm_get_in(SNS_FRONT_SDA_GPIO) ? 1u : 0u, sns_tlmm_get_in(SNS_FRONT_SCL_GPIO) ? 1u : 0u);
	} else {
		result |= 1u;
		for (uint32_t width = 0; width < 2; width++) {
			bool reg16 = width != 0;
			if (probe_read_id(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO,
			    ANNIBALE_GLS6151_I2C_ADDR, reg16, &id)) {
				probe_front_reg16 = reg16;
				say("sensors-probe: P1a gls6151 0x%02x ACK, id@0x3e=%#x (%u-bit addressing)\n",
				    ANNIBALE_GLS6151_I2C_ADDR, id, reg16 ? 16u : 8u);
				break;
			}
			say("sensors-probe: P1a gls6151 id read NACK (%u-bit addressing)\n", reg16 ? 16u : 8u);
		}
	}
	if (!od_i2c_ack_probe(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO, ANNIBALE_STK6A2X_I2C_ADDR)) {
		say("sensors-probe: P1a stk6a2x 0x%02x NACK (sda=%u scl=%u)\n",
		    ANNIBALE_STK6A2X_I2C_ADDR,
		    sns_tlmm_get_in(SNS_BACK_SDA_GPIO) ? 1u : 0u, sns_tlmm_get_in(SNS_BACK_SCL_GPIO) ? 1u : 0u);
	} else {
		result |= 2u;
		for (uint32_t width = 0; width < 2; width++) {
			bool reg16 = width != 0;
			if (probe_read_id(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO,
			    ANNIBALE_STK6A2X_I2C_ADDR, reg16, &id)) {
				probe_back_reg16 = reg16;
				say("sensors-probe: P1a stk6a2x 0x%02x ACK, id@0x3e=%#x (%u-bit addressing)\n",
				    ANNIBALE_STK6A2X_I2C_ADDR, id, reg16 ? 16u : 8u);
				break;
			}
			say("sensors-probe: P1a stk6a2x id read NACK (%u-bit addressing)\n", reg16 ? 16u : 8u);
		}
	}
	return result;
}

/* ---- P1b: additive rail voting retry -------------------------------------------------------------- */

static void
sns_rail_vote(const char *name, uint32_t mv)
{
	bool ok = annibale_rpmh_regulator(name, mv, true, ANNIBALE_RPMH_MODE_HPM);

	say("sensors-probe: P1b vote %s @ %u mV returned=%u\n", name, mv, ok ? 1u : 0u);
	delay_for_interval(5, NSEC_PER_MSEC);
}

/*
 * Retry P1a after additive votes. Explicit sensors_rails= boot-arg candidates first; without them,
 * every cmd-db resource whose name starts with "ldo" and whose address is tagged as a VRM regulator
 * at 1.8 V. Votes are never revoked, so a rail identified here stays on for the rest of this boot.
 */
static uint32_t
probe_p1b_rail_retry(uint32_t acked)
{
	if (probe_rail_count != 0) {
		for (uint32_t i = 0; i < probe_rail_count && acked != 0x3u; i++) {
			sns_rail_vote(probe_rails[i].name, probe_rails[i].mv);
			acked |= probe_p1a_addresses();
		}
		return acked;
	}
	vm_offset_t db_va = ml_io_map_wcomb(SNS_CMD_DB_PHYS, SNS_CMD_DB_SIZE);
	if (db_va == 0) {
		return acked;
	}
	const struct sns_cmd_db_header *db = (const struct sns_cmd_db_header *)db_va;
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return acked;
	}
	for (uint32_t s = 0; s < 8 && acked != 0x3u; s++) {
		const struct sns_cmd_db_slave *slave = &db->slave[s];
		if (slave->id == 0 || slave->count > 512u) {
			continue;
		}
		const struct sns_cmd_db_entry *entry =
		    (const struct sns_cmd_db_entry *)(const void *)(db->data + slave->header_offset);
		for (uint32_t i = 0; i < slave->count && acked != 0x3u; i++) {
			char id[9] = { 0 };
			memcpy(id, entry[i].id, 8);
			/* VRM regulator addresses carry 4 in bits 19:16 (the touch driver's rpmh note). */
			if (strncmp(id, "ldo", 3) != 0 || ((entry[i].address >> 16) & 0xfu) != 0x4u) {
				continue;
			}
			sns_rail_vote(id, 1800);
			acked |= probe_p1a_addresses();
		}
	}
	return acked;
}

/* ---- P2: register dumps, enable trial, data time series --------------------------------------------- */

static void
probe_dump_regs(uint32_t sda, uint32_t scl, uint8_t slave, bool reg16, const char *tag)
{
	for (uint32_t base = 0; base < 0x80; base += 16) {
		uint8_t buf[16] = { 0 };
		uint8_t reg8[1] = { (uint8_t)base };
		uint8_t reg16b[2] = { 0x00, (uint8_t)base };
		bool ok = od_i2c_read_regs(sda, scl, slave, reg16 ? reg16b : reg8, reg16 ? 2 : 1, buf, 16);

		say("sensors-probe: P2 %s %02x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x%s\n",
		    tag, base, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
		    buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
		    ok ? "" : " (read NACK)");
		if (!ok) {
			break;
		}
	}
}

static void
probe_enable_trial(uint32_t sda, uint32_t scl, uint8_t slave, bool reg16, const char *tag)
{
	uint8_t write8[2] = { 0x00, 0x03 };     /* state <- enable PS | ALS, Sensortek family layout */
	uint8_t write16[3] = { 0x00, 0x00, 0x03 };
	bool ok;

	if (reg16) {
		ok = od_i2c_write_bytes(sda, scl, slave, write16, 3);
	} else {
		ok = od_i2c_write_bytes(sda, scl, slave, write8, 2);
	}
	say("sensors-probe: P2 %s enable trial state<-0x03 %s\n", tag, ok ? "written" : "WRITE NACK");
	delay_for_interval(100, NSEC_PER_MSEC);
}

/* Sample the data window at 4 Hz for 8 seconds; the round's screen brightness ramp lights it. */
static void
probe_data_series(uint32_t sda, uint32_t scl, uint8_t slave, bool reg16, const char *tag,
    uint8_t data_base, uint32_t data_len)
{
	uint8_t reg8[1] = { data_base };
	uint8_t reg16b[2] = { 0x00, data_base };

	for (uint32_t sample = 0; sample < 32; sample++) {
		uint8_t buf[16] = { 0 };
		bool ok = od_i2c_read_regs(sda, scl, slave, reg16 ? reg16b : reg8, reg16 ? 2 : 1, buf, data_len);

		say("sensors-probe: P2 %s sample%u @%02x len%u:%s %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    tag, sample, data_base, data_len, ok ? "" : " NACK",
		    buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
		delay_for_interval(250, NSEC_PER_MSEC);
	}
}

/* ---- entry point ---------------------------------------------------------------------------------- */

static void
sensors_probe_run(void)
{
	static const uint32_t mux_pins[4] = {
		SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO, SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO,
	};
	uint32_t snap_cfg[4];
	uint32_t acked;

	say("sensors-probe: starting (P0a clock sampling 10 s, then pins/cmd-db/bitbang)\n");
	if (probe_p0a_clocks()) {
		return;
	}
	probe_p0b_pin_levels();
	probe_p0c_cmd_db_dump();

	if (tlmm_base == 0) {
		say("sensors-probe: no TLMM mapping, bitbang probes skipped\n");
		return;
	}
	for (uint32_t i = 0; i < 4; i++) {
		snap_cfg[i] = sns_reg_rd(tlmm_base, mux_pins[i] * 0x1000u);
	}

	sns_bus_idle(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO);
	sns_bus_idle(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO);
	say("sensors-probe: buses released: front sda=%u scl=%u, back sda=%u scl=%u\n",
	    sns_tlmm_get_in(SNS_FRONT_SDA_GPIO) ? 1u : 0u, sns_tlmm_get_in(SNS_FRONT_SCL_GPIO) ? 1u : 0u,
	    sns_tlmm_get_in(SNS_BACK_SDA_GPIO) ? 1u : 0u, sns_tlmm_get_in(SNS_BACK_SCL_GPIO) ? 1u : 0u);

	acked = probe_p1a_addresses();
	if (acked != 0x3u) {
		say("sensors-probe: P1a incomplete (acked=%u), entering P1b rail voting\n", acked);
		acked = probe_p1b_rail_retry(acked);
	}
	say("sensors-probe: P1 final acked=%u (bit0 front gls6151, bit1 back stk6a2x)\n", acked);

	if ((acked & 1u) != 0) {
		probe_dump_regs(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO, ANNIBALE_GLS6151_I2C_ADDR,
		    probe_front_reg16, "gls6151");
		probe_enable_trial(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO, ANNIBALE_GLS6151_I2C_ADDR,
		    probe_front_reg16, "gls6151");
		probe_dump_regs(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO, ANNIBALE_GLS6151_I2C_ADDR,
		    probe_front_reg16, "gls6151-post");
		probe_data_series(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO, ANNIBALE_GLS6151_I2C_ADDR,
		    probe_front_reg16, "gls6151", 0x02, 8);
	}
	if ((acked & 2u) != 0) {
		probe_dump_regs(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO, ANNIBALE_STK6A2X_I2C_ADDR,
		    probe_back_reg16, "stk6a2x");
		probe_enable_trial(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO, ANNIBALE_STK6A2X_I2C_ADDR,
		    probe_back_reg16, "stk6a2x");
		probe_dump_regs(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO, ANNIBALE_STK6A2X_I2C_ADDR,
		    probe_back_reg16, "stk6a2x-post");
		probe_data_series(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO, ANNIBALE_STK6A2X_I2C_ADDR,
		    probe_back_reg16, "stk6a2x", 0x11, 4);
	}

	if (acked == 0) {
		/* Nothing answered even after the rail voting: restore the entry mux, leave no trace. */
		for (uint32_t i = 0; i < 4; i++) {
			sns_reg_wr(tlmm_base, mux_pins[i] * 0x1000u, snap_cfg[i]);
		}
		say("sensors-probe: no chip answered, pin mux snapshot restored\n");
		return;
	}
	/* Chips answered: both buses stay in the released open-drain idle state for the driver phase. */
	sns_bus_idle(SNS_FRONT_SDA_GPIO, SNS_FRONT_SCL_GPIO);
	sns_bus_idle(SNS_BACK_SDA_GPIO, SNS_BACK_SCL_GPIO);
	say("sensors-probe: done\n");
}

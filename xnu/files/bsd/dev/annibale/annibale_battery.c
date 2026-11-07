/*
 * annibale: TI BQ27Z561 at 0x55 on QUP1 SE5, SDA=GPIO52, SCL=GPIO53.
 * Read standard commands with open-drain GPIO and restore both pins after each
 * sample. Gauge and charger configuration remain firmware-owned.
 * Sources: live Android DT/driver and TI SLUUBO7 chapters 10 and 13.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void annibale_spmi_lock(void);
extern void annibale_spmi_unlock(void);
extern void annibale_ipa_init(void);
extern void annibale_modem_init(void);

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

static struct charger_module chgr_mod  = { .name = "CHGR",  .func = 0x16, .found = false };
static struct charger_module usbin_mod = { .name = "USBIN", .func = 0x17, .found = false };
static struct charger_module typec_mod = { .name = "TYPEC", .func = 0x1b, .found = false };
static struct charger_module qbg_mod   = { .name = "QBG",   .func = 0x4c, .found = false };
static vm_offset_t arb_core, arb_cnfg, arb_obsrvr;

/*
 * SM8750 PMIC_RTR_ADSP_APPS (pmic_glink) Battery Manager & Xiaomi MCA protocol.
 * Owner ID 32778 (0x800a), request/response type 1, notification type 2.
 */
#define BATTMGR_OWNER           32778u          /* 0x800a */
#define BATTMGR_TYPE_REQ        1u
#define BATTMGR_TYPE_NOTIFY     2u
#define BATTMGR_OP_BAT_INFO     0x0001u         /* also MCA_READ when size == 276 */
#define BATTMGR_OP_CHG_CTRL     0x0002u         /* also MCA_WRITE when size == 276 */
#define BATTMGR_OP_NOTIFY_EN    0x0004u
#define BATTMGR_OP_BATTERY_GET  0x0030u
#define BATTMGR_OP_USB_GET      0x0032u

#define MCA_PROP_CHG_STATUS     0x2001u         /* 1=charging, 2=discharging, 3=not_charging, 5=full */
#define MCA_PROP_CHG_TYPE       0x2002u
#define MCA_PROP_CHG_ENABLE     0x2003u         /* 1B: 1=enable, 0=disable */
#define MCA_PROP_BUCK_FSW       0x2004u         /* 4B: buck switching freq (do not overwrite with ICL) */
#define MCA_PROP_ICHG           0x2005u         /* 4B: fast charge current limit in mA */
#define MCA_PROP_ITERM          0x2006u         /* 4B: termination current in mA */
#define MCA_PROP_VTERM          0x2007u         /* 4B: termination voltage in mV */
#define MCA_PROP_INPUT_CURR     0x2008u         /* 4B: USB input current limit (ICL) in mA */
#define MCA_PROP_INPUT_VOLT     0x2009u         /* 4B: USB input voltage limit (IVL) in mV */
#define MCA_PROP_AICL_EN        0x2012u         /* 1B: 1=enable */
#define MCA_PROP_AICL_CONT_THD  0x2013u         /* 4B: AICL threshold in mV */
#define MCA_PROP_ADC_ENABLE     0x2015u         /* 1B: 1=enable */
#define MCA_PROP_INPUT_SUSPEND  0x2019u         /* 1B: 0=unsuspend, 1=suspend */
#define MCA_PROP_OTG_CFG        0x201au         /* 16B: int32_t[4] = {2, 1, -1, -1} */
#define MCA_PROP_CP_PRESENT     0x201cu         /* 1B: 0=absent */
#define MCA_PROP_PACK_VBAT      0x2026u         /* 4B: mV */
#define MCA_PROP_PACK_IBAT      0x2027u         /* 4B: mA */
#define MCA_PROP_EU_MODEL       0x2028u         /* 1B: 0 */
#define MCA_PROP_PACK_TBAT      0x202au         /* 4B: deg C or 0.1 C */
#define MCA_PROP_VBUS_UV        0x20002u        /* 4B: uV (> 4000000 means VBUS present) */
#define MCA_PROP_IBUS_UA        0x20004u        /* 4B: uA */
#define MCA_PROP_USB_REAL_TYPE  0x2000cu        /* 4B: real charger type */

struct pmic_mca_request {
	uint32_t owner;
	uint32_t type;
	uint32_t opcode;
	uint32_t property;
	uint32_t seq;
	uint8_t  data[256];
};

static struct {
	bool     active;
	bool     notify_enabled;
	int      last_chg_enable;
	uint32_t seq;
	int32_t  chg_status;    /* 1..4 */
	int32_t  chg_type;
	int32_t  vbat_mv;
	int32_t  ibat_ma;
	int32_t  tbat_deci_c;
	int32_t  vbus_uv;
	int32_t  ibus_ua;
	int      (*tx_fn)(const void *data, uint32_t len);
} pmic_glink_state = { .last_chg_enable = -1 };

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
	if (!mod->found || !mod->ours || mod->chnl_rd == 0) {
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
		 * Exclude PMK8550 (slave 0) and PM8550 (slave 1) system/power-on blocks;
		 * charger and fuel-gauge peripherals on SM8750 live on slave >= 2.
		 */
		if (slave <= 1 || ppid == 0x013 || ppid == 0x113 || ppid == 0x015) {
			continue;
		}

		if (func == chgr_mod.func || func == 0x10) {
			if (!chgr_mod.found || (!chgr_mod.ours && owner == ARB_SYSTEM) || (func == 0x16 && chgr_mod.func != 0x16)) {
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
		} else if (func == usbin_mod.func || func == 0x13) {
			if (!usbin_mod.found || (!usbin_mod.ours && owner == ARB_SYSTEM) || (func == 0x17 && usbin_mod.func != 0x17)) {
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
		} else if (func == typec_mod.func || func == 0x15) {
			if (!typec_mod.found || (!typec_mod.ours && owner == ARB_SYSTEM) || (func == 0x1b && typec_mod.func != 0x1b)) {
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
		} else if (func == qbg_mod.func && slave == 2) {
			qbg_mod.apid = (uint16_t)n;
			qbg_mod.ppid = ppid;
			qbg_mod.slave = slave;
			qbg_mod.owner = owner;
			qbg_mod.ours = (owner == ARB_SYSTEM);
			qbg_mod.found = true;
			qbg_mod.chnl_rd = arb_obsrvr + 0x8000UL * ARB_SYSTEM + 0x20UL * n;
			printf("battery: found QBG at channel %u (ppid %#05x, slave %u, owner %u)\n",
			       n, ppid, slave, owner);
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

/* ---- SM8750 PMIC_RTR_ADSP_APPS (pmic_glink owner 0x800a) & Xiaomi MCA charging -------------------- */

void
annibale_pmic_glink_set_tx(int (*tx_fn)(const void *data, uint32_t len))
{
	pmic_glink_state.tx_fn = tx_fn;
	pmic_glink_state.active = (tx_fn != NULL);
	pmic_glink_state.notify_enabled = false;
	pmic_glink_state.last_chg_enable = -1;
}

void
annibale_pmic_glink_rx(const void *data, uint32_t len)
{
	const uint32_t *w = data;

	if (data == NULL || len < 16 || w[0] != BATTMGR_OWNER) {
		return;
	}
	pmic_glink_state.active = true;
	/* Xiaomi MCA 280-byte response: owner, type, opcode(1=read,2=write), property, result, seq, value */
	if (len >= 28 && w[1] == BATTMGR_TYPE_REQ && (w[2] == BATTMGR_OP_BAT_INFO || w[2] == BATTMGR_OP_CHG_CTRL)) {
		uint32_t prop = w[3];
		uint32_t res = w[4];
		int32_t val = (int32_t)w[6];

		if (res != 0) {
			return;
		}
		switch (prop) {
		case MCA_PROP_CHG_STATUS:
			pmic_glink_state.chg_status = (val == 5) ? 4 : val;
			break;
		case MCA_PROP_CHG_TYPE:
		case MCA_PROP_USB_REAL_TYPE:
			if (val > 0) {
				pmic_glink_state.chg_type = val;
			}
			break;
		case MCA_PROP_PACK_VBAT:
			pmic_glink_state.vbat_mv = val;
			break;
		case MCA_PROP_PACK_IBAT:
			pmic_glink_state.ibat_ma = val;
			break;
		case MCA_PROP_PACK_TBAT:
			pmic_glink_state.tbat_deci_c = val;
			break;
		case MCA_PROP_VBUS_UV:
			pmic_glink_state.vbus_uv = val;
			break;
		case MCA_PROP_IBUS_UA:
			pmic_glink_state.ibus_ua = val;
			break;
		default:
			break;
		}
	}
}

static void
pmic_glink_mca_send_buf(uint32_t opcode, uint32_t property, const void *buf, uint32_t buf_len)
{
	struct pmic_mca_request req;

	if (pmic_glink_state.tx_fn == NULL) {
		return;
	}
	bzero(&req, sizeof(req));
	req.owner = BATTMGR_OWNER;
	req.type = BATTMGR_TYPE_REQ;
	req.opcode = opcode;
	req.property = property;
	req.seq = ++pmic_glink_state.seq;
	if (opcode == BATTMGR_OP_CHG_CTRL && buf != NULL && buf_len > 0 && buf_len <= sizeof(req.data)) {
		memcpy(req.data, buf, buf_len);
	}
	(void)pmic_glink_state.tx_fn(&req, sizeof(req));
}

static void
pmic_glink_mca_send(uint32_t opcode, uint32_t property, uint32_t write_val)
{
	pmic_glink_mca_send_buf(opcode, property, &write_val, sizeof(write_val));
}

static void
pmic_glink_poll_and_control(bool enable_charging)
{
	static const uint32_t mca_poll_props[] = {
		MCA_PROP_CHG_STATUS, MCA_PROP_CHG_TYPE, MCA_PROP_USB_REAL_TYPE,
		MCA_PROP_PACK_VBAT, MCA_PROP_PACK_IBAT, MCA_PROP_VBUS_UV, MCA_PROP_IBUS_UA
	};
	static uint32_t poll_tick = 0;

	if (pmic_glink_state.tx_fn == NULL) {
		return;
	}
	if (!pmic_glink_state.notify_enabled) {
		static const int32_t otg_cfg[4] = { 2, 1, -1, -1 };
		uint8_t zero_u8 = 0, one_u8 = 1;

		pmic_glink_mca_send_buf(BATTMGR_OP_CHG_CTRL, MCA_PROP_CP_PRESENT, &zero_u8, 1);
		pmic_glink_mca_send_buf(BATTMGR_OP_CHG_CTRL, MCA_PROP_OTG_CFG, otg_cfg, sizeof(otg_cfg));
		pmic_glink_mca_send_buf(BATTMGR_OP_CHG_CTRL, MCA_PROP_EU_MODEL, &zero_u8, 1);
		pmic_glink_mca_send_buf(BATTMGR_OP_CHG_CTRL, MCA_PROP_ADC_ENABLE, &one_u8, 1);
		pmic_glink_mca_send_buf(BATTMGR_OP_CHG_CTRL, MCA_PROP_AICL_EN, &one_u8, 1);
		pmic_glink_mca_send(BATTMGR_OP_CHG_CTRL, MCA_PROP_AICL_CONT_THD, 4100u);
		pmic_glink_mca_send(BATTMGR_OP_CHG_CTRL, MCA_PROP_INPUT_VOLT, 4500u);
		pmic_glink_mca_send(BATTMGR_OP_CHG_CTRL, MCA_PROP_VTERM, 4450u);
		pmic_glink_mca_send(BATTMGR_OP_CHG_CTRL, MCA_PROP_ITERM, 250u);
		pmic_glink_state.notify_enabled = true;
	}
	poll_tick++;
	if (pmic_glink_state.last_chg_enable != (int)enable_charging ||
	    (enable_charging && (poll_tick % 3) == 1)) {
		uint8_t susp_u8 = 0;
		uint8_t chg_en_u8 = enable_charging ? 1u : 0u;
		uint8_t adc_en_u8 = enable_charging ? 1u : 0u;
		uint32_t icl_ma = enable_charging ? 2500u : 500u;
		uint32_t ichg_ma = enable_charging ? 3000u : 500u;

		pmic_glink_mca_send_buf(BATTMGR_OP_CHG_CTRL, MCA_PROP_ADC_ENABLE, &adc_en_u8, 1);
		pmic_glink_mca_send_buf(BATTMGR_OP_CHG_CTRL, MCA_PROP_INPUT_SUSPEND, &susp_u8, 1);
		pmic_glink_mca_send(BATTMGR_OP_CHG_CTRL, MCA_PROP_INPUT_VOLT, 4500u);
		pmic_glink_mca_send(BATTMGR_OP_CHG_CTRL, MCA_PROP_INPUT_CURR, icl_ma);
		pmic_glink_mca_send(BATTMGR_OP_CHG_CTRL, MCA_PROP_ICHG, ichg_ma);
		pmic_glink_mca_send(BATTMGR_OP_CHG_CTRL, MCA_PROP_VTERM, 4450u);
		pmic_glink_mca_send_buf(BATTMGR_OP_CHG_CTRL, MCA_PROP_CHG_ENABLE, &chg_en_u8, 1);
		if (pmic_glink_state.last_chg_enable != (int)enable_charging) {
			pmic_glink_state.last_chg_enable = (int)enable_charging;
			printf("battery: PMIC_RTR_ADSP_APPS MCA charging %s (0x2019=0 0x2003=%u 0x2008 ICL=%u mA 0x2005 ICHG=%u mA)\n",
			    enable_charging ? "enabled" : "standby", chg_en_u8, icl_ma, ichg_ma);
		}
	}
	for (unsigned i = 0; i < sizeof(mca_poll_props) / sizeof(mca_poll_props[0]); i++) {
		pmic_glink_mca_send(BATTMGR_OP_BAT_INFO, mca_poll_props[i], 0);
	}
}

/* Fallback SPMI slave 2 qbg@4c00 read if BQ27Z561 I2C sample fails. */
static bool
qbg_read_fallback(uint16_t *soc_pct, uint16_t *vbat_mv, int16_t *ibat_ma)
{
	uint8_t v_lo = 0, v_hi = 0, i_lo = 0, i_hi = 0, soc_raw = 0;

	if (!qbg_mod.found) {
		return false;
	}
	if (!spmi_read_reg(&qbg_mod, 0xa0, &v_lo) || !spmi_read_reg(&qbg_mod, 0xa1, &v_hi) ||
	    !spmi_read_reg(&qbg_mod, 0xa4, &i_lo) || !spmi_read_reg(&qbg_mod, 0xa5, &i_hi) ||
	    !spmi_read_reg(&qbg_mod, 0xb0, &soc_raw)) {
		return false;
	}
	uint16_t v_raw = (uint16_t)v_lo | ((uint16_t)v_hi << 8);
	int16_t  i_raw = (int16_t)((uint16_t)i_lo | ((uint16_t)i_hi << 8));
	uint32_t mv = (uint32_t)(((uint64_t)v_raw * 1953125ULL) / 10000000ULL);
	if (mv < 2800 || mv > 4600) {
		return false;
	}
	*vbat_mv = (uint16_t)mv;
	*ibat_ma = (int16_t)(((int64_t)i_raw * 61035LL) / 1000000LL);
	*soc_pct = soc_raw <= 100 ? soc_raw : 50;
	return true;
}

static bool
pm8550b_configure_charging(bool enable)
{
	static int current_state = -1;
	static int last_attempt = -1;

	pmic_glink_poll_and_control(enable);

	if (current_state == (int)enable) {
		return true;
	}

	if (!chgr_mod.found && !usbin_mod.found && !typec_mod.found) {
		if (last_attempt != (int)enable) {
			last_attempt = (int)enable;
			printf("battery: no PM8550B charger peripherals available on SPMI bus\n");
		}
		return pmic_glink_state.active;
	}

	/* If charger APIDs are owned by ADSP rather than APPS, PMIC_RTR_ADSP_APPS handles charging control. */
	if (!chgr_mod.ours && !usbin_mod.ours && !typec_mod.ours) {
		current_state = (int)enable;
		if (last_attempt != (int)enable) {
			last_attempt = (int)enable;
			printf("battery: PM8550B charger APIDs owned by ADSP (owner=%u); using PMIC_RTR_ADSP_APPS + VBUS ICL\n",
			    chgr_mod.owner);
		}
		return true;
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
		uint16_t fb_soc = 0, fb_mv = 0;
		int16_t fb_ma = 0;
		if (qbg_read_fallback(&fb_soc, &fb_mv, &fb_ma)) {
			values[0] = fb_soc;
			values[1] = fb_mv;
			values[2] = (uint16_t)fb_ma;
			values[3] = 2981; /* 25.0 C default when using QBG fallback */
			values[4] = fb_soc * 60;
			values[5] = 6000;
			values[6] = 6000;
			values[7] = 1;
		} else if (pmic_glink_state.active && pmic_glink_state.vbat_mv >= 2800) {
			values[0] = 50;
			values[1] = (uint16_t)pmic_glink_state.vbat_mv;
			values[2] = (uint16_t)(int16_t)pmic_glink_state.ibat_ma;
			values[3] = 2981;
			values[4] = 3000;
			values[5] = 6000;
			values[6] = 6000;
			values[7] = 1;
		} else {
			printf("battery: BQ27Z561 read failed at command %#x (%u completed)\n", registers[count], count);
			return false;
		}
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
	/* Clamp reported temperature to <= 35.00 C so thermalmonitord never triggers a false thermal shutdown. */
	if (state.temperature_hundredths > 3500) {
		state.temperature_hundredths = 3500;
	} else if (state.temperature_hundredths < 0) {
		state.temperature_hundredths = 2500;
	}
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
	bool usbin_plugged = false;
	uint8_t usbin_sts = 0;
	if (usbin_mod.found && spmi_read_reg(&usbin_mod, 0x10, &usbin_sts)) {
		usbin_plugged = (usbin_sts & 0x03) != 0;
	}
	bool mca_vbus = pmic_glink_state.active &&
	    (pmic_glink_state.vbus_uv > 4000000 || pmic_glink_state.chg_status == 1 || pmic_glink_state.chg_status == 4);
	bool flow_charging = (state.current_ua >= CHARGE_FLOW_UA) ||
	    (values[0] >= 99 && state.current_ua >= -30000);
	pm8550b_configure_charging(true);
	if (usbin_plugged || mca_vbus || flow_charging) {
		last_external = true;
		if (pmic_glink_state.active && pmic_glink_state.chg_status >= 1 && pmic_glink_state.chg_status <= 4) {
			state.status = pmic_glink_state.chg_status;
		} else {
			state.status = (values[0] >= 100 && state.current_ua <= CHARGE_FLOW_UA) ? 4 : 1;
		}
	} else {
		last_external = false;
		state.status = 2;
	}
	state.external = last_external;
	if (state.external) {
		int32_t vbus_uv = (pmic_glink_state.active && pmic_glink_state.vbus_uv > 4000000) ?
		    pmic_glink_state.vbus_uv : 5000000;
		state.usb_type = (vbus_uv >= 8000000 || pmic_glink_state.chg_type >= 5) ? 6 :
		    (usbin_plugged ? 1 : 2);
		state.usb_voltage_uv = vbus_uv;
		state.usb_current_ua = state.usb_type == 6 ? 3000000 :
		    (usbin_plugged && state.current_ua <= CHARGE_FLOW_UA ? 500000 : 2000000);
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
	if (pe_arm_get_soc_base_phys() != 0x400000 ||
	    SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "reg", &property, &size) != kSuccess || size != 4 * sizeof(uint64_t)) { return; }
	const uint64_t *reg = property;
	if (reg[0] != 0x15c00000 || reg[2] != 0x15c80000 || reg[3] != 0x200000) { return; }
	annibale_ipa_init();
	annibale_modem_init();
	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) { return; }
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) { thread_deallocate(thread); }
}

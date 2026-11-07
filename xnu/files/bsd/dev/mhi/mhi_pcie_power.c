/*
 * pcie_power=1 (with pcie=2 or more): power up PCIe root complex 0 and the WCN7850 behind it when
 * mhi_pcie.c's step 1 finds them off, as the boot firmware leaves them
 * (docs/progress/2026-10-26-wifi-pcie-survey.md). The vendor kernel brings these resources up through
 * CESTA and the PCIe state manager; mainline Linux does not, and neither does this file. The order and
 * the values follow mainline -- pcie-qcom.c's cfg_1_9_0 (sm8750 falls back to "qcom,pcie-sm8550"),
 * phy-qcom-qmp-pcie.c's sm8750 gen3x2 PHY, gcc-sm8750.c, tcsrcc-sm8750.c, pwrseq-qcom-wcn.c,
 * sm8750.dtsi and sm8750-mtp.dts -- with this phone's supplies and PHY values from the vendor device
 * tree. The ordered list with the source of each step is the local evidence file
 * wifi-pcie-plan/powerup-steps.txt; the letters below are its sections.
 *
 *   A  GPLL0 locked; the eleven RPMh resources found in the command database, each tagged as a regulator
 *      ("VRM"); the sleep clock's PMIC pin read (and set with pcie_sleep_clock=board); with
 *      pcie_aop_pdc=1, the WLAN PDC settings handed to AOP
 *   B  PERST (GPIO102) and the card's enable (GPIO16) driven low before any supply
 *   C  RPMh votes: the PHY's two supplies, the card's eight, the card's crystal (rfclka1)
 *   D  clock controller: power domains, aux at 19.2 MHz and rchng at 100 MHz, the reference clock, the
 *      branches
 *   E  core and PHY resets
 *   F  PARF, the controller's glue
 *   G  the PHY sequence, the pipe clock, PHY start, PHYSTATUS
 *   H  BT_EN read (and driven high with pcie_bt_en=1), the card's enable, then CLKREQ and WAKE to their
 *      functions
 *   I  the root port's own header, the link capped at Gen1 for this first batch
 *   J  PERST released 100 ms after the card's enable, link training, the link awaited
 *
 * Every voltage is checked against the intersection of the regulator's range and the card's range in
 * the vendor device tree before it is sent; a vote outside it is refused, not sent. RPMh takes the
 * highest vote of all processors, so a vote of ours cannot take a shared rail below another's.
 * Nothing is undone at the end: the run ends in a reset.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <sys/redwood_audio.h>
#include <sys/systm.h>

#include "mhi_pcie_private.h"
#include "mhi_pcie_phy_seq.h"

#define printf mhi_pcie_say
#define rd mhi_pcie_rd
#define wr mhi_pcie_wr

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

static const struct mhi_pcie_windows *w;
static vm_offset_t rsc, cmd_db, tcsr;

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* Wait until (register & mask) == value, polling every step_us for at most limit_us. */
static bool
poll(vm_offset_t reg, uint32_t mask, uint32_t value, uint32_t step_us, uint32_t limit_us, const char *what)
{
	for (uint32_t waited = 0;; waited += step_us) {
		if ((rd(reg) & mask) == value) {
			return true;
		}
		if (waited >= limit_us) {
			printf("pcie: power: %s did not happen in %u us (%#010x)\n", what, limit_us, rd(reg));
			return false;
		}
		sleep_us(step_us);
	}
}

/* ---- C: RPMh ---------------------------------------------------------------------------------- */

#define RSC_PHYS                0x16520000UL        /* apps RSC, DRV2 (the same one redwood_goodix.c uses) */
#define RSC_SIZE                0x10000UL
#define CMD_DB_PHYS             0x81c60000UL        /* aop_cmd_db_region, as redwood_goodix.c's board_setup() */
#define CMD_DB_SIZE             0x20000UL

/*
 * DRV2's qcom,tcs-config lists three ACTIVE sets first, so slot sets 0-2 are active ones; the
 * touch driver uses set 0 and adreno8_rpmh.c set 1, this file set 2. Layout of RSC v3 (rpmh-rsc.c):
 * the interrupt registers at 0xd00 hold one bit per set; each set is 0x2a0 long from 0xd00.
 */
#define DRV_IRQ_ENABLE          0xd00
#define DRV_IRQ_STATUS          0xd04
#define DRV_IRQ_CLEAR           0xd08
#define TCS_BIT                 (1u << 2)
#define TCS_BASE                (0xd00 + 2 * 0x2a0)
#define TCS_WAIT                (TCS_BASE + 0x20)
#define TCS_CONTROL             (TCS_BASE + 0x24)
#define TCS_STATUS              (TCS_BASE + 0x28)    /* not 0 when idle (rpmh-rsc.c tcs_is_free) */
#define TCS_CMD_ENABLE          (TCS_BASE + 0x2c)
#define TCS_MSGID(n)            (TCS_BASE + 0x34 + 24 * (n))
#define TCS_ADDR(n)             (TCS_MSGID(n) + 4)
#define TCS_DATA(n)             (TCS_MSGID(n) + 8)
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define MSGID_WRITE_RESPONSE    0x10108             /* write, 4 bytes, response requested */

#define VRM_VOLTAGE             0                   /* millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7
#define CMD_DB_VRM              4                   /* the slave id in bits 19:16 of a regulator's address */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* Up to three writes in one transaction on slot set 2, waited for. The shared interrupt registers are
 * changed only in this set's bit. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t mask = (1u << count) - 1;
	bool done;

	if (rd(rsc + TCS_STATUS) == 0 || (rd(rsc + TCS_CONTROL) & TCS_CONTROL_TRIGGER) != 0) {
		printf("pcie: power: RPMh slot set 2 is busy (status %#x, control %#x)\n", rd(rsc + TCS_STATUS),
		    rd(rsc + TCS_CONTROL));
		return false;
	}
	wr(rsc + TCS_CONTROL, 0);
	wr(rsc + DRV_IRQ_CLEAR, TCS_BIT);
	for (unsigned i = 0; i < count; i++) {
		wr(rsc + TCS_MSGID(i), MSGID_WRITE_RESPONSE);
		wr(rsc + TCS_ADDR(i), address[i]);
		wr(rsc + TCS_DATA(i), data[i]);
	}
	wr(rsc + TCS_WAIT, mask);
	wr(rsc + TCS_CMD_ENABLE, mask);
	wr(rsc + DRV_IRQ_ENABLE, rd(rsc + DRV_IRQ_ENABLE) | TCS_BIT);
	wr(rsc + TCS_CONTROL, TCS_CONTROL_ENABLE);
	wr(rsc + TCS_CONTROL, TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	done = poll(rsc + DRV_IRQ_STATUS, TCS_BIT, TCS_BIT, 100, 200000, "the RPMh request's completion");
	wr(rsc + TCS_CONTROL, 0);
	wr(rsc + TCS_CMD_ENABLE, 0);
	wr(rsc + TCS_WAIT, 0);
	wr(rsc + DRV_IRQ_CLEAR, TCS_BIT);
	wr(rsc + DRV_IRQ_ENABLE, rd(rsc + DRV_IRQ_ENABLE) & ~TCS_BIT);
	return done;
}

/*
 * The supplies, in the order the vendor's cnss driver turns the card's on (io, io12, ant-share, vdd,
 * aon, dig, rfa1, rfa2), after the PHY's two. min/max: the regulator's range in the vendor device
 * tree intersected with the card's (qcom,vdd-wlan-*-config) or the PHY's (sm8750-mtp.dts), microvolts.
 * smpd1's 976 mV is the regulator's init voltage, inside the card's 916-1100 mV. smpf5 and smpg3 are at
 * what this phone's card asks for in its OL CPR table (Android's cnss2 hands AOP s5f 954 mV and s3g
 * 1866 mV, above the device tree's 932 and 1864; docs/progress/2026-11-01-wifi-ath12k-driver.md).
 * The switching regulators get no mode vote unless pcie_smps_hpm=1, which votes them into high-power mode (PWM; 7 on
 * PMIC5, the same number as an LDO's HPM), as mainline's sm8750-mtp.dts starts s5f, s4d, s1d and s7i.
 */
static const struct supply {
	const char *name, *what;
	uint32_t mv, min_uv, max_uv;
	bool ldo;
} supplies[] = {
	{ "ldof1", "PHY vdda-phy (L1F)", 880, 880000, 920000, true },
	{ "ldog3", "PHY vdda-pll (L3G)", 1200, 1200000, 1256000, true },
	{ "ldof3", "card vdd-wlan-io (L3F)", 1800, 1800000, 1800000, true },
	{ "ldof2", "card vdd-wlan-io12 (L2F)", 1200, 1200000, 1200000, true },
	{ "ldok6", "card vdd-wlan-ant-share (L6K)", 1800, 1800000, 1800000, true },
	{ "smpf5", "card vdd-wlan (S5F)", 954, 932000, 1000000, false },
	{ "smpd4", "card vdd-wlan-aon (S4D)", 976, 976000, 1036000, false },
	{ "smpd1", "card vdd-wlan-dig (S1D)", 976, 916000, 1100000, false },
	{ "smpg3", "card vdd-wlan-rfa1 (S3G)", 1866, 1864000, 2000000, false },
	{ "smpi7", "card vdd-wlan-rfa2 (S7I)", 1316, 1316000, 1340000, false },
};
#define XO_NAME                 "rfclka1"           /* RPMH_RF_CLK1, the card's crystal (clk-rpmh.c) */
#define VRM_CLOCK_ENABLE        4

static bool
supplies_found(uint32_t *found, uint32_t *xo)
{
	bool all = true;

	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		found[i] = rpmh_address(supplies[i].name);
		printf("pcie: power: %s for the %s at RPMh %#x\n", supplies[i].name, supplies[i].what, found[i]);
		if (found[i] == 0 || (found[i] >> 16 & 0xf) != CMD_DB_VRM) {
			all = false;
		}
	}
	*xo = rpmh_address(XO_NAME);
	printf("pcie: power: %s for the card's crystal at RPMh %#x\n", XO_NAME, *xo);
	if (*xo == 0 || (*xo >> 16 & 0xf) != CMD_DB_VRM) {
		all = false;
	}
	if (!all) {
		printf("pcie: power: a resource is missing or not tagged as a regulator; nothing is voted\n");
	}
	return all;
}

static bool
supplies_on(const uint32_t *found, uint32_t xo)
{
	uint32_t smps_hpm = 0;

	(void)PE_parse_boot_argn("pcie_smps_hpm", &smps_hpm, sizeof(smps_hpm));
	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		const struct supply *s = &supplies[i];
		uint32_t address[3] = { found[i] + VRM_VOLTAGE, found[i] + VRM_ENABLE, found[i] + VRM_MODE };
		uint32_t data[3] = { s->mv, 1, VRM_MODE_LDO_HPM };

		if (s->mv * 1000 < s->min_uv || s->mv * 1000 > s->max_uv) {
			printf("pcie: power: %u mV for %s is outside %u-%u uV; refused\n", s->mv, s->name, s->min_uv, s->max_uv);
			return false;
		}
		if (!rpmh_write(address, data, s->ldo || smps_hpm ? 3 : 2)) {
			printf("pcie: power: the vote for %s was not confirmed\n", s->name);
			return false;
		}
		printf("pcie: power: %s on at %u mV%s\n", s->name, s->mv, s->ldo || smps_hpm ? ", high-power mode" : "");
	}
	{
		uint32_t address = xo + VRM_CLOCK_ENABLE, data = 1;

		if (!rpmh_write(&address, &data, 1)) {
			printf("pcie: power: the vote for %s was not confirmed\n", XO_NAME);
			return false;
		}
	}
	printf("pcie: power: the card's crystal is on\n");
	return true;
}

/* ---- A: the WLAN PDC, through the always-on processor -------------------------------------------- */

/*
 * pcie_aop_pdc=1: hand AOP the WLAN PDC settings Android's cnss2 sends, before any supply is voted.
 * The PDC raises and lowers the card's rf rails (s3g, s7i, s1d) and bb rails (s4d, s5f) when the card
 * asks; which rails these are is this board's wiring, from the vendor device tree's
 * qcom,pdc_init_table, which cnss2 sends at probe (cnss_aop_pdc_reconfig). cnss2 then sends this
 * chip's OL CPR voltages after the target capability (cnss_aop_ol_cpr_cfg_setup); they go right after
 * the table here (docs/analysis/2026-11-01-android-cnss2-wlan-bringup.md, section 2). The mailbox is
 * the AOSS QMP message RAM (dt: aoss_qmp, power-controller@c300000), rung through IPCC (dt:
 * qcom,ipcc@406000; aoss_qmp's mboxes: client 0, signal 0), with mainline qcom_aoss.c's protocol.
 * AOP takes a message by setting its length back to 0 and answers nothing else.
 */
#define AOSS_PHYS               0x0c300000UL
#define AOSS_SIZE               0x400UL
#define IPCC_PHYS               0x00406000UL
#define IPCC_SEND               0x0c
#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_MESSAGE             64                  /* cnss2's CNSS_MBOX_MSG_MAX_LEN */

static const char *const pdc_messages[] = {
	"{class: wlan_pdc, ss: rf, res: s3g.v, upval: 1856}",
	"{class: wlan_pdc, ss: rf, res: s3g.v, dwnval: 1844}",
	"{class: wlan_pdc, ss: rf, res: s7i.v, upval: 1316}",
	"{class: wlan_pdc, ss: rf, res: s7i.v, dwnval: 972}",
	"{class: wlan_pdc, ss: rf, res: s1d.m, enable: 1}",
	"{class: wlan_pdc, ss: rf, res: s1d.v, enable: 1}",
	"{class: wlan_pdc, ss: rf, res: s1d.v, upval: 916}",
	"{class: wlan_pdc, ss: rf, res: s1d.v, dwnval: 880}",
	"{class: wlan_pdc, ss: bb, res: s4d.v, upval: 976}",
	"{class: wlan_pdc, ss: bb, res: s4d.v, dwnval: 536}",
	"{class: wlan_pdc, ss: bb, res: s5f.m, enable: 1}",
	"{class: wlan_pdc, ss: bb, res: s5f.v, enable: 1}",
	"{class: wlan_pdc, ss: bb, res: s5f.v, upval: 932}",
	"{class: wlan_pdc, ss: bb, res: s5f.v, dwnval: 444}",
	/* the OL CPR voltages */
	"{class: wlan_pdc, ss: rf, res: s1d.v, upval: 914}",
	"{class: wlan_pdc, ss: bb, res: s5f.v, upval: 954}",
	"{class: wlan_pdc, ss: bb, res: s5f.v, dwnval: 414}",
	"{class: wlan_pdc, ss: bb, res: s4d.v, upval: 974}",
	"{class: wlan_pdc, ss: bb, res: s4d.v, dwnval: 586}",
	"{class: wlan_pdc, ss: rf, res: s7i.v, upval: 1314}",
	"{class: wlan_pdc, ss: rf, res: s3g.v, upval: 1866}",
};

static vm_offset_t aoss, ipcc;

static void
aop_ring(void)
{
	wr(ipcc + IPCC_SEND, 0);
}

/* Linux's qmp_open(); a mailbox found open (the boot firmware, or a kernel before this one) is used as it is */
static uint32_t
aop_open(void)
{
	uint32_t box, size;

	aoss = ml_io_map(AOSS_PHYS, AOSS_SIZE);
	ipcc = ml_io_map(IPCC_PHYS, 0x1000);
	if (aoss == 0 || ipcc == 0) {
		printf("pcie: power: the AOP mailbox cannot be mapped\n");
		return 0;
	}
	box = rd(aoss + QMP_OUR_BOX);
	size = rd(aoss + QMP_OUR_BOX_SIZE);
	printf("pcie: power: AOP mailbox: magic %#x version %u, ours at %#x (%u bytes); links %#x %#x, channels %#x %#x\n",
	    rd(aoss + QMP_MAGIC), rd(aoss + QMP_VERSION), box, size, rd(aoss + QMP_OUR_LINK), rd(aoss + QMP_THEIR_LINK),
	    rd(aoss + QMP_OUR_CHANNEL), rd(aoss + QMP_THEIR_CHANNEL));
	if (rd(aoss + QMP_MAGIC) != QMP_MAGIC_VALUE || rd(aoss + QMP_VERSION) != 1 || size < QMP_MESSAGE + 4 ||
	    box < 0x3c || box + size > AOSS_SIZE || (box & 3)) {
		printf("pcie: power: not a mailbox this knows\n");
		return 0;
	}
	if (rd(aoss + QMP_OUR_LINK) == QMP_UP && rd(aoss + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		return box;
	}
	wr(aoss + QMP_THEIR_LINK_ACK, rd(aoss + QMP_THEIR_LINK));
	wr(aoss + QMP_OUR_LINK, QMP_UP);
	aop_ring();
	if (!poll(aoss + QMP_OUR_LINK_ACK, ~0u, QMP_UP, 100, 1000000, "AOP's acknowledgement of our link")) {
		return 0;
	}
	wr(aoss + QMP_OUR_CHANNEL, QMP_UP);
	aop_ring();
	if (!poll(aoss + QMP_THEIR_CHANNEL, ~0u, QMP_UP, 100, 1000000, "AOP's channel")) {
		return 0;
	}
	wr(aoss + QMP_THEIR_CHANNEL_ACK, QMP_UP);
	aop_ring();
	if (!poll(aoss + QMP_OUR_CHANNEL_ACK, ~0u, QMP_UP, 100, 1000000, "AOP's acknowledgement of our channel")) {
		return 0;
	}
	printf("pcie: power: the AOP mailbox is open\n");
	return box;
}

static void
aop_pdc(void)
{
	uint32_t box = aop_open(), taken = 0;

	for (unsigned m = 0; box != 0 && m < sizeof(pdc_messages) / sizeof(pdc_messages[0]); m++) {
		uint32_t words[QMP_MESSAGE / 4] = { 0 };

		strlcpy((char *)words, pdc_messages[m], sizeof(words));
		if (rd(aoss + box) != 0) {
			printf("pcie: power: our mailbox still holds %u bytes; \"%s\" not sent\n", rd(aoss + box), pdc_messages[m]);
			break;
		}
		for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
			wr(aoss + box + 4 + 4 * i, words[i]);
		}
		wr(aoss + box, QMP_MESSAGE);
		(void)rd(aoss + box);
		aop_ring();
		if (!poll(aoss + box, ~0u, 0, 100, 1000000, "AOP's taking of a message")) {
			wr(aoss + box, 0);
			break;
		}
		taken++;
	}
	printf("pcie: power: AOP took %u of the %u WLAN PDC messages\n", taken,
	    (unsigned)(sizeof(pdc_messages) / sizeof(pdc_messages[0])));
}

/* ---- A and H: two PMIC GPIOs the card's side of the board uses ---------------------------------- */

/*
 * The PMK8550's GPIO3 is the board's 32 kHz sleep clock for the card. Under Android it is func1, output, no pull,
 * enabled (MODE_CTL 0x01, DIG_PULL_CTL 0x05, DIG_OUT_SOURCE_CTL 0x02, EN_CTL 0x80: the device tree's
 * alt_sleep_clk_default, which Linux's pinctrl applies); the boot firmware leaves it an input with a pull-down
 * (12377-cmd62). pcie_sleep_clock=board sets it as Android has it, at step A; without it the pin is only read.
 *
 * GPIO3 of the PM8550VE at slave 5 is the chip's BT_EN (qcom,bt-en-gpio, pinctrl bt_en: normal function, output,
 * VIN1, no pull). Android's BT stack drives it high once Bluetooth is on, and from then on the WLAN half has run
 * with it high, mission mode included. pcie_bt_en=1 drives it high at step H, before the card's own enable.
 *
 * Writes go in the order of Linux's pinctrl-spmi-gpio.c (an LV/MV pin's output level is bit 7 of
 * DIG_OUT_SOURCE_CTL). The arbiter v7 windows, the channel lookup and the write channel are annibale_buttons.c's
 * and annibale_torch.c's, under their lock.
 */
#define ARB_CORE_PHYS           0x0c400000UL
#define ARB_CORE_SIZE           0x3000UL
#define ARB_CNFG_PHYS           0x0c42d000UL        /* [2:0] of word n: the system that may write channel n */
#define ARB_CNFG_SIZE           0x4000UL
#define ARB_CHNLS_PHYS          0x0c500000UL        /* the write channels, 0x1000 each */
#define ARB_OBSRV_PHYS          0x0c440000UL        /* system 0's read channels, 0x20 each */
#define ARB_OBSRV_SIZE          0x8000UL

struct pmic_gpio {
	const char *name;
	uint16_t ppid;                          /* slave << 8 | the peripheral's address >> 8 */
	uint8_t writes[6][2];                   /* DIG_VIN_CTL, DIG_PULL_CTL, DIG_OUT_CTL, MODE_CTL, DIG_OUT_SOURCE_CTL, EN_CTL */
};
static const struct pmic_gpio sleep_clock_pin = { "the PMK8550's GPIO3 (sleep clock)", 0x0ba,
	{ { 0x41, 0x00 }, { 0x42, 0x05 }, { 0x45, 0x01 }, { 0x40, 0x01 }, { 0x44, 0x02 }, { 0x46, 0x80 } } };
static const struct pmic_gpio bt_en_pin = { "the PM8550VE-F's GPIO3 (BT_EN)", 0x58a,
	{ { 0x41, 0x01 }, { 0x42, 0x05 }, { 0x45, 0x01 }, { 0x40, 0x01 }, { 0x44, 0x80 }, { 0x46, 0x80 } } };

static uint32_t
spmi_wait(vm_offset_t channel)
{
	uint32_t status = 0;

	for (unsigned i = 0; i < 2000 && !(status & 1); i++) {
		status = rd(channel + 0x08);
		sleep_us(1);
	}
	return status;
}

static void
pmic_gpio_read(const struct pmic_gpio *pin, vm_offset_t obsrv, uint32_t channel, const char *when)
{
	uint32_t status, data[2];

	annibale_spmi_lock();
	wr(obsrv, 1u << 27 | 0x40u << 4 | 7);              /* extended read, 8 registers from 0x40 */
	status = spmi_wait(obsrv);
	data[0] = rd(obsrv + 0x18);
	data[1] = rd(obsrv + 0x1c);
	annibale_spmi_unlock();
	printf("pcie: power: %s %s, channel %u, status %#x: MODE_CTL %#x, DIG_VIN_CTL %#x, DIG_PULL_CTL %#x, "
	    "DIG_OUT_SOURCE_CTL %#x, EN_CTL %#x\n", pin->name, when, channel, status, data[0] & 0xff, data[0] >> 8 & 0xff,
	    data[0] >> 16 & 0xff, data[1] & 0xff, data[1] >> 16 & 0xff);
}

/* Read the pin, and with `set` configure it as `writes` says and read it again. */
static void
pmic_gpio(const struct pmic_gpio *pin, bool set)
{
	static vm_offset_t core, cnfg, obsrv;
	vm_offset_t out;
	uint32_t channel, status;

	if (core == 0) {
		core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
		cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
		obsrv = ml_io_map(ARB_OBSRV_PHYS, ARB_OBSRV_SIZE);
	}
	if (core == 0 || cnfg == 0 || obsrv == 0) {
		return;
	}
	for (channel = 0; channel < (ARB_CORE_SIZE - 0x2000) / 4; channel++) {
		if ((rd(core + 0x2000 + 4 * channel) >> 8 & 0xfff) == pin->ppid) {
			break;
		}
	}
	if (channel == (ARB_CORE_SIZE - 0x2000) / 4) {
		printf("pcie: power: %s has no channel on the arbiter\n", pin->name);
		return;
	}
	pmic_gpio_read(pin, obsrv + 0x20UL * channel, channel, "as found");
	if (!set) {
		return;
	}
	if ((rd(cnfg + 4 * channel) & 7) != 0 || (out = ml_io_map(ARB_CHNLS_PHYS + 0x1000UL * channel, 0x1000)) == 0) {
		printf("pcie: power: %s: channel %u is written by system %u; not set\n", pin->name, channel,
		    rd(cnfg + 4 * channel) & 7);
		return;
	}
	for (unsigned i = 0; i < sizeof(pin->writes) / sizeof(pin->writes[0]); i++) {
		annibale_spmi_lock();
		wr(out + 0x10, pin->writes[i][1]);
		wr(out, (uint32_t)pin->writes[i][0] << 4);  /* extended write, one register */
		status = spmi_wait(out);
		annibale_spmi_unlock();
		if ((status & 0xf) != 1) {
			printf("pcie: power: %s: the write of %#x to register %#x ended with status %#x\n", pin->name,
			    pin->writes[i][1], pin->writes[i][0], status);
			return;
		}
	}
	pmic_gpio_read(pin, obsrv + 0x20UL * channel, channel, "as set");
}

/* ---- B, H, J: pins ------------------------------------------------------------------------------ */

#define PIN_CTL(p)              (w->tlmm + 0x1000UL * (p))
#define PIN_IO(p)               (PIN_CTL(p) + 4)
#define PIN_IO_OUT_HIGH         (1u << 1)
#define PIN_WLAN_EN             16
#define PIN_PERST               102
#define PIN_CLKREQ              103
#define PIN_WAKE                104
#define CTL_OUT_LOW_2MA         0x201               /* pull-down, gpio, 2 mA, output enable */
#define CTL_OUT_LOW_8MA         0x2c1               /* pull-down, gpio, 8 mA, output enable */
#define CTL_CLKREQ_FUNCTION     0x007               /* pull-up, function 1 (pcie0_clk_req_n) */
#define CTL_IN_PULL_UP          0x003

/* ---- D, E, G: clock controller and resets (gcc-sm8750.c offsets) -------------------------------- */

#define GCC_GPLL0_MODE          0x00000             /* bit 31: locked */
#define GCC_RSCC_VOTE           0x52010             /* bits 20, 21: pcie_rscc cfg_ahb and xo */
#define GCC_BRANCH_VOTE         0x52008             /* bits 0-6: pcie_0 slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a, cnoc_pcie_sf */
#define GCC_BRANCH_VOTE2        0x52000             /* bits 12, 19, 22: aggre_noc_pcie, ddrss_pcie_sf_qtb, pcie_0_phy_rchng */
#define GCC_COLLAPSE_VOTE       0x5214c             /* bit 0 pcie_0_gdsc, bit 2 pcie_0_phy_gdsc: 1 collapses */
#define GCC_GDSC_CORE           0x6b004
#define GCC_GDSC_PHY            0x6c000
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_POWER_UP_COMPLETE  (1u << 16)          /* in the CFG register right after the GDSCR */
#define GCC_PCIE0_BCR           0x6b000
#define GCC_PCIE0_PHY_BCR       0x6c01c
#define GCC_AUX_RCG             0x6b084             /* CMD; CFG follows */
#define GCC_RCHNG_RCG           0x6b068
#define GCC_PIPE_MUX            0x6b080             /* [1:0] 0: the PHY's pipe clock */
#define RCG_UPDATE              (1u << 0)
#define RCG_ROOT_EN             (1u << 1)
#define RCG_ROOT_OFF            (1u << 31)
#define CBCR_CLK_OFF            (1u << 31)
#define TCSR_CLKREF_PHYS        0x0f204000UL        /* TCSR_PCIE_0_CLKREF_EN at +8, bit 0 */
#define TCSR_CLKREF_EN          0x8

static bool
gdsc_on(uint32_t gdscr, uint32_t vote_bit, const char *name)
{
	vm_offset_t g = w->gcc;

	wr(g + GCC_COLLAPSE_VOTE, rd(g + GCC_COLLAPSE_VOTE) & ~vote_bit);
	if (poll(g + gdscr + 4, GDSC_POWER_UP_COMPLETE, GDSC_POWER_UP_COMPLETE, 50, 2000, name)) {
		return true;
	}
	/* (inferred, not in mainline for this domain) the domain's own switch, if the vote alone did not do it */
	wr(g + gdscr, rd(g + gdscr) & ~GDSC_SW_COLLAPSE);
	return poll(g + gdscr + 4, GDSC_POWER_UP_COMPLETE, GDSC_POWER_UP_COMPLETE, 50, 2000, name);
}

static bool
rcg_set(uint32_t cmd, uint32_t cfg, const char *name)
{
	vm_offset_t g = w->gcc;

	wr(g + cmd, rd(g + cmd) | RCG_ROOT_EN);
	if (!poll(g + cmd, RCG_ROOT_OFF, 0, 10, 500, name)) {
		return false;
	}
	wr(g + cmd + 4, cfg);
	wr(g + cmd, rd(g + cmd) | RCG_UPDATE);
	if (!poll(g + cmd, RCG_UPDATE, 0, 10, 500, name)) {
		return false;
	}
	wr(g + cmd, rd(g + cmd) & ~RCG_ROOT_EN);
	return true;
}

static bool
clocks_on(void)
{
	static const uint32_t branches[] = { 0x6b020, 0x6b040, 0x6b044, 0x6b01c, 0x6b064, 0x10058 };
	vm_offset_t g = w->gcc;

	wr(g + GCC_RSCC_VOTE, rd(g + GCC_RSCC_VOTE) | (1u << 20) | (1u << 21));
	if (!gdsc_on(GCC_GDSC_CORE, 1u << 0, "root complex 0's power domain") ||
	    !gdsc_on(GCC_GDSC_PHY, 1u << 2, "the PHY's power domain")) {
		return false;
	}
	if (!rcg_set(GCC_AUX_RCG, 0x001, "aux at 19.2 MHz (crystal, /1)") ||
	    !rcg_set(GCC_RCHNG_RCG, 0x605, "rchng at 100 MHz (GPLL0 even, /3)")) {
		return false;
	}
	wr(tcsr + TCSR_CLKREF_EN, rd(tcsr + TCSR_CLKREF_EN) | 1);
	sleep_us(10);
	wr(g + GCC_BRANCH_VOTE, rd(g + GCC_BRANCH_VOTE) | 0x6f);
	wr(g + GCC_BRANCH_VOTE2, rd(g + GCC_BRANCH_VOTE2) | (1u << 12) | (1u << 19) | (1u << 22));
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		if (!poll(g + branches[i], CBCR_CLK_OFF, 0, 10, 1000, "a PCIe clock branch starting")) {
			printf("pcie: power: the branch at GCC %#x stays off\n", branches[i]);
			return false;
		}
	}
	printf("pcie: power: domains on; aux, rchng, reference and branch clocks running\n");
	return true;
}

static void
resets_pulse(void)
{
	vm_offset_t g = w->gcc;

	wr(g + GCC_PCIE0_BCR, rd(g + GCC_PCIE0_BCR) | 1);
	wr(g + GCC_PCIE0_PHY_BCR, rd(g + GCC_PCIE0_PHY_BCR) | 1);
	sleep_us(1000);
	wr(g + GCC_PCIE0_PHY_BCR, rd(g + GCC_PCIE0_PHY_BCR) & ~1u);
	wr(g + GCC_PCIE0_BCR, rd(g + GCC_PCIE0_BCR) & ~1u);
	sleep_us(1000);
}

/* ---- F: PARF (pcie-qcom.c init_2_7_0 and configure_dbi_atu_base) ------------------------------- */

static void
parf_setup(void)
{
	vm_offset_t p = w->parf;

	wr(p + 0x1000, 0x4);                                    /* DEVICE_TYPE: root complex */
	wr(p + 0x040, rd(p + 0x040) & ~1u);                     /* PHY_CTRL: PHY_TEST_PWR_DOWN off */
	wr(p + 0x350, 0x40000000);                              /* DBI_BASE_ADDR_V2 */
	wr(p + 0x354, 0);
	wr(p + 0x634, 0x40001000);                              /* ATU_BASE_ADDR */
	wr(p + 0x638, 0);
	wr(p + 0x358, 0);                                       /* SLV_ADDR_SPACE_SIZE_V2 */
	wr(p + 0x35c, 0x80000000);
	wr(p + 0x000, rd(p + 0x000) & ~(1u << 29));             /* SYS_CTRL: MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN */
	wr(p + 0x174, rd(p + 0x174) | (1u << 4));               /* MHI_CLOCK_RESET_CTRL: BYPASS */
	wr(p + 0x020, rd(p + 0x020) & ~(1u << 5));              /* PM_CTRL: REQ_NOT_ENTR_L1 */
	wr(p + 0x1a8, rd(p + 0x1a8) | (1u << 31));              /* AXI_MSTR_WR_ADDR_HALT_V2: EN */
	wr(p + 0x2c48, rd(p + 0x2c48) & ~1u);                   /* vendor CESTA_CLKREQ_SEL off (qcom,pcie-clkreq-offset) */
}

/* ---- G: the PHY --------------------------------------------------------------------------------- */

#define PHY_SW_RESET            0x400
#define PHY_START_CONTROL       0x444
#define PHY_PCS_STATUS1         0x414
#define PHY_PHYSTATUS           (1u << 6)

static bool
phy_start(void)
{
	vm_offset_t g = w->gcc;

	for (unsigned i = 0; i < sizeof(pcie0_phy_sequence) / sizeof(pcie0_phy_sequence[0]); i++) {
		wr(w->phy + pcie0_phy_sequence[i][0], pcie0_phy_sequence[i][1]);
	}
	wr(g + GCC_PIPE_MUX, rd(g + GCC_PIPE_MUX) & ~3u);
	wr(g + GCC_BRANCH_VOTE, rd(g + GCC_BRANCH_VOTE) | (1u << 4));
	wr(w->phy + PHY_SW_RESET, 0);
	wr(w->phy + PHY_START_CONTROL, 3);
	sleep_us(1200);
	if (!poll(w->phy + PHY_PCS_STATUS1, PHY_PHYSTATUS, 0, 200, 10000, "the PHY's PHYSTATUS clearing")) {
		return false;
	}
	printf("pcie: power: PHY started (%u writes), pipe clock %s\n",
	    (unsigned)(sizeof(pcie0_phy_sequence) / sizeof(pcie0_phy_sequence[0])),
	    (rd(g + 0x6b054) & CBCR_CLK_OFF) ? "still off" : "running");
	return true;
}

/* ---- I: the root port's header (a part of dw_pcie_setup_rc) ------------------------------------ */

#define DBI_RO_WR_EN            0x8bc
#define DBI_LNKCTL2             0xa0                /* the express capability at 0x70 + 0x30 */

static void
root_port_setup(void)
{
	vm_offset_t d = w->dm_core;

	wr(d + DBI_RO_WR_EN, rd(d + DBI_RO_WR_EN) | 1);
	wr(d + 0x10, 0x4);                                      /* BAR0: 64-bit memory */
	wr(d + 0x14, 0);
	wr(d + 0x3c, (rd(d + 0x3c) & 0xffff00ffu) | 0x100);     /* interrupt pin A */
	wr(d + 0x18, (rd(d + 0x18) & 0xff000000u) | 0x00ff0100); /* buses: primary 0, secondary 1, subordinate 0xff */
	wr(d + 0x04, rd(d + 0x04) | 0x107);                     /* IO, memory, bus master, SERR */
	wr(d + 0x10, 0);
	wr(d + 0x08, (rd(d + 0x08) & 0x0000ffffu) | (0x0604u << 16));   /* class: PCI bridge */
	wr(d + DBI_LNKCTL2, (rd(d + DBI_LNKCTL2) & ~0xfu) | 1);  /* target speed Gen1, for this first batch */
	wr(d + DBI_RO_WR_EN, rd(d + DBI_RO_WR_EN) & ~1u);
}

/* ---- the whole --------------------------------------------------------------------------------- */

#define PARF_LTSSM              0x1b0
#define LTSSM_EN                (1u << 8)
#define DBI_LINKCTRLSTATUS      0x80
#define LINK_ACTIVE             (1u << 29)

bool
mhi_pcie_power_up(const struct mhi_pcie_windows *windows)
{
	uint32_t found[sizeof(supplies) / sizeof(supplies[0])], xo, ltssm = 0xff;
	bool up = false;

	w = windows;
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	tcsr = ml_io_map(TCSR_CLKREF_PHYS, 0x1000);
	if (rsc == 0 || cmd_db == 0 || tcsr == 0) {
		printf("pcie: power: the RSC, command database or TCSR cannot be mapped\n");
		return false;
	}
	printf("pcie: power: A: GPLL0 %s; RSC version %u.%u\n",
	    (rd(w->gcc + GCC_GPLL0_MODE) & (1u << 31)) ? "locked" : "NOT locked",
	    rd(rsc) >> 16 & 0xff, rd(rsc) >> 8 & 0xff);
	if (!(rd(w->gcc + GCC_GPLL0_MODE) & (1u << 31)) || (rd(rsc) >> 16 & 0xff) != 3 || !supplies_found(found, &xo)) {
		return false;
	}
	{
		char source[16] = "";

		(void)PE_parse_boot_argn("pcie_sleep_clock", source, sizeof(source));
		pmic_gpio(&sleep_clock_pin, strcmp(source, "board") == 0);
	}
	{
		uint32_t pdc = 0;

		if (PE_parse_boot_argn("pcie_aop_pdc", &pdc, sizeof(pdc)) && pdc != 0) {
			aop_pdc();
		}
	}

	mhi_pcie_pause("B: PERST and the card's enable driven low");
	wr(PIN_IO(PIN_PERST), 0);
	wr(PIN_CTL(PIN_PERST), CTL_OUT_LOW_2MA);
	wr(PIN_IO(PIN_WLAN_EN), 0);
	wr(PIN_CTL(PIN_WLAN_EN), CTL_OUT_LOW_8MA);

	mhi_pcie_pause("C: RPMh votes for the PHY's and the card's supplies");
	if (!supplies_on(found, xo)) {
		return false;
	}
	mhi_pcie_pause("D: power domains and clocks");
	if (!clocks_on()) {
		return false;
	}
	mhi_pcie_pause("E, F: resets, then the controller's glue");
	resets_pulse();
	parf_setup();
	mhi_pcie_pause("G: the PHY");
	if (!phy_start()) {
		return false;
	}

	mhi_pcie_pause("H: the card's enable");
	{
		uint32_t bt_en = 0;

		(void)PE_parse_boot_argn("pcie_bt_en", &bt_en, sizeof(bt_en));
		pmic_gpio(&bt_en_pin, bt_en != 0);
	}
	wr(PIN_IO(PIN_WLAN_EN), PIN_IO_OUT_HIGH);
	delay_for_interval(50, NSEC_PER_MSEC);
	wr(PIN_CTL(PIN_CLKREQ), CTL_CLKREQ_FUNCTION);
	wr(PIN_CTL(PIN_WAKE), CTL_IN_PULL_UP);
	printf("pcie: power: the card's enable is high; sw-ctrl pins read %u %u\n",
	    rd(PIN_IO(18)) & 1, rd(PIN_IO(19)) & 1);
	root_port_setup();
	delay_for_interval(50, NSEC_PER_MSEC);              /* 100 ms from the enable to PERST (PCIE_T_PVPERL_MS) */

	wr(PIN_IO(PIN_PERST), PIN_IO_OUT_HIGH);
	sleep_us(1000);
	wr(w->parf + PARF_LTSSM, rd(w->parf + PARF_LTSSM) | LTSSM_EN);
	for (unsigned i = 0; i < 10 && !up; i++) {
		uint32_t state = rd(w->parf + PARF_LTSSM) & 0x3f;

		if (state != ltssm) {
			printf("pcie: power: J: link training, state machine %#x after %u ms\n", state, i * 90);
			ltssm = state;
		}
		up = (rd(w->dm_core + DBI_LINKCTRLSTATUS) & LINK_ACTIVE) != 0;
		if (!up) {
			delay_for_interval(90, NSEC_PER_MSEC);
		}
	}
	printf("pcie: power: the link is %s (state machine %#x, link status %#010x)\n", up ? "up" : "still down",
	    rd(w->parf + PARF_LTSSM) & 0x3f, rd(w->dm_core + DBI_LINKCTRLSTATUS));
	if (up) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	return true;
}

/*
 * Shared SM8750 (annibale) CMD_DB (0x81c60000) and Apps RSC DRV2 (0x16520000) active TCS voter.
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/annibale_rpmh.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

#define CMD_DB_PHYS         0x81c60000UL
#define CMD_DB_SIZE         0x00020000UL
#define RSC_PHYS            0x16520000UL
#define RSC_SIZE            0x00010000UL
#define RSC_TCS_BASE        0x0d00u
#define RSC_IRQ_ENABLE      (RSC_TCS_BASE + 0x00u)
#define RSC_IRQ_STATUS      (RSC_TCS_BASE + 0x04u)
#define RSC_IRQ_CLEAR       (RSC_TCS_BASE + 0x08u)
#define RSC_TCS(n)          (RSC_TCS_BASE + 0x2a0u * (n))
#define TCS_ACTIVE_MODE     (1u << 16)
#define TCS_TRIGGER         (1u << 24)
#define MSGID_WRITE_WAITED  (8u | (1u << 8) | (1u << 16))

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};

struct cmd_db_header {
	uint32_t version;
	uint8_t  magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t  data[];
};

struct cmd_db_entry {
	char     id[8];
	uint32_t priority[2], address;
	uint16_t length, offset;
};

static LCK_GRP_DECLARE(annibale_rpmh_lck_grp, "annibale-rpmh");
static LCK_MTX_DECLARE(annibale_rpmh_lock, &annibale_rpmh_lck_grp);

static const struct cmd_db_header *cmd_db_va;
static vm_offset_t rsc_va;

static inline uint32_t
rpmh_rd(vm_offset_t addr)
{
	uint32_t v = *(volatile uint32_t *)addr;
	__builtin_arm_dsb(0xf);
	return v;
}

static inline void
rpmh_wr(vm_offset_t addr, uint32_t val)
{
	__builtin_arm_dsb(0xf);
	*(volatile uint32_t *)addr = val;
	__builtin_arm_dsb(0xf);
}

uint32_t
annibale_cmd_db_lookup(const char *name, const uint8_t **data_out, uint32_t *len_out)
{
	char id[8] = { 0 };
	uint32_t found_addr = 0;
	size_t name_len;

	if (name == NULL) {
		return 0;
	}
	name_len = strnlen(name, sizeof(id) + 1u);
	if (name_len > sizeof(id)) {
		return 0;
	}
	/* CMD_DB IDs occupy eight bytes; a full-width ID has no terminator. */
	memcpy(id, name, name_len);
	lck_mtx_lock(&annibale_rpmh_lock);
	if (cmd_db_va == NULL) {
		cmd_db_va = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
		if (cmd_db_va == NULL) {
			lck_mtx_unlock(&annibale_rpmh_lock);
			return 0;
		}
	}
	if (cmd_db_va->magic[0] != 0xdb || cmd_db_va->magic[1] != 0x30 ||
	    cmd_db_va->magic[2] != 0x03 || cmd_db_va->magic[3] != 0x0c) {
		lck_mtx_unlock(&annibale_rpmh_lock);
		return 0;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slv = &cmd_db_va->slave[s];
		const struct cmd_db_entry *ent;
		if (slv->id == 0 || slv->header_offset >= CMD_DB_SIZE - 0x1000u || slv->count > 512u) {
			continue;
		}
		ent = (const struct cmd_db_entry *)(const void *)(cmd_db_va->data + slv->header_offset);
		for (unsigned i = 0; i < slv->count; i++) {
			if (memcmp(ent[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((uint32_t)slv->data_offset + ent[i].offset + ent[i].length > CMD_DB_SIZE - 0x1000u) {
				lck_mtx_unlock(&annibale_rpmh_lock);
				return 0;
			}
			if (data_out != NULL) {
				*data_out = cmd_db_va->data + slv->data_offset + ent[i].offset;
			}
			if (len_out != NULL) {
				*len_out = ent[i].length;
			}
			found_addr = ent[i].address;
			lck_mtx_unlock(&annibale_rpmh_lock);
			return found_addr;
		}
	}
	lck_mtx_unlock(&annibale_rpmh_lock);
	return 0;
}

bool
annibale_rpmh_tcs_send(const uint32_t *addrs, const uint32_t *vals, unsigned count)
{
	uint32_t major, off_wait, off_ctrl, off_stat, off_en, off_cmd, stride;
	uint32_t irq_before, mask, ctrl, done = 0;
	unsigned tcs = 0;
	bool idle = false;

	if (addrs == NULL || vals == NULL || count == 0 || count > 8u) {
		return false;
	}
	lck_mtx_lock(&annibale_rpmh_lock);
	if (rsc_va == 0) {
		rsc_va = ml_io_map(RSC_PHYS, RSC_SIZE);
		if (rsc_va == 0) {
			lck_mtx_unlock(&annibale_rpmh_lock);
			return false;
		}
	}
	major = (rpmh_rd(rsc_va) >> 16) & 0xffu;
	off_wait = major >= 3u ? 0x20u : 0x10u;
	off_ctrl = major >= 3u ? 0x24u : 0x14u;
	off_stat = major >= 3u ? 0x28u : 0x18u;
	off_en = major >= 3u ? 0x2cu : 0x1cu;
	off_cmd = major >= 3u ? 0x34u : 0x30u;
	stride = major >= 3u ? 24u : 20u;

	for (unsigned attempt = 0; attempt < 200u && !idle; attempt++) {
		for (unsigned n = 0; n < 3u; n++) {
			if (rpmh_rd(rsc_va + RSC_TCS(n) + off_stat) != 0) {
				tcs = n;
				idle = true;
				break;
			}
		}
		if (!idle) {
			delay_for_interval(50, NSEC_PER_USEC);
		}
	}
	if (!idle) {
		lck_mtx_unlock(&annibale_rpmh_lock);
		return false;
	}

	mask = (1u << count) - 1u;
	irq_before = rpmh_rd(rsc_va + RSC_IRQ_ENABLE);
	ctrl = rpmh_rd(rsc_va + RSC_TCS(tcs) + off_ctrl);
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_ctrl, ctrl & ~TCS_TRIGGER);
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_ctrl, ctrl & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	rpmh_wr(rsc_va + RSC_IRQ_CLEAR, 1u << tcs);
	rpmh_wr(rsc_va + RSC_IRQ_ENABLE, irq_before | (1u << tcs));

	for (unsigned i = 0; i < count; i++) {
		uint32_t cbase = RSC_TCS(tcs) + off_cmd + i * stride;
		rpmh_wr(rsc_va + cbase + 0u, MSGID_WRITE_WAITED);
		rpmh_wr(rsc_va + cbase + 4u, addrs[i]);
		rpmh_wr(rsc_va + cbase + 8u, vals[i]);
	}
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_wait, mask);
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_en, mask);
	ctrl = rpmh_rd(rsc_va + RSC_TCS(tcs) + off_ctrl) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE);
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_ctrl, ctrl | TCS_ACTIVE_MODE);
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_ctrl, ctrl | TCS_ACTIVE_MODE | TCS_TRIGGER);

	for (unsigned poll = 0; poll < 2000u; poll++) {
		done = rpmh_rd(rsc_va + RSC_IRQ_STATUS) & (1u << tcs);
		if (done != 0 && rpmh_rd(rsc_va + RSC_TCS(tcs) + off_stat) != 0) {
			break;
		}
		done = 0;
		delay_for_interval(25, NSEC_PER_USEC);
	}

	/* Restore TCS control, enable, wait, and IRQ mask so TCS0..2 stay clean for all callers. */
	ctrl = rpmh_rd(rsc_va + RSC_TCS(tcs) + off_ctrl);
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_ctrl, ctrl & ~TCS_TRIGGER);
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_ctrl, ctrl & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_en, 0);
	rpmh_wr(rsc_va + RSC_TCS(tcs) + off_wait, 0);
	rpmh_wr(rsc_va + RSC_IRQ_CLEAR, 1u << tcs);
	rpmh_wr(rsc_va + RSC_IRQ_ENABLE, irq_before);
	lck_mtx_unlock(&annibale_rpmh_lock);
	return done != 0;
}

bool
annibale_rpmh_regulator(const char *name, uint32_t mv, bool enable, uint32_t mode)
{
	uint32_t base_addr = annibale_cmd_db_lookup(name, NULL, NULL);
	uint32_t addrs[3], vals[3];
	unsigned n = 0;

	if (base_addr == 0) {
		return false;
	}
	if (mv != 0) {
		addrs[n] = base_addr + 0x0u;
		vals[n++] = mv;
	}
	if (mode != 0) {
		addrs[n] = base_addr + 0x8u;
		vals[n++] = mode;
	}
	addrs[n] = base_addr + 0x4u;
	vals[n++] = enable ? 1u : 0u;
	return annibale_rpmh_tcs_send(addrs, vals, n);
}

bool
annibale_rpmh_arc_vote(const char *name, uint32_t min_level)
{
	const uint8_t *data = NULL;
	uint32_t len = 0, addr, selector = 0;
	bool matched = false;

	addr = annibale_cmd_db_lookup(name, &data, &len);
	if (addr == 0 || data == NULL || len < 2u) {
		return false;
	}
	if (min_level == 0) {
		selector = 0;
		matched = true;
	} else {
		for (unsigned i = 0; i < len / 2u && i < 16u; i++) {
			uint32_t lv = (uint32_t)(data[2u * i] | ((uint32_t)data[2u * i + 1u] << 8));
			if (lv >= min_level) {
				selector = i;
				matched = true;
				break;
			}
		}
	}
	if (!matched) {
		return false;
	}
	return annibale_rpmh_tcs_send(&addr, &selector, 1u);
}

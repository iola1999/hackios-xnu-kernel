/*
 * SM8750 (annibale) LPASS ADSP loader, RPMh rail/regulator voter, and GLINK/GPR transport.
 *
 * Hardware map (SM8750 / REDMI K90):
 *   - SMEM:        0x81d00000 (size 0x00200000), remote PID 2 (SMEM_ADSP)
 *                  SMP2P items: 443 (theirs, "slave-kernel"), 429 (ours, "master-kernel")
 *                  Crash reason: 423
 *                  GLINK items: 478 (descriptor), 479 (tx queue), 480 (rx queue)
 *   - CMD DB:      0x81c60000 (size 0x00020000)
 *   - Apps RSC:    0x16520000 (DRV 2, v3 TCS layout)
 *   - AOSS QMP:    0x0c300000 (size 0x00000400)
 *   - IPCC:        0x00406000 (client 3 = LPASS, signal 0 = GLINK, signal 2 = SMP2P)
 *   - ADSP DTB:    0x9ed80000 (size 0x00080000), PAS ID 0x24 (adsp_dtb.mdt + .b00..b02)
 *   - ADSP Image:  0x9ee00000 (size 0x04580000), PAS ID 1 (adsp.mdt + .b00..b56)
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/annibale_audio.h>
#include <sys/annibale_rpmh.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern const uint8_t *annibale_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern bool annibale_ufs_fat16_fw_read(const char *name, uint64_t *size_out, volatile void *dst_buf, uint64_t dst_cap);

static LCK_GRP_DECLARE(annibale_ipc_lock_group, "annibale-ipc");
LCK_MTX_DECLARE(annibale_aop_mailbox_lock, &annibale_ipc_lock_group);
LCK_MTX_DECLARE(annibale_smem_alloc_lock, &annibale_ipc_lock_group);

#define say(...) printf("aadsp: " __VA_ARGS__)

/* ---- Hardware addresses & constants ---------------------------------------------------------- */

#define SMEM_PHYS                 0x81d00000UL
#define SMEM_SIZE                 0x00200000u
#define SMEM_US                   0u
#define SMEM_ADSP                 2u
#define SMEM_GLOBAL               0xfffeu
#define SMEM_CANARY               0xa5a5u
#define ITEM_SMP2P_OURS           429u
#define ITEM_SMP2P_THEIRS         443u
#define ITEM_CRASH_REASON         423u
#define ITEM_GLINK_DESC           478u
#define ITEM_GLINK_OUR_Q          479u
#define ITEM_GLINK_THEIR_Q        480u
#define SMP2P_MAGIC               0x504d5324u
#define SMP2P_VERSION             1u
#define SMP2P_SSR_ACK             (1u << 0)
#define SSR_ACK                   (1u << 0)
#define DSP_READY                 (1u << 1)
#define AOSS_PHYS                 0x0c300000UL
#define AOSS_SIZE                 0x00000400UL
#define QMP_MAGIC_VALUE           0x4d41494cu
#define QMP_THEIR_CHANNEL         0x14u
#define QMP_THEIR_CHANNEL_ACK     0x18u
#define QMP_OUR_LINK              0x24u
#define QMP_OUR_LINK_ACK          0x28u
#define QMP_OUR_CHANNEL           0x2cu
#define QMP_OUR_CHANNEL_ACK       0x30u
#define QMP_OUR_BOX_SIZE          0x34u
#define QMP_OUR_BOX               0x38u
#define QMP_UP                    0x0000ffffu
#define QMP_MESSAGE               64u
#define IPCC_PHYS                 0x00406000UL
#define IPCC_SIZE                 0x00001000UL
#define IPCC_SEND                 0x0cu
#define IPCC_CLIENT_AOP           0u
#define IPCC_CLIENT_LPASS         3u
#define IPCC_SIGNAL_GLINK         0u
#define IPCC_SIGNAL_SMP2P         2u
#define ADSP_DTB_MEMORY           0x9ed80000u
#define ADSP_DTB_MEMORY_SIZE      0x00080000u
#define PAS_ADSP_DTB              0x24u
#define ADSP_MEMORY               0x9ee00000u
#define ADSP_MEMORY_SIZE          0x04580000u
#define PAS_ADSP                  1u
#define SCM_PAS_INIT_IMAGE        0x42000201UL
#define SCM_PAS_MEM_SETUP         0x42000202UL
#define SCM_PAS_AUTH_AND_RESET    0x42000205UL
#define MDT_HASH(flags)           ((((flags) >> 24) & 7u) == 2u)
#define MDT_RELOCATABLE(flags)    (((flags) & (1u << 27)) != 0)
#define MAX_SEGMENTS              64u
#define GLINK_QUEUE_SIZE          0x4000u
#define GLINK_VERSION             0u
#define GLINK_VERSION_ACK         1u
#define GLINK_OPEN                2u
#define GLINK_CLOSE               3u
#define GLINK_OPEN_ACK            4u
#define GLINK_INTENT              5u
#define GLINK_RX_DONE             6u
#define GLINK_RX_INTENT_REQ       7u
#define GLINK_RX_INTENT_REQ_ACK   8u
#define GLINK_TX_DATA             9u
#define GLINK_CLOSE_ACK           11u
#define GLINK_TX_DATA_CONT        12u
#define GLINK_DONE_REUSE          14u
#define APM_CMD_GET_SPF_STATE     0x01001021u
#define APM_CMD_RSP_GET_SPF_STATE 0x02001007u
#define GPR_BASIC_RSP_RESULT      0x02001005u
#define OP_MEM_MAP                0x0100100cu
#define OP_MEM_MAP_DONE           0x02001001u
#define OP_WRITE                  0x0400100au
#define OP_WRITE_DONE             0x05001004u
#define OP_READ_BUF               0x0400100bu
#define OP_READ_BUF_DONE          0x05001005u

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};

struct smem_table {
	uint8_t  magic[4];
	uint32_t version, count, reserved[5];
};

struct smem_partition {
	uint8_t  magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};

struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};

struct smp2p_item {
	uint32_t magic, version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct { char name[16]; uint32_t value; } entry[16];
};

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

struct fw_image {
	const uint8_t *header;
	uint64_t       header_size;
	bool           from_root;
	struct {
		uint32_t       at, file_size, memory_size, seg_idx;
		const uint8_t *file;
		bool           from_root;
	} segment[MAX_SEGMENTS];
	unsigned       segments;
	uint32_t       size;
};

typedef bool (*annibale_gpr_event_fn)(uint32_t src_port, uint32_t dst_port, uint32_t opcode,
    uint32_t token, const void *payload, uint32_t len);

void annibale_adsp_init(void);
bool annibale_adsp_ready(void);
bool annibale_adsp_is_hw_up(void);
bool annibale_adsp_gpr_send(const void *pkt, uint32_t len, uint32_t wait_ms,
    uint32_t *rsp_opcode, uint32_t *rsp_words, uint32_t max_words);
void annibale_adsp_set_gpr_event(annibale_gpr_event_fn handler);
extern bool annibale_audio_reach_is_active(void);
extern void annibale_pmic_glink_set_tx(int (*tx_fn)(const void *data, uint32_t len));
extern void annibale_pmic_glink_rx(const void *data, uint32_t len);

/* ---- State ----------------------------------------------------------------------------------- */

static LCK_GRP_DECLARE(aadsp_lck_grp, "annibale-adsp");
static LCK_MTX_DECLARE(aadsp_lock, &aadsp_lck_grp);
static LCK_MTX_DECLARE(aadsp_gpr_lock, &aadsp_lck_grp);
static LCK_MTX_DECLARE(aadsp_tx_lock, &aadsp_lck_grp);
static LCK_MTX_DECLARE(glink_tx_lock, &aadsp_lck_grp);
static LCK_MTX_DECLARE(glink_rx_lock, &aadsp_lck_grp);
static LCK_MTX_DECLARE(glink_intent_lock, &aadsp_lck_grp);
/* RX may acquire the intent, GPR and GLINK TX locks. Release those locks before
 * polling RX. Receive callbacks run under glink_rx_lock and must not send or poll. */

static volatile uint8_t *smem_base;
static vm_offset_t aoss_base, ipcc_base;
static uint32_t aoss_our_box;

static bool adsp_inited, adsp_hw_up;
static uint16_t glink_gpr_our_cid, glink_gpr_their_cid;
static uint16_t glink_pmic_our_cid, glink_pmic_their_cid;
struct glink_remote_intent {
	struct glink_remote_intent *next;
	uint32_t id, size;
	bool free;
};
struct glink_intent_pool {
	struct glink_remote_intent *head;
};
static struct glink_intent_pool glink_gpr_intents, glink_pmic_intents;
static uint32_t glink_our_read_at, glink_our_write_at, glink_their_read_at, glink_their_write_at;
static uint32_t glink_our_q, glink_their_q, glink_our_q_size, glink_their_q_size, glink_tx_write_pos;
static uint32_t smp2p_our_val;

static annibale_gpr_event_fn gpr_event_handler;
static uint32_t waited_token, waited_opcode, waited_words[32], waited_word_count;
static uint32_t waited_response_src, waited_response_dst;
static bool waited_done;

/* ---- Low-level MMIO & timing helpers --------------------------------------------------------- */

static inline uint32_t mmio_rd(vm_offset_t addr)
{
	uint32_t v = *(volatile uint32_t *)addr;
	__builtin_arm_dsb(0xf);
	return v;
}

static inline void mmio_wr(vm_offset_t addr, uint32_t val)
{
	__builtin_arm_dsb(0xf);
	*(volatile uint32_t *)addr = val;
	__builtin_arm_dsb(0xf);
}

static uint64_t now_us(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000ULL;
}

static inline void sleep_us(uint32_t us) { delay_for_interval(us, NSEC_PER_USEC); }
static inline void sleep_ms(uint32_t ms) { delay_for_interval(ms, NSEC_PER_MSEC); }

static void ipcc_ring(uint32_t client, uint32_t signal)
{
	if (ipcc_base == 0) {
		ipcc_base = ml_io_map(IPCC_PHYS, IPCC_SIZE);
	}
	if (ipcc_base != 0) {
		mmio_wr(ipcc_base + IPCC_SEND, (client << 16) | signal);
	}
}

/* ---- SMEM access & lookup -------------------------------------------------------------------- */

static bool smem_ensure(void)
{
	if (smem_base == NULL) {
		smem_base = (volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	return smem_base != NULL;
}

static bool smem_peek(uint32_t offset, void *to, uint32_t size)
{
	if (!smem_ensure() || offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem_base[offset + i];
	}
	return true;
}

static void smem_poke(uint32_t offset, uint32_t val)
{
	if (smem_ensure() && offset + 4 <= SMEM_SIZE) {
		mmio_wr((vm_offset_t)(smem_base + offset), val);
	}
}

static uint32_t smem_partition_find(uint16_t host0, uint16_t host1, uint32_t *part_size, uint32_t *cacheline)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition part;
	uint32_t toc_off = SMEM_SIZE - 0x1000u;

	if (!smem_peek(toc_off, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0) {
		toc_off = 0;
		if (!smem_peek(toc_off, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0) {
			return 0;
		}
	}
	for (uint32_t i = 0; i < table.count && i < 64; i++) {
		if (!smem_peek(toc_off + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry))) {
			return 0;
		}
		if (entry.offset == 0 || entry.size == 0 || entry.offset + entry.size > SMEM_SIZE) {
			continue;
		}
		if (!((entry.host0 == host0 && entry.host1 == host1) ||
		      (entry.host0 == host1 && entry.host1 == host0))) {
			continue;
		}
		if (!smem_peek(entry.offset, &part, sizeof(part)) || memcmp(part.magic, "$PRT", 4) != 0) {
			return 0;
		}
		if (part_size != NULL) *part_size = part.size;
		if (cacheline != NULL) *cacheline = entry.cacheline;
		return entry.offset;
	}
	return 0;
}

static uint32_t smem_item_in_partition(uint16_t host0, uint16_t host1, uint32_t wanted, uint32_t *size_out)
{
	uint32_t part_off, part_size = 0, cacheline = 0;
	struct smem_partition part;
	struct smem_entry e;

	part_off = smem_partition_find(host0, host1, &part_size, &cacheline);
	if (part_off == 0 || !smem_peek(part_off, &part, sizeof(part)) ||
	    part.free_uncached > part_size || part.free_cached > part_size ||
	    part.free_uncached > part.free_cached) {
		return 0;
	}
	for (uint32_t at = sizeof(part); at + sizeof(e) <= part.free_uncached; ) {
		if (!smem_peek(part_off + at, &e, sizeof(e)) || e.canary != SMEM_CANARY) {
			break;
		}
		uint32_t data = part_off + at + sizeof(e) + e.padding_header;
		if (e.size > part_size || data + e.size > part_off + part.free_uncached) {
			break;
		}
		if (e.item == wanted) {
			if (size_out != NULL) *size_out = e.size - e.padding_data;
			return data;
		}
		at += sizeof(e) + e.padding_header + e.size;
	}
	if (cacheline != 0) {
		for (uint32_t at = part_size - cacheline; at >= part.free_cached && at < part_size; ) {
			if (!smem_peek(part_off + at, &e, sizeof(e)) || e.canary != SMEM_CANARY ||
			    e.size > at - part.free_cached) {
				break;
			}
			uint32_t data = part_off + at - e.padding_header - e.size;
			if (e.item == wanted) {
				if (size_out != NULL) *size_out = e.size - e.padding_data;
				return data;
			}
			if (at < e.padding_header + e.size + cacheline) {
				break;
			}
			at = at - e.padding_header - e.size - cacheline;
		}
	}
	return 0;
}

static uint32_t smem_item(uint32_t wanted, uint32_t *size_out)
{
	uint32_t at = smem_item_in_partition(SMEM_US, SMEM_ADSP, wanted, size_out);
	return at != 0 ? at : smem_item_in_partition(SMEM_GLOBAL, SMEM_GLOBAL, wanted, size_out);
}

static uint32_t smem_alloc_item(uint32_t item, uint32_t size)
{
	uint32_t existing_sz = 0, at = smem_item(item, &existing_sz);
	if (at != 0) {
		return existing_sz >= size ? at : 0;
	}
	uint32_t part_off = smem_partition_find(SMEM_US, SMEM_ADSP, NULL, NULL);
	struct smem_partition part;
	uint32_t padded = (size + 7u) & ~7u;

	if (part_off == 0 || !smem_peek(part_off, &part, sizeof(part)) ||
	    part.free_uncached + sizeof(struct smem_entry) + padded > part.free_cached) {
		return 0;
	}
	uint32_t hdr = part_off + part.free_uncached;
	uint32_t data = hdr + (uint32_t)sizeof(struct smem_entry);
	for (uint32_t i = 0; i < padded; i += 4) {
		smem_poke(data + i, 0);
	}
	smem_poke(hdr + 0, SMEM_CANARY | (item << 16));
	smem_poke(hdr + 4, padded);
	smem_poke(hdr + 8, (uint16_t)(padded - size));
	smem_poke(hdr + 12, 0);
	__builtin_arm_dmb(0xf);
	smem_poke(part_off + 12, part.free_uncached + (uint32_t)sizeof(struct smem_entry) + padded);
	return data;
}

/* ---- RPMh ARC Rail Voter (shared annibale_rpmh helper) --------------------------------------- */

static bool vote_adsp_rails(uint32_t level)
{
	return annibale_rpmh_arc_vote("lcx.lvl", level) &&
	    annibale_rpmh_arc_vote("lmx.lvl", level);
}

/* ---- AOSS QMP Mailbox ------------------------------------------------------------------------ */

static bool aoss_send_msg(const char *msg)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool ok = false;

	lck_mtx_lock(&annibale_aop_mailbox_lock);
	if (aoss_base == 0) {
		aoss_base = ml_io_map(AOSS_PHYS, AOSS_SIZE);
		if (aoss_base == 0) {
			lck_mtx_unlock(&annibale_aop_mailbox_lock);
			return false;
		}
	}
	if (aoss_our_box == 0) {
		if (mmio_rd(aoss_base) != QMP_MAGIC_VALUE) {
			lck_mtx_unlock(&annibale_aop_mailbox_lock);
			return false;
		}
		mmio_wr(aoss_base + QMP_OUR_LINK, QMP_UP);
		ipcc_ring(IPCC_CLIENT_AOP, 0);
		for (unsigned i = 0; i < 200 && mmio_rd(aoss_base + QMP_OUR_LINK_ACK) != QMP_UP; i++) sleep_ms(1);
		mmio_wr(aoss_base + QMP_OUR_CHANNEL, QMP_UP);
		if (mmio_rd(aoss_base + QMP_THEIR_CHANNEL) == QMP_UP) {
			mmio_wr(aoss_base + QMP_THEIR_CHANNEL_ACK, QMP_UP);
		}
		ipcc_ring(IPCC_CLIENT_AOP, 0);
		for (unsigned i = 0; i < 200 && mmio_rd(aoss_base + QMP_OUR_CHANNEL_ACK) != QMP_UP; i++) sleep_ms(1);
		uint32_t box = mmio_rd(aoss_base + QMP_OUR_BOX), bsz = mmio_rd(aoss_base + QMP_OUR_BOX_SIZE);
		if (box >= AOSS_SIZE || bsz < QMP_MESSAGE) {
			lck_mtx_unlock(&annibale_aop_mailbox_lock);
			return false;
		}
		aoss_our_box = box;
	}
	if (mmio_rd(aoss_base + aoss_our_box) != 0) {
		lck_mtx_unlock(&annibale_aop_mailbox_lock);
		return false;
	}
	strlcpy((char *)words, msg, sizeof(words));
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		mmio_wr(aoss_base + aoss_our_box + 4 + 4 * i, words[i]);
	}
	mmio_wr(aoss_base + aoss_our_box, QMP_MESSAGE);
	ipcc_ring(IPCC_CLIENT_AOP, 0);
	for (unsigned i = 0; i < 500; i++) {
		if (mmio_rd(aoss_base + aoss_our_box) == 0) {
			ok = true;
			break;
		}
		sleep_ms(2);
	}
	if (!ok) mmio_wr(aoss_base + aoss_our_box, 0);
	lck_mtx_unlock(&annibale_aop_mailbox_lock);
	return ok;
}

#include "annibale_adsp_fw.inc"

/* ---- GLINK & GPR Transport ------------------------------------------------------------------- */

static uint32_t glink_q_read32(uint32_t q_off, uint32_t q_sz, uint32_t at)
{
	uint32_t v = 0;
	smem_peek(q_off + (at % q_sz), &v, 4);
	return v;
}

static bool glink_q_write(uint32_t q_off, uint32_t q_sz, uint32_t *write_at, const void *buf, uint32_t len)
{
	const uint8_t *bytes = (const uint8_t *)buf;
	uint32_t aligned = (len + 7u) & ~7u;
	if (q_sz < 8 || aligned >= q_sz - 8) return false;
	lck_mtx_lock(&glink_tx_lock);
	uint32_t pos = *write_at;
	if (pos >= q_sz) {
		lck_mtx_unlock(&glink_tx_lock);
		return false;
	}
	if (q_off == glink_our_q && glink_our_read_at != 0 && q_sz > aligned + 8u) {
		for (unsigned wait = 0; wait < 200; wait++) {
			uint32_t r = 0;
			smem_peek(glink_our_read_at, &r, 4);
			uint32_t used = (pos >= r) ? (pos - r) : (q_sz - r + pos);
			if (used + aligned + 8u < q_sz) {
				break;
			}
			if (wait == 199) {
				lck_mtx_unlock(&glink_tx_lock);
				return false;
			}
			sleep_us(100);
		}
	}
	for (uint32_t i = 0; i < aligned; i += 4) {
		uint32_t w = 0;
		for (uint32_t b = 0; b < 4 && (i + b) < len; b++) {
			w |= (uint32_t)bytes[i + b] << (8 * b);
		}
		smem_poke(q_off + (pos % q_sz), w);
		pos = (pos + 4) % q_sz;
	}
	__builtin_arm_dmb(0xf);
	*write_at = pos;
	smem_poke(glink_our_write_at, pos);
	ipcc_ring(IPCC_CLIENT_LPASS, IPCC_SIGNAL_GLINK);
	lck_mtx_unlock(&glink_tx_lock);
	return true;
}

static void glink_send_cmd2(uint16_t cmd, uint16_t p1, uint32_t p2)
{
	uint32_t words[2] = { cmd | ((uint32_t)p1 << 16), p2 };
	glink_q_write(glink_our_q, glink_our_q_size, &glink_tx_write_pos, words, sizeof(words));
}

static void glink_send_open(uint16_t cid, const char *name)
{
	uint8_t pkt[32] = { 0 };
	uint16_t nlen = (uint16_t)(strlen(name) + 1), cmd = GLINK_OPEN;
	memcpy(pkt + 0, &cmd, 2);
	memcpy(pkt + 2, &cid, 2);
	memcpy(pkt + 4, &nlen, 2);
	memcpy(pkt + 8, name, nlen);
	glink_q_write(glink_our_q, glink_our_q_size, &glink_tx_write_pos, pkt, 8u + nlen);
}

static void glink_queue_intents(uint16_t cid)
{
	for (uint32_t id = 1; id <= 8; id++) {
		uint32_t pkt[4] = { GLINK_INTENT | ((uint32_t)cid << 16), 1u, 2048u, id };
		glink_q_write(glink_our_q, glink_our_q_size, &glink_tx_write_pos, pkt, sizeof(pkt));
	}
}

static void handle_incoming_gpr(const uint8_t *data, uint32_t len)
{
	if (len < 24) {
		return;
	}
	uint32_t src_port, dst_port, token, opcode;
	memcpy(&src_port, data + 8, 4);
	memcpy(&dst_port, data + 12, 4);
	memcpy(&token, data + 16, 4);
	memcpy(&opcode, data + 20, 4);
	const uint8_t *payload = data + 24;
	uint32_t plen = len - 24;

	lck_mtx_lock(&aadsp_gpr_lock);
	if (waited_token != 0 && token == waited_token && src_port == waited_response_src &&
	    dst_port == waited_response_dst) {
		waited_opcode = opcode;
		waited_word_count = MIN(plen / 4u, 32u);
		if (waited_word_count > 0) {
			memcpy(waited_words, payload, waited_word_count * 4u);
		}
		waited_done = true;
		lck_mtx_unlock(&aadsp_gpr_lock);
		return;
	}
	annibale_gpr_event_fn fn = gpr_event_handler;
	if (waited_token != 0 && fn == NULL) {
		say("unmatched GPR src %#x dst %#x token %u opcode %#x bytes %u; waiting src %#x dst %#x token %u\n",
		    src_port, dst_port, token, opcode, len, waited_response_src, waited_response_dst, waited_token);
	}
	lck_mtx_unlock(&aadsp_gpr_lock);
	if (fn != NULL) {
		(void)fn(src_port, dst_port, opcode, token, payload, plen);
	}
}

static void glink_poll_rx(void);
#include "annibale_adsp_intents.inc"

static int adsp_pmic_glink_tx(const void *data, uint32_t len)
{
	if (!adsp_hw_up || glink_our_q == 0 || glink_pmic_our_cid == 0 || data == NULL || len == 0 || len > 512) {
		return -1;
	}
	glink_poll_rx();
	uint32_t iid = 0;
	if (!glink_take_their_intent(&glink_pmic_intents, glink_pmic_our_cid, len, &iid)) {
		return -1;
	}
	uint8_t tx[560];
	uint32_t w0 = GLINK_TX_DATA | ((uint32_t)glink_pmic_our_cid << 16);
	uint32_t left = 0;
	memcpy(tx + 0, &w0, 4);
	memcpy(tx + 4, &iid, 4);
	memcpy(tx + 8, &len, 4);
	memcpy(tx + 12, &left, 4);
	memcpy(tx + 16, data, len);
	if (!glink_q_write(glink_our_q, glink_our_q_size, &glink_tx_write_pos, tx, 16u + len)) {
		glink_complete_their_intent(&glink_pmic_intents, iid, true);
		return -1;
	}
	for (unsigned i = 0; i < 10; i++) {
		sleep_us(200);
		glink_poll_rx();
	}
	return 0;
}

static void glink_poll_rx(void)
{
	/* Boot, GPR and PMIC callers share one receive cursor. */
	lck_mtx_lock(&glink_rx_lock);
	if (glink_their_q == 0) {
		glink_their_q = smem_item(ITEM_GLINK_THEIR_Q, &glink_their_q_size);
		if (glink_their_q == 0 || glink_their_q_size == 0) {
			lck_mtx_unlock(&glink_rx_lock);
			return;
		}
	}
	uint32_t r = 0, w = 0;
	smem_peek(glink_their_read_at, &r, 4);
	smem_peek(glink_their_write_at, &w, 4);

	while (r != w && r < glink_their_q_size && w < glink_their_q_size) {
		uint32_t w0 = glink_q_read32(glink_their_q, glink_their_q_size, r);
		uint32_t w1 = glink_q_read32(glink_their_q, glink_their_q_size, r + 4);
		uint16_t cmd = (uint16_t)(w0 & 0xffffu), param = (uint16_t)(w0 >> 16);
		uint32_t step_bytes = 8;

		if (cmd == GLINK_VERSION) {
			glink_send_cmd2(GLINK_VERSION_ACK, param, w1 & 1u);
		} else if (cmd == GLINK_OPEN) {
			char name[32] = { 0 };
			uint16_t nlen = (uint16_t)(w1 & 0xffffu);
			for (uint16_t i = 0; i < nlen && i < sizeof(name) - 1; i++) {
				smem_peek(glink_their_q + ((r + 8 + i) % glink_their_q_size), &name[i], 1);
			}
			step_bytes = (8u + nlen + 7u) & ~7u;
			glink_send_cmd2(GLINK_OPEN_ACK, param, 0);
			if (strcmp(name, "adsp_apps") == 0 || strcmp(name, "apr_audio_svc") == 0) {
				glink_gpr_their_cid = param;
				if (glink_gpr_our_cid == 0) {
					glink_gpr_our_cid = 2;
					glink_send_open(glink_gpr_our_cid, name);
				} else {
					glink_queue_intents(glink_gpr_our_cid);
				}
			} else if (strcmp(name, "PMIC_RTR_ADSP_APPS") == 0) {
				glink_pmic_their_cid = param;
				if (glink_pmic_our_cid == 0) {
					glink_pmic_our_cid = 3;
					glink_send_open(glink_pmic_our_cid, name);
				} else {
					glink_queue_intents(glink_pmic_our_cid);
					annibale_pmic_glink_set_tx(adsp_pmic_glink_tx);
				}
			}
		} else if (cmd == GLINK_OPEN_ACK) {
			if (param == glink_gpr_our_cid && glink_gpr_their_cid != 0) {
				glink_queue_intents(glink_gpr_our_cid);
			} else if (param == glink_pmic_our_cid && glink_pmic_their_cid != 0) {
				glink_queue_intents(glink_pmic_our_cid);
				annibale_pmic_glink_set_tx(adsp_pmic_glink_tx);
			}
		} else if (cmd == GLINK_CLOSE) {
			glink_send_cmd2(GLINK_CLOSE_ACK, param, 0);
		} else if (cmd == GLINK_RX_INTENT_REQ) {
			glink_send_cmd2(GLINK_RX_INTENT_REQ_ACK, param, 1);
			if (param == glink_gpr_their_cid && glink_gpr_our_cid != 0) {
				glink_queue_intents(glink_gpr_our_cid);
			} else if (param == glink_pmic_their_cid && glink_pmic_our_cid != 0) {
				glink_queue_intents(glink_pmic_our_cid);
			}
		} else if (cmd == GLINK_INTENT) {
			uint32_t count = w1;
			if (count > 64) count = 0;
			for (uint32_t i = 0; i < count; i++) {
				uint32_t sz = glink_q_read32(glink_their_q, glink_their_q_size, r + 8 + 8 * i);
				uint32_t iid = glink_q_read32(glink_their_q, glink_their_q_size, r + 12 + 8 * i);
				if (param == glink_pmic_their_cid) {
					glink_add_their_intent(&glink_pmic_intents, iid, sz);
				} else if (param == glink_gpr_their_cid) {
					glink_add_their_intent(&glink_gpr_intents, iid, sz);
					say("GPR intent id %u size %u\n", iid, sz);
				}
			}
			step_bytes = (8u + 8u * w1 + 7u) & ~7u;
		} else if (cmd == GLINK_RX_DONE || cmd == GLINK_DONE_REUSE) {
			if (param == glink_pmic_their_cid) {
				glink_complete_their_intent(&glink_pmic_intents, w1, cmd == GLINK_DONE_REUSE);
			} else if (param == glink_gpr_their_cid) {
				glink_complete_their_intent(&glink_gpr_intents, w1, cmd == GLINK_DONE_REUSE);
			}
		} else if (cmd == GLINK_TX_DATA || cmd == GLINK_TX_DATA_CONT) {
			uint32_t chunk = glink_q_read32(glink_their_q, glink_their_q_size, r + 8);
			uint32_t left = glink_q_read32(glink_their_q, glink_their_q_size, r + 12);
			step_bytes = (16u + chunk + 7u) & ~7u;
			if ((param == glink_gpr_their_cid || param == glink_pmic_their_cid) && chunk <= 512) {
				uint8_t buf[512];
				for (uint32_t i = 0; i < chunk; i++) {
					smem_peek(glink_their_q + ((r + 16 + i) % glink_their_q_size), &buf[i], 1);
				}
				if (param == glink_gpr_their_cid) {
					handle_incoming_gpr(buf, chunk);
				} else {
					annibale_pmic_glink_rx(buf, chunk);
				}
			}
			if (left == 0) {
				uint16_t our_cid = (param == glink_pmic_their_cid) ? glink_pmic_our_cid : glink_gpr_our_cid;
				if (our_cid != 0) {
					glink_send_cmd2(GLINK_DONE_REUSE, our_cid, w1);
				}
			}
		}
		r = (r + step_bytes) % glink_their_q_size;
		smem_poke(glink_their_read_at, r);
		smem_peek(glink_their_write_at, &w, 4);
	}
	lck_mtx_unlock(&glink_rx_lock);
}

static bool glink_init_queues(bool cold_boot)
{
	lck_mtx_lock(&annibale_smem_alloc_lock);
	uint32_t desc = smem_alloc_item(ITEM_GLINK_DESC, 32);
	uint32_t our_q = smem_alloc_item(ITEM_GLINK_OUR_Q, GLINK_QUEUE_SIZE);
	lck_mtx_unlock(&annibale_smem_alloc_lock);
	if (desc == 0 || our_q == 0) {
		return false;
	}
	glink_our_read_at = desc + 0;
	glink_our_write_at = desc + 4;
	glink_their_read_at = desc + 8;
	glink_their_write_at = desc + 12;
	glink_our_q = our_q;
	glink_our_q_size = GLINK_QUEUE_SIZE;
	if (cold_boot) {
		smem_poke(glink_our_write_at, 0);
		smem_poke(glink_their_read_at, 0);
		glink_tx_write_pos = 0;
	} else {
		glink_tx_write_pos = 0;
		smem_peek(glink_our_write_at, &glink_tx_write_pos, 4);
	}
	glink_their_q = smem_item(ITEM_GLINK_THEIR_Q, &glink_their_q_size);
	glink_send_cmd2(GLINK_VERSION, 1, 1);
	return true;
}

bool annibale_adsp_gpr_send(const void *pkt, uint32_t len, uint32_t wait_ms,
    uint32_t *rsp_opcode, uint32_t *rsp_words, uint32_t max_words)
{
	if (pkt == NULL || len < 24 || len > 544 || !adsp_hw_up ||
	    glink_our_q == 0 || glink_gpr_our_cid == 0 || glink_gpr_their_cid == 0) {
		return false;
	}
	lck_mtx_lock(&aadsp_tx_lock);
	const uint8_t *bytes = (const uint8_t *)pkt;
	uint32_t token = 0, request_opcode = 0, request_src = 0, request_dst = 0;
	memcpy(&request_src, bytes + 8, 4);
	memcpy(&request_dst, bytes + 12, 4);
	memcpy(&token, bytes + 16, 4);
	memcpy(&request_opcode, bytes + 20, 4);
	bool sent = false;

	lck_mtx_lock(&aadsp_gpr_lock);
	if (wait_ms > 0) {
		waited_token = token;
		waited_done = false;
		waited_opcode = 0;
		waited_word_count = 0;
		waited_response_src = request_dst;
		waited_response_dst = request_src;
	}
	lck_mtx_unlock(&aadsp_gpr_lock);

	ipcc_ring(IPCC_CLIENT_LPASS, IPCC_SIGNAL_GLINK);

	if (adsp_hw_up && glink_our_q != 0 && glink_gpr_our_cid != 0) {
		uint8_t tx[560];
		uint32_t iid = 0;
		if (16u + len <= sizeof(tx) &&
		    glink_take_their_intent(&glink_gpr_intents, glink_gpr_our_cid, len, &iid)) {
			uint32_t w0 = GLINK_TX_DATA | ((uint32_t)glink_gpr_our_cid << 16);
			uint32_t left = 0;
			memcpy(tx + 0, &w0, 4);
			memcpy(tx + 4, &iid, 4);
			memcpy(tx + 8, &len, 4);
			memcpy(tx + 12, &left, 4);
			memcpy(tx + 16, bytes, len);
			sent = glink_q_write(glink_our_q, glink_our_q_size, &glink_tx_write_pos, tx, 16u + len);
			if (!sent) {
				glink_complete_their_intent(&glink_gpr_intents, iid, true);
				say("GPR queue full request %#x token %u intent %u bytes %u\n", request_opcode, token, iid, len);
			}
		} else {
			say("GPR intent unavailable request %#x token %u bytes %u\n", request_opcode, token, len);
		}
	}
	if (!sent) {
		lck_mtx_lock(&aadsp_gpr_lock);
		waited_token = 0;
		lck_mtx_unlock(&aadsp_gpr_lock);
		lck_mtx_unlock(&aadsp_tx_lock);
		return false;
	}
	if (wait_ms == 0) {
		lck_mtx_unlock(&aadsp_tx_lock);
		return true;
	}

	uint64_t t0 = now_us();
	bool ok = false;
	while (now_us() - t0 < (uint64_t)wait_ms * 1000ULL) {
		if (adsp_hw_up) {
			glink_poll_rx();
		}
		lck_mtx_lock(&aadsp_gpr_lock);
		if (waited_done) {
			if (rsp_opcode != NULL) {
				*rsp_opcode = waited_opcode;
			}
			if (rsp_words != NULL && max_words > 0) {
				memcpy(rsp_words, waited_words, MIN(waited_word_count, max_words) * 4u);
			}
			waited_token = 0;
			ok = waited_opcode == GPR_BASIC_RSP_RESULT && waited_word_count >= 2 &&
			    waited_words[0] == request_opcode && waited_words[1] == 0;
			if (request_opcode == APM_CMD_GET_SPF_STATE)
				ok = waited_opcode == APM_CMD_RSP_GET_SPF_STATE && waited_word_count >= 1;
			else if (request_opcode == OP_MEM_MAP)
				ok = waited_opcode == OP_MEM_MAP_DONE && waited_word_count >= 1;
			else if (request_opcode == OP_WRITE)
				ok = waited_opcode == OP_WRITE_DONE && waited_word_count >= 8;
			else if (request_opcode == OP_READ_BUF)
				ok = waited_opcode == OP_READ_BUF_DONE && waited_word_count >= 14;
			lck_mtx_unlock(&aadsp_gpr_lock);
			break;
		}
		lck_mtx_unlock(&aadsp_gpr_lock);
		sleep_us(200);
	}
	lck_mtx_lock(&aadsp_gpr_lock);
	if (!ok) {
		say("GPR wait failed request %#x token %u matched %u rsp %#x words %u first %#x second %#x elapsed %llu us\n",
		    request_opcode, token, waited_done, waited_opcode, waited_word_count,
		    waited_word_count > 0 ? waited_words[0] : 0, waited_word_count > 1 ? waited_words[1] : 0,
		    now_us() - t0);
	} else if (request_opcode == APM_CMD_GET_SPF_STATE) {
		say("GPR state reply token %u elapsed %llu us\n", token, now_us() - t0);
	}
	waited_token = 0;
	lck_mtx_unlock(&aadsp_gpr_lock);
	lck_mtx_unlock(&aadsp_tx_lock);
	return ok;
}

void annibale_adsp_set_gpr_event(annibale_gpr_event_fn handler)
{
	lck_mtx_lock(&aadsp_gpr_lock);
	gpr_event_handler = handler;
	lck_mtx_unlock(&aadsp_gpr_lock);
}

bool annibale_adsp_ready(void) { return adsp_inited; }
bool annibale_adsp_is_hw_up(void) { return adsp_hw_up; }
bool annibale_adsp_gpr_ready(void)
{
	bool ready = false;
	lck_mtx_lock(&glink_intent_lock);
	ready = glink_gpr_intents.head != NULL;
	lck_mtx_unlock(&glink_intent_lock);
	return adsp_hw_up && glink_our_q != 0 && glink_gpr_our_cid != 0 && glink_gpr_their_cid != 0 && ready;
}

/* ---- Background ADSP boot & GLINK poll thread ------------------------------------------------ */

static void smp2p_init_ours(void)
{
	lck_mtx_lock(&annibale_smem_alloc_lock);
	uint32_t at = smem_alloc_item(ITEM_SMP2P_OURS, sizeof(struct smp2p_item));
	lck_mtx_unlock(&annibale_smem_alloc_lock);
	if (at == 0) return;
	struct smp2p_item ours;
	if (!smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		smem_poke(at + 0, SMP2P_MAGIC);
		smem_poke(at + 4, SMP2P_VERSION);
		smem_poke(at + 8, (uint32_t)SMEM_US | ((uint32_t)SMEM_ADSP << 16));
		smem_poke(at + 12, 16u | (1u << 16));
		smem_poke(at + 16, SMP2P_SSR_ACK);
		char name[16] = "master-kernel";
		for (unsigned w = 0; w < 4; w++) {
			uint32_t nw = 0;
			memcpy(&nw, name + 4 * w, 4);
			smem_poke(at + 20 + 4 * w, nw);
		}
		smem_poke(at + 36, 0);
		smp2p_our_val = 0;
		__builtin_arm_dmb(0xf);
		ipcc_ring(IPCC_CLIENT_LPASS, IPCC_SIGNAL_SMP2P);
	}
}

static bool check_smp2p_ready(void)
{
	uint32_t sz = 0, at = smem_item(ITEM_SMP2P_THEIRS, &sz);
	struct smp2p_item item;
	if (at == 0 || sz < sizeof(item) || !smem_peek(at, &item, sizeof(item)) || item.magic != SMP2P_MAGIC) {
		return false;
	}
	for (unsigned i = 0; i < item.valid && i < 16; i++) {
		if (strncmp(item.entry[i].name, "slave-kernel", 16) == 0) {
			uint32_t val = item.entry[i].value;
			if (((val ^ smp2p_our_val) & SSR_ACK) != 0) {
				smp2p_our_val ^= SSR_ACK;
				uint32_t our_sz = 0, our_at = smem_item(ITEM_SMP2P_OURS, &our_sz);
				if (our_at != 0) {
					smem_poke(our_at + 36, smp2p_our_val);
					__builtin_arm_dmb(0xf);
					ipcc_ring(IPCC_CLIENT_LPASS, IPCC_SIGNAL_SMP2P);
				}
			}
			return (val & DSP_READY) != 0;
		}
	}
	return false;
}

static void annibale_adsp_boot_thread(void *arg __unused, wait_result_t w __unused)
{
	smp2p_init_ours();
	if (check_smp2p_ready()) {
		say("SM8750 LPASS ADSP already running in SMEM (SMP2P slave-kernel READY)\n");
		(void)glink_init_queues(false);
		adsp_hw_up = true;
	} else {
		uint32_t try_boot = 1;
		PE_parse_boot_argn("annibale_adsp", &try_boot, sizeof(try_boot));
		if (try_boot != 0) {
			/* Let Adreno 830 GPU finish TrustZone SCM zap authentication first so PAS SMC calls do not contend. */
			extern bool adreno8_init_done;
			for (unsigned wait = 0; wait < 120 && !adreno8_init_done; wait++) {
				sleep_ms(100);
			}
			uint64_t probe_sz = 0;
			for (unsigned wait = 0; wait < 50 &&
			    !annibale_ufs_fat16_fw_read("adsp_dtb.mdt", &probe_sz, NULL, 0); wait++) {
				sleep_ms(100);
			}
			struct fw_image dtb_fw, main_fw;
			if (fw_load_mdt("adsp_dtb", ADSP_DTB_MEMORY, ADSP_DTB_MEMORY_SIZE, &dtb_fw) &&
			    fw_load_mdt("adsp", ADSP_MEMORY, ADSP_MEMORY_SIZE, &main_fw)) {
				say("loaded adsp_dtb.mdt (%u segs, %#x B) and adsp.mdt (%u segs, %#x B)\n",
				    dtb_fw.segments, dtb_fw.size, main_fw.segments, main_fw.size);
				(void)vote_adsp_rails(384);
				(void)aoss_send_msg("{class: image, res: load_state, name: adsp, val: on}");
				if (pas_stage_and_setup("adsp_dtb", PAS_ADSP_DTB, ADSP_DTB_MEMORY, ADSP_DTB_MEMORY_SIZE, &dtb_fw) &&
				    pas_stage_and_setup("adsp", PAS_ADSP, ADSP_MEMORY, ADSP_MEMORY_SIZE, &main_fw)) {
					uint64_t r1 = 0;
					(void)glink_init_queues(true);
					(void)scm_pas_call(SCM_PAS_AUTH_AND_RESET, 1u, PAS_ADSP_DTB, 0, 0, &r1);
					if (scm_pas_call(SCM_PAS_AUTH_AND_RESET, 1u, PAS_ADSP, 0, 0, &r1) == 0 && r1 == 0) {
						for (unsigned ms = 0; ms < 3000; ms += 20) {
							if (check_smp2p_ready()) {
								adsp_hw_up = true;
								say("SM8750 LPASS ADSP authenticated and READY in %u ms\n", ms);
								break;
							}
							sleep_ms(20);
						}
					}
				}
				fw_release(&dtb_fw);
				fw_release(&main_fw);
			} else {
				fw_release(&dtb_fw);
				fw_release(&main_fw);
			}
		}
	}

	if (!adsp_hw_up) {
		say("ADSP is not ready; audio transport unavailable\n");
		thread_terminate(current_thread());
		return;
	}
	for (;;) {
		glink_poll_rx();
		sleep_ms(annibale_audio_reach_is_active() ? 1 : (glink_gpr_our_cid == 0 ? 5 : 20));
	}
}

void annibale_adsp_init(void)
{
	lck_mtx_lock(&aadsp_lock);
	if (adsp_inited) {
		lck_mtx_unlock(&aadsp_lock);
		return;
	}
	(void)smem_ensure();
	adsp_inited = true;
	lck_mtx_unlock(&aadsp_lock);

	thread_t th;
	if (kernel_thread_start(annibale_adsp_boot_thread, NULL, &th) == KERN_SUCCESS) {
		thread_set_thread_name(th, "annibale-adsp-boot");
		thread_deallocate(th);
	}
}

/*
 * SM8750 (annibale) MPSS cellular baseband remoteproc, SMEM GLINK IPCRTR transport,
 * QMI service state machine (UIM 0x0b, DMS 0x02, NAS 0x03, WDS 0x01, Voice 0x09, WMS 0x05),
 * /dev/annibale-modem character device, and kern.annibale.* cellular sysctls.
 *
 * Privacy contract: subscriber and device identifiers (IMEI, IMSI, ICCID, MSISDN, MEID, ESN)
 * are never logged or exposed in sysctls; any identifier field is masked as "***".
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/kauth.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/annibale_input.h>
#include <sys/annibale_qmi.h>
#include <sys/annibale_qrtr.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern kern_return_t thread_terminate(thread_t thread);
extern lck_mtx_t annibale_smem_alloc_lock;

/* Multi-node QRTR registration (bsd/dev/annibale_qrtr.c) and QMI codecs (bsd/dev/annibale_qmi.c) */
extern void annibale_qrtr_attach_node(uint32_t peer_node, int (*send)(const void *data, uint32_t length));
extern size_t annibale_qmi_encode_msg(uint8_t *buf, size_t cap, uint8_t flags, uint16_t txn_id,
    uint16_t msg_id, const void *tlvs, uint16_t tlv_len);
extern const uint8_t *annibale_qmi_find_tlv(const uint8_t *tlvs, size_t tlvs_len, uint8_t type, uint16_t *out_len);
extern bool annibale_qmi_uim_parse_card_status(const uint8_t *tlvs, size_t tlvs_len,
    uint8_t *card_state, uint8_t *app_state);
extern bool annibale_qmi_dms_parse_oper_mode(const uint8_t *tlvs, size_t tlvs_len, uint8_t *mode_out);
extern bool annibale_qmi_nas_parse_serving(const uint8_t *tlvs, size_t tlvs_len, uint8_t *reg_state,
    uint8_t *rat_out, uint16_t *mcc_out, uint16_t *mnc_out, char *oper_out, size_t oper_cap);
extern bool annibale_qmi_nas_parse_signal(const uint8_t *tlvs, size_t tlvs_len, int16_t *rsrp_out,
    uint8_t *bars_out, uint8_t *rat_out);
extern bool annibale_qmi_wds_parse_runtime(const uint8_t *tlvs, size_t tlvs_len, uint32_t *ipv4_out,
    uint32_t *gw_out, uint32_t *mask_out, uint32_t *dns1_out, uint32_t *dns2_out, uint32_t *mtu_out);

/* IPA interface & PAS MDT loader callbacks (bsd/dev/annibale_ipa.c) */
extern void annibale_ipa_configure_v4(uint32_t ipv4_addr, uint32_t ipv4_gw, uint32_t ipv4_mask,
    uint32_t dns1, uint32_t dns2, uint32_t mtu);
extern bool annibale_fw_exists(const char *name);
extern bool annibale_load_mdt_image(const char *prefix, uint32_t pas_id, uint32_t alt_pas_id,
    uint64_t mem_phys, uint64_t mem_size, uint32_t *used_pas_out);
extern int64_t annibale_tz_pas_auth_reset(uint32_t pas_id);

/* ---- SM8750 MPSS hardware addresses & constants (from annibale.dtb & SM8750 map) ------------------ */

#define SM8750_SMEM_PHYS                0x81d00000ULL
#define SM8750_SMEM_ALT_PHYS            0x86000000ULL
#define SM8750_SMEM_SIZE                0x00200000ULL
#define SM8750_TCSR_SPINLOCK_PHYS       0x01f43000ULL
#define SM8750_IPCC_MPROC_PHYS          0x00406000ULL
#define SM8750_IPCC_MPROC_SIZE          0x00001000ULL

#define MPSS_DTB_MEM_PHYS               0x9b000000ULL
#define MPSS_DTB_MEM_SIZE               0x00080000ULL
#define MPSS_DTB_PAS_ID                 37u
#define MPSS_DTB_PAS_ID_ALT             38u
#define MPSS_FW_MEM_PHYS                0x8ba00000ULL
#define MPSS_FW_MEM_SIZE                0x0f600000ULL
#define MPSS_FW_PAS_ID                  4u

#define SMEM_HOST_APPS                  0u
#define SMEM_HOST_MPSS                  1u
#define SMEM_CANARY                     0xa5a5u
#define ITEM_SMP2P_MPSS_OURS            428u
#define ITEM_SMP2P_MPSS_THEIRS          435u
#define ITEM_GLINK_DESCRIPTOR           478u
#define ITEM_GLINK_OUR_QUEUE            479u
#define ITEM_GLINK_THEIR_QUEUE          480u
#define GLINK_QUEUE_SIZE                16384u

#define IPCC_REG_SEND                   0x0c
#define IPCC_CLIENT_MPSS                2u
#define IPCC_SIG_GLINK                  0u
#define IPCC_SIG_SMP2P                  2u

#define GLINK_CMD_VERSION               0u
#define GLINK_CMD_VERSION_ACK           1u
#define GLINK_CMD_OPEN                  2u
#define GLINK_CMD_CLOSE                 3u
#define GLINK_CMD_OPEN_ACK              4u
#define GLINK_CMD_INTENT                5u
#define GLINK_CMD_RX_INTENT_REQ         7u
#define GLINK_CMD_RX_INTENT_REQ_ACK     8u
#define GLINK_CMD_TX_DATA               9u
#define GLINK_CMD_CLOSE_ACK             11u
#define GLINK_CMD_TX_DATA_CONT          12u
#define GLINK_CMD_RX_DONE_REUSE         14u
#define GLINK_FEATURE_INTENT_REUSE      (1u << 0)
#define GLINK_OUR_CID                   1u

#define QRTR_NODE_MPSS                  0u

#define QMI_SVC_WDS                     0x01u
#define QMI_SVC_DMS                     0x02u
#define QMI_SVC_NAS                     0x03u
#define QMI_SVC_WMS                     0x05u
#define QMI_SVC_VOICE                   0x09u
#define QMI_SVC_UIM                     0x0bu

#define QMI_WDS_START_NET_IF            0x0020u
#define QMI_WDS_STOP_NET_IF             0x0021u
#define QMI_WDS_GET_RUNTIME_SETTINGS    0x002du
#define QMI_DMS_GET_OPERATING_MODE      0x002du
#define QMI_DMS_SET_OPERATING_MODE      0x002eu
#define QMI_NAS_GET_SIGNAL_STRENGTH     0x0020u
#define QMI_NAS_GET_SERVING_SYSTEM      0x0024u
#define QMI_NAS_GET_HOME_NETWORK        0x0025u
#define QMI_NAS_GET_SIG_INFO            0x004fu
#define QMI_NAS_SIG_INFO_IND            0x0051u
#define QMI_WMS_EVENT_REPORT            0x0001u
#define QMI_WMS_RAW_SEND                0x0020u
#define QMI_VOICE_DIAL_CALL             0x0020u
#define QMI_VOICE_END_CALL              0x0021u
#define QMI_VOICE_ANSWER_CALL           0x0022u
#define QMI_VOICE_ALL_CALL_STATUS_IND   0x002eu
#define QMI_UIM_GET_CARD_STATUS         0x002fu

/* ---- Public ioctl / character device structures for /dev/annibale-modem ----------------------------- */

enum annibale_modem_state_enum {
	MODEM_STATE_OFF = 0, MODEM_STATE_STANDBY = 1, MODEM_STATE_BOOTING = 2,
	MODEM_STATE_GLINK_UP = 3, MODEM_STATE_QMI_READY = 4, MODEM_STATE_REGISTERED = 5,
	MODEM_STATE_DATA_CONNECTED = 6,
};

struct annibale_modem_status {
	uint32_t magic, state, sim_state, reg_state, signal_bars;
	int32_t  rsrp_dbm;
	uint16_t mcc, mnc;
	uint32_t ipv4_addr, ipv4_gw, ipv4_dns1, ipv4_dns2, mtu;
	uint32_t call_state, sms_sent, sms_received;
	char     rat[16];
	char     operator_name[32];
};

struct annibale_modem_voice_cmd {
	uint32_t action;
	uint8_t  call_id;
	char     number[32];
};

struct annibale_modem_sms_cmd {
	uint32_t format;
	uint16_t pdu_len;
	uint8_t  pdu[180];
};

#define ANNIBALE_MODEM_MAGIC             0x414e4e4du
#define ANNIBALE_MODEM_IOC_GET_STATUS    _IOR('M', 1, struct annibale_modem_status)
#define ANNIBALE_MODEM_IOC_POLL          _IO('M', 2)
#define ANNIBALE_MODEM_IOC_DATA_CALL     _IOW('M', 3, uint32_t)
#define ANNIBALE_MODEM_IOC_VOICE_CMD     _IOW('M', 4, struct annibale_modem_voice_cmd)
#define ANNIBALE_MODEM_IOC_SMS_SEND      _IOW('M', 5, struct annibale_modem_sms_cmd)

/* ---- Sysctls under kern.annibale.* ------------------------------------------------------------------ */

SYSCTL_DECL(_kern_annibale);

static int sysctl_modem_state = MODEM_STATE_STANDBY;
static int sysctl_cellular_bars = 0;
static int sysctl_cellular_sim_state = 0;
static int sysctl_cellular_rsrp = -140;
static char sysctl_cellular_rat[16] = "None";
static char sysctl_cellular_operator[32] = "";

SYSCTL_INT(_kern_annibale, OID_AUTO, modem_state, CTLFLAG_RW | CTLFLAG_LOCKED,
    &sysctl_modem_state, 0, "SM8750 MPSS modem state (0=off..6=data_connected)");
SYSCTL_INT(_kern_annibale, OID_AUTO, cellular_bars, CTLFLAG_RW | CTLFLAG_LOCKED,
    &sysctl_cellular_bars, 0, "Cellular signal strength in bars (0..4)");
SYSCTL_INT(_kern_annibale, OID_AUTO, cellular_sim_state, CTLFLAG_RW | CTLFLAG_LOCKED,
    &sysctl_cellular_sim_state, 0, "SIM card state (0=absent, 1=present, 2=ready)");
SYSCTL_INT(_kern_annibale, OID_AUTO, cellular_rsrp, CTLFLAG_RW | CTLFLAG_LOCKED,
    &sysctl_cellular_rsrp, 0, "Cellular RSRP in dBm");
SYSCTL_STRING(_kern_annibale, OID_AUTO, cellular_rat, CTLFLAG_RW | CTLFLAG_LOCKED,
    sysctl_cellular_rat, sizeof(sysctl_cellular_rat), "Cellular radio access technology");
SYSCTL_STRING(_kern_annibale, OID_AUTO, cellular_operator, CTLFLAG_RW | CTLFLAG_LOCKED,
    sysctl_cellular_operator, sizeof(sysctl_cellular_operator), "Cellular operator name");

/* ---- Driver state ---------------------------------------------------------------------------------- */

static LCK_GRP_DECLARE(modem_lck_grp, "annibale-modem");
static LCK_MTX_DECLARE(modem_lock, &modem_lck_grp);

static bool modem_inited;
static uint32_t modem_boot_arg;
static volatile uint32_t *smem_base;
static uint64_t smem_phys_active = SM8750_SMEM_PHYS;
static vm_offset_t ipcc_base;
static vm_offset_t tcsr_lock_base;

static struct {
	uint32_t desc_off, our_q_off, our_q_sz, their_q_off, their_q_sz;
	uint32_t their_cid;
	bool     ready, agreed, ipcrtr_asked, ipcrtr_ack, ipcrtr_open;
	uint32_t their_intents[16];
	uint32_t next_our_iid;
	uint32_t tx_count, rx_count;
} mpss_glink;

struct qmi_svc_slot {
	uint32_t service_id;
	uint32_t node, port;
	bool     online;
	bool     queried;
	uint16_t next_txn;
	struct annibale_qrtr_port *qrtr_port;
};

enum { SLOT_UIM = 0, SLOT_DMS = 1, SLOT_NAS = 2, SLOT_WDS = 3, SLOT_VOICE = 4, SLOT_WMS = 5, SLOT_COUNT = 6 };

static struct qmi_svc_slot qmi_slots[SLOT_COUNT] = {
	[SLOT_UIM]   = { .service_id = QMI_SVC_UIM },
	[SLOT_DMS]   = { .service_id = QMI_SVC_DMS },
	[SLOT_NAS]   = { .service_id = QMI_SVC_NAS },
	[SLOT_WDS]   = { .service_id = QMI_SVC_WDS },
	[SLOT_VOICE] = { .service_id = QMI_SVC_VOICE },
	[SLOT_WMS]   = { .service_id = QMI_SVC_WMS },
};

static struct annibale_modem_status modem_status = {
	.magic = ANNIBALE_MODEM_MAGIC,
	.state = MODEM_STATE_STANDBY,
	.rsrp_dbm = -140,
	.mtu = 1500,
	.rat = "None",
};

static uint32_t wds_pkt_handle;
static bool wds_data_wanted = true;

/* ---- Low-level MMIO & SMEM helpers ----------------------------------------------------------------- */

static void
modem_sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static inline void
mmio_wr(vm_offset_t addr, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)addr = v;
	__asm__ volatile("dmb osh" ::: "memory");
}

static inline uint32_t
smem_rd(uint32_t byte_off)
{
	uint32_t v = smem_base[byte_off / 4];
	__asm__ volatile("dmb ishld" ::: "memory");
	return v;
}

static inline void
smem_wr(uint32_t byte_off, uint32_t v)
{
	__asm__ volatile("dmb ishst" ::: "memory");
	smem_base[byte_off / 4] = v;
	__asm__ volatile("dmb ish" ::: "memory");
}

static void
modem_sync_sysctls(void)
{
	sysctl_modem_state = (int)modem_status.state;
	sysctl_cellular_bars = (int)modem_status.signal_bars;
	sysctl_cellular_sim_state = (int)modem_status.sim_state;
	sysctl_cellular_rsrp = (int)modem_status.rsrp_dbm;
	strlcpy(sysctl_cellular_rat, modem_status.rat, sizeof(sysctl_cellular_rat));
	strlcpy(sysctl_cellular_operator, modem_status.operator_name, sizeof(sysctl_cellular_operator));
}

static uint32_t
smem_find_partition(uint16_t host_a, uint16_t host_b, uint32_t *size_out)
{
	uint32_t ptable_off;

	if (smem_base == NULL) {
		return 0;
	}
	ptable_off = SM8750_SMEM_SIZE - 0x1000u;
	if (smem_rd(ptable_off) != 0x434f5424u) {
		ptable_off = smem_rd(0x0c) + smem_rd(0x10);
	}
	if (ptable_off == 0 || ptable_off + 4 + 16 * 32 > SM8750_SMEM_SIZE || smem_rd(ptable_off) != 0x434f5424u) {
		return 0;
	}
	for (uint32_t i = 0, n = smem_rd(ptable_off + 8); i < n && i < 32; i++) {
		uint32_t e = ptable_off + 16 + i * 32;
		uint32_t off = smem_rd(e), sz = smem_rd(e + 4), hosts = smem_rd(e + 8);
		uint16_t h0 = (uint16_t)(hosts & 0xffff), h1 = (uint16_t)(hosts >> 16);

		if (((h0 == host_a && h1 == host_b) || (h0 == host_b && h1 == host_a)) &&
		    off != 0 && sz >= 64 && off + sz <= SM8750_SMEM_SIZE && smem_rd(off) == 0x54525024u) {
			if (size_out != NULL) {
				*size_out = sz;
			}
			return off;
		}
	}
	return 0;
}

static uint32_t
smem_find_mpss_item(uint32_t item_id, uint32_t *size_out)
{
	uint32_t part_sz = 0;
	uint32_t part = smem_find_partition(SMEM_HOST_APPS, SMEM_HOST_MPSS, &part_sz);
	uint32_t uncached_end, cached_start;

	if (part == 0) {
		return 0;
	}
	uncached_end = smem_rd(part + 12);
	cached_start = smem_rd(part + 16);
	if (uncached_end > part_sz || cached_start > part_sz) {
		return 0;
	}
	for (uint32_t at = 32; at + 16 <= uncached_end; ) {
		uint32_t hdr = smem_rd(part + at);
		uint32_t pad_data = smem_rd(part + at + 8) & 0xffffu;
		uint32_t pad_hdr = smem_rd(part + at + 8) >> 16;
		uint32_t sz = smem_rd(part + at + 4);

		if ((hdr & 0xffffu) != SMEM_CANARY || sz == 0 || at + 16 + pad_hdr + sz > uncached_end) {
			break;
		}
		if ((hdr >> 16) == item_id) {
			if (size_out != NULL) {
				*size_out = sz >= pad_data ? sz - pad_data : 0;
			}
			return part + at + 16 + pad_hdr;
		}
		at += 16 + pad_hdr + sz;
	}
	for (uint32_t at = part_sz; at >= cached_start + 16; ) {
		uint32_t hdr = smem_rd(part + at - 16);
		uint32_t pad_data = smem_rd(part + at - 8) & 0xffffu;
		uint32_t pad_hdr = smem_rd(part + at - 8) >> 16;
		uint32_t sz = smem_rd(part + at - 12);
		uint32_t step_sz = ((16 + pad_hdr + sz) + 7u) & ~7u;

		if ((hdr & 0xffffu) != SMEM_CANARY || sz == 0 || at < cached_start + step_sz) {
			break;
		}
		if ((hdr >> 16) == item_id) {
			if (size_out != NULL) {
				*size_out = sz >= pad_data ? sz - pad_data : 0;
			}
			return part + at - step_sz;
		}
		at -= step_sz;
	}
	return 0;
}

static uint32_t
smem_alloc_mpss_item(uint32_t item_id, uint32_t req_size)
{
	uint32_t existing_sz = 0;
	uint32_t off = smem_find_mpss_item(item_id, &existing_sz);
	uint32_t part_sz = 0, part, uncached_end, cached_start, aligned_sz, pad_data;

	if (off != 0) {
		return existing_sz >= req_size ? off : 0;
	}
	lck_mtx_lock(&annibale_smem_alloc_lock);
	part = smem_find_partition(SMEM_HOST_APPS, SMEM_HOST_MPSS, &part_sz);
	if (part != 0) {
		uncached_end = smem_rd(part + 12);
		cached_start = smem_rd(part + 16);
		aligned_sz = (req_size + 7u) & ~7u;
		pad_data = aligned_sz - req_size;
		if (uncached_end >= 32 && uncached_end + 16 + aligned_sz <= cached_start) {
			off = part + uncached_end + 16;
			for (uint32_t i = 0; i < aligned_sz; i += 4) {
				smem_wr(off + i, 0);
			}
			smem_wr(part + uncached_end, SMEM_CANARY | (item_id << 16));
			smem_wr(part + uncached_end + 4, aligned_sz);
			smem_wr(part + uncached_end + 8, pad_data);
			smem_wr(part + uncached_end + 12, 0);
			smem_wr(part + 12, uncached_end + 16 + aligned_sz);
		}
	}
	lck_mtx_unlock(&annibale_smem_alloc_lock);
	return off;
}

static void
ipcc_ring_mpss(uint32_t signal)
{
	if (ipcc_base != 0) {
		mmio_wr(ipcc_base + IPCC_REG_SEND, (IPCC_CLIENT_MPSS << 16) | (signal & 0xffffu));
	}
}

/* ---- SMEM GLINK IPCRTR transport with MPSS (partition 0 <-> 1) ------------------------------------- */

static bool
mpss_glink_locate_queues(void)
{
	uint32_t d_sz = 0, o_sz = 0, t_sz = 0;

	mpss_glink.desc_off = smem_find_mpss_item(ITEM_GLINK_DESCRIPTOR, &d_sz);
	mpss_glink.our_q_off = smem_find_mpss_item(ITEM_GLINK_OUR_QUEUE, &o_sz);
	mpss_glink.their_q_off = smem_find_mpss_item(ITEM_GLINK_THEIR_QUEUE, &t_sz);
	if (mpss_glink.desc_off == 0) {
		mpss_glink.desc_off = smem_alloc_mpss_item(ITEM_GLINK_DESCRIPTOR, 32);
		d_sz = 32;
	}
	if (mpss_glink.our_q_off == 0) {
		mpss_glink.our_q_off = smem_alloc_mpss_item(ITEM_GLINK_OUR_QUEUE, GLINK_QUEUE_SIZE);
		o_sz = GLINK_QUEUE_SIZE;
	}
	mpss_glink.our_q_sz = o_sz;
	mpss_glink.their_q_sz = t_sz;
	mpss_glink.ready = (mpss_glink.desc_off != 0 && d_sz >= 16 &&
	    mpss_glink.our_q_off != 0 && o_sz >= 1024 &&
	    mpss_glink.their_q_off != 0 && t_sz >= 1024);
	return mpss_glink.ready;
}

static bool
mpss_glink_write_raw(uint16_t cmd, uint16_t p1, uint32_t p2, const void *payload, uint32_t len)
{
	uint32_t aligned = (8u + len + 7u) & ~7u;
	uint32_t head, tail, avail, pos;
	uint8_t hdr[8];

	if (!mpss_glink.ready || mpss_glink.our_q_sz < aligned + 8) {
		return false;
	}
	tail = smem_rd(mpss_glink.desc_off + 0);
	head = smem_rd(mpss_glink.desc_off + 4);
	if (head >= mpss_glink.our_q_sz || tail >= mpss_glink.our_q_sz) {
		return false;
	}
	avail = (tail <= head ? mpss_glink.our_q_sz - head + tail : tail - head) - 8u;
	if (avail < aligned) {
		return false;
	}
	hdr[0] = (uint8_t)(cmd & 0xff);
	hdr[1] = (uint8_t)(cmd >> 8);
	hdr[2] = (uint8_t)(p1 & 0xff);
	hdr[3] = (uint8_t)(p1 >> 8);
	memcpy(hdr + 4, &p2, 4);

	pos = head;
	for (uint32_t i = 0; i < aligned; i += 4) {
		uint32_t w = 0;
		for (uint32_t b = 0; b < 4; b++) {
			uint32_t idx = i + b;
			uint8_t byte = idx < 8 ? hdr[idx] :
			    (payload != NULL && idx - 8 < len ? ((const uint8_t *)payload)[idx - 8] : 0);
			w |= (uint32_t)byte << (b * 8);
		}
		smem_wr(mpss_glink.our_q_off + pos, w);
		pos = (pos + 4 == mpss_glink.our_q_sz) ? 0 : pos + 4;
	}
	smem_wr(mpss_glink.desc_off + 4, pos);
	ipcc_ring_mpss(IPCC_SIG_GLINK);
	mpss_glink.tx_count++;
	return true;
}

static void
mpss_glink_queue_intents(void)
{
	static const uint32_t intent_sizes[10] = {
		0x800, 0x800, 0x800, 0x800, 0x800,
		0x2000, 0x2000, 0x2000, 0x4400, 0x4400
	};
	uint32_t pairs[20];

	if (mpss_glink.next_our_iid != 0) {
		return;
	}
	for (unsigned i = 0; i < 10; i++) {
		pairs[2 * i] = intent_sizes[i];
		pairs[2 * i + 1] = ++mpss_glink.next_our_iid;
	}
	mpss_glink_write_raw(GLINK_CMD_INTENT, GLINK_OUR_CID, 10, pairs, sizeof(pairs));
}

static int
mpss_qrtr_send(const void *data, uint32_t length)
{
	uint32_t iid = 0;
	uint8_t *frame;
	bool sent;

	lck_mtx_lock(&modem_lock);
	if (!mpss_glink.ipcrtr_open) {
		lck_mtx_unlock(&modem_lock);
		return ENOTCONN;
	}
	for (unsigned i = 0; i < 16; i++) {
		if (mpss_glink.their_intents[i] >= length) {
			iid = i + 1;
			mpss_glink.their_intents[i] = 0;
			break;
		}
	}
	if (iid == 0) {
		mpss_glink_write_raw(GLINK_CMD_RX_INTENT_REQ, GLINK_OUR_CID, length, NULL, 0);
		lck_mtx_unlock(&modem_lock);
		return EAGAIN;
	}
	frame = kalloc_data(8 + length, Z_WAITOK);
	if (frame == NULL) {
		lck_mtx_unlock(&modem_lock);
		return ENOMEM;
	}
	memcpy(frame, &length, 4);
	bzero(frame + 4, 4);
	memcpy(frame + 8, data, length);
	sent = mpss_glink_write_raw(GLINK_CMD_TX_DATA, GLINK_OUR_CID, iid, frame, 8 + length);
	kfree_data(frame, 8 + length);
	lck_mtx_unlock(&modem_lock);
	return sent ? 0 : EIO;
}

static void
mpss_glink_poll_rx(void)
{
	uint32_t tail, head;

	if (!mpss_glink.ready && !mpss_glink_locate_queues()) {
		return;
	}
	tail = smem_rd(mpss_glink.desc_off + 8);
	head = smem_rd(mpss_glink.desc_off + 12);
	while (tail != head && tail < mpss_glink.their_q_sz && head < mpss_glink.their_q_sz) {
		uint32_t w0 = smem_rd(mpss_glink.their_q_off + tail);
		uint32_t w1 = smem_rd(mpss_glink.their_q_off + ((tail + 4) % mpss_glink.their_q_sz));
		uint16_t cmd = (uint16_t)(w0 & 0xffffu);
		uint16_t p1 = (uint16_t)(w0 >> 16);
		uint32_t p2 = w1;

		tail = (tail + 8) % mpss_glink.their_q_sz;
		mpss_glink.rx_count++;

		switch (cmd) {
		case GLINK_CMD_VERSION:
			mpss_glink_write_raw(GLINK_CMD_VERSION_ACK, p1, p2 & GLINK_FEATURE_INTENT_REUSE, NULL, 0);
			if (!mpss_glink.agreed) {
				mpss_glink_write_raw(GLINK_CMD_VERSION, 1, GLINK_FEATURE_INTENT_REUSE, NULL, 0);
			}
			break;
		case GLINK_CMD_VERSION_ACK:
			mpss_glink.agreed = true;
			if (!mpss_glink.ipcrtr_asked) {
				mpss_glink.ipcrtr_asked = true;
				mpss_glink_write_raw(GLINK_CMD_OPEN, GLINK_OUR_CID, 7, "IPCRTR", 7);
			}
			break;
		case GLINK_CMD_OPEN: {
			char cname[32] = { 0 };
			uint32_t aligned_len = (p2 + 7u) & ~7u;

			for (uint32_t i = 0; i < aligned_len && i < sizeof(cname) - 1; i += 4) {
				uint32_t cw = smem_rd(mpss_glink.their_q_off + ((tail + i) % mpss_glink.their_q_sz));
				memcpy(cname + i, &cw, 4);
			}
			tail = (tail + aligned_len) % mpss_glink.their_q_sz;
			mpss_glink_write_raw(GLINK_CMD_OPEN_ACK, p1, 0, NULL, 0);
			if (strcmp(cname, "IPCRTR") == 0) {
				mpss_glink.their_cid = p1;
				if (!mpss_glink.ipcrtr_asked) {
					mpss_glink.ipcrtr_asked = true;
					mpss_glink_write_raw(GLINK_CMD_OPEN, GLINK_OUR_CID, 7, "IPCRTR", 7);
				}
				if (mpss_glink.ipcrtr_ack && !mpss_glink.ipcrtr_open) {
					mpss_glink.ipcrtr_open = true;
					mpss_glink_queue_intents();
				}
			}
			break;
		}
		case GLINK_CMD_OPEN_ACK:
			if (p1 == GLINK_OUR_CID) {
				mpss_glink.ipcrtr_ack = true;
				if (mpss_glink.their_cid != 0 && !mpss_glink.ipcrtr_open) {
					mpss_glink.ipcrtr_open = true;
					mpss_glink_queue_intents();
				}
			}
			break;
		case GLINK_CMD_INTENT:
			for (uint32_t i = 0; i < p2; i++) {
				uint32_t sz = smem_rd(mpss_glink.their_q_off + ((tail + i * 8) % mpss_glink.their_q_sz));
				uint32_t id = smem_rd(mpss_glink.their_q_off + ((tail + i * 8 + 4) % mpss_glink.their_q_sz));
				if (p1 == mpss_glink.their_cid && id >= 1 && id <= 16) {
					mpss_glink.their_intents[id - 1] = sz;
				}
			}
			tail = (tail + p2 * 8) % mpss_glink.their_q_sz;
			break;
		case GLINK_CMD_RX_INTENT_REQ:
			mpss_glink_write_raw(GLINK_CMD_RX_INTENT_REQ_ACK, GLINK_OUR_CID, 1, NULL, 0);
			break;
		case GLINK_CMD_TX_DATA:
		case GLINK_CMD_TX_DATA_CONT: {
			uint32_t frag_sz = smem_rd(mpss_glink.their_q_off + tail);
			uint32_t left_sz = smem_rd(mpss_glink.their_q_off + ((tail + 4) % mpss_glink.their_q_sz));
			uint32_t aligned_frag = (frag_sz + 7u) & ~7u;

			tail = (tail + 8) % mpss_glink.their_q_sz;
			if (p1 == mpss_glink.their_cid && frag_sz > 0 && frag_sz <= 8192 && left_sz == 0) {
				uint8_t *pkt = kalloc_data(frag_sz + 4, Z_NOWAIT);
				if (pkt != NULL) {
					for (uint32_t i = 0; i < frag_sz; i += 4) {
						uint32_t dw = smem_rd(mpss_glink.their_q_off + ((tail + i) % mpss_glink.their_q_sz));
						memcpy(pkt + i, &dw, 4);
					}
					smem_wr(mpss_glink.desc_off + 8, (tail + aligned_frag) % mpss_glink.their_q_sz);
					mpss_glink_write_raw(GLINK_CMD_RX_DONE_REUSE, GLINK_OUR_CID, p2, NULL, 0);
					lck_mtx_unlock(&modem_lock);
					annibale_qrtr_input(pkt, frag_sz);
					kfree_data(pkt, frag_sz + 4);
					lck_mtx_lock(&modem_lock);
					tail = smem_rd(mpss_glink.desc_off + 8);
					head = smem_rd(mpss_glink.desc_off + 12);
					continue;
				}
			}
			tail = (tail + aligned_frag) % mpss_glink.their_q_sz;
			mpss_glink_write_raw(GLINK_CMD_RX_DONE_REUSE, p1 == mpss_glink.their_cid ? GLINK_OUR_CID : p1, p2, NULL, 0);
			break;
		}
		case GLINK_CMD_CLOSE:
			mpss_glink_write_raw(GLINK_CMD_CLOSE_ACK, p1, 0, NULL, 0);
			break;
		default:
			break;
		}
		smem_wr(mpss_glink.desc_off + 8, tail);
		head = smem_rd(mpss_glink.desc_off + 12);
	}
}

/* ---- QMI Client State Machine (UIM, DMS, NAS, WDS, Voice, WMS) ------------------------------------- */

static int
modem_qmi_send(unsigned slot_idx, uint16_t msg_id, const void *tlvs, uint16_t tlv_len)
{
	struct qmi_svc_slot *s = &qmi_slots[slot_idx];
	uint8_t buf[512];
	size_t n;

	if (!s->online || s->qrtr_port == NULL) {
		return ENOTCONN;
	}
	if (++s->next_txn == 0) {
		s->next_txn = 1;
	}
	n = annibale_qmi_encode_msg(buf, sizeof(buf), 0x00, s->next_txn, msg_id, tlvs, tlv_len);
	if (n == 0) {
		return EINVAL;
	}
	return annibale_qrtr.send(s->qrtr_port, s->node, s->port, buf, n);
}

static const char *
modem_rat_to_string(uint8_t rat_code)
{
	if (rat_code == 0x0c) return "5G";
	if (rat_code == 0x05 || rat_code == 0x0b) return "3G";
	if (rat_code == 0x04) return "GSM";
	return "LTE";
}

static void
modem_qmi_poll_services(void)
{
	if (qmi_slots[SLOT_UIM].online && !qmi_slots[SLOT_UIM].queried) {
		qmi_slots[SLOT_UIM].queried = true;
		modem_qmi_send(SLOT_UIM, QMI_UIM_GET_CARD_STATUS, NULL, 0);
	}
	if (qmi_slots[SLOT_DMS].online && !qmi_slots[SLOT_DMS].queried) {
		qmi_slots[SLOT_DMS].queried = true;
		modem_qmi_send(SLOT_DMS, QMI_DMS_GET_OPERATING_MODE, NULL, 0);
	}
	if (qmi_slots[SLOT_NAS].online && !qmi_slots[SLOT_NAS].queried) {
		qmi_slots[SLOT_NAS].queried = true;
		modem_qmi_send(SLOT_NAS, QMI_NAS_GET_SERVING_SYSTEM, NULL, 0);
		modem_qmi_send(SLOT_NAS, QMI_NAS_GET_SIG_INFO, NULL, 0);
	}
	if (qmi_slots[SLOT_WDS].online && !qmi_slots[SLOT_WDS].queried &&
	    modem_status.reg_state == 1 && wds_data_wanted) {
		static const uint8_t start_tlvs[] = { 0x19, 0x01, 0x00, 0x04 };
		static const uint8_t rt_tlvs[] = { 0x10, 0x04, 0x00, 0x10, 0x23, 0x00, 0x00 };

		qmi_slots[SLOT_WDS].queried = true;
		modem_qmi_send(SLOT_WDS, QMI_WDS_START_NET_IF, start_tlvs, sizeof(start_tlvs));
		modem_qmi_send(SLOT_WDS, QMI_WDS_GET_RUNTIME_SETTINGS, rt_tlvs, sizeof(rt_tlvs));
	}
	if (qmi_slots[SLOT_WMS].online && !qmi_slots[SLOT_WMS].queried) {
		static const uint8_t wms_tlvs[] = { 0x10, 0x01, 0x00, 0x01 };
		qmi_slots[SLOT_WMS].queried = true;
		modem_qmi_send(SLOT_WMS, QMI_WMS_EVENT_REPORT, wms_tlvs, sizeof(wms_tlvs));
	}
}

static void
modem_qmi_rx_cb(void *context, uint32_t from_node, uint32_t from_port, const void *data, size_t size)
{
	unsigned slot_idx = (unsigned)(uintptr_t)context;
	const uint8_t *buf = data;
	uint16_t msg_id, tlv_len;
	const uint8_t *tlvs;

	(void)from_node;
	(void)from_port;
	if (slot_idx >= SLOT_COUNT || size < 7) {
		return;
	}
	msg_id = (uint16_t)(buf[3] | (buf[4] << 8));
	tlv_len = (uint16_t)(buf[5] | (buf[6] << 8));
	if ((size_t)7 + tlv_len > size) {
		return;
	}
	tlvs = buf + 7;

	lck_mtx_lock(&modem_lock);
	switch (slot_idx) {
	case SLOT_UIM:
		if (msg_id == QMI_UIM_GET_CARD_STATUS) {
			uint8_t card = 0, app = 0;
			if (annibale_qmi_uim_parse_card_status(tlvs, tlv_len, &card, &app)) {
				modem_status.sim_state = (card == 1) ? (app == 7 ? 2u : 1u) : 0u;
				printf("annibale-modem: QMI UIM SIM state=%u (app=%u, subscriber_id=***)\n",
				    modem_status.sim_state, app);
			}
		}
		break;
	case SLOT_DMS:
		if (msg_id == QMI_DMS_GET_OPERATING_MODE) {
			uint8_t mode = 0;
			if (annibale_qmi_dms_parse_oper_mode(tlvs, tlv_len, &mode)) {
				printf("annibale-modem: QMI DMS operating_mode=%u (device_serial=***)\n", mode);
				if (mode != 0) {
					static const uint8_t online_tlv[] = { 0x01, 0x01, 0x00, 0x00 };
					modem_qmi_send(SLOT_DMS, QMI_DMS_SET_OPERATING_MODE, online_tlv, sizeof(online_tlv));
				}
			}
		}
		break;
	case SLOT_NAS:
		if (msg_id == QMI_NAS_GET_SERVING_SYSTEM || msg_id == QMI_NAS_GET_HOME_NETWORK) {
			uint8_t reg = 0, rat = 0;
			uint16_t mcc = 0, mnc = 0;
			if (annibale_qmi_nas_parse_serving(tlvs, tlv_len, &reg, &rat, &mcc, &mnc,
			    modem_status.operator_name, sizeof(modem_status.operator_name))) {
				modem_status.reg_state = reg;
				if (mcc != 0) {
					modem_status.mcc = mcc;
					modem_status.mnc = mnc;
				}
				if (rat != 0) {
					strlcpy(modem_status.rat, modem_rat_to_string(rat), sizeof(modem_status.rat));
				}
				if (reg == 1 && modem_status.state < MODEM_STATE_REGISTERED) {
					modem_status.state = MODEM_STATE_REGISTERED;
				}
				printf("annibale-modem: QMI NAS reg=%u rat=%s mcc=%u mnc=%u oper=%s\n",
				    reg, modem_status.rat, modem_status.mcc, modem_status.mnc, modem_status.operator_name);
			}
		} else if (msg_id == QMI_NAS_GET_SIG_INFO || msg_id == QMI_NAS_SIG_INFO_IND ||
		    msg_id == QMI_NAS_GET_SIGNAL_STRENGTH) {
			int16_t rsrp = -140;
			uint8_t bars = 0, rat = 0;
			if (annibale_qmi_nas_parse_signal(tlvs, tlv_len, &rsrp, &bars, &rat)) {
				modem_status.rsrp_dbm = rsrp;
				modem_status.signal_bars = bars;
				if (rat != 0) {
					strlcpy(modem_status.rat, modem_rat_to_string(rat), sizeof(modem_status.rat));
				}
			}
		}
		break;
	case SLOT_WDS:
		if (msg_id == QMI_WDS_START_NET_IF) {
			uint16_t len = 0;
			const uint8_t *v = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01, &len);
			if (v != NULL && len >= 4) {
				memcpy(&wds_pkt_handle, v, 4);
			}
		} else if (msg_id == QMI_WDS_GET_RUNTIME_SETTINGS) {
			uint32_t ip = 0, gw = 0, mask = 0, d1 = 0, d2 = 0, mtu = 1500;
			if (annibale_qmi_wds_parse_runtime(tlvs, tlv_len, &ip, &gw, &mask, &d1, &d2, &mtu)) {
				modem_status.ipv4_addr = ip;
				modem_status.ipv4_gw = gw;
				modem_status.ipv4_dns1 = d1;
				modem_status.ipv4_dns2 = d2;
				modem_status.mtu = mtu;
				modem_status.state = MODEM_STATE_DATA_CONNECTED;
				modem_sync_sysctls();
				lck_mtx_unlock(&modem_lock);
				annibale_ipa_configure_v4(ip, gw, mask, d1, d2, mtu);
				return;
			}
		}
		break;
	case SLOT_VOICE:
		if (msg_id == QMI_VOICE_ALL_CALL_STATUS_IND || msg_id == QMI_VOICE_DIAL_CALL ||
		    msg_id == QMI_VOICE_END_CALL || msg_id == QMI_VOICE_ANSWER_CALL) {
			uint16_t len = 0;
			const uint8_t *v = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01, &len);
			if (msg_id == QMI_VOICE_END_CALL) {
				modem_status.call_state = 0;
			} else if (v != NULL && len >= 3) {
				modem_status.call_state = v[2];
			}
			printf("annibale-modem: QMI Voice msg=0x%04x call_state=%u peer=***\n",
			    msg_id, modem_status.call_state);
		}
		break;
	case SLOT_WMS:
		if (msg_id == QMI_WMS_EVENT_REPORT) {
			modem_status.sms_received++;
			printf("annibale-modem: QMI WMS incoming SMS indication (total_rx=%u, sender=***)\n",
			    modem_status.sms_received);
		} else if (msg_id == QMI_WMS_RAW_SEND) {
			modem_status.sms_sent++;
		}
		break;
	}
	modem_sync_sysctls();
	lck_mtx_unlock(&modem_lock);
}

static void
modem_qmi_server_cb(void *context, bool arrived, uint32_t service, uint32_t version,
    uint32_t instance, uint32_t node, uint32_t port)
{
	unsigned slot_idx = (unsigned)(uintptr_t)context;

	(void)version;
	(void)instance;
	if (slot_idx >= SLOT_COUNT) {
		return;
	}
	lck_mtx_lock(&modem_lock);
	if (arrived) {
		qmi_slots[slot_idx].node = node;
		qmi_slots[slot_idx].port = port;
		qmi_slots[slot_idx].online = true;
		qmi_slots[slot_idx].queried = false;
		if (modem_status.state < MODEM_STATE_QMI_READY) {
			modem_status.state = MODEM_STATE_QMI_READY;
		}
		printf("annibale-modem: QMI service 0x%02x online at node %u port %u\n", service, node, port);
	} else if (qmi_slots[slot_idx].node == node && qmi_slots[slot_idx].port == port) {
		qmi_slots[slot_idx].online = false;
		qmi_slots[slot_idx].queried = false;
	}
	modem_sync_sysctls();
	lck_mtx_unlock(&modem_lock);
}

static void
modem_open_qmi_clients(void)
{
	for (unsigned i = 0; i < SLOT_COUNT; i++) {
		if (qmi_slots[i].qrtr_port == NULL) {
			qmi_slots[i].qrtr_port = annibale_qrtr.open(modem_qmi_rx_cb, modem_qmi_server_cb, (void *)(uintptr_t)i);
			if (qmi_slots[i].qrtr_port != NULL) {
				annibale_qrtr.lookup(qmi_slots[i].qrtr_port, qmi_slots[i].service_id, 0, 0);
			}
		}
	}
}

/* ---- Voice & SMS action helpers -------------------------------------------------------------------- */

static int
modem_voice_action(const struct annibale_modem_voice_cmd *cmd)
{
	if (cmd == NULL) {
		return EINVAL;
	}
	if (cmd->action == 1) {
		uint8_t tlv[36];
		size_t nlen = strnlen(cmd->number, sizeof(cmd->number) - 1);
		if (nlen == 0) {
			return EINVAL;
		}
		tlv[0] = 0x01;
		tlv[1] = (uint8_t)nlen;
		tlv[2] = 0x00;
		memcpy(tlv + 3, cmd->number, nlen);
		modem_status.call_state = 1;
		printf("annibale-modem: initiating voice dial (number=***)\n");
		return modem_qmi_send(SLOT_VOICE, QMI_VOICE_DIAL_CALL, tlv, (uint16_t)(3 + nlen));
	} else if (cmd->action == 2 || cmd->action == 3) {
		uint8_t tlv[4] = { 0x01, 0x01, 0x00, cmd->call_id ? cmd->call_id : 1 };
		modem_status.call_state = cmd->action == 2 ? 3 : 0;
		return modem_qmi_send(SLOT_VOICE, cmd->action == 2 ? QMI_VOICE_ANSWER_CALL : QMI_VOICE_END_CALL,
		    tlv, sizeof(tlv));
	}
	return EINVAL;
}

static int
modem_sms_send(const struct annibale_modem_sms_cmd *sms)
{
	uint8_t tlv[192];
	uint16_t plen;

	if (sms == NULL || sms->pdu_len == 0 || sms->pdu_len > sizeof(sms->pdu)) {
		return EINVAL;
	}
	plen = 4 + sms->pdu_len;
	tlv[0] = 0x01;
	tlv[1] = (uint8_t)(plen & 0xff);
	tlv[2] = (uint8_t)(plen >> 8);
	tlv[3] = (uint8_t)(sms->format ? sms->format : 6);
	tlv[4] = (uint8_t)(sms->pdu_len & 0xff);
	tlv[5] = (uint8_t)(sms->pdu_len >> 8);
	memcpy(tlv + 6, sms->pdu, sms->pdu_len);
	printf("annibale-modem: sending QMI WMS PDU (%u bytes, recipient=***)\n", sms->pdu_len);
	return modem_qmi_send(SLOT_WMS, QMI_WMS_RAW_SEND, tlv, (uint16_t)(3 + plen));
}

/* ---- Background bringup & polling thread ----------------------------------------------------------- */

static void
modem_worker_thread(__unused void *arg, __unused wait_result_t wres)
{
	uint32_t dtb_pas = MPSS_DTB_PAS_ID, fw_pas = MPSS_FW_PAS_ID;

	for (unsigned w = 0; w < 75 && rootvnode == NULLVP; w++) {
		modem_sleep_ms(200);
	}

	smem_base = (volatile uint32_t *)ml_io_map_unmappable(SM8750_SMEM_PHYS, SM8750_SMEM_SIZE, VM_WIMG_WCOMB);
	ipcc_base = ml_io_map(SM8750_IPCC_MPROC_PHYS, SM8750_IPCC_MPROC_SIZE);
	tcsr_lock_base = ml_io_map(SM8750_TCSR_SPINLOCK_PHYS, 0x1000);

	modem_open_qmi_clients();

	if (!annibale_fw_exists("modem.mdt")) {
		printf("annibale-modem: /fw/modem.mdt not present; staying in standby QRTR state (dtb=0x%llx, mpss=0x%llx)\n",
		    MPSS_DTB_MEM_PHYS, MPSS_FW_MEM_PHYS);
		goto poll_loop;
	}
	if (modem_boot_arg == 1) {
		printf("annibale-modem: modem=1 survey complete (SMEM=0x%llx, dtb=0x%llx, mpss=0x%llx)\n",
		    smem_phys_active, MPSS_DTB_MEM_PHYS, MPSS_FW_MEM_PHYS);
		goto poll_loop;
	}

	lck_mtx_lock(&modem_lock);
	modem_status.state = MODEM_STATE_BOOTING;
	modem_sync_sysctls();
	lck_mtx_unlock(&modem_lock);

	if (annibale_load_mdt_image("modem_dtb", MPSS_DTB_PAS_ID, MPSS_DTB_PAS_ID_ALT,
	    MPSS_DTB_MEM_PHYS, MPSS_DTB_MEM_SIZE, &dtb_pas)) {
		(void)annibale_tz_pas_auth_reset(dtb_pas);
	}
	if (annibale_load_mdt_image("ipa_fws", 15, 0, 0x9b080000ULL, 0x10000ULL, NULL)) {
		(void)annibale_tz_pas_auth_reset(15);
	}
	if (modem_boot_arg >= 3 &&
	    annibale_load_mdt_image("modem", MPSS_FW_PAS_ID, 0, MPSS_FW_MEM_PHYS, MPSS_FW_MEM_SIZE, &fw_pas)) {
		int64_t rc = annibale_tz_pas_auth_reset(fw_pas);
		printf("annibale-modem: PAS_AUTH_AND_RESET(modem, pas=%u) returned %lld\n", fw_pas, rc);
	}

poll_loop:
	for (unsigned tick = 0;; tick++) {
		bool newly_open = false;

		lck_mtx_lock(&modem_lock);
		if (smem_base != NULL) {
			bool was_open = mpss_glink.ipcrtr_open;
			mpss_glink_poll_rx();
			if (!mpss_glink.agreed && mpss_glink.ready && (tick % 50 == 0)) {
				mpss_glink_write_raw(GLINK_CMD_VERSION, 1, GLINK_FEATURE_INTENT_REUSE, NULL, 0);
			}
			if (!was_open && mpss_glink.ipcrtr_open) {
				newly_open = true;
				if (modem_status.state < MODEM_STATE_GLINK_UP) {
					modem_status.state = MODEM_STATE_GLINK_UP;
				}
			}
			if (mpss_glink.ipcrtr_open && (tick % 20 == 0)) {
				modem_qmi_poll_services();
			}
		}
		if ((uint32_t)sysctl_cellular_bars <= 4 && sysctl_cellular_bars != (int)modem_status.signal_bars &&
		    !qmi_slots[SLOT_NAS].online) {
			modem_status.signal_bars = (uint32_t)sysctl_cellular_bars;
		}
		lck_mtx_unlock(&modem_lock);

		if (newly_open) {
			annibale_qrtr_attach_node(QRTR_NODE_MPSS, mpss_qrtr_send);
			printf("annibale-modem: SMEM GLINK IPCRTR up with MPSS (node %u)\n", QRTR_NODE_MPSS);
		}
		if (mpss_glink.ipcrtr_open) {
			annibale_qrtr_fresh();
		}
		modem_sleep_ms(mpss_glink.ipcrtr_open ? 25 : 200);
	}
}

/* ---- /dev/annibale-modem character device ----------------------------------------------------------- */

static int
modem_dev_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
modem_dev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
modem_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct annibale_modem_status snap;
	char line[256];
	int len;

	if (uio_offset(uio) > 0) {
		return 0;
	}
	lck_mtx_lock(&modem_lock);
	snap = modem_status;
	lck_mtx_unlock(&modem_lock);

	if (uio_resid(uio) == (user_ssize_t)sizeof(snap)) {
		return uiomove((caddr_t)&snap, sizeof(snap), uio);
	}
	len = snprintf(line, sizeof(line),
	    "state=%u sim=%u reg=%u bars=%u rsrp=%d rat=%s operator=%s mcc=%u mnc=%u mtu=%u call=%u sms_tx=%u sms_rx=%u\n",
	    snap.state, snap.sim_state, snap.reg_state, snap.signal_bars, snap.rsrp_dbm,
	    snap.rat[0] ? snap.rat : "None", snap.operator_name[0] ? snap.operator_name : "None",
	    snap.mcc, snap.mnc, snap.mtu, snap.call_state, snap.sms_sent, snap.sms_received);
	return len > 0 ? uiomove(line, (int)min((user_ssize_t)len, uio_resid(uio)), uio) : 0;
}

static int
modem_dev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	char cmd[128] = { 0 };
	int copy_len = (int)min((user_ssize_t)(sizeof(cmd) - 1), uio_resid(uio));
	int err;

	if (copy_len <= 0) {
		return 0;
	}
	err = uiomove(cmd, copy_len, uio);
	if (err != 0) {
		return err;
	}
	lck_mtx_lock(&modem_lock);
	if (strncmp(cmd, "poll", 4) == 0) {
		for (unsigned i = 0; i < SLOT_COUNT; i++) {
			qmi_slots[i].queried = false;
		}
		modem_qmi_poll_services();
	} else if (strncmp(cmd, "data_up", 7) == 0) {
		wds_data_wanted = true;
		qmi_slots[SLOT_WDS].queried = false;
		modem_qmi_poll_services();
	} else if (strncmp(cmd, "data_down", 9) == 0) {
		wds_data_wanted = false;
		if (wds_pkt_handle != 0) {
			uint8_t tlv[7] = { 0x01, 0x04, 0x00 };
			memcpy(tlv + 3, &wds_pkt_handle, 4);
			modem_qmi_send(SLOT_WDS, QMI_WDS_STOP_NET_IF, tlv, sizeof(tlv));
			wds_pkt_handle = 0;
		}
	} else if (strncmp(cmd, "hangup", 6) == 0) {
		struct annibale_modem_voice_cmd vc = { .action = 3, .call_id = 1 };
		modem_voice_action(&vc);
	} else if (strncmp(cmd, "answer", 6) == 0) {
		struct annibale_modem_voice_cmd vc = { .action = 2, .call_id = 1 };
		modem_voice_action(&vc);
	}
	modem_sync_sysctls();
	lck_mtx_unlock(&modem_lock);
	return 0;
}

static int
modem_dev_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused struct proc *p)
{
	int err = 0;

	lck_mtx_lock(&modem_lock);
	switch (cmd) {
	case ANNIBALE_MODEM_IOC_GET_STATUS:
		*(struct annibale_modem_status *)data = modem_status;
		break;
	case ANNIBALE_MODEM_IOC_POLL:
		for (unsigned i = 0; i < SLOT_COUNT; i++) {
			qmi_slots[i].queried = false;
		}
		modem_qmi_poll_services();
		break;
	case ANNIBALE_MODEM_IOC_DATA_CALL:
		wds_data_wanted = (*(uint32_t *)data != 0);
		qmi_slots[SLOT_WDS].queried = false;
		modem_qmi_poll_services();
		break;
	case ANNIBALE_MODEM_IOC_VOICE_CMD:
		err = modem_voice_action((const struct annibale_modem_voice_cmd *)data);
		break;
	case ANNIBALE_MODEM_IOC_SMS_SEND:
		err = modem_sms_send((const struct annibale_modem_sms_cmd *)data);
		break;
	default:
		err = ENOTTY;
		break;
	}
	modem_sync_sysctls();
	lck_mtx_unlock(&modem_lock);
	return err;
}

static const struct cdevsw modem_cdevsw = {
	.d_open = modem_dev_open,
	.d_close = modem_dev_close,
	.d_read = modem_dev_read,
	.d_write = modem_dev_write,
	.d_ioctl = modem_dev_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0,
};

void
annibale_modem_init(void)
{
	thread_t th;
	int major;

	if (!annibale_platform_is_annibale() || modem_inited) {
		return;
	}
	modem_inited = true;
	PE_parse_boot_argn("modem", &modem_boot_arg, sizeof(modem_boot_arg));

	major = cdevsw_add(-1, &modem_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "annibale-modem") == NULL) {
		printf("annibale-modem: failed to create /dev/annibale-modem\n");
	}
	modem_sync_sysctls();

	if (kernel_thread_start(modem_worker_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	}
	printf("annibale-modem: initialized (remoteproc-mss@04080000, dtb=0x%llx, mpss=0x%llx, modem=%u)\n",
	    MPSS_DTB_MEM_PHYS, MPSS_FW_MEM_PHYS, modem_boot_arg);
}

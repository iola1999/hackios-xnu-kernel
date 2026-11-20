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
#include <sys/annibale_rpmh.h>
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
extern uint64_t kvtophys(vm_offset_t va);
extern int annibale_ufs_part_read(const char *part_name, uint64_t offset, void *buf, uint64_t len);
extern bool annibale_ufs_fat16_path_read(const char *path, uint64_t *size_out, volatile void *dst_buf, uint64_t dst_cap);
extern kern_return_t thread_terminate(thread_t thread);
extern lck_mtx_t annibale_smem_alloc_lock;

/* Multi-node QRTR registration (bsd/dev/annibale_qrtr.c) and QMI codecs (bsd/dev/annibale_qmi.c) */
extern void annibale_qrtr_attach_node(uint32_t peer_node, int (*send)(const void *data, uint32_t length));
extern int annibale_qrtr_publish(struct annibale_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance);
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
    uint64_t mem_phys, uint64_t region_size, uint32_t *used_pas_out);
extern int64_t annibale_tz_pas_auth_reset(uint32_t pas_id);
extern int64_t annibale_tz_pas_is_supported(uint32_t pas_id, uint64_t *res1_out);
extern int64_t annibale_tz_pas_auth_reset_detail(uint32_t pas_id, uint64_t *res1_out);
extern int64_t annibale_tz_assign_mem(uint64_t region_pa, uint64_t region_sz,
    uint32_t peer_vmid, bool shared, uint64_t *res1_out);
extern bool annibale_aoss_send_msg(const char *msg);

/* ---- SM8750 MPSS hardware addresses & constants (from annibale.dtb & SM8750 map) ------------------ */

#define SM8750_SMEM_PHYS                0x81d00000ULL
#define SM8750_SMEM_ALT_PHYS            0x86000000ULL
#define SM8750_SMEM_SIZE                0x00200000ULL
#define SM8750_TCSR_SPINLOCK_PHYS       0x01f43000ULL
#define SM8750_IPCC_MPROC_PHYS          0x00406000ULL
#define SM8750_IPCC_MPROC_SIZE          0x00001000ULL

#define MPSS_DSM1_MEM_PHYS              0x84a00000ULL
#define MPSS_DSM1_MEM_SIZE              0x04900000ULL
#define MPSS_DSM2_MEM_PHYS              0x89300000ULL
#define MPSS_DSM2_MEM_SIZE              0x00a80000ULL
#define MPSS_VMID_MSA                   15u
#define MPSS_DTB_MEM_PHYS               0x9b000000ULL
#define MPSS_DTB_MEM_SIZE               0x00080000ULL
#define MPSS_DTB_PAS_ID                 38u
/* pas=37 is cdsp_dtb (sec_id=0x52); modem_dtb is pas=38 (sec_id=0x51). */
#define MPSS_DTB_PAS_ID_ALT             0u
#define MPSS_FW_MEM_PHYS                0x8ba00000ULL
#define MPSS_FW_MEM_SIZE                0x0f600000ULL
#define MPSS_FW_PAS_ID                  4u

#define SMEM_HOST_APPS                  0u
#define SMEM_HOST_MPSS                  1u
#define SMEM_HOST_GLOBAL                0xfffeu
#define SMEM_CANARY                     0xa5a5u
#define ITEM_MPSS_CRASH_REASON          421u
#define ITEM_SMP2P_MPSS_OURS            428u
#define ITEM_SMP2P_MPSS_THEIRS          435u
#define ITEM_GLINK_DESCRIPTOR           478u
#define ITEM_GLINK_OUR_QUEUE            479u
#define ITEM_GLINK_THEIR_QUEUE          480u
#define GLINK_QUEUE_SIZE                16384u
#define SMP2P_MAGIC                     0x504d5324u
#define SMP2P_VERSION                   1u
#define SMP2P_SSR_ACK                   (1u << 0)
#define SMP2P_MPSS_FATAL                (1u << 0)
#define SMP2P_MPSS_READY                (1u << 1)
#define SMP2P_MPSS_HANDOVER             (1u << 2)

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
#define GLINK_CMD_RX_DONE               6u
#define GLINK_CMD_RX_INTENT_REQ         7u
#define GLINK_CMD_RX_INTENT_REQ_ACK     8u
#define GLINK_CMD_TX_DATA               9u
#define GLINK_CMD_TX_SHORT_DATA         10u
#define GLINK_CMD_CLOSE_ACK             11u
#define GLINK_CMD_TX_DATA_CONT          12u
#define GLINK_CMD_READ_NOTIF            13u
#define GLINK_CMD_RX_DONE_REUSE         14u
#define GLINK_CMD_SIGNALS               15u
#define GLINK_FEATURE_INTENT_REUSE      (1u << 0)
#define GLINK_OUR_CID                   1u
#define MPSS_GLINK_MAX_INTENTS          64u
#define MPSS_GLINK_REASM_MAX            8192u

#define QRTR_NODE_MPSS                  0u

#define QMI_SVC_WDS                     0x01u
#define QMI_SVC_DMS                     0x02u
#define QMI_SVC_NAS                     0x03u
#define QMI_SVC_WMS                     0x05u
#define QMI_SVC_VOICE                   0x09u
#define QMI_SVC_UIM                     0x0bu
#define QMI_SVC_LOC                     0x10u

#define QMI_LOC_GET_SUPPORTED_MSGS      0x001eu
#define QMI_LOC_GET_SUPPORTED_FIELDS    0x001fu
#define QMI_LOC_INFORM_CLIENT_REV       0x0020u
#define QMI_LOC_REG_EVENTS              0x0021u
#define QMI_LOC_START                   0x0022u
#define QMI_LOC_STOP                    0x0023u

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

static int sysctl_modem_state = MODEM_STATE_OFF;
static int sysctl_cellular_bars = 0;
static int sysctl_cellular_sim_state = 0;
static int sysctl_cellular_rsrp = -140;
static char sysctl_cellular_rat[16] = "None";
static char sysctl_cellular_operator[32] = "";
static char sysctl_modem_loc_status[128] =
    "dms_mode=255 dms_err=0 uim_card=255 uim_err=255 loc_reg=0 loc_fb=0 loc_err=0 loc_rev=0 loc_sv=0 loc_eng=-1 loc_fix=255";

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
SYSCTL_STRING(_kern_annibale, OID_AUTO, modem_loc_status, CTLFLAG_RD | CTLFLAG_LOCKED,
    sysctl_modem_loc_status, sizeof(sysctl_modem_loc_status), "MPSS DMS/UIM/QMI_LOC status (privacy-safe)");

/* ---- Driver state ---------------------------------------------------------------------------------- */

static LCK_GRP_DECLARE(modem_lck_grp, "annibale-modem");
static LCK_MTX_DECLARE(modem_lock, &modem_lck_grp);
static LCK_MTX_DECLARE(mpss_glink_lock, &modem_lck_grp);

static bool modem_inited;
static uint32_t modem_boot_arg;
static volatile uint32_t *smem_base;
static uint64_t smem_phys_active = SM8750_SMEM_PHYS;
static vm_offset_t ipcc_base;
static vm_offset_t tcsr_lock_base;

struct mpss_glink_intent {
	uint32_t id;
	uint32_t size;
	bool     valid;
	bool     busy;
};

static struct {
	uint32_t desc_off, our_q_off, our_q_sz, their_q_off, their_q_sz;
	uint32_t their_cid;
	uint32_t our_features;
	bool     ready, version_sent, agreed, ipcrtr_asked, ipcrtr_ack, ipcrtr_open;
	bool     intents_sent, qrtr_attached, qrtr_hello_pending, logged_stall;
	struct mpss_glink_intent their_intents[MPSS_GLINK_MAX_INTENTS];
	uint32_t their_intent_count;
	uint32_t next_our_iid;
	uint32_t tx_count, rx_count, rx_log_count, open_log_count;
	uint32_t reasm_len, reasm_total;
	bool     reasm_active, reasm_drop;
	uint8_t  reasm_buf[MPSS_GLINK_REASM_MAX];
} mpss_glink = {
	.our_features = GLINK_FEATURE_INTENT_REUSE,
};

struct qmi_svc_slot {
	uint32_t service_id;
	uint32_t node, port;
	bool     online;
	bool     queried;
	uint16_t next_txn;
	struct annibale_qrtr_port *qrtr_port;
};

enum {
	SLOT_UIM = 0, SLOT_DMS = 1, SLOT_NAS = 2, SLOT_WDS = 3,
	SLOT_VOICE = 4, SLOT_WMS = 5, SLOT_LOC = 6, SLOT_COUNT = 7,
};

static struct qmi_svc_slot qmi_slots[SLOT_COUNT] = {
	[SLOT_UIM]   = { .service_id = QMI_SVC_UIM },
	[SLOT_DMS]   = { .service_id = QMI_SVC_DMS },
	[SLOT_NAS]   = { .service_id = QMI_SVC_NAS },
	[SLOT_WDS]   = { .service_id = QMI_SVC_WDS },
	[SLOT_VOICE] = { .service_id = QMI_SVC_VOICE },
	[SLOT_WMS]   = { .service_id = QMI_SVC_WMS },
	[SLOT_LOC]   = { .service_id = QMI_SVC_LOC },
};

static struct annibale_modem_status modem_status = {
	.magic = ANNIBALE_MODEM_MAGIC,
	.state = MODEM_STATE_OFF,
	.rsrp_dbm = -140,
	.mtu = 1500,
	.rat = "None",
};

static uint32_t wds_pkt_handle;
static bool wds_data_wanted = true;
static uint8_t dms_retry_count;
static uint32_t modem_tick, dms_retry_at_tick, loc_stop_tick;
static bool loc_rev_done, loc_reg_sent, loc_reg_ok, loc_min_retried, loc_probe_started, loc_probe_stopped;
static const uint8_t loc_start_tlvs[] = { 0x01, 0x01, 0x00, 0x01, 0x10, 0x04, 0x00, 0x01, 0, 0, 0,
    0x12, 0x04, 0x00, 0x01, 0, 0, 0, 0x13, 0x04, 0x00, 0xe8, 0x03, 0, 0 };
static const uint8_t loc_stop_tlvs[] = { 0x01, 0x01, 0x00, 0x01 };

/* ---- Low-level MMIO & SMEM helpers ----------------------------------------------------------------- */

static void
modem_sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static inline void
mmio_wr(vm_offset_t addr, uint32_t v)
{
	__builtin_arm_dsb(0xf);
	*(volatile uint32_t *)addr = v;
	__builtin_arm_dsb(0xf);
}

static inline uint32_t
smem_rd(uint32_t byte_off)
{
	uint32_t v = smem_base[byte_off / 4];
	__builtin_arm_dsb(0xf);
	return v;
}

static inline void
smem_wr(uint32_t byte_off, uint32_t v)
{
	__builtin_arm_dsb(0xf);
	smem_base[byte_off / 4] = v;
	__builtin_arm_dsb(0xf);
}

static void modem_format_loc_status(char *out, size_t cap, bool loc_reg, bool loc_fb);

static void
modem_sync_sysctls(void)
{
	sysctl_modem_state = (int)modem_status.state;
	sysctl_cellular_bars = (int)modem_status.signal_bars;
	sysctl_cellular_sim_state = (int)modem_status.sim_state;
	sysctl_cellular_rsrp = (int)modem_status.rsrp_dbm;
	strlcpy(sysctl_cellular_rat, modem_status.rat, sizeof(sysctl_cellular_rat));
	strlcpy(sysctl_cellular_operator, modem_status.operator_name, sizeof(sysctl_cellular_operator));
	modem_format_loc_status(sysctl_modem_loc_status, sizeof(sysctl_modem_loc_status),
	    loc_reg_ok, loc_min_retried);
}

#include "annibale_modem_boot.inc"
#include "annibale_modem_svc.inc"
#include "annibale_modem_tftp.inc"

/* ---- SMEM GLINK IPCRTR transport with MPSS (partition 0 <-> 1) ------------------------------------- */

static int
mpss_qrtr_send(const void *data, uint32_t length)
{
	uint32_t iid = 0;
	uint8_t *frame;
	bool sent;

	lck_mtx_lock(&mpss_glink_lock);
	if (!mpss_glink.ipcrtr_open) {
		lck_mtx_unlock(&mpss_glink_lock);
		return ENOTCONN;
	}
	if (!mpss_glink_take_intent(length, &iid)) {
		mpss_glink_write_raw(GLINK_CMD_RX_INTENT_REQ, GLINK_OUR_CID, length, NULL, 0);
		lck_mtx_unlock(&mpss_glink_lock);
		return EAGAIN;
	}
	frame = kalloc_data(8u + length, Z_WAITOK);
	if (frame == NULL) {
		mpss_glink_complete_intent(iid, true);
		lck_mtx_unlock(&mpss_glink_lock);
		return ENOMEM;
	}
	memcpy(frame, &length, 4u);
	bzero(frame + 4, 4u);
	memcpy(frame + 8, data, length);
	sent = mpss_glink_write_raw(GLINK_CMD_TX_DATA, GLINK_OUR_CID, iid, frame, 8u + length);
	kfree_data(frame, 8u + length);
	if (!sent) {
		mpss_glink_complete_intent(iid, true);
	}
	lck_mtx_unlock(&mpss_glink_lock);
	return sent ? 0 : EIO;
}

static void
mpss_glink_poll_rx(void)
{
	uint32_t tail, head;
	bool stop_loop = false;

	lck_mtx_lock(&mpss_glink_lock);
	if (!mpss_glink.ready && !mpss_glink_locate_queues()) {
		lck_mtx_unlock(&mpss_glink_lock);
		return;
	}
	tail = smem_rd(mpss_glink.desc_off + 8u);
	head = smem_rd(mpss_glink.desc_off + 12u);
	if (tail >= mpss_glink.their_q_sz || head >= mpss_glink.their_q_sz) {
		if (head < mpss_glink.their_q_sz) {
			smem_wr(mpss_glink.desc_off + 8u, head);
			(void)smem_rd(mpss_glink.desc_off + 8u);
		}
		lck_mtx_unlock(&mpss_glink_lock);
		return;
	}
	while (!stop_loop && tail != head && tail < mpss_glink.their_q_sz && head < mpss_glink.their_q_sz) {
		uint32_t w0 = smem_rd(mpss_glink.their_q_off + tail);
		uint32_t w1 = smem_rd(mpss_glink.their_q_off + ((tail + 4u) % mpss_glink.their_q_sz));
		uint16_t cmd = (uint16_t)(w0 & 0xffffu);
		uint16_t p1 = (uint16_t)(w0 >> 16);
		uint32_t p2 = w1;

		tail = (tail + 8u) % mpss_glink.their_q_sz;
		mpss_glink.rx_count++;
		if (mpss_glink.rx_log_count < 32u) {
			mpss_glink.rx_log_count++;
			printf("annibale-modem: GLINK rx #%u cmd=%u p1=%u p2=%#x\n",
			    mpss_glink.rx_count, cmd, p1, p2);
		}

		switch (cmd) {
		case GLINK_CMD_VERSION:
			mpss_glink_write_raw(GLINK_CMD_VERSION_ACK, p1, p2 & mpss_glink.our_features, NULL, 0);
			if (!mpss_glink.version_sent) {
				mpss_glink.version_sent = mpss_glink_write_raw(GLINK_CMD_VERSION, 1u,
				    mpss_glink.our_features, NULL, 0);
			}
			break;
		case GLINK_CMD_VERSION_ACK:
			if (p1 == 1u && (p2 & mpss_glink.our_features) != mpss_glink.our_features) {
				mpss_glink.our_features &= p2;
				mpss_glink_write_raw(GLINK_CMD_VERSION, 1u, mpss_glink.our_features, NULL, 0);
			} else {
				mpss_glink.agreed = true;
				if (!mpss_glink.ipcrtr_asked) {
					mpss_glink.ipcrtr_asked = true;
					mpss_glink_write_raw(GLINK_CMD_OPEN, GLINK_OUR_CID, 7u, "IPCRTR", 7u);
				}
			}
			break;
		case GLINK_CMD_OPEN: {
			char cname[32] = { 0 };
			uint32_t nlen = p2 & 0xffffu;
			uint32_t aligned_len = (nlen + 7u) & ~7u;

			if (nlen == 0 || aligned_len > mpss_glink.their_q_sz) {
				tail = head;
				stop_loop = true;
				break;
			}
			for (uint32_t i = 0; i < aligned_len && i < sizeof(cname) - 1u; i += 4u) {
				uint32_t cw = smem_rd(mpss_glink.their_q_off + ((tail + i) % mpss_glink.their_q_sz));
				uint32_t take = (sizeof(cname) - 1u - i >= 4u) ? 4u : (sizeof(cname) - 1u - i);
				memcpy(cname + i, &cw, take);
			}
			cname[nlen < sizeof(cname) ? nlen : sizeof(cname) - 1u] = '\0';
			tail = (tail + aligned_len) % mpss_glink.their_q_sz;
			if (mpss_glink.open_log_count < 24u) {
				mpss_glink.open_log_count++;
				printf("annibale-modem: GLINK open rcid=%u name=%s (nlen=%u)\n", p1, cname, nlen);
			}
			mpss_glink_write_raw(GLINK_CMD_OPEN_ACK, p1, 0, NULL, 0);
			if (strcmp(cname, "IPCRTR") == 0) {
				mpss_glink.their_cid = p1;
				if (!mpss_glink.ipcrtr_asked) {
					mpss_glink.ipcrtr_asked = true;
					mpss_glink_write_raw(GLINK_CMD_OPEN, GLINK_OUR_CID, 7u, "IPCRTR", 7u);
				}
				if (mpss_glink.ipcrtr_ack && !mpss_glink.ipcrtr_open) {
					mpss_glink.ipcrtr_open = true;
					mpss_glink_queue_intents();
					if (mpss_glink.their_intent_count == 0) {
						mpss_glink_write_raw(GLINK_CMD_RX_INTENT_REQ, GLINK_OUR_CID, 0x800u, NULL, 0);
					}
					stop_loop = true;
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
					if (mpss_glink.their_intent_count == 0) {
						mpss_glink_write_raw(GLINK_CMD_RX_INTENT_REQ, GLINK_OUR_CID, 0x800u, NULL, 0);
					}
					stop_loop = true;
				}
			}
			break;
		case GLINK_CMD_INTENT:
			if (p2 > 64u || p2 * 8u > mpss_glink.their_q_sz) {
				tail = head;
				stop_loop = true;
				break;
			}
			for (uint32_t i = 0; i < p2; i++) {
				uint32_t sz = smem_rd(mpss_glink.their_q_off + ((tail + i * 8u) % mpss_glink.their_q_sz));
				uint32_t id = smem_rd(mpss_glink.their_q_off + ((tail + i * 8u + 4u) % mpss_glink.their_q_sz));
				if (p1 == mpss_glink.their_cid && mpss_glink.their_cid != 0) {
					mpss_glink_add_intent(id, sz);
				}
			}
			tail = (tail + p2 * 8u) % mpss_glink.their_q_sz;
			break;
		case GLINK_CMD_RX_DONE:
		case GLINK_CMD_RX_DONE_REUSE:
			if (p1 == mpss_glink.their_cid && mpss_glink.their_cid != 0) {
				mpss_glink_complete_intent(p2, cmd == GLINK_CMD_RX_DONE_REUSE);
			}
			break;
		case GLINK_CMD_RX_INTENT_REQ:
			if (p1 == mpss_glink.their_cid && mpss_glink.their_cid != 0) {
				if (!mpss_glink.intents_sent) {
					mpss_glink_queue_intents();
				} else if (p2 > 0x4400u && p2 <= 0x8000u) {
					uint32_t pair[2] = { p2, ++mpss_glink.next_our_iid };
					mpss_glink_write_raw(GLINK_CMD_INTENT, GLINK_OUR_CID, 1u, pair, sizeof(pair));
				}
				mpss_glink_write_raw(GLINK_CMD_RX_INTENT_REQ_ACK, GLINK_OUR_CID, 1u, NULL, 0);
			}
			break;
		case GLINK_CMD_RX_INTENT_REQ_ACK:
		case GLINK_CMD_CLOSE_ACK:
		case GLINK_CMD_SIGNALS:
			break;
		case GLINK_CMD_READ_NOTIF:
			ipcc_ring_mpss(IPCC_SIG_GLINK);
			break;
		case GLINK_CMD_TX_SHORT_DATA:
			tail = (tail + 16u) % mpss_glink.their_q_sz;
			break;
		case GLINK_CMD_TX_DATA:
		case GLINK_CMD_TX_DATA_CONT: {
			uint32_t frag_sz = smem_rd(mpss_glink.their_q_off + tail);
			uint32_t left_sz = smem_rd(mpss_glink.their_q_off + ((tail + 4u) % mpss_glink.their_q_sz));
			uint32_t aligned_frag = (frag_sz + 7u) & ~7u;

			tail = (tail + 8u) % mpss_glink.their_q_sz;
			if (aligned_frag > mpss_glink.their_q_sz) {
				tail = head;
				stop_loop = true;
				break;
			}
			if (p1 == mpss_glink.their_cid && mpss_glink.their_cid != 0) {
				if (cmd == GLINK_CMD_TX_DATA) {
					uint64_t total = (uint64_t)frag_sz + (uint64_t)left_sz;
					mpss_glink.reasm_active = true;
					mpss_glink.reasm_len = 0;
					mpss_glink.reasm_total = (uint32_t)total;
					mpss_glink.reasm_drop = (total == 0 || total > MPSS_GLINK_REASM_MAX);
				}
				if (mpss_glink.reasm_active && !mpss_glink.reasm_drop) {
					if ((uint64_t)mpss_glink.reasm_len + (uint64_t)frag_sz > (uint64_t)mpss_glink.reasm_total ||
					    (uint64_t)mpss_glink.reasm_len + (uint64_t)frag_sz > MPSS_GLINK_REASM_MAX) {
						mpss_glink.reasm_drop = true;
					} else {
						for (uint32_t i = 0; i < frag_sz; i += 4u) {
							uint32_t dw = smem_rd(mpss_glink.their_q_off + ((tail + i) % mpss_glink.their_q_sz));
							uint32_t take = (frag_sz - i >= 4u) ? 4u : (frag_sz - i);
							memcpy(mpss_glink.reasm_buf + mpss_glink.reasm_len + i, &dw, take);
						}
						mpss_glink.reasm_len += frag_sz;
					}
				}
				tail = (tail + aligned_frag) % mpss_glink.their_q_sz;
				smem_wr(mpss_glink.desc_off + 8u, tail);
				(void)smem_rd(mpss_glink.desc_off + 8u);
				if (left_sz == 0) {
					uint32_t pkt_len = (!mpss_glink.reasm_drop && mpss_glink.reasm_active &&
					    mpss_glink.reasm_len == mpss_glink.reasm_total) ? mpss_glink.reasm_len : 0u;
					uint8_t *pkt = (pkt_len > 0) ? kalloc_data(pkt_len, Z_NOWAIT) : NULL;

					if (pkt != NULL) {
						memcpy(pkt, mpss_glink.reasm_buf, pkt_len);
					}
					mpss_glink.reasm_active = false;
					if ((mpss_glink.our_features & GLINK_FEATURE_INTENT_REUSE) != 0) {
						mpss_glink_write_raw(GLINK_CMD_RX_DONE_REUSE, GLINK_OUR_CID, p2, NULL, 0);
					} else {
						uint32_t pair[2] = { 0x4400u, ++mpss_glink.next_our_iid };
						mpss_glink_write_raw(GLINK_CMD_RX_DONE, GLINK_OUR_CID, p2, NULL, 0);
						mpss_glink_write_raw(GLINK_CMD_INTENT, GLINK_OUR_CID, 1u, pair, sizeof(pair));
					}
					if (pkt != NULL) {
						lck_mtx_unlock(&mpss_glink_lock);
						annibale_qrtr_input(pkt, pkt_len);
						kfree_data(pkt, pkt_len);
						lck_mtx_lock(&mpss_glink_lock);
					}
				}
				tail = smem_rd(mpss_glink.desc_off + 8u);
				head = smem_rd(mpss_glink.desc_off + 12u);
				continue;
			}
			tail = (tail + aligned_frag) % mpss_glink.their_q_sz;
			break;
		}
		case GLINK_CMD_CLOSE:
			mpss_glink_write_raw(GLINK_CMD_CLOSE_ACK, p1, 0, NULL, 0);
			break;
		default:
			printf("annibale-modem: GLINK unknown cmd=%u p1=%u p2=%#x (resync tail=%u->%u)\n",
			    cmd, p1, p2, tail, head);
			tail = head;
			stop_loop = true;
			break;
		}
		smem_wr(mpss_glink.desc_off + 8u, tail);
		(void)smem_rd(mpss_glink.desc_off + 8u);
		head = smem_rd(mpss_glink.desc_off + 12u);
	}
	lck_mtx_unlock(&mpss_glink_lock);
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
	if (qmi_slots[SLOT_UIM].online && !qmi_slots[SLOT_UIM].queried &&
	    modem_qmi_send(SLOT_UIM, QMI_UIM_GET_CARD_STATUS, NULL, 0) == 0) {
		qmi_slots[SLOT_UIM].queried = true;
	}
	if (qmi_slots[SLOT_DMS].online && !qmi_slots[SLOT_DMS].queried &&
	    modem_tick >= dms_retry_at_tick &&
	    modem_qmi_send(SLOT_DMS, QMI_DMS_GET_OPERATING_MODE, NULL, 0) == 0) {
		qmi_slots[SLOT_DMS].queried = true;
	}
	if (qmi_slots[SLOT_NAS].online && !qmi_slots[SLOT_NAS].queried &&
	    modem_qmi_send(SLOT_NAS, QMI_NAS_GET_SERVING_SYSTEM, NULL, 0) == 0) {
		qmi_slots[SLOT_NAS].queried = true;
		modem_qmi_send(SLOT_NAS, QMI_NAS_GET_SIG_INFO, NULL, 0);
	}
	if (qmi_slots[SLOT_WDS].online && !qmi_slots[SLOT_WDS].queried &&
	    modem_status.reg_state == 1 && wds_data_wanted) {
		static const uint8_t start_tlvs[] = { 0x19, 0x01, 0x00, 0x04 };
		static const uint8_t rt_tlvs[] = { 0x10, 0x04, 0x00, 0x10, 0x23, 0x00, 0x00 };

		if (modem_qmi_send(SLOT_WDS, QMI_WDS_START_NET_IF, start_tlvs, sizeof(start_tlvs)) == 0) {
			qmi_slots[SLOT_WDS].queried = true;
			modem_qmi_send(SLOT_WDS, QMI_WDS_GET_RUNTIME_SETTINGS, rt_tlvs, sizeof(rt_tlvs));
		}
	}
	if (qmi_slots[SLOT_WMS].online && !qmi_slots[SLOT_WMS].queried) {
		static const uint8_t wms_tlvs[] = { 0x10, 0x01, 0x00, 0x01 };
		if (modem_qmi_send(SLOT_WMS, QMI_WMS_EVENT_REPORT, wms_tlvs, sizeof(wms_tlvs)) == 0) {
			qmi_slots[SLOT_WMS].queried = true;
		}
	}
	if (qmi_slots[SLOT_LOC].online && !qmi_slots[SLOT_LOC].queried) {
		static const uint8_t loc_fields_tlvs[] = { 0x01, 0x02, 0x00, 0x21, 0x00 };
		static const uint8_t loc_rev_tlvs[] = { 0x01, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00 };
		(void)modem_qmi_send(SLOT_LOC, QMI_LOC_GET_SUPPORTED_MSGS, NULL, 0);
		(void)modem_qmi_send(SLOT_LOC, QMI_LOC_GET_SUPPORTED_FIELDS, loc_fields_tlvs, sizeof(loc_fields_tlvs));
		if (modem_qmi_send(SLOT_LOC, QMI_LOC_INFORM_CLIENT_REV, loc_rev_tlvs, sizeof(loc_rev_tlvs)) == 0) {
			qmi_slots[SLOT_LOC].queried = true;
			printf("annibale-modem: QMI LOC querying msgs/fields(0x21) + INFORM_CLIENT_REV=2\n");
		}
	}
	if (qmi_slots[SLOT_LOC].online && loc_reg_ok && !loc_probe_started) {
		if (modem_qmi_send(SLOT_LOC, QMI_LOC_START, loc_start_tlvs, sizeof(loc_start_tlvs)) == 0) {
			loc_probe_started = true;
			loc_stop_tick = modem_tick + 600u;
			printf("annibale-modem: QMI LOC probe START (15s bounded)\n");
		}
	}
}

static void
modem_qmi_rx_cb(void *context, uint32_t from_node, uint32_t from_port, const void *data, size_t size)
{
	unsigned slot_idx = (unsigned)(uintptr_t)context;
	const uint8_t *buf = data;
	uint8_t qmi_flags;
	uint16_t txn_id, msg_id, tlv_len;
	const uint8_t *tlvs;

	(void)from_node;
	(void)from_port;
	if (slot_idx >= SLOT_COUNT || size < 7) {
		return;
	}
	qmi_flags = buf[0];
	txn_id = (uint16_t)(buf[1] | (buf[2] << 8));
	msg_id = (uint16_t)(buf[3] | (buf[4] << 8));
	tlv_len = (uint16_t)(buf[5] | (buf[6] << 8));
	if ((size_t)7 + tlv_len > size) {
		return;
	}
	tlvs = buf + 7;

	lck_mtx_lock(&modem_lock);
	switch (slot_idx) {
	case SLOT_UIM:
		modem_uim_handle_rx(qmi_flags, msg_id, tlvs, tlv_len, &modem_status.sim_state);
		break;
	case SLOT_DMS: {
		uint8_t mode = 0;
		bool want_online = false, requery = false;
		modem_dms_handle_rx(qmi_flags, msg_id, tlvs, tlv_len, &mode, &want_online, &requery);
		if (want_online && dms_retry_count < 4u) {
			static const uint8_t online_tlv[] = { 0x01, 0x01, 0x00, 0x00 };
			if (modem_qmi_send(SLOT_DMS, QMI_DMS_SET_OPERATING_MODE, online_tlv, sizeof(online_tlv)) == 0) {
				dms_retry_count++;
			}
		}
		if (requery) {
			qmi_slots[SLOT_DMS].queried = false;
			dms_retry_at_tick = modem_tick + 80u;
		}
		break;
	}
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
		modem_voice_handle_rx(msg_id, tlvs, tlv_len, &modem_status.call_state);
		break;
	case SLOT_WMS:
		modem_wms_handle_rx(qmi_flags, msg_id, tlvs, tlv_len,
		    &modem_status.sms_received, &modem_status.sms_sent);
		break;
	case SLOT_LOC: {
		bool rev_done = false, retry_min = false;
		modem_loc_handle_rx(qmi_flags, txn_id, msg_id, tlvs, tlv_len, &rev_done, &loc_reg_ok, &retry_min);
		if (rev_done) {
			loc_rev_done = true;
		}
		if (loc_rev_done && !loc_reg_sent) {
			static const uint8_t loc_reg_tlvs[] = { 0x01, 0x08, 0x00, 0x87, 0x01, 0, 0, 0, 0, 0, 0 };
			loc_reg_sent = true;
			if (modem_qmi_send(SLOT_LOC, QMI_LOC_REG_EVENTS, loc_reg_tlvs, sizeof(loc_reg_tlvs)) == 0) {
				printf("annibale-modem: QMI LOC REG_EVENTS mask=0x187\n");
			}
		}
		if (retry_min && !loc_min_retried) {
			static const uint8_t min_tlvs[] = { 0x01, 0x08, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0 };
			loc_min_retried = true;
			if (modem_qmi_send(SLOT_LOC, QMI_LOC_REG_EVENTS, min_tlvs, sizeof(min_tlvs)) == 0) {
				printf("annibale-modem: QMI LOC fallback REG_EVENTS mask=0x01\n");
			}
		}
		break;
	}
	}
	modem_sync_sysctls();
	lck_mtx_unlock(&modem_lock);
}

static void
modem_qmi_server_cb(void *context, bool arrived, uint32_t service, uint32_t svc_version,
    uint32_t instance, uint32_t node, uint32_t port)
{
	unsigned slot_idx = (unsigned)(uintptr_t)context;

	(void)svc_version;
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
	for (unsigned w = 0; w < 75 && rootvnode == NULLVP; w++) {
		modem_sleep_ms(200);
	}

	smem_base = (volatile uint32_t *)ml_io_map_unmappable(SM8750_SMEM_PHYS, SM8750_SMEM_SIZE, VM_WIMG_WCOMB);
	ipcc_base = ml_io_map(SM8750_IPCC_MPROC_PHYS, SM8750_IPCC_MPROC_SIZE);
	tcsr_lock_base = ml_io_map(SM8750_TCSR_SPINLOCK_PHYS, 0x1000);

	modem_open_qmi_clients();
	modem_start_local_services();
	modem_start_tftp();
	modem_bringup_sequence();
	bool smp2p_seen = false;
	uint32_t smp2p_last_val = 0, smp2p_last_flags = 0;
	for (unsigned tick = 0; !mpss_stopping; tick++) {
		bool newly_open = false, retry_hello = false, is_open = false;
		uint32_t rcid_snap = 0, intent_snap = 0;

		modem_worker_glink_tick(tick, &smp2p_seen, &smp2p_last_val, &smp2p_last_flags,
		    &newly_open, &retry_hello, &is_open, &rcid_snap, &intent_snap);

		if (newly_open) {
			lck_mtx_lock(&modem_lock);
			if (modem_status.state < MODEM_STATE_GLINK_UP) {
				modem_status.state = MODEM_STATE_GLINK_UP;
				modem_sync_sysctls();
			}
			lck_mtx_unlock(&modem_lock);
			annibale_qrtr_attach_node(QRTR_NODE_MPSS, mpss_qrtr_send);
			printf("annibale-modem: SMEM GLINK IPCRTR up with MPSS (node %u, rcid=%u, intents=%u)\n",
			    QRTR_NODE_MPSS, rcid_snap, intent_snap);
		} else if (retry_hello) {
			annibale_qrtr_attach_node(QRTR_NODE_MPSS, mpss_qrtr_send);
		}

		lck_mtx_lock(&modem_lock);
		modem_tick = tick;
		if (is_open && (tick % 20 == 0)) {
			modem_qmi_poll_services();
		}
		if (is_open && loc_probe_started && !loc_probe_stopped && tick >= loc_stop_tick &&
		    modem_qmi_send(SLOT_LOC, QMI_LOC_STOP, loc_stop_tlvs, sizeof(loc_stop_tlvs)) == 0) {
			loc_probe_stopped = true;
			printf("annibale-modem: QMI LOC probe auto-STOP\n");
		}
		if ((uint32_t)sysctl_cellular_bars <= 4 && sysctl_cellular_bars != (int)modem_status.signal_bars &&
		    !qmi_slots[SLOT_NAS].online) {
			modem_status.signal_bars = (uint32_t)sysctl_cellular_bars;
		}
		lck_mtx_unlock(&modem_lock);

		if (is_open) {
			annibale_qrtr_fresh();
			modem_tftp_poll();
		}
		modem_sleep_ms(is_open ? 25 : 200);
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
	} else if (strncmp(cmd, "gps_start", 9) == 0) {
		modem_qmi_send(SLOT_LOC, QMI_LOC_START, loc_start_tlvs, sizeof(loc_start_tlvs));
	} else if (strncmp(cmd, "gps_stop", 8) == 0) {
		modem_qmi_send(SLOT_LOC, QMI_LOC_STOP, loc_stop_tlvs, sizeof(loc_stop_tlvs));
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

	if (modem_boot_arg == 0) {
		printf("annibale-modem: modem=0 (disabled)\n");
		return;
	}
	if (kernel_thread_start(modem_worker_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	}
	printf("annibale-modem: initialized (remoteproc-mss@04080000, dtb=0x%llx, mpss=0x%llx, modem=%u)\n",
	    MPSS_DTB_MEM_PHYS, MPSS_FW_MEM_PHYS, modem_boot_arg);
}

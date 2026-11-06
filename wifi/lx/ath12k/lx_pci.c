/*
 * The radio's bus for ath12k, where its pci.c and mhi.c would be: the K90's WCN7850 on PCIe root
 * complex 0. The kernel has powered the root complex and the card, trained the link and given BAR0
 * an address (bsd/dev/mhi_pcie.c, pcie=10); from there this does what ath12k's probe does for this
 * chip, in its order:
 *
 *   - probe: the chip's hardware version, ath12k's tables for it (wifi7/hw.c), the copy engines'
 *     and the data path's rings described, then core.c's start;
 *   - power up: the kernel's steps 7 and 8 (lx_os_rproc_boot()), the firmware through MHI to mission
 *     mode and QRTR on the IPCR channels, over which qmi.c then talks to the firmware;
 *   - registers: BAR0, with the window that ath12k's pci.c moves for offsets past 512 KiB;
 *   - interrupts: none. A thread looks at the copy engines every millisecond, and at the data path's
 *     ring groups once the driver has enabled them.
 *
 * The rest -- what to do with the radio once it is registered: scan, join, carry frames -- is the
 * station of src/lx_80211.c, as for ath11k (src/lx_bus.c, which this follows).
 */
#define LX_QMI_AS_IT_IS				/* (this file is where the driver's requests go past) */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/skbuff.h>

#include "core.h"
#include "ce.h"
#include "debug.h"
#include "dp.h"
#include "dp_peer.h"
#include "hal.h"
#include "hif.h"
#include "pci.h"
#include "qmi.h"
#include "wifi7/core.h"
#include "wifi7/hal_desc.h"
#include "wifi7/hw.h"

#include <lx_os.h>
#include "lx.h"

#define TCSR_SOC_HW_VERSION	0x1b00000
#define WCN7850_WINDOW_REG	0x310c		/* wifi7/pci.c: WINDOW_REG_ADDRESS */
#define WINDOW_ENABLE_BIT	0x40000000	/* the window register (pci.c) */
#define WINDOW_VALUE_MASK	GENMASK(24, 19)
#define WINDOW_START		0x80000
#define WINDOW_RANGE_MASK	GENMASK(18, 0)
#define WCN7850_VENDOR		0x17cb
#define WCN7850_DEVICE		0x1107
#define DATA_BUDGET		64

static struct {
	u8 __iomem *mem;			/* BAR0 */
	u32 window;				/* what the window register was last set to; ~0: not known */
	spinlock_t window_lock;
	unsigned reads, writes;
	volatile bool looking;			/* the thread that looks at the rings is wanted ... */
	volatile bool looks;			/* ... and runs */
	volatile bool data;			/* ... and looks at the data path's groups as well */
	volatile bool in_pass;			/* ... and is in the middle of a look */
} bus;

/* ---- what the driver says (debug.c is not built) ------------------------------------------------- */

static void
driver_says(const char *kind, const char *format, va_list list, bool always)
{
	char line[320];
	size_t length;

	unsigned long long ms = lx_os_nanoseconds() / 1000000ull;

	vsnprintf(line, sizeof(line), format, list);
	length = strlen(line);
	/* (at the kernel's uptime, as the kernel's MHI lines have it; Linux ends a line for whoever has not) */
	if (always) {
		lx_tell("ath12k%s at %llu ms: %s%s", kind, ms, line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
	} else {
		printf("ath12k%s at %llu ms: %s%s", kind, ms, line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
	}
}

void
ath12k_info(struct ath12k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says("", format, list, false);
	va_end(list);
}

void
ath12k_err(struct ath12k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says(" ERROR", format, list, true);
	va_end(list);
}

void
__ath12k_warn(struct device *dev, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says(" WARNS", format, list, true);
	va_end(list);
}

void
__ath12k_dbg(struct ath12k_base *ab, enum ath12k_debug_mask mask, const char *format, ...)
{
	va_list list;

	if (!(ath12k_debug_mask & mask)) {
		return;
	}
	va_start(list, format);
	driver_says(" (debug)", format, list, false);
	va_end(list);
}

void
ath12k_dbg_dump(struct ath12k_base *ab, enum ath12k_debug_mask mask, const char *msg, const char *prefix, const void *buf,
    size_t len)
{
}

/* ---- the chip's registers ---------------------------------------------------------------------- */

/*
 * As pci.c reaches them for a chip without static windows: the first 512 KiB of BAR0 are the chip's
 * first 512 KiB; the second show a 512 KiB window anywhere in the chip, chosen by bits 19..24 of the
 * offset written to the window register. MHI's registers, which the chip also has in a window of
 * its own (PCI_MHIREGLEN_REG..PCI_MHI_REGION_END), are reached at their place in the first 512 KiB.
 */
static u8 __iomem *
reach(u32 offset)
{
	u32 window;

	if (offset < WINDOW_START) {
		return (bus.mem + offset);
	}
	if (offset >= PCI_MHIREGLEN_REG && offset <= PCI_MHI_REGION_END) {
		return (bus.mem + ((offset - PCI_MHIREGLEN_REG) & WINDOW_RANGE_MASK));
	}
	window = u32_get_bits(offset, WINDOW_VALUE_MASK);
	if (window != bus.window) {
		iowrite32(WINDOW_ENABLE_BIT | window, bus.mem + WCN7850_WINDOW_REG);
		(void)ioread32(bus.mem + WCN7850_WINDOW_REG);
		bus.window = window;
	}
	return (bus.mem + WINDOW_START + (offset & WINDOW_RANGE_MASK));
}

static u32
bus_read32(struct ath12k_base *ab, u32 offset)
{
	u32 value;

	spin_lock_bh(&bus.window_lock);
	value = ioread32(reach(offset));
	spin_unlock_bh(&bus.window_lock);
	bus.reads++;
	return (value);
}

static void
bus_write32(struct ath12k_base *ab, u32 offset, u32 value)
{
	spin_lock_bh(&bus.window_lock);
	__asm __volatile("dsb sy");		/* (what was written to a ring is in memory before the chip is told of it) */
	iowrite32(value, reach(offset));
	spin_unlock_bh(&bus.window_lock);
	bus.writes++;
}

/* ---- in place of the interrupts ---------------------------------------------------------------- */

/*
 * Each copy engine is looked at as its interrupt would have ath12k do (ce.c:
 * ath12k_ce_per_engine_service(): what has been received goes to its owner, what has been sent is
 * given back), and once the driver has enabled the data path's "interrupts" so is each of the ring
 * groups (dp.h: ath12k_dp_service_srng(), what pci.c's NAPI poll calls). Once a millisecond while
 * nothing comes; again at once while a group has used up what it was allowed in one go.
 */
/*
 * The WMI endpoint's receive callback, wrapped once the endpoint is there: some events are said in hex before
 * ath12k gets them. Those of the STATS group, which ath12k does not handle (0x16005, which the firmware sends
 * right after WMI_INIT); the answer to ath12k's hardware mode request and the firmware's own change of mode
 * (seen at the first vdev start at 5 GHz), neither of which ath12k reads: old and new mode, and which MAC each
 * vdev is on; and each vdev start response, which names the MAC the vdev was started on. ath12k_wmi_op_rx() is
 * static; its address comes from the endpoint.
 */
static void (*wmi_rx)(struct ath12k_base *, struct sk_buff *);

static void
wmi_rx_said(struct ath12k_base *ab, struct sk_buff *skb)
{
	u32 id = skb->len >= sizeof(struct wmi_cmd_hdr) ?
	    le32_get_bits(((struct wmi_cmd_hdr *)skb->data)->cmd_id, WMI_CMD_HDR_CMD_ID) : 0;

	if ((id >> 12) == WMI_GRP_STATS || id == WMI_PDEV_SET_HW_MODE_RESP_EVENTID || id == WMI_PDEV_HW_MODE_TRANSITION_EVENTID ||
	    id == WMI_VDEV_START_RESP_EVENTID) {
		printf("lx: WMI event %#x, %u bytes:\n", id, skb->len);
		for (unsigned at = 0; at < skb->len && at < 1024; at += 32) {
			char line[3 * 32 + 1];
			unsigned n = 0;

			for (unsigned i = at; i < skb->len && i < at + 32; i++) {
				n += snprintf(line + n, sizeof(line) - n, " %02x", skb->data[i]);
			}
			printf("lx:   %04x%s\n", at, line);
		}
	}
	wmi_rx(ab, skb);
}

static void
wmi_rx_wrap(struct ath12k_base *ab)
{
	struct ath12k_htc_ep *ep;

	if (wmi_rx != NULL || ab->wmi_ab.wmi_endpoint_id[0] == ATH12K_HTC_EP_0 ||
	    ab->wmi_ab.wmi_endpoint_id[0] >= ATH12K_HTC_EP_COUNT) {
		return;
	}
	ep = &ab->htc.endpoint[ab->wmi_ab.wmi_endpoint_id[0]];
	if (ep->ep_ops.ep_rx_complete != NULL && ep->ep_ops.ep_rx_complete != wmi_rx_said) {
		wmi_rx = ep->ep_ops.ep_rx_complete;
		ep->ep_ops.ep_rx_complete = wmi_rx_said;
	}
}

/*
 * What becomes of the frames given to the data path, as the chip has them, read before ath12k takes the
 * completions: each descriptor put on a TCL ring, with how far the chip has read that ring and the peers the
 * firmware has mapped (the descriptor names its peer by AST index and hash); and each completion, with the block
 * that gave the frame back (TQM, or the firmware), its reason (enum hal_wbm_tqm_rel_reason, or for the firmware
 * enum hal_wbm_htt_tx_comp_status), how many times it was sent, and for which peer and traffic class. It is for
 * the first join after a start, whose message 2 of the handshake can stay queued until the vdev is stopped: this
 * says where it waits. The first TX_SAID_MOST lines only.
 */
#define TX_SAID_MOST	64

static struct {
	u32 queued[DP_TCL_NUM_RING_MAX];	/* each TCL ring's head as last looked at, in words */
	u32 done[DP_TCL_NUM_RING_MAX];		/* each completion ring's, likewise */
	unsigned said;
} watch;

static void
say_peers(struct ath12k_dp *dp)
{
	struct ath12k_dp_link_peer *peer;
	char line[240] = "";

	spin_lock_bh(&dp->dp_lock);
	list_for_each_entry(peer, &dp->peers, list) {
		snprintf(line + strlen(line), sizeof(line) - strlen(line), " %d (AST index %u, hash %u, vdev %d%s)", peer->peer_id,
		    peer->hw_peer_id, peer->ast_hash, peer->vdev_id, peer->sta != NULL ? ", a station" : "");
	}
	spin_unlock_bh(&dp->dp_lock);
	printf("lx:   the peers mapped:%s\n", line[0] != 0 ? line : " none");
	watch.said++;
}

static void
tx_watch(struct ath12k_base *ab)
{
	struct ath12k_dp *dp = ath12k_ab_to_dp(ab);
	unsigned long long ms = lx_os_nanoseconds() / 1000000ull;

	for (unsigned i = 0; i < dp->hw_params->max_tx_ring && i < DP_TCL_NUM_RING_MAX; i++) {
		struct hal_srng *tcl = &dp->hal->srng_list[dp->tx_ring[i].tcl_data_ring.ring_id];
		struct hal_srng *done = &dp->hal->srng_list[dp->tx_ring[i].tcl_comp_ring.ring_id];
		bool queued = false;
		u32 head, at;

		if (!tcl->initialized || !done->initialized) {
			continue;
		}
		for (;;) {
			struct hal_tcl_data_cmd d;
			u32 read;

			spin_lock_bh(&tcl->lock);
			head = tcl->u.src_ring.hp;
			read = READ_ONCE(*tcl->u.src_ring.tp_addr);
			if ((at = watch.queued[i]) != head) {
				memcpy(&d, tcl->ring_base_vaddr + at, sizeof(d));
				watch.queued[i] = (at + tcl->entry_size) % tcl->ring_size;
			}
			spin_unlock_bh(&tcl->lock);
			if (at == head || watch.said >= TX_SAID_MOST) {
				break;
			}
			printf("lx: tx at %llu ms: ring %u, entry %u queued (the chip has read to %u): bank %u, metadata %#x, %u bytes%s%s, "
			    "vdev %u, pmac %u, tid %u, AST index %u, hash %u\n", ms, i, at / tcl->entry_size, read / tcl->entry_size,
			    (unsigned)le32_get_bits(d.info0, HAL_TCL_DATA_CMD_INFO0_BANK_ID),
			    (unsigned)le32_get_bits(d.info1, HAL_TCL_DATA_CMD_INFO1_CMD_NUM),
			    (unsigned)le32_get_bits(d.info2, HAL_TCL_DATA_CMD_INFO2_DATA_LEN),
			    le32_get_bits(d.info0, HAL_TCL_DATA_CMD_INFO0_DESC_TYPE) ? " behind an extension descriptor" : "",
			    le32_get_bits(d.info2, HAL_TCL_DATA_CMD_INFO2_TO_FW) ? ", to the firmware" : "",
			    (unsigned)le32_get_bits(d.info3, HAL_TCL_DATA_CMD_INFO3_VDEV_ID),
			    (unsigned)le32_get_bits(d.info3, HAL_TCL_DATA_CMD_INFO3_PMAC_ID),
			    (unsigned)le32_get_bits(d.info3, HAL_TCL_DATA_CMD_INFO3_TID),
			    (unsigned)le32_get_bits(d.info4, HAL_TCL_DATA_CMD_INFO4_SEARCH_INDEX),
			    (unsigned)le32_get_bits(d.info4, HAL_TCL_DATA_CMD_INFO4_CACHE_SET_NUM));
			watch.said++;
			queued = true;
		}
		if (queued && watch.said < TX_SAID_MOST) {
			say_peers(dp);
		}

		/* (the chip writes a completion before it moves the head; ath12k takes them later in this same pass) */
		head = READ_ONCE(*done->u.dst_ring.hp_addr);
		dma_rmb();
		for (at = watch.done[i]; at != head && watch.said < TX_SAID_MOST; at = (at + done->entry_size) % done->ring_size) {
			const struct hal_wbm_completion_ring_tx *c = (const void *)(done->ring_base_vaddr + at);
			u32 source = le32_get_bits(c->info0, HAL_WBM_COMPL_TX_INFO0_REL_SRC_MODULE);

			printf("lx: tx at %llu ms: ring %u, a completion: given back by %s, reason %u, sent %u times, peer %u, tid %u "
			    "(the chip has read the ring to %u of %u)\n", ms, i, source == HAL_WBM_REL_SRC_MODULE_TQM ? "TQM" :
			    source == HAL_WBM_REL_SRC_MODULE_FW ? "the firmware" : "another block",
			    (unsigned)le32_get_bits(c->info0, HAL_WBM_COMPL_TX_INFO0_TQM_RELEASE_REASON),
			    (unsigned)le32_get_bits(c->info1, HAL_WBM_COMPL_TX_INFO1_TRANSMIT_COUNT),
			    (unsigned)le32_get_bits(c->info3, HAL_WBM_COMPL_TX_INFO3_PEER_ID),
			    (unsigned)le32_get_bits(c->info3, HAL_WBM_COMPL_TX_INFO3_TID),
			    READ_ONCE(*tcl->u.src_ring.tp_addr) / tcl->entry_size, tcl->u.src_ring.hp / tcl->entry_size);
			watch.said++;
		}
		watch.done[i] = head;
	}
}

static void
bus_looks(void *argument)
{
	struct ath12k_base *ab = argument;
	unsigned long long said = lx_os_nanoseconds();

	bus.looks = true;
	while (bus.looking) {
		bool more = false;

		if (lx_os_nanoseconds() - said > 300000000000ull) {	/* every five minutes: how the station and memory stand */
			said = lx_os_nanoseconds();
			lx_station_say();
			lx_memory_say();
		}
		bus.in_pass = true;
		wmi_rx_wrap(ab);
		for (unsigned i = 0; i < ab->hw_params->ce_count; i++) {
			ath12k_ce_per_engine_service(ab, i);
		}
		if (bus.data && watch.said < TX_SAID_MOST) {
			tx_watch(ab);
		}
		for (unsigned i = 0; bus.data && i < ATH12K_EXT_IRQ_GRP_NUM_MAX; i++) {
			more |= ath12k_dp_service_srng(ath12k_ab_to_dp(ab), &ab->ext_irq_grp[i], DATA_BUDGET) >= DATA_BUDGET;
		}
		bus.in_pass = false;
		if (!more) {
			lx_pause_ms(1);
		}
	}
	bus.looks = false;
}

/* the data path's groups are looked at no more; returns when the thread is not in the middle of a look */
static void
bus_data_off(void)
{
	bus.data = false;
	while (bus.looks && bus.in_pass) {
		lx_pause_ms(1);
	}
}

/* ... nor the copy engines */
static void
bus_quiet(void)
{
	bus.data = false;
	bus.looking = false;
	while (bus.looks) {
		lx_pause_ms(1);
	}
}

/*
 * A request of the driver's to the firmware, on its way out (linux/soc/qcom/qmi.h, ours): the mode
 * asked for is said, it being the line between the exchange and the radio's use.
 */
int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	if (msg_id == QMI_WLANFW_WLAN_MODE_REQ_V01) {
		lx_tell("lx: the firmware is asked for mode %u (0: mission, 4: off, 7: calibration)\n",
		    ((const struct qmi_wlanfw_wlan_mode_req_msg_v01 *)structure)->mode);
	}
	return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure));
}

/* ---- the bus's operations ------------------------------------------------------------------------ */

/* pci.c: ath12k_pci_start(), with the thread above where it enables the interrupts */
static int
bus_start(struct ath12k_base *ab)
{
	printf("lx: the driver starts the bus: buffers for the copy engines that receive (%u reads and %u writes of the chip's "
	    "registers so far)\n", bus.reads, bus.writes);
	ath12k_ce_rx_post_buf(ab);
	bus.looking = true;
	if (lx_os_thread("wifi-rings", bus_looks, ab) != 0) {
		bus.looking = false;
		return (-ENOMEM);
	}
	lx_tell("lx: the rings are set up and the copy engines have their buffers: the bus is started\n");
	return (0);
}

/* pci.c: ath12k_pci_stop() */
static void
bus_stop(struct ath12k_base *ab)
{
	bus_quiet();
	ath12k_ce_cleanup_pipes(ab);
	printf("lx: the driver stops the bus (%u reads and %u writes of the chip's registers in all)\n", bus.reads, bus.writes);
}

/* pci.c: ath12k_pci_map_service_to_pipe(): which copy engines carry a service, by the chip's table */
static int
bus_map_service_to_pipe(struct ath12k_base *ab, u16 service, u8 *up, u8 *down)
{
	bool has_up = false, has_down = false;

	for (u32 i = 0; i < ab->hw_params->svc_to_ce_map_len; i++) {
		const struct service_to_pipe *entry = &ab->hw_params->svc_to_ce_map[i];
		u32 direction = __le32_to_cpu(entry->pipedir);

		if (__le32_to_cpu(entry->service_id) != service) {
			continue;
		}
		if (direction == PIPEDIR_IN || direction == PIPEDIR_INOUT) {
			*down = (u8)__le32_to_cpu(entry->pipenum);
			has_down = true;
		}
		if (direction == PIPEDIR_OUT || direction == PIPEDIR_INOUT) {
			*up = (u8)__le32_to_cpu(entry->pipenum);
			has_up = true;
		}
	}
	return (has_up && has_down ? 0 : -ENOENT);
}

/* pci.c: ath12k_pci_power_up(): the card's reset and the firmware's download are the kernel's (steps 7 and 8) */
static int
bus_power_up(struct ath12k_base *ab)
{
	int error = lx_os_rproc_boot();

	spin_lock_bh(&bus.window_lock);
	bus.window = ~0u;			/* (the kernel's steps moved it) */
	spin_unlock_bh(&bus.window_lock);
	return (-error);
}

static void
bus_power_down(struct ath12k_base *ab, bool is_suspend)
{
	lx_tell("lx: the driver powers the card down: it is left running (the kernel does not stop it yet)\n");
}

/* pci.c: ath12k_pci_ext_irq_enable(): the data path's rings are set up, and are looked at from here on */
static void
bus_irq_enable(struct ath12k_base *ab)
{
	for (unsigned i = 0; i < ATH12K_EXT_IRQ_GRP_NUM_MAX; i++) {
		ab->ext_irq_grp[i].ab = ab;
		ab->ext_irq_grp[i].grp_id = i;
	}
	set_bit(ATH12K_FLAG_EXT_IRQ_ENABLED, &ab->dev_flags);
	bus.data = true;
}

static void
bus_irq_disable(struct ath12k_base *ab)
{
	if (test_and_clear_bit(ATH12K_FLAG_EXT_IRQ_ENABLED, &ab->dev_flags)) {
		bus_data_off();
	}
}

/* (no interrupt vectors are offered: ce.c and dp.c then set the rings up without any) */
static const struct ath12k_hif_ops bus_ops = {
	.read32 = bus_read32,
	.write32 = bus_write32,
	.irq_enable = bus_irq_enable,
	.irq_disable = bus_irq_disable,
	.start = bus_start,
	.stop = bus_stop,
	.map_service_to_pipe = bus_map_service_to_pipe,
	.power_up = bus_power_up,
	.power_down = bus_power_down,
};

/* ---- what the driver asks of a Linux kernel and its device tree, of which there is none --------------- */

struct device_node *
of_parse_phandle(const struct device_node *np, const char *name, int index)
{
	return (NULL);
}

const struct property *
of_find_property(const struct device_node *np, const char *name, int *lenp)
{
	return (NULL);
}

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	return (-EINVAL);
}

/* the machine's memory, which core.c weighs for the QCN9274 only: more than enough */
void
si_meminfo(struct sysinfo *info)
{
	memset(info, 0, sizeof(*info));
	info->mem_unit = PAGE_SIZE;
	info->totalram = (1ul << 30) / PAGE_SIZE;
}

/* ---- the entries ---------------------------------------------------------------------------------- */

static struct ath12k_base *volatile driver;	/* the driver, while it is up: between lx_start() and lx_stop() */

/* After the driver is registered: what the firmware said of the radio. */
static void
say_radio(struct ath12k_base *ab)
{
	const struct ath12k_wmi_target_cap_arg *t = &ab->target_caps;

	printf("lx: THE FIRMWARE SAYS OF THE RADIO: %u radio(s); %u chains (mask %#x); up to %u channels in a scan; phy %#x, "
	    "HT %#x, VHT %#x, system %#x; its address %02x:%02x:%02x:%02x:%02x:%02x\n", ab->num_radios, t->num_rf_chains,
	    t->txrx_chainmask, t->max_num_scan_channels, t->phy_capability, t->ht_cap_info, t->vht_cap_info, t->sys_cap_info,
	    ab->mac_addr[0], ab->mac_addr[1], ab->mac_addr[2], ab->mac_addr[3], ab->mac_addr[4], ab->mac_addr[5]);
}

/* pci.c: ath12k_pci_probe() and wifi7/pci.c: ath12k_wifi7_pci_probe(), for this chip */
static int
probe(struct ath12k_base **out)
{
	static struct device device;
	unsigned long long physical;
	unsigned long size;
	struct ath12k_base *ab;
	u32 version;
	int ret;

	if ((ret = lx_os_radio_registers(&physical, &size)) != 0) {
		lx_tell("lx: the kernel has no card on PCIe for the driver (%d): pcie=10 brings it up\n", ret);
		return (ret);
	}
	spin_lock_init(&bus.window_lock);
	bus.window = ~0u;
	bus.mem = lx_os_map_registers(physical, size);
	device.kobj.name = "wifi";
	ab = ath12k_core_alloc(&device, 0, ATH12K_BUS_PCI);
	if (bus.mem == NULL || ab == NULL) {
		return (ENOMEM);
	}
	ab->dev = &device;
	ab->mem = bus.mem;			/* (hal.c reckons the rings' register offsets from it) */
	ab->mem_len = size;
	ab->hif.ops = &bus_ops;
	ab->fw_mode = ATH12K_FIRMWARE_MODE_NORMAL;
	ab->id.vendor = WCN7850_VENDOR;
	ab->id.device = WCN7850_DEVICE;
	ab->id.bdf_search = ATH12K_BDF_SEARCH_BUS_AND_BOARD;
	ab->static_window_map = false;
	ab->target_mem_mode = ATH12K_QMI_MEMORY_MODE_DEFAULT;
	version = bus_read32(ab, TCSR_SOC_HW_VERSION);
	printf("lx: BAR0 at %#llx (%lu KiB); the chip's hardware version %#x\n", physical, size >> 10, version);
	if (u32_get_bits(version, GENMASK(11, 8)) != 2) {
		lx_tell("lx: the WCN7850's hardware version is %#x: ath12k knows 2.0 only\n", version);
		return (ENODEV);
	}
	ab->hw_rev = ATH12K_HW_WCN7850_HW20;
	if ((ret = ath12k_wifi7_hw_init(ab)) != 0 || (ret = ath12k_core_pre_init(ab)) != 0 ||
	    (ret = ath12k_hal_srng_init(ab)) != 0 || (ret = ath12k_ce_alloc_pipes(ab)) != 0) {
		lx_tell("lx: the driver does not get as far as its exchange with the firmware: %d\n", ret);
		return (-ret);
	}
	if (lx_os_option("wifi_hostcap") == 1) {
		/*
		 * The host capability as Android's cnss2 sends it on this phone: feature list 0x6 and no nm_modem
		 * (its device tree has no qcom,sleep-clk-support). ath12k sends 0x18 and nm_modem 0xe (internal
		 * sleep clock, host C-states, PCIe global reset), both from hw_params.
		 */
		static struct ath12k_hw_params cnss2;

		cnss2 = *ab->hw_params;
		cnss2.internal_sleep_clock = false;
		cnss2.qmi_cnss_feature_bitmap = BIT(1) | BIT(2);
		ab->hw_params = &cnss2;
		printf("lx: wifi_hostcap=1: feature list %#llx, no nm_modem\n",
		    (unsigned long long)cnss2.qmi_cnss_feature_bitmap);
	}
	if (lx_os_option("wifi_mlo") == 1) {
		/*
		 * Single-chip MLO, as cnss2 runs this chip, so that the host capability carries the MLO fields
		 * (max_mlo_peer 32, one chip, two links): WLAN.HMT.2.0-02977 sizes its location service's pools by
		 * max_mlo_peer and fails with fatal 386 when it is absent (bsd/dev/mhi_pcie_wlfw.c, pcie_mlo). ath12k
		 * takes the bit from the PHY capability, which this firmware reports as 0; the reply only ever sets it.
		 */
		ab->single_chip_mlo_support = true;
		printf("lx: wifi_mlo=1: single-chip MLO, max_mlo_peer %u\n", ab->hw_params->max_mlo_peer);
	}
	/* pci.c: ath12k_pci_init_qmi_ce_config() */
	ab->qmi.ce_cfg.tgt_ce = ab->hw_params->target_ce_config;
	ab->qmi.ce_cfg.tgt_ce_len = ab->hw_params->target_ce_count;
	ab->qmi.ce_cfg.svc_to_ce_map = ab->hw_params->svc_to_ce_map;
	ab->qmi.ce_cfg.svc_to_ce_map_len = ab->hw_params->svc_to_ce_map_len;
	ab->qmi.service_ins_id = ab->hw_params->qmi_service_ins_id;
	if ((ret = ath12k_wifi7_arch_init(ab)) != 0) {
		lx_tell("lx: the driver's data path cannot be set up: %d\n", ret);
		return (-ret);
	}
	printf("lx: the driver (\"%s\") is set up; its exchange with the firmware begins\n", ab->hw_params->name);
	*out = ab;
	return (0);
}

int
lx_start(void)
{
	struct ath12k_base *ab;
	unsigned long long began;
	bool up;
	int ret;

	if (lx_prepare() != 0) {
		lx_tell("lx: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	ath12k_debug_mask = lx_os_option("wifi_quiet") ? 0 : ATH12K_DBG_QMI | ATH12K_DBG_BOOT | ATH12K_DBG_PCI |
	    ATH12K_DBG_WMI | ATH12K_DBG_REG | ATH12K_DBG_MAC;
	if ((ret = probe(&ab)) != 0) {
		return (ret);
	}
	if ((ret = ath12k_core_init(ab)) != 0) {
		lx_tell("lx: the driver's start fails: %d\n", ret);
		return (-ret);
	}

	/*
	 * The rest happens on the driver's own threads, until core.c has registered the radio (the flag
	 * below) or the exchange with the firmware has failed. It went well if the bus is still up.
	 */
	began = lx_os_nanoseconds();
	while (!test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) && !test_bit(ATH12K_FLAG_QMI_FAIL, &ab->dev_flags) &&
	    lx_os_nanoseconds() - began < 60000000000ull) {
		lx_pause_ms(10);
	}
	up = test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) && bus.looks;
	lx_tell("lx: %llu ms after its start the driver %s; its flags: %#lx\n", (lx_os_nanoseconds() - began) / 1000000ull,
	    up ? "IS UP: the radio is registered" : test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) ?
	    "HAS GIVEN UP on the way and wound itself down" : "HAS NOT GOT THROUGH its exchange with the firmware", ab->dev_flags);
	if (!up) {
		return (test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) ? ETIMEDOUT : EIO);
	}
	lx_pause_ms(1000);			/* (for what the firmware says unasked once it is ready) */
	say_radio(ab);

	driver = ab;
	if (lx_os_option("wifi_station")) {
		/* whoever wants a network asks the kernel, and the kernel asks the station: the driver stays up until lx_stop() */
		if ((ret = lx_station_offer()) == 0) {
			return (0);
		}
	} else if (lx_os_option("wifi_join")) {
		if ((ret = lx_station_join()) == 0) {
			return (0);		/* the network is joined, and the driver stays up until lx_stop() */
		}
	} else {
		ret = lx_os_option("wifi_scan") ? lx_station_scan() : 0;
		if (ret == 0) {
			return (0);		/* (the driver stays up, registered, for whoever looks next) */
		}
	}
	lx_stop();
	return (ret);
}

/* The thread that counts the ticks, and the memory the radio is shown: once. 0, or an errno. */
int
lx_prepare(void)
{
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

/* The network left, and the driver down as its own removal does it. */
void
lx_stop(void)
{
	struct ath12k_base *ab = driver;

	if (ab == NULL) {
		return;
	}
	driver = NULL;
	lx_station_withdraw();
	lx_station_leave();
	ath12k_core_deinit(ab);
	lx_tell("lx: the driver is down again\n");
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (driver != NULL ? lx_station_send(frame, length) : ENETDOWN);
}

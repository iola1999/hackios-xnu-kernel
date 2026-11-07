/*
 * SM8750 (annibale) IPA v5.5 cellular data path and iOS pdp_ip0 network interface.
 *
 * Registers the pdp_ip0 cellular interface (IFT_CELLULAR), handles QMAP (Qualcomm
 * Multiplexing and Aggregation Protocol) framing/deframing for mux_id 1..16, Synthesizes
 * local L2 resolution so the BSD IPv4/IPv6 stack routes over pdp_ip0 without custom
 * DLIL protocol plumbers, and optionally loads /fw/ipa_fws.mdt (PAS ID 15) into
 * ipa_fw_region (0x9b080000).
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/redwood_input.h>
#include <sys/sockio.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

#ifndef IFT_CELLULAR
#define IFT_CELLULAR            0xff
#endif

/* SM8750 IPA v5.5 hardware & reserved memory regions from annibale.dtb */
#define IPA_WRAPPER_PHYS        0x01e00000ULL
#define IPA_WRAPPER_SIZE        0x00100000ULL
#define IPA_FW_MEM_PHYS         0x9b080000ULL
#define IPA_FW_MEM_SIZE         0x00010000ULL
#define IPA_GSI_MEM_PHYS        0x9b090000ULL
#define IPA_GSI_MEM_SIZE        0x0000a000ULL
#define IPA_PAS_ID              15u

#define IPA_DEFAULT_MTU         1500u
#define IPA_FRAME_MAX           2048u
#define QMAP_HDR_LEN            4u
#define QMAP_DEFAULT_MUX_ID     1u

struct qmap_hdr {
	uint8_t pad_cd;         /* bit 7: control/data (0 = data), bits 5..0: pad bytes */
	uint8_t mux_id;         /* PDN mux identifier (default 1) */
	uint16_t pkt_len_be;    /* payload length including padding, big-endian */
} __attribute__((packed));

static LCK_GRP_DECLARE(ipa_lck_grp, "annibale-ipa");
static LCK_MTX_DECLARE(ipa_lock, &ipa_lck_grp);

static ifnet_t ipa_ifp;
static bool ipa_inited;
static bool ipa_link_up;
static uint8_t ipa_mux_id = QMAP_DEFAULT_MUX_ID;
static const uint8_t ipa_lladdr[ETHER_ADDR_LEN] = { 0x02, 0x50, 0x44, 0x50, 0x00, 0x00 };
static const uint8_t ipa_gw_mac[ETHER_ADDR_LEN] = { 0x02, 0x50, 0x44, 0x50, 0x00, 0x01 };

static struct {
	uint64_t tx_pkts, tx_bytes, rx_pkts, rx_bytes, tx_drops, rx_drops;
	uint32_t ipv4_addr, ipv4_gw, ipv4_mask, ipv4_dns1, ipv4_dns2, mtu;
} ipa_state = { .mtu = IPA_DEFAULT_MTU };

static int (*ipa_modem_tx_fn)(const uint8_t *qmap_frame, size_t len);

SYSCTL_DECL(_kern_redwood);
static char sysctl_cellular_ipv4[32] = "0.0.0.0";
static char sysctl_cellular_gw[32] = "0.0.0.0";
static char sysctl_cellular_dns[64] = "0.0.0.0";
SYSCTL_STRING(_kern_redwood, OID_AUTO, cellular_ipv4, CTLFLAG_RD | CTLFLAG_LOCKED,
    sysctl_cellular_ipv4, sizeof(sysctl_cellular_ipv4), "Cellular pdp_ip0 IPv4 address");
SYSCTL_STRING(_kern_redwood, OID_AUTO, cellular_gw, CTLFLAG_RD | CTLFLAG_LOCKED,
    sysctl_cellular_gw, sizeof(sysctl_cellular_gw), "Cellular pdp_ip0 IPv4 gateway");
SYSCTL_STRING(_kern_redwood, OID_AUTO, cellular_dns, CTLFLAG_RD | CTLFLAG_LOCKED,
    sysctl_cellular_dns, sizeof(sysctl_cellular_dns), "Cellular pdp_ip0 IPv4 DNS servers");

static void
ipa_format_ip(uint32_t ip, char *buf, size_t cap)
{
	snprintf(buf, cap, "%u.%u.%u.%u",
	    (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

static void
ipa_link_event(ifnet_t ifp, uint32_t code)
{
	_Alignas(struct kern_event_msg) char msg[sizeof(struct kern_event_msg) + sizeof(struct net_event_data)] = { 0 };
	struct kern_event_msg *hdr = (struct kern_event_msg *)msg;
	struct net_event_data *ev = (struct net_event_data *)(hdr + 1);

	if (ifp == NULL) {
		return;
	}
	hdr->total_size = sizeof(msg);
	hdr->vendor_code = KEV_VENDOR_APPLE;
	hdr->kev_class = KEV_NETWORK_CLASS;
	hdr->kev_subclass = KEV_DL_SUBCLASS;
	hdr->event_code = code;
	ev->if_family = ifnet_family(ifp);
	ev->if_unit = ifnet_unit(ifp);
	strlcpy(ev->if_name, ifnet_name(ifp), IFNAMSIZ);
	ifnet_event(ifp, hdr);
}

/* Inject an Ethernet-framed packet into pdp_ip0's BSD receive path. */
static void
ipa_inject_ether_rx(const uint8_t *frame, size_t length)
{
	struct ifnet_stat_increment_param stats = { .packets_in = 1, .bytes_in = (uint32_t)length };
	unsigned int chunks = 1;
	mbuf_t packet = NULL;
	char *header;

	if (ipa_ifp == NULL || length < ETHER_HDR_LEN || length > IPA_FRAME_MAX ||
	    mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet) != 0) {
		ipa_state.rx_drops++;
		return;
	}
	if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
		mbuf_freem(packet);
		ipa_state.rx_drops++;
		return;
	}
	header = mbuf_data(packet);
	mbuf_pkthdr_setrcvif(packet, ipa_ifp);
	mbuf_pkthdr_setheader(packet, header);
	mbuf_pkthdr_adjustlen(packet, -ETHER_HDR_LEN);
	mbuf_setdata(packet, header + ETHER_HDR_LEN, length - ETHER_HDR_LEN);
	bpf_tap_in(ipa_ifp, DLT_EN10MB, packet, header, ETHER_HDR_LEN);
	ipa_state.rx_pkts++;
	ipa_state.rx_bytes += length - ETHER_HDR_LEN;
	ifnet_input(ipa_ifp, packet, &stats);
}

/* Synthesize an ARP reply for the point-to-point cellular gateway so IPv4 routes over Ethernet framing. */
static void
ipa_handle_arp(const uint8_t *frame, size_t len)
{
	uint8_t reply[42];

	if (len < 42 || frame[14] != 0x00 || frame[15] != 0x01 ||
	    frame[16] != 0x08 || frame[17] != 0x00 ||
	    frame[20] != 0x00 || frame[21] != 0x01) {
		return;
	}
	memcpy(reply, frame + 6, 6);
	memcpy(reply + 6, ipa_gw_mac, 6);
	reply[12] = 0x08;
	reply[13] = 0x06;
	memcpy(reply + 14, frame + 14, 6);
	reply[20] = 0x00;
	reply[21] = 0x02; /* ARP reply */
	memcpy(reply + 22, ipa_gw_mac, 6);
	memcpy(reply + 28, frame + 38, 4); /* target IP becomes sender IP */
	memcpy(reply + 32, frame + 22, 10);
	ipa_inject_ether_rx(reply, sizeof(reply));
}

/* Receive a raw IPv4/IPv6 packet from MPSS/IPA and deliver it to pdp_ip0. */
void
annibale_ipa_rx_ip(const uint8_t *ip_pkt, size_t ip_len)
{
	uint8_t *frame;
	uint8_t ver;
	uint16_t ethertype;

	if (ip_pkt == NULL || ip_len < 20 || ip_len + ETHER_HDR_LEN > IPA_FRAME_MAX) {
		ipa_state.rx_drops++;
		return;
	}
	ver = ip_pkt[0] >> 4;
	if (ver == 4) {
		ethertype = ETHERTYPE_IP;
	} else if (ver == 6) {
		ethertype = ETHERTYPE_IPV6;
	} else {
		ipa_state.rx_drops++;
		return;
	}
	frame = kalloc_data(ETHER_HDR_LEN + ip_len, Z_NOWAIT);
	if (frame == NULL) {
		ipa_state.rx_drops++;
		return;
	}
	memcpy(frame, ipa_lladdr, ETHER_ADDR_LEN);
	memcpy(frame + ETHER_ADDR_LEN, ipa_gw_mac, ETHER_ADDR_LEN);
	frame[12] = (uint8_t)(ethertype >> 8);
	frame[13] = (uint8_t)(ethertype & 0xff);
	memcpy(frame + ETHER_HDR_LEN, ip_pkt, ip_len);
	ipa_inject_ether_rx(frame, ETHER_HDR_LEN + ip_len);
	kfree_data(frame, ETHER_HDR_LEN + ip_len);
}

/* Decapsulate one or more aggregated QMAP frames from MPSS/IPA and pass each IP packet to pdp_ip0. */
void
annibale_ipa_rx_qmap(const uint8_t *data, size_t len)
{
	size_t off = 0;

	while (off + QMAP_HDR_LEN <= len) {
		const struct qmap_hdr *qh = (const struct qmap_hdr *)(data + off);
		uint16_t pkt_len = (uint16_t)(qh->pkt_len_be >> 8 | qh->pkt_len_be << 8);
		uint8_t pad = qh->pad_cd & 0x3f;

		off += QMAP_HDR_LEN;
		if (pkt_len == 0 || off + pkt_len > len) {
			break;
		}
		if ((qh->pad_cd & 0x80) == 0 && pkt_len > pad) {
			annibale_ipa_rx_ip(data + off, pkt_len - pad);
		}
		off += pkt_len;
	}
}

/* Encapsulate an outgoing IPv4/IPv6 packet into QMAP and transmit via MPSS/IPA. */
static errno_t
ipa_output(ifnet_t ifp, mbuf_t packet)
{
	size_t len = mbuf_pkthdr_len(packet);
	uint8_t *buf = NULL;
	const uint8_t *ip_ptr;
	size_t ip_len, pad, qmap_total;
	int (*tx_fn)(const uint8_t *, size_t);
	bool sent = false;

	if (len < ETHER_HDR_LEN || len > IPA_FRAME_MAX) {
		ipa_state.tx_drops++;
		ifnet_stat_increment_out(ifp, 0, 0, 1);
		mbuf_freem(packet);
		return 0;
	}
	bpf_tap_out(ifp, DLT_EN10MB, packet, NULL, 0);
	buf = kalloc_data(QMAP_HDR_LEN + len + 4, Z_WAITOK);
	if (buf == NULL) {
		ipa_state.tx_drops++;
		mbuf_freem(packet);
		return 0;
	}
	if (mbuf_copydata(packet, 0, len, buf + QMAP_HDR_LEN) != 0) {
		kfree_data(buf, QMAP_HDR_LEN + len + 4);
		ipa_state.tx_drops++;
		mbuf_freem(packet);
		return 0;
	}
	mbuf_freem(packet);

	/* Check for ARP requests on the Ethernet-framed cellular interface. */
	if (buf[QMAP_HDR_LEN + 12] == 0x08 && buf[QMAP_HDR_LEN + 13] == 0x06) {
		ipa_handle_arp(buf + QMAP_HDR_LEN, len);
		kfree_data(buf, QMAP_HDR_LEN + len + 4);
		ifnet_stat_increment_out(ifp, 1, (uint32_t)len, 0);
		return 0;
	}

	/* Strip the 14-byte L2 header and prepend the 4-byte QMAP header right before the IP payload. */
	ip_ptr = buf + QMAP_HDR_LEN + ETHER_HDR_LEN;
	ip_len = len - ETHER_HDR_LEN;
	pad = (4 - (ip_len & 3)) & 3;
	if (pad > 0) {
		bzero((void *)(uintptr_t)(ip_ptr + ip_len), pad);
	}
	qmap_total = QMAP_HDR_LEN + ip_len + pad;
	{
		uint8_t *qmap_pkt = (uint8_t *)(uintptr_t)(ip_ptr - QMAP_HDR_LEN);
		uint16_t plen = (uint16_t)(ip_len + pad);

		qmap_pkt[0] = (uint8_t)(pad & 0x3f);
		qmap_pkt[1] = ipa_mux_id;
		qmap_pkt[2] = (uint8_t)(plen >> 8);
		qmap_pkt[3] = (uint8_t)(plen & 0xff);

		lck_mtx_lock(&ipa_lock);
		tx_fn = ipa_modem_tx_fn;
		lck_mtx_unlock(&ipa_lock);
		if (ipa_link_up && tx_fn != NULL && tx_fn(qmap_pkt, qmap_total) == 0) {
			sent = true;
		}
	}
	kfree_data(buf, QMAP_HDR_LEN + len + 4);
	if (sent) {
		ipa_state.tx_pkts++;
		ipa_state.tx_bytes += ip_len;
		ifnet_stat_increment_out(ifp, 1, (uint32_t)ip_len, 0);
	} else {
		ipa_state.tx_drops++;
		ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	return 0;
}

static errno_t
ipa_ioctl(ifnet_t ifp, u_long command, void *data)
{
	struct ifreq *ifr = data;
	struct ifmediareq *ifmr = data;
	const int media[] = { IFM_ETHER | IFM_AUTO };
	user_addr_t list;
	errno_t error = 0;

	switch (command) {
	case SIOCSIFADDR:
		ifnet_set_flags(ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
		break;
	case SIOCSIFFLAGS:
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		break;
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64:
		list = command == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)ifmr)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)ifmr)->ifmu_ulist);
		ifmr->ifm_active = ifmr->ifm_current = media[0];
		ifmr->ifm_mask = 0;
		ifmr->ifm_status = IFM_AVALID | (ipa_link_up ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && ifmr->ifm_count > 0) {
			error = copyout(media, list, sizeof(media));
		}
		ifmr->ifm_count = 1;
		break;
	case SIOCGIFDEVMTU:
		ifr->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
		ifr->ifr_devmtu.ifdm_max = IPA_DEFAULT_MTU;
		ifr->ifr_devmtu.ifdm_min = IF_MINMTU;
		break;
	case SIOCSIFMTU:
		error = (ifr->ifr_mtu > (int)IPA_DEFAULT_MTU || ifr->ifr_mtu < IF_MINMTU) ?
		    EINVAL : ifnet_set_mtu(ifp, ifr->ifr_mtu);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}
	return error;
}

void
annibale_ipa_set_tx_handler(int (*tx_fn)(const uint8_t *qmap_frame, size_t len), uint8_t mux_id)
{
	lck_mtx_lock(&ipa_lock);
	ipa_modem_tx_fn = tx_fn;
	if (mux_id != 0) {
		ipa_mux_id = mux_id;
	}
	lck_mtx_unlock(&ipa_lock);
}

void
annibale_ipa_configure_v4(uint32_t ipv4_addr, uint32_t ipv4_gw, uint32_t ipv4_mask,
    uint32_t dns1, uint32_t dns2, uint32_t mtu)
{
	char dns1_str[24], dns2_str[24];
	bool was_up;

	lck_mtx_lock(&ipa_lock);
	was_up = ipa_link_up;
	ipa_state.ipv4_addr = ipv4_addr;
	ipa_state.ipv4_gw = ipv4_gw;
	ipa_state.ipv4_mask = ipv4_mask ? ipv4_mask : 0xfffffffcU;
	ipa_state.ipv4_dns1 = dns1;
	ipa_state.ipv4_dns2 = dns2;
	if (mtu >= IF_MINMTU && mtu <= IPA_DEFAULT_MTU) {
		ipa_state.mtu = mtu;
	}
	ipa_link_up = (ipv4_addr != 0);
	ipa_format_ip(ipv4_addr, sysctl_cellular_ipv4, sizeof(sysctl_cellular_ipv4));
	ipa_format_ip(ipv4_gw, sysctl_cellular_gw, sizeof(sysctl_cellular_gw));
	ipa_format_ip(dns1, dns1_str, sizeof(dns1_str));
	ipa_format_ip(dns2, dns2_str, sizeof(dns2_str));
	if (dns2 != 0) {
		snprintf(sysctl_cellular_dns, sizeof(sysctl_cellular_dns), "%s,%s", dns1_str, dns2_str);
	} else {
		strlcpy(sysctl_cellular_dns, dns1_str, sizeof(sysctl_cellular_dns));
	}
	lck_mtx_unlock(&ipa_lock);

	if (ipa_ifp != NULL) {
		if (mtu >= IF_MINMTU && mtu <= IPA_DEFAULT_MTU) {
			ifnet_set_mtu(ipa_ifp, mtu);
		}
		if (ipa_link_up) {
			ifnet_set_flags(ipa_ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
			if (!was_up) {
				ipa_link_event(ipa_ifp, KEV_DL_LINK_ON);
			}
		} else if (was_up) {
			ifnet_set_flags(ipa_ifp, 0, IFF_RUNNING);
			ipa_link_event(ipa_ifp, KEV_DL_LINK_OFF);
		}
	}
	printf("annibale-ipa: pdp_ip0 runtime configured ip=%s gw=%s dns=%s mtu=%u link=%s\n",
	    sysctl_cellular_ipv4, sysctl_cellular_gw, sysctl_cellular_dns,
	    ipa_state.mtu, ipa_link_up ? "up" : "down");
}

void
annibale_ipa_init(void)
{
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = "annibale-pdp_ip0",
		.uniqueid_len = 16,
		.name = "pdp_ip",
		.unit = 0,
		.family = IFNET_FAMILY_ETHERNET,
		.subfamily = IFNET_SUBFAMILY_ANY,
		.type = IFT_CELLULAR,
		.output = ipa_output,
		.demux = ether_demux,
		.add_proto = ether_add_proto,
		.del_proto = ether_del_proto,
		.check_multi = ether_check_multi,
		.framer_extended = ether_frameout_extended,
		.ioctl = ipa_ioctl,
		.broadcast_addr = etherbroadcastaddr,
		.broadcast_len = ETHER_ADDR_LEN,
	};
	ifnet_t ifp = NULL;
	errno_t error;

	if (!redwood_platform_is_annibale() || ipa_inited) {
		return;
	}
	ipa_inited = true;

	error = ifnet_allocate_extended(&init, &ifp);
	if (error != 0) {
		printf("annibale-ipa: failed to allocate pdp_ip0 (err=%d)\n", error);
		return;
	}
	ifnet_set_capabilities_enabled(ifp, 0, -1);
	ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
	ifnet_set_baudrate(ifp, 1000ULL * 1000ULL * 1000ULL);
	ifnet_set_mtu(ifp, IPA_DEFAULT_MTU);
	ifnet_set_flags(ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
	ifnet_set_hdrlen(ifp, ETHER_HDR_LEN);
	ifnet_set_offload(ifp, 0);

	error = ifnet_attach(ifp, NULL);
	if (error != 0) {
		printf("annibale-ipa: failed to attach pdp_ip0 (err=%d)\n", error);
		ifnet_release(ifp);
		return;
	}
	ifnet_set_lladdr(ifp, ipa_lladdr, ETHER_ADDR_LEN);
	bpfattach(ifp, DLT_EN10MB, ETHER_HDR_LEN);
	ipa_ifp = ifp;
	printf("annibale-ipa: pdp_ip0 attached (IPA v5.5 @ 0x%llx, fw_mem=0x%llx, mux_id=%u, mtu=%u)\n",
	    IPA_WRAPPER_PHYS, IPA_FW_MEM_PHYS, ipa_mux_id, IPA_DEFAULT_MTU);
}

/* ---- Shared SM8750 TrustZone PAS .mdt + .bNN firmware loader (for IPA & MPSS) ---------------------- */

#define SCM_64                  (1ULL << 30)
#define SCM_SIP                 (2ULL << 24)
#define SCM_CALL(svc, cmd)      ((uint64_t)(svc) << 8 | (cmd))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)

struct ipa_elf32_phdr {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

static int64_t
annibale_tz_smc(uint64_t fn, uint64_t arg_cnt, uint64_t a0, uint64_t a1, uint64_t a2)
{
	for (unsigned tries = 0; tries < 500; tries++) {
		register uint64_t x0 __asm__("x0") = fn | SCM_SIP | SCM_64;
		register uint64_t x1 __asm__("x1") = (arg_cnt & 0xff) | (1ULL << 8);
		register uint64_t x2 __asm__("x2") = a0;
		register uint64_t x3 __asm__("x3") = a1;
		register uint64_t x4 __asm__("x4") = a2;
		register uint64_t x6 __asm__("x6") = 0;

		__asm__ volatile("smc #0"
		    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x6)
		    :
		    : "x5", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		if ((int64_t)x0 != 1 && (int64_t)x0 != -6 && (int64_t)x0 != -13) {
			return (int64_t)x0;
		}
		if ((int64_t)x0 == 1) {
			fn = x0;
			continue;
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	return -1;
}

extern bool redwood_ufs_fat16_fw_read(const char *name, uint64_t *size_out, volatile void *dst_buf, uint64_t dst_cap);

int64_t
annibale_tz_pas_auth_reset(uint32_t pas_id)
{
	return annibale_tz_smc(SCM_PAS_AUTH_AND_RESET, 1, pas_id, 0, 0);
}

static uint8_t *
annibale_read_fw_file(const char *name, uint64_t *size_out, bool *needs_free_out)
{
	static const char *const fw_dirs[] = { "/fw", "/redwood/fw", "/System/Volumes/iOS/fw" };
	const uint8_t *img_data;
	vfs_context_t ctx;

	*size_out = 0;
	*needs_free_out = false;
	img_data = redwood_image_file("fw", name, size_out, NULL);
	if (img_data != NULL && *size_out > 0) {
		return __DECONST(uint8_t *, img_data);
	}
	if (rootvnode != NULLVP) {
		ctx = vfs_context_kernel();
		for (unsigned d = 0; d < sizeof(fw_dirs) / sizeof(fw_dirs[0]); d++) {
			char path[96];
			vnode_t vp = NULLVP;
			struct vnode_attr va;
			uint8_t *buf;
			int resid = 0, err;

			snprintf(path, sizeof(path), "%s/%s", fw_dirs[d], name);
			if (vnode_lookup(path, 0, &vp, ctx) != 0) {
				continue;
			}
			VATTR_INIT(&va);
			VATTR_WANTED(&va, va_data_size);
			if (vnode_getattr(vp, &va, ctx) != 0 || va.va_data_size == 0 || va.va_data_size > 0x0f600000ULL) {
				vnode_put(vp);
				continue;
			}
			buf = kalloc_data(va.va_data_size, Z_WAITOK);
			if (buf == NULL) {
				vnode_put(vp);
				return NULL;
			}
			err = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)va.va_data_size, 0, UIO_SYSSPACE, 0,
			    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
			vnode_put(vp);
			if (err == 0 && resid == 0) {
				*size_out = va.va_data_size;
				*needs_free_out = true;
				return buf;
			}
			kfree_data(buf, va.va_data_size);
		}
	}
	uint64_t fat_sz = 0;
	if (redwood_ufs_fat16_fw_read(name, &fat_sz, NULL, 0) && fat_sz > 0 && fat_sz <= 0x400000ULL) {
		uint8_t *buf = kalloc_data(fat_sz, Z_WAITOK);
		if (buf != NULL) {
			if (redwood_ufs_fat16_fw_read(name, &fat_sz, buf, fat_sz)) {
				*size_out = fat_sz;
				*needs_free_out = true;
				return buf;
			}
			kfree_data(buf, fat_sz);
		}
	}
	return NULL;
}

bool
annibale_fw_exists(const char *name)
{
	uint64_t sz = 0;
	bool need_free = false;
	if (redwood_ufs_fat16_fw_read(name, &sz, NULL, 0) && sz > 0) {
		return true;
	}
	uint8_t *buf = annibale_read_fw_file(name, &sz, &need_free);

	if (buf == NULL) {
		return false;
	}
	if (need_free) {
		kfree_data(buf, sz);
	}
	return true;
}

bool
annibale_load_mdt_image(const char *prefix, uint32_t pas_id, uint32_t alt_pas_id,
    uint64_t mem_phys, uint64_t mem_size, uint32_t *used_pas_out)
{
	char mdt_name[32];
	uint64_t mdt_sz = 0;
	bool mdt_free = false, ok = false;
	uint8_t *mdt_buf;
	uint32_t phnum, lowest = ~0u, highest = 0, active_pas = pas_id;
	void *tz_hdr_page = NULL;
	vm_offset_t carveout = 0;
	int64_t rc;

	snprintf(mdt_name, sizeof(mdt_name), "%s.mdt", prefix);
	mdt_buf = annibale_read_fw_file(mdt_name, &mdt_sz, &mdt_free);
	if (mdt_buf == NULL || mdt_sz < 52) {
		if (mdt_buf != NULL && mdt_free) {
			kfree_data(mdt_buf, mdt_sz);
		}
		return false;
	}
	phnum = mdt_buf[44] | ((uint32_t)mdt_buf[45] << 8);
	if (memcmp(mdt_buf, "\177ELF\1", 5) != 0 || phnum < 2 || phnum > 64 || 52 + phnum * 32 > mdt_sz) {
		goto out;
	}
	tz_hdr_page = kalloc_data(mdt_sz < PAGE_SIZE ? PAGE_SIZE : mdt_sz, Z_WAITOK | Z_ZERO);
	if (tz_hdr_page == NULL) {
		goto out;
	}
	memcpy(tz_hdr_page, mdt_buf, mdt_sz);
	__asm__ volatile("dc civac, %0; dsb sy" :: "r"(tz_hdr_page) : "memory");

	rc = annibale_tz_smc(SCM_PAS_INIT_IMAGE, 2, active_pas, kvtophys((vm_offset_t)tz_hdr_page), 0);
	if (rc != 0 && alt_pas_id != 0) {
		active_pas = alt_pas_id;
		rc = annibale_tz_smc(SCM_PAS_INIT_IMAGE, 2, active_pas, kvtophys((vm_offset_t)tz_hdr_page), 0);
	}
	if (rc != 0) {
		printf("annibale-ipa: PAS_INIT_IMAGE(%s, pas=%u) returned %lld\n", prefix, active_pas, rc);
		goto out;
	}
	carveout = ml_io_map_unmappable(mem_phys, mem_size, VM_WIMG_WCOMB);
	if (carveout == 0) {
		goto out;
	}
	for (uint32_t i = 0; i < phnum; i++) {
		struct ipa_elf32_phdr ph;
		char seg_name[32];
		uint64_t seg_sz = 0;
		bool seg_free = false;
		uint8_t *seg_buf;
		uint32_t rel_off;

		memcpy(&ph, mdt_buf + 52 + i * 32, sizeof(ph));
		if (ph.type != 1 || ((ph.flags >> 24) & 7) == 2 || ph.memsz == 0) {
			continue;
		}
		if (ph.paddr < mem_phys || (uint64_t)ph.paddr + ph.memsz > mem_phys + mem_size) {
			goto out;
		}
		rel_off = (uint32_t)(ph.paddr - mem_phys);
		if (ph.filesz > 0) {
			snprintf(seg_name, sizeof(seg_name), "%s.b%02u", prefix, i);
			seg_buf = annibale_read_fw_file(seg_name, &seg_sz, &seg_free);
			if (seg_buf == NULL || seg_sz != ph.filesz) {
				if (seg_buf != NULL && seg_free) {
					kfree_data(seg_buf, seg_sz);
				}
				goto out;
			}
			memcpy((void *)(carveout + rel_off), seg_buf, ph.filesz);
			if (seg_free) {
				kfree_data(seg_buf, seg_sz);
			}
		}
		if (ph.memsz > ph.filesz) {
			bzero((void *)(carveout + rel_off + ph.filesz), ph.memsz - ph.filesz);
		}
		if (ph.paddr < lowest) {
			lowest = ph.paddr;
		}
		if (ph.paddr + ph.memsz > highest) {
			highest = ph.paddr + ph.memsz;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	rc = annibale_tz_smc(SCM_PAS_MEM_SETUP, 3, active_pas, mem_phys,
	    ((highest + 0xfffu) & ~0xfffu) - (uint32_t)mem_phys);
	if (rc != 0) {
		printf("annibale-ipa: PAS_MEM_SETUP(%s, pas=%u) returned %lld\n", prefix, active_pas, rc);
		goto out;
	}
	if (used_pas_out != NULL) {
		*used_pas_out = active_pas;
	}
	ok = true;
out:
	if (carveout != 0) {
		ml_io_unmap(carveout, mem_size);
	}
	if (tz_hdr_page != NULL) {
		kfree_data(tz_hdr_page, mdt_sz < PAGE_SIZE ? PAGE_SIZE : mdt_sz);
	}
	if (mdt_buf != NULL && mdt_free) {
		kfree_data(mdt_buf, mdt_sz);
	}
	return ok;
}

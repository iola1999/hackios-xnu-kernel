/*
 * Bluetooth for iOS 26 (boot-arg bt=4): the Skywalk kernel-pipe nexus and kernel control socket
 * com.apple.uart.sk.bluetooth, the character devices /dev/uart.bluetooth, /dev/cu.bluetooth and
 * /dev/bt_h4, the readiness sysctl kern.redwood.bt_ready, and the H4 pump over SE14.
 *
 * bluetoothd's POSIX H4 transport looks up the IOKit service "bluetooth" (RedwoodBT.cpp) for its
 * BSD Name and 6-byte local-mac-address, connects the kernel control socket com.apple.uart.sk.bluetooth,
 * reads the 16-byte Skywalk nexus UUID from socket option 0x5412, and opens a Skywalk channel on that
 * nexus. Both the control socket connect and /dev/uart.bluetooth open wait up to 40 s for the kernel's
 * WCN7850 firmware download and HCI Reset (annibale_bt_hci.c) to finish before serving H4 packets.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <skywalk/os_skywalk_private.h>
#include <skywalk/nexus/kpipe/nx_kernel_pipe.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <os/atomic_private.h>
#include "annibale_bt_private.h"

extern void RedwoodBTStart(const char *bsdname, const uint8_t *address);
extern void RedwoodBTSetAddress(const uint8_t *address);

SYSCTL_DECL(_kern_redwood);

#define BT_RING                 (16u << 10)
#define IBS_WAKE_ACK            0xfc
#define IBS_WAKE_IND            0xfd
#define IBS_SLEEP_IND           0xfe

static uint8_t *bt_ring;
static uint32_t bt_ring_in, bt_ring_out, bt_rx_lost;
static lck_mtx_t *bt_ring_lock;
static struct selinfo bt_sel;
static int bt_serving;
static bool bt_ibs_asleep;
static volatile uint32_t bt_irq_event;

SYSCTL_INT(_kern_redwood, OID_AUTO, bt_ready, CTLFLAG_RD | CTLFLAG_LOCKED, &bt_serving, 0,
    "whether the WCN7850 Bluetooth firmware and reset are ready");

static void
ms(uint32_t n)
{
	delay_for_interval(n, NSEC_PER_MSEC);
}

static uint32_t
bt_ring_used(void)
{
	return bt_ring_in - bt_ring_out;
}

static void
bt_ring_put_byte(uint8_t b)
{
	if (bt_ring_used() < BT_RING) {
		bt_ring[bt_ring_in++ % BT_RING] = b;
	} else {
		bt_rx_lost++;
	}
}

/*
 * Move bytes from SE14's receive buffer into bt_ring under bt_ring_lock, tracking H4 packet boundaries
 * so single-byte Qualcomm In-Band Sleep indicators (0xfc/0xfd/0xfe) or line noise between packets are
 * handled here and never corrupt bluetoothd's H4 stream.
 */
static void
bt_ring_fill(void)
{
	static uint8_t h4_type;
	static uint32_t h4_hdr_pos, h4_hdr_need, h4_rem, h4_len_lo;
	static uint32_t ibs_said;
	uint32_t have = 0;
	const uint8_t *rx;
	bool send_wake_ack = false;

	(void)annibale_bt_uart_poll();
	rx = annibale_bt_uart_rx(&have);
	for (uint32_t i = 0; i < have; i++) {
		uint8_t b = rx[i];

		if (h4_rem != 0) {
			bt_ring_put_byte(b);
			h4_rem--;
			continue;
		}
		if (h4_type != 0) {
			bt_ring_put_byte(b);
			h4_hdr_pos++;
			if (h4_type == 0x04 && h4_hdr_pos == 2) {
				h4_rem = b;
				h4_type = 0;
			} else if (h4_type == 0x02 && h4_hdr_pos == 3) {
				h4_len_lo = b;
			} else if (h4_type == 0x02 && h4_hdr_pos == 4) {
				h4_rem = h4_len_lo | ((uint32_t)(b & 0x0f) << 8);
				h4_type = 0;
			} else if (h4_type == 0x03 && h4_hdr_pos == 3) {
				h4_rem = b;
				h4_type = 0;
			}
			continue;
		}
		if (b == IBS_WAKE_IND) {
			bt_ibs_asleep = false;
			send_wake_ack = true;
			if (ibs_said++ < 8) {
				bt_say("bt: chip sent IBS wake (0xfd); replying 0xfc\n");
			}
			continue;
		}
		if (b == IBS_SLEEP_IND) {
			bt_ibs_asleep = true;
			if (ibs_said++ < 8) {
				bt_say("bt: chip sent IBS sleep (0xfe)\n");
			}
			continue;
		}
		if (b == IBS_WAKE_ACK) {
			bt_ibs_asleep = false;
			continue;
		}
		if (b == 0x04 || b == 0x02 || b == 0x03) {
			bt_ring_put_byte(b);
			h4_type = b;
			h4_hdr_pos = 0;
			h4_hdr_need = b == 0x04 ? 2 : (b == 0x02 ? 4 : 3);
			(void)h4_hdr_need;
			continue;
		}
		if (ibs_said++ < 8) {
			bt_say("bt: dropped non-H4 byte %#04x at packet boundary\n", b);
		}
	}
	if (have != 0) {
		annibale_bt_uart_take(have);
	}
	if (send_wake_ack) {
		uint8_t ack = IBS_WAKE_ACK;

		(void)annibale_bt_uart_write(&ack, 1);
	}
}

static bool
bt_uart_send_locked(const uint8_t *data, uint32_t len)
{
	bool ok;

	lck_mtx_lock(bt_ring_lock);
	if (bt_ibs_asleep) {
		uint8_t wake = IBS_WAKE_IND;

		bt_ibs_asleep = false;
		(void)annibale_bt_uart_write(&wake, 1);
	}
	ok = annibale_bt_uart_write(data, len);
	bt_ring_fill();
	lck_mtx_unlock(bt_ring_lock);
	return ok;
}

/* ---- Skywalk kernel-pipe nexus (com.apple.uart.sk.bluetooth) -------------------------------- */

static uuid_t btsk_dom_uuid, btsk_prov_uuid, btsk_nx_uuid;
static nexus_controller_t btsk_ncd;
static kern_pbufpool_t btsk_rx_pp;
static kern_channel_ring_t btsk_txring, btsk_rxring;
static uint64_t btsk_bufsz;
static volatile int btsk_up;
static uint32_t btsk_said;

#define BTSK_SAY(...) do { if (btsk_said++ < 40) bt_say("btsk: " __VA_ARGS__); } while (0)

static void bt_ios_put(const uint8_t *p, uint32_t n);
static uint32_t bt_ios_take(uint8_t *p, uint32_t most);

static errno_t
btsk_dom_init(kern_nexus_domain_provider_t domprov __unused)
{
	return 0;
}

static void
btsk_dom_fini(kern_nexus_domain_provider_t domprov __unused)
{
}

static errno_t
btsk_pre_connect(kern_nexus_provider_t nxprov __unused, proc_t p __unused, kern_nexus_t nexus,
    nexus_port_t port, kern_channel_t channel __unused, void **ch_ctx)
{
	kern_pbufpool_t tx_pp = NULL;
	errno_t error = kern_nexus_get_pbufpool(nexus, &tx_pp, &btsk_rx_pp);

	*ch_ctx = NULL;
	BTSK_SAY("bluetoothd opens a channel (port %u): pools %d\n", port, error);
	return error;
}

static errno_t
btsk_connected(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_t channel __unused)
{
	btsk_up = 1;
	BTSK_SAY("channel is connected: the H4 stream runs through it\n");
	return 0;
}

static void
btsk_pre_disconnect(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_t channel __unused)
{
	btsk_up = 0;
}

static void
btsk_disconnected(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_t channel __unused)
{
	btsk_up = 0;
	btsk_rx_pp = NULL;
	BTSK_SAY("channel is closed\n");
}

static errno_t
btsk_ring_init(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_t channel __unused,
    kern_channel_ring_t ring, boolean_t is_tx_ring, void **ring_ctx)
{
	if (is_tx_ring) {
		btsk_txring = ring;
	} else {
		btsk_rxring = ring;
	}
	*ring_ctx = NULL;
	return 0;
}

static void
btsk_ring_fini(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring)
{
	if (ring == btsk_txring) {
		btsk_txring = NULL;
	} else if (ring == btsk_rxring) {
		btsk_rxring = NULL;
	}
}

static errno_t
btsk_slot_init(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring __unused,
    channel_slot_t slot __unused, uint32_t slot_id __unused, struct kern_slot_prop **slot_prop_addr, void **pslot_ctx)
{
	*slot_prop_addr = NULL;
	*pslot_ctx = NULL;
	return 0;
}

static void
btsk_slot_fini(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring __unused,
    channel_slot_t slot __unused, uint32_t slot_id __unused)
{
}

static errno_t
btsk_sync_tx(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring,
    uint32_t flags __unused)
{
	kern_channel_slot_t ts, last = NULL;

	for (ts = kern_channel_get_next_slot(ring, NULL, NULL); ts != NULL; ts = kern_channel_get_next_slot(ring, ts, NULL)) {
		kern_packet_t ph = kern_channel_slot_get_packet(ring, ts);
		kern_buflet_t buf = ph != 0 ? kern_packet_get_next_buflet(ph, NULL) : NULL;

		if (buf != NULL) {
			uint8_t *base = kern_buflet_get_data_address(buf);
			uint32_t len = kern_buflet_get_data_length(buf), off = kern_buflet_get_data_offset(buf);

			if (base != NULL && len != 0) {
				bt_ios_put(base + off, len);
			}
		}
		last = ts;
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	return 0;
}

static errno_t
btsk_sync_rx(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring,
    uint32_t flags __unused)
{
	kern_channel_slot_t rs, last = NULL;

	(void)kern_channel_reclaim(ring);
	if (btsk_rx_pp == NULL) {
		return 0;
	}
	for (rs = kern_channel_get_next_slot(ring, NULL, NULL); rs != NULL; rs = kern_channel_get_next_slot(ring, rs, NULL)) {
		kern_packet_t ph = 0;
		kern_buflet_t buf;
		uint8_t *base;
		uint32_t n, room;

		if (kern_pbufpool_alloc_nosleep(btsk_rx_pp, 1, &ph) != 0 || ph == 0) {
			break;
		}
		buf = kern_packet_get_next_buflet(ph, NULL);
		base = buf != NULL ? kern_buflet_get_data_address(buf) : NULL;
		room = (uint32_t)MIN(btsk_bufsz, 1024ull);
		n = base != NULL ? bt_ios_take(base, room) : 0;
		if (n == 0) {
			kern_pbufpool_free(btsk_rx_pp, ph);
			break;
		}
		(void)kern_buflet_set_data_offset(buf, 0);
		(void)kern_buflet_set_data_length(buf, (uint16_t)n);
		(void)kern_packet_finalize(ph);
		if (kern_channel_slot_attach_packet(ring, rs, ph) != 0) {
			kern_pbufpool_free(btsk_rx_pp, ph);
			break;
		}
		last = rs;
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	return 0;
}

static void
btsk_kick(void)
{
	kern_channel_ring_t ring = btsk_rxring;

	if (btsk_up && ring != NULL) {
		kern_channel_notify(ring, 0);
	}
}

static bool
btsk_uuid(uint8_t uuid[16])
{
	if (uuid_is_null(btsk_nx_uuid)) {
		return false;
	}
	memcpy(uuid, btsk_nx_uuid, 16);
	return true;
}

static void
btsk_start(void)
{
	nexus_attr_t nxa = NULL;
	errno_t error;
	uuid_string_t text;

	struct kern_nexus_domain_provider_init dom_init = {
		.nxdpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxdpi_flags = 0,
		.nxdpi_init = btsk_dom_init,
		.nxdpi_fini = btsk_dom_fini,
	};
	struct kern_nexus_provider_init prov_init = {
		.nxpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxpi_flags = NXPIF_VIRTUAL_DEVICE,
		.nxpi_pre_connect = btsk_pre_connect,
		.nxpi_connected = btsk_connected,
		.nxpi_pre_disconnect = btsk_pre_disconnect,
		.nxpi_disconnected = btsk_disconnected,
		.nxpi_ring_init = btsk_ring_init,
		.nxpi_ring_fini = btsk_ring_fini,
		.nxpi_slot_init = btsk_slot_init,
		.nxpi_slot_fini = btsk_slot_fini,
		.nxpi_sync_tx = btsk_sync_tx,
		.nxpi_sync_rx = btsk_sync_rx,
		.nxpi_tx_doorbell = NULL,
	};
	struct kern_nexus_init nx_init = {
		.nxi_version = KERN_NEXUS_CURRENT_VERSION,
		.nxi_flags = 0,
		.nxi_tx_pbufpool = NULL,
		.nxi_rx_pbufpool = NULL,
	};

	if (!uuid_is_null(btsk_nx_uuid)) {
		return;
	}
	error = kern_nexus_register_domain_provider(NEXUS_TYPE_KERNEL_PIPE, (const uint8_t *)"com.apple.uart.sk.bluetooth",
	    &dom_init, sizeof(dom_init), &btsk_dom_uuid);
	if (error == 0) {
		error = kern_nexus_controller_create(&btsk_ncd);
	}
	if (error == 0) {
		error = kern_nexus_attr_create(&nxa);
	}
	if (error == 0) {
		error = kern_nexus_attr_set(nxa, NEXUS_ATTR_ANONYMOUS, 1);
	}
	if (error == 0) {
		error = kern_nexus_controller_register_provider(btsk_ncd, btsk_dom_uuid,
		    (const uint8_t *)"com.apple.nexus.uart.sk.bluetooth", &prov_init, sizeof(prov_init), nxa, &btsk_prov_uuid);
	}
	if (error == 0) {
		(void)kern_nexus_controller_read_provider_attr(btsk_ncd, btsk_prov_uuid, nxa);
		(void)kern_nexus_attr_get(nxa, NEXUS_ATTR_SLOT_BUF_SIZE, &btsk_bufsz);
		error = kern_nexus_controller_alloc_provider_instance(btsk_ncd, btsk_prov_uuid, NULL, NULL, &btsk_nx_uuid,
		    &nx_init);
	}
	if (nxa != NULL) {
		kern_nexus_attr_destroy(nxa);
	}
	uuid_unparse_upper(btsk_nx_uuid, text);
	bt_say("btsk: the Bluetooth UART's nexus: %s (error %d), buffers of %llu bytes\n", error == 0 ? text : "NONE", error,
	    btsk_bufsz);
}

/* ---- Kernel control socket (com.apple.uart.sk.bluetooth) ----------------------------------- */

static kern_ctl_ref bt_kctl;
static uint32_t bt_kctl_unit;
static volatile int bt_kctl_up;
static bool bt_answer_vendor(const uint8_t *packet, uint32_t n);

static errno_t
bt_kctl_connect(kern_ctl_ref ref __unused, struct sockaddr_ctl *sac, void **unitinfo)
{
	int waited;

	for (waited = 0; !bt_serving && waited < 40000; waited += 50) {
		ms(50);
	}
	bt_kctl_unit = sac->sc_unit;
	*unitinfo = NULL;
	bt_kctl_up = 1;
	bt_say("bt: connected to com.apple.uart.sk.bluetooth (unit %u, waited %d ms, ready %d)\n",
	    sac->sc_unit, waited, bt_serving);
	return bt_serving ? 0 : ENXIO;
}

static errno_t
bt_kctl_disconnect(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused)
{
	bt_kctl_up = 0;
	bt_say("bt: disconnected from com.apple.uart.sk.bluetooth\n");
	return 0;
}

static errno_t
bt_kctl_send(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused, mbuf_t m, int flags __unused)
{
	static uint32_t said;
	uint8_t buf[1100];
	size_t n = mbuf_pkthdr_len(m);

	if (n > sizeof(buf)) {
		n = sizeof(buf);
	}
	if (mbuf_copydata(m, 0, n, buf) == 0 && n != 0) {
		if (said++ < 24) {
			annibale_bt_hex("iOS sends (socket)", buf, (uint32_t)(n < 16 ? n : 16));
		}
		if (!bt_answer_vendor(buf, (uint32_t)n)) {
			(void)bt_uart_send_locked(buf, (uint32_t)n);
		}
	}
	mbuf_freem(m);
	return 0;
}

static errno_t
bt_kctl_setopt(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused, int opt, void *data __unused,
    size_t len)
{
	static uint32_t said;

	if (said++ < 16) {
		bt_say("bt: bluetoothd sets socket option %#x (%zu bytes): taken\n", opt, len);
	}
	return 0;
}

static errno_t
bt_kctl_getopt(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused, int opt, void *data,
    size_t *len)
{
	static uint32_t said;

	if (data != NULL && len != NULL) {
		bzero(data, *len);
		if ((opt & 0xffff) == 0x5412 && *len >= 16) {
			(void)btsk_uuid(data);
		}
	}
	if (said++ < 16) {
		bt_say("bt: bluetoothd asks socket option %#x (%zu bytes)%s\n", opt, len ? *len : 0,
		    (opt & 0xffff) == 0x5412 ? ": the nexus UUID" : ": zeros");
	}
	return 0;
}

static void
bt_kctl_register(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	if (bt_kctl != NULL) {
		return;
	}
	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.sk.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_sendsize = 64 * 1024;
	reg.ctl_recvsize = 64 * 1024;
	reg.ctl_connect = bt_kctl_connect;
	reg.ctl_disconnect = bt_kctl_disconnect;
	reg.ctl_send = bt_kctl_send;
	reg.ctl_setopt = bt_kctl_setopt;
	reg.ctl_getopt = bt_kctl_getopt;
	error = ctl_register(&reg, &bt_kctl);
	bt_say("bt: kernel control com.apple.uart.sk.bluetooth: %s (%d)\n", error == 0 ? "registered" : "NOT registered", error);
}

/* ---- H4 stream & vendor command responder -------------------------------------------------- */

static void
bt_ios_put(const uint8_t *p, uint32_t n)
{
	static uint32_t said;

	if (said++ < 120) {
		annibale_bt_hex("iOS sends (channel)", p, n < 16 ? n : 16);
	}
	if (!bt_answer_vendor(p, n)) {
		(void)bt_uart_send_locked(p, n);
	}
}

static uint32_t
bt_ios_take(uint8_t *p, uint32_t most)
{
	uint32_t n = 0;

	if (bt_ring_lock == NULL) {
		return 0;
	}
	lck_mtx_lock(bt_ring_lock);
	while (n < most && bt_ring_used() != 0) {
		p[n++] = bt_ring[bt_ring_out++ % BT_RING];
	}
	lck_mtx_unlock(bt_ring_lock);
	{
		static uint32_t said_refused, said_adv;

		for (uint32_t i = 0; i + 7 <= n; i++) {
			if (p[i] == 0x04 && p[i + 1] == 0x0e && p[i + 6] != 0 && said_refused < 40) {
				said_refused++;
				bt_say("bt: the chip refuses command %#06x: status %#x\n", p[i + 4] | p[i + 5] << 8, p[i + 6]);
			} else if (p[i] == 0x04 && p[i + 1] == 0x0f && p[i + 3] != 0 && said_refused < 40) {
				said_refused++;
				bt_say("bt: the chip refuses command %#06x (status event): %#x\n", p[i + 5] | p[i + 6] << 8, p[i + 3]);
			} else if (p[i] == 0x04 && p[i + 1] == 0x3e && p[i + 3] == 0x02 && i + 11 <= n && said_adv < 8) {
				said_adv++;
				bt_say("bt: LE advertising report to bluetoothd (%02x:%02x:%02x:xx:xx:xx, %u bytes)\n",
				    p[i + 11], p[i + 10], p[i + 9], p[i + 2]);
			} else if (p[i] == 0x04 && p[i + 1] == 0x3e && (p[i + 3] == 0x01 || p[i + 3] == 0x0a)) {
				bt_say("bt: LE connection established with peripheral (subevent %#x, handle %#x)\n",
				    p[i + 3], p[i + 4] | p[i + 5] << 8);
			} else if (p[i] == 0x02 && i + 5 <= n) {
				static uint32_t said_acl;
				if (said_acl++ < 20) {
					bt_say("bt: ACL data packet (handle %#x, length %u)\n",
					    (p[i + 1] | p[i + 2] << 8) & 0x0fff, p[i + 3] | p[i + 4] << 8);
				}
			}
		}
	}
	/*
	 * Patch the Read Local Supported Commands answer (04 0e 44 01 02 10 00, then 64 octets): set bits 2 and 3 of
	 * octet 18 (Read/Write Default Erroneous Data Reporting), which bluetoothd requires ("status 628") and which
	 * bt_answer_vendor answers locally.
	 */
	{
		static const uint8_t head[7] = { 0x04, 0x0e, 0x44, 0x01, 0x02, 0x10, 0x00 };
		static uint32_t matched, after;

		for (uint32_t i = 0; i < n; i++) {
			if (after != 0) {
				if (after == 19) {
					p[i] |= 0x0c;
					bt_say("bt: supported commands: octet 18 now %#x (erroneous data reporting answered here)\n", p[i]);
					after = 0;
				} else {
					after++;
				}
				continue;
			}
			matched = p[i] == head[matched] ? matched + 1 : (p[i] == head[0] ? 1 : 0);
			if (matched == sizeof(head)) {
				matched = 0;
				after = 1;
			}
		}
	}
	return n;
}

static bool
bt_answer_vendor(const uint8_t *packet, uint32_t n)
{
	static uint32_t pass = 0xffffffffu;
	static uint32_t bt_vsc_answered;
	const uint8_t event[7] = { 0x04, 0x0e, 0x04, 0x01, packet[1], packet[2], 0x00 };

	if (pass == 0xffffffffu) {
		uint32_t answer = 1;

		if (!PE_parse_boot_argn("bt_vsc", &answer, sizeof(answer))) {
			(void)PE_parse_boot_argn("dadabt_vsc", &answer, sizeof(answer));
		}
		pass = answer == 0;
	}
	if (pass || n < 4 || packet[0] != 0x01) {
		return false;
	}
	if ((packet[2] & 0xfc) != 0xfc) {
		/*
		 * 0x204a (HCI_LE_Read_Periodic_Advertiser_List_Size) is in Bluetooth 5.0 Table 3.2 (Extended
		 * advertising commands); once WCN7850 receives it, it rejects Legacy advertising/scanning commands
		 * 0x2007 and 0x200b with status 0x0c (Command Disallowed). Answer 0x204a locally so WCN7850 stays in
		 * Legacy mode for bluetoothd's 0x2007/0x200b/0x200c sequence.
		 */
		static uint32_t fake[10] = { 0x0c5b, 0x0c5a, 0x204a };
		static int fake_read;
		uint32_t op = packet[1] | (uint32_t)packet[2] << 8;
		bool hit = false;

		if (!fake_read) {
			char list[96] = { 0 };
			char *q = list;
			unsigned k = 3;

			fake_read = 1;
			if (PE_parse_boot_arg_str("bt_fake", list, sizeof(list)) ||
			    PE_parse_boot_arg_str("dadabt_fake", list, sizeof(list))) {
				while (*q != 0 && k < 10) {
					fake[k++] = (uint32_t)strtoul(q, &q, 0);
					while (*q == ',') {
						q++;
					}
				}
			}
		}
		for (unsigned k = 0; k < 10; k++) {
			hit |= fake[k] != 0 && fake[k] == op;
		}
		if (!hit) {
			return false;
		}
	}
	if (packet[1] == 0x48 && packet[2] == 0xfc) {
		uint32_t old_baud = annibale_bt_uart_get_baud();
		uint32_t req_baud = (n > 4 && packet[4] == 0x0e) ? 3000000u : 115200u;
		bool chip_acked = false;

		bt_say("bt: baud transition: sending vendor command 0xFC48 (param %#x for %u baud, current %u)\n",
		    n > 4 ? packet[4] : 0, req_baud, old_baud);

		/* 1. 向物理芯片下发指令并检查返回值 */
		if (!bt_uart_send_locked(packet, n)) {
			bt_say("bt: stop: failed to transmit 0xFC48 command to chip\n");
			const uint8_t ev_err[7] = { 0x04, 0x0e, 0x04, 0x01, 0x48, 0xfc, 0x12 };
			lck_mtx_lock(bt_ring_lock);
			for (unsigned i = 0; i < sizeof(ev_err); i++) {
				bt_ring_put_byte(ev_err[i]);
			}
			wakeup(&bt_ring_in);
			lck_mtx_unlock(bt_ring_lock);
			selwakeup(&bt_sel);
			return false;
		}

		/* 2. 等待芯片以当前波特率回复 Command Complete 事件（最多等待 200 ms） */
		for (int t = 0; t < 200; t++) {
			delay_for_interval(1, NSEC_PER_MSEC);
			lck_mtx_lock(bt_ring_lock);
			bt_ring_fill();
			if (bt_ring_used() >= 7) {
				for (uint32_t pos = bt_ring_out; pos + 7 <= bt_ring_in; pos++) {
					if (bt_ring[pos % BT_RING] == 0x04 &&
					    bt_ring[(pos + 1) % BT_RING] == 0x0e &&
					    bt_ring[(pos + 4) % BT_RING] == 0x48 &&
					    bt_ring[(pos + 5) % BT_RING] == 0xfc) {
						chip_acked = (bt_ring[(pos + 6) % BT_RING] == 0x00);
						break;
					}
				}
			}
			lck_mtx_unlock(bt_ring_lock);
			if (chip_acked) {
				break;
			}
		}

		if (!chip_acked) {
			bt_say("bt: stop: WCN7850 chip did not acknowledge 0xFC48, aborting baud transition\n");
			const uint8_t ev_fail[7] = { 0x04, 0x0e, 0x04, 0x01, 0x48, 0xfc, 0x1f };
			lck_mtx_lock(bt_ring_lock);
			for (unsigned i = 0; i < sizeof(ev_fail); i++) {
				bt_ring_put_byte(ev_fail[i]);
			}
			wakeup(&bt_ring_in);
			lck_mtx_unlock(bt_ring_lock);
			selwakeup(&bt_sel);
			return false;
		}

		/* 3. 延时 10 ms 待物理总线稳定 */
		delay_for_interval(10, NSEC_PER_MSEC);

		/* 4. 切换主机串口时钟，失败时执行降级回滚 */
		if (!annibale_bt_uart_set_baud(req_baud)) {
			bt_say("bt: stop: host UART switch to %u failed; performing rollback to %u baud\n", req_baud, old_baud);
			(void)annibale_bt_uart_set_baud(old_baud ? old_baud : 115200u);
			return false;
		}

		delay_for_interval(10, NSEC_PER_MSEC);
		bt_say("bt: baud transition to %u completed successfully\n", req_baud);

		/* 5. 唤醒上层处理协议栈 */
		lck_mtx_lock(bt_ring_lock);
		if (bt_ring_used() != 0 && (btsk_up || bt_kctl_up)) {
			lck_mtx_unlock(bt_ring_lock);
			btsk_kick();
			lck_mtx_lock(bt_ring_lock);
		}
		wakeup(&bt_ring_in);
		lck_mtx_unlock(bt_ring_lock);
		selwakeup(&bt_sel);
		return true;
	}
	lck_mtx_lock(bt_ring_lock);
	{
		bool vendor = (packet[2] & 0xfc) == 0xfc;
		bool one_more = (packet[1] == 0x5a && packet[2] == 0x0c) ||
		    (packet[1] == 0x4a && packet[2] == 0x20);
		uint8_t ev[16];
		unsigned len;
		static uint32_t shorts[8];
		static int shorts_read;
		uint32_t op = packet[1] | (uint32_t)packet[2] << 8;

		if (!shorts_read) {
			char list[96] = { 0 };
			char *q = list;
			unsigned k = 0;

			shorts_read = 1;
			if (PE_parse_boot_arg_str("bt_short", list, sizeof(list)) ||
			    PE_parse_boot_arg_str("dadabt_short", list, sizeof(list))) {
				while (*q != 0 && k < 8) {
					shorts[k++] = (uint32_t)strtoul(q, &q, 0);
					while (*q == ',') {
						q++;
					}
				}
			}
		}
		for (unsigned k = 0; k < 8; k++) {
			if (shorts[k] != 0 && shorts[k] == op) {
				vendor = false;
			}
		}
		bzero(ev, sizeof(ev));
		memcpy(ev, event, sizeof(event));
		if (vendor) {
			ev[7] = n > 4 ? packet[4] : 0;
			len = 7 + 1 + 8;
		} else {
			len = 7 + (one_more ? 1 : 0);
		}
		ev[2] = (uint8_t)(len - 3);
		for (unsigned i = 0; i < len; i++) {
			bt_ring_put_byte(ev[i]);
		}
	}
	wakeup(&bt_ring_in);
	lck_mtx_unlock(bt_ring_lock);
	selwakeup(&bt_sel);
	if (bt_vsc_answered++ < 120) {
		bt_say("bt: iOS's command %#06x (%u bytes of parameters) answered here, not sent\n",
		    packet[1] | packet[2] << 8, packet[3]);
	}
	return true;
}

void
annibale_bt_uart_isr(void *ctx __unused, uint32_t intid)
{
	/* 1. 屏蔽 GIC 电平中断，防止工作线程运行期间产生中断风暴 */
	extern void gic_v3_spi_mask(uint32_t intid);
	gic_v3_spi_mask(intid);

	/* 2. 清除 GENI 硬件中断状态位 */
	annibale_bt_uart_irq_clear();

	/* 3. 中断安全地唤醒工作线程，严禁获取睡眠互斥锁 */
	os_atomic_inc(&bt_irq_event, relaxed);
	thread_wakeup((event_t)&bt_irq_event);
}

__dead2
static void
bt_pump_thread(void *arg __unused, wait_result_t w __unused)
{
	extern void gic_v3_spi_unmask(uint32_t intid);
	for (;;) {
		bool woke = false;

		/* 等待中断唤醒或 20 ms 保底超时轮询 */
		assert_wait_timeout((event_t)&bt_irq_event, THREAD_INTERRUPTIBLE, 20, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);

		lck_mtx_lock(bt_ring_lock);
		bt_ring_fill();
		if (bt_ring_used() != 0 && (btsk_up || bt_kctl_up)) {
			lck_mtx_unlock(bt_ring_lock);
			btsk_kick();
			lck_mtx_lock(bt_ring_lock);
		}
		if (bt_ring_used() != 0) {
			woke = true;
			wakeup(&bt_ring_in);
		}
		lck_mtx_unlock(bt_ring_lock);
		if (woke) {
			selwakeup(&bt_sel);
		}

		/* 处理完毕后重新使能 GIC 电平中断 */
		gic_v3_spi_unmask(32u + 0x1cdu);
	}
}

/* ---- Character devices (/dev/uart.bluetooth, /dev/cu.bluetooth, /dev/bt_h4) ---------------- */

static uint32_t bt_opens, bt_writes, bt_ioctls;

static int
bt_dev_open(dev_t dev __unused, int flags, int devtype __unused, struct proc *p __unused)
{
	int waited;

	if (bt_opens < 8) {
		bt_opens++;
		bt_say("bt: /dev/uart.bluetooth opened (flags %#x; chip %s)\n", flags, bt_serving ? "up" : "not up yet");
	}
	for (waited = 0; !bt_serving && waited < 40000; waited += 50) {
		ms(50);
	}
	return bt_serving ? 0 : ENXIO;
}

static int
bt_dev_close(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	return 0;
}

static int
bt_dev_read(dev_t dev __unused, struct uio *uio, int ioflag)
{
	uint8_t chunk[512];
	int error = 0;

	lck_mtx_lock(bt_ring_lock);
	while (bt_ring_used() == 0) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(bt_ring_lock);
			return EWOULDBLOCK;
		}
		error = msleep(&bt_ring_in, bt_ring_lock, PCATCH, "btread", NULL);
		if (error != 0) {
			lck_mtx_unlock(bt_ring_lock);
			return error == ERESTART ? 0 : error;
		}
	}
	lck_mtx_unlock(bt_ring_lock);
	while (uio_resid(uio) > 0 && error == 0) {
		user_ssize_t resid = uio_resid(uio);
		uint32_t want = resid > (user_ssize_t)sizeof(chunk) ? (uint32_t)sizeof(chunk) : (uint32_t)resid;
		uint32_t n = bt_ios_take(chunk, want);

		if (n == 0) {
			break;
		}
		error = uiomove((caddr_t)chunk, (int)n, uio);
	}
	return error;
}

static int
bt_dev_write(dev_t dev __unused, struct uio *uio, int ioflag __unused)
{
	uint8_t buf[512];
	int error = 0;

	while (uio_resid(uio) > 0 && error == 0) {
		user_ssize_t want = uio_resid(uio);
		uint32_t n = want > (user_ssize_t)sizeof(buf) ? (uint32_t)sizeof(buf) : (uint32_t)want;

		error = uiomove((caddr_t)buf, (int)n, uio);
		if (error != 0) {
			break;
		}
		if (bt_writes < 12) {
			bt_writes++;
			annibale_bt_hex(buf[0] == 1 ? "iOS writes a command" : buf[0] == 2 ? "iOS writes ACL" : "iOS writes",
			    buf, n < 16 ? n : 16);
		}
		if (bt_answer_vendor(buf, n)) {
			continue;
		}
		if (!bt_uart_send_locked(buf, n)) {
			error = EIO;
		}
	}
	return error;
}

static int
bt_dev_select(dev_t dev __unused, int which, void *wql, struct proc *p)
{
	int ready = 0;

	if (which == FREAD) {
		lck_mtx_lock(bt_ring_lock);
		if (bt_ring_used() != 0) {
			ready = 1;
		} else {
			selrecord(p, &bt_sel, wql);
		}
		lck_mtx_unlock(bt_ring_lock);
	} else if (which == FWRITE) {
		ready = 1;
	}
	return ready;
}

static int
bt_dev_ioctl(dev_t dev __unused, u_long command, caddr_t data __unused, int flags __unused, struct proc *p __unused)
{
	if (bt_ioctls < 16) {
		bt_ioctls++;
		bt_say("bt: ioctl %#lx\n", command);
	}
	switch (command) {
	case TIOCGETA:
	case TIOCSETA:
	case TIOCSETAW:
	case TIOCSETAF:
	case TIOCMGET:
	case TIOCMSET:
	case TIOCMBIS:
	case TIOCMBIC:
	case TIOCFLUSH:
	case TIOCEXCL:
	case TIOCNXCL:
	default:
		return 0;
	}
}

static const struct cdevsw bt_cdevsw = {
	.d_open = bt_dev_open, .d_close = bt_dev_close, .d_read = bt_dev_read, .d_write = bt_dev_write,
	.d_ioctl = bt_dev_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = bt_dev_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
annibale_bt_ios_early(void)
{
	static lck_grp_t *grp;
	int major;

	if (bt_ring != NULL) {
		return;
	}
	bt_ring = kalloc_data(BT_RING, Z_WAITOK | Z_ZERO);
	if (bt_ring == NULL) {
		return;
	}
	grp = lck_grp_alloc_init("bt-uart", LCK_GRP_ATTR_NULL);
	bt_ring_lock = lck_mtx_alloc_init(grp, LCK_ATTR_NULL);
	major = cdevsw_add(-1, &bt_cdevsw);
	if (major < 0) {
		bt_say("bt: cannot register the bluetooth cdev\n");
		return;
	}
	(void)devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cu.bluetooth");
	(void)devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "uart.bluetooth");
	(void)devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "bt_h4");
	bt_say("bt: /dev/cu.bluetooth, /dev/uart.bluetooth and /dev/bt_h4 made (opens wait for bt_ready)\n");
	RedwoodBTStart("uart.bluetooth", NULL);
	btsk_start();
	bt_kctl_register();
}

void
annibale_bt_ios_serve(const uint8_t addr[6])
{
	thread_t thread;

	annibale_bt_ios_early();
	if (bt_ring == NULL) {
		return;
	}
	if (addr != NULL && (addr[0] | addr[1] | addr[2] | addr[3] | addr[4] | addr[5]) != 0) {
		RedwoodBTSetAddress(addr);
	}
	lck_mtx_lock(bt_ring_lock);
	bt_ring_fill();
	bt_serving = 1;
	lck_mtx_unlock(bt_ring_lock);
	if (kernel_thread_start(bt_pump_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "bt-uart-pump");
		thread_deallocate(thread);
	}
	bt_say("bt: /dev/uart.bluetooth and com.apple.uart.sk.bluetooth serving; iOS's bluetoothd drives the chip now\n");
}

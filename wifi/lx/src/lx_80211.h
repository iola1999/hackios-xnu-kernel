/*
 * What the two halves of the 802.11 layer share (lx_80211.c: the radio, its channels, scans and
 * frames; lx_join.c: joining a network, its keys and block ack, and the station the kernel asks).
 */
#ifndef _LX_80211_H_
#define _LX_80211_H_

#include <linux/kernel.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

#include <lx_os.h>
#include "lx.h"
#include "lx_wpa.h"

#define HEARD_MOST	128
#define ELEMENTS_MOST	REDWOOD_STA_ELEMENTS_MOST

/* A network, as its beacons describe it. */
struct lx_heard {
	uint8_t sender[ETH_ALEN];		/* the access point's address (BSSID) */
	uint8_t name[32], name_length;
	uint16_t frequency;			/* MHz, of the channel it was heard on at its strongest */
	uint8_t says_channel;			/* the channel it says it is on (0: it does not say) */
	int8_t signal;				/* dBm, the strongest */
	uint16_t capability, interval;
	bool protected;				/* it announces WPA2 or later (an RSN element) */
	unsigned times;
	unsigned long long last;		/* when it was last heard: nanoseconds */
	uint16_t elements_length;		/* all it says of itself, as last heard (nothing, if that was too much to keep) */
	uint8_t elements[ELEMENTS_MOST];
};

/* The access point of the wanted network that was heard best, with all it says of itself. */
struct lx_target {
	bool found;
	uint8_t address[ETH_ALEN];
	uint16_t frequency, capability, interval;
	int8_t signal;
	unsigned elements_length;
	uint8_t elements[ELEMENTS_MOST];
};

/* What a management frame from the access point has answered (join() waits for these). */
enum { ANSWER_NONE, ANSWER_AUTHENTICATED, ANSWER_ASSOCIATED, ANSWER_REFUSED, ANSWER_GONE };

/* A radio: what stands behind the driver's "hw" and "wiphy". There is one. */
struct lx_radio {
	void *memory;				/* all of this was allocated as one piece, which begins here */
	struct wiphy *wiphy;
	struct ieee80211_hw hw;
	const struct ieee80211_ops *ops;
	bool registered;
	bool kept;				/* the channels' flags and powers as the driver set them have been put by */
	int applying;				/* a regulatory domain is being applied to the channels right now */
	struct workqueue_struct *work;		/* for what the driver has "the stack" run for it */
	struct linuxkpi_ieee80211_regdomain *regd;
	struct ieee80211_vif *vif;		/* the one interface, while it is up */
	void *vif_memory;
	bool started;

	/* a scan */
	volatile int scan;			/* 0: none, 1: running, 2: over, 3: given up by the driver */
	unsigned frames, others;		/* received in all, and of them neither beacon nor probe response */
	unsigned heard_count, heard_lost;
	struct lx_heard heard[HEARD_MOST];
	uint8_t want[32], want_length;		/* the name of the network that is wanted ... */
	unsigned width;				/* MHz of the channel joined: 20, or with wifi_wide=1 what the access point offers */
	struct lx_target target;		/* ... and the best of it */

	/* the network joined */
	struct ieee80211_sta *sta;		/* its access point, as the driver knows it */
	void *sta_memory;
	struct ieee80211_chanctx_conf *channel;
	struct cfg80211_bss bss;		/* what the driver may ask of the access point's beacon */
	struct cfg80211_bss_ies bss_elements;
	volatile int answer;
	uint16_t answer_status, aid;
	unsigned answer_length;
	uint8_t answer_elements[ELEMENTS_MOST];	/* of the association response */
	uint8_t rsn[24];			/* our RSN element, as the association request has it */
	struct lx_wpa wpa;
	volatile int handshake;			/* the last thing lx_wpa_message() said */
	const char *handshake_why;
	unsigned pending_eapol_len;
	uint8_t pending_eapol[256];
	struct work_struct group_work;		/* a new group key, later on */
	/* block ack: the access point's requests to send us frames in aggregates, by traffic class (TID) */
	struct work_struct ba_work;
	volatile uint8_t ba_asked;		/* TIDs with a request (ADDBA or DELBA) not yet dealt with */
	struct {
		bool start;			/* ADDBA (else DELBA) */
		bool active;			/* the driver takes aggregates of it */
		bool amsdu;
		uint8_t token;
		uint16_t ssn, size, timeout;
	} ba[8];
	unsigned ba_sessions, ba_refused;
	struct ieee80211_key_conf *pairwise_key, *group_key;
	volatile bool joined;			/* keys set: frames pass */
	volatile bool joining;
	int reached;				/* how far the access point has come as the driver's station (IEEE80211_STA_...) */
	int8_t signal;				/* dBm, of its last frame: an answer while joining, or a beacon */
	unsigned beacons;			/* its beacons heard since the join began ... */
	int8_t weakest;				/* ... and the weakest of its frames since then, dBm (0: none yet) */
	int last_error;
	unsigned sent, sent_lost, received, received_group, eapol, rekeys;
	unsigned long long joined_at;		/* nanoseconds */
};

extern struct lx_radio *the_radio;		/* the one radio, while it is registered */
extern const uint8_t no_network[ETH_ALEN];	/* the address of the network an interface has joined, while it has joined none */
extern const uint8_t wmm[4];			/* the start of a vendor element: Microsoft's, which is how WMM is announced */

struct lx_radio *lx_radio_of(struct ieee80211_hw *hw);
unsigned lx_channel_of(unsigned frequency);		/* the channel's number, of its frequency in MHz */
/* an element of a frame's elements by its number (and a vendor's by the bytes it begins with); NULL if it is not there */
const uint8_t *lx_element(const uint8_t *elements, unsigned length, unsigned number);
const uint8_t *lx_vendor_element(const uint8_t *elements, unsigned length, const uint8_t *begins);
/* frames to send: a buffer, a management frame's header in it, the buffer to the driver; an Ethernet frame to the network */
struct sk_buff *lx_frame_new(struct lx_radio *radio, unsigned length, uint8_t **bytes);
uint8_t *lx_management(struct lx_radio *radio, uint8_t *frame, unsigned kind);
void lx_frame_send(struct lx_radio *radio, struct sk_buff *skb);
bool lx_ethernet_send(struct lx_radio *radio, const uint8_t *frame, unsigned length);
void lx_handshake_message(struct lx_radio *radio, const uint8_t *frame, unsigned length);
/* the interface in station mode, up and down; a scan with it */
int lx_station_up(struct lx_radio *radio);
void lx_station_down(struct lx_radio *radio);
int lx_scan(struct lx_radio *radio, const uint8_t *name, unsigned name_length, bool quiet, const struct redwood_sta_scan *what);

#endif

/*
 * Where the driver meets 802.11 itself: under Linux that is mac80211 and cfg80211, under FreeBSD
 * LinuxKPI's stand-in for them on top of net80211, and here it is ours -- the functions that
 * LinuxKPI's headers (net/mac80211.h, net/cfg80211.h) declare under linuxkpi_ names, as far as
 * the driver calls them, and a station: the layer that asks the driver for things, in
 * mac80211's order (FreeBSD's linux_80211.c shows it).
 *
 * What is here: the radio as the driver describes it when it registers; the regulatory domain
 * the driver hands over, applied to the radio's channels the way cfg80211 applies it; one
 * interface in station mode; a scan of every channel the domain allows; and the joining of one
 * network that is protected with WPA2 and a passphrase -- found by its name, authenticated to
 * (open system), associated with, the keys agreed in the four-way handshake (lx_wpa.c) and
 * given to the driver --, after which Ethernet frames pass between the kernel and the driver
 * (the firmware puts them into 802.11 and ciphers them: the driver's "frame mode" Ethernet).
 *
 * What is not: more than one network, roaming, power saving, 40 MHz and wider channels, HE,
 * protected management frames, and the check of the packet numbers of the group's frames that
 * the driver leaves to this layer.
 *
 * On the phone, 13 October, this scans -- 26 channels in 4.1 s, the access points around heard
 * with their channels and strengths -- and joins: the handshake is through 130 ms after the
 * association, and iOS has the network (bsd/dev/redwood_wifi.c, step_driver(), has the
 * account). What is said of other people's networks names them: it is for the log of the run.
 * The name of the network to join and its passphrase are never said, only their lengths.
 */
#include "lx_80211.h"

int linuxkpi_debug_80211;			/* (which of the headers' own remarks are printed: none) */

struct lx_radio *the_radio;
const uint8_t no_network[ETH_ALEN];

struct lx_radio *
lx_radio_of(struct ieee80211_hw *hw)
{
	return (__containerof(hw, struct lx_radio, hw));
}

/* ---- the radio, as the driver makes and registers it -------------------------------------------- */

struct ieee80211_hw *
linuxkpi_ieee80211_alloc_hw(size_t private, const struct ieee80211_ops *ops)
{
	size_t ours = roundup2(sizeof(struct lx_radio), 64);
	uint8_t *memory = kzalloc(64 + offsetof(struct wiphy, priv) + ours + private, GFP_KERNEL);
	struct lx_radio *radio;
	struct wiphy *wiphy;

	if (memory == NULL) {
		return (NULL);
	}
	wiphy = (struct wiphy *)roundup2((uintptr_t)memory, 64);
	radio = (struct lx_radio *)wiphy->priv;
	radio->memory = memory;
	radio->wiphy = wiphy;
	radio->ops = ops;
	radio->hw.wiphy = wiphy;
	radio->hw.priv = (uint8_t *)radio + ours;
	radio->work = alloc_ordered_workqueue("wifi-80211", 0);
	mutex_init(&wiphy->mtx);
	return (&radio->hw);
}

struct ieee80211_hw *
linuxkpi_wiphy_to_ieee80211_hw(struct wiphy *wiphy)
{
	return (&((struct lx_radio *)wiphy->priv)->hw);
}

void
linuxkpi_set_ieee80211_dev(struct ieee80211_hw *hw)
{
}

int
linuxkpi_80211_wiphy_register(struct wiphy *wiphy)
{
	return (0);
}

/* ieee80211_register_hw(): the driver has described its radio, and from here on may be asked for things. */
int
linuxkpi_ieee80211_ifattach(struct ieee80211_hw *hw)
{
	struct lx_radio *radio = lx_radio_of(hw);
	char bands[96] = "";

	/*
	 * ath12k takes the radio's address only from the firmware's ready event (ath11k asks the device first, which
	 * is the kernel's wifi_mac), and the WCN7850's firmware on this phone has none: the kernel's, then.
	 */
	if (is_zero_ether_addr(hw->wiphy->perm_addr)) {
		(void)lx_os_radio_address(hw->wiphy->perm_addr);
	}
	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		if (hw->wiphy->bands[band] != NULL) {
			snprintf(bands + strlen(bands), sizeof(bands) - strlen(bands), " %d in band %u,", hw->wiphy->bands[band]->n_channels,
			    band);
		}
	}
	printf("lx: the driver registers a radio: channels:%s (0: 2.4 GHz, 1: 5 GHz, 3: 6 GHz); %s; up to %d names and %u "
	    "bytes of extras in a scan\n", bands, is_zero_ether_addr(hw->wiphy->perm_addr) ? "IT HAS NO ADDRESS" :
	    "it has an address", hw->wiphy->max_scan_ssids, hw->wiphy->max_scan_ie_len);
	radio->registered = true;
	the_radio = radio;
	return (0);
}

/* The driver takes its radio back, and frees the channels next: nobody may be at them when this returns. */
void
linuxkpi_ieee80211_unregister_hw(struct ieee80211_hw *hw)
{
	struct lx_radio *radio = lx_radio_of(hw);

	lx_os_lock();
	radio->registered = false;
	while (radio->applying != 0) {
		lx_os_sleep(&radio->applying, 5 * 1000000ull);
	}
	if (the_radio == radio) {
		the_radio = NULL;
	}
	lx_os_unlock();
}

void
linuxkpi_ieee80211_iffree(struct ieee80211_hw *hw)
{
	struct lx_radio *radio = lx_radio_of(hw);

	if (radio->work != NULL) {
		destroy_workqueue(radio->work);
		radio->work = NULL;
	}
}

void
linuxkpi_wiphy_free(struct wiphy *wiphy)
{
	struct lx_radio *radio = (struct lx_radio *)wiphy->priv;

	kfree(radio->regd);
	kfree(radio->memory);
}

/* ---- the regulatory domain ------------------------------------------------------------------------ */

/*
 * What cfg80211 does with a domain (Linux: net/wireless/reg.c, handle_channel()): a channel
 * may be used if a rule covers its 20 MHz, and takes the rule's limits; otherwise it is
 * disabled. The driver reads the result off the channels when it tells the firmware which it
 * may use (reg.c: ath11k_reg_update_chan_list()), and so does the scan below.
 */
static void
regd_apply(struct lx_radio *radio)
{
	const struct linuxkpi_ieee80211_regdomain *regd = radio->regd;
	char said[160] = "";

	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *has = radio->wiphy->bands[band];
		int allowed = 0;

		for (int i = 0; has != NULL && i < has->n_channels; i++) {
			struct linuxkpi_ieee80211_channel *channel = &has->channels[i];
			const struct ieee80211_reg_rule *rule = NULL;

			if (!radio->kept) {
				channel->orig_flags = channel->flags;
				channel->orig_mpwr = channel->max_power;
			}
			for (int r = 0; r < regd->n_reg_rules && rule == NULL; r++) {
				if (regd->reg_rules[r].freq_range.start_freq_khz <= ((int)channel->center_freq - 10) * 1000 &&
				    regd->reg_rules[r].freq_range.end_freq_khz >= ((int)channel->center_freq + 10) * 1000) {
					rule = &regd->reg_rules[r];
				}
			}
			channel->flags = channel->orig_flags & ~(IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR);
			if (rule == NULL) {
				channel->flags |= IEEE80211_CHAN_DISABLED;
				continue;
			}
			if (rule->flags & NL80211_RRF_NO_IR) {
				channel->flags |= IEEE80211_CHAN_NO_IR;
			}
			if (rule->flags & NL80211_RRF_DFS) {
				channel->flags |= IEEE80211_CHAN_RADAR;
			}
			channel->max_reg_power = rule->power_rule.max_eirp / 100;	/* (hundredths of a dBm) */
			channel->max_antenna_gain = rule->power_rule.max_antenna_gain / 100;
			channel->max_power = channel->orig_mpwr != 0 && channel->orig_mpwr < channel->max_reg_power ?
			    channel->orig_mpwr : channel->max_reg_power;
			allowed++;
		}
		if (has != NULL) {
			snprintf(said + strlen(said), sizeof(said) - strlen(said), " %d of %d in band %u,", allowed, has->n_channels, band);
		}
	}
	radio->kept = true;
	printf("lx: the regulatory domain \"%c%c\" (%d rules) allows the radio's channels:%s\n", regd->alpha2[0], regd->alpha2[1],
	    regd->n_reg_rules, said);
}

/*
 * The driver's domain for its radio (which it has from the firmware). It keeps its own copy and
 * may free it. The driver calls this from work of its own, one call at a time, and may be
 * waiting for that work to end while it holds the radio's lock: so that lock is not taken here.
 * A domain that comes when the radio is no longer registered is not applied (the firmware sends
 * one whenever the country changes, also while the driver is on its way down).
 */
int
linuxkpi_regulatory_set_wiphy_regd_sync(struct wiphy *wiphy, struct linuxkpi_ieee80211_regdomain *regd)
{
	struct lx_radio *radio = (struct lx_radio *)wiphy->priv;
	struct linuxkpi_ieee80211_regdomain *copy, *old;
	size_t size;

	if (regd == NULL) {
		return (-EINVAL);
	}
	size = sizeof(*regd) + regd->n_reg_rules * sizeof(regd->reg_rules[0]);
	copy = kmalloc(size, GFP_KERNEL);
	if (copy == NULL) {
		return (-ENOMEM);
	}
	memcpy(copy, regd, size);
	lx_os_lock();
	if (!radio->registered) {
		lx_os_unlock();
		kfree(copy);
		return (-ENODEV);
	}
	radio->applying++;
	lx_os_unlock();
	old = radio->regd;
	radio->regd = copy;
	wiphy->regd = copy;
	regd_apply(radio);
	lx_os_lock();
	radio->applying--;
	lx_os_wakeup(&radio->applying);
	lx_os_unlock();
	kfree(old);
	return (0);
}

/* IEEE 802.11-2020, Annex E: a channel's centre, in MHz */
uint32_t
linuxkpi_ieee80211_channel_to_frequency(uint32_t channel, enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return (channel == 14 ? 2484 : 2407 + 5 * channel);
	case NL80211_BAND_5GHZ:
		return (channel >= 182 && channel <= 196 ? 4000 + 5 * channel : 5000 + 5 * channel);
	case NL80211_BAND_6GHZ:
		return (channel == 2 ? 5935 : 5950 + 5 * channel);
	default:
		return (0);
	}
}

unsigned
lx_channel_of(unsigned frequency)
{
	return (frequency == 2484 ? 14 : frequency < 2484 ? (frequency - 2407) / 5 : frequency < 5950 ? (frequency - 5000) / 5 :
	       (frequency - 5950) / 5);
}

uint32_t
linuxkpi_ieee80211_frequency_to_channel(uint32_t frequency, uint32_t flags)
{
	return (lx_channel_of(frequency));
}

/* The channel of that frequency among those the radio was registered with. */
struct linuxkpi_ieee80211_channel *
linuxkpi_ieee80211_get_channel(struct wiphy *wiphy, uint32_t frequency)
{
	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *has = wiphy->bands[band];

		for (int i = 0; has != NULL && i < has->n_channels; i++) {
			if (has->channels[i].center_freq == frequency) {
				return (&has->channels[i]);
			}
		}
	}
	return (NULL);
}

/* ---- what the driver has the layer above do for it ------------------------------------------------- */

void
linuxkpi_ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work)
{
	queue_work(lx_radio_of(hw)->work, work);
}

void
linuxkpi_ieee80211_queue_delayed_work(struct ieee80211_hw *hw, struct delayed_work *work, int delay)
{
	queue_delayed_work(lx_radio_of(hw)->work, work, delay);
}

/* (flags: all interfaces, or those that are up; here the one there is is up for as long as it is there) */
void
linuxkpi_ieee80211_iterate_interfaces(struct ieee80211_hw *hw, enum ieee80211_iface_iter flags,
    void (*function)(void *, uint8_t *, struct ieee80211_vif *), void *argument)
{
	struct ieee80211_vif *vif = lx_radio_of(hw)->vif;

	if (vif != NULL) {
		function(argument, vif->addr, vif);
	}
}

/* (no network has been joined: there are no stations and no channel is held) */
void
linuxkpi_ieee80211_iterate_stations_atomic(struct ieee80211_hw *hw, void (*function)(void *, struct ieee80211_sta *), void *argument)
{
}

void
linuxkpi_ieee80211_iterate_chan_contexts(struct ieee80211_hw *hw,
    void (*function)(struct ieee80211_hw *, struct ieee80211_chanctx_conf *, void *), void *argument)
{
}

/* The one station there can be: the access point of the network being joined. */
struct ieee80211_sta *
linuxkpi_ieee80211_find_sta(struct ieee80211_vif *vif, const u8 *address)
{
	struct lx_radio *radio = the_radio;

	return (radio != NULL && radio->sta != NULL && memcmp(radio->sta->addr, address, ETH_ALEN) == 0 ? radio->sta : NULL);
}

struct ieee80211_sta *
linuxkpi_ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const uint8_t *address, const uint8_t *own)
{
	struct lx_radio *radio = lx_radio_of(hw);

	return (radio->sta != NULL && memcmp(radio->sta->addr, address, ETH_ALEN) == 0 ? radio->sta : NULL);
}

/*
 * What a scan has learnt of an access point. The driver asks this of the one it is associating
 * with, to see whether that announces RSN: it then tells the firmware to let nothing but the
 * handshake through until the keys are set (mac.c: ath11k_peer_assoc_h_crypto()).
 */
struct cfg80211_bss *
linuxkpi_cfg80211_get_bss(struct wiphy *wiphy, struct linuxkpi_ieee80211_channel *channel, const uint8_t *bssid,
    const uint8_t *ssid, size_t ssid_length, enum ieee80211_bss_type type, enum ieee80211_privacy privacy)
{
	struct lx_radio *radio = (struct lx_radio *)wiphy->priv;

	if (!radio->target.found || bssid == NULL || memcmp(radio->target.address, bssid, ETH_ALEN) != 0) {
		return (NULL);
	}
	radio->bss_elements.data = radio->target.elements;
	radio->bss_elements.len = radio->target.elements_length;
	radio->bss.ies = radio->bss.beacon_ies = &radio->bss_elements;
	radio->bss.signal = radio->target.signal * 100;
	return (&radio->bss);
}

void
linuxkpi_cfg80211_put_bss(struct wiphy *wiphy, struct cfg80211_bss *bss)
{
}

/* (nothing is queued for sending: there is nothing to wake, and nothing sent to account for) */
void
linuxkpi_ieee80211_wake_queues(struct ieee80211_hw *hw)
{
}

void
linuxkpi_ieee80211_stop_queues(struct ieee80211_hw *hw)
{
}

void
linuxkpi_ieee80211_handle_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *queue)
{
}

/*
 * What became of our own messages of the handshake: an access point that sends message 1 again has not taken
 * message 2, and only the driver's report tells whether the frame was lost on the air (not acknowledged) or
 * arrived and was refused. The driver may have taken the QoS field out by now, so the LLC/SNAP header with the
 * type 0x888e is looked for where either header would leave it.
 */
static void
handshake_sent(const struct sk_buff *skb, const char *what)
{
	static const uint8_t eapol[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e };

	if (skb == NULL || skb->len < 34 || (skb->data[0] & 0x0c) != 0x08 || (skb->data[1] & 0x40) != 0) {
		return;
	}
	if (memcmp(skb->data + 24, eapol, sizeof(eapol)) == 0 || memcmp(skb->data + 26, eapol, sizeof(eapol)) == 0) {
		printf("lx: the handshake: our message of %u bytes %s\n", skb->len, what);
	}
}

void
linuxkpi_ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	handshake_sent(skb, IEEE80211_SKB_CB(skb)->flags & IEEE80211_TX_STAT_ACK ? "was acknowledged" :
	    "WAS NOT ACKNOWLEDGED");
	dev_kfree_skb_any(skb);
}

void
linuxkpi_ieee80211_tx_status_ext(struct ieee80211_hw *hw, struct ieee80211_tx_status *status)
{
	if (status->skb != NULL) {
		handshake_sent(status->skb, status->info != NULL && (status->info->flags & IEEE80211_TX_STAT_ACK) ?
		    "was acknowledged" : "WAS NOT ACKNOWLEDGED");
		dev_kfree_skb_any(status->skb);
	}
}

void
linuxkpi_ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb, int status)
{
	handshake_sent(skb, "WAS DROPPED by the driver");
	dev_kfree_skb_any(skb);
}

void
linuxkpi_ieee80211_beacon_loss(struct ieee80211_vif *vif)
{
	printf("lx: the driver reports beacons lost\n");
}

void
linuxkpi_ieee80211_connection_loss(struct ieee80211_vif *vif)
{
	printf("lx: the driver reports the connection lost\n");
}

/* (the driver's recovery after its firmware has died, which nothing here goes through) */
void
linuxkpi_ieee80211_restart_hw(struct ieee80211_hw *hw)
{
}

/* ---- elements --------------------------------------------------------------------------------------- */

/* The element of that number among `length` bytes of elements (number, length, contents; one after another), or NULL. */
const uint8_t *
lx_element(const uint8_t *elements, unsigned length, unsigned number)
{
	for (unsigned at = 0; at + 2 <= length && at + 2 + elements[at + 1] <= length; at += 2 + elements[at + 1]) {
		if (elements[at] == number) {
			return (elements + at);
		}
	}
	return (NULL);
}

/* A vendor's element (221) that begins with these four bytes. */
const uint8_t *
lx_vendor_element(const uint8_t *elements, unsigned length, const uint8_t *begins)
{
	for (unsigned at = 0; at + 2 <= length && at + 2 + elements[at + 1] <= length; at += 2 + elements[at + 1]) {
		if (elements[at] == 221 && elements[at + 1] >= 4 && memcmp(elements + at + 2, begins, 4) == 0) {
			return (elements + at);
		}
	}
	return (NULL);
}

const uint8_t wmm[4] = { 0x00, 0x50, 0xf2, 0x02 };

/* ---- what the radio hears -------------------------------------------------------------------------- */

/*
 * A beacon or an answer to a probe (IEEE 802.11-2020, 9.3.3): after the 24 bytes every
 * management frame begins with -- the sender's network is the third address -- a time stamp, the
 * beacon interval, the capabilities, and then elements, of which the name (0), the channel the
 * sender says it is on (3) and whether it announces RSN (48) are taken. Of the network that is
 * wanted, the access point heard best is kept whole.
 */
static void
heard(struct lx_radio *radio, const uint8_t *frame, unsigned length, unsigned frequency, int signal)
{
	struct lx_heard *network = NULL;
	const uint8_t *elements = frame + 36, *name = lx_element(elements, length - 36, 0);

	lx_os_lock();
	if (name != NULL && radio->want_length != 0 && name[1] == radio->want_length &&
	    memcmp(name + 2, radio->want, radio->want_length) == 0 && (!radio->target.found || signal > radio->target.signal) &&
	    length - 36 <= ELEMENTS_MOST && radio->sta == NULL) {
		radio->target.found = true;
		memcpy(radio->target.address, frame + 16, ETH_ALEN);
		radio->target.frequency = (uint16_t)frequency;
		radio->target.interval = (uint16_t)(frame[32] | frame[33] << 8);
		radio->target.capability = (uint16_t)(frame[34] | frame[35] << 8);
		radio->target.signal = (int8_t)signal;
		radio->target.elements_length = length - 36;
		memcpy(radio->target.elements, elements, length - 36);
	}
	for (unsigned i = 0; i < radio->heard_count && network == NULL; i++) {
		if (memcmp(radio->heard[i].sender, frame + 16, ETH_ALEN) == 0) {
			network = &radio->heard[i];
		}
	}
	if (network == NULL) {
		if (radio->heard_count == HEARD_MOST) {
			radio->heard_lost++;
			lx_os_unlock();
			return;
		}
		network = &radio->heard[radio->heard_count++];
		bzero(network, sizeof(*network));
		memcpy(network->sender, frame + 16, ETH_ALEN);
		network->signal = -128;
	}
	network->times++;
	network->last = lx_os_nanoseconds();
	network->interval = (uint16_t)(frame[32] | frame[33] << 8);
	network->capability = (uint16_t)(frame[34] | frame[35] << 8);
	if (length - 36 <= ELEMENTS_MOST) {
		network->elements_length = (uint16_t)(length - 36);
		memcpy(network->elements, elements, length - 36);
	}
	if (radio->sta != NULL && memcmp(radio->target.address, frame + 16, ETH_ALEN) == 0) {
		radio->signal = (int8_t)signal;
		radio->weakest = signal < radio->weakest ? (int8_t)signal : radio->weakest;
		radio->beacons++;
	}
	if (signal > network->signal) {
		network->signal = (int8_t)signal;
		network->frequency = (uint16_t)frequency;
	}
	if (name != NULL && name[1] <= sizeof(network->name) && (name[1] != 0 || network->name_length == 0)) {
		network->name_length = name[1];
		memcpy(network->name, name + 2, name[1]);
	}
	if (lx_element(elements, length - 36, 3) != NULL && lx_element(elements, length - 36, 3)[1] == 1) {
		network->says_channel = lx_element(elements, length - 36, 3)[2];
	}
	network->protected = lx_element(elements, length - 36, 48) != NULL;
	lx_os_unlock();
}

/* The driver's word that the scan it was asked for is over. */
void
linuxkpi_ieee80211_scan_completed(struct ieee80211_hw *hw, struct cfg80211_scan_info *info)
{
	struct lx_radio *radio = lx_radio_of(hw);

	lx_os_lock();
	radio->scan = info != NULL && info->aborted ? 3 : 2;
	lx_os_wakeup(&radio->scan);
	lx_os_unlock();
}

/* What was heard, the strongest first. Names and addresses of other people's networks: for the log of the run only. */
static void
say_heard(struct lx_radio *radio)
{
	unsigned on_channel[200] = { 0 }, low = 0, high = 0, open = 0, nameless = 0;
	int strongest = -128, weakest = 127;
	char line[400] = "";
	bool said[HEARD_MOST] = { false };

	for (unsigned n = 0; n < radio->heard_count; n++) {
		const struct lx_heard *network = NULL;
		char name[33];
		unsigned which = 0, channel;
		bool wanted;

		for (unsigned i = 0; i < radio->heard_count; i++) {
			if (!said[i] && (network == NULL || radio->heard[i].signal > network->signal)) {
				network = &radio->heard[i];
				which = i;
			}
		}
		said[which] = true;
		for (unsigned i = 0; i < network->name_length; i++) {
			name[i] = network->name[i] >= 0x20 && network->name[i] < 0x7f ? (char)network->name[i] : '?';
		}
		name[network->name_length] = 0;
		channel = lx_channel_of(network->frequency);
		if (channel < nitems(on_channel)) {
			on_channel[channel]++;
		}
		*(network->frequency < 4000 ? &low : &high) += 1;
		open += !(network->capability & 0x10);
		nameless += network->name_length == 0 || network->name[0] == 0;
		strongest = network->signal > strongest ? network->signal : strongest;
		weakest = network->signal < weakest ? network->signal : weakest;
		/* (the network that is wanted is the user's own: its name is not said even here) */
		wanted = radio->want_length != 0 && network->name_length == radio->want_length &&
		    memcmp(network->name, radio->want, radio->want_length) == 0;
		printf("lx: heard: channel %3u (%u MHz) %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x  %s, %u times  \"%s\"\n", channel,
		    network->frequency, network->signal, network->sender[0], network->sender[1], network->sender[2], network->sender[3],
		    network->sender[4], network->sender[5], !(network->capability & 0x10) ? "open" : network->protected ? "RSN" :
		    "protected", network->times, wanted ? "(the network that is wanted)" : name);
	}
	for (unsigned channel = 0; channel < nitems(on_channel); channel++) {
		if (on_channel[channel] != 0) {
			snprintf(line + strlen(line), sizeof(line) - strlen(line), " %u on %u,", on_channel[channel], channel);
		}
	}
	printf("lx: THE SCAN HEARD %u NETWORKS (access points): %u at 2.4 GHz, %u at 5 GHz; by channel:%s the strongest at %d dBm, "
	    "the weakest at %d; %u open, %u without a name; %u frames in all, %u of them neither beacon nor answer to a probe%s\n",
	    radio->heard_count, low, high, line, radio->heard_count ? strongest : 0, radio->heard_count ? weakest : 0, open, nameless,
	    radio->frames, radio->others, radio->heard_lost ? "; MORE WERE HEARD THAN THERE WAS ROOM FOR" : "");
}

/* ---- frames ------------------------------------------------------------------------------------------ */

/* A frame for the driver to send: `length` bytes at the returned place, in a buffer the radio can read. */
struct sk_buff *
lx_frame_new(struct lx_radio *radio, unsigned length, uint8_t **bytes)
{
	struct sk_buff *skb = dev_alloc_skb(length + 256);
	struct ieee80211_tx_info *info;

	if (skb == NULL) {
		return (NULL);
	}
	skb_reserve(skb, 128);			/* (room in front for what the driver puts there) */
	*bytes = skb_put(skb, length);
	info = IEEE80211_SKB_CB(skb);
	bzero(info, sizeof(*info));
	info->control.vif = radio->vif;
	info->band = radio->channel != NULL ? radio->channel->def.chan->band : NL80211_BAND_2GHZ;
	return (skb);
}

void
lx_frame_send(struct lx_radio *radio, struct sk_buff *skb)
{
	struct ieee80211_tx_control control = { .sta = radio->sta };

	radio->ops->tx(&radio->hw, &control, skb);
}

/* The 24 bytes a management frame to the access point begins with (the kind: 0xb0 authentication, 0x00 association request, ...). */
uint8_t *
lx_management(struct lx_radio *radio, uint8_t *frame, unsigned kind)
{
	bzero(frame, 24);
	frame[0] = (uint8_t)kind;
	memcpy(frame + 4, radio->target.address, ETH_ALEN);
	memcpy(frame + 10, radio->vif->addr, ETH_ALEN);
	memcpy(frame + 16, radio->target.address, ETH_ALEN);
	return (frame + 24);
}

/*
 * An Ethernet frame, through the driver's data path. When the interface is in the firmware's Ethernet mode
 * (IEEE80211_OFFLOAD_ENCAP_ENABLED) the firmware makes 802.11 of it, and ciphers it once there is a key. Otherwise it
 * goes as mac80211 sends one: a data frame to the access point (QoS data if it has WMM: TID 7 for the handshake, 0 for
 * the rest), LLC and SNAP before the type, marked protected once there is a key, which the hardware applies. ath12k's
 * Wi-Fi 7 descriptors take the frame's kind from the interface and not from the frame, and the WCN7850's firmware on
 * this phone offers no Ethernet mode.
 */
bool
lx_ethernet_send(struct lx_radio *radio, const uint8_t *frame, unsigned length)
{
	static const uint8_t llc_snap[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
	bool ethernet = radio->vif != NULL && (radio->vif->offload_flags & IEEE80211_OFFLOAD_ENCAP_ENABLED);
	bool qos = radio->sta != NULL && radio->sta->wme;
	unsigned header = qos ? 26 : 24;
	struct sk_buff *skb;
	uint8_t *bytes;

	if (radio->vif == NULL || radio->sta == NULL || length < 14 ||
	    (skb = lx_frame_new(radio, ethernet ? length : header + 8 + length - 12, &bytes)) == NULL) {
		radio->sent_lost++;
		return (false);
	}
	if (ethernet) {
		memcpy(bytes, frame, length);
		IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_CTL_HW_80211_ENCAP;
	} else {
		bzero(bytes, header);
		bytes[0] = qos ? 0x88 : 0x08;
		bytes[1] = 0x01 | (radio->pairwise_key != NULL ? 0x40 : 0);	/* to the access point; protected */
		memcpy(bytes + 4, radio->target.address, ETH_ALEN);
		memcpy(bytes + 10, radio->vif->addr, ETH_ALEN);
		memcpy(bytes + 16, frame, ETH_ALEN);
		if (qos) {
			skb->priority = frame[12] == 0x88 && frame[13] == 0x8e ? 7 : 0;
			bytes[24] = (uint8_t)skb->priority;
		}
		memcpy(bytes + header, llc_snap, sizeof(llc_snap));
		memcpy(bytes + header + 6, frame + 12, length - 12);
	}
	IEEE80211_SKB_CB(skb)->control.hw_key = radio->pairwise_key;
	radio->sent++;
	lx_frame_send(radio, skb);
	return (true);
}

/*
 * An 802.11 data frame made into the Ethernet frame it carries, in place: the driver hands two
 * kinds over in 802.11 although the firmware had them as Ethernet -- the handshake's, and what
 * was sent to everybody and deciphered (dp_rx.c: ath11k_dp_rx_h_undecap()). The header is 24
 * bytes, more with a fourth address, the QoS field, the HT field and, if the driver left it
 * there, the cipher's 8 bytes; then 8 bytes of LLC and SNAP whose last two are the type.
 */
static bool
to_ethernet(struct sk_buff *skb, bool cipher_header_gone)
{
	const uint8_t *f = skb->data;
	unsigned control = f[0] | f[1] << 8, header = 24;
	uint8_t to[ETH_ALEN], from[ETH_ALEN], type[2], *e;

	if ((control & 0x000c) != 0x0008 || skb->len < 32) {
		return (false);
	}
	memcpy(to, control & 0x0100 ? f + 16 : f + 4, ETH_ALEN);
	memcpy(from, (control & 0x0300) == 0x0300 ? f + 24 : control & 0x0200 ? f + 16 : f + 10, ETH_ALEN);
	header += (control & 0x0300) == 0x0300 ? 6 : 0;
	header += control & 0x0080 ? 2 : 0;
	header += (control & 0x8080) == 0x8080 ? 4 : 0;
	header += (control & 0x4000) && !cipher_header_gone ? 8 : 0;
	if (skb->len < header + 8 || f[header] != 0xaa || f[header + 1] != 0xaa || f[header + 2] != 0x03) {
		return (false);
	}
	memcpy(type, f + header + 6, 2);
	e = skb_pull(skb, header + 8 - 14);
	memcpy(e, to, ETH_ALEN);
	memcpy(e + 6, from, ETH_ALEN);
	memcpy(e + 12, type, 2);
	return (true);
}

/* A message of the handshake from the access point: answered at once; the keys are for join() to give to the driver. */
void
lx_handshake_message(struct lx_radio *radio, const uint8_t *frame, unsigned length)
{
	uint8_t reply[14 + LX_WPA_REPLY_MOST];
	unsigned reply_length = 0;
	const char *why = "";
	int said;

	radio->eapol++;
	if (radio->sta == NULL || memcmp(frame + 6, radio->target.address, ETH_ALEN) != 0) {
		return;
	}
	said = lx_wpa_message(&radio->wpa, frame + 14, length - 14, reply + 14, &reply_length, &why);
	if (reply_length != 0) {
		memcpy(reply, radio->target.address, ETH_ALEN);
		memcpy(reply + 6, radio->vif->addr, ETH_ALEN);
		reply[12] = 0x88;
		reply[13] = 0x8e;
		lx_ethernet_send(radio, reply, 14 + reply_length);
	}
	printf("lx: the handshake: a message of %u bytes from the access point: %s%s\n", length - 14, said == LX_WPA_ANSWER ?
	    "its first; answered with ours (message 2)" : said == LX_WPA_KEYS ? "its second, SIGNED WITH THE KEY WE HAVE TOO, and "
	    "with the group's key; answered (message 4)" : said == LX_WPA_GROUP ? "a new key for the group; answered" :
	    said == LX_WPA_BAD ? "NOT GOOD: " : "not one to answer", why);
	if (said == LX_WPA_GROUP && radio->joined) {
		queue_work(radio->work, &radio->group_work);
	}
	if (said != LX_WPA_NOTHING) {
		lx_os_lock();
		radio->handshake = said;
		radio->handshake_why = why;
		lx_os_wakeup(&radio->handshake);
		lx_os_unlock();
	}
}

/* An Ethernet frame the radio has received. */
static void
ethernet_received(struct lx_radio *radio, struct sk_buff *skb)
{
	if (skb->len >= 14 && skb->data[12] == 0x88 && skb->data[13] == 0x8e) {
		if (radio->reached < IEEE80211_STA_ASSOC && skb->len <= sizeof(radio->pending_eapol)) {
			lx_os_lock();
			if (radio->reached < IEEE80211_STA_ASSOC) {
				memcpy(radio->pending_eapol, skb->data, skb->len);
				radio->pending_eapol_len = skb->len;
				lx_os_unlock();
				return;
			}
			lx_os_unlock();
		}
		lx_handshake_message(radio, skb->data, skb->len);
	} else if (radio->joined && skb->len >= 14) {
		*(skb->data[0] & 1 ? &radio->received_group : &radio->received) += 1;
		lx_os_net_input(skb->data, skb->len);
	}
}

/*
 * A management frame from the access point of the network being joined, heard at `signal` dBm. Its answers while
 * joining are heard on its own channel moments before the network is joined: how strongly they come is what the
 * join reports, in place of the scan's figure, which can rest on a single frame read some 70 dB weaker than the
 * access point's other frames.
 */
static void
management_received(struct lx_radio *radio, const uint8_t *f, unsigned length, int signal)
{
	int answer = ANSWER_NONE;
	unsigned status = 0;

	if (radio->sta == NULL || length < 26 || memcmp(f + 10, radio->target.address, ETH_ALEN) != 0 ||
	    memcmp(f + 4, radio->vif->addr, ETH_ALEN) != 0) {
		return;
	}
	radio->signal = (int8_t)signal;
	radio->weakest = signal < radio->weakest ? (int8_t)signal : radio->weakest;
	if (f[0] == 0xb0 && length >= 30) {		/* authentication: algorithm, step (2: the answer), status */
		status = f[28] | f[29] << 8;
		answer = (f[26] | f[27] << 8) != 2 ? ANSWER_NONE : status == 0 ? ANSWER_AUTHENTICATED : ANSWER_REFUSED;
	} else if (f[0] == 0x10 && length >= 30) {	/* association response: capabilities, status, our number, elements */
		status = f[26] | f[27] << 8;
		answer = status == 0 ? ANSWER_ASSOCIATED : ANSWER_REFUSED;
		lx_os_lock();
		radio->aid = (uint16_t)((f[28] | f[29] << 8) & 0x3fff);
		radio->answer_length = length - 30 < ELEMENTS_MOST ? length - 30 : ELEMENTS_MOST;
		memcpy(radio->answer_elements, f + 30, radio->answer_length);
		lx_os_unlock();
	} else if (f[0] == 0xd0 && length >= 28 && f[24] == 3 && radio->reached >= IEEE80211_STA_ASSOC && lx_os_option("wifi_ba")) {
		/* an action frame, block ack's: ADDBA request (0) or DELBA (2); done on the driver's terms in ba_handle() */
		unsigned tid;

		lx_os_lock();
		if (f[25] == 0 && length >= 33) {
			unsigned params = f[27] | f[28] << 8;

			tid = params >> 2 & 7;
			radio->ba[tid].start = true;
			radio->ba[tid].amsdu = params & 1;
			radio->ba[tid].size = (uint16_t)(params >> 6);
			radio->ba[tid].token = f[26];
			radio->ba[tid].timeout = (uint16_t)(f[29] | f[30] << 8);
			radio->ba[tid].ssn = (uint16_t)((f[31] | f[32] << 8) >> 4);
			radio->ba_asked |= (uint8_t)(1u << tid);
		} else if (f[25] == 2) {
			tid = (f[26] | f[27] << 8) >> 12 & 7;
			radio->ba[tid].start = false;
			radio->ba_asked |= (uint8_t)(1u << tid);
		}
		lx_os_unlock();
		queue_work(radio->work, &radio->ba_work);
		return;
	} else if (f[0] == 0xc0 || f[0] == 0xa0) {	/* the access point shows us the door: with a reason */
		status = f[24] | f[25] << 8;
		answer = ANSWER_GONE;
		lx_tell("lx: THE ACCESS POINT %s US, reason %u\n", f[0] == 0xc0 ? "DEAUTHENTICATES" : "DISASSOCIATES", status);
		if (radio->joined) {
			radio->joined = false;
			radio->last_error = ECONNRESET;
			lx_os_net_link(0);
			lx_os_station_lost(ECONNRESET);
		}
	}
	if (answer != ANSWER_NONE) {
		lx_os_lock();
		radio->answer = answer;
		radio->answer_status = (uint16_t)status;
		lx_os_wakeup(&radio->answer);
		lx_os_unlock();
	}
}

/*
 * A frame the radio has received, with the driver's description behind it (where, how strong,
 * and in which form: an Ethernet frame, or 802.11 as it was on the air).
 */
void
linuxkpi_ieee80211_rx(struct ieee80211_hw *hw, struct sk_buff *skb, struct ieee80211_sta *sta, struct napi_struct *napi,
    struct list_head *list)
{
	struct lx_radio *radio = lx_radio_of(hw);
	const struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);

	radio->frames++;
	if (status->flag & RX_FLAG_8023) {
		ethernet_received(radio, skb);
	} else if (skb->len >= 36 && (skb->data[0] == 0x80 || skb->data[0] == 0x50)) {	/* a beacon, an answer to a probe */
		heard(radio, skb->data, skb->len, status->freq, status->signal);
	} else if (skb->len >= 24 && (skb->data[0] & 0x0c) == 0x00) {
		radio->others++;
		management_received(radio, skb->data, skb->len, status->signal);
	} else if (to_ethernet(skb, (status->flag & RX_FLAG_IV_STRIPPED) != 0)) {
		ethernet_received(radio, skb);
	} else {
		radio->others++;
	}
	dev_kfree_skb_any(skb);
}

/* ---- the station: an interface, a scan ---------------------------------------------------------------- */

/* The radio started and an interface in station mode on it, as mac80211 brings one up. 0, or an errno. */
int
lx_station_up(struct lx_radio *radio)
{
	struct ieee80211_hw *hw = &radio->hw;
	struct ieee80211_vif *vif;
	int ret;

	if (radio->vif != NULL) {
		return (0);
	}
	if (is_zero_ether_addr(hw->wiphy->perm_addr)) {
		lx_tell("lx: THE RADIO HAS NO ADDRESS (the kernel was given none, and the firmware has none): no interface is made\n");
		return (EADDRNOTAVAIL);
	}
	radio->vif_memory = kzalloc(64 + sizeof(*vif) + hw->vif_data_size, GFP_KERNEL);
	if (radio->vif_memory == NULL) {
		return (ENOMEM);
	}
	vif = (struct ieee80211_vif *)roundup2((uintptr_t)radio->vif_memory, 64);
	vif->type = NL80211_IFTYPE_STATION;
	memcpy(vif->addr, hw->wiphy->perm_addr, ETH_ALEN);
	memcpy(vif->bss_conf.addr, hw->wiphy->perm_addr, ETH_ALEN);
	vif->bss_conf.vif = vif;
	vif->bss_conf.bssid = no_network;	/* (the driver compares every beacon's sender with it) */
	vif->link_conf[0] = &vif->bss_conf;
	/* the firmware is to take Ethernet frames and to give them, if the driver offers that (its frame mode) */
	if (ieee80211_hw_check(hw, SUPPORTS_TX_ENCAP_OFFLOAD)) {
		vif->offload_flags |= IEEE80211_OFFLOAD_ENCAP_ENABLED;
	}
	if (ieee80211_hw_check(hw, SUPPORTS_RX_DECAP_OFFLOAD)) {
		vif->offload_flags |= IEEE80211_OFFLOAD_DECAP_ENABLED;
	}

	mutex_lock(&hw->wiphy->mtx);
	ret = radio->ops->start(hw);
	printf("lx: the radio is started: %d\n", ret);
	if (ret == 0) {
		radio->started = true;
		ret = radio->ops->add_interface(hw, vif);
		printf("lx: an interface in station mode is added to it: %d (Ethernet frames %s the firmware, %s it)\n", ret,
		    vif->offload_flags & IEEE80211_OFFLOAD_ENCAP_ENABLED ? "to" : "NOT to", vif->offload_flags &
		    IEEE80211_OFFLOAD_DECAP_ENABLED ? "from" : "NOT from");
		if (ret == 0) {
			radio->vif = vif;
		}
	}
	mutex_unlock(&hw->wiphy->mtx);
	return (-ret);
}

void
lx_station_down(struct lx_radio *radio)
{
	struct ieee80211_hw *hw = &radio->hw;

	mutex_lock(&hw->wiphy->mtx);
	if (radio->vif != NULL) {
		radio->ops->remove_interface(hw, radio->vif);
		radio->vif = NULL;
	}
	if (radio->started) {
		radio->ops->stop(hw, false);
		radio->started = false;
	}
	mutex_unlock(&hw->wiphy->mtx);
	kfree(radio->vif_memory);
	radio->vif_memory = NULL;
	printf("lx: the interface is removed and the radio stopped\n");
}

/*
 * A scan of every channel the regulatory domain allows, or of those `what` names. With a name
 * the radio asks for that network on every channel where it may send (a probe request; and the
 * access points that hear it answer); without one it asks for any network there, or, if it
 * is to be `quiet`, only listens. 0 when the driver has said the scan is over.
 */
int
lx_scan(struct lx_radio *radio, const uint8_t *name, unsigned name_length, bool quiet, const struct redwood_sta_scan *what)
{
	struct ieee80211_hw *hw = &radio->hw;
	struct ieee80211_scan_request *request;
	struct cfg80211_ssid asked;
	unsigned long long began;
	int channels = 0, ret;

	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		channels += hw->wiphy->bands[band] != NULL ? hw->wiphy->bands[band]->n_channels : 0;
	}
	request = kzalloc(sizeof(*request) + channels * sizeof(request->req.channels[0]), GFP_KERNEL);
	if (request == NULL) {
		return (ENOMEM);
	}
	request->req.wiphy = hw->wiphy;
	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *has = hw->wiphy->bands[band];

		for (int i = 0; has != NULL && i < has->n_channels; i++) {
			bool wanted = what == NULL || what->frequency_count == 0;

			for (unsigned k = 0; what != NULL && k < what->frequency_count && !wanted; k++) {
				wanted = what->frequencies[k] == has->channels[i].center_freq;
			}
			if (wanted && !(has->channels[i].flags & IEEE80211_CHAN_DISABLED)) {
				request->req.channels[request->req.n_channels++] = &has->channels[i];
			}
		}
	}
	if (request->req.n_channels == 0) {
		kfree(request);
		return (EINVAL);
	}
	if (!quiet) {				/* (a name of no length asks for every network) */
		bzero(&asked, sizeof(asked));
		memcpy(asked.ssid, name, name_length);
		asked.ssid_len = (uint8_t)name_length;
		request->req.ssids = &asked;
		request->req.n_ssids = 1;
	}
	if (what != NULL && what->dwell_ms != 0) {
		request->req.duration = what->dwell_ms;
	}
	radio->scan = 1;
	radio->frames = radio->others = radio->heard_count = radio->heard_lost = 0;
	began = lx_os_nanoseconds();
	mutex_lock(&hw->wiphy->mtx);
	ret = radio->ops->hw_scan(hw, radio->vif, request);
	mutex_unlock(&hw->wiphy->mtx);
	printf("lx: THE DRIVER IS ASKED TO SCAN %d channels, %s: %d\n", request->req.n_channels, quiet ? "listening only" :
	    name_length != 0 ? "asking for one network by name where it may send" : "asking for any network where it may send", ret);
	if (ret == 0) {
		lx_os_lock();
		while (radio->scan == 1 && lx_os_nanoseconds() - began < 40000000000ull) {
			lx_os_sleep(&radio->scan, 100 * 1000000ull);
		}
		lx_os_unlock();
		printf("lx: %llu ms later the scan is %s\n", (lx_os_nanoseconds() - began) / 1000000ull, radio->scan == 2 ? "OVER" :
		    radio->scan == 3 ? "GIVEN UP by the driver" : "NOT OVER: the driver has not said so");
		say_heard(radio);
		if (radio->scan == 1 && radio->ops->cancel_hw_scan != NULL) {
			mutex_lock(&hw->wiphy->mtx);
			radio->ops->cancel_hw_scan(hw, radio->vif);
			mutex_unlock(&hw->wiphy->mtx);
		}
	}
	kfree(request);
	return (ret != 0 ? -ret : radio->scan == 2 ? 0 : ETIMEDOUT);
}

/* An interface, a scan that only listens, and down again: what the radio hears is said. 0, or an errno. */
int
lx_station_scan(void)
{
	struct lx_radio *radio = the_radio;
	int error;

	if (radio == NULL || !radio->registered) {
		printf("lx: no radio is registered: nothing to scan with\n");
		return (ENXIO);
	}
	if ((error = lx_station_up(radio)) == 0) {
		error = lx_scan(radio, NULL, 0, true, NULL);
	}
	lx_station_down(radio);
	return (error);
}

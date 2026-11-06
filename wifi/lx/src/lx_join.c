/*
 * The station of the 802.11 layer, its second half (the first is lx_80211.c, whose remarks at the
 * top describe both): joining one network protected with WPA2 -- authentication, association,
 * the four-way handshake of lx_wpa.c, the keys given to the driver --, block ack sessions the
 * access point asks for, leaving again, and the station as the kernel asks for it (redwood_sta.h).
 */
#include "lx_80211.h"

/* ---- the station: block ack -------------------------------------------------------------------------- */

/*
 * The access point's requests to send us frames in aggregates (A-MPDU) for a traffic class, and to
 * stop doing so, as mac80211 handles them (net/mac80211/agg-rx.c): the driver told (its
 * ampdu_action, which sets up the firmware's reorder queue for the class), then the answer, ADDBA
 * response, with the window size the driver can take. Without this every frame goes on its own
 * and the link carries about a third of what it could. On the driver's work queue: the driver's
 * call sends WMI commands, which the thread that brought the request must be free to see answered.
 */
static void
ba_handle(struct work_struct *work)
{
	struct lx_radio *radio = __containerof(work, struct lx_radio, ba_work);
	struct ieee80211_hw *hw = &radio->hw;

	for (unsigned tid = 0; tid < 8; tid++) {
		struct ieee80211_ampdu_params params = { .sta = radio->sta, .tid = (uint8_t)tid };
		struct sk_buff *skb;
		uint8_t *frame;
		bool asked;
		int ret = 0;

		lx_os_lock();
		asked = (radio->ba_asked & 1u << tid) != 0;
		radio->ba_asked &= (uint8_t)~(1u << tid);
		params.ssn = radio->ba[tid].ssn;
		params.timeout = radio->ba[tid].timeout;
		params.amsdu = radio->ba[tid].amsdu;
		params.buf_size = radio->ba[tid].size == 0 || radio->ba[tid].size > hw->max_rx_aggregation_subframes ?
		    hw->max_rx_aggregation_subframes : radio->ba[tid].size;
		lx_os_unlock();
		if (!asked || radio->sta == NULL || radio->reached < IEEE80211_STA_ASSOC) {
			continue;
		}
		mutex_lock(&hw->wiphy->mtx);
		if (radio->ba[tid].active) {			/* a new request for a class that has one: the old one goes first */
			params.action = IEEE80211_AMPDU_RX_STOP;
			radio->ops->ampdu_action(hw, radio->vif, &params);
			radio->ba[tid].active = false;
		}
		if (radio->ba[tid].start) {
			params.action = IEEE80211_AMPDU_RX_START;
			ret = radio->ops->ampdu_action(hw, radio->vif, &params);
			radio->ba[tid].active = ret == 0;
		}
		mutex_unlock(&hw->wiphy->mtx);
		if (!radio->ba[tid].start) {
			continue;
		}
		radio->ba_sessions += ret == 0;
		radio->ba_refused += ret != 0;
		lx_tell("lx: BLOCK ACK for traffic class %u: the access point asks to send aggregates of up to %u frames; the driver "
		    "takes %u: %d\n", tid, radio->ba[tid].size, params.buf_size, ret);
		if ((skb = lx_frame_new(radio, 24 + 9, &frame)) == NULL) {
			continue;
		}
		frame = lx_management(radio, frame, 0xd0);
		frame[0] = 3;				/* block ack */
		frame[1] = 1;				/* ADDBA response */
		frame[2] = radio->ba[tid].token;
		frame[3] = ret == 0 ? 0 : 37;		/* success, or "the request has been declined" */
		frame[4] = 0;
		{
			unsigned answer = (params.amsdu ? 1u : 0u) | 2u | tid << 2 | (unsigned)params.buf_size << 6;

			frame[5] = (uint8_t)answer;
			frame[6] = (uint8_t)(answer >> 8);
		}
		frame[7] = (uint8_t)params.timeout;
		frame[8] = (uint8_t)(params.timeout >> 8);
		lx_frame_send(radio, skb);
	}
}

/* ---- the station: joining a network -------------------------------------------------------------------- */

/* Wait for the access point's answer to a management frame: one of the ANSWER_s, or ANSWER_NONE when the time is up. */
static int
answer_wait(struct lx_radio *radio, unsigned ms)
{
	unsigned long long began = lx_os_nanoseconds();
	int answer;

	lx_os_lock();
	while (radio->answer == ANSWER_NONE && lx_os_nanoseconds() - began < ms * 1000000ull) {
		lx_os_sleep(&radio->answer, 20 * 1000000ull);
	}
	answer = radio->answer;
	lx_os_unlock();
	return (answer);
}

/* The rates among `count` bytes of a rates element that the band has, as bits in the band's own order; and the "basic" ones. */
static uint32_t
rates_of(const struct ieee80211_supported_band *band, const uint8_t *rates, unsigned count, uint32_t *basic)
{
	uint32_t has = 0;

	for (unsigned i = 0; i < count; i++) {
		for (int r = 0; r < band->n_bitrates; r++) {
			if (band->bitrates[r].bitrate == (rates[i] & 0x7f) * 5u) {	/* (half megabits there, tenths here) */
				has |= 1u << r;
				*basic |= rates[i] & 0x80 ? 1u << r : 0;
			}
		}
	}
	return (has);
}

/*
 * What the access point says it can do, into the form the driver reads it in when it tells the
 * firmware about the peer (mac.c: ath11k_peer_assoc_prepare()): rates, the HT and VHT
 * capabilities, WMM. `elements` are the association response's, or the beacon's before that.
 */
static void
station_abilities(struct lx_radio *radio, const uint8_t *elements, unsigned length)
{
	struct ieee80211_supported_band *band = radio->wiphy->bands[radio->channel->def.chan->band];
	struct ieee80211_sta *sta = radio->sta;
	struct ieee80211_bss_conf *conf = &radio->vif->bss_conf;
	const uint8_t *e;
	uint32_t rates = 0, basic = 0;

	if ((e = lx_element(elements, length, 1)) != NULL) {
		rates |= rates_of(band, e + 2, e[1], &basic);
	}
	if ((e = lx_element(elements, length, 50)) != NULL) {
		rates |= rates_of(band, e + 2, e[1], &basic);
	}
	sta->deflink.supp_rates[band->band] = rates;
	conf->basic_rates = basic;
	sta->wme = conf->qos = lx_vendor_element(elements, length, wmm) != NULL;
	sta->deflink.bandwidth = radio->width == 80 ? IEEE80211_STA_RX_BW_80 : radio->width == 40 ? IEEE80211_STA_RX_BW_40 :
	    IEEE80211_STA_RX_BW_20;
	sta->deflink.rx_nss = 1;
	sta->deflink.smps_mode = IEEE80211_SMPS_OFF;
	if ((e = lx_element(elements, length, 45)) != NULL && e[1] >= 26 && band->ht_cap.ht_supported) {	/* HT capabilities */
		struct ieee80211_sta_ht_cap *ht = &sta->deflink.ht_cap;

		ht->ht_supported = true;
		/* (without the bits for 40 MHz where the channel is 20 MHz wide) */
		ht->cap = (uint16_t)((e[2] | e[3] << 8) & (radio->width > 20 ? 0xffff : ~(IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
		    IEEE80211_HT_CAP_SGI_40)));
		ht->ampdu_factor = e[4] & 3;
		ht->ampdu_density = e[4] >> 2 & 7;
		memcpy(&ht->mcs, e + 5, sizeof(ht->mcs));
		sta->deflink.rx_nss = ht->mcs.rx_mask[1] != 0 ? 2 : 1;
	}
	if ((e = lx_element(elements, length, 191)) != NULL && e[1] >= 12 && band->vht_cap.vht_supported) {	/* VHT capabilities */
		struct ieee80211_sta_vht_cap *vht = &sta->deflink.vht_cap;

		vht->vht_supported = true;
		vht->cap = e[2] | e[3] << 8 | e[4] << 16 | (uint32_t)e[5] << 24;
		memcpy(&vht->vht_mcs, e + 6, sizeof(vht->vht_mcs));
	}
	if ((e = lx_element(elements, length, 61)) != NULL && e[1] >= 22) {	/* HT operation: the protection in use */
		conf->ht_operation_mode = (uint16_t)(e[4] | e[5] << 8);
	}
	if ((e = lx_element(elements, length, 5)) != NULL && e[1] >= 2 && e[3] != 0) {	/* the TIM: every how many beacons */
		conf->dtim_period = e[3];
	}
}

/*
 * Our association request's elements, after the two fixed fields: the name, our rates, what
 * 802.11h asks of a station where the access point uses it, RSN (what we agree to of what it
 * offers: CCMP between the two, its cipher for the group, a passphrase's key), our HT and VHT
 * capabilities if it has them, and WMM. Returns their length.
 */
static unsigned
request_elements(struct lx_radio *radio, uint8_t *out, unsigned group_cipher)
{
	const struct lx_target *target = &radio->target;
	struct linuxkpi_ieee80211_channel *channel = radio->channel->def.chan;
	struct ieee80211_supported_band *band = radio->wiphy->bands[channel->band];
	uint8_t *at = out;
	int count = band->n_bitrates;

	*at++ = 0;
	*at++ = radio->want_length;
	memcpy(at, radio->want, radio->want_length);
	at += radio->want_length;
	*at++ = 1;
	*at++ = (uint8_t)(count < 8 ? count : 8);
	for (int r = 0; r < count; r++) {
		if (r == 8) {
			*at++ = 50;
			*at++ = (uint8_t)(count - 8);
		}
		*at++ = (uint8_t)(band->bitrates[r].bitrate / 5 | (radio->vif->bss_conf.basic_rates & 1u << r ? 0x80 : 0));
	}
	if (target->capability & 0x0100) {	/* spectrum management: the power we can send with, the channels we have */
		*at++ = 33;
		*at++ = 2;
		*at++ = 0;
		*at++ = (uint8_t)channel->max_power;
		*at++ = 36;
		*at++ = 2;
		*at++ = (uint8_t)lx_channel_of(channel->center_freq);
		*at++ = 1;
	}
	radio->rsn[0] = 48;
	radio->rsn[1] = 20;
	memcpy(radio->rsn + 2, "\x01\x00\x00\x0f\xac\x04\x01\x00\x00\x0f\xac\x04\x01\x00\x00\x0f\xac\x02\x00\x00", 20);
	radio->rsn[7] = (uint8_t)group_cipher;
	memcpy(at, radio->rsn, 22);
	at += 22;
	if (lx_element(target->elements, target->elements_length, 45) != NULL && band->ht_cap.ht_supported) {
		uint16_t cap = (uint16_t)(band->ht_cap.cap & (radio->width > 20 ? 0xffff : ~(IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
		    IEEE80211_HT_CAP_SGI_40)));

		*at++ = 45;
		*at++ = 26;
		bzero(at, 26);
		at[0] = (uint8_t)cap;
		at[1] = (uint8_t)(cap >> 8);
		at[2] = (uint8_t)(band->ht_cap.ampdu_factor | band->ht_cap.ampdu_density << 2);
		memcpy(at + 3, &band->ht_cap.mcs, sizeof(band->ht_cap.mcs));
		at += 26;
	}
	if (lx_element(target->elements, target->elements_length, 191) != NULL && band->vht_cap.vht_supported) {
		*at++ = 191;
		*at++ = 12;
		at[0] = (uint8_t)band->vht_cap.cap;
		at[1] = (uint8_t)(band->vht_cap.cap >> 8);
		at[2] = (uint8_t)(band->vht_cap.cap >> 16);
		at[3] = (uint8_t)(band->vht_cap.cap >> 24);
		memcpy(at + 4, &band->vht_cap.vht_mcs, sizeof(band->vht_cap.vht_mcs));
		at += 12;
	}
	if (lx_vendor_element(target->elements, target->elements_length, wmm) != NULL) {
		memcpy(at, "\xdd\x07\x00\x50\xf2\x02\x00\x01\x00", 9);
		at += 9;
	}
	return ((unsigned)(at - out));
}

/*
 * What kind of protection the access point offers (its RSN element: version, the group's
 * cipher, the ciphers and the kinds of key management it takes, capabilities). NULL if we can
 * join it -- CCMP, a passphrase's key (kind 2), management frames unprotected --, and
 * *group_cipher is then what it ciphers frames to everybody with; else what stands in the way.
 */
static const char *
protection(const struct lx_target *target, unsigned *group_cipher)
{
	const uint8_t *rsn = lx_element(target->elements, target->elements_length, 48), *at, *end;
	bool ccmp = false, passphrase = false;
	unsigned count;

	if (rsn == NULL) {
		return (target->capability & 0x10 ? "it is protected by something older than WPA2" : "it is not protected at all: an "
		       "open network is not joined by this");
	}
	at = rsn + 2;
	end = at + rsn[1];
	if (end - at < 8 || at[2] != 0x00 || at[3] != 0x0f || at[4] != 0xac) {
		return ("its RSN element is not one this reads");
	}
	*group_cipher = at[5];
	count = at[6] | at[7] << 8;
	at += 8;
	for (; count != 0 && end - at >= 4; count--, at += 4) {
		ccmp |= at[0] == 0x00 && at[1] == 0x0f && at[2] == 0xac && at[3] == 4;
	}
	if (end - at < 2) {
		return ("its RSN element names no kind of key management");
	}
	count = at[0] | at[1] << 8;
	at += 2;
	for (; count != 0 && end - at >= 4; count--, at += 4) {
		passphrase |= at[0] == 0x00 && at[1] == 0x0f && at[2] == 0xac && at[3] == 2;
	}
	if (*group_cipher != 4) {
		return ("it ciphers frames to everybody with something other than CCMP (TKIP, for old stations), which is not done here");
	}
	if (!ccmp) {
		return ("it does not offer CCMP");
	}
	if (!passphrase) {
		return ("it does not take a passphrase's key of the WPA2 kind (WPA3 only, or an enterprise network)");
	}
	if (end - at >= 2 && (at[0] & 0x40)) {
		return ("it requires protected management frames, which are not done here");
	}
	return (NULL);
}

/* A key for the driver to give the firmware: CCMP, for frames between us and the access point or for the group's. */
static struct ieee80211_key_conf *
key_set(struct lx_radio *radio, const uint8_t *key, unsigned index, bool pairwise, int *error)
{
	struct ieee80211_key_conf *conf = kzalloc(sizeof(*conf) + 16, GFP_KERNEL);

	if (conf == NULL) {
		*error = -ENOMEM;
		return (NULL);
	}
	conf->cipher = WLAN_CIPHER_SUITE_CCMP;
	conf->keyidx = (uint8_t)index;
	conf->keylen = 16;
	conf->iv_len = 8;
	conf->icv_len = 8;
	conf->flags = pairwise ? IEEE80211_KEY_FLAG_PAIRWISE : 0;
	conf->link_id = -1;
	memcpy(conf->key, key, 16);
	mutex_lock(&radio->wiphy->mtx);
	*error = radio->ops->set_key(&radio->hw, SET_KEY, radio->vif, pairwise ? radio->sta : NULL, conf);
	mutex_unlock(&radio->wiphy->mtx);
	return (conf);
}

static void
key_drop(struct lx_radio *radio, struct ieee80211_key_conf **conf, bool pairwise)
{
	if (*conf != NULL) {
		mutex_lock(&radio->wiphy->mtx);
		radio->ops->set_key(&radio->hw, DISABLE_KEY, radio->vif, pairwise ? radio->sta : NULL, *conf);
		mutex_unlock(&radio->wiphy->mtx);
		explicit_bzero((*conf)->key, 16);
		kfree(*conf);
		*conf = NULL;
	}
}

/* The access point has changed the group's key (it does every so often): the new one to the driver, the old one gone. */
static void
group_rekey(struct work_struct *work)
{
	struct lx_radio *radio = __containerof(work, struct lx_radio, group_work);
	struct ieee80211_key_conf *old = radio->group_key;
	int error = 0;

	radio->rekeys++;
	radio->group_key = key_set(radio, radio->wpa.group, radio->wpa.group_index, false, &error);
	lx_tell("lx: the group's new key (number %u) is given to the driver: %d\n", radio->wpa.group_index, error);
	key_drop(radio, &old, false);
}

/* One step of the station's state as the driver is told of it (mac80211: drv_sta_state()). */
static int
station_state(struct lx_radio *radio, enum ieee80211_sta_state from, enum ieee80211_sta_state to)
{
	int ret;

	mutex_lock(&radio->wiphy->mtx);
	ret = radio->ops->sta_state(&radio->hw, radio->vif, radio->sta, from, to);
	mutex_unlock(&radio->wiphy->mtx);
	return (ret);
}

/*
 * mac80211: ieee80211_bss_info_change_notify(). A driver with links (ath12k) has two calls in place of
 * bss_info_changed: what is of the interface as a whole, and what is of its one link, in that order.
 */
static void
bss_changed(struct lx_radio *radio, u64 what)
{
	const u64 of_interface = BSS_CHANGED_ASSOC | BSS_CHANGED_IDLE | BSS_CHANGED_PS | BSS_CHANGED_ARP_FILTER | BSS_CHANGED_SSID;

	mutex_lock(&radio->wiphy->mtx);
	if (radio->ops->bss_info_changed != NULL) {
		radio->ops->bss_info_changed(&radio->hw, radio->vif, &radio->vif->bss_conf, what);
	} else {
		if ((what & of_interface) != 0) {
			radio->ops->vif_cfg_changed(&radio->hw, radio->vif, what & of_interface);
		}
		if ((what & ~of_interface) != 0) {
			radio->ops->link_info_changed(&radio->hw, radio->vif, &radio->vif->bss_conf, what & ~of_interface);
		}
	}
	mutex_unlock(&radio->wiphy->mtx);
}

/* Everything a join has made, undone in mac80211's order; as far as it got. */
static void
station_leave(struct lx_radio *radio)
{
	struct ieee80211_hw *hw = &radio->hw;
	struct ieee80211_vif *vif = radio->vif;
	int state = radio->reached;

	if (radio->joined) {
		radio->joined = false;
		lx_os_net_link(0);
	}
	if (radio->sta != NULL && state >= IEEE80211_STA_AUTH && radio->answer != ANSWER_GONE) {
		struct sk_buff *skb;
		uint8_t *frame;

		if ((skb = lx_frame_new(radio, 26, &frame)) != NULL) {	/* deauthentication, reason 3: we are leaving */
			frame = lx_management(radio, frame, 0xc0);
			frame[0] = 3;
			frame[1] = 0;
			lx_frame_send(radio, skb);
			lx_pause_ms(50);
		}
	}
	cancel_work_sync(&radio->group_work);
	cancel_work_sync(&radio->ba_work);
	radio->ba_asked = 0;
	bzero(radio->ba, sizeof(radio->ba));		/* (the driver forgets the classes' queues with the station) */
	key_drop(radio, &radio->group_key, false);
	key_drop(radio, &radio->pairwise_key, true);
	explicit_bzero(&radio->wpa, sizeof(radio->wpa));
	for (; radio->sta != NULL && state > IEEE80211_STA_NOTEXIST; state--) {
		station_state(radio, state, state - 1);
	}
	if (vif != NULL && vif->cfg.assoc) {
		vif->cfg.assoc = false;
		vif->cfg.aid = 0;
		bss_changed(radio, BSS_CHANGED_ASSOC);
	}
	if (radio->channel != NULL) {
		mutex_lock(&hw->wiphy->mtx);
		radio->ops->unassign_vif_chanctx(hw, vif, &vif->bss_conf, radio->channel);
		vif->bss_conf.chanctx_conf = NULL;
		radio->ops->remove_chanctx(hw, radio->channel);
		mutex_unlock(&hw->wiphy->mtx);
		kfree(radio->channel);
		radio->channel = NULL;
	}
	if (vif != NULL) {
		vif->bss_conf.bssid = no_network;
		bss_changed(radio, BSS_CHANGED_BSSID);
	}
	kfree(radio->sta_memory);
	radio->sta_memory = radio->sta = NULL;
	radio->reached = IEEE80211_STA_NOTEXIST;
}

/*
 * How wide the channel is to be: with wifi_wide=1 as wide as the access point says it works (its
 * HT operation element: the second 20 MHz above or below; its VHT operation element: 80 MHz around
 * a centre) and as the radio may tune (every 20 MHz of it allowed here); else 20 MHz. 80 MHz
 * only at 5 GHz. mac80211 does this in ieee80211_determine_chan_mode().
 */
static void
station_width(struct lx_radio *radio, struct linuxkpi_ieee80211_channel *primary, struct cfg80211_chan_def *def)
{
	const struct lx_target *target = &radio->target;
	struct ieee80211_supported_band *band = radio->wiphy->bands[primary->band];
	const uint8_t *ht = lx_element(target->elements, target->elements_length, 61);
	const uint8_t *vht = lx_element(target->elements, target->elements_length, 192);
	unsigned offset;

	def->chan = primary;
	def->width = NL80211_CHAN_WIDTH_20;
	def->center_freq1 = primary->center_freq;
	radio->width = 20;
	if (!lx_os_option("wifi_wide") || ht == NULL || ht[1] < 22 || !band->ht_cap.ht_supported ||
	    !(band->ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40) || !(ht[3] & 4)) {
		return;
	}
	if (vht != NULL && vht[1] >= 3 && vht[2] == 1 && primary->band == NL80211_BAND_5GHZ && band->vht_cap.vht_supported) {
		unsigned center = 5000 + 5 * vht[3];
		bool all = true;

		for (unsigned frequency = center - 30; frequency <= center + 30; frequency += 20) {
			struct linuxkpi_ieee80211_channel *c = linuxkpi_ieee80211_get_channel(radio->wiphy, frequency);

			all &= c != NULL && !(c->flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_80MHZ));
		}
		if (all && primary->center_freq >= center - 30 && primary->center_freq <= center + 30) {
			def->width = NL80211_CHAN_WIDTH_80;
			def->center_freq1 = center;
			radio->width = 80;
			return;
		}
	}
	offset = ht[3] & 3;		/* the second 20 MHz: 1 above, 3 below, 0 none */
	if (offset == 1 || offset == 3) {
		struct linuxkpi_ieee80211_channel *second = linuxkpi_ieee80211_get_channel(radio->wiphy, offset == 1 ?
		    primary->center_freq + 20 : primary->center_freq - 20);

		if (second != NULL && !(second->flags & IEEE80211_CHAN_DISABLED) && !(primary->flags & (offset == 1 ?
		    IEEE80211_CHAN_NO_HT40PLUS : IEEE80211_CHAN_NO_HT40MINUS))) {
			def->width = NL80211_CHAN_WIDTH_40;
			def->center_freq1 = offset == 1 ? primary->center_freq + 10 : primary->center_freq - 10;
			radio->width = 40;
		}
	}
}

/*
 * Join the network of that name with that passphrase, in the order mac80211 goes through
 * (net/mac80211/mlme.c of Linux: ieee80211_mgd_auth(), ieee80211_mgd_assoc(), and what the
 * supplicant does after them): the access point found by a scan that asks for the network; the
 * channel taken; the access point made a station the driver knows; authentication; association;
 * the handshake; the keys. 0 when frames can pass, or an errno with the reason said. A network
 * that was joined before is left first. The access point is the one a scan has heard best --
 * the last scan, if that heard the network, or one made now.
 */
static int
station_join(struct lx_radio *radio, const struct redwood_sta_join *what)
{
	const uint8_t *name = what->ssid;
	unsigned name_length = what->ssid_length;
	struct ieee80211_hw *hw = &radio->hw;
	struct ieee80211_vif *vif;
	struct linuxkpi_ieee80211_channel *channel;
	struct lx_target *target = &radio->target;
	struct sk_buff *skb;
	uint8_t *frame;
	const uint8_t *e;
	const char *obstacle;
	unsigned long long began;
	unsigned group_cipher = 0, length;
	int ret, error = 0, answer = ANSWER_NONE;

	if (name_length == 0 || name_length > sizeof(radio->want)) {
		return (EINVAL);
	}
	if (what->security != REDWOOD_STA_WPA2_PSK) {
		lx_tell("lx: a network without WPA2 and a passphrase is asked for: that is not built\n");
		return (ENOTSUP);
	}
	if (radio->sta != NULL) {
		station_leave(radio);
		lx_tell("lx: the network joined before is left\n");
	}
	INIT_WORK(&radio->group_work, group_rekey);
	INIT_WORK(&radio->ba_work, ba_handle);
	radio->ba_asked = 0;
	bzero(radio->ba, sizeof(radio->ba));
	memcpy(radio->want, name, name_length);
	radio->want_length = (uint8_t)name_length;
	bzero(target, sizeof(*target));
	if ((ret = lx_station_up(radio)) != 0) {
		return (ret);
	}
	vif = radio->vif;
	lx_os_lock();
	for (unsigned i = 0; i < radio->heard_count; i++) {
		const struct lx_heard *network = &radio->heard[i];

		if (network->name_length == name_length && memcmp(network->name, name, name_length) == 0 && network->elements_length != 0 &&
		    (is_zero_ether_addr(what->bssid) || memcmp(what->bssid, network->sender, ETH_ALEN) == 0) &&
		    (!target->found || network->signal > target->signal)) {
			target->found = true;
			memcpy(target->address, network->sender, ETH_ALEN);
			target->frequency = network->frequency;
			target->interval = network->interval;
			target->capability = network->capability;
			target->signal = network->signal;
			target->elements_length = network->elements_length;
			memcpy(target->elements, network->elements, network->elements_length);
		}
	}
	lx_os_unlock();
	for (unsigned attempt = 0; attempt < 3 && !target->found; attempt++) {
		lx_scan(radio, name, name_length, false, NULL);
	}
	if (!target->found) {
		lx_tell("lx: THE NETWORK THAT IS WANTED WAS NOT HEARD (a name of %u bytes), in three scans\n", name_length);
		return (ENOENT);
	}
	channel = linuxkpi_ieee80211_get_channel(hw->wiphy, target->frequency);
	if ((e = lx_element(target->elements, target->elements_length, 3)) != NULL && e[1] == 1 && channel != NULL &&
	    linuxkpi_ieee80211_get_channel(hw->wiphy, linuxkpi_ieee80211_channel_to_frequency(e[2], channel->band)) != NULL) {
		/* (a beacon can be heard on the channel next to its own: where it says it is counts) */
		channel = linuxkpi_ieee80211_get_channel(hw->wiphy, linuxkpi_ieee80211_channel_to_frequency(e[2], channel->band));
	}
	obstacle = protection(target, &group_cipher);
	lx_tell("lx: the network that is wanted: an access point on channel %u (%u MHz) at %d dBm, capabilities %#x, beacons every "
	    "%u units; HT %s, VHT %s, WMM %s; %s%s\n", channel != NULL ? lx_channel_of(channel->center_freq) : 0, channel != NULL ?
	    channel->center_freq : 0, target->signal, target->capability, target->interval, lx_element(target->elements,
	    target->elements_length, 45) ? "yes" : "no", lx_element(target->elements, target->elements_length, 191) ? "yes" : "no",
	    lx_vendor_element(target->elements, target->elements_length, wmm) ? "yes" : "no", obstacle == NULL ? "WPA2 with a "
	    "passphrase and CCMP: it can be joined" : "IT CANNOT BE JOINED: ", obstacle == NULL ? "" : obstacle);
	if (obstacle != NULL || channel == NULL || (channel->flags & IEEE80211_CHAN_DISABLED)) {
		return (ENOTSUP);
	}

	/* the key of the name and the passphrase, and what the handshake needs besides */
	began = lx_os_nanoseconds();
	bzero(&radio->wpa, sizeof(radio->wpa));
	if (what->passphrase_length != 0) {
		lx_wpa_pmk(what->passphrase, what->passphrase_length, name, name_length, radio->wpa.pmk);
	} else {
		memcpy(radio->wpa.pmk, what->pmk, sizeof(radio->wpa.pmk));
	}
	memcpy(radio->wpa.own, vif->addr, ETH_ALEN);
	memcpy(radio->wpa.peer, target->address, ETH_ALEN);
	get_random_bytes(radio->wpa.own_nonce, sizeof(radio->wpa.own_nonce));
	radio->wpa.rsn = radio->rsn;
	radio->wpa.rsn_length = 22;
	radio->handshake = LX_WPA_NOTHING;
	radio->eapol = 0;
	radio->pending_eapol_len = 0;
	printf("lx: the key of the name (%u bytes) and the passphrase (%u bytes; 0: the key itself was given) is there: %llu ms; "
	    "its fingerprint %04x\n", name_length, what->passphrase_length, (lx_os_nanoseconds() - began) / 1000000ull,
	    lx_wpa_fingerprint(radio->wpa.pmk));

	/* the access point's address, and its channel, 20 MHz wide */
	vif->bss_conf.bssid = target->address;
	vif->bss_conf.beacon_int = target->interval;
	vif->bss_conf.assoc_capability = target->capability;
	vif->bss_conf.dtim_period = 1;
	vif->bss_conf.use_short_slot = (target->capability & 0x0400) != 0 || channel->band != NL80211_BAND_2GHZ;
	vif->bss_conf.use_short_preamble = (target->capability & 0x0020) != 0;
	memcpy(vif->cfg.ap_addr, target->address, ETH_ALEN);
	memcpy(vif->cfg.ssid, name, name_length);
	vif->cfg.ssid_len = name_length;
	bss_changed(radio, BSS_CHANGED_BSSID);
	radio->channel = kzalloc(sizeof(*radio->channel) + hw->chanctx_data_size + 64, GFP_KERNEL);
	radio->sta_memory = kzalloc(64 + sizeof(*radio->sta) + hw->sta_data_size, GFP_KERNEL);
	if (radio->channel == NULL || radio->sta_memory == NULL) {
		error = ENOMEM;
		goto out;
	}
	station_width(radio, channel, &radio->channel->def);
	radio->channel->min_def = radio->channel->def;
	radio->channel->rx_chains_static = radio->channel->rx_chains_dynamic = 2;
	vif->bss_conf.chanreq.oper = radio->channel->def;
	mutex_lock(&hw->wiphy->mtx);
	ret = radio->ops->add_chanctx(hw, radio->channel);
	if (ret == 0) {
		vif->bss_conf.chanctx_conf = radio->channel;
		ret = radio->ops->assign_vif_chanctx(hw, vif, &vif->bss_conf, radio->channel);
	}
	mutex_unlock(&hw->wiphy->mtx);
	lx_tell("lx: the channel is taken, %u MHz wide around %u MHz: %d\n", radio->width, radio->channel->def.center_freq1, ret);
	if (ret != 0) {
		error = -ret;
		goto out;
	}

	/* the access point as a station of the driver's: the firmware learns of it, and the radio goes to its channel */
	radio->signal = target->signal;
	radio->weakest = 0;
	radio->beacons = 0;
	radio->sta = (struct ieee80211_sta *)roundup2((uintptr_t)radio->sta_memory, 64);
	memcpy(radio->sta->addr, target->address, ETH_ALEN);
	memcpy(radio->sta->deflink.addr, target->address, ETH_ALEN);
	radio->sta->deflink.sta = radio->sta;
	radio->sta->link[0] = &radio->sta->deflink;
	station_abilities(radio, target->elements, target->elements_length);
	ret = station_state(radio, IEEE80211_STA_NOTEXIST, IEEE80211_STA_NONE);
	printf("lx: the access point is made known to the driver: %d\n", ret);
	if (ret != 0) {
		kfree(radio->sta_memory);
		radio->sta_memory = radio->sta = NULL;
		error = -ret;
		goto out;
	}
	radio->reached = IEEE80211_STA_NONE;

	/* authentication, of the kind that asks nothing ("open system": the passphrase is proved in the handshake) */
	for (unsigned attempt = 0; attempt < 3 && answer == ANSWER_NONE; attempt++) {
		radio->answer = ANSWER_NONE;
		if ((skb = lx_frame_new(radio, 30, &frame)) == NULL) {
			break;
		}
		frame = lx_management(radio, frame, 0xb0);
		memcpy(frame, "\x00\x00\x01\x00\x00\x00", 6);	/* algorithm 0, step 1, status 0 */
		lx_frame_send(radio, skb);
		answer = answer_wait(radio, 500);
	}
	lx_tell("lx: AUTHENTICATION: %s (status %u)\n", answer == ANSWER_AUTHENTICATED ? "the access point accepts" : answer ==
	    ANSWER_NONE ? "NO ANSWER from the access point to three requests" : "THE ACCESS POINT REFUSES", radio->answer_status);
	if (answer != ANSWER_AUTHENTICATED || (ret = station_state(radio, IEEE80211_STA_NONE, IEEE80211_STA_AUTH)) != 0) {
		error = answer == ANSWER_NONE ? ETIMEDOUT : ECONNREFUSED;
		goto out;
	}
	radio->reached = IEEE80211_STA_AUTH;

	/* association: what we can do, and that we want its protection */
	answer = ANSWER_NONE;
	for (unsigned attempt = 0; attempt < 3 && answer == ANSWER_NONE; attempt++) {
		radio->answer = ANSWER_NONE;
		if ((skb = lx_frame_new(radio, 28 + 300, &frame)) == NULL) {
			break;
		}
		frame = lx_management(radio, frame, 0x00);
		/* capabilities: an infrastructure network, protected, and of the access point's own what a station repeats */
		frame[0] = (uint8_t)(0x01 | 0x10 | (target->capability & 0x20));
		frame[1] = (uint8_t)((target->capability & 0x0500) >> 8);
		frame[2] = 10;				/* we listen at least every tenth beacon */
		frame[3] = 0;
		length = request_elements(radio, frame + 4, group_cipher);
		skb_trim(skb, 28 + length);
		lx_frame_send(radio, skb);
		answer = answer_wait(radio, 500);
	}
	lx_tell("lx: ASSOCIATION: %s (status %u); we are its station number %u; its answer has %u bytes of elements\n", answer ==
	    ANSWER_ASSOCIATED ? "THE ACCESS POINT ACCEPTS" : answer == ANSWER_NONE ? "NO ANSWER from the access point to three "
	    "requests" : "THE ACCESS POINT REFUSES", radio->answer_status, radio->aid, radio->answer_length);
	if (answer != ANSWER_ASSOCIATED) {
		error = answer == ANSWER_NONE ? ETIMEDOUT : ECONNREFUSED;
		goto out;
	}
	station_abilities(radio, radio->answer_elements, radio->answer_length);
	radio->sta->aid = radio->aid;
	if ((ret = station_state(radio, IEEE80211_STA_AUTH, IEEE80211_STA_ASSOC)) != 0) {
		error = -ret;
		goto out;
	}
	radio->reached = IEEE80211_STA_ASSOC;
	vif->cfg.assoc = true;
	vif->cfg.aid = radio->aid;
	bss_changed(radio, BSS_CHANGED_ASSOC | BSS_CHANGED_BEACON_INT | BSS_CHANGED_ERP_SLOT | BSS_CHANGED_ERP_PREAMBLE |
	    BSS_CHANGED_QOS);
	if ((e = lx_vendor_element(radio->answer_elements, radio->answer_length, wmm)) != NULL && e[1] >= 24 && e[6] == 1) {
		/* WMM's parameters: for each of the four classes how long to wait and how long to hold the air */
		mutex_lock(&hw->wiphy->mtx);
		for (unsigned i = 0; i < 4; i++) {
			const uint8_t *record = e + 10 + 4 * i;
			static const uint8_t classes[4] = { IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_VI, IEEE80211_AC_VO };
			struct ieee80211_tx_queue_params params = { .aifs = record[0] & 0x0f, .cw_min = (uint16_t)((1u << (record[1] &
				                                    0x0f)) - 1), .cw_max = (uint16_t)((1u << (record[1] >> 4)) - 1),
				                                    .txop = (uint16_t)(record[2] | record[3] << 8), .acm = record[0] >> 4 &
				                                    1 };

			radio->ops->conf_tx(hw, vif, 0, classes[record[0] >> 5 & 3], &params);
		}
		mutex_unlock(&hw->wiphy->mtx);
	}
	printf("lx: the driver is told of the association (rates %#x, HT %s, VHT %s, %u streams)\n",
	    radio->sta->deflink.supp_rates[channel->band], radio->sta->deflink.ht_cap.ht_supported ? "yes" : "no",
	    radio->sta->deflink.vht_cap.vht_supported ? "yes" : "no", radio->sta->deflink.rx_nss);

	lx_os_lock();
	if (radio->pending_eapol_len != 0) {
		uint8_t pending[sizeof(radio->pending_eapol)];
		unsigned pending_len = radio->pending_eapol_len;
		memcpy(pending, radio->pending_eapol, pending_len);
		radio->pending_eapol_len = 0;
		lx_os_unlock();
		lx_handshake_message(radio, pending, pending_len);
	} else {
		lx_os_unlock();
	}

	/* the handshake: the access point begins it; its messages come in as frames and are answered where they come in */
	began = lx_os_nanoseconds();
	lx_os_lock();
	while (radio->handshake != LX_WPA_KEYS && radio->handshake != LX_WPA_BAD && radio->answer != ANSWER_GONE &&
	    lx_os_nanoseconds() - began < (radio->handshake == LX_WPA_NOTHING ? 2500000000ull : 8000000000ull)) {
		lx_os_sleep(&radio->handshake, 20 * 1000000ull);
	}
	lx_os_unlock();
	if (radio->handshake != LX_WPA_KEYS) {
		lx_tell("lx: THE HANDSHAKE DID NOT COME THROUGH in %llu ms: %s%s (%u of its messages came)\n", (lx_os_nanoseconds() - began) /
		    1000000ull, radio->answer == ANSWER_GONE ? "the access point has shown us the door (a wrong passphrase gives this)" :
		    radio->handshake == LX_WPA_BAD ? "a message was not good: " : radio->handshake == LX_WPA_ANSWER ? "our message 2 was "
		    "not answered (a wrong passphrase gives this)" : "the access point did not begin it", radio->handshake == LX_WPA_BAD ?
		    radio->handshake_why : "", radio->eapol);
		error = EAUTH;
		goto out;
	}
	lx_pause_ms(30);			/* (for message 4 to have left unciphered, before there is a key to cipher with) */
	radio->pairwise_key = key_set(radio, radio->wpa.pairwise, 0, true, &ret);
	if (ret == 0) {
		radio->group_key = key_set(radio, radio->wpa.group, radio->wpa.group_index, false, &ret);
	}
	lx_tell("lx: THE KEYS ARE GIVEN TO THE DRIVER, %llu ms after the association: ours with the access point, and the group's "
	    "(number %u, %u bytes): %d\n", (lx_os_nanoseconds() - began) / 1000000ull, radio->wpa.group_index,
	    radio->wpa.group_length, ret);
	if (ret != 0 || (ret = station_state(radio, IEEE80211_STA_ASSOC, IEEE80211_STA_AUTHORIZED)) != 0) {
		error = -ret;
		goto out;
	}
	radio->reached = IEEE80211_STA_AUTHORIZED;
	radio->sent = radio->sent_lost = radio->received = radio->received_group = radio->rekeys = 0;
	radio->joined_at = lx_os_nanoseconds();
	radio->joined = true;
	lx_os_net_link(1);
	lx_tell("lx: THE NETWORK IS JOINED: frames can pass; the access point is heard at %d dBm on its channel (the scan said %d), "
	    "%u of its beacons so far\n", radio->signal, target->signal, radio->beacons);
	return (0);
out:
	station_leave(radio);
	return (error);
}

/* ---- for the rest of the layer ---------------------------------------------------------------------------- */

/* Join the network the kernel was given the name and passphrase of. 0 when frames can pass, or an errno. */
int
lx_station_join(void)
{
	static struct redwood_sta_join what;
	struct lx_radio *radio = the_radio;
	unsigned name_length = sizeof(what.ssid), passphrase_length = sizeof(what.passphrase);
	int error;

	if (radio == NULL || !radio->registered) {
		lx_tell("lx: no radio is registered: nothing to join with\n");
		return (ENXIO);
	}
	bzero(&what, sizeof(what));
	if (!lx_os_network(what.ssid, &name_length, what.passphrase, &passphrase_length)) {
		return (ENOENT);
	}
	what.ssid_length = (uint8_t)name_length;
	what.passphrase_length = (uint8_t)passphrase_length;
	what.security = REDWOOD_STA_WPA2_PSK;
	lx_os_net_attach(radio->wiphy->perm_addr);
	error = station_join(radio, &what);
	explicit_bzero(&what, sizeof(what));
	return (error);
}

/* What has passed since the network was joined. */
void
lx_station_say(void)
{
	struct lx_radio *radio = the_radio;

	if (radio != NULL) {
		lx_tell("lx: the network is %s, %llu s after it was joined: %u frames sent (%u could not be), %u received for us and "
		    "%u sent to everybody; %u messages of the handshake, the group's key changed %u times; block ack: %u classes "
		    "taken, %u refused; the access point last heard at %d dBm, at its weakest %d, %u of its beacons since the join\n",
		    radio->joined ? "joined" : "NOT joined", radio->joined_at != 0 ? (lx_os_nanoseconds() - radio->joined_at) /
		    1000000000ull : 0, radio->sent, radio->sent_lost, radio->received, radio->received_group, radio->eapol,
		    radio->rekeys, radio->ba_sessions, radio->ba_refused, radio->signal, radio->weakest, radio->beacons);
	}
}

/* Leave the network, if one is joined, and take the interface down. */
void
lx_station_leave(void)
{
	struct lx_radio *radio = the_radio;

	if (radio == NULL || radio->vif == NULL) {
		return;
	}
	lx_station_say();
	if (radio->sta != NULL) {
		station_leave(radio);
		lx_tell("lx: the network is left\n");
	}
	lx_station_down(radio);
}

/* An Ethernet frame from the kernel for the network. 0, or an errno. */
int
lx_station_send(const unsigned char *frame, unsigned long length)
{
	struct lx_radio *radio = the_radio;

	if (radio == NULL || !radio->joined) {
		return (ENETDOWN);
	}
	return (lx_ethernet_send(radio, frame, (unsigned)length) ? 0 : ENOBUFS);
}

/* ---- the station boundary: what the kernel asks through sys/redwood_sta.h --------------------------------- */

static int
boundary_power(bool on)
{
	struct lx_radio *radio = the_radio;

	if (radio == NULL || !radio->registered) {
		return (ENXIO);
	}
	if (on) {
		return (lx_station_up(radio));
	}
	if (radio->sta != NULL) {
		station_leave(radio);
		lx_tell("lx: the network is left\n");
	}
	lx_station_down(radio);
	return (0);
}

static int
boundary_scan(const struct redwood_sta_scan *what)
{
	static unsigned said;
	struct lx_radio *radio = the_radio;
	unsigned long long began;
	int error;

	if (radio == NULL || !radio->registered) {
		return (ENXIO);
	}
	if ((error = lx_station_up(radio)) != 0) {
		return (error);
	}
	began = lx_os_nanoseconds();
	error = lx_scan(radio, what->ssid, what->ssid_length <= sizeof(what->ssid) ? what->ssid_length : 0, what->passive, what);
	if (said < 24) {			/* (the first few are told: whoever asks goes on asking as long as the system runs) */
		said++;
		lx_tell("lx: a scan for the kernel: %u channels named (0: all), %s, %u ms on each (0: the radio's own): %d; %u access "
		    "points heard, %u frames, in %llu ms\n", what->frequency_count, what->passive ? "listening" : "asking",
		    what->dwell_ms, error, radio->heard_count, radio->frames, (lx_os_nanoseconds() - began) / 1000000ull);
	}
	return (error);
}

static void
boundary_heard(void (*each)(const struct redwood_sta_bss *, void *), void *context)
{
	static struct redwood_sta_bss bss;	/* (one thread calls this) */
	struct lx_radio *radio = the_radio;

	for (unsigned i = 0; radio != NULL && i < radio->heard_count; i++) {
		const struct lx_heard *network = &radio->heard[i];

		lx_os_lock();
		bzero(&bss, sizeof(bss));
		memcpy(bss.bssid, network->sender, ETH_ALEN);
		memcpy(bss.ssid, network->name, network->name_length);
		bss.ssid_length = network->name_length;
		/* (a beacon can be heard on the channel next to its own: where it says it is counts) */
		bss.frequency = network->says_channel != 0 ? (uint16_t)linuxkpi_ieee80211_channel_to_frequency(network->says_channel,
		    network->frequency < 4000 ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ) : network->frequency;
		bss.rssi = network->signal;
		bss.beacon_interval = network->interval;
		bss.capability = network->capability;
		bss.age_ms = (uint32_t)((lx_os_nanoseconds() - network->last) / 1000000ull);
		bss.elements_length = network->elements_length;
		memcpy(bss.elements, network->elements, network->elements_length);
		lx_os_unlock();
		each(&bss, context);
	}
}

static int
boundary_join(const struct redwood_sta_join *what)
{
	struct lx_radio *radio = the_radio;
	int error;

	if (radio == NULL || !radio->registered) {
		return (ENXIO);
	}
	radio->joining = true;
	error = station_join(radio, what);
	if (error == EAUTH && radio->handshake != LX_WPA_BAD && radio->answer != ANSWER_GONE) {
		lx_tell("lx: retrying handshake once after initial association timeout\n");
		error = station_join(radio, what);
	}
	radio->last_error = error;
	radio->joining = false;
	return (error);
}

static int
boundary_leave(void)
{
	struct lx_radio *radio = the_radio;

	if (radio != NULL && radio->sta != NULL) {
		lx_station_say();
		station_leave(radio);
		radio->last_error = 0;
		lx_tell("lx: the network is left\n");
	}
	return (0);
}

/* (on any thread: nothing here but reading) */
static void
boundary_state(struct redwood_sta_state *state)
{
	struct lx_radio *radio = the_radio;

	bzero(state, sizeof(*state));
	if (radio == NULL || radio->vif == NULL) {
		return;
	}
	lx_os_lock();
	state->phase = radio->joined ? REDWOOD_STA_JOINED : radio->joining ? REDWOOD_STA_JOINING : radio->scan == 1 ?
	    REDWOOD_STA_SCANNING : REDWOOD_STA_IDLE;
	state->last_error = radio->last_error;
	if ((radio->joined || radio->joining) && radio->target.found) {
		memcpy(state->bss.bssid, radio->target.address, ETH_ALEN);
		memcpy(state->bss.ssid, radio->want, radio->want_length);
		state->bss.ssid_length = radio->want_length;
		state->bss.frequency = radio->channel != NULL && radio->channel->def.chan != NULL ?
		    radio->channel->def.chan->center_freq : radio->target.frequency;
		state->bss.rssi = radio->joined ? radio->signal : radio->target.signal;
		state->bss.beacon_interval = radio->target.interval;
		state->bss.capability = radio->target.capability;
		state->bss.elements_length = (uint16_t)radio->target.elements_length;
		memcpy(state->bss.elements, radio->target.elements, radio->target.elements_length);
	}
	lx_os_unlock();
}

static unsigned
boundary_channels(struct redwood_sta_channel *channels, unsigned most)
{
	struct lx_radio *radio = the_radio;
	unsigned count = 0;

	for (unsigned band = 0; radio != NULL && band < NUM_NL80211_BANDS; band++) {
		const struct ieee80211_supported_band *has = radio->wiphy->bands[band];

		for (int i = 0; has != NULL && i < has->n_channels && count < most; i++) {
			if (!(has->channels[i].flags & IEEE80211_CHAN_DISABLED)) {
				channels[count].frequency = (uint16_t)has->channels[i].center_freq;
				channels[count++].flags = (uint16_t)((has->channels[i].flags & IEEE80211_CHAN_NO_IR ? REDWOOD_STA_CHANNEL_PASSIVE :
				    0) | (has->channels[i].flags & IEEE80211_CHAN_RADAR ? REDWOOD_STA_CHANNEL_RADAR : 0));
			}
		}
	}
	return (count);
}

static struct redwood_sta_ops boundary = {
	.power = boundary_power,
	.scan = boundary_scan,
	.heard = boundary_heard,
	.join = boundary_join,
	.leave = boundary_leave,
	.state = boundary_state,
	.channels = boundary_channels,
};

/*
 * The radio is the kernel's to ask from now on: an interface is brought up on it, and the
 * kernel is given the calls above. 0, or an errno.
 */
int
lx_station_offer(void)
{
	struct lx_radio *radio = the_radio;
	int error;

	if (radio == NULL || !radio->registered) {
		lx_tell("lx: no radio is registered: there is no station to offer\n");
		return (ENXIO);
	}
	lx_os_net_attach(radio->wiphy->perm_addr);
	if ((error = lx_station_up(radio)) != 0) {
		return (error);
	}
	memcpy(boundary.address, radio->wiphy->perm_addr, ETH_ALEN);
	lx_os_station(&boundary);
	return (0);
}

/* ... and no longer: returns when no call of the kernel's is under way. */
void
lx_station_withdraw(void)
{
	lx_os_station(NULL);
}

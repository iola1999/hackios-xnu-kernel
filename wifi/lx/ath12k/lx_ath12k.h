/*
 * What ath12k (Linux 7.0) uses of Linux and the headers of LinuxKPI (FreeBSD 15.1) do not have,
 * beyond what ath11k needed (include/lx_more.h): included before every file of the ath12k build.
 * The values are those of IEEE Std 802.11be-2024 and of radiotap.org; the functions are written
 * here from what the standard says the fields mean.
 */
#ifndef _LX_ATH12K_H_
#define _LX_ATH12K_H_

#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/cleanup.h>
#include <linux/bitfield.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

/* --- lists, locks, RCU */
#ifndef LIST_HEAD_INIT
#define LIST_HEAD_INIT(name)		LINUX_LIST_HEAD_INIT(name)
#endif

/* move the entries of HEAD before ENTRY to LIST (empty or not, it is overwritten); ENTRY stays on HEAD */
static inline void
list_cut_before(struct list_head *list, struct list_head *head, struct list_head *entry)
{
	if (head->next == entry) {
		INIT_LIST_HEAD(list);
		return;
	}
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry->prev;
	list->prev->next = list;
	head->next = entry;
	entry->prev = head;
}

DEFINE_LOCK_GUARD_1(spinlock_bh, spinlock_t, spin_lock_bh(_T->lock), spin_unlock_bh(_T->lock))

#define RCU_LOCKDEP_WARN(condition, message)	do { } while (0)
#define rcu_read_lock_any_held()		1

/* --- socket buffers: ours are all one piece (src/lx_base.c), so there is never a fragment to copy */
static inline int
skb_cow_data(struct sk_buff *skb, int tail, struct sk_buff **trailer)
{
	if (skb_tailroom(skb) < tail)
		return (-ENOMEM);
	*trailer = skb;
	return (1);
}

static inline void *
pskb_put(struct sk_buff *skb, struct sk_buff *tail, int length)
{
	return (skb_put(tail, length));
}

/* --- 802.11 */
#define KHZ_TO_MHZ(frequency)			((frequency) / 1000)

#define WLAN_ACTION_ADDBA_RESP			1
#define WLAN_EID_NON_TX_BSSID_CAP		83
#define WLAN_EXT_CAPA11_BCN_PROTECT		BIT(4)
#define IEEE80211_EHT_OPER_MCS15_DISABLE	0x40
#define IEEE80211_EHT_PHY_CAP4_PSR_SR_SUPP	0x02
#define IEEE80211_TPE_EIRP_ENTRIES_320MHZ	5
#define IEEE80211_TPE_PSD_ENTRIES_320MHZ	16

/* an extension element (element ID 255) by its extension ID; NULL if there is none */
static inline const struct element *
cfg80211_find_ext_elem(u8 ext_id, const u8 *data, size_t length)
{
	const u8 *e = cfg80211_find_ext_ie(ext_id, data, length);

	return ((const struct element *)e);
}

/*
 * The size in bytes of the Supported EHT-MCS And NSS Set field (802.11be 9.4.2.323): 4 for a
 * station that only has 20 MHz, else 3 for each of the widths up to 80, 160 and 320 MHz.
 */
static inline u8
ieee80211_eht_mcs_nss_size(const struct ieee80211_he_cap_elem *he, const struct ieee80211_eht_cap_elem_fixed *eht,
    bool from_ap)
{
	u8 widths = he->phy_cap_info[0];
	u8 size = 0;

	if (widths & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G)
		return (3);
	if (widths & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G)
		size += 3;
	if (widths & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G)
		size += 3;
	if (eht->phy_cap_info[0] & IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ)
		size += 3;
	return (size != 0 ? size : from_ap ? 3 : 4);
}

/*
 * The EML Capabilities subfield of a multi-link element (802.11be Tables 9-417i, j, m): codes
 * 1..4 of the padding delay are 32, 64, 128, 256 us; 1..5 of the transition delay 16..256 us;
 * 1..11 of the transition timeout 128 us..128 TU. Codes outside those mean 0.
 */
static inline u32
ieee80211_emlsr_pad_delay_in_us(u16 eml_cap)
{
	u32 code = u16_get_bits(eml_cap, IEEE80211_EML_CAP_EMLSR_PADDING_DELAY);

	return (code >= 1 && code <= 4 ? 16u << code : 0);
}

static inline u32
ieee80211_emlsr_trans_delay_in_us(u16 eml_cap)
{
	u32 code = u16_get_bits(eml_cap, IEEE80211_EML_CAP_EMLSR_TRANSITION_DELAY);

	return (code >= 1 && code <= 5 ? 8u << code : 0);
}

static inline u32
ieee80211_eml_trans_timeout_in_us(u16 eml_cap)
{
	u32 code = u16_get_bits(eml_cap, IEEE80211_EML_CAP_TRANSITION_TIMEOUT);

	return (code >= 1 && code <= 11 ? 64u << code : 0);
}

/* mac80211's: whether the driver has stopped the queue (lx_80211.c never stops one) */
bool ieee80211_queue_stopped(struct ieee80211_hw *hw, int queue);

/* --- radiotap's EHT fields (radiotap.org, "EHT" and "U-SIG"), for the monitor's headers */
#define IEEE80211_RADIOTAP_EHT_KNOWN_DISREGARD_O		0x00000100
#define IEEE80211_RADIOTAP_EHT_KNOWN_DISREGARD_S		0x00000200
#define IEEE80211_RADIOTAP_EHT_KNOWN_CRC1			0x00002000
#define IEEE80211_RADIOTAP_EHT_KNOWN_TAIL1			0x00004000
#define IEEE80211_RADIOTAP_EHT_KNOWN_RU_MRU_SIZE_OM		0x00400000
#define IEEE80211_RADIOTAP_EHT_KNOWN_RU_MRU_INDEX_OM		0x00800000
#define IEEE80211_RADIOTAP_EHT_DATA0_DISREGARD_S		0x000c0000
#define IEEE80211_RADIOTAP_EHT_DATA0_DISREGARD_O		0x003c0000
#define IEEE80211_RADIOTAP_EHT_DATA0_CRC1_O			0x03c00000
#define IEEE80211_RADIOTAP_EHT_DATA1_RU_SIZE			0x0000001f
#define IEEE80211_RADIOTAP_EHT_DATA1_RU_INDEX			0x00001fe0
#define IEEE80211_RADIOTAP_EHT_USER_INFO_SPATIAL_CONFIG_KNOWN_M	0x00000040
#define IEEE80211_RADIOTAP_EHT_USER_INFO_SPATIAL_CONFIG_M	0x3f000000
/* (net80211 has the fields of these three as macros; the driver names the types as well) */
enum ieee80211_radiotap_eht_data { LX_RADIOTAP_EHT_DATA };
enum ieee80211_radiotap_eht_usig_mu { LX_RADIOTAP_EHT_USIG_MU };
enum ieee80211_radiotap_eht_usig_tb { LX_RADIOTAP_EHT_USIG_TB };

#endif

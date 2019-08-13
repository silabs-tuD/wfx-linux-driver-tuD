/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Datapath implementation.
 *
 * Copyright (c) 2017-2019, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 */
#ifndef WFX_DATA_TX_H
#define WFX_DATA_TX_H

#include <linux/list.h>

#include "wsm_cmd_api.h"

struct wfx_txpriv;
struct wfx_dev;
struct wfx_vif;

struct tx_policy {
	union {
		__le32 tbl[3];
		u8 raw[12];
	};
	u8  defined;
	u8  usage_count;
	u8  retry_count;
	u8  uploaded;
};

struct tx_policy_cache_entry {
	struct tx_policy policy;
	struct list_head link;
};

struct tx_policy_cache {
	struct tx_policy_cache_entry cache[WSM_MIB_NUM_TX_RATE_RETRY_POLICIES];
	struct list_head used;
	struct list_head free;
	spinlock_t lock;
};

struct wfx_ht_info {
	struct ieee80211_sta_ht_cap	ht_cap;
	enum nl80211_channel_type	channel_type;
	u16				operation_mode;
};

void tx_policy_init(struct wfx_vif *wvif);
void tx_policy_upload_work(struct work_struct *work);

void wfx_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
	    struct sk_buff *skb);
void wfx_tx_confirm_cb(struct wfx_dev *wdev, WsmHiTxCnfBody_t *arg);
void wfx_skb_dtor(struct wfx_dev *wdev, struct sk_buff *skb,
		  const struct wfx_txpriv *txpriv);

void wfx_link_id_work(struct work_struct *work);
void wfx_link_id_gc_work(struct work_struct *work);
void wfx_link_id_reset_work(struct work_struct *work);
int wfx_find_link_id(struct wfx_vif *wvif, const u8 *mac);

static inline int wfx_is_ht(const struct wfx_ht_info *ht_info)
{
	return ht_info->channel_type != NL80211_CHAN_NO_HT;
}

static inline int wfx_ht_greenfield(const struct wfx_ht_info *ht_info)
{
	return wfx_is_ht(ht_info) &&
		(ht_info->ht_cap.cap & IEEE80211_HT_CAP_GRN_FLD) &&
		!(ht_info->operation_mode &
		  IEEE80211_HT_OP_MODE_NON_GF_STA_PRSNT);
}

static inline int wfx_ht_fecCoding(const struct wfx_ht_info *ht_info)
{
	return wfx_is_ht(ht_info) &&
	       (ht_info->ht_cap.cap & IEEE80211_HT_CAP_LDPC_CODING);
}

static inline int wfx_ht_shortGi(const struct wfx_ht_info *ht_info)
{
	return wfx_is_ht(ht_info) &&
	       (ht_info->ht_cap.cap & IEEE80211_HT_CAP_SGI_20);
}

static inline int wfx_ht_ampdu_density(const struct wfx_ht_info *ht_info)
{
	if (!wfx_is_ht(ht_info))
		return 0;
	return ht_info->ht_cap.ampdu_density;
}

#endif /* WFX_DATA_TX_H */
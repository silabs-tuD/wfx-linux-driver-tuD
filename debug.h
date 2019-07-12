/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Debugfs interface.
 *
 * Copyright (c) 2017-2019, Silicon Laboratories, Inc.
 * Copyright (c) 2011, ST-Ericsson
 */
#ifndef WFX_DEBUG_H
#define WFX_DEBUG_H

#include <linux/printk.h>
#include <linux/list.h>

#include "wfx.h"

struct wfx_dbg_param;

struct wfx_debug_priv {
	int tx;
	int tx_agg;
	int rx;
	int rx_agg;
	int tx_multi;
	int tx_multi_frames;
	int tx_cache_miss;
	int tx_align;
	int tx_burst;
	int ba_cnt;
	int ba_acc;
	int ba_cnt_rx;
	int ba_acc_rx;
	struct wfx_dbg_param *dbg_params;
	struct list_head dbg_params_active;
};

int wfx_debug_init(struct wfx_dev *wdev);
void wfx_dbg_filter_wsm(struct wfx_dev *wdev, void *buf);

const char *get_wsm_name(unsigned long id);
const char *get_mib_name(unsigned long id);
const char *get_reg_name(unsigned long id);
const char *get_fw_type(unsigned long id);

static inline void wfx_debug_txed(struct wfx_dev *wdev)
{
	++wdev->debug->tx;
}

static inline void wfx_debug_txed_agg(struct wfx_dev *wdev)
{
	++wdev->debug->tx_agg;
}

static inline void wfx_debug_txed_multi(struct wfx_dev *wdev, int count)
{
	++wdev->debug->tx_multi;
	wdev->debug->tx_multi_frames += count;
}

static inline void wfx_debug_rxed(struct wfx_dev *wdev)
{
	++wdev->debug->rx;
}

static inline void wfx_debug_rxed_agg(struct wfx_dev *wdev)
{
	++wdev->debug->rx_agg;
}

static inline void wfx_debug_tx_cache_miss(struct wfx_dev *wdev)
{
	++wdev->debug->tx_cache_miss;
}

static inline void wfx_debug_tx_align(struct wfx_dev *wdev)
{
	++wdev->debug->tx_align;
}

static inline void wfx_debug_tx_burst(struct wfx_dev *wdev)
{
	++wdev->debug->tx_burst;
}

static inline void wfx_debug_ba(struct wfx_dev *wdev,
				int ba_cnt, int ba_acc,
				int ba_cnt_rx, int ba_acc_rx)
{
	wdev->debug->ba_cnt = ba_cnt;
	wdev->debug->ba_acc = ba_acc;
	wdev->debug->ba_cnt_rx = ba_cnt_rx;
	wdev->debug->ba_acc_rx = ba_acc_rx;
}

#endif /* WFX_DEBUG_H */

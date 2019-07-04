// SPDX-License-Identifier: GPL-2.0-only
/*
 * Interrupt bottom half (BH).
 *
 * Copyright (c) 2017-2019, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 */
#include <linux/gpio/consumer.h>
#include <net/mac80211.h>

#include "bh.h"
#include "wfx.h"
#include "hwio.h"
#include "debug.h"
#include "wsm_rx.h"
#include "traces.h"
#include "secure_link.h"

static void device_wakeup(struct wfx_dev *wdev)
{
	if (!wdev->pdata.gpio_wakeup)
		return;
	if (gpiod_get_value(wdev->pdata.gpio_wakeup))
		return;

	gpiod_set_value(wdev->pdata.gpio_wakeup, 1);
	if (wfx_api_older_than(wdev, 1, 4)) {
		if (!completion_done(&wdev->hif.ctrl_ready))
			udelay(2000);
	} else {
		// completion.h does not provide any function to wait
		// completion without consume it (a kind of
		// wait_for_completion_done_timeout()). So we have to emulate
		// it.
		if (wait_for_completion_timeout(&wdev->hif.ctrl_ready, msecs_to_jiffies(2) + 1))
			complete(&wdev->hif.ctrl_ready);
		else
			dev_err(wdev->dev, "timeout while wake up chip\n");
	}
}

static void device_release(struct wfx_dev *wdev)
{
	if (!wdev->pdata.gpio_wakeup)
		return;

	gpiod_set_value(wdev->pdata.gpio_wakeup, 0);
}

static int rx_helper(struct wfx_dev *wdev, size_t read_len, int *is_cnf)
{
	struct sk_buff *skb;
	struct wmsg *wsm;
	size_t alloc_len;
	size_t computed_len;
	int release_count;
	int piggyback = 0;

	WARN_ON(read_len < 4);

	// Add 2 to take into account piggyback size
	alloc_len = wdev->hwbus_ops->align_size(wdev->hwbus_priv, read_len + 2);
	skb = dev_alloc_skb(alloc_len);
	if (!skb)
		return -ENOMEM;

	if (wfx_data_read(wdev, skb->data, alloc_len))
		goto err;

	piggyback = le16_to_cpup((u16 *) (skb->data + alloc_len - 2));
	_trace_piggyback(piggyback, false);

	wsm = (struct wmsg *) skb->data;
	WARN(wsm->encrypted & 0x1, "unsupported encryption type");
	if (wsm->encrypted == 0x2) {
		if (wfx_sl_decode(wdev, (void *) wsm))
			goto err;
		le16_to_cpus(wsm->len);
		computed_len = round_up(wsm->len - sizeof(wsm->len), 16)
			       + sizeof(struct sl_wmsg)
			       + sizeof(struct sl_tag);
	} else {
		le16_to_cpus(wsm->len);
		computed_len = round_up(wsm->len, 2);
	}
	if (computed_len != read_len) {
		dev_err(wdev->dev, "inconsistent message length: %zu != %zu\n",
			computed_len, read_len);
		print_hex_dump(KERN_INFO, "wsm: ", DUMP_PREFIX_OFFSET, 16, 1,
			       wsm, read_len, true);
		goto err;
	}
	_trace_wsm_recv(wsm);

	if (wsm->id != HI_EXCEPTION_IND_ID && wsm->id != HI_ERROR_IND_ID) {
		if (wsm->seqnum != wdev->hif.rx_seqnum)
			dev_warn(wdev->dev, "wrong message sequence: %d != %d\n",
				 wsm->seqnum, wdev->hif.rx_seqnum);
		wdev->hif.rx_seqnum = (wsm->seqnum + 1) % (WMSG_COUNTER_MAX + 1);
	}

	if (!(wsm->id & WMSG_ID_IS_INDICATION)) {
		(*is_cnf)++;
		if (wsm->id == WSM_HI_MULTI_TRANSMIT_CNF_ID)
			release_count = le32_to_cpu(((WsmHiMultiTransmitCnfBody_t *) wsm->body)->NumTxConfs);
		else
			release_count = 1;
		WARN(wdev->hif.tx_buffers_used < release_count, "corrupted buffer counter");
		wdev->hif.tx_buffers_used -= release_count;
		if (!wdev->hif.tx_buffers_used)
			wake_up(&wdev->hif.tx_buffers_empty);
	}

	skb_put(skb, wsm->len);
	// wfx_wsm_rx takes care on SKB livetime
	wsm_handle_rx(wdev, wsm, &skb);

	if (skb)
		dev_kfree_skb(skb);

	return piggyback;

err:
	if (skb)
		dev_kfree_skb(skb);
	return -EIO;
}

static int bh_work_rx(struct wfx_dev *wdev, int max_msg, int *num_cnf)
{
	size_t len;
	int i;
	int ctrl_reg, piggyback;

	piggyback = 0;
	for (i = 0; i < max_msg; i++) {
		if (piggyback & CTRL_NEXT_LEN_MASK)
			ctrl_reg = piggyback;
		else if (try_wait_for_completion(&wdev->hif.ctrl_ready))
			ctrl_reg = atomic_xchg(&wdev->hif.ctrl_reg, 0);
		else
			ctrl_reg = 0;
		if (!(ctrl_reg & CTRL_NEXT_LEN_MASK))
			return i;
		// ctrl_reg units are 16bits words
		len = (ctrl_reg & CTRL_NEXT_LEN_MASK) * 2;
		piggyback = rx_helper(wdev, len, num_cnf);
		if (piggyback < 0)
			return i;
		if (!(piggyback & CTRL_WLAN_READY))
			dev_err(wdev->dev, "unexpected piggyback value: ready bit not set: %04x", piggyback);
	}
	if (piggyback & CTRL_NEXT_LEN_MASK) {
		ctrl_reg = atomic_xchg(&wdev->hif.ctrl_reg, piggyback);
		complete(&wdev->hif.ctrl_ready);
		if (ctrl_reg)
			dev_err(wdev->dev, "unexpected IRQ happened: %04x/%04x", ctrl_reg, piggyback);
	}
	return i;
}

static void tx_helper(struct wfx_dev *wdev, u8 *data, size_t len)
{
	int ret;
	struct wmsg *wsm;
	bool is_encrypted = false;

	wsm = (struct wmsg *) data;
	BUG_ON(len < sizeof(*wsm));
	BUG_ON(wsm->len != len);

	wsm->seqnum = wdev->hif.tx_seqnum;
	wdev->hif.tx_seqnum = (wdev->hif.tx_seqnum + 1) % (WMSG_COUNTER_MAX + 1);

	if (wfx_is_secure_command(wdev, wsm->id)) {
		len = round_up(len - sizeof(wsm->len), 16) + sizeof(wsm->len) + 4 + sizeof(struct sl_tag);
		// FIXME: It may be possible to encrypt wsm in-place (AES
		// support in-place encryption). However, it is also necessary
		// to shift buffer to add secure link header. In add, do we
		// garantee that data are no more necessary after sent?
		data = kmalloc(len, GFP_KERNEL);
		if (!data)
			goto end;
		is_encrypted = true;
		ret = wfx_sl_encode(wdev, wsm, (void *) data);
		if (ret)
			goto end;
	}
	len = wdev->hwbus_ops->align_size(wdev->hwbus_priv, len);
	ret = wfx_data_write(wdev, data, len);
	if (ret)
		goto end;

	_trace_wsm_send(wsm);
	wdev->hif.tx_buffers_used++;
end:
	if (is_encrypted)
		kfree(data);
}

static int bh_work_tx(struct wfx_dev *wdev, int max_msg)
{
	u8 *data;
	size_t len;
	int i;

	for (i = 0; i < max_msg; i++) {
		data = NULL;
		if (wdev->hif.tx_buffers_used < wdev->wsm_caps.NumInpChBufs)
			wsm_get_tx(wdev, &data, &len);
		if (!data)
			return i;
		tx_helper(wdev, data, len);
	}
	return i;
}

/* In SDIO mode, it is necessary to make an access to a register to acknowledge
 * last received message. It could be possible to restrict this acknowledge to
 * SDIO mode and only if last operation was rx.
 */
static void ack_sdio_data(struct wfx_dev *wdev)
{
	uint32_t cfg_reg;

	config_reg_read(wdev, &cfg_reg);
	if (cfg_reg & 0xFF) {
		dev_warn(wdev->dev, "chip reports errors: %02x\n", cfg_reg & 0xFF);
		config_reg_write_bits(wdev, 0xFF, 0x00);
	}
}

static void bh_work(struct work_struct *work)
{
	struct wfx_dev *wdev = container_of(work, struct wfx_dev, hif.bh);
	int stats_req = 0, stats_cnf = 0, stats_ind = 0;
	bool release_chip, last_op_is_rx = false;
	int num_tx, num_rx;

	device_wakeup(wdev);
	do {
		num_tx = bh_work_tx(wdev, 32);
		stats_req += num_tx;
		if (num_tx)
			last_op_is_rx = false;
		num_rx = bh_work_rx(wdev, 32, &stats_cnf);
		stats_ind += num_rx;
		if (num_rx)
			last_op_is_rx = true;
	} while (num_rx || num_tx);
	stats_ind -= stats_cnf;

	if (last_op_is_rx)
		ack_sdio_data(wdev);
	if (!wdev->hif.tx_buffers_used && !work_pending(work) && !atomic_read(&wdev->scan_in_progress)) {
		device_release(wdev);
		release_chip = true;
	} else {
		release_chip = false;
	}
	_trace_bh_stats(stats_ind, stats_req, stats_cnf, wdev->hif.tx_buffers_used, release_chip);
}

/*
 * An IRQ from chip did occur
 */
void wfx_bh_request_rx(struct wfx_dev *wdev)
{
	u32 cur, prev;

	control_reg_read(wdev, &cur);
	prev = atomic_xchg(&wdev->hif.ctrl_reg, cur);
	complete(&wdev->hif.ctrl_ready);
	queue_work(system_highpri_wq, &wdev->hif.bh);

	if (!(cur & CTRL_NEXT_LEN_MASK))
		dev_err(wdev->dev, "unexpected control register value: length field is 0: %04x", cur);
	if (prev != 0)
		dev_err(wdev->dev, "received IRQ but previous data was not (yet) read: %04x/%04x", prev, cur);
}

/*
 * Driver want to send data
 */
void wfx_bh_request_tx(struct wfx_dev *wdev)
{
	queue_work(system_highpri_wq, &wdev->hif.bh);
}

void wfx_bh_register(struct wfx_dev *wdev)
{
	INIT_WORK(&wdev->hif.bh, bh_work);
	init_completion(&wdev->hif.ctrl_ready);
	init_waitqueue_head(&wdev->hif.tx_buffers_empty);
}

void wfx_bh_unregister(struct wfx_dev *wdev)
{
	flush_work(&wdev->hif.bh);
}


// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#include <linux/mailbox_client.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include "hfi_interface.h"
#include "hfi_core.h"
#include "hfi_core_debug.h"
#include "hfi_ipc.h"

#define IRQ_LABEL_SIZE                                             32
#define MBOX_POWER_IDX                                              0
#define MBOX_XFER_IDX                                               1

struct hfi_irq_signal_info {
	u32 irq;
	char irq_label[IRQ_LABEL_SIZE];
};

enum mbox_channel_type {
	MBOX_CHAN_POWER            = 0x1,
	MBOX_CHAN_XFER             = 0x2,
	MBOX_CHAN_MAX              = 0x3,
};

struct hfi_mbox_info {
	enum hfi_core_client_id client_id;
	struct mbox_client client_data;
	struct mbox_chan *power_chan;
	struct mbox_chan *xfer_chan;
	struct hfi_irq_signal_info irq_power;
	struct hfi_irq_signal_info irq_xfer;
	hfi_ipc_cb mbox_ipc_cb;
};

static int mbox_init(struct hfi_core_drv_data *drv_data,
	enum hfi_core_client_id client_id)
{
	struct device *dev = drv_data->dev;
	struct mbox_client *mbox_client;
	struct hfi_mbox_info *mbox_ipc; // ipc_data
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!dev || client_id >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid device or client id: %u\n",
			client_id);
		return -EINVAL;
	}

	mbox_ipc = kzalloc(sizeof(*mbox_ipc), GFP_KERNEL);
	if (!mbox_ipc) {
		ret = -ENOMEM;
		goto error;
	}

	mbox_client = &mbox_ipc->client_data;

	mbox_client->dev = dev;
	mbox_client->knows_txdone = true;
	mbox_ipc->power_chan = mbox_request_channel(mbox_client, MBOX_POWER_IDX);
	if (IS_ERR(mbox_ipc->power_chan)) {
		ret = PTR_ERR(mbox_ipc->power_chan);
		if (ret != -EPROBE_DEFER)
			HFI_CORE_ERR("failed to acquire IPC power channel, ret : %d\n",
				ret);
		goto free_mbox;
	}

	mbox_ipc->xfer_chan = mbox_request_channel(mbox_client, MBOX_XFER_IDX);
	if (IS_ERR(mbox_ipc->xfer_chan)) {
		ret = PTR_ERR(mbox_ipc->xfer_chan);
		if (ret != -EPROBE_DEFER)
			HFI_CORE_ERR("failed to acquire IPC xfer channel, ret: %d\n",
				ret);
		goto free_chan;
	}

	drv_data->client_data[client_id].ipc_info.data = (void *)mbox_ipc;
	HFI_CORE_DBG_H("mbox init success\n");
	HFI_CORE_DBG_H("-\n");
	return 0;

free_chan:
	mbox_free_channel(mbox_ipc->power_chan);
free_mbox:
	kfree(mbox_ipc);
error:
	return ret;
}

static int mbox_trigger_signal(struct hfi_mbox_info *mbox_ipc,
	enum mbox_channel_type idx, void *msg)
{
	int ret = 0;
	struct mbox_chan *mchan = NULL;
	char *chan_type = "unknmown";

	HFI_CORE_DBG_H("+\n");

	if (!mbox_ipc) {
		HFI_CORE_ERR("invalid device\n");
		return -EINVAL;
	}

	if (idx >= MBOX_CHAN_MAX) {
		HFI_CORE_ERR("unsupported mbox channel id: %u\n", idx);
		return -EINVAL;
	}

	if (idx == MBOX_CHAN_POWER) {
		mchan = mbox_ipc->power_chan;
		chan_type = "POWER";
	} else if (idx == MBOX_CHAN_XFER) {
		mchan = mbox_ipc->xfer_chan;
		chan_type = "XFER";
	}

	if (!mchan) {
		HFI_CORE_ERR("%d[%s] channel is null\n", idx, chan_type);
		return -EINVAL;
	}

	ret = mbox_send_message(mchan, msg);
	mbox_client_txdone(mchan, ret);
	if (ret < 0) {
		HFI_CORE_ERR("failed to send msg for %d[%s] channel ret: %d\n",
			idx, chan_type, ret);
		return ret;
	}

	HFI_CORE_DBG_H("sent msg for %d[%s] channel\n", idx, chan_type);

	HFI_CORE_DBG_H("-\n");
	return 0;
}

static void mbox_deinit(struct hfi_mbox_info *mbox_ipc)
{
	HFI_CORE_DBG_H("+\n");

	if (mbox_ipc->power_chan)
		mbox_free_channel(mbox_ipc->power_chan);

	if (mbox_ipc->xfer_chan)
		mbox_free_channel(mbox_ipc->xfer_chan);

	mbox_ipc->power_chan = NULL;
	mbox_ipc->xfer_chan = NULL;
	kfree(mbox_ipc);

	HFI_CORE_DBG_H("mbox deinit success\n");
	HFI_CORE_DBG_H("-\n");
	return;
}

static irqreturn_t dcp_irq_handler(int irq, void *data)
{
	enum ipc_notification_type ipc_notify;
	struct hfi_mbox_info *mbox_ipc = (struct hfi_mbox_info *)data;
	u32 client_id;

	HFI_CORE_DBG_H("+\n");
	HFI_CORE_DBG_H("irq: %d\n", irq);

	if (!mbox_ipc) {
		HFI_CORE_ERR("mbox data is null, irq: %d\n", irq);
		return IRQ_NONE;
	}

	client_id = mbox_ipc->client_id;
	if (client_id < HFI_CORE_CLIENT_ID_0 ||
		client_id >= HFI_CORE_CLIENT_ID_MAX ) {
		HFI_CORE_ERR("invalid client id: %u\n", client_id);
		return IRQ_NONE;
	}

	if (irq == mbox_ipc->irq_power.irq) {
		ipc_notify = HFI_IPC_EVENT_POWER_NOTIFY;
	} else if (irq == mbox_ipc->irq_xfer.irq) {
		ipc_notify = HFI_IPC_EVENT_QUEUE_NOTIFY;
	} else {
		HFI_CORE_ERR("Unknown irq: %d\n", irq);
		return IRQ_NONE;
	}

	if (mbox_ipc->mbox_ipc_cb)
		mbox_ipc->mbox_ipc_cb(NULL, mbox_ipc->client_id, ipc_notify);

	HFI_CORE_DBG_H("-\n");
	return IRQ_HANDLED;
}

static int setup_irq(struct hfi_core_drv_data *drv_data,
	struct hfi_mbox_info *mbox_ipc, u32 irq_idx)
{
	struct device *dev = (struct device *)drv_data->dev;
	int irq, ret = 0;
	u32 *core_irq_ptr;
	char *irq_label_ptr;
	char *irq_label;

	switch(irq_idx) {
		case MBOX_POWER_IDX:
		{
			core_irq_ptr = &mbox_ipc->irq_power.irq;
			irq_label_ptr = mbox_ipc->irq_power.irq_label;
			irq_label = "hfi-core-irq-power";
			break;
		}
		case MBOX_XFER_IDX:
		{
			core_irq_ptr = &mbox_ipc->irq_xfer.irq;
			irq_label_ptr = mbox_ipc->irq_xfer.irq_label;
			irq_label = "hfi-core-irq-xfer";
			break;
		}
		default:
		{
			HFI_CORE_ERR("invalid irq idx %u\n", irq_idx);
			return -EINVAL;
		}
	}

	/* init mbox channel irq */
	irq = of_irq_get(dev->of_node, irq_idx);
	if (irq < 0)
		return irq;

	*core_irq_ptr = irq;
	snprintf(irq_label_ptr, IRQ_LABEL_SIZE, "%s", irq_label);
	ret = devm_request_irq(dev, *core_irq_ptr, dcp_irq_handler, 0,
		irq_label_ptr, mbox_ipc);
	if (ret) {
		HFI_CORE_ERR("failed to request %s IRQ, ret: %d\n",
			irq_label, ret);
		return ret;
	}
	enable_irq_wake(*core_irq_ptr);

	return 0;
}

static int mbox_irq_init(struct hfi_core_drv_data *drv_data,
	hfi_ipc_cb hfi_core_cb, enum hfi_core_client_id client_id)
{
	struct hfi_mbox_info *mbox_ipc;
	int ret;

	HFI_CORE_DBG_H("+\n");

	if (client_id >= HFI_CORE_CLIENT_ID_MAX ) {
		HFI_CORE_ERR("invalid client id: %u\n", client_id);
		return -EINVAL;
	}

	if (!drv_data->dev || !drv_data->client_data[client_id].ipc_info.data) {
		HFI_CORE_ERR("invalid params in drv data\n");
		return -EINVAL;
	}

	mbox_ipc = (struct hfi_mbox_info *)
		(drv_data->client_data[client_id].ipc_info.data);
	if (!mbox_ipc) {
		HFI_CORE_ERR("mbox_ipc data for client id: %u is null\n",
			client_id);
		return -EINVAL;
	}
	mbox_ipc->client_id = client_id;
	mbox_ipc->mbox_ipc_cb = hfi_core_cb;

	/* init mbox power channel irq */
	ret = setup_irq(drv_data, mbox_ipc, MBOX_POWER_IDX);
	if (ret) {
		HFI_CORE_ERR("failed to setup power IRQ, ret: %d\n", ret);
		return ret;
	}

	/* init mbox xfer channel irq */
	ret = setup_irq(drv_data, mbox_ipc, MBOX_XFER_IDX);
	if (ret) {
		HFI_CORE_ERR("failed to setup xfer IRQ, ret: %d\n", ret);
		return ret;
	}

	HFI_CORE_DBG_H("-\n");
	return ret;
}

static void mbox_irq_deinit(struct hfi_core_drv_data *drv_data,
	enum hfi_core_client_id client_id)
{
	struct device *dev = drv_data->dev;
	struct hfi_mbox_info *mbox_ipc;

	HFI_CORE_DBG_H("+\n");

	if (!dev) {
		HFI_CORE_ERR("invalid device\n");
		return;
	}

	mbox_ipc = (struct hfi_mbox_info *)drv_data->client_data[client_id].ipc_info.data;
	if (mbox_ipc->irq_power.irq) {
		disable_irq_wake(mbox_ipc->irq_power.irq);
		devm_free_irq(dev, mbox_ipc->irq_power.irq, drv_data);
	}

	if (mbox_ipc->irq_xfer.irq) {
		disable_irq_wake(mbox_ipc->irq_xfer.irq);
		devm_free_irq(dev, mbox_ipc->irq_xfer.irq, drv_data);
	}

	HFI_CORE_DBG_H("-\n");
	return;
}

int init_ipc(struct hfi_core_drv_data *drv_data, hfi_ipc_cb hfi_core_cb)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !hfi_core_cb) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}

	/* Currently only HFI_CORE_CLIENT_ID_0 is supported
	 * TODO: This client id has to come from DT. Also, for now
	 * adding this in the 'drv_data', but this should be part of the
	 * per-client data.. along with the irq's.. and can all of this be part of the ipc-specific
	 * data, so we can isolate ipc-specific from overall drv data.
	 * NOTE that we only have one APPS_NS0 for this client running in LA, and we would have
	 * to initialize for APPS_NS1 for TVM, therefore for more clients in same LA, we would need
	 * to extend on 'signals' only.. but is that a use case? (not for now.. we would need
	 * to revisit for future)
	 */
	for (int i = HFI_CORE_CLIENT_ID_0; i <= HFI_CORE_CLIENT_ID_0; i++) {
		if (drv_data->client_data[i].ipc_info.type != HFI_IPC_TYPE_MBOX)
			continue;

		ret = mbox_init(drv_data, i);
		if (ret) {
			HFI_CORE_ERR("init ipc failed\n");
			goto exit;
		}

		ret = mbox_irq_init(drv_data, hfi_core_cb, i);
		if (ret) {
			HFI_CORE_ERR("init ipc irq failed\n");
			goto mbox_irq_fail;
		}
	}

	HFI_CORE_DBG_H("-\n");
	return 0;

mbox_irq_fail:
	deinit_ipc(drv_data);
exit:
	HFI_CORE_DBG_H("-\n");
	return ret;
}

int deinit_ipc(struct hfi_core_drv_data *drv_data)
{
	struct hfi_mbox_info *mbox_ipc;
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data) {
		HFI_CORE_ERR("invalid device\n");
		return -EINVAL;
	}

	for (int i = HFI_CORE_CLIENT_ID_0; i <= HFI_CORE_CLIENT_ID_0; i++) {
		if (drv_data->client_data[i].ipc_info.type != HFI_IPC_TYPE_MBOX)
			continue;

		/* irq deinit */
		mbox_irq_deinit(drv_data, i);

		/* mbox deinit */
		mbox_ipc = (struct hfi_mbox_info *)
			(drv_data->client_data[i].ipc_info.data);
		if (mbox_ipc)
			mbox_deinit(mbox_ipc);
		else
			HFI_CORE_ERR("mbox ipc data is null\n");
	}

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int trigger_ipc(u32 client_id, struct hfi_core_drv_data *drv_data,
	enum ipc_notification_type ipc_notify)
{
	struct hfi_mbox_info *mbox_ipc;
	void *msg = NULL;
	enum mbox_channel_type ipc_chan;
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data) {
		HFI_CORE_ERR("invalid device\n");
		return -EINVAL;
	}

	if (client_id >= HFI_CORE_CLIENT_ID_MAX ) {
		HFI_CORE_ERR("invalid client id: %u\n", client_id);
		return -EINVAL;
	}

	if (drv_data->client_data[client_id].ipc_info.type !=
		HFI_IPC_TYPE_MBOX)
		return 0;

	if (ipc_notify == HFI_IPC_EVENT_QUEUE_NOTIFY) {
		ipc_chan = MBOX_CHAN_XFER;
	} else {
		ipc_chan = MBOX_CHAN_POWER;
	}
	mbox_ipc = (struct hfi_mbox_info *)(
		drv_data->client_data[client_id].ipc_info.data);
	ret = mbox_trigger_signal(mbox_ipc, ipc_chan, msg);
	if (ret) {
		HFI_CORE_ERR("mbox signalling failed client id: %u\n",
			client_id);
		return ret;
	}

	HFI_CORE_DBG_H("-\n");
	return ret;
}

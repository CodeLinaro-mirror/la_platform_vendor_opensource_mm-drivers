// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#include <linux/module.h>
#include "hfi_interface.h"
#include "hfi_core.h"
#include "hfi_if_abstraction.h"
#include "hfi_swi.h"
#include "hfi_smmu.h"
#include "hfi_ipc.h"
#include "hfi_queue_controller.h"
#include "hfi_core_debug.h"

struct hfi_core_drv_data *drv_data;

static int hfi_ipc_core_cb(void *data, enum hfi_core_client_id client_idx,
	enum ipc_notification_type ipc_notify)
{
	struct client_data *client_data;
	u32 flags = 0;
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || client_idx >= HFI_CORE_CLIENT_ID_MAX)
	{
		ret = -EINVAL;
		HFI_CORE_ERR("invalid client id provided: client_id : %d\n",
			client_idx);
		goto error;
	}
	client_data = &drv_data->client_data[client_idx];

	switch (ipc_notify) {
	case (HFI_IPC_EVENT_QUEUE_NOTIFY):
		if (client_data && client_data->cb_fn) {
			client_data->cb_fn(client_data->session,
				client_data->cb_data, flags);
		}

		break;
	case (HFI_IPC_EVENT_POWER_NOTIFY):
		/* notify IFAL about the power notification for this client */
		power_notification(client_idx, drv_data);
		break;
	default:
		HFI_CORE_ERR("invalid IPC notification: %d\n", ipc_notify);
		break;
	}

	HFI_CORE_DBG_H("-\n");
error:
	return ret;
}

int hfi_core_init(struct hfi_core_drv_data *init_drv_data)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!init_drv_data) {
		HFI_CORE_ERR("invalid params\n");
		ret = -EINVAL;
		goto exit;
	}
	drv_data = init_drv_data;

	ret = init_smmu(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to init smmu ret :%d\n", ret);
		goto exit;
	}

	ret = init_swi(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to init swi ret :%d\n", ret);
		goto exit;
	}

	ret = init_resources(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to init queues ret :%d\n", ret);
		goto exit;
	}

	ret = init_ipc(drv_data, hfi_ipc_core_cb);
	if (ret) {
		HFI_CORE_ERR("failed to init ipc ret :%d\n", ret);
		goto exit;
	}

	ret = hfi_core_dbg_debugfs_register(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to register debugfs ret :%d\n", ret);
		goto exit;
	}

	HFI_CORE_DBG_H("-\n");
exit:
	return ret;
}

int hfi_core_deinit(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data) {
		HFI_CORE_ERR("invalid params\n");
		ret = -EINVAL;
		goto exit;
	}

	hfi_core_dbg_debugfs_unregister(drv_data);

	ret = deinit_ipc(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to deinit ipc ret :%d\n", ret);
		goto exit;
	}

	ret = deinit_resources(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to deinit resources ret :%d\n", ret);
		goto exit;
	}

	ret = deinit_swi(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to deinit swi ret :%d\n", ret);
		goto exit;
	}

	ret = deinit_smmu(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to deinit smmu ret :%d\n", ret);
		goto exit;
	}

	HFI_CORE_DBG_H("-\n");
exit:
	return ret;
}

struct hfi_core_session *hfi_core_open_session(
	struct hfi_core_open_params *params)
{
	struct hfi_core_session *hfi_handle;
	u32 client_id;
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!params || !params->ops ||
		params->client_id >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid hfi open params or client id\n");
		return NULL;
	}
	client_id = params->client_id;

	/* if same client requested again, return previous handle */
	if (drv_data->client_data[client_id].session) {
		HFI_CORE_ERR("cliend: %d already present\n", client_id);
		return drv_data->client_data[client_id].session;
	}

	hfi_handle = kzalloc(sizeof(*hfi_handle), GFP_KERNEL);
	if (!hfi_handle) {
		HFI_CORE_ERR("failed to allocate memory for hfi_handle\n");
		return NULL;
	}

	ret = set_power_vote(drv_data, true);
	if (ret) {
		HFI_CORE_ERR("failed to vote power, ret: %d\n", ret);
		goto error;
	}

	hfi_handle->client_id = client_id;
	drv_data->client_data[client_id].session = hfi_handle;
	drv_data->client_data[client_id].cb_fn = params->ops->hfi_cb_fn;
	drv_data->client_data[client_id].cb_data = params->ops->cb_data;
	trigger_ipc(client_id, drv_data, HFI_IPC_EVENT_QUEUE_NOTIFY);

	HFI_CORE_DBG_H("-\n");
	return hfi_handle;

error:
	kfree(hfi_handle);
	return NULL;
}

int hfi_core_close_session(struct hfi_core_session *hfi_handle)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!hfi_handle) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}

	/* remove client data for drv data */
	drv_data->client_data[hfi_handle->client_id].cb_fn = NULL;
	drv_data->client_data[hfi_handle->client_id].cb_data = NULL;
	drv_data->client_data[hfi_handle->client_id].session = NULL;

	kfree(hfi_handle);

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int hfi_core_cmds_tx_buf_get(struct hfi_core_session *hfi_session,
	struct hfi_core_cmds_buf_desc *buff_desc)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!hfi_session || !buff_desc) {
		HFI_CORE_ERR("invalid hfi session or buffer desc\n");
		return -EINVAL;
	}

	ret = get_tx_buffer(drv_data, hfi_session->client_id, buff_desc);
	if (ret) {
		HFI_CORE_ERR("invalid hfi buffer descriptor\n");
		return ret;
	}

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int hfi_core_cmds_rx_buf_get(struct hfi_core_session *hfi_session,
	struct hfi_core_cmds_buf_desc *buff_desc)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!hfi_session || !buff_desc) {
		HFI_CORE_ERR("invalid hfi session or buffer desc\n");
		return -EINVAL;
	}

	ret = get_rx_buffer(drv_data, hfi_session->client_id, buff_desc);
	if (ret) {
		HFI_CORE_ERR("invalid hfi buffer descriptor\n");
		return ret;
	}

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int hfi_core_cmds_tx_buf_send(struct hfi_core_session *hfi_session,
	struct hfi_core_cmds_buf_desc **buff_desc, u32 num_buff_desc,
	u32 flags)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!hfi_session || !buff_desc) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}

	/* update tx-buff signal */
	ret = set_tx_buffer(drv_data, hfi_session->client_id, buff_desc,
		num_buff_desc);
	if (ret) {
		HFI_CORE_ERR("failed to set tx buff for signal\n");
		return ret;
	}

	/* trigger the ipc now after setting the tx-buff */
	if (flags & HFI_CORE_SET_FLAGS_TRIGGER_IPC)
		trigger_ipc(hfi_session->client_id, drv_data,
			HFI_IPC_EVENT_QUEUE_NOTIFY);

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int hfi_core_release_rx_buffer(struct hfi_core_session *hfi_session,
	struct hfi_core_cmds_buf_desc **buff_desc, u32 num_buff_desc)
{
	if (!hfi_session || !buff_desc) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}

	/* release Rx Buff	*/
	return put_rx_buffer(drv_data, hfi_session->client_id, buff_desc,
		num_buff_desc);
}

int hfi_core_release_tx_buffer(struct hfi_core_session *hfi_session,
	struct hfi_core_cmds_buf_desc **buff_desc, u32 num_buff_desc)
{
	if (!hfi_session || !buff_desc) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}

	/* release Tx Buff without signal */
	return put_tx_buffer(drv_data, hfi_session->client_id, buff_desc,
		num_buff_desc);
}

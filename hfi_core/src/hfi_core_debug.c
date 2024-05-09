// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#include <linux/debugfs.h>
#include "hfi_core_debug.h"
#include "hfi_core.h"
#include "hfi_interface.h"
#include "hfi_if_abstraction.h"
#include "hfi_dbg_packet.h"
#include <linux/kthread.h>

u32 msm_hfi_core_debug_level = HFI_CORE_INIT | HFI_CORE_HIGH  |
	HFI_CORE_PRINTK;

/**
 * struct dbg_client_data - Structure holding the data of the debug clients.
 *
 * @list: client node.
 * @client_id: client id.
 * @client_handle: handle for the client, this is returned by the HFI core
 *                 driver after a successful registration of the client.
 * @open_params: client hfi core open parameters
 */
struct dbg_client_data {
	struct list_head list;
	void *client_handle;
	struct hfi_core_open_params open_params;
	struct hfi_core_cmds_buf_desc *buf_desc;
};

/**
 * struct hfi_core_dbg_data - structure holding debugfs data.
 *
 * @root: debugfs root
 * @clients_list: list of debug clients registered
 * @clients_list_lock: lock to synchronize access to the clients list
 * @signaled_clients_mask: Clients mask for which callback is received
 * @wait_queue: wait queue for clients that are waiting for callback
 *                  signal
 * @listener_thread: Thread to process callback signals received for
 *                   clients in "wait queue".
 */
struct hfi_core_dbg_data {
	struct dentry *root;
	struct list_head clients_list;
	struct mutex clients_list_lock;
	atomic_t signaled_clients_mask;
	wait_queue_head_t wait_queue;
	struct task_struct *listener_thread;
};

#if IS_ENABLED(CONFIG_DEBUG_FS)

static int _get_debugfs_input_client(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos,
	struct hfi_core_drv_data **drv_data)
{
	char buf[10];
	int client_id;

	HFI_CORE_DBG_H("+\n");

	if (!file || !file->private_data) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}

	if (!user_buf) {
		HFI_CORE_ERR("user buffer is null\n");
		return -EINVAL;
	}
	*drv_data = file->private_data;

	if (count >= sizeof(buf))
		return -EFAULT;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	buf[count] = 0; /* end of string */

	if (kstrtouint(buf, 0, &client_id))
		return -EFAULT;

	if (client_id < HFI_CORE_CLIENT_ID_0 ||
		client_id >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid client_id:%d min:%d max:%d\n", client_id,
			HFI_CORE_CLIENT_ID_0, HFI_CORE_CLIENT_ID_MAX);
		return -EINVAL;
	}

	HFI_CORE_DBG_H("-\n");
	return client_id;
}

struct dbg_client_data *_get_client_node(struct hfi_core_drv_data *drv_data,
	u32 client_id)
{
	struct dbg_client_data *node = NULL;
	bool found = false;
        struct hfi_core_dbg_data *debugfs_data;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->debug_info.data) {
		HFI_CORE_ERR("invalid params\n");
		return NULL;
	}
        debugfs_data = (struct hfi_core_dbg_data *)drv_data->debug_info.data;

	mutex_lock(&debugfs_data->clients_list_lock);
	list_for_each_entry(node, &debugfs_data->clients_list, list) {
		if (node && node->open_params.client_id == client_id) {
			found = true;
			break;
		}
	}
	mutex_unlock(&debugfs_data->clients_list_lock);

	HFI_CORE_DBG_H("-\n");
	return found ? node : NULL;
}

static int print_hfi_header_info(struct hfi_header_info *header_info)
{
	char *cmd_buff_type_str;

	if (!header_info) {
		HFI_CORE_ERR("invalid header to print\n");
		return -EINVAL;
	}

	switch (header_info->cmd_buff_type) {
	case HFI_CMD_BUFF_SYSTEM:
		cmd_buff_type_str = "SYSTEM";
		break;
	case HFI_CMD_BUFF_DEVICE:
		cmd_buff_type_str = "DEVICE";
		break;
	case HFI_CMD_BUFF_DISPLAY:
		cmd_buff_type_str = "DISPLAY";
		break;
	case HFI_CMD_BUFF_DEBUG:
		cmd_buff_type_str = "DEBUG";
		break;
	case HFI_CMD_BUFF_VIRTUALIZATION:
		cmd_buff_type_str = "VIRTZ";
		break;
	default:
		cmd_buff_type_str = "Unknown";
		break;
	}

	HFI_CORE_DBG_H(
		"num_packets: %u cmd_buff_type: %s object_id: %u header_id: %u",
		header_info->num_packets, cmd_buff_type_str,
		header_info->object_id, header_info->header_id);

	return 0;
}

static int print_u32_payload(void *payload_ptr, u32 payload_size)
{
	u32 *payload_u32_ptr = (u32 *)payload_ptr;
	u32 array_size = 0;

	if (!payload_u32_ptr || !payload_size || (payload_size < 4)) {
		HFI_CORE_ERR("invalid payload to print, payload_sz: %u\n",
			payload_size);
		return -EINVAL;
	}

        array_size = payload_size / sizeof(u32);
	for (int i = 0; i < array_size; i++) {
		HFI_CORE_DBG_H("payload dword[%d]: %u\n", i, *payload_u32_ptr);
		payload_u32_ptr++;
	}
	return 0;
}

static int print_hfi_packet_info(struct hfi_packet_info *packet_info)
{
	char *payload_type_str;

	if (!packet_info) {
		HFI_CORE_ERR("invalid packet to print\n");
		return -EINVAL;
	}

	switch (packet_info->payload_type) {
	case HFI_PAYLOAD_NONE:
		payload_type_str = "NONE";
		break;
	case HFI_PAYLOAD_U32:
		payload_type_str = "U32";
		break;
	case HFI_PAYLOAD_U32_ARRAY:
		payload_type_str = "U32_ARRAY";
		break;
	case HFI_PAYLOAD_U64:
		payload_type_str = "U64";
		break;
	case HFI_PAYLOAD_U64_ARRAY:
		payload_type_str = "U64_ARRAY";
		break;
	case HFI_PAYLOAD_BLOB:
		payload_type_str = "BLOB";
		break;
	default:
		payload_type_str = "Unknown";
		break;
	}

	HFI_CORE_DBG_H(
		"cmd: 0x%x id: 0x%x flags: 0x%x packet_id: 0x%x payload_type: %s payload_size: %u payload_ptr: 0x%llx",
		packet_info->cmd, packet_info->id,
		packet_info->flags, packet_info->packet_id,
		payload_type_str, packet_info->payload_size,
		(u64)packet_info->payload_ptr);

	/* print payload */
	switch (packet_info->payload_type) {
	case HFI_PAYLOAD_U32:
	case HFI_PAYLOAD_U32_ARRAY:
		print_u32_payload(packet_info->payload_ptr,
			packet_info->payload_size);
		break;
	default:
		HFI_CORE_ERR("not support payload type to print\n");
		return -EINVAL;
	}

	return 0;
}

static int process_rx_buffer(struct hfi_core_drv_data *drv_data,
	struct hfi_core_cmds_buf_desc *buff_desc)
{
	int ret = 0;
	struct hfi_cmd_buff_hdl cmd_buf_hdl;
	struct hfi_header_info header_info;
	struct hfi_packet_info packet_info;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !buff_desc) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}

	cmd_buf_hdl.cmd_buffer = buff_desc->pbuf_vaddr;
	cmd_buf_hdl.size = (u32)buff_desc->size;
	ret = hfi_unpacker_get_header_info(&cmd_buf_hdl, &header_info);
	if (ret) {
		HFI_CORE_ERR(
			"failed to get header info for buff desc: 0x%llx\n",
			(u64)buff_desc->pbuf_vaddr);
		return ret;
	}

	if (!header_info.num_packets) {
		HFI_CORE_DBG_H("buff desc 0x%llx has no packets\n",
			(u64)buff_desc->pbuf_vaddr);
		return 0;
	}
	if (header_info.cmd_buff_type != HFI_CMD_BUFF_DEBUG) {
		HFI_CORE_ERR(
			"buff desc 0x%llx has invalid cmd buff type : %d\n",
			(u64)buff_desc->pbuf_vaddr,
			header_info.cmd_buff_type);
		return -EINVAL;
	}
	print_hfi_header_info(&header_info);

	for (int i = 1; i <= header_info.num_packets; i ++) {
		ret = hfi_unpacker_get_packet_info(&cmd_buf_hdl, i, &packet_info);
		if (ret) {
			HFI_CORE_ERR(
				"failed to get packet info for buff desc: 0x%llx packet: %d\n",
				(u64)buff_desc->pbuf_vaddr, i);
			return ret;
		}
		print_hfi_packet_info(&packet_info);
	}

	HFI_CORE_DBG_H("-\n");
	return ret;
}

static int get_rx_buffers(struct hfi_core_drv_data *drv_data, int client_id)
{
	int ret = 0;
	struct hfi_core_cmds_buf_desc buff_desc;
	struct dbg_client_data *client;
	struct hfi_core_cmds_buf_desc *buff_desc_ptr_array[1];

	HFI_CORE_DBG_H("+\n");

	client = _get_client_node(drv_data, client_id);
	if (!client || IS_ERR_OR_NULL(client->client_handle)) {
		HFI_CORE_ERR("client with id: %d is not found\n", client_id);
		return -EINVAL;
	}

	while (1) {
		memset(&buff_desc, 0, sizeof(buff_desc));
		ret = hfi_core_cmds_rx_buf_get(client->client_handle,
			&buff_desc);
		if (ret == -ENOBUFS) {
			HFI_CORE_DBG_H(
				"no more rx buffers to process for client: %d\n",
				client_id);
			break;
		}

		if (ret || !buff_desc.pbuf_vaddr || !buff_desc.size) {
			HFI_CORE_ERR(
				"failed to get rx buffer for client: %d ret: %d\n",
				client_id, ret);
			return -EINVAL;
		}
		process_rx_buffer(drv_data, &buff_desc);

                buff_desc_ptr_array[0] = &buff_desc;
		ret = hfi_core_release_rx_buffer(client->client_handle,
                        buff_desc_ptr_array, 1);
		if (ret) {
			HFI_CORE_DBG_H("failed to release rx buffer for client: %d\n",
				client_id);
			return ret;
		}
	}

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int hfi_core_dgb_client_cb(struct hfi_core_session *hfi_session,
			const void *cb_data, u32 flags)
{
	HFI_CORE_DBG_H("+\n");

	if (!hfi_session || !cb_data) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}

	struct hfi_core_dbg_data *dbg_data =
		(struct hfi_core_dbg_data *)cb_data;
	atomic_or(BIT(hfi_session->client_id),
		&dbg_data->signaled_clients_mask);
	wake_up_all(&dbg_data->wait_queue);

	HFI_CORE_DBG_H("-\n");
	return 0;
}

static int hfi_core_dbg_listener(void *data)
{
	u32 mask;
	struct hfi_core_dbg_data *dbg_data;
	struct hfi_core_drv_data *drv_data = (struct hfi_core_drv_data *)data;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->debug_info.data) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}
	dbg_data = (struct hfi_core_dbg_data *)drv_data->debug_info.data;

	while (1) {
		wait_event(dbg_data->wait_queue,
			atomic_read(&dbg_data->signaled_clients_mask) != 0);
		mask = atomic_xchg(&dbg_data->signaled_clients_mask, 0);
		HFI_CORE_DBG_H("mask: %u\n", mask);
		if (mask) {
			for (int client_id = HFI_CORE_CLIENT_ID_0;
				client_id < HFI_CORE_CLIENT_ID_MAX; client_id++) {
				if (BIT(client_id) & mask)
					get_rx_buffers(drv_data, client_id);
			}
		}
	}

	HFI_CORE_DBG_H("-\n");

	return 0;
}

static ssize_t hfi_core_dbg_reg_client(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	int client_id;
	struct hfi_core_drv_data *drv_data;
	struct dbg_client_data *client;
	struct hfi_core_cb_ops cb_ops;
        struct hfi_core_dbg_data *debugfs_data;

	HFI_CORE_DBG_H("+\n");

	client_id = _get_debugfs_input_client(file, user_buf, count, ppos,
		&drv_data);
	if (client_id < 0 || !drv_data || !drv_data->debug_info.data){
		HFI_CORE_ERR(
			"failed to get client id: %d or drv data / dgb data\n",
			client_id);
		return -EINVAL;
	}

	/* we cannot create same debug client twice */
	if (_get_client_node(drv_data, client_id)) {
		HFI_CORE_ERR("client:%d already registered as debug client\n",
			client_id);
		return -EINVAL;
	}

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	HFI_CORE_DBG_H("register client %d\n", client_id);

        debugfs_data = (struct hfi_core_dbg_data *)drv_data->debug_info.data;

	client->open_params.client_id = client_id;
	cb_ops.hfi_cb_fn = hfi_core_dgb_client_cb;
	cb_ops.cb_data = debugfs_data;
	client->open_params.ops = &cb_ops;

	client->client_handle = hfi_core_open_session(&client->open_params);
	if (IS_ERR_OR_NULL(client->client_handle)) {
		HFI_CORE_ERR("error registering as debug client:%d\n",
			client_id);
		client->client_handle = NULL;
		return -EFAULT;
	}

	mutex_lock(&debugfs_data->clients_list_lock);
	list_add(&client->list, &debugfs_data->clients_list);
	mutex_unlock(&debugfs_data->clients_list_lock);

	HFI_CORE_DBG_H("-\n");
	return count;
}

static ssize_t hfi_core_dbg_get_buf(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	int client_id, ret = 0;
	struct hfi_core_drv_data *drv_data;
	struct dbg_client_data *client;
	struct hfi_core_cmds_buf_desc *buff_desc;

	HFI_CORE_DBG_H("+\n");

	client_id = _get_debugfs_input_client(file, user_buf, count, ppos,
		&drv_data);
	if (client_id < 0 || !drv_data || !drv_data->debug_info.data){
		HFI_CORE_ERR(
			"failed to get client id: %d or drv data / dgb data\n",
			client_id);
		return -EINVAL;
	}

	client = _get_client_node(drv_data, client_id);
	if (!client || IS_ERR_OR_NULL(client->client_handle)) {
		HFI_CORE_ERR("client with id: %d is not found\n", client_id);
		return -EINVAL;
	}

	/* Allocate buf desc memory */
	buff_desc = kzalloc(sizeof(struct hfi_core_cmds_buf_desc), GFP_KERNEL);
	if (!buff_desc) {
		HFI_CORE_ERR(
			"failed to allocate buffer memory for client: %d\n",
			client_id);
		return -EINVAL;
	}

	buff_desc->prio_info = HFI_CORE_PRIO_1;
	ret = hfi_core_cmds_tx_buf_get(client->client_handle, buff_desc);
	if (ret || !buff_desc->pbuf_vaddr || !buff_desc->size) {
		HFI_CORE_ERR(
			"failed to get tx buffer for client: %d ret: %d\n",
			client_id, ret);
		return -EINVAL;
	}
	client->buf_desc = buff_desc;

	HFI_CORE_DBG_H(
		"got tx buffer for client: %d with pbuf_vaddr: 0x%pK size: %lu\n",
		client_id, client->buf_desc->pbuf_vaddr,
		client->buf_desc->size);

	HFI_CORE_DBG_H("-\n");
	return count;
}

static ssize_t hfi_core_dbg_send_buf(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	int client_id;
	struct hfi_core_drv_data *drv_data;
	struct dbg_client_data *client;
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	client_id = _get_debugfs_input_client(file, user_buf, count, ppos,
		&drv_data);
	if (client_id < 0 || !drv_data || !drv_data->debug_info.data){
		HFI_CORE_ERR(
			"failed to get client id: %d or drv data / dgb data\n",
			client_id);
		return -EINVAL;
	}

	client = _get_client_node(drv_data, client_id);
	if (!client || IS_ERR_OR_NULL(client->client_handle)) {
		HFI_CORE_ERR("client with id: %d is not found\n", client_id);
		return -EINVAL;
	}

	if (client && client->client_handle) {
		HFI_CORE_DBG_H("client with id: %d\n", client_id);
	} else {
		HFI_CORE_ERR("client is null\n");
		return -EINVAL;
	}

	if (client->buf_desc) {
		HFI_CORE_DBG_H(
			"sending tx buffer with pbuf_vaddr: 0x%pK size: %lu\n",
			client->buf_desc->pbuf_vaddr,
			client->buf_desc->size);
	} else {
		HFI_CORE_ERR("client buffer desc is null\n");
		return -EINVAL;
	}
	// supports to send only one buf desc at a time
	ret = hfi_core_cmds_tx_buf_send(client->client_handle,
		&client->buf_desc, 1, HFI_CORE_SET_FLAGS_TRIGGER_IPC);
	if (ret) {
		HFI_CORE_ERR(
			"failed to send tx buffer for client: %d pbuf_vaddr: 0x%pK\n",
			client_id, client->buf_desc->pbuf_vaddr);
		return ret;
	}

	HFI_CORE_ERR("sent tx buffer for client: %d with pbuf_vaddr: 0x%pK size: %lu\n",
		client_id, client->buf_desc->pbuf_vaddr,
		client->buf_desc->size);

	HFI_CORE_DBG_H("-\n");
	return count;
}

static ssize_t hfi_core_dbg_put_tx_buf(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	int client_id;
	struct hfi_core_drv_data *drv_data;
	struct dbg_client_data *client;
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	client_id = _get_debugfs_input_client(file, user_buf, count, ppos,
		&drv_data);
	if (client_id < 0 || !drv_data || !drv_data->debug_info.data){
		HFI_CORE_ERR(
			"failed to get client id: %d or drv data / dgb data\n",
			client_id);
		return -EINVAL;
	}

	client = _get_client_node(drv_data, client_id);
	if (!client || IS_ERR_OR_NULL(client->client_handle)) {
		HFI_CORE_ERR("client with id: %d is not found\n", client_id);
		return -EINVAL;
	}

	if (client && client->client_handle) {
		HFI_CORE_DBG_H("client with id: %d\n", client_id);
	} else {
		HFI_CORE_ERR("client is null\n");
		return -EINVAL;
	}

	if (client->buf_desc) {
		HFI_CORE_DBG_H("put tx buffer with pbuf_vaddr: 0x%pK size: %lu\n",
			client->buf_desc->pbuf_vaddr, client->buf_desc->size);
	} else {
		HFI_CORE_ERR("client buffer desc is null\n");
		return count;
	}

	HFI_CORE_ERR(
		"put tx buffer for client: %d with pbuf_vaddr: 0x%pK size: %lu\n",
		client_id, client->buf_desc->pbuf_vaddr,
		client->buf_desc->size);
	// supports to send only one buf desc at a time
	ret = hfi_core_release_tx_buffer(client->client_handle,
		&client->buf_desc, 1);
	if (ret) {
		HFI_CORE_ERR(
			"failed to send tx buffer for client: %d pbuf_vaddr: 0x%pK\n",
			client_id, client->buf_desc->pbuf_vaddr);
		return ret;
	}
	kfree(client->buf_desc);

	HFI_CORE_DBG_H("-\n");
	return count;
}

static ssize_t hfi_core_dbg_unreg_client(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	int client_id;
	struct hfi_core_drv_data *drv_data;
	struct dbg_client_data *client;
        struct hfi_core_dbg_data *debugfs_data;
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	client_id = _get_debugfs_input_client(file, user_buf, count, ppos,
		&drv_data);
	if (client_id < 0 || !drv_data || !drv_data->debug_info.data){
		HFI_CORE_ERR(
			"failed to get client id: %d or drv data / dgb data\n",
			client_id);
		return -EINVAL;
	}

	client = _get_client_node(drv_data, client_id);
	if (!client || IS_ERR_OR_NULL(client->client_handle)) {
		HFI_CORE_ERR("client with id: %d is not found\n", client_id);
		return -EINVAL;
	}
        debugfs_data = (struct hfi_core_dbg_data *)drv_data->debug_info.data;

	if (client && client->client_handle) {
		HFI_CORE_DBG_H("client with id: %d\n", client_id);
	} else {
		HFI_CORE_ERR("client is null\n");
		return -EINVAL;
	}

	// supports to send only one buf desc at a time
	ret = hfi_core_close_session(client->client_handle);
	if (ret) {
		HFI_CORE_ERR("failed to close client: %d\n", client_id);
		return ret;
	}

	mutex_lock(&debugfs_data->clients_list_lock);
	list_del_init(&client->list);
	mutex_unlock(&debugfs_data->clients_list_lock);
	kfree(client);

	HFI_CORE_ERR("closed client: %d\n", client_id);

	HFI_CORE_DBG_H("-\n");
	return count;
}

static ssize_t hfi_core_dbg_print_res_tbl(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	int client_id;
	struct hfi_core_drv_data *drv_data;
	struct hfi_resource_data *res_data = NULL;
	struct hfi_core_resource_table_hdr *tbl_hdr;
	struct hfi_core_resource_hdr *res_hdr;
	struct hfi_channel_virtio_virtq *virtq_chan;
	struct hfi_virtio_virtq *virtq_res_hdr;

	HFI_CORE_DBG_H("+\n");

	client_id = _get_debugfs_input_client(file, user_buf, count, ppos,
		&drv_data);
	if (client_id < 0 || !drv_data || !drv_data->debug_info.data ||
		!drv_data->client_data[client_id].resource_info.res_data_mem){
		HFI_CORE_ERR(
			"failed to get client id: %d or drv data / dgb data / res data\n",
			client_id);
		return -EINVAL;
	}

	res_data = (struct hfi_resource_data *)
		drv_data->client_data[client_id].resource_info.res_data_mem;

	/* print res tbl */
	if (!res_data->tbl_res_hdr_mem.cpu_va) {
		HFI_CORE_ERR("table header cpu va is null\n");
		return -EINVAL;
	}
	if (res_data->tbl_res_hdr_mem.size_wr <
		sizeof(struct hfi_core_resource_table_hdr)) {
		HFI_CORE_ERR("resource table size wr: %zu is incorrect\n",
			res_data->tbl_res_hdr_mem.size_wr);
		return -EINVAL;
	}
	tbl_hdr = (struct hfi_core_resource_table_hdr *)
		res_data->tbl_res_hdr_mem.cpu_va;
	HFI_CORE_DBG_H(
		"TBL HDR: ver: %x size: %x hdr_off: %x hdr_size: %x hdr_num: %u\n",
		tbl_hdr->version, tbl_hdr->size, tbl_hdr->res_hdr_offset,
		tbl_hdr->res_hdr_size, tbl_hdr->res_hdrs_num);

	/* print res header */
	if (res_data->tbl_res_hdr_mem.size_wr <
		(sizeof(struct hfi_core_resource_table_hdr) +
		(tbl_hdr->res_hdrs_num * sizeof(struct hfi_core_resource_hdr)))) {
		HFI_CORE_ERR("resource hdr size wr: %zu is incorrect\n",
			res_data->tbl_res_hdr_mem.size_wr);
		return -EINVAL;
	}
	res_hdr = (struct hfi_core_resource_hdr *)
		((u8 *)tbl_hdr + sizeof(struct hfi_core_resource_table_hdr));
	for (int i = 0; i < tbl_hdr->res_hdrs_num ; i++ ) {
		HFI_CORE_DBG_H(
			"RES HDR[%d]: ver: %x type: %u status: %u addrh: %x addrl: %u size: %x\n",
			i, res_hdr->version, res_hdr->type, res_hdr->status,
			res_hdr->start_addr_high, res_hdr->start_addr_low,
			res_hdr->size);
		res_hdr++;
	}

	/* print virtq res hr channel */
	if (!res_data->vitq_res.q_hdr_mem.mem.cpu_va) {
		HFI_CORE_ERR("virtq res hdr cpu va is null\n");
		return -EINVAL;
	}
	if (res_data->vitq_res.q_hdr_mem.mem.size_wr <
		sizeof(struct hfi_channel_virtio_virtq)) {
		HFI_CORE_ERR("virtq res hdr chan size wr: %zu is incorrect\n",
			res_data->vitq_res.q_hdr_mem.mem.size_wr);
		return -EINVAL;
	}
	virtq_chan = (struct hfi_channel_virtio_virtq *)
		res_data->vitq_res.q_hdr_mem.mem.cpu_va;
	HFI_CORE_DBG_H(
		"VIRTQ RES HDR CHAN: dev id: %u flags: %x res num: %u\n",
		virtq_chan->dcp_device_id, virtq_chan->flags, virtq_chan->num);

	/* print virtq res hrs */
	if (res_data->vitq_res.q_hdr_mem.mem.size_wr <
		(sizeof(struct hfi_channel_virtio_virtq) +
		(virtq_chan->num * sizeof(struct hfi_virtio_virtq)))) {
		HFI_CORE_ERR("virtq res hdr size wr: %zu is incorrect\n",
			res_data->vitq_res.q_hdr_mem.mem.size_wr);
		return -EINVAL;
	}
	virtq_res_hdr = (struct hfi_virtio_virtq *)
		((u8 *)virtq_chan + sizeof(struct hfi_channel_virtio_virtq));
	for (int i = 0; i < virtq_chan->num ; i++) {
		HFI_CORE_DBG_H(
			"VIRTQ RES HDR[%d]: queue id: %u prio: %u type: %u queue size: 0x%x addrl: 0x%x addrh: 0x%x align: %u size: 0x%x\n",
			i, virtq_res_hdr->queue_id,
			virtq_res_hdr->queue_priority, virtq_res_hdr->type,
			virtq_res_hdr->queue_size, virtq_res_hdr->addr_lower,
			virtq_res_hdr->addr_higher, virtq_res_hdr->alignment,
			virtq_res_hdr->size);
		virtq_res_hdr++;
	}

	HFI_CORE_DBG_H("-\n");
	return count;
}

static ssize_t hfi_core_dbg_test_packet(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	int client_id;
	struct hfi_core_drv_data *drv_data;
	struct dbg_client_data *client;
	int ret = 0;
	struct hfi_cmd_buff_hdl pkt_buff_hdl;
	struct hfi_header_info header_info;
	struct hfi_packet_info packet_info;
	struct hfi_core_cmds_buf_desc *buff_desc;
	u32 pkt_payload = 9630;

	HFI_CORE_DBG_H("+\n");

	client_id = _get_debugfs_input_client(file, user_buf, count, ppos,
		&drv_data);
	if (client_id < 0 || !drv_data || !drv_data->debug_info.data){
		HFI_CORE_ERR(
			"failed to get client id: %d or drv data / dgb data\n",
			client_id);
		return -EINVAL;
	}

	client = _get_client_node(drv_data, client_id);
	if (!client || IS_ERR_OR_NULL(client->client_handle)) {
		HFI_CORE_ERR("client with id: %d is not found\n", client_id);
		return -EINVAL;
	}

	if (client && client->client_handle) {
		HFI_CORE_DBG_H("client with id: %d\n", client_id);
	} else {
		HFI_CORE_ERR("client is null\n");
		return -EINVAL;
	}

	/* get tx buffer */
	/* Allocate buf desc memory */
	buff_desc = kzalloc(sizeof(struct hfi_core_cmds_buf_desc), GFP_KERNEL);
	if (!buff_desc) {
		HFI_CORE_ERR(
			"failed to allocate buffer memory for client: %d\n",
			client_id);
		return -EINVAL;
	}

	buff_desc->prio_info = HFI_CORE_PRIO_1;
	ret = hfi_core_cmds_tx_buf_get(client->client_handle, buff_desc);
	if (ret || !buff_desc->pbuf_vaddr ||
		!buff_desc->size) {
		HFI_CORE_ERR("failed to get tx buffer for client: %d ret: %d\n",
			client_id, ret);
		return -EINVAL;
	}
	client->buf_desc = buff_desc;

	HFI_CORE_DBG_H(
		"got tx buffer for client: %d with pbuf_vaddr: 0x%llx size: %lu dva: 0x%llx\n",
		client_id, (u64)client->buf_desc->pbuf_vaddr,
		client->buf_desc->size, client->buf_desc->priv_dva);

	/* fill tx buffer */
	pkt_buff_hdl.cmd_buffer = client->buf_desc->pbuf_vaddr;
	pkt_buff_hdl.size = client->buf_desc->size;
	header_info.cmd_buff_type = HFI_CMD_BUFF_DEBUG;
	header_info.object_id = 0;
	header_info.header_id = 1;
	ret = hfi_create_header(&pkt_buff_hdl, &header_info);
	if (ret) {
		HFI_CORE_ERR("failed to create hfi header\n");
		return ret;
	}

	packet_info.cmd = HFI_COMMAND_DEBUG_LOOPBACK_U32;
	packet_info.id = 0;
	packet_info.flags = HFI_TX_FLAGS_INTR_REQUIRED |
		HFI_TX_FLAGS_RESPONSE_REQUIRED;
	packet_info.packet_id = 1;
	packet_info.payload_type = HFI_PAYLOAD_U32;
	packet_info.payload_size = sizeof(pkt_payload);
	packet_info.payload_ptr = &pkt_payload;
	ret = hfi_create_full_packet(&pkt_buff_hdl, &packet_info);
	if (ret) {
		HFI_CORE_ERR("failed to create hfi header\n");
		return ret;
	}

	/* send tx buffer */
	// supports to send only one buf desc at a time
	ret = hfi_core_cmds_tx_buf_send(client->client_handle,
		&client->buf_desc, 1, HFI_CORE_SET_FLAGS_TRIGGER_IPC);
	if (ret) {
		HFI_CORE_ERR("failed to send tx buffer for client: %d pbuf_vaddr: 0x%pK\n",
			client_id, client->buf_desc->pbuf_vaddr);
		return ret;
	}

	HFI_CORE_DBG_H("-\n");
	return count;
}

char *get_dump_event_str(u32 val)
{
	char *str;

	switch(val) {
	case 0xbeef:
		str = "init trace dumps. [max trace events][trace mem ptr]";
		break;
	default:
		str = NULL;
	}

	return str;
}

static inline int _dump_event(struct hfi_core_trace_event *event, char *buf,
	int len, int max_size, u32 index)
{
	char data[HFI_CORE_MAX_DATA_PER_EVENT_DUMP];
	u32 data_cnt;
	int i, tmp_len = 0, ret = 0;
	char *dump_info;

	memset(&data, 0, sizeof(data));
	if (event->data_cnt > HFI_CORE_EVENT_MAX_DATA) {
		HFI_CORE_ERR(
			"event[%d] has invalid data_cnt:%d greater than max_data_cnt: %d\n",
			index, event->data_cnt, HFI_CORE_EVENT_MAX_DATA);
		data_cnt = HFI_CORE_EVENT_MAX_DATA;
	} else {
		data_cnt = event->data_cnt;
	}

	for (i = 0; i < data_cnt; i++) {
		if (i == 0 && event->data[i]) {
			dump_info = get_dump_event_str(event->data[i]);
			if (dump_info) {
				tmp_len += scnprintf(data + tmp_len,
					HFI_CORE_MAX_DATA_PER_EVENT_DUMP - tmp_len,
					"%s-->", dump_info);
				continue;
			}
		}
		tmp_len += scnprintf(data + tmp_len,
			HFI_CORE_MAX_DATA_PER_EVENT_DUMP - tmp_len,
			"%lx ", (unsigned long)event->data[i]);
	}

	ret = scnprintf(buf + len, max_size - len, HFI_CORE_EVT_MSG, index, (u64)event,
		event->data_cnt, data);

	HFI_CORE_DBG_H(
		HFI_CORE_EVT_MSG, index, (u64)event, event->data_cnt, data);

	return ret;
}

static ssize_t hfi_core_dbg_dump_events_rd(struct file *file,
	char __user *user_buf, size_t user_buf_size, loff_t *ppos)
{
	struct hfi_core_drv_data *drv_data;
	u32 entry_size = sizeof(struct hfi_core_trace_event), max_size = SZ_4K;
	char *buf = NULL;
	int len = 0;
	static u64 start_time;
	static int index, start_index;
	static bool wraparound;
	struct hfi_core_trace_event *event;

	if (!file || !file->private_data) {
		HFI_CORE_ERR("unexpected data 0x%llx\n", (u64)file);
		return -EINVAL;
	}
	drv_data = file->private_data;
	if (!drv_data) {
		HFI_CORE_ERR("drv data is null\n");
		return -EINVAL;
	}

	if (!drv_data->fw_trace_mem) {
		HFI_CORE_ERR("fw trace events not supported\n");
		return -EINVAL;
	}

	if (wraparound && index >= start_index) {
		HFI_CORE_DBG_H("no more data index: %d total_events: %d\n", index,
			HFI_CORE_MAX_TRACE_EVENTS);
		start_time = 0;
		index = 0;
		wraparound = false;
		return 0;
	}

	if (user_buf_size < entry_size) {
		HFI_CORE_ERR("not enough buff size: %zu to dump entries: %d\n",
			user_buf_size, entry_size);
		return -EINVAL;
	}

	buf = kzalloc(max_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	event = (struct hfi_core_trace_event *)drv_data->fw_trace_mem->cpu_va;
	HFI_CORE_DBG_H("events:0x%pK start_index:%d", event, start_index);
	while ((!wraparound || index < start_index) &&
		len < (max_size - entry_size)) {
		len += _dump_event(&event[index], buf, len, max_size, index);
		//event++;
		index++;
		if (index >= HFI_CORE_MAX_TRACE_EVENTS) {
			index = 0;
			wraparound = true;
		}
	}
	HFI_CORE_DBG_H("-- dump_events: index:%d\n", index);

	if (len <= 0 || len > user_buf_size) {
		HFI_CORE_ERR("len: %d invalid buff size: %zu\n",
			len, user_buf_size);
		len = 0;
		goto exit;
	}

	if (copy_to_user(user_buf, buf, len)) {
		HFI_CORE_ERR("failed to copy to user!\n");
		len = -EFAULT;
		goto exit;
	}
	*ppos += len;
exit:
	kfree(buf);
	return len;
}

static const struct file_operations hfi_core_register_clients_fops = {
	.open = simple_open,
	.write = hfi_core_dbg_reg_client,
};

static const struct file_operations hfi_core_dbg_get_buf_fops = {
	.open = simple_open,
	.write = hfi_core_dbg_get_buf,
};

static const struct file_operations hfi_core_dbg_send_buf_fops = {
	.open = simple_open,
	.write = hfi_core_dbg_send_buf,
};

static const struct file_operations hfi_core_dbg_put_buf_fops = {
	.open = simple_open,
	.write = hfi_core_dbg_put_tx_buf,
};

static const struct file_operations hfi_core_unregister_clients_fops = {
	.open = simple_open,
	.write = hfi_core_dbg_unreg_client,
};

static const struct file_operations hfi_core_print_res_table_fops = {
	.open = simple_open,
	.write = hfi_core_dbg_print_res_tbl,
};

static const struct file_operations hfi_core_dbg_test_pkt_fops = {
	.open = simple_open,
	.write = hfi_core_dbg_test_packet,
};

static const struct file_operations hfi_core_dbg_dump_events_fops = {
	.open = simple_open,
	.read = hfi_core_dbg_dump_events_rd,
};

int hfi_core_dbg_debugfs_register(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;
	struct dentry *debugfs_root;
	struct task_struct *thread;
        struct hfi_core_dbg_data *debugfs_data;

	HFI_CORE_DBG_H("+\n");

        if (!drv_data) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
        }

	debugfs_data = kzalloc(sizeof(*debugfs_data), GFP_KERNEL);
	if (!debugfs_data)
		return -ENOMEM;

	debugfs_root = debugfs_create_dir("hfi_core", NULL);
	if (IS_ERR_OR_NULL(debugfs_root)) {
		HFI_CORE_ERR("debugfs_root create_dir fail, error %ld\n",
			PTR_ERR(debugfs_root));
		ret = PTR_ERR(debugfs_root);
		goto failed_create_dir;
	}

	drv_data->debug_info.data = (void *)debugfs_data;
	mutex_init(&debugfs_data->clients_list_lock);
	INIT_LIST_HEAD(&debugfs_data->clients_list);

	debugfs_create_file("hfi_core_register_client", 0600, debugfs_root,
		drv_data, &hfi_core_register_clients_fops);
	debugfs_create_file("hfi_core_get_buf", 0600, debugfs_root,
		drv_data, &hfi_core_dbg_get_buf_fops);
	debugfs_create_file("hfi_core_trigger_ipc", 0600, debugfs_root,
		drv_data, &hfi_core_dbg_send_buf_fops);
	debugfs_create_file("hfi_core_put_buf", 0600, debugfs_root,
		drv_data, &hfi_core_dbg_put_buf_fops);
	debugfs_create_file("hfi_core_unregister_client", 0600, debugfs_root,
		drv_data, &hfi_core_unregister_clients_fops);
	debugfs_create_file("hfi_core_print_res_tbl", 0600, debugfs_root,
		drv_data, &hfi_core_print_res_table_fops);
	debugfs_create_file("hfi_core_dbg_test_pkt_send", 0600, debugfs_root,
		drv_data, &hfi_core_dbg_test_pkt_fops);
	debugfs_create_file("hfi_core_dump_events", 0600, debugfs_root,
		drv_data, &hfi_core_dbg_dump_events_fops);
	debugfs_create_u32("hfi_core_debug_level", 0600, debugfs_root,
		&msm_hfi_core_debug_level);

	debugfs_data->root = debugfs_root;

	// NOTE: This wait-object has to be initialized before the thread runs
	init_waitqueue_head(&debugfs_data->wait_queue);
	thread = kthread_run(hfi_core_dbg_listener, (void *)drv_data,
		"hfi_core_dbg_client_listener");
	if (IS_ERR(thread)) {
		HFI_CORE_ERR("listener thread create failed\n");
		ret = PTR_ERR(thread);
		goto failed_thread;
	}
	debugfs_data->listener_thread = thread;

	HFI_CORE_DBG_H("-\n");
	return 0;

failed_thread:
	if (debugfs_root)
		debugfs_remove_recursive(debugfs_root);
failed_create_dir:
        if (debugfs_data)
                kfree(debugfs_data);
	drv_data->debug_info.data = NULL;
	return ret;
}

void hfi_core_dbg_debugfs_unregister(struct hfi_core_drv_data *drv_data)
{
        struct hfi_core_dbg_data *debugfs_data;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->debug_info.data) {
		HFI_CORE_ERR("invalid params\n");
		return;
	}
        debugfs_data = (struct hfi_core_dbg_data *)drv_data->debug_info.data;

	if (debugfs_data->listener_thread)
		kthread_stop(debugfs_data->listener_thread);

	if (debugfs_data->root)
		debugfs_remove_recursive(debugfs_data->root);
        kfree(debugfs_data);
	drv_data->debug_info.data = NULL;

	HFI_CORE_DBG_H("-\n");
	return;
}

#else

int hfi_core_dbg_debugfs_register(struct hfi_core_drv_data *drv_data)
{
	return 0;
}

void hfi_core_dbg_debugfs_unregister(struct hfi_core_drv_data *drv_data)
{
	return;
}

#endif /* CONFIG_DEBUG_FS */
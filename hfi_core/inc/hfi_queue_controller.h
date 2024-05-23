// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#ifndef __HFI_QUEUE_CONTROLLER_H__
#define __HFI_QUEUE_CONTROLLER_H__

/**
 * set_tx_buffer() - Set TX buffer descriptor.
 *
 * This calls sets the buffer within the transport layer, so the buffer
 * is received by the other end, receiving the buffers transferred by this client.
 * Once this API is called, buffer must not be dereferenced anymore by this client.
 *
 * Return: 0 on success or negative errno
 */
int set_tx_buffer(u32 client_id, struct hfi_core_cmds_buf_desc **buff_desc,
	u32 num_buff_desc);

/**
 * get_rx_buffer() - Get rx buffer descriptor .
 *
 * This calls get the buffer descriptor from the clients Rx queues.
 *
 * Return: 0 on success or negative errno
 */
int get_rx_buffer(u32 client_id, struct hfi_core_cmds_buf_desc *buff_desc);

/**
 * get_tx_buffer() - Get tx buffer descriptor .
 *
 * This call gets a Tx buffer that must be filled by the client
 * with the data to send to the other end.
 *
 * Return: 0 on success or negative errno.
 */
int get_tx_buffer(u32 client_id, struct hfi_core_cmds_buf_desc *buf_desc);

/**
 * put_rx_buffer() - Releases Rx buffer.
 *
 * This call releases the rx buffer based on the buffer descriptor provided.
 *
 * Return: 0 on success or negative errno
 */
int put_tx_buffer(u32 client_id, struct hfi_core_cmds_buf_desc **buff_desc,
	u32 num_buff_desc);

/**
 * put_tx_buffer() - Releases Tx buffer.
 *
 * This call releases the tx buffer based on the buffer descriptor provided.
 *
 * Return: 0 on success or negative errno
 */
int put_rx_buffer(u32 client_id, struct hfi_core_cmds_buf_desc **buff_desc,
	u32 num_buff_desc);

/**
 * init_queues() - Setup queues for firmware communication
 *
 * This call creates all queues required for the client aligning to
 * the resources requirements specified in clients compat table
 *
 * Return: 0 on success or negative errno
 */
int init_queues(struct hfi_core_drv_data *drv_data);

/**
 * deinit_queues() - Destory all queues to stop firmware communication.
 *
 * This call destroys all queues created by init_queues() API.
 *
 * Return: 0 on success or negative errno
 */
int deinit_queues(struct hfi_core_drv_data *drv_data);

#endif // __HFI_QUEUE_CONTROLLER_H__

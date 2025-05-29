// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/habmm.h>
#include <linux/kthread.h>

#include "hw_fence_drv_priv.h"
#include "hw_fence_drv_virtio.h"
#include "hw_fence_drv_utils.h"
#include "hw_fence_drv_debug.h"

#define HW_FENCE_HAB_MAJOR_MMID MM_DISP_5
#define HW_FENCE_HAB_REQUEST_POWER_MMID HW_FENCE_HAB_MAJOR_MMID
#define HW_FENCE_HAB_SSR_NOTIFY_MMID HAB_MMID_CREATE(HW_FENCE_HAB_MAJOR_MMID, 0x1)
#define HW_FENCE_HAB_SOCKET_OPEN_TIMEOUT_MS -1 /* block indefinitely */
#define HW_FENCE_HAB_REQUEST_TIMEOUT_MS 1000

int hw_fence_virtio_init(struct hw_fence_driver_data *drv_data)
{
	int ret, tmp_ret;

	if (IS_ERR_OR_NULL(drv_data) || !drv_data->drv_id) {
		HWFNC_ERR("invalid input drv_data:0x%pK id:%d\n", drv_data,
			drv_data ? drv_data->drv_id : -1);
		return -EINVAL;
	}

	mutex_init(&drv_data->virtio_lock);
	ret = habmm_socket_open(&drv_data->send_socket, HW_FENCE_HAB_REQUEST_POWER_MMID,
		HW_FENCE_HAB_SOCKET_OPEN_TIMEOUT_MS, 0);
	if (ret) {
		HWFNC_ERR("failed to open hab socket for sending messages to pvm mmid:%d ret:%d\n",
			HW_FENCE_HAB_REQUEST_POWER_MMID, ret);
		return ret;
	}

	ret = habmm_socket_open(&drv_data->recv_socket, HW_FENCE_HAB_SSR_NOTIFY_MMID,
		HW_FENCE_HAB_SOCKET_OPEN_TIMEOUT_MS, 0);
	if (ret) {
		HWFNC_ERR("failed to open hab socket for receiving msg from pvm mmid:%d ret:%d\n",
			HW_FENCE_HAB_SSR_NOTIFY_MMID, ret);
		tmp_ret = habmm_socket_close(drv_data->recv_socket);
		if (tmp_ret)
			HWFNC_ERR("failed to close handle:%d for failure during bootup ret:%d\n",
				drv_data->recv_socket, ret);
		return ret;
	}

	HWFNC_DBG_INIT("successfully opened hab send_socket:%d mmid:%d recv_socket:%d mmid:%d\n",
		drv_data->send_socket, HW_FENCE_HAB_REQUEST_POWER_MMID, drv_data->recv_socket,
		HW_FENCE_HAB_SSR_NOTIFY_MMID);

	return 0;
}

int hw_fence_virtio_uninit(struct hw_fence_driver_data *drv_data)
{
	int ret = 0;

	if (IS_ERR_OR_NULL(drv_data)) {
		HWFNC_ERR("invalid input drv_data:0x%pK\n", drv_data);
		return -EINVAL;
	}

	ret = habmm_socket_close(drv_data->send_socket);
	if (ret)
		HWFNC_ERR("failed to close handle:%d to send msg to pvm\n", drv_data->send_socket);

	ret = habmm_socket_close(drv_data->recv_socket);
	if (ret)
		HWFNC_ERR("failed to close handle:%d to recv msg from pvm\n",
			drv_data->recv_socket);

	return ret;
}

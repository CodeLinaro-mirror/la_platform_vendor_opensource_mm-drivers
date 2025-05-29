/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __HW_FENCE_DRV_VIRTIO_H
#define __HW_FENCE_DRV_VIRTIO_H

#include <linux/types.h>
#include <linux/err.h>

#if IS_ENABLED(CONFIG_MSM_HAB)

/**
 * hw_fence_virtio_init() - Initialize virtio communication
 *
 * @drv_data: hw fence driver data
 *
 * Returns zero if success, otherwise returns negative error code
 */
int hw_fence_virtio_init(struct hw_fence_driver_data *drv_data);

/**
 * hw_fence_virtio_uninit - Uninitialzie virtio communication
 *
 * @drv_data: hw fence driver data
 *
 * Returns zero if success, otherwise returns negative error code
 */
int hw_fence_virtio_uninit(struct hw_fence_driver_data *drv_data);

/**
 * hw_fence_virtio_request_power() - Submit request to host vm via virtio communication
 *
 * @drv_data: hw fence driver data
 * @client_id: client id (external) of requesting client
 * @state: true if enabling proxy power votes, false if disabling proxy power votes
 *
 * Returns zero if success, otherwise returns negative error code
 */
int hw_fence_virtio_request_power(struct hw_fence_driver_data *drv_data,
	enum hw_fence_client_id client_id, bool state);

#else
static inline int hw_fence_virtio_init(struct hw_fence_driver_data *drv_data)
{
	return -EINVAL;
}

static inline int hw_fence_virtio_uninit(struct hw_fence_driver_data *drv_data)
{
	return -EINVAL;
}

static inline int hw_fence_virtio_request_power(struct hw_fence_driver_data *drv_data,
	enum hw_fence_client_id client_id, bool state)
{
	return -EINVAL;
}
#endif /* CONFIG_MSM_HAB */

#endif /* __HW_FENCE_DRV_VIRTIO_H */

// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#ifndef __HFI_IF_ABSTRACTION_H__
#define __HFI_IF_ABSTRACTION_H__

/**
 * init_resources() - resources initialization.
 *
 * This call initializes the queues required from the compact data
 * for the device to communicate with DCP.
 *
 * Return: 0 on success or negative errno
 */
int init_resources(struct hfi_core_drv_data *drv_data);

/**
 * deinit_resources() - resources deinitialization.
 *
 * This call deinitializes the queues required from the compact data
 * for the device to communicate with DCP.
 *
 * Return: 0 on success or negative errno
 */
int deinit_resources(struct hfi_core_drv_data *drv_data);

/**
 * power_notification() - power notification callback
 *
 * This function will turn power on/off disp_cc clocks upon request
 * received.
 */
int power_notification(uint32_t client_id, struct hfi_core_drv_data *drv_data);

#endif // __HFI_IF_ABSTRACTION_H__

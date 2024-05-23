// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#ifndef __HFI_CORE_H__
#define __HFI_CORE_H__

#include "hfi_interface.h"

/* struct that holds client info like callback functions, data */
struct client_data {
	struct hfi_core_drv_data *drv_data;
	enum hfi_core_type core_type;
	struct hfi_core_session *session;
	hfi_core_cb cb_fn;
	void *cb_data;
};

 /* Internal struct that holds data required by the hfi core driver */
struct hfi_core_drv_data {
	/* device handle */
	void *dev;
	enum hfi_core_type core_type;
	struct client_data client_data[HFI_CORE_CLIENT_ID_MAX];
	u32 num_clients;

	/*smmu data */

	/* queue data */

	/* swi data */

	/* debug info */
};

/**
 * hfi_core_init() - HFI core initialization.
 *
 * This call initializes the communication channel required for the device
 * to communicate with DCP.
 *
 * Return: 0 on success or negative errno
 */
int hfi_core_init(struct hfi_core_drv_data *init_drv_data);

/**
 * hfi_core_deinit() - HFI core deinitialization.
 *
 * This call deinitializes the communication channel required for the device
 * to communicate with DCP.
 *
 * Return: 0 on success or negative errno
 */
int hfi_core_deinit(struct hfi_core_drv_data *drv_data);

#endif // __HFI_CORE_H__

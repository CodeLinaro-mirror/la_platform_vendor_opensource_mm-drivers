// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#ifndef __HFI_SWI_H__
#define __HFI_SWI_H__

/**
 * init_swi() - Global SWI register initialization.
 *
 * This call programs the initial global swi registers required
 * for the device to communicate with DCP.
 *
 * Return: 0 on success or negative errno
 */
int init_swi(struct hfi_core_drv_data *drv_data);

/**
 * deinit_swi() - Global SWI register deinitialization.
 *
 * This call clears the initial global swi registers programmed
 * for the device to communicate with DCP.
 *
 * Return: 0 on success or negative errno
 */
int deinit_swi(struct hfi_core_drv_data *drv_data);

#endif // __HFI_SWI_H__

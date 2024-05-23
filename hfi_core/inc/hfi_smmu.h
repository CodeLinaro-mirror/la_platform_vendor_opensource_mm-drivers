// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#ifndef __HFI_SMMU_H__
#define __HFI_SMMU_H__

/**
 * init_smmu() - SMMU initialization.
 *
 * This call initializes the DCP context bank required for the device
 * to communicate with DCP.
 *
 * Return: 0 on success or negative errno
 */
int init_smmu(struct hfi_core_drv_data *drv_data);

/**
 * deinit_smmu() - SMMU deinitialization.
 *
 * This call deinitializes the DCP context bank required for the device
 * to communicate with DCP.
 *
 * Return: 0 on success or negative errno
 */
int deinit_smmu(struct hfi_core_drv_data *drv_data);

#endif // __HFI_SMMU_H__

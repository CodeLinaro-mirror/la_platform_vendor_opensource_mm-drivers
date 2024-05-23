// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#include <linux/debugfs.h>
#include "hfi_core_debug.h"
#include "hfi_core.h"

u32 msm_hfi_core_debug_level = HFI_CORE_INIT | HFI_CORE_HIGH  | HFI_CORE_PRINTK;

#if IS_ENABLED(CONFIG_DEBUG_FS)

int hfi_core_dbg_debugfs_register(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;

        //Place holder
	return ret;
}

void hfi_core_dbg_debugfs_unregister(struct hfi_core_drv_data *drv_data)
{
        //Place holder
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
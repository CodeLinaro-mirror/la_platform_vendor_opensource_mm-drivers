// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#include <linux/iommu.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include "hfi_core_debug.h"
#include "hfi_interface.h"
#include "hfi_core.h"
#include "hfi_smmu.h"
#include "hfi_swi.h"

static int map_swi_registers(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;
	unsigned int reg_config[2];
	void __iomem *ptr;
	struct device *dev = (struct device *)drv_data->dev;

	HFI_CORE_DBG_H("+\n");

	ret = of_property_read_u32_array(dev->of_node, "qcom,swi-reg",
		reg_config, 2);
	if (ret) {
		HFI_CORE_ERR("failed to read swi reg, ret: %d\n", ret);
		return ret;
	}

	drv_data->swi_info.reg_base = reg_config[0];
	drv_data->swi_info.size = reg_config[1];

	ptr = devm_ioremap(dev, drv_data->swi_info.reg_base,
		drv_data->swi_info.size);
	if (!ptr) {
		HFI_CORE_ERR("failed to ioremap swi regs\n");
		return -ENOMEM;
	}
	drv_data->swi_info.io_mem = ptr;

	HFI_CORE_DBG_H("-\n");
	return ret;
}

static void unmap_swi_registers(struct hfi_core_drv_data *drv_data)
{
	struct device *dev = (struct device *)drv_data->dev;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data->swi_info.io_mem) {
		HFI_CORE_ERR("swi regs io mem addr not available\n");
		return;
	}

	devm_iounmap(dev, drv_data->swi_info.io_mem);

	HFI_CORE_DBG_H("-\n");
}

static int map_mdss_registers_for_dcp(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;
	unsigned int reg_config[2];
	unsigned long mapped_iova = 0;
	struct device *dev = (struct device *)drv_data->dev;

	HFI_CORE_DBG_H("+\n");

	ret = of_property_read_u32_array(dev->of_node, "qcom,mdss-reg",
		reg_config, 2);
	if (ret) {
		HFI_CORE_ERR("failed to read mdss reg, ret: %d\n", ret);
		return ret;
	}

	drv_data->mdss_info.reg_base = reg_config[0];
	drv_data->mdss_info.size = reg_config[1];

	ret = smmu_mmap_for_fw(drv_data, drv_data->mdss_info.reg_base, &mapped_iova,
		drv_data->mdss_info.size, HFI_CORE_MMAP_READ | HFI_CORE_MMAP_WRITE);
	if (ret) {
		HFI_CORE_ERR("failed to map mdss registers, ret: %d\n", ret);
		return ret;
	}
	drv_data->mdss_info.iova = mapped_iova;

	HFI_CORE_DBG_H("mapped memory: 0x%llx size: 0x%x to addr:0x%lx\n",
		drv_data->mdss_info.reg_base, drv_data->mdss_info.size,
		mapped_iova);

	HFI_CORE_DBG_H("-\n");
	return ret;
}

static void unmap_mdss_registers_for_dcp(struct hfi_core_drv_data *drv_data)
{
	HFI_CORE_DBG_H("+\n");

	smmu_unmmap_for_fw(drv_data, drv_data->mdss_info.iova,
		drv_data->mdss_info.size);
	drv_data->mdss_info.reg_base = 0x0;
	drv_data->mdss_info.size = 0;
	drv_data->mdss_info.iova = 0;

	HFI_CORE_ERR("unmap mdss registers\n");

	HFI_CORE_DBG_H("-\n");
}

int init_swi(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->dev) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	ret = map_mdss_registers_for_dcp(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to map mdss regs\n");
		goto exit;
	}

	ret = map_swi_registers(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to map swi regs\n");
		goto exit;
	}

exit:
	HFI_CORE_DBG_H("-\n");
	return ret;
}

int deinit_swi(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->dev) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	unmap_swi_registers(drv_data);
	unmap_mdss_registers_for_dcp(drv_data);

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int swi_setup_resources(u32 client_id, struct hfi_core_drv_data *drv_data)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	// Setup swi registers

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int swi_reg_power_off(u32 client_id, struct hfi_core_drv_data *drv_data)
{
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	// Set swi register POWER_OFF bit

	HFI_CORE_DBG_H("-\n");
	return ret;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/of_address.h>
#include <linux/version.h>
#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
#include <linux/firmware/qcom/qcom_scm.h>
#else
#include <linux/qcom_scm.h>
#endif
#include <linux/soc/qcom/mdt_loader.h>
#include <linux/soc/qcom/smem.h>
#include <linux/devcoredump.h>
#include <linux/firmware.h>

#include "hfi_core_firmware.h"
#include "hfi_core.h"
#include "hfi_core_debug.h"

static int hfi_core_firmware_load_regions(struct hfi_core_drv_data *drv_data,
	struct hfi_core_firmware_info *fw_info)
{
	const struct firmware *firmware = NULL;
	ssize_t fw_size = 0;
	void *virt = NULL;
	struct device *dev = (struct device *)drv_data->dev;
	int ret = 0;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !fw_info) {
		HFI_CORE_ERR("null driver data or firmware info\n");
		return -EINVAL;
	}
	dev = (struct device *)drv_data->dev;

	ret = request_firmware(&firmware, fw_info->firmware_name, dev);
	if (ret) {
		HFI_CORE_ERR("failed to request fw \"%s\", error %d\n",
			fw_info->firmware_name, ret);
		return ret;
	}

	fw_size = qcom_mdt_get_size(firmware);
	if (fw_size < 0 || fw_info->fw_mem_size < (size_t)fw_size) {
		ret = -EINVAL;
		HFI_CORE_ERR("out of bound fw image fw size: %ld, fw_mem_size: %lu",
			fw_size, fw_info->fw_mem_size);
		goto cleanup;
	}

	virt = memremap(fw_info->phys_fw_mem_addr,
		fw_info->fw_mem_size, MEMREMAP_WC);
	if (!virt) {
		HFI_CORE_ERR("failed to remap fw memory phys %llu[p]\n",
			fw_info->phys_fw_mem_addr);
		ret = -ENOMEM;
		goto cleanup;
	}

	/* prevent system suspend during fw_load */
	pm_stay_awake(dev->parent);

	ret = qcom_mdt_load(dev, firmware, fw_info->firmware_name,
		fw_info->pas_id, virt, fw_info->phys_fw_mem_addr,
		fw_info->fw_mem_size, NULL);

	pm_relax(dev->parent);
	if (ret) {
		HFI_CORE_ERR("error %d loading fw %s\n", ret, fw_info->firmware_name);
		goto cleanup;
	}

	ret = qcom_scm_pas_auth_and_reset(fw_info->pas_id);
	if (ret) {
		HFI_CORE_ERR("error %d authenticating fw \"%s\"\n", ret, fw_info->firmware_name);
		goto cleanup;
	}

	HFI_CORE_DBG_INFO("firmware \"%s\" loaded successfully\n", fw_info->firmware_name);

cleanup:
	if (virt)
		memunmap(virt);
	if (firmware)
		release_firmware(firmware);

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int hfi_core_firmware_load(struct hfi_core_drv_data *drv_data)
{
	int ret = 0, i, index;
	static const int fw_regions[] = {
		/* load firmware dtb first and then load firmware */
		HFI_CORE_FIRMWARE_DTB_IMAGE_INDEX,
		HFI_CORE_FIRMWARE_IMAGE_INDEX };

	HFI_CORE_DBG_H("+\n");

	if (!drv_data) {
		HFI_CORE_ERR("null driver data or firmware info\n");
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(fw_regions); i++) {
		index = fw_regions[i];
		if (!drv_data->firmware_info[index].fw_mem_size) {
			HFI_CORE_DBG_H("skip loading firmware for region %d\n", index);
			continue;
		}
		ret = hfi_core_firmware_load_regions(drv_data, &drv_data->firmware_info[index]);
		if (ret) {
			HFI_CORE_ERR("Failed to load firmware %d\n", index);
			return ret;
		}
	}

	HFI_CORE_DBG_H("-\n");
	return 0;
}

int hfi_core_firmware_unload(struct hfi_core_drv_data *drv_data)
{
	int ret = 0, i, index;
	static const int fw_regions[] = {
		/* unload firmware first and then unload firmware dtb */
		HFI_CORE_FIRMWARE_IMAGE_INDEX,
		HFI_CORE_FIRMWARE_DTB_IMAGE_INDEX};

	HFI_CORE_DBG_H("+\n");

	if (!drv_data) {
		HFI_CORE_ERR("null driver data\n");
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(fw_regions); i++) {
		index = fw_regions[i];
		if (!drv_data->firmware_info[index].fw_mem_size) {
			HFI_CORE_DBG_H("skip unloading firmware for region %d\n", index);
			continue;
		}
		ret = qcom_scm_pas_shutdown(drv_data->firmware_info[index].pas_id);
		if (ret) {
			HFI_CORE_ERR("Firmware unload failed ret=%d\n", ret);
			return ret;
		}
	}

	HFI_CORE_DBG_H("-\n");
	return 0;
}

int hfi_core_firmware_core_dump(struct hfi_core_drv_data *drv_data)
{
	phys_addr_t fw_mem_phys;
	void *fw_mem_va = NULL;
	size_t fw_mem_size;
	void *dump = NULL;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data) {
		HFI_CORE_ERR("null driver data\n");
		return -EINVAL;
	}

	for (int i = 0; i < HFI_CORE_MAX_FIRMWARE_REGIONS; i++) {
		fw_mem_phys = drv_data->firmware_info[i].phys_fw_mem_addr;
		fw_mem_size = drv_data->firmware_info[i].fw_mem_size;
		if (!fw_mem_size) {
			HFI_CORE_DBG_H("invalid fw size/addr, skip core dump for region %d\n", i);
			continue;
		}

		fw_mem_va = memremap(fw_mem_phys, fw_mem_size, MEMREMAP_WC);
		if (!fw_mem_va) {
			HFI_CORE_ERR("unable to remap firmware memory\n");
			return -ENOMEM;
		}

		dump = vmalloc(fw_mem_size);
		if (!dump) {
			memunmap(fw_mem_va);
			HFI_CORE_ERR("unable to allocate memory to dump fw mem region\n");
			return -ENOMEM;
		}

		/* copy firmware dump */
		memcpy(dump, fw_mem_va, fw_mem_size);
		memunmap(fw_mem_va);

		dev_coredumpv(drv_data->dev, dump, fw_mem_size, GFP_KERNEL);
	}

	HFI_CORE_DBG_H("-\n");
	return 0;
}

int hfi_core_firmware_init(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;
	struct device_node *mem_node = NULL;
	struct resource res = { 0 };
	struct device *dev;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}
	dev = (struct device *)drv_data->dev;

	if (!IS_ENABLED(CONFIG_QCOM_MDT_LOADER) || !qcom_scm_is_available()) {
		HFI_CORE_ERR("mdt loader enable status: %d or qcom scm is unavailable\n",
			IS_ENABLED(CONFIG_QCOM_MDT_LOADER));
		return -EPROBE_DEFER;
	}

	for (int i = 0; i < HFI_CORE_MAX_FIRMWARE_REGIONS; i++) {
		drv_data->firmware_info[i].fw_mem_size = 0;
		mem_node = of_parse_phandle(dev->of_node, "fw-memory-region", i);
		if (!mem_node) {
			HFI_CORE_DBG_H("not found %d \"fw-memory-region\"\n", i);
			mem_node = of_parse_phandle(dev->of_node, "memory-region", i);
			if (!mem_node) {
				HFI_CORE_DBG_INFO(
					"failed to read %d \"fw-memory-region\" and \"memory-region\"\n",
					i);
				if (i == HFI_CORE_FIRMWARE_DTB_IMAGE_INDEX) {
					/*
					 * firmware image region is optional,
					 * so continue with next region
					 */
					continue;
				}
				return -EINVAL;
			}
		}

		ret = of_address_to_resource(mem_node, 0, &res);
		if (ret) {
			HFI_CORE_ERR("failed to read \"memory-region\", error %d\n", ret);
			return ret;
		}

		drv_data->firmware_info[i].phys_fw_mem_addr = res.start;
		drv_data->firmware_info[i].fw_mem_size = (size_t)(res.end - res.start + 1);

		ret = of_property_read_u32_index(dev->of_node, "qcom,pas-id", i,
			&drv_data->firmware_info[i].pas_id);
		if (ret) {
			HFI_CORE_ERR("failed to read qcom,pas-id %d\n", ret);
			return ret;
		}

		ret = of_property_read_string_index(dev->of_node, "qcom,fw_image_name", i,
			&drv_data->firmware_info[i].firmware_name);
		if (ret) {
			HFI_CORE_ERR("failed to read qcom,fw_image_name %d\n", ret);
			return ret;
		}

		HFI_CORE_DBG_INFO("fw_mem_addr: 0x%llx fw_mem_size: 0x%zx pas_id: %d fw_name: %s\n",
			drv_data->firmware_info[i].phys_fw_mem_addr,
			drv_data->firmware_info[i].fw_mem_size, drv_data->firmware_info[i].pas_id,
			drv_data->firmware_info[i].firmware_name);
	}

	HFI_CORE_DBG_H("-\n");
	return 0;
}

int hfi_core_firmware_deinit(struct hfi_core_drv_data *drv_data)
{
	HFI_CORE_DBG_H("+\n");

	for (int i = 0; i < HFI_CORE_MAX_FIRMWARE_REGIONS; i++) {
		drv_data->firmware_info[i].phys_fw_mem_addr = 0x0;
		drv_data->firmware_info[i].fw_mem_size = 0;
		drv_data->firmware_info[i].pas_id = 0;
		drv_data->firmware_info[i].firmware_name = NULL;
	}

	HFI_CORE_DBG_H("-\n");
	return 0;
}

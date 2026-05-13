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

static bool hfi_mdt_phdr_valid(const struct elf32_phdr *phdr)
{
	if (phdr->p_type != PT_LOAD)
		return false;

	if ((phdr->p_flags & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH)
		return false;

	if (!phdr->p_memsz)
		return false;

	return true;
}

static ssize_t hfi_mdt_load_split_segment(void *ptr, const struct elf32_phdr *phdrs,
	unsigned int segment, const char *fw_name,
	struct device *dev)
{
	const struct elf32_phdr *phdr = &phdrs[segment];
	const struct firmware *seg_fw = NULL;
	char *seg_name = NULL;
	ssize_t ret;

	if (strlen(fw_name) < 4) {
		HFI_CORE_ERR("firmware name too short: %s\n", fw_name);
		ret = -EINVAL;
		goto exit;
	}

	seg_name = kstrdup(fw_name, GFP_KERNEL);
	if (!seg_name) {
		HFI_CORE_ERR("failed to allocate memory for segment name\n");
		ret = -ENOMEM;
		goto exit;
	}

	/*
	 * Replace the last 3 characters of the firmware name (e.g. "mbn" in
	 * "dcp.mbn") with "bXX" to construct the split-segment filename
	 * (e.g. "dcp.b00", "dcp.b01").
	 */
	scnprintf(seg_name + strlen(fw_name) - 3, 4, "b%02d", segment);
	ret = request_firmware_into_buf(&seg_fw, seg_name, dev,
		ptr, phdr->p_filesz);
	if (ret) {
		HFI_CORE_ERR("error %zd loading %s\n", ret, seg_name);
		goto exit;
	}

	if (seg_fw->size != phdr->p_filesz) {
		HFI_CORE_ERR("failed to load segment %d from truncated file %s\n",
			segment, seg_name);
		ret = -EINVAL;
	}

exit:
	if (seg_fw)
		release_firmware(seg_fw);
	kfree(seg_name);

	return ret;
}

static bool hfi_qcom_mdt_bins_are_split(const struct firmware *fw, const char *fw_name)
{
	const struct elf32_phdr *phdrs;
	const struct elf32_hdr *ehdr;
	uint64_t seg_start, seg_end;
	int i;

	ehdr = (struct elf32_hdr *)fw->data;
	phdrs = (struct elf32_phdr *)(ehdr + 1);

	for (i = 0; i < ehdr->e_phnum; i++) {
		/*
		 * The size of the MDT file is not padded to include any
		 * zero-sized segments at the end. Ignore these, as they should
		 * not affect the decision about image being split or not.
		 */
		if (!phdrs[i].p_filesz)
			continue;

		seg_start = phdrs[i].p_offset;
		seg_end = phdrs[i].p_offset + phdrs[i].p_filesz;
		if (seg_end < seg_start || seg_start > fw->size || seg_end > fw->size)
			return true;
	}

	return false;
}

static int __hfi_qcom_mdt_load_io(struct device *dev, const struct firmware *fw,
	const char *fw_name, int pas_id, void *mem_region,
	phys_addr_t mem_phys, size_t mem_size,
	phys_addr_t *reloc_base, bool pas_init)
{
	const struct elf32_phdr *phdrs;
	const struct elf32_phdr *phdr;
	const struct elf32_hdr *ehdr;
	phys_addr_t mem_reloc;
	phys_addr_t min_addr = PHYS_ADDR_MAX;
	ssize_t offset;
	bool relocate = false;
	bool is_split;
	void *ptr;
	int ret = 0;
	int i;

	if (!fw || !fw->data || !mem_region || !mem_phys || !mem_size) {
		HFI_CORE_ERR("invalid params, fw: %d fw->data: %d mem_region: %d mem_phys: %d mem_size: %d\n",
			!fw, !fw || !fw->data, !mem_region, !mem_phys, !mem_size);
		return -EINVAL;
	}

	is_split = hfi_qcom_mdt_bins_are_split(fw, fw_name);
	ehdr = (struct elf32_hdr *)fw->data;
	phdrs = (struct elf32_phdr *)(ehdr + 1);

	for (i = 0; i < ehdr->e_phnum; i++) {
		phdr = &phdrs[i];

		if (!hfi_mdt_phdr_valid(phdr))
			continue;

		if (phdr->p_flags & QCOM_MDT_RELOCATABLE)
			relocate = true;

		if (phdr->p_paddr < min_addr)
			min_addr = phdr->p_paddr;
	}

	if (relocate) {
		/*
		 * The image is relocatable, so offset each segment based on
		 * the lowest segment address.
		 */
		mem_reloc = min_addr;
	} else {
		/*
		 * Image is not relocatable, so offset each segment based on
		 * the allocated physical chunk of memory.
		 */
		mem_reloc = mem_phys;
	}

	for (i = 0; i < ehdr->e_phnum; i++) {
		phdr = &phdrs[i];

		if (!hfi_mdt_phdr_valid(phdr))
			continue;

		offset = phdr->p_paddr - mem_reloc;
		if (offset < 0 || offset + phdr->p_memsz > mem_size) {
			HFI_CORE_ERR("segment outside memory range\n");
			ret = -EINVAL;
			break;
		}

		if (phdr->p_filesz > phdr->p_memsz) {
			HFI_CORE_ERR("refusing to load segment %d with p_filesz > p_memsz\n",
				i);
			ret = -EINVAL;
			break;
		}

		ptr = mem_region + offset;

		if (phdr->p_filesz && !is_split) {
			/* Firmware is large enough to be non-split */
			if (phdr->p_offset + phdr->p_filesz > fw->size) {
				HFI_CORE_ERR("file %s segment %d would be truncated\n",
					fw_name, i);
				ret = -EINVAL;
				break;
			}

			memcpy_toio(ptr, fw->data + phdr->p_offset, phdr->p_filesz);
		} else if (phdr->p_filesz) {
			/* Firmware not large enough, load split-out segments */
			ret = hfi_mdt_load_split_segment(ptr, phdrs, i, fw_name, dev);
			if (ret)
				break;
		}

		if (phdr->p_memsz > phdr->p_filesz)
			memset_io(ptr + phdr->p_filesz, 0, phdr->p_memsz - phdr->p_filesz);
	}

	if (reloc_base)
		*reloc_base = mem_reloc;

	return ret;
}

int hfi_qcom_mdt_load_io(struct device *dev, const struct firmware *fw,
	const char *firmware, int pas_id, void *mem_region,
	phys_addr_t mem_phys, size_t mem_size,
	phys_addr_t *reloc_base)
{
	int ret;

	ret = qcom_mdt_pas_init(dev, fw, firmware, pas_id, mem_phys, NULL, false);
	if (ret)
		return ret;

	return __hfi_qcom_mdt_load_io(dev, fw, firmware, pas_id, mem_region, mem_phys,
			       mem_size, reloc_base, true);
}

static int hfi_core_firmware_load_regions(struct hfi_core_drv_data *drv_data,
	struct hfi_core_firmware_info *fw_info)
{
	const struct firmware *firmware = NULL;
	ssize_t fw_size = 0;
	void *virt = NULL;
	struct device *dev = NULL;
	size_t effective_mem_size;
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

	if (IS_ERR_OR_NULL(firmware)) {
		HFI_CORE_ERR("Invalid firmware\n");
		return PTR_ERR(firmware);
	}
	fw_size = qcom_mdt_get_size(firmware);
	if (fw_size < 0) {
		ret = -EINVAL;
		HFI_CORE_ERR("invalid fw size %ld for \"%s\"\n",
			fw_size, fw_info->firmware_name);
		goto cleanup;
	}

	if (!fw_info->is_tcm && fw_info->fw_mem_size < (size_t)fw_size) {
		ret = -EINVAL;
		HFI_CORE_ERR("out of bound fw image fw size: %ld, fw_mem_size: %zu\n",
			fw_size, fw_info->fw_mem_size);
		goto cleanup;
	}

	effective_mem_size = fw_info->is_tcm ? (size_t)fw_size : fw_info->fw_mem_size;

	fw_info->fw_image_size = effective_mem_size;

	virt = memremap(fw_info->phys_fw_mem_addr, effective_mem_size, MEMREMAP_WC);
	if (!virt) {
		HFI_CORE_ERR("failed to remap fw memory phys %llu[p]\n",
			fw_info->phys_fw_mem_addr);
		ret = -ENOMEM;
		goto cleanup;
	}

	/* prevent system suspend during fw_load */
	pm_stay_awake(dev->parent);

	if (fw_info->is_tcm) {
		/*
		 * Device memory (mapped as Device-nGnRnE or similar) typically requires strictly
		 * aligned accesses. An unaligned access or a specific instruction type (like STP
		 * on some buses) can trigger an Alignment Fault or External Abort.
		 * Standard library functions (memcpy, memset) are optimized for Normal Memory
		 * (RAM) and often violate the strict access rules required by Device Memory.
		 * For device memory, _io Functions are implemented to respect alignment
		 * requirements, often copying data in strictly aligned chunks.
		 * Use effective_mem_size (actual fw size) to stay within AC-granted range.
		 */
		ret = hfi_qcom_mdt_load_io(dev, firmware, fw_info->firmware_name,
			fw_info->pas_id, virt, fw_info->phys_fw_mem_addr,
			effective_mem_size, NULL);
	} else {
		/* DDR path: standard qcom_mdt_load() with regular memcpy */
		ret = qcom_mdt_load(dev, firmware, fw_info->firmware_name,
			fw_info->pas_id, virt, fw_info->phys_fw_mem_addr,
			fw_info->fw_mem_size, NULL);
	}

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
		if (!drv_data->firmware_info[index].phys_fw_mem_addr) {
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
		if (!drv_data->firmware_info[index].phys_fw_mem_addr) {
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

		if (drv_data->firmware_info[i].is_tcm && drv_data->firmware_info[i].fw_image_size)
			fw_mem_size = drv_data->firmware_info[i].fw_image_size;
		else
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

		drv_data->firmware_info[i].is_tcm =
			of_property_read_bool(dev->of_node, "qcom,fw-mem-tcm");

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
		drv_data->firmware_info[i].fw_image_size = 0;
		drv_data->firmware_info[i].pas_id = 0;
		drv_data->firmware_info[i].firmware_name = NULL;
		drv_data->firmware_info[i].is_tcm = false;
	}

	HFI_CORE_DBG_H("-\n");
	return 0;
}

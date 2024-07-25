// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#include <linux/iommu.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/version.h>
#if (KERNEL_VERSION(6, 5, 0) <= LINUX_VERSION_CODE)
#include <linux/remoteproc/qcom_rproc.h>
#endif
#include <linux/remoteproc.h>
#include "hfi_interface.h"
#include "hfi_core_debug.h"
#include "hfi_core.h"
#include "hfi_smmu.h"

#define SOCCP_MAP_ADDR                                0xF0000000
#define SOCCP_DCP                                              1

struct hfi_smmu_info {
#ifdef SOCCP_DCP
	struct rproc *soccp_rproc;
#endif
	unsigned long soccp_map_iova_index;
	struct iommu_domain *domain;
};

static int get_drv_domain(struct hfi_core_drv_data *drv_data)
{
	struct hfi_smmu_info *smmu =
		(struct hfi_smmu_info *)drv_data->smmu_info.data;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data->dev) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	smmu->domain = iommu_get_domain_for_dev(drv_data->dev);
	if (IS_ERR_OR_NULL(smmu->domain)) {
		HFI_CORE_ERR("failed to get iommu domain for device ret:%ld\n",
			PTR_ERR(smmu->domain));
		return PTR_ERR(smmu->domain);
	}

	HFI_CORE_DBG_H("-\n");
	return 0;
}

#ifdef SOCCP_DCP
static int parse_dt_props(struct hfi_core_drv_data *drv_data)
{
	int ret;
	phandle ph;
	struct device_node *node;
	struct device *dev = NULL;
	struct hfi_smmu_info *smmu =
		(struct hfi_smmu_info *)drv_data->smmu_info.data;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data->dev) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}
	dev = (struct device *)drv_data->dev;
	node = dev->of_node;

	/* check presence of soccp */
	ret = of_property_read_u32(node, "soccp_controller",&ph);
	if (ret) {
		HFI_CORE_DBG_INFO("failed to get soccp controller: %u\n", ph);
		ret = -EINVAL;
		goto exit;
	}
	smmu->soccp_rproc = rproc_get_by_phandle(ph);
	if (IS_ERR_OR_NULL(smmu->soccp_rproc)) {
		HFI_CORE_DBG_INFO("failed to find rproc for phandle:%u\n", ph);
		ret = -EPROBE_DEFER;
		goto exit;
	}

exit:
	HFI_CORE_DBG_H("-\n");
	return ret;
}

/* soccp power vote */
static int set_power_vote(struct hfi_core_drv_data *drv_data, bool state)
{
	int ret = 0;
	struct hfi_smmu_info *smmu =
		(struct hfi_smmu_info *)drv_data->smmu_info.data;

	HFI_CORE_DBG_H("+\n");

#if (KERNEL_VERSION(6, 5, 0) <= LINUX_VERSION_CODE)
		if (!smmu->soccp_rproc) {
			HFI_CORE_ERR("smmu soccp proc is null\n");
			return -EINVAL;
		}
		ret = rproc_set_state(smmu->soccp_rproc, state);
#else
		ret = -EINVAL;
#endif

	HFI_CORE_DBG_H("-\n");

	return ret;
}
#endif

int smmu_alloc_and_map_for_drv(struct hfi_core_drv_data *drv_data,
    phys_addr_t *addr, size_t size, void **__iomem cpu_va, enum dma_alloc_type type)
{
    	void *p;
    	u32 dma_flags = 0;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->dev || !addr || !cpu_va) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	if (type == DMA_ALLOC_UNCACHE) {
		dma_flags = DMA_ATTR_NO_KERNEL_MAPPING | DMA_ATTR_WRITE_COMBINE;
	} else {
		HFI_CORE_ERR("unsupported dma alloc type %d requested\n",
			type);
		return -EINVAL;
	}

	p = dma_alloc_attrs(drv_data->dev, size, addr, GFP_KERNEL, dma_flags);
	if (!p) {
		HFI_CORE_ERR("Failed to allocate memory:0x%llx sz:%zu\n", *addr, size);
		return -ENOMEM;
	}

	*cpu_va = memremap(*addr, size, MEMREMAP_WB);
	memset_io(*cpu_va, 0x0, size);

	HFI_CORE_DBG_H("mapped allocated:0x%llx size:%zx cpu_va: 0x%llx\n",
		*addr, size, (u64)*cpu_va);

	HFI_CORE_DBG_H("-\n");
	return 0;
}

void smmu_unmap_for_drv(void *__iomem cpu_va)
{
	HFI_CORE_DBG_H("+\n");

	if (cpu_va)
		memunmap(cpu_va);

	HFI_CORE_DBG_H("-\n");
	return;
}

int smmu_mmap_for_fw(struct hfi_core_drv_data *drv_data, phys_addr_t addr,
	unsigned long *iova, size_t size, enum mmap_flags flags)
{
	int ret = 0;
    	u32 iommu_flags = 0;
	struct hfi_smmu_info *smmu = NULL;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->smmu_info.data || !iova) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}
	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;
	if (!smmu->domain) {
		HFI_CORE_ERR("smmu domain is null\n");
		return -EINVAL;
	}

	if (flags & MMAP_READ) {
		iommu_flags |= IOMMU_READ;
	}
	if (flags & MMAP_WRITE) {
		iommu_flags |= IOMMU_WRITE;
	}

	ret = iommu_map(smmu->domain, smmu->soccp_map_iova_index, addr,
		size, iommu_flags, GFP_KERNEL);
	if (ret) {
		HFI_CORE_ERR("iommu map failed for addr: 0x%llx size: %zx to addr: 0x%lx\n",
			addr, size, smmu->soccp_map_iova_index);
		return ret;
    	}
	*iova = smmu->soccp_map_iova_index;

	HFI_CORE_DBG_H("mapped memory:0x%llx size:%zx to addr:0x%lx\n",
		addr, size, smmu->soccp_map_iova_index);

	/* update soccp memory map addr index */
	smmu->soccp_map_iova_index += size;

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int smmu_unmmap_for_fw(struct hfi_core_drv_data *drv_data, unsigned long iova,
	size_t size)
{
	struct hfi_smmu_info *smmu = NULL;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->smmu_info.data) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}
	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;
	if (!smmu->domain) {
		HFI_CORE_ERR("smmu domain is null\n");
		return -EINVAL;
	}

	iommu_unmap(smmu->domain, iova, size);

	HFI_CORE_DBG_H("unmapped addr:0x%lx size: %zx\n", iova, size);

	HFI_CORE_DBG_H("-\n");
	return 0;
}

int init_smmu(struct hfi_core_drv_data *drv_data)
{
	int ret;
	struct hfi_smmu_info *smmu = NULL;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	smmu = kzalloc(sizeof(*smmu), GFP_KERNEL);
	if (!smmu) {
		ret = -ENOMEM;
		goto exit;
	}
	drv_data->smmu_info.data = (void *)smmu;

	ret = get_drv_domain(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to get domain\n");
		goto exit;
	}

#ifdef SOCCP_DCP
	ret = parse_dt_props(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to set dt properties\n");
		goto exit;
	}

	ret = set_power_vote(drv_data, true);
	if (ret) {
		HFI_CORE_ERR("failed to vote power\n");
		goto exit;
	}
#endif

	smmu->soccp_map_iova_index = SOCCP_MAP_ADDR;

exit:
	HFI_CORE_DBG_H("-\n");
	return ret;
}

int deinit_smmu(struct hfi_core_drv_data *drv_data)
{
	HFI_CORE_DBG_H("+\n");
	struct hfi_smmu_info *smmu = NULL;

	if (!drv_data || !drv_data->smmu_info.data) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}
	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;

#ifdef SOCCP_DCP
	if (smmu->soccp_rproc) {
		rproc_put(smmu->soccp_rproc);
	}
#endif

	HFI_CORE_DBG_H("-\n");
	return 0;
}

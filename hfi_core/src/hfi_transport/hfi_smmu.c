// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/iommu.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/genalloc.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#if (KERNEL_VERSION(6, 5, 0) <= LINUX_VERSION_CODE)
#include <linux/remoteproc/qcom_rproc.h>
#endif
#include <linux/remoteproc.h>
#include "hfi_interface.h"
#include "hfi_core_debug.h"
#include "hfi_core.h"
#include "hfi_smmu.h"

#define DCP_TRACE_EVENTS_ADDR_OFFSET                                   0x410000
#define DCP_DEBUG_LOG_ADDR_OFFSET                                      0x7BA000

#ifndef HFI_CORE_MAX_DEBUG_LOG_BYTES
#define HFI_CORE_MAX_DEBUG_LOG_BYTES                                   (256 * 1024)
#endif

#define HFI_DEBUG_MEM_REGIONS                                          2
#define HFI_DEBUG_MEM_REGION_TRACE_EVENTS                              0
#define HFI_DEBUG_MEM_REGION_DEBUG_MSG                                 1

/* max size supported for scatter-page allocation (sanity cap) */
#define DCP_MAX_PAGE_ALLOC_SIZE                                         4000000

/**
 * struct hfi_iova_mapping - Track individual IOVA mappings
 * @iova: Allocated IOVA address
 * @size: Size of the mapping
 * @list: List node for tracking
 */
struct hfi_iova_mapping {
	unsigned long iova;
	size_t size;
	struct list_head list;
};

struct hfi_smmu_info {
	struct gen_pool *iova_pool;
	unsigned long iova_start;
	unsigned long iova_end;
	struct list_head mappings;
	spinlock_t mapping_slock;
	struct iommu_domain *domain;
};

/**
 * struct hfi_debug_mem_region - Debug memory region descriptor
 *
 * @name:          Region name for logging
 * @size_wr:       Requested size in bytes
 * @size_aligned:  Aligned size in bytes
 * @iova_offset:   Fixed IOVA offset for this region
 * @cpu_va_offset: Offset within combined allocation for CPU VA
 * @phys_offset:   Offset within combined allocation for physical address
 */
struct hfi_debug_mem_region {
	const char *name;
	size_t size_wr;
	size_t size_aligned;
	unsigned long iova_offset;
	size_t cpu_va_offset;
	size_t phys_offset;
};

static const unsigned long hfi_debug_region_iova_offsets[HFI_DEBUG_MEM_REGIONS] = {
	[HFI_DEBUG_MEM_REGION_TRACE_EVENTS] = DCP_TRACE_EVENTS_ADDR_OFFSET,
	[HFI_DEBUG_MEM_REGION_DEBUG_MSG]    = DCP_DEBUG_LOG_ADDR_OFFSET,
};

static const char * const hfi_debug_region_names[HFI_DEBUG_MEM_REGIONS] = {
	[HFI_DEBUG_MEM_REGION_TRACE_EVENTS] = "trace events",
	[HFI_DEBUG_MEM_REGION_DEBUG_MSG]    = "debug strings",
};

/*
 * smmu_alloc_scatter_pages() - Allocate individual order-0 pages and build an
 * sg_table from them.  Each page is allocated independently so the kernel
 * never needs to find a physically-contiguous run larger than one page.
 *
 * @size:    PAGE_ALIGN()ed byte count to allocate.
 * @out_sgt: on success, points to the newly allocated sg_table.
 * @out_va:  on success, points to the vmapped kernel-virtual address.
 *
 * Returns 0 on success, negative errno on failure.
 * On failure all partially-allocated pages and the sg_table are freed.
 */
static int smmu_alloc_scatter_pages(size_t size, struct sg_table **out_sgt,
	void **out_va)
{
	struct sg_table *sgt;
	struct scatterlist *sg;
	struct page **pages;
	unsigned int num_pages = size >> PAGE_SHIFT;
	unsigned int total_pages = num_pages + 1; /* +1 trailer page to store sgt ptr */
	unsigned int i;
	void *va;
	int ret;

	/*
	 * Layout: pages[0..num_pages-1] are the data pages; pages[num_pages] is a
	 * trailer page used to store the sg_table pointer after the data region.
	 * The sg_table covers only the data pages.  vmap covers all total_pages so
	 * the trailer is accessible at va + num_pages*PAGE_SIZE.
	 * out_va = va, so cpu_va points directly at the data — callers see no offset.
	 */
	pages = kvmalloc_array(total_pages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	for (i = 0; i < total_pages; i++) {
		pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pages[i]) {
			HFI_CORE_ERR("alloc_page failed at index %u of %u\n",
				i, total_pages);
			ret = -ENOMEM;
			goto free_pages;
		}
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto free_pages;
	}

	/*
	 * One sg entry per data page — avoids the merging behaviour of
	 * sg_alloc_table_from_pages() which would cause page leaks on free.
	 */
	ret = sg_alloc_table(sgt, num_pages, GFP_KERNEL);
	if (ret) {
		HFI_CORE_ERR("sg_alloc_table failed: %d\n", ret);
		goto free_sgt;
	}
	for_each_sg(sgt->sgl, sg, num_pages, i)
		sg_set_page(sg, pages[i], PAGE_SIZE, 0);

	/* vmap: data pages first, trailer page last */
	va = vmap(pages, total_pages, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!va) {
		HFI_CORE_ERR("vmap failed for %u pages\n", total_pages);
		ret = -ENOMEM;
		goto free_sg_table;
	}

	/* Store sgt pointer in the trailer page for recovery on free */
	*(struct sg_table **)((u8 *)va + num_pages * PAGE_SIZE) = sgt;

	kvfree(pages);
	*out_sgt = sgt;
	*out_va  = va;
	return 0;

free_sg_table:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
free_pages:
	for (i = 0; i < total_pages && pages[i]; i++)
		__free_page(pages[i]);
	kvfree(pages);
	return ret;
}

/*
 * smmu_free_scatter_pages() - Unmap the vmap region and free every individual
 * page that was allocated by smmu_alloc_scatter_pages().
 */
static void smmu_free_scatter_pages(void *va, struct sg_table *sgt)
{
	struct scatterlist *sg;
	struct page *trailer_page;
	struct page *page;
	int i;

	if (!va || !sgt)
		return;

	/*
	 * The trailer page sits at va + orig_nents*PAGE_SIZE (just past the data).
	 * Retrieve its struct page *before* vunmap() invalidates the VA.
	 */
	trailer_page = vmalloc_to_page((u8 *)va + sgt->orig_nents * PAGE_SIZE);

	/* Release the entire vmap region (data pages + trailer page) */
	vunmap(va);

	/* Free the trailer page */
	if (trailer_page)
		__free_page(trailer_page);

	/* Free the data pages tracked in the sg_table (one entry per page) */
	for_each_sg(sgt->sgl, sg, sgt->orig_nents, i) {
		page = sg_page(sg);
		if (page)
			__free_page(page);
	}

	sg_free_table(sgt);
	kfree(sgt);
}

/**
 * smmu_mmap_debug_mem_for_fw() - Generic debug memory mapping function
 *
 * Maps debug memory (traces or strings) at a fixed IOVA offset.
 * Called twice: once for traces, once for strings.
 *
 * @drv_data:    Driver data
 * @addr:        Physical address to map
 * @iova:        Output IOVA address
 * @size:        Size to map
 * @iova_offset: Fixed IOVA offset from base address
 * @mem_name:    Name for debug logging
 *
 * Return: 0 on success, negative error code on failure
 */
static int smmu_mmap_debug_mem_for_fw(struct hfi_core_drv_data *drv_data,
				      struct sg_table *sgt, unsigned long *iova,
				      size_t size, unsigned long iova_offset,
				      const char *mem_name)
{
	ssize_t mapped;
	struct hfi_smmu_info *smmu = NULL;
	struct hfi_core_resource_info *res_info;
	enum hfi_core_client_id client;
	unsigned long map_iova;

	HFI_CORE_DBG_H("+ %s size=%zu offset=0x%lx\n",
		       mem_name, size, iova_offset);

	if (!drv_data || !drv_data->smmu_info.data || !iova || !sgt) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	client = drv_data->drv_client_id;
	if (client >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid client id: %u\n", client);
		return -EINVAL;
	}

	res_info = &drv_data->client_data[client].resource_info;
	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;
	if (!smmu->domain) {
		HFI_CORE_ERR("smmu domain is null\n");
		return -EINVAL;
	}

	map_iova = res_info->dcp_map_addr + iova_offset;

	/* ← replace iommu_map with iommu_map_sg */
#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
	mapped = iommu_map_sg(smmu->domain, map_iova, sgt->sgl,
			      sgt->orig_nents, IOMMU_READ | IOMMU_WRITE,
			      GFP_KERNEL);
#else
	mapped = iommu_map_sg(smmu->domain, map_iova, sgt->sgl,
			      sgt->orig_nents, IOMMU_READ | IOMMU_WRITE);
#endif

	if (mapped < 0) {
		HFI_CORE_ERR("%s map_sg failed: iova=0x%lx ret=%zd\n",
			     mem_name, map_iova, mapped);
		return (int)mapped;
	} else if ((size_t)mapped != size) {
		HFI_CORE_ERR("%s map_sg size mismatch: mapped=%zd expected=%zu\n",
			     mem_name, mapped, size);
		iommu_unmap(smmu->domain, map_iova, (size_t)mapped);
		return -EINVAL;
	}

	*iova = map_iova;
	HFI_CORE_DBG_H("mapped %s: iova=0x%lx size=0x%zx\n",
		       mem_name, map_iova, size);
	HFI_CORE_DBG_H("-\n");
	return 0;
}

static int get_drv_domain(struct hfi_core_drv_data *drv_data)
{
	struct hfi_smmu_info *smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;

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

static int parse_dt_props(struct hfi_core_drv_data *drv_data, enum hfi_core_client_id client)
{
	int ret;
	struct device_node *node;
	struct device *dev = NULL;
	unsigned int reg_config[2];
	struct hfi_core_resource_info *res_info = &drv_data->client_data[client].resource_info;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data->dev) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}
	dev = (struct device *)drv_data->dev;
	node = dev->of_node;

	ret = of_property_read_u32_array(dev->of_node, "qcom,device-map-addr-reg", reg_config, 2);
	if (ret) {
		HFI_CORE_ERR("failed to read swi reg, ret: %d\n", ret);
		goto exit;
	}

	res_info->dcp_map_addr = reg_config[0];
	res_info->dcp_map_addr_max_size = reg_config[1];

	/* Read device tree property for display collapse handling */
	drv_data->enable_dcp_fast_reset =
		of_property_read_bool(node, "qcom,enable-dcp-fast-reset");

exit:
	HFI_CORE_DBG_H("-\n");
	return ret;
}

/**
 * smmu_alloc_iova() - Allocate IOVA from the gen_pool
 *
 * @smmu: SMMU info structure
 * @size: Size to allocate (will be page-aligned)
 *
 * Allocate an IOVA address using gen_pool allocator.
 *
 * Return: Allocated IOVA address, or 0 on failure
 */
static unsigned long smmu_alloc_iova(struct hfi_smmu_info *smmu, size_t size)
{
	unsigned long iova;

	HFI_CORE_DBG_H("+\n");

	if (!smmu || !smmu->iova_pool) {
		HFI_CORE_ERR("invalid SMMU info or IOVA pool (smmu=%pK pool=%pK)\n",
		       smmu, smmu ? smmu->iova_pool : NULL);
		return 0;
	}

	/* Align size to page boundary */
	size = PAGE_ALIGN(size);

	/* Allocate IOVA using gen_pool */
	iova = gen_pool_alloc(smmu->iova_pool, size);
	if (!iova) {
		HFI_CORE_ERR("IOVA allocation failed (size=%zu avail=%zu)\n",
		       size, gen_pool_avail(smmu->iova_pool));
		return 0;
	}

	HFI_CORE_DBG_H("Allocated IOVA: 0x%lx size=%zu\n", iova, size);

	HFI_CORE_DBG_H("-\n");
	return iova;
}

/**
 * smmu_free_iova() - Free IOVA back to the gen_pool
 * @smmu: SMMU info structure
 * @iova: IOVA address to free
 * @size: Size of the mapping
 */
static void smmu_free_iova(struct hfi_smmu_info *smmu, unsigned long iova, size_t size)
{
	HFI_CORE_DBG_H("+\n");

	if (!smmu || !smmu->iova_pool) {
		HFI_CORE_ERR("invalid SMMU info or IOVA pool (smmu=%pK pool=%pK)\n",
		       smmu, smmu ? smmu->iova_pool : NULL);
		return;
	}

	if (!iova) {
		HFI_CORE_WARN("Attempting to free NULL IOVA\n");
		return;
	}

	size = PAGE_ALIGN(size);
	gen_pool_free(smmu->iova_pool, iova, size);

	HFI_CORE_DBG_H("Freed IOVA: 0x%lx size=%zu\n", iova, size);
	HFI_CORE_DBG_H("-\n");
}

int smmu_alloc_and_map_for_drv(struct hfi_core_drv_data *drv_data,
	phys_addr_t *addr, size_t size, void **__iomem cpu_va,
	enum hfi_core_dma_alloc_type type, struct sg_table **out_sgt)
{
	struct sg_table *sgt = NULL;
	void *va = NULL;
	int ret;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->dev || !addr || !cpu_va) {
		HFI_CORE_ERR("invalid params drv_data %pK device %pK addr %pK cpu_va %pK\n",
			drv_data, (drv_data ? drv_data->dev : NULL), addr, cpu_va);
		return -EINVAL;
	}

	if (size > DCP_MAX_PAGE_ALLOC_SIZE) {
		HFI_CORE_ERR("invalid size to allocate: %zx, max supported: %x\n", size,
			DCP_MAX_PAGE_ALLOC_SIZE);
		return -EINVAL;
	}

	if (type != HFI_CORE_DMA_ALLOC_UNCACHE) {
		HFI_CORE_ERR("unsupported dma alloc type %d requested\n", type);
		return -EINVAL;
	}

	size = PAGE_ALIGN(size);

	/*
	 * Allocate memory as individual order-0 pages instead of a single
	 * physically-contiguous region.  This avoids high-order page allocator
	 * pressure (previously up to order-6 for the 256 KB async queue buffers)
	 * while still giving the firmware a contiguous IOVA range via the IOMMU
	 * scatter-gather map that follows in smmu_mmap_sgt_for_fw().
	 */
	HFI_CORE_DBG_H("scatter alloc: size:%zx num_pages:%zu\n",
		size, size >> PAGE_SHIFT);

	ret = smmu_alloc_scatter_pages(size, &sgt, &va);
	if (ret) {
		HFI_CORE_ERR("scatter page alloc failed: %d\n", ret);
		return ret;
	}

	/*
	 * cpu_va points directly at the start of the data region (va).
	 * The sgt pointer is stored in the trailer page at va + size
	 * and is also returned via out_sgt for callers that store it.
	 */
	*cpu_va  = va;
	*addr    = page_to_phys(sg_page(sgt->sgl));
	*out_sgt = sgt;

	HFI_CORE_DBG_H("scatter alloc done: cpu_va:%p phys:0x%llx pages:%zu\n",
		va, (unsigned long long)*addr, size >> PAGE_SHIFT);

	HFI_CORE_DBG_H("-\n");
	return 0;
}

void smmu_unmap_for_drv(void *cpu_va, struct sg_table *sgt)
{
	HFI_CORE_DBG_H("+\n");

	if (!cpu_va)
		return;

	smmu_free_scatter_pages(cpu_va, sgt);

	HFI_CORE_DBG_H("-\n");
}

int smmu_mmap_for_fw(struct hfi_core_drv_data *drv_data, phys_addr_t addr,
	unsigned long *iova, size_t size, u32 flags)
{
	int ret = 0;
	u32 iommu_flags = 0;
	struct hfi_smmu_info *smmu = NULL;
	struct hfi_iova_mapping *mapping;
	unsigned long allocated_iova;

	HFI_CORE_DBG_H("+ addr=0x%llx size=%zu\n", addr, size);

	if (!drv_data || !drv_data->smmu_info.data || !iova) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;
	if (!smmu || !smmu->domain || !smmu->iova_pool) {
		HFI_CORE_ERR("invalid params smmu: 0x%pK domain: 0x%pK iova_pool: 0x%pK\n",
			smmu, smmu ? smmu->domain : NULL, smmu ? smmu->iova_pool : NULL);
		return -EINVAL;
	}

	if (flags & HFI_CORE_MMAP_READ)
		iommu_flags |= IOMMU_READ;

	if (flags & HFI_CORE_MMAP_WRITE)
		iommu_flags |= IOMMU_WRITE;

	if (flags & HFI_CORE_MMAP_CACHE)
		iommu_flags |= IOMMU_CACHE;

	/* Allocate IOVA */
	allocated_iova = smmu_alloc_iova(smmu, size);
	if (!allocated_iova) {
		HFI_CORE_ERR("Failed to allocate IOVA (size=%zu)\n", size);
		return -ENOMEM;
	}

	/* Create mapping structure for tracking */
	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping) {
		HFI_CORE_ERR("Failed to allocate mapping structure\n");
		ret = -ENOMEM;
		goto free_iova;
	}

	mapping->iova = allocated_iova;
	mapping->size = PAGE_ALIGN(size);

	/* Perform IOMMU mapping */
#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
	ret = iommu_map(smmu->domain, allocated_iova, addr, size, iommu_flags, GFP_KERNEL);
#else
	ret = iommu_map(smmu->domain, allocated_iova, addr, size, iommu_flags);
#endif

	if (ret) {
		HFI_CORE_ERR("failed: phys=0x%llx sz=%zu iova=0x%lx flags: 0x%x ret=%d\n",
			addr, size, allocated_iova, flags, ret);
		goto free_mapping;
	}

	/* Add to tracking list */
	spin_lock(&smmu->mapping_slock);
	list_add_tail(&mapping->list, &smmu->mappings);
	spin_unlock(&smmu->mapping_slock);

	*iova = allocated_iova;

	HFI_CORE_DBG_H("mapped: phys=0x%llx size=0x%zx iova=0x%lx flags=0x%x\n",
		addr, size, allocated_iova, iommu_flags);

	HFI_CORE_DBG_H("-\n");
	return 0;

free_mapping:
	kfree(mapping);
free_iova:
	if (allocated_iova)
		smmu_free_iova(smmu, allocated_iova, size);
	return ret;
}

int smmu_mmap_sgt_for_fw(struct hfi_core_drv_data *drv_data, struct sg_table *sgt,
		size_t size, unsigned long *iova, u32 flags)
{
	int ret = 0;
	ssize_t mapped;
	u32 iommu_flags = 0;
	struct hfi_smmu_info *smmu;
	struct hfi_iova_mapping *mapping;
	unsigned long allocated_iova;

	HFI_CORE_DBG_H("+ size=%zu\n", size);

	if (!drv_data || !drv_data->smmu_info.data || !iova) {
		HFI_CORE_ERR("invalid drv_data params or iova\n");
		return -EINVAL;
	}

	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;
	if (!smmu || !smmu->domain || !smmu->iova_pool) {
		HFI_CORE_ERR("invalid params smmu: 0x%pK domain: 0x%pK iova_pool: 0x%pK\n",
			smmu, smmu ? smmu->domain : NULL, smmu ? smmu->iova_pool : NULL);
		return -EINVAL;
	}

	if (flags & HFI_CORE_MMAP_READ)
		iommu_flags |= IOMMU_READ;

	if (flags & HFI_CORE_MMAP_WRITE)
		iommu_flags |= IOMMU_WRITE;

	if (flags & HFI_CORE_MMAP_CACHE)
		iommu_flags |= IOMMU_CACHE;

	/* Allocate IOVA */
	allocated_iova = smmu_alloc_iova(smmu, size);
	if (!allocated_iova) {
		HFI_CORE_ERR("Failed to allocate IOVA (size=%zu)\n", size);
		return -ENOMEM;
	}

	/* Create mapping structure for tracking */
	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping) {
		HFI_CORE_ERR("Failed to allocate mapping structure\n");
		ret = -ENOMEM;
		goto free_iova;
	}

	mapping->iova = allocated_iova;
	mapping->size = PAGE_ALIGN(size);

	/* Perform IOMMU mapping */

#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
	mapped = iommu_map_sg(smmu->domain, allocated_iova, sgt->sgl,
		sgt->orig_nents, iommu_flags, GFP_ATOMIC);
#else
	mapped = iommu_map_sg(smmu->domain, allocated_iova, sgt->sgl, sgt->orig_nents, iommu_flags);
#endif

	if (mapped < 0) {
		HFI_CORE_ERR("iommu_map_sg failed: iova=0x%lx flags: 0x%x ret=%zd\n",
			allocated_iova, flags, mapped);
		ret = mapped;
		goto free_mapping;
	} else if ((size_t)mapped != size) {
		HFI_CORE_ERR("iommu_map_sg size mismatch: expected=0x%zx mapped=%zd\n",
			size, mapped);
		/* Unmap the partial mapping before freeing IOVA */
		iommu_unmap(smmu->domain, allocated_iova, (size_t)mapped);
		ret = -EINVAL;
		goto free_mapping;
	}

	/* Add to tracking list */
	spin_lock(&smmu->mapping_slock);
	list_add_tail(&mapping->list, &smmu->mappings);
	spin_unlock(&smmu->mapping_slock);

	*iova = allocated_iova;

	HFI_CORE_DBG_H("mapped sgt: iova=0x%lx flags=0x%x size=0x%zx\n",
		allocated_iova, iommu_flags, mapped);

	HFI_CORE_DBG_H("-\n");
	return 0;

free_mapping:
	kfree(mapping);
free_iova:
	smmu_free_iova(smmu, allocated_iova, size);
	return ret;
}

int smmu_remap_sgt_for_fw(struct hfi_core_drv_data *drv_data, struct sg_table *sgt,
		size_t size, unsigned long target_iova, u32 flags)
{
	ssize_t mapped;
	u32 iommu_flags = 0;
	struct hfi_smmu_info *smmu = NULL;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->smmu_info.data || !target_iova) {
		HFI_CORE_ERR("invalid drv_data params or target_iova\n");
		return -EINVAL;
	}
	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;
	if (!smmu->domain) {
		HFI_CORE_ERR("smmu domain is null\n");
		return -EINVAL;
	}

	if (flags & HFI_CORE_MMAP_READ)
		iommu_flags |= IOMMU_READ;

	if (flags & HFI_CORE_MMAP_WRITE)
		iommu_flags |= IOMMU_WRITE;

	if (flags & HFI_CORE_MMAP_CACHE)
		iommu_flags |= IOMMU_CACHE;

#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
	mapped = iommu_map_sg(smmu->domain, target_iova, sgt->sgl, sgt->orig_nents,
		iommu_flags, GFP_ATOMIC);
#else
	mapped = iommu_map_sg(smmu->domain, target_iova, sgt->sgl, sgt->orig_nents,
		iommu_flags);
#endif

	if (mapped < 0) {
		HFI_CORE_ERR("iommu remap failed for sgt to addr: 0x%lx ret: %zd\n",
			target_iova, mapped);
		return (int)mapped;
	} else if ((size_t)mapped != size) {
		HFI_CORE_ERR("iommu remap size mismatch mapped: %zd size: %zu\n", mapped, size);
		return -EINVAL;
	}

	HFI_CORE_DBG_INIT("remapped sgt to fixed addr:0x%lx iommu_flags:0x%x size:%zd\n",
		target_iova, iommu_flags, mapped);

	/* target_iova is a fixed address; IOVA pool is not advanced */

	/* soccp_map_iova_index is intentionally NOT advanced */
	HFI_CORE_DBG_H("-\n");
	return 0;
}

int smmu_unmmap_for_fw(struct hfi_core_drv_data *drv_data, unsigned long iova, size_t size)
{
	struct hfi_smmu_info *smmu;
	struct hfi_iova_mapping *mapping, *tmp;
	bool found = false;

	HFI_CORE_DBG_H("+ iova=0x%lx size=%zu\n", iova, size);

	if (!drv_data || !drv_data->smmu_info.data) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}
	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;
	if (!smmu->domain) {
		HFI_CORE_ERR("smmu domain is null\n");
		return -EINVAL;
	}

	/* Find and remove mapping from tracking list */
	spin_lock(&smmu->mapping_slock);
	list_for_each_entry_safe(mapping, tmp, &smmu->mappings, list) {
		if (mapping->iova == iova) {
			list_del(&mapping->list);
			/* kfree() is safe inside spinlock */
			kfree(mapping);
			found = true;
			break;
		}
	}
	spin_unlock(&smmu->mapping_slock);

	if (!found) {
		HFI_CORE_WARN("unmap: mapping not found (iova=0x%lx size=%zu)\n",
			iova, size);
	}

	/* Perform IOMMU unmapping - use page-aligned size to match allocation */
	size = PAGE_ALIGN(size);
	iommu_unmap(smmu->domain, iova, size);

	/* Free IOVA back to allocator for reuse */
	smmu_free_iova(smmu, iova, size);

	HFI_CORE_DBG_H("unmapped: iova=0x%lx size=%zu\n", iova, size);

	HFI_CORE_DBG_H("-\n");
	return 0;
}

/**
 * hfi_calc_debug_mem_region_size() - Calculate debug memory region size
 *
 * @region_type: Type of region (0=trace events, 1=debug strings)
 * @size_wr:     Output parameter for requested size (before alignment)
 *
 * Return: Page-aligned allocation size
 */
static size_t hfi_calc_debug_mem_region_size(int region_type, size_t *size_wr)
{
	size_t req_size;
	size_t aligned_size;

	HFI_CORE_DBG_H("+\n");

	if (region_type == HFI_DEBUG_MEM_REGION_TRACE_EVENTS) {
		req_size = sizeof(struct hfi_core_trace_event) *
			   HFI_CORE_MAX_TRACE_EVENTS;
	} else if (region_type == HFI_DEBUG_MEM_REGION_DEBUG_MSG) {
		req_size = sizeof(struct hfi_fw_debug_msg_ring) +
			   HFI_CORE_MAX_DEBUG_LOG_BYTES;
	} else {
		HFI_CORE_ERR("invalid region type: %d\n", region_type);
		if (size_wr)
			*size_wr = 0;
		return 0;
	}

	/* Calculate page-aligned size */
	aligned_size = PAGE_ALIGN(req_size);

	if (size_wr)
		*size_wr = req_size;

	HFI_CORE_DBG_H("-\n");

	return aligned_size;
}

/**
 * hfi_init_fw_trace_mem() - Allocate and map all debug memory regions.
 *
 * Iterates over the debug region descriptor table, allocating and
 * mapping each region independently. On failure, unwinds all
 * previously successful allocations in reverse order.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int hfi_init_fw_trace_mem(struct hfi_core_drv_data *drv_data)
{
	int ret = 0, i;
	struct hfi_memory_alloc_info *alloc[HFI_DEBUG_MEM_REGIONS] = { NULL };
	struct hfi_smmu_info *smmu;
	struct hfi_core_resource_info *res_info;
	enum hfi_core_client_id client;
	/*
	 * Region descriptors - order must match HFI_DEBUG_MEM_REGION_*
	 * defines so that alloc[] indices correspond correctly.
	 */
	static const struct {
		const char     *name;
		unsigned long   iova_offset;
		int             region_type;
	} regions[HFI_DEBUG_MEM_REGIONS] = {
		[HFI_DEBUG_MEM_REGION_TRACE_EVENTS] = {
			.name        = "trace events",
			.iova_offset = DCP_TRACE_EVENTS_ADDR_OFFSET,
			.region_type = HFI_DEBUG_MEM_REGION_TRACE_EVENTS,
		},
		[HFI_DEBUG_MEM_REGION_DEBUG_MSG] = {
			.name        = "debug strings",
			.iova_offset = DCP_DEBUG_LOG_ADDR_OFFSET,
			.region_type = HFI_DEBUG_MEM_REGION_DEBUG_MSG,
		},
	};

	HFI_CORE_DBG_H("+\n");

	client = drv_data->drv_client_id;
	if (client >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid client id: %u\n", client);
		return -EINVAL;
	}

	/* Skip trace and log memory allocation for trusted VM use case */
	if (client == HFI_CORE_CLIENT_ID_1)
		return 0;

	res_info = &drv_data->client_data[client].resource_info;
	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;

	for (i = 0; i < HFI_DEBUG_MEM_REGIONS; i++) {
		size_t size_wr;
		size_t size_aligned = hfi_calc_debug_mem_region_size(regions[i].region_type,
								     &size_wr);

		alloc[i] = kzalloc(sizeof(*alloc[i]), GFP_KERNEL);
		if (!alloc[i]) {
			ret = -ENOMEM;
			goto unwind;
		}

		alloc[i]->size_wr = size_wr;
		alloc[i]->size_allocated = size_aligned;

		ret = smmu_alloc_and_map_for_drv(drv_data, &alloc[i]->phy_addr,
						 alloc[i]->size_allocated,
						 &alloc[i]->cpu_va, HFI_CORE_DMA_ALLOC_UNCACHE,
						 &alloc[i]->sgt);
		if (ret) {
			HFI_CORE_ERR("failed to alloc %s ret=%d\n",
				     regions[i].name, ret);
			goto unwind;
		}

		memset_io(alloc[i]->cpu_va, 0x0, alloc[i]->size_allocated);

		ret = smmu_mmap_debug_mem_for_fw(drv_data, alloc[i]->sgt,
						 &alloc[i]->mapped_iova, alloc[i]->size_allocated,
						 hfi_debug_region_iova_offsets[i],
						 hfi_debug_region_names[i]);
		if (ret) {
			HFI_CORE_ERR("failed to map %s ret=%d\n",
				     regions[i].name, ret);
			/*
			 * smmu_alloc_and_map_for_drv succeeded for this index
			 * so increment i before unwind so the unwind loop
			 * also cleans up this region's drv mapping.
			 */
			i++;
			goto unwind;
		}

		HFI_CORE_DBG_H("%s: cpu_va=0x%llx iova=0x%lx size=0x%zx\n",
			       regions[i].name, (u64)alloc[i]->cpu_va,
			       alloc[i]->mapped_iova,
			       alloc[i]->size_allocated);
	}

	/* Initialize debug log ring buffer header */
	{
		struct hfi_fw_debug_msg_ring *dbg_ring =
			(struct hfi_fw_debug_msg_ring *)
			alloc[HFI_DEBUG_MEM_REGION_DEBUG_MSG]->cpu_va;

		dbg_ring->write_idx = 0;
		dbg_ring->read_idx  = 0;
		dbg_ring->size      = HFI_CORE_MAX_DEBUG_LOG_BYTES;
		memset(dbg_ring->reserved, 0, sizeof(dbg_ring->reserved));
	}

	drv_data->fw_trace_mem    = alloc[HFI_DEBUG_MEM_REGION_TRACE_EVENTS];
	drv_data->fw_debug_msg_mem = alloc[HFI_DEBUG_MEM_REGION_DEBUG_MSG];

	HFI_CORE_DBG_H("-\n");
	return 0;

unwind:
	/*
	 * Unwind in reverse: for each successfully mapped region unmap
	 * from FW then free the drv allocation. alloc[j] is NULL for
	 * indices that were never reached so kfree(NULL) is safe.
	 */
	for (i--; i >= 0; i--) {
		if (!alloc[i])
			continue;
		iommu_unmap(smmu->domain,
			    res_info->dcp_map_addr + regions[i].iova_offset,
			    PAGE_ALIGN(alloc[i]->size_allocated));
		smmu_unmap_for_drv(alloc[i]->cpu_va, alloc[i]->sgt);
		kfree(alloc[i]);
	}
	return ret;
}

/**
 * hfi_deinit_fw_trace_mem() - Unmap and free all debug memory regions.
 *
 * Iterates over the debug region descriptor table in reverse,
 * unmapping from FW IOVA space and freeing each allocation.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int hfi_deinit_fw_trace_mem(struct hfi_core_drv_data *drv_data)
{
	int ret = 0, i;
	struct hfi_smmu_info *smmu;
	enum hfi_core_client_id client;
	struct hfi_memory_alloc_info *alloc[HFI_DEBUG_MEM_REGIONS];
	static const struct {
		const char *name;
	} regions[HFI_DEBUG_MEM_REGIONS] = {
		[HFI_DEBUG_MEM_REGION_TRACE_EVENTS] = { .name = "trace events"  },
		[HFI_DEBUG_MEM_REGION_DEBUG_MSG]    = { .name = "debug strings" },
	};

	HFI_CORE_DBG_H("+\n");

	/* Handle case where trace memory was not allocated for TVM */
	if (!drv_data || !drv_data->fw_trace_mem) {
		HFI_CORE_DBG_H("trace memory not allocated, skipping\n");
		return 0;
	}

	client = drv_data->drv_client_id;
	if (client >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid client id: %u\n", client);
		return -EINVAL;
	}

	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;
	if (!smmu || !smmu->domain) {
		HFI_CORE_ERR("invalid SMMU info\n");
		return -EINVAL;
	}

	alloc[HFI_DEBUG_MEM_REGION_TRACE_EVENTS] = drv_data->fw_trace_mem;
	alloc[HFI_DEBUG_MEM_REGION_DEBUG_MSG]    = drv_data->fw_debug_msg_mem;

	/* Unmap and free in reverse order */
	for (i = HFI_DEBUG_MEM_REGIONS - 1; i >= 0; i--) {
		if (!alloc[i])
			continue;

		HFI_CORE_DBG_H("freeing %s: iova=0x%lx size=0x%zx\n",
			       regions[i].name, alloc[i]->mapped_iova,
			       alloc[i]->size_allocated);

		iommu_unmap(smmu->domain,
			    alloc[i]->mapped_iova,
			    PAGE_ALIGN(alloc[i]->size_allocated));
		smmu_unmap_for_drv(alloc[i]->cpu_va, alloc[i]->sgt);
		kfree(alloc[i]);
	}

	drv_data->fw_trace_mem     = NULL;
	drv_data->fw_debug_msg_mem = NULL;

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int init_smmu(struct hfi_core_drv_data *drv_data)
{
	int ret;
	struct hfi_smmu_info *smmu = NULL;
	struct hfi_core_resource_info *res_info;
	enum hfi_core_client_id client;
	int i;
	unsigned long region_starts[HFI_DEBUG_MEM_REGIONS];
	unsigned long region_ends[HFI_DEBUG_MEM_REGIONS];
	size_t region_sizes[HFI_DEBUG_MEM_REGIONS];

	HFI_CORE_DBG_H("+\n");

	if (!drv_data) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}
	client = drv_data->drv_client_id;

	if (client >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid client id: %u\n", client);
		return -EINVAL;
	}

	res_info = &drv_data->client_data[client].resource_info;

	smmu = kzalloc(sizeof(*smmu), GFP_KERNEL);
	if (!smmu) {
		ret = -ENOMEM;
		goto exit;
	}
	drv_data->smmu_info.data = (void *)smmu;

	ret = get_drv_domain(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to get domain\n");
		goto free_smmu;
	}

	ret = parse_dt_props(drv_data, client);
	if (ret) {
		HFI_CORE_ERR("failed to set dt properties\n");
		goto free_smmu;
	}

	/* Store IOVA range */
	smmu->iova_start = res_info->dcp_map_addr;
	smmu->iova_end = res_info->dcp_map_addr + res_info->dcp_map_addr_max_size - 1;

	/* Allocate and initialize gen_pool for IOVA management */
	smmu->iova_pool = gen_pool_create(PAGE_SHIFT, -1);
	if (!smmu->iova_pool) {
		HFI_CORE_ERR("failed to create IOVA gen_pool\n");
		ret = -ENOMEM;
		goto free_smmu;
	}

	/* Use best-fit algorithm for efficient IOVA space utilization */
	gen_pool_set_algo(smmu->iova_pool, gen_pool_best_fit, NULL);

	/* Calculate reserved trace memory regions */
	for (i = 0; i < HFI_DEBUG_MEM_REGIONS; i++) {
		region_sizes[i]  = hfi_calc_debug_mem_region_size(i, NULL);
		region_starts[i] = smmu->iova_start + hfi_debug_region_iova_offsets[i];
		region_ends[i]   = region_starts[i] + region_sizes[i];
	}

	/*
	 * Add the IOVA range to the pool, excluding the trace memory
	 * regions which are at fixed offsets and have fixed lifetimes.
	 */

	/* Add range before first region */
	if (smmu->iova_start < region_starts[0]) {
		ret = gen_pool_add(smmu->iova_pool, smmu->iova_start,
				   region_starts[0] - smmu->iova_start,
				   -1);
		if (ret) {
			HFI_CORE_ERR("failed to add range (0x%lx-0x%lx) len: 0x%lx ret=%d\n",
				     smmu->iova_start, region_starts[0] - 1,
				     region_starts[0] - smmu->iova_start, ret);
			goto free_pool;
		}
	}

	/* Add ranges between regions */
	for (i = 0; i < HFI_DEBUG_MEM_REGIONS - 1; i++) {
		unsigned long gap_start = region_ends[i];
		unsigned long gap_end = region_starts[i + 1];

		if (gap_start < gap_end) {
			ret = gen_pool_add(smmu->iova_pool, gap_start,
					   gap_end - gap_start, -1);
			if (ret) {
				HFI_CORE_ERR("failed to add range (0x%lx-0x%lx) len 0x%lx ret=%d\n",
					     gap_start, gap_end - 1,
					     gap_end - gap_start, ret);
				goto free_pool;
			}
		}
	}

	/* Add range after last region */
	if (region_ends[HFI_DEBUG_MEM_REGIONS - 1] <= smmu->iova_end) {
		unsigned long gap_start =
			region_ends[HFI_DEBUG_MEM_REGIONS - 1];
		unsigned long gap_size =
			smmu->iova_end - gap_start + 1;

		ret = gen_pool_add(smmu->iova_pool, gap_start,
				   gap_size, -1);
		if (ret) {
			HFI_CORE_ERR("failed to add range (0x%lx - 0x%lx) len: 0x%lx to IOVA pool ret=%d\n",
				gap_start, smmu->iova_end, gap_size, ret);
			goto free_pool;
		}
	}

	/* Initialize mapping tracking */
	INIT_LIST_HEAD(&smmu->mappings);
	spin_lock_init(&smmu->mapping_slock);

	ret = hfi_init_fw_trace_mem(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to init fw trace mem ret: %d\n", ret);
		goto free_pool;
	}

	HFI_CORE_DBG_INIT("hfi_core: SMMU initialized: IOVA range 0x%lx-0x%lx\n",
			  smmu->iova_start, smmu->iova_end);

	for (i = 0; i < HFI_DEBUG_MEM_REGIONS; i++) {
		HFI_CORE_DBG_INIT("hfi_core: Reserved %s: 0x%lx-0x%lx\n",
				  hfi_debug_region_names[i], region_starts[i],
				  region_ends[i] - 1);
	}

	HFI_CORE_DBG_H("-\n");
	return 0;

free_pool:
	gen_pool_destroy(smmu->iova_pool);
	smmu->iova_pool = NULL;
free_smmu:
	kfree(smmu);
	drv_data->smmu_info.data = NULL;
exit:
	HFI_CORE_DBG_H("-\n");
	return ret;
}

int deinit_smmu(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;
	struct hfi_smmu_info *smmu = NULL;
	struct hfi_iova_mapping *mapping, *tmp;
	LIST_HEAD(mappings_to_free);

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !drv_data->smmu_info.data) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}
	smmu = (struct hfi_smmu_info *)drv_data->smmu_info.data;

	ret = hfi_deinit_fw_trace_mem(drv_data);
	if (ret) {
		HFI_CORE_ERR("failed to deinit fw trace mem\n");
		return ret;
	}

	/* Move all mappings to temporary list while holding spinlock */
	spin_lock(&smmu->mapping_slock);
	list_for_each_entry_safe(mapping, tmp, &smmu->mappings, list) {
		list_move(&mapping->list, &mappings_to_free);
	}
	spin_unlock(&smmu->mapping_slock);

	/*
	 * iommu_unmap() and smmu_free_iova() (gen_pool_free) must not be
	 * called while holding a spinlock as they may sleep or acquire
	 * other locks internally. kfree() itself is spinlock-safe but
	 * kept here since we are already outside the lock.
	 */
	list_for_each_entry_safe(mapping, tmp, &mappings_to_free, list) {
		/* Unmap mapping */
		iommu_unmap(smmu->domain, mapping->iova, mapping->size);

		/* Free IOVA */
		smmu_free_iova(smmu, mapping->iova, mapping->size);
		kfree(mapping);
	}

	/* Destroy IOVA pool */
	if (smmu->iova_pool) {
		gen_pool_destroy(smmu->iova_pool);
		smmu->iova_pool = NULL;
	}

	kfree(drv_data->smmu_info.data);
	drv_data->smmu_info.data = NULL;

	HFI_CORE_DBG_H("-\n");
	return 0;
}

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __HW_FENCE_DRV_HW_DMA_FENCE
#define __HW_FENCE_DRV_HW_DMA_FENCE

#define HW_FENCE_NAME_SIZE 64

/**
 * struct hw_dma_fence - fences internally created by hw-fence driver.
 * @base: base dma-fence structure, this must remain at beginning of the struct.
 * @name: name of each fence.
 * @client_handle: handle for the client owner of this fence, this is returned by the hw-fence
 *                 driver after a successful registration of the client and used by this fence
 *                 during release.
 * @data: internal data to process the fence ops.
 * @dma_fence_key: key for the dma-fence hash table.
 * @is_internal: true if this fence is initialized internally by hw-fence driver, false otherwise
 * @signal_cb: drv_data, hash, and signal_cb of hw_fence
 * @node: node for fences held in the dma-fences hash table linked lists
 */
struct hw_dma_fence {
	struct dma_fence base;
	char name[HW_FENCE_NAME_SIZE];
	void *client_handle;
	u32 dma_fence_key;
	bool is_internal;
	struct hw_fence_signal_cb signal_cb;
	struct hlist_node node;
};

extern struct dma_fence_ops hw_fence_dbg_ops;

static inline struct hw_dma_fence *to_hw_dma_fence(struct dma_fence *fence)
{
	return container_of(fence, struct hw_dma_fence, base);
}

bool dma_fence_is_hw_dma(struct dma_fence *fence);

struct dma_fence *hw_dma_fence_init(struct msm_hw_fence_client *hw_fence_client, u64 context,
	u64 seqno);

#endif /* __HW_FENCE_DRV_HW_DMA_FENCE */

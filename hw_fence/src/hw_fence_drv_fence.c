// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include "hw_fence_drv_priv.h"
#include "hw_fence_drv_fence.h"
#include "hw_fence_drv_debug.h"

static const char *hw_fence_dbg_get_driver_name(struct dma_fence *fence)
{
	struct hw_dma_fence *hw_dma_fence = to_hw_dma_fence(fence);

	return hw_dma_fence->name;
}

static const char *hw_fence_dbg_get_timeline_name(struct dma_fence *fence)
{
	struct hw_dma_fence *hw_dma_fence = to_hw_dma_fence(fence);

	return hw_dma_fence->name;
}

static bool hw_fence_dbg_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static void _hw_fence_release(struct hw_dma_fence *hw_dma_fence)
{
	int ret = 0;

	if (IS_ERR_OR_NULL(hw_dma_fence->client_handle) || (hw_dma_fence->is_internal &&
			IS_ERR_OR_NULL(hw_dma_fence->signal_cb.drv_data))) {
		HWFNC_ERR("invalid hwfence data %pK %pK, won't release hw_fence!\n",
			hw_dma_fence->client_handle, hw_dma_fence->signal_cb.drv_data);
		return;
	}

	/* release hw-fence */
	if (hw_dma_fence->is_internal) /* internally owned hw_dma_fence has its own refcount */
		ret = hw_fence_destroy_refcount(hw_dma_fence->signal_cb.drv_data,
			hw_dma_fence->signal_cb.hash, HW_FENCE_DMA_FENCE_REFCOUNT);
	else /* externally owned hw_dma_fence uses standard hlos refcount */
		ret = msm_hw_fence_destroy(hw_dma_fence->client_handle, &hw_dma_fence->base);

	if (ret)
		HWFNC_ERR("failed to release hw_fence!\n");
}

static void hw_fence_dbg_release(struct dma_fence *fence)
{
	struct hw_dma_fence *hw_dma_fence;

	if (!fence)
		return;

	HWFNC_DBG_H("release backing fence %pK\n", fence);
	hw_dma_fence = to_hw_dma_fence(fence);

	if (test_bit(MSM_HW_FENCE_FLAG_ENABLED_BIT, &fence->flags))
		_hw_fence_release(hw_dma_fence);

	kfree(fence->lock);
	kfree(hw_dma_fence);
}

struct dma_fence_ops hw_fence_dbg_ops = {
	.get_driver_name = hw_fence_dbg_get_driver_name,
	.get_timeline_name = hw_fence_dbg_get_timeline_name,
	.enable_signaling = hw_fence_dbg_enable_signaling,
	.wait = dma_fence_default_wait,
	.release = hw_fence_dbg_release,
};

struct dma_fence *hw_dma_fence_init(struct msm_hw_fence_client *hw_fence_client, u64 context,
	u64 seqno)
{
	struct hw_dma_fence *fence;
	spinlock_t *fence_lock;

	/* create dma fence */
	fence_lock = kzalloc(sizeof(*fence_lock), GFP_ATOMIC);
	if (!fence_lock)
		return ERR_PTR(-ENOMEM);

	fence = kzalloc(sizeof(*fence), GFP_ATOMIC);
	if (!fence) {
		kfree(fence_lock);
		return ERR_PTR(-ENOMEM);
	}

	snprintf(fence->name, HW_FENCE_NAME_SIZE, "hwfence:id:%d:ctx=%llu:seqno:%llu",
		hw_fence_client->client_id, context, seqno);
	spin_lock_init(fence_lock);

	HWFNC_DBG_L("creating dma_fence for client:%d ctx:%llu seqno:%llu\n",
		hw_fence_client->client_id, context, seqno);

	dma_fence_init(&fence->base, &hw_fence_dbg_ops, fence_lock, context, seqno);
	fence->client_handle = hw_fence_client;

	return (struct dma_fence *)fence;
}

bool dma_fence_is_hw_dma(struct dma_fence *fence)
{
	return fence->ops == &hw_fence_dbg_ops;
}

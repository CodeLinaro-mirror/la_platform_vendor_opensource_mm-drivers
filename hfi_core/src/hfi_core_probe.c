// SPDX-License-Identifier: GPL-2.0-only
/*
 * ​​​​Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.​
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include "hfi_core_probe.h"
#include "hfi_core.h"
#include "hfi_core_debug.h"

static int msm_hfi_core_probe_init(struct platform_device *pdev)
{
	int rc;
	struct hfi_core_drv_data *drv_data;

	HFI_CORE_DBG_H("+\n");

	drv_data = kzalloc(sizeof(*drv_data), GFP_KERNEL);
	if (!drv_data) {
		HFI_CORE_ERR("%s: drv data allocation failed\n", __func__);
		return -ENOMEM;
	}
	drv_data->client_data[HFI_CORE_CLIENT_ID_0].ipc_info.type =
		HFI_IPC_TYPE_MBOX;
	dev_set_drvdata(&pdev->dev, drv_data);
	drv_data->dev = (void *)(&pdev->dev);

	/* initialize hfi core driver resources */
	rc = hfi_core_init(drv_data);
	if (rc)
		goto error;

	HFI_CORE_DBG_H("%s: probe success\n", __func__);
	HFI_CORE_DBG_H("-\n");
	return rc;

error:
	dev_set_drvdata(&pdev->dev, NULL);
	kfree(drv_data);
	drv_data = (void *) -EPROBE_DEFER;

	HFI_CORE_DBG_INFO("error %d\n", rc);

	return rc;
}

static int msm_hfi_core_probe(struct platform_device *pdev)
{
	int rc = 0;

	HFI_CORE_DBG_H("+\n");

	if (!pdev) {
		HFI_CORE_ERR("%s: null platform dev\n", __func__);
		return -EINVAL;
	}

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,msm-hfi-core"))
		rc = msm_hfi_core_probe_init(pdev);
	if (rc)
		goto err_exit;

	HFI_CORE_DBG_H("-\n");
	return 0;

err_exit:
	if (rc == -EPROBE_DEFER) {
		HFI_CORE_DBG_INFO("probe defer for hfi core\n");
	} else {
		HFI_CORE_ERR_ONCE("error %d\n", rc);
	}
	return rc;
}

static int msm_hfi_core_remove(struct platform_device *pdev)
{
	int rc = 0;
	struct hfi_core_drv_data *drv_data;

	HFI_CORE_DBG_H("+\n");

	if (!pdev) {
		HFI_CORE_ERR("%s: null platform dev\n", __func__);
		return -EINVAL;
	}

	drv_data = dev_get_drvdata(&pdev->dev);
	if (!drv_data) {
		HFI_CORE_ERR("%s: null driver data\n", __func__);
		return -EINVAL;
	}

	rc = hfi_core_deinit(drv_data);
	if (rc) {
		HFI_CORE_ERR("%s: failed to deinit hfi core driver data\n", __func__);
		return -EINVAL;
	}

	dev_set_drvdata(&pdev->dev, NULL);
	kfree(drv_data);
	drv_data = (void *) -EPROBE_DEFER;

	HFI_CORE_DBG_H("-\n");
	return 0;
}

static const struct of_device_id msm_hfi_core_dt_match[] = {
	{.compatible = "qcom,msm-hfi-core"},
	{}
};

static struct platform_driver msm_hfi_core_driver = {
	.probe = msm_hfi_core_probe,
	.remove = msm_hfi_core_remove,
	.driver = {
		.name = "msm-hfi-core",
		.of_match_table = of_match_ptr(msm_hfi_core_dt_match),
	},
};

static int __init msm_hfi_core_init(void)
{
	int rc = 0;

	HFI_CORE_DBG_H("+\n");

	rc = platform_driver_register(&msm_hfi_core_driver);
	if (rc) {
		HFI_CORE_ERR("%s: failed to register platform driver\n",
			__func__);
		return rc;
	}

	HFI_CORE_DBG_H("-\n");

	return 0;
}

static void __exit msm_hfi_core_exit(void)
{
	HFI_CORE_DBG_H("+\n");

	platform_driver_unregister(&msm_hfi_core_driver);

	HFI_CORE_DBG_H("-\n");
}

module_init(msm_hfi_core_init);
module_exit(msm_hfi_core_exit);

MODULE_DESCRIPTION("QTI HFI Core Driver");
MODULE_LICENSE("GPL v2");
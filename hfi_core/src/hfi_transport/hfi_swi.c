// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/iommu.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include "hfi_core_debug.h"
#include "hfi_interface.h"
#include "hfi_core.h"
#include "hfi_smmu.h"
#include "hfi_swi.h"

#define REG_WRITE(_val, _addr)                      writel_relaxed(_val, _addr)
#define REG_READ(_addr)                                    readl_relaxed(_addr)

#define DISP_RSC_POWER_UP_STATUS                                           0x2d
#define HW_STATUS_POLL_INTERVAL_US                                           50
#define RSCC_CLOCK_ON_DELAY_US                                             1000
#define CLK_RESET_PULSE_DELAY_US                                           1000
#define MAX_WAIT_ITERATIONS                                                 200
#define DCP_P_S_G_REG_6_OFFSET                                             0x18

#define HFI_DEV_SWI_CTRL_INIT(base)                                (base + 0x8)
#define HFI_DEV_SWI_RES_TBL_INFO(base)                             (base + 0xc)
#define HFI_DEV_SWI_RES_TBL_ADDR_H(base)                          (base + 0x10)
#define HFI_DEV_SWI_RES_TBL_ADDR_L(base)                          (base + 0x14)

#define HFI_DEV_CTRL_POWER_OFF(val)                          ((val & 0x1) << 1)
#define HFI_RES_TBL_HOSTID(val)                            ((val & 0xFF) << 24)
#define HFI_TBL_STATUS(val)                                      ((val & 0xFF))

/* Display collapse register access macros */
#define SDE_RSCC_RSC_STATUS(base)                                        (base)
#define DCP_P_S_G_REG_6(base)                   (base + DCP_P_S_G_REG_6_OFFSET)
#define DCP_RVCP_RVSSCP_STATUS(base)                                     (base)
#define DISP_CC_DCP_PROC_H_CBCR(base)                                    (base)

/* Display collapse register bit field macros */
#define DCP_PROC_H_CBCR_CLK_ARES_SET(val)                    ((val) | (1 << 2))
#define DCP_PROC_H_CBCR_CLK_ARES_CLR(val)                   ((val) & ~(1 << 2))
#define DCP_RVCP_STATUS_BIT(val)                           (((val) >> 3) & 0x1)

#define SDE_RSCC_WRAPPER_OVERRIDE_CTRL(base)                       (base + 0x4)
#define PWR_PU_ACK_BIT_CHECK(val)                            ((val) & (1 << 5))

int swi_handle_disp_collapse(struct hfi_core_drv_data *drv_data,
	struct client_data *client)
{
	void __iomem *dcp_p_s_g = NULL;
	void __iomem *dcp_rvcp_rvsscp_status = NULL;
	void __iomem *disp_cc_dcp_proc_h_cbcr = NULL;
	void __iomem *sde_rscc_rsc = NULL;
	void __iomem *sde_rscc_wrapper = NULL;
	int timeout;
	int val;
	u32 reg_val;

	HFI_CORE_DBG_H("+\n");

	if (!drv_data || !client) {
		HFI_CORE_ERR("invalid params\n");
		return -EINVAL;
	}

	if (!atomic_read(&drv_data->is_disp_collapsed))
		return 0;

	HFI_CORE_DBG_H("display collapsed, running dcp reset sequence\n");

	if (!client->sde_rscc_rsc_info.io_mem ||
			!client->swi_page0_info.io_mem ||
			!client->dcp_rvcp_rvsscp_status_info.io_mem ||
			!client->disp_cc_dcp_proc_h_cbcr_info.io_mem ||
			!client->sde_rscc_wrapper_info.io_mem) {
		HFI_CORE_ERR("Invalid iomem: %d %d %d %d %d\n",
			!client->sde_rscc_rsc_info.io_mem,
			!client->swi_page0_info.io_mem,
			!client->dcp_rvcp_rvsscp_status_info.io_mem,
			!client->disp_cc_dcp_proc_h_cbcr_info.io_mem,
			!client->sde_rscc_wrapper_info.io_mem);

		return -EINVAL;
	}

	sde_rscc_rsc = client->sde_rscc_rsc_info.io_mem;
	dcp_p_s_g = client->swi_page0_info.io_mem;
	dcp_rvcp_rvsscp_status = client->dcp_rvcp_rvsscp_status_info.io_mem;
	disp_cc_dcp_proc_h_cbcr = client->disp_cc_dcp_proc_h_cbcr_info.io_mem;
	sde_rscc_wrapper = client->sde_rscc_wrapper_info.io_mem;

	usleep_range(RSCC_CLOCK_ON_DELAY_US, RSCC_CLOCK_ON_DELAY_US + 5);
	timeout = MAX_WAIT_ITERATIONS;

	while (!PWR_PU_ACK_BIT_CHECK(REG_READ(SDE_RSCC_WRAPPER_OVERRIDE_CTRL(sde_rscc_wrapper)))) {
		usleep_range(HW_STATUS_POLL_INTERVAL_US, HW_STATUS_POLL_INTERVAL_US + 5);
		if (--timeout <= 0) {
			HFI_CORE_ERR("Timeout waiting for sde_rscc_rsc\n");
			HFI_CORE_ERR("WRAPPER_OVERRIDE_CTRL = 0x%x, RSC_STATUS = 0x%x",
				REG_READ(SDE_RSCC_WRAPPER_OVERRIDE_CTRL(sde_rscc_wrapper)),
				REG_READ(SDE_RSCC_RSC_STATUS(sde_rscc_rsc)));
			return -EINVAL;
		}
	}

	val = REG_READ(DCP_P_S_G_REG_6(dcp_p_s_g));
	if (val) {
		timeout = MAX_WAIT_ITERATIONS;
		while (!DCP_RVCP_STATUS_BIT(
			REG_READ(DCP_RVCP_RVSSCP_STATUS(dcp_rvcp_rvsscp_status)))) {
			usleep_range(HW_STATUS_POLL_INTERVAL_US, HW_STATUS_POLL_INTERVAL_US + 5);
			if (--timeout <= 0) {
				HFI_CORE_ERR("Timeout waiting for rvcp status\n");
				return -EINVAL;
			}
		}

		reg_val = REG_READ(DISP_CC_DCP_PROC_H_CBCR(disp_cc_dcp_proc_h_cbcr));
		reg_val = DCP_PROC_H_CBCR_CLK_ARES_SET(reg_val);
		REG_WRITE(reg_val, DISP_CC_DCP_PROC_H_CBCR(disp_cc_dcp_proc_h_cbcr));
		usleep_range(CLK_RESET_PULSE_DELAY_US, CLK_RESET_PULSE_DELAY_US + 5);
		reg_val = DCP_PROC_H_CBCR_CLK_ARES_CLR(reg_val);
		REG_WRITE(reg_val, DISP_CC_DCP_PROC_H_CBCR(disp_cc_dcp_proc_h_cbcr));
	}
	atomic_set(&drv_data->is_disp_collapsed, 0);

	HFI_CORE_DBG_H("-\n");
	return 0;
}

static int map_swi_register(struct hfi_core_drv_data *drv_data, u32 client_id,
	const char *reg_name, struct hfi_core_swi_info *swi_info)
{
	int ret = 0;
	void *__iomem ptr;
	struct device *dev = (struct device *)drv_data->dev;
	struct platform_device *pdev = NULL;
	struct resource *res;
	unsigned long res_size;

	HFI_CORE_DBG_H("+\n");

	if (client_id != HFI_CORE_CLIENT_ID_0 && client_id != HFI_CORE_CLIENT_ID_1) {
		HFI_CORE_ERR("client id: %u is not supported\n", client_id);
		return -EINVAL;
	}

	if (!reg_name || !swi_info) {
		HFI_CORE_ERR("invalid parameters\n");
		return -EINVAL;
	}

	pdev = to_platform_device(dev);
	if (!pdev) {
		HFI_CORE_ERR("invalid platform device for client: %u\n", client_id);
		return -EINVAL;
	}
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, reg_name);
	if (!res) {
		HFI_CORE_DBG_INFO("swi resource: %s is unavailable\n. skip mapping", reg_name);
		return 0;
	}
	res_size = resource_size(res);

	swi_info->reg_base = res->start;
	swi_info->size = res_size;

	ptr = memremap(swi_info->reg_base, swi_info->size, MEMREMAP_WB);
	if (!ptr) {
		HFI_CORE_ERR("failed to ioremap swi reg: %s at 0x%llx size 0x%lx\n",
			reg_name, swi_info->reg_base, res_size);
		return -ENOMEM;
	}
	swi_info->io_mem = ptr;

	HFI_CORE_DBG_H("-\n");
	return ret;
}

static void unmap_swi_register(struct hfi_core_swi_info *swi_info, const char *reg_name)
{
	HFI_CORE_DBG_H("+\n");

	if (!swi_info || !swi_info->io_mem) {
		HFI_CORE_ERR("swi regs io mem addr not available for %s\n",
			reg_name ? reg_name : "unknown");
		return;
	}

	memunmap(swi_info->io_mem);

	HFI_CORE_DBG_H("-\n");
}

int init_swi(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;
	enum hfi_core_client_id client;
	char *dt_string = NULL;

	HFI_CORE_DBG_H("+\n");

	client = drv_data->drv_client_id;

	if (client >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid client id: %u\n", client);
		return -EINVAL;
	}

	if (!drv_data || !drv_data->dev) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	if (client == HFI_CORE_CLIENT_ID_1)
		dt_string = "swi_dev1";
	else
		dt_string = "swi_dev0";

	ret = map_swi_register(drv_data, client, dt_string,
		&drv_data->client_data[client].swi_info);
	if (ret) {
		HFI_CORE_ERR("failed to map swi_dev0 regs\n");
		goto exit;
	}

	ret = map_swi_register(drv_data, client, "swi_page0",
		&drv_data->client_data[client].swi_page0_info);
	if (ret) {
		HFI_CORE_ERR("failed to map swi_page0 regs\n");
		goto exit;
	}

	ret = map_swi_register(drv_data, client, "sde_rscc_rsc",
		&drv_data->client_data[client].sde_rscc_rsc_info);
	if (ret) {
		HFI_CORE_ERR("failed to map sde_rscc_rsc regs\n");
		goto exit;
	}

	ret = map_swi_register(drv_data, client, "dcp_rvcp_rvsscp_status",
		&drv_data->client_data[client].dcp_rvcp_rvsscp_status_info);
	if (ret) {
		HFI_CORE_ERR("failed to map dcp_rvcp_rvsscp_status regs\n");
		goto exit;
	}

	ret = map_swi_register(drv_data, client, "disp_cc_dcp_proc_h_cbcr",
		&drv_data->client_data[client].disp_cc_dcp_proc_h_cbcr_info);
	if (ret) {
		HFI_CORE_ERR("failed to map disp_cc_dcp_proc_h_cbcr regs\n");
		goto exit;
	}

	ret = map_swi_register(drv_data, client, "sde_rscc_wrapper",
		&drv_data->client_data[client].sde_rscc_wrapper_info);
	if (ret) {
		HFI_CORE_ERR("failed to map sde_rscc_wrapper regs\n");
		goto exit;
	}

exit:
	HFI_CORE_DBG_H("-\n");
	return ret;
}

int deinit_swi(struct hfi_core_drv_data *drv_data)
{
	int ret = 0;
	enum hfi_core_client_id client;

	HFI_CORE_DBG_H("+\n");

	client = drv_data->drv_client_id;

	if (client >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid client id: %u\n", client);
		return -EINVAL;
	}

	if (!drv_data || !drv_data->dev) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	unmap_swi_register(&drv_data->client_data[client].swi_info, "swi_dev0");
	unmap_swi_register(&drv_data->client_data[client].swi_page0_info, "swi_page0");
	unmap_swi_register(&drv_data->client_data[client].sde_rscc_rsc_info, "sde_rscc_rsc");
	unmap_swi_register(&drv_data->client_data[client].dcp_rvcp_rvsscp_status_info,
		"dcp_rvcp_rvsscp_status");
	unmap_swi_register(&drv_data->client_data[client].disp_cc_dcp_proc_h_cbcr_info,
		"disp_cc_dcp_proc_h_cbcr");
	unmap_swi_register(&drv_data->client_data[client].sde_rscc_wrapper_info,
		"sde_rscc_wrapper");

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int swi_setup_resources(u32 client_id, struct hfi_core_drv_data *drv_data)
{
	int ret = 0;
	struct hfi_core_resource_info *res_info = NULL;
	u32 reg_val, val_to_write;
	void __iomem *reg_io_mem_base;

	HFI_CORE_DBG_H("+\n");

	if (client_id >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid client id: %u\n", client_id);
		return -EINVAL;
	}

	if (!drv_data) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	if (!drv_data->client_data[client_id].swi_info.io_mem) {
		HFI_CORE_DBG_INFO("swi io mem unavailable. skipping configuration\n");
		return 0;
	}

	res_info = (struct hfi_core_resource_info *)
		&drv_data->client_data[client_id].resource_info;
	if (!res_info->resource_table_iova) {
		HFI_CORE_ERR("client: %u res tbl addr [%x] is invalid\n", client_id,
			(u32)res_info->resource_table_iova);
		return -EINVAL;
	}
	reg_io_mem_base = drv_data->client_data[client_id].swi_info.io_mem;

	/* configure resource table info register */
	reg_val = REG_READ(HFI_DEV_SWI_RES_TBL_INFO(reg_io_mem_base));
	val_to_write = reg_val | HFI_RES_TBL_HOSTID(0x1) | HFI_TBL_STATUS(0x1);
	REG_WRITE(val_to_write,  HFI_DEV_SWI_RES_TBL_INFO(reg_io_mem_base));

	/* configure resource table addr high register */
	REG_WRITE(0x0,  HFI_DEV_SWI_RES_TBL_ADDR_H(reg_io_mem_base));

	/* configure resource table addr low register */
	REG_WRITE(res_info->resource_table_iova,  HFI_DEV_SWI_RES_TBL_ADDR_L(reg_io_mem_base));

	HFI_CORE_DBG_L("configured client[%u] swi reg phy[%llx] with dcp map addr[%x]\n",
		client_id,
		drv_data->client_data[client_id].swi_info.reg_base,
		(u32)res_info->resource_table_iova);

	HFI_CORE_DBG_H("-\n");
	return ret;
}

int swi_reg_power_off(u32 client_id, struct hfi_core_drv_data *drv_data)
{
	int ret = 0;
	struct hfi_core_resource_info *res_info = NULL;
	u32 reg_val, val_to_write;
	void __iomem *reg_io_mem_base;

	HFI_CORE_DBG_H("+\n");

	if (client_id >= HFI_CORE_CLIENT_ID_MAX) {
		HFI_CORE_ERR("invalid client id: %u\n", client_id);
		return -EINVAL;
	}

	if (!drv_data) {
		HFI_CORE_ERR("invalid params drv_data\n");
		return -EINVAL;
	}

	if (!drv_data->client_data[client_id].swi_info.io_mem) {
		HFI_CORE_DBG_INFO("swi io mem unavailable. skipping reg power off\n");
		return 0;
	}

	res_info = (struct hfi_core_resource_info *)
		&drv_data->client_data[client_id].resource_info;
	if (!res_info->resource_table_iova) {
		HFI_CORE_ERR("client: %u res tbl addr [%x] is invalid\n", client_id,
			(u32)res_info->resource_table_iova);
		return -EINVAL;
	}
	reg_io_mem_base = drv_data->client_data[client_id].swi_info.io_mem;

	/* configure swi ctrl init register power off bit to 1 */
	reg_val = REG_READ(HFI_DEV_SWI_CTRL_INIT(reg_io_mem_base));
	val_to_write = reg_val | HFI_DEV_CTRL_POWER_OFF(1);
	REG_WRITE(val_to_write, HFI_DEV_SWI_CTRL_INIT(reg_io_mem_base));

	HFI_CORE_DBG_H("-\n");
	return ret;
}

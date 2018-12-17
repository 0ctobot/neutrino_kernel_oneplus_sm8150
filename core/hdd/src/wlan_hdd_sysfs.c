/*
 * Copyright (c) 2017-2018 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 *  DOC: wlan_hdd_sysfs.c
 *
 *  WLAN Host Device Driver implementation
 *
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/fs.h>
#include <linux/string.h>
#include "wlan_hdd_includes.h"
#include "wlan_hdd_sysfs.h"
#include "qwlan_version.h"
#include "cds_api.h"
#include <wlan_osif_request_manager.h>
#include <qdf_mem.h>
#include <sir_api.h>

#define MAX_PSOC_ID_SIZE 10

#ifdef MULTI_IF_NAME
#define DRIVER_NAME MULTI_IF_NAME
#else
#define DRIVER_NAME "wlan"
#endif

static struct kobject *wlan_kobject;
static struct kobject *driver_kobject;
static struct kobject *fw_kobject;
static struct kobject *psoc_kobject;

static ssize_t __show_driver_version(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     char *buf)
{
	return scnprintf(buf, PAGE_SIZE, QWLAN_VERSIONSTR);
}

static ssize_t show_driver_version(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf)
{
	ssize_t ret_val;

	cds_ssr_protect(__func__);
	ret_val = __show_driver_version(kobj, attr, buf);
	cds_ssr_unprotect(__func__);

	return ret_val;
}

static ssize_t __show_fw_version(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	uint32_t major_spid = 0, minor_spid = 0, siid = 0, crmid = 0;
	uint32_t sub_id = 0;
	struct hdd_context *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	int ret;

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret) {
		hdd_err("hdd ctx is invalid");
		return ret;
	}

	hdd_debug("Rcvd req for FW version");
	hdd_get_fw_version(hdd_ctx, &major_spid, &minor_spid, &siid,
			   &crmid);
	sub_id = (hdd_ctx->target_fw_vers_ext & 0xf0000000) >> 28;

	return scnprintf(buf, PAGE_SIZE,
			 "FW:%d.%d.%d.%d.%d HW:%s Board version: %x Ref design id: %x Customer id: %x Project id: %x Board Data Rev: %x\n",
			 major_spid, minor_spid, siid, crmid, sub_id,
			 hdd_ctx->target_hw_name,
			 hdd_ctx->hw_bd_info.bdf_version,
			 hdd_ctx->hw_bd_info.ref_design_id,
			 hdd_ctx->hw_bd_info.customer_id,
			 hdd_ctx->hw_bd_info.project_id,
			 hdd_ctx->hw_bd_info.board_data_rev);
}

static ssize_t show_fw_version(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       char *buf)
{
	ssize_t ret_val;

	cds_ssr_protect(__func__);
	ret_val = __show_fw_version(kobj, attr, buf);
	cds_ssr_unprotect(__func__);

	return ret_val;
};

struct power_stats_priv {
	struct power_stats_response power_stats;
};

static void hdd_power_debugstats_dealloc(void *priv)
{
	struct power_stats_priv *stats = priv;

	qdf_mem_free(stats->power_stats.debug_registers);
	stats->power_stats.debug_registers = NULL;
}

static void hdd_power_debugstats_cb(struct power_stats_response *response,
				    void *context)
{
	struct osif_request *request;
	struct power_stats_priv *priv;
	uint32_t *debug_registers;
	uint32_t debug_registers_len;

	hdd_enter();

	request = osif_request_get(context);
	if (!request) {
		hdd_err("Obsolete request");
		return;
	}

	priv = osif_request_priv(request);

	/* copy fixed-sized data */
	priv->power_stats = *response;

	/* copy variable-size data */
	if (response->num_debug_register) {
		debug_registers_len = (sizeof(response->debug_registers[0]) *
				       response->num_debug_register);
		debug_registers = qdf_mem_malloc(debug_registers_len);
		priv->power_stats.debug_registers = debug_registers;
		if (debug_registers) {
			qdf_mem_copy(debug_registers,
				     response->debug_registers,
				     debug_registers_len);
		} else {
			hdd_err("Power stats memory alloc fails!");
			priv->power_stats.num_debug_register = 0;
		}
	}
	osif_request_complete(request);
	osif_request_put(request);
	hdd_exit();
}

static ssize_t __show_device_power_stats(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	struct hdd_context *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	QDF_STATUS status;
	struct power_stats_response *chip_power_stats;
	ssize_t ret_cnt = 0;
	int j;
	void *cookie;
	struct osif_request *request;
	struct power_stats_priv *priv;
	static const struct osif_request_params params = {
		.priv_size = sizeof(*priv),
		.timeout_ms = WLAN_WAIT_TIME_STATS,
		.dealloc = hdd_power_debugstats_dealloc,
	};

	hdd_enter();

	ret_cnt = wlan_hdd_validate_context(hdd_ctx);
	if (ret_cnt)
		return ret_cnt;

	request = osif_request_alloc(&params);
	if (!request) {
		hdd_err("Request allocation failure");
		return -ENOMEM;
	}
	cookie = osif_request_cookie(request);

	status = sme_power_debug_stats_req(hdd_ctx->mac_handle,
					   hdd_power_debugstats_cb,
					   cookie);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("chip power stats request failed");
		ret_cnt = -EINVAL;
		goto cleanup;
	}

	ret_cnt = osif_request_wait_for_response(request);
	if (ret_cnt) {
		hdd_err("Target response timed out Power stats");
		ret_cnt = -ETIMEDOUT;
		goto cleanup;
	}
	priv = osif_request_priv(request);
	chip_power_stats = &priv->power_stats;

	ret_cnt += scnprintf(buf, PAGE_SIZE,
			"POWER DEBUG STATS\n=================\n"
			"cumulative_sleep_time_ms: %d\n"
			"cumulative_total_on_time_ms: %d\n"
			"deep_sleep_enter_counter: %d\n"
			"last_deep_sleep_enter_tstamp_ms: %d\n"
			"debug_register_fmt: %d\n"
			"num_debug_register: %d\n",
			chip_power_stats->cumulative_sleep_time_ms,
			chip_power_stats->cumulative_total_on_time_ms,
			chip_power_stats->deep_sleep_enter_counter,
			chip_power_stats->last_deep_sleep_enter_tstamp_ms,
			chip_power_stats->debug_register_fmt,
			chip_power_stats->num_debug_register);

	for (j = 0; j < chip_power_stats->num_debug_register; j++) {
		if ((PAGE_SIZE - ret_cnt) > 0)
			ret_cnt += scnprintf(buf + ret_cnt,
					PAGE_SIZE - ret_cnt,
					"debug_registers[%d]: 0x%x\n", j,
					chip_power_stats->debug_registers[j]);
		else
			j = chip_power_stats->num_debug_register;
	}

cleanup:
	osif_request_put(request);
	hdd_exit();
	return ret_cnt;
}

static ssize_t show_device_power_stats(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buf)
{
	ssize_t ret_val;

	cds_ssr_protect(__func__);
	ret_val = __show_device_power_stats(kobj, attr, buf);
	cds_ssr_unprotect(__func__);

	return ret_val;
}

static struct kobj_attribute dr_ver_attribute =
	__ATTR(driver_version, 0440, show_driver_version, NULL);
static struct kobj_attribute fw_ver_attribute =
	__ATTR(version, 0440, show_fw_version, NULL);
static struct kobj_attribute power_stats_attribute =
	__ATTR(power_stats, 0440, show_device_power_stats, NULL);

void hdd_sysfs_create_version_interface(struct wlan_objmgr_psoc *psoc)
{
	int error = 0;
	uint32_t psoc_id;
	char buf[MAX_PSOC_ID_SIZE];

	if (!driver_kobject || !wlan_kobject) {
		hdd_err("could not get driver kobject!");
		return;
	}

	error = sysfs_create_file(wlan_kobject, &dr_ver_attribute.attr);
	if (error) {
		hdd_err("could not create wlan sysfs file");
		return;
	}

	fw_kobject = kobject_create_and_add("fw", driver_kobject);
	if (!fw_kobject) {
		hdd_err("could not allocate fw kobject");
		goto free_fw_kobj;
	}

	psoc_id = wlan_psoc_get_nif_phy_version(psoc);
	scnprintf(buf, PAGE_SIZE, "%d", psoc_id);

	psoc_kobject = kobject_create_and_add(buf, fw_kobject);
	if (!psoc_kobject) {
		hdd_err("could not allocate psoc kobject");
		goto free_fw_kobj;
	}

	error = sysfs_create_file(psoc_kobject, &fw_ver_attribute.attr);
	if (error) {
		hdd_err("could not create fw sysfs file");
		goto free_psoc_kobj;
	}

	return;

free_psoc_kobj:
	kobject_put(psoc_kobject);
	psoc_kobject = NULL;

free_fw_kobj:
	kobject_put(fw_kobject);
	fw_kobject = NULL;
}

void hdd_sysfs_destroy_version_interface(void)
{
	if (psoc_kobject) {
		kobject_put(psoc_kobject);
		psoc_kobject = NULL;
		kobject_put(fw_kobject);
		fw_kobject = NULL;
	}
}

void hdd_sysfs_create_powerstats_interface(void)
{
	int error;

	if (!driver_kobject) {
		hdd_err("could not get driver kobject!");
		return;
	}

	error = sysfs_create_file(driver_kobject, &power_stats_attribute.attr);
	if (error)
		hdd_err("could not create power_stats sysfs file");
}

void hdd_sysfs_destroy_powerstats_interface(void)
{
	if (!driver_kobject) {
		hdd_err("could not get driver kobject!");
		return;
	}
	sysfs_remove_file(driver_kobject, &power_stats_attribute.attr);
}

void hdd_sysfs_create_driver_root_obj(void)
{
	driver_kobject = kobject_create_and_add(DRIVER_NAME, kernel_kobj);
	if (!driver_kobject) {
		hdd_err("could not allocate driver kobject");
		return;
	}

	wlan_kobject = kobject_create_and_add("wlan", driver_kobject);
	if (!wlan_kobject) {
		hdd_err("could not allocate wlan kobject");
		kobject_put(driver_kobject);
		driver_kobject = NULL;
	}
}

void hdd_sysfs_destroy_driver_root_obj(void)
{
	if (wlan_kobject) {
		kobject_put(wlan_kobject);
		wlan_kobject = NULL;
	}

	if (driver_kobject) {
		kobject_put(driver_kobject);
		driver_kobject = NULL;
		kobject_put(wlan_kobject);
		wlan_kobject = NULL;
	}
}

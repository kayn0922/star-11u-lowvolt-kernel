// SPDX-License-Identifier: GPL-2.0-only
/*
 * star_volt_soc.c - rebuildable "battery voltage -> displayed percentage"
 *                   mapping for the Xiaomi 11 Ultra (star) low-voltage kernel.
 *
 * The fuel gauge on sm8350 runs on the ADSP/PMIC GLINK remote subsystem: its
 * OCV table is in firmware and in the vendor dtb (neither of which is rebuilt
 * by a kernel-only tree).  The stock gauge reports 0% at ~3.4 V and the
 * kernel driver then calls kernel_power_off().  Android's BatteryService also
 * shuts the phone down whenever it sees capacity == 0.
 *
 * This file puts a rewritable voltage->SOC table in front of the power supply
 * property that Android actually reads, and provides one tunable shutdown
 * voltage used by the emergency power-off check.
 *
 * Runtime interface (built-in parameters):
 *   /sys/module/star_volt_soc/parameters/table          (RW)
 *   /sys/module/star_volt_soc/parameters/shutdown_mv    (RW)
 *   /sys/module/star_volt_soc/parameters/enable         (RW)
 *   /sys/module/star_volt_soc/parameters/apply_below_mv (RW)
 *   /sys/module/star_volt_soc/parameters/min_soc        (RW)
 *   /sys/module/star_volt_soc/parameters/debug          (RW)
 *   /sys/module/star_volt_soc/parameters/status         (RO)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "star_volt_soc.h"

#define VSOC_MAX_POINTS		32
#define VSOC_STR_LEN		1024
#define VSOC_STATUS_LEN		256

struct vsoc_point {
	int mv;
	int soc;
};

static char vsoc_table[VSOC_STR_LEN] = STAR_VOLT_SOC_DEFAULT_TABLE;
module_param_string(table, vsoc_table, VSOC_STR_LEN, 0644);
MODULE_PARM_DESC(table,
	"Voltage->SOC table: mV:percent,mV:percent,... strictly ascending mV");

static char vsoc_default_table[VSOC_STR_LEN] = STAR_VOLT_SOC_DEFAULT_TABLE;

static int vsoc_enable = 1;
module_param_named(enable, vsoc_enable, int, 0644);
MODULE_PARM_DESC(enable, "1 = apply the table (default), 0 = pass raw FG SOC");

static int vsoc_shutdown_mv = STAR_VOLT_SOC_SHUTDOWN_MV;
module_param_named(shutdown_mv, vsoc_shutdown_mv, int, 0644);
MODULE_PARM_DESC(shutdown_mv,
	"Emergency power-off voltage in mV (default 2980). Effective immediately");

/* 0 = rewrite the whole curve; e.g. 3600 = only rewrite below 3.6 V. */
static int vsoc_apply_below_mv;
module_param_named(apply_below_mv, vsoc_apply_below_mv, int, 0644);
MODULE_PARM_DESC(apply_below_mv,
	"Only apply the table below this voltage in mV, 0 = whole range");

/* Android shuts down at 0%, so never expose 0% above the cutoff. */
static int vsoc_min_soc = 1;
module_param_named(min_soc, vsoc_min_soc, int, 0644);
MODULE_PARM_DESC(min_soc,
	"Minimum reported SOC while voltage is above shutdown_mv (default 1)");

static int vsoc_debug;
module_param_named(debug, vsoc_debug, int, 0644);
MODULE_PARM_DESC(debug, "1 = rate-limited debug log for every remap");

static int vsoc_last_raw = -1;
static int vsoc_last_volt_uv;
static int vsoc_last_status = -1;
static int vsoc_last_mapped = -1;
static DEFINE_SPINLOCK(vsoc_lock);

static int vsoc_atoi(const char **pp)
{
	const char *p = *pp;
	int val = 0;

	while (*p >= '0' && *p <= '9') {
		val = val * 10 + (*p - '0');
		p++;
	}
	*pp = p;
	return val;
}

/*
 * Accepts "2980:0,3400:10,4200:100" (also '=' or whitespace as separators).
 * Returns the number of points, or 0 when the table is unusable.  Callers
 * then fall back to the compiled-in default table.
 */
static int vsoc_parse(const char *src, struct vsoc_point *pts, int max)
{
	const char *p = src;
	int n = 0;

	while (*p && n < max) {
		int mv, soc;

		while (*p && (*p < '0' || *p > '9'))
			p++;
		if (!*p)
			break;
		mv = vsoc_atoi(&p);
		while (*p && (*p < '0' || *p > '9'))
			p++;
		if (!*p)
			break;
		soc = vsoc_atoi(&p);

		if (mv < 2000 || mv > 5000 || soc < 0 || soc > 100)
			return 0;
		if (n && mv <= pts[n - 1].mv)
			return 0;
		if (n && soc < pts[n - 1].soc)
			return 0;

		pts[n].mv = mv;
		pts[n].soc = soc;
		n++;
	}

	return n >= 2 ? n : 0;
}

static int vsoc_interpolate(const struct vsoc_point *pts, int n, int mv)
{
	int i;

	if (mv <= pts[0].mv)
		return pts[0].soc;
	if (mv >= pts[n - 1].mv)
		return pts[n - 1].soc;

	for (i = 1; i < n; i++) {
		if (mv <= pts[i].mv) {
			int span = pts[i].mv - pts[i - 1].mv;
			int dsoc = pts[i].soc - pts[i - 1].soc;

			if (!span)
				return pts[i].soc;
			return pts[i - 1].soc +
				dsoc * (mv - pts[i - 1].mv) / span;
		}
	}

	return pts[n - 1].soc;
}

static int vsoc_status_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int raw, volt, status, mapped;

	spin_lock_irqsave(&vsoc_lock, flags);
	raw = vsoc_last_raw;
	volt = vsoc_last_volt_uv;
	status = vsoc_last_status;
	mapped = vsoc_last_mapped;
	spin_unlock_irqrestore(&vsoc_lock, flags);

	return scnprintf(buf, VSOC_STATUS_LEN,
		"raw_soc=%d volt_uv=%d mapped_soc=%d status=%d "
		"enable=%d shutdown_mv=%d table=%s\n",
		raw, volt, mapped, status, vsoc_enable,
		star_volt_soc_shutdown_mv(), vsoc_table);
}

static int vsoc_status_set(const char *val, const struct kernel_param *kp)
{
	return -EPERM;
}

static const struct kernel_param_ops vsoc_status_ops = {
	.set = vsoc_status_set,
	.get = vsoc_status_get,
};
module_param_cb(status, &vsoc_status_ops, NULL, 0444);

/**
 * star_volt_soc_apply - remap the SOC reported by the fuel gauge
 * @raw_soc: SOC (0..100) computed by the original driver
 * @volt_uv: current battery voltage in microvolts
 * @status:  POWER_SUPPLY_STATUS_* value
 *
 * Returns the SOC that must be handed to userspace.  While charging the
 * original gauge is left alone (voltage rises with the charger, so a
 * voltage-only curve would jump around).  Above the shutdown voltage a
 * minimum of vsoc_min_soc is kept so Android never sees 0%.
 */
int star_volt_soc_apply(int raw_soc, int volt_uv, int status)
{
	struct vsoc_point pts[VSOC_MAX_POINTS];
	char buf[VSOC_STR_LEN];
	unsigned long flags;
	int n, mv, soc;

	raw_soc = clamp_val(raw_soc, 0, 100);

	spin_lock_irqsave(&vsoc_lock, flags);
	vsoc_last_raw = raw_soc;
	vsoc_last_volt_uv = volt_uv;
	vsoc_last_status = status;
	spin_unlock_irqrestore(&vsoc_lock, flags);

	if (!vsoc_enable || volt_uv <= 0) {
		soc = raw_soc;
		goto out;
	}
	if (status == POWER_SUPPLY_STATUS_CHARGING ||
	    status == POWER_SUPPLY_STATUS_FULL) {
		soc = raw_soc;
		goto out;
	}

	mv = volt_uv / 1000;

	/* Below the cutoff: show 0% and let battery_check_work power us off. */
	if (mv <= star_volt_soc_shutdown_mv()) {
		soc = 0;
		goto out;
	}

	if (vsoc_apply_below_mv > 0 && mv > vsoc_apply_below_mv) {
		soc = raw_soc;
		goto out;
	}

	strscpy(buf, vsoc_table, sizeof(buf));
	n = vsoc_parse(buf, pts, VSOC_MAX_POINTS);
	if (n == 0) {
		pr_warn_ratelimited("star_volt_soc: invalid table, using default\n");
		strscpy(buf, vsoc_default_table, sizeof(buf));
		n = vsoc_parse(buf, pts, VSOC_MAX_POINTS);
	}
	if (n == 0) {
		soc = raw_soc;
		goto out;
	}

	soc = clamp_val(vsoc_interpolate(pts, n, mv), 0, 100);
	if (soc < clamp_val(vsoc_min_soc, 0, 100))
		soc = clamp_val(vsoc_min_soc, 0, 100);

	if (vsoc_debug)
		pr_info_ratelimited("star_volt_soc: %d uV -> %d%% (raw %d%%)\n",
				    volt_uv, soc, raw_soc);

out:
	spin_lock_irqsave(&vsoc_lock, flags);
	vsoc_last_mapped = soc;
	spin_unlock_irqrestore(&vsoc_lock, flags);
	return soc;
}
EXPORT_SYMBOL_GPL(star_volt_soc_apply);

int star_volt_soc_shutdown_mv(void)
{
	/* Guard against an accidentally dangerous sysfs value. */
	return clamp_val(vsoc_shutdown_mv, 2500, 3400);
}
EXPORT_SYMBOL_GPL(star_volt_soc_shutdown_mv);

MODULE_AUTHOR("star low-voltage kernel build");
MODULE_DESCRIPTION("Rebuildable voltage -> battery SOC table for Xiaomi 11 Ultra");
MODULE_LICENSE("GPL v2");

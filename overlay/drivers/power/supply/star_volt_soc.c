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
 * property that Android actually reads, and provides a robust emergency
 * power-off check.
 *
 * Why filtering + debounce matter:
 *   A lithium cell's *instantaneous* terminal voltage = OCV - I*R_internal.
 *   Under load it sags well below the resting (OCV) voltage, so a momentary
 *   spike of current can momentarily read below the cutoff even when the cell
 *   is still healthy at, say, 3.3 V.  Keying the shutdown on the raw voltage
 *   therefore killed the phone far above the real 2.98 V cutoff.  We instead
 *   feed an EMA-filtered voltage and only power off once that filtered value
 *   has stayed at/below the cutoff for a sustained period.  The displayed
 *   percentage uses the same filtered voltage plus a second EMA, so it no
 *   longer jumps around with load.
 *
 * Runtime interface (built-in parameters, all tunable live via sysfs):
 *   /sys/module/star_volt_soc/parameters/table           (RW)
 *   /sys/module/star_volt_soc/parameters/shutdown_mv     (RW, mV)
 *   /sys/module/star_volt_soc/parameters/volt_ema_alpha  (RW, %)
 *   /sys/module/star_volt_soc/parameters/soc_ema_alpha   (RW, %)
 *   /sys/module/star_volt_soc/parameters/debounce_ms     (RW, ms)
 *   /sys/module/star_volt_soc/parameters/hard_cutoff_mv  (RW, mV)
 *   /sys/module/star_volt_soc/parameters/enable          (RW)
 *   /sys/module/star_volt_soc/parameters/apply_below_mv  (RW)
 *   /sys/module/star_volt_soc/parameters/min_soc         (RW)
 *   /sys/module/star_volt_soc/parameters/debug           (RW)
 *   /sys/module/star_volt_soc/parameters/status          (RO)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/types.h>

#include "star_volt_soc.h"

#define VSOC_MAX_POINTS		32
#define VSOC_STR_LEN		1024
#define VSOC_STATUS_LEN		320

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

/*
 * Voltage EMA coefficient (percent applied per update).  Smaller = smoother and
 * more resistant to load sag, but slower to react to genuine discharge.
 */
static int vsoc_volt_ema_alpha = 5;
module_param_named(volt_ema_alpha, vsoc_volt_ema_alpha, int, 0644);
MODULE_PARM_DESC(volt_ema_alpha, "Voltage EMA coefficient (%) - smaller = smoother");

/* Second EMA on the SOC itself, for extra display smoothing. */
static int vsoc_soc_ema_alpha = 4;
module_param_named(soc_ema_alpha, vsoc_soc_ema_alpha, int, 0644);
MODULE_PARM_DESC(soc_ema_alpha, "SOC EMA coefficient (%) - smaller = smoother");

/*
 * The filtered voltage must stay at/below the cutoff for this long before we
 * actually power off.  This is what prevents a momentary load sag (which can
 * momentarily read below 2.98 V at a 3.3 V cell) from killing the phone.
 */
static int vsoc_debounce_ms = 20000;
module_param_named(debounce_ms, vsoc_debounce_ms, int, 0644);
MODULE_PARM_DESC(debounce_ms,
	"Sustained below-cutoff time required before power-off (ms)");

/*
 * If the RAW voltage is at or below this, the cell is genuinely depleted and
 * we power off immediately (no debounce).  Kept safely below the cutoff.
 */
static int vsoc_hard_cutoff_mv = 2900;
module_param_named(hard_cutoff_mv, vsoc_hard_cutoff_mv, int, 0644);
MODULE_PARM_DESC(hard_cutoff_mv,
	"Raw voltage <= this (mV) powers off immediately");

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

/* ---- shared state (protected by vsoc_lock) ---- */
static int vsoc_last_raw = -1;
static int vsoc_last_volt_uv;
static int vsoc_last_status = -1;
static int vsoc_last_mapped = -1;
static long vsoc_v_filt;          /* EMA of voltage, mV */
static int  vsoc_v_init;
static int  vsoc_soc_filt;        /* EMA of SOC, 0..100 */
static int  vsoc_soc_init;
static unsigned long vsoc_below_jiffies; /* when filtered first fell below cutoff */
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

/*
 * Feed one raw voltage observation (mV) into the EMA and return the filtered
 * value.  Called from both the SOC mapping path and the shutdown path, so the
 * two stay consistent.
 */
static long vsoc_feed(int raw_mv)
{
	unsigned long flags;
	long vf;

	spin_lock_irqsave(&vsoc_lock, flags);
	if (!vsoc_v_init) {
		vsoc_v_filt = raw_mv;
		vsoc_v_init = 1;
	} else {
		int a = clamp_val(vsoc_volt_ema_alpha, 1, 50);

		vsoc_v_filt += (long)(raw_mv - vsoc_v_filt) * a / 100;
	}
	vf = vsoc_v_filt;
	spin_unlock_irqrestore(&vsoc_lock, flags);
	return vf;
}

static int vsoc_status_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int raw, volt, status, mapped;
	long vf;

	spin_lock_irqsave(&vsoc_lock, flags);
	raw = vsoc_last_raw;
	volt = vsoc_last_volt_uv;
	status = vsoc_last_status;
	mapped = vsoc_last_mapped;
	vf = vsoc_v_filt;
	spin_unlock_irqrestore(&vsoc_lock, flags);

	return scnprintf(buf, VSOC_STATUS_LEN,
		"raw_soc=%d volt_uv=%d v_filt_mv=%ld mapped_soc=%d status=%d "
		"enable=%d shutdown_mv=%d hard_cutoff_mv=%d debounce_ms=%d "
		"volt_ema_alpha=%d soc_ema_alpha=%d table=%s\n",
		raw, volt, vf, mapped, status, vsoc_enable,
		star_volt_soc_shutdown_mv(), vsoc_hard_cutoff_mv,
		vsoc_debounce_ms, vsoc_volt_ema_alpha, vsoc_soc_ema_alpha,
		vsoc_table);
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
 * Returns the SOC that must be handed to userspace.  The voltage used is the
 * EMA-filtered value (so load sags do not make the percentage jump), and the
 * resulting percentage is itself EMA-smoothed.  While charging the original
 * gauge is left alone (voltage rises with the charger, so a voltage-only curve
 * would jump around).  Above the shutdown voltage a minimum of vsoc_min_soc is
 * kept so Android never sees 0%.
 */
int star_volt_soc_apply(int raw_soc, int volt_uv, int status)
{
	struct vsoc_point pts[VSOC_MAX_POINTS];
	char buf[VSOC_STR_LEN];
	unsigned long flags;
	int n, soc, target, sda;
	long mv_f;

	raw_soc = clamp_val(raw_soc, 0, 100);

	spin_lock_irqsave(&vsoc_lock, flags);
	vsoc_last_raw = raw_soc;
	vsoc_last_volt_uv = volt_uv;
	vsoc_last_status = status;
	spin_unlock_irqrestore(&vsoc_lock, flags);

	if (!vsoc_enable) {
		soc = raw_soc;
		goto out;
	}

	/* keep the EMA tracking in every state (incl. charging) */
	if (volt_uv > 0)
		mv_f = vsoc_feed(volt_uv / 1000);

	if (volt_uv <= 0 ||
	    status == POWER_SUPPLY_STATUS_CHARGING ||
	    status == POWER_SUPPLY_STATUS_FULL) {
		soc = raw_soc;
		goto out;
	}

	if (mv_f <= star_volt_soc_shutdown_mv()) {
		/* filtered voltage at/below the real cutoff: expose 0% and let
		 * star_volt_soc_should_shutdown() do the power-off.  Using the
		 * filtered value means a momentary sag will not flip us to 0%. */
		soc = 0;
		goto out;
	}

	if (vsoc_apply_below_mv > 0 && (volt_uv / 1000) > vsoc_apply_below_mv) {
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

	target = clamp_val(vsoc_interpolate(pts, n, (int)mv_f), 0, 100);
	if (target < clamp_val(vsoc_min_soc, 0, 100))
		target = clamp_val(vsoc_min_soc, 0, 100);

	/* second EMA on SOC so the displayed % does not jump with load */
	sda = clamp_val(vsoc_soc_ema_alpha, 1, 50);
	spin_lock_irqsave(&vsoc_lock, flags);
	if (!vsoc_soc_init) {
		vsoc_soc_filt = target;
		vsoc_soc_init = 1;
	} else {
		vsoc_soc_filt += (target - vsoc_soc_filt) * sda / 100;
	}
	soc = vsoc_soc_filt;
	spin_unlock_irqrestore(&vsoc_lock, flags);

	if (vsoc_debug)
		pr_info_ratelimited("star_volt_soc: %ld mV -> %d%% (raw %d%%)\n",
				    mv_f, soc, raw_soc);

out:
	spin_lock_irqsave(&vsoc_lock, flags);
	vsoc_last_mapped = soc;
	spin_unlock_irqrestore(&vsoc_lock, flags);
	return soc;
}
EXPORT_SYMBOL_GPL(star_volt_soc_apply);

/**
 * star_volt_soc_should_shutdown - decide whether to power the phone off
 * @raw_mv: raw instantaneous battery voltage in mV
 *
 * Returns true only when the cell is genuinely at/below the cutoff: either the
 * raw voltage is catastrophically low (<= hard_cutoff_mv), or the EMA-filtered
 * voltage has stayed at/below the cutoff for at least debounce_ms.  This is
 * what keeps a momentary load sag (which reads below 2.98 V at a healthy
 * 3.3 V cell) from shutting the phone down.
 */
bool star_volt_soc_should_shutdown(int raw_mv)
{
	unsigned long flags, now = jiffies;
	int sd = star_volt_soc_shutdown_mv();
	long vf;
	bool trip = false;

	if (raw_mv < 2000)
		return false;           /* bogus read */

	vf = vsoc_feed(raw_mv);

	spin_lock_irqsave(&vsoc_lock, flags);
	if (raw_mv <= clamp_val(vsoc_hard_cutoff_mv, 2500, 3400)) {
		trip = true;
	} else if (vf <= sd) {
		if (vsoc_below_jiffies == 0)
			vsoc_below_jiffies = now;
		if (time_after(now, vsoc_below_jiffies +
			       msecs_to_jiffies(clamp_val(vsoc_debounce_ms,
						       0, 600000))))
			trip = true;
	} else {
		vsoc_below_jiffies = 0;
	}
	spin_unlock_irqrestore(&vsoc_lock, flags);
	return trip;
}
EXPORT_SYMBOL_GPL(star_volt_soc_should_shutdown);

int star_volt_soc_shutdown_mv(void)
{
	/* Guard against an accidentally dangerous sysfs value. */
	return clamp_val(vsoc_shutdown_mv, 2500, 3400);
}
EXPORT_SYMBOL_GPL(star_volt_soc_shutdown_mv);

MODULE_AUTHOR("star low-voltage kernel build");
MODULE_DESCRIPTION("Rebuildable voltage -> battery SOC table for Xiaomi 11 Ultra");
MODULE_LICENSE("GPL v2");

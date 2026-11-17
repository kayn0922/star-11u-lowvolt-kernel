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
 *   /sys/module/star_volt_soc/parameters/soc_rise_alpha  (RW, %)
 *   /sys/module/star_volt_soc/parameters/report_knee_mv  (RW, mV)
 *   /sys/module/star_volt_soc/parameters/report_ratio    (RW, %)
 *   /sys/module/star_volt_soc/parameters/report_floor_mv (RW)
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
#define VSOC_STATUS_LEN		512

/*
 * EMA sampling interval.  get_property() is called many times per second by
 * Android's health HAL, so feeding the EMA on every call would make it track
 * the instantaneous (noisy) voltage and defeat the whole point of smoothing.
 * We only accept a fresh sample every VSOC_SAMPLE_MS; in between, the last
 * computed percentage is returned unchanged.
 *
 * 500 ms (instead of 1000 ms) makes the displayed % advance in smaller, more
 * frequent steps, so a slow discharge no longer looks like a staircase.  The
 * EMA coefficient is a per-sample ratio, so halving the interval simply gives
 * twice as many (half-sized) steps; the smoothing strength is unchanged.
 */
#define VSOC_SAMPLE_MS		500

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
 *
 * At the 500 ms sampling interval, alpha 3 gives a time constant of roughly
 * 17 s.  That is what actually kills the "percentage jumps when a heavy app
 * starts / a charger is unplugged" symptom: a 200 mV load sag lasting a few
 * seconds is attenuated to a few tens of mV before it ever reaches the table.
 * A genuine discharge moves slowly enough that the lag is irrelevant, and the
 * power-off decision does not use this filter at all (it uses the raw voltage
 * with a debounce), so smoothing here costs no safety.
 */
static int vsoc_volt_ema_alpha = 3;
module_param_named(volt_ema_alpha, vsoc_volt_ema_alpha, int, 0644);
MODULE_PARM_DESC(volt_ema_alpha, "Voltage EMA coefficient (%) - smaller = smoother");

/*
 * Second EMA on the SOC itself.  Kept MILD by default (50, i.e. clamp max):
 * the voltage EMA above already does the heavy anti-jump smoothing, so an SOC
 * EMA this fast is enough to follow the real voltage without oversmoothing.
 * A very small value (e.g. 4) made the displayed % crawl toward its target only
 * a couple of samples per read and visibly freeze half-way (observed: stuck
 * at 44 instead of the correct 39), because Android stops reading capacity
 * once it looks stable.  Smaller = smoother display but slower to follow.
 * Range 1..100; at 100 the SOC tracks the table target immediately.
 */
static int vsoc_soc_ema_alpha = 50;
module_param_named(soc_ema_alpha, vsoc_soc_ema_alpha, int, 0644);
MODULE_PARM_DESC(soc_ema_alpha, "SOC EMA coefficient (%) - smaller = smoother, slower to follow");

/*
 * Asymmetric EMA for UPWARD corrections only.  Discharging should only ever
 * move the percentage down, but two things push it up:
 *
 *   1. load removal - the terminal voltage springs back by 100-300 mV and the
 *      table immediately maps that to a higher percentage;
 *   2. the charge->discharge handover - while charging we report the raw gauge
 *      value, and on unplug we switch to the table.  If the gauge only reached
 *      83% while the table says 100% at 4.4 V, the switch used to teleport the
 *      display from 83% to 100% within a couple of samples (observed: "full
 *      charge shows 80-something, then a few minutes after unplugging it jumps
 *      to 100%").
 *
 * Using a small alpha for increases turns those jumps into a short, smooth
 * ramp (alpha 8 at 500 ms: 83% -> 100% in roughly 20 s, instead of teleporting
 * there in two samples).  Decreases keep using soc_ema_alpha above, so normal
 * discharge is unaffected.  Range 1..100.
 */
static int vsoc_soc_rise_alpha = 8;
module_param_named(soc_rise_alpha, vsoc_soc_rise_alpha, int, 0644);
MODULE_PARM_DESC(soc_rise_alpha,
	"SOC EMA coefficient for UPWARD moves only (%) - smaller = no sudden jumps up");

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
static int vsoc_min_soc = 5;
module_param_named(min_soc, vsoc_min_soc, int, 0644);
MODULE_PARM_DESC(min_soc,
	"Minimum reported SOC while voltage is above shutdown_mv (default 5)");

/*
 * Reported-voltage compression (the fix for the "shutting down in N seconds"
 * reminder that HyperOS raises at ~3.4 V).
 *
 * Field evidence: with the hard floor OFF, the countdown came back at 3.4 V
 * even though the rebuilt table reports 29% there.  With the floor ON (v7,
 * voltage pinned at 3.6 V) the countdown did not appear.  So HyperOS is keying
 * on the *voltage*, not the percentage.
 *
 * Pinning the voltage is not an option though: a constant value stops the
 * battery-changed broadcast and freezes the whole battery UI.  Instead we
 * COMPRESS the low end of the curve.  Below report_knee_mv we report
 *
 *	reported = knee - (knee - raw) * report_ratio / 100
 *
 * i.e. only report_ratio % of the drop is visible.  With the defaults
 * (knee 3900 mV, ratio 20): raw 3900 -> 3900, 3.4 V -> 3800, 3.3 V -> 3780,
 * 2.98 V -> 3716.  The reported value keeps moving monotonically, so broadcasts
 * still fire and the percentage keeps updating, but the framework never sees a
 * value anywhere near its 3.4 V warning threshold.
 *
 * This affects ONLY the value reported to userspace.  The power-off path
 * (star_volt_soc_should_shutdown) always uses the real voltage, so safety is
 * unchanged.  Charging is never tampered with.  Set report_knee_mv=0 to disable.
 */
static int vsoc_report_knee_mv = 3900;
module_param_named(report_knee_mv, vsoc_report_knee_mv, int, 0644);
MODULE_PARM_DESC(report_knee_mv,
	"Compress reported voltage below this knee (mV, 0 = off, default 3900)");

/* How much of the drop below the knee stays visible (%, default 20). */
static int vsoc_report_ratio = 20;
module_param_named(report_ratio, vsoc_report_ratio, int, 0644);
MODULE_PARM_DESC(report_ratio,
	"Percent of the sub-knee voltage drop reported to userspace (default 20)");

/*
 * Optional hard floor, applied after the compression above.  DISABLED by
 * default (0): a *constant* floor pins the voltage, which freezes Android's
 * battery-changed broadcast (the whole battery UI stops updating).  Prefer
 * report_knee_mv/report_ratio; this is only a last-resort knob.
 */
static int vsoc_report_floor_mv = 0;
module_param_named(report_floor_mv, vsoc_report_floor_mv, int, 0644);
MODULE_PARM_DESC(report_floor_mv,
	"Fake voltage floor reported to Android while discharging (mV, 0=off)");

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
/*
 * SOC EMA is stored as fixed-point, scaled by VSOC_FP (100).  A plain integer
 * EMA stalls when the remaining gap is small: (gap=1)*alpha/100 truncates to 0,
 * so the value froze 1% short of its target and only jumped once the target
 * moved >=2% away -> "staircase" discharging.  Keeping 2 decimal places means
 * every sample advances a little, giving a smooth, continuous decline.
 */
#define VSOC_FP  100
static int  vsoc_soc_filt;        /* EMA of SOC, fixed-point 0..100*VSOC_FP */
static int  vsoc_soc_init;
static unsigned long vsoc_below_jiffies; /* when filtered first fell below cutoff */
static unsigned long vsoc_last_sample_jiffies; /* EMA sample throttle */
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
		"volt_ema_alpha=%d soc_ema_alpha=%d soc_rise_alpha=%d "
		"report_knee_mv=%d report_ratio=%d report_floor_mv=%d table=%s\n",
		raw, volt, vf, mapped, status, vsoc_enable,
		star_volt_soc_shutdown_mv(), vsoc_hard_cutoff_mv,
		vsoc_debounce_ms, vsoc_volt_ema_alpha, vsoc_soc_ema_alpha,
		vsoc_soc_rise_alpha, vsoc_report_knee_mv, vsoc_report_ratio,
		vsoc_report_floor_mv, vsoc_table);
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
 * Returns the SOC that must be handed to userspace (Android's health HAL reads
 * it from the "battery" power_supply).  The voltage used is the EMA-filtered
 * value, but the EMA only advances at most once per VSOC_SAMPLE_MS regardless
 * of how often this is called - otherwise the high call frequency from
 * userspace would defeat the smoothing and the % would still jump.
 *
 * The freshly computed value is cached; callers between samples get the stable
 * last value.  If the voltage observation is invalid (<= 0) we simply return
 * the cached value instead of falling back to the real (0%) gauge reading, so
 * a transient bad read can never flip the phone to 0% and trip Android's
 * critical-shutdown path.
 *
 * Above the shutdown voltage a minimum of vsoc_min_soc is kept so Android
 * never sees 0%.
 */
int star_volt_soc_apply(int raw_soc, int volt_uv, int status)
{
	struct vsoc_point pts[VSOC_MAX_POINTS];
	char buf[VSOC_STR_LEN];
	unsigned long flags, now = jiffies;
	int n = 0, soc = 0, target = 0, sda = 0, prev_status;
	long mv_f = 0;
	bool sampled = false, status_changed;

	raw_soc = clamp_val(raw_soc, 0, 100);

	spin_lock_irqsave(&vsoc_lock, flags);
	prev_status = vsoc_last_status;
	vsoc_last_raw = raw_soc;
	vsoc_last_volt_uv = volt_uv;
	vsoc_last_status = status;
	spin_unlock_irqrestore(&vsoc_lock, flags);

	/*
	 * Only a crossing between "on charger" and "on battery" needs a re-seed.
	 * The QTI driver flips between DISCHARGING and NOT_CHARGING while on
	 * battery, and treating that as a transition would re-seed the filter on
	 * every sample and freeze the percentage.
	 */
	status_changed =
		((status == POWER_SUPPLY_STATUS_CHARGING ||
		  status == POWER_SUPPLY_STATUS_FULL) !=
		 (prev_status == POWER_SUPPLY_STATUS_CHARGING ||
		  prev_status == POWER_SUPPLY_STATUS_FULL));

	if (!vsoc_enable)
		return raw_soc;

	if (volt_uv > 0 &&
	    time_after(now, vsoc_last_sample_jiffies +
		       msecs_to_jiffies(VSOC_SAMPLE_MS))) {
		mv_f = vsoc_feed(volt_uv / 1000);
		vsoc_last_sample_jiffies = now;
		sampled = true;

		if (status == POWER_SUPPLY_STATUS_FULL) {
			/*
			 * Charge complete.  Report 100%: the raw gauge often tops
			 * out well below 100 (observed 83%) even though the cell
			 * is genuinely full at ~4.4 V, which used to leave the
			 * status bar showing "80-something %" on a full battery
			 * and then teleporting to 100% after unplugging.
			 */
			soc = 100;
		} else if (status == POWER_SUPPLY_STATUS_CHARGING) {
			/*
			 * While charging, the terminal voltage is dominated by the
			 * charge current (CC phase sits at 4.3-4.4 V from ~50%),
			 * so the table would over-read badly.  Use the gauge's own
			 * percentage; the handover to the table on unplug is made
			 * continuous by the seeding + rise limit below.
			 */
			soc = raw_soc;
		} else if (mv_f <= star_volt_soc_shutdown_mv()) {
			/*
			 * Filtered voltage at/below the real cutoff: expose 0%
			 * and let star_volt_soc_should_shutdown() pull the plug.
			 * Using the filtered value means a momentary sag will
			 * not flip us to 0%.
			 */
			soc = 0;
		} else if (vsoc_apply_below_mv > 0 &&
			   (volt_uv / 1000) > vsoc_apply_below_mv) {
			soc = raw_soc;
		} else {
			strscpy(buf, vsoc_table, sizeof(buf));
			n = vsoc_parse(buf, pts, VSOC_MAX_POINTS);
			if (n == 0) {
				pr_warn_ratelimited("star_volt_soc: invalid table, using default\n");
				strscpy(buf, vsoc_default_table, sizeof(buf));
				n = vsoc_parse(buf, pts, VSOC_MAX_POINTS);
			}
			if (n == 0) {
				soc = raw_soc;
			} else {
				target = clamp_val(
					vsoc_interpolate(pts, n, (int)mv_f),
					0, 100);
				sda = clamp_val(vsoc_soc_ema_alpha, 1, 100);
				spin_lock_irqsave(&vsoc_lock, flags);
				if (!vsoc_soc_init || status_changed) {
					/*
					 * First sample, or we just switched between
					 * charging and discharging.  Seed from the
					 * value the user is currently looking at so
					 * the handover is continuous: without this,
					 * the stale filter (never updated while
					 * charging) walked to the new target in a
					 * couple of samples, which is what made the
					 * display look like it jumped to 100%.
					 */
					int seed = vsoc_last_mapped;

					if (seed < 0)
						seed = target;
					vsoc_soc_filt =
						clamp_val(seed, 0, 100) * VSOC_FP;
					vsoc_soc_init = 1;
				} else {
					int goal = target * VSOC_FP;
					/*
					 * Asymmetric: falls follow soc_ema_alpha
					 * (fast, so the normal discharge curve is
					 * not delayed), rises follow the much
					 * smaller soc_rise_alpha so a load-release
					 * rebound or a charge/discharge handover
					 * becomes a short ramp instead of a jump.
					 */
					int a = (goal > vsoc_soc_filt) ?
						clamp_val(vsoc_soc_rise_alpha,
							  1, 100) : sda;

					vsoc_soc_filt += (goal - vsoc_soc_filt) *
							 a / 100;
				}
				/* round to nearest integer percentage for userspace */
				soc = (vsoc_soc_filt + VSOC_FP / 2) / VSOC_FP;
				spin_unlock_irqrestore(&vsoc_lock, flags);
			}
		}

		/* floor while above the cutoff so Android never sees 0% */
		if (mv_f > star_volt_soc_shutdown_mv() &&
		    soc < clamp_val(vsoc_min_soc, 0, 100))
			soc = clamp_val(vsoc_min_soc, 0, 100);

		spin_lock_irqsave(&vsoc_lock, flags);
		vsoc_last_mapped = soc;
		spin_unlock_irqrestore(&vsoc_lock, flags);
	}

	if (vsoc_debug && sampled)
		pr_info_ratelimited("star_volt_soc: %ld mV -> %d%% (raw %d%%)\n",
				    mv_f, soc, raw_soc);

	spin_lock_irqsave(&vsoc_lock, flags);
	soc = vsoc_last_mapped;
	spin_unlock_irqrestore(&vsoc_lock, flags);
	return soc;
}
EXPORT_SYMBOL_GPL(star_volt_soc_apply);

/**
 * star_volt_soc_should_shutdown - decide whether to power the phone off
 * @raw_mv: raw instantaneous battery voltage in mV
 *
 * Returns true only when the cell is genuinely at/below the cutoff: either the
 * raw voltage is catastrophically low (<= hard_cutoff_mv), or the raw voltage
 * has stayed at/below the cutoff for at least debounce_ms.  This is what keeps
 * a momentary load sag (which reads below 2.98 V at a healthy 3.3 V cell) from
 * shutting the phone down.
 *
 * NOTE: this function deliberately does NOT feed the display EMA
 * (vsoc_feed) - a sag observed here must not drag the percentage shown to
 * the user down toward 0%.
 */
bool star_volt_soc_should_shutdown(int raw_mv)
{
	unsigned long flags, now = jiffies;
	int sd = star_volt_soc_shutdown_mv();
	bool trip = false;

	if (raw_mv < 2000)
		return false;           /* bogus read */

	spin_lock_irqsave(&vsoc_lock, flags);
	if (raw_mv <= clamp_val(vsoc_hard_cutoff_mv, 2500, 3400)) {
		trip = true;
	} else if (raw_mv <= sd) {
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

int star_volt_soc_hardcutoff_mv(void)
{
	/* Guard against an accidentally dangerous sysfs value. */
	return clamp_val(vsoc_hard_cutoff_mv, 2500, 3400);
}
EXPORT_SYMBOL_GPL(star_volt_soc_hardcutoff_mv);

/**
 * star_volt_soc_report_volt_uv - voltage shown to Android's framework
 * @raw_uv: real instantaneous battery voltage in microvolts
 * @status: POWER_SUPPLY_STATUS_* value
 *
 * HyperOS raises its "battery low, shutting down in N seconds" countdown off
 * the reported *voltage* (confirmed in the field: it fires at 3.4 V even when
 * the percentage there is 29%, and stops firing when the voltage was pinned at
 * 3.6 V).  So the low end of the reported curve is compressed instead of
 * clamped:
 *
 *	reported = knee - (knee - raw) * ratio / 100     (raw < knee)
 *
 * The reported value stays monotonic with the real voltage, so Android keeps
 * seeing changes and the battery UI keeps refreshing, while the framework never
 * sees anything close to its warning threshold.  Defaults (knee 3900, ratio 20)
 * report 3.4 V as 3800 mV and the 2.98 V cutoff as 3716 mV.
 *
 * This affects ONLY what userspace reads: the shutdown path
 * (star_volt_soc_should_shutdown) uses the real voltage, so safety is
 * unchanged.  Charging is never tampered with, and once the cell is genuinely
 * depleted (<= hard_cutoff_mv) the true voltage is passed through so the UI can
 * still show a real "about to die" reading.
 */
int star_volt_soc_report_volt_uv(int raw_uv, int status)
{
	int raw_mv, out_mv, knee, ratio, floor_mv;

	if (!vsoc_enable || raw_uv <= 0)
		return raw_uv;
	if (status == POWER_SUPPLY_STATUS_CHARGING ||
	    status == POWER_SUPPLY_STATUS_FULL)
		return raw_uv;

	raw_mv = raw_uv / 1000;
	if (raw_mv <= star_volt_soc_hardcutoff_mv())
		return raw_uv;              /* really depleted: show the truth */

	out_mv = raw_mv;
	knee = clamp_val(vsoc_report_knee_mv, 0, 4400);
	ratio = clamp_val(vsoc_report_ratio, 0, 100);
	if (knee > 0 && raw_mv < knee)
		out_mv = knee - (knee - raw_mv) * ratio / 100;

	floor_mv = clamp_val(vsoc_report_floor_mv, 0, 4200);
	if (floor_mv > 0 && out_mv < floor_mv)
		out_mv = floor_mv;

	if (out_mv > 4200)
		out_mv = 4200;

	return out_mv * 1000;
}
EXPORT_SYMBOL_GPL(star_volt_soc_report_volt_uv);

MODULE_AUTHOR("star low-voltage kernel build");
MODULE_DESCRIPTION("Rebuildable voltage -> battery SOC table for Xiaomi 11 Ultra");
MODULE_LICENSE("GPL v2");

// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/base/dd.c - The core device/driver interactions.
 *
 * This file contains the (sometimes tricky) code that controls the
 * interactions between devices and drivers, which primarily includes
 * driver binding and unbinding.
 *
 * All of this code used to exist in drivers/base/bus.c, but was
 * relocated to here in the name of compartmentalization (since it wasn't
 * strictly code just for the 'struct bus_type'.
 *
 * Copyright (c) 2002-5 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2007-2009 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2007-2009 Novell Inc.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/async.h>
#include <linux/pm_runtime.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/slab.h>
#include <linux/sched/debug.h>

#include "base.h"
#include "power/power.h"

/*
 * Deferred Probe infrastructure.
 *
 * Sometimes driver probe order matters, but the kernel doesn't always have
 * dependency information which means some drivers will get probed before a
 * resource it depends on is available.  For example, an SDHCI driver may
 * first need a GPIO line from an i2c GPIO controller before it can be
 * initialized.  If a required resource is not available yet, a driver can
 * request probing to be deferred by returning -EPROBE_DEFER from its probe hook
 *
 * Deferred probe maintains two lists of devices, a pending list and an active
 * list.  A driver returning -EPROBE_DEFER causes the device to be added to the
 * pending list.  A successful driver probe will trigger moving all devices
 * from the pending to the active list so that the workqueue will eventually
 * retry them.
 *
 * The deferred_probe_mutex must be held any time the deferred_probe_*_list
 * of the (struct device*)->p->deferred_probe pointers are manipulated
 */
static DEFINE_MUTEX(deferred_probe_mutex);
static LIST_HEAD(deferred_probe_pending_list);
static LIST_HEAD(deferred_probe_active_list);
static atomic_t deferred_trigger_count = ATOMIC_INIT(0);
static bool initcalls_done;

/* Save the async probe drivers' name from kernel cmdline */
#define ASYNC_DRV_NAMES_MAX_LEN	256
static char async_probe_drv_names[ASYNC_DRV_NAMES_MAX_LEN];
static bool async_probe_default;

/*
 * In some cases, like suspend to RAM or hibernation, It might be reasonable
 * to prohibit probing of devices as it could be unsafe.
 * Once defer_all_probes is true all drivers probes will be forcibly deferred.
 */
static bool defer_all_probes;

static void __device_set_deferred_probe_reason(const struct device *dev, char *reason)
{
	kfree(dev->p->deferred_probe_reason);
	dev->p->deferred_probe_reason = reason;
}

/*
 * Surya bring-up support.
 *
 * One driver that sleeps in its ->probe(), instead of returning
 * -EPROBE_DEFER, is fatal here: deferred_probe_work_func() probes devices
 * serially, deferred_probe_initcall() and wait_for_device_probe() both wait
 * for that work, and first-stage init runs behind those waits.  That is the
 * 2026-11-28 capture in full -- the probe worker (T65) printed "disp_cc-sm6150
 * af00000.clock-controller: Registered DISP CC clocks" at 0.839582s, after
 * apps_rsc drv@2 at 0.801626s and the RPMh regulator parents at 0.805-0.822s,
 * and said nothing more; the only later lines are "workqueue lockup - pool
 * cpus=6 ... in-flight: 65:deferred_probe_work_func BAR(1)" at 32s and 64s,
 * with no "Registered GCC clocks", no "Freeing unused kernel memory" and no
 * "Run /init".  The boot device was one step from working: at 0.643558s T1 had
 * probed /soc/ufshc@1d84000 past all four "assuming enabled" rails and stopped
 * at "ufshcd_init_clocks: core_clk clk get failed, -517" -- devm_clk_get("core")
 * is answered by GCC, which the wedged pass never reached.
 *
 * So the deferred list holds the UFS host waiting for GCC, GCC waits for the
 * RPMh ARC regulators that the wedged pass had in fact already brought up, and
 * nobody is walking the list any more.  Three responses, all gated on this
 * machine (compatible "xiaomi,surya" / "qcom,sm6150") so no other board changes
 * behaviour, and none of them touching how drivers bind:
 *
 *  - drive the storage-critical provider chain from a kernel thread with
 *    retries, in dependency order, instead of relying on the serial pass to
 *    reach it -- the retry is what the UFS host needs and cannot ask for;
 *  - bound both waits on deferred_probe_work, so a wedged driver costs a fixed
 *    delay instead of the boot;
 *  - name the device the pass is inside and dump that worker's stack, so the
 *    next capture identifies the sleeping driver rather than guessing it from
 *    the last line before the silence.
 *
 * All three were needed and they worked as designed, per the next capture
 * (console-ramoops-10.txt): the chain bound /soc/rsc@18200000 and every
 * interconnect provider, the waits expired instead of hanging, and Android's
 * first stage finally ran -- "Freeing unused kernel memory" at 25.445s,
 * "Run /init as init process", "init first stage started!", then
 * "partition(s) not found ... metadata, super, vbmeta_system" and
 * "Kernel panic - not syncing: Attempted to kill init! exitcode=0x00007f00"
 * (init's 127 after its own 10 s partition poll).  No block device exists
 * because the one device the chain could not finish is still holding it: the
 * chain printed "r0 probing /soc/clock-controller@100000" at 1.257457s and
 * never printed the result, and the pass was still queueing on that device
 * mutex at 21.475s.  So gcc_sm6150_probe() sleeps with the device lock held,
 * which was blamed on its mandatory vdd_cx_ao rail (see
 * arch/arm64/boot/dts/vendor/qcom/surya-sm6150.dts).
 *
 * That diagnosis was half right and the fix was not enough.  With the AO edge
 * retargeted -- asserted on the flashed DTB, and captured 11 shows all three
 * GCC supplies resolving to .../rpmh-regulator-cxlvl/regulator-pm6150-s1 and
 * .../rpmh-regulator-mxlvl/regulator-pm6150-s3 -- console-ramoops-11.txt is
 * still silent about GCC: "r0 probing /soc/clock-controller@100000" at
 * 1.202755s, no result line, no "Registered GCC clocks" in the whole 0.08s to
 * 21.49s the buffer still holds, and the watchdog's new holder dump printed
 * "task:surya_chain_... state:R" instead of a sleeping stack.  The rest of
 * that capture says what GCC is waiting for, though: /soc/ufshc@1d84000 had
 * already been probed at 0.893865s and stopped at "ufshcd_init_clocks:
 * core_clk clk get failed, -517", and /soc/ufsphy_mem@1d87000 sat at
 * "can_match: 0" because its clocks are <&gcc 0x7c> and <&gcc 0x82>.  Every
 * consumer of GCC defers; GCC itself defers on
 * devm_regulator_get("vdd_cx"|"vdd_cx_ao"|"vdd_mx"), because the *class*
 * devices behind those three phandles are the regulator children of
 * rpmh-regulator-cxlvl/mxlvl, which the chain binds only at the parent level.
 * A parent binding is not enough: rpmh-regulator creates its children with
 * devm_regulator_register()/of_platform_populate() while
 * driver_deferred_probe_enable is still false, so the children land on the
 * deferred list -- behind GCC, which is the first entry the serial pass
 * retries and the one that defers.  Hence two changes here: the arc regulator
 * children are named in the chain so they bind in the round before GCC is
 * retried, and GCC moved to the end of the list so a parked provider can no
 * longer cost the storage pair its round.
 */
static bool surya_bringup;
static char surya_attempt[96] = "idle";
static struct task_struct *surya_attempt_task;
static atomic_t surya_attempt_busy = ATOMIC_INIT(0);
/*
 * The chain thread is normally the *holder* of the device mutex the serial pass
 * is queueing behind, so the pass only shows where it waits and the chain shows
 * why.  Both are needed: capture 10 named /soc/clock-controller@100000 as the
 * device everything hung on, but not the function that sleeps, because only the
 * waiting stack was printed.
 */
static struct task_struct *surya_chain_task;
static bool surya_chain_settled;
static char surya_chain_at[96] = "idle";

static void surya_attempt_begin(struct device *dev)
{
	const char *node = dev->of_node ? of_node_full_name(dev->of_node) : NULL;

	if (node)
		snprintf(surya_attempt, sizeof(surya_attempt), "%s [%s]",
			 node, dev_name(dev));
	else
		snprintf(surya_attempt, sizeof(surya_attempt), "%s", dev_name(dev));
	WRITE_ONCE(surya_attempt_task, current);
	smp_wmb();
	atomic_set(&surya_attempt_busy, 1);
}

static void surya_attempt_end(void)
{
	atomic_set(&surya_attempt_busy, 0);
	WRITE_ONCE(surya_attempt_task, NULL);
}

/*
 * deferred_probe_work_func() - Retry probing devices in the active list.
 */
static void deferred_probe_work_func(struct work_struct *work)
{
	struct device *dev;
	struct device_private *private;
	/*
	 * This block processes every device in the deferred 'active' list.
	 * Each device is removed from the active list and passed to
	 * bus_probe_device() to re-attempt the probe.  The loop continues
	 * until every device in the active list is removed and retried.
	 *
	 * Note: Once the device is removed from the list and the mutex is
	 * released, it is possible for the device get freed by another thread
	 * and cause a illegal pointer dereference.  This code uses
	 * get/put_device() to ensure the device structure cannot disappear
	 * from under our feet.
	 */
	mutex_lock(&deferred_probe_mutex);
	while (!list_empty(&deferred_probe_active_list)) {
		private = list_first_entry(&deferred_probe_active_list,
					typeof(*dev->p), deferred_probe);
		dev = private->device;
		list_del_init(&private->deferred_probe);

		get_device(dev);

		__device_set_deferred_probe_reason(dev, NULL);

		/*
		 * Drop the mutex while probing each device; the probe path may
		 * manipulate the deferred list
		 */
		mutex_unlock(&deferred_probe_mutex);

		/*
		 * Force the device to the end of the dpm_list since
		 * the PM code assumes that the order we add things to
		 * the list is a good order for suspend but deferred
		 * probe makes that very unsafe.
		 */
		device_pm_move_to_tail(dev);

		dev_dbg(dev, "Retrying from deferred list\n");
		if (surya_bringup)
			surya_attempt_begin(dev);
		bus_probe_device(dev);
		if (surya_bringup)
			surya_attempt_end();
		mutex_lock(&deferred_probe_mutex);

		put_device(dev);
	}
	mutex_unlock(&deferred_probe_mutex);
}
static DECLARE_WORK(deferred_probe_work, deferred_probe_work_func);

void driver_deferred_probe_add(struct device *dev)
{
	if (!dev->can_match)
		return;

	mutex_lock(&deferred_probe_mutex);
	if (list_empty(&dev->p->deferred_probe)) {
		dev_dbg(dev, "Added to deferred list\n");
		list_add_tail(&dev->p->deferred_probe, &deferred_probe_pending_list);
	}
	mutex_unlock(&deferred_probe_mutex);
}

void driver_deferred_probe_del(struct device *dev)
{
	mutex_lock(&deferred_probe_mutex);
	if (!list_empty(&dev->p->deferred_probe)) {
		dev_dbg(dev, "Removed from deferred list\n");
		list_del_init(&dev->p->deferred_probe);
		__device_set_deferred_probe_reason(dev, NULL);
	}
	mutex_unlock(&deferred_probe_mutex);
}

static bool driver_deferred_probe_enable;
/**
 * driver_deferred_probe_trigger() - Kick off re-probing deferred devices
 *
 * This functions moves all devices from the pending list to the active
 * list and schedules the deferred probe workqueue to process them.  It
 * should be called anytime a driver is successfully bound to a device.
 *
 * Note, there is a race condition in multi-threaded probe. In the case where
 * more than one device is probing at the same time, it is possible for one
 * probe to complete successfully while another is about to defer. If the second
 * depends on the first, then it will get put on the pending list after the
 * trigger event has already occurred and will be stuck there.
 *
 * The atomic 'deferred_trigger_count' is used to determine if a successful
 * trigger has occurred in the midst of probing a driver. If the trigger count
 * changes in the midst of a probe, then deferred processing should be triggered
 * again.
 */
void driver_deferred_probe_trigger(void)
{
	if (!driver_deferred_probe_enable)
		return;

	/*
	 * A successful probe means that all the devices in the pending list
	 * should be triggered to be reprobed.  Move all the deferred devices
	 * into the active list so they can be retried by the workqueue
	 */
	mutex_lock(&deferred_probe_mutex);
	atomic_inc(&deferred_trigger_count);
	list_splice_tail_init(&deferred_probe_pending_list,
			      &deferred_probe_active_list);
	mutex_unlock(&deferred_probe_mutex);

	/*
	 * Kick the re-probe thread.  It may already be scheduled, but it is
	 * safe to kick it again.
	 */
	queue_work(system_unbound_wq, &deferred_probe_work);
}

/**
 * device_block_probing() - Block/defer device's probes
 *
 *	It will disable probing of devices and defer their probes instead.
 */
void device_block_probing(void)
{
	defer_all_probes = true;
	/* sync with probes to avoid races. */
	wait_for_device_probe();
}

/**
 * device_unblock_probing() - Unblock/enable device's probes
 *
 *	It will restore normal behavior and trigger re-probing of deferred
 * devices.
 */
void device_unblock_probing(void)
{
	defer_all_probes = false;
	driver_deferred_probe_trigger();
}

/**
 * device_set_deferred_probe_reason() - Set defer probe reason message for device
 * @dev: the pointer to the struct device
 * @vaf: the pointer to va_format structure with message
 */
void device_set_deferred_probe_reason(const struct device *dev, struct va_format *vaf)
{
	const char *drv = dev_driver_string(dev);
	char *reason;

	mutex_lock(&deferred_probe_mutex);

	reason = kasprintf(GFP_KERNEL, "%s: %pV", drv, vaf);
	__device_set_deferred_probe_reason(dev, reason);

	mutex_unlock(&deferred_probe_mutex);
}

/*
 * deferred_devs_show() - Show the devices in the deferred probe pending list.
 */
static int deferred_devs_show(struct seq_file *s, void *data)
{
	struct device_private *curr;

	mutex_lock(&deferred_probe_mutex);

	list_for_each_entry(curr, &deferred_probe_pending_list, deferred_probe)
		seq_printf(s, "%s\t%s", dev_name(curr->device),
			   curr->device->p->deferred_probe_reason ?: "\n");

	mutex_unlock(&deferred_probe_mutex);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(deferred_devs);

/*
 * DEBUG (k61.stop): log the devices whose probe is still deferred, with
 * the reason when the driver gave one, and the platform devices that no
 * driver bound to. Bring-up only.
 */
static int unbound_dev_show(struct device *dev, void *data)
{
	const char *compat = NULL;

	if (dev->driver || !dev->of_node)
		return 0;

	of_property_read_string(dev->of_node, "compatible", &compat);
	pr_info("k61.stop: unbound %s (%s)\n", dev_name(dev), compat ?: "-");
	return 0;
}

void driver_probe_state_dump(void)
{
	struct device_private *curr;

	mutex_lock(&deferred_probe_mutex);
	list_for_each_entry(curr, &deferred_probe_pending_list, deferred_probe)
		pr_info("k61.stop: deferred %s: %s", dev_name(curr->device),
			curr->deferred_probe_reason ?: "\n");
	mutex_unlock(&deferred_probe_mutex);

	bus_for_each_dev(&platform_bus_type, NULL, NULL, unbound_dev_show);
}

#ifdef CONFIG_MODULES
int driver_deferred_probe_timeout = 10;
#else
int driver_deferred_probe_timeout;
#endif

EXPORT_SYMBOL_GPL(driver_deferred_probe_timeout);

static int __init deferred_probe_timeout_setup(char *str)
{
	int timeout;

	if (!kstrtoint(str, 10, &timeout))
		driver_deferred_probe_timeout = timeout;
	return 1;
}
__setup("deferred_probe_timeout=", deferred_probe_timeout_setup);

/**
 * driver_deferred_probe_check_state() - Check deferred probe state
 * @dev: device to check
 *
 * Return:
 * * -ENODEV if initcalls have completed and modules are disabled.
 * * -ETIMEDOUT if the deferred probe timeout was set and has expired
 *   and modules are enabled.
 * * -EPROBE_DEFER in other cases.
 *
 * Drivers or subsystems can opt-in to calling this function instead of directly
 * returning -EPROBE_DEFER.
 */
int driver_deferred_probe_check_state(struct device *dev)
{
	if (!IS_ENABLED(CONFIG_MODULES) && initcalls_done) {
		dev_warn(dev, "ignoring dependency for device, assuming no driver\n");
		return -ENODEV;
	}

	if (!driver_deferred_probe_timeout && initcalls_done) {
		dev_warn(dev, "deferred probe timeout, ignoring dependency\n");
		return -ETIMEDOUT;
	}

	return -EPROBE_DEFER;
}
EXPORT_SYMBOL_GPL(driver_deferred_probe_check_state);

static void deferred_probe_timeout_work_func(struct work_struct *work)
{
	struct device_private *p;

	fw_devlink_drivers_done();

	driver_deferred_probe_timeout = 0;
	driver_deferred_probe_trigger();
	flush_work(&deferred_probe_work);

	mutex_lock(&deferred_probe_mutex);
	list_for_each_entry(p, &deferred_probe_pending_list, deferred_probe)
		dev_info(p->device, "deferred probe pending\n");
	mutex_unlock(&deferred_probe_mutex);
}
static DECLARE_DELAYED_WORK(deferred_probe_timeout_work, deferred_probe_timeout_work_func);

void deferred_probe_extend_timeout(void)
{
	/*
	 * If the work hasn't been queued yet or if the work expired, don't
	 * start a new one.
	 */
	if (cancel_delayed_work(&deferred_probe_timeout_work)) {
		schedule_delayed_work(&deferred_probe_timeout_work,
				driver_deferred_probe_timeout * HZ);
		pr_debug("Extended deferred probe timeout by %d secs\n",
					driver_deferred_probe_timeout);
	}
}

/*
 * Surya first-stage init can exit before the normal deferred-probe timeout.
 * The interconnect paths are the providers named by the UFS host's
 * interconnects property; 0x1700000 is deliberately absent because SDMMagpie
 * numbers that block aggre2 and this SoC has no node there -- an earlier
 * capture listed it and the "missing; no platform device" line was read as a
 * UFS supplier, which it never was.
 */
static const char * const surya_storage_paths[] = {
	"/soc/ufshc@1d84000",
	"/soc/ufsphy_mem@1d87000",
	"/soc/interconnect@16e0000",
	"/soc/interconnect@1500000",
	"/soc/interconnect@9680000",
	"/soc/interconnect@2",
	"/soc/interconnect@1620000",
	"/soc/clock-controller@100000",
	"/soc/rsc@18200000",
	"/soc/rsc@18200000/drv@2",
	"/soc/rsc@18200000/drv@2/bcm_voter",
	"/soc/rsc@18200000/drv@2/clock-controller",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-ldoa19",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-ldoa12",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-ldoc3",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-ldoa4",
	"/soc/mailbox@17c00000",
	"/soc/power-controller@c300000",
};

/*
 * fw_devlink parks a consumer in silence while one of its supplier phandles
 * points at a firmware node that never gains a struct device: the vendor RPMh
 * regulator driver registers its LDOs with devm_regulator_register(), so the
 * child device tree nodes have no primary fwnode device and the link from the
 * consumer is never purged.  Print the supplier view for an unbound node so a
 * stall names its blocker instead of leaving it to be guessed.
 */
static const struct {
	const char *list;
	const char *cells;
} surya_supplier_lists[] = {
	{ "clocks", "#clock-cells" },
	{ "interconnects", "#interconnect-cells" },
	{ "phys", "#phy-cells" },
	{ "resets", "#reset-cells" },
	{ "power-domains", "#power-domain-cells" },
	{ "vdd-hba-supply", NULL },
	{ "vcc-supply", NULL },
	{ "vccq2-supply", NULL },
	{ "qcom,vddp-ref-clk-supply", NULL },
	{ "vdda-phy-supply", NULL },
	{ "vdda-pll-supply", NULL },
};

static void surya_report_suppliers(struct device_node *con)
{
	int i, index;

	for (i = 0; i < ARRAY_SIZE(surya_supplier_lists); i++) {
		const char *list = surya_supplier_lists[i].list;
		const char *cells = surya_supplier_lists[i].cells;

		for (index = 0; ; index++) {
			struct of_phandle_args args;
			struct device_node *sup;
			struct fwnode_handle *fwn;
			struct device *sdev;

			if (cells) {
				if (of_parse_phandle_with_args(con, list, cells,
								index, &args))
					break;
				sup = args.np;
			} else {
				sup = of_parse_phandle(con, list, index);
				if (!sup)
					break;
			}

			fwn = of_fwnode_handle(sup);
			sdev = READ_ONCE(fwn->dev);
			pr_notice("Surya storage snapshot: supplier %s[%d] -> %pOFn available:%d device:%d driver:%d\n",
				  list, index, sup, of_device_is_available(sup),
				  !!sdev, sdev ? !!READ_ONCE(sdev->driver) : 0);
			of_node_put(sup);
		}
	}
}

static bool surya_storage_device(const struct device *dev)
{
	int i;

	if (!dev->of_node)
		return false;
	for (i = 0; i < ARRAY_SIZE(surya_storage_paths); i++)
		if (!strcmp(of_node_full_name(dev->of_node), surya_storage_paths[i]))
			return true;
	return false;
}

static void surya_snapshot(void)
{
	struct device_private *p;
	struct device_node *np;
	int i;

	for (i = 0; i < ARRAY_SIZE(surya_storage_paths); i++) {
		const char *path = surya_storage_paths[i];
		struct platform_device *pdev;
		const char *state;

		np = of_find_node_by_path(path);
		pdev = np ? of_find_device_by_node(np) : NULL;
		state = !np ? "missing" : !of_device_is_available(np) ?
			"disabled" : "enabled";
		pr_notice("Surya storage snapshot: %s %s; %s\n", path, state,
			  !pdev ? "no platform device" :
			  READ_ONCE(pdev->dev.driver) ? "driver assigned" : "unbound");
		if (pdev) {
			pr_notice("Surya storage snapshot: %s can_match: %d\n",
				  path, pdev->dev.can_match);
			if (!READ_ONCE(pdev->dev.driver))
				surya_report_suppliers(np);
			put_device(&pdev->dev);
		}
		of_node_put(np);
	}

	mutex_lock(&deferred_probe_mutex);
	list_for_each_entry(p, &deferred_probe_pending_list, deferred_probe) {
		if (surya_storage_device(p->device))
			dev_notice(p->device, "Surya storage snapshot: deferred: %s\n",
				   p->deferred_probe_reason ?: "reason unavailable");
	}
	mutex_unlock(&deferred_probe_mutex);
}
static void surya_ufs_snapshot(struct work_struct *work)
{
	surya_snapshot();
}
static DECLARE_DELAYED_WORK(surya_ufs_snapshot_work, surya_ufs_snapshot);

/*
 * Whether one deferred probe is currently in flight, and on which device.
 * Returns true if the pass finished within the deadline.
 */
static bool surya_wait_probes(unsigned int secs)
{
	unsigned long deadline = jiffies + secs * HZ;

	while (time_before(jiffies, deadline)) {
		bool busy;

		mutex_lock(&deferred_probe_mutex);
		busy = !!work_busy(&deferred_probe_work) ||
		       atomic_read(&surya_attempt_busy) ||
		       !list_empty(&deferred_probe_active_list);
		mutex_unlock(&deferred_probe_mutex);
		if (!busy)
			return true;
		msleep(100);
	}
	return !work_busy(&deferred_probe_work) &&
	       !atomic_read(&surya_attempt_busy);
}

static void surya_wedge_watchdog(struct work_struct *work);
static DECLARE_DELAYED_WORK(surya_wedge_work, surya_wedge_watchdog);

/*
 * Nothing here changes what gets probed; it only says, out loud and into the
 * pstore window, what the serial deferred pass is stuck on.  That is the
 * difference between fixing the next driver and guessing at it.
 */
static void surya_wedge_watchdog(struct work_struct *work)
{
	static unsigned int rounds;

	/*
	 * Read the marker before the stack: if the probe returns while we are
	 * dumping, surya_attempt goes stale underneath us, and a slightly
	 * stale name is still the answer we came for.
	 */
	if (atomic_read(&surya_attempt_busy)) {
		pr_notice("Surya bringup: deferred probe still in %s after %u s\n",
			  surya_attempt, (rounds + 1) * 20);
		if (surya_attempt_task)
			sched_show_task(surya_attempt_task);
		surya_snapshot();
	}
	/* The chain, separately: blocked in a probe, settled, or never started. */
	if (READ_ONCE(surya_chain_task)) {
		pr_notice("Surya bringup: provider chain blocked in %s\n",
			  surya_chain_at);
		sched_show_task(READ_ONCE(surya_chain_task));
	} else if (!surya_chain_settled) {
		pr_notice("Surya bringup: provider chain has not reached a probe\n");
	} else {
		pr_notice("Surya bringup: provider chain settled at %s\n",
			  surya_chain_at);
	}
	if (++rounds < 6)
		queue_delayed_work_on(1, system_long_wq, &surya_wedge_work,
				      20 * HZ);
}

/*
 * Providers first-stage init needs, in dependency order.  The serial deferred
 * pass cannot be trusted to reach them (see above), so the chain is re-driven
 * from a kernel thread with retries: a round either binds one more device or
 * every remaining one returns -EPROBE_DEFER again and is retried after 100 ms.
 * Doing this in a thread rather than in the initcall matters: if any provider
 * here is the driver that sleeps forever, only this thread stalls.
 */
static const char * const surya_chain[] = {
	"/soc/rsc@18200000",
	"/soc/rsc@18200000/drv@2",
	"/soc/rsc@18200000/drv@2/bcm_voter",
	"/soc/rsc@18200000/drv@2/clock-controller",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-cxlvl",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-cxlvl/regulator-pm6150-s1",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-cxlvl/regulator-pm6150-s1-level-ao",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-mxlvl",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-mxlvl/regulator-pm6150-s3",
	"/soc/rsc@18200000/drv@2/rpmh-regulator-mxlvl/regulator-pm6150-s3-level-ao",
	"/soc/interconnect@2",
	"/soc/interconnect@9680000",
	"/soc/interconnect@1500000",
	"/soc/interconnect@1620000",
	"/soc/interconnect@16e0000",
	"/soc/ufsphy_mem@1d87000",
	"/soc/ufshc@1d84000",
	/*
	 * GCC last, not eleventh: console-ramoops-11.txt caught the round
	 * parking on the clock controller and never reaching the storage pair
	 * behind it, so a provider that parks must at least be the last thing
	 * the round loses.  The workflow below (surya-dtb.yml) extracts every
	 * quoted path from this list and checks it against the flashed DTB,
	 * so keep prose here free of absolute node paths.
	 */
	"/soc/clock-controller@100000",
};

static DECLARE_COMPLETION(surya_chain_done);

static int surya_chain_thread(void *unused)
{
	unsigned long deadline = jiffies + 20 * HZ;
	int round = 0, i;
	int bound = 0, last_bound = -1;

	for (round = 0; time_before(jiffies, deadline) && bound != last_bound;
	     round++) {
		last_bound = bound;
		bound = 0;

		for (i = 0; i < ARRAY_SIZE(surya_chain); i++) {
			struct device_node *np = of_find_node_by_path(surya_chain[i]);
			struct platform_device *pdev;

			if (!np)
				continue;
			pdev = of_find_device_by_node(np);
			of_node_put(np);
			if (!pdev)
				continue;
			if (pdev->dev.driver) {
				bound++;
			} else {
				snprintf(surya_chain_at, sizeof(surya_chain_at),
					 "%s", surya_chain[i]);
				WRITE_ONCE(surya_chain_task, current);
				pr_notice("Surya bringup: r%d probing %s\n",
					  round, surya_chain[i]);
				bus_probe_device(&pdev->dev);
				if (pdev->dev.driver) {
					bound++;
					pr_notice("Surya bringup: r%d %s -> bound\n",
						  round, surya_chain[i]);
				} else {
					pr_notice("Surya bringup: r%d %s -> still deferred\n",
						  round, surya_chain[i]);
				}
			}
			put_device(&pdev->dev);
		}
		msleep(100);
	}
	if (time_after_eq(jiffies, deadline))
		pr_notice("Surya bringup: provider chain out of time at %s\n",
			  surya_chain_at);
	WRITE_ONCE(surya_chain_task, NULL);
	surya_chain_settled = true;
	surya_snapshot();
	snprintf(surya_chain_at, sizeof(surya_chain_at), "done");
	pr_notice("Surya bringup: provider chain pass finished\n");
	complete(&surya_chain_done);
	return 0;
}

/**
 * deferred_probe_initcall() - Enable probing of deferred devices
 *
 * We don't want to get in the way when the bulk of drivers are getting probed.
 * Instead, this initcall makes sure that deferred probing is delayed until
 * late_initcall time.
 */
static int deferred_probe_initcall(void)
{
	struct task_struct *chain;

	if (of_machine_is_compatible("xiaomi,surya") ||
	    of_machine_is_compatible("qcom,sm6150")) {
		surya_bringup = true;
		pr_notice("Surya bringup: armed (compatible xiaomi,surya=%d qcom,sm6150=%d)\n",
			  of_machine_is_compatible("xiaomi,surya"),
			  of_machine_is_compatible("qcom,sm6150"));
		/*
		 * system_long_wq, not system_wq: a wedged ->probe() starves the
		 * per-CPU pool of the workqueue whose worker is stuck inside it,
		 * and the 2026-11-28 capture lost the 4 s snapshot to exactly
		 * that -- zero lines, where the same code had printed ten of them
		 * one build earlier.  system_long_wq is the pool reserved for work
		 * items that may block for a long time, and it is a separate pool
		 * from the one deferred_probe_work runs on.
		 */
		queue_delayed_work_on(0, system_long_wq,
				      &surya_ufs_snapshot_work, 4 * HZ);
		queue_delayed_work_on(1, system_long_wq, &surya_wedge_work,
				      20 * HZ);
	}

	debugfs_create_file("devices_deferred", 0444, NULL, NULL,
			    &deferred_devs_fops);

	if (surya_bringup) {
		chain = kthread_run(surya_chain_thread, NULL, "surya_chain");
		if (IS_ERR(chain))
			complete(&surya_chain_done);
	} else {
		chain = ERR_PTR(-ENODEV);
	}

	driver_deferred_probe_enable = true;
	driver_deferred_probe_trigger();
	/* Sort as many dependencies as possible before exiting initcalls */
	if (surya_bringup) {
		/*
		 * Give the provider chain a few seconds to reach the UFS host on
		 * its own before the serial pass is waited on: the point is a
		 * mounted root, not a tidy initcall ordering.
		 */
		if (!IS_ERR(chain) &&
		    !wait_for_completion_timeout(&surya_chain_done,
						 msecs_to_jiffies(4000)))
			pr_notice("Surya bringup: provider chain still at %s after 4 s\n",
				  surya_chain_at);
		if (!surya_wait_probes(10))
			pr_notice("Surya bringup: deferred pass still busy after 10 s, continuing init\n");
	} else {
		flush_work(&deferred_probe_work);
	}
	initcalls_done = true;

	if (!IS_ENABLED(CONFIG_MODULES))
		fw_devlink_drivers_done();

	/*
	 * Trigger deferred probe again, this time we won't defer anything
	 * that is optional
	 */
	driver_deferred_probe_trigger();
	if (surya_bringup) {
		/*
		 * Same reason as above: the second pass is a nicety, not a
		 * licence for one driver's ->probe() to stop the boot.
		 */
		if (!surya_wait_probes(10))
			pr_notice("Surya bringup: second deferred pass still busy, continuing init\n");
	} else {
		flush_work(&deferred_probe_work);
	}

	if (driver_deferred_probe_timeout > 0) {
		schedule_delayed_work(&deferred_probe_timeout_work,
			driver_deferred_probe_timeout * HZ);
	}
	return 0;
}
late_initcall(deferred_probe_initcall);

static void __exit deferred_probe_exit(void)
{
	debugfs_lookup_and_remove("devices_deferred", NULL);
}
__exitcall(deferred_probe_exit);

/**
 * device_is_bound() - Check if device is bound to a driver
 * @dev: device to check
 *
 * Returns true if passed device has already finished probing successfully
 * against a driver.
 *
 * This function must be called with the device lock held.
 */
bool device_is_bound(struct device *dev)
{
	return dev->p && klist_node_attached(&dev->p->knode_driver);
}

static void driver_bound(struct device *dev)
{
	if (device_is_bound(dev)) {
		pr_warn("%s: device %s already bound\n",
			__func__, kobject_name(&dev->kobj));
		return;
	}

	pr_debug("driver: '%s': %s: bound to device '%s'\n", dev->driver->name,
		 __func__, dev_name(dev));

	klist_add_tail(&dev->p->knode_driver, &dev->driver->p->klist_devices);
	device_links_driver_bound(dev);

	device_pm_check_callbacks(dev);

	/*
	 * Make sure the device is no longer in one of the deferred lists and
	 * kick off retrying all pending devices
	 */
	driver_deferred_probe_del(dev);
	driver_deferred_probe_trigger();

	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_BOUND_DRIVER, dev);

	kobject_uevent(&dev->kobj, KOBJ_BIND);
}

static ssize_t coredump_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	device_lock(dev);
	dev->driver->coredump(dev);
	device_unlock(dev);

	return count;
}
static DEVICE_ATTR_WO(coredump);

static int driver_sysfs_add(struct device *dev)
{
	int ret;

	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_BIND_DRIVER, dev);

	ret = sysfs_create_link(&dev->driver->p->kobj, &dev->kobj,
				kobject_name(&dev->kobj));
	if (ret)
		goto fail;

	ret = sysfs_create_link(&dev->kobj, &dev->driver->p->kobj,
				"driver");
	if (ret)
		goto rm_dev;

	if (!IS_ENABLED(CONFIG_DEV_COREDUMP) || !dev->driver->coredump)
		return 0;

	ret = device_create_file(dev, &dev_attr_coredump);
	if (!ret)
		return 0;

	sysfs_remove_link(&dev->kobj, "driver");

rm_dev:
	sysfs_remove_link(&dev->driver->p->kobj,
			  kobject_name(&dev->kobj));

fail:
	return ret;
}

static void driver_sysfs_remove(struct device *dev)
{
	struct device_driver *drv = dev->driver;

	if (drv) {
		if (drv->coredump)
			device_remove_file(dev, &dev_attr_coredump);
		sysfs_remove_link(&drv->p->kobj, kobject_name(&dev->kobj));
		sysfs_remove_link(&dev->kobj, "driver");
	}
}

/**
 * device_bind_driver - bind a driver to one device.
 * @dev: device.
 *
 * Allow manual attachment of a driver to a device.
 * Caller must have already set @dev->driver.
 *
 * Note that this does not modify the bus reference count.
 * Please verify that is accounted for before calling this.
 * (It is ok to call with no other effort from a driver's probe() method.)
 *
 * This function must be called with the device lock held.
 *
 * Callers should prefer to use device_driver_attach() instead.
 */
int device_bind_driver(struct device *dev)
{
	int ret;

	ret = driver_sysfs_add(dev);
	if (!ret) {
		device_links_force_bind(dev);
		driver_bound(dev);
	}
	else if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_DRIVER_NOT_BOUND, dev);
	return ret;
}
EXPORT_SYMBOL_GPL(device_bind_driver);

static atomic_t probe_count = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(probe_waitqueue);

static ssize_t state_synced_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	bool val;

	device_lock(dev);
	val = dev->state_synced;
	device_unlock(dev);

	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(state_synced);

static void device_unbind_cleanup(struct device *dev)
{
	devres_release_all(dev);
	arch_teardown_dma_ops(dev);
	kfree(dev->dma_range_map);
	dev->dma_range_map = NULL;
	dev->driver = NULL;
	dev_set_drvdata(dev, NULL);
	if (dev->pm_domain && dev->pm_domain->dismiss)
		dev->pm_domain->dismiss(dev);
	pm_runtime_reinit(dev);
	dev_pm_set_driver_flags(dev, 0);
}

static void device_remove(struct device *dev)
{
	device_remove_file(dev, &dev_attr_state_synced);
	device_remove_groups(dev, dev->driver->dev_groups);

	if (dev->bus && dev->bus->remove)
		dev->bus->remove(dev);
	else if (dev->driver->remove)
		dev->driver->remove(dev);
}

static int call_driver_probe(struct device *dev, struct device_driver *drv)
{
	int ret = 0;

	if (dev->bus->probe)
		ret = dev->bus->probe(dev);
	else if (drv->probe)
		ret = drv->probe(dev);

	switch (ret) {
	case 0:
		break;
	case -EPROBE_DEFER:
		/* Driver requested deferred probing */
		dev_dbg(dev, "Driver %s requests probe deferral\n", drv->name);
		break;
	case -ENODEV:
	case -ENXIO:
		pr_debug("%s: probe of %s rejects match %d\n",
			 drv->name, dev_name(dev), ret);
		break;
	default:
		/* driver matched but the probe failed */
		pr_warn("%s: probe of %s failed with error %d\n",
			drv->name, dev_name(dev), ret);
		break;
	}

	return ret;
}

static int really_probe(struct device *dev, struct device_driver *drv)
{
	bool test_remove = IS_ENABLED(CONFIG_DEBUG_TEST_DRIVER_REMOVE) &&
			   !drv->suppress_bind_attrs;
	int ret, link_ret;

	if (defer_all_probes) {
		/*
		 * Value of defer_all_probes can be set only by
		 * device_block_probing() which, in turn, will call
		 * wait_for_device_probe() right after that to avoid any races.
		 */
		dev_dbg(dev, "Driver %s force probe deferral\n", drv->name);
		return -EPROBE_DEFER;
	}

	link_ret = device_links_check_suppliers(dev);
	if (link_ret == -EPROBE_DEFER)
		return link_ret;

	pr_debug("bus: '%s': %s: probing driver %s with device %s\n",
		 drv->bus->name, __func__, drv->name, dev_name(dev));
	if (!list_empty(&dev->devres_head)) {
		dev_crit(dev, "Resources present before probing\n");
		ret = -EBUSY;
		goto done;
	}

re_probe:
	dev->driver = drv;

	/* If using pinctrl, bind pins now before probing */
	ret = pinctrl_bind_pins(dev);
	if (ret)
		goto pinctrl_bind_failed;

	if (dev->bus->dma_configure) {
		ret = dev->bus->dma_configure(dev);
		if (ret)
			goto pinctrl_bind_failed;
	}

	ret = driver_sysfs_add(dev);
	if (ret) {
		pr_err("%s: driver_sysfs_add(%s) failed\n",
		       __func__, dev_name(dev));
		goto sysfs_failed;
	}

	if (dev->pm_domain && dev->pm_domain->activate) {
		ret = dev->pm_domain->activate(dev);
		if (ret)
			goto probe_failed;
	}

	ret = call_driver_probe(dev, drv);
	if (ret) {
		/*
		 * If fw_devlink_best_effort is active (denoted by -EAGAIN), the
		 * device might actually probe properly once some of its missing
		 * suppliers have probed. So, treat this as if the driver
		 * returned -EPROBE_DEFER.
		 */
		if (link_ret == -EAGAIN)
			ret = -EPROBE_DEFER;

		/*
		 * Return probe errors as positive values so that the callers
		 * can distinguish them from other errors.
		 */
		ret = -ret;
		goto probe_failed;
	}

	ret = device_add_groups(dev, drv->dev_groups);
	if (ret) {
		dev_err(dev, "device_add_groups() failed\n");
		goto dev_groups_failed;
	}

	if (dev_has_sync_state(dev)) {
		ret = device_create_file(dev, &dev_attr_state_synced);
		if (ret) {
			dev_err(dev, "state_synced sysfs add failed\n");
			goto dev_sysfs_state_synced_failed;
		}
	}

	if (test_remove) {
		test_remove = false;

		device_remove(dev);
		driver_sysfs_remove(dev);
		if (dev->bus && dev->bus->dma_cleanup)
			dev->bus->dma_cleanup(dev);
		device_unbind_cleanup(dev);

		goto re_probe;
	}

	pinctrl_init_done(dev);

	if (dev->pm_domain && dev->pm_domain->sync)
		dev->pm_domain->sync(dev);

	driver_bound(dev);
	pr_debug("bus: '%s': %s: bound device %s to driver %s\n",
		 drv->bus->name, __func__, dev_name(dev), drv->name);
	goto done;

dev_sysfs_state_synced_failed:
dev_groups_failed:
	device_remove(dev);
probe_failed:
	driver_sysfs_remove(dev);
sysfs_failed:
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_DRIVER_NOT_BOUND, dev);
	if (dev->bus && dev->bus->dma_cleanup)
		dev->bus->dma_cleanup(dev);
pinctrl_bind_failed:
	device_links_no_driver(dev);
	/* call_driver_probe() errors are positive here */
	if (ret != -EPROBE_DEFER && ret != EPROBE_DEFER)
		device_links_probe_failed(dev);
	device_unbind_cleanup(dev);
done:
	return ret;
}

/*
 * For initcall_debug, show the driver probe time.
 */
static int really_probe_debug(struct device *dev, struct device_driver *drv)
{
	ktime_t calltime, rettime;
	int ret;

	printk(KERN_DEBUG "k61probe: %s (%s)\n", dev_name(dev), drv->name);
	calltime = ktime_get();
	ret = really_probe(dev, drv);
	rettime = ktime_get();
	/*
	 * Don't change this to pr_debug() because that requires
	 * CONFIG_DYNAMIC_DEBUG and we want a simple 'initcall_debug' on the
	 * kernel commandline to print this all the time at the debug level.
	 */
	printk(KERN_DEBUG "probe of %s returned %d after %lld usecs\n",
		 dev_name(dev), ret, ktime_us_delta(rettime, calltime));
	return ret;
}

/**
 * driver_probe_done
 * Determine if the probe sequence is finished or not.
 *
 * Should somehow figure out how to use a semaphore, not an atomic variable...
 */
int driver_probe_done(void)
{
	int local_probe_count = atomic_read(&probe_count);

	pr_debug("%s: probe_count = %d\n", __func__, local_probe_count);
	if (local_probe_count)
		return -EBUSY;
	return 0;
}

/**
 * wait_for_device_probe
 * Wait for device probing to be completed.
 */
void wait_for_device_probe(void)
{
	bool wedged = false;

	if (surya_bringup) {
		/*
		 * One provider driver that sleeps instead of returning
		 * -EPROBE_DEFER would otherwise stop first-stage init here
		 * forever.  So: first wait for the storage provider chain to
		 * settle, which is the wait that actually matters -- it ends
		 * when /soc/ufshc@1d84000 is bound and a block device exists --
		 * then give the serial pass a bounded grace period.  By then the
		 * UFS host is either bound or it never will be, and waiting
		 * longer changes nothing.
		 */
		if (!wait_for_completion_timeout(&surya_chain_done, 20 * HZ))
			pr_notice("Surya bringup: provider chain still at %s after 20 s\n",
				  surya_chain_at);
		if (!surya_wait_probes(20)) {
			pr_notice("Surya bringup: wait_for_device_probe() timed out with %s\n",
				  surya_attempt);
			if (!atomic_read(&probe_count))
				async_synchronize_full();
			return;
		}
		wedged = !wait_event_timeout(probe_waitqueue,
					     atomic_read(&probe_count) == 0,
					     20 * HZ);
		if (wedged)
			pr_notice("Surya bringup: %d device probe(s) still running\n",
				  atomic_read(&probe_count));
	} else {
		/* wait for the deferred probe workqueue to finish */
		flush_work(&deferred_probe_work);

		/* wait for the known devices to complete their probing */
		wait_event(probe_waitqueue, atomic_read(&probe_count) == 0);
	}

	if (wedged)
		return;

	async_synchronize_full();
}
EXPORT_SYMBOL_GPL(wait_for_device_probe);

/**
 * flush_deferred_probe_now
 *
 * This function should be used sparingly. It's meant for when we need to flush
 * the deferred probe list at earlier initcall levels. Really meant only for KVM
 * needs. This function should never be exported because it makes no sense for
 * modules to call this.
 */
void flush_deferred_probe_now(void)
{
	/*
	 * Really shouldn't using this if deferred probe has already been
	 * enabled
	 */
	if (WARN_ON(driver_deferred_probe_enable))
		return;

	driver_deferred_probe_enable = true;
	driver_deferred_probe_trigger();
	wait_for_device_probe();
	driver_deferred_probe_enable = false;
}

static int __driver_probe_device(struct device_driver *drv, struct device *dev)
{
	int ret = 0;

	if (dev->p->dead || !device_is_registered(dev))
		return -ENODEV;
	if (dev->driver)
		return -EBUSY;

	dev->can_match = true;
	pr_debug("bus: '%s': %s: matched device %s with driver %s\n",
		 drv->bus->name, __func__, dev_name(dev), drv->name);

	pm_runtime_get_suppliers(dev);
	if (dev->parent)
		pm_runtime_get_sync(dev->parent);

	pm_runtime_barrier(dev);
	if (initcall_debug)
		ret = really_probe_debug(dev, drv);
	else
		ret = really_probe(dev, drv);
	pm_request_idle(dev);

	if (dev->parent)
		pm_runtime_put(dev->parent);

	pm_runtime_put_suppliers(dev);
	return ret;
}

/**
 * driver_probe_device - attempt to bind device & driver together
 * @drv: driver to bind a device to
 * @dev: device to try to bind to the driver
 *
 * This function returns -ENODEV if the device is not registered, -EBUSY if it
 * already has a driver, 0 if the device is bound successfully and a positive
 * (inverted) error code for failures from the ->probe method.
 *
 * This function must be called with @dev lock held.  When called for a
 * USB interface, @dev->parent lock must be held as well.
 *
 * If the device has a parent, runtime-resume the parent before driver probing.
 */
static int driver_probe_device(struct device_driver *drv, struct device *dev)
{
	int trigger_count = atomic_read(&deferred_trigger_count);
	int ret;

	atomic_inc(&probe_count);
	ret = __driver_probe_device(drv, dev);
	if (ret == -EPROBE_DEFER || ret == EPROBE_DEFER) {
		driver_deferred_probe_add(dev);

		/*
		 * Did a trigger occur while probing? Need to re-trigger if yes
		 */
		if (trigger_count != atomic_read(&deferred_trigger_count) &&
		    !defer_all_probes)
			driver_deferred_probe_trigger();
	}
	atomic_dec(&probe_count);
	wake_up_all(&probe_waitqueue);
	return ret;
}

static inline bool cmdline_requested_async_probing(const char *drv_name)
{
	bool async_drv;

	async_drv = parse_option_str(async_probe_drv_names, drv_name);

	return (async_probe_default != async_drv);
}

/* The option format is "driver_async_probe=drv_name1,drv_name2,..." */
static int __init save_async_options(char *buf)
{
	if (strlen(buf) >= ASYNC_DRV_NAMES_MAX_LEN)
		pr_warn("Too long list of driver names for 'driver_async_probe'!\n");

	strscpy(async_probe_drv_names, buf, ASYNC_DRV_NAMES_MAX_LEN);
	async_probe_default = parse_option_str(async_probe_drv_names, "*");

	return 1;
}
__setup("driver_async_probe=", save_async_options);

bool driver_allows_async_probing(struct device_driver *drv)
{
	switch (drv->probe_type) {
	case PROBE_PREFER_ASYNCHRONOUS:
		return true;

	case PROBE_FORCE_SYNCHRONOUS:
		return false;

	default:
		if (cmdline_requested_async_probing(drv->name))
			return true;

		if (module_requested_async_probing(drv->owner))
			return true;

		return false;
	}
}

struct device_attach_data {
	struct device *dev;

	/*
	 * Indicates whether we are considering asynchronous probing or
	 * not. Only initial binding after device or driver registration
	 * (including deferral processing) may be done asynchronously, the
	 * rest is always synchronous, as we expect it is being done by
	 * request from userspace.
	 */
	bool check_async;

	/*
	 * Indicates if we are binding synchronous or asynchronous drivers.
	 * When asynchronous probing is enabled we'll execute 2 passes
	 * over drivers: first pass doing synchronous probing and second
	 * doing asynchronous probing (if synchronous did not succeed -
	 * most likely because there was no driver requiring synchronous
	 * probing - and we found asynchronous driver during first pass).
	 * The 2 passes are done because we can't shoot asynchronous
	 * probe for given device and driver from bus_for_each_drv() since
	 * driver pointer is not guaranteed to stay valid once
	 * bus_for_each_drv() iterates to the next driver on the bus.
	 */
	bool want_async;

	/*
	 * We'll set have_async to 'true' if, while scanning for matching
	 * driver, we'll encounter one that requests asynchronous probing.
	 */
	bool have_async;
};

static int __device_attach_driver(struct device_driver *drv, void *_data)
{
	struct device_attach_data *data = _data;
	struct device *dev = data->dev;
	bool async_allowed;
	int ret;

	ret = driver_match_device(drv, dev);
	if (ret == 0) {
		/* no match */
		return 0;
	} else if (ret == -EPROBE_DEFER) {
		dev_dbg(dev, "Device match requests probe deferral\n");
		dev->can_match = true;
		driver_deferred_probe_add(dev);
		/*
		 * Device can't match with a driver right now, so don't attempt
		 * to match or bind with other drivers on the bus.
		 */
		return ret;
	} else if (ret < 0) {
		dev_dbg(dev, "Bus failed to match device: %d\n", ret);
		return ret;
	} /* ret > 0 means positive match */

	async_allowed = driver_allows_async_probing(drv);

	if (async_allowed)
		data->have_async = true;

	if (data->check_async && async_allowed != data->want_async)
		return 0;

	/*
	 * Ignore errors returned by ->probe so that the next driver can try
	 * its luck.
	 */
	ret = driver_probe_device(drv, dev);
	if (ret < 0)
		return ret;
	return ret == 0;
}

static void __device_attach_async_helper(void *_dev, async_cookie_t cookie)
{
	struct device *dev = _dev;
	struct device_attach_data data = {
		.dev		= dev,
		.check_async	= true,
		.want_async	= true,
	};

	device_lock(dev);

	/*
	 * Check if device has already been removed or claimed. This may
	 * happen with driver loading, device discovery/registration,
	 * and deferred probe processing happens all at once with
	 * multiple threads.
	 */
	if (dev->p->dead || dev->driver)
		goto out_unlock;

	if (dev->parent)
		pm_runtime_get_sync(dev->parent);

	bus_for_each_drv(dev->bus, NULL, &data, __device_attach_driver);
	dev_dbg(dev, "async probe completed\n");

	pm_request_idle(dev);

	if (dev->parent)
		pm_runtime_put(dev->parent);
out_unlock:
	device_unlock(dev);

	put_device(dev);
}

static int __device_attach(struct device *dev, bool allow_async)
{
	int ret = 0;
	bool async = false;

	device_lock(dev);
	if (dev->p->dead) {
		goto out_unlock;
	} else if (dev->driver) {
		if (device_is_bound(dev)) {
			ret = 1;
			goto out_unlock;
		}
		ret = device_bind_driver(dev);
		if (ret == 0)
			ret = 1;
		else {
			dev->driver = NULL;
			ret = 0;
		}
	} else {
		struct device_attach_data data = {
			.dev = dev,
			.check_async = allow_async,
			.want_async = false,
		};

		if (dev->parent)
			pm_runtime_get_sync(dev->parent);

		ret = bus_for_each_drv(dev->bus, NULL, &data,
					__device_attach_driver);
		if (!ret && allow_async && data.have_async) {
			/*
			 * If we could not find appropriate driver
			 * synchronously and we are allowed to do
			 * async probes and there are drivers that
			 * want to probe asynchronously, we'll
			 * try them.
			 */
			dev_dbg(dev, "scheduling asynchronous probe\n");
			get_device(dev);
			async = true;
		} else {
			pm_request_idle(dev);
		}

		if (dev->parent)
			pm_runtime_put(dev->parent);
	}
out_unlock:
	device_unlock(dev);
	if (async)
		async_schedule_dev(__device_attach_async_helper, dev);
	return ret;
}

/**
 * device_attach - try to attach device to a driver.
 * @dev: device.
 *
 * Walk the list of drivers that the bus has and call
 * driver_probe_device() for each pair. If a compatible
 * pair is found, break out and return.
 *
 * Returns 1 if the device was bound to a driver;
 * 0 if no matching driver was found;
 * -ENODEV if the device is not registered.
 *
 * When called for a USB interface, @dev->parent lock must be held.
 */
int device_attach(struct device *dev)
{
	return __device_attach(dev, false);
}
EXPORT_SYMBOL_GPL(device_attach);

void device_initial_probe(struct device *dev)
{
	__device_attach(dev, true);
}

/*
 * __device_driver_lock - acquire locks needed to manipulate dev->drv
 * @dev: Device we will update driver info for
 * @parent: Parent device. Needed if the bus requires parent lock
 *
 * This function will take the required locks for manipulating dev->drv.
 * Normally this will just be the @dev lock, but when called for a USB
 * interface, @parent lock will be held as well.
 */
static void __device_driver_lock(struct device *dev, struct device *parent)
{
	if (parent && dev->bus->need_parent_lock)
		device_lock(parent);
	device_lock(dev);
}

/*
 * __device_driver_unlock - release locks needed to manipulate dev->drv
 * @dev: Device we will update driver info for
 * @parent: Parent device. Needed if the bus requires parent lock
 *
 * This function will release the required locks for manipulating dev->drv.
 * Normally this will just be the @dev lock, but when called for a
 * USB interface, @parent lock will be released as well.
 */
static void __device_driver_unlock(struct device *dev, struct device *parent)
{
	device_unlock(dev);
	if (parent && dev->bus->need_parent_lock)
		device_unlock(parent);
}

/**
 * device_driver_attach - attach a specific driver to a specific device
 * @drv: Driver to attach
 * @dev: Device to attach it to
 *
 * Manually attach driver to a device. Will acquire both @dev lock and
 * @dev->parent lock if needed. Returns 0 on success, -ERR on failure.
 */
int device_driver_attach(struct device_driver *drv, struct device *dev)
{
	int ret;

	__device_driver_lock(dev, dev->parent);
	ret = __driver_probe_device(drv, dev);
	__device_driver_unlock(dev, dev->parent);

	/* also return probe errors as normal negative errnos */
	if (ret > 0)
		ret = -ret;
	if (ret == -EPROBE_DEFER)
		return -EAGAIN;
	return ret;
}
EXPORT_SYMBOL_GPL(device_driver_attach);

static void __driver_attach_async_helper(void *_dev, async_cookie_t cookie)
{
	struct device *dev = _dev;
	struct device_driver *drv;
	int ret;

	__device_driver_lock(dev, dev->parent);
	drv = dev->p->async_driver;
	dev->p->async_driver = NULL;
	ret = driver_probe_device(drv, dev);
	__device_driver_unlock(dev, dev->parent);

	dev_dbg(dev, "driver %s async attach completed: %d\n", drv->name, ret);

	put_device(dev);
}

static int __driver_attach(struct device *dev, void *data)
{
	struct device_driver *drv = data;
	bool async = false;
	int ret;

	/*
	 * Lock device and try to bind to it. We drop the error
	 * here and always return 0, because we need to keep trying
	 * to bind to devices and some drivers will return an error
	 * simply if it didn't support the device.
	 *
	 * driver_probe_device() will spit a warning if there
	 * is an error.
	 */

	ret = driver_match_device(drv, dev);
	if (ret == 0) {
		/* no match */
		return 0;
	} else if (ret == -EPROBE_DEFER) {
		dev_dbg(dev, "Device match requests probe deferral\n");
		dev->can_match = true;
		driver_deferred_probe_add(dev);
		/*
		 * Driver could not match with device, but may match with
		 * another device on the bus.
		 */
		return 0;
	} else if (ret < 0) {
		dev_dbg(dev, "Bus failed to match device: %d\n", ret);
		/*
		 * Driver could not match with device, but may match with
		 * another device on the bus.
		 */
		return 0;
	} /* ret > 0 means positive match */

	if (driver_allows_async_probing(drv)) {
		/*
		 * Instead of probing the device synchronously we will
		 * probe it asynchronously to allow for more parallelism.
		 *
		 * We only take the device lock here in order to guarantee
		 * that the dev->driver and async_driver fields are protected
		 */
		dev_dbg(dev, "probing driver %s asynchronously\n", drv->name);
		device_lock(dev);
		if (!dev->driver && !dev->p->async_driver) {
			get_device(dev);
			dev->p->async_driver = drv;
			async = true;
		}
		device_unlock(dev);
		if (async)
			async_schedule_dev(__driver_attach_async_helper, dev);
		return 0;
	}

	__device_driver_lock(dev, dev->parent);
	driver_probe_device(drv, dev);
	__device_driver_unlock(dev, dev->parent);

	return 0;
}

/**
 * driver_attach - try to bind driver to devices.
 * @drv: driver.
 *
 * Walk the list of devices that the bus has on it and try to
 * match the driver with each one.  If driver_probe_device()
 * returns 0 and the @dev->driver is set, we've found a
 * compatible pair.
 */
int driver_attach(struct device_driver *drv)
{
	return bus_for_each_dev(drv->bus, NULL, drv, __driver_attach);
}
EXPORT_SYMBOL_GPL(driver_attach);

/*
 * __device_release_driver() must be called with @dev lock held.
 * When called for a USB interface, @dev->parent lock must be held as well.
 */
static void __device_release_driver(struct device *dev, struct device *parent)
{
	struct device_driver *drv;

	drv = dev->driver;
	if (drv) {
		pm_runtime_get_sync(dev);

		while (device_links_busy(dev)) {
			__device_driver_unlock(dev, parent);

			device_links_unbind_consumers(dev);

			__device_driver_lock(dev, parent);
			/*
			 * A concurrent invocation of the same function might
			 * have released the driver successfully while this one
			 * was waiting, so check for that.
			 */
			if (dev->driver != drv) {
				pm_runtime_put(dev);
				return;
			}
		}

		driver_sysfs_remove(dev);

		if (dev->bus)
			blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
						     BUS_NOTIFY_UNBIND_DRIVER,
						     dev);

		pm_runtime_put_sync(dev);

		device_remove(dev);

		if (dev->bus && dev->bus->dma_cleanup)
			dev->bus->dma_cleanup(dev);

		device_unbind_cleanup(dev);
		device_links_driver_cleanup(dev);

		klist_remove(&dev->p->knode_driver);
		device_pm_check_callbacks(dev);
		if (dev->bus)
			blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
						     BUS_NOTIFY_UNBOUND_DRIVER,
						     dev);

		kobject_uevent(&dev->kobj, KOBJ_UNBIND);
	}
}

void device_release_driver_internal(struct device *dev,
				    struct device_driver *drv,
				    struct device *parent)
{
	__device_driver_lock(dev, parent);

	if (!drv || drv == dev->driver)
		__device_release_driver(dev, parent);

	__device_driver_unlock(dev, parent);
}

/**
 * device_release_driver - manually detach device from driver.
 * @dev: device.
 *
 * Manually detach device from driver.
 * When called for a USB interface, @dev->parent lock must be held.
 *
 * If this function is to be called with @dev->parent lock held, ensure that
 * the device's consumers are unbound in advance or that their locks can be
 * acquired under the @dev->parent lock.
 */
void device_release_driver(struct device *dev)
{
	/*
	 * If anyone calls device_release_driver() recursively from
	 * within their ->remove callback for the same device, they
	 * will deadlock right here.
	 */
	device_release_driver_internal(dev, NULL, NULL);
}
EXPORT_SYMBOL_GPL(device_release_driver);

/**
 * device_driver_detach - detach driver from a specific device
 * @dev: device to detach driver from
 *
 * Detach driver from device. Will acquire both @dev lock and @dev->parent
 * lock if needed.
 */
void device_driver_detach(struct device *dev)
{
	device_release_driver_internal(dev, NULL, dev->parent);
}

/**
 * driver_detach - detach driver from all devices it controls.
 * @drv: driver.
 */
void driver_detach(struct device_driver *drv)
{
	struct device_private *dev_prv;
	struct device *dev;

	if (driver_allows_async_probing(drv))
		async_synchronize_full();

	for (;;) {
		spin_lock(&drv->p->klist_devices.k_lock);
		if (list_empty(&drv->p->klist_devices.k_list)) {
			spin_unlock(&drv->p->klist_devices.k_lock);
			break;
		}
		dev_prv = list_last_entry(&drv->p->klist_devices.k_list,
				     struct device_private,
				     knode_driver.n_node);
		dev = dev_prv->device;
		get_device(dev);
		spin_unlock(&drv->p->klist_devices.k_lock);
		device_release_driver_internal(dev, drv, dev->parent);
		put_device(dev);
	}
}

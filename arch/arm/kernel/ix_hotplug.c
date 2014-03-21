/* Copyright (c) 2013, Steve Loebrich <sloebric@gmail.com>. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

/*
 * Generic auto hotplug driver for ARM SoCs. Targeted at current generation
 * SoCs with dual and quad core applications processors.
 * Automatically hotplugs online and offline CPUs based on system load.
 * It is also capable of immediately onlining a core based on an external
 * event by calling void hotplug_boostpulse(void)
 *
 * Not recommended for use with OMAP4460 due to the potential for lockups
 * whilst hotplugging.
 * 
 * Thanks to Thalamus for the inspiration!
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/cpufreq.h>
//#include <linux/kernel_stat.h>
//#include <linux/tick.h>
//#include <linux/rq_stats.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#define IX_HOTPLUG "ix_hotplug"

/*
 * Load defines:
 * ENABLE_ALL is a high watermark to rapidly online all CPUs
 *
 * ENABLE is the load which is required to enable 1 extra CPU
 * DISABLE is the load at which a CPU is disabled
 * These two are scaled based on num_online_cpus()
 */

static DEFINE_MUTEX(ix_hotplug_mutex);

static struct delayed_work hotplug_decision_work;
static struct work_struct suspend;
static struct work_struct resume;

static unsigned int enable_all_load = 800;
static unsigned int enable_load[5] = {0, 100, 220, 320, 0};
static unsigned int disable_load[5] = {0, 0, 30, 160, 240};
static unsigned int sample_rate[5] = {0, 50, 150, 100, 50};
static unsigned int load_enable;
static unsigned int load_disable;
static unsigned int sampling_rate;
static unsigned int load_multiplier = 1;
static unsigned int available_cpus = 4;
static unsigned int online_sample = 1;
static unsigned int offline_sample = 1;
static unsigned int online_sampling_periods = 3;
//static unsigned int offline_sampling_periods = 6;
static unsigned int offline_sampling_periods[5] = {0, 0, 10, 5, 4};
static unsigned int online_cpus;
static unsigned int min_cpus_online = 1;

static void hotplug_online_single_work(void)
{
	cpu_up(online_cpus);
	pr_info("ix_hotplug: CPU%d up.\n", online_cpus);
	return;
}

static void hotplug_online_all_work(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (likely(!cpu_online(cpu))) {
			cpu_up(cpu);
			pr_info("ix_hotplug: CPU%d up.\n", cpu);
		}
	}
	return;
}

static void hotplug_offline_work(void)
{
	cpu_down(online_cpus - 1);
	pr_info("ix_hotplug: CPU%d down.\n", (online_cpus - 1));
	return;
}

static void __ref hotplug_decision_work_fn(struct work_struct *work)
{
	unsigned int avg_running, io_wait;
	//unsigned int rq_depth;
	
	sched_get_nr_running_avg(&avg_running, &io_wait);
	//rq_depth = rq_info.rq_avg;
	
	load_disable = disable_load[online_cpus];

	if (avg_running <= load_disable && online_cpus > min_cpus_online) {
		if (offline_sample >= offline_sampling_periods[online_cpus]) {
			hotplug_offline_work();
			offline_sample = 1;
			//pr_info("ix_hotplug: Threshold: %d Load: %d Sampling: %d RQ: %d\n", load_disable, avg_running, sampling_rate, rq_depth);
		} else {
			offline_sample++;
		}
		online_sample = 1;
		goto exit;
	}

	if (online_cpus < available_cpus) {

		if (avg_running >= enable_all_load) {
			hotplug_online_all_work();
			//pr_info("ix_hotplug: Threshold: %d Load: %d Sampling: %d RQ: %d\n", enable_all_load, avg_running, sampling_rate, rq_depth);
			offline_sample = 1;
			goto exit;
		}				

		load_enable = enable_load[online_cpus] * load_multiplier;

		if (avg_running >= load_enable) {
			if (online_sample >= online_sampling_periods) { 
				hotplug_online_single_work();
				online_sample = 1;
				//pr_info("ix_hotplug: Threshold: %d Load: %d Sampling: %d RQ: %d\n", load_enable, avg_running, sampling_rate, rq_depth);
			} else {
				online_sample++;
			}
			offline_sample = 1;
		}
	}

exit:

	online_cpus = num_online_cpus();

	sampling_rate = sample_rate[online_cpus] * load_multiplier;
	
	//pr_info("ix_hotplug: CPUs: %d Load: %d Sampling: %d RQ: %d\n", online_cpus, avg_running, sampling_rate, rq_depth);
	
	schedule_delayed_work_on(0, &hotplug_decision_work, msecs_to_jiffies(sampling_rate));
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void ix_hotplug_suspend(struct work_struct *work)
{
	mutex_lock(&ix_hotplug_mutex);
	load_multiplier = 3;
	min_cpus_online = 1;
	mutex_unlock(&ix_hotplug_mutex);
	
	//pr_info("ix_hotplug: early suspend handler\n");
}

static void __ref ix_hotplug_resume(struct work_struct *work)
{
	mutex_lock(&ix_hotplug_mutex);
	load_multiplier = 1;
	min_cpus_online = 1;
	mutex_unlock(&ix_hotplug_mutex);

	//pr_info("ix_hotplug: late resume handler\n");
}

static void ix_hotplug_early_suspend(struct early_suspend *handler)
{
	schedule_work(&suspend);
}

static void ix_hotplug_late_resume(struct early_suspend *handler)
{
	schedule_work(&resume);
}

static struct early_suspend early_suspend = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 20,
	.suspend = ix_hotplug_early_suspend,
	.resume = ix_hotplug_late_resume,
};
#endif /* CONFIG_HAS_EARLYSUSPEND */

static int __devinit ix_hotplug_probe(struct platform_device *pdev)
{	
	online_cpus = num_online_cpus();
	
#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&early_suspend);
#endif
	
	INIT_WORK(&suspend, ix_hotplug_suspend);
	INIT_WORK(&resume, ix_hotplug_resume);
	INIT_DELAYED_WORK(&hotplug_decision_work, hotplug_decision_work_fn);

	/*
	 * Give the system time to boot before fiddling with hotplugging.
	 */

	schedule_delayed_work_on(0, &hotplug_decision_work, msecs_to_jiffies(10000));

	pr_info("ix_hotplug: v1.0 - InstigatorX\n");
	pr_info("ix_hotplug: based on v0.220 by _thalamus\n");
	
	return 0;
}

static struct platform_device ix_hotplug_device = {
	.name = IX_HOTPLUG,
	.id = -1,
};

static int ix_hotplug_remove(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&hotplug_decision_work);
	
	return 0;
}

static struct platform_driver ix_hotplug_driver = {
	.probe = ix_hotplug_probe,
	.remove = ix_hotplug_remove,
	.driver = {
		.name = IX_HOTPLUG,
		.owner = THIS_MODULE,
	},
};

static int __init ix_hotplug_init(void)
{
	int ret;

	ret = platform_driver_register(&ix_hotplug_driver);

	if (ret)
	{
		return ret;
	}

	ret = platform_device_register(&ix_hotplug_device);

	if (ret)
	{
		return ret;
	}

	pr_info("%s: init\n", IX_HOTPLUG);

	return ret;
}

static void __exit ix_hotplug_exit(void)
{
	platform_device_unregister(&ix_hotplug_device);
	platform_driver_unregister(&ix_hotplug_driver);
}

late_initcall(ix_hotplug_init);
module_exit(ix_hotplug_exit);

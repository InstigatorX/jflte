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
#include "../../../kernel/sched/sched.h"

#include <mach/cpufreq.h>

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
static struct workqueue_struct *ixwq;

static unsigned int enable_all_load = 700;
static unsigned int enable_load[5] = {200, 200, 235, 300, 4000};
static unsigned int sample_rate[5] = {100, 50, 100, 150, 100};
static unsigned int disable_load = 70;
static unsigned int sampling_rate = 100;
static unsigned int load_multiplier = 1;
static unsigned int available_cpus = 4;
static unsigned int online_sample = 1;
static unsigned int offline_sample = 1;
static unsigned int online_sampling_periods = 3;
static unsigned int offline_sampling_periods = 5;

static void hotplug_online_single_work(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (cpu) {
			if (!cpu_online(cpu)) {
				cpu_up(cpu);
				pr_info("ix_hotplug: CPU%d up.\n", cpu);
				break;
			}
		}
	}
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
	int cpu; 
	unsigned long load, min_load = ULONG_MAX;
	int idlest = -1;
	
	for_each_online_cpu(cpu) {
		if (cpu) {
			load = cpu_rq(cpu)->load.weight;
			//pr_info("ix_hotplug: load.weight %ld\n", load);
			if (load < min_load) {
				min_load = load;
				idlest = cpu;
			}
		}
	}
	cpu_down(idlest);
	pr_info("ix_hotplug: CPU%d down.\n", idlest);

	return;
}

static void __ref hotplug_decision_work_fn(struct work_struct *work)
{
	unsigned int online_cpus, load_enable;
	unsigned int avg_running, io_wait;

	online_cpus = num_online_cpus();
	load_enable = enable_load[online_cpus] * load_multiplier;
	
	sched_get_nr_running_avg(&avg_running, &io_wait);

	if (avg_running >= enable_all_load && online_cpus < available_cpus) {
		pr_info("ix_hotplug: Onlining all CPUs, Avg Running: %d Sample Rate: %d IOWait: %d\n", avg_running, sampling_rate, io_wait);
		hotplug_online_all_work();
	} else if (avg_running >= load_enable && online_cpus < available_cpus) {
		if (online_sample >= online_sampling_periods) { 
			hotplug_online_single_work();
			online_sample = 1;
			pr_info("ix_hotplug: Avg Running: %d Sample Rate: %d IOWait: %d\n", avg_running, sampling_rate, io_wait);
		} else {
			online_sample++;
		}
		offline_sample = 1;
	} else if (avg_running <= disable_load && online_cpus > 1) {
		if (offline_sample >= offline_sampling_periods) {
			if (io_wait == 0) { 
				hotplug_offline_work();
				offline_sample = 1;
			}
			pr_info("ix_hotplug: Avg Running: %d Sample Rate: %d IOWait: %d\n", avg_running, sampling_rate, io_wait);
		} else {
			offline_sample++;
		}
		online_sample = 1;
	}
	
	sampling_rate = sample_rate[online_cpus] * load_multiplier;

	queue_delayed_work_on(0, ixwq, &hotplug_decision_work, msecs_to_jiffies(sampling_rate));
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void ix_hotplug_suspend(struct work_struct *work)
{
	mutex_lock(&ix_hotplug_mutex);
	load_multiplier = 2;
	mutex_unlock(&ix_hotplug_mutex);
	
	pr_info("ix_hotplug: early suspend handler\n");
}

static void __ref ix_hotplug_resume(struct work_struct *work)
{
	mutex_lock(&ix_hotplug_mutex);
	load_multiplier = 1;
	mutex_unlock(&ix_hotplug_mutex);

	pr_info("ix_hotplug: late resume handler\n");
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
	ixwq = create_singlethread_workqueue("ix_hotplug_workqueue");
    
    if (!ixwq)
        return -ENOMEM;
	
#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&early_suspend);
#endif
	
	INIT_WORK(&suspend, ix_hotplug_suspend);
	INIT_WORK(&resume, ix_hotplug_resume);
	INIT_DELAYED_WORK(&hotplug_decision_work, hotplug_decision_work_fn);

	/*
	 * Give the system time to boot before fiddling with hotplugging.
	 */

	queue_delayed_work_on(0, ixwq, &hotplug_decision_work, msecs_to_jiffies(10000));

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
	destroy_workqueue(ixwq);

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

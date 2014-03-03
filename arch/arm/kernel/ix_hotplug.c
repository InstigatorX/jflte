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
#include <linux/platform_device.h>
#include <linux/cpufreq.h>

#include <mach/cpufreq.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#define IX_HOTPLUG "ix_hotplug"
#define CPUS_AVAILABLE		4

/*
 * Load defines:
 * ENABLE_ALL is a high watermark to rapidly online all CPUs
 *
 * ENABLE is the load which is required to enable 1 extra CPU
 * DISABLE is the load at which a CPU is disabled
 * These two are scaled based on num_online_cpus()
 */
#define ENABLE_ALL_LOAD_THRESHOLD	600
#define ENABLE_LOAD_THRESHOLD		200
#define DISABLE_LOAD_THRESHOLD		70

#define ONLINE_SAMPLING_PERIODS		3
#define OFFLINE_SAMPLING_PERIODS	5
#define SAMPLING_RATE				100

static struct delayed_work hotplug_decision_work;
static struct work_struct suspend;
static struct work_struct resume;
static struct workqueue_struct *ixwq;

static unsigned int enable_all_load = ENABLE_ALL_LOAD_THRESHOLD;
static unsigned int enable_load[5] = {200, 200, 235, 300, 4000};
static unsigned int disable_load = DISABLE_LOAD_THRESHOLD;
static unsigned int sampling_rate = SAMPLING_RATE;

static unsigned int available_cpus = CPUS_AVAILABLE;

static unsigned int online_sample = 1;
static unsigned int offline_sample = 1;
static unsigned int online_sampling_periods = ONLINE_SAMPLING_PERIODS;
static unsigned int offline_sampling_periods = OFFLINE_SAMPLING_PERIODS;

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
	for_each_online_cpu(cpu) {
		if (cpu) {
			cpu_down(cpu);
			pr_info("ix_hotplug: CPU%d down.\n", cpu);
			break;
		}
	}
	return;
}

static void __ref hotplug_decision_work_fn(struct work_struct *work)
{
	unsigned int online_cpus;
	unsigned int avg_running, io_wait;

	online_cpus = num_online_cpus();

	sched_get_nr_running_avg(&avg_running, &io_wait);
	
	if (unlikely((avg_running >= enable_all_load) && (online_cpus < available_cpus))) {
		pr_info("ix_hotplug: Onlining all CPUs, avg running: %d\n", avg_running);
		hotplug_online_all_work();
	} else if ((avg_running >= enable_load[online_cpus]) && (online_cpus < available_cpus)) {
		if (online_sample >= online_sampling_periods) { 
			hotplug_online_single_work();
			sampling_rate = 50 * online_cpus + 50;
			online_sample = 1;
			pr_info("ix_hotplug: Avg Running: %d Sample Rate: %d\n", avg_running, sampling_rate);
		} else {
			online_sample++;
		}
		offline_sample = 1;
	} else if (avg_running <= disable_load && (online_cpus > 1)) {
		if (offline_sample >= offline_sampling_periods) {
			sampling_rate = 50 * online_cpus;
			hotplug_offline_work();
			offline_sample = 1;
			pr_info("ix_hotplug: Avg Running: %d Sample Rate: %d\n", avg_running, sampling_rate);
		} else {
			offline_sample++;
		}
		online_sample = 1;
	}
	
	queue_delayed_work_on(0, ixwq, &hotplug_decision_work, msecs_to_jiffies(sampling_rate));
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void ix_hotplug_suspend(struct work_struct *work)
{
	int cpu;

	pr_info("ix_hotplug: early suspend handler\n");

	for_each_online_cpu(cpu)
	{
		if (!cpu)
			continue;

		cpu_down(cpu);
	}
}

static void __ref ix_hotplug_resume(struct work_struct *work)
{
	int cpu;

	for_each_possible_cpu(cpu)
	{
		if (!cpu)
			continue;

		cpu_up(cpu);
	}
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
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = ix_hotplug_early_suspend,
	.resume = ix_hotplug_late_resume,
};
#endif /* CONFIG_HAS_EARLYSUSPEND */

static int __devinit ix_hotplug_probe(struct platform_device *pdev)
{
	int delay = usecs_to_jiffies(50000);
	
	ixwq = create_freezable_workqueue("ix_hotplug_workqueue");
    
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
	if (num_online_cpus() > 1)
		delay -= jiffies % delay;
		
	queue_delayed_work_on(0, ixwq, &hotplug_decision_work, delay);

	pr_info("ix_hotplug: v1.0 - InstigatorX\n");
	pr_info("ix_hotplug: based on v0.220 by _thalamus\n");
	pr_info("ix_hotplug: %d CPUs detected\n", CPUS_AVAILABLE);

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

MODULE_AUTHOR("Steve Loebrich <sloebric@gmail.com>");
MODULE_DESCRIPTION("ARM Hotplug Driver");
MODULE_LICENSE("GPL");

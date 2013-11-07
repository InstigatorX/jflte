/* Copyright (c) 2012, Will Tisdale <willtisdale@gmail.com>. All rights reserved.
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
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>

#include <mach/cpufreq.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

/*
 * Enable debug output to dump the average
 * calculations and ring buffer array values
 * WARNING: Enabling this causes a ton of overhead
 *
 * FIXME: Turn it into debugfs stats (somehow)
 * because currently it is a sack of shit.
 */
#define DEBUG 0

#define CPUS_AVAILABLE		num_possible_cpus()

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

struct delayed_work hotplug_decision_work;

static struct workqueue_struct *ixwq;

static unsigned int enable_all_load = ENABLE_ALL_LOAD_THRESHOLD;
static unsigned int enable_load = ENABLE_LOAD_THRESHOLD;
static unsigned int disable_load = DISABLE_LOAD_THRESHOLD;
static unsigned int sampling_rate = SAMPLING_RATE;

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
				pr_info("auto_hotplug: CPU%d up.\n", cpu);
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
			pr_info("auto_hotplug: CPU%d up.\n", cpu);
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
			pr_info("auto_hotplug: CPU%d down.\n", cpu);
			break;
		}
	}
	return;
}

static void hotplug_decision_work_fn(struct work_struct *work)
{
	unsigned int online_cpus, available_cpus;
	unsigned int avg_running, io_wait;

	online_cpus = num_online_cpus();
	available_cpus = CPUS_AVAILABLE;

	sched_get_nr_running_avg(&avg_running, &io_wait);
	
	if (unlikely((avg_running >= enable_all_load) && (online_cpus < available_cpus))) {
		pr_info("auto_hotplug: Onlining all CPUs, avg running: %d\n", avg_running);
		hotplug_online_all_work();
	} else if ((avg_running >= enable_load) && (online_cpus < available_cpus)) {
		if (online_sample >= online_sampling_periods) { 
			pr_info("auto_hotplug: Onlining single CPU, avg running: %d\n", avg_running);
			hotplug_online_single_work();
			sampling_rate = 200;
		} else {
			online_sample++;
		}
		offline_sample = 1;
	} else if (avg_running <= disable_load && (online_cpus > 1)) {
		if (offline_sample >= offline_sampling_periods) {
			pr_info("auto_hotplug: Offlining CPU, avg running: %d\n", avg_running);
			hotplug_offline_work();
			sampling_rate = 100;
		} else {
			offline_sample++;
		}
		online_sample = 1;
	}

	queue_delayed_work_on(0, ixwq, &hotplug_decision_work, msecs_to_jiffies(sampling_rate));
}

static ssize_t show_enable_all_load(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", enable_all_load);
}

static ssize_t store_enable_all_load(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	int ret;
	long unsigned int val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	enable_all_load = val;
	return count;
}

static struct global_attr enable_all_load_attr = __ATTR(enable_all_load, 0666,
		show_enable_all_load, store_enable_all_load);
		
static ssize_t show_enable_load(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", enable_load);
}
		
static ssize_t store_enable_load(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	int ret;
	long unsigned int val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	enable_load = val;
	return count;
}

static struct global_attr enable_load_attr = __ATTR(enable_load, 06666,
		show_enable_load, store_enable_load);
		
static ssize_t show_disable_load(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", disable_load);
}

static ssize_t store_disable_load(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	int ret;
	long unsigned int val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	disable_load = val;
	return count;
}

static struct global_attr disable_load_attr = __ATTR(disable_load, 0666,
		show_disable_load, store_disable_load);

static struct attribute *auto_hotplug_attributes[] = {
	&enable_all_load_attr.attr,
	&enable_load_attr.attr,
	&disable_load_attr.attr,
	NULL,
};

static struct attribute_group auto_hotplug_attr_group = {
	.attrs = auto_hotplug_attributes,
	.name = "auto_hotplug",
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void auto_hotplug_early_suspend(struct early_suspend *handler)
{
	int i;
	
	pr_info("auto_hotplug: early suspend handler\n");

	/* Cancel all scheduled delayed work to avoid races */
	cancel_delayed_work_sync(&hotplug_decision_work);
	flush_workqueue(ixwq);
	
	for (i = 3; i > 0; i--) {
		cpu_down(i);
	}
    pr_info("auto_hotplug: Offlining CPUs for early suspend\n");
}

static void auto_hotplug_late_resume(struct early_suspend *handler)
{
	int i;

	pr_info("auto_hotplug: late resume handler\n");

	for (i = 1; i < 4; i++) {
		cpu_up(i);
	}
	queue_delayed_work_on(0, ixwq, &hotplug_decision_work, msecs_to_jiffies(10));
}

static struct early_suspend auto_hotplug_suspend = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = auto_hotplug_early_suspend,
	.resume = auto_hotplug_late_resume,
};
#endif /* CONFIG_HAS_EARLYSUSPEND */

static int __init auto_hotplug_init(void)
{
	int rc;

	int delay = usecs_to_jiffies(sampling_rate);

	pr_info("iX_auto_hotplug: based on v0.220 by _thalamus\n");
	pr_info("iX_auto_hotplug: %d CPUs detected\n", CPUS_AVAILABLE);


	if (num_online_cpus() > 1)
		delay -= jiffies % delay;
		
	ixwq = create_freezable_workqueue("auto_hotplug_workqueue");
    
    if (!ixwq)
        return -ENOMEM;
	
	rc = sysfs_create_group(cpufreq_global_kobject,
				&auto_hotplug_attr_group);
	
	INIT_DELAYED_WORK(&hotplug_decision_work, hotplug_decision_work_fn);

	/*
	 * Give the system time to boot before fiddling with hotplugging.
	 */
	queue_delayed_work_on(0, ixwq, &hotplug_decision_work, delay);

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&auto_hotplug_suspend);
#endif

	return 0;
}
late_initcall(auto_hotplug_init);

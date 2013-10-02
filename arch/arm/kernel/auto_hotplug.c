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
 * SAMPLING_PERIODS * MIN_SAMPLING_RATE is the minimum
 * load history which will be averaged
 */
#define SAMPLING_PERIODS	10
#define INDEX_MAX_VALUE		(SAMPLING_PERIODS - 1)
/*
 * MIN_SAMPLING_RATE is scaled based on num_online_cpus()
 */
#define MIN_SAMPLING_RATE	100

/*
 * Load defines:
 * ENABLE_ALL is a high watermark to rapidly online all CPUs
 *
 * ENABLE is the load which is required to enable 1 extra CPU
 * DISABLE is the load at which a CPU is disabled
 * These two are scaled based on num_online_cpus()
 */
#define ENABLE_ALL_LOAD_THRESHOLD	600
#define ENABLE_LOAD_THRESHOLD		275
#define DISABLE_LOAD_THRESHOLD		125

struct delayed_work hotplug_decision_work;

static struct workqueue_struct *ixwq;

static unsigned int history[SAMPLING_PERIODS];
static unsigned int index;

static unsigned int enable_all_load = ENABLE_ALL_LOAD_THRESHOLD;
static unsigned int enable_load = ENABLE_LOAD_THRESHOLD;
static unsigned int disable_load = DISABLE_LOAD_THRESHOLD;
static unsigned int sampling_rate = MIN_SAMPLING_RATE;

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
	unsigned int running, cur_disable_load, cur_enable_load;
	unsigned int avg_running = 0, online_cpus, available_cpus, i, j;
#if DEBUG
	unsigned int k;
#endif

	online_cpus = num_online_cpus();
	available_cpus = CPUS_AVAILABLE;
	cur_disable_load = disable_load * online_cpus;
	cur_enable_load = enable_load * online_cpus;

	/*
	 * Multiply nr_running() by 100 so we don't have to
	 * use fp division to get the average.
	 */
	running = nr_running() * 100;

	history[index] = running;

#if DEBUG
	pr_info("online_cpus is: %d\n", online_cpus);
	pr_info("cur_enable_load is: %d\n", cur_enable_load);
	pr_info("cur_disable_load is: %d\n", cur_disable_load);
	pr_info("index is: %d\n", index);
	pr_info("running is: %d\n", running);
#endif

	/*
	 * Use a circular buffer to calculate the average load
	 * over the sampling periods.
	 * This will absorb load spikes of short duration where
	 * we don't want additional cores to be onlined because
	 * the cpufreq driver should take care of those load spikes.
	 */
	for (i = 0, j = index; i < SAMPLING_PERIODS; i++, j--) {
		avg_running += history[j];
		if (unlikely(j == 0))
			j = INDEX_MAX_VALUE;
	}

	/*
	 * If we are at the end of the buffer, return to the beginning.
	 */
	if (unlikely(index++ == INDEX_MAX_VALUE))
		index = 0;

#if DEBUG
	pr_info("array contents: ");
	for (k = 0; k < SAMPLING_PERIODS; k++) {
		 pr_info("%d: %d\t",k, history[k]);
	}
	pr_info("\n");
	pr_info("avg_running before division: %d\n", avg_running);
#endif

	avg_running = avg_running / SAMPLING_PERIODS;

#if DEBUG
	pr_info("average_running is: %d\n", avg_running);
#endif

	if (unlikely((avg_running >= enable_all_load) && (online_cpus < available_cpus))) {
		pr_info("auto_hotplug: Onlining all CPUs, avg running: %d\n", avg_running);
		hotplug_online_all_work();
	} else if ((avg_running >= cur_enable_load) && (online_cpus < available_cpus)) {
		pr_info("auto_hotplug: Onlining single CPU, avg running: %d\n", avg_running);
		hotplug_online_single_work();
	} else if (avg_running <= cur_disable_load) {
		pr_info("auto_hotplug: Offlining CPU, avg running: %d\n", avg_running);
		hotplug_offline_work();
		/* If boostpulse is active, clear the flags */
	}

	/*
	 * Reduce the sampling rate dynamically based on online cpus.
	 */

#if DEBUG
	pr_info("sampling_rate is: %dms\n", jiffies_to_msecs(sampling_rate));
#endif

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

static struct global_attr enable_all_load_attr = __ATTR(enable_all_load, 0644,
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

static struct global_attr enable_load_attr = __ATTR(enable_load, 0644,
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

static struct global_attr disable_load_attr = __ATTR(disable_load, 0644,
		show_disable_load, store_disable_load);

static ssize_t show_sampling_rate(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", sampling_rate);
}

static ssize_t store_sampling_rate(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	int ret;
	long unsigned int val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	sampling_rate = val;
	return count;
}

static struct global_attr sampling_rate_attr = __ATTR(sampling_rate, 0644,
		show_sampling_rate, store_sampling_rate);

static struct attribute *auto_hotplug_attributes[] = {
	&enable_all_load_attr.attr,
	&enable_load_attr.attr,
	&disable_load_attr.attr,
	&sampling_rate_attr.attr,
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

	int delay = usecs_to_jiffies(MIN_SAMPLING_RATE);

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

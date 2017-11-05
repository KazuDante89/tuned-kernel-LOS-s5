/*
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/rq_stats.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>

#define DEF_SAMPLING_MS			(50)

static struct delayed_work tuned_plug_work;
static struct workqueue_struct *tunedplug_wq;

static unsigned int tuned_plug_active = 1;
module_param(tuned_plug_active, uint, 0644);

static unsigned int sampling_time = 0;
static unsigned int toolow = 0;

static void inline __cpuinit tuned_plug_work_fn(struct work_struct *work)
{
	static unsigned int nr_run_stat, nr_cpus, i, i2;
        static struct cpufreq_policy policy;

	queue_delayed_work_on(0, tunedplug_wq, &tuned_plug_work, sampling_time);

	if (!tuned_plug_active)
		return;

	nr_run_stat = avg_nr_running();
	nr_cpus = num_online_cpus();

//	pr_info("nr_run_stat: %d .. nr_cpus: %d .. toolow: %d", nr_run_stat, nr_cpus, toolow);

        if (((nr_run_stat < 2000) && (nr_cpus > 1)) || toolow > 300)  {
		if (toolow > 300) toolow=0;
                for (i2 = NR_CPUS; i2 > 0; --i2) {
                        if (cpu_online(i2)) {
                                cpu_down(i2);
				pr_info("tunedplug: DOWN cpu %d . nr_run_stat: %d", i2, nr_run_stat);
				msleep_interruptible(1000);
                                break;
                        }
                }
	}
	else if (nr_cpus < NR_CPUS) {
	        for_each_online_cpu(i) {
	                if (cpufreq_get_policy(&policy, i) != 0)
	                        continue;
	                if (policy.cur >= policy.max) {
				for (i2 = 1; i2 < NR_CPUS; i2++) {
					if (!cpu_online(i2)) {
						cpu_up(i2);
						pr_info("tunedplug: UP cpu %d . nr_run_stat: %d", i2, nr_run_stat);
						toolow=0;
						if (nr_run_stat < 4000)	break;
					}
				}
			}
			else toolow++;
		}
	}
	else { msleep_interruptible(1000); }

}


int __init tuned_plug_init(void)
{

	tunedplug_wq = alloc_workqueue("tunedplug", WQ_HIGHPRI | WQ_UNBOUND, 1);

	INIT_DELAYED_WORK(&tuned_plug_work, tuned_plug_work_fn);

	sampling_time = msecs_to_jiffies(DEF_SAMPLING_MS);

	queue_delayed_work_on(0, tunedplug_wq, &tuned_plug_work, msecs_to_jiffies(10000));

	return 0;
}

MODULE_AUTHOR("Heiler Bemerguy <heiler.bemerguy@gmail.com>");
MODULE_DESCRIPTION("'tuned_plug' - A simple cpu hotplug driver");
MODULE_LICENSE("GPL");

late_initcall(tuned_plug_init);

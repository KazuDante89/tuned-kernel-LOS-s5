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
#include <linux/lcd_notify.h>

#define DEF_SAMPLING msecs_to_jiffies(50)

static struct notifier_block lcd_notif;
static struct delayed_work tuned_plug_work;
static struct workqueue_struct *tunedplug_wq;

static unsigned int tuned_plug_active = 1;
module_param(tuned_plug_active, uint, 0644);

static unsigned int sampling_time = 0;
static unsigned int lowthresh = 4000;

static bool displayon = true;

static void inline down_one(void){
	unsigned int i;
	for (i = NR_CPUS; i > 0; --i) {
		if (cpu_online(i)) {
			cpu_down(i);
			pr_info("tunedplug: DOWN cpu %d", i);
			msleep_interruptible(1000);
			break;
		}
	}
}

static void __cpuinit tuned_plug_work_fn(struct work_struct *work)
{
	unsigned int nr_run_stat, nr_cpus, i;

        queue_delayed_work_on(0, tunedplug_wq, &tuned_plug_work, sampling_time);

        if (!tuned_plug_active)
                return;

	if (!displayon) {
		if (lowthresh < 10000) lowthresh += 5;
		if (sampling_time < 200) sampling_time += 1;
	}

	nr_run_stat = avg_nr_running();
	nr_cpus = num_online_cpus();


	pr_info("tunedplug: run_stat: %d . lowthresh: %d . sampling: %d", nr_run_stat, lowthresh, sampling_time);

	if (nr_run_stat < lowthresh) {
		if (nr_cpus > 1) {
			down_one();
			return;
		}
	}
	else if (nr_cpus < NR_CPUS) {
		for (i = 1; i < NR_CPUS; i++) {
			if (!cpu_online(i)) {
				cpu_up(i);
				pr_info("tunedplug: UP cpu %d", i);
				if (nr_run_stat < lowthresh*2)
					return;
			}
		}
	}
}
static int lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
        switch (event)
        {
                case LCD_EVENT_OFF_START:
			pr_info("tunedplug: screen off!");
			displayon = false;
                        break;

                case LCD_EVENT_ON_END:
			pr_info("tunedplug: screen on again!");
			displayon = true;
        	        lowthresh = 4000;
	                sampling_time = DEF_SAMPLING;
                        break;

                default:
                        break;
        }
        return 0;
}

static void initnotifier(void)
{
        lcd_notif.notifier_call = lcd_notifier_callback;
        if (lcd_register_client(&lcd_notif) != 0)
                pr_err("%s: Failed to register lcd callback\n", __func__);

}

int __init tuned_plug_init(void)
{

	tunedplug_wq = alloc_workqueue("tunedplug", WQ_HIGHPRI | WQ_UNBOUND, 1);

	INIT_DELAYED_WORK(&tuned_plug_work, tuned_plug_work_fn);

	sampling_time = DEF_SAMPLING;

	queue_delayed_work_on(0, tunedplug_wq, &tuned_plug_work, msecs_to_jiffies(10000));

	initnotifier();

	return 0;
}

MODULE_AUTHOR("Heiler Bemerguy <heiler.bemerguy@gmail.com>");
MODULE_DESCRIPTION("'tuned_plug' - A simple cpu hotplug driver");
MODULE_LICENSE("GPL");

late_initcall(tuned_plug_init);

/*
 *
 * (C) COPYRIGHT ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */





/**
 * @file mali_kbase_pm_metrics.c
 * Metrics for power management
 */

#include <mali_kbase.h>
#include <mali_kbase_pm.h>

#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/proc_fs.h>

/* When VSync is being hit aim for utilisation between 70-90% */
#define KBASE_PM_VSYNC_MIN_UTILISATION          70
#define KBASE_PM_VSYNC_MAX_UTILISATION          90
/* Otherwise aim for 10-40% */
#define KBASE_PM_NO_VSYNC_MIN_UTILISATION       10
#define KBASE_PM_NO_VSYNC_MAX_UTILISATION       40

int g_current_sample_gl_utilization = 0;
int g_current_sample_cl_utilization[2] = {0};

/* Shift used for kbasep_pm_metrics_data.time_busy/idle - units of (1 << 8) ns
   This gives a maximum period between samples of 2^(32+8)/100 ns = slightly under 11s.
   Exceeding this will cause overflow */
#define KBASE_PM_TIME_SHIFT			8

static enum hrtimer_restart dvfs_callback(struct hrtimer *timer)
{
	unsigned long flags;
	kbase_pm_dvfs_action action;
	kbasep_pm_metrics_data *metrics;

	KBASE_DEBUG_ASSERT(timer != NULL);

	metrics = container_of(timer, kbasep_pm_metrics_data, timer);
	action = kbase_pm_get_dvfs_action(metrics->kbdev);

	spin_lock_irqsave(&metrics->lock, flags);

	if (metrics->timer_active)
		hrtimer_start(timer,
					  HR_TIMER_DELAY_MSEC(metrics->kbdev->pm.platform_dvfs_frequency),
					  HRTIMER_MODE_REL);

	spin_unlock_irqrestore(&metrics->lock, flags);

	return HRTIMER_NORESTART;
}

mali_error kbasep_pm_metrics_init(kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev != NULL);

	kbdev->pm.metrics.kbdev = kbdev;
	kbdev->pm.metrics.vsync_hit = 0;
	kbdev->pm.metrics.utilisation = 0;
	kbdev->pm.metrics.util_cl_share[0] = 0;
	kbdev->pm.metrics.util_cl_share[1] = 0;
	kbdev->pm.metrics.util_gl_share = 0;

	kbdev->pm.metrics.time_period_start = ktime_get();
	kbdev->pm.metrics.time_busy = 0;
	kbdev->pm.metrics.time_idle = 0;
	kbdev->pm.metrics.gpu_active = MALI_TRUE;
	kbdev->pm.metrics.timer_active = MALI_TRUE;
	kbdev->pm.metrics.active_cl_ctx[0] = 0;
	kbdev->pm.metrics.active_cl_ctx[1] = 0;
	kbdev->pm.metrics.active_gl_ctx = 0;
	kbdev->pm.metrics.busy_cl[0] = 0;
	kbdev->pm.metrics.busy_cl[1] = 0;
	kbdev->pm.metrics.busy_gl = 0;

	spin_lock_init(&kbdev->pm.metrics.lock);

	hrtimer_init(&kbdev->pm.metrics.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	kbdev->pm.metrics.timer.function = dvfs_callback;

	hrtimer_start(&kbdev->pm.metrics.timer, HR_TIMER_DELAY_MSEC(kbdev->pm.platform_dvfs_frequency), HRTIMER_MODE_REL);

	kbase_pm_register_vsync_callback(kbdev);

	return MALI_ERROR_NONE;
}

KBASE_EXPORT_TEST_API(kbasep_pm_metrics_init)

void kbasep_pm_metrics_term(kbase_device *kbdev)
{
	unsigned long flags;
	KBASE_DEBUG_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);
	kbdev->pm.metrics.timer_active = MALI_FALSE;
	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);

	hrtimer_cancel(&kbdev->pm.metrics.timer);

	kbase_pm_unregister_vsync_callback(kbdev);
}

KBASE_EXPORT_TEST_API(kbasep_pm_metrics_term)

/*caller needs to hold kbdev->pm.metrics.lock before calling this function*/
void kbasep_pm_record_job_status(kbase_device *kbdev)
{
	ktime_t now;
	ktime_t diff;
	u32 ns_time;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	now = ktime_get();
	diff = ktime_sub(now, kbdev->pm.metrics.time_period_start);

	ns_time = (u32) (ktime_to_ns(diff) >> KBASE_PM_TIME_SHIFT);
	kbdev->pm.metrics.time_busy += ns_time;
	kbdev->pm.metrics.busy_gl += ns_time * kbdev->pm.metrics.active_gl_ctx;
	kbdev->pm.metrics.busy_cl[0] += ns_time * kbdev->pm.metrics.active_cl_ctx[0];
	kbdev->pm.metrics.busy_cl[1] += ns_time * kbdev->pm.metrics.active_cl_ctx[1];
	kbdev->pm.metrics.time_period_start = now;
}

KBASE_EXPORT_TEST_API(kbasep_pm_record_job_status)

void kbasep_pm_record_gpu_idle(kbase_device *kbdev)
{
	unsigned long flags;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);

	KBASE_DEBUG_ASSERT(kbdev->pm.metrics.gpu_active == MALI_TRUE);

	kbdev->pm.metrics.gpu_active = MALI_FALSE;

	kbasep_pm_record_job_status(kbdev);

	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);
}

KBASE_EXPORT_TEST_API(kbasep_pm_record_gpu_idle)

void kbasep_pm_record_gpu_active(kbase_device *kbdev)
{
	unsigned long flags;
	ktime_t now;
	ktime_t diff;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);

	KBASE_DEBUG_ASSERT(kbdev->pm.metrics.gpu_active == MALI_FALSE);

	kbdev->pm.metrics.gpu_active = MALI_TRUE;

	now = ktime_get();
	diff = ktime_sub(now, kbdev->pm.metrics.time_period_start);

	kbdev->pm.metrics.time_idle += (u32) (ktime_to_ns(diff) >> KBASE_PM_TIME_SHIFT);
	kbdev->pm.metrics.time_period_start = now;

	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);
}

KBASE_EXPORT_TEST_API(kbasep_pm_record_gpu_active)

void kbase_pm_report_vsync(kbase_device *kbdev, int buffer_updated)
{
	unsigned long flags;
	KBASE_DEBUG_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);
	kbdev->pm.metrics.vsync_hit = buffer_updated;
	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);
}

KBASE_EXPORT_TEST_API(kbase_pm_report_vsync)

/*caller needs to hold kbdev->pm.metrics.lock before calling this function*/
int kbase_pm_get_dvfs_utilisation(kbase_device *kbdev, int *util_gl_share, int util_cl_share[2])
{
	int utilisation = 0;
	int busy;
	ktime_t now = ktime_get();
	ktime_t diff;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	diff = ktime_sub(now, kbdev->pm.metrics.time_period_start);

	if (kbdev->pm.metrics.gpu_active) {
		u32 ns_time = (u32) (ktime_to_ns(diff) >> KBASE_PM_TIME_SHIFT);
		kbdev->pm.metrics.time_busy += ns_time;
		kbdev->pm.metrics.busy_cl[0] += ns_time * kbdev->pm.metrics.active_cl_ctx[0];
		kbdev->pm.metrics.busy_cl[1] += ns_time * kbdev->pm.metrics.active_cl_ctx[1];
		kbdev->pm.metrics.busy_gl += ns_time * kbdev->pm.metrics.active_gl_ctx;
		kbdev->pm.metrics.time_period_start = now;
	} else {
		kbdev->pm.metrics.time_idle += (u32) (ktime_to_ns(diff) >> KBASE_PM_TIME_SHIFT);
		kbdev->pm.metrics.time_period_start = now;
	}

	if (kbdev->pm.metrics.time_idle + kbdev->pm.metrics.time_busy == 0) {
		/* No data - so we return NOP */
		utilisation = -1;
		if (util_gl_share)
			*util_gl_share = -1;
		if (util_cl_share) {
			util_cl_share[0] = -1;
			util_cl_share[1] = -1;
		}
		goto out;
	}

	utilisation = (100 * kbdev->pm.metrics.time_busy) /
			(kbdev->pm.metrics.time_idle +
			 kbdev->pm.metrics.time_busy);

	busy = kbdev->pm.metrics.busy_gl +
		kbdev->pm.metrics.busy_cl[0] +
		kbdev->pm.metrics.busy_cl[1];

	if (busy != 0) {
		if (util_gl_share)
			*util_gl_share =
				(100 * kbdev->pm.metrics.busy_gl) / busy;
		if (util_cl_share) {
			util_cl_share[0] =
				(100 * kbdev->pm.metrics.busy_cl[0]) / busy;
			util_cl_share[1] =
				(100 * kbdev->pm.metrics.busy_cl[1]) / busy;
		}
	} else {
		if (util_gl_share)
			*util_gl_share = -1;
		if (util_cl_share) {
			util_cl_share[0] = -1;
			util_cl_share[1] = -1;
		}
	}

out:

	kbdev->pm.metrics.time_idle = 0;
	kbdev->pm.metrics.time_busy = 0;
	kbdev->pm.metrics.busy_cl[0] = 0;
	kbdev->pm.metrics.busy_cl[1] = 0;
	kbdev->pm.metrics.busy_gl = 0;

	return utilisation;
}

kbase_pm_dvfs_action kbase_pm_get_dvfs_action(kbase_device *kbdev)
{
	unsigned long flags;
	int utilisation, util_gl_share;
	int util_cl_share[2];
	kbase_pm_dvfs_action action;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);

	utilisation = kbase_pm_get_dvfs_utilisation(kbdev, &util_gl_share, util_cl_share);

	if (utilisation < 0 || util_gl_share < 0 || util_cl_share < 0) {
		action = KBASE_PM_DVFS_NOP;
		utilisation = 0;
		util_gl_share = 0;
		util_cl_share[0] = 0;
		util_cl_share[1] = 0;
		goto out;
	}

	if (kbdev->pm.metrics.vsync_hit) {
		/* VSync is being met */
		if (utilisation < KBASE_PM_VSYNC_MIN_UTILISATION)
			action = KBASE_PM_DVFS_CLOCK_DOWN;
		else if (utilisation > KBASE_PM_VSYNC_MAX_UTILISATION)
			action = KBASE_PM_DVFS_CLOCK_UP;
		else
			action = KBASE_PM_DVFS_NOP;
	} else {
		/* VSync is being missed */
		if (utilisation < KBASE_PM_NO_VSYNC_MIN_UTILISATION)
			action = KBASE_PM_DVFS_CLOCK_DOWN;
		else if (utilisation > KBASE_PM_NO_VSYNC_MAX_UTILISATION)
			action = KBASE_PM_DVFS_CLOCK_UP;
		else
			action = KBASE_PM_DVFS_NOP;
	}

	kbdev->pm.metrics.utilisation = utilisation;
	kbdev->pm.metrics.util_cl_share[0] = util_cl_share[0];
	kbdev->pm.metrics.util_cl_share[1] = util_cl_share[1];
	kbdev->pm.metrics.util_gl_share = util_gl_share;

	g_current_sample_gl_utilization = utilisation;
	g_current_sample_cl_utilization[0] = util_cl_share[0];
	g_current_sample_cl_utilization[1] = util_cl_share[1];
   
out:
#ifdef CONFIG_MALI_MIDGARD_DVFS
	kbase_platform_dvfs_event(kbdev, utilisation, util_gl_share, util_cl_share);
#endif				/*CONFIG_MALI_MIDGARD_DVFS */
	kbdev->pm.metrics.time_idle = 0;
	kbdev->pm.metrics.time_busy = 0;
	kbdev->pm.metrics.busy_cl[0] = 0;
	kbdev->pm.metrics.busy_cl[1] = 0;
	kbdev->pm.metrics.busy_gl = 0;
	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);

	return action;
}
KBASE_EXPORT_TEST_API(kbase_pm_get_dvfs_action)

mali_bool kbase_pm_metrics_is_active(kbase_device *kbdev)
{
	mali_bool isactive;
	unsigned long flags;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);
	isactive = (kbdev->pm.metrics.timer_active == MALI_TRUE);
	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);

	return isactive;
}
KBASE_EXPORT_TEST_API(kbase_pm_metrics_is_active)


u32 kbasep_get_gl_utilization(void)
{
	return g_current_sample_gl_utilization;
}
KBASE_EXPORT_TEST_API(kbasep_get_gl_utilization)

u32 kbasep_get_cl_js0_utilization(void)
{
	return g_current_sample_cl_utilization[0];
}
KBASE_EXPORT_TEST_API(kbasep_get_cl_js0_utilization)

u32 kbasep_get_cl_js1_utilization(void)
{
	return g_current_sample_cl_utilization[1];
}
KBASE_EXPORT_TEST_API(kbasep_get_cl_js1_utilization)


/// For GPU memory usage
static int proc_gpu_memoryusage_show(struct seq_file *m, void *v)
{
	ssize_t ret = 0;
	struct list_head *entry;
	const struct list_head *kbdev_list;
	kbdev_list = kbase_dev_list_get();
	list_for_each(entry, kbdev_list) {
		struct kbase_device *kbdev = NULL;
		kbasep_kctx_list_element *element;
      int pages;
		kbdev = list_entry(entry, struct kbase_device, entry);
		pages = atomic_read(&(kbdev->memdev.used_pages));
		/* output the total memory usage in bytes for this device */
		ret = seq_printf(m, "%-16s  %10u\n", \
				kbdev->devname, \
				pages*4096);
	}
	kbase_dev_list_put(kbdev_list);

   return ret;
}

static int kbasep_gpu_memoryusage_debugfs_open(struct inode *in, struct file *file)
{
    return single_open(file, proc_gpu_memoryusage_show, NULL);
}

static const struct file_operations kbasep_gpu_memory_usage_debugfs_open = {
    .open    = kbasep_gpu_memoryusage_debugfs_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/// For GL/CL utilization
static int proc_gpu_utilization_show(struct seq_file *m, void *v)
{
    unsigned long gl, cl0, cl1;
    
    gl  = kbasep_get_gl_utilization();
    cl0 = kbasep_get_cl_js0_utilization();
    cl1 = kbasep_get_cl_js1_utilization();

    seq_printf(m, "gpu/cljs0/cljs1=%lu/%lu/%lu\n", gl, cl0, cl1);

    return 0;
}

/*
 *  File operations related to debugfs entry for gpu utilization
 */
STATIC int kbasep_gpu_utilization_debugfs_open(struct inode *in, struct file *file)
{
	return single_open(file, proc_gpu_utilization_show , NULL);
}

static const struct file_operations kbasep_gpu_utilization_debugfs_fops = {
	.open    = kbasep_gpu_utilization_debugfs_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release_private,
};


#ifdef CONFIG_PROC_FS
static struct proc_dir_entry *mali_pentry;

void proc_mali_register(void)
{    
    mali_pentry = proc_mkdir("mali", NULL);    
   
    if (!mali_pentry)
        return;
         
    proc_create("memory_usage", 0, mali_pentry, &kbasep_gpu_memory_usage_debugfs_open);
    proc_create("utilization", 0, mali_pentry, &kbasep_gpu_utilization_debugfs_fops);
}
void proc_mali_unregister(void)
{
    if (!mali_pentry)
        return;

    remove_proc_entry("memory_usage", mali_pentry);
    remove_proc_entry("utilization", mali_pentry);
    remove_proc_entry("mali", NULL);
    mali_pentry = NULL;
}
#else
#define proc_mali_register() do{}while(0)
#define proc_mali_unregister() do{}while(0)
#endif /// CONFIG_PROC_FS

#if 0
/*
 *  Initialize debugfs entry for gpu_memory
 */
mali_error kbasep_gpu_utilization_debugfs_init(kbase_device *kbdev)
{
	kbdev->gpu_memory_dentry = debugfs_create_file("utilization", \
					S_IRUGO, \
					kbdev->mali_debugfs_directory, \
					NULL, \
					&kbasep_gpu_utilization_debugfs_fops);
	if (IS_ERR(kbdev->gpu_util_dentry))
		return MALI_ERROR_FUNCTION_FAILED;

	return MALI_ERROR_NONE;
}

/*
 *  Terminate debugfs entry for gpu_memory
 */
void kbasep_gpu_utilization_debugfs_term(kbase_device *kbdev)
{
	debugfs_remove(kbdev->gpu_util_dentry);
}
#endif /// #if 0

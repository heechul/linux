/**
 * Memory bandwidth controller for multi-core systems
 *
 * Copyright (C) 2012  Heechul Yun <heechul@illinois.edu>
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/

#define USE_TIMING 0
#define USE_DEBUG  1

/**************************************************************************
 * Included Files
 **************************************************************************/
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/smp.h> /* IPI calls */
#include <linux/irq_work.h>
#include <linux/hardirq.h>
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <asm/atomic.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define MAX_NCPUS 32
#define NUM_RETRY  5
#define CACHE_LINE_SIZE 64

#if USE_TIMING
#  define TIMING_DEBUG(x) x
#else
#  define TIMING_DEBUG(x)
#endif

#if USE_DEBUG
#  define DEBUG_RECLAIM(x) x
#  define DEBUG_IRQWORK(x)
#  define DEBUG_USER(x) x
#else
#  define DEBUG_RECLAIM(x)
#  define DEBUG_IRQWORK(x)
#  define DEBUG_USER(x)
#endif

/**************************************************************************
 * Public Types
 **************************************************************************/
struct timeing {
	ktime_t throttle_cost;
	u32 throttle_cnt;
	ktime_t unthrottle_cost;
	u32 unthrottle_cnt;
	ktime_t reload_cost;
	u32 reload_cnt;
};

struct memstat{
	u64 used_budget;         /* used budget*/
	u64 assigned_budget;
	u64 throttled_time_ns;   
	int throttled;           /* throttled period count */
	u64 throttled_error;     /* throttled & error */
	int throttled_error_dist[10]; /* pct distribution */
	int exclusive;           /* exclusive period count */
};

/* percpu info */
struct core_info {
	/* user configurations */
	int budget;              /* assigned budget */

	int limit;               /* limit mode (exclusive to weight)*/
	int weight;              /* weight mode (exclusive to limit)*/

	/* for control logic */
	int cur_budget;          /* currently available budget */

	int throttled;           /* #of tasks throttled in current period */
	ktime_t throttled_time;  /* absolute time when throttled */

	u64 old_val;             /* hold previous counter value */
	int prev_throttle_error; /* check whether there was throttle error in 
				    the previous period */

	u64 exclusive_vtime_ns;  /* exclusive mode virtual time for scheduling */
	int exclusive_mode;      /* 1 - if in exclusive mode */
	ktime_t exclusive_time;  /* time when exclusive mode begins */

	struct irq_work	pending; /* delayed work for NMIs */
	struct perf_event *event;/* performance counter i/f */

	/* statistics */
	struct memstat overall;  /* stat for overall periods. reset by user */
	int used[3];             /* EWMA memory load */
	long period_cnt;         /* active periods count */
#if USE_TIMING
	struct timing tm;
#endif
};

/* global info */
struct memsched_info {
	int period_in_jiffies;
	int start_tick;
	int budget;              /* reclaimed budget */
	long period_cnt;
	spinlock_t lock;
	int max_budget;          /* \sum(cinfo->budget) */

	cpumask_var_t throttle_mask;
	cpumask_var_t active_mask;
};


/**************************************************************************
 * Global Variables
 **************************************************************************/
static struct memsched_info memsched_info;
static struct core_info __percpu *core_info;

static int g_period_us = 1000;
static int g_use_reclaim = 0; /* minimum remaining time to reclaim */
static int g_use_exclusive = 0;
static int g_use_task_priority = 0;
static int g_budget_pct[MAX_NCPUS];
static int g_budget_cnt = 4;
static int g_budget_min_value = 1000;
static int g_budget_max_bw = 2100; /* MB/s. best=6000 MB/s, worst=2100 MB/s */ 
static int g_use_hw = 1; /* 1 - PERF_COUNT_HW_MISSES, 0 - SW_CPU_CLOCK */

static struct dentry *memsched_dir;

/* copied from kernel/sched/sched.h */
static const int prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

/**************************************************************************
 * External Function Prototypes
 **************************************************************************/
extern int throttle_rq_cpu(int cpu);
extern int unthrottle_rq_cpu(int cpu);
extern void register_throttle_period_callback(void *func);

/**************************************************************************
 * Local Function Prototypes
 **************************************************************************/
static void __reset_stats(void *info);

/**************************************************************************
 * Module parameters
 **************************************************************************/

module_param(g_use_hw, int,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_use_hw, "hardware or software(vm)");

module_param(g_use_reclaim, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_use_reclaim, "enable/disable reclaim");

module_param(g_period_us, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_period_us, "throttling period in usec");

module_param_array(g_budget_pct, int, &g_budget_cnt, 0000);
MODULE_PARM_DESC(g_budget_pct, "array of budget per cpu");

module_param(g_budget_max_bw, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_budget_max_bw, "maximum memory bandwidth (MB/s)");

/**************************************************************************
 * Module main code
 **************************************************************************/

/** convert MB/s to #of events (i.e., LLC miss counts) per 1ms */
static inline u64 convert_mb_to_events(int mb)
{
	return div64_u64((u64)mb*1024*1024, CACHE_LINE_SIZE*1000);
}
static inline int convert_events_to_mb(u64 events)
{
	int divisor = 1024*1024;
	int mb = div64_u64(events*CACHE_LINE_SIZE*1000 + (divisor-1), divisor);
	return mb;
}

static inline void print_current_context(void)
{
	trace_printk("in_interrupt(%ld)(hard(%ld),softirq(%d),in_nmi(%d)),irqs_disabled(%d)\n",
		     in_interrupt(), in_irq(), (int)in_softirq(),
		     (int)in_nmi(), (int)irqs_disabled());
}

/** read current counter value. */
static inline u64 perf_event_count(struct perf_event *event)
{
	return local64_read(&event->count) + atomic64_read(&event->child_count);
}

/** return used event in the current period */
static inline u64 memsched_event_used(struct core_info *cinfo)
{
	return perf_event_count(cinfo->event) - cinfo->old_val;
}

static void print_core_info(int cpu, struct core_info *cinfo)
{
	printk(KERN_INFO "CPU%d: budget: %d, cur_budget: %d, period: %ld\n", cpu, 
	       cinfo->budget, cinfo->cur_budget, cinfo->period_cnt);
}

/**
 * update per-core usage statistics
 */
void update_statistics(struct core_info *cinfo)
{
	/* counter must be stopped by now. */
	s64 new;
	int used;
	u64 exclusive_ns = 0;

	new = perf_event_count(cinfo->event);
	used = (int)(new - cinfo->old_val); 

	cinfo->period_cnt++;
	cinfo->old_val = new;
	cinfo->overall.used_budget += used;
	cinfo->overall.assigned_budget += cinfo->budget;

	/* EWMA filtered per-core usage statistics */
	cinfo->used[0] = used;
	cinfo->used[1] = (cinfo->used[1] * (2-1) + used) >> 1; 
	/* used[1]_k = 1/2 used[1]_k-1 + 1/2 used */
	cinfo->used[2] = (cinfo->used[2] * (4-1) + used) >> 2; 
	/* used[2]_k = 3/4 used[2]_k-1 + 1/4 used */

	/* core is currently throttled. */
	if (cinfo->throttled > 0) {
		cinfo->overall.throttled_time_ns +=
			(ktime_get().tv64 - cinfo->throttled_time.tv64);
		cinfo->overall.throttled++;
	}

	/* throttling error condition:
	   I was too intensity in giving up "unsed" budget */
	if (cinfo->prev_throttle_error && used < cinfo->budget) {
		int diff = cinfo->budget - used;
		int idx;

		cinfo->overall.throttled_error ++; // += diff;
		idx = (int)(diff * 10 / cinfo->budget);
		cinfo->overall.throttled_error_dist[idx]++;
		trace_printk("ERR: throttled_error: %d < %d\n", 
			     used, cinfo->budget);
		/* compensation for error to catch-up*/
		cinfo->used[1] = cinfo->budget + diff;
	}
	cinfo->prev_throttle_error = 0;

	/* I was the lucky guy who used the DRAM exclusively */
	if (cinfo->exclusive_mode) {
		exclusive_ns = (ktime_get().tv64 - cinfo->exclusive_time.tv64);
		cinfo->exclusive_vtime_ns += exclusive_ns;
		cinfo->exclusive_mode = 0;
		cinfo->overall.exclusive++;
	}

	trace_printk("%lld %d %d CPU%d org: %d cur: %d excl: %lld\n",
		     new, used, cinfo->throttled,
		     smp_processor_id(), 
		     cinfo->budget,
		     cinfo->cur_budget,
		     exclusive_ns);
}


/**
 * budget is used up. PMU generate an interrupt
 * this run in hardirq, nmi context with irq disabled
 */
static void event_overflow_callback(struct perf_event *event,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
				    int nmi,
#endif
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo);
	irq_work_queue(&cinfo->pending);
}

static int donate_budget(long cur_period, int budget)
{
	struct memsched_info *global = &memsched_info;
	spin_lock(&global->lock);
	if (global->period_cnt != cur_period) {
		global->period_cnt = cur_period;
		global->budget = budget;
	} else {
		global->budget += budget;
	}
	spin_unlock(&global->lock);
	return global->budget;
}

static int reclaim_budget(long cur_period, int budget)
{
	struct memsched_info *global = &memsched_info;
	int reclaimed = 0;
	spin_lock(&global->lock);
	if (global->period_cnt == cur_period) {
		reclaimed = min(budget, global->budget);
		global->budget -= reclaimed;
	}
	spin_unlock(&global->lock);
	return reclaimed;
}

/**
 * reclaim local budget from global budget pool
 */
static int request_budget(struct memsched_info *global,
			   struct core_info *cinfo)
{
	int amount = 0;
	int old_budget;
	int budget_used = memsched_event_used(cinfo);

	BUG_ON(!global || !cinfo);

	old_budget = global->budget;
	if (budget_used < cinfo->budget) {
		/* didn't used up my original budget */
		amount = min(cinfo->budget - budget_used, old_budget);
	} else {
		/* I'm requesting more than I originall assigned */
		amount = min(g_budget_min_value, old_budget);
	}

	if (amount > 0) {
		/* successfully reclaim my budget */
		amount = reclaim_budget(jiffies, amount);
	}
	return amount;
}

/**
 * called by process_overflow
 */
static void __unthrottle_core(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	if (cinfo->throttled > 0) {
		int count;
		cinfo->exclusive_mode = 1;
		cinfo->exclusive_time = ktime_get();
		count = unthrottle_rq_cpu(smp_processor_id());
		if (count > 0)
			cinfo->throttled -= count;
		trace_printk("ALG2: no regulation mode begin\n");
	}
}

/**
 * memory overflow handler.
 * must not be executed in NMI context. but in hard irq context
 */
static void memsched_process_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memsched_info *global = &memsched_info;

	int count = 0;
	int amount = 0;
	ktime_t start = ktime_get();
	s64 budget_used;

	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		trace_printk("not active\n");
		return;
	}
	smp_mb();

	BUG_ON(in_nmi() || !in_irq());

	budget_used = memsched_event_used(cinfo);

	WARN_ON(cinfo->budget > global->max_budget);

	/* erroneous overflow, that could have happend before period timer
	   stop the pmu */
	if (budget_used == 0 || budget_used < cinfo->cur_budget) {
		trace_printk("ERR: used %lld < cur_budget %d. ignore\n",
			     budget_used, cinfo->cur_budget);
		return;
	}

	/* try to reclaim budget from the global pool */
	amount = request_budget(global, cinfo);
	if (amount > 0) {
		cinfo->cur_budget += amount;
		// cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);
		local64_set(&cinfo->event->hw.period_left, amount);
		// atomic_add(1, &cinfo->event->event_limit);
		// cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD);
		DEBUG_RECLAIM(trace_printk("successfully reclaimed %d\n", amount));
		return;
	}

	if (budget_used < cinfo->budget) {
		trace_printk("ERR: throttling error\n");
		cinfo->prev_throttle_error = 1;
		return;
	}

	/* we are going to be throttled */
	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);

	/* all other cores are alreay throttled. 
	   either wake them up or me start until the next period */
	if (cpumask_equal(global->throttle_mask, global->active_mask) &&
	    g_use_exclusive) {
		/* algorithm 1: last one get the whole remaining time */
		/* algorithm 2: wakeup all (i.e., non regulation) */
		/* algorithm 3: TODO: schedule according to exclusive mode vtime */
		if (g_use_exclusive == 2)
			smp_call_function(__unthrottle_core, NULL, 0);

		cinfo->exclusive_mode = 1;
		cinfo->exclusive_time = ktime_get();
		DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
		return;
	}
	/*
	 * fail to reclaim. now throttle this core
	 */
	DEBUG_RECLAIM(trace_printk("fail to reclaim after %lld nsec.\n",
				   ktime_get().tv64 - start.tv64));

	/* throttle the core. but it may fail because other cores may possess
	   the rq->lock for migration */
	if ((count = throttle_rq_cpu(smp_processor_id())) < 0) {
		/* throttle failed. do nothing */
		trace_printk("ERR: failed to throttle. err=%d\n", count);
	} else {
		/* throttle successful. update throttled task count */
		cinfo->throttled += count;
		cinfo->throttled_time = start;
	}

	TIMING_DEBUG(cinfo->throttle_cost =
		     ktime_add(cinfo->throttle_cost,
			       ktime_sub(ktime_get(), start)));
	TIMING_DEBUG(cinfo->throttle_cnt++);

	local64_set(&cinfo->event->hw.period_left, global->max_budget);
}

/**
 * per-core period processing
 *
 * called by scheduler tick to replenish budget and unthrottle if needed
 * run in interrupt context (irq disabled)
 */

/*
 * period_timer algorithm:
 *	excess = 0;
 *	if predict < budget:
 *	   excess = budget - predict;
 *	   global += excess
 *	set interrupt at (budget - excess)
 */
static void period_timer_callback(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memsched_info *global = &memsched_info;
	int count;
	int nr_running = (int)info;

	TIMING_DEBUG(ktime_t start;);

	smp_mb();

	/* stop counter */
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);
	
	/* no task is running */
	if (nr_running == 0 && cinfo->throttled == 0) {
		int cpu = smp_processor_id();
		if (cpumask_test_cpu(smp_processor_id(), global->active_mask))
			trace_printk("enter idle\n");
		cpumask_clear_cpu(cpu, global->active_mask);
		return;
	}

	/* must be irq disabled. hard irq */
	BUG_ON(!irqs_disabled() || !in_irq());

	/* I'm actively participating */
	cpumask_set_cpu(smp_processor_id(), global->active_mask);
	cpumask_clear_cpu(smp_processor_id(), global->throttle_mask);

	trace_printk("%d|%d|New period %ld. global->budget=%d\n",
		     nr_running, cinfo->throttled,
		     jiffies - global->start_tick,
		     global->budget);
	
	/* update statistics. */
	update_statistics(cinfo);

	/* task priority to weight conversion */
	if (g_use_task_priority) {
		int prio = current->static_prio - MAX_RT_PRIO;
		cinfo->weight = prio_to_weight[prio];
		trace_printk("Task WGT: %d prio:%d\n", cinfo->weight, prio);
	}

	/* new budget assignment from user */
	if (cinfo->weight > 0) {
		/* weight mode */
		int wsum = 0; int i;
		smp_mb();
		for_each_cpu(i, global->active_mask)
			wsum += per_cpu_ptr(core_info, i)->weight;
		cinfo->budget = div64_u64((u64)global->max_budget*cinfo->weight, wsum);
		trace_printk("WGT: budget:%d/%d weight:%d/%d\n",
			     cinfo->budget, global->max_budget, cinfo->weight, wsum);
	} else if (cinfo->limit > 0) {
		/* limit mode */
		cinfo->budget = cinfo->limit;
	} else {
		printk(KERN_ERR "both limit and weight = 0");
	}

	if (cinfo->budget > global->max_budget)
		trace_printk("ERR: c->budget(%d) > g->max_budget(%d)\n",
		     cinfo->budget, global->max_budget);

	if (cinfo->event->hw.sample_period != cinfo->budget) {
		/* new budget is assigned */
		trace_printk("MSG: new budget %d is assigned\n", cinfo->budget);
		cinfo->event->hw.sample_period = cinfo->budget;
	}

	/* unthrottle tasks (if any) */
	if (cinfo->throttled > 0) {
		/* there's tasks that were throttled in the previous period */
		TIMING_DEBUG(start = ktime_get());
		count = unthrottle_rq_cpu(smp_processor_id());
		if (count > 0)
			cinfo->throttled -= count;
		/* WARN_ON(cinfo->throttled != 0); */
		TIMING_DEBUG(cinfo->unthrottle_cost =
			     ktime_add(cinfo->unthrottle_cost,
				       ktime_sub(ktime_get(), start)));
		TIMING_DEBUG(cinfo->unthrottle_cnt++);
	} else if (cinfo->throttled < 0) {
		/* throttle was failed for whatever reason */
		trace_printk("ERR: throttle was failed(%d) before\n", cinfo->throttled);
		cinfo->throttled = 0;
	}

	/* setup an interrupt */
	if (g_use_reclaim &&
	    cinfo->used[1]  < cinfo->budget)
	{
		/* donate 'expected surplus' ahead of time. */
		int surplus = max(cinfo->budget - cinfo->used[1], 1);
		WARN_ON(surplus > global->max_budget);
		donate_budget(jiffies, surplus);
		cinfo->cur_budget = cinfo->budget - surplus;
		trace_printk("surplus: %d, budget: %d, global->budget: %d\n",
			     surplus, cinfo->budget, global->budget);
	} else {
		cinfo->cur_budget = cinfo->budget;
	}
	local64_set(&cinfo->event->hw.period_left, cinfo->cur_budget);
	cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD);
	/* make cinfo-> changes are globaly visible */
}

static void __init_per_core(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);

	memset(cinfo, 0, sizeof(core_info));

	smp_rmb();

	/* initialize per_event structure */
	cinfo->event = (struct perf_event *)info;

	/* initialize budget */
	cinfo->budget = cinfo->limit = cinfo->event->hw.sample_period;

	/* initialize statistics */
	__reset_stats(cinfo);

	print_core_info(smp_processor_id(), cinfo);

	smp_wmb();

	/* initialize nmi irq_work_queue */
	init_irq_work(&cinfo->pending, memsched_process_overflow);
}

static struct perf_event *init_counter(int cpu, int budget)
{
	struct perf_event *event = NULL;
	struct perf_event_attr sched_perf_hw_attr = {
		.type           = PERF_TYPE_HARDWARE,
		.config         = PERF_COUNT_HW_CACHE_MISSES,
		.size		= sizeof(struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
		.exclude_kernel = 1,
		.pinned = 1,
	};

	if (!g_use_hw) {
		sched_perf_hw_attr.type           = PERF_TYPE_SOFTWARE;
		sched_perf_hw_attr.config         = PERF_COUNT_SW_CPU_CLOCK;
	}

	/* select based on requested event type */
	sched_perf_hw_attr.sample_period = budget;

	/* Try to register using hardware perf events */
	event = perf_event_create_kernel_counter(
		&sched_perf_hw_attr,
		cpu, NULL,
		event_overflow_callback
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0) 
		,NULL
#endif
		);

	if (!event)
		return NULL;

	if (IS_ERR(event)) {
		/* vary the KERN level based on the returned errno */
		if (PTR_ERR(event) == -EOPNOTSUPP)
			printk(KERN_INFO "memsched: cpu%d. not supported\n", cpu);
		else if (PTR_ERR(event) == -ENOENT)
			printk(KERN_INFO "memsched: cpu%d. not h/w event\n", cpu);
		else
			printk(KERN_ERR "memsched: cpu%d. unable to create perf event: %ld\n",
			       cpu, PTR_ERR(event));
		return NULL;
	}

	/* success path */
	printk(KERN_INFO "memsched: cpu%d enabled counter.\n", cpu);

	smp_wmb();

	return event;
}

static void __disable_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo->event);

	/* stop the counter */
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);
	cinfo->event->pmu->del(cinfo->event, 0);

	/* unthrottle tasks before exit */
	if (cinfo->throttled > 0) {
		if (unthrottle_rq_cpu(smp_processor_id()) < 0)
			trace_printk("failed to unthrottle\n");
		else
			trace_printk("unthrottled %d tasks\n",
				     cinfo->throttled);
	}

	printk(KERN_INFO "LLC bandwidth throttling disabled\n");
}

static void disable_counters(void)
{
	on_each_cpu(__disable_counter, NULL, 0);
}

static void __start_counter(void* info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	cinfo->event->pmu->add(cinfo->event, PERF_EF_START);
}

static void start_counters(void)
{
	on_each_cpu(__start_counter, NULL, 0);
}

/**************************************************************************
 * Local Functions
 **************************************************************************/

static ssize_t memsched_control_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[256];
	char *p = buf;
	copy_from_user(&buf, ubuf, (cnt > 256) ? 256: cnt);

	if (!strncmp(p, "maxbw ", 6)) {
		sscanf(p+6, "%d", &g_budget_max_bw);
		memsched_info.max_budget =
			convert_mb_to_events(g_budget_max_bw);
	}
	else if (!strncmp(p, "taskprio ", 9))
		sscanf(p+9, "%d", &g_use_task_priority);
	else if (!strncmp(p, "reclaim ", 8))
		sscanf(p+8, "%d", &g_use_reclaim);
	else if (!strncmp(p, "exclusive ", 10))
		sscanf(p+10, "%d", &g_use_exclusive);
	else
		printk(KERN_INFO "ERROR: %s\n", p);
	smp_mb();
	return cnt;
}

static int memsched_control_show(struct seq_file *m, void *v)
{
	char buf[64];
	struct memsched_info *global = &memsched_info;

	seq_printf(m, "maxbw: %d (MB/s)\n", g_budget_max_bw);
	seq_printf(m, "reclaim: %d\n", g_use_reclaim);
	seq_printf(m, "exclusive: %d\n", g_use_exclusive);
	seq_printf(m, "taskprio: %d\n", g_use_task_priority);
	cpulist_scnprintf(buf, 64, global->active_mask);
	seq_printf(m, "active: %s\n", buf);
	cpulist_scnprintf(buf, 64, global->throttle_mask);
	seq_printf(m, "throttle: %s\n", buf);
	return 0;
}

static int memsched_control_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memsched_control_show, NULL);
}

static const struct file_operations memsched_control_fops = {
	.open		= memsched_control_open,
	.write          = memsched_control_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static void __do_barrier(void *info)
{
	smp_mb();
}

static void __update_budget(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	cinfo->limit = (int)info;
	cinfo->weight = 0;
	smp_mb();
	DEBUG_USER(trace_printk("MSG: New budget of Core%d is %d\n",
				smp_processor_id(), cinfo->budget));

}

static void __update_weight(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	cinfo->weight = (int)info;
	cinfo->limit = 0;
	smp_mb();
	DEBUG_USER(trace_printk("MSG: New weight of Core%d is %d\n",
				smp_processor_id(), cinfo->weight));
}

static ssize_t memsched_limit_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[256];
	char *p = buf;
	int i;
	int max_budget = 0;
	int use_mb = 0;
	copy_from_user(&buf, ubuf, (cnt > 256) ? 256: cnt);

	if (!strncmp(p, "mb ", 3)) {
		use_mb = 1;
		p+=3;
	}
	get_online_cpus();
	for_each_online_cpu(i) {
		int input;
		u64 events;
		sscanf(p, "%d", &input);
		if (!use_mb)
			input = g_budget_max_bw*100/input;
		events = convert_mb_to_events(input);
		max_budget += events;
		printk(KERN_INFO "CPU%d: New budget=%d (%d %s)\n", i, 
		       (int)events, input, (use_mb)?"MB/s": "pct");
		smp_call_function_single(i, __update_budget,
					 (void *)(int)events, 0);

		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}
	memsched_info.max_budget = max_budget;
	g_budget_max_bw = convert_events_to_mb(max_budget);

	smp_mb();

	put_online_cpus();
	return cnt;
}

static int memsched_limit_show(struct seq_file *m, void *v)
{
	int i, cpu;
	int wsum = 0;
	struct memsched_info *global = &memsched_info;
	cpu = get_cpu();

	smp_mb();
	seq_printf(m, "cpu  |budget (MB/s,pct,weight)\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i)
		wsum += per_cpu_ptr(core_info, i)->weight;
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0, pct;
		if (cinfo->limit > 0)
			budget = cinfo->limit;
		else if (cinfo->weight > 0) {
			budget = (int)div64_u64((u64)global->max_budget*cinfo->weight,
					   wsum);
		}
		WARN_ON(budget == 0);
		pct = div64_u64((u64)budget*100+(global->max_budget-1), 
				global->max_budget);
		seq_printf(m, "CPU%d: %d (%dMB/s, %d pct, w%d)\n", 
			   i, budget,
			   convert_events_to_mb(budget),
			   pct, cinfo->weight);
	}
	seq_printf(m, "g_budget_max_bw: %d MB/s, (%d)\n", g_budget_max_bw,
		global->max_budget);
	put_cpu();
	return 0;
}

static int memsched_limit_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memsched_limit_show, NULL);
}

static const struct file_operations memsched_limit_fops = {
	.open		= memsched_limit_open,
	.write          = memsched_limit_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static ssize_t memsched_share_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[256];
	char *p = buf;
	int i, cpu;

	copy_from_user(&buf, ubuf, (cnt > 256) ? 256: cnt);
	cpu = get_cpu();
	for_each_online_cpu(i) {
		int input;
		sscanf(p, "%d", &input);

		printk(KERN_INFO "CPU%d: input=%d\n", i, input);
		if (input == 0)
			input = 1024;
		printk(KERN_INFO "CPU%d: New weight=%d\n", i, (int)input);
		smp_call_function_single(i, __update_weight,
					 (void *)(int)input, 0);
		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}
	put_cpu();
	return cnt;
}


static const struct file_operations memsched_share_fops = {
	.open		= memsched_limit_open,
	.write          = memsched_share_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};



/**
 * Display usage statistics
 *
 * TODO: use IPI
 */
static int memsched_usage_show(struct seq_file *m, void *v)
{
	int i, j;

	get_online_cpus();
	on_each_cpu(__do_barrier, NULL, 1);
	smp_mb();

	/* current utilization */
	for (j = 0; j < 3; j++) {
		for_each_online_cpu(i) {
			struct core_info *cinfo = per_cpu_ptr(core_info, i);
			u64 budget, used, util;

			budget = cinfo->budget;
			used = cinfo->used[j];
			util = div64_u64(used * 100, (budget) ? budget : 1);
			seq_printf(m, "%llu ", util);
		}
		seq_printf(m, "\n");
	}
	seq_printf(m, "<overall>----\n");

	/* overall utilization
	   WARN: assume budget did not changed */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		u64 total_budget, total_used, result;

		total_budget = cinfo->overall.assigned_budget;
		total_used   = cinfo->overall.used_budget;
		result       = div64_u64(total_used * 100, (total_budget) ? total_budget : 1 );
		seq_printf(m, "%lld ", result);
	}
	seq_printf(m, "\n<exclusive>----\n");
	/* exclusive time */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%lld ", cinfo->exclusive_vtime_ns);
	}
	seq_printf(m, "\n");
	put_online_cpus();
	return 0;
}

static int memsched_usage_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memsched_usage_show, NULL);
}

static const struct file_operations memsched_usage_fops = {
	.open		= memsched_usage_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void __reset_stats(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	trace_printk("CPU%d\n", smp_processor_id());

	/* update local period information */
	cinfo->period_cnt = 0;
	cinfo->used[0] = cinfo->used[1] = cinfo->used[2] =
		cinfo->budget; /* initial condition */
	cinfo->cur_budget = cinfo->budget;
	cinfo->overall.used_budget = 0;
	cinfo->overall.assigned_budget = 0;
	cinfo->overall.throttled_time_ns = 0;
	cinfo->overall.throttled = 0;
	cinfo->overall.throttled_error = 0;
	memset(cinfo->overall.throttled_error_dist, 0, sizeof(int)*10);
	cinfo->throttled_time = ktime_set(0,0);
	smp_mb();

	DEBUG_USER(trace_printk("MSG: Clear statistics of Core%d\n",
				smp_processor_id()));
}


static ssize_t memsched_failcnt_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	/* reset local statistics */
	struct memsched_info *global = &memsched_info;

	global->budget = global->period_cnt = 0;
	global->start_tick = jiffies;
	smp_mb();
	on_each_cpu(__reset_stats, NULL, 0);
	return cnt;
}

static int memsched_failcnt_show(struct seq_file *m, void *v)
{
	int i;

	on_each_cpu(__do_barrier, NULL, 1);
	smp_mb();
	get_online_cpus();
	/* total #of throttled periods */
	seq_printf(m, "throttled: ");
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%d ", cinfo->overall.throttled);
	}
	seq_printf(m, "\nthrottle_error: ");
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%lld ", cinfo->overall.throttled_error);
	}

	seq_printf(m, "\ncore-pct   10    20    30    40    50    60    70    80    90    100\n");
	seq_printf(m, "--------------------------------------------------------------------");
	for_each_online_cpu(i) {
		int idx;
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "\n%4d    ", i);
		for (idx = 0; idx < 10; idx++)
			seq_printf(m, "%5d ",
				cinfo->overall.throttled_error_dist[idx]);
	}

	/* total #of exclusive mode periods */
	seq_printf(m, "\nexclusive: ");
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%d ", cinfo->overall.exclusive);
	}

	/* out of total periods */
	seq_printf(m, "\ntotal_periods %ld\n", per_cpu_ptr(core_info, 0)->period_cnt);
	put_online_cpus();
	return 0;
}

static int memsched_failcnt_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memsched_failcnt_show, NULL);
}

static const struct file_operations memsched_failcnt_fops = {
	.open		= memsched_failcnt_open,
	.write          = memsched_failcnt_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int memsched_init_debugfs(void)
{

	memsched_dir = debugfs_create_dir("memsched", NULL);
	BUG_ON(!memsched_dir);
	debugfs_create_file("control", 0444, memsched_dir, NULL,
			    &memsched_control_fops);

	debugfs_create_file("limit", 0444, memsched_dir, NULL,
			    &memsched_limit_fops);

	debugfs_create_file("share", 0444, memsched_dir, NULL,
			    &memsched_share_fops);

	debugfs_create_file("usage", 0666, memsched_dir, NULL,
			    &memsched_usage_fops);

	debugfs_create_file("failcnt", 0644, memsched_dir, NULL,
			    &memsched_failcnt_fops);
	return 0;
}

int init_module( void )
{
	int i;

	/* initialized memsched_info structure */
	memset(&memsched_info, 0, sizeof(memsched_info));
	zalloc_cpumask_var(&memsched_info.throttle_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&memsched_info.active_mask, GFP_NOWAIT);

	spin_lock_init(&memsched_info.lock);
	memsched_info.start_tick = jiffies;
	memsched_info.period_in_jiffies = max(g_period_us * HZ / 1000000, 1);
	memsched_info.max_budget = convert_mb_to_events(g_budget_max_bw);

	/* initialize all online cpus to be active */
	cpumask_copy(memsched_info.active_mask, cpu_online_mask);

	printk(KERN_INFO "HZ=%d, period=%d jiffies (g_period_us=%d)\n", HZ,
	       memsched_info.period_in_jiffies, g_period_us);

	/* Memory performance characteristics */
	if (g_budget_max_bw == 0) {
		printk(KERN_INFO "budget_max must be set\n");
		return -ENODEV;
	}

	printk(KERN_INFO "Max. b/w: %d (MB/s)\n", g_budget_max_bw);
	printk(KERN_INFO "Max. events per %d us: %lld\n", g_period_us,
	       convert_mb_to_events(g_budget_max_bw));
	if (g_use_reclaim)
		printk(KERN_INFO "Use reclaim\n");

	preempt_disable();

	printk("Initilizing perf counter from CPU%d\n", smp_processor_id());
	core_info = alloc_percpu(struct core_info);
	smp_mb();

	for_each_online_cpu(i) {
		struct perf_event *event;
		int budget, mb;
		/* initialize counter h/w & event structure */
		if (g_budget_pct[i] == 0) /* uninitialized. assign max value */
			g_budget_pct[i] = 100 / num_online_cpus();
		mb = div64_u64((u64)g_budget_max_bw * g_budget_pct[i],  100);
		budget = convert_mb_to_events(mb);
		printk(KERN_INFO "budget[%d] = %d (%d pct, %d MB/s)\n", i,
		       budget,g_budget_pct[i], mb);

		event = init_counter(i, budget);

		if (!event)
			break;
		smp_call_function_single(i, __init_per_core, (void *)event, 1);
	}
	preempt_enable();
	memsched_init_debugfs();

	printk("Start event counters\n");
	start_counters();
	smp_mb();

	printk("Start period timer (period=%d jiffies)\n", 
	       memsched_info.period_in_jiffies);
	register_throttle_period_callback(&period_timer_callback);

	return 0;
}

void cleanup_module( void )
{
	int i;

	smp_mb();

	/* unregister sched-tick callback */
	register_throttle_period_callback(NULL);

	/* remove debugfs entries */
	debugfs_remove_recursive(memsched_dir);

	/* stop perf_event counters */
	disable_counters();

	/* update all data structure */
	smp_mb();

	/* destroy perf objects */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		perf_event_release_kernel(cinfo->event);

#if USE_TIMING
		/* print timing measurement */
		{
			s64 avg_cost = ktime_to_ns(cinfo->throttle_cost);
			if (cinfo->throttle_cnt)
				do_div(avg_cost, cinfo->throttle_cnt);
			printk("core%d throttle avg_cost cnt: %lld %u\n",
				     i, avg_cost, cinfo->throttle_cnt);

			avg_cost = ktime_to_ns(cinfo->unthrottle_cost);
			if (cinfo->unthrottle_cnt)
				do_div(avg_cost, cinfo->unthrottle_cnt);
			printk("core%d unthrottle avg_cost cnt: %lld %u\n",
				     i, avg_cost, cinfo->unthrottle_cnt);

			avg_cost = ktime_to_ns(cinfo->reload_cost);
			if (cinfo->reload_cnt)
				do_div(avg_cost, cinfo->reload_cnt);
			printk("core%d reload avg_cost cnt: %lld %u\n",
				     i, avg_cost, cinfo->reload_cnt);
		}
#endif
	}

	free_percpu(core_info);

	printk("module uninstalled successfully\n");
	return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Heechul Yun <heechul@illinois.edu>");

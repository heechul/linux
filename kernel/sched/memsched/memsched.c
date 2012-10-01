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
#define __SELF_TEST__ 0

/**************************************************************************
 * Included Files
 **************************************************************************/
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

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define MAX_NCPUS 32
#define NUM_RETRY  5

#if USE_TIMING
#  define TIMING_DEBUG(x) x
#else
#  define TIMING_DEBUG(x)
#endif

#if USE_DEBUG
#  define DEBUG_RECLAIM(x) x
#else
#  define DEBUG_RECLAIM(x)
#endif

/**************************************************************************
 * Public Types
 **************************************************************************/
/* global info */
struct memsched_info {
	// u64 budget;        /* returned budget */
	atomic64_t budget;
	atomic64_t period_cnt;
	ktime_t period;
	struct hrtimer timer;
	int orun;
#if USE_TIMING
	ktime_t timer_timestamp;
#endif
};

struct timeing {
	ktime_t throttle_cost;
	u32 throttle_cnt;
	ktime_t unthrottle_cost;
	u32 unthrottle_cnt;
	ktime_t reload_cost;
	u32 reload_cnt;
};

struct memstat{
	u64 used_budget; /* used */
	u64 assigned_budget;
	u64 throttled_time_ns; /* us */
	int throttled; /* throttled period count */
	int throttled_error; /* throttled & error */
};

/* percpu info */
struct core_info {
	/* user configurations */
	u64 budget;        /* assigned budget */
	int intensity;     /* predicted memory intensity (0..100)
			      0 - auto. 1 - least intense, 100 - most intense */
	/* for control logic */
	u64 cur_budget;    /* currently available budget */
	int throttled;     /* #of tasks throttled in current period */
	ktime_t throttled_time; /* absolute time when throttled */
	u64 old_val;       /* hold previous counter value */

	/* statistics */
	struct memstat overall; /* stat for overall periods. reset by user */
	u64 used[3];       /* EWMA memory load */

	/* delayed work for NMIs */
	struct irq_work	   pending;
	spinlock_t lock;   /* lock to protect atomic update */
	struct perf_event *event; /* structure */

	/* local copy of global->period_cnt */
	s64 period_cnt;

	struct hrtimer reclaim_timer; /* reclaim timer */
	ktime_t reclaim_interval;
#if USE_TIMING
	struct timing tm;
#endif
};

/**************************************************************************
 * Global Variables
 **************************************************************************/
static struct memsched_info memsched_info;
static struct core_info __percpu *core_info;

static int g_period_us = 10000;
static int g_reclaim_threshold_us = 100; /* minimum remaining time to reclaim */
static int g_reclaim_threshold_pct = 90; /* maximum system load to initiate reclaim */

static int g_budget[MAX_NCPUS];
static int g_budget_cnt = 4;
static int g_budget_min_value = 1000;
static int g_ns_per_event = 0; /* 0 - autodetect, 17ns in i5.  33ns in core2quad */
static int g_use_hw = 1; /* 1 - PERF_COUNT_HW_MISSES, 0 - SW_CPU_CLOCK */

static struct dentry *memsched_dir;

#if __SELF_TEST__
static int g_test = 1000;
#endif

/**************************************************************************
 * External Function Prototypes
 **************************************************************************/
extern int throttle_rq_cpu(int cpu);
extern int unthrottle_rq_cpu(int cpu);


/**************************************************************************
 * Module parameters
 **************************************************************************/

#if __SELF_TEST__
module_param(g_test, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_test, "number of test iterations");
static int self_test(void);
#endif

module_param(g_period_us, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_period_us, "throttling period in usec");

module_param(g_reclaim_threshold_us, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_reclaim_threshold_us, "reclaim threshold in usec");

module_param(g_reclaim_threshold_pct, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_reclaim_threshold_pct, "reclaim threshold in pct");

module_param(g_use_hw, int,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_use_hw, "hardware or software(vm)");

module_param_array(g_budget, int, &g_budget_cnt, 0000);
MODULE_PARM_DESC(g_budget, "array of budget per cpu");

module_param(g_ns_per_event, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_ns_per_event, "average time(ns) per each event");

/**************************************************************************
 * Module main code
 **************************************************************************/

static inline u64 convert_bandwidth_to_events(int pct)
{
	u64 events;
	events = div64_u64(g_period_us * 1000 * pct, g_ns_per_event * 100);
	return events;
}
/*
 * Measure timing of memory read/write/update operation for 2x of
 * cache-line. Then calculate latency for 1 cache-line access
 */
static int detect_average_cacheline_cost(int cache_size_MB)
{
	int i;
	int mem_size = cache_size_MB * 2 * 1024 * 1024;
	int cache_line_size = 64;
	char *mem_ptr;
	u64 nread, sum;
	ktime_t start, duration;
	u64 result;

	mem_ptr = (char *)vmalloc(mem_size);
	if (!mem_ptr)
		return 0;

	sum = nread = 0;
	start = ktime_get();
	for ( i = 0; i < mem_size; i += cache_line_size * 8) {
		mem_ptr[i] = i;
		sum += mem_ptr[i];
		nread += cache_line_size;
	}
	duration = ktime_sub(ktime_get(), start);
	vfree(mem_ptr);

	/* nread : duration.tv64 = 64 : x
	   x = duration.tv64 * 64 / nread
	*/
	result = div64_u64(duration.tv64 * cache_line_size, nread);

	return (int)result;
}

static inline void print_current_context(void)
{
	trace_printk("in_interrupt(%ld)(hard(%ld),softirq(%d),in_nmi(%d)),irqs_disabled(%d)\n",
		     in_interrupt(), in_irq(), (int)in_softirq(),
		     (int)in_nmi(), (int)irqs_disabled());
}

/* read current counter value. */
static inline u64 perf_event_count(struct perf_event *event)
{
	return local64_read(&event->count) + atomic64_read(&event->child_count);
}

/* return used event in the current period */
static inline u64 memsched_event_used(struct core_info *cinfo)
{
	return perf_event_count(cinfo->event) - cinfo->old_val;
}

/* predict my need for a given time */
static inline u64 compute_my_need(struct core_info *cinfo, s64 time_remained_ns)
{
	u64 result;

	BUG_ON(!cinfo || cinfo->intensity > 100 || cinfo->intensity < 0);
	BUG_ON(g_ns_per_event == 0);
	WARN_ON(time_remained_ns > g_period_us * 1000);
	if (cinfo->intensity == 0) {
		/* automatic: use previous usage slope
		 * result = ns / (compute_time/prev_used_budget)
		 */
		u64 tmp = (u64)g_period_us * 1000;
		int ns_per_event = (int)div64_u64(tmp, max(cinfo->used[0], (u64)g_budget_min_value));
		do_div(time_remained_ns, ns_per_event+1);
		result = time_remained_ns;
		trace_printk("prev(time: %lld, used: %lld), ns_per_event: %d, result:%lld\n",
			     tmp, cinfo->used[0], ns_per_event, result);
	} else if (cinfo->intensity == 100) {
		/* most conservative */
		do_div(time_remained_ns, g_ns_per_event);
		result = time_remained_ns;
	} else {
		/* manual: use cinfo->intensity.
		 * restul = (ns / g_ns_per_event) * aggr / 100.
		 *        = (ns * aggr) / (g_ns_per_event * 100)
		 */
		result = div64_u64(time_remained_ns * cinfo->intensity, g_ns_per_event * 100);
	}
	return result;
}


/* unnecessary budget reclaimation.
 * irq disabled. hard irq. preempt depth=1 (d.h1) */
static void __reclaim_unused_budget(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memsched_info *global = &memsched_info;

	s64 new, budget_used, budget_remained, budget_surplus, budget_need;
	ktime_t time_remained;

	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);

	/* compute surplus budget */
	new = perf_event_count(cinfo->event);
	budget_used = new - cinfo->old_val;
	budget_remained = (s64)cinfo->cur_budget - budget_used;
	time_remained = ktime_sub(hrtimer_get_expires(&global->timer), ktime_get());
	budget_need = compute_my_need(cinfo, time_remained.tv64);

	if (budget_remained > 0)
		budget_surplus = budget_remained - budget_need;
	else
		budget_surplus = 0;

	/* return excessive budget to the global budget pool */
	if (budget_surplus > g_budget_min_value) {
		atomic64_add(budget_surplus, &global->budget);
		cinfo->cur_budget -= budget_surplus;
		local64_set(&cinfo->event->hw.period_left, budget_remained - budget_surplus);
		trace_printk("cur_budget=%lld remained=%lld need=%lld surplus=%lld\n",
			     cinfo->cur_budget, budget_remained, budget_need,
			     budget_surplus);
	}
	else {
		trace_printk("cur_budget=%lld remained=%lld need=%lld. zero surplus\n",
			     cinfo->cur_budget, budget_remained, budget_need);
	}
	cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD);
}

void update_statistics(struct core_info *cinfo)
{
	/* counter must be stopped by now. */
	u64 new, used;
	s64 left;

	new = perf_event_count(cinfo->event);
	used = new - cinfo->old_val;
	left = (s64)cinfo->cur_budget - used;

	cinfo->old_val = new;
	cinfo->overall.used_budget += used;
	cinfo->overall.assigned_budget += cinfo->budget;

	if (cinfo->throttled > 0) {
		cinfo->overall.throttled_time_ns += 
			(ktime_get().tv64 - cinfo->throttled_time.tv64);
		cinfo->overall.throttled++;
		/* throttling error condition:
		   I was too intensity in giving up "unsed" budget */
		if (used < cinfo->budget) {
			cinfo->overall.throttled_error ++;
			trace_printk("ERR: throttled_error: %lld < %lld\n", used, cinfo->budget);
		}
	} 

	/* EWMA filtered per-core usage statistics */
	cinfo->used[0] = used;
	cinfo->used[1] = (cinfo->used[1] * (2-1) + used) >> 1;
	cinfo->used[2] = (cinfo->used[2] * (4-1) + used) >> 2;

	if (unlikely(left < 0))
		trace_printk("%llu %llu %lld %d org: %lld cur: %lld\n", new, used, left,
			     cinfo->throttled, cinfo->budget, cinfo->cur_budget);
	else
		trace_printk("%llu %llu %lld %d\n", new, used, left,
			     cinfo->throttled);
}


/**
 * budget is used up. PMU generate an interrupt
 * this run in hardirq, nmi context with irq disabled
 */
static void event_overflow_callback(struct perf_event *event,
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	irq_work_queue(&cinfo->pending);
}

static s64 reclaim_budget(struct memsched_info *global,
			   struct core_info *cinfo)
{
	s64 amount = 0;
	u64 old_budget, new_budget;
	int ntries = 0;

	BUG_ON(!global || !cinfo);
retry:
	old_budget = atomic64_read(&global->budget);
	amount = min(cinfo->budget,old_budget);
	new_budget = old_budget - amount;
	if (atomic64_cmpxchg(&global->budget, old_budget, new_budget) != old_budget) {
		amount = 0;
		if (ntries++ < 5) 
			goto retry;
	}

	if (amount > 0) {
		/* successfully reclaim my budget */
		cinfo->cur_budget += amount;

		trace_printk("reclaimed %lld. new_budget=%lld\n",
			     amount, cinfo->cur_budget);

		cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);
		local64_set(&cinfo->event->hw.period_left, amount);
		atomic_add(1, &cinfo->event->event_limit);
		cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD);
	}
	return amount;
}

/**
 * send reclaim IPI to underloaded CPUs
 */
static bool request_reclaim(int mycpu)
{
	int util_pct;
	bool requested = false;
	int i;

	for_each_online_cpu(i) {
		struct core_info *ci;
		if (i == mycpu)
			continue;
		ci = per_cpu_ptr(core_info, i);
		BUG_ON(!ci->budget);
		util_pct = (int)div64_u64(ci->used[0] * 100, ci->budget);
		if (util_pct < g_reclaim_threshold_pct/2) {
			DEBUG_RECLAIM(trace_printk("send reclaim IPI to Core%d\n", i));
			smp_call_function_single(i, __reclaim_unused_budget, NULL, 0);
			requested = true;
		}
	}
	return requested;
}

/**
 * not in NMI context. bug still hardirq context
 */
static void memsched_process_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memsched_info *global = &memsched_info;

	int count = 0;
	u64 amount = 0;
	ktime_t time_remained;
	ktime_t start = ktime_get();
	s64 budget_used;
	s64 used_sum, assigned_sum;
	int util_pct;
	int i;
	bool requested;
	s64 period_no = atomic64_read(&global->period_cnt);

	BUG_ON(in_nmi());

	/* overflow after a new period */
	if (period_no != cinfo->period_cnt) {
		trace_printk("ERR: global->period_cnt(%lld) != cinfo->period_cnt(%lld)\n",
			     period_no, cinfo->period_cnt);
		return;
	}
	budget_used = memsched_event_used(cinfo);

	/* erroneous overflow, that could have happend before period timer stop the pmu */
	if (budget_used == 0 || budget_used < cinfo->cur_budget) {
		trace_printk("ERR: used %lld < cur_budget %lld. ignore\n",
			     budget_used, cinfo->cur_budget);
		return;
	}

	time_remained = ktime_sub(hrtimer_get_expires(&global->timer), ktime_get());
	/* overflow occured after deadline (How possible?) */
	if (time_remained.tv64 < 0) {
		trace_printk("ERR: overflow after deadline\n");
		return;
	}

	/*
	 * try to reclaim budget from the global pool
	 */
	amount = reclaim_budget(global, cinfo);
	if (amount > 0)
		return;

	/* check if time is too short */
	if (time_remained.tv64 < (s64)g_reclaim_threshold_us * 1000) {
		DEBUG_RECLAIM(trace_printk("remaining time (%lld ns) is too short\n",
					   time_remained.tv64));
		goto out_reclaim;
	}

        /*
	 * check if global load is low enough to reclaim
	 * There's no point reclaiming highly loaded system
	 * \sum{ used(i) } / \sum{ assigned(i) } < threshold
	 */
	used_sum = assigned_sum = 1;
	for_each_online_cpu(i) {
		struct core_info *ci = per_cpu_ptr(core_info, i);
		/* let's not count reclaimed budget */
		if (i == smp_processor_id())
			used_sum += min(ci->budget, ci->used[0]);
		else
			used_sum += ci->used[0];
		assigned_sum += ci->budget;
	}
	util_pct = (int)div64_u64(used_sum * 100, assigned_sum);
	if (util_pct >= g_reclaim_threshold_pct) {
		DEBUG_RECLAIM(trace_printk("System load (%d) is too high to reclaim\n", util_pct));
		goto out_reclaim;
	}

	/*
	 * now send reclaim request to underloaded cores
	 */
	requested = request_reclaim(smp_processor_id());

	/*
	 * re-try to reclaim budget from the global pool
	 */
	if (requested) {
		udelay(5); /* IPI propargation delay */
		amount = reclaim_budget(global, cinfo);
		if (amount > 0)
			return;
	}

out_reclaim:
	/* 
	 * fail to reclaim. now throttle this core
	 */
	DEBUG_RECLAIM(trace_printk("fail to reclaim after %lld nsec.\n",
				   ktime_get().tv64 - start.tv64));

	/* just in case previous unthrottle failed, we have to unthrottled them */
	if (unlikely(cinfo->throttled > 0)) {
		count = unthrottle_rq_cpu(smp_processor_id());
		if (count < 0) { /* if still failed, we should unthrottled them later */
			trace_printk("ERR: still failed to unthrottle %d tasks\n",
				     cinfo->throttled);
			goto out_overflow;
		}
		cinfo->throttled -= count;
		WARN_ON(cinfo->throttled != 0);
	}

	/* throttle the core. but it may fail because other cores may possess
	   the rq->lock for migration */
	if ((count = throttle_rq_cpu(smp_processor_id())) < 0) {
		/* throttle failed. do nothing */
		trace_printk("ERR: failed to throttle. err=%d\n", count);
	} else {
		/* throttle successful. update throttled task count */
		cinfo->throttled += count;
	}
	cinfo->throttled_time = start;

	time_remained = ktime_sub(hrtimer_get_expires(&global->timer), ktime_get());
#if 0
	/* if there's enough time left. retry reclaim later */
	if (time_remained.tv64 > (s64)g_reclaim_threshold_us * 1000 * 2) {
		DEBUG_RECLAIM(trace_printk("start reclaim timer\n"));
		hrtimer_start(&cinfo->reclaim_timer, 
			      cinfo->reclaim_interval, HRTIMER_MODE_REL_PINNED);
	}
#endif
out_overflow:
	TIMING_DEBUG(cinfo->throttle_cost =
		     ktime_add(cinfo->throttle_cost,
			       ktime_sub(ktime_get(), start)));
	TIMING_DEBUG(cinfo->throttle_cnt++);
}
/*
 * called by period timer to replenish budget and unthrottle if needed
 * run in interrupt context (irq disabled)
 */
static void __reset_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memsched_info *global = &memsched_info;
	int count;

	TIMING_DEBUG(ktime_t start;);

	/* must be irq disabled. hard irq */
	BUG_ON(!irqs_disabled() || !in_irq());

	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);

	/* update local period information */
	cinfo->period_cnt = atomic64_read(&global->period_cnt);

	/* cinfo->lock is not needed, since this is run on hardirq
	   context and the only contender is userspace thread that re-configure
	   budget via debugfs interface */
	update_statistics(cinfo);

	/* reset the dynamic budget */
	cinfo->cur_budget = cinfo->budget;

	/* new budget assignment */
	if (cinfo->event->hw.sample_period != cinfo->budget) {
		/* new budget is assigned */
		trace_printk("INFO: new budget %lld is assigned\n", cinfo->budget);
		cinfo->event->hw.sample_period = cinfo->budget;
	}

	if (cinfo->throttled > 0) {
		/* there's tasks that were throttled in the previous period */
		TIMING_DEBUG(start = ktime_get());
		count = unthrottle_rq_cpu(smp_processor_id());
		if (count < 0) {
			trace_printk("ERR: failed to unthrottle. err=%d\n",
				     count);
		} else {
			cinfo->throttled -= count;
			if (cinfo->throttled != 0) {
				trace_printk("ERR: throttled(%d), unthrottle count(%d)\n",
					     cinfo->throttled, count);
			}
		}
		TIMING_DEBUG(cinfo->unthrottle_cost =
			     ktime_add(cinfo->unthrottle_cost,
				       ktime_sub(ktime_get(), start)));
		TIMING_DEBUG(cinfo->unthrottle_cnt++);
	} else if (cinfo->throttled < 0) {
		/* throttle was failed for whatever reason */
		trace_printk("ERR: throttle was failed(%d) before\n", cinfo->throttled);
		cinfo->throttled = 0;
	}

	local64_set(&cinfo->event->hw.period_left, cinfo->budget);
	cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD);

	/* make cinfo-> changes are globaly visible */
	smp_wmb(); 
}

static void reset_counters(void)
{
	TIMING_DEBUG(memsched_info.timer_timestamp = ktime_get(); smp_wmb());

	/* resource manager */
	/* TODO */
	on_each_cpu(__reset_counter, NULL, 0);
}

enum hrtimer_restart reclaim_timer_callback(struct hrtimer *timer)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memsched_info *global = &memsched_info;
	ktime_t now = ktime_get();
	ktime_t deadline = hrtimer_get_expires(&global->timer); /* period end */
	ktime_t time_remained = ktime_sub(deadline, now);
	s64 amount;

	DEBUG_RECLAIM(trace_printk("reclaim timer is called\n"));

	/* if overrun. return stop */
	if (time_remained.tv64 < 0)
		return HRTIMER_NORESTART;

	if (request_reclaim(smp_processor_id())) {
		udelay(2);
	}

	/* try to reclaim again */
	amount = reclaim_budget(global, cinfo);
	if (amount > 0) {
		int count;
		DEBUG_RECLAIM(trace_printk("reclaimed\n"));
		/* unthrottle throttled tasks */
		if (cinfo->throttled > 0) {
			count = unthrottle_rq_cpu(smp_processor_id());
			if (count > 0)
				cinfo->throttled -= count;
			else
				WARN_ON(cinfo->throttled != 0);
		}
		return HRTIMER_NORESTART;
	} 

	/* retry if there's enough time */
	if (time_remained.tv64 > (s64)g_reclaim_threshold_us * 1000 * 2) {
		/* enough time is left. try again later */
		int orun;
		DEBUG_RECLAIM(trace_printk("reclaim_timer restart\n"));
		orun = hrtimer_forward(timer, hrtimer_get_expires(timer), 
				       cinfo->reclaim_interval);
		return HRTIMER_RESTART;
	}

	/* not enough time is left. stop */
	return HRTIMER_NORESTART;
}

enum hrtimer_restart period_timer_callback( struct hrtimer *timer )
{
	int orun;
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memsched_info *global = &memsched_info;
	u64 unused = 0;

	/* stop consuming budget. It's an end of the current period */
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);

	/* update global information */
	unused = atomic64_read(&global->budget);
	atomic64_set(&global->budget, 0);
	atomic64_inc(&global->period_cnt);

	if (unused >0)
		trace_printk("ERR: unused reclaimed budget %lld\n", unused);

	trace_printk("new period %ld started\n", atomic64_read(&global->period_cnt));

	/* asynchronous delivery. w/o ack. start pmu again */
	reset_counters();

	/* restart the period timer */
	orun = hrtimer_forward(timer, hrtimer_get_expires(timer), global->period);
	if (orun > 1) {
		global->orun += orun;
		trace_printk("ERR: overrun %d\n", orun);
	}

	cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD);

	return HRTIMER_RESTART;
}

static void __init_reclaim_timer(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	printk(KERN_INFO "Register reclaim timer\n");
	hrtimer_init(&cinfo->reclaim_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	cinfo->reclaim_timer.function = &reclaim_timer_callback;
	cinfo->reclaim_interval = 
			ktime_set(g_reclaim_threshold_us/ 1000000,
				  (g_reclaim_threshold_us % 1000000)*1000);
}

static int init_counter(int cpu, u64 budget)
{
	unsigned long flags;
	struct core_info *cinfo = per_cpu_ptr(core_info, cpu);
	struct perf_event_attr sched_perf_hw_attr = {
		.type           = PERF_TYPE_HARDWARE,
		.config         = PERF_COUNT_HW_CACHE_MISSES,
		.size		= sizeof(struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
	};

	if (!g_use_hw) {
		sched_perf_hw_attr.type           = PERF_TYPE_SOFTWARE;
		sched_perf_hw_attr.config         = PERF_COUNT_SW_CPU_CLOCK;
	}
	memset(cinfo, 0, sizeof(core_info));

	/* initialize lock */
	spin_lock_init(&cinfo->lock);

	/* grab a lock to protect cinfo structure update */
	spin_lock_irqsave(&cinfo->lock, flags);

	/* initialize budget */
	if (budget == 0)
		budget = g_budget_min_value;
	cinfo->budget = budget;
	cinfo->intensity = 100;
	cinfo->throttled_time = ktime_set(0,0);
	cinfo->cur_budget = cinfo->budget;
	cinfo->throttled = 0;
	cinfo->old_val = 0;
	cinfo->used[0] = cinfo->used[1] = cinfo->used[2] = 0;

#if USE_TIMING
	memset(&cinfo->tm, 0, sizeof(cinfo->tm));
#endif

	/* select based on requested event type */
	sched_perf_hw_attr.sample_period = cinfo->budget;


	/* Try to register using hardware perf events */
	cinfo->event = perf_event_create_kernel_counter(
		&sched_perf_hw_attr,
		cpu, NULL,
		event_overflow_callback,
		NULL);

	spin_unlock_irqrestore(&cinfo->lock, flags);

	/* initialize reclaim timer on each cpu */
	smp_call_function_single(cpu, __init_reclaim_timer, NULL, 1);

	if (!cinfo->event)
		return -ENODEV;


	if (!IS_ERR(cinfo->event)) {
		printk(KERN_INFO "memsched: cpu%d enabled counter.\n", cpu);
		goto out_save;
	}

	/* vary the KERN level based on the returned errno */
	if (PTR_ERR(cinfo->event) == -EOPNOTSUPP)
		printk(KERN_INFO "memsched: cpu%i. not supported\n", cpu);
	else if (PTR_ERR(cinfo->event) == -ENOENT)
		printk(KERN_INFO "memsched: cpu%i. not h/w event\n", cpu);
	else
		printk(KERN_ERR "memsched: cpu%i. unable to create perf event: %ld\n",
		       cpu, PTR_ERR(cinfo->event));
	return PTR_ERR(cinfo->event);
	/* success path */
out_save:

	init_irq_work(&cinfo->pending, memsched_process_overflow);

	return 0;
}


static void __disable_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo->event);
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);
	printk(KERN_INFO "LLC bandwidth throttling disabled\n");
}

static void disable_counters(void)
{
	on_each_cpu(__disable_counter, NULL, 0);
}

static void __start_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	perf_event_refresh(cinfo->event, 1);
}

static void start_counters(void)
{
	on_each_cpu(__start_counter, NULL, 0);
}

/**************************************************************************
 * Local Functions
 **************************************************************************/

static ssize_t memsched_limit_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[256];
	char *p = buf;
	int i;
	unsigned long flags;

	copy_from_user(&buf, ubuf, (cnt > 256) ? 256: cnt);
	if (!strncmp(p, "aggr", 4)) {
		p = strchr(p, ' '); p++;
		for_each_online_cpu(i) {
			struct core_info *cinfo = per_cpu_ptr(core_info, i);
			int input;
			sscanf(p, "%d", &input);
			cinfo->intensity = input;
			printk(KERN_DEBUG "New intensity for CPU%d is %d\n", i,
			       per_cpu_ptr(core_info, i)->intensity);
			p = strchr(p, ' ');
			if (!p) break;
			p++;
		}
	} else {
		for_each_online_cpu(i) {
			struct core_info *cinfo = per_cpu_ptr(core_info, i);
			int input;
			u64 events;
			sscanf(p, "%d", &input);
			if (input == 0)
				input = g_budget_min_value;

			events = convert_bandwidth_to_events(input);

			if (spin_trylock_irqsave(&cinfo->lock, flags)) {
				cinfo->budget = events;
				spin_unlock_irqrestore(&cinfo->lock, flags);
				printk(KERN_DEBUG "New budget for CPU%d is %lld (%d pct of max)\n", i,
				       events, input);
			} else {
				printk(KERN_DEBUG "Ooops. locking failed\n");
			}

			p = strchr(p, ' ');
			if (!p) break;
			p++;
		}
	}
	return cnt;
}

static int memsched_limit_show(struct seq_file *m, void *v)
{
	int i;
	seq_printf(m, "budget: ");
	for_each_online_cpu(i)
		seq_printf(m, "%lld ", per_cpu_ptr(core_info, i)->budget);
	seq_printf(m, "\nintensity: ");
	for_each_online_cpu(i)
		seq_printf(m, "%d ", per_cpu_ptr(core_info, i)->intensity);
	seq_printf(m, "\n");

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



static int memsched_usage_show(struct seq_file *m, void *v)
{
	int i, j;
	unsigned long flags;

	/* current utilization */

	for (j = 0; j < 3; j++) {
		for_each_online_cpu(i) {
			struct core_info *cinfo = per_cpu_ptr(core_info, i);
			u64 budget, used, util;

			spin_lock_irqsave(&cinfo->lock, flags);
			budget = cinfo->budget;
			used = cinfo->used[j];
			spin_unlock_irqrestore(&cinfo->lock, flags);

			util = div64_u64(used * 100, budget);
			seq_printf(m, "%llu ", util);
		}
		seq_printf(m, "\n");
	}

	seq_printf(m, "================================================\n");

	/* overall utilization
	   WARN: assume budget did not changed */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		u64 total_budget, total_used, result;

		total_budget = cinfo->overall.assigned_budget;
		total_used   = cinfo->overall.used_budget;
		result       = div64_u64(total_used * 100, total_budget);
		seq_printf(m, "%lld ", result);
	}
	seq_printf(m, "\n");
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


static ssize_t memsched_failcnt_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	int i;
	unsigned long flags;

	/* reset global statistics */
	atomic64_set(&memsched_info.period_cnt, 0);

	smp_mb();
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		spin_lock_irqsave(&cinfo->lock, flags);

		cinfo->overall.used_budget = 0;
		cinfo->overall.assigned_budget = 0;
		cinfo->overall.throttled_time_ns = 0;
		cinfo->overall.throttled = 0;
		cinfo->overall.throttled_error = 0;

		spin_unlock_irqrestore(&cinfo->lock, flags);

	}
	smp_mb();
	return cnt;
}

static int memsched_failcnt_show(struct seq_file *m, void *v)
{
	int i;
	/* total #of throttled periods */
	smp_rmb();

	seq_printf(m, "throttled: ");
	for_each_online_cpu(i)
		seq_printf(m, "%d ", per_cpu_ptr(core_info, i)->overall.throttled);

	seq_printf(m, "\nthrottle error: ");
	for_each_online_cpu(i)
		seq_printf(m, "%d ", per_cpu_ptr(core_info, i)->overall.throttled_error);

	/* out of total periods */
	seq_printf(m, "\ntotal_periods %ld\n", atomic64_read(&memsched_info.period_cnt));
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
	debugfs_create_file("limit", 0444, memsched_dir, NULL,
			    &memsched_limit_fops);


	debugfs_create_file("usage", 0666, memsched_dir, NULL,
			    &memsched_usage_fops);

	debugfs_create_file("failcnt", 0644, memsched_dir, NULL,
			    &memsched_failcnt_fops);
	return 0;
}

int init_module( void )
{
	int i;

	preempt_disable();
	/* Memory performance characteristics */
	if (g_ns_per_event == 0) {
		g_ns_per_event = detect_average_cacheline_cost(8);
		g_budget_min_value = convert_bandwidth_to_events(5); /* 5% */

	}
	printk(KERN_INFO "Avg. cache-line miss cost: %d\n", g_ns_per_event);
	printk(KERN_INFO "Max. events per %d us: %lld\n", g_period_us, 
	       convert_bandwidth_to_events(100));

	/* initialized memsched_info structure */
	atomic64_set(&memsched_info.budget, 0);
	memsched_info.orun = 0;
	memsched_info.period = ktime_set(g_period_us/ 1000000, (g_period_us % 1000000)*1000);
	atomic64_set(&memsched_info.period_cnt, 0);
	hrtimer_init(&memsched_info.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	memsched_info.timer.function = &period_timer_callback;

	/* initialize global variables */
	printk(KERN_INFO "g_period_us: %d\n", g_period_us);

	printk("Initilizing perf counter\n");
	core_info = alloc_percpu(struct core_info);
	for_each_online_cpu(i) {
		/* initialize counter h/w & event structure */
		if (g_budget[i] == 0) /* uninitialized. assign max value */
			g_budget[i] = 100;
		printk(KERN_INFO "budget[%d] = %lld (%d pct)\n", i,
		       convert_bandwidth_to_events(g_budget[i]),
		       g_budget[i]);
		init_counter(i, convert_bandwidth_to_events(g_budget[i]));
	}

	preempt_enable();

	memsched_init_debugfs();

#if __SELF_TEST__
	if (self_test() < 0)
		return -ENODEV;
	else
		return 0;
#endif
	printk("Start event counters\n");
	start_counters();

	printk("Start period timer (period=%lldms)\n", ktime_to_ms(memsched_info.period));
	hrtimer_start( &memsched_info.timer, memsched_info.period, HRTIMER_MODE_REL_PINNED );
	return 0;
}

void cleanup_module( void )
{
	int ret, i;

	/* stop perf_event counters */
	disable_counters();

	/* stop period timer */
	ret = hrtimer_cancel(&memsched_info.timer);
	if (ret)
		trace_printk("The timer was still in use...\n");

	/* destroy perf objects */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);

		perf_event_release_kernel(cinfo->event);

		/* unthrottle tasks before exit */
		if (cinfo->throttled > 0)
			if (unthrottle_rq_cpu(i) < 0)
				trace_printk("failed to unthrottle\n");
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

	/* remove debugfs entries */
	debugfs_remove_recursive(memsched_dir);

#if __SELF_TEST__
	printk("test finished\n");
	return;
#endif
	free_percpu(core_info);

	printk("module uninstalled successfully\n");

	return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Heechul Yun <heechul@illinois.edu>");


#if __SELF_TEST__
/* called hardirq context via IPI */
static void __self_test(void *unused)
{
	int cnt, cnt2;
	int cpu = smp_processor_id();
	struct core_info *cinfo;
	int ntries = 10;

	cinfo = this_cpu_ptr(core_info);
	cinfo->throttled = 0;
	cinfo->throttled_cnt = 0;
retry:
	cnt = throttle_rq_cpu(cpu);
	if (cnt < 0) {
		trace_printk("failed to throttle cpu%d, ret=%d, ntries=%d\n",
			     cpu, cnt, ntries);
		if (ntries-- > 0)
			goto retry;
		cinfo->throttled += -1;
	}
	ntries = 10;
retry_u:
	cnt2 = unthrottle_rq_cpu(cpu);
	if (cnt2 < 0) {
		trace_printk("failed to unthrottled cpu%d, ret=%d, ntries=%d\n",
			     cpu, cnt2, ntries);
		if (ntries-- > 0)
			goto retry_u;
		cinfo->throttled += -1;
	}
	if (cnt != cnt2) {
		trace_printk("count did not match cpu%d. t=%d, u=%d\n",
			     cpu, cnt, cnt2);
		cinfo->throttled += -1;
	}
}

static int self_test(void)
{
	int i, j;

	/* - throttle unthrottle cpus APIs unit test */
	preempt_disable();
	printk(KERN_CRIT "test begin\n");
	for (j = 0; j < g_test; j++ ) {
		/* call other cpus except me via ipi */
		smp_call_function(__self_test, NULL, 1);

		/* check result */
		for_each_online_cpu(i) {
			struct core_info *cinfo =
				per_cpu_ptr(core_info, i);
			if (i == smp_processor_id())
				continue;
			if (cinfo->throttled < 0) {
				printk(KERN_ERR "%d: err on Core%d, ret=%d\n",
				       j, i, cinfo->throttled);
				goto out;
			}
		}
	}
	printk(KERN_CRIT "test success\n");
	preempt_enable();
	return 0;
out:
	preempt_enable();
	return -1;
}

#endif /* __SELF_TEST__ */

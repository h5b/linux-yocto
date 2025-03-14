/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) IBM Corporation, 2001
 *
 * Authors: Dipankar Sarma <dipankar@in.ibm.com>
 *	    Manfred Spraul <manfred@colorfullife.com>
 * 
 * Based on the original work by Paul McKenney <paulmck@us.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 * Papers:
 * http://www.rdrop.com/users/paulmck/paper/rclockpdcsproof.pdf
 * http://lse.sourceforge.net/locking/rclock_OLS.2001.05.01c.sc.pdf (OLS2001)
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 * 		http://lse.sourceforge.net/locking/rcupdate.html
 *
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/rcupdate.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <asm/atomic.h>
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/rcupdate.h>
#include <linux/cpu.h>

/* Definition for rcupdate control block. */
struct rcu_ctrlblk rcu_ctrlblk = {
	.cur = -300,
	.completed = -300,
	.lock = SPIN_LOCK_UNLOCKED,
	.cpumask = CPU_MASK_NONE,
};
struct rcu_ctrlblk rcu_bh_ctrlblk = {
	.cur = -300,
	.completed = -300,
	.lock = SPIN_LOCK_UNLOCKED,
	.cpumask = CPU_MASK_NONE,
};

DEFINE_PER_CPU(struct rcu_data, rcu_data) = { 0L };
DEFINE_PER_CPU(struct rcu_data, rcu_bh_data) = { 0L };

/* Fake initialization required by compiler */
static DEFINE_PER_CPU(struct tasklet_struct, rcu_tasklet) = {NULL};
static int maxbatch = 10000;

/**
 * call_rcu - Queue an RCU callback for invocation after a grace period.
 * @head: structure to be used for queueing the RCU updates.
 * @func: actual update function to be invoked after the grace period
 *
 * The update function will be invoked some time after a full grace
 * period elapses, in other words after all currently executing RCU
 * read-side critical sections have completed.  RCU read-side critical
 * sections are delimited by rcu_read_lock() and rcu_read_unlock(),
 * and may be nested.
 */
void fastcall call_rcu(struct rcu_head *head,
				void (*func)(struct rcu_head *rcu))
{
	unsigned long flags;
	struct rcu_data *rdp;

	head->func = func;
	head->next = NULL;
	local_irq_save(flags);
	rdp = &__get_cpu_var(rcu_data);
	*rdp->nxttail = head;
	rdp->nxttail = &head->next;

	if (unlikely(++rdp->count > 10000))
		set_need_resched();

	local_irq_restore(flags);
}

static atomic_t rcu_barrier_cpu_count;
static struct semaphore rcu_barrier_sema;
static struct completion rcu_barrier_completion;

/**
 * call_rcu_bh - Queue an RCU for invocation after a quicker grace period.
 * @head: structure to be used for queueing the RCU updates.
 * @func: actual update function to be invoked after the grace period
 *
 * The update function will be invoked some time after a full grace
 * period elapses, in other words after all currently executing RCU
 * read-side critical sections have completed. call_rcu_bh() assumes
 * that the read-side critical sections end on completion of a softirq
 * handler. This means that read-side critical sections in process
 * context must not be interrupted by softirqs. This interface is to be
 * used when most of the read-side critical sections are in softirq context.
 * RCU read-side critical sections are delimited by rcu_read_lock() and
 * rcu_read_unlock(), * if in interrupt context or rcu_read_lock_bh()
 * and rcu_read_unlock_bh(), if in process context. These may be nested.
 */
void fastcall call_rcu_bh(struct rcu_head *head,
				void (*func)(struct rcu_head *rcu))
{
	unsigned long flags;
	struct rcu_data *rdp;

	head->func = func;
	head->next = NULL;
	local_irq_save(flags);
	rdp = &__get_cpu_var(rcu_bh_data);
	*rdp->nxttail = head;
	rdp->nxttail = &head->next;
	rdp->count++;
/*
 *  Should we directly call rcu_do_batch() here ?
 *  if (unlikely(rdp->count > 10000))
 *      rcu_do_batch(rdp);
 */
	local_irq_restore(flags);
}

/*
 * Return the number of RCU batches processed thus far.  Useful
 * for debug and statistics.
 */
long rcu_batches_completed(void)
{
	return rcu_ctrlblk.completed;
}

static void rcu_barrier_callback(struct rcu_head *notused)
{
	if (atomic_dec_and_test(&rcu_barrier_cpu_count))
		complete(&rcu_barrier_completion);
}

/*
 * Called with preemption disabled, and from cross-cpu IRQ context.
 */
static void rcu_barrier_func(void *notused)
{
	int cpu = smp_processor_id();
	struct rcu_data *rdp = &per_cpu(rcu_data, cpu);
	struct rcu_head *head;

	head = &rdp->barrier;
	atomic_inc(&rcu_barrier_cpu_count);
	call_rcu(head, rcu_barrier_callback);
}

/**
 * rcu_barrier - Wait until all the in-flight RCUs are complete.
 */
void rcu_barrier(void)
{
	BUG_ON(in_interrupt());
	/* Take cpucontrol semaphore to protect against CPU hotplug */
	down(&rcu_barrier_sema);
	init_completion(&rcu_barrier_completion);
	atomic_set(&rcu_barrier_cpu_count, 0);
	on_each_cpu(rcu_barrier_func, NULL, 0, 1);
	wait_for_completion(&rcu_barrier_completion);
	up(&rcu_barrier_sema);
}
EXPORT_SYMBOL_GPL(rcu_barrier);

/*
 * Invoke the completed RCU callbacks. They are expected to be in
 * a per-cpu list.
 */
static void rcu_do_batch(struct rcu_data *rdp)
{
	struct rcu_head *next, *list;
	int count = 0;

	list = rdp->donelist;
	while (list) {
		next = rdp->donelist = list->next;
		list->func(list);
		list = next;
		rdp->count--;
		if (++count >= maxbatch)
			break;
	}
	if (!rdp->donelist)
		rdp->donetail = &rdp->donelist;
	else
		tasklet_schedule(&per_cpu(rcu_tasklet, rdp->cpu));
}

/*
 * Grace period handling:
 * The grace period handling consists out of two steps:
 * - A new grace period is started.
 *   This is done by rcu_start_batch. The start is not broadcasted to
 *   all cpus, they must pick this up by comparing rcp->cur with
 *   rdp->quiescbatch. All cpus are recorded  in the
 *   rcu_ctrlblk.cpumask bitmap.
 * - All cpus must go through a quiescent state.
 *   Since the start of the grace period is not broadcasted, at least two
 *   calls to rcu_check_quiescent_state are required:
 *   The first call just notices that a new grace period is running. The
 *   following calls check if there was a quiescent state since the beginning
 *   of the grace period. If so, it updates rcu_ctrlblk.cpumask. If
 *   the bitmap is empty, then the grace period is completed.
 *   rcu_check_quiescent_state calls rcu_start_batch(0) to start the next grace
 *   period (if necessary).
 */
/*
 * Register a new batch of callbacks, and start it up if there is currently no
 * active batch and the batch to be registered has not already occurred.
 * Caller must hold rcu_ctrlblk.lock.
 */
static void rcu_start_batch(struct rcu_ctrlblk *rcp)
{
	if (rcp->next_pending &&
			rcp->completed == rcp->cur) {
		rcp->next_pending = 0;
		/*
		 * next_pending == 0 must be visible in
		 * __rcu_process_callbacks() before it can see new value of cur.
		 */
		smp_wmb();
		rcp->cur++;

		/*
		 * Accessing nohz_cpu_mask before incrementing rcp->cur needs a
		 * Barrier  Otherwise it can cause tickless idle CPUs to be
		 * included in rcp->cpumask, which will extend graceperiods
		 * unnecessarily.
		 */
		smp_mb();
		cpus_andnot(rcp->cpumask, cpu_online_map, nohz_cpu_mask);

	}
}

/*
 * cpu went through a quiescent state since the beginning of the grace period.
 * Clear it from the cpu mask and complete the grace period if it was the last
 * cpu. Start another grace period if someone has further entries pending
 */
static void cpu_quiet(int cpu, struct rcu_ctrlblk *rcp)
{
	cpu_clear(cpu, rcp->cpumask);
	if (cpus_empty(rcp->cpumask)) {
		/* batch completed ! */
		rcp->completed = rcp->cur;
		rcu_start_batch(rcp);
	}
}

/*
 * Check if the cpu has gone through a quiescent state (say context
 * switch). If so and if it already hasn't done so in this RCU
 * quiescent cycle, then indicate that it has done so.
 */
static void rcu_check_quiescent_state(struct rcu_ctrlblk *rcp,
					struct rcu_data *rdp)
{
	if (rdp->quiescbatch != rcp->cur) {
		/* start new grace period: */
		rdp->qs_pending = 1;
		rdp->passed_quiesc = 0;
		rdp->quiescbatch = rcp->cur;
		return;
	}

	/* Grace period already completed for this cpu?
	 * qs_pending is checked instead of the actual bitmap to avoid
	 * cacheline trashing.
	 */
	if (!rdp->qs_pending)
		return;

	/* 
	 * Was there a quiescent state since the beginning of the grace
	 * period? If no, then exit and wait for the next call.
	 */
	if (!rdp->passed_quiesc)
		return;
	rdp->qs_pending = 0;

	spin_lock(&rcp->lock);
	/*
	 * rdp->quiescbatch/rcp->cur and the cpu bitmap can come out of sync
	 * during cpu startup. Ignore the quiescent state.
	 */
	if (likely(rdp->quiescbatch == rcp->cur))
		cpu_quiet(rdp->cpu, rcp);

	spin_unlock(&rcp->lock);
}


#ifdef CONFIG_HOTPLUG_CPU

/* warning! helper for rcu_offline_cpu. do not use elsewhere without reviewing
 * locking requirements, the list it's pulling from has to belong to a cpu
 * which is dead and hence not processing interrupts.
 */
static void rcu_move_batch(struct rcu_data *this_rdp, struct rcu_head *list,
				struct rcu_head **tail)
{
	local_irq_disable();
	*this_rdp->nxttail = list;
	if (list)
		this_rdp->nxttail = tail;
	local_irq_enable();
}

static void __rcu_offline_cpu(struct rcu_data *this_rdp,
				struct rcu_ctrlblk *rcp, struct rcu_data *rdp)
{
	/* if the cpu going offline owns the grace period
	 * we can block indefinitely waiting for it, so flush
	 * it here
	 */
	spin_lock_bh(&rcp->lock);
	if (rcp->cur != rcp->completed)
		cpu_quiet(rdp->cpu, rcp);
	spin_unlock_bh(&rcp->lock);
	rcu_move_batch(this_rdp, rdp->curlist, rdp->curtail);
	rcu_move_batch(this_rdp, rdp->nxtlist, rdp->nxttail);
	rcu_move_batch(this_rdp, rdp->donelist, rdp->donetail);
}

static void rcu_offline_cpu(int cpu)
{
	struct rcu_data *this_rdp = &get_cpu_var(rcu_data);
	struct rcu_data *this_bh_rdp = &get_cpu_var(rcu_bh_data);

	__rcu_offline_cpu(this_rdp, &rcu_ctrlblk,
					&per_cpu(rcu_data, cpu));
	__rcu_offline_cpu(this_bh_rdp, &rcu_bh_ctrlblk,
					&per_cpu(rcu_bh_data, cpu));
	put_cpu_var(rcu_data);
	put_cpu_var(rcu_bh_data);
	tasklet_kill_immediate(&per_cpu(rcu_tasklet, cpu), cpu);
}

#else

static void rcu_offline_cpu(int cpu)
{
}

#endif

/*
 * This does the RCU processing work from tasklet context. 
 */
static void __rcu_process_callbacks(struct rcu_ctrlblk *rcp,
					struct rcu_data *rdp)
{
	if (rdp->curlist && !rcu_batch_before(rcp->completed, rdp->batch)) {
		*rdp->donetail = rdp->curlist;
		rdp->donetail = rdp->curtail;
		rdp->curlist = NULL;
		rdp->curtail = &rdp->curlist;
	}

	local_irq_disable();
	if (rdp->nxtlist && !rdp->curlist) {
		rdp->curlist = rdp->nxtlist;
		rdp->curtail = rdp->nxttail;
		rdp->nxtlist = NULL;
		rdp->nxttail = &rdp->nxtlist;
		local_irq_enable();

		/*
		 * start the next batch of callbacks
		 */

		/* determine batch number */
		rdp->batch = rcp->cur + 1;
		/* see the comment and corresponding wmb() in
		 * the rcu_start_batch()
		 */
		smp_rmb();

		if (!rcp->next_pending) {
			/* and start it/schedule start if it's a new batch */
			spin_lock(&rcp->lock);
			rcp->next_pending = 1;
			rcu_start_batch(rcp);
			spin_unlock(&rcp->lock);
		}
	} else {
		local_irq_enable();
	}
	rcu_check_quiescent_state(rcp, rdp);
	if (rdp->donelist)
		rcu_do_batch(rdp);
}

static void rcu_process_callbacks(unsigned long unused)
{
	__rcu_process_callbacks(&rcu_ctrlblk, &__get_cpu_var(rcu_data));
	__rcu_process_callbacks(&rcu_bh_ctrlblk, &__get_cpu_var(rcu_bh_data));
}

static int __rcu_pending(struct rcu_ctrlblk *rcp, struct rcu_data *rdp)
{
	/* This cpu has pending rcu entries and the grace period
	 * for them has completed.
	 */
	if (rdp->curlist && !rcu_batch_before(rcp->completed, rdp->batch))
		return 1;

	/* This cpu has no pending entries, but there are new entries */
	if (!rdp->curlist && rdp->nxtlist)
		return 1;

	/* This cpu has finished callbacks to invoke */
	if (rdp->donelist)
		return 1;

	/* The rcu core waits for a quiescent state from the cpu */
	if (rdp->quiescbatch != rcp->cur || rdp->qs_pending)
		return 1;

	/* nothing to do */
	return 0;
}

int rcu_pending(int cpu)
{
	return __rcu_pending(&rcu_ctrlblk, &per_cpu(rcu_data, cpu)) ||
		__rcu_pending(&rcu_bh_ctrlblk, &per_cpu(rcu_bh_data, cpu));
}

void rcu_check_callbacks(int cpu, int user)
{
	if (user || 
	    (idle_cpu(cpu) && !in_softirq() && 
				hardirq_count() <= (1 << HARDIRQ_SHIFT))) {
		rcu_qsctr_inc(cpu);
		rcu_bh_qsctr_inc(cpu);
	} else if (!in_softirq())
		rcu_bh_qsctr_inc(cpu);
	tasklet_schedule(&per_cpu(rcu_tasklet, cpu));
}

static void rcu_init_percpu_data(int cpu, struct rcu_ctrlblk *rcp,
						struct rcu_data *rdp)
{
	memset(rdp, 0, sizeof(*rdp));
	rdp->curtail = &rdp->curlist;
	rdp->nxttail = &rdp->nxtlist;
	rdp->donetail = &rdp->donelist;
	rdp->quiescbatch = rcp->completed;
	rdp->qs_pending = 0;
	rdp->cpu = cpu;
}

static void __devinit rcu_online_cpu(int cpu)
{
	struct rcu_data *rdp = &per_cpu(rcu_data, cpu);
	struct rcu_data *bh_rdp = &per_cpu(rcu_bh_data, cpu);

	rcu_init_percpu_data(cpu, &rcu_ctrlblk, rdp);
	rcu_init_percpu_data(cpu, &rcu_bh_ctrlblk, bh_rdp);
	tasklet_init(&per_cpu(rcu_tasklet, cpu), rcu_process_callbacks, 0UL);
}

static int __devinit rcu_cpu_notify(struct notifier_block *self, 
				unsigned long action, void *hcpu)
{
	long cpu = (long)hcpu;
	switch (action) {
	case CPU_UP_PREPARE:
		rcu_online_cpu(cpu);
		break;
	case CPU_DEAD:
		rcu_offline_cpu(cpu);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block __devinitdata rcu_nb = {
	.notifier_call	= rcu_cpu_notify,
};

/*
 * Initializes rcu mechanism.  Assumed to be called early.
 * That is before local timer(SMP) or jiffie timer (uniproc) is setup.
 * Note that rcu_qsctr and friends are implicitly
 * initialized due to the choice of ``0'' for RCU_CTR_INVALID.
 */
void __init rcu_init(void)
{
	sema_init(&rcu_barrier_sema, 1);
	rcu_cpu_notify(&rcu_nb, CPU_UP_PREPARE,
			(void *)(long)smp_processor_id());
	/* Register notifier for non-boot CPUs */
	register_cpu_notifier(&rcu_nb);
}

struct rcu_synchronize {
	struct rcu_head head;
	struct completion completion;
};

/* Because of FASTCALL declaration of complete, we use this wrapper */
static void wakeme_after_rcu(struct rcu_head  *head)
{
	struct rcu_synchronize *rcu;

	rcu = container_of(head, struct rcu_synchronize, head);
	complete(&rcu->completion);
}

/**
 * synchronize_rcu - wait until a grace period has elapsed.
 *
 * Control will return to the caller some time after a full grace
 * period has elapsed, in other words after all currently executing RCU
 * read-side critical sections have completed.  RCU read-side critical
 * sections are delimited by rcu_read_lock() and rcu_read_unlock(),
 * and may be nested.
 *
 * If your read-side code is not protected by rcu_read_lock(), do -not-
 * use synchronize_rcu().
 */
void synchronize_rcu(void)
{
	struct rcu_synchronize rcu;

	init_completion(&rcu.completion);
	/* Will wake me after RCU finished */
	call_rcu(&rcu.head, wakeme_after_rcu);

	/* Wait for it */
	wait_for_completion(&rcu.completion);
}

/*
 * Deprecated, use synchronize_rcu() or synchronize_sched() instead.
 */
void synchronize_kernel(void)
{
	synchronize_rcu();
}

module_param(maxbatch, int, 0);
EXPORT_SYMBOL_GPL(rcu_batches_completed);
EXPORT_SYMBOL(call_rcu);  /* WARNING: GPL-only in April 2006. */
EXPORT_SYMBOL(call_rcu_bh);  /* WARNING: GPL-only in April 2006. */
EXPORT_SYMBOL_GPL(synchronize_rcu);
EXPORT_SYMBOL(synchronize_kernel);  /* WARNING: GPL-only in April 2006. */

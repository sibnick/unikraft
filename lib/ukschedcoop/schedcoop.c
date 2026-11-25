/* SPDX-License-Identifier: MIT */
/*
 * Authors: Grzegorz Milos
 *          Robert Kaiser
 *          Costin Lupu <costin.lupu@cs.pub.ro>
 *
 * Copyright (c) 2005, Intel Research Cambridge
 * Copyright (c) 2017, NEC Europe Ltd., NEC Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
/*
 * The scheduler is non-preemptive (cooperative), and schedules according
 * to Round Robin algorithm.
 */
#include <uk/plat/config.h>
#include <uk/lcpu.h>
#include <uk/plat/memory.h>
#include <uk/plat/time.h>
#include <uk/atomic.h>
#include <uk/sched_impl.h>
#include <uk/schedcoop.h>
#include <uk/essentials.h>
#include "schedcoop.h"

/* Upper bound for an idle halt in multicore images, so that threads
 * handed over through the affinity inbox are taken in within a short
 * time even when the destination CPU has no other reason to wake up.
 */
#define UK_SCHEDCOOP_IDLE_POLL_NS (10000000LL) /* 10 ms */

/* CPU-migration inbox (see `uk_sched.thread_inbox`).
 *
 * Producers on other CPUs push a not-queued thread here with a CAS,
 * chaining through the `queue` link the thread owns while it belongs to
 * no queue. The owner CPU takes the whole chain with one exchange and
 * enqueues the threads on its own queues, so no scheduler list is ever
 * mutated from two CPUs at the same time.
 */
static int schedcoop_thread_inbox(struct uk_sched *s, struct uk_thread *t)
{
	struct schedcoop *c = uksched2schedcoop(s);
	struct uk_thread *head;

	do {
		head = uk_load_n(&c->mbox);
		t->queue.tqe_next = head;
	} while (!uk_compare_exchange_n(&c->mbox, &head, t));

	return 0;
}

/* Drain the inbox: called from the scheduler's own CPU with interrupts
 * off, from within the schedule path.
 */
static void schedcoop_inbox_drain(struct schedcoop *c)
{
	struct uk_thread *t, *next;

	t = uk_exchange_n(&c->mbox, NULL);
	while (t) {
		next = t->queue.tqe_next;
		t->queue.tqe_next = NULL;

		if (unlikely(uk_thread_is_exited(t))) {
			t = next;
			continue;
		}
		if (uk_thread_is_runnable(t))
			UK_TAILQ_INSERT_TAIL(&c->run_queue, t, queue);
		else if (t->wakeup_time > 0)
			/* Blocked with a deadline that moved here:
			 * re-arm it on this scheduler's sleep queue so
			 * the timeout keeps being tracked.
			 */
			UK_TAILQ_INSERT_TAIL(&c->sleep_queue, t, queue);

		t = next;
	}
}

static void schedcoop_schedule(struct uk_sched *s)
{
	struct schedcoop *c = uksched2schedcoop(s);
	struct uk_thread *prev, *next, *thread, *tmp;
	__snsec now, min_wakeup_time;
	unsigned long flags;
	int cpu;

	if (unlikely(uk_lcpu_irqs_disabled()))
		UK_CRASH("Must not call %s with IRQs disabled\n", __func__);

	now = ukplat_monotonic_clock();
	prev = uk_thread_current();
	flags = uk_lcpu_save_irqf();

#if 0 // TODO
	if (in_callback)
		UK_CRASH("Must not call %s from a callback\n", __func__);
#endif

	/* Update execution time of current thread */
	/* WARNING: We assume here that scheduler `s` is only responsible for
	 *          the current logical CPU. Otherwise, we would have to store
	 *          the time of the last context switch per logical core.
	 */
	prev->exec_time += now - c->ts_prev_switch;
	c->ts_prev_switch = now;

	/* The scheduler instances with affinity support maintain the
	 * EXECUTING marker: `prev` is the running thread of this CPU.
	 */
	uk_thread_set_executing(prev);

	cpu = uk_sched_current_cpu();

	/* Examine all sleeping threads.
	 * Wake up expired ones and find the time when the next timeout expires.
	 */
	min_wakeup_time = 0;
	UK_TAILQ_FOREACH_SAFE(thread, &c->sleep_queue, queue, tmp)
	{
		if (likely(thread->wakeup_time)) {
			/* A migrate_pending thread was affinity-moved
			 * while parked here: cut its deadline and
			 * finish the move now instead of waiting.
			 */
			if (thread->wakeup_time <= now ||
			    unlikely(thread->migrate_pending)) {
				uk_thread_wake(thread);
				/* Finish an affinity hand-off: the thread
				 * is runnable and queued on this CPU now,
				 * safely off its stack. Take it out of the
				 * queue and move it to the scheduler of an
				 * allowed CPU.
				 */
				if (unlikely(thread->migrate_pending)) {
					thread->migrate_pending = 0;
					UK_TAILQ_REMOVE(&c->run_queue,
							thread, queue);
					if (uk_sched_thread_rehome_dequeued(
						    thread) != 0)
						/* Nowhere to go or failed:
						 * queue it here.
						 */
						UK_TAILQ_INSERT_TAIL(
							&c->run_queue,
							thread, queue);
				}
			} else if (!min_wakeup_time ||
				   thread->wakeup_time < min_wakeup_time)
				min_wakeup_time = thread->wakeup_time;
		}
	}

	/* Take in threads handed over by affinity migrations from other
	 * CPUs before we pick.
	 */
	if (unlikely(uk_load_n(&c->mbox)))
		schedcoop_inbox_drain(c);

	/* Pick the first runnable thread that is allowed to run on this
	 * CPU. Threads whose affinity mask excludes this CPU move to the
	 * scheduler of an allowed CPU.
	 */
	next = NULL;
	UK_TAILQ_FOREACH_SAFE(thread, &c->run_queue, queue, tmp)
	{
		if (cpu >= 0 &&
		    !uk_thread_affinity_allows(thread, (unsigned int)cpu)) {
			UK_TAILQ_REMOVE(&c->run_queue, thread, queue);
			uk_thread_set_queueable(thread);
			/* On failure the thread is re-added to this
			 * queue, so it cannot get lost.
			 */
			uk_sched_thread_rehome_dequeued(thread);
			continue;
		}
		next = thread;
		UK_TAILQ_REMOVE(&c->run_queue, next, queue);

		UK_ASSERT(next != prev);
		UK_ASSERT(uk_thread_is_runnable(next));
		UK_ASSERT(!uk_thread_is_exited(next));
		break;
	}

	if (next) {
		/* Put previous thread on the end of the list, unless its
		 * affinity excludes this CPU. Then it hands off to the
		 * destination CPU instead (blocked here, enqueued
		 * there), so the switch below leaves it safely.
		 */
		if ((prev != &c->idle) && uk_thread_is_runnable(prev) &&
		    !uk_thread_is_exited(prev)) {
			if (cpu < 0 ||
			    uk_thread_affinity_allows(prev, (unsigned int)cpu))
				UK_TAILQ_INSERT_TAIL(&c->run_queue, prev,
						     queue);
			else if (uk_sched_thread_migrate_handoff(prev) != 0)
				/* Nowhere to go or failed: keep running
				 * it here until the next pick.
				 */
				UK_TAILQ_INSERT_TAIL(&c->run_queue, prev,
						     queue);
		}
	} else if (uk_thread_is_runnable(prev) && !uk_thread_is_exited(prev) &&
		   (cpu < 0 ||
		    uk_thread_affinity_allows(prev, (unsigned int)cpu))) {
		next = prev;
	} else if (uk_thread_is_runnable(prev) && !uk_thread_is_exited(prev)) {
		/* The running thread must leave this CPU and nothing
		 * allowed here is queued. Start the hand-off and idle:
		 * the idle loop re-runs schedule() immediately (expired
		 * deadline), which completes the move. If the thread
		 * has nowhere to go (no started scheduler on an allowed
		 * CPU yet), it stays.
		 */
		if (uk_sched_thread_migrate_handoff(prev) == 0) {
			next = &c->idle;
			c->idle_return_time = 1;
			uk_sched_stats_idle_count_incr(s);
		} else
			next = prev;
	} else {
		/*
		 * Schedule idle thread that will halt the CPU
		 * We select the idle thread only if we do not have anything
		 * else to execute
		 */
		c->idle_return_time = min_wakeup_time;
		next = &c->idle;
		uk_sched_stats_idle_count_incr(s);
	}

	if (next != prev) {
		/*
		 * Queueable is used to cover the case when during a
		 * context switch, the thread that is about to be
		 * evacuated is interrupted and woken up.
		 */
		uk_thread_set_queueable(prev);
		uk_thread_clear_queueable(next);

		/* `next` becomes the executing thread of this CPU; `prev`
		 * stops executing when the context switch below leaves it.
		 * The affinity code must not move `prev` out of the queue
		 * it is on while it still executes here, so we clear the
		 * marker only now and set it on `next`.
		 */
		uk_thread_clear_executing(prev);
		uk_thread_set_executing(next);
	}

	uk_sched_stats_sched_count_incr(s);

	uk_lcpu_restore_irqf(flags);

	/* Interrupting the switch is equivalent to having the next thread
	 * interrupted at the return instruction. And therefore at safe point.
	 */
	if (prev != next) {
		if (next != &c->idle)
			uk_sched_stats_next_count_incr(s);
		uk_sched_thread_switch(next);
	}
}

static void schedcoop_yield(struct uk_sched *s)
{
	uk_sched_stats_yield_count_incr(s);
	schedcoop_schedule(s);
}

static int schedcoop_thread_add(struct uk_sched *s, struct uk_thread *t)
{
	struct schedcoop *c = uksched2schedcoop(s);

	UK_ASSERT(t);
	UK_ASSERT(!uk_thread_is_exited(t));

	/* Add to run queue if runnable */
	if (uk_thread_is_runnable(t))
		UK_TAILQ_INSERT_TAIL(&c->run_queue, t, queue);

	return 0;
}

static void schedcoop_thread_remove(struct uk_sched *s, struct uk_thread *t)
{
	struct schedcoop *c = uksched2schedcoop(s);

	/* Remove from run_queue */
	if (t != uk_thread_current() && uk_thread_is_runnable(t))
		UK_TAILQ_REMOVE(&c->run_queue, t, queue);
	else if (!uk_thread_is_runnable(t) && t->wakeup_time > 0)
		/* Blocked with a deadline: the thread sits on the sleep
		 * queue. Leaving the link behind would corrupt the queue
		 * when the thread moves to another CPU via affinity.
		 */
		UK_TAILQ_REMOVE(&c->sleep_queue, t, queue);
}

static void schedcoop_thread_blocked(struct uk_sched *s, struct uk_thread *t)
{
	struct schedcoop *c = uksched2schedcoop(s);

	UK_ASSERT(uk_lcpu_irqs_disabled());

	if (t != uk_thread_current())
		UK_TAILQ_REMOVE(&c->run_queue, t, queue);
	if (t->wakeup_time > 0)
		UK_TAILQ_INSERT_TAIL(&c->sleep_queue, t, queue);
}

static __noreturn void idle_thread_fn(void *argp)
{
	struct schedcoop *c = (struct schedcoop *)argp;
	__nsec now, wake_up_time;
	unsigned long flags;

	UK_ASSERT(c);

	for (;;) {
		flags = uk_lcpu_save_irqf();

		/*
		 * FIXME: We assume that `uk_sched_thread_gc()` is non-blocking
		 *        because we implement a cooperative scheduler. However,
		 *        this assumption may not be true depending on the
		 *        destructor functions that are assigned to the threads
		 *        and are called by `uk_sched_thred_gc()`.
		 *	  Also check if in the meantime we got a runnable
		 *	  thread.
		 * NOTE:  This idle thread must be non-blocking so that the
		 *        scheduler has always something to schedule.
		 */
		if (uk_sched_thread_gc(&c->sched) > 0 ||
		    UK_TAILQ_FIRST(&c->run_queue) ||
		    uk_load_n(&c->mbox)) {
			/* We collected successfully some garbage or there is
			 * a runnable thread in the queue.
			 * Check if something else can be scheduled now.
			 */
			uk_lcpu_restore_irqf(flags);

			/* Use yield() here instead of schedule(), as the
			 * latter would cause num_sched to exceed num_yield,
			 * which would errneously imply a preemption, as
			 * num_preempt = num_sched - num_yield
			 */
			schedcoop_yield(&c->sched);

			continue;
		}

		/* Read return time set by last schedule operation */
		wake_up_time = (volatile __nsec)c->idle_return_time;
		now = ukplat_monotonic_clock();

		/* In a multicore image this CPU may receive threads
		 * through its affinity inbox at any moment, and the
		 * sender cannot raise an interrupt here (cross-CPU
		 * kicks are not reliable on all platforms). Bound the
		 * halt so the inbox is drained promptly instead of
		 * waiting for an event that may never come.
		 */
		if (uk_sched_head && uk_sched_head->next) {
			__nsec cap = now + UK_SCHEDCOOP_IDLE_POLL_NS;

			if (!wake_up_time || wake_up_time > cap)
				wake_up_time = cap;
		}

		if (!wake_up_time || wake_up_time > now) {
			if (wake_up_time)
				uk_lcpu_halt_irq_until(wake_up_time);
			else
				uk_lcpu_halt_irq();

			/* handle pending events if any */
			uk_lcpu_irqs_handle_pending();
		}

		uk_lcpu_restore_irqf(flags);

		/* Try to schedule a thread that might now be available.
		 * Use yield() here instead of schedule(), as the
		 * latter would cause num_sched to exceed num_yield,
		 * which would errneously imply a preemption, as
		 * num_preempt = num_sched - num_yield
		 */
		schedcoop_yield(&c->sched);
	}
}

static int schedcoop_start(struct uk_sched *s,
			   struct uk_thread *main_thread __maybe_unused)
{
	struct schedcoop *c = uksched2schedcoop(s);

	UK_ASSERT(main_thread);
	UK_ASSERT(main_thread->sched == s);
	UK_ASSERT(uk_thread_is_runnable(main_thread));
	UK_ASSERT(!uk_thread_is_exited(main_thread));
	UK_ASSERT(uk_thread_current() == main_thread);

	/* Since we are now starting to schedule, we save the current timestamp
	 * as the start time for the first time slice.
	 */
	c->ts_prev_switch = ukplat_monotonic_clock();

	/* NOTE: We do not put `main_thread` into the thread list.
	 *       Current running threads will be added as soon as
	 *       a different thread is scheduled.
	 */

	uk_lcpu_enable_irq();

	return 0;
}

static const struct uk_thread *schedcoop_idle_thread(struct uk_sched *s,
						     unsigned int proc_id)
{
	struct schedcoop *c = uksched2schedcoop(s);

	/* NOTE: We only support one processing LCPU (for now) */
	if (proc_id > 0)
		return NULL;

	return &(c->idle);
}

struct uk_sched *uk_schedcoop_create(struct uk_alloc *a, struct uk_alloc *sa,
				     struct uk_alloc *auxsa,
				     struct uk_alloc *tls_a)
{
	struct schedcoop *c = NULL;
	int rc;

	uk_pr_info("Initializing cooperative scheduler\n");
	c = uk_zalloc(a, sizeof(struct schedcoop));
	if (!c)
		goto err_out;

	UK_TAILQ_INIT(&c->run_queue);
	UK_TAILQ_INIT(&c->sleep_queue);

	/* Create idle thread */
	rc = uk_thread_init_fn1(&c->idle, idle_thread_fn, (void *)c, sa,
				STACK_SIZE, auxsa, AUXSTACK_SIZE, a, false,
				NULL, "idle", NULL, NULL);
	if (rc < 0)
		goto err_free_c;

	c->idle.sched = &c->sched;

	uk_sched_init(&c->sched, schedcoop_start, schedcoop_yield,
		      schedcoop_thread_add, schedcoop_thread_remove,
		      schedcoop_thread_blocked, schedcoop_thread_woken_isr,
		      schedcoop_thread_woken_isr, schedcoop_idle_thread, a, sa,
		      auxsa, tls_a);
	c->sched.thread_inbox = schedcoop_thread_inbox;

	/* Add idle thread to the scheduler's thread list */
	UK_TAILQ_INSERT_TAIL(&c->sched.thread_list, &c->idle, thread_list);

	return &c->sched;

err_free_c:
	uk_free(a, c);
err_out:
	return NULL;
}

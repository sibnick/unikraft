/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2026, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/* Thread CPU affinity.
 *
 * SMP Unikraft runtimes run one scheduler instance per logical CPU:
 * every CPU starts its own scheduler, and `uk_sched_start()` records the
 * CPU index in `s->cpu_id`. A thread's placement is the scheduler that
 * owns it. Setting an affinity mask that excludes the owner CPU therefore
 * means moving the thread to the scheduler of an allowed CPU.
 *
 * Migration rules, by state of the target thread:
 *   - Blocked or queued (not running anywhere): moved immediately.
 *   - Running on the CPU that sets the affinity, or running on any CPU
 *     at a scheduler pick point: migrated through a hand-off (see
 *     `uk_sched_thread_migrate_handoff()`), because the cooperative
 *     scheduler must never place a live thread on a foreign run queue.
 *   - Running on a foreign CPU: the request is recorded and the owner
 *     CPU performs the hand-off at the thread's next schedule point.
 *   - The mask of the calling thread takes effect at its next schedule
 *     point even when the current CPU becomes forbidden.
 *
 * Locking: a global migration lock serializes moves, so two concurrent
 * pins cannot corrupt the queues. The per-scheduler run queues keep the
 * locking discipline they already had before affinity existed: queue
 * operations run with interrupts off on the acting CPU, the same
 * discipline that cross-CPU `uk_thread_wake()` already relies on.
 */

#include <errno.h>
#include <string.h>

#include <uk/atomic.h>
#include <uk/assert.h>
#include <uk/cpumask.h>
#include <uk/lcpu.h>
#include <uk/pcpuvar.h>
#include <uk/plat/config.h>
#include <uk/plat/time.h>
#include <uk/print.h>
#include <uk/sched.h>
#include <uk/sched_impl.h>
#include <uk/thread.h>

/* Global migration lock: guards thread moves between schedulers.
 * Acquired with interrupts off, same idiom as the buddy allocator lock.
 */
static unsigned int affinity_migration_lock;

static unsigned long affinity_lock_acquire(void)
{
	unsigned long flags = uk_lcpu_save_irqf();
	unsigned int exp = 0;

	while (!uk_compare_exchange_n(&affinity_migration_lock, &exp, 1u))
		exp = 0;

	return flags;
}

static void affinity_lock_release(unsigned long flags)
{
	uk_exchange_n(&affinity_migration_lock, 0u);
	uk_lcpu_restore_irqf(flags);
}

int uk_sched_current_cpu(void)
{
#if CONFIG_HAVE_SMP
	__u64 idx = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);

	if (idx < UK_CPUMASK_BITS)
		return (int)idx;
	return -1;
#else  /* !CONFIG_HAVE_SMP */
	return 0;
#endif /* CONFIG_HAVE_SMP */
}

static int cpu_is_online(unsigned int cpu)
{
#if CONFIG_HAVE_SMP
	struct uk_lcpu *lcpu;

	if (cpu >= UK_CPUMASK_BITS)
		return 0;
	lcpu = &uk_pcpuvar_lval((__u64)cpu, uk_lcpus);
	return uk_lcpu_state_is_online(lcpu->state);
#else  /* !CONFIG_HAVE_SMP */
	return cpu == 0;
#endif /* CONFIG_HAVE_SMP */
}

void uk_cpumask_online(struct uk_cpumask *mask)
{
	unsigned int cpu;

	uk_cpumask_zero(mask);
	for (cpu = 0; cpu < UK_CPUMASK_BITS; cpu++)
		if (cpu_is_online(cpu))
			uk_cpumask_set_cpu(cpu, mask);

	/* Before LCPU init finishes, or on a single-core image without
	 * LCPU state, the calling CPU is at least present.
	 */
	if (uk_cpumask_empty(mask))
		uk_cpumask_set_cpu(0, mask);
}

void uk_sched_thread_getaffinity(const struct uk_thread *thread,
				 struct uk_cpumask *mask)
{
	UK_ASSERT(thread);
	UK_ASSERT(mask);

	if (thread->affinity_set)
		uk_cpumask_copy(mask, &thread->affinity);
	else
		uk_cpumask_online(mask);
}

struct uk_sched *uk_sched_get_for_cpu(int cpu)
{
	struct uk_sched *s;

	for (s = uk_sched_head; s; s = s->next) {
		if (!s->is_started || s->cpu_id < 0)
			continue;
		if (s->cpu_id == cpu)
			return s;
	}

	return NULL;
}

/* First started scheduler that runs on an online CPU allowed by `mask`.
 * Callers hold the migration lock.
 */
static struct uk_sched *sched_pick_target(const struct uk_cpumask *mask)
{
	struct uk_sched *s;

	for (s = uk_sched_head; s; s = s->next) {
		if (!s->is_started || s->cpu_id < 0)
			continue;
		if (!uk_cpumask_test_cpu((unsigned int)s->cpu_id, mask))
			continue;
		if (!cpu_is_online((unsigned int)s->cpu_id))
			continue;
		return s;
	}

	return NULL;
}

/* Make sure the destination CPU looks at its run queue again.
 *
 * NOTE: A cross-CPU `uk_lcpu_run()` nudge would deliver the kick
 * immediately, but on the current KVM x86 interrupt path a RUN IRQ can
 * be observed twice for one send (the halted CPU also replays the
 * latched vector through `uk_lcpu_irqs_handle_pending()`), which trips
 * the empty-slot assertion in lib/uklcpu. Until the RUN IRQ delivery is
 * de-duplicated platform-side, we do not send IPIs from the affinity
 * code. A halted destination CPU still picks the migrated thread at the
 * next timer tick, which bounds placement latency.
 */
void uk_sched_kick_cpu(int cpu __unused) {}

/* Move `thread` from its current scheduler to `dst`. The thread must not
 * be running anywhere: it is either blocked or queued, and `dequeued`
 * says whether the caller already took it out of the source queues.
 *
 * Callers hold the migration lock and run with interrupts off.
 */
static int move_thread(struct uk_thread *thread, struct uk_sched *dst,
		       bool dequeued)
{
	struct uk_sched *src = thread->sched;
	bool was_blocked_timeout;
	int rc;

	UK_ASSERT(src);
	UK_ASSERT(dst);
	UK_ASSERT(src != dst);

	/* Blocked with a live deadline: the remove drops the source
	 * sleep-queue link; re-arm the deadline on the destination so a
	 * timeout wake still finds the thread.
	 */
	was_blocked_timeout =
	    !uk_thread_is_runnable(thread) && thread->wakeup_time > 0;

	if (dequeued) {
		UK_TAILQ_REMOVE(&src->thread_list, thread, thread_list);
		thread->sched = NULL;
	} else {
		rc = uk_sched_thread_remove(thread);
		if (unlikely(rc))
			return rc;
	}

	if (dst->cpu_id != uk_sched_current_cpu() && dst->thread_inbox) {
		/* Remote scheduler: hand over through its lock-free
		 * inbox. The destination CPU enqueues the thread on its
		 * own queues when it drains the inbox in its schedule
		 * path; until then the thread belongs to no queue.
		 */
		thread->sched = dst;
		UK_TAILQ_INSERT_TAIL(&dst->thread_list, thread, thread_list);
		dst->thread_inbox(dst, thread);
		rc = 0;
	} else {
		rc = uk_sched_thread_add(dst, thread);
		if (unlikely(rc)) {
			/* Put the thread back where it came from. */
			uk_sched_thread_add(src, thread);
			return rc;
		}

		if (was_blocked_timeout)
			dst->thread_blocked(dst, thread);
	}

	uk_pr_debug("thread %p (%s): moved from sched cpu%d to cpu%d\n", thread,
		    thread->name ? thread->name : "<unnamed>", src->cpu_id,
		    dst->cpu_id);

	return 0;
}

/* Move a not-running `thread` to the scheduler of an online CPU allowed
 * by its mask. Callers hold the migration lock.
 *
 * @return
 *   - (0):  Moved to a different scheduler
 *   - (1):  Nothing to do (allowed where it is, or no target exists yet)
 *   - (<0): Move failed, the thread stayed where it was
 */
static int rehome_locked(struct uk_thread *thread, bool dequeued)
{
	struct uk_sched *dst;
	int rc;

	if (!thread->affinity_set || !thread->sched)
		return 1;

	if (thread->sched->cpu_id >= 0 &&
	    uk_thread_affinity_allows(thread,
				      (unsigned int)thread->sched->cpu_id))
		return 1;

	dst = sched_pick_target(&thread->affinity);
	if (!dst || dst == thread->sched)
		return 1; /* nowhere to go: keep as is */

	rc = move_thread(thread, dst, dequeued);
	if (rc == 0 && dst->cpu_id != uk_sched_current_cpu())
		uk_sched_kick_cpu(dst->cpu_id);

	return rc;
}

int uk_sched_thread_rehome_dequeued(struct uk_thread *thread)
{
	unsigned long lock_flags;
	int rc;

	UK_ASSERT(thread);

	if (!thread->affinity_set || !thread->sched)
		return 1;

	lock_flags = affinity_lock_acquire();
	rc = rehome_locked(thread, true);
	affinity_lock_release(lock_flags);

	return rc;
}

int uk_sched_thread_rehome(struct uk_thread *thread)
{
	unsigned long lock_flags;
	int rc;

	UK_ASSERT(thread);

	if (!thread->affinity_set || !thread->sched)
		return 1;

	lock_flags = affinity_lock_acquire();
	rc = rehome_locked(thread, false);
	affinity_lock_release(lock_flags);

	return rc;
}

int uk_sched_thread_migrate_handoff(struct uk_thread *thread)
{
	struct uk_sched *src;
	unsigned long lock_flags;
	int rc = 1;

	UK_ASSERT(thread);

	if (!thread->affinity_set || !thread->sched)
		return 1;

	/* Already parked for a move: the pending hand-off applies the
	 * current mask when it completes, do not park it a second time.
	 */
	if (thread->migrate_pending)
		return 0;

	if (thread->sched->cpu_id >= 0 &&
	    uk_thread_affinity_allows(thread,
				      (unsigned int)thread->sched->cpu_id))
		return 1;

	lock_flags = affinity_lock_acquire();

	if (!sched_pick_target(&thread->affinity))
		goto out; /* nowhere to go: keep as is */

	src = thread->sched;

	/* Park on the source sleep queue with an expired deadline. The
	 * thread keeps `sched == src` while parked: the source CPU's next
	 * schedule() run wakes it through the regular path, which only
	 * touches source-CPU queues, and the pick path then completes the
	 * move with `uk_sched_thread_rehome_dequeued()`, when the thread
	 * is safely off the CPU and off every queue.
	 */
	uk_thread_set_blocked(thread);
	uk_thread_set_queueable(thread);
	thread->wakeup_time = ukplat_monotonic_clock();
	src->thread_blocked(src, thread);
	thread->migrate_pending = 1;

	uk_pr_debug("thread %p (%s): hand-off from cpu%d\n", thread,
		    thread->name ? thread->name : "<unnamed>", src->cpu_id);

	rc = 0;
out:
	affinity_lock_release(lock_flags);

	return rc;
}

int uk_sched_thread_setaffinity(struct uk_thread *thread,
				const struct uk_cpumask *mask)
{
	struct uk_cpumask online, allowed;
	unsigned long lock_flags;
	struct uk_thread *cur = uk_thread_current();
	int rc;

	if (!thread || !mask)
		return -EINVAL;
	if (uk_cpumask_empty(mask))
		return -EINVAL;
	if (uk_thread_is_exited(thread))
		return -ESRCH;

	/* The mask may only name online CPUs: per-core service stacks
	 * rely on the pinned CPU really existing.
	 */
	uk_cpumask_online(&online);
	uk_cpumask_and(&allowed, mask, &online);
	if (!uk_cpumask_equal(&allowed, mask))
		return -EINVAL;

	if (!thread->sched)
		return -ENOENT;

	lock_flags = affinity_lock_acquire();

	uk_cpumask_copy(&thread->affinity, mask);
	thread->affinity_set = 1;

	if (thread == cur) {
		/* The calling thread keeps running where it is; the
		 * scheduler pick path performs the hand-off at its next
		 * schedule point if this CPU is no longer allowed. This
		 * is also what Linux does for a running thread on a
		 * cooperative scheduler without forced preemption.
		 */
		affinity_lock_release(lock_flags);
		return 0;
	}

	/* Never touch a foreign CPU's queues from here: that CPU is
	 * actively running and mutating pick operations, and the
	 * cooperative scheduler has no per-queue locks. Threads that
	 * belong to another CPU defer the move to that CPU's own
	 * schedule point, which applies the mask we just recorded.
	 */
	if (thread->flags & UK_THREADF_EXECUTING) {
		/* Actively running on a foreign CPU: the owner CPU runs
		 * the hand-off at the thread's next pick.
		 */
		affinity_lock_release(lock_flags);
		uk_pr_debug("thread %p (%s): migrates at next schedule point\n",
			    thread, thread->name ? thread->name : "<unnamed>");
		return 0;
	}

	if (thread->sched->cpu_id >= 0 &&
	    thread->sched->cpu_id != uk_sched_current_cpu()) {
		if (uk_thread_is_runnable(thread)) {
			/* Queued on the foreign run queue: its pick
			 * path skips forbidden threads and re-homes
			 * them. Nothing more to do here.
			 */
			affinity_lock_release(lock_flags);
			return 0;
		}
		if (thread->wakeup_time > 0) {
			/* Parked on the foreign sleep queue with a
			 * deadline: flag it so that the owner CPU
			 * completes the move in its wake-up scan
			 * without waiting for the deadline.
			 */
			thread->migrate_pending = 1;
			affinity_lock_release(lock_flags);
			return 0;
		}
		/* Blocked without a deadline: the thread is on no
		 * queue, so only bookkeeping moves. Safe from here.
		 */
	}

	/* Owned by this CPU (or blocked without deadline): the move is
	 * safe right now.
	 */
	rc = rehome_locked(thread, false);
	affinity_lock_release(lock_flags);

	if (rc == 1)
		rc = 0; /* nothing to do is success for the caller */

	return rc;
}
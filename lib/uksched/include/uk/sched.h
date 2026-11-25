/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Costin Lupu <costin.lupu@cs.pub.ro>
 *          Simon Kuenzer <simon@unikraft.io>
 *
 * Copyright (c) 2017, NEC Europe Ltd., NEC Corporation. All rights reserved.
 * Copyright (c) 2022, NEC Laboratories GmbH, NEC Corrporation.
 *                     All rights reserved.
 * Copyright (c) 2022, Unikraft GmbH. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __UK_SCHED_H__
#define __UK_SCHED_H__

#include <uk/alloc.h>
#include <uk/thread.h>
#include <uk/assert.h>
#include <uk/arch/types.h>
#include <uk/essentials.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uk_sched;

static inline struct uk_sched *uk_sched_current(void)
{
	struct uk_thread *th = uk_thread_current();

	if (th)
		return th->sched;
	return NULL;
}

typedef void (*uk_sched_yield_func_t)(struct uk_sched *s);

typedef int (*uk_sched_thread_add_func_t)(struct uk_sched *s,
					  struct uk_thread *t);
typedef void (*uk_sched_thread_remove_func_t)(struct uk_sched *s,
					      struct uk_thread *t);
typedef void (*uk_sched_thread_blocked_func_t)(struct uk_sched *s,
					       struct uk_thread *t);
typedef void (*uk_sched_thread_woken_func_t)(struct uk_sched *s,
					     struct uk_thread *t);

typedef const struct uk_thread *(*uk_sched_idle_thread_func_t)(
    struct uk_sched *s, unsigned int proc_id);

typedef int (*uk_sched_start_t)(struct uk_sched *s, struct uk_thread *main);

#if CONFIG_LIBUKSCHED_STATS
struct uk_sched_stats {
	unsigned long sched_count;
	unsigned long yield_count;
	unsigned long next_count;
	unsigned long idle_count;
};
#endif /* CONFIG_LIBUKSCHED_STATS */

struct uk_sched {
	uk_sched_yield_func_t yield;

	uk_sched_thread_add_func_t thread_add;
	uk_sched_thread_remove_func_t thread_remove;
	uk_sched_thread_blocked_func_t thread_blocked;
	uk_sched_thread_woken_func_t thread_woken;
	uk_sched_thread_woken_func_t thread_woken_isr;
	/*
	 * Optional lock-free inbox for CPU migration (see
	 * `uk_sched_thread_setaffinity()`). Schedulers with one instance
	 * per CPU set this so that other CPUs can hand threads over
	 * without touching the owner CPU's queues: the owner CPU drains
	 * the inbox in its own schedule path. Schedulers without it only
	 * support affinity on the local CPU.
	 */
	int (*thread_inbox)(struct uk_sched *s, struct uk_thread *t);
	uk_sched_idle_thread_func_t idle_thread;

	uk_sched_start_t sched_start;

	/* internal */
	bool is_started;
	/** Linear CPU index of the logical CPU that runs this scheduler
	 *  instance (`uk_sched_start()` records it), or -1 before start.
	 *  The affinity code uses it to map CPU affinity masks onto the
	 *  scheduler that owns each CPU.
	 */
	int cpu_id;
	struct uk_thread_list thread_list;
	struct uk_thread_list exited_threads;
	struct uk_alloc *a;	  /**< default allocator for struct uk_thread */
	struct uk_alloc *a_stack; /**< default allocator for stacks */
	struct uk_alloc *a_auxstack; /**< default allocator for aux stacks */
	struct uk_alloc *a_uktls;    /**< default allocator for TLS+ectx */
	struct uk_sched *next;
#if CONFIG_LIBUKSCHED_STATS
	__nsec start_time;
	struct uk_sched_stats stats;
#endif /* CONFIG_LIBUKSCHED_STATS */
};

/* wrapper functions over scheduler callbacks */
static inline void uk_sched_yield(void)
{
	struct uk_sched *s;
	struct uk_thread *current = uk_thread_current();

	UK_ASSERT(current);

	s = current->sched;
	UK_ASSERT(s);
	s->yield(s);
}

int uk_sched_thread_add(struct uk_sched *s, struct uk_thread *t);

int uk_sched_thread_remove(struct uk_thread *t);

static inline void uk_sched_thread_blocked(struct uk_thread *t)
{
	struct uk_sched *s;

	UK_ASSERT(t);
	UK_ASSERT(t->sched);
	UK_ASSERT(!uk_thread_is_runnable(t));

	s = t->sched;
	s->thread_blocked(s, t);
}

static inline void uk_sched_thread_woken(struct uk_thread *t)
{

	struct uk_sched *s;

	UK_ASSERT(t);
	UK_ASSERT(t->sched);
	UK_ASSERT(uk_thread_is_runnable(t));

	s = t->sched;
	s->thread_woken(s, t);
}

/**
 * Returns the reference to the idle thread that is responsible for
 * the processing unit `proc_id`. Please note that `proc_id` is not
 * a logical core ID; it is a linear increasing number over the managed
 * logical cores by the scheduler instance `s`.
 * It is possible that a scheduler implementation does not operate with idle
 * threads and returns `NULL`.
 */
static inline const struct uk_thread *uk_sched_idle_thread(struct uk_sched *s,
							   unsigned int proc_id)
{
	UK_ASSERT(s);

	if (unlikely(!s->idle_thread))
		return NULL;

	return s->idle_thread(s, proc_id);
}

/**
 * Create a main thread from current context and call thread starter function
 */
int uk_sched_start(struct uk_sched *sched);

/**
 * Linear index of the logical CPU the caller runs on, or -1 when the
 * platform does not expose a CPU index.
 */
int uk_sched_current_cpu(void);

/**
 * Find the started scheduler instance that owns the logical CPU `cpu`.
 *
 * @return
 *   - (NULL): No started scheduler runs on that CPU
 *   - (>=): Reference to the scheduler
 */
struct uk_sched *uk_sched_get_for_cpu(int cpu);

/**
 * Return the set of currently online CPUs in `mask`.
 */
void uk_cpumask_online(struct uk_cpumask *mask);

/**
 * Return the affinity mask of `thread`.
 *
 * Threads without an explicitly assigned mask are unrestricted and report
 * the currently online CPUs.
 */
void uk_sched_thread_getaffinity(const struct uk_thread *thread,
				 struct uk_cpumask *mask);

/**
 * Assign the CPU affinity mask `mask` to `thread` and apply it.
 *
 * The mask must be non-empty and may only name online CPUs. The change
 * takes effect immediately:
 *   - A thread that is blocked or runnable but not running moves to the
 *     scheduler of an allowed CPU at once.
 *   - The calling thread moves and yields if its CPU is no longer allowed.
 *   - A thread that is actively running on another CPU moves there at its
 *     next scheduling point; the cooperative scheduler never preempts a
 *     running thread.
 *
 * @return
 *   - (0):  Success
 *   - (-EINVAL): mask empty or names an offline/out-of-range CPU
 *   - (-ENOENT): thread has no scheduler
 */
int uk_sched_thread_setaffinity(struct uk_thread *thread,
				const struct uk_cpumask *mask);

/**
 * Move `thread` to the scheduler of a CPU allowed by its affinity mask.
 *
 * Callers: scheduler pick paths, when they notice a thread that is not
 * allowed to run on the current CPU. Threads without an explicit mask are
 * left alone.
 *
 * @return
 *   - (0):  Thread moved to a different scheduler
 *   - (1):  Thread stays where it is (allowed, or no allowed CPU hosts a
 *           started scheduler yet)
 *   - (<0): Error
 */
int uk_sched_thread_rehome(struct uk_thread *thread);

/**
 * Variant of `uk_sched_thread_rehome()` for scheduler pick paths that
 * have just removed `thread` from their run queue: the thread owns no
 * queue entry and is not executing, so only the scheduler bookkeeping
 * and the destination `thread_add()` are performed.
 */
int uk_sched_thread_rehome_dequeued(struct uk_thread *thread);

/**
 * Start the CPU migration of `thread`, which is running on the calling
 * CPU (the `prev` thread of a scheduler pick). The cooperative scheduler
 * must not move a running thread between run queues, because the
 * destination CPU could pick it up while its registers are still live.
 *
 * The hand-off instead re-homes the scheduler bookkeeping, parks the
 * thread on the source scheduler's sleep queue with an expired deadline
 * and blocks it. The next schedule run on the source CPU wakes it, which
 * enqueues it on the destination run queue through the regular wake
 * path and kicks a halted destination CPU.
 *
 * @return
 *   - (0):  Hand-off started; the caller must not run `thread` again
 *   - (1):  Nothing to do (allowed here, or no target scheduler yet)
 *   - (<0): Failed; the thread stayed runnable where it was
 */
int uk_sched_thread_migrate_handoff(struct uk_thread *thread);

/**
 * Nudge the scheduler of logical CPU `cpu` to look at its run queue
 * again (best effort, used after cross-CPU wake-ups).
 */
void uk_sched_kick_cpu(int cpu);

/**
 * Allocates a uk_thread and assigns it to a scheduler.
 * Similar to `uk_thread_create_fn0()`, general-purpose registers are reset
 * on thread start.
 *
 * @param s
 *   Reference to scheduler that will execute the thread after creation
 *   (required)
 * @param fn0
 *   Thread entry function (required)
 * @param stack_len
 *   Size of the thread stack. If set to 0, a default stack size is used
 *   for the stack allocation.
 * @param auxstack_len
 *   Size of the thread auxiliary stack. If set to 0, a default stack size is
 *   used for the stack allocation.
 * @param no_uktls
 *   If set, no memory is allocated for a TLS. Functions must not use
 *   any TLS variables.
 * @param no_ectx
 *   If set, no memory is allocated for saving/restoring extended CPU
 *   context state (e.g., floating point, vector registers). In such a
 *   case, no extended context is saved nor restored on thread switches.
 *   Executed functions must be ISR-safe.
 * @param name
 *   Optional name for the thread
 * @param priv
 *   Reference to external data that corresponds to this thread
 * @param dtor
 *   Destructor that is called when this thread is released
 * @return
 *   - (NULL): Allocation failed
 *   - Reference to created uk_thread
 */
struct uk_thread *uk_sched_thread_create_fn0(struct uk_sched *s,
					     uk_thread_fn0_t fn0,
					     size_t stack_len,
					     size_t auxstack_len, bool no_uktls,
					     bool no_ectx, const char *name,
					     void *priv, uk_thread_dtor_t dtor);

/**
 * Similar to `uk_sched_thread_create_fn0()` but with a thread function
 * accepting one argument
 */
struct uk_thread *uk_sched_thread_create_fn1(struct uk_sched *s,
					     uk_thread_fn1_t fn1, void *argp,
					     size_t stack_len,
					     size_t auxstack_len, bool no_uktls,
					     bool no_ectx, const char *name,
					     void *priv, uk_thread_dtor_t dtor);

/**
 * Similar to `uk_sched_thread_create_fn0()` but with a thread function
 * accepting two arguments
 */
struct uk_thread *uk_sched_thread_create_fn2(struct uk_sched *s,
					     uk_thread_fn2_t fn2, void *argp0,
					     void *argp1, size_t stack_len,
					     size_t auxstack_len, bool no_uktls,
					     bool no_ectx, const char *name,
					     void *priv, uk_thread_dtor_t dtor);

/* Shortcut for creating a thread with default settings */
#define uk_sched_thread_create(s, fn1, argp, name)                             \
	uk_sched_thread_create_fn1((s), (fn1), (void *)(argp), 0x0, 0x0,       \
				   false, false, (name), NULL, NULL)

#define uk_sched_foreach_thread(sched, itr)                                    \
	UK_TAILQ_FOREACH((itr), &(sched)->thread_list, thread_list)
#define uk_sched_foreach_thread_safe(sched, itr, tmp)                          \
	UK_TAILQ_FOREACH_SAFE((itr), &(sched)->thread_list, thread_list, (tmp))

void uk_sched_dumpk_threads(int klvl, struct uk_sched *s);

void uk_sched_thread_sleep(__nsec nsec);

/* Exits the current thread context */
void uk_sched_thread_exit(void) __noreturn;

/* Exits the current thread and runs the given routine when releasing the
 * thread from garbage collection context.
 */
void uk_sched_thread_exit2(uk_thread_gc_t gc_fn, void *gc_argp) __noreturn;

/* Terminates another thread */
void uk_sched_thread_terminate(struct uk_thread *thread);

#ifdef __cplusplus
}
#endif

#endif /* __UK_SCHED_H__ */

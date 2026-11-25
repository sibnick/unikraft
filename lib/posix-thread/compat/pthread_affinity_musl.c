/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2026, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Application-side pthread affinity shim for musl static binaries.
 *
 * musl implements none of the pthread_*_np affinity calls, so an
 * application that needs them (for example a JVM that pins its carrier
 * threads to the CPU owning each socket) compiles this file into the
 * application image. It provides:
 *
 *   pthread_setaffinity_np()   - musl-compat return codes (0 / errno)
 *   pthread_getaffinity_np()
 *   pthread_attr_setaffinity_np()
 *   pthread_attr_getaffinity_np()
 *
 * and, when the application links with `-Wl,--wrap=pthread_create`,
 * applies the attribute affinity by wrapping thread creation through
 * __wrap_pthread_create().
 *
 * How the pthread_t -> tid mapping works: musl's pthread_t is an opaque
 * handle the kernel cannot be asked about directly (its sched_setaffinity
 * syscall takes a tid). So the wrapper records, from inside the new
 * thread, the tid that the kernel's clone() handler assigned, keyed by
 * the pthread_t the parent received. The first thing the wrapped start
 * routine does is publish its tid and apply the attribute affinity; the
 * parent waits (bounded, yielding) until the tid is published before it
 * ever resolves a handle, so the mapping is race-free.
 *
 * The affinity itself travels over the normal Linux syscall interface:
 * sched_setaffinity(2)/sched_getaffinity(2), implemented in-kernel by
 * lib/posix-process with full tid resolution.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>

#ifndef SYS_gettid
#define SYS_gettid 186 /* x86_64 */
#endif

struct ptid_entry {
	volatile unsigned long pt; /* pthread_t as an integer, 0 = free */
	volatile pid_t tid;	   /* valid once tid_ready */
	volatile int tid_ready;
	volatile int alive;
};

#define MAX_TRACKED_THREADS 512
static struct ptid_entry ptmap[MAX_TRACKED_THREADS];

struct start_wrap {
	void *(*start)(void *);
	void *arg;
	int slot;
	int have_aff;
	cpu_set_t aff;
};

static void ptmap_set_tid(int slot, pid_t tid)
{
	ptmap[slot].tid = tid;
	__atomic_store_n(&ptmap[slot].tid_ready, 1, __ATOMIC_RELEASE);
}

/* Resolve `pt` to its kernel tid, waiting briefly for a just-created
 * thread to publish its tid. Cooperative scheduling makes the wait
 * short: the new thread runs at the next scheduling point.
 *
 * @return tid (>0) or negative errno
 */
static pid_t ptmap_resolve(pthread_t pt, unsigned long pti)
{
	int i, spins;

	for (;;) {
		for (i = 0; i < MAX_TRACKED_THREADS; i++) {
			if (ptmap[i].pt != pti)
				continue;
			if (ptmap[i].tid_ready)
				return ptmap[i].tid;
			break;
		}
		if (++spins > 1000000)
			return -ESRCH;
		sched_yield();
	}
	(void)pt;
}

/* The real entry of every tracked thread. */
static void *aff_start(void *a)
{
	struct start_wrap *w = a;
	void *ret;

	ptmap_set_tid(w->slot, (pid_t)syscall(SYS_gettid));

	if (w->have_aff)
		/* Best effort: an invalid cpuset must not kill the
		 * thread; Linux applications observe the failure
		 * through pthread_attr results, not here.
		 */
		sched_setaffinity(0, sizeof(cpu_set_t), &w->aff);

	ret = w->start(w->arg);

	__atomic_store_n(&ptmap[w->slot].pt, 0, __ATOMIC_RELEASE);
	free(w);
	return ret;
}

/* ---- pthread_[gs]etaffinity_np ---- */

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
			   const cpu_set_t *cpuset)
{
	unsigned long pti = (unsigned long)(uintptr_t)thread;
	pid_t tid;
	int rc;

	if (!cpuset || cpusetsize == 0)
		return EINVAL;

	tid = ptmap_resolve(thread, pti);
	if (tid < 0)
		return (int)-tid;

	rc = sched_setaffinity(tid, cpusetsize, cpuset);
	return rc ? errno : 0;
}

int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize,
			   cpu_set_t *cpuset)
{
	unsigned long pti = (unsigned long)(uintptr_t)thread;
	pid_t tid;
	int rc;

	if (!cpuset || cpusetsize == 0)
		return EINVAL;

	tid = ptmap_resolve(thread, pti);
	if (tid < 0)
		return (int)-tid;

	rc = sched_getaffinity(tid, cpusetsize, cpuset);
	return rc ? errno : 0;
}

/* ---- pthread_attr_*affinity_np ----
 *
 * musl's pthread_attr_t has no cpuset field, so masks are kept in a
 * side registry keyed by the attribute object address. Entries are
 * consumed by __wrap_pthread_create(); attributes that are destroyed
 * without being used simply keep a stale entry until the address is
 * reused, which matches fresh-object semantics.
 */

struct attr_entry {
	void *attr;
	int used;
	cpu_set_t mask;
};

#define ATTR_SLOTS 32
static struct attr_entry attrmap[ATTR_SLOTS];
static pthread_mutex_t attr_lock = PTHREAD_MUTEX_INITIALIZER;

static struct attr_entry *attr_slot(void *attr, int alloc)
{
	int i;
	struct attr_entry *freeme = NULL;

	for (i = 0; i < ATTR_SLOTS; i++) {
		if (attrmap[i].used && attrmap[i].attr == attr)
			return &attrmap[i];
		if (alloc && !attrmap[i].used && !freeme)
			freeme = &attrmap[i];
	}
	if (alloc && freeme) {
		freeme->used = 1;
		freeme->attr = attr;
		return freeme;
	}
	return NULL;
}

int pthread_attr_setaffinity_np(pthread_attr_t *attr, size_t cpusetsize,
				const cpu_set_t *cpuset)
{
	struct attr_entry *e;
	int rc = 0;

	if (!attr || !cpuset || cpusetsize != sizeof(cpu_set_t))
		return EINVAL;

	pthread_mutex_lock(&attr_lock);
	e = attr_slot(attr, 1);
	if (e)
		memcpy(&e->mask, cpuset, sizeof(*cpuset));
	else
		rc = ENOMEM;
	pthread_mutex_unlock(&attr_lock);
	return rc;
}

int pthread_attr_getaffinity_np(const pthread_attr_t *attr, size_t cpusetsize,
				cpu_set_t *cpuset)
{
	struct attr_entry *e;
	int rc = ESRCH;

	if (!attr || !cpuset || cpusetsize != sizeof(cpu_set_t))
		return EINVAL;

	pthread_mutex_lock(&attr_lock);
	e = attr_slot((void *)(uintptr_t)attr, 0);
	if (e) {
		memcpy(cpuset, &e->mask, sizeof(*cpuset));
		rc = 0;
	}
	pthread_mutex_unlock(&attr_lock);
	return rc;
}

static void attr_take(const pthread_attr_t *attr, cpu_set_t *out, int *have)
{
	struct attr_entry *e;

	*have = 0;
	if (!attr)
		return;

	pthread_mutex_lock(&attr_lock);
	e = attr_slot((void *)(uintptr_t)attr, 0);
	if (e) {
		*out = e->mask;
		e->used = 0;
		e->attr = NULL;
		*have = 1;
	}
	pthread_mutex_unlock(&attr_lock);
}

/* ---- pthread_create wrapper ----
 *
 * Active when the application links with -Wl,--wrap=pthread_create:
 * all calls to pthread_create land here, __real_pthread_create is
 * musl's implementation.
 */

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*start_routine)(void *), void *arg);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*start_routine)(void *), void *arg)
{
	struct start_wrap *w;
	cpu_set_t aff;
	int have_aff, slot, rc;

	w = calloc(1, sizeof(*w));
	if (!w)
		return EAGAIN;

	w->start = start_routine;
	w->arg = arg;
	attr_take(attr, &aff, &have_aff);
	w->have_aff = have_aff;
	if (have_aff)
		w->aff = aff;

	/* Claim a mapping slot before creation so the pthread_t can be
	 * recorded the instant musl hands it out.
	 */
	slot = 0;
	while (slot < MAX_TRACKED_THREADS && ptmap[slot].pt != 0)
		slot++;
	if (slot >= MAX_TRACKED_THREADS) {
		free(w);
		return EAGAIN;
	}
	w->slot = slot;
	/* Reserve: pthread_t value is written after creation, the
	 * sentinel below marks the slot taken.
	 */
	ptmap[slot].pt = (unsigned long)-1;
	ptmap[slot].alive = 1;
	ptmap[slot].tid_ready = 0;

	rc = __real_pthread_create(thread, attr, aff_start, w);
	if (rc) {
		ptmap[slot].pt = 0;
		free(w);
		return rc;
	}

	ptmap[slot].pt = (unsigned long)(uintptr_t)*thread;
	return 0;
}


/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2026, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * musl-compatible pthread affinity calls.
 *
 * musl has no pthread_*affinity* functions at all, so this library
 * provides them on top of lib/uksched's per-thread CPU affinity:
 *
 *   - pthread_t resolution: musl uses the address of the thread control
 *     block as pthread_t. On x86_64 musl keeps a self-pointer inside
 *     the TLS block at a fixed offset below the thread pointer, and the
 *     thread pointer itself is recorded in uk_thread.tlsp by
 *     lib/posix-process when the clone() syscall creates the thread.
 *     Scanning the scheduler thread lists for the matching thread
 *     pointer, with the self-pointer back-reference verified, maps the
 *     handle back to the uk_thread.
 *
 *   - pthread_attr_t affinity: musl's attribute object has no room for
 *     a cpuset, so masks are stored in a small side registry keyed by
 *     the attribute address and consumed by the application-side
 *     pthread_create wrapper (compat/pthread_affinity_musl.c).
 */

#include <errno.h>
#include <string.h>

#include <uk/arch/types.h>
#include <uk/assert.h>
#include <uk/cpumask.h>
#include <uk/essentials.h>
#include <uk/sched.h>
#include <uk/sched_impl.h>
#include <uk/arch/spinlock.h>
#include <uk/thread.h>

#include <uk/pthread_ext.h>

/* musl x86_64 (TLS_ABOVE_TP): struct pthread starts with the `self`
 * member, and on x86_64 musl places the thread pointer 16 bytes above
 * it, so `*(void **)(tp - 16) == tp - 16` holds for a live musl thread.
 * This is the same layout assumption lib/posix-process relies on for
 * clone()-created threads.
 */
#if !CONFIG_ARCH_X86_64
#define UK_PTHREAD_RESOLVE_SUPPORTED 0
#else
#define UK_PTHREAD_RESOLVE_SUPPORTED 1
#define MUSL_SELF_OFF(tp) ((char *)(tp) - 16)
#endif /* CONFIG_ARCH_X86_64 */

struct uk_thread *uk_pthread_resolve(void *pt)
{
#if UK_PTHREAD_RESOLVE_SUPPORTED
	struct uk_sched *s;
	struct uk_thread *t;

	if (!pt)
		return NULL;

	for (s = uk_sched_head; s; s = s->next) {
		UK_TAILQ_FOREACH(t, &s->thread_list, thread_list)
		{
			char *self;

			if (!t->tlsp)
				continue;
			self = MUSL_SELF_OFF(t->tlsp);
			/* Verify the self-pointer back-reference: the
			 * slot must point at itself, and pthread_t is
			 * that address.
			 */
			if (*(void **)self != self)
				continue;
			if ((char *)pt == self)
				return t;
		}
	}
#else  /* !UK_PTHREAD_RESOLVE_SUPPORTED */
	(void)pt;
#endif /* UK_PTHREAD_RESOLVE_SUPPORTED */

	return NULL;
}

int uk_pthread_setaffinity_np(void *pt, size_t cpusetsize,
			      const unsigned long *mask)
{
	struct uk_thread *t;
	struct uk_cpumask m;
	int rc;

	t = uk_pthread_resolve(pt);
	if (!t)
		return -ESRCH;

	rc = uk_cpumask_from_user(&m, mask, (long)cpusetsize);
	if (unlikely(rc))
		return rc;

	return uk_sched_thread_setaffinity(t, &m);
}

long uk_pthread_getaffinity_np(void *pt, size_t cpusetsize, unsigned long *mask)
{
	struct uk_thread *t;
	struct uk_cpumask m;

	t = uk_pthread_resolve(pt);
	if (!t)
		return -ESRCH;

	uk_sched_thread_getaffinity(t, &m);

	return uk_cpumask_to_user(mask, (long)cpusetsize, &m);
}

/* Attribute affinity side registry: pthread_attr_t objects live in the
 * application, and their lifetime is short (typically created, filled,
 * passed to pthread_create, destroyed), so a handful of slots with
 * address keys is enough. Reusing an address that still has a stale
 * entry simply overwrites it: that matches the musl behavior where the
 * attribute object would be fresh.
 */
#define ATTR_SLOTS 16

struct attr_affinity_entry {
	void *attr;
	struct uk_cpumask mask;
};

static struct attr_affinity_entry attr_affinity[ATTR_SLOTS];
static struct __spinlock attr_affinity_lock =
	UKARCH_SPINLOCK_INITIALIZER();

static struct attr_affinity_entry *attr_slot(void *attr, bool alloc)
{
	struct attr_affinity_entry *e, *free_slot = NULL;
	unsigned int i;

	for (i = 0, e = attr_affinity; i < ATTR_SLOTS; i++, e++) {
		if (e->attr == attr)
			return e;
		if (!alloc)
			continue;
		if (!e->attr && !free_slot)
			free_slot = e;
	}

	if (alloc && free_slot) {
		free_slot->attr = attr;
		return free_slot;
	}

	return NULL;
}

int uk_pthread_attr_setaffinity_np(void *attr, size_t cpusetsize,
				   const unsigned long *mask)
{
	struct uk_cpumask m;
	struct attr_affinity_entry *e;
	int rc;

	if (!attr || !mask)
		return -EINVAL;

	rc = uk_cpumask_from_user(&m, mask, (long)cpusetsize);
	if (unlikely(rc))
		return rc;

	ukarch_spin_lock(&attr_affinity_lock);
	e = attr_slot(attr, true);
	if (e)
		e->mask = m;
	ukarch_spin_unlock(&attr_affinity_lock);

	return e ? 0 : -ENOSPC;
}

int uk_pthread_attr_getaffinity_np(void *attr, size_t cpusetsize,
				   unsigned long *mask)
{
	struct attr_affinity_entry *e;
	struct uk_cpumask m;
	long rc;

	if (!attr || !mask)
		return -EINVAL;

	ukarch_spin_lock(&attr_affinity_lock);
	e = attr_slot(attr, false);
	if (e)
		m = e->mask;
	ukarch_spin_unlock(&attr_affinity_lock);

	if (!e)
		return -ESRCH;

	rc = uk_cpumask_to_user(mask, (long)cpusetsize, &m);
	return (rc < 0) ? rc : 0;
}

int uk_pthread_attr_take_affinity(void *attr, struct uk_cpumask *mask)
{
	struct attr_affinity_entry *e;

	if (!attr || !mask)
		return -EINVAL;

	ukarch_spin_lock(&attr_affinity_lock);
	e = attr_slot(attr, false);
	if (e) {
		*mask = e->mask;
		e->attr = NULL;
	}
	ukarch_spin_unlock(&attr_affinity_lock);

	return e ? 0 : -ESRCH;
}
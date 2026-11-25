/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2026, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * musl-compatible pthread affinity extensions.
 *
 * The `pthread_t` and `pthread_attr_t` parameters are passed as opaque
 * pointers: musl uses the thread control block address as pthread_t and
 * keeps pthread_attr_t as a small opaque buffer.
 */
#ifndef _UK_PTHREAD_EXT_H_
#define _UK_PTHREAD_EXT_H_

#include <stddef.h>

#include <uk/cpumask.h>
#include <uk/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Resolve a musl pthread_t to its uk_thread.
 *
 * musl x86_64 stores the thread's self-pointer - the value used as
 * pthread_t - at a fixed offset from the thread pointer that was
 * installed with clone(); lib/posix-process records that pointer in
 * uk_thread.tlsp. The resolution verifies the self-pointer
 * back-reference, so a bogus handle cannot alias another thread.
 *
 * @return
 *   - (!=NULL): The resolved thread
 *   - (NULL):   `pt` does not address a live thread of this image
 */
struct uk_thread *uk_pthread_resolve(void *pt);

/**
 * `pthread_setaffinity_np()` semantics on a musl pthread_t.
 *
 * @return  0 on success, negative errno on failure (-ESRCH for an
 *          unknown handle)
 */
int uk_pthread_setaffinity_np(void *pt, size_t cpusetsize,
			      const unsigned long *mask);

/**
 * `pthread_getaffinity_np()` semantics on a musl pthread_t.
 *
 * @return  number of bytes written on success, negative errno otherwise
 */
long uk_pthread_getaffinity_np(void *pt, size_t cpusetsize,
			       unsigned long *mask);

/**
 * `pthread_attr_setaffinity_np()` storage.
 *
 * musl's pthread_attr_t carries no affinity field, so the mask is kept
 * in a small side registry keyed by the attribute object address. The
 * entry survives until it is taken by `uk_pthread_attr_take_affinity()`
 * or replaced; application code creating threads must consume it (see
 * compat/pthread_affinity_musl.c).
 *
 * @return  0 on success, negative errno on failure
 */
int uk_pthread_attr_setaffinity_np(void *attr, size_t cpusetsize,
				   const unsigned long *mask);

/**
 * Read the stored attribute affinity without consuming it.
 *
 * @return  0 on success (mask filled), -ESRCH when the attribute object
 *          has no affinity set, negative errno on invalid input
 */
int uk_pthread_attr_getaffinity_np(void *attr, size_t cpusetsize,
				   unsigned long *mask);

/**
 * Consume the stored attribute affinity for `attr` (used by the
 * pthread_create wrapper right after the new thread exists).
 *
 * @return
 *   - (0):   Mask returned, registry entry cleared
 *   - (-ESRCH): No affinity was set on this attribute object
 */
int uk_pthread_attr_take_affinity(void *attr, struct uk_cpumask *mask);

#ifdef __cplusplus
}
#endif

#endif /* _UK_PTHREAD_EXT_H_ */
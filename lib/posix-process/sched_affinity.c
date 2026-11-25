/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2026, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/* Linux-semantics sched_setaffinity(2) / sched_getaffinity(2).
 *
 * These live in lib/posix-process because they resolve `pid` against the
 * POSIX thread registry: `pid == 0` means the calling thread, a positive
 * value is a real thread id (tid), and a thread group id applies to all
 * threads of that process, like on Linux. The scheduling mechanics are
 * provided by lib/uksched (uk_sched_thread_setaffinity()).
 */

#include <errno.h>
#include <uk/cpumask.h>
#include <uk/sched.h>
#include <uk/syscall.h>
#include <uk/thread.h>

#include "process.h"

/* Apply `m` to the thread(s) addressed by `pid`.
 *
 * @return
 *   - (0):  Applied to at least one thread (Linux reports the first
 *           failing errno if some applications failed)
 *   - (-ESRCH): `pid` does not address any existing thread
 */
static int setaffinity_apply(pid_t pid, const struct uk_cpumask *m)
{
	struct uk_thread *cur;
	int ret = 0;

	if (pid < 0 || (size_t)pid > CONFIG_LIBPOSIX_PROCESS_MAX_PID)
		return -ESRCH;

	if (pid == 0) {
		cur = uk_thread_current();
		if (!cur)
			return -ESRCH;
		return uk_sched_thread_setaffinity(cur, m);
	}

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	{
		struct posix_thread *pth, *iter, *tmp;
		struct posix_process *pproc;
		bool found = false;

		/* A tid matches exactly one thread */
		pth = tid2pthread(pid);
		if (pth && pth->thread)
			return uk_sched_thread_setaffinity(pth->thread, m);

		/* A tgid applies to every thread of the process */
		pproc = pid2pprocess(pid);
		if (!pproc || pproc->state != POSIX_PROCESS_RUNNING)
			return -ESRCH;

		uk_pprocess_foreach_pthread(pproc, iter, tmp)
		{
			if (iter->state == POSIX_THREAD_EXITED ||
			    iter->state == POSIX_THREAD_KILLED)
				continue;
			if (!iter->thread)
				continue;
			found = true;
			int rc = uk_sched_thread_setaffinity(iter->thread, m);

			if (unlikely(rc) && !ret)
				ret = rc;
		}

		return found ? ret : -ESRCH;
	}
#else
	/* No thread registry: only the calling thread can be addressed */
	return -ESRCH;
#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */
}

/* Resolve `pid` to a single thread for the affinity query. Linux reports
 * the mask of any thread of a thread group; all threads of one mask
 * application share it anyway.
 */
static struct uk_thread *getaffinity_resolve(pid_t pid)
{
	if (pid < 0 || (size_t)pid > CONFIG_LIBPOSIX_PROCESS_MAX_PID)
		return NULL;

	if (pid == 0)
		return uk_thread_current();

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	{
		struct posix_thread *pth;
		struct posix_process *pproc;

		pth = tid2pthread(pid);
		if (pth)
			return pth->thread;

		/* A tgid: take the group leader's thread */
		pproc = pid2pprocess(pid);
		if (!pproc || pproc->state != POSIX_PROCESS_RUNNING)
			return NULL;
		if (!uk_list_empty(&pproc->threads)) {
			struct posix_thread *first;

			first = uk_list_first_entry(&pproc->threads,
						    struct posix_thread,
						    thread_list_entry);
			return first->thread;
		}
		return NULL;
	}
#else
	return NULL;
#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */
}

UK_SYSCALL_R_DEFINE(int, sched_setaffinity, int, pid, long, cpusetsize,
		    const unsigned long *, mask)
{
	struct uk_cpumask m;
	int rc;

	if (cpusetsize <= 0)
		return -EINVAL;
	if (!mask)
		return -EFAULT;

	rc = uk_cpumask_from_user(&m, mask, cpusetsize);
	if (unlikely(rc))
		return rc;

	return setaffinity_apply(pid, &m);
}

UK_SYSCALL_R_DEFINE(int, sched_getaffinity, int, pid, long, cpusetsize,
		    unsigned long *, mask)
{
	struct uk_thread *t;
	struct uk_cpumask m;

	if (cpusetsize <= 0)
		return -EINVAL;
	if (!mask)
		return -EFAULT;

	t = getaffinity_resolve(pid);
	if (!t)
		return -ESRCH;

	uk_sched_thread_getaffinity(t, &m);

	return (int)uk_cpumask_to_user(mask, cpusetsize, &m);
}
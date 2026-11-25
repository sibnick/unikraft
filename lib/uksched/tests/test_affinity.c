/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2026, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/*
 * Unit tests for the CPU affinity primitives of lib/uksched.
 *
 * These tests are scheduler-agnostic: they exercise the cpumask helpers
 * and the affinity bookkeeping of `uk_sched_thread_get/setaffinity()`
 * from the calling thread, so they run on any scheduler and on a single
 * CPU. Cross-CPU migration paths are covered by the multicore
 * integration tests, not here.
 */

#include <errno.h>

#include <uk/test.h>
#include <uk/cpumask.h>
#include <uk/sched.h>
#include <uk/thread.h>

UK_TESTCASE(uksched_affinity, test_cpumask_basic)
{
	struct uk_cpumask a, b;

	uk_cpumask_zero(&a);
	UK_TEST_EXPECT(uk_cpumask_empty(&a) == 1);
	UK_TEST_EXPECT_SNUM_EQ(uk_cpumask_first(&a), -1);

	uk_cpumask_set_cpu(0, &a);
	uk_cpumask_set_cpu(3, &a);
	UK_TEST_EXPECT(uk_cpumask_empty(&a) == 0);
	UK_TEST_EXPECT(uk_cpumask_test_cpu(0, &a));
	UK_TEST_EXPECT(uk_cpumask_test_cpu(3, &a));
	UK_TEST_EXPECT(!uk_cpumask_test_cpu(1, &a));
	UK_TEST_EXPECT_SNUM_EQ(uk_cpumask_first(&a), 0);

	/* Out-of-range CPU ids are ignored on set, never test set */
	uk_cpumask_set_cpu(UK_CPUMASK_BITS, &a);
	UK_TEST_EXPECT(!uk_cpumask_test_cpu(UK_CPUMASK_BITS, &a));

	uk_cpumask_clear_cpu(3, &a);
	UK_TEST_EXPECT(!uk_cpumask_test_cpu(3, &a));

	uk_cpumask_copy(&b, &a);
	UK_TEST_EXPECT(uk_cpumask_equal(&a, &b));

	uk_cpumask_and(&b, &a, &b);
	UK_TEST_EXPECT(uk_cpumask_equal(&a, &b));

	uk_cpumask_zero(&b);
	UK_TEST_EXPECT(!uk_cpumask_intersects(&a, &b));
	uk_cpumask_set_cpu(0, &b);
	UK_TEST_EXPECT(uk_cpumask_intersects(&a, &b));
}

UK_TESTCASE(uksched_affinity, test_cpumask_user_roundtrip)
{
	struct uk_cpumask m;
	unsigned long buf[4];
	long rc;

	uk_cpumask_zero(&m);
	uk_cpumask_set_cpu(1, &m);

	rc = uk_cpumask_to_user(buf, sizeof(buf), &m);
	UK_TEST_EXPECT_SNUM_EQ(rc, (long)UK_CPUMASK_BYTES);

	rc = uk_cpumask_from_user(&m, buf, UK_CPUMASK_BYTES);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	UK_TEST_EXPECT(uk_cpumask_test_cpu(1, &m));

	/* Invalid sizes and pointers */
	rc = uk_cpumask_from_user(&m, buf, 0);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EINVAL);
	rc = uk_cpumask_from_user(&m, NULL, sizeof(buf));
	UK_TEST_EXPECT_SNUM_EQ(rc, -EINVAL);
	rc = uk_cpumask_to_user(NULL, sizeof(buf), &m);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EFAULT);
	rc = uk_cpumask_to_user(buf, 0, &m);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EINVAL);

	/* A mask naming a CPU id the image cannot host is rejected */
	memset(buf, 0, sizeof(buf));
	if (UK_CPUMASK_BITS < 8 * sizeof(unsigned long)) {
		buf[0] = 1UL << UK_CPUMASK_BITS;
		rc = uk_cpumask_from_user(&m, buf, UK_CPUMASK_BYTES);
		UK_TEST_EXPECT_SNUM_EQ(rc, -EINVAL);
	}
}

UK_TESTCASE(uksched_affinity, test_default_affinity_is_online)
{
	struct uk_thread *cur = uk_thread_current();
	struct uk_cpumask online, own;
	unsigned int cpu = uk_sched_current_cpu();

	uk_sched_thread_getaffinity(cur, &own);

	/* Unrestricted threads run on every online CPU, in particular
	 * on the one they run on right now.
	 */
	UK_TEST_EXPECT(!uk_cpumask_empty(&own));
	UK_TEST_EXPECT(uk_cpumask_test_cpu(cpu, &own));

	uk_cpumask_online(&online);
	UK_TEST_EXPECT(!uk_cpumask_empty(&online));
	UK_TEST_EXPECT(uk_cpumask_test_cpu(cpu, &online));
	UK_TEST_EXPECT(uk_cpumask_equal(&own, &online));
}

UK_TESTCASE(uksched_affinity, test_setaffinity_invalid)
{
	struct uk_thread *cur = uk_thread_current();
	struct uk_cpumask m, back;
	int rc;

	uk_sched_thread_getaffinity(cur, &back);

	/* Empty mask: Linux returns EINVAL */
	uk_cpumask_zero(&m);
	rc = uk_sched_thread_setaffinity(cur, &m);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EINVAL);

	/* A mask whose only CPU is online-ineligible (not online) is
	 * rejected. On images where every supported CPU is online there
	 * is no such CPU to name, so the case self-skips.
	 */
	{
		struct uk_cpumask online;
		unsigned int cpu;

		uk_cpumask_online(&online);
		uk_cpumask_zero(&m);
		for (cpu = 0; cpu < UK_CPUMASK_BITS; cpu++)
			if (!uk_cpumask_test_cpu(cpu, &online)) {
				uk_cpumask_set_cpu(cpu, &m);
				break;
			}
		if (!uk_cpumask_empty(&m)) {
			rc = uk_sched_thread_setaffinity(cur, &m);
			UK_TEST_EXPECT_SNUM_EQ(rc, -EINVAL);
		}
	}

	/* NULL arguments */
	rc = uk_sched_thread_setaffinity(NULL, &back);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EINVAL);
	rc = uk_sched_thread_setaffinity(cur, NULL);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EINVAL);

	/* The failed calls must have left the mask untouched */
	uk_sched_thread_getaffinity(cur, &m);
	UK_TEST_EXPECT(uk_cpumask_equal(&m, &back));
}

UK_TESTCASE(uksched_affinity, test_setaffinity_roundtrip_self)
{
	struct uk_thread *cur = uk_thread_current();
	struct uk_cpumask m, back;
	int rc;

	/* Pin to the current CPU only: legal everywhere, and on this
	 * single-running-thread test the hand-off path is never taken.
	 */
	uk_cpumask_zero(&m);
	uk_cpumask_set_cpu(uk_sched_current_cpu(), &m);
	rc = uk_sched_thread_setaffinity(cur, &m);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);

	uk_sched_thread_getaffinity(cur, &back);
	UK_TEST_EXPECT(uk_cpumask_equal(&back, &m));

	/* Restore the unrestricted mask */
	uk_cpumask_online(&m);
	rc = uk_sched_thread_setaffinity(cur, &m);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	uk_sched_thread_getaffinity(cur, &back);
	UK_TEST_EXPECT(uk_cpumask_equal(&back, &m));
}

uk_testsuite_register(uksched_affinity, NULL);
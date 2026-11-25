/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2026, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/* Fixed-size CPU bitmaps used for thread affinity masks.
 *
 * A mask addresses logical CPUs by their linear CPU index
 * (`uk_pcpuvar_cpu_idx`), the same index the platform LCPU code and the
 * application per-core tables use. The bitmap is statically sized from
 * `CONFIG_UKPLAT_CPU_MAXCOUNT`, so a `uk_cpumask_t` lives anywhere a
 * `struct uk_thread` lives: no allocation, safe before the heap exists.
 */

#ifndef __UK_CPUMASK_H__
#define __UK_CPUMASK_H__

#include <string.h>
#include <uk/plat/config.h>
#include <uk/arch/types.h>
#include <uk/essentials.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UK_CPUMASK_BITS CONFIG_UKPLAT_CPU_MAXCOUNT
#define UK_CPUMASK_WORDS DIV_ROUND_UP(UK_CPUMASK_BITS, 8 * sizeof(long))

struct uk_cpumask {
	unsigned long bits[UK_CPUMASK_WORDS];
};

#define UK_CPUMASK_BYTES (sizeof(struct uk_cpumask))

static inline void uk_cpumask_zero(struct uk_cpumask *m)
{
	memset(m->bits, 0, sizeof(m->bits));
}

static inline void uk_cpumask_copy(struct uk_cpumask *dst,
				   const struct uk_cpumask *src)
{
	memcpy(dst->bits, src->bits, sizeof(dst->bits));
}

static inline void uk_cpumask_set_cpu(unsigned int cpu, struct uk_cpumask *m)
{
	if (cpu < UK_CPUMASK_BITS)
		m->bits[cpu / (8 * sizeof(long))] |=
		    1UL << (cpu % (8 * sizeof(long)));
}

static inline void uk_cpumask_clear_cpu(unsigned int cpu, struct uk_cpumask *m)
{
	if (cpu < UK_CPUMASK_BITS)
		m->bits[cpu / (8 * sizeof(long))] &=
		    ~(1UL << (cpu % (8 * sizeof(long))));
}

static inline int uk_cpumask_test_cpu(unsigned int cpu,
				      const struct uk_cpumask *m)
{
	if (cpu >= UK_CPUMASK_BITS)
		return 0;
	return !!(m->bits[cpu / (8 * sizeof(long))] &
		  (1UL << (cpu % (8 * sizeof(long)))));
}

static inline int uk_cpumask_empty(const struct uk_cpumask *m)
{
	unsigned int i;

	for (i = 0; i < UK_CPUMASK_WORDS; i++)
		if (m->bits[i])
			return 0;
	return 1;
}

/* True if both masks have at least one CPU in common */
static inline int uk_cpumask_intersects(const struct uk_cpumask *a,
					const struct uk_cpumask *b)
{
	unsigned int i;

	for (i = 0; i < UK_CPUMASK_WORDS; i++)
		if (a->bits[i] & b->bits[i])
			return 1;
	return 0;
}

/* dst = dst & src */
static inline void uk_cpumask_and(struct uk_cpumask *dst,
				  const struct uk_cpumask *a,
				  const struct uk_cpumask *b)
{
	unsigned int i;

	for (i = 0; i < UK_CPUMASK_WORDS; i++)
		dst->bits[i] = a->bits[i] & b->bits[i];
}

static inline int uk_cpumask_equal(const struct uk_cpumask *a,
				   const struct uk_cpumask *b)
{
	return !memcmp(a->bits, b->bits, sizeof(a->bits));
}

/* Lowest set CPU id, or -1 when the mask is empty */
static inline int uk_cpumask_first(const struct uk_cpumask *m)
{
	unsigned int i;

	for (i = 0; i < UK_CPUMASK_BITS; i++)
		if (uk_cpumask_test_cpu(i, m))
			return (int)i;
	return -1;
}

/**
 * Load a user-supplied affinity mask into a `uk_cpumask_t`.
 *
 * The caller buffer holds `cpusetsize` bytes of a little-endian CPU bitmap,
 * the Linux `cpu_set_t` wire format. Bytes beyond the kernel mask size are
 * ignored, missing bytes read as zero. Rejects trailing bits that reference
 * CPU ids the image cannot host (`>= UK_CPUMASK_BITS`).
 *
 * @param m
 *   Destination mask (fully written, also on failure)
 * @param buf
 *   Source buffer, may be shorter or longer than the kernel mask
 * @param cpusetsize
 *   Size of `buf` in bytes
 * @return
 *   - (0):  Success
 *   - (-EINVAL): cpusetsize <= 0, buf == NULL, or buf names an out-of-range
 *               CPU id
 */
static inline int uk_cpumask_from_user(struct uk_cpumask *m,
				       const unsigned long *buf,
				       long cpusetsize)
{
	unsigned int w;

	if (!buf || cpusetsize <= 0)
		return -EINVAL;

	uk_cpumask_zero(m);
	for (w = 0; w < UK_CPUMASK_WORDS; w++) {
		const long bstart = (long)(w * sizeof(unsigned long));

		if (bstart >= cpusetsize)
			break;
		if (cpusetsize - bstart >= (long)sizeof(unsigned long))
			m->bits[w] = buf[w];
		else {
			/* Partial last word: take whole bytes, round the
			 * remainder up as the Linux kernel does.
			 */
			unsigned int nbytes =
			    (unsigned int)(cpusetsize - bstart);
			unsigned char *d = (unsigned char *)&m->bits[w];
			const unsigned char *s = (const unsigned char *)buf;

			memcpy(d, s + bstart, nbytes);
			memset(d + nbytes, 0, sizeof(m->bits[w]) - nbytes);
		}
	}

	/* Bits beyond the supported CPU range must not be set.
	 * This catches CPU ids that the image can never host.
	 */
	{
		unsigned int used = UK_CPUMASK_BITS % (8 * sizeof(long));

		if (used != 0) {
			/* High bits of the last word, above UK_CPUMASK_BITS */
			unsigned long tail_mask = ~0UL << used;

			if (m->bits[UK_CPUMASK_WORDS - 1] & tail_mask)
				return -EINVAL;
		}
	}

	return 0;
}

/**
 * Store a `uk_cpumask_t` into a user-supplied affinity mask buffer.
 *
 * Copies min(cpusetsize, kernel mask size) bytes and zero-fills the rest of
 * the caller buffer, matching the Linux `sched_getaffinity(2)` behavior.
 *
 * @return
 *   - (>0): Number of bytes of the kernel mask (`cpumask_size()`)
 *   - (-EINVAL): cpusetsize <= 0
 *   - (-EFAULT): buf == NULL
 */
static inline long uk_cpumask_to_user(unsigned long *buf, long cpusetsize,
				      const struct uk_cpumask *m)
{
	unsigned int ncopy;

	if (cpusetsize <= 0)
		return -EINVAL;
	if (!buf)
		return -EFAULT;

	ncopy = MIN((unsigned long)cpusetsize, UK_CPUMASK_BYTES);
	memcpy(buf, m->bits, ncopy);
	if ((unsigned long)cpusetsize > UK_CPUMASK_BYTES)
		memset((char *)buf + UK_CPUMASK_BYTES, 0,
		       (unsigned long)cpusetsize - UK_CPUMASK_BYTES);

	return (long)UK_CPUMASK_BYTES;
}

#define uk_cpumask_for_each_cpu(cpu, m)                                        \
	for ((cpu) = 0; (cpu) < UK_CPUMASK_BITS; (cpu)++)                      \
		if (uk_cpumask_test_cpu((cpu), (m)))

#ifdef __cplusplus
}
#endif

#endif /* __UK_CPUMASK_H__ */
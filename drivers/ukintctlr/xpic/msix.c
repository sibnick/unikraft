/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * MSI-X vector allocation and message construction for the x86_64
 * APIC-based interrupt controller (xpic).
 *
 * Copyright (c) 2026, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <uk/intctlr.h>
#include <uk/intctlr/msix.h>
#include <uk/intctlr/limits.h>
#include <uk/pcpuvar.h>

#if CONFIG_LIBUKINTCTLR_APIC

/*
 * xAPIC physical destination mode: the low 32 bits of the 64-bit
 * MSI address are the APIC MMIO base (0xFEE00000) with the
 * destination APIC ID in bits 31:24. The high 32 bits are zero.
 */
#define UK_MSIX_XAPIC_ADDR_BASE		0xFEE00000ULL

/*
 * Two bands stay outside the allocatable pool: the wake IPI (irq
 * 208, vector 0xF0) and the per-vCPU LAPIC one-shot (irq 16,
 * vector 0x30). With the pool spanning irq 20..207 (see limits.h)
 * an allocated MSI-X vector can never collide with either band,
 * and the IPI band no longer shares a vector with the one-shot.
 */
#define UK_MSIX_FIRST_VECTOR		(32 + UK_INTCTLR_FIRST_ALLOCABLE_IRQ)

static unsigned int apic_id_of_lcpu(unsigned int lcpu)
{
	unsigned int id = (unsigned int)uk_pcpuvar_lval(lcpu,
							  uk_pcpuvar_cpu_id);

	/*
	 * If bring-up did not populate a slot, the code falls back to
	 * the vCPU index. KVM and QEMU use that index as the APIC ID.
	 */
	if (id == 0 && lcpu != 0)
		id = lcpu;

	return id;
}

int uk_intctlr_msix_alloc(unsigned int lcpu, unsigned int *irq,
			   __u64 *addr, __u32 *data)
{
	unsigned int nirq;
	__u32 vector;
	int rc;

	if (!irq || !addr || !data)
		return -EINVAL;

	rc = uk_intctlr_irq_alloc(&nirq, 1);
	if (rc)
		return rc;

	vector = 32 + nirq;

	/*
	 * The spurious vector 0xff never enters a table entry, and
	 * the reserved IPI band stays clear. The check guards against
	 * a pool that grows past its last valid vector.
	 */
	if (vector < UK_MSIX_FIRST_VECTOR || vector >= 0xff) {
		uk_intctlr_irq_free(&nirq, 1);
		return -ENOSPC;
	}

	*irq = nirq;
	*data = vector;

	/*
	 * xAPIC physical destination mode: the destination APIC ID
	 * goes into bits 19:12 of the MSI address. Bits 31:20 are
	 * the fixed 0xFEE prefix. A destination written to bits
	 * 31:24 breaks that prefix, names a non-LAPIC address, and
	 * the message is lost.
	 */
	*addr = UK_MSIX_XAPIC_ADDR_BASE |
		((__u64)apic_id_of_lcpu(lcpu) << 12);

	return 0;
}

int uk_intctlr_msix_free(unsigned int irq)
{
	unsigned int nirq = irq;

	return uk_intctlr_irq_free(&nirq, 1);
}

#else /* !CONFIG_LIBUKINTCTLR_APIC */

int uk_intctlr_msix_alloc(unsigned int lcpu, unsigned int *irq,
			   __u64 *addr, __u32 *data)
{
	(void)lcpu;
	(void)irq;
	(void)addr;
	(void)data;
	return -ENOTSUP;
}

int uk_intctlr_msix_free(unsigned int irq)
{
	(void)irq;
	return -ENOTSUP;
}

#endif /* !CONFIG_LIBUKINTCTLR_APIC */

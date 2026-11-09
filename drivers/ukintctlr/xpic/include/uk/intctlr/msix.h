/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * MSI-X vector allocation and message construction for the x86_64
 * APIC-based interrupt controller (xpic).
 *
 * Copyright (c) 2026, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#ifndef __UK_INTCTLR_MSIX_H__
#define __UK_INTCTLR_MSIX_H__

#include <uk/essentials.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate one MSI-X vector and build its 16-byte message.
 *
 * Reserves one irq from the uk_intctlr pool. The hardware vector
 * number is 32 + irq, so the interrupt lands in the target lcpu's
 * IDT IRQ gate. uk_intctlr dispatches the interrupt, and the APIC
 * handler writes the EOI.
 *
 * The message targets lcpu @p lcpu in xAPIC physical destination
 * mode. The function reads the destination APIC ID from the
 * lcpu's recorded initial APIC ID (uk_pcpuvar_cpu_id). KVM and
 * QEMU set that ID equal to the vCPU index.
 *
 * @param lcpu    Target lcpu that should receive the interrupt.
 * @param irq     Output: allocated irq number (vector = 32 + irq).
 * @param addr    Output: 64-bit MSI address to store in table
 *                words 0 and 1 (low and high 32-bit halves).
 * @param data    Output: 32-bit data word (the vector) to store in
 *                table word 2.
 * @return zero on success, or a negative errno value on failure.
 */
int uk_intctlr_msix_alloc(unsigned int lcpu, unsigned int *irq,
			  __u64 *addr, __u32 *data);

/**
 * Free a vector previously allocated by uk_intctlr_msix_alloc().
 *
 * @param irq The irq number returned by uk_intctlr_msix_alloc().
 * @return zero on success, or a negative errno value on failure.
 */
int uk_intctlr_msix_free(unsigned int irq);

#ifdef __cplusplus
}
#endif

#endif /* __UK_INTCTLR_MSIX_H__ */

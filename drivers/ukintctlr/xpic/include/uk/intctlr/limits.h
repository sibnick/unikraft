/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#ifndef __UK_INTCTLR_XPIC_LIMITS_H__
#define __UK_INTCTLR_XPIC_LIMITS_H__

/*
 * The per-vCPU LAPIC one-shot timer keeps irq 16 (vector 0x30).
 * That band stays outside the allocatable pool, so an allocated
 * MSI-X or device vector can never collide with it.
 * [Ticket a5b91e7993]
 *
 * The allocatable pool spans irq 20..207, that is vectors 52..239.
 * Vectors 240..254 stay outside the pool. The spurious vector 255
 * stays outside the pool.
*/
#define UK_INTCTLR_FIRST_ALLOCABLE_IRQ			20
#define UK_INTCTLR_LAST_ALLOCABLE_IRQ			207
#define UK_INTCTLR_MAX_IRQ				(256 - 32)

#define UK_INTCTLR_ALLOCABLE_IRQ_COUNT			\
	(UK_INTCTLR_LAST_ALLOCABLE_IRQ - UK_INTCTLR_FIRST_ALLOCABLE_IRQ + 1)

#endif /* __UK_INTCTLR_XPIC_LIMITS_H__ */

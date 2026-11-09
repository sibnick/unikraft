/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <uk/arch/util.h>
#include <uk/arch/x86_64.h>
#include <uk/assert.h>
#include <uk/event.h>
#include <uk/config.h>
#include <uk/intctlr.h>
#include <uk/lcpu.h>
#include <uk/pcpuvar.h>
#include <uk/plat/time.h>

#if CONFIG_LIBUKINTCTLR_APIC
#include <uk/arch/x86_64.h>
#include <uk/arch/util.h>

static __u64 apic_mmio_base;

/*
 * Force the local APIC into xAPIC (MMIO) mode. The EXTD bit (10)
 * of the APIC base MSR is never set: the x2APIC TSC-deadline
 * timer is broken on this platform, and secondary cores rely on
 * the xAPIC one-shot timer for waking from hlt.
 */
static inline int xapic_enable(void)
{
	__u32 eax, edx;

	uk_arch_x86_64_rdmsr(UK_ARCH_X86_64_APIC_MSR_BASE, &eax, &edx);

	/* MMIO base is bits 35:12 of the base MSR; default is 0xFEE00000 */
	apic_mmio_base = (((__u64)edx << 32) | (__u64)eax) & 0xFFFFF000UL;
	if (!apic_mmio_base)
		apic_mmio_base = 0xFEE00000UL;

	/* Set EN (bit 11) and clear EXTD (bit 10) */
	eax |= UK_ARCH_X86_64_APIC_BASE_EN;
	eax &= ~UK_ARCH_X86_64_APIC_BASE_EXTD;
	uk_arch_x86_64_wrmsr(UK_ARCH_X86_64_APIC_MSR_BASE, eax, edx);

	/*
	 * Software enable: set the enable bit (0x100) and the spurious
	 * vector in the SVR register at offset 0xF0.
	 */
	volatile __u32 *svr = (volatile __u32 *)(apic_mmio_base + 0xF0);

	*svr = (*svr) | 0x100 | 0xFF;

	return 0;
}

static inline void xapic_ack_interrupt(void)
{
	volatile __u32 *eoi = (volatile __u32 *)(apic_mmio_base + 0x0B0);

	*eoi = 0;
}


#define apic_ack_interrupt	xapic_ack_interrupt
#define apic_enable		xapic_enable
#endif /* CONFIG_LIBUKINTCTLR_APIC */

#include "pic.h"

static struct uk_intctlr_desc intctlr;

static int configure_irq(struct uk_intctlr_irq *irq __unused)
{
	return 0;
}

static int uk_intctlr_xpic_handle_irq(void *data)
{
	struct uk_lcpu_except_irq_ctx *ctx;
	__u32 irq;

	ctx = data;
	uk_intctlr_irq_handle(ctx);

	irq = uk_lcpu_except_irq_ctx_get_irq(ctx);

#if CONFIG_LIBUKINTCTLR_APIC
	apic_ack_interrupt();

	/* FIXME This is here because right now we only use
	 * APIC for IPIs on SMP. This should be removed as
	 * soon as we fully implement APIC and get rid of
	 * PIC
	 */
	/*
	 * The 8259 decodes irq 0..15 only. The per-vCPU LAPIC
	 * timer uses irq 16: it must not send an EOI to the
	 * 8259. [Ticket a5b91e7993]
	 */
	if (irq <= 15)
		pic_ack_irq(irq);
#else   /* !CONFIG_LIBUKINTCTLR_APIC */
	pic_ack_irq(irq);
#endif /* !CONFIG_LIBUKINTCTLR_APIC */

	return UK_EVENT_HANDLED;
}

UK_EVENT_HANDLER(UK_LCPU_EXCEPT_EVENT_IRQ, uk_intctlr_xpic_handle_irq);

int uk_intctlr_probe(void)
{
	int rc = -ENODEV;
	struct uk_intctlr_driver_ops *ops;

	rc = pic_init(&ops);
	if (unlikely(rc))
		return rc;

#if CONFIG_LIBUKINTCTLR_APIC
	apic_enable();
	intctlr.name = "APIC";
#else /* ! CONFIG_LIBUKINTCTLR_APIC */
	intctlr.name = "PIC";
#endif /* CONFIG_LIBUKINTCTLR_APIC */

	intctlr.ops = ops;
	intctlr.ops->configure_irq = configure_irq;

	return uk_intctlr_register(&intctlr);
}

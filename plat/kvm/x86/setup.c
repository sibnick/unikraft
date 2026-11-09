/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <string.h>
#include <x86/traps.h>
#if CONFIG_LIBUKACPI
#include <uk/acpi.h>
#endif /* CONFIG_LIBUKACPI */
#include <uk/arch/limits.h>
#include <uk/arch/types.h>
#include <uk/arch/util.h>
#include <uk/arch/x86_64.h>
#include <uk/asm/cfi.h>
#include <uk/boot.h>
#include <uk/paging.h>
#include <uk/plat/console.h>
#include <uk/assert.h>
#include <uk/essentials.h>
#include <uk/intctlr.h>

#include <uk/lcpu.h>
#include <uk/plat/common/sections.h>
#include <uk/plat/common/bootinfo.h>
#include <uk/plat/common/memory.h>

#if CONFIG_HAVE_SYSCALL
/* syscall entrance provided by platform library */
void _ukplat_syscall(void);

/* _init_syscall is derived from hermitux: `processor.c`:
 *
 * Copyright (c) 2010, Stefan Lankes, RWTH Aachen University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the University nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

static inline void _init_syscall(void)
{
	__u32 eax, ebx, ecx, edx;
	int have_syscall = 0;

	/* Check for availability of extended features */
	uk_arch_x86_64_cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
	if (eax >= 0x80000001) {
		uk_arch_x86_64_cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
		have_syscall = (edx & UK_ARCH_X86_64_CPUID3_SYSCALL);
	}

	if (!have_syscall)
		UK_CRASH("CPU does not support SYSCALL/SYSRET!\n");

	/* Enable and program syscall/sysret */
	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_MSR_EFER,
			      uk_arch_x86_64_rdmsrl(UK_ARCH_X86_64_MSR_EFER) |
			      UK_ARCH_X86_64_EFER_LMA |
			      UK_ARCH_X86_64_EFER_LME |
			      UK_ARCH_X86_64_EFER_SCE);
	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_MSR_STAR,
			      (0x08ULL << 48) | (0x08ULL << 32));
	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_MSR_LSTAR,
			      (__uptr)_ukplat_syscall);

	/* Clear IF flag during an interrupt */
	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_MSR_SYSCALL_MASK,
			      UK_ARCH_X86_64_RFLAGS_TF |
			      UK_ARCH_X86_64_RFLAGS_DF |
			      UK_ARCH_X86_64_RFLAGS_IF |
			      UK_ARCH_X86_64_RFLAGS_AC |
			      UK_ARCH_X86_64_RFLAGS_NT);

	uk_pr_info("SYSCALL entrance @ %p\n", _ukplat_syscall);
}
#endif /* CONFIG_HAVE_SYSCALL */

#if CONFIG_HAVE_X86PKU
static inline void _check_ospke(void)
{
	__u32 eax, ebx, ecx, edx;

	uk_arch_x86_64_cpuid(0x7, 0, &eax, &ebx, &ecx, &edx);
	if (!(ecx & UK_ARCH_X86_64_CPUID7_ECX_OSPKE)) {
		/* if PKU is not enabled, abort the boot process. Images
		 * compiled with HAVE_X86PKU are *specialized* to be executed on
		 * PKU-enabled hardware. This allows us to avoid checks later at
		 * runtime.
		 */
		UK_CRASH("CPU does not support PKU!\n");
	}
}
#endif /* CONFIG_HAVE_X86PKU */

static void __noreturn ukplat_entry2(void *arg __unused)
{
	/* It's not possible to unwind past this function, because the stack
	 * pointer was overwritten in uk_arch_jump_to. Therefore, mark the
	 * previous instruction pointer as undefined, so that debuggers or
	 * profilers stop unwinding here.
	 */
	ukarch_cfi_unwind_end();

	uk_boot_entry();
	UK_BUG(); /* noreturn */
}

/* At this point we expect that the C runtime is configured and that
 * bootcode has enabled all CPU features used by compiled code.
 */
/*
 * Base address of the local APIC MMIO page, or 0 when the APIC is
 * disabled or in x2APIC mode. x2APIC uses MSRs instead of MMIO.
 */
static __u64 lapic_base;

static __u64 ukplat_lapic_mmio_base(void)
{
	__u32 lo = 0, hi = 0;
	__u64 base;

	uk_arch_x86_64_rdmsr(UK_ARCH_X86_64_APIC_MSR_BASE, &lo, &hi);
	if (!(lo & UK_ARCH_X86_64_APIC_BASE_EN))
		return 0;
	if (lo & UK_ARCH_X86_64_APIC_BASE_EXTD)
		return 0;

	base = ((((__u64)hi << 32) | (__u64)lo) & 0xFFFFF000UL);
	if (!base)
		base = 0xFEE00000UL;

	return base;
}

/*
 * Register the local APIC MMIO page as a platform memory region.
 *
 * GRUB may not report the page at the APIC base address in the
 * memory map. The paging boot then leaves it unmapped and the
 * per-vCPU LAPIC one-shot wake path in tscclock faults on its
 * first MMIO access. Add the page as a reserved region before the
 * paging boot init runs, so the kernel page table maps it.
 * [Ticket 4ecf9dcedf]
 */
static void ukplat_add_lapic_region(struct ukplat_bootinfo *bi)
{
	struct ukplat_memregion_desc mrd = {0};

	lapic_base = ukplat_lapic_mmio_base();
	if (!lapic_base)
		return;

	mrd.pbase = lapic_base;
	mrd.vbase = mrd.pbase;
	mrd.len = 0x1000;
	mrd.type = UKPLAT_MEMRT_RESERVED;
	mrd.flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_WRITE;
	mrd.pg_off = mrd.vbase % UK_PAGING_PAGE_SIZE;
	mrd.pg_count = UK_PAGING_PAGE_COUNT(mrd.pg_off + mrd.len);

	if (ukplat_memregion_list_insert(&bi->mrds, &mrd))
		uk_pr_warn("kvm: could not register local APIC page 0x%llx\n",
			   (unsigned long long)mrd.pbase);
}

#if CONFIG_LIBUKPAGING
#include <uk/paging.h>

/*
 * Map the local APIC page uncacheable. The paging boot maps regions
 * with protection bits only, so the page starts cached. Cacheable MMIO
 * is architecturally undefined for the local APIC. Set uncacheable
 * (PCD and PWT) on the page after the kernel page table is active.
 * [Ticket 96a26ff367]
 */
static void ukplat_map_lapic_uncacheable(void)
{
	int rc;

	if (!lapic_base)
		return;

	rc = uk_paging_page_set_attr(uk_paging_pt_get_active(),
				     (__vaddr_t)lapic_base, 1,
				     UK_PAGING_PAGE_ATTR_PROT_RW |
				     UK_ARCH_X86_64_PAGE_ATTR_WRITECOMBINE,
				     0);
	if (rc)
		uk_pr_warn("kvm: uncacheable remap of APIC page 0x%llx failed: %d\n",
			   (unsigned long long)lapic_base, rc);
}
#endif /* CONFIG_LIBUKPAGING */

void _ukplat_entry(struct ukplat_bootinfo *bi)
{
	void *bstack;
	int rc;

	/* Initialize LCPU of bootstrap processor */
	rc = uk_lcpu_init(uk_pcpuvar_current_ptr_get(uk_lcpus));
	if (unlikely(rc))
		UK_CRASH("Bootstrap processor init failed: %d\n", rc);

	/* Map the local APIC page before the paging boot init */
	ukplat_add_lapic_region(bi);

	/* Execute ealry init */
	uk_boot_early_init(bi);

	/* Initialize IRQ controller */
	rc = uk_intctlr_probe();
	if (unlikely(rc))
		UK_CRASH("Interrupt controller init failed: %d\n", rc);

#if CONFIG_HAVE_SMP
	rc = uk_lcpu_mp_init(CONFIG_LIBUKLCPU_RUN_IRQ,
			     CONFIG_LIBUKLCPU_WAKEUP_IRQ);
	if (unlikely(rc))
		uk_pr_err("SMP init failed: %d\n", rc);
#endif /* CONFIG_HAVE_SMP */

	/* Allocate boot stack */
	bstack = ukplat_memregion_alloc(__STACK_SIZE, UKPLAT_MEMRT_STACK,
					UKPLAT_MEMRF_READ |
					UKPLAT_MEMRF_WRITE);
	if (unlikely(!bstack))
		UK_CRASH("Boot stack alloc failed\n");

	bstack = (void *)((__uptr)bstack + __STACK_SIZE);

	/* Initialize memory */
	rc = ukplat_mem_init();
	if (unlikely(rc))
		UK_CRASH("Mem init failed: %d\n", rc);

#if CONFIG_LIBUKPAGING
	/* The kernel page table is active now. Set the APIC page
	 * uncacheable for the MMIO access rules.
	 */
	ukplat_map_lapic_uncacheable();
#endif /* CONFIG_LIBUKPAGING */

#ifdef CONFIG_HAVE_SYSCALL
	_init_syscall();
#endif /* CONFIG_HAVE_SYSCALL */

#if CONFIG_HAVE_X86PKU
	_check_ospke();
#endif /* CONFIG_HAVE_X86PKU */

	/* Switch away from the bootstrap stack */
	uk_pr_info("Switch from bootstrap stack to stack @%p\n", bstack);
	uk_arch_x86_64_jump_to((__u64)bstack, (__u64)ukplat_entry2);
}

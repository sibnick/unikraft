/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * KVM x86_64 platform: secondary CPU (AP) bring-up.
 *
 * Secondary cores enter the 16-bit real-mode trampoline in lcpu_start.S.
 * That code is part of the image, which is linked at 1 MiB, but the
 * trampoline must sit in the first 1 MiB of physical memory. A copy of it
 * is placed in a fixed hole below 1 MiB and its internal references are
 * fixed up before any AP is started.
 *
 * APs are started with the standard x86 INIT/SIPI sequence through the
 * local APIC. The mode (xapic MMIO or x2apic MSR) is detected at boot
 * from the APIC base MSR.
 *
 * Based on the native platform lcpu_pm.c (same start16 relocation logic).
 */

#include <uk/arch/util.h>
#include <uk/arch/x86_64.h>
#include <uk/atomic.h>
#include <uk/essentials.h>
#include <uk/print.h>
#include <uk/lcpu.h>
#include <uk/lcpu/pm.h>
#include <uk/pcpuvar.h>
#include <uk/prio.h>

#if CONFIG_LIBUKBOOT
#include <uk/boot/earlytab.h>
#include <uk/plat/common/bootinfo.h>
#include <uk/plat/common/memory.h>
#include <string.h>
#endif /* CONFIG_LIBUKBOOT */

#include <x86/delay.h>

#if CONFIG_HAVE_SMP
static inline void x2apic_send_sipi(__vaddr_t addr, int dest)
{
	__u32 eax;

	UK_ASSERT((addr &
		   (UK_ARCH_X86_64_APIC_ICR_VECTOR_MASK << __PAGE_SHIFT)) == addr);

	eax = UK_ARCH_X86_64_APIC_ICR_TRIGGER_LEVEL |
	      UK_ARCH_X86_64_APIC_ICR_LEVEL_ASSERT |
	      UK_ARCH_X86_64_APIC_ICR_DESTMODE_PHYSICAL |
	      UK_ARCH_X86_64_APIC_ICR_DMODE_SUP |
	      (addr >> __PAGE_SHIFT);

	uk_arch_x86_64_wrmsr(UK_ARCH_X86_64_APIC_MSR_ICR, eax, dest);
}

static inline void x2apic_send_iipi(int dest)
{
	__u32 eax;

	eax = UK_ARCH_X86_64_APIC_ICR_TRIGGER_LEVEL |
	      UK_ARCH_X86_64_APIC_ICR_LEVEL_ASSERT |
	      UK_ARCH_X86_64_APIC_ICR_DESTMODE_PHYSICAL |
	      UK_ARCH_X86_64_APIC_ICR_DMODE_INIT;

	uk_arch_x86_64_wrmsr(UK_ARCH_X86_64_APIC_MSR_ICR, eax, dest);
}

/* Deassert only supported on Pentium and P6 familiy processors */
#define x2apic_send_iipi_deassert() {}

/*
 * The BSP may run with either the xapic (MMIO) or the x2apic (MSR) local
 * APIC mode enabled, depending on the boot protocol and the hypervisor.
 * Detect the active mode at boot and dispatch IPIs to it.
 */
static int apic_mode; /* 0 = xapic, 1 = x2apic */
static __u64 apic_mmio_base;

static int apic_mode_detect(void)
{
	__u32 lo = 0, hi = 0;

	uk_arch_x86_64_rdmsr(0x01B, &lo, &hi);

	if (unlikely(!(lo & (1u << 11))))
		return -ENODEV;

	if (lo & (1u << 10))
		uk_pr_err("kvm: x2APIC (EXTD) set in APIC base MSR; forcing xAPIC mode\n");

	apic_mode = 0;
	apic_mmio_base = (((__u64)hi << 32) | (__u64)lo) & 0xFFFFF000UL;
	if (unlikely(!apic_mmio_base))
		apic_mmio_base = 0xFEE00000UL;
	return 0;
}

/*
 * Bound for one ICR wait. The delivery-status bit clears as soon as
 * the local APIC accepts the write. One hundred milliseconds per
 * wait covers a slow hypervisor. A hypervisor that never clears the
 * bit must not hang boot. [Ticket f05cbd814a]
 */
#define UK_XAPIC_ICR_TIMEOUT_US		100000u

/* Wait until the delivery-status bit (12) of ICR-low is clear.
 * @return 0 on accept, -ETIMEDOUT if the bit stays set. */
static int xapic_icr_wait(volatile const __u32 *icr_lo)
{
	unsigned int us;

	for (us = 0; us < UK_XAPIC_ICR_TIMEOUT_US; us++) {
		if (!(*icr_lo & (1u << 12)))
			return 0;
		uk_arch_x86_64_spinwait();
		udelay(1);
	}

	return -ETIMEDOUT;
}

static inline int xapic_write_icr(__u32 lo, __u64 dest)
{
	volatile __u32 *icr_hi = (volatile __u32 *)(apic_mmio_base + 0x310);
	volatile __u32 *icr_lo = (volatile __u32 *)(apic_mmio_base + 0x300);
	int rc;

	rc = xapic_icr_wait(icr_lo);
	if (unlikely(rc))
		return rc;

	*icr_hi = ((__u32)dest) << 24;
	*icr_lo = lo;

	return xapic_icr_wait(icr_lo);
}

static int apic_send_sipi(__u64 addr, __u64 dest)
{
	if (apic_mode) {
		x2apic_send_sipi(addr, (int)dest);
		return 0;
	}

	__u32 eax = UK_ARCH_X86_64_APIC_ICR_TRIGGER_EDGE |
		    UK_ARCH_X86_64_APIC_ICR_LEVEL_ASSERT |
		    UK_ARCH_X86_64_APIC_ICR_DESTMODE_PHYSICAL |
		    UK_ARCH_X86_64_APIC_ICR_DMODE_SUP |
		    ((addr >> __PAGE_SHIFT) & UK_ARCH_X86_64_APIC_ICR_VECTOR_MASK);

	return xapic_write_icr(eax, dest);
}

static int apic_send_iipi(__u64 dest)
{
	if (apic_mode) {
		x2apic_send_iipi((int)dest);
		return 0;
	}

	__u32 eax = UK_ARCH_X86_64_APIC_ICR_TRIGGER_LEVEL |
		    UK_ARCH_X86_64_APIC_ICR_LEVEL_ASSERT |
		    UK_ARCH_X86_64_APIC_ICR_DESTMODE_PHYSICAL |
		    UK_ARCH_X86_64_APIC_ICR_DMODE_INIT;

	return xapic_write_icr(eax, dest);
}

#define apic_send_iipi_deassert() do {} while (0)

/* Secondary cores start in 16-bit real-mode and we have to provide the
 * corresponding boot code somewhere in the first 1 MiB. We copy the trampoline
 * code to the target address during MP initialization.
 */
extern void *x86_start16_begin[];
extern void *x86_start16_end[];
extern __uptr uk_plat_native_x86_64_start16_addr; /* target address */

#define UK_ARCH_START16_SIZE						\
	((__uptr)x86_start16_end - (__uptr)x86_start16_begin)

#define START16_MOV_SYM(sym, sz)					\
	sym##_imm##sz##_start16

#define START16_DATA_SYM(sym, sz)					\
	sym##_data##sz##_start16

#define IMPORT_START16_SYM(sym, sz, type)				\
	extern void *sym[];						\
	extern void *START16_##type##_SYM(sym, sz)[]

#define START16_MOV_OFF(sym, sz)					\
	((void *)START16_MOV_SYM(sym, sz) -				\
	(void *)x86_start16_begin)

#define START16_DATA_OFF(sym, sz)					\
	((void *)START16_DATA_SYM(sym, sz) -				\
	(void *)x86_start16_begin)

IMPORT_START16_SYM(gdt32_ptr, 2, MOV);
IMPORT_START16_SYM(gdt32, 4, DATA);
IMPORT_START16_SYM(lcpu_start16, 2, MOV);
IMPORT_START16_SYM(jump_to32, 2, MOV);
IMPORT_START16_SYM(lcpu_start32, 4, MOV);

#define START16_RELOC_ENTRY(sym, sz, type)				\
	{								\
		.r_mem_off = START16_##type##_OFF(sym, sz),		\
		.r_addr = (void *)(sym) - (void *)x86_start16_begin,	\
		.r_sz = (sz),						\
	}

static void apply_start16_reloc(__u64 baddr, __u64 r_mem_off,
				__u64 r_addr, __u32 r_sz)
{
	switch (r_sz) {
	case 2:
		*(__u16 *)((__u8 *)baddr + r_mem_off) = (__u16)(baddr + r_addr);
		break;
	case 4:
		*(__u32 *)((__u8 *)baddr + r_mem_off) = (__u32)(baddr + r_addr);
		break;
	}
}

static void start16_reloc_mp_init(void)
{
	struct {
		__u64 r_mem_off;
		__u64 r_addr;
		__u32 r_sz;
	} x86_start16_relocs[] = {
		START16_RELOC_ENTRY(lcpu_start16, 2, MOV),
		START16_RELOC_ENTRY(gdt32_ptr, 2, MOV),
		START16_RELOC_ENTRY(gdt32, 4, DATA),
		START16_RELOC_ENTRY(jump_to32, 2, MOV),
		START16_RELOC_ENTRY(lcpu_start32, 4, MOV),
	};
	__sz i;

	for (i = 0; i < ARRAY_SIZE(x86_start16_relocs); i++)
		apply_start16_reloc((__u64)uk_plat_native_x86_64_start16_addr,
				    x86_start16_relocs[i].r_mem_off,
				    x86_start16_relocs[i].r_addr,
				    x86_start16_relocs[i].r_sz);

	/* Unlike the other entries, lcpu_start32 must stay the same
	 * as it is not part of the start16 section.
	 */
	apply_start16_reloc((__u64)uk_plat_native_x86_64_start16_addr,
			    START16_MOV_OFF(lcpu_start32, 4),
			    (__u64)lcpu_start32 -
			    (__u64)uk_plat_native_x86_64_start16_addr, 4);
}

#if CONFIG_LIBUKBOOT
static int boot_memregion_alloc_sipi_vect(struct ukplat_bootinfo *bi __unused)
{
#define X86_VIDEO_MEM_START	0xA0000UL
	__sz len;

	len = (__sz)((__uptr)x86_start16_end - (__uptr)x86_start16_begin);
	len = UK_PAGING_PAGE_ALIGN_UP(len);
	uk_plat_native_x86_64_start16_addr = (__uptr)ukplat_memregion_alloc(len,
							  UKPLAT_MEMRT_RESERVED,
							  UKPLAT_MEMRF_READ  |
							  UKPLAT_MEMRF_WRITE);
	if (unlikely(!uk_plat_native_x86_64_start16_addr ||
		     uk_plat_native_x86_64_start16_addr >= X86_VIDEO_MEM_START))
		return -ENOMEM;

	/* Copy AP startup code to target address in first 1MiB */
	UK_ASSERT(uk_plat_native_x86_64_start16_addr < 0x100000);
	memcpy((void *)uk_plat_native_x86_64_start16_addr, &x86_start16_begin,
	       UK_ARCH_START16_SIZE);

	start16_reloc_mp_init();

	uk_pr_info("Copied AP 16-bit boot code to 0x%"__PRIvaddr" (SIPI vector 0x%02lx)\n",
		   uk_plat_native_x86_64_start16_addr,
		   (unsigned long)(uk_plat_native_x86_64_start16_addr >> 12));

	return 0;
}
#endif /* CONFIG_LIBUKBOOT */

/*
 * The SIPI vector holds the startup page number of the trampoline.
 * Reject a zero address, an address that is not page aligned, and an
 * address at or above 1 MiB. A bad vector would send the AP to a
 * wrong real-mode address. [Ticket f05cbd814a]
 */
static int sipi_vector_check(__u64 addr)
{
	if (addr < __PAGE_SIZE || addr >= 0x100000ULL ||
	    (addr & (__PAGE_SIZE - 1)))
		return -EINVAL;

	return 0;
}

static int kvm_x86_lcpu_start(__u64 idx)
{
	int rc;

	/* Send INIT IPI */
	rc = apic_send_iipi(uk_pcpuvar_lval(idx, uk_pcpuvar_cpu_id));
	if (unlikely(rc)) {
		uk_pr_err("kvm: INIT IPI to AP 0x%lx failed: %d\n",
			  uk_pcpuvar_lval(idx, uk_pcpuvar_cpu_id), rc);
		return rc;
	}

	/* Deassert */
	apic_send_iipi_deassert();

	return 0;
}

/*
 * One SIPI round plus the trampoline run must finish fast. One
 * second per AP is far above any real boot time and still ends the
 * wait with a clear error. [Ticket f05cbd814a]
 */
#define UK_AP_ONLINE_TIMEOUT_MS		1000

#if CONFIG_HAVE_CPU_MULTI_PHASE_STARTUP
static int kvm_x86_lcpu_post_start(const __u64 lcpuidx[], unsigned int *num)
{
	__u64 id, this_cpu_id = uk_pcpuvar_current_get(uk_pcpuvar_cpu_id);
	unsigned int i, j;
	int rc = 0;

	if (unlikely(sipi_vector_check(
			     uk_plat_native_x86_64_start16_addr) != 0)) {
		uk_pr_err("GATE_FAIL: kvm: invalid SIPI vector 0x%lx; AP start refused\n",
			  (unsigned long)
			  uk_plat_native_x86_64_start16_addr);
		return -EINVAL;
	}

	/* wait 10 msec (according to Intel manual 8.4.4.1) */
	mdelay(10);

	for (i = 0; i < *num; i++) {
		id = uk_pcpuvar_lval(lcpuidx[i], uk_pcpuvar_cpu_id);
		if (id == this_cpu_id)
			continue;

		for (j = 0; j < 2; j++) {
			/* Send STARTUP IPI */
			rc = apic_send_sipi(uk_plat_native_x86_64_start16_addr,
					    id);
			if (unlikely(rc)) {
				uk_pr_err("GATE_FAIL: kvm: SIPI to AP 0x%lx failed: %d\n",
					  id, rc);
				return rc;
			}

			/* wait 200 usec (according to Intel manual 8.4.4.1) */
			udelay(200);
		}
	}

	/*
	 * Check that every AP came online. On the AP, uk_lcpu_init
	 * sets the per-CPU state to a busy state before the AP entry
	 * runs. Retry SIPI at mid-timeout if the AP has not come online.
	 * A core still offline after the timeout never started;
	 * print GATE_FAIL naming the AP and mark it halted.
	 * [Ticket f05cbd814a] [Ticket dd4a8c64ea]
	 */
	for (i = 0; i < *num; i++) {
		struct uk_lcpu *lcpu;
		unsigned int ms;
		int retried = 0;

		id = uk_pcpuvar_lval(lcpuidx[i], uk_pcpuvar_cpu_id);
		if (id == this_cpu_id)
			continue;

		lcpu = &uk_pcpuvar_lval(lcpuidx[i], uk_lcpus);
		for (ms = 0; ms < UK_AP_ONLINE_TIMEOUT_MS; ms++) {
			if (uk_lcpu_state_is_online(uk_load_n(&lcpu->state)))
				break;
			if (!retried && ms >= (UK_AP_ONLINE_TIMEOUT_MS / 2)) {
				/* Retry SIPI once before giving up */
				apic_send_sipi(uk_plat_native_x86_64_start16_addr, id);
				retried = 1;
			}
			mdelay(1);
		}

		if (unlikely(!uk_lcpu_state_is_online(
				     uk_load_n(&lcpu->state)))) {
			uk_pr_err("GATE_FAIL: kvm: AP 0x%lx did not come online within %d ms\n",
				  id, UK_AP_ONLINE_TIMEOUT_MS);
			lcpu->state = UK_LCPU_STATE_HALTED;
			lcpu->error_code = -ETIMEDOUT;
			if (!rc)
				rc = -ETIMEDOUT;
		}
	}

	return rc;
}
#endif /* CONFIG_HAVE_CPU_MULTI_PHASE_STARTUP */
#endif /* CONFIG_HAVE_SMP */

static void kvm_x86_lcpu_halt(void)
{
	uk_arch_x86_64_halt();
}

static void kvm_x86_lcpu_halt_irq(void)
{
	/*
	 * We have to be careful when enabling interrupts before entering a
	 * halt state. If we want to wait for an interrupt (e.g., a timer)
	 * the interrupt may fire in the short window between sti and hlt and
	 * we are going to halt forever. As sti only enables interrupts after
	 * the following instruction, we can avoid the race condition by
	 * ensuring that hlt immediately follows sti. There must be no
	 * instruction in between.
	 */
	asm volatile (
		"sti\n\t"
		"hlt\n\t"
		"cli\n\t"
		:
		:
		: "memory"
	);
}

static const struct uk_lcpu_pm_ops kvm_x86_pm_ops = {
#if CONFIG_HAVE_SMP
	.start = kvm_x86_lcpu_start,
#if CONFIG_HAVE_CPU_MULTI_PHASE_STARTUP
	.post_start = kvm_x86_lcpu_post_start,
#endif /* CONFIG_HAVE_CPU_MULTI_PHASE_STARTUP */
#endif /* CONFIG_HAVE_SMP */
	.halt = kvm_x86_lcpu_halt,
	.halt_irq = kvm_x86_lcpu_halt_irq,
};

#if CONFIG_HAVE_SMP
/*
 * Used when the AP start setup fails. Without .start,
 * uk_lcpu_start() returns -ENODEV and no AP receives an INIT or a
 * SIPI. The core keeps its halt operations, so boot continues.
 * [Ticket f05cbd814a]
 */
static const struct uk_lcpu_pm_ops kvm_x86_pm_ops_noap = {
	.halt = kvm_x86_lcpu_halt,
	.halt_irq = kvm_x86_lcpu_halt_irq,
};
#endif /* CONFIG_HAVE_SMP */

__isr static
int kvm_x86_lcpu_pm_ops_register(struct ukplat_bootinfo *bi __unused)
{
#if CONFIG_HAVE_SMP
	if (apic_mode_detect()) {
		uk_pr_err("kvm: local APIC not ready; SMP start disabled\n");
		return uk_lcpu_pm_ops_register(&kvm_x86_pm_ops_noap);
	}
#endif
#if CONFIG_LIBUKBOOT
	if (boot_memregion_alloc_sipi_vect(bi) < 0) {
#if CONFIG_HAVE_SMP
		uk_pr_err("kvm: failed to allocate SIPI vector trampoline; SMP start disabled\n");
		return uk_lcpu_pm_ops_register(&kvm_x86_pm_ops_noap);
#else
		uk_pr_warn("kvm: failed to allocate SIPI vector trampoline\n");
#endif
	}
#endif
	return uk_lcpu_pm_ops_register(&kvm_x86_pm_ops);
}

UK_BOOT_EARLYTAB_ENTRY(kvm_x86_lcpu_pm_ops_register, UK_PRIO_EARLIEST);

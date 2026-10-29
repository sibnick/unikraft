/* SPDX-License-Identifier: ISC */
/*
 * Authors: Dan Williams
 *          Martin Lucina
 *          Ricardo Koller
 *          Costin Lupu <costin.lupu@cs.pub.ro>
 *
 * Copyright (c) 2015-2017 IBM
 * Copyright (c) 2016-2017 Docker, Inc.
 * Copyright (c) 2018, NEC Europe Ltd., NEC Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/* Taken from solo5 tscclock.c */

/*-
 * Copyright (c) 2014, 2015 Antti Kantee.  All Rights Reserved.
 * Copyright (c) 2015 Martin Lucina.  All Rights Reserved.
 * Modified for solo5 by Ricardo Koller <kollerr@us.ibm.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>

#include <uk/arch/util.h>
#include <uk/arch/x86_64.h>
#include <uk/intctlr.h>
#include <uk/lcpu.h>
#include <uk/pcpuvar.h>
#include <uk/plat/time.h>
#include <uk/timeconv.h>
#include <uk/print.h>
#include <uk/assert.h>
#include <uk/atomic.h>

#define TIMER_CNTR           0x40
#define TIMER_MODE           0x43
#define TIMER_SEL0           0x00
#define TIMER_LATCH          0x00
#define TIMER_RATEGEN        0x04
#define TIMER_ONESHOT        0x08
#define TIMER_16BIT          0x30
#define TIMER_HZ             1193182

#define	RTC_COMMAND          0x70
#define	RTC_DATA             0x71
#define RTC_NMI_DISABLE      (1<<8)
#define RTC_NMI_ENABLE       0
#define	RTC_SEC              0x00
#define	RTC_MIN              0x02
#define	RTC_HOUR             0x04
#define	RTC_DAY              0x07
#define	RTC_MONTH            0x08
#define	RTC_YEAR             0x09
#define	RTC_STATUS_A         0x0a
#define	RTC_UIP              (1<<7)

/*
 * Compile-time check to make sure we don't tick faster than the PIT can go.
 * This is really only a basic sanity check. We'll run into serious issues WAY
 * earlier.
 */
#if TIMER_HZ / CONFIG_HZ < 1
#error Timer tick frequency (CONFIG_HZ) cannot be higher than PIT frequency!
#endif

/* RTC wall time offset at monotonic time base. */
static __u64 rtc_epochoffset;

/*
 * TSC clock specific.
 */

/* Base time values at the last call to tscclock_monotonic(). */
static __u64 time_base;
static __u64 tsc_base;

/* Multiplier for converting TSC ticks to nsecs. (0.32) fixed point. */
static __u32 tsc_mult;

/*
 * Multiplier for converting nsecs to PIT ticks. (1.32) fixed point.
 *
 * Calculated as:
 *
 *     f = UKARCH_NSEC_PER_SEC / TIMER_HZ   (0.31) fixed point.
 *     pit_mult = 1 / f                     (1.32) fixed point.
 */
static const __u32 pit_mult =
	(1ULL << 63) / ((UKARCH_NSEC_PER_SEC << 31) / TIMER_HZ);


/*
 * Read the current i8254 channel 0 tick count.
 */
static unsigned int i8254_gettick(void)
{
	__u16 rdval;

	uk_arch_x86_64_outb(TIMER_MODE, TIMER_SEL0 | TIMER_LATCH);
	rdval  = uk_arch_x86_64_inb(TIMER_CNTR);
	rdval |= (uk_arch_x86_64_inb(TIMER_CNTR) << 8);
	return rdval;
}

/*
 * Delay for approximately n microseconds using the i8254 channel 0 counter.
 * Timer must be programmed appropriately before calling this function.
 */
static void i8254_delay(unsigned int n)
{
	unsigned int cur_tick, initial_tick;
	int remaining;
	const unsigned long timer_rval = TIMER_HZ / CONFIG_HZ;

	initial_tick = i8254_gettick();

	remaining = (unsigned long long) n * TIMER_HZ / 1000000;

	while (remaining > 1) {
		cur_tick = i8254_gettick();
		if (cur_tick > initial_tick)
			remaining -= timer_rval - (cur_tick - initial_tick);
		else
			remaining -= initial_tick - cur_tick;
		initial_tick = cur_tick;
	}
}

/*
 * Read a RTC register. Due to PC platform braindead-ness also disables NMI.
 */
static inline __u8 rtc_read(__u8 reg)
{
	uk_arch_x86_64_outb(RTC_COMMAND, reg | RTC_NMI_DISABLE);
	return uk_arch_x86_64_inb(RTC_DATA);
}

/*
 * Return current RTC time. Note that due to waiting for the update cycle to
 * complete, this call may take some time.
 */
static __u64 rtc_gettimeofday(void)
{
	struct uktimeconv_bmkclock dt;
	unsigned long flags;

	flags = uk_lcpu_save_irqf();

	/*
	 * If RTC_UIP is down, we have at least 244us to obtain a
	 * consistent reading before an update can occur.
	 */
	while (rtc_read(RTC_STATUS_A) & RTC_UIP)
		continue;

	dt.dt_sec = uktimeconv_bcdtobin(rtc_read(RTC_SEC));
	dt.dt_min = uktimeconv_bcdtobin(rtc_read(RTC_MIN));
	dt.dt_hour = uktimeconv_bcdtobin(rtc_read(RTC_HOUR));
	dt.dt_day = uktimeconv_bcdtobin(rtc_read(RTC_DAY));
	dt.dt_mon = uktimeconv_bcdtobin(rtc_read(RTC_MONTH));
	dt.dt_year = uktimeconv_bcdtobin(rtc_read(RTC_YEAR)) + 2000;

	uk_lcpu_restore_irqf(flags);

	return uktimeconv_bmkclock_to_nsec(&dt);
}

/*
 * Return monotonic time using TSC clock.
 */
__u64 tscclock_monotonic(void)
{
	__u64 tsc_now, tsc_delta;

	/*
	 * Update time_base (monotonic time) and tsc_base (TSC time).
	 */
	tsc_now = uk_arch_x86_64_rdtsc();
	tsc_delta = tsc_now - tsc_base;
	if (tsc_delta >= UINT64_MAX / 2)
		tsc_delta = 1;
	time_base += uk_arch_x86_64_mul64_32(tsc_delta, tsc_mult);
	tsc_base = tsc_now;

	return time_base;
}

/*
 * Calibrate TSC and initialise TSC clock.
 */
int tscclock_init(void)
{
	__u64 tsc_freq = 0, rtc_boot;
	__u32 eax, ebx, ecx, edx;

	/* Initialise i8254 timer channel 0 to mode 2 at CONFIG_HZ frequency */
	uk_arch_x86_64_outb(TIMER_MODE,
			    TIMER_SEL0 | TIMER_RATEGEN | TIMER_16BIT);
	uk_arch_x86_64_outb(TIMER_CNTR, (TIMER_HZ / CONFIG_HZ) & 0xff);
	uk_arch_x86_64_outb(TIMER_CNTR, (TIMER_HZ / CONFIG_HZ) >> 8);

	/*
	 * Read RTC "time at boot". This must be done just before tsc_base is
	 * initialised in order to get a correct offset below.
	 */
	rtc_boot = rtc_gettimeofday();

	/*
	 * Attempt to retrieve TSC frequency via the hypervisor generic cpuid
	 * timing information leaf. 0x40000010 returns the (virtual) TSC
	 * frequency in kHz, or 0 if the feature is not supported by the
	 * hypervisor.
	 */
	uk_arch_x86_64_cpuid(0x40000000, 0, &eax, &ebx, &ecx, &edx);
	if (eax >= 0x40000010) {
		uk_pr_info("Retrieving TSC clock frequency from hypervisor\n");
		tsc_base = uk_arch_x86_64_rdtsc();
		uk_arch_x86_64_cpuid(0x40000010, 0, &eax, &ebx, &ecx, &edx);
		tsc_freq = eax * 1000;
	}

	/*
	 * If we could not retrieve the TSC frequency from the hypervisor,
	 * calibrate against an 0.1s delay using the i8254 timer. This is
	 * undesirable as it delays the boot sequence.
	 */
	if (!tsc_freq) {
		uk_pr_info("Calibrating TSC clock against i8254 timer\n");
		tsc_base = uk_arch_x86_64_rdtsc();
		i8254_delay(100000);
		tsc_freq = (uk_arch_x86_64_rdtsc() - tsc_base) * 10;
	}

	/*
	 * Calculate TSC scaling multiplier.
	 *
	 * (0.32) tsc_mult = UKARCH_NSEC_PER_SEC (32.32) / tsc_freq (32.0)
	 *
	 * Warning, do not print anything between TSC calibration and the
	 * setting of tsc_mult: if CONFIG_LIBUKDEBUG_PRINT_TIME is enabled
	 * this will trigger a reset of tsc_base via tscclock_monotonic
	 * and delay the clock starting point.
	 *
	 * FIXME: this will overflow with small TSC frequencies. We should
	 * probably calculate the TSC shift dynamically like solo5/hvt does.
	 */
	tsc_mult = (UKARCH_NSEC_PER_SEC << 32) / tsc_freq;

	uk_pr_info("Clock source: TSC, frequency estimate is %llu Hz\n",
		   (unsigned long long) tsc_freq);

	/*
	 * Monotonic time begins at tsc_base (first read of TSC before
	 * calibration).
	 */
	tscclock_monotonic();

	/*
	 * Compute RTC epoch offset by subtracting monotonic time_base from RTC
	 * time at boot.
	 */
	rtc_epochoffset = rtc_boot - time_base;

	/*
	 * Initialise i8254 timer channel 0 to mode 4 (one shot).
	 */
	uk_arch_x86_64_outb(TIMER_MODE,
			    TIMER_SEL0 | TIMER_ONESHOT | TIMER_16BIT);
	uk_arch_x86_64_outb(TIMER_CNTR, 0);
	uk_arch_x86_64_outb(TIMER_CNTR, 0);

	return 0;
}

/*
 * Return epoch offset (wall time offset to monotonic clock start).
 */
__u64 tscclock_epochoffset(void)
{
	return rtc_epochoffset;
}

/*
 * Minimum delta to sleep using PIT. Programming seems to have an overhead of
 * 3-4us, but play it safe here.
 */
#define PIT_MIN_DELTA	16

/*
 * Per-vCPU LAPIC one-shot wake. [Ticket a5b91e7993]
 *
 * The i8254 one-shot reaches only vCPU 0. Its output is wired to
 * the 8259, and the hypervisor routes that interrupt to the boot
 * CPU. A halted secondary core then has no wake source, so its
 * device queue goes unpolled. The local APIC timer is per-vCPU.
 * Arm it in one-shot mode for the sleep, so each core is woken by
 * its own timer.
 *
 * The platform runs the local APIC in xAPIC (MMIO) mode: the boot
 * code forces that mode in except.c, in the xpic driver, and in
 * lcpu.c, because the x2APIC TSC-deadline timer raises #GP on this
 * hypervisor. The one-shot LVT path is a different code path. The
 * prepare step below prints the APIC base MSR (bit 10 shows the
 * mode) and probes the one-shot path once per core: it arms a
 * masked one-shot and checks that the current-count register runs
 * down. If the probe fails, the code keeps the i8254 arm.
 */

#define APIC_LVT_TIMER_REG     0x320
#define APIC_TIMER_ICR_REG     0x380
#define APIC_TIMER_CCR_REG     0x390
#define APIC_TIMER_DCR_REG     0x3E0
#define APIC_LVT_MASKED        (1u << 16)
/* Timer mode bits 18:17: 00 one-shot, 01 TSC-deadline, 10 periodic. */
#define APIC_LVT_TIMER_ONESHOT 0u
/* The timer vector sits at irq 16, outside the allocatable pool. */
#define APIC_TIMER_IRQ         16
#define APIC_TIMER_VECTOR      (32 + APIC_TIMER_IRQ)
/* Divide configuration: divide by one (P6 and later encoding). */
#define APIC_TIMER_DIV1        0xB

/* Probe and calibration windows, ns. */
#define LAPIC_PROBE_NS         1000000ULL
#define LAPIC_CAL_NS           1000000ULL

/*
 * A longer sleep uses the maximum one-shot count; the caller loop
 * re-arms for the rest. The cap also keeps the delta times the
 * multiplier product inside 64 bits.
 */
#define LAPIC_MAX_DELTA_NS     200000000ULL

/* Do not halt for a delta below about 2 us. */
#define LAPIC_MIN_DELTA_NS     2000ULL

#define LAPIC_MAX_CPUS         CONFIG_UKPLAT_CPU_MAXCOUNT

/* Per-CPU state, indexed by the logical CPU index. */
static __u64 lapic_mmio_base[LAPIC_MAX_CPUS];
/* Ticks per ns, 32 fractional bits. Valid when lapic_ready is set. */
static __u64 lapic_mult[LAPIC_MAX_CPUS];
static unsigned int lapic_tried[LAPIC_MAX_CPUS];
static unsigned int lapic_ready[LAPIC_MAX_CPUS];
static unsigned int lapic_timer_irq_registered;

static inline __u32 lapic_reg_read(__u64 base, unsigned int off)
{
	return *(volatile __u32 *)((volatile __u8 *)base + off);
}

static inline void lapic_reg_write(__u64 base, unsigned int off, __u32 val)
{
	*(volatile __u32 *)((volatile __u8 *)base + off) = val;
}

/*
 * Handler for the per-vCPU timer vector. The wake is the interrupt
 * that ends the halt. The xpic dispatch writes the APIC EOI after
 * this handler returns.
 */
static int lapic_timer_handler(void *arg __unused)
{
	return 1;
}

/*
 * Detect the APIC mode, probe the one-shot path, and measure the
 * tick rate. Runs once per core, with interrupts disabled, from
 * the first halt of that core.
 */
static void tscclock_lapic_prepare(void)
{
	unsigned int id = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	__u32 lo = 0, hi = 0;
	__u64 base;
	__u32 c0, c1;
	__nsec t0, t1;

	if (id >= LAPIC_MAX_CPUS)
		return;
	if (lapic_tried[id])
		return;
	lapic_tried[id] = 1;

	uk_arch_x86_64_rdmsr(UK_ARCH_X86_64_APIC_MSR_BASE, &lo, &hi);
	uk_pr_info("kvm: cpu %u APIC base MSR 0x%08x%08x: EN=%u EXTD(x2apic)=%u\n",
		   id, hi, lo, (lo >> 11) & 1u, (lo >> 10) & 1u);

	if (!(lo & UK_ARCH_X86_64_APIC_BASE_EN)) {
		uk_pr_warn("kvm: cpu %u local APIC not enabled; using i8254 wake\n",
			   id);
		return;
	}
	if (lo & UK_ARCH_X86_64_APIC_BASE_EXTD) {
		uk_pr_warn("kvm: cpu %u local APIC in x2APIC mode; one-shot probe not attempted; using i8254 wake\n",
			   id);
		return;
	}

	base = (((__u64)hi << 32) | (__u64)lo) & 0xFFFFF000UL;
	if (!base)
		base = 0xFEE00000UL;
	lapic_mmio_base[id] = base;

	/*
	 * Probe: arm a masked one-shot with the maximum count. A
	 * masked timer still counts down. If the current-count
	 * register moves, the one-shot path is emulated.
	 */
	lapic_reg_write(base, APIC_TIMER_DCR_REG, APIC_TIMER_DIV1);
	lapic_reg_write(base, APIC_LVT_TIMER_REG,
			APIC_LVT_MASKED | APIC_LVT_TIMER_ONESHOT |
			APIC_TIMER_VECTOR);
	lapic_reg_write(base, APIC_TIMER_ICR_REG, 0xFFFFFFFFu);

	t0 = ukplat_monotonic_clock();
	while (ukplat_monotonic_clock() - t0 < LAPIC_PROBE_NS)
		;
	c0 = lapic_reg_read(base, APIC_TIMER_CCR_REG);
	lapic_reg_write(base, APIC_TIMER_ICR_REG, 0);

	if (c0 == 0xFFFFFFFFu) {
		uk_pr_warn("kvm: cpu %u LAPIC one-shot probe failed: counter did not move; using i8254 wake\n",
			   id);
		return;
	}

	/* Calibration: sample the running counter over a short window. */
	lapic_reg_write(base, APIC_LVT_TIMER_REG,
			APIC_LVT_MASKED | APIC_LVT_TIMER_ONESHOT |
			APIC_TIMER_VECTOR);
	lapic_reg_write(base, APIC_TIMER_ICR_REG, 0xFFFFFFFFu);
	c0 = lapic_reg_read(base, APIC_TIMER_CCR_REG);
	t0 = ukplat_monotonic_clock();
	while (ukplat_monotonic_clock() - t0 < LAPIC_CAL_NS)
		;
	c1 = lapic_reg_read(base, APIC_TIMER_CCR_REG);
	t1 = ukplat_monotonic_clock();
	lapic_reg_write(base, APIC_TIMER_ICR_REG, 0);

	if (c0 <= c1 || t1 <= t0) {
		uk_pr_warn("kvm: cpu %u LAPIC one-shot calibration failed; using i8254 wake\n",
			   id);
		return;
	}

	lapic_mult[id] = ((__u64)(c0 - c1) << 32) / (__u64)(t1 - t0);

	if (!lapic_timer_irq_registered) {
		if (uk_intctlr_irq_register(APIC_TIMER_IRQ,
					   lapic_timer_handler, __NULL) < 0) {
			uk_pr_warn("kvm: cpu %u failed to register timer irq %u; using i8254 wake\n",
				   id, APIC_TIMER_IRQ);
			return;
		}
		lapic_timer_irq_registered = 1;
	}

	lapic_ready[id] = 1;
	uk_pr_info("kvm: cpu %u LAPIC one-shot wake ready: %u ticks/us\n",
		   id, (unsigned int)((lapic_mult[id] * 1000ULL) >> 32));
}

/*
 * Halt this core until the deadline, armed on its own LAPIC
 * one-shot. Returns after any interrupt, like the i8254 path.
 */
static void tscclock_cpu_block_lapic(unsigned int id, __u64 delta_ns)
{
	__u64 ticks;

	if (delta_ns < LAPIC_MIN_DELTA_NS) {
		/*
		 * Too short to program. Spin with interrupts briefly
		 * enabled, as the i8254 path does.
		 */
		uk_lcpu_enable_irq();
		uk_arch_x86_64_nop(); /* ints are enabled 1 instr after sti */
		uk_lcpu_disable_irq();
		return;
	}

	if (delta_ns > LAPIC_MAX_DELTA_NS)
		ticks = 0xFFFFFFFFULL;
	else
		ticks = (delta_ns * lapic_mult[id]) >> 32;
	if (ticks == 0)
		ticks = 1;
	if (ticks > 0xFFFFFFFFULL)
		ticks = 0xFFFFFFFFULL;

	lapic_reg_write(lapic_mmio_base[id], APIC_LVT_TIMER_REG,
			APIC_LVT_TIMER_ONESHOT | APIC_TIMER_VECTOR);
	lapic_reg_write(lapic_mmio_base[id], APIC_TIMER_ICR_REG,
			(__u32)ticks);

	uk_lcpu_halt_irq();
}

/*
 * Returns early if any interrupts are serviced, or if the requested delay is
 * too short. Must be called with interrupts disabled, will enable interrupts
 * "atomically" during idle loop.
 *
 * This function must be called only from the scheduler. It will screw
 * your system if you do otherwise. And, there is no reason you
 * actually want to use it anywhere else. THIS IS NOT A YIELD or any
 * kind of mutex_lock. It will simply halt the cpu, not allowing any
 * other thread to execute.
 */
static void tscclock_cpu_block(__u64 until)
{
	__u64 now, delta_ns;
	__u64 delta_ticks;
	unsigned int ticks;
	unsigned int id;

	UK_ASSERT(uk_lcpu_irqs_disabled());

	now = ukplat_monotonic_clock();
	delta_ns = until - now;

	/*
	 * Prefer the per-vCPU LAPIC one-shot: it wakes this core.
	 * The i8254 path below reaches only vCPU 0. The prepare
	 * step runs once per core and probes the one-shot path;
	 * if it fails, this core keeps the i8254 arm.
	 * [Ticket a5b91e7993]
	 */
	id = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	if (id < LAPIC_MAX_CPUS) {
		if (!lapic_tried[id])
			tscclock_lapic_prepare();
		if (lapic_ready[id]) {
			tscclock_cpu_block_lapic(id, delta_ns);
			return;
		}
	}

	/*
	 * Compute delta in PIT ticks. Return if it is less than minimum safe
	 * amount of ticks.  Essentially this will cause us to spin until
	 * the timeout.
	 */
	delta_ticks = uk_arch_x86_64_mul64_32(delta_ns, pit_mult);
	if (delta_ticks < PIT_MIN_DELTA) {
		/*
		 * Since we are "spinning", quickly enable interrupts in
		 * the hopes that we might get new work and can do something
		 * else than spin.
		 */
		uk_lcpu_enable_irq();
		uk_arch_x86_64_nop(); /* ints are enabled 1 instr after sti */
		uk_lcpu_disable_irq();
		return;
	}

	/*
	 * Program the timer to interrupt the CPU after the delay has expired.
	 * Maximum timer delay is 65535 ticks.
	 */
	if (delta_ticks > 65535)
		ticks = 65535;
	else
		ticks = delta_ticks;

	/*
	 * Note that according to the Intel 82C54 datasheet, p12 the
	 * interrupt is actually delivered in N + 1 ticks.
	 */
	ticks -= 1;
	uk_arch_x86_64_outb(TIMER_CNTR, ticks & 0xff);
	uk_arch_x86_64_outb(TIMER_CNTR, ticks >> 8);

	/*
	 * Wait for any interrupt. If we got an interrupt then just
	 * return into the scheduler (this func is called _ONLY_ from
	 * a scheduler, see the note above) which will check if there
	 * is work to do and call us again here if not.
	 *
	 * TODO: It would be more efficient for longer sleeps to be
	 * able to distinguish if the interrupt was the PIT interrupt
	 * and no other, but this will do for now.
	 */
	uk_lcpu_halt_irq();
}

unsigned long sched_have_pending_events;

/*
 * Per-vCPU early-wake flags set by the xpic IRQ dispatch for any
 * non-timer interrupt. The shared sched_have_pending_events flag is
 * one slot for all CPUs, so another core's halting loop can consume
 * it before the core that the interrupt targeted wakes. The per-CPU
 * flag fixes that: a device interrupt cuts short only the sleep of
 * the core it targets.
 */
extern unsigned long
	uk_intctlr_pending_events[CONFIG_UKPLAT_CPU_MAXCOUNT];

void time_block_until(__snsec until)
{
	__u64 cpu_idx = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);

	if (unlikely(cpu_idx >= CONFIG_UKPLAT_CPU_MAXCOUNT)) {
		UK_ASSERT(cpu_idx < CONFIG_UKPLAT_CPU_MAXCOUNT);
		return;
	}

	while ((__snsec) ukplat_monotonic_clock() < until) {
		/*
		 * Check for pending events before halting. An event
		 * that arrived after the caller's last poll but
		 * before this point would otherwise be missed: the
		 * flag is already set, no new interrupt will come,
		 * and the halt would run to the timer.
		 */
		if (uk_and_relax(&sched_have_pending_events, 0))
			break;
		if (uk_and_relax(&uk_intctlr_pending_events[cpu_idx], 0))
			break;

		tscclock_cpu_block(until);

		if (uk_and_relax(&sched_have_pending_events, 0))
			break;
		if (uk_and_relax(&uk_intctlr_pending_events[cpu_idx], 0))
			break;
	}
}

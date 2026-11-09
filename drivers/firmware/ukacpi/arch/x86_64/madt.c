/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2022, Karlsruhe Institute of Technology (KIT)
 *                     All rights reserved.
 * Copyright (c) 2022, University POLITEHNICA of Bucharest.
 *                     All rights reserved.
 * Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <uk/acpi.h>
#include <uk/acpi/prio.h>
#include <uk/arch/x86_64.h>
#include <uk/assert.h>
#include <uk/pcpuvar.h>
#include <uk/print.h>
#include <uk/prio.h>

#if CONFIG_LIBUKBOOT
#include <uk/boot/earlytab.h>
#include <uk/plat/common/bootinfo.h>
#endif /* CONFIG_LIBUKBOOT */

/*
 * CPUs that the platform presents, clamped to the configured maximum.
 * Code that sizes per-core resources reads this instead of guessing
 * from CONFIG_UKPLAT_CPU_MAXCOUNT. [Ticket 9a037affad]
 */
unsigned int uk_acpi_present_cpu_count = CONFIG_UKPLAT_CPU_MAXCOUNT;

static unsigned int madt_detect_cpuid_cpu_count(void)
{
	__u32 a = 0, b = 0, c = 0, d = 0;
	unsigned int max_leaf;
	unsigned int count = 0;

	uk_arch_x86_64_cpuid(0, 0, &a, &b, &c, &d);
	max_leaf = a;

	if (max_leaf >= 0x1f) {
		unsigned int sub = 0;

		while (1) {
			uk_arch_x86_64_cpuid(0x1f, sub, &a, &b, &c, &d);
			if (((c >> 8) & 0xff) == 0)
				break;
			count = b & 0xffff;
			sub++;
		}
	}

	if (!count && max_leaf >= 0xb) {
		unsigned int sub = 0;

		while (1) {
			uk_arch_x86_64_cpuid(0xb, sub, &a, &b, &c, &d);
			if (((c >> 8) & 0xff) == 0)
				break;
			count = b & 0xffff;
			sub++;
		}
	}

	if (!count && max_leaf >= 1) {
		uk_arch_x86_64_cpuid(1, 0, &a, &b, &c, &d);
		if (d & (1u << 28))
			count = (b >> 16) & 0xff;
	}

	if (!count)
		count = 1;

	return count;
}

int uk_acpi_madt_fill_cpu_idmap(void)
{
	__u32 a = 0, b = 0, c = 0, d = 0;
	uk_arch_x86_64_cpuid(1, 0, &a, &b, &c, &d);
	__u64 bsp_cpu_id = (b >> 24) & 0xff;
	uk_pcpuvar_lval(0, uk_pcpuvar_cpu_id) = bsp_cpu_id;
	uk_pcpuvar_lval(0, uk_pcpuvar_cpu_idx) = 0;
	union {
		struct uk_acpi_madt_x2apic *x2apic;
		struct uk_acpi_madt_lapic *lapic;
		struct uk_acpi_subsdt_hdr *h;
	} m;
	int bsp_found __maybe_unused = 0;
	struct uk_acpi_madt *madt;
	__sz off, len;
	__u64 cpu_id;
	__u32 idx;

	uk_pr_info("Bootstrapping processor has the ID %ld\n", bsp_cpu_id);

	/* Enumerate all other CPUs */
	madt = uk_acpi_get_madt();
	if (unlikely(!madt)) {
		/*
		 * UEFI boot loaders (such as GRUB in EFI mode) do not
		 * publish the legacy ACPI RSDP. Without a RSDP there is
		 * no MADT. Query the CPU count via CPUID leaves 0x1f,
		 * 0xb, or 0x1. Clamp the count to CONFIG_UKPLAT_CPU_MAXCOUNT.
		 * On KVM hypervisors APIC IDs are contiguous. Set up an
		 * identity map for present CPUs. [Ticket b1326f22d2]
		 */
		unsigned int cpu_cnt = madt_detect_cpuid_cpu_count();
		unsigned int cur_idx = 1;

		if (cpu_cnt > CONFIG_UKPLAT_CPU_MAXCOUNT)
			cpu_cnt = CONFIG_UKPLAT_CPU_MAXCOUNT;

		uk_acpi_present_cpu_count = cpu_cnt;

		uk_pr_warn("No MADT present: detected %u CPU(s) via CPUID; using identity CPU id map\n",
			   cpu_cnt);

		for (__u64 apic_id = 0; apic_id < cpu_cnt && cur_idx < cpu_cnt; apic_id++) {
			if (apic_id == bsp_cpu_id)
				continue;
			uk_pcpuvar_lval(cur_idx, uk_pcpuvar_cpu_id) = apic_id;
			uk_pcpuvar_lval(cur_idx, uk_pcpuvar_cpu_idx) = cur_idx;
			cur_idx++;
		}

		return 0;
	}

	len = madt->hdr.tab_len - sizeof(*madt);
	idx = 1;
	for (off = 0; off < len; off += m.h->len) {
		m.h = (struct uk_acpi_subsdt_hdr *)(madt->entries + off);

		switch (m.h->type) {
		case UK_ACPI_MADT_LAPIC:
			if (!(m.lapic->flags & UK_ACPI_MADT_LAPIC_FLAGS_EN) &&
			    !(m.lapic->flags & UK_ACPI_MADT_LAPIC_FLAGS_ON_CAP))
				continue; /* goto next MADT entry */

			cpu_id = m.lapic->lapic_id;
			break;

		case UK_ACPI_MADT_LX2APIC:
			if (!(m.x2apic->flags & UK_ACPI_MADT_X2APIC_FLAGS_EN) &&
			    !(m.x2apic->flags &
			      UK_ACPI_MADT_X2APIC_FLAGS_ON_CAP))
				continue; /* goto next MADT entry */

			cpu_id = m.x2apic->lapic_id;
			break;

		default:
			continue; /* goto next MADT entry */
		}

		if (bsp_cpu_id == cpu_id) {
			UK_ASSERT(!bsp_found);

			bsp_found = 1;
			continue;
		}

		uk_pcpuvar_lval(idx, uk_pcpuvar_cpu_id) = cpu_id;
		uk_pcpuvar_lval(idx, uk_pcpuvar_cpu_idx) = idx;
		idx++;

		/* Ignore cores that exceed max configured value */
		if (unlikely(idx == CONFIG_UKPLAT_CPU_MAXCOUNT)) {
			uk_pr_info("Configured maximum number of cores (%u) reached.\n", CONFIG_UKPLAT_CPU_MAXCOUNT);
			break;
		}
	}
	UK_ASSERT(bsp_found);

	return 0;
}

#if CONFIG_LIBUKBOOT
static int boot_acpi_madt_fill_cpu_idmap(struct ukplat_bootinfo *bi __unused)
{
	return uk_acpi_madt_fill_cpu_idmap();
}

UK_BOOT_EARLYTAB_ENTRY(boot_acpi_madt_fill_cpu_idmap,
		       UK_PRIO_AFTER(UK_ACPI_INIT_PRIO));
#endif /* CONFIG_LIBUKBOOT */

/* TODO: SPDX Header */
/*
 * Authors: Simon Kuenzer <simon.kuenzer@neclab.eu>
 *          Hugo Lefeuvre <hugo.lefeuvre@neclab.eu>
 *
 * Copyright (c) 2018, NEC Europe Ltd., NEC Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/* Some code was derived from Solo5: */
/*
 * Copyright (c) 2015-2017 Contributors as noted in the AUTHORS file
 *
 * This file is part of Solo5, a unikernel base layer.
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

#include <string.h>

#include <uk/arch/util.h>
#include <uk/atomic.h>
#include <uk/lcpu.h>
#include <uk/print.h>
#include <uk/bus/pci.h>

#define PCI_CONF_READ(type, ret, a, s)					\
	do {								\
		__u32 _conf_data;					\
		uk_arch_x86_64_outl(PCI_CONFIG_ADDR, (a) | PCI_CONF_##s);\
		_conf_data = ((uk_arch_x86_64_inl(PCI_CONFIG_DATA) >>	\
			       PCI_CONF_##s##_SHFT) & PCI_CONF_##s##_MASK);\
		*(ret) = (type) _conf_data;				\
	} while (0)

static inline int pci_driver_add_device(struct pci_driver *drv,
					struct pci_address *addr,
					struct pci_device_id *devid)
{
	struct pci_device *dev;
	__u32 config_addr;
	int ret;

	UK_ASSERT(drv != NULL);
	UK_ASSERT(drv->add_dev != NULL);
	UK_ASSERT(addr != NULL);
	UK_ASSERT(devid != NULL);

	dev = (struct pci_device *) uk_calloc(ph.a, 1, sizeof(*dev));
	if (!dev) {
		uk_pr_err("PCI %02x:%02x.%02x: Failed to initialize: Out of memory!\n",
			  (int) addr->bus,
			  (int) addr->devid,
			  (int) addr->function);
		return -ENOMEM;
	}

	memcpy(&dev->id,   devid, sizeof(dev->id));
	memcpy(&dev->addr, addr,  sizeof(dev->addr));
	dev->drv = drv;

	config_addr = (PCI_ENABLE_BIT)
			| (addr->bus << PCI_BUS_SHIFT)
			| (addr->devid << PCI_DEVICE_SHIFT);
	PCI_CONF_READ(__u16, &dev->base, config_addr, IOBAR);
	PCI_CONF_READ(__u8, &dev->irq, config_addr, IRQ);

	ret = drv->add_dev(dev);
	if (ret < 0) {
		uk_pr_err("PCI %02x:%02x.%02x: Failed to initialize device driver\n",
			  (int) addr->bus,
			  (int) addr->devid,
			  (int) addr->function);
		uk_free(ph.a, dev);
	}
	return 0;
}

static void probe_bus(__u32);

/* Probe a function. Return 1 if the function does not exist in the device, 0
 * otherwise.
 */
static int probe_function(__u32 bus, __u32 device, __u32 function)
{
	__u32 config_addr, config_data, secondary_bus;
	struct pci_address addr;
	struct pci_device_id devid;
	struct pci_driver *drv;

	config_addr = (PCI_ENABLE_BIT)
			| (bus << PCI_BUS_SHIFT)
			| (device << PCI_DEVICE_SHIFT)
			| (function << PCI_FUNCTION_SHIFT);

	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, config_addr);
	config_data = uk_arch_x86_64_inl(PCI_CONFIG_DATA);

	devid.vendor_id = config_data & PCI_DEVICE_ID_MASK;
	if (devid.vendor_id == PCI_INVALID_ID) {
		/* Device doesn't exist */
		return 1;
	}

	addr.domain   = 0x0;
	addr.bus      = bus;
	addr.devid    = device;
	addr.function = function;

	/* These I/O reads could be batched, but it practice this does not
	 * appear to make the code more performant.
	 */
	PCI_CONF_READ(__u32, &devid.class_id,
			config_addr, CLASS_ID);
	PCI_CONF_READ(__u32, &devid.sub_class_id,
			config_addr, SUBCLASS_ID);
	PCI_CONF_READ(__u16, &devid.vendor_id,
			config_addr, VENDOR_ID);
	PCI_CONF_READ(__u16, &devid.device_id,
			config_addr, DEVICE_ID);
	PCI_CONF_READ(__u16, &devid.subsystem_device_id,
			config_addr, SUBSYS_ID);
	PCI_CONF_READ(__u16, &devid.subsystem_vendor_id,
			config_addr, SUBSYSVEN_ID);

	uk_pr_info("PCI %02x:%02x.%02x (%04x %04x:%04x): ",
		   (int) addr.bus,
		   (int) addr.devid,
		   (int) addr.function,
		   (int) devid.class_id,
		   (int) devid.vendor_id,
		   (int) devid.device_id);

	drv = pci_find_driver(&devid);
	if (!drv) {
		uk_pr_info("<no driver>\n");
	} else {
		uk_pr_info("driver %p\n", drv);
		pci_driver_add_device(drv, &addr, &devid);
	}

	/* 0x06 = Bridge Device, 0x04 = PCI-to-PCI bridge */
	if ((devid.class_id == 0x06) && (devid.sub_class_id == 0x04)) {
		PCI_CONF_READ(__u32, &secondary_bus,
				config_addr, SECONDARY_BUS);
		probe_bus(secondary_bus);
	}

	return 0;
}

/* Recursive PCI enumeration: this function is called recursively by
 * probe_function upon discovering PCI-to-PCI bridges.
 */
static void probe_bus(__u32 bus)
{
	__u32 config_addr, device, header_type, function = 0;

	for (device = 0; device < PCI_MAX_DEVICES; ++device) {
		if (!probe_function(bus, device, function))
			continue;

		config_addr = (PCI_ENABLE_BIT);
		PCI_CONF_READ(__u32, &header_type,
				config_addr, HEADER_TYPE);

		/* Is this a multi-function device? */
		if ((header_type & PCI_HEADER_TYPE_MSB_MASK) == 0)
			continue;

		/* Check remaining functions */
		for (function = 1; function < PCI_MAX_FUNCTIONS; function++)
			probe_function(bus, device, function);
	}
}

int arch_pci_probe(struct uk_alloc *pha)
{
	__u32 config_addr, function, header_type, vendor_id;

	uk_pr_debug("Probe PCI\n");

	ph.a = pha;

	config_addr = (PCI_ENABLE_BIT);
	PCI_CONF_READ(__u32, &header_type,
			config_addr, HEADER_TYPE);

	if ((header_type & PCI_HEADER_TYPE_MSB_MASK) == 0) {
		/* Single PCI host controller */
		probe_bus(0);
	} else {
		/* Multiple PCI host controllers */
		for (function = 0; function < PCI_MAX_FUNCTIONS; function++) {
			config_addr = (PCI_ENABLE_BIT) |
					(function << PCI_FUNCTION_SHIFT);

			PCI_CONF_READ(__u32, &vendor_id,
					config_addr, VENDOR_ID);

			if (vendor_id == PCI_INVALID_ID)
				break;

			probe_bus(function);
		}
	}

	return 0;
}

/*
 * Build the CF8 address for one function and one 4-byte aligned
 * configuration offset.
 */
static inline __u32 pci_conf_addr(const struct pci_address *addr,
				  __u8 offset)
{
	return (__u32)PCI_ENABLE_BIT
		| ((__u32)addr->bus << PCI_BUS_SHIFT)
		| ((__u32)addr->devid << PCI_DEVICE_SHIFT)
		| ((__u32)addr->function << PCI_FUNCTION_SHIFT)
		| ((__u32)offset & 0xfcU);
}

/*
 * The CF8/CFC port pair is one global resource. A config access
 * writes the address to CF8, then reads or writes CFC. On an SMP
 * guest two cores can interleave the two steps and hit the wrong
 * register. Such a stray write can destroy a BAR or the MSI-X
 * control word. One core holds this lock with interrupts off for
 * the whole pair. Interrupts stay off so a handler on the lock
 * holder cannot recurse into these accessors and deadlock.
 * [Ticket 8c30628023]
 */
static unsigned int pci_conf_lock;

static unsigned long pci_conf_lock_get(void)
{
	unsigned long flags = uk_lcpu_save_irqf();
	unsigned int exp = 0;

	while (!uk_compare_exchange_n(&pci_conf_lock, &exp, 1u))
		exp = 0;

	return flags;
}

static void pci_conf_lock_put(unsigned long flags)
{
	uk_exchange_n(&pci_conf_lock, 0u);
	uk_lcpu_restore_irqf(flags);
}

/* Config space access helpers. The caller holds the lock. */
static __u32 pci_conf_read32(const struct pci_address *addr, __u8 offset)
{
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, pci_conf_addr(addr, offset));
	return uk_arch_x86_64_inl(PCI_CONFIG_DATA);
}

static void pci_conf_write32(const struct pci_address *addr, __u8 offset,
			     __u32 value)
{
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, pci_conf_addr(addr, offset));
	uk_arch_x86_64_outl(PCI_CONFIG_DATA, value);
}

__u32 uk_pci_read_config32(const struct pci_address *addr, __u8 offset)
{
	unsigned long flags;
	__u32 value;

	flags = pci_conf_lock_get();
	value = pci_conf_read32(addr, offset);
	pci_conf_lock_put(flags);

	return value;
}

void uk_pci_write_config32(const struct pci_address *addr, __u8 offset,
			   __u32 value)
{
	unsigned long flags;

	flags = pci_conf_lock_get();
	pci_conf_write32(addr, offset, value);
	pci_conf_lock_put(flags);
}

__u64 uk_pci_read_bar(const struct pci_address *addr, __u8 bar)
{
	unsigned long flags;
	__u32 low;
	__u64 base;

	if (bar < PCI_BASE_ADDRESS_0 || bar > PCI_BASE_ADDRESS_5)
		return 0;

	flags = pci_conf_lock_get();
	low = pci_conf_read32(addr, bar);
	if (low & 0x1U) { /* I/O space BAR. The value is a port number. */
		base = (__u64)(low & ~0x3U);
	} else {
		base = (__u64)(low & ~0xfU);
		/* 64-bit memory BAR: the next register holds the high
		 * half. No register follows the last BAR, so a 64-bit
		 * type field there is invalid and the high half stays
		 * zero. [Ticket 8c30628023]
		 */
		if (((low >> 1) & 0x3U) == 0x2U &&
		    bar != PCI_BASE_ADDRESS_5)
			base |= (__u64)pci_conf_read32(addr, bar + 4) << 32;
	}
	pci_conf_lock_put(flags);

	return base;
}

__u32 uk_pci_probe_bar_mask(const struct pci_address *addr, __u8 bar_off)
{
	unsigned long flags;
	__u32 orig;
	__u32 mask;

	if (bar_off < PCI_BASE_ADDRESS_0 || bar_off > PCI_BASE_ADDRESS_5)
		return 0;

	flags = pci_conf_lock_get();
	orig = pci_conf_read32(addr, bar_off);
	pci_conf_write32(addr, bar_off, 0xffffffffU);
	mask = pci_conf_read32(addr, bar_off);
	pci_conf_write32(addr, bar_off, orig);
	pci_conf_lock_put(flags);

	return mask;
}

/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

#ifndef _LINUX_KHO_ABI_PCI_H
#define _LINUX_KHO_ABI_PCI_H

#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/types.h>

/**
 * DOC: PCI File-Lifecycle Bound (FLB) Live Update ABI
 *
 * This header defines the ABI for preserving core PCI state across kexec using
 * Live Update File-Lifecycle Bound (FLB) data.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in the PCI_LUO_FLB_VERSION number.
 */

#define PCI_LUO_FLB_COMPATIBLE "pci"
#define PCI_LUO_FLB_VERSION 2

/**
 * struct pci_dev_ser - Serialized state about a single PCI device.
 *
 * @domain: The device's PCI domain number (segment).
 * @bdf: The device's PCI bus, device, and function number.
 * @refcount: Reference count used by the PCI core to keep track of whether it
 *            is done using a device's struct pci_dev_ser. The value of the
 *            refcount is equal to the number of preserved devices at or below
 *            it in the PCI hierarchy when the struct pci_dev_ser is in use, and
 *            0 otherwise.
 */
struct pci_dev_ser {
	u32 domain;
	u16 bdf;
	u16 refcount;
} __packed;

/**
 * struct pci_ser - PCI Subsystem Live Update State
 *
 * This struct tracks state about all devices that are being preserved across
 * a Live Update for the next kernel. It contains only state owned by the PCI
 * core; the state a driver needs to resume its device is preserved separately
 * by that driver.
 *
 * @version: The version of the "pci" FLB struct. This field must never be
 *           deleted, moved, or resized, as the kernel depends on always being
 *           able to check the struct pci_ser version number.
 * @nr_devices: The number of devices that were preserved.
 * @devices: Physical address of the first KHO block containing pci_dev_ser.
 */
struct pci_ser {
	u32 version;
	u32 nr_devices;
	u64 devices;
} __packed;

/**
 * struct pci_cap_saved_data_ser - Serialized state of one saved capability.
 *
 * @cap_nr: The capability ID.
 * @cap_extended: 1 if @cap_nr is an extended capability ID, 0 otherwise.
 * @reserved: Must be zero.
 * @size: Size of @data in bytes. An entry with a zero @size ends the list.
 * @data: The saved capability registers.
 */
struct pci_cap_saved_data_ser {
	u16 cap_nr;
	u8 cap_extended;
	u8 reserved;
	u32 size;
	u32 data[];
} __packed;

/**
 * struct pci_saved_state_ser - Serialized struct pci_saved_state.
 *
 * Used by drivers to hand the state returned by pci_store_saved_state() to the
 * next kernel, see pci_liveupdate_preserve_saved_state(). The layout is part of
 * the ABI of the driver that embeds it, so any change to it requires a version
 * bump of that driver's file handler compatible string.
 *
 * @config_space: The saved standard configuration header.
 * @cap: The saved capabilities, terminated by an entry with a zero size.
 */
struct pci_saved_state_ser {
	u32 config_space[16];
	struct pci_cap_saved_data_ser cap[];
} __packed;

#endif /* _LINUX_KHO_ABI_PCI_H */

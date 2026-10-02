/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2026, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 * David Matlack <dmatlack@google.com>
 */

#ifndef _LINUX_LIVEUPDATE_ABI_VFIO_PCI_H
#define _LINUX_LIVEUPDATE_ABI_VFIO_PCI_H

#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/types.h>

/**
 * DOC: VFIO PCI Live Update ABI
 *
 * VFIO uses the ABI defined below for preserving device files across a kexec
 * reboot using LUO.
 *
 * Device metadata is serialized into memory which is then handed to the next
 * kernel via KHO.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in the VFIO_PCI_LUO_FH_COMPATIBLE string.
 */

#define VFIO_PCI_LUO_FH_COMPATIBLE "vfio-pci-v2"

/* Size of the config space copies in struct vfio_pci_core_device_ser. */
#define VFIO_PCI_SER_CFG_SPACE_SIZE 4096

/*
 * The device was kept running across the kexec, i.e. it was neither reset nor
 * stopped from doing DMA, and the next kernel must take it over as is.
 */
#define VFIO_PCI_SER_F_RUNNING BIT(0)

/**
 * struct vfio_pci_core_device_ser - Serialized state of a single VFIO PCI
 * device.
 *
 * @domain: The device's PCI domain number (segment).
 * @bdf: The device's PCI bus, device, and function number.
 * @flags: VFIO_PCI_SER_F_* flags.
 * @cfg_size: The size of the device's config space, i.e. the number of valid
 *            bytes in @pci_config_map and @vconfig.
 * @reset_works: 1 if the device could be reset when it was first opened, 0
 *               otherwise.
 * @reserved: Must be zero.
 * @saved_state: The physical address of the struct pci_saved_state_ser holding
 *               the state of the device when it was first opened, or 0 if there
 *               is none.
 * @pci_config_map: The map of the capabilities in the device's config space.
 * @vconfig: The device's virtualized config space, as seen by userspace.
 *
 * All the fields after @flags are only valid if VFIO_PCI_SER_F_RUNNING is set.
 */
struct vfio_pci_core_device_ser {
	u32 domain;
	u16 bdf;
	u16 flags;
	u16 cfg_size;
	u8 reset_works;
	u8 reserved[5];
	u64 saved_state;
	u8 pci_config_map[VFIO_PCI_SER_CFG_SPACE_SIZE];
	u8 vconfig[VFIO_PCI_SER_CFG_SPACE_SIZE];
} __packed;

#endif /* _LINUX_LIVEUPDATE_ABI_VFIO_PCI_H */

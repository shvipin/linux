// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

/**
 * DOC: PCI Live Update
 *
 * The PCI subsystem participates in the Live Update process to enable drivers
 * to preserve their PCI devices across kexec.
 *
 * .. note::
 *    The support for preserving PCI devices across Live Update is currently
 *    *partial* and should be considered *experimental*. It should only be
 *    used by developers working on the implementation for the time being.
 *
 *    To enable the support, enable ``CONFIG_PCI_LIVEUPDATE``.
 *
 * File-Lifecycle-Bound (FLB) Data
 * ===============================
 *
 * PCI device preservation across Live Update is built on top of the Live Update
 * Orchestrator's (LUO) support for file preservation across kexec. Drivers
 * are expected to expose a file to represent a single PCI device and support
 * preservation of that file with ``ioctl(LIVEUPDATE_SESSION_PRESERVE_FD)``.
 * This allows userspace to control the preservation of devices and ensure
 * proper lifecycle management while a device is preserved. The first intended
 * use-case is preserving vfio-pci device files.
 *
 * The PCI core maintains its own state about what devices are being preserved
 * across Live Update using a feature called File-Lifecycle-Bound (FLB) data in
 * LUO.  Essentially, this allows the PCI core to allocate struct pci_ser when
 * the first device (file) is preserved and free it when the last device (file)
 * is unpreserved. After kexec, the PCI core can fetch the struct pci_ser (which
 * was constructed by the previous kernel) from LUO at any time (e.g. during
 * enumeration) so that it knows which devices were preserved.
 *
 * To enable the PCI core to be notified whenever a file representing a device
 * is preserved, drivers must register their struct liveupdate_file_handler with
 * the PCI core by using the following APIs:
 *
 *  * ``pci_liveupdate_register_flb(driver_file_handler)``
 *  * ``pci_liveupdate_unregister_flb(driver_file_handler)``
 *
 * Device Tracking
 * ===============
 *
 * Drivers must notify the PCI core when specific devices are preserved or
 * unpreserved with the following APIs:
 *
 *  * ``pci_liveupdate_preserve(pci_dev)``
 *  * ``pci_liveupdate_unpreserve(pci_dev)``
 *
 * This allows the PCI core to keep it's FLB data (struct pci_ser) up to date
 * with the list of **outgoing** preserved devices for the next kernel.
 *
 * After kexec, whenever a device is enumerated, the PCI core will check if it
 * is an **incoming** preserved device (i.e. preserved by the previous kernel)
 * by checking the incoming FLB data (struct pci_ser).
 *
 * Drivers must notify the PCI core when an **incoming** device is done
 * participating in the incoming Live Update with the following API:
 *
 *  * ``pci_liveupdate_finish(pci_dev)``
 *
 * The PCI core does not enforce any ordering of ``pci_liveupdate_finish()`` and
 * ``pci_liveupdate_preserve()``. i.e. A PCI device can be **outgoing**
 * (preserved for next kernel) and **incoming** (preserved by previous kernel)
 * at the same time.
 *
 * Restrictions
 * ============
 *
 * The PCI core enforces the following restrictions on which devices can be
 * preserved. These may be relaxed in the future:
 *
 *  * The device cannot be a Virtual Function (VF).
 *
 * Driver Binding
 * ==============
 *
 * In the outgoing kernel, it is the driver's responsibility to ensure that it
 * does not release a device between pci_liveupdate_preserve() and
 * pci_liveupdate_unpreserve().
 *
 * In the incoming kernel, it is the driver's responsibility to ensure that it
 * does not release a preserved device between probe() and
 * pci_liveupdate_finish().
 *
 * It is the user's responsibility to ensure that incoming preserved devices are
 * bound to the correct driver. i.e. The PCI core does not protect against a
 * device getting preserved by driver A in the outgoing kernel and then getting
 * bound to driver B in the incoming kernel.
 *
 * BDF Stability
 * =============
 *
 * The PCI core guarantees that incoming preserved devices can be identified by
 * the same bus, device, and function numbers as prior to kexec. To accomplish
 * this, the PCI core always inherits the secondary and subordinate bus numbers
 * assigned to bridges during enumeration, rather than assigning new ones (the
 * PCI core assumes that the previous kernel established a sane topology).
 *
 * If a misconfigured or unconfigured bridge is encountered during enumeration
 * while there are incoming preserved devices, it's secondary and subordinate
 * bus numbers will be cleared and devices below it will not be enumerated.
 *
 * PCI-to-PCI Bridges
 * ==================
 *
 * Any PCI-to-PCI bridges upstream of a preserved device are automatically
 * preserved when the device is preserved. The PCI core keeps track of the
 * number of downstream devices that are preserved under a bridge so that the
 * bridge is only unpreserved once all downstream devices are unpreserved.
 *
 * This enables the PCI core and any drivers bound to the bridge to participate
 * in the Live Update so that preserved endpoints can continue issuing memory
 * transactions during the Live Update.
 *
 * Handling Preserved Devices
 * ==========================
 *
 * The PCI core treats preserved devices differently than non-preserved devices.
 * This section enumerates those differences.
 *
 *  * The PCI core inherits all ACS flags enabled on incoming preserved devices
 *    rather than assigning new ones. This ensures that TLPs are routed the same
 *    way after Live Update and ensures that IOMMU groups do not change.
 *
 *  * The PCI core inherits ARI Forwarding Enable on all bridges with downstream
 *    preserved devices to ensure that all preserved devices on the bridge's
 *    secondary bus are addressable after the Live Update.
 */

#define pr_fmt(fmt) "PCI: liveupdate: " fmt

#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/pci.h>
#include <linux/liveupdate.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/pci.h>

#include "pci.h"

static DEFINE_MUTEX(pci_flb_outgoing_lock);

struct pci_flb_incoming {
	/* The pci_ser struct passed by the previous kernel. */
	struct pci_ser *ser;

	/* xarray used to quickly find a device in ser->devices[] */
	struct xarray xa;
};

static unsigned long pci_ser_xa_key(unsigned long domain, unsigned long bdf)
{
	return domain << 16 | bdf;
}

static int pci_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct pci_dev *dev = NULL;
	u32 max_nr_devices = 0;
	struct pci_ser *ser;
	unsigned long size;

	/*
	 * Allocate enough space to preserve all of the devices that are
	 * currently present on the system. Extra padding can be added to this
	 * in the future to increase the chances that there is enough room to
	 * preserve devices that are not yet present on the system (e.g. VFs,
	 * hot-plugged devices).
	 */
	for_each_pci_dev(dev)
		max_nr_devices++;

	size = struct_size_t(struct pci_ser, devices, max_nr_devices);

	pr_debug("Preserving struct pci_ser with room for %u devices\n",
		 max_nr_devices);

	ser = kho_alloc_preserve(size);
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	ser->max_nr_devices = max_nr_devices;
	ser->nr_devices = 0;

	args->obj = ser;
	args->data = virt_to_phys(ser);
	return 0;
}

static void pci_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	struct pci_ser *ser = args->obj;

	pr_debug("Unpreserving struct pci_ser\n");
	WARN_ON_ONCE(ser->nr_devices);
	kho_unpreserve_free(ser);
}

static int pci_flb_retrieve(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_incoming *incoming;
	int i, ret;

	incoming = kmalloc(sizeof(*incoming), GFP_KERNEL);
	if (!incoming)
		return -ENOMEM;

	incoming->ser = phys_to_virt(args->data);

	xa_init(&incoming->xa);

	for (i = 0; i < incoming->ser->max_nr_devices; i++) {
		struct pci_dev_ser *dev_ser = &incoming->ser->devices[i];
		unsigned long key;

		if (!dev_ser->refcount)
			continue;

		key = pci_ser_xa_key(dev_ser->domain, dev_ser->bdf);
		ret = xa_err(xa_store(&incoming->xa, key, dev_ser, GFP_KERNEL));
		if (ret) {
			xa_destroy(&incoming->xa);
			kfree(incoming);
			return ret;
		}
	}

	args->obj = incoming;
	return 0;
}

static void pci_flb_finish(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_incoming *incoming = args->obj;

	xa_destroy(&incoming->xa);
	kho_restore_free(incoming->ser);
	kfree(incoming);
}

static struct liveupdate_flb_ops pci_liveupdate_flb_ops = {
	.preserve = pci_flb_preserve,
	.unpreserve = pci_flb_unpreserve,
	.retrieve = pci_flb_retrieve,
	.finish = pci_flb_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_flb pci_liveupdate_flb = {
	.ops = &pci_liveupdate_flb_ops,
	.compatible = PCI_LUO_FLB_COMPATIBLE,
};

static int pci_liveupdate_preserve_device(struct pci_ser *ser, struct pci_dev *dev)
{
	int i;

	if (dev->liveupdate_outgoing) {
		dev->liveupdate_outgoing->refcount++;
		return 0;
	}

	if (ser->nr_devices == ser->max_nr_devices)
		return -ENOSPC;

	for (i = 0; i < ser->max_nr_devices; i++) {
		/*
		 * Start searching at index ser->nr_devices. This should result
		 * in a constant time search under expected conditions (devices
		 * are not getting unpreserved).
		 */
		int index = (ser->nr_devices + i) % ser->max_nr_devices;
		struct pci_dev_ser *dev_ser = &ser->devices[index];

		if (dev_ser->refcount)
			continue;

		pci_info(dev, "Device will be preserved across next Live Update\n");
		ser->nr_devices++;

		dev_ser->domain = pci_domain_nr(dev->bus);
		dev_ser->bdf = pci_dev_id(dev);
		dev_ser->refcount = 1;

		dev->liveupdate_outgoing = dev_ser;
		return 0;
	}

	return -ENOSPC;
}

static void pci_liveupdate_unpreserve_path(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev *upstream_bridge = dev->bus->self;
	struct pci_dev_ser *dev_ser;

	if (upstream_bridge)
		pci_liveupdate_unpreserve_path(ser, upstream_bridge);

	dev_ser = dev->liveupdate_outgoing;
	if (!dev_ser) {
		pci_warn(dev, "Cannot unpreserve device that is not preserved\n");
		return;
	}

	if (--dev_ser->refcount == 0) {
		pci_info(dev, "Device will no longer be preserved across next Live Update\n");
		ser->nr_devices--;
		memset(dev_ser, 0, sizeof(*dev_ser));
		dev->liveupdate_outgoing = NULL;
	}
}

static int pci_liveupdate_preserve_path(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev *upstream_bridge = dev->bus->self;
	int ret = 0;

	if (upstream_bridge) {
		ret = pci_liveupdate_preserve_path(ser, upstream_bridge);
		if (ret)
			return ret;
	} else if (!pci_is_root_bus(dev->bus)) {
		pci_err(dev, "Failed to preserve up to root port\n");
		return -EINVAL;
	}

	ret = pci_liveupdate_preserve_device(ser, dev);
	if (ret)
		goto err;

	return 0;

err:
	if (upstream_bridge)
		pci_liveupdate_unpreserve_path(ser, upstream_bridge);

	return ret;
}

int pci_liveupdate_preserve(struct pci_dev *dev)
{
	struct pci_ser *ser;
	int ret;

	guard(mutex)(&pci_flb_outgoing_lock);

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&ser);
	if (ret)
		return ret;

	if (!ser)
		return -ENOENT;

	if (dev->is_virtfn)
		return -EINVAL;

	if (!pci_device_group_immutable_singleton(dev)) {
		pci_warn(dev, "Device preservation limited to immutable singleton iommu groups\n");
		return -EINVAL;
	}

	if (dev->liveupdate_outgoing)
		return -EBUSY;

	return pci_liveupdate_preserve_path(ser, dev);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_preserve);

void pci_liveupdate_unpreserve(struct pci_dev *dev)
{
	struct pci_ser *ser = NULL;
	int ret;

	guard(mutex)(&pci_flb_outgoing_lock);

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&ser);

	if (ret || !ser) {
		pci_warn(dev, "Cannot unpreserve device without outgoing Live Update state\n");
		return;
	}

	pci_liveupdate_unpreserve_path(ser, dev);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_unpreserve);

static struct xarray *pci_liveupdate_flb_get_incoming(void)
{
	struct pci_flb_incoming *incoming;
	int ret;

	ret = liveupdate_flb_get_incoming(&pci_liveupdate_flb, (void **)&incoming);

	/* Live Update is not enabled. */
	if (ret == -EOPNOTSUPP)
		return NULL;

	/* Live Update is enabled, but there is no incoming FLB data. */
	if (ret == -ENODATA)
		return NULL;

	/*
	 * Live Update is enabled and there is incoming FLB data, but none of it
	 * matches pci_liveupdate_flb.compatible.
	 *
	 * This could mean that no PCI FLB data was passed by the previous
	 * kernel, but it could also mean the previous kernel used a different
	 * compatibility string (i.e. a different ABI).
	 */
	if (ret == -ENOENT) {
		pr_info_once("No incoming FLB matched %s\n", pci_liveupdate_flb.compatible);
		return NULL;
	}

	/*
	 * There is incoming FLB data that matches pci_liveupdate_flb.compatible
	 * but it cannot be retrieved.
	 */
	if (ret) {
		WARN_ONCE(ret, "Failed to retrieve incoming FLB data\n");
		return NULL;
	}

	return &incoming->xa;
}

static void pci_liveupdate_flb_put_incoming(void)
{
	liveupdate_flb_put_incoming(&pci_liveupdate_flb);
}

void pci_liveupdate_setup_device(struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser;
	struct xarray *xa;
	unsigned long key;

	xa = pci_liveupdate_flb_get_incoming();
	if (!xa)
		return;

	/*
	 * During a Live Update, preserved devices are allowed to continue
	 * performing memory transactions. The kernel must not change the fabric
	 * topology, including bus numbers, since that would require disabling
	 * and flushing any memory transactions first.
	 *
	 * To keep things simple, inherit the secondary and subordinate bus
	 * numbers on _all_ bridges if _any_ PCI devices were preserved (i.e.
	 * even bridges without any downstream endpoints that were preserved).
	 * This avoids accidentally assigning a bridge a new window that
	 * overlaps with a preserved device that is downstream of a different
	 * bridge.
	 */
	dev->liveupdate_inherit_buses = true;

	key = pci_ser_xa_key(pci_domain_nr(dev->bus), pci_dev_id(dev));
	dev_ser = xa_load(xa, key);

	/* This device was not preserved across Live Update */
	if (!dev_ser) {
		pci_liveupdate_flb_put_incoming();
		return;
	}

	/*
	 * This device was preserved, but has already been probed and gone
	 * through pci_liveupdate_finish(). This can happen if PCI core probes
	 * the same device multiple times, e.g. due to hotplug.
	 */
	if (!dev_ser->refcount) {
		pci_liveupdate_flb_put_incoming();
		return;
	}

	pci_info(dev, "Device was preserved by previous kernel across Live Update\n");

	/*
	 * Hold the ref on the incoming FLB until pci_liveupdate_finish() so
	 * that dev_ser does not get freed while it is in use.
	 */
	dev->liveupdate_incoming = dev_ser;
}

void pci_liveupdate_cleanup_device(struct pci_dev *dev)
{
	/*
	 * Drop the FLB reference acquired in pci_liveupdate_setup_device() if
	 * the device is being cleaned up before pci_liveupdate_finish(), e.g.
	 * due to allocation failure during setup.
	 *
	 * Do not drop dev->liveupdate_incoming->refcount since this device has
	 * not gone through pci_liveupdate_finish() and thus is still an
	 * incoming preserved device.
	 *
	 * Note: This cannot race with pci_liveupdate_finish() since it is only
	 * called in cleanup paths when there are no users of the pci_dev.
	 */
	if (dev->liveupdate_incoming)
		pci_liveupdate_flb_put_incoming();
}

static void pci_liveupdate_finish_path(struct pci_dev *dev)
{
	struct pci_dev *upstream_bridge = dev->bus->self;

	if (upstream_bridge)
		pci_liveupdate_finish_path(upstream_bridge);

	/*
	 * Decrement the refcount so this device does not get treated as an
	 * incoming device again, e.g. in case pci_liveupdate_setup_device()
	 * gets called again becase the device is hot-plugged.
	 */
	if (--dev->liveupdate_incoming->refcount)
		return;

	pci_info(dev, "Device is finished participating in Live Update\n");
	dev->liveupdate_incoming = NULL;
}

void pci_liveupdate_finish(struct pci_dev *dev)
{
	if (!dev->liveupdate_incoming) {
		pci_warn(dev, "Cannot finish preserving an unpreserved device\n");
		return;
	}

	pci_liveupdate_finish_path(dev);

	/* Drop this device's reference on the incoming FLB. */
	pci_liveupdate_flb_put_incoming();
}
EXPORT_SYMBOL_GPL(pci_liveupdate_finish);

int pci_liveupdate_register_flb(struct liveupdate_file_handler *fh)
{
	pr_debug("Registering file handler \"%s\"\n", fh->compatible);
	return liveupdate_register_flb(fh, &pci_liveupdate_flb);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_register_flb);

void pci_liveupdate_unregister_flb(struct liveupdate_file_handler *fh)
{
	pr_debug("Unregistering file handler \"%s\"\n", fh->compatible);
	liveupdate_unregister_flb(fh, &pci_liveupdate_flb);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_unregister_flb);

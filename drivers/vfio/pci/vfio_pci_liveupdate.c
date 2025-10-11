// SPDX-License-Identifier: GPL-2.0

/*
 * Liveupdate support for VFIO devices.
 *
 * Copyright (c) 2025, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 */

#include <linux/liveupdate.h>
#include <linux/vfio.h>
#include <linux/errno.h>
#include <linux/kexec_handover.h>

#include "vfio_pci_priv.h"

struct vfio_pci_core_device_ser {
	u16 bdf;
} __packed;

static int vfio_pci_lu_serialize(struct vfio_pci_core_device *vdev,
				 struct vfio_pci_core_device_ser *ser)
{
	ser->bdf = pci_dev_id(vdev->pdev);
	return 0;
}

static int vfio_pci_liveupdate_prepare(struct liveupdate_file_handler *handler,
				       struct file *file, u64 *data)
{
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;
	struct folio *folio;
	int err;

	device = vfio_device_from_file(file);
	vdev = container_of(device, struct vfio_pci_core_device, vdev);

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, get_order(sizeof(*ser)));
	if (!folio)
		return -ENOMEM;

	ser = folio_address(folio);

	err = vfio_pci_lu_serialize(vdev, ser);
	if (err)
		goto err_free_folio;

	err = kho_preserve_folio(folio);
	if (err)
		goto err_free_folio;

	*data = virt_to_phys(ser);

	return 0;

err_free_folio:
	folio_put(folio);
	return err;
}

static void vfio_pci_liveupdate_cancel(struct liveupdate_file_handler *handler,
				       struct file *file, u64 data)
{
	struct vfio_pci_core_device_ser *ser = phys_to_virt(data);
	struct folio *folio = virt_to_folio(ser);

	WARN_ON_ONCE(kho_unpreserve_folio(folio));
	folio_put(folio);
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_handler *handler,
					u64 data, struct file **file)
{
	return -EOPNOTSUPP;
}

static bool vfio_pci_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					     struct file *file)
{
	struct vfio_device *device = vfio_device_from_file(file);

	if (!device)
		return false;

	guard(mutex)(&device->dev_set->lock);
	return vfio_device_cdev_opened(device);
}

static const struct liveupdate_file_ops vfio_pci_luo_fops = {
	.prepare = vfio_pci_liveupdate_prepare,
	.cancel = vfio_pci_liveupdate_cancel,
	.retrieve = vfio_pci_liveupdate_retrieve,
	.can_preserve = vfio_pci_liveupdate_can_preserve,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler vfio_pci_luo_handler = {
	.ops = &vfio_pci_luo_fops,
	.compatible = "vfio-v1",
};

void __init vfio_pci_liveupdate_init(void)
{
	int err = liveupdate_register_file_handler(&vfio_pci_luo_handler);

	if (err)
		pr_err("VFIO PCI liveupdate file handler register failed, error %d.\n", err);
}

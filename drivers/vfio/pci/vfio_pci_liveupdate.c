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

#include "vfio_pci_priv.h"

static int vfio_pci_liveupdate_prepare(struct liveupdate_file_handler *handler,
				       struct file *file, u64 *data)
{
	return -EOPNOTSUPP;
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

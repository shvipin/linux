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
#include <linux/anon_inodes.h>
#include <linux/kexec_handover.h>
#include <linux/file.h>

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
	vdev->pdev->skip_kexec_clear_master = true;

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
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;

	device = vfio_device_from_file(file);
	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	vdev->pdev->skip_kexec_clear_master = false;
	WARN_ON_ONCE(kho_unpreserve_folio(folio));
	folio_put(folio);
}

static int match_bdf(struct device *device, const void *bdf)
{
	struct vfio_device *core_vdev =
		container_of(device, struct vfio_device, device);
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	return *(u16 *)bdf == pci_dev_id(vdev->pdev);
}

static void vfio_pci_liveupdate_finish(struct liveupdate_file_handler *handler,
				       struct file *file, u64 data, bool reclaimed)
{
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;
	struct folio *folio;

	if (reclaimed) {
		folio = virt_to_folio(phys_to_virt(data));
		goto out_folio_put;
	} else {
		folio = kho_restore_folio(data);
	}

	if (!folio)
		return;

	ser = folio_address(folio);

	device = vfio_find_device_in_cdev_class(&ser->bdf, match_bdf);
	if (!device)
		goto out_folio_put;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pci_try_reset_function(vdev->pdev);
	put_device(&device->device);

out_folio_put:
	folio_put(folio);
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_handler *handler,
					u64 data, struct file **file)
{
	struct vfio_pci_core_device_ser *ser;
	struct vfio_device_file *df;
	struct vfio_device *device;
	struct folio *folio;
	struct file *filep;
	int err;

	folio = kho_restore_folio(data);
	if (!folio)
		return -ENOENT;

	ser = folio_address(folio);
	device = vfio_find_device_in_cdev_class(&ser->bdf, match_bdf);
	if (!device)
		return -ENODEV;

	df = vfio_allocate_device_file(device);
	if (IS_ERR(df)) {
		err = PTR_ERR(df);
		goto err_vfio_device_file;
	}

	filep = anon_inode_getfile_fmode("[vfio-cdev]", &vfio_device_fops, df,
					 O_RDWR, FMODE_PREAD | FMODE_PWRITE);
	if (IS_ERR(filep)) {
		err = PTR_ERR(filep);
		goto err_anon_inode;
	}

	/* Paired with the put in vfio_device_fops_release() */
	if (!vfio_device_try_get_registration(device)) {
		err = -ENODEV;
		goto err_get_registration;
	}

	put_device(&device->device);

	/*
	 * Use the pseudo fs inode on the device to link all mmaps
	 * to the same address space, allowing us to unmap all vmas
	 * associated to this device using unmap_mapping_range().
	 */
	filep->f_mapping = device->inode->i_mapping;
	*file = filep;

	return 0;

err_get_registration:
	fput(filep);
err_anon_inode:
	kfree(df);
err_vfio_device_file:
	put_device(&device->device);
	return err;
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
	.finish = vfio_pci_liveupdate_finish,
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

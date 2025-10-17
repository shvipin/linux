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
#include <linux/pci.h>

#include "vfio_pci_priv.h"

struct pci_cap_saved_data_ser {
	u16		cap_nr;
	bool		cap_extended;
	unsigned int	size;
	u32		data[];
} __packed;

struct pci_saved_state_ser {
	u32 config_space[16];
	struct pci_cap_saved_data_ser cap[];
} __packed;

struct vfio_pci_core_device_ser {
	u16 bdf;
	u32 cfg_size;
	u8 pci_config_map[PCI_CFG_SPACE_EXP_SIZE];
	u8 vconfig[PCI_CFG_SPACE_EXP_SIZE];
	u32 rbar[7];
	u8 reset_works;
	u64 pci_saved_state_phys;
} __packed;

static int vfio_pci_liveupdate_deserialize_config(struct vfio_pci_core_device *vdev,
						  struct vfio_pci_core_device_ser *ser)
{
	struct pci_dev *pdev = vdev->pdev;

	if (WARN_ON_ONCE(pdev->cfg_size != ser->cfg_size)) {
		dev_err(&pdev->dev, "Config size in serialized (%d) not matching the one pci_dev (%d)",
			ser->cfg_size, pdev->cfg_size);
		return -EINVAL;
	}

	memcpy(vdev->pci_config_map, ser->pci_config_map, ser->cfg_size);
	memcpy(vdev->vconfig, ser->vconfig, ser->cfg_size);
	memcpy(vdev->rbar, ser->rbar, sizeof(vdev->rbar));
	return 0;
}

static void vfio_pci_liveupdate_serialize_config(struct vfio_pci_core_device *vdev,
						 struct vfio_pci_core_device_ser *ser)
{
	ser->cfg_size = vdev->pdev->cfg_size;
	memcpy(ser->pci_config_map, vdev->pci_config_map, ser->cfg_size);
	memcpy(ser->vconfig, vdev->vconfig, ser->cfg_size);
	memcpy(ser->rbar, vdev->rbar, sizeof(vdev->rbar));
}

static size_t pci_saved_state_size(struct pci_saved_state *state)
{
	struct pci_cap_saved_data *cap;
	size_t size;

	/* One empty cap to denote end. */
	size = sizeof(struct pci_saved_state) + sizeof(struct pci_cap_saved_data);

	cap = state->cap;
	while (cap->size) {
		size_t len = sizeof(struct pci_cap_saved_data) + cap->size;

		size += len;
		cap = (struct pci_cap_saved_data *)((u8 *)cap + len);
	}

	return size;
}

static size_t pci_saved_state_size_from_ser(struct pci_saved_state_ser *state)
{
	struct pci_cap_saved_data_ser *cap;
	size_t size;

	/* One empty cap to denote end. */
	size = sizeof(struct pci_saved_state) + sizeof(struct pci_cap_saved_data);

	cap = state->cap;
	while (cap->size) {
		size_t len = sizeof(struct pci_cap_saved_data) + cap->size;

		size += len;
		cap = (struct pci_cap_saved_data_ser *)((u8 *)cap + len);
	}

	return size;
}

static void serialize_pci_cap_saved_data(struct pci_saved_state *state,
					 struct pci_saved_state_ser *state_ser)
{
	struct pci_cap_saved_data_ser *cap_ser = state_ser->cap;
	struct pci_cap_saved_data *cap = state->cap;

	while (cap->size) {
		cap_ser->cap_nr = cap->cap_nr;
		cap_ser->cap_extended = cap->cap_extended;
		cap_ser->size = cap->size;
		memcpy(cap_ser->data, cap->data, cap_ser->size);

		cap = (void *)cap + sizeof(*cap) + cap->size;
		cap_ser = (void *)cap_ser + sizeof(*cap_ser) + cap_ser->size;
	}
}

static void deserialize_pci_cap_saved_data(struct pci_saved_state *state,
					   struct pci_saved_state_ser *state_ser)
{
	struct pci_cap_saved_data_ser *cap_ser = state_ser->cap;
	struct pci_cap_saved_data *cap = state->cap;

	while (cap_ser->size) {
		cap->cap_nr = cap_ser->cap_nr;
		cap->cap_extended = cap_ser->cap_extended;
		cap->size = cap_ser->size;
		memcpy(cap->data, cap_ser->data, cap_ser->size);

		cap = (void *)cap + sizeof(*cap) + cap->size;
		cap_ser = (void *)cap_ser + sizeof(*cap_ser) + cap_ser->size;
	}
}

static int serialize_pci_saved_state(struct vfio_pci_core_device *vdev,
				     struct vfio_pci_core_device_ser *ser)
{
	struct pci_saved_state *state = vdev->pci_saved_state;
	struct pci_saved_state_ser *state_ser;
	struct folio *folio;
	size_t size;
	int ret;

	if (!state)
		return 0;

	size = pci_saved_state_size(state);

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, get_order(size));
	if (!folio)
		return -ENOMEM;

	state_ser = folio_address(folio);

	memcpy(state_ser->config_space, state->config_space,
	       sizeof(state_ser->config_space));

	serialize_pci_cap_saved_data(state, state_ser);

	ret = kho_preserve_folio(folio);
	if (ret) {
		folio_put(folio);
		return ret;
	}

	ser->pci_saved_state_phys = virt_to_phys(state_ser);

	return 0;
}

static int deserialize_pci_saved_state(struct vfio_pci_core_device *vdev,
				       struct vfio_pci_core_device_ser *ser)
{
	struct pci_saved_state_ser *state_ser;
	struct pci_saved_state *state;
	size_t size;

	if (!ser->pci_saved_state_phys)
		return 0;

	state_ser = phys_to_virt(ser->pci_saved_state_phys);
	size = pci_saved_state_size_from_ser(state_ser);
	state = kzalloc(size, GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	memcpy(state->config_space, state_ser->config_space,
	       sizeof(state_ser->config_space));

	deserialize_pci_cap_saved_data(state, state_ser);
	vdev->pci_saved_state = state;
	return 0;
}

static int vfio_pci_lu_serialize(struct vfio_pci_core_device *vdev,
				 struct vfio_pci_core_device_ser *ser)
{
	int err;

	ser->bdf = pci_dev_id(vdev->pdev);
	vfio_pci_liveupdate_serialize_config(vdev, ser);
	ser->reset_works = vdev->reset_works;
	err = serialize_pci_saved_state(vdev, ser);
	if (err)
		return err;

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
	struct folio *pci_saved_state_folio;
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;

	device = vfio_device_from_file(file);
	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	vdev->pdev->skip_kexec_clear_master = false;
	if (ser->pci_saved_state_phys) {
		pci_saved_state_folio = virt_to_folio(phys_to_virt(ser->pci_saved_state_phys));
		WARN_ON_ONCE(kho_unpreserve_folio(pci_saved_state_folio));
		folio_put(pci_saved_state_folio);
	}
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

	if (reclaimed)
		folio = virt_to_folio(phys_to_virt(data));
	else
		folio = kho_restore_folio(data);

	if (!folio)
		return;

	ser = folio_address(folio);

	if (!reclaimed && ser->pci_saved_state_phys)
		kho_restore_folio(ser->pci_saved_state_phys);

	device = vfio_find_device_in_cdev_class(&ser->bdf, match_bdf);
	if (!device)
		goto out_folio_put;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	if (reclaimed) {
		guard(mutex)(&device->dev_set->lock);
		if (!vfio_device_cdev_opened(device))
			pci_err(vdev->pdev, "Open count is 0, userspace might not have restored the device.\n");
		vdev->liveupdate_restore = NULL;
	} else {
		pci_try_reset_function(vdev->pdev);
	}
	put_device(&device->device);

out_folio_put:
	if (ser->pci_saved_state_phys)
		folio_put(virt_to_folio(phys_to_virt(ser->pci_saved_state_phys)));
	folio_put(folio);
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_handler *handler,
					u64 data, struct file **file)
{
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	struct vfio_device_file *df;
	struct vfio_device *device;
	struct folio *folio;
	struct file *filep;
	int err;

	folio = kho_restore_folio(data);
	if (!folio)
		return -ENOENT;

	ser = folio_address(folio);
	if (ser->pci_saved_state_phys) {
		if (!kho_restore_folio(ser->pci_saved_state_phys))
			return -ENOENT;
	}

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
	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	guard(mutex)(&device->dev_set->lock);
	vdev->liveupdate_restore = ser;

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

int vfio_pci_liveupdate_restore_config(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_core_device_ser *ser = vdev->liveupdate_restore;

	return vfio_pci_liveupdate_deserialize_config(vdev, ser);
}

int vfio_pci_liveupdate_restore_device(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_core_device_ser *ser = vdev->liveupdate_restore;
	int err;

	err = deserialize_pci_saved_state(vdev, ser);
	if (err)
		return err;

	vdev->reset_works = ser->reset_works;
	return 0;
}

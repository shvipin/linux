// SPDX-License-Identifier: GPL-2.0-only
#include <linux/pci_regs.h>
#include <linux/vfio.h>

#include <libvfio.h>

#include "kselftest_harness.h"

static const char *device_bdf;

FIXTURE(vfio_pci_mmap_reset_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
};

FIXTURE_SETUP(vfio_pci_mmap_reset_test)
{
	self->iommu = iommu_init(MODE_IOMMUFD);
	self->device = vfio_pci_device_init(device_bdf, self->iommu);
}

FIXTURE_TEARDOWN(vfio_pci_mmap_reset_test)
{
	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
}

TEST_F(vfio_pci_mmap_reset_test, mmap_fault_and_reset)
{
	volatile char dummy;
	bool has_mmap = false;
	int i;

	if (!(self->device->info.flags & VFIO_DEVICE_FLAGS_RESET))
		SKIP(return, "Device does not support reset\n");

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		struct vfio_pci_bar *bar = &self->device->bars[i];

		if (!bar->vaddr)
			continue;

		/* Touch BAR to trigger page fault under mmap_lock */
		dummy = *(volatile char *)bar->vaddr;
		(void)dummy;
		has_mmap = true;
	}

	if (!has_mmap)
		SKIP(return, "No mmapable BAR found on device\n");

	/* Trigger device reset under memory_lock */
	vfio_pci_device_reset(self->device);
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test the VFIO selftests drivers, e.g. Intel DSA, with a device bound to
 * vfio-pci in noiommu mode.
 *
 * In noiommu mode the device DMAs to physical addresses (PAs), and nothing
 * stops it from accessing memory it was not given. The drivers program the
 * addresses they get from to_iova() into the device, so this test maps every
 * buffer the device accesses at IOVA == PA, using IOMMU_IOAS_NOIOMMU_GET_PA to
 * look up the PA. Each buffer is physically contiguous, which makes every IOVA
 * in it the PA of the same byte. The buffers come from either:
 *
 *  - anonymous_hugetlb_* variants: one hugetlb page per buffer. The PAs can
 *    change from one run to the next.
 *
 *  - devdax_* variants: fixed offsets in a device-dax instance, e.g. one made
 *    from RAM reserved with memmap=nn!ss. The kernel never allocates from that
 *    memory and the devdax configuration is stored in it, so the PAs are the
 *    same on every run, and across kexec as long as the next kernel is booted
 *    with the same memmap=.
 *
 * Unlike vfio_pci_driver_test, this test does not DMA to or from unmapped
 * IOVAs, since in noiommu mode those are arbitrary physical addresses.
 *
 * Setup, e.g. for an Intel DSA device on a host booted with intel_iommu=off:
 *
 *   echo Y > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
 *   echo 2 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
 *   echo 1 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
 *   tools/testing/selftests/vfio/scripts/setup.sh <segment:bus:device.function>
 *
 * For the devdax variants, also boot with e.g. memmap=2G!4G (the range must be
 * RAM; needs CONFIG_X86_PMEM_LEGACY and CONFIG_DEV_DAX_PMEM), turn the
 * resulting pmem namespace into a devdax instance with at most 2MB alignment,
 * and point the test at it:
 *
 *   ndctl create-namespace -f -e namespace0.0 -m devdax -M mem -a 2M
 *   export VFIO_SELFTESTS_DAX=/dev/dax0.0
 *
 * Variants that cannot get their memory are skipped.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/mman.h>
#include <linux/sizes.h>

#include <libvfio.h>

#include "kselftest_harness.h"

/*
 * The noiommu HWPT has a 64-bit IOVA space but holds at most 52-bit physical
 * addresses, so a mapping at or above this IOVA never overlaps a mapping at
 * IOVA == PA.
 */
#define PIN_IOVA	BIT_ULL(52)

/* The driver's descriptors and completion records are DMA'd too. */
#define DRIVER_REGION_SIZE	SZ_2M

/* Where the regions are in the devdax instance. */
#define DEVDAX_DRIVER_REGION_OFFSET	0
#define DEVDAX_MEMCPY_REGION_OFFSET	DRIVER_REGION_SIZE

static const char *device_bdf;

/* The devdax instance named by $VFIO_SELFTESTS_DAX, if any. */
static struct {
	const char *path;
	int fd;
	u64 pa;
	u64 size;
	u64 align;
} devdax = { .fd = -1 };

#define fcntl_set_msi_nonblock(_self) do {				\
	typeof(_self) __self = (_self);					\
									\
	if (__self->device->driver.ops->send_msi)			\
		fcntl_set_nonblock(__self->msi_fd);			\
} while (0)

#define ASSERT_NO_MSI(_self) do {					\
	typeof(_self) __self = (_self);					\
	u64 __value;							\
									\
	if (!__self->device->driver.ops->send_msi)			\
		break;							\
	ASSERT_EQ(-1, read(__self->msi_fd, &__value, 8));		\
	ASSERT_EQ(EAGAIN, errno);					\
} while (0)

static void *anonymous_hugetlb_mmap(u64 size, int mmap_flags)
{
	return mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_ANONYMOUS | mmap_flags, -1, 0);
}

static void *devdax_mmap(u64 offset, u64 size)
{
	void *vaddr;

	vaddr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		     devdax.fd, offset);
	VFIO_ASSERT_NE(vaddr, MAP_FAILED, "Failed to mmap %s at offset 0x%lx\n",
		       devdax.path, offset);

	return vaddr;
}

/*
 * Map the physically contiguous buffer at @vaddr at IOVA == PA, so that the
 * IOVAs drivers get from to_iova() are the physical addresses the device has
 * to DMA to.
 */
static void region_setup(struct iommu *iommu, struct dma_region *region,
			 void *vaddr, u64 size)
{
	struct dma_region pin = {};
	u64 pa, length;

	/* Map the buffer at a temporary IOVA to pin it and look up its PA. */
	pin.vaddr = vaddr;
	pin.iova = PIN_IOVA;
	pin.size = size;
	iommu_map(iommu, &pin);

	VFIO_ASSERT_EQ(__iommu_noiommu_get_pa(iommu, pin.iova, size, &pa, &length), 0);
	VFIO_ASSERT_EQ(length, size, "Buffer is not physically contiguous\n");

	/*
	 * Map it again at IOVA == PA before dropping the temporary mapping, so
	 * that the pages stay pinned, and hence at the same PA, throughout.
	 */
	region->vaddr = vaddr;
	region->iova = pa;
	region->size = size;
	iommu_map(iommu, region);
	iommu_unmap(iommu, &pin);

	VFIO_ASSERT_EQ(__iommu_noiommu_get_pa(iommu, region->iova, size, &pa, &length), 0);
	VFIO_ASSERT_EQ(pa, region->iova);
	VFIO_ASSERT_EQ(length, size);

	printf("Mapped HVA %p (size 0x%lx) at IOVA == PA 0x%lx\n", vaddr, size, pa);
}

static void region_teardown(struct iommu *iommu, struct dma_region *region)
{
	iommu_unmap(iommu, region);
	VFIO_ASSERT_EQ(munmap(region->vaddr, region->size), 0);
}

FIXTURE(vfio_pci_driver_noiommu_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
	struct dma_region memcpy_region;
	int msi_fd;

	u64 size;
	void *src;
	void *dst;
	iova_t src_iova;
	iova_t dst_iova;
};

FIXTURE_VARIANT(vfio_pci_driver_noiommu_test) {
	/* Size of memcpy_region. */
	u64 size;
	/* Take the regions from the devdax instance... */
	bool devdax;
	/* ...or from anonymous hugetlb pages, memcpy_region with these flags. */
	int mmap_flags;
};

FIXTURE_VARIANT_ADD(vfio_pci_driver_noiommu_test, anonymous_hugetlb_2mb) {
	.size = SZ_2M,
	.mmap_flags = MAP_HUGETLB | MAP_HUGE_2MB,
};

FIXTURE_VARIANT_ADD(vfio_pci_driver_noiommu_test, anonymous_hugetlb_1gb) {
	.size = SZ_1G,
	.mmap_flags = MAP_HUGETLB | MAP_HUGE_1GB,
};

FIXTURE_VARIANT_ADD(vfio_pci_driver_noiommu_test, devdax_2mb) {
	.size = SZ_2M,
	.devdax = true,
};

FIXTURE_VARIANT_ADD(vfio_pci_driver_noiommu_test, devdax_1gb) {
	.size = SZ_1G,
	.devdax = true,
};

FIXTURE_SETUP(vfio_pci_driver_noiommu_test)
{
	struct vfio_pci_driver *driver;
	void *memcpy_vaddr;
	void *driver_vaddr;

	if (variant->devdax) {
		if (devdax.fd < 0)
			SKIP(return, "Set VFIO_SELFTESTS_DAX to use devdax\n");

		if (devdax.size < DEVDAX_MEMCPY_REGION_OFFSET + variant->size)
			SKIP(return, "%s is smaller than 0x%lx bytes\n", devdax.path,
			     DEVDAX_MEMCPY_REGION_OFFSET + variant->size);
	}

	self->iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	self->device = vfio_pci_device_init(device_bdf, self->iommu);

	driver = &self->device->driver;

	if (variant->devdax) {
		memcpy_vaddr = devdax_mmap(DEVDAX_MEMCPY_REGION_OFFSET, variant->size);
		driver_vaddr = devdax_mmap(DEVDAX_DRIVER_REGION_OFFSET, DRIVER_REGION_SIZE);
	} else {
		memcpy_vaddr = anonymous_hugetlb_mmap(variant->size, variant->mmap_flags);
		if (memcpy_vaddr == MAP_FAILED)
			SKIP(return, "Cannot allocate memcpy region: %s\n", strerror(errno));

		driver_vaddr = anonymous_hugetlb_mmap(DRIVER_REGION_SIZE,
						      MAP_HUGETLB | MAP_HUGE_2MB);
		if (driver_vaddr == MAP_FAILED)
			SKIP(return, "Cannot allocate driver region: %s\n", strerror(errno));
	}

	region_setup(self->iommu, &self->memcpy_region, memcpy_vaddr, variant->size);
	region_setup(self->iommu, &driver->region, driver_vaddr, DRIVER_REGION_SIZE);

	/* The devdax regions are at fixed PAs, whichever process maps them. */
	if (variant->devdax) {
		VFIO_ASSERT_EQ(self->memcpy_region.iova, devdax.pa + DEVDAX_MEMCPY_REGION_OFFSET);
		VFIO_ASSERT_EQ(driver->region.iova, devdax.pa + DEVDAX_DRIVER_REGION_OFFSET);
	}

	vfio_pci_driver_init(self->device);
	self->msi_fd = self->device->msi_eventfds[driver->msi];

	/*
	 * Use 4x the driver's max_memcpy_size to exercise the chunking
	 * logic in vfio_pci_driver_memcpy(). Cap to half the memcpy
	 * region so src and dst do not overlap.
	 */
	self->size = min_t(u64, driver->max_memcpy_size * 4,
			   self->memcpy_region.size / 2);

	self->src = self->memcpy_region.vaddr;
	self->dst = self->src + self->size;

	/* These are the PAs of src and dst, see region_setup(). */
	self->src_iova = to_iova(self->device, self->src);
	self->dst_iova = to_iova(self->device, self->dst);
}

FIXTURE_TEARDOWN(vfio_pci_driver_noiommu_test)
{
	struct vfio_pci_driver *driver = &self->device->driver;

	/* Stop the device before unpinning the memory it DMAs to. */
	vfio_pci_driver_remove(self->device);

	region_teardown(self->iommu, &self->memcpy_region);
	region_teardown(self->iommu, &driver->region);

	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
}

TEST_F(vfio_pci_driver_noiommu_test, init_remove)
{
	int i;

	for (i = 0; i < 10; i++) {
		vfio_pci_driver_remove(self->device);
		vfio_pci_driver_init(self->device);
	}
}

TEST_F(vfio_pci_driver_noiommu_test, memcpy_success)
{
	u64 *src = self->src;
	u64 i;

	fcntl_set_msi_nonblock(self);

	/*
	 * Fill each word of src with its own PA, so that the copy does not match
	 * if the device reads from the wrong address.
	 */
	for (i = 0; i < self->size / sizeof(*src); i++)
		src[i] = self->src_iova + i * sizeof(*src);

	memset(self->dst, 'y', self->size);

	ASSERT_EQ(0, vfio_pci_driver_memcpy(self->device,
					    self->src_iova,
					    self->dst_iova,
					    self->size));

	ASSERT_EQ(0, memcmp(self->src, self->dst, self->size));
	ASSERT_NO_MSI(self);
}

TEST_F(vfio_pci_driver_noiommu_test, send_msi)
{
	u64 value;

	if (!self->device->driver.ops->send_msi)
		SKIP(return, "Driver does not support send_msi()\n");

	vfio_pci_driver_send_msi(self->device);
	ASSERT_EQ(8, read(self->msi_fd, &value, 8));
	ASSERT_EQ(1, value);
}

TEST_F(vfio_pci_driver_noiommu_test, mix_and_match)
{
	u64 value;
	int i;

	for (i = 0; i < 10; i++) {
		memset(self->src, 'x', self->size);
		memset(self->dst, 'y', self->size);

		ASSERT_EQ(0, vfio_pci_driver_memcpy(self->device,
						    self->src_iova,
						    self->dst_iova,
						    self->size));

		ASSERT_EQ(0, memcmp(self->src, self->dst, self->size));

		if (!self->device->driver.ops->send_msi)
			continue;

		vfio_pci_driver_send_msi(self->device);
		ASSERT_EQ(8, read(self->msi_fd, &value, 8));
		ASSERT_EQ(1, value);
	}
}

TEST_F_TIMEOUT(vfio_pci_driver_noiommu_test, memcpy_storm, 60)
{
	struct vfio_pci_driver *driver = &self->device->driver;
	u64 total_size;
	u64 size;
	u64 count;

	fcntl_set_msi_nonblock(self);

	/*
	 * Perform up to 250GiB worth of DMA reads and writes across several
	 * memcpy operations. Some devices can support even more but the test
	 * will take too long.
	 */
	total_size = 250UL * SZ_1G;
	size = min(driver->max_memcpy_size, self->memcpy_region.size / 2);
	count = min(total_size / size, driver->max_memcpy_count);

	printf("Kicking off %lu memcpys of size 0x%lx\n", count, size);
	vfio_pci_driver_memcpy_start(self->device,
				     self->src_iova,
				     self->dst_iova,
				     size, count);

	ASSERT_EQ(0, vfio_pci_driver_memcpy_wait(self->device));
	ASSERT_NO_MSI(self);
}

static bool device_has_selftests_driver(const char *bdf)
{
	struct vfio_pci_device *device;
	struct iommu *iommu;
	bool has_driver;

	iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	device = vfio_pci_device_init(bdf, iommu);

	has_driver = !!device->driver.ops;
	if (has_driver)
		printf("Using driver %s for %s\n", device->driver.ops->name, bdf);

	vfio_pci_device_cleanup(device);
	iommu_cleanup(iommu);

	return has_driver;
}

/* Read a number from sysfs attribute @attr of the char device @rdev. */
static u64 devdax_attr_get(dev_t rdev, const char *attr)
{
	char path[PATH_MAX];
	char buf[32] = {};
	u64 val;
	int fd;

	snprintf_assert(path, PATH_MAX, "/sys/dev/char/%u:%u/%s",
			major(rdev), minor(rdev), attr);

	fd = open(path, O_RDONLY);
	VFIO_ASSERT_GE(fd, 0, "Failed to open %s\n", path);
	VFIO_ASSERT_GT(read(fd, buf, sizeof(buf) - 1), 0);
	VFIO_ASSERT_EQ(close(fd), 0);

	errno = 0;
	val = strtoull(buf, NULL, 0);
	VFIO_ASSERT_EQ(errno, 0, "%s is not a number: %s\n", path, buf);

	return val;
}

/*
 * Open the devdax instance at @path, if any, and look up where it is in
 * physical memory.
 */
static void devdax_init(const char *path)
{
	struct stat st;

	if (!path)
		return;

	devdax.path = path;
	devdax.fd = open(path, O_RDWR);
	VFIO_ASSERT_GE(devdax.fd, 0, "Failed to open %s\n", path);

	VFIO_ASSERT_EQ(fstat(devdax.fd, &st), 0);
	VFIO_ASSERT_TRUE(S_ISCHR(st.st_mode), "%s is not a devdax device\n", path);

	devdax.pa = devdax_attr_get(st.st_rdev, "resource");
	devdax.size = devdax_attr_get(st.st_rdev, "size");
	devdax.align = devdax_attr_get(st.st_rdev, "align");

	/*
	 * Each region must be a whole number of devdax pages. The alignment is a
	 * power of two, so it must just not be larger than the smallest region.
	 */
	VFIO_ASSERT_LE(devdax.align, DRIVER_REGION_SIZE,
		       "%s alignment must be at most 2MB\n", path);

	printf("Using %s: PA 0x%lx, size 0x%lx, align 0x%lx\n",
	       path, devdax.pa, devdax.size, devdax.align);
}

int main(int argc, char *argv[])
{
	const char *cdev_path;
	int fd;

	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	cdev_path = __vfio_pci_get_cdev_path(device_bdf, true);
	if (!cdev_path)
		ksft_exit_skip("%s is not bound to vfio-pci in noiommu mode\n",
			       device_bdf);

	fd = open(cdev_path, O_RDWR);
	if (fd < 0)
		ksft_exit_skip("Cannot open %s: %s (requires CAP_SYS_RAWIO)\n",
			       cdev_path, strerror(errno));
	VFIO_ASSERT_EQ(close(fd), 0);
	free((void *)cdev_path);

	if (!device_has_selftests_driver(device_bdf))
		ksft_exit_skip("No driver found for device %s\n", device_bdf);

	devdax_init(getenv("VFIO_SELFTESTS_DAX"));

	return test_harness_run(argc, argv);
}

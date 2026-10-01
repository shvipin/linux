// SPDX-License-Identifier: GPL-2.0-only
/*
 * Example and test of VFIO cdev noiommu mode (CONFIG_IOMMUFD_NOIOMMU).
 *
 * In noiommu mode no IOMMU translates DMA: userspace programs the device with
 * physical addresses and gets no isolation or protection. iommufd is still
 * used to pin and track user memory. IOMMU_IOAS_MAP records IOVA -> PA in a
 * SW-only page table owned by a noiommu HWPT, and IOMMU_IOAS_NOIOMMU_GET_PA
 * returns the physical address backing an IOVA plus the number of physically
 * contiguous bytes that follow it.
 *
 * Requirements:
 *
 *  - CONFIG_VFIO_DEVICE_CDEV=y and CONFIG_VFIO_NOIOMMU=y. Together they select
 *    CONFIG_IOMMUFD_NOIOMMU, which is not user-visible.
 *
 *  - A PCI device that is not behind an IOMMU, e.g. a device in a VM without a
 *    vIOMMU, or a host with the IOMMU disabled.
 *
 *  - noiommu mode enabled *before* the device is bound to vfio-pci:
 *
 *      echo Y > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
 *      tools/testing/selftests/vfio/scripts/setup.sh <segment:bus:device.function>
 *
 *    The device cdev is then /dev/vfio/devices/noiommu-vfioX and the kernel is
 *    tainted.
 *
 *  - CAP_SYS_RAWIO to open the cdev and to use IOMMU_IOAS_NOIOMMU_GET_PA, and
 *    CAP_SYS_ADMIN to cross-check the physical addresses against
 *    /proc/self/pagemap.
 *
 * The flow is the same as for an IOMMU-backed device plus the PA lookup:
 *
 *   device_fd = open("/dev/vfio/devices/noiommu-vfioX")
 *   iommufd = open("/dev/iommu")
 *   ioctl(device_fd, VFIO_DEVICE_BIND_IOMMUFD)
 *   ioctl(iommufd, IOMMU_IOAS_ALLOC)
 *   ioctl(device_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT)  // allocates noiommu HWPT
 *   ioctl(iommufd, IOMMU_IOAS_MAP)                   // pins the memory
 *   ioctl(iommufd, IOMMU_IOAS_NOIOMMU_GET_PA)        // PA to program into the device
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <linux/bits.h>
#include <linux/capability.h>
#include <linux/iommufd.h>
#include <linux/limits.h>
#include <linux/mman.h>
#include <linux/pci_regs.h>
#include <linux/sizes.h>
#include <linux/vfio.h>

#include <libvfio.h>

#include "kselftest_harness.h"

#define PAGEMAP_PFN_MASK	GENMASK_ULL(54, 0)
#define PAGEMAP_PRESENT		BIT_ULL(63)

static const char *device_bdf;
static const char *cdev_path;
static size_t page_size;

static int device_bind_iommufd(int device_fd, int iommufd, u32 *dev_id)
{
	struct vfio_device_bind_iommufd args = {
		.argsz = sizeof(args),
		.iommufd = iommufd,
	};

	if (ioctl(device_fd, VFIO_DEVICE_BIND_IOMMUFD, &args))
		return -errno;

	*dev_id = args.out_devid;
	return 0;
}

static int device_attach_pt(int device_fd, u32 pt_id)
{
	struct vfio_device_attach_iommufd_pt args = {
		.argsz = sizeof(args),
		.pt_id = pt_id,
	};

	if (ioctl(device_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &args))
		return -errno;

	return 0;
}

static int device_detach_pt(int device_fd)
{
	struct vfio_device_detach_iommufd_pt args = {
		.argsz = sizeof(args),
	};

	if (ioctl(device_fd, VFIO_DEVICE_DETACH_IOMMUFD_PT, &args))
		return -errno;

	return 0;
}

static int hwpt_alloc(int iommufd, u32 dev_id, u32 pt_id, u32 flags,
		      u32 *hwpt_id)
{
	struct iommu_hwpt_alloc args = {
		.size = sizeof(args),
		.flags = flags,
		.dev_id = dev_id,
		.pt_id = pt_id,
	};

	if (ioctl(iommufd, IOMMU_HWPT_ALLOC, &args))
		return -errno;

	*hwpt_id = args.out_hwpt_id;
	return 0;
}

static int iommufd_destroy(int iommufd, u32 id)
{
	struct iommu_destroy args = {
		.size = sizeof(args),
		.id = id,
	};

	if (ioctl(iommufd, IOMMU_DESTROY, &args))
		return -errno;

	return 0;
}

/* Issue IOMMU_IOAS_NOIOMMU_GET_PA with caller-crafted (possibly invalid) args. */
static int ioas_noiommu_get_pa(int iommufd, struct iommu_ioas_noiommu_get_pa *args)
{
	if (ioctl(iommufd, IOMMU_IOAS_NOIOMMU_GET_PA, args))
		return -errno;

	return 0;
}

/*
 * Return the physical address backing @vaddr according to /proc/self/pagemap,
 * or 0 if it is not available (PFNs read as 0 without CAP_SYS_ADMIN).
 */
static u64 pagemap_get_pa(int pagemap_fd, void *vaddr)
{
	off_t offset = (uintptr_t)vaddr / page_size * sizeof(u64);
	u64 entry;

	VFIO_ASSERT_EQ(pread(pagemap_fd, &entry, sizeof(entry), offset),
		       sizeof(entry));

	if (!(entry & PAGEMAP_PRESENT) || !(entry & PAGEMAP_PFN_MASK))
		return 0;

	return (entry & PAGEMAP_PFN_MASK) * page_size +
	       (uintptr_t)vaddr % page_size;
}

static void drop_cap_sys_rawio(void)
{
	struct __user_cap_header_struct header = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];

	VFIO_ASSERT_EQ(syscall(SYS_capget, &header, data), 0);
	data[CAP_TO_INDEX(CAP_SYS_RAWIO)].effective &= ~CAP_TO_MASK(CAP_SYS_RAWIO);
	VFIO_ASSERT_EQ(syscall(SYS_capset, &header, data), 0);
}

static u16 sysfs_pci_id_get(const char *bdf, const char *file)
{
	char path[PATH_MAX];
	char buf[16] = {};
	int fd;

	snprintf_assert(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", bdf, file);

	fd = open(path, O_RDONLY);
	VFIO_ASSERT_GE(fd, 0, "open(%s) failed\n", path);
	VFIO_ASSERT_GT(read(fd, buf, sizeof(buf) - 1), 0);
	VFIO_ASSERT_EQ(close(fd), 0);

	return strtoul(buf, NULL, 0);
}

/*
 * Open the noiommu cdev, bind it to @iommu's iommufd and attach it to the
 * IOAS. The attach allocates a HWPT backed by a SW-only page table, which
 * holds the IOVA -> PA translations of every mapping in the IOAS.
 */
static int noiommu_device_open(struct iommu *iommu, u32 *dev_id)
{
	int device_fd;

	device_fd = open(cdev_path, O_RDWR);
	VFIO_ASSERT_GE(device_fd, 0, "open(%s) failed\n", cdev_path);

	VFIO_ASSERT_EQ(device_bind_iommufd(device_fd, iommu->iommufd, dev_id), 0);
	VFIO_ASSERT_EQ(device_attach_pt(device_fd, iommu->ioas_id), 0);

	return device_fd;
}

static void *mmap_anonymous(size_t size, int flags)
{
	return mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE | flags, -1, 0);
}

FIXTURE(vfio_noiommu) {
	struct iommu *iommu;
	struct iova_allocator *iova_allocator;
	int device_fd;
	u32 dev_id;
};

FIXTURE_SETUP(vfio_noiommu)
{
	self->iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	self->device_fd = noiommu_device_open(self->iommu, &self->dev_id);
	self->iova_allocator = iova_allocator_init(self->iommu);
}

FIXTURE_TEARDOWN(vfio_noiommu)
{
	iova_allocator_cleanup(self->iova_allocator);
	ASSERT_EQ(0, close(self->device_fd));
	iommu_cleanup(self->iommu);
}

TEST_F(vfio_noiommu, device_uapi)
{
	struct vfio_device_info info = { .argsz = sizeof(info) };
	struct vfio_region_info config = {
		.argsz = sizeof(config),
		.index = VFIO_PCI_CONFIG_REGION_INDEX,
	};
	u16 vendor, device;

	/* The VFIO device UAPI works the same as for an IOMMU-backed device. */
	ASSERT_EQ(0, ioctl(self->device_fd, VFIO_DEVICE_GET_INFO, &info));
	ASSERT_TRUE(info.flags & VFIO_DEVICE_FLAGS_PCI);

	ASSERT_EQ(0, ioctl(self->device_fd, VFIO_DEVICE_GET_REGION_INFO, &config));
	ASSERT_EQ(sizeof(vendor), pread(self->device_fd, &vendor, sizeof(vendor),
					config.offset + PCI_VENDOR_ID));
	ASSERT_EQ(sizeof(device), pread(self->device_fd, &device, sizeof(device),
					config.offset + PCI_DEVICE_ID));

	printf("Vendor: %04x, Device: %04x\n", vendor, device);
	ASSERT_EQ(sysfs_pci_id_get(device_bdf, "vendor"), vendor);
	ASSERT_EQ(sysfs_pci_id_get(device_bdf, "device"), device);
}

TEST_F(vfio_noiommu, get_hw_info_unsupported)
{
	struct iommu_hw_info info = {
		.size = sizeof(info),
		.dev_id = self->dev_id,
	};

	/* There is no IOMMU hardware to report information about. */
	ASSERT_EQ(-1, ioctl(self->iommu->iommufd, IOMMU_GET_HW_INFO, &info));
	ASSERT_EQ(EOPNOTSUPP, errno);
}

TEST_F(vfio_noiommu, get_pa_sub_page)
{
	const u64 offset = page_size / 2 + 8;
	struct dma_region region = {};
	u64 phys, sub_phys, length;

	region.vaddr = mmap_anonymous(page_size, 0);
	ASSERT_NE(MAP_FAILED, region.vaddr);
	region.iova = iova_allocator_alloc(self->iova_allocator, page_size);
	region.size = page_size;
	iommu_map(self->iommu, &region);

	ASSERT_EQ(0, __iommu_noiommu_get_pa(self->iommu, region.iova, page_size,
					    &phys, &length));
	ASSERT_EQ(0, phys % page_size);
	ASSERT_EQ(page_size, length);

	/* An unaligned IOVA returns the same offset into the physical page. */
	ASSERT_EQ(0, __iommu_noiommu_get_pa(self->iommu, region.iova + offset, page_size,
					    &sub_phys, &length));
	ASSERT_EQ(phys + offset, sub_phys);
	ASSERT_EQ(page_size - offset, length);

	/* The returned length never exceeds the requested length. */
	ASSERT_EQ(0, __iommu_noiommu_get_pa(self->iommu, region.iova + offset, 1,
					    &sub_phys, &length));
	ASSERT_EQ(phys + offset, sub_phys);
	ASSERT_EQ(1, length);

	iommu_unmap(self->iommu, &region);
	ASSERT_EQ(0, munmap(region.vaddr, page_size));
}

TEST_F(vfio_noiommu, get_pa_invalid_args)
{
	struct iommu_ioas_noiommu_get_pa valid, args;
	int iommufd = self->iommu->iommufd;
	struct dma_region region = {};
	iova_t unmapped_iova;

	region.vaddr = mmap_anonymous(page_size, 0);
	ASSERT_NE(MAP_FAILED, region.vaddr);
	region.iova = iova_allocator_alloc(self->iova_allocator, page_size);
	region.size = page_size;
	iommu_map(self->iommu, &region);

	/* Allocated from the IOAS, but never mapped. */
	unmapped_iova = iova_allocator_alloc(self->iova_allocator, page_size);

	valid = (struct iommu_ioas_noiommu_get_pa) {
		.size = sizeof(valid),
		.ioas_id = self->iommu->ioas_id,
		.iova = region.iova,
		.length = page_size,
	};

	args = valid;
	ASSERT_EQ(0, ioas_noiommu_get_pa(iommufd, &args));

	args = valid;
	args.flags = 1;
	ASSERT_EQ(-EOPNOTSUPP, ioas_noiommu_get_pa(iommufd, &args));

	args = valid;
	args.__reserved = 1;
	ASSERT_EQ(-EOPNOTSUPP, ioas_noiommu_get_pa(iommufd, &args));

	args = valid;
	args.length = 0;
	ASSERT_EQ(-EINVAL, ioas_noiommu_get_pa(iommufd, &args));

	args = valid;
	args.iova = unmapped_iova;
	ASSERT_EQ(-ENOENT, ioas_noiommu_get_pa(iommufd, &args));

	args = valid;
	args.iova = UINT64_MAX;
	ASSERT_EQ(-EOVERFLOW, ioas_noiommu_get_pa(iommufd, &args));

	/* The device ID is a valid object ID, but not an IOAS. */
	args = valid;
	args.ioas_id = self->dev_id;
	ASSERT_EQ(-ENOENT, ioas_noiommu_get_pa(iommufd, &args));

	iommu_unmap(self->iommu, &region);
	ASSERT_EQ(0, munmap(region.vaddr, page_size));
}

TEST_F(vfio_noiommu, get_pa_requires_attach)
{
	struct dma_region region = {};
	u64 phys, length;
	int pagemap_fd;
	u64 expected;

	/*
	 * The IOVA -> PA translations live in the noiommu HWPT. Detaching the
	 * device destroys the HWPT that was allocated when it was attached.
	 */
	ASSERT_EQ(0, device_detach_pt(self->device_fd));

	region.vaddr = mmap_anonymous(page_size, 0);
	ASSERT_NE(MAP_FAILED, region.vaddr);
	region.iova = iova_allocator_alloc(self->iova_allocator, page_size);
	region.size = page_size;
	iommu_map(self->iommu, &region);

	/* The IOAS has no noiommu page table to look the IOVA up in. */
	ASSERT_EQ(-EOPNOTSUPP, __iommu_noiommu_get_pa(self->iommu, region.iova,
						      page_size, &phys, &length));

	/* Attaching allocates a new HWPT and fills it with existing mappings. */
	ASSERT_EQ(0, device_attach_pt(self->device_fd, self->iommu->ioas_id));
	ASSERT_EQ(0, __iommu_noiommu_get_pa(self->iommu, region.iova, page_size,
					    &phys, &length));
	ASSERT_EQ(page_size, length);

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	ASSERT_LE(0, pagemap_fd);
	expected = pagemap_get_pa(pagemap_fd, region.vaddr);
	if (expected)
		ASSERT_EQ(expected, phys);
	ASSERT_EQ(0, close(pagemap_fd));

	iommu_unmap(self->iommu, &region);
	ASSERT_EQ(0, munmap(region.vaddr, page_size));
}

TEST_F(vfio_noiommu, hwpt_alloc)
{
	int iommufd = self->iommu->iommufd;
	u32 ioas_id = self->iommu->ioas_id;
	struct dma_region region = {};
	u64 phys, length;
	u32 hwpt_id;

	/*
	 * A noiommu HWPT is a SW-only page table, so it supports none of the
	 * allocation flags, such as nesting or dirty tracking.
	 */
	ASSERT_EQ(-EOPNOTSUPP, hwpt_alloc(iommufd, self->dev_id, ioas_id,
					  IOMMU_HWPT_ALLOC_NEST_PARENT, &hwpt_id));
	ASSERT_EQ(-EOPNOTSUPP, hwpt_alloc(iommufd, self->dev_id, ioas_id,
					  IOMMU_HWPT_ALLOC_DIRTY_TRACKING, &hwpt_id));

	/* Explicitly allocate a noiommu HWPT and move the device to it. */
	ASSERT_EQ(0, hwpt_alloc(iommufd, self->dev_id, ioas_id, 0, &hwpt_id));
	ASSERT_EQ(0, device_attach_pt(self->device_fd, hwpt_id));

	region.vaddr = mmap_anonymous(page_size, 0);
	ASSERT_NE(MAP_FAILED, region.vaddr);
	region.iova = iova_allocator_alloc(self->iova_allocator, page_size);
	region.size = page_size;
	iommu_map(self->iommu, &region);

	ASSERT_EQ(0, __iommu_noiommu_get_pa(self->iommu, region.iova, page_size,
					    &phys, &length));
	ASSERT_EQ(page_size, length);

	iommu_unmap(self->iommu, &region);
	ASSERT_EQ(0, munmap(region.vaddr, page_size));

	ASSERT_EQ(0, device_detach_pt(self->device_fd));
	ASSERT_EQ(0, iommufd_destroy(iommufd, hwpt_id));
}

TEST_F(vfio_noiommu, cap_sys_rawio_required)
{
	struct dma_region region = {};
	u64 phys, length;

	region.vaddr = mmap_anonymous(page_size, 0);
	ASSERT_NE(MAP_FAILED, region.vaddr);
	region.iova = iova_allocator_alloc(self->iova_allocator, page_size);
	region.size = page_size;
	iommu_map(self->iommu, &region);

	ASSERT_EQ(0, __iommu_noiommu_get_pa(self->iommu, region.iova, page_size,
					    &phys, &length));

	/*
	 * A noiommu device can DMA anywhere, so opening its cdev and looking up
	 * physical addresses both require CAP_SYS_RAWIO.
	 */
	drop_cap_sys_rawio();

	ASSERT_EQ(-1, open(cdev_path, O_RDWR));
	ASSERT_EQ(EPERM, errno);

	ASSERT_EQ(-EPERM, __iommu_noiommu_get_pa(self->iommu, region.iova, page_size,
						 &phys, &length));

	iommu_unmap(self->iommu, &region);
	ASSERT_EQ(0, munmap(region.vaddr, page_size));
}

FIXTURE(vfio_noiommu_map) {
	struct iommu *iommu;
	struct iova_allocator *iova_allocator;
	int device_fd;
	u32 dev_id;
};

FIXTURE_VARIANT(vfio_noiommu_map) {
	u64 size;
	int mmap_flags;
};

FIXTURE_VARIANT_ADD(vfio_noiommu_map, anonymous) {
	.size = SZ_1M,
	.mmap_flags = 0,
};

FIXTURE_VARIANT_ADD(vfio_noiommu_map, anonymous_hugetlb_2mb) {
	.size = SZ_2M,
	.mmap_flags = MAP_HUGETLB | MAP_HUGE_2MB,
};

FIXTURE_VARIANT_ADD(vfio_noiommu_map, anonymous_hugetlb_1gb) {
	.size = SZ_1G,
	.mmap_flags = MAP_HUGETLB | MAP_HUGE_1GB,
};

FIXTURE_SETUP(vfio_noiommu_map)
{
	self->iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	self->device_fd = noiommu_device_open(self->iommu, &self->dev_id);
	self->iova_allocator = iova_allocator_init(self->iommu);
}

FIXTURE_TEARDOWN(vfio_noiommu_map)
{
	iova_allocator_cleanup(self->iova_allocator);
	ASSERT_EQ(0, close(self->device_fd));
	iommu_cleanup(self->iommu);
}

TEST_F(vfio_noiommu_map, iova_to_pa)
{
	const bool hugetlb = variant->mmap_flags & MAP_HUGETLB;
	const u64 size = variant->size;
	struct dma_region region = {};
	u64 offset, phys, length;
	u64 nr_chunks = 0;
	bool check_pagemap;
	int pagemap_fd;

	region.vaddr = mmap_anonymous(size, variant->mmap_flags);
	if (region.vaddr == MAP_FAILED && hugetlb)
		SKIP(return, "mmap() failed: %s (%d)\n", strerror(errno), errno);
	ASSERT_NE(MAP_FAILED, region.vaddr);

	region.iova = iova_allocator_alloc(self->iova_allocator, size);
	region.size = size;

	/*
	 * Pin the memory and record its IOVA -> PA translations in the noiommu
	 * HWPT. The pages stay pinned (no mlock() needed) until they are
	 * unmapped from the IOAS.
	 */
	iommu_map(self->iommu, &region);
	printf("Mapped HVA %p (size 0x%lx) at IOVA 0x%lx\n",
	       region.vaddr, size, region.iova);

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	ASSERT_LE(0, pagemap_fd);

	check_pagemap = pagemap_get_pa(pagemap_fd, region.vaddr);
	if (!check_pagemap)
		printf("PFNs hidden in /proc/self/pagemap, not cross-checking PAs\n");

	/*
	 * Split the region into physically contiguous chunks, the way a driver
	 * builds a scatter-gather list to hand to a noiommu device.
	 */
	for (offset = 0; offset < size; offset += length) {
		ASSERT_EQ(0, __iommu_noiommu_get_pa(self->iommu, region.iova + offset,
						    size - offset, &phys, &length));
		ASSERT_EQ(0, phys % page_size);
		ASSERT_LT(0, length);
		ASSERT_GE(size - offset, length);

		if (nr_chunks++ < 4)
			printf("IOVA 0x%lx -> PA 0x%lx (0x%lx bytes)\n",
			       region.iova + offset, phys, length);

		for (u64 i = 0; check_pagemap && i < length; i += page_size)
			ASSERT_EQ(pagemap_get_pa(pagemap_fd, region.vaddr + offset + i),
				  phys + i);
	}

	printf("0x%lx bytes in %lu physically contiguous chunk(s)\n",
	       size, nr_chunks);

	/* A hugetlb page is physically contiguous. */
	if (hugetlb)
		ASSERT_EQ(1, nr_chunks);

	/* Unmapping unpins the memory and removes the translations. */
	iommu_unmap(self->iommu, &region);
	ASSERT_EQ(-ENOENT, __iommu_noiommu_get_pa(self->iommu, region.iova, size,
						  &phys, &length));

	ASSERT_EQ(0, close(pagemap_fd));
	ASSERT_EQ(0, munmap(region.vaddr, size));
}

int main(int argc, char *argv[])
{
	int fd;

	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	page_size = getpagesize();

	cdev_path = __vfio_pci_get_cdev_path(device_bdf, true);
	if (!cdev_path)
		ksft_exit_skip("%s is not bound to vfio-pci in noiommu mode\n",
			       device_bdf);

	fd = open(cdev_path, O_RDWR);
	if (fd < 0)
		ksft_exit_skip("Cannot open %s: %s (requires CAP_SYS_RAWIO)\n",
			       cdev_path, strerror(errno));
	VFIO_ASSERT_EQ(close(fd), 0);

	printf("Using noiommu cdev %s\n", cdev_path);
	return test_harness_run(argc, argv);
}

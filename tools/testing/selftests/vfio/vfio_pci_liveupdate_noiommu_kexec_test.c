// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test that VFIO keeps a device bound to vfio-pci in noiommu mode running
 * across a Live Update: the device keeps doing DMA during and after the kexec,
 * and userspace takes it over in the next kernel without it being reset.
 *
 * Before kexec, the test sets up the device with a VFIO selftests driver that
 * supports attach(), e.g. Intel DSA, kicks off a long-running DMA memcpy,
 * preserves the device in a Live Update session and waits for the kexec. After
 * kexec, it retrieves the device, takes it over by binding it to an iommufd,
 * waits for the memcpy to complete and checks the result. It then uses the
 * device as is, which only works if the device was not reset.
 *
 * In noiommu mode the device DMAs to physical addresses (PAs), so the memory it
 * accesses must be at the same PAs in both kernels, and must not be used by the
 * next kernel. The test takes it from the device-dax instance named by
 * $VFIO_SELFTESTS_DAX and maps it at IOVA == PA, see
 * vfio_pci_driver_noiommu_test.c. The instance must be at the same PAs in both
 * kernels, e.g. made from RAM reserved with the same memmap= in both. It must
 * be at least 1GB + 2MB in size and have at most 2MB alignment.
 *
 * Both kernels must have CONFIG_VFIO_PCI_LIVEUPDATE and CONFIG_IOMMUFD_NOIOMMU
 * enabled and bind the device to vfio-pci in noiommu mode, before any other
 * driver can claim it. E.g. for an Intel DSA device, boot both with:
 *
 *   intel_iommu=off kho=on liveupdate=on vfio.enable_unsafe_noiommu_mode=1
 *   vfio-pci.ids=8086:0b25 vfio-pci.disable_denylist=1 modprobe.blacklist=idxd
 *
 * vfio-pci.disable_denylist=1 is needed for the Sapphire Rapids DSA (0b25).
 * Then run:
 *
 *   export VFIO_SELFTESTS_DAX=/dev/dax0.0
 *   ./vfio_pci_liveupdate_noiommu_kexec_test segment:bus:device.function
 *   (kexec into the next kernel)
 *   ./vfio_pci_liveupdate_noiommu_kexec_test -s 2 segment:bus:device.function
 *
 * To instead check that VFIO resets the device if userspace does not take it
 * over, set $VFIO_SELFTESTS_UNCLAIMED for the run after kexec. The test then
 * finishes the session without binding the device to an iommufd, and without
 * even retrieving it unless the variable is set to "retrieved". It checks that
 * bus mastering got disabled, then opens and uses the device as usual.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/pci_regs.h>
#include <linux/sizes.h>

#include <libliveupdate.h>
#include <libvfio.h>

/* See vfio_pci_driver_noiommu_test.c. */
#define PIN_IOVA	BIT_ULL(52)

/* The driver's descriptors and completion records are DMA'd too. */
#define DRIVER_REGION_SIZE	SZ_2M
#define MEMCPY_REGION_SIZE	SZ_1G

/* Where the regions are in the devdax instance. */
#define DEVDAX_DRIVER_REGION_OFFSET	0
#define DEVDAX_MEMCPY_REGION_OFFSET	DRIVER_REGION_SIZE
#define DEVDAX_SIZE	((u64)DEVDAX_MEMCPY_REGION_OFFSET + MEMCPY_REGION_SIZE)

/*
 * VFIO virtualizes the Interrupt Line register, so the test can change it
 * without affecting the device, to check that VFIO hands over the virtualized
 * config space to the next kernel.
 */
#define INTERRUPT_LINE	0xa5

/* How long to wait for the memcpy operations started before kexec. */
#define MEMCPY_TIMEOUT_S	(30 * 60)

static struct dma_region memcpy_region;
static const char *device_bdf;

static char state_session[LIVEUPDATE_SESSION_NAME_LENGTH];
static char device_session[LIVEUPDATE_SESSION_NAME_LENGTH];

enum {
	STATE_TOKEN,
	DEVICE_TOKEN,
	DEVDAX_TOKEN,
};

static struct {
	const char *path;
	int fd;
	u64 pa;
	u64 size;
	u64 align;
} devdax = { .fd = -1 };

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
 * Open the devdax instance named by $VFIO_SELFTESTS_DAX and look up where it is
 * in physical memory.
 */
static void devdax_init(void)
{
	struct stat st;

	devdax.path = getenv("VFIO_SELFTESTS_DAX");
	VFIO_ASSERT_NOT_NULL(devdax.path, "VFIO_SELFTESTS_DAX is not set\n");

	devdax.fd = open(devdax.path, O_RDWR);
	VFIO_ASSERT_GE(devdax.fd, 0, "Failed to open %s\n", devdax.path);

	VFIO_ASSERT_EQ(fstat(devdax.fd, &st), 0);
	VFIO_ASSERT_TRUE(S_ISCHR(st.st_mode), "%s is not a devdax device\n",
			 devdax.path);

	devdax.pa = devdax_attr_get(st.st_rdev, "resource");
	devdax.size = devdax_attr_get(st.st_rdev, "size");
	devdax.align = devdax_attr_get(st.st_rdev, "align");

	printf("Using %s: PA 0x%lx, size 0x%lx, align 0x%lx\n",
	       devdax.path, devdax.pa, devdax.size, devdax.align);

	/* Each region must be a whole number of devdax pages. */
	VFIO_ASSERT_LE(devdax.align, DRIVER_REGION_SIZE,
		       "%s alignment must be at most 2MB\n", devdax.path);
	VFIO_ASSERT_GE(devdax.size, DEVDAX_SIZE,
		       "%s must be at least 0x%lx bytes\n", devdax.path,
		       DEVDAX_SIZE);
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
 * The device DMAs to the same PAs in both kernels, so record the PA of the
 * devdax instance to check that it did not change across the kexec.
 */
static void devdax_save_pa(int session_fd)
{
	char buf[32];

	snprintf_assert(buf, sizeof(buf), "0x%lx", devdax.pa);
	VFIO_ASSERT_EQ(create_and_preserve_memfd(session_fd, DEVDAX_TOKEN, buf), 0);
}

static void devdax_check_pa(int session_fd)
{
	char buf[32];
	int fd;

	snprintf_assert(buf, sizeof(buf), "0x%lx", devdax.pa);
	fd = restore_and_verify_memfd(session_fd, DEVDAX_TOKEN, buf);
	VFIO_ASSERT_GE(fd, 0, "%s moved across kexec\n", devdax.path);
	VFIO_ASSERT_EQ(close(fd), 0);
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

	printf("Mapped HVA %p (size 0x%lx) at IOVA == PA 0x%lx\n", vaddr, size, pa);
}

static void region_teardown(struct iommu *iommu, struct dma_region *region)
{
	iommu_unmap(iommu, region);
	VFIO_ASSERT_EQ(munmap(region->vaddr, region->size), 0);
}

/*
 * Set up the driver and memcpy regions, at the same offsets in the devdax
 * instance, and hence at the same PAs, in both kernels.
 */
static void regions_setup(struct vfio_pci_device *device)
{
	struct dma_region *driver_region = &device->driver.region;

	region_setup(device->iommu, driver_region,
		     devdax_mmap(DEVDAX_DRIVER_REGION_OFFSET, DRIVER_REGION_SIZE),
		     DRIVER_REGION_SIZE);
	region_setup(device->iommu, &memcpy_region,
		     devdax_mmap(DEVDAX_MEMCPY_REGION_OFFSET, MEMCPY_REGION_SIZE),
		     MEMCPY_REGION_SIZE);

	VFIO_ASSERT_EQ(driver_region->iova, devdax.pa + DEVDAX_DRIVER_REGION_OFFSET);
	VFIO_ASSERT_EQ(memcpy_region.iova, devdax.pa + DEVDAX_MEMCPY_REGION_OFFSET);
}

static void regions_teardown(struct vfio_pci_device *device)
{
	region_teardown(device->iommu, &memcpy_region);
	region_teardown(device->iommu, &device->driver.region);
}

/* Read from the config space of @bdf without going through VFIO. */
static void sysfs_config_read(const char *bdf, int offset, void *buf, size_t size)
{
	char path[PATH_MAX];
	int fd;

	snprintf_assert(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);

	fd = open(path, O_RDONLY);
	VFIO_ASSERT_GE(fd, 0, "Failed to open %s\n", path);
	VFIO_ASSERT_EQ(pread(fd, buf, size, offset), size);
	VFIO_ASSERT_EQ(close(fd), 0);
}

static void vconfig_set(struct vfio_pci_device *device)
{
	vfio_pci_config_writeb(device, PCI_INTERRUPT_LINE, INTERRUPT_LINE);
	VFIO_ASSERT_EQ(vfio_pci_config_readb(device, PCI_INTERRUPT_LINE),
		       INTERRUPT_LINE);
}

static void vconfig_check(struct vfio_pci_device *device)
{
	u8 line;

	/* The device itself has the value set by this kernel, if any. */
	sysfs_config_read(device_bdf, PCI_INTERRUPT_LINE, &line, sizeof(line));
	VFIO_ASSERT_NE(line, INTERRUPT_LINE);

	VFIO_ASSERT_EQ(vfio_pci_config_readb(device, PCI_INTERRUPT_LINE),
		       INTERRUPT_LINE,
		       "The virtualized config space was not preserved\n");
}

static bool bus_master_enabled(const char *bdf)
{
	u16 cmd;

	sysfs_config_read(bdf, PCI_COMMAND, &cmd, sizeof(cmd));
	return cmd & PCI_COMMAND_MASTER;
}

static u64 memcpy_size(struct vfio_pci_device *device)
{
	return min_t(u64, memcpy_region.size / 2, device->driver.max_memcpy_size);
}

/* Check that all @size bytes at @buf are @c. */
static bool is_filled(const char *buf, u64 size, char c)
{
	return buf[0] == c && !memcmp(buf, buf + 1, size - 1);
}

static void dma_memcpy_one(struct vfio_pci_device *device, char c)
{
	u64 size = memcpy_size(device);
	void *src = memcpy_region.vaddr;
	void *dst = src + size;

	memset(src, c, size);
	memset(dst, 0, size);

	printf("Kicking off 1 DMA memcpy operation of size 0x%lx...\n", size);
	VFIO_ASSERT_EQ(vfio_pci_driver_memcpy(device, to_iova(device, src),
					      to_iova(device, dst), size), 0);
	VFIO_ASSERT_TRUE(is_filled(dst, size, c));
}

static void dma_memcpy_start(struct vfio_pci_device *device)
{
	u64 size = memcpy_size(device);
	void *src = memcpy_region.vaddr;
	void *dst = src + size;
	u64 count;

	/*
	 * Rough Math: If we assume the device will perform memcpy at a rate of
	 * 30GB/s then 7200GB of transfers will run for about 4 minutes.
	 */
	count = (u64)7200 * SZ_1G / size;
	count = min_t(u64, count, device->driver.max_memcpy_count);

	memset(src, 'x', size);
	memset(dst, 0, size);

	printf("Kicking off %lu DMA memcpy operations of size 0x%lx...\n", count, size);
	vfio_pci_driver_memcpy_start(device, to_iova(device, src),
				     to_iova(device, dst), size, count);
}

/*
 * Wait for the memcpy operations started by dma_memcpy_start() before kexec
 * to complete, and check that they did.
 */
static void dma_memcpy_wait(struct vfio_pci_device *device)
{
	u64 size = memcpy_size(device);
	void *src = memcpy_region.vaddr;
	void *dst = src + size;
	struct timespec start, end;
	long ms;

	printf("Waiting up to %d s for the DMA memcpy operations kicked off before kexec...\n",
	       MEMCPY_TIMEOUT_S);
	VFIO_ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &start), 0);

	/* Fail rather than hang if the device stopped, e.g. as it was reset. */
	alarm(MEMCPY_TIMEOUT_S);
	VFIO_ASSERT_EQ(vfio_pci_driver_memcpy_wait(device), 0);
	alarm(0);

	VFIO_ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &end), 0);
	ms = (end.tv_sec - start.tv_sec) * 1000 +
	     (end.tv_nsec - start.tv_nsec) / 1000000;

	VFIO_ASSERT_TRUE(is_filled(src, size, 'x'));
	VFIO_ASSERT_TRUE(is_filled(dst, size, 'x'));

	/*
	 * If there was anything to wait for, the device kept doing DMA after
	 * the kexec. If not, take over the device sooner after the kexec to
	 * check that.
	 */
	printf("Waited %ld ms for the memcpy operations to complete\n", ms);
}

/* Skip the test unless the device is bound to vfio-pci in noiommu mode. */
static void check_noiommu(void)
{
	const char *cdev_path;
	int fd;

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
}

static void before_kexec(int luo_fd)
{
	struct vfio_pci_device *device;
	struct iommu *iommu;
	int session_fd;
	int ret;

	check_noiommu();

	if (!getenv("VFIO_SELFTESTS_DAX"))
		ksft_exit_skip("Set VFIO_SELFTESTS_DAX to a devdax instance\n");
	devdax_init();

	iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	device = vfio_pci_device_init(device_bdf, iommu);

	if (!device->driver.ops || !device->driver.ops->attach)
		ksft_exit_skip("No driver that supports attach() found for %s\n",
			       device_bdf);

	regions_setup(device);

	create_state_file(luo_fd, state_session, STATE_TOKEN, /*next_stage=*/2);

	session_fd = luo_create_session(luo_fd, device_session);
	VFIO_ASSERT_GE(session_fd, 0);

	printf("Preserving device in session\n");
	ret = luo_session_preserve_fd(session_fd, device->fd, DEVICE_TOKEN);
	VFIO_ASSERT_EQ(ret, 0);

	devdax_save_pa(session_fd);

	/*
	 * VFIO hands over the config space as it is at kexec, so it includes
	 * changes made after the device was preserved.
	 */
	vconfig_set(device);

	vfio_pci_driver_init(device);
	dma_memcpy_start(device);

	/*
	 * Disable interrupts on the device or freeze() will fail. The memcpy
	 * operations do not generate any.
	 */
	vfio_pci_msix_disable(device);

	close(luo_fd);
	daemonize_and_wait();
}

static void check_open_vfio_device_fails(void)
{
	const char *cdev_path = __vfio_pci_get_cdev_path(device_bdf, true);
	int ret;

	VFIO_ASSERT_NOT_NULL(cdev_path, "%s is not bound to vfio-pci in noiommu mode\n",
			     device_bdf);

	printf("Verifying open(%s) is rejected with EBUSY for a preserved device\n", cdev_path);
	ret = open(cdev_path, O_RDWR);
	VFIO_ASSERT_EQ(ret, -1);
	VFIO_ASSERT_EQ(errno, EBUSY);
	free((void *)cdev_path);
}

static void device_cleanup(struct vfio_pci_device *device, struct iommu *iommu)
{
	/* Stop the device before unpinning the memory it DMAs to. */
	vfio_pci_driver_remove(device);
	regions_teardown(device);

	vfio_pci_device_cleanup(device);
	iommu_cleanup(iommu);
}

/*
 * Check that VFIO resets the device if userspace does not take it over: finish
 * the session without binding the device to an iommufd, and without even
 * retrieving it unless @unclaimed is "retrieved".
 */
static void after_kexec_unclaimed(int session_fd, const char *unclaimed)
{
	struct vfio_pci_device *device;
	struct iommu *iommu;
	int device_fd = -1;

	if (!strcmp(unclaimed, "retrieved")) {
		printf("Retrieving the device FD from LUO\n");
		device_fd = luo_session_retrieve_fd(session_fd, DEVICE_TOKEN);
		VFIO_ASSERT_GE(device_fd, 0);
	}

	printf("Finishing the session without taking over the device\n");
	VFIO_ASSERT_EQ(luo_session_finish(session_fd), 0);

	VFIO_ASSERT_FALSE(bus_master_enabled(device_bdf),
			  "Bus mastering is still enabled, the device was not reset\n");

	if (device_fd >= 0)
		VFIO_ASSERT_EQ(close(device_fd), 0);

	/* The device is no longer preserved, so it can be opened as usual. */
	iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	device = vfio_pci_device_init(device_bdf, iommu);

	regions_setup(device);
	vfio_pci_driver_init(device);
	dma_memcpy_one(device, 'z');

	device_cleanup(device, iommu);
}

static void after_kexec(int luo_fd, int state_session_fd)
{
	struct vfio_pci_device *device;
	const char *unclaimed;
	struct iommu *iommu;
	int session_fd;
	int device_fd;
	int stage;

	devdax_init();

	check_open_vfio_device_fails();

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	VFIO_ASSERT_EQ(stage, 2);

	session_fd = luo_retrieve_session(luo_fd, device_session);
	VFIO_ASSERT_GE(session_fd, 0);

	devdax_check_pa(session_fd);

	VFIO_ASSERT_TRUE(bus_master_enabled(device_bdf),
			 "Bus mastering was disabled across kexec\n");

	unclaimed = getenv("VFIO_SELFTESTS_UNCLAIMED");
	if (unclaimed) {
		after_kexec_unclaimed(session_fd, unclaimed);
		return;
	}

	printf("Retrieving the device FD from LUO\n");
	device_fd = luo_session_retrieve_fd(session_fd, DEVICE_TOKEN);
	VFIO_ASSERT_GE(device_fd, 0);

	printf("Taking over the device by binding it to an iommufd\n");
	iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	device = __vfio_pci_device_init(device_bdf, iommu, device_fd);

	VFIO_ASSERT_TRUE(bus_master_enabled(device_bdf),
			 "Bus mastering was disabled when taking over the device\n");
	vconfig_check(device);

	regions_setup(device);
	vfio_pci_driver_attach(device, /*memcpy_in_progress=*/true);
	dma_memcpy_wait(device);

	/* The device was not reset, so it can be used without reinitializing it. */
	dma_memcpy_one(device, 'y');

	printf("Finishing the session\n");
	VFIO_ASSERT_EQ(luo_session_finish(session_fd), 0);

	/* The device can then be reinitialized as usual. */
	vfio_pci_driver_remove(device);
	vfio_pci_driver_init(device);
	dma_memcpy_one(device, 'z');

	device_cleanup(device, iommu);
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	sprintf(device_session, "noiommu-device-%s", device_bdf);
	sprintf(state_session, "noiommu-state-%s", device_bdf);

	return luo_test(argc, argv, state_session, before_kexec, after_kexec);
}

// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 */

#include <linux/liveupdate.h>
#include <liveupdate_util.h>
#include <vfio_util.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define SESSION_NAME "multi_file_session"
#define TOKEN 1234

static void run_pre_kexec(int luo_fd, const char *bdf)
{
	struct vfio_pci_device *device;
	int session_fd;
	u16 command;

	device = vfio_pci_device_init(bdf, "iommufd");

	command = vfio_pci_config_readw(device, PCI_COMMAND);
	VFIO_ASSERT_FALSE(command & PCI_COMMAND_MASTER);

	vfio_pci_config_writew(device, PCI_COMMAND,
			       command | PCI_COMMAND_MASTER);

	session_fd = luo_create_session(luo_fd, SESSION_NAME);
	VFIO_ASSERT_GE(session_fd, 0, "Failed to create session %s",
		       SESSION_NAME);
	VFIO_ASSERT_EQ(luo_session_preserve_fd(session_fd, device->fd, TOKEN),
		       0, "Failed to preserve VFIO device");
	VFIO_ASSERT_EQ(luo_set_global_event(luo_fd, LIVEUPDATE_PREPARE), 0,
		       "Failed to set global PREPARE event");

	VFIO_ASSERT_EQ(system(KEXEC_SCRIPT), 0, "kexec script failed");

	sleep(10); /* Should not be reached */
	vfio_pci_device_cleanup(device);
	exit(EXIT_FAILURE);
}

static void run_post_kexec(int luo_fd, const char *bdf)
{
	int session_fd;
	int vfio_fd;
	struct vfio_pci_device *device;
	u16 command;


	session_fd = luo_retrieve_session(luo_fd, SESSION_NAME);
	VFIO_ASSERT_GE(session_fd, 0, "Failed to retrieve session %s",
		       SESSION_NAME);

	vfio_fd = luo_session_restore_fd(session_fd, TOKEN);
	if (vfio_fd < 0) {
		printf("Failed to restore VFIO device, error %d", vfio_fd);
		exit(1);
	}

	device = vfio_pci_device_init_fd(vfio_fd);

	if (luo_set_global_event(luo_fd, LIVEUPDATE_FINISH) < 0) {
		printf("Failed to set global FINISH event");
		exit(1);
	}

	close(session_fd);

	command = vfio_pci_config_readw(device, PCI_COMMAND);
	VFIO_ASSERT_TRUE(command & PCI_COMMAND_MASTER);
	vfio_pci_device_cleanup(device);
}

int main(int argc, char *argv[])
{
	enum liveupdate_state state;
	const char *device_bdf;
	int luo_fd;

	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	luo_fd = luo_open_device();
	VFIO_ASSERT_GE(luo_fd, 0, "Failed to open %s", LUO_DEVICE);
	VFIO_ASSERT_EQ(luo_get_global_state(luo_fd, &state), 0, "Failed to get LUO state.");

	switch (state) {
	case LIVEUPDATE_STATE_NORMAL:
		printf("Running pre-kexec actions.\n");
		run_pre_kexec(luo_fd, device_bdf);
		break;
	case LIVEUPDATE_STATE_UPDATED:
		printf("Running post-kexec actions.\n");
		run_post_kexec(luo_fd, device_bdf);
		break;
	default:
		printf("Test started in an unexpected state: %d", state);
	}

	close(luo_fd);
}

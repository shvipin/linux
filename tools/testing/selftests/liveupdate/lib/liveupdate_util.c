// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#define _GNU_SOURCE

#include <liveupdate_util.h>
#include <linux/liveupdate.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>

int luo_open_device(void)
{
	return open(LUO_DEVICE, O_RDWR);
}

int luo_create_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_create_session arg = { .size = sizeof(arg) };

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);
	if (ioctl(luo_fd, LIVEUPDATE_IOCTL_CREATE_SESSION, &arg) < 0)
		return -errno;
	return arg.fd;
}

int luo_retrieve_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_retrieve_session arg = { .size = sizeof(arg) };

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);
	if (ioctl(luo_fd, LIVEUPDATE_IOCTL_RETRIEVE_SESSION, &arg) < 0)
		return -errno;
	return arg.fd;
}

int luo_set_session_event(int session_fd, enum liveupdate_event event)
{
	struct liveupdate_session_set_event arg = { .size = sizeof(arg) };

	arg.event = event;
	return ioctl(session_fd, LIVEUPDATE_SESSION_SET_EVENT, &arg);
}

int luo_set_global_event(int luo_fd, enum liveupdate_event event)
{
	struct liveupdate_ioctl_set_event arg = { .size = sizeof(arg) };

	arg.event = event;
	return ioctl(luo_fd, LIVEUPDATE_IOCTL_SET_EVENT, &arg);
}

int luo_get_global_state(int luo_fd, enum liveupdate_state *state)
{
	struct liveupdate_ioctl_get_state arg = { .size = sizeof(arg) };

	if (ioctl(luo_fd, LIVEUPDATE_IOCTL_GET_STATE, &arg) < 0)
		return -errno;
	*state = arg.state;
	return 0;
}

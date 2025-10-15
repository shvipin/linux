/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef SELFTESTS_LIVEUPDATE_LIB_LIVEUPDATE_UTIL_H
#define SELFTESTS_LIVEUPDATE_LIB_LIVEUPDATE_UTIL_H

#include <linux/liveupdate.h>

#define LUO_DEVICE "/dev/liveupdate"
#define KEXEC_SCRIPT "libliveupdate/do_kexec.sh"

int luo_open_device(void);
int luo_create_session(int luo_fd, const char *name);
int luo_retrieve_session(int luo_fd, const char *name);
int luo_session_preserve_fd(int session_fd, int fd, int token);
int luo_session_unpreserve_fd(int session_fd, int token);
int luo_session_restore_fd(int session_fd, int token);

int luo_set_session_event(int session_fd, enum liveupdate_event event);
int luo_set_global_event(int luo_fd, enum liveupdate_event event);
int luo_get_global_state(int luo_fd, enum liveupdate_state *state);

#endif /* SELFTESTS_LIVEUPDATE_LIB_LIVEUPDATE_UTIL_H */

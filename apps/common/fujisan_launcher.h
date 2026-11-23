/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Freestanding Pure-ARM32 Child Process Launcher Helper
 */

#ifndef _FUJISAN_LAUNCHER_H_
#define _FUJISAN_LAUNCHER_H_

#include <stdbool.h>
#include <stddef.h>

#include "fujisan_sys.h"

#if defined(__arm__) && !defined(FUJISAN_HOST_TEST)
static inline int fj_run_app(const char *path)
{
	long pid;
	int status = 0;

	if (!path || !fj_path_exists(path))
		return -1;
	pid = fj_sc1(FJ_SYS_fork, 0);
	if (pid == 0) {
		char *argv[2];
		char *envp[2];

		argv[0] = (char *)path;
		argv[1] = NULL;
		envp[0] = "PATH=/usr/local/bin:/usr/bin:/bin";
		envp[1] = NULL;
		fj_sc3(FJ_SYS_execve, (long)path, (long)argv, (long)envp);
		fj_sc1(FJ_SYS_exit, 127);
	}
	if (pid < 0)
		return -1;
	fj_sc4(FJ_SYS_wait4, pid, (long)&status, 0, 0);
	return (status >> 8) & 0xFF;
}
#endif /* __arm__ && !FUJISAN_HOST_TEST */

#endif /* _FUJISAN_LAUNCHER_H_ */

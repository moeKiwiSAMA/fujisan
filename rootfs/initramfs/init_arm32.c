/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Freestanding Pure-ARM32 Offline Stage-1 /init
 *
 * Zero libc / zero libgcc, 0 Thumb ($t) symbols.
 * Mounts /proc, /sys, /dev, /tmp, /run and launches /usr/local/bin/fujisan-home.
 */

#include "fujisan_sys.h"
#include "fujisan_launcher.h"

void _start(void)
{
	fj_sc2(FJ_SYS_mkdir, (long)"/proc", 0755);
	fj_sc2(FJ_SYS_mkdir, (long)"/sys", 0755);
	fj_sc2(FJ_SYS_mkdir, (long)"/dev", 0755);
	fj_sc2(FJ_SYS_mkdir, (long)"/run", 0755);
	fj_sc2(FJ_SYS_mkdir, (long)"/tmp", 01777);
	fj_sc2(FJ_SYS_mkdir, (long)"/mnt", 0755);
	fj_sc2(FJ_SYS_mkdir, (long)"/mnt/sd1", 0755);
	fj_sc2(FJ_SYS_mkdir, (long)"/mnt/sd2", 0755);

	fj_sc5(FJ_SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0);
	fj_sc5(FJ_SYS_mount, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);
	fj_sc5(FJ_SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
	fj_sc5(FJ_SYS_mount, (long)"tmpfs", (long)"/run", (long)"tmpfs", 0, 0);
	fj_sc5(FJ_SYS_mount, (long)"tmpfs", (long)"/tmp", (long)"tmpfs", 0, 0);

	for (;;) {
		if (fj_path_exists("/usr/local/bin/fujisan-home"))
			fj_run_app("/usr/local/bin/fujisan-home");
		fj_sleep_ms(500);
	}
}

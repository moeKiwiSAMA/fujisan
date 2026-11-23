/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Home Launcher Core (home_core.h)
 *
 * Host-testable tile navigation and touch hit-testing logic for fujisan-home.
 */

#ifndef _FUJISAN_HOME_CORE_H_
#define _FUJISAN_HOME_CORE_H_

#include <stdbool.h>
#include <stdint.h>

#define FJ_HOME_TILE_COUNT	4U

enum fj_home_tile {
	FJ_TILE_CAM = 0,
	FJ_TILE_TERM = 1,
	FJ_TILE_USB = 2,
	FJ_TILE_STOCK = 3,
};

struct fj_home_tile_rect {
	int x;
	int y;
	int w;
	int h;
	const char *title;
	const char *subtitle;
	const char *exec_path;
};

static const struct fj_home_tile_rect fj_home_tiles[FJ_HOME_TILE_COUNT] = {
	{ 64,  140, 420, 240, "FUJISAN CAM",   "IMX461 V4L2 + LENSCOM + SD1 DCIM", "/usr/local/bin/fujisan-cam" },
	{ 540, 140, 420, 240, "TERMINAL",      "SPX4 Diagnostics & Shell",         "/usr/local/bin/fujisan-term" },
	{ 64,  420, 420, 240, "USB GADGET",    "DWC3 NCM / UVC / MTP Status",      NULL },
	{ 540, 420, 420, 240, "REBOOT STOCK",  "Clean Sync & Stock Firmware",      "/usr/local/bin/fujisan-reboot-stock" },
};

static inline uint32_t fj_home_move(uint32_t cur, int dx, int dy)
{
	uint32_t col = cur & 1U;
	uint32_t row = (cur >> 1) & 1U;

	if (dx != 0)
		col ^= 1U;
	if (dy != 0)
		row ^= 1U;
	return (row << 1) | col;
}

static inline int fj_home_hit_test(int tx, int ty)
{
	uint32_t i;

	for (i = 0U; i < FJ_HOME_TILE_COUNT; i++) {
		const struct fj_home_tile_rect *r = &fj_home_tiles[i];

		if (tx >= r->x && tx < r->x + r->w && ty >= r->y && ty < r->y + r->h)
			return (int)i;
	}
	return -1;
}

#endif /* _FUJISAN_HOME_CORE_H_ */

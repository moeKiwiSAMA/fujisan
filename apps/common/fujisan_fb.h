/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Pure-ARM32 Framebuffer (/dev/fb0) & DRM (/dev/dri/card0)
 * Drawing Primitives + Complete Printable ASCII 5x7 Bitmap Font.
 *
 * Honors SPX4 LCDC1 RGBA8888 (A=0xFF opaque) and XRGB8888 pixel layouts.
 */

#ifndef _FUJISAN_FB_H_
#define _FUJISAN_FB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fujisan_sys.h"

#define FJ_FBIOGET_VSCREENINFO	0x4600U
#define FJ_FBIOGET_FSCREENINFO	0x4602U
#define FJ_DRM_IOCTL_VERSION	0xC0246400U

struct fj_fb_bitfield {
	uint32_t offset;
	uint32_t length;
	uint32_t msb_right;
};

struct fj_fb_var_screeninfo {
	uint32_t xres;
	uint32_t yres;
	uint32_t xres_virtual;
	uint32_t yres_virtual;
	uint32_t xoffset;
	uint32_t yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct fj_fb_bitfield red;
	struct fj_fb_bitfield green;
	struct fj_fb_bitfield blue;
	struct fj_fb_bitfield transp;
	uint32_t rest[20];
};

struct fj_fb_fix_screeninfo {
	char id[16];
	uint32_t smem_start;
	uint32_t smem_len;
	uint32_t type;
	uint32_t type_aux;
	uint32_t visual;
	uint16_t xpanstep;
	uint16_t ypanstep;
	uint16_t ywrapstep;
	uint32_t line_length;
	uint32_t rest[8];
};

struct fj_drm_version {
	int32_t version_major;
	int32_t version_minor;
	int32_t version_patchlevel;
	uint32_t name_len;
	uint32_t name_ptr;
	uint32_t date_len;
	uint32_t date_ptr;
	uint32_t desc_len;
	uint32_t desc_ptr;
};

struct fj_fb {
	int fd;
	uint32_t *pixels;
	uint32_t map_len;
	uint32_t width;
	uint32_t height;
	uint32_t stride_px;
	uint32_t r_off;
	uint32_t g_off;
	uint32_t b_off;
	uint32_t a_off;
	bool drm_detected;
	char drm_driver[32];
};

static inline uint32_t fj_fb_pack(const struct fj_fb *fb, uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)r << fb->r_off) |
	       ((uint32_t)g << fb->g_off) |
	       ((uint32_t)b << fb->b_off) |
	       (0xFFU << fb->a_off);
}

static inline const uint8_t *fj_font5x7(char ch)
{
	static const uint8_t font[95][7] = {
		{ 0,  0,  0,  0,  0,  0,  0 },  /* ' ' */
		{ 4,  4,  4,  4,  4,  0,  4 },  /* '!' */
		{ 10, 10, 10, 0,  0,  0,  0 },  /* '"' */
		{ 10, 31, 10, 10, 31, 10, 0 },  /* '#' */
		{ 4,  15, 20, 14, 5,  30, 4 },  /* '$' */
		{ 24, 25, 2,  4,  8,  19, 3 },  /* '%' */
		{ 12, 18, 20, 8,  21, 18, 13 }, /* '&' */
		{ 4,  4,  8,  0,  0,  0,  0 },  /* '\'' */
		{ 2,  4,  8,  8,  8,  4,  2 },  /* '(' */
		{ 8,  4,  2,  2,  2,  4,  8 },  /* ')' */
		{ 4,  21, 14, 31, 14, 21, 4 },  /* '*' */
		{ 0,  4,  4,  31, 4,  4,  0 },  /* '+' */
		{ 0,  0,  0,  0,  4,  4,  8 },  /* ',' */
		{ 0,  0,  0,  31, 0,  0,  0 },  /* '-' */
		{ 0,  0,  0,  0,  0,  12, 12 }, /* '.' */
		{ 1,  2,  2,  4,  8,  8,  16 }, /* '/' */
		{ 14, 17, 19, 21, 25, 17, 14 }, /* '0' */
		{ 4,  12, 4,  4,  4,  4,  14 }, /* '1' */
		{ 14, 17, 1,  2,  4,  8,  31 }, /* '2' */
		{ 30, 1,  1,  14, 1,  1,  30 }, /* '3' */
		{ 2,  6,  10, 18, 31, 2,  2 },  /* '4' */
		{ 31, 16, 30, 1,  1,  17, 14 }, /* '5' */
		{ 14, 16, 30, 17, 17, 17, 14 }, /* '6' */
		{ 31, 1,  2,  4,  8,  8,  8 },  /* '7' */
		{ 14, 17, 17, 14, 17, 17, 14 }, /* '8' */
		{ 14, 17, 17, 15, 1,  2,  28 }, /* '9' */
		{ 0,  4,  0,  0,  0,  4,  0 },  /* ':' */
		{ 0,  4,  0,  0,  4,  4,  8 },  /* ';' */
		{ 2,  4,  8,  16, 8,  4,  2 },  /* '<' */
		{ 0,  0,  31, 0,  31, 0,  0 },  /* '=' */
		{ 8,  4,  2,  1,  2,  4,  8 },  /* '>' */
		{ 14, 17, 1,  2,  4,  0,  4 },  /* '?' */
		{ 14, 17, 23, 21, 23, 16, 14 }, /* '@' */
		{ 14, 17, 17, 31, 17, 17, 17 }, /* 'A' */
		{ 30, 17, 17, 30, 17, 17, 30 }, /* 'B' */
		{ 14, 17, 16, 16, 16, 17, 14 }, /* 'C' */
		{ 28, 18, 17, 17, 17, 18, 28 }, /* 'D' */
		{ 31, 16, 16, 30, 16, 16, 31 }, /* 'E' */
		{ 31, 16, 16, 30, 16, 16, 16 }, /* 'F' */
		{ 14, 17, 16, 23, 17, 17, 15 }, /* 'G' */
		{ 17, 17, 17, 31, 17, 17, 17 }, /* 'H' */
		{ 14, 4,  4,  4,  4,  4,  14 }, /* 'I' */
		{ 7,  2,  2,  2,  18, 18, 12 }, /* 'J' */
		{ 17, 18, 20, 24, 20, 18, 17 }, /* 'K' */
		{ 16, 16, 16, 16, 16, 16, 31 }, /* 'L' */
		{ 17, 27, 21, 21, 17, 17, 17 }, /* 'M' */
		{ 17, 25, 21, 19, 17, 17, 17 }, /* 'N' */
		{ 14, 17, 17, 17, 17, 17, 14 }, /* 'O' */
		{ 30, 17, 17, 30, 16, 16, 16 }, /* 'P' */
		{ 14, 17, 17, 17, 21, 18, 13 }, /* 'Q' */
		{ 30, 17, 17, 30, 20, 18, 17 }, /* 'R' */
		{ 15, 16, 16, 14, 1,  1,  30 }, /* 'S' */
		{ 31, 4,  4,  4,  4,  4,  4 },  /* 'T' */
		{ 17, 17, 17, 17, 17, 17, 14 }, /* 'U' */
		{ 17, 17, 17, 17, 17, 10, 4 },  /* 'V' */
		{ 17, 17, 17, 21, 21, 27, 17 }, /* 'W' */
		{ 17, 17, 10, 4,  10, 17, 17 }, /* 'X' */
		{ 17, 17, 10, 4,  4,  4,  4 },  /* 'Y' */
		{ 31, 1,  2,  4,  8,  16, 31 }, /* 'Z' */
		{ 14, 8,  8,  8,  8,  8,  14 }, /* '[' */
		{ 16, 8,  8,  4,  2,  2,  1 },  /* '\\' */
		{ 14, 2,  2,  2,  2,  2,  14 }, /* ']' */
		{ 4,  10, 17, 0,  0,  0,  0 },  /* '^' */
		{ 0,  0,  0,  0,  0,  0,  31 }, /* '_' */
		{ 8,  4,  2,  0,  0,  0,  0 },  /* '`' */
		{ 0,  0,  14, 1,  15, 17, 15 }, /* 'a' */
		{ 16, 16, 22, 25, 17, 17, 30 }, /* 'b' */
		{ 0,  0,  14, 16, 16, 17, 14 }, /* 'c' */
		{ 1,  1,  13, 19, 17, 17, 15 }, /* 'd' */
		{ 0,  0,  14, 17, 31, 16, 14 }, /* 'e' */
		{ 6,  9,  8,  28, 8,  8,  8 },  /* 'f' */
		{ 0,  15, 17, 17, 15, 1,  14 }, /* 'g' */
		{ 16, 16, 22, 25, 17, 17, 17 }, /* 'h' */
		{ 4,  0,  12, 4,  4,  4,  14 }, /* 'i' */
		{ 2,  0,  6,  2,  2,  18, 12 }, /* 'j' */
		{ 16, 16, 18, 20, 24, 20, 18 }, /* 'k' */
		{ 12, 4,  4,  4,  4,  4,  14 }, /* 'l' */
		{ 0,  0,  26, 21, 21, 17, 17 }, /* 'm' */
		{ 0,  0,  22, 25, 17, 17, 17 }, /* 'n' */
		{ 0,  0,  14, 17, 17, 17, 14 }, /* 'o' */
		{ 0,  0,  30, 17, 30, 16, 16 }, /* 'p' */
		{ 0,  0,  13, 19, 15, 1,  1 },  /* 'q' */
		{ 0,  0,  22, 25, 16, 16, 16 }, /* 'r' */
		{ 0,  0,  14, 16, 14, 1,  30 }, /* 's' */
		{ 8,  8,  28, 8,  8,  9,  6 },  /* 't' */
		{ 0,  0,  17, 17, 17, 19, 13 }, /* 'u' */
		{ 0,  0,  17, 17, 17, 10, 4 },  /* 'v' */
		{ 0,  0,  17, 17, 21, 21, 10 }, /* 'w' */
		{ 0,  0,  17, 10, 4,  10, 17 }, /* 'x' */
		{ 0,  0,  17, 17, 15, 1,  14 }, /* 'y' */
		{ 0,  0,  31, 2,  4,  8,  31 }, /* 'z' */
		{ 2,  4,  4,  8,  4,  4,  2 },  /* '{' */
		{ 4,  4,  4,  4,  4,  4,  4 },  /* '|' */
		{ 8,  4,  4,  2,  4,  4,  8 },  /* '}' */
		{ 0,  8,  21, 2,  0,  0,  0 },  /* '~' */
	};
	unsigned char uc = (unsigned char)ch;

	if (uc < 0x20U || uc > 0x7EU)
		return font[0];
	return font[uc - 0x20U];
}

static inline void fj_fb_fill_rect(const struct fj_fb *fb, int x, int y, int w, int h,
				   uint32_t col)
{
	int ix, iy;

	if (!fb || !fb->pixels)
		return;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > (int)fb->width)
		w = (int)fb->width - x;
	if (y + h > (int)fb->height)
		h = (int)fb->height - y;
	if (w <= 0 || h <= 0)
		return;

	for (iy = 0; iy < h; iy++) {
		uint32_t *row = fb->pixels + ((uint32_t)(y + iy) * fb->stride_px) + (uint32_t)x;

		for (ix = 0; ix < w; ix++)
			row[ix] = col;
	}
}

static inline void fj_fb_draw_border(const struct fj_fb *fb, int x, int y, int w, int h,
				     int bw, uint32_t col)
{
	fj_fb_fill_rect(fb, x, y, w, bw, col);
	fj_fb_fill_rect(fb, x, y + h - bw, w, bw, col);
	fj_fb_fill_rect(fb, x, y, bw, h, col);
	fj_fb_fill_rect(fb, x + w - bw, y, bw, h, col);
}

static inline void fj_fb_draw_char(const struct fj_fb *fb, int x, int y, int scale,
				   char ch, uint32_t col)
{
	const uint8_t *g = fj_font5x7(ch);
	int r, c;

	for (r = 0; r < 7; r++) {
		for (c = 0; c < 5; c++) {
			if (g[r] & (1U << (4 - c)))
				fj_fb_fill_rect(fb, x + c * scale, y + r * scale,
						scale, scale, col);
		}
	}
}

static inline void fj_fb_draw_text(const struct fj_fb *fb, int x, int y, int scale,
				   const char *s, uint32_t col)
{
	while (s && *s) {
		fj_fb_draw_char(fb, x, y, scale, *s, col);
		x += 6 * scale;
		s++;
	}
}

static inline void fj_fb_draw_text_n(const struct fj_fb *fb, int x, int y, int scale,
				     const char *s, size_t max_chars, uint32_t col)
{
	size_t i = 0;

	while (s && s[i] && i < max_chars) {
		fj_fb_draw_char(fb, x, y, scale, s[i], col);
		x += 6 * scale;
		i++;
	}
}

#if defined(__arm__) && !defined(FUJISAN_HOST_TEST)
static inline void fj_drm_probe(struct fj_fb *fb)
{
	int fd;
	struct fj_drm_version ver;
	char name[32];

	fb->drm_detected = false;
	fj_strlcpy(fb->drm_driver, "fbdev-only", sizeof(fb->drm_driver));

	fd = (int)fj_sc3(FJ_SYS_open, (long)"/dev/dri/card0", FJ_O_RDWR, 0);
	if (fd < 0)
		fd = (int)fj_sc3(FJ_SYS_open, (long)"/dev/dri/card0", FJ_O_RDONLY, 0);
	if (fd < 0)
		return;

	memset(&ver, 0, sizeof(ver));
	memset(name, 0, sizeof(name));
	ver.name_len = sizeof(name) - 1;
	ver.name_ptr = (uint32_t)(uintptr_t)name;
	if (fj_sc3(FJ_SYS_ioctl, fd, (long)FJ_DRM_IOCTL_VERSION, (long)&ver) == 0) {
		name[sizeof(name) - 1] = '\0';
		if (name[0]) {
			fj_strlcpy(fb->drm_driver, name, sizeof(fb->drm_driver));
			fb->drm_detected = true;
		}
	}
	fj_sc1(FJ_SYS_close, fd);
}

static inline bool fj_fb_open(struct fj_fb *fb)
{
	struct fj_fb_var_screeninfo vinfo;
	struct fj_fb_fix_screeninfo finfo;
	long mapped;

	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
	fj_drm_probe(fb);

	fb->fd = (int)fj_sc3(FJ_SYS_open, (long)"/dev/fb0", FJ_O_RDWR, 0);
	if (fb->fd < 0)
		return false;

	memset(&vinfo, 0, sizeof(vinfo));
	memset(&finfo, 0, sizeof(finfo));
	if (fj_sc3(FJ_SYS_ioctl, fb->fd, FJ_FBIOGET_VSCREENINFO, (long)&vinfo) < 0 ||
	    fj_sc3(FJ_SYS_ioctl, fb->fd, FJ_FBIOGET_FSCREENINFO, (long)&finfo) < 0) {
		fj_sc1(FJ_SYS_close, fb->fd);
		fb->fd = -1;
		return false;
	}

	fb->width = vinfo.xres ? vinfo.xres : 1024U;
	fb->height = vinfo.yres ? vinfo.yres : 768U;
	fb->stride_px = finfo.line_length ? (finfo.line_length >> 2) : fb->width;
	fb->r_off = vinfo.red.length ? vinfo.red.offset : 24U;
	fb->g_off = vinfo.green.length ? vinfo.green.offset : 16U;
	fb->b_off = vinfo.blue.length ? vinfo.blue.offset : 8U;
	fb->a_off = vinfo.transp.length ? vinfo.transp.offset : 0U;
	fb->map_len = finfo.smem_len ? finfo.smem_len : (fb->stride_px * fb->height * 4U);

	mapped = fj_sc6(FJ_SYS_mmap2, 0, (long)fb->map_len,
			FJ_PROT_READ | FJ_PROT_WRITE, FJ_MAP_SHARED, fb->fd, 0);
	if (mapped < 0 && mapped > -4096) {
		fj_sc1(FJ_SYS_close, fb->fd);
		fb->fd = -1;
		return false;
	}
	fb->pixels = (uint32_t *)(uintptr_t)mapped;
	return true;
}

static inline void fj_fb_close(struct fj_fb *fb)
{
	if (fb->pixels) {
		fj_sc2(FJ_SYS_munmap, (long)(uintptr_t)fb->pixels, (long)fb->map_len);
		fb->pixels = NULL;
	}
	if (fb->fd >= 0) {
		fj_sc1(FJ_SYS_close, fb->fd);
		fb->fd = -1;
	}
}

static inline int fj_evdev_open_all(struct fj_pollfd *pfds, int max_fds)
{
	int i, nfds = 0;

	for (i = 0; i < 6 && nfds < max_fds; i++) {
		char path[24] = "/dev/input/event0";
		int fd;

		path[16] = (char)('0' + i);
		fd = (int)fj_sc3(FJ_SYS_open, (long)path, FJ_O_RDONLY | FJ_O_NONBLOCK, 0);
		if (fd >= 0) {
			pfds[nfds].fd = fd;
			pfds[nfds].events = FJ_POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}
	}
	return nfds;
}
#endif /* __arm__ && !FUJISAN_HOST_TEST */

#endif /* _FUJISAN_FB_H_ */

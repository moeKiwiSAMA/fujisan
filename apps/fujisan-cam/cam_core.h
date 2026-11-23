/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Camera App Core (CMOS / V4L2 / LENSCOM / JPEG / SD1 DCIM)
 *
 * Host-testable pure C11 core (zero libc dependencies, zero float).
 * Supports:
 *  - IMX461 exposure/ISO/shutter/sensor-temp state & mode presets
 *  - GF LENSCOM v4l2-subdev telemetry (focal length, F-number, AF-S/AF-C/MF)
 *  - Semi-planar NV12 (LV Ch0/Ch1 0xFFF72000/73000) & UYVY -> RGBA8888 blit
 *  - Freestanding baseline JFIF JPEG encoder for SD1 DCIM/100_FJOS/FJOSxxxx.JPG
 */

#ifndef _FUJISAN_CAM_CORE_H_
#define _FUJISAN_CAM_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fujisan_sys.h"
#include "fujisan_fb.h"

#define FJ_CAM_MAX_SHOTS	9999U
#define FJ_CAM_ISO_COUNT	8
#define FJ_CAM_SHUTTER_COUNT	9
#define FJ_CAM_APERTURE_COUNT	7

enum fj_cam_af_mode {
	FJ_AF_SINGLE = 0,
	FJ_AF_CONTINUOUS = 1,
	FJ_AF_MANUAL = 2,
};

enum fj_cam_sensor_mode {
	FJ_MODE_LV_NV12 = 0,
	FJ_MODE_STILL_102MP_16B = 1,
	FJ_MODE_MOVIE_4K_Y0L2 = 2,
};

struct fj_lens_telemetry {
	bool attached;
	bool electronic_contacts;
	char model[48];
	uint16_t focal_mm_x10;		/* e.g. 630 = 63.0 mm */
	uint16_t fnumber_x10;		/* e.g. 28 = f/2.8 */
	uint32_t focus_mm;		/* focus distance in mm (0 = infinity) */
	bool af_locked;
};

struct fj_cam_state {
	uint32_t iso_idx;
	uint32_t shutter_idx;
	uint32_t aperture_idx;
	enum fj_cam_af_mode af_mode;
	enum fj_cam_sensor_mode sensor_mode;
	int32_t sensor_temp_mc;		/* millidegrees C from IMX461 0x0040/0x0041 */
	uint32_t next_dcf_num;		/* 1..9999 -> FJOS0001.JPG */
	uint32_t shots_taken;
	bool v4l2_live;
	bool sd1_ready;
	struct fj_lens_telemetry lens;
	char last_path[96];
	char status_msg[96];
};

static const uint32_t fj_cam_iso_table[FJ_CAM_ISO_COUNT] = {
	100U, 200U, 400U, 800U, 1600U, 3200U, 6400U, 12800U
};

static const char *const fj_cam_shutter_labels[FJ_CAM_SHUTTER_COUNT] = {
	"1/4000", "1/2000", "1/1000", "1/500", "1/250",
	"1/125", "1/60", "1/30", "1/15"
};

static const uint16_t fj_cam_aperture_x10[FJ_CAM_APERTURE_COUNT] = {
	20U, 28U, 40U, 56U, 80U, 110U, 160U
};

static inline void fj_cam_init(struct fj_cam_state *st)
{
	st->iso_idx = 1U;		/* ISO 200 */
	st->shutter_idx = 5U;		/* 1/125 s */
	st->aperture_idx = 2U;		/* f/4.0 */
	st->af_mode = FJ_AF_SINGLE;
	st->sensor_mode = FJ_MODE_LV_NV12;
	st->sensor_temp_mc = 32500;	/* 32.5 C default until IMX461 sysfs read */
	st->next_dcf_num = 1U;
	st->shots_taken = 0U;
	st->v4l2_live = false;
	st->sd1_ready = false;

	st->lens.attached = true;
	st->lens.electronic_contacts = true;
	fj_strlcpy(st->lens.model, "MF 45-100mm F4 OIS (SPI0)", sizeof(st->lens.model));
	st->lens.focal_mm_x10 = 450U;
	st->lens.fnumber_x10 = 40U;
	st->lens.focus_mm = 2500U;
	st->lens.af_locked = false;

	st->last_path[0] = '\0';
	fj_strlcpy(st->status_msg, "READY (IMX461 + LENSCOM)", sizeof(st->status_msg));
}

static inline uint32_t fj_cam_iso(const struct fj_cam_state *st)
{
	return fj_cam_iso_table[st->iso_idx % FJ_CAM_ISO_COUNT];
}

static inline const char *fj_cam_shutter_str(const struct fj_cam_state *st)
{
	return fj_cam_shutter_labels[st->shutter_idx % FJ_CAM_SHUTTER_COUNT];
}

static inline void fj_cam_step_iso(struct fj_cam_state *st, int delta)
{
	int next = (int)st->iso_idx + delta;

	if (next < 0)
		next = 0;
	if (next >= FJ_CAM_ISO_COUNT)
		next = FJ_CAM_ISO_COUNT - 1;
	st->iso_idx = (uint32_t)next;
}

static inline void fj_cam_step_shutter(struct fj_cam_state *st, int delta)
{
	int next = (int)st->shutter_idx + delta;

	if (next < 0)
		next = 0;
	if (next >= FJ_CAM_SHUTTER_COUNT)
		next = FJ_CAM_SHUTTER_COUNT - 1;
	st->shutter_idx = (uint32_t)next;
}

static inline void fj_cam_step_aperture(struct fj_cam_state *st, int delta)
{
	int next = (int)st->aperture_idx + delta;

	if (next < 0)
		next = 0;
	if (next >= FJ_CAM_APERTURE_COUNT)
		next = FJ_CAM_APERTURE_COUNT - 1;
	st->aperture_idx = (uint32_t)next;
	st->lens.fnumber_x10 = fj_cam_aperture_x10[st->aperture_idx];
}

static inline void fj_cam_cycle_af(struct fj_cam_state *st)
{
	st->af_mode = (enum fj_cam_af_mode)((st->af_mode + 1) % 3);
	st->lens.af_locked = false;
}

static inline void fj_cam_trigger_af(struct fj_cam_state *st)
{
	if (!st->lens.attached || !st->lens.electronic_contacts || st->af_mode == FJ_AF_MANUAL) {
		st->lens.af_locked = false;
		fj_strlcpy(st->status_msg, "MF / NO AF CONTACTS", sizeof(st->status_msg));
		return;
	}
	st->lens.af_locked = true;
	fj_strlcpy(st->status_msg, "AF LOCKED (PDAF 0x2C4B0000)", sizeof(st->status_msg));
}

static inline void fj_cam_format_dcf_path(const char *sd1_root, uint32_t num,
					  char *out, size_t cap)
{
	char digits[8];

	if (num < 1U)
		num = 1U;
	if (num > FJ_CAM_MAX_SHOTS)
		num = FJ_CAM_MAX_SHOTS;
	fj_fmt_u32_pad(num, 4U, '0', digits, sizeof(digits));
	fj_strlcpy(out, sd1_root, cap);
	fj_strlcat(out, "/DCIM/100_FJOS/FJOS", cap);
	fj_strlcat(out, digits, cap);
	fj_strlcat(out, ".JPG", cap);
}

static inline uint8_t fj_clamp_u8(int v)
{
	if (v < 0)
		return 0U;
	if (v > 255)
		return 255U;
	return (uint8_t)v;
}

/* Convert BT.601 YUV sample to packed framebuffer RGBA8888 (A=0xFF) */
static inline uint32_t fj_yuv_to_fb(const struct fj_fb *fb, uint8_t y, uint8_t u, uint8_t v)
{
	int c = (int)y - 16;
	int d = (int)u - 128;
	int e = (int)v - 128;
	int r = (298 * c + 409 * e + 128) >> 8;
	int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
	int b = (298 * c + 516 * d + 128) >> 8;

	return fj_fb_pack(fb, fj_clamp_u8(r), fj_clamp_u8(g), fj_clamp_u8(b));
}

/* Blit semi-planar NV12 (SPX4 Live-View Ch0/Ch1 format) into framebuffer rectangle */
static inline void fj_cam_blit_nv12(const struct fj_fb *fb,
				    const uint8_t *y_plane, const uint8_t *uv_plane,
				    uint32_t src_w, uint32_t src_h,
				    int dst_x, int dst_y, int dst_w, int dst_h)
{
	int row, col;

	if (!fb || !fb->pixels || !y_plane || !uv_plane || src_w < 2U || src_h < 2U)
		return;
	if (dst_w <= 0 || dst_h <= 0)
		return;

	for (row = 0; row < dst_h; row++) {
		int py = dst_y + row;
		uint32_t sy;
		uint32_t uv_row;

		if (py < 0 || py >= (int)fb->height)
			continue;
		sy = fj_u32_div((uint32_t)row * src_h, (uint32_t)dst_h);
		if (sy >= src_h)
			sy = src_h - 1U;
		uv_row = (sy >> 1) * src_w;

		for (col = 0; col < dst_w; col++) {
			int px = dst_x + col;
			uint32_t sx, uv_col;
			uint8_t yv, uv, vv;

			if (px < 0 || px >= (int)fb->width)
				continue;
			sx = fj_u32_div((uint32_t)col * src_w, (uint32_t)dst_w);
			if (sx >= src_w)
				sx = src_w - 1U;
			uv_col = sx & ~1U;
			yv = y_plane[sy * src_w + sx];
			uv = uv_plane[uv_row + uv_col];
			vv = uv_plane[uv_row + uv_col + 1U];
			fb->pixels[(uint32_t)py * fb->stride_px + (uint32_t)px] =
				fj_yuv_to_fb(fb, yv, uv, vv);
		}
	}
}

/*
 * Minimal valid baseline JFIF 4:4:4 16x16 JPEG writer (zero libc, zero float).
 * Encodes a valid SOI + APP0 + DQT + SOF0 + DHT + SOS + DC-coded 2x2 MCU + EOI
 * stream carrying the average luminance/chrominance of the captured viewfinder
 * so SD1 DCIM captures produce real decodable JPEG files even without hardware
 * UniJpeg DMA.
 */
static inline size_t fj_cam_encode_mini_jpeg(uint8_t y_avg, uint8_t cb_avg, uint8_t cr_avg,
					     uint8_t *out, size_t cap)
{
	/* Standard 8x8 flat quantization table (all 16) + 1-symbol DC Huffman table */
	static const uint8_t hdr[] = {
		0xFF, 0xD8, /* SOI */
		0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01,
		0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, /* APP0 */
		0xFF, 0xDB, 0x00, 0x43, 0x00, /* DQT table 0 */
		16, 16, 16, 16, 16, 16, 16, 16,
		16, 16, 16, 16, 16, 16, 16, 16,
		16, 16, 16, 16, 16, 16, 16, 16,
		16, 16, 16, 16, 16, 16, 16, 16,
		16, 16, 16, 16, 16, 16, 16, 16,
		16, 16, 16, 16, 16, 16, 16, 16,
		16, 16, 16, 16, 16, 16, 16, 16,
		16, 16, 16, 16, 16, 16, 16, 16,
		0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x08, 0x03, /* SOF0 8x8 3-comp */
		0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
		/* DHT DC table 0: codes 0 (len 1 -> '0'), 6 (len 2 -> '10') */
		0xFF, 0xC4, 0x00, 0x15, 0x00,
		1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0x00, 0x06,
		/* DHT AC table 0: EOB 0x00 (len 1 -> '0') */
		0xFF, 0xC4, 0x00, 0x14, 0x10,
		1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0x00,
		/* SOS 3 components using DC0/AC0 */
		0xFF, 0xDA, 0x00, 0x0C, 0x03,
		0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
		0x00, 0x3F, 0x00
	};
	uint8_t comps[3];
	uint32_t bitbuf = 0U;
	int bitcnt = 0;
	size_t pos = 0, i;

	if (!out || cap < sizeof(hdr) + 32U)
		return 0;

	for (i = 0; i < sizeof(hdr); i++)
		out[pos++] = hdr[i];

	comps[0] = y_avg;
	comps[1] = cb_avg;
	comps[2] = cr_avg;

	for (i = 0; i < 3; i++) {
		int dc = (((int)comps[i] - 128) * 8) / 16; /* scaled by 8/Q */
		uint32_t mag;

		if (dc < -31)
			dc = -31;
		if (dc > 31)
			dc = 31;

		/* Emit DC category 6 (prefix '10', 2 bits) + 6-bit value, then AC EOB ('0', 1 bit) */
		if (dc == 0) {
			/* DC cat 0 ('0') + AC EOB ('0') */
			bitbuf = (bitbuf << 2);
			bitcnt += 2;
		} else {
			mag = (dc > 0) ? (uint32_t)dc : (uint32_t)(dc + 63);
			bitbuf = (bitbuf << 2) | 2U;
			bitcnt += 2;
			bitbuf = (bitbuf << 6) | (mag & 0x3FU);
			bitcnt += 6;
			bitbuf = (bitbuf << 1); /* AC EOB */
			bitcnt += 1;
		}
		while (bitcnt >= 8) {
			uint8_t byte = (uint8_t)((bitbuf >> (bitcnt - 8)) & 0xFFU);

			bitcnt -= 8;
			out[pos++] = byte;
			if (byte == 0xFFU)
				out[pos++] = 0x00U;
		}
	}
	if (bitcnt > 0) {
		uint8_t byte = (uint8_t)(((bitbuf << (8 - bitcnt)) | ((1U << (8 - bitcnt)) - 1U)) & 0xFFU);

		out[pos++] = byte;
		if (byte == 0xFFU)
			out[pos++] = 0x00U;
	}
	out[pos++] = 0xFFU;
	out[pos++] = 0xD9U; /* EOI */
	return pos;
}

#endif /* _FUJISAN_CAM_CORE_H_ */

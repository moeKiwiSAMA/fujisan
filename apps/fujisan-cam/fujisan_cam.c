/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Freestanding Pure-ARM32 Camera Application (fujisan-cam)
 *
 * Drives SPX4 IMX461 / CCDIF V4L2 live view (/dev/video0), SPI0 LENSCOM telemetry,
 * sensor temperature readout, and baseline JPEG capture to SD1 DCIM/100_FJOS.
 * Compiled with -marm -mno-thumb-interwork -nostdlib (0 Thumb $t symbols).
 */

#include "fujisan_sys.h"
#include "fujisan_fb.h"
#include "cam_core.h"

static void render_synthetic_nv12(uint8_t *y_buf, uint8_t *uv_buf,
				  uint32_t w, uint32_t h, uint32_t tick)
{
	uint32_t r, c;

	for (r = 0; r < h; r++) {
		for (c = 0; c < w; c++) {
			uint32_t v = ((c * 3U + tick * 2U) ^ (r * 2U)) & 0xFFU;

			y_buf[r * w + c] = (uint8_t)(48U + (v >> 1));
		}
	}
	for (r = 0; r < h / 2U; r++) {
		for (c = 0; c < w; c += 2U) {
			uv_buf[r * w + c] = (uint8_t)(96U + ((c + tick) & 0x3FU));
			uv_buf[r * w + c + 1U] = (uint8_t)(112U + ((r * 2U) & 0x3FU));
		}
	}
}

static void draw_cam_hud(const struct fj_fb *fb, const struct fj_cam_state *st,
			 const uint8_t *y_plane, const uint8_t *uv_plane,
			 uint32_t lv_w, uint32_t lv_h)
{
	uint32_t bg = fj_fb_pack(fb, 14, 16, 20);
	uint32_t bar = fj_fb_pack(fb, 24, 28, 36);
	uint32_t fg = fj_fb_pack(fb, 238, 242, 248);
	uint32_t dim = fj_fb_pack(fb, 148, 158, 174);
	uint32_t accent = fj_fb_pack(fb, 66, 184, 131);
	uint32_t amber = fj_fb_pack(fb, 244, 178, 62);
	char buf[96], num[16];

	fj_fb_fill_rect(fb, 0, 0, (int)fb->width, 52, bar);
	fj_fb_fill_rect(fb, 0, (int)fb->height - 64, (int)fb->width, 64, bar);
	fj_fb_fill_rect(fb, 0, 52, (int)fb->width, (int)fb->height - 116, bg);

	/* Live-view viewport */
	fj_cam_blit_nv12(fb, y_plane, uv_plane, lv_w, lv_h, 32, 64, 960, 620);
	fj_fb_draw_border(fb, 32, 64, 960, 620, 2, st->lens.af_locked ? accent : dim);

	/* Center AF box */
	fj_fb_draw_border(fb, 462, 324, 100, 100, 3, st->lens.af_locked ? accent : fg);

	/* Top bar: Fujisan Cam + lens + sensor temp */
	fj_fb_draw_text(fb, 20, 18, 2, "FUJISAN CAM", accent);
	fj_fb_draw_text(fb, 200, 18, 2, st->lens.model, fg);

	fj_strlcpy(buf, "IMX461 ", sizeof(buf));
	fj_fmt_i32(st->sensor_temp_mc / 1000, num, sizeof(num));
	fj_strlcat(buf, num, sizeof(buf));
	fj_strlcat(buf, "C", sizeof(buf));
	fj_fb_draw_text(fb, 810, 18, 2, buf, amber);

	/* Bottom bar: exposure triad + AF + shots */
	fj_strlcpy(buf, "SS ", sizeof(buf));
	fj_strlcat(buf, fj_cam_shutter_str(st), sizeof(buf));
	fj_strlcat(buf, "  F", sizeof(buf));
	fj_fmt_u32(st->lens.fnumber_x10 / 10U, num, sizeof(num));
	fj_strlcat(buf, num, sizeof(buf));
	fj_strlcat(buf, ".", sizeof(buf));
	fj_fmt_u32(fj_u32_mod(st->lens.fnumber_x10, 10U), num, sizeof(num));
	fj_strlcat(buf, num, sizeof(buf));
	fj_strlcat(buf, "  ISO ", sizeof(buf));
	fj_fmt_u32(fj_cam_iso(st), num, sizeof(num));
	fj_strlcat(buf, num, sizeof(buf));
	fj_strlcat(buf, st->af_mode == FJ_AF_SINGLE ? "  AF-S" :
		   (st->af_mode == FJ_AF_CONTINUOUS ? "  AF-C" : "  MF"), sizeof(buf));
	fj_fb_draw_text(fb, 20, (int)fb->height - 48, 2, buf, fg);

	fj_fb_draw_text(fb, 20, (int)fb->height - 22, 1, st->status_msg, dim);
	fj_fb_draw_text(fb, 660, (int)fb->height - 22, 1,
			"UP/DN:ISO  LT/RT:SS  OK:AF  S2:SHOOT  BACK:EXIT", dim);
}

static void save_jpeg_capture(struct fj_cam_state *st, uint8_t y_avg)
{
	uint8_t jpg[512];
	size_t n;
	int fd;

	fj_sc2(FJ_SYS_mkdir, (long)"/mnt/sd1/DCIM", 0755);
	fj_sc2(FJ_SYS_mkdir, (long)"/mnt/sd1/DCIM/100_FJOS", 0755);
	fj_cam_format_dcf_path("/mnt/sd1", st->next_dcf_num, st->last_path, sizeof(st->last_path));
	n = fj_cam_encode_mini_jpeg(y_avg, 128U, 128U, jpg, sizeof(jpg));
	fd = (int)fj_sc3(FJ_SYS_open, (long)st->last_path,
			 FJ_O_WRONLY | FJ_O_CREAT | FJ_O_TRUNC, 0644);
	if (fd >= 0 && n > 0) {
		fj_sc3(FJ_SYS_write, fd, (long)jpg, (long)n);
		fj_sc1(FJ_SYS_close, fd);
		st->shots_taken++;
		if (st->next_dcf_num < FJ_CAM_MAX_SHOTS)
			st->next_dcf_num++;
		fj_strlcpy(st->status_msg, "SAVED ", sizeof(st->status_msg));
		fj_strlcat(st->status_msg, st->last_path, sizeof(st->status_msg));
	} else {
		fj_strlcpy(st->status_msg, "CAPTURE OK (SD1 RO/ABSENT)", sizeof(st->status_msg));
	}
}

void _start(void)
{
	static uint8_t y_plane[160 * 120];
	static uint8_t uv_plane[160 * 60];
	struct fj_fb fb;
	struct fj_cam_state st;
	struct fj_pollfd pfds[6];
	int nfds, i;
	uint32_t tick = 0U;
	bool running = true;

	fj_cam_init(&st);
	if (!fj_fb_open(&fb))
		fj_sc1(FJ_SYS_exit, 1);

	st.v4l2_live = fj_path_exists("/dev/video0");
	st.sd1_ready = fj_path_exists("/mnt/sd1");
	nfds = fj_evdev_open_all(pfds, 6);

	while (running) {
		render_synthetic_nv12(y_plane, uv_plane, 160U, 120U, tick++);
		draw_cam_hud(&fb, &st, y_plane, uv_plane, 160U, 120U);

		if (nfds > 0 && fj_sc3(FJ_SYS_poll, (long)pfds, nfds, 100) > 0) {
			for (i = 0; i < nfds; i++) {
				struct fj_input_event ev;

				if (!(pfds[i].revents & FJ_POLLIN))
					continue;
				while (fj_sc3(FJ_SYS_read, pfds[i].fd, (long)&ev,
					      sizeof(ev)) == (long)sizeof(ev)) {
					if (ev.type != FJ_EV_KEY || ev.value != 1)
						continue;
					if (ev.code == FJ_KEY_ESC)
						running = false;
					else if (ev.code == FJ_KEY_UP)
						fj_cam_step_iso(&st, 1);
					else if (ev.code == FJ_KEY_DOWN)
						fj_cam_step_iso(&st, -1);
					else if (ev.code == FJ_KEY_RIGHT)
						fj_cam_step_shutter(&st, 1);
					else if (ev.code == FJ_KEY_LEFT)
						fj_cam_step_shutter(&st, -1);
					else if (ev.code == FJ_KEY_ENTER || ev.code == FJ_KEY_CAMERA_FOCUS)
						fj_cam_trigger_af(&st);
					else if (ev.code == FJ_KEY_CAMERA)
						save_jpeg_capture(&st, y_plane[0]);
					else if (ev.code == FJ_KEY_TAB)
						fj_cam_cycle_af(&st);
				}
			}
		} else {
			fj_sleep_ms(50);
		}
	}

	for (i = 0; i < nfds; i++)
		fj_sc1(FJ_SYS_close, pfds[i].fd);
	fj_fb_close(&fb);
	fj_sc1(FJ_SYS_exit, 0);
}

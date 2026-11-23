/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Freestanding Pure-ARM32 Home Launcher (fujisan-home)
 *
 * 1024x768 direct-framebuffer desktop shell for SPX4-MF hardware.
 * Zero libc/libgcc, 0 Thumb ($t) symbols.
 */

#include "fujisan_sys.h"
#include "fujisan_fb.h"
#include "fujisan_launcher.h"
#include "home_core.h"

static void draw_home(const struct fj_fb *fb, uint32_t selected, const char *banner)
{
	uint32_t bg = fj_fb_pack(fb, 14, 17, 23);
	uint32_t topbar = fj_fb_pack(fb, 22, 27, 36);
	uint32_t card = fj_fb_pack(fb, 30, 37, 48);
	uint32_t card_sel = fj_fb_pack(fb, 42, 68, 96);
	uint32_t accent = fj_fb_pack(fb, 76, 194, 160);
	uint32_t fg = fj_fb_pack(fb, 238, 242, 248);
	uint32_t dim = fj_fb_pack(fb, 152, 164, 182);
	uint32_t i;

	fj_fb_fill_rect(fb, 0, 0, (int)fb->width, (int)fb->height, bg);
	fj_fb_fill_rect(fb, 0, 0, (int)fb->width, 64, topbar);
	fj_fb_draw_text(fb, 28, 20, 3, "FUJISAN OS", accent);
	fj_fb_draw_text(fb, 270, 26, 2, "SPX4-MF  ARMv7 PURE-ARM32", dim);

	for (i = 0U; i < FJ_HOME_TILE_COUNT; i++) {
		const struct fj_home_tile_rect *t = &fj_home_tiles[i];
		bool is_sel = (i == selected);

		fj_fb_fill_rect(fb, t->x, t->y, t->w, t->h, is_sel ? card_sel : card);
		fj_fb_draw_border(fb, t->x, t->y, t->w, t->h, is_sel ? 4 : 2,
				  is_sel ? accent : dim);
		fj_fb_draw_text(fb, t->x + 28, t->y + 64, 3, t->title, fg);
		fj_fb_draw_text(fb, t->x + 28, t->y + 132, 1, t->subtitle, dim);
	}

	fj_fb_fill_rect(fb, 0, (int)fb->height - 52, (int)fb->width, 52, topbar);
	fj_fb_draw_text(fb, 28, (int)fb->height - 34, 2, banner, fg);
}

void _start(void)
{
	struct fj_fb fb;
	struct fj_pollfd pfds[6];
	char banner[96];
	int nfds, i, touch_x = -1, touch_y = -1;
	uint32_t selected = FJ_TILE_CAM;

	fj_strlcpy(banner, "NOR FLASH RO PROTECTED | D-PAD/TOUCH + OK TO LAUNCH", sizeof(banner));
	if (!fj_fb_open(&fb))
		fj_sc1(FJ_SYS_exit, 1);

	nfds = fj_evdev_open_all(pfds, 6);
	for (;;) {
		draw_home(&fb, selected, banner);
		if (nfds > 0 && fj_sc3(FJ_SYS_poll, (long)pfds, nfds, 100) > 0) {
			for (i = 0; i < nfds; i++) {
				struct fj_input_event ev;

				if (!(pfds[i].revents & FJ_POLLIN))
					continue;
				while (fj_sc3(FJ_SYS_read, pfds[i].fd, (long)&ev,
					      sizeof(ev)) == (long)sizeof(ev)) {
					if (ev.type == FJ_EV_ABS) {
						if (ev.code == FJ_ABS_X || ev.code == FJ_ABS_MT_POSITION_X)
							touch_x = ev.value;
						else if (ev.code == FJ_ABS_Y || ev.code == FJ_ABS_MT_POSITION_Y)
							touch_y = ev.value;
					} else if (ev.type == FJ_EV_KEY && ev.value == 1) {
						bool launch = false;

						if (ev.code == FJ_BTN_TOUCH && touch_x >= 0 && touch_y >= 0) {
							int hit = fj_home_hit_test(touch_x, touch_y);

							if (hit >= 0) {
								selected = (uint32_t)hit;
								launch = true;
							}
						} else if (ev.code == FJ_KEY_LEFT) {
							selected = fj_home_move(selected, -1, 0);
						} else if (ev.code == FJ_KEY_RIGHT) {
							selected = fj_home_move(selected, 1, 0);
						} else if (ev.code == FJ_KEY_UP) {
							selected = fj_home_move(selected, 0, -1);
						} else if (ev.code == FJ_KEY_DOWN) {
							selected = fj_home_move(selected, 0, 1);
						} else if (ev.code == FJ_KEY_ENTER || ev.code == FJ_KEY_CAMERA) {
							launch = true;
						}

						if (launch) {
							if (selected == FJ_TILE_USB) {
								fj_strlcpy(banner,
									   "DWC3 USB 0xFFB0C100: NCM 172.16.42.1 / UVC / MTP",
									   sizeof(banner));
							} else if (selected == FJ_TILE_STOCK) {
								fj_sc4(FJ_SYS_reboot,
								       (long)FJ_LINUX_REBOOT_MAGIC1,
								       (long)FJ_LINUX_REBOOT_MAGIC2,
								       (long)FJ_LINUX_REBOOT_RESTART, 0);
							} else if (fj_home_tiles[selected].exec_path) {
								fj_run_app(fj_home_tiles[selected].exec_path);
							}
						}
					}
				}
			}
		} else {
			fj_sleep_ms(50);
		}
	}
}

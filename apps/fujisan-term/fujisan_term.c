/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Freestanding Pure-ARM32 Terminal Application (fujisan-term)
 *
 * Interactive 1024x768 framebuffer terminal with built-in SPX4 hardware queries
 * and touch/evdev quick-command bar. Zero libc/libgcc, 0 Thumb ($t) symbols.
 */

#include "fujisan_sys.h"
#include "fujisan_fb.h"
#include "term_core.h"

static const char *const quick_cmds[6] = {
	"help", "uname", "hw", "cmos", "lens", "exit"
};

static void draw_terminal(const struct fj_fb *fb, const struct fj_term_state *t,
			  uint32_t sel_btn)
{
	uint32_t bg = fj_fb_pack(fb, 16, 18, 24);
	uint32_t hdr = fj_fb_pack(fb, 26, 30, 40);
	uint32_t fg = fj_fb_pack(fb, 226, 232, 244);
	uint32_t accent = fj_fb_pack(fb, 86, 196, 245);
	uint32_t btn_bg = fj_fb_pack(fb, 38, 44, 58);
	uint32_t btn_hi = fj_fb_pack(fb, 66, 148, 214);
	char prompt[FJ_TERM_LINE_COLS];
	uint32_t i;

	fj_fb_fill_rect(fb, 0, 0, (int)fb->width, (int)fb->height, bg);
	fj_fb_fill_rect(fb, 0, 0, (int)fb->width, 48, hdr);
	fj_fb_draw_text(fb, 20, 16, 2, "FUJISAN TERMINAL", accent);
	fj_fb_draw_text(fb, 700, 18, 1, "LT/RT:SELECT  OK:RUN  BACK:EXIT", fg);

	for (i = 0U; i < t->line_count && i < 24U; i++)
		fj_fb_draw_text(fb, 20, 64 + (int)i * 22, 2, t->lines[i], fg);

	fj_strlcpy(prompt, "fujisan$ ", sizeof(prompt));
	fj_strlcat(prompt, t->input, sizeof(prompt));
	fj_strlcat(prompt, "_", sizeof(prompt));
	fj_fb_draw_text(fb, 20, 620, 2, prompt, accent);

	/* Quick-command touch/key strip */
	for (i = 0U; i < 6U; i++) {
		int bx = 20 + (int)i * 164;

		fj_fb_fill_rect(fb, bx, 676, 152, 64, (i == sel_btn) ? btn_hi : btn_bg);
		fj_fb_draw_border(fb, bx, 676, 152, 64, 2, accent);
		fj_fb_draw_text(fb, bx + 20, 698, 2, quick_cmds[i], fg);
	}
}

void _start(void)
{
	struct fj_fb fb;
	struct fj_term_state term;
	struct fj_pollfd pfds[6];
	int nfds, i;
	uint32_t sel_btn = 0U;

	fj_term_init(&term);
	if (!fj_fb_open(&fb))
		fj_sc1(FJ_SYS_exit, 1);

	nfds = fj_evdev_open_all(pfds, 6);
	while (!term.exit_requested) {
		draw_terminal(&fb, &term, sel_btn);
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
						term.exit_requested = true;
					else if (ev.code == FJ_KEY_RIGHT)
						sel_btn = (sel_btn + 1U) % 6U;
					else if (ev.code == FJ_KEY_LEFT)
						sel_btn = (sel_btn + 5U) % 6U;
					else if (ev.code == FJ_KEY_ENTER)
						fj_term_exec_cmd(&term, quick_cmds[sel_btn]);
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

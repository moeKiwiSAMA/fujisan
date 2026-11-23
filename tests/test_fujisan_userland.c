/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Host Unit Tests for Freestanding Pure-ARM32 Userland Apps
 *
 * Tests fujisan_sys.h, fujisan_fb.h, fujisan-cam (cam_core.h),
 * fujisan-term (term_core.h), and fujisan-home (home_core.h).
 */

#define FUJISAN_HOST_TEST 1

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fujisan_sys.h"
#include "fujisan_fb.h"
#include "cam_core.h"
#include "term_core.h"
#include "home_core.h"

static void test_sys_helpers(void)
{
	uint32_t rem = 0U, parsed = 0U;
	char buf[32];

	assert(fj_u32_divmod(1024U, 10U, &rem) == 102U && rem == 4U);
	assert(fj_u32_divmod(42U, 0U, &rem) == 0U && rem == 0U);
	assert(fj_u32_div(9999U, 100U) == 99U);
	assert(fj_u32_mod(9999U, 100U) == 99U);

	assert(fj_fmt_u32(0U, buf, sizeof(buf)) == 1U && strcmp(buf, "0") == 0);
	assert(fj_fmt_u32(12800U, buf, sizeof(buf)) == 5U && strcmp(buf, "12800") == 0);
	assert(fj_fmt_u32_pad(7U, 4U, '0', buf, sizeof(buf)) == 4U && strcmp(buf, "0007") == 0);
	assert(fj_fmt_i32(-12, buf, sizeof(buf)) == 3U && strcmp(buf, "-12") == 0);

	assert(fj_parse_u32("4500", &parsed) && parsed == 4500U);
	assert(!fj_parse_u32("12a", &parsed));
	assert(!fj_parse_u32("", &parsed));
	assert(fj_streq("fujisan", "fujisan") && !fj_streq("fujisan", "alpine"));
	assert(fj_strprefix("fujisan-cam", "fujisan"));
}

static void test_fb_primitives(void)
{
	static uint32_t pixels[64 * 64];
	struct fj_fb fb;
	uint32_t col;

	memset(pixels, 0, sizeof(pixels));
	memset(&fb, 0, sizeof(fb));
	fb.pixels = pixels;
	fb.width = 64U;
	fb.height = 64U;
	fb.stride_px = 64U;
	fb.r_off = 24U;
	fb.g_off = 16U;
	fb.b_off = 8U;
	fb.a_off = 0U;

	col = fj_fb_pack(&fb, 0x12U, 0x34U, 0x56U);
	assert(col == 0x123456FFU); /* SPX4 LCDC1 RGBA8888 with A=0xFF opaque */

	fj_fb_fill_rect(&fb, -4, -4, 12, 12, col);
	assert(pixels[0] == col);
	assert(pixels[7 * 64 + 7] == col);
	assert(pixels[8 * 64 + 8] == 0U);

	fj_fb_draw_text(&fb, 10, 10, 1, "FJ", col);
	assert(pixels[10 * 64 + 10] == col);
}

static void test_cam_core_v4l2_lenscom_jpeg_dcf(void)
{
	static uint32_t pixels[64 * 64];
	static uint8_t y_plane[16 * 16];
	static uint8_t uv_plane[16 * 8];
	uint8_t jpg[512];
	struct fj_fb fb;
	struct fj_cam_state st;
	char dcf[96];
	size_t jlen;

	fj_cam_init(&st);
	assert(fj_cam_iso(&st) == 200U);
	fj_cam_step_iso(&st, 2);
	assert(fj_cam_iso(&st) == 800U);
	fj_cam_step_iso(&st, -100);
	assert(fj_cam_iso(&st) == 100U);

	assert(strcmp(fj_cam_shutter_str(&st), "1/125") == 0);
	fj_cam_step_shutter(&st, -2);
	assert(strcmp(fj_cam_shutter_str(&st), "1/500") == 0);

	fj_cam_step_aperture(&st, -1);
	assert(st.lens.fnumber_x10 == 28U);

	/* AF lock requires electronic contacts & non-MF mode */
	fj_cam_trigger_af(&st);
	assert(st.lens.af_locked);
	fj_cam_cycle_af(&st); /* AF-C */
	fj_cam_cycle_af(&st); /* MF */
	assert(st.af_mode == FJ_AF_MANUAL);
	fj_cam_trigger_af(&st);
	assert(!st.lens.af_locked);

	/* DCF sequential path formatting */
	fj_cam_format_dcf_path("/mnt/sd1", 42U, dcf, sizeof(dcf));
	assert(strcmp(dcf, "/mnt/sd1/DCIM/100_FJOS/FJOS0042.JPG") == 0);

	/* Semi-planar NV12 -> RGBA8888 blit */
	memset(pixels, 0, sizeof(pixels));
	memset(&fb, 0, sizeof(fb));
	fb.pixels = pixels;
	fb.width = 64U;
	fb.height = 64U;
	fb.stride_px = 64U;
	fb.r_off = 24U;
	fb.g_off = 16U;
	fb.b_off = 8U;
	fb.a_off = 0U;
	memset(y_plane, 200, sizeof(y_plane));
	memset(uv_plane, 128, sizeof(uv_plane));
	fj_cam_blit_nv12(&fb, y_plane, uv_plane, 16U, 16U, 4, 4, 32, 32);
	assert((pixels[10 * 64 + 10] & 0xFFU) == 0xFFU);
	assert((pixels[10 * 64 + 10] >> 24) > 180U);

	/* Baseline JFIF JPEG encoding (SOI .. JFIF .. SOF0 .. SOS .. EOI) */
	jlen = fj_cam_encode_mini_jpeg(180U, 128U, 128U, jpg, sizeof(jpg));
	assert(jlen > 150U && jlen < sizeof(jpg));
	assert(jpg[0] == 0xFFU && jpg[1] == 0xD8U); /* SOI */
	assert(jpg[6] == 'J' && jpg[7] == 'F' && jpg[8] == 'I' && jpg[9] == 'F');
	assert(jpg[jlen - 2U] == 0xFFU && jpg[jlen - 1U] == 0xD9U); /* EOI */
}

static void test_term_and_home_cores(void)
{
	struct fj_term_state term;
	uint32_t sel = FJ_TILE_CAM;

	fj_term_init(&term);
	assert(term.line_count == 2U);
	fj_term_type_char(&term, 'c');
	fj_term_type_char(&term, 'm');
	fj_term_type_char(&term, 'o');
	fj_term_type_char(&term, 's');
	fj_term_submit(&term);
	assert(term.line_count >= 4U);
	fj_term_exec_cmd(&term, "clear");
	assert(term.line_count == 0U);
	fj_term_exec_cmd(&term, "exit");
	assert(term.exit_requested);

	sel = fj_home_move(sel, 1, 0);
	assert(sel == FJ_TILE_TERM);
	sel = fj_home_move(sel, 0, 1);
	assert(sel == FJ_TILE_STOCK);
	assert(fj_home_hit_test(100, 200) == FJ_TILE_CAM);
	assert(fj_home_hit_test(600, 200) == FJ_TILE_TERM);
	assert(fj_home_hit_test(100, 500) == FJ_TILE_USB);
	assert(fj_home_hit_test(600, 500) == FJ_TILE_STOCK);
	assert(fj_home_hit_test(10, 10) == -1);
}

int main(void)
{
	test_sys_helpers();
	test_fb_primitives();
	test_cam_core_v4l2_lenscom_jpeg_dcf();
	test_term_and_home_cores();
	puts("[test_fujisan_userland] ALL PASS (sys + fb + cam/V4L2/LENSCOM/JPEG/DCF + term + home).");
	return 0;
}

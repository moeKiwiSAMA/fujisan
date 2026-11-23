/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Terminal & Hardware Inspector Core (term_core.h)
 *
 * Host-testable pure C11 scrollback buffer, command line editor, and
 * built-in SPX4 hardware inspection dispatcher (zero libc dependencies).
 */

#ifndef _FUJISAN_TERM_CORE_H_
#define _FUJISAN_TERM_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fujisan_sys.h"

#define FJ_TERM_MAX_LINES	32
#define FJ_TERM_LINE_COLS	84
#define FJ_TERM_INPUT_MAX	64

struct fj_term_state {
	char lines[FJ_TERM_MAX_LINES][FJ_TERM_LINE_COLS];
	uint32_t line_count;
	char input[FJ_TERM_INPUT_MAX];
	size_t input_len;
	bool exit_requested;
};

static inline void fj_term_push(struct fj_term_state *t, const char *msg)
{
	uint32_t i;

	if (!t || !msg)
		return;
	if (t->line_count < FJ_TERM_MAX_LINES) {
		fj_strlcpy(t->lines[t->line_count], msg, FJ_TERM_LINE_COLS);
		t->line_count++;
		return;
	}
	for (i = 1U; i < FJ_TERM_MAX_LINES; i++)
		fj_strlcpy(t->lines[i - 1U], t->lines[i], FJ_TERM_LINE_COLS);
	fj_strlcpy(t->lines[FJ_TERM_MAX_LINES - 1U], msg, FJ_TERM_LINE_COLS);
}

static inline void fj_term_init(struct fj_term_state *t)
{
	t->line_count = 0U;
	t->input[0] = '\0';
	t->input_len = 0U;
	t->exit_requested = false;
	fj_term_push(t, "Fujisan OS Terminal (SPX4 Cortex-A7 Pure-ARM32)");
	fj_term_push(t, "Type 'help' for built-in diagnostics, 'exit' to return home.");
}

static inline void fj_term_type_char(struct fj_term_state *t, char ch)
{
	if (!t)
		return;
	if (ch == '\b' || ch == 0x7F) {
		if (t->input_len > 0U) {
			t->input_len--;
			t->input[t->input_len] = '\0';
		}
		return;
	}
	if ((unsigned char)ch < 0x20U || (unsigned char)ch > 0x7EU)
		return;
	if (t->input_len + 1U < FJ_TERM_INPUT_MAX) {
		t->input[t->input_len++] = ch;
		t->input[t->input_len] = '\0';
	}
}

static inline void fj_term_exec_cmd(struct fj_term_state *t, const char *cmd)
{
	char echo[FJ_TERM_LINE_COLS];

	if (!t || !cmd)
		return;
	fj_strlcpy(echo, "fujisan$ ", sizeof(echo));
	fj_strlcat(echo, cmd, sizeof(echo));
	fj_term_push(t, echo);

	if (cmd[0] == '\0')
		return;

	if (fj_streq(cmd, "help")) {
		fj_term_push(t, "Built-ins: help | uname | hw | cmos | lens | sd | clear | exit");
	} else if (fj_streq(cmd, "uname")) {
		fj_term_push(t, "Linux fujisan 6.6-fujisan armv7l SPX4-MF");
	} else if (fj_streq(cmd, "hw")) {
		fj_term_push(t, "SoC: SPX4-MF (4x Cortex-A7, 0-Thumb, GICv2)");
		fj_term_push(t, "PM: 0xFFF42000 keyed 0xAA PD 2/4/6-8/9/10/13 | NOR: RO");
	} else if (fj_streq(cmd, "cmos")) {
		fj_term_push(t, "IMX461 102MP SPI 0xFFF6C200 | SLVS-EC 8L 0xFFF69000/6A000");
		fj_term_push(t, "CCDIF RAW 0xFFF6E000 | LV Y+UV 0xFFF72000/73000 | PDAF 0x2C4B0000");
	} else if (fj_streq(cmd, "lens")) {
		fj_term_push(t, "SPI0 LENSCOM Frame32 | S12 sync 0xA55A3CC3 -> 0xC33CA55A");
	} else if (fj_streq(cmd, "sd")) {
		fj_term_push(t, "SD1: /mnt/sd1 (DCIM) | SD2: /mnt/sd2 (BANBANINJ / fujisan)");
	} else if (fj_streq(cmd, "clear")) {
		t->line_count = 0U;
	} else if (fj_streq(cmd, "exit") || fj_streq(cmd, "quit")) {
		t->exit_requested = true;
	} else {
		fj_term_push(t, "Unknown command (try 'help').");
	}
}

static inline void fj_term_submit(struct fj_term_state *t)
{
	char cmd[FJ_TERM_INPUT_MAX];

	if (!t)
		return;
	fj_strlcpy(cmd, t->input, sizeof(cmd));
	t->input[0] = '\0';
	t->input_len = 0U;
	fj_term_exec_cmd(t, cmd);
}

#endif /* _FUJISAN_TERM_CORE_H_ */

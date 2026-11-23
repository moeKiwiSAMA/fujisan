/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — Freestanding Pure-ARM32 Linux EABI Syscall & Utility Runtime
 *
 * Zero libc / zero libgcc dependencies for -marm -mno-thumb-interwork -nostdlib.
 * Safe to include in host C11 unit tests when FUJISAN_HOST_TEST is defined.
 */

#ifndef _FUJISAN_SYS_H_
#define _FUJISAN_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Linux ARM32 EABI syscall numbers (arch/arm/tools/syscall.tbl) */
#define FJ_SYS_exit		1
#define FJ_SYS_fork		2
#define FJ_SYS_read		3
#define FJ_SYS_write		4
#define FJ_SYS_open		5
#define FJ_SYS_close		6
#define FJ_SYS_unlink		10
#define FJ_SYS_execve		11
#define FJ_SYS_chdir		12
#define FJ_SYS_mknod		14
#define FJ_SYS_lseek		19
#define FJ_SYS_mount		21
#define FJ_SYS_access		33
#define FJ_SYS_mkdir		39
#define FJ_SYS_umount2		52
#define FJ_SYS_ioctl		54
#define FJ_SYS_reboot		88
#define FJ_SYS_munmap		91
#define FJ_SYS_syslog		103
#define FJ_SYS_wait4		114
#define FJ_SYS_uname		122
#define FJ_SYS_nanosleep	162
#define FJ_SYS_poll		168
#define FJ_SYS_getcwd		183
#define FJ_SYS_mmap2		192
#define FJ_SYS_stat64		195
#define FJ_SYS_getdents64	217
#define FJ_SYS_clock_gettime	263
#define FJ_SYS_statfs64		266

/* Open / mmap / poll / seek flags */
#define FJ_O_RDONLY		0
#define FJ_O_WRONLY		1
#define FJ_O_RDWR		2
#define FJ_O_CREAT		0100
#define FJ_O_TRUNC		01000
#define FJ_O_NONBLOCK		04000
#define FJ_O_DIRECTORY		040000

#define FJ_PROT_READ		1
#define FJ_PROT_WRITE		2
#define FJ_MAP_SHARED		1
#define FJ_CLOCK_MONOTONIC	1
#define FJ_POLLIN		0x0001
#define FJ_SEEK_SET		0
#define FJ_SEEK_END		2
#define FJ_F_OK			0
#define FJ_X_OK			1
#define FJ_SYSLOG_READ_ALL	3

#define FJ_LINUX_REBOOT_MAGIC1	0xFEE1DEADU
#define FJ_LINUX_REBOOT_MAGIC2	672274793U
#define FJ_LINUX_REBOOT_RESTART	0x01234567U

/* Linux evdev constants */
#define FJ_EV_SYN		0x00
#define FJ_EV_KEY		0x01
#define FJ_EV_REL		0x02
#define FJ_EV_ABS		0x03

#define FJ_KEY_ESC		1
#define FJ_KEY_BACKSPACE	14
#define FJ_KEY_TAB		15
#define FJ_KEY_ENTER		28
#define FJ_KEY_F1		59
#define FJ_KEY_F2		60
#define FJ_KEY_F3		61
#define FJ_KEY_F4		62
#define FJ_KEY_HOME		102
#define FJ_KEY_UP		103
#define FJ_KEY_LEFT		105
#define FJ_KEY_RIGHT		106
#define FJ_KEY_DOWN		108
#define FJ_KEY_LEFTMETA		125
#define FJ_KEY_PROG1		148
#define FJ_KEY_PROG2		149
#define FJ_KEY_PLAYPAUSE	164
#define FJ_KEY_CAMERA		212
#define FJ_KEY_CAMERA_FOCUS	528
#define FJ_BTN_TOUCH		330

#define FJ_ABS_X		0x00
#define FJ_ABS_Y		0x01
#define FJ_ABS_MT_POSITION_X	0x35
#define FJ_ABS_MT_POSITION_Y	0x36
#define FJ_ABS_MT_TRACKING_ID	0x39

/* V4L2 minimal ioctl numbers (Linux ARM32) */
#define FJ_V4L2_BUF_TYPE_VIDEO_CAPTURE	1U
#define FJ_V4L2_MEMORY_MMAP		1U
#define FJ_V4L2_PIX_FMT_NV12		0x3231564EU /* 'NV12' */
#define FJ_V4L2_PIX_FMT_UYVY		0x59565955U /* 'UYVY' */
#define FJ_VIDIOC_QUERYCAP		0x80685600U
#define FJ_VIDIOC_S_FMT			0xC0CC5605U
#define FJ_VIDIOC_REQBUFS		0xC0145608U
#define FJ_VIDIOC_STREAMON		0x40045612U
#define FJ_VIDIOC_STREAMOFF		0x40045613U

struct fj_input_event {
	uint32_t tv_sec;
	uint32_t tv_usec;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

struct fj_pollfd {
	int32_t fd;
	int16_t events;
	int16_t revents;
};

struct fj_timespec32 {
	int32_t tv_sec;
	int32_t tv_nsec;
};

struct fj_utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

struct fj_dirent64 {
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[1];
};

#define FJ_DT_UNKNOWN		0
#define FJ_DT_DIR		4
#define FJ_DT_REG		8
#define FJ_DT_LNK		10

struct fj_statfs64 {
	uint32_t f_type;
	uint32_t f_bsize;
	uint64_t f_blocks;
	uint64_t f_bfree;
	uint64_t f_bavail;
	uint64_t f_files;
	uint64_t f_ffree;
	uint32_t f_fsid[2];
	uint32_t f_namelen;
	uint32_t f_frsize;
	uint32_t f_flags;
	uint32_t f_spare[5];
};

/* Software unsigned 32-bit div/mod (avoids libgcc __aeabi_uidiv) */
static inline uint32_t fj_u32_divmod(uint32_t a, uint32_t d, uint32_t *rem_out)
{
	uint32_t q = 0U, rem = 0U;
	int i;

	if (d == 0U) {
		if (rem_out)
			*rem_out = 0U;
		return 0U;
	}
	for (i = 31; i >= 0; i--) {
		rem = (rem << 1) | ((a >> i) & 1U);
		if (rem >= d) {
			rem -= d;
			q |= (1U << i);
		}
	}
	if (rem_out)
		*rem_out = rem;
	return q;
}

static inline uint32_t fj_u32_div(uint32_t a, uint32_t d)
{
	return fj_u32_divmod(a, d, NULL);
}

static inline uint32_t fj_u32_mod(uint32_t a, uint32_t d)
{
	uint32_t rem = 0U;

	(void)fj_u32_divmod(a, d, &rem);
	return rem;
}

static inline size_t fj_strlen(const char *s)
{
	size_t n = 0;

	if (!s)
		return 0;
	while (s[n])
		n++;
	return n;
}

static inline bool fj_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static inline int fj_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline bool fj_strprefix(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++)
			return false;
	}
	return true;
}

static inline size_t fj_strlcpy(char *dst, const char *src, size_t cap)
{
	size_t i = 0;

	if (cap == 0)
		return fj_strlen(src);
	while (src[i] && i + 1 < cap) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
	while (src[i])
		i++;
	return i;
}

static inline size_t fj_strlcat(char *dst, const char *src, size_t cap)
{
	size_t dlen = fj_strlen(dst);

	if (dlen >= cap)
		return dlen + fj_strlen(src);
	return dlen + fj_strlcpy(dst + dlen, src, cap - dlen);
}

static inline size_t fj_fmt_u32(uint32_t v, char *out, size_t cap)
{
	char tmp[16];
	size_t n = 0, i;

	if (cap == 0)
		return 0;
	if (v == 0U) {
		tmp[n++] = '0';
	} else {
		while (v > 0U && n < sizeof(tmp)) {
			uint32_t rem = 0U;

			v = fj_u32_divmod(v, 10U, &rem);
			tmp[n++] = (char)('0' + rem);
		}
	}
	for (i = 0; i < n && i + 1 < cap; i++)
		out[i] = tmp[n - 1 - i];
	out[i] = '\0';
	return i;
}

static inline size_t fj_fmt_u32_pad(uint32_t v, size_t min_width, char pad, char *out, size_t cap)
{
	char num[16];
	size_t nlen = fj_fmt_u32(v, num, sizeof(num));
	size_t pos = 0, i;

	if (cap == 0)
		return 0;
	while (nlen + pos < min_width && pos + 1 < cap)
		out[pos++] = pad;
	for (i = 0; i < nlen && pos + 1 < cap; i++)
		out[pos++] = num[i];
	out[pos] = '\0';
	return pos;
}

static inline size_t fj_fmt_i32(int32_t v, char *out, size_t cap)
{
	if (cap == 0)
		return 0;
	if (v < 0) {
		uint32_t mag = (uint32_t)(-(v + 1)) + 1U;

		out[0] = '-';
		return 1 + fj_fmt_u32(mag, out + 1, cap - 1);
	}
	return fj_fmt_u32((uint32_t)v, out, cap);
}

static inline bool fj_parse_u32(const char *s, uint32_t *out)
{
	uint32_t val = 0U;
	bool any = false;

	while (*s >= '0' && *s <= '9') {
		val = val * 10U + (uint32_t)(*s - '0');
		s++;
		any = true;
	}
	if (!any || *s != '\0')
		return false;
	if (out)
		*out = val;
	return true;
}

#if defined(__arm__) && !defined(FUJISAN_HOST_TEST)
static inline long fj_sc1(long n, long a0)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a0;

	__asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory");
	return r0;
}

static inline long fj_sc2(long n, long a0, long a1)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a0;
	register long r1 __asm__("r1") = a1;

	__asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1) : "memory");
	return r0;
}

static inline long fj_sc3(long n, long a0, long a1, long a2)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a0;
	register long r1 __asm__("r1") = a1;
	register long r2 __asm__("r2") = a2;

	__asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
	return r0;
}

static inline long fj_sc4(long n, long a0, long a1, long a2, long a3)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a0;
	register long r1 __asm__("r1") = a1;
	register long r2 __asm__("r2") = a2;
	register long r3 __asm__("r3") = a3;

	__asm__ volatile("svc #0"
			 : "+r"(r0)
			 : "r"(r7), "r"(r1), "r"(r2), "r"(r3)
			 : "memory");
	return r0;
}

static inline long fj_sc5(long n, long a0, long a1, long a2, long a3, long a4)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a0;
	register long r1 __asm__("r1") = a1;
	register long r2 __asm__("r2") = a2;
	register long r3 __asm__("r3") = a3;
	register long r4 __asm__("r4") = a4;

	__asm__ volatile("svc #0"
			 : "+r"(r0)
			 : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
			 : "memory");
	return r0;
}

static inline long fj_sc6(long n, long a0, long a1, long a2, long a3, long a4, long a5)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a0;
	register long r1 __asm__("r1") = a1;
	register long r2 __asm__("r2") = a2;
	register long r3 __asm__("r3") = a3;
	register long r4 __asm__("r4") = a4;
	register long r5 __asm__("r5") = a5;

	__asm__ volatile("svc #0"
			 : "+r"(r0)
			 : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
			 : "memory");
	return r0;
}

static inline int fj_monotonic_secs(void)
{
	struct fj_timespec32 ts = { 0, 0 };

	fj_sc2(FJ_SYS_clock_gettime, FJ_CLOCK_MONOTONIC, (long)&ts);
	return ts.tv_sec;
}

static inline void fj_sleep_ms(int ms)
{
	struct fj_timespec32 ts;

	ts.tv_sec = 0;
	ts.tv_nsec = ms * 1000000;
	while (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	fj_sc2(FJ_SYS_nanosleep, (long)&ts, 0);
}

static inline bool fj_path_exists(const char *path)
{
	return fj_sc2(FJ_SYS_access, (long)path, FJ_F_OK) == 0;
}

static inline int fj_read_text_file(const char *path, char *buf, size_t cap)
{
	int fd;
	long n;

	if (!buf || cap == 0)
		return -1;
	fd = (int)fj_sc3(FJ_SYS_open, (long)path, FJ_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	n = fj_sc3(FJ_SYS_read, fd, (long)buf, (long)(cap - 1));
	fj_sc1(FJ_SYS_close, fd);
	if (n < 0)
		return -1;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;
	buf[n] = '\0';
	return (int)n;
}

void *memset(void *dst, int c, size_t n)
{
	uint8_t *p = (uint8_t *)dst;

	while (n--)
		*p++ = (uint8_t)c;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else if (d > s) {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}
#endif /* __arm__ && !FUJISAN_HOST_TEST */

#endif /* _FUJISAN_SYS_H_ */

// SPDX-License-Identifier: MIT
/* Helpers shared by ramon_cli, ramon_test and ramon_accept */
#include "tools_common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures, skips, quiet;

static void emit(const char *tag, int always, const char *fmt, va_list ap)
{
	if (!always && quiet)
		return;
	flockfile(stdout);
	fputs(tag, stdout);
	vprintf(fmt, ap);
	putchar('\n');
	fflush(stdout);
	funlockfile(stdout);
}

void tc_ok(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	emit("ok:   ", 0, fmt, ap);
	va_end(ap);
}

void tc_fail(const char *fmt, ...)
{
	va_list ap;

	__atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
	va_start(ap, fmt);
	emit("FAIL: ", 1, fmt, ap);
	va_end(ap);
}

void tc_skip(const char *fmt, ...)
{
	va_list ap;

	__atomic_add_fetch(&skips, 1, __ATOMIC_RELAXED);
	va_start(ap, fmt);
	emit("SKIP: ", 1, fmt, ap);
	va_end(ap);
}

void tc_info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	emit("      ", 0, fmt, ap);
	va_end(ap);
}

void tc_fail_st(const char *what, const struct ramon_status *st)
{
	char buf[256];

	ramon_strerror(st, buf, sizeof(buf));
	tc_fail("%s: %s", what, buf);
}

int tc_failures(void)
{
	return __atomic_load_n(&failures, __ATOMIC_RELAXED);
}

int tc_skips(void)
{
	return __atomic_load_n(&skips, __ATOMIC_RELAXED);
}

void tc_set_quiet(int q)
{
	quiet = q;
}

uint64_t tc_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

double tc_ms_since(uint64_t t0_ns)
{
	return (double)(tc_now_ns() - t0_ns) / 1e6;
}

void tc_sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

double tc_mibps(uint64_t bytes, uint64_t elapsed_ns)
{
	return elapsed_ns ? (double)bytes / 1048576.0 / ((double)elapsed_ns / 1e9) : 0.0;
}

static uint32_t pattern_word(uint32_t seed, size_t i)
{
	return seed ^ (uint32_t)(i * 2654435761u);
}

void tc_fill(void *p, size_t bytes, uint32_t seed)
{
	uint32_t *w = p, last;
	size_t i, n = bytes / 4;

	for (i = 0; i < n; i++)
		w[i] = pattern_word(seed, i);
	if (bytes % 4) {
		last = pattern_word(seed, n);
		memcpy((uint8_t *)p + n * 4, &last, bytes % 4);
	}
}

long tc_find_mismatch(const void *p, size_t bytes, uint32_t seed)
{
	const uint32_t *w = p;
	uint32_t last;
	size_t i, n = bytes / 4;

	for (i = 0; i < n; i++)
		if (w[i] != pattern_word(seed, i)) {
			const uint8_t *a = (const uint8_t *)&w[i];
			uint32_t want = pattern_word(seed, i);
			const uint8_t *b = (const uint8_t *)&want;
			size_t k;

			for (k = 0; k < 4 && a[k] == b[k]; k++)
				;
			return (long)(i * 4 + k);
		}
	if (bytes % 4) {
		last = pattern_word(seed, n);
		if (memcmp((const uint8_t *)p + n * 4, &last, bytes % 4))
			return (long)(n * 4);
	}
	return -1;
}

int tc_all_zero(const void *p, size_t bytes)
{
	const uint8_t *b = p;
	size_t i;

	for (i = 0; i < bytes; i++)
		if (b[i])
			return 0;
	return 1;
}

void tc_hexdump(const void *p, size_t len, size_t base_off, size_t max_len)
{
	const uint8_t *b = p;
	size_t i, j;

	if (len > max_len)
		len = max_len;
	for (i = 0; i < len; i += 16) {
		printf("      %08zx ", base_off + i);
		for (j = i; j < i + 16; j++) {
			if (j < len)
				printf(" %02x", b[j]);
			else
				printf("   ");
		}
		printf("  ");
		for (j = i; j < i + 16 && j < len; j++)
			putchar(b[j] >= 0x20 && b[j] < 0x7f ? b[j] : '.');
		putchar('\n');
	}
}

int tc_parse_u64(const char *s, uint64_t *v)
{
	char *end;

	if (!s || !*s || *s == '-' || *s == '+')
		return -EINVAL;
	errno = 0;
	*v = strtoull(s, &end, 0);
	return errno || *end ? -EINVAL : 0;
}

int tc_parse_u32(const char *s, uint32_t *v)
{
	uint64_t x;

	if (tc_parse_u64(s, &x) || x > 0xFFFFFFFFull)
		return -EINVAL;
	*v = (uint32_t)x;
	return 0;
}

int tc_parse_pair(const char *s, uint32_t v[2])
{
	char a[32], *comma;

	if (strlen(s) >= sizeof(a))
		return -EINVAL;
	strcpy(a, s);
	comma = strchr(a, ',');
	if (!comma)
		return -EINVAL;
	*comma = 0;
	return tc_parse_u32(a, &v[0]) || tc_parse_u32(comma + 1, &v[1]) ? -EINVAL : 0;
}

long tc_read_meminfo_kb(const char *key)
{
	char line[128];
	size_t n = strlen(key);
	long v = -1;
	FILE *f;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, n) && line[n] == ':') {
			v = strtol(line + n + 1, NULL, 10);
			break;
		}
	fclose(f);
	return v;
}

int tc_read_line(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	int ret = 0;

	if (!f)
		return -errno;
	if (!fgets(buf, (int)cap, f))
		ret = -EIO;
	else
		buf[strcspn(buf, "\n")] = 0;
	fclose(f);
	return ret;
}

int tc_read_file(const char *path, uint8_t **data, size_t *len, size_t max_len)
{
	FILE *f = fopen(path, "rb");
	long size;
	int ret = 0;

	*data = NULL;
	*len = 0;
	if (!f)
		return -errno;
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
		ret = -errno;
		goto out;
	}
	if ((size_t)size > max_len) {
		ret = -EFBIG;
		goto out;
	}
	*data = malloc(size ? (size_t)size : 1);
	if (!*data) {
		ret = -ENOMEM;
		goto out;
	}
	if (fread(*data, 1, (size_t)size, f) != (size_t)size) {
		free(*data);
		*data = NULL;
		ret = -EIO;
		goto out;
	}
	*len = (size_t)size;
out:
	fclose(f);
	return ret;
}

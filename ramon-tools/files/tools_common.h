/* SPDX-License-Identifier: MIT */
/* Helpers shared by ramon_cli, ramon_test and ramon_accept */
#ifndef TOOLS_COMMON_H
#define TOOLS_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "ramon.h"
#include "ramon_spw.h"
#include "ramon_spfi.h"

/* Must equal PV in ramon-tools.bb. */
#define RAMON_TOOLS_VERSION	"0.5.4"

#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))

/* ---- reporting: thread-safe, counted ---- */
void tc_ok(const char *fmt, ...) RAMON_PRINTF(1, 2);
void tc_fail(const char *fmt, ...) RAMON_PRINTF(1, 2);
void tc_skip(const char *fmt, ...) RAMON_PRINTF(1, 2);
/* an indented detail line */
void tc_info(const char *fmt, ...) RAMON_PRINTF(1, 2);
/* "FAIL: what: <ramon_strerror>" */
void tc_fail_st(const char *what, const struct ramon_status *st);
int tc_failures(void);
int tc_skips(void);
void tc_set_quiet(int quiet);	/* drops ok: and detail lines */

/* ---- time ---- */
uint64_t tc_now_ns(void);
double tc_ms_since(uint64_t t0_ns);
void tc_sleep_ms(unsigned ms);
double tc_mibps(uint64_t bytes, uint64_t elapsed_ns);

/* ---- data patterns: 32-bit words seed ^ (i * 2654435761) ---- */
void tc_fill(void *p, size_t bytes, uint32_t seed);
/* byte offset of the first difference, or -1 */
long tc_find_mismatch(const void *p, size_t bytes, uint32_t seed);
int tc_all_zero(const void *p, size_t bytes);
/* hex dump of len bytes (at most max_len), offsets starting at base_off */
void tc_hexdump(const void *p, size_t len, size_t base_off, size_t max_len);

/* ---- parsing: decimal, 0x hex, 0 octal; the whole string must be a number ---- */
int tc_parse_u64(const char *s, uint64_t *v);
int tc_parse_u32(const char *s, uint32_t *v);
/* "4,4" */
int tc_parse_pair(const char *s, uint32_t v[2]);

/* ---- system ---- */
/* a /proc/meminfo value in kB, or -1 */
long tc_read_meminfo_kb(const char *key);
/* first line of a file without the newline; 0 or -errno */
int tc_read_line(const char *path, char *buf, size_t cap);
/* whole file into a malloc'd buffer of at most max_len bytes; 0 or -errno */
int tc_read_file(const char *path, uint8_t **data, size_t *len, size_t max_len);

#endif /* TOOLS_COMMON_H */

// SPDX-License-Identifier: MIT
/* ramon_accept: RS-TOP version and time tag, SYSMON */
#include "accept.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMETAG_GAP_MS	1100	/* two time tag samples, to see whether it counts */
#define TEMP_MIN_C	-40.0	/* default temperature limits, as ramon_test */
#define TEMP_MAX_C	125.0
#define BAD_REG		0xFFFFFFFFu	/* what an unanswered AXI read tends to return */

/* ---- rstop-version (was rdrstopver) ---- */

static void step_rstop_version(void)
{
	const uint32_t *want = A.o.rstop_expect;
	struct ramon_status st;
	uint32_t v[3], cfg;

	if (a_check(ramon_rstop_version(A.c, v, &st), "RS-TOP version registers 0x00/0x04/0x08",
		    &st) ||
	    a_check(ramon_reg_read(A.c, "rs_top", RAMON_RSTOP_CONFIG, &cfg, &st),
		    "RS-TOP configuration register 0x0C", &st))
		return;
	tc_info("RS TOP version: %u.%u.%u (0x%x 0x%x 0x%x)", v[0], v[1], v[2], v[0], v[1], v[2]);
	tc_info("master/slave configuration (0x0C): 0x%x", cfg);
	a_summary("RS-TOP %u.%u.%u, configuration 0x%x", v[0], v[1], v[2], cfg);

	if (v[0] == BAD_REG || v[1] == BAD_REG || v[2] == BAD_REG)
		tc_fail("a version register reads 0xffffffff: RS-TOP does not answer");
	else if (!v[0] && !v[1] && !v[2])
		tc_fail("version 0.0.0: RS-TOP is not loaded or not clocked");
	if (A.o.have_rstop_expect &&
	    (v[0] != want[0] || v[1] != want[1] || v[2] != want[2]))
		tc_fail("RS-TOP version %u.%u.%u, expected %u.%u.%u (--rstop-version)", v[0], v[1], v[2],
			want[0], want[1], want[2]);
	else if (!A.o.have_rstop_expect)
		tc_info("not compared: pass --rstop-version to check it against the delivery");
}

/* ---- rstop-timetag (was readtimetag) ---- */

static int time_valid(const struct ramon_rstop_time *t)
{
	return t->day >= 1 && t->day <= 31 && t->month >= 1 && t->month <= 12 && t->hour <= 23 &&
	       t->min <= 59 && t->sec <= 59;
}

static void step_rstop_timetag(void)
{
	struct ramon_rstop_time t, t2;
	struct ramon_status st;
	uint32_t a, b;

	if (a_check(ramon_reg_read(A.c, "rs_top", RAMON_RSTOP_TIMESTAMP, &a, &st),
		    "RS-TOP time tag register 0x10", &st))
		return;
	tc_sleep_ms(TIMETAG_GAP_MS);
	if (a_check(ramon_reg_read(A.c, "rs_top", RAMON_RSTOP_TIMESTAMP, &b, &st),
		    "RS-TOP time tag register 0x10", &st))
		return;
	ramon_rstop_decode_time(a, &t);
	ramon_rstop_decode_time(b, &t2);
	tc_info("time tag 0x%08x: %02u/%02u/%02u %02u:%02u:%02u (dd/mm/yy hh:mm:ss)", a, t.day,
		t.month, t.year, t.hour, t.min, t.sec);
	if (a == b)
		tc_info("unchanged after %u ms (a fixed time stamp)", TIMETAG_GAP_MS);
	else
		tc_info("after %u ms: 0x%08x, %02u/%02u/%02u %02u:%02u:%02u (counting)", TIMETAG_GAP_MS,
			b, t2.day, t2.month, t2.year, t2.hour, t2.min, t2.sec);
	a_summary("%02u/%02u/%02u %02u:%02u:%02u (%s)", t.day, t.month, t.year, t.hour, t.min, t.sec,
		  a == b ? "fixed" : "counting");

	if (a == BAD_REG)
		tc_fail("the time tag reads 0xffffffff: RS-TOP does not answer");
	else if (!time_valid(&t) || !time_valid(&t2))
		tc_fail("the time tag does not decode to a valid date and time");
}

/* ---- sysmon (was sysmon) ---- */

/* keys: temp_ps, temp_pl, then ps.<rail> / pl.<rail> for the 37 rails */
#define N_KEYS		(2 + RAMON_SYSMON_RAILS)

struct limit {
	int set;
	double min, max;
};

static void key_name(unsigned k, char *buf, size_t len)
{
	const struct ramon_sysmon_rail *r;

	if (k < 2) {
		snprintf(buf, len, "temp_%s", k ? "pl" : "ps");
		return;
	}
	r = &ramon_sysmon_rails[k - 2];
	snprintf(buf, len, "%s.%s", r->win == RAMON_WIN_SYSMON_PS ? "ps" : "pl", r->name);
}

static int key_find(const char *name)
{
	char buf[48];
	unsigned k;

	for (k = 0; k < N_KEYS; k++) {
		key_name(k, buf, sizeof(buf));
		if (!strcmp(buf, name))
			return (int)k;
	}
	return -1;
}

/* "key min max" per line, # comments; reports and returns -1 on any error */
static int load_limits(const char *path, struct limit *lim)
{
	char line[256], key[64], tail[2];
	unsigned lineno = 0;
	double lo, hi;
	int k, ret = 0;
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		tc_fail("limits file %s: %s", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *p = line;

		lineno++;
		line[strcspn(line, "#\r\n")] = 0;
		while (isspace((unsigned char)*p))
			p++;
		if (!*p)
			continue;
		if (sscanf(p, "%63s %lf %lf %1s", key, &lo, &hi, tail) != 3 || lo > hi) {
			tc_fail("%s:%u: expected \"name min max\" with min <= max", path, lineno);
			ret = -1;
			continue;
		}
		k = key_find(key);
		if (k < 0) {
			tc_fail("%s:%u: unknown name \"%s\" (temp_ps, temp_pl, ps.<rail>, pl.<rail>)",
				path, lineno, key);
			ret = -1;
			continue;
		}
		lim[k].set = 1;
		lim[k].min = lo;
		lim[k].max = hi;
	}
	fclose(f);
	return ret;
}

/* prints one reading with its limit; returns 1 if it is out of range */
static int show(unsigned k, double value, const char *unit, const char *desc,
		const struct limit *l)
{
	char name[48], range[48] = "";
	int bad = l->set && (value < l->min || value > l->max);

	key_name(k, name, sizeof(name));
	if (!l->set) {
		tc_info("%-16s %8.3f %s  %s", name, value, unit, desc);
		return 0;
	}
	snprintf(range, sizeof(range), "[%.3f .. %.3f] %s", l->min, l->max,
		 bad ? "OUT OF RANGE" : "ok");
	tc_info("%-16s %8.3f %s  %-40s %s", name, value, unit, desc, range);
	return bad;
}

static void step_sysmon(void)
{
	struct limit lim[N_KEYS];
	struct ramon_status st;
	struct ramon_sysmon s;
	unsigned k, checked = 0, bad = 0;

	memset(lim, 0, sizeof(lim));
	lim[0] = lim[1] = (struct limit){ 1, TEMP_MIN_C, TEMP_MAX_C };
	if (A.o.limits) {
		tc_info("limits from %s", A.o.limits);
		if (load_limits(A.o.limits, lim)) {
			a_summary("bad limits file (see above)");
			return;
		}
	}
	if (a_check(ramon_sysmon_read(A.c, &s, &st), "SYSMON", &st))
		return;

	bad += show(0, s.ps_temp_c, "C", "PS temperature", &lim[0]);
	bad += show(1, s.pl_temp_c, "C", "PL temperature", &lim[1]);
	for (k = 2; k < N_KEYS; k++) {
		bad += show(k, s.volts[k - 2], "V", ramon_sysmon_rails[k - 2].desc, &lim[k]);
		checked += lim[k].set;
	}
	if (!checked)
		tc_info("no rail limits configured: rails printed only (--limits, or %s)",
			ACCEPT_LIMITS_DEFAULT);
	a_summary("PS %.1f C, PL %.1f C; %u of %u rails checked", s.ps_temp_c, s.pl_temp_c, checked,
		  RAMON_SYSMON_RAILS);
	if (bad)
		tc_fail("%u reading(s) out of range", bad);
}

const struct step accept_board[] = {
	{ "rstop-version", step_rstop_version, NEEDS_DEV, "rdrstopver",
	  "RS-TOP version 0x00/0x04/0x08 and configuration 0x0C", NULL },
	{ "rstop-timetag", step_rstop_timetag, NEEDS_DEV, "readtimetag",
	  "RS-TOP time tag 0x10 decodes to a valid date and time", NULL },
	{ "sysmon", step_sysmon, NEEDS_DEV, "sysmon",
	  "PS/PL temperatures and the 37 SYSMON rails within limits", NULL },
	{ NULL, NULL, 0, NULL, NULL, NULL },
};

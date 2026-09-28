// SPDX-License-Identifier: MIT
/*
 * ramon_test: automatic tests of libramon and ramon_dma on the board.
 *
 *   ramon_test [options] [test...]
 *
 * Without names it runs every default test. Tests whose prerequisite is
 * missing (no ZDMA, no synced SPW link, SPFI link down) are skipped.
 * Output: "== test", "ok:", "FAIL:", "SKIP:" lines, then PASSED/FAILED.
 * Exit status: 0 passed, 1 failed, 2 usage or device error.
 */
#include "test.h"

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_METRICS	32
#define SYNC_TRIES	200	/* 2 s per link; the uncabled NN never syncs */

struct test_env T;

struct metric {
	char name[40];
	double value;
};

struct result {
	const struct test *t;
	const char *outcome;	/* "pass", "fail", "skip" */
	double ms;
	unsigned n_metrics;
	struct metric m[MAX_METRICS];
};

static struct result *cur;

static const struct test *const suites[] = { test_core, test_spw, test_spfi, test_soak };

void t_metric(const char *name, double value)
{
	if (!cur || cur->n_metrics == MAX_METRICS)
		return;
	snprintf(cur->m[cur->n_metrics].name, sizeof(cur->m[0].name), "%s", name);
	cur->m[cur->n_metrics++].value = value;
}

int t_check(int ret, const char *what, const struct ramon_status *st)
{
	if (ret)
		tc_fail_st(what, st);
	return ret;
}

static void list_tests(void)
{
	static const char *const when[] = { "", " (--long)", " (--minutes)" };
	const struct test *t;
	size_t s;

	for (s = 0; s < ARRAY_SIZE(suites); s++)
		for (t = suites[s]; t->name; t++)
			printf("  %-14s %s%s\n", t->name, t->help, when[t->when]);
}

static const struct test *find_test(const char *name)
{
	const struct test *t;
	size_t s;

	for (s = 0; s < ARRAY_SIZE(suites); s++)
		for (t = suites[s]; t->name; t++)
			if (!strcmp(t->name, name))
				return t;
	return NULL;
}

/* why a test cannot run, or NULL */
static const char *missing(unsigned needs)
{
	if ((needs & NEEDS_ZDMA) && !T.n_zdma)
		return "no ZDMA channels";
	if ((needs & (NEEDS_SPW | NEEDS_SPW_LINK)) && T.o.skip_spw)
		return "SPW not in use (--skip-spw, or its setup failed above)";
	if ((needs & NEEDS_SPW) && !T.spw_present)
		return "no SPW NN present";
	if ((needs & NEEDS_SPW_LINK) && T.spw_nn < 0)
		return "no synced SPW link";
	if ((needs & (NEEDS_SPFI | NEEDS_SPFI_LINK)) && T.o.skip_spfi)
		return "SPFI not in use (--skip-spfi, or its setup failed above)";
	if ((needs & NEEDS_SPFI) && !T.spfi_present)
		return "no SPFI NN present";
	if ((needs & NEEDS_SPFI_LINK) && T.spfi_nn < 0)
		return "no SPFI link up (bring it up with --prep)";
	return NULL;
}

static int setup(void)
{
	struct ramon_spw_config sc;
	struct ramon_spfi_config fc;
	struct ramon_status st;
	uint32_t nn, tries, raw;
	char line[160];
	int ret, up;

	ret = ramon_open(T.o.dev, &T.c, &st);
	if (ret) {
		tc_fail_st("ramon_open", &st);
		return ret;
	}
	ramon_version_line(T.c, line, sizeof(line));
	printf("ramon_test %s, %s\n", RAMON_TOOLS_VERSION, line);
	T.n_zdma = ramon_info(T.c)->n_zdma_chan;
	T.spw_nn = T.spfi_nn = -1;

	ramon_spw_config_default(&sc);
	sc.node_id = T.o.node;
	sc.nn_node = T.o.target;
	sc.sync_tries = SYNC_TRIES;
	/* a layer that cannot start fails its group, the other tests still run */
	if (!T.o.skip_spw && ramon_info(T.c)->spw_mask && ramon_spw_init(T.c, &sc, &st)) {
		tc_fail_st("ramon_spw_init (SPW tests will be skipped)", &st);
		T.o.skip_spw = 1;
	}
	ramon_spfi_config_default(&fc);
	fc.chan[0] = T.o.spfi_chan[0];
	fc.chan[1] = T.o.spfi_chan[1];
	if (!T.o.skip_spfi && ramon_info(T.c)->spfi_mask && ramon_spfi_init(T.c, &fc, &st)) {
		tc_fail_st("ramon_spfi_init (SPFI tests will be skipped)", &st);
		T.o.skip_spfi = 1;
	}

	for (nn = 0; nn < RAMON_NN_COUNT && !T.o.skip_spw; nn++) {
		if (!ramon_spw_present(T.c, nn))
			continue;
		T.spw_present |= 1u << nn;
		if (!ramon_spw_sync(T.c, nn, &tries, &st)) {
			printf("      spw%u: link synced after %u reset(s)\n", nn, tries);
			if (T.spw_nn < 0)
				T.spw_nn = (int)nn;
		} else {
			printf("      spw%u: link not synced (expected for an NN without a cable)\n", nn);
		}
	}
	for (nn = 0; nn < RAMON_NN_COUNT && !T.o.skip_spfi; nn++) {
		if (!ramon_spfi_present(T.c, nn))
			continue;
		T.spfi_present |= 1u << nn;
		if (T.o.prep) {
			struct ramon_spfi_prep_opts po = { T.o.format, 0 };

			printf("      spfi%u: bring-up%s\n", nn, T.o.format ? " with FORMAT" : "");
			if (ramon_spfi_prep(T.c, nn, &po, NULL, &st))
				tc_fail_st("spfi bring-up", &st);
		}
		up = ramon_spfi_link_up(T.c, nn, &raw, &st);
		printf("      spfi%u: link %s (0xCC = 0x%x)\n", nn, up > 0 ? "up" : "down", raw);
		if (up > 0 && T.spfi_nn < 0)
			T.spfi_nn = (int)nn;
	}
	return 0;
}

static void print_json(const struct result *r, unsigned n)
{
	const struct ramon_get_info *gi = T.c ? ramon_info(T.c) : NULL;
	unsigned i, k;

	printf("{\"tool\":\"ramon_test\",\"version\":\"%s\",\"lib\":\"%s\"", RAMON_TOOLS_VERSION,
	       ramon_api_version());
	if (gi)
		printf(",\"driver\":\"%u.%u.%u\",\"abi\":%u", gi->drv_major, gi->drv_minor,
		       gi->drv_patch, gi->abi_version);
	printf(",\"tests\":[");
	for (i = 0; i < n; i++) {
		printf("%s{\"name\":\"%s\",\"result\":\"%s\",\"ms\":%.0f,\"metrics\":{", i ? "," : "",
		       r[i].t->name, r[i].outcome, r[i].ms);
		for (k = 0; k < r[i].n_metrics; k++)
			printf("%s\"%s\":%.3f", k ? "," : "", r[i].m[k].name, r[i].m[k].value);
		printf("}}");
	}
	printf("],\"failures\":%d,\"skipped\":%d}\n", tc_failures(), tc_skips());
}

static void usage(FILE *f)
{
	fprintf(f,
		"usage: ramon_test [options] [test...]\n"
		"  -d, --dev PATH         device (default %s)\n"
		"      --node N           our SPW node id (default %u)\n"
		"      --target N         the NN's SPW node id (default 0x%x)\n"
		"      --spfi-chans A,B   AXI write channel of SPFI NN0,NN1 (default %u,%u)\n"
		"      --skip-spw         no SPW tests (and no link resets)\n"
		"      --skip-spfi        no SPFI tests\n"
		"      --prep             SPFI bring-up first (link, SPW DPS, INIT)\n"
		"      --format           with --prep: FORMAT the NN (erases every stream)\n"
		"      --spfi-pages N     largest SPFI write in spfi-perf (default 64)\n"
		"      --runs N           writes/reads per size in spfi-perf (default 8)\n"
		"      --spw-max N        largest payload in spw-loop (default 65536)\n"
		"      --long             also run spfi-maxlist (writes 156 MiB to the NN)\n"
		"      --minutes N        also run soak for N minutes\n"
		"      --fail-fast        stop at the first failing test\n"
		"      --strict           a skipped test counts as a failure\n"
		"      --json             one JSON summary line at the end\n"
		"  -l, --list             list the tests\n"
		"  -V, --version          print the versions\n",
		RAMON_DEV_PATH, RAMON_SPW_NODE_DEFAULT, RAMON_SPW_NN_NODE_DEFAULT,
		RAMON_SPFI_CHAN_NN0, RAMON_SPFI_CHAN_NN1);
}

static int opt_u32(const char *s, const char *what, uint32_t max, uint32_t *v)
{
	if (!tc_parse_u32(s, v) && *v <= max)
		return 0;
	fprintf(stderr, "ramon_test: bad %s \"%s\"\n", what, s);
	return -1;
}

int main(int argc, char **argv)
{
	enum { O_NODE = 1, O_TARGET, O_CHANS, O_SKIP_SPW, O_SKIP_SPFI, O_PREP, O_FORMAT,
	       O_PAGES, O_RUNS, O_SPW_MAX, O_LONG, O_MINUTES, O_FAIL_FAST, O_STRICT, O_JSON };
	static const struct option opts[] = {
		{ "dev", required_argument, NULL, 'd' },
		{ "node", required_argument, NULL, O_NODE },
		{ "target", required_argument, NULL, O_TARGET },
		{ "spfi-chans", required_argument, NULL, O_CHANS },
		{ "skip-spw", no_argument, NULL, O_SKIP_SPW },
		{ "skip-spfi", no_argument, NULL, O_SKIP_SPFI },
		{ "prep", no_argument, NULL, O_PREP },
		{ "format", no_argument, NULL, O_FORMAT },
		{ "spfi-pages", required_argument, NULL, O_PAGES },
		{ "runs", required_argument, NULL, O_RUNS },
		{ "spw-max", required_argument, NULL, O_SPW_MAX },
		{ "long", no_argument, NULL, O_LONG },
		{ "minutes", required_argument, NULL, O_MINUTES },
		{ "fail-fast", no_argument, NULL, O_FAIL_FAST },
		{ "strict", no_argument, NULL, O_STRICT },
		{ "json", no_argument, NULL, O_JSON },
		{ "list", no_argument, NULL, 'l' },
		{ "version", no_argument, NULL, 'V' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const struct test *t, *sel[64];
	struct result *res;
	unsigned n_sel = 0, i, n_run = 0;
	char ver[160];
	uint32_t v;
	size_t s;
	int opt, before;
	uint64_t t0;

	T.o.node = RAMON_SPW_NODE_DEFAULT;
	T.o.target = RAMON_SPW_NN_NODE_DEFAULT;
	T.o.spfi_chan[0] = RAMON_SPFI_CHAN_NN0;
	T.o.spfi_chan[1] = RAMON_SPFI_CHAN_NN1;
	T.o.spfi_pages = 64;
	T.o.runs = 8;
	T.o.spw_max = 65536;
	while ((opt = getopt_long(argc, argv, "d:lVh", opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			T.o.dev = optarg;
			break;
		case O_NODE:
		case O_TARGET:
			if (opt_u32(optarg, "node id", 255, &v))
				return 2;
			if (opt == O_NODE)
				T.o.node = (uint8_t)v;
			else
				T.o.target = (uint8_t)v;
			break;
		case O_CHANS:
			if (tc_parse_pair(optarg, T.o.spfi_chan)) {
				fprintf(stderr, "ramon_test: --spfi-chans wants A,B\n");
				return 2;
			}
			break;
		case O_SKIP_SPW:
			T.o.skip_spw = 1;
			break;
		case O_SKIP_SPFI:
			T.o.skip_spfi = 1;
			break;
		case O_PREP:
			T.o.prep = 1;
			break;
		case O_FORMAT:
			T.o.format = 1;
			break;
		case O_PAGES:
			if (opt_u32(optarg, "--spfi-pages", RAMON_SPFI_WRITE_MAX_PAGES, &T.o.spfi_pages) ||
			    !T.o.spfi_pages)
				return 2;
			break;
		case O_RUNS:
			if (opt_u32(optarg, "--runs", 100000, &T.o.runs) || !T.o.runs)
				return 2;
			break;
		case O_SPW_MAX:
			if (opt_u32(optarg, "--spw-max", RAMON_SPW_BUF_DEFAULT - 64, &T.o.spw_max))
				return 2;
			break;
		case O_LONG:
			T.o.long_tests = 1;
			break;
		case O_MINUTES:
			if (opt_u32(optarg, "--minutes", 100000, &T.o.minutes) || !T.o.minutes)
				return 2;
			break;
		case O_FAIL_FAST:
			T.o.fail_fast = 1;
			break;
		case O_STRICT:
			T.o.strict = 1;
			break;
		case O_JSON:
			T.o.json = 1;
			break;
		case 'l':
			list_tests();
			return 0;
		case 'V':
			ramon_version_line(NULL, ver, sizeof(ver));
			printf("ramon_test %s, %s\n", RAMON_TOOLS_VERSION, ver);
			return 0;
		case 'h':
			usage(stdout);
			printf("tests:\n");
			list_tests();
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (T.o.format && !T.o.prep) {
		fprintf(stderr, "ramon_test: --format only works together with --prep\n");
		return 2;
	}

	/* the tests to run, in table order */
	if (optind < argc) {
		for (i = (unsigned)optind; i < (unsigned)argc; i++) {
			if (!find_test(argv[i])) {
				fprintf(stderr, "ramon_test: no test \"%s\" (--list)\n", argv[i]);
				return 2;
			}
		}
		for (s = 0; s < ARRAY_SIZE(suites); s++)
			for (t = suites[s]; t->name; t++)
				for (i = (unsigned)optind; i < (unsigned)argc; i++)
					if (!strcmp(argv[i], t->name) && n_sel < ARRAY_SIZE(sel)) {
						sel[n_sel++] = t;
						break;
					}
		if (!T.o.minutes)
			T.o.minutes = 1;
	} else {
		for (s = 0; s < ARRAY_SIZE(suites); s++)
			for (t = suites[s]; t->name; t++)
				if (n_sel < ARRAY_SIZE(sel) &&
				    (t->when == RUN_DEFAULT || (t->when == RUN_LONG && T.o.long_tests) ||
				     (t->when == RUN_SOAK && T.o.minutes)))
					sel[n_sel++] = t;
	}

	res = calloc(n_sel ? n_sel : 1, sizeof(*res));
	if (!res || setup()) {
		printf("FAILED: could not set up\n");
		free(res);
		ramon_close(T.c);
		return 2;
	}
	for (i = 0; i < n_sel; i++) {
		const char *why = missing(sel[i]->needs);

		cur = &res[n_run++];
		cur->t = sel[i];
		printf("== %s\n", sel[i]->name);
		fflush(stdout);
		if (why) {
			tc_skip("%s: %s", sel[i]->name, why);
			cur->outcome = "skip";
			continue;
		}
		before = tc_failures();
		t0 = tc_now_ns();
		sel[i]->fn();
		cur->ms = tc_ms_since(t0);
		cur->outcome = tc_failures() == before ? "pass" : "fail";
		if (tc_failures() == before)
			tc_ok("%s (%.0f ms)", sel[i]->name, cur->ms);
		else if (T.o.fail_fast)
			break;
	}
	cur = NULL;

	for (i = 0; i < n_run; i++)
		if (res[i].n_metrics) {
			unsigned k;

			printf("   %-14s", res[i].t->name);
			for (k = 0; k < res[i].n_metrics; k++)
				printf(" %s=%.1f", res[i].m[k].name, res[i].m[k].value);
			printf("\n");
		}
	if (T.o.json)
		print_json(res, n_run);
	if (tc_failures() || (T.o.strict && tc_skips()))
		printf("FAILED: %d failure(s), %d test(s) skipped\n", tc_failures(), tc_skips());
	else
		printf("PASSED: %u test(s), %d skipped\n", n_run - (unsigned)tc_skips(), tc_skips());
	i = tc_failures() || (T.o.strict && tc_skips());
	free(res);
	ramon_close(T.c);
	return (int)i;
}

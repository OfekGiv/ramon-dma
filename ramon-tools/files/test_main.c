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

struct test_env t_env;

struct metric {
	char name[40];
	double value;
};

struct result {
	const struct test *test;
	const char *outcome;	/* "pass", "fail", "skip" */
	double ms;
	unsigned n_metrics;
	struct metric metrics[MAX_METRICS];
};

static struct result *cur_result;

static const struct test *const suites[] = { t_core_tests, t_spw_tests, t_spfi_tests,
					     t_soak_tests };

void t_add_metric(const char *name, double value)
{
	if (!cur_result || cur_result->n_metrics == MAX_METRICS)
		return;
	snprintf(cur_result->metrics[cur_result->n_metrics].name,
		 sizeof(cur_result->metrics[0].name), "%s", name);
	cur_result->metrics[cur_result->n_metrics++].value = value;
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
static const char *skip_reason(unsigned needs)
{
	if ((needs & NEEDS_ZDMA) && !t_env.n_zdma_chan)
		return "no ZDMA channels";
	if ((needs & (NEEDS_SPW | NEEDS_SPW_LINK)) && t_env.opts.skip_spw)
		return "SPW not in use (--skip-spw, or its setup failed above)";
	if ((needs & NEEDS_SPW) && !t_env.spw_present_mask)
		return "no SPW NN present";
	if ((needs & NEEDS_SPW_LINK) && t_env.spw_nn < 0)
		return "no synced SPW link";
	if ((needs & (NEEDS_SPFI | NEEDS_SPFI_LINK)) && t_env.opts.skip_spfi)
		return "SPFI not in use (--skip-spfi, or its setup failed above)";
	if ((needs & NEEDS_SPFI) && !t_env.spfi_present_mask)
		return "no SPFI NN present";
	if ((needs & NEEDS_SPFI_LINK) && t_env.spfi_nn < 0)
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

	ret = ramon_open(t_env.opts.dev, &t_env.c, &st);
	if (ret) {
		tc_fail_st("ramon_open", &st);
		return ret;
	}
	ramon_version_line(t_env.c, line, sizeof(line));
	printf("ramon_test %s, %s\n", RAMON_TOOLS_VERSION, line);
	t_env.n_zdma_chan = ramon_info(t_env.c)->n_zdma_chan;
	t_env.spw_nn = t_env.spfi_nn = -1;

	ramon_spw_config_default(&sc);
	sc.node_id = t_env.opts.node;
	sc.nn_node = t_env.opts.target;
	sc.sync_tries = SYNC_TRIES;
	/* a layer that cannot start fails its group, the other tests still run */
	if (!t_env.opts.skip_spw && ramon_info(t_env.c)->spw_mask &&
	    ramon_spw_init(t_env.c, &sc, &st)) {
		tc_fail_st("ramon_spw_init (SPW tests will be skipped)", &st);
		t_env.opts.skip_spw = 1;
	}
	ramon_spfi_config_default(&fc);
	fc.chan[0] = t_env.opts.spfi_chan[0];
	fc.chan[1] = t_env.opts.spfi_chan[1];
	if (!t_env.opts.skip_spfi && ramon_info(t_env.c)->spfi_mask &&
	    ramon_spfi_init(t_env.c, &fc, &st)) {
		tc_fail_st("ramon_spfi_init (SPFI tests will be skipped)", &st);
		t_env.opts.skip_spfi = 1;
	}

	for (nn = 0; nn < RAMON_NN_COUNT && !t_env.opts.skip_spw; nn++) {
		if (!ramon_spw_present(t_env.c, nn))
			continue;
		t_env.spw_present_mask |= 1u << nn;
		if (!ramon_spw_sync(t_env.c, nn, &tries, &st)) {
			printf("      spw%u: link synced after %u reset(s)\n", nn, tries);
			if (t_env.spw_nn < 0)
				t_env.spw_nn = (int)nn;
		} else {
			printf("      spw%u: link not synced (expected for an NN without a cable)\n", nn);
		}
	}
	for (nn = 0; nn < RAMON_NN_COUNT && !t_env.opts.skip_spfi; nn++) {
		if (!ramon_spfi_present(t_env.c, nn))
			continue;
		t_env.spfi_present_mask |= 1u << nn;
		if (t_env.opts.prep_requested) {
			struct ramon_spfi_prep_opts po = { t_env.opts.format_requested, 0 };

			printf("      spfi%u: bring-up%s\n", nn,
			       t_env.opts.format_requested ? " with FORMAT" : "");
			if (ramon_spfi_prep(t_env.c, nn, &po, NULL, &st))
				tc_fail_st("spfi bring-up", &st);
		}
		up = ramon_spfi_link_up(t_env.c, nn, &raw, &st);
		printf("      spfi%u: link %s (0xCC = 0x%x)\n", nn, up > 0 ? "up" : "down", raw);
		if (up > 0 && t_env.spfi_nn < 0)
			t_env.spfi_nn = (int)nn;
	}
	return 0;
}

static void print_json(const struct result *r, unsigned n)
{
	const struct ramon_get_info *gi = t_env.c ? ramon_info(t_env.c) : NULL;
	unsigned i, k;

	printf("{\"tool\":\"ramon_test\",\"version\":\"%s\",\"lib\":\"%s\"", RAMON_TOOLS_VERSION,
	       ramon_api_version());
	if (gi)
		printf(",\"driver\":\"%u.%u.%u\",\"abi\":%u", gi->drv_major, gi->drv_minor,
		       gi->drv_patch, gi->abi_version);
	printf(",\"tests\":[");
	for (i = 0; i < n; i++) {
		printf("%s{\"name\":\"%s\",\"result\":\"%s\",\"ms\":%.0f,\"metrics\":{", i ? "," : "",
		       r[i].test->name, r[i].outcome, r[i].ms);
		for (k = 0; k < r[i].n_metrics; k++)
			printf("%s\"%s\":%.3f", k ? "," : "", r[i].metrics[k].name,
			       r[i].metrics[k].value);
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
		"      --spw-max N        largest SPW payload in spw-loop and axi-loop (at most and default 4096)\n"
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

static int parse_opt_u32(const char *s, const char *what, uint32_t max, uint32_t *v)
{
	if (!tc_parse_u32(s, v) && *v <= max)
		return 0;
	fprintf(stderr, "ramon_test: bad %s \"%s\" (0..%u)\n", what, s, max);
	return -1;
}

int main(int argc, char **argv)
{
	enum { OPT_NODE = 1, OPT_TARGET, OPT_SPFI_CHANS, OPT_SKIP_SPW, OPT_SKIP_SPFI, OPT_PREP,
	       OPT_FORMAT, OPT_SPFI_PAGES, OPT_RUNS, OPT_SPW_MAX, OPT_LONG, OPT_MINUTES,
	       OPT_FAIL_FAST, OPT_STRICT, OPT_JSON };
	static const struct option long_opts[] = {
		{ "dev", required_argument, NULL, 'd' },
		{ "node", required_argument, NULL, OPT_NODE },
		{ "target", required_argument, NULL, OPT_TARGET },
		{ "spfi-chans", required_argument, NULL, OPT_SPFI_CHANS },
		{ "skip-spw", no_argument, NULL, OPT_SKIP_SPW },
		{ "skip-spfi", no_argument, NULL, OPT_SKIP_SPFI },
		{ "prep", no_argument, NULL, OPT_PREP },
		{ "format", no_argument, NULL, OPT_FORMAT },
		{ "spfi-pages", required_argument, NULL, OPT_SPFI_PAGES },
		{ "runs", required_argument, NULL, OPT_RUNS },
		{ "spw-max", required_argument, NULL, OPT_SPW_MAX },
		{ "long", no_argument, NULL, OPT_LONG },
		{ "minutes", required_argument, NULL, OPT_MINUTES },
		{ "fail-fast", no_argument, NULL, OPT_FAIL_FAST },
		{ "strict", no_argument, NULL, OPT_STRICT },
		{ "json", no_argument, NULL, OPT_JSON },
		{ "list", no_argument, NULL, 'l' },
		{ "version", no_argument, NULL, 'V' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const struct test *t, *selected[64];
	struct result *results;
	unsigned n_selected = 0, i, n_run = 0;
	char version_line[160];
	uint32_t node_id;
	size_t s;
	int opt, failures_before;
	uint64_t t0;

	t_env.opts.node = RAMON_SPW_NODE_DEFAULT;
	t_env.opts.target = RAMON_SPW_NN_NODE_DEFAULT;
	t_env.opts.spfi_chan[0] = RAMON_SPFI_CHAN_NN0;
	t_env.opts.spfi_chan[1] = RAMON_SPFI_CHAN_NN1;
	t_env.opts.spfi_perf_max_pages = 64;
	t_env.opts.runs = 8;
	t_env.opts.spw_max_payload_bytes = RAMON_SPW_MAX_PAYLOAD;
	while ((opt = getopt_long(argc, argv, "d:lVh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			t_env.opts.dev = optarg;
			break;
		case OPT_NODE:
		case OPT_TARGET:
			if (parse_opt_u32(optarg, "node id", 255, &node_id))
				return 2;
			if (opt == OPT_NODE)
				t_env.opts.node = (uint8_t)node_id;
			else
				t_env.opts.target = (uint8_t)node_id;
			break;
		case OPT_SPFI_CHANS:
			if (tc_parse_pair(optarg, t_env.opts.spfi_chan)) {
				fprintf(stderr, "ramon_test: --spfi-chans wants A,B\n");
				return 2;
			}
			break;
		case OPT_SKIP_SPW:
			t_env.opts.skip_spw = 1;
			break;
		case OPT_SKIP_SPFI:
			t_env.opts.skip_spfi = 1;
			break;
		case OPT_PREP:
			t_env.opts.prep_requested = 1;
			break;
		case OPT_FORMAT:
			t_env.opts.format_requested = 1;
			break;
		case OPT_SPFI_PAGES:
			if (parse_opt_u32(optarg, "--spfi-pages", RAMON_SPFI_WRITE_MAX_PAGES,
					  &t_env.opts.spfi_perf_max_pages) ||
			    !t_env.opts.spfi_perf_max_pages)
				return 2;
			break;
		case OPT_RUNS:
			if (parse_opt_u32(optarg, "--runs", 100000, &t_env.opts.runs) ||
			    !t_env.opts.runs)
				return 2;
			break;
		case OPT_SPW_MAX:
			/* the FPGA takes at most RAMON_SPW_MAX_PAYLOAD bytes */
			if (parse_opt_u32(optarg, "--spw-max", RAMON_SPW_MAX_PAYLOAD,
					  &t_env.opts.spw_max_payload_bytes))
				return 2;
			break;
		case OPT_LONG:
			t_env.opts.long_requested = 1;
			break;
		case OPT_MINUTES:
			if (parse_opt_u32(optarg, "--minutes", 100000, &t_env.opts.soak_minutes) ||
			    !t_env.opts.soak_minutes)
				return 2;
			break;
		case OPT_FAIL_FAST:
			t_env.opts.fail_fast = 1;
			break;
		case OPT_STRICT:
			t_env.opts.strict = 1;
			break;
		case OPT_JSON:
			t_env.opts.json_requested = 1;
			break;
		case 'l':
			list_tests();
			return 0;
		case 'V':
			ramon_version_line(NULL, version_line, sizeof(version_line));
			printf("ramon_test %s, %s\n", RAMON_TOOLS_VERSION, version_line);
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
	if (t_env.opts.format_requested && !t_env.opts.prep_requested) {
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
					if (!strcmp(argv[i], t->name) &&
					    n_selected < ARRAY_SIZE(selected)) {
						selected[n_selected++] = t;
						break;
					}
		if (!t_env.opts.soak_minutes)
			t_env.opts.soak_minutes = 1;
	} else {
		for (s = 0; s < ARRAY_SIZE(suites); s++)
			for (t = suites[s]; t->name; t++)
				if (n_selected < ARRAY_SIZE(selected) &&
				    (t->when == RUN_DEFAULT ||
				     (t->when == RUN_LONG && t_env.opts.long_requested) ||
				     (t->when == RUN_SOAK && t_env.opts.soak_minutes)))
					selected[n_selected++] = t;
	}

	results = calloc(n_selected ? n_selected : 1, sizeof(*results));
	if (!results || setup()) {
		printf("FAILED: could not set up\n");
		free(results);
		ramon_close(t_env.c);
		return 2;
	}
	for (i = 0; i < n_selected; i++) {
		const char *why = skip_reason(selected[i]->needs);

		cur_result = &results[n_run++];
		cur_result->test = selected[i];
		printf("== %s\n", selected[i]->name);
		fflush(stdout);
		if (why) {
			tc_skip("%s: %s", selected[i]->name, why);
			cur_result->outcome = "skip";
			continue;
		}
		failures_before = tc_failures();
		t0 = tc_now_ns();
		selected[i]->fn();
		cur_result->ms = tc_ms_since(t0);
		cur_result->outcome = tc_failures() == failures_before ? "pass" : "fail";
		if (tc_failures() == failures_before)
			tc_ok("%s (%.0f ms)", selected[i]->name, cur_result->ms);
		else if (t_env.opts.fail_fast)
			break;
	}
	cur_result = NULL;

	for (i = 0; i < n_run; i++)
		if (results[i].n_metrics) {
			unsigned k;

			printf("   %-14s", results[i].test->name);
			for (k = 0; k < results[i].n_metrics; k++)
				printf(" %s=%.1f", results[i].metrics[k].name,
				       results[i].metrics[k].value);
			printf("\n");
		}
	if (t_env.opts.json_requested)
		print_json(results, n_run);
	if (tc_failures() || (t_env.opts.strict && tc_skips()))
		printf("FAILED: %d failure(s), %d test(s) skipped\n", tc_failures(), tc_skips());
	else
		printf("PASSED: %u test(s), %d skipped\n", n_run - (unsigned)tc_skips(), tc_skips());
	i = tc_failures() || (t_env.opts.strict && tc_skips());
	free(results);
	ramon_close(t_env.c);
	return (int)i;
}

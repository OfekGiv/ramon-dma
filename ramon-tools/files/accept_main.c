// SPDX-License-Identifier: MIT
/*
 * ramon_accept: the board acceptance test (the manual delivery script of the
 * old dmaapi_testv2, as automatic steps with a verdict each).
 *
 *   ramon_accept [options] [step...]
 *
 * Without names it runs every step in order. Output: a header (tool, library
 * and driver versions, host, kernel, date), "== step" sections with "ok:" /
 * "FAIL:" lines, a summary with one line per step, then PASSED or FAILED.
 * Exit status: 0 passed, 1 failed, 2 usage error.
 */
#include "accept.h"

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define SUMMARY_LEN	120

struct accept_env a_env;

struct result {
	const struct step *step;
	const char *verdict;	/* "PASS", "FAIL" */
	double ms;
	char summary[SUMMARY_LEN];
};

static struct result *cur_result;

/* nn last: nn-update needs a power cycle afterwards */
static const struct step *const groups[] = { a_board_steps, a_ddr4_steps, a_spw_steps, a_spfi_steps,
					     a_mixed_steps, a_nn_steps };

void a_set_summary(const char *fmt, ...)
{
	va_list ap;

	if (!cur_result)
		return;
	va_start(ap, fmt);
	vsnprintf(cur_result->summary, sizeof(cur_result->summary), fmt, ap);
	va_end(ap);
}

int a_check(int ret, const char *what, const struct ramon_status *st)
{
	if (ret)
		tc_fail_st(what, st);
	return ret;
}

static void list_steps(void)
{
	const struct step *s;
	size_t g;

	for (g = 0; g < ARRAY_SIZE(groups); g++)
		for (s = groups[g]; s->name; s++)
			printf("  %-14s %s%s%s%s\n", s->name, s->help,
			       s->legacy_cmd ? " (was " : "", s->legacy_cmd ? s->legacy_cmd : "",
			       s->legacy_cmd ? ")" : "");
}

static const struct step *find_step(const char *name)
{
	const struct step *s;
	size_t g;

	for (g = 0; g < ARRAY_SIZE(groups); g++)
		for (s = groups[g]; s->name; s++)
			if (!strcmp(s->name, name))
				return s;
	return NULL;
}

/* versions, host, kernel and time: the header of the record */
static void print_header(void)
{
	char line[160], host[64] = "?", when[32] = "?";
	struct utsname u;
	struct tm tm;
	time_t now = time(NULL);

	ramon_version_line(a_env.c, line, sizeof(line));
	printf("ramon_accept %s, %s\n", RAMON_TOOLS_VERSION, line);
	gethostname(host, sizeof(host) - 1);
	if (localtime_r(&now, &tm))
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %Z", &tm);
	printf("host %s, kernel %s, %s\n", host, uname(&u) ? "?" : u.release, when);
}

static void usage(FILE *f)
{
	fprintf(f,
		"usage: ramon_accept [options] [step...]\n"
		"  -d, --dev PATH          device (default %s)\n"
		"      --rstop-version V   expected RS-TOP version, MAJOR.MINOR.MINOR_MINOR\n"
		"      --limits FILE       SYSMON limits (default %s if it exists)\n"
		"      --ddr4-label NAME   DT label of the PL DDR4 node (default: search __symbols__)\n"
		"      --ddr4-base ADDR    PL DDR4 physical address (with --ddr4-size: no DT lookup)\n"
		"      --ddr4-size BYTES   PL DDR4 size\n"
		"      --ddr4-mb N         test only the first N MiB of the PL DDR4\n"
		"      --dt PATH           device tree root (default /proc/device-tree)\n"
		"      --mem PATH          physical memory device (default /dev/mem)\n"
		"      --spw-nn LIST       SPW NNs that must be tested, e.g. 0 or 0,1 (default: every synced NN)\n"
		"      --spw-node N        our SPW node id, the loop packets' destination (default %u)\n"
		"      --spw-app N         application type of the loop packets (default %u)\n"
		"      --spw-sizes LIST    payload sizes in bytes (default 1,4096)\n"
		"      --spw-count N       packets per size (default %u)\n"
		"      --spw-seconds N     run each size for N seconds instead of --spw-count packets\n"
		"      --spw-window N      packets in flight (default %u)\n"
		"      --spw-sync-tries N  link resets, 10 ms apart, before a link counts as down (default %u)\n"
		"      --nn-expect TEXT    nn-version: the image version must contain TEXT\n"
		"      --nn-config [NN=]FILE   nn-config: JSON to push (FILE: every tested NN); repeatable\n"
		"      --nn-update [NN=]FILE   nn-update: software package to flash; repeatable\n"
		"      --nn-sw-type N      nn-update: software type in the start packet (default 0)\n"
		"      --spfi-nn LIST      SPFI NNs that must be tested (default: every NN whose link is up)\n"
		"      --spfi-chans A,B    AXI write channel of SPFI NN0,NN1 (default %u,%u)\n"
		"      --spfi-format       spfi-prep FORMATs the NN first: erases every stream (spfifmt)\n"
		"      --spfi-scale PCT    run PCT%% of the SPFI runs and cycles, for a quick check (default 100)\n"
		"      --spfi-long         also run spfi-long (5000 x 200, current measurements)\n"
		"      --spfi-soak         also run spfi-soak (the two 20-minute concurrent runs)\n"
		"      --mixed-seconds N   length of mixed-write and mixed-rw (default 60)\n"
		"      --mixed-spw-bytes N payload of their SPW messages (default and maximum 4096)\n"
		"      --mixed-late-ok     missed deadlines in the mixed tests are reported, not failed\n"
		"      --mixed-report-minutes N  a line per mixed-test user every N minutes (default 1, 0 = off)\n"
		"      --fail-fast         stop at the first failing step\n"
		"  -l, --list              list the steps\n"
		"  -V, --version           print the versions\n",
		RAMON_DEV_PATH, ACCEPT_LIMITS_DEFAULT, RAMON_SPW_NODE_DEFAULT, SPW_APP_DEFAULT,
		SPW_COUNT_DEFAULT, SPW_WINDOW_DEFAULT, SPW_SYNC_TRIES_DEFAULT, RAMON_SPFI_CHAN_NN0,
		RAMON_SPFI_CHAN_NN1);
}

/* FILE or NN=FILE into per_nn[NN] or *all */
static int parse_nn_file(const char *s, const char **per_nn, const char **all)
{
	size_t digits = strspn(s, "0123456789");

	if (digits && s[digits] == '=') {
		unsigned long nn = strtoul(s, NULL, 10);

		if (nn >= RAMON_NN_COUNT || !s[digits + 1])
			return -EINVAL;
		per_nn[nn] = s + digits + 1;
		return 0;
	}
	if (!s[0] || *all)
		return -EINVAL;
	*all = s;
	return 0;
}

/* "1,4096" into at most capacity values, each <= max_value */
static int parse_list(const char *s, uint32_t *values, unsigned capacity, uint32_t max_value,
		      unsigned *n_values)
{
	char buf[256], *tok, *save = NULL;

	if (strlen(s) >= sizeof(buf))
		return -EINVAL;
	strcpy(buf, s);
	*n_values = 0;
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		if (*n_values == capacity || tc_parse_u32(tok, &values[*n_values]) ||
		    values[*n_values] > max_value)
			return -EINVAL;
		(*n_values)++;
	}
	return *n_values ? 0 : -EINVAL;
}

static int parse_version(const char *s, uint32_t v[3])
{
	char tail;

	return sscanf(s, "%u.%u.%u%c", &v[0], &v[1], &v[2], &tail) == 3 ? 0 : -EINVAL;
}

static int parse_opt_u64(const char *s, const char *what, uint64_t *v)
{
	if (!tc_parse_u64(s, v) && *v)
		return 0;
	fprintf(stderr, "ramon_accept: bad %s \"%s\"\n", what, s);
	return -1;
}

int main(int argc, char **argv)
{
	enum { OPT_RSTOP_VERSION = 1, OPT_LIMITS, OPT_DDR4_LABEL, OPT_DDR4_BASE, OPT_DDR4_SIZE,
	       OPT_DDR4_MB, OPT_DT, OPT_MEM, OPT_FAIL_FAST,
	       OPT_SPW_NN, OPT_SPW_NODE, OPT_SPW_APP, OPT_SPW_SIZES, OPT_SPW_COUNT, OPT_SPW_SECONDS,
	       OPT_SPW_WINDOW,
	       OPT_NN_EXPECT, OPT_NN_CONFIG, OPT_NN_UPDATE, OPT_NN_SW_TYPE, OPT_SPW_SYNC_TRIES,
	       OPT_SPFI_NN, OPT_SPFI_FORMAT, OPT_SPFI_SCALE, OPT_SPFI_LONG, OPT_SPFI_SOAK,
	       OPT_SPFI_CHANS,
	       OPT_MIXED_SECONDS, OPT_MIXED_SPW_BYTES, OPT_MIXED_LATE_OK,
	       OPT_MIXED_REPORT_MINUTES };
	static const struct option long_opts[] = {
		{ "dev", required_argument, NULL, 'd' },
		{ "rstop-version", required_argument, NULL, OPT_RSTOP_VERSION },
		{ "limits", required_argument, NULL, OPT_LIMITS },
		{ "ddr4-label", required_argument, NULL, OPT_DDR4_LABEL },
		{ "ddr4-base", required_argument, NULL, OPT_DDR4_BASE },
		{ "ddr4-size", required_argument, NULL, OPT_DDR4_SIZE },
		{ "ddr4-mb", required_argument, NULL, OPT_DDR4_MB },
		{ "dt", required_argument, NULL, OPT_DT },
		{ "mem", required_argument, NULL, OPT_MEM },
		{ "fail-fast", no_argument, NULL, OPT_FAIL_FAST },
		{ "spw-nn", required_argument, NULL, OPT_SPW_NN },
		{ "spw-node", required_argument, NULL, OPT_SPW_NODE },
		{ "spw-app", required_argument, NULL, OPT_SPW_APP },
		{ "spw-sizes", required_argument, NULL, OPT_SPW_SIZES },
		{ "spw-count", required_argument, NULL, OPT_SPW_COUNT },
		{ "spw-seconds", required_argument, NULL, OPT_SPW_SECONDS },
		{ "spw-window", required_argument, NULL, OPT_SPW_WINDOW },
		{ "spw-sync-tries", required_argument, NULL, OPT_SPW_SYNC_TRIES },
		{ "spfi-nn", required_argument, NULL, OPT_SPFI_NN },
		{ "spfi-format", no_argument, NULL, OPT_SPFI_FORMAT },
		{ "spfi-scale", required_argument, NULL, OPT_SPFI_SCALE },
		{ "spfi-long", no_argument, NULL, OPT_SPFI_LONG },
		{ "spfi-soak", no_argument, NULL, OPT_SPFI_SOAK },
		{ "spfi-chans", required_argument, NULL, OPT_SPFI_CHANS },
		{ "mixed-seconds", required_argument, NULL, OPT_MIXED_SECONDS },
		{ "mixed-spw-bytes", required_argument, NULL, OPT_MIXED_SPW_BYTES },
		{ "mixed-late-ok", no_argument, NULL, OPT_MIXED_LATE_OK },
		{ "mixed-report-minutes", required_argument, NULL, OPT_MIXED_REPORT_MINUTES },
		{ "nn-expect", required_argument, NULL, OPT_NN_EXPECT },
		{ "nn-config", required_argument, NULL, OPT_NN_CONFIG },
		{ "nn-update", required_argument, NULL, OPT_NN_UPDATE },
		{ "nn-sw-type", required_argument, NULL, OPT_NN_SW_TYPE },
		{ "list", no_argument, NULL, 'l' },
		{ "version", no_argument, NULL, 'V' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const struct step *s, *selected[32];
	struct ramon_status st;
	struct result *results;
	unsigned n_selected = 0, i, n_run = 0, n_pass = 0, needs = 0;
	char version_line[160], open_err[256] = "";
	uint32_t nn_list[RAMON_NN_COUNT + 1], value;
	uint64_t t0, ddr4_mib;
	size_t g;
	int opt, failures_before;

	a_env.opts.dt_root = "/proc/device-tree";
	a_env.opts.mem_dev = "/dev/mem";
	a_env.opts.spw_node = RAMON_SPW_NODE_DEFAULT;
	a_env.opts.spw_app = SPW_APP_DEFAULT;
	a_env.opts.spw_sizes[0] = 1;
	a_env.opts.spw_sizes[1] = 4096;
	a_env.opts.n_spw_sizes = 2;
	a_env.opts.spw_count = SPW_COUNT_DEFAULT;
	a_env.opts.spw_window = SPW_WINDOW_DEFAULT;
	a_env.opts.spw_sync_tries = SPW_SYNC_TRIES_DEFAULT;
	a_env.opts.spfi_scale_pct = 100;
	a_env.opts.spfi_chan[0] = RAMON_SPFI_CHAN_NN0;
	a_env.opts.spfi_chan[1] = RAMON_SPFI_CHAN_NN1;
	a_env.opts.mixed_seconds = 60;
	a_env.opts.mixed_spw_bytes = RAMON_SPW_MAX_PAYLOAD;
	a_env.opts.mixed_report_minutes = 1;
	while ((opt = getopt_long(argc, argv, "d:lVh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			a_env.opts.dev = optarg;
			break;
		case OPT_RSTOP_VERSION:
			if (parse_version(optarg, a_env.opts.rstop_version)) {
				fprintf(stderr, "ramon_accept: --rstop-version wants MAJOR.MINOR.MINOR_MINOR\n");
				return 2;
			}
			a_env.opts.rstop_version_given = 1;
			break;
		case OPT_LIMITS:
			a_env.opts.limits = optarg;
			a_env.opts.limits_given = 1;
			break;
		case OPT_DDR4_LABEL:
			a_env.opts.ddr4_label = optarg;
			break;
		case OPT_DDR4_BASE:
			if (parse_opt_u64(optarg, "--ddr4-base", &a_env.opts.ddr4_base))
				return 2;
			break;
		case OPT_DDR4_SIZE:
			if (parse_opt_u64(optarg, "--ddr4-size", &a_env.opts.ddr4_size))
				return 2;
			break;
		case OPT_DDR4_MB:
			if (parse_opt_u64(optarg, "--ddr4-mb", &ddr4_mib) ||
			    ddr4_mib > (1ull << 24))
				return 2;
			a_env.opts.ddr4_limit_bytes = ddr4_mib << 20;
			break;
		case OPT_DT:
			a_env.opts.dt_root = optarg;
			break;
		case OPT_MEM:
			a_env.opts.mem_dev = optarg;
			break;
		case OPT_FAIL_FAST:
			a_env.opts.fail_fast = 1;
			break;
		case OPT_SPW_NN:
		case OPT_SPFI_NN:
			if (parse_list(optarg, nn_list, RAMON_NN_COUNT, RAMON_NN_COUNT - 1, &i)) {
				fprintf(stderr, "ramon_accept: %s wants NN numbers 0..%u, e.g. 0,1\n",
					opt == OPT_SPW_NN ? "--spw-nn" : "--spfi-nn",
					RAMON_NN_COUNT - 1);
				return 2;
			}
			while (i--)
				*(opt == OPT_SPW_NN ? &a_env.opts.spw_nn_mask :
				  &a_env.opts.spfi_nn_mask) |= 1u << nn_list[i];
			break;
		case OPT_SPFI_FORMAT:
			a_env.opts.spfi_format_requested = 1;
			break;

		case OPT_SPFI_SCALE:
			if (tc_parse_u32(optarg, &value) || !value || value > 100) {
				fprintf(stderr, "ramon_accept: --spfi-scale wants 1..100\n");
				return 2;
			}
			a_env.opts.spfi_scale_pct = value;
			break;
		case OPT_SPFI_LONG:
			a_env.opts.spfi_long_requested = 1;
			break;
		case OPT_SPFI_SOAK:
			a_env.opts.spfi_soak_requested = 1;
			break;
		case OPT_MIXED_SECONDS:
			if (tc_parse_u32(optarg, &value) || !value || value > 86400) {
				fprintf(stderr, "ramon_accept: --mixed-seconds wants 1..86400\n");
				return 2;
			}
			a_env.opts.mixed_seconds = value;
			break;
		case OPT_MIXED_SPW_BYTES:
			if (tc_parse_u32(optarg, &value) || value > RAMON_SPW_MAX_PAYLOAD) {
				fprintf(stderr, "ramon_accept: --mixed-spw-bytes wants 0..%u (the SPW maximum)\n",
					RAMON_SPW_MAX_PAYLOAD);
				return 2;
			}
			a_env.opts.mixed_spw_bytes = value;
			break;
		case OPT_MIXED_LATE_OK:
			a_env.opts.mixed_late_ok = 1;
			break;
		case OPT_MIXED_REPORT_MINUTES:
			if (tc_parse_u32(optarg, &value) || value > 1440) {
				fprintf(stderr, "ramon_accept: --mixed-report-minutes wants 0..1440\n");
				return 2;
			}
			a_env.opts.mixed_report_minutes = value;
			break;
		case OPT_SPFI_CHANS:
			if (tc_parse_pair(optarg, a_env.opts.spfi_chan)) {
				fprintf(stderr, "ramon_accept: --spfi-chans wants A,B\n");
				return 2;
			}
			break;
		case OPT_SPW_NODE:
		case OPT_SPW_APP:
			if (tc_parse_u32(optarg, &value) ||
			    value > (opt == OPT_SPW_NODE ? 255u : 65535u)) {
				fprintf(stderr, "ramon_accept: bad %s \"%s\"\n",
					opt == OPT_SPW_NODE ? "--spw-node" : "--spw-app", optarg);
				return 2;
			}
			if (opt == OPT_SPW_NODE)
				a_env.opts.spw_node = (uint8_t)value;
			else
				a_env.opts.spw_app = (uint16_t)value;
			break;
		case OPT_SPW_SIZES:
			/* the FPGA takes at most RAMON_SPW_MAX_PAYLOAD bytes */
			if (parse_list(optarg, a_env.opts.spw_sizes, SPW_MAX_SIZES,
				       RAMON_SPW_MAX_PAYLOAD, &a_env.opts.n_spw_sizes)) {
				fprintf(stderr, "ramon_accept: --spw-sizes wants up to %u sizes of 0..%u bytes (the SPW maximum is %u)\n",
					SPW_MAX_SIZES, RAMON_SPW_MAX_PAYLOAD, RAMON_SPW_MAX_PAYLOAD);
				return 2;
			}
			break;
		case OPT_SPW_COUNT:
		case OPT_SPW_SECONDS:
		case OPT_SPW_WINDOW:
		case OPT_SPW_SYNC_TRIES:
			if (tc_parse_u32(optarg, &value) || !value) {
				fprintf(stderr, "ramon_accept: bad value \"%s\"\n", optarg);
				return 2;
			}
			if (opt == OPT_SPW_COUNT)
				a_env.opts.spw_count = value;
			else if (opt == OPT_SPW_SECONDS)
				a_env.opts.spw_seconds = value;
			else if (opt == OPT_SPW_SYNC_TRIES)
				a_env.opts.spw_sync_tries = value;
			else
				a_env.opts.spw_window = value;
			break;
		case OPT_NN_EXPECT:
			a_env.opts.nn_expect = optarg;
			break;
		case OPT_NN_CONFIG:
		case OPT_NN_UPDATE:
			if (opt == OPT_NN_CONFIG ?
			    parse_nn_file(optarg, a_env.opts.nn_config,
					  &a_env.opts.nn_config_all) :
			    parse_nn_file(optarg, a_env.opts.nn_update,
					  &a_env.opts.nn_update_all)) {
				fprintf(stderr, "ramon_accept: %s wants FILE (once) or NN=FILE with NN 0..%u\n",
					opt == OPT_NN_CONFIG ? "--nn-config" : "--nn-update",
					RAMON_NN_COUNT - 1);
				return 2;
			}
			break;
		case OPT_NN_SW_TYPE:
			if (tc_parse_u32(optarg, &value) || value > 255) {
				fprintf(stderr, "ramon_accept: bad --nn-sw-type \"%s\"\n", optarg);
				return 2;
			}
			a_env.opts.nn_sw_type = value;
			break;
		case 'l':
			list_steps();
			return 0;
		case 'V':
			ramon_version_line(NULL, version_line, sizeof(version_line));
			printf("ramon_accept %s, %s\n", RAMON_TOOLS_VERSION, version_line);
			return 0;
		case 'h':
			usage(stdout);
			printf("steps:\n");
			list_steps();
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (!a_env.opts.ddr4_base != !a_env.opts.ddr4_size) {
		fprintf(stderr, "ramon_accept: --ddr4-base and --ddr4-size go together\n");
		return 2;
	}
	if (a_env.opts.ddr4_base && a_env.opts.ddr4_label) {
		fprintf(stderr, "ramon_accept: --ddr4-label and --ddr4-base exclude each other\n");
		return 2;
	}
	if (!a_env.opts.limits && !access(ACCEPT_LIMITS_DEFAULT, F_OK))
		a_env.opts.limits = ACCEPT_LIMITS_DEFAULT;

	/* the steps to run, in table order */
	for (i = (unsigned)optind; i < (unsigned)argc; i++)
		if (!find_step(argv[i])) {
			fprintf(stderr, "ramon_accept: no step \"%s\" (--list)\n", argv[i]);
			return 2;
		}
	for (g = 0; g < ARRAY_SIZE(groups); g++)
		for (s = groups[g]; s->name; s++) {
			/* by default every step whose option, if it needs one, is given */
			int want = optind >= argc && (!s->wanted || s->wanted());

			for (i = (unsigned)optind; i < (unsigned)argc && !want; i++)
				want = !strcmp(argv[i], s->name);
			if (want && n_selected < ARRAY_SIZE(selected))
				selected[n_selected++] = s;
		}
	results = calloc(n_selected ? n_selected : 1, sizeof(*results));
	if (!results) {
		fprintf(stderr, "ramon_accept: out of memory\n");
		return 2;
	}

	/* a device that does not open fails the steps that need it; the others still run */
	for (i = 0; i < n_selected; i++)
		needs |= selected[i]->needs;
	if ((needs & NEEDS_DEV) && ramon_open(a_env.opts.dev, &a_env.c, &st)) {
		a_env.c = NULL;
		ramon_strerror(&st, open_err, sizeof(open_err));
	}
	print_header();

	for (i = 0; i < n_selected; i++) {
		cur_result = &results[n_run++];
		cur_result->step = selected[i];
		printf("== %s\n", selected[i]->name);
		fflush(stdout);
		failures_before = tc_failures();
		t0 = tc_now_ns();
		if ((selected[i]->needs & NEEDS_DEV) && !a_env.c) {
			tc_fail("%s: the device did not open: %s", selected[i]->name, open_err);
			a_set_summary("device not open");
		} else {
			selected[i]->fn();
		}
		cur_result->ms = tc_ms_since(t0);
		cur_result->verdict = tc_failures() == failures_before ? "PASS" : "FAIL";
		if (tc_failures() == failures_before) {
			n_pass++;
			tc_ok("%s (%.0f ms)", selected[i]->name, cur_result->ms);
		} else if (a_env.opts.fail_fast) {
			break;
		}
	}
	cur_result = NULL;

	printf("== summary\n");
	for (i = 0; i < n_run; i++)
		printf("   %-14s %s  %s\n", results[i].step->name, results[i].verdict,
		       results[i].summary);
	if (n_pass == n_selected && !tc_failures())
		printf("PASSED: %u of %u step(s)\n", n_pass, n_selected);
	else
		printf("FAILED: %u of %u step(s) passed\n", n_pass, n_selected);
	i = !(n_pass == n_selected && !tc_failures());
	free(results);
	ramon_close(a_env.c);
	return (int)i;
}

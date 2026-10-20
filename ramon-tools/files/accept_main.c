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

struct accept_env A;

struct result {
	const struct step *s;
	const char *verdict;	/* "PASS", "FAIL" */
	double ms;
	char summary[SUMMARY_LEN];
};

static struct result *cur;

/* nn last: nn-update needs a power cycle afterwards */
static const struct step *const groups[] = { accept_board, accept_ddr4, accept_spw, accept_spfi,
					     accept_mixed, accept_nn };

void a_summary(const char *fmt, ...)
{
	va_list ap;

	if (!cur)
		return;
	va_start(ap, fmt);
	vsnprintf(cur->summary, sizeof(cur->summary), fmt, ap);
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
			printf("  %-14s %s%s%s%s\n", s->name, s->help, s->legacy ? " (was " : "",
			       s->legacy ? s->legacy : "", s->legacy ? ")" : "");
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

	ramon_version_line(A.c, line, sizeof(line));
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

/* "1,4096" into at most max values, each <= limit */
static int parse_list(const char *s, uint32_t *v, unsigned max, uint32_t limit, unsigned *n)
{
	char buf[256], *tok, *save = NULL;

	if (strlen(s) >= sizeof(buf))
		return -EINVAL;
	strcpy(buf, s);
	*n = 0;
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		if (*n == max || tc_parse_u32(tok, &v[*n]) || v[*n] > limit)
			return -EINVAL;
		(*n)++;
	}
	return *n ? 0 : -EINVAL;
}

static int parse_version(const char *s, uint32_t v[3])
{
	char tail;

	return sscanf(s, "%u.%u.%u%c", &v[0], &v[1], &v[2], &tail) == 3 ? 0 : -EINVAL;
}

static int opt_u64(const char *s, const char *what, uint64_t *v)
{
	if (!tc_parse_u64(s, v) && *v)
		return 0;
	fprintf(stderr, "ramon_accept: bad %s \"%s\"\n", what, s);
	return -1;
}

int main(int argc, char **argv)
{
	enum { O_RSTOP = 1, O_LIMITS, O_LABEL, O_BASE, O_SIZE, O_MB, O_DT, O_MEM, O_FAIL_FAST,
	       O_SPW_NN, O_SPW_NODE, O_SPW_APP, O_SPW_SIZES, O_SPW_COUNT, O_SPW_SECONDS, O_SPW_WINDOW,
	       O_NN_EXPECT, O_NN_CONFIG, O_NN_UPDATE, O_NN_SW_TYPE, O_SPW_SYNC,
	       O_SPFI_NN, O_SPFI_FORMAT, O_SPFI_SCALE, O_SPFI_LONG, O_SPFI_SOAK, O_SPFI_CHANS,
	       O_MIXED_SECONDS, O_MIXED_BYTES, O_MIXED_LATE_OK, O_MIXED_REPORT };
	static const struct option opts[] = {
		{ "dev", required_argument, NULL, 'd' },
		{ "rstop-version", required_argument, NULL, O_RSTOP },
		{ "limits", required_argument, NULL, O_LIMITS },
		{ "ddr4-label", required_argument, NULL, O_LABEL },
		{ "ddr4-base", required_argument, NULL, O_BASE },
		{ "ddr4-size", required_argument, NULL, O_SIZE },
		{ "ddr4-mb", required_argument, NULL, O_MB },
		{ "dt", required_argument, NULL, O_DT },
		{ "mem", required_argument, NULL, O_MEM },
		{ "fail-fast", no_argument, NULL, O_FAIL_FAST },
		{ "spw-nn", required_argument, NULL, O_SPW_NN },
		{ "spw-node", required_argument, NULL, O_SPW_NODE },
		{ "spw-app", required_argument, NULL, O_SPW_APP },
		{ "spw-sizes", required_argument, NULL, O_SPW_SIZES },
		{ "spw-count", required_argument, NULL, O_SPW_COUNT },
		{ "spw-seconds", required_argument, NULL, O_SPW_SECONDS },
		{ "spw-window", required_argument, NULL, O_SPW_WINDOW },
		{ "spw-sync-tries", required_argument, NULL, O_SPW_SYNC },
		{ "spfi-nn", required_argument, NULL, O_SPFI_NN },
		{ "spfi-format", no_argument, NULL, O_SPFI_FORMAT },
		{ "spfi-scale", required_argument, NULL, O_SPFI_SCALE },
		{ "spfi-long", no_argument, NULL, O_SPFI_LONG },
		{ "spfi-soak", no_argument, NULL, O_SPFI_SOAK },
		{ "spfi-chans", required_argument, NULL, O_SPFI_CHANS },
		{ "mixed-seconds", required_argument, NULL, O_MIXED_SECONDS },
		{ "mixed-spw-bytes", required_argument, NULL, O_MIXED_BYTES },
		{ "mixed-late-ok", no_argument, NULL, O_MIXED_LATE_OK },
		{ "mixed-report-minutes", required_argument, NULL, O_MIXED_REPORT },
		{ "nn-expect", required_argument, NULL, O_NN_EXPECT },
		{ "nn-config", required_argument, NULL, O_NN_CONFIG },
		{ "nn-update", required_argument, NULL, O_NN_UPDATE },
		{ "nn-sw-type", required_argument, NULL, O_NN_SW_TYPE },
		{ "list", no_argument, NULL, 'l' },
		{ "version", no_argument, NULL, 'V' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const struct step *s, *sel[32];
	struct ramon_status st;
	struct result *res;
	unsigned n_sel = 0, i, n_run = 0, n_pass = 0, needs = 0;
	char ver[160], open_err[256] = "";
	uint32_t list[RAMON_NN_COUNT + 1], v32;
	uint64_t t0, v;
	size_t g;
	int opt, before;

	A.o.dt_root = "/proc/device-tree";
	A.o.mem_dev = "/dev/mem";
	A.o.spw_node = RAMON_SPW_NODE_DEFAULT;
	A.o.spw_app = SPW_APP_DEFAULT;
	A.o.spw_sizes[0] = 1;
	A.o.spw_sizes[1] = 4096;
	A.o.n_spw_sizes = 2;
	A.o.spw_count = SPW_COUNT_DEFAULT;
	A.o.spw_window = SPW_WINDOW_DEFAULT;
	A.o.spw_sync_tries = SPW_SYNC_TRIES_DEFAULT;
	A.o.spfi_scale = 100;
	A.o.spfi_chan[0] = RAMON_SPFI_CHAN_NN0;
	A.o.spfi_chan[1] = RAMON_SPFI_CHAN_NN1;
	A.o.mixed_seconds = 60;
	A.o.mixed_spw_bytes = RAMON_SPW_MAX_PAYLOAD;
	A.o.mixed_report_min = 1;
	while ((opt = getopt_long(argc, argv, "d:lVh", opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			A.o.dev = optarg;
			break;
		case O_RSTOP:
			if (parse_version(optarg, A.o.rstop_expect)) {
				fprintf(stderr, "ramon_accept: --rstop-version wants MAJOR.MINOR.MINOR_MINOR\n");
				return 2;
			}
			A.o.have_rstop_expect = 1;
			break;
		case O_LIMITS:
			A.o.limits = optarg;
			A.o.limits_given = 1;
			break;
		case O_LABEL:
			A.o.ddr4_label = optarg;
			break;
		case O_BASE:
			if (opt_u64(optarg, "--ddr4-base", &A.o.ddr4_base))
				return 2;
			break;
		case O_SIZE:
			if (opt_u64(optarg, "--ddr4-size", &A.o.ddr4_size))
				return 2;
			break;
		case O_MB:
			if (opt_u64(optarg, "--ddr4-mb", &v) || v > (1ull << 24))
				return 2;
			A.o.ddr4_limit = v << 20;
			break;
		case O_DT:
			A.o.dt_root = optarg;
			break;
		case O_MEM:
			A.o.mem_dev = optarg;
			break;
		case O_FAIL_FAST:
			A.o.fail_fast = 1;
			break;
		case O_SPW_NN:
		case O_SPFI_NN:
			if (parse_list(optarg, list, RAMON_NN_COUNT, RAMON_NN_COUNT - 1, &i)) {
				fprintf(stderr, "ramon_accept: %s wants NN numbers 0..%u, e.g. 0,1\n",
					opt == O_SPW_NN ? "--spw-nn" : "--spfi-nn", RAMON_NN_COUNT - 1);
				return 2;
			}
			while (i--)
				*(opt == O_SPW_NN ? &A.o.spw_nn_mask : &A.o.spfi_nn_mask) |= 1u << list[i];
			break;
		case O_SPFI_FORMAT:
			A.o.spfi_format = 1;
			break;

		case O_SPFI_SCALE:
			if (tc_parse_u32(optarg, &v32) || !v32 || v32 > 100) {
				fprintf(stderr, "ramon_accept: --spfi-scale wants 1..100\n");
				return 2;
			}
			A.o.spfi_scale = v32;
			break;
		case O_SPFI_LONG:
			A.o.spfi_long = 1;
			break;
		case O_SPFI_SOAK:
			A.o.spfi_soak = 1;
			break;
		case O_MIXED_SECONDS:
			if (tc_parse_u32(optarg, &v32) || !v32 || v32 > 86400) {
				fprintf(stderr, "ramon_accept: --mixed-seconds wants 1..86400\n");
				return 2;
			}
			A.o.mixed_seconds = v32;
			break;
		case O_MIXED_BYTES:
			if (tc_parse_u32(optarg, &v32) || v32 > RAMON_SPW_MAX_PAYLOAD) {
				fprintf(stderr, "ramon_accept: --mixed-spw-bytes wants 0..%u (the SPW maximum)\n",
					RAMON_SPW_MAX_PAYLOAD);
				return 2;
			}
			A.o.mixed_spw_bytes = v32;
			break;
		case O_MIXED_LATE_OK:
			A.o.mixed_late_ok = 1;
			break;
		case O_MIXED_REPORT:
			if (tc_parse_u32(optarg, &v32) || v32 > 1440) {
				fprintf(stderr, "ramon_accept: --mixed-report-minutes wants 0..1440\n");
				return 2;
			}
			A.o.mixed_report_min = v32;
			break;
		case O_SPFI_CHANS:
			if (tc_parse_pair(optarg, A.o.spfi_chan)) {
				fprintf(stderr, "ramon_accept: --spfi-chans wants A,B\n");
				return 2;
			}
			break;
		case O_SPW_NODE:
		case O_SPW_APP:
			if (tc_parse_u32(optarg, &v32) || v32 > (opt == O_SPW_NODE ? 255u : 65535u)) {
				fprintf(stderr, "ramon_accept: bad %s \"%s\"\n",
					opt == O_SPW_NODE ? "--spw-node" : "--spw-app", optarg);
				return 2;
			}
			if (opt == O_SPW_NODE)
				A.o.spw_node = (uint8_t)v32;
			else
				A.o.spw_app = (uint16_t)v32;
			break;
		case O_SPW_SIZES:
			/* the FPGA takes at most RAMON_SPW_MAX_PAYLOAD bytes */
			if (parse_list(optarg, A.o.spw_sizes, SPW_MAX_SIZES, RAMON_SPW_MAX_PAYLOAD,
				       &A.o.n_spw_sizes)) {
				fprintf(stderr, "ramon_accept: --spw-sizes wants up to %u sizes of 0..%u bytes (the SPW maximum is %u)\n",
					SPW_MAX_SIZES, RAMON_SPW_MAX_PAYLOAD, RAMON_SPW_MAX_PAYLOAD);
				return 2;
			}
			break;
		case O_SPW_COUNT:
		case O_SPW_SECONDS:
		case O_SPW_WINDOW:
		case O_SPW_SYNC:
			if (tc_parse_u32(optarg, &v32) || !v32) {
				fprintf(stderr, "ramon_accept: bad value \"%s\"\n", optarg);
				return 2;
			}
			if (opt == O_SPW_COUNT)
				A.o.spw_count = v32;
			else if (opt == O_SPW_SECONDS)
				A.o.spw_seconds = v32;
			else if (opt == O_SPW_SYNC)
				A.o.spw_sync_tries = v32;
			else
				A.o.spw_window = v32;
			break;
		case O_NN_EXPECT:
			A.o.nn_expect = optarg;
			break;
		case O_NN_CONFIG:
		case O_NN_UPDATE:
			if (opt == O_NN_CONFIG ?
			    parse_nn_file(optarg, A.o.nn_config, &A.o.nn_config_all) :
			    parse_nn_file(optarg, A.o.nn_update, &A.o.nn_update_all)) {
				fprintf(stderr, "ramon_accept: %s wants FILE (once) or NN=FILE with NN 0..%u\n",
					opt == O_NN_CONFIG ? "--nn-config" : "--nn-update", RAMON_NN_COUNT - 1);
				return 2;
			}
			break;
		case O_NN_SW_TYPE:
			if (tc_parse_u32(optarg, &v32) || v32 > 255) {
				fprintf(stderr, "ramon_accept: bad --nn-sw-type \"%s\"\n", optarg);
				return 2;
			}
			A.o.nn_sw_type = v32;
			break;
		case 'l':
			list_steps();
			return 0;
		case 'V':
			ramon_version_line(NULL, ver, sizeof(ver));
			printf("ramon_accept %s, %s\n", RAMON_TOOLS_VERSION, ver);
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
	if (!A.o.ddr4_base != !A.o.ddr4_size) {
		fprintf(stderr, "ramon_accept: --ddr4-base and --ddr4-size go together\n");
		return 2;
	}
	if (A.o.ddr4_base && A.o.ddr4_label) {
		fprintf(stderr, "ramon_accept: --ddr4-label and --ddr4-base exclude each other\n");
		return 2;
	}
	if (!A.o.limits && !access(ACCEPT_LIMITS_DEFAULT, F_OK))
		A.o.limits = ACCEPT_LIMITS_DEFAULT;

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
			if (want && n_sel < ARRAY_SIZE(sel))
				sel[n_sel++] = s;
		}
	res = calloc(n_sel ? n_sel : 1, sizeof(*res));
	if (!res) {
		fprintf(stderr, "ramon_accept: out of memory\n");
		return 2;
	}

	/* a device that does not open fails the steps that need it; the others still run */
	for (i = 0; i < n_sel; i++)
		needs |= sel[i]->needs;
	if ((needs & NEEDS_DEV) && ramon_open(A.o.dev, &A.c, &st)) {
		A.c = NULL;
		ramon_strerror(&st, open_err, sizeof(open_err));
	}
	print_header();

	for (i = 0; i < n_sel; i++) {
		cur = &res[n_run++];
		cur->s = sel[i];
		printf("== %s\n", sel[i]->name);
		fflush(stdout);
		before = tc_failures();
		t0 = tc_now_ns();
		if ((sel[i]->needs & NEEDS_DEV) && !A.c) {
			tc_fail("%s: the device did not open: %s", sel[i]->name, open_err);
			a_summary("device not open");
		} else {
			sel[i]->fn();
		}
		cur->ms = tc_ms_since(t0);
		cur->verdict = tc_failures() == before ? "PASS" : "FAIL";
		if (tc_failures() == before) {
			n_pass++;
			tc_ok("%s (%.0f ms)", sel[i]->name, cur->ms);
		} else if (A.o.fail_fast) {
			break;
		}
	}
	cur = NULL;

	printf("== summary\n");
	for (i = 0; i < n_run; i++)
		printf("   %-14s %s  %s\n", res[i].s->name, res[i].verdict, res[i].summary);
	if (n_pass == n_sel && !tc_failures())
		printf("PASSED: %u of %u step(s)\n", n_pass, n_sel);
	else
		printf("FAILED: %u of %u step(s) passed\n", n_pass, n_sel);
	i = !(n_pass == n_sel && !tc_failures());
	free(res);
	ramon_close(A.c);
	return (int)i;
}

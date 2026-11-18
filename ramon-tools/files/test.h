/* SPDX-License-Identifier: MIT */
/* ramon_test internals */
#ifndef TEST_H
#define TEST_H

#include "tools_common.h"

/* struct test.needs */
#define NEEDS_ZDMA	0x01	/* ZDMA channels */
#define NEEDS_SPW	0x02	/* an SPW NN present */
#define NEEDS_SPW_LINK	0x04	/* a synced SPW link */
#define NEEDS_SPFI	0x08	/* an SPFI NN present */
#define NEEDS_SPFI_LINK	0x10	/* an SPFI link up (0xCC low byte 0x88) */

/* struct test.when */
#define RUN_DEFAULT	0	/* in the default run */
#define RUN_LONG	1	/* with --long, or by name */
#define RUN_SOAK	2	/* with --minutes, or by name */

struct test_opts {
	const char *dev;
	uint8_t node;
	uint8_t target;
	uint32_t spfi_chan[RAMON_NN_COUNT];
	uint32_t spfi_perf_max_pages;
	uint32_t runs;
	uint32_t spw_max_payload_bytes;
	uint32_t soak_minutes;
	int prep_requested;
	int format_requested;
	int skip_spw;
	int skip_spfi;
	int long_requested;
	int strict;
	int json_requested;
	int fail_fast;
};

struct test_env {
	ramon_ctx *c;
	struct test_opts opts;
	int spw_nn;			/* first synced SPW NN, or -1 */
	int spfi_nn;			/* first SPFI NN whose link is up, or -1 */
	uint32_t spw_present_mask;	/* masks */
	uint32_t spfi_present_mask;
	uint32_t n_zdma_chan;
};

extern struct test_env t_env;

struct test {
	const char *name;
	void (*fn)(void);
	int when;
	unsigned needs;
	const char *help;
};

/* a number for --json and the summary of the current test */
void t_add_metric(const char *name, double value);
/* reports a failed call; returns ret */
int t_check(int ret, const char *what, const struct ramon_status *st);

/* SPW helpers shared with the core and soak tests (test_spw.c) */
/* sends n loopback packets on NN nn and returns how many came back intact */
unsigned t_spw_loop_packets(uint32_t nn, unsigned n);

extern const struct test t_core_tests[];
extern const struct test t_spw_tests[];
extern const struct test t_spfi_tests[];
extern const struct test t_soak_tests[];

#endif /* TEST_H */

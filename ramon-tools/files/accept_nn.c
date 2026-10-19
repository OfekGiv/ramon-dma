// SPDX-License-Identifier: MIT
/*
 * ramon_accept: NN services over SpaceWire.
 *
 * nn-version  software versions of each tested NN (was nnswver)
 * nn-config   JSON configuration pushed and acknowledged (was nnconf);
 *             runs only with --nn-config
 * nn-update   NN software update (was nnupdate); runs only with --nn-update,
 *             and last: the board needs a power cycle afterwards
 *
 * --nn-config and --nn-update take FILE (every tested NN) or NN=FILE (that
 * NN; given for an NN, it overrides FILE).
 */
#include "accept.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPDATE_MAX	(0xFFFFull * RAMON_SPW_SWUP_CHUNK)	/* the packet index is 16 bits */
#define PROGRESS_STEPS	10

/* the file for NN nn, or NULL */
static const char *file_for(const char *const per_nn[RAMON_NN_COUNT], const char *all, uint32_t nn)
{
	return per_nn[nn] ? per_nn[nn] : all;
}

/* NNs with a file; an NN named in NN=FILE must be tested (its link synced) */
static uint32_t nns_with_file(const char *const per_nn[RAMON_NN_COUNT], const char *all,
			      uint32_t use)
{
	uint32_t nn, mask = 0;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (per_nn[nn] && !(use & (1u << nn)))
			tc_fail("spw%u: a file is given for this NN but its link is not synced", nn);
		else if ((use & (1u << nn)) && file_for(per_nn, all, nn))
			mask |= 1u << nn;
	}
	return mask;
}

/* ---- nn-version (was nnswver) ---- */

static void step_nn_version(void)
{
	struct ramon_nn_version v;
	struct ramon_status st;
	char sum[160] = "";
	size_t used = 0;
	uint32_t use, nn;

	use = a_spw_setup();
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(use & (1u << nn)))
			continue;
		if (a_check(ramon_spw_nn_version(A.c, nn, &v, 0, &st), "NN version request", &st))
			continue;
		tc_info("spw%u: 4KBL version:  %s", nn, v.kbl);
		tc_info("spw%u: RSBL version:  %s", nn, v.rsbl);
		tc_info("spw%u: image version: %s", nn, v.image);
		tc_info("spw%u: FW version:    %s", nn, v.fw);
		if (!v.image[0] && !v.kbl[0] && !v.rsbl[0] && !v.fw[0])
			tc_fail("spw%u: the NN answered with empty version strings", nn);
		if (A.o.nn_expect && !strstr(v.image, A.o.nn_expect))
			tc_fail("spw%u: image version \"%s\" does not contain \"%s\" (--nn-expect)", nn,
				v.image, A.o.nn_expect);
		if (used < sizeof(sum))
			used += (size_t)snprintf(sum + used, sizeof(sum) - used, "%sspw%u image %s",
						 used ? ", " : "", nn, v.image[0] ? v.image : "(empty)");
	}
	if (use && !A.o.nn_expect)
		tc_info("not compared: pass --nn-expect to check the image version");
	a_summary("%s", sum[0] ? sum : "no NN answered");
}

/* ---- nn-config (was nnconf) ---- */

static int want_config(void)
{
	unsigned nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		if (A.o.nn_config[nn])
			return 1;
	return A.o.nn_config_all != NULL;
}

static void step_nn_config(void)
{
	struct ramon_status st;
	uint32_t use, nn, done = 0;
	const char *path;
	uint8_t *data;
	size_t len;
	int ret;

	if (!want_config()) {
		tc_fail("nn-config needs --nn-config FILE or --nn-config NN=FILE");
		a_summary("no configuration file given");
		return;
	}
	use = nns_with_file(A.o.nn_config, A.o.nn_config_all, a_spw_setup());
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(use & (1u << nn)))
			continue;
		path = file_for(A.o.nn_config, A.o.nn_config_all, nn);
		ret = tc_read_file(path, &data, &len, RAMON_SPW_CONFIG_MAX);
		if (ret) {
			tc_fail("spw%u: %s: %s%s", nn, path, strerror(-ret),
				ret == -EFBIG ? " (an NN configuration is at most 4096 bytes)" : "");
			continue;
		}
		if (!len) {
			tc_fail("spw%u: %s is empty", nn, path);
			free(data);
			continue;
		}
		tc_info("spw%u: sending %s (%zu bytes)", nn, path, len);
		if (!a_check(ramon_spw_nn_config_push(A.c, nn, data, (uint32_t)len, 0, &st),
			     "NN configuration", &st)) {
			tc_info("spw%u: configuration acknowledged", nn);
			done++;
		}
		free(data);
	}
	a_summary("%u NN(s) configured", done);
}

/* ---- nn-update (was nnupdate) ---- */

static int want_update(void)
{
	unsigned nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		if (A.o.nn_update[nn])
			return 1;
	return A.o.nn_update_all != NULL;
}

struct progress {
	uint32_t nn;
	unsigned shown;
	uint64_t t0;
};

/* a line per tenth: the output is often a log file, so no carriage returns */
static void update_progress(void *user, uint32_t done, uint32_t total)
{
	struct progress *p = user;
	unsigned step = total ? (unsigned)((uint64_t)done * PROGRESS_STEPS / total) : PROGRESS_STEPS;

	if (step > p->shown) {
		p->shown = step;
		tc_info("spw%u: packet %u / %u (%.0f s)", p->nn, done, total, tc_ms_since(p->t0) / 1000);
	}
}

static void step_nn_update(void)
{
	struct ramon_status st;
	struct progress pr;
	uint32_t use, nn, done = 0;
	const char *path;
	uint8_t *img;
	size_t len;
	int ret;

	if (!want_update()) {
		tc_fail("nn-update needs --nn-update FILE or --nn-update NN=FILE");
		a_summary("no software package given");
		return;
	}
	use = nns_with_file(A.o.nn_update, A.o.nn_update_all, a_spw_setup());
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(use & (1u << nn)))
			continue;
		path = file_for(A.o.nn_update, A.o.nn_update_all, nn);
		ret = tc_read_file(path, &img, &len, UPDATE_MAX);
		if (ret || !len) {
			tc_fail("spw%u: %s: %s", nn, path, ret ? strerror(-ret) : "empty file");
			if (!ret)
				free(img);
			continue;
		}
		tc_info("spw%u: updating with %s (%zu bytes, %zu packets, software type %u)", nn, path,
			len, (len + RAMON_SPW_SWUP_CHUNK - 1) / RAMON_SPW_SWUP_CHUNK, A.o.nn_sw_type);
		pr.nn = nn;
		pr.shown = 0;
		pr.t0 = tc_now_ns();
		if (!a_check(ramon_spw_nn_sw_update(A.c, nn, (uint8_t)A.o.nn_sw_type, img, len,
						    update_progress, &pr, &st),
			     "NN software update", &st)) {
			tc_info("spw%u: software update accepted by the NN", nn);
			done++;
		}
		free(img);
	}
	if (done)
		tc_info("power-cycle the board, then check with: ramon_accept nn-version --nn-expect <version>");
	a_summary("%u NN(s) updated%s", done, done ? "; power-cycle, then run nn-version" : "");
}

const struct step accept_nn[] = {
	{ "nn-version", step_nn_version, NEEDS_DEV, "nnswver",
	  "NN software versions (4KBL, RSBL, image, FW)", NULL },
	{ "nn-config", step_nn_config, NEEDS_DEV, "nnconf",
	  "NN JSON configuration acknowledged (only with --nn-config)", want_config },
	{ "nn-update", step_nn_update, NEEDS_DEV, "nnupdate",
	  "NN software update, then power-cycle (only with --nn-update)", want_update },
	{ NULL, NULL, 0, NULL, NULL, NULL },
};

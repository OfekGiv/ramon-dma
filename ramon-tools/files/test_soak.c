// SPDX-License-Identifier: MIT
/*
 * ramon_test: soak. Mixed load for --minutes: ZDMA copies, buffer churn,
 * register and counter reads, NN version round trips over SPW (RX thread
 * running), and SPFI table reads plus reads of one stream written once at the
 * start. Like ramon_smoke --stress it does not keep writing SPFI data, to
 * spare the NN's storage.
 */
#include "test.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPORT_S	30
#define SPFI_PAGES	8
#define SPW_PERIOD_MS	100

enum { W_ZDMA, W_CHURN, W_REGS, W_STATS, W_SPW, W_SPFI, W_COUNT };

static const char *const worker_names[W_COUNT] = { "zdma", "churn", "regs", "stats", "spw",
						   "spfi" };

struct worker {
	int kind;
	pthread_t th;
	uint64_t ops;		/* __atomic */
	uint64_t errors;	/* __atomic */
	int started;
};

static struct worker workers[W_COUNT];
static int stop_requested;	/* __atomic */
static struct ramon_spfi_stream soak_stream;

static void soak_fail(struct worker *w, const char *what, const struct ramon_status *st)
{
	if (__atomic_add_fetch(&w->errors, 1, __ATOMIC_RELAXED) <= 3)
		tc_fail_st(what, st);
}

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	struct ramon_status st;
	ramon_buf src, dst;
	unsigned seed = (unsigned)w->kind * 7919u + 1;
	int ret;

	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	if ((w->kind == W_ZDMA || w->kind == W_SPFI) &&
	    (ramon_buf_alloc(t_env.c, (uint64_t)SPFI_PAGES * RAMON_SPFI_PAGE, 0, &src, &st) ||
	     ramon_buf_alloc(t_env.c, (uint64_t)SPFI_PAGES * RAMON_SPFI_PAGE, 0, &dst, &st))) {
		soak_fail(w, "alloc", &st);
		goto out;
	}
	if (w->kind == W_ZDMA)
		tc_fill(src.ptr, src.size, 0x50A50000u);
	while (!__atomic_load_n(&stop_requested, __ATOMIC_RELAXED)) {
		ret = 0;
		switch (w->kind) {
		case W_ZDMA:
			ret = ramon_zdma_copy1(t_env.c, &src, 0, &dst, 0, 65536, 0, &st);
			if (!ret && (w->ops & 63) == 0 && memcmp(src.ptr, dst.ptr, 65536))
				ramon_status_set(&st, RAMON_EL_INVAL, -EIO, 0, 0, "copy differs"), ret = -EIO;
			break;
		case W_CHURN: {
			ramon_buf churn_buf;
			uint64_t size = 4096 + (seed = seed * 1103515245u + 12345u) % (256 * 1024);

			ret = ramon_buf_alloc(t_env.c, size, 0, &churn_buf, &st);
			if (!ret) {
				tc_fill(churn_buf.ptr, 4096, seed);
				if (tc_find_mismatch(churn_buf.ptr, 4096, seed) >= 0)
					ramon_status_set(&st, RAMON_EL_INVAL, -EIO, 0, 0,
							 "buffer reads back wrong"), ret = -EIO;
				ramon_buf_free(&churn_buf, NULL);
			}
			break;
		}
		case W_REGS: {
			struct ramon_sysmon s;

			ret = ramon_sysmon_read(t_env.c, &s, &st);
			break;
		}
		case W_STATS: {
			struct ramon_get_stats g;

			ret = ramon_get_stats(t_env.c, 0, &g, &st);
			tc_sleep_ms(10);
			break;
		}
		case W_SPW: {
			struct ramon_nn_version v;

			ret = ramon_spw_nn_version(t_env.c, (uint32_t)t_env.spw_nn, &v, 0, &st);
			tc_sleep_ms(SPW_PERIOD_MS);
			break;
		}
		case W_SPFI: {
			struct ramon_spfi_table *t = malloc(sizeof(*t));

			ret = t ? ramon_spfi_get_table(t_env.c, soak_stream.nn, t, &st) : -ENOMEM;
			free(t);
			if (!ret) {
				memset(dst.ptr, 0, dst.size);
				ret = ramon_spfi_read_seq(t_env.c, soak_stream.nn, soak_stream.sid,
							  soak_stream.first_page, SPFI_PAGES, &dst,
							  0, 0, &st);
			}
			if (!ret && tc_find_mismatch(dst.ptr, dst.size, 0x50F10000u) >= 0)
				ramon_status_set(&st, RAMON_EL_INVAL, -EIO, 0, 0, "stream data differs"),
					ret = -EIO;
			break;
		}
		}
		if (ret)
			soak_fail(w, worker_names[w->kind], &st);
		else
			__atomic_add_fetch(&w->ops, 1, __ATOMIC_RELAXED);
	}
out:
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
	return NULL;
}

static void soak_cb(void *user, const struct ramon_spw_rx *rx)
{
	(void)user;
	(void)rx;
}

/* the stream the SPFI worker reads: written and flushed once */
static int soak_spfi_setup(void)
{
	uint32_t nn = (uint32_t)t_env.spfi_nn, sid;
	struct ramon_spfi_table *t = malloc(sizeof(*t));
	struct ramon_status st;
	ramon_buf b;
	int ret;

	memset(&soak_stream, 0, sizeof(soak_stream));
	if (!t)
		return -1;
	ret = t_check(ramon_spfi_get_table(t_env.c, nn, t, &st), "stream table", &st);
	if (!ret && ramon_spfi_find_free(t, &sid, 1) != 1) {
		tc_fail("no free stream id");
		ret = -1;
	}
	free(t);
	if (ret || t_check(ramon_buf_alloc(t_env.c, (uint64_t)SPFI_PAGES * RAMON_SPFI_PAGE, 0,
					   &b, &st), "alloc", &st))
		return -1;
	tc_fill(b.ptr, b.size, 0x50F10000u);
	ret = t_check(ramon_spfi_stream_open(t_env.c, nn, sid, SPFI_PAGES, &soak_stream, &st),
		      "open", &st);
	if (!ret)
		ret = t_check(ramon_spfi_stream_append(t_env.c, &soak_stream, &b, 0, SPFI_PAGES, 0,
						       &st), "write", &st);
	if (!ret)
		ret = t_check(ramon_spfi_stream_flush(t_env.c, &soak_stream, &st), "flush", &st);
	ramon_buf_free(&b, NULL);
	return ret;
}

static void run_soak(void)
{
	uint64_t end = tc_now_ns() + (uint64_t)t_env.opts.soak_minutes * 60 * 1000000000ull, next;
	struct ramon_status st;
	int k, spw_rx_on = 0;

	__atomic_store_n(&stop_requested, 0, __ATOMIC_RELAXED);
	memset(workers, 0, sizeof(workers));
	if (t_env.spw_nn >= 0 &&
	    !t_check(ramon_spw_rx_start(t_env.c, (uint32_t)t_env.spw_nn, soak_cb, NULL, &st),
		     "RX thread", &st))
		spw_rx_on = 1;
	for (k = 0; k < W_COUNT; k++) {
		workers[k].kind = k;
		if ((k == W_ZDMA && !t_env.n_zdma_chan) || (k == W_SPW && !spw_rx_on) ||
		    (k == W_SPFI && (t_env.spfi_nn < 0 || soak_spfi_setup())))
			continue;
		if (pthread_create(&workers[k].th, NULL, worker_main, &workers[k]))
			tc_fail("pthread_create");
		else
			workers[k].started = 1;
	}
	tc_info("soak for %u minute(s): %s%s%s%s%s%s", t_env.opts.soak_minutes,
		workers[W_ZDMA].started ? "zdma " : "",
		workers[W_CHURN].started ? "churn " : "", workers[W_REGS].started ? "regs " : "",
		workers[W_STATS].started ? "stats " : "", workers[W_SPW].started ? "spw " : "",
		workers[W_SPFI].started ? "spfi" : "");
	next = tc_now_ns() + REPORT_S * 1000000000ull;
	while (tc_now_ns() < end) {
		tc_sleep_ms(500);
		if (tc_now_ns() < next || tc_now_ns() >= end)
			continue;
		next += REPORT_S * 1000000000ull;
		flockfile(stdout);
		printf("      %4.0f s left:", (double)(end - tc_now_ns()) / 1e9);
		for (k = 0; k < W_COUNT; k++)
			if (workers[k].started)
				printf(" %s %llu/%llu", worker_names[k],
				       (unsigned long long)__atomic_load_n(&workers[k].ops,
									   __ATOMIC_RELAXED),
				       (unsigned long long)__atomic_load_n(&workers[k].errors,
									   __ATOMIC_RELAXED));
		printf(" (ops/errors)\n");
		fflush(stdout);
		funlockfile(stdout);
	}
	__atomic_store_n(&stop_requested, 1, __ATOMIC_RELAXED);
	for (k = 0; k < W_COUNT; k++)
		if (workers[k].started)
			pthread_join(workers[k].th, NULL);
	if (spw_rx_on)
		ramon_spw_rx_stop(t_env.c, (uint32_t)t_env.spw_nn);
	if (soak_stream.open || soak_stream.pages_written)
		t_check(ramon_spfi_stream_close(t_env.c, &soak_stream, 1, &st), "close + delete",
			&st);
	for (k = 0; k < W_COUNT; k++)
		if (workers[k].started) {
			tc_info("%-5s %llu ops, %llu errors", worker_names[k],
				(unsigned long long)workers[k].ops,
				(unsigned long long)workers[k].errors);
			t_add_metric(worker_names[k], (double)workers[k].ops);
		}
}

const struct test t_soak_tests[] = {
	{ "soak", run_soak, RUN_SOAK, 0, "mixed multi-thread load for --minutes, zero errors" },
	{ NULL, NULL, 0, 0, NULL },
};

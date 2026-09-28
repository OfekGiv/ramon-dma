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

static const char *const wname[W_COUNT] = { "zdma", "churn", "regs", "stats", "spw", "spfi" };

struct worker {
	int kind;
	pthread_t th;
	uint64_t ops;		/* __atomic */
	uint64_t errors;	/* __atomic */
	int running;
};

static struct worker W[W_COUNT];
static int stop;		/* __atomic */
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
	ramon_buf a, b;
	unsigned seed = (unsigned)w->kind * 7919u + 1;
	int ret;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	if ((w->kind == W_ZDMA || w->kind == W_SPFI) &&
	    (ramon_buf_alloc(T.c, (uint64_t)SPFI_PAGES * RAMON_SPFI_PAGE, 0, &a, &st) ||
	     ramon_buf_alloc(T.c, (uint64_t)SPFI_PAGES * RAMON_SPFI_PAGE, 0, &b, &st))) {
		soak_fail(w, "alloc", &st);
		goto out;
	}
	if (w->kind == W_ZDMA)
		tc_fill(a.ptr, a.size, 0x50A50000u);
	while (!__atomic_load_n(&stop, __ATOMIC_RELAXED)) {
		ret = 0;
		switch (w->kind) {
		case W_ZDMA:
			ret = ramon_zdma_copy1(T.c, &a, 0, &b, 0, 65536, 0, &st);
			if (!ret && (w->ops & 63) == 0 && memcmp(a.ptr, b.ptr, 65536))
				ramon_status_set(&st, RAMON_EL_INVAL, -EIO, 0, 0, "copy differs"), ret = -EIO;
			break;
		case W_CHURN: {
			ramon_buf c;
			uint64_t size = 4096 + (seed = seed * 1103515245u + 12345u) % (256 * 1024);

			ret = ramon_buf_alloc(T.c, size, 0, &c, &st);
			if (!ret) {
				tc_fill(c.ptr, 4096, seed);
				if (tc_verify(c.ptr, 4096, seed) >= 0)
					ramon_status_set(&st, RAMON_EL_INVAL, -EIO, 0, 0,
							 "buffer reads back wrong"), ret = -EIO;
				ramon_buf_free(&c, NULL);
			}
			break;
		}
		case W_REGS: {
			struct ramon_sysmon s;

			ret = ramon_sysmon_read(T.c, &s, &st);
			break;
		}
		case W_STATS: {
			struct ramon_get_stats g;

			ret = ramon_get_stats(T.c, 0, &g, &st);
			tc_sleep_ms(10);
			break;
		}
		case W_SPW: {
			struct ramon_nn_version v;

			ret = ramon_spw_nn_version(T.c, (uint32_t)T.spw_nn, &v, 0, &st);
			tc_sleep_ms(SPW_PERIOD_MS);
			break;
		}
		case W_SPFI: {
			struct ramon_spfi_table *t = malloc(sizeof(*t));

			ret = t ? ramon_spfi_get_table(T.c, soak_stream.nn, t, &st) : -ENOMEM;
			free(t);
			if (!ret) {
				memset(b.ptr, 0, b.size);
				ret = ramon_spfi_read_seq(T.c, soak_stream.nn, soak_stream.sid,
							  soak_stream.first_page, SPFI_PAGES, &b, 0, 0, &st);
			}
			if (!ret && tc_verify(b.ptr, b.size, 0x50F10000u) >= 0)
				ramon_status_set(&st, RAMON_EL_INVAL, -EIO, 0, 0, "stream data differs"),
					ret = -EIO;
			break;
		}
		}
		if (ret)
			soak_fail(w, wname[w->kind], &st);
		else
			__atomic_add_fetch(&w->ops, 1, __ATOMIC_RELAXED);
	}
out:
	ramon_buf_free(&a, NULL);
	ramon_buf_free(&b, NULL);
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
	uint32_t nn = (uint32_t)T.spfi_nn, sid;
	struct ramon_spfi_table *t = malloc(sizeof(*t));
	struct ramon_status st;
	ramon_buf b;
	int ret;

	memset(&soak_stream, 0, sizeof(soak_stream));
	if (!t)
		return -1;
	ret = t_check(ramon_spfi_get_table(T.c, nn, t, &st), "stream table", &st);
	if (!ret && ramon_spfi_find_free(t, &sid, 1) != 1) {
		tc_fail("no free stream id");
		ret = -1;
	}
	free(t);
	if (ret || t_check(ramon_buf_alloc(T.c, (uint64_t)SPFI_PAGES * RAMON_SPFI_PAGE, 0, &b, &st),
			   "alloc", &st))
		return -1;
	tc_fill(b.ptr, b.size, 0x50F10000u);
	ret = t_check(ramon_spfi_stream_open(T.c, nn, sid, SPFI_PAGES, &soak_stream, &st), "open", &st);
	if (!ret)
		ret = t_check(ramon_spfi_stream_append(T.c, &soak_stream, &b, 0, SPFI_PAGES, 0, &st),
			      "write", &st);
	if (!ret)
		ret = t_check(ramon_spfi_stream_flush(T.c, &soak_stream, &st), "flush", &st);
	ramon_buf_free(&b, NULL);
	return ret;
}

static void run_soak(void)
{
	uint64_t end = tc_now_ns() + (uint64_t)T.o.minutes * 60 * 1000000000ull, next;
	struct ramon_status st;
	int k, spw = 0;

	__atomic_store_n(&stop, 0, __ATOMIC_RELAXED);
	memset(W, 0, sizeof(W));
	if (T.spw_nn >= 0 &&
	    !t_check(ramon_spw_rx_start(T.c, (uint32_t)T.spw_nn, soak_cb, NULL, &st), "RX thread", &st))
		spw = 1;
	for (k = 0; k < W_COUNT; k++) {
		W[k].kind = k;
		if ((k == W_ZDMA && !T.n_zdma) || (k == W_SPW && !spw) ||
		    (k == W_SPFI && (T.spfi_nn < 0 || soak_spfi_setup())))
			continue;
		if (pthread_create(&W[k].th, NULL, worker_main, &W[k]))
			tc_fail("pthread_create");
		else
			W[k].running = 1;
	}
	tc_info("soak for %u minute(s): %s%s%s%s%s%s", T.o.minutes, W[W_ZDMA].running ? "zdma " : "",
		W[W_CHURN].running ? "churn " : "", W[W_REGS].running ? "regs " : "",
		W[W_STATS].running ? "stats " : "", W[W_SPW].running ? "spw " : "",
		W[W_SPFI].running ? "spfi" : "");
	next = tc_now_ns() + REPORT_S * 1000000000ull;
	while (tc_now_ns() < end) {
		tc_sleep_ms(500);
		if (tc_now_ns() < next || tc_now_ns() >= end)
			continue;
		next += REPORT_S * 1000000000ull;
		flockfile(stdout);
		printf("      %4.0f s left:", (double)(end - tc_now_ns()) / 1e9);
		for (k = 0; k < W_COUNT; k++)
			if (W[k].running)
				printf(" %s %llu/%llu", wname[k],
				       (unsigned long long)__atomic_load_n(&W[k].ops, __ATOMIC_RELAXED),
				       (unsigned long long)__atomic_load_n(&W[k].errors, __ATOMIC_RELAXED));
		printf(" (ops/errors)\n");
		fflush(stdout);
		funlockfile(stdout);
	}
	__atomic_store_n(&stop, 1, __ATOMIC_RELAXED);
	for (k = 0; k < W_COUNT; k++)
		if (W[k].running)
			pthread_join(W[k].th, NULL);
	if (spw)
		ramon_spw_rx_stop(T.c, (uint32_t)T.spw_nn);
	if (soak_stream.open || soak_stream.pages_written)
		t_check(ramon_spfi_stream_close(T.c, &soak_stream, 1, &st), "close + delete", &st);
	for (k = 0; k < W_COUNT; k++)
		if (W[k].running) {
			tc_info("%-5s %llu ops, %llu errors", wname[k], (unsigned long long)W[k].ops,
				(unsigned long long)W[k].errors);
			t_metric(wname[k], (double)W[k].ops);
		}
}

const struct test test_soak[] = {
	{ "soak", run_soak, RUN_SOAK, 0, "mixed multi-thread load for --minutes, zero errors" },
	{ NULL, NULL, 0, 0, NULL },
};

// SPDX-License-Identifier: MIT
/* ramon_test: SPFI tests (stream write-read, performance, concurrency, alerts, rules) */
#include "test.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE		RAMON_SPFI_PAGE
#define STREAM_PAGES	8
#define ROUNDS		3
#define MAXLIST_ITEMS	RAMON_SPFI_WRITE_MAX_PAGES
#define MAXLIST_CHUNK	64

static struct ramon_spfi_table *table(uint32_t nn)
{
	struct ramon_spfi_table *t = malloc(sizeof(*t));
	struct ramon_status st;

	if (!t) {
		tc_fail("out of memory");
		return NULL;
	}
	if (t_check(ramon_spfi_get_table(T.c, nn, t, &st), "stream table", &st)) {
		free(t);
		return NULL;
	}
	return t;
}

/* n free stream ids, highest first; 0 or -1 */
static int free_ids(uint32_t nn, uint32_t *ids, unsigned n)
{
	struct ramon_spfi_table *t = table(nn);
	unsigned open;
	int ret = -1;

	if (!t)
		return -1;
	open = ramon_spfi_count_open(t);
	if (open + n > RAMON_SPFI_MAX_OPEN)
		tc_fail("spfi%u: %u streams are open, %u more would pass the NN limit of %u (spfidel in ramon_cli)",
			nn, open, n, RAMON_SPFI_MAX_OPEN);
	else if (ramon_spfi_find_free(t, ids, n) != n)
		tc_fail("spfi%u: fewer than %u free stream ids", nn, n);
	else
		ret = 0;
	free(t);
	return ret;
}

/* CLOSE + DELETE, then the table must not list the stream */
static void cleanup(struct ramon_spfi_stream *s)
{
	struct ramon_spfi_table *t;
	struct ramon_status st;

	if (!s->open && !s->pages_written)
		return;
	if (t_check(ramon_spfi_stream_close(T.c, s, 1, &st), "close + delete", &st))
		return;
	t = table(s->nn);
	if (t && (t->rec[s->sid].status & RAMON_SPFI_ST_EXIST))
		tc_fail("spfi%u stream %u still exists after DELETE", s->nn, s->sid);
	free(t);
}

static void test_spfi_link(void)
{
	struct ramon_spfi_table *t;
	struct ramon_status st;
	uint32_t nn, raw, sid;
	int up, any = 0;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(T.spfi_present & (1u << nn)))
			continue;
		up = ramon_spfi_link_up(T.c, nn, &raw, &st);
		if (up < 0) {
			tc_fail_st("link status", &st);
			continue;
		}
		tc_info("spfi%u: 0xCC = 0x%x, link %s", nn, raw, up ? "up" : "down");
		if (!up)
			continue;
		any = 1;
		t = table(nn);
		if (!t)
			continue;
		tc_info("spfi%u: media %u, allocated %u, used %u; %u stream(s) exist, %u open", nn,
			t->hdr.total_media_size, t->hdr.total_allocated_size, t->hdr.total_used_size,
			ramon_spfi_count_exist(t), ramon_spfi_count_open(t));
		for (sid = 0; sid < RAMON_SPFI_STREAMS; sid++)
			if (t->rec[sid].status & RAMON_SPFI_ST_EXIST)
				tc_info("  stream %u: status 0x%x, latest_write_offs %d", sid,
					t->rec[sid].status, (int)t->rec[sid].latest_write_offs);
		if (ramon_spfi_count_open(t) > RAMON_SPFI_MAX_OPEN)
			tc_fail("spfi%u: %u streams open", nn, ramon_spfi_count_open(t));
		free(t);
	}
	if (!any)
		tc_fail("no SPFI link is up; bring it up with --prep (or ramon_cli spfiprep)");
}

static void test_spfi_stream(void)
{
	const size_t bytes = (size_t)STREAM_PAGES * PAGE;
	uint32_t nn = (uint32_t)T.spfi_nn, sid;
	struct ramon_spfi_stream s;
	struct ramon_status st;
	ramon_buf src, dst;
	long bad;

	memset(&s, 0, sizeof(s));
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	if (free_ids(nn, &sid, 1) ||
	    t_check(ramon_buf_alloc(T.c, bytes, 0, &src, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(T.c, bytes, 0, &dst, &st), "alloc", &st) ||
	    t_check(ramon_spfi_stream_open(T.c, nn, sid, STREAM_PAGES, &s, &st), "open", &st))
		goto out;
	tc_info("spfi%u stream %u: first page %u", nn, sid, s.first_page);
	tc_fill(src.ptr, bytes, 0x5F5F0000u | sid);
	if (t_check(ramon_spfi_stream_append(T.c, &s, &src, 0, STREAM_PAGES, 0, &st), "write", &st) ||
	    t_check(ramon_spfi_stream_flush(T.c, &s, &st), "flush", &st) ||
	    t_check(ramon_spfi_read_seq(T.c, nn, sid, s.first_page, STREAM_PAGES, &dst, 0, 0, &st),
		    "read", &st))
		goto out;
	bad = tc_verify(dst.ptr, bytes, 0x5F5F0000u | sid);
	if (bad >= 0)
		tc_fail("read back differs at byte %ld (page %ld)", bad, bad / PAGE);
	else if (ramon_crc32(src.ptr, bytes) != ramon_crc32(dst.ptr, bytes))
		tc_fail("CRC differs");
	else
		tc_info("%u pages written, flushed, read back and verified (CRC32 0x%08x)", STREAM_PAGES,
			ramon_crc32(dst.ptr, bytes));
out:
	cleanup(&s);
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
}

/* one write/read throughput measurement of `pages` per call, `runs` times */
static void perf_size(uint32_t nn, uint32_t pages, uint32_t runs)
{
	const size_t bytes = (size_t)pages * PAGE;
	uint64_t t0, wns = 0, rns = 0;
	struct ramon_spfi_stream s;
	struct ramon_status st;
	ramon_buf src, dst;
	uint32_t sid, r, bad = 0;
	char name[40];

	memset(&s, 0, sizeof(s));
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	if (free_ids(nn, &sid, 1) ||
	    t_check(ramon_buf_alloc(T.c, bytes, 0, &src, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(T.c, bytes, 0, &dst, &st), "alloc", &st) ||
	    t_check(ramon_spfi_stream_open(T.c, nn, sid, pages * runs, &s, &st), "open", &st))
		goto out;
	for (r = 0; r < runs; r++) {
		tc_fill(src.ptr, bytes, r);		/* outside the timed region */
		t0 = tc_now_ns();
		if (t_check(ramon_spfi_stream_append(T.c, &s, &src, 0, pages, 0, &st), "write", &st))
			goto out;
		wns += tc_now_ns() - t0;
	}
	if (t_check(ramon_spfi_stream_flush(T.c, &s, &st), "flush", &st))
		goto out;
	for (r = 0; r < runs; r++) {
		memset(dst.ptr, 0, bytes);		/* a failed read cannot pass on stale data */
		t0 = tc_now_ns();
		if (t_check(ramon_spfi_read_seq(T.c, nn, sid, s.first_page + r * pages, pages, &dst, 0, 0,
						&st), "read", &st))
			goto out;
		rns += tc_now_ns() - t0;
		if (tc_verify(dst.ptr, bytes, r) >= 0)
			bad++;
	}
	if (bad)
		tc_fail("%u pages x %u runs: %u read(s) returned different data", pages, runs, bad);
	tc_info("%4u pages x %u: write %7.1f MiB/s, read %7.1f MiB/s", pages, runs,
		tc_mibps((uint64_t)bytes * runs, wns), tc_mibps((uint64_t)bytes * runs, rns));
	snprintf(name, sizeof(name), "write_%up_MiBps", pages);
	t_metric(name, tc_mibps((uint64_t)bytes * runs, wns));
	snprintf(name, sizeof(name), "read_%up_MiBps", pages);
	t_metric(name, tc_mibps((uint64_t)bytes * runs, rns));
out:
	cleanup(&s);
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
}

static void test_spfi_perf(void)
{
	uint32_t sizes[3] = { 1, 8, T.o.spfi_pages }, i;

	for (i = 0; i < 3; i++)
		if (!i || sizes[i] > sizes[i - 1])
			perf_size((uint32_t)T.spfi_nn, sizes[i], T.o.runs);
}

struct writer {
	struct ramon_spfi_stream *s;
	ramon_buf *src;
	int ok;
};

static void *writer_thread(void *arg)
{
	struct writer *w = arg;
	struct ramon_status st;
	uint32_t r;

	for (r = 0; r < ROUNDS; r++) {
		tc_fill(w->src->ptr, (size_t)STREAM_PAGES * PAGE, 0x5000 + r);
		if (t_check(ramon_spfi_stream_append(T.c, w->s, w->src, 0, STREAM_PAGES, 0, &st),
			    "concurrent write", &st))
			return NULL;
	}
	w->ok = 1;
	return NULL;
}

/* stream A written while stream B is read, as the old spfirxtx and the smoke test */
static void test_spfi_rxtx(void)
{
	const size_t bytes = (size_t)STREAM_PAGES * PAGE;
	uint32_t nn = (uint32_t)T.spfi_nn, ids[2], r;
	struct ramon_spfi_stream s1, s2;
	struct ramon_status st;
	ramon_buf src, dst, src2, dst2;
	struct writer w;
	pthread_t th;

	memset(&s1, 0, sizeof(s1));
	memset(&s2, 0, sizeof(s2));
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	memset(&src2, 0, sizeof(src2));
	memset(&dst2, 0, sizeof(dst2));
	if (free_ids(nn, ids, 2) ||
	    t_check(ramon_buf_alloc(T.c, bytes, 0, &src, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(T.c, bytes, 0, &dst, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(T.c, bytes, 0, &src2, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(T.c, ROUNDS * bytes, 0, &dst2, &st), "alloc", &st) ||
	    t_check(ramon_spfi_stream_open(T.c, nn, ids[0], STREAM_PAGES, &s1, &st), "open", &st))
		goto out;
	tc_fill(src.ptr, bytes, 0x4000 + nn);
	if (t_check(ramon_spfi_stream_append(T.c, &s1, &src, 0, STREAM_PAGES, 0, &st), "write", &st) ||
	    t_check(ramon_spfi_stream_flush(T.c, &s1, &st), "flush", &st) ||
	    t_check(ramon_spfi_stream_open(T.c, nn, ids[1], ROUNDS * STREAM_PAGES, &s2, &st), "open",
		    &st))
		goto out;
	w.s = &s2;
	w.src = &src2;
	w.ok = 0;
	if (pthread_create(&th, NULL, writer_thread, &w)) {
		tc_fail("pthread_create");
		goto out;
	}
	for (r = 0; r < ROUNDS; r++) {
		memset(dst.ptr, 0, bytes);
		if (!t_check(ramon_spfi_read_seq(T.c, nn, s1.sid, s1.first_page, STREAM_PAGES, &dst, 0, 0,
						 &st), "concurrent read", &st) &&
		    tc_verify(dst.ptr, bytes, 0x4000 + nn) >= 0)
			tc_fail("stream %u read %u during the writes differs", s1.sid, r);
	}
	pthread_join(th, NULL);
	if (!w.ok || t_check(ramon_spfi_stream_flush(T.c, &s2, &st), "flush", &st) ||
	    t_check(ramon_spfi_read_seq(T.c, nn, s2.sid, s2.first_page, ROUNDS * STREAM_PAGES, &dst2, 0,
					0, &st), "read", &st))
		goto out;
	for (r = 0; r < ROUNDS; r++)
		if (tc_verify((uint8_t *)dst2.ptr + r * bytes, bytes, 0x5000 + r) >= 0)
			tc_fail("stream %u round %u differs", s2.sid, r);
	tc_info("%u writes to stream %u while stream %u was read %u times", ROUNDS, s2.sid, s1.sid,
		ROUNDS);
out:
	cleanup(&s2);
	cleanup(&s1);
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
	ramon_buf_free(&src2, NULL);
	ramon_buf_free(&dst2, NULL);
}

static void print_alert(void *user, const struct ramon_spfi_alert *a)
{
	(void)user;
	tc_info("spfi%u alert: code 0x%x sub 0x%x param1 0x%x param2 0x%x status 0x%x", a->nn,
		a->code, a->sub_code, a->param1, a->param2, a->rx_status);
}

static void test_spfi_alerts(void)
{
	uint32_t nn = (uint32_t)T.spfi_nn, n;
	struct ramon_spfi_alert a;
	struct ramon_status st;
	uint64_t t0;
	double ms;
	int ret;

	if (!t_check(ramon_spfi_drain_alerts(T.c, nn, 200, print_alert, NULL, &n, &st), "drain", &st))
		tc_info("%u pending alert(s)", n);
	ret = ramon_spfi_wait_alert(T.c, nn, 200, &a, &st);
	if (ret != -ETIMEDOUT || st.code != RAMON_E_SPFI_ALERT_TIMEOUT)
		tc_fail("idle alert wait: ret %d code %s", ret, ramon_err_str(st.code));
	if (t_check(ramon_spfi_alert_start(T.c, nn, print_alert, NULL, &st), "alert thread", &st))
		return;
	tc_sleep_ms(100);
	t0 = tc_now_ns();
	ramon_spfi_alert_stop(T.c, nn);
	ms = tc_ms_since(t0);
	tc_info("alert thread stopped in %.0f ms", ms);
	if (ms > 1500)
		tc_fail("stopping the alert thread took %.0f ms", ms);
}

/* requests the library or the driver must refuse before any NN traffic */
static void test_spfi_rules(void)
{
	uint32_t nn = (uint32_t)T.spfi_nn, pages[2] = { 0, 1 }, sid = RAMON_SPFI_STREAMS;
	struct ramon_spfi_stream s;
	struct ramon_spfi_table *t;
	struct ramon_sg_item it;
	struct ramon_status st;
	ramon_buf one;
	int ret, created = 0;

	ret = ramon_spfi_open(T.c, nn, RAMON_SPFI_STREAMS_USABLE, 0, NULL, &st);
	if (ret != -EINVAL || st.code != RAMON_EL_SPFI_RULE)
		tc_fail("open of reserved stream %u: ret %d code %s", RAMON_SPFI_STREAMS_USABLE, ret,
			ramon_err_str(st.code));

	/* an existing stream cannot be opened again: use one, or make one */
	t = table(nn);
	if (!t)
		return;
	for (sid = 0; sid < RAMON_SPFI_STREAMS_USABLE; sid++)
		if (t->rec[sid].status & RAMON_SPFI_ST_EXIST)
			break;
	if (sid == RAMON_SPFI_STREAMS_USABLE && ramon_spfi_find_free(t, &sid, 1) == 1) {
		if (!t_check(ramon_spfi_open(T.c, nn, sid, 1, NULL, &st), "open", &st) &&
		    !t_check(ramon_spfi_close(T.c, nn, sid, NULL, &st), "close", &st))
			created = 1;
		else
			sid = RAMON_SPFI_STREAMS;
	}
	free(t);
	if (sid < RAMON_SPFI_STREAMS_USABLE) {
		ret = ramon_spfi_stream_open(T.c, nn, sid, 0, &s, &st);
		if (ret != -EINVAL || st.code != RAMON_EL_SPFI_RULE)
			tc_fail("reopen of existing stream %u: ret %d code %s", sid, ret,
				ramon_err_str(st.code));
		if (created)
			t_check(ramon_spfi_delete(T.c, nn, sid, NULL, &st), "delete", &st);
	}

	ret = ramon_spfi_write(T.c, nn, 0, 0, NULL, 0, 1, 0, NULL, &st);
	if (ret != -EINVAL || st.code != RAMON_EL_INVAL)
		tc_fail("write with 0 items: ret %d code %s", ret, ramon_err_str(st.code));
	if (t_check(ramon_buf_alloc(T.c, PAGE, 0, &one, &st), "alloc", &st))
		return;
	memset(&it, 0, sizeof(it));
	it.handle = one.handle;
	it.len = PAGE;
	ret = ramon_spfi_write(T.c, nn, 0, 0, &it, 1, 2, 0, NULL, &st);
	if (ret != -EINVAL || st.code != RAMON_EL_INVAL)
		tc_fail("write of 1 page declared as 2: ret %d code %s", ret, ramon_err_str(st.code));
	ret = ramon_spfi_read(T.c, nn, 0, pages, 2, &one, 0, 0, NULL, &st);
	if (ret != -ERANGE || st.code != RAMON_E_SPFI_DST_TOO_SMALL)
		tc_fail("read of 2 pages into 1: ret %d code %s", ret, ramon_err_str(st.code));
	ramon_buf_free(&one, NULL);
}

/* 5000 items (the pool size) all pointing at one page, twice back to back */
static void test_spfi_maxlist(void)
{
	uint32_t nn = (uint32_t)T.spfi_nn, sid, i, k;
	struct ramon_sg_item *items;
	struct ramon_spfi_stream s;
	struct ramon_status st;
	ramon_buf src, dst;
	uint64_t t0;

	memset(&s, 0, sizeof(s));
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	items = calloc(MAXLIST_ITEMS, sizeof(*items));
	if (!items || free_ids(nn, &sid, 1) ||
	    t_check(ramon_buf_alloc(T.c, PAGE, 0, &src, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(T.c, (uint64_t)MAXLIST_CHUNK * PAGE, 0, &dst, &st), "alloc", &st) ||
	    t_check(ramon_spfi_stream_open(T.c, nn, sid, 2 * MAXLIST_ITEMS, &s, &st), "open", &st))
		goto out;
	tc_fill(src.ptr, PAGE, 0x3A3A0000u);
	for (i = 0; i < MAXLIST_ITEMS; i++) {
		items[i].handle = src.handle;
		items[i].len = PAGE;
	}
	t0 = tc_now_ns();
	for (k = 0; k < 2; k++) {
		if (t_check(ramon_spfi_write(T.c, nn, sid, s.next_page, items, MAXLIST_ITEMS,
					     MAXLIST_ITEMS, 0, NULL, &st), "5000-item write", &st))
			goto out;
		s.next_page += MAXLIST_ITEMS;
		s.pages_written += MAXLIST_ITEMS;
	}
	tc_info("2 x %u items written in %.0f ms (%llu prep retries so far)", MAXLIST_ITEMS,
		tc_ms_since(t0), (unsigned long long)ramon_axi_retries(T.c));
	if (t_check(ramon_spfi_stream_flush(T.c, &s, &st), "flush", &st))
		goto out;
	for (i = 0; i < 2 * MAXLIST_ITEMS; i += k) {
		k = 2 * MAXLIST_ITEMS - i < MAXLIST_CHUNK ? 2 * MAXLIST_ITEMS - i : MAXLIST_CHUNK;
		memset(dst.ptr, 0, (size_t)k * PAGE);
		if (t_check(ramon_spfi_read_seq(T.c, nn, sid, s.first_page + i, k, &dst, 0, 0, &st),
			    "read", &st))
			goto out;
		for (uint32_t p = 0; p < k; p++)
			if (memcmp((uint8_t *)dst.ptr + (size_t)p * PAGE, src.ptr, PAGE)) {
				tc_fail("page %u differs", i + p);
				goto out;
			}
	}
	tc_info("%u pages read back and verified", 2 * MAXLIST_ITEMS);
out:
	cleanup(&s);
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
	free(items);
}

const struct test test_spfi[] = {
	{ "spfi-link", test_spfi_link, RUN_DEFAULT, NEEDS_SPFI, "link state (0xCC), stream table" },
	{ "spfi-stream", test_spfi_stream, RUN_DEFAULT, NEEDS_SPFI_LINK,
	  "open, write, flush, read back, verify + CRC, delete" },
	{ "spfi-perf", test_spfi_perf, RUN_DEFAULT, NEEDS_SPFI_LINK,
	  "write/read MiB/s for 1, 8 and --spfi-pages pages, all verified" },
	{ "spfi-rxtx", test_spfi_rxtx, RUN_DEFAULT, NEEDS_SPFI_LINK,
	  "one stream written while another is read" },
	{ "spfi-alerts", test_spfi_alerts, RUN_DEFAULT, NEEDS_SPFI_LINK,
	  "pending alerts, idle timeout, alert thread stop" },
	{ "spfi-rules", test_spfi_rules, RUN_DEFAULT, NEEDS_SPFI_LINK,
	  "reserved ids, reopen, bad sizes refused before NN traffic" },
	{ "spfi-maxlist", test_spfi_maxlist, RUN_LONG, NEEDS_SPFI_LINK,
	  "2 x 5000-item writes back to back, read back" },
	{ NULL, NULL, 0, 0, NULL },
};

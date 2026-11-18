// SPDX-License-Identifier: MIT
/* ramon_test: SPFI tests (stream write-read, performance, concurrency, alerts, rules) */
#include "test.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPFI_PAGE_BYTES		RAMON_SPFI_PAGE
#define STREAM_PAGES		8
#define ROUNDS			3
#define MAXLIST_ITEMS		RAMON_SPFI_WRITE_MAX_PAGES
#define MAXLIST_CHUNK_PAGES	64

static struct ramon_spfi_table *read_table(uint32_t nn)
{
	struct ramon_spfi_table *t = malloc(sizeof(*t));
	struct ramon_status st;

	if (!t) {
		tc_fail("out of memory");
		return NULL;
	}
	if (t_check(ramon_spfi_get_table(t_env.c, nn, t, &st), "stream table", &st)) {
		free(t);
		return NULL;
	}
	return t;
}

/* n free stream ids, highest first; 0 or -1 */
static int find_free_ids(uint32_t nn, uint32_t *ids, unsigned n)
{
	struct ramon_spfi_table *t = read_table(nn);
	unsigned n_open;
	int ret = -1;

	if (!t)
		return -1;
	n_open = ramon_spfi_count_open(t);
	if (n_open + n > RAMON_SPFI_MAX_OPEN)
		tc_fail("spfi%u: %u streams are open, %u more would pass the NN limit of %u (spfidel in ramon_cli)",
			nn, n_open, n, RAMON_SPFI_MAX_OPEN);
	else if (ramon_spfi_find_free(t, ids, n) != n)
		tc_fail("spfi%u: fewer than %u free stream ids", nn, n);
	else
		ret = 0;
	free(t);
	return ret;
}

/* CLOSE + DELETE, then the table must not list the stream */
static void delete_stream(struct ramon_spfi_stream *s)
{
	struct ramon_spfi_table *t;
	struct ramon_status st;

	if (!s->open && !s->pages_written)
		return;
	if (t_check(ramon_spfi_stream_close(t_env.c, s, 1, &st), "close + delete", &st))
		return;
	t = read_table(s->nn);
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
		if (!(t_env.spfi_present_mask & (1u << nn)))
			continue;
		up = ramon_spfi_link_up(t_env.c, nn, &raw, &st);
		if (up < 0) {
			tc_fail_st("link status", &st);
			continue;
		}
		tc_info("spfi%u: 0xCC = 0x%x, link %s", nn, raw, up ? "up" : "down");
		if (!up)
			continue;
		any = 1;
		t = read_table(nn);
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
	const size_t bytes = (size_t)STREAM_PAGES * SPFI_PAGE_BYTES;
	uint32_t nn = (uint32_t)t_env.spfi_nn, sid;
	struct ramon_spfi_stream s;
	struct ramon_status st;
	ramon_buf src, dst;
	long bad_off;

	memset(&s, 0, sizeof(s));
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	if (find_free_ids(nn, &sid, 1) ||
	    t_check(ramon_buf_alloc(t_env.c, bytes, 0, &src, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(t_env.c, bytes, 0, &dst, &st), "alloc", &st) ||
	    t_check(ramon_spfi_stream_open(t_env.c, nn, sid, STREAM_PAGES, &s, &st), "open", &st))
		goto out;
	tc_info("spfi%u stream %u: first page %u", nn, sid, s.first_page);
	tc_fill(src.ptr, bytes, 0x5F5F0000u | sid);
	if (t_check(ramon_spfi_stream_append(t_env.c, &s, &src, 0, STREAM_PAGES, 0, &st), "write",
		    &st) ||
	    t_check(ramon_spfi_stream_flush(t_env.c, &s, &st), "flush", &st) ||
	    t_check(ramon_spfi_read_seq(t_env.c, nn, sid, s.first_page, STREAM_PAGES, &dst, 0, 0,
					&st), "read", &st))
		goto out;
	bad_off = tc_find_mismatch(dst.ptr, bytes, 0x5F5F0000u | sid);
	if (bad_off >= 0)
		tc_fail("read back differs at byte %ld (page %ld)", bad_off,
			bad_off / SPFI_PAGE_BYTES);
	else if (ramon_crc32(src.ptr, bytes) != ramon_crc32(dst.ptr, bytes))
		tc_fail("CRC differs");
	else
		tc_info("%u pages written, flushed, read back and verified (CRC32 0x%08x)", STREAM_PAGES,
			ramon_crc32(dst.ptr, bytes));
out:
	delete_stream(&s);
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
}

/* one write/read throughput measurement of `pages` per call, `runs` times */
static void run_perf_size(uint32_t nn, uint32_t pages, uint32_t runs)
{
	const size_t bytes = (size_t)pages * SPFI_PAGE_BYTES;
	uint64_t t0, write_ns = 0, read_ns = 0;
	struct ramon_spfi_stream s;
	struct ramon_status st;
	ramon_buf src, dst;
	uint32_t sid, r, bad = 0;
	char name[40];

	memset(&s, 0, sizeof(s));
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	if (find_free_ids(nn, &sid, 1) ||
	    t_check(ramon_buf_alloc(t_env.c, bytes, 0, &src, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(t_env.c, bytes, 0, &dst, &st), "alloc", &st) ||
	    t_check(ramon_spfi_stream_open(t_env.c, nn, sid, pages * runs, &s, &st), "open", &st))
		goto out;
	for (r = 0; r < runs; r++) {
		tc_fill(src.ptr, bytes, r);		/* outside the timed region */
		t0 = tc_now_ns();
		if (t_check(ramon_spfi_stream_append(t_env.c, &s, &src, 0, pages, 0, &st), "write",
			    &st))
			goto out;
		write_ns += tc_now_ns() - t0;
	}
	if (t_check(ramon_spfi_stream_flush(t_env.c, &s, &st), "flush", &st))
		goto out;
	for (r = 0; r < runs; r++) {
		memset(dst.ptr, 0, bytes);		/* a failed read cannot pass on stale data */
		t0 = tc_now_ns();
		if (t_check(ramon_spfi_read_seq(t_env.c, nn, sid, s.first_page + r * pages, pages,
						&dst, 0, 0, &st), "read", &st))
			goto out;
		read_ns += tc_now_ns() - t0;
		if (tc_find_mismatch(dst.ptr, bytes, r) >= 0)
			bad++;
	}
	if (bad)
		tc_fail("%u pages x %u runs: %u read(s) returned different data", pages, runs, bad);
	tc_info("%4u pages x %u: write %7.1f MiB/s, read %7.1f MiB/s", pages, runs,
		tc_mibps((uint64_t)bytes * runs, write_ns),
		tc_mibps((uint64_t)bytes * runs, read_ns));
	snprintf(name, sizeof(name), "write_%up_MiBps", pages);
	t_add_metric(name, tc_mibps((uint64_t)bytes * runs, write_ns));
	snprintf(name, sizeof(name), "read_%up_MiBps", pages);
	t_add_metric(name, tc_mibps((uint64_t)bytes * runs, read_ns));
out:
	delete_stream(&s);
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
}

static void test_spfi_perf(void)
{
	uint32_t pages[3] = { 1, 8, t_env.opts.spfi_perf_max_pages }, i;

	for (i = 0; i < 3; i++)
		if (!i || pages[i] > pages[i - 1])
			run_perf_size((uint32_t)t_env.spfi_nn, pages[i], t_env.opts.runs);
}

struct writer {
	struct ramon_spfi_stream *stream;
	ramon_buf *src;
	int ok;
};

static void *writer_thread(void *arg)
{
	struct writer *w = arg;
	struct ramon_status st;
	uint32_t r;

	for (r = 0; r < ROUNDS; r++) {
		tc_fill(w->src->ptr, (size_t)STREAM_PAGES * SPFI_PAGE_BYTES, 0x5000 + r);
		if (t_check(ramon_spfi_stream_append(t_env.c, w->stream, w->src, 0, STREAM_PAGES, 0,
						     &st), "concurrent write", &st))
			return NULL;
	}
	w->ok = 1;
	return NULL;
}

/* write_stream is written while read_stream is read, as the old spfirxtx and the smoke test */
static void test_spfi_rxtx(void)
{
	const size_t bytes = (size_t)STREAM_PAGES * SPFI_PAGE_BYTES;
	uint32_t nn = (uint32_t)t_env.spfi_nn, ids[2], r;
	struct ramon_spfi_stream read_stream, write_stream;
	struct ramon_status st;
	ramon_buf read_src, read_dst, write_src, write_dst;
	struct writer w;
	pthread_t th;

	memset(&read_stream, 0, sizeof(read_stream));
	memset(&write_stream, 0, sizeof(write_stream));
	memset(&read_src, 0, sizeof(read_src));
	memset(&read_dst, 0, sizeof(read_dst));
	memset(&write_src, 0, sizeof(write_src));
	memset(&write_dst, 0, sizeof(write_dst));
	if (find_free_ids(nn, ids, 2) ||
	    t_check(ramon_buf_alloc(t_env.c, bytes, 0, &read_src, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(t_env.c, bytes, 0, &read_dst, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(t_env.c, bytes, 0, &write_src, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(t_env.c, ROUNDS * bytes, 0, &write_dst, &st), "alloc", &st) ||
	    t_check(ramon_spfi_stream_open(t_env.c, nn, ids[0], STREAM_PAGES, &read_stream, &st),
		    "open", &st))
		goto out;
	tc_fill(read_src.ptr, bytes, 0x4000 + nn);
	if (t_check(ramon_spfi_stream_append(t_env.c, &read_stream, &read_src, 0,
					     STREAM_PAGES, 0, &st), "write", &st) ||
	    t_check(ramon_spfi_stream_flush(t_env.c, &read_stream, &st), "flush", &st) ||
	    t_check(ramon_spfi_stream_open(t_env.c, nn, ids[1], ROUNDS * STREAM_PAGES,
					   &write_stream, &st), "open", &st))
		goto out;
	w.stream = &write_stream;
	w.src = &write_src;
	w.ok = 0;
	if (pthread_create(&th, NULL, writer_thread, &w)) {
		tc_fail("pthread_create");
		goto out;
	}
	for (r = 0; r < ROUNDS; r++) {
		memset(read_dst.ptr, 0, bytes);
		if (!t_check(ramon_spfi_read_seq(t_env.c, nn, read_stream.sid,
						 read_stream.first_page, STREAM_PAGES,
						 &read_dst, 0, 0, &st), "concurrent read", &st) &&
		    tc_find_mismatch(read_dst.ptr, bytes, 0x4000 + nn) >= 0)
			tc_fail("stream %u read %u during the writes differs", read_stream.sid, r);
	}
	pthread_join(th, NULL);
	if (!w.ok || t_check(ramon_spfi_stream_flush(t_env.c, &write_stream, &st), "flush", &st) ||
	    t_check(ramon_spfi_read_seq(t_env.c, nn, write_stream.sid, write_stream.first_page,
					ROUNDS * STREAM_PAGES, &write_dst, 0, 0, &st),
		    "read", &st))
		goto out;
	for (r = 0; r < ROUNDS; r++)
		if (tc_find_mismatch((uint8_t *)write_dst.ptr + r * bytes, bytes, 0x5000 + r) >= 0)
			tc_fail("stream %u round %u differs", write_stream.sid, r);
	tc_info("%u writes to stream %u while stream %u was read %u times", ROUNDS,
		write_stream.sid, read_stream.sid, ROUNDS);
out:
	delete_stream(&write_stream);
	delete_stream(&read_stream);
	ramon_buf_free(&read_src, NULL);
	ramon_buf_free(&read_dst, NULL);
	ramon_buf_free(&write_src, NULL);
	ramon_buf_free(&write_dst, NULL);
}

static void print_alert(void *user, const struct ramon_spfi_alert *a)
{
	(void)user;
	tc_info("spfi%u alert: code 0x%x sub 0x%x param1 0x%x param2 0x%x status 0x%x", a->nn,
		a->code, a->sub_code, a->param1, a->param2, a->rx_status);
}

static void test_spfi_alerts(void)
{
	uint32_t nn = (uint32_t)t_env.spfi_nn, n;
	struct ramon_spfi_alert a;
	struct ramon_status st;
	uint64_t t0;
	double ms;
	int ret;

	if (!t_check(ramon_spfi_drain_alerts(t_env.c, nn, 200, print_alert, NULL, &n, &st), "drain",
		     &st))
		tc_info("%u pending alert(s)", n);
	ret = ramon_spfi_wait_alert(t_env.c, nn, 200, &a, &st);
	if (ret != -ETIMEDOUT || st.code != RAMON_E_SPFI_ALERT_TIMEOUT)
		tc_fail("idle alert wait: ret %d code %s", ret, ramon_err_str(st.code));
	if (t_check(ramon_spfi_alert_start(t_env.c, nn, print_alert, NULL, &st), "alert thread",
		    &st))
		return;
	tc_sleep_ms(100);
	t0 = tc_now_ns();
	ramon_spfi_alert_stop(t_env.c, nn);
	ms = tc_ms_since(t0);
	tc_info("alert thread stopped in %.0f ms", ms);
	if (ms > 1500)
		tc_fail("stopping the alert thread took %.0f ms", ms);
}

/* requests the library or the driver must refuse before any NN traffic */
static void test_spfi_rules(void)
{
	uint32_t nn = (uint32_t)t_env.spfi_nn, pages[2] = { 0, 1 }, sid = RAMON_SPFI_STREAMS;
	struct ramon_spfi_stream s;
	struct ramon_spfi_table *t;
	struct ramon_sg_item it;
	struct ramon_status st;
	ramon_buf one;
	int ret, created = 0;

	ret = ramon_spfi_open(t_env.c, nn, RAMON_SPFI_STREAMS_USABLE, 0, NULL, &st);
	if (ret != -EINVAL || st.code != RAMON_EL_SPFI_RULE)
		tc_fail("open of reserved stream %u: ret %d code %s", RAMON_SPFI_STREAMS_USABLE, ret,
			ramon_err_str(st.code));

	/* an existing stream cannot be opened again: use one, or make one */
	t = read_table(nn);
	if (!t)
		return;
	for (sid = 0; sid < RAMON_SPFI_STREAMS_USABLE; sid++)
		if (t->rec[sid].status & RAMON_SPFI_ST_EXIST)
			break;
	if (sid == RAMON_SPFI_STREAMS_USABLE && ramon_spfi_find_free(t, &sid, 1) == 1) {
		if (!t_check(ramon_spfi_open(t_env.c, nn, sid, 1, NULL, &st), "open", &st) &&
		    !t_check(ramon_spfi_close(t_env.c, nn, sid, NULL, &st), "close", &st))
			created = 1;
		else
			sid = RAMON_SPFI_STREAMS;
	}
	free(t);
	if (sid < RAMON_SPFI_STREAMS_USABLE) {
		ret = ramon_spfi_stream_open(t_env.c, nn, sid, 0, &s, &st);
		if (ret != -EINVAL || st.code != RAMON_EL_SPFI_RULE)
			tc_fail("reopen of existing stream %u: ret %d code %s", sid, ret,
				ramon_err_str(st.code));
		if (created)
			t_check(ramon_spfi_delete(t_env.c, nn, sid, NULL, &st), "delete", &st);
	}

	ret = ramon_spfi_write(t_env.c, nn, 0, 0, NULL, 0, 1, 0, NULL, &st);
	if (ret != -EINVAL || st.code != RAMON_EL_INVAL)
		tc_fail("write with 0 items: ret %d code %s", ret, ramon_err_str(st.code));
	if (t_check(ramon_buf_alloc(t_env.c, SPFI_PAGE_BYTES, 0, &one, &st), "alloc", &st))
		return;
	memset(&it, 0, sizeof(it));
	it.handle = one.handle;
	it.len = SPFI_PAGE_BYTES;
	ret = ramon_spfi_write(t_env.c, nn, 0, 0, &it, 1, 2, 0, NULL, &st);
	if (ret != -EINVAL || st.code != RAMON_EL_INVAL)
		tc_fail("write of 1 page declared as 2: ret %d code %s", ret, ramon_err_str(st.code));
	ret = ramon_spfi_read(t_env.c, nn, 0, pages, 2, &one, 0, 0, NULL, &st);
	if (ret != -ERANGE || st.code != RAMON_E_SPFI_DST_TOO_SMALL)
		tc_fail("read of 2 pages into 1: ret %d code %s", ret, ramon_err_str(st.code));
	ramon_buf_free(&one, NULL);
}

/* 5000 items (the pool size) all pointing at one page, twice back to back */
static void test_spfi_maxlist(void)
{
	uint32_t nn = (uint32_t)t_env.spfi_nn, sid, i, k;
	struct ramon_sg_item *items;
	struct ramon_spfi_stream s;
	struct ramon_status st;
	ramon_buf src, dst;
	uint64_t t0;

	memset(&s, 0, sizeof(s));
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	items = calloc(MAXLIST_ITEMS, sizeof(*items));
	if (!items || find_free_ids(nn, &sid, 1) ||
	    t_check(ramon_buf_alloc(t_env.c, SPFI_PAGE_BYTES, 0, &src, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(t_env.c, (uint64_t)MAXLIST_CHUNK_PAGES * SPFI_PAGE_BYTES, 0,
				    &dst, &st), "alloc", &st) ||
	    t_check(ramon_spfi_stream_open(t_env.c, nn, sid, 2 * MAXLIST_ITEMS, &s, &st), "open",
		    &st))
		goto out;
	tc_fill(src.ptr, SPFI_PAGE_BYTES, 0x3A3A0000u);
	for (i = 0; i < MAXLIST_ITEMS; i++) {
		items[i].handle = src.handle;
		items[i].len = SPFI_PAGE_BYTES;
	}
	t0 = tc_now_ns();
	for (k = 0; k < 2; k++) {
		if (t_check(ramon_spfi_write(t_env.c, nn, sid, s.next_page, items, MAXLIST_ITEMS,
					     MAXLIST_ITEMS, 0, NULL, &st), "5000-item write", &st))
			goto out;
		s.next_page += MAXLIST_ITEMS;
		s.pages_written += MAXLIST_ITEMS;
	}
	tc_info("2 x %u items written in %.0f ms (%llu prep retries so far)", MAXLIST_ITEMS,
		tc_ms_since(t0), (unsigned long long)ramon_axi_retries(t_env.c));
	if (t_check(ramon_spfi_stream_flush(t_env.c, &s, &st), "flush", &st))
		goto out;
	for (i = 0; i < 2 * MAXLIST_ITEMS; i += k) {
		k = 2 * MAXLIST_ITEMS - i < MAXLIST_CHUNK_PAGES ? 2 * MAXLIST_ITEMS - i :
		    MAXLIST_CHUNK_PAGES;
		memset(dst.ptr, 0, (size_t)k * SPFI_PAGE_BYTES);
		if (t_check(ramon_spfi_read_seq(t_env.c, nn, sid, s.first_page + i, k, &dst, 0, 0,
						&st), "read", &st))
			goto out;
		for (uint32_t p = 0; p < k; p++)
			if (memcmp((uint8_t *)dst.ptr + (size_t)p * SPFI_PAGE_BYTES, src.ptr,
				   SPFI_PAGE_BYTES)) {
				tc_fail("page %u differs", i + p);
				goto out;
			}
	}
	tc_info("%u pages read back and verified", 2 * MAXLIST_ITEMS);
out:
	delete_stream(&s);
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
	free(items);
}

const struct test t_spfi_tests[] = {
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

// SPDX-License-Identifier: MIT
/*
 * ramon_accept: SPFI steps (the SPFI part of the delivery script).
 *
 * spfi-link    link up: low byte of 0xCC is 0x88 (was spfidrvinit, spfireg +204)
 * spfi-prep    SPW link + DPS, INIT 0, FORMAT with --spfi-format, stream table
 *              (was spwdps 0, spfiinit 0, spfifmt, spfiginfo 1); run it first,
 *              the NN answers stream commands only after it
 * spfi-stream  150 pages x 1000, verified (stream #2)
 * spfi-perf    150 x 1000 and 5000 x 100, throughput only (#3, #5)
 * spfi-large   5000 x 100, verified (#4)
 * spfi-odd     257, 4999 and 1 page x 100, verified (#10, #11, #12)
 * spfi-rxtx    one stream written while another is read (#17-#20)
 * spfi-long    5000 x 200, throughput only (#13); only with --spfi-long
 * spfi-soak    the two 20-minute concurrent runs (#21-#24); only with --spfi-soak
 *
 * A page ("offset") is 16 KiB. Every test opens new streams on free ids and
 * deletes them afterwards (the NN keeps at most 8 open). Verified data: each
 * page holds a fixed pattern and a 16-byte stamp (magic, stream, run, page);
 * only the stamps change between runs, so a read of the wrong run or page
 * fails. Reads are compared against a copy in ordinary memory, outside the
 * timed part. --spfi-scale cuts runs and cycles (in a single-cycle concurrent
 * test, its writes and reads) for a quick check.
 */
#include "accept.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE		RAMON_SPFI_PAGE
#define MIB		1048576.0
#define STAMP_MAGIC	0x5EF1A000u
#define MAX_REPORTED	3		/* bad pages printed per test */
#define CYCLE_MS	1000		/* the delivery script's "each second" */
#define FILL_SEED	0x0C0FFEE5u
#define LINK_WAIT_MS	2000		/* SPFI link after the SPW enable and sync */

struct stamp {
	uint32_t magic, sid, run, page;
};

/* ---- setup ---- */

uint32_t a_spfi_setup(void)
{
	struct ramon_spfi_config fc;
	struct ramon_status st;
	uint32_t nn, raw, want;
	int up;

	if (!ramon_info(A.c)->spfi_mask) {
		tc_fail("the driver reports no SPFI NN (GET_INFO spfi_mask 0x0): see `dmesg | grep -i spfi` "
			"(device-tree node, interrupts, the spfi_mask module parameter)");
		return 0;
	}
	if (!A.spfi_inited) {
		/*
		 * The SPFI link comes up only once the SPW core of the NN is enabled
		 * and synced (as the delivery script ran spwinit before spfireg +204).
		 */
		if (!A.spw_inited && !A.o.spw_nn_mask)
			/* SPFI needs only the SPW link of its own NN: no 10 s on an uncabled one */
			A.o.spw_nn_mask = ramon_info(A.c)->spfi_mask & ramon_info(A.c)->spw_mask;
		a_spw_setup();
		ramon_spfi_config_default(&fc);
		fc.chan[0] = A.o.spfi_chan[0];
		fc.chan[1] = A.o.spfi_chan[1];
		if (a_check(ramon_spfi_init(A.c, &fc, &st), "ramon_spfi_init", &st))
			return 0;
		A.spfi_inited = 1;
		for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
			if (!ramon_spfi_present(A.c, nn))
				continue;
			up = !ramon_spfi_wait_link(A.c, nn, LINK_WAIT_MS, &raw, &st);
			if (up)
				A.spfi_up |= 1u << nn;
			else
				tc_info("spfi%u: link down (0xCC = 0x%x, low byte must be 0x%x)", nn, raw,
					RAMON_SPFI_LINK_UP);
		}
	}
	want = A.o.spfi_nn_mask ? A.o.spfi_nn_mask : A.spfi_up;
	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		if ((want & (1u << nn)) && !(A.spfi_up & (1u << nn)))
			tc_fail("spfi%u: link not up (required by --spfi-nn)", nn);
	if (!want)
		tc_fail("no SPFI link up");
	return want & A.spfi_up;
}

static uint32_t scaled(uint32_t n)
{
	uint64_t v = (uint64_t)n * A.o.spfi_scale / 100;

	return v ? (uint32_t)v : 1;
}

/* n free stream ids on NN nn; reports and fails when there are fewer */
int a_spfi_free_ids(uint32_t nn, uint32_t *ids, unsigned n)
{
	struct ramon_spfi_table *t = malloc(sizeof(*t));
	struct ramon_status st;
	int ret = -1;

	if (!t) {
		tc_fail("out of memory");
		return -1;
	}
	if (!a_check(ramon_spfi_get_table(A.c, nn, t, &st), "stream table", &st)) {
		if (ramon_spfi_find_free(t, ids, n) == n &&
		    ramon_spfi_count_open(t) + n <= RAMON_SPFI_MAX_OPEN)
			ret = 0;
		else
			tc_fail("spfi%u: need %u free stream id(s) and room to open them: %u exist, %u open "
				"(at most %u); delete old streams or run spfi-prep with --spfi-format",
				nn, n, ramon_spfi_count_exist(t), ramon_spfi_count_open(t),
				RAMON_SPFI_MAX_OPEN);
	}
	free(t);
	return ret;
}

/* ---- pages: a fixed pattern plus a stamp per page ---- */

void a_pages_free(struct a_pages *p)
{
	ramon_buf_free(&p->b, NULL);
	free(p->ref);
	memset(p, 0, sizeof(*p));
}

int a_pages_alloc(struct a_pages *p, uint32_t n, int with_ref)
{
	struct ramon_status st;
	size_t bytes = (size_t)n * PAGE;

	memset(p, 0, sizeof(*p));
	p->n = n;
	if (a_check(ramon_buf_alloc(A.c, bytes, 0, &p->b, &st), "DMA buffer", &st))
		return -1;
	if (with_ref) {
		p->ref = malloc(bytes);
		if (!p->ref) {
			tc_fail("out of memory for %zu bytes", bytes);
			a_pages_free(p);
			return -1;
		}
		tc_fill(p->ref, bytes, FILL_SEED);
		memcpy(p->b.ptr, p->ref, bytes);
	}
	return 0;
}

void a_stamp_pages(struct a_pages *p, uint32_t n, uint32_t sid, uint32_t run)
{
	struct stamp s = { STAMP_MAGIC, sid, run, 0 };
	uint32_t i;

	for (i = 0; i < n; i++) {
		s.page = i;
		memcpy((uint8_t *)p->b.ptr + (size_t)i * PAGE, &s, sizeof(s));
	}
}

/*
 * Compares n pages of got (a read of run `run` of stream sid) with the
 * pattern of ref; returns the number of bad pages and prints the first few.
 */
uint32_t a_check_pages(const struct a_pages *got, const struct a_pages *ref, uint32_t n,
			    uint32_t sid, uint32_t run, uint32_t *reported)
{
	struct stamp want = { STAMP_MAGIC, sid, run, 0 }, have;
	uint32_t i, bad = 0;

	for (i = 0; i < n; i++) {
		const uint8_t *g = (const uint8_t *)got->b.ptr + (size_t)i * PAGE;
		const uint8_t *r = ref->ref + (size_t)i * PAGE;
		const char *what = NULL;

		want.page = i;
		memcpy(&have, g, sizeof(have));
		if (memcmp(&have, &want, sizeof(want)))
			what = "stamp";
		else if (memcmp(g + sizeof(want), r + sizeof(want), PAGE - sizeof(want)))
			what = "data";
		if (!what)
			continue;
		bad++;
		if ((*reported)++ < MAX_REPORTED)
			tc_info("  run %u page %u: %s differs (stamp read: run %u page %u stream %u, magic 0x%x)",
				run, i, what, have.run, have.page, have.sid, have.magic);
	}
	return bad;
}

/* ---- one stream: write runs x pages, flush, read every run back ---- */

struct stream_result {
	double w_mibps, r_mibps, secs;
	uint32_t bad_pages, bad_runs;
};

static int stream_run(uint32_t nn, uint32_t pages, uint32_t runs, int verify,
		      struct stream_result *res)
{
	struct ramon_spfi_stream s;
	struct ramon_status st;
	struct a_pages src, dst;
	uint64_t t0 = tc_now_ns(), t, wns = 0, rns = 0, bytes = (uint64_t)pages * PAGE;
	uint32_t sid, r, bad, reported = 0, wruns = 0, rruns = 0;
	int ret = -1;

	memset(res, 0, sizeof(*res));
	memset(&s, 0, sizeof(s));
	memset(&dst, 0, sizeof(dst));
	if (a_spfi_free_ids(nn, &sid, 1) || a_pages_alloc(&src, pages, verify))
		return -1;
	if (a_pages_alloc(&dst, pages, 0))
		goto out;
	if (a_check(ramon_spfi_stream_open(A.c, nn, sid, pages * runs, &s, &st), "OPEN", &st))
		goto out;
	for (r = 0; r < runs; r++) {
		if (verify)
			a_stamp_pages(&src, pages, sid, r);
		t = tc_now_ns();
		if (a_check(ramon_spfi_stream_append(A.c, &s, &src.b, 0, pages, 0, &st), "write", &st))
			goto close;
		wns += tc_now_ns() - t;
		wruns++;
	}
	if (a_check(ramon_spfi_stream_flush(A.c, &s, &st), "FLUSH", &st))
		goto close;
	for (r = 0; r < runs; r++) {
		t = tc_now_ns();
		if (a_check(ramon_spfi_read_seq(A.c, nn, sid, s.first_page + r * pages, pages, &dst.b, 0, 0,
						&st), "read", &st))
			goto close;
		rns += tc_now_ns() - t;
		rruns++;
		if (verify) {
			bad = a_check_pages(&dst, &src, pages, sid, r, &reported);
			res->bad_pages += bad;
			res->bad_runs += bad != 0;
		}
	}
	ret = 0;
close:
	if (a_check(ramon_spfi_stream_close(A.c, &s, 1, &st), "CLOSE and DELETE", &st))
		ret = -1;
out:
	res->w_mibps = wns ? bytes * wruns / MIB / (wns / 1e9) : 0;
	res->r_mibps = rns ? bytes * rruns / MIB / (rns / 1e9) : 0;
	res->secs = tc_ms_since(t0) / 1000;
	a_pages_free(&src);
	a_pages_free(&dst);
	return ret;
}

struct stream_job {
	uint32_t pages, runs;
	int verify;
	const char *was;
};

static void run_stream_jobs(const struct stream_job *jobs, unsigned n)
{
	struct stream_result r;
	char sum[160] = "";
	size_t used = 0;
	uint32_t use, nn, runs;
	unsigned i;

	use = a_spfi_setup();
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(use & (1u << nn)))
			continue;
		for (i = 0; i < n; i++) {
			runs = scaled(jobs[i].runs);
			tc_info("spfi%u: %u page(s) x %u run(s) = %.0f MiB, %s (%s)", nn, jobs[i].pages,
				runs, (double)jobs[i].pages * runs * PAGE / MIB,
				jobs[i].verify ? "verified" : "throughput only", jobs[i].was);
			if (stream_run(nn, jobs[i].pages, runs, jobs[i].verify, &r)) {
				if (used < sizeof(sum))
					used += (size_t)snprintf(sum + used, sizeof(sum) - used,
								 "%s%ux%u failed", used ? ", " : "",
								 jobs[i].pages, runs);
				break;
			}
			tc_info("spfi%u:   write %.1f MiB/s, read %.1f MiB/s, %.1f s%s", nn, r.w_mibps,
				r.r_mibps, r.secs, jobs[i].verify ? (r.bad_pages ? "" : ", data verified") : "");
			if (r.bad_pages)
				tc_fail("spfi%u %u x %u: %u bad page(s) in %u of %u run(s)", nn, jobs[i].pages,
					runs, r.bad_pages, r.bad_runs, runs);
			if (used < sizeof(sum))
				used += (size_t)snprintf(sum + used, sizeof(sum) - used,
							 "%s%ux%u W %.0f R %.0f MiB/s", used ? ", " : "",
							 jobs[i].pages, runs, r.w_mibps, r.r_mibps);
		}
	}
	a_summary("%s", sum[0] ? sum : use ? "nothing run" : "no SPFI link up");
}

/* ---- concurrent: one stream written while another is read ---- */

struct rxtx_job {
	uint32_t wp[2], rp[2];		/* pages per write / read, two sizes each */
	uint32_t wn[2], rn[2];		/* how many of each per cycle */
	uint32_t cycles;
	const char *was;
};

struct writer {
	struct ramon_spfi_stream *s;
	ramon_buf *src;
	const struct rxtx_job *j;
	uint64_t ns;
	int failed;
	struct ramon_status st;
};

static void *writer_thread(void *arg)
{
	struct writer *w = arg;
	uint64_t t0 = tc_now_ns();
	uint32_t k, i;

	for (k = 0; k < 2 && !w->failed; k++)
		for (i = 0; i < w->j->wn[k] && w->j->wp[k] && !w->failed; i++)
			w->failed = ramon_spfi_stream_append(A.c, w->s, w->src, 0, w->j->wp[k], 0, &w->st);
	w->ns = tc_now_ns() - t0;
	return NULL;
}

static uint32_t max2(const uint32_t v[2])
{
	return v[0] > v[1] ? v[0] : v[1];
}

struct rxtx_result {
	double w_mibps, r_mibps, min_slack, max_cycle;
	uint32_t cycles_done, late, bad_pages;
};

static int rxtx_run(uint32_t nn, const struct rxtx_job *j, uint32_t cycles, struct rxtx_result *res)
{
	struct ramon_spfi_stream ws, rs;
	struct a_pages wsrc, rdst[2], ref;
	struct ramon_status st;
	struct writer w;
	pthread_t th;
	uint32_t ids[2], rpages = max2(j->rp), c, k, i, reported = 0, step;
	uint64_t wbytes = 0, rbytes = 0, wns = 0, rns = 0, t, c0, cyc, crb, crns;
	int ret = -1, rfail = 0;

	memset(res, 0, sizeof(*res));
	memset(&ws, 0, sizeof(ws));
	memset(&rs, 0, sizeof(rs));
	memset(&wsrc, 0, sizeof(wsrc));
	memset(rdst, 0, sizeof(rdst));
	memset(&ref, 0, sizeof(ref));
	res->min_slack = 1e9;
	if (a_spfi_free_ids(nn, ids, 2) || a_pages_alloc(&ref, rpages, 1) || a_pages_alloc(&wsrc, max2(j->wp), 0))
		goto out;
	for (k = 0; k < 2; k++)
		if (j->rp[k] && a_pages_alloc(&rdst[k], j->rp[k], 0))
			goto out;
	/* the read stream: rpages pages written once and flushed */
	a_stamp_pages(&ref, rpages, ids[1], 0);
	if (a_check(ramon_spfi_stream_open(A.c, nn, ids[1], rpages, &rs, &st), "OPEN read stream", &st) ||
	    a_check(ramon_spfi_stream_append(A.c, &rs, &ref.b, 0, rpages, 0, &st), "fill read stream",
		    &st) ||
	    a_check(ramon_spfi_stream_flush(A.c, &rs, &st), "FLUSH read stream", &st))
		goto close;
	for (k = 0; k < 2; k++)
		wbytes += (uint64_t)j->wn[k] * j->wp[k] * PAGE;
	if (a_check(ramon_spfi_stream_open(A.c, nn, ids[0], (uint32_t)(wbytes / PAGE) * cycles, &ws,
					   &st), "OPEN write stream", &st))
		goto close;
	step = cycles >= 10 ? cycles / 10 : 1;
	for (c = 0; c < cycles; c++) {
		memset(&w, 0, sizeof(w));
		w.s = &ws;
		w.src = &wsrc.b;
		w.j = j;
		c0 = tc_now_ns();
		if (pthread_create(&th, NULL, writer_thread, &w)) {
			tc_fail("pthread_create failed");
			goto close;
		}
		t = tc_now_ns();
		crb = rbytes;
		for (k = 0; k < 2 && !rfail; k++)
			for (i = 0; i < j->rn[k] && j->rp[k] && !rfail; i++) {
				rfail = a_check(ramon_spfi_read_seq(A.c, nn, ids[1], rs.first_page, j->rp[k],
								    &rdst[k].b, 0, 0, &st), "read", &st);
				if (!rfail)
					rbytes += (uint64_t)j->rp[k] * PAGE;
			}
		crns = tc_now_ns() - t;
		rns += crns;
		crb = rbytes - crb;
		pthread_join(th, NULL);
		cyc = tc_now_ns() - c0;
		if (w.failed)
			tc_fail_st("write", &w.st);
		if (w.failed || rfail)
			goto close;
		wns += w.ns;
		res->cycles_done++;
		if (cyc / 1e6 > res->max_cycle)
			res->max_cycle = cyc / 1e6;
		if (CYCLE_MS - cyc / 1e6 < res->min_slack)
			res->min_slack = CYCLE_MS - cyc / 1e6;
		res->late += cyc / 1e6 > CYCLE_MS;
		/* the last read of each size, outside the timed part */
		for (k = 0; k < 2; k++)
			if (j->rp[k] && j->rn[k])
				res->bad_pages += a_check_pages(&rdst[k], &ref, j->rp[k], ids[1], 0, &reported);
		if (cycles <= 10 || (c + 1) % step == 0)
			tc_info("spfi%u:   cycle %u/%u: %.0f ms (slack %.0f ms), write %.1f, read %.1f MiB/s",
				nn, c + 1, cycles, cyc / 1e6, CYCLE_MS - cyc / 1e6,
				w.ns ? (double)wbytes / MIB / (w.ns / 1e9) : 0,
				crns ? (double)crb / MIB / (crns / 1e9) : 0);
	}
	ret = 0;
close:
	if (a_check(ramon_spfi_stream_close(A.c, &ws, 1, &st), "CLOSE and DELETE write stream", &st) ||
	    a_check(ramon_spfi_stream_close(A.c, &rs, 1, &st), "CLOSE and DELETE read stream", &st))
		ret = -1;
out:
	res->w_mibps = wns ? (double)wbytes * res->cycles_done / MIB / (wns / 1e9) : 0;
	res->r_mibps = rns ? (double)rbytes / MIB / (rns / 1e9) : 0;
	a_pages_free(&ref);
	a_pages_free(&wsrc);
	a_pages_free(&rdst[0]);
	a_pages_free(&rdst[1]);
	return ret;
}

static void run_rxtx_jobs(const struct rxtx_job *jobs, unsigned n)
{
	struct rxtx_result r;
	char sum[160] = "";
	size_t used = 0;
	uint32_t use, nn, cycles;
	unsigned i;

	use = a_spfi_setup();
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(use & (1u << nn)))
			continue;
		for (i = 0; i < n; i++) {
			struct rxtx_job js = jobs[i];
			const struct rxtx_job *j = &js;
			unsigned k;

			/* one cycle: scale its writes and reads; several: scale the cycles */
			if (js.cycles == 1)
				for (k = 0; k < 2; k++) {
					js.wn[k] = js.wn[k] ? scaled(js.wn[k]) : 0;
					js.rn[k] = js.rn[k] ? scaled(js.rn[k]) : 0;
				}
			cycles = scaled(js.cycles);
			tc_info("spfi%u: %u cycle(s), each writing %ux%u + %ux%u pages while reading "
				"%ux%u + %ux%u pages (%s)", nn, cycles, j->wn[0], j->wp[0], j->wn[1], j->wp[1],
				j->rn[0], j->rp[0], j->rn[1], j->rp[1], j->was);
			if (rxtx_run(nn, j, cycles, &r)) {
				if (used < sizeof(sum))
					used += (size_t)snprintf(sum + used, sizeof(sum) - used,
								 "%s%s failed", used ? "; " : "", j->was);
				break;
			}
			tc_info("spfi%u:   write %.1f MiB/s, read %.1f MiB/s; longest cycle %.0f ms, "
				"%u of %u cycle(s) over %u ms%s", nn, r.w_mibps, r.r_mibps, r.max_cycle, r.late,
				r.cycles_done, CYCLE_MS, r.bad_pages ? "" : "; reads verified");
			if (r.bad_pages)
				tc_fail("spfi%u: %u bad page(s) read during the concurrent writes", nn,
					r.bad_pages);
			if (used < sizeof(sum))
				used += (size_t)snprintf(sum + used, sizeof(sum) - used,
							 "%sW %.0f R %.0f MiB/s, %u/%u late", used ? "; " : "",
							 r.w_mibps, r.r_mibps, r.late, r.cycles_done);
		}
	}
	a_summary("%s", sum[0] ? sum : use ? "nothing run" : "no SPFI link up");
}

/* ---- steps ---- */

static void step_spfi_link(void)
{
	static const uint32_t ver[] = { RAMON_SPFI_REG_WR_VERSION, RAMON_SPFI_REG_IP_MAJOR,
					RAMON_SPFI_REG_IP_MINOR };
	char win[8], line[96] = "", *p = line;
	uint32_t nn, use, raw, v[3], i;
	int up;

	use = a_spfi_setup();
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!ramon_spfi_present(A.c, nn))
			continue;
		snprintf(win, sizeof(win), "spfi%u", nn);
		for (i = 0; i < ARRAY_SIZE(ver); i++)
			if (ramon_reg_read(A.c, win, ver[i], &v[i], NULL))
				v[i] = 0;
		/* judged from this read, whether or not the SPFI layer came up */
		up = ramon_spfi_link_up(A.c, nn, &raw, NULL) > 0;
		tc_info("spfi%u: wrapper version 0x%x, IP %u.%u, 0xCC = 0x%x (link %s: low byte 0x%02x, "
			"0x%02x when up)%s", nn, v[0], v[1], v[2], raw, up ? "up" : "down", raw & 0xFF,
			RAMON_SPFI_LINK_UP, (use >> nn) & 1 ? ", tested" : "");
		if (!up)
			tc_fail("spfi%u: link down (0xCC low byte 0x%02x, not 0x%02x)", nn, raw & 0xFF,
				RAMON_SPFI_LINK_UP);
		p += snprintf(p, sizeof(line) - (size_t)(p - line), "%sspfi%u 0xCC 0x%x %s",
			      p == line ? "" : ", ", nn, raw, up ? "up" : "down");
	}
	a_summary("%s", line[0] ? line : "the driver has no SPFI NN (spfi_mask 0x0)");
}

static void step_spfi_prep(void)
{
	struct ramon_spfi_prep_opts o = { A.o.spfi_format, 0 };
	struct ramon_spfi_table *t;
	struct ramon_status st;
	uint32_t use, nn, free_n, ids[RAMON_SPFI_STREAMS];
	char sum[160] = "";
	size_t used = 0;

	use = a_spfi_setup();
	if (!use) {
		a_summary("no SPFI link up");
		return;
	}
	a_spw_setup();		/* SPW layer with our node id; DPS goes over SPW */
	t = malloc(sizeof(*t));
	if (!t) {
		tc_fail("out of memory");
		return;
	}
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(use & (1u << nn)))
			continue;
		tc_info("spfi%u: SPW DPS, INIT 0%s, stream table", nn,
			A.o.spfi_format ? ", FORMAT (erases every stream)" : "");
		if (a_check(ramon_spfi_prep(A.c, nn, &o, t, &st), "SPFI bring-up", &st)) {
			if (used < sizeof(sum))
				used += (size_t)snprintf(sum + used, sizeof(sum) - used,
							 "%sspfi%u bring-up failed: %.40s", used ? ", " : "",
							 nn, st.msg);
			continue;
		}
		free_n = ramon_spfi_find_free(t, ids, RAMON_SPFI_STREAMS);
		tc_info("spfi%u: media size %u, allocated %u, used %u; quality %u/%u", nn,
			t->hdr.total_media_size, t->hdr.total_allocated_size, t->hdr.total_used_size,
			t->hdr.dynamic_quality, t->hdr.static_quality);
		tc_info("spfi%u: %u stream(s) exist, %u open, %u usable id(s) free", nn,
			ramon_spfi_count_exist(t), ramon_spfi_count_open(t), free_n);
		if (!A.o.spfi_format)
			tc_info("spfi%u: not formatted (--spfi-format erases every stream first, as spfifmt)",
				nn);
		if (free_n < 2 || ramon_spfi_count_open(t) + 2 > RAMON_SPFI_MAX_OPEN)
			tc_fail("spfi%u: the stream tests need 2 free ids and room to open 2 streams; "
				"delete old streams or use --spfi-format", nn);
		if (used < sizeof(sum))
			used += (size_t)snprintf(sum + used, sizeof(sum) - used,
						 "%sspfi%u%s: %u exist, %u open", used ? ", " : "", nn,
						 A.o.spfi_format ? " formatted" : "",
						 ramon_spfi_count_exist(t), ramon_spfi_count_open(t));
	}
	free(t);
	a_summary("%s", sum[0] ? sum : "nothing run");
}

static void step_spfi_stream(void)
{
	static const struct stream_job jobs[] = { { 150, 1000, 1, "stream #2" } };

	run_stream_jobs(jobs, ARRAY_SIZE(jobs));
}

static void step_spfi_perf(void)
{
	static const struct stream_job jobs[] = {
		{ 150, 1000, 0, "stream #3" },
		{ 5000, 100, 0, "stream #5" },
	};

	run_stream_jobs(jobs, ARRAY_SIZE(jobs));
}

static void step_spfi_large(void)
{
	static const struct stream_job jobs[] = { { 5000, 100, 1, "stream #4" } };

	run_stream_jobs(jobs, ARRAY_SIZE(jobs));
}

static void step_spfi_odd(void)
{
	static const struct stream_job jobs[] = {
		{ 257, 100, 1, "stream #10" },
		{ 4999, 100, 1, "stream #11" },
		{ 1, 100, 1, "stream #12" },
	};

	run_stream_jobs(jobs, ARRAY_SIZE(jobs));
}

static void step_spfi_long(void)
{
	static const struct stream_job jobs[] = { { 5000, 200, 0, "stream #13" } };

	run_stream_jobs(jobs, ARRAY_SIZE(jobs));
}

/* old spfirxtx: wpages1 wpages2 rpages1 rpages2 stw str wnum1 wnum2 rnum1 rnum2 cycles */
static void step_spfi_rxtx(void)
{
	static const struct rxtx_job jobs[] = {
		{ { 5000, 0 }, { 5000, 0 }, { 100, 0 }, { 80, 0 }, 1, "streams #17/#18, read measurement" },
		{ { 5000, 0 }, { 5000, 0 }, { 80, 0 }, { 100, 0 }, 1, "streams #19/#20, write measurement" },
	};

	run_rxtx_jobs(jobs, ARRAY_SIZE(jobs));
}

static void step_spfi_soak(void)
{
	static const struct rxtx_job jobs[] = {
		{ { 100, 1900 }, { 100, 700 }, { 10, 10 }, { 10, 10 }, 750, "streams #21/#22" },
		{ { 10, 190 }, { 10, 70 }, { 10, 10 }, { 10, 10 }, 7000, "streams #23/#24" },
	};

	run_rxtx_jobs(jobs, ARRAY_SIZE(jobs));
}

static int want_long(void)
{
	return A.o.spfi_long;
}

static int want_soak(void)
{
	return A.o.spfi_soak;
}

const struct step accept_spfi[] = {
	{ "spfi-link", step_spfi_link, NEEDS_DEV, "spfidrvinit, spfireg +204",
	  "SPFI link up (0xCC low byte 0x88), IP versions", NULL },
	{ "spfi-prep", step_spfi_prep, NEEDS_DEV, "spwdps 0, spfiinit 0, spfifmt, spfiginfo 1",
	  "SPW DPS, INIT, FORMAT with --spfi-format, stream table", NULL },
	{ "spfi-stream", step_spfi_stream, NEEDS_DEV, "spfitest +2 +150 +1000 1",
	  "150 pages x 1000 written, read back, verified", NULL },
	{ "spfi-perf", step_spfi_perf, NEEDS_DEV, "spfitest +3/+5, verify 0",
	  "150 x 1000 and 5000 x 100: write and read MiB/s", NULL },
	{ "spfi-large", step_spfi_large, NEEDS_DEV, "spfitest +4 +5000 +100 1",
	  "5000 pages x 100, verified", NULL },
	{ "spfi-odd", step_spfi_odd, NEEDS_DEV, "spfitest +10/+11/+12",
	  "257, 4999 and 1 page x 100, verified", NULL },
	{ "spfi-rxtx", step_spfi_rxtx, NEEDS_DEV, "spfirxtx (#17-#20)",
	  "one stream written while another is read, reads verified", NULL },
	{ "spfi-long", step_spfi_long, NEEDS_DEV, "spfitest +13 +5000 +200 0",
	  "5000 x 200 for current measurements (only with --spfi-long)", want_long },
	{ "spfi-soak", step_spfi_soak, NEEDS_DEV, "spfirxtx (#21-#24)",
	  "the two 20-minute concurrent runs (only with --spfi-soak)", want_soak },
	{ NULL, NULL, 0, NULL, NULL, NULL },
};

// SPDX-License-Identifier: MIT
/*
 * ramon_accept: mixed load, SPW and SPFI users at the same time.
 *
 * mixed-write  3 SPW users (a message every 1 s, 1 s, 0.1 s) and 3 SPFI
 *              writers (12 pages every 1 s, 2 pages every 1 s, 4096 pages
 *              every 0.1 s), each to its own stream
 * mixed-rw     the same SPW users, SPFI writers of 12, 2 and 2048 pages, and
 *              an SPFI reader of 2048 pages every 0.1 s
 *
 * Every user is a thread on a fixed schedule from a common start. An
 * operation still running when its next slot comes is a missed deadline; the
 * missed slots are skipped, not caught up. SPW messages go to our own node and
 * the NN routes them back (as spw-nn-loop); the RX thread checks each one. The
 * reader checks every page's stamp and one whole page per read, and the last
 * read completely. A user stops at its first error; the others run on.
 * Passes with no error, no lost or bad message or page, and no missed
 * deadline (--mixed-late-ok: deadlines are only reported). Every
 * --mixed-report-minutes (default 1) a line per user shows the interval.
 */
#include "accept.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE		RAMON_SPFI_PAGE
#define MIB		1048576.0
#define MAX_USERS	8
#define SPW_TAG		0x6D000000u	/* attribute_id: SPW_TAG + user index */
#define SPW_IDLE_MS	2000		/* stragglers after the end */

enum ukind { U_SPW, U_SPFI_WRITE, U_SPFI_READ };

struct ujob {
	const char *name;
	enum ukind kind;
	uint32_t period_ms;
	uint32_t pages;			/* SPFI; SPW sends --mixed-spw-bytes */
};

struct user {
	const struct ujob *j;
	unsigned idx;
	uint32_t nn;
	pthread_t th;
	/* SPFI */
	uint32_t sid;
	struct ramon_spfi_stream s;
	struct a_pages buf, ref;	/* ref: the read stream's pattern */
	uint32_t reported;
	/* results; the user thread updates them under slock, the reports read them */
	pthread_mutex_t slock;
	uint64_t ops, late, bytes, op_ns_max, op_ns_sum;
	uint64_t ival_op_ns_max;	/* longest operation since the last interim report */
	uint64_t p_ops, p_late, p_bytes, p_back;	/* at the last interim report */
	uint64_t spw_back, spw_bad;	/* under the RX lock */
	uint32_t bad_pages;
	int failed;
	struct ramon_status st;
};

static struct {
	struct user u[MAX_USERS];
	unsigned n;
	uint64_t t_start, t_end;
	pthread_mutex_t lock;		/* the SPW counters, from the RX thread */
	uint64_t spw_foreign, last_rx_ns;
} M = { .lock = PTHREAD_MUTEX_INITIALIZER };

static uint32_t spw_seed(unsigned idx, uint16_t id)
{
	return 0xA5000000u ^ (idx << 16) ^ id;
}

/* ---- SPW ---- */

static void mixed_spw_cb(void *user, const struct ramon_spw_rx *rx)
{
	const struct ramon_spw_hdr *h = rx->hdr;
	unsigned idx;
	int ok;

	(void)user;
	if (!h || (rx->flags & (RAMON_SPW_RX_HDR_CRC_BAD | RAMON_SPW_RX_TRUNCATED)) ||
	    (h->attribute_id & 0xFF000000u) != SPW_TAG ||
	    (idx = h->attribute_id & 0xFF) >= M.n || M.u[idx].j->kind != U_SPW) {
		pthread_mutex_lock(&M.lock);
		M.spw_foreign++;
		pthread_mutex_unlock(&M.lock);
		return;
	}
	ok = !rx->flags && rx->payload_len == A.o.mixed_spw_bytes &&
	     tc_verify(rx->payload, rx->payload_len, spw_seed(idx, h->packet_id)) < 0;
	pthread_mutex_lock(&M.lock);
	M.u[idx].spw_back++;
	M.u[idx].spw_bad += !ok;
	M.last_rx_ns = tc_now_ns();
	pthread_mutex_unlock(&M.lock);
}

/* ---- one operation per user kind ---- */

static int op_spw(struct user *u, uint64_t seq)
{
	struct ramon_spw_msg m;
	struct ramon_iov iov;

	memset(&m, 0, sizeof(m));
	m.dst = A.o.spw_node;
	m.app_type = A.o.spw_app;
	m.attribute_id = SPW_TAG + u->idx;
	m.packet_id = (uint16_t)seq;
	m.flags = RAMON_SPW_MSG_KEEP_ID;
	tc_fill(u->ref.ref, A.o.mixed_spw_bytes, spw_seed(u->idx, m.packet_id));
	iov.base = u->ref.ref;
	iov.len = A.o.mixed_spw_bytes;
	return ramon_spw_send(A.c, u->nn, &m, &iov, 1, 0, &u->st);
}

static int op_spfi_write(struct user *u)
{
	return ramon_spfi_stream_append(A.c, &u->s, &u->buf.b, 0, u->j->pages, 0, &u->st);
}

static int op_spfi_read(struct user *u, uint64_t seq)
{
	struct stamp {
		uint32_t magic, sid, run, page;
	} want, have;
	uint32_t i, full = (uint32_t)(seq % u->j->pages);
	const uint8_t *g;
	int ret;

	ret = ramon_spfi_read_seq(A.c, u->nn, u->sid, u->s.first_page, u->j->pages, &u->buf.b, 0, 0,
				  &u->st);
	if (ret)
		return ret;
	/* every page's stamp, and one whole page per read */
	memcpy(&want, u->ref.b.ptr, sizeof(want));
	for (i = 0; i < u->j->pages; i++) {
		g = (const uint8_t *)u->buf.b.ptr + (size_t)i * PAGE;
		want.page = i;
		memcpy(&have, g, sizeof(have));
		if (memcmp(&have, &want, sizeof(want)) ||
		    (i == full && memcmp(g + sizeof(want), u->ref.ref + (size_t)i * PAGE + sizeof(want),
					 PAGE - sizeof(want)))) {
			u->bad_pages++;
			if (u->reported++ < 3)
				tc_info("  %s: read %llu page %u differs (stamp read: page %u, stream %u)",
					u->j->name, (unsigned long long)seq, i, have.page, have.sid);
		}
	}
	return 0;
}

/* ---- the user thread: one operation per slot ---- */

static void sleep_until(uint64_t t_ns)
{
	struct timespec ts = { (time_t)(t_ns / 1000000000ull), (long)(t_ns % 1000000000ull) };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static void *user_thread(void *arg)
{
	struct user *u = arg;
	const uint64_t period = (uint64_t)u->j->period_ms * 1000000ull;
	const uint64_t op_bytes = u->j->kind == U_SPW ? A.o.mixed_spw_bytes :
				  (uint64_t)u->j->pages * PAGE;
	uint64_t slot = 0, next = M.t_start, t0, dt, now;
	int ret;

	while (next < M.t_end) {
		sleep_until(next);
		t0 = tc_now_ns();
		ret = u->j->kind == U_SPW ? op_spw(u, slot) :
		      u->j->kind == U_SPFI_WRITE ? op_spfi_write(u) : op_spfi_read(u, slot);
		dt = tc_now_ns() - t0;
		pthread_mutex_lock(&u->slock);
		if (ret) {
			u->failed = 1;
			pthread_mutex_unlock(&u->slock);
			break;
		}
		u->ops++;
		u->bytes += op_bytes;
		u->op_ns_sum += dt;
		if (dt > u->op_ns_max)
			u->op_ns_max = dt;
		if (dt > u->ival_op_ns_max)
			u->ival_op_ns_max = dt;
		/* the next slot that is still ahead; the ones passed are missed */
		slot++;
		next = M.t_start + slot * period;
		now = tc_now_ns();
		if (now > next) {
			uint64_t behind = (now - next) / period + 1;

			u->late += behind;
			slot += behind;
			next = M.t_start + slot * period;
		}
		pthread_mutex_unlock(&u->slock);
	}
	return NULL;
}

/* ---- interim reports while the users run ---- */

static void interim_report(uint64_t t_ns, uint64_t ival_ns)
{
	uint64_t ops, late, bytes, max_ns, back = 0;
	double secs = ival_ns / 1e9;
	int failed;
	unsigned i, min = (unsigned)((t_ns - M.t_start) / 60000000000ull);

	for (i = 0; i < M.n; i++) {
		struct user *u = &M.u[i];
		int spw = u->j->kind == U_SPW;

		pthread_mutex_lock(&u->slock);
		ops = u->ops;
		late = u->late;
		bytes = u->bytes;
		max_ns = u->ival_op_ns_max;
		u->ival_op_ns_max = 0;
		failed = u->failed;
		pthread_mutex_unlock(&u->slock);
		if (spw) {
			pthread_mutex_lock(&M.lock);
			back = u->spw_back;
			pthread_mutex_unlock(&M.lock);
		}
		tc_info("[%3u min] %-7s %llu done (+%llu), %llu missed (+%llu), op max %.1f ms, %.1f %s%s",
			min, u->j->name, (unsigned long long)ops, (unsigned long long)(ops - u->p_ops),
			(unsigned long long)late, (unsigned long long)(late - u->p_late), max_ns / 1e6,
			secs > 0 ? (bytes - u->p_bytes) / (spw ? 1024.0 : MIB) / secs : 0,
			spw ? "KiB/s" : "MiB/s", failed ? ", STOPPED (error)" : "");
		if (spw)
			tc_info("[%3u min] %-7s   %llu back (+%llu)", min, u->j->name,
				(unsigned long long)back, (unsigned long long)(back - u->p_back));
		u->p_ops = ops;
		u->p_late = late;
		u->p_bytes = bytes;
		u->p_back = back;
	}
}

/* while the users run: a report every --mixed-report-minutes that falls before the end */
static void interim_reports(void)
{
	const uint64_t period = (uint64_t)A.o.mixed_report_min * 60000000000ull;
	uint64_t next, last = M.t_start;

	if (!period)
		return;
	for (next = M.t_start + period; next < M.t_end; next += period) {
		sleep_until(next);
		interim_report(next, next - last);
		last = next;
	}
}

/* ---- setup, run, report ---- */

static int user_prepare(struct user *u)
{
	struct ramon_status st;
	uint64_t slots = (uint64_t)A.o.mixed_seconds * 1000 / u->j->period_ms + 1;

	switch (u->j->kind) {
	case U_SPW:
		/* the payload is built in ordinary memory */
		u->ref.ref = malloc(A.o.mixed_spw_bytes ? A.o.mixed_spw_bytes : 1);
		if (!u->ref.ref) {
			tc_fail("out of memory");
			return -1;
		}
		return 0;
	case U_SPFI_WRITE:
		if (a_pages_alloc(&u->buf, u->j->pages, 0))
			return -1;
		return a_check(ramon_spfi_stream_open(A.c, u->nn, u->sid, (uint32_t)(slots * u->j->pages),
						      &u->s, &st), "OPEN", &st);
	case U_SPFI_READ:
		/* the stream to read: its pages written once, stamped, flushed */
		if (a_pages_alloc(&u->ref, u->j->pages, 1) || a_pages_alloc(&u->buf, u->j->pages, 0))
			return -1;
		a_stamp_pages(&u->ref, u->j->pages, u->sid, 0);
		if (a_check(ramon_spfi_stream_open(A.c, u->nn, u->sid, u->j->pages, &u->s, &st), "OPEN",
			    &st) ||
		    a_check(ramon_spfi_stream_append(A.c, &u->s, &u->ref.b, 0, u->j->pages, 0, &st),
			    "fill the read stream", &st) ||
		    a_check(ramon_spfi_stream_flush(A.c, &u->s, &st), "FLUSH", &st))
			return -1;
		return 0;
	}
	return -1;
}

static void user_cleanup(struct user *u)
{
	struct ramon_status st;

	if (u->j->kind != U_SPW && (u->s.open || u->s.pages_written))
		a_check(ramon_spfi_stream_close(A.c, &u->s, 1, &st), "CLOSE and DELETE", &st);
	a_pages_free(&u->buf);
	if (u->j->kind == U_SPW)
		free(u->ref.ref);
	else
		a_pages_free(&u->ref);
	memset(&u->ref, 0, sizeof(u->ref));
}

static void report(struct user *u, double secs, int *any_late)
{
	const struct ujob *j = u->j;
	double avg = u->ops ? u->op_ns_sum / 1e6 / u->ops : 0;
	char what[64];
	uint64_t lost = 0;

	if (j->kind == U_SPW) {
		snprintf(what, sizeof(what), "%u-byte SPW message", A.o.mixed_spw_bytes);
		lost = u->ops > u->spw_back ? u->ops - u->spw_back : 0;
	} else {
		snprintf(what, sizeof(what), "SPFI %s of %u page(s)",
			 j->kind == U_SPFI_WRITE ? "write" : "read", j->pages);
	}
	tc_info("%-7s %s every %u ms: %llu done, %llu deadline(s) missed, op %.1f ms avg / %.1f ms max, "
		"%.1f %s%s", j->name, what, j->period_ms, (unsigned long long)u->ops,
		(unsigned long long)u->late, avg, u->op_ns_max / 1e6,
		secs > 0 ? u->bytes / (j->kind == U_SPW ? 1024.0 : MIB) / secs : 0,
		j->kind == U_SPW ? "KiB/s" : "MiB/s", j->kind == U_SPFI_READ ? ", pages checked" : "");
	if (j->kind == U_SPW)
		tc_info("%-7s   %llu back, %llu lost, %llu bad", j->name,
			(unsigned long long)u->spw_back, (unsigned long long)lost,
			(unsigned long long)u->spw_bad);
	if (u->failed)
		tc_fail_st(j->name, &u->st);
	if (lost || u->spw_bad)
		tc_fail("%s: %llu message(s) lost, %llu bad", j->name, (unsigned long long)lost,
			(unsigned long long)u->spw_bad);
	if (u->bad_pages)
		tc_fail("%s: %u bad page(s) read", j->name, u->bad_pages);
	if (u->late) {
		*any_late = 1;
		if (!A.o.mixed_late_ok)
			tc_fail("%s: %llu deadline(s) of %u ms missed (longest operation %.1f ms)",
				j->name, (unsigned long long)u->late, j->period_ms, u->op_ns_max / 1e6);
	}
}

static void run_mixed(const struct ujob *jobs, unsigned n)
{
	uint32_t spw_use, spfi_use, spw_nn, spfi_nn, ids[MAX_USERS], n_spfi = 0, k = 0;
	char sum[160] = "";
	size_t used = 0;
	unsigned i, started = 0;
	uint64_t late = 0, slots = 0;
	int any_late = 0, spw_users = 0, rx_on = 0;
	struct ramon_status st;
	double secs;

	memset(M.u, 0, sizeof(M.u));
	M.n = n;
	M.spw_foreign = 0;
	for (i = 0; i < n; i++) {
		spw_users += jobs[i].kind == U_SPW;
		n_spfi += jobs[i].kind != U_SPW;
	}
	spfi_use = n_spfi ? a_spfi_setup() : 0;
	spw_use = a_spw_setup();
	if ((n_spfi && !spfi_use) || (spw_users && !spw_use)) {
		a_summary("no %s link", !spw_use ? "SPW" : "SPFI");
		return;
	}
	/* the first NN of each */
	for (spw_nn = 0; spw_nn < RAMON_NN_COUNT && !((spw_use >> spw_nn) & 1); spw_nn++)
		;
	for (spfi_nn = 0; spfi_nn < RAMON_NN_COUNT && !((spfi_use >> spfi_nn) & 1); spfi_nn++)
		;
	if (n_spfi && a_spfi_free_ids(spfi_nn, ids, n_spfi)) {
		a_summary("no room for %u streams", n_spfi);
		return;
	}
	tc_info("%u s, SPW on spw%u (to node %u, %u-byte messages), SPFI on spfi%u", A.o.mixed_seconds,
		spw_nn, A.o.spw_node, A.o.mixed_spw_bytes, spfi_nn);

	for (i = 0; i < n; i++) {
		struct user *u = &M.u[i];

		u->j = &jobs[i];
		u->idx = i;
		pthread_mutex_init(&u->slock, NULL);
		u->nn = jobs[i].kind == U_SPW ? spw_nn : spfi_nn;
		if (jobs[i].kind != U_SPW)
			u->sid = ids[k++];
		if (user_prepare(u))
			goto out;
	}
	if (spw_users) {
		if (a_check(ramon_spw_drain(A.c, spw_nn, 20, NULL, &st), "SPW drain", &st) ||
		    a_check(ramon_spw_rx_start(A.c, spw_nn, mixed_spw_cb, NULL, &st), "SPW RX thread",
			    &st))
			goto out;
		rx_on = 1;
	}

	M.t_start = tc_now_ns() + 50000000ull;		/* all users start together */
	M.t_end = M.t_start + (uint64_t)A.o.mixed_seconds * 1000000000ull;
	for (i = 0; i < n; i++, started++)
		if (pthread_create(&M.u[i].th, NULL, user_thread, &M.u[i])) {
			tc_fail("pthread_create failed");
			break;
		}
	if (started == n)
		interim_reports();
	for (i = 0; i < started; i++)
		pthread_join(M.u[i].th, NULL);
	secs = (tc_now_ns() - M.t_start) / 1e9;
	if (rx_on) {
		/* the last messages may still be on their way back: wait while they arrive */
		pthread_mutex_lock(&M.lock);
		M.last_rx_ns = tc_now_ns();
		pthread_mutex_unlock(&M.lock);
		for (;;) {
			uint64_t sent = 0, back = 0, idle;

			pthread_mutex_lock(&M.lock);
			for (i = 0; i < n; i++)
				if (jobs[i].kind == U_SPW) {
					sent += M.u[i].ops;
					back += M.u[i].spw_back;
				}
			idle = tc_now_ns() - M.last_rx_ns;
			pthread_mutex_unlock(&M.lock);
			if (back >= sent || idle >= (uint64_t)SPW_IDLE_MS * 1000000ull)
				break;
			tc_sleep_ms(20);
		}
		ramon_spw_rx_stop(A.c, spw_nn);
		rx_on = 0;
	}
	/* the reader's last read, completely */
	for (i = 0; i < n; i++)
		if (jobs[i].kind == U_SPFI_READ && M.u[i].ops)
			M.u[i].bad_pages += a_check_pages(&M.u[i].buf, &M.u[i].ref, jobs[i].pages,
							  M.u[i].sid, 0, &M.u[i].reported);

	for (i = 0; i < n; i++) {
		report(&M.u[i], secs, &any_late);
		late += M.u[i].late;
		slots += M.u[i].ops + M.u[i].late;
	}
	used = (size_t)snprintf(sum, sizeof(sum), "%llu of %llu deadlines missed",
				(unsigned long long)late, (unsigned long long)slots);
	/* the heavy SPFI users' rates */
	for (i = 0; i < n && used < sizeof(sum); i++)
		if (jobs[i].kind != U_SPW && jobs[i].period_ms < 1000)
			used += (size_t)snprintf(sum + used, sizeof(sum) - used, "; %s %.0f MiB/s",
						 jobs[i].name, secs > 0 ? M.u[i].bytes / MIB / secs : 0);
	if (M.spw_foreign)
		tc_info("%llu other SPW packet(s) to node %u were ignored",
			(unsigned long long)M.spw_foreign, A.o.spw_node);
	if (any_late && A.o.mixed_late_ok)
		tc_info("deadlines were missed; not counted as a failure (--mixed-late-ok)");
	a_summary("%s", sum);
out:
	if (rx_on)
		ramon_spw_rx_stop(A.c, spw_nn);
	for (i = 0; i < n; i++)
		if (M.u[i].j) {
			user_cleanup(&M.u[i]);
			pthread_mutex_destroy(&M.u[i].slock);
		}
}

static void step_mixed_write(void)
{
	static const struct ujob jobs[] = {
		{ "spw-a", U_SPW, 1000, 0 },
		{ "spw-b", U_SPW, 1000, 0 },
		{ "spw-c", U_SPW, 100, 0 },
		{ "spfi-a", U_SPFI_WRITE, 1000, 12 },
		{ "spfi-b", U_SPFI_WRITE, 1000, 2 },
		{ "spfi-c", U_SPFI_WRITE, 100, 4096 },
	};

	run_mixed(jobs, ARRAY_SIZE(jobs));
}

static void step_mixed_rw(void)
{
	static const struct ujob jobs[] = {
		{ "spw-a", U_SPW, 1000, 0 },
		{ "spw-b", U_SPW, 1000, 0 },
		{ "spw-c", U_SPW, 100, 0 },
		{ "spfi-a", U_SPFI_WRITE, 1000, 12 },
		{ "spfi-b", U_SPFI_WRITE, 1000, 2 },
		{ "spfi-c", U_SPFI_WRITE, 100, 2048 },
		{ "spfi-d", U_SPFI_READ, 100, 2048 },
	};

	run_mixed(jobs, ARRAY_SIZE(jobs));
}

const struct step accept_mixed[] = {
	{ "mixed-write", step_mixed_write, NEEDS_DEV, NULL,
	  "test #1: 3 SPW users (1 s, 1 s, 0.1 s) + SPFI writes of 12, 2, 4096 pages (1 s, 1 s, 0.1 s)",
	  NULL },
	{ "mixed-rw", step_mixed_rw, NEEDS_DEV, NULL,
	  "test #2: 3 SPW users + SPFI writes of 12, 2, 2048 pages and reads of 2048 pages", NULL },
	{ NULL, NULL, 0, NULL, NULL, NULL },
};

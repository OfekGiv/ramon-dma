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

#define SPFI_PAGE_BYTES	RAMON_SPFI_PAGE
#define MIB		1048576.0
#define MAX_USERS	8
#define SPW_TAG		0x6D000000u	/* attribute_id: SPW_TAG + user index */
#define SPW_IDLE_MS	2000		/* stragglers after the end */

enum user_kind { U_SPW, U_SPFI_WRITE, U_SPFI_READ };

struct user_job {
	const char *name;
	enum user_kind kind;
	uint32_t period_ms;
	uint32_t pages;			/* SPFI; SPW sends --mixed-spw-bytes */
};

struct user {
	const struct user_job *job;
	unsigned idx;
	uint32_t nn;
	pthread_t th;
	/* SPFI */
	uint32_t sid;
	struct ramon_spfi_stream stream;
	struct a_pages buf, reference;	/* the read stream's pattern, or the SPW payload */
	uint32_t reported;
	/* results; the user thread updates them under stats_lock, the reports read them */
	pthread_mutex_t stats_lock;
	uint64_t ops, late, bytes, op_ns_max, op_ns_sum;
	uint64_t interval_op_max_ns;	/* longest operation since the last interim report */
	uint64_t prev_ops, prev_late, prev_bytes, prev_back;	/* at the last interim report */
	uint64_t spw_back, spw_bad;	/* under the RX lock */
	uint32_t bad_pages;
	int failed;
	struct ramon_status st;
};

static struct {
	struct user users[MAX_USERS];
	unsigned n_users;
	uint64_t start_ns, end_ns;
	pthread_mutex_t rx_lock;	/* the SPW counters, from the RX thread */
	uint64_t spw_foreign, last_rx_ns;
} mixed = { .rx_lock = PTHREAD_MUTEX_INITIALIZER };

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
	    (idx = h->attribute_id & 0xFF) >= mixed.n_users ||
	    mixed.users[idx].job->kind != U_SPW) {
		pthread_mutex_lock(&mixed.rx_lock);
		mixed.spw_foreign++;
		pthread_mutex_unlock(&mixed.rx_lock);
		return;
	}
	ok = !rx->flags && rx->payload_len == a_env.opts.mixed_spw_bytes &&
	     tc_find_mismatch(rx->payload, rx->payload_len, spw_seed(idx, h->packet_id)) < 0;
	pthread_mutex_lock(&mixed.rx_lock);
	mixed.users[idx].spw_back++;
	mixed.users[idx].spw_bad += !ok;
	mixed.last_rx_ns = tc_now_ns();
	pthread_mutex_unlock(&mixed.rx_lock);
}

/* ---- one operation per user kind ---- */

static int op_spw(struct user *u, uint64_t seq)
{
	struct ramon_spw_msg m;
	struct ramon_iov iov;

	memset(&m, 0, sizeof(m));
	m.dst = a_env.opts.spw_node;
	m.app_type = a_env.opts.spw_app;
	m.attribute_id = SPW_TAG + u->idx;
	m.packet_id = (uint16_t)seq;
	m.flags = RAMON_SPW_MSG_KEEP_ID;
	tc_fill(u->reference.pattern, a_env.opts.mixed_spw_bytes, spw_seed(u->idx, m.packet_id));
	iov.base = u->reference.pattern;
	iov.len = a_env.opts.mixed_spw_bytes;
	return ramon_spw_send(a_env.c, u->nn, &m, &iov, 1, 0, &u->st);
}

static int op_spfi_write(struct user *u)
{
	return ramon_spfi_stream_append(a_env.c, &u->stream, &u->buf.dma, 0, u->job->pages, 0,
					&u->st);
}

static int op_spfi_read(struct user *u, uint64_t seq)
{
	struct stamp {
		uint32_t magic, sid, run, page;
	} want, have;
	uint32_t i, full_page = (uint32_t)(seq % u->job->pages);
	const uint8_t *g;
	int ret;

	ret = ramon_spfi_read_seq(a_env.c, u->nn, u->sid, u->stream.first_page, u->job->pages,
				  &u->buf.dma, 0, 0, &u->st);
	if (ret)
		return ret;
	/* every page's stamp, and one whole page per read */
	memcpy(&want, u->reference.dma.ptr, sizeof(want));
	for (i = 0; i < u->job->pages; i++) {
		g = (const uint8_t *)u->buf.dma.ptr + (size_t)i * SPFI_PAGE_BYTES;
		want.page = i;
		memcpy(&have, g, sizeof(have));
		if (memcmp(&have, &want, sizeof(want)) ||
		    (i == full_page &&
		     memcmp(g + sizeof(want),
			    u->reference.pattern + (size_t)i * SPFI_PAGE_BYTES + sizeof(want),
			    SPFI_PAGE_BYTES - sizeof(want)))) {
			u->bad_pages++;
			if (u->reported++ < 3)
				tc_info("  %s: read %llu page %u differs (stamp read: page %u, stream %u)",
					u->job->name, (unsigned long long)seq, i, have.page,
					have.sid);
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
	const uint64_t period_ns = (uint64_t)u->job->period_ms * 1000000ull;
	const uint64_t op_bytes = u->job->kind == U_SPW ? a_env.opts.mixed_spw_bytes :
				  (uint64_t)u->job->pages * SPFI_PAGE_BYTES;
	uint64_t slot = 0, next = mixed.start_ns, t0, dt, now;
	int ret;

	while (next < mixed.end_ns) {
		sleep_until(next);
		t0 = tc_now_ns();
		ret = u->job->kind == U_SPW ? op_spw(u, slot) :
		      u->job->kind == U_SPFI_WRITE ? op_spfi_write(u) : op_spfi_read(u, slot);
		dt = tc_now_ns() - t0;
		pthread_mutex_lock(&u->stats_lock);
		if (ret) {
			u->failed = 1;
			pthread_mutex_unlock(&u->stats_lock);
			break;
		}
		u->ops++;
		u->bytes += op_bytes;
		u->op_ns_sum += dt;
		if (dt > u->op_ns_max)
			u->op_ns_max = dt;
		if (dt > u->interval_op_max_ns)
			u->interval_op_max_ns = dt;
		/* the next slot that is still ahead; the ones passed are missed */
		slot++;
		next = mixed.start_ns + slot * period_ns;
		now = tc_now_ns();
		if (now > next) {
			uint64_t behind = (now - next) / period_ns + 1;

			u->late += behind;
			slot += behind;
			next = mixed.start_ns + slot * period_ns;
		}
		pthread_mutex_unlock(&u->stats_lock);
	}
	return NULL;
}

/* ---- interim reports while the users run ---- */

static void interim_report(uint64_t t_ns, uint64_t interval_ns)
{
	uint64_t ops, late, bytes, max_ns, back = 0;
	double secs = interval_ns / 1e9;
	int failed;
	unsigned i, minutes = (unsigned)((t_ns - mixed.start_ns) / 60000000000ull);

	for (i = 0; i < mixed.n_users; i++) {
		struct user *u = &mixed.users[i];
		int is_spw = u->job->kind == U_SPW;

		pthread_mutex_lock(&u->stats_lock);
		ops = u->ops;
		late = u->late;
		bytes = u->bytes;
		max_ns = u->interval_op_max_ns;
		u->interval_op_max_ns = 0;
		failed = u->failed;
		pthread_mutex_unlock(&u->stats_lock);
		if (is_spw) {
			pthread_mutex_lock(&mixed.rx_lock);
			back = u->spw_back;
			pthread_mutex_unlock(&mixed.rx_lock);
		}
		tc_info("[%3u min] %-7s %llu done (+%llu), %llu missed (+%llu), op max %.1f ms, %.1f %s%s",
			minutes, u->job->name, (unsigned long long)ops,
			(unsigned long long)(ops - u->prev_ops), (unsigned long long)late,
			(unsigned long long)(late - u->prev_late), max_ns / 1e6,
			secs > 0 ? (bytes - u->prev_bytes) / (is_spw ? 1024.0 : MIB) / secs : 0,
			is_spw ? "KiB/s" : "MiB/s", failed ? ", STOPPED (error)" : "");
		if (is_spw)
			tc_info("[%3u min] %-7s   %llu back (+%llu)", minutes, u->job->name,
				(unsigned long long)back,
				(unsigned long long)(back - u->prev_back));
		u->prev_ops = ops;
		u->prev_late = late;
		u->prev_bytes = bytes;
		u->prev_back = back;
	}
}

/* while the users run: a report every --mixed-report-minutes that falls before the end */
static void run_interim_reports(void)
{
	const uint64_t period_ns = (uint64_t)a_env.opts.mixed_report_minutes * 60000000000ull;
	uint64_t next, last = mixed.start_ns;

	if (!period_ns)
		return;
	for (next = mixed.start_ns + period_ns; next < mixed.end_ns; next += period_ns) {
		sleep_until(next);
		interim_report(next, next - last);
		last = next;
	}
}

/* ---- setup, run, report ---- */

static int user_prepare(struct user *u)
{
	struct ramon_status st;
	uint64_t slots = (uint64_t)a_env.opts.mixed_seconds * 1000 / u->job->period_ms + 1;

	switch (u->job->kind) {
	case U_SPW:
		/* the payload is built in ordinary memory */
		u->reference.pattern = malloc(a_env.opts.mixed_spw_bytes ?
					     a_env.opts.mixed_spw_bytes : 1);
		if (!u->reference.pattern) {
			tc_fail("out of memory");
			return -1;
		}
		return 0;
	case U_SPFI_WRITE:
		if (a_pages_alloc(&u->buf, u->job->pages, 0))
			return -1;
		return a_check(ramon_spfi_stream_open(a_env.c, u->nn, u->sid,
						      (uint32_t)(slots * u->job->pages),
						      &u->stream, &st), "OPEN", &st);
	case U_SPFI_READ:
		/* the stream to read: its pages written once, stamped, flushed */
		if (a_pages_alloc(&u->reference, u->job->pages, 1) ||
		    a_pages_alloc(&u->buf, u->job->pages, 0))
			return -1;
		a_stamp_pages(&u->reference, u->job->pages, u->sid, 0);
		if (a_check(ramon_spfi_stream_open(a_env.c, u->nn, u->sid, u->job->pages,
						   &u->stream, &st), "OPEN", &st) ||
		    a_check(ramon_spfi_stream_append(a_env.c, &u->stream, &u->reference.dma, 0,
						     u->job->pages, 0, &st),
			    "fill the read stream", &st) ||
		    a_check(ramon_spfi_stream_flush(a_env.c, &u->stream, &st), "FLUSH", &st))
			return -1;
		return 0;
	}
	return -1;
}

static void user_cleanup(struct user *u)
{
	struct ramon_status st;

	if (u->job->kind != U_SPW && (u->stream.open || u->stream.pages_written))
		a_check(ramon_spfi_stream_close(a_env.c, &u->stream, 1, &st), "CLOSE and DELETE",
			&st);
	a_pages_free(&u->buf);
	if (u->job->kind == U_SPW)
		free(u->reference.pattern);
	else
		a_pages_free(&u->reference);
	memset(&u->reference, 0, sizeof(u->reference));
}

static void report_user(struct user *u, double secs, int *any_late)
{
	const struct user_job *j = u->job;
	double avg = u->ops ? u->op_ns_sum / 1e6 / u->ops : 0;
	char what[64];
	uint64_t lost = 0;

	if (j->kind == U_SPW) {
		snprintf(what, sizeof(what), "%u-byte SPW message", a_env.opts.mixed_spw_bytes);
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
		if (!a_env.opts.mixed_late_ok)
			tc_fail("%s: %llu deadline(s) of %u ms missed (longest operation %.1f ms)",
				j->name, (unsigned long long)u->late, j->period_ms, u->op_ns_max / 1e6);
	}
}

static void run_mixed(const struct user_job *jobs, unsigned n_jobs)
{
	uint32_t spw_test_mask, spfi_test_mask, spw_nn, spfi_nn, ids[MAX_USERS], n_spfi_users = 0,
		 k = 0;
	char summary[160] = "";
	size_t used = 0;
	unsigned i, started = 0;
	uint64_t late = 0, slots = 0;
	int any_late = 0, n_spw_users = 0, rx_on = 0;
	struct ramon_status st;
	double secs;

	memset(mixed.users, 0, sizeof(mixed.users));
	mixed.n_users = n_jobs;
	mixed.spw_foreign = 0;
	for (i = 0; i < n_jobs; i++) {
		n_spw_users += jobs[i].kind == U_SPW;
		n_spfi_users += jobs[i].kind != U_SPW;
	}
	spfi_test_mask = n_spfi_users ? a_spfi_setup() : 0;
	spw_test_mask = a_spw_setup();
	if ((n_spfi_users && !spfi_test_mask) || (n_spw_users && !spw_test_mask)) {
		a_set_summary("no %s link", !spw_test_mask ? "SPW" : "SPFI");
		return;
	}
	/* the first NN of each */
	for (spw_nn = 0; spw_nn < RAMON_NN_COUNT && !((spw_test_mask >> spw_nn) & 1); spw_nn++)
		;
	for (spfi_nn = 0; spfi_nn < RAMON_NN_COUNT && !((spfi_test_mask >> spfi_nn) & 1); spfi_nn++)
		;
	if (n_spfi_users && a_spfi_find_free_ids(spfi_nn, ids, n_spfi_users)) {
		a_set_summary("no room for %u streams", n_spfi_users);
		return;
	}
	tc_info("%u s, SPW on spw%u (to node %u, %u-byte messages), SPFI on spfi%u",
		a_env.opts.mixed_seconds, spw_nn, a_env.opts.spw_node, a_env.opts.mixed_spw_bytes,
		spfi_nn);

	for (i = 0; i < n_jobs; i++) {
		struct user *u = &mixed.users[i];

		u->job = &jobs[i];
		u->idx = i;
		pthread_mutex_init(&u->stats_lock, NULL);
		u->nn = jobs[i].kind == U_SPW ? spw_nn : spfi_nn;
		if (jobs[i].kind != U_SPW)
			u->sid = ids[k++];
		if (user_prepare(u))
			goto out;
	}
	if (n_spw_users) {
		if (a_check(ramon_spw_drain(a_env.c, spw_nn, 20, NULL, &st), "SPW drain", &st) ||
		    a_check(ramon_spw_rx_start(a_env.c, spw_nn, mixed_spw_cb, NULL, &st),
			    "SPW RX thread", &st))
			goto out;
		rx_on = 1;
	}

	mixed.start_ns = tc_now_ns() + 50000000ull;	/* all users start together */
	mixed.end_ns = mixed.start_ns + (uint64_t)a_env.opts.mixed_seconds * 1000000000ull;
	for (i = 0; i < n_jobs; i++, started++)
		if (pthread_create(&mixed.users[i].th, NULL, user_thread, &mixed.users[i])) {
			tc_fail("pthread_create failed");
			break;
		}
	if (started == n_jobs)
		run_interim_reports();
	for (i = 0; i < started; i++)
		pthread_join(mixed.users[i].th, NULL);
	secs = (tc_now_ns() - mixed.start_ns) / 1e9;
	if (rx_on) {
		/* the last messages may still be on their way back: wait while they arrive */
		pthread_mutex_lock(&mixed.rx_lock);
		mixed.last_rx_ns = tc_now_ns();
		pthread_mutex_unlock(&mixed.rx_lock);
		for (;;) {
			uint64_t sent = 0, back = 0, idle;

			pthread_mutex_lock(&mixed.rx_lock);
			for (i = 0; i < n_jobs; i++)
				if (jobs[i].kind == U_SPW) {
					sent += mixed.users[i].ops;
					back += mixed.users[i].spw_back;
				}
			idle = tc_now_ns() - mixed.last_rx_ns;
			pthread_mutex_unlock(&mixed.rx_lock);
			if (back >= sent || idle >= (uint64_t)SPW_IDLE_MS * 1000000ull)
				break;
			tc_sleep_ms(20);
		}
		ramon_spw_rx_stop(a_env.c, spw_nn);
		rx_on = 0;
	}
	/* the reader's last read, completely */
	for (i = 0; i < n_jobs; i++)
		if (jobs[i].kind == U_SPFI_READ && mixed.users[i].ops)
			mixed.users[i].bad_pages += a_check_pages(&mixed.users[i].buf,
								  &mixed.users[i].reference,
								  jobs[i].pages, mixed.users[i].sid,
								  0, &mixed.users[i].reported);

	for (i = 0; i < n_jobs; i++) {
		report_user(&mixed.users[i], secs, &any_late);
		late += mixed.users[i].late;
		slots += mixed.users[i].ops + mixed.users[i].late;
	}
	used = (size_t)snprintf(summary, sizeof(summary), "%llu of %llu deadlines missed",
				(unsigned long long)late, (unsigned long long)slots);
	/* the heavy SPFI users' rates */
	for (i = 0; i < n_jobs && used < sizeof(summary); i++)
		if (jobs[i].kind != U_SPW && jobs[i].period_ms < 1000)
			used += (size_t)snprintf(summary + used, sizeof(summary) - used,
						 "; %s %.0f MiB/s", jobs[i].name,
						 secs > 0 ? mixed.users[i].bytes / MIB / secs : 0);
	if (mixed.spw_foreign)
		tc_info("%llu other SPW packet(s) to node %u were ignored",
			(unsigned long long)mixed.spw_foreign, a_env.opts.spw_node);
	if (any_late && a_env.opts.mixed_late_ok)
		tc_info("deadlines were missed; not counted as a failure (--mixed-late-ok)");
	a_set_summary("%s", summary);
out:
	if (rx_on)
		ramon_spw_rx_stop(a_env.c, spw_nn);
	for (i = 0; i < n_jobs; i++)
		if (mixed.users[i].job) {
			user_cleanup(&mixed.users[i]);
			pthread_mutex_destroy(&mixed.users[i].stats_lock);
		}
}

static void step_mixed_write(void)
{
	static const struct user_job jobs[] = {
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
	static const struct user_job jobs[] = {
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

const struct step a_mixed_steps[] = {
	{ "mixed-write", step_mixed_write, NEEDS_DEV, NULL,
	  "test #1: 3 SPW users (1 s, 1 s, 0.1 s) + SPFI writes of 12, 2, 4096 pages (1 s, 1 s, 0.1 s)",
	  NULL },
	{ "mixed-rw", step_mixed_rw, NEEDS_DEV, NULL,
	  "test #2: 3 SPW users + SPFI writes of 12, 2, 2048 pages and reads of 2048 pages", NULL },
	{ NULL, NULL, 0, NULL, NULL, NULL },
};

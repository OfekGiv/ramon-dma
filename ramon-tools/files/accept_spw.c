// SPDX-License-Identifier: MIT
/*
 * ramon_accept: SpaceWire steps.
 *
 * spw-link       links synced (was spwinit)
 * spw-nn-loop    packets to our own node with FPGA loopback off: the NN routes
 *                them back (far-end loopback; was loopback 0 + spwsend ... +68 100)
 * spw-fpga-loop  the same traffic with FPGA loopback on (was loopback 1 + spwsend)
 *
 * The loop steps send, for each payload size, a stream of packets addressed
 * to our node id (68) and check every packet that comes back on the library
 * RX thread: header CRC, payload CRC, length, application type, packet id in
 * sequence, payload pattern and the FPGA footer. At most --spw-window packets
 * are in flight. A size passes when every packet came back once, intact and
 * in order, with no driver RX overrun. Each size's packets carry a run tag in
 * attribute_id, so a straggler from an earlier size is reported, not charged
 * to the current one.
 */
#include "accept.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IDLE_MS		2000	/* nothing came back for this long: the rest is lost */
#define MAX_REPORTED	5	/* bad packets printed per size */
#define MIB		1048576.0
#define TAG_BASE	0x5A000000u	/* attribute_id of the loop packets: TAG_BASE + run number */
#define IDS		65536		/* packet_id is 16 bits */
#define SPW_CHAR_BITS	10		/* a SpaceWire data character on the wire */

/* ---- setup ---- */

/*
 * SPW layer initialised once; links synced on first use, as the old
 * spw_init(): while (0x28 & 7) != 5, write 0 to 0x1C and wait 10 ms, up to
 * --spw-sync-tries times. With --spw-nn only those NNs are synced. Returns
 * the NNs to test.
 */
uint32_t a_spw_setup(void)
{
	struct ramon_spw_config cfg;
	struct ramon_status st;
	uint32_t nn, raw, tries, want;
	uint64_t t0;
	int r;

	if (!A.spw_inited) {
		ramon_spw_config_default(&cfg);
		cfg.node_id = A.o.spw_node;
		cfg.sync_tries = A.o.spw_sync_tries;
		if (a_check(ramon_spw_init(A.c, &cfg, &st), "ramon_spw_init", &st))
			return 0;
		A.spw_inited = 1;
		for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
			if (!ramon_spw_present(A.c, nn) ||
			    (A.o.spw_nn_mask && !(A.o.spw_nn_mask & (1u << nn))))
				continue;
			r = ramon_spw_link_status(A.c, nn, &raw, &st);
			if (r == 1) {
				tc_info("spw%u: link already synced (0x28 = 0x%x)", nn, raw);
			} else if (r == 0) {
				t0 = tc_now_ns();
				if (!ramon_spw_sync(A.c, nn, &tries, &st)) {
					tc_info("spw%u: link synced after %u reset(s), %.1f s", nn, tries,
						tc_ms_since(t0) / 1000);
					r = 1;
				}
			}
			ramon_spw_link_status(A.c, nn, &raw, NULL);
			if (r == 1)
				A.spw_synced |= 1u << nn;
			else
				tc_info("spw%u: link not synced after %u resets (0x28 = 0x%x)", nn,
					A.o.spw_sync_tries, raw);
		}
	}
	want = A.o.spw_nn_mask ? A.o.spw_nn_mask : A.spw_synced;
	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		if ((want & (1u << nn)) && !(A.spw_synced & (1u << nn)))
			tc_fail("spw%u: link not synced (required by --spw-nn)", nn);
	if (!want)
		tc_fail("no SPW link synced");
	return want & A.spw_synced;
}

/* ---- spw-link (was spwinit) ---- */

static void step_spw_link(void)
{
	static const uint32_t ver[] = { RAMON_SPW_REG_VERSION, RAMON_SPW_REG_IP_MAJOR,
					RAMON_SPW_REG_IP_MINOR };
	uint32_t nn, use, raw, v[3], i;
	char win[8], line[96] = "", *p = line;

	use = a_spw_setup();
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!ramon_spw_present(A.c, nn))
			continue;
		snprintf(win, sizeof(win), "spw%u", nn);
		for (i = 0; i < ARRAY_SIZE(ver); i++)
			if (ramon_reg_read(A.c, win, ver[i], &v[i], NULL))
				v[i] = 0;
		ramon_spw_link_status(A.c, nn, &raw, NULL);
		tc_info("spw%u: version 0x%x, IP %u.%u, link 0x28 = 0x%x (%s)%s", nn, v[0], v[1], v[2],
			raw, (A.spw_synced >> nn) & 1 ? "synced" : "not synced",
			(use >> nn) & 1 ? ", tested" : "");
		p += snprintf(p, sizeof(line) - (size_t)(p - line), "%sspw%u %s", p == line ? "" : ", ",
			      nn, (A.spw_synced >> nn) & 1 ? "synced" :
			      A.o.spw_nn_mask && !((A.o.spw_nn_mask >> nn) & 1) ? "not tested" : "down");
	}
	a_summary("%s", line[0] ? line : "no SPW NN present");
}

/* ---- the packet flow shared by both loop steps ---- */

struct flow {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint32_t nn, len;
	uint32_t tag;		/* attribute_id of this run's packets */
	uint32_t got;		/* distinct test packets received, good or bad */
	uint32_t bad;		/* CRC, length, pattern or footer wrong */
	uint32_t order;		/* packet id not the one expected next */
	uint32_t dup;		/* a packet id received twice */
	uint32_t stale;		/* clean packets of an earlier run */
	uint32_t foreign;	/* other traffic to our node (not counted as test packets) */
	uint32_t next_id;
	uint64_t last_rx_ns;
	uint8_t seen[IDS / 8];	/* ids received, a sliding half-window */
};

static uint32_t seed_of(uint16_t id)
{
	return 0x9E3779B9u ^ ((uint32_t)id * 0x10001u);
}

static void flow_cb(void *user, const struct ramon_spw_rx *rx)
{
	struct flow *f = user;
	const struct ramon_spw_hdr *h = rx->hdr;
	int hdr_ok = h && !(rx->flags & (RAMON_SPW_RX_HDR_CRC_BAD | RAMON_SPW_RX_TRUNCATED));
	const char *why = NULL;
	uint32_t n_bad = 0, len;

	pthread_mutex_lock(&f->lock);
	len = f->len;
	/* a clean header from someone else is other traffic, not a test packet */
	if (hdr_ok && (h->app_type != A.o.spw_app || h->src != A.o.spw_node ||
		       (h->attribute_id & 0xFF000000u) != TAG_BASE)) {
		f->foreign++;
		goto out;
	}
	if (hdr_ok && h->attribute_id != f->tag) {
		f->stale++;
		goto out;
	}
	f->last_rx_ns = tc_now_ns();
	if (hdr_ok) {
		uint16_t id = h->packet_id;
		int16_t d = (int16_t)(uint16_t)(id - f->next_id);	/* < 0: late or repeated */

		if (f->seen[id / 8] & (1u << (id % 8))) {
			f->dup++;
			goto out;
		}
		f->seen[id / 8] |= (uint8_t)(1u << (id % 8));
		id = (uint16_t)(id + IDS / 2);				/* forget the far half */
		f->seen[id / 8] &= (uint8_t)~(1u << (id % 8));
		if (d)
			f->order++;
		if (d >= 0)
			f->next_id = (uint16_t)(h->packet_id + 1);
	} else {
		f->next_id = (uint16_t)(f->next_id + 1);
	}
	if (!hdr_ok)
		why = rx->flags & RAMON_SPW_RX_TRUNCATED ? "truncated" : "header CRC";
	else if (rx->flags & RAMON_SPW_RX_PAYLOAD_CRC_BAD)
		why = "payload CRC";
	else if (rx->payload_len != len)
		why = "payload length";
	else if (rx->flags & RAMON_SPW_RX_FOOTER_ERR)
		why = "footer error flags";
	else if (tc_verify(rx->payload, rx->payload_len, seed_of(h->packet_id)) >= 0)
		why = "payload pattern";
	f->got++;
	if (why)
		n_bad = ++f->bad;
	pthread_cond_broadcast(&f->cond);
out:
	pthread_mutex_unlock(&f->lock);
	if (why && n_bad <= MAX_REPORTED)
		tc_info("  bad packet (%s): %u bytes, id %d, payload %u (sent %u), flags 0x%x, footer err 0x%x",
			why, rx->raw_size, h ? h->packet_id : -1, rx->payload_len, len, rx->flags,
			rx->footer.error_flags);
}

/* waits until got >= n; 0, or -1 when nothing arrived for IDLE_MS */
static int flow_wait(struct flow *f, uint32_t n)
{
	struct timespec ts;
	int ret = 0;

	pthread_mutex_lock(&f->lock);
	while (f->got < n) {
		if (tc_now_ns() - f->last_rx_ns > (uint64_t)IDLE_MS * 1000000ull) {
			ret = -1;
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_nsec += 20 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&f->cond, &f->lock, &ts);
	}
	pthread_mutex_unlock(&f->lock);
	return ret;
}

struct flow_result {
	uint32_t sent, got, bad, order, dup, stale, foreign;
	uint64_t overrun;
	double secs, pps, mibps, link_mbps;
};

/* one payload size on one NN; the RX thread runs with flow_cb */
static int run_size(struct flow *f, uint32_t nn, uint32_t len, struct flow_result *r)
{
	struct ramon_get_stats g0, g1;
	struct ramon_spw_msg m;
	struct ramon_status st;
	struct ramon_iov iov;
	uint64_t t0, t_end = 0, t_last;
	uint32_t seq = 0, lost_wait = 0;
	uint8_t *buf;
	int ret = 0;

	memset(r, 0, sizeof(*r));
	memset(&m, 0, sizeof(m));
	buf = malloc(len ? len : 1);
	if (!buf) {
		tc_fail("out of memory");
		return -1;
	}
	pthread_mutex_lock(&f->lock);
	f->len = len;
	f->tag++;
	f->got = f->bad = f->order = f->dup = f->stale = f->foreign = f->next_id = 0;
	memset(f->seen, 0, sizeof(f->seen));
	f->last_rx_ns = tc_now_ns();
	m.attribute_id = f->tag;
	pthread_mutex_unlock(&f->lock);
	if (a_check(ramon_get_stats(A.c, 0, &g0, &st), "GET_STATS", &st)) {
		free(buf);
		return -1;
	}

	t0 = tc_now_ns();
	if (A.o.spw_seconds)
		t_end = t0 + (uint64_t)A.o.spw_seconds * 1000000000ull;
	m.dst = A.o.spw_node;
	m.app_type = A.o.spw_app;
	m.flags = RAMON_SPW_MSG_KEEP_ID;
	iov.base = buf;
	iov.len = len;
	while (A.o.spw_seconds ? tc_now_ns() < t_end : seq < A.o.spw_count) {
		/* keep at most spw_window packets in flight */
		if (seq >= A.o.spw_window && flow_wait(f, seq - A.o.spw_window + 1)) {
			lost_wait = 1;
			break;
		}
		m.packet_id = (uint16_t)seq;
		tc_fill(buf, len, seed_of(m.packet_id));
		ret = ramon_spw_send(A.c, nn, &m, &iov, 1, 0, &st);
		if (ret) {
			tc_fail_st("ramon_spw_send", &st);
			break;
		}
		seq++;
	}
	if (!lost_wait)
		flow_wait(f, seq);

	pthread_mutex_lock(&f->lock);
	r->sent = seq;
	r->got = f->got;
	r->bad = f->bad;
	r->order = f->order;
	r->dup = f->dup;
	r->stale = f->stale;
	r->foreign = f->foreign;
	t_last = f->got ? f->last_rx_ns : tc_now_ns();
	pthread_mutex_unlock(&f->lock);
	if (!a_check(ramon_get_stats(A.c, 0, &g1, &st), "GET_STATS", &st))
		r->overrun = g1.spw_overrun[nn] - g0.spw_overrun[nn];
	r->secs = (double)(t_last - t0) / 1e9;
	if (r->secs > 0) {
		r->pps = (r->got - r->bad) / r->secs;
		r->mibps = (double)(r->got - r->bad) * len / MIB / r->secs;
		/* header + payload + EOP per packet, 10 bits per character */
		r->link_mbps = (double)(r->got - r->bad) * (len + RAMON_SPW_HDR_BYTES + 1) *
			       SPW_CHAR_BITS / 1e6 / r->secs;
	}
	free(buf);
	return ret;
}

/* runs every size on NN nn and reports; appends "1B 12.3k pkt/s ..." to sum */
static void run_sizes(uint32_t nn, char *sum, size_t sum_len)
{
	struct flow f;
	struct flow_result r;
	struct ramon_status st;
	pthread_condattr_t ca;
	size_t used = strlen(sum);
	unsigned i;

	memset(&f, 0, sizeof(f));
	pthread_mutex_init(&f.lock, NULL);
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&f.cond, &ca);
	pthread_condattr_destroy(&ca);
	f.nn = nn;
	f.tag = TAG_BASE;

	if (a_check(ramon_spw_drain(A.c, nn, 20, NULL, &st), "drain", &st) ||
	    a_check(ramon_spw_rx_start(A.c, nn, flow_cb, &f, &st), "RX thread", &st))
		goto out;
	for (i = 0; i < A.o.n_spw_sizes; i++) {
		uint32_t len = A.o.spw_sizes[i];
		uint32_t lost;

		if (run_size(&f, nn, len, &r))
			break;
		lost = r.sent > r.got ? r.sent - r.got : 0;
		tc_info("spw%u %u-byte payload: sent %u, back %u, lost %u, bad %u, out of order %u, "
			"repeated %u%s; %.0f packets/s, %.2f MiB/s payload, %.1f Mbit/s on the link "
			"over %.2f s", nn, len, r.sent, r.got, lost, r.bad, r.order, r.dup,
			r.overrun ? ", DRIVER RX OVERRUN" : "", r.pps, r.mibps, r.link_mbps, r.secs);
		if (r.foreign)
			tc_info("  %u other packet(s) to node %u were ignored", r.foreign, A.o.spw_node);
		if (r.stale)
			tc_info("  %u late packet(s) of the previous size arrived during this one",
				r.stale);
		if (lost || r.bad || r.order || r.dup || r.overrun || r.got > r.sent || !r.sent)
			tc_fail("spw%u %u-byte payload: lost %u, bad %u, out of order %u, repeated %u, "
				"driver overruns %llu", nn, len, lost, r.bad, r.order, r.dup,
				(unsigned long long)r.overrun);
		if (used < sum_len)
			used += (size_t)snprintf(sum + used, sum_len - used,
						 "%s%uB %.0f pkt/s %.2f MiB/s %.1f Mbit/s",
						 used ? ", " : "", len, r.pps, r.mibps, r.link_mbps);
	}
	ramon_spw_rx_stop(A.c, nn);
out:
	pthread_cond_destroy(&f.cond);
	pthread_mutex_destroy(&f.lock);
}

static void describe_run(void)
{
	char sizes[128] = "";
	size_t used = 0;
	unsigned i;

	for (i = 0; i < A.o.n_spw_sizes && used < sizeof(sizes); i++)
		used += (size_t)snprintf(sizes + used, sizeof(sizes) - used, "%s%u", i ? ", " : "",
					 A.o.spw_sizes[i]);
	if (A.o.spw_seconds)
		tc_info("payload sizes %s bytes, %u s each, at most %u packets in flight", sizes,
			A.o.spw_seconds, A.o.spw_window);
	else
		tc_info("payload sizes %s bytes, %u packets each, at most %u in flight", sizes,
			A.o.spw_count, A.o.spw_window);
}

/* fpga = 1: FPGA loopback on; 0: off (the NN routes the packets back). Restores the setting. */
static void loop_step(int fpga)
{
	struct ramon_status st;
	char sum[160] = "";
	uint32_t use, nn;
	int was_on;

	use = a_spw_setup();
	if (use)
		describe_run();
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(use & (1u << nn)))
			continue;
		if (a_check(ramon_spw_loopback_get(A.c, nn, &was_on, &st), "loopback state", &st))
			continue;
		if (was_on != fpga &&
		    a_check(ramon_spw_loopback(A.c, nn, fpga, &st),
			    fpga ? "FPGA loopback on" : "FPGA loopback off and link re-sync", &st))
			continue;
		tc_info("spw%u: FPGA loopback %s, packets to node %u, application type %u", nn,
			fpga ? "on" : "off (the NN sends them back)", A.o.spw_node, A.o.spw_app);
		run_sizes(nn, sum, sizeof(sum));
		if (was_on != fpga &&
		    a_check(ramon_spw_loopback(A.c, nn, was_on, &st), "restoring the loopback setting",
			    &st))
			continue;
	}
	a_summary("%s", sum[0] ? sum : "nothing run");
}

static void step_spw_nn_loop(void)
{
	loop_step(0);
}

static void step_spw_fpga_loop(void)
{
	loop_step(1);
}

const struct step accept_spw[] = {
	{ "spw-link", step_spw_link, NEEDS_DEV, "spwinit",
	  "SPW links synced, IP versions", NULL },
	{ "spw-nn-loop", step_spw_nn_loop, NEEDS_DEV, "loopback 0 + spwsend",
	  "packets to our node routed back by the NN: intact, in order, MiB/s", NULL },
	{ "spw-fpga-loop", step_spw_fpga_loop, NEEDS_DEV, "loopback 1 + spwsend",
	  "the same traffic through the FPGA loopback", NULL },
	{ NULL, NULL, 0, NULL, NULL, NULL },
};

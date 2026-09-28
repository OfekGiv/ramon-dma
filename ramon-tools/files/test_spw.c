// SPDX-License-Identifier: MIT
/* ramon_test: SpaceWire tests (loopback write-read, NN round trips, cancel, raw AXI loop) */
#include "test.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOOP_IDS	128
#define LOOP_WAIT_MS	3000
#define BURST		32
#define TPUT_LEN	16384
#define AXI_LOOP_LEN	65536
#define AXI_LOOP_ITEMS	16
#define AXI_LOOP_RUNS	32

/* ---- loopback bookkeeping: the RX callback checks each packet by its id ---- */

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint32_t expect_len[LOOP_IDS];
	uint8_t state[LOOP_IDS];	/* 0 waiting, 1 intact, 2 wrong */
	uint32_t got;
	uint32_t order_errors;
	uint32_t foreign;
	int last_id;
} L = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, { 0 }, { 0 }, 0, 0, 0, -1 };

static uint32_t loop_seed(uint32_t id)
{
	return 0x10001u * (id + 1);
}

static void loop_cb(void *user, const struct ramon_spw_rx *rx)
{
	uint32_t id = rx->hdr ? rx->hdr->packet_id : LOOP_IDS;
	int ok;

	(void)user;
	pthread_mutex_lock(&L.lock);
	if (id >= LOOP_IDS) {
		L.foreign++;
		pthread_mutex_unlock(&L.lock);
		return;
	}
	ok = !rx->flags && rx->payload_len == L.expect_len[id] &&
	     tc_verify(rx->payload, rx->payload_len, loop_seed(id)) < 0;
	L.state[id] = ok ? 1 : 2;
	if ((int)id < L.last_id)
		L.order_errors++;
	L.last_id = (int)id;
	L.got++;
	pthread_cond_broadcast(&L.cond);
	pthread_mutex_unlock(&L.lock);
	if (!ok)
		tc_info("packet %u: flags 0x%x, payload %u bytes (sent %u), footer error 0x%x", id,
			rx->flags, rx->payload_len, L.expect_len[id], rx->footer.error_flags);
}

static void loop_reset(void)
{
	pthread_mutex_lock(&L.lock);
	memset(L.state, 0, sizeof(L.state));
	L.got = L.order_errors = L.foreign = 0;
	L.last_id = -1;
	pthread_mutex_unlock(&L.lock);
}

/* 0 once `got` reached n, -1 on timeout */
static int loop_wait(uint32_t n, uint32_t timeout_ms)
{
	uint64_t end = tc_now_ns() + (uint64_t)timeout_ms * 1000000ull;
	int ret = 0;

	pthread_mutex_lock(&L.lock);
	while (L.got < n) {
		struct timespec ts;
		uint64_t now = tc_now_ns();

		if (now >= end) {
			ret = -1;
			break;
		}
		/* the condvar uses CLOCK_REALTIME: wait in short slices */
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 20 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&L.cond, &L.lock, &ts);
	}
	pthread_mutex_unlock(&L.lock);
	return ret;
}

static int loop_send(uint32_t nn, uint32_t id, uint8_t *buf, uint32_t len)
{
	struct ramon_spw_msg m;
	struct ramon_status st;
	struct ramon_iov iov = { buf, len };
	struct ramon_spw_config cfg;

	ramon_spw_get_config(T.c, &cfg);
	pthread_mutex_lock(&L.lock);
	L.expect_len[id] = len;
	pthread_mutex_unlock(&L.lock);
	tc_fill(buf, len, loop_seed(id));
	memset(&m, 0, sizeof(m));
	m.dst = cfg.node_id;
	m.packet_id = (uint16_t)id;
	m.flags = RAMON_SPW_MSG_KEEP_ID;
	return t_check(ramon_spw_send(T.c, nn, &m, &iov, 1, 0, &st), "ramon_spw_send", &st);
}

/* loopback on (remembering the old setting), queue drained, RX thread with loop_cb */
static int loop_begin(uint32_t nn, int *was_on)
{
	struct ramon_status st;

	if (t_check(ramon_spw_loopback_get(T.c, nn, was_on, &st), "loopback state", &st) ||
	    t_check(ramon_spw_loopback(T.c, nn, 1, &st), "loopback on", &st) ||
	    t_check(ramon_spw_drain(T.c, nn, 20, NULL, &st), "drain", &st))
		return -1;
	loop_reset();
	return t_check(ramon_spw_rx_start(T.c, nn, loop_cb, NULL, &st), "RX thread", &st);
}

static void loop_end(uint32_t nn, int was_on)
{
	struct ramon_status st;

	ramon_spw_rx_stop(T.c, nn);
	if (!was_on)
		t_check(ramon_spw_loopback(T.c, nn, 0, &st), "loopback off and link re-sync", &st);
}

unsigned t_spw_loop_packets(uint32_t nn, unsigned n)
{
	uint8_t buf[256];
	unsigned i, ok = 0;
	int was_on;

	if (n > LOOP_IDS || loop_begin(nn, &was_on))
		return 0;
	for (i = 0; i < n; i++)
		if (loop_send(nn, i, buf, sizeof(buf)))
			break;
	if (loop_wait(i, LOOP_WAIT_MS))
		tc_fail("spw%u: %u of %u loopback packets came back", nn, L.got, i);
	for (i = 0; i < n; i++)
		ok += L.state[i] == 1;
	loop_end(nn, was_on);
	return ok;
}

/* ---- tests ---- */

static void test_spw_sync(void)
{
	struct ramon_status st;
	uint32_t nn, raw, tries, synced = 0;
	int r;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(T.spw_present & (1u << nn)))
			continue;
		r = ramon_spw_link_status(T.c, nn, &raw, &st);
		if (r < 0) {
			tc_fail_st("link status", &st);
			continue;
		}
		if (r == 0 && !ramon_spw_sync(T.c, nn, &tries, &st))
			r = 1;
		tc_info("spw%u: %s (0x28 = 0x%x)", nn, r ? "synced" : "not synced (no cable?)", raw);
		synced += r == 1;
	}
	if (!synced)
		tc_fail("no SPW link synced");
}

static void test_spw_loop(void)
{
	static const uint32_t sizes[] = { 0, 1, 7, 64, 1023, 4096, 16384 };
	uint32_t nn = (uint32_t)T.spw_nn, id = 0, i, first, max = T.o.spw_max;
	struct ramon_get_stats g0, g1;
	struct ramon_spw_config cfg;
	struct ramon_status st;
	uint64_t t0, ns;
	uint8_t *buf;
	int was_on;

	ramon_spw_get_config(T.c, &cfg);
	if (max > cfg.tx_slot_bytes - RAMON_SPW_HDR_BYTES)
		max = cfg.tx_slot_bytes - RAMON_SPW_HDR_BYTES;
	buf = malloc(max > TPUT_LEN ? max : TPUT_LEN);
	if (!buf) {
		tc_fail("out of memory");
		return;
	}
	if (loop_begin(nn, &was_on))
		goto out;
	tc_info("spw%u: loopback on, node %u sends to itself", nn, cfg.node_id);

	/* size sweep, one packet at a time */
	for (i = 0; i <= ARRAY_SIZE(sizes); i++, id++) {
		uint32_t len = i < ARRAY_SIZE(sizes) ? sizes[i] : max;

		if (i == ARRAY_SIZE(sizes) && max <= sizes[i - 1])
			break;
		if (loop_send(nn, id, buf, len))
			goto end;
		if (loop_wait(id + 1, LOOP_WAIT_MS)) {
			tc_fail("spw%u: the %u-byte packet did not come back", nn, len);
			goto end;
		}
		if (L.state[id] != 1)
			tc_fail("spw%u: the %u-byte packet came back wrong", nn, len);
	}
	tc_info("size sweep 0..%u bytes done", max);

	/* burst: back to back, in order, nothing lost */
	if (t_check(ramon_get_stats(T.c, 0, &g0, &st), "GET_STATS", &st))
		goto end;
	for (first = id, i = 0; i < BURST; i++, id++)
		if (loop_send(nn, id, buf, 1024))
			goto end;
	if (loop_wait(id, 2 * LOOP_WAIT_MS))
		tc_fail("spw%u: burst: %u of %u packets came back", nn, L.got - first, BURST);
	for (i = first; i < id; i++)
		if (L.state[i] != 1) {
			tc_fail("spw%u: burst packet %u missing or wrong", nn, i - first);
			break;
		}
	if (L.order_errors)
		tc_fail("spw%u: %u packet(s) out of order", nn, L.order_errors);
	if (!t_check(ramon_get_stats(T.c, 0, &g1, &st), "GET_STATS", &st) &&
	    g1.spw_overrun[nn] != g0.spw_overrun[nn])
		tc_fail("spw%u: RX size FIFO overran %llu time(s)", nn,
			(unsigned long long)(g1.spw_overrun[nn] - g0.spw_overrun[nn]));

	/* throughput */
	t0 = tc_now_ns();
	for (first = id, i = 0; i < BURST; i++, id++)
		if (loop_send(nn, id, buf, TPUT_LEN))
			goto end;
	if (loop_wait(id, 2 * LOOP_WAIT_MS))
		tc_fail("spw%u: throughput burst: %u of %u packets came back", nn, L.got - first, BURST);
	ns = tc_now_ns() - t0;
	tc_info("%u x %u bytes round trip: %.1f MiB/s", BURST, TPUT_LEN,
		tc_mibps((uint64_t)BURST * TPUT_LEN, ns));
	t_metric("roundtrip_MiBps", tc_mibps((uint64_t)BURST * TPUT_LEN, ns));
	if (L.foreign)
		tc_info("%u packet(s) with other ids were ignored", L.foreign);
end:
	loop_end(nn, was_on);
out:
	free(buf);
}

/* loopback must be off to talk to the NN; returns the old setting */
static int nn_mode(uint32_t nn, int *was_on)
{
	struct ramon_status st;

	if (t_check(ramon_spw_loopback_get(T.c, nn, was_on, &st), "loopback state", &st))
		return -1;
	return *was_on ? t_check(ramon_spw_loopback(T.c, nn, 0, &st), "loopback off", &st) : 0;
}

static void nn_restore(uint32_t nn, int was_on)
{
	struct ramon_status st;

	if (was_on)
		t_check(ramon_spw_loopback(T.c, nn, 1, &st), "loopback back on", &st);
}

static int printable(const char *s)
{
	for (; *s; s++)
		if (*s < 0x20 || *s > 0x7e)
			return 0;
	return 1;
}

static void count_cb(void *user, const struct ramon_spw_rx *rx)
{
	(void)rx;
	__atomic_add_fetch((unsigned *)user, 1, __ATOMIC_RELAXED);
}

static void test_spw_version(void)
{
	uint32_t nn = (uint32_t)T.spw_nn;
	struct ramon_nn_version v;
	struct ramon_status st;
	unsigned others = 0;
	int was_on;

	if (nn_mode(nn, &was_on))
		return;
	/* receive in this thread */
	if (!t_check(ramon_spw_nn_version(T.c, nn, &v, 0, &st), "version request", &st)) {
		tc_info("4KBL \"%s\", RSBL \"%s\"", v.kbl, v.rsbl);
		tc_info("Image \"%s\", FW \"%s\"", v.image, v.fw);
		if (!printable(v.kbl) || !printable(v.rsbl) || !printable(v.image) || !printable(v.fw))
			tc_fail("version strings hold unprintable characters");
	}
	/* again with the library RX thread handing the reply over */
	if (!t_check(ramon_spw_rx_start(T.c, nn, count_cb, &others, &st), "RX thread", &st)) {
		t_check(ramon_spw_nn_version(T.c, nn, &v, 0, &st), "version request (RX thread)", &st);
		ramon_spw_rx_stop(T.c, nn);
		if (others)
			tc_info("%u other packet(s) went to the callback", others);
	}
	nn_restore(nn, was_on);
}

static void test_spw_dps(void)
{
	uint32_t nn = (uint32_t)T.spw_nn;
	struct ramon_status st;
	int was_on;

	if (nn_mode(nn, &was_on))
		return;
	t_check(ramon_spw_nn_dps(T.c, nn, 0, &st), "DPS", &st);
	nn_restore(nn, was_on);
}

static void test_spw_cancel(void)
{
	struct ramon_spw_match want = { 0, 0x7F, 1, 0, 1, 0 };
	uint32_t nn = (uint32_t)T.spw_nn;
	struct ramon_spw_msg m;
	struct ramon_spw_rx rx;
	struct ramon_status st;
	uint8_t reply[256];
	unsigned count = 0;
	uint64_t t0;
	double ms;
	int ret, was_on;

	if (t_check(ramon_spw_rx_start(T.c, nn, count_cb, &count, &st), "RX thread", &st))
		return;
	ret = ramon_spw_recv(T.c, nn, reply, sizeof(reply), 10, &rx, &st);
	if (ret != -EBUSY || st.code != RAMON_EL_BUSY)
		tc_fail("receive while the RX thread runs: ret %d code %s", ret, ramon_err_str(st.code));
	tc_sleep_ms(100);
	t0 = tc_now_ns();
	ramon_spw_rx_stop(T.c, nn);
	ms = tc_ms_since(t0);
	tc_info("RX thread stopped in %.0f ms", ms);
	if (ms > 1500)
		tc_fail("stopping the RX thread took %.0f ms", ms);

	/*
	 * A request nobody answers. With loopback on, the packet to node 0x7F comes
	 * straight back, is not addressed to us, and is dropped: nothing reaches the NN.
	 */
	if (t_check(ramon_spw_loopback_get(T.c, nn, &was_on, &st), "loopback state", &st) ||
	    t_check(ramon_spw_loopback(T.c, nn, 1, &st), "loopback on", &st))
		return;
	memset(&m, 0, sizeof(m));
	m.dst = 0x7F;
	t0 = tc_now_ns();
	ret = ramon_spw_request(T.c, nn, &m, NULL, 0, &want, reply, sizeof(reply), 500, &rx, &st);
	ms = tc_ms_since(t0);
	if (ret != -ETIMEDOUT || st.code != RAMON_EL_SPW_REPLY_TIMEOUT)
		tc_fail("request to node 0x7F: ret %d code %s", ret, ramon_err_str(st.code));
	else if (ms < 450 || ms > 1000)
		tc_fail("500 ms request timeout took %.0f ms", ms);
	else
		tc_info("unanswered request timed out after %.0f ms", ms);
	if (!was_on)
		t_check(ramon_spw_loopback(T.c, nn, 0, &st), "loopback off and link re-sync", &st);
}

/* the old xsgdma: raw SG TX of one packet, WAIT_RX, RX of exactly that size, compare */
static void test_axi_loop(void)
{
	struct ramon_sg_item items[AXI_LOOP_ITEMS];
	uint32_t nn = (uint32_t)T.spw_nn, rxc, txc, len, i, ok = 0, piece;
	struct ramon_spw_config cfg;
	struct ramon_spw_wait_rx w;
	struct ramon_spw_msg m;
	struct ramon_status st;
	struct ramon_iov iov;
	ramon_buf tx, rx;
	uint8_t *payload;
	uint64_t t0, ns;
	int was_on;

	memset(&tx, 0, sizeof(tx));
	memset(&rx, 0, sizeof(rx));
	payload = malloc(AXI_LOOP_LEN);
	ramon_spw_get_config(T.c, &cfg);
	if (!payload || t_check(ramon_axi_find_pair(T.c, nn, &rxc, &txc, &st), "channel pair", &st) ||
	    t_check(ramon_buf_alloc(T.c, AXI_LOOP_LEN, 0, &tx, &st), "alloc", &st) ||
	    t_check(ramon_buf_alloc(T.c, AXI_LOOP_LEN + RAMON_SPW_FOOTER_BYTES, 0, &rx, &st), "alloc",
		    &st))
		goto out;
	tc_fill(payload, AXI_LOOP_LEN - RAMON_SPW_HDR_BYTES, 0xA5A50000u);
	memset(&m, 0, sizeof(m));
	m.dst = cfg.node_id;
	iov.base = payload;
	iov.len = AXI_LOOP_LEN - RAMON_SPW_HDR_BYTES;
	ramon_spw_build(cfg.node_id, &m, &iov, 1, tx.ptr, tx.size, &len);
	piece = len / AXI_LOOP_ITEMS;
	for (i = 0; i < AXI_LOOP_ITEMS; i++) {
		items[i].handle = tx.handle;
		items[i].pad = 0;
		items[i].offset = (uint64_t)i * piece;
		items[i].len = i + 1 < AXI_LOOP_ITEMS ? piece : len - (uint64_t)i * piece;
	}
	if (t_check(ramon_spw_loopback_get(T.c, nn, &was_on, &st), "loopback state", &st) ||
	    t_check(ramon_spw_loopback(T.c, nn, 1, &st), "loopback on", &st) ||
	    t_check(ramon_spw_drain(T.c, nn, 20, NULL, &st), "drain", &st))
		goto out;
	t0 = tc_now_ns();
	for (i = 0; i < AXI_LOOP_RUNS; i++) {
		if (t_check(ramon_axi_xfer(T.c, txc, items, AXI_LOOP_ITEMS, 0, NULL, &st), "AXI TX", &st))
			break;
		memset(&w, 0, sizeof(w));
		w.nn = nn;
		w.timeout_ms = LOOP_WAIT_MS;
		if (t_check(ramon_ioc_spw_wait_rx(T.c, &w), "SPW_WAIT_RX", &w.st))
			break;
		if (w.size > rx.size) {
			tc_fail("received %u bytes for %u sent; RX is out of step", w.size, len);
			break;
		}
		if (t_check(ramon_axi_xfer1(T.c, rxc, &rx, 0, w.size, 0, &st), "AXI RX", &st))
			break;
		if (w.size != len + RAMON_SPW_FOOTER_BYTES || memcmp(rx.ptr, tx.ptr, len)) {
			tc_fail("loop %u: %u bytes back for %u sent, or different data", i, w.size, len);
			break;
		}
		ok++;
	}
	ns = tc_now_ns() - t0;
	tc_info("%u of %u loops of %u bytes in %u SG items (chan %u -> %u): %.1f MiB/s, %llu prep retries",
		ok, AXI_LOOP_RUNS, len, AXI_LOOP_ITEMS, txc, rxc, tc_mibps((uint64_t)ok * len, ns),
		(unsigned long long)ramon_axi_retries(T.c));
	t_metric("MiBps", tc_mibps((uint64_t)ok * len, ns));
	if (!was_on)
		t_check(ramon_spw_loopback(T.c, nn, 0, &st), "loopback off and link re-sync", &st);
out:
	ramon_buf_free(&tx, NULL);
	ramon_buf_free(&rx, NULL);
	free(payload);
}

const struct test test_spw[] = {
	{ "spw-sync", test_spw_sync, RUN_DEFAULT, NEEDS_SPW, "at least one SPW link syncs" },
	{ "spw-loop", test_spw_loop, RUN_DEFAULT, NEEDS_SPW_LINK,
	  "loopback write-read: size sweep, burst order, round-trip MiB/s" },
	{ "spw-version", test_spw_version, RUN_DEFAULT, NEEDS_SPW_LINK,
	  "NN version request, with and without the RX thread" },
	{ "spw-dps", test_spw_dps, RUN_DEFAULT, NEEDS_SPW_LINK, "NN DPS request answered with 0x101" },
	{ "spw-cancel", test_spw_cancel, RUN_DEFAULT, NEEDS_SPW_LINK,
	  "RX thread stop, receiver exclusion, request timeout (in loopback)" },
	{ "axi-loop", test_axi_loop, RUN_DEFAULT, NEEDS_SPW_LINK,
	  "raw AXI SG TX -> loopback -> RX, compared, MiB/s (old xsgdma)" },
	{ NULL, NULL, 0, 0, NULL },
};

// SPDX-License-Identifier: MIT
/* ramon_cli: SpaceWire commands */
#include "cli.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FDIR_WORDS	73
#define FDIR_FILL	0xCAFECAFEu
#define FDIR_NODE	33
#define FDIR_APP	0x100
#define SPW_RECV_CAP	(RAMON_SPW_BUF_DEFAULT + 64)
#define LOOP_WAIT_MS	3000
#define AXILOOP_SEED	0xA5A50000u


/* ---- FDIR snapshot, as the old fdir_get_payload_values() ---- */

static void fdir_regs(struct cli *cl, const char *win, const uint32_t *offs, unsigned n,
		      uint32_t *w, unsigned *k)
{
	unsigned i;

	for (i = 0; i < n; i++)
		if (ramon_reg_read(cl->c, win, offs[i], &w[(*k)++], NULL))
			w[*k - 1] = FDIR_FILL;
}

static void fdir_fill(uint32_t *w, unsigned *k, unsigned n)
{
	while (n--)
		w[(*k)++] = FDIR_FILL;
}

static void fdir_snapshot(struct cli *cl, uint32_t w[FDIR_WORDS])
{
	static const uint32_t rstop[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18 };
	static const uint32_t spfi[] = { 0x40, 0x44, 0x48, 0xC8, 0xCC };
	static const uint32_t spw[] = { 0x00, 0x04, 0x08, 0x20, 0x24 };
	const struct ramon_get_info *gi = ramon_info(cl->c);
	struct ramon_sysmon s;
	unsigned k = 0, i;

	fdir_regs(cl, "rs_top", rstop, ARRAY_SIZE(rstop), w, &k);
	fdir_fill(w, &k, 3);			/* RFSoC status, reserved */
	fdir_regs(cl, "spfi0", spfi, ARRAY_SIZE(spfi), w, &k);
	fdir_fill(w, &k, 3);
	/* SPFI1 is not used on this board: placeholders unless the driver enables it */
	if (gi->spfi_mask & 2)
		fdir_regs(cl, "spfi1", spfi, ARRAY_SIZE(spfi), w, &k);
	else
		fdir_fill(w, &k, ARRAY_SIZE(spfi));
	fdir_fill(w, &k, 3);
	fdir_regs(cl, "spw0", spw, ARRAY_SIZE(spw), w, &k);
	fdir_fill(w, &k, ARRAY_SIZE(spw));	/* SPW NN1 placeholders, as the old code */
	if (ramon_sysmon_read(cl->c, &s, NULL))
		memset(&s, 0xCA, sizeof(s));
	for (i = 10; i < RAMON_SYSMON_RAILS; i++)
		w[k++] = s.raw[i];
	for (i = 0; i < 10; i++)
		w[k++] = s.raw[i];
}

static int fdir_send(struct cli *cl, uint32_t nn, uint32_t dst, uint32_t app)
{
	uint32_t w[FDIR_WORDS];
	struct ramon_iov iov = { w, sizeof(w) };
	struct ramon_spw_msg m;

	fdir_snapshot(cl, w);
	memset(&m, 0, sizeof(m));
	m.dst = (uint8_t)dst;
	m.app_type = (uint16_t)app;
	return ramon_spw_send(cl->c, nn, &m, &iov, 1, 0, &cl->st);
}

/* ---- receive callback (spwrx) ---- */

static void print_rx(const struct ramon_spw_rx *rx, int dump_payload)
{
	const struct ramon_spw_hdr *h = rx->hdr;

	if (!h) {
		printf("      spw%u: %u-byte packet without a header\n", rx->nn, rx->raw_size);
		return;
	}
	printf("      spw%u: %u bytes, dst %u src %u proto %u app 0x%x attr 0x%x id %u, payload %u, footer size %u err 0x%x count %u%s%s%s%s\n",
	       rx->nn, rx->raw_size, h->dst, h->src, h->protocol_id, h->app_type, h->attribute_id,
	       h->packet_id, rx->payload_len, rx->footer.pkt_size, rx->footer.error_flags,
	       rx->footer.packet_count,
	       rx->flags & RAMON_SPW_RX_HDR_CRC_BAD ? " HEADER-CRC-BAD" : "",
	       rx->flags & RAMON_SPW_RX_PAYLOAD_CRC_BAD ? " PAYLOAD-CRC-BAD" : "",
	       rx->flags & RAMON_SPW_RX_TRUNCATED ? " TRUNCATED" : "",
	       rx->flags & RAMON_SPW_RX_NOT_FOR_US ? " NOT-FOR-US" : "");
	if (dump_payload)
		tc_hexdump(rx->payload, rx->payload_len, 0, 64);
}

/* the old spwsend payload: 32-bit words 0, 1, 2, ... */
static int index_pattern_ok(const uint8_t *p, uint32_t len)
{
	uint32_t i, v;

	for (i = 0; i + 4 <= len; i += 4) {
		memcpy(&v, p + i, 4);
		if (v != i / 4)
			return 0;
	}
	return 1;
}

void cli_spw_rx_cb(void *user, const struct ramon_spw_rx *rx)
{
	struct cli *cl = user;
	int bad = (rx->flags & (RAMON_SPW_RX_BAD | RAMON_SPW_RX_FOOTER_ERR)) != 0;

	__atomic_add_fetch(&cl->rx_packets, 1, __ATOMIC_RELAXED);
	if (cl->dump_level == 1 && rx->hdr && !index_pattern_ok(rx->payload, rx->payload_len))
		bad = 1;
	if (bad)
		__atomic_add_fetch(&cl->rx_errors, 1, __ATOMIC_RELAXED);
	flockfile(stdout);
	if (cl->log_rx || cl->dump_level >= 2 || (cl->dump_level == 1 && bad))
		print_rx(rx, cl->dump_level >= 3);
	fflush(stdout);
	funlockfile(stdout);
	if (cl->fdir_auto && rx->hdr && rx->hdr->attribute_id == RAMON_SPW_FDIR_REQUEST_ATTR) {
		struct ramon_status st;
		struct cli tmp = *cl;

		printf("      FDIR requested on spw%u, sending the snapshot\n", rx->nn);
		if (fdir_send(&tmp, rx->nn, FDIR_NODE, FDIR_APP)) {
			st = tmp.st;
			tc_fail_st("FDIR reply", &st);
		}
	}
}

/* ---- commands ---- */

static int nn_or_all(struct cli *cl, int argc, char **argv, int i, uint32_t *first, uint32_t *last)
{
	uint32_t nn;
	int ret;

	*first = 0;
	*last = RAMON_NN_COUNT - 1;
	if (i >= argc)
		return 0;
	ret = cli_u32(cl, argv[i], "nn", &nn);
	if (!ret)
		ret = cli_nn(cl, nn);
	if (!ret)
		*first = *last = nn;
	return ret;
}

static int cmd_spwinit(struct cli *cl, int argc, char **argv)
{
	uint32_t nn, tries, synced = 0, present = 0;
	int ret;

	(void)argc;
	(void)argv;
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!ramon_spw_present(cl->c, nn))
			continue;
		present++;
		ret = ramon_spw_sync(cl->c, nn, &tries, &cl->st);
		if (!ret) {
			printf("      spw%u: link synced after %u reset(s)\n", nn, tries);
			synced++;
		} else if (cl->st.code == RAMON_EL_SPW_LINK_DOWN) {
			printf("      spw%u: not synced (expected on an NN without a cable)\n", nn);
		} else {
			return ret;
		}
	}
	printf("      our node %u, NN node 0x%x\n", cl->node, cl->target);
	if (present && !synced)
		return cli_err(cl, ENOLINK, "no SPW link synced");
	return 0;
}

static int cmd_spwterm(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	ramon_spw_fini(cl->c);
	return 0;
}

static int cmd_spwnode(struct cli *cl, int argc, char **argv)
{
	uint32_t v;
	int ret = cli_u32(cl, argv[1], "node", &v);

	(void)argc;
	if (!ret && v > 255)
		ret = cli_err(cl, EINVAL, "node ids are 0..255");
	if (ret)
		return ret;
	cl->node = (uint8_t)v;
	if (ramon_spw_is_inited(cl->c)) {
		ramon_spw_fini(cl->c);
		return cli_spw(cl);
	}
	return 0;
}

static int cmd_loopback(struct cli *cl, int argc, char **argv)
{
	uint32_t nn, en;
	int ret;

	(void)argc;
	ret = cli_u32(cl, argv[1], "nn", &nn);
	if (!ret)
		ret = cli_nn(cl, nn);
	if (!ret)
		ret = cli_u32(cl, argv[2], "enable", &en);
	if (!ret && en > 1)
		ret = cli_err(cl, EINVAL, "enable is 0 or 1");
	return ret ? ret : ramon_spw_loopback(cl->c, nn, (int)en, &cl->st);
}

static int cmd_spwsync(struct cli *cl, int argc, char **argv)
{
	uint32_t first, last, nn, tries;
	int ret = nn_or_all(cl, argc, argv, 1, &first, &last);

	for (nn = first; !ret && nn <= last; nn++) {
		if (!ramon_spw_present(cl->c, nn))
			continue;
		ret = ramon_spw_sync(cl->c, nn, &tries, &cl->st);
		if (!ret)
			printf("      spw%u: synced after %u reset(s)\n", nn, tries);
	}
	return ret;
}

static int cmd_spwstatus(struct cli *cl, int argc, char **argv)
{
	static const uint32_t regs[] = { RAMON_SPW_REG_TX_FSM, RAMON_SPW_REG_RX_FSM,
					 RAMON_SPW_REG_LINK, RAMON_SPW_REG_LOOPBACK };
	struct ramon_spw_stats s;
	struct ramon_get_stats g;
	uint32_t first, last, nn, v;
	char win[8];
	unsigned i;
	int ret = nn_or_all(cl, argc, argv, 1, &first, &last);

	if (!ret)
		ret = ramon_get_stats(cl->c, 0, &g, &cl->st);
	for (nn = first; !ret && nn <= last; nn++) {
		if (!ramon_spw_present(cl->c, nn))
			continue;
		snprintf(win, sizeof(win), "spw%u", nn);
		printf("      spw%u: driver rx %llu overrun %llu; regs", nn,
		       (unsigned long long)g.spw_rx[nn], (unsigned long long)g.spw_overrun[nn]);
		for (i = 0; i < ARRAY_SIZE(regs); i++)
			if (!ramon_reg_read(cl->c, win, regs[i], &v, NULL))
				printf(" 0x%02x=0x%x", regs[i], v);
		printf("; RX thread %s\n", ramon_spw_rx_running(cl->c, nn) ? "running" : "off");
		if (ramon_spw_is_inited(cl->c) && !ramon_spw_get_stats(cl->c, nn, &s, 0))
			printf("      spw%u: lib tx %llu (%llu bytes, %llu failed), rx %llu (%llu bytes), crc errors %llu, truncated %llu, not for us %llu, footer errors %llu, oversize %llu, lost %llu, dropped %llu, to requests %llu\n",
			       nn, (unsigned long long)s.tx_packets, (unsigned long long)s.tx_bytes,
			       (unsigned long long)s.tx_failures, (unsigned long long)s.rx_packets,
			       (unsigned long long)s.rx_bytes, (unsigned long long)s.rx_crc_errors,
			       (unsigned long long)s.rx_truncated, (unsigned long long)s.rx_not_for_us,
			       (unsigned long long)s.rx_footer_errors, (unsigned long long)s.rx_oversize,
			       (unsigned long long)s.rx_consume_failures,
			       (unsigned long long)s.rx_dropped, (unsigned long long)s.rx_to_request);
	}
	if (!ret)
		printf("      SPW statistics: rx packet(%llu) errors(%llu)\n",
		       (unsigned long long)__atomic_load_n(&cl->rx_packets, __ATOMIC_RELAXED),
		       (unsigned long long)__atomic_load_n(&cl->rx_errors, __ATOMIC_RELAXED));
	return ret;
}

static int cmd_spwreg(struct cli *cl, int argc, char **argv)
{
	uint32_t nn, off, val = 0;
	char win[8];
	int ret;

	ret = cli_u32(cl, argv[1], "nn", &nn);
	if (!ret)
		ret = cli_nn(cl, nn);
	if (!ret)
		ret = cli_u32(cl, argv[2], "offset", &off);
	if (!ret && argc > 3)
		ret = cli_u32(cl, argv[3], "value", &val);
	if (ret)
		return ret;
	snprintf(win, sizeof(win), "spw%u", nn);
	return cli_regio(cl, win, 0, 0, off, argc > 3, val);
}

static int cmd_spwrx(struct cli *cl, int argc, char **argv)
{
	uint32_t nn, on;
	int ret;

	(void)argc;
	ret = cli_u32(cl, argv[1], "nn", &nn);
	if (!ret)
		ret = cli_nn(cl, nn);
	if (!ret)
		ret = cli_u32(cl, argv[2], "on", &on);
	if (ret)
		return ret;
	if (!on)
		return ramon_spw_rx_stop(cl->c, nn);
	return ramon_spw_rx_start(cl->c, nn, cli_spw_rx_cb, cl, &cl->st);
}

static int set_level(struct cli *cl, const char *s, uint32_t max, int *field, const char *what)
{
	uint32_t v;
	int ret = cli_u32(cl, s, what, &v);

	if (!ret && v > max)
		ret = cli_err(cl, EINVAL, "%s is 0..%u", what, max);
	if (!ret)
		*field = (int)v;
	return ret;
}

static int cmd_spwdump(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	return set_level(cl, argv[1], 3, &cl->dump_level, "level");
}

static int cmd_log(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	return set_level(cl, argv[1], 1, &cl->log_rx, "log");
}

static int cmd_fdirauto(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	return set_level(cl, argv[1], 1, &cl->fdir_auto, "fdirauto");
}

struct send_job {
	struct cli *cl;
	uint32_t nn, count, delay_ms;
	struct ramon_spw_msg m;
	struct ramon_iov iov;
	uint32_t sent;
	int ret;
	struct ramon_status st;
};

static void *send_thread(void *arg)
{
	struct send_job *j = arg;
	uint32_t i;

	for (i = 0; i < j->count && !cli_intr(); i++) {
		j->ret = ramon_spw_send(j->cl->c, j->nn, &j->m, &j->iov, 1, 0, &j->st);
		if (j->ret)
			break;
		j->sent++;
		if (j->delay_ms)
			tc_sleep_ms(j->delay_ms);
	}
	return NULL;
}

static int cmd_spwsend(struct cli *cl, int argc, char **argv)
{
	uint32_t len, count, threads, delay, nn, dst, app, proto, attr, i, sent = 0;
	struct send_job *jobs;
	pthread_t *th;
	uint32_t *payload;
	uint64_t t0, ns;
	int ret;

	ret = cli_u32(cl, argv[1], "len", &len);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 2, 1, "count", &count);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 3, 1, "threads", &threads);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 4, 0, "delay_ms", &delay);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 5, 0, "nn", &nn);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 6, cl->node, "dst_node", &dst);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 7, 0, "dst_app", &app);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 8, 0, "proto", &proto);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 9, 0, "attr", &attr);
	if (!ret)
		ret = cli_nn(cl, nn);
	if (!ret && (!threads || threads > 64))
		ret = cli_err(cl, EINVAL, "threads is 1..64");
	if (ret)
		return ret;
	payload = calloc(1, len + 4);
	jobs = calloc(threads, sizeof(*jobs));
	th = calloc(threads, sizeof(*th));
	if (!payload || !jobs || !th) {
		ret = cli_err(cl, ENOMEM, "out of memory");
		goto out;
	}
	for (i = 0; i < (len + 3) / 4; i++)
		payload[i] = i;
	__atomic_store_n(&cl->rx_packets, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&cl->rx_errors, 0, __ATOMIC_RELAXED);
	t0 = tc_now_ns();
	for (i = 0; i < threads; i++) {
		jobs[i].cl = cl;
		jobs[i].nn = nn;
		jobs[i].count = count;
		jobs[i].delay_ms = delay;
		jobs[i].m.dst = (uint8_t)dst;
		jobs[i].m.app_type = (uint16_t)app;
		jobs[i].m.protocol_id = (uint8_t)proto;
		jobs[i].m.attribute_id = attr;
		jobs[i].iov.base = payload;
		jobs[i].iov.len = len;
		if (pthread_create(&th[i], NULL, send_thread, &jobs[i])) {
			ret = cli_err(cl, EAGAIN, "pthread_create failed");
			threads = i;
			break;
		}
	}
	for (i = 0; i < threads; i++) {
		pthread_join(th[i], NULL);
		sent += jobs[i].sent;
		if (jobs[i].ret && !ret) {
			ret = jobs[i].ret;
			cl->st = jobs[i].st;
		}
	}
	ns = tc_now_ns() - t0;
	printf("      sent %u packet(s) of %u payload bytes to node %u app 0x%x on spw%u, %.1f MiB/s\n",
	       sent, len, dst, app, nn, tc_mibps((uint64_t)sent * (len + RAMON_SPW_HDR_BYTES), ns));
out:
	free(payload);
	free(jobs);
	free(th);
	return ret;
}

static int cmd_spwrecv(struct cli *cl, int argc, char **argv)
{
	struct ramon_spw_rx rx;
	uint32_t nn, timeout;
	uint8_t *buf;
	int ret;

	ret = cli_u32(cl, argv[1], "nn", &nn);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 2, 3000, "timeout_ms", &timeout);
	if (ret)
		return ret;
	buf = malloc(SPW_RECV_CAP);
	if (!buf)
		return cli_err(cl, ENOMEM, "out of memory");
	ret = ramon_spw_recv(cl->c, nn, buf, SPW_RECV_CAP, timeout, &rx, &cl->st);
	if (!ret)
		print_rx(&rx, 1);
	free(buf);
	return ret;
}

static int cmd_spwdrain(struct cli *cl, int argc, char **argv)
{
	uint32_t first, last, nn, n;
	int ret = nn_or_all(cl, argc, argv, 1, &first, &last);

	for (nn = first; !ret && nn <= last; nn++) {
		if (!ramon_spw_present(cl->c, nn))
			continue;
		ret = ramon_spw_drain(cl->c, nn, 50, &n, &cl->st);
		if (!ret)
			printf("      spw%u: dropped %u packet(s)\n", nn, n);
	}
	return ret;
}

/* send to our own node with FPGA loopback on, receive, compare */
static int cmd_spwloop(struct cli *cl, int argc, char **argv)
{
	struct ramon_spw_msg m;
	struct ramon_spw_rx rx;
	struct ramon_iov iov;
	struct ramon_status st;
	uint32_t nn, len, count, i, ok = 0;
	uint8_t *tx = NULL, *rbuf = NULL;
	uint64_t t0, ns;
	int was_on = 0, ret;

	ret = cli_u32(cl, argv[1], "nn", &nn);
	if (!ret)
		ret = cli_nn(cl, nn);
	if (!ret)
		ret = cli_u32(cl, argv[2], "len", &len);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 3, 1, "count", &count);
	if (ret)
		return ret;
	if (ramon_spw_rx_running(cl->c, nn))
		return cli_err(cl, EBUSY, "stop the RX thread first (spwrx %u 0)", nn);
	tx = malloc(len + 1);
	rbuf = malloc(len + 256);
	if (!tx || !rbuf) {
		ret = cli_err(cl, ENOMEM, "out of memory");
		goto out;
	}
	ret = ramon_spw_loopback_get(cl->c, nn, &was_on, &cl->st);
	if (!ret)
		ret = ramon_spw_loopback(cl->c, nn, 1, &cl->st);
	if (!ret)
		ret = ramon_spw_drain(cl->c, nn, 20, NULL, &cl->st);
	t0 = tc_now_ns();
	for (i = 0; !ret && i < count && !cli_intr(); i++) {
		tc_fill(tx, len, i);
		memset(&m, 0, sizeof(m));
		m.dst = cl->node;
		m.packet_id = (uint16_t)i;
		m.flags = RAMON_SPW_MSG_KEEP_ID;
		iov.base = tx;
		iov.len = len;
		ret = ramon_spw_send(cl->c, nn, &m, &iov, 1, 0, &cl->st);
		if (!ret)
			ret = ramon_spw_recv(cl->c, nn, rbuf, len + 256, LOOP_WAIT_MS, &rx, &cl->st);
		if (ret)
			break;
		if (rx.flags || rx.hdr->packet_id != (uint16_t)i || rx.payload_len != len ||
		    tc_verify(rx.payload, len, i) >= 0) {
			print_rx(&rx, 0);
			ret = cli_err(cl, EIO, "packet %u came back wrong", i);
			break;
		}
		ok++;
	}
	ns = tc_now_ns() - t0;
	printf("      %u of %u packet(s) of %u bytes came back intact, %.1f MiB/s round trip\n", ok,
	       count, len, tc_mibps((uint64_t)ok * len, ns));
out:
	if (!was_on && ramon_spw_loopback(cl->c, nn, 0, &st))
		tc_fail_st("restoring loopback off", &st);
	free(tx);
	free(rbuf);
	return ret;
}

static int cmd_spwdps(struct cli *cl, int argc, char **argv)
{
	uint32_t nn;
	int ret = cli_opt_u32(cl, argc, argv, 1, 0, "nn", &nn);

	if (!ret)
		ret = ramon_spw_nn_dps(cl->c, nn, 0, &cl->st);
	if (!ret)
		printf("      spw%u: DPS acknowledged (attribute 0x%x)\n", nn, RAMON_SPW_ACK_ATTR);
	return ret;
}

static int cmd_nnswver(struct cli *cl, int argc, char **argv)
{
	struct ramon_nn_version v;
	uint32_t nn;
	int ret = cli_opt_u32(cl, argc, argv, 1, 0, "nn", &nn);

	if (!ret)
		ret = ramon_spw_nn_version(cl->c, nn, &v, 0, &cl->st);
	if (!ret)
		printf("      4KBL version:  %s\n      RSBL version:  %s\n      Image version: %s\n      FW version:    %s\n",
		       v.kbl, v.rsbl, v.image, v.fw);
	return ret;
}

static void swup_progress(void *user, uint32_t done, uint32_t total)
{
	struct cli *cl = user;

	if (!cl->script || done == total) {
		printf("\r      packet %u/%u", done, total);
		if (done == total)
			printf("\n");
		fflush(stdout);
	}
}

static int cmd_nnupdate(struct cli *cl, int argc, char **argv)
{
	uint32_t nn, type;
	uint8_t *img;
	size_t len;
	int ret;

	ret = cli_opt_u32(cl, argc, argv, 2, 0, "nn", &nn);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 3, 0, "sw_type", &type);
	if (ret)
		return ret;
	ret = tc_read_file(argv[1], &img, &len, 0xFFFFu * RAMON_SPW_SWUP_CHUNK);
	if (ret)
		return cli_err(cl, -ret, "%s: %s", argv[1], strerror(-ret));
	printf("      %s: %zu bytes, %zu packets\n", argv[1], len,
	       (len + RAMON_SPW_SWUP_CHUNK - 1) / RAMON_SPW_SWUP_CHUNK);
	ret = ramon_spw_nn_sw_update(cl->c, nn, (uint8_t)type, img, len, swup_progress, cl, &cl->st);
	if (ret)
		printf("\n");
	else
		printf("      NN SW updated successfully\n");
	free(img);
	return ret;
}

static int cmd_nnconf(struct cli *cl, int argc, char **argv)
{
	uint32_t nn;
	uint8_t *data;
	size_t len;
	int ret = cli_opt_u32(cl, argc, argv, 2, 0, "nn", &nn);

	if (ret)
		return ret;
	ret = tc_read_file(argv[1], &data, &len, RAMON_SPW_CONFIG_MAX);
	if (ret)
		return cli_err(cl, -ret, "%s: %s%s", argv[1], strerror(-ret),
			       ret == -EFBIG ? " (the limit is 4096 bytes)" : "");
	ret = ramon_spw_nn_config_push(cl->c, nn, data, (uint32_t)len, 0, &cl->st);
	if (!ret)
		printf("      NN configuration (%zu bytes) acknowledged\n", len);
	free(data);
	return ret;
}

static int cmd_fdir(struct cli *cl, int argc, char **argv)
{
	uint32_t nn, dst, app;
	int ret;

	(void)argc;
	ret = cli_u32(cl, argv[1], "nn", &nn);
	if (!ret)
		ret = cli_u32(cl, argv[2], "dst_node", &dst);
	if (!ret)
		ret = cli_u32(cl, argv[3], "dst_app", &app);
	if (!ret)
		ret = fdir_send(cl, nn, dst, app);
	if (!ret)
		printf("      FDIR snapshot (%d words) sent to node %u app 0x%x\n", FDIR_WORDS, dst, app);
	return ret;
}

/* the old xsgdma: raw TX of one SPW packet in SG pieces, WAIT_RX, RX, compare */
static int cmd_axiloop(struct cli *cl, int argc, char **argv)
{
	struct ramon_sg_item *items = NULL;
	struct ramon_spw_wait_rx w;
	struct ramon_spw_msg m;
	struct ramon_iov iov;
	struct ramon_status st;
	ramon_buf tx, rx;
	uint32_t nn, size, n_items, count, rxc, txc, len, i, ok = 0, piece;
	uint8_t *payload = NULL;
	uint64_t t0, ns;
	int was_on = 0, ret;

	memset(&tx, 0, sizeof(tx));
	memset(&rx, 0, sizeof(rx));
	ret = cli_u32(cl, argv[1], "nn", &nn);
	if (!ret)
		ret = cli_nn(cl, nn);
	if (!ret)
		ret = cli_u32(cl, argv[2], "size", &size);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 3, 1, "items", &n_items);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 4, 1, "count", &count);
	if (!ret && (size < RAMON_SPW_HDR_BYTES || !n_items || n_items > RAMON_AXI_MAX_ITEMS ||
		     n_items > size))
		ret = cli_err(cl, EINVAL, "size >= 40 bytes and items 1..min(size, %u)",
			      RAMON_AXI_MAX_ITEMS);
	if (ret)
		return ret;
	if (ramon_spw_rx_running(cl->c, nn))
		return cli_err(cl, EBUSY, "stop the RX thread first (spwrx %u 0)", nn);
	ret = ramon_axi_find_pair(cl->c, nn, &rxc, &txc, &cl->st);
	if (!ret)
		ret = ramon_buf_alloc(cl->c, size, 0, &tx, &cl->st);
	if (!ret)
		ret = ramon_buf_alloc(cl->c, size + RAMON_SPW_FOOTER_BYTES, 0, &rx, &cl->st);
	payload = malloc(size);
	items = calloc(n_items, sizeof(*items));
	if (!ret && (!payload || !items))
		ret = cli_err(cl, ENOMEM, "out of memory");
	if (ret)
		goto out;
	tc_fill(payload, size - RAMON_SPW_HDR_BYTES, AXILOOP_SEED);
	memset(&m, 0, sizeof(m));
	m.dst = cl->node;
	iov.base = payload;
	iov.len = size - RAMON_SPW_HDR_BYTES;
	ramon_spw_build(cl->node, &m, &iov, 1, tx.ptr, tx.size, &len);
	piece = len / n_items;
	for (i = 0; i < n_items; i++) {
		items[i].handle = tx.handle;
		items[i].offset = (uint64_t)i * piece;
		items[i].len = i + 1 < n_items ? piece : len - (uint64_t)i * piece;
	}
	ret = ramon_spw_loopback_get(cl->c, nn, &was_on, &cl->st);
	if (!ret)
		ret = ramon_spw_loopback(cl->c, nn, 1, &cl->st);
	t0 = tc_now_ns();
	for (i = 0; !ret && i < count && !cli_intr(); i++) {
		ret = ramon_axi_xfer(cl->c, txc, items, n_items, 0, NULL, &cl->st);
		memset(&w, 0, sizeof(w));
		w.nn = nn;
		w.timeout_ms = LOOP_WAIT_MS;
		if (!ret)
			ret = ramon_ioc_spw_wait_rx(cl->c, &w);
		if (ret) {
			if (w.st.code)
				cl->st = w.st;
			break;
		}
		if (w.size > rx.size) {
			ret = cli_err(cl, EIO, "received %u bytes, sent %u: the RX stream is out of step",
				      w.size, len);
			break;
		}
		ret = ramon_axi_xfer1(cl->c, rxc, &rx, 0, w.size, 0, &cl->st);
		if (ret)
			break;
		if (w.size != len + RAMON_SPW_FOOTER_BYTES || memcmp(rx.ptr, tx.ptr, len)) {
			ret = cli_err(cl, EIO, "iteration %u: %u bytes back for %u sent, or different data",
				      i, w.size, len);
			break;
		}
		ok++;
	}
	ns = tc_now_ns() - t0;
	printf("      %u of %u loop(s) of %u bytes in %u SG item(s) on channels %u -> %u, %.1f MiB/s, %llu prep retries\n",
	       ok, count, len, n_items, txc, rxc, tc_mibps((uint64_t)ok * len, ns),
	       (unsigned long long)ramon_axi_retries(cl->c));
	if (!was_on && ramon_spw_loopback(cl->c, nn, 0, &st))
		tc_fail_st("restoring loopback off", &st);
out:
	ramon_buf_free(&tx, NULL);
	ramon_buf_free(&rx, NULL);
	free(payload);
	free(items);
	return ret;
}

const struct cli_cmd cli_spw_cmds[] = {
	{ "spwinit", cmd_spwinit, 0, 0, CMD_NEED_SPW, "", "set up SPW and sync every present link" },
	{ "spwterm", cmd_spwterm, 0, 0, 0, "", "stop the RX threads and free the SPW buffers" },
	{ "spwnode", cmd_spwnode, 1, 1, 0, "<node>", "our node id (old spwappinit)" },
	{ "loopback", cmd_loopback, 2, 2, 0, "<nn> <0|1>", "FPGA SPW loopback" },
	{ "spwsync", cmd_spwsync, 0, 1, 0, "[nn]", "reset the link until it syncs" },
	{ "spwstatus", cmd_spwstatus, 0, 1, 0, "[nn]", "SPW registers and counters" },
	{ "spwreg", cmd_spwreg, 2, 3, 0, "<nn> <off> [val]", "SPW register" },
	{ "spwrx", cmd_spwrx, 2, 2, CMD_NEED_SPW, "<nn> <0|1>",
	  "RX thread that prints/counts packets (see spwdump, log)" },
	{ "spwdump", cmd_spwdump, 1, 1, 0, "<0-3>",
	  "RX printing: 1 errors + spwsend pattern check, 2 headers, 3 payload" },
	{ "log", cmd_log, 1, 1, 0, "<0|1>", "RX: one line per packet" },
	{ "fdirauto", cmd_fdirauto, 1, 1, 0, "<0|1>",
	  "RX: answer attribute 0xA0 with the FDIR snapshot" },
	{ "spwsend", cmd_spwsend, 1, 9, CMD_NEED_SPW,
	  "<len> [count] [threads] [delay_ms] [nn] [dst_node] [dst_app] [proto] [attr]",
	  "send packets (payload words 0,1,2..; dst default: our node)" },
	{ "spwrecv", cmd_spwrecv, 1, 2, CMD_NEED_SPW, "<nn> [timeout_ms]", "receive one packet" },
	{ "spwdrain", cmd_spwdrain, 0, 1, CMD_NEED_SPW, "[nn]", "drop queued packets" },
	{ "spwloop", cmd_spwloop, 2, 3, CMD_NEED_SPW, "<nn> <len> [count]",
	  "loopback round trip, verified (replaces spwtest)" },
	{ "spwtest", cmd_spwloop, 2, 3, CMD_NEED_SPW | CMD_HIDDEN, "<nn> <len> [count]",
	  "old name of spwloop" },
	{ "spwdps", cmd_spwdps, 0, 1, CMD_NEED_SPW, "[nn]", "DPS request to the NN" },
	{ "nnswver", cmd_nnswver, 0, 1, CMD_NEED_SPW, "[nn]", "NN software versions" },
	{ "nnupdate", cmd_nnupdate, 1, 3, CMD_NEED_SPW | CMD_DESTRUCTIVE, "<file> [nn] [sw_type]",
	  "NN software update" },
	{ "nnconf", cmd_nnconf, 1, 2, CMD_NEED_SPW, "<file> [nn]", "send a JSON configuration" },
	{ "fdir", cmd_fdir, 3, 3, CMD_NEED_SPW, "<nn> <dst_node> <dst_app>", "send the FDIR snapshot" },
	{ "axiloop", cmd_axiloop, 2, 4, 0, "<nn> <size> [items] [count]",
	  "raw AXI loopback through SPW, with MiB/s (old xsgdma)" },
	{ "xsgdma", cmd_axiloop, 2, 4, CMD_HIDDEN, "<nn> <size> [items] [count]", "old name of axiloop" },
	{ NULL, NULL, 0, 0, 0, NULL, NULL },
};

// SPDX-License-Identifier: MIT
/* ramon_test: device, buffer, channel, register, ZDMA and statistics tests */
#include "test.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB		(1024u * 1024u)
#define CMA_SLACK_KB	2048
#define ZDMA_THREADS	4
#define ZDMA_THREAD_OPS	100

static void test_version(void)
{
	const struct ramon_get_info *gi = ramon_info(T.c);
	struct ramon_get_info now;
	struct ramon_status st;
	char want[48], sysfs[48], line[160];

	ramon_version_line(T.c, line, sizeof(line));
	tc_info("%s", line);
	if (gi->abi_version != RAMON_ABI_VERSION)
		tc_fail("driver abi %u, library abi %u", gi->abi_version, RAMON_ABI_VERSION);
	snprintf(want, sizeof(want), "%u.%u.%u", gi->drv_major, gi->drv_minor, gi->drv_patch);
	if (tc_read_line("/sys/module/ramon_dma/version", sysfs, sizeof(sysfs)))
		tc_info("no /sys/module/ramon_dma/version (driver built in?)");
	else if (strcmp(sysfs, want))
		tc_fail("sysfs version %s, GET_INFO %s", sysfs, want);
	if (!t_check(ramon_get_info(T.c, &now, &st), "GET_INFO", &st) &&
	    (now.drv_major != gi->drv_major || now.drv_minor != gi->drv_minor ||
	     now.drv_patch != gi->drv_patch || now.page_size != gi->page_size))
		tc_fail("GET_INFO changed since ramon_open");
	if (!gi->max_buf_bytes || !gi->n_regwin)
		tc_fail("GET_INFO: max_buf_bytes %llu, n_regwin %u",
			(unsigned long long)gi->max_buf_bytes, gi->n_regwin);
}

static void test_buffers(void)
{
	static const uint64_t sizes[] = { 4096, 65536, MIB, 16 * MIB, 1, 4096 + 904 };
	uint64_t page = ramon_page_size(T.c), max = ramon_max_buf_bytes(T.c), dma, want;
	long cma0 = tc_meminfo_kb("CmaFree"), cma1;
	struct ramon_status st;
	ramon_buf b, many[32];
	unsigned i;
	long bad;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		if (sizes[i] > max)
			continue;
		if (t_check(ramon_buf_alloc(T.c, sizes[i], 0, &b, &st), "ramon_buf_alloc", &st))
			return;
		want = (sizes[i] + page - 1) / page * page;
		if (b.size != want)
			tc_fail("%llu bytes: size %llu, expected %llu", (unsigned long long)sizes[i],
				(unsigned long long)b.size, (unsigned long long)want);
		if (!tc_all_zero(b.ptr, b.size))
			tc_fail("%llu-byte buffer is not zeroed", (unsigned long long)b.size);
		tc_fill(b.ptr, b.size, (uint32_t)i);
		bad = tc_verify(b.ptr, b.size, (uint32_t)i);
		if (bad >= 0)
			tc_fail("%llu-byte buffer reads back wrong at byte %ld",
				(unsigned long long)b.size, bad);
		if (!t_check(ramon_buf_dma_addr(&b, &dma, &st), "BUF_INFO", &st) &&
		    (!dma || dma % page))
			tc_fail("dma address 0x%llx", (unsigned long long)dma);
		t_check(ramon_buf_free(&b, &st), "ramon_buf_free", &st);
	}
	memset(many, 0, sizeof(many));
	for (i = 0; i < ARRAY_SIZE(many); i++)
		if (t_check(ramon_buf_alloc(T.c, 65536, 0, &many[i], &st), "alloc of 32 buffers", &st))
			break;
	for (i = 0; i < ARRAY_SIZE(many); i++)
		ramon_buf_free(&many[i], NULL);
	cma1 = tc_meminfo_kb("CmaFree");
	if (cma0 > 0 && cma1 >= 0) {
		tc_info("CmaFree %ld kB before, %ld kB after", cma0, cma1);
		if (cma0 - cma1 > CMA_SLACK_KB)
			tc_fail("%ld kB of CMA not given back", cma0 - cma1);
	}
}

static double bw(void *p, size_t n, int what)
{
	uint64_t t0 = tc_now_ns();
	int pass;

	for (pass = 0; pass < 4; pass++) {
		if (what == 0)
			memset(p, pass, n);
		else if (what == 1)
			tc_fill(p, n, (uint32_t)pass);
		else if (tc_verify(p, n, 3) >= 0)
			tc_fail("verify of the CPU bandwidth buffer failed");
	}
	return tc_mibps((uint64_t)n * 4, tc_now_ns() - t0);
}

/* informational: CPU access to coherent DMA buffers versus ordinary memory */
static void test_cpu_bw(void)
{
	const size_t n = 4 * MIB;
	struct ramon_status st;
	double d[3], m[3];
	ramon_buf b;
	void *p;
	int k;

	if (t_check(ramon_buf_alloc(T.c, n, 0, &b, &st), "ramon_buf_alloc", &st))
		return;
	p = malloc(n);
	if (!p) {
		tc_fail("out of memory");
		ramon_buf_free(&b, NULL);
		return;
	}
	for (k = 0; k < 3; k++) {
		d[k] = bw(b.ptr, n, k);
		m[k] = bw(p, n, k);
	}
	tc_info("DMA buffer: memset %.0f, fill %.0f, verify %.0f MiB/s", d[0], d[1], d[2]);
	tc_info("malloc:     memset %.0f, fill %.0f, verify %.0f MiB/s", m[0], m[1], m[2]);
	t_metric("dma_fill_MiBps", d[1]);
	t_metric("dma_verify_MiBps", d[2]);
	t_metric("heap_fill_MiBps", m[1]);
	free(p);
	ramon_buf_free(&b, NULL);
}

static void test_chan(void)
{
	const struct ramon_get_info *gi = ramon_info(T.c);
	struct ramon_spfi_config fc;
	struct ramon_chan_info ci;
	struct ramon_status st;
	uint32_t i, nn, rx, tx;

	for (i = 0; i < gi->n_axi_chan + gi->n_zdma_chan; i++) {
		if (t_check(ramon_chan_info(T.c, i, &ci, &st), "CHAN_INFO", &st))
			return;
		if ((i < gi->n_axi_chan) != (ci.type == RAMON_CHAN_AXI))
			tc_fail("channel %u: type %u out of order (AXI first, then ZDMA)", i, ci.type);
		if (ci.type == RAMON_CHAN_ZDMA && ci.dir != RAMON_DIR_MEMCPY)
			tc_fail("ZDMA channel %u is not MEMCPY", i);
	}
	tc_info("%u AXI and %u ZDMA channels", gi->n_axi_chan, gi->n_zdma_chan);
	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		if ((T.spw_present & (1u << nn)) &&
		    !t_check(ramon_axi_find_pair(T.c, nn, &rx, &tx, &st), "SPW channel pair", &st))
			tc_info("spw%u: RX channel %u, TX channel %u", nn, rx, tx);
	ramon_spfi_get_config(T.c, &fc);
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(T.spfi_present & (1u << nn)))
			continue;
		if (t_check(ramon_chan_info(T.c, fc.chan[nn], &ci, &st), "CHAN_INFO", &st))
			continue;
		if (ci.type != RAMON_CHAN_AXI || ci.dir != RAMON_DIR_MEM_TO_DEV)
			tc_fail("spfi%u write channel %u is not AXI MEM_TO_DEV", nn, fc.chan[nn]);
		else
			tc_info("spfi%u: write channel %u", nn, fc.chan[nn]);
	}
}

static void test_regwin(void)
{
	static const char *const fixed[RAMON_WIN_FIXED] = {
		"rs_top", "sysmon_ps", "sysmon_pl", "rtc", "ttc0", "ttc1", "ttc2", "ttc3",
		"spw0", "spw1", "spfi0", "spfi1",
	};
	struct ramon_regwin_info ri;
	struct ramon_status st;
	uint32_t i, n = ramon_info(T.c)->n_regwin;
	size_t len;

	if (n < RAMON_WIN_FIXED) {
		tc_fail("%u register windows, at least %u expected", n, RAMON_WIN_FIXED);
		return;
	}
	for (i = 0; i < n; i++) {
		if (t_check(ramon_regwin_info(T.c, i, &ri, &st), "REGWIN_INFO", &st))
			return;
		ri.name[RAMON_NAME_LEN - 1] = 0;
		len = strcspn(ri.name, "@");
		if (i < RAMON_WIN_FIXED &&
		    (strlen(fixed[i]) != len || strncmp(ri.name, fixed[i], len)))
			tc_fail("window %u is \"%s\", expected \"%s\"", i, ri.name, fixed[i]);
		if (i >= RAMON_WIN_SYSMON_PS && i < RAMON_WIN_SPW0 && !(ri.flags & RAMON_REGWIN_RO))
			tc_fail("window %u (%s) should be read-only", i, ri.name);
		if ((ri.flags & RAMON_REGWIN_ABSENT) && (ri.size || ri.phys))
			tc_fail("absent window %u has size 0x%x phys 0x%llx", i, ri.size,
				(unsigned long long)ri.phys);
		tc_info("%2u %-24s 0x%010llx size 0x%x%s%s", i, ri.name, (unsigned long long)ri.phys,
			ri.size, ri.flags & RAMON_REGWIN_RO ? " RO" : "",
			ri.flags & RAMON_REGWIN_ABSENT ? " absent" : "");
	}
}

static void test_reg_rstop(void)
{
	struct ramon_rstop_time tm;
	struct ramon_status st;
	uint32_t off, a, b, v;

	for (off = 0; off <= 8; off += 4) {
		if (t_check(ramon_reg_read(T.c, "rs_top", off, &a, &st), "rs_top by name", &st) ||
		    t_check(ramon_reg_read_idx(T.c, RAMON_WIN_RS_TOP, off, &b, &st), "rs_top by index",
			    &st))
			return;
		if (a != b)
			tc_fail("rs_top+0x%x: 0x%x by name, 0x%x by index", off, a, b);
	}
	{
		uint32_t ver[3];

		if (!t_check(ramon_rstop_version(T.c, ver, &st), "RS-TOP version", &st))
			tc_info("RS TOP version %u.%u.%u", ver[0], ver[1], ver[2]);
	}
	if (!t_check(ramon_reg_write(T.c, "rs_top", RAMON_RSTOP_TRST_SPI_EN, 1, &st), "rs_top+0x40 write",
		     &st) &&
	    !t_check(ramon_reg_read(T.c, "rs_top", RAMON_RSTOP_TRST_SPI_EN, &v, &st), "rs_top+0x40 read",
		     &st) && v != 1)
		tc_fail("rs_top+0x40 reads 0x%x after writing 1", v);
	if (!t_check(ramon_reg_read(T.c, "rs_top", RAMON_RSTOP_TIMESTAMP, &v, &st), "time stamp", &st)) {
		ramon_rstop_decode_time(v, &tm);
		tc_info("time stamp %02u/%02u/%02u %02u:%02u:%02u", tm.day, tm.month, tm.year, tm.hour,
			tm.min, tm.sec);
		if (!tm.month || tm.month > 12 || !tm.day || tm.sec > 59 || tm.min > 59 || tm.hour > 23)
			tc_info("note: the time stamp does not decode to a valid date (not a failure)");
	}
}

static void test_reg_sysmon(void)
{
	struct ramon_status st;
	struct ramon_sysmon s;

	if (t_check(ramon_sysmon_read(T.c, &s, &st), "SYSMON", &st))
		return;
	tc_info("PS %.1f C, PL %.1f C, VCCINT %.3f V, VCC_PSAUX %.3f V", s.ps_temp_c, s.pl_temp_c,
		s.volts[26], s.volts[2]);
	t_metric("ps_temp_C", s.ps_temp_c);
	t_metric("pl_temp_C", s.pl_temp_c);
	if (s.ps_temp_c < -40 || s.ps_temp_c > 125 || s.pl_temp_c < -40 || s.pl_temp_c > 125)
		tc_fail("temperature out of -40..125 C: PS %.1f PL %.1f", s.ps_temp_c, s.pl_temp_c);
}

static void test_reg_rtc_ttc(void)
{
	struct ramon_regwin_info ri;
	struct ramon_status st;
	uint32_t a, b, v, i;

	if (t_check(ramon_reg_read(T.c, "rtc", 0x10, &a, &st), "RTC", &st))
		return;
	tc_sleep_ms(1200);
	if (t_check(ramon_reg_read(T.c, "rtc", 0x10, &b, &st), "RTC", &st))
		return;
	tc_info("RTC %u -> %u over 1.2 s", a, b);
	if (b - a < 1 || b - a > 2)
		tc_fail("RTC advanced by %u s over 1.2 s", b - a);
	for (i = 0; i < RAMON_TTC_COUNT; i++) {
		if (t_check(ramon_regwin_info(T.c, RAMON_WIN_TTC0 + i, &ri, &st), "REGWIN_INFO", &st) ||
		    (ri.flags & RAMON_REGWIN_ABSENT))
			continue;
		if (!t_check(ramon_reg_read_idx(T.c, RAMON_WIN_TTC0 + i, 0x0C, &v, &st), "TTC", &st))
			tc_info("ttc%u counter control 1: 0x%x", i, v);
	}
}

/* version registers of the SPW and SPFI windows, by prefix name and by index */
static void test_reg_spw_spfi(void)
{
	static const uint32_t spw_offs[] = { 0x00, 0x04, 0x08 };
	static const uint32_t spfi_offs[] = { 0x40, 0x44, 0x48 };
	struct ramon_status st;
	char win[8];
	uint32_t nn, i, a, b;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(ramon_info(T.c)->spw_mask & (1u << nn)))
			continue;
		snprintf(win, sizeof(win), "spw%u", nn);
		for (i = 0; i < ARRAY_SIZE(spw_offs); i++)
			if (!t_check(ramon_reg_read(T.c, win, spw_offs[i], &a, &st), win, &st) &&
			    !t_check(ramon_reg_read_idx(T.c, RAMON_WIN_SPW0 + nn, spw_offs[i], &b, &st),
				     win, &st) && a != b)
				tc_fail("%s+0x%x: 0x%x by name, 0x%x by index", win, spw_offs[i], a, b);
		tc_info("%s: version 0x%x", win, a);
	}
	/* SPFI1 is not used: only the NNs the driver enables */
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(ramon_info(T.c)->spfi_mask & (1u << nn)))
			continue;
		snprintf(win, sizeof(win), "spfi%u", nn);
		for (i = 0; i < ARRAY_SIZE(spfi_offs); i++)
			if (!t_check(ramon_reg_read(T.c, win, spfi_offs[i], &a, &st), win, &st) &&
			    !t_check(ramon_reg_read_idx(T.c, RAMON_WIN_SPFI0 + nn, spfi_offs[i], &b, &st),
				     win, &st) && a != b)
				tc_fail("%s+0x%x: 0x%x by name, 0x%x by index", win, spfi_offs[i], a, b);
		tc_info("%s: IP version 0x%x", win, a);
	}
}

static void test_reg_ro(void)
{
	struct ramon_status st;
	char msg[256];
	int ret;

	ret = ramon_reg_write(T.c, "sysmon_ps", 0, 0, &st);
	ramon_strerror(&st, msg, sizeof(msg));
	if (ret != -EROFS || st.code != RAMON_E_REGWIN_READ_ONLY ||
	    !strstr(msg, "RAMON_E_REGWIN_READ_ONLY"))
		tc_fail("write to sysmon_ps: ret %d, \"%s\"", ret, msg);
	else
		tc_info("refused as expected: %s", msg);
}

struct zdma_job {
	unsigned id;
	int failed;
};

static void *zdma_thread(void *arg)
{
	struct zdma_job *j = arg;
	struct ramon_status st;
	ramon_buf src, dst;
	unsigned i;

	if (ramon_buf_alloc(T.c, 65536, 0, &src, &st) || ramon_buf_alloc(T.c, 65536, 0, &dst, &st)) {
		tc_fail_st("zdma thread alloc", &st);
		j->failed = 1;
		return NULL;
	}
	tc_fill(src.ptr, src.size, 0x7000 + j->id);
	for (i = 0; i < ZDMA_THREAD_OPS && !j->failed; i++)
		if (ramon_zdma_copy1(T.c, &src, 0, &dst, 0, src.size, 0, &st)) {
			tc_fail_st("zdma thread copy", &st);
			j->failed = 1;
		}
	if (!j->failed && tc_verify(dst.ptr, dst.size, 0x7000 + j->id) >= 0) {
		tc_fail("zdma thread %u: destination differs", j->id);
		j->failed = 1;
	}
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
	return NULL;
}

static void test_zdma(void)
{
	static const uint64_t sizes[] = { 4096, 65536, MIB };
	struct zdma_job jobs[ZDMA_THREADS];
	pthread_t th[ZDMA_THREADS];
	struct ramon_copy e[100];
	struct ramon_status st;
	ramon_buf src, dst;
	uint64_t off = 0, t0, ns;
	uint32_t done, i;

	if (t_check(ramon_buf_alloc(T.c, 4 * MIB, 0, &src, &st), "alloc", &st))
		return;
	if (t_check(ramon_buf_alloc(T.c, 4 * MIB, 0, &dst, &st), "alloc", &st)) {
		ramon_buf_free(&src, NULL);
		return;
	}
	tc_fill(src.ptr, src.size, 0x2D2D);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		memset(dst.ptr, 0, sizes[i]);
		if (!t_check(ramon_zdma_copy1(T.c, &src, 0, &dst, 0, sizes[i], 0, &st), "zdma copy", &st) &&
		    memcmp(src.ptr, dst.ptr, sizes[i]))
			tc_fail("%llu-byte copy differs", (unsigned long long)sizes[i]);
	}
	/* 100 entries of 64 * (i + 1) bytes, packed into dst */
	memset(dst.ptr, 0, dst.size);
	for (i = 0; i < ARRAY_SIZE(e); i++) {
		e[i].src_handle = src.handle;
		e[i].dst_handle = dst.handle;
		e[i].src_off = (uint64_t)i * 8192;
		e[i].dst_off = off;
		e[i].len = 64 * (i + 1);
		off += e[i].len;
	}
	if (!t_check(ramon_zdma_copy(T.c, e, ARRAY_SIZE(e), 0, &done, &st), "zdma list", &st)) {
		if (done != ARRAY_SIZE(e))
			tc_fail("zdma list: %u of %zu entries done", done, ARRAY_SIZE(e));
		for (i = 0; i < ARRAY_SIZE(e); i++)
			if (memcmp((uint8_t *)src.ptr + e[i].src_off, (uint8_t *)dst.ptr + e[i].dst_off,
				   e[i].len)) {
				tc_fail("zdma list entry %u differs", i);
				break;
			}
	}
	t0 = tc_now_ns();
	for (i = 0; i < 8; i++)
		if (t_check(ramon_zdma_copy1(T.c, &src, 0, &dst, 0, MIB, 0, &st), "zdma copy", &st))
			break;
	ns = tc_now_ns() - t0;
	tc_info("8 x 1 MiB: %.1f MiB/s", tc_mibps((uint64_t)i * MIB, ns));
	t_metric("MiBps", tc_mibps((uint64_t)i * MIB, ns));
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);

	for (i = 0; i < ZDMA_THREADS; i++) {
		jobs[i].id = i;
		jobs[i].failed = 0;
		if (pthread_create(&th[i], NULL, zdma_thread, &jobs[i])) {
			tc_fail("pthread_create");
			break;
		}
	}
	while (i--)
		pthread_join(th[i], NULL);
}

static void test_stats(void)
{
	struct ramon_get_stats g0, g1;
	struct ramon_status st;
	ramon_buf a, b;
	unsigned i, spw = 0;
	int nn = T.spw_nn;

	if (t_check(ramon_buf_alloc(T.c, 65536, 0, &a, &st), "alloc", &st))
		return;
	if (t_check(ramon_buf_alloc(T.c, 65536, 0, &b, &st), "alloc", &st))
		goto out_a;
	if (t_check(ramon_get_stats(T.c, 0, &g0, &st), "GET_STATS", &st))
		goto out;
	for (i = 0; i < 10; i++)
		if (t_check(ramon_zdma_copy1(T.c, &a, 0, &b, 0, a.size, 0, &st), "zdma", &st))
			goto out;
	if (nn >= 0 && !T.o.skip_spw)
		spw = t_spw_loop_packets((uint32_t)nn, 5);
	if (t_check(ramon_get_stats(T.c, 0, &g1, &st), "GET_STATS", &st))
		goto out;
	tc_info("deltas: zdma_ops %llu, axi_tx %llu, axi_rx %llu%s",
		(unsigned long long)(g1.zdma_ops - g0.zdma_ops),
		(unsigned long long)(g1.axi_tx - g0.axi_tx), (unsigned long long)(g1.axi_rx - g0.axi_rx),
		nn >= 0 ? " (after 5 SPW loopback packets)" : "");
	if (g1.zdma_ops - g0.zdma_ops < 10)
		tc_fail("zdma_ops grew by %llu for 10 copies",
			(unsigned long long)(g1.zdma_ops - g0.zdma_ops));
	if (spw && (g1.axi_tx - g0.axi_tx < spw || g1.axi_rx - g0.axi_rx < spw ||
		    g1.spw_rx[nn] - g0.spw_rx[nn] < spw))
		tc_fail("after %u SPW packets: axi_tx +%llu axi_rx +%llu spw_rx +%llu", spw,
			(unsigned long long)(g1.axi_tx - g0.axi_tx),
			(unsigned long long)(g1.axi_rx - g0.axi_rx),
			(unsigned long long)(g1.spw_rx[nn] - g0.spw_rx[nn]));
	/* reset zeroes the counters for every user of the device */
	if (!t_check(ramon_get_stats(T.c, 1, &g0, &st), "GET_STATS reset", &st) &&
	    !t_check(ramon_get_stats(T.c, 0, &g1, &st), "GET_STATS", &st) &&
	    (g1.zdma_ops || g1.axi_tx || g1.axi_rx))
		tc_fail("after reset: zdma_ops %llu axi_tx %llu axi_rx %llu",
			(unsigned long long)g1.zdma_ops, (unsigned long long)g1.axi_tx,
			(unsigned long long)g1.axi_rx);
out:
	ramon_buf_free(&b, NULL);
out_a:
	ramon_buf_free(&a, NULL);
}

const struct test test_core[] = {
	{ "version", test_version, RUN_DEFAULT, 0, "ABI, driver, sysfs and library versions" },
	{ "buffers", test_buffers, RUN_DEFAULT, 0, "alloc/map/zeroed/pattern/free, CMA returned" },
	{ "cpu-bw", test_cpu_bw, RUN_DEFAULT, 0, "CPU MiB/s on DMA buffers vs malloc (information)" },
	{ "chan", test_chan, RUN_DEFAULT, 0, "channel list, SPW pairs, SPFI write channels" },
	{ "regwin", test_regwin, RUN_DEFAULT, 0, "register windows: names, order, flags" },
	{ "reg-rstop", test_reg_rstop, RUN_DEFAULT, 0, "RS-TOP by name/index, 0x40 read-back, time stamp" },
	{ "reg-sysmon", test_reg_sysmon, RUN_DEFAULT, 0, "temperatures in range, 37 rails" },
	{ "reg-rtc-ttc", test_reg_rtc_ttc, RUN_DEFAULT, 0, "RTC advances, TTCs readable" },
	{ "reg-spw-spfi", test_reg_spw_spfi, RUN_DEFAULT, 0, "SPW/SPFI version registers by name and index" },
	{ "reg-ro", test_reg_ro, RUN_DEFAULT, 0, "a read-only window refuses writes" },
	{ "zdma", test_zdma, RUN_DEFAULT, NEEDS_ZDMA, "ZDMA copies, list, MiB/s, 4 threads" },
	{ "stats", test_stats, RUN_DEFAULT, NEEDS_ZDMA, "driver counters follow the traffic; reset" },
	{ NULL, NULL, 0, 0, NULL },
};

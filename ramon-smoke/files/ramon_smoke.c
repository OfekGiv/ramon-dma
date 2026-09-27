// SPDX-License-Identifier: GPL-2.0
/*
 * On-target smoke test for /dev/ramon_dma.
 *
 *   ramon_smoke --version             tool version and the ABI it was built for
 *   ramon_smoke [-d DEV]              default sequence (every test marked "default")
 *   ramon_smoke [-d DEV] info         GET_INFO, version check, concurrent GET_INFO   (default)
 *   ramon_smoke [-d DEV] buf          alloc/mmap/pattern/free, mapping outlives free  (default)
 *   ramon_smoke [-d DEV] churn        alloc/mmap/free from many threads on one fd    (default)
 *   ramon_smoke [-d DEV] kill         kill -9 a process holding a mapped buffer       (default)
 *   ramon_smoke [-d DEV] regwin       register windows: list, RS-TOP, SYSMON, RTC, TTC (default)
 *   ramon_smoke [-d DEV] chan         DMA channel list                                 (default)
 *   ramon_smoke [-d DEV] spw          SPW: wait/cancel, link sync, NN version round trip (default)
 *   ramon_smoke [-d DEV] zdma         ZDMA single, list and 8-thread copies             (default)
 *   ramon_smoke [-d DEV] spfi         SPFI table, stream round trip, concurrent w+r, alerts (default)
 *   ramon_smoke [-d DEV] stats        GET_STATS, reset                                  (default)
 *   ramon_smoke [-d DEV] --stress     everything concurrently for --minutes N (default 10)
 *   ramon_smoke spwdps [nn]           the old "spwdps": link sync, DPS, reply attribute 0x101
 *   ramon_smoke spwsend <nn> <dst> <proto> <app> <attr> [hexpayload]
 *                                     any SPW message; the reply is printed
 *   ramon_smoke spfiprep [nn] [--format]
 *                                     the old SPFI bring-up: 0xCC == 0x88, spwdps, INIT 0,
 *                                     FORMAT only with --format (erases all streams), table
 *   ramon_smoke spfidel <nn> <sid>    CLOSE (if open) + DELETE one stream, e.g. a leftover
 *   ramon_smoke reg <win> <off> [val] read (or write, then read back) one register, e.g.
 *                                     "reg spfi0 0xc8" for SPFI_WR_STATUS0 (word 50)
 *   ramon_smoke [-d DEV] unbind       unbind with an fd open and a buffer mapped, rebind (root)
 *   ramon_smoke [-d DEV] --errors     error-path sweep (includes a CMA exhaustion check)
 *
 * Options: -d DEV (default /dev/ramon_dma), --node N (our SPW node id, default 68,
 * as the old "spwappinit 0 +68"), --target N (NN node id for the version
 * request, default 0x41, as the old get_nn_version()), --spfi-chans A,B (AXI
 * write channel of NN0,NN1, default 4,4: channel 5 was removed), --spfi-init
 * (send INIT type 0 before the SPFI stream test), --minutes N (--stress).
 *
 * Built by ramon-smoke.bb against the header installed by ramon-dma.bb, or by hand:
 *   $CC -Wall -Wextra -O2 -I ramon-dma/files -o ramon_smoke ramon-smoke/files/ramon_smoke.c -lpthread
 * Exit status is 0 only if every check passed.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ramon_dma_uapi.h"

/* 0.<driver step>.<tool revision>; ramon-smoke.bb PV must match */
#define SMOKE_VERSION		"0.8.9"

#define INFO_THREADS		8
#define INFO_ITERATIONS		20000
#define CHURN_THREADS		8
#define CHURN_ITERATIONS	300
#define CHURN_MAX_SIZE		(256 * 1024)
#define CMA_TEST_SIZE		(16 * 1024 * 1024)
#define CMA_SLACK_KB		2048	/* other CMA users may move CmaFree a little */
#define CMA_SETTLE_MS		2000
#define EXHAUST_MAX_BUFS	64
#define BOGUS_HANDLE		424242
#define REG_THREADS		8
#define REG_ITERATIONS		5000
#define RSTOP_MAJOR		0x00
#define RSTOP_MINOR		0x04
#define RSTOP_MINOR_MINOR	0x08
#define RSTOP_TRST_SPI_EN	0x40
#define SYSMON_TEMP		0x000
#define SYSMON_TEMP_MIN_C	(-40.0)
#define SYSMON_TEMP_MAX_C	125.0
#define RTC_CUR_TIME		0x10
#define TTC_REG			0x0C
#define SPW_VERSION		0x00
#define SPFI_WR_VERSION		0x40	/* word 16 */
#define SPW_LINK_STATUS		0x28
#define SPW_LINK_MASK		0x07
#define SPW_LINK_SYNCED		0x05
#define SPW_LOOPBACK_REG	0x10
#define SPW_HDR_BYTES		40	/* L2 + L3 + L4 */
#define SPW_FOOTER_BYTES	16
#define SPW_WAIT_MS		3000
#define SPW_SHORT_WAIT_MS	200
#define SPW_RESET_REG	0x1C
#define CANCEL_AFTER_MS		300
#define ZDMA_LIST_N		100
#define ZDMA_LIST_BUF		(4 * 1024 * 1024)
#define ZDMA_SMALL		64
#define ZDMA_THREADS		8
#define ZDMA_THREAD_COPIES	100
#define ZDMA_THREAD_BYTES	(64 * 1024)
#define SYSFS_VERSION		"/sys/module/" RAMON_DEV_NAME "/version"
#define SYSFS_DRIVER		"/sys/bus/platform/drivers/" RAMON_DEV_NAME
#define SYSFS_MISC_DEVICE	"/sys/class/misc/" RAMON_DEV_NAME "/device"
#define REBIND_WAIT_MS		2000

static const char *dev_path = "/dev/" RAMON_DEV_NAME;
static int fd = -1;
static int failures;
static size_t page_size;
static unsigned int page_shift;
static unsigned int drv_step;	/* 0.<step>.0 of the loaded driver */
static unsigned char code_seen[256];	/* RAMON_E_* codes a check has produced */
static int targc;			/* arguments after the test name */
static char *targv[8];

static uint32_t targ_u32(int i, uint32_t def)
{
	return i < targc ? strtoul(targv[i], NULL, 0) : def;
}

static void seen(uint32_t code)
{
	code_seen[code & 0xff] = 1;
}

/* ---- reporting ---- */

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	flockfile(stdout);
	fputs("FAIL: ", stdout);
	vprintf(fmt, ap);
	putchar('\n');
	funlockfile(stdout);
	va_end(ap);
	__atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
}

static void pass(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	flockfile(stdout);
	fputs("ok:   ", stdout);
	vprintf(fmt, ap);
	putchar('\n');
	funlockfile(stdout);
	va_end(ap);
}

static void print_status(const char *what, int err, const struct ramon_status *st)
{
	printf("      %s: errno %d (%s), %s [%s], arg 0x%" PRIx64 " 0x%" PRIx64 ": \"%s\"\n",
	       what, err, strerror(err), ramon_err_name(st->code), ramon_err_desc(st->code),
	       (uint64_t)st->arg[0], (uint64_t)st->arg[1], st->msg);
}

/* ioctl that must succeed; on failure prints errno and the whole trailer */
static int must(unsigned long req, void *arg, struct ramon_status *st, const char *what)
{
	int err;

	if (ioctl(fd, req, arg) == 0) {
		if (st->code != RAMON_E_OK || st->err != 0)
			fail("%s: returned 0 but status code %u err %d", what, st->code, st->err);
		return 0;
	}
	err = errno;
	fail("%s", what);
	print_status(what, err, st);
	return -err;
}

/*
 * ioctl that must fail with errno want_errno and code want_code, with a
 * message that contains want_in_msg (the offending value).
 */
static void expect_fail(unsigned long req, void *arg, struct ramon_status *st, const char *what,
			int want_errno, uint32_t want_code, const char *want_in_msg)
{
	int err;

	if (ioctl(fd, req, arg) == 0) {
		fail("%s: succeeded, expected %s", what, ramon_err_name(want_code));
		return;
	}
	err = errno;
	if (err != want_errno || st->code != want_code || st->err != -want_errno ||
	    !st->msg[0] || (want_in_msg && !strstr(st->msg, want_in_msg))) {
		fail("%s: want errno %d %s, msg containing \"%s\"", what, want_errno,
		     ramon_err_name(want_code), want_in_msg ? want_in_msg : "");
		print_status("got", err, st);
		return;
	}
	seen(st->code);
	pass("%s -> %s \"%s\"", what, ramon_err_name(st->code), st->msg);
}

/* ioctl that must fail before reaching a handler (no trailer) */
static void expect_errno(unsigned long req, void *arg, const char *what, int want_errno)
{
	int err;

	if (ioctl(fd, req, arg) == 0) {
		fail("%s: succeeded, expected errno %d", what, want_errno);
		return;
	}
	err = errno;
	if (err != want_errno)
		fail("%s: errno %d (%s), expected %d (%s)", what, err, strerror(err),
		     want_errno, strerror(want_errno));
	else
		pass("%s -> errno %d (%s)", what, err, strerror(err));
}

/* ---- helpers ---- */

static int get_info(struct ramon_get_info *gi)
{
	memset(gi, 0, sizeof(*gi));
	return must(RAMON_IOC_GET_INFO, gi, &gi->st, "GET_INFO");
}

static int buf_alloc(uint64_t size, uint32_t *handle, uint64_t *actual)
{
	struct ramon_buf_alloc a;

	memset(&a, 0, sizeof(a));
	a.size = size;
	if (must(RAMON_IOC_BUF_ALLOC, &a, &a.st, "BUF_ALLOC"))
		return -1;
	*handle = a.handle;
	if (actual)
		*actual = a.actual_size;
	return 0;
}

static int buf_free(uint32_t handle)
{
	struct ramon_buf_free f;

	memset(&f, 0, sizeof(f));
	f.handle = handle;
	return must(RAMON_IOC_BUF_FREE, &f, &f.st, "BUF_FREE");
}

static void *buf_map(uint32_t handle, size_t len, int flags)
{
	return mmap(NULL, len, PROT_READ | PROT_WRITE, flags, fd, (off_t)handle << page_shift);
}

static size_t round_page(uint64_t size)
{
	return (size + page_size - 1) & ~(page_size - 1);
}

static void fill(uint32_t *p, size_t bytes, uint32_t seed)
{
	size_t i;

	for (i = 0; i < bytes / sizeof(*p); i++)
		p[i] = seed ^ (uint32_t)(i * 2654435761u);
}

/* index of the first bad word, or -1 */
static long verify(const uint32_t *p, size_t bytes, uint32_t seed)
{
	size_t i;

	for (i = 0; i < bytes / sizeof(*p); i++)
		if (p[i] != (seed ^ (uint32_t)(i * 2654435761u)))
			return (long)i;
	return -1;
}

static int all_zero(const uint8_t *p, size_t bytes)
{
	size_t i;

	for (i = 0; i < bytes; i++)
		if (p[i])
			return 0;
	return 1;
}

/* CmaFree / CmaTotal from /proc/meminfo in kB, -1 if absent */
static long meminfo_kb(const char *key)
{
	char line[128];
	size_t klen = strlen(key);
	long kb = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, klen) && line[klen] == ':') {
			kb = strtol(line + klen + 1, NULL, 10);
			break;
		}
	fclose(f);
	return kb;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* waits up to CMA_SETTLE_MS for CmaFree to come back to at least want_kb */
static long cma_wait_back(long want_kb)
{
	long now = meminfo_kb("CmaFree");
	int waited;

	for (waited = 0; now < want_kb && waited < CMA_SETTLE_MS; waited += 50) {
		sleep_ms(50);
		now = meminfo_kb("CmaFree");
	}
	return now;
}

/* ---- info ---- */

static void check_sysfs_version(const struct ramon_get_info *gi)
{
	char want[32], got[32] = "";
	FILE *f = fopen(SYSFS_VERSION, "r");

	snprintf(want, sizeof(want), "%u.%u.%u", gi->drv_major, gi->drv_minor, gi->drv_patch);
	if (!f) {
		fail("%s: %s", SYSFS_VERSION, strerror(errno));
		return;
	}
	if (!fgets(got, sizeof(got), f))
		got[0] = '\0';
	fclose(f);
	got[strcspn(got, "\n")] = '\0';
	if (strcmp(want, got))
		fail("MODULE_VERSION \"%s\" != GET_INFO \"%s\"", got, want);
	else
		pass("MODULE_VERSION == GET_INFO == %s", got);
}

static void *info_thread(void *unused)
{
	struct ramon_get_info gi;
	long bad = 0;
	int i;

	(void)unused;
	for (i = 0; i < INFO_ITERATIONS; i++) {
		/* garbage in the trailer must come back cleared */
		memset(&gi, 0xa5, sizeof(gi));
		if (ioctl(fd, RAMON_IOC_GET_INFO, &gi) || gi.abi_version != RAMON_ABI_VERSION ||
		    gi.st.code != RAMON_E_OK || gi.st.err != 0 || gi.st.msg[0])
			bad++;
	}
	return (void *)bad;
}

static void test_info(void)
{
	struct ramon_get_info gi;
	pthread_t th[INFO_THREADS];
	long bad = 0;
	void *ret;
	int i;

	if (get_info(&gi))
		return;
	printf("      abi %u, driver %u.%u.%u, axi %u, zdma %u, regwin %u, spw_mask 0x%x, spfi_mask 0x%x, page %u, max_buf %" PRIu64 " bytes\n",
	       gi.abi_version, gi.drv_major, gi.drv_minor, gi.drv_patch, gi.n_axi_chan,
	       gi.n_zdma_chan, gi.n_regwin, gi.spw_mask, gi.spfi_mask, gi.page_size,
	       (uint64_t)gi.max_buf_bytes);

	if (gi.abi_version != RAMON_ABI_VERSION)
		fail("driver abi %u, this tool was built for %u", gi.abi_version, RAMON_ABI_VERSION);
	else
		pass("abi_version %u", gi.abi_version);
	if (gi.page_size != page_size)
		fail("page_size %u != sysconf %zu", gi.page_size, page_size);
	if (!gi.max_buf_bytes)
		fail("max_buf_bytes is 0");
	check_sysfs_version(&gi);

	for (i = 0; i < INFO_THREADS; i++)
		if (pthread_create(&th[i], NULL, info_thread, NULL)) {
			fail("pthread_create");
			return;
		}
	for (i = 0; i < INFO_THREADS; i++) {
		pthread_join(th[i], &ret);
		bad += (long)ret;
	}
	if (bad)
		fail("concurrent GET_INFO: %ld bad results", bad);
	else
		pass("concurrent GET_INFO: %d threads x %d calls on one fd",
		     INFO_THREADS, INFO_ITERATIONS);
}

/* ---- buf ---- */

/* alloc, info, map, zero check, pattern, free, stale handle */
static void buf_roundtrip(uint64_t size)
{
	struct ramon_buf_info bi;
	uint64_t actual;
	uint32_t h;
	long bad;
	void *p;

	if (buf_alloc(size, &h, &actual))
		return;
	if (actual != round_page(size))
		fail("size %" PRIu64 ": actual_size %" PRIu64 ", want %zu", size, actual,
		     round_page(size));

	memset(&bi, 0, sizeof(bi));
	bi.handle = h;
	if (!must(RAMON_IOC_BUF_INFO, &bi, &bi.st, "BUF_INFO") &&
	    (bi.size != actual || !bi.dma_addr || bi.dma_addr & (page_size - 1)))
		fail("BUF_INFO handle %u: size %" PRIu64 " dma 0x%" PRIx64, h,
		     (uint64_t)bi.size, (uint64_t)bi.dma_addr);

	p = buf_map(h, actual, MAP_SHARED);
	if (p == MAP_FAILED) {
		fail("mmap handle %u len %" PRIu64 ": %s", h, actual, strerror(errno));
	} else {
		if (!all_zero(p, actual))
			fail("handle %u: fresh buffer is not zeroed", h);
		fill(p, actual, h);
		bad = verify(p, actual, h);
		if (bad >= 0)
			fail("handle %u: pattern mismatch at word %ld", h, bad);
		munmap(p, actual);
	}
	if (buf_free(h))
		return;

	memset(&bi, 0, sizeof(bi));
	bi.handle = h;
	if (ioctl(fd, RAMON_IOC_BUF_INFO, &bi) == 0 || bi.st.code != RAMON_E_NO_SUCH_HANDLE)
		fail("BUF_INFO on freed handle %u: code %u", h, bi.st.code);
	else
		pass("size %" PRIu64 " -> handle %u, %" PRIu64 " bytes at dma 0x%" PRIx64
		     ", zeroed, pattern ok, freed", size, h, actual, (uint64_t)bi.dma_addr);
}

/* BUF_FREE while mapped: the mapping keeps the memory; munmap releases it */
static void buf_map_outlives_free(void)
{
	long before = meminfo_kb("CmaFree"), mid, after;
	uint64_t actual;
	uint32_t h;
	void *p;

	if (buf_alloc(CMA_TEST_SIZE, &h, &actual))
		return;
	p = buf_map(h, actual, MAP_SHARED);
	if (p == MAP_FAILED) {
		fail("mmap handle %u: %s", h, strerror(errno));
		buf_free(h);
		return;
	}
	fill(p, actual, 0x5a5a0000u);
	if (buf_free(h)) {
		munmap(p, actual);
		return;
	}
	mid = meminfo_kb("CmaFree");
	fill(p, actual, 0x0f0f0000u);
	if (verify(p, actual, 0x0f0f0000u) >= 0)
		fail("mapping of freed handle %u is not usable", h);
	munmap(p, actual);

	if (before < 0 || meminfo_kb("CmaTotal") <= 0) {
		pass("mapping outlives BUF_FREE (no CMA in /proc/meminfo, accounting skipped)");
		return;
	}
	after = cma_wait_back(before - CMA_SLACK_KB);
	printf("      CmaFree kB: before %ld, freed-but-mapped %ld, after munmap %ld\n",
	       before, mid, after);
	if (before - mid < (long)(actual / 1024) - CMA_SLACK_KB)
		printf("      note: the %" PRIu64 " byte buffer did not come from CMA\n", actual);
	if (after < before - CMA_SLACK_KB)
		fail("CmaFree did not come back after munmap (%ld kB < %ld kB)", after, before);
	else
		pass("mapping outlives BUF_FREE; memory returned at munmap");
}

static void buf_partial_map(void)
{
	uint32_t h;
	void *p;

	if (buf_alloc(3 * page_size, &h, NULL))
		return;
	p = buf_map(h, page_size, MAP_SHARED);
	if (p == MAP_FAILED) {
		fail("mapping 1 of 3 pages of handle %u: %s", h, strerror(errno));
	} else {
		fill(p, page_size, 7);
		munmap(p, page_size);
		pass("mapping a prefix of a buffer");
	}
	buf_free(h);
}

static void test_buf(void)
{
	buf_roundtrip(1);
	buf_roundtrip(page_size);
	buf_roundtrip(page_size + 904);
	buf_roundtrip(1024 * 1024 + 1);
	buf_partial_map();
	buf_map_outlives_free();
}

/* ---- churn: alloc/map/free from many threads on one fd ---- */

static void *churn_thread(void *arg)
{
	unsigned int seed = (unsigned int)(uintptr_t)arg;
	long bad = 0;
	int i;

	for (i = 0; i < CHURN_ITERATIONS; i++) {
		uint64_t size = 1 + rand_r(&seed) % CHURN_MAX_SIZE, actual;
		uint32_t h, tag = (seed << 8) ^ (uint32_t)i;
		void *p;

		if (buf_alloc(size, &h, &actual)) {
			bad++;
			continue;
		}
		p = buf_map(h, actual, MAP_SHARED);
		if (p == MAP_FAILED) {
			bad++;
		} else {
			/* a fresh zeroed buffer proves no other thread got the same handle */
			if (!all_zero(p, actual))
				bad++;
			fill(p, actual, tag);
			if (verify(p, actual, tag) >= 0)
				bad++;
			munmap(p, actual);
		}
		if (buf_free(h))
			bad++;
	}
	return (void *)bad;
}

static void test_churn(void)
{
	pthread_t th[CHURN_THREADS];
	long bad = 0;
	void *ret;
	int i;

	for (i = 0; i < CHURN_THREADS; i++)
		if (pthread_create(&th[i], NULL, churn_thread, (void *)(uintptr_t)(i + 1))) {
			fail("pthread_create");
			return;
		}
	for (i = 0; i < CHURN_THREADS; i++) {
		pthread_join(th[i], &ret);
		bad += (long)ret;
	}
	if (bad)
		fail("churn: %ld bad iterations", bad);
	else
		pass("churn: %d threads x %d alloc/mmap/verify/free on one fd",
		     CHURN_THREADS, CHURN_ITERATIONS);
}

/* ---- kill: SIGKILL a process that holds a mapped buffer ---- */

static void kill_child(int ready_fd)
{
	uint32_t h;
	void *p;

	/* own fd, so the buffer belongs only to this process */
	fd = open(dev_path, O_RDWR);
	if (fd < 0 || buf_alloc(CMA_TEST_SIZE, &h, NULL))
		_exit(1);
	p = buf_map(h, CMA_TEST_SIZE, MAP_SHARED);
	if (p == MAP_FAILED)
		_exit(1);
	fill(p, CMA_TEST_SIZE, 1);
	if (write(ready_fd, "r", 1) != 1)
		_exit(1);
	for (;;)
		pause();
}

static void test_kill(void)
{
	long before = meminfo_kb("CmaFree"), held, after;
	int pipefd[2], status;
	char c;
	pid_t pid;

	if (pipe(pipefd)) {
		fail("pipe: %s", strerror(errno));
		return;
	}
	pid = fork();
	if (pid < 0) {
		fail("fork: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		close(pipefd[0]);
		kill_child(pipefd[1]);
	}
	close(pipefd[1]);
	if (read(pipefd[0], &c, 1) != 1) {
		fail("child did not get its buffer mapped");
		waitpid(pid, &status, 0);
		close(pipefd[0]);
		return;
	}
	close(pipefd[0]);
	held = meminfo_kb("CmaFree");
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
		fail("child did not die from SIGKILL (status 0x%x)", status);

	if (before < 0 || meminfo_kb("CmaTotal") <= 0) {
		pass("kill -9 with a mapped buffer (no CMA in /proc/meminfo, accounting skipped)");
		return;
	}
	after = cma_wait_back(before - CMA_SLACK_KB);
	printf("      CmaFree kB: before %ld, child holding %d MiB %ld, after kill %ld\n",
	       before, CMA_TEST_SIZE >> 20, held, after);
	if (after < before - CMA_SLACK_KB)
		fail("CmaFree did not come back after kill -9 (%ld kB < %ld kB)", after, before);
	else
		pass("kill -9 with a mapped buffer: memory returned");
}

/* ---- regwin: register windows ---- */

static const char *const fixed_win_names[RAMON_WIN_FIXED] = {
	"rs_top", "sysmon_ps", "sysmon_pl", "rtc", "ttc0", "ttc1", "ttc2", "ttc3",
	"spw0", "spw1", "spfi0", "spfi1",
};

static int win_is_ro(uint32_t i)
{
	return i >= RAMON_WIN_SYSMON_PS && i < RAMON_WIN_TTC0 + RAMON_TTC_COUNT;
}

/* 0 or errno; prints nothing, so it can probe for the end of the list */
static int regwin_info(uint32_t index, struct ramon_regwin_info *ri)
{
	memset(ri, 0, sizeof(*ri));
	ri->index = index;
	return ioctl(fd, RAMON_IOC_REGWIN_INFO, ri) ? errno : 0;
}

/* name == NULL selects by index */
static int reg_io(const char *name, uint32_t index, uint32_t op, uint32_t offset, uint32_t *value)
{
	struct ramon_reg_io r;
	char what[64];
	int ret;

	memset(&r, 0, sizeof(r));
	if (name)
		snprintf(r.name, sizeof(r.name), "%s", name);
	r.index = index;
	r.op = op;
	r.offset = offset;
	r.value = *value;
	snprintf(what, sizeof(what), "REG_IO %s %s[0x%x]", op ? "write" : "read",
		 name ? name : fixed_win_names[index < RAMON_WIN_FIXED ? index : 0], offset);
	ret = must(RAMON_IOC_REG_IO, &r, &r.st, what);
	*value = r.value;
	return ret;
}

static int reg_read(const char *name, uint32_t index, uint32_t offset, uint32_t *value)
{
	*value = 0;
	return reg_io(name, index, RAMON_REG_OP_READ, offset, value);
}

static int win_present(uint32_t index)
{
	struct ramon_regwin_info ri;

	return !regwin_info(index, &ri) && !(ri.flags & RAMON_REGWIN_ABSENT);
}

static void regwin_list(uint32_t n)
{
	struct ramon_regwin_info ri;
	size_t flen;
	uint32_t i;
	int err;

	for (i = 0; i < n; i++) {
		err = regwin_info(i, &ri);
		if (err) {
			fail("REGWIN_INFO %u of %u: %s", i, n, strerror(err));
			print_status("REGWIN_INFO", err, &ri.st);
			continue;
		}
		printf("      %2u %-24.32s phys 0x%09" PRIx64 " size 0x%05x%s%s\n", i, ri.name,
		       (uint64_t)ri.phys, ri.size, ri.flags & RAMON_REGWIN_RO ? " RO" : "",
		       ri.flags & RAMON_REGWIN_ABSENT ? " ABSENT" : "");
		if (ri.flags & RAMON_REGWIN_ABSENT && (ri.size || ri.phys))
			fail("window %u %s: absent but size 0x%x phys 0x%" PRIx64, i, ri.name,
			     ri.size, (uint64_t)ri.phys);
		if (i >= RAMON_WIN_FIXED)
			continue;
		flen = strlen(fixed_win_names[i]);
		if (strncmp(ri.name, fixed_win_names[i], flen) ||
		    (ri.name[flen] && ri.name[flen] != '@'))
			fail("window %u is \"%.32s\", expected \"%s\"", i, ri.name,
			     fixed_win_names[i]);
		if (!(ri.flags & RAMON_REGWIN_ABSENT) &&
		    !!(ri.flags & RAMON_REGWIN_RO) != win_is_ro(i))
			fail("window %u %s: RO flag %s", i, ri.name,
			     win_is_ro(i) ? "missing" : "unexpected");
	}
	pass("%u windows listed; fixed names, order and RO flags as in the UAPI", n);
}

static void regwin_rstop(void)
{
	uint32_t maj, min, mm, by_name, en = 1;

	if (!win_present(RAMON_WIN_RS_TOP)) {
		printf("      rs_top absent: skipped\n");
		return;
	}
	if (reg_read(NULL, RAMON_WIN_RS_TOP, RSTOP_MAJOR, &maj) ||
	    reg_read(NULL, RAMON_WIN_RS_TOP, RSTOP_MINOR, &min) ||
	    reg_read(NULL, RAMON_WIN_RS_TOP, RSTOP_MINOR_MINOR, &mm) ||
	    reg_read("rs_top", 0, RSTOP_MAJOR, &by_name))
		return;
	if (by_name != maj)
		fail("rs_top[0] by name 0x%x != by index 0x%x", by_name, maj);
	pass("RS-TOP version %u.%u.%u (0x%x 0x%x 0x%x); name and index agree",
	     maj, min, mm, maj, min, mm);
	/* the same write probe does: enable TRST and SPI_EN */
	if (!reg_io(NULL, RAMON_WIN_RS_TOP, RAMON_REG_OP_WRITE, RSTOP_TRST_SPI_EN, &en) &&
	    !reg_read(NULL, RAMON_WIN_RS_TOP, RSTOP_TRST_SPI_EN, &en))
		pass("RS-TOP 0x40 (TRST/SPI_EN) written 1, reads back 0x%x", en);
}

static void regwin_sysmon(void)
{
	static const uint32_t win[] = { RAMON_WIN_SYSMON_PS, RAMON_WIN_SYSMON_PL };
	uint32_t raw;
	double c;
	size_t i;

	for (i = 0; i < 2; i++) {
		if (!win_present(win[i])) {
			printf("      %s absent: skipped\n", fixed_win_names[win[i]]);
			continue;
		}
		if (reg_read(NULL, win[i], SYSMON_TEMP, &raw))
			continue;
		/* ZynqMP SYSMON temperature, same formula as the old app */
		c = raw / 65536.0 * 509.314 - 280.239;
		if (c < SYSMON_TEMP_MIN_C || c > SYSMON_TEMP_MAX_C)
			fail("%s temperature %.2f C (raw 0x%x) is implausible",
			     fixed_win_names[win[i]], c, raw);
		else
			pass("%s temperature %.2f C (raw 0x%x)", fixed_win_names[win[i]], c, raw);
	}
}

static void regwin_rtc_ttc(void)
{
	uint32_t t0, t1, v, i;

	if (win_present(RAMON_WIN_RTC) && !reg_read(NULL, RAMON_WIN_RTC, RTC_CUR_TIME, &t0)) {
		sleep_ms(1200);
		if (!reg_read(NULL, RAMON_WIN_RTC, RTC_CUR_TIME, &t1))
			printf("      rtc[0x10] %u then %u after 1.2 s%s\n", t0, t1,
			       t1 == t0 ? " (RTC not counting?)" : "");
	}
	for (i = 0; i < RAMON_TTC_COUNT; i++)
		if (win_present(RAMON_WIN_TTC0 + i) &&
		    !reg_read(NULL, RAMON_WIN_TTC0 + i, TTC_REG, &v))
			printf("      ttc%u[0x0c] 0x%08x\n", i, v);
}

/* the prefix before '@' finds the SPW/SPFI windows; print their version registers */
static void regwin_spw_spfi(void)
{
	static const char *const prefix[] = { "spw0", "spw1", "spfi0", "spfi1" };
	static const uint32_t reg[] = { SPW_VERSION, SPW_VERSION, SPFI_WR_VERSION, SPFI_WR_VERSION };
	uint32_t i, by_name, by_index;

	for (i = 0; i < 4; i++) {
		if (!win_present(RAMON_WIN_SPW0 + i))
			continue;
		if (reg_read(prefix[i], 0, reg[i], &by_name) ||
		    reg_read(NULL, RAMON_WIN_SPW0 + i, reg[i], &by_index))
			continue;
		if (by_name != by_index)
			fail("%s[0x%x]: by name 0x%x != by index 0x%x", prefix[i], reg[i],
			     by_name, by_index);
		else
			pass("%s[0x%x] = 0x%x (prefix lookup matches index)", prefix[i], reg[i],
			     by_name);
	}
}

static void *regwin_thread(void *arg)
{
	uint32_t want = (uint32_t)(uintptr_t)arg;
	long bad = 0;
	int i;

	for (i = 0; i < REG_ITERATIONS; i++) {
		struct ramon_reg_io r;

		memset(&r, 0, sizeof(r));
		if (i & 1)
			snprintf(r.name, sizeof(r.name), "rs_top");
		r.offset = RSTOP_MAJOR;
		if (ioctl(fd, RAMON_IOC_REG_IO, &r) || r.value != want)
			bad++;
		memset(&r, 0, sizeof(r));
		r.index = RAMON_WIN_SYSMON_PS;
		if (ioctl(fd, RAMON_IOC_REG_IO, &r))
			bad++;
	}
	return (void *)bad;
}

static void regwin_concurrent(void)
{
	pthread_t th[REG_THREADS];
	uint32_t maj;
	long bad = 0;
	void *ret;
	int i;

	if (!win_present(RAMON_WIN_RS_TOP) || !win_present(RAMON_WIN_SYSMON_PS) ||
	    reg_read(NULL, RAMON_WIN_RS_TOP, RSTOP_MAJOR, &maj)) {
		printf("      rs_top or sysmon_ps absent: concurrent reads skipped\n");
		return;
	}
	for (i = 0; i < REG_THREADS; i++)
		if (pthread_create(&th[i], NULL, regwin_thread, (void *)(uintptr_t)maj)) {
			fail("pthread_create");
			return;
		}
	for (i = 0; i < REG_THREADS; i++) {
		pthread_join(th[i], &ret);
		bad += (long)ret;
	}
	if (bad)
		fail("concurrent REG_IO: %ld bad results", bad);
	else
		pass("concurrent REG_IO: %d threads x %d rs_top + sysmon reads", REG_THREADS,
		     REG_ITERATIONS);
}

static void test_regwin(void)
{
	struct ramon_get_info gi;

	if (get_info(&gi))
		return;
	if (gi.n_regwin < RAMON_WIN_FIXED)
		fail("n_regwin %u < %u fixed windows", gi.n_regwin, RAMON_WIN_FIXED);
	regwin_list(gi.n_regwin);
	regwin_rstop();
	regwin_sysmon();
	regwin_rtc_ttc();
	regwin_spw_spfi();
	regwin_concurrent();
}

/* ---- shared DMA helpers ---- */

/* a buffer allocated and mapped on the global fd */
struct mbuf {
	uint32_t h;
	uint64_t size;
	uint8_t *p;
};

static int mbuf_new(struct mbuf *m, uint64_t size)
{
	void *p;

	if (buf_alloc(size, &m->h, &m->size))
		return -1;
	p = buf_map(m->h, m->size, MAP_SHARED);
	if (p == MAP_FAILED) {
		fail("mmap handle %u: %s", m->h, strerror(errno));
		buf_free(m->h);
		return -1;
	}
	m->p = p;
	return 0;
}

static void mbuf_del(struct mbuf *m)
{
	munmap(m->p, m->size);
	buf_free(m->h);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int chan_info(uint32_t index, struct ramon_chan_info *ci)
{
	memset(ci, 0, sizeof(*ci));
	ci->index = index;
	return ioctl(fd, RAMON_IOC_CHAN_INFO, ci) ? errno : 0;
}

static int axi_xfer(uint32_t chan, uint32_t h, uint64_t off, uint64_t len, uint32_t timeout_ms,
		    const char *what)
{
	struct ramon_sg_item it = { .handle = h, .offset = off, .len = len };
	struct ramon_axi_xfer x;
	int ret;

	memset(&x, 0, sizeof(x));
	x.chan = chan;
	x.timeout_ms = timeout_ms;
	x.n_items = 1;
	x.items = (uintptr_t)&it;
	ret = must(RAMON_IOC_AXI_XFER, &x, &x.st, what);
	if (!ret && x.bytes != len)
		fail("%s: bytes %" PRIu64 " != %" PRIu64, what, (uint64_t)x.bytes, len);
	return ret;
}

static int zdma_copy(const struct ramon_copy *c, uint32_t n, const char *what)
{
	struct ramon_zdma_copy z;
	int ret;

	memset(&z, 0, sizeof(z));
	z.n = n;
	z.entries = (uintptr_t)c;
	ret = must(RAMON_IOC_ZDMA_COPY, &z, &z.st, what);
	if (!ret && z.done != n)
		fail("%s: done %u != %u", what, z.done, n);
	return ret;
}

/* ---- chan ---- */

static void test_chan(void)
{
	static const char *const type[] = { "AXI", "ZDMA" };
	static const char *const dir[] = { "MEM_TO_DEV", "DEV_TO_MEM", "MEMCPY" };
	struct ramon_get_info gi;
	struct ramon_chan_info ci;
	uint32_t i, n;
	int err;

	if (get_info(&gi))
		return;
	n = gi.n_axi_chan + gi.n_zdma_chan;
	for (i = 0; i < n; i++) {
		err = chan_info(i, &ci);
		if (err) {
			fail("CHAN_INFO %u of %u: %s", i, n, strerror(err));
			print_status("CHAN_INFO", err, &ci.st);
			continue;
		}
		if (ci.type > RAMON_CHAN_ZDMA || ci.dir > RAMON_DIR_MEMCPY) {
			fail("chan %u: type %u dir %u out of range", i, ci.type, ci.dir);
			continue;
		}
		printf("      %2u %-4s %-10s device-id %2u phys %#" PRIx64 " %.32s\n", i,
		       type[ci.type], dir[ci.dir], ci.device_id, (uint64_t)ci.phys, ci.name);
		if ((i < gi.n_axi_chan) != (ci.type == RAMON_CHAN_AXI) ||
		    (ci.type == RAMON_CHAN_ZDMA) != (ci.dir == RAMON_DIR_MEMCPY))
			fail("chan %u: type %s dir %s in the wrong place", i, type[ci.type],
			     dir[ci.dir]);
	}
	pass("%u AXI + %u ZDMA channels listed", gi.n_axi_chan, gi.n_zdma_chan);
}

/* ---- spw: link sync, NN version request/reply, wait, cancel ---- */

/*
 * SPW packet header as built by the old spw_api (spw_api.h): L2 (8) + L3 (24)
 * + L4 (8) bytes, little endian, packed. The FPGA appends a 16-byte footer on RX.
 */
struct spw_hdr {
	uint8_t dst;
	uint8_t protocol_id;
	uint8_t l2_reserved1[2];
	uint8_t src;
	uint8_t l2_reserved2[3];
	uint16_t nack_id;
	uint8_t l3_reserved[6];
	uint32_t l3_len;		/* L3 + L4 + payload */
	uint32_t header_crc;		/* over L2 + L3 with both CRC fields 0 */
	uint32_t payload_crc;		/* over L4 + payload */
	uint16_t packet_id;
	uint16_t app_type;
	uint32_t attribute_id;
	uint8_t l4_reserved[4];
} __attribute__((packed));

_Static_assert(sizeof(struct spw_hdr) == SPW_HDR_BYTES, "SPW header is 40 bytes");

#define SPW_L2L3_BYTES		32
#define SPW_L3_BYTES		24
#define SPW_RX_MAX		0x100000	/* MAX_SPW_RX_MSG_SIZE of the old spw_api */
#define SPW_NODE_DEFAULT	68		/* old "spwappinit 0 +68": our own node id */
#define SPW_TARGET_DEFAULT	0x41		/* old get_nn_version() destination */
#define SPW_VERSION_PROTOCOL	9
#define SPW_VERSION_APP_TYPE	9
#define SPW_VERSION_ATTRIBUTE	0x98
#define SPW_SYNC_TRIES		1000		/* as the old spw_init() */
#define SPW_SYNC_SLEEP_MS	10
#define SPW_REPLY_WAIT_MS	3000

static uint32_t spw_node = SPW_NODE_DEFAULT;
static uint32_t spw_target = SPW_TARGET_DEFAULT;
static uint32_t crc_table[256];

/* the old rc_crc32sw() with initial value 0, byte by byte (standard CRC-32) */
static void crc_init(void)
{
	uint32_t i, r;
	int j;

	for (i = 0; i < 256; i++) {
		r = i;
		for (j = 0; j < 8; j++)
			r = (r & 1 ? 0 : 0xEDB88320u) ^ r >> 1;
		crc_table[i] = r ^ 0xFF000000u;
	}
}

static uint32_t crc_rc(const void *data, size_t n)
{
	const uint8_t *p = data;
	uint32_t crc = 0;
	size_t i;

	for (i = 0; i < n; i++)
		crc = crc_table[(uint8_t)crc ^ p[i]] ^ crc >> 8;
	return crc;
}

static uint32_t spw_header_crc(const struct spw_hdr *h)
{
	struct spw_hdr t = *h;

	t.header_crc = 0;
	t.payload_crc = 0;
	return crc_rc(&t, SPW_L2L3_BYTES);
}

struct spw_nn {
	uint32_t nn;
	uint32_t rx;		/* AXI channel ids, from CHAN_INFO */
	uint32_t tx;
	uint32_t saved_loopback;
	char win[8];		/* "spw0" */
	struct mbuf scratch;	/* RX landing area, SPW_RX_MAX bytes */
};

/* NN n uses AXI channels 2n and 2n+1, the S2MM one for RX (the old spw_api layout) */
static int spw_channels(struct spw_nn *s)
{
	struct ramon_chan_info a, b;

	if (chan_info(2 * s->nn, &a) || chan_info(2 * s->nn + 1, &b) ||
	    a.type != RAMON_CHAN_AXI || b.type != RAMON_CHAN_AXI || a.dir == b.dir) {
		fail("spw%u: AXI channels %u/%u are not an RX/TX pair", s->nn, 2 * s->nn,
		     2 * s->nn + 1);
		return -1;
	}
	s->rx = a.dir == RAMON_DIR_DEV_TO_MEM ? a.index : b.index;
	s->tx = a.dir == RAMON_DIR_DEV_TO_MEM ? b.index : a.index;
	printf("      spw%u: rx chan %u, tx chan %u\n", s->nn, s->rx, s->tx);
	return 0;
}

static int spw_wait(uint32_t nn, uint32_t timeout_ms, uint32_t *size, struct ramon_status *st)
{
	struct ramon_spw_wait_rx w;
	int err = 0;

	memset(&w, 0, sizeof(w));
	w.nn = nn;
	w.timeout_ms = timeout_ms;
	if (ioctl(fd, RAMON_IOC_SPW_WAIT_RX, &w))
		err = errno;
	*size = w.size;
	if (st)
		*st = w.st;
	return err;
}

static int spw_loopback(uint32_t nn, uint32_t enable)
{
	struct ramon_spw_loopback l;

	memset(&l, 0, sizeof(l));
	l.nn = nn;
	l.enable = enable;
	return must(RAMON_IOC_SPW_LOOPBACK, &l, &l.st, "SPW_LOOPBACK");
}

/* a popped size must always be followed by the RX DMA of that packet */
static int spw_consume(struct spw_nn *s, uint32_t size)
{
	if (size > s->scratch.size) {
		fail("spw%u: RX size %u > %" PRIu64 " byte scratch buffer", s->nn, size,
		     s->scratch.size);
		return -1;
	}
	return axi_xfer(s->rx, s->scratch.h, 0, size, SPW_WAIT_MS, "AXI_XFER spw rx");
}

/* collect packets nobody asked for */
static void spw_drain(struct spw_nn *s)
{
	uint32_t size;
	int n = 0;

	while (!spw_wait(s->nn, 50, &size, NULL) && !spw_consume(s, size))
		n++;
	if (n)
		printf("      spw%u: drained %d unsolicited packet(s)\n", s->nn, n);
}

/* the old spw_init(): reset the core until the link reports synced; 0 synced, 1 not, -1 error */
static int spw_sync(struct spw_nn *s)
{
	uint32_t v = 0, zero = 0;
	int tries;

	for (tries = 0; tries < SPW_SYNC_TRIES; tries++) {
		if (reg_read(s->win, 0, SPW_LINK_STATUS, &v))
			return -1;
		if ((v & SPW_LINK_MASK) == SPW_LINK_SYNCED) {
			pass("%s: link synced after %d core reset(s) (0x28 = 0x%x)", s->win, tries,
			     v);
			return 0;
		}
		/* the old code wrote 0 to FPGA_SPW_RESET here */
		if (reg_io(s->win, 0, RAMON_REG_OP_WRITE, SPW_RESET_REG, &zero))
			return -1;
		sleep_ms(SPW_SYNC_SLEEP_MS);
	}
	/* not an error by itself: only the NN that is cabled can sync */
	printf("      %s: link not synced after %d resets (0x28 = 0x%x); skipped\n", s->win,
	       SPW_SYNC_TRIES, v);
	return 1;
}

struct spw_rx_arg {
	struct spw_nn *s;
	uint32_t size;
	int ok;
};

/* RX side: wait for one packet, then move exactly its size */
static void *spw_rx_thread(void *arg)
{
	struct spw_rx_arg *a = arg;
	struct ramon_status st;
	int err;

	err = spw_wait(a->s->nn, SPW_REPLY_WAIT_MS, &a->size, &st);
	if (err) {
		fail("spw%u: no reply within %d ms", a->s->nn, SPW_REPLY_WAIT_MS);
		print_status("SPW_WAIT_RX", err, &st);
		return NULL;
	}
	a->ok = !spw_consume(a->s, a->size);
	return NULL;
}

/* version reply of get_nn_version(): four fixed-width text fields (old dmaapi_testv2.cpp) */
#define SPW_VER_FIELD		200
#define SPW_VER_FW_FIELD	20
#define SPW_VER_BYTES		(3 * SPW_VER_FIELD + SPW_VER_FW_FIELD)

static void spw_print_field(const char *name, const uint8_t *p, uint32_t len)
{
	uint32_t n, i;

	for (n = 0; n < len && p[n]; n++)
		;
	printf("      %-14s \"", name);
	for (i = 0; i < n; i++)
		putchar(p[i] >= 0x20 && p[i] < 0x7f ? p[i] : '.');
	printf("\"\n");
}

static void spw_print_versions(const uint8_t *p)
{
	spw_print_field("4KBL version:", p, SPW_VER_FIELD);
	spw_print_field("RSBL version:", p + SPW_VER_FIELD, SPW_VER_FIELD);
	spw_print_field("Image version:", p + 2 * SPW_VER_FIELD, SPW_VER_FIELD);
	spw_print_field("FW version:", p + 3 * SPW_VER_FIELD, SPW_VER_FW_FIELD);
}

static void spw_print_payload(const uint8_t *p, uint32_t len)
{
	uint32_t i;

	if (len >= SPW_VER_BYTES) {
		spw_print_versions(p);
		return;
	}
	printf("      payload %u bytes:", len);
	for (i = 0; i < len && i < 64; i++)
		printf(" %02x", p[i]);
	printf("%s\n      as text: \"", len > 64 ? " ..." : "");
	for (i = 0; i < len && i < 128; i++)
		putchar(p[i] >= 0x20 && p[i] < 0x7f ? p[i] : '.');
	printf("\"\n");
}

/* prints the reply in s->scratch; 0 if addressed to us with both CRCs right */
static int spw_check_reply(struct spw_nn *s, uint32_t size)
{
	const struct spw_hdr *h = (const struct spw_hdr *)s->scratch.p;
	int before = __atomic_load_n(&failures, __ATOMIC_RELAXED);
	const uint32_t *f;
	uint32_t payload, crc;

	if (size < SPW_HDR_BYTES + SPW_FOOTER_BYTES) {
		fail("spw%u: reply of %u bytes is shorter than header + footer", s->nn, size);
		return -1;
	}
	payload = size - SPW_HDR_BYTES - SPW_FOOTER_BYTES;
	f = (const uint32_t *)(s->scratch.p + SPW_HDR_BYTES + payload);
	printf("      reply: %u bytes, dst %u src 0x%x proto %u app 0x%x attr 0x%x l3_len %u, footer pkt_size %u err 0x%x count %u\n",
	       size, h->dst, h->src, h->protocol_id, h->app_type, h->attribute_id, h->l3_len,
	       f[0], f[1], f[2]);
	if (h->dst != spw_node)
		fail("spw%u: reply addressed to node %u, we are %u", s->nn, h->dst, spw_node);
	crc = spw_header_crc(h);
	if (crc != h->header_crc)
		fail("spw%u: reply header CRC 0x%08x, computed 0x%08x", s->nn, h->header_crc, crc);
	if (h->l3_len < SPW_L3_BYTES || h->l3_len - SPW_L3_BYTES > size - SPW_L2L3_BYTES) {
		fail("spw%u: reply l3_len %u does not fit %u bytes", s->nn, h->l3_len, size);
		return -1;
	}
	crc = crc_rc(s->scratch.p + SPW_L2L3_BYTES, h->l3_len - SPW_L3_BYTES);
	if (crc != h->payload_crc)
		fail("spw%u: reply payload CRC 0x%08x, computed 0x%08x", s->nn, h->payload_crc,
		     crc);
	spw_print_payload(s->scratch.p + SPW_HDR_BYTES, payload);
	return __atomic_load_n(&failures, __ATOMIC_RELAXED) != before ? -1 : 0;
}

/* one SPW message: header fields and payload, as the old spw_send() takes them */
struct spw_msg {
	uint32_t dst;
	uint32_t protocol_id;
	uint32_t app_type;
	uint32_t attribute_id;
	const uint8_t *payload;
	uint32_t len;
};

/* a built packet: buffer and its exact length (the buffer is page-rounded) */
struct spw_pkt {
	struct mbuf buf;
	uint32_t len;
};

#define SPW_DPS_APP_TYPE	1
#define SPW_DPS_ATTRIBUTE	0x01
#define SPW_DPS_REPLY_ATTR	0x101
#define SPW_MAX_PAYLOAD		4096

static struct spw_msg spw_version_msg(void)
{
	return (struct spw_msg){ spw_target, SPW_VERSION_PROTOCOL, SPW_VERSION_APP_TYPE,
				 SPW_VERSION_ATTRIBUTE, NULL, 0 };
}

static struct spw_msg spw_dps_msg(void)
{
	return (struct spw_msg){ spw_target, SPW_VERSION_PROTOCOL, SPW_DPS_APP_TYPE,
				 SPW_DPS_ATTRIBUTE, NULL, 0 };
}

/* header + payload + CRCs exactly as the old spw_send() builds them */
static int spw_build(struct spw_pkt *pkt, const struct spw_msg *m)
{
	struct spw_hdr *h;

	pkt->len = SPW_HDR_BYTES + m->len;
	if (mbuf_new(&pkt->buf, pkt->len))
		return -1;
	h = (struct spw_hdr *)pkt->buf.p;
	memset(h, 0, sizeof(*h));
	h->dst = m->dst;
	h->src = spw_node;
	h->protocol_id = m->protocol_id;
	h->app_type = m->app_type;
	h->attribute_id = m->attribute_id;
	h->l3_len = SPW_L3_BYTES + (SPW_HDR_BYTES - SPW_L2L3_BYTES) + m->len;
	if (m->len)
		memcpy(pkt->buf.p + SPW_HDR_BYTES, m->payload, m->len);
	h->header_crc = spw_header_crc(h);
	h->payload_crc = crc_rc(pkt->buf.p + SPW_L2L3_BYTES, h->l3_len - SPW_L3_BYTES);
	return 0;
}

/* request, then wait and read the reply in the same thread (quiet on success) */
static int spw_roundtrip_seq(struct spw_nn *s, struct spw_pkt *pkt)
{
	const struct spw_hdr *h = (const struct spw_hdr *)s->scratch.p;
	uint32_t size;

	if (axi_xfer(s->tx, pkt->buf.h, 0, pkt->len, SPW_WAIT_MS, "AXI_XFER spw tx"))
		return -1;
	if (spw_wait(s->nn, SPW_REPLY_WAIT_MS, &size, NULL)) {
		fail("spw%u: no reply within %d ms", s->nn, SPW_REPLY_WAIT_MS);
		return -1;
	}
	if (spw_consume(s, size))
		return -1;
	if (size < SPW_HDR_BYTES || h->dst != spw_node || spw_header_crc(h) != h->header_crc) {
		fail("spw%u: bad reply: %u bytes, dst %u", s->nn, size, h->dst);
		return -1;
	}
	return 0;
}

/*
 * Sends @m while an RX thread waits for the answer and moves exactly its
 * size into s->scratch. Returns 0 with the reply size in @size.
 */
static int spw_request(struct spw_nn *s, const struct spw_msg *m, const char *name,
		       uint32_t *size)
{
	struct spw_rx_arg a = { .s = s };
	struct spw_pkt pkt;
	pthread_t th;

	if (spw_build(&pkt, m))
		return -1;
	if (pthread_create(&th, NULL, spw_rx_thread, &a)) {
		fail("pthread_create");
		mbuf_del(&pkt.buf);
		return -1;
	}
	sleep_ms(10);	/* let the RX thread block first */
	if (!axi_xfer(s->tx, pkt.buf.h, 0, pkt.len, SPW_WAIT_MS, "AXI_XFER spw tx"))
		printf("      spw%u: %s sent: node %u -> 0x%x, proto %u app %u attr 0x%x, %u payload bytes\n",
		       s->nn, name, spw_node, m->dst, m->protocol_id, m->app_type, m->attribute_id,
		       m->len);
	pthread_join(th, NULL);
	mbuf_del(&pkt.buf);
	*size = a.size;
	return a.ok ? 0 : -1;
}

/* get_nn_version() of the old spw_api */
static void spw_version(struct spw_nn *s)
{
	struct spw_msg m = spw_version_msg();
	uint32_t size;

	if (spw_request(s, &m, "version request", &size))
		return;
	if (!spw_check_reply(s, size))
		pass("spw%u: version request/reply round trip (TX and RX threads)", s->nn);
}

/* the old "spwdps": answered with attribute 0x101 */
static void spw_dps(struct spw_nn *s)
{
	const struct spw_hdr *h = (const struct spw_hdr *)s->scratch.p;
	struct spw_msg m = spw_dps_msg();
	uint32_t size;

	if (spw_request(s, &m, "DPS", &size) || spw_check_reply(s, size))
		return;
	if (h->attribute_id != SPW_DPS_REPLY_ATTR)
		fail("spw%u: DPS answered with attribute 0x%x, expected 0x%x", s->nn,
		     h->attribute_id, SPW_DPS_REPLY_ATTR);
	else
		pass("spw%u: DPS acknowledged (attribute 0x%x)", s->nn, h->attribute_id);
}

static void spw_timeout_check(struct spw_nn *s)
{
	struct ramon_status st;
	long long t0 = now_ms();
	uint32_t size;
	int err;

	err = spw_wait(s->nn, SPW_SHORT_WAIT_MS, &size, &st);
	if (!err)
		spw_consume(s, size);
	if (err != ETIMEDOUT || st.code != RAMON_E_SPW_TIMEOUT)
		fail("spw%u: idle wait: errno %d code %s, expected ETIMEDOUT", s->nn, err,
		     ramon_err_name(st.code));
	else
		pass("spw%u: idle wait of %d ms timed out after %lld ms", s->nn,
		     SPW_SHORT_WAIT_MS, now_ms() - t0);
}

struct cancel_arg {
	uint32_t nn;
	int err;
	uint32_t code;
	long long ms;
};

static void *cancel_thread(void *arg)
{
	struct cancel_arg *c = arg;
	struct ramon_status st;
	long long t0 = now_ms();
	uint32_t size;

	c->err = spw_wait(c->nn, 10 * SPW_WAIT_MS, &size, &st);
	c->code = st.code;
	c->ms = now_ms() - t0;
	return NULL;
}

/* two waiters on one NN, one SPW_CANCEL releases both */
static void spw_cancel_check(uint32_t nn)
{
	struct cancel_arg c[2] = { { .nn = nn }, { .nn = nn } };
	struct ramon_spw_cancel sc;
	pthread_t th[2];
	int i;

	for (i = 0; i < 2; i++)
		if (pthread_create(&th[i], NULL, cancel_thread, &c[i])) {
			fail("pthread_create");
			return;
		}
	sleep_ms(CANCEL_AFTER_MS);
	memset(&sc, 0, sizeof(sc));
	sc.nn = nn;
	must(RAMON_IOC_SPW_CANCEL, &sc, &sc.st, "SPW_CANCEL");
	for (i = 0; i < 2; i++) {
		pthread_join(th[i], NULL);
		if (c[i].err != ECANCELED || c[i].code != RAMON_E_SPW_CANCELLED)
			fail("spw%u waiter %d: errno %d code %s, expected ECANCELED", nn, i,
			     c[i].err, ramon_err_name(c[i].code));
		else
			pass("spw%u waiter %d cancelled after %lld ms", nn, i, c[i].ms);
	}
}

static int spw_setup(struct spw_nn *s, uint32_t nn)
{
	memset(s, 0, sizeof(*s));
	s->nn = nn;
	snprintf(s->win, sizeof(s->win), "spw%u", nn);
	if (spw_channels(s) || reg_read(s->win, 0, SPW_LOOPBACK_REG, &s->saved_loopback))
		return -1;
	return mbuf_new(&s->scratch, SPW_RX_MAX);
}

/* returns 1 if the link synced and the version round trip was attempted */
static int spw_one(uint32_t nn)
{
	struct spw_nn s;
	int synced = 0;

	if (spw_setup(&s, nn))
		return 0;
	/* loopback isolates the RX side from NN traffic for the wait/cancel checks */
	if (!spw_loopback(nn, 1)) {
		spw_drain(&s);
		spw_timeout_check(&s);
		spw_cancel_check(nn);
	}
	/* then the old spwinit: loopback off (its default), link sync, talk to the NN */
	if (!spw_loopback(nn, 0) && !spw_sync(&s)) {
		synced = 1;
		spw_drain(&s);
		spw_version(&s);
		spw_drain(&s);
	}
	if (s.saved_loopback && !spw_loopback(nn, 1))
		printf("      spw%u: loopback restored to 1\n", nn);
	mbuf_del(&s.scratch);
	return synced;
}

static void test_spw(void)
{
	struct ramon_get_info gi;
	int present = 0, synced = 0;
	uint32_t nn;

	if (get_info(&gi))
		return;
	printf("      our node id %u, version requests go to node 0x%x (--node / --target)\n",
	       spw_node, spw_target);
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(gi.spw_mask & (1u << nn))) {
			printf("      spw%u absent: skipped\n", nn);
			continue;
		}
		present++;
		synced += spw_one(nn);
	}
	if (present && !synced)
		fail("no SPW link synced; the NN round trip was not tested");
}

/* ---- spfi: stream table, stream round trip, concurrent write + read, alerts ---- */

#define SPFI_PAGE		RAMON_SPFI_OFFSET_BYTES
#define SPFI_HDR_BYTES		64
#define SPFI_REC_BYTES		64
#define SPFI_STREAMS		198
#define SPFI_TABLE_BYTES	(SPFI_HDR_BYTES + SPFI_STREAMS * SPFI_REC_BYTES)
#define SPFI_ST_EXIST		0x01
#define SPFI_ST_OPEN		0x02
#define SPFI_STREAMS_USABLE	(SPFI_STREAMS - 5)	/* NN rule: never use the last 5 */
#define SPFI_MAX_OPEN		8			/* NN rule: at most 8 open at once */
#define SPFI_PAGES		8	/* per write */
#define SPFI_ROUNDS		3	/* writes to the second stream */
#define SPFI_ALERT_DRAIN	10

/* RX message table, as parsed by the old spfi_api.cpp */
struct spfi_table_hdr {
	uint32_t total_media_size;
	uint32_t total_allocated_size;
	uint32_t total_used_size;
	uint8_t dynamic_quality;
	uint8_t static_quality;
	uint8_t reserved[50];
} __attribute__((packed));

struct spfi_record {
	uint8_t status;			/* bit0 exist, bit1 open, bit2 cyclic */
	uint8_t reserved0;
	uint16_t src_net_addr;
	uint16_t write_prog_target;
	uint16_t last_host_rd;
	uint16_t last_host_wr;
	uint16_t reserved1;
	uint32_t size_thresh;
	uint32_t latest_readable_offset;
	uint32_t alloc_size;
	uint32_t write_time;
	uint32_t read_time;
	uint32_t latest_read_offs;
	uint32_t latest_write_offs;
	uint8_t reserved2[24];
} __attribute__((packed));

_Static_assert(sizeof(struct spfi_table_hdr) == SPFI_HDR_BYTES, "table header");
_Static_assert(sizeof(struct spfi_record) == SPFI_REC_BYTES, "table record");

/*
 * AXI write channel per SPFI NN. The old spfidrvinit used { 5, 4 }; channel 5
 * has since been removed from the FPGA and channel 4 is the SPFI write path.
 */
static uint32_t spfi_chan[RAMON_NN_COUNT] = { 4, 4 };
static int spfi_init_first;
static int spfi_format;

#define SPFI_LINK_STATUS	0xCC	/* word 51; the old "spfireg +204" */
#define SPFI_LINK_MASK		0xFF	/* only the low byte is the link state, e.g. 0x4488 */
#define SPFI_LINK_UP		0x88

/* the old "spfireg +204" check */
static int spfi_link_up(uint32_t nn)
{
	char win[8];
	uint32_t v;

	snprintf(win, sizeof(win), "spfi%u", nn);
	if (reg_read(win, 0, SPFI_LINK_STATUS, &v))
		return 0;
	if ((v & SPFI_LINK_MASK) == SPFI_LINK_UP) {
		printf("      spfi%u: link status 0x%x at 0xCC (low byte 0x%x: up)\n", nn, v,
		       SPFI_LINK_UP);
		return 1;
	}
	printf("      spfi%u: link status 0x%x at 0xCC, low byte not 0x%x\n", nn, v, SPFI_LINK_UP);
	return 0;
}

static int spfi_cmd(uint32_t nn, uint32_t opcode, struct ramon_spfi_cmd *c, const char *what)
{
	c->nn = nn;
	c->opcode = opcode;
	c->wait_ack = 1;
	return must(RAMON_IOC_SPFI_CMD, c, &c->st, what);
}

/* GET_ALL_STREAM_STATUS, then the RX message table into @tab */
static int spfi_table(uint32_t nn, uint8_t *tab)
{
	struct ramon_spfi_mem_read m;
	struct ramon_spfi_cmd c;

	memset(&c, 0, sizeof(c));
	if (spfi_cmd(nn, RAMON_SPFI_OP_GET_ALL_STREAM_STATUS, &c, "SPFI GET_ALL_STREAM_STATUS"))
		return -1;
	memset(&m, 0, sizeof(m));
	m.nn = nn;
	m.size = SPFI_TABLE_BYTES;
	m.data = (uintptr_t)tab;
	return must(RAMON_IOC_SPFI_MEM_READ, &m, &m.st, "SPFI_MEM_READ stream table");
}

static const struct spfi_record *spfi_rec(const uint8_t *tab, uint32_t sid)
{
	return (const struct spfi_record *)(tab + SPFI_HDR_BYTES + sid * SPFI_REC_BYTES);
}

/* OPEN / FLUSH / CLOSE / DELETE; an rx_err_code is a failure here */
static int spfi_stream(uint32_t nn, uint32_t opcode, uint32_t sid, uint32_t last,
		       const char *what)
{
	struct ramon_spfi_cmd c;

	memset(&c, 0, sizeof(c));
	c.stream_id = sid;
	c.stream_last_offset = last;
	if (spfi_cmd(nn, opcode, &c, what))
		return -1;
	if (c.rx_err_code) {
		fail("spfi%u %s stream %u: rx_err_code %u (\"%s\")", nn, what, sid, c.rx_err_code,
		     c.st.msg);
		return -1;
	}
	if (opcode == RAMON_SPFI_OP_FLUSH_STREAM)
		printf("      spfi%u FLUSH stream %u: rx_offset %u, rx_status 0x%x\n", nn, sid,
		       c.rx_offset, c.rx_status);
	return 0;
}

/* SPFI status words (48..51 VC ctrl / write status, 19 rx opcode, 21 err, 33 rx status) */
static const uint32_t spfi_diag_words[] = { 48, 49, 50, 51, 19, 21, 33 };

/* SPFI_WR_STATUS0 (word 50) bits, as described by the FPGA team */
static void spfi_decode_ws0(uint32_t v)
{
	if (v & (1u << 5))
		printf(" VC1-sticky-no-TLAST");
	if (v & (1u << 3))
		printf(" VC1-FULL");
	if (v & (1u << 1))
		printf(" VC1-CMD-OVERFLOW");
}

static void spfi_diag(uint32_t nn, const char *when)
{
	struct ramon_get_stats g;
	char win[8];
	uint32_t i, v;

	snprintf(win, sizeof(win), "spfi%u", nn);
	printf("      spfi%u %s: words", nn, when);
	for (i = 0; i < sizeof(spfi_diag_words) / sizeof(spfi_diag_words[0]); i++) {
		struct ramon_reg_io r;

		memset(&r, 0, sizeof(r));
		snprintf(r.name, sizeof(r.name), "%s", win);
		r.offset = spfi_diag_words[i] * 4;
		v = ioctl(fd, RAMON_IOC_REG_IO, &r) ? 0xdeadbeef : r.value;
		printf(" [%u]=0x%x", spfi_diag_words[i], v);
		if (spfi_diag_words[i] == 50)
			spfi_decode_ws0(v);
	}
	memset(&g, 0, sizeof(g));
	if (!ioctl(fd, RAMON_IOC_GET_STATS, &g)) {
		printf("; irq vectors:");
		for (i = 0; i < RAMON_SPFI_IRQ_VECTORS; i++)
			if (g.spfi_irq_hist[nn][i])
				printf(" 0x%x:%" PRIu64, i, (uint64_t)g.spfi_irq_hist[nn][i]);
		printf("; unexpected %" PRIu64 " timeouts %" PRIu64, (uint64_t)g.spfi_unexpected[nn],
		       (uint64_t)g.spfi_timeouts[nn]);
	}
	printf("\n");
}

/* DATA_WRITE of SPFI_PAGES pages; non-cyclic, so stream_last_offset is 0 (old spfi_send) */
static int spfi_write(uint32_t nn, uint32_t sid, uint32_t tx_offset, const struct mbuf *src)
{
	struct ramon_sg_item it[SPFI_PAGES];
	struct ramon_spfi_write w;
	uint32_t i;

	for (i = 0; i < SPFI_PAGES; i++) {
		it[i].handle = src->h;
		it[i].pad = 0;
		it[i].offset = (uint64_t)i * SPFI_PAGE;
		it[i].len = SPFI_PAGE;
	}
	memset(&w, 0, sizeof(w));
	w.nn = nn;
	w.chan = spfi_chan[nn];
	w.stream_id = sid;
	w.tx_offset = tx_offset;
	w.tx_num_offset = SPFI_PAGES;
	w.n_items = SPFI_PAGES;
	w.items = (uintptr_t)it;
	printf("      spfi%u DATA_WRITE stream %u: tx_offset %u, tx_num_offset %u, stream_type 0, last 0, chan %u\n",
	       nn, sid, tx_offset, SPFI_PAGES, spfi_chan[nn]);
	if (!must(RAMON_IOC_SPFI_WRITE, &w, &w.st, "SPFI_WRITE"))
		return 0;
	spfi_diag(nn, "after the failed write");
	return -1;
}

static int spfi_read(uint32_t nn, uint32_t sid, uint32_t first, uint32_t pages,
		     const struct mbuf *dst)
{
	uint32_t off[SPFI_PAGES * SPFI_ROUNDS], i;
	struct ramon_spfi_read r;

	for (i = 0; i < pages; i++)
		off[i] = first + i;
	memset(&r, 0, sizeof(r));
	r.nn = nn;
	r.stream_id = sid;
	r.n_offsets = pages;
	r.offsets = (uintptr_t)off;
	r.dst_handle = dst->h;
	return must(RAMON_IOC_SPFI_READ, &r, &r.st, "SPFI_READ");
}

/* prints the header and the existing streams; returns how many are open */
static uint32_t spfi_print_table(uint32_t nn, const uint8_t *tab)
{
	const struct spfi_table_hdr *h = (const struct spfi_table_hdr *)tab;
	uint32_t sid, exist = 0, open = 0;
	uint8_t st;

	for (sid = 0; sid < SPFI_STREAMS; sid++) {
		st = spfi_rec(tab, sid)->status;
		exist += !!(st & SPFI_ST_EXIST);
		open += !!(st & SPFI_ST_OPEN);
	}
	printf("      spfi%u table: media %u pages, allocated %u, used %u; %u stream(s) exist, %u open\n",
	       nn, h->total_media_size, h->total_allocated_size, h->total_used_size, exist, open);
	for (sid = 0; sid < SPFI_STREAMS; sid++) {
		st = spfi_rec(tab, sid)->status;
		if (st & SPFI_ST_EXIST)
			printf("        stream %3u: status 0x%02x%s latest_write_offs %d%s\n", sid, st,
			       st & SPFI_ST_OPEN ? " open  " : " closed",
			       (int)spfi_rec(tab, sid)->latest_write_offs,
			       sid >= SPFI_STREAMS_USABLE ? "  (reserved id!)" : "");
	}
	return open;
}

/*
 * The two highest usable stream ids that do not exist (an existing stream
 * must be deleted before its id is opened again), provided two more open
 * streams stay within the NN's limit.
 */
static int spfi_free_streams(uint32_t nn, const uint8_t *tab, uint32_t *s1, uint32_t *s2)
{
	uint32_t sid, found = 0, open;

	open = spfi_print_table(nn, tab);
	if (open + 2 > SPFI_MAX_OPEN) {
		fail("spfi%u: %u streams are open; two more would exceed the NN limit of %d "
		     "(close/delete with \"ramon_smoke spfidel %u <sid>\")", nn, open, SPFI_MAX_OPEN, nn);
		return -1;
	}
	for (sid = SPFI_STREAMS_USABLE; sid-- > 0 && found < 2;) {
		if (spfi_rec(tab, sid)->status & SPFI_ST_EXIST)
			continue;
		if (found++)
			*s2 = sid;
		else
			*s1 = sid;
	}
	if (found < 2)
		fail("spfi%u: fewer than 2 free streams", nn);
	return found < 2 ? -1 : 0;
}

/*
 * The first page to write comes from the table read *before* OPEN (as the old
 * spfiginfo + spfi_send did); no GET_ALL_STREAM_STATUS between OPEN and the
 * first DATA_WRITE, matching the old command order.
 */
static int spfi_open(uint32_t nn, uint32_t sid, uint32_t last, const uint8_t *tab, uint32_t *tx)
{
	const struct spfi_record *r = spfi_rec(tab, sid);

	*tx = r->latest_write_offs + 1;
	if (spfi_stream(nn, RAMON_SPFI_OP_OPEN_STREAM_FOR_WRITE, sid, last, "OPEN"))
		return -1;
	printf("      spfi%u stream %u opened: table before OPEN had status 0x%x latest_write_offs %d -> first page %u\n",
	       nn, sid, r->status, (int)r->latest_write_offs, *tx);
	spfi_diag(nn, "after OPEN");
	return 0;
}

struct spfi_writer {
	uint32_t nn, sid, tx;
	struct mbuf *src;
	int ok;
};

/* SPFI_ROUNDS writes to the second stream, round r filled with seed 0x5000 + r */
static void *spfi_writer_thread(void *arg)
{
	struct spfi_writer *w = arg;
	uint32_t r;

	for (r = 0; r < SPFI_ROUNDS; r++) {
		fill((uint32_t *)w->src->p, w->src->size, 0x5000 + r);
		if (spfi_write(w->nn, w->sid, w->tx + r * SPFI_PAGES, w->src))
			return NULL;
	}
	w->ok = 1;
	return NULL;
}

static void spfi_verify(uint32_t nn, const struct mbuf *dst, size_t bytes, uint32_t seed,
			const char *what)
{
	long bad = verify((const uint32_t *)dst->p, bytes, seed);

	if (bad >= 0)
		fail("spfi%u %s: data differs at word %ld", nn, what, bad);
}

/* stream 1 round trip, then stream 2 written while stream 1 is read */
static void spfi_streams(uint32_t nn, uint8_t *tab, uint32_t s1, uint32_t s2, int *open1,
			 int *open2)
{
	struct mbuf src, dst, src2, dst2;
	struct spfi_writer w = { .nn = nn, .sid = s2, .src = &src2 };
	uint32_t tx1, r;
	pthread_t th;

	if (mbuf_new(&src, SPFI_PAGES * SPFI_PAGE))
		return;
	if (mbuf_new(&dst, SPFI_PAGES * SPFI_PAGE))
		goto out_src;
	if (mbuf_new(&src2, SPFI_PAGES * SPFI_PAGE))
		goto out_dst;
	if (mbuf_new(&dst2, SPFI_ROUNDS * SPFI_PAGES * SPFI_PAGE))
		goto out_src2;

	if (spfi_open(nn, s1, SPFI_PAGES, tab, &tx1))
		goto out;
	*open1 = 1;
	fill((uint32_t *)src.p, src.size, 0x4000 + nn);
	if (spfi_write(nn, s1, tx1, &src) ||
	    spfi_stream(nn, RAMON_SPFI_OP_FLUSH_STREAM, s1, 0, "FLUSH") ||
	    spfi_read(nn, s1, tx1, SPFI_PAGES, &dst))
		goto out;
	spfi_verify(nn, &dst, dst.size, 0x4000 + nn, "stream 1 read back");
	pass("spfi%u stream %u: open, write %d x 16 KiB, flush, read back, verified", nn, s1,
	     SPFI_PAGES);

	if (spfi_open(nn, s2, SPFI_ROUNDS * SPFI_PAGES, tab, &w.tx))
		goto out;
	*open2 = 1;
	if (pthread_create(&th, NULL, spfi_writer_thread, &w)) {
		fail("pthread_create");
		goto out;
	}
	for (r = 0; r < SPFI_ROUNDS; r++) {
		memset(dst.p, 0, dst.size);
		if (!spfi_read(nn, s1, tx1, SPFI_PAGES, &dst))
			spfi_verify(nn, &dst, dst.size, 0x4000 + nn, "concurrent read");
	}
	pthread_join(th, NULL);
	if (!w.ok || spfi_stream(nn, RAMON_SPFI_OP_FLUSH_STREAM, s2, 0, "FLUSH") ||
	    spfi_read(nn, s2, w.tx, SPFI_ROUNDS * SPFI_PAGES, &dst2))
		goto out;
	for (r = 0; r < SPFI_ROUNDS; r++) {
		long bad = verify((const uint32_t *)(dst2.p + (size_t)r * SPFI_PAGES * SPFI_PAGE),
				  SPFI_PAGES * SPFI_PAGE, 0x5000 + r);
		if (bad >= 0)
			fail("spfi%u stream %u round %u: data differs at word %ld", nn, s2, r, bad);
	}
	pass("spfi%u: %d writes to stream %u while stream %u was read %d times, all verified",
	     nn, SPFI_ROUNDS, s2, s1, SPFI_ROUNDS);
out:
	mbuf_del(&dst2);
out_src2:
	mbuf_del(&src2);
out_dst:
	mbuf_del(&dst);
out_src:
	mbuf_del(&src);
}

static int spfi_wait_alert(uint32_t nn, uint32_t timeout_ms, struct ramon_spfi_wait_alert *a)
{
	memset(a, 0, sizeof(*a));
	a->nn = nn;
	a->timeout_ms = timeout_ms;
	return ioctl(fd, RAMON_IOC_SPFI_WAIT_ALERT, a) ? errno : 0;
}

struct alert_arg {
	uint32_t nn;
	int err;
	struct ramon_spfi_wait_alert a;
};

static void *alert_thread(void *arg)
{
	struct alert_arg *c = arg;

	c->err = spfi_wait_alert(c->nn, 0, &c->a);
	return NULL;
}

/* print pending alerts, then an idle timeout and a cancel of two waiters */
static void spfi_alerts(uint32_t nn)
{
	struct alert_arg c[2] = { { .nn = nn }, { .nn = nn } };
	struct ramon_spfi_cancel_alert ca;
	struct ramon_spfi_wait_alert a;
	pthread_t th[2];
	int i, err = 0;

	for (i = 0; i < SPFI_ALERT_DRAIN; i++) {
		err = spfi_wait_alert(nn, SPW_SHORT_WAIT_MS, &a);
		if (err)
			break;
		printf("      spfi%u alert: code 0x%x sub 0x%x param1 0x%x param2 0x%x status 0x%x\n",
		       nn, a.code, a.sub_code, a.param1, a.param2, a.rx_status);
	}
	if (err != ETIMEDOUT || a.st.code != RAMON_E_SPFI_ALERT_TIMEOUT)
		fail("spfi%u idle alert wait: errno %d code %s", nn, err,
		     ramon_err_name(a.st.code));
	else
		pass("spfi%u: idle alert wait timed out", nn);

	for (i = 0; i < 2; i++)
		if (pthread_create(&th[i], NULL, alert_thread, &c[i])) {
			fail("pthread_create");
			return;
		}
	sleep_ms(CANCEL_AFTER_MS);
	memset(&ca, 0, sizeof(ca));
	ca.nn = nn;
	must(RAMON_IOC_SPFI_CANCEL_ALERT, &ca, &ca.st, "SPFI_CANCEL_ALERT");
	for (i = 0; i < 2; i++) {
		pthread_join(th[i], NULL);
		if (!c[i].err)
			printf("      spfi%u waiter %d got alert 0x%x instead of the cancel\n", nn, i,
			       c[i].a.code);
		else if (c[i].err != ECANCELED || c[i].a.st.code != RAMON_E_SPFI_ALERT_CANCELLED)
			fail("spfi%u waiter %d: errno %d code %s, expected ECANCELED", nn, i,
			     c[i].err, ramon_err_name(c[i].a.st.code));
		else
			pass("spfi%u alert waiter %d cancelled", nn, i);
	}
}

static int spfi_chan_ok(uint32_t nn)
{
	struct ramon_chan_info ci;

	if (chan_info(spfi_chan[nn], &ci) || ci.type != RAMON_CHAN_AXI ||
	    ci.dir != RAMON_DIR_MEM_TO_DEV) {
		fail("spfi%u: AXI channel %u is not a MEM_TO_DEV channel (--spfi-chans)", nn,
		     spfi_chan[nn]);
		return 0;
	}
	printf("      spfi%u: write channel %u (%.32s)\n", nn, spfi_chan[nn], ci.name);
	return 1;
}

static void spfi_init_nn(uint32_t nn)
{
	struct ramon_spfi_cmd c;

	memset(&c, 0, sizeof(c));
	c.init_type = 0;
	if (!spfi_cmd(nn, RAMON_SPFI_OP_INIT, &c, "SPFI INIT"))
		printf("      spfi%u INIT type 0: rx_err_code %u init_info 0x%x\n", nn,
		       c.rx_err_code, c.rx_init_info);
}

/* returns 1 if the SPFI link was up and the stream test ran */
static int spfi_one(uint32_t nn)
{
	uint8_t *tab;
	uint32_t s1 = 0, s2 = 0;
	int open1 = 0, open2 = 0;

	if (!spfi_link_up(nn)) {
		printf("      spfi%u: skipped; bring it up with \"ramon_smoke spfiprep %u\"\n", nn, nn);
		return 0;
	}
	tab = malloc(SPFI_TABLE_BYTES);
	if (!tab || !spfi_chan_ok(nn))
		goto out;
	if (spfi_init_first)
		spfi_init_nn(nn);
	spfi_diag(nn, "at start");
	if (spfi_table(nn, tab))
		goto out;
	spfi_diag(nn, "after GET_ALL_STREAM_STATUS");
	if (spfi_free_streams(nn, tab, &s1, &s2))
		goto out;
	spfi_streams(nn, tab, s1, s2, &open1, &open2);
	/* only what this test created is closed and deleted */
	if (open2 && !spfi_stream(nn, RAMON_SPFI_OP_CLOSE_STREAM_FOR_WRITE, s2, 0, "CLOSE"))
		spfi_stream(nn, RAMON_SPFI_OP_DELETE_STREAM, s2, 0, "DELETE");
	if (open1 && !spfi_stream(nn, RAMON_SPFI_OP_CLOSE_STREAM_FOR_WRITE, s1, 0, "CLOSE"))
		spfi_stream(nn, RAMON_SPFI_OP_DELETE_STREAM, s1, 0, "DELETE");
	if ((open1 || open2) && !spfi_table(nn, tab)) {
		if ((spfi_rec(tab, s1)->status | spfi_rec(tab, s2)->status) & SPFI_ST_EXIST)
			fail("spfi%u: test streams %u/%u still exist after DELETE", nn, s1, s2);
		else
			pass("spfi%u: test streams %u and %u closed and deleted", nn, s1, s2);
	}
	spfi_alerts(nn);
out:
	free(tab);
	return 1;
}

static void test_spfi(void)
{
	struct ramon_get_info gi;
	int present = 0, up = 0;
	uint32_t nn;

	if (get_info(&gi))
		return;
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(gi.spfi_mask & (1u << nn))) {
			printf("      spfi%u absent: skipped\n", nn);
			continue;
		}
		present++;
		up += spfi_one(nn);
	}
	if (present && !up)
		fail("no SPFI link is up (0xCC low byte != 0x88); run \"ramon_smoke spfiprep <nn>\" first");
}

/* ---- explicit SPW / SPFI commands (not in the default sequence) ---- */

/* loopback off (the operating state the old spwinit leaves), link synced, drained */
static int spw_open_link(struct spw_nn *s, uint32_t nn)
{
	struct ramon_get_info gi;
	int ret;

	if (get_info(&gi))
		return -1;
	if (nn >= RAMON_NN_COUNT || !(gi.spw_mask & (1u << nn))) {
		fail("spw%u is not present", nn);
		return -1;
	}
	if (spw_setup(s, nn))
		return -1;
	ret = spw_loopback(nn, 0);
	if (!ret)
		ret = spw_sync(s);
	if (ret) {
		if (ret > 0)
			fail("spw%u: link not synced", nn);
		mbuf_del(&s->scratch);
		return -1;
	}
	spw_drain(s);
	return 0;
}

/* ramon_smoke spwdps [nn] */
static void test_spwdps(void)
{
	struct spw_nn s;

	if (spw_open_link(&s, targ_u32(0, 0)))
		return;
	spw_dps(&s);
	spw_drain(&s);
	mbuf_del(&s.scratch);
}

static int parse_hex(const char *hex, uint8_t *out, uint32_t max, uint32_t *len)
{
	size_t n = strlen(hex);
	uint32_t i;
	char byte[3] = "";

	if (n % 2 || n / 2 > max)
		return -1;
	for (i = 0; i < n / 2; i++) {
		byte[0] = hex[2 * i];
		byte[1] = hex[2 * i + 1];
		if (!isxdigit((unsigned char)byte[0]) || !isxdigit((unsigned char)byte[1]))
			return -1;
		out[i] = strtoul(byte, NULL, 16);
	}
	*len = n / 2;
	return 0;
}

/* ramon_smoke spwsend <nn> <dst> <proto> <app> <attr> [hexpayload] */
static void test_spwsend(void)
{
	static uint8_t payload[SPW_MAX_PAYLOAD];
	struct spw_msg m;
	struct spw_nn s;
	uint32_t size;

	if (targc < 5) {
		fail("usage: spwsend <nn> <dst> <proto> <app> <attr> [hexpayload]");
		return;
	}
	memset(&m, 0, sizeof(m));
	m.dst = targ_u32(1, 0);
	m.protocol_id = targ_u32(2, 0);
	m.app_type = targ_u32(3, 0);
	m.attribute_id = targ_u32(4, 0);
	m.payload = payload;
	if (targc > 5 && parse_hex(targv[5], payload, sizeof(payload), &m.len)) {
		fail("payload \"%s\": even number of hex digits, at most %d bytes", targv[5],
		     SPW_MAX_PAYLOAD);
		return;
	}
	if (spw_open_link(&s, targ_u32(0, 0)))
		return;
	if (!spw_request(&s, &m, "message", &size) && !spw_check_reply(&s, size))
		pass("spw%u: reply received", s.nn);
	spw_drain(&s);
	mbuf_del(&s.scratch);
}

/* whatever alerts the NN queued (an answer may have come as an alert) */
static void spfi_print_alerts(uint32_t nn)
{
	struct ramon_spfi_wait_alert a;
	int i;

	for (i = 0; i < SPFI_ALERT_DRAIN && !spfi_wait_alert(nn, 100, &a); i++)
		printf("      spfi%u alert: code 0x%x sub 0x%x param1 0x%x param2 0x%x status 0x%x\n",
		       nn, a.code, a.sub_code, a.param1, a.param2, a.rx_status);
	if (!i)
		printf("      spfi%u: no alerts pending\n", nn);
}

/* ramon_smoke reg <window> <offset> [value]: one REG_IO read, or write + read back */
static void test_reg(void)
{
	uint32_t off = targ_u32(1, 0), v = targ_u32(2, 0), index;
	const char *name;

	if (targc < 2) {
		fail("usage: reg <window name or index> <offset> [value]");
		return;
	}
	/* a leading digit selects by index, anything else by name */
	name = isdigit((unsigned char)targv[0][0]) ? NULL : targv[0];
	index = name ? 0 : targ_u32(0, 0);
	if (targc > 2 && reg_io(name, index, RAMON_REG_OP_WRITE, off, &v))
		return;
	if (!reg_read(name, index, off, &v))
		printf("      %s[0x%x] = 0x%08x (%u)\n", targv[0], off, v, v);
}

/* ramon_smoke spfidel <nn> <sid>: CLOSE if open, then DELETE (any id, also 193..197) */
static void test_spfidel(void)
{
	uint32_t nn = targ_u32(0, 0), sid = targ_u32(1, SPFI_STREAMS);
	uint8_t *tab;

	if (targc < 2 || nn >= RAMON_NN_COUNT || sid >= SPFI_STREAMS) {
		fail("usage: spfidel <nn> <stream id 0..%d>", SPFI_STREAMS - 1);
		return;
	}
	tab = malloc(SPFI_TABLE_BYTES);
	if (!tab || spfi_table(nn, tab))
		goto out;
	if (!(spfi_rec(tab, sid)->status & SPFI_ST_EXIST)) {
		printf("      spfi%u stream %u does not exist; nothing to do\n", nn, sid);
		goto out;
	}
	if ((spfi_rec(tab, sid)->status & SPFI_ST_OPEN) &&
	    spfi_stream(nn, RAMON_SPFI_OP_CLOSE_STREAM_FOR_WRITE, sid, 0, "CLOSE"))
		goto out;
	if (spfi_stream(nn, RAMON_SPFI_OP_DELETE_STREAM, sid, 0, "DELETE") ||
	    spfi_table(nn, tab))
		goto out;
	if (spfi_rec(tab, sid)->status & SPFI_ST_EXIST)
		fail("spfi%u stream %u still exists after DELETE", nn, sid);
	else
		pass("spfi%u stream %u closed and deleted", nn, sid);
	spfi_print_table(nn, tab);
out:
	free(tab);
}

/*
 * ramon_smoke spfiprep [nn] [--format]: the old bring-up
 * spfireg +204 (0x88), spwdps, spfiinit 0, [spfifmt], spfiginfo
 */
static void test_spfiprep(void)
{
	uint32_t nn = targ_u32(0, 0);
	struct ramon_get_info gi;
	struct ramon_spfi_cmd c;
	uint8_t *tab;
	struct spw_nn s;

	if (get_info(&gi))
		return;
	if (nn >= RAMON_NN_COUNT || !(gi.spfi_mask & (1u << nn))) {
		fail("spfi%u is not present", nn);
		return;
	}
	if (!spfi_link_up(nn)) {
		fail("spfi%u: link not up; the old flow requires 0x88 in the low byte of 0xCC", nn);
		return;
	}
	pass("spfi%u: link up", nn);
	if (spw_open_link(&s, nn))
		return;
	spw_dps(&s);
	spw_drain(&s);
	mbuf_del(&s.scratch);
	spfi_init_nn(nn);
	spfi_diag(nn, "after INIT");
	if (spfi_format) {
		printf("      spfi%u: FORMAT erases every stream on the NN\n", nn);
		memset(&c, 0, sizeof(c));
		if (!spfi_cmd(nn, RAMON_SPFI_OP_FORMAT, &c, "SPFI FORMAT"))
			printf("      spfi%u FORMAT: rx_err_code %u\n", nn, c.rx_err_code);
		spfi_diag(nn, "after FORMAT");
	}
	tab = malloc(SPFI_TABLE_BYTES);
	if (tab && !spfi_table(nn, tab)) {
		spfi_print_table(nn, tab);
		pass("spfi%u prepared", nn);
	} else {
		spfi_diag(nn, "after the failed GET_ALL_STREAM_STATUS");
		spfi_print_alerts(nn);
		if (!spfi_format)
			printf("      hint: after a power-up the NN answers GET_ALL_STREAM_STATUS only once\n"
			       "      formatted; the old flow always ran spfifmt here: rerun with --format\n"
			       "      (it erases every stream on the NN)\n");
	}
	free(tab);
}

/* ---- zdma ---- */

static void zdma_single(void)
{
	struct ramon_copy c;
	struct mbuf a, b;
	long bad;

	if (mbuf_new(&a, 1024 * 1024))
		return;
	if (!mbuf_new(&b, 1024 * 1024)) {
		fill((uint32_t *)a.p, a.size, 0x11);
		memset(&c, 0, sizeof(c));
		c.src_handle = a.h;
		c.dst_handle = b.h;
		c.len = a.size;
		if (!zdma_copy(&c, 1, "ZDMA_COPY single")) {
			bad = verify((uint32_t *)b.p, b.size, 0x11);
			if (bad >= 0)
				fail("zdma single: mismatch at word %ld", bad);
			else
				pass("zdma single copy of %" PRIu64 " bytes verified", a.size);
		}
		mbuf_del(&b);
	}
	mbuf_del(&a);
}

/* n entries of scattered sizes, or all ZDMA_SMALL bytes, into consecutive dst slots */
static void zdma_list(uint32_t n, int small, const char *what)
{
	struct ramon_copy *c = calloc(n, sizeof(*c));
	uint64_t dst_off = 0;
	unsigned int seed = n;
	struct mbuf a, b;
	uint32_t i;

	if (!c || mbuf_new(&a, ZDMA_LIST_BUF)) {
		free(c);
		return;
	}
	if (mbuf_new(&b, ZDMA_LIST_BUF))
		goto out_a;
	fill((uint32_t *)a.p, a.size, 0x22);
	memset(b.p, 0, b.size);
	for (i = 0; i < n; i++) {
		c[i].src_handle = a.h;
		c[i].dst_handle = b.h;
		c[i].len = small ? ZDMA_SMALL : ZDMA_SMALL * (1 + rand_r(&seed) % 512);
		c[i].src_off = (rand_r(&seed) % (a.size - c[i].len)) & ~(uint64_t)(ZDMA_SMALL - 1);
		c[i].dst_off = dst_off;
		dst_off += c[i].len;
	}
	if (!zdma_copy(c, n, what)) {
		for (i = 0; i < n; i++)
			if (memcmp(b.p + c[i].dst_off, a.p + c[i].src_off, c[i].len))
				break;
		if (i < n)
			fail("%s: entry %u differs", what, i);
		else
			pass("%s: %u entries (%" PRIu64 " bytes) verified", what, n, dst_off);
	}
	mbuf_del(&b);
out_a:
	mbuf_del(&a);
	free(c);
}

static void *zdma_thread(void *arg)
{
	uint32_t seed = (uint32_t)(uintptr_t)arg;
	struct ramon_zdma_copy z;
	struct ramon_copy c;
	struct mbuf a, b;
	long bad = 0;
	int i;

	if (mbuf_new(&a, ZDMA_THREAD_BYTES))
		return (void *)1L;
	if (mbuf_new(&b, ZDMA_THREAD_BYTES)) {
		mbuf_del(&a);
		return (void *)1L;
	}
	for (i = 0; i < ZDMA_THREAD_COPIES; i++) {
		fill((uint32_t *)a.p, a.size, seed + i);
		memset(&c, 0, sizeof(c));
		c.src_handle = a.h;
		c.dst_handle = b.h;
		c.len = a.size;
		memset(&z, 0, sizeof(z));
		z.n = 1;
		z.entries = (uintptr_t)&c;
		if (ioctl(fd, RAMON_IOC_ZDMA_COPY, &z) || z.done != 1 ||
		    verify((uint32_t *)b.p, b.size, seed + i) >= 0)
			bad++;
	}
	mbuf_del(&b);
	mbuf_del(&a);
	return (void *)bad;
}

static void zdma_threads(uint32_t n_chan)
{
	pthread_t th[ZDMA_THREADS];
	long bad = 0;
	void *ret;
	int i;

	for (i = 0; i < ZDMA_THREADS; i++)
		if (pthread_create(&th[i], NULL, zdma_thread, (void *)(uintptr_t)(i << 16))) {
			fail("pthread_create");
			return;
		}
	for (i = 0; i < ZDMA_THREADS; i++) {
		pthread_join(th[i], &ret);
		bad += (long)ret;
	}
	if (bad)
		fail("zdma contention: %ld bad copies", bad);
	else
		pass("zdma contention: %d threads x %d copies over %u channels", ZDMA_THREADS,
		     ZDMA_THREAD_COPIES, n_chan);
}

static void test_zdma(void)
{
	struct ramon_get_info gi;

	if (get_info(&gi))
		return;
	if (!gi.n_zdma_chan) {
		printf("      no ZDMA channels (zdma_channels=0, or ps2psk still loaded?): skipped\n");
		return;
	}
	zdma_single();
	zdma_list(ZDMA_LIST_N, 0, "ZDMA_COPY list");
	zdma_list(RAMON_ZDMA_MAX_COPIES, 1, "ZDMA_COPY max list");
	zdma_threads(gi.n_zdma_chan);
}

/* ---- stats ---- */

static void test_stats(void)
{
	struct ramon_get_stats g;
	uint32_t nn, v;

	memset(&g, 0, sizeof(g));
	if (must(RAMON_IOC_GET_STATS, &g, &g.st, "GET_STATS"))
		return;
	printf("      axi tx %" PRIu64 " rx %" PRIu64 " timeouts %" PRIu64
	       ", zdma ops %" PRIu64 " timeouts %" PRIu64 "\n",
	       (uint64_t)g.axi_tx, (uint64_t)g.axi_rx, (uint64_t)g.axi_timeouts,
	       (uint64_t)g.zdma_ops, (uint64_t)g.zdma_timeouts);
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		printf("      nn%u: spw rx %" PRIu64 " overrun %" PRIu64
		       ", spfi unexpected %" PRIu64 " alert overrun %" PRIu64 " timeouts %" PRIu64
		       ", irq vectors:", nn, (uint64_t)g.spw_rx[nn], (uint64_t)g.spw_overrun[nn],
		       (uint64_t)g.spfi_unexpected[nn], (uint64_t)g.spfi_alert_overrun[nn],
		       (uint64_t)g.spfi_timeouts[nn]);
		for (v = 0; v < RAMON_SPFI_IRQ_VECTORS; v++)
			if (g.spfi_irq_hist[nn][v])
				printf(" 0x%x:%" PRIu64, v, (uint64_t)g.spfi_irq_hist[nn][v]);
		printf("\n");
	}

	memset(&g, 0, sizeof(g));
	g.reset = 1;
	if (must(RAMON_IOC_GET_STATS, &g, &g.st, "GET_STATS reset"))
		return;
	memset(&g, 0, sizeof(g));
	if (must(RAMON_IOC_GET_STATS, &g, &g.st, "GET_STATS after reset"))
		return;
	if (g.axi_tx || g.axi_rx || g.zdma_ops)
		fail("counters not zero after reset: axi %" PRIu64 "/%" PRIu64 " zdma %" PRIu64,
		     (uint64_t)g.axi_tx, (uint64_t)g.axi_rx, (uint64_t)g.zdma_ops);
	else
		pass("GET_STATS reset zeroes the counters");
}

/* ---- stress: everything at once for --minutes ---- */

enum { ST_ZDMA, ST_CHURN, ST_REG, ST_STATS, ST_SPW, ST_SPFI, ST_GROUPS };

static const char *const stress_names[ST_GROUPS] = {
	"zdma copy", "buf churn", "reg read", "get stats", "spw round trip", "spfi table",
};

static unsigned int stress_minutes = 10;
static atomic_int stress_stop;
static atomic_long stress_ops[ST_GROUPS];
static atomic_long stress_fails[ST_GROUPS];

struct stress_arg {
	int group;
	uint32_t seed;
	struct spw_nn *spw;
	struct spw_pkt *spw_tx;
	uint32_t nn;
};

static int stress_zdma(struct stress_arg *a, struct mbuf *src, struct mbuf *dst)
{
	struct ramon_zdma_copy z;
	struct ramon_copy c;

	fill((uint32_t *)src->p, src->size, a->seed++);
	memset(&c, 0, sizeof(c));
	c.src_handle = src->h;
	c.dst_handle = dst->h;
	c.len = src->size;
	memset(&z, 0, sizeof(z));
	z.n = 1;
	z.entries = (uintptr_t)&c;
	return ioctl(fd, RAMON_IOC_ZDMA_COPY, &z) || z.done != 1 ||
	       verify((uint32_t *)dst->p, dst->size, a->seed - 1) >= 0;
}

static int stress_churn(struct stress_arg *a)
{
	uint64_t size = 1 + rand_r(&a->seed) % CHURN_MAX_SIZE, actual;
	int bad = 0;
	uint32_t h;
	void *p;

	if (buf_alloc(size, &h, &actual))
		return 1;
	p = buf_map(h, actual, MAP_SHARED);
	if (p == MAP_FAILED) {
		bad = 1;
	} else {
		fill(p, actual, a->seed);
		bad = verify(p, actual, a->seed) >= 0;
		munmap(p, actual);
	}
	return buf_free(h) || bad;
}

static int stress_one(struct stress_arg *a, struct mbuf *src, struct mbuf *dst, uint8_t *tab)
{
	struct ramon_get_stats g;
	uint32_t v;

	switch (a->group) {
	case ST_ZDMA:
		return stress_zdma(a, src, dst);
	case ST_CHURN:
		return stress_churn(a);
	case ST_REG:
		return reg_read(NULL, RAMON_WIN_SYSMON_PS, SYSMON_TEMP, &v);
	case ST_STATS:
		memset(&g, 0, sizeof(g));
		return must(RAMON_IOC_GET_STATS, &g, &g.st, "GET_STATS");
	case ST_SPW:
		return spw_roundtrip_seq(a->spw, a->spw_tx);
	case ST_SPFI:
		return spfi_table(a->nn, tab);
	}
	return 1;
}

static void *stress_thread(void *arg)
{
	struct stress_arg *a = arg;
	uint8_t *tab = malloc(SPFI_TABLE_BYTES);
	struct mbuf src, dst;
	int have_bufs = 0;

	if (a->group == ST_ZDMA) {
		if (mbuf_new(&src, ZDMA_THREAD_BYTES))
			goto out;
		if (mbuf_new(&dst, ZDMA_THREAD_BYTES)) {
			mbuf_del(&src);
			goto out;
		}
		have_bufs = 1;
	}
	while (tab && !atomic_load(&stress_stop)) {
		if (stress_one(a, &src, &dst, tab))
			atomic_fetch_add(&stress_fails[a->group], 1);
		atomic_fetch_add(&stress_ops[a->group], 1);
	}
	if (have_bufs) {
		mbuf_del(&dst);
		mbuf_del(&src);
	}
out:
	free(tab);
	return NULL;
}

static void stress_report(const char *when)
{
	int g;

	printf("      %s:", when);
	for (g = 0; g < ST_GROUPS; g++)
		printf(" %s %ld/%ld", stress_names[g], atomic_load(&stress_ops[g]),
		       atomic_load(&stress_fails[g]));
	printf("  (ops/failures)\n");
}

/* SPW round trips need a synced link with loopback off */
static int stress_spw_setup(struct spw_nn *s, struct spw_pkt *tx, uint32_t nn)
{
	struct spw_msg m = spw_version_msg();

	if (spw_setup(s, nn))
		return -1;
	if (spw_loopback(nn, 0) || spw_sync(s) || spw_build(tx, &m)) {
		mbuf_del(&s->scratch);
		return -1;
	}
	spw_drain(s);
	return 0;
}

static void test_stress(void)
{
	struct stress_arg args[16];
	struct spw_nn spw[RAMON_NN_COUNT];
	struct spw_pkt spw_tx[RAMON_NN_COUNT];
	int spw_ok[RAMON_NN_COUNT] = { 0 };
	struct ramon_get_info gi;
	pthread_t th[16];
	int n = 0, i, g;
	long long end;
	uint32_t nn;

	if (get_info(&gi))
		return;
	for (i = 0; i < 4 && gi.n_zdma_chan; i++)
		args[n++] = (struct stress_arg){ .group = ST_ZDMA, .seed = 0x10000u * i };
	for (i = 0; i < 2; i++)
		args[n++] = (struct stress_arg){ .group = ST_CHURN, .seed = 77 + i };
	args[n++] = (struct stress_arg){ .group = ST_REG };
	args[n++] = (struct stress_arg){ .group = ST_STATS };
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if ((gi.spw_mask & (1u << nn)) && !stress_spw_setup(&spw[nn], &spw_tx[nn], nn)) {
			spw_ok[nn] = 1;
			args[n++] = (struct stress_arg){ .group = ST_SPW, .spw = &spw[nn],
							 .spw_tx = &spw_tx[nn] };
		}
		if ((gi.spfi_mask & (1u << nn)) && spfi_link_up(nn))
			args[n++] = (struct stress_arg){ .group = ST_SPFI, .nn = nn };
	}
	printf("      %d threads for %u minute(s); no SPFI stream data is written (media wear)\n",
	       n, stress_minutes);
	for (i = 0; i < n; i++)
		if (pthread_create(&th[i], NULL, stress_thread, &args[i]))
			fail("pthread_create");
	end = now_ms() + stress_minutes * 60000LL;
	for (g = 1; now_ms() < end; g++) {
		sleep_ms(end - now_ms() < 60000 ? end - now_ms() : 60000);
		if (now_ms() < end) {
			char when[32];

			snprintf(when, sizeof(when), "after %d min", g);
			stress_report(when);
		}
	}
	atomic_store(&stress_stop, 1);
	for (i = 0; i < n; i++)
		pthread_join(th[i], NULL);
	stress_report("total");
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!spw_ok[nn])
			continue;
		mbuf_del(&spw_tx[nn].buf);
		mbuf_del(&spw[nn].scratch);
		if (spw[nn].saved_loopback)
			spw_loopback(nn, 1);
	}
	for (g = 0; g < ST_GROUPS; g++)
		if (atomic_load(&stress_fails[g]))
			fail("stress %s: %ld failures", stress_names[g],
			     atomic_load(&stress_fails[g]));
	pass("stress finished");
}

/* ---- unbind: unbind with an fd open and a buffer mapped, then rebind ---- */

static int sysfs_write(const char *path, const char *val)
{
	int f = open(path, O_WRONLY), err = 0;

	if (f < 0)
		return errno;
	if (write(f, val, strlen(val)) < 0)
		err = errno;
	close(f);
	return err;
}

/* platform device name behind /dev/ramon_dma, e.g. "a0000000.nanrec_axidma" */
static int bound_device(char *name, size_t len)
{
	char link[PATH_MAX];
	const char *base;
	ssize_t n = readlink(SYSFS_MISC_DEVICE, link, sizeof(link) - 1);

	if (n < 0) {
		fail("readlink %s: %s", SYSFS_MISC_DEVICE, strerror(errno));
		return -1;
	}
	link[n] = '\0';
	base = strrchr(link, '/');
	snprintf(name, len, "%s", base ? base + 1 : link);
	return 0;
}

static int reopen_device(void)
{
	int waited;

	for (waited = 0; waited < REBIND_WAIT_MS; waited += 50) {
		fd = open(dev_path, O_RDWR);
		if (fd >= 0)
			return 0;
		sleep_ms(50);
	}
	fail("%s did not come back after rebind: %s", dev_path, strerror(errno));
	return -1;
}

static void unbind_checks(uint32_t h, void *p, uint64_t actual)
{
	struct ramon_buf_alloc a;
	struct ramon_get_info gi;

	if (access(dev_path, F_OK) == 0)
		fail("%s still exists after unbind", dev_path);
	/* GET_INFO does not need a live device */
	if (!get_info(&gi))
		pass("GET_INFO after unbind");
	if (verify(p, actual, 3) >= 0)
		fail("mapping changed across the unbind");
	else
		pass("mapping still readable after unbind");
	memset(&a, 0, sizeof(a));
	a.size = page_size;
	expect_fail(RAMON_IOC_BUF_ALLOC, &a, &a.st, "BUF_ALLOC after unbind", ENODEV,
		    RAMON_E_REMOVED, "removed");
	if (buf_map(h, page_size, MAP_SHARED) != MAP_FAILED || errno != ENODEV)
		fail("mmap after unbind: want ENODEV, got %s", strerror(errno));
	else
		pass("mmap after unbind -> ENODEV");
}

/* a thread blocked in a waiting ioctl across the unbind */
struct blocker {
	const char *what;
	unsigned long req;
	union {
		struct ramon_spfi_wait_alert alert;
		struct ramon_spw_wait_rx spw;
	} u;
	struct ramon_status *st;
	int err;
	pthread_t th;
	int running;
};

static void *blocker_thread(void *arg)
{
	struct blocker *b = arg;

	b->err = ioctl(fd, b->req, &b->u) ? errno : 0;
	return NULL;
}

static int blockers_start(struct blocker *b, const struct ramon_get_info *gi)
{
	uint32_t nn;
	int n = 0;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (gi->spfi_mask & (1u << nn)) {
			memset(&b[n], 0, sizeof(b[n]));
			b[n].what = nn ? "SPFI_WAIT_ALERT nn1" : "SPFI_WAIT_ALERT nn0";
			b[n].req = RAMON_IOC_SPFI_WAIT_ALERT;
			b[n].u.alert.nn = nn;
			b[n].st = &b[n].u.alert.st;
			n++;
		}
		if (gi->spw_mask & (1u << nn)) {
			memset(&b[n], 0, sizeof(b[n]));
			b[n].what = nn ? "SPW_WAIT_RX nn1" : "SPW_WAIT_RX nn0";
			b[n].req = RAMON_IOC_SPW_WAIT_RX;
			b[n].u.spw.nn = nn;
			b[n].u.spw.timeout_ms = RAMON_TIMEOUT_MAX_MS;
			b[n].st = &b[n].u.spw.st;
			n++;
		}
	}
	for (nn = 0; nn < (uint32_t)n; nn++)
		b[nn].running = !pthread_create(&b[nn].th, NULL, blocker_thread, &b[nn]);
	sleep_ms(CANCEL_AFTER_MS);
	return n;
}

static void blockers_check(struct blocker *b, int n)
{
	struct timespec deadline;
	int i;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	for (i = 0; i < n; i++) {
		if (!b[i].running)
			continue;
		if (pthread_timedjoin_np(b[i].th, NULL, &deadline)) {
			fail("%s still blocked 5 s after the unbind", b[i].what);
			continue;
		}
		if (!b[i].err)
			printf("      %s returned data before the unbind\n", b[i].what);
		else if (b[i].err != ENODEV || b[i].st->code != RAMON_E_REMOVED)
			fail("%s: errno %d code %s after unbind, expected ENODEV / REMOVED",
			     b[i].what, b[i].err, ramon_err_name(b[i].st->code));
		else
			pass("%s released by the unbind with ENODEV / RAMON_E_REMOVED", b[i].what);
	}
}

static void test_unbind(void)
{
	struct blocker blk[2 * RAMON_NN_COUNT];
	int n_blk;
	struct ramon_get_info gi;
	char name[PATH_MAX];
	uint64_t actual;
	uint32_t h;
	void *p;
	int err;

	if (bound_device(name, sizeof(name)) || buf_alloc(1024 * 1024, &h, &actual))
		return;
	p = buf_map(h, actual, MAP_SHARED);
	if (p == MAP_FAILED) {
		fail("mmap handle %u: %s", h, strerror(errno));
		buf_free(h);
		return;
	}
	fill(p, actual, 3);
	if (get_info(&gi))
		return;
	n_blk = drv_step >= 7 ? blockers_start(blk, &gi) : 0;

	err = sysfs_write(SYSFS_DRIVER "/unbind", name);
	if (err) {
		fail("unbind %s: %s (needs root)", name, strerror(err));
		munmap(p, actual);
		buf_free(h);
		return;
	}
	pass("unbound %s with fd open, buffer %u mapped and %d waiter(s) blocked", name, h, n_blk);
	blockers_check(blk, n_blk);
	unbind_checks(h, p, actual);

	/* the last references go now: the buffer is freed on the unbound device */
	munmap(p, actual);
	buf_free(h);
	close(fd);
	fd = -1;

	err = sysfs_write(SYSFS_DRIVER "/bind", name);
	if (err) {
		fail("bind %s: %s", name, strerror(err));
		return;
	}
	if (reopen_device() || get_info(&gi))
		return;
	pass("rebound %s; a new fd works (check dmesg for warnings)", name);
}

/* ---- error sweep ---- */

static void errors_dispatch(void)
{
	struct ramon_get_info gi;

	memset(&gi, 0, sizeof(gi));
	expect_errno(_IOWR(RAMON_IOC_MAGIC, 99, struct ramon_get_info), &gi,
		     "unknown ioctl number", ENOTTY);
	expect_errno(_IOWR('I', 1, struct ramon_get_info), &gi, "foreign ioctl magic", ENOTTY);
	expect_errno(_IOWR(RAMON_IOC_MAGIC, 1, struct ramon_chan_info), &gi,
		     "GET_INFO number with a wrong size", ENOTTY);
	expect_errno(_IOR(RAMON_IOC_MAGIC, 1, struct ramon_get_info), &gi,
		     "GET_INFO number with a wrong direction", ENOTTY);
	expect_errno(RAMON_IOC_GET_INFO, (void *)8, "GET_INFO with a bad pointer", EFAULT);
}

static void errors_buf_ioctls(uint64_t max)
{
	struct ramon_buf_alloc a;
	struct ramon_buf_free f;
	struct ramon_buf_info bi;
	char want[32];

	memset(&a, 0, sizeof(a));
	expect_fail(RAMON_IOC_BUF_ALLOC, &a, &a.st, "BUF_ALLOC size 0", EINVAL,
		    RAMON_E_INVAL_ARG, "size 0");

	memset(&a, 0, sizeof(a));
	a.size = max + 1;
	snprintf(want, sizeof(want), "0x%" PRIx64, max + 1);
	expect_fail(RAMON_IOC_BUF_ALLOC, &a, &a.st, "BUF_ALLOC above max_buf_mb", EINVAL,
		    RAMON_E_BUF_TOO_LARGE, want);

	snprintf(want, sizeof(want), "%u", BOGUS_HANDLE);
	memset(&f, 0, sizeof(f));
	f.handle = BOGUS_HANDLE;
	expect_fail(RAMON_IOC_BUF_FREE, &f, &f.st, "BUF_FREE unknown handle", ENOENT,
		    RAMON_E_NO_SUCH_HANDLE, want);
	memset(&bi, 0, sizeof(bi));
	bi.handle = BOGUS_HANDLE;
	expect_fail(RAMON_IOC_BUF_INFO, &bi, &bi.st, "BUF_INFO unknown handle", ENOENT,
		    RAMON_E_NO_SUCH_HANDLE, want);
}

static void expect_map_errno(uint32_t handle, size_t len, int flags, const char *what,
			     int want_errno)
{
	void *p = buf_map(handle, len, flags);

	if (p != MAP_FAILED) {
		fail("%s: mmap succeeded", what);
		munmap(p, len);
	} else if (errno != want_errno) {
		fail("%s: errno %d (%s), expected %d (%s)", what, errno, strerror(errno),
		     want_errno, strerror(want_errno));
	} else {
		pass("%s -> errno %d (%s)", what, errno, strerror(errno));
	}
}

static void errors_mmap(void)
{
	uint32_t h;

	if (buf_alloc(page_size, &h, NULL))
		return;
	expect_map_errno(h, page_size, MAP_PRIVATE, "mmap MAP_PRIVATE", EINVAL);
	expect_map_errno(h, 2 * page_size, MAP_SHARED, "mmap longer than the buffer", EINVAL);
	expect_map_errno(BOGUS_HANDLE, page_size, MAP_SHARED, "mmap unknown handle", ENOENT);
	expect_map_errno(0, page_size, MAP_SHARED, "mmap offset 0 (handle 0)", ENOENT);
	buf_free(h);
}

/* allocate max-size buffers until the coherent allocator gives up */
static void errors_cma_exhaustion(uint64_t max)
{
	uint32_t held[EXHAUST_MAX_BUFS];
	struct ramon_buf_alloc a;
	int n, i;

	for (n = 0; n < EXHAUST_MAX_BUFS; n++) {
		memset(&a, 0, sizeof(a));
		a.size = max;
		if (ioctl(fd, RAMON_IOC_BUF_ALLOC, &a))
			break;
		held[n] = a.handle;
	}
	if (n == EXHAUST_MAX_BUFS) {
		printf("      note: %d x %" PRIu64 " bytes all succeeded; exhaustion not reached\n",
		       n, max);
	} else if (errno != ENOMEM || a.st.code != RAMON_E_BUF_ALLOC_FAILED || !a.st.msg[0]) {
		fail("allocation %d of %" PRIu64 " bytes failed unexpectedly", n + 1, max);
		print_status("got", errno, &a.st);
	} else {
		seen(a.st.code);
		pass("CMA exhaustion after %d x %" PRIu64 " bytes -> %s \"%s\"", n, max,
		     ramon_err_name(a.st.code), a.st.msg);
	}
	for (i = 0; i < n; i++)
		buf_free(held[i]);
}

static void expect_reg_fail(const char *name, uint32_t index, uint32_t op, uint32_t offset,
			    const char *what, int want_errno, uint32_t want_code,
			    const char *want_in_msg)
{
	struct ramon_reg_io r;

	memset(&r, 0, sizeof(r));
	if (name)
		memcpy(r.name, name, strnlen(name, sizeof(r.name)));
	r.index = index;
	r.op = op;
	r.offset = offset;
	expect_fail(RAMON_IOC_REG_IO, &r, &r.st, what, want_errno, want_code, want_in_msg);
}

static void errors_regwin(uint32_t n)
{
	struct ramon_regwin_info ri;
	char want[32], name[64];
	uint32_t i;

	snprintf(want, sizeof(want), "%u", n);
	memset(&ri, 0, sizeof(ri));
	ri.index = n;
	expect_fail(RAMON_IOC_REGWIN_INFO, &ri, &ri.st, "REGWIN_INFO past the end", ENOENT,
		    RAMON_E_REGWIN_BAD_INDEX, want);
	expect_reg_fail(NULL, n, RAMON_REG_OP_READ, 0, "REG_IO index past the end", ENOENT,
			RAMON_E_REGWIN_BAD_INDEX, want);
	expect_reg_fail("no_such_window", 0, RAMON_REG_OP_READ, 0, "REG_IO unknown name",
			ENOENT, RAMON_E_REGWIN_BAD_NAME, "no_such_window");
	/* 32 characters, no NUL: the driver must not read past the field */
	expect_reg_fail("abcdefghijklmnopqrstuvwxyz012345", 0, RAMON_REG_OP_READ, 0,
			"REG_IO 32-char unterminated name", ENOENT, RAMON_E_REGWIN_BAD_NAME,
			"abcdefghijklmnopqrstuvwxyz012345");

	if (win_present(RAMON_WIN_SYSMON_PS)) {
		expect_reg_fail("sysmon_ps", 0, RAMON_REG_OP_READ, 0x2, "REG_IO unaligned offset",
				EINVAL, RAMON_E_REGWIN_UNALIGNED, "0x2");
		expect_reg_fail("sysmon_ps", 0, RAMON_REG_OP_READ, 0x300,
				"REG_IO offset at the end of the window", ERANGE,
				RAMON_E_REGWIN_OFFSET_RANGE, "0x300");
		expect_reg_fail("sysmon_ps", 0, RAMON_REG_OP_READ, 0xfffffffc,
				"REG_IO offset near 4 GiB", ERANGE,
				RAMON_E_REGWIN_OFFSET_RANGE, "0xfffffffc");
		expect_reg_fail("sysmon_ps", 0, RAMON_REG_OP_WRITE, 0, "REG_IO write to read-only",
				EROFS, RAMON_E_REGWIN_READ_ONLY, "read-only");
		expect_reg_fail("sysmon_ps", 0, 7, 0, "REG_IO op 7", EINVAL, RAMON_E_INVAL_ARG,
				"op 7");
	}
	for (i = 0; i < n; i++) {
		if (regwin_info(i, &ri) || !(ri.flags & RAMON_REGWIN_ABSENT))
			continue;
		snprintf(name, sizeof(name), "REG_IO on absent window %.32s", ri.name);
		expect_reg_fail(NULL, i, RAMON_REG_OP_READ, 0, name, ENXIO, RAMON_E_NOT_PRESENT,
				"not present");
		break;
	}
	if (i == n)
		printf("      every window is present: NOT_PRESENT not exercised\n");
}

static void expect_axi_fail(uint32_t chan, uint32_t n, uint64_t items, uint32_t timeout_ms,
			    const char *what, int want_errno, uint32_t want_code,
			    const char *want_in_msg)
{
	struct ramon_axi_xfer x;

	memset(&x, 0, sizeof(x));
	x.chan = chan;
	x.n_items = n;
	x.items = items;
	x.timeout_ms = timeout_ms;
	expect_fail(RAMON_IOC_AXI_XFER, &x, &x.st, what, want_errno, want_code, want_in_msg);
}

static void errors_axi(const struct ramon_get_info *gi)
{
	struct ramon_sg_item it;
	struct ramon_chan_info ci;
	char want[64];
	uint32_t h;

	snprintf(want, sizeof(want), "%u", gi->n_axi_chan + gi->n_zdma_chan);
	memset(&ci, 0, sizeof(ci));
	ci.index = gi->n_axi_chan + gi->n_zdma_chan;
	expect_fail(RAMON_IOC_CHAN_INFO, &ci, &ci.st, "CHAN_INFO past the end", ENOENT,
		    RAMON_E_AXI_BAD_CHAN, want);
	snprintf(want, sizeof(want), "chan %u", gi->n_axi_chan);
	expect_axi_fail(gi->n_axi_chan, 1, 0, 0, "AXI_XFER bad channel", ENOENT,
			RAMON_E_AXI_BAD_CHAN, want);
	expect_axi_fail(0, 0, 0, 0, "AXI_XFER 0 items", EINVAL, RAMON_E_BAD_COUNT, "n_items 0");
	expect_axi_fail(0, RAMON_AXI_MAX_ITEMS + 1, 0, 0, "AXI_XFER too many items", EINVAL,
			RAMON_E_BAD_COUNT, "513");
	expect_axi_fail(0, 1, 8, 0, "AXI_XFER bad items pointer", EFAULT, RAMON_E_COPY_FAULT,
			"0x8");

	if (buf_alloc(page_size, &h, NULL))
		return;
	memset(&it, 0, sizeof(it));
	it.handle = h;
	expect_axi_fail(0, 1, (uintptr_t)&it, 0, "AXI_XFER item len 0", EINVAL,
			RAMON_E_INVAL_ARG, "len 0");
	it.handle = BOGUS_HANDLE;
	it.len = 8;
	snprintf(want, sizeof(want), "buf %u", BOGUS_HANDLE);
	expect_axi_fail(0, 1, (uintptr_t)&it, 0, "AXI_XFER unknown handle", ENOENT,
			RAMON_E_NO_SUCH_HANDLE, want);
	it.handle = h;
	it.offset = page_size - 4;
	snprintf(want, sizeof(want), "offset 0x%zx", page_size - 4);
	expect_axi_fail(0, 1, (uintptr_t)&it, 0, "AXI_XFER item past the buffer end", ERANGE,
			RAMON_E_BUF_RANGE, want);
	it.offset = UINT64_MAX - 3;
	expect_axi_fail(0, 1, (uintptr_t)&it, 0, "AXI_XFER offset + len overflows", ERANGE,
			RAMON_E_BUF_RANGE, "0xfffffffffffffffc");
	buf_free(h);
}

/* an RX transfer that cannot complete: loopback on, nothing sent */
static void errors_axi_timeout(const struct ramon_get_info *gi)
{
	struct ramon_sg_item it;
	struct spw_nn s;

	if (!(gi->spw_mask & 1) || spw_setup(&s, 0))
		return;
	if (!spw_loopback(0, 1)) {
		spw_drain(&s);
		memset(&it, 0, sizeof(it));
		it.handle = s.scratch.h;
		it.len = page_size;
		printf("      the next check terminates AXI ch%u, which resets its axi_dma IP\n",
		       s.rx);
		expect_axi_fail(s.rx, 1, (uintptr_t)&it, SPW_SHORT_WAIT_MS,
				"AXI_XFER RX with no traffic", ETIMEDOUT, RAMON_E_AXI_TIMEOUT,
				"200 ms");
		spw_loopback(0, s.saved_loopback ? 1 : 0);
	}
	mbuf_del(&s.scratch);
}

static void expect_zdma_fail(const struct ramon_copy *c, uint32_t n, uint64_t ptr,
			     const char *what, int want_errno, uint32_t want_code,
			     const char *want_in_msg)
{
	struct ramon_zdma_copy z;

	memset(&z, 0, sizeof(z));
	z.n = n;
	z.entries = c ? (uintptr_t)c : ptr;
	expect_fail(RAMON_IOC_ZDMA_COPY, &z, &z.st, what, want_errno, want_code, want_in_msg);
	if (z.done)
		fail("%s: done %u, expected 0", what, z.done);
}

static void errors_zdma(const struct ramon_get_info *gi)
{
	struct ramon_copy c[2];
	struct mbuf a;
	char want[64];

	if (!gi->n_zdma_chan) {
		expect_zdma_fail(NULL, 1, 0, "ZDMA_COPY without channels", ENXIO,
				 RAMON_E_ZDMA_NO_CHANNELS, "zdma_channels");
		return;
	}
	expect_zdma_fail(NULL, 0, 0, "ZDMA_COPY n 0", EINVAL, RAMON_E_BAD_COUNT, "n 0");
	expect_zdma_fail(NULL, RAMON_ZDMA_MAX_COPIES + 1, 0, "ZDMA_COPY n too large", EINVAL,
			 RAMON_E_BAD_COUNT, "4097");
	expect_zdma_fail(NULL, 1, 8, "ZDMA_COPY bad entries pointer", EFAULT,
			 RAMON_E_COPY_FAULT, "0x8");
	if (mbuf_new(&a, 2 * page_size))
		return;
	memset(c, 0, sizeof(c));
	c[0].src_handle = a.h;
	c[0].dst_handle = a.h;
	c[0].dst_off = page_size;
	c[0].len = ZDMA_SMALL;
	c[1] = c[0];
	c[1].src_handle = BOGUS_HANDLE;
	snprintf(want, sizeof(want), "buf %u", BOGUS_HANDLE);
	memset(a.p, 0x33, ZDMA_SMALL);
	memset(a.p + page_size, 0, ZDMA_SMALL);
	expect_zdma_fail(c, 2, 0, "ZDMA_COPY list with a bad second entry", ENOENT,
			 RAMON_E_NO_SUCH_HANDLE, want);
	if (a.p[page_size])
		fail("ZDMA_COPY with an invalid entry copied the valid one");
	else
		pass("an invalid list copies nothing");
	c[1] = c[0];
	c[1].dst_off = 2 * page_size - ZDMA_SMALL / 2;
	expect_zdma_fail(c, 2, 0, "ZDMA_COPY dst past the buffer end", ERANGE,
			 RAMON_E_BUF_RANGE, "zdma dst 1");
	mbuf_del(&a);
}

/* idle wait and cancel with loopback on, so no NN traffic interferes */
static void errors_spw_waits(void)
{
	struct ramon_spw_wait_rx w;
	struct spw_nn s;

	if (spw_setup(&s, 0))
		return;
	if (!spw_loopback(0, 1)) {
		spw_drain(&s);
		memset(&w, 0, sizeof(w));
		w.timeout_ms = SPW_SHORT_WAIT_MS;
		expect_fail(RAMON_IOC_SPW_WAIT_RX, &w, &w.st, "SPW_WAIT_RX idle", ETIMEDOUT,
			    RAMON_E_SPW_TIMEOUT, "200 ms");
		spw_cancel_check(0);
		seen(RAMON_E_SPW_CANCELLED);
		spw_loopback(0, s.saved_loopback ? 1 : 0);
	}
	mbuf_del(&s.scratch);
}

static void errors_spw(const struct ramon_get_info *gi)
{
	struct ramon_spw_wait_rx w;
	struct ramon_spw_loopback l;
	uint32_t nn;

	memset(&w, 0, sizeof(w));
	w.nn = RAMON_NN_COUNT;
	expect_fail(RAMON_IOC_SPW_WAIT_RX, &w, &w.st, "SPW_WAIT_RX nn 2", EINVAL,
		    RAMON_E_SPW_BAD_NN, "nn 2");
	memset(&l, 0, sizeof(l));
	l.enable = 2;
	expect_fail(RAMON_IOC_SPW_LOOPBACK, &l, &l.st, "SPW_LOOPBACK enable 2", EINVAL,
		    RAMON_E_INVAL_ARG, "enable 2");
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (gi->spw_mask & (1u << nn))
			continue;
		memset(&w, 0, sizeof(w));
		w.nn = nn;
		expect_fail(RAMON_IOC_SPW_WAIT_RX, &w, &w.st, "SPW_WAIT_RX on an absent NN",
			    ENXIO, RAMON_E_NOT_PRESENT, "not present");
	}
}

static void expect_spfi_cmd_fail(uint32_t nn, uint32_t opcode, const char *what,
				 int want_errno, uint32_t want_code, const char *want_in_msg)
{
	struct ramon_spfi_cmd c;

	memset(&c, 0, sizeof(c));
	c.nn = nn;
	c.opcode = opcode;
	c.wait_ack = 1;
	expect_fail(RAMON_IOC_SPFI_CMD, &c, &c.st, what, want_errno, want_code, want_in_msg);
}

/* a 1 ms timeout on a harmless command; the late answer is then counted as unexpected */
static void errors_spfi_cmd_timeout(uint32_t nn)
{
	struct ramon_spfi_cmd c;
	int err;

	memset(&c, 0, sizeof(c));
	c.nn = nn;
	c.opcode = RAMON_SPFI_OP_GET_ALL_STREAM_STATUS;
	c.wait_ack = 1;
	c.timeout_ms = 1;
	err = ioctl(fd, RAMON_IOC_SPFI_CMD, &c) ? errno : 0;
	if (!err)
		printf("      the NN answered within 1 ms: SPFI_CMD_TIMEOUT not exercised\n");
	else if (err != ETIMEDOUT || c.st.code != RAMON_E_SPFI_CMD_TIMEOUT)
		fail("SPFI_CMD 1 ms timeout: errno %d code %s", err, ramon_err_name(c.st.code));
	else {
		seen(c.st.code);
		pass("SPFI_CMD with a 1 ms timeout -> %s \"%s\"", ramon_err_name(c.st.code),
		     c.st.msg);
	}
	sleep_ms(500);	/* let the late answer land before the next command */
}

static void errors_spfi_read(uint32_t nn)
{
	struct ramon_spfi_read r;
	uint32_t off[2] = { 0, 1 };
	struct mbuf small;

	memset(&r, 0, sizeof(r));
	r.nn = nn;
	expect_fail(RAMON_IOC_SPFI_READ, &r, &r.st, "SPFI_READ 0 offsets", EINVAL,
		    RAMON_E_BAD_COUNT, "n_offsets 0");
	r.n_offsets = RAMON_SPFI_TX_OFFS_MAX / 4 + 1;
	expect_fail(RAMON_IOC_SPFI_READ, &r, &r.st, "SPFI_READ too many offsets", EINVAL,
		    RAMON_E_BAD_COUNT, "8193");
	if (mbuf_new(&small, SPFI_PAGE))
		return;
	r.n_offsets = 2;
	r.offsets = (uintptr_t)off;
	r.dst_handle = small.h;
	expect_fail(RAMON_IOC_SPFI_READ, &r, &r.st, "SPFI_READ into a 1-page buffer", ERANGE,
		    RAMON_E_SPFI_DST_TOO_SMALL, "0x8000");
	r.n_offsets = 1;
	r.offsets = 8;
	expect_fail(RAMON_IOC_SPFI_READ, &r, &r.st, "SPFI_READ bad offsets pointer", EFAULT,
		    RAMON_E_COPY_FAULT, "0x8");
	mbuf_del(&small);
}

/* a READ of a stream that does not exist: expected to time out */
static void errors_spfi_read_timeout(uint32_t nn)
{
	uint8_t *tab = malloc(SPFI_TABLE_BYTES);
	struct ramon_spfi_read r;
	uint32_t s1, s2, off = 0;
	struct mbuf dst;
	int err;

	if (!tab || spfi_table(nn, tab) || spfi_free_streams(nn, tab, &s1, &s2) ||
	    mbuf_new(&dst, SPFI_PAGE)) {
		free(tab);
		return;
	}
	memset(&r, 0, sizeof(r));
	r.nn = nn;
	r.stream_id = s1;
	r.n_offsets = 1;
	r.offsets = (uintptr_t)&off;
	r.dst_handle = dst.h;
	r.timeout_ms = 300;
	err = ioctl(fd, RAMON_IOC_SPFI_READ, &r) ? errno : 0;
	if (err == ETIMEDOUT && r.st.code == RAMON_E_SPFI_READ_TIMEOUT) {
		seen(r.st.code);
		pass("SPFI_READ of missing stream %u -> %s \"%s\"", s1,
		     ramon_err_name(r.st.code), r.st.msg);
	} else {
		printf("      SPFI_READ of missing stream %u: errno %d %s \"%s\" (READ_TIMEOUT not exercised)\n",
		       s1, err, ramon_err_name(r.st.code), r.st.msg);
	}
	sleep_ms(500);
	mbuf_del(&dst);
	free(tab);
}

static void errors_spfi(const struct ramon_get_info *gi)
{
	struct ramon_spfi_tx_offs_write t;
	struct ramon_spfi_mem_read m;
	struct ramon_spfi_write w;
	struct ramon_sg_item it;
	uint8_t buf[8];
	struct mbuf one;
	uint32_t nn;

	expect_spfi_cmd_fail(RAMON_NN_COUNT, RAMON_SPFI_OP_GET_ALL_STREAM_STATUS,
			     "SPFI_CMD nn 2", EINVAL, RAMON_E_SPFI_BAD_NN, "nn 2");
	for (nn = 0; nn < RAMON_NN_COUNT && !(gi->spfi_mask & (1u << nn)); nn++)
		;
	if (nn == RAMON_NN_COUNT) {
		expect_spfi_cmd_fail(0, RAMON_SPFI_OP_GET_ALL_STREAM_STATUS,
				     "SPFI_CMD without SPFI", ENXIO, RAMON_E_NOT_PRESENT, "spfi0");
		return;
	}
	expect_spfi_cmd_fail(nn, RAMON_SPFI_OP_DATA_WRITE, "SPFI_CMD DATA_WRITE", EINVAL,
			     RAMON_E_SPFI_BAD_OPCODE, "0x10");
	expect_spfi_cmd_fail(nn, 0x77, "SPFI_CMD opcode 0x77", EINVAL, RAMON_E_SPFI_BAD_OPCODE,
			     "0x77");

	memset(&m, 0, sizeof(m));
	m.nn = nn;
	m.data = (uintptr_t)buf;
	expect_fail(RAMON_IOC_SPFI_MEM_READ, &m, &m.st, "SPFI_MEM_READ size 0", EINVAL,
		    RAMON_E_INVAL_ARG, "size 0");
	m.offset = RAMON_SPFI_MEM_READ_MAX - 4;
	m.size = 8;
	expect_fail(RAMON_IOC_SPFI_MEM_READ, &m, &m.st, "SPFI_MEM_READ past the table", ERANGE,
		    RAMON_E_SPFI_MEM_RANGE, "0x3ffc");
	memset(&t, 0, sizeof(t));
	t.nn = nn;
	t.offset = RAMON_SPFI_TX_OFFS_MAX - 4;
	t.size = 8;
	t.data = (uintptr_t)buf;
	expect_fail(RAMON_IOC_SPFI_TX_OFFS_WRITE, &t, &t.st, "SPFI_TX_OFFS_WRITE past the table",
		    ERANGE, RAMON_E_SPFI_OFFS_RANGE, "0x7ffc");
	errors_spfi_read(nn);

	if (!mbuf_new(&one, SPFI_PAGE)) {
		memset(&it, 0, sizeof(it));
		it.handle = one.h;
		it.len = SPFI_PAGE;
		memset(&w, 0, sizeof(w));
		w.nn = nn;
		w.chan = spfi_chan[nn];
		w.tx_num_offset = 2;
		w.n_items = 1;
		w.items = (uintptr_t)&it;
		expect_fail(RAMON_IOC_SPFI_WRITE, &w, &w.st, "SPFI_WRITE 1 page for 2 offsets",
			    EINVAL, RAMON_E_INVAL_ARG, "tx_num_offset 2");
		mbuf_del(&one);
	}
	if (spfi_link_up(nn)) {
		errors_spfi_cmd_timeout(nn);
		errors_spfi_read_timeout(nn);
	}
}

static void sigusr1(int sig)
{
	(void)sig;
}

/* a signal releases a blocked wait with EINTR */
static void errors_signal(const struct ramon_get_info *gi)
{
	struct blocker b;
	uint32_t nn;

	memset(&b, 0, sizeof(b));
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (gi->spfi_mask & (1u << nn)) {
			b.what = "SPFI_WAIT_ALERT";
			b.req = RAMON_IOC_SPFI_WAIT_ALERT;
			b.u.alert.nn = nn;
			b.st = &b.u.alert.st;
			break;
		}
		if (gi->spw_mask & (1u << nn)) {
			b.what = "SPW_WAIT_RX";
			b.req = RAMON_IOC_SPW_WAIT_RX;
			b.u.spw.nn = nn;
			b.u.spw.timeout_ms = RAMON_TIMEOUT_MAX_MS;
			b.st = &b.u.spw.st;
			break;
		}
	}
	if (!b.req || pthread_create(&b.th, NULL, blocker_thread, &b))
		return;
	sleep_ms(CANCEL_AFTER_MS);
	pthread_kill(b.th, SIGUSR1);
	pthread_join(b.th, NULL);
	if (!b.err)
		printf("      %s returned data before the signal\n", b.what);
	else if (b.err != EINTR || b.st->code != RAMON_E_INTERRUPTED)
		fail("%s + SIGUSR1: errno %d code %s, expected EINTR", b.what, b.err,
		     ramon_err_name(b.st->code));
	else {
		seen(b.st->code);
		pass("%s interrupted by SIGUSR1 -> %s \"%s\"", b.what, ramon_err_name(b.st->code),
		     b.st->msg);
	}
}

/* a copy from an odd address: NOT_ALIGNED if the ZDMA engine needs alignment */
static void errors_align(const struct ramon_get_info *gi)
{
	struct ramon_zdma_copy z;
	struct ramon_copy c;
	struct mbuf a;
	int err;

	if (!gi->n_zdma_chan || mbuf_new(&a, 2 * page_size))
		return;
	memset(&c, 0, sizeof(c));
	c.src_handle = a.h;
	c.dst_handle = a.h;
	c.src_off = 1;
	c.dst_off = page_size;
	c.len = ZDMA_SMALL;
	memset(&z, 0, sizeof(z));
	z.n = 1;
	z.entries = (uintptr_t)&c;
	err = ioctl(fd, RAMON_IOC_ZDMA_COPY, &z) ? errno : 0;
	if (!err)
		printf("      ZDMA copies from odd addresses: NOT_ALIGNED not reachable here\n");
	else if (err != EINVAL || z.st.code != RAMON_E_NOT_ALIGNED)
		fail("ZDMA odd src: errno %d code %s", err, ramon_err_name(z.st.code));
	else {
		seen(z.st.code);
		pass("ZDMA odd src -> %s \"%s\"", ramon_err_name(z.st.code), z.st.msg);
	}
	mbuf_del(&a);
}

/* which RAMON_E_* codes this run produced */
static void errors_coverage(void)
{
	char line[512] = "";
	uint32_t code;
	int n = 0, total = 0;

	for (code = 1; code < 256; code++) {
		if (!ramon_err_lookup(code))
			continue;
		total++;
		if (code_seen[code]) {
			n++;
			continue;
		}
		if (strlen(line) + 40 < sizeof(line)) {
			strcat(line, " ");
			strcat(line, ramon_err_name(code) + strlen("RAMON_E_"));
		}
	}
	printf("      --errors produced %d of %d error codes; not produced:%s\n", n, total, line);
	printf("      (engine failures and races such as AXI/ZDMA PREP/SUBMIT/DMA_ERROR,\n"
	       "       SPFI_UNEXPECTED_OPCODE, SPFI_WRITE_TIMEOUT and NO_MEMORY cannot be\n"
	       "       provoked safely; REMOVED is produced by the unbind test)\n");
}

static void test_errors(void)
{
	struct ramon_get_info gi;

	errors_dispatch();
	if (get_info(&gi))
		return;
	errors_buf_ioctls(gi.max_buf_bytes);
	errors_mmap();
	if (drv_step >= 3)
		errors_regwin(gi.n_regwin);
	if (drv_step >= 4)
		errors_axi(&gi);
	if (drv_step >= 5) {
		errors_spw(&gi);
		if (gi.spw_mask & 1)
			errors_spw_waits();
		errors_axi_timeout(&gi);
	}
	if (drv_step >= 6) {
		errors_zdma(&gi);
		errors_align(&gi);
	}
	if (drv_step >= 7) {
		errors_spfi(&gi);
		errors_signal(&gi);
	}
	errors_cma_exhaustion(gi.max_buf_bytes);
	errors_coverage();
	printf("      check that dmesg shows nothing new for the argument errors above\n");
}

/* ---- main ---- */

/*
 * min_step: the implementation step (driver version 0.<step>.0) that added
 * what the test uses. An older driver answers those ioctls with ENOTTY, so
 * such tests are refused up front with one clear message.
 */
struct test {
	const char *name;
	void (*fn)(void);
	int in_default;
	unsigned int min_step;
};

static const struct test tests[] = {
	{ "info",	test_info,	1, 1 },
	{ "buf",	test_buf,	1, 2 },
	{ "churn",	test_churn,	1, 2 },
	{ "kill",	test_kill,	1, 2 },
	{ "regwin",	test_regwin,	1, 3 },
	{ "chan",	test_chan,	1, 4 },
	{ "spw",	test_spw,	1, 5 },
	{ "zdma",	test_zdma,	1, 6 },
	{ "spfi",	test_spfi,	1, 7 },
	{ "stats",	test_stats,	1, 6 },
	{ "unbind",	test_unbind,	0, 2 },
	{ "--errors",	test_errors,	0, 2 },
	{ "--stress",	test_stress,	0, 7 },
	{ "spwdps",	test_spwdps,	0, 5 },
	{ "spwsend",	test_spwsend,	0, 5 },
	{ "spfiprep",	test_spfiprep,	0, 7 },
	{ "spfidel",	test_spfidel,	0, 7 },
	{ "reg",	test_reg,	0, 3 },
};

/*
 * Prints the tool and driver versions; returns the driver step, or 0 if it
 * cannot be told (a 1.x driver passes every min_step).
 */
static unsigned int driver_step(void)
{
	struct ramon_get_info gi;

	if (get_info(&gi))
		return 0;
	printf("ramon_smoke %s (abi %u), driver %u.%u.%u (abi %u)\n", SMOKE_VERSION,
	       RAMON_ABI_VERSION, gi.drv_major, gi.drv_minor, gi.drv_patch, gi.abi_version);
	return gi.drv_major ? ~0u : gi.drv_minor;
}

#define N_TESTS (sizeof(tests) / sizeof(tests[0]))

static void usage(const char *argv0)
{
	size_t i;

	fprintf(stderr, "usage: %s [-d DEV] [--node N] [--target N] [--spfi-chans A,B] [--spfi-init]\n"
		"       [--format] [--minutes N] [test [args]] | --version\ntests:", argv0);
	for (i = 0; i < N_TESTS; i++)
		fprintf(stderr, " %s", tests[i].name);
	fprintf(stderr, "\nwith no test, runs the default sequence\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *which = NULL;
	unsigned int step;
	size_t i;
	int a;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "--version")) {
			printf("ramon_smoke %s (abi %u)\n", SMOKE_VERSION, RAMON_ABI_VERSION);
			return 0;
		}
		if (!strcmp(argv[a], "-d") && a + 1 < argc)
			dev_path = argv[++a];
		else if (!strcmp(argv[a], "--node") && a + 1 < argc)
			spw_node = strtoul(argv[++a], NULL, 0);
		else if (!strcmp(argv[a], "--target") && a + 1 < argc)
			spw_target = strtoul(argv[++a], NULL, 0);
		else if (!strcmp(argv[a], "--minutes") && a + 1 < argc)
			stress_minutes = strtoul(argv[++a], NULL, 0);
		else if (!strcmp(argv[a], "--spfi-init"))
			spfi_init_first = 1;
		else if (!strcmp(argv[a], "--spfi-chans") && a + 1 < argc &&
			 sscanf(argv[a + 1], "%u,%u", &spfi_chan[0], &spfi_chan[1]) == 2)
			a++;
		else if (!strcmp(argv[a], "--format"))
			spfi_format = 1;
		else if (!which)
			which = argv[a];
		else if (targc < (int)(sizeof(targv) / sizeof(targv[0])))
			targv[targc++] = argv[a];
		else
			usage(argv[0]);
	}

	if (which) {
		for (i = 0; i < N_TESTS && strcmp(which, tests[i].name); i++)
			;
		if (i == N_TESTS)
			usage(argv[0]);
	}

	crc_init();
	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = sigusr1;	/* no SA_RESTART: blocked ioctls return EINTR */
		sigaction(SIGUSR1, &sa, NULL);
	}
	page_size = (size_t)sysconf(_SC_PAGESIZE);
	while ((1UL << page_shift) < page_size)
		page_shift++;
	setvbuf(stdout, NULL, _IOLBF, 0);

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	step = driver_step();
	drv_step = step;
	for (i = 0; i < N_TESTS; i++) {
		if (which ? strcmp(which, tests[i].name) : !tests[i].in_default)
			continue;
		printf("== %s\n", tests[i].name);
		if (step < tests[i].min_step) {
			fail("%s needs driver 0.%u.0 or later; the loaded ramon_dma is 0.%u "
			     "(cat /sys/module/%s/version). Load the matching ramon_dma.ko.",
			     tests[i].name, tests[i].min_step, step, RAMON_DEV_NAME);
			continue;
		}
		tests[i].fn();
	}

	close(fd);
	printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}

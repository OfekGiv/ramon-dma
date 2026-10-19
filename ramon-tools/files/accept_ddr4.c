// SPDX-License-Identifier: MIT
/*
 * ramon_accept: PL DDR4 write/read test (was ddr4test).
 *
 * The PL DDR4 sits behind the PL, outside the kernel's memory, so it is
 * reached through /dev/mem as the old test did. Its base and size come from
 * the device tree (label "ddr4_0", as the old test looked it up) unless given
 * on the command line. The mapping is device memory: only aligned 64-bit
 * volatile accesses, no memset/memcpy.
 */
#define _FILE_OFFSET_BITS 64

#include "accept.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DDR4_LABEL	"ddr4_0"
#define FPGA_STATE	"/sys/class/fpga_manager/fpga0/state"
#define GIB		(1ull << 30)
#define MIB		(1ull << 20)
#define MAX_REPORTED	8	/* mismatches printed per pass */
#define PATH_LEN	256

typedef volatile uint64_t vu64;

struct region {
	uint64_t base, size;
	char label[64];
	char path[PATH_LEN];	/* DT node path, "" when given on the command line */
};

/* ---- device tree ---- */

static int dt_read(const char *node, const char *prop, uint8_t **data, size_t *len)
{
	char path[2 * PATH_LEN];

	snprintf(path, sizeof(path), "%s%s/%s", A.o.dt_root, node, prop);
	return tc_read_file(path, data, len, 4096);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t cells(const uint8_t *p, uint32_t n)
{
	uint64_t v = 0;

	while (n--) {
		v = v << 32 | be32(p);
		p += 4;
	}
	return v;
}

/* #address-cells / #size-cells of the node's parent; the spec defaults are 2 and 1 */
static uint32_t parent_cells(const char *node, const char *prop, uint32_t dflt)
{
	char parent[PATH_LEN];
	uint8_t *d;
	size_t len;
	uint32_t v = dflt;
	char *slash;

	snprintf(parent, sizeof(parent), "%s", node);
	slash = strrchr(parent, '/');
	if (!slash)
		return dflt;
	*slash = 0;	/* "/amba_pl@0/ddr4@..." -> "/amba_pl@0"; "/ddr4@..." -> "" (the root) */
	if (!dt_read(parent, prop, &d, &len)) {
		if (len == 4)
			v = be32(d);
		free(d);
	}
	return v;
}

/* the node path for a __symbols__ label; 0 or -errno */
static int dt_label(const char *label, char *path, size_t len)
{
	uint8_t *d;
	size_t n;
	int ret;

	ret = dt_read("/__symbols__", label, &d, &n);
	if (ret)
		return ret;
	snprintf(path, len, "%.*s", (int)strnlen((char *)d, n), (char *)d);
	free(d);
	return path[0] == '/' ? 0 : -EINVAL;
}

/* the one label containing "ddr4"; reports and fails when there are none or several */
static int dt_search(struct region *r)
{
	char dir[PATH_LEN + 16], path[PATH_LEN], found[4][PATH_LEN];
	unsigned n = 0, i;
	struct dirent *e;
	DIR *d;

	snprintf(dir, sizeof(dir), "%s/__symbols__", A.o.dt_root);
	d = opendir(dir);
	if (!d) {
		tc_fail("%s: %s (no device tree symbols; pass --ddr4-base and --ddr4-size)", dir,
			strerror(errno));
		return -1;
	}
	while ((e = readdir(d))) {
		if (!strstr(e->d_name, "ddr4") || dt_label(e->d_name, path, sizeof(path)))
			continue;
		for (i = 0; i < n && strcmp(found[i], path); i++)
			;
		if (i < n)
			continue;	/* two labels of one node */
		if (n < ARRAY_SIZE(found)) {
			snprintf(found[n], PATH_LEN, "%s", path);
			if (!n)
				snprintf(r->label, sizeof(r->label), "%.63s", e->d_name);
		}
		n++;
	}
	closedir(d);
	if (n == 1) {
		snprintf(r->path, sizeof(r->path), "%s", found[0]);
		return 0;
	}
	if (!n)
		tc_fail("no DT label \"%s\" and none containing \"ddr4\" (--ddr4-label, or "
			"--ddr4-base and --ddr4-size)", DDR4_LABEL);
	else
		tc_fail("%u DT nodes have a label containing \"ddr4\"; choose one with --ddr4-label", n);
	for (i = 0; i < n && i < ARRAY_SIZE(found); i++)
		tc_info("candidate: %s", found[i]);
	return -1;
}

static int find_region(struct region *r)
{
	uint32_t ac, sc;
	uint8_t *d;
	size_t len;
	int ret;

	memset(r, 0, sizeof(*r));
	if (A.o.ddr4_base) {
		r->base = A.o.ddr4_base;
		r->size = A.o.ddr4_size;
		snprintf(r->label, sizeof(r->label), "command line");
		return 0;
	}
	snprintf(r->label, sizeof(r->label), "%s", A.o.ddr4_label ? A.o.ddr4_label : DDR4_LABEL);
	ret = dt_label(r->label, r->path, sizeof(r->path));
	if (ret && A.o.ddr4_label) {
		tc_fail("DT label \"%s\": %s", r->label, strerror(-ret));
		return -1;
	}
	if (ret && dt_search(r))
		return -1;

	ac = parent_cells(r->path, "#address-cells", 2);
	sc = parent_cells(r->path, "#size-cells", 1);
	ret = dt_read(r->path, "reg", &d, &len);
	if (ret) {
		tc_fail("%s: no reg property: %s", r->path, strerror(-ret));
		return -1;
	}
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2 || len < (ac + sc) * 4) {
		tc_fail("%s: reg of %zu bytes with #address-cells %u, #size-cells %u", r->path, len,
			ac, sc);
		free(d);
		return -1;
	}
	r->base = cells(d, ac);
	r->size = cells(d + ac * 4, sc);
	free(d);
	if (!dt_read(r->path, "compatible", &d, &len)) {
		tc_info("DT node %s (label %s, compatible \"%.*s\")", r->path, r->label,
			(int)strnlen((char *)d, len), (char *)d);
		free(d);
	} else {
		tc_info("DT node %s (label %s)", r->path, r->label);
	}
	return 0;
}

/* ---- safety ---- */

/* An access to an unprogrammed PL can hang the CPU or end in an SError panic. */
static int fpga_programmed(void)
{
	char state[64];

	if (tc_read_line(FPGA_STATE, state, sizeof(state))) {
		tc_info("%s not readable: cannot check that the PL is programmed", FPGA_STATE);
		return 1;
	}
	if (strcmp(state, "operating")) {
		tc_fail("FPGA manager state is \"%s\", not \"operating\": the PL is not programmed, "
			"the DDR4 was not touched", state);
		return 0;
	}
	return 1;
}

/* ---- tests ---- */

static unsigned log2u64(uint64_t v)
{
	unsigned n = 0;

	while (v >>= 1)
		n++;
	return n;
}

/* walking ones and zeros on the first word: every data line toggles */
static int data_bus(vu64 *p, uint64_t base)
{
	uint64_t stuck = 0, v;
	unsigned bit;

	for (bit = 0; bit < 64; bit++) {
		p[0] = 1ull << bit;
		v = p[0];
		stuck |= v ^ (1ull << bit);
		p[0] = ~(1ull << bit);
		v = p[0];
		stuck |= v ^ ~(1ull << bit);
	}
	if (stuck)
		tc_fail("data bus at 0x%llx: bits 0x%016llx do not follow the written value",
			(unsigned long long)base, (unsigned long long)stuck);
	else
		tc_info("data bus: 64 walking ones and zeros read back");
	return stuck ? -1 : 0;
}

/*
 * Address lines: one word at each power-of-two offset. A line stuck high or
 * low, or two lines shorted, make two of these words alias.
 */
static int address_bus(vu64 *p, uint64_t words, uint64_t base)
{
	const uint64_t pat = 0xAAAAAAAAAAAAAAAAull, anti = 0x5555555555555555ull;
	uint64_t off, off2;
	int bad = 0;

	for (off = 1; off < words; off <<= 1)
		p[off] = pat;
	p[0] = anti;
	for (off = 1; off < words; off <<= 1)
		if (p[off] != pat) {
			tc_fail("address bit %u (0x%llx) aliases address 0x%llx: stuck",
				log2u64(off * 8), (unsigned long long)(base + off * 8),
				(unsigned long long)base);
			bad = 1;
		}
	p[0] = pat;
	for (off = 1; off < words && !bad; off <<= 1) {
		p[off] = anti;
		if (p[0] != pat) {
			tc_fail("address bit %u (0x%llx) aliases address 0x%llx: stuck",
				log2u64(off * 8), (unsigned long long)(base + off * 8),
				(unsigned long long)base);
			bad = 1;
		}
		for (off2 = 1; off2 < words && !bad; off2 <<= 1)
			if (off2 != off && p[off2] != pat) {
				tc_fail("address bits %u and %u are shorted (0x%llx aliases 0x%llx)",
					log2u64(off * 8), log2u64(off2 * 8),
					(unsigned long long)(base + off * 8),
					(unsigned long long)(base + off2 * 8));
				bad = 1;
			}
		p[off] = pat;
	}
	if (!bad)
		tc_info("address bus: %u address lines, no aliasing", log2u64(words * 8));
	return bad ? -1 : 0;
}

static void progress(const char *what, uint64_t done, uint64_t total, uint64_t t0)
{
	tc_info("  %s %llu / %llu MiB, %.0f MiB/s", what, (unsigned long long)(done / MIB),
		(unsigned long long)(total / MIB), tc_mibps(done, tc_now_ns() - t0));
}

/*
 * Writes every word, then reads every word back. The pattern is the word's
 * own physical address (inverted when invert is set), so an address fault
 * shows as a value from the wrong place.
 */
static int full_pass(vu64 *p, uint64_t words, uint64_t base, int invert, double *w_mibps,
		     double *r_mibps)
{
	const uint64_t mask = invert ? ~0ull : 0, bytes = words * 8;
	const uint64_t step = bytes > 2 * GIB ? GIB / 8 : words + 1;	/* progress lines */
	uint64_t i, t0, bad = 0, want, got;
	const char *name = invert ? "inverted address" : "address";

	t0 = tc_now_ns();
	for (i = 0; i < words; i++) {
		p[i] = (base + i * 8) ^ mask;
		if (i && i % step == 0)
			progress("write", i * 8, bytes, t0);
	}
	*w_mibps = tc_mibps(bytes, tc_now_ns() - t0);

	t0 = tc_now_ns();
	for (i = 0; i < words; i++) {
		want = (base + i * 8) ^ mask;
		got = p[i];
		if (got != want && bad++ < MAX_REPORTED)
			tc_info("  0x%010llx: read 0x%016llx, expected 0x%016llx (bits 0x%016llx)",
				(unsigned long long)(base + i * 8), (unsigned long long)got,
				(unsigned long long)want, (unsigned long long)(got ^ want));
		if (i && i % step == 0)
			progress("read", i * 8, bytes, t0);
	}
	*r_mibps = tc_mibps(bytes, tc_now_ns() - t0);
	if (bad)
		tc_fail("%s pattern: %llu of %llu words wrong", name, (unsigned long long)bad,
			(unsigned long long)words);
	else
		tc_info("%s pattern: %llu MiB written at %.0f MiB/s, read back at %.0f MiB/s", name,
			(unsigned long long)(bytes / MIB), *w_mibps, *r_mibps);
	return bad ? -1 : 0;
}

static void step_ddr4(void)
{
	long page = sysconf(_SC_PAGESIZE);
	double w[2] = { 0, 0 }, r[2] = { 0, 0 };
	uint64_t len, words;
	struct region reg;
	void *map;
	int fd;

	if (find_region(&reg)) {
		a_summary("PL DDR4 region not found");
		return;
	}
	tc_info("PL DDR4: 0x%llx, %llu MiB (from %s)", (unsigned long long)reg.base,
		(unsigned long long)(reg.size / MIB), reg.label);
	len = A.o.ddr4_limit && A.o.ddr4_limit < reg.size ? A.o.ddr4_limit : reg.size;
	len &= ~((uint64_t)page - 1);
	if (!len || reg.base % (uint64_t)page) {
		tc_fail("region 0x%llx + 0x%llx is not page aligned", (unsigned long long)reg.base,
			(unsigned long long)reg.size);
		a_summary("bad region");
		return;
	}
	if (len < reg.size)
		tc_info("testing the first %llu MiB only (--ddr4-mb)", (unsigned long long)(len / MIB));
	if (!fpga_programmed()) {
		a_summary("PL not programmed");
		return;
	}

	fd = open(A.o.mem_dev, O_RDWR | O_SYNC);
	if (fd < 0) {
		tc_fail("open %s: %s%s", A.o.mem_dev, strerror(errno),
			errno == ENOENT ? " (kernel built without CONFIG_DEVMEM?)" :
			errno == EACCES || errno == EPERM ? " (run as root)" : "");
		a_summary("cannot open %s", A.o.mem_dev);
		return;
	}
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)reg.base);
	close(fd);
	if (map == MAP_FAILED) {
		tc_fail("mmap of 0x%llx + %llu MiB through %s: %s%s", (unsigned long long)reg.base,
			(unsigned long long)(len / MIB), A.o.mem_dev, strerror(errno),
			errno == EPERM ? " (refused by CONFIG_STRICT_DEVMEM / CONFIG_IO_STRICT_DEVMEM)" :
			"");
		a_summary("cannot map the PL DDR4");
		return;
	}
	tc_info("overwrites the tested PL DDR4 range; its old contents are lost");

	words = len / 8;
	if (data_bus(map, reg.base) || address_bus(map, words, reg.base)) {
		tc_info("the full-region passes are skipped after a bus fault");
	} else {
		full_pass(map, words, reg.base, 0, &w[0], &r[0]);
		full_pass(map, words, reg.base, 1, &w[1], &r[1]);
	}
	munmap(map, len);
	a_summary("%llu MiB at 0x%llx%s, write %.0f MiB/s, read %.0f MiB/s",
		  (unsigned long long)(len / MIB), (unsigned long long)reg.base,
		  len < reg.size ? " (partial)" : "", (w[0] + w[1]) / 2, (r[0] + r[1]) / 2);
}

const struct step accept_ddr4[] = {
	{ "ddr4", step_ddr4, 0, "ddr4test",
	  "PL DDR4: data bus, address bus, address and inverted patterns over the region", NULL },
	{ NULL, NULL, 0, NULL, NULL, NULL },
};

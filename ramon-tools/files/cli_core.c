// SPDX-License-Identifier: MIT
/* ramon_cli: device, buffers, AXI, ZDMA and register commands */
#include "cli.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZDMA_SEED	0x2D2D0000u

/* ---- device ---- */

static int cmd_version(struct cli *cl, int argc, char **argv)
{
	char line[160], sysfs[64];

	(void)argc;
	(void)argv;
	ramon_version_line(cl->c, line, sizeof(line));
	printf("      ramon_cli %s, %s\n", RAMON_TOOLS_VERSION, line);
	if (!tc_read_line("/sys/module/ramon_dma/version", sysfs, sizeof(sysfs)))
		printf("      /sys/module/ramon_dma/version: %s\n", sysfs);
	return 0;
}

static int cmd_info(struct cli *cl, int argc, char **argv)
{
	const struct ramon_get_info *gi = ramon_info(cl->c);

	(void)argc;
	(void)argv;
	printf("      driver %u.%u.%u, abi %u, page size %u, largest buffer %llu MiB\n",
	       gi->drv_major, gi->drv_minor, gi->drv_patch, gi->abi_version, gi->page_size,
	       (unsigned long long)(gi->max_buf_bytes >> 20));
	printf("      %u AXI channels, %u ZDMA channels, %u register windows\n", gi->n_axi_chan,
	       gi->n_zdma_chan, gi->n_regwin);
	printf("      spw_mask 0x%x, spfi_mask 0x%x\n", gi->spw_mask, gi->spfi_mask);
	return 0;
}

static int cmd_reopen(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	cli_close_ctx(cl);
	return cli_ctx(cl);
}

static int cmd_stats(struct cli *cl, int argc, char **argv)
{
	struct ramon_get_stats g;
	uint32_t reset, nn, i;
	int ret;

	ret = cli_opt_u32(cl, argc, argv, 1, 0, "reset", &reset);
	if (!ret)
		ret = ramon_get_stats(cl->c, reset, &g, &cl->st);
	if (ret)
		return ret;
	printf("      axi: tx %llu rx %llu timeouts %llu; zdma: ops %llu timeouts %llu\n",
	       (unsigned long long)g.axi_tx, (unsigned long long)g.axi_rx,
	       (unsigned long long)g.axi_timeouts, (unsigned long long)g.zdma_ops,
	       (unsigned long long)g.zdma_timeouts);
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		printf("      nn%u: spw rx %llu overrun %llu; spfi alert overrun %llu unexpected %llu timeouts %llu; irq vectors:",
		       nn, (unsigned long long)g.spw_rx[nn], (unsigned long long)g.spw_overrun[nn],
		       (unsigned long long)g.spfi_alert_overrun[nn],
		       (unsigned long long)g.spfi_unexpected[nn],
		       (unsigned long long)g.spfi_timeouts[nn]);
		for (i = 0; i < RAMON_SPFI_IRQ_VECTORS; i++)
			if (g.spfi_irq_hist[nn][i])
				printf(" 0x%x:%llu", i, (unsigned long long)g.spfi_irq_hist[nn][i]);
		printf("\n");
	}
	if (reset)
		printf("      counters reset (for every user of the device)\n");
	return 0;
}

static const char *chan_dir(uint32_t dir)
{
	return dir == RAMON_DIR_MEM_TO_DEV ? "MEM_TO_DEV" :
	       dir == RAMON_DIR_DEV_TO_MEM ? "DEV_TO_MEM" : "MEMCPY";
}

static int cmd_chan(struct cli *cl, int argc, char **argv)
{
	const struct ramon_get_info *gi = ramon_info(cl->c);
	struct ramon_chan_info ci;
	uint32_t i;
	int ret;

	(void)argc;
	(void)argv;
	for (i = 0; i < gi->n_axi_chan + gi->n_zdma_chan; i++) {
		ret = ramon_chan_info(cl->c, i, &ci, &cl->st);
		if (ret)
			return ret;
		ci.name[RAMON_NAME_LEN - 1] = 0;
		if (ci.type == RAMON_CHAN_AXI)
			printf("      %2u AXI  %-10s device-id %u  axi_dma 0x%llx  %s\n", i,
			       chan_dir(ci.dir), ci.device_id, (unsigned long long)ci.phys, ci.name);
		else
			printf("      %2u ZDMA %-10s %s\n", i, chan_dir(ci.dir), ci.name);
	}
	return 0;
}

/* ---- registers ---- */

int cli_regio(struct cli *cl, const char *win, uint32_t index, int by_index, uint32_t off,
	      int write, uint32_t val)
{
	char label[48];
	uint32_t v;
	int ret;

	if (by_index)
		snprintf(label, sizeof(label), "window %u", index);
	else
		snprintf(label, sizeof(label), "%s", win);
	if (write) {
		ret = by_index ? ramon_reg_write_idx(cl->c, index, off, val, &cl->st) :
				 ramon_reg_write(cl->c, win, off, val, &cl->st);
		if (ret)
			return ret;
		printf("      %s+0x%x <- 0x%08x\n", label, off, val);
	}
	ret = by_index ? ramon_reg_read_idx(cl->c, index, off, &v, &cl->st) :
			 ramon_reg_read(cl->c, win, off, &v, &cl->st);
	if (!ret)
		printf("      %s+0x%x = 0x%08x (%u)\n", label, off, v, v);
	return ret;
}

/* argv[i] = offset, argv[i + 1] = optional value */
static int regio_args(struct cli *cl, int argc, char **argv, int i, const char *win, uint32_t index,
		      int by_index)
{
	uint32_t off, val = 0;
	int ret;

	ret = cli_u32(cl, argv[i], "offset", &off);
	if (!ret && i + 1 < argc)
		ret = cli_u32(cl, argv[i + 1], "value", &val);
	if (ret)
		return ret;
	return cli_regio(cl, win, index, by_index, off, i + 1 < argc, val);
}

static int cmd_regio(struct cli *cl, int argc, char **argv)
{
	uint32_t index;
	int ret;

	if (isdigit((unsigned char)argv[1][0])) {
		ret = cli_u32(cl, argv[1], "window index", &index);
		return ret ? ret : regio_args(cl, argc, argv, 2, NULL, index, 1);
	}
	return regio_args(cl, argc, argv, 2, argv[1], 0, 0);
}

static int cmd_rstopregio(struct cli *cl, int argc, char **argv)
{
	return regio_args(cl, argc, argv, 1, "rs_top", 0, 0);
}

static int cmd_regaccessio(struct cli *cl, int argc, char **argv)
{
	uint32_t i;
	int ret = cli_u32(cl, argv[1], "reg-access index", &i);

	return ret ? ret : regio_args(cl, argc, argv, 2, NULL, RAMON_WIN_FIXED + i, 1);
}

static int cmd_regaccessioname(struct cli *cl, int argc, char **argv)
{
	return regio_args(cl, argc, argv, 2, argv[1], 0, 0);
}

static int cmd_regwins(struct cli *cl, int argc, char **argv)
{
	struct ramon_regwin_info ri;
	uint32_t i;
	int ret;

	(void)argc;
	(void)argv;
	for (i = 0; i < ramon_info(cl->c)->n_regwin; i++) {
		ret = ramon_regwin_info(cl->c, i, &ri, &cl->st);
		if (ret)
			return ret;
		ri.name[RAMON_NAME_LEN - 1] = 0;
		printf("      %2u %-24s phys 0x%010llx size 0x%-5x%s%s\n", i, ri.name,
		       (unsigned long long)ri.phys, ri.size,
		       ri.flags & RAMON_REGWIN_RO ? " read-only" : "",
		       ri.flags & RAMON_REGWIN_ABSENT ? " absent" : "");
	}
	return 0;
}

static int cmd_rdrstopver(struct cli *cl, int argc, char **argv)
{
	uint32_t v[3];
	int ret;

	(void)argc;
	(void)argv;
	ret = ramon_rstop_version(cl->c, v, &cl->st);
	if (!ret)
		printf("      RS TOP version: %u.%u.%u\n", v[0], v[1], v[2]);
	return ret;
}

static int cmd_readtimetag(struct cli *cl, int argc, char **argv)
{
	struct ramon_rstop_time t;
	uint32_t v;
	int ret;

	(void)argc;
	(void)argv;
	ret = ramon_reg_read(cl->c, "rs_top", RAMON_RSTOP_TIMESTAMP, &v, &cl->st);
	if (ret)
		return ret;
	ramon_rstop_decode_time(v, &t);
	printf("      %02u/%02u/%02u - %02u:%02u:%02u (0x%08x)\n", t.day, t.month, t.year, t.hour,
	       t.min, t.sec, v);
	return 0;
}

static int cmd_sysmon(struct cli *cl, int argc, char **argv)
{
	struct ramon_sysmon s;
	unsigned i;
	int ret;

	(void)argc;
	(void)argv;
	ret = ramon_sysmon_read(cl->c, &s, &cl->st);
	if (ret)
		return ret;
	printf("      PS temperature: %.2f C\n      PL temperature: %.2f C\n", s.ps_temp_c,
	       s.pl_temp_c);
	for (i = 0; i < RAMON_SYSMON_RAILS; i++) {
		const struct ramon_sysmon_rail *r = &ramon_sysmon_rails[i];

		if (i == 0 || i == 10)
			printf("      %s readings:\n", i ? "PL" : "PS");
		printf("      0x%04X : %6.3f V | %-12s | %s\n", r->offset, s.volts[i], r->name,
		       r->desc);
	}
	return 0;
}

static int cmd_rtcttc(struct cli *cl, int argc, char **argv)
{
	char win[8];
	uint32_t ttc, rtc, v;
	int ret;

	ret = cli_opt_u32(cl, argc, argv, 1, 0, "ttc", &ttc);
	if (!ret && ttc >= RAMON_TTC_COUNT)
		ret = cli_err(cl, EINVAL, "ttc must be 0..%u", RAMON_TTC_COUNT - 1);
	if (!ret)
		ret = ramon_reg_read(cl->c, "rtc", 0x10, &rtc, &cl->st);
	snprintf(win, sizeof(win), "ttc%u", ttc);
	if (!ret)
		ret = ramon_reg_read(cl->c, win, 0x0C, &v, &cl->st);
	if (!ret)
		printf("      RTC current time: %u\n      TTC%u counter control 1: 0x%02X\n", rtc, ttc,
		       v);
	return ret;
}

/* ---- buffers ---- */

static int cmd_alloc(struct cli *cl, int argc, char **argv)
{
	uint64_t size, dma;
	uint32_t count, n = 0, i;
	int ret;

	ret = cli_u64(cl, argv[1], "size", &size);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 2, 1, "count", &count);
	if (ret)
		return ret;
	for (i = 0; i < CLI_SLOTS && n < count; i++) {
		if (cl->slot[i].handle)
			continue;
		ret = ramon_buf_alloc(cl->c, size, 0, &cl->slot[i], &cl->st);
		if (ret)
			return ret;
		ramon_buf_dma_addr(&cl->slot[i], &dma, NULL);
		printf("      slot %u: handle %u, %llu bytes, dma 0x%llx\n", i, cl->slot[i].handle,
		       (unsigned long long)cl->slot[i].size, (unsigned long long)dma);
		n++;
	}
	if (n < count)
		return cli_err(cl, ENOSPC, "only %u of %u allocated: all %u slots in use", n, count,
			       CLI_SLOTS);
	return 0;
}

static int cmd_free(struct cli *cl, int argc, char **argv)
{
	ramon_buf *b;
	unsigned i;
	int ret;

	(void)argc;
	if (!strcmp(argv[1], "all")) {
		for (i = 0; i < CLI_SLOTS; i++)
			ramon_buf_free(&cl->slot[i], NULL);
		return 0;
	}
	ret = cli_slot(cl, argv[1], &b);
	return ret ? ret : ramon_buf_free(b, &cl->st);
}

static int cmd_bufinfo(struct cli *cl, int argc, char **argv)
{
	uint64_t dma;
	unsigned i, n = 0;

	(void)argc;
	(void)argv;
	for (i = 0; i < CLI_SLOTS; i++) {
		if (!cl->slot[i].handle)
			continue;
		ramon_buf_dma_addr(&cl->slot[i], &dma, NULL);
		printf("      slot %u: handle %u, %llu bytes, dma 0x%llx\n", i, cl->slot[i].handle,
		       (unsigned long long)cl->slot[i].size, (unsigned long long)dma);
		n++;
	}
	printf("      %u slot(s) in use; CmaFree %ld kB\n", n, tc_meminfo_kb("CmaFree"));
	return 0;
}

/* <slot> [seed] [len] */
static int slot_seed_len(struct cli *cl, int argc, char **argv, ramon_buf **b, uint32_t *seed,
			 uint64_t *len)
{
	int ret = cli_slot(cl, argv[1], b);

	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 2, 0, "seed", seed);
	*len = ret ? 0 : (*b)->size;
	if (!ret && argc > 3)
		ret = cli_u64(cl, argv[3], "len", len);
	if (!ret && *len > (*b)->size)
		ret = cli_err(cl, ERANGE, "len %llu > buffer size %llu", (unsigned long long)*len,
			      (unsigned long long)(*b)->size);
	return ret;
}

static int cmd_fill(struct cli *cl, int argc, char **argv)
{
	ramon_buf *b;
	uint32_t seed;
	uint64_t len;
	int ret = slot_seed_len(cl, argc, argv, &b, &seed, &len);

	if (!ret)
		tc_fill(b->ptr, len, seed);
	return ret;
}

static int cmd_verify(struct cli *cl, int argc, char **argv)
{
	ramon_buf *b;
	uint32_t seed;
	uint64_t len;
	long bad;
	int ret = slot_seed_len(cl, argc, argv, &b, &seed, &len);

	if (ret)
		return ret;
	bad = tc_verify(b->ptr, len, seed);
	if (bad >= 0)
		return cli_err(cl, EIO, "pattern %#x differs at byte %ld", seed, bad);
	printf("      %llu bytes match pattern %#x\n", (unsigned long long)len, seed);
	return 0;
}

static int cmd_dump(struct cli *cl, int argc, char **argv)
{
	ramon_buf *b;
	uint64_t off = 0, len = 256;
	int ret = cli_slot(cl, argv[1], &b);

	if (!ret && argc > 2)
		ret = cli_u64(cl, argv[2], "offset", &off);
	if (!ret && argc > 3)
		ret = cli_u64(cl, argv[3], "len", &len);
	if (!ret && off >= b->size)
		ret = cli_err(cl, ERANGE, "offset beyond the buffer");
	if (ret)
		return ret;
	if (len > b->size - off)
		len = b->size - off;
	tc_hexdump((uint8_t *)b->ptr + off, len, off, len);
	return 0;
}

static int cmd_zdma(struct cli *cl, int argc, char **argv)
{
	ramon_buf *src, *dst;
	uint64_t len, t0, ns;
	uint32_t count, i;
	long bad;
	int ret;

	ret = cli_slot(cl, argv[1], &src);
	if (!ret)
		ret = cli_slot(cl, argv[2], &dst);
	if (ret)
		return ret;
	len = src->size < dst->size ? src->size : dst->size;
	if (argc > 3)
		ret = cli_u64(cl, argv[3], "len", &len);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 4, 1, "count", &count);
	if (!ret && (len > src->size || len > dst->size))
		ret = cli_err(cl, ERANGE, "len larger than a buffer");
	if (ret)
		return ret;
	tc_fill(src->ptr, len, ZDMA_SEED);
	memset(dst->ptr, 0, len);
	t0 = tc_now_ns();
	for (i = 0; i < count && !cli_intr(); i++) {
		ret = ramon_zdma_copy1(cl->c, src, 0, dst, 0, len, 0, &cl->st);
		if (ret)
			return ret;
	}
	ns = tc_now_ns() - t0;
	bad = tc_verify(dst->ptr, len, ZDMA_SEED);
	printf("      %u copies of %llu bytes, %.1f MiB/s\n", i, (unsigned long long)len,
	       tc_mibps(len * i, ns));
	if (bad >= 0)
		return cli_err(cl, EIO, "destination differs at byte %ld", bad);
	return 0;
}

static int cmd_zdmalist(struct cli *cl, int argc, char **argv)
{
	struct ramon_copy *e = NULL;
	ramon_buf src, dst;
	uint64_t size, t0, ns;
	uint32_t n, count, i, done;
	int ret;

	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	ret = cli_u32(cl, argv[1], "n", &n);
	if (!ret)
		ret = cli_u64(cl, argv[2], "size", &size);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 3, 1, "count", &count);
	if (!ret && (!n || n > RAMON_ZDMA_MAX_COPIES || !size))
		ret = cli_err(cl, EINVAL, "n must be 1..%u and size > 0", RAMON_ZDMA_MAX_COPIES);
	if (ret)
		return ret;
	e = calloc(n, sizeof(*e));
	if (!e)
		return cli_err(cl, ENOMEM, "out of memory");
	ret = ramon_buf_alloc(cl->c, n * size, 0, &src, &cl->st);
	if (!ret)
		ret = ramon_buf_alloc(cl->c, n * size, 0, &dst, &cl->st);
	if (ret)
		goto out;
	/* entry i copies chunk i to chunk n-1-i */
	for (i = 0; i < n; i++) {
		e[i].src_handle = src.handle;
		e[i].dst_handle = dst.handle;
		e[i].src_off = i * size;
		e[i].dst_off = (n - 1 - i) * size;
		e[i].len = size;
	}
	tc_fill(src.ptr, n * size, ZDMA_SEED);
	memset(dst.ptr, 0, n * size);
	t0 = tc_now_ns();
	for (i = 0; i < count && !ret && !cli_intr(); i++)
		ret = ramon_zdma_copy(cl->c, e, n, 0, &done, &cl->st);
	ns = tc_now_ns() - t0;
	if (ret)
		goto out;
	for (i = 0; i < n; i++)
		if (memcmp((uint8_t *)dst.ptr + (n - 1 - i) * size, (uint8_t *)src.ptr + i * size,
			   size)) {
			ret = cli_err(cl, EIO, "entry %u copied wrongly", i);
			goto out;
		}
	printf("      %u lists of %u x %llu bytes, %.1f MiB/s\n", count, n,
	       (unsigned long long)size, tc_mibps(n * size * count, ns));
out:
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
	free(e);
	return ret;
}

static int cmd_axiraw(struct cli *cl, int argc, char **argv)
{
	uint32_t chan, timeout;
	uint64_t off, len, t0;
	ramon_buf *b;
	int ret;

	ret = cli_u32(cl, argv[1], "chan", &chan);
	if (!ret)
		ret = cli_slot(cl, argv[2], &b);
	if (!ret)
		ret = cli_u64(cl, argv[3], "offset", &off);
	if (!ret)
		ret = cli_u64(cl, argv[4], "len", &len);
	if (!ret)
		ret = cli_opt_u32(cl, argc, argv, 5, 0, "timeout_ms", &timeout);
	if (ret)
		return ret;
	t0 = tc_now_ns();
	ret = ramon_axi_xfer1(cl->c, chan, b, off, len, timeout, &cl->st);
	if (!ret)
		printf("      %llu bytes on channel %u in %.3f ms\n", (unsigned long long)len, chan,
		       tc_ms_since(t0));
	return ret;
}

const struct cli_cmd cli_core_cmds[] = {
	{ "version", cmd_version, 0, 0, 0, "", "tool, library and driver versions" },
	{ "getdrvver", cmd_version, 0, 0, CMD_HIDDEN, "", "old name of version" },
	{ "info", cmd_info, 0, 0, 0, "", "driver and board summary (GET_INFO)" },
	{ "reopen", cmd_reopen, 0, 0, 0, "", "close and reopen the device (frees every buffer)" },
	{ "stats", cmd_stats, 0, 1, 0, "[reset]", "driver counters; reset 1 zeroes them" },
	{ "chan", cmd_chan, 0, 0, 0, "", "AXI and ZDMA channels (old xinfo)" },
	{ "xinfo", cmd_chan, 0, 0, CMD_HIDDEN, "", "old name of chan" },
	{ "regwins", cmd_regwins, 0, 0, 0, "", "register windows" },
	{ "regio", cmd_regio, 2, 3, 0, "<win|index> <off> [val]",
	  "read a register, or write it and read it back" },
	{ "rstopregio", cmd_rstopregio, 1, 2, 0, "<off> [val]", "RS-TOP register" },
	{ "regaccessio", cmd_regaccessio, 2, 3, 0, "<index> <off> [val]",
	  "register of reg-access device <index>" },
	{ "regaccessioname", cmd_regaccessioname, 2, 3, 0, "<name> <off> [val]",
	  "register of the window called <name>" },
	{ "rdrstopver", cmd_rdrstopver, 0, 0, 0, "", "RS-TOP version" },
	{ "readtimetag", cmd_readtimetag, 0, 0, 0, "", "RS-TOP time stamp" },
	{ "sysmon", cmd_sysmon, 0, 0, 0, "", "temperatures and the 37 voltage rails" },
	{ "rtcttc", cmd_rtcttc, 0, 1, 0, "[ttc]", "RTC time and a TTC counter control register" },
	{ "alloc", cmd_alloc, 1, 2, 0, "<size> [count]", "DMA buffers into free slots" },
	{ "free", cmd_free, 1, 1, 0, "<slot|all>", "free DMA buffers" },
	{ "bufinfo", cmd_bufinfo, 0, 0, 0, "", "list the buffer slots" },
	{ "fill", cmd_fill, 1, 3, 0, "<slot> [seed] [len]", "fill a buffer with the test pattern" },
	{ "verify", cmd_verify, 1, 3, 0, "<slot> [seed] [len]", "check a buffer against the pattern" },
	{ "dump", cmd_dump, 1, 3, 0, "<slot> [off] [len]", "hex dump (default 256 bytes)" },
	{ "zdma", cmd_zdma, 2, 4, 0, "<src> <dst> [len] [count]",
	  "ZDMA copy between slots, verified, with MiB/s (old dma)" },
	{ "dma", cmd_zdma, 2, 4, CMD_HIDDEN, "<src> <dst> [len] [count]", "old name of zdma" },
	{ "zdmalist", cmd_zdmalist, 2, 3, 0, "<n> <size> [count]",
	  "ZDMA copy list of n entries (old lalloc/ldma)" },
	{ "axiraw", cmd_axiraw, 4, 5, 0, "<chan> <slot> <off> <len> [timeout_ms]",
	  "one raw AXI transfer" },
	{ NULL, NULL, 0, 0, 0, NULL, NULL },
};

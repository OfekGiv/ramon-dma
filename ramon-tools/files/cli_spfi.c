// SPDX-License-Identifier: MIT
/* ramon_cli: SPFI commands; they act on the current NN (spfinnid, --nn) */
#include "cli.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPFI_PAGE_BYTES	RAMON_SPFI_PAGE
#define GSEND_SEED	0x53460000u	/* spfigsend pattern: GSEND_SEED | sid */
#define RXTX_READ_SEED	0x52520000u

static struct ramon_spfi_table *table_new(struct cli *cl)
{
	struct ramon_spfi_table *t = malloc(sizeof(*t));

	if (!t)
		cli_set_err(cl, ENOMEM, "out of memory");
	return t;
}

static int get_table(struct cli *cl, struct ramon_spfi_table *t)
{
	return ramon_spfi_get_table(cl->c, cl->spfi_nn, t, &cl->st);
}

static void print_reply(const struct ramon_spfi_reply *r)
{
	printf("      rx_opcode 0x%x rx_err_code %u rx_stream_id %u rx_offset %u init_info 0x%x tod %u rx_status 0x%x\n",
	       r->rx_opcode, r->rx_err_code, r->rx_stream_id, r->rx_offset, r->rx_init_info,
	       r->rx_curr_tod, r->rx_status);
}

/* "auto" = the highest free usable id */
static int parse_sid(struct cli *cl, const char *s, uint32_t *sid)
{
	struct ramon_spfi_table *t;
	int ret;

	if (strcmp(s, "auto"))
		return cli_parse_u32(cl, s, "stream id", sid);
	t = table_new(cl);
	if (!t)
		return -ENOMEM;
	ret = get_table(cl, t);
	if (!ret && ramon_spfi_find_free(t, sid, 1) != 1)
		ret = cli_set_err(cl, ENOSPC, "no free stream id");
	if (!ret)
		printf("      using free stream id %u\n", *sid);
	free(t);
	return ret;
}

static int cmd_spfidrvinit(struct cli *cl, int argc, char **argv)
{
	if (argc > 1 && tc_parse_pair(argv[1], cl->spfi_chan))
		return cli_set_err(cl, EINVAL, "channels are written A,B");
	ramon_spfi_fini(cl->c);
	return cli_init_spfi(cl);
}

static int cmd_spfidrvterm(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	ramon_spfi_fini(cl->c);
	return 0;
}

static int cmd_spfinnid(struct cli *cl, int argc, char **argv)
{
	uint32_t nn;
	int ret = cli_parse_u32(cl, argv[1], "nn", &nn);

	(void)argc;
	if (!ret)
		ret = cli_check_nn(cl, nn);
	if (!ret) {
		cl->spfi_nn = nn;
		if (!(ramon_info(cl->c)->spfi_mask & (1u << nn)))
			printf("      note: spfi%u is masked off by the driver (spfi_mask 0x%x)\n", nn,
			       ramon_info(cl->c)->spfi_mask);
	}
	return ret;
}

static int cmd_spfiloop(struct cli *cl, int argc, char **argv)
{
	char win[8];
	uint32_t en;
	int ret = cli_parse_u32(cl, argv[1], "enable", &en);

	(void)argc;
	if (ret)
		return ret;
	snprintf(win, sizeof(win), "spfi%u", cl->spfi_nn);
	return cli_access_reg(cl, win, 0, 0, RAMON_SPFI_REG_LOOPBACK, 1, en);
}

static int cmd_spfiinit(struct cli *cl, int argc, char **argv)
{
	struct ramon_spfi_reply r;
	uint32_t type;
	int ret = cli_parse_opt_u32(cl, argc, argv, 1, 0, "init_type", &type);

	if (!ret)
		ret = cli_init_spfi(cl);
	if (!ret)
		ret = ramon_spfi_init_nn(cl->c, cl->spfi_nn, type, &r, &cl->st);
	if (!ret) {
		print_reply(&r);
		if (type == 0 && r.rx_err_code == 1 && r.rx_init_info == 0x2)
			printf("      (rx_err_code 1 / init_info 0x2 is the normal answer on this board)\n");
	}
	return ret;
}

static int simple_op(struct cli *cl, int (*fn)(ramon_ctx *, uint32_t, struct ramon_spfi_reply *,
					      struct ramon_status *))
{
	struct ramon_spfi_reply r;
	int ret = cli_init_spfi(cl);

	if (!ret)
		ret = fn(cl->c, cl->spfi_nn, &r, &cl->st);
	if (!ret)
		print_reply(&r);
	return ret;
}

static int cmd_spfifmt(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return simple_op(cl, ramon_spfi_format);
}

static int cmd_spfipdown(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return simple_op(cl, ramon_spfi_power_down);
}

static int stream_op(struct cli *cl, const char *s,
		     int (*fn)(ramon_ctx *, uint32_t, uint32_t, struct ramon_spfi_reply *,
			       struct ramon_status *))
{
	struct ramon_spfi_reply r;
	uint32_t sid;
	int ret = cli_init_spfi(cl);

	if (!ret)
		ret = cli_parse_u32(cl, s, "stream id", &sid);
	if (!ret)
		ret = fn(cl->c, cl->spfi_nn, sid, &r, &cl->st);
	if (!ret)
		print_reply(&r);
	return ret;
}

static int cmd_spficlose(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	return stream_op(cl, argv[1], ramon_spfi_close);
}

static int cmd_spfidel(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	return stream_op(cl, argv[1], ramon_spfi_delete);
}

static int cmd_spfiflush(struct cli *cl, int argc, char **argv)
{
	(void)argc;
	return stream_op(cl, argv[1], ramon_spfi_flush);
}

static int cmd_spfistod(struct cli *cl, int argc, char **argv)
{
	struct ramon_spfi_reply r;
	uint32_t tod;
	int ret = cli_parse_u32(cl, argv[1], "tod", &tod);

	(void)argc;
	if (!ret)
		ret = cli_init_spfi(cl);
	if (!ret)
		ret = ramon_spfi_set_tod(cl->c, cl->spfi_nn, tod, &r, &cl->st);
	if (!ret)
		print_reply(&r);
	return ret;
}

static int cmd_spfiopens(struct cli *cl, int argc, char **argv)
{
	struct ramon_spfi_reply r;
	uint32_t sid, cyclic, last;
	int ret;

	ret = cli_parse_u32(cl, argv[1], "stream id", &sid);
	if (!ret)
		ret = cli_parse_opt_u32(cl, argc, argv, 2, 0, "cyclic", &cyclic);
	if (!ret)
		ret = cli_parse_opt_u32(cl, argc, argv, 3, 0, "last_offset", &last);
	if (!ret && cyclic)
		ret = cli_set_err(cl, EINVAL, "cyclic streams are not supported by the NN");
	if (!ret)
		ret = cli_init_spfi(cl);
	if (!ret)
		ret = ramon_spfi_open(cl->c, cl->spfi_nn, sid, last, &r, &cl->st);
	if (!ret)
		print_reply(&r);
	return ret;
}

static int cmd_spfiginfo(struct cli *cl, int argc, char **argv)
{
	struct ramon_spfi_table *t;
	uint32_t disp, sid;
	int ret = cli_parse_opt_u32(cl, argc, argv, 1, 1, "display", &disp);

	if (ret || !(t = table_new(cl)))
		return ret ? ret : -ENOMEM;
	ret = cli_init_spfi(cl);
	if (!ret)
		ret = get_table(cl, t);
	if (!ret) {
		printf("      spfi%u: media %u, allocated %u, used %u; quality %u/%u; %u stream(s) exist, %u open\n",
		       cl->spfi_nn, t->hdr.total_media_size, t->hdr.total_allocated_size,
		       t->hdr.total_used_size, t->hdr.dynamic_quality, t->hdr.static_quality,
		       ramon_spfi_count_exist(t), ramon_spfi_count_open(t));
		for (sid = 0; disp != 2 && sid < RAMON_SPFI_STREAMS; sid++) {
			const struct ramon_spfi_record *r = &t->rec[sid];

			if (disp != 3 && !(r->status & RAMON_SPFI_ST_EXIST))
				continue;
			printf("      stream %3u: status 0x%02x %-6s latest_write_offs %d latest_read_offs %d size_thresh %u alloc %u%s\n",
			       sid, r->status, r->status & RAMON_SPFI_ST_OPEN ? "open" : "closed",
			       (int)r->latest_write_offs, (int)r->latest_read_offs, r->size_thresh,
			       r->alloc_size, sid >= RAMON_SPFI_STREAMS_USABLE ? " (reserved id)" : "");
		}
	}
	free(t);
	return ret;
}

/* first page: a number, or "auto" = latest_write_offs + 1 */
static int parse_first_page(struct cli *cl, int argc, char **argv, int i, uint32_t sid,
			    uint32_t *first)
{
	struct ramon_spfi_table *t;
	int ret;

	if (i < argc && strcmp(argv[i], "auto"))
		return cli_parse_u32(cl, argv[i], "first page", first);
	t = table_new(cl);
	if (!t)
		return -ENOMEM;
	ret = get_table(cl, t);
	if (!ret)
		*first = t->rec[sid < RAMON_SPFI_STREAMS ? sid : 0].latest_write_offs + 1;
	free(t);
	return ret;
}

static int cmd_spfigsend(struct cli *cl, int argc, char **argv)
{
	uint32_t sid, pages, first;
	ramon_buf b;
	int ret;

	memset(&b, 0, sizeof(b));
	ret = cli_init_spfi(cl);
	if (!ret)
		ret = cli_parse_u32(cl, argv[1], "stream id", &sid);
	if (!ret)
		ret = cli_parse_u32(cl, argv[2], "pages", &pages);
	if (!ret && (!pages || pages > RAMON_SPFI_WRITE_MAX_PAGES))
		ret = cli_set_err(cl, EINVAL, "pages is 1..%u", RAMON_SPFI_WRITE_MAX_PAGES);
	if (!ret)
		ret = parse_first_page(cl, argc, argv, 3, sid, &first);
	if (!ret)
		ret = ramon_buf_alloc(cl->c, (uint64_t)pages * SPFI_PAGE_BYTES, 0, &b, &cl->st);
	if (ret)
		return ret;
	tc_fill(b.ptr, (size_t)pages * SPFI_PAGE_BYTES, GSEND_SEED | sid);
	ret = ramon_spfi_write_buf(cl->c, cl->spfi_nn, sid, first, &b, 0, pages, 0, &cl->st);
	if (!ret)
		printf("      stream %u pages %u..%u written, CRC32 0x%08x (pattern 0x%x)\n", sid, first,
		       first + pages - 1, ramon_crc32(b.ptr, (size_t)pages * SPFI_PAGE_BYTES),
		       GSEND_SEED | sid);
	ramon_buf_free(&b, NULL);
	return ret;
}

static int cmd_spfircv(struct cli *cl, int argc, char **argv)
{
	uint32_t sid, first, pages;
	uint64_t dump_off = 0, dump_size = 0;
	ramon_buf b;
	int ret;

	memset(&b, 0, sizeof(b));
	ret = cli_init_spfi(cl);
	if (!ret)
		ret = cli_parse_u32(cl, argv[1], "stream id", &sid);
	if (!ret)
		ret = cli_parse_u32(cl, argv[2], "start page", &first);
	if (!ret)
		ret = cli_parse_u32(cl, argv[3], "pages", &pages);
	if (!ret && argc > 4)
		ret = cli_parse_u64(cl, argv[4], "dump offset", &dump_off);
	if (!ret && argc > 5)
		ret = cli_parse_u64(cl, argv[5], "dump size", &dump_size);
	if (!ret && (!pages || pages > RAMON_SPFI_READ_MAX_PAGES))
		ret = cli_set_err(cl, EINVAL, "pages is 1..%u", RAMON_SPFI_READ_MAX_PAGES);
	if (!ret)
		ret = ramon_buf_alloc(cl->c, (uint64_t)pages * SPFI_PAGE_BYTES, 0, &b, &cl->st);
	if (ret)
		return ret;
	ret = ramon_spfi_read_seq(cl->c, cl->spfi_nn, sid, first, pages, &b, 0, 0, &cl->st);
	if (!ret) {
		long bad = tc_find_mismatch(b.ptr, (size_t)pages * SPFI_PAGE_BYTES,
					    GSEND_SEED | sid);

		printf("      stream %u pages %u..%u read, CRC32 0x%08x; %s\n", sid, first,
		       first + pages - 1, ramon_crc32(b.ptr, (size_t)pages * SPFI_PAGE_BYTES),
		       bad < 0 ? "matches the spfigsend pattern" : "not the spfigsend pattern");
		if (dump_size && dump_off < b.size)
			tc_hexdump((uint8_t *)b.ptr + dump_off, dump_size, dump_off,
				   b.size - dump_off);
	}
	ramon_buf_free(&b, NULL);
	return ret;
}

static int cmd_spfireg(struct cli *cl, int argc, char **argv)
{
	uint32_t off, val = 0;
	char win[8];
	int ret = cli_parse_u32(cl, argv[1], "offset", &off);

	if (!ret && argc > 2)
		ret = cli_parse_u32(cl, argv[2], "value", &val);
	if (ret)
		return ret;
	snprintf(win, sizeof(win), "spfi%u", cl->spfi_nn);
	return cli_access_reg(cl, win, 0, 0, off, argc > 2, val);
}

static int cmd_spfiinfo(struct cli *cl, int argc, char **argv)
{
	struct ramon_spfi_diag d;
	char line[512];
	uint32_t raw;
	int up, ret;

	(void)argc;
	(void)argv;
	up = ramon_spfi_link_up(cl->c, cl->spfi_nn, &raw, &cl->st);
	if (up < 0)
		return up;
	ret = ramon_spfi_diag(cl->c, cl->spfi_nn, &d, &cl->st);
	if (ret)
		return ret;
	ramon_spfi_diag_format(&d, line, sizeof(line));
	printf("      spfi%u link %s (0xCC = 0x%x)\n      %s\n", cl->spfi_nn, up ? "up" : "DOWN",
	       raw, line);
	return 0;
}

static int cmd_spfitxoffs(struct cli *cl, int argc, char **argv)
{
	uint32_t off, count, nn, *w, i;
	int ret;

	ret = cli_parse_u32(cl, argv[1], "offset", &off);
	if (!ret)
		ret = cli_parse_u32(cl, argv[2], "count", &count);
	if (!ret)
		ret = cli_parse_opt_u32(cl, argc, argv, 3, cl->spfi_nn, "nn", &nn);
	if (!ret && (!count || count > RAMON_SPFI_TX_OFFS_MAX / 4))
		ret = cli_set_err(cl, EINVAL, "count is 1..%u", RAMON_SPFI_TX_OFFS_MAX / 4);
	if (ret)
		return ret;
	w = malloc(count * sizeof(*w));
	if (!w)
		return cli_set_err(cl, ENOMEM, "out of memory");
	for (i = 0; i < count; i++)
		w[i] = i + 1;
	ret = ramon_spfi_tx_offs_write(cl->c, nn, off, count * 4, w, &cl->st);
	if (!ret)
		printf("      wrote offsets 1..%u at byte 0x%x of the spfi%u TX offset table\n", count,
		       off, nn);
	free(w);
	return ret;
}

static void print_alert(void *user, const struct ramon_spfi_alert *a)
{
	(void)user;
	flockfile(stdout);
	printf("      spfi%u alert: code 0x%x sub 0x%x param1 0x%x param2 0x%x status 0x%x\n", a->nn,
	       a->code, a->sub_code, a->param1, a->param2, a->rx_status);
	fflush(stdout);
	funlockfile(stdout);
}

static int cmd_spfialerts(struct cli *cl, int argc, char **argv)
{
	uint32_t idle_ms, n;
	int ret = cli_parse_opt_u32(cl, argc, argv, 1, 200, "idle_ms", &idle_ms);

	if (!ret)
		ret = cli_init_spfi(cl);
	if (!ret)
		ret = ramon_spfi_drain_alerts(cl->c, cl->spfi_nn, idle_ms, print_alert, NULL, &n,
					      &cl->st);
	if (!ret)
		printf("      %u alert(s)\n", n);
	return ret;
}

static int cmd_spfialert(struct cli *cl, int argc, char **argv)
{
	uint32_t on;
	int ret = cli_parse_u32(cl, argv[1], "on", &on);

	(void)argc;
	if (!ret)
		ret = cli_init_spfi(cl);
	if (ret)
		return ret;
	if (!on)
		return ramon_spfi_alert_stop(cl->c, cl->spfi_nn);
	return ramon_spfi_alert_start(cl->c, cl->spfi_nn, print_alert, NULL, &cl->st);
}

static int cmd_spfiprep(struct cli *cl, int argc, char **argv)
{
	struct ramon_spfi_prep_opts o;
	struct ramon_spfi_table *t;
	uint32_t nn = cl->spfi_nn;
	int ret = 0;

	memset(&o, 0, sizeof(o));
	if (argc > 1)
		ret = cli_parse_u32(cl, argv[1], "nn", &nn);
	if (!ret && argc > 2) {
		if (strcmp(argv[2], "format") || argc != 4 || strcmp(argv[3], "yes"))
			return cli_set_err(cl, EPERM, "FORMAT erases every stream: write \"spfiprep %u format yes\"",
					   nn);
		o.format = 1;
	}
	if (!ret)
		ret = cli_init_spw(cl);
	if (!ret)
		ret = cli_init_spfi(cl);
	if (ret || !(t = table_new(cl)))
		return ret ? ret : -ENOMEM;
	ret = ramon_spfi_prep(cl->c, nn, &o, t, &cl->st);
	if (!ret)
		printf("      spfi%u prepared%s: %u stream(s) exist, %u open\n", nn,
		       o.format ? " and formatted" : "", ramon_spfi_count_exist(t),
		       ramon_spfi_count_open(t));
	free(t);
	return ret;
}

/* ---- spfitest: the old write/read performance and correctness test ---- */

static int cmd_spfitest(struct cli *cl, int argc, char **argv)
{
	struct ramon_spfi_stream s;
	struct ramon_status st;
	uint32_t sid, pages, runs, verify, r, bad_runs = 0;
	uint64_t bytes, t0, write_ns = 0, read_ns = 0;
	ramon_buf src, dst;
	int ret;

	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	memset(&s, 0, sizeof(s));
	ret = cli_init_spfi(cl);
	if (!ret)
		ret = parse_sid(cl, argv[1], &sid);
	if (!ret)
		ret = cli_parse_opt_u32(cl, argc, argv, 2, 16, "pages", &pages);
	if (!ret)
		ret = cli_parse_opt_u32(cl, argc, argv, 3, 10, "runs", &runs);
	if (!ret)
		ret = cli_parse_opt_u32(cl, argc, argv, 4, 1, "verify", &verify);
	if (!ret && (!pages || pages > RAMON_SPFI_WRITE_MAX_PAGES || !runs))
		ret = cli_set_err(cl, EINVAL, "pages is 1..%u, runs >= 1",
				  RAMON_SPFI_WRITE_MAX_PAGES);
	if (!ret)
		ret = ramon_buf_alloc(cl->c, (uint64_t)pages * SPFI_PAGE_BYTES, 0, &src, &cl->st);
	if (!ret)
		ret = ramon_buf_alloc(cl->c, (uint64_t)pages * SPFI_PAGE_BYTES, 0, &dst, &cl->st);
	if (!ret)
		ret = ramon_spfi_stream_open(cl->c, cl->spfi_nn, sid, pages * runs, &s, &cl->st);
	if (ret)
		goto out;
	bytes = (uint64_t)pages * SPFI_PAGE_BYTES;
	printf("      stream %u opened, first page %u; %u run(s) of %u page(s)\n", sid, s.first_page,
	       runs, pages);
	for (r = 0; r < runs && !ret && !cli_is_interrupted(); r++) {
		tc_fill(src.ptr, bytes, r);
		t0 = tc_now_ns();
		ret = ramon_spfi_stream_append(cl->c, &s, &src, 0, pages, 0, &cl->st);
		write_ns += tc_now_ns() - t0;
	}
	if (!ret)
		ret = ramon_spfi_stream_flush(cl->c, &s, &cl->st);
	if (ret)
		goto close;
	printf("      spfi send size(%llu) bytes, %.1f MiB/s\n", (unsigned long long)bytes * runs,
	       tc_mibps(bytes * runs, write_ns));
	for (r = 0; r < runs && !ret && !cli_is_interrupted(); r++) {
		memset(dst.ptr, 0, bytes);
		t0 = tc_now_ns();
		ret = ramon_spfi_read_seq(cl->c, cl->spfi_nn, sid, s.first_page + r * pages, pages,
					  &dst, 0, 0, &cl->st);
		read_ns += tc_now_ns() - t0;
		if (!ret && verify && tc_find_mismatch(dst.ptr, bytes, r) >= 0)
			bad_runs++;
	}
	if (!ret) {
		printf("      spfi receive size(%llu) bytes, %.1f MiB/s, %s\n",
		       (unsigned long long)bytes * r, tc_mibps(bytes * r, read_ns),
		       verify ? (bad_runs ? "DATA ERRORS" : "data verified") : "not verified");
		if (bad_runs)
			ret = cli_set_err(cl, EIO, "%u of %u read(s) returned different data",
					  bad_runs, r);
	}
close:
	if (ramon_spfi_stream_close(cl->c, &s, 1, &st) && !ret) {
		cl->st = st;
		ret = -EIO;
	}
out:
	ramon_buf_free(&src, NULL);
	ramon_buf_free(&dst, NULL);
	return ret;
}

/* ---- spfirxtx: writes to one stream while another is read ---- */

struct rxtx_writer {
	struct cli *cl;
	struct ramon_spfi_stream *stream;
	ramon_buf *src;
	uint32_t pages, n_writes, cycle;
	uint64_t write_ns;
	int ret;
	struct ramon_status st;
};

static void *rxtx_write_thread(void *arg)
{
	struct rxtx_writer *w = arg;
	uint32_t i;
	uint64_t t0 = tc_now_ns();

	for (i = 0; i < w->n_writes && !w->ret; i++) {
		tc_fill(w->src->ptr, (size_t)w->pages * SPFI_PAGE_BYTES, w->cycle << 16 | i);
		w->ret = ramon_spfi_stream_append(w->cl->c, w->stream, w->src, 0, w->pages, 0,
						  &w->st);
	}
	w->write_ns = tc_now_ns() - t0;
	return NULL;
}

static int cmd_spfirxtx(struct cli *cl, int argc, char **argv)
{
	struct ramon_spfi_stream write_stream, read_stream;
	struct rxtx_writer w;
	struct ramon_status st;
	uint32_t ids[2] = { 0, 0 }, write_sid = 0, read_sid = 0, wpages, rpages, wnum, rnum, cycles,
		 cycle, i, n_bad_reads = 0;
	ramon_buf write_src, read_buf;
	uint64_t t0, cycle_ns, read_ns;
	pthread_t th;
	int ret;

	memset(&write_stream, 0, sizeof(write_stream));
	memset(&read_stream, 0, sizeof(read_stream));
	memset(&write_src, 0, sizeof(write_src));
	memset(&read_buf, 0, sizeof(read_buf));
	ret = cli_init_spfi(cl);
	if (!ret && (!strcmp(argv[1], "auto") || !strcmp(argv[2], "auto"))) {
		struct ramon_spfi_table *t = table_new(cl);

		if (!t)
			return -ENOMEM;
		ret = get_table(cl, t);
		if (!ret && ramon_spfi_find_free(t, ids, 2) != 2)
			ret = cli_set_err(cl, ENOSPC, "fewer than two free stream ids");
		free(t);
	}
	if (!ret)
		ret = strcmp(argv[1], "auto") ? cli_parse_u32(cl, argv[1], "stw", &write_sid) :
						(write_sid = ids[0], 0);
	if (!ret)
		ret = strcmp(argv[2], "auto") ? cli_parse_u32(cl, argv[2], "str", &read_sid) :
						(read_sid = ids[1], 0);
	if (!ret)
		ret = cli_parse_u32(cl, argv[3], "wpages", &wpages);
	if (!ret)
		ret = cli_parse_u32(cl, argv[4], "rpages", &rpages);
	if (!ret)
		ret = cli_parse_u32(cl, argv[5], "wnum", &wnum);
	if (!ret)
		ret = cli_parse_u32(cl, argv[6], "rnum", &rnum);
	if (!ret)
		ret = cli_parse_opt_u32(cl, argc, argv, 7, 1, "cycles", &cycles);
	if (!ret && (write_sid == read_sid || !wpages || wpages > RAMON_SPFI_WRITE_MAX_PAGES ||
		     !rpages || rpages > RAMON_SPFI_WRITE_MAX_PAGES || !cycles))
		ret = cli_set_err(cl, EINVAL, "two different streams, 1..%u pages, cycles >= 1",
				  RAMON_SPFI_WRITE_MAX_PAGES);
	if (!ret)
		ret = ramon_buf_alloc(cl->c, (uint64_t)wpages * SPFI_PAGE_BYTES, 0, &write_src,
				      &cl->st);
	if (!ret)
		ret = ramon_buf_alloc(cl->c, (uint64_t)rpages * SPFI_PAGE_BYTES, 0, &read_buf,
				      &cl->st);
	/* the read stream: rpages written once and flushed */
	if (!ret)
		ret = ramon_spfi_stream_open(cl->c, cl->spfi_nn, read_sid, rpages, &read_stream,
					     &cl->st);
	if (!ret) {
		tc_fill(read_buf.ptr, (size_t)rpages * SPFI_PAGE_BYTES, RXTX_READ_SEED);
		ret = ramon_spfi_stream_append(cl->c, &read_stream, &read_buf, 0, rpages, 0,
					       &cl->st);
	}
	if (!ret)
		ret = ramon_spfi_stream_flush(cl->c, &read_stream, &cl->st);
	if (!ret)
		ret = ramon_spfi_stream_open(cl->c, cl->spfi_nn, write_sid, wpages * wnum * cycles,
					     &write_stream, &cl->st);
	if (ret)
		goto out;
	printf("      write stream %u (%u x %u pages per cycle), read stream %u (%u x %u pages)\n",
	       write_sid, wnum, wpages, read_sid, rnum, rpages);
	for (cycle = 0; cycle < cycles && !ret && !cli_is_interrupted(); cycle++) {
		memset(&w, 0, sizeof(w));
		w.cl = cl;
		w.stream = &write_stream;
		w.src = &write_src;
		w.pages = wpages;
		w.n_writes = wnum;
		w.cycle = cycle;
		t0 = tc_now_ns();
		if (pthread_create(&th, NULL, rxtx_write_thread, &w)) {
			ret = cli_set_err(cl, EAGAIN, "pthread_create failed");
			break;
		}
		read_ns = 0;
		for (i = 0; i < rnum && !ret; i++) {
			uint64_t read_t0 = tc_now_ns();

			memset(read_buf.ptr, 0, (size_t)rpages * SPFI_PAGE_BYTES);
			ret = ramon_spfi_read_seq(cl->c, cl->spfi_nn, read_sid,
						  read_stream.first_page, rpages, &read_buf, 0, 0,
						  &cl->st);
			read_ns += tc_now_ns() - read_t0;
			if (!ret && tc_find_mismatch(read_buf.ptr, (size_t)rpages * SPFI_PAGE_BYTES,
						     RXTX_READ_SEED) >= 0)
				n_bad_reads++;
		}
		pthread_join(th, NULL);
		cycle_ns = tc_now_ns() - t0;
		if (!ret && w.ret) {
			ret = w.ret;
			cl->st = w.st;
		}
		printf("      cycle %u: write %.1f MiB/s, read %.1f MiB/s, takes %.5f s, slack %.5f s\n",
		       cycle, tc_mibps((uint64_t)wnum * wpages * SPFI_PAGE_BYTES, w.write_ns),
		       tc_mibps((uint64_t)rnum * rpages * SPFI_PAGE_BYTES, read_ns), cycle_ns / 1e9,
		       1.0 - cycle_ns / 1e9);
	}
	if (!ret && n_bad_reads)
		ret = cli_set_err(cl, EIO, "%u read(s) returned different data", n_bad_reads);
out:
	if (ramon_spfi_stream_close(cl->c, &write_stream,
				    write_stream.nn == cl->spfi_nn &&
				    (write_stream.open || write_stream.pages_written), &st) &&
	    !ret) {
		cl->st = st;
		ret = -EIO;
	}
	if (ramon_spfi_stream_close(cl->c, &read_stream,
				    read_stream.nn == cl->spfi_nn &&
				    (read_stream.open || read_stream.pages_written), &st) &&
	    !ret) {
		cl->st = st;
		ret = -EIO;
	}
	ramon_buf_free(&write_src, NULL);
	ramon_buf_free(&read_buf, NULL);
	return ret;
}

const struct cli_cmd cli_spfi_cmds[] = {
	{ "spfidrvinit", cmd_spfidrvinit, 0, 1, 0, "[chan0,chan1]", "set up SPFI (write channels)" },
	{ "spfidrvterm", cmd_spfidrvterm, 0, 0, 0, "", "stop the alert threads, free the SPFI buffers" },
	{ "spfinnid", cmd_spfinnid, 1, 1, 0, "<nn>", "the NN the spfi* commands use" },
	{ "spfiprep", cmd_spfiprep, 0, 3, 0, "[nn] [format yes]",
	  "bring-up: link, SPW sync + DPS, INIT, optional FORMAT, table" },
	{ "spfiinfo", cmd_spfiinfo, 0, 0, 0, "", "link state, status words, IRQ counters" },
	{ "spfidiag", cmd_spfiinfo, 0, 0, CMD_HIDDEN, "", "same as spfiinfo" },
	{ "spfiloop", cmd_spfiloop, 1, 1, 0, "<0|1>", "SPFI loopback register (word 11)" },
	{ "spfiinit", cmd_spfiinit, 0, 1, 0, "[init_type]", "INIT command" },
	{ "spfifmt", cmd_spfifmt, 0, 0, CMD_DESTRUCTIVE, "", "FORMAT: erases every stream" },
	{ "spfiopens", cmd_spfiopens, 1, 3, 0, "<sid> [cyclic=0] [last_offset]", "OPEN a stream" },
	{ "spfiginfo", cmd_spfiginfo, 0, 1, 0, "[1 streams|2 header|3 all]", "stream table" },
	{ "spfigsend", cmd_spfigsend, 2, 3, 0, "<sid> <pages> [first|auto]",
	  "write pages of the test pattern" },
	{ "spfircv", cmd_spfircv, 3, 5, 0, "<sid> <start> <pages> [dump_off] [dump_size]",
	  "read pages, print the CRC, optional hex dump" },
	{ "spfistod", cmd_spfistod, 1, 1, 0, "<tod>", "SET_TOD" },
	{ "spficlose", cmd_spficlose, 1, 1, 0, "<sid>", "CLOSE a stream" },
	{ "spfidel", cmd_spfidel, 1, 1, 0, "<sid>", "DELETE a stream" },
	{ "spfiflush", cmd_spfiflush, 1, 1, 0, "<sid>", "FLUSH a stream" },
	{ "spfipdown", cmd_spfipdown, 0, 0, CMD_DESTRUCTIVE, "", "GRACEFUL_POWER_DOWN of the NN" },
	{ "spfireg", cmd_spfireg, 1, 2, 0, "<off> [val]", "SPFI register" },
	{ "spfitest", cmd_spfitest, 1, 4, 0, "<sid|auto> [pages=16] [runs=10] [verify=1]",
	  "write/read throughput and data check on a new stream (deleted after)" },
	{ "spfirxtx", cmd_spfirxtx, 6, 7, 0, "<stw|auto> <str|auto> <wpages> <rpages> <wnum> <rnum> [cycles]",
	  "writes to stream stw while stream str is read (streams deleted after)" },
	{ "spfitxoffs", cmd_spfitxoffs, 2, 3, 0, "<byte_off> <count> [nn]",
	  "write offsets 1..count into the TX offset table (old alimentest)" },
	{ "alimentest", cmd_spfitxoffs, 2, 3, CMD_HIDDEN, "<byte_off> <count> [nn]",
	  "old name of spfitxoffs" },
	{ "spfialerts", cmd_spfialerts, 0, 1, 0, "[idle_ms]", "print pending alerts" },
	{ "spfialert", cmd_spfialert, 1, 1, 0, "<0|1>", "alert thread that prints alerts" },
	{ NULL, NULL, 0, 0, 0, NULL, NULL },
};

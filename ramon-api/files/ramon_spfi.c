// SPDX-License-Identifier: MIT
/* libramon SPFI stream layer (ported from the old spfi_api.cpp and ramon_smoke) */
#include "ramon_priv.h"

#define SPFI_SETTLE_DEFAULT_MS	500
#define SPFI_SCRATCH_DEFAULT	64
#define SPFI_LINK_REG		RAMON_SPFI_REG_PHY_STATUS1
#define SPFI_LINK_MASK		0xFFu
#define SPFI_ALERT_WAIT_MS	1000	/* alert thread wait period; bounds how long a stop takes */
#define SPFI_ERR_BACKOFF_MS	100
#define SPFI_RETRY_DELAY_MS	2

/* ---- configuration and lifecycle ---- */

void ramon_spfi_config_default(struct ramon_spfi_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->chan[0] = RAMON_SPFI_CHAN_NN0;
	cfg->chan[1] = RAMON_SPFI_CHAN_NN1;
	cfg->settle_ms = SPFI_SETTLE_DEFAULT_MS;
	cfg->scratch_pages = SPFI_SCRATCH_DEFAULT;
}

static void spfi_fill_config(struct ramon_spfi_config *d, const struct ramon_spfi_config *in,
			     const struct ramon_get_info *gi)
{
	struct ramon_spfi_config def;

	ramon_spfi_config_default(&def);
	*d = in ? *in : def;
	if (!d->chan[0] && !d->chan[1]) {
		d->chan[0] = def.chan[0];
		d->chan[1] = def.chan[1];
	}
	if (!d->settle_ms)
		d->settle_ms = def.settle_ms;
	if (!d->scratch_pages)
		d->scratch_pages = def.scratch_pages;
	d->nn_mask = d->nn_mask ? d->nn_mask & gi->spfi_mask : gi->spfi_mask;
}

void ramon__spfi_ctx_init(struct ramon_ctx *c)
{
	uint32_t nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		struct spfi_nn *s = &c->spfi.nn[nn];

		s->ctx = c;
		s->nn = nn;
		pthread_mutex_init(&s->scratch_lock, NULL);
		pthread_mutex_init(&s->lock, NULL);
	}
	spfi_fill_config(&c->spfi.cfg, NULL, &c->info);
}

void ramon__spfi_ctx_destroy(struct ramon_ctx *c)
{
	uint32_t nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		pthread_mutex_destroy(&c->spfi.nn[nn].scratch_lock);
		pthread_mutex_destroy(&c->spfi.nn[nn].lock);
	}
}

int ramon_spfi_is_inited(const ramon_ctx *c)
{
	return c->spfi.inited;
}

void ramon_spfi_get_config(const ramon_ctx *c, struct ramon_spfi_config *cfg)
{
	*cfg = c->spfi.cfg;
}

int ramon_spfi_present(const ramon_ctx *c, uint32_t nn)
{
	return nn < RAMON_NN_COUNT && (c->spfi.cfg.nn_mask & (1u << nn));
}

int ramon_spfi_init(ramon_ctx *c, const struct ramon_spfi_config *cfg, struct ramon_status *st)
{
	struct ramon_spfi_config d;
	struct ramon_chan_info ci;
	uint32_t nn;
	int ret;

	pthread_mutex_lock(&c->lock);
	if (c->spfi.inited) {
		pthread_mutex_unlock(&c->lock);
		return ramon__fail(st, RAMON_EL_BUSY, EALREADY, 0, 0, "SPFI layer already initialised");
	}
	spfi_fill_config(&d, cfg, &c->info);
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		struct spfi_nn *s = &c->spfi.nn[nn];

		s->present = 0;
		if (!(d.nn_mask & (1u << nn)))
			continue;
		ret = ramon_chan_info(c, d.chan[nn], &ci, st);
		if (!ret && (ci.type != RAMON_CHAN_AXI || ci.dir != RAMON_DIR_MEM_TO_DEV))
			ret = ramon__fail(st, RAMON_EL_INVAL, 0, nn, d.chan[nn],
					  "spfi%u: channel %u is not an AXI MEM_TO_DEV channel", nn,
					  d.chan[nn]);
		if (ret) {
			pthread_mutex_unlock(&c->lock);
			return ret;
		}
		s->chan = d.chan[nn];
		s->present = 1;
	}
	c->spfi.cfg = d;
	c->spfi.inited = 1;
	pthread_mutex_unlock(&c->lock);
	ramon_status_clear(st);
	return 0;
}

void ramon_spfi_fini(ramon_ctx *c)
{
	uint32_t nn;

	if (!c->spfi.inited)
		return;
	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		ramon_spfi_alert_stop(c, nn);
	pthread_mutex_lock(&c->lock);
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		ramon_buf_free(&c->spfi.nn[nn].scratch, NULL);
		c->spfi.nn[nn].present = 0;
	}
	c->spfi.inited = 0;
	spfi_fill_config(&c->spfi.cfg, NULL, &c->info);
	pthread_mutex_unlock(&c->lock);
}

/* every SPFI entry: NN valid, layer initialised (with the defaults if needed), NN enabled */
static int spfi_check(struct ramon_ctx *c, uint32_t nn, struct ramon_status *st)
{
	int ret;

	if (nn >= RAMON_NN_COUNT)
		return ramon__fail(st, RAMON_EL_INVAL, 0, nn, 0, "no SPFI NN %u", nn);
	if (!(c->info.spfi_mask & (1u << nn)))
		return ramon__fail(st, RAMON_EL_NOT_PRESENT, 0, nn, c->info.spfi_mask,
				   "spfi%u is not present or masked off by the driver (spfi_mask 0x%x)",
				   nn, c->info.spfi_mask);
	if (!c->spfi.inited) {
		ret = ramon_spfi_init(c, NULL, st);
		if (ret && ret != -EALREADY)
			return ret;
	}
	if (!c->spfi.nn[nn].present)
		return ramon__fail(st, RAMON_EL_NOT_PRESENT, 0, nn, c->spfi.cfg.nn_mask,
				   "spfi%u is not enabled (nn_mask 0x%x)", nn, c->spfi.cfg.nn_mask);
	return 0;
}

/* after an SPFI timeout a late answer may still come: keep the next command back */
static void spfi_settle(struct spfi_nn *s)
{
	uint64_t until = __atomic_load_n(&s->settle_until_ns, __ATOMIC_RELAXED);
	uint64_t now = ramon__now_ns();

	if (now < until)
		ramon__sleep_ms((uint32_t)((until - now + 999999) / 1000000));
}

static void spfi_note(struct ramon_ctx *c, struct spfi_nn *s, const struct ramon_status *t)
{
	switch (t->code) {
	case RAMON_E_SPFI_CMD_TIMEOUT:
	case RAMON_E_SPFI_WRITE_TIMEOUT:
	case RAMON_E_SPFI_READ_TIMEOUT:
	case RAMON_E_SPFI_UNEXPECTED_OPCODE:
		__atomic_store_n(&s->settle_until_ns,
				 ramon__now_ns() + (uint64_t)c->spfi.cfg.settle_ms * 1000000ull,
				 __ATOMIC_RELAXED);
		ramon__log(c, RAMON_LOG_INFO, "spfi%u: %s, next command waits %u ms", s->nn,
			   ramon_err_str(t->code), c->spfi.cfg.settle_ms);
		break;
	default:
		break;
	}
}

int ramon_spfi_link_up(ramon_ctx *c, uint32_t nn, uint32_t *raw, struct ramon_status *st)
{
	char win[8];
	uint32_t v;
	int ret;

	if (nn >= RAMON_NN_COUNT)
		return ramon__fail(st, RAMON_EL_INVAL, 0, nn, 0, "no SPFI NN %u", nn);
	snprintf(win, sizeof(win), "spfi%u", nn);
	ret = ramon_reg_read(c, win, SPFI_LINK_REG, &v, st);
	if (ret)
		return ret;
	if (raw)
		*raw = v;
	return (v & SPFI_LINK_MASK) == RAMON_SPFI_LINK_UP;
}

/* ---- short commands ---- */

const char *ramon_spfi_op_name(uint32_t opcode)
{
	switch (opcode) {
	case RAMON_SPFI_OP_DATA_WRITE:			return "DATA_WRITE";
	case RAMON_SPFI_OP_FLUSH_STREAM:		return "FLUSH";
	case RAMON_SPFI_OP_READ_STREAM:			return "READ_STREAM";
	case RAMON_SPFI_OP_OPEN_STREAM_FOR_WRITE:	return "OPEN";
	case RAMON_SPFI_OP_CLOSE_STREAM_FOR_WRITE:	return "CLOSE";
	case RAMON_SPFI_OP_DELETE_STREAM:		return "DELETE";
	case RAMON_SPFI_OP_GET_ALL_STREAM_STATUS:	return "GET_ALL_STREAM_STATUS";
	case RAMON_SPFI_OP_PLATFORM_RESET:		return "PLATFORM_RESET";
	case RAMON_SPFI_OP_INIT:			return "INIT";
	case RAMON_SPFI_OP_GRACEFUL_POWER_DOWN:		return "GRACEFUL_POWER_DOWN";
	case RAMON_SPFI_OP_SET_TOD:			return "SET_TOD";
	case RAMON_SPFI_OP_FORMAT:			return "FORMAT";
	default:					return "unknown opcode";
	}
}

int ramon_spfi_command(ramon_ctx *c, uint32_t nn, uint32_t opcode, const struct ramon_spfi_args *a,
		       struct ramon_spfi_reply *r, struct ramon_status *st)
{
	struct ramon_spfi_cmd k;
	struct spfi_nn *s;
	int ret;

	ret = spfi_check(c, nn, st);
	if (ret)
		return ret;
	if (opcode == RAMON_SPFI_OP_DATA_WRITE || opcode == RAMON_SPFI_OP_READ_STREAM)
		return ramon__fail(st, RAMON_EL_INVAL, 0, opcode, 0,
				   "%s goes through ramon_spfi_write/read", ramon_spfi_op_name(opcode));
	s = &c->spfi.nn[nn];
	memset(&k, 0, sizeof(k));
	k.nn = nn;
	k.opcode = opcode;
	k.wait_ack = 1;
	k.timeout_ms = c->spfi.cfg.cmd_timeout_ms;
	if (a) {
		k.stream_id = a->stream_id;
		k.stream_type = a->stream_type;
		k.stream_last_offset = a->stream_last_offset;
		k.tod = a->tod;
		k.init_type = a->init_type;
		k.wait_ack = !a->no_ack;
		if (a->timeout_ms)
			k.timeout_ms = a->timeout_ms;
	}
	spfi_settle(s);
	ret = ramon__ioctl(c, RAMON_IOC_SPFI_CMD, &k, &k.st, st);
	if (ret)
		spfi_note(c, s, &k.st);
	if (r) {
		r->rx_opcode = k.rx_opcode;
		r->rx_err_code = k.rx_err_code;
		r->rx_stream_id = k.rx_stream_id;
		r->rx_offset = k.rx_offset;
		r->rx_init_info = k.rx_init_info;
		r->rx_curr_tod = k.rx_curr_tod;
		r->rx_status = k.rx_status;
	}
	return ret;
}

static int spfi_op(struct ramon_ctx *c, uint32_t nn, uint32_t opcode, const struct ramon_spfi_args *a,
		   struct ramon_spfi_reply *r, int check, struct ramon_status *st)
{
	struct ramon_spfi_reply tmp;
	int ret;

	if (!r)
		r = &tmp;
	memset(r, 0, sizeof(*r));
	ret = ramon_spfi_command(c, nn, opcode, a, r, st);
	if (!ret && check && r->rx_err_code)
		return ramon__fail(st, RAMON_EL_SPFI_NN_ERROR, 0, r->rx_err_code, r->rx_opcode,
				   "spfi%u %s stream %u: NN rx_err_code %u", nn,
				   ramon_spfi_op_name(opcode), a ? a->stream_id : 0, r->rx_err_code);
	return ret;
}

int ramon_spfi_init_nn(ramon_ctx *c, uint32_t nn, uint32_t init_type, struct ramon_spfi_reply *r,
		       struct ramon_status *st)
{
	struct ramon_spfi_args a;

	memset(&a, 0, sizeof(a));
	a.init_type = init_type;
	return spfi_op(c, nn, RAMON_SPFI_OP_INIT, &a, r, 0, st);
}

int ramon_spfi_format(ramon_ctx *c, uint32_t nn, struct ramon_spfi_reply *r, struct ramon_status *st)
{
	return spfi_op(c, nn, RAMON_SPFI_OP_FORMAT, NULL, r, 1, st);
}

int ramon_spfi_platform_reset(ramon_ctx *c, uint32_t nn, struct ramon_spfi_reply *r,
			      struct ramon_status *st)
{
	return spfi_op(c, nn, RAMON_SPFI_OP_PLATFORM_RESET, NULL, r, 1, st);
}

int ramon_spfi_power_down(ramon_ctx *c, uint32_t nn, struct ramon_spfi_reply *r,
			  struct ramon_status *st)
{
	return spfi_op(c, nn, RAMON_SPFI_OP_GRACEFUL_POWER_DOWN, NULL, r, 1, st);
}

int ramon_spfi_set_tod(ramon_ctx *c, uint32_t nn, uint32_t tod, struct ramon_spfi_reply *r,
		       struct ramon_status *st)
{
	struct ramon_spfi_args a;

	memset(&a, 0, sizeof(a));
	a.tod = tod;
	return spfi_op(c, nn, RAMON_SPFI_OP_SET_TOD, &a, r, 1, st);
}

static int spfi_stream_cmd(struct ramon_ctx *c, uint32_t nn, uint32_t opcode, uint32_t sid,
			   uint32_t last, struct ramon_spfi_reply *r, struct ramon_status *st)
{
	struct ramon_spfi_args a;

	if (sid >= RAMON_SPFI_STREAMS)
		return ramon__fail(st, RAMON_EL_INVAL, 0, sid, 0, "no stream id %u", sid);
	memset(&a, 0, sizeof(a));
	a.stream_id = sid;
	a.stream_last_offset = last;
	return spfi_op(c, nn, opcode, &a, r, 1, st);
}

int ramon_spfi_open(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t size_pages,
		    struct ramon_spfi_reply *r, struct ramon_status *st)
{
	if (sid >= RAMON_SPFI_STREAMS_USABLE)
		return ramon__fail(st, RAMON_EL_SPFI_RULE, 0, sid, RAMON_SPFI_STREAMS_USABLE,
				   "stream ids %u..%u are reserved by the NN", RAMON_SPFI_STREAMS_USABLE,
				   RAMON_SPFI_STREAMS - 1);
	return spfi_stream_cmd(c, nn, RAMON_SPFI_OP_OPEN_STREAM_FOR_WRITE, sid, size_pages, r, st);
}

int ramon_spfi_close(ramon_ctx *c, uint32_t nn, uint32_t sid, struct ramon_spfi_reply *r,
		     struct ramon_status *st)
{
	return spfi_stream_cmd(c, nn, RAMON_SPFI_OP_CLOSE_STREAM_FOR_WRITE, sid, 0, r, st);
}

int ramon_spfi_delete(ramon_ctx *c, uint32_t nn, uint32_t sid, struct ramon_spfi_reply *r,
		      struct ramon_status *st)
{
	return spfi_stream_cmd(c, nn, RAMON_SPFI_OP_DELETE_STREAM, sid, 0, r, st);
}

int ramon_spfi_flush(ramon_ctx *c, uint32_t nn, uint32_t sid, struct ramon_spfi_reply *r,
		     struct ramon_status *st)
{
	return spfi_stream_cmd(c, nn, RAMON_SPFI_OP_FLUSH_STREAM, sid, 0, r, st);
}

/* ---- table ---- */

int ramon_spfi_mem_read(ramon_ctx *c, uint32_t nn, uint32_t off, uint32_t size, void *out,
			struct ramon_status *st)
{
	struct ramon_spfi_mem_read m;
	int ret;

	ret = spfi_check(c, nn, st);
	if (ret)
		return ret;
	memset(&m, 0, sizeof(m));
	m.nn = nn;
	m.offset = off;
	m.size = size;
	m.data = (uintptr_t)out;
	return ramon__ioctl(c, RAMON_IOC_SPFI_MEM_READ, &m, &m.st, st);
}

int ramon_spfi_tx_offs_write(ramon_ctx *c, uint32_t nn, uint32_t off, uint32_t size, const void *in,
			     struct ramon_status *st)
{
	struct ramon_spfi_tx_offs_write m;
	int ret;

	ret = spfi_check(c, nn, st);
	if (ret)
		return ret;
	memset(&m, 0, sizeof(m));
	m.nn = nn;
	m.offset = off;
	m.size = size;
	m.data = (uintptr_t)in;
	return ramon__ioctl(c, RAMON_IOC_SPFI_TX_OFFS_WRITE, &m, &m.st, st);
}

int ramon_spfi_get_table(ramon_ctx *c, uint32_t nn, struct ramon_spfi_table *t,
			 struct ramon_status *st)
{
	int ret;

	ret = spfi_op(c, nn, RAMON_SPFI_OP_GET_ALL_STREAM_STATUS, NULL, NULL, 0, st);
	if (!ret)
		ret = ramon_spfi_mem_read(c, nn, 0, RAMON_SPFI_TABLE_BYTES, t, st);
	return ret;
}

unsigned ramon_spfi_count_open(const struct ramon_spfi_table *t)
{
	unsigned i, n = 0;

	for (i = 0; i < RAMON_SPFI_STREAMS; i++)
		n += !!(t->rec[i].status & RAMON_SPFI_ST_OPEN);
	return n;
}

unsigned ramon_spfi_count_exist(const struct ramon_spfi_table *t)
{
	unsigned i, n = 0;

	for (i = 0; i < RAMON_SPFI_STREAMS; i++)
		n += !!(t->rec[i].status & RAMON_SPFI_ST_EXIST);
	return n;
}

unsigned ramon_spfi_find_free(const struct ramon_spfi_table *t, uint32_t *out, unsigned want)
{
	unsigned n = 0;
	uint32_t sid;

	for (sid = RAMON_SPFI_STREAMS_USABLE; sid-- > 0 && n < want;)
		if (!(t->rec[sid].status & RAMON_SPFI_ST_EXIST))
			out[n++] = sid;
	return n;
}

/* ---- data path ---- */

int ramon_spfi_write(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page,
		     const struct ramon_sg_item *items, uint32_t n_items, uint32_t n_pages,
		     uint32_t timeout_ms, uint32_t *rx_status, struct ramon_status *st)
{
	struct ramon_spfi_write w;
	struct spfi_nn *s;
	uint64_t sum = 0;
	uint32_t i;
	int ret, retried = 0;

	ret = spfi_check(c, nn, st);
	if (ret)
		return ret;
	if (sid >= RAMON_SPFI_STREAMS)
		return ramon__fail(st, RAMON_EL_INVAL, 0, sid, 0, "no stream id %u", sid);
	if (!n_items || n_items > RAMON_AXI_MAX_ITEMS)
		return ramon__fail(st, RAMON_EL_INVAL, 0, n_items, RAMON_AXI_MAX_ITEMS,
				   "%u SG items (1..%u)", n_items, RAMON_AXI_MAX_ITEMS);
	if (!n_pages || n_pages > RAMON_SPFI_WRITE_MAX_PAGES)
		return ramon__fail(st, RAMON_EL_SPFI_RULE, 0, n_pages, RAMON_SPFI_WRITE_MAX_PAGES,
				   "%u pages per write (1..%u)", n_pages, RAMON_SPFI_WRITE_MAX_PAGES);
	for (i = 0; i < n_items; i++)
		sum += items[i].len;
	if (sum != (uint64_t)n_pages * RAMON_SPFI_PAGE)
		return ramon__fail(st, RAMON_EL_INVAL, 0, sum, (uint64_t)n_pages * RAMON_SPFI_PAGE,
				   "items hold %llu bytes, %u pages are %llu",
				   (unsigned long long)sum, n_pages,
				   (unsigned long long)n_pages * RAMON_SPFI_PAGE);
	s = &c->spfi.nn[nn];
	spfi_settle(s);
	for (;;) {
		memset(&w, 0, sizeof(w));
		w.nn = nn;
		w.chan = s->chan;
		w.stream_id = sid;
		w.tx_offset = first_page;
		w.tx_num_offset = n_pages;
		w.timeout_ms = timeout_ms ? timeout_ms : c->spfi.cfg.write_timeout_ms;
		w.n_items = n_items;
		w.items = (uintptr_t)items;
		ret = ramon__ioctl(c, RAMON_IOC_SPFI_WRITE, &w, &w.st, st);
		if (ret != -ENOSPC || w.st.code != RAMON_E_AXI_PREP_FAILED || retried)
			break;
		retried = 1;
		__atomic_add_fetch(&c->axi_retries, 1, __ATOMIC_RELAXED);
		ramon__sleep_ms(SPFI_RETRY_DELAY_MS);
	}
	if (ret)
		spfi_note(c, s, &w.st);
	if (rx_status)
		*rx_status = w.rx_status;
	return ret;
}

int ramon_spfi_write_buf(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page,
			 const ramon_buf *b, uint64_t off, uint32_t n_pages, uint32_t timeout_ms,
			 struct ramon_status *st)
{
	struct ramon_sg_item it;

	memset(&it, 0, sizeof(it));
	it.handle = b->handle;
	it.offset = off;
	it.len = (uint64_t)n_pages * RAMON_SPFI_PAGE;
	return ramon_spfi_write(c, nn, sid, first_page, &it, 1, n_pages, timeout_ms, NULL, st);
}

int ramon_spfi_read(ramon_ctx *c, uint32_t nn, uint32_t sid, const uint32_t *pages, uint32_t n,
		    const ramon_buf *dst, uint64_t dst_off, uint32_t timeout_ms, uint32_t *rx_status,
		    struct ramon_status *st)
{
	struct ramon_spfi_read r;
	struct spfi_nn *s;
	int ret;

	ret = spfi_check(c, nn, st);
	if (ret)
		return ret;
	if (!n || n > RAMON_SPFI_READ_MAX_PAGES)
		return ramon__fail(st, RAMON_EL_INVAL, 0, n, RAMON_SPFI_READ_MAX_PAGES,
				   "%u pages per read (1..%u)", n, RAMON_SPFI_READ_MAX_PAGES);
	s = &c->spfi.nn[nn];
	spfi_settle(s);
	memset(&r, 0, sizeof(r));
	r.nn = nn;
	r.stream_id = sid;
	r.n_offsets = n;
	r.dst_handle = dst->handle;
	r.offsets = (uintptr_t)pages;
	r.dst_off = dst_off;
	r.timeout_ms = timeout_ms ? timeout_ms : c->spfi.cfg.read_timeout_ms;
	ret = ramon__ioctl(c, RAMON_IOC_SPFI_READ, &r, &r.st, st);
	if (ret)
		spfi_note(c, s, &r.st);
	if (rx_status)
		*rx_status = r.rx_status;
	return ret;
}

int ramon_spfi_read_seq(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page, uint32_t n,
			const ramon_buf *dst, uint64_t dst_off, uint32_t timeout_ms,
			struct ramon_status *st)
{
	uint32_t *pages, i;
	int ret;

	if (!n || n > RAMON_SPFI_READ_MAX_PAGES)
		return ramon__fail(st, RAMON_EL_INVAL, 0, n, RAMON_SPFI_READ_MAX_PAGES,
				   "%u pages per read (1..%u)", n, RAMON_SPFI_READ_MAX_PAGES);
	pages = malloc((size_t)n * sizeof(*pages));
	if (!pages)
		return ramon__fail(st, RAMON_EL_SYS, ENOMEM, 0, 0, "out of memory");
	for (i = 0; i < n; i++)
		pages[i] = first_page + i;
	ret = ramon_spfi_read(c, nn, sid, pages, n, dst, dst_off, timeout_ms, NULL, st);
	free(pages);
	return ret;
}

/* the bounce buffer of the *_mem calls; scratch_lock held */
static int spfi_scratch(struct ramon_ctx *c, struct spfi_nn *s, struct ramon_status *st)
{
	if (s->scratch.handle)
		return 0;
	return ramon_buf_alloc(c, (uint64_t)c->spfi.cfg.scratch_pages * RAMON_SPFI_PAGE, 0,
			       &s->scratch, st);
}

int ramon_spfi_write_mem(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page,
			 const void *data, size_t len, uint32_t timeout_ms, struct ramon_status *st)
{
	const uint8_t *p = data;
	struct spfi_nn *s;
	uint32_t pages, done = 0, n;
	int ret;

	ret = spfi_check(c, nn, st);
	if (ret)
		return ret;
	if (!len || len % RAMON_SPFI_PAGE)
		return ramon__fail(st, RAMON_EL_INVAL, 0, len, RAMON_SPFI_PAGE,
				   "%zu bytes is not a whole number of 16 KiB pages", len);
	s = &c->spfi.nn[nn];
	pages = (uint32_t)(len / RAMON_SPFI_PAGE);
	pthread_mutex_lock(&s->scratch_lock);
	ret = spfi_scratch(c, s, st);
	while (!ret && done < pages) {
		n = pages - done;
		if (n > c->spfi.cfg.scratch_pages)
			n = c->spfi.cfg.scratch_pages;
		memcpy(s->scratch.ptr, p + (size_t)done * RAMON_SPFI_PAGE,
		       (size_t)n * RAMON_SPFI_PAGE);
		ret = ramon_spfi_write_buf(c, nn, sid, first_page + done, &s->scratch, 0, n,
					   timeout_ms, st);
		done += n;
	}
	pthread_mutex_unlock(&s->scratch_lock);
	return ret;
}

int ramon_spfi_read_mem(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page, uint32_t n,
			void *out, uint32_t timeout_ms, struct ramon_status *st)
{
	uint8_t *p = out;
	struct spfi_nn *s;
	uint32_t done = 0, k;
	int ret;

	ret = spfi_check(c, nn, st);
	if (ret)
		return ret;
	s = &c->spfi.nn[nn];
	pthread_mutex_lock(&s->scratch_lock);
	ret = spfi_scratch(c, s, st);
	while (!ret && done < n) {
		k = n - done;
		if (k > c->spfi.cfg.scratch_pages)
			k = c->spfi.cfg.scratch_pages;
		ret = ramon_spfi_read_seq(c, nn, sid, first_page + done, k, &s->scratch, 0,
					  timeout_ms, st);
		if (!ret)
			memcpy(p + (size_t)done * RAMON_SPFI_PAGE, s->scratch.ptr,
			       (size_t)k * RAMON_SPFI_PAGE);
		done += k;
	}
	pthread_mutex_unlock(&s->scratch_lock);
	return ret;
}

/* ---- stream helper ---- */

int ramon_spfi_stream_open(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t size_pages,
			   struct ramon_spfi_stream *s, struct ramon_status *st)
{
	struct ramon_spfi_table *t;
	const struct ramon_spfi_record *r;
	unsigned open;
	int ret;

	memset(s, 0, sizeof(*s));
	if (sid >= RAMON_SPFI_STREAMS_USABLE)
		return ramon__fail(st, RAMON_EL_SPFI_RULE, 0, sid, RAMON_SPFI_STREAMS_USABLE,
				   "stream ids %u..%u are reserved by the NN", RAMON_SPFI_STREAMS_USABLE,
				   RAMON_SPFI_STREAMS - 1);
	t = malloc(sizeof(*t));
	if (!t)
		return ramon__fail(st, RAMON_EL_SYS, ENOMEM, 0, 0, "out of memory");
	ret = ramon_spfi_get_table(c, nn, t, st);
	if (ret)
		goto out;
	r = &t->rec[sid];
	open = ramon_spfi_count_open(t);
	if (r->status & RAMON_SPFI_ST_EXIST) {
		ret = ramon__fail(st, RAMON_EL_SPFI_RULE, 0, sid, r->status,
				  "spfi%u stream %u exists (status 0x%x): delete it before opening it again",
				  nn, sid, r->status);
		goto out;
	}
	if (open >= RAMON_SPFI_MAX_OPEN) {
		ret = ramon__fail(st, RAMON_EL_SPFI_RULE, 0, open, RAMON_SPFI_MAX_OPEN,
				  "spfi%u: %u streams are open, the NN allows %u", nn, open,
				  RAMON_SPFI_MAX_OPEN);
		goto out;
	}
	/* the first page comes from the table read before OPEN, as the old spfi_send() did */
	s->nn = nn;
	s->sid = sid;
	s->first_page = r->latest_write_offs + 1;
	s->next_page = s->first_page;
	ret = ramon_spfi_open(c, nn, sid, size_pages, NULL, st);
	if (!ret)
		s->open = 1;
out:
	free(t);
	return ret;
}

int ramon_spfi_stream_append(ramon_ctx *c, struct ramon_spfi_stream *s, const ramon_buf *b,
			     uint64_t off, uint32_t n_pages, uint32_t timeout_ms,
			     struct ramon_status *st)
{
	int ret;

	if (!s->open)
		return ramon__fail(st, RAMON_EL_INVAL, 0, s->sid, 0, "stream %u is not open", s->sid);
	ret = ramon_spfi_write_buf(c, s->nn, s->sid, s->next_page, b, off, n_pages, timeout_ms, st);
	if (!ret) {
		s->next_page += n_pages;
		s->pages_written += n_pages;
	}
	return ret;
}

int ramon_spfi_stream_append_mem(ramon_ctx *c, struct ramon_spfi_stream *s, const void *data,
				 size_t len, uint32_t timeout_ms, struct ramon_status *st)
{
	uint32_t n = (uint32_t)(len / RAMON_SPFI_PAGE);
	int ret;

	if (!s->open)
		return ramon__fail(st, RAMON_EL_INVAL, 0, s->sid, 0, "stream %u is not open", s->sid);
	ret = ramon_spfi_write_mem(c, s->nn, s->sid, s->next_page, data, len, timeout_ms, st);
	if (!ret) {
		s->next_page += n;
		s->pages_written += n;
	}
	return ret;
}

int ramon_spfi_stream_flush(ramon_ctx *c, struct ramon_spfi_stream *s, struct ramon_status *st)
{
	return ramon_spfi_flush(c, s->nn, s->sid, NULL, st);
}

int ramon_spfi_stream_close(ramon_ctx *c, struct ramon_spfi_stream *s, int and_delete,
			    struct ramon_status *st)
{
	int ret = 0;

	if (s->open) {
		ret = ramon_spfi_close(c, s->nn, s->sid, NULL, st);
		if (ret)
			return ret;
		s->open = 0;
	}
	if (and_delete)
		ret = ramon_spfi_delete(c, s->nn, s->sid, NULL, st);
	return ret;
}

/* ---- alerts ---- */

static int spfi_wait_alert_raw(struct ramon_ctx *c, uint32_t nn, uint32_t timeout_ms,
			       struct ramon_spfi_alert *a, struct ramon_status *st)
{
	struct ramon_spfi_wait_alert w;
	int ret;

	memset(&w, 0, sizeof(w));
	w.nn = nn;
	w.timeout_ms = timeout_ms;
	ret = ramon__ioctl(c, RAMON_IOC_SPFI_WAIT_ALERT, &w, &w.st, st);
	if (!ret && a) {
		a->nn = nn;
		a->code = w.code;
		a->sub_code = w.sub_code;
		a->param1 = w.param1;
		a->param2 = w.param2;
		a->rx_status = w.rx_status;
	}
	return ret;
}

int ramon_spfi_wait_alert(ramon_ctx *c, uint32_t nn, uint32_t timeout_ms, struct ramon_spfi_alert *a,
			  struct ramon_status *st)
{
	int ret = spfi_check(c, nn, st);

	return ret ? ret : spfi_wait_alert_raw(c, nn, timeout_ms, a, st);
}

int ramon_spfi_cancel_alert(ramon_ctx *c, uint32_t nn, struct ramon_status *st)
{
	struct ramon_spfi_cancel_alert k;
	int ret;

	ret = spfi_check(c, nn, st);
	if (ret)
		return ret;
	memset(&k, 0, sizeof(k));
	k.nn = nn;
	return ramon__ioctl(c, RAMON_IOC_SPFI_CANCEL_ALERT, &k, &k.st, st);
}

static int spfi_alert_state(struct spfi_nn *s)
{
	int state;

	pthread_mutex_lock(&s->lock);
	state = s->alert_state;
	pthread_mutex_unlock(&s->lock);
	return state;
}

static void *spfi_alert_main(void *arg)
{
	struct spfi_nn *s = arg;
	struct ramon_ctx *c = s->ctx;
	struct ramon_spfi_alert a;
	struct ramon_status st;
	int ret;

	while (spfi_alert_state(s) == SPFI_ALERT_RUNNING) {
		ret = spfi_wait_alert_raw(c, s->nn, SPFI_ALERT_WAIT_MS, &a, &st);
		if (!ret) {
			s->cb(s->cb_user, &a);
			continue;
		}
		if (ret == -ETIMEDOUT || ret == -ECANCELED || ret == -EINTR)
			continue;
		if (ret == -ENODEV) {
			ramon__log(c, RAMON_LOG_ERR, "spfi%u: device removed, alert thread stops", s->nn);
			break;
		}
		ramon__log(c, RAMON_LOG_WARN, "spfi%u: SPFI_WAIT_ALERT: %s", s->nn,
			   ramon_err_str(st.code));
		ramon__sleep_ms(SPFI_ERR_BACKOFF_MS);
	}
	return NULL;
}

int ramon_spfi_alert_start(ramon_ctx *c, uint32_t nn, ramon_spfi_alert_cb cb, void *user,
			   struct ramon_status *st)
{
	struct spfi_nn *s;
	int ret;

	ret = spfi_check(c, nn, st);
	if (ret)
		return ret;
	if (!cb)
		return ramon__fail(st, RAMON_EL_INVAL, 0, 0, 0, "no alert callback");
	s = &c->spfi.nn[nn];
	pthread_mutex_lock(&s->lock);
	if (s->thread_valid) {
		pthread_mutex_unlock(&s->lock);
		return ramon__fail(st, RAMON_EL_BUSY, 0, nn, 0, "spfi%u: alert thread already running",
				   nn);
	}
	s->cb = cb;
	s->cb_user = user;
	s->alert_state = SPFI_ALERT_RUNNING;
	ret = ramon__thread_start(&s->thread, spfi_alert_main, s);
	if (ret) {
		s->alert_state = SPFI_ALERT_NONE;
		pthread_mutex_unlock(&s->lock);
		return ramon__fail(st, RAMON_EL_SYS, -ret, 0, 0, "pthread_create: %s", strerror(-ret));
	}
	s->thread_valid = 1;
	pthread_mutex_unlock(&s->lock);
	ramon_status_clear(st);
	return 0;
}

int ramon_spfi_alert_stop(ramon_ctx *c, uint32_t nn)
{
	struct ramon_spfi_cancel_alert k;
	struct spfi_nn *s;

	if (nn >= RAMON_NN_COUNT)
		return -EINVAL;
	s = &c->spfi.nn[nn];
	pthread_mutex_lock(&s->lock);
	if (!s->thread_valid) {
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (pthread_equal(pthread_self(), s->thread)) {
		pthread_mutex_unlock(&s->lock);
		return -EDEADLK;
	}
	s->alert_state = SPFI_ALERT_STOPPING;
	pthread_mutex_unlock(&s->lock);
	memset(&k, 0, sizeof(k));
	k.nn = nn;
	ramon__ioctl(c, RAMON_IOC_SPFI_CANCEL_ALERT, &k, &k.st, NULL);
	pthread_join(s->thread, NULL);
	pthread_mutex_lock(&s->lock);
	s->thread_valid = 0;
	s->alert_state = SPFI_ALERT_NONE;
	s->cb = NULL;
	s->cb_user = NULL;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

int ramon_spfi_drain_alerts(ramon_ctx *c, uint32_t nn, uint32_t idle_ms, ramon_spfi_alert_cb cb,
			    void *user, uint32_t *n, struct ramon_status *st)
{
	struct ramon_spfi_alert a;
	uint32_t count = 0;
	int ret;

	ret = spfi_check(c, nn, st);
	while (!ret) {
		ret = spfi_wait_alert_raw(c, nn, idle_ms ? idle_ms : 1, &a, st);
		if (ret)
			break;
		count++;
		if (cb)
			cb(user, &a);
	}
	if (n)
		*n = count;
	if (ret == -ETIMEDOUT && st && st->code == RAMON_E_SPFI_ALERT_TIMEOUT) {
		ramon_status_clear(st);
		return 0;
	}
	return ret == -ETIMEDOUT ? 0 : ret;
}

/* ---- diagnostics and bring-up ---- */

const uint32_t ramon_spfi_diag_word_index[RAMON_SPFI_DIAG_WORDS] = { 48, 49, 50, 51, 19, 21, 33 };

int ramon_spfi_diag(ramon_ctx *c, uint32_t nn, struct ramon_spfi_diag *d, struct ramon_status *st)
{
	struct ramon_get_stats g;
	char win[8];
	unsigned i;
	int ret;

	memset(d, 0, sizeof(*d));
	if (nn >= RAMON_NN_COUNT)
		return ramon__fail(st, RAMON_EL_INVAL, 0, nn, 0, "no SPFI NN %u", nn);
	snprintf(win, sizeof(win), "spfi%u", nn);
	for (i = 0; i < RAMON_SPFI_DIAG_WORDS; i++) {
		ret = ramon_reg_read(c, win, ramon_spfi_diag_word_index[i] * 4, &d->words[i], st);
		if (ret)
			return ret;
	}
	ret = ramon_get_stats(c, 0, &g, st);
	if (ret)
		return ret;
	for (i = 0; i < RAMON_SPFI_IRQ_VECTORS; i++)
		d->irq_hist[i] = g.spfi_irq_hist[nn][i];
	d->unexpected = g.spfi_unexpected[nn];
	d->timeouts = g.spfi_timeouts[nn];
	d->alert_overrun = g.spfi_alert_overrun[nn];
	return 0;
}

size_t ramon_spfi_diag_format(const struct ramon_spfi_diag *d, char *buf, size_t len)
{
	size_t n = 0;
	unsigned i;

#define OUT(...) do { \
		int k_ = snprintf(buf + n, n < len ? len - n : 0, __VA_ARGS__); \
		if (k_ > 0) \
			n += (size_t)k_; \
	} while (0)
	OUT("words");
	for (i = 0; i < RAMON_SPFI_DIAG_WORDS; i++) {
		OUT(" [%u]=0x%x", ramon_spfi_diag_word_index[i], d->words[i]);
		if (ramon_spfi_diag_word_index[i] == 50) {
			if (d->words[i] & (1u << 5))
				OUT("(VC1-sticky-no-TLAST)");
			if (d->words[i] & (1u << 3))
				OUT("(VC1-FULL)");
			if (d->words[i] & (1u << 1))
				OUT("(VC1-CMD-OVERFLOW)");
		}
	}
	OUT("; irq vectors:");
	for (i = 0; i < RAMON_SPFI_IRQ_VECTORS; i++)
		if (d->irq_hist[i])
			OUT(" 0x%x:%llu", i, (unsigned long long)d->irq_hist[i]);
	OUT("; unexpected %llu, timeouts %llu, alert overruns %llu",
	    (unsigned long long)d->unexpected, (unsigned long long)d->timeouts,
	    (unsigned long long)d->alert_overrun);
#undef OUT
	return n;
}

/* prefixes st->msg with the bring-up stage that failed */
static int prep_stage(struct ramon_status *st, const char *stage, int ret)
{
	char msg[RAMON_MSG_LEN];

	if (ret && st) {
		memcpy(msg, st->msg, sizeof(msg));
		msg[sizeof(msg) - 1] = 0;
		snprintf(st->msg, sizeof(st->msg), "%s: %.*s", stage,
			 (int)(sizeof(st->msg) - strlen(stage) - 3), msg);
	}
	return ret;
}

int ramon_spfi_prep(ramon_ctx *c, uint32_t nn, const struct ramon_spfi_prep_opts *o,
		    struct ramon_spfi_table *t, struct ramon_status *st)
{
	struct ramon_spfi_prep_opts none;
	struct ramon_spfi_reply r;
	uint32_t raw = 0;
	int ret;

	if (!o) {
		memset(&none, 0, sizeof(none));
		o = &none;
	}
	ret = spfi_check(c, nn, st);
	if (ret)
		return prep_stage(st, "setup", ret);
	ret = ramon_spfi_link_up(c, nn, &raw, st);
	if (ret < 0)
		return prep_stage(st, "link", ret);
	if (!ret)
		return ramon__fail(st, RAMON_EL_SPFI_LINK_DOWN, 0, raw, RAMON_SPFI_LINK_UP,
				   "link: spfi%u 0xCC = 0x%x, low byte is not 0x%x", nn, raw,
				   RAMON_SPFI_LINK_UP);
	if (!o->skip_spw_dps) {
		if (!ramon_spw_is_inited(c)) {
			ret = ramon_spw_init(c, NULL, st);
			if (ret && ret != -EALREADY)
				return prep_stage(st, "spw-init", ret);
		}
		ret = ramon_spw_sync(c, nn, NULL, st);
		if (ret)
			return prep_stage(st, "spw-sync", ret);
		ret = ramon_spw_nn_dps(c, nn, 0, st);
		if (ret)
			return prep_stage(st, "dps", ret);
	}
	ret = ramon_spfi_init_nn(c, nn, 0, &r, st);
	if (ret)
		return prep_stage(st, "init", ret);
	ramon__log(c, RAMON_LOG_INFO, "spfi%u INIT: rx_err_code %u init_info 0x%x", nn,
		   r.rx_err_code, r.rx_init_info);
	if (o->format) {
		ret = ramon_spfi_format(c, nn, NULL, st);
		if (ret)
			return prep_stage(st, "format", ret);
	}
	if (t) {
		ret = ramon_spfi_get_table(c, nn, t, st);
		if (ret)
			return prep_stage(st, "table", ret);
	}
	ramon_status_clear(st);
	return 0;
}

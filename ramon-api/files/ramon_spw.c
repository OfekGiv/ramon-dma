// SPDX-License-Identifier: MIT
/* libramon SpaceWire packet layer (ported from the old spw_api.cpp and ramon_smoke) */
#include "ramon_priv.h"

#define SPW_LINK_MASK		0x07u
#define SPW_LINK_SYNCED		0x05u
#define SPW_RX_WAIT_MS		1000	/* RX thread wait period; bounds how long a stop takes */
#define SPW_RX_XFER_MS		3000
#define SPW_ERR_BACKOFF_MS	100
#define SPW_REPLY_CAP		0x10000
#define SPW_DRAIN_FIRST_MS	1
/* l3_len counts L3 + L4 + payload */
#define SPW_L3L4_BYTES		(RAMON_SPW_L3_BYTES + RAMON_SPW_HDR_BYTES - RAMON_SPW_L2L3_BYTES)

#define STAT_ADD(s, f, v)	__atomic_add_fetch(&(s)->stats.f, (v), __ATOMIC_RELAXED)
#define STAT_INC(s, f)		STAT_ADD(s, f, 1)

/* ---- configuration and lifecycle ---- */

void ramon_spw_config_default(struct ramon_spw_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->node_id = RAMON_SPW_NODE_DEFAULT;
	cfg->nn_node = RAMON_SPW_NN_NODE_DEFAULT;
	cfg->rx_buf_bytes = RAMON_SPW_BUF_DEFAULT;
	cfg->tx_slots = 2;
	cfg->tx_slot_bytes = RAMON_SPW_BUF_DEFAULT;
	cfg->sync_tries = 1000;
	cfg->sync_sleep_ms = 10;
}

static void spw_fill_config(struct ramon_spw_config *d, const struct ramon_spw_config *in,
			    const struct ramon_get_info *gi)
{
	struct ramon_spw_config def;

	ramon_spw_config_default(&def);
	*d = in ? *in : def;
	if (!d->node_id)
		d->node_id = def.node_id;
	if (!d->nn_node)
		d->nn_node = def.nn_node;
	if (!d->rx_buf_bytes)
		d->rx_buf_bytes = def.rx_buf_bytes;
	if (!d->tx_slots)
		d->tx_slots = def.tx_slots;
	if (!d->tx_slot_bytes)
		d->tx_slot_bytes = def.tx_slot_bytes;
	if (!d->sync_tries)
		d->sync_tries = def.sync_tries;
	if (!d->sync_sleep_ms)
		d->sync_sleep_ms = def.sync_sleep_ms;
	d->nn_mask = d->nn_mask ? d->nn_mask & gi->spw_mask : gi->spw_mask;
}

void ramon__spw_ctx_init(struct ramon_ctx *c)
{
	uint32_t nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		struct spw_nn *s = &c->spw.nn[nn];

		s->ctx = c;
		s->nn = nn;
		snprintf(s->win, sizeof(s->win), "spw%u", nn);
		pthread_mutex_init(&s->lock, NULL);
		ramon__cond_init(&s->cond);
	}
	pthread_mutex_init(&c->spw.pool_lock, NULL);
	ramon__cond_init(&c->spw.pool_cond);
	spw_fill_config(&c->spw.cfg, NULL, &c->info);
}

void ramon__spw_ctx_destroy(struct ramon_ctx *c)
{
	uint32_t nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		pthread_mutex_destroy(&c->spw.nn[nn].lock);
		pthread_cond_destroy(&c->spw.nn[nn].cond);
	}
	pthread_mutex_destroy(&c->spw.pool_lock);
	pthread_cond_destroy(&c->spw.pool_cond);
}

int ramon_spw_is_inited(const ramon_ctx *c)
{
	return c->spw.inited;
}

void ramon_spw_get_config(const ramon_ctx *c, struct ramon_spw_config *cfg)
{
	*cfg = c->spw.cfg;
}

int ramon_spw_present(const ramon_ctx *c, uint32_t nn)
{
	return nn < RAMON_NN_COUNT && (c->spw.cfg.nn_mask & (1u << nn));
}

static int spw_check(struct ramon_ctx *c, uint32_t nn, int need_init, struct ramon_status *st)
{
	if (nn >= RAMON_NN_COUNT)
		return ramon__fail(st, RAMON_EL_INVAL, 0, nn, 0, "no SPW NN %u", nn);
	if (!(c->info.spw_mask & (1u << nn)))
		return ramon__fail(st, RAMON_EL_NOT_PRESENT, 0, nn, c->info.spw_mask,
				   "spw%u is not present (spw_mask 0x%x)", nn, c->info.spw_mask);
	if (!need_init)
		return 0;
	if (!c->spw.inited)
		return ramon__fail(st, RAMON_EL_NOT_INITED, 0, 0, 0, "ramon_spw_init() not called");
	if (!c->spw.nn[nn].present)
		return ramon__fail(st, RAMON_EL_NOT_PRESENT, 0, nn, c->spw.cfg.nn_mask,
				   "spw%u is not enabled (nn_mask 0x%x)", nn, c->spw.cfg.nn_mask);
	return 0;
}

static void spw_free_all(struct ramon_ctx *c)
{
	struct spw_state *w = &c->spw;
	unsigned i;
	uint32_t nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		ramon_buf_free(&w->nn[nn].rxbuf, NULL);
		free(w->nn[nn].rxcopy);
		w->nn[nn].rxcopy = NULL;
		w->nn[nn].present = 0;
	}
	for (i = 0; w->slot && i < w->n_slots; i++)
		ramon_buf_free(&w->slot[i], NULL);
	free(w->slot);
	free(w->busy);
	w->slot = NULL;
	w->busy = NULL;
	w->n_slots = 0;
}

int ramon_spw_init(ramon_ctx *c, const struct ramon_spw_config *cfg, struct ramon_status *st)
{
	struct spw_state *w = &c->spw;
	struct ramon_spw_config d;
	unsigned i;
	uint32_t nn;
	int ret = 0;

	pthread_mutex_lock(&c->lock);
	if (w->inited) {
		pthread_mutex_unlock(&c->lock);
		return ramon__fail(st, RAMON_EL_BUSY, EALREADY, 0, 0, "SPW layer already initialised");
	}
	spw_fill_config(&d, cfg, &c->info);
	if (d.tx_slot_bytes <= RAMON_SPW_HDR_BYTES || d.rx_buf_bytes < RAMON_SPW_HDR_BYTES) {
		pthread_mutex_unlock(&c->lock);
		return ramon__fail(st, RAMON_EL_INVAL, 0, d.tx_slot_bytes, d.rx_buf_bytes,
				   "tx_slot_bytes %u / rx_buf_bytes %u too small", d.tx_slot_bytes,
				   d.rx_buf_bytes);
	}
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		struct spw_nn *s = &w->nn[nn];

		s->present = 0;
		memset(&s->stats, 0, sizeof(s->stats));
		if (!(d.nn_mask & (1u << nn)))
			continue;
		ret = ramon_axi_find_pair(c, nn, &s->rx_chan, &s->tx_chan, st);
		if (!ret)
			ret = ramon_buf_alloc(c, d.rx_buf_bytes, 0, &s->rxbuf, st);
		if (ret)
			goto err;
		s->rxcopy = malloc(s->rxbuf.size);
		if (!s->rxcopy) {
			ret = ramon__fail(st, RAMON_EL_SYS, ENOMEM, 0, 0, "out of memory");
			goto err;
		}
		s->present = 1;
	}
	w->slot = calloc(d.tx_slots, sizeof(*w->slot));
	w->busy = calloc(d.tx_slots, 1);
	if (!w->slot || !w->busy) {
		ret = ramon__fail(st, RAMON_EL_SYS, ENOMEM, 0, 0, "out of memory");
		goto err;
	}
	w->n_slots = d.tx_slots;
	for (i = 0; i < d.tx_slots; i++) {
		ret = ramon_buf_alloc(c, d.tx_slot_bytes, 0, &w->slot[i], st);
		if (ret)
			goto err;
	}
	w->cfg = d;
	w->inited = 1;
	pthread_mutex_unlock(&c->lock);
	ramon_status_clear(st);
	return 0;
err:
	spw_free_all(c);
	pthread_mutex_unlock(&c->lock);
	return ret;
}

void ramon_spw_fini(ramon_ctx *c)
{
	uint32_t nn;

	if (!c->spw.inited)
		return;
	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		ramon_spw_rx_stop(c, nn);
	pthread_mutex_lock(&c->lock);
	spw_free_all(c);
	c->spw.inited = 0;
	spw_fill_config(&c->spw.cfg, NULL, &c->info);
	pthread_mutex_unlock(&c->lock);
}

/* ---- links ---- */

int ramon_spw_link_status(ramon_ctx *c, uint32_t nn, uint32_t *raw, struct ramon_status *st)
{
	uint32_t v;
	int ret;

	ret = spw_check(c, nn, 0, st);
	if (!ret)
		ret = ramon_reg_read(c, c->spw.nn[nn].win, RAMON_SPW_REG_LINK, &v, st);
	if (ret)
		return ret;
	if (raw)
		*raw = v;
	return (v & SPW_LINK_MASK) == SPW_LINK_SYNCED;
}

int ramon_spw_sync(ramon_ctx *c, uint32_t nn, uint32_t *tries, struct ramon_status *st)
{
	const struct ramon_spw_config *cfg = &c->spw.cfg;
	uint32_t i, v = 0;
	int ret;

	for (i = 0; i < cfg->sync_tries; i++) {
		ret = ramon_spw_link_status(c, nn, &v, st);
		if (ret < 0)
			return ret;
		if (ret == 1) {
			if (tries)
				*tries = i;
			ramon_status_clear(st);
			return 0;
		}
		ret = ramon_reg_write(c, c->spw.nn[nn].win, RAMON_SPW_REG_RESET, cfg->reset_value, st);
		if (ret)
			return ret;
		ramon__sleep_ms(cfg->sync_sleep_ms);
	}
	if (tries)
		*tries = i;
	return ramon__fail(st, RAMON_EL_SPW_LINK_DOWN, 0, v, i,
			   "spw%u: not synced after %u resets (0x28 = 0x%x)", nn, i, v);
}

int ramon_spw_loopback_get(ramon_ctx *c, uint32_t nn, int *enabled, struct ramon_status *st)
{
	uint32_t v;
	int ret;

	ret = spw_check(c, nn, 0, st);
	if (!ret)
		ret = ramon_reg_read(c, c->spw.nn[nn].win, RAMON_SPW_REG_LOOPBACK, &v, st);
	if (!ret)
		*enabled = v != 0;
	return ret;
}

int ramon_spw_loopback(ramon_ctx *c, uint32_t nn, int enable, struct ramon_status *st)
{
	struct ramon_spw_loopback l;
	int cur, ret;

	ret = ramon_spw_loopback_get(c, nn, &cur, st);
	if (ret)
		return ret;
	memset(&l, 0, sizeof(l));
	l.nn = nn;
	l.enable = enable ? 1 : 0;
	ret = ramon__ioctl(c, RAMON_IOC_SPW_LOOPBACK, &l, &l.st, st);
	if (ret || cur == !!enable)
		return ret;
	/* the driver reset the link */
	if (enable) {
		ramon__sleep_ms(c->spw.cfg.sync_sleep_ms);
		return 0;
	}
	return ramon_spw_sync(c, nn, NULL, st);
}

/* ---- packets ---- */

uint32_t ramon_spw_header_crc(const struct ramon_spw_hdr *h)
{
	struct ramon_spw_hdr t;

	memcpy(&t, h, sizeof(t));
	t.header_crc = 0;
	t.payload_crc = 0;
	return ramon_crc32(&t, RAMON_SPW_L2L3_BYTES);
}

int ramon_spw_build(uint8_t src_node, const struct ramon_spw_msg *m, const struct ramon_iov *iov,
		    unsigned n_iov, void *out, size_t cap, uint32_t *out_len)
{
	struct ramon_spw_hdr h;
	uint8_t *p = out;
	size_t payload = 0, off;
	uint32_t crc;
	unsigned i;

	for (i = 0; i < n_iov; i++) {
		if (iov[i].len && !iov[i].base)
			return -EINVAL;
		payload += iov[i].len;
	}
	if (payload > cap || RAMON_SPW_HDR_BYTES > cap - payload || payload > 0x7FFFFFFF)
		return -EMSGSIZE;
	memset(&h, 0, sizeof(h));
	h.dst = m->dst;
	h.src = src_node;
	h.protocol_id = m->protocol_id;
	h.nack_id = m->nack_id;
	h.l3_len = SPW_L3L4_BYTES + (uint32_t)payload;
	h.packet_id = m->packet_id;
	h.app_type = m->app_type;
	h.attribute_id = m->attribute_id;
	/* payload CRC over L4 + payload, computed from the (cached) sources */
	crc = ramon_crc32((const uint8_t *)&h + RAMON_SPW_L2L3_BYTES,
			  RAMON_SPW_HDR_BYTES - RAMON_SPW_L2L3_BYTES);
	for (i = 0; i < n_iov; i++)
		crc = ramon_crc32_update(crc, iov[i].base, iov[i].len);
	h.payload_crc = crc;
	h.header_crc = ramon_spw_header_crc(&h);
	memcpy(p, &h, sizeof(h));
	for (i = 0, off = RAMON_SPW_HDR_BYTES; i < n_iov; off += iov[i].len, i++)
		if (iov[i].len)
			memcpy(p + off, iov[i].base, iov[i].len);
	if (out_len)
		*out_len = (uint32_t)(RAMON_SPW_HDR_BYTES + payload);
	return 0;
}

int ramon_spw_parse(const void *raw, uint32_t raw_size, uint8_t our_node, struct ramon_spw_rx *rx)
{
	const uint8_t *p = raw;
	const struct ramon_spw_hdr *h = raw;
	uint32_t area;

	memset(rx, 0, sizeof(*rx));
	rx->raw = p;
	rx->raw_size = raw_size;
	if (raw_size < RAMON_SPW_HDR_BYTES) {
		rx->flags = RAMON_SPW_RX_TRUNCATED;
		return -EINVAL;
	}
	rx->hdr = h;
	rx->payload = p + RAMON_SPW_HDR_BYTES;
	if (raw_size < RAMON_SPW_HDR_BYTES + RAMON_SPW_FOOTER_BYTES) {
		rx->flags |= RAMON_SPW_RX_TRUNCATED;
		rx->payload_len = raw_size - RAMON_SPW_HDR_BYTES;
	} else {
		memcpy(&rx->footer, p + raw_size - RAMON_SPW_FOOTER_BYTES, sizeof(rx->footer));
		area = raw_size - RAMON_SPW_HDR_BYTES - RAMON_SPW_FOOTER_BYTES;
		if (h->l3_len < SPW_L3L4_BYTES || h->l3_len - SPW_L3L4_BYTES > area) {
			rx->flags |= RAMON_SPW_RX_TRUNCATED;
			rx->payload_len = area;
		} else {
			rx->payload_len = h->l3_len - SPW_L3L4_BYTES;
			if (ramon_crc32(p + RAMON_SPW_L2L3_BYTES, h->l3_len - RAMON_SPW_L3_BYTES) !=
			    h->payload_crc)
				rx->flags |= RAMON_SPW_RX_PAYLOAD_CRC_BAD;
		}
		if (rx->footer.error_flags)
			rx->flags |= RAMON_SPW_RX_FOOTER_ERR;
	}
	if (ramon_spw_header_crc(h) != h->header_crc)
		rx->flags |= RAMON_SPW_RX_HDR_CRC_BAD;
	if (h->dst != our_node)
		rx->flags |= RAMON_SPW_RX_NOT_FOR_US;
	return 0;
}

/* ---- TX ---- */

static unsigned spw_slot_get(struct spw_state *w)
{
	unsigned i;

	pthread_mutex_lock(&w->pool_lock);
	for (;;) {
		for (i = 0; i < w->n_slots; i++)
			if (!w->busy[i]) {
				w->busy[i] = 1;
				pthread_mutex_unlock(&w->pool_lock);
				return i;
			}
		pthread_cond_wait(&w->pool_cond, &w->pool_lock);
	}
}

static void spw_slot_put(struct spw_state *w, unsigned i)
{
	pthread_mutex_lock(&w->pool_lock);
	w->busy[i] = 0;
	pthread_cond_signal(&w->pool_cond);
	pthread_mutex_unlock(&w->pool_lock);
}

static void spw_tx_account(struct spw_nn *s, int ret, uint32_t len)
{
	if (ret) {
		STAT_INC(s, tx_failures);
		return;
	}
	STAT_INC(s, tx_packets);
	STAT_ADD(s, tx_bytes, len);
}

int ramon_spw_send(ramon_ctx *c, uint32_t nn, const struct ramon_spw_msg *m,
		   const struct ramon_iov *iov, unsigned n_iov, uint32_t timeout_ms,
		   struct ramon_status *st)
{
	struct spw_state *w = &c->spw;
	struct spw_nn *s = &w->nn[nn < RAMON_NN_COUNT ? nn : 0];
	struct ramon_spw_msg mm = *m;
	uint64_t payload = 0;
	uint32_t len;
	unsigned i, slot;
	int ret;

	ret = spw_check(c, nn, 1, st);
	if (ret)
		return ret;
	for (i = 0; i < n_iov; i++)
		payload += iov[i].len;
	if (payload + RAMON_SPW_HDR_BYTES > w->cfg.tx_slot_bytes)
		return ramon__fail(st, RAMON_EL_TOO_LARGE, 0, payload, w->cfg.tx_slot_bytes,
				   "payload of %llu bytes + header > tx_slot_bytes %u",
				   (unsigned long long)payload, w->cfg.tx_slot_bytes);
	if (!(m->flags & RAMON_SPW_MSG_KEEP_ID))
		mm.packet_id = (uint16_t)__atomic_fetch_add(&w->packet_id, 1, __ATOMIC_RELAXED);
	slot = spw_slot_get(w);
	ret = ramon_spw_build(w->cfg.node_id, &mm, iov, n_iov, w->slot[slot].ptr,
			      w->slot[slot].size, &len);
	if (ret)
		ret = ramon__fail(st, RAMON_EL_INVAL, -ret, 0, 0, "bad payload gather list");
	else
		ret = ramon_axi_xfer1(c, s->tx_chan, &w->slot[slot], 0, len, timeout_ms, st);
	spw_slot_put(w, slot);
	spw_tx_account(s, ret, len);
	return ret;
}

int ramon_spw_send_buf(ramon_ctx *c, uint32_t nn, const ramon_buf *pkt, uint64_t off, uint32_t len,
		       uint32_t timeout_ms, struct ramon_status *st)
{
	struct spw_nn *s = &c->spw.nn[nn < RAMON_NN_COUNT ? nn : 0];
	int ret;

	ret = spw_check(c, nn, 1, st);
	if (ret)
		return ret;
	if (len < RAMON_SPW_HDR_BYTES)
		return ramon__fail(st, RAMON_EL_INVAL, 0, len, 0, "packet of %u bytes has no header",
				   len);
	ret = ramon_axi_xfer1(c, s->tx_chan, pkt, off, len, timeout_ms, st);
	spw_tx_account(s, ret, len);
	return ret;
}

/* ---- RX ---- */

/*
 * Moves the packet whose size SPW_WAIT_RX returned into a cached buffer:
 * s->rxcopy, or a malloc'd one (*tmp set) when it is larger than rxbuf.
 * Every popped size must be followed by exactly this transfer.
 */
static int spw_consume(struct ramon_ctx *c, struct spw_nn *s, uint32_t size, uint8_t **data,
		       int *tmp, struct ramon_status *st)
{
	ramon_buf big, *b = &s->rxbuf;
	uint8_t *dst = s->rxcopy;
	int ret;

	*tmp = 0;
	if (!size) {
		STAT_INC(s, rx_consume_failures);
		return ramon__fail(st, RAMON_EL_SPW_BAD_PACKET, 0, 0, 0, "spw%u: zero-size packet",
				   s->nn);
	}
	memset(&big, 0, sizeof(big));
	if (size > s->rxbuf.size) {
		STAT_INC(s, rx_oversize);
		dst = malloc(size);
		ret = dst ? ramon_buf_alloc(c, size, 0, &big, st) :
			    ramon__fail(st, RAMON_EL_SYS, ENOMEM, size, 0, "out of memory");
		if (ret) {
			/* the packet stays in the FPGA: the stream is out of step from here */
			free(dst);
			STAT_INC(s, rx_consume_failures);
			ramon__log(c, RAMON_LOG_ERR, "spw%u: no buffer for a %u-byte packet", s->nn,
				   size);
			return ret;
		}
		b = &big;
	}
	ret = ramon_axi_xfer1(c, s->rx_chan, b, 0, size, SPW_RX_XFER_MS, st);
	if (!ret)
		memcpy(dst, b->ptr, size);
	ramon_buf_free(&big, NULL);
	if (ret) {
		if (dst != s->rxcopy)
			free(dst);
		STAT_INC(s, rx_consume_failures);
		ramon__log(c, RAMON_LOG_ERR, "spw%u: RX transfer of %u bytes failed", s->nn, size);
		return ret;
	}
	*data = dst;
	*tmp = dst != s->rxcopy;
	return 0;
}

static void spw_parse_account(struct ramon_ctx *c, struct spw_nn *s, const uint8_t *data,
			      uint32_t size, struct ramon_spw_rx *rx)
{
	ramon_spw_parse(data, size, c->spw.cfg.node_id, rx);
	rx->nn = s->nn;
	if (c->spw.cfg.flags & RAMON_SPW_ACCEPT_ANY_DST)
		rx->flags &= ~RAMON_SPW_RX_NOT_FOR_US;
	STAT_INC(s, rx_packets);
	STAT_ADD(s, rx_bytes, size);
	if (rx->flags & (RAMON_SPW_RX_HDR_CRC_BAD | RAMON_SPW_RX_PAYLOAD_CRC_BAD))
		STAT_INC(s, rx_crc_errors);
	if (rx->flags & RAMON_SPW_RX_TRUNCATED)
		STAT_INC(s, rx_truncated);
	if (rx->flags & RAMON_SPW_RX_NOT_FOR_US)
		STAT_INC(s, rx_not_for_us);
	if (rx->flags & RAMON_SPW_RX_FOOTER_ERR)
		STAT_INC(s, rx_footer_errors);
}

static int spw_match(const struct ramon_spw_match *w, const struct ramon_spw_rx *rx)
{
	if (!w->any_src && rx->hdr->src != w->src)
		return 0;
	if (!w->any_app && rx->hdr->app_type != w->app_type)
		return 0;
	if (!w->any_attr && rx->hdr->attribute_id != w->attribute_id)
		return 0;
	return 1;
}

/* is rx the reply a request waits for */
static int spw_wanted(struct ramon_ctx *c, int any, const struct ramon_spw_match *want,
		      const struct ramon_spw_rx *rx)
{
	if (!rx->hdr || (rx->flags & RAMON_SPW_RX_NOT_FOR_US))
		return 0;
	if ((rx->flags & RAMON_SPW_RX_BAD) && !(c->spw.cfg.flags & RAMON_SPW_NO_CRC_CHECK))
		return 0;
	return any || spw_match(want, rx);
}

/* RX thread: hand a consumed packet to a waiting request or to the callback */
static void spw_deliver(struct ramon_ctx *c, struct spw_nn *s, uint32_t size)
{
	struct spw_waiter *w = &s->waiter;
	struct ramon_spw_rx rx;
	ramon_spw_rx_cb cb;
	uint8_t *data;
	void *user;
	int tmp;

	if (spw_consume(c, s, size, &data, &tmp, NULL))
		return;
	spw_parse_account(c, s, data, size, &rx);
	pthread_mutex_lock(&s->lock);
	if (w->armed && !w->done && spw_wanted(c, w->any, &w->want, &rx)) {
		if (size <= w->cap) {
			memcpy(w->buf, data, size);
			ramon_spw_parse(w->buf, size, c->spw.cfg.node_id, &w->rx);
			w->rx.nn = s->nn;
			w->rx.flags = rx.flags;
			w->err = 0;
		} else {
			w->err = ramon__fail(&w->st, RAMON_EL_TOO_LARGE, 0, size, w->cap,
					     "reply of %u bytes, buffer of %u", size, w->cap);
		}
		w->done = 1;
		pthread_cond_broadcast(&s->cond);
		pthread_mutex_unlock(&s->lock);
		STAT_INC(s, rx_to_request);
		goto out;
	}
	cb = s->cb;
	user = s->cb_user;
	pthread_mutex_unlock(&s->lock);
	if (cb)
		cb(user, &rx);
	else
		STAT_INC(s, rx_dropped);
out:
	if (tmp)
		free(data);
}

static int spw_state(struct spw_nn *s)
{
	int state;

	pthread_mutex_lock(&s->lock);
	state = s->state;
	pthread_mutex_unlock(&s->lock);
	return state;
}

static void *spw_rx_main(void *arg)
{
	struct spw_nn *s = arg;
	struct ramon_ctx *c = s->ctx;
	struct ramon_spw_wait_rx w;
	int ret;

	while (spw_state(s) == SPW_RX_RUNNING) {
		memset(&w, 0, sizeof(w));
		w.nn = s->nn;
		w.timeout_ms = SPW_RX_WAIT_MS;
		ret = ramon__ioctl(c, RAMON_IOC_SPW_WAIT_RX, &w, &w.st, NULL);
		if (!ret) {
			spw_deliver(c, s, w.size);
			continue;
		}
		if (ret == -ETIMEDOUT || ret == -EINTR)
			continue;
		if (ret == -ECANCELED) {
			if (spw_state(s) == SPW_RX_RUNNING)
				ramon__log(c, RAMON_LOG_INFO,
					   "spw%u: RX wait cancelled by another SPW_CANCEL user", s->nn);
			continue;
		}
		if (ret == -ENODEV) {
			ramon__log(c, RAMON_LOG_ERR, "spw%u: device removed, RX thread stops", s->nn);
			break;
		}
		STAT_INC(s, rx_wait_errors);
		ramon__log(c, RAMON_LOG_WARN, "spw%u: SPW_WAIT_RX: %s", s->nn,
			   ramon_err_str(w.st.code));
		ramon__sleep_ms(SPW_ERR_BACKOFF_MS);
	}
	pthread_mutex_lock(&s->lock);
	s->thread_done = 1;
	pthread_mutex_unlock(&s->lock);
	return NULL;
}

int ramon_spw_rx_start(ramon_ctx *c, uint32_t nn, ramon_spw_rx_cb cb, void *user,
		       struct ramon_status *st)
{
	struct spw_nn *s;
	int ret;

	ret = spw_check(c, nn, 1, st);
	if (ret)
		return ret;
	s = &c->spw.nn[nn];
	pthread_mutex_lock(&s->lock);
	if (s->thread_valid && !s->thread_done) {
		pthread_mutex_unlock(&s->lock);
		return ramon__fail(st, RAMON_EL_BUSY, 0, nn, 0, "spw%u: RX thread already running", nn);
	}
	if (s->receiver) {
		pthread_mutex_unlock(&s->lock);
		return ramon__fail(st, RAMON_EL_BUSY, 0, nn, 0, "spw%u: a receiver is active", nn);
	}
	if (s->thread_valid) {
		/* a thread that ended by itself (device removed) */
		pthread_mutex_unlock(&s->lock);
		pthread_join(s->thread, NULL);
		pthread_mutex_lock(&s->lock);
		s->thread_valid = 0;
	}
	s->cb = cb;
	s->cb_user = user;
	s->state = SPW_RX_RUNNING;
	s->thread_done = 0;
	ret = ramon__thread_start(&s->thread, spw_rx_main, s);
	if (ret) {
		s->state = SPW_RX_NONE;
		pthread_mutex_unlock(&s->lock);
		return ramon__fail(st, RAMON_EL_SYS, -ret, 0, 0, "pthread_create: %s", strerror(-ret));
	}
	s->thread_valid = 1;
	pthread_mutex_unlock(&s->lock);
	ramon_status_clear(st);
	return 0;
}

int ramon_spw_rx_stop(ramon_ctx *c, uint32_t nn)
{
	struct ramon_spw_cancel k;
	struct spw_nn *s;

	if (nn >= RAMON_NN_COUNT)
		return -EINVAL;
	s = &c->spw.nn[nn];
	pthread_mutex_lock(&s->lock);
	if (!s->thread_valid) {
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (pthread_equal(pthread_self(), s->thread)) {
		pthread_mutex_unlock(&s->lock);
		return -EDEADLK;
	}
	s->state = SPW_RX_STOPPING;
	pthread_mutex_unlock(&s->lock);
	memset(&k, 0, sizeof(k));
	k.nn = nn;
	ramon__ioctl(c, RAMON_IOC_SPW_CANCEL, &k, &k.st, NULL);
	pthread_join(s->thread, NULL);
	pthread_mutex_lock(&s->lock);
	s->thread_valid = 0;
	s->state = SPW_RX_NONE;
	s->cb = NULL;
	s->cb_user = NULL;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

int ramon_spw_rx_running(ramon_ctx *c, uint32_t nn)
{
	struct spw_nn *s;
	int r;

	if (nn >= RAMON_NN_COUNT)
		return 0;
	s = &c->spw.nn[nn];
	pthread_mutex_lock(&s->lock);
	r = s->thread_valid && s->state == SPW_RX_RUNNING && !s->thread_done;
	pthread_mutex_unlock(&s->lock);
	return r;
}

/* mode B: the calling thread becomes the only consumer of the NN */
static int spw_claim(struct spw_nn *s, struct ramon_status *st)
{
	pthread_mutex_lock(&s->lock);
	if (s->thread_valid || s->receiver) {
		pthread_mutex_unlock(&s->lock);
		return ramon__fail(st, RAMON_EL_BUSY, 0, s->nn, 0,
				   "spw%u: an RX thread or another receiver owns the NN", s->nn);
	}
	s->receiver = 1;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

static void spw_release(struct spw_nn *s)
{
	pthread_mutex_lock(&s->lock);
	s->receiver = 0;
	pthread_mutex_unlock(&s->lock);
}

/* one packet into buf (or only consumed and dropped when buf is NULL) */
static int spw_recv_one(struct ramon_ctx *c, struct spw_nn *s, void *buf, uint32_t cap,
			uint32_t timeout_ms, struct ramon_spw_rx *rx, struct ramon_status *st)
{
	struct ramon_spw_wait_rx w;
	struct ramon_spw_rx tmp_rx;
	uint8_t *data;
	int ret, tmp;

	memset(&w, 0, sizeof(w));
	w.nn = s->nn;
	w.timeout_ms = timeout_ms;
	ret = ramon__ioctl(c, RAMON_IOC_SPW_WAIT_RX, &w, &w.st, st);
	if (ret)
		return ret;
	ret = spw_consume(c, s, w.size, &data, &tmp, st);
	if (ret)
		return ret;
	spw_parse_account(c, s, data, w.size, &tmp_rx);
	if (!buf) {
		STAT_INC(s, rx_dropped);
	} else if (w.size > cap) {
		STAT_INC(s, rx_dropped);
		ret = ramon__fail(st, RAMON_EL_TOO_LARGE, 0, w.size, cap,
				  "spw%u: packet of %u bytes, buffer of %u", s->nn, w.size, cap);
	} else {
		memcpy(buf, data, w.size);
		ramon_spw_parse(buf, w.size, c->spw.cfg.node_id, rx);
		rx->nn = s->nn;
		rx->flags = tmp_rx.flags;
	}
	if (tmp)
		free(data);
	return ret;
}

int ramon_spw_recv(ramon_ctx *c, uint32_t nn, void *buf, uint32_t cap, uint32_t timeout_ms,
		   struct ramon_spw_rx *rx, struct ramon_status *st)
{
	struct spw_nn *s;
	int ret;

	ret = spw_check(c, nn, 1, st);
	if (ret)
		return ret;
	s = &c->spw.nn[nn];
	ret = spw_claim(s, st);
	if (ret)
		return ret;
	ret = spw_recv_one(c, s, buf, cap, timeout_ms, rx, st);
	spw_release(s);
	if (!ret)
		ramon_status_clear(st);
	return ret;
}

static int spw_drain_claimed(struct ramon_ctx *c, struct spw_nn *s, uint32_t idle_ms,
			     uint32_t *n_dropped, struct ramon_status *st)
{
	uint32_t n = 0;
	int ret;

	for (;;) {
		ret = spw_recv_one(c, s, NULL, 0, idle_ms, NULL, st);
		if (ret)
			break;
		n++;
	}
	if (n_dropped)
		*n_dropped = n;
	if (ret == -ETIMEDOUT) {
		ramon_status_clear(st);
		return 0;
	}
	return ret;
}

int ramon_spw_drain(ramon_ctx *c, uint32_t nn, uint32_t idle_ms, uint32_t *n_dropped,
		    struct ramon_status *st)
{
	struct spw_nn *s;
	int ret;

	if (n_dropped)
		*n_dropped = 0;
	ret = spw_check(c, nn, 1, st);
	if (ret)
		return ret;
	s = &c->spw.nn[nn];
	ret = spw_claim(s, st);
	if (ret)
		return ret;
	ret = spw_drain_claimed(c, s, idle_ms ? idle_ms : 1, n_dropped, st);
	spw_release(s);
	return ret;
}

/* ---- request / reply ---- */

static int spw_request_thread(struct ramon_ctx *c, struct spw_nn *s, const struct ramon_spw_msg *m,
			      const struct ramon_iov *iov, unsigned n_iov,
			      const struct ramon_spw_match *want, void *reply, uint32_t cap,
			      uint32_t timeout_ms, struct ramon_spw_rx *rx, struct ramon_status *st)
{
	struct spw_waiter *w = &s->waiter;
	struct timespec ts;
	int ret;

	/* s->lock is held on entry */
	if (w->armed) {
		pthread_mutex_unlock(&s->lock);
		return ramon__fail(st, RAMON_EL_BUSY, 0, s->nn, 0,
				   "spw%u: another request is waiting for its reply", s->nn);
	}
	memset(w, 0, sizeof(*w));
	w->armed = 1;
	w->any = !want;
	if (want)
		w->want = *want;
	w->buf = reply;
	w->cap = cap;
	pthread_mutex_unlock(&s->lock);

	ret = ramon_spw_send(c, s->nn, m, iov, n_iov, 0, st);
	ramon__deadline(&ts, timeout_ms);
	pthread_mutex_lock(&s->lock);
	while (!ret && !w->done)
		if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) == ETIMEDOUT)
			break;
	if (!ret) {
		if (!w->done) {
			ret = ramon__fail(st, RAMON_EL_SPW_REPLY_TIMEOUT, 0, m->dst, timeout_ms,
					  "spw%u: no reply from node 0x%x within %u ms", s->nn,
					  m->dst, timeout_ms);
		} else if (w->err) {
			ret = w->err;
			if (st)
				*st = w->st;
		} else if (rx) {
			*rx = w->rx;
		}
	}
	w->armed = 0;
	w->done = 0;
	pthread_mutex_unlock(&s->lock);
	return ret;
}

int ramon_spw_request(ramon_ctx *c, uint32_t nn, const struct ramon_spw_msg *m,
		      const struct ramon_iov *iov, unsigned n_iov,
		      const struct ramon_spw_match *want, void *reply, uint32_t cap,
		      uint32_t timeout_ms, struct ramon_spw_rx *rx, struct ramon_status *st)
{
	struct ramon_spw_rx got;
	struct spw_nn *s;
	uint64_t end;
	int ret;

	ret = spw_check(c, nn, 1, st);
	if (ret)
		return ret;
	if (!timeout_ms)
		timeout_ms = RAMON_SPW_REPLY_MS;
	s = &c->spw.nn[nn];
	pthread_mutex_lock(&s->lock);
	if (s->thread_valid && s->state == SPW_RX_RUNNING && !s->thread_done)
		return spw_request_thread(c, s, m, iov, n_iov, want, reply, cap, timeout_ms, rx, st);
	pthread_mutex_unlock(&s->lock);

	/* no RX thread: this thread receives */
	ret = spw_claim(s, st);
	if (ret)
		return ret;
	ret = spw_drain_claimed(c, s, SPW_DRAIN_FIRST_MS, NULL, st);
	if (!ret)
		ret = ramon_spw_send(c, nn, m, iov, n_iov, 0, st);
	end = ramon__now_ns() + (uint64_t)timeout_ms * 1000000ull;
	while (!ret) {
		uint64_t now = ramon__now_ns();
		uint32_t left = now < end ? (uint32_t)((end - now + 999999) / 1000000) : 0;

		if (!left) {
			ret = -ETIMEDOUT;
			break;
		}
		ret = spw_recv_one(c, s, reply, cap, left, &got, st);
		if (ret)
			break;
		if (spw_wanted(c, !want, want, &got)) {
			if (rx)
				*rx = got;
			break;
		}
		STAT_INC(s, rx_dropped);
	}
	spw_release(s);
	if (ret == -ETIMEDOUT)
		ret = ramon__fail(st, RAMON_EL_SPW_REPLY_TIMEOUT, 0, m->dst, timeout_ms,
				  "spw%u: no reply from node 0x%x within %u ms", nn, m->dst,
				  timeout_ms);
	else if (!ret)
		ramon_status_clear(st);
	return ret;
}

/* ---- NN services ---- */

static void spw_nn_msg(struct ramon_ctx *c, struct ramon_spw_msg *m, uint16_t app, uint32_t attr)
{
	memset(m, 0, sizeof(*m));
	m->dst = c->spw.cfg.nn_node;
	m->protocol_id = RAMON_SPW_NN_PROTOCOL;
	m->app_type = app;
	m->attribute_id = attr;
}

/* request to the NN; *reply is malloc'd (free it) */
static int spw_nn_request(struct ramon_ctx *c, uint32_t nn, const struct ramon_spw_msg *m,
			  const void *data, uint32_t len, uint32_t timeout_ms, uint8_t **reply,
			  struct ramon_spw_rx *rx, struct ramon_status *st)
{
	struct ramon_iov iov = { data, len };
	int ret;

	*reply = malloc(SPW_REPLY_CAP);
	if (!*reply)
		return ramon__fail(st, RAMON_EL_SYS, ENOMEM, 0, 0, "out of memory");
	ret = ramon_spw_request(c, nn, m, &iov, len ? 1 : 0, NULL, *reply, SPW_REPLY_CAP,
				timeout_ms, rx, st);
	if (ret) {
		free(*reply);
		*reply = NULL;
	}
	return ret;
}

static void copy_field(char *dst, const uint8_t *src, size_t n)
{
	memcpy(dst, src, n);
	dst[n] = 0;
}

int ramon_spw_nn_version(ramon_ctx *c, uint32_t nn, struct ramon_nn_version *v, uint32_t timeout_ms,
			 struct ramon_status *st)
{
	struct ramon_spw_msg m;
	struct ramon_spw_rx rx;
	uint8_t *reply;
	int ret;

	memset(v, 0, sizeof(*v));
	spw_nn_msg(c, &m, RAMON_SPW_VERSION_APP, RAMON_SPW_VERSION_ATTR);
	ret = spw_nn_request(c, nn, &m, NULL, 0, timeout_ms, &reply, &rx, st);
	if (ret)
		return ret;
	if (rx.payload_len < 3 * 200 + 20) {
		ret = ramon__fail(st, RAMON_EL_SPW_BAD_PACKET, 0, rx.payload_len, 620,
				  "version reply has %u payload bytes, expected 620", rx.payload_len);
	} else {
		copy_field(v->kbl, rx.payload, 200);
		copy_field(v->rsbl, rx.payload + 200, 200);
		copy_field(v->image, rx.payload + 400, 200);
		copy_field(v->fw, rx.payload + 600, 20);
	}
	free(reply);
	return ret;
}

/* request whose answer must carry attribute 0x101 */
static int spw_nn_ack(struct ramon_ctx *c, uint32_t nn, uint16_t app, uint32_t attr,
		      const void *data, uint32_t len, uint32_t timeout_ms, const char *what,
		      struct ramon_status *st)
{
	struct ramon_spw_msg m;
	struct ramon_spw_rx rx;
	uint8_t *reply;
	uint32_t got;
	int ret;

	spw_nn_msg(c, &m, app, attr);
	ret = spw_nn_request(c, nn, &m, data, len, timeout_ms, &reply, &rx, st);
	if (ret)
		return ret;
	got = rx.hdr->attribute_id;
	free(reply);
	if (got != RAMON_SPW_ACK_ATTR)
		return ramon__fail(st, RAMON_EL_SPW_NACK, 0, got, RAMON_SPW_ACK_ATTR,
				   "%s answered with attribute 0x%x, expected 0x%x", what, got,
				   RAMON_SPW_ACK_ATTR);
	return 0;
}

int ramon_spw_nn_dps(ramon_ctx *c, uint32_t nn, uint32_t timeout_ms, struct ramon_status *st)
{
	return spw_nn_ack(c, nn, RAMON_SPW_DPS_APP, RAMON_SPW_DPS_ATTR, NULL, 0, timeout_ms, "DPS",
			  st);
}

int ramon_spw_nn_config_push(ramon_ctx *c, uint32_t nn, const void *data, uint32_t len,
			     uint32_t timeout_ms, struct ramon_status *st)
{
	if (!len || len > RAMON_SPW_CONFIG_MAX)
		return ramon__fail(st, RAMON_EL_INVAL, 0, len, RAMON_SPW_CONFIG_MAX,
				   "configuration of %u bytes (1..%u)", len, RAMON_SPW_CONFIG_MAX);
	return spw_nn_ack(c, nn, RAMON_SPW_CONFIG_APP, RAMON_SPW_CONFIG_ATTR, data, len, timeout_ms,
			  "configuration", st);
}

struct swup_start {
	uint8_t magic;
	uint8_t sw_type;
	uint16_t n_packets;
} __attribute__((packed));

struct swup_data {
	uint8_t magic;
	uint8_t reserve;
	uint16_t index;
	uint8_t data[RAMON_SPW_SWUP_CHUNK];
} __attribute__((packed));

struct swup_end {
	uint8_t magic;
	uint8_t reserve;
	uint16_t index;
	uint16_t data_length;
	uint16_t reserve2;
	uint8_t data[RAMON_SPW_SWUP_CHUNK];
} __attribute__((packed));

const char *ramon_spw_swup_status_str(int phase, uint32_t status)
{
	static const char *const start[] = {
		"ok", "update not allowed: the application is running", "illegal SW type",
	};
	static const char *const end[] = {
		"ok", "illegal package structure", "bad package CRC", "bad 4KBL CRC",
		"bad RSBL CRC", "bad image CRC", "programming 4KBL failed",
		"programming RSBL failed", "programming image failed", "general boot flash error",
	};

	if (phase == RAMON_SWUP_START && status < ARRAY_SIZE(start))
		return start[status];
	if (phase == RAMON_SWUP_END && status < ARRAY_SIZE(end))
		return end[status];
	return status ? "rejected" : "ok";
}

/* one update packet; the answer's status is the second 16-bit word of its payload */
static int swup_step(struct ramon_ctx *c, uint32_t nn, uint32_t attr, const void *pkt, uint32_t len,
		     int phase, uint32_t index, struct ramon_status *st)
{
	struct ramon_spw_msg m;
	struct ramon_spw_rx rx;
	uint8_t *reply;
	uint16_t status;
	int ret;

	spw_nn_msg(c, &m, RAMON_SPW_SWUP_APP, attr);
	ret = spw_nn_request(c, nn, &m, pkt, len, RAMON_SPW_REPLY_MS, &reply, &rx, st);
	if (ret) {
		if (st && st->code == RAMON_EL_SPW_REPLY_TIMEOUT)
			st->arg[1] = (uint64_t)phase << 16 | index;
		return ret;
	}
	if (rx.payload_len < 4) {
		free(reply);
		return ramon__fail(st, RAMON_EL_SPW_BAD_PACKET, 0, rx.payload_len,
				   (uint64_t)phase << 16 | index,
				   "SW update answer with %u payload bytes", rx.payload_len);
	}
	memcpy(&status, rx.payload + 2, sizeof(status));
	free(reply);
	if (status)
		return ramon__fail(st, RAMON_EL_SPW_NACK, 0, status, (uint64_t)phase << 16 | index,
				   "SW update packet %u: status %u (%s)", index, status,
				   ramon_spw_swup_status_str(phase, status));
	return 0;
}

int ramon_spw_nn_sw_update(ramon_ctx *c, uint32_t nn, uint8_t sw_type, const void *image, size_t len,
			   ramon_progress_fn progress, void *user, struct ramon_status *st)
{
	const uint8_t *img = image;
	struct swup_start s0;
	struct swup_data *d;
	struct swup_end *e;
	uint32_t n, i;
	size_t last;
	int ret;

	if (!len || (len + RAMON_SPW_SWUP_CHUNK - 1) / RAMON_SPW_SWUP_CHUNK > 0xFFFF)
		return ramon__fail(st, RAMON_EL_INVAL, 0, len, 0, "image of %zu bytes", len);
	n = (uint32_t)((len + RAMON_SPW_SWUP_CHUNK - 1) / RAMON_SPW_SWUP_CHUNK);
	d = calloc(1, sizeof(*d));
	e = calloc(1, sizeof(*e));
	if (!d || !e) {
		ret = ramon__fail(st, RAMON_EL_SYS, ENOMEM, 0, 0, "out of memory");
		goto out;
	}
	s0.magic = RAMON_SPW_SWUP_MAGIC;
	s0.sw_type = sw_type;
	s0.n_packets = (uint16_t)n;
	ret = swup_step(c, nn, RAMON_SPW_SWUP_START_ATTR, &s0, sizeof(s0), RAMON_SWUP_START, 0, st);
	if (progress && !ret)
		progress(user, 0, n);
	/* data packets 1..n-1 carry the first n-1 chunks; the end packet carries the last */
	for (i = 1; !ret && i < n; i++) {
		d->magic = RAMON_SPW_SWUP_MAGIC;
		d->index = (uint16_t)i;
		memcpy(d->data, img + (size_t)(i - 1) * RAMON_SPW_SWUP_CHUNK, RAMON_SPW_SWUP_CHUNK);
		ret = swup_step(c, nn, RAMON_SPW_SWUP_DATA_ATTR, d, sizeof(*d), RAMON_SWUP_DATA, i, st);
		if (progress && !ret)
			progress(user, i, n);
	}
	if (ret)
		goto out;
	last = len - (size_t)(n - 1) * RAMON_SPW_SWUP_CHUNK;
	e->magic = RAMON_SPW_SWUP_MAGIC;
	e->index = (uint16_t)n;
	e->data_length = (uint16_t)last;
	memcpy(e->data, img + (size_t)(n - 1) * RAMON_SPW_SWUP_CHUNK, last);
	ret = swup_step(c, nn, RAMON_SPW_SWUP_END_ATTR, e, sizeof(*e), RAMON_SWUP_END, n, st);
	if (progress && !ret)
		progress(user, n, n);
out:
	free(d);
	free(e);
	return ret;
}

int ramon_spw_get_stats(ramon_ctx *c, uint32_t nn, struct ramon_spw_stats *out, int reset)
{
	uint64_t *src, *dst = (uint64_t *)out;
	size_t i;

	if (nn >= RAMON_NN_COUNT)
		return -EINVAL;
	src = (uint64_t *)&c->spw.nn[nn].stats;
	for (i = 0; i < sizeof(*out) / sizeof(uint64_t); i++)
		dst[i] = reset ? __atomic_exchange_n(&src[i], 0, __ATOMIC_RELAXED) :
				 __atomic_load_n(&src[i], __ATOMIC_RELAXED);
	return 0;
}

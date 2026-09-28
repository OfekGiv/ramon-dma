// SPDX-License-Identifier: MIT
/* libramon AXI DMA and ZDMA transfers */
#include "ramon_priv.h"

#define AXI_RETRY_DELAY_MS	2

int ramon_axi_xfer(ramon_ctx *c, uint32_t chan, const struct ramon_sg_item *items,
		   uint32_t n_items, uint32_t timeout_ms, uint64_t *bytes, struct ramon_status *st)
{
	struct ramon_axi_xfer x;
	int ret, retried = 0;

	for (;;) {
		memset(&x, 0, sizeof(x));
		x.chan = chan;
		x.timeout_ms = timeout_ms;
		x.n_items = n_items;
		x.items = (uintptr_t)items;
		ret = ramon__ioctl(c, RAMON_IOC_AXI_XFER, &x, &x.st, st);
		/* the descriptor pool can still be busy right after a long list (BUILD_NOTES 0.8.8) */
		if (ret != -ENOSPC || x.st.code != RAMON_E_AXI_PREP_FAILED || retried)
			break;
		retried = 1;
		__atomic_add_fetch(&c->axi_retries, 1, __ATOMIC_RELAXED);
		ramon__log(c, RAMON_LOG_INFO, "AXI chan %u: descriptor prep failed, retrying", chan);
		ramon__sleep_ms(AXI_RETRY_DELAY_MS);
	}
	if (bytes)
		*bytes = ret ? 0 : x.bytes;
	return ret;
}

int ramon_axi_xfer1(ramon_ctx *c, uint32_t chan, const ramon_buf *b, uint64_t off, uint64_t len,
		    uint32_t timeout_ms, struct ramon_status *st)
{
	struct ramon_sg_item it;
	uint64_t bytes;
	int ret;

	memset(&it, 0, sizeof(it));
	it.handle = b->handle;
	it.offset = off;
	it.len = len;
	ret = ramon_axi_xfer(c, chan, &it, 1, timeout_ms, &bytes, st);
	if (!ret && bytes != len)
		return ramon__fail(st, RAMON_EL_INVAL, EIO, bytes, len,
				   "AXI chan %u moved %llu of %llu bytes", chan,
				   (unsigned long long)bytes, (unsigned long long)len);
	return ret;
}

uint64_t ramon_axi_retries(const ramon_ctx *c)
{
	return __atomic_load_n(&c->axi_retries, __ATOMIC_RELAXED);
}

int ramon_axi_find_pair(ramon_ctx *c, uint32_t nn, uint32_t *rx_chan, uint32_t *tx_chan,
			struct ramon_status *st)
{
	struct ramon_chan_info a, b;
	int ret;

	if (nn >= RAMON_NN_COUNT)
		return ramon__fail(st, RAMON_EL_INVAL, 0, nn, 0, "no NN %u", nn);
	ret = ramon_chan_info(c, 2 * nn, &a, st);
	if (!ret)
		ret = ramon_chan_info(c, 2 * nn + 1, &b, st);
	if (ret)
		return ret;
	if (a.type != RAMON_CHAN_AXI || b.type != RAMON_CHAN_AXI || a.dir == b.dir ||
	    a.dir == RAMON_DIR_MEMCPY || b.dir == RAMON_DIR_MEMCPY)
		return ramon__fail(st, RAMON_EL_INVAL, ENODEV, 2 * nn, 2 * nn + 1,
				   "AXI channels %u/%u are not an RX/TX pair", 2 * nn, 2 * nn + 1);
	*rx_chan = a.dir == RAMON_DIR_DEV_TO_MEM ? a.index : b.index;
	*tx_chan = a.dir == RAMON_DIR_DEV_TO_MEM ? b.index : a.index;
	return 0;
}

int ramon_zdma_copy(ramon_ctx *c, const struct ramon_copy *e, uint32_t n, uint32_t timeout_ms,
		    uint32_t *done, struct ramon_status *st)
{
	struct ramon_zdma_copy z;
	int ret;

	memset(&z, 0, sizeof(z));
	z.timeout_ms = timeout_ms;
	z.n = n;
	z.entries = (uintptr_t)e;
	ret = ramon__ioctl(c, RAMON_IOC_ZDMA_COPY, &z, &z.st, st);
	if (done)
		*done = z.done;
	return ret;
}

int ramon_zdma_copy1(ramon_ctx *c, const ramon_buf *src, uint64_t src_off, const ramon_buf *dst,
		     uint64_t dst_off, uint64_t len, uint32_t timeout_ms, struct ramon_status *st)
{
	struct ramon_copy e;

	memset(&e, 0, sizeof(e));
	e.src_handle = src->handle;
	e.dst_handle = dst->handle;
	e.src_off = src_off;
	e.dst_off = dst_off;
	e.len = len;
	return ramon_zdma_copy(c, &e, 1, timeout_ms, NULL, st);
}

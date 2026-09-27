// SPDX-License-Identifier: GPL-2.0
/*
 * ZDMA memcpy: a pool of DMA_MEMCPY channels taken by capability mask (no DT).
 * xilinx_dma never advertises DMA_MEMCPY, so AXI channels cannot be taken.
 * A copy takes a free channel (sleeping killably if none), runs its list in
 * chunks that fit the channel's descriptor pool, and returns the channel.
 * zdma.lock guards only the free mask and is never held across a copy.
 */
#define pr_fmt(fmt) "ramon_dma: " fmt

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/nospec.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "ramon_dma.h"

/* one validated copy: buffer references and resolved addresses */
struct ramon_zdma_op {
	struct ramon_buf *src;
	struct ramon_buf *dst;
	dma_addr_t src_dma;
	dma_addr_t dst_dma;
	size_t len;
};

static void ramon_zdma_done(void *arg)
{
	struct ramon_zdma_chan *zc = arg;

	complete(&zc->done);
}

/* ---- channel pool ---- */

static bool ramon_zdma_try_get(struct ramon_zdma *z, u32 *idx)
{
	bool got = false;

	spin_lock(&z->lock);
	if (z->free_mask) {
		*idx = __ffs(z->free_mask);
		__clear_bit(*idx, &z->free_mask);
		got = true;
	}
	spin_unlock(&z->lock);
	return got;
}

static void ramon_zdma_put_chan(struct ramon_zdma *z, u32 idx)
{
	spin_lock(&z->lock);
	__set_bit(idx, &z->free_mask);
	spin_unlock(&z->lock);
	wake_up(&z->wq);
}

/* wait condition: a channel was taken, or the device is going away */
static bool ramon_zdma_ready(struct ramon_dev *rd, u32 *idx, bool *got)
{
	*got = ramon_zdma_try_get(&rd->zdma, idx);
	return *got || READ_ONCE(rd->dead);
}

static int ramon_zdma_get_chan(struct ramon_dev *rd, u32 *idx, struct ramon_status *st)
{
	struct ramon_zdma *z = &rd->zdma;
	bool got = false;

	if (wait_event_killable(z->wq, ramon_zdma_ready(rd, idx, &got)))
		return ramon_fail(st, RAMON_E_INTERRUPTED, z->n, 0,
				  "zdma: killed while waiting for one of %u channels", z->n);
	if (READ_ONCE(rd->dead)) {
		if (got)
			ramon_zdma_put_chan(z, *idx);
		return ramon_fail(st, RAMON_E_REMOVED, 0, 0, "zdma: device removed");
	}
	return 0;
}

/* ---- validation ---- */

static int ramon_zdma_resolve(struct ramon_file *rf, const struct ramon_copy *c, u32 i,
			      struct ramon_zdma_op *op, struct ramon_status *st)
{
	struct dma_device *dev = rf->rd->zdma.ch[0].chan->device;
	int ret;

	ret = ramon_buf_ref(rf, c->src_handle, c->src_off, c->len, "zdma src", i,
			    RAMON_E_BUF_RANGE, &op->src, st);
	if (ret)
		return ret;
	ret = ramon_buf_ref(rf, c->dst_handle, c->dst_off, c->len, "zdma dst", i,
			    RAMON_E_BUF_RANGE, &op->dst, st);
	if (ret)
		return ret;
	op->src_dma = op->src->dma + c->src_off;
	op->dst_dma = op->dst->dma + c->dst_off;
	op->len = c->len;
	if (!is_dma_copy_aligned(dev, op->src_dma, op->dst_dma, op->len))
		return ramon_fail(st, RAMON_E_NOT_ALIGNED, i, c->len,
				  "zdma entry %u: src off 0x%llx, dst off 0x%llx, len 0x%llx not all %u-byte aligned",
				  i, c->src_off, c->dst_off, c->len, 1U << dev->copy_align);
	return 0;
}

static void ramon_zdma_release_ops(struct ramon_zdma_op *ops, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++) {
		if (ops[i].src)
			ramon_buf_put(ops[i].src);
		if (ops[i].dst)
			ramon_buf_put(ops[i].dst);
	}
	kvfree(ops);
}

/* copies and validates the whole list up front: an invalid list copies nothing */
static int ramon_zdma_prepare(struct ramon_file *rf, const struct ramon_zdma_copy *p,
			      struct ramon_zdma_op **opsp, struct ramon_status *st)
{
	struct ramon_zdma_op *ops;
	struct ramon_copy *c;
	int ret = 0;
	u32 i;

	c = vmemdup_user(u64_to_user_ptr(p->entries), array_size(p->n, sizeof(*c)));
	if (IS_ERR(c))
		return ramon_fail(st, PTR_ERR(c) == -EFAULT ? RAMON_E_COPY_FAULT :
				  RAMON_E_NO_MEMORY, p->entries, p->n,
				  "zdma: cannot copy %u entries from 0x%llx (%ld)",
				  p->n, p->entries, PTR_ERR(c));
	ops = kvcalloc(p->n, sizeof(*ops), GFP_KERNEL);
	if (!ops) {
		kvfree(c);
		return ramon_fail(st, RAMON_E_NO_MEMORY, p->n, 0,
				  "zdma: no memory for %u entries", p->n);
	}
	for (i = 0; i < p->n && !ret; i++)
		ret = ramon_zdma_resolve(rf, &c[i], i, &ops[i], st);
	kvfree(c);
	if (ret) {
		ramon_zdma_release_ops(ops, p->n);
		return ret;
	}
	*opsp = ops;
	return 0;
}

/* ---- running ---- */

static int ramon_zdma_wait(struct ramon_dev *rd, struct ramon_zdma_chan *zc, dma_cookie_t last,
			   u32 first, u32 cnt, u32 timeout_ms, struct ramon_status *st)
{
	enum dma_status status;
	long left;

	dma_async_issue_pending(zc->chan);
	left = wait_for_completion_killable_timeout(&zc->done, msecs_to_jiffies(timeout_ms));
	status = dmaengine_tx_status(zc->chan, last, NULL);
	if (left > 0 && status == DMA_COMPLETE)
		return 0;

	dmaengine_terminate_sync(zc->chan);
	if (left == 0) {
		atomic64_inc(&rd->stats.zdma_timeouts);
		dev_warn_ratelimited(&rd->pdev->dev,
				     "zdma %s: entries %u..%u not done after %u ms (%s); terminated\n",
				     dma_chan_name(zc->chan), first, first + cnt - 1, timeout_ms,
				     ramon_dma_status_name(status));
		return ramon_fail(st, RAMON_E_ZDMA_TIMEOUT, first, timeout_ms,
				  "zdma %s: entries %u..%u not done after %u ms, dmaengine status %s, channel terminated",
				  dma_chan_name(zc->chan), first, first + cnt - 1, timeout_ms,
				  ramon_dma_status_name(status));
	}
	if (left < 0)
		return ramon_fail(st, RAMON_E_INTERRUPTED, first, 0,
				  "zdma %s: killed during entries %u..%u, channel terminated",
				  dma_chan_name(zc->chan), first, first + cnt - 1);
	if (READ_ONCE(rd->dead))
		return ramon_fail(st, RAMON_E_REMOVED, first, 0,
				  "zdma: device removed during entries %u..%u",
				  first, first + cnt - 1);
	dev_warn_ratelimited(&rd->pdev->dev, "zdma %s: entries %u..%u ended %s; terminated\n",
			     dma_chan_name(zc->chan), first, first + cnt - 1,
			     ramon_dma_status_name(status));
	return ramon_fail(st, RAMON_E_ZDMA_DMA_ERROR, first, status,
			  "zdma %s: entries %u..%u ended with dmaengine status %s",
			  dma_chan_name(zc->chan), first, first + cnt - 1,
			  ramon_dma_status_name(status));
}

/* one chunk: prep + submit each entry, interrupt and callback on the last only */
static int ramon_zdma_chunk(struct ramon_dev *rd, struct ramon_zdma_chan *zc,
			    const struct ramon_zdma_op *ops, u32 first, u32 cnt, u32 timeout_ms,
			    struct ramon_status *st)
{
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie = 0;
	u32 i;

	reinit_completion(&zc->done);
	/* pairs with smp_store_mb(->dead) in remove(), which then complete_all()s */
	smp_mb();
	if (READ_ONCE(rd->dead))
		return ramon_fail(st, RAMON_E_REMOVED, first, 0, "zdma: device removed");

	for (i = 0; i < cnt; i++) {
		const struct ramon_zdma_op *op = &ops[first + i];
		bool last = i == cnt - 1;

		tx = dmaengine_prep_dma_memcpy(zc->chan, op->dst_dma, op->src_dma, op->len,
					       DMA_CTRL_ACK | (last ? DMA_PREP_INTERRUPT : 0));
		if (!tx) {
			dmaengine_terminate_sync(zc->chan);
			return ramon_fail(st, RAMON_E_ZDMA_PREP_FAILED, first + i, op->len,
					  "zdma %s: prep of entry %u (0x%zx bytes) failed",
					  dma_chan_name(zc->chan), first + i, op->len);
		}
		if (last) {
			tx->callback = ramon_zdma_done;
			tx->callback_param = zc;
		}
		cookie = dmaengine_submit(tx);
		if (dma_submit_error(cookie)) {
			dmaengine_terminate_sync(zc->chan);
			return ramon_fail(st, RAMON_E_ZDMA_SUBMIT_FAILED, first + i, cookie,
					  "zdma %s: submit of entry %u returned %d",
					  dma_chan_name(zc->chan), first + i, cookie);
		}
	}
	return ramon_zdma_wait(rd, zc, cookie, first, cnt, timeout_ms, st);
}

int ramon_ioc_zdma_copy(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_zdma_copy *p = arg;
	struct ramon_dev *rd = rf->rd;
	struct ramon_zdma_op *ops;
	u32 idx, first, cnt, timeout_ms;
	int ret;

	p->done = 0;
	if (!rd->zdma.n)
		return ramon_fail(st, RAMON_E_ZDMA_NO_CHANNELS, 0, ramon_param_zdma_channels(),
				  "zdma: no DMA_MEMCPY channel was acquired at probe (zdma_channels=%u)",
				  ramon_param_zdma_channels());
	if (!p->n || p->n > RAMON_ZDMA_MAX_COPIES)
		return ramon_fail(st, RAMON_E_BAD_COUNT, p->n, RAMON_ZDMA_MAX_COPIES,
				  "zdma: n %u not in 1..%u", p->n, RAMON_ZDMA_MAX_COPIES);
	ret = ramon_zdma_prepare(rf, p, &ops, st);
	if (ret)
		return ret;
	ret = ramon_zdma_get_chan(rd, &idx, st);
	if (ret)
		goto out;

	timeout_ms = ramon_timeout_ms(p->timeout_ms, RAMON_TMO_ZDMA_MS);
	for (first = 0; first < p->n && !ret; first += cnt) {
		cnt = min_t(u32, p->n - first, RAMON_ZDMA_CHUNK);
		ret = ramon_zdma_chunk(rd, &rd->zdma.ch[idx], ops, first, cnt, timeout_ms, st);
		if (!ret) {
			p->done += cnt;
			atomic64_add(cnt, &rd->stats.zdma_ops);
		}
	}
	ramon_zdma_put_chan(&rd->zdma, idx);
out:
	ramon_zdma_release_ops(ops, p->n);
	return ret;
}

/* CHAN_INFO for indices past the AXI channels */
int ramon_zdma_chan_info(struct ramon_dev *rd, struct ramon_chan_info *p,
			 struct ramon_status *st)
{
	u32 i = p->index - rd->n_axi;
	struct ramon_zdma_chan *zc;

	if (i >= rd->zdma.n)
		return ramon_fail(st, RAMON_E_AXI_BAD_CHAN, p->index, rd->n_axi + rd->zdma.n,
				  "chan index %u: only %u AXI + %u ZDMA channels",
				  p->index, rd->n_axi, rd->zdma.n);
	zc = &rd->zdma.ch[array_index_nospec(i, rd->zdma.n)];
	p->type = RAMON_CHAN_ZDMA;
	p->dir = RAMON_DIR_MEMCPY;
	p->device_id = 0;
	p->phys = zc->phys;
	strscpy(p->name, dma_chan_name(zc->chan), sizeof(p->name));
	return 0;
}

/* ---- probe / remove ---- */

static struct dma_chan *ramon_zdma_request(struct device *dev, u32 i)
{
	struct dma_chan *chan;
	dma_cap_mask_t mask;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(chan)) {
		dev_info(dev, "zdma: no more DMA_MEMCPY channels after %u (%ld)\n",
			 i, PTR_ERR(chan));
		return NULL;
	}
	if (!chan->device->device_prep_dma_memcpy) {
		dev_warn(dev, "zdma: %s advertises DMA_MEMCPY without a memcpy prep; not used\n",
			 dma_chan_name(chan));
		dma_release_channel(chan);
		return NULL;
	}
	return chan;
}

int ramon_zdma_probe(struct ramon_dev *rd)
{
	struct device *dev = &rd->pdev->dev;
	struct ramon_zdma *z = &rd->zdma;
	u32 want = ramon_param_zdma_channels(), i;
	struct resource res;

	spin_lock_init(&z->lock);
	init_waitqueue_head(&z->wq);
	if (!want) {
		dev_info(dev, "zdma: zdma_channels=0; ZDMA_COPY disabled\n");
		return 0;
	}
	z->ch = devm_kcalloc(dev, want, sizeof(*z->ch), GFP_KERNEL);
	if (!z->ch)
		return -ENOMEM;
	for (i = 0; i < want; i++) {
		struct ramon_zdma_chan *zc = &z->ch[i];

		zc->chan = ramon_zdma_request(dev, i);
		if (!zc->chan)
			break;
		init_completion(&zc->done);
		if (!of_address_to_resource(zc->chan->device->dev->of_node, 0, &res))
			zc->phys = res.start;
		dev_info(dev, "zdma ch%u: %s (%pOF), copy_align %u\n", i, dma_chan_name(zc->chan),
			 zc->chan->device->dev->of_node, 1U << zc->chan->device->copy_align);
	}
	z->n = i;
	z->free_mask = z->n ? GENMASK(z->n - 1, 0) : 0;
	dev_info(dev, "zdma: %u of %u memcpy channels\n", z->n, want);
	return 0;
}

void ramon_zdma_wake(struct ramon_dev *rd)
{
	u32 i;

	wake_up_all(&rd->zdma.wq);
	for (i = 0; i < rd->zdma.n; i++)
		complete_all(&rd->zdma.ch[i].done);
}

void ramon_zdma_remove(struct ramon_dev *rd)
{
	u32 i;

	for (i = 0; i < rd->zdma.n; i++)
		dma_release_channel(rd->zdma.ch[i].chan);
}

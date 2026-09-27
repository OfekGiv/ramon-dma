// SPDX-License-Identifier: GPL-2.0
/*
 * AXI DMA scatter-gather channels (xilinx_dma), one per "dmas" entry of our
 * DT node; index == DT position. Transfers are synchronous: prepare (copy and
 * resolve the items, holding a reference on every buffer), run (channel mutex,
 * prep, submit, wait) and release. SPFI_WRITE uses the same three steps.
 *
 * On xilinx_dma a terminate soft-resets the whole axi_dma IP, i.e. both
 * directions, so every terminate is logged with the IP node.
 */
#define pr_fmt(fmt) "ramon_dma: " fmt

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/jiffies.h>
#include <linux/nospec.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "ramon_dma.h"

const char *ramon_dma_status_name(enum dma_status status)
{
	switch (status) {
	case DMA_COMPLETE:
		return "COMPLETE";
	case DMA_IN_PROGRESS:
		return "IN_PROGRESS";
	case DMA_PAUSED:
		return "PAUSED";
	case DMA_ERROR:
		return "ERROR";
	default:
		return "UNKNOWN";
	}
}

static void ramon_axi_done(void *arg)
{
	struct ramon_axichan *ch = arg;

	complete(&ch->done);
}

/* ---- prepare / run / release ---- */

/* resolve one item into sg entry @i; takes a buffer reference into job->bufs[i] */
static int ramon_axi_item(struct ramon_file *rf, struct ramon_axi_job *job, u32 i,
			  const struct ramon_sg_item *it, struct ramon_status *st)
{
	struct dma_chan *chan = job->ch->chan;
	u32 align = BIT(chan->device->copy_align) - 1;
	dma_addr_t dma;
	int ret;

	if (it->len > U32_MAX)
		return ramon_fail(st, RAMON_E_INVAL_ARG, i, it->len,
				  "axi ch%u: item %u: len 0x%llx > 4 GiB - 1",
				  job->chan, i, it->len);
	ret = ramon_buf_ref(rf, it->handle, it->offset, it->len, "axi item", i,
			    RAMON_E_BUF_RANGE, &job->bufs[i], st);
	if (ret)
		return ret;
	dma = job->bufs[i]->dma + it->offset;
	if (dma & align)
		return ramon_fail(st, RAMON_E_NOT_ALIGNED, i, it->offset,
				  "axi ch%u: item %u: buf %u offset 0x%llx not %u-byte aligned (no DRE)",
				  job->chan, i, it->handle, it->offset, align + 1);
	sg_dma_address(&job->sg[i]) = dma;
	sg_dma_len(&job->sg[i]) = it->len;
	job->bytes += it->len;
	return 0;
}

/**
 * ramon_axi_job_prepare() - validate a transfer and pin its buffers
 * @rf: caller's file; handles are resolved against its buffers only
 * @chan: AXI channel index
 * @uitems: user pointer to struct ramon_sg_item[@n]
 * @n: item count, 1..RAMON_AXI_MAX_ITEMS
 * @job: filled on success; release with ramon_axi_job_release() in all cases
 * @st: status trailer
 *
 * Return: 0 or the negative errno from ramon_fail().
 */
int ramon_axi_job_prepare(struct ramon_file *rf, u32 chan, u64 uitems, u32 n,
			  struct ramon_axi_job *job, struct ramon_status *st)
{
	struct ramon_dev *rd = rf->rd;
	struct ramon_sg_item *items;
	int ret = 0;
	u32 i;

	memset(job, 0, sizeof(*job));
	job->chan = chan;
	if (chan >= rd->n_axi)
		return ramon_fail(st, RAMON_E_AXI_BAD_CHAN, chan, rd->n_axi,
				  "chan %u: only %u AXI channels", chan, rd->n_axi);
	job->ch = &rd->axi[array_index_nospec(chan, rd->n_axi)];
	if (!job->ch->chan)
		return ramon_fail(st, RAMON_E_NOT_PRESENT, chan, 0,
				  "axi ch%u (%s): channel unavailable (see probe log)",
				  chan, job->ch->name);
	if (!n || n > RAMON_AXI_MAX_ITEMS)
		return ramon_fail(st, RAMON_E_BAD_COUNT, n, RAMON_AXI_MAX_ITEMS,
				  "axi ch%u: n_items %u not in 1..%u",
				  chan, n, RAMON_AXI_MAX_ITEMS);

	items = memdup_user(u64_to_user_ptr(uitems), array_size(n, sizeof(*items)));
	if (IS_ERR(items))
		return ramon_fail(st, PTR_ERR(items) == -EFAULT ? RAMON_E_COPY_FAULT :
				  RAMON_E_NO_MEMORY, uitems, n,
				  "axi ch%u: cannot copy %u items from 0x%llx (%ld)",
				  chan, n, uitems, PTR_ERR(items));
	job->bufs = kcalloc(n, sizeof(*job->bufs), GFP_KERNEL);
	job->sg = kcalloc(n, sizeof(*job->sg), GFP_KERNEL);
	if (!job->bufs || !job->sg) {
		ret = ramon_fail(st, RAMON_E_NO_MEMORY, n, 0,
				 "axi ch%u: no memory for %u sg entries", chan, n);
		goto out;
	}
	job->n_bufs = n;
	sg_init_table(job->sg, n);
	for (i = 0; i < n && !ret; i++)
		ret = ramon_axi_item(rf, job, i, &items[i], st);
out:
	kfree(items);
	return ret;
}

void ramon_axi_job_release(struct ramon_axi_job *job)
{
	u32 i;

	for (i = 0; i < job->n_bufs; i++)
		if (job->bufs[i])
			ramon_buf_put(job->bufs[i]);
	kfree(job->bufs);
	kfree(job->sg);
	job->bufs = NULL;
	job->sg = NULL;
	job->n_bufs = 0;
}

/* after a failed or abandoned transfer: stop the channel, which resets the IP */
static void ramon_axi_terminate(struct ramon_dev *rd, struct ramon_axichan *ch, u32 chan,
				const char *why)
{
	dmaengine_terminate_sync(ch->chan);
	dev_warn_ratelimited(&rd->pdev->dev,
			     "axi ch%u (%s): %s; terminated, which resets axi_dma %pOF (both directions)\n",
			     chan, ch->name, why, ch->ip_node);
}

/* waits for the job's completion; the channel mutex is held by the caller */
static int ramon_axi_wait(struct ramon_dev *rd, struct ramon_axi_job *job, dma_cookie_t cookie,
			  u32 timeout_ms, struct ramon_status *st)
{
	struct ramon_axichan *ch = job->ch;
	enum dma_status status;
	long left;

	left = wait_for_completion_killable_timeout(&ch->done, msecs_to_jiffies(timeout_ms));
	status = dmaengine_tx_status(ch->chan, cookie, NULL);
	if (left > 0 && status == DMA_COMPLETE) {
		atomic64_inc(ch->dir == DMA_MEM_TO_DEV ? &rd->stats.axi_tx : &rd->stats.axi_rx);
		return 0;
	}

	if (left == 0) {
		atomic64_inc(&rd->stats.axi_timeouts);
		ramon_axi_terminate(rd, ch, job->chan, "timeout");
		return ramon_fail(st, RAMON_E_AXI_TIMEOUT, job->chan, timeout_ms,
				  "axi ch%u (%s): no completion after %u ms, dmaengine status %s, channel terminated",
				  job->chan, ch->name, timeout_ms, ramon_dma_status_name(status));
	}
	if (left < 0) {
		ramon_axi_terminate(rd, ch, job->chan, "caller killed");
		return ramon_fail(st, RAMON_E_INTERRUPTED, job->chan, 0,
				  "axi ch%u (%s): killed while waiting, channel terminated",
				  job->chan, ch->name);
	}
	ramon_axi_terminate(rd, ch, job->chan, READ_ONCE(rd->dead) ? "device removed" :
			    "completed in error");
	if (READ_ONCE(rd->dead))
		return ramon_fail(st, RAMON_E_REMOVED, job->chan, 0,
				  "axi ch%u (%s): device removed during the transfer",
				  job->chan, ch->name);
	return ramon_fail(st, RAMON_E_AXI_DMA_ERROR, job->chan, status,
			  "axi ch%u (%s): transfer ended with dmaengine status %s",
			  job->chan, ch->name, ramon_dma_status_name(status));
}

static int ramon_axi_start(struct ramon_dev *rd, struct ramon_axi_job *job, u32 timeout_ms,
			   struct ramon_status *st)
{
	struct ramon_axichan *ch = job->ch;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;

	tx = dmaengine_prep_slave_sg(ch->chan, job->sg, job->n_bufs, ch->dir,
				     DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!tx)
		return ramon_fail(st, RAMON_E_AXI_PREP_FAILED, job->chan, job->n_bufs,
				  "axi ch%u (%s): prep of %u items (0x%llx bytes) failed, descriptor pool exhausted?",
				  job->chan, ch->name, job->n_bufs, job->bytes);
	reinit_completion(&ch->done);
	/* pairs with smp_store_mb(->dead) in remove(), which then complete_all()s */
	smp_mb();
	if (READ_ONCE(rd->dead)) {
		dmaengine_terminate_sync(ch->chan);
		return ramon_fail(st, RAMON_E_REMOVED, job->chan, 0,
				  "axi ch%u (%s): device removed", job->chan, ch->name);
	}
	tx->callback = ramon_axi_done;
	tx->callback_param = ch;
	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie)) {
		ramon_axi_terminate(rd, ch, job->chan, "submit failed");
		return ramon_fail(st, RAMON_E_AXI_SUBMIT_FAILED, job->chan, cookie,
				  "axi ch%u (%s): dmaengine_submit returned %d",
				  job->chan, ch->name, cookie);
	}
	dma_async_issue_pending(ch->chan);
	return ramon_axi_wait(rd, job, cookie, timeout_ms, st);
}

/**
 * ramon_axi_job_run() - run a prepared transfer to completion
 * @rd: device
 * @job: from ramon_axi_job_prepare()
 * @timeout_ms: completion timeout (already defaulted and clamped)
 * @st: status trailer
 *
 * Serializes on the channel. The caller must hold no SPFI mutex other than
 * its own direction lock.
 *
 * Return: 0 or the negative errno from ramon_fail().
 */
int ramon_axi_job_run(struct ramon_dev *rd, struct ramon_axi_job *job, u32 timeout_ms,
		      struct ramon_status *st)
{
	struct ramon_axichan *ch = job->ch;
	int ret;

	if (mutex_lock_interruptible(&ch->lock))
		return ramon_fail(st, RAMON_E_INTERRUPTED, job->chan, 0,
				  "axi ch%u (%s): interrupted waiting for the channel",
				  job->chan, ch->name);
	ret = ramon_axi_start(rd, job, timeout_ms, st);
	mutex_unlock(&ch->lock);
	return ret;
}

int ramon_ioc_axi_xfer(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_axi_xfer *p = arg;
	struct ramon_axi_job job;
	int ret;

	ret = ramon_axi_job_prepare(rf, p->chan, p->items, p->n_items, &job, st);
	if (!ret)
		ret = ramon_axi_job_run(rf->rd, &job,
					ramon_timeout_ms(p->timeout_ms, RAMON_TMO_AXI_MS), st);
	if (!ret)
		p->bytes = job.bytes;
	ramon_axi_job_release(&job);
	return ret;
}

/* ---- CHAN_INFO: AXI channels first, then ZDMA ---- */

int ramon_ioc_chan_info(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_chan_info *p = arg;
	struct ramon_dev *rd = rf->rd;
	struct ramon_axichan *ch;

	if (p->index >= rd->n_axi)
		return ramon_zdma_chan_info(rd, p, st);
	ch = &rd->axi[array_index_nospec(p->index, rd->n_axi)];
	p->type = RAMON_CHAN_AXI;
	p->dir = ch->dir == DMA_MEM_TO_DEV ? RAMON_DIR_MEM_TO_DEV : RAMON_DIR_DEV_TO_MEM;
	p->device_id = ch->device_id;
	p->phys = ch->phys;
	strscpy(p->name, ch->name ? ch->name : "", sizeof(p->name));
	return 0;
}

/* ---- probe / remove ---- */

static int ramon_axi_chan_probe(struct ramon_dev *rd, u32 i)
{
	struct device *dev = &rd->pdev->dev;
	struct ramon_axichan *ch = &rd->axi[i];
	struct ramon_of_axichan of = { };
	int ret;

	mutex_init(&ch->lock);
	init_completion(&ch->done);
	ret = ramon_of_axi(dev, i, &of);
	if (ret)
		return ret;
	ch->name = of.name;
	ch->dir = of.dir;
	ch->device_id = of.device_id;
	ch->phys = of.phys;
	ch->ip_node = of.ip_node;

	ch->chan = dma_request_chan(dev, ch->name);
	if (IS_ERR(ch->chan)) {
		ret = PTR_ERR(ch->chan);
		ch->chan = NULL;
		return dev_err_probe(dev, ret, "axi ch%u (%s): dma_request_chan failed\n",
				     i, ch->name);
	}
	dev_info(dev, "axi ch%u %s: %s, device-id %u, axi_dma %pa %pOF, copy_align %u\n",
		 i, ch->name, ch->dir == DMA_MEM_TO_DEV ? "MEM_TO_DEV (mm2s)" : "DEV_TO_MEM (s2mm)",
		 ch->device_id, &ch->phys, ch->ip_node, 1U << ch->chan->device->copy_align);
	return 0;
}

static void ramon_axi_release(struct ramon_dev *rd, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++) {
		if (rd->axi[i].chan)
			dma_release_channel(rd->axi[i].chan);
		rd->axi[i].chan = NULL;
		of_node_put(rd->axi[i].ip_node);
		rd->axi[i].ip_node = NULL;
	}
}

int ramon_axidma_probe(struct ramon_dev *rd)
{
	struct device *dev = &rd->pdev->dev;
	u32 i, usable = 0;
	int n, ret;

	n = ramon_of_axi_count(dev);
	if (n <= 0)
		return n ? n : -ENODEV;
	rd->axi = devm_kcalloc(dev, n, sizeof(*rd->axi), GFP_KERNEL);
	if (!rd->axi)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		ret = ramon_axi_chan_probe(rd, i);
		if (ret == -EPROBE_DEFER) {
			ramon_axi_release(rd, i + 1);
			return ret;
		}
		if (!ret)
			usable++;
	}
	if (!usable) {
		dev_err(dev, "%pOF: none of the %d AXI channels is usable\n", dev->of_node, n);
		ramon_axi_release(rd, n);
		return -ENODEV;
	}
	rd->n_axi = n;
	dev_info(dev, "%u AXI channels, %u usable\n", rd->n_axi, usable);
	return 0;
}

/* remove(): release waiters before the gate is drained */
void ramon_axidma_wake(struct ramon_dev *rd)
{
	u32 i;

	for (i = 0; i < rd->n_axi; i++)
		complete_all(&rd->axi[i].done);
}

/* remove(): after the gate is drained, nothing uses the channels any more */
void ramon_axidma_remove(struct ramon_dev *rd)
{
	ramon_axi_release(rd, rd->n_axi);
}

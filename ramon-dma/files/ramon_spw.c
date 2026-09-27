// SPDX-License-Identifier: GPL-2.0
/*
 * SpaceWire instances (NN A = 0, NN B = 1). The RX interrupt acks the IP,
 * latches RX_PKT_SIZE into a per-NN kfifo and wakes the waiters; SPW_WAIT_RX
 * pops one size per call. The packet itself is then moved by an AXI_XFER on
 * the NN's RX channel, chosen by userspace. Registers come from the regwin
 * table (spw<nn> windows).
 */
#define pr_fmt(fmt) "ramon_dma: " fmt

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/nospec.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "ramon_dma.h"

static const char * const ramon_spw_irq_names[RAMON_NN_COUNT] = { "ramon_spw0", "ramon_spw1" };

static irqreturn_t ramon_spw_irq(int irq, void *data)
{
	struct ramon_spw *spw = data;
	struct ramon_stats *stats = &spw->rd->stats;
	u32 size;

	spin_lock(&spw->lock);
	writel(RAMON_SPW_RX_IRQ_ACK_VAL, spw->regs + RAMON_SPW_RX_IRQ_ACK);
	size = readl(spw->regs + RAMON_SPW_RX_PKT_SIZE);
	if (!kfifo_put(&spw->rx_sizes, size))
		atomic64_inc(&stats->spw_overrun[spw->nn]);
	spin_unlock(&spw->lock);
	atomic64_inc(&stats->spw_rx[spw->nn]);
	wake_up(&spw->wq);
	return IRQ_HANDLED;
}

/* @nn validated, instance present: returns it, else fails the ioctl */
static int ramon_spw_get(struct ramon_dev *rd, u32 nn, bool need_irq, struct ramon_spw **spwp,
			 struct ramon_status *st)
{
	struct ramon_spw *spw;

	if (nn >= RAMON_NN_COUNT)
		return ramon_fail(st, RAMON_E_SPW_BAD_NN, nn, RAMON_NN_COUNT,
				  "spw nn %u: only %u NNs", nn, RAMON_NN_COUNT);
	spw = &rd->spw[array_index_nospec(nn, RAMON_NN_COUNT)];
	if (!spw->regs)
		return ramon_fail(st, RAMON_E_NOT_PRESENT, nn, 0,
				  "spw%u: not present on this board (see probe log)", nn);
	if (need_irq && !spw->irq)
		return ramon_fail(st, RAMON_E_NOT_PRESENT, nn, 0,
				  "spw%u (%s): no RX interrupt (see probe log)",
				  nn, spw->node_name);
	*spwp = spw;
	return 0;
}

/* wait condition: a size was popped, the waiter was cancelled or the device is going away */
static bool ramon_spw_ready(struct ramon_spw *spw, u32 gen, u32 *size, bool *got)
{
	bool done;

	spin_lock_irq(&spw->lock);
	*got = kfifo_get(&spw->rx_sizes, size);
	done = *got || spw->cancel_gen != gen;
	spin_unlock_irq(&spw->lock);
	return done || READ_ONCE(spw->rd->dead);
}

int ramon_ioc_spw_wait_rx(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spw_wait_rx *p = arg;
	struct ramon_spw *spw;
	u32 timeout_ms, gen, size = 0;
	bool got = false;
	long left;
	int ret;

	ret = ramon_spw_get(rf->rd, p->nn, true, &spw, st);
	if (ret)
		return ret;
	timeout_ms = ramon_timeout_ms(p->timeout_ms, RAMON_TMO_SPW_MS);

	spin_lock_irq(&spw->lock);
	gen = spw->cancel_gen;
	spin_unlock_irq(&spw->lock);

	left = wait_event_interruptible_timeout(spw->wq, ramon_spw_ready(spw, gen, &size, &got),
						msecs_to_jiffies(timeout_ms));
	if (got) {
		p->size = size;
		return 0;
	}
	if (READ_ONCE(rf->rd->dead))
		return ramon_fail(st, RAMON_E_REMOVED, p->nn, 0, "spw%u: device removed", p->nn);
	if (left > 0)
		return ramon_fail(st, RAMON_E_SPW_CANCELLED, p->nn, 0,
				  "spw%u: wait cancelled by SPW_CANCEL", p->nn);
	if (left < 0)
		return ramon_fail(st, RAMON_E_INTERRUPTED, p->nn, 0,
				  "spw%u: wait interrupted by a signal", p->nn);
	/* routine for a polling reader: no log */
	return ramon_fail(st, RAMON_E_SPW_TIMEOUT, p->nn, timeout_ms,
			  "spw%u: no packet within %u ms", p->nn, timeout_ms);
}

int ramon_ioc_spw_cancel(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spw_cancel *p = arg;
	struct ramon_spw *spw;
	int ret;

	ret = ramon_spw_get(rf->rd, p->nn, true, &spw, st);
	if (ret)
		return ret;
	spin_lock_irq(&spw->lock);
	spw->cancel_gen++;
	spin_unlock_irq(&spw->lock);
	wake_up_all(&spw->wq);
	return 0;
}

/* read LOOPBACK_ENABLE; if it differs, write it and reset the SPW link */
int ramon_ioc_spw_loopback(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spw_loopback *p = arg;
	struct ramon_spw *spw;
	int ret;

	if (p->enable > 1)
		return ramon_fail(st, RAMON_E_INVAL_ARG, p->nn, p->enable,
				  "spw%u: loopback enable %u is neither 0 nor 1", p->nn, p->enable);
	ret = ramon_spw_get(rf->rd, p->nn, false, &spw, st);
	if (ret)
		return ret;
	spin_lock_irq(&spw->lock);
	if (readl(spw->regs + RAMON_SPW_LOOPBACK_ENABLE) != p->enable) {
		writel(p->enable, spw->regs + RAMON_SPW_LOOPBACK_ENABLE);
		writel(RAMON_SPW_RESET_VAL, spw->regs + RAMON_SPW_RESET);
	}
	spin_unlock_irq(&spw->lock);
	return 0;
}

/* ---- probe / remove ---- */

static void ramon_spw_probe_one(struct ramon_dev *rd, u32 nn, int irq)
{
	struct device *dev = &rd->pdev->dev;
	struct ramon_regwin *win = &rd->win[RAMON_WIN_SPW0 + nn];
	struct ramon_spw *spw = &rd->spw[nn];
	int ret;

	spw->rd = rd;
	spw->nn = nn;
	spw->node_name = win->name;
	spin_lock_init(&spw->lock);
	INIT_KFIFO(spw->rx_sizes);
	init_waitqueue_head(&spw->wq);
	if (win->flags & RAMON_REGWIN_ABSENT)
		return;
	spw->regs = win->base;
	if (irq <= 0)
		return;
	ret = devm_request_irq(dev, irq, ramon_spw_irq, 0, ramon_spw_irq_names[nn], spw);
	if (ret) {
		dev_warn(dev, "spw%u (%s): request_irq(%d) failed: %d; SPW_WAIT_RX unavailable\n",
			 nn, spw->node_name, irq, ret);
		return;
	}
	spw->irq = irq;
	dev_info(dev, "spw%u %s: irq %d\n", nn, spw->node_name, irq);
}

/* after ramon_regwin_probe(): the SPW registers come from its spw<nn> windows */
void ramon_spw_probe(struct ramon_dev *rd, const struct ramon_of_hw *hw)
{
	u32 nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		ramon_spw_probe_one(rd, nn, hw->spw_irq[nn]);
}

/* remove(), first step: no more interrupts */
void ramon_spw_free_irqs(struct ramon_dev *rd)
{
	u32 nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		if (rd->spw[nn].irq)
			devm_free_irq(&rd->pdev->dev, rd->spw[nn].irq, &rd->spw[nn]);
}

void ramon_spw_wake(struct ramon_dev *rd)
{
	u32 nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		if (rd->spw[nn].regs)
			wake_up_all(&rd->spw[nn].wq);
}

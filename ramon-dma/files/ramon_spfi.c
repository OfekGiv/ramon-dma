// SPDX-License-Identifier: GPL-2.0
/*
 * SPFI instances (NN0, NN1): short commands, the composite DATA_WRITE and
 * READ_STREAM operations, alerts and the two DT-label memories.
 *
 * Locking (outer to inner): write_lock / read_lock / short_lock, then
 * cmd_lock (TX register programming only, never held across a wait), then
 * lock (spinlock, also taken by the IRQ). short_lock serializes short
 * commands across their wait, because there is one pending-command slot per
 * NN. CLOSE and FLUSH also take write_lock: their VC1TX ack is the same
 * interrupt that completes a DATA_WRITE.
 */
#define pr_fmt(fmt) "ramon_dma: " fmt

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/nospec.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "ramon_dma.h"

/* parameter words a short command writes, in this order */
#define SPFI_P_STREAM_ID	BIT(0)
#define SPFI_P_STREAM_TYPE	BIT(1)
#define SPFI_P_LAST_OFFSET	BIT(2)
#define SPFI_P_INIT_TYPE	BIT(3)
#define SPFI_P_TOD		BIT(4)

struct ramon_spfi_op {
	u8 tx;			/* RAMON_SPFI_OP_* */
	u8 rx;			/* expected RAMON_SPFI_RX_* */
	u8 params;		/* SPFI_P_* */
	bool long_timeout;	/* RAMON_TMO_SPFI_CMD_LONG_MS */
	bool vc1tx_ack;		/* acked on VC1TX too: exclusive with DATA_WRITE */
	const char *name;
};

/* Appendix B, every opcode SPFI_CMD accepts */
static const struct ramon_spfi_op ramon_spfi_ops[] = {
	{ RAMON_SPFI_OP_FLUSH_STREAM, RAMON_SPFI_RX_FLUSH_EXECUTED,
	  SPFI_P_STREAM_ID, false, true, "FLUSH" },
	{ RAMON_SPFI_OP_OPEN_STREAM_FOR_WRITE, RAMON_SPFI_RX_RCV_READY,
	  SPFI_P_STREAM_ID | SPFI_P_STREAM_TYPE | SPFI_P_LAST_OFFSET, false, false, "OPEN" },
	{ RAMON_SPFI_OP_CLOSE_STREAM_FOR_WRITE, RAMON_SPFI_RX_STREAM_WAS_CLOSED,
	  SPFI_P_STREAM_ID, false, true, "CLOSE" },
	{ RAMON_SPFI_OP_DELETE_STREAM, RAMON_SPFI_RX_STREAM_WAS_DELETED,
	  SPFI_P_STREAM_ID, false, false, "DELETE" },
	{ RAMON_SPFI_OP_GET_ALL_STREAM_STATUS, RAMON_SPFI_RX_ALL_STREAM_STATUS,
	  0, false, false, "GET_ALL_STREAM_STATUS" },
	{ RAMON_SPFI_OP_PLATFORM_RESET, RAMON_SPFI_RX_PLATFORM_RESET_ACK,
	  0, false, false, "PLATFORM_RESET" },
	{ RAMON_SPFI_OP_INIT, RAMON_SPFI_RX_INIT_COMPLETED,
	  SPFI_P_INIT_TYPE, true, false, "INIT" },
	{ RAMON_SPFI_OP_GRACEFUL_POWER_DOWN, RAMON_SPFI_RX_READY_TO_POWER_OFF,
	  0, true, false, "GRACEFUL_POWER_DOWN" },
	{ RAMON_SPFI_OP_SET_TOD, RAMON_SPFI_RX_TOD_WAS_SET,
	  SPFI_P_TOD, false, false, "SET_TOD" },
	{ RAMON_SPFI_OP_FORMAT, RAMON_SPFI_RX_FORMAT_COMPLETED,
	  0, false, false, "FORMAT" },
};

static const char * const ramon_spfi_irq_names[RAMON_NN_COUNT] = { "ramon_spfi0", "ramon_spfi1" };

static u32 ramon_spfi_rd(struct ramon_spfi *sp, u32 word)
{
	return readl(sp->regs + RAMON_SPFI_REG(word));
}

static void ramon_spfi_wr(struct ramon_spfi *sp, u32 word, u32 val)
{
	writel(val, sp->regs + RAMON_SPFI_REG(word));
}

/* ---- interrupt ---- */

/* one RX completion, decoded by the received opcode; sp->lock held */
static bool ramon_spfi_rx(struct ramon_spfi *sp)
{
	struct ramon_stats *stats = &sp->rd->stats;
	struct ramon_spfi_alert a;
	u32 op = ramon_spfi_rd(sp, RAMON_SPFI_RX_COMMAND);

	if (op == RAMON_SPFI_RX_ALERT) {
		a.code = ramon_spfi_rd(sp, RAMON_SPFI_RX_ALERT_CODE);
		a.sub_code = ramon_spfi_rd(sp, RAMON_SPFI_RX_ALERT_SUB_CODE);
		a.param1 = ramon_spfi_rd(sp, RAMON_SPFI_RX_ALERT_PARAM1);
		a.param2 = ramon_spfi_rd(sp, RAMON_SPFI_RX_ALERT_PARAM2);
		a.rx_status = ramon_spfi_rd(sp, RAMON_SPFI_RX_STATUS);
		if (!kfifo_put(&sp->alerts, a))
			atomic64_inc(&stats->spfi_alert_overrun[sp->nn]);
		return true;
	}
	if (op == RAMON_SPFI_RX_DATA_FROM_STREAM && sp->read_armed) {
		sp->read_armed = false;
		complete(&sp->read_done);
	} else if (op != RAMON_SPFI_RX_DATA_FROM_STREAM && sp->cmd_pending) {
		sp->cmd_rx_opcode = op;
		sp->cmd_pending = false;
		complete(&sp->cmd_done);
	} else {
		atomic64_inc(&stats->spfi_unexpected[sp->nn]);
	}
	return false;
}

static irqreturn_t ramon_spfi_irq(int irq, void *data)
{
	struct ramon_spfi *sp = data;
	bool alert = false;
	u32 vec;

	spin_lock(&sp->lock);
	vec = ramon_spfi_rd(sp, RAMON_SPFI_IRQ_VECTOR_INT);
	ramon_spfi_wr(sp, RAMON_SPFI_IRQ_VECTOR_SRC, vec);
	if (!vec) {
		spin_unlock(&sp->lock);
		return IRQ_NONE;
	}
	atomic64_inc(&sp->rd->stats.spfi_irq_hist[sp->nn][vec & (RAMON_SPFI_IRQ_VECTORS - 1)]);
	/* VC1TX also acks CLOSE/FLUSH; those never overlap an armed write */
	if ((vec & RAMON_SPFI_IRQ_VC1TX) && sp->write_armed) {
		sp->write_armed = false;
		complete(&sp->write_done);
	}
	/* VC0RX and VC1RX share RX_COMMAND: handle the opcode once */
	if (vec & (RAMON_SPFI_IRQ_VC0RX | RAMON_SPFI_IRQ_VC1RX))
		alert = ramon_spfi_rx(sp);
	spin_unlock(&sp->lock);
	if (alert)
		wake_up(&sp->alert_wq);
	return IRQ_HANDLED;
}

/* ---- helpers ---- */

static int ramon_spfi_get(struct ramon_dev *rd, u32 nn, struct ramon_spfi **spp,
			  struct ramon_status *st)
{
	struct ramon_spfi *sp;

	if (nn >= RAMON_NN_COUNT)
		return ramon_fail(st, RAMON_E_SPFI_BAD_NN, nn, RAMON_NN_COUNT,
				  "spfi nn %u: only %u NNs", nn, RAMON_NN_COUNT);
	sp = &rd->spfi[array_index_nospec(nn, RAMON_NN_COUNT)];
	if (!sp->regs)
		return ramon_fail(st, RAMON_E_NOT_PRESENT, nn, 0,
				  "spfi%u: not present on this board (see probe log)", nn);
	*spp = sp;
	return 0;
}

static int ramon_spfi_lock(struct ramon_spfi *sp, struct mutex *m, const char *what,
			   struct ramon_status *st)
{
	if (mutex_lock_interruptible(m))
		return ramon_fail(st, RAMON_E_INTERRUPTED, sp->nn, 0,
				  "spfi%u: interrupted waiting for the %s lock", sp->nn, what);
	if (READ_ONCE(sp->rd->dead)) {
		mutex_unlock(m);
		return ramon_fail(st, RAMON_E_REMOVED, sp->nn, 0, "spfi%u: device removed",
				  sp->nn);
	}
	return 0;
}

/* waits for an armed completion; -ENODEV at once if the device is going away */
static long ramon_spfi_wait(struct ramon_spfi *sp, struct completion *done, u32 timeout_ms)
{
	/* after arming: pairs with smp_store_mb(->dead) in remove(), which then complete_all()s */
	smp_mb();
	if (READ_ONCE(sp->rd->dead))
		return -ENODEV;
	return wait_for_completion_interruptible_timeout(done, msecs_to_jiffies(timeout_ms));
}

/*
 * Common tail of a failed completion wait. @armed is cleared under the lock;
 * if the IRQ completed it meanwhile the operation still counts as failed.
 */
static int ramon_spfi_wait_failed(struct ramon_spfi *sp, bool *armed, long left, u32 code,
				  u32 timeout_ms, const char *what, u32 opcode,
				  struct ramon_status *st)
{
	struct device *dev = &sp->rd->pdev->dev;

	spin_lock_irq(&sp->lock);
	*armed = false;
	spin_unlock_irq(&sp->lock);
	if (READ_ONCE(sp->rd->dead))
		return ramon_fail(st, RAMON_E_REMOVED, sp->nn, opcode,
				  "spfi%u: device removed during %s", sp->nn, what);
	if (left < 0)
		return ramon_fail(st, RAMON_E_INTERRUPTED, sp->nn, opcode,
				  "spfi%u: %s interrupted by a signal", sp->nn, what);
	atomic64_inc(&sp->rd->stats.spfi_timeouts[sp->nn]);
	dev_warn_ratelimited(dev, "spfi%u (%s): %s (opcode 0x%x) not acknowledged after %u ms\n",
			     sp->nn, sp->node_name, what, opcode, timeout_ms);
	return ramon_fail(st, code, opcode, timeout_ms,
			  "spfi%u: %s (opcode 0x%x) not acknowledged after %u ms",
			  sp->nn, what, opcode, timeout_ms);
}

/* ---- SPFI_CMD ---- */

static const struct ramon_spfi_op *ramon_spfi_op(u32 opcode)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(ramon_spfi_ops); i++)
		if (ramon_spfi_ops[i].tx == opcode)
			return &ramon_spfi_ops[i];
	return NULL;
}

/* TX registers of a short command; sp->lock held */
static void ramon_spfi_program(struct ramon_spfi *sp, const struct ramon_spfi_op *op,
			       const struct ramon_spfi_cmd *p)
{
	ramon_spfi_wr(sp, RAMON_SPFI_TX_SRC_LOGICAL_ADDR, RAMON_SPFI_SRC_LOGICAL_ADDR_VAL);
	if (op->params & SPFI_P_STREAM_ID)
		ramon_spfi_wr(sp, RAMON_SPFI_TX_STREAM_ID, p->stream_id);
	if (op->params & SPFI_P_STREAM_TYPE)
		ramon_spfi_wr(sp, RAMON_SPFI_TX_STREAM_TYPE, p->stream_type);
	if (op->params & SPFI_P_LAST_OFFSET)
		ramon_spfi_wr(sp, RAMON_SPFI_TX_STREAM_LAST_OFFSET, p->stream_last_offset);
	if (op->params & SPFI_P_INIT_TYPE)
		ramon_spfi_wr(sp, RAMON_SPFI_TX_INIT_TYPE, p->init_type);
	if (op->params & SPFI_P_TOD)
		ramon_spfi_wr(sp, RAMON_SPFI_TX_TOD, p->tod);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_COMMAND, op->tx);
}

/* result registers of the received opcode, plus RX_STATUS; sp->lock held */
static void ramon_spfi_results(struct ramon_spfi *sp, struct ramon_spfi_cmd *p)
{
	switch (p->rx_opcode) {
	case RAMON_SPFI_RX_FLUSH_EXECUTED:
		p->rx_offset = ramon_spfi_rd(sp, RAMON_SPFI_RX_OFFSET);
		fallthrough;
	case RAMON_SPFI_RX_RCV_READY:
	case RAMON_SPFI_RX_STREAM_WAS_CLOSED:
	case RAMON_SPFI_RX_STREAM_WAS_DELETED:
		p->rx_stream_id = ramon_spfi_rd(sp, RAMON_SPFI_RX_STR_ID);
		p->rx_err_code = ramon_spfi_rd(sp, RAMON_SPFI_RX_ERR_CODE);
		break;
	case RAMON_SPFI_RX_INIT_COMPLETED:
		p->rx_err_code = ramon_spfi_rd(sp, RAMON_SPFI_RX_ERR_CODE);
		p->rx_init_info = ramon_spfi_rd(sp, RAMON_SPFI_RX_INIT_INFO);
		break;
	case RAMON_SPFI_RX_TOD_WAS_SET:
		p->rx_err_code = ramon_spfi_rd(sp, RAMON_SPFI_RX_ERR_CODE);
		p->rx_curr_tod = ramon_spfi_rd(sp, RAMON_SPFI_RX_CURR_TOD);
		break;
	case RAMON_SPFI_RX_FORMAT_COMPLETED:
		p->rx_err_code = ramon_spfi_rd(sp, RAMON_SPFI_RX_ERR_CODE);
		break;
	default:
		break;
	}
	p->rx_status = ramon_spfi_rd(sp, RAMON_SPFI_RX_STATUS);
}

/* send, then (if asked) wait for the answer; short_lock held */
static int ramon_spfi_cmd_run(struct ramon_spfi *sp, const struct ramon_spfi_op *op,
			      struct ramon_spfi_cmd *p, struct ramon_status *st)
{
	u32 timeout_ms = ramon_timeout_ms(p->timeout_ms, op->long_timeout ?
					  RAMON_TMO_SPFI_CMD_LONG_MS : RAMON_TMO_SPFI_CMD_MS);
	long left;
	int ret;

	ret = ramon_spfi_lock(sp, &sp->cmd_lock, "command", st);
	if (ret)
		return ret;
	spin_lock_irq(&sp->lock);
	sp->cmd_pending = p->wait_ack;
	reinit_completion(&sp->cmd_done);
	ramon_spfi_program(sp, op, p);
	spin_unlock_irq(&sp->lock);
	mutex_unlock(&sp->cmd_lock);
	if (!p->wait_ack)
		return 0;

	left = ramon_spfi_wait(sp, &sp->cmd_done, timeout_ms);
	if (left <= 0 || READ_ONCE(sp->rd->dead))
		return ramon_spfi_wait_failed(sp, &sp->cmd_pending, left,
					      RAMON_E_SPFI_CMD_TIMEOUT, timeout_ms, op->name,
					      op->tx, st);

	spin_lock_irq(&sp->lock);
	p->rx_opcode = sp->cmd_rx_opcode;
	ramon_spfi_results(sp, p);
	spin_unlock_irq(&sp->lock);
	if (p->rx_opcode != op->rx)
		return ramon_fail(st, RAMON_E_SPFI_UNEXPECTED_OPCODE, op->rx, p->rx_opcode,
				  "spfi%u: %s (0x%x) answered 0x%x, expected 0x%x (rx_err_code %u)",
				  sp->nn, op->name, op->tx, p->rx_opcode, op->rx, p->rx_err_code);
	if (p->rx_err_code)
		ramon_note(st, "spfi%u: %s stream %u acked with rx_err_code %u",
			   sp->nn, op->name, p->stream_id, p->rx_err_code);
	return 0;
}

static const char *ramon_spfi_opcode_hint(u32 opcode)
{
	if (opcode == RAMON_SPFI_OP_DATA_WRITE)
		return " (use SPFI_WRITE)";
	if (opcode == RAMON_SPFI_OP_READ_STREAM)
		return " (use SPFI_READ)";
	return "";
}

int ramon_ioc_spfi_cmd(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spfi_cmd *p = arg;
	const struct ramon_spfi_op *op;
	struct ramon_spfi *sp;
	int ret;

	ret = ramon_spfi_get(rf->rd, p->nn, &sp, st);
	if (ret)
		return ret;
	op = ramon_spfi_op(p->opcode);
	if (!op)
		return ramon_fail(st, RAMON_E_SPFI_BAD_OPCODE, p->opcode, p->nn,
				  "spfi%u: opcode 0x%x not accepted by SPFI_CMD%s",
				  p->nn, p->opcode, ramon_spfi_opcode_hint(p->opcode));
	p->rx_opcode = 0;
	p->rx_err_code = 0;
	p->rx_stream_id = 0;
	p->rx_offset = 0;
	p->rx_init_info = 0;
	p->rx_curr_tod = 0;
	p->rx_status = 0;

	if (op->vc1tx_ack) {
		ret = ramon_spfi_lock(sp, &sp->write_lock, "write", st);
		if (ret)
			return ret;
	}
	ret = ramon_spfi_lock(sp, &sp->short_lock, "short command", st);
	if (!ret) {
		ret = ramon_spfi_cmd_run(sp, op, p, st);
		mutex_unlock(&sp->short_lock);
	}
	if (op->vc1tx_ack)
		mutex_unlock(&sp->write_lock);
	return ret;
}

/* ---- SPFI_WRITE: DATA_WRITE + AXI transfer + VC1TX ---- */

static int ramon_spfi_write_run(struct ramon_spfi *sp, struct ramon_spfi_write *p,
				struct ramon_axi_job *job, u32 timeout_ms,
				struct ramon_status *st)
{
	u32 gap_us;
	long left;
	int ret;

	ret = ramon_spfi_lock(sp, &sp->cmd_lock, "command", st);
	if (ret)
		return ret;
	spin_lock_irq(&sp->lock);
	sp->write_armed = true;
	reinit_completion(&sp->write_done);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_SRC_LOGICAL_ADDR, RAMON_SPFI_SRC_LOGICAL_ADDR_VAL);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_STREAM_ID, p->stream_id);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_NUM_OFFSETS, p->tx_num_offset);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_OFFSET, p->tx_offset);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_STREAM_TYPE, p->stream_type);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_STREAM_LAST_OFFSET, p->stream_last_offset);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_COMMAND, RAMON_SPFI_OP_DATA_WRITE);
	spin_unlock_irq(&sp->lock);
	mutex_unlock(&sp->cmd_lock);

	gap_us = ramon_param_spfi_write_gap_us();
	if (gap_us)
		fsleep(gap_us);
	/* only write_lock is held while the channel lock is taken */
	ret = ramon_axi_job_run(sp->rd, job, timeout_ms, st);
	if (ret) {
		spin_lock_irq(&sp->lock);
		sp->write_armed = false;
		spin_unlock_irq(&sp->lock);
		return ret;
	}
	left = ramon_spfi_wait(sp, &sp->write_done, timeout_ms);
	if (left <= 0 || READ_ONCE(sp->rd->dead))
		return ramon_spfi_wait_failed(sp, &sp->write_armed, left,
					      RAMON_E_SPFI_WRITE_TIMEOUT, timeout_ms, "DATA_WRITE",
					      RAMON_SPFI_OP_DATA_WRITE, st);
	spin_lock_irq(&sp->lock);
	p->rx_status = ramon_spfi_rd(sp, RAMON_SPFI_RX_STATUS);
	spin_unlock_irq(&sp->lock);
	return 0;
}

int ramon_ioc_spfi_write(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spfi_write *p = arg;
	u64 want = (u64)p->tx_num_offset * RAMON_SPFI_OFFSET_BYTES;
	u32 timeout_ms = ramon_timeout_ms(p->timeout_ms, RAMON_TMO_SPFI_CMD_MS);
	struct ramon_axi_job job;
	struct ramon_spfi *sp;
	int ret;

	p->rx_status = 0;
	ret = ramon_spfi_get(rf->rd, p->nn, &sp, st);
	if (ret)
		return ret;
	/* validate everything before the NN is told to expect data */
	ret = ramon_axi_job_prepare(rf, p->chan, p->items, p->n_items, &job, st);
	if (ret)
		goto out;
	if (!p->tx_num_offset || job.bytes != want) {
		ret = ramon_fail(st, RAMON_E_INVAL_ARG, job.bytes, p->tx_num_offset,
				 "spfi%u write: items total 0x%llx, tx_num_offset %u x 16 KiB = 0x%llx",
				 p->nn, job.bytes, p->tx_num_offset, want);
		goto out;
	}
	ret = ramon_spfi_lock(sp, &sp->write_lock, "write", st);
	if (ret)
		goto out;
	ret = ramon_spfi_write_run(sp, p, &job, timeout_ms, st);
	mutex_unlock(&sp->write_lock);
out:
	ramon_axi_job_release(&job);
	return ret;
}

/* ---- device memory copies (any alignment: bytes until 4-aligned, words, tail) ---- */

static void ramon_io_read(void *dst, const void __iomem *src, size_t n)
{
	u8 *d = dst;
	u32 v;

	for (; n && ((uintptr_t)src & (sizeof(u32) - 1)); n--)
		*d++ = readb(src++);
	for (; n >= sizeof(u32); n -= sizeof(u32), src += sizeof(u32), d += sizeof(u32)) {
		v = readl(src);
		memcpy(d, &v, sizeof(v));
	}
	for (; n; n--)
		*d++ = readb(src++);
}

static void ramon_io_write(void __iomem *dst, const void *src, size_t n)
{
	const u8 *s = src;
	u32 v;

	for (; n && ((uintptr_t)dst & (sizeof(u32) - 1)); n--)
		writeb(*s++, dst++);
	for (; n >= sizeof(u32); n -= sizeof(u32), dst += sizeof(u32), s += sizeof(u32)) {
		memcpy(&v, s, sizeof(v));
		writel(v, dst);
	}
	for (; n; n--)
		writeb(*s++, dst++);
}

/* ---- SPFI_READ: offsets table + READ_STREAM + VC1RX 0x98 ---- */

/*
 * Offsets go to the TX read-offset table from byte 0. For an odd count a zero
 * word follows the last one, as the old spfi_receive() wrote it (8 bytes).
 */
static void ramon_spfi_put_offsets(struct ramon_spfi *sp, const u32 *offsets, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		writel(offsets[i], sp->tx_offs + i * sizeof(u32));
	if ((n & 1) && (n + 1) * sizeof(u32) <= sp->tx_offs_size)
		writel(0, sp->tx_offs + n * sizeof(u32));
}

/* @issued is set once READ_STREAM was written: the FPGA may DMA into dst from then on */
static int ramon_spfi_read_run(struct ramon_spfi *sp, struct ramon_spfi_read *p, dma_addr_t dst,
			       u32 timeout_ms, bool *issued, struct ramon_status *st)
{
	long left;
	int ret;

	ret = ramon_spfi_lock(sp, &sp->cmd_lock, "command", st);
	if (ret)
		return ret;
	*issued = true;
	spin_lock_irq(&sp->lock);
	sp->read_armed = true;
	reinit_completion(&sp->read_done);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_SRC_LOGICAL_ADDR, RAMON_SPFI_SRC_LOGICAL_ADDR_VAL);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_STREAM_ID, p->stream_id);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_NUM_OFFSETS, p->n_offsets);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_RD_BURST_ADDR_LO, lower_32_bits(dst));
	ramon_spfi_wr(sp, RAMON_SPFI_TX_RD_BURST_ADDR_HI, upper_32_bits(dst));
	udelay(RAMON_SPFI_READ_SETUP_US);
	ramon_spfi_wr(sp, RAMON_SPFI_TX_COMMAND, RAMON_SPFI_OP_READ_STREAM);
	spin_unlock_irq(&sp->lock);
	mutex_unlock(&sp->cmd_lock);

	left = ramon_spfi_wait(sp, &sp->read_done, timeout_ms);
	if (left <= 0 || READ_ONCE(sp->rd->dead))
		return ramon_spfi_wait_failed(sp, &sp->read_armed, left,
					      RAMON_E_SPFI_READ_TIMEOUT, timeout_ms, "READ_STREAM",
					      RAMON_SPFI_OP_READ_STREAM, st);
	spin_lock_irq(&sp->lock);
	p->rx_status = ramon_spfi_rd(sp, RAMON_SPFI_RX_STATUS);
	spin_unlock_irq(&sp->lock);
	return 0;
}

int ramon_ioc_spfi_read(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spfi_read *p = arg;
	u32 timeout_ms = ramon_timeout_ms(p->timeout_ms, RAMON_TMO_SPFI_CMD_MS);
	struct ramon_buf *buf, *stale = NULL;
	struct ramon_spfi *sp;
	bool issued = false;
	u32 *offsets;
	int ret;

	p->rx_status = 0;
	ret = ramon_spfi_get(rf->rd, p->nn, &sp, st);
	if (ret)
		return ret;
	if (!sp->tx_offs)
		return ramon_fail(st, RAMON_E_NOT_PRESENT, p->nn, 0,
				  "spfi%u: TX read-offset table not found in the DT", p->nn);
	if (!p->n_offsets || p->n_offsets > sp->tx_offs_size / sizeof(u32))
		return ramon_fail(st, RAMON_E_BAD_COUNT, p->n_offsets,
				  sp->tx_offs_size / sizeof(u32),
				  "spfi%u read: n_offsets %u not in 1..%zu", p->nn, p->n_offsets,
				  sp->tx_offs_size / sizeof(u32));
	ret = ramon_buf_ref(rf, p->dst_handle, p->dst_off,
			    (u64)p->n_offsets * RAMON_SPFI_OFFSET_BYTES, "spfi read dst", 0,
			    RAMON_E_SPFI_DST_TOO_SMALL, &buf, st);
	if (ret)
		return ret;
	offsets = memdup_user(u64_to_user_ptr(p->offsets), array_size(p->n_offsets, sizeof(u32)));
	if (IS_ERR(offsets)) {
		ret = ramon_fail(st, PTR_ERR(offsets) == -EFAULT ? RAMON_E_COPY_FAULT :
				 RAMON_E_NO_MEMORY, p->offsets, p->n_offsets,
				 "spfi%u read: cannot copy %u offsets from 0x%llx (%ld)",
				 p->nn, p->n_offsets, p->offsets, PTR_ERR(offsets));
		goto out_buf;
	}
	ret = ramon_spfi_lock(sp, &sp->read_lock, "read", st);
	if (ret)
		goto out;
	ramon_spfi_put_offsets(sp, offsets, p->n_offsets);
	ret = ramon_spfi_read_run(sp, p, buf->dma + p->dst_off, timeout_ms, &issued, st);
	/*
	 * An unanswered READ_STREAM may still land later: its buffer stays
	 * referenced until a later read completes (answers come in order) or
	 * remove(). The caller's reference moves into that slot.
	 */
	if (ret && issued) {
		stale = sp->read_stale;
		sp->read_stale = buf;
		buf = NULL;
	} else if (!ret) {
		stale = sp->read_stale;
		sp->read_stale = NULL;
	}
	mutex_unlock(&sp->read_lock);
	if (stale)
		ramon_buf_put(stale);
out:
	kfree(offsets);
out_buf:
	if (buf)
		ramon_buf_put(buf);
	return ret;
}

/* ---- alerts ---- */

/* wait condition: an alert was popped, the waiter was cancelled or the device is going away */
static bool ramon_spfi_alert_ready(struct ramon_spfi *sp, u32 gen, struct ramon_spfi_alert *a,
				   bool *got)
{
	bool done;

	spin_lock_irq(&sp->lock);
	*got = kfifo_get(&sp->alerts, a);
	done = *got || sp->alert_gen != gen;
	spin_unlock_irq(&sp->lock);
	return done || READ_ONCE(sp->rd->dead);
}

int ramon_ioc_spfi_wait_alert(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spfi_wait_alert *p = arg;
	u32 timeout_ms = min_t(u32, p->timeout_ms, RAMON_TIMEOUT_MAX_MS);
	struct ramon_spfi_alert a;
	struct ramon_spfi *sp;
	bool got = false;
	long left;
	u32 gen;
	int ret;

	ret = ramon_spfi_get(rf->rd, p->nn, &sp, st);
	if (ret)
		return ret;
	spin_lock_irq(&sp->lock);
	gen = sp->alert_gen;
	spin_unlock_irq(&sp->lock);

	/* timeout_ms 0: wait until an alert, a cancel, a signal or remove() */
	if (timeout_ms)
		left = wait_event_interruptible_timeout(sp->alert_wq,
							ramon_spfi_alert_ready(sp, gen, &a, &got),
							msecs_to_jiffies(timeout_ms));
	else
		left = wait_event_interruptible(sp->alert_wq,
						ramon_spfi_alert_ready(sp, gen, &a, &got)) ?: 1;
	if (got) {
		p->code = a.code;
		p->sub_code = a.sub_code;
		p->param1 = a.param1;
		p->param2 = a.param2;
		p->rx_status = a.rx_status;
		return 0;
	}
	if (READ_ONCE(rf->rd->dead))
		return ramon_fail(st, RAMON_E_REMOVED, p->nn, 0, "spfi%u: device removed", p->nn);
	if (left > 0)
		return ramon_fail(st, RAMON_E_SPFI_ALERT_CANCELLED, p->nn, 0,
				  "spfi%u: alert wait cancelled by SPFI_CANCEL_ALERT", p->nn);
	if (left < 0)
		return ramon_fail(st, RAMON_E_INTERRUPTED, p->nn, 0,
				  "spfi%u: alert wait interrupted by a signal", p->nn);
	return ramon_fail(st, RAMON_E_SPFI_ALERT_TIMEOUT, p->nn, timeout_ms,
			  "spfi%u: no alert within %u ms", p->nn, timeout_ms);
}

int ramon_ioc_spfi_cancel_alert(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spfi_cancel_alert *p = arg;
	struct ramon_spfi *sp;
	int ret;

	ret = ramon_spfi_get(rf->rd, p->nn, &sp, st);
	if (ret)
		return ret;
	spin_lock_irq(&sp->lock);
	sp->alert_gen++;
	spin_unlock_irq(&sp->lock);
	wake_up_all(&sp->alert_wq);
	return 0;
}

/* ---- the two DT-label memories ---- */

int ramon_ioc_spfi_mem_read(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spfi_mem_read *p = arg;
	struct ramon_spfi *sp;
	void __iomem *io;
	u32 end;
	void *tmp;
	int ret;

	ret = ramon_spfi_get(rf->rd, p->nn, &sp, st);
	if (ret)
		return ret;
	if (!sp->rx_mem_size)
		return ramon_fail(st, RAMON_E_NOT_PRESENT, p->nn, 0,
				  "spfi%u: RX message table not found in the DT", p->nn);
	if (!p->size || p->size > RAMON_SPFI_MEM_READ_MAX)
		return ramon_fail(st, RAMON_E_INVAL_ARG, p->size, RAMON_SPFI_MEM_READ_MAX,
				  "spfi%u mem read: size %u not in 1..%u", p->nn, p->size,
				  RAMON_SPFI_MEM_READ_MAX);
	if (check_add_overflow(p->offset, p->size, &end) || end > sp->rx_mem_size)
		return ramon_fail(st, RAMON_E_SPFI_MEM_RANGE, p->offset, p->size,
				  "spfi%u mem read: offset 0x%x + size 0x%x > table size 0x%x",
				  p->nn, p->offset, p->size, sp->rx_mem_size);
	tmp = kmalloc(p->size, GFP_KERNEL);
	if (!tmp)
		return ramon_fail(st, RAMON_E_NO_MEMORY, p->size, 0,
				  "spfi%u mem read: no memory for 0x%x bytes", p->nn, p->size);
	io = ioremap(sp->rx_mem + p->offset, p->size);
	if (!io) {
		ret = ramon_fail(st, RAMON_E_NO_MEMORY, p->offset, p->size,
				 "spfi%u mem read: ioremap(%pa + 0x%x, 0x%x) failed",
				 p->nn, &sp->rx_mem, p->offset, p->size);
		goto out;
	}
	ramon_io_read(tmp, io, p->size);
	iounmap(io);
	if (copy_to_user(u64_to_user_ptr(p->data), tmp, p->size))
		ret = ramon_fail(st, RAMON_E_COPY_FAULT, p->data, p->size,
				 "spfi%u mem read: cannot copy 0x%x bytes to 0x%llx",
				 p->nn, p->size, p->data);
out:
	kfree(tmp);
	return ret;
}

int ramon_ioc_spfi_tx_offs_write(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_spfi_tx_offs_write *p = arg;
	struct ramon_spfi *sp;
	void *tmp;
	u32 end;
	int ret;

	ret = ramon_spfi_get(rf->rd, p->nn, &sp, st);
	if (ret)
		return ret;
	if (!sp->tx_offs)
		return ramon_fail(st, RAMON_E_NOT_PRESENT, p->nn, 0,
				  "spfi%u: TX read-offset table not found in the DT", p->nn);
	if (!p->size || p->size > RAMON_SPFI_TX_OFFS_MAX)
		return ramon_fail(st, RAMON_E_INVAL_ARG, p->size, RAMON_SPFI_TX_OFFS_MAX,
				  "spfi%u tx offs write: size %u not in 1..%u", p->nn, p->size,
				  RAMON_SPFI_TX_OFFS_MAX);
	if (check_add_overflow(p->offset, p->size, &end) || end > sp->tx_offs_size)
		return ramon_fail(st, RAMON_E_SPFI_OFFS_RANGE, p->offset, p->size,
				  "spfi%u tx offs write: offset 0x%x + size 0x%x > table size 0x%x",
				  p->nn, p->offset, p->size, sp->tx_offs_size);
	tmp = memdup_user(u64_to_user_ptr(p->data), p->size);
	if (IS_ERR(tmp))
		return ramon_fail(st, PTR_ERR(tmp) == -EFAULT ? RAMON_E_COPY_FAULT :
				  RAMON_E_NO_MEMORY, p->data, p->size,
				  "spfi%u tx offs write: cannot copy 0x%x bytes from 0x%llx (%ld)",
				  p->nn, p->size, p->data, PTR_ERR(tmp));
	ret = ramon_spfi_lock(sp, &sp->read_lock, "read", st);
	if (!ret) {
		ramon_io_write(sp->tx_offs + p->offset, tmp, p->size);
		mutex_unlock(&sp->read_lock);
	}
	kfree(tmp);
	return ret;
}

/* ---- probe / remove ---- */

static void ramon_spfi_init(struct ramon_dev *rd, u32 nn)
{
	struct ramon_spfi *sp = &rd->spfi[nn];

	sp->rd = rd;
	sp->nn = nn;
	sp->node_name = rd->win[RAMON_WIN_SPFI0 + nn].name;
	spin_lock_init(&sp->lock);
	mutex_init(&sp->cmd_lock);
	mutex_init(&sp->write_lock);
	mutex_init(&sp->read_lock);
	mutex_init(&sp->short_lock);
	init_completion(&sp->cmd_done);
	init_completion(&sp->write_done);
	init_completion(&sp->read_done);
	INIT_KFIFO(sp->alerts);
	init_waitqueue_head(&sp->alert_wq);
}

static void ramon_spfi_mems(struct ramon_dev *rd, struct ramon_spfi *sp,
			    const struct ramon_of_hw *hw)
{
	struct device *dev = &rd->pdev->dev;
	const struct resource *r;
	u32 size;

	if (hw->spfi_tx_offs_ok[sp->nn]) {
		r = &hw->spfi_tx_offs[sp->nn];
		size = min_t(resource_size_t, resource_size(r), RAMON_SPFI_TX_OFFS_MAX);
		sp->tx_offs = devm_ioremap(dev, r->start, size);
		if (sp->tx_offs)
			sp->tx_offs_size = size;
		dev_info(dev, "spfi%u: TX read-offset table %pa, 0x%x bytes%s\n", sp->nn,
			 &r->start, size, sp->tx_offs ? "" : ", ioremap failed");
	}
	if (hw->spfi_rx_mem_ok[sp->nn]) {
		r = &hw->spfi_rx_mem[sp->nn];
		sp->rx_mem = r->start;
		sp->rx_mem_size = min_t(resource_size_t, resource_size(r),
					RAMON_SPFI_MEM_READ_MAX);
		dev_info(dev, "spfi%u: RX message table %pa, 0x%x bytes\n", sp->nn,
			 &sp->rx_mem, sp->rx_mem_size);
	}
}

/* after ramon_regwin_probe(): the registers come from its spfi<nn> windows */
void ramon_spfi_probe(struct ramon_dev *rd, const struct ramon_of_hw *hw)
{
	struct device *dev = &rd->pdev->dev;
	struct ramon_regwin *win;
	struct ramon_spfi *sp;
	int ret;
	u32 nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		ramon_spfi_init(rd, nn);
		sp = &rd->spfi[nn];
		win = &rd->win[RAMON_WIN_SPFI0 + nn];
		if (!(ramon_param_spfi_mask() & BIT(nn))) {
			dev_info(dev, "spfi%u: not used (spfi_mask=0x%x)\n", nn,
				 ramon_param_spfi_mask());
			continue;
		}
		if (!hw->spfi_irq_ok || (win->flags & RAMON_REGWIN_ABSENT))
			continue;
		/* REG_IO on this window then takes the same lock as the IRQ */
		sp->regs = win->base;
		win->lock = &sp->lock;
		ret = devm_request_irq(dev, hw->spfi_irq[nn], ramon_spfi_irq, 0,
				       ramon_spfi_irq_names[nn], sp);
		if (ret) {
			dev_warn(dev, "spfi%u (%s): request_irq(%d) failed: %d; SPFI disabled\n",
				 nn, sp->node_name, hw->spfi_irq[nn], ret);
			sp->regs = NULL;
			win->lock = NULL;
			continue;
		}
		sp->irq = hw->spfi_irq[nn];
		ramon_spfi_mems(rd, sp, hw);
		dev_info(dev, "spfi%u %s: irq %d\n", nn, sp->node_name, sp->irq);
	}
}

void ramon_spfi_free_irqs(struct ramon_dev *rd)
{
	u32 nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++)
		if (rd->spfi[nn].irq)
			devm_free_irq(&rd->pdev->dev, rd->spfi[nn].irq, &rd->spfi[nn]);
}

/* after the gate is drained: drop buffers of reads that never completed */
void ramon_spfi_remove(struct ramon_dev *rd)
{
	u32 nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (rd->spfi[nn].read_stale)
			ramon_buf_put(rd->spfi[nn].read_stale);
		rd->spfi[nn].read_stale = NULL;
	}
}

void ramon_spfi_wake(struct ramon_dev *rd)
{
	struct ramon_spfi *sp;
	u32 nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		sp = &rd->spfi[nn];
		if (!sp->regs)
			continue;
		complete_all(&sp->cmd_done);
		complete_all(&sp->write_done);
		complete_all(&sp->read_done);
		wake_up_all(&sp->alert_wq);
	}
}

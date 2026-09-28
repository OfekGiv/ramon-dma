// SPDX-License-Identifier: GPL-2.0
/*
 * Platform driver, /dev/ramon_dma and the table-driven ioctl dispatcher.
 */
#define pr_fmt(fmt) "ramon_dma: " fmt

#include <linux/build_bug.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/nospec.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/stddef.h>
#include <linux/uaccess.h>

#include "ramon_dma.h"

static unsigned int max_buf_mb = RAMON_MAX_BUF_MB_DEFAULT;
module_param(max_buf_mb, uint, 0444);
MODULE_PARM_DESC(max_buf_mb, "largest single BUF_ALLOC in MiB (default "
		 __stringify(RAMON_MAX_BUF_MB_DEFAULT) ")");

/* SPFI instances to use; the DT lists both NNs, the board wires only NN0 */
static unsigned int spfi_mask = RAMON_SPFI_MASK_DEFAULT;
module_param(spfi_mask, uint, 0444);
MODULE_PARM_DESC(spfi_mask, "SPFI NNs to use, bit n = NN n (default "
		 __stringify(RAMON_SPFI_MASK_DEFAULT) ")");

static unsigned int zdma_channels = RAMON_ZDMA_CHANNELS_DEFAULT;
module_param(zdma_channels, uint, 0444);
MODULE_PARM_DESC(zdma_channels, "DMA_MEMCPY channels to take for ZDMA_COPY, 0.."
		 __stringify(RAMON_ZDMA_CHANNELS_MAX) " (default "
		 __stringify(RAMON_ZDMA_CHANNELS_DEFAULT) ")");

/* ---- error codes ---- */

struct ramon_err_entry {
	u32 code;
	u16 err;		/* positive errno */
	const char *name;
};

#define RAMON_ERR_ENTRY(name, code, err, desc)	{ code, err, "RAMON_E_" #name },

static const struct ramon_err_entry ramon_errs[] = {
	RAMON_ERR_LIST(RAMON_ERR_ENTRY)
};

/* Failure paths only, so a linear scan is fine. A bad code is a driver bug. */
static const struct ramon_err_entry *ramon_err_lookup(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ramon_errs); i++)
		if (ramon_errs[i].code == code && code != RAMON_E_OK)
			return &ramon_errs[i];
	WARN_ONCE(1, "ramon_dma: failure with invalid code %u\n", code);
	return ramon_err_lookup(RAMON_E_INVAL_ARG);
}

/**
 * ramon_fail() - record a failure in the status trailer
 * @st: trailer of the ioctl struct being handled
 * @code: RAMON_E_* (not RAMON_E_OK)
 * @arg0: most relevant value (handle, index, opcode ...)
 * @arg1: second most relevant value
 * @fmt: message naming the offending values and the bound they violated
 *
 * The only way a handler fails. The dispatcher logs the result with dev_dbg.
 *
 * Return: the negative errno fixed for @code by RAMON_ERR_LIST.
 */
int ramon_fail(struct ramon_status *st, u32 code, u64 arg0, u64 arg1, const char *fmt, ...)
{
	const struct ramon_err_entry *e = ramon_err_lookup(code);
	va_list ap;

	st->code = e->code;
	st->err = -(s32)e->err;
	st->arg[0] = arg0;
	st->arg[1] = arg1;
	va_start(ap, fmt);
	vscnprintf(st->msg, sizeof(st->msg), fmt, ap);
	va_end(ap);
	return st->err;
}

/**
 * ramon_fail_dbg() - fail a path that has no status trailer (mmap)
 * @rd: device, for the log line
 * @code: RAMON_E_* (not RAMON_E_OK)
 * @fmt: message naming the offending values
 *
 * Return: the negative errno fixed for @code by RAMON_ERR_LIST.
 */
int ramon_fail_dbg(struct ramon_dev *rd, u32 code, const char *fmt, ...)
{
	const struct ramon_err_entry *e = ramon_err_lookup(code);
	struct va_format vaf;
	va_list ap;

	va_start(ap, fmt);
	vaf.fmt = fmt;
	vaf.va = &ap;
	dev_dbg(&rd->pdev->dev, "%s: %pV\n", e->name, &vaf);
	va_end(ap);
	return -(int)e->err;
}

/**
 * ramon_note() - informational message in the trailer of a successful ioctl
 * @st: status trailer
 * @fmt: message (e.g. an NN protocol error that is not an ioctl failure)
 */
void ramon_note(struct ramon_status *st, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vscnprintf(st->msg, sizeof(st->msg), fmt, ap);
	va_end(ap);
}

u64 ramon_max_buf_bytes(void)
{
	return (u64)max_buf_mb * SZ_1M;
}

u32 ramon_param_zdma_channels(void)
{
	return zdma_channels;
}

u32 ramon_param_spfi_mask(void)
{
	return spfi_mask;
}

/* ioctl timeouts: 0 selects @def, anything above RAMON_TIMEOUT_MAX_MS is clamped */
u32 ramon_timeout_ms(u32 requested, u32 def)
{
	return requested ? min_t(u32, requested, RAMON_TIMEOUT_MAX_MS) : def;
}

/* ---- ioctl handlers owned by the core ---- */

static int ramon_ioc_get_info(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_get_info *p = arg;
	struct ramon_dev *rd = rf->rd;
	u32 i;

	p->abi_version = RAMON_ABI_VERSION;
	p->drv_major = RAMON_DRV_MAJOR;
	p->drv_minor = RAMON_DRV_MINOR;
	p->drv_patch = RAMON_DRV_PATCH;
	p->n_axi_chan = rd->n_axi;
	p->n_zdma_chan = rd->zdma.n;
	p->n_regwin = rd->n_win;
	p->spw_mask = 0;
	p->spfi_mask = 0;
	for (i = 0; i < RAMON_NN_COUNT; i++) {
		if (rd->spw[i].regs)
			p->spw_mask |= BIT(i);
		if (rd->spfi[i].regs)
			p->spfi_mask |= BIT(i);
	}
	p->page_size = PAGE_SIZE;
	p->max_buf_bytes = ramon_max_buf_bytes();
	return 0;
}

static u64 ramon_stat(atomic64_t *v, bool reset)
{
	return reset ? atomic64_xchg(v, 0) : atomic64_read(v);
}

static int ramon_ioc_get_stats(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_get_stats *p = arg;
	struct ramon_stats *s = &rf->rd->stats;
	bool reset = p->reset;
	u32 nn, v;

	p->axi_tx = ramon_stat(&s->axi_tx, reset);
	p->axi_rx = ramon_stat(&s->axi_rx, reset);
	p->axi_timeouts = ramon_stat(&s->axi_timeouts, reset);
	p->zdma_ops = ramon_stat(&s->zdma_ops, reset);
	p->zdma_timeouts = ramon_stat(&s->zdma_timeouts, reset);
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		p->spw_rx[nn] = ramon_stat(&s->spw_rx[nn], reset);
		p->spw_overrun[nn] = ramon_stat(&s->spw_overrun[nn], reset);
		for (v = 0; v < RAMON_SPFI_IRQ_VECTORS; v++)
			p->spfi_irq_hist[nn][v] = ramon_stat(&s->spfi_irq_hist[nn][v], reset);
		p->spfi_alert_overrun[nn] = ramon_stat(&s->spfi_alert_overrun[nn], reset);
		p->spfi_unexpected[nn] = ramon_stat(&s->spfi_unexpected[nn], reset);
		p->spfi_timeouts[nn] = ramon_stat(&s->spfi_timeouts[nn], reset);
	}
	return 0;
}

/* ---- dispatcher ---- */

/* Large enough for any ioctl struct; the dispatcher copies the exact size. */
union ramon_ioc_buf {
	struct ramon_get_info get_info;
	struct ramon_chan_info chan_info;
	struct ramon_buf_alloc buf_alloc;
	struct ramon_buf_free buf_free;
	struct ramon_buf_info buf_info;
	struct ramon_axi_xfer axi_xfer;
	struct ramon_zdma_copy zdma_copy;
	struct ramon_spw_wait_rx spw_wait_rx;
	struct ramon_spw_cancel spw_cancel;
	struct ramon_spw_loopback spw_loopback;
	struct ramon_spfi_cmd spfi_cmd;
	struct ramon_spfi_write spfi_write;
	struct ramon_spfi_read spfi_read;
	struct ramon_spfi_wait_alert spfi_wait_alert;
	struct ramon_spfi_cancel_alert spfi_cancel_alert;
	struct ramon_spfi_mem_read spfi_mem_read;
	struct ramon_spfi_tx_offs_write spfi_tx_offs_write;
	struct ramon_regwin_info regwin_info;
	struct ramon_reg_io reg_io;
	struct ramon_get_stats get_stats;
};

/*
 * ABI rules, checked for every ioctl: the command encodes the struct size,
 * sizes are multiples of 8 and the status trailer is the last member.
 */
#define RAMON_ABI_OK(cmd, type)						\
	(_IOC_SIZE(cmd) == sizeof(type) &&				\
	 sizeof(type) % sizeof(__u64) == 0 &&				\
	 offsetof(type, st) + sizeof(struct ramon_status) == sizeof(type))

static_assert(sizeof(struct ramon_status) % sizeof(__u64) == 0);
static_assert(sizeof(struct ramon_sg_item) % sizeof(__u64) == 0);
static_assert(sizeof(struct ramon_copy) % sizeof(__u64) == 0);
static_assert(RAMON_ABI_OK(RAMON_IOC_GET_INFO, struct ramon_get_info));
static_assert(RAMON_ABI_OK(RAMON_IOC_CHAN_INFO, struct ramon_chan_info));
static_assert(RAMON_ABI_OK(RAMON_IOC_BUF_ALLOC, struct ramon_buf_alloc));
static_assert(RAMON_ABI_OK(RAMON_IOC_BUF_FREE, struct ramon_buf_free));
static_assert(RAMON_ABI_OK(RAMON_IOC_BUF_INFO, struct ramon_buf_info));
static_assert(RAMON_ABI_OK(RAMON_IOC_AXI_XFER, struct ramon_axi_xfer));
static_assert(RAMON_ABI_OK(RAMON_IOC_ZDMA_COPY, struct ramon_zdma_copy));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPW_WAIT_RX, struct ramon_spw_wait_rx));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPW_CANCEL, struct ramon_spw_cancel));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPW_LOOPBACK, struct ramon_spw_loopback));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPFI_CMD, struct ramon_spfi_cmd));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPFI_WRITE, struct ramon_spfi_write));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPFI_READ, struct ramon_spfi_read));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPFI_WAIT_ALERT, struct ramon_spfi_wait_alert));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPFI_CANCEL_ALERT, struct ramon_spfi_cancel_alert));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPFI_MEM_READ, struct ramon_spfi_mem_read));
static_assert(RAMON_ABI_OK(RAMON_IOC_SPFI_TX_OFFS_WRITE, struct ramon_spfi_tx_offs_write));
static_assert(RAMON_ABI_OK(RAMON_IOC_REGWIN_INFO, struct ramon_regwin_info));
static_assert(RAMON_ABI_OK(RAMON_IOC_REG_IO, struct ramon_reg_io));
static_assert(RAMON_ABI_OK(RAMON_IOC_GET_STATS, struct ramon_get_stats));

struct ramon_ioctl_desc {
	u16 size;
	u16 status_offset;
	ramon_ioctl_fn handler;
	bool needs_dev_alive;
	const char *name;
};

/* Indexed by _IOC_NR; the dispatcher rebuilds the full command from the entry. */
#define RAMON_IOCTL(_cmd, _type, _fn, _alive)			\
	[_IOC_NR(_cmd)] = {					\
		.size = sizeof(_type),				\
		.status_offset = offsetof(_type, st),		\
		.handler = (_fn),				\
		.needs_dev_alive = (_alive),			\
		.name = #_cmd,					\
	}

static const struct ramon_ioctl_desc ramon_ioctls[] = {
	RAMON_IOCTL(RAMON_IOC_GET_INFO, struct ramon_get_info, ramon_ioc_get_info, false),
	RAMON_IOCTL(RAMON_IOC_CHAN_INFO, struct ramon_chan_info, ramon_ioc_chan_info, true),
	RAMON_IOCTL(RAMON_IOC_BUF_ALLOC, struct ramon_buf_alloc, ramon_ioc_buf_alloc, true),
	RAMON_IOCTL(RAMON_IOC_BUF_FREE, struct ramon_buf_free, ramon_ioc_buf_free, false),
	RAMON_IOCTL(RAMON_IOC_BUF_INFO, struct ramon_buf_info, ramon_ioc_buf_info, false),
	RAMON_IOCTL(RAMON_IOC_AXI_XFER, struct ramon_axi_xfer, ramon_ioc_axi_xfer, true),
	RAMON_IOCTL(RAMON_IOC_ZDMA_COPY, struct ramon_zdma_copy, ramon_ioc_zdma_copy, true),
	RAMON_IOCTL(RAMON_IOC_SPW_WAIT_RX, struct ramon_spw_wait_rx, ramon_ioc_spw_wait_rx, true),
	RAMON_IOCTL(RAMON_IOC_SPW_CANCEL, struct ramon_spw_cancel, ramon_ioc_spw_cancel, true),
	RAMON_IOCTL(RAMON_IOC_SPW_LOOPBACK, struct ramon_spw_loopback, ramon_ioc_spw_loopback,
		    true),
	RAMON_IOCTL(RAMON_IOC_SPFI_CMD, struct ramon_spfi_cmd, ramon_ioc_spfi_cmd, true),
	RAMON_IOCTL(RAMON_IOC_SPFI_WRITE, struct ramon_spfi_write, ramon_ioc_spfi_write, true),
	RAMON_IOCTL(RAMON_IOC_SPFI_READ, struct ramon_spfi_read, ramon_ioc_spfi_read, true),
	RAMON_IOCTL(RAMON_IOC_SPFI_WAIT_ALERT, struct ramon_spfi_wait_alert,
		    ramon_ioc_spfi_wait_alert, true),
	RAMON_IOCTL(RAMON_IOC_SPFI_CANCEL_ALERT, struct ramon_spfi_cancel_alert,
		    ramon_ioc_spfi_cancel_alert, true),
	RAMON_IOCTL(RAMON_IOC_SPFI_MEM_READ, struct ramon_spfi_mem_read, ramon_ioc_spfi_mem_read,
		    true),
	RAMON_IOCTL(RAMON_IOC_SPFI_TX_OFFS_WRITE, struct ramon_spfi_tx_offs_write,
		    ramon_ioc_spfi_tx_offs_write, true),
	RAMON_IOCTL(RAMON_IOC_REGWIN_INFO, struct ramon_regwin_info, ramon_ioc_regwin_info, true),
	RAMON_IOCTL(RAMON_IOC_REG_IO, struct ramon_reg_io, ramon_ioc_reg_io, true),
	RAMON_IOCTL(RAMON_IOC_GET_STATS, struct ramon_get_stats, ramon_ioc_get_stats, false),
};

static long ramon_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ramon_file *rf = file->private_data;
	struct ramon_dev *rd = rf->rd;
	void __user *uarg = (void __user *)arg;
	const struct ramon_ioctl_desc *d;
	union ramon_ioc_buf buf;
	struct ramon_status *st;
	unsigned int nr = _IOC_NR(cmd);
	int idx, ret;

	if (_IOC_TYPE(cmd) != RAMON_IOC_MAGIC || nr >= ARRAY_SIZE(ramon_ioctls))
		return -ENOTTY;
	d = &ramon_ioctls[array_index_nospec(nr, ARRAY_SIZE(ramon_ioctls))];
	if (!d->handler || cmd != _IOC(_IOC_READ | _IOC_WRITE, RAMON_IOC_MAGIC, nr, d->size))
		return -ENOTTY;

	if (copy_from_user(&buf, uarg, d->size))
		return -EFAULT;
	st = (struct ramon_status *)((u8 *)&buf + d->status_offset);
	memset(st, 0, sizeof(*st));

	idx = srcu_read_lock(&rd->gate);
	if (d->needs_dev_alive && READ_ONCE(rd->dead))
		ret = ramon_fail(st, RAMON_E_REMOVED, 0, 0, "%s: device was removed",
				 dev_name(&rd->pdev->dev));
	else
		ret = d->handler(rf, &buf, st);
	srcu_read_unlock(&rd->gate, idx);

	if (ret) {
		/* Handlers must fail through ramon_fail(); keep the trailer coherent if not. */
		if (WARN_ON_ONCE(ret > 0 || st->code == RAMON_E_OK))
			ret = ramon_fail(st, RAMON_E_INVAL_ARG, ret, 0,
					 "internal: %s returned %d without status", d->name, ret);
		dev_dbg(&rd->pdev->dev, "%s: %s (%d): %s\n",
			d->name, ramon_err_lookup(st->code)->name, ret, st->msg);
	} else {
		st->code = RAMON_E_OK;
		st->err = 0;
	}

	if (copy_to_user(uarg, &buf, d->size))
		return -EFAULT;
	return ret;
}

/* ---- file operations ---- */

static void ramon_dev_release(struct kref *kref)
{
	struct ramon_dev *rd = container_of(kref, struct ramon_dev, kref);

	cleanup_srcu_struct(&rd->gate);
	put_device(&rd->pdev->dev);
	kfree(rd);
}

void ramon_dev_get(struct ramon_dev *rd)
{
	kref_get(&rd->kref);
}

/* May sleep. */
void ramon_dev_put(struct ramon_dev *rd)
{
	kref_put(&rd->kref, ramon_dev_release);
}

static int ramon_open(struct inode *inode, struct file *file)
{
	/* misc_open() holds misc_mtx here, so remove() cannot pass misc_deregister(). */
	struct ramon_dev *rd = container_of(file->private_data, struct ramon_dev, misc);
	struct ramon_file *rf;
	int ret;

	if (READ_ONCE(rd->dead))
		return -ENODEV;
	ret = nonseekable_open(inode, file);
	if (ret)
		return ret;

	rf = kzalloc(sizeof(*rf), GFP_KERNEL);
	if (!rf)
		return -ENOMEM;
	mutex_init(&rf->lock);
	idr_init(&rf->bufs);
	ramon_dev_get(rd);
	rf->rd = rd;
	file->private_data = rf;
	return 0;
}

static int ramon_release(struct inode *inode, struct file *file)
{
	struct ramon_file *rf = file->private_data;
	struct ramon_dev *rd = rf->rd;

	ramon_buf_release_all(rf);
	mutex_destroy(&rf->lock);
	kfree(rf);
	ramon_dev_put(rd);
	return 0;
}

static const struct file_operations ramon_fops = {
	.owner		= THIS_MODULE,
	.open		= ramon_open,
	.release	= ramon_release,
	.unlocked_ioctl	= ramon_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.mmap		= ramon_buf_mmap,
};

/* ---- platform driver ---- */

/* register windows, then SPW on top of them; DT discovery happens once here */
static int ramon_probe_blocks(struct ramon_dev *rd)
{
	struct ramon_of_hw *hw;
	int ret;

	hw = kzalloc(sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return -ENOMEM;
	ramon_of_discover(&rd->pdev->dev, hw);
	ret = ramon_regwin_probe(rd, hw);
	if (!ret) {
		ramon_spw_probe(rd, hw);
		ramon_spfi_probe(rd, hw);
	}
	kfree(hw);
	return ret;
}

static int ramon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ramon_dev *rd;
	int ret;

	rd = kzalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return -ENOMEM;
	ret = init_srcu_struct(&rd->gate);
	if (ret) {
		dev_err(dev, "%pOF: init_srcu_struct failed: %d\n", dev->of_node, ret);
		kfree(rd);
		return ret;
	}
	kref_init(&rd->kref);
	rd->pdev = pdev;
	get_device(dev);
	platform_set_drvdata(pdev, rd);

	/* first: the only step that may defer, before anything touches hardware */
	ret = ramon_axidma_probe(rd);
	if (ret)
		goto err_put;
	ret = ramon_zdma_probe(rd);
	if (ret)
		goto err_axi;
	ret = ramon_probe_blocks(rd);
	if (ret) {
		dev_err(dev, "%pOF: register windows: %d\n", dev->of_node, ret);
		goto err_zdma;
	}

	/* Last: /dev/ramon_dma may be opened as soon as it is registered. */
	rd->misc.minor = MISC_DYNAMIC_MINOR;
	rd->misc.name = RAMON_DEV_NAME;
	rd->misc.fops = &ramon_fops;
	rd->misc.parent = dev;
	ret = misc_register(&rd->misc);
	if (ret) {
		dev_err(dev, "%pOF: misc_register(/dev/%s) failed: %d\n",
			dev->of_node, RAMON_DEV_NAME, ret);
		goto err_irqs;
	}

	dev_info(dev, "%pOF: ramon_dma %s (abi %u) ready as /dev/%s\n",
		 dev->of_node, RAMON_DRV_VERSION, RAMON_ABI_VERSION, RAMON_DEV_NAME);
	return 0;

err_irqs:
	/* devm would free them only after rd is gone */
	ramon_spw_free_irqs(rd);
	ramon_spfi_free_irqs(rd);
err_zdma:
	ramon_zdma_remove(rd);
err_axi:
	ramon_axidma_remove(rd);
err_put:
	platform_set_drvdata(pdev, NULL);
	ramon_dev_put(rd);
	return ret;
}

static int ramon_remove(struct platform_device *pdev)
{
	struct ramon_dev *rd = platform_get_drvdata(pdev);

	ramon_spw_free_irqs(rd);
	ramon_spfi_free_irqs(rd);
	/* full barrier: pairs with the smp_mb() between reinit_completion() and the dead check */
	smp_store_mb(rd->dead, true);
	ramon_axidma_wake(rd);
	ramon_zdma_wake(rd);
	ramon_spw_wake(rd);
	ramon_spfi_wake(rd);
	misc_deregister(&rd->misc);

	/* Wait for every in-flight ioctl and mmap to leave; later ones see ->dead. */
	synchronize_srcu(&rd->gate);
	ramon_spfi_remove(rd);
	ramon_zdma_remove(rd);
	ramon_axidma_remove(rd);

	platform_set_drvdata(pdev, NULL);
	dev_dbg(&pdev->dev, "removed; freed when the last file and buffer are gone\n");
	ramon_dev_put(rd);
	return 0;
}

static const struct of_device_id ramon_of_match[] = {
	{ .compatible = RAMON_DT_COMPAT_CORE },
	{ }
};
MODULE_DEVICE_TABLE(of, ramon_of_match);

static struct platform_driver ramon_driver = {
	.driver = {
		.name		= RAMON_DEV_NAME,
		.of_match_table	= ramon_of_match,
	},
	.probe	= ramon_probe,
	.remove	= ramon_remove,
};

static int __init ramon_init(void)
{
	if (!max_buf_mb || max_buf_mb > RAMON_MAX_BUF_MB_LIMIT) {
		pr_err("max_buf_mb=%u out of range 1..%u\n", max_buf_mb, RAMON_MAX_BUF_MB_LIMIT);
		return -EINVAL;
	}
	if (spfi_mask & ~RAMON_SPFI_MASK_ALL) {
		pr_err("spfi_mask=0x%x has bits beyond the %u NNs\n", spfi_mask, RAMON_NN_COUNT);
		return -EINVAL;
	}
	if (zdma_channels > RAMON_ZDMA_CHANNELS_MAX) {
		pr_err("zdma_channels=%u out of range 0..%u\n", zdma_channels,
		       RAMON_ZDMA_CHANNELS_MAX);
		return -EINVAL;
	}
	return platform_driver_register(&ramon_driver);
}
module_init(ramon_init);

static void __exit ramon_exit(void)
{
	platform_driver_unregister(&ramon_driver);
}
module_exit(ramon_exit);

MODULE_DESCRIPTION("ramon board AXI DMA, ZDMA, SpaceWire and SPFI driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(RAMON_DRV_VERSION);
MODULE_SOFTDEP("pre: xilinx_dma zynqmp_dma");

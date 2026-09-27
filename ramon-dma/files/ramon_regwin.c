// SPDX-License-Identifier: GPL-2.0
/*
 * Bounded register windows for REG_IO: RS-TOP, the PS SYSMON/RTC/TTC blocks,
 * the SPW and SPFI register blocks and the DT "reg-access" devices. Every
 * window is mapped once at probe (devm, released after remove() has drained
 * the gate) and keeps its index even when absent on this board.
 */
#define pr_fmt(fmt) "ramon_dma: " fmt

#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/nospec.h>
#include <linux/string.h>

#include "ramon_dma.h"

/* Names window @idx and marks it absent until ramon_win_map() succeeds. */
__printf(3, 4)
static struct ramon_regwin *ramon_win_name(struct ramon_dev *rd, u32 idx, const char *fmt, ...)
{
	struct ramon_regwin *win = &rd->win[idx];
	va_list ap;

	va_start(ap, fmt);
	vscnprintf(win->name, sizeof(win->name), fmt, ap);
	va_end(ap);
	win->flags = RAMON_REGWIN_ABSENT;
	return win;
}

static void ramon_win_map(struct ramon_dev *rd, struct ramon_regwin *win, phys_addr_t phys,
			  u32 size, u32 flags)
{
	struct device *dev = &rd->pdev->dev;
	u32 idx = win - rd->win;

	win->base = devm_ioremap(dev, phys, size);
	if (!win->base) {
		dev_warn(dev, "regwin %u %s: ioremap(%pa, 0x%x) failed; absent\n",
			 idx, win->name, &phys, size);
		return;
	}
	win->phys = phys;
	win->size = size;
	win->flags = flags;
	dev_info(dev, "regwin %u %s: %pa size 0x%x%s\n", idx, win->name, &phys, size,
		 flags & RAMON_REGWIN_RO ? " read-only" : "");
}

static u32 ramon_res_size(const struct resource *res)
{
	return min_t(resource_size_t, resource_size(res), U32_MAX);
}

/* SPW/SPFI blocks are mapped with the fixed window size the old driver used. */
static void ramon_win_map_block(struct ramon_dev *rd, struct ramon_regwin *win,
				const struct resource *res, u32 size)
{
	if (ramon_res_size(res) < size)
		dev_warn(&rd->pdev->dev, "regwin %s: DT reg size 0x%x < register window 0x%x\n",
			 win->name, ramon_res_size(res), size);
	ramon_win_map(rd, win, res->start, size, 0);
}

static void ramon_regwin_rstop(struct ramon_dev *rd, const struct ramon_of_hw *hw)
{
	struct device *dev = &rd->pdev->dev;
	struct ramon_regwin *win;
	u32 size;

	win = ramon_win_name(rd, RAMON_WIN_RS_TOP, RAMON_WIN_NAME_RS_TOP);
	if (!hw->rstop_ok)
		return;
	size = ramon_res_size(&hw->rstop);
	if (size < RAMON_RSTOP_MIN_SIZE) {
		dev_warn(dev, "rs_top: DT reg size 0x%x < 0x%x; mapping 0x%x to reach TRST/SPI_EN at 0x%x\n",
			 size, RAMON_RSTOP_MIN_SIZE, RAMON_RSTOP_MIN_SIZE, RAMON_RSTOP_TRST_SPI_EN);
		size = RAMON_RSTOP_MIN_SIZE;
	}
	ramon_win_map(rd, win, hw->rstop.start, size, 0);
	if (win->base)
		writel(RAMON_RSTOP_TRST_SPI_EN_ON, win->base + RAMON_RSTOP_TRST_SPI_EN);
}

static void ramon_regwin_ps(struct ramon_dev *rd)
{
	u32 i;

	ramon_win_map(rd, ramon_win_name(rd, RAMON_WIN_SYSMON_PS, RAMON_WIN_NAME_SYSMON_PS),
		      RAMON_SYSMON_PS_BASE, RAMON_SYSMON_SIZE, RAMON_REGWIN_RO);
	ramon_win_map(rd, ramon_win_name(rd, RAMON_WIN_SYSMON_PL, RAMON_WIN_NAME_SYSMON_PL),
		      RAMON_SYSMON_PL_BASE, RAMON_SYSMON_SIZE, RAMON_REGWIN_RO);
	ramon_win_map(rd, ramon_win_name(rd, RAMON_WIN_RTC, RAMON_WIN_NAME_RTC),
		      RAMON_RTC_BASE, RAMON_RTC_SIZE, RAMON_REGWIN_RO);
	for (i = 0; i < RAMON_TTC_COUNT; i++)
		ramon_win_map(rd,
			      ramon_win_name(rd, RAMON_WIN_TTC0 + i, RAMON_WIN_NAME_TTC "%u", i),
			      RAMON_TTC_BASE + i * RAMON_TTC_STRIDE, RAMON_TTC_SIZE,
			      RAMON_REGWIN_RO);
}

static void ramon_regwin_spw(struct ramon_dev *rd, const struct ramon_of_hw *hw)
{
	struct ramon_regwin *win;
	u32 nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(hw->spw_mask & BIT(nn))) {
			ramon_win_name(rd, RAMON_WIN_SPW0 + nn, RAMON_WIN_NAME_SPW "%u", nn);
			continue;
		}
		win = ramon_win_name(rd, RAMON_WIN_SPW0 + nn, RAMON_WIN_NAME_SPW "%u@%llx",
				     nn, (u64)hw->spw[nn].start);
		ramon_win_map_block(rd, win, &hw->spw[nn], RAMON_SPW_WIN_SIZE);
	}
}

static void ramon_regwin_spfi(struct ramon_dev *rd, const struct ramon_of_hw *hw)
{
	struct ramon_regwin *win;
	u32 nn;

	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!hw->spfi_ok) {
			ramon_win_name(rd, RAMON_WIN_SPFI0 + nn, RAMON_WIN_NAME_SPFI "%u", nn);
			continue;
		}
		win = ramon_win_name(rd, RAMON_WIN_SPFI0 + nn, RAMON_WIN_NAME_SPFI "%u@%llx",
				     nn, (u64)hw->spfi[nn].start);
		ramon_win_map_block(rd, win, &hw->spfi[nn], RAMON_SPFI_WIN_SIZE);
	}
}

static void ramon_regwin_regaccess(struct ramon_dev *rd, const struct ramon_of_hw *hw)
{
	const struct ramon_of_regdev *ra = hw->regaccess;
	struct ramon_regwin *win;
	u32 i;

	for (i = 0; i < hw->n_regaccess; i++) {
		win = ramon_win_name(rd, RAMON_WIN_FIXED + i, "%s", ra[i].name);
		if (ra[i].present)
			ramon_win_map(rd, win, ra[i].res.start, ramon_res_size(&ra[i].res), 0);
	}
}

int ramon_regwin_probe(struct ramon_dev *rd, const struct ramon_of_hw *hw)
{
	struct device *dev = &rd->pdev->dev;
	u32 n = RAMON_WIN_FIXED + hw->n_regaccess, i, absent = 0;

	rd->win = devm_kcalloc(dev, n, sizeof(*rd->win), GFP_KERNEL);
	if (!rd->win)
		return -ENOMEM;
	rd->n_win = n;

	ramon_regwin_rstop(rd, hw);
	ramon_regwin_ps(rd);
	ramon_regwin_spw(rd, hw);
	ramon_regwin_spfi(rd, hw);
	ramon_regwin_regaccess(rd, hw);

	for (i = 0; i < rd->n_win; i++)
		if (rd->win[i].flags & RAMON_REGWIN_ABSENT)
			absent++;
	dev_info(dev, "%u register windows, %u absent\n", rd->n_win, absent);
	return 0;
}

/* Exact name first, then the part before '@' ("spw0" finds "spw0@a0020000"). */
static struct ramon_regwin *ramon_win_by_name(struct ramon_dev *rd, const char *name)
{
	size_t len = strnlen(name, RAMON_NAME_LEN);
	u32 i;

	for (i = 0; i < rd->n_win; i++)
		if (!strncmp(rd->win[i].name, name, RAMON_NAME_LEN))
			return &rd->win[i];
	for (i = 0; i < rd->n_win && len < RAMON_NAME_LEN; i++)
		if (!strncmp(rd->win[i].name, name, len) && rd->win[i].name[len] == '@')
			return &rd->win[i];
	return NULL;
}

static int ramon_win_by_index(struct ramon_dev *rd, u32 index, struct ramon_regwin **winp,
			      struct ramon_status *st)
{
	if (index >= rd->n_win)
		return ramon_fail(st, RAMON_E_REGWIN_BAD_INDEX, index, rd->n_win,
				  "regwin index %u: only %u windows", index, rd->n_win);
	*winp = &rd->win[array_index_nospec(index, rd->n_win)];
	return 0;
}

int ramon_ioc_regwin_info(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_regwin_info *p = arg;
	struct ramon_regwin *win;
	int ret;

	ret = ramon_win_by_index(rf->rd, p->index, &win, st);
	if (ret)
		return ret;
	p->size = win->size;
	p->phys = win->phys;
	p->flags = win->flags;
	memcpy(p->name, win->name, sizeof(p->name));
	return 0;
}

/* SPFI windows share their instance's spinlock with its IRQ handler */
static void ramon_reg_access(struct ramon_regwin *win, struct ramon_reg_io *p)
{
	unsigned long flags = 0;

	if (win->lock)
		spin_lock_irqsave(win->lock, flags);
	if (p->op == RAMON_REG_OP_READ)
		p->value = readl(win->base + p->offset);
	else
		writel(p->value, win->base + p->offset);
	if (win->lock)
		spin_unlock_irqrestore(win->lock, flags);
}

int ramon_ioc_reg_io(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_reg_io *p = arg;
	struct ramon_dev *rd = rf->rd;
	struct ramon_regwin *win;
	int ret;

	if (p->name[0]) {
		win = ramon_win_by_name(rd, p->name);
		if (!win)
			return ramon_fail(st, RAMON_E_REGWIN_BAD_NAME, 0, rd->n_win,
					  "regwin \"%.*s\": no such window",
					  RAMON_NAME_LEN, p->name);
	} else {
		ret = ramon_win_by_index(rd, p->index, &win, st);
		if (ret)
			return ret;
	}

	if (win->flags & RAMON_REGWIN_ABSENT)
		return ramon_fail(st, RAMON_E_NOT_PRESENT, win - rd->win, 0,
				  "regwin %s: not present on this board", win->name);
	if (p->op != RAMON_REG_OP_READ && p->op != RAMON_REG_OP_WRITE)
		return ramon_fail(st, RAMON_E_INVAL_ARG, p->op, 0,
				  "regwin %s: op %u is neither read (%u) nor write (%u)",
				  win->name, p->op, RAMON_REG_OP_READ, RAMON_REG_OP_WRITE);
	if (p->offset % sizeof(u32))
		return ramon_fail(st, RAMON_E_REGWIN_UNALIGNED, p->offset, win->size,
				  "regwin %s: offset 0x%x not 4-aligned", win->name, p->offset);
	if (win->size < sizeof(u32) || p->offset > win->size - sizeof(u32))
		return ramon_fail(st, RAMON_E_REGWIN_OFFSET_RANGE, p->offset, win->size,
				  "regwin %s: offset 0x%x + 4 > size 0x%x",
				  win->name, p->offset, win->size);

	if (p->op == RAMON_REG_OP_WRITE && (win->flags & RAMON_REGWIN_RO))
		return ramon_fail(st, RAMON_E_REGWIN_READ_ONLY, p->offset, p->value,
				  "regwin %s: read-only, write of 0x%x at 0x%x refused",
				  win->name, p->value, p->offset);
	ramon_reg_access(win, p);
	return 0;
}

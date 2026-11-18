// SPDX-License-Identifier: GPL-2.0
/*
 * Device-tree discovery. The device tree is the old drivers' one, unchanged:
 * the AXI channels come from our own node's "dmas"/"dma-names"; everything
 * else is found by global compatible searches, exactly as axidmasgk did.
 * Every function logs why something is missing.
 */
#define pr_fmt(fmt) "ramon_dma: " fmt

#include <linux/device.h>
#include <linux/minmax.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/string.h>

#include "ramon_dma.h"

/* ---- AXI DMA channels (own node) ---- */

int ramon_of_axi_count(struct device *dev)
{
	int n = of_count_phandle_with_args(dev->of_node, "dmas", "#dma-cells");

	if (n <= 0)
		dev_err(dev, "%pOF: no usable \"dmas\" property (%d)\n", dev->of_node, n);
	return n;
}

/* child channel node of @dma_node for DT channel argument @channel; referenced */
static struct device_node *ramon_of_get_axi_child(struct device_node *dma_node, u32 channel)
{
	struct device_node *child, *second;

	child = of_get_next_child(dma_node, NULL);
	if (!child || !(channel & 1))
		return child;
	/* of_get_next_child() drops the reference on its "prev" argument */
	of_node_get(child);
	second = of_get_next_child(dma_node, child);
	if (!second)
		return child;
	of_node_put(child);
	return second;
}

static int ramon_of_parse_axi_child(struct device *dev, u32 chan, struct device_node *child,
				    struct ramon_of_axi_chan *out)
{
	if (of_property_read_u32(child, "xlnx,device-id", &out->device_id)) {
		dev_err(dev, "axi ch%u: %pOF has no \"xlnx,device-id\"\n", chan, child);
		return -EINVAL;
	}
	if (of_device_is_compatible(child, RAMON_DT_COMPAT_AXI_MM2S)) {
		out->dir = DMA_MEM_TO_DEV;
	} else if (of_device_is_compatible(child, RAMON_DT_COMPAT_AXI_S2MM)) {
		out->dir = DMA_DEV_TO_MEM;
	} else {
		dev_err(dev, "axi ch%u: %pOF is neither %s nor %s\n", chan, child,
			RAMON_DT_COMPAT_AXI_MM2S, RAMON_DT_COMPAT_AXI_S2MM);
		return -EINVAL;
	}
	return 0;
}

/*
 * Parses "dmas" entry @chan. On success out->ip_node holds a reference on the
 * axi_dma node, which the caller must put.
 */
int ramon_of_parse_axi(struct device *dev, u32 chan, struct ramon_of_axi_chan *out)
{
	struct device_node *np = dev->of_node, *child;
	struct of_phandle_args args;
	struct resource res;
	int ret, children;

	ret = of_property_read_string_index(np, "dma-names", chan, &out->name);
	if (ret) {
		dev_err(dev, "axi ch%u: %pOF has no dma-names[%u] (%d)\n", chan, np, chan, ret);
		return ret;
	}
	ret = of_parse_phandle_with_args(np, "dmas", "#dma-cells", chan, &args);
	if (ret) {
		dev_err(dev, "axi ch%u (%s): dmas[%u] does not resolve (%d)\n", chan, out->name,
			chan, ret);
		return ret;
	}
	if (args.args_count < 1 || args.args[0] > 1) {
		dev_err(dev, "axi ch%u (%s): dmas[%u] channel argument must be 0 or 1\n",
			chan, out->name, chan);
		ret = -EINVAL;
		goto err_put;
	}
	children = of_get_child_count(args.np);
	if (children < 1 || children > RAMON_AXI_DT_MAX_CHILDREN) {
		dev_err(dev, "axi ch%u (%s): %pOF has %d channel nodes, need 1..%u\n",
			chan, out->name, args.np, children, RAMON_AXI_DT_MAX_CHILDREN);
		ret = -EINVAL;
		goto err_put;
	}
	out->phys = of_address_to_resource(args.np, 0, &res) ? 0 : res.start;

	child = ramon_of_get_axi_child(args.np, args.args[0]);
	ret = ramon_of_parse_axi_child(dev, chan, child, out);
	of_node_put(child);
	if (ret)
		goto err_put;
	out->ip_node = args.np;
	return 0;

err_put:
	of_node_put(args.np);
	return ret;
}

/* ---- blocks found by global compatible search ---- */

/* RS-TOP: first "xlnx,nanrec-rs-top-1.0" node, reg 0 */
static void ramon_of_find_rstop(struct device *dev, struct ramon_of_hw *hw)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, RAMON_DT_COMPAT_RSTOP);
	if (!np) {
		dev_info(dev, "rs_top: no %s node; RS-TOP absent\n", RAMON_DT_COMPAT_RSTOP);
		return;
	}
	ret = of_address_to_resource(np, 0, &hw->rstop);
	if (ret)
		dev_warn(dev, "rs_top: %pOF has no usable reg entry (%d); RS-TOP absent\n",
			 np, ret);
	else
		hw->rstop_present = true;
	of_node_put(np);
}

static void ramon_of_parse_spw_node(struct device *dev, struct device_node *np, u32 nn,
				    struct ramon_of_hw *hw)
{
	int ret = of_address_to_resource(np, 0, &hw->spw[nn]);

	if (ret) {
		dev_warn(dev, "spw%u: %pOF has no usable reg entry (%d); absent\n", nn, np, ret);
		return;
	}
	hw->spw_mask |= BIT(nn);
	hw->spw_irq[nn] = irq_of_parse_and_map(np, 0);
	if (!hw->spw_irq[nn])
		dev_warn(dev, "spw%u: %pOF has no usable interrupt; SPW_WAIT_RX unavailable\n",
			 nn, np);
}

/* SPW: "xlnx,nanrec-spw-wr-1.0" nodes in tree order, index 0 = NN A */
static void ramon_of_find_spw(struct device *dev, struct ramon_of_hw *hw)
{
	struct device_node *np;
	u32 nn = 0;

	for_each_compatible_node(np, NULL, RAMON_DT_COMPAT_SPW) {
		if (nn >= RAMON_NN_COUNT) {
			dev_warn(dev, "spw: ignoring extra node %pOF (only %u NNs)\n",
				 np, RAMON_NN_COUNT);
			continue;
		}
		ramon_of_parse_spw_node(dev, np, nn, hw);
		nn++;
	}
	for (; nn < RAMON_NN_COUNT; nn++)
		dev_info(dev, "spw%u: no %s node #%u in the device tree; absent\n",
			 nn, RAMON_DT_COMPAT_SPW, nn);
}

/* a DT label ("vH_RS_...") -> its node's reg 0, through /__symbols__ */
static int ramon_of_resolve_label(struct device *dev, const char *label, struct resource *res)
{
	struct device_node *sym, *np;
	const char *path;
	int ret;

	sym = of_find_node_by_path("/__symbols__");
	if (!sym) {
		dev_warn(dev, "spfi: no /__symbols__ (DT built without -@); %s unresolved\n",
			 label);
		return -ENODEV;
	}
	ret = of_property_read_string(sym, label, &path);
	np = ret ? NULL : of_find_node_by_path(path);
	of_node_put(sym);
	if (!np) {
		dev_warn(dev, "spfi: label %s %s\n", label,
			 ret ? "is not in /__symbols__" : "points to a missing node");
		return -ENODEV;
	}
	ret = of_address_to_resource(np, 0, res);
	if (ret)
		dev_warn(dev, "spfi: %s (%pOF) has no usable reg entry (%d)\n", label, np, ret);
	of_node_put(np);
	return ret;
}

static void ramon_of_find_spfi_mems(struct device *dev, struct ramon_of_hw *hw)
{
	static const char * const rx_mem[RAMON_NN_COUNT] = {
		RAMON_DT_SPFI_RX_MEM_NNA, RAMON_DT_SPFI_RX_MEM_NNB,
	};
	static const char * const tx_offs[RAMON_NN_COUNT] = {
		RAMON_DT_SPFI_TX_OFFS_NNA, RAMON_DT_SPFI_TX_OFFS_NNB,
	};
	u32 nn;

	/* only the NNs the SPFI node describes; the others have no labels to look up */
	for (nn = 0; nn < RAMON_NN_COUNT; nn++) {
		if (!(hw->spfi_reg_mask & BIT(nn)))
			continue;
		hw->spfi_rx_mem_present[nn] = !ramon_of_resolve_label(dev, rx_mem[nn],
								      &hw->spfi_rx_mem[nn]);
		hw->spfi_tx_offs_present[nn] = !ramon_of_resolve_label(dev, tx_offs[nn],
								       &hw->spfi_tx_offs[nn]);
	}
}

/*
 * SPFI: the first "xlnx,nanrec-spfi-wr-1.0" node; NN nn is reg[nn] and
 * interrupts[nn]. A node with one reg entry describes NN0 only.
 */
static void ramon_of_find_spfi(struct device *dev, struct ramon_of_hw *hw)
{
	struct device_node *np;
	u32 i;

	np = of_find_compatible_node(NULL, NULL, RAMON_DT_COMPAT_SPFI);
	if (!np) {
		dev_info(dev, "spfi: no %s node; SPFI absent\n", RAMON_DT_COMPAT_SPFI);
		return;
	}
	for (i = 0; i < RAMON_NN_COUNT; i++) {
		if (of_address_to_resource(np, i, &hw->spfi[i]))
			break;
		hw->spfi_reg_mask |= BIT(i);
		hw->spfi_irq[i] = irq_of_parse_and_map(np, i);
		if (hw->spfi_irq[i])
			hw->spfi_irq_mask |= BIT(i);
		else
			dev_warn(dev, "spfi: node %pOF has no interrupt %u; SPFI%u commands disabled\n",
				 np, i, i);
	}
	if (!hw->spfi_reg_mask)
		dev_warn(dev, "spfi: node %pOF has no reg entry; SPFI disabled\n", np);
	else if (i < RAMON_NN_COUNT)
		dev_info(dev, "spfi: node %pOF has %u reg entr%s; SPFI%u and above absent\n", np, i,
			 i == 1 ? "y" : "ies", i);
	of_node_put(np);
	ramon_of_find_spfi_mems(dev, hw);
}

static void ramon_of_parse_regdev(struct device *dev, struct device_node *np, u32 i,
				  struct ramon_of_regdev *out)
{
	struct device_node *target;
	const char *name;
	int ret;

	if (of_property_read_string_index(np, "reg-device-names", i, &name))
		snprintf(out->name, sizeof(out->name), RAMON_WIN_NAME_REGACCESS "%u", i);
	else if (strscpy(out->name, name, sizeof(out->name)) < 0)
		dev_warn(dev, "reg-access: name \"%s\" cut to \"%s\"\n", name, out->name);

	target = of_parse_phandle(np, "reg-devices", i);
	if (!target) {
		dev_warn(dev, "reg-access: %pOF reg-devices[%u] does not resolve; %s absent\n",
			 np, i, out->name);
		return;
	}
	ret = of_address_to_resource(target, 0, &out->res);
	if (ret)
		dev_warn(dev, "reg-access: %pOF has no usable reg entry (%d); %s absent\n",
			 target, ret, out->name);
	else
		out->present = true;
	of_node_put(target);
}

/* reg-access: "reg-devices" phandles of the first "reg-access" node */
static void ramon_of_find_regaccess(struct device *dev, struct ramon_of_hw *hw)
{
	struct device_node *np;
	int count;
	u32 i;

	np = of_find_compatible_node(NULL, NULL, RAMON_DT_COMPAT_REGACCESS);
	if (!np) {
		dev_info(dev, "reg-access: no %s node; no extra register windows\n",
			 RAMON_DT_COMPAT_REGACCESS);
		return;
	}
	count = of_count_phandle_with_args(np, "reg-devices", NULL);
	if (count < 0) {
		dev_warn(dev, "reg-access: %pOF has no usable reg-devices (%d)\n", np, count);
	} else {
		hw->n_regaccess = min_t(u32, count, RAMON_REGACCESS_MAX);
		if ((u32)count > RAMON_REGACCESS_MAX)
			dev_warn(dev, "reg-access: %pOF lists %d reg-devices, using the first %u\n",
				 np, count, RAMON_REGACCESS_MAX);
	}
	for (i = 0; i < hw->n_regaccess; i++)
		ramon_of_parse_regdev(dev, np, i, &hw->regaccess[i]);
	of_node_put(np);
}

/* Fills @hw (zeroed by the caller) with every block found by global search. */
void ramon_of_discover(struct device *dev, struct ramon_of_hw *hw)
{
	ramon_of_find_rstop(dev, hw);
	ramon_of_find_spw(dev, hw);
	ramon_of_find_spfi(dev, hw);
	ramon_of_find_regaccess(dev, hw);
}

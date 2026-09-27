/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hardware facts and kernel-side limits of the "ramon" board.
 *
 * Every number the driver uses is named here. Values userspace also needs
 * (ABI limits, SPFI opcodes) are in ramon_dma_uapi.h, which this file
 * includes. Sources: the old axidmasgk/ps2psk drivers and the FPGA register
 * maps (see README.md, appendix "register maps").
 */
#ifndef RAMON_BOARD_H
#define RAMON_BOARD_H

#include <linux/bits.h>

#include "ramon_dma_uapi.h"

/* ---- device tree (unchanged from the old drivers) ---- */
#define RAMON_DT_COMPAT_CORE		"xlnx,nanrec-axidmasg-1.0"
#define RAMON_DT_COMPAT_SPW		"xlnx,nanrec-spw-wr-1.0"
#define RAMON_DT_COMPAT_SPFI		"xlnx,nanrec-spfi-wr-1.0"
#define RAMON_DT_COMPAT_RSTOP		"xlnx,nanrec-rs-top-1.0"
#define RAMON_DT_COMPAT_REGACCESS	"reg-access"
#define RAMON_DT_COMPAT_AXI_MM2S	"xlnx,axi-dma-mm2s-channel"
#define RAMON_DT_COMPAT_AXI_S2MM	"xlnx,axi-dma-s2mm-channel"
#define RAMON_DT_SPFI_RX_MEM_NNA	"vH_RS_nanrec_spfi_top_spfi_rx_msg_mem_ctrl_nna"
#define RAMON_DT_SPFI_RX_MEM_NNB	"vH_RS_nanrec_spfi_top_spfi_rx_msg_mem_ctrl_nnb"
#define RAMON_DT_SPFI_TX_OFFS_NNA	"vH_RS_nanrec_spfi_top_spfi_tx_rd_offset_ctrl_nna"
#define RAMON_DT_SPFI_TX_OFFS_NNB	"vH_RS_nanrec_spfi_top_spfi_tx_rd_offset_ctrl_nnb"
#define RAMON_AXI_DT_MAX_CHILDREN	2	/* mm2s and/or s2mm child per axi_dma */
#define RAMON_REGACCESS_MAX		8	/* entries taken from "reg-devices" */

/* ---- PS register windows (owned by in-tree drivers; mapped read-only) ---- */
#define RAMON_SYSMON_PS_BASE		0xFFA50800
#define RAMON_SYSMON_PL_BASE		0xFFA50C00
#define RAMON_SYSMON_SIZE		0x300
#define RAMON_RTC_BASE			0xFFA60000
#define RAMON_RTC_SIZE			0x5C
#define RAMON_TTC_BASE			0xFF110000
#define RAMON_TTC_STRIDE		0x10000
#define RAMON_TTC_SIZE			0x100

/* ---- register window names (ABI: see enum ramon_win_index) ---- */
#define RAMON_WIN_NAME_RS_TOP		"rs_top"
#define RAMON_WIN_NAME_SYSMON_PS	"sysmon_ps"
#define RAMON_WIN_NAME_SYSMON_PL	"sysmon_pl"
#define RAMON_WIN_NAME_RTC		"rtc"
#define RAMON_WIN_NAME_TTC		"ttc"		/* + index */
#define RAMON_WIN_NAME_SPW		"spw"		/* + nn + "@" + unit address */
#define RAMON_WIN_NAME_SPFI		"spfi"		/* + nn + "@" + unit address */
#define RAMON_WIN_NAME_REGACCESS	"regaccess"	/* + index, without reg-device-names */

/* ---- RS-TOP (byte offsets) ---- */
#define RAMON_RSTOP_TRST_SPI_EN		0x40	/* probe writes 1: enable TRST and SPI_EN */
#define RAMON_RSTOP_TRST_SPI_EN_ON	1
#define RAMON_RSTOP_MIN_SIZE		0x44	/* must cover RAMON_RSTOP_TRST_SPI_EN */

/* ---- SPW (byte offsets, one window per NN) ---- */
#define RAMON_SPW_WIN_SIZE		0x1000
#define RAMON_SPW_RX_PKT_SIZE		0x0C	/* read in the IRQ after the ack */
#define RAMON_SPW_LOOPBACK_ENABLE	0x10
#define RAMON_SPW_RX_IRQ_ACK		0x14	/* PL2PS_RX_INTERR, write 0 to ack */
#define RAMON_SPW_RESET			0x1C	/* FPGA_SPW_RESET, write 1 */
#define RAMON_SPW_RX_IRQ_ACK_VAL	0
#define RAMON_SPW_RESET_VAL		1
#define RAMON_SPW_RX_FIFO_DEPTH		64	/* power of two (kfifo) */

/* ---- SPFI (32-bit word indices, one 0x2000 window per NN) ---- */
#define RAMON_SPFI_WIN_SIZE		0x2000
#define RAMON_SPFI_REG(word)		((word) * sizeof(u32))

enum ramon_spfi_word {
	RAMON_SPFI_TX_COMMAND		= 0,	/* written last, triggers the command */
	RAMON_SPFI_TX_STREAM_ID		= 1,
	RAMON_SPFI_TX_OFFSET		= 2,
	RAMON_SPFI_TX_STREAM_TYPE	= 3,
	RAMON_SPFI_TX_STREAM_LAST_OFFSET = 4,
	RAMON_SPFI_TX_INIT_TYPE		= 5,
	RAMON_SPFI_TX_TOD		= 6,
	RAMON_SPFI_TX_SRC_LOGICAL_ADDR	= 7,	/* written first, before every command */
	RAMON_SPFI_TX_NUM_OFFSETS	= 8,
	RAMON_SPFI_TX_RD_BURST_ADDR_LO	= 9,
	RAMON_SPFI_TX_RD_BURST_ADDR_HI	= 10,
	RAMON_SPFI_RX_COMMAND		= 19,
	RAMON_SPFI_RX_OFFSET		= 20,
	RAMON_SPFI_RX_ERR_CODE		= 21,
	RAMON_SPFI_RX_CURR_TOD		= 25,
	RAMON_SPFI_RX_STR_ID		= 26,
	RAMON_SPFI_RX_INIT_INFO		= 27,
	RAMON_SPFI_RX_ALERT_CODE	= 29,
	RAMON_SPFI_RX_ALERT_SUB_CODE	= 30,
	RAMON_SPFI_RX_ALERT_PARAM1	= 31,
	RAMON_SPFI_RX_ALERT_PARAM2	= 32,
	RAMON_SPFI_RX_STATUS		= 33,	/* checksum status */
	RAMON_SPFI_IRQ_VECTOR_SRC	= 61,	/* write the vector back to clear */
	RAMON_SPFI_IRQ_VECTOR_INT	= 63,	/* pending vector, read first */
};

#define RAMON_SPFI_SRC_LOGICAL_ADDR_VAL	1
#define RAMON_SPFI_IRQ_VC0TX		BIT(0)	/* command accepted: ignored */
#define RAMON_SPFI_IRQ_VC1TX		BIT(1)	/* data-write / close / flush ack */
#define RAMON_SPFI_IRQ_VC0RX		BIT(2)	/* command completion or alert */
#define RAMON_SPFI_IRQ_VC1RX		BIT(3)	/* read data (0x98) or 0xD0 completion */
#define RAMON_SPFI_READ_SETUP_US	10	/* delay before READ_STREAM (old driver) */
#define RAMON_SPFI_ALERT_FIFO_DEPTH	16	/* power of two (kfifo) */

/* ---- default timeouts in ms (0 in an ioctl selects these) ---- */
#define RAMON_TMO_AXI_MS		2000
#define RAMON_TMO_ZDMA_MS		3000
#define RAMON_TMO_SPFI_CMD_MS		3000
#define RAMON_TMO_SPFI_CMD_LONG_MS	10000	/* INIT, GRACEFUL_POWER_DOWN */
#define RAMON_TMO_SPW_MS		1000

/* ---- ZDMA ---- */
#define RAMON_ZDMA_CHUNK		16	/* zynqmp_dma has 32 descriptors per channel */
#define RAMON_ZDMA_CHANNELS_DEFAULT	4
#define RAMON_ZDMA_CHANNELS_MAX		16

/* ---- buffers ---- */
#define RAMON_MAX_BUF_MB_DEFAULT	256
#define RAMON_MAX_BUF_MB_LIMIT		4096	/* sanity bound for the module parameter */
#define RAMON_BUF_HANDLE_MIN		1	/* 0 is never a handle */
#define RAMON_BUF_HANDLE_MAX		0x7FFFF	/* handle << 12 fits a 32-bit signed off_t */

#endif /* RAMON_BOARD_H */

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * The userspace ABI of /dev/ramon_dma.
 *
 * This file is shared verbatim by the kernel module and by userspace (installed
 * to ${includedir}/ramon/). Anything userspace needs lives here; kernel-only
 * constants live in ramon_board.h.
 *
 * Conventions:
 * - Every ioctl is _IOWR(RAMON_IOC_MAGIC, nr, struct) and every struct ends
 *   with a struct ramon_status trailer. The driver copies the whole struct back
 *   even when the ioctl fails, so the trailer is always readable after an
 *   ioctl that returned 0 or -1 with errno != ENOTTY/EFAULT.
 * - Only fixed-width types. Sizes are multiples of 8 and the layout is the
 *   same for 32-bit and 64-bit userspace. User pointers travel as __u64.
 * - Buffers are opaque per-fd handles returned by RAMON_IOC_BUF_ALLOC. Every
 *   DMA operand is (handle, offset, len); physical addresses are never input.
 * - timeout_ms == 0 selects the driver default (see each ioctl); values above
 *   RAMON_TIMEOUT_MAX_MS are clamped, except for SPFI_WAIT_ALERT where 0 means
 *   "wait forever".
 */
#ifndef RAMON_DMA_UAPI_H
#define RAMON_DMA_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define RAMON_ABI_VERSION		1
#define RAMON_DEV_NAME			"ramon_dma"	/* /dev/ramon_dma */
#define RAMON_IOC_MAGIC			'R'

/* ---- limits userspace must respect ---- */
#define RAMON_NN_COUNT			2
#define RAMON_NAME_LEN			32
#define RAMON_MSG_LEN			96
#define RAMON_AXI_MAX_ITEMS		8192	/* = SPFI offsets per TX offset table */
#define RAMON_ZDMA_MAX_COPIES		4096
#define RAMON_SPFI_OFFSET_BYTES		16384	/* one SPFI "offset" unit */
#define RAMON_SPFI_MEM_READ_MAX		16384	/* RX message table size */
#define RAMON_SPFI_TX_OFFS_MAX		32768	/* TX read-offset table size */
#define RAMON_SPFI_IRQ_VECTORS		16	/* histogram buckets, vec & 0xf */
#define RAMON_TTC_COUNT			4
#define RAMON_TIMEOUT_MAX_MS		60000

/* ---- SPFI TX opcodes (RAMON_IOC_SPFI_CMD.opcode) ---- */
#define RAMON_SPFI_OP_DATA_WRITE		0x10	/* only via SPFI_WRITE */
#define RAMON_SPFI_OP_FLUSH_STREAM		0x13	/* "FLASH_STREAM" in the old code */
#define RAMON_SPFI_OP_READ_STREAM		0x18	/* only via SPFI_READ */
#define RAMON_SPFI_OP_OPEN_STREAM_FOR_WRITE	0x30
#define RAMON_SPFI_OP_CLOSE_STREAM_FOR_WRITE	0x40
#define RAMON_SPFI_OP_DELETE_STREAM		0x41
#define RAMON_SPFI_OP_GET_ALL_STREAM_STATUS	0x50
#define RAMON_SPFI_OP_PLATFORM_RESET		0x58
#define RAMON_SPFI_OP_INIT			0x59
#define RAMON_SPFI_OP_GRACEFUL_POWER_DOWN	0x5A
#define RAMON_SPFI_OP_SET_TOD			0x5B
#define RAMON_SPFI_OP_FORMAT			0x5C

/* ---- SPFI RX opcodes (rx_opcode) ---- */
#define RAMON_SPFI_RX_FLUSH_EXECUTED		0x93
#define RAMON_SPFI_RX_DATA_FROM_STREAM		0x98
#define RAMON_SPFI_RX_RCV_READY			0xB0
#define RAMON_SPFI_RX_STREAM_WAS_CLOSED		0xC0
#define RAMON_SPFI_RX_STREAM_WAS_DELETED	0xC1
#define RAMON_SPFI_RX_ALL_STREAM_STATUS		0xD0
#define RAMON_SPFI_RX_PLATFORM_RESET_ACK	0xD8
#define RAMON_SPFI_RX_INIT_COMPLETED		0xD9
#define RAMON_SPFI_RX_READY_TO_POWER_OFF	0xDA
#define RAMON_SPFI_RX_TOD_WAS_SET		0xDB
#define RAMON_SPFI_RX_FORMAT_COMPLETED		0xDC
#define RAMON_SPFI_RX_ALERT			0xFE

/*
 * Error codes: X(name, code, errno, description).
 * RAMON_E_<name> == code; the ioctl returns -errno. Namespaces:
 * generic 1-31, buf 32-63, axi 64-95, zdma 96-127, spw 128-159,
 * spfi 160-191, regwin 192-223. Codes are never renumbered; 9 and 10 are
 * retired (generic CANCELLED / TIMEOUT: every wait has its own code).
 */
#define RAMON_ERR_LIST(X) \
	X(OK,			  0, 0,		"success") \
	X(INVAL_ARG,		  1, EINVAL,	"invalid argument") \
	X(NO_SUCH_HANDLE,	  2, ENOENT,	"no such buffer handle on this fd") \
	X(BUF_RANGE,		  3, ERANGE,	"offset/len outside the buffer") \
	X(BAD_COUNT,		  4, EINVAL,	"item count out of range") \
	X(COPY_FAULT,		  5, EFAULT,	"bad user pointer") \
	X(NOT_PRESENT,		  6, ENXIO,	"sub-block absent on this board") \
	X(REMOVED,		  7, ENODEV,	"device was removed") \
	X(INTERRUPTED,		  8, EINTR,	"interrupted by a signal") \
	X(NO_MEMORY,		 11, ENOMEM,	"kernel memory allocation failed") \
	X(NOT_ALIGNED,		 12, EINVAL,	"value not aligned as required") \
	X(BUF_TOO_LARGE,	 32, EINVAL,	"buffer size above max_buf_mb") \
	X(BUF_ALLOC_FAILED,	 33, ENOMEM,	"coherent allocation failed (CMA exhausted?)") \
	X(BUF_MMAP_LEN,		 34, EINVAL,	"mmap length exceeds the buffer") \
	X(AXI_BAD_CHAN,		 64, ENOENT,	"no such AXI channel") \
	X(AXI_PREP_FAILED,	 65, ENOSPC,	"AXI descriptor prep failed (pool exhausted?)") \
	X(AXI_SUBMIT_FAILED,	 66, EIO,	"AXI descriptor submit failed") \
	X(AXI_TIMEOUT,		 67, ETIMEDOUT,	"AXI transfer timed out") \
	X(AXI_DMA_ERROR,	 68, EIO,	"AXI transfer ended in error") \
	X(ZDMA_NO_CHANNELS,	 96, ENXIO,	"no ZDMA memcpy channels acquired") \
	X(ZDMA_PREP_FAILED,	 97, ENOSPC,	"ZDMA descriptor prep failed") \
	X(ZDMA_SUBMIT_FAILED,	 98, EIO,	"ZDMA descriptor submit failed") \
	X(ZDMA_TIMEOUT,		 99, ETIMEDOUT,	"ZDMA copy timed out") \
	X(ZDMA_DMA_ERROR,	100, EIO,	"ZDMA copy ended in error") \
	X(SPW_BAD_NN,		128, EINVAL,	"no such SPW instance") \
	X(SPW_TIMEOUT,		129, ETIMEDOUT,	"no SPW packet before the timeout") \
	X(SPW_CANCELLED,	130, ECANCELED,	"SPW wait cancelled") \
	X(SPFI_BAD_NN,		160, EINVAL,	"no such SPFI instance") \
	X(SPFI_BAD_OPCODE,	161, EINVAL,	"opcode not allowed in SPFI_CMD") \
	X(SPFI_CMD_TIMEOUT,	162, ETIMEDOUT,	"SPFI command not acknowledged in time") \
	X(SPFI_UNEXPECTED_OPCODE, 163, EPROTO,	"SPFI answered with another rx opcode") \
	X(SPFI_WRITE_TIMEOUT,	164, ETIMEDOUT,	"SPFI data-write not acknowledged in time") \
	X(SPFI_READ_TIMEOUT,	165, ETIMEDOUT,	"SPFI read data not received in time") \
	X(SPFI_ALERT_TIMEOUT,	166, ETIMEDOUT,	"no SPFI alert before the timeout") \
	X(SPFI_ALERT_CANCELLED,	167, ECANCELED,	"SPFI alert wait cancelled") \
	X(SPFI_MEM_RANGE,	168, ERANGE,	"outside the SPFI RX message table") \
	X(SPFI_OFFS_RANGE,	169, ERANGE,	"outside the SPFI TX offset table") \
	X(SPFI_DST_TOO_SMALL,	170, ERANGE,	"destination buffer too small for the read") \
	X(REGWIN_BAD_INDEX,	192, ENOENT,	"no such register window index") \
	X(REGWIN_BAD_NAME,	193, ENOENT,	"no such register window name") \
	X(REGWIN_OFFSET_RANGE,	194, ERANGE,	"register offset outside the window") \
	X(REGWIN_UNALIGNED,	195, EINVAL,	"register offset not 4-aligned") \
	X(REGWIN_READ_ONLY,	196, EROFS,	"register window is read-only")

#define RAMON_ERR_ENUM(name, code, err, desc)	RAMON_E_##name = code,
enum ramon_err {
	RAMON_ERR_LIST(RAMON_ERR_ENUM)
};

/*
 * Trailer of every ioctl struct. On success code == RAMON_E_OK and err == 0;
 * msg may still carry a note (e.g. an SPFI rx_err_code). On failure it names
 * the offending values, e.g. "buf 3: offset 0x10000 + len 0x8000 > size 0x10000".
 */
struct ramon_status {
	__u32 code;			/* RAMON_E_* */
	__s32 err;			/* negative errno returned by the ioctl, or 0 */
	__u64 arg[2];			/* the two most relevant values */
	char  msg[RAMON_MSG_LEN];	/* NUL-terminated */
};

/* 1: driver and board summary */
struct ramon_get_info {
	__u32 abi_version;		/* RAMON_ABI_VERSION of the driver */
	__u32 drv_major;
	__u32 drv_minor;
	__u32 drv_patch;
	__u32 n_axi_chan;
	__u32 n_zdma_chan;
	__u32 n_regwin;
	__u32 spw_mask;			/* bit i: SPW instance i present */
	__u32 spfi_mask;		/* bit i: SPFI instance i present */
	__u32 page_size;
	__u64 max_buf_bytes;		/* largest BUF_ALLOC */
	struct ramon_status st;
};

enum ramon_chan_type {
	RAMON_CHAN_AXI		= 0,
	RAMON_CHAN_ZDMA		= 1,
};

enum ramon_chan_dir {
	RAMON_DIR_MEM_TO_DEV	= 0,
	RAMON_DIR_DEV_TO_MEM	= 1,
	RAMON_DIR_MEMCPY	= 2,
};

/* 2: channel index -> description. AXI channels first, then ZDMA. */
struct ramon_chan_info {
	__u32 index;			/* in */
	__u32 type;			/* out: enum ramon_chan_type */
	__u32 dir;			/* out: enum ramon_chan_dir */
	__u32 device_id;		/* out: DT "xlnx,device-id" (AXI) */
	__u64 phys;			/* out: DMA IP register base (AXI) */
	char  name[RAMON_NAME_LEN];	/* out: DT dma-names entry / dma_chan_name */
	struct ramon_status st;
};

/* 3: allocate a zeroed, page-aligned coherent buffer owned by this fd */
struct ramon_buf_alloc {
	__u64 size;			/* in: bytes, 1..max_buf_bytes */
	__u64 actual_size;		/* out: PAGE_ALIGN(size) */
	__u32 handle;			/* out: mmap offset is handle << PAGE_SHIFT */
	__u32 pad;
	struct ramon_status st;
};

/* 4: drop the handle; memory is freed once no transfer or mapping uses it */
struct ramon_buf_free {
	__u32 handle;
	__u32 pad;
	struct ramon_status st;
};

/* 5: debug only; dma_addr is never accepted as input anywhere */
struct ramon_buf_info {
	__u32 handle;			/* in */
	__u32 pad;
	__u64 size;			/* out */
	__u64 dma_addr;			/* out */
	struct ramon_status st;
};

struct ramon_sg_item {
	__u32 handle;
	__u32 pad;
	__u64 offset;
	__u64 len;
};

/* 6: synchronous scatter-gather transfer on one AXI DMA channel */
struct ramon_axi_xfer {
	__u32 chan;			/* in: AXI channel index (DT dmas order) */
	__u32 timeout_ms;		/* in: 0 = 2000 */
	__u32 n_items;			/* in: 1..RAMON_AXI_MAX_ITEMS */
	__u32 pad;
	__u64 items;			/* in: user pointer to struct ramon_sg_item[] */
	__u64 bytes;			/* out: bytes transferred */
	struct ramon_status st;
};

struct ramon_copy {
	__u32 src_handle;
	__u32 dst_handle;
	__u64 src_off;
	__u64 dst_off;
	__u64 len;
};

/* 7: ZDMA memcpy of one entry (n == 1) or a list, in chunks */
struct ramon_zdma_copy {
	__u32 timeout_ms;		/* in: per chunk, 0 = 3000 */
	__u32 n;			/* in: 1..RAMON_ZDMA_MAX_COPIES */
	__u64 entries;			/* in: user pointer to struct ramon_copy[] */
	__u32 done;			/* out: entries completed */
	__u32 pad;
	struct ramon_status st;
};

/* 8: wait for one received SPW packet on NN nn and return its size */
struct ramon_spw_wait_rx {
	__u32 nn;			/* in */
	__u32 timeout_ms;		/* in: 0 = 1000 */
	__u32 size;			/* out: RX_PKT_SIZE latched by the IRQ */
	__u32 pad;
	struct ramon_status st;
};

/* 9: release every SPW_WAIT_RX blocked on NN nn with RAMON_E_SPW_CANCELLED */
struct ramon_spw_cancel {
	__u32 nn;
	__u32 pad;
	struct ramon_status st;
};

/* 10: set SPW loopback on NN nn (resets the SPW link if it changes) */
struct ramon_spw_loopback {
	__u32 nn;
	__u32 enable;
	struct ramon_status st;
};

/* 11: SPFI short command (every opcode except DATA_WRITE and READ_STREAM) */
struct ramon_spfi_cmd {
	__u32 nn;			/* in */
	__u32 opcode;			/* in: RAMON_SPFI_OP_* */
	__u32 wait_ack;			/* in: 0 = fire and forget */
	__u32 timeout_ms;		/* in: 0 = 3000 (INIT, GRACEFUL_POWER_DOWN: 10000) */
	__u32 stream_id;		/* in */
	__u32 stream_type;		/* in */
	__u32 stream_last_offset;	/* in */
	__u32 tod;			/* in */
	__u32 init_type;		/* in */
	__u32 rx_opcode;		/* out: RAMON_SPFI_RX_* */
	__u32 rx_err_code;		/* out: NN protocol error, not an ioctl error */
	__u32 rx_stream_id;		/* out */
	__u32 rx_offset;		/* out */
	__u32 rx_init_info;		/* out */
	__u32 rx_curr_tod;		/* out */
	__u32 rx_status;		/* out: RX_STATUS (checksum status) */
	struct ramon_status st;
};

/* 12: DATA_WRITE + AXI transfer on chan + wait for the write-done IRQ */
struct ramon_spfi_write {
	__u32 nn;			/* in */
	__u32 chan;			/* in: AXI TX channel feeding this NN */
	__u32 stream_id;		/* in */
	__u32 stream_type;		/* in */
	__u32 stream_last_offset;	/* in */
	__u32 tx_offset;		/* in */
	__u32 tx_num_offset;		/* in: sum(len) == tx_num_offset * 16 KiB */
	__u32 timeout_ms;		/* in: each of DMA and ack, 0 = 3000 */
	__u32 n_items;			/* in: 1..RAMON_AXI_MAX_ITEMS */
	__u32 rx_status;		/* out: RX_STATUS */
	__u64 items;			/* in: user pointer to struct ramon_sg_item[] */
	struct ramon_status st;
};

/* 13: READ_STREAM of n_offsets * 16 KiB into (dst_handle, dst_off) */
struct ramon_spfi_read {
	__u32 nn;			/* in */
	__u32 stream_id;		/* in */
	__u32 n_offsets;		/* in: 1..RAMON_SPFI_TX_OFFS_MAX / 4 */
	__u32 dst_handle;		/* in */
	__u64 offsets;			/* in: user pointer to __u32[n_offsets] */
	__u64 dst_off;			/* in */
	__u32 timeout_ms;		/* in: 0 = 3000 */
	__u32 rx_status;		/* out: RX_STATUS */
	struct ramon_status st;
};

/* 14: pop one alert received on NN nn */
struct ramon_spfi_wait_alert {
	__u32 nn;			/* in */
	__u32 timeout_ms;		/* in: 0 = wait forever */
	__u32 code;			/* out */
	__u32 sub_code;			/* out */
	__u32 param1;			/* out */
	__u32 param2;			/* out */
	__u32 rx_status;		/* out */
	__u32 pad;
	struct ramon_status st;
};

/* 15: release every SPFI_WAIT_ALERT on NN nn with RAMON_E_SPFI_ALERT_CANCELLED */
struct ramon_spfi_cancel_alert {
	__u32 nn;
	__u32 pad;
	struct ramon_status st;
};

/* 16: read bytes from the SPFI RX message table */
struct ramon_spfi_mem_read {
	__u32 nn;
	__u32 offset;			/* bytes, any alignment */
	__u32 size;			/* 1..RAMON_SPFI_MEM_READ_MAX */
	__u32 pad;
	__u64 data;			/* user pointer, size bytes */
	struct ramon_status st;
};

/* 17: write bytes to the SPFI TX read-offset table */
struct ramon_spfi_tx_offs_write {
	__u32 nn;
	__u32 offset;			/* bytes, any alignment */
	__u32 size;			/* 1..RAMON_SPFI_TX_OFFS_MAX */
	__u32 pad;
	__u64 data;			/* user pointer, size bytes */
	struct ramon_status st;
};

#define RAMON_REGWIN_RO		0x1u	/* writes rejected */
#define RAMON_REGWIN_ABSENT	0x2u	/* not on this board; size == 0 */

/*
 * Register window indices, stable across boards: a window missing on a board
 * is still listed with RAMON_REGWIN_ABSENT. Names in brackets; the SPW and
 * SPFI names carry the unit address ("spw0@a0020000"). The "reg-devices" of
 * the DT "reg-access" node follow from RAMON_WIN_FIXED on, named from
 * "reg-device-names" (fallback "regaccess<i>").
 */
enum ramon_win_index {
	RAMON_WIN_RS_TOP	= 0,				/* "rs_top" */
	RAMON_WIN_SYSMON_PS	= 1,				/* "sysmon_ps", RO */
	RAMON_WIN_SYSMON_PL	= 2,				/* "sysmon_pl", RO */
	RAMON_WIN_RTC		= 3,				/* "rtc", RO */
	RAMON_WIN_TTC0		= 4,				/* "ttc0".."ttc3", RO */
	RAMON_WIN_SPW0		= RAMON_WIN_TTC0 + RAMON_TTC_COUNT,	/* "spw0@..", "spw1@.." */
	RAMON_WIN_SPFI0		= RAMON_WIN_SPW0 + RAMON_NN_COUNT,	/* "spfi0@..", "spfi1@.." */
	RAMON_WIN_FIXED		= RAMON_WIN_SPFI0 + RAMON_NN_COUNT,
};

/* 18: register window index -> description */
struct ramon_regwin_info {
	__u32 index;			/* in */
	__u32 size;			/* out: bytes */
	__u64 phys;			/* out */
	__u32 flags;			/* out: RAMON_REGWIN_* */
	__u32 pad;
	char  name[RAMON_NAME_LEN];	/* out, e.g. "rs_top", "spw0@a0020000" */
	struct ramon_status st;
};

#define RAMON_REG_OP_READ	0
#define RAMON_REG_OP_WRITE	1

/*
 * 19: one 32-bit register access in a window picked by name or index.
 * A name matches a window's full name, or its part before '@' ("spw0").
 */
struct ramon_reg_io {
	__u32 index;			/* in: used when name[0] == 0 */
	__u32 op;			/* in: RAMON_REG_OP_* */
	__u32 offset;			/* in: bytes, 4-aligned */
	__u32 value;			/* in (write) / out (read) */
	char  name[RAMON_NAME_LEN];	/* in: window name, or empty */
	struct ramon_status st;
};

/* 20: counters; reset != 0 zeroes them after reading */
struct ramon_get_stats {
	__u32 reset;			/* in */
	__u32 pad;
	__u64 axi_tx;
	__u64 axi_rx;
	__u64 axi_timeouts;
	__u64 zdma_ops;
	__u64 zdma_timeouts;
	__u64 spw_rx[RAMON_NN_COUNT];
	__u64 spw_overrun[RAMON_NN_COUNT];
	__u64 spfi_irq_hist[RAMON_NN_COUNT][RAMON_SPFI_IRQ_VECTORS];
	__u64 spfi_alert_overrun[RAMON_NN_COUNT];
	__u64 spfi_unexpected[RAMON_NN_COUNT];	/* RX IRQ with nothing armed */
	__u64 spfi_timeouts[RAMON_NN_COUNT];
	struct ramon_status st;
};

#define RAMON_IOC_GET_INFO		_IOWR(RAMON_IOC_MAGIC,  1, struct ramon_get_info)
#define RAMON_IOC_CHAN_INFO		_IOWR(RAMON_IOC_MAGIC,  2, struct ramon_chan_info)
#define RAMON_IOC_BUF_ALLOC		_IOWR(RAMON_IOC_MAGIC,  3, struct ramon_buf_alloc)
#define RAMON_IOC_BUF_FREE		_IOWR(RAMON_IOC_MAGIC,  4, struct ramon_buf_free)
#define RAMON_IOC_BUF_INFO		_IOWR(RAMON_IOC_MAGIC,  5, struct ramon_buf_info)
#define RAMON_IOC_AXI_XFER		_IOWR(RAMON_IOC_MAGIC,  6, struct ramon_axi_xfer)
#define RAMON_IOC_ZDMA_COPY		_IOWR(RAMON_IOC_MAGIC,  7, struct ramon_zdma_copy)
#define RAMON_IOC_SPW_WAIT_RX		_IOWR(RAMON_IOC_MAGIC,  8, struct ramon_spw_wait_rx)
#define RAMON_IOC_SPW_CANCEL		_IOWR(RAMON_IOC_MAGIC,  9, struct ramon_spw_cancel)
#define RAMON_IOC_SPW_LOOPBACK		_IOWR(RAMON_IOC_MAGIC, 10, struct ramon_spw_loopback)
#define RAMON_IOC_SPFI_CMD		_IOWR(RAMON_IOC_MAGIC, 11, struct ramon_spfi_cmd)
#define RAMON_IOC_SPFI_WRITE		_IOWR(RAMON_IOC_MAGIC, 12, struct ramon_spfi_write)
#define RAMON_IOC_SPFI_READ		_IOWR(RAMON_IOC_MAGIC, 13, struct ramon_spfi_read)
#define RAMON_IOC_SPFI_WAIT_ALERT	_IOWR(RAMON_IOC_MAGIC, 14, struct ramon_spfi_wait_alert)
#define RAMON_IOC_SPFI_CANCEL_ALERT	_IOWR(RAMON_IOC_MAGIC, 15, struct ramon_spfi_cancel_alert)
#define RAMON_IOC_SPFI_MEM_READ		_IOWR(RAMON_IOC_MAGIC, 16, struct ramon_spfi_mem_read)
#define RAMON_IOC_SPFI_TX_OFFS_WRITE	_IOWR(RAMON_IOC_MAGIC, 17, struct ramon_spfi_tx_offs_write)
#define RAMON_IOC_REGWIN_INFO		_IOWR(RAMON_IOC_MAGIC, 18, struct ramon_regwin_info)
#define RAMON_IOC_REG_IO		_IOWR(RAMON_IOC_MAGIC, 19, struct ramon_reg_io)
#define RAMON_IOC_GET_STATS		_IOWR(RAMON_IOC_MAGIC, 20, struct ramon_get_stats)

#ifndef __KERNEL__
#include <errno.h>
#include <stddef.h>

struct ramon_err_info {
	__u32 code;
	int err;		/* positive errno */
	const char *name;	/* "RAMON_E_BUF_RANGE" */
	const char *desc;	/* "offset/len outside the buffer" */
};

#define RAMON_ERR_INFO(name, code, err, desc)	{ code, err, "RAMON_E_" #name, desc },

/* NULL if code is unknown to this header version */
static inline const struct ramon_err_info *ramon_err_lookup(__u32 code)
{
	static const struct ramon_err_info tab[] = {
		RAMON_ERR_LIST(RAMON_ERR_INFO)
		{ 0, 0, NULL, NULL }	/* end marker */
	};
	const struct ramon_err_info *e;

	for (e = tab; e->name; e++)
		if (e->code == code)
			return e;
	return NULL;
}

static inline const char *ramon_err_name(__u32 code)
{
	const struct ramon_err_info *e = ramon_err_lookup(code);

	return e ? e->name : "RAMON_E_UNKNOWN";
}

static inline const char *ramon_err_desc(__u32 code)
{
	const struct ramon_err_info *e = ramon_err_lookup(code);

	return e ? e->desc : "unknown ramon_dma error code";
}

/* the positive errno the driver returns with code; 0 if code is unknown */
static inline int ramon_err_errno(__u32 code)
{
	const struct ramon_err_info *e = ramon_err_lookup(code);

	return e ? e->err : 0;
}
#endif /* !__KERNEL__ */

#endif /* RAMON_DMA_UAPI_H */

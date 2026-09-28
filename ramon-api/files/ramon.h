/* SPDX-License-Identifier: MIT */
/*
 * libramon: user-space API for the ramon_dma driver (/dev/ramon_dma).
 *
 * This header is the core layer: device context, DMA buffers, AXI and ZDMA
 * transfers, register windows, statistics and error reporting. The SpaceWire
 * packet layer is in ramon_spw.h and the SPFI stream layer in ramon_spfi.h.
 *
 * Conventions (every function in every libramon header):
 * - The return value is 0 on success or a negative errno.
 * - A trailing "struct ramon_status *st" may be NULL. When it is not, it is
 *   filled on success and on failure: with the driver's trailer for driver
 *   errors (st->code is a RAMON_E_* code), or with a library code
 *   (RAMON_EL_*, >= RAMON_E_LIB_BASE) for errors detected in user space.
 *   ramon_strerror() turns either into one readable line. There is no global
 *   or per-context "last error", so status reporting is thread-safe.
 * - timeout_ms == 0 selects the driver default for that operation.
 * - Every function may be called from any thread on a shared ramon_ctx unless
 *   its comment says otherwise. A ramon_buf is not locked; do not free it
 *   while another thread uses it.
 */
#ifndef RAMON_H
#define RAMON_H

#include <stddef.h>
#include <stdint.h>

#include "ramon_dma_uapi.h"
#include "ramon_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define RAMON_PRINTF(a, b)	__attribute__((format(printf, a, b)))
#else
#define RAMON_PRINTF(a, b)
#endif

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */
/* ------------------------------------------------------------------------ */

#define RAMON_E_LIB_BASE	0x1000u

/* X(name, code, errno, description): errors detected by the library itself */
#define RAMON_LIB_ERR_LIST(X) \
	X(SYS,			0x1000, 0,		"system call failed") \
	X(ABI_MISMATCH,		0x1001, EPROTO,		"driver ABI version differs from the library's") \
	X(INVAL,		0x1002, EINVAL,		"invalid argument") \
	X(BUSY,			0x1003, EBUSY,		"resource busy") \
	X(NOT_INITED,		0x1004, EINVAL,		"subsystem not initialised") \
	X(NOT_PRESENT,		0x1005, ENXIO,		"NN instance not present or not enabled") \
	X(TOO_LARGE,		0x1006, EMSGSIZE,	"data larger than the buffer for it") \
	X(SPW_LINK_DOWN,	0x1007, ENOLINK,	"SpaceWire link not synced") \
	X(SPW_BAD_PACKET,	0x1008, EBADMSG,	"SpaceWire packet failed its CRC or length checks") \
	X(SPW_REPLY_TIMEOUT,	0x1009, ETIMEDOUT,	"no matching SpaceWire reply before the timeout") \
	X(SPW_NACK,		0x100A, EPROTO,		"the NN rejected the request") \
	X(SPFI_LINK_DOWN,	0x100B, ENOLINK,	"SPFI link down (0xCC low byte is not 0x88)") \
	X(SPFI_NN_ERROR,	0x100C, EPROTO,		"the NN answered the SPFI command with an error code") \
	X(SPFI_RULE,		0x100D, EINVAL,		"request breaks an NN stream rule")

#define RAMON_LIB_ERR_ENUM(name, code, err, desc)	RAMON_EL_##name = code,
enum ramon_lib_err {
	RAMON_LIB_ERR_LIST(RAMON_LIB_ERR_ENUM)
};

/* "RAMON_E_BUF_RANGE", "RAMON_EL_SPW_NACK", or "RAMON_E_UNKNOWN" */
const char *ramon_err_str(uint32_t code);
/* the one-line description of a driver or library code */
const char *ramon_err_text(uint32_t code);
/*
 * Formats st as one line, e.g.
 * "RAMON_E_BUF_RANGE (offset/len outside the buffer): buf 3: offset 0x10000 + len 0x8000 > size 0x10000 [errno 34]".
 * Returns the length it wanted to write, like snprintf.
 */
size_t ramon_strerror(const struct ramon_status *st, char *buf, size_t len);
/* sets every field of st; a no-op when st is NULL */
void ramon_status_set(struct ramon_status *st, uint32_t code, int err, uint64_t arg0,
		      uint64_t arg1, const char *fmt, ...) RAMON_PRINTF(6, 7);
void ramon_status_clear(struct ramon_status *st);

/* ------------------------------------------------------------------------ */
/* Context                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct ramon_ctx ramon_ctx;

#define RAMON_DEV_PATH		"/dev/" RAMON_DEV_NAME

/*
 * Opens the device (NULL = /dev/ramon_dma), reads GET_INFO and checks the
 * driver's ABI version. Fails with RAMON_EL_ABI_MISMATCH if it differs from
 * the header this library was built with; logs a warning if the driver is
 * older than RAMON_API_DRIVER_MIN.
 */
int ramon_open(const char *dev_path, ramon_ctx **out, struct ramon_status *st);
/* Stops the SPW RX and SPFI alert threads, frees every buffer, closes the device. NULL is ok. */
void ramon_close(ramon_ctx *c);
int ramon_fd(const ramon_ctx *c);
/* the GET_INFO snapshot taken by ramon_open() */
const struct ramon_get_info *ramon_info(const ramon_ctx *c);
size_t ramon_page_size(const ramon_ctx *c);
/* "libramon 0.1.0 (abi 1), driver 0.8.8 (abi 1)" */
int ramon_version_line(const ramon_ctx *c, char *buf, size_t len);

enum ramon_log_level {
	RAMON_LOG_ERR	= 0,
	RAMON_LOG_WARN	= 1,
	RAMON_LOG_INFO	= 2,
	RAMON_LOG_DEBUG	= 3,
};
/* may be called from library threads; must be thread-safe */
typedef void (*ramon_log_fn)(void *user, int level, const char *msg);
/* fn == NULL restores the default (stderr); messages above level are dropped. Default level: WARN. */
void ramon_set_log(ramon_ctx *c, ramon_log_fn fn, void *user, int level);

/* ------------------------------------------------------------------------ */
/* Raw layer: one wrapper per ioctl. The caller fills the input fields; the  */
/* output fields and the trailer (a->st) are always written back. ENOTTY and */
/* EFAULT, for which the driver writes no trailer, get a RAMON_EL_SYS one.   */
/* ------------------------------------------------------------------------ */

int ramon_ioctl(ramon_ctx *c, unsigned long req, void *arg);
int ramon_ioc_get_info(ramon_ctx *c, struct ramon_get_info *a);
int ramon_ioc_chan_info(ramon_ctx *c, struct ramon_chan_info *a);
int ramon_ioc_buf_alloc(ramon_ctx *c, struct ramon_buf_alloc *a);
int ramon_ioc_buf_free(ramon_ctx *c, struct ramon_buf_free *a);
int ramon_ioc_buf_info(ramon_ctx *c, struct ramon_buf_info *a);
int ramon_ioc_axi_xfer(ramon_ctx *c, struct ramon_axi_xfer *a);
int ramon_ioc_zdma_copy(ramon_ctx *c, struct ramon_zdma_copy *a);
int ramon_ioc_spw_wait_rx(ramon_ctx *c, struct ramon_spw_wait_rx *a);
int ramon_ioc_spw_cancel(ramon_ctx *c, struct ramon_spw_cancel *a);
int ramon_ioc_spw_loopback(ramon_ctx *c, struct ramon_spw_loopback *a);
int ramon_ioc_spfi_cmd(ramon_ctx *c, struct ramon_spfi_cmd *a);
int ramon_ioc_spfi_write(ramon_ctx *c, struct ramon_spfi_write *a);
int ramon_ioc_spfi_read(ramon_ctx *c, struct ramon_spfi_read *a);
int ramon_ioc_spfi_wait_alert(ramon_ctx *c, struct ramon_spfi_wait_alert *a);
int ramon_ioc_spfi_cancel_alert(ramon_ctx *c, struct ramon_spfi_cancel_alert *a);
int ramon_ioc_spfi_mem_read(ramon_ctx *c, struct ramon_spfi_mem_read *a);
int ramon_ioc_spfi_tx_offs_write(ramon_ctx *c, struct ramon_spfi_tx_offs_write *a);
int ramon_ioc_regwin_info(ramon_ctx *c, struct ramon_regwin_info *a);
int ramon_ioc_reg_io(ramon_ctx *c, struct ramon_reg_io *a);
int ramon_ioc_get_stats(ramon_ctx *c, struct ramon_get_stats *a);

/* convenience forms of the query ioctls */
int ramon_get_info(ramon_ctx *c, struct ramon_get_info *gi, struct ramon_status *st);
int ramon_chan_info(ramon_ctx *c, uint32_t index, struct ramon_chan_info *ci, struct ramon_status *st);
int ramon_regwin_info(ramon_ctx *c, uint32_t index, struct ramon_regwin_info *ri, struct ramon_status *st);
/* reset != 0 zeroes the driver counters after reading them (for every user of the device) */
int ramon_get_stats(ramon_ctx *c, int reset, struct ramon_get_stats *g, struct ramon_status *st);

/* ------------------------------------------------------------------------ */
/* DMA buffers                                                               */
/* ------------------------------------------------------------------------ */

/*
 * A zeroed, page-aligned, physically contiguous buffer owned by the context.
 * size is the page-rounded size; ptr is its mapping (NULL with RAMON_BUF_NOMAP).
 * Buffers are coherent and on ZynqMP most likely uncached: CPU access is slow,
 * prefer ordinary memory for CPU-heavy work and copy in and out.
 */
typedef struct ramon_buf {
	ramon_ctx *ctx;
	uint32_t handle;
	uint32_t flags;
	uint64_t size;
	void *ptr;
} ramon_buf;

#define RAMON_BUF_NOMAP		0x1u

int ramon_buf_alloc(ramon_ctx *c, uint64_t size, unsigned flags, ramon_buf *b,
		    struct ramon_status *st);
int ramon_buf_map(ramon_buf *b, struct ramon_status *st);
void ramon_buf_unmap(ramon_buf *b);
/* munmap + BUF_FREE, then zeroes *b. A zeroed ramon_buf is accepted and ignored. */
int ramon_buf_free(ramon_buf *b, struct ramon_status *st);
/* debug only: the bus address the DMA engines use */
int ramon_buf_dma_addr(const ramon_buf *b, uint64_t *dma_addr, struct ramon_status *st);
uint64_t ramon_max_buf_bytes(const ramon_ctx *c);

/* ------------------------------------------------------------------------ */
/* AXI DMA and ZDMA                                                          */
/* ------------------------------------------------------------------------ */

/*
 * Synchronous scatter-gather transfer on one AXI channel (n_items
 * 1..RAMON_AXI_MAX_ITEMS). A transient RAMON_E_AXI_PREP_FAILED (descriptor
 * pool still busy after a long list) is retried once.
 * Note: a timeout resets the whole axi_dma IP, both directions.
 */
int ramon_axi_xfer(ramon_ctx *c, uint32_t chan, const struct ramon_sg_item *items,
		   uint32_t n_items, uint32_t timeout_ms, uint64_t *bytes, struct ramon_status *st);
int ramon_axi_xfer1(ramon_ctx *c, uint32_t chan, const ramon_buf *b, uint64_t off, uint64_t len,
		    uint32_t timeout_ms, struct ramon_status *st);
/* how many AXI_PREP_FAILED retries this context has made */
uint64_t ramon_axi_retries(const ramon_ctx *c);
/* SPW NN nn uses AXI channels 2nn and 2nn+1; returns which is RX (DEV_TO_MEM) and TX */
int ramon_axi_find_pair(ramon_ctx *c, uint32_t nn, uint32_t *rx_chan, uint32_t *tx_chan,
			struct ramon_status *st);

/* n 1..RAMON_ZDMA_MAX_COPIES; *done = entries completed (may be NULL) */
int ramon_zdma_copy(ramon_ctx *c, const struct ramon_copy *e, uint32_t n, uint32_t timeout_ms,
		    uint32_t *done, struct ramon_status *st);
int ramon_zdma_copy1(ramon_ctx *c, const ramon_buf *src, uint64_t src_off, const ramon_buf *dst,
		     uint64_t dst_off, uint64_t len, uint32_t timeout_ms, struct ramon_status *st);

/* ------------------------------------------------------------------------ */
/* Register windows                                                          */
/* ------------------------------------------------------------------------ */

/*
 * 32-bit register access. win is a window name ("rs_top", "sysmon_ps",
 * "spw0", "spfi0@a0100000", a reg-access device name, ...); the part before
 * '@' is enough. off is a byte offset, 4-aligned.
 */
int ramon_reg_read(ramon_ctx *c, const char *win, uint32_t off, uint32_t *val,
		   struct ramon_status *st);
int ramon_reg_write(ramon_ctx *c, const char *win, uint32_t off, uint32_t val,
		    struct ramon_status *st);
int ramon_reg_read_idx(ramon_ctx *c, uint32_t index, uint32_t off, uint32_t *val,
		       struct ramon_status *st);
int ramon_reg_write_idx(ramon_ctx *c, uint32_t index, uint32_t off, uint32_t val,
			struct ramon_status *st);
/* the window index for a full name or the part before '@' */
int ramon_regwin_find(ramon_ctx *c, const char *name, uint32_t *index, struct ramon_status *st);

/* RS-TOP registers (byte offsets) */
#define RAMON_RSTOP_MAJOR		0x00
#define RAMON_RSTOP_MINOR		0x04
#define RAMON_RSTOP_MINOR_MINOR		0x08
#define RAMON_RSTOP_CONFIG		0x0C
#define RAMON_RSTOP_TIMESTAMP		0x10
#define RAMON_RSTOP_PPS_COUNT		0x14
#define RAMON_RSTOP_CLOCKS		0x18
#define RAMON_RSTOP_TRST_SPI_EN		0x40

/* SPW registers (byte offsets in the spw<n> window) */
#define RAMON_SPW_REG_VERSION		0x00
#define RAMON_SPW_REG_IP_MAJOR		0x04
#define RAMON_SPW_REG_IP_MINOR		0x08
#define RAMON_SPW_REG_LOOPBACK		0x10
#define RAMON_SPW_REG_RESET		0x1C
#define RAMON_SPW_REG_TX_FSM		0x20
#define RAMON_SPW_REG_RX_FSM		0x24
#define RAMON_SPW_REG_LINK		0x28

/* SPFI registers (byte offsets in the spfi<n> window) */
#define RAMON_SPFI_REG_LOOPBACK		0x2C	/* word 11 */
#define RAMON_SPFI_REG_WR_VERSION	0x40	/* word 16 */
#define RAMON_SPFI_REG_IP_MAJOR		0x44	/* word 17 */
#define RAMON_SPFI_REG_IP_MINOR		0x48	/* word 18 */
#define RAMON_SPFI_REG_WR_STATUS0	0xC8	/* word 50 */
#define RAMON_SPFI_REG_PHY_STATUS1	0xCC	/* word 51 */

struct ramon_rstop_time {
	unsigned sec, min, hour, day, month, year;	/* year: two digits */
};
/* RS-TOP 0x00/0x04/0x08 */
int ramon_rstop_version(ramon_ctx *c, uint32_t v[3], struct ramon_status *st);
/* RS-TOP 0x10 bit fields: sec[5:0] min[11:6] hour[16:12] year[22:17] month[26:23] day[31:27] */
void ramon_rstop_decode_time(uint32_t v, struct ramon_rstop_time *t);

/* SYSMON: temperature at offset 0 of each window, 37 voltage rails */
#define RAMON_SYSMON_RAILS	37
struct ramon_sysmon_rail {
	const char *name;
	const char *desc;
	uint32_t win;		/* RAMON_WIN_SYSMON_PS or RAMON_WIN_SYSMON_PL */
	uint32_t offset;
	uint32_t mult;
};
extern const struct ramon_sysmon_rail ramon_sysmon_rails[RAMON_SYSMON_RAILS];

struct ramon_sysmon {
	uint32_t ps_temp_raw, pl_temp_raw;
	double ps_temp_c, pl_temp_c;
	uint32_t raw[RAMON_SYSMON_RAILS];
	double volts[RAMON_SYSMON_RAILS];
};
double ramon_sysmon_temp_c(uint32_t raw);			/* raw / 65536 * 509.314 - 280.239 */
double ramon_sysmon_volts(uint32_t raw, uint32_t mult);		/* raw / 65536 * mult */
int ramon_sysmon_read(ramon_ctx *c, struct ramon_sysmon *out, struct ramon_status *st);

#ifdef __cplusplus
}
#endif

#endif /* RAMON_H */

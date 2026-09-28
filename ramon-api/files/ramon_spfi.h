/* SPDX-License-Identifier: MIT */
/*
 * libramon SPFI stream layer: NN storage streams in 16 KiB pages.
 *
 * NN rules this layer enforces or documents:
 * - Streams are non-cyclic (stream_type 0).
 * - Stream ids 193..197 are reserved; use 0..192.
 * - At most 8 streams may be open at once.
 * - A stream must be deleted before its id is opened again.
 * - The first page of a new stream is its table record's latest_write_offs + 1,
 *   read before OPEN (ramon_spfi_stream_open does this).
 * - FORMAT erases every stream.
 * - SPFI1 is not wired on the ramon board; the driver's spfi_mask keeps it off.
 *
 * A command the NN answers with a non-zero rx_err_code is still a successful
 * ioctl. ramon_spfi_command() returns 0 then; the per-operation wrappers turn
 * it into RAMON_EL_SPFI_NN_ERROR (st->arg[0] = rx_err_code).
 *
 * After any SPFI timeout the next SPFI call on that NN waits settle_ms
 * (default 500), so that a late answer is not taken for the next command's.
 */
#ifndef RAMON_SPFI_H
#define RAMON_SPFI_H

#include "ramon.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RAMON_SPFI_PAGE			RAMON_SPFI_OFFSET_BYTES	/* 16384 */
#define RAMON_SPFI_STREAMS		198
#define RAMON_SPFI_STREAMS_USABLE	193
#define RAMON_SPFI_MAX_OPEN		8
#define RAMON_SPFI_WRITE_MAX_PAGES	5000	/* pages per DATA_WRITE the NN accepts */
#define RAMON_SPFI_READ_MAX_PAGES	(RAMON_SPFI_TX_OFFS_MAX / 4)	/* 8192 */
#define RAMON_SPFI_ST_EXIST		0x01
#define RAMON_SPFI_ST_OPEN		0x02
#define RAMON_SPFI_ST_CYCLIC		0x04
#define RAMON_SPFI_CHAN_NN0		5	/* AXI write channel feeding NN0 */
#define RAMON_SPFI_CHAN_NN1		4
#define RAMON_SPFI_LINK_UP		0x88	/* low byte of register 0xCC */

/* the RX message table after GET_ALL_STREAM_STATUS */
struct ramon_spfi_table_hdr {
	uint32_t total_media_size;
	uint32_t total_allocated_size;
	uint32_t total_used_size;
	uint8_t  dynamic_quality;
	uint8_t  static_quality;
	uint8_t  reserved[50];
} __attribute__((packed));

struct ramon_spfi_record {
	uint8_t  status;		/* RAMON_SPFI_ST_* */
	uint8_t  reserved0;
	uint16_t src_net_addr;
	uint16_t write_prog_target;
	uint16_t last_host_rd;
	uint16_t last_host_wr;
	uint16_t reserved1;
	uint32_t size_thresh;
	uint32_t latest_readable_offset;
	uint32_t alloc_size;
	uint32_t write_time;
	uint32_t read_time;
	uint32_t latest_read_offs;
	uint32_t latest_write_offs;
	uint8_t  reserved2[24];
} __attribute__((packed));

struct ramon_spfi_table {
	struct ramon_spfi_table_hdr hdr;
	struct ramon_spfi_record rec[RAMON_SPFI_STREAMS];
} __attribute__((packed));

#define RAMON_SPFI_TABLE_BYTES	(64 + RAMON_SPFI_STREAMS * 64)	/* 12736 */

#ifdef __cplusplus
static_assert(sizeof(struct ramon_spfi_table) == RAMON_SPFI_TABLE_BYTES, "SPFI table layout");
#else
_Static_assert(sizeof(struct ramon_spfi_table_hdr) == 64, "SPFI table header is 64 bytes");
_Static_assert(sizeof(struct ramon_spfi_record) == 64, "SPFI table record is 64 bytes");
_Static_assert(sizeof(struct ramon_spfi_table) == RAMON_SPFI_TABLE_BYTES, "SPFI table layout");
#endif

/* every 0 selects the default shown */
struct ramon_spfi_config {
	uint32_t chan[RAMON_NN_COUNT];	/* AXI write channel per NN: {5, 4} */
	uint32_t nn_mask;		/* NNs to use: spfi_mask from GET_INFO */
	uint32_t cmd_timeout_ms;	/* driver default */
	uint32_t write_timeout_ms;	/* driver default */
	uint32_t read_timeout_ms;	/* driver default */
	uint32_t settle_ms;		/* pause after an SPFI timeout: 500 */
	uint32_t scratch_pages;		/* bounce buffer of the *_mem calls: 64 pages (1 MiB) */
};
void ramon_spfi_config_default(struct ramon_spfi_config *cfg);
/*
 * Optional: the first SPFI call initialises the layer with the defaults.
 * Checks that each NN's channel is an AXI MEM_TO_DEV channel; RAMON_EL_BUSY
 * if already initialised (call ramon_spfi_fini() first to change the config).
 */
int ramon_spfi_init(ramon_ctx *c, const struct ramon_spfi_config *cfg, struct ramon_status *st);
/* stops the alert threads and frees the bounce buffers; do not call it while other threads use SPFI */
void ramon_spfi_fini(ramon_ctx *c);
int ramon_spfi_is_inited(const ramon_ctx *c);
void ramon_spfi_get_config(const ramon_ctx *c, struct ramon_spfi_config *cfg);
int ramon_spfi_present(const ramon_ctx *c, uint32_t nn);

/* 1 up, 0 down, < 0 error; works without init; raw (may be NULL) = register 0xCC */
int ramon_spfi_link_up(ramon_ctx *c, uint32_t nn, uint32_t *raw, struct ramon_status *st);

/* short commands */
struct ramon_spfi_args {
	uint32_t stream_id;
	uint32_t stream_type;
	uint32_t stream_last_offset;
	uint32_t tod;
	uint32_t init_type;
	uint32_t no_ack;	/* fire and forget; the late answer is counted as unexpected */
	uint32_t timeout_ms;	/* 0 = cfg.cmd_timeout_ms */
};
struct ramon_spfi_reply {
	uint32_t rx_opcode;
	uint32_t rx_err_code;
	uint32_t rx_stream_id;
	uint32_t rx_offset;
	uint32_t rx_init_info;
	uint32_t rx_curr_tod;
	uint32_t rx_status;
};
/* any RAMON_SPFI_OP_* but DATA_WRITE and READ_STREAM; 0 whenever the ioctl succeeded */
int ramon_spfi_command(ramon_ctx *c, uint32_t nn, uint32_t opcode, const struct ramon_spfi_args *a,
		       struct ramon_spfi_reply *r, struct ramon_status *st);
const char *ramon_spfi_op_name(uint32_t opcode);

/*
 * Per-operation wrappers (r may be NULL). Each fails with RAMON_EL_SPFI_NN_ERROR
 * on a non-zero rx_err_code, except ramon_spfi_init_nn: INIT type 0 answers
 * rx_err_code 1 / init_info 0x2 on this board, so the caller judges r.
 */
int ramon_spfi_init_nn(ramon_ctx *c, uint32_t nn, uint32_t init_type, struct ramon_spfi_reply *r,
		       struct ramon_status *st);
int ramon_spfi_format(ramon_ctx *c, uint32_t nn, struct ramon_spfi_reply *r, struct ramon_status *st);
int ramon_spfi_platform_reset(ramon_ctx *c, uint32_t nn, struct ramon_spfi_reply *r,
			      struct ramon_status *st);
int ramon_spfi_power_down(ramon_ctx *c, uint32_t nn, struct ramon_spfi_reply *r,
			  struct ramon_status *st);
int ramon_spfi_set_tod(ramon_ctx *c, uint32_t nn, uint32_t tod, struct ramon_spfi_reply *r,
		       struct ramon_status *st);
/* size_pages: the stream's planned size, passed as stream_last_offset (0 = none) */
int ramon_spfi_open(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t size_pages,
		    struct ramon_spfi_reply *r, struct ramon_status *st);
int ramon_spfi_close(ramon_ctx *c, uint32_t nn, uint32_t sid, struct ramon_spfi_reply *r,
		     struct ramon_status *st);
int ramon_spfi_delete(ramon_ctx *c, uint32_t nn, uint32_t sid, struct ramon_spfi_reply *r,
		      struct ramon_status *st);
/* r->rx_offset = the flushed offset */
int ramon_spfi_flush(ramon_ctx *c, uint32_t nn, uint32_t sid, struct ramon_spfi_reply *r,
		     struct ramon_status *st);

/* GET_ALL_STREAM_STATUS, then the table */
int ramon_spfi_get_table(ramon_ctx *c, uint32_t nn, struct ramon_spfi_table *t,
			 struct ramon_status *st);
unsigned ramon_spfi_count_open(const struct ramon_spfi_table *t);
unsigned ramon_spfi_count_exist(const struct ramon_spfi_table *t);
/* up to want usable ids that do not exist, highest first; returns how many */
unsigned ramon_spfi_find_free(const struct ramon_spfi_table *t, uint32_t *out, unsigned want);
int ramon_spfi_mem_read(ramon_ctx *c, uint32_t nn, uint32_t off, uint32_t size, void *out,
			struct ramon_status *st);
int ramon_spfi_tx_offs_write(ramon_ctx *c, uint32_t nn, uint32_t off, uint32_t size, const void *in,
			     struct ramon_status *st);

/*
 * Data path; first_page is the stream page ("offset") of the first 16 KiB.
 * write: sum of item lengths == n_pages * 16 KiB, n_items 1..5000, n_pages 1..5000.
 */
int ramon_spfi_write(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page,
		     const struct ramon_sg_item *items, uint32_t n_items, uint32_t n_pages,
		     uint32_t timeout_ms, uint32_t *rx_status, struct ramon_status *st);
int ramon_spfi_write_buf(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page,
			 const ramon_buf *b, uint64_t off, uint32_t n_pages, uint32_t timeout_ms,
			 struct ramon_status *st);
/* from ordinary memory through the NN's bounce buffer; len a multiple of 16 KiB */
int ramon_spfi_write_mem(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page,
			 const void *data, size_t len, uint32_t timeout_ms, struct ramon_status *st);
/* pages[0..n-1] (1..8192) into dst at dst_off; dst must hold n * 16 KiB there */
int ramon_spfi_read(ramon_ctx *c, uint32_t nn, uint32_t sid, const uint32_t *pages, uint32_t n,
		    const ramon_buf *dst, uint64_t dst_off, uint32_t timeout_ms, uint32_t *rx_status,
		    struct ramon_status *st);
int ramon_spfi_read_seq(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page, uint32_t n,
			const ramon_buf *dst, uint64_t dst_off, uint32_t timeout_ms,
			struct ramon_status *st);
int ramon_spfi_read_mem(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t first_page, uint32_t n,
			void *out, uint32_t timeout_ms, struct ramon_status *st);

/* an open stream and where the next write goes */
struct ramon_spfi_stream {
	uint32_t nn;
	uint32_t sid;
	uint32_t first_page;	/* first page of this session's writes */
	uint32_t next_page;
	uint32_t pages_written;
	int open;
};
/*
 * Reads the table, checks the rules (id usable, not existing, fewer than 8
 * open: RAMON_EL_SPFI_RULE otherwise), takes latest_write_offs + 1 as the
 * first page, then OPENs.
 */
int ramon_spfi_stream_open(ramon_ctx *c, uint32_t nn, uint32_t sid, uint32_t size_pages,
			   struct ramon_spfi_stream *s, struct ramon_status *st);
int ramon_spfi_stream_append(ramon_ctx *c, struct ramon_spfi_stream *s, const ramon_buf *b,
			     uint64_t off, uint32_t n_pages, uint32_t timeout_ms,
			     struct ramon_status *st);
int ramon_spfi_stream_append_mem(ramon_ctx *c, struct ramon_spfi_stream *s, const void *data,
				 size_t len, uint32_t timeout_ms, struct ramon_status *st);
int ramon_spfi_stream_flush(ramon_ctx *c, struct ramon_spfi_stream *s, struct ramon_status *st);
/* CLOSE, and DELETE if and_delete */
int ramon_spfi_stream_close(ramon_ctx *c, struct ramon_spfi_stream *s, int and_delete,
			    struct ramon_status *st);

/* alerts */
struct ramon_spfi_alert {
	uint32_t nn;
	uint32_t code;
	uint32_t sub_code;
	uint32_t param1;
	uint32_t param2;
	uint32_t rx_status;
};
/* called on the library alert thread; must return quickly and not call ramon_spfi_alert_stop */
typedef void (*ramon_spfi_alert_cb)(void *user, const struct ramon_spfi_alert *a);
/* timeout_ms 0 = wait forever */
int ramon_spfi_wait_alert(ramon_ctx *c, uint32_t nn, uint32_t timeout_ms, struct ramon_spfi_alert *a,
			  struct ramon_status *st);
/* releases every alert waiter on NN nn, in every process */
int ramon_spfi_cancel_alert(ramon_ctx *c, uint32_t nn, struct ramon_status *st);
int ramon_spfi_alert_start(ramon_ctx *c, uint32_t nn, ramon_spfi_alert_cb cb, void *user,
			   struct ramon_status *st);
int ramon_spfi_alert_stop(ramon_ctx *c, uint32_t nn);
/* waits for alerts until none arrives for idle_ms; cb may be NULL */
int ramon_spfi_drain_alerts(ramon_ctx *c, uint32_t nn, uint32_t idle_ms, ramon_spfi_alert_cb cb,
			    void *user, uint32_t *n, struct ramon_status *st);

/* diagnostics: SPFI status words and the driver's per-NN counters */
#define RAMON_SPFI_DIAG_WORDS	7
extern const uint32_t ramon_spfi_diag_word_index[RAMON_SPFI_DIAG_WORDS];	/* 48 49 50 51 19 21 33 */
struct ramon_spfi_diag {
	uint32_t words[RAMON_SPFI_DIAG_WORDS];
	uint64_t irq_hist[RAMON_SPFI_IRQ_VECTORS];
	uint64_t unexpected;
	uint64_t timeouts;
	uint64_t alert_overrun;
};
int ramon_spfi_diag(ramon_ctx *c, uint32_t nn, struct ramon_spfi_diag *d, struct ramon_status *st);
/* one line, including the SPFI_WR_STATUS0 (word 50) bit meanings */
size_t ramon_spfi_diag_format(const struct ramon_spfi_diag *d, char *buf, size_t len);

/*
 * NN bring-up after power-up (the smoke tool's spfiprep): SPFI link check,
 * SPW link sync and DPS on the same NN index (initialises the SPW layer with
 * defaults if needed), INIT type 0, FORMAT if format != 0 (erases every
 * stream), then the stream table into t (may be NULL). st->msg names the
 * stage that failed.
 */
struct ramon_spfi_prep_opts {
	int format;
	int skip_spw_dps;
};
int ramon_spfi_prep(ramon_ctx *c, uint32_t nn, const struct ramon_spfi_prep_opts *o,
		    struct ramon_spfi_table *t, struct ramon_status *st);

#ifdef __cplusplus
}
#endif

#endif /* RAMON_SPFI_H */

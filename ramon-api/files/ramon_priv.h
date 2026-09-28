/* SPDX-License-Identifier: MIT */
/* libramon internals; not installed. Include first in every library source. */
#ifndef RAMON_PRIV_H
#define RAMON_PRIV_H

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "ramon.h"
#include "ramon_spw.h"
#include "ramon_spfi.h"

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

/* ---- SPW ---- */

enum spw_rx_state {
	SPW_RX_NONE = 0,
	SPW_RX_RUNNING,
	SPW_RX_STOPPING,
};

/* one pending ramon_spw_request() in RX-thread mode */
struct spw_waiter {
	int armed;
	int done;
	int err;
	int any;			/* want was NULL */
	struct ramon_spw_match want;
	void *buf;
	uint32_t cap;
	struct ramon_spw_rx rx;
	struct ramon_status st;
};

struct spw_nn {
	struct ramon_ctx *ctx;
	int present;
	uint32_t nn;
	uint32_t rx_chan;
	uint32_t tx_chan;
	char win[8];			/* "spw0" */
	ramon_buf rxbuf;		/* DMA landing area of the RX thread and of mode-B receivers */
	uint8_t *rxcopy;		/* cached copy of rxbuf that is parsed and handed out */

	pthread_mutex_t lock;		/* everything below */
	pthread_cond_t cond;		/* waiter.done */
	int state;			/* enum spw_rx_state */
	int thread_valid;		/* thread created and not yet joined */
	int thread_done;		/* the thread left its loop by itself (device removed) */
	pthread_t thread;
	int receiver;			/* a mode-B receiver owns rxbuf */
	ramon_spw_rx_cb cb;
	void *cb_user;
	struct spw_waiter waiter;
	struct ramon_spw_stats stats;
};

struct spw_state {
	int inited;
	struct ramon_spw_config cfg;
	struct spw_nn nn[RAMON_NN_COUNT];
	pthread_mutex_t pool_lock;
	pthread_cond_t pool_cond;
	ramon_buf *slot;
	unsigned char *busy;
	unsigned n_slots;
	uint32_t packet_id;		/* __atomic */
};

/* ---- SPFI ---- */

enum spfi_alert_state {
	SPFI_ALERT_NONE = 0,
	SPFI_ALERT_RUNNING,
	SPFI_ALERT_STOPPING,
};

struct spfi_nn {
	struct ramon_ctx *ctx;
	int present;
	uint32_t nn;
	uint32_t chan;
	uint64_t settle_until_ns;	/* __atomic */
	pthread_mutex_t scratch_lock;	/* scratch, held across the DMA that uses it */
	ramon_buf scratch;
	pthread_mutex_t lock;		/* the alert thread fields */
	int alert_state;
	int thread_valid;
	pthread_t thread;
	ramon_spfi_alert_cb cb;
	void *cb_user;
};

struct spfi_state {
	int inited;
	struct ramon_spfi_config cfg;
	struct spfi_nn nn[RAMON_NN_COUNT];
};

/* ---- context ---- */

struct ramon_ctx {
	int fd;
	struct ramon_get_info info;
	size_t page_size;
	unsigned page_shift;
	pthread_mutex_t lock;		/* init/fini of the SPW and SPFI layers */
	ramon_log_fn log_fn;
	void *log_user;
	int log_level;
	uint64_t axi_retries;		/* __atomic */
	struct spw_state spw;
	struct spfi_state spfi;
};

/* ioctl with the trailer copied to st; ENOTTY/EFAULT get a synthesized trailer */
int ramon__ioctl(struct ramon_ctx *c, unsigned long req, void *arg, struct ramon_status *trailer,
		 struct ramon_status *st);
/* sets a library error in st and returns -(its errno), or -err_override if non-zero */
int ramon__fail(struct ramon_status *st, uint32_t code, int err, uint64_t a0, uint64_t a1,
		const char *fmt, ...) RAMON_PRINTF(6, 7);
void ramon__log(struct ramon_ctx *c, int level, const char *fmt, ...) RAMON_PRINTF(3, 4);
uint64_t ramon__now_ns(void);
void ramon__sleep_ms(uint32_t ms);
/* pthread_cond_t using CLOCK_MONOTONIC */
void ramon__cond_init(pthread_cond_t *cv);
/* absolute CLOCK_MONOTONIC time timeout_ms from now */
void ramon__deadline(struct timespec *ts, uint32_t timeout_ms);
/* creates a thread with every signal blocked */
int ramon__thread_start(pthread_t *th, void *(*fn)(void *), void *arg);

void ramon__spw_ctx_init(struct ramon_ctx *c);
void ramon__spw_ctx_destroy(struct ramon_ctx *c);
void ramon__spfi_ctx_init(struct ramon_ctx *c);
void ramon__spfi_ctx_destroy(struct ramon_ctx *c);

#endif /* RAMON_PRIV_H */

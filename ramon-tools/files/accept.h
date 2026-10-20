/* SPDX-License-Identifier: MIT */
/* ramon_accept internals */
#ifndef ACCEPT_H
#define ACCEPT_H

#include "tools_common.h"

/* struct step.needs */
#define NEEDS_DEV	0x01	/* /dev/ramon_dma open */

#define ACCEPT_LIMITS_DEFAULT	"/etc/ramon/accept_limits.conf"

#define SPW_MAX_SIZES		16
#define SPW_APP_DEFAULT		100	/* application type of the delivery script's spwsend */
#define SPW_COUNT_DEFAULT	10000	/* packets per size, as the delivery script */
#define SPW_WINDOW_DEFAULT	32	/* packets in flight; below the driver's 64-entry RX queue */
#define SPW_SYNC_TRIES_DEFAULT	1000	/* link resets, 10 ms apart: the old spw_init() */

struct accept_opts {
	const char *dev;
	const char *limits;		/* SYSMON limits file, or NULL */
	int limits_given;		/* --limits on the command line: the file must exist */
	int have_rstop_expect;
	uint32_t rstop_expect[3];	/* --rstop-version X.Y.Z */
	const char *dt_root;		/* /proc/device-tree */
	const char *mem_dev;		/* /dev/mem */
	const char *ddr4_label;		/* DT label, or NULL to search */
	uint64_t ddr4_base;		/* with ddr4_size: skip the DT */
	uint64_t ddr4_size;
	uint64_t ddr4_limit;		/* --ddr4-mb in bytes; 0 = the whole region */
	uint32_t spw_nn_mask;		/* --spw-nn; 0 = every NN whose link syncs */
	uint8_t spw_node;		/* our node id, the destination of the loop packets */
	uint16_t spw_app;		/* application type of the loop packets */
	uint32_t spw_sizes[SPW_MAX_SIZES];	/* payload bytes */
	unsigned n_spw_sizes;
	uint32_t spw_count;		/* packets per size */
	uint32_t spw_seconds;		/* per size, instead of spw_count when non-zero */
	uint32_t spw_window;		/* packets in flight */
	uint32_t spw_sync_tries;	/* link resets before a link counts as down */
	const char *nn_config[RAMON_NN_COUNT];	/* --nn-config NN=FILE */
	const char *nn_config_all;		/* --nn-config FILE: every tested NN */
	const char *nn_update[RAMON_NN_COUNT];	/* --nn-update NN=FILE */
	const char *nn_update_all;		/* --nn-update FILE: every tested NN */
	uint32_t nn_sw_type;		/* --nn-sw-type, the update's software type */
	const char *nn_expect;		/* --nn-expect: text the image version must contain */
	uint32_t spfi_nn_mask;		/* --spfi-nn; 0 = every NN whose SPFI link is up */
	uint32_t spfi_chan[RAMON_NN_COUNT];	/* --spfi-chans: AXI write channel per NN */
	int spfi_format;		/* --spfi-format: spfi-prep FORMATs the NN */
	uint32_t spfi_scale;		/* --spfi-scale: percent of the runs and cycles, 1..100 */
	int spfi_long;			/* --spfi-long */
	int spfi_soak;			/* --spfi-soak */
	uint32_t mixed_seconds;		/* --mixed-seconds: length of each mixed test */
	uint32_t mixed_spw_bytes;	/* --mixed-spw-bytes: payload of the SPW messages */
	int mixed_late_ok;		/* --mixed-late-ok: missed deadlines are reported, not failed */
	uint32_t mixed_report_min;	/* --mixed-report-minutes: interim report period, 0 = none */
	int fail_fast;
};

struct accept_env {
	ramon_ctx *c;			/* NULL when the device did not open */
	struct accept_opts o;
	int spw_inited;			/* ramon_spw_init done (accept_spw.c) */
	uint32_t spw_synced;		/* NNs whose link synced */
	int spfi_inited;		/* ramon_spfi_init done (accept_spfi.c) */
	uint32_t spfi_up;		/* NNs whose SPFI link is up */
};

extern struct accept_env A;

struct step {
	const char *name;
	void (*fn)(void);
	unsigned needs;
	const char *legacy;		/* the dmaapi_testv2 command it replaces, or NULL */
	const char *help;
	/* NULL: in every default run; else only when it returns true (the step's option is given) */
	int (*wanted)(void);
};

/* the one-line result of the current step, shown in the summary */
void a_summary(const char *fmt, ...) RAMON_PRINTF(1, 2);
/* reports a failed call; returns ret */
int a_check(int ret, const char *what, const struct ramon_status *st);
/* SPW layer up and links synced once; returns the NNs to test (accept_spw.c) */
uint32_t a_spw_setup(void);

/* SPFI helpers shared by accept_spfi.c and accept_mixed.c */
/* SPW and SPFI set up, links waited for; returns the SPFI NNs to test */
uint32_t a_spfi_setup(void);
/* n free stream ids on NN nn, with room to open them; reports and fails otherwise */
int a_spfi_free_ids(uint32_t nn, uint32_t *ids, unsigned n);

/* n pages of 16 KiB: a fixed pattern, and a 16-byte stamp per page */
struct a_pages {
	ramon_buf b;		/* DMA buffer */
	uint8_t *ref;		/* the pattern in ordinary memory, for comparing */
	uint32_t n;
};
int a_pages_alloc(struct a_pages *p, uint32_t n, int with_ref);
void a_pages_free(struct a_pages *p);
void a_stamp_pages(struct a_pages *p, uint32_t n, uint32_t sid, uint32_t run);
/* bad pages of a read of run `run` of stream sid; prints the first few */
uint32_t a_check_pages(const struct a_pages *got, const struct a_pages *ref, uint32_t n,
		       uint32_t sid, uint32_t run, uint32_t *reported);

extern const struct step accept_board[];	/* accept_board.c */
extern const struct step accept_ddr4[];		/* accept_ddr4.c */
extern const struct step accept_spw[];		/* accept_spw.c */
extern const struct step accept_nn[];		/* accept_nn.c */
extern const struct step accept_spfi[];		/* accept_spfi.c */
extern const struct step accept_mixed[];	/* accept_mixed.c */

#endif /* ACCEPT_H */

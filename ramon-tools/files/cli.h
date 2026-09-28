/* SPDX-License-Identifier: MIT */
/* ramon_cli internals */
#ifndef CLI_H
#define CLI_H

#include <signal.h>

#include "tools_common.h"

#define CLI_SLOTS	16
#define CLI_MAX_ARGS	32

/* struct cli_cmd.flags */
#define CMD_NEED_SPW	0x1	/* initialise the SPW layer first */
#define CMD_DESTRUCTIVE	0x2	/* the last argument must be "yes" (not counted in min/max) */
#define CMD_HIDDEN	0x4	/* alias: not listed by help */
#define CMD_NO_DEV	0x8	/* runs without /dev/ramon_dma */

struct cli;
typedef int (*cli_fn)(struct cli *cl, int argc, char **argv);

struct cli_cmd {
	const char *name;
	cli_fn fn;
	int min_args;
	int max_args;
	unsigned flags;
	const char *usage;	/* arguments only */
	const char *help;
};

struct cli_group {
	const char *title;
	const struct cli_cmd *cmds;	/* ends with a NULL name */
};

struct cli {
	const char *dev;
	ramon_ctx *c;
	struct ramon_status st;		/* the failing command fills it */
	uint8_t node;			/* --node */
	uint8_t target;			/* --target */
	uint32_t spfi_chan[RAMON_NN_COUNT];
	uint32_t nn;			/* current NN of the spfi* commands (spfinnid) */
	int script;			/* not interactive: no progress lines */
	ramon_buf slot[CLI_SLOTS];
	/* SPW receive callback settings (spwrx, spwdump, log, fdirauto) */
	int dump_level;
	int log_rx;
	int fdir_auto;
	uint64_t rx_packets;		/* __atomic */
	uint64_t rx_errors;		/* __atomic */
};

extern volatile sig_atomic_t cli_interrupted;
/* 1 once Ctrl-C was pressed during the current command */
int cli_intr(void);

/* opens the device if needed; 0 or -errno with cl->st set */
int cli_ctx(struct cli *cl);
/* initialises the SPW layer with --node/--target if needed */
int cli_spw(struct cli *cl);
/* initialises the SPFI layer with --spfi-chans if needed */
int cli_spfi(struct cli *cl);
void cli_close_ctx(struct cli *cl);

/* argument parsing that reports into cl->st */
int cli_u32(struct cli *cl, const char *s, const char *what, uint32_t *v);
int cli_u64(struct cli *cl, const char *s, const char *what, uint64_t *v);
/* optional argument argv[i] (default def) */
int cli_opt_u32(struct cli *cl, int argc, char **argv, int i, uint32_t def, const char *what,
		uint32_t *v);
int cli_nn(struct cli *cl, uint32_t nn);
int cli_slot(struct cli *cl, const char *s, ramon_buf **b);
int cli_err(struct cli *cl, int err, const char *fmt, ...) RAMON_PRINTF(3, 4);

/* reg read/write helper used by several commands: prints "win+off = value" */
int cli_regio(struct cli *cl, const char *win, uint32_t index, int by_index, uint32_t off,
	      int write, uint32_t val);

/* the SPW receive callback of spwrx (cli_spw.c) */
void cli_spw_rx_cb(void *user, const struct ramon_spw_rx *rx);

extern const struct cli_cmd cli_core_cmds[];
extern const struct cli_cmd cli_spw_cmds[];
extern const struct cli_cmd cli_spfi_cmds[];
extern const struct cli_cmd cli_misc_cmds[];

#endif /* CLI_H */

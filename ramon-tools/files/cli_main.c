// SPDX-License-Identifier: MIT
/*
 * ramon_cli: command line for libramon / ramon_dma, the successor of the old
 * dmaapi_testv2 shell.
 *
 *   ramon_cli [options]                  interactive (TAB completion, history)
 *   ramon_cli [options] <cmd> [args]     one command
 *   ramon_cli [options] -f <script>      one command per line ("-" = stdin)
 *
 * Numbers are decimal unless written 0x.. (hex) or 0.. (octal).
 */
#include "cli.h"

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "linenoise.h"

#define HISTORY_FILE	".ramon_cli_history"
#define HISTORY_LEN	500
#define LINE_MAX_LEN	1024

volatile sig_atomic_t cli_interrupted;
static volatile sig_atomic_t last_intr_sec;
static int quit_requested;

#define N_MAIN_CMDS	7
static const struct cli_cmd cli_main_cmds[N_MAIN_CMDS];

static const struct cli_group groups[] = {
	{ "general", cli_main_cmds },
	{ "device, buffers, AXI, ZDMA, registers", cli_core_cmds },
	{ "SpaceWire", cli_spw_cmds },
	{ "SPFI", cli_spfi_cmds },
	{ "board extras (no ramon_dma needed)", cli_misc_cmds },
};

/* ---- context helpers ---- */

int cli_err(struct cli *cl, int err, const char *fmt, ...)
{
	va_list ap;

	cl->st.code = RAMON_EL_INVAL;
	cl->st.err = -err;
	cl->st.arg[0] = cl->st.arg[1] = 0;
	va_start(ap, fmt);
	vsnprintf(cl->st.msg, sizeof(cl->st.msg), fmt, ap);
	va_end(ap);
	return -err;
}

static void cli_log(void *user, int level, const char *msg)
{
	static const char *const names[] = { "error", "warning", "info", "debug" };

	(void)user;
	flockfile(stdout);
	printf("      libramon %s: %s\n", names[level & 3], msg);
	fflush(stdout);
	funlockfile(stdout);
}

int cli_ctx(struct cli *cl)
{
	int ret;

	if (cl->c)
		return 0;
	ret = ramon_open(cl->dev, &cl->c, &cl->st);
	if (!ret)
		ramon_set_log(cl->c, cli_log, NULL, RAMON_LOG_WARN);
	return ret;
}

int cli_spw(struct cli *cl)
{
	struct ramon_spw_config cfg;
	int ret = cli_ctx(cl);

	if (ret || ramon_spw_is_inited(cl->c))
		return ret;
	ramon_spw_config_default(&cfg);
	cfg.node_id = cl->node;
	cfg.nn_node = cl->target;
	return ramon_spw_init(cl->c, &cfg, &cl->st);
}

int cli_spfi(struct cli *cl)
{
	struct ramon_spfi_config cfg;
	int ret = cli_ctx(cl);

	if (ret || ramon_spfi_is_inited(cl->c))
		return ret;
	ramon_spfi_config_default(&cfg);
	cfg.chan[0] = cl->spfi_chan[0];
	cfg.chan[1] = cl->spfi_chan[1];
	return ramon_spfi_init(cl->c, &cfg, &cl->st);
}

void cli_close_ctx(struct cli *cl)
{
	unsigned i;

	if (!cl->c)
		return;
	for (i = 0; i < CLI_SLOTS; i++)
		ramon_buf_free(&cl->slot[i], NULL);
	ramon_close(cl->c);
	cl->c = NULL;
}

int cli_u64(struct cli *cl, const char *s, const char *what, uint64_t *v)
{
	if (!tc_parse_u64(s, v))
		return 0;
	return cli_err(cl, EINVAL, "%s: \"%s\" is not a number (decimal, or 0x.. hex)", what, s);
}

int cli_u32(struct cli *cl, const char *s, const char *what, uint32_t *v)
{
	if (!tc_parse_u32(s, v))
		return 0;
	return cli_err(cl, EINVAL, "%s: \"%s\" is not a 32-bit number (decimal, or 0x.. hex)", what,
		       s);
}

int cli_opt_u32(struct cli *cl, int argc, char **argv, int i, uint32_t def, const char *what,
		uint32_t *v)
{
	*v = def;
	return i < argc ? cli_u32(cl, argv[i], what, v) : 0;
}

int cli_nn(struct cli *cl, uint32_t nn)
{
	if (nn < RAMON_NN_COUNT)
		return 0;
	return cli_err(cl, EINVAL, "nn must be 0 or 1");
}

int cli_slot(struct cli *cl, const char *s, ramon_buf **b)
{
	uint32_t i;
	int ret = cli_u32(cl, s, "slot", &i);

	if (ret)
		return ret;
	if (i >= CLI_SLOTS || !cl->slot[i].handle)
		return cli_err(cl, EINVAL, "slot %u is empty (see \"bufinfo\")", i);
	*b = &cl->slot[i];
	return 0;
}

int cli_intr(void)
{
	return cli_interrupted != 0;
}

static void on_sigint(int sig)
{
	struct timespec ts;

	(void)sig;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (cli_interrupted && ts.tv_sec - last_intr_sec <= 1) {
		static const char msg[] = "\nramon_cli: interrupted twice, exiting\n";

		if (write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0)
			_exit(130);
		_exit(130);
	}
	cli_interrupted = 1;
	last_intr_sec = (sig_atomic_t)ts.tv_sec;
}

/* ---- general commands ---- */

static const struct cli_cmd *find_exact(const char *name)
{
	const struct cli_cmd *k;
	size_t g;

	for (g = 0; g < ARRAY_SIZE(groups); g++)
		for (k = groups[g].cmds; k->name; k++)
			if (!strcmp(k->name, name))
				return k;
	return NULL;
}

/* exact name, else a unique prefix of a listed command */
static const struct cli_cmd *find_cmd(const char *name, int *ambiguous)
{
	const struct cli_cmd *k, *hit = NULL;
	size_t g, n = strlen(name);
	int count = 0;

	*ambiguous = 0;
	k = find_exact(name);
	if (k)
		return k;
	for (g = 0; g < ARRAY_SIZE(groups); g++)
		for (k = groups[g].cmds; k->name; k++)
			if (!(k->flags & CMD_HIDDEN) && !strncmp(k->name, name, n)) {
				hit = k;
				count++;
			}
	if (count > 1) {
		*ambiguous = 1;
		return NULL;
	}
	return hit;
}

static void print_usage(const struct cli_cmd *k)
{
	printf("  %-16s %s%s\n", k->name, k->usage,
	       k->flags & CMD_DESTRUCTIVE ? (k->usage[0] ? " yes" : "yes") : "");
	printf("  %-16s %s\n", "", k->help);
}

static int cmd_help(struct cli *cl, int argc, char **argv)
{
	const struct cli_cmd *k;
	size_t g;
	int amb;

	(void)cl;
	if (argc > 1) {
		k = find_cmd(argv[1], &amb);
		if (!k)
			return cli_err(cl, EINVAL, "no command \"%s\"%s", argv[1],
				       amb ? " (ambiguous prefix)" : "");
		print_usage(k);
		return 0;
	}
	printf("Numbers: decimal, 0x.. hex, 0.. octal. TAB completes, \"help <cmd>\" shows one command.\n");
	printf("Destructive commands need a final \"yes\". Ctrl-C interrupts a waiting command; twice exits.\n");
	for (g = 0; g < ARRAY_SIZE(groups); g++) {
		printf("\n%s:\n", groups[g].title);
		for (k = groups[g].cmds; k->name; k++) {
			char args[96];

			if (k->flags & CMD_HIDDEN)
				continue;
			snprintf(args, sizeof(args), "%s%s", k->usage,
				 k->flags & CMD_DESTRUCTIVE ? (k->usage[0] ? " yes" : "yes") : "");
			if (strlen(args) > 30)
				printf("  %-16s %s\n  %-16s %-30s %s\n", k->name, args, "", "", k->help);
			else
				printf("  %-16s %-30s %s\n", k->name, args, k->help);
		}
	}
	return 0;
}

static int cmd_quit(struct cli *cl, int argc, char **argv)
{
	(void)cl;
	(void)argc;
	(void)argv;
	quit_requested = 1;
	return 0;
}

static int cmd_echo(struct cli *cl, int argc, char **argv)
{
	int i;

	(void)cl;
	for (i = 1; i < argc; i++)
		printf("%s%s", argv[i], i + 1 < argc ? " " : "");
	printf("\n");
	return 0;
}

static int cmd_sleep(struct cli *cl, int argc, char **argv)
{
	uint32_t ms;
	int ret = cli_u32(cl, argv[1], "ms", &ms);

	(void)argc;
	if (!ret)
		tc_sleep_ms(ms);
	return ret;
}

static const struct cli_cmd cli_main_cmds[N_MAIN_CMDS] = {
	{ "help", cmd_help, 0, 1, CMD_NO_DEV, "[cmd]", "list the commands, or show one" },
	{ "quit", cmd_quit, 0, 0, CMD_NO_DEV, "", "leave ramon_cli (also exit, exitapp)" },
	{ "exit", cmd_quit, 0, 0, CMD_NO_DEV | CMD_HIDDEN, "", "leave ramon_cli" },
	{ "exitapp", cmd_quit, 0, 0, CMD_NO_DEV | CMD_HIDDEN, "", "leave ramon_cli" },
	{ "echo", cmd_echo, 0, CLI_MAX_ARGS, CMD_NO_DEV, "[text...]", "print the text (for scripts)" },
	{ "sleep", cmd_sleep, 1, 1, CMD_NO_DEV, "<ms>", "pause (for scripts)" },
	{ NULL, NULL, 0, 0, 0, NULL, NULL },
};

/* ---- line handling ---- */

/* splits line in place: blanks separate words, "..." quotes, # starts a comment */
static int tokenize(char *line, char **argv)
{
	int argc = 0;
	char *p = line, *out;

	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (!*p || *p == '#')
			break;
		if (argc == CLI_MAX_ARGS)
			return -1;
		argv[argc++] = out = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
			if (*p == '"') {
				p++;
				while (*p && *p != '"')
					*out++ = *p++;
				if (*p == '"')
					p++;
			} else {
				*out++ = *p++;
			}
		}
		if (*p)
			p++;
		*out = 0;
	}
	return argc;
}

/* runs one command line; 0 ok (or empty), < 0 failed */
static int run_line(struct cli *cl, char *line)
{
	char *argv[CLI_MAX_ARGS + 1];
	const struct cli_cmd *k;
	char msg[256];
	int argc, nargs, amb, ret;

	argc = tokenize(line, argv);
	if (argc < 0) {
		printf("FAIL: more than %d words\n", CLI_MAX_ARGS);
		return -E2BIG;
	}
	if (!argc)
		return 0;
	argv[argc] = NULL;
	k = find_cmd(argv[0], &amb);
	if (!k) {
		printf("FAIL: %s command \"%s\"; \"help\" lists them\n",
		       amb ? "ambiguous" : "unknown", argv[0]);
		return -EINVAL;
	}
	nargs = argc - 1;
	if (k->flags & CMD_DESTRUCTIVE) {
		if (!nargs || strcmp(argv[argc - 1], "yes")) {
			printf("FAIL: %s is destructive: repeat it with \"yes\" as the last word\n",
			       k->name);
			return -EPERM;
		}
		argv[--argc] = NULL;
		nargs--;
	}
	if (nargs < k->min_args || nargs > k->max_args) {
		printf("FAIL: usage:\n");
		print_usage(k);
		return -EINVAL;
	}
	memset(&cl->st, 0, sizeof(cl->st));
	cli_interrupted = 0;
	ret = 0;
	if (!(k->flags & CMD_NO_DEV))
		ret = cli_ctx(cl);
	if (!ret && (k->flags & CMD_NEED_SPW))
		ret = cli_spw(cl);
	if (!ret)
		ret = k->fn(cl, argc, argv);
	if (!ret) {
		if (!quit_requested)
			printf("ok: %s\n", k->name);
	} else if (ret == -EINTR || cli_interrupted) {
		printf("FAIL: %s: interrupted\n", k->name);
	} else if (cl->st.code || cl->st.err) {
		ramon_strerror(&cl->st, msg, sizeof(msg));
		printf("FAIL: %s: %s\n", k->name, msg);
	} else {
		printf("FAIL: %s: %s\n", k->name, strerror(-ret));
	}
	fflush(stdout);
	cli_interrupted = 0;
	return ret;
}

/* ---- completion ---- */

static char win_names[64][RAMON_NAME_LEN];
static unsigned n_win_names;

static void cache_windows(struct cli *cl)
{
	struct ramon_regwin_info ri;
	uint32_t i;

	n_win_names = 0;
	if (!cl->c)
		return;
	for (i = 0; i < ramon_info(cl->c)->n_regwin && n_win_names < ARRAY_SIZE(win_names); i++) {
		if (ramon_regwin_info(cl->c, i, &ri, NULL) || (ri.flags & RAMON_REGWIN_ABSENT))
			continue;
		ri.name[RAMON_NAME_LEN - 1] = 0;
		ri.name[strcspn(ri.name, "@")] = 0;
		strcpy(win_names[n_win_names++], ri.name);
	}
}

static void add_completion(linenoiseCompletions *lc, const char *buf, size_t word_start,
			   const char *word)
{
	char line[LINE_MAX_LEN];

	if (word_start + strlen(word) + 2 > sizeof(line))
		return;
	memcpy(line, buf, word_start);
	strcpy(line + word_start, word);
	strcat(line, " ");
	linenoiseAddCompletion(lc, line);
}

static void completion(const char *buf, linenoiseCompletions *lc)
{
	const struct cli_cmd *k;
	char first[64];
	size_t start, len = strlen(buf), n;
	unsigned i;
	size_t g;
	int word = 0;

	/* which word is being completed, and where it starts */
	for (i = 0, start = 0; i < len; i++)
		if (buf[i] == ' ' && (i + 1 == len || buf[i + 1] != ' ')) {
			word++;
			start = i + 1;
		}
	while (start < len && buf[start] == ' ')
		start++;
	n = len - start;
	if (word == 0) {
		for (g = 0; g < ARRAY_SIZE(groups); g++)
			for (k = groups[g].cmds; k->name; k++)
				if (!(k->flags & CMD_HIDDEN) && !strncmp(k->name, buf + start, n))
					add_completion(lc, buf, start, k->name);
		return;
	}
	if (sscanf(buf, "%63s", first) != 1)
		return;
	if (word == 1 && !strcmp(first, "help")) {
		for (g = 0; g < ARRAY_SIZE(groups); g++)
			for (k = groups[g].cmds; k->name; k++)
				if (!(k->flags & CMD_HIDDEN) && !strncmp(k->name, buf + start, n))
					add_completion(lc, buf, start, k->name);
		return;
	}
	if (word == 1 && (!strcmp(first, "regio") || !strcmp(first, "regaccessioname"))) {
		for (i = 0; i < n_win_names; i++)
			if (!strncmp(win_names[i], buf + start, n))
				add_completion(lc, buf, start, win_names[i]);
		return;
	}
	k = find_exact(first);
	if (k && (k->flags & CMD_DESTRUCTIVE) && !strncmp("yes", buf + start, n))
		add_completion(lc, buf, start, "yes");
}

static char *hints(const char *buf, int *color, int *bold)
{
	static char hint[160];
	const struct cli_cmd *k;
	char name[64];
	size_t len = strlen(buf);

	if (!len || buf[len - 1] != ' ' || sscanf(buf, "%63s", name) != 1)
		return NULL;
	/* only right after "<command> " */
	if (strlen(name) + 1 != len)
		return NULL;
	k = find_exact(name);
	if (!k || (!k->usage[0] && !(k->flags & CMD_DESTRUCTIVE)))
		return NULL;
	snprintf(hint, sizeof(hint), "%s%s", k->usage,
		 k->flags & CMD_DESTRUCTIVE ? (k->usage[0] ? " yes" : "yes") : "");
	*color = 90;
	*bold = 0;
	return hint;
}

/* ---- modes ---- */

/*
 * A serial console often reports 0 columns. linenoise would then ask the
 * terminal for the cursor position and wait for an answer a plain serial
 * tool never sends; give it a width instead ($COLUMNS, else 80).
 */
static void set_width(void)
{
	struct winsize ws;
	const char *cols = getenv("COLUMNS");

	if (getenv("LINENOISE_COLS"))
		return;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col)
		return;
	setenv("LINENOISE_COLS", cols && atoi(cols) > 0 ? cols : "80", 1);
}

/* --plain: a prompt and ordinary line input, for terminals without escape sequences */
static int plain_repl(struct cli *cl)
{
	char line[LINE_MAX_LEN];
	int last = 0;

	while (!quit_requested) {
		printf("ramon> ");
		fflush(stdout);
		if (!fgets(line, sizeof(line), stdin)) {
			if (ferror(stdin) && errno == EINTR) {
				clearerr(stdin);
				cli_interrupted = 0;
				printf("\n");
				continue;
			}
			break;
		}
		line[strcspn(line, "\r\n")] = 0;
		last = run_line(cl, line);
	}
	return last ? 1 : 0;
}

static int repl(struct cli *cl)
{
	char path[512];
	const char *home = getenv("HOME");
	char *line;
	int last = 0;

	set_width();
	path[0] = 0;
	if (home)
		snprintf(path, sizeof(path), "%s/%s", home, HISTORY_FILE);
	linenoiseSetCompletionCallback(completion);
	linenoiseSetHintsCallback(hints);
	linenoiseHistorySetMaxLen(HISTORY_LEN);
	if (path[0])
		linenoiseHistoryLoad(path);
	cache_windows(cl);
	while (!quit_requested) {
		errno = 0;
		line = linenoise("ramon> ");
		if (!line) {
			if (errno == EAGAIN) {	/* Ctrl-C at the prompt */
				cli_interrupted = 0;
				continue;
			}
			break;			/* Ctrl-D */
		}
		if (line[strspn(line, " \t")]) {
			linenoiseHistoryAdd(line);
			last = run_line(cl, line);
			if (!n_win_names)
				cache_windows(cl);
		}
		linenoiseFree(line);
	}
	if (path[0])
		linenoiseHistorySave(path);
	return last ? 1 : 0;
}

static int script(struct cli *cl, FILE *f, int keep_going, int echo)
{
	char line[LINE_MAX_LEN];
	int failed = 0, n = 0;

	cl->script = 1;
	while (!quit_requested && fgets(line, sizeof(line), f)) {
		n++;
		line[strcspn(line, "\r\n")] = 0;
		if (!line[strspn(line, " \t")] || line[strspn(line, " \t")] == '#')
			continue;
		if (echo)
			printf("> %s\n", line);
		if (run_line(cl, line)) {
			failed++;
			if (!keep_going) {
				printf("stopped at line %d (use -k to keep going)\n", n);
				break;
			}
		}
	}
	return failed ? 1 : 0;
}

static void usage(FILE *f)
{
	fprintf(f,
		"usage: ramon_cli [options]                 interactive\n"
		"       ramon_cli [options] <cmd> [args]    one command\n"
		"       ramon_cli [options] -f <file|->     a script, one command per line\n"
		"options:\n"
		"  -d, --dev PATH         device (default %s)\n"
		"      --node N           our SPW node id (default %u)\n"
		"      --target N         the NN's SPW node id (default 0x%x)\n"
		"      --spfi-chans A,B   AXI write channel of SPFI NN0,NN1 (default %u,%u)\n"
		"      --nn N             current NN of the spfi* commands (default 0)\n"
		"  -f, --file FILE        run a script (\"-\" = stdin)\n"
		"  -k, --keep-going       scripts: continue after a failed command\n"
		"  -q, --quiet            scripts: do not echo the commands\n"
		"      --plain            interactive without line editing (dumb terminals)\n"
		"  -V, --version          print the versions and exit\n"
		"  -h, --help             this text; \"ramon_cli help\" lists the commands\n",
		RAMON_DEV_PATH, RAMON_SPW_NODE_DEFAULT, RAMON_SPW_NN_NODE_DEFAULT,
		RAMON_SPFI_CHAN_NN0, RAMON_SPFI_CHAN_NN1);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "dev", required_argument, NULL, 'd' },
		{ "node", required_argument, NULL, 1 },
		{ "target", required_argument, NULL, 2 },
		{ "spfi-chans", required_argument, NULL, 3 },
		{ "nn", required_argument, NULL, 4 },
		{ "file", required_argument, NULL, 'f' },
		{ "keep-going", no_argument, NULL, 'k' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "plain", no_argument, NULL, 5 },
		{ "version", no_argument, NULL, 'V' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct sigaction sa;
	struct cli cl;
	const char *file = NULL;
	char line[LINE_MAX_LEN], ver[160];
	int opt, keep_going = 0, echo = 1, plain = 0, ret, i;
	uint32_t v;

	memset(&cl, 0, sizeof(cl));
	cl.node = RAMON_SPW_NODE_DEFAULT;
	cl.target = RAMON_SPW_NN_NODE_DEFAULT;
	cl.spfi_chan[0] = RAMON_SPFI_CHAN_NN0;
	cl.spfi_chan[1] = RAMON_SPFI_CHAN_NN1;
	/* "+": stop at the first non-option, which is the command */
	while ((opt = getopt_long(argc, argv, "+d:f:kqVh", opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			cl.dev = optarg;
			break;
		case 1:
		case 2:
			if (tc_parse_u32(optarg, &v) || v > 255) {
				fprintf(stderr, "ramon_cli: bad node id \"%s\"\n", optarg);
				return 2;
			}
			if (opt == 1)
				cl.node = (uint8_t)v;
			else
				cl.target = (uint8_t)v;
			break;
		case 3:
			if (tc_parse_pair(optarg, cl.spfi_chan)) {
				fprintf(stderr, "ramon_cli: --spfi-chans wants A,B\n");
				return 2;
			}
			break;
		case 4:
			if (tc_parse_u32(optarg, &cl.nn) || cl.nn >= RAMON_NN_COUNT) {
				fprintf(stderr, "ramon_cli: --nn must be 0 or 1\n");
				return 2;
			}
			break;
		case 'f':
			file = optarg;
			break;
		case 'k':
			keep_going = 1;
			break;
		case 'q':
			echo = 0;
			break;
		case 5:
			plain = 1;
			break;
		case 'V':
			ramon_version_line(NULL, ver, sizeof(ver));
			printf("ramon_cli %s, %s\n", RAMON_TOOLS_VERSION, ver);
			return 0;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigint;	/* no SA_RESTART: a blocked wait returns EINTR */
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);

	if (optind < argc) {
		/* one command: rebuild the line, quoting words with blanks */
		line[0] = 0;
		for (i = optind; i < argc; i++) {
			int quote = strpbrk(argv[i], " \t") != NULL;

			if (strlen(line) + strlen(argv[i]) + 4 > sizeof(line)) {
				fprintf(stderr, "ramon_cli: command line too long\n");
				return 2;
			}
			strcat(line, quote ? "\"" : "");
			strcat(line, argv[i]);
			strcat(line, quote ? "\" " : " ");
		}
		cl.script = 1;
		ret = run_line(&cl, line) ? 1 : 0;
		cli_close_ctx(&cl);
		return ret;
	}
	if (file) {
		FILE *f = strcmp(file, "-") ? fopen(file, "r") : stdin;

		if (!f) {
			fprintf(stderr, "ramon_cli: %s: %s\n", file, strerror(errno));
			return 2;
		}
		ret = script(&cl, f, keep_going, echo);
		if (f != stdin)
			fclose(f);
		cli_close_ctx(&cl);
		return ret;
	}
	if (!isatty(STDIN_FILENO)) {
		ret = script(&cl, stdin, keep_going, echo);
		cli_close_ctx(&cl);
		return ret;
	}

	if (!cli_ctx(&cl)) {
		ramon_version_line(cl.c, ver, sizeof(ver));
		printf("ramon_cli %s, %s. \"help\" lists the commands.\n", RAMON_TOOLS_VERSION, ver);
	} else {
		char msg[256];

		ramon_strerror(&cl.st, msg, sizeof(msg));
		ramon_version_line(NULL, ver, sizeof(ver));
		printf("ramon_cli %s, %s. The device is not open: %s\n", RAMON_TOOLS_VERSION, ver, msg);
	}
	ret = plain ? plain_repl(&cl) : repl(&cl);
	cli_close_ctx(&cl);
	return ret;
}

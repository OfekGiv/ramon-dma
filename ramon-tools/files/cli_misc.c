// SPDX-License-Identifier: MIT
/* ramon_cli: board commands that do not use ramon_dma (Lattice SPI, scripts, /proc) */
#include "cli.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define LATTICE_SPI	"/dev/spidev2.0"
#define LATTICE_HZ	10000000u
#define LATTICE_FRAME	8
#define FWUPDATE_TOOL	"/apps/fwupdate/fwupdate"
#define SCRUB_SCRIPT	"/home/root/scrubbing/Scrubbing-tests4.sh"
#define SCRUB_MAX	1024

static int cmd_meminfo(struct cli *cl, int argc, char **argv)
{
	static const char *const keys[] = {
		"MemTotal", "MemFree", "MemAvailable", "CmaTotal", "CmaFree",
	};
	unsigned i;

	(void)cl;
	(void)argc;
	(void)argv;
	for (i = 0; i < ARRAY_SIZE(keys); i++)
		printf("      %-13s %ld kB\n", keys[i], tc_meminfo_kb(keys[i]));
	return 0;
}

static int cmd_devtree(struct cli *cl, int argc, char **argv)
{
	char path[320], node[256];
	uint8_t *reg;
	size_t len, i;
	int ret;

	(void)argc;
	if (strchr(argv[1], '/'))
		return cli_err(cl, EINVAL, "a symbol name, not a path");
	snprintf(path, sizeof(path), "/proc/device-tree/__symbols__/%s", argv[1]);
	ret = tc_read_line(path, node, sizeof(node));
	if (ret)
		return cli_err(cl, -ret, "%s: %s", path, strerror(-ret));
	printf("      %s -> %s\n", argv[1], node);
	snprintf(path, sizeof(path), "/proc/device-tree%s/reg", node);
	if (!tc_read_file(path, &reg, &len, 4096)) {
		printf("      reg:");
		for (i = 0; i + 4 <= len; i += 4)
			printf(" 0x%08x", (unsigned)reg[i] << 24 | (unsigned)reg[i + 1] << 16 |
			       (unsigned)reg[i + 2] << 8 | reg[i + 3]);
		printf("\n");
		free(reg);
	}
	return 0;
}

/* one 8-byte full-duplex frame to the Lattice FPGA (mode 0, 8 bits, 10 MHz) */
static int lattice_xfer(struct cli *cl, const uint8_t tx[LATTICE_FRAME], uint8_t rx[LATTICE_FRAME])
{
	struct spi_ioc_transfer t;
	uint32_t speed = LATTICE_HZ;
	uint8_t mode = SPI_MODE_0, bits = 8;
	int fd, ret = 0;

	fd = open(LATTICE_SPI, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return cli_err(cl, errno, "%s: %s", LATTICE_SPI, strerror(errno));
	memset(&t, 0, sizeof(t));
	t.tx_buf = (uintptr_t)tx;
	t.rx_buf = (uintptr_t)rx;
	t.len = LATTICE_FRAME;
	t.speed_hz = speed;
	t.bits_per_word = bits;
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 || ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0)
		ret = cli_err(cl, errno, "%s: %s", LATTICE_SPI, strerror(errno));
	close(fd);
	return ret;
}

static int cmd_rdlatver(struct cli *cl, int argc, char **argv)
{
	uint8_t tx[LATTICE_FRAME] = { 0 }, rx[LATTICE_FRAME] = { 0 };
	int ret;

	(void)argc;
	(void)argv;
	ret = lattice_xfer(cl, tx, rx);
	if (!ret)
		printf("      Lattice version: %u.%u\n", rx[6], rx[7]);
	return ret;
}

static int cmd_wrlatreg(struct cli *cl, int argc, char **argv)
{
	uint8_t tx[LATTICE_FRAME] = { 0x01, 0, 0, 0, 0x04, 0, 0, 0 }, rx[LATTICE_FRAME];
	uint32_t v;
	int ret;

	(void)argc;
	ret = cli_u32(cl, argv[1], "value", &v);
	if (!ret && v > 0xFFFF)
		ret = cli_err(cl, EINVAL, "the test register is 16 bits");
	if (ret)
		return ret;
	tx[5] = (uint8_t)(v >> 8);
	tx[6] = (uint8_t)v;
	return lattice_xfer(cl, tx, rx);
}

static int cmd_rdlatreg(struct cli *cl, int argc, char **argv)
{
	uint8_t tx[LATTICE_FRAME] = { 0x00, 0, 0, 0, 0x04, 0, 0, 0 }, rx[LATTICE_FRAME] = { 0 };
	int ret;

	(void)argc;
	(void)argv;
	ret = lattice_xfer(cl, tx, rx);
	if (!ret)
		printf("      Lattice test register: %u\n", (unsigned)rx[6] << 8 | rx[7]);
	return ret;
}

/* runs argv without a shell, echoing its output; the exit status is the result */
static int run_tool(struct cli *cl, char *const args[])
{
	int status;
	pid_t pid;

	fflush(stdout);
	pid = fork();
	if (pid < 0)
		return cli_err(cl, errno, "fork: %s", strerror(errno));
	if (!pid) {
		execv(args[0], args);
		fprintf(stderr, "%s: %s\n", args[0], strerror(errno));
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return cli_err(cl, errno, "waitpid: %s", strerror(errno));
	if (!WIFEXITED(status))
		return cli_err(cl, EIO, "%s did not exit normally", args[0]);
	if (WEXITSTATUS(status))
		return cli_err(cl, EIO, "%s exited with status %d", args[0], WEXITSTATUS(status));
	return 0;
}

static int cmd_fwupdate(struct cli *cl, int argc, char **argv)
{
	char *args[] = { (char *)FWUPDATE_TOOL, (char *)"Program", argv[1], NULL };

	(void)argc;
	if (access(argv[1], R_OK))
		return cli_err(cl, errno, "%s: %s", argv[1], strerror(errno));
	return run_tool(cl, args);
}

static int cmd_scrubtest(struct cli *cl, int argc, char **argv)
{
	char n_str[16];
	char *args[] = { (char *)SCRUB_SCRIPT, n_str, NULL };
	uint32_t n;
	int ret;

	(void)argc;
	ret = cli_u32(cl, argv[1], "blocks", &n);
	if (!ret && (!n || n > SCRUB_MAX))
		ret = cli_err(cl, EINVAL, "blocks must be 1..%u", SCRUB_MAX);
	if (ret)
		return ret;
	snprintf(n_str, sizeof(n_str), "%u", n);
	return run_tool(cl, args);
}

const struct cli_cmd cli_misc_cmds[] = {
	{ "meminfo", cmd_meminfo, 0, 0, CMD_NO_DEV, "", "memory and CMA from /proc/meminfo" },
	{ "devtree", cmd_devtree, 1, 1, CMD_NO_DEV, "<symbol>", "device-tree node and reg of a label" },
	{ "rdlatver", cmd_rdlatver, 0, 0, CMD_NO_DEV, "", "Lattice FPGA version over " LATTICE_SPI },
	{ "wrlatreg", cmd_wrlatreg, 1, 1, CMD_NO_DEV, "<val>", "write the Lattice test register" },
	{ "rdlatreg", cmd_rdlatreg, 0, 0, CMD_NO_DEV, "", "read the Lattice test register" },
	{ "fwupdate", cmd_fwupdate, 1, 1, CMD_NO_DEV | CMD_DESTRUCTIVE, "<file>",
	  "program FPGA firmware with " FWUPDATE_TOOL },
	{ "scrubtest", cmd_scrubtest, 1, 1, CMD_NO_DEV, "<blocks>", "run " SCRUB_SCRIPT },
	{ NULL, NULL, 0, 0, 0, NULL, NULL },
};

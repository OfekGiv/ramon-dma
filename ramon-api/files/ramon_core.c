// SPDX-License-Identifier: MIT
/* libramon core: context, errors, logging, one wrapper per ioctl */
#include "ramon_priv.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

const char *ramon_api_version(void)
{
	return RAMON_API_VERSION;
}

unsigned ramon_api_abi_version(void)
{
	return RAMON_ABI_VERSION;
}

/* ---- errors ---- */

struct lib_err {
	uint32_t code;
	int err;
	const char *name;
	const char *desc;
};

#define LIB_ERR_INFO(name, code, err, desc)	{ code, err, "RAMON_EL_" #name, desc },
static const struct lib_err lib_errs[] = {
	RAMON_LIB_ERR_LIST(LIB_ERR_INFO)
};

static const struct lib_err *lib_err_lookup(uint32_t code)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(lib_errs); i++)
		if (lib_errs[i].code == code)
			return &lib_errs[i];
	return NULL;
}

const char *ramon_err_str(uint32_t code)
{
	const struct lib_err *e;

	if (code < RAMON_E_LIB_BASE)
		return ramon_err_name(code);
	e = lib_err_lookup(code);
	return e ? e->name : "RAMON_EL_UNKNOWN";
}

const char *ramon_err_text(uint32_t code)
{
	const struct lib_err *e;

	if (code < RAMON_E_LIB_BASE)
		return ramon_err_desc(code);
	e = lib_err_lookup(code);
	return e ? e->desc : "unknown libramon error code";
}

size_t ramon_strerror(const struct ramon_status *st, char *buf, size_t len)
{
	char msg[RAMON_MSG_LEN + 1];
	int n;

	if (!st) {
		n = snprintf(buf, len, "no status");
		return n < 0 ? 0 : (size_t)n;
	}
	memcpy(msg, st->msg, RAMON_MSG_LEN);
	msg[RAMON_MSG_LEN] = 0;
	if (st->code == RAMON_E_OK && st->err == 0) {
		n = snprintf(buf, len, "%s%s", msg[0] ? "ok: " : "ok", msg);
		return n < 0 ? 0 : (size_t)n;
	}
	n = snprintf(buf, len, "%s (%s)%s%s", ramon_err_str(st->code), ramon_err_text(st->code),
		     msg[0] ? ": " : "", msg);
	if (n >= 0 && st->err && (size_t)n < len)
		n += snprintf(buf + n, len - n, " [errno %d]", -st->err);
	return n < 0 ? 0 : (size_t)n;
}

static void status_vset(struct ramon_status *st, uint32_t code, int err, uint64_t a0, uint64_t a1,
			const char *fmt, va_list ap)
{
	st->code = code;
	st->err = err;
	st->arg[0] = a0;
	st->arg[1] = a1;
	if (fmt)
		vsnprintf(st->msg, sizeof(st->msg), fmt, ap);
	else
		st->msg[0] = 0;
}

void ramon_status_set(struct ramon_status *st, uint32_t code, int err, uint64_t a0, uint64_t a1,
		      const char *fmt, ...)
{
	va_list ap;

	if (!st)
		return;
	va_start(ap, fmt);
	status_vset(st, code, err, a0, a1, fmt, ap);
	va_end(ap);
}

void ramon_status_clear(struct ramon_status *st)
{
	if (st)
		memset(st, 0, sizeof(*st));
}

int ramon__fail(struct ramon_status *st, uint32_t code, int err, uint64_t a0, uint64_t a1,
		const char *fmt, ...)
{
	const struct lib_err *e = lib_err_lookup(code);
	va_list ap;

	if (!err)
		err = e && e->err ? e->err : EIO;
	if (st) {
		va_start(ap, fmt);
		status_vset(st, code, -err, a0, a1, fmt, ap);
		va_end(ap);
	}
	return -err;
}

/* ---- small helpers ---- */

uint64_t ramon__now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void ramon__sleep_ms(uint32_t ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

void ramon__cond_init(pthread_cond_t *cv)
{
	pthread_condattr_t a;

	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	pthread_cond_init(cv, &a);
	pthread_condattr_destroy(&a);
}

void ramon__deadline(struct timespec *ts, uint32_t timeout_ms)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	ts->tv_sec += timeout_ms / 1000;
	ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

int ramon__thread_start(pthread_t *th, void *(*fn)(void *), void *arg)
{
	sigset_t all, old;
	int ret;

	/* library threads never take signals: only the caller's blocking call sees EINTR */
	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &old);
	ret = pthread_create(th, NULL, fn, arg);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	return -ret;
}

/* ---- logging ---- */

static const char *const level_names[] = { "error", "warning", "info", "debug" };

void ramon__log(struct ramon_ctx *c, int level, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	if (level > c->log_level)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (c->log_fn)
		c->log_fn(c->log_user, level, msg);
	else
		fprintf(stderr, "libramon %s: %s\n",
			level_names[level < 0 ? 0 : level > RAMON_LOG_DEBUG ? RAMON_LOG_DEBUG : level],
			msg);
}

void ramon_set_log(ramon_ctx *c, ramon_log_fn fn, void *user, int level)
{
	c->log_fn = fn;
	c->log_user = user;
	c->log_level = level;
}

/* ---- ioctls ---- */

int ramon__ioctl(struct ramon_ctx *c, unsigned long req, void *arg, struct ramon_status *trailer,
		 struct ramon_status *st)
{
	int err = 0;

	if (ioctl(c->fd, req, arg)) {
		err = errno;
		/* the driver writes no trailer for these two */
		if (err == ENOTTY || err == EFAULT)
			ramon_status_set(trailer, RAMON_EL_SYS, -err, req, 0, "ioctl 0x%lx: %s",
					 req, strerror(err));
		else if (trailer->err == 0)
			trailer->err = -err;
	}
	if (st)
		*st = *trailer;
	return -err;
}

int ramon_ioctl(ramon_ctx *c, unsigned long req, void *arg)
{
	return ioctl(c->fd, req, arg) ? -errno : 0;
}

#define IOC_WRAP(fn, req, type) \
	int ramon_ioc_##fn(ramon_ctx *c, struct type *a) \
	{ \
		return ramon__ioctl(c, req, a, &a->st, NULL); \
	}

IOC_WRAP(get_info, RAMON_IOC_GET_INFO, ramon_get_info)
IOC_WRAP(chan_info, RAMON_IOC_CHAN_INFO, ramon_chan_info)
IOC_WRAP(buf_alloc, RAMON_IOC_BUF_ALLOC, ramon_buf_alloc)
IOC_WRAP(buf_free, RAMON_IOC_BUF_FREE, ramon_buf_free)
IOC_WRAP(buf_info, RAMON_IOC_BUF_INFO, ramon_buf_info)
IOC_WRAP(axi_xfer, RAMON_IOC_AXI_XFER, ramon_axi_xfer)
IOC_WRAP(zdma_copy, RAMON_IOC_ZDMA_COPY, ramon_zdma_copy)
IOC_WRAP(spw_wait_rx, RAMON_IOC_SPW_WAIT_RX, ramon_spw_wait_rx)
IOC_WRAP(spw_cancel, RAMON_IOC_SPW_CANCEL, ramon_spw_cancel)
IOC_WRAP(spw_loopback, RAMON_IOC_SPW_LOOPBACK, ramon_spw_loopback)
IOC_WRAP(spfi_cmd, RAMON_IOC_SPFI_CMD, ramon_spfi_cmd)
IOC_WRAP(spfi_write, RAMON_IOC_SPFI_WRITE, ramon_spfi_write)
IOC_WRAP(spfi_read, RAMON_IOC_SPFI_READ, ramon_spfi_read)
IOC_WRAP(spfi_wait_alert, RAMON_IOC_SPFI_WAIT_ALERT, ramon_spfi_wait_alert)
IOC_WRAP(spfi_cancel_alert, RAMON_IOC_SPFI_CANCEL_ALERT, ramon_spfi_cancel_alert)
IOC_WRAP(spfi_mem_read, RAMON_IOC_SPFI_MEM_READ, ramon_spfi_mem_read)
IOC_WRAP(spfi_tx_offs_write, RAMON_IOC_SPFI_TX_OFFS_WRITE, ramon_spfi_tx_offs_write)
IOC_WRAP(regwin_info, RAMON_IOC_REGWIN_INFO, ramon_regwin_info)
IOC_WRAP(reg_io, RAMON_IOC_REG_IO, ramon_reg_io)
IOC_WRAP(get_stats, RAMON_IOC_GET_STATS, ramon_get_stats)

int ramon_get_info(ramon_ctx *c, struct ramon_get_info *gi, struct ramon_status *st)
{
	memset(gi, 0, sizeof(*gi));
	return ramon__ioctl(c, RAMON_IOC_GET_INFO, gi, &gi->st, st);
}

int ramon_chan_info(ramon_ctx *c, uint32_t index, struct ramon_chan_info *ci,
		    struct ramon_status *st)
{
	memset(ci, 0, sizeof(*ci));
	ci->index = index;
	return ramon__ioctl(c, RAMON_IOC_CHAN_INFO, ci, &ci->st, st);
}

int ramon_regwin_info(ramon_ctx *c, uint32_t index, struct ramon_regwin_info *ri,
		      struct ramon_status *st)
{
	memset(ri, 0, sizeof(*ri));
	ri->index = index;
	return ramon__ioctl(c, RAMON_IOC_REGWIN_INFO, ri, &ri->st, st);
}

int ramon_get_stats(ramon_ctx *c, int reset, struct ramon_get_stats *g, struct ramon_status *st)
{
	memset(g, 0, sizeof(*g));
	g->reset = reset ? 1 : 0;
	return ramon__ioctl(c, RAMON_IOC_GET_STATS, g, &g->st, st);
}

/* ---- context ---- */

#define VERSION_KEY(a, b, c)	((uint64_t)(a) << 40 | (uint64_t)(b) << 20 | (uint64_t)(c))

static int version_older(const struct ramon_get_info *gi)
{
	return VERSION_KEY(gi->drv_major, gi->drv_minor, gi->drv_patch) <
	       VERSION_KEY(RAMON_API_DRIVER_MIN_MAJOR, RAMON_API_DRIVER_MIN_MINOR,
			   RAMON_API_DRIVER_MIN_PATCH);
}

int ramon_open(const char *dev_path, ramon_ctx **out, struct ramon_status *st)
{
	struct ramon_ctx *c;
	int ret;

	*out = NULL;
	if (!dev_path)
		dev_path = RAMON_DEV_PATH;
	c = calloc(1, sizeof(*c));
	if (!c)
		return ramon__fail(st, RAMON_EL_SYS, ENOMEM, 0, 0, "out of memory");
	c->log_level = RAMON_LOG_WARN;
	c->fd = open(dev_path, O_RDWR | O_CLOEXEC);
	if (c->fd < 0) {
		ret = errno;
		free(c);
		return ramon__fail(st, RAMON_EL_SYS, ret, 0, 0, "open %s: %s", dev_path,
				   strerror(ret));
	}
	ret = ramon_get_info(c, &c->info, st);
	if (ret)
		goto err;
	if (c->info.abi_version != RAMON_ABI_VERSION) {
		ret = ramon__fail(st, RAMON_EL_ABI_MISMATCH, 0, c->info.abi_version,
				  RAMON_ABI_VERSION, "driver abi %u, libramon built for abi %u",
				  c->info.abi_version, RAMON_ABI_VERSION);
		goto err;
	}
	if (version_older(&c->info))
		ramon__log(c, RAMON_LOG_WARN, "driver %u.%u.%u is older than %s, which libramon %s expects",
			   c->info.drv_major, c->info.drv_minor, c->info.drv_patch,
			   RAMON_API_DRIVER_MIN, RAMON_API_VERSION);
	c->page_size = c->info.page_size;
	if (!c->page_size || (c->page_size & (c->page_size - 1))) {
		ret = ramon__fail(st, RAMON_EL_INVAL, EPROTO, c->page_size, 0,
				  "driver reports page size %zu", c->page_size);
		goto err;
	}
	while ((1ul << c->page_shift) < c->page_size)
		c->page_shift++;
	pthread_mutex_init(&c->lock, NULL);
	ramon__spw_ctx_init(c);
	ramon__spfi_ctx_init(c);
	*out = c;
	ramon_status_clear(st);
	return 0;
err:
	close(c->fd);
	free(c);
	return ret;
}

void ramon_close(ramon_ctx *c)
{
	if (!c)
		return;
	ramon_spw_fini(c);
	ramon_spfi_fini(c);
	ramon__spw_ctx_destroy(c);
	ramon__spfi_ctx_destroy(c);
	pthread_mutex_destroy(&c->lock);
	close(c->fd);
	free(c);
}

int ramon_fd(const ramon_ctx *c)
{
	return c->fd;
}

const struct ramon_get_info *ramon_info(const ramon_ctx *c)
{
	return &c->info;
}

size_t ramon_page_size(const ramon_ctx *c)
{
	return c->page_size;
}

int ramon_version_line(const ramon_ctx *c, char *buf, size_t len)
{
	if (!c)
		return snprintf(buf, len, "libramon %s (abi %u)", RAMON_API_VERSION,
				RAMON_ABI_VERSION);
	return snprintf(buf, len, "libramon %s (abi %u), driver %u.%u.%u (abi %u)",
			RAMON_API_VERSION, RAMON_ABI_VERSION, c->info.drv_major, c->info.drv_minor,
			c->info.drv_patch, c->info.abi_version);
}

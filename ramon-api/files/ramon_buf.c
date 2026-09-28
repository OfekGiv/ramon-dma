// SPDX-License-Identifier: MIT
/* libramon DMA buffers: BUF_ALLOC + mmap of handle << PAGE_SHIFT */
#include "ramon_priv.h"

#include <sys/mman.h>

uint64_t ramon_max_buf_bytes(const ramon_ctx *c)
{
	return c->info.max_buf_bytes;
}

int ramon_buf_map(ramon_buf *b, struct ramon_status *st)
{
	void *p;
	int err;

	if (!b->ctx || !b->handle)
		return ramon__fail(st, RAMON_EL_INVAL, 0, 0, 0, "buffer not allocated");
	if (b->ptr)
		return 0;
	p = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->ctx->fd,
		 (off_t)b->handle << b->ctx->page_shift);
	if (p == MAP_FAILED) {
		err = errno;
		return ramon__fail(st, RAMON_EL_SYS, err, b->handle, b->size,
				   "mmap of buffer %u (%llu bytes): %s", b->handle,
				   (unsigned long long)b->size, strerror(err));
	}
	b->ptr = p;
	return 0;
}

void ramon_buf_unmap(ramon_buf *b)
{
	if (b->ptr) {
		munmap(b->ptr, b->size);
		b->ptr = NULL;
	}
}

int ramon_buf_alloc(ramon_ctx *c, uint64_t size, unsigned flags, ramon_buf *b,
		    struct ramon_status *st)
{
	struct ramon_buf_alloc a;
	int ret;

	memset(b, 0, sizeof(*b));
	memset(&a, 0, sizeof(a));
	a.size = size;
	ret = ramon__ioctl(c, RAMON_IOC_BUF_ALLOC, &a, &a.st, st);
	if (ret)
		return ret;
	b->ctx = c;
	b->handle = a.handle;
	b->flags = flags;
	b->size = a.actual_size;
	if (flags & RAMON_BUF_NOMAP)
		return 0;
	ret = ramon_buf_map(b, st);
	if (ret) {
		struct ramon_buf_free f;

		memset(&f, 0, sizeof(f));
		f.handle = b->handle;
		ramon__ioctl(c, RAMON_IOC_BUF_FREE, &f, &f.st, NULL);
		memset(b, 0, sizeof(*b));
	}
	return ret;
}

int ramon_buf_free(ramon_buf *b, struct ramon_status *st)
{
	struct ramon_buf_free f;
	int ret;

	if (!b || !b->ctx || !b->handle)
		return 0;
	ramon_buf_unmap(b);
	memset(&f, 0, sizeof(f));
	f.handle = b->handle;
	ret = ramon__ioctl(b->ctx, RAMON_IOC_BUF_FREE, &f, &f.st, st);
	memset(b, 0, sizeof(*b));
	return ret;
}

int ramon_buf_dma_addr(const ramon_buf *b, uint64_t *dma_addr, struct ramon_status *st)
{
	struct ramon_buf_info i;
	int ret;

	if (!b->ctx || !b->handle)
		return ramon__fail(st, RAMON_EL_INVAL, 0, 0, 0, "buffer not allocated");
	memset(&i, 0, sizeof(i));
	i.handle = b->handle;
	ret = ramon__ioctl(b->ctx, RAMON_IOC_BUF_INFO, &i, &i.st, st);
	*dma_addr = ret ? 0 : i.dma_addr;
	return ret;
}

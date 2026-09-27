// SPDX-License-Identifier: GPL-2.0
/*
 * Per-file coherent DMA buffers: allocation, opaque handles, mmap.
 *
 * BUF_FREE only drops the handle-table reference. A buffer that is still
 * mapped or used by a transfer lives until that user drops its reference,
 * and all of a file's handles are dropped when the file is released.
 */
#define pr_fmt(fmt) "ramon_dma: " fmt

#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/srcu.h>

#include "ramon_dma.h"

static void ramon_buf_release(struct kref *kref)
{
	struct ramon_buf *buf = container_of(kref, struct ramon_buf, kref);
	struct ramon_dev *rd = buf->rd;

	dma_free_coherent(&rd->pdev->dev, buf->size, buf->cpu, buf->dma);
	kfree(buf);
	ramon_dev_put(rd);
}

/* May sleep: the last put frees the coherent memory. */
void ramon_buf_put(struct ramon_buf *buf)
{
	kref_put(&buf->kref, ramon_buf_release);
}

/* Returns the buffer with a reference held, or NULL if @handle is not one of @rf's. */
static struct ramon_buf *ramon_buf_get(struct ramon_file *rf, u32 handle)
{
	struct ramon_buf *buf;

	mutex_lock(&rf->lock);
	buf = idr_find(&rf->bufs, handle);
	if (buf)
		kref_get(&buf->kref);
	mutex_unlock(&rf->lock);
	return buf;
}

/**
 * ramon_buf_ref() - resolve one (handle, offset, len) operand of a transfer
 * @rf: caller's file; only its own buffers resolve
 * @handle: buffer handle
 * @off: byte offset into the buffer
 * @len: byte count, > 0
 * @what: operand kind for the message ("axi item", "zdma src" ...)
 * @i: operand index for the message
 * @range_code: failure code when the range is outside the buffer (normally RAMON_E_BUF_RANGE)
 * @bufp: set to the buffer, with a reference the caller must put
 * @st: status trailer
 *
 * Return: 0 or the negative errno from ramon_fail().
 */
int ramon_buf_ref(struct ramon_file *rf, u32 handle, u64 off, u64 len, const char *what, u32 i,
		  u32 range_code, struct ramon_buf **bufp, struct ramon_status *st)
{
	struct ramon_buf *buf;
	size_t size;
	u64 end;

	if (!len)
		return ramon_fail(st, RAMON_E_INVAL_ARG, i, handle, "%s %u: buf %u: len 0",
				  what, i, handle);
	buf = ramon_buf_get(rf, handle);
	if (!buf)
		return ramon_fail(st, RAMON_E_NO_SUCH_HANDLE, handle, i,
				  "%s %u: buf %u: no such handle on this fd", what, i, handle);
	size = buf->size;
	if (check_add_overflow(off, len, &end) || end > size) {
		ramon_buf_put(buf);
		return ramon_fail(st, range_code, off, len,
				  "%s %u: buf %u: offset 0x%llx + len 0x%llx > size 0x%zx",
				  what, i, handle, off, len, size);
	}
	*bufp = buf;
	return 0;
}

int ramon_ioc_buf_alloc(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_buf_alloc *p = arg;
	struct ramon_dev *rd = rf->rd;
	u64 max = ramon_max_buf_bytes();
	struct ramon_buf *buf;
	int id;

	if (!p->size)
		return ramon_fail(st, RAMON_E_INVAL_ARG, 0, max, "buf alloc: size 0");
	if (p->size > max)
		return ramon_fail(st, RAMON_E_BUF_TOO_LARGE, p->size, max,
				  "buf alloc: size 0x%llx > max_buf_mb limit 0x%llx", p->size, max);

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return ramon_fail(st, RAMON_E_NO_MEMORY, p->size, 0,
				  "buf alloc: no memory for the buffer descriptor");
	buf->size = PAGE_ALIGN(p->size);
	/* dma_alloc_coherent() returns zeroed memory */
	buf->cpu = dma_alloc_coherent(&rd->pdev->dev, buf->size, &buf->dma,
				      GFP_KERNEL | __GFP_NOWARN);
	if (!buf->cpu) {
		kfree(buf);
		return ramon_fail(st, RAMON_E_BUF_ALLOC_FAILED, p->size, 0,
				  "buf alloc: dma_alloc_coherent(0x%llx) failed, CMA exhausted or fragmented",
				  (u64)PAGE_ALIGN(p->size));
	}
	kref_init(&buf->kref);
	buf->rd = rd;
	ramon_dev_get(rd);

	mutex_lock(&rf->lock);
	id = idr_alloc_cyclic(&rf->bufs, buf, RAMON_BUF_HANDLE_MIN, RAMON_BUF_HANDLE_MAX + 1,
			      GFP_KERNEL);
	if (id >= 0)
		buf->handle = id;
	mutex_unlock(&rf->lock);

	if (id < 0) {
		ramon_buf_put(buf);
		if (id == -ENOSPC)
			return ramon_fail(st, RAMON_E_BAD_COUNT, RAMON_BUF_HANDLE_MAX, 0,
					  "buf alloc: this fd already holds %u buffers",
					  RAMON_BUF_HANDLE_MAX);
		return ramon_fail(st, RAMON_E_NO_MEMORY, p->size, 0,
				  "buf alloc: no memory for the handle (%d)", id);
	}

	p->handle = id;
	p->actual_size = buf->size;
	return 0;
}

int ramon_ioc_buf_free(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_buf_free *p = arg;
	struct ramon_buf *buf;

	mutex_lock(&rf->lock);
	buf = idr_remove(&rf->bufs, p->handle);
	mutex_unlock(&rf->lock);
	if (!buf)
		return ramon_fail(st, RAMON_E_NO_SUCH_HANDLE, p->handle, 0,
				  "buf %u: no such handle on this fd", p->handle);
	ramon_buf_put(buf);
	return 0;
}

int ramon_ioc_buf_info(struct ramon_file *rf, void *arg, struct ramon_status *st)
{
	struct ramon_buf_info *p = arg;
	struct ramon_buf *buf;

	buf = ramon_buf_get(rf, p->handle);
	if (!buf)
		return ramon_fail(st, RAMON_E_NO_SUCH_HANDLE, p->handle, 0,
				  "buf %u: no such handle on this fd", p->handle);
	p->size = buf->size;
	p->dma_addr = buf->dma;
	ramon_buf_put(buf);
	return 0;
}

/* vma copies (fork, split, mremap) each hold their own buffer reference. */
static void ramon_buf_vm_open(struct vm_area_struct *vma)
{
	struct ramon_buf *buf = vma->vm_private_data;

	kref_get(&buf->kref);
}

static void ramon_buf_vm_close(struct vm_area_struct *vma)
{
	ramon_buf_put(vma->vm_private_data);
}

static const struct vm_operations_struct ramon_buf_vm_ops = {
	.open	= ramon_buf_vm_open,
	.close	= ramon_buf_vm_close,
};

/* mmap(fd, len, ..., MAP_SHARED, handle << PAGE_SHIFT) maps the buffer from its start. */
int ramon_buf_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ramon_file *rf = file->private_data;
	struct ramon_dev *rd = rf->rd;
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long handle = vma->vm_pgoff;
	struct ramon_buf *buf;
	int idx, ret;

	if (!(vma->vm_flags & VM_SHARED))
		return ramon_fail_dbg(rd, RAMON_E_INVAL_ARG,
				      "mmap handle %lu: MAP_SHARED required", handle);
	if (handle < RAMON_BUF_HANDLE_MIN || handle > RAMON_BUF_HANDLE_MAX)
		return ramon_fail_dbg(rd, RAMON_E_NO_SUCH_HANDLE,
				      "mmap: page offset 0x%lx is not a buffer handle", handle);

	idx = srcu_read_lock(&rd->gate);
	if (READ_ONCE(rd->dead)) {
		ret = ramon_fail_dbg(rd, RAMON_E_REMOVED, "mmap handle %lu: device was removed",
				     handle);
		goto out;
	}
	buf = ramon_buf_get(rf, handle);
	if (!buf) {
		ret = ramon_fail_dbg(rd, RAMON_E_NO_SUCH_HANDLE,
				     "mmap handle %lu: no such handle on this fd", handle);
		goto out;
	}
	if (len > buf->size) {
		ret = ramon_fail_dbg(rd, RAMON_E_BUF_MMAP_LEN,
				     "mmap handle %lu: length 0x%lx > size 0x%zx",
				     handle, len, buf->size);
		goto out_put;
	}

	/* vm_pgoff selected the buffer; dma_mmap_coherent() takes it as an offset into it */
	vma->vm_pgoff = 0;
	ret = dma_mmap_coherent(&rd->pdev->dev, vma, buf->cpu, buf->dma, buf->size);
	if (ret) {
		dev_dbg(&rd->pdev->dev, "mmap handle %lu: dma_mmap_coherent failed: %d\n",
			handle, ret);
		goto out_put;
	}
	/* the reference from ramon_buf_get() now belongs to this vma */
	vma->vm_private_data = buf;
	vma->vm_ops = &ramon_buf_vm_ops;
	goto out;

out_put:
	ramon_buf_put(buf);
out:
	srcu_read_unlock(&rd->gate, idx);
	return ret;
}

/*
 * Drops every handle of a file being released. No mapping can remain (each
 * vma holds the file open), but a buffer may outlive this if a transfer on a
 * dup()ed fd still holds it.
 */
void ramon_buf_release_all(struct ramon_file *rf)
{
	struct ramon_buf *buf;
	int id;

	idr_for_each_entry(&rf->bufs, buf, id)
		ramon_buf_put(buf);
	idr_destroy(&rf->bufs);
}

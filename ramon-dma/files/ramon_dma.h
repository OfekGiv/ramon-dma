/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal objects, locking rules and cross-file prototypes of ramon_dma.
 *
 * Locking summary (details at each struct):
 * - ramon_dev.gate (SRCU): every ioctl and mmap runs inside a read section,
 *   including its sleeps. remove() sets ->dead, wakes every waiter, then
 *   synchronize_srcu()s, so no handler touches MMIO or a DMA channel after
 *   remove() starts releasing them. Every wait condition includes ->dead.
 *   SRCU rather than an rwsem: readers never block, so mmap (entered with
 *   mmap_lock held) cannot deadlock against an ioctl faulting on a user
 *   pointer while remove() waits.
 * - ramon_file.lock: protects only the handle idr; never held across a wait,
 *   a DMA or a user copy.
 * - ramon_axichan.lock: one per channel; never two at once.
 * - SPFI: write_lock / read_lock -> cmd_lock -> spfi.lock (spinlock, IRQ).
 * - IRQ handlers touch only spinlocked state, kfifos and atomics.
 */
#ifndef RAMON_DMA_H
#define RAMON_DMA_H

#include <linux/atomic.h>
#include <linux/compiler_attributes.h>
#include <linux/completion.h>
#include <linux/dmaengine.h>
#include <linux/idr.h>
#include <linux/ioport.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/stringify.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "ramon_board.h"

/* The minor number tracks the implementation step while the rewrite is in progress. */
#define RAMON_DRV_MAJOR		0
#define RAMON_DRV_MINOR		8
#define RAMON_DRV_PATCH		2
#define RAMON_DRV_VERSION	__stringify(RAMON_DRV_MAJOR) "." \
				__stringify(RAMON_DRV_MINOR) "." \
				__stringify(RAMON_DRV_PATCH)

struct ramon_dev;

/* All counters are atomics: written from IRQ, DMA callback and ioctl context. */
struct ramon_stats {
	atomic64_t axi_tx;
	atomic64_t axi_rx;
	atomic64_t axi_timeouts;
	atomic64_t zdma_ops;
	atomic64_t zdma_timeouts;
	atomic64_t spw_rx[RAMON_NN_COUNT];
	atomic64_t spw_overrun[RAMON_NN_COUNT];
	atomic64_t spfi_irq_hist[RAMON_NN_COUNT][RAMON_SPFI_IRQ_VECTORS];
	atomic64_t spfi_alert_overrun[RAMON_NN_COUNT];
	atomic64_t spfi_unexpected[RAMON_NN_COUNT];
	atomic64_t spfi_timeouts[RAMON_NN_COUNT];
};

/* One AXI DMA channel from the "dmas" list; index == DT position. */
struct ramon_axichan {
	struct dma_chan *chan;
	enum dma_transfer_direction dir;
	u32 device_id;			/* child "xlnx,device-id" */
	phys_addr_t phys;		/* axi_dma register base */
	const char *name;		/* "dma-names" entry (DT-owned string) */
	struct device_node *ip_node;	/* axi_dma IP; terminate resets the whole IP */
	struct mutex lock;		/* serializes transfers on this channel */
	struct completion done;		/* completed by the dmaengine callback */
};

struct ramon_zdma_chan {
	struct dma_chan *chan;
	struct completion done;		/* completed by the chunk's last descriptor */
	phys_addr_t phys;		/* ZDMA register base, for CHAN_INFO */
};

/* Pool of DMA_MEMCPY channels; @lock is never held across a copy. */
struct ramon_zdma {
	struct ramon_zdma_chan *ch;
	u32 n;
	spinlock_t lock;		/* guards free_mask */
	unsigned long free_mask;
	wait_queue_head_t wq;
};

/* One SPW instance (NN A = 0, NN B = 1). */
struct ramon_spw {
	struct ramon_dev *rd;
	u32 nn;
	void __iomem *regs;		/* NULL: instance absent */
	int irq;
	const char *node_name;		/* regwin name, "spw0@a0020000" */
	spinlock_t lock;		/* regs, rx_sizes, cancel_gen; taken in IRQ */
	DECLARE_KFIFO(rx_sizes, u32, RAMON_SPW_RX_FIFO_DEPTH);
	wait_queue_head_t wq;
	u32 cancel_gen;			/* bumped by SPW_CANCEL, under @lock */
};

struct ramon_spfi_alert {
	u32 code;
	u32 sub_code;
	u32 param1;
	u32 param2;
	u32 rx_status;
};

/* One SPFI instance. Lock order: write_lock / read_lock -> cmd_lock -> lock. */
struct ramon_spfi {
	struct ramon_dev *rd;
	u32 nn;
	void __iomem *regs;		/* NULL: instance absent */
	int irq;
	const char *node_name;
	void __iomem *tx_offs;		/* TX read-offset table, mapped at probe */
	u32 tx_offs_size;
	phys_addr_t rx_mem;		/* RX message table, mapped per MEM_READ */
	u32 rx_mem_size;
	spinlock_t lock;		/* regs and every field below; taken in IRQ */
	struct mutex cmd_lock;		/* TX register programming only, never across a wait */
	struct mutex write_lock;	/* one DATA_WRITE at a time */
	struct mutex read_lock;		/* one READ_STREAM / TX offset table user at a time */
	struct mutex short_lock;	/* one short command in flight, across its wait */
	struct ramon_buf *read_stale;	/* dst of a READ_STREAM that never completed; read_lock */
	bool cmd_pending;
	u32 cmd_rx_opcode;
	struct completion cmd_done;
	bool write_armed;
	struct completion write_done;
	bool read_armed;
	struct completion read_done;
	DECLARE_KFIFO(alerts, struct ramon_spfi_alert, RAMON_SPFI_ALERT_FIFO_DEPTH);
	wait_queue_head_t alert_wq;
	u32 alert_gen;			/* bumped by SPFI_CANCEL_ALERT, under @lock */
};

/* A bounded MMIO window for REG_IO; RAMON_REGWIN_ABSENT means base == NULL. */
struct ramon_regwin {
	char name[RAMON_NAME_LEN];
	void __iomem *base;
	u32 size;
	phys_addr_t phys;
	u32 flags;			/* RAMON_REGWIN_* */
	spinlock_t *lock;		/* taken around REG_IO if set (SPFI windows) */
};

/*
 * One bound platform device. Allocated in probe and freed by the last
 * kref_put: the bound driver holds one reference and each open file one.
 * Holds a reference on pdev->dev for its whole lifetime.
 */
struct ramon_dev {
	struct platform_device *pdev;
	struct miscdevice misc;
	struct kref kref;
	struct srcu_struct gate;	/* see the locking summary */
	bool dead;			/* set once by remove(); READ_ONCE/WRITE_ONCE */
	struct ramon_axichan *axi;
	u32 n_axi;
	struct ramon_zdma zdma;
	struct ramon_spw spw[RAMON_NN_COUNT];
	struct ramon_spfi spfi[RAMON_NN_COUNT];
	struct ramon_regwin *win;
	u32 n_win;
	struct ramon_stats stats;
};

/* Per open() state. */
struct ramon_file {
	struct ramon_dev *rd;
	struct mutex lock;		/* guards bufs only */
	struct idr bufs;
};

/*
 * A coherent buffer. References: the handle table, each vma mapping it and
 * each transfer using it; freed (possibly sleeping) by the last put. Each
 * buffer holds a reference on its ramon_dev.
 */
struct ramon_buf {
	u32 handle;			/* written once under ramon_file.lock */
	size_t size;			/* PAGE_ALIGNed */
	void *cpu;
	dma_addr_t dma;
	struct kref kref;
	struct ramon_dev *rd;
};

typedef int (*ramon_ioctl_fn)(struct ramon_file *rf, void *arg, struct ramon_status *st);

/* An AXI transfer between prepare and release; see ramon_axidma.c. */
struct ramon_axi_job {
	u32 chan;
	struct ramon_axichan *ch;
	struct ramon_buf **bufs;	/* one reference per item */
	struct scatterlist *sg;
	u32 n_bufs;
	u64 bytes;
};

/* ramon_core.c */
__printf(5, 6)
int ramon_fail(struct ramon_status *st, u32 code, u64 arg0, u64 arg1, const char *fmt, ...);
__printf(3, 4)
int ramon_fail_dbg(struct ramon_dev *rd, u32 code, const char *fmt, ...);
__printf(2, 3)
void ramon_note(struct ramon_status *st, const char *fmt, ...);
void ramon_dev_get(struct ramon_dev *rd);
void ramon_dev_put(struct ramon_dev *rd);
u64 ramon_max_buf_bytes(void);
u32 ramon_param_zdma_channels(void);
u32 ramon_param_spfi_write_gap_us(void);
u32 ramon_timeout_ms(u32 requested, u32 def);

/* ramon_of.c */
struct ramon_of_regdev {
	struct resource res;
	char name[RAMON_NAME_LEN];
	bool present;
};

/* Everything found by global DT search, gathered once at probe. */
struct ramon_of_hw {
	bool rstop_ok;
	struct resource rstop;
	u32 spw_mask;			/* bit nn: spw[nn] valid */
	struct resource spw[RAMON_NN_COUNT];
	int spw_irq[RAMON_NN_COUNT];	/* 0: none */
	bool spfi_ok;			/* both reg entries */
	struct resource spfi[RAMON_NN_COUNT];
	bool spfi_irq_ok;		/* both interrupts */
	int spfi_irq[RAMON_NN_COUNT];
	bool spfi_rx_mem_ok[RAMON_NN_COUNT];
	struct resource spfi_rx_mem[RAMON_NN_COUNT];
	bool spfi_tx_offs_ok[RAMON_NN_COUNT];
	struct resource spfi_tx_offs[RAMON_NN_COUNT];
	u32 n_regaccess;
	struct ramon_of_regdev regaccess[RAMON_REGACCESS_MAX];
};

struct ramon_of_axichan {
	const char *name;
	u32 device_id;
	enum dma_transfer_direction dir;
	phys_addr_t phys;
	struct device_node *ip_node;
};

void ramon_of_discover(struct device *dev, struct ramon_of_hw *hw);
int ramon_of_axi_count(struct device *dev);
int ramon_of_axi(struct device *dev, u32 i, struct ramon_of_axichan *out);

/* ramon_buf.c */
int ramon_ioc_buf_alloc(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_buf_free(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_buf_info(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_buf_mmap(struct file *file, struct vm_area_struct *vma);
void ramon_buf_release_all(struct ramon_file *rf);
int ramon_buf_ref(struct ramon_file *rf, u32 handle, u64 off, u64 len, const char *what, u32 i,
		  u32 range_code, struct ramon_buf **bufp, struct ramon_status *st);
void ramon_buf_put(struct ramon_buf *buf);

/* ramon_regwin.c */
int ramon_regwin_probe(struct ramon_dev *rd, const struct ramon_of_hw *hw);
int ramon_ioc_regwin_info(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_reg_io(struct ramon_file *rf, void *arg, struct ramon_status *st);

/* ramon_axidma.c */
const char *ramon_dma_status_name(enum dma_status status);
int ramon_axi_job_prepare(struct ramon_file *rf, u32 chan, u64 uitems, u32 n,
			  struct ramon_axi_job *job, struct ramon_status *st);
int ramon_axi_job_run(struct ramon_dev *rd, struct ramon_axi_job *job, u32 timeout_ms,
		      struct ramon_status *st);
void ramon_axi_job_release(struct ramon_axi_job *job);
int ramon_ioc_axi_xfer(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_chan_info(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_axidma_probe(struct ramon_dev *rd);
void ramon_axidma_wake(struct ramon_dev *rd);
void ramon_axidma_remove(struct ramon_dev *rd);

/* ramon_zdma.c */
int ramon_ioc_zdma_copy(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_zdma_chan_info(struct ramon_dev *rd, struct ramon_chan_info *p,
			 struct ramon_status *st);
int ramon_zdma_probe(struct ramon_dev *rd);
void ramon_zdma_wake(struct ramon_dev *rd);
void ramon_zdma_remove(struct ramon_dev *rd);

/* ramon_spw.c */
int ramon_ioc_spw_wait_rx(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_spw_cancel(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_spw_loopback(struct ramon_file *rf, void *arg, struct ramon_status *st);
void ramon_spw_probe(struct ramon_dev *rd, const struct ramon_of_hw *hw);
void ramon_spw_free_irqs(struct ramon_dev *rd);
void ramon_spw_wake(struct ramon_dev *rd);

/* ramon_spfi.c */
int ramon_ioc_spfi_cmd(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_spfi_write(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_spfi_read(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_spfi_wait_alert(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_spfi_cancel_alert(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_spfi_mem_read(struct ramon_file *rf, void *arg, struct ramon_status *st);
int ramon_ioc_spfi_tx_offs_write(struct ramon_file *rf, void *arg, struct ramon_status *st);
void ramon_spfi_probe(struct ramon_dev *rd, const struct ramon_of_hw *hw);
void ramon_spfi_free_irqs(struct ramon_dev *rd);
void ramon_spfi_wake(struct ramon_dev *rd);
void ramon_spfi_remove(struct ramon_dev *rd);

#endif /* RAMON_DMA_H */

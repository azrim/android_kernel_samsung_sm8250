// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 MediaTek Inc.
 *
 * kcompressd: offload zram page compression from kswapd to a set of kernel
 * daemons. Adapted from the out-of-tree MediaTek "kcompressd" RFC to the
 * Linux 4.19 zram rw_page() path.
 */

#define pr_fmt(fmt) "kcompressd: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/swap.h>

#include "kcompressd.h"

#define INIT_QUEUE_SIZE		256
#define DEFAULT_NR_KCOMPRESSD	4

static atomic_t enable_kcompressd;
static unsigned int nr_kcompressd = DEFAULT_NR_KCOMPRESSD;
static unsigned int queue_size_per_kcompressd = INIT_QUEUE_SIZE;
/* Round-robin cursor: each write starts scanning at a different daemon. */
static atomic_t kcompressd_next;

module_param(nr_kcompressd, uint, 0644);
MODULE_PARM_DESC(nr_kcompressd, "Number of daemons for page compression");
module_param(queue_size_per_kcompressd, uint, 0644);
MODULE_PARM_DESC(queue_size_per_kcompressd, "Size of queue for kcompressd");

enum run_state {
	KCOMPRESSD_NOT_STARTED = 0,
	KCOMPRESSD_RUNNING,
	KCOMPRESSD_SLEEPING,
};

struct write_work {
	void *mem;
	struct page *page;
	u32 index;
	int offset;
	compress_callback cb;
};

struct kcompress {
	struct task_struct *kcompressd;
	wait_queue_head_t kcompressd_wait;
	struct kfifo write_fifo;
	atomic_t running;
};

struct kcompressd_para {
	wait_queue_head_t *kcompressd_wait;
	struct kfifo *write_fifo;
	atomic_t *running;
};

static struct kcompress *kcompress;
static struct kcompressd_para *kcompressd_para;

int kcompressd_enabled(void)
{
	return likely(atomic_read(&enable_kcompressd));
}

static void kcompressd_try_to_sleep(struct kcompressd_para *p)
{
	DEFINE_WAIT(wait);

	if (!kfifo_is_empty(p->write_fifo))
		return;

	if (freezing(current) || kthread_should_stop())
		return;

	atomic_set(p->running, KCOMPRESSD_SLEEPING);
	prepare_to_wait(p->kcompressd_wait, &wait, TASK_INTERRUPTIBLE);

	/*
	 * After a short sleep, check if it was a premature sleep. If not, then
	 * go fully to sleep until explicitly woken up.
	 */
	if (!kthread_should_stop() && kfifo_is_empty(p->write_fifo))
		schedule();

	finish_wait(p->kcompressd_wait, &wait);
	atomic_set(p->running, KCOMPRESSD_RUNNING);
}

static void kcompressd_do_work(struct write_work *entry)
{
	entry->cb(entry->mem, entry->page, entry->index, entry->offset);
}

static int kcompressd_worker(void *para)
{
	struct task_struct *tsk = current;
	struct kcompressd_para *p = (struct kcompressd_para *)para;

	tsk->flags |= PF_MEMALLOC | PF_KSWAPD;
	set_freezable();

	while (!kthread_should_stop()) {
		bool ret;

		kcompressd_try_to_sleep(p);
		ret = try_to_freeze();
		if (kthread_should_stop())
			break;

		if (ret)
			continue;

		while (!kfifo_is_empty(p->write_fifo)) {
			struct write_work entry;

			if (sizeof(struct write_work) ==
			    kfifo_out(p->write_fifo, &entry,
				      sizeof(struct write_work)))
				kcompressd_do_work(&entry);
		}
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_KSWAPD);
	atomic_set(p->running, KCOMPRESSD_NOT_STARTED);
	return 0;
}

static int init_write_queue(void)
{
	int i;
	unsigned int queue_len = queue_size_per_kcompressd *
				 sizeof(struct write_work);

	for (i = 0; i < nr_kcompressd; i++) {
		if (kfifo_alloc(&kcompress[i].write_fifo, queue_len,
				GFP_KERNEL)) {
			pr_err("Failed to alloc kfifo %d\n", i);
			while (i--)
				kfifo_free(&kcompress[i].write_fifo);
			return -ENOMEM;
		}
	}
	return 0;
}

static void drain_write_queue(int idx)
{
	struct write_work entry;

	while (sizeof(struct write_work) ==
	       kfifo_out(&kcompress[idx].write_fifo, &entry,
			 sizeof(struct write_work)))
		kcompressd_do_work(&entry);
}

static void clean_write_queue(int idx)
{
	drain_write_queue(idx);
	kfifo_free(&kcompress[idx].write_fifo);
}

static void stop_all_kcompressd_thread(void)
{
	int i;

	for (i = 0; i < nr_kcompressd; i++) {
		if (kcompress[i].kcompressd)
			kthread_stop(kcompress[i].kcompressd);
		kcompress[i].kcompressd = NULL;
		clean_write_queue(i);
	}
}

int schedule_bio_write(void *mem, struct page *page, u32 index, int offset,
		       compress_callback cb)
{
	unsigned int i, idx, start;
	size_t sz_work = sizeof(struct write_work);
	struct write_work entry = {
		.mem = mem,
		.page = page,
		.index = index,
		.offset = offset,
		.cb = cb,
	};

	if (unlikely(!atomic_read(&enable_kcompressd)))
		return -EBUSY;

	if (!nr_kcompressd || !current_is_kswapd())
		return -EBUSY;

	/*
	 * Round-robin over the daemons rather than always filling the lowest
	 * index first. Starting each write at a different daemon spreads
	 * steady-state load across all of them instead of pinning it on
	 * kcompressd:0; a full queue just moves on to the next one.
	 */
	start = (unsigned int)atomic_inc_return(&kcompressd_next) % nr_kcompressd;

	for (i = 0; i < nr_kcompressd; i++) {
		idx = (start + i) % nr_kcompressd;

		if (kfifo_avail(&kcompress[idx].write_fifo) < sz_work)
			continue;

		/*
		 * Start the worker before queueing the entry.  The previous
		 * order queued first and, on kthread_run() failure, called
		 * kfifo_out() to "undo" the insert -- but kfifo_out() removes
		 * the *oldest* entry, so an unrelated page's callback was
		 * silently dropped and that page never got page_endio().
		 * Starting first means there is nothing to undo on failure.
		 *
		 * Claim the NOT_STARTED -> RUNNING transition with a cmpxchg
		 * so two concurrent writers cannot both kthread_run() and
		 * leave one worker untracked (surviving kcompressd_exit() and
		 * dereferencing freed state).
		 */
		if (atomic_read(&kcompress[idx].running) == KCOMPRESSD_NOT_STARTED) {
			if (atomic_cmpxchg(&kcompress[idx].running,
					   KCOMPRESSD_NOT_STARTED,
					   KCOMPRESSD_RUNNING) ==
			    KCOMPRESSD_NOT_STARTED) {
				kcompress[idx].kcompressd =
					kthread_run(kcompressd_worker,
						    &kcompressd_para[idx],
						    "kcompressd:%d", idx);
				if (IS_ERR(kcompress[idx].kcompressd)) {
					kcompress[idx].kcompressd = NULL;
					atomic_set(&kcompress[idx].running,
						   KCOMPRESSD_NOT_STARTED);
					pr_warn("Failed to start kcompressd:%d\n",
						idx);
					/* nothing queued: caller falls back */
					return -EBUSY;
				}
			}
		}

		if (kfifo_in(&kcompress[idx].write_fifo, &entry, sz_work) != sz_work)
			continue;

		/*
		 * Enqueue *before* waking: if the wake were issued first the
		 * worker could re-check an empty fifo and go to sleep forever,
		 * leaving this entry stranded.
		 */
		if (atomic_read(&kcompress[idx].running) == KCOMPRESSD_SLEEPING)
			wake_up_interruptible(&kcompress[idx].kcompressd_wait);

		return 0;
	}

	return -EBUSY;
}

int kcompressd_init(void)
{
	int i, ret;

	if (!nr_kcompressd)
		nr_kcompressd = DEFAULT_NR_KCOMPRESSD;
	if (!queue_size_per_kcompressd)
		queue_size_per_kcompressd = INIT_QUEUE_SIZE;

	kcompress = kvmalloc_array(nr_kcompressd, sizeof(struct kcompress),
				   GFP_KERNEL);
	if (!kcompress)
		return -ENOMEM;

	kcompressd_para = kvmalloc_array(nr_kcompressd,
			sizeof(struct kcompressd_para), GFP_KERNEL);
	if (!kcompressd_para)
		goto err_free;

	ret = init_write_queue();
	if (ret) {
		pr_err("Initialization of writing to FIFOs failed!!\n");
		goto err_free;
	}

	for (i = 0; i < nr_kcompressd; i++) {
		init_waitqueue_head(&kcompress[i].kcompressd_wait);
		kcompressd_para[i].kcompressd_wait =
			&kcompress[i].kcompressd_wait;
		kcompressd_para[i].write_fifo = &kcompress[i].write_fifo;
		kcompressd_para[i].running = &kcompress[i].running;
	}

	atomic_set(&kcompressd_next, 0);
	atomic_set(&enable_kcompressd, true);
	return 0;

err_free:
	kvfree(kcompress);
	kvfree(kcompressd_para);
	kcompress = NULL;
	kcompressd_para = NULL;
	return -ENOMEM;
}

void kcompressd_exit(void)
{
	atomic_set(&enable_kcompressd, false);
	if (kcompress)
		stop_all_kcompressd_thread();

	kvfree(kcompress);
	kvfree(kcompressd_para);
	kcompress = NULL;
	kcompressd_para = NULL;
}

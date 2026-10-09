// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 MediaTek Inc.
 *
 * kcompressd: offload zram page compression from direct reclaim and kswapd
 * to a dedicated set of background kernel daemons using lockless ring buffers.
 */

#define pr_fmt(fmt) "kcompressd: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/delay.h>

#include "kcompressd.h"

#define INIT_QUEUE_SIZE		256
#define DEFAULT_NR_KCOMPRESSD	4
#define KCOMPRESSD_BATCH_SIZE	16

static atomic_t enable_kcompressd;
static unsigned int nr_kcompressd = DEFAULT_NR_KCOMPRESSD;
static unsigned int queue_size_per_kcompressd = INIT_QUEUE_SIZE;
/* Round-robin cursor: each write starts scanning at a different daemon. */
static atomic_t kcompressd_next;

/*
 * Both are read-only after init: kcompress[] is sized once by
 * kcompressd_init() and there is no reallocation or synchronization for a
 * runtime change.  A larger value makes schedule_bio_write() index past the
 * array; a smaller one makes kcompressd_exit() skip (and then kvfree() from
 * under) the daemons above the new bound.  Set them on the kernel command
 * line (zram.nr_kcompressd=N) instead.
 */
module_param(nr_kcompressd, uint, 0444);
MODULE_PARM_DESC(nr_kcompressd, "Number of daemons for page compression");
module_param(queue_size_per_kcompressd, uint, 0444);
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
	unsigned int ready;
};

struct kcompress {
	struct task_struct *kcompressd;
	wait_queue_head_t kcompressd_wait;
	atomic_t head;
	unsigned int tail;
	atomic_t count;
	/*
	 * Number of dequeued entries whose callback has not finished yet.
	 * count is dropped at dequeue time, so it reaching zero only means
	 * "the ring is empty", not "the work is done".  kcompressd_flush()
	 * must also wait for in_flight to drain before the caller is allowed
	 * to free the zram table/pool/comp the callbacks touch.
	 */
	atomic_t in_flight;
	atomic_t running;
	unsigned int ring_size;
	unsigned int ring_mask;
	struct write_work *ring;
};

static struct kcompress *kcompress;

int kcompressd_enabled(void)
{
	return likely(atomic_read(&enable_kcompressd));
}

static void kcompressd_do_work(struct write_work *entry)
{
	entry->cb(entry->mem, entry->page, entry->index, entry->offset);
}

static int kcompressd_worker(void *para)
{
	struct task_struct *tsk = current;
	struct kcompress *kc = (struct kcompress *)para;

	tsk->flags |= PF_MEMALLOC;
	set_user_nice(tsk, 5);
	set_freezable();

	while (!kthread_should_stop()) {
		struct write_work batch[KCOMPRESSD_BATCH_SIZE];
		int nr_batch = 0;
		int i;

		if (kthread_should_park()) {
			kthread_parkme();
			continue;
		}

		if (try_to_freeze())
			continue;

		/* Dequeue a batch from the lockless ring buffer */
		while (nr_batch < KCOMPRESSD_BATCH_SIZE) {
			unsigned int tail = kc->tail;
			unsigned int slot = tail & kc->ring_mask;

			if (!smp_load_acquire(&kc->ring[slot].ready))
				break;

			batch[nr_batch].mem = kc->ring[slot].mem;
			batch[nr_batch].page = kc->ring[slot].page;
			batch[nr_batch].index = kc->ring[slot].index;
			batch[nr_batch].offset = kc->ring[slot].offset;
			batch[nr_batch].cb = kc->ring[slot].cb;

			WRITE_ONCE(kc->ring[slot].ready, 0);
			kc->tail = tail + 1;
			smp_wmb();
			/*
			 * Account the entry as in-flight before dropping the
			 * queue count, so a concurrent kcompressd_flush() can
			 * never observe count==0 with the callback still to
			 * run.
			 */
			atomic_inc(&kc->in_flight);
			atomic_dec(&kc->count);
			nr_batch++;
		}

		if (nr_batch > 0) {
			int done = 0;

			for (i = 0; i < nr_batch; i++) {
				kcompressd_do_work(&batch[i]);
				atomic_dec(&kc->in_flight);
				done++;

				if (kthread_should_stop() || kthread_should_park())
					break;

				/* Yield CPU immediately if higher priority tasks need it */
				if (need_resched())
					cond_resched();
			}

			/*
			 * If we bailed out early the remaining entries are
			 * dropped (as before); release their in-flight refs so
			 * kcompressd_flush() cannot wait on work that will
			 * never run.
			 */
			if (done < nr_batch)
				atomic_sub(nr_batch - done, &kc->in_flight);
			continue;
		}

		/* Sleep until new pages arrive, or stop/park/freeze is requested */
		atomic_set(&kc->running, KCOMPRESSD_SLEEPING);
		wait_event_freezable(kc->kcompressd_wait,
				     atomic_read(&kc->count) > 0 ||
				     kthread_should_stop() ||
				     kthread_should_park());
		atomic_set(&kc->running, KCOMPRESSD_RUNNING);
	}

	tsk->flags &= ~PF_MEMALLOC;
	atomic_set(&kc->running, KCOMPRESSD_NOT_STARTED);
	return 0;
}

int schedule_bio_write(void *mem, struct page *page, u32 index, int offset,
		       compress_callback cb)
{
	unsigned int i, idx, start;
	int ret = -EBUSY;

	if (unlikely(!atomic_read(&enable_kcompressd)))
		return -EBUSY;

	if (!nr_kcompressd || !kcompress)
		return -EBUSY;

	/* Round-robin across daemons to balance steady-state load */
	start = (unsigned int)atomic_inc_return(&kcompressd_next) % nr_kcompressd;

	for (i = 0; i < nr_kcompressd; i++) {
		struct kcompress *kc;
		unsigned int slot;

		idx = (start + i) % nr_kcompressd;
		kc = &kcompress[idx];

		if (unlikely(!kc->kcompressd))
			continue;

		/* Check queue capacity; if saturated, try next daemon or fall back */
		if (atomic_inc_return(&kc->count) > kc->ring_size) {
			atomic_dec(&kc->count);
			continue;
		}

		preempt_disable();
		slot = ((unsigned int)atomic_fetch_add(1, &kc->head)) & kc->ring_mask;
		kc->ring[slot].mem = mem;
		kc->ring[slot].page = page;
		kc->ring[slot].index = index;
		kc->ring[slot].offset = offset;
		kc->ring[slot].cb = cb;
		smp_store_release(&kc->ring[slot].ready, 1);
		preempt_enable();

		wake_up_interruptible(&kc->kcompressd_wait);
		return 0;
	}

	return ret;
}

void kcompressd_flush(void)
{
	int i;

	if (!kcompress)
		return;

	for (i = 0; i < nr_kcompressd; i++)
		wake_up_interruptible(&kcompress[i].kcompressd_wait);

	for (i = 0; i < nr_kcompressd; i++) {
		struct kcompress *kc = &kcompress[i];
		unsigned long timeout = jiffies + msecs_to_jiffies(1000);

		/*
		 * Wait for the ring to drain *and* for every callback that
		 * was already dequeued to finish.  Only then are the zram
		 * table/pool/comp safe to free.
		 */
		while ((atomic_read(&kc->count) > 0 ||
			atomic_read(&kc->in_flight) > 0) &&
		       time_before(jiffies, timeout)) {
			wake_up_interruptible(&kc->kcompressd_wait);
			usleep_range(500, 1000);
		}
	}
}

void kcompressd_exit(void)
{
	int i;

	atomic_set(&enable_kcompressd, false);
	smp_mb();

	if (!kcompress)
		return;

	for (i = 0; i < nr_kcompressd; i++) {
		if (kcompress[i].kcompressd) {
			kthread_stop(kcompress[i].kcompressd);
			kcompress[i].kcompressd = NULL;
		}

		if (kcompress[i].ring) {
			while (atomic_read(&kcompress[i].count) > 0) {
				unsigned int tail = kcompress[i].tail;
				unsigned int slot = tail & kcompress[i].ring_mask;
				struct write_work entry;

				if (!smp_load_acquire(&kcompress[i].ring[slot].ready))
					break;

				entry = kcompress[i].ring[slot];
				WRITE_ONCE(kcompress[i].ring[slot].ready, 0);
				kcompress[i].tail = tail + 1;
				smp_wmb();
				atomic_dec(&kcompress[i].count);

				kcompressd_do_work(&entry);
			}
			kvfree(kcompress[i].ring);
			kcompress[i].ring = NULL;
		}
	}

	kvfree(kcompress);
	kcompress = NULL;
}

int kcompressd_init(void)
{
	int i, ret;
	unsigned int qsize;

	if (!nr_kcompressd)
		nr_kcompressd = DEFAULT_NR_KCOMPRESSD;
	if (!queue_size_per_kcompressd)
		queue_size_per_kcompressd = INIT_QUEUE_SIZE;

	qsize = roundup_pow_of_two(queue_size_per_kcompressd);

	kcompress = kvzalloc(nr_kcompressd * sizeof(struct kcompress),
			     GFP_KERNEL);
	if (!kcompress)
		return -ENOMEM;

	for (i = 0; i < nr_kcompressd; i++) {
		kcompress[i].ring_size = qsize;
		kcompress[i].ring_mask = qsize - 1;
		kcompress[i].ring = kvzalloc(qsize * sizeof(struct write_work),
					     GFP_KERNEL);
		if (!kcompress[i].ring) {
			ret = -ENOMEM;
			goto err_free;
		}

		init_waitqueue_head(&kcompress[i].kcompressd_wait);
		atomic_set(&kcompress[i].head, 0);
		kcompress[i].tail = 0;
		atomic_set(&kcompress[i].count, 0);
		atomic_set(&kcompress[i].in_flight, 0);
		atomic_set(&kcompress[i].running, KCOMPRESSD_RUNNING);

		kcompress[i].kcompressd = kthread_run(kcompressd_worker,
						      &kcompress[i],
						      "kcompressd:%d", i);
		if (IS_ERR(kcompress[i].kcompressd)) {
			ret = PTR_ERR(kcompress[i].kcompressd);
			kcompress[i].kcompressd = NULL;
			pr_err("Failed to start kcompressd:%d (%d)\n", i, ret);
			goto err_free;
		}
	}

	atomic_set(&kcompressd_next, 0);
	atomic_set(&enable_kcompressd, true);
	return 0;

err_free:
	kcompressd_exit();
	return ret;
}

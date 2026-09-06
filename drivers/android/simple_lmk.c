// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2023 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "simple_lmk: " fmt

#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/sort.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/oom.h>
#include <linux/sched/mm.h>
#include <linux/psi.h>
#include <linux/psi_types.h>
#include <uapi/linux/sched/types.h>

/* Consider at most this many victims per reclaim */
#define MAX_VICTIMS 256

/* Timeout in jiffies for each reclaim */
#define RECLAIM_EXPIRES msecs_to_jiffies(CONFIG_ANDROID_SIMPLE_LMK_TIMEOUT_MSEC)

/*
 * Reclaim target, in MiB: 1/64th of installed RAM, clamped to the configured
 * bounds. This yields 96 MiB on the 6 GiB S20FE and 128 MiB on the 8 GiB one,
 * so a single image reclaims proportionally on both. An under-sized target is
 * itself an over-kill mechanism: each event frees less than the deficit, so
 * pressure survives and the next PSI window fires again -- death by a
 * thousand cuts, which reads to the user exactly like over-killing.
 */
#define TARGET_RAM_DIVISOR 64
#define TARGET_MIN_MIB 64
#define TARGET_MAX_MIB 256

/* psi_trigger_create() accepts windows between 500ms and 10s */
#define PSI_WINDOW_MIN_US 500000
#define PSI_WINDOW_MAX_US 10000000

/*
 * oom_score_adj floors. The routine, PSI-driven path never touches adj 0,
 * which is the foreground app; only the OOM emergency path may, because there
 * the alternative to killing it is letting the stock OOM killer choose.
 */
#define ADJ_FLOOR_ROUTINE 1
#define ADJ_FLOOR_EMERGENCY 0

struct victim_info {
	struct task_struct *tsk;
	struct mm_struct *mm;
	unsigned long size;	/* Reclaimable pages: drives the accounting */
	unsigned long score;	/* Decayed pages: drives kill ordering only */
	unsigned long anon;	/* Anon pages: drives reaper ordering only */
};

static struct victim_info victims[MAX_VICTIMS] __cacheline_aligned_in_smp;
static struct task_struct *task_bucket[OOM_SCORE_ADJ_MAX + 1];
static DECLARE_WAIT_QUEUE_HEAD(reclaim_waitq);
static DECLARE_WAIT_QUEUE_HEAD(reaper_waitq);

static __cacheline_aligned_in_smp DEFINE_RWLOCK(mm_free_lock);
static int nr_victims;
static bool reclaim_active;
static DEFINE_MUTEX(reclaim_lock);
static atomic_t needs_emergency = ATOMIC_INIT(0);
static atomic_t needs_reap = ATOMIC_INIT(0);

/*
 * The PSI trigger is recreated whenever its threshold or window changes, so it
 * cannot be the reclaim thread's sleep target: freeing a waitqueue that a
 * thread is still enqueued on is a use-after-free. The thread therefore polls
 * mem_trigger->event from its own stable waitqueue above, and every read of
 * the trigger pointer is serialized by slmk_lock.
 */
static DEFINE_MUTEX(slmk_lock);
static struct psi_trigger *mem_trigger;
static bool slmk_running;

/*
 * PSI stall threshold and window, both in MICROSECONDS. psi_trigger_create()
 * compares accumulated stall time against the threshold, so these are absolute
 * durations and not percentages: "some 70 2000000" is 70 microseconds of stall
 * per 2 seconds, i.e. 0.0035%, which fires on essentially any stall at all.
 * The defaults below are 3.5% of the window, near the ratio AOSP's lmkd uses.
 * Both are writable at runtime -- the change takes effect by recreating the
 * trigger, so the right value can be swept on-device without a rebuild.
 */
static unsigned int psi_threshold_us =
	CONFIG_ANDROID_SIMPLE_LMK_PSI_THRESHOLD_US;
static unsigned int psi_window_us = CONFIG_ANDROID_SIMPLE_LMK_PSI_WINDOW_US;

/* Reclaim target in MiB; 0 derives it from installed RAM */
static unsigned int target_mib;

/* Hard cap on victims killed per reclaim; bounds worst-case kill volume */
static unsigned int max_kills = CONFIG_ANDROID_SIMPLE_LMK_MAX_KILLS;

/* How often the reclaim thread polls the PSI trigger, in ms */
static unsigned int poll_msec = 100;

/* Counters for measuring kill volume in the field; read-only */
static unsigned long stat_events;
static unsigned long stat_reclaims;
static unsigned long stat_kills;
static unsigned long stat_pages_freed;
static unsigned long stat_no_victims;

/*
 * Descending by the named field. Comparing explicitly rather than subtracting
 * matters: the fields are unsigned long and sort() wants an int, so a
 * difference above INT_MAX pages would truncate into the wrong sign and
 * silently invert the order.
 */
#define DEFINE_VICTIM_CMP(name, field)					\
	static int name(const void *lhs_ptr, const void *rhs_ptr)	\
	{								\
		const struct victim_info *lhs = lhs_ptr;			\
		const struct victim_info *rhs = rhs_ptr;			\
									\
		if (lhs->field < rhs->field)				\
			return 1;					\
		if (lhs->field > rhs->field)				\
			return -1;					\
		return 0;						\
	}

DEFINE_VICTIM_CMP(victim_score_cmp, score)
DEFINE_VICTIM_CMP(victim_size_cmp, size)
DEFINE_VICTIM_CMP(victim_anon_cmp, anon)

static void victim_swap(void *lhs_ptr, void *rhs_ptr, int size)
{
	struct victim_info *lhs = lhs_ptr;
	struct victim_info *rhs = rhs_ptr;

	swap(*lhs, *rhs);
}

static unsigned long get_reclaimable_mm_pages(struct mm_struct *mm)
{
	/*
	 * What killing this task actually frees: its resident anonymous
	 * pages, its swapped-out pages (freeing a zram slot returns real RAM),
	 * and its mapped file pages, which survive the kill in page cache but
	 * become unmapped and thus reclaimable by normal reclaim.
	 *
	 * This is deliberately NOT decayed. It is the only number allowed to
	 * be counted against the reclaim target: a decayed value collapses
	 * toward zero for every process during a refault storm, which makes
	 * the target look unreachable and triggers a kill-everything sweep
	 * precisely when the system is most stressed.
	 */
	return get_mm_counter(mm, MM_ANONPAGES) +
		get_mm_counter(mm, MM_SWAPENTS) +
		get_mm_counter(mm, MM_FILEPAGES);
}

static unsigned long get_mm_kill_score(struct mm_struct *mm)
{
	unsigned long anon_pages = get_mm_counter(mm, MM_ANONPAGES);
	unsigned long swap_pages = get_mm_counter(mm, MM_SWAPENTS);
	unsigned long age = jiffies - READ_ONCE(mm->last_accessed);
	unsigned long decay = msecs_to_jiffies(CONFIG_ANDROID_SIMPLE_LMK_DECAY_MSEC);

	/*
	 * Ordering key only: discount the resident and swapped-out anonymous
	 * pages of an mm that was touched recently, so cold processes are
	 * preferred as victims over ones actively using their memory. A
	 * recently foregrounded app is the likeliest to be reopened, and
	 * killing it only buys a reload stall.
	 */
	if (age < decay) {
		anon_pages = anon_pages * age / decay;
		swap_pages = swap_pages * age / decay;
	}

	return anon_pages + swap_pages + get_mm_counter(mm, MM_FILEPAGES);
}

static unsigned long find_victims(int *vindex, unsigned long target,
				  short adj_floor)
{
	short i, min_adj = SHRT_MAX, max_adj = 0;
	unsigned long pages_found = 0;
	struct task_struct *tsk;

	rcu_read_lock();
	for_each_process(tsk) {
		struct signal_struct *sig;
		short adj;

		/*
		 * Consider thread group leaders only. for_each_process()
		 * walks every thread, not just leaders -- copy_process() puts
		 * every task on init_task.tasks -- so without this a
		 * 20-thread app occupies 20 victim slots that all share one
		 * mm: the page accounting is inflated 20x, the victims array
		 * can be exhausted by duplicates of a single process, and the
		 * same thread group is signalled repeatedly.
		 */
		if (!thread_group_leader(tsk))
			continue;

		sig = tsk->signal;
		adj = READ_ONCE(sig->oom_score_adj);
		/*
		 * Never target adj 0 (FOREGROUND_APP_ADJ) from the routine
		 * path. A negative adj marks tasks that must not be killed,
		 * which also excludes init and kthreads. The adj bound is
		 * re-checked here rather than trusted: oom_score_adj is
		 * user-writable and indexes a fixed-size array.
		 */
		if (adj < adj_floor || adj > OOM_SCORE_ADJ_MAX ||
		    sig->flags & (SIGNAL_GROUP_EXIT | SIGNAL_GROUP_COREDUMP) ||
		    (thread_group_empty(tsk) && tsk->flags & PF_EXITING))
			continue;

		/* Store the task in a linked-list bucket based on its adj */
		tsk->simple_lmk_next = task_bucket[adj];
		task_bucket[adj] = tsk;

		/* Track the min and max adjs to speed up the loop below */
		if (adj > max_adj)
			max_adj = adj;
		if (adj < min_adj)
			min_adj = adj;
	}

	if (min_adj > max_adj)
		goto done;

	/* Start searching for victims from the highest adj (least important) */
	for (i = max_adj; i >= min_adj; i--) {
		int old_vindex;

		tsk = task_bucket[i];
		if (!tsk)
			continue;

		/* Clear out this bucket for the next time reclaim is done */
		task_bucket[i] = NULL;

		/* Iterate through every task with this adj */
		old_vindex = *vindex;
		do {
			struct task_struct *vtsk;
			struct mm_struct *mm;
			int j;

			vtsk = find_lock_task_mm(tsk);
			if (!vtsk)
				continue;

			mm = vtsk->mm;

			/*
			 * Two thread group leaders can share one mm (a
			 * CLONE_VM-but-not-CLONE_THREAD child, e.g. from
			 * vfork). Counting it twice would double the
			 * accounting and kill two processes for one mm.
			 */
			for (j = 0; j < *vindex; j++) {
				if (victims[j].mm != mm)
					continue;
				task_unlock(vtsk);
				mm = NULL;
				break;
			}
			if (!mm)
				continue;

			/* Store this potential victim away for later */
			victims[*vindex].tsk = vtsk;
			victims[*vindex].mm = mm;
			victims[*vindex].size = get_reclaimable_mm_pages(mm);
			victims[*vindex].score = get_mm_kill_score(mm);
			victims[*vindex].anon = 0;

			/* Count the number of pages that have been found */
			pages_found += victims[*vindex].size;

			/* Make sure there's space left in the victim array */
			if (++*vindex == MAX_VICTIMS)
				break;
		} while ((tsk = tsk->simple_lmk_next));

		/* Go to the next bucket if nothing was found */
		if (*vindex == old_vindex)
			continue;

		/*
		 * Sort the victims in descending order of kill score to
		 * prioritize killing the coldest, largest ones first.
		 */
		sort(&victims[old_vindex], *vindex - old_vindex,
		     sizeof(*victims), victim_score_cmp, victim_swap);

		/* Stop when we are out of space or have enough pages found */
		if (*vindex == MAX_VICTIMS || pages_found >= target) {
			/* Zero out any remaining buckets we didn't touch */
			if (i > min_adj)
				memset(&task_bucket[min_adj], 0,
				       (i - min_adj) * sizeof(*task_bucket));
			break;
		}
	}

done:
	rcu_read_unlock();

	return pages_found;
}

/*
 * Decide which of the first @vlen victims to kill, releasing the task lock on
 * every victim spared. Stops at whichever comes first: enough pages to satisfy
 * the target, or the per-reclaim kill cap.
 */
static int process_victims(int vlen, unsigned long target, int kill_cap)
{
	unsigned long pages_found = 0;
	int i, nr_to_kill = 0;

	for (i = 0; i < vlen; i++) {
		struct victim_info *victim = &victims[i];

		if (nr_to_kill >= kill_cap || pages_found >= target) {
			/* The victim's mm lock is taken in find_victims */
			task_unlock(victim->tsk);
		} else {
			pages_found += victim->size;
			nr_to_kill++;
		}
	}

	return nr_to_kill;
}

static void set_task_rt_prio(struct task_struct *tsk, int priority)
{
	const struct sched_param rt_prio = {
		.sched_priority = priority
	};

	sched_setscheduler_nocheck(tsk, SCHED_RR, &rt_prio);
}

static unsigned long reclaim_target_pages(void)
{
	unsigned int mib = target_mib;

	if (!mib)
		mib = (totalram_pages >> (20 - PAGE_SHIFT)) /
			TARGET_RAM_DIVISOR;

	return clamp_t(unsigned long, mib, TARGET_MIN_MIB, TARGET_MAX_MIB) *
		SZ_1M / PAGE_SIZE;
}

/* Returns whether any victim was killed */
static bool scan_and_kill(short adj_floor)
{
	int i, nr_to_kill, nr_found = 0, kill_cap;
	unsigned long pages_found, pages_freed = 0, target;

	target = reclaim_target_pages();
	kill_cap = clamp_t(unsigned int, max_kills, 1, MAX_VICTIMS);

	/*
	 * Reset nr_victims so the reaper thread and simple_lmk_mm_freed() are
	 * aware that the victims array is no longer valid.
	 */
	write_lock(&mm_free_lock);
	nr_victims = 0;
	write_unlock(&mm_free_lock);

	/* Populate the victims array with tasks sorted by adj and then score */
	pages_found = find_victims(&nr_found, target, adj_floor);
	if (unlikely(!nr_found)) {
		stat_no_victims++;
		pr_err_ratelimited("No processes available to kill!\n");
		return false;
	}

	/*
	 * Minimize the body count: take victims in adj order until the target
	 * is met, then re-sort that set by real size so that a few large
	 * victims can replace many small ones. When fewer pages are available
	 * than the target both passes simply take as many as the cap allows,
	 * so a thrashing system can't empty out the whole cached-app set at
	 * once.
	 */
	nr_to_kill = process_victims(nr_found, target, kill_cap);
	sort(victims, nr_to_kill, sizeof(*victims), victim_size_cmp,
	     victim_swap);
	nr_to_kill = process_victims(nr_to_kill, target, kill_cap);

	/*
	 * Store the final number of victims for simple_lmk_mm_freed() and the
	 * reaper thread, and indicate that reclaim is active.
	 */
	write_lock(&mm_free_lock);
	nr_victims = nr_to_kill;
	reclaim_active = true;
	write_unlock(&mm_free_lock);

	/* Kill the victims */
	for (i = 0; i < nr_to_kill; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *t, *vtsk = victim->tsk;
		struct mm_struct *mm = victim->mm;

		pr_info("Killing %s with adj %d to free %lu KiB\n", vtsk->comm,
			vtsk->signal->oom_score_adj,
			victim->size << (PAGE_SHIFT - 10));

		/* Make the victim reap anonymous memory first in exit_mmap() */
		set_bit(MMF_OOM_VICTIM, &mm->flags);

		/* Accelerate the victim's death by forcing the kill signal */
		do_send_sig_info(SIGKILL, SEND_SIG_FORCED, vtsk, PIDTYPE_TGID);

		/*
		 * Record the kill for the userspace-LMK bookkeeping that
		 * should_ulmk_retry() reads to decide whether LMK is stuck.
		 * This is done explicitly rather than by routing through
		 * group_send_sig_info(), which would also run
		 * check_kill_permission() and the add_to_oom_reaper() and
		 * foreground-kill-panic hooks -- all three gated on the
		 * caller being named "lmkd", which this thread is not.
		 */
		ulmk_update_last_kill();

		/*
		 * Mark the thread group dead so that other kernel code knows,
		 * and then elevate the thread group to SCHED_RR with minimum RT
		 * priority. The entire group needs to be elevated because
		 * there's no telling which threads have references to the mm as
		 * well as which thread will happen to put the final reference
		 * and release the mm's memory. If the mm is released from a
		 * thread with low scheduling priority then it may take a very
		 * long time for exit_mmap() to complete.
		 */
		rcu_read_lock();
		for_each_thread(vtsk, t)
			set_tsk_thread_flag(t, TIF_MEMDIE);
		for_each_thread(vtsk, t)
			set_task_rt_prio(t, 1);
		rcu_read_unlock();

		/*
		 * Allow the victim to run on any CPU. cpu_all_mask always
		 * contains the task's current CPU, so __set_cpus_allowed_ptr()
		 * takes its early-out and never calls stop_one_cpu(); widening
		 * the mask to anything narrower here would make this sleep
		 * while task_lock() is held.
		 */
		set_cpus_allowed_ptr(vtsk, cpu_all_mask);

		/* Signals can't wake frozen tasks; only a thaw operation can */
		__thaw_task(vtsk);

		/* Store the anon page count to sort victims for reaping */
		victim->anon = get_mm_counter(mm, MM_ANONPAGES);
		pages_freed += victim->size;

		/* Finally release the victim's task lock acquired earlier */
		task_unlock(vtsk);
	}

	/*
	 * Sort the victims by descending order of anonymous pages so the reaper
	 * thread can prioritize reaping the victims with the most anonymous
	 * pages first. Then wake the reaper thread if it's asleep. The lock
	 * orders the needs_reap store before waitqueue_active().
	 */
	write_lock(&mm_free_lock);
	sort(victims, nr_to_kill, sizeof(*victims), victim_cmp, victim_swap);
	atomic_set(&needs_reap, 1);
	reclaim_active = false;
	write_unlock(&mm_free_lock);
	if (waitqueue_active(&reaper_waitq))
		wake_up(&reaper_waitq);

	return pages_found;
}

static bool reclaim_needed(void)
{
	if (mem_trigger && cmpxchg(&mem_trigger->event, 1, 0))
		return true;
	return atomic_cmpxchg_relaxed(&needs_reclaim, 1, 0);
}

static int simple_lmk_reclaim_thread(void *data)
{
	/* Use maximum RT priority */
	set_task_rt_prio(current, MAX_RT_PRIO - 1);
	set_freezable();

	while (1) {
		wait_event_freezable(*reclaim_waitq,
				     kthread_should_stop() || reclaim_needed());
		if (kthread_should_stop())
			break;
		if (mutex_trylock(&reclaim_lock)) {
			scan_and_kill();
			mutex_unlock(&reclaim_lock);
		}
	}

	return 0;
}

static struct mm_struct *next_reap_victim(void)
{
	struct mm_struct *mm = NULL;
	bool should_retry = false;
	int i;

	/* Take a write lock so no victim's mm can be freed while scanning */
	write_lock(&mm_free_lock);
	for (i = 0; i < nr_victims; i++, mm = NULL) {
		/* Check if this victim is alive and hasn't been reaped yet */
		mm = victims[i].mm;
		if (!mm || test_bit(MMF_OOM_SKIP, &mm->flags))
			continue;

		/*
		 * Skip victims that haven't been killed yet. New victims are
		 * published to this array before the reclaim thread sends them
		 * SIGKILL, so a reaper left over from a timed-out reclaim must
		 * not unmap a not-yet-dying victim's memory.
		 */
		if (!test_bit(MMF_OOM_VICTIM, &mm->flags))
			continue;

		/* Do a trylock so the reaper thread doesn't sleep */
		if (!down_read_trylock(&mm->mmap_sem)) {
			should_retry = true;
			continue;
		}

		/*
		 * Check MMF_OOM_SKIP again under the lock in case this mm was
		 * reaped by exit_mmap() and then had its page tables destroyed.
		 * No mmgrab() is needed because the reclaim thread sets
		 * MMF_OOM_VICTIM under task_lock() for the mm's task, which
		 * guarantees that MMF_OOM_VICTIM is always set before the
		 * victim mm can enter exit_mmap(). Therefore, an mmap read lock
		 * is sufficient to keep the mm struct itself from being freed.
		 */
		if (!test_bit(MMF_OOM_SKIP, &mm->flags))
			break;
		up_read(&mm->mmap_sem);
	}

	if (!mm) {
		if (should_retry)
			/* Return ERR_PTR(-EAGAIN) to try reaping again later */
			mm = ERR_PTR(-EAGAIN);
		else if (!reclaim_active)
			/*
			 * Nothing left to reap, so stop simple_lmk_mm_freed()
			 * from iterating over the victims array since reclaim
			 * is no longer active. Return NULL to stop reaping.
			 */
			nr_victims = 0;
	}
	write_unlock(&mm_free_lock);

	return mm;
}

static void reap_victims(void)
{
	struct mm_struct *mm;
	int retries = 0;

	while ((mm = next_reap_victim())) {
		if (IS_ERR(mm)) {
			/*
			 * Give up after the reclaim timeout if the victims'
			 * mmap_sem stays contended; exit_mmap() will reap them
			 * when they die.  Bounding the retry prevents a stuck
			 * victim from keeping the reaper spinning forever.
			 */
			if (++retries >= RECLAIM_EXPIRES)
				break;
			/* Wait one jiffy before trying to reap again */
			schedule_timeout_uninterruptible(1);
			continue;
		}

		/* Reset the retry counter on a successful reap */
		retries = 0;

		/*
		 * Try to reap the victim. Unflag the mm for exit_mmap() reaping
		 * and mark it as reaped with MMF_OOM_SKIP if successful.
		 */
		if (__oom_reap_task_mm(mm)) {
			clear_bit(MMF_OOM_VICTIM, &mm->flags);
			set_bit(MMF_OOM_SKIP, &mm->flags);
		}
		up_read(&mm->mmap_sem);
	}
}

static int simple_lmk_reaper_thread(void *data)
{
	/* Use a lower priority than the reclaim thread */
	set_task_rt_prio(current, MAX_RT_PRIO - 2);
	set_freezable();

	while (1) {
		wait_event_freezable(reaper_waitq,
				     kthread_should_stop() ||
				     atomic_cmpxchg_relaxed(&needs_reap, 1, 0));
		if (kthread_should_stop())
			break;
		reap_victims();
	}

	return 0;
}

void simple_lmk_mm_freed(struct mm_struct *mm)
{
	int i;

	/*
	 * Victims are guaranteed to have MMF_OOM_SKIP set after exit_mmap()
	 * finishes. Use this to ignore unrelated dying processes.
	 */
	if (!test_bit(MMF_OOM_SKIP, &mm->flags))
		return;

	read_lock(&mm_free_lock);
	for (i = 0; i < nr_victims; i++) {
		if (victims[i].mm == mm) {
			/* Prevent the reaper from touching a freed victim */
			victims[i].mm = NULL;
			break;
		}
	}
	read_unlock(&mm_free_lock);
}

static int simple_lmk_oom_cb(struct notifier_block *nb,
			     unsigned long action, void *data)
{
	int *freed = data;

	/*
	 * Try to reclaim synchronously so the kernel OOM killer is
	 * preempted.  If another reclaim is in progress, fall back to
	 * waking the reclaim thread.
	 */
	if (mutex_trylock(&reclaim_lock)) {
		if (scan_and_kill() && freed)
			*freed = 1;
		mutex_unlock(&reclaim_lock);
	} else {
		atomic_set(&needs_reclaim, 1);
		smp_mb__after_atomic();
		if (waitqueue_active(reclaim_waitq))
			wake_up(reclaim_waitq);
	}

	return NOTIFY_OK;
}

static struct notifier_block oom_notif = {
	.notifier_call = simple_lmk_oom_cb,
	.priority = INT_MAX
};


/* Initialize Simple LMK when lmkd in Android writes to the minfree parameter */
static int simple_lmk_init_set(const char *val, const struct kernel_param *kp)
{
	static atomic_t init_done = ATOMIC_INIT(0);
	struct task_struct *reaper, *reclaim;

	if (atomic_cmpxchg(&init_done, 0, 1))
		return 0;

	mem_trigger = psi_trigger_create(&psi_system, PSI_MEM_SPEC,
					 sizeof(PSI_MEM_SPEC) - 1, PSI_MEM);
	if (IS_ERR(mem_trigger)) {
		pr_err("Failed to create PSI trigger: %ld; relying on OOM notifications only\n",
		       PTR_ERR(mem_trigger));
		mem_trigger = NULL;
	} else {
		reclaim_waitq = &mem_trigger->event_wait;
	}

	reaper = kthread_run(simple_lmk_reaper_thread, NULL,
			     "simple_lmkd_reaper");
	if (IS_ERR(reaper)) {
		pr_err("Failed to create reaper thread: %ld\n", PTR_ERR(reaper));
		goto err_trigger;
	}

	reclaim = kthread_run(simple_lmk_reclaim_thread, NULL, "simple_lmkd");
	if (IS_ERR(reclaim)) {
		pr_err("Failed to create reclaim thread: %ld\n",
		       PTR_ERR(reclaim));
		kthread_stop(reaper);
		goto err_trigger;
	}

	if (register_oom_notifier(&oom_notif)) {
		pr_err("Failed to register OOM notifier\n");
		kthread_stop(reaper);
		kthread_stop(reclaim);
		goto err_trigger;
	}

	return 0;

err_trigger:
	psi_trigger_destroy(mem_trigger);
	mem_trigger = NULL;
	reclaim_waitq = &oom_waitq;
	atomic_set(&init_done, 0);
	return 0;
}

static const struct kernel_param_ops simple_lmk_init_ops = {
	.set = simple_lmk_init_set
};

/* Needed to prevent Android from thinking there's no LMK and thus rebooting */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
module_param_cb(minfree, &simple_lmk_init_ops, NULL, 0200);

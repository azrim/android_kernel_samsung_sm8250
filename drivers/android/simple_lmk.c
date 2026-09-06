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
	sort(victims, nr_to_kill, sizeof(*victims), victim_anon_cmp,
	     victim_swap);
	atomic_set(&needs_reap, 1);
	reclaim_active = false;
	write_unlock(&mm_free_lock);
	if (waitqueue_active(&reaper_waitq))
		wake_up(&reaper_waitq);

	stat_reclaims++;
	stat_kills += nr_to_kill;
	stat_pages_freed += pages_freed;

	return nr_to_kill > 0;
}

static bool reclaim_needed(int *adj_floor)
{
	struct psi_trigger *t;
	bool needed;

	/* A reclaim deferred by the OOM notifier while one was running */
	if (atomic_cmpxchg(&needs_emergency, 1, 0)) {
		*adj_floor = ADJ_FLOOR_EMERGENCY;
		return true;
	}

	mutex_lock(&slmk_lock);
	t = mem_trigger;
	/*
	 * Consuming the event is what psi_trigger_poll() would do for a
	 * userspace reader, so pet the userspace-LMK watchdog here. Without
	 * this the watchdog stays armed but is never petted: it latches
	 * expired after the first event, and should_ulmk_retry() then decides
	 * forever that the stall consumer is stuck.
	 */
	needed = t && cmpxchg(&t->event, 1, 0);
	if (needed)
		ulmk_watchdog_pet(&t->wdog_timer);
	mutex_unlock(&slmk_lock);

	return needed;
}

static int simple_lmk_reclaim_thread(void *data)
{
	/* Use maximum RT priority */
	set_task_rt_prio(current, MAX_RT_PRIO - 1);
	set_freezable();

	while (1) {
		int adj_floor = ADJ_FLOOR_ROUTINE;
		/*
		 * Clamped: this is user-writable, and a zero timeout would
		 * turn the wait into a busy loop running at RT priority 98.
		 */
		unsigned long timeout = msecs_to_jiffies(clamp(poll_msec,
							       10u, 1000u));

		/*
		 * The PSI trigger's event flag is polled rather than waited
		 * on, so that the trigger can be recreated at runtime to sweep
		 * its threshold. The OOM notifier still wakes this queue
		 * directly, so a real OOM is not delayed by the poll interval.
		 */
		wait_event_freezable_timeout(reclaim_waitq,
					     kthread_should_stop() ||
					     atomic_read(&needs_emergency),
					     timeout);
		if (kthread_should_stop())
			break;
		if (!reclaim_needed(&adj_floor))
			continue;
		stat_events++;
		if (mutex_trylock(&reclaim_lock)) {
			scan_and_kill(adj_floor);
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
		 * victim mm can enter exit_mmap(). exit_mmap() then takes
		 * mmap_sem for write after setting MMF_OOM_SKIP, which is what
		 * actually holds off the mm's release while this read lock is
		 * held. MMF_OOM_VICTIM is therefore never cleared here.
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
			 * when they die. Bounding the retry prevents a stuck
			 * victim from keeping the reaper spinning forever.
			 */
			if (++retries >= RECLAIM_EXPIRES)
				break;
			/* Wait one jiffy before trying to reap again */
			schedule_timeout_uninterruptible(1);
			continue;
		}

		/*
		 * Try to reap the victim, and mark it as reaped so that
		 * exit_mmap() skips it. The retry counter is reset only on
		 * success: a victim whose mmu notifier blocks the reap keeps
		 * neither MMF_OOM_SKIP nor a cleared MMF_OOM_VICTIM, so it is
		 * offered again, and resetting on every acquisition would turn
		 * that into an endless loop at RT priority.
		 */
		if (__oom_reap_task_mm(mm)) {
			set_bit(MMF_OOM_SKIP, &mm->flags);
			retries = 0;
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

	/* This writes the array, so it needs the write side of the lock */
	write_lock(&mm_free_lock);
	for (i = 0; i < nr_victims; i++) {
		if (victims[i].mm == mm) {
			/* Prevent the reaper from touching a freed victim */
			victims[i].mm = NULL;
			break;
		}
	}
	write_unlock(&mm_free_lock);
}

static int simple_lmk_oom_cb(struct notifier_block *nb,
			     unsigned long action, void *data)
{
	int *freed = data;

	/*
	 * The page allocator is out of memory now, so reclaim synchronously
	 * instead of waiting for the next poll. This is the only path allowed
	 * to consider adj 0. If a reclaim is already running, defer it and
	 * leave *freed alone: out_of_memory() then falls through to the stock
	 * OOM killer rather than reporting progress that wasn't made.
	 */
	if (mutex_trylock(&reclaim_lock)) {
		if (scan_and_kill(ADJ_FLOOR_EMERGENCY) && freed)
			*freed = 1;
		mutex_unlock(&reclaim_lock);
	} else {
		atomic_set(&needs_emergency, 1);
		smp_mb__after_atomic();
		if (waitqueue_active(&reclaim_waitq))
			wake_up(&reclaim_waitq);
	}

	return NOTIFY_OK;
}

static struct notifier_block oom_notif = {
	.notifier_call = simple_lmk_oom_cb,
	.priority = INT_MAX
};

static bool psi_spec_valid(void)
{
	return psi_window_us >= PSI_WINDOW_MIN_US &&
		psi_window_us <= PSI_WINDOW_MAX_US &&
		psi_threshold_us > 0 && psi_threshold_us <= psi_window_us;
}

/* Caller holds slmk_lock. psi_trigger_create() parses buf but keeps no copy */
static int psi_trigger_swap(void)
{
	char spec[32];
	struct psi_trigger *old, *new;
	int len;

	len = snprintf(spec, sizeof(spec), "some %u %u", psi_threshold_us,
		       psi_window_us);
	if (len >= (int)sizeof(spec))
		return -EINVAL;

	new = psi_trigger_create(&psi_system, spec, len, PSI_MEM);
	if (IS_ERR(new))
		return PTR_ERR(new);

	old = mem_trigger;
	mem_trigger = new;
	/*
	 * Safe to free the old trigger here: no thread is ever enqueued on its
	 * waitqueue, since the reclaim thread polls the event flag instead.
	 */
	psi_trigger_destroy(old);

	return 0;
}

/* Apply a changed threshold or window, if the trigger already exists */
static int psi_trigger_apply(void)
{
	int ret = 0;

	if (!psi_spec_valid())
		return -EINVAL;

	mutex_lock(&slmk_lock);
	if (mem_trigger)
		ret = psi_trigger_swap();
	mutex_unlock(&slmk_lock);

	return ret;
}

static int set_psi_threshold_us(const char *val, const struct kernel_param *kp)
{
	unsigned int v = psi_threshold_us;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	/* Validate before committing so a rejected write changes nothing */
	if (!v || v > psi_window_us)
		return -EINVAL;

	psi_threshold_us = v;
	return psi_trigger_apply();
}

static int set_psi_window_us(const char *val, const struct kernel_param *kp)
{
	unsigned int v = psi_window_us;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	if (v < PSI_WINDOW_MIN_US || v > PSI_WINDOW_MAX_US ||
	    v < psi_threshold_us)
		return -EINVAL;

	psi_window_us = v;
	return psi_trigger_apply();
}

static const struct kernel_param_ops psi_threshold_ops = {
	.set = set_psi_threshold_us,
	.get = param_get_uint,
};

static const struct kernel_param_ops psi_window_ops = {
	.set = set_psi_window_us,
	.get = param_get_uint,
};

/* Initialize Simple LMK when lmkd in Android writes to the minfree parameter */
static int simple_lmk_init_set(const char *val, const struct kernel_param *kp)
{
	struct task_struct *reaper, *reclaim;

	mutex_lock(&slmk_lock);
	if (slmk_running) {
		mutex_unlock(&slmk_lock);
		return 0;
	}

	if (!psi_spec_valid()) {
		pr_err("Invalid PSI spec: threshold=%u window=%u\n",
		       psi_threshold_us, psi_window_us);
		mutex_unlock(&slmk_lock);
		return 0;
	}

	if (psi_trigger_swap())
		pr_info("PSI trigger unavailable; relying on OOM notifications\n");
	mutex_unlock(&slmk_lock);

	reaper = kthread_run(simple_lmk_reaper_thread, NULL,
			     "simple_lmkd_reaper");
	if (IS_ERR(reaper)) {
		pr_err("Failed to create reaper thread: %ld\n", PTR_ERR(reaper));
		return 0;
	}

	reclaim = kthread_run(simple_lmk_reclaim_thread, NULL, "simple_lmkd");
	if (IS_ERR(reclaim)) {
		pr_err("Failed to create reclaim thread: %ld\n",
		       PTR_ERR(reclaim));
		kthread_stop(reaper);
		return 0;
	}

	if (register_oom_notifier(&oom_notif)) {
		pr_err("Failed to register OOM notifier\n");
		kthread_stop(reaper);
		kthread_stop(reclaim);
		return 0;
	}

	mutex_lock(&slmk_lock);
	slmk_running = true;
	mutex_unlock(&slmk_lock);

	pr_info("Initialized: target=%lu MiB threshold=%u us window=%u us cap=%u\n",
		reclaim_target_pages() * PAGE_SIZE / SZ_1M, psi_threshold_us,
		psi_window_us, max_kills);

	/* Always succeed: a failure here would make lmkd think LMK is absent */
	return 0;
}

static const struct kernel_param_ops simple_lmk_init_ops = {
	.set = simple_lmk_init_set
};

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "simple_lmk."

module_param_cb(psi_threshold_us, &psi_threshold_ops, &psi_threshold_us, 0644);
MODULE_PARM_DESC(psi_threshold_us,
		 "Microseconds of memory stall per window before reclaiming; not a percentage");
module_param_cb(psi_window_us, &psi_window_ops, &psi_window_us, 0644);
MODULE_PARM_DESC(psi_window_us, "PSI stall window in microseconds");
module_param(target_mib, uint, 0644);
MODULE_PARM_DESC(target_mib, "MiB to free per reclaim; 0 derives it from RAM");
module_param(max_kills, uint, 0644);
MODULE_PARM_DESC(max_kills, "Maximum processes killed by a single reclaim");
module_param(poll_msec, uint, 0644);
MODULE_PARM_DESC(poll_msec, "How often the reclaim thread checks the PSI trigger");

module_param(stat_events, ulong, 0444);
MODULE_PARM_DESC(stat_events, "Reclaims triggered by memory pressure");
module_param(stat_reclaims, ulong, 0444);
MODULE_PARM_DESC(stat_reclaims, "Reclaims that ran a victim scan");
module_param(stat_kills, ulong, 0444);
MODULE_PARM_DESC(stat_kills, "Total processes killed");
module_param(stat_pages_freed, ulong, 0444);
MODULE_PARM_DESC(stat_pages_freed, "Total pages accounted as freed");
module_param(stat_no_victims, ulong, 0444);
MODULE_PARM_DESC(stat_no_victims, "Reclaims that found nothing killable");

/* Needed to prevent Android from thinking there's no LMK and thus rebooting */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
module_param_cb(minfree, &simple_lmk_init_ops, NULL, 0200);

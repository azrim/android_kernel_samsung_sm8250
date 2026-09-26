// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2023 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "simple_lmk: " fmt

#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/sort.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/oom.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/psi.h>
#include <linux/psi_types.h>
#include <linux/cgroup.h>
#include <linux/memcontrol.h>
#include <linux/simple_lmk.h>
#include <uapi/linux/sched/types.h>

/* Consider at most this many victims per reclaim */
#define MAX_VICTIMS 256

/* Timeout in jiffies for each reclaim */
#define RECLAIM_EXPIRES msecs_to_jiffies(CONFIG_ANDROID_SIMPLE_LMK_TIMEOUT_MSEC)

/*
 * Reclaim target bounds, in MiB. The target itself is a real deficit
 * estimate: the fraction of the last 10s that tasks spent stalled on
 * memory (PSI avg10), applied to installed RAM -- mild pressure at
 * avg10 ~1% frees about 1% of RAM, a stall storm saturates the upper
 * bound. The floor is what keeps a system whose trigger fires but whose
 * average is still low from death by a thousand cuts: each event must
 * free enough that the next window can observe real relief, or PSI
 * re-fires forever and reads to the user exactly like over-killing.
 */
#define TARGET_MIN_MIB 64
#define TARGET_MAX_MIB 256

/* psi_trigger_create() accepts windows between 500ms and 10s */
#define PSI_WINDOW_MIN_US 500000
#define PSI_WINDOW_MAX_US 10000000
#define PSI_THRESHOLD_MIN_US 1000
#define PSI_THRESHOLD_MAX_US 10000000
#define POLL_MSEC_MIN 10
#define POLL_MSEC_MAX 1000

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
 * Memcg cooperation (OneUI).  The memory controller stays enabled
 * because Samsung userspace expects it; Simple LMK treats a group's
 * limit as a kill signal instead of charging past it silently.
 *
 * emergency_scope pins the memcg of a memcg-OOM until the reclaim
 * thread consumes it.  NULL means a global (PSI / stock OOM) pass.
 * css_tryget() in the notifier; css_put() after the scoped scan.
 */
static struct mem_cgroup *emergency_scope;
/* Protects emergency_scope; held from the allocator OOM path, so spinlock */
static DEFINE_SPINLOCK(scope_lock);
static bool memcg_aware = true;
/*
 * When a candidate's memcg is at or above this percentage of its
 * limit, boost its kill score so global PSI passes prefer the group
 * that is actually about to OOM, not a random background app.
 */
static unsigned int memcg_boost_pct = 80;
static unsigned long stat_memcg_scoped;
static unsigned long stat_memcg_skipped;

/*
 * What the last completed scan achieved, published for the OOM notifier.
 * The notifier must never scan itself -- it runs inside the page allocator
 * with locks held -- so it enqueues the emergency batch and answers
 * out_of_memory() from this verdict instead: anything but SLMK_EMPTY claims
 * that Simple LMK is working or about to work, and SLMK_EMPTY is the signal
 * to hand control back to the stock OOM killer. Any scan (emergency or
 * routine) refreshes the verdict, so a system whose PSI path is actively
 * killing never strands the emergency path on a stale "found nobody".
 */
enum slmk_kill_state {
	SLMK_IDLE,	/* no scan has completed yet */
	SLMK_RUNNING,	/* a scan is in flight */
	SLMK_KILLED,	/* last completed scan killed at least one task */
	SLMK_EMPTY,	/* last completed scan found nobody to kill */
};
static atomic_t kill_state = ATOMIC_INIT(SLMK_IDLE);

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

/*
 * How often the reclaim thread polls the PSI trigger, in ms. PSI generates an
 * event at most once per window and polls the trigger itself every
 * window/10, so at the default 2s window there is nothing to see more often
 * than every 200ms: a longer interval just adds that much latency to noticing
 * an event that is already set, and a shorter one burns wakeups for no gain.
 */
static unsigned int poll_msec = 200;

/* Skip routine reclaim while at least this much memory is available, in MiB */
static unsigned int reserve_mib = 1024;

/*
 * Minimum settle time after a reclaim that killed victims, in ms. The victims
 * only return their memory as they finish exit_mmap(), so reclaiming again
 * inside that window double-charges the deficit. 0 disables it. Like
 * poll_msec and reserve_mib, this deliberately has no Kconfig default: it is
 * a tunable, not a policy switch, and adding a config symbol would churn
 * out/.config for no functional gain.
 */
static unsigned int grace_msec = 2000;
#define GRACE_MSEC_MAX 30000

/*
 * Grace period protecting tasks that just changed state, in ms. On the
 * routine path a candidate is skipped when its oom_score_adj was last
 * rewritten -- an app ActivityManager just backgrounded, or one freshly
 * promoted -- or when the task itself launched, within this window.
 * Those are exactly the tasks the user is about to reopen, and killing
 * one buys nothing but a reload stall; they get their fair turn first
 * while older background tasks drain. The timestamp comes from
 * __set_oom_adj() (stamped only when the adj really moves, so a busy
 * service rewriting the same value is not protected forever) and the
 * launch time from task start, both in absolute clocks so suspend does
 * not eat the grace. 0 disables it. The OOM emergency path never
 * consults it: when the allocator is out of memory, waiting is a
 * luxury. Deliberately a module parameter like poll_msec and
 * reserve_mib -- a tunable, not a policy switch, so no Kconfig symbol
 * churns out/.config for it.
 */
static unsigned int bg_grace_msec = 5000;
#define BG_GRACE_MSEC_MAX 60000

/* jiffies at which the last reclaim that killed victims finished */
static unsigned long last_reclaim_end;

/* Counters for measuring kill volume in the field; read-only */
static unsigned long stat_events;
static unsigned long stat_reclaims;
static unsigned long stat_kills;
static unsigned long stat_pages_freed;
static unsigned long stat_no_victims;
static unsigned long stat_gated;
static unsigned long stat_grace_dropped;

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

static bool slmk_memcg_in_scope(struct mem_cgroup *memcg,
				struct mem_cgroup *scope)
{
	for (; memcg; memcg = parent_mem_cgroup(memcg))
		if (memcg == scope)
			return true;
	return false;
}

/* Caller must hold RCU or a css ref on the result's group. */
static struct mem_cgroup *slmk_task_memcg(struct task_struct *tsk)
{
	if (!memcg_aware)
		return NULL;
	return mem_cgroup_from_task(tsk);
}

static bool slmk_memcg_hot(struct mem_cgroup *memcg)
{
	unsigned long max, usage;

	if (!memcg || !memcg_aware || !memcg_boost_pct)
		return false;
	max = mem_cgroup_get_max(memcg);
	if (!max)
		return false;
	usage = page_counter_read(&memcg->memory);
	return usage * 100 >= max * memcg_boost_pct;
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
				  short adj_floor, struct mem_cgroup *scope)
{
	short i, min_adj = SHRT_MAX, max_adj = 0;
	unsigned long pages_found = 0;
	unsigned long bg_grace = 0;
	u64 grace_ns = 0, boot_now = 0;
	struct task_struct *tsk;

	/*
	 * Resolve the background grace window once: the per-task checks
	 * run under rcu_read_lock() and must not call the timekeeper more
	 * than necessary. bg_grace == 0 for an emergency scan, which
	 * never skips.
	 */
	if (adj_floor == ADJ_FLOOR_ROUTINE && bg_grace_msec) {
		bg_grace = msecs_to_jiffies(bg_grace_msec);
		grace_ns = (u64)bg_grace_msec * NSEC_PER_MSEC;
		boot_now = ktime_get_boottime_ns();
	}

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
		 * but init and kthreads share adj 0 with ordinary tasks, so
		 * they need an explicit exclusion. The adj bound is
		 * re-checked here rather than trusted: oom_score_adj is
		 * user-writable and indexes a fixed-size array.
		 */
		if (adj < adj_floor || adj > OOM_SCORE_ADJ_MAX ||
		    is_global_init(tsk) || (tsk->flags & PF_KTHREAD) ||
		    sig->flags & (SIGNAL_GROUP_EXIT | SIGNAL_GROUP_COREDUMP) ||
		    (thread_group_empty(tsk) && tsk->flags & PF_EXITING))
			continue;

		/*
		 * Skip candidates inside the background grace window: the
		 * adj change timestamp (state change, e.g. just moved to
		 * background) or the task's launch time is too recent.
		 * READ_ONCE pairs with the plain store in __set_oom_adj();
		 * a stale reading can only extend or miss the grace by one
		 * store, never fault, and sig itself is pinned by the RCU
		 * read-side section around this loop.
		 */
		if (bg_grace &&
		    (time_before(jiffies, READ_ONCE(sig->oom_adj_change) +
				 bg_grace) ||
		     boot_now - tsk->real_start_time < grace_ns))
			continue;

		/*
		 * Memcg OOM pass: only kill inside the group that is
		 * over its limit (or a descendant).  Killing a cached
		 * app in some other memcg cannot relieve this charge.
		 */
		if (scope) {
			struct mem_cgroup *memcg = slmk_task_memcg(tsk);

			if (!memcg || !slmk_memcg_in_scope(memcg, scope)) {
				stat_memcg_skipped++;
				continue;
			}
		}

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

			/*
			 * Prefer the process sitting in a memcg that is
			 * about to hit its limit: that is the allocation
			 * that will trip memcg OOM next, and its pages are
			 * charged to a group OneUI userspace is watching.
			 */
			if (slmk_memcg_hot(slmk_task_memcg(vtsk)))
				victims[*vindex].score = victims[*vindex].score
							 ? victims[*vindex].score << 1
							 : 1;

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
 * Drop victims with nothing left to reclaim, releasing the task lock taken in
 * find_victims(), and compact the survivors to the front of the array.
 *
 * A zero-size victim is never killed, but leaving it in place would break
 * process_victims()'s invariant that the victims it selects occupy
 * victims[0..nr_to_kill-1]: the two selection passes would each unlock it
 * while a still-locked victim past the selected prefix would never be
 * released.
 */
static int compact_victims(int vlen)
{
	int i, n = 0;

	for (i = 0; i < vlen; i++) {
		if (!victims[i].size) {
			task_unlock(victims[i].tsk);
			continue;
		}
		if (n != i)
			victims[n] = victims[i];
		n++;
	}

	return n;
}

/*
 * Decide which of the first @vlen victims to kill, releasing the task lock on
 * every victim spared. Stops at whichever comes first: enough pages to satisfy
 * the target, or the per-reclaim kill cap.
 *
 * The victims selected here always occupy victims[0..N-1]: both conditions are
 * monotonic, and victims with nothing to reclaim have been compacted out by
 * compact_victims() beforehand.
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
	unsigned long pages;

	/*
	 * Clamp only the value derived from PSI. A value written to
	 * target_mib is already range-checked by set_target_mib(), and
	 * silently shrinking it to TARGET_MAX_MIB would make the parameter
	 * a lie for anything above 256.
	 */
	if (mib)
		return (unsigned long)mib * SZ_1M / PAGE_SIZE;

	/*
	 * Estimate the deficit from stall metrics instead of a fixed
	 * fraction of RAM: the PSI avg10 of "some" memory stall is the
	 * fraction of the last 10 seconds during which at least one task
	 * was stalled on memory, and a system that spent that fraction of
	 * its time stalling was missing roughly that fraction of RAM.
	 * Scale installed RAM by it and clamp to the configured bounds;
	 * the floor also guarantees a non-zero target when PSI is disabled
	 * and the helper answers 0.
	 */
	pages = (unsigned long)div_u64((u64)totalram_pages *
				       psi_mem_stall_avg10(), 10000);

	return clamp_t(unsigned long, pages,
		       (TARGET_MIN_MIB << 20) / PAGE_SIZE,
		       (TARGET_MAX_MIB << 20) / PAGE_SIZE);
}

/* Returns whether any victim was killed */
static bool scan_and_kill(short adj_floor, struct mem_cgroup *scope)
{
	int i, nr_to_kill, nr_found = 0, kill_cap;
	unsigned long pages_found, pages_freed = 0, target;

	if (scope)
		stat_memcg_scoped++;
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
	pages_found = find_victims(&nr_found, target, adj_floor, scope);
	nr_found = compact_victims(nr_found);
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
		 * Pin the task so it stays valid after its lock is dropped.
		 */
		get_task_struct(vtsk);

		/* Store the anon page count to sort victims for reaping */
		victim->anon = get_mm_counter(mm, MM_ANONPAGES);
		pages_freed += victim->size;

		/*
		 * Release the victim's task lock acquired in find_victims()
		 * before touching affinity: set_cpus_allowed_ptr() may sleep
		 * (stop_one_cpu migration) for an affinity-restricted victim,
		 * which must never run under task_lock().
		 */
		task_unlock(vtsk);

		/*
		 * Allow the victim to run on any CPU so a task pinned to an
		 * offline or isolated CPU can still run to die. A failure is
		 * benign (e.g. perf/prime-affine tasks that must keep their
		 * mask); the kill signal is already delivered.
		 */
		set_cpus_allowed_ptr(vtsk, cpu_all_mask);

		/* Signals can't wake frozen tasks; only a thaw operation can */
		__thaw_task(vtsk);

		put_task_struct(vtsk);
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

	/*
	 * Arm the grace period only for a reclaim that killed someone: an
	 * empty scan freed nothing, so there is nothing to wait for. The
	 * reclaim thread is the only caller and holds reclaim_lock, so the
	 * write is serialized; readers pair it with READ_ONCE().
	 */
	if (nr_to_kill > 0)
		WRITE_ONCE(last_reclaim_end, jiffies);

	return nr_to_kill > 0;
}

static bool reclaim_needed(int *adj_floor, struct mem_cgroup **scope)
{
	struct psi_trigger *t;
	bool needed;

	*scope = NULL;

	/* The emergency batch armed by the OOM / memcg-OOM notifier */
	if (atomic_cmpxchg(&needs_emergency, 1, 0)) {
		*adj_floor = ADJ_FLOOR_EMERGENCY;
		spin_lock(&scope_lock);
		*scope = emergency_scope;
		emergency_scope = NULL;
		spin_unlock(&scope_lock);
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

	/*
	 * Grace period: the previous reclaim's victims only return their
	 * memory as they finish exit_mmap(), which under load takes hundreds
	 * of milliseconds. Reclaiming again inside that window double-charges
	 * the deficit -- the pages the last batch promised are still en route
	 * -- and sweeps more cached apps than the pressure justifies. The
	 * event is consumed and counted, like the reserve gate below: PSI
	 * re-fires next window if the pressure is real. A reclaim that killed
	 * nobody doesn't arm the grace period. The OOM emergency path above
	 * is never gated: when the allocator is out of memory, waiting is a
	 * luxury.
	 */
	if (needed && grace_msec && READ_ONCE(last_reclaim_end) &&
	    time_before(jiffies, READ_ONCE(last_reclaim_end) +
			msecs_to_jiffies(grace_msec))) {
		stat_grace_dropped++;
		return false;
	}

	/*
	 * Avoid PSI thrash when memory is still available.
	 * PSI can fire with 2.7GB available at boot.
	 *
	 * Count the suppression: the event was already consumed above, so
	 * returning false here makes it invisible to stat_events, and the
	 * counter would otherwise under-report real memory pressure.
	 */
	if (needed && reserve_mib && si_mem_available() >
	    ((unsigned long)reserve_mib << (20 - PAGE_SHIFT))) {
		stat_gated++;
		return false;
	}

	return needed;
}

static int simple_lmk_reclaim_thread(void *data)
{
	/* Use maximum RT priority */
	set_task_rt_prio(current, MAX_RT_PRIO - 1);
	set_freezable();

	while (1) {
		int adj_floor = ADJ_FLOOR_ROUTINE;
		struct mem_cgroup *scope = NULL;
		bool killed;
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
		if (!reclaim_needed(&adj_floor, &scope))
			continue;
		stat_events++;
		/*
		 * The OOM notifier no longer takes this lock, so this
		 * thread is reclaim_lock's only holder: a plain lock, not
		 * a trylock, and kill_state is published around the scan
		 * for the notifier to read without any lock at all.
		 */
		mutex_lock(&reclaim_lock);
		atomic_set(&kill_state, SLMK_RUNNING);
		killed = scan_and_kill(adj_floor, scope);
		if (scope)
			css_put(&scope->css);
		atomic_set(&kill_state, killed ? SLMK_KILLED : SLMK_EMPTY);
		mutex_unlock(&reclaim_lock);
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
		 * Skip victims the reclaim thread hasn't gotten to yet. It
		 * publishes nr_victims before running its kill loop, so a
		 * reaper left over from a timed-out reclaim can see fresh
		 * entries whose mm isn't marked MMF_OOM_VICTIM yet. That bit
		 * is set before the SIGKILL on purpose: the mm must already be
		 * a victim if the signal takes it into exit_mmap(), else
		 * exit_mmap() skips its reap and never sets MMF_OOM_SKIP,
		 * leaving this array entry pointing into a freed mm.
		 */
		if (!test_bit(MMF_OOM_VICTIM, &mm->flags))
			continue;

		/* Do a trylock so the reaper thread doesn't sleep */
		if (!mmap_read_trylock(mm)) {
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
		mmap_read_unlock(mm);
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
	int retries = 0, fails = 0;

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
		 * exit_mmap() skips it. The mmap retry counter is reset only
		 * on success: a victim whose mmu notifier blocks the reap
		 * keeps neither MMF_OOM_SKIP nor a cleared MMF_OOM_VICTIM, so
		 * it is offered again, and resetting on every acquisition
		 * would turn that into an endless loop at RT priority. Bound
		 * consecutive reap failures the same way, since this loop
		 * otherwise spins forever on a victim that can never reap.
		 */
		if (__oom_reap_task_mm(mm)) {
			set_bit(MMF_OOM_SKIP, &mm->flags);
			retries = 0;
			fails = 0;
		} else if (++fails >= RECLAIM_EXPIRES) {
			/*
			 * This victim's mmu notifier persistently refuses to
			 * invalidate (e.g. a GPU or DMA-BUF mapping), so the
			 * reap can never succeed. Mark the mm skipped so that
			 * next_reap_victim() moves on to the remaining victims
			 * instead of abandoning the whole batch -- which is
			 * what upstream oom_reap_task() does after exhausting
			 * its retries. The victim is still dying, so
			 * exit_mmap() will unmap the rest.
			 */
			set_bit(MMF_OOM_SKIP, &mm->flags);
			fails = 0;
		}
		mmap_read_unlock(mm);
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

/*
 * Memcg OOM hook.  Called from out_of_memory() with the charge still
 * outstanding and the allocator locks held: must not sleep.  Pins the
 * group for the scoped pass and wakes the reclaim thread.  Does not
 * itself choose a victim -- scan_and_kill() does that in process
 * context, where it may sleep.
 */
void simple_lmk_notify_memcg_oom(struct mem_cgroup *memcg)
{
	struct mem_cgroup *old_scope;

	if (!memcg_aware || !memcg)
		return;

	/*
	 * Replace any older scope: the newest over-limit group is the
	 * one whose charge is stuck now.  tryget fails if the css is
	 * already offline; then leave the previous scope alone.
	 */
	if (!css_tryget(&memcg->css))
		return;

	/*
	 * out_of_memory() runs in the charge path and must not sleep,
	 * so this lock is a spinlock, not slmk_lock's mutex.  css_put()
	 * does not sleep.
	 */
	spin_lock(&scope_lock);
	old_scope = emergency_scope;
	emergency_scope = memcg;
	spin_unlock(&scope_lock);
	if (old_scope)
		css_put(&old_scope->css);

	atomic_set(&needs_emergency, 1);
	smp_mb__after_atomic();
	if (waitqueue_active(&reclaim_waitq))
		wake_up(&reclaim_waitq);
}

static int simple_lmk_oom_cb(struct notifier_block *nb,
			     unsigned long action, void *data)
{
	int *freed = data;
	int prev;

	/*
	 * The page allocator is out of memory now, but this callback must
	 * never scan or kill from here: it runs inside the allocator with
	 * locks held, and the scan path sleeps (set_cpus_allowed_ptr()).
	 * The old code did exactly that whenever reclaim_lock was free.
	 * Instead, arm the emergency batch, wake the reclaim thread, and
	 * return immediately -- "join in flight" if a scan is already
	 * running, since it will pick the emergency floor up on its next
	 * loop iteration once needs_emergency is set.
	 *
	 * *freed reports the last completed scan's verdict: any state
	 * other than SLMK_EMPTY means Simple LMK is working or about to
	 * work, so out_of_memory() returns true and lets the wakeup land.
	 * SLMK_EMPTY means the last scan found nobody killable; only then
	 * do we report failure, so out_of_memory() hands control back to
	 * the stock OOM killer as the genuine last resort. A stale EMPTY
	 * is self-correcting: the flag armed here still sends the reclaim
	 * thread through a fresh emergency scan on its next wakeup, which
	 * refreshes the verdict before the following OOM invocation.
	 */
	prev = atomic_read(&kill_state);
	atomic_set(&needs_emergency, 1);
	/* Order the arming store before the wake and the verdict read */
	smp_mb__after_atomic();
	/* Lockless check: a race here only costs a spurious wakeup */
	if (waitqueue_active(&reclaim_waitq))
		wake_up(&reclaim_waitq);

	if (freed)
		*freed = (prev != SLMK_EMPTY);

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
		psi_threshold_us >= PSI_THRESHOLD_MIN_US &&
		psi_threshold_us <= PSI_THRESHOLD_MAX_US &&
		psi_threshold_us <= psi_window_us;
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

	/*
	 * psi identifies the ULMK trigger by t->comm: update_triggers() arms
	 * its watchdog timer, and psi_emergency_trigger()/psi_is_trigger_active()
	 * match on it. psi_trigger_create() stamps the comm of whichever task
	 * called it, which is lmkd on the minfree init write but an arbitrary
	 * shell when the threshold or window is swept at runtime. Force the
	 * magic name so those paths keep working regardless of the writer.
	 */
	memcpy(new->comm, ULMK_MAGIC, sizeof(ULMK_MAGIC));

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
	if (v < PSI_THRESHOLD_MIN_US || v > PSI_THRESHOLD_MAX_US ||
	    v > psi_window_us)
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

static int set_poll_msec(const char *val, const struct kernel_param *kp)
{
	unsigned int v = poll_msec;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	if (v < POLL_MSEC_MIN || v > POLL_MSEC_MAX)
		return -EINVAL;
	poll_msec = v;
	return 0;
}

static int set_max_kills(const char *val, const struct kernel_param *kp)
{
	unsigned int v = max_kills;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	/*
	 * A single reclaim can never kill more than MAX_VICTIMS: the victim
	 * array is that size. Reject a larger request rather than accepting
	 * it and silently clamping the effective cap in scan_and_kill().
	 */
	if (v < 1 || v > MAX_VICTIMS)
		return -EINVAL;
	max_kills = v;
	return 0;
}

static int set_target_mib(const char *val, const struct kernel_param *kp)
{
	unsigned int v = target_mib;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	/*
	 * 0 means auto (deficit derived from the PSI stall average,
	 * clamped to [64,256] MiB). An explicit value is bounded by
	 * TARGET_MAX_MIB: the victim array holds only
	 * MAX_VICTIMS entries, so a larger target cannot be reached in one
	 * reclaim. It would never satisfy pages_found >= target, and every
	 * subsequent PSI window would scan and kill again -- the repeated
	 * under-sized-reclaim pattern that the stall-derived target's
	 * floor exists to avoid.
	 */
	if (v != 0 && (v < TARGET_MIN_MIB || v > TARGET_MAX_MIB))
		return -EINVAL;
	target_mib = v;
	return 0;
}

static int set_reserve_mib(const char *val, const struct kernel_param *kp)
{
	unsigned int v = reserve_mib;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	/* 0 disables the floor; anything above 4 GiB would skip every reclaim */
	if (v > 4096)
		return -EINVAL;
	reserve_mib = v;
	return 0;
}

static int set_grace_msec(const char *val, const struct kernel_param *kp)
{
	unsigned int v = grace_msec;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	/* 0 disables the grace period; beyond 30s LMK is effectively off */
	if (v > GRACE_MSEC_MAX)
		return -EINVAL;
	grace_msec = v;
	return 0;
}

static int set_bg_grace_msec(const char *val, const struct kernel_param *kp)
{
	unsigned int v = bg_grace_msec;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	/* 0 disables the grace; beyond 60s freshly backgrounded apps never die */
	if (v > BG_GRACE_MSEC_MAX)
		return -EINVAL;
	bg_grace_msec = v;
	return 0;
}

static const struct kernel_param_ops poll_msec_ops = {
	.set = set_poll_msec,
	.get = param_get_uint,
};

static const struct kernel_param_ops max_kills_ops = {
	.set = set_max_kills,
	.get = param_get_uint,
};

static const struct kernel_param_ops target_mib_ops = {
	.set = set_target_mib,
	.get = param_get_uint,
};

static const struct kernel_param_ops reserve_mib_ops = {
	.set = set_reserve_mib,
	.get = param_get_uint,
};

static const struct kernel_param_ops grace_msec_ops = {
	.set = set_grace_msec,
	.get = param_get_uint,
};

static const struct kernel_param_ops bg_grace_msec_ops = {
	.set = set_bg_grace_msec,
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

	/*
	 * Claim the init slot before dropping slmk_lock. Thread creation and
	 * notifier registration take long enough for a second write to
	 * minfree (init and lmkd both write it) to land in the gap: it would
	 * see slmk_running == false, spawn a duplicate pair of kthreads, and
	 * re-register oom_notif -- list-adding the same notifier_block twice
	 * corrupts the notifier chain and hangs every out_of_memory().
	 */
	slmk_running = true;

	if (!psi_spec_valid()) {
		pr_err("Invalid PSI spec: threshold=%u window=%u\n",
		       psi_threshold_us, psi_window_us);
		slmk_running = false;
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
		goto unclaim;
	}

	reclaim = kthread_run(simple_lmk_reclaim_thread, NULL, "simple_lmkd");
	if (IS_ERR(reclaim)) {
		pr_err("Failed to create reclaim thread: %ld\n",
		       PTR_ERR(reclaim));
		kthread_stop(reaper);
		goto unclaim;
	}

	if (register_oom_notifier(&oom_notif)) {
		pr_err("Failed to register OOM notifier\n");
		kthread_stop(reaper);
		kthread_stop(reclaim);
		goto unclaim;
	}

	pr_info("Initialized: target=%lu MiB threshold=%u us window=%u us cap=%u grace=%u ms bg_grace=%u ms\n",
		reclaim_target_pages() * PAGE_SIZE / SZ_1M, psi_threshold_us,
		psi_window_us, max_kills, grace_msec, bg_grace_msec);

	/* Always succeed: a failure here would make lmkd think LMK is absent */
	return 0;

unclaim:
	/* Free the slot so a later write can retry the failed setup */
	mutex_lock(&slmk_lock);
	slmk_running = false;
	mutex_unlock(&slmk_lock);
	return 0;
}

static const struct kernel_param_ops simple_lmk_init_ops = {
	.set = simple_lmk_init_set
};

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "simple_lmk."

module_param_cb(psi_threshold_us, &psi_threshold_ops, &psi_threshold_us, 0644);
module_param_named(memcg_aware, memcg_aware, bool, 0644);
module_param_named(memcg_boost_pct, memcg_boost_pct, uint, 0644);
MODULE_PARM_DESC(psi_threshold_us,
		 "Microseconds of memory stall per window before reclaiming; not a percentage");
module_param_cb(psi_window_us, &psi_window_ops, &psi_window_us, 0644);
MODULE_PARM_DESC(psi_window_us, "PSI stall window in microseconds");
module_param_cb(target_mib, &target_mib_ops, &target_mib, 0644);
MODULE_PARM_DESC(target_mib,
		 "MiB to free per reclaim; 0 derives it from the PSI stall average");
module_param_cb(reserve_mib, &reserve_mib_ops, &reserve_mib, 0644);
MODULE_PARM_DESC(reserve_mib,
		"MiB of free memory below which routine reclaim may run; 0 disables");
module_param_cb(grace_msec, &grace_msec_ops, &grace_msec, 0644);
MODULE_PARM_DESC(grace_msec,
		"Minimum settle time after a killing reclaim, ms; 0 disables");
module_param_cb(bg_grace_msec, &bg_grace_msec_ops, &bg_grace_msec, 0644);
MODULE_PARM_DESC(bg_grace_msec,
		"Routine-path grace for freshly launched/backgrounded tasks, ms; 0 disables");
module_param_cb(max_kills, &max_kills_ops, &max_kills, 0644);
MODULE_PARM_DESC(max_kills, "Maximum processes killed by a single reclaim");
module_param_cb(poll_msec, &poll_msec_ops, &poll_msec, 0644);
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
module_param(stat_gated, ulong, 0444);
MODULE_PARM_DESC(stat_gated,
		 "Pressure events whose reclaim was suppressed by reserve_mib");
module_param(stat_grace_dropped, ulong, 0444);
MODULE_PARM_DESC(stat_grace_dropped,
		 "Pressure events whose reclaim was suppressed by grace_msec");

/* Needed to prevent Android from thinking there's no LMK and thus rebooting */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
module_param_cb(minfree, &simple_lmk_init_ops, NULL, 0200);

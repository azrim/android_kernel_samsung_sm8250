/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Read-Copy Update mechanism for mutual exclusion (tree-based version)
 * Internal non-public definitions that provide either classic
 * or preemptible semantics.
 *
 * Copyright Red Hat, 2009
 * Copyright IBM Corporation, 2009
 *
 * Author: Ingo Molnar <mingo@elte.hu>
 *	   Paul E. McKenney <paulmck@linux.vnet.ibm.com>
 */

#ifdef CONFIG_RCU_NOCB_CPU
static cpumask_var_t rcu_nocb_mask; /* CPUs to have callbacks offloaded. */
static bool __read_mostly rcu_nocb_poll;    /* Offload kthread are to poll. */
#endif /* #ifdef CONFIG_RCU_NOCB_CPU */

#ifdef CONFIG_RCU_NOCB_CPU

/*
 * Offload callback processing from the boot-time-specified set of CPUs
 * specified by rcu_nocb_mask.  For each CPU in the set, there is a
 * kthread created that pulls the callbacks from the corresponding CPU,
 * waits for a grace period to elapse, and invokes the callbacks.
 * The no-CBs CPUs do a wake_up() on their kthread when they insert
 * a callback into any empty list, unless the rcu_nocb_poll boot parameter
 * has been specified, in which case each kthread actively polls its
 * CPU.  (Which isn't so great for energy efficiency, but which does
 * reduce RCU's overhead on that CPU.)
 *
 * This is intended to be used in conjunction with Frederic Weisbecker's
 * adaptive-idle work, which would seriously reduce OS jitter on CPUs
 * running CPU-bound user-mode computations.
 *
 * Offloading of callback processing could also in theory be used as
 * an energy-efficiency measure because CPUs with no RCU callbacks
 * queued are more aggressive about entering dyntick-idle mode.
 */


/* Parse the boot-time rcu_nocb_mask CPU list from the kernel parameters. */
static int __init rcu_nocb_setup(char *str)
{
	alloc_bootmem_cpumask_var(&rcu_nocb_mask);
	cpulist_parse(str, rcu_nocb_mask);
	return 1;
}
__setup("rcu_nocbs=", rcu_nocb_setup);

static int __init parse_rcu_nocb_poll(char *arg)
{
	rcu_nocb_poll = true;
	return 0;
}
early_param("rcu_nocb_poll", parse_rcu_nocb_poll);

/*
 * Wake up any no-CBs CPUs' kthreads that were waiting on the just-ended
 * grace period.
 */
static void rcu_nocb_gp_cleanup(struct swait_queue_head *sq)
{
	swake_up_all(sq);
}

static struct swait_queue_head *rcu_nocb_gp_get(struct rcu_node *rnp)
{
	return &rnp->nocb_gp_wq[rcu_seq_ctr(rnp->gp_seq) & 0x1];
}

static void rcu_init_one_nocb(struct rcu_node *rnp)
{
	init_swait_queue_head(&rnp->nocb_gp_wq[0]);
	init_swait_queue_head(&rnp->nocb_gp_wq[1]);
}

/* Is the specified CPU a no-CBs CPU? */
bool rcu_is_nocb_cpu(int cpu)
{
	if (cpumask_available(rcu_nocb_mask))
		return cpumask_test_cpu(cpu, rcu_nocb_mask);
	return false;
}

/*
 * Kick the leader kthread for this NOCB group.  Caller holds ->nocb_lock
 * and this function releases it.
 */
static void __wake_nocb_leader(struct rcu_data *rdp, bool force,
			       unsigned long flags)
	__releases(rdp->nocb_lock)
{
	struct rcu_data *rdp_leader = rdp->nocb_leader;

	lockdep_assert_held(&rdp->nocb_lock);
	if (!READ_ONCE(rdp_leader->nocb_kthread)) {
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		return;
	}
	if (rdp_leader->nocb_leader_sleep || force) {
		/* Prior smp_mb__after_atomic() orders against prior enqueue. */
		WRITE_ONCE(rdp_leader->nocb_leader_sleep, false);
		del_timer(&rdp->nocb_timer);
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		smp_mb(); /* ->nocb_leader_sleep before swake_up_one(). */
		swake_up_one(&rdp_leader->nocb_wq);
	} else {
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
	}
}

/*
 * Kick the leader kthread for this NOCB group, but caller has not
 * acquired locks.
 */
static void wake_nocb_leader(struct rcu_data *rdp, bool force)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
	__wake_nocb_leader(rdp, force, flags);
}

/*
 * Arrange to wake the leader kthread for this NOCB group at some
 * future time when it is safe to do so.
 */
static void wake_nocb_leader_defer(struct rcu_data *rdp, int waketype,
				   const char *reason)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
	if (rdp->nocb_defer_wakeup == RCU_NOCB_WAKE_NOT)
		mod_timer(&rdp->nocb_timer, jiffies + 1);
	WRITE_ONCE(rdp->nocb_defer_wakeup, waketype);
	trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu, reason);
	raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
}

/*
 * Does the specified CPU need an RCU callback for the specified flavor
 * of rcu_barrier()?
 */
static bool rcu_nocb_cpu_needs_barrier(struct rcu_state *rsp, int cpu)
{
	struct rcu_data *rdp = per_cpu_ptr(rsp->rda, cpu);
	unsigned long ret;
#ifdef CONFIG_PROVE_RCU
	struct rcu_head *rhp;
#endif /* #ifdef CONFIG_PROVE_RCU */

	/*
	 * Check count of all no-CBs callbacks awaiting invocation.
	 * There needs to be a barrier before this function is called,
	 * but associated with a prior determination that no more
	 * callbacks would be posted.  In the worst case, the first
	 * barrier in _rcu_barrier() suffices (but the caller cannot
	 * necessarily rely on this, not a substitute for the caller
	 * getting the concurrency design right!).  There must also be
	 * a barrier between the following load an posting of a callback
	 * (if a callback is in fact needed).  This is associated with an
	 * atomic_inc() in the caller.
	 */
	ret = atomic_long_read(&rdp->nocb_q_count);

#ifdef CONFIG_PROVE_RCU
	rhp = READ_ONCE(rdp->nocb_head);
	if (!rhp)
		rhp = READ_ONCE(rdp->nocb_gp_head);
	if (!rhp)
		rhp = READ_ONCE(rdp->nocb_follower_head);

	/* Having no rcuo kthread but CBs after scheduler starts is bad! */
	if (!READ_ONCE(rdp->nocb_kthread) && rhp &&
	    rcu_scheduler_fully_active) {
		/* RCU callback enqueued before CPU first came online??? */
		pr_err("RCU: Never-onlined no-CBs CPU %d has CB %p\n",
		       cpu, rhp->func);
		WARN_ON_ONCE(1);
	}
#endif /* #ifdef CONFIG_PROVE_RCU */

	return !!ret;
}

/*
 * Enqueue the specified string of rcu_head structures onto the specified
 * CPU's no-CBs lists.  The CPU is specified by rdp, the head of the
 * string by rhp, and the tail of the string by rhtp.  The non-lazy/lazy
 * counts are supplied by rhcount and rhcount_lazy.
 *
 * If warranted, also wake up the kthread servicing this CPUs queues.
 */
static void __call_rcu_nocb_enqueue(struct rcu_data *rdp,
				    struct rcu_head *rhp,
				    struct rcu_head **rhtp,
				    int rhcount, int rhcount_lazy,
				    unsigned long flags)
{
	int len;
	struct rcu_head **old_rhpp;
	struct task_struct *t;

	/* Enqueue the callback on the nocb list and update counts. */
	atomic_long_add(rhcount, &rdp->nocb_q_count);
	/* rcu_barrier() relies on ->nocb_q_count add before xchg. */
	old_rhpp = xchg(&rdp->nocb_tail, rhtp);
	WRITE_ONCE(*old_rhpp, rhp);
	atomic_long_add(rhcount_lazy, &rdp->nocb_q_count_lazy);
	smp_mb__after_atomic(); /* Store *old_rhpp before _wake test. */

	/* If we are not being polled and there is a kthread, awaken it ... */
	t = READ_ONCE(rdp->nocb_kthread);
	if (rcu_nocb_poll || !t) {
		trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu,
				    TPS("WakeNotPoll"));
		return;
	}
	len = atomic_long_read(&rdp->nocb_q_count);
	if (old_rhpp == &rdp->nocb_head) {
		if (!irqs_disabled_flags(flags)) {
			/* ... if queue was empty ... */
			wake_nocb_leader(rdp, false);
			trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu,
					    TPS("WakeEmpty"));
		} else {
			wake_nocb_leader_defer(rdp, RCU_NOCB_WAKE,
					       TPS("WakeEmptyIsDeferred"));
		}
		rdp->qlen_last_fqs_check = 0;
	} else if (len > rdp->qlen_last_fqs_check + qhimark) {
		/* ... or if many callbacks queued. */
		if (!irqs_disabled_flags(flags)) {
			wake_nocb_leader(rdp, true);
			trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu,
					    TPS("WakeOvf"));
		} else {
			wake_nocb_leader_defer(rdp, RCU_NOCB_WAKE_FORCE,
					       TPS("WakeOvfIsDeferred"));
		}
		rdp->qlen_last_fqs_check = LONG_MAX / 2;
	} else {
		trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu, TPS("WakeNot"));
	}
	return;
}

/*
 * This is a helper for __call_rcu(), which invokes this when the normal
 * callback queue is inoperable.  If this is not a no-CBs CPU, this
 * function returns failure back to __call_rcu(), which can complain
 * appropriately.
 *
 * Otherwise, this function queues the callback where the corresponding
 * "rcuo" kthread can find it.
 */
static bool __call_rcu_nocb(struct rcu_data *rdp, struct rcu_head *rhp,
			    bool lazy, unsigned long flags)
{

	if (!rcu_is_nocb_cpu(rdp->cpu))
		return false;
	__call_rcu_nocb_enqueue(rdp, rhp, &rhp->next, 1, lazy, flags);
	if (__is_kvfree_rcu_offset((unsigned long)rhp->func))
		trace_rcu_kfree_callback(rdp->rsp->name, rhp,
					 (unsigned long)rhp->func,
					 -atomic_long_read(&rdp->nocb_q_count_lazy),
					 -atomic_long_read(&rdp->nocb_q_count));
	else
		trace_rcu_callback(rdp->rsp->name, rhp,
				   -atomic_long_read(&rdp->nocb_q_count_lazy),
				   -atomic_long_read(&rdp->nocb_q_count));

	/*
	 * If called from an extended quiescent state with interrupts
	 * disabled, invoke the RCU core in order to allow the idle-entry
	 * deferred-wakeup check to function.
	 */
	if (irqs_disabled_flags(flags) &&
	    !rcu_is_watching() &&
	    cpu_online(smp_processor_id()))
		invoke_rcu_core();

	return true;
}

/*
 * Adopt orphaned callbacks on a no-CBs CPU, or return 0 if this is
 * not a no-CBs CPU.
 */
static bool __maybe_unused rcu_nocb_adopt_orphan_cbs(struct rcu_data *my_rdp,
						     struct rcu_data *rdp,
						     unsigned long flags)
{
	lockdep_assert_irqs_disabled();
	if (!rcu_is_nocb_cpu(smp_processor_id()))
		return false; /* Not NOCBs CPU, caller must migrate CBs. */
	__call_rcu_nocb_enqueue(my_rdp, rcu_segcblist_head(&rdp->cblist),
				rcu_segcblist_tail(&rdp->cblist),
				rcu_segcblist_n_cbs(&rdp->cblist),
				rcu_segcblist_n_lazy_cbs(&rdp->cblist), flags);
	rcu_segcblist_init(&rdp->cblist);
	rcu_segcblist_disable(&rdp->cblist);
	return true;
}

/*
 * If necessary, kick off a new grace period, and either way wait
 * for a subsequent grace period to complete.
 */
static void rcu_nocb_wait_gp(struct rcu_data *rdp)
{
	unsigned long c;
	bool d;
	unsigned long flags;
	bool needwake;
	struct rcu_node *rnp = rdp->mynode;

	local_irq_save(flags);
	c = rcu_seq_snap(&rdp->rsp->gp_seq);
	if (!rdp->gpwrap && ULONG_CMP_GE(rdp->gp_seq_needed, c)) {
		local_irq_restore(flags);
	} else {
		raw_spin_lock_rcu_node(rnp); /* irqs already disabled. */
		needwake = rcu_start_this_gp(rnp, rdp, c);
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		if (needwake)
			rcu_gp_kthread_wake(rdp->rsp);
	}

	/*
	 * Wait for the grace period.  Do so interruptibly to avoid messing
	 * up the load average.
	 */
	trace_rcu_this_gp(rnp, rdp, c, TPS("StartWait"));
	for (;;) {
		swait_event_interruptible_exclusive(
			rnp->nocb_gp_wq[rcu_seq_ctr(c) & 0x1],
			(d = rcu_seq_done(&rnp->gp_seq, c)));
		if (likely(d))
			break;
		WARN_ON(signal_pending(current));
		trace_rcu_this_gp(rnp, rdp, c, TPS("ResumeWait"));
	}
	trace_rcu_this_gp(rnp, rdp, c, TPS("EndWait"));
	smp_mb(); /* Ensure that CB invocation happens after GP end. */
}

/*
 * Leaders come here to wait for additional callbacks to show up.
 * This function does not return until callbacks appear.
 */
static void nocb_leader_wait(struct rcu_data *my_rdp)
{
	bool firsttime = true;
	unsigned long flags;
	bool gotcbs;
	struct rcu_data *rdp;
	struct rcu_head **tail;

wait_again:

	/* Wait for callbacks to appear. */
	if (!rcu_nocb_poll) {
		trace_rcu_nocb_wake(my_rdp->rsp->name, my_rdp->cpu, TPS("Sleep"));
		swait_event_interruptible_exclusive(my_rdp->nocb_wq,
				!READ_ONCE(my_rdp->nocb_leader_sleep));
		raw_spin_lock_irqsave(&my_rdp->nocb_lock, flags);
		my_rdp->nocb_leader_sleep = true;
		WRITE_ONCE(my_rdp->nocb_defer_wakeup, RCU_NOCB_WAKE_NOT);
		del_timer(&my_rdp->nocb_timer);
		raw_spin_unlock_irqrestore(&my_rdp->nocb_lock, flags);
	} else if (firsttime) {
		firsttime = false; /* Don't drown trace log with "Poll"! */
		trace_rcu_nocb_wake(my_rdp->rsp->name, my_rdp->cpu, TPS("Poll"));
	}

	/*
	 * Each pass through the following loop checks a follower for CBs.
	 * We are our own first follower.  Any CBs found are moved to
	 * nocb_gp_head, where they await a grace period.
	 */
	gotcbs = false;
	smp_mb(); /* wakeup and _sleep before ->nocb_head reads. */
	for (rdp = my_rdp; rdp; rdp = rdp->nocb_next_follower) {
		rdp->nocb_gp_head = READ_ONCE(rdp->nocb_head);
		if (!rdp->nocb_gp_head)
			continue;  /* No CBs here, try next follower. */

		/* Move callbacks to wait-for-GP list, which is empty. */
		WRITE_ONCE(rdp->nocb_head, NULL);
		rdp->nocb_gp_tail = xchg(&rdp->nocb_tail, &rdp->nocb_head);
		gotcbs = true;
	}

	/* No callbacks?  Sleep a bit if polling, and go retry.  */
	if (unlikely(!gotcbs)) {
		WARN_ON(signal_pending(current));
		if (rcu_nocb_poll) {
			schedule_timeout_interruptible(1);
		} else {
			trace_rcu_nocb_wake(my_rdp->rsp->name, my_rdp->cpu,
					    TPS("WokeEmpty"));
		}
		goto wait_again;
	}

	/* Wait for one grace period. */
	rcu_nocb_wait_gp(my_rdp);

	/* Each pass through the following loop wakes a follower, if needed. */
	for (rdp = my_rdp; rdp; rdp = rdp->nocb_next_follower) {
		if (!rcu_nocb_poll &&
		    READ_ONCE(rdp->nocb_head) &&
		    READ_ONCE(my_rdp->nocb_leader_sleep)) {
			raw_spin_lock_irqsave(&my_rdp->nocb_lock, flags);
			my_rdp->nocb_leader_sleep = false;/* No need to sleep.*/
			raw_spin_unlock_irqrestore(&my_rdp->nocb_lock, flags);
		}
		if (!rdp->nocb_gp_head)
			continue; /* No CBs, so no need to wake follower. */

		/* Append callbacks to follower's "done" list. */
		raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
		tail = rdp->nocb_follower_tail;
		rdp->nocb_follower_tail = rdp->nocb_gp_tail;
		*tail = rdp->nocb_gp_head;
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		if (rdp != my_rdp && tail == &rdp->nocb_follower_head) {
			/* List was empty, so wake up the follower.  */
			swake_up_one(&rdp->nocb_wq);
		}
	}

	/* If we (the leader) don't have CBs, go wait some more. */
	if (!my_rdp->nocb_follower_head)
		goto wait_again;
}

/*
 * Followers come here to wait for additional callbacks to show up.
 * This function does not return until callbacks appear.
 */
static void nocb_follower_wait(struct rcu_data *rdp)
{
	for (;;) {
		trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu, TPS("FollowerSleep"));
		swait_event_interruptible_exclusive(rdp->nocb_wq,
					 READ_ONCE(rdp->nocb_follower_head));
		if (smp_load_acquire(&rdp->nocb_follower_head)) {
			/* ^^^ Ensure CB invocation follows _head test. */
			return;
		}
		WARN_ON(signal_pending(current));
		trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu, TPS("WokeEmpty"));
	}
}

/*
 * Per-rcu_data kthread, but only for no-CBs CPUs.  Each kthread invokes
 * callbacks queued by the corresponding no-CBs CPU, however, there is
 * an optional leader-follower relationship so that the grace-period
 * kthreads don't have to do quite so many wakeups.
 */
static int rcu_nocb_kthread(void *arg)
{
	int c, cl;
	unsigned long flags;
	struct rcu_head *list;
	struct rcu_head *next;
	struct rcu_head **tail;
	struct rcu_data *rdp = arg;

	/* Each pass through this loop invokes one batch of callbacks */
	for (;;) {
		/* Wait for callbacks. */
		if (rdp->nocb_leader == rdp)
			nocb_leader_wait(rdp);
		else
			nocb_follower_wait(rdp);

		/* Pull the ready-to-invoke callbacks onto local list. */
		raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
		list = rdp->nocb_follower_head;
		rdp->nocb_follower_head = NULL;
		tail = rdp->nocb_follower_tail;
		rdp->nocb_follower_tail = &rdp->nocb_follower_head;
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		BUG_ON(!list);
		trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu, TPS("WokeNonEmpty"));

		/* Each pass through the following loop invokes a callback. */
		trace_rcu_batch_start(rdp->rsp->name,
				      atomic_long_read(&rdp->nocb_q_count_lazy),
				      atomic_long_read(&rdp->nocb_q_count), -1);
		c = cl = 0;
		while (list) {
			next = list->next;
			/* Wait for enqueuing to complete, if needed. */
			while (next == NULL && &list->next != tail) {
				trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu,
						    TPS("WaitQueue"));
				schedule_timeout_interruptible(1);
				trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu,
						    TPS("WokeQueue"));
				next = list->next;
			}
			debug_rcu_head_unqueue(list);
			local_bh_disable();
			if (__rcu_reclaim(rdp->rsp->name, list))
				cl++;
			c++;
			local_bh_enable();
			cond_resched_tasks_rcu_qs();
			list = next;
		}
		trace_rcu_batch_end(rdp->rsp->name, c, !!list, 0, 0, 1);
		smp_mb__before_atomic();  /* _add after CB invocation. */
		atomic_long_add(-c, &rdp->nocb_q_count);
		atomic_long_add(-cl, &rdp->nocb_q_count_lazy);
	}
	return 0;
}

/* Is a deferred wakeup of rcu_nocb_kthread() required? */
static int rcu_nocb_need_deferred_wakeup(struct rcu_data *rdp)
{
	return READ_ONCE(rdp->nocb_defer_wakeup);
}

/* Do a deferred wakeup of rcu_nocb_kthread(). */
static void do_nocb_deferred_wakeup_common(struct rcu_data *rdp)
{
	unsigned long flags;
	int ndw;

	raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
	if (!rcu_nocb_need_deferred_wakeup(rdp)) {
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		return;
	}
	ndw = READ_ONCE(rdp->nocb_defer_wakeup);
	WRITE_ONCE(rdp->nocb_defer_wakeup, RCU_NOCB_WAKE_NOT);
	__wake_nocb_leader(rdp, ndw == RCU_NOCB_WAKE_FORCE, flags);
	trace_rcu_nocb_wake(rdp->rsp->name, rdp->cpu, TPS("DeferredWake"));
}

/* Do a deferred wakeup of rcu_nocb_kthread() from a timer handler. */
static void do_nocb_deferred_wakeup_timer(struct timer_list *t)
{
	struct rcu_data *rdp = from_timer(rdp, t, nocb_timer);

	do_nocb_deferred_wakeup_common(rdp);
}

/*
 * Do a deferred wakeup of rcu_nocb_kthread() from fastpath.
 * This means we do an inexact common-case check.  Note that if
 * we miss, ->nocb_timer will eventually clean things up.
 */
static void do_nocb_deferred_wakeup(struct rcu_data *rdp)
{
	if (rcu_nocb_need_deferred_wakeup(rdp))
		do_nocb_deferred_wakeup_common(rdp);
}

void __init rcu_init_nohz(void)
{
	int cpu;
	bool need_rcu_nocb_mask = false;
	struct rcu_state *rsp;

#if defined(CONFIG_NO_HZ_FULL)
	if (tick_nohz_full_running && cpumask_weight(tick_nohz_full_mask))
		need_rcu_nocb_mask = true;
#endif /* #if defined(CONFIG_NO_HZ_FULL) */

	if (!cpumask_available(rcu_nocb_mask) && need_rcu_nocb_mask) {
		if (!zalloc_cpumask_var(&rcu_nocb_mask, GFP_KERNEL)) {
			pr_info("rcu_nocb_mask allocation failed, callback offloading disabled.\n");
			return;
		}
	}
	if (!cpumask_available(rcu_nocb_mask))
		return;

#if defined(CONFIG_NO_HZ_FULL)
	if (tick_nohz_full_running)
		cpumask_or(rcu_nocb_mask, rcu_nocb_mask, tick_nohz_full_mask);
#endif /* #if defined(CONFIG_NO_HZ_FULL) */

	if (!cpumask_subset(rcu_nocb_mask, cpu_possible_mask)) {
		pr_info("\tNote: kernel parameter 'rcu_nocbs=', 'nohz_full', or 'isolcpus=' contains nonexistent CPUs.\n");
		cpumask_and(rcu_nocb_mask, cpu_possible_mask,
			    rcu_nocb_mask);
	}
	if (cpumask_empty(rcu_nocb_mask))
		pr_info("\tOffload RCU callbacks from CPUs: (none).\n");
	else
		pr_info("\tOffload RCU callbacks from CPUs: %*pbl.\n",
			cpumask_pr_args(rcu_nocb_mask));
	if (rcu_nocb_poll)
		pr_info("\tPoll for callbacks from no-CBs CPUs.\n");

	for_each_rcu_flavor(rsp) {
		for_each_cpu(cpu, rcu_nocb_mask)
			init_nocb_callback_list(per_cpu_ptr(rsp->rda, cpu));
		rcu_organize_nocb_kthreads(rsp);
	}
}

/* Initialize per-rcu_data variables for no-CBs CPUs. */
static void __init rcu_boot_init_nocb_percpu_data(struct rcu_data *rdp)
{
	rdp->nocb_tail = &rdp->nocb_head;
	init_swait_queue_head(&rdp->nocb_wq);
	rdp->nocb_follower_tail = &rdp->nocb_follower_head;
	raw_spin_lock_init(&rdp->nocb_lock);
	timer_setup(&rdp->nocb_timer, do_nocb_deferred_wakeup_timer, 0);
}

/*
 * If the specified CPU is a no-CBs CPU that does not already have its
 * rcuo kthread for the specified RCU flavor, spawn it.  If the CPUs are
 * brought online out of order, this can require re-organizing the
 * leader-follower relationships.
 */
static void rcu_spawn_one_nocb_kthread(struct rcu_state *rsp, int cpu)
{
	struct rcu_data *rdp;
	struct rcu_data *rdp_last;
	struct rcu_data *rdp_old_leader;
	struct rcu_data *rdp_spawn = per_cpu_ptr(rsp->rda, cpu);
	struct task_struct *t;

	/*
	 * If this isn't a no-CBs CPU or if it already has an rcuo kthread,
	 * then nothing to do.
	 */
	if (!rcu_is_nocb_cpu(cpu) || rdp_spawn->nocb_kthread)
		return;

	/* If we didn't spawn the leader first, reorganize! */
	rdp_old_leader = rdp_spawn->nocb_leader;
	if (rdp_old_leader != rdp_spawn && !rdp_old_leader->nocb_kthread) {
		rdp_last = NULL;
		rdp = rdp_old_leader;
		do {
			rdp->nocb_leader = rdp_spawn;
			if (rdp_last && rdp != rdp_spawn)
				rdp_last->nocb_next_follower = rdp;
			if (rdp == rdp_spawn) {
				rdp = rdp->nocb_next_follower;
			} else {
				rdp_last = rdp;
				rdp = rdp->nocb_next_follower;
				rdp_last->nocb_next_follower = NULL;
			}
		} while (rdp);
		rdp_spawn->nocb_next_follower = rdp_old_leader;
	}

	/* Spawn the kthread for this CPU and RCU flavor. */
	t = kthread_run(rcu_nocb_kthread, rdp_spawn,
			"rcuo%c/%d", rsp->abbr, cpu);
	BUG_ON(IS_ERR(t));
	WRITE_ONCE(rdp_spawn->nocb_kthread, t);
}

/*
 * If the specified CPU is a no-CBs CPU that does not already have its
 * rcuo kthreads, spawn them.
 */
static void rcu_spawn_all_nocb_kthreads(int cpu)
{
	struct rcu_state *rsp;

	if (rcu_scheduler_fully_active)
		for_each_rcu_flavor(rsp)
			rcu_spawn_one_nocb_kthread(rsp, cpu);
}

/*
 * Once the scheduler is running, spawn rcuo kthreads for all online
 * no-CBs CPUs.  This assumes that the early_initcall()s happen before
 * non-boot CPUs come online -- if this changes, we will need to add
 * some mutual exclusion.
 */
static void __init rcu_spawn_nocb_kthreads(void)
{
	int cpu;

	for_each_online_cpu(cpu)
		rcu_spawn_all_nocb_kthreads(cpu);
}

/* How many follower CPU IDs per leader?  Default of -1 for sqrt(nr_cpu_ids). */
static int rcu_nocb_leader_stride = -1;
module_param(rcu_nocb_leader_stride, int, 0444);

/*
 * Initialize leader-follower relationships for all no-CBs CPU.
 */
static void __init rcu_organize_nocb_kthreads(struct rcu_state *rsp)
{
	int cpu;
	int ls = rcu_nocb_leader_stride;
	int nl = 0;  /* Next leader. */
	struct rcu_data *rdp;
	struct rcu_data *rdp_leader = NULL;  /* Suppress misguided gcc warn. */
	struct rcu_data *rdp_prev = NULL;

	if (!cpumask_available(rcu_nocb_mask))
		return;
	if (ls == -1) {
		ls = int_sqrt(nr_cpu_ids);
		rcu_nocb_leader_stride = ls;
	}

	/*
	 * Each pass through this loop sets up one rcu_data structure.
	 * Should the corresponding CPU come online in the future, then
	 * we will spawn the needed set of rcu_nocb_kthread() kthreads.
	 */
	for_each_cpu(cpu, rcu_nocb_mask) {
		rdp = per_cpu_ptr(rsp->rda, cpu);
		if (rdp->cpu >= nl) {
			/* New leader, set up for followers & next leader. */
			nl = DIV_ROUND_UP(rdp->cpu + 1, ls) * ls;
			rdp->nocb_leader = rdp;
			rdp_leader = rdp;
		} else {
			/* Another follower, link to previous leader. */
			rdp->nocb_leader = rdp_leader;
			rdp_prev->nocb_next_follower = rdp;
		}
		rdp_prev = rdp;
	}
}

/* Prevent __call_rcu() from enqueuing callbacks on no-CBs CPUs */
static bool init_nocb_callback_list(struct rcu_data *rdp)
{
	if (!rcu_is_nocb_cpu(rdp->cpu))
		return false;

	/* If there are early-boot callbacks, move them to nocb lists. */
	if (!rcu_segcblist_empty(&rdp->cblist)) {
		rdp->nocb_head = rcu_segcblist_head(&rdp->cblist);
		rdp->nocb_tail = rcu_segcblist_tail(&rdp->cblist);
		atomic_long_set(&rdp->nocb_q_count,
				rcu_segcblist_n_cbs(&rdp->cblist));
		atomic_long_set(&rdp->nocb_q_count_lazy,
				rcu_segcblist_n_lazy_cbs(&rdp->cblist));
		rcu_segcblist_init(&rdp->cblist);
	}
	rcu_segcblist_disable(&rdp->cblist);
	return true;
}

#else /* #ifdef CONFIG_RCU_NOCB_CPU */

static bool rcu_nocb_cpu_needs_barrier(struct rcu_state *rsp, int cpu)
{
	WARN_ON_ONCE(1); /* Should be dead code. */
	return false;
}

static void rcu_nocb_gp_cleanup(struct swait_queue_head *sq)
{
}

static struct swait_queue_head *rcu_nocb_gp_get(struct rcu_node *rnp)
{
	return NULL;
}

static void rcu_init_one_nocb(struct rcu_node *rnp)
{
}

static bool __call_rcu_nocb(struct rcu_data *rdp, struct rcu_head *rhp,
			    bool lazy, unsigned long flags)
{
	return false;
}

static bool __maybe_unused rcu_nocb_adopt_orphan_cbs(struct rcu_data *my_rdp,
						     struct rcu_data *rdp,
						     unsigned long flags)
{
	return false;
}

static void __init rcu_boot_init_nocb_percpu_data(struct rcu_data *rdp)
{
}

static int rcu_nocb_need_deferred_wakeup(struct rcu_data *rdp)
{
	return false;
}

static void do_nocb_deferred_wakeup(struct rcu_data *rdp)
{
}

static void rcu_spawn_all_nocb_kthreads(int cpu)
{
}

static void __init rcu_spawn_nocb_kthreads(void)
{
}

static bool init_nocb_callback_list(struct rcu_data *rdp)
{
	return false;
}

#endif /* #else #ifdef CONFIG_RCU_NOCB_CPU */

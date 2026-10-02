/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CONTEXT_TRACKING_H
#define _LINUX_CONTEXT_TRACKING_H

#include <linux/sched.h>
#include <linux/vtime.h>
#include <linux/context_tracking_state.h>

#include <asm/ptrace.h>


#ifdef CONFIG_CONTEXT_TRACKING_USER
extern void ct_cpu_track_user(int cpu);

/* Called with interrupts disabled.  */
extern void __ct_user_enter(enum ctx_state state);
extern void __ct_user_exit(enum ctx_state state);

extern void ct_user_enter(enum ctx_state state);
extern void ct_user_exit(enum ctx_state state);

extern void user_enter_callable(void);
extern void user_exit_callable(void);

static inline void user_enter(void)
{
	if (context_tracking_enabled())
		ct_user_enter(CT_STATE_USER);

}
static inline void user_exit(void)
{
	if (context_tracking_enabled())
		ct_user_exit(CT_STATE_USER);
}

/* Called with interrupts disabled.  */
static __always_inline void user_enter_irqoff(void)
{
	if (context_tracking_enabled())
		__ct_user_enter(CT_STATE_USER);

}
static __always_inline void user_exit_irqoff(void)
{
	if (context_tracking_enabled())
		__ct_user_exit(CT_STATE_USER);
}

static inline enum ctx_state exception_enter(void)
{
	enum ctx_state prev_ctx;

	if (IS_ENABLED(CONFIG_HAVE_CONTEXT_TRACKING_USER_OFFSTACK) ||
	    !context_tracking_enabled())
		return 0;

	prev_ctx = __ct_state();
	if (prev_ctx != CT_STATE_KERNEL)
		ct_user_exit(prev_ctx);

	return prev_ctx;
}

static inline void exception_exit(enum ctx_state prev_ctx)
{
	if (!IS_ENABLED(CONFIG_HAVE_CONTEXT_TRACKING_USER_OFFSTACK) &&
	    context_tracking_enabled()) {
		if (prev_ctx != CT_STATE_KERNEL)
			ct_user_enter(prev_ctx);
	}
}

static __always_inline bool context_tracking_guest_enter(void)
{
	if (context_tracking_enabled())
		__ct_user_enter(CT_STATE_GUEST);

	return context_tracking_enabled_this_cpu();
}

static __always_inline bool context_tracking_guest_exit(void)
{
	if (context_tracking_enabled())
		__ct_user_exit(CT_STATE_GUEST);

	return context_tracking_enabled_this_cpu();
}

#define CT_WARN_ON(cond) WARN_ON(context_tracking_enabled() && (cond))

#else
static inline void user_enter(void) { }
static inline void user_exit(void) { }
static inline void user_enter_irqoff(void) { }
static inline void user_exit_irqoff(void) { }
static inline int exception_enter(void) { return 0; }
static inline void exception_exit(enum ctx_state prev_ctx) { }
static inline int ct_state(void) { return -1; }
static inline int __ct_state(void) { return -1; }
static __always_inline bool context_tracking_guest_enter(void) { return false; }
static __always_inline bool context_tracking_guest_exit(void) { return false; }
#define CT_WARN_ON(cond) do { } while (0)
#endif /* !CONFIG_CONTEXT_TRACKING_USER */

#ifdef CONFIG_CONTEXT_TRACKING_USER_FORCE
extern void context_tracking_init(void);
#else
static inline void context_tracking_init(void) { }
#endif /* CONFIG_CONTEXT_TRACKING_USER_FORCE */

#ifdef CONFIG_CONTEXT_TRACKING_IDLE
extern void ct_idle_enter(void);
extern void ct_idle_exit(void);

/*
 * Is RCU watching the current CPU (IOW, it is not in an extended quiescent state)?
 *
 * Note that this returns the actual boolean data (watching / not watching),
 * whereas ct_rcu_watching() returns the RCU_WATCHING subvariable of
 * context_tracking.state.
 *
 * No ordering, as we are sampling CPU-local information.
 */
static __always_inline bool rcu_is_watching_curr_cpu(void)
{
	return atomic_read(this_cpu_ptr(&context_tracking.state)) & CT_RCU_WATCHING;
}

/*
 * Increment the current CPU's context_tracking structure's ->state field
 * with ordering.  Return the new value.
 */
static __always_inline unsigned long ct_state_inc(int incby)
{
	return atomic_add_return(incby, this_cpu_ptr(&context_tracking.state));
}

static __always_inline bool warn_rcu_enter(void)
{
	bool ret = false;

	/*
	 * Horrible hack to shut up recursive RCU isn't watching fail since
	 * lots of the actual reporting also relies on RCU.
	 */
	preempt_disable_notrace();
	if (!rcu_is_watching_curr_cpu()) {
		ret = true;
		ct_state_inc(CT_RCU_WATCHING);
	}

	return ret;
}

static __always_inline void warn_rcu_exit(bool rcu)
{
	if (rcu)
		ct_state_inc(CT_RCU_WATCHING);
	preempt_enable_notrace();
}

#else
static inline void ct_idle_enter(void) { }
static inline void ct_idle_exit(void) { }

static __always_inline bool warn_rcu_enter(void) { return false; }
static __always_inline void warn_rcu_exit(bool rcu) { }
#endif /* !CONFIG_CONTEXT_TRACKING_IDLE */


/*
 * Guest entry/exit helpers.
 *
 * NOTE: v6.12 moved these into include/linux/kvm_host.h as part of a
 * separate KVM entry rework.  They are kept here (their 4.19 location)
 * but retargeted onto the new ct API, so that KVM keeps working.
 */
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
/* must be called with irqs disabled */
static inline void guest_enter_irqoff(void)
{
	if (vtime_accounting_cpu_enabled())
		vtime_guest_enter(current);
	else
		current->flags |= PF_VCPU;

	if (context_tracking_enabled())
		__ct_user_enter(CT_STATE_GUEST);

	/* KVM does not hold any references to rcu protected data when it
	 * switches CPU into a guest mode. In fact switching to a guest mode
	 * is very similar to exiting to userspace from rcu point of view. In
	 * addition CPU may stay in a guest mode for quite a long time (up to
	 * one time slice). Lets treat guest mode as quiescent state, just like
	 * we do with user-mode execution.
	 */
	if (!context_tracking_enabled_this_cpu())
		rcu_virt_note_context_switch(smp_processor_id());
}

static inline void guest_exit_irqoff(void)
{
	if (context_tracking_enabled())
		__ct_user_exit(CT_STATE_GUEST);

	if (vtime_accounting_cpu_enabled())
		vtime_guest_exit(current);
	else
		current->flags &= ~PF_VCPU;
}

#else
static inline void guest_enter_irqoff(void)
{
	/*
	 * This is running in ioctl context so its safe
	 * to assume that it's the stime pending cputime
	 * to flush.
	 */
	vtime_account_system(current);
	current->flags |= PF_VCPU;
	rcu_virt_note_context_switch(smp_processor_id());
}

static inline void guest_exit_irqoff(void)
{
	/* Flush the guest cputime we spent on the guest */
	vtime_account_system(current);
	current->flags &= ~PF_VCPU;
}
#endif /* CONFIG_VIRT_CPU_ACCOUNTING_GEN */

static inline void guest_enter(void)
{
	unsigned long flags;

	local_irq_save(flags);
	guest_enter_irqoff();
	local_irq_restore(flags);
}

static inline void guest_exit(void)
{
	unsigned long flags;

	local_irq_save(flags);
	guest_exit_irqoff();
	local_irq_restore(flags);
}

#endif

/*
 * SUSFS v2.x tracks the process identity with thread_info flags
 * (TIF_PROC_NO_SU / TIF_PROC_UMOUNTED / TIF_PROC_UMOUNTED_FOR_ZYGOTE_NEXT)
 * instead of the v1.5.5 per-task latch.  All of the SUSFS kernel hooks gate
 * their hiding on those flags, so they must be set here for every
 * zygote / zygote_next spawned process.
 */
#ifdef CONFIG_KSU_SUSFS
#include <linux/workqueue.h>
#endif

static __always_inline void ksu_handle_extra_susfs_work(void)
{
#ifdef CONFIG_KSU_SUSFS
	extern struct work_struct susfs_extra_works;

	if (work_pending(&susfs_extra_works))
		return;
	schedule_work(&susfs_extra_works);
#endif
}

#ifdef CONFIG_KSU_SUSFS
extern u32 susfs_zygote_sid;
extern u32 susfs_zygote_next_sid;

static __always_inline void ksu_handle_zygote_setresuid(struct cred *new,
							const struct cred *old,
							uid_t ruid,
							bool is_zygote_next)
{
	// Check if spawned process is isolated service first, and force to do umount if so
	if (is_isolated_process(ruid)) {
		susfs_set_current_proc_no_su();
		susfs_set_current_proc_umounted();
		if (is_zygote_next)
			susfs_set_current_proc_umounted_for_zygote_next();
		goto do_susfs_work;
	}

	// - Since ksu manager app uid is excluded in allow_list_arr, ksu_uid_should_umount(manager_uid)
	//   always returns true, so check the manager uid explicitly.
	// - Disable seccomp restriction for the manager since running with "su" disables seccomp anyway
	if (likely(ksu_is_manager_appid_valid()) && unlikely(is_uid_manager(ruid))) {
		disable_seccomp();
		pr_info("install fd for manager: %d\n", ruid);
		ksu_install_fd();
		return;
	}

	// - Check if spawned process is a normal user app and needs to be umounted
	if (likely(is_appuid(ruid) && ksu_uid_should_umount(ruid))) {
		susfs_set_current_proc_no_su();
		susfs_set_current_proc_umounted();
		if (is_zygote_next)
			susfs_set_current_proc_umounted_for_zygote_next();
		goto do_susfs_work;
	}

	// Disable seccomp restriction for root allowed apps since running with "su" disables seccomp anyway
	if (ksu_is_allow_uid_for_current(ruid)) {
		disable_seccomp();
		return;
	}

	// Process not umounted but also root not allowed
	susfs_set_current_proc_no_su();
	return;

do_susfs_work:
	{
		// - zygote_next spawned processes live in the init mount namespace,
		//   so there is nothing to umount here.
		if (!is_zygote_next)
			ksu_handle_umount(new, old);

		// Handle extra susfs work (deferred sus_path re-flag loop)
		ksu_handle_extra_susfs_work();
	}
}
#endif // CONFIG_KSU_SUSFS

static __always_inline void ksu_handle_setresuid_cred(struct cred *new, const struct cred *old)
{
	uid_t new_uid;
	uid_t old_uid;

	if (!new || !old)
		return;

	new_uid = ksu_get_uid_t(new->uid);
	old_uid = ksu_get_uid_t(old->uid);

	// old process is not root, ignore it.
	if (unlikely(!!old_uid))
		return;

	if (IS_ENABLED(CONFIG_KSU_DEBUG))
		pr_info("handle_setresuid from %d to %d\n", old_uid, new_uid);

#ifdef CONFIG_KSU_SUSFS
	// We only interest in processes spawned by zygote or zygote_next
	if (susfs_is_sid_equal(old, susfs_zygote_sid)) {
		ksu_handle_zygote_setresuid(new, old, new_uid, false);
		return;
	}
	if (susfs_is_sid_equal(old, susfs_zygote_next_sid)) {
		ksu_handle_zygote_setresuid(new, old, new_uid, true);
		return;
	}
#endif // CONFIG_KSU_SUSFS

	// we dont have those new fancy things upstream has
	// lets just do the original thing where we disable seccomp
	if (unlikely(is_uid_manager(new_uid)))
		goto install_ksu_fd;

	if (ksu_is_allow_uid_for_current(new_uid))
		goto kill_seccomp;

	// Handle kernel umount
	ksu_handle_umount(new, old);
	return;

install_ksu_fd:
	pr_info("install fd for manager: %d\n", new_uid);
	ksu_install_fd();

kill_seccomp:
	disable_seccomp();
	set_thread_flag(TIF_KSU_MANAGED); // sucompat fast-path
	return;
}

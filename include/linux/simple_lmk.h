/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019-2020 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
#ifndef _SIMPLE_LMK_H_
#define _SIMPLE_LMK_H_

struct mm_struct;

struct mem_cgroup;

#ifdef CONFIG_ANDROID_SIMPLE_LMK
void simple_lmk_mm_freed(struct mm_struct *mm);
/*
 * Memcg OOM: arm a Simple LMK pass scoped to @memcg.  Safe from the
 * allocator / memcg charge path (must not sleep).  The caller still
 * decides the charge outcome; this only queues the kill work.
 */
void simple_lmk_notify_memcg_oom(struct mem_cgroup *memcg);
#else
static inline void simple_lmk_mm_freed(struct mm_struct *mm)
{
}
static inline void simple_lmk_notify_memcg_oom(struct mem_cgroup *memcg)
{
}
#endif

#endif /* _SIMPLE_LMK_H_ */

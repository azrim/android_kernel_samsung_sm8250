// SPDX-License-Identifier: GPL-2.0
/*
 * sec_mm/
 *
 * Copyright (C) 2020 Samsung Electronics
 *
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/ratelimit.h>
#include "sec_mm.h"

static DEFINE_RATELIMIT_STATE(mm_debug_rs, 30 * HZ, 1);

#define MIN_FILE_SIZE_HIGH	300
#define MIN_FILE_SIZE_LOW	200
#define MIN_FILE_SIZE_THR_GB	3
static unsigned long min_file;

/*
 * The full task dump is a few hundred lines and runs from the shrink_slab
 * path while shrinker_rwsem is held, so keep it off by default.
 * Enable with: echo 1 > /sys/module/sec_mm/parameters/dump_tasks
 */
static bool dump_tasks;
module_param(dump_tasks, bool, 0644);
MODULE_PARM_DESC(dump_tasks, "dump all tasks when low file cache is detected");

static unsigned long lowfile_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	unsigned long inactive_file, active_file, file;

	inactive_file = global_node_page_state(NR_INACTIVE_FILE);
	active_file = global_node_page_state(NR_ACTIVE_FILE);
	file = inactive_file + active_file;
	if (file < min_file && __ratelimit(&mm_debug_rs)) {
		pr_info("low file detected : %lukB < %luKB\n", K(file),
			K(min_file));
		mm_debug_show_free_areas();
		if (dump_tasks)
			mm_debug_dump_tasks();
	}

	return 0; /* return 0 not to call to scan_objects */
}

static struct shrinker mm_debug_shrinker = {
	.count_objects = lowfile_count,
};

void init_lowfile_detect(void)
{
	if (totalram_pages > GB_TO_PAGES(MIN_FILE_SIZE_THR_GB))
		min_file = MB_TO_PAGES(MIN_FILE_SIZE_HIGH);
	else
		min_file = MB_TO_PAGES(MIN_FILE_SIZE_LOW);

	register_shrinker(&mm_debug_shrinker);
}

void exit_lowfile_detect(void)
{
	unregister_shrinker(&mm_debug_shrinker);
}

MODULE_LICENSE("GPL");

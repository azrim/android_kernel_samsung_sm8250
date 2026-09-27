/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 MediaTek Inc.
 */

#ifndef _KCOMPRESSD_H_
#define _KCOMPRESSD_H_

#include <linux/types.h>

struct page;

typedef void (*compress_callback)(void *mem, struct page *page, u32 index,
				  int offset);

int kcompressd_enabled(void);
int schedule_bio_write(void *mem, struct page *page, u32 index, int offset,
		       compress_callback cb);

int kcompressd_init(void);
void kcompressd_exit(void);

#endif /* _KCOMPRESSD_H_ */

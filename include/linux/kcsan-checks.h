/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal KCSAN stub for the 4.19 tree.
 *
 * The v5.15 RCU core uses the KCSAN check helpers and the
 * ASSERT_EXCLUSIVE_* annotations.  This kernel has no KCSAN, so provide
 * no-op equivalents.
 */
#ifndef _LINUX_KCSAN_CHECKS_H
#define _LINUX_KCSAN_CHECKS_H

#include <linux/types.h>

static inline void kcsan_check_access(const volatile void *ptr, size_t size, int type) { }
static inline void kcsan_check_read(const volatile void *ptr, size_t size) { }
static inline void kcsan_check_write(const volatile void *ptr, size_t size) { }
static inline void kcsan_check_read_write(const volatile void *ptr, size_t size) { }
static inline void kcsan_check_atomic_read(const volatile void *ptr, size_t size) { }
static inline void kcsan_check_atomic_write(const volatile void *ptr, size_t size) { }
static inline void kcsan_check_atomic_read_write(const volatile void *ptr, size_t size) { }

/* Marks the beginning/end of a KCSAN-disabled region. */
static inline void kcsan_disable_current(void) { }
static inline void kcsan_enable_current(void) { }
static inline void kcsan_begin_atomic(void) { }
static inline void kcsan_end_atomic(void) { }

/*
 * The ASSERT_EXCLUSIVE_*() annotations only do something with KCSAN; without
 * it they expand to nothing.
 */
#define ASSERT_EXCLUSIVE_WRITER(ptr)		do { } while (0)
#define ASSERT_EXCLUSIVE_ACCESS(ptr)		do { } while (0)
#define ASSERT_EXCLUSIVE_ACCESS_BOOL(ptr, b)	do { } while (0)

#endif /* _LINUX_KCSAN_CHECKS_H */

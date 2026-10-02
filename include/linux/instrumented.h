/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This header provides generic wrappers for memory access instrumentation that
 * the compiler cannot emit for: KASAN, KCSAN.
 *
 * Adapted for the 4.19 tree: KCSAN is absent, so only the KASAN checks are
 * kept.
 */
#ifndef _LINUX_INSTRUMENTED_H
#define _LINUX_INSTRUMENTED_H

#include <linux/compiler.h>
#include <linux/kasan-checks.h>
#include <linux/kcsan-checks.h>
#include <linux/types.h>

/**
 * instrument_read - instrument regular read access
 * @v: address of access
 * @size: size of access
 */
static __always_inline void instrument_read(const volatile void *v, size_t size)
{
	kasan_check_read(v, size);
	kcsan_check_read(v, size);
}

/**
 * instrument_write - instrument regular write access
 * @v: address of access
 * @size: size of access
 */
static __always_inline void instrument_write(const volatile void *v, size_t size)
{
	kasan_check_write(v, size);
	kcsan_check_write(v, size);
}

/**
 * instrument_read_write - instrument regular read-write access
 * @v: address of access
 * @size: size of access
 */
static __always_inline void instrument_read_write(const volatile void *v, size_t size)
{
	kasan_check_write(v, size);
	kcsan_check_read_write(v, size);
}

/**
 * instrument_atomic_read - instrument atomic read access
 * @v: address of access
 * @size: size of access
 */
static __always_inline void instrument_atomic_read(const volatile void *v, size_t size)
{
	kasan_check_read(v, size);
	kcsan_check_atomic_read(v, size);
}

/**
 * instrument_atomic_write - instrument atomic write access
 * @v: address of access
 * @size: size of access
 */
static __always_inline void instrument_atomic_write(const volatile void *v, size_t size)
{
	kasan_check_write(v, size);
	kcsan_check_atomic_write(v, size);
}

/**
 * instrument_atomic_read_write - instrument atomic read-write access
 * @v: address of access
 * @size: size of access
 */
static __always_inline void instrument_atomic_read_write(const volatile void *v, size_t size)
{
	kasan_check_write(v, size);
	kcsan_check_atomic_read_write(v, size);
}

/**
 * instrument_copy_to_user - instrument reads of copy_to_user
 * @to: destination address
 * @from: source address
 * @n: number of bytes to copy
 */
static __always_inline void
instrument_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	kasan_check_read(from, n);
	kcsan_check_read(from, n);
}

/**
 * instrument_copy_from_user - instrument writes of copy_from_user
 * @to: destination address
 * @from: source address
 * @n: number of bytes to copy
 */
static __always_inline void
instrument_copy_from_user(const void *to, const void __user *from, unsigned long n)
{
	kasan_check_write(to, n);
	kcsan_check_write(to, n);
}

#endif /* _LINUX_INSTRUMENTED_H */

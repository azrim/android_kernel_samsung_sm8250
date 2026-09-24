/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_LSE_H
#define __ASM_LSE_H

#include <asm/atomic_ll_sc.h>

#ifdef CONFIG_ARM64_LSE_ATOMICS

#define __LSE_PREAMBLE	".arch_extension lse\n"

#include <linux/compiler_types.h>
#include <linux/export.h>
#include <linux/jump_label.h>
#include <linux/stringify.h>
#include <asm/alternative.h>
#include <asm/atomic_lse.h>
#include <asm/cpucaps.h>

extern struct static_key_false cpu_hwcap_keys[ARM64_NCAPS];
extern struct static_key_false arm64_const_caps_ready;

/*
 * This kernel only runs on SM8250 (Kryo 585), which implements the ARMv8.2
 * LSE atomics. Selecting them at compile time lets the compiler drop the
 * runtime capability check and emit a single LSE instruction for every
 * atomic, instead of a checked branch around an LL/SC retry loop.
 */
#define system_uses_lse_atomics()	true

#define __lse_ll_sc_body(op, ...)					\
({									\
	system_uses_lse_atomics() ?					\
		__lse_##op(__VA_ARGS__) :				\
		__ll_sc_##op(__VA_ARGS__);				\
})

/*
 * The LSE variant is chosen at compile time, so there is nothing to patch in
 * at runtime. The LL/SC string is still accepted (and discarded here) so that
 * the call sites remain valid if CONFIG_ARM64_LSE_ATOMICS is ever turned off.
 */
#define ARM64_LSE_ATOMIC_INSN(llsc, lse)	__LSE_PREAMBLE lse

#else	/* CONFIG_ARM64_LSE_ATOMICS */

static inline bool system_uses_lse_atomics(void) { return false; }

#define __lse_ll_sc_body(op, ...)		__ll_sc_##op(__VA_ARGS__)

#define ARM64_LSE_ATOMIC_INSN(llsc, lse)	llsc

#endif	/* CONFIG_ARM64_LSE_ATOMICS */
#endif	/* __ASM_LSE_H */

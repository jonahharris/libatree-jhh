/*
 * Compiler and platform shims. This is the only file in src/ allowed to
 * contain compiler-specific or platform-specific conditionals.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_COMPILER_H
#define ATREE_COMPILER_H

#include <stddef.h>

/* Compile-time assertion usable at file scope and in blocks. C11 has
 * _Static_assert; for C99 fall back to a negative-array-size typedef. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define ATREE_STATIC_ASSERT(cond, name) _Static_assert((cond), #name)
#else
#define ATREE_STATIC_ASSERT(cond, name) typedef char atree__static_assert_##name[(cond) ? 1 : -1]
#endif

/* Marks a deliberately unused parameter or variable. */
#define ATREE_UNUSED(x) ((void)(x))

/* For generated static inline functions that a translation unit may not use. */
#if defined(__GNUC__) || defined(__clang__)
#define ATREE_MAYBE_UNUSED __attribute__((unused))
#else
#define ATREE_MAYBE_UNUSED
#endif

/* Hidden visibility is set by -fvisibility=hidden on the command line; the
 * public symbols are marked ATREE_API in include/atree.h. Nothing here. */

/* Hint for the optimizer; never affects semantics. */
#if defined(__GNUC__) || defined(__clang__)
#define ATREE_LIKELY(x) __builtin_expect(!!(x), 1)
#define ATREE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define ATREE_LIKELY(x) (x)
#define ATREE_UNLIKELY(x) (x)
#endif

#endif /* ATREE_COMPILER_H */

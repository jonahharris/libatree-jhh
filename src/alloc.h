/*
 * Allocator wrappers. Every allocation in the library goes through these,
 * which forward to the user's atree_allocator_t and keep live-byte accounting.
 *
 * Rules enforced here (see CLAUDE.md):
 *   - size 0 is never passed to the user allocator;
 *   - free/realloc receive the exact size previously requested;
 *   - array sizes are overflow-checked before multiplication.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_ALLOC_H
#define ATREE_ALLOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atree.h"

struct atree__mem {
    atree_allocator_t a;
    size_t live;  /* bytes currently allocated through this context */
    size_t peak;  /* high-water mark of live                        */
    size_t calls; /* alloc + realloc + free calls, for allocation-free tests */
};

/* Initializes with the given allocator, or the default when a is NULL. */
void atree__mem_init(struct atree__mem *m, const atree_allocator_t *a);

/* size > 0. Returns NULL on failure. */
void *atree__alloc(struct atree__mem *m, size_t size);
/* Like atree__alloc but zero-filled. */
void *atree__zalloc(struct atree__mem *m, size_t size);
/* new_size > 0. ptr may be NULL only if old_size == 0 (then behaves as
 * alloc). On failure returns NULL and leaves ptr valid. */
void *atree__realloc(struct atree__mem *m, void *ptr, size_t old_size, size_t new_size);
/* ptr may be NULL (no-op). size is the size originally requested. */
void atree__free(struct atree__mem *m, void *ptr, size_t size);

/* Overflow-checked array helpers. n > 0 and elem > 0. */
void *atree__alloc_array(struct atree__mem *m, size_t n, size_t elem);
void *atree__zalloc_array(struct atree__mem *m, size_t n, size_t elem);
void *atree__realloc_array(struct atree__mem *m, void *ptr, size_t old_n, size_t new_n,
                           size_t elem);
void atree__free_array(struct atree__mem *m, void *ptr, size_t n, size_t elem);

/* Copies len bytes and appends a NUL. Returns NULL on failure. */
char *atree__strndup(struct atree__mem *m, const char *s, size_t len);
void atree__strfree(struct atree__mem *m, char *s, size_t len);

/* *out = a * b (or a + b); returns true if the result does not fit size_t. */
static inline bool atree__mul_overflows(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return true;
    }
    *out = a * b;
    return false;
}

static inline bool atree__add_overflows(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) {
        return true;
    }
    *out = a + b;
    return false;
}

#endif /* ATREE_ALLOC_H */

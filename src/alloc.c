/*
 * Allocator wrappers and the default (C library) allocator.
 *
 * SPDX-License-Identifier: MIT
 */
#include "alloc.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"

/* ---- default allocator -------------------------------------------------- */

static void *default_alloc(void *ctx, size_t size)
{
    ATREE_UNUSED(ctx);
    return malloc(size);
}

static void *default_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    ATREE_UNUSED(ctx);
    ATREE_UNUSED(old_size);
    return realloc(ptr, new_size);
}

static void default_free(void *ctx, void *ptr, size_t size)
{
    ATREE_UNUSED(ctx);
    ATREE_UNUSED(size);
    free(ptr);
}

static const atree_allocator_t atree__default_allocator = {
    default_alloc,
    default_realloc,
    default_free,
    NULL,
};

const atree_allocator_t *atree_default_allocator(void)
{
    return &atree__default_allocator;
}

/* ---- accounting wrappers ----------------------------------------------- */

void atree__mem_init(struct atree__mem *m, const atree_allocator_t *a)
{
    m->a = a ? *a : atree__default_allocator;
    m->live = 0;
    m->peak = 0;
    m->calls = 0;
}

static void account_add(struct atree__mem *m, size_t size)
{
    m->live += size;
    if (m->live > m->peak) {
        m->peak = m->live;
    }
}

void *atree__alloc(struct atree__mem *m, size_t size)
{
    void *p;
    assert(size > 0);
    m->calls++;
    p = m->a.alloc(m->a.ctx, size);
    if (p != NULL) {
        account_add(m, size);
    }
    return p;
}

void *atree__zalloc(struct atree__mem *m, size_t size)
{
    void *p = atree__alloc(m, size);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

void *atree__realloc(struct atree__mem *m, void *ptr, size_t old_size, size_t new_size)
{
    void *p;
    assert(new_size > 0);
    if (ptr == NULL) {
        assert(old_size == 0);
        return atree__alloc(m, new_size);
    }
    m->calls++;
    p = m->a.realloc(m->a.ctx, ptr, old_size, new_size);
    if (p != NULL) {
        assert(m->live >= old_size);
        m->live -= old_size;
        account_add(m, new_size);
    }
    return p;
}

void atree__free(struct atree__mem *m, void *ptr, size_t size)
{
    if (ptr == NULL) {
        return;
    }
    assert(m->live >= size);
    m->calls++;
    m->live -= size;
    m->a.free(m->a.ctx, ptr, size);
}

/* ---- arrays ------------------------------------------------------------- */

void *atree__alloc_array(struct atree__mem *m, size_t n, size_t elem)
{
    size_t bytes;
    assert(n > 0 && elem > 0);
    if (atree__mul_overflows(n, elem, &bytes)) {
        return NULL;
    }
    return atree__alloc(m, bytes);
}

void *atree__zalloc_array(struct atree__mem *m, size_t n, size_t elem)
{
    size_t bytes;
    assert(n > 0 && elem > 0);
    if (atree__mul_overflows(n, elem, &bytes)) {
        return NULL;
    }
    return atree__zalloc(m, bytes);
}

void *atree__realloc_array(struct atree__mem *m, void *ptr, size_t old_n, size_t new_n, size_t elem)
{
    size_t old_bytes;
    size_t new_bytes;
    assert(new_n > 0 && elem > 0);
    if (atree__mul_overflows(new_n, elem, &new_bytes)) {
        return NULL;
    }
    old_bytes = old_n * elem; /* old_n * elem was already allocated, so it fits */
    return atree__realloc(m, ptr, old_bytes, new_bytes);
}

void atree__free_array(struct atree__mem *m, void *ptr, size_t n, size_t elem)
{
    if (ptr == NULL) {
        return;
    }
    atree__free(m, ptr, n * elem);
}

/* ---- strings ------------------------------------------------------------ */

char *atree__strndup(struct atree__mem *m, const char *s, size_t len)
{
    size_t bytes;
    char *copy;
    if (atree__add_overflows(len, 1, &bytes)) {
        return NULL;
    }
    copy = atree__alloc(m, bytes);
    if (copy == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(copy, s, len);
    }
    copy[len] = '\0';
    return copy;
}

void atree__strfree(struct atree__mem *m, char *s, size_t len)
{
    if (s != NULL) {
        atree__free(m, s, len + 1);
    }
}

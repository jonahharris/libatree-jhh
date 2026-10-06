/*
 * Test allocator: counts calls, tracks live bytes, verifies that the library
 * passes the exact size it requested to free/realloc, and can fail the Nth
 * allocation on demand. Header-only; include from one translation unit.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_TEST_ALLOC_H
#define ATREE_TEST_ALLOC_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atree.h"

/* 16-byte header keeps the payload 16-byte aligned when malloc is. */
struct test_alloc_hdr {
    size_t size;
    size_t magic;
};

#define TEST_ALLOC_MAGIC ((size_t)0x5a5aa5a5u)

struct test_alloc {
    atree_allocator_t a; /* pass &ta.a to the library */
    size_t live;         /* bytes currently allocated          */
    size_t peak;
    size_t n_alloc;
    size_t n_realloc;
    size_t n_free;
    size_t fail_at;         /* 0 = never; else fail the Nth alloc/realloc (1-based) */
    size_t attempts;        /* alloc + realloc attempts so far                      */
    size_t size_mismatches; /* free/realloc called with a wrong old size         */
    size_t zero_size_calls; /* contract violations: size 0 requested             */
};

static void *test_alloc_alloc(void *ctx, size_t size)
{
    struct test_alloc *t = (struct test_alloc *)ctx;
    struct test_alloc_hdr *h;
    if (size == 0) {
        t->zero_size_calls++;
    }
    t->attempts++;
    if (t->fail_at != 0 && t->attempts == t->fail_at) {
        return NULL;
    }
    h = (struct test_alloc_hdr *)malloc(sizeof *h + size);
    if (h == NULL) {
        return NULL;
    }
    h->size = size;
    h->magic = TEST_ALLOC_MAGIC;
    t->n_alloc++;
    t->live += size;
    if (t->live > t->peak) {
        t->peak = t->live;
    }
    return (void *)(h + 1);
}

static void *test_alloc_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    struct test_alloc *t = (struct test_alloc *)ctx;
    struct test_alloc_hdr *h = (struct test_alloc_hdr *)ptr - 1;
    struct test_alloc_hdr *nh;
    if (new_size == 0) {
        t->zero_size_calls++;
    }
    if (h->magic != TEST_ALLOC_MAGIC || h->size != old_size) {
        t->size_mismatches++;
        fprintf(stderr, "test_alloc: realloc old_size %lu but block is %lu\n",
                (unsigned long)old_size, (unsigned long)h->size);
    }
    t->attempts++;
    if (t->fail_at != 0 && t->attempts == t->fail_at) {
        return NULL;
    }
    nh = (struct test_alloc_hdr *)realloc(h, sizeof *nh + new_size);
    if (nh == NULL) {
        return NULL;
    }
    t->n_realloc++;
    t->live -= nh->size;
    t->live += new_size;
    if (t->live > t->peak) {
        t->peak = t->live;
    }
    nh->size = new_size;
    return (void *)(nh + 1);
}

static void test_alloc_free(void *ctx, void *ptr, size_t size)
{
    struct test_alloc *t = (struct test_alloc *)ctx;
    struct test_alloc_hdr *h = (struct test_alloc_hdr *)ptr - 1;
    if (h->magic != TEST_ALLOC_MAGIC || h->size != size) {
        t->size_mismatches++;
        fprintf(stderr, "test_alloc: free size %lu but block is %lu\n", (unsigned long)size,
                (unsigned long)h->size);
    }
    t->n_free++;
    t->live -= h->size;
    h->magic = 0;
    free(h);
}

static void test_alloc_init(struct test_alloc *t)
{
    memset(t, 0, sizeof *t);
    t->a.alloc = test_alloc_alloc;
    t->a.realloc = test_alloc_realloc;
    t->a.free = test_alloc_free;
    t->a.ctx = t;
}

/* True when every allocation was returned and no contract violation seen. */
static int test_alloc_clean(const struct test_alloc *t)
{
    return t->live == 0 && t->size_mismatches == 0 && t->zero_size_calls == 0;
}

#endif /* ATREE_TEST_ALLOC_H */

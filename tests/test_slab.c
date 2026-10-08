/*
 * White-box tests for src/slab.h: segmented slabs grow without moving full
 * segments, carry at most one segment of slack, and fail cleanly.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/slab.h"

#include "test.h"
#include "test_alloc.h"

struct big {
    uint64_t a[8]; /* 64 bytes, like a node */
};
ATREE_SLAB_DEFINE(bigslab, struct big);

TEST(push_keeps_values_and_bounds_slack)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__u32slab v;
    const uint32_t n = 300000; /* 18.3 segments */
    uint32_t i;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__u32slab_init(&v);
    for (i = 0; i < n; i++) {
        ASSERT_OK(atree__u32slab_push(&m, &v, i * 7));
    }
    ASSERT_EQ_U64(v.len, n);
    ASSERT_TRUE(v.cap >= v.len);
    ASSERT_TRUE(v.cap - v.len < ATREE_SLAB_SEG); /* at most one segment of slack */
    ASSERT_EQ_U64(v.nsegs, (n + ATREE_SLAB_SEG - 1) / ATREE_SLAB_SEG);
    ASSERT_TRUE(m.live <= (size_t)v.cap * sizeof(uint32_t) + (size_t)v.segcap * sizeof(void *));
    for (i = 0; i < n; i++) {
        ASSERT_EQ_U64(*atree__u32slab_at(&v, i), i * 7);
    }
    atree__u32slab_free(&m, &v);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_NULL(v.segs);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(first_segment_doubles_then_segments_never_move)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct bigslab v;
    const struct big *first;
    size_t allocs;
    uint32_t i;
    struct big val;
    memset(&val, 0, sizeof val);
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    bigslab_init(&v);
    for (i = 0; i < ATREE_SLAB_SEG; i++) {
        val.a[0] = i;
        ASSERT_OK(bigslab_push(&m, &v, val));
    }
    /* Geometric growth of the first segment: a handful of reallocations. */
    ASSERT_TRUE(ta.n_alloc + ta.n_realloc <= 16);
    ASSERT_EQ_U64(v.cap, ATREE_SLAB_SEG);
    ASSERT_EQ_U64(v.nsegs, 1);
    first = bigslab_at(&v, 0);
    allocs = ta.n_alloc;
    /* The next element opens a second segment and copies nothing. */
    val.a[0] = ATREE_SLAB_SEG;
    ASSERT_OK(bigslab_push(&m, &v, val));
    ASSERT_EQ_U64(ta.n_alloc, allocs + 1);
    ASSERT_EQ_U64(v.nsegs, 2);
    ASSERT_EQ_U64(v.cap, 2 * ATREE_SLAB_SEG);
    ASSERT_EQ_PTR(bigslab_at(&v, 0), first);
    for (i = 0; i < 5 * ATREE_SLAB_SEG; i++) {
        val.a[0] = (uint64_t)i + ATREE_SLAB_SEG + 1;
        ASSERT_OK(bigslab_push(&m, &v, val));
    }
    ASSERT_EQ_PTR(bigslab_at(&v, 0), first);
    ASSERT_EQ_U64(bigslab_at(&v, ATREE_SLAB_SEG)->a[0], ATREE_SLAB_SEG);
    ASSERT_EQ_U64(bigslab_at(&v, v.len - 1)->a[0], v.len - 1);
    bigslab_free(&m, &v);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(reserve_then_push_allocates_nothing)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__u64slab v;
    size_t calls;
    uint32_t i;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__u64slab_init(&v);
    ASSERT_OK(atree__u64slab_reserve(&m, &v, 100000));
    ASSERT_TRUE(v.cap >= 100000);
    ASSERT_TRUE(v.cap - 100000 < ATREE_SLAB_SEG);
    calls = m.calls;
    for (i = 0; i < 100000; i++) {
        ASSERT_OK(atree__u64slab_push(&m, &v, i));
    }
    ASSERT_EQ_U64(m.calls, calls);
    ASSERT_OK(atree__u64slab_reserve(&m, &v, 10)); /* already covered: a no-op */
    ASSERT_EQ_U64(m.calls, calls);
    atree__u64slab_free(&m, &v);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(failed_growth_leaves_the_slab_unchanged)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__u64slab v;
    size_t k;
    uint32_t i;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__u64slab_init(&v);
    for (i = 0; i < 5; i++) {
        ASSERT_OK(atree__u64slab_push(&m, &v, i + 100));
    }
    /* Fail each allocation the growth to three segments makes, in turn. */
    for (k = 1;; k++) {
        uint32_t cap = v.cap;
        uint32_t nsegs = v.nsegs;
        size_t live = m.live;
        atree_status_t st;
        ta.fail_at = ta.attempts + k;
        st = atree__u64slab_reserve(&m, &v, 2 * ATREE_SLAB_SEG + 1);
        ta.fail_at = 0;
        if (st == ATREE_OK) {
            break;
        }
        ASSERT_STATUS(st, ATREE_ERR_NOMEM);
        ASSERT_EQ_U64(v.len, 5);
        /* Partial progress is kept (each step is complete in itself), never
         * lost, and the accounting stays exact. */
        ASSERT_TRUE(v.cap >= cap);
        ASSERT_TRUE(v.nsegs >= nsegs);
        ASSERT_TRUE(m.live >= live);
        for (i = 0; i < 5; i++) {
            ASSERT_EQ_U64(*atree__u64slab_at(&v, i), i + 100);
        }
        ASSERT_TRUE(k < 64);
    }
    ASSERT_TRUE(v.cap >= 2 * ATREE_SLAB_SEG + 1);
    ASSERT_EQ_U64(v.nsegs, 3);
    for (i = 0; i < 5; i++) {
        ASSERT_EQ_U64(*atree__u64slab_at(&v, i), i + 100);
    }
    atree__u64slab_free(&m, &v);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(push_keeps_values_and_bounds_slack);
RUN_TEST(first_segment_doubles_then_segments_never_move);
RUN_TEST(reserve_then_push_allocates_nothing);
RUN_TEST(failed_growth_leaves_the_slab_unchanged);
TEST_MAIN_END()

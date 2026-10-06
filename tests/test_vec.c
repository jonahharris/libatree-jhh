/*
 * White-box tests for src/vec.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/vec.h"

#include "test.h"
#include "test_alloc.h"

struct point {
    int x;
    int y;
};
ATREE_VEC_DEFINE(pointvec, struct point);

TEST(push_grows_geometrically_and_keeps_order)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__u32vec v;
    uint32_t i;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__u32vec_init(&v);
    ASSERT_EQ_U64(v.len, 0);
    for (i = 0; i < 100000; i++) {
        ASSERT_OK(atree__u32vec_push(&m, &v, i * 3));
    }
    ASSERT_EQ_U64(v.len, 100000);
    ASSERT_TRUE(v.cap >= v.len);
    for (i = 0; i < 100000; i++) {
        ASSERT_EQ_U64(v.data[i], i * 3);
    }
    /* Geometric growth: far fewer reallocations than pushes. */
    ASSERT_TRUE(ta.n_alloc + ta.n_realloc < 40);
    atree__u32vec_free(&m, &v);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_TRUE(test_alloc_clean(&ta));
    ASSERT_NULL(v.data);
    return 0;
}

TEST(reserve_then_push_does_not_reallocate)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__u64vec v;
    uint32_t i;
    size_t calls;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__u64vec_init(&v);
    ASSERT_OK(atree__u64vec_reserve(&m, &v, 1000));
    ASSERT_TRUE(v.cap >= 1000);
    calls = ta.attempts;
    for (i = 0; i < 1000; i++) {
        ASSERT_OK(atree__u64vec_push(&m, &v, (uint64_t)i << 40));
    }
    ASSERT_EQ_U64(ta.attempts, calls);
    ASSERT_EQ_U64(v.data[999], (uint64_t)999 << 40);
    atree__u64vec_free(&m, &v);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(insert_remove_swap_remove)
{
    struct atree__mem m;
    struct atree__u32vec v;
    atree__mem_init(&m, NULL);
    atree__u32vec_init(&v);
    ASSERT_OK(atree__u32vec_insert_at(&m, &v, 0, 10)); /* [10] */
    ASSERT_OK(atree__u32vec_insert_at(&m, &v, 0, 5));  /* [5 10] */
    ASSERT_OK(atree__u32vec_insert_at(&m, &v, 2, 20)); /* [5 10 20] */
    ASSERT_OK(atree__u32vec_insert_at(&m, &v, 1, 7));  /* [5 7 10 20] */
    ASSERT_EQ_U64(v.len, 4);
    ASSERT_EQ_U64(v.data[0], 5);
    ASSERT_EQ_U64(v.data[1], 7);
    ASSERT_EQ_U64(v.data[2], 10);
    ASSERT_EQ_U64(v.data[3], 20);
    atree__u32vec_remove_at(&v, 1); /* [5 10 20] */
    ASSERT_EQ_U64(v.len, 3);
    ASSERT_EQ_U64(v.data[1], 10);
    atree__u32vec_remove_at(&v, 2); /* [5 10] (last) */
    ASSERT_EQ_U64(v.len, 2);
    ASSERT_OK(atree__u32vec_push(&m, &v, 30)); /* [5 10 30] */
    atree__u32vec_swap_remove(&v, 0);          /* [30 10] */
    ASSERT_EQ_U64(v.len, 2);
    ASSERT_EQ_U64(v.data[0], 30);
    ASSERT_EQ_U64(v.data[1], 10);
    atree__u32vec_swap_remove(&v, 1); /* [30] (last) */
    ASSERT_EQ_U64(v.len, 1);
    ASSERT_EQ_U64(atree__u32vec_find(&v, 30), 0);
    ASSERT_EQ_U64(atree__u32vec_find(&v, 31), UINT32_MAX);
    atree__u32vec_clear(&v);
    ASSERT_EQ_U64(v.len, 0);
    ASSERT_TRUE(v.cap > 0);
    atree__u32vec_free(&m, &v);
    ASSERT_EQ_U64(m.live, 0);
    return 0;
}

TEST(struct_elements)
{
    struct atree__mem m;
    struct pointvec v;
    struct point p;
    atree__mem_init(&m, NULL);
    pointvec_init(&v);
    p.x = 1;
    p.y = 2;
    ASSERT_OK(pointvec_push(&m, &v, p));
    p.x = 3;
    ASSERT_OK(pointvec_push(&m, &v, p));
    ASSERT_EQ_I64(v.data[0].x, 1);
    ASSERT_EQ_I64(v.data[1].x, 3);
    ASSERT_EQ_I64(v.data[1].y, 2);
    pointvec_free(&m, &v);
    ASSERT_EQ_U64(m.live, 0);
    return 0;
}

TEST(push_failure_leaves_vector_intact)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__u32vec v;
    uint32_t i;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__u32vec_init(&v);
    for (i = 0; i < ATREE_VEC_MIN_CAP; i++) {
        ASSERT_OK(atree__u32vec_push(&m, &v, i));
    }
    ASSERT_EQ_U64(v.cap, ATREE_VEC_MIN_CAP);
    ta.fail_at = ta.attempts + 1; /* next growth fails */
    ASSERT_STATUS(atree__u32vec_push(&m, &v, 99), ATREE_ERR_NOMEM);
    ASSERT_EQ_U64(v.len, ATREE_VEC_MIN_CAP);
    ASSERT_EQ_U64(v.cap, ATREE_VEC_MIN_CAP);
    for (i = 0; i < ATREE_VEC_MIN_CAP; i++) {
        ASSERT_EQ_U64(v.data[i], i);
    }
    ta.fail_at = 0;
    ASSERT_OK(atree__u32vec_push(&m, &v, 99));
    ASSERT_EQ_U64(v.data[ATREE_VEC_MIN_CAP], 99);
    atree__u32vec_free(&m, &v);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(grow_rejects_byte_overflow)
{
    struct atree__mem m;
    atree_status_t st = ATREE_OK;
    uint32_t cap = 0;
    void *p;
    atree__mem_init(&m, NULL);
    /* 2^31 elements of 2^33 bytes each overflows size_t on 64-bit; on 32-bit
     * the element size alone overflows. Either way: ATREE_ERR_LIMIT. */
    p = atree__vec_grow(&m, NULL, (size_t)1 << (sizeof(size_t) * 8 - 2), 0, UINT32_C(1) << 31, &cap,
                        &st);
    ASSERT_NULL(p);
    ASSERT_STATUS(st, ATREE_ERR_LIMIT);
    ASSERT_EQ_U64(cap, 0);
    ASSERT_EQ_U64(m.calls, 0);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(push_grows_geometrically_and_keeps_order);
RUN_TEST(reserve_then_push_does_not_reallocate);
RUN_TEST(insert_remove_swap_remove);
RUN_TEST(struct_elements);
RUN_TEST(push_failure_leaves_vector_intact);
RUN_TEST(grow_rejects_byte_overflow);
TEST_MAIN_END()

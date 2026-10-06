/*
 * White-box tests for src/alloc.c: accounting, overflow checks, default
 * allocator, and the size contract observed through the test allocator.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test_alloc.h"

#include "../src/alloc.h"

#include "test.h"

TEST(default_allocator_round_trip)
{
    struct atree__mem m;
    void *p;
    atree__mem_init(&m, NULL);
    ASSERT_EQ_PTR(m.a.alloc, atree_default_allocator()->alloc);
    p = atree__alloc(&m, 100);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(m.live, 100);
    ASSERT_EQ_U64(m.peak, 100);
    p = atree__realloc(&m, p, 100, 250);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(m.live, 250);
    ASSERT_EQ_U64(m.peak, 250);
    p = atree__realloc(&m, p, 250, 10);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(m.live, 10);
    ASSERT_EQ_U64(m.peak, 250);
    atree__free(&m, p, 10);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_EQ_U64(m.calls, 4);
    return 0;
}

TEST(zalloc_zero_fills)
{
    struct atree__mem m;
    unsigned char *p;
    size_t i;
    atree__mem_init(&m, NULL);
    p = atree__zalloc(&m, 64);
    ASSERT_NOT_NULL(p);
    for (i = 0; i < 64; i++) {
        ASSERT_EQ_U64(p[i], 0);
    }
    atree__free(&m, p, 64);
    return 0;
}

TEST(free_null_is_noop)
{
    struct atree__mem m;
    atree__mem_init(&m, NULL);
    atree__free(&m, NULL, 0);
    atree__free_array(&m, NULL, 10, 8);
    ASSERT_EQ_U64(m.calls, 0);
    return 0;
}

TEST(realloc_null_behaves_as_alloc)
{
    struct atree__mem m;
    void *p;
    atree__mem_init(&m, NULL);
    p = atree__realloc(&m, NULL, 0, 32);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(m.live, 32);
    atree__free(&m, p, 32);
    return 0;
}

TEST(overflow_helpers)
{
    size_t out = 0;
    ASSERT_FALSE(atree__mul_overflows(0, SIZE_MAX, &out));
    ASSERT_EQ_U64(out, 0);
    ASSERT_FALSE(atree__mul_overflows(SIZE_MAX, 1, &out));
    ASSERT_EQ_U64(out, SIZE_MAX);
    ASSERT_TRUE(atree__mul_overflows(SIZE_MAX, 2, &out));
    ASSERT_TRUE(atree__mul_overflows(SIZE_MAX / 2 + 1, 2, &out));
    ASSERT_FALSE(atree__mul_overflows(SIZE_MAX / 2, 2, &out));
    ASSERT_FALSE(atree__add_overflows(SIZE_MAX - 1, 1, &out));
    ASSERT_EQ_U64(out, SIZE_MAX);
    ASSERT_TRUE(atree__add_overflows(SIZE_MAX, 1, &out));
    return 0;
}

TEST(array_helpers_check_overflow)
{
    struct test_alloc ta;
    struct atree__mem m;
    void *p;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);

    p = atree__alloc_array(&m, SIZE_MAX / 4 + 1, 8);
    ASSERT_NULL(p);
    ASSERT_EQ_U64(ta.attempts, 0); /* rejected before reaching the allocator */

    p = atree__zalloc_array(&m, 10, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(m.live, 80);
    p = atree__realloc_array(&m, p, 10, 20, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(m.live, 160);
    ASSERT_NULL(atree__realloc_array(&m, p, 20, SIZE_MAX / 4 + 1, 8));
    ASSERT_EQ_U64(m.live, 160);
    atree__free_array(&m, p, 20, 8);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(custom_allocator_is_used_with_exact_sizes)
{
    struct test_alloc ta;
    struct atree__mem m;
    void *p;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    p = atree__alloc(&m, 17);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(ta.n_alloc, 1);
    ASSERT_EQ_U64(ta.live, 17);
    p = atree__realloc(&m, p, 17, 40);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(ta.n_realloc, 1);
    ASSERT_EQ_U64(ta.live, 40);
    atree__free(&m, p, 40);
    ASSERT_EQ_U64(ta.n_free, 1);
    ASSERT_TRUE(test_alloc_clean(&ta));
    ASSERT_EQ_U64(ta.live, m.live);
    return 0;
}

TEST(failing_allocator_propagates_null)
{
    struct test_alloc ta;
    struct atree__mem m;
    void *p;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    ta.fail_at = 2;
    p = atree__alloc(&m, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_NULL(atree__alloc(&m, 8));
    ASSERT_EQ_U64(m.live, 8); /* failed call did not change accounting */
    ta.fail_at = 3;
    ASSERT_NULL(atree__realloc(&m, p, 8, 16)); /* attempt 3 fails, p still valid */
    ASSERT_EQ_U64(m.live, 8);
    ta.fail_at = 0;
    p = atree__realloc(&m, p, 8, 16);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(m.live, 16);
    atree__free(&m, p, 16);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(strndup_copies_and_terminates)
{
    struct atree__mem m;
    char *s;
    atree__mem_init(&m, NULL);
    s = atree__strndup(&m, "hello world", 5);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_STR(s, "hello");
    ASSERT_EQ_U64(m.live, 6);
    atree__strfree(&m, s, 5);
    ASSERT_EQ_U64(m.live, 0);
    s = atree__strndup(&m, "", 0);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_STR(s, "");
    atree__strfree(&m, s, 0);
    ASSERT_NULL(atree__strndup(&m, "x", SIZE_MAX));
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(default_allocator_round_trip);
RUN_TEST(zalloc_zero_fills);
RUN_TEST(free_null_is_noop);
RUN_TEST(realloc_null_behaves_as_alloc);
RUN_TEST(overflow_helpers);
RUN_TEST(array_helpers_check_overflow);
RUN_TEST(custom_allocator_is_used_with_exact_sizes);
RUN_TEST(failing_allocator_propagates_null);
RUN_TEST(strndup_copies_and_terminates);
TEST_MAIN_END()

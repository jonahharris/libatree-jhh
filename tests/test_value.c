/*
 * White-box tests for src/value.c: doubles, sorted lists, value hash/equality.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>

#include "../src/value.h"

#include "test.h"
#include "test_alloc.h"

TEST(double_equality_and_nan)
{
    double qnan = nan("");
    double inf = HUGE_VAL;
    ASSERT_TRUE(atree__double_eq(1.5, 1.5));
    ASSERT_FALSE(atree__double_eq(1.5, 1.5000001));
    ASSERT_TRUE(atree__double_eq(0.0, -0.0));
    ASSERT_FALSE(atree__double_eq(qnan, qnan));
    ASSERT_FALSE(atree__double_eq(qnan, 0.0));
    ASSERT_TRUE(atree__double_eq(inf, inf));
    ASSERT_TRUE(atree__double_is_nan(qnan));
    ASSERT_FALSE(atree__double_is_nan(inf));
    ASSERT_FALSE(atree__double_is_finite(inf));
    ASSERT_FALSE(atree__double_is_finite(-inf));
    ASSERT_FALSE(atree__double_is_finite(qnan));
    ASSERT_TRUE(atree__double_is_finite(1e308));
    ASSERT_EQ_I64(atree__double_cmp(1.0, 2.0), -1);
    ASSERT_EQ_I64(atree__double_cmp(2.0, 1.0), 1);
    ASSERT_EQ_I64(atree__double_cmp(-0.0, 0.0), 0);
    return 0;
}

TEST(double_bits_canonicalize_negative_zero)
{
    ASSERT_EQ_U64(atree__double_bits(0.0), 0);
    ASSERT_EQ_U64(atree__double_bits(-0.0), 0);
    ASSERT_TRUE(atree__double_bits(1.0) != 0);
    ASSERT_TRUE(atree__double_bits(1.0) != atree__double_bits(-1.0));
    return 0;
}

TEST(int_to_double_exact)
{
    double d = 0;
    ASSERT_TRUE(atree__int_to_double_exact(0, &d));
    ASSERT_TRUE(atree__double_eq(d, 0.0));
    ASSERT_TRUE(atree__int_to_double_exact(-42, &d));
    ASSERT_TRUE(atree__double_eq(d, -42.0));
    ASSERT_TRUE(atree__int_to_double_exact((int64_t)1 << 53, &d));
    ASSERT_FALSE(atree__int_to_double_exact(((int64_t)1 << 53) + 1, &d));
    ASSERT_FALSE(atree__int_to_double_exact(INT64_MAX, &d));
    ASSERT_FALSE(atree__int_to_double_exact(INT64_MIN, &d));
    return 0;
}

TEST(sort_unique)
{
    int64_t a[] = {5, -3, 5, 0, INT64_MAX, INT64_MIN, 0, -3};
    uint32_t sa[] = {3, 1, 3, 2, 1, 0};
    int64_t one[] = {7};
    uint32_t n;
    n = atree__sort_unique_i64(a, 8);
    ASSERT_EQ_U64(n, 5);
    ASSERT_EQ_I64(a[0], INT64_MIN);
    ASSERT_EQ_I64(a[1], -3);
    ASSERT_EQ_I64(a[2], 0);
    ASSERT_EQ_I64(a[3], 5);
    ASSERT_EQ_I64(a[4], INT64_MAX);
    n = atree__sort_unique_u32(sa, 6);
    ASSERT_EQ_U64(n, 4);
    ASSERT_EQ_U64(sa[0], 0);
    ASSERT_EQ_U64(sa[3], 3);
    ASSERT_EQ_U64(atree__sort_unique_i64(one, 1), 1);
    ASSERT_EQ_U64(atree__sort_unique_i64(one, 0), 0);
    return 0;
}

TEST(binary_search)
{
    int64_t v[] = {-10, -1, 0, 7, 100};
    uint32_t s[] = {1, 4, 9};
    ASSERT_TRUE(atree__bsearch_i64(v, 5, -10));
    ASSERT_TRUE(atree__bsearch_i64(v, 5, 100));
    ASSERT_TRUE(atree__bsearch_i64(v, 5, 0));
    ASSERT_FALSE(atree__bsearch_i64(v, 5, 1));
    ASSERT_FALSE(atree__bsearch_i64(v, 5, -11));
    ASSERT_FALSE(atree__bsearch_i64(v, 5, 101));
    ASSERT_FALSE(atree__bsearch_i64(v, 0, 0));
    ASSERT_TRUE(atree__bsearch_u32(s, 3, 4));
    ASSERT_FALSE(atree__bsearch_u32(s, 3, 5));
    return 0;
}

TEST(intersects_and_contains_all)
{
    int64_t a[] = {1, 3, 5, 7};
    int64_t b[] = {2, 4, 6, 7};
    int64_t c[] = {2, 4, 6};
    int64_t d[] = {3, 5};
    int64_t e[] = {3, 5, 9};
    uint32_t sa[] = {1, 2};
    uint32_t sb[] = {2, 3};
    uint32_t sc[] = {3};
    ASSERT_TRUE(atree__intersects_i64(a, 4, b, 4));
    ASSERT_FALSE(atree__intersects_i64(a, 4, c, 3));
    ASSERT_FALSE(atree__intersects_i64(a, 0, b, 4));
    ASSERT_FALSE(atree__intersects_i64(a, 4, b, 0));
    ASSERT_TRUE(atree__intersects_u32(sa, 2, sb, 2));
    ASSERT_FALSE(atree__intersects_u32(sa, 2, sc, 1));

    ASSERT_TRUE(atree__contains_all_i64(a, 4, d, 2));  /* {3,5} subset of a */
    ASSERT_FALSE(atree__contains_all_i64(a, 4, e, 3)); /* 9 missing */
    ASSERT_FALSE(atree__contains_all_i64(d, 2, a, 4)); /* needles longer than hay */
    ASSERT_TRUE(atree__contains_all_i64(a, 4, a, 4));
    ASSERT_TRUE(atree__contains_all_i64(a, 4, d, 0));  /* empty needles */
    ASSERT_FALSE(atree__contains_all_i64(a, 0, d, 2)); /* empty hay */
    ASSERT_TRUE(atree__contains_all_u32(sa, 2, sa, 2));
    ASSERT_FALSE(atree__contains_all_u32(sa, 2, sc, 1));
    return 0;
}

TEST(value_hash_and_equality)
{
    struct atree__value a;
    struct atree__value b;
    int64_t la[] = {1, 2, 3};
    int64_t lb[] = {1, 2, 3};
    int64_t lc[] = {1, 2, 4};

    a.kind = ATREE_V_INT;
    a.u.i = 42;
    b = a;
    ASSERT_TRUE(atree__value_equal(&a, &b));
    ASSERT_EQ_U64(atree__value_hash(&a), atree__value_hash(&b));
    b.u.i = 43;
    ASSERT_FALSE(atree__value_equal(&a, &b));
    ASSERT_TRUE(atree__value_hash(&a) != atree__value_hash(&b));

    a.kind = ATREE_V_FLOAT;
    a.u.f = 0.0;
    b.kind = ATREE_V_FLOAT;
    b.u.f = -0.0;
    ASSERT_TRUE(atree__value_equal(&a, &b));
    ASSERT_EQ_U64(atree__value_hash(&a), atree__value_hash(&b));

    /* Same bits, different kinds: never equal, different hashes. */
    a.kind = ATREE_V_INT;
    a.u.i = 0;
    b.kind = ATREE_V_STRING;
    b.u.s = 0;
    ASSERT_FALSE(atree__value_equal(&a, &b));
    ASSERT_TRUE(atree__value_hash(&a) != atree__value_hash(&b));

    a.kind = ATREE_V_INT_LIST;
    a.u.il.data = la;
    a.u.il.len = 3;
    b.kind = ATREE_V_INT_LIST;
    b.u.il.data = lb;
    b.u.il.len = 3;
    ASSERT_TRUE(atree__value_equal(&a, &b));
    ASSERT_EQ_U64(atree__value_hash(&a), atree__value_hash(&b));
    b.u.il.data = lc;
    ASSERT_FALSE(atree__value_equal(&a, &b));
    ASSERT_TRUE(atree__value_hash(&a) != atree__value_hash(&b));
    b.u.il.data = lb;
    b.u.il.len = 2;
    ASSERT_FALSE(atree__value_equal(&a, &b));
    return 0;
}

TEST(value_copy_and_free)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__value src;
    struct atree__value dst;
    uint32_t ids[] = {4, 8};
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);

    src.kind = ATREE_V_STRING_LIST;
    src.u.sl.data = ids;
    src.u.sl.len = 2;
    ASSERT_OK(atree__value_copy(&m, &dst, &src));
    ASSERT_TRUE(dst.u.sl.data != ids);
    ASSERT_TRUE(atree__value_equal(&src, &dst));
    ASSERT_EQ_U64(m.live, 2 * sizeof(uint32_t));
    atree__value_free(&m, &dst);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_EQ_U64(dst.kind, ATREE_V_UNDEFINED);

    src.kind = ATREE_V_INT;
    src.u.i = 9;
    ASSERT_OK(atree__value_copy(&m, &dst, &src));
    ASSERT_EQ_U64(m.live, 0);
    atree__value_free(&m, &dst);

    ta.fail_at = ta.attempts + 1;
    src.kind = ATREE_V_STRING_LIST;
    src.u.sl.data = ids;
    src.u.sl.len = 2;
    ASSERT_STATUS(atree__value_copy(&m, &dst, &src), ATREE_ERR_NOMEM);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(double_equality_and_nan);
RUN_TEST(double_bits_canonicalize_negative_zero);
RUN_TEST(int_to_double_exact);
RUN_TEST(sort_unique);
RUN_TEST(binary_search);
RUN_TEST(intersects_and_contains_all);
RUN_TEST(value_hash_and_equality);
RUN_TEST(value_copy_and_free);
TEST_MAIN_END()

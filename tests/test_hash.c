/*
 * White-box tests for src/hash.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/hash.h"

#include "test.h"
#include "test_alloc.h"

TEST(hash_functions_are_deterministic_and_spread)
{
    ASSERT_EQ_U64(atree__hash_u64(42), atree__hash_u64(42));
    ASSERT_TRUE(atree__hash_u64(1) != atree__hash_u64(2));
    ASSERT_EQ_U64(atree__hash_bytes("abc", 3), atree__hash_bytes("abc", 3));
    ASSERT_TRUE(atree__hash_bytes("abc", 3) != atree__hash_bytes("abd", 3));
    ASSERT_TRUE(atree__hash_bytes("abc", 3) != atree__hash_bytes("abc", 2));
    ASSERT_TRUE(atree__hash_bytes("", 0) != atree__hash_bytes("a", 1));
    ASSERT_TRUE(atree__hash_combine(1, 2) != atree__hash_combine(2, 1));
    return 0;
}

TEST(u64map_basic)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__u64map map;
    uint32_t v = 0;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__u64map_init(&map);

    ASSERT_FALSE(atree__u64map_get(&map, 1, &v));
    ASSERT_FALSE(atree__u64map_remove(&map, 1));
    ASSERT_OK(atree__u64map_put(&m, &map, 1, 100));
    ASSERT_OK(atree__u64map_put(&m, &map, 2, 200));
    ASSERT_OK(atree__u64map_put(&m, &map, UINT64_MAX, 300));
    ASSERT_OK(atree__u64map_put(&m, &map, 0, 400));
    ASSERT_EQ_U64(map.count, 4);
    ASSERT_TRUE(atree__u64map_get(&map, 1, &v));
    ASSERT_EQ_U64(v, 100);
    ASSERT_TRUE(atree__u64map_get(&map, UINT64_MAX, &v));
    ASSERT_EQ_U64(v, 300);
    ASSERT_TRUE(atree__u64map_get(&map, 0, &v));
    ASSERT_EQ_U64(v, 400);
    ASSERT_TRUE(atree__u64map_get(&map, 2, NULL));
    ASSERT_FALSE(atree__u64map_get(&map, 3, &v));

    ASSERT_OK(atree__u64map_put(&m, &map, 1, 101)); /* overwrite */
    ASSERT_EQ_U64(map.count, 4);
    ASSERT_TRUE(atree__u64map_get(&map, 1, &v));
    ASSERT_EQ_U64(v, 101);

    ASSERT_TRUE(atree__u64map_remove(&map, 1));
    ASSERT_FALSE(atree__u64map_remove(&map, 1));
    ASSERT_FALSE(atree__u64map_get(&map, 1, &v));
    ASSERT_EQ_U64(map.count, 3);

    atree__u64map_clear(&map);
    ASSERT_EQ_U64(map.count, 0);
    ASSERT_FALSE(atree__u64map_get(&map, 2, &v));
    ASSERT_OK(atree__u64map_put(&m, &map, 2, 1));
    ASSERT_TRUE(atree__u64map_get(&map, 2, &v));

    atree__u64map_free(&m, &map);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(u64map_many_with_churn)
{
    struct atree__mem m;
    struct atree__u64map map;
    uint64_t k;
    uint32_t v = 0;
    uint32_t iter;
    uint32_t seen;
    uint32_t cap_after_fill;
    atree__mem_init(&m, NULL);
    atree__u64map_init(&map);

    for (k = 1; k <= 200000; k++) {
        ASSERT_OK(atree__u64map_put(&m, &map, k * 2654435761u, (uint32_t)k));
    }
    ASSERT_EQ_U64(map.count, 200000);
    for (k = 1; k <= 200000; k++) {
        ASSERT_TRUE(atree__u64map_get(&map, k * 2654435761u, &v));
        ASSERT_EQ_U64(v, k);
    }
    /* Remove the even keys, then re-insert them: tombstones must be reused
     * and the table must not grow without bound. */
    cap_after_fill = map.cap;
    for (k = 2; k <= 200000; k += 2) {
        ASSERT_TRUE(atree__u64map_remove(&map, k * 2654435761u));
    }
    ASSERT_EQ_U64(map.count, 100000);
    for (k = 2; k <= 200000; k += 2) {
        ASSERT_FALSE(atree__u64map_get(&map, k * 2654435761u, &v));
        ASSERT_OK(atree__u64map_put(&m, &map, k * 2654435761u, (uint32_t)(k + 1)));
    }
    ASSERT_EQ_U64(map.count, 200000);
    ASSERT_TRUE(map.cap <= cap_after_fill * 2);
    for (k = 1; k <= 200000; k++) {
        ASSERT_TRUE(atree__u64map_get(&map, k * 2654435761u, &v));
        ASSERT_EQ_U64(v, (k % 2 == 0) ? k + 1 : k);
    }
    /* Repeated remove/insert of one key must never grow the table. */
    cap_after_fill = map.cap;
    for (k = 0; k < 1000000; k++) {
        ASSERT_TRUE(atree__u64map_remove(&map, 2654435761u));
        ASSERT_OK(atree__u64map_put(&m, &map, 2654435761u, 1));
    }
    ASSERT_EQ_U64(map.cap, cap_after_fill);

    iter = 0;
    seen = 0;
    while (atree__u64map_next(&map, &iter, &k, &v)) {
        seen++;
    }
    ASSERT_EQ_U64(seen, 200000);

    atree__u64map_free(&m, &map);
    ASSERT_EQ_U64(m.live, 0);
    return 0;
}

TEST(u64map_reserve_avoids_rehash)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__u64map map;
    uint64_t k;
    size_t attempts;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__u64map_init(&map);
    ASSERT_OK(atree__u64map_reserve(&m, &map, 5000));
    attempts = ta.attempts;
    for (k = 0; k < 5000; k++) {
        ASSERT_OK(atree__u64map_put(&m, &map, k, 0));
    }
    ASSERT_EQ_U64(ta.attempts, attempts);
    atree__u64map_free(&m, &map);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(u64map_put_failure_leaves_map_intact)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__u64map map;
    uint32_t v = 0;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__u64map_init(&map);
    ASSERT_OK(atree__u64map_put(&m, &map, 7, 70));
    ta.fail_at = ta.attempts + 1;
    /* Force a rehash by filling past the threshold. */
    {
        uint64_t k;
        atree_status_t st = ATREE_OK;
        for (k = 100; k < 200 && st == ATREE_OK; k++) {
            st = atree__u64map_put(&m, &map, k, 1);
        }
        ASSERT_STATUS(st, ATREE_ERR_NOMEM);
    }
    ASSERT_TRUE(atree__u64map_get(&map, 7, &v));
    ASSERT_EQ_U64(v, 70);
    ta.fail_at = 0;
    atree__u64map_free(&m, &map);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(strmap_basic)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__strmap map;
    uint32_t v = 0;
    const char *k1 = "country";
    const char *k2 = "count";
    const char *k3 = "";
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    atree__strmap_init(&map);

    ASSERT_FALSE(atree__strmap_get(&map, k1, 7, &v));
    ASSERT_OK(atree__strmap_put(&m, &map, k1, 7, 1));
    ASSERT_OK(atree__strmap_put(&m, &map, k2, 5, 2));
    ASSERT_OK(atree__strmap_put(&m, &map, k3, 0, 3));
    ASSERT_EQ_U64(map.count, 3);
    ASSERT_TRUE(atree__strmap_get(&map, "country", 7, &v));
    ASSERT_EQ_U64(v, 1);
    ASSERT_TRUE(atree__strmap_get(&map, "count", 5, &v));
    ASSERT_EQ_U64(v, 2);
    ASSERT_TRUE(atree__strmap_get(&map, "", 0, &v));
    ASSERT_EQ_U64(v, 3);
    ASSERT_TRUE(atree__strmap_get(&map, "countryX", 7, &v)); /* length-delimited */
    ASSERT_FALSE(atree__strmap_get(&map, "countr", 6, &v));
    ASSERT_FALSE(atree__strmap_get(&map, "Country", 7, &v)); /* case-sensitive */

    ASSERT_OK(atree__strmap_put(&m, &map, "country", 7, 11));
    ASSERT_EQ_U64(map.count, 3);
    ASSERT_TRUE(atree__strmap_get(&map, k1, 7, &v));
    ASSERT_EQ_U64(v, 11);

    ASSERT_TRUE(atree__strmap_remove(&map, "count", 5));
    ASSERT_FALSE(atree__strmap_remove(&map, "count", 5));
    ASSERT_FALSE(atree__strmap_get(&map, "count", 5, &v));
    ASSERT_EQ_U64(map.count, 2);

    atree__strmap_free(&m, &map);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(strmap_many)
{
    struct atree__mem m;
    struct atree__strmap map;
    static char keys[20000][12];
    uint32_t i;
    uint32_t v = 0;
    atree__mem_init(&m, NULL);
    atree__strmap_init(&map);
    for (i = 0; i < 20000; i++) {
        int n = snprintf(keys[i], sizeof keys[i], "k%u", (unsigned)i);
        ASSERT_TRUE(n > 0);
        ASSERT_OK(atree__strmap_put(&m, &map, keys[i], (size_t)n, i));
    }
    ASSERT_EQ_U64(map.count, 20000);
    for (i = 0; i < 20000; i++) {
        ASSERT_TRUE(atree__strmap_get(&map, keys[i], strlen(keys[i]), &v));
        ASSERT_EQ_U64(v, i);
    }
    for (i = 0; i < 20000; i += 3) {
        ASSERT_TRUE(atree__strmap_remove(&map, keys[i], strlen(keys[i])));
    }
    for (i = 0; i < 20000; i++) {
        ASSERT_TRUE(atree__strmap_get(&map, keys[i], strlen(keys[i]), &v) == (i % 3 != 0));
    }
    atree__strmap_free(&m, &map);
    ASSERT_EQ_U64(m.live, 0);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(hash_functions_are_deterministic_and_spread);
RUN_TEST(u64map_basic);
RUN_TEST(u64map_many_with_churn);
RUN_TEST(u64map_reserve_avoids_rehash);
RUN_TEST(u64map_put_failure_leaves_map_intact);
RUN_TEST(strmap_basic);
RUN_TEST(strmap_many);
TEST_MAIN_END()

/*
 * White-box tests for src/strtab.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/strtab.h"

#include "test.h"
#include "test_alloc.h"

TEST(intern_and_lookup)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__strtab t;
    uint32_t id1 = 0;
    uint32_t id2 = 0;
    uint32_t id3 = 0;
    uint32_t len = 0;
    const char *s;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    ASSERT_OK(atree__strtab_init(&m, &t));
    ASSERT_EQ_U64(atree__strtab_count(&t), 0);

    ASSERT_EQ_U64(atree__strtab_lookup(&t, "deal-1", 6), ATREE_STR_UNKNOWN);
    ASSERT_OK(atree__strtab_intern(&m, &t, "deal-1", 6, &id1));
    ASSERT_TRUE(id1 != ATREE_STR_UNKNOWN);
    ASSERT_OK(atree__strtab_intern(&m, &t, "deal-2", 6, &id2));
    ASSERT_TRUE(id2 != id1);
    ASSERT_OK(atree__strtab_intern(&m, &t, "deal-1", 6, &id3));
    ASSERT_EQ_U64(id3, id1); /* same string, same id */
    ASSERT_EQ_U64(atree__strtab_count(&t), 2);

    ASSERT_EQ_U64(atree__strtab_lookup(&t, "deal-1", 6), id1);
    ASSERT_EQ_U64(atree__strtab_lookup(&t, "deal-1xyz", 6), id1);
    ASSERT_EQ_U64(atree__strtab_lookup(&t, "deal-1", 5), ATREE_STR_UNKNOWN);
    ASSERT_EQ_U64(atree__strtab_lookup(&t, "DEAL-1", 6), ATREE_STR_UNKNOWN);

    s = atree__strtab_get(&t, id1, &len);
    ASSERT_EQ_STR(s, "deal-1");
    ASSERT_EQ_U64(len, 6);
    ASSERT_NULL(atree__strtab_get(&t, ATREE_STR_UNKNOWN, &len));
    ASSERT_NULL(atree__strtab_get(&t, 999, &len));

    /* Empty string is a valid, distinct string. */
    ASSERT_OK(atree__strtab_intern(&m, &t, "", 0, &id3));
    ASSERT_TRUE(id3 != ATREE_STR_UNKNOWN && id3 != id1 && id3 != id2);
    ASSERT_EQ_STR(atree__strtab_get(&t, id3, &len), "");
    ASSERT_EQ_U64(len, 0);

    /* Embedded NUL bytes are preserved by length. */
    ASSERT_OK(atree__strtab_intern(&m, &t, "a\0b", 3, &id3));
    s = atree__strtab_get(&t, id3, &len);
    ASSERT_EQ_U64(len, 3);
    ASSERT_TRUE(memcmp(s, "a\0b", 3) == 0);
    ASSERT_EQ_U64(atree__strtab_lookup(&t, "a\0b", 3), id3);
    ASSERT_EQ_U64(atree__strtab_lookup(&t, "a\0c", 3), ATREE_STR_UNKNOWN);

    atree__strtab_free(&m, &t);
    ASSERT_EQ_U64(m.live, 0);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(ids_are_dense_and_pointers_stable_across_chunks)
{
    struct atree__mem m;
    struct atree__strtab t;
    static char buf[64];
    const char *first;
    uint32_t id = 0;
    uint32_t i;
    uint32_t len;
    atree__mem_init(&m, NULL);
    ASSERT_OK(atree__strtab_init(&m, &t));
    ASSERT_OK(atree__strtab_intern(&m, &t, "first", 5, &id));
    ASSERT_EQ_U64(id, 1);
    first = atree__strtab_get(&t, 1, NULL);
    /* 50k strings of ~10 bytes cross several 64 KiB chunks. */
    for (i = 0; i < 50000; i++) {
        int n = snprintf(buf, sizeof buf, "string-%u", (unsigned)i);
        ASSERT_OK(atree__strtab_intern(&m, &t, buf, (size_t)n, &id));
        ASSERT_EQ_U64(id, i + 2);
    }
    ASSERT_EQ_U64(atree__strtab_count(&t), 50001);
    ASSERT_TRUE(t.chunks.len > 1);
    ASSERT_EQ_PTR(atree__strtab_get(&t, 1, NULL), first); /* no relocation */
    ASSERT_EQ_STR(first, "first");
    for (i = 0; i < 50000; i += 997) {
        int n = snprintf(buf, sizeof buf, "string-%u", (unsigned)i);
        ASSERT_EQ_U64(atree__strtab_lookup(&t, buf, (size_t)n), i + 2);
        ASSERT_EQ_STR(atree__strtab_get(&t, i + 2, &len), buf);
        ASSERT_EQ_U64(len, (uint64_t)n);
    }
    atree__strtab_free(&m, &t);
    ASSERT_EQ_U64(m.live, 0);
    return 0;
}

TEST(string_larger_than_chunk_gets_own_chunk)
{
    struct atree__mem m;
    struct atree__strtab t;
    size_t big_len = ATREE_STRTAB_CHUNK_SIZE * 3 + 17;
    char *big = (char *)malloc(big_len);
    uint32_t id = 0;
    uint32_t small = 0;
    uint32_t len = 0;
    ASSERT_NOT_NULL(big);
    memset(big, 'x', big_len);
    atree__mem_init(&m, NULL);
    ASSERT_OK(atree__strtab_init(&m, &t));
    ASSERT_OK(atree__strtab_intern(&m, &t, "small", 5, &small));
    ASSERT_OK(atree__strtab_intern(&m, &t, big, big_len, &id));
    ASSERT_NOT_NULL(atree__strtab_get(&t, id, &len));
    ASSERT_EQ_U64(len, big_len);
    ASSERT_TRUE(memcmp(atree__strtab_get(&t, id, NULL), big, big_len) == 0);
    ASSERT_EQ_U64(atree__strtab_lookup(&t, big, big_len), id);
    ASSERT_EQ_STR(atree__strtab_get(&t, small, NULL), "small");
    ASSERT_TRUE(t.chunks.len >= 2);
    atree__strtab_free(&m, &t);
    ASSERT_EQ_U64(m.live, 0);
    free(big);
    return 0;
}

TEST(intern_failure_leaves_table_consistent)
{
    struct test_alloc ta;
    struct atree__mem m;
    struct atree__strtab t;
    uint32_t id = 0;
    size_t k;
    test_alloc_init(&ta);
    atree__mem_init(&m, &ta.a);
    ASSERT_OK(atree__strtab_init(&m, &t));
    ASSERT_OK(atree__strtab_intern(&m, &t, "keep", 4, &id));
    /* Fail each subsequent allocation point in turn; the table must still
     * answer correctly for "keep" and either fully contain or fully lack the
     * new string. */
    for (k = 1; k <= 6; k++) {
        uint32_t got = 0;
        atree_status_t st;
        ta.fail_at = ta.attempts + k;
        st = atree__strtab_intern(&m, &t, "new-string", 10, &got);
        ta.fail_at = 0;
        ASSERT_EQ_U64(atree__strtab_lookup(&t, "keep", 4), id);
        if (st == ATREE_OK) {
            ASSERT_EQ_U64(atree__strtab_lookup(&t, "new-string", 10), got);
            ASSERT_EQ_STR(atree__strtab_get(&t, got, NULL), "new-string");
        } else {
            ASSERT_STATUS(st, ATREE_ERR_NOMEM);
            ASSERT_EQ_U64(atree__strtab_lookup(&t, "new-string", 10), ATREE_STR_UNKNOWN);
            ASSERT_EQ_U64(atree__strtab_count(&t), 1);
        }
    }
    atree__strtab_free(&m, &t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(intern_and_lookup);
RUN_TEST(ids_are_dense_and_pointers_stable_across_chunks);
RUN_TEST(string_larger_than_chunk_gets_own_chunk);
RUN_TEST(intern_failure_leaves_table_consistent);
TEST_MAIN_END()

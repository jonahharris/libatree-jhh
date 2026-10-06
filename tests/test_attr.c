/*
 * Tests for tree creation and the attribute table through the public API.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "test_alloc.h"

static const atree_attr_def_t DEFS[] = {
    {"exchange_id", ATREE_TYPE_INT},      {"country", ATREE_TYPE_STRING},
    {"deal_ids", ATREE_TYPE_STRING_LIST}, {"private", ATREE_TYPE_BOOL},
    {"bidfloor", ATREE_TYPE_FLOAT},       {"segment_ids", ATREE_TYPE_INT_LIST},
};
#define NDEFS (sizeof DEFS / sizeof DEFS[0])

TEST(create_and_query)
{
    struct test_alloc ta;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_stats_t st;
    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;

    ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_U64(atree_attr_count(t), NDEFS);
    ASSERT_EQ_U64(atree_attr_lookup(t, "exchange_id"), 0);
    ASSERT_EQ_U64(atree_attr_lookup(t, "segment_ids"), 5);
    ASSERT_EQ_U64(atree_attr_lookup(t, "Country"), ATREE_ATTR_INVALID); /* case-sensitive */
    ASSERT_EQ_U64(atree_attr_lookup(t, "nope"), ATREE_ATTR_INVALID);
    ASSERT_EQ_U64(atree_attr_lookup(t, NULL), ATREE_ATTR_INVALID);
    ASSERT_EQ_U64(atree_attr_lookup(NULL, "country"), ATREE_ATTR_INVALID);
    ASSERT_EQ_STR(atree_attr_name(t, 1), "country");
    ASSERT_NULL(atree_attr_name(t, 6));
    ASSERT_EQ_U64(atree_attr_type(t, 4), ATREE_TYPE_FLOAT);
    ASSERT_EQ_U64(atree_attr_type(t, 2), ATREE_TYPE_STRING_LIST);

    atree_stats(t, &st);
    ASSERT_EQ_U64(st.strings, 0);
    ASSERT_EQ_U64(st.bytes_allocated, ta.live);
    ASSERT_TRUE(st.bytes_allocated > 0);

    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    atree_destroy(NULL);
    return 0;
}

TEST(create_with_defaults_and_no_attributes)
{
    atree_t *t = NULL;
    ASSERT_OK(atree_create(NULL, NULL, 0, &t));
    ASSERT_EQ_U64(atree_attr_count(t), 0);
    atree_destroy(t);
    ASSERT_STATUS(atree_create(NULL, DEFS, NDEFS, NULL), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_create(NULL, NULL, 3, &t), ATREE_ERR_INVALID_ARG);
    ASSERT_NULL(t);
    return 0;
}

TEST(rejects_bad_definitions)
{
    atree_t *t = NULL;
    atree_attr_def_t dup[] = {{"a", ATREE_TYPE_INT}, {"b", ATREE_TYPE_INT}, {"a", ATREE_TYPE_BOOL}};
    atree_attr_def_t empty[] = {{"", ATREE_TYPE_INT}};
    atree_attr_def_t nul[] = {{NULL, ATREE_TYPE_INT}};
    atree_attr_def_t kw[] = {{"AND", ATREE_TYPE_INT}};
    atree_attr_def_t kw2[] = {{"one", ATREE_TYPE_INT}};
    atree_attr_def_t bad_ident[] = {{"9lives", ATREE_TYPE_INT}};
    atree_attr_def_t bad_ident2[] = {{"a b", ATREE_TYPE_INT}};
    atree_attr_def_t bad_type[] = {{"a", (atree_type_t)42}};
    atree_attr_def_t ok_dash[] = {{"deal-ids_2", ATREE_TYPE_INT}};

    ASSERT_STATUS(atree_create(NULL, dup, 3, &t), ATREE_ERR_DUPLICATE_ATTR);
    ASSERT_NULL(t);
    ASSERT_STATUS(atree_create(NULL, empty, 1, &t), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_create(NULL, nul, 1, &t), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_create(NULL, kw, 1, &t), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_create(NULL, kw2, 1, &t), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_create(NULL, bad_ident, 1, &t), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_create(NULL, bad_ident2, 1, &t), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_create(NULL, bad_type, 1, &t), ATREE_ERR_INVALID_ARG);
    ASSERT_OK(atree_create(NULL, ok_dash, 1, &t));
    atree_destroy(t);
    return 0;
}

TEST(rejects_bad_config)
{
    atree_t *t = NULL;
    atree_config_t cfg;
    atree_allocator_t half = {NULL, NULL, NULL, NULL};
    atree_lock_t halflock = {NULL, NULL, NULL, NULL, NULL};
    atree_config_init(&cfg);
    cfg.flags = 1u << 10;
    ASSERT_STATUS(atree_create(&cfg, DEFS, NDEFS, &t), ATREE_ERR_INVALID_ARG);
    atree_config_init(&cfg);
    cfg.allocator = &half;
    ASSERT_STATUS(atree_create(&cfg, DEFS, NDEFS, &t), ATREE_ERR_INVALID_ARG);
    atree_config_init(&cfg);
    cfg.lock = &halflock;
    ASSERT_STATUS(atree_create(&cfg, DEFS, NDEFS, &t), ATREE_ERR_INVALID_ARG);
    atree_config_init(&cfg);
    cfg.flags = ATREE_FLAG_NO_REORGANIZE | ATREE_FLAG_NO_PREDICATE_INDEX;
    ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));
    atree_destroy(t);
    return 0;
}

/* Every allocation during create may fail; nothing may leak and the result
 * must be ATREE_ERR_NOMEM with *out == NULL. */
TEST(create_allocation_failures_do_not_leak)
{
    struct test_alloc ta;
    atree_config_t cfg;
    size_t k;
    size_t total;
    atree_t *t = NULL;

    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));
    total = ta.attempts;
    atree_destroy(t);
    ASSERT_TRUE(total > 3);

    for (k = 1; k <= total; k++) {
        test_alloc_init(&ta);
        ta.fail_at = k;
        t = (atree_t *)&k; /* poison */
        ASSERT_STATUS(atree_create(&cfg, DEFS, NDEFS, &t), ATREE_ERR_NOMEM);
        ASSERT_NULL(t);
        ASSERT_TRUE(test_alloc_clean(&ta));
    }
    return 0;
}

/* Lock vtable: every public read call takes and releases the read lock. */
struct lock_counter {
    int rd, rdun, wr, wrun, depth;
};
static void lc_rdlock(void *ctx)
{
    struct lock_counter *c = (struct lock_counter *)ctx;
    c->rd++;
    c->depth++;
}
static void lc_rdunlock(void *ctx)
{
    struct lock_counter *c = (struct lock_counter *)ctx;
    c->rdun++;
    c->depth--;
}
static void lc_wrlock(void *ctx)
{
    struct lock_counter *c = (struct lock_counter *)ctx;
    c->wr++;
    c->depth++;
}
static void lc_wrunlock(void *ctx)
{
    struct lock_counter *c = (struct lock_counter *)ctx;
    c->wrun++;
    c->depth--;
}

TEST(lock_is_taken_and_released)
{
    struct lock_counter c = {0, 0, 0, 0, 0};
    atree_lock_t lock;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_stats_t st;
    lock.rdlock = lc_rdlock;
    lock.rdunlock = lc_rdunlock;
    lock.wrlock = lc_wrlock;
    lock.wrunlock = lc_wrunlock;
    lock.ctx = &c;
    atree_config_init(&cfg);
    cfg.lock = &lock;
    ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));
    ASSERT_EQ_U64(atree_attr_lookup(t, "country"), 1);
    atree_stats(t, &st);
    ASSERT_EQ_I64(c.rd, 2);
    ASSERT_EQ_I64(c.rdun, 2);
    ASSERT_EQ_I64(c.depth, 0);
    atree_destroy(t);
    ASSERT_EQ_I64(c.depth, 0);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(create_and_query);
RUN_TEST(create_with_defaults_and_no_attributes);
RUN_TEST(rejects_bad_definitions);
RUN_TEST(rejects_bad_config);
RUN_TEST(create_allocation_failures_do_not_leak);
RUN_TEST(lock_is_taken_and_released);
TEST_MAIN_END()

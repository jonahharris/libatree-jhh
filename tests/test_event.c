/*
 * Tests for events through the public API, with white-box access to the
 * stored values (../src/event.h) and the tree's string table.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>

#include "../src/atree_internal.h"
#include "../src/event.h"

#include "test.h"
#include "test_alloc.h"

static const atree_attr_def_t DEFS[] = {
    {"flag", ATREE_TYPE_BOOL},         {"age", ATREE_TYPE_INT},
    {"price", ATREE_TYPE_FLOAT},       {"country", ATREE_TYPE_STRING},
    {"segments", ATREE_TYPE_INT_LIST}, {"deals", ATREE_TYPE_STRING_LIST},
};
enum { A_BOOL = 0, A_INT, A_FLOAT, A_STR, A_ILIST, A_SLIST, A_COUNT };

TEST(set_by_name_and_id)
{
    struct test_alloc ta;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    uint32_t id_ca = 0;
    uint32_t id_us = 0;
    const char *deals[] = {"US", "unknown", "CA", "US"};
    int64_t segs[] = {5, 3, 5, 1};
    const struct atree__value *v;

    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(atree_create(&cfg, DEFS, A_COUNT, &t));
    /* Pre-intern two strings as inserting expressions would (M4). */
    ASSERT_OK(atree__strtab_intern(&t->mem, &t->strings, "CA", 2, &id_ca));
    ASSERT_OK(atree__strtab_intern(&t->mem, &t->strings, "US", 2, &id_us));

    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_NOT_NULL(ev);
    {
        uint32_t i;
        for (i = 0; i < A_COUNT; i++) {
            ASSERT_EQ_U64(atree__event_value(ev, i)->kind, ATREE_V_UNDEFINED);
        }
    }

    ASSERT_OK(atree_event_set_bool(ev, "flag", true));
    v = atree__event_value(ev, A_BOOL);
    ASSERT_EQ_U64(v->kind, ATREE_V_BOOL);
    ASSERT_TRUE(v->u.b);

    ASSERT_OK(atree_event_set_int_id(ev, A_INT, -12));
    v = atree__event_value(ev, A_INT);
    ASSERT_EQ_U64(v->kind, ATREE_V_INT);
    ASSERT_EQ_I64(v->u.i, -12);

    ASSERT_OK(atree_event_set_float(ev, "price", 2.5));
    v = atree__event_value(ev, A_FLOAT);
    ASSERT_EQ_U64(v->kind, ATREE_V_FLOAT);
    ASSERT_TRUE(atree__double_eq(v->u.f, 2.5));
    ASSERT_OK(atree_event_set_float(ev, "price", nan(""))); /* NaN -> undefined */
    ASSERT_EQ_U64(v->kind, ATREE_V_UNDEFINED);

    ASSERT_OK(atree_event_set_string(ev, "country", "CA", 2));
    v = atree__event_value(ev, A_STR);
    ASSERT_EQ_U64(v->kind, ATREE_V_STRING);
    ASSERT_EQ_U64(v->u.s, id_ca);
    ASSERT_OK(atree_event_set_string(ev, "country", "US", SIZE_MAX)); /* strlen */
    ASSERT_EQ_U64(v->u.s, id_us);
    ASSERT_OK(atree_event_set_string(ev, "country", "never-seen", SIZE_MAX));
    ASSERT_EQ_U64(v->u.s, ATREE_STR_UNKNOWN);
    ASSERT_OK(atree_event_set_string(ev, "country", "CAX", 2)); /* length-delimited */
    ASSERT_EQ_U64(v->u.s, id_ca);

    ASSERT_OK(atree_event_set_int_list(ev, "segments", segs, 4));
    v = atree__event_value(ev, A_ILIST);
    ASSERT_EQ_U64(v->kind, ATREE_V_INT_LIST);
    ASSERT_EQ_U64(v->u.il.len, 3); /* sorted, unique */
    ASSERT_EQ_I64(v->u.il.data[0], 1);
    ASSERT_EQ_I64(v->u.il.data[1], 3);
    ASSERT_EQ_I64(v->u.il.data[2], 5);
    ASSERT_OK(atree_event_set_int_list(ev, "segments", NULL, 0)); /* empty list */
    ASSERT_EQ_U64(v->kind, ATREE_V_INT_LIST);
    ASSERT_EQ_U64(v->u.il.len, 0);

    ASSERT_OK(atree_event_set_string_list(ev, "deals", deals, NULL, 4));
    v = atree__event_value(ev, A_SLIST);
    ASSERT_EQ_U64(v->kind, ATREE_V_STRING_LIST);
    ASSERT_EQ_U64(v->u.sl.len, 3); /* {unknown->0, CA, US} */
    ASSERT_EQ_U64(v->u.sl.data[0], ATREE_STR_UNKNOWN);
    ASSERT_EQ_U64(v->u.sl.data[1], id_ca);
    ASSERT_EQ_U64(v->u.sl.data[2], id_us);

    ASSERT_OK(atree_event_set_undefined(ev, "flag"));
    ASSERT_EQ_U64(atree__event_value(ev, A_BOOL)->kind, ATREE_V_UNDEFINED);
    ASSERT_OK(atree_event_set_undefined_id(ev, A_SLIST));
    ASSERT_EQ_U64(atree__event_value(ev, A_SLIST)->kind, ATREE_V_UNDEFINED);

    atree_event_clear(ev);
    {
        uint32_t i;
        for (i = 0; i < A_COUNT; i++) {
            ASSERT_EQ_U64(atree__event_value(ev, i)->kind, ATREE_V_UNDEFINED);
        }
    }

    atree_event_destroy(ev);
    atree_event_destroy(NULL);
    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(validation_errors)
{
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    int64_t one[] = {1};
    const char *s[] = {"x"};
    const char *nul[] = {NULL};
    ASSERT_OK(atree_create(NULL, DEFS, A_COUNT, &t));
    ASSERT_OK(atree_event_create(t, &ev));

    ASSERT_STATUS(atree_event_set_bool(ev, "nope", true), ATREE_ERR_UNKNOWN_ATTR);
    ASSERT_STATUS(atree_event_set_bool(ev, NULL, true), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_event_set_bool(NULL, "flag", true), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_event_set_bool(ev, "age", true), ATREE_ERR_TYPE_MISMATCH);
    ASSERT_STATUS(atree_event_set_int(ev, "flag", 1), ATREE_ERR_TYPE_MISMATCH);
    ASSERT_STATUS(atree_event_set_float(ev, "age", 1.0), ATREE_ERR_TYPE_MISMATCH);
    ASSERT_STATUS(atree_event_set_string(ev, "age", "x", 1), ATREE_ERR_TYPE_MISMATCH);
    ASSERT_STATUS(atree_event_set_string(ev, "country", NULL, 0), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_event_set_int_list(ev, "deals", one, 1), ATREE_ERR_TYPE_MISMATCH);
    ASSERT_STATUS(atree_event_set_int_list(ev, "segments", NULL, 1), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_event_set_string_list(ev, "segments", s, NULL, 1), ATREE_ERR_TYPE_MISMATCH);
    ASSERT_STATUS(atree_event_set_string_list(ev, "deals", nul, NULL, 1), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_event_set_int_id(ev, A_COUNT, 1), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_event_set_undefined_id(ev, A_COUNT), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_event_set_undefined(ev, "nope"), ATREE_ERR_UNKNOWN_ATTR);
    ASSERT_STATUS(atree_event_create(NULL, &ev), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_event_create(t, NULL), ATREE_ERR_INVALID_ARG);

    /* a failed set leaves the previous value in place */
    ASSERT_OK(atree_event_set_int(ev, "age", 7));
    ASSERT_STATUS(atree_event_set_float(ev, "age", 1.0), ATREE_ERR_TYPE_MISMATCH);
    ASSERT_EQ_I64(atree__event_value(ev, A_INT)->u.i, 7);

    atree_event_destroy(ev);
    atree_destroy(t);
    return 0;
}

/* Steady state is allocation-free, and event memory is never charged to the
 * tree (reader threads must not write tree counters). */
TEST(reusable_without_allocation_and_separate_from_tree)
{
    struct test_alloc ta;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_stats_t before;
    atree_stats_t after;
    int64_t segs[64];
    const char *deals[8] = {"a", "b", "c", "d", "e", "f", "g", "h"};
    size_t attempts;
    int round;
    int i;

    for (i = 0; i < 64; i++) {
        segs[i] = 64 - i;
    }
    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(atree_create(&cfg, DEFS, A_COUNT, &t));
    atree_stats(t, &before);

    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_event_set_int_list(ev, "segments", segs, 64));
    ASSERT_OK(atree_event_set_string_list(ev, "deals", deals, NULL, 8));
    ASSERT_OK(atree_event_set_string(ev, "country", "CA", 2));
    attempts = ta.attempts;
    for (round = 0; round < 1000; round++) {
        atree_event_clear(ev);
        ASSERT_OK(atree_event_set_int_list(ev, "segments", segs, (size_t)(round % 65)));
        ASSERT_OK(atree_event_set_string_list(ev, "deals", deals, NULL, (size_t)(round % 9)));
        ASSERT_OK(atree_event_set_string(ev, "country", "CA", 2));
        ASSERT_OK(atree_event_set_int(ev, "age", round));
    }
    ASSERT_EQ_U64(ta.attempts, attempts);
    ASSERT_EQ_U64(atree__event_value(ev, A_ILIST)->u.il.len, 999 % 65);

    atree_stats(t, &after);
    ASSERT_EQ_U64(after.bytes_allocated, before.bytes_allocated);
    ASSERT_EQ_U64(after.bytes_peak, before.bytes_peak);
    ASSERT_TRUE(ta.live > before.bytes_allocated);                 /* the event's bytes exist... */
    ASSERT_EQ_U64(ev->mem.live, ta.live - before.bytes_allocated); /* ...on its own account */

    atree_event_destroy(ev);
    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(allocation_failures_do_not_leak)
{
    struct test_alloc ta;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    int64_t segs[100];
    size_t k;
    int i;
    for (i = 0; i < 100; i++) {
        segs[i] = i;
    }
    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(atree_create(&cfg, DEFS, A_COUNT, &t));

    for (k = 1; k <= 6; k++) {
        size_t base = ta.attempts;
        atree_status_t st;
        ta.fail_at = base + k;
        st = atree_event_create(t, &ev);
        if (st == ATREE_OK) {
            st = atree_event_set_int_list(ev, "segments", segs, 100);
            if (st != ATREE_OK) {
                ASSERT_STATUS(st, ATREE_ERR_NOMEM);
                ASSERT_EQ_U64(atree__event_value(ev, A_ILIST)->kind, ATREE_V_UNDEFINED);
            }
            atree_event_destroy(ev);
        } else {
            ASSERT_STATUS(st, ATREE_ERR_NOMEM);
            ASSERT_NULL(ev);
        }
        ta.fail_at = 0;
    }
    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(set_by_name_and_id);
RUN_TEST(validation_errors);
RUN_TEST(reusable_without_allocation_and_separate_from_tree);
RUN_TEST(allocation_failures_do_not_leak);
TEST_MAIN_END()

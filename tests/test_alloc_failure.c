/*
 * Allocation-failure tests for the tree: every allocation point in insert,
 * delete, report creation and search is failed in turn. An insert that
 * fails must leave the tree exactly as it was (same node/edge/subscription
 * counts, valid structure, same search results); nothing may leak.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "test_alloc.h"

static const atree_attr_def_t DEFS[] = {
    {"a", ATREE_TYPE_BOOL},         {"b", ATREE_TYPE_BOOL},   {"i", ATREE_TYPE_INT},
    {"f", ATREE_TYPE_FLOAT},        {"s", ATREE_TYPE_STRING}, {"il", ATREE_TYPE_INT_LIST},
    {"sl", ATREE_TYPE_STRING_LIST},
};
#define NDEFS (sizeof DEFS / sizeof DEFS[0])

static const char *const SCRIPT[] = {
    "a and i = 1",
    "(a and i = 1) or s = 'x'",
    "b or sl one of ['p', 'q', 'r'] or il all of [1, 2]",
    "not (a or b) and f > 1.5",
    "i in [1, 2, 3] and s in ['x', 'y'] and (a or b)",
    "true",
    "a and false",
    "(a and i = 1) xor b",
    "il is empty or s is null",
    "a and b and i = 1 and f < 2 and s <> 'z'",
    "(a or b) and (i = 1 or i = 2) and (s = 'x' or s = 'y')",
    "not a",
};
#define NSCRIPT (sizeof SCRIPT / sizeof SCRIPT[0])

static int valid(const atree_t *t)
{
    char msg[256];
    if (atree_validate(t, msg, sizeof msg) != ATREE_OK) {
        fprintf(stderr, "validate: %s\n", msg);
        return 0;
    }
    return 1;
}

static int same_shape(const atree_stats_t *a, const atree_stats_t *b)
{
    return a->nodes == b->nodes && a->leaves == b->leaves && a->edges == b->edges &&
        a->subscriptions == b->subscriptions && a->max_level == b->max_level;
}

/* Fills an event that matches several script entries. */
static int make_event(atree_t *t, atree_event_t **out)
{
    const char *sl[] = {"q"};
    int64_t il[] = {1, 2, 5};
    if (atree_event_create(t, out) != ATREE_OK) {
        return 1;
    }
    (void)atree_event_set_bool(*out, "a", true);
    (void)atree_event_set_bool(*out, "b", false);
    (void)atree_event_set_int(*out, "i", 1);
    (void)atree_event_set_float(*out, "f", 1.75);
    (void)atree_event_set_string(*out, "s", "x", 1);
    (void)atree_event_set_int_list(*out, "il", il, 3);
    (void)atree_event_set_string_list(*out, "sl", sl, NULL, 1);
    return 0;
}

/* Runs the whole script on a fresh tree with the given allocator. */
static int run_script(struct test_alloc *ta, atree_t **out, size_t *attempts)
{
    atree_config_t cfg;
    size_t i;
    size_t base = ta->attempts;
    atree_config_init(&cfg);
    cfg.allocator = &ta->a;
    if (atree_create(&cfg, DEFS, NDEFS, out) != ATREE_OK) {
        return 1;
    }
    for (i = 0; i < NSCRIPT; i++) {
        if (atree_insert(*out, (atree_id_t)(i + 1), SCRIPT[i], SIZE_MAX, NULL) != ATREE_OK) {
            return 1;
        }
    }
    *attempts = ta->attempts - base;
    return 0;
}

TEST(insert_failures_roll_back_completely)
{
    struct test_alloc ta;
    atree_t *ref = NULL;
    atree_event_t *ref_ev = NULL;
    atree_report_t *ref_rep = NULL;
    atree_stats_t ref_stats;
    size_t total;
    size_t k;
    int problems = 0;

    /* Reference run: the expected final shape and search result. */
    test_alloc_init(&ta);
    ASSERT_FALSE(run_script(&ta, &ref, &total));
    ASSERT_TRUE(total > 50);
    atree_stats(ref, &ref_stats);
    ASSERT_FALSE(make_event(ref, &ref_ev));
    ASSERT_OK(atree_report_create(ref, &ref_rep));
    ASSERT_OK(atree_search(ref, ref_ev, ref_rep));
    ASSERT_TRUE(atree_report_count(ref_rep) >= 3);

    for (k = 1; k <= total && problems == 0; k++) {
        struct test_alloc tb;
        atree_config_t cfg;
        atree_t *t = NULL;
        size_t i;
        int failed_once = 0;
        test_alloc_init(&tb);
        atree_config_init(&cfg);
        cfg.allocator = &tb.a;
        tb.fail_at = k;
        if (atree_create(&cfg, DEFS, NDEFS, &t) != ATREE_OK) {
            /* create itself failed: nothing to check but cleanliness */
            if (!test_alloc_clean(&tb)) {
                fprintf(stderr, "k=%lu: create leaked\n", (unsigned long)k);
                problems++;
            }
            continue;
        }
        for (i = 0; i < NSCRIPT; i++) {
            atree_stats_t before;
            atree_stats_t after;
            atree_error_t err;
            atree_status_t st;
            atree_stats(t, &before);
            st = atree_insert(t, (atree_id_t)(i + 1), SCRIPT[i], SIZE_MAX, &err);
            if (st == ATREE_OK) {
                continue;
            }
            if (st != ATREE_ERR_NOMEM) {
                fprintf(stderr, "k=%lu script %lu: %s\n", (unsigned long)k, (unsigned long)i,
                        atree_strerror(st));
                problems++;
                break;
            }
            failed_once = 1;
            atree_stats(t, &after);
            if (!same_shape(&before, &after) || !valid(t) ||
                atree_contains(t, (atree_id_t)(i + 1))) {
                fprintf(stderr, "k=%lu script %lu: tree changed by a failed insert\n",
                        (unsigned long)k, (unsigned long)i);
                problems++;
                break;
            }
            /* retry without failure: must succeed and land on the same shape */
            tb.fail_at = 0;
            if (atree_insert(t, (atree_id_t)(i + 1), SCRIPT[i], SIZE_MAX, &err) != ATREE_OK) {
                fprintf(stderr, "k=%lu script %lu: retry failed\n", (unsigned long)k,
                        (unsigned long)i);
                problems++;
                break;
            }
        }
        tb.fail_at = 0;
        if (problems == 0) {
            atree_stats_t final_stats;
            atree_event_t *ev = NULL;
            atree_report_t *rep = NULL;
            atree_stats(t, &final_stats);
            if (!same_shape(&final_stats, &ref_stats)) {
                fprintf(stderr, "k=%lu: final shape differs (failed_once=%d)\n", (unsigned long)k,
                        failed_once);
                problems++;
            }
            if (make_event(t, &ev) == 0 && atree_report_create(t, &rep) == ATREE_OK &&
                atree_search(t, ev, rep) == ATREE_OK) {
                if (atree_report_count(rep) != atree_report_count(ref_rep) ||
                    memcmp(atree_report_matches(rep), atree_report_matches(ref_rep),
                           atree_report_count(rep) * sizeof(atree_id_t)) != 0) {
                    fprintf(stderr, "k=%lu: search result differs\n", (unsigned long)k);
                    problems++;
                }
            } else {
                problems++;
            }
            atree_report_destroy(rep);
            atree_event_destroy(ev);
            for (i = 0; i < NSCRIPT; i++) {
                (void)atree_delete(t, (atree_id_t)(i + 1));
            }
            {
                atree_stats_t empty;
                atree_stats(t, &empty);
                if (empty.nodes != 0 || empty.subscriptions != 0 || !valid(t)) {
                    fprintf(stderr, "k=%lu: not empty after deleting all\n", (unsigned long)k);
                    problems++;
                }
            }
        }
        atree_destroy(t);
        if (!test_alloc_clean(&tb)) {
            fprintf(stderr, "k=%lu: leak or size mismatch (live %lu)\n", (unsigned long)k,
                    (unsigned long)tb.live);
            problems++;
        }
    }
    atree_report_destroy(ref_rep);
    atree_event_destroy(ref_ev);
    atree_destroy(ref);
    ASSERT_TRUE(test_alloc_clean(&ta));
    ASSERT_EQ_I64(problems, 0);
    return 0;
}

TEST(delete_and_search_failures)
{
    struct test_alloc ta;
    atree_t *t = NULL;
    size_t total;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_stats_t before;
    atree_stats_t after;
    size_t k;
    size_t base;
    int problems = 0;

    test_alloc_init(&ta);
    ASSERT_FALSE(run_script(&ta, &t, &total));
    ASSERT_FALSE(make_event(t, &ev));
    atree_stats(t, &before);

    /* Delete may only fail before it mutates anything. */
    base = ta.attempts;
    ta.fail_at = base + 1;
    {
        atree_status_t st = atree_delete(t, 5);
        ta.fail_at = 0;
        atree_stats(t, &after);
        if (st == ATREE_OK) {
            ASSERT_TRUE(after.subscriptions == before.subscriptions - 1);
            ASSERT_OK(atree_insert(t, 5, SCRIPT[4], SIZE_MAX, NULL));
        } else {
            ASSERT_STATUS(st, ATREE_ERR_NOMEM);
            ASSERT_TRUE(same_shape(&before, &after));
            ASSERT_TRUE(atree_contains(t, 5));
        }
        ASSERT_TRUE(valid(t));
    }

    /* Report creation and search: fail each allocation point; the tree
     * must be unaffected and a report must stay usable afterwards. */
    for (k = 1; k <= 12; k++) {
        atree_status_t st;
        base = ta.attempts;
        ta.fail_at = base + k;
        st = atree_report_create(t, &rep);
        if (st == ATREE_OK) {
            st = atree_search(t, ev, rep);
            if (st != ATREE_OK) {
                if (st != ATREE_ERR_NOMEM || atree_report_count(rep) != 0) {
                    problems++;
                }
                /* the next search on the same report works */
                ta.fail_at = 0;
                if (atree_search(t, ev, rep) != ATREE_OK || atree_report_count(rep) < 3) {
                    problems++;
                }
            }
            atree_report_destroy(rep);
            rep = NULL;
        } else if (st != ATREE_ERR_NOMEM || rep != NULL) {
            problems++;
        }
        ta.fail_at = 0;
        atree_stats(t, &after);
        if (!same_shape(&before, &after) || !valid(t)) {
            problems++;
        }
    }
    ASSERT_EQ_I64(problems, 0);

    atree_event_destroy(ev);
    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(insert_failures_roll_back_completely);
RUN_TEST(delete_and_search_failures);
TEST_MAIN_END()

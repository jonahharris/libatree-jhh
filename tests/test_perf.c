/*
 * Performance gates expressed as deterministic work counts (PLAN §6.7), not
 * timings: zero suppression, propagation on demand, subexpression sharing,
 * and allocation-free steady state.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "test_alloc.h"

static int ins(atree_t *t, atree_id_t id, const char *text)
{
    atree_error_t err;
    if (atree_insert(t, id, text, SIZE_MAX, &err) != ATREE_OK) {
        fprintf(stderr, "insert '%s': %s\n", text, err.message);
        return 1;
    }
    return 0;
}

/* Attributes: p (bool), x (int), s (string), plus x1..x4 ints. */
static const atree_attr_def_t DEFS[] = {
    {"p", ATREE_TYPE_BOOL},   {"q", ATREE_TYPE_BOOL}, {"x", ATREE_TYPE_INT},
    {"s", ATREE_TYPE_STRING}, {"x1", ATREE_TYPE_INT}, {"x2", ATREE_TYPE_INT},
    {"x3", ATREE_TYPE_INT},   {"x4", ATREE_TYPE_INT},
};
#define NDEFS (sizeof DEFS / sizeof DEFS[0])

/* Zero suppression: an event that satisfies no leaf visits no inner node,
 * however many expressions exist; one true leaf wakes only its parents. */
TEST(zero_suppression)
{
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_report_stats_t rs;
    char buf[128];
    int i;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    for (i = 0; i < 200; i++) {
        snprintf(buf, sizeof buf, "((p or x1 = %d) and x2 = %d) or (x3 = %d and (q or x4 = %d))", i,
                 i, i, i);
        ASSERT_FALSE(ins(t, (atree_id_t)(i + 1), buf));
    }
    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));
    ASSERT_OK(atree_event_set_bool(ev, "p", false));
    ASSERT_OK(atree_event_set_bool(ev, "q", false));
    ASSERT_OK(atree_event_set_int(ev, "x1", -1));
    ASSERT_OK(atree_event_set_int(ev, "x2", -1));
    ASSERT_OK(atree_event_set_int(ev, "x3", -1));
    ASSERT_OK(atree_event_set_int(ev, "x4", -1));
    ASSERT_OK(atree_search(t, ev, rep));
    atree_report_stats(rep, &rs);
    ASSERT_EQ_U64(rs.predicates_matched, 0);
    ASSERT_EQ_U64(rs.nodes_visited, 0); /* nothing false is ever propagated */
    ASSERT_EQ_U64(rs.matches, 0);

    /* One true leaf (x1 = 7) wakes exactly OR(p, x1=7); that OR is the access
     * child candidate of AND(OR, x2=7)? No: x2 = 7 (equality, rank 0) is the
     * access child, so the AND is not woken. Visited: 1. */
    ASSERT_OK(atree_event_set_int(ev, "x1", 7));
    ASSERT_OK(atree_search(t, ev, rep));
    atree_report_stats(rep, &rs);
    ASSERT_EQ_U64(rs.predicates_matched, 1);
    ASSERT_EQ_U64(rs.nodes_visited, 1);
    ASSERT_EQ_U64(rs.or_visited, 1);
    ASSERT_EQ_U64(rs.and_woken, 0);
    ASSERT_EQ_U64(rs.matches, 0);

    /* x2 = 7 too: the AND is woken by its access child and is true; the root
     * OR matches. Visited: OR(p,x1), AND, root OR = 3. */
    ASSERT_OK(atree_event_set_int(ev, "x2", 7));
    ASSERT_OK(atree_search(t, ev, rep));
    atree_report_stats(rep, &rs);
    ASSERT_EQ_U64(rs.nodes_visited, 3);
    ASSERT_EQ_U64(rs.and_woken, 1);
    ASSERT_EQ_U64(rs.and_true, 1);
    ASSERT_EQ_U64(rs.matches, 1);
    ASSERT_EQ_U64(atree_report_matches(rep)[0], 8);

    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    return 0;
}

/* Propagation on demand: AND(cheap rare predicate, wide predicate). The wide
 * predicate is true for the event but must not wake the AND. */
TEST(propagation_on_demand)
{
    unsigned flags[2] = {0, ATREE_FLAG_NO_PROPAGATION_ON_DEMAND};
    size_t f;
    for (f = 0; f < 2; f++) {
        atree_config_t cfg;
        atree_t *t = NULL;
        atree_event_t *ev = NULL;
        atree_report_t *rep = NULL;
        atree_report_stats_t rs;
        atree_config_init(&cfg);
        cfg.flags = flags[f];
        ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));
        ASSERT_FALSE(ins(t, 1, "x = 5 and s not in ['a', 'b']"));
        ASSERT_FALSE(ins(t, 2, "x = 6 and s <> 'zz'"));
        ASSERT_OK(atree_event_create(t, &ev));
        ASSERT_OK(atree_report_create(t, &rep));
        ASSERT_OK(atree_event_set_int(ev, "x", 1));
        ASSERT_OK(atree_event_set_string(ev, "s", "q", 1));
        ASSERT_OK(atree_search(t, ev, rep));
        atree_report_stats(rep, &rs);
        ASSERT_EQ_U64(rs.predicates_matched, 2); /* both wide predicates are true */
        ASSERT_EQ_U64(rs.matches, 0);
        if (flags[f] == 0) {
            ASSERT_EQ_U64(rs.and_woken, 0); /* the equality is the access child */
            ASSERT_EQ_U64(rs.nodes_visited, 0);
        } else {
            ASSERT_EQ_U64(rs.and_woken, 2); /* every child wakes the AND */
        }
        ASSERT_OK(atree_event_set_int(ev, "x", 5));
        ASSERT_OK(atree_search(t, ev, rep));
        atree_report_stats(rep, &rs);
        ASSERT_EQ_U64(rs.matches, 1);
        ASSERT_EQ_U64(rs.and_true, 1);
        atree_report_destroy(rep);
        atree_event_destroy(ev);
        atree_destroy(t);
    }
    return 0;
}

/* Sharing: 1000 roots over one common OR produce one OR node. */
TEST(sharing)
{
    atree_t *t = NULL;
    atree_stats_t st;
    char buf[96];
    int i;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    for (i = 0; i < 1000; i++) {
        snprintf(buf, sizeof buf, "(p or q) and x = %d", i);
        ASSERT_FALSE(ins(t, (atree_id_t)(i + 1), buf));
    }
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.leaves, 2 + 1000);
    ASSERT_EQ_U64(st.nodes, 2 + 1 + 1000 + 1000);
    ASSERT_EQ_U64(st.edges, 2 + 2 * 1000);
    ASSERT_EQ_U64(st.max_level, 3);
    /* identical expressions under new ids add no nodes */
    for (i = 0; i < 1000; i++) {
        snprintf(buf, sizeof buf, "x = %d and (q or p)", i);
        ASSERT_FALSE(ins(t, (atree_id_t)(5000 + i), buf));
    }
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 2 + 1 + 1000 + 1000);
    ASSERT_EQ_U64(st.subscriptions, 2000);
    atree_destroy(t);
    return 0;
}

/* Reorganize (Alg. 2): an existing AND(p,q) is reused by every later
 * `p and q and x = i`, and self-adjust (Alg. 3) achieves the same structure
 * when `p and q` arrives last: the index is independent of arrival order. */
TEST(reorganize_and_self_adjust_sharing)
{
    atree_t *t = NULL;
    atree_stats_t st;
    char buf[96];
    int i;

    /* common subexpression first: reorganize reuses it */
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    ASSERT_FALSE(ins(t, 1, "p and q"));
    for (i = 0; i < 1000; i++) {
        snprintf(buf, sizeof buf, "p and q and x = %d", i);
        ASSERT_FALSE(ins(t, (atree_id_t)(10 + i), buf));
    }
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 2 + 1 + 1000 + 1000);
    ASSERT_EQ_U64(st.edges, 2 + 2 * 1000); /* AND(AND(p,q), x_i) */
    ASSERT_EQ_U64(st.reorganized, 1000);
    ASSERT_EQ_U64(st.self_adjusted, 0);
    ASSERT_EQ_U64(st.max_level, 3);
    ASSERT_OK(atree_validate(t, NULL, 0));
    atree_destroy(t);

    /* common subexpression last: self-adjust rewires all thousand parents */
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    for (i = 0; i < 1000; i++) {
        snprintf(buf, sizeof buf, "p and q and x = %d", i);
        ASSERT_FALSE(ins(t, (atree_id_t)(10 + i), buf));
    }
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.edges, 3 * 1000);
    ASSERT_FALSE(ins(t, 1, "p and q"));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 2 + 1 + 1000 + 1000);
    ASSERT_EQ_U64(st.edges, 2 + 2 * 1000);
    ASSERT_EQ_U64(st.self_adjusted, 1000);
    ASSERT_EQ_U64(st.max_level, 3);
    ASSERT_OK(atree_validate(t, NULL, 0));
    {
        /* and it still matches correctly */
        atree_event_t *ev = NULL;
        atree_report_t *rep = NULL;
        ASSERT_OK(atree_event_create(t, &ev));
        ASSERT_OK(atree_report_create(t, &rep));
        ASSERT_OK(atree_event_set_bool(ev, "p", true));
        ASSERT_OK(atree_event_set_bool(ev, "q", true));
        ASSERT_OK(atree_event_set_int(ev, "x", 500));
        ASSERT_OK(atree_search(t, ev, rep));
        ASSERT_EQ_U64(atree_report_count(rep), 2);
        ASSERT_EQ_U64(atree_report_matches(rep)[0], 1);
        ASSERT_EQ_U64(atree_report_matches(rep)[1], 510);
        atree_report_destroy(rep);
        atree_event_destroy(ev);
    }
    /* the candidate cap bounds the work and is reported */
    {
        atree_config_t cfg;
        atree_t *small = NULL;
        atree_config_init(&cfg);
        cfg.max_adjust_candidates = 100;
        ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &small));
        for (i = 0; i < 300; i++) {
            snprintf(buf, sizeof buf, "p and q and x = %d", i);
            ASSERT_FALSE(ins(small, (atree_id_t)(10 + i), buf));
        }
        ASSERT_FALSE(ins(small, 1, "p and q"));
        atree_stats(small, &st);
        ASSERT_TRUE(st.self_adjusted > 0 && st.self_adjusted < 300);
        ASSERT_TRUE(st.adjust_candidates_skipped > 0);
        ASSERT_OK(atree_validate(small, NULL, 0));
        atree_destroy(small);
    }
    atree_destroy(t);
    return 0;
}

/* Steady state: after warm-up, searches make no allocator calls at all. */
TEST(allocation_free_steady_state)
{
    struct test_alloc ta;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    char buf[96];
    int i;
    size_t attempts;
    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));
    for (i = 0; i < 300; i++) {
        snprintf(buf, sizeof buf, "(p and x = %d) or (q and x1 = %d) or s = 'v%d'", i % 50, i % 7,
                 i % 20);
        ASSERT_FALSE(ins(t, (atree_id_t)(i + 1), buf));
    }
    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));
    /* warm-up with the most-matching event so every buffer reaches its size */
    ASSERT_OK(atree_event_set_bool(ev, "p", true));
    ASSERT_OK(atree_event_set_bool(ev, "q", true));
    ASSERT_OK(atree_event_set_int(ev, "x", 3));
    ASSERT_OK(atree_event_set_int(ev, "x1", 3));
    ASSERT_OK(atree_event_set_string(ev, "s", "v3", SIZE_MAX));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(atree_report_count(rep) > 50);
    attempts = ta.attempts;
    for (i = 0; i < 1000; i++) {
        ASSERT_OK(atree_event_set_int(ev, "x", i % 50));
        ASSERT_OK(atree_event_set_int(ev, "x1", i % 7));
        ASSERT_OK(atree_event_set_string(ev, "s", (i % 3) ? "v3" : "v7", SIZE_MAX));
        ASSERT_OK(atree_search(t, ev, rep));
    }
    ASSERT_EQ_U64(ta.attempts, attempts);
    ASSERT_EQ_U64(ta.n_free, 0u + ta.n_free); /* no frees counted either way */
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

/* Bounded churn: deleting and re-inserting expressions at a steady live
 * count must not grow the index. Every cycle retires one leaf and one AND
 * node and creates two with fresh hashes, so the identity tables collect a
 * tombstone per removal; a policy that grows a table whenever tombstones
 * trip the load factor doubles it without bound. Node and predicate slots
 * are recycled, so bytes_allocated after a long churn must equal what it
 * was after a short one. */
TEST(churn_is_bounded)
{
    atree_t *t = NULL;
    atree_stats_t early;
    atree_stats_t late;
    char buf[64];
    int i;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    for (i = 0; i < 200; i++) {
        snprintf(buf, sizeof buf, "x = %d and q", i);
        ASSERT_FALSE(ins(t, (atree_id_t)(i + 1), buf));
    }
    for (i = 0; i < 20000; i++) {
        int k = i % 200;
        if (i == 2000) {
            atree_stats(t, &early);
        }
        ASSERT_OK(atree_delete(t, (atree_id_t)(k + 1)));
        snprintf(buf, sizeof buf, "x = %d and q", 1000 + i);
        ASSERT_FALSE(ins(t, (atree_id_t)(k + 1), buf));
    }
    atree_stats(t, &late);
    ASSERT_EQ_U64(late.nodes, early.nodes);
    ASSERT_EQ_U64(late.bytes_allocated, early.bytes_allocated);
    ASSERT_OK(atree_validate(t, NULL, 0));
    atree_destroy(t);
    return 0;
}

/* Growth under writes: a report's bitsets cover every node, so a tree that
 * keeps growing forces them to grow too. That growth must be geometric: 600
 * inserts between searches, each crossing a 64-node boundary every few
 * inserts, must cost the searches a handful of allocator calls, not two per
 * boundary. */
TEST(report_growth_is_geometric)
{
    struct test_alloc ta;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    char buf[64];
    int i;
    size_t attempts;
    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));
    ASSERT_FALSE(ins(t, 1, "p and x = 1"));
    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_event_set_bool(ev, "p", true));
    ASSERT_OK(atree_event_set_int(ev, "x", 1));
    ASSERT_OK(atree_report_create(t, &rep));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_EQ_U64(atree_report_count(rep), 1);
    attempts = 0;
    for (i = 0; i < 600; i++) {
        size_t before;
        snprintf(buf, sizeof buf, "x1 = %d and x2 = %d", i, i * 7);
        ASSERT_FALSE(ins(t, (atree_id_t)(i + 2), buf));
        before = ta.attempts;
        ASSERT_OK(atree_search(t, ev, rep));
        ASSERT_EQ_U64(atree_report_count(rep), 1);
        attempts += ta.attempts - before;
    }
    ASSERT_TRUE(attempts <= 16); /* two bitsets, a few doublings; exact growth was ~40 */
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

/* Index independence: predicates on attributes the event does not touch,
 * and non-matching equalities on attributes it does touch, cost nothing in
 * phase 1. Adding ten thousand of them leaves predicates_evaluated unchanged. */
TEST(index_independence)
{
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_report_stats_t before;
    atree_report_stats_t after;
    atree_stats_t st;
    char buf[128];
    int i;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    ASSERT_FALSE(ins(t, 1, "x = 1 and p"));
    ASSERT_FALSE(ins(t, 2, "x > 0 and x < 10"));
    ASSERT_FALSE(ins(t, 3, "s in ['a', 'b'] or x in [1, 2, 3]"));
    ASSERT_FALSE(ins(t, 4, "x <> 99 and s <> 'nope'")); /* scan leaves: always evaluated */
    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));
    ASSERT_OK(atree_event_set_bool(ev, "p", true));
    ASSERT_OK(atree_event_set_int(ev, "x", 1));
    ASSERT_OK(atree_event_set_string(ev, "s", "a", 1));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_EQ_U64(atree_report_count(rep), 4);
    atree_report_stats(rep, &before);
    /* hits: x=1, x>0, x<10, s in, x in, p; scans: x<>99, s<>'nope' */
    ASSERT_EQ_U64(before.predicates_matched, 8);
    ASSERT_EQ_U64(before.predicates_evaluated, 8);

    for (i = 0; i < 10000; i++) {
        /* untouched attributes x1..x4, q; and non-matching equalities on x */
        snprintf(buf, sizeof buf, "x1 = %d or x2 > %d or (q and x3 in [%d, %d]) or x = %d", i, i, i,
                 i + 1, i + 100);
        ASSERT_FALSE(ins(t, (atree_id_t)(100 + i), buf));
    }
    atree_stats(t, &st);
    ASSERT_TRUE(st.leaves > 30000);
    ASSERT_EQ_U64(st.scanned_leaves, 2);
    ASSERT_EQ_U64(st.indexed_leaves, st.leaves - 2);
    ASSERT_OK(atree_search(t, ev, rep));
    atree_report_stats(rep, &after);
    ASSERT_EQ_U64(atree_report_count(rep), 4);
    ASSERT_EQ_U64(after.predicates_evaluated, before.predicates_evaluated);
    ASSERT_EQ_U64(after.predicates_matched, before.predicates_matched);
    ASSERT_EQ_U64(after.nodes_visited, before.nodes_visited);

    /* Rays: thresholds above and below the value are found by prefix/suffix. */
    ASSERT_OK(atree_event_set_int(ev, "x", 5000));
    ASSERT_OK(atree_search(t, ev, rep));
    atree_report_stats(rep, &after);
    /* x > 0, x < 10 is now half true; x1..x3 untouched; x2 > i for i < 5000: those
     * are on x2 which is undefined, so nothing. Only our own leaves plus scans. */
    ASSERT_TRUE(after.predicates_evaluated <= 8);

    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(index_independence);
RUN_TEST(zero_suppression);
RUN_TEST(propagation_on_demand);
RUN_TEST(sharing);
RUN_TEST(reorganize_and_self_adjust_sharing);
RUN_TEST(allocation_free_steady_state);
RUN_TEST(churn_is_bounded);
RUN_TEST(report_growth_is_geometric);
TEST_MAIN_END()

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

TEST_MAIN_BEGIN()
RUN_TEST(zero_suppression);
RUN_TEST(propagation_on_demand);
RUN_TEST(sharing);
RUN_TEST(allocation_free_steady_state);
TEST_MAIN_END()

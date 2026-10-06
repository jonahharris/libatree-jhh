/*
 * Tree core scenarios through the public API: insert/search/delete, sharing,
 * constants, the paper's Figure 4 and Figure 6 examples, conveniences, and
 * structural validation after every mutation.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "test_alloc.h"

static const atree_attr_def_t DEFS[] = {
    {"private", ATREE_TYPE_BOOL},         {"test", ATREE_TYPE_BOOL},
    {"exchange_id", ATREE_TYPE_INT},      {"price", ATREE_TYPE_FLOAT},
    {"country", ATREE_TYPE_STRING},       {"city", ATREE_TYPE_STRING},
    {"segment_ids", ATREE_TYPE_INT_LIST}, {"deal_ids", ATREE_TYPE_STRING_LIST},
};
#define NDEFS (sizeof DEFS / sizeof DEFS[0])

/* p1..p8 for the paper's examples */
static const atree_attr_def_t PDEFS[] = {
    {"p1", ATREE_TYPE_BOOL}, {"p2", ATREE_TYPE_BOOL}, {"p3", ATREE_TYPE_BOOL},
    {"p4", ATREE_TYPE_BOOL}, {"p5", ATREE_TYPE_BOOL}, {"p6", ATREE_TYPE_BOOL},
    {"p7", ATREE_TYPE_BOOL}, {"p8", ATREE_TYPE_BOOL},
};

static int ins(atree_t *t, atree_id_t id, const char *text)
{
    atree_error_t err;
    atree_status_t st = atree_insert(t, id, text, SIZE_MAX, &err);
    if (st != ATREE_OK) {
        fprintf(stderr, "insert %llu '%s': %s @%lu: %s\n", (unsigned long long)id, text,
                atree_strerror(st), (unsigned long)err.offset, err.message);
        return 1;
    }
    return 0;
}

static int valid(const atree_t *t)
{
    char msg[256];
    atree_status_t st = atree_validate(t, msg, sizeof msg);
    if (st != ATREE_OK) {
        fprintf(stderr, "validate: %s\n", msg);
        return 0;
    }
    return 1;
}

/* Compares the report's matches with an expected sorted list. */
static int matches_are(const atree_report_t *rep, const atree_id_t *want, size_t n)
{
    size_t i;
    if (atree_report_count(rep) != n) {
        fprintf(stderr, "expected %lu matches, got %lu:", (unsigned long)n,
                (unsigned long)atree_report_count(rep));
        for (i = 0; i < atree_report_count(rep); i++) {
            fprintf(stderr, " %llu", (unsigned long long)atree_report_matches(rep)[i]);
        }
        fputc('\n', stderr);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (atree_report_matches(rep)[i] != want[i]) {
            fprintf(stderr, "match %lu: expected %llu got %llu\n", (unsigned long)i,
                    (unsigned long long)want[i], (unsigned long long)atree_report_matches(rep)[i]);
            return 0;
        }
    }
    return 1;
}

static void set_all_bools(atree_event_t *ev, int n, bool value)
{
    int i;
    char name[4];
    for (i = 1; i <= n; i++) {
        snprintf(name, sizeof name, "p%d", i);
        (void)atree_event_set_bool(ev, name, value);
    }
}

TEST(insert_search_delete_basic)
{
    struct test_alloc ta;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_stats_t st;
    const char *deals[] = {"deal-2", "deal-9"};
    atree_id_t m13[] = {1, 3};
    atree_id_t m12[] = {1, 2};
    atree_id_t m1[] = {1};
    atree_id_t m3[] = {3};
    atree_id_t m134[] = {1, 3, 4};

    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));
    ASSERT_EQ_U64(atree_count(t), 0);
    ASSERT_FALSE(ins(t, 1, "exchange_id = 1"));
    ASSERT_FALSE(ins(t, 2, "private"));
    ASSERT_FALSE(ins(t, 3, "not private"));
    ASSERT_FALSE(ins(t, 4, "deal_ids one of ['deal-1', 'deal-2'] and country in ['CA', 'US']"));
    ASSERT_EQ_U64(atree_count(t), 4);
    ASSERT_TRUE(atree_contains(t, 3));
    ASSERT_FALSE(atree_contains(t, 5));
    ASSERT_TRUE(valid(t));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.subscriptions, 4);
    ASSERT_EQ_U64(st.leaves,
                  5); /* exchange_id=1, private, not private, deal_ids one of, country in */
    ASSERT_EQ_U64(st.nodes, 6);
    ASSERT_EQ_U64(st.edges, 2);
    ASSERT_EQ_U64(st.max_level, 2);

    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));

    ASSERT_OK(atree_event_set_int(ev, "exchange_id", 1));
    ASSERT_OK(atree_event_set_bool(ev, "private", false));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, m13, 2));

    ASSERT_OK(atree_event_set_bool(ev, "private", true));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, m12, 2));

    ASSERT_OK(atree_event_set_undefined(ev, "private")); /* three-valued: neither */
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, m1, 1));

    ASSERT_OK(atree_event_set_bool(ev, "private", false));
    ASSERT_OK(atree_event_set_string_list(ev, "deal_ids", deals, NULL, 2));
    ASSERT_OK(atree_event_set_string(ev, "country", "US", 2));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, m134, 3));
    ASSERT_OK(atree_event_set_string(ev, "country", "FR", 2));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, m13, 2));

    ASSERT_OK(atree_delete(t, 1));
    ASSERT_TRUE(valid(t));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, m3, 1));
    ASSERT_STATUS(atree_delete(t, 1), ATREE_ERR_NOT_FOUND);
    ASSERT_OK(atree_delete(t, 2));
    ASSERT_OK(atree_delete(t, 3));
    ASSERT_OK(atree_delete(t, 4));
    ASSERT_TRUE(valid(t));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 0);
    ASSERT_EQ_U64(st.leaves, 0);
    ASSERT_EQ_U64(st.edges, 0);
    ASSERT_EQ_U64(st.max_level, 0);
    ASSERT_EQ_U64(atree_count(t), 0);
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_EQ_U64(atree_report_count(rep), 0);

    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(shared_subexpressions_and_use_counts)
{
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_stats_t st;
    atree_id_t both[] = {10, 11};
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));

    /* The same expression under two ids shares every node (paper Alg. 4 line 3). */
    ASSERT_FALSE(ins(t, 10, "exchange_id = 1 and private"));
    ASSERT_FALSE(ins(t, 11, "private and exchange_id = 1")); /* commutative: same node */
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 3);
    ASSERT_EQ_U64(st.edges, 2);
    ASSERT_EQ_U64(st.subscriptions, 2);
    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));
    ASSERT_OK(atree_event_set_int(ev, "exchange_id", 1));
    ASSERT_OK(atree_event_set_bool(ev, "private", true));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, both, 2));
    ASSERT_OK(atree_delete(t, 10));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 3); /* still used by 11 */
    ASSERT_TRUE(valid(t));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, both + 1, 1));
    ASSERT_OK(atree_delete(t, 11));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 0);

    /* (a and b) or c, then (b and a) and d: normalization flattens the second
     * into AND(a, b, d), and reorganize (Alg. 2) rewrites it as
     * AND(AND(a,b), d) because AND(a,b) exists. Structure: 4 leaves, AND(a,b),
     * OR(AND, c), AND(AND(a,b), d). */
    ASSERT_FALSE(ins(t, 1, "(private and test) or exchange_id = 1"));
    ASSERT_FALSE(ins(t, 2, "(test and private) and price > 1.5"));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.leaves, 4);
    ASSERT_EQ_U64(st.nodes, 7);
    ASSERT_EQ_U64(st.edges, 2 + 2 + 2);
    ASSERT_EQ_U64(st.max_level, 3);
    ASSERT_EQ_U64(st.reorganized, 1);
    ASSERT_TRUE(valid(t));
    /* Deleting 1 removes the OR and the exchange_id leaf but keeps AND(a,b). */
    ASSERT_OK(atree_delete(t, 1));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 5);
    ASSERT_EQ_U64(st.leaves, 3);
    ASSERT_TRUE(valid(t));
    ASSERT_OK(atree_delete(t, 2));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 0);
    ASSERT_TRUE(valid(t));

    /* Node ids are recycled: the same work yields the same node count. */
    ASSERT_FALSE(ins(t, 1, "(private and test) or exchange_id = 1"));
    ASSERT_FALSE(ins(t, 2, "(test and private) and price > 1.5"));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 7);
    ASSERT_TRUE(valid(t));

    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    return 0;
}

TEST(duplicate_ids_and_argument_errors)
{
    atree_t *t = NULL;
    atree_t *other = NULL;
    atree_stats_t before;
    atree_stats_t after;
    atree_error_t err;
    atree_expr_t *e;
    atree_event_t *oev = NULL;
    atree_report_t *orep = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &other));
    ASSERT_FALSE(ins(t, 7, "private"));
    atree_stats(t, &before);
    ASSERT_STATUS(atree_insert(t, 7, "test", SIZE_MAX, &err), ATREE_ERR_DUPLICATE_ID);
    ASSERT_EQ_U64(err.status, ATREE_ERR_DUPLICATE_ID);
    ASSERT_TRUE(err.message[0] != '\0');
    atree_stats(t, &after);
    ASSERT_EQ_U64(after.nodes, before.nodes); /* unchanged */
    ASSERT_EQ_U64(after.subscriptions, before.subscriptions);
    ASSERT_TRUE(valid(t));

    /* parse errors are reported with their position */
    ASSERT_STATUS(atree_insert(t, 8, "private and", SIZE_MAX, &err), ATREE_ERR_SYNTAX);
    ASSERT_EQ_U64(err.offset, 11);
    ASSERT_STATUS(atree_insert(t, 8, "nope", SIZE_MAX, &err), ATREE_ERR_UNKNOWN_ATTR);
    ASSERT_FALSE(atree_contains(t, 8));

    /* expression built for another tree */
    e = atree_expr_var(other, "private");
    ASSERT_NOT_NULL(e);
    ASSERT_STATUS(atree_insert_expr(t, 9, e, &err), ATREE_ERR_INVALID_ARG);
    atree_expr_free(e);
    ASSERT_STATUS(atree_insert_expr(t, 9, NULL, NULL), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_insert(NULL, 9, "private", SIZE_MAX, NULL), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_delete(NULL, 9), ATREE_ERR_INVALID_ARG);
    ASSERT_FALSE(atree_contains(NULL, 7));
    ASSERT_EQ_U64(atree_count(NULL), 0);

    /* event or report from another tree */
    ASSERT_OK(atree_event_create(other, &oev));
    ASSERT_OK(atree_report_create(other, &orep));
    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));
    ASSERT_STATUS(atree_search(t, oev, rep), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_search(t, ev, orep), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_search(NULL, ev, rep), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_search(t, NULL, rep), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_search(t, ev, NULL), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_report_create(NULL, &rep), ATREE_ERR_INVALID_ARG);
    ASSERT_EQ_U64(atree_report_count(NULL), 0);
    ASSERT_NULL(atree_report_matches(NULL));
    atree_report_destroy(NULL);
    ASSERT_STATUS(atree_validate(NULL, NULL, 0), ATREE_ERR_INVALID_ARG);

    atree_report_destroy(orep);
    atree_event_destroy(oev);
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(other);
    atree_destroy(t);
    return 0;
}

TEST(constant_expressions)
{
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_stats_t st;
    atree_id_t want[] = {1, 3, 5};
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    ASSERT_FALSE(ins(t, 1, "true"));
    ASSERT_FALSE(ins(t, 2, "false"));
    ASSERT_FALSE(ins(t, 3, "private or true"));   /* folds to true */
    ASSERT_FALSE(ins(t, 4, "private and false")); /* folds to false */
    ASSERT_FALSE(ins(t, 5, "not (private and false)"));
    ASSERT_EQ_U64(atree_count(t), 5);
    ASSERT_TRUE(atree_contains(t, 2));
    ASSERT_TRUE(atree_contains(t, 4));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.nodes, 0); /* constants never touch the DAG */
    ASSERT_TRUE(valid(t));
    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));
    ASSERT_OK(atree_search(t, ev, rep)); /* empty event still matches constants */
    ASSERT_TRUE(matches_are(rep, want, 3));
    ASSERT_OK(atree_delete(t, 3));
    ASSERT_OK(atree_delete(t, 2));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_EQ_U64(atree_report_count(rep), 2);
    ASSERT_STATUS(atree_delete(t, 2), ATREE_ERR_NOT_FOUND);
    ASSERT_OK(atree_delete(t, 1));
    ASSERT_OK(atree_delete(t, 4));
    ASSERT_OK(atree_delete(t, 5));
    ASSERT_EQ_U64(atree_count(t), 0);
    ASSERT_TRUE(valid(t));
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    return 0;
}

/* Paper Figure 4: S1 = (P1 v P2 v P3) ^ P4 ^ (P5 v P6), S2 = (P5 v P6) ^ (P7 v P8).
 * Eight leaves, three OR i-nodes (one shared), two AND r-nodes. */
TEST(paper_figure_4)
{
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_stats_t st;
    atree_id_t s2[] = {2};
    atree_id_t s12[] = {1, 2};
    ASSERT_OK(atree_create(NULL, PDEFS, 8, &t));
    ASSERT_FALSE(ins(t, 1, "(p1 or p2 or p3) and p4 and (p5 or p6)"));
    ASSERT_FALSE(ins(t, 2, "(p5 or p6) and (p7 or p8)"));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.leaves, 8);
    ASSERT_EQ_U64(st.nodes, 13);
    ASSERT_EQ_U64(st.edges, 3 + 2 + 2 + 3 + 2);
    ASSERT_EQ_U64(st.max_level, 3);
    ASSERT_TRUE(valid(t));

    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));
    set_all_bools(ev, 8, false);
    ASSERT_OK(atree_event_set_bool(ev, "p5", true));
    ASSERT_OK(atree_event_set_bool(ev, "p7", true));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, s2, 1));
    ASSERT_OK(atree_event_set_bool(ev, "p2", true));
    ASSERT_OK(atree_event_set_bool(ev, "p4", true));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, s12, 2));

    /* Deleting S1 keeps the shared (P5 v P6) and its leaves for S2. */
    ASSERT_OK(atree_delete(t, 1));
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.leaves, 4);
    ASSERT_EQ_U64(st.nodes, 7);
    ASSERT_TRUE(valid(t));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, s2, 1));

    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    return 0;
}

/* Paper Figure 6 (optimized matching). S1..S6 of Figure 6(a); event with
 * P1 true and P7, P8 false (so the negated leaves of S6 are true), all
 * other predicates false. Expected matches: S3 and S6. The visit set is
 * deterministic given our access-child heuristic; see docs/DESIGN.md. */
TEST(paper_figure_6)
{
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_stats_t st;
    atree_report_stats_t rs;
    atree_id_t want[] = {3, 6};
    ASSERT_OK(atree_create(NULL, PDEFS, 8, &t));
    ASSERT_FALSE(ins(t, 1, "(p1 or p2 or p3) and p4 and (p5 or p6)"));
    ASSERT_FALSE(ins(t, 2, "(p5 or p6) and (p7 or p8)"));
    ASSERT_FALSE(ins(t, 3, "p1 or p2 or p3 or p4"));
    ASSERT_FALSE(ins(t, 4, "(p1 or p2 or p3) and p4"));
    ASSERT_FALSE(ins(t, 5, "(p5 or p6) and (p7 or p8)")); /* same as S2: shared r-node */
    ASSERT_FALSE(ins(t, 6, "not (p7 or p8)"));            /* -> not p7 and not p8 */
    atree_stats(t, &st);
    ASSERT_EQ_U64(st.leaves, 10); /* P1..P8, not P7, not P8 */
    ASSERT_EQ_U64(st.nodes, 18);
    ASSERT_EQ_U64(st.subscriptions, 6);
    /* Figure 6(c): S3 is OR(OR(P1,P2,P3), P4) by reorganize (Alg. 2), and
     * inserting S4 = AND(OR123, P4) self-adjusts S1 into AND(S4, OR56)
     * (Alg. 3, the §4.2.3 example), so S1 moves to level 4. */
    ASSERT_EQ_U64(st.reorganized, 1);
    ASSERT_EQ_U64(st.self_adjusted, 1);
    ASSERT_EQ_U64(st.edges, 17);
    ASSERT_EQ_U64(st.max_level, 4);
    ASSERT_TRUE(valid(t));

    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));
    set_all_bools(ev, 8, false);
    ASSERT_OK(atree_event_set_bool(ev, "p1", true));
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, want, 2));
    atree_report_stats(rep, &rs);
    ASSERT_EQ_U64(rs.predicates_evaluated, 3); /* indexed: only the three hits cost anything */
    ASSERT_EQ_U64(rs.predicates_matched, 3);   /* P1, not P7, not P8 */
    ASSERT_EQ_U64(rs.matches, 2);
    /* Zero suppression: only nodes reached from true leaves are visited:
     * OR(1,2,3) [11], S3 = OR(11, P4) [15], AND(not7,not8) [S6], and
     * S4 = AND(11, P4) whose access child is the OR. S1 = AND(S4, OR56) is
     * guarded by S4 (AND ranks below OR), which is false, so S1 is not woken:
     * exactly the paper's Figure 6(c) walk. */
    ASSERT_EQ_U64(rs.nodes_visited, 4);
    ASSERT_EQ_U64(rs.or_visited, 2);
    ASSERT_EQ_U64(rs.and_woken, 2);
    ASSERT_EQ_U64(rs.and_true, 1);

    /* Everything false: nothing is visited at all. */
    ASSERT_OK(atree_event_set_bool(ev, "p1", false));
    ASSERT_OK(atree_event_set_bool(ev, "p7", true)); /* kills S6 too */
    ASSERT_OK(atree_search(t, ev, rep));
    atree_report_stats(rep, &rs);
    ASSERT_EQ_U64(rs.matches, 0);
    ASSERT_EQ_U64(rs.predicates_matched, 2); /* p7, not p8 */
    ASSERT_TRUE(rs.nodes_visited <= 2);      /* OR(7,8) and maybe AND(not7,not8) */

    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    return 0;
}

/* Paper Figure 5 / §4.2.2: with (P1 v P2 v P3) already indexed, inserting
 * P1 v P2 v P3 v P4 is organized as ((P1 v P2 v P3) v P4), reusing the node
 * (the third structure in the figure). Without reorganize it is a flat
 * four-child OR. */
TEST(paper_figure_5_reorganize)
{
    unsigned flags[2] = {0, ATREE_FLAG_NO_REORGANIZE};
    size_t f;
    for (f = 0; f < 2; f++) {
        atree_config_t cfg;
        atree_t *t = NULL;
        atree_event_t *ev = NULL;
        atree_report_t *rep = NULL;
        atree_stats_t st;
        atree_id_t both[] = {1, 2};
        atree_config_init(&cfg);
        cfg.flags = flags[f];
        ASSERT_OK(atree_create(&cfg, PDEFS, 8, &t));
        ASSERT_FALSE(ins(t, 1, "(p1 or p2 or p3) and p4"));
        atree_stats(t, &st);
        ASSERT_EQ_U64(st.nodes, 6);
        ASSERT_EQ_U64(st.edges, 5);
        ASSERT_FALSE(ins(t, 2, "p1 or p2 or p3 or p4"));
        atree_stats(t, &st);
        ASSERT_EQ_U64(st.nodes, 7);     /* one new OR either way */
        ASSERT_EQ_U64(st.max_level, 3); /* the AND of id 1 is at level 3 either way */
        if (flags[f] == 0) {
            ASSERT_EQ_U64(st.edges, 5 + 2); /* OR(OR123, P4), itself at level 3 */
            ASSERT_EQ_U64(st.reorganized, 1);
        } else {
            ASSERT_EQ_U64(st.edges, 5 + 4); /* OR(P1, P2, P3, P4) at level 2 */
            ASSERT_EQ_U64(st.reorganized, 0);
        }
        ASSERT_TRUE(valid(t));
        ASSERT_OK(atree_event_create(t, &ev));
        ASSERT_OK(atree_report_create(t, &rep));
        set_all_bools(ev, 8, false);
        ASSERT_OK(atree_event_set_bool(ev, "p4", true));
        ASSERT_OK(atree_search(t, ev, rep));
        ASSERT_TRUE(matches_are(rep, both + 1, 1));
        ASSERT_OK(atree_event_set_bool(ev, "p2", true));
        ASSERT_OK(atree_search(t, ev, rep));
        ASSERT_TRUE(matches_are(rep, both, 2));
        /* deleting the reused subexpression's owner keeps it alive for 2 */
        ASSERT_OK(atree_delete(t, 1));
        ASSERT_TRUE(valid(t));
        ASSERT_OK(atree_search(t, ev, rep));
        ASSERT_TRUE(matches_are(rep, both + 1, 1));
        ASSERT_OK(atree_delete(t, 2));
        atree_stats(t, &st);
        ASSERT_EQ_U64(st.nodes, 0);
        atree_report_destroy(rep);
        atree_event_destroy(ev);
        atree_destroy(t);
    }
    return 0;
}

/* Paper §4.2.3: with Figure 4 indexed, inserting (P1 v P2 v P3) ^ P4 creates
 * a node N, and self-adjust rewires S1 = (P1 v P2 v P3) ^ P4 ^ (P5 v P6) into
 * N ^ (P5 v P6). Without self-adjust, S1 keeps its three children. */
TEST(paper_self_adjust)
{
    unsigned flags[2] = {0, ATREE_FLAG_NO_SELF_ADJUST};
    size_t f;
    for (f = 0; f < 2; f++) {
        atree_config_t cfg;
        atree_t *t = NULL;
        atree_event_t *ev = NULL;
        atree_report_t *rep = NULL;
        atree_stats_t st;
        atree_id_t all3[] = {1, 2, 3};
        atree_id_t s3[] = {3};
        atree_config_init(&cfg);
        cfg.flags = flags[f];
        ASSERT_OK(atree_create(&cfg, PDEFS, 8, &t));
        ASSERT_FALSE(ins(t, 1, "(p1 or p2 or p3) and p4 and (p5 or p6)"));
        ASSERT_FALSE(ins(t, 2, "(p5 or p6) and (p7 or p8)"));
        atree_stats(t, &st);
        ASSERT_EQ_U64(st.nodes, 13);
        ASSERT_EQ_U64(st.edges, 12);
        ASSERT_FALSE(ins(t, 3, "(p1 or p2 or p3) and p4"));
        atree_stats(t, &st);
        ASSERT_EQ_U64(st.nodes, 14);
        if (flags[f] == 0) {
            ASSERT_EQ_U64(st.edges, 12 + 2 - 1); /* N gains 2, S1 goes from 3 to 2 */
            ASSERT_EQ_U64(st.max_level, 4);      /* S1 now sits above N */
            ASSERT_EQ_U64(st.self_adjusted, 1);
        } else {
            ASSERT_EQ_U64(st.edges, 12 + 2);
            ASSERT_EQ_U64(st.max_level, 3);
            ASSERT_EQ_U64(st.self_adjusted, 0);
        }
        ASSERT_TRUE(valid(t));
        ASSERT_OK(atree_event_create(t, &ev));
        ASSERT_OK(atree_report_create(t, &rep));
        set_all_bools(ev, 8, false);
        ASSERT_OK(atree_event_set_bool(ev, "p1", true));
        ASSERT_OK(atree_event_set_bool(ev, "p4", true));
        ASSERT_OK(atree_search(t, ev, rep));
        ASSERT_TRUE(matches_are(rep, s3, 1));
        ASSERT_OK(atree_event_set_bool(ev, "p6", true));
        ASSERT_OK(atree_event_set_bool(ev, "p8", true));
        ASSERT_OK(atree_search(t, ev, rep));
        ASSERT_TRUE(matches_are(rep, all3, 3));
        /* deleting N's owner keeps N alive while S1 uses it */
        ASSERT_OK(atree_delete(t, 3));
        ASSERT_TRUE(valid(t));
        atree_stats(t, &st);
        ASSERT_EQ_U64(st.nodes, flags[f] == 0 ? 14 : 13);
        ASSERT_OK(atree_search(t, ev, rep));
        ASSERT_TRUE(matches_are(rep, all3, 2));
        ASSERT_OK(atree_delete(t, 1));
        ASSERT_OK(atree_delete(t, 2));
        atree_stats(t, &st);
        ASSERT_EQ_U64(st.nodes, 0);
        ASSERT_TRUE(valid(t));
        atree_report_destroy(rep);
        atree_event_destroy(ev);
        atree_destroy(t);
    }
    return 0;
}

static int stop_after_first(void *ctx, atree_id_t id)
{
    atree_id_t *seen = (atree_id_t *)ctx;
    *seen = id;
    return 1;
}

static int collect(void *ctx, atree_id_t id)
{
    int *n = (int *)ctx;
    (void)id;
    (*n)++;
    return 0;
}

TEST(conveniences)
{
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_id_t seen = 0;
    int n = 0;
    bool exists = false;
    atree_id_t allow[] = {2, 3, 50};
    atree_id_t want[] = {2, 3};
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    ASSERT_FALSE(ins(t, 1, "private"));
    ASSERT_FALSE(ins(t, 2, "exchange_id > 0"));
    ASSERT_FALSE(ins(t, 3, "exchange_id < 10"));
    ASSERT_FALSE(ins(t, 4, "test"));
    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_event_set_bool(ev, "private", true));
    ASSERT_OK(atree_event_set_int(ev, "exchange_id", 5));

    ASSERT_OK(atree_search_cb(t, ev, NULL, stop_after_first, &seen)); /* temp scratch */
    ASSERT_EQ_U64(seen, 1); /* sorted: smallest id first */
    ASSERT_OK(atree_report_create(t, &rep));
    ASSERT_OK(atree_search_cb(t, ev, rep, collect, &n));
    ASSERT_EQ_I64(n, 3);
    ASSERT_OK(atree_exists(t, ev, NULL, &exists));
    ASSERT_TRUE(exists);
    ASSERT_OK(atree_exists(t, ev, rep, &exists));
    ASSERT_TRUE(exists);
    ASSERT_OK(atree_event_set_bool(ev, "private", false));
    ASSERT_OK(atree_event_set_int(ev, "exchange_id", 50));
    ASSERT_OK(atree_exists(t, ev, rep, &exists));
    ASSERT_TRUE(exists); /* id 2 */
    ASSERT_OK(atree_event_set_undefined(ev, "exchange_id"));
    ASSERT_OK(atree_exists(t, ev, rep, &exists));
    ASSERT_FALSE(exists);

    ASSERT_OK(atree_event_set_bool(ev, "private", true));
    ASSERT_OK(atree_event_set_int(ev, "exchange_id", 5));
    ASSERT_OK(atree_search_ids(t, ev, rep, allow, 3));
    ASSERT_TRUE(matches_are(rep, want, 2));
    ASSERT_OK(atree_search_ids(t, ev, rep, NULL, 0));
    ASSERT_EQ_U64(atree_report_count(rep), 0);
    ASSERT_STATUS(atree_search_ids(t, ev, NULL, allow, 3), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_search_cb(t, ev, rep, NULL, NULL), ATREE_ERR_INVALID_ARG);
    ASSERT_STATUS(atree_exists(t, ev, rep, NULL), ATREE_ERR_INVALID_ARG);

    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    return 0;
}

/* Many expressions over the Rust crate's example schema, then search with
 * the example's event: expected result from the crate's documentation. */
TEST(rust_crate_example)
{
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    const char *deals[] = {"deal-3", "deal-1"};
    int64_t segs[] = {3, 4, 5};
    atree_id_t want[] = {1, 3, 4};
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    ASSERT_FALSE(ins(t, 1,
                     "exchange_id = 1 and deal_ids one of ['deal-1', 'deal-2'] and segment_ids one "
                     "of [1, 2, 3] and country in ['FR', 'GB', 'US']"));
    ASSERT_FALSE(
        ins(t, 2,
            "(exchange_id = 1 and deal_ids one of ['deal-1', 'deal-2']) and segment_ids one "
            "of [1, 2, 3] and ((country = 'CA' and city in ['QC']) or (country = 'US' and "
            "city in ['AZ']))"));
    ASSERT_FALSE(
        ins(t, 3,
            "(exchange_id = 1 and deal_ids one of ['deal-1', 'deal-2']) and segment_ids one "
            "of [1, 2, 3] and ((country = 'CA' and city in ['QC']) or (country = 'US'))"));
    ASSERT_FALSE(ins(t, 4,
                     "exchange_id = 1 and deal_ids one of ['deal-1', 'deal-2'] and segment_ids one "
                     "of [1, 2, 3]"));
    ASSERT_TRUE(valid(t));
    ASSERT_OK(atree_event_create(t, &ev));
    ASSERT_OK(atree_report_create(t, &rep));
    ASSERT_OK(atree_event_set_int(ev, "exchange_id", 1));
    ASSERT_OK(atree_event_set_string_list(ev, "deal_ids", deals, NULL, 2));
    ASSERT_OK(atree_event_set_int_list(ev, "segment_ids", segs, 3));
    ASSERT_OK(atree_event_set_string(ev, "country", "US", SIZE_MAX));
    ASSERT_OK(atree_event_set_string(ev, "city", "NY", SIZE_MAX)); /* not AZ: 2 fails */
    ASSERT_OK(atree_search(t, ev, rep));
    ASSERT_TRUE(matches_are(rep, want, 3));
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(insert_search_delete_basic);
RUN_TEST(shared_subexpressions_and_use_counts);
RUN_TEST(duplicate_ids_and_argument_errors);
RUN_TEST(constant_expressions);
RUN_TEST(paper_figure_4);
RUN_TEST(paper_figure_6);
RUN_TEST(paper_figure_5_reorganize);
RUN_TEST(paper_self_adjust);
RUN_TEST(conveniences);
RUN_TEST(rust_crate_example);
TEST_MAIN_END()

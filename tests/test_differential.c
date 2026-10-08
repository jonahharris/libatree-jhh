/*
 * Differential test: the tree's search results must equal brute-force
 * evaluation of every live expression with the reference evaluator, for
 * random expressions and random events, under every optimization flag
 * combination, with deletes and re-inserts interleaved and the structure
 * validated after every mutation.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>

#include "test.h"
#include "test_alloc.h"

#include "gen.h"

#define NEXPR 160
#define NEVENTS 60
#define NCHURN 3

struct live {
    atree_expr_t *expr; /* NULL when not inserted */
    atree_id_t id;
};

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int valid(const atree_t *t)
{
    char msg[256];
    if (atree_validate(t, msg, sizeof msg) != ATREE_OK) {
        fprintf(stderr, "validate: %s\n", msg);
        return 0;
    }
    return 1;
}

/* One search compared against brute force. Returns 1 on mismatch. */
static int compare_once(struct gen *g, struct live *live, size_t n, atree_event_t *ev,
                        atree_report_t *rep, uint64_t *scratch, unsigned flags, int round)
{
    size_t i;
    size_t m = 0;
    size_t k = 0;
    const atree_id_t *got;
    if (atree_search(g->tree, ev, rep) != ATREE_OK) {
        fprintf(stderr, "search failed\n");
        return 1;
    }
    for (i = 0; i < n; i++) {
        if (live[i].expr != NULL && atree_expr_eval(live[i].expr, ev) == ATREE_TRUE) {
            scratch[m++] = live[i].id;
        }
    }
    qsort(scratch, m, sizeof *scratch, cmp_u64);
    got = atree_report_matches(rep);
    k = atree_report_count(rep);
    if (k != m || (m > 0 && memcmp(got, scratch, m * sizeof *scratch) != 0)) {
        fprintf(stderr, "flags=%u round=%d: tree returned %lu ids, brute force %lu\n", flags, round,
                (unsigned long)k, (unsigned long)m);
        for (i = 0; i < n; i++) {
            if (live[i].expr == NULL) {
                continue;
            }
            {
                int in_tree = 0;
                size_t j;
                int in_brute = atree_expr_eval(live[i].expr, ev) == ATREE_TRUE;
                for (j = 0; j < k; j++) {
                    if (got[j] == live[i].id) {
                        in_tree = 1;
                    }
                }
                if (in_tree != in_brute) {
                    fprintf(stderr, "  id %llu tree=%d brute=%d: ", (unsigned long long)live[i].id,
                            in_tree, in_brute);
                    gen_dump_expr("", live[i].expr);
                }
            }
        }
        return 1;
    }
    return 0;
}

static int run_with_flags(unsigned flags, uint64_t seed)
{
    struct test_alloc ta;
    atree_config_t cfg;
    struct gen g;
    struct live live[NEXPR + NCHURN * 50];
    size_t nlive = 0;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    uint64_t scratch[NEXPR + NCHURN * 50];
    atree_stats_t st;
    uint64_t nodes_after_first_fill;
    int failures = 0;
    size_t i;
    int round;
    int churn;
    atree_id_t next_id = 1;

    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    cfg.flags = flags;
    if (gen_init(&g, &cfg, seed, 6) != ATREE_OK) {
        return 1;
    }
    g.unknown_percent = 15;

    for (i = 0; i < NEXPR; i++) {
        atree_error_t err;
        live[nlive].expr = gen_expr(&g);
        live[nlive].id = next_id++;
        if (live[nlive].expr == NULL) {
            fprintf(stderr, "expression generation failed (out of memory)\n");
            return 1;
        }
        if (atree_insert_expr(g.tree, live[nlive].id, live[nlive].expr, &err) != ATREE_OK) {
            fprintf(stderr, "insert failed: %s\n", err.message);
            return 1;
        }
        nlive++;
    }
    if (!valid(g.tree)) {
        return 1;
    }
    atree_stats(g.tree, &st);
    nodes_after_first_fill = st.nodes;
    if (st.subscriptions != NEXPR) {
        return 1;
    }

    if (atree_event_create(g.tree, &ev) != ATREE_OK ||
        atree_report_create(g.tree, &rep) != ATREE_OK) {
        return 1;
    }
    for (round = 0; round < NEVENTS && failures == 0; round++) {
        if (gen_event(&g, ev) != ATREE_OK) {
            return 1;
        }
        failures += compare_once(&g, live, nlive, ev, rep, scratch, flags, round);
    }

    /* Churn: delete some, insert new, compare again, validate each step. */
    for (churn = 0; churn < NCHURN && failures == 0; churn++) {
        int d;
        for (d = 0; d < 50; d++) {
            size_t pick = gen_below(&g.rng, (uint32_t)nlive);
            if (live[pick].expr != NULL) {
                if (atree_delete(g.tree, live[pick].id) != ATREE_OK) {
                    fprintf(stderr, "delete failed\n");
                    return 1;
                }
                atree_expr_free(live[pick].expr);
                live[pick].expr = NULL;
            }
        }
        if (!valid(g.tree)) {
            return 1;
        }
        for (d = 0; d < 50; d++) {
            live[nlive].expr = gen_expr(&g);
            live[nlive].id = next_id++;
            if (live[nlive].expr == NULL ||
                atree_insert_expr(g.tree, live[nlive].id, live[nlive].expr, NULL) != ATREE_OK) {
                return 1;
            }
            nlive++;
        }
        if (!valid(g.tree)) {
            return 1;
        }
        for (round = 0; round < NEVENTS / 2 && failures == 0; round++) {
            if (gen_event(&g, ev) != ATREE_OK) {
                return 1;
            }
            failures += compare_once(&g, live, nlive, ev, rep, scratch, flags, 1000 + round);
        }
    }

    /* Delete everything: the DAG must be empty, then refill identically. */
    for (i = 0; i < nlive; i++) {
        if (live[i].expr != NULL) {
            if (atree_delete(g.tree, live[i].id) != ATREE_OK) {
                return 1;
            }
        }
    }
    atree_stats(g.tree, &st);
    if (st.nodes != 0 || st.leaves != 0 || st.edges != 0 || st.subscriptions != 0 ||
        st.max_level != 0 || !valid(g.tree)) {
        fprintf(stderr, "not empty after deleting everything\n");
        return 1;
    }
    {
        atree_report_stats_t rs;
        if (gen_event(&g, ev) != ATREE_OK || atree_search(g.tree, ev, rep) != ATREE_OK) {
            return 1;
        }
        atree_report_stats(rep, &rs);
        if (rs.matches != 0 || atree_report_count(rep) != 0) {
            return 1;
        }
    }
    for (i = 0; i < NEXPR; i++) {
        if (live[i].expr == NULL) {
            continue;
        }
        if (atree_insert_expr(g.tree, live[i].id, live[i].expr, NULL) != ATREE_OK) {
            return 1;
        }
    }
    /* Same expressions (minus the churned ones) can only yield fewer nodes. */
    atree_stats(g.tree, &st);
    if (st.nodes > nodes_after_first_fill || !valid(g.tree)) {
        fprintf(stderr, "refill produced more nodes than the first fill\n");
        return 1;
    }

    for (i = 0; i < nlive; i++) {
        atree_expr_free(live[i].expr);
    }
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    gen_free(&g);
    if (!test_alloc_clean(&ta)) {
        fprintf(stderr, "allocator not clean: live=%lu mismatches=%lu\n", (unsigned long)ta.live,
                (unsigned long)ta.size_mismatches);
        return 1;
    }
    return failures;
}

TEST(differential_all_flag_combinations)
{
    uint64_t seed = gen_seed_from_env(20260406);
    unsigned combos[16];
    size_t c;
    for (c = 0; c < 16; c++) {
        combos[c] = (unsigned)c; /* every subset of the four ATREE_FLAG_NO_* bits */
    }
    size_t i;
    int failures = 0;
    for (i = 0; i < sizeof combos / sizeof combos[0]; i++) {
        int r = run_with_flags(combos[i], seed + i);
        if (r != 0) {
            fprintf(stderr, "flags %u failed (seed %llu)\n", combos[i], (unsigned long long)seed);
            failures++;
        }
    }
    ASSERT_EQ_I64(failures, 0);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(differential_all_flag_combinations);
TEST_MAIN_END()

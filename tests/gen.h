/*
 * Deterministic random generator for property tests: a fixed attribute
 * schema, a small string vocabulary, random expressions through the public
 * builder API, and random events. Small value domains make collisions
 * (equalities, overlaps) frequent so every operator is exercised in both
 * outcomes. White-box: interns the vocabulary into the tree so the reference
 * evaluator can resolve string literals before anything is inserted.
 *
 * Seed from ATREE_TEST_SEED when set so failures can be replayed.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_TEST_GEN_H
#define ATREE_TEST_GEN_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/atree_internal.h"

#include "atree.h"

/* ---- PRNG (splitmix64) -------------------------------------------------- */

struct gen_rng {
    uint64_t state;
};

static uint64_t gen_next(struct gen_rng *r)
{
    uint64_t z = (r->state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static uint32_t gen_below(struct gen_rng *r, uint32_t n)
{
    return n == 0 ? 0 : (uint32_t)(gen_next(r) % n);
}

static int gen_chance(struct gen_rng *r, uint32_t percent)
{
    return gen_below(r, 100) < percent;
}

static uint64_t gen_seed_from_env(uint64_t fallback)
{
    const char *s = getenv("ATREE_TEST_SEED");
    if (s != NULL && *s != '\0') {
        return strtoull(s, NULL, 10);
    }
    return fallback;
}

/* ---- schema ------------------------------------------------------------- */

enum { GEN_B0 = 0, GEN_B1, GEN_I0, GEN_I1, GEN_F0, GEN_S0, GEN_S1, GEN_IL0, GEN_SL0, GEN_NATTRS };

static const atree_attr_def_t GEN_DEFS[GEN_NATTRS] = {
    {"b0", ATREE_TYPE_BOOL},   {"b1", ATREE_TYPE_BOOL},      {"i0", ATREE_TYPE_INT},
    {"i1", ATREE_TYPE_INT},    {"f0", ATREE_TYPE_FLOAT},     {"s0", ATREE_TYPE_STRING},
    {"s1", ATREE_TYPE_STRING}, {"il0", ATREE_TYPE_INT_LIST}, {"sl0", ATREE_TYPE_STRING_LIST},
};

#define GEN_VOCAB_SIZE 6
static const char *const GEN_VOCAB[GEN_VOCAB_SIZE] = {"alpha", "beta",    "gamma",
                                                      "delta", "epsilon", "zeta"};
/* Never interned: exercises unknown-string handling. */
static const char *const GEN_UNKNOWN[2] = {"unknown-1", "unknown-2"};

#define GEN_INT_DOMAIN 6  /* integers 0..5 */
#define GEN_FLOAT_STEPS 5 /* floats 0.0, 0.5, ..., 2.0 */
#define GEN_MAX_LIST 4

struct gen {
    struct gen_rng rng;
    atree_t *tree;
    uint32_t max_depth;
    uint32_t unknown_percent; /* chance that a string literal/value is unknown */
};

/* Creates the schema tree (cfg may be NULL) and interns the vocabulary. */
static atree_status_t gen_init(struct gen *g, const atree_config_t *cfg, uint64_t seed,
                               uint32_t max_depth)
{
    atree_status_t st;
    uint32_t i;
    g->rng.state = seed;
    g->tree = NULL;
    g->max_depth = max_depth;
    g->unknown_percent = 10;
    st = atree_create(cfg, GEN_DEFS, GEN_NATTRS, &g->tree);
    if (st != ATREE_OK) {
        return st;
    }
    for (i = 0; i < GEN_VOCAB_SIZE; i++) {
        uint32_t id;
        st = atree__strtab_intern(&g->tree->mem, &g->tree->strings, GEN_VOCAB[i],
                                  strlen(GEN_VOCAB[i]), &id);
        if (st != ATREE_OK) {
            atree_destroy(g->tree);
            g->tree = NULL;
            return st;
        }
    }
    return ATREE_OK;
}

static void gen_free(struct gen *g)
{
    atree_destroy(g->tree);
    g->tree = NULL;
}

/* ---- random values ------------------------------------------------------ */

static int64_t gen_int(struct gen *g)
{
    return (int64_t)gen_below(&g->rng, GEN_INT_DOMAIN);
}

static double gen_float(struct gen *g)
{
    return 0.5 * (double)gen_below(&g->rng, GEN_FLOAT_STEPS);
}

static const char *gen_string(struct gen *g)
{
    if (gen_chance(&g->rng, g->unknown_percent)) {
        return GEN_UNKNOWN[gen_below(&g->rng, 2)];
    }
    return GEN_VOCAB[gen_below(&g->rng, GEN_VOCAB_SIZE)];
}

static uint32_t gen_ints(struct gen *g, int64_t *out, uint32_t min_n)
{
    uint32_t n = min_n + gen_below(&g->rng, GEN_MAX_LIST + 1 - min_n);
    uint32_t i;
    for (i = 0; i < n; i++) {
        out[i] = gen_int(g);
    }
    return n;
}

static uint32_t gen_strings(struct gen *g, const char **out, uint32_t min_n)
{
    uint32_t n = min_n + gen_below(&g->rng, GEN_MAX_LIST + 1 - min_n);
    uint32_t i;
    for (i = 0; i < n; i++) {
        out[i] = gen_string(g);
    }
    return n;
}

/* ---- random expressions ------------------------------------------------- */

static atree_expr_t *gen_leaf(struct gen *g)
{
    const atree_t *t = g->tree;
    atree_op_t ops[6] = {ATREE_OP_LT, ATREE_OP_LE, ATREE_OP_GT,
                         ATREE_OP_GE, ATREE_OP_EQ, ATREE_OP_NE};
    atree_list_op_t lops[3] = {ATREE_LIST_ONE_OF, ATREE_LIST_NONE_OF, ATREE_LIST_ALL_OF};
    int64_t ints[GEN_MAX_LIST];
    const char *strs[GEN_MAX_LIST];
    uint32_t n;
    uint32_t attr = gen_below(&g->rng, GEN_NATTRS);
    const char *name = GEN_DEFS[attr].name;

    if (gen_chance(&g->rng, 5)) { /* null checks on any attribute */
        if (attr >= GEN_IL0) {
            return atree_expr_null(t, name,
                                   gen_chance(&g->rng, 50) ? ATREE_IS_EMPTY : ATREE_IS_NOT_EMPTY);
        }
        return atree_expr_null(t, name,
                               gen_chance(&g->rng, 50) ? ATREE_IS_NULL : ATREE_IS_NOT_NULL);
    }
    switch (attr) {
    case GEN_B0:
    case GEN_B1:
        return atree_expr_var(t, name);
    case GEN_I0:
    case GEN_I1:
        if (gen_chance(&g->rng, 40)) {
            n = gen_ints(g, ints, 1);
            return atree_expr_in_ints(t, name, gen_chance(&g->rng, 60), ints, n);
        }
        return atree_expr_cmp_int(t, name, ops[gen_below(&g->rng, 6)], gen_int(g));
    case GEN_F0:
        return atree_expr_cmp_float(t, name, ops[gen_below(&g->rng, 6)], gen_float(g));
    case GEN_S0:
    case GEN_S1:
        if (gen_chance(&g->rng, 40)) {
            n = gen_strings(g, strs, 1);
            return atree_expr_in_strings(t, name, gen_chance(&g->rng, 60), strs, NULL, n);
        }
        return atree_expr_eq_string(t, name, gen_chance(&g->rng, 60), gen_string(g), SIZE_MAX);
    case GEN_IL0:
        n = gen_ints(g, ints, 1);
        return atree_expr_list_ints(t, name, lops[gen_below(&g->rng, 3)], ints, n);
    case GEN_SL0:
    default:
        n = gen_strings(g, strs, 1);
        return atree_expr_list_strings(t, name, lops[gen_below(&g->rng, 3)], strs, NULL, n);
    }
}

static atree_expr_t *gen_expr_depth(struct gen *g, uint32_t depth)
{
    atree_expr_t *kids[4];
    uint32_t n;
    uint32_t i;
    uint32_t roll;

    if (depth + 1 >= g->max_depth) {
        return gen_leaf(g);
    }
    roll = gen_below(&g->rng, 100);
    if (roll < 2) {
        return roll == 0 ? atree_expr_true(g->tree) : atree_expr_false(g->tree);
    }
    if (roll < 32) {
        return gen_leaf(g);
    }
    if (roll < 42) {
        return atree_expr_not(gen_expr_depth(g, depth + 1));
    }
    if (roll < 47) {
        return atree_expr_xor(gen_expr_depth(g, depth + 1), gen_expr_depth(g, depth + 1));
    }
    if (roll < 52) {
        return atree_expr_xnor(gen_expr_depth(g, depth + 1), gen_expr_depth(g, depth + 1));
    }
    n = 2 + gen_below(&g->rng, 3);
    for (i = 0; i < n; i++) {
        kids[i] = gen_expr_depth(g, depth + 1);
    }
    return roll < 76 ? atree_expr_and(kids, n) : atree_expr_or(kids, n);
}

static atree_expr_t *gen_expr(struct gen *g)
{
    return gen_expr_depth(g, 1);
}

/* ---- random events ------------------------------------------------------ */

static atree_status_t gen_event(struct gen *g, atree_event_t *ev)
{
    atree_status_t st = ATREE_OK;
    int64_t ints[GEN_MAX_LIST];
    const char *strs[GEN_MAX_LIST];
    uint32_t n;
    uint32_t a;

    atree_event_clear(ev);
    for (a = 0; a < GEN_NATTRS && st == ATREE_OK; a++) {
        if (gen_chance(&g->rng, 20)) {
            continue; /* undefined */
        }
        switch (a) {
        case GEN_B0:
        case GEN_B1:
            st = atree_event_set_bool_id(ev, a, gen_chance(&g->rng, 50));
            break;
        case GEN_I0:
        case GEN_I1:
            st = atree_event_set_int_id(ev, a, gen_int(g));
            break;
        case GEN_F0:
            st = atree_event_set_float_id(ev, a, gen_chance(&g->rng, 5) ? nan("") : gen_float(g));
            break;
        case GEN_S0:
        case GEN_S1:
            st = atree_event_set_string_id(ev, a, gen_string(g), SIZE_MAX);
            break;
        case GEN_IL0:
            n = gen_ints(g, ints, 0);
            st = atree_event_set_int_list_id(ev, a, ints, n);
            break;
        case GEN_SL0:
        default:
            n = gen_strings(g, strs, 0);
            st = atree_event_set_string_list_id(ev, a, strs, NULL, n);
            break;
        }
    }
    return st;
}

/* ---- printing helper for failure reports -------------------------------- */

static int gen_sink_write(void *ctx, const char *data, size_t len)
{
    FILE *f = (FILE *)ctx;
    return fwrite(data, 1, len, f) == len ? 0 : 1;
}

static void gen_dump_expr(const char *label, const atree_expr_t *e)
{
    fprintf(stderr, "%s: ", label);
    (void)atree_expr_print(e, gen_sink_write, stderr);
    fputc('\n', stderr);
}

#endif /* ATREE_TEST_GEN_H */

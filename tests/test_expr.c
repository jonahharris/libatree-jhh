/*
 * Tests for the expression builder, normalization (zero suppression filter),
 * the reference evaluator and the printer. White-box for normalization and
 * structural equality (../src/expr.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>

#include "../src/expr.h"

#include "test.h"
#include "test_alloc.h"

#include "gen.h"

/* ---- helpers ------------------------------------------------------------ */

struct sink {
    char buf[2048];
    size_t len;
};

static int sink_write(void *ctx, const char *data, size_t len)
{
    struct sink *s = (struct sink *)ctx;
    if (s->len + len >= sizeof s->buf) {
        return 1;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    s->buf[s->len] = '\0';
    return 0;
}

static const char *render(const atree_expr_t *e, struct sink *s)
{
    s->len = 0;
    s->buf[0] = '\0';
    if (atree_expr_print(e, sink_write, s) != ATREE_OK) {
        return "<print failed>";
    }
    return s->buf;
}

/* Normalizes and frees the input. */
static atree_expr_t *norm(atree_expr_t *e)
{
    atree_expr_t *out = NULL;
    atree_status_t st = atree__expr_normalize(e, 64, &out);
    atree_expr_free(e);
    return st == ATREE_OK ? out : NULL;
}

/* True when every node is PRED/AND/OR (constants only at the root), no
 * AND/OR child repeats its parent's kind, and children are strictly
 * increasing in the canonical order. */
static int is_normal_form(const atree_expr_t *e, int root)
{
    uint32_t i;
    if (e->kind == ATREE_EXPR_TRUE || e->kind == ATREE_EXPR_FALSE) {
        return root;
    }
    if (e->kind == ATREE_EXPR_PRED) {
        return 1;
    }
    if (e->kind != ATREE_EXPR_AND && e->kind != ATREE_EXPR_OR) {
        return 0;
    }
    if (e->nchildren < 2) {
        return 0;
    }
    for (i = 0; i < e->nchildren; i++) {
        if (e->children[i]->kind == e->kind) {
            return 0;
        }
        if (i > 0 && atree__expr_cmp(e->children[i - 1], e->children[i]) >= 0) {
            return 0;
        }
        if (!is_normal_form(e->children[i], 0)) {
            return 0;
        }
    }
    return 1;
}

static const atree_attr_def_t DEFS[] = {
    {"private", ATREE_TYPE_BOOL},         {"test", ATREE_TYPE_BOOL},
    {"exchange_id", ATREE_TYPE_INT},      {"price", ATREE_TYPE_FLOAT},
    {"country", ATREE_TYPE_STRING},       {"segment_ids", ATREE_TYPE_INT_LIST},
    {"deal_ids", ATREE_TYPE_STRING_LIST},
};
#define NDEFS (sizeof DEFS / sizeof DEFS[0])

#define AND2(a, b) and2((a), (b))
static atree_expr_t *and2(atree_expr_t *a, atree_expr_t *b)
{
    atree_expr_t *c[2];
    c[0] = a;
    c[1] = b;
    return atree_expr_and(c, 2);
}
static atree_expr_t *or2(atree_expr_t *a, atree_expr_t *b)
{
    atree_expr_t *c[2];
    c[0] = a;
    c[1] = b;
    return atree_expr_or(c, 2);
}

/* ---- builder ------------------------------------------------------------ */

TEST(builders_validate_and_print)
{
    struct test_alloc ta;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_expr_t *e;
    struct sink s;
    int64_t ints[] = {3, 1, 2, 1};
    const char *strs[] = {"US", "CA", "US"};
    size_t lens[] = {2, SIZE_MAX, 2};

    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(atree_create(&cfg, DEFS, NDEFS, &t));

    e = atree_expr_var(t, "private");
    ASSERT_NOT_NULL(e);
    ASSERT_EQ_STR(render(e, &s), "private");
    atree_expr_free(e);

    e = atree_expr_cmp_int(t, "exchange_id", ATREE_OP_GE, 5);
    ASSERT_EQ_STR(render(e, &s), "exchange_id >= 5");
    atree_expr_free(e);

    e = atree_expr_cmp_float(t, "price", ATREE_OP_LT, 2.5);
    ASSERT_EQ_STR(render(e, &s), "price < 2.5");
    atree_expr_free(e);

    e = atree_expr_eq_string(t, "country", false, "CA", 2);
    ASSERT_EQ_STR(render(e, &s), "country <> \"CA\"");
    atree_expr_free(e);

    e = atree_expr_in_ints(t, "exchange_id", true, ints, 4);
    ASSERT_EQ_STR(render(e, &s), "exchange_id in [1, 2, 3]"); /* sorted, unique */
    atree_expr_free(e);

    e = atree_expr_in_strings(t, "country", false, strs, lens, 3);
    ASSERT_EQ_STR(render(e, &s), "country not in [\"CA\", \"US\"]"); /* sorted, unique */
    atree_expr_free(e);

    e = atree_expr_list_ints(t, "segment_ids", ATREE_LIST_ALL_OF, ints, 4);
    ASSERT_EQ_STR(render(e, &s), "segment_ids all of [1, 2, 3]");
    atree_expr_free(e);

    e = atree_expr_list_strings(t, "deal_ids", ATREE_LIST_NONE_OF, strs, NULL, 3);
    ASSERT_EQ_STR(render(e, &s), "deal_ids none of [\"CA\", \"US\"]");
    atree_expr_free(e);

    e = atree_expr_null(t, "segment_ids", ATREE_IS_NOT_EMPTY);
    ASSERT_EQ_STR(render(e, &s), "segment_ids is not empty");
    atree_expr_free(e);

    e = atree_expr_true(t);
    ASSERT_EQ_STR(render(e, &s), "true");
    atree_expr_free(e);

    /* connectives, precedence parentheses, not, xor */
    e = or2(
        AND2(atree_expr_var(t, "private"), atree_expr_cmp_int(t, "exchange_id", ATREE_OP_EQ, 1)),
        atree_expr_not(or2(atree_expr_var(t, "test"), atree_expr_false(t))));
    ASSERT_NOT_NULL(e);
    ASSERT_EQ_STR(render(e, &s), "(private and exchange_id = 1) or not (test or false)");
    ASSERT_EQ_U64(e->depth, 4); /* or > not > or > leaves */
    atree_expr_free(e);

    e = atree_expr_xnor(atree_expr_xor(atree_expr_var(t, "private"), atree_expr_var(t, "test")),
                        atree_expr_not(atree_expr_var(t, "private")));
    ASSERT_EQ_STR(render(e, &s), "(private xor test) xnor not private");
    atree_expr_free(e);

    /* invalid leaves */
    ASSERT_NULL(atree_expr_var(t, "nope"));
    ASSERT_NULL(atree_expr_var(t, "exchange_id"));      /* not bool */
    e = atree_expr_cmp_int(t, "price", ATREE_OP_LT, 1); /* int literal promotes */
    ASSERT_NOT_NULL(e);
    ASSERT_EQ_STR(render(e, &s), "price < 1.0");
    atree_expr_free(e);
    ASSERT_NULL(atree_expr_cmp_float(t, "exchange_id", ATREE_OP_LT, 1.0)); /* no demotion */
    ASSERT_NULL(atree_expr_cmp_float(t, "price", ATREE_OP_LT, nan("")));
    ASSERT_NULL(atree_expr_eq_string(t, "country", true, NULL, 0));
    ASSERT_NULL(atree_expr_in_ints(t, "exchange_id", true, ints, 0)); /* empty list */
    ASSERT_NULL(atree_expr_in_ints(t, "segment_ids", true, ints, 2)); /* list attr */
    ASSERT_NULL(atree_expr_list_ints(t, "exchange_id", ATREE_LIST_ONE_OF, ints, 2));
    ASSERT_NULL(atree_expr_null(t, "segment_ids", ATREE_IS_NULL));
    ASSERT_NULL(atree_expr_null(t, "country", ATREE_IS_EMPTY));
    ASSERT_NULL(atree_expr_var(NULL, "private"));
    ASSERT_NULL(atree_expr_true(NULL));

    /* connectives free their inputs on failure */
    {
        atree_expr_t *c[2];
        c[0] = atree_expr_var(t, "private");
        c[1] = NULL;
        ASSERT_NULL(atree_expr_and(c, 2));
        ASSERT_NULL(atree_expr_or(NULL, 0));
        ASSERT_NULL(atree_expr_not(NULL));
        ASSERT_NULL(atree_expr_xor(atree_expr_var(t, "private"), NULL));
        c[0] = atree_expr_var(t, "private");
        ASSERT_NULL(atree_expr_and(c, 0));
        atree_expr_free(c[0]);
    }
    atree_destroy(t);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

TEST(mixed_trees_are_rejected)
{
    atree_t *t1 = NULL;
    atree_t *t2 = NULL;
    atree_expr_t *c[2];
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t1));
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t2));
    c[0] = atree_expr_var(t1, "private");
    c[1] = atree_expr_var(t2, "private");
    ASSERT_NOT_NULL(c[0]);
    ASSERT_NOT_NULL(c[1]);
    ASSERT_NULL(atree_expr_and(c, 2)); /* both freed */
    atree_destroy(t1);
    atree_destroy(t2);
    return 0;
}

/* ---- normalization identities (ports of the Rust crate's ast tests) ------ */

TEST(normalize_identities)
{
    atree_t *t = NULL;
    atree_expr_t *a;
    atree_expr_t *b;
    struct sink s;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
#define P atree_expr_var(t, "private")
#define Q atree_expr_var(t, "test")
#define X atree_expr_cmp_int(t, "exchange_id", ATREE_OP_LT, 3)
#define NP atree_expr_not(P)
#define NQ atree_expr_not(Q)
#define NX atree_expr_not(X)

    /* not not p == p */
    a = norm(atree_expr_not(atree_expr_not(P)));
    ASSERT_NOT_NULL(a);
    ASSERT_EQ_STR(render(a, &s), "private");
    atree_expr_free(a);

    /* not p becomes a negated leaf; not (x < 3) flips the operator */
    a = norm(NP);
    ASSERT_EQ_STR(render(a, &s), "not private");
    atree_expr_free(a);
    a = norm(NX);
    ASSERT_EQ_STR(render(a, &s), "exchange_id >= 3");
    atree_expr_free(a);

    /* De Morgan: not (p or q) == not p and not q */
    a = norm(atree_expr_not(or2(P, Q)));
    b = norm(AND2(NP, NQ));
    ASSERT_TRUE(atree__expr_equal(a, b));
    ASSERT_EQ_U64(a->kind, ATREE_EXPR_AND);
    atree_expr_free(a);
    atree_expr_free(b);

    /* not (p and q) == not p or not q */
    a = norm(atree_expr_not(AND2(P, Q)));
    b = norm(or2(NP, NQ));
    ASSERT_TRUE(atree__expr_equal(a, b));
    ASSERT_EQ_U64(a->kind, ATREE_EXPR_OR);
    atree_expr_free(a);
    atree_expr_free(b);

    /* Rust: not (not (p or p) and ((p or p) and (p or p))) == p or (not p and not p)
     * which further collapses: p or p -> p, not p and not p -> not p. */
    a = norm(atree_expr_not(AND2(atree_expr_not(or2(P, P)), AND2(or2(P, P), or2(P, P)))));
    b = norm(or2(P, NP));
    ASSERT_TRUE(atree__expr_equal(a, b));
    ASSERT_EQ_U64(a->nchildren, 2);
    atree_expr_free(a);
    atree_expr_free(b);

    /* nested negation below the top level */
    a = norm(AND2(atree_expr_not(AND2(P, Q)), X));
    b = norm(AND2(or2(NP, NQ), X));
    ASSERT_TRUE(atree__expr_equal(a, b));
    atree_expr_free(a);
    atree_expr_free(b);

    /* commutativity and associativity: one canonical form */
    a = norm(or2(P, Q));
    b = norm(or2(Q, P));
    ASSERT_TRUE(atree__expr_equal(a, b));
    atree_expr_free(b);
    b = norm(AND2(P, Q));
    ASSERT_FALSE(atree__expr_equal(a, b)); /* operator matters */
    atree_expr_free(a);
    atree_expr_free(b);
    a = norm(AND2(AND2(P, Q), X));
    b = norm(AND2(P, AND2(Q, X)));
    ASSERT_TRUE(atree__expr_equal(a, b));
    ASSERT_EQ_U64(a->nchildren, 3); /* flattened */
    atree_expr_free(b);
    {
        atree_expr_t *c[3];
        c[0] = X;
        c[1] = P;
        c[2] = Q;
        b = norm(atree_expr_and(c, 3));
        ASSERT_TRUE(atree__expr_equal(a, b));
        atree_expr_free(b);
    }
    atree_expr_free(a);

    /* idempotence and single-child collapse */
    a = norm(AND2(P, P));
    ASSERT_EQ_STR(render(a, &s), "private");
    atree_expr_free(a);
    a = norm(or2(AND2(P, Q), AND2(Q, P)));
    ASSERT_EQ_U64(a->kind, ATREE_EXPR_AND);
    ASSERT_EQ_U64(a->nchildren, 2);
    atree_expr_free(a);

    /* constant folding */
    a = norm(AND2(P, atree_expr_true(t)));
    ASSERT_EQ_STR(render(a, &s), "private");
    atree_expr_free(a);
    a = norm(AND2(P, atree_expr_false(t)));
    ASSERT_EQ_STR(render(a, &s), "false");
    atree_expr_free(a);
    a = norm(or2(P, atree_expr_false(t)));
    ASSERT_EQ_STR(render(a, &s), "private");
    atree_expr_free(a);
    a = norm(or2(P, atree_expr_true(t)));
    ASSERT_EQ_STR(render(a, &s), "true");
    atree_expr_free(a);
    a = norm(atree_expr_not(atree_expr_true(t)));
    ASSERT_EQ_STR(render(a, &s), "false");
    atree_expr_free(a);
    a = norm(atree_expr_not(AND2(P, atree_expr_false(t)))); /* not false */
    ASSERT_EQ_STR(render(a, &s), "true");
    atree_expr_free(a);
    a = norm(or2(atree_expr_false(t), atree_expr_false(t)));
    ASSERT_EQ_STR(render(a, &s), "false");
    atree_expr_free(a);

    /* xor / xnor expansion */
    a = norm(atree_expr_xor(P, Q));
    b = norm(or2(AND2(P, NQ), AND2(NP, Q)));
    ASSERT_TRUE(atree__expr_equal(a, b));
    ASSERT_EQ_U64(a->kind, ATREE_EXPR_OR);
    ASSERT_EQ_U64(a->nchildren, 2);
    atree_expr_free(a);
    atree_expr_free(b);
    a = norm(atree_expr_xnor(P, Q));
    b = norm(or2(AND2(P, Q), AND2(NP, NQ)));
    ASSERT_TRUE(atree__expr_equal(a, b));
    atree_expr_free(b);
    b = norm(atree_expr_not(atree_expr_xor(P, Q)));
    ASSERT_TRUE(atree__expr_equal(a, b)); /* not xor == xnor */
    atree_expr_free(a);
    atree_expr_free(b);
    a = norm(atree_expr_xor(P, P)); /* (p and not p) or (not p and p): not folded, but valid */
    ASSERT_TRUE(is_normal_form(a, 1));
    atree_expr_free(a);

    /* the result never contains NOT, XOR or XNOR */
    a = norm(atree_expr_not(atree_expr_xor(atree_expr_not(or2(P, X)), atree_expr_xnor(Q, X))));
    ASSERT_TRUE(is_normal_form(a, 1));
    atree_expr_free(a);

#undef P
#undef Q
#undef X
#undef NP
#undef NQ
#undef NX
    atree_destroy(t);
    return 0;
}

TEST(depth_limit)
{
    atree_t *t = NULL;
    atree_expr_t *e;
    atree_expr_t *out = NULL;
    int i;
    ASSERT_OK(atree_create(NULL, DEFS, NDEFS, &t));
    e = atree_expr_var(t, "private");
    for (i = 0; i < 10; i++) {
        e = atree_expr_not(e);
    }
    ASSERT_NOT_NULL(e);
    ASSERT_EQ_U64(e->depth, 11);
    ASSERT_STATUS(atree__expr_normalize(e, 10, &out), ATREE_ERR_TOO_DEEP);
    ASSERT_NULL(out);
    ASSERT_OK(atree__expr_normalize(e, 11, &out));
    ASSERT_NOT_NULL(out);
    ASSERT_EQ_U64(out->kind, ATREE_EXPR_PRED); /* ten negations cancel */
    atree_expr_free(out);
    atree_expr_free(e);
    atree_destroy(t);
    return 0;
}

/* ---- reference evaluation ----------------------------------------------- */

TEST(reference_evaluation_three_valued)
{
    struct gen g;
    atree_event_t *ev = NULL;
    atree_expr_t *e;
    atree_expr_t *tr;
    const char *known[] = {"alpha", "beta"};
    const char *mixed[] = {"alpha", "unknown-1"};
    const char *unknown[] = {"unknown-1"};
    ASSERT_OK(gen_init(&g, NULL, 1, 4));
    ASSERT_OK(atree_event_create(g.tree, &ev));
    ASSERT_OK(atree_event_set_bool(ev, "b0", true));
    ASSERT_OK(atree_event_set_int(ev, "i0", 3));
    ASSERT_OK(atree_event_set_string(ev, "s0", "alpha", SIZE_MAX));
    ASSERT_OK(atree_event_set_string(ev, "s1", "unknown-2", SIZE_MAX));
    ASSERT_OK(atree_event_set_string_list(ev, "sl0", known, NULL, 2));

#define U atree_expr_cmp_int(g.tree, "i1", ATREE_OP_EQ, 1)
#define T atree_expr_var(g.tree, "b0")
#define F atree_expr_cmp_int(g.tree, "i0", ATREE_OP_EQ, 0)

    e = U;
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_UNDEFINED);
    atree_expr_free(e);
    e = T;
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);
    e = F;
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_FALSE);
    atree_expr_free(e);

    /* Table 2 */
    e = and2(U, T);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_UNDEFINED);
    atree_expr_free(e);
    e = and2(U, F);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_FALSE);
    atree_expr_free(e);
    e = or2(U, T);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);
    e = or2(U, F);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_UNDEFINED);
    atree_expr_free(e);
    e = atree_expr_not(U);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_UNDEFINED);
    atree_expr_free(e);
    e = atree_expr_xor(U, T);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_UNDEFINED);
    atree_expr_free(e);
    e = atree_expr_xnor(U, F);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_UNDEFINED);
    atree_expr_free(e);
    e = atree_expr_xor(T, F);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);
    e = atree_expr_xnor(T, F);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_FALSE);
    atree_expr_free(e);
    e = atree_expr_true(g.tree);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);

    /* is null is the only predicate true on an undefined attribute */
    e = atree_expr_null(g.tree, "i1", ATREE_IS_NULL);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);
    e = atree_expr_null(g.tree, "i1", ATREE_IS_NOT_NULL);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_FALSE);
    atree_expr_free(e);

    /* unknown string literals: present in no event */
    e = atree_expr_eq_string(g.tree, "s0", true, "unknown-1", SIZE_MAX);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_FALSE);
    atree_expr_free(e);
    e = atree_expr_eq_string(g.tree, "s1", true, "unknown-2", SIZE_MAX); /* both unknown */
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_FALSE);
    atree_expr_free(e);
    e = atree_expr_eq_string(g.tree, "s1", false, "unknown-2", SIZE_MAX);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);
    e = atree_expr_in_strings(g.tree, "s0", true, mixed, NULL, 2);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE); /* alpha is known and present */
    atree_expr_free(e);
    e = atree_expr_in_strings(g.tree, "s0", true, unknown, NULL, 1);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_FALSE);
    atree_expr_free(e);
    e = atree_expr_in_strings(g.tree, "s0", false, unknown, NULL, 1);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);
    e = atree_expr_list_strings(g.tree, "sl0", ATREE_LIST_ALL_OF, mixed, NULL, 2);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_FALSE); /* unknown-1 cannot be present */
    atree_expr_free(e);
    e = atree_expr_list_strings(g.tree, "sl0", ATREE_LIST_ALL_OF, known, NULL, 2);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);
    e = atree_expr_list_strings(g.tree, "sl0", ATREE_LIST_NONE_OF, unknown, NULL, 1);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);
    e = atree_expr_list_strings(g.tree, "sl0", ATREE_LIST_ONE_OF, mixed, NULL, 2);
    ASSERT_EQ_U64(atree_expr_eval(e, ev), ATREE_TRUE);
    atree_expr_free(e);

    /* event from another tree */
    {
        atree_t *other = NULL;
        atree_event_t *oev = NULL;
        ASSERT_OK(atree_create(NULL, GEN_DEFS, GEN_NATTRS, &other));
        ASSERT_OK(atree_event_create(other, &oev));
        tr = T;
        ASSERT_EQ_U64(atree_expr_eval(tr, oev), ATREE_UNDEFINED);
        ASSERT_EQ_U64(atree_expr_eval(NULL, ev), ATREE_UNDEFINED);
        ASSERT_EQ_U64(atree_expr_eval(tr, NULL), ATREE_UNDEFINED);
        atree_expr_free(tr);
        atree_event_destroy(oev);
        atree_destroy(other);
    }
#undef U
#undef T
#undef F
    atree_event_destroy(ev);
    gen_free(&g);
    return 0;
}

/* ---- property: normalization preserves the three-valued result ---------- */

TEST(normalization_preserves_evaluation)
{
    struct gen g;
    atree_event_t *ev = NULL;
    uint64_t seed = gen_seed_from_env(20260106);
    int failures = 0;
    int rounds;
    int evs;
    int nexpr = 400;
    int nev = 40;

    ASSERT_OK(gen_init(&g, NULL, seed, 6));
    ASSERT_OK(atree_event_create(g.tree, &ev));
    for (rounds = 0; rounds < nexpr && failures == 0; rounds++) {
        atree_expr_t *e = gen_expr(&g);
        atree_expr_t *n = NULL;
        atree_expr_t *nn = NULL;
        ASSERT_NOT_NULL(e);
        ASSERT_OK(atree__expr_normalize(e, 64, &n));
        if (!is_normal_form(n, 1)) {
            gen_dump_expr("not in normal form", n);
            failures++;
        }
        /* idempotent */
        ASSERT_OK(atree__expr_normalize(n, 64, &nn));
        if (!atree__expr_equal(n, nn)) {
            gen_dump_expr("normalize not idempotent, first", n);
            gen_dump_expr("second", nn);
            failures++;
        }
        for (evs = 0; evs < nev && failures == 0; evs++) {
            atree_tri_t a;
            atree_tri_t b;
            ASSERT_OK(gen_event(&g, ev));
            a = atree_expr_eval(e, ev);
            b = atree_expr_eval(n, ev);
            if (a != b) {
                fprintf(stderr, "seed %llu round %d event %d: original=%d normalized=%d\n",
                        (unsigned long long)seed, rounds, evs, (int)a, (int)b);
                gen_dump_expr("original", e);
                gen_dump_expr("normalized", n);
                failures++;
            }
        }
        atree_expr_free(nn);
        atree_expr_free(n);
        atree_expr_free(e);
    }
    atree_event_destroy(ev);
    gen_free(&g);
    ASSERT_EQ_I64(failures, 0);
    return 0;
}

/* ---- resolving literals ------------------------------------------------- */

TEST(lookup_and_intern)
{
    struct gen g;
    atree_expr_t *e;
    struct atree__pred p;
    uint32_t dropped = 99;
    const char *strs[] = {"alpha", "unknown-1", "beta", "alpha"};
    atree_stats_t st;
    ASSERT_OK(gen_init(&g, NULL, 1, 4));

    e = atree_expr_in_strings(g.tree, "s0", true, strs, NULL, 4);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ_U64(e->nstrs, 3); /* alpha deduplicated */
    ASSERT_OK(atree__expr_pred_lookup(e, &g.tree->mem, &g.tree->strings, &p, &dropped));
    ASSERT_EQ_U64(dropped, 1);
    ASSERT_EQ_U64(p.operand.u.sl.len, 2);
    ASSERT_TRUE(p.operand.u.sl.data[0] < p.operand.u.sl.data[1]);
    atree__pred_free(&g.tree->mem, &p);
    atree_stats(g.tree, &st);
    ASSERT_EQ_U64(st.strings, GEN_VOCAB_SIZE); /* lookup interned nothing */

    ASSERT_OK(atree__expr_pred_intern(e, &g.tree->mem, &g.tree->strings, &p));
    ASSERT_EQ_U64(p.operand.u.sl.len, 3);
    atree__pred_free(&g.tree->mem, &p);
    atree_stats(g.tree, &st);
    ASSERT_EQ_U64(st.strings, GEN_VOCAB_SIZE + 1);
    ASSERT_OK(atree__expr_pred_lookup(e, &g.tree->mem, &g.tree->strings, &p, &dropped));
    ASSERT_EQ_U64(dropped, 0);
    ASSERT_EQ_U64(p.operand.u.sl.len, 3);
    atree__pred_free(&g.tree->mem, &p);
    atree_expr_free(e);

    e = atree_expr_eq_string(g.tree, "s0", true, "unknown-2", SIZE_MAX);
    ASSERT_OK(atree__expr_pred_lookup(e, &g.tree->mem, &g.tree->strings, &p, &dropped));
    ASSERT_EQ_U64(dropped, 1);
    ASSERT_EQ_U64(p.operand.u.s, ATREE_STR_UNKNOWN);
    atree__pred_free(&g.tree->mem, &p);
    atree_expr_free(e);

    e = atree_expr_var(g.tree, "b0");
    ASSERT_STATUS(
        atree__expr_pred_lookup(atree_expr_not(e), &g.tree->mem, &g.tree->strings, &p, &dropped),
        ATREE_ERR_INVALID_ARG);
    gen_free(&g);
    return 0;
}

/* ---- allocation failures ------------------------------------------------ */

TEST(allocation_failures_do_not_leak)
{
    struct test_alloc ta;
    atree_config_t cfg;
    struct gen g;
    size_t k;
    size_t build_attempts;
    int leaks = 0;

    test_alloc_init(&ta);
    atree_config_init(&cfg);
    cfg.allocator = &ta.a;
    ASSERT_OK(gen_init(&g, &cfg, 7, 6));
    {
        size_t base = ta.attempts;
        atree_expr_t *e = gen_expr(&g);
        atree_expr_t *n = NULL;
        ASSERT_NOT_NULL(e);
        ASSERT_OK(atree__expr_normalize(e, 64, &n));
        build_attempts = ta.attempts - base;
        atree_expr_free(n);
        atree_expr_free(e);
    }
    ASSERT_TRUE(build_attempts > 10);

    for (k = 1; k <= build_attempts; k++) {
        struct gen g2;
        size_t live_before;
        atree_expr_t *e;
        atree_expr_t *n = NULL;
        g2 = g;
        g2.rng.state = 7; /* same expression every time */
        live_before = ta.live;
        ta.fail_at = ta.attempts + k;
        e = gen_expr(&g2);
        if (e != NULL) {
            atree_status_t st = atree__expr_normalize(e, 64, &n);
            if (st != ATREE_OK) {
                ASSERT_STATUS(st, ATREE_ERR_NOMEM);
                ASSERT_NULL(n);
            }
            atree_expr_free(n);
            atree_expr_free(e);
        }
        ta.fail_at = 0;
        if (ta.live != live_before) {
            fprintf(stderr, "leak when failing allocation %lu\n", (unsigned long)k);
            leaks++;
        }
    }
    gen_free(&g);
    ASSERT_TRUE(test_alloc_clean(&ta));
    ASSERT_EQ_I64(leaks, 0);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(builders_validate_and_print);
RUN_TEST(mixed_trees_are_rejected);
RUN_TEST(normalize_identities);
RUN_TEST(depth_limit);
RUN_TEST(reference_evaluation_three_valued);
RUN_TEST(normalization_preserves_evaluation);
RUN_TEST(lookup_and_intern);
RUN_TEST(allocation_failures_do_not_leak);
TEST_MAIN_END()

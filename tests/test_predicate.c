/*
 * White-box tests for src/predicate.c: type rules, negation, three-valued
 * evaluation, hashing, cost, printing.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>

#include "../src/predicate.h"

#include "test.h"
#include "test_alloc.h"

/* Attribute ids in this table. */
enum { A_BOOL = 0, A_INT, A_FLOAT, A_STR, A_ILIST, A_SLIST, A_COUNT };

static const atree_attr_def_t DEFS[] = {
    {"flag", ATREE_TYPE_BOOL},         {"age", ATREE_TYPE_INT},
    {"price", ATREE_TYPE_FLOAT},       {"country", ATREE_TYPE_STRING},
    {"segments", ATREE_TYPE_INT_LIST}, {"deals", ATREE_TYPE_STRING_LIST},
};

struct fixture {
    struct atree__mem m;
    struct atree__attrs attrs;
    struct atree__strtab strings;
    uint32_t s_ca, s_us, s_fr;
};

static int fixture_init(struct fixture *f, const atree_allocator_t *a)
{
    atree__mem_init(&f->m, a);
    if (atree__attrs_init(&f->m, &f->attrs, DEFS, A_COUNT) != ATREE_OK) {
        return 1;
    }
    if (atree__strtab_init(&f->m, &f->strings) != ATREE_OK) {
        return 1;
    }
    if (atree__strtab_intern(&f->m, &f->strings, "CA", 2, &f->s_ca) != ATREE_OK ||
        atree__strtab_intern(&f->m, &f->strings, "US", 2, &f->s_us) != ATREE_OK ||
        atree__strtab_intern(&f->m, &f->strings, "FR", 2, &f->s_fr) != ATREE_OK) {
        return 1;
    }
    return 0;
}

static void fixture_free(struct fixture *f)
{
    atree__strtab_free(&f->m, &f->strings);
    atree__attrs_free(&f->m, &f->attrs);
}

static struct atree__pred mk(atree_attr_id_t attr, enum atree__pred_kind kind, atree_op_t op)
{
    struct atree__pred p;
    p.attr = attr;
    p.kind = (uint8_t)kind;
    p.op = (uint8_t)op;
    p.operand.kind = ATREE_V_UNDEFINED;
    return p;
}

static struct atree__value v_undef(void)
{
    struct atree__value v;
    v.kind = ATREE_V_UNDEFINED;
    return v;
}
static struct atree__value v_bool(bool b)
{
    struct atree__value v;
    v.kind = ATREE_V_BOOL;
    v.u.b = b;
    return v;
}
static struct atree__value v_int(int64_t i)
{
    struct atree__value v;
    v.kind = ATREE_V_INT;
    v.u.i = i;
    return v;
}
static struct atree__value v_float(double d)
{
    struct atree__value v;
    v.kind = ATREE_V_FLOAT;
    v.u.f = d;
    return v;
}
static struct atree__value v_str(uint32_t s)
{
    struct atree__value v;
    v.kind = ATREE_V_STRING;
    v.u.s = s;
    return v;
}
static struct atree__value v_ilist(int64_t *d, uint32_t n)
{
    struct atree__value v;
    v.kind = ATREE_V_INT_LIST;
    v.u.il.data = d;
    v.u.il.len = n;
    return v;
}
static struct atree__value v_slist(uint32_t *d, uint32_t n)
{
    struct atree__value v;
    v.kind = ATREE_V_STRING_LIST;
    v.u.sl.data = d;
    v.u.sl.len = n;
    return v;
}

TEST(type_rules)
{
    struct fixture f;
    struct atree__pred p;
    int64_t il[] = {3, 1, 2, 1};
    uint32_t sl[] = {2, 1};
    ASSERT_FALSE(fixture_init(&f, NULL));

    /* bool variable only on bool attrs */
    p = mk(A_BOOL, ATREE_PRED_VAR, ATREE_OP_EQ);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    p = mk(A_INT, ATREE_PRED_NOT_VAR, ATREE_OP_EQ);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);

    /* comparisons */
    p = mk(A_INT, ATREE_PRED_CMP, ATREE_OP_LT);
    p.operand = v_int(5);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    p.operand = v_float(5.0);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);
    p = mk(A_FLOAT, ATREE_PRED_CMP, ATREE_OP_GE);
    p.operand = v_int(7); /* promoted */
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    ASSERT_EQ_U64(p.operand.kind, ATREE_V_FLOAT);
    ASSERT_TRUE(atree__double_eq(p.operand.u.f, 7.0));
    p.operand = v_int(((int64_t)1 << 53) + 1);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_INVALID_LITERAL);
    p.operand = v_float(nan(""));
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_INVALID_LITERAL);
    p.operand = v_float(HUGE_VAL);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_INVALID_LITERAL);
    p = mk(A_STR, ATREE_PRED_CMP, ATREE_OP_EQ);
    p.operand = v_str(f.s_ca);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    p.op = ATREE_OP_LT; /* strings: equality only */
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);
    p = mk(A_BOOL, ATREE_PRED_CMP, ATREE_OP_EQ);
    p.operand = v_int(1);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);
    p = mk(A_INT, ATREE_PRED_CMP, (atree_op_t)99);
    p.operand = v_int(1);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_INVALID_ARG);

    /* set membership: scalar attr, list literal; list is normalized */
    p = mk(A_INT, ATREE_PRED_IN, ATREE_OP_EQ);
    p.operand = v_ilist(il, 4);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    ASSERT_EQ_U64(p.operand.u.il.len, 3);
    ASSERT_EQ_I64(il[0], 1);
    ASSERT_EQ_I64(il[2], 3);
    p.operand = v_ilist(il, 0);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_INVALID_LITERAL);
    p = mk(A_STR, ATREE_PRED_NOT_IN, ATREE_OP_EQ);
    p.operand = v_slist(sl, 2);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    ASSERT_EQ_U64(sl[0], 1);
    p = mk(A_ILIST, ATREE_PRED_IN, ATREE_OP_EQ);
    p.operand = v_ilist(il, 3);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);
    p = mk(A_INT, ATREE_PRED_IN, ATREE_OP_EQ);
    p.operand = v_slist(sl, 2);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);

    /* list operators: list attr, list literal of the same element type */
    p = mk(A_ILIST, ATREE_PRED_ONE_OF, ATREE_OP_EQ);
    p.operand = v_ilist(il, 3);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    p = mk(A_SLIST, ATREE_PRED_ALL_OF, ATREE_OP_EQ);
    p.operand = v_slist(sl, 2);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    p = mk(A_SLIST, ATREE_PRED_NONE_OF, ATREE_OP_EQ);
    p.operand = v_ilist(il, 3);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);
    p = mk(A_INT, ATREE_PRED_ONE_OF, ATREE_OP_EQ);
    p.operand = v_ilist(il, 3);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);

    /* null / empty */
    p = mk(A_STR, ATREE_PRED_IS_NULL, ATREE_OP_EQ);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    p = mk(A_SLIST, ATREE_PRED_IS_NOT_NULL, ATREE_OP_EQ);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);
    p = mk(A_ILIST, ATREE_PRED_IS_EMPTY, ATREE_OP_EQ);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    p = mk(A_INT, ATREE_PRED_IS_NOT_EMPTY, ATREE_OP_EQ);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_TYPE_MISMATCH);

    /* bad ids */
    p = mk(A_COUNT, ATREE_PRED_VAR, ATREE_OP_EQ);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_INVALID_ARG);
    p = mk(A_BOOL, (enum atree__pred_kind)200, ATREE_OP_EQ);
    ASSERT_STATUS(atree__pred_check(&f.attrs, &p), ATREE_ERR_INVALID_ARG);

    fixture_free(&f);
    ASSERT_EQ_U64(f.m.live, 0);
    return 0;
}

/* Three-valued negation law: eval(not p) == not eval(p), with undefined
 * mapping to undefined, for every kind and for defined and undefined values. */
static int check_negation(const struct atree__pred *p, const struct atree__value *v)
{
    struct atree__pred n = *p;
    atree_tri_t a;
    atree_tri_t b;
    atree__pred_negate(&n);
    a = atree__pred_eval(p, v);
    b = atree__pred_eval(&n, v);
    if (a == ATREE_UNDEFINED) {
        return b == ATREE_UNDEFINED ? 0 : 1;
    }
    if (b == ATREE_UNDEFINED) {
        return 1;
    }
    if ((a == ATREE_TRUE) == (b == ATREE_TRUE)) {
        return 1;
    }
    atree__pred_negate(&n);
    return atree__pred_equal(&n, p) ? 0 : 1; /* double negation is identity */
}

TEST(evaluation_and_negation)
{
    struct fixture f;
    struct atree__pred p;
    struct atree__value u = v_undef();
    int64_t lit_i[] = {10, 20, 30};
    uint32_t lit_s[2];
    int64_t ev_i1[] = {5, 20};
    int64_t ev_i2[] = {10, 20, 30, 40};
    int64_t ev_i3[] = {1, 2};
    uint32_t ev_s1[2];
    ASSERT_FALSE(fixture_init(&f, NULL));
    lit_s[0] = f.s_ca;
    lit_s[1] = f.s_us;
    ev_s1[0] = f.s_us; /* event lists are sorted by id: CA=1, US=2, FR=3 */
    ev_s1[1] = f.s_fr;

    /* booleans */
    p = mk(A_BOOL, ATREE_PRED_VAR, ATREE_OP_EQ);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    {
        struct atree__value t = v_bool(true);
        struct atree__value fl = v_bool(false);
        ASSERT_EQ_U64(atree__pred_eval(&p, &t), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &fl), ATREE_FALSE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &u), ATREE_UNDEFINED);
        ASSERT_FALSE(check_negation(&p, &t));
        ASSERT_FALSE(check_negation(&p, &u));
    }

    /* integer comparisons, every operator, including extremes */
    {
        atree_op_t ops[] = {ATREE_OP_LT, ATREE_OP_LE, ATREE_OP_GT,
                            ATREE_OP_GE, ATREE_OP_EQ, ATREE_OP_NE};
        int expect_lt[] = {1, 1, 0, 0, 0, 1}; /* value 4 vs literal 5 */
        int expect_eq[] = {0, 1, 0, 1, 1, 0}; /* 5 vs 5 */
        int expect_gt[] = {0, 0, 1, 1, 0, 1}; /* 6 vs 5 */
        size_t i;
        for (i = 0; i < 6; i++) {
            struct atree__value lt = v_int(4);
            struct atree__value eq = v_int(5);
            struct atree__value gt = v_int(6);
            struct atree__value mn = v_int(INT64_MIN);
            p = mk(A_INT, ATREE_PRED_CMP, ops[i]);
            p.operand = v_int(5);
            ASSERT_OK(atree__pred_check(&f.attrs, &p));
            ASSERT_EQ_U64(atree__pred_eval(&p, &lt), expect_lt[i] ? ATREE_TRUE : ATREE_FALSE);
            ASSERT_EQ_U64(atree__pred_eval(&p, &eq), expect_eq[i] ? ATREE_TRUE : ATREE_FALSE);
            ASSERT_EQ_U64(atree__pred_eval(&p, &gt), expect_gt[i] ? ATREE_TRUE : ATREE_FALSE);
            ASSERT_EQ_U64(atree__pred_eval(&p, &u), ATREE_UNDEFINED);
            ASSERT_FALSE(check_negation(&p, &lt));
            ASSERT_FALSE(check_negation(&p, &eq));
            ASSERT_FALSE(check_negation(&p, &gt));
            ASSERT_FALSE(check_negation(&p, &mn));
            ASSERT_FALSE(check_negation(&p, &u));
        }
    }

    /* float comparisons with a promoted integer literal */
    p = mk(A_FLOAT, ATREE_PRED_CMP, ATREE_OP_LE);
    p.operand = v_int(2);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    {
        struct atree__value a = v_float(1.999999);
        struct atree__value b = v_float(2.0);
        struct atree__value c = v_float(2.000001);
        ASSERT_EQ_U64(atree__pred_eval(&p, &a), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &b), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &c), ATREE_FALSE);
        ASSERT_FALSE(check_negation(&p, &c));
    }
    p = mk(A_FLOAT, ATREE_PRED_CMP, ATREE_OP_EQ);
    p.operand = v_float(0.0);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    {
        struct atree__value nz = v_float(-0.0);
        ASSERT_EQ_U64(atree__pred_eval(&p, &nz), ATREE_TRUE);
    }

    /* string equality */
    p = mk(A_STR, ATREE_PRED_CMP, ATREE_OP_EQ);
    p.operand = v_str(f.s_ca);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    {
        struct atree__value ca = v_str(f.s_ca);
        struct atree__value us = v_str(f.s_us);
        struct atree__value unknown = v_str(ATREE_STR_UNKNOWN);
        ASSERT_EQ_U64(atree__pred_eval(&p, &ca), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &us), ATREE_FALSE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &unknown), ATREE_FALSE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &u), ATREE_UNDEFINED);
        ASSERT_FALSE(check_negation(&p, &unknown)); /* unknown <> "CA" is true */
    }

    /* in / not in */
    p = mk(A_INT, ATREE_PRED_IN, ATREE_OP_EQ);
    p.operand = v_ilist(lit_i, 3);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    {
        struct atree__value hit = v_int(20);
        struct atree__value miss = v_int(25);
        ASSERT_EQ_U64(atree__pred_eval(&p, &hit), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &miss), ATREE_FALSE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &u), ATREE_UNDEFINED);
        ASSERT_FALSE(check_negation(&p, &hit));
        ASSERT_FALSE(check_negation(&p, &miss));
    }
    p = mk(A_STR, ATREE_PRED_IN, ATREE_OP_EQ);
    p.operand = v_slist(lit_s, 2);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    {
        struct atree__value us = v_str(f.s_us);
        struct atree__value fr = v_str(f.s_fr);
        ASSERT_EQ_U64(atree__pred_eval(&p, &us), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &fr), ATREE_FALSE);
    }

    /* one of / none of / all of on int lists */
    p = mk(A_ILIST, ATREE_PRED_ONE_OF, ATREE_OP_EQ);
    p.operand = v_ilist(lit_i, 3);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    {
        struct atree__value e1 = v_ilist(ev_i1, 2); /* shares 20 */
        struct atree__value e2 = v_ilist(ev_i2, 4); /* superset */
        struct atree__value e3 = v_ilist(ev_i3, 2); /* disjoint */
        struct atree__value e0 = v_ilist(ev_i3, 0); /* empty */
        ASSERT_EQ_U64(atree__pred_eval(&p, &e1), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &e2), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &e3), ATREE_FALSE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &e0), ATREE_FALSE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &u), ATREE_UNDEFINED);
        ASSERT_FALSE(check_negation(&p, &e1));
        ASSERT_FALSE(check_negation(&p, &e3));
        ASSERT_FALSE(check_negation(&p, &u));

        p.kind = ATREE_PRED_ALL_OF; /* every literal present in the event list */
        ASSERT_EQ_U64(atree__pred_eval(&p, &e1), ATREE_FALSE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &e2), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &e0), ATREE_FALSE);
        ASSERT_FALSE(check_negation(&p, &e1));
        ASSERT_FALSE(check_negation(&p, &e2));

        p.kind = ATREE_PRED_IS_EMPTY;
        p.operand = v_undef();
        ASSERT_EQ_U64(atree__pred_eval(&p, &e0), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &e1), ATREE_FALSE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &u), ATREE_UNDEFINED);
        ASSERT_FALSE(check_negation(&p, &e0));
        ASSERT_FALSE(check_negation(&p, &u));
    }

    /* string lists */
    p = mk(A_SLIST, ATREE_PRED_NONE_OF, ATREE_OP_EQ);
    p.operand = v_slist(lit_s, 2);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    {
        struct atree__value e = v_slist(ev_s1, 2); /* {FR, US} shares US */
        ASSERT_EQ_U64(atree__pred_eval(&p, &e), ATREE_FALSE);
        ASSERT_FALSE(check_negation(&p, &e));
    }

    /* is null / is not null are the only kinds defined on undefined */
    p = mk(A_INT, ATREE_PRED_IS_NULL, ATREE_OP_EQ);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    {
        struct atree__value d = v_int(1);
        ASSERT_EQ_U64(atree__pred_eval(&p, &u), ATREE_TRUE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &d), ATREE_FALSE);
        ASSERT_FALSE(check_negation(&p, &u));
        ASSERT_FALSE(check_negation(&p, &d));
        atree__pred_negate(&p);
        ASSERT_EQ_U64(p.kind, ATREE_PRED_IS_NOT_NULL);
        ASSERT_EQ_U64(atree__pred_eval(&p, &u), ATREE_FALSE);
        ASSERT_EQ_U64(atree__pred_eval(&p, &d), ATREE_TRUE);
    }

    fixture_free(&f);
    return 0;
}

TEST(hash_equality_cost_rank)
{
    struct fixture f;
    struct atree__pred a;
    struct atree__pred b;
    int64_t la[] = {1, 2, 3, 4};
    int64_t lb[] = {4, 3, 2, 1, 1};
    ASSERT_FALSE(fixture_init(&f, NULL));

    a = mk(A_INT, ATREE_PRED_IN, ATREE_OP_EQ);
    a.operand = v_ilist(la, 4);
    b = mk(A_INT, ATREE_PRED_IN, ATREE_OP_EQ);
    b.operand = v_ilist(lb, 5);
    ASSERT_OK(atree__pred_check(&f.attrs, &a));
    ASSERT_OK(atree__pred_check(&f.attrs, &b));
    ASSERT_TRUE(atree__pred_equal(&a, &b)); /* order and duplicates normalized away */
    ASSERT_EQ_U64(atree__pred_content_hash(&a, &f.strings),
                  atree__pred_content_hash(&b, &f.strings));
    b.kind = ATREE_PRED_NOT_IN;
    ASSERT_FALSE(atree__pred_equal(&a, &b));
    ASSERT_TRUE(atree__pred_content_hash(&a, &f.strings) !=
                atree__pred_content_hash(&b, &f.strings));

    a = mk(A_INT, ATREE_PRED_CMP, ATREE_OP_LT);
    a.operand = v_int(1);
    b = mk(A_INT, ATREE_PRED_CMP, ATREE_OP_LE);
    b.operand = v_int(1);
    ASSERT_FALSE(atree__pred_equal(&a, &b));
    ASSERT_TRUE(atree__pred_content_hash(&a, &f.strings) !=
                atree__pred_content_hash(&b, &f.strings));
    b.op = ATREE_OP_LT;
    b.attr = A_FLOAT;
    ASSERT_FALSE(atree__pred_equal(&a, &b));

    /* cost grows with list length; constant kinds cost 1 */
    a = mk(A_INT, ATREE_PRED_CMP, ATREE_OP_EQ);
    a.operand = v_int(1);
    ASSERT_EQ_U64(atree__pred_cost(&a), 1);
    a = mk(A_INT, ATREE_PRED_IN, ATREE_OP_EQ);
    a.operand = v_ilist(la, 4);
    ASSERT_EQ_U64(atree__pred_cost(&a), 3); /* 1 + log2(4) */
    a = mk(A_ILIST, ATREE_PRED_ONE_OF, ATREE_OP_EQ);
    a.operand = v_ilist(la, 4);
    ASSERT_EQ_U64(atree__pred_cost(&a), 4);

    /* wake rank: equality < range < bool < negated */
    a = mk(A_INT, ATREE_PRED_CMP, ATREE_OP_EQ);
    b = mk(A_INT, ATREE_PRED_CMP, ATREE_OP_GT);
    ASSERT_TRUE(atree__pred_wake_rank(&a) < atree__pred_wake_rank(&b));
    a = mk(A_BOOL, ATREE_PRED_VAR, ATREE_OP_EQ);
    ASSERT_TRUE(atree__pred_wake_rank(&b) < atree__pred_wake_rank(&a));
    b = mk(A_INT, ATREE_PRED_CMP, ATREE_OP_NE);
    ASSERT_TRUE(atree__pred_wake_rank(&a) < atree__pred_wake_rank(&b));
    a = mk(A_INT, ATREE_PRED_NOT_IN, ATREE_OP_EQ);
    ASSERT_EQ_I64(atree__pred_wake_rank(&a), atree__pred_wake_rank(&b));

    fixture_free(&f);
    return 0;
}

TEST(copy_and_free)
{
    struct test_alloc ta;
    struct fixture f;
    struct atree__pred src;
    struct atree__pred dst;
    int64_t l[] = {2, 1};
    test_alloc_init(&ta);
    ASSERT_FALSE(fixture_init(&f, &ta.a));
    src = mk(A_ILIST, ATREE_PRED_ALL_OF, ATREE_OP_EQ);
    src.operand = v_ilist(l, 2);
    ASSERT_OK(atree__pred_check(&f.attrs, &src));
    ASSERT_OK(atree__pred_copy(&f.m, &dst, &src));
    ASSERT_TRUE(atree__pred_equal(&src, &dst));
    ASSERT_TRUE(dst.operand.u.il.data != l);
    atree__pred_free(&f.m, &dst);
    fixture_free(&f);
    ASSERT_TRUE(test_alloc_clean(&ta));
    return 0;
}

/* Writer capturing into a buffer. */
struct sink {
    char buf[512];
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

static int render(struct fixture *f, const struct atree__pred *p, struct sink *s)
{
    struct atree__writer w;
    s->len = 0;
    s->buf[0] = '\0';
    atree__writer_init(&w, sink_write, s);
    atree__pred_print(p, &f->attrs, atree__strtab_resolver, &f->strings, &w);
    return atree__writer_status(&w) == ATREE_OK ? 0 : 1;
}

TEST(print)
{
    struct fixture f;
    struct atree__pred p;
    struct sink s;
    int64_t il[] = {30, 10, 20};
    uint32_t sl[2];
    ASSERT_FALSE(fixture_init(&f, NULL));
    sl[0] = f.s_us;
    sl[1] = f.s_ca;

    p = mk(A_BOOL, ATREE_PRED_VAR, ATREE_OP_EQ);
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "flag");
    atree__pred_negate(&p);
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "not flag");

    p = mk(A_INT, ATREE_PRED_CMP, ATREE_OP_GE);
    p.operand = v_int(-7);
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "age >= -7");

    p = mk(A_FLOAT, ATREE_PRED_CMP, ATREE_OP_LT);
    p.operand = v_int(3);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "price < 3.0");
    p.operand = v_float(0.1);
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "price < 0.1");
    p.operand = v_float(1e20);
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "price < 1.0e+20");

    p = mk(A_STR, ATREE_PRED_CMP, ATREE_OP_NE);
    p.operand = v_str(f.s_ca);
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "country <> \"CA\"");

    p = mk(A_INT, ATREE_PRED_NOT_IN, ATREE_OP_EQ);
    p.operand = v_ilist(il, 3);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "age not in [10, 20, 30]");

    p = mk(A_SLIST, ATREE_PRED_ONE_OF, ATREE_OP_EQ);
    p.operand = v_slist(sl, 2);
    ASSERT_OK(atree__pred_check(&f.attrs, &p));
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "deals one of [\"CA\", \"US\"]");
    p.kind = ATREE_PRED_NONE_OF;
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "deals none of [\"CA\", \"US\"]");
    p.kind = ATREE_PRED_ALL_OF;
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "deals all of [\"CA\", \"US\"]");
    p.kind = ATREE_PRED_NOT_ALL_OF;
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "not (deals all of [\"CA\", \"US\"])");

    p = mk(A_STR, ATREE_PRED_IS_NULL, ATREE_OP_EQ);
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "country is null");
    p.kind = ATREE_PRED_IS_NOT_NULL;
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "country is not null");
    p = mk(A_ILIST, ATREE_PRED_IS_EMPTY, ATREE_OP_EQ);
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "segments is empty");
    p.kind = ATREE_PRED_IS_NOT_EMPTY;
    ASSERT_FALSE(render(&f, &p, &s));
    ASSERT_EQ_STR(s.buf, "segments is not empty");

    /* strings needing escapes */
    {
        uint32_t id;
        ASSERT_OK(atree__strtab_intern(&f.m, &f.strings, "a\"b\\c\n", 6, &id));
        p = mk(A_STR, ATREE_PRED_CMP, ATREE_OP_EQ);
        p.operand = v_str(id);
        ASSERT_FALSE(render(&f, &p, &s));
        ASSERT_EQ_STR(s.buf, "country = \"a\\\"b\\\\c\\n\"");
    }

    fixture_free(&f);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(type_rules);
RUN_TEST(evaluation_and_negation);
RUN_TEST(hash_equality_cost_rank);
RUN_TEST(copy_and_free);
RUN_TEST(print);
TEST_MAIN_END()

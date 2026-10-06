/*
 * Predicates.
 *
 * SPDX-License-Identifier: MIT
 */
#include "predicate.h"

#include <assert.h>

#include "hash.h"

static bool op_is_valid(uint8_t op)
{
    switch ((atree_op_t)op) {
    case ATREE_OP_LT:
    case ATREE_OP_LE:
    case ATREE_OP_GT:
    case ATREE_OP_GE:
    case ATREE_OP_EQ:
    case ATREE_OP_NE:
        return true;
    default:
        return false;
    }
}

static bool op_is_equality(uint8_t op)
{
    return op == ATREE_OP_EQ || op == ATREE_OP_NE;
}

/* Sorts and deduplicates a list operand; rejects empty lists. */
static atree_status_t normalize_list(struct atree__value *v)
{
    if (v->kind == ATREE_V_INT_LIST) {
        if (v->u.il.len == 0) {
            return ATREE_ERR_INVALID_LITERAL;
        }
        v->u.il.len = atree__sort_unique_i64(v->u.il.data, v->u.il.len);
        return ATREE_OK;
    }
    if (v->kind == ATREE_V_STRING_LIST) {
        if (v->u.sl.len == 0) {
            return ATREE_ERR_INVALID_LITERAL;
        }
        v->u.sl.len = atree__sort_unique_u32(v->u.sl.data, v->u.sl.len);
        return ATREE_OK;
    }
    return ATREE_ERR_TYPE_MISMATCH;
}

atree_status_t atree__pred_check(const struct atree__attrs *attrs, struct atree__pred *p)
{
    atree_type_t t;
    struct atree__value *v = &p->operand;

    if (p->attr >= atree__attrs_count(attrs) || p->kind >= ATREE_PRED_KIND_COUNT) {
        return ATREE_ERR_INVALID_ARG;
    }
    t = atree__attrs_get(attrs, p->attr)->type;

    switch ((enum atree__pred_kind)p->kind) {
    case ATREE_PRED_VAR:
    case ATREE_PRED_NOT_VAR:
        if (t != ATREE_TYPE_BOOL) {
            return ATREE_ERR_TYPE_MISMATCH;
        }
        v->kind = ATREE_V_UNDEFINED;
        return ATREE_OK;

    case ATREE_PRED_CMP:
        if (!op_is_valid(p->op)) {
            return ATREE_ERR_INVALID_ARG;
        }
        if (t == ATREE_TYPE_INT) {
            return v->kind == ATREE_V_INT ? ATREE_OK : ATREE_ERR_TYPE_MISMATCH;
        }
        if (t == ATREE_TYPE_FLOAT) {
            if (v->kind == ATREE_V_INT) {
                double d;
                if (!atree__int_to_double_exact(v->u.i, &d)) {
                    return ATREE_ERR_INVALID_LITERAL;
                }
                v->kind = ATREE_V_FLOAT;
                v->u.f = d;
            }
            if (v->kind != ATREE_V_FLOAT) {
                return ATREE_ERR_TYPE_MISMATCH;
            }
            return atree__double_is_finite(v->u.f) ? ATREE_OK : ATREE_ERR_INVALID_LITERAL;
        }
        if (t == ATREE_TYPE_STRING) {
            if (v->kind != ATREE_V_STRING) {
                return ATREE_ERR_TYPE_MISMATCH;
            }
            return op_is_equality(p->op) ? ATREE_OK : ATREE_ERR_TYPE_MISMATCH;
        }
        return ATREE_ERR_TYPE_MISMATCH;

    case ATREE_PRED_IN:
    case ATREE_PRED_NOT_IN:
        if ((t == ATREE_TYPE_INT && v->kind == ATREE_V_INT_LIST) ||
            (t == ATREE_TYPE_STRING && v->kind == ATREE_V_STRING_LIST)) {
            return normalize_list(v);
        }
        return ATREE_ERR_TYPE_MISMATCH;

    case ATREE_PRED_ONE_OF:
    case ATREE_PRED_NONE_OF:
    case ATREE_PRED_ALL_OF:
    case ATREE_PRED_NOT_ALL_OF:
        if ((t == ATREE_TYPE_INT_LIST && v->kind == ATREE_V_INT_LIST) ||
            (t == ATREE_TYPE_STRING_LIST && v->kind == ATREE_V_STRING_LIST)) {
            return normalize_list(v);
        }
        return ATREE_ERR_TYPE_MISMATCH;

    case ATREE_PRED_IS_NULL:
    case ATREE_PRED_IS_NOT_NULL:
        if (atree__type_is_list(t)) {
            return ATREE_ERR_TYPE_MISMATCH;
        }
        v->kind = ATREE_V_UNDEFINED;
        return ATREE_OK;

    case ATREE_PRED_IS_EMPTY:
    case ATREE_PRED_IS_NOT_EMPTY:
        if (!atree__type_is_list(t)) {
            return ATREE_ERR_TYPE_MISMATCH;
        }
        v->kind = ATREE_V_UNDEFINED;
        return ATREE_OK;

    case ATREE_PRED_KIND_COUNT:
    default:
        return ATREE_ERR_INVALID_ARG;
    }
}

void atree__pred_negate(struct atree__pred *p)
{
    switch ((enum atree__pred_kind)p->kind) {
    case ATREE_PRED_VAR:
        p->kind = ATREE_PRED_NOT_VAR;
        break;
    case ATREE_PRED_NOT_VAR:
        p->kind = ATREE_PRED_VAR;
        break;
    case ATREE_PRED_CMP:
        switch ((atree_op_t)p->op) {
        case ATREE_OP_LT:
            p->op = ATREE_OP_GE;
            break;
        case ATREE_OP_LE:
            p->op = ATREE_OP_GT;
            break;
        case ATREE_OP_GT:
            p->op = ATREE_OP_LE;
            break;
        case ATREE_OP_GE:
            p->op = ATREE_OP_LT;
            break;
        case ATREE_OP_EQ:
            p->op = ATREE_OP_NE;
            break;
        case ATREE_OP_NE:
            p->op = ATREE_OP_EQ;
            break;
        default:
            break;
        }
        break;
    case ATREE_PRED_IN:
        p->kind = ATREE_PRED_NOT_IN;
        break;
    case ATREE_PRED_NOT_IN:
        p->kind = ATREE_PRED_IN;
        break;
    case ATREE_PRED_ONE_OF:
        p->kind = ATREE_PRED_NONE_OF;
        break;
    case ATREE_PRED_NONE_OF:
        p->kind = ATREE_PRED_ONE_OF;
        break;
    case ATREE_PRED_ALL_OF:
        p->kind = ATREE_PRED_NOT_ALL_OF;
        break;
    case ATREE_PRED_NOT_ALL_OF:
        p->kind = ATREE_PRED_ALL_OF;
        break;
    case ATREE_PRED_IS_NULL:
        p->kind = ATREE_PRED_IS_NOT_NULL;
        break;
    case ATREE_PRED_IS_NOT_NULL:
        p->kind = ATREE_PRED_IS_NULL;
        break;
    case ATREE_PRED_IS_EMPTY:
        p->kind = ATREE_PRED_IS_NOT_EMPTY;
        break;
    case ATREE_PRED_IS_NOT_EMPTY:
        p->kind = ATREE_PRED_IS_EMPTY;
        break;
    case ATREE_PRED_KIND_COUNT:
    default:
        break;
    }
}

static bool apply_op(uint8_t op, int cmp)
{
    switch ((atree_op_t)op) {
    case ATREE_OP_LT:
        return cmp < 0;
    case ATREE_OP_LE:
        return cmp <= 0;
    case ATREE_OP_GT:
        return cmp > 0;
    case ATREE_OP_GE:
        return cmp >= 0;
    case ATREE_OP_EQ:
        return cmp == 0;
    case ATREE_OP_NE:
        return cmp != 0;
    default:
        return false;
    }
}

static atree_tri_t tri(bool b)
{
    return b ? ATREE_TRUE : ATREE_FALSE;
}

static uint32_t list_len(const struct atree__value *v)
{
    return v->kind == ATREE_V_INT_LIST ? v->u.il.len : v->u.sl.len;
}

atree_tri_t atree__pred_eval(const struct atree__pred *p, const struct atree__value *v)
{
    const struct atree__value *lit = &p->operand;

    if (v->kind == ATREE_V_UNDEFINED) {
        if (p->kind == ATREE_PRED_IS_NULL) {
            return ATREE_TRUE;
        }
        if (p->kind == ATREE_PRED_IS_NOT_NULL) {
            return ATREE_FALSE;
        }
        return ATREE_UNDEFINED;
    }

    switch ((enum atree__pred_kind)p->kind) {
    case ATREE_PRED_VAR:
        assert(v->kind == ATREE_V_BOOL);
        return tri(v->u.b);
    case ATREE_PRED_NOT_VAR:
        assert(v->kind == ATREE_V_BOOL);
        return tri(!v->u.b);

    case ATREE_PRED_CMP:
        if (v->kind == ATREE_V_INT && lit->kind == ATREE_V_INT) {
            return tri(apply_op(p->op, (v->u.i > lit->u.i) - (v->u.i < lit->u.i)));
        }
        if (v->kind == ATREE_V_FLOAT && lit->kind == ATREE_V_FLOAT) {
            return tri(apply_op(p->op, atree__double_cmp(v->u.f, lit->u.f)));
        }
        if (v->kind == ATREE_V_STRING && lit->kind == ATREE_V_STRING) {
            return tri(apply_op(p->op, (v->u.s > lit->u.s) - (v->u.s < lit->u.s)));
        }
        assert(0 && "predicate/value type mismatch");
        return ATREE_UNDEFINED;

    case ATREE_PRED_IN:
    case ATREE_PRED_NOT_IN: {
        bool in;
        if (v->kind == ATREE_V_INT && lit->kind == ATREE_V_INT_LIST) {
            in = atree__bsearch_i64(lit->u.il.data, lit->u.il.len, v->u.i);
        } else if (v->kind == ATREE_V_STRING && lit->kind == ATREE_V_STRING_LIST) {
            in = atree__bsearch_u32(lit->u.sl.data, lit->u.sl.len, v->u.s);
        } else {
            assert(0 && "predicate/value type mismatch");
            return ATREE_UNDEFINED;
        }
        return tri(p->kind == ATREE_PRED_IN ? in : !in);
    }

    case ATREE_PRED_ONE_OF:
    case ATREE_PRED_NONE_OF: {
        bool hit;
        if (v->kind == ATREE_V_INT_LIST && lit->kind == ATREE_V_INT_LIST) {
            hit = atree__intersects_i64(v->u.il.data, v->u.il.len, lit->u.il.data, lit->u.il.len);
        } else if (v->kind == ATREE_V_STRING_LIST && lit->kind == ATREE_V_STRING_LIST) {
            hit = atree__intersects_u32(v->u.sl.data, v->u.sl.len, lit->u.sl.data, lit->u.sl.len);
        } else {
            assert(0 && "predicate/value type mismatch");
            return ATREE_UNDEFINED;
        }
        return tri(p->kind == ATREE_PRED_ONE_OF ? hit : !hit);
    }

    case ATREE_PRED_ALL_OF:
    case ATREE_PRED_NOT_ALL_OF: {
        bool all;
        if (v->kind == ATREE_V_INT_LIST && lit->kind == ATREE_V_INT_LIST) {
            all = atree__contains_all_i64(v->u.il.data, v->u.il.len, lit->u.il.data, lit->u.il.len);
        } else if (v->kind == ATREE_V_STRING_LIST && lit->kind == ATREE_V_STRING_LIST) {
            all = atree__contains_all_u32(v->u.sl.data, v->u.sl.len, lit->u.sl.data, lit->u.sl.len);
        } else {
            assert(0 && "predicate/value type mismatch");
            return ATREE_UNDEFINED;
        }
        return tri(p->kind == ATREE_PRED_ALL_OF ? all : !all);
    }

    case ATREE_PRED_IS_NULL:
        return ATREE_FALSE;
    case ATREE_PRED_IS_NOT_NULL:
        return ATREE_TRUE;
    case ATREE_PRED_IS_EMPTY:
        assert(v->kind == ATREE_V_INT_LIST || v->kind == ATREE_V_STRING_LIST);
        return tri(list_len(v) == 0);
    case ATREE_PRED_IS_NOT_EMPTY:
        assert(v->kind == ATREE_V_INT_LIST || v->kind == ATREE_V_STRING_LIST);
        return tri(list_len(v) != 0);

    case ATREE_PRED_KIND_COUNT:
    default:
        return ATREE_UNDEFINED;
    }
}

uint64_t atree__pred_hash(const struct atree__pred *p)
{
    uint64_t h = atree__hash_u64(UINT64_C(0x70726564) ^ p->attr);
    h = atree__hash_combine(h, ((uint64_t)p->kind << 8) | p->op);
    return atree__hash_combine(h, atree__value_hash(&p->operand));
}

bool atree__pred_equal(const struct atree__pred *a, const struct atree__pred *b)
{
    return a->attr == b->attr && a->kind == b->kind && a->op == b->op &&
        atree__value_equal(&a->operand, &b->operand);
}

static uint64_t log2_floor(uint64_t n)
{
    uint64_t r = 0;
    while (n > 1) {
        n >>= 1;
        r++;
    }
    return r;
}

uint64_t atree__pred_cost(const struct atree__pred *p)
{
    switch ((enum atree__pred_kind)p->kind) {
    case ATREE_PRED_IN:
    case ATREE_PRED_NOT_IN:
        return 1 + log2_floor(list_len(&p->operand));
    case ATREE_PRED_ONE_OF:
    case ATREE_PRED_NONE_OF:
    case ATREE_PRED_ALL_OF:
    case ATREE_PRED_NOT_ALL_OF:
        return list_len(&p->operand);
    case ATREE_PRED_VAR:
    case ATREE_PRED_NOT_VAR:
    case ATREE_PRED_CMP:
    case ATREE_PRED_IS_NULL:
    case ATREE_PRED_IS_NOT_NULL:
    case ATREE_PRED_IS_EMPTY:
    case ATREE_PRED_IS_NOT_EMPTY:
    case ATREE_PRED_KIND_COUNT:
    default:
        return 1;
    }
}

int atree__pred_wake_rank(const struct atree__pred *p)
{
    switch ((enum atree__pred_kind)p->kind) {
    case ATREE_PRED_CMP:
        if (p->op == ATREE_OP_EQ) {
            return 0;
        }
        return p->op == ATREE_OP_NE ? 5 : 1;
    case ATREE_PRED_IN:
    case ATREE_PRED_ONE_OF:
        return 0;
    case ATREE_PRED_ALL_OF:
    case ATREE_PRED_IS_EMPTY:
        return 1;
    case ATREE_PRED_VAR:
    case ATREE_PRED_NOT_VAR:
    case ATREE_PRED_IS_NULL:
        return 4;
    case ATREE_PRED_NOT_IN:
    case ATREE_PRED_NONE_OF:
    case ATREE_PRED_NOT_ALL_OF:
    case ATREE_PRED_IS_NOT_NULL:
    case ATREE_PRED_IS_NOT_EMPTY:
    case ATREE_PRED_KIND_COUNT:
    default:
        return 5;
    }
}

atree_status_t atree__pred_copy(struct atree__mem *m, struct atree__pred *dst,
                                const struct atree__pred *src)
{
    dst->attr = src->attr;
    dst->kind = src->kind;
    dst->op = src->op;
    return atree__value_copy(m, &dst->operand, &src->operand);
}

void atree__pred_free(struct atree__mem *m, struct atree__pred *p)
{
    atree__value_free(m, &p->operand);
}

/* ---- printing ----------------------------------------------------------- */

static void print_scalar(const struct atree__value *v, const struct atree__strtab *strings,
                         struct atree__writer *w)
{
    switch (v->kind) {
    case ATREE_V_INT:
        atree__write_i64(w, v->u.i);
        break;
    case ATREE_V_FLOAT:
        atree__write_double(w, v->u.f);
        break;
    case ATREE_V_STRING: {
        uint32_t len = 0;
        const char *s = atree__strtab_get(strings, v->u.s, &len);
        atree__write_quoted(w, s != NULL ? s : "", s != NULL ? len : 0);
        break;
    }
    default:
        break;
    }
}

static void print_list(const struct atree__value *v, const struct atree__strtab *strings,
                       struct atree__writer *w)
{
    uint32_t i;
    atree__write(w, "[", 1);
    if (v->kind == ATREE_V_INT_LIST) {
        for (i = 0; i < v->u.il.len; i++) {
            if (i > 0) {
                atree__write(w, ", ", 2);
            }
            atree__write_i64(w, v->u.il.data[i]);
        }
    } else if (v->kind == ATREE_V_STRING_LIST) {
        for (i = 0; i < v->u.sl.len; i++) {
            uint32_t len = 0;
            const char *s = atree__strtab_get(strings, v->u.sl.data[i], &len);
            if (i > 0) {
                atree__write(w, ", ", 2);
            }
            atree__write_quoted(w, s != NULL ? s : "", s != NULL ? len : 0);
        }
    }
    atree__write(w, "]", 1);
}

static const char *op_text(uint8_t op)
{
    switch ((atree_op_t)op) {
    case ATREE_OP_LT:
        return "<";
    case ATREE_OP_LE:
        return "<=";
    case ATREE_OP_GT:
        return ">";
    case ATREE_OP_GE:
        return ">=";
    case ATREE_OP_EQ:
        return "=";
    case ATREE_OP_NE:
        return "<>";
    default:
        return "?";
    }
}

void atree__pred_print(const struct atree__pred *p, const struct atree__attrs *attrs,
                       const struct atree__strtab *strings, struct atree__writer *w)
{
    const struct atree__attr *a = atree__attrs_get(attrs, p->attr);
    const char *name = a->name;
    size_t name_len = a->name_len;

    switch ((enum atree__pred_kind)p->kind) {
    case ATREE_PRED_VAR:
        atree__write(w, name, name_len);
        break;
    case ATREE_PRED_NOT_VAR:
        atree__write_cstr(w, "not ");
        atree__write(w, name, name_len);
        break;
    case ATREE_PRED_CMP:
        atree__write(w, name, name_len);
        atree__write(w, " ", 1);
        atree__write_cstr(w, op_text(p->op));
        atree__write(w, " ", 1);
        print_scalar(&p->operand, strings, w);
        break;
    case ATREE_PRED_IN:
    case ATREE_PRED_NOT_IN:
        atree__write(w, name, name_len);
        atree__write_cstr(w, p->kind == ATREE_PRED_IN ? " in " : " not in ");
        print_list(&p->operand, strings, w);
        break;
    case ATREE_PRED_ONE_OF:
    case ATREE_PRED_NONE_OF:
    case ATREE_PRED_ALL_OF:
        atree__write(w, name, name_len);
        atree__write_cstr(w,
                          p->kind == ATREE_PRED_ONE_OF        ? " one of "
                              : p->kind == ATREE_PRED_NONE_OF ? " none of "
                                                              : " all of ");
        print_list(&p->operand, strings, w);
        break;
    case ATREE_PRED_NOT_ALL_OF:
        atree__write_cstr(w, "not (");
        atree__write(w, name, name_len);
        atree__write_cstr(w, " all of ");
        print_list(&p->operand, strings, w);
        atree__write(w, ")", 1);
        break;
    case ATREE_PRED_IS_NULL:
        atree__write(w, name, name_len);
        atree__write_cstr(w, " is null");
        break;
    case ATREE_PRED_IS_NOT_NULL:
        atree__write(w, name, name_len);
        atree__write_cstr(w, " is not null");
        break;
    case ATREE_PRED_IS_EMPTY:
        atree__write(w, name, name_len);
        atree__write_cstr(w, " is empty");
        break;
    case ATREE_PRED_IS_NOT_EMPTY:
        atree__write(w, name, name_len);
        atree__write_cstr(w, " is not empty");
        break;
    case ATREE_PRED_KIND_COUNT:
    default:
        atree__write_cstr(w, "<invalid predicate>");
        break;
    }
}

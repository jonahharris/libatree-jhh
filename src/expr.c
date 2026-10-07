/*
 * Expressions: builders, normalization, reference evaluation, printing.
 *
 * SPDX-License-Identifier: MIT
 */
#include "expr.h"

#include <stdlib.h>
#include <string.h>

#include "atree_internal.h"
#include "event.h"
#include "hash.h"
#include "vec.h"

ATREE_VEC_DEFINE(exprvec, atree_expr_t *);

/* ---- node lifecycle ----------------------------------------------------- */

static atree_expr_t *node_new(const atree_t *tree, enum atree__expr_kind kind)
{
    struct atree__mem mem;
    atree_expr_t *e;
    atree__mem_init(&mem, &tree->mem.a);
    e = atree__zalloc(&mem, sizeof *e);
    if (e == NULL) {
        return NULL;
    }
    e->mem = mem;
    e->tree = tree;
    e->kind = (uint8_t)kind;
    e->depth = 1;
    e->pred.operand.kind = ATREE_V_UNDEFINED;
    return e;
}

static void arena_free(struct atree__arena *a);

void atree_expr_free(atree_expr_t *e)
{
    struct atree__mem mem;
    uint32_t i;
    if (e == NULL) {
        return;
    }
    if (e->arena != NULL) {
        arena_free(e->arena); /* the whole normalized tree at once */
        return;
    }
    for (i = 0; i < e->nchildren; i++) {
        atree_expr_free(e->children[i]);
    }
    atree__free_array(&e->mem, e->children, e->nchildren, sizeof *e->children);
    if (e->pred.operand.kind == ATREE_V_INT_LIST) {
        atree__pred_free(&e->mem, &e->pred);
    }
    for (i = 0; i < e->nstrs; i++) {
        atree__strfree(&e->mem, e->strs[i].data, e->strs[i].len);
    }
    atree__free_array(&e->mem, e->strs, e->nstrs, sizeof *e->strs);
    mem = e->mem;
    atree__free(&mem, e, sizeof *e);
}

static void free_children_array(atree_expr_t **children, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        atree_expr_free(children[i]);
    }
}

/* ---- hashing and ordering ----------------------------------------------- */

static int rawstr_cmp(const struct atree__rawstr *a, const struct atree__rawstr *b)
{
    uint32_t n = a->len < b->len ? a->len : b->len;
    int c = n == 0 ? 0 : memcmp(a->data, b->data, n);
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return (a->len > b->len) - (a->len < b->len);
}

static int rawstr_qsort_cmp(const void *pa, const void *pb)
{
    return rawstr_cmp((const struct atree__rawstr *)pa, (const struct atree__rawstr *)pb);
}

/* A leaf's structural hash is the tree's leaf identity hash, so the one
 * computation at build time serves normalization (ordering, dedup) and the
 * insert's identity probes alike. */
static uint64_t compute_hash(const atree_expr_t *e)
{
    uint64_t h = atree__hash_u64(UINT64_C(0x65787072) ^ e->kind);
    uint32_t i;
    if (e->kind == ATREE_EXPR_PRED) {
        return atree__expr_leaf_hash(e);
    }
    for (i = 0; i < e->nchildren; i++) {
        h = atree__hash_combine(h, e->children[i]->hash);
    }
    return h;
}

static int value_cmp(const struct atree__value *a, const struct atree__value *b)
{
    uint32_t i;
    if (a->kind != b->kind) {
        return (a->kind > b->kind) - (a->kind < b->kind);
    }
    switch (a->kind) {
    case ATREE_V_BOOL:
        return (a->u.b > b->u.b) - (a->u.b < b->u.b);
    case ATREE_V_INT:
        return (a->u.i > b->u.i) - (a->u.i < b->u.i);
    case ATREE_V_FLOAT:
        return atree__double_cmp(a->u.f, b->u.f);
    case ATREE_V_STRING:
        return (a->u.s > b->u.s) - (a->u.s < b->u.s);
    case ATREE_V_INT_LIST:
        if (a->u.il.len != b->u.il.len) {
            return (a->u.il.len > b->u.il.len) - (a->u.il.len < b->u.il.len);
        }
        for (i = 0; i < a->u.il.len; i++) {
            if (a->u.il.data[i] != b->u.il.data[i]) {
                return a->u.il.data[i] < b->u.il.data[i] ? -1 : 1;
            }
        }
        return 0;
    case ATREE_V_STRING_LIST:
        /* Unresolved lists carry no ids; their strings are compared by the caller. */
        return (a->u.sl.len > b->u.sl.len) - (a->u.sl.len < b->u.sl.len);
    case ATREE_V_UNDEFINED:
    default:
        return 0;
    }
}

int atree__expr_cmp(const atree_expr_t *a, const atree_expr_t *b)
{
    uint32_t i;
    int c;
    if (a == b) {
        return 0;
    }
    if (a->hash != b->hash) {
        return a->hash < b->hash ? -1 : 1;
    }
    if (a->kind != b->kind) {
        return (a->kind > b->kind) - (a->kind < b->kind);
    }
    if (a->kind == ATREE_EXPR_PRED) {
        if (a->pred.attr != b->pred.attr) {
            return a->pred.attr < b->pred.attr ? -1 : 1;
        }
        if (a->pred.kind != b->pred.kind) {
            return a->pred.kind < b->pred.kind ? -1 : 1;
        }
        if (a->pred.op != b->pred.op) {
            return a->pred.op < b->pred.op ? -1 : 1;
        }
        c = value_cmp(&a->pred.operand, &b->pred.operand);
        if (c != 0) {
            return c;
        }
        if (a->nstrs != b->nstrs) {
            return a->nstrs < b->nstrs ? -1 : 1;
        }
        for (i = 0; i < a->nstrs; i++) {
            c = rawstr_cmp(&a->strs[i], &b->strs[i]);
            if (c != 0) {
                return c;
            }
        }
        return 0;
    }
    if (a->nchildren != b->nchildren) {
        return a->nchildren < b->nchildren ? -1 : 1;
    }
    for (i = 0; i < a->nchildren; i++) {
        c = atree__expr_cmp(a->children[i], b->children[i]);
        if (c != 0) {
            return c;
        }
    }
    return 0;
}

bool atree__expr_equal(const atree_expr_t *a, const atree_expr_t *b)
{
    return atree__expr_cmp(a, b) == 0;
}

static int expr_qsort_cmp(const void *pa, const void *pb)
{
    return atree__expr_cmp(*(atree_expr_t *const *)pa, *(atree_expr_t *const *)pb);
}

/* ---- constructors ------------------------------------------------------- */

atree_status_t atree__expr_new_const(const atree_t *tree, bool value, atree_expr_t **out)
{
    atree_expr_t *e;
    if (out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (tree == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    e = node_new(tree, value ? ATREE_EXPR_TRUE : ATREE_EXPR_FALSE);
    if (e == NULL) {
        return ATREE_ERR_NOMEM;
    }
    e->hash = compute_hash(e);
    *out = e;
    return ATREE_OK;
}

/* Copies, sorts and deduplicates an int list into the node. */
static atree_status_t own_ints(atree_expr_t *e, const int64_t *ints, size_t n)
{
    int64_t *copy;
    uint32_t m;
    e->pred.operand.kind = ATREE_V_INT_LIST;
    e->pred.operand.u.il.data = NULL;
    e->pred.operand.u.il.len = 0;
    if (n == 0) {
        return ATREE_OK;
    }
    copy = atree__alloc_array(&e->mem, n, sizeof *copy);
    if (copy == NULL) {
        return ATREE_ERR_NOMEM;
    }
    memcpy(copy, ints, n * sizeof *copy);
    m = atree__sort_unique_i64(copy, (uint32_t)n);
    if (m < n) {
        int64_t *shrunk = atree__realloc_array(&e->mem, copy, n, m, sizeof *copy);
        if (shrunk == NULL) {
            atree__free_array(&e->mem, copy, n, sizeof *copy);
            return ATREE_ERR_NOMEM;
        }
        copy = shrunk;
    }
    e->pred.operand.u.il.data = copy;
    e->pred.operand.u.il.len = m;
    return ATREE_OK;
}

/* Copies raw strings into the node, sorted bytewise and unique. */
static atree_status_t own_strs(atree_expr_t *e, const char *const *strs, const size_t *lens,
                               size_t n)
{
    size_t i;
    uint32_t m;
    uint32_t k;
    if (n == 0) {
        return ATREE_OK;
    }
    e->strs = atree__zalloc_array(&e->mem, n, sizeof *e->strs);
    if (e->strs == NULL) {
        return ATREE_ERR_NOMEM;
    }
    e->nstrs = (uint32_t)n; /* allocation size; entries may be NULL while filling */
    for (i = 0; i < n; i++) {
        size_t len;
        if (strs[i] == NULL) {
            return ATREE_ERR_INVALID_ARG;
        }
        len = (lens != NULL && lens[i] != SIZE_MAX) ? lens[i] : strlen(strs[i]);
        if (len >= UINT32_MAX) {
            return ATREE_ERR_LIMIT;
        }
        e->strs[i].data = atree__strndup(&e->mem, strs[i], len);
        if (e->strs[i].data == NULL) {
            return ATREE_ERR_NOMEM;
        }
        e->strs[i].len = (uint32_t)len;
    }
    if (n == 1) {
        return ATREE_OK;
    }
    qsort(e->strs, n, sizeof *e->strs, rawstr_qsort_cmp);
    m = 0;
    for (k = 0; k < n; k++) {
        if (m > 0 && rawstr_cmp(&e->strs[m - 1], &e->strs[k]) == 0) {
            atree__strfree(&e->mem, e->strs[k].data, e->strs[k].len);
            e->strs[k].data = NULL;
            e->strs[k].len = 0;
        } else {
            struct atree__rawstr keep = e->strs[k];
            e->strs[k].data = NULL;
            e->strs[k].len = 0;
            e->strs[m++] = keep;
        }
    }
    if (m < n) {
        /* Tail entries are NULL; shrink so the array size equals nstrs. */
        struct atree__rawstr *shrunk =
            atree__realloc_array(&e->mem, e->strs, n, m, sizeof *e->strs);
        if (shrunk == NULL) {
            return ATREE_ERR_NOMEM; /* nstrs still equals the allocation size */
        }
        e->strs = shrunk;
        e->nstrs = m;
    }
    return ATREE_OK;
}

atree_status_t atree__expr_new_pred(const atree_t *tree, atree_attr_id_t attr,
                                    enum atree__pred_kind kind, atree_op_t op,
                                    enum atree__vkind operand_kind,
                                    const struct atree__value *scalar, const int64_t *ints,
                                    size_t nints, const char *const *strs, const size_t *lens,
                                    size_t nstrs, atree_expr_t **out)
{
    atree_expr_t *e;
    atree_status_t st = ATREE_OK;

    if (out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (tree == NULL || (nints > 0 && ints == NULL) || (nstrs > 0 && strs == NULL) ||
        nints >= UINT32_MAX || nstrs >= UINT32_MAX) {
        return ATREE_ERR_INVALID_ARG;
    }
    e = node_new(tree, ATREE_EXPR_PRED);
    if (e == NULL) {
        return ATREE_ERR_NOMEM;
    }
    e->pred.attr = attr;
    e->pred.kind = (uint8_t)kind;
    e->pred.op = (uint8_t)op;

    switch (operand_kind) {
    case ATREE_V_INT:
    case ATREE_V_FLOAT:
        if (scalar == NULL || scalar->kind != operand_kind) {
            st = ATREE_ERR_INVALID_ARG;
        } else {
            e->pred.operand = *scalar;
        }
        break;
    case ATREE_V_INT_LIST:
        st = own_ints(e, ints, nints);
        break;
    case ATREE_V_STRING:
        st = nstrs == 1 ? own_strs(e, strs, lens, 1) : ATREE_ERR_INVALID_ARG;
        e->pred.operand.kind = ATREE_V_STRING;
        e->pred.operand.u.s = 0;
        break;
    case ATREE_V_STRING_LIST:
        st = own_strs(e, strs, lens, nstrs);
        e->pred.operand.kind = ATREE_V_STRING_LIST;
        e->pred.operand.u.sl.data = NULL;
        e->pred.operand.u.sl.len = e->nstrs;
        break;
    case ATREE_V_UNDEFINED:
        e->pred.operand.kind = ATREE_V_UNDEFINED;
        break;
    case ATREE_V_BOOL:
    default:
        st = ATREE_ERR_INVALID_ARG;
        break;
    }
    if (st == ATREE_OK) {
        st = atree__pred_check(&tree->attrs, &e->pred);
    }
    if (st != ATREE_OK) {
        atree_expr_free(e);
        return st;
    }
    e->hash = compute_hash(e);
    *out = e;
    return ATREE_OK;
}

atree_status_t atree__expr_new_nary(enum atree__expr_kind kind, atree_expr_t **children, size_t n,
                                    atree_expr_t **out)
{
    atree_expr_t *e;
    const atree_t *tree = NULL;
    size_t i;
    uint32_t depth = 0;
    bool ok = n >= 1 && n < UINT32_MAX && children != NULL;

    if (out == NULL) {
        if (children != NULL) {
            free_children_array(children, n);
        }
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (kind == ATREE_EXPR_NOT) {
        ok = ok && n == 1;
    } else if (kind == ATREE_EXPR_XOR || kind == ATREE_EXPR_XNOR) {
        ok = ok && n == 2;
    } else if (kind != ATREE_EXPR_AND && kind != ATREE_EXPR_OR) {
        ok = false;
    }
    for (i = 0; ok && i < n; i++) {
        if (children[i] == NULL) {
            ok = false;
        } else if (tree == NULL) {
            tree = children[i]->tree;
        } else if (children[i]->tree != tree) {
            ok = false;
        }
    }
    if (!ok) {
        if (children != NULL) {
            free_children_array(children, n);
        }
        return ATREE_ERR_INVALID_ARG;
    }
    e = node_new(tree, kind);
    if (e == NULL) {
        free_children_array(children, n);
        return ATREE_ERR_NOMEM;
    }
    e->children = atree__alloc_array(&e->mem, n, sizeof *e->children);
    if (e->children == NULL) {
        free_children_array(children, n);
        atree_expr_free(e);
        return ATREE_ERR_NOMEM;
    }
    memcpy(e->children, children, n * sizeof *children);
    e->nchildren = (uint32_t)n;
    for (i = 0; i < n; i++) {
        if (children[i]->depth > depth) {
            depth = children[i]->depth;
        }
    }
    e->depth = depth == UINT32_MAX ? UINT32_MAX : depth + 1;
    e->hash = compute_hash(e);
    *out = e;
    return ATREE_OK;
}

/* ---- public builders ---------------------------------------------------- */

static atree_expr_t *leaf(const atree_t *tree, const char *attr, enum atree__pred_kind kind,
                          atree_op_t op, enum atree__vkind operand_kind,
                          const struct atree__value *scalar, const int64_t *ints, size_t nints,
                          const char *const *strs, const size_t *lens, size_t nstrs)
{
    atree_expr_t *e = NULL;
    atree_attr_id_t id;
    if (tree == NULL || attr == NULL) {
        return NULL;
    }
    id = atree_attr_lookup(tree, attr);
    if (id == ATREE_ATTR_INVALID) {
        return NULL;
    }
    if (atree__expr_new_pred(tree, id, kind, op, operand_kind, scalar, ints, nints, strs, lens,
                             nstrs, &e) != ATREE_OK) {
        return NULL;
    }
    return e;
}

atree_expr_t *atree_expr_var(const atree_t *tree, const char *attr)
{
    return leaf(tree, attr, ATREE_PRED_VAR, ATREE_OP_EQ, ATREE_V_UNDEFINED, NULL, NULL, 0, NULL,
                NULL, 0);
}

atree_expr_t *atree_expr_cmp_int(const atree_t *tree, const char *attr, atree_op_t op,
                                 int64_t value)
{
    struct atree__value v;
    v.kind = ATREE_V_INT;
    v.u.i = value;
    return leaf(tree, attr, ATREE_PRED_CMP, op, ATREE_V_INT, &v, NULL, 0, NULL, NULL, 0);
}

atree_expr_t *atree_expr_cmp_float(const atree_t *tree, const char *attr, atree_op_t op,
                                   double value)
{
    struct atree__value v;
    v.kind = ATREE_V_FLOAT;
    v.u.f = value;
    return leaf(tree, attr, ATREE_PRED_CMP, op, ATREE_V_FLOAT, &v, NULL, 0, NULL, NULL, 0);
}

atree_expr_t *atree_expr_eq_string(const atree_t *tree, const char *attr, bool equal, const char *s,
                                   size_t len)
{
    const char *strs[1];
    size_t lens[1];
    if (s == NULL) {
        return NULL;
    }
    strs[0] = s;
    lens[0] = len;
    return leaf(tree, attr, ATREE_PRED_CMP, equal ? ATREE_OP_EQ : ATREE_OP_NE, ATREE_V_STRING, NULL,
                NULL, 0, strs, lens, 1);
}

atree_expr_t *atree_expr_in_ints(const atree_t *tree, const char *attr, bool in,
                                 const int64_t *values, size_t n)
{
    return leaf(tree, attr, in ? ATREE_PRED_IN : ATREE_PRED_NOT_IN, ATREE_OP_EQ, ATREE_V_INT_LIST,
                NULL, values, n, NULL, NULL, 0);
}

atree_expr_t *atree_expr_in_strings(const atree_t *tree, const char *attr, bool in,
                                    const char *const *strings, const size_t *lens, size_t n)
{
    return leaf(tree, attr, in ? ATREE_PRED_IN : ATREE_PRED_NOT_IN, ATREE_OP_EQ,
                ATREE_V_STRING_LIST, NULL, NULL, 0, strings, lens, n);
}

static enum atree__pred_kind list_kind(atree_list_op_t op, bool *ok)
{
    *ok = true;
    switch (op) {
    case ATREE_LIST_ONE_OF:
        return ATREE_PRED_ONE_OF;
    case ATREE_LIST_NONE_OF:
        return ATREE_PRED_NONE_OF;
    case ATREE_LIST_ALL_OF:
        return ATREE_PRED_ALL_OF;
    default:
        *ok = false;
        return ATREE_PRED_ONE_OF;
    }
}

atree_expr_t *atree_expr_list_ints(const atree_t *tree, const char *attr, atree_list_op_t op,
                                   const int64_t *values, size_t n)
{
    bool ok;
    enum atree__pred_kind kind = list_kind(op, &ok);
    if (!ok) {
        return NULL;
    }
    return leaf(tree, attr, kind, ATREE_OP_EQ, ATREE_V_INT_LIST, NULL, values, n, NULL, NULL, 0);
}

atree_expr_t *atree_expr_list_strings(const atree_t *tree, const char *attr, atree_list_op_t op,
                                      const char *const *strings, const size_t *lens, size_t n)
{
    bool ok;
    enum atree__pred_kind kind = list_kind(op, &ok);
    if (!ok) {
        return NULL;
    }
    return leaf(tree, attr, kind, ATREE_OP_EQ, ATREE_V_STRING_LIST, NULL, NULL, 0, strings, lens,
                n);
}

atree_expr_t *atree_expr_null(const atree_t *tree, const char *attr, atree_null_op_t op)
{
    enum atree__pred_kind kind;
    switch (op) {
    case ATREE_IS_NULL:
        kind = ATREE_PRED_IS_NULL;
        break;
    case ATREE_IS_NOT_NULL:
        kind = ATREE_PRED_IS_NOT_NULL;
        break;
    case ATREE_IS_EMPTY:
        kind = ATREE_PRED_IS_EMPTY;
        break;
    case ATREE_IS_NOT_EMPTY:
        kind = ATREE_PRED_IS_NOT_EMPTY;
        break;
    default:
        return NULL;
    }
    return leaf(tree, attr, kind, ATREE_OP_EQ, ATREE_V_UNDEFINED, NULL, NULL, 0, NULL, NULL, 0);
}

atree_expr_t *atree_expr_true(const atree_t *tree)
{
    atree_expr_t *e = NULL;
    (void)atree__expr_new_const(tree, true, &e);
    return e;
}

atree_expr_t *atree_expr_false(const atree_t *tree)
{
    atree_expr_t *e = NULL;
    (void)atree__expr_new_const(tree, false, &e);
    return e;
}

atree_expr_t *atree_expr_and(atree_expr_t **children, size_t n)
{
    atree_expr_t *e = NULL;
    (void)atree__expr_new_nary(ATREE_EXPR_AND, children, n, &e);
    return e;
}

atree_expr_t *atree_expr_or(atree_expr_t **children, size_t n)
{
    atree_expr_t *e = NULL;
    (void)atree__expr_new_nary(ATREE_EXPR_OR, children, n, &e);
    return e;
}

atree_expr_t *atree_expr_not(atree_expr_t *child)
{
    atree_expr_t *e = NULL;
    atree_expr_t *c[1];
    c[0] = child;
    (void)atree__expr_new_nary(ATREE_EXPR_NOT, c, 1, &e);
    return e;
}

atree_expr_t *atree_expr_xor(atree_expr_t *a, atree_expr_t *b)
{
    atree_expr_t *e = NULL;
    atree_expr_t *c[2];
    c[0] = a;
    c[1] = b;
    (void)atree__expr_new_nary(ATREE_EXPR_XOR, c, 2, &e);
    return e;
}

atree_expr_t *atree_expr_xnor(atree_expr_t *a, atree_expr_t *b)
{
    atree_expr_t *e = NULL;
    atree_expr_t *c[2];
    c[0] = a;
    c[1] = b;
    (void)atree__expr_new_nary(ATREE_EXPR_XNOR, c, 2, &e);
    return e;
}

/* ---- normalization (paper §5.2.1 zero suppression filter, PLAN §4.3) ---- */

/* The normalized copy of an expression is short-lived (one insert) and
 * built bottom-up, so every node, child array, list and string literal is
 * bump-allocated from one arena and released together. This removes the
 * per-leaf allocations that otherwise dominate the insert of a large
 * expression. Discarded intermediates (flattened connectives, duplicate
 * operands, folded constants) are simply left in the arena. */

#define ARENA_ALIGN ((size_t)16)
#define ARENA_CHUNK ((size_t)4096)

struct arena_chunk {
    struct arena_chunk *next;
    size_t size; /* bytes of the whole chunk, header included */
};

struct atree__arena {
    struct atree__mem mem;
    struct arena_chunk *head;
    size_t used; /* bytes handed out from head, header included */
};

static size_t round_align(size_t n)
{
    return (n + ARENA_ALIGN - 1) / ARENA_ALIGN * ARENA_ALIGN;
}

static void *arena_alloc(struct atree__arena *a, size_t size)
{
    size_t need = round_align(size);
    size_t hdr = round_align(sizeof(struct arena_chunk));
    char *p;
    if (size == 0) {
        size = 1;
        need = ARENA_ALIGN;
    }
    if (a->head == NULL || a->used + need > a->head->size) {
        size_t total;
        struct arena_chunk *c;
        if (atree__add_overflows(hdr, need, &total)) {
            return NULL;
        }
        if (total < ARENA_CHUNK) {
            total = ARENA_CHUNK;
        }
        c = atree__alloc(&a->mem, total);
        if (c == NULL) {
            return NULL;
        }
        c->next = a->head;
        c->size = total;
        a->head = c;
        a->used = hdr;
    }
    p = (char *)a->head + a->used;
    a->used += need;
    return p; /* not zeroed: every caller writes the whole block */
}

static void arena_free(struct atree__arena *a)
{
    struct atree__mem mem = a->mem;
    struct arena_chunk *c = a->head;
    while (c != NULL) {
        struct arena_chunk *next = c->next;
        atree__free(&mem, c, c->size);
        c = next;
    }
    atree__free(&mem, a, sizeof *a);
}

void atree__arena_free(struct atree__arena *a)
{
    if (a != NULL) {
        arena_free(a);
    }
}

void atree__arena_reset(struct atree__arena *a)
{
    struct arena_chunk *keep = NULL;
    struct arena_chunk *c = a->head;
    while (c != NULL) {
        struct arena_chunk *next = c->next;
        if (keep == NULL || c->size > keep->size) {
            if (keep != NULL) {
                atree__free(&a->mem, keep, keep->size);
            }
            keep = c;
        } else {
            atree__free(&a->mem, c, c->size);
        }
        c = next;
    }
    a->head = keep;
    if (keep != NULL) {
        keep->next = NULL;
    }
    a->used = round_align(sizeof(struct arena_chunk));
}

static atree_status_t arena_new(const atree_t *tree, struct atree__arena **out)
{
    struct atree__mem mem;
    struct atree__arena *a;
    atree__mem_init(&mem, &tree->mem.a);
    a = atree__zalloc(&mem, sizeof *a);
    if (a == NULL) {
        return ATREE_ERR_NOMEM;
    }
    a->mem = mem;
    *out = a;
    return ATREE_OK;
}

static atree_expr_t *norm_node(struct atree__arena *a, const atree_t *tree,
                               enum atree__expr_kind kind)
{
    atree_expr_t *e = arena_alloc(a, sizeof *e);
    if (e == NULL) {
        return NULL;
    }
    memset(e, 0, sizeof *e);
    e->arena = a;
    e->tree = tree;
    e->kind = (uint8_t)kind;
    e->depth = 1;
    e->pred.operand.kind = ATREE_V_UNDEFINED;
    return e;
}

static atree_status_t norm_const(struct atree__arena *a, const atree_t *tree, bool value,
                                 atree_expr_t **out)
{
    atree_expr_t *e = norm_node(a, tree, value ? ATREE_EXPR_TRUE : ATREE_EXPR_FALSE);
    if (e == NULL) {
        return ATREE_ERR_NOMEM;
    }
    e->hash = compute_hash(e);
    *out = e;
    return ATREE_OK;
}

/* Copies a leaf into the arena, negating it when neg is set. */
static atree_status_t norm_pred(struct atree__arena *a, const atree_expr_t *e, bool neg,
                                atree_expr_t **out)
{
    atree_expr_t *n = norm_node(a, e->tree, ATREE_EXPR_PRED);
    uint32_t i;
    if (n == NULL) {
        return ATREE_ERR_NOMEM;
    }
    n->pred = e->pred;
    if (n->pred.operand.kind == ATREE_V_INT_LIST && n->pred.operand.u.il.len > 0) {
        size_t bytes = (size_t)n->pred.operand.u.il.len * sizeof(int64_t);
        int64_t *data = arena_alloc(a, bytes);
        if (data == NULL) {
            return ATREE_ERR_NOMEM;
        }
        memcpy(data, e->pred.operand.u.il.data, bytes);
        n->pred.operand.u.il.data = data;
    }
    if (e->nstrs > 0) {
        n->strs = arena_alloc(a, (size_t)e->nstrs * sizeof *n->strs);
        if (n->strs == NULL) {
            return ATREE_ERR_NOMEM;
        }
        for (i = 0; i < e->nstrs; i++) {
            char *d = arena_alloc(a, (size_t)e->strs[i].len + 1);
            if (d == NULL) {
                return ATREE_ERR_NOMEM;
            }
            memcpy(d, e->strs[i].data, (size_t)e->strs[i].len + 1);
            n->strs[i].data = d;
            n->strs[i].len = e->strs[i].len;
        }
        n->nstrs = e->nstrs;
    }
    if (neg) {
        atree__pred_negate(&n->pred);
        n->hash = compute_hash(n);
    } else {
        n->hash = e->hash;
    }
    *out = n;
    return ATREE_OK;
}

static atree_status_t normalize_rec(struct atree__arena *a, const atree_expr_t *e, bool neg,
                                    atree_expr_t **out);

/* Combines already-normalized operands under AND/OR: flattens same-kind
 * children, folds constants, sorts canonically, drops duplicates, collapses
 * a single child. */
static atree_status_t combine(struct atree__arena *a, const atree_t *tree,
                              enum atree__expr_kind kind, atree_expr_t **parts, uint32_t nparts,
                              atree_expr_t **out)
{
    enum atree__expr_kind absorbing = kind == ATREE_EXPR_AND ? ATREE_EXPR_FALSE : ATREE_EXPR_TRUE;
    enum atree__expr_kind neutral = kind == ATREE_EXPR_AND ? ATREE_EXPR_TRUE : ATREE_EXPR_FALSE;
    atree_expr_t **flat;
    uint32_t cap = 0;
    uint32_t n = 0;
    uint32_t i;
    uint32_t m;
    uint32_t depth = 0;
    atree_expr_t *node;

    *out = NULL;
    for (i = 0; i < nparts; i++) {
        if (parts[i]->kind == absorbing) {
            *out = parts[i];
            return ATREE_OK;
        }
        cap += parts[i]->kind == kind ? parts[i]->nchildren : 1;
    }
    flat = arena_alloc(a, (size_t)(cap == 0 ? 1 : cap) * sizeof *flat);
    if (flat == NULL) {
        return ATREE_ERR_NOMEM;
    }
    for (i = 0; i < nparts; i++) {
        atree_expr_t *c = parts[i];
        if (c->kind == neutral) {
            continue;
        }
        if (c->kind == kind) {
            for (m = 0; m < c->nchildren; m++) {
                flat[n++] = c->children[m]; /* flatten: adopt the grandchildren */
            }
        } else {
            flat[n++] = c;
        }
    }
    if (n > 1) {
        qsort(flat, n, sizeof *flat, expr_qsort_cmp);
        m = 0;
        for (i = 0; i < n; i++) {
            if (m == 0 || !atree__expr_equal(flat[m - 1], flat[i])) {
                flat[m++] = flat[i];
            }
        }
        n = m;
    }
    if (n == 0) {
        return norm_const(a, tree, neutral == ATREE_EXPR_TRUE, out);
    }
    if (n == 1) {
        *out = flat[0];
        return ATREE_OK;
    }
    node = norm_node(a, tree, kind);
    if (node == NULL) {
        return ATREE_ERR_NOMEM;
    }
    node->children = flat;
    node->nchildren = n;
    for (i = 0; i < n; i++) {
        if (flat[i]->depth > depth) {
            depth = flat[i]->depth;
        }
    }
    node->depth = depth == UINT32_MAX ? UINT32_MAX : depth + 1;
    node->hash = compute_hash(node);
    *out = node;
    return ATREE_OK;
}

/* Normalizes each part with its own negation flag, then combines. */
static atree_status_t norm_nary(struct atree__arena *a, const atree_t *tree,
                                enum atree__expr_kind kind, const atree_expr_t *const *srcs,
                                const bool *negs, uint32_t n, atree_expr_t **out)
{
    atree_expr_t **parts;
    atree_status_t st = ATREE_OK;
    uint32_t i;

    *out = NULL;
    parts = arena_alloc(a, (size_t)(n == 0 ? 1 : n) * sizeof *parts);
    if (parts == NULL) {
        return ATREE_ERR_NOMEM;
    }
    for (i = 0; st == ATREE_OK && i < n; i++) {
        st = normalize_rec(a, srcs[i], negs[i], &parts[i]);
    }
    if (st != ATREE_OK) {
        return st;
    }
    return combine(a, tree, kind, parts, n, out);
}

static atree_status_t normalize_rec(struct atree__arena *a, const atree_expr_t *e, bool neg,
                                    atree_expr_t **out)
{
    const atree_t *tree = e->tree;
    atree_status_t st;

    *out = NULL;
    switch ((enum atree__expr_kind)e->kind) {
    case ATREE_EXPR_TRUE:
        return norm_const(a, tree, !neg, out);
    case ATREE_EXPR_FALSE:
        return norm_const(a, tree, neg, out);
    case ATREE_EXPR_PRED:
        return norm_pred(a, e, neg, out);
    case ATREE_EXPR_NOT:
        return normalize_rec(a, e->children[0], !neg, out);

    case ATREE_EXPR_AND:
    case ATREE_EXPR_OR: {
        /* De Morgan: a negated AND becomes an OR of negated children. */
        enum atree__expr_kind k = (enum atree__expr_kind)e->kind;
        bool *negs;
        uint32_t i;
        if (neg) {
            k = k == ATREE_EXPR_AND ? ATREE_EXPR_OR : ATREE_EXPR_AND;
        }
        negs = arena_alloc(a, (size_t)(e->nchildren == 0 ? 1 : e->nchildren) * sizeof *negs);
        if (negs == NULL) {
            return ATREE_ERR_NOMEM;
        }
        for (i = 0; i < e->nchildren; i++) {
            negs[i] = neg;
        }
        return norm_nary(a, tree, k, (const atree_expr_t *const *)e->children, negs, e->nchildren,
                         out);
    }

    case ATREE_EXPR_XOR:
    case ATREE_EXPR_XNOR: {
        /* a xor b  = (a and not b) or (not a and b)
         * a xnor b = (a and b) or (not a and not b); negation swaps the two. */
        bool xnor = (e->kind == ATREE_EXPR_XNOR) != neg;
        const atree_expr_t *ab[2];
        bool n1[2];
        bool n2[2];
        atree_expr_t *parts[2];

        ab[0] = e->children[0];
        ab[1] = e->children[1];
        n1[0] = false;
        n1[1] = !xnor;
        n2[0] = true;
        n2[1] = xnor;
        st = norm_nary(a, tree, ATREE_EXPR_AND, ab, n1, 2, &parts[0]);
        if (st != ATREE_OK) {
            return st;
        }
        st = norm_nary(a, tree, ATREE_EXPR_AND, ab, n2, 2, &parts[1]);
        if (st != ATREE_OK) {
            return st;
        }
        return combine(a, tree, ATREE_EXPR_OR, parts, 2, out);
    }

    default:
        return ATREE_ERR_INVALID_ARG;
    }
}

atree_status_t atree__arena_new(const atree_t *tree, struct atree__arena **out)
{
    return arena_new(tree, out);
}

atree_status_t atree__expr_normalize_in(struct atree__arena *a, const atree_expr_t *e,
                                        size_t max_depth, atree_expr_t **out)
{
    if (e == NULL || out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (e->depth > max_depth) {
        return ATREE_ERR_TOO_DEEP;
    }
    return normalize_rec(a, e, false, out);
}

atree_status_t atree__expr_normalize(const atree_expr_t *e, size_t max_depth, atree_expr_t **out)
{
    struct atree__arena *a;
    atree_status_t st;
    if (e == NULL || out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (e->depth > max_depth) {
        return ATREE_ERR_TOO_DEEP;
    }
    st = arena_new(e->tree, &a);
    if (st != ATREE_OK) {
        return st;
    }
    st = atree__expr_normalize_in(a, e, max_depth, out);
    if (st != ATREE_OK) {
        arena_free(a);
        *out = NULL;
    }
    return st;
}

/* ---- resolving string literals ------------------------------------------ */

static atree_status_t resolve(const atree_expr_t *e, struct atree__mem *m,
                              const struct atree__strtab *ro, struct atree__strtab *rw,
                              struct atree__pred *out, uint32_t *dropped)
{
    const struct atree__value *src = &e->pred.operand;
    atree_status_t st;
    uint32_t i;
    uint32_t kept = 0;

    out->attr = e->pred.attr;
    out->kind = e->pred.kind;
    out->op = e->pred.op;
    out->operand = *src;
    *dropped = 0;

    switch (src->kind) {
    case ATREE_V_INT_LIST:
        return atree__value_copy(m, &out->operand, src);

    case ATREE_V_STRING: {
        uint32_t id;
        if (rw != NULL) {
            st = atree__strtab_intern(m, rw, e->strs[0].data, e->strs[0].len, &id);
            if (st != ATREE_OK) {
                return st;
            }
        } else {
            id = atree__strtab_lookup(ro, e->strs[0].data, e->strs[0].len);
            if (id == ATREE_STR_UNKNOWN) {
                *dropped = 1;
            }
        }
        out->operand.u.s = id;
        return ATREE_OK;
    }

    case ATREE_V_STRING_LIST: {
        uint32_t *ids;
        out->operand.u.sl.data = NULL;
        out->operand.u.sl.len = 0;
        if (e->nstrs == 0) {
            return ATREE_OK;
        }
        ids = atree__alloc_array(m, e->nstrs, sizeof *ids);
        if (ids == NULL) {
            return ATREE_ERR_NOMEM;
        }
        for (i = 0; i < e->nstrs; i++) {
            uint32_t id;
            if (rw != NULL) {
                st = atree__strtab_intern(m, rw, e->strs[i].data, e->strs[i].len, &id);
                if (st != ATREE_OK) {
                    atree__free_array(m, ids, e->nstrs, sizeof *ids);
                    return st;
                }
            } else {
                id = atree__strtab_lookup(ro, e->strs[i].data, e->strs[i].len);
                if (id == ATREE_STR_UNKNOWN) {
                    (*dropped)++;
                    continue;
                }
            }
            ids[kept++] = id;
        }
        kept = atree__sort_unique_u32(ids, kept);
        if (kept == 0) {
            atree__free_array(m, ids, e->nstrs, sizeof *ids);
            return ATREE_OK;
        }
        if (kept < e->nstrs) {
            uint32_t *shrunk = atree__realloc_array(m, ids, e->nstrs, kept, sizeof *ids);
            if (shrunk == NULL) {
                atree__free_array(m, ids, e->nstrs, sizeof *ids);
                return ATREE_ERR_NOMEM;
            }
            ids = shrunk;
        }
        out->operand.u.sl.data = ids;
        out->operand.u.sl.len = kept;
        return ATREE_OK;
    }

    case ATREE_V_UNDEFINED:
    case ATREE_V_BOOL:
    case ATREE_V_INT:
    case ATREE_V_FLOAT:
    default:
        return ATREE_OK;
    }
}

atree_status_t atree__expr_pred_lookup(const atree_expr_t *e, struct atree__mem *m,
                                       const struct atree__strtab *strings, struct atree__pred *out,
                                       uint32_t *dropped)
{
    uint32_t d = 0;
    atree_status_t st;
    if (e == NULL || e->kind != ATREE_EXPR_PRED) {
        return ATREE_ERR_INVALID_ARG;
    }
    st = resolve(e, m, strings, NULL, out, &d);
    if (dropped != NULL) {
        *dropped = d;
    }
    return st;
}

atree_status_t atree__expr_pred_intern(const atree_expr_t *e, struct atree__mem *m,
                                       struct atree__strtab *strings, struct atree__pred *out)
{
    uint32_t d = 0;
    if (e == NULL || e->kind != ATREE_EXPR_PRED) {
        return ATREE_ERR_INVALID_ARG;
    }
    return resolve(e, m, strings, strings, out, &d);
}

uint64_t atree__expr_leaf_hash(const atree_expr_t *e)
{
    const struct atree__pred *p = &e->pred;
    const struct atree__value *v = &p->operand;
    uint64_t h = atree__hash_u64(UINT64_C(0x70726564) ^ p->attr);
    uint64_t sum = 0;
    uint32_t i;
    h = atree__hash_combine(h, ((uint64_t)p->kind << 8) | p->op);
    h = atree__hash_combine(h, (uint64_t)v->kind + UINT64_C(0x51ed270b));
    switch (v->kind) {
    case ATREE_V_UNDEFINED:
        break;
    case ATREE_V_BOOL:
        h = atree__hash_combine(h, v->u.b ? 1 : 0);
        break;
    case ATREE_V_INT:
        h = atree__hash_combine(h, (uint64_t)v->u.i);
        break;
    case ATREE_V_FLOAT:
        h = atree__hash_combine(h, atree__double_bits(v->u.f));
        break;
    case ATREE_V_STRING:
        h = atree__hash_combine(h,
                                e->nstrs > 0 ? atree__hash_bytes(e->strs[0].data, e->strs[0].len)
                                             : atree__hash_bytes(NULL, 0));
        break;
    case ATREE_V_INT_LIST:
        h = atree__hash_combine(h, v->u.il.len);
        for (i = 0; i < v->u.il.len; i++) {
            h = atree__hash_combine(h, (uint64_t)v->u.il.data[i]);
        }
        break;
    case ATREE_V_STRING_LIST:
        h = atree__hash_combine(h, e->nstrs);
        for (i = 0; i < e->nstrs; i++) {
            sum += atree__hash_u64(atree__hash_bytes(e->strs[i].data, e->strs[i].len));
        }
        h = atree__hash_combine(h, sum);
        break;
    default:
        break;
    }
    return h;
}

/* Position of the string with these bytes in e->strs (sorted bytewise,
 * unique), or UINT32_MAX. */
static uint32_t find_raw(const atree_expr_t *e, const char *s, uint32_t len)
{
    uint32_t lo = 0;
    uint32_t hi = e->nstrs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const struct atree__rawstr *r = &e->strs[mid];
        uint32_t n = r->len < len ? r->len : len;
        int c = n == 0 ? 0 : memcmp(r->data, s, n);
        if (c == 0) {
            c = (r->len > len) - (r->len < len);
        }
        if (c == 0) {
            return mid;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return UINT32_MAX;
}

bool atree__expr_leaf_equals(const atree_expr_t *e, const struct atree__pred *p,
                             const struct atree__strtab *strings)
{
    const struct atree__value *a = &e->pred.operand;
    const struct atree__value *b = &p->operand;
    uint32_t i;
    if (e->kind != ATREE_EXPR_PRED || e->pred.attr != p->attr || e->pred.kind != p->kind ||
        e->pred.op != p->op || a->kind != b->kind) {
        return false;
    }
    switch (b->kind) {
    case ATREE_V_STRING: {
        uint32_t len = 0;
        const char *s = atree__strtab_get(strings, b->u.s, &len);
        return s != NULL && e->nstrs == 1 && e->strs[0].len == len &&
            (len == 0 || memcmp(e->strs[0].data, s, len) == 0);
    }
    case ATREE_V_STRING_LIST:
        if (b->u.sl.len != e->nstrs) {
            return false;
        }
        for (i = 0; i < b->u.sl.len; i++) {
            uint32_t len = 0;
            const char *s = atree__strtab_get(strings, b->u.sl.data[i], &len);
            if (s == NULL || find_raw(e, s, len) == UINT32_MAX) {
                return false;
            }
        }
        return true; /* both sides unique and equal in size: a bijection */
    case ATREE_V_UNDEFINED:
    case ATREE_V_BOOL:
    case ATREE_V_INT:
    case ATREE_V_FLOAT:
    case ATREE_V_INT_LIST:
    default:
        return atree__value_equal(a, b);
    }
}

const char *atree__expr_str_resolver(const void *ctx, uint32_t ref, uint32_t *len)
{
    const atree_expr_t *e = (const atree_expr_t *)ctx;
    if (ref >= e->nstrs) {
        return NULL;
    }
    if (len != NULL) {
        *len = e->strs[ref].len;
    }
    return e->strs[ref].data;
}

/* ---- reference evaluation (paper §3.2, Table 2) -------------------------- */

static atree_tri_t tri_not(atree_tri_t t)
{
    return t == ATREE_UNDEFINED ? ATREE_UNDEFINED : (t == ATREE_TRUE ? ATREE_FALSE : ATREE_TRUE);
}

/* Evaluates a leaf whose string literals may be unknown to the tree. An
 * unknown literal can match no event string, so it behaves as a value that
 * is present in no event: equality with it is false, membership tests skip
 * it, and `all of` cannot be satisfied when one is required. */
static atree_tri_t eval_pred(const atree_expr_t *e, const atree_event_t *ev)
{
    const struct atree__value *v = atree__event_value(ev, e->pred.attr);
    struct atree__mem tmp;
    struct atree__pred p;
    uint32_t dropped = 0;
    atree_tri_t r;

    atree__mem_init(&tmp, &e->tree->mem.a);
    if (atree__expr_pred_lookup(e, &tmp, &e->tree->strings, &p, &dropped) != ATREE_OK) {
        return ATREE_UNDEFINED; /* out of memory: no answer */
    }
    if (v->kind == ATREE_V_UNDEFINED || dropped == 0) {
        r = atree__pred_eval(&p, v);
    } else if (p.operand.kind == ATREE_V_STRING) {
        r = p.op == ATREE_OP_EQ ? ATREE_FALSE : ATREE_TRUE;
    } else {
        switch ((enum atree__pred_kind)p.kind) {
        case ATREE_PRED_ALL_OF:
            r = ATREE_FALSE;
            break;
        case ATREE_PRED_NOT_ALL_OF:
            r = ATREE_TRUE;
            break;
        case ATREE_PRED_IN:
        case ATREE_PRED_ONE_OF:
            r = p.operand.u.sl.len == 0 ? ATREE_FALSE : atree__pred_eval(&p, v);
            break;
        case ATREE_PRED_NOT_IN:
        case ATREE_PRED_NONE_OF:
            r = p.operand.u.sl.len == 0 ? ATREE_TRUE : atree__pred_eval(&p, v);
            break;
        case ATREE_PRED_VAR:
        case ATREE_PRED_NOT_VAR:
        case ATREE_PRED_CMP:
        case ATREE_PRED_IS_NULL:
        case ATREE_PRED_IS_NOT_NULL:
        case ATREE_PRED_IS_EMPTY:
        case ATREE_PRED_IS_NOT_EMPTY:
        case ATREE_PRED_KIND_COUNT:
        default:
            r = atree__pred_eval(&p, v);
            break;
        }
    }
    atree__pred_free(&tmp, &p);
    return r;
}

atree_tri_t atree_expr_eval(const atree_expr_t *e, const atree_event_t *ev)
{
    uint32_t i;
    atree_tri_t r;
    atree_tri_t c;

    if (e == NULL || ev == NULL || ev->tree != e->tree) {
        return ATREE_UNDEFINED;
    }
    switch ((enum atree__expr_kind)e->kind) {
    case ATREE_EXPR_TRUE:
        return ATREE_TRUE;
    case ATREE_EXPR_FALSE:
        return ATREE_FALSE;
    case ATREE_EXPR_PRED:
        return eval_pred(e, ev);
    case ATREE_EXPR_NOT:
        return tri_not(atree_expr_eval(e->children[0], ev));
    case ATREE_EXPR_AND:
        r = ATREE_TRUE;
        for (i = 0; i < e->nchildren; i++) {
            c = atree_expr_eval(e->children[i], ev);
            if (c == ATREE_FALSE) {
                return ATREE_FALSE;
            }
            if (c == ATREE_UNDEFINED) {
                r = ATREE_UNDEFINED;
            }
        }
        return r;
    case ATREE_EXPR_OR:
        r = ATREE_FALSE;
        for (i = 0; i < e->nchildren; i++) {
            c = atree_expr_eval(e->children[i], ev);
            if (c == ATREE_TRUE) {
                return ATREE_TRUE;
            }
            if (c == ATREE_UNDEFINED) {
                r = ATREE_UNDEFINED;
            }
        }
        return r;
    case ATREE_EXPR_XOR:
    case ATREE_EXPR_XNOR: {
        atree_tri_t a = atree_expr_eval(e->children[0], ev);
        atree_tri_t b = atree_expr_eval(e->children[1], ev);
        bool x;
        if (a == ATREE_UNDEFINED || b == ATREE_UNDEFINED) {
            return ATREE_UNDEFINED;
        }
        x = (a == ATREE_TRUE) != (b == ATREE_TRUE);
        if (e->kind == ATREE_EXPR_XNOR) {
            x = !x;
        }
        return x ? ATREE_TRUE : ATREE_FALSE;
    }
    default:
        return ATREE_UNDEFINED;
    }
}

/* ---- printing ----------------------------------------------------------- */

static bool is_connective(const atree_expr_t *e)
{
    return e->kind == ATREE_EXPR_AND || e->kind == ATREE_EXPR_OR || e->kind == ATREE_EXPR_XOR ||
        e->kind == ATREE_EXPR_XNOR;
}

static void print_rec(const atree_expr_t *e, struct atree__writer *w)
{
    const char *sep;
    uint32_t i;
    switch ((enum atree__expr_kind)e->kind) {
    case ATREE_EXPR_TRUE:
        atree__write_cstr(w, "true");
        return;
    case ATREE_EXPR_FALSE:
        atree__write_cstr(w, "false");
        return;
    case ATREE_EXPR_PRED:
        atree__pred_print(&e->pred, &e->tree->attrs, atree__expr_str_resolver, e, w);
        return;
    case ATREE_EXPR_NOT:
        atree__write_cstr(w, "not ");
        if (is_connective(e->children[0])) {
            atree__write(w, "(", 1);
            print_rec(e->children[0], w);
            atree__write(w, ")", 1);
        } else {
            print_rec(e->children[0], w);
        }
        return;
    case ATREE_EXPR_AND:
        sep = " and ";
        break;
    case ATREE_EXPR_OR:
        sep = " or ";
        break;
    case ATREE_EXPR_XOR:
        sep = " xor ";
        break;
    case ATREE_EXPR_XNOR:
        sep = " xnor ";
        break;
    default:
        atree__write_cstr(w, "<invalid expression>");
        return;
    }
    for (i = 0; i < e->nchildren; i++) {
        if (i > 0) {
            atree__write_cstr(w, sep);
        }
        if (is_connective(e->children[i])) {
            atree__write(w, "(", 1);
            print_rec(e->children[i], w);
            atree__write(w, ")", 1);
        } else {
            print_rec(e->children[i], w);
        }
    }
}

atree_status_t atree_expr_print(const atree_expr_t *e, atree_write_fn fn, void *ctx)
{
    struct atree__writer w;
    if (e == NULL || fn == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    atree__writer_init(&w, fn, ctx);
    atree__rdlock(e->tree);
    print_rec(e, &w);
    atree__rdunlock(e->tree);
    return atree__writer_status(&w);
}

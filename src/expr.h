/*
 * Expressions: the caller-facing Boolean expression tree (paper §3.1) that
 * atree_insert_expr() turns into DAG nodes, plus normalization (§4.3 of the
 * plan: NOT push-down, De Morgan, XOR/XNOR expansion, flattening,
 * deduplication, constant folding, single-child collapse).
 *
 * Expressions are built against a tree (for attribute validation and the
 * allocator) through `const atree_t *`, so they must never write to the
 * tree. String literals therefore stay as raw bytes inside the expression
 * and are interned only at insert time (write path) or resolved by lookup
 * for the reference evaluator (read path).
 *
 * Every node carries its own allocator counters (struct atree__mem) so that
 * all of a node's allocations and frees balance within the node.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_EXPR_H
#define ATREE_EXPR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atree.h"

#include "alloc.h"
#include "predicate.h"
#include "strtab.h"
#include "writer.h"

enum atree__expr_kind {
    ATREE_EXPR_TRUE = 0,
    ATREE_EXPR_FALSE,
    ATREE_EXPR_PRED,
    ATREE_EXPR_AND,
    ATREE_EXPR_OR,
    ATREE_EXPR_NOT,
    ATREE_EXPR_XOR,
    ATREE_EXPR_XNOR
};

struct atree__rawstr {
    char *data; /* owned, NUL-terminated */
    uint32_t len;
};

struct atree__arena;

struct atree_expr {
    struct atree__mem mem;      /* owns children[], pred int list, strs[] and their bytes */
    struct atree__arena *arena; /* normalized trees: every node and operand lives in
                                   one arena owned by the root; NULL for built nodes */
    const atree_t *tree;
    uint8_t kind;   /* enum atree__expr_kind */
    uint32_t depth; /* 1 for leaves and constants */
    uint64_t hash;  /* structural hash; children in stored order; for a PRED
                       leaf atree__expr_leaf_hash (the tree's leaf identity) */
    uint32_t nchildren;
    atree_expr_t **children;
    struct atree__pred pred;    /* ATREE_EXPR_PRED. String operands are unresolved:
                                   u.s unused, u.sl.data == NULL, u.sl.len == nstrs */
    struct atree__rawstr *strs; /* string literal(s), sorted bytewise, unique */
    uint32_t nstrs;
    /* Insert-time lookup state (tree.c, Alg. 4 lines 1-4), set on the
     * normalized copy only: the flat content hash matching the tree's
     * content table, whether it could be computed (every string literal is
     * known to the tree), and the node this subexpression resolved to. */
    uint64_t chash;
    uint32_t resolved;    /* node id when lookup_state == 1 */
    uint8_t lookup_state; /* 0 not tried, 1 found, 2 absent */
    uint8_t chash_ok;
};

/* ---- construction (used by the public builders, the parser and tests) --- */

/* Leaf. operand_kind selects which operand arguments are read: ATREE_V_INT /
 * ATREE_V_FLOAT take *scalar; ATREE_V_INT_LIST takes ints[nints];
 * ATREE_V_STRING takes strs[0]; ATREE_V_STRING_LIST takes strs[nstrs];
 * ATREE_V_UNDEFINED takes none. lens may be NULL (NUL-terminated strings) and
 * an entry may be SIZE_MAX. Everything is copied; lists are sorted and
 * deduplicated; the predicate is type-checked against the tree. */
atree_status_t atree__expr_new_pred(const atree_t *tree, atree_attr_id_t attr,
                                    enum atree__pred_kind kind, atree_op_t op,
                                    enum atree__vkind operand_kind,
                                    const struct atree__value *scalar, const int64_t *ints,
                                    size_t nints, const char *const *strs, const size_t *lens,
                                    size_t nstrs, atree_expr_t **out);
atree_status_t atree__expr_new_const(const atree_t *tree, bool value, atree_expr_t **out);
/* Connective over n >= 1 children (NOT: exactly 1; XOR/XNOR: exactly 2),
 * all from the same tree. Takes ownership of the children on success and on
 * failure (they are freed on failure). */
atree_status_t atree__expr_new_nary(enum atree__expr_kind kind, atree_expr_t **children, size_t n,
                                    atree_expr_t **out);

/* ---- normalization ------------------------------------------------------ */

/* Produces an equivalent expression containing only TRUE/FALSE (at the root
 * only), PRED, AND and OR, with n-ary flattened connectives whose children
 * are canonically ordered and unique. Fails with ATREE_ERR_TOO_DEEP when
 * e->depth > max_depth. */
/* The result is a self-contained tree allocated from a single arena: free it
 * with atree_expr_free on its root only (children are not individually
 * freeable). */
atree_status_t atree__expr_normalize(const atree_expr_t *e, size_t max_depth, atree_expr_t **out);

/* Same, into a caller-owned arena, which the caller later resets or frees;
 * the result must not be passed to atree_expr_free. Lets a writer reuse one
 * arena across inserts so that normalizing allocates nothing in steady
 * state. */
atree_status_t atree__arena_new(const atree_t *tree, struct atree__arena **out);
void atree__arena_free(struct atree__arena *a);
/* Forgets everything allocated so far, keeping one chunk for reuse. */
void atree__arena_reset(struct atree__arena *a);
atree_status_t atree__expr_normalize_in(struct atree__arena *a, const atree_expr_t *e,
                                        size_t max_depth, atree_expr_t **out);

/* Structural identity. Children are compared in stored order (canonical
 * after normalization). Raw strings compare by bytes. */
bool atree__expr_equal(const atree_expr_t *a, const atree_expr_t *b);
/* Total order consistent with atree__expr_equal; used for canonical child order. */
int atree__expr_cmp(const atree_expr_t *a, const atree_expr_t *b);

/* ---- resolving string literals ------------------------------------------ */

/* Builds a resolved predicate for a PRED leaf: int lists copied, string
 * literals looked up (never interned). Unknown strings are dropped and
 * counted in *dropped; a string-list operand may end up empty (len 0,
 * data NULL). Lists are allocated from m; release with atree__pred_free(m). */
atree_status_t atree__expr_pred_lookup(const atree_expr_t *e, struct atree__mem *m,
                                       const struct atree__strtab *strings, struct atree__pred *out,
                                       uint32_t *dropped);

/* atree__pred_content_hash of a PRED leaf computed from its raw literals:
 * no string table access, no allocation. */
uint64_t atree__expr_leaf_hash(const atree_expr_t *e);
/* Whether a PRED leaf denotes exactly the resolved predicate p (string
 * literals compared by bytes through the string table). */
bool atree__expr_leaf_equals(const atree_expr_t *e, const struct atree__pred *p,
                             const struct atree__strtab *strings);

/* Same, but interns the literals (write path, M4). */
atree_status_t atree__expr_pred_intern(const atree_expr_t *e, struct atree__mem *m,
                                       struct atree__strtab *strings, struct atree__pred *out);

/* Resolver for atree__pred_print over an expression's raw strings (ctx = e). */
const char *atree__expr_str_resolver(const void *ctx, uint32_t ref, uint32_t *len);

#endif /* ATREE_EXPR_H */

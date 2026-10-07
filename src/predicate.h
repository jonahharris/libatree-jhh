/*
 * Predicates: the leaves of the A-Tree. A predicate is <attribute, operator,
 * operand> (paper §3.1) with the operand already normalized (sorted unique
 * lists, strings interned, integer literals promoted for float attributes).
 *
 * Evaluation is three-valued (paper §3.2): a predicate whose attribute is
 * undefined in the event evaluates to ATREE_UNDEFINED, except `is null`
 * (true) and `is not null` (false).
 *
 * Every kind has an exact negation (so NOT can be pushed into leaves, paper
 * §5.2.1): VAR<->NOT_VAR, < <-> >=, <= <-> >, = <-> <>, IN<->NOT_IN,
 * ONE_OF<->NONE_OF, ALL_OF<->NOT_ALL_OF, IS_NULL<->IS_NOT_NULL,
 * IS_EMPTY<->IS_NOT_EMPTY.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_PREDICATE_H
#define ATREE_PREDICATE_H

#include <stdbool.h>
#include <stdint.h>

#include "atree.h"

#include "attr.h"
#include "strtab.h"
#include "value.h"
#include "writer.h"

enum atree__pred_kind {
    ATREE_PRED_VAR = 0, /* bool attribute is true                         */
    ATREE_PRED_NOT_VAR, /* bool attribute is false                        */
    ATREE_PRED_CMP,     /* attr <op> literal; op is atree_op_t            */
    ATREE_PRED_IN,      /* scalar attr is in literal list                 */
    ATREE_PRED_NOT_IN,
    ATREE_PRED_ONE_OF,     /* list attr intersects literal list              */
    ATREE_PRED_NONE_OF,    /* list attr disjoint from literal list           */
    ATREE_PRED_ALL_OF,     /* list attr contains every literal               */
    ATREE_PRED_NOT_ALL_OF, /* internal: negation of ALL_OF                   */
    ATREE_PRED_IS_NULL,    /* scalar attr undefined in the event             */
    ATREE_PRED_IS_NOT_NULL,
    ATREE_PRED_IS_EMPTY, /* list attr defined and empty                    */
    ATREE_PRED_IS_NOT_EMPTY,
    ATREE_PRED_KIND_COUNT
};

struct atree__pred {
    atree_attr_id_t attr;
    uint8_t kind;                /* enum atree__pred_kind */
    uint8_t op;                  /* atree_op_t, meaningful for ATREE_PRED_CMP only */
    struct atree__value operand; /* ATREE_V_UNDEFINED when the kind has none */
};

/* Type rules only (PLAN §3), no mutation: attribute exists, kind/op valid,
 * operand kind matches the attribute type (an integer literal is accepted
 * for a float attribute when exactly representable), lists non-empty, floats
 * finite. Works on unresolved string lists (data == NULL, len = count). */
atree_status_t atree__pred_typecheck(const struct atree__attrs *attrs, const struct atree__pred *p);

/* atree__pred_typecheck plus normalization in place: promotes an integer
 * literal to double for a float attribute, sorts and deduplicates lists,
 * clears the operand of kinds that have none. Errors: ATREE_ERR_INVALID_ARG,
 * ATREE_ERR_TYPE_MISMATCH, ATREE_ERR_INVALID_LITERAL. */
atree_status_t atree__pred_check(const struct atree__attrs *attrs, struct atree__pred *p);

/* Replaces p by its exact logical negation. */
void atree__pred_negate(struct atree__pred *p);

/* Three-valued evaluation against the event's value for p->attr. */
atree_tri_t atree__pred_eval(const struct atree__pred *p, const struct atree__value *v);

uint64_t atree__pred_hash(const struct atree__pred *p);
/* Hash over the predicate's content with string literals hashed by their
 * bytes (lists order-free), so an unresolved expression leaf hashes to the
 * same value (atree__expr_leaf_hash) without touching the string table. */
struct atree__strtab;
uint64_t atree__pred_content_hash(const struct atree__pred *p, const struct atree__strtab *strings);
bool atree__pred_equal(const struct atree__pred *a, const struct atree__pred *b);

/* Estimated evaluation cost (PLAN §4.9): 1 for constant-time kinds,
 * 1 + log2(len) for set membership, len for list operators. */
uint64_t atree__pred_cost(const struct atree__pred *p);

/* Estimated likelihood of being true, lower = rarer (PLAN §4.4): 0 for
 * equality/membership, 1 for ranges and all-of/is-empty, 4 for booleans and
 * is-null, 5 for negated/wide forms. Used to pick an AND node's access child. */
int atree__pred_wake_rank(const struct atree__pred *p);

/* Deep copy / release of the operand. */
atree_status_t atree__pred_copy(struct atree__mem *m, struct atree__pred *dst,
                                const struct atree__pred *src);
void atree__pred_free(struct atree__mem *m, struct atree__pred *p);

/* Resolves a string reference to bytes for printing: for resolved predicates
 * ref is a string-table id; for unresolved ones (operand data == NULL) it is
 * the index into the expression's literal array. NULL -> "". */
typedef const char *(*atree__strfn)(const void *ctx, uint32_t ref, uint32_t *len);

/* atree__strfn over a struct atree__strtab (ctx). */
const char *atree__strtab_resolver(const void *ctx, uint32_t ref, uint32_t *len);

/* Renders DSL text, e.g. `country in ["CA", "US"]`. NOT_ALL_OF renders as
 * `not (attr all of [...])`. */
void atree__pred_print(const struct atree__pred *p, const struct atree__attrs *attrs,
                       atree__strfn str, const void *ctx, struct atree__writer *w);

#endif /* ATREE_PREDICATE_H */

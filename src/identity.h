/*
 * Expression-to-node identity table (paper §4.2.1, H_en). An open-addressing
 * set of node ids keyed by structural hash; a lookup compares the full
 * structure (operator + sorted child ids, or the whole predicate), so two
 * different subexpressions can never be merged by a hash collision.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_IDENTITY_H
#define ATREE_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#include "atree.h"

#include "node.h"

struct atree__idset {
    uint32_t *slots; /* node id, ATREE_NID_NONE (empty) or ATREE_IDSET_TOMB */
    uint32_t cap;    /* power of two or 0 */
    uint32_t count;
    uint32_t used;     /* count + tombstones */
    uint32_t reserved; /* slots a rollback may still need (see atree__idset_reinsert) */
};

#define ATREE_IDSET_TOMB (UINT32_MAX - 1)

/* The content table (t->content) is a second set of the same shape that
 * holds inner nodes under their flat structural hash: operator plus the
 * multiset of children, where a child of the same operator contributes
 * its own flat members (so AND(AND(a,b),c) and AND(a,b,c) share a key). It
 * is what lets an insert return an existing node without rebuilding it
 * (paper Alg. 4 lines 1-4); a hit is always verified structurally, so a
 * collision merges nothing. Its keys never change: self-adjust rewires a
 * node to a same-operator child, which leaves the flat members alone. */
uint64_t atree__content_key(const struct atree *t, atree__nid id);
atree_status_t atree__cset_insert(struct atree *t, uint64_t hash, atree__nid id);
bool atree__cset_remove(struct atree *t, uint64_t hash, atree__nid id);
/* Iterates the nodes whose content key is `hash`: start with *cursor ==
 * UINT32_MAX; returns ATREE_NID_NONE when exhausted. */
atree__nid atree__cset_next(const struct atree *t, uint64_t hash, uint32_t *cursor);
void atree__cset_free(struct atree *t);

/* The identity functions operate on t->identity with t->mem and read
 * t->nodes/preds. */
void atree__idset_init(struct atree__idset *s);
void atree__idset_free(struct atree *t);

/* Finds the node matching the probe, or ATREE_NID_NONE. */
atree__nid atree__idset_find(const struct atree *t, const struct atree__probe *probe);
/* Inserts a node under its hash (the caller guarantees it is not present).
 * Keeps `reserved` slots free under the load factor on top of this one. */
atree_status_t atree__idset_insert(struct atree *t, uint64_t hash, atree__nid id);
/* Rollback's insert: re-files a node under a hash it was removed from in
 * the same insert, consuming one of the slots a rewire reserved for it
 * (t->identity.reserved++ before the rewire's own insert), so it never
 * allocates and cannot fail. */
void atree__idset_reinsert(struct atree *t, uint64_t hash, atree__nid id);
/* Removes a node; returns false if it was not present. */
bool atree__idset_remove(struct atree *t, uint64_t hash, atree__nid id);

#endif /* ATREE_IDENTITY_H */

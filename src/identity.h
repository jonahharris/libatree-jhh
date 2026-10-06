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
    uint32_t used; /* count + tombstones */
};

#define ATREE_IDSET_TOMB (UINT32_MAX - 1)

/* All functions operate on t->identity with t->mem and read t->nodes/preds. */
void atree__idset_init(struct atree__idset *s);
void atree__idset_free(struct atree *t);

/* Finds the node matching the probe, or ATREE_NID_NONE. */
atree__nid atree__idset_find(const struct atree *t, const struct atree__probe *probe);
/* Inserts a node under its hash (the caller guarantees it is not present). */
atree_status_t atree__idset_insert(struct atree *t, uint64_t hash, atree__nid id);
/* Removes a node; returns false if it was not present. */
bool atree__idset_remove(struct atree *t, uint64_t hash, atree__nid id);
/* Pre-sizes for n entries. */
atree_status_t atree__idset_reserve(struct atree *t, uint32_t n);

#endif /* ATREE_IDENTITY_H */

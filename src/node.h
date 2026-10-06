/*
 * DAG node layout (paper §4.1). One 64-byte node per distinct predicate
 * (leaf) or subexpression (AND/OR). The paper's r-nodes are nodes that carry
 * subscription ids (HAS_SUBS); a node may be both shared and subscribed.
 *
 * Nodes live in a slab indexed by atree__nid; ids are reused through a free
 * list, which is safe because a node is removed from every parent, from the
 * identity table and from the indexes before its slot is recycled. Any
 * pointer into the slab is invalidated by node allocation: code re-fetches
 * `&t->nodes.data[id]` after anything that may allocate a node.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_NODE_H
#define ATREE_NODE_H

#include <stdint.h>

#include "compiler.h"
#include "predicate.h"
#include "vec.h"

typedef uint32_t atree__nid;
#define ATREE_NID_NONE UINT32_MAX

enum atree__node_kind {
    ATREE_NODE_FREE = 0, /* slot on the free list */
    ATREE_NODE_LEAF,
    ATREE_NODE_AND,
    ATREE_NODE_OR
};

enum { ATREE_NODE_HAS_SUBS = 1u << 0 };

struct atree__node {
    uint8_t kind;  /* enum atree__node_kind                                      */
    uint8_t flags; /* ATREE_NODE_*                                                */
    uint16_t reserved;
    uint32_t level;                /* 1 for leaves, 1 + max(children) otherwise       */
    uint64_t hash;                 /* structural hash (identity table)                 */
    uint32_t use_count;            /* paper's useCount == parents.len + #subscriptions */
    atree__nid access_child;       /* AND with propagation on demand: the waking child */
    struct atree__u32vec children; /* inner: sorted ascending, unique, len >= 2     */
    struct atree__u32vec parents;  /* every structural parent                       */
    uint32_t pred;                 /* leaf: index into the predicate slab, else UINT32_MAX    */
    uint32_t index_slot;           /* leaf: position in the phase-1 leaf list / index (M5)    */
};

ATREE_STATIC_ASSERT(sizeof(struct atree__node) <= 64, node_fits_in_a_cache_line);

ATREE_VEC_DEFINE(atree__nodevec, struct atree__node);
ATREE_VEC_DEFINE(atree__predvec, struct atree__pred);
ATREE_VEC_DEFINE(atree__sublistvec, struct atree__u64vec);

/* Description of a node that may not exist yet, for identity lookups. */
struct atree__probe {
    uint8_t kind;
    uint64_t hash;
    const struct atree__pred *pred; /* leaf */
    const uint32_t *children;       /* inner: sorted unique ids */
    uint32_t nchildren;
};

#endif /* ATREE_NODE_H */

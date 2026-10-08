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

/* Parent lists are kept in four regions so that two scans read only what
 * they need, without ever filtering a long list:
 *
 *   [0, end_aw)      anchored wakers
 *   [end_aw, end_a)  anchored non-wakers
 *   [end_a, end_w)   non-anchored wakers
 *   [end_w, len)     non-anchored non-wakers
 *
 * Anchors: every inner node designates one child as its anchor (the child
 * with the fewest parents when the node was created, re-chosen on rewire).
 * Reorganize (Alg. 2) scans only anchored parents, [0, end_a): a cover set
 * contains its own anchor, so this finds every cover while skipping the
 * long parent lists of popular leaves, which are rarely anchors because
 * they were already popular when their parents were built. The per-edge
 * position entries carry the anchor flag in their high bit.
 *
 * Wakers: a parent wakes this node during matching when it is an OR node,
 * or an AND node whose access child is this node (propagation on demand,
 * §5.2.2), or any AND node when propagation on demand is off. Search
 * (emit in search.c) walks [0, end_aw) and [end_a, end_w) and never looks
 * at an AND parent it cannot wake, which for a popular leaf is most of
 * them. Moving an edge between regions is a bounded number of swaps and
 * never allocates. */
struct atree__node {
    uint8_t kind;    /* enum atree__node_kind                                      */
    uint8_t flags;   /* ATREE_NODE_*                                                */
    uint16_t level;  /* 1 for leaves, 1 + max(children) otherwise; <= 65535          */
    uint32_t end_aw; /* parents region boundaries, see above                        */
    uint64_t hash;   /* structural hash (identity table)                 */
    uint32_t end_a;
    uint32_t end_w;
    atree__nid access_child;       /* AND with propagation on demand: the waking child */
    uint32_t pred;                 /* leaf: index into the predicate slab, else UINT32_MAX    */
    struct atree__u32vec children; /* inner: sorted ascending, unique, len >= 2     */
    struct atree__u32vec parents;  /* every structural parent                       */
};
ATREE_STATIC_ASSERT(sizeof(struct atree__node) <= 64, node_fits_in_a_cache_line);

ATREE_VEC_DEFINE(atree__nodevec, struct atree__node);
ATREE_VEC_DEFINE(atree__predvec, struct atree__pred);
/* Subscription ids attached to one node. Almost every node that carries
 * ids carries exactly one, so the first id is stored inline and a heap
 * list is allocated only for the second. `cap` is the tag: 0 means `u.one`
 * is the storage (len <= 1); otherwise `u.many` holds `cap` slots. */
struct atree__sublist {
    union {
        uint64_t one;
        uint64_t *many;
    } u;
    uint32_t len;
    uint32_t cap;
};
ATREE_VEC_DEFINE(atree__sublistvec, struct atree__sublist);

/* Description of a node that may not exist yet, for identity lookups. */
struct atree_expr;

struct atree__probe {
    uint8_t kind;
    uint64_t hash;
    const struct atree__pred *pred; /* leaf: resolved predicate, or NULL when `leaf` is set */
    const struct atree_expr *leaf;  /* leaf: unresolved expression leaf (string literals raw) */
    const uint32_t *children;       /* inner: sorted unique ids */
    uint32_t nchildren;
};

#endif /* ATREE_NODE_H */

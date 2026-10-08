/*
 * Per-attribute predicate indexes for phase 1 of matching (paper §5.1
 * "predicate matching"; PLAN §4.8). Instead of evaluating every leaf, the
 * event's value for each attribute is used to look up exactly the leaves
 * it satisfies:
 *
 *   bool  x / not x        two lists, picked by the value
 *   = c                    hash bucket keyed by c
 *   in [..] / one of [..]  hash bucket per element (a leaf sits in k buckets)
 *   < <= > >=              sorted "ray" arrays: prefix / suffix after a
 *                          binary search on the threshold
 *   is null                list seeded only when the attribute is undefined
 *   everything else        per-attribute scan list, evaluated for the event
 *                          (<>, not in, none of, all of, is not null, is
 *                          [not] empty)
 *
 * Disabled by ATREE_FLAG_NO_PREDICATE_INDEX, in which case phase 1 scans the
 * tree's leaf list; the differential test compares both modes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_INDEX_H
#define ATREE_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#include "atree.h"

#include "hash.h"
#include "node.h"
#include "vec.h"

struct atree;
struct atree_event;

enum atree__route {
    ATREE_ROUTE_NONE = 0,
    ATREE_ROUTE_BOOL_TRUE,
    ATREE_ROUTE_BOOL_FALSE,
    ATREE_ROUTE_EQ,
    ATREE_ROUTE_MEMBER,
    ATREE_ROUTE_LOWER, /* x > c, x >= c */
    ATREE_ROUTE_UPPER, /* x < c, x <= c */
    ATREE_ROUTE_NULL,
    ATREE_ROUTE_SCAN
};

struct atree__ray {
    int64_t i; /* threshold for int attributes   */
    double f;  /* threshold for float attributes */
    atree__nid id;
    uint8_t op; /* atree_op_t */
};

ATREE_VEC_DEFINE(atree__rayvec, struct atree__ray);
ATREE_VEC_DEFINE(atree__bucketvec, struct atree__u32vec);

struct atree__index_attr {
    struct atree__u32vec bool_true;
    struct atree__u32vec bool_false;
    struct atree__u64map eq;     /* key -> bucket slot */
    struct atree__u64map member; /* key -> bucket slot */
    struct atree__rayvec lower;  /* sorted by threshold, then id */
    struct atree__rayvec upper;
    struct atree__u32vec nulls;
    struct atree__u32vec scan;
};

struct atree__index {
    struct atree__index_attr *attrs; /* one per attribute */
    uint32_t nattrs;
    struct atree__bucketvec buckets; /* pool shared by all eq/member maps */
    struct atree__u32vec free_buckets;
    struct atree__u32vec null_attrs; /* attributes whose `nulls` list is non-empty: the
                                        only ones phase 1 must look at when the event
                                        leaves them undefined */
    struct atree__u32slab list_pos;  /* per node id: position in its single list, or NONE */
    uint64_t indexed;                /* leaves in bool/eq/member/ray/null structures */
    uint64_t scanned;                /* leaves in scan lists */
};

atree_status_t atree__index_init(struct atree *t);
void atree__index_free(struct atree *t);

/* Where a leaf predicate goes, given its attribute's type. */
enum atree__route atree__index_route(const struct atree *t, const struct atree__pred *p);

/* Adds / removes a leaf node (its predicate must be in the slab). */
atree_status_t atree__index_add(struct atree *t, atree__nid id);
void atree__index_remove(struct atree *t, atree__nid id);

/* Phase-1 callback: a leaf is true for the event. */
typedef atree_status_t (*atree__seed_fn)(void *ctx, atree__nid id);

/* Finds every leaf the event satisfies; `evaluated` counts leaf evaluations
 * and index hits. */
atree_status_t atree__index_probe(const struct atree *t, const struct atree_event *ev,
                                  atree__seed_fn seed, void *ctx, uint64_t *evaluated);

/* Linear consistency check (validate): every leaf appears exactly where its
 * route says, nothing else appears anywhere, lists agree with list_pos, rays
 * are sorted. Uses `scratch` for a per-node counter array. Returns
 * ATREE_ERR_CORRUPT with a message, ATREE_ERR_NOMEM, or ATREE_OK. */
atree_status_t atree__index_check(const struct atree *t, struct atree__mem *scratch, char *msg,
                                  size_t cap);

#endif /* ATREE_INDEX_H */

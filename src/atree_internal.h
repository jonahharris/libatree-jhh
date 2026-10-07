/*
 * Layout of struct atree and the lock helpers shared by every module.
 *
 * Thread-safety rule (CLAUDE.md): functions taking `const atree_t *` never
 * write to anything reachable from the tree. In particular, events and
 * reports carry their own struct atree__mem (same allocator, separate
 * counters) so that creating them from reader threads never touches
 * tree->mem.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_INTERNAL_H
#define ATREE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "atree.h"

#include "alloc.h"
#include "attr.h"
#include "hash.h"
#include "identity.h"
#include "index.h"
#include "node.h"
#include "strtab.h"

#define ATREE_DEFAULT_MAX_DEPTH 64u
#define ATREE_DEFAULT_MAX_ADJUST_CANDIDATES 4096u
#define ATREE_DEFAULT_INITIAL_NODES 1024u

/* Values in the subscription map that are not node ids. */
#define ATREE_SUB_ALWAYS (UINT32_MAX - 1) /* constant-true expression  */
#define ATREE_SUB_NEVER (UINT32_MAX - 2)  /* constant-false expression */

/* Insert journal (tree.c): what an insert changed, undone newest-first on
 * failure. */
enum atree__journal_kind {
    ATREE_J_CREATED, /* node created by this insert                           */
    ATREE_J_REWIRED, /* self-adjust rewired `node`; old child set kept        */
    ATREE_J_LEVEL    /* `node` changed level from old_level                    */
};

struct atree__journal_entry {
    uint8_t kind;
    atree__nid node;
    atree__nid added;                  /* REWIRED: the child that replaced old ones */
    struct atree__u32vec old_children; /* REWIRED: owned until commit             */
    uint64_t old_hash;
    uint32_t old_level;
    atree__nid old_access;
};

ATREE_VEC_DEFINE(journalvec, struct atree__journal_entry);

struct atree__arena;

struct atree {
    struct atree__mem mem;
    atree_lock_t lock; /* copy of cfg->lock; valid when has_lock */
    bool has_lock;
    unsigned flags;
    size_t max_depth;
    size_t max_adjust_candidates;
    size_t initial_nodes;
    struct atree__attrs attrs;
    struct atree__strtab strings;

    /* The DAG. */
    struct atree__nodevec nodes;     /* slab; kind == ATREE_NODE_FREE for free slots */
    struct atree__u32vec free_nodes; /* recycled slots                                */
    struct atree__predvec preds;     /* predicate slab, referenced by leaves           */
    struct atree__u32vec leaf_pos;   /* per predicate slot: position in `leaves`       */
    struct atree__u32vec free_preds;
    struct atree__idset identity;      /* paper's H_en                                   */
    struct atree__idset content;       /* inner nodes by flat content (identity.h)       */
    struct atree__u64vec csum;         /* per node: sum of flat member keys (inner nodes) */
    struct atree__u32vec leaves;       /* every leaf; phase 1 scans this when unindexed  */
    struct atree__u32vec level_counts; /* nodes per level; [0] unused                   */
    uint32_t max_level;
    struct atree__index index; /* phase-1 predicate indexes (M5); unused when
                                  ATREE_FLAG_NO_PREDICATE_INDEX is set           */

    /* Subscriptions. */
    struct atree__u64map subs;         /* atree_id_t -> node id / ATREE_SUB_*            */
    struct atree__u32vec sub_slot;     /* per node id: index into sublists, or UINT32_MAX */
    struct atree__sublistvec sublists; /* ids attached to a node                         */
    struct atree__u32vec free_sublists;
    struct atree__u64vec always; /* ids of constant-true subscriptions              */
    uint64_t nsubs;

    /* Writer-side scratch. */
    struct journalvec journal;       /* insert journal, reused across inserts        */
    struct atree__arena *norm_arena; /* normalization arena, reused across inserts   */
    struct atree__u32vec worklist;
    struct atree__u32vec scratch; /* lookup verification: flat member sets      */
    struct atree__u32vec mark; /* per node id: epoch stamp for set tests (reorganize/self-adjust) */
    uint32_t mark_epoch;

    /* Cumulative statistics. */
    uint64_t edges;
    uint64_t reorganized;
    uint64_t self_adjusted;
    uint64_t adjust_candidates_skipped;
};

static inline void atree__rdlock(const atree_t *t)
{
    if (t->has_lock) {
        t->lock.rdlock(t->lock.ctx);
    }
}

static inline void atree__rdunlock(const atree_t *t)
{
    if (t->has_lock) {
        t->lock.rdunlock(t->lock.ctx);
    }
}

static inline void atree__wrlock(atree_t *t)
{
    if (t->has_lock) {
        t->lock.wrlock(t->lock.ctx);
    }
}

static inline void atree__wrunlock(atree_t *t)
{
    if (t->has_lock) {
        t->lock.wrunlock(t->lock.ctx);
    }
}

/* Subscription ids attached to a node, or NULL. */
const struct atree__u64vec *atree__node_sublist(const atree_t *t, atree__nid id);

#endif /* ATREE_INTERNAL_H */

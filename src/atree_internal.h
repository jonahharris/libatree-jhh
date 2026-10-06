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
#include "strtab.h"

#define ATREE_DEFAULT_MAX_DEPTH 64u
#define ATREE_DEFAULT_MAX_ADJUST_CANDIDATES 4096u
#define ATREE_DEFAULT_INITIAL_NODES 1024u

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
    /* M4 adds: node slab, identity table, subscription map, indexes, stats. */
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

#endif /* ATREE_INTERNAL_H */

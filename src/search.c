/*
 * Event matching (paper §5, Alg. 6) with the zero suppression filter
 * (§5.2.1) and propagation on demand (§5.2.2).
 *
 * Phase 1 finds the leaves the event satisfies (M4: evaluates every leaf;
 * M5 adds per-attribute indexes). Phase 2 is a level-synchronous bottom-up
 * sweep: a node is enqueued at its level when a child becomes true (for an
 * AND node only when its access child does), evaluated with bit lookups
 * over its children (lower levels are final), and, if true, emits its
 * subscriptions and wakes its parents. Nothing false is ever propagated.
 *
 * All per-search state lives in the report; the tree is never written.
 * Reset is proportional to the nodes touched: the level queues double as
 * the dirty list whose bits are cleared at the end.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "atree_internal.h"
#include "event.h"

struct atree_report {
    const atree_t *tree;
    struct atree__mem mem;
    uint64_t *is_true;            /* one bit per node slot                 */
    uint64_t *queued;             /* enqueued in this search               */
    uint32_t words;               /* length of both bitsets, in uint64_t   */
    struct atree__u32vec *queues; /* indexed by level; [0] unused */
    uint32_t nqueues;
    struct atree__u64vec matches;
    atree_report_stats_t stats;
};

/* ---- bitsets ------------------------------------------------------------ */

static bool bit_get(const uint64_t *w, uint32_t i)
{
    return (w[i / 64] & (UINT64_C(1) << (i % 64))) != 0;
}

static void bit_set(uint64_t *w, uint32_t i)
{
    w[i / 64] |= UINT64_C(1) << (i % 64);
}

static void bit_clear(uint64_t *w, uint32_t i)
{
    w[i / 64] &= ~(UINT64_C(1) << (i % 64));
}

/* ---- report lifecycle --------------------------------------------------- */

atree_status_t atree_report_create(const atree_t *tree, atree_report_t **out)
{
    struct atree__mem mem;
    atree_report_t *r;
    if (tree == NULL || out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    atree__mem_init(&mem, &tree->mem.a);
    r = atree__zalloc(&mem, sizeof *r);
    if (r == NULL) {
        return ATREE_ERR_NOMEM;
    }
    r->mem = mem;
    r->tree = tree;
    atree__u64vec_init(&r->matches);
    *out = r;
    return ATREE_OK;
}

void atree_report_destroy(atree_report_t *r)
{
    struct atree__mem mem;
    uint32_t i;
    if (r == NULL) {
        return;
    }
    if (r->words > 0) {
        atree__free_array(&r->mem, r->is_true, r->words, sizeof *r->is_true);
        atree__free_array(&r->mem, r->queued, r->words, sizeof *r->queued);
    }
    for (i = 0; i < r->nqueues; i++) {
        atree__u32vec_free(&r->mem, &r->queues[i]);
    }
    if (r->nqueues > 0) {
        atree__free_array(&r->mem, r->queues, r->nqueues, sizeof *r->queues);
    }
    atree__u64vec_free(&r->mem, &r->matches);
    mem = r->mem;
    atree__free(&mem, r, sizeof *r);
}

/* Grows the scratch to the tree's current size; zero-fills only new words. */
static atree_status_t ensure_capacity(atree_report_t *r, const atree_t *t)
{
    uint32_t words = (t->nodes.len + 63) / 64;
    uint32_t nq = t->max_level + 1;
    if (nq < 2) {
        nq = 2;
    }
    if (words > r->words) {
        uint64_t *a;
        uint64_t *b;
        if (r->words == 0) {
            a = atree__alloc_array(&r->mem, words, sizeof *a);
            b = a != NULL ? atree__alloc_array(&r->mem, words, sizeof *b) : NULL;
            if (b == NULL) {
                atree__free_array(&r->mem, a, words, sizeof *a);
                return ATREE_ERR_NOMEM;
            }
        } else {
            a = atree__realloc_array(&r->mem, r->is_true, r->words, words, sizeof *a);
            if (a == NULL) {
                return ATREE_ERR_NOMEM;
            }
            r->is_true = a;
            b = atree__realloc_array(&r->mem, r->queued, r->words, words, sizeof *b);
            if (b == NULL) {
                /* is_true already grown; keep sizes consistent by shrinking back */
                uint64_t *back =
                    atree__realloc_array(&r->mem, r->is_true, words, r->words, sizeof *a);
                if (back != NULL) {
                    r->is_true = back;
                }
                return ATREE_ERR_NOMEM;
            }
        }
        memset(a + r->words, 0, (size_t)(words - r->words) * sizeof *a);
        memset(b + r->words, 0, (size_t)(words - r->words) * sizeof *b);
        r->is_true = a;
        r->queued = b;
        r->words = words;
    }
    if (nq > r->nqueues) {
        struct atree__u32vec *q;
        uint32_t i;
        if (r->nqueues == 0) {
            q = atree__alloc_array(&r->mem, nq, sizeof *q);
        } else {
            q = atree__realloc_array(&r->mem, r->queues, r->nqueues, nq, sizeof *q);
        }
        if (q == NULL) {
            return ATREE_ERR_NOMEM;
        }
        for (i = r->nqueues; i < nq; i++) {
            atree__u32vec_init(&q[i]);
        }
        r->queues = q;
        r->nqueues = nq;
    }
    return ATREE_OK;
}

/* Clears every bit set in this search and empties the queues. */
static void reset_scratch(atree_report_t *r)
{
    uint32_t l;
    uint32_t i;
    for (l = 0; l < r->nqueues; l++) {
        struct atree__u32vec *q = &r->queues[l];
        for (i = 0; i < q->len; i++) {
            bit_clear(r->is_true, q->data[i]);
            bit_clear(r->queued, q->data[i]);
        }
        q->len = 0;
    }
}

/* ---- the search --------------------------------------------------------- */

static int cmp_u64(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa;
    uint64_t b = *(const uint64_t *)pb;
    return (a > b) - (a < b);
}

/* A node is true: collect its subscriptions, wake its parents. */
static atree_status_t emit(atree_report_t *r, const atree_t *t, atree__nid id,
                           const struct atree__node *n)
{
    uint32_t i;
    atree_status_t st;
    if ((n->flags & ATREE_NODE_HAS_SUBS) != 0) {
        const struct atree__u64vec *subs = atree__node_sublist(t, id);
        for (i = 0; subs != NULL && i < subs->len; i++) {
            st = atree__u64vec_push(&r->mem, &r->matches, subs->data[i]);
            if (st != ATREE_OK) {
                return st;
            }
        }
    }
    for (i = 0; i < n->parents.len; i++) {
        atree__nid pid = n->parents.data[i];
        const struct atree__node *p = &t->nodes.data[pid];
        /* Propagation on demand: only the access child wakes an AND node. */
        if (p->kind == ATREE_NODE_AND && p->access_child != ATREE_NID_NONE &&
            p->access_child != id) {
            continue;
        }
        if (bit_get(r->queued, pid)) {
            continue;
        }
        bit_set(r->queued, pid);
        st = atree__u32vec_push(&r->mem, &r->queues[p->level], pid);
        if (st != ATREE_OK) {
            return st;
        }
    }
    return ATREE_OK;
}

/* Phase-1 seed callback: a leaf is true for this event. Dedupes through the
 * queued bit (a membership leaf can be hit once per matching element). */
static atree_status_t seed_leaf(void *ctx, atree__nid id)
{
    atree_report_t *r = (atree_report_t *)ctx;
    if (bit_get(r->queued, id)) {
        return ATREE_OK;
    }
    r->stats.predicates_matched++;
    bit_set(r->is_true, id);
    bit_set(r->queued, id);
    return atree__u32vec_push(&r->mem, &r->queues[1], id);
}

/* Phase 1 with indexes: look up exactly the satisfied leaves per attribute. */
static atree_status_t phase1_index(atree_report_t *r, const atree_t *t, const atree_event_t *ev)
{
    return atree__index_probe(t, ev, seed_leaf, r, &r->stats.predicates_evaluated);
}

/* Phase 1 without indexes: evaluate every leaf against the event. */
static atree_status_t phase1_scan(atree_report_t *r, const atree_t *t, const atree_event_t *ev)
{
    uint32_t i;
    for (i = 0; i < t->leaves.len; i++) {
        atree__nid id = t->leaves.data[i];
        const struct atree__node *n = &t->nodes.data[id];
        const struct atree__pred *p = &t->preds.data[n->pred];
        atree_tri_t v = atree__pred_eval(p, atree__event_value(ev, p->attr));
        r->stats.predicates_evaluated++;
        if (v == ATREE_TRUE) {
            atree_status_t st;
            r->stats.predicates_matched++;
            bit_set(r->is_true, id);
            bit_set(r->queued, id);
            st = atree__u32vec_push(&r->mem, &r->queues[1], id);
            if (st != ATREE_OK) {
                return st;
            }
        }
    }
    return ATREE_OK;
}

static bool evaluate_inner(const atree_report_t *r, const struct atree__node *n)
{
    uint32_t i;
    if (n->kind == ATREE_NODE_AND) {
        for (i = 0; i < n->children.len; i++) {
            if (!bit_get(r->is_true, n->children.data[i])) {
                return false;
            }
        }
        return true;
    }
    for (i = 0; i < n->children.len; i++) {
        if (bit_get(r->is_true, n->children.data[i])) {
            return true;
        }
    }
    return false;
}

/* Runs a full search; the lock is held by the caller. */
static atree_status_t search_locked(atree_report_t *r, const atree_t *t, const atree_event_t *ev)
{
    atree_status_t st;
    uint32_t level;
    uint32_t i;

    memset(&r->stats, 0, sizeof r->stats);
    r->matches.len = 0;
    st = ensure_capacity(r, t);
    if (st != ATREE_OK) {
        return st;
    }
    reset_scratch(r); /* no-op unless a previous search was interrupted */

    if ((t->flags & ATREE_FLAG_NO_PREDICATE_INDEX) == 0) {
        st = phase1_index(r, t, ev);
    } else {
        st = phase1_scan(r, t, ev);
    }
    /* Phase 2: level-synchronous sweep. Queues only receive higher levels
     * than the one being drained, so iterating by index is safe. */
    for (level = 1; st == ATREE_OK && level <= t->max_level; level++) {
        struct atree__u32vec *q = &r->queues[level];
        for (i = 0; st == ATREE_OK && i < q->len; i++) {
            atree__nid id = q->data[i];
            const struct atree__node *n = &t->nodes.data[id];
            if (level > 1) {
                bool v = evaluate_inner(r, n);
                r->stats.nodes_visited++;
                if (n->kind == ATREE_NODE_AND) {
                    r->stats.and_woken++;
                    if (v) {
                        r->stats.and_true++;
                    }
                } else {
                    r->stats.or_visited++;
                }
                if (!v) {
                    continue; /* zero suppression: false is never propagated */
                }
                bit_set(r->is_true, id);
            }
            st = emit(r, t, id, n);
        }
    }
    for (i = 0; st == ATREE_OK && i < t->always.len; i++) {
        st = atree__u64vec_push(&r->mem, &r->matches, t->always.data[i]);
    }
    reset_scratch(r);
    if (st != ATREE_OK) {
        r->matches.len = 0;
        return st;
    }
    if (r->matches.len > 1) {
        qsort(r->matches.data, r->matches.len, sizeof *r->matches.data, cmp_u64);
    }
    r->stats.matches = r->matches.len;
    return ATREE_OK;
}

atree_status_t atree_search(const atree_t *t, const atree_event_t *ev, atree_report_t *r)
{
    atree_status_t st;
    if (t == NULL || ev == NULL || r == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (ev->tree != t || r->tree != t) {
        return ATREE_ERR_INVALID_ARG;
    }
    atree__rdlock(t);
    st = search_locked(r, t, ev);
    atree__rdunlock(t);
    return st;
}

size_t atree_report_count(const atree_report_t *r)
{
    return r == NULL ? 0 : r->matches.len;
}

const atree_id_t *atree_report_matches(const atree_report_t *r)
{
    return r == NULL ? NULL : r->matches.data;
}

void atree_report_stats(const atree_report_t *r, atree_report_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    if (r == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = r->stats;
}

/* ---- conveniences ------------------------------------------------------- */

atree_status_t atree_search_cb(const atree_t *t, const atree_event_t *ev, atree_report_t *scratch,
                               atree_match_fn fn, void *ctx)
{
    atree_report_t *r = scratch;
    atree_status_t st;
    uint32_t i;
    if (t == NULL || ev == NULL || fn == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (r == NULL) {
        st = atree_report_create(t, &r);
        if (st != ATREE_OK) {
            return st;
        }
    }
    st = atree_search(t, ev, r);
    for (i = 0; st == ATREE_OK && i < r->matches.len; i++) {
        if (fn(ctx, r->matches.data[i]) != 0) {
            break;
        }
    }
    if (r != scratch) {
        atree_report_destroy(r);
    }
    return st;
}

atree_status_t atree_exists(const atree_t *t, const atree_event_t *ev, atree_report_t *scratch,
                            bool *out)
{
    atree_report_t *r = scratch;
    atree_status_t st;
    if (t == NULL || ev == NULL || out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = false;
    if (r == NULL) {
        st = atree_report_create(t, &r);
        if (st != ATREE_OK) {
            return st;
        }
    }
    st = atree_search(t, ev, r);
    if (st == ATREE_OK) {
        *out = r->matches.len > 0;
    }
    if (r != scratch) {
        atree_report_destroy(r);
    }
    return st;
}

atree_status_t atree_search_ids(const atree_t *t, const atree_event_t *ev, atree_report_t *r,
                                const atree_id_t *allow, size_t nallow)
{
    atree_status_t st;
    uint32_t i;
    uint32_t m = 0;
    if (r == NULL || (allow == NULL && nallow > 0)) {
        return ATREE_ERR_INVALID_ARG;
    }
    st = atree_search(t, ev, r);
    if (st != ATREE_OK) {
        return st;
    }
    for (i = 0; i < r->matches.len; i++) {
        size_t lo = 0;
        size_t hi = nallow;
        atree_id_t id = r->matches.data[i];
        bool found = false;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (allow[mid] < id) {
                lo = mid + 1;
            } else if (allow[mid] > id) {
                hi = mid;
            } else {
                found = true;
                break;
            }
        }
        if (found) {
            r->matches.data[m++] = id;
        }
    }
    r->matches.len = m;
    r->stats.matches = m;
    return ATREE_OK;
}

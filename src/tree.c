/*
 * Tree lifecycle, index construction (paper §4.2) and deletion (§4.2.4).
 *
 * Insert (Alg. 1/4 without reorganize/self-adjust, which arrive in M6):
 * the normalized expression is built bottom-up; every predicate and every
 * (operator, sorted child set) is looked up in the identity table and
 * reused when present, otherwise a node is created and linked. Any failure
 * rolls the tree back by cascading over the nodes this insert created
 * (they are exactly the nodes whose use count returns to zero).
 *
 * Delete (Alg. 5): detach the id from its node, decrement, and cascade
 * through children whose use count reaches zero, iteratively.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "atree_internal.h"
#include "expr.h"
#include "lexer.h"

#define ATREE_FLAG_ALL                                                                             \
    (ATREE_FLAG_NO_REORGANIZE | ATREE_FLAG_NO_SELF_ADJUST | ATREE_FLAG_NO_PROPAGATION_ON_DEMAND |  \
     ATREE_FLAG_NO_PREDICATE_INDEX)

/* Node ids must stay below the identity-table sentinels. */
#define ATREE_MAX_NODES (UINT32_MAX - 3)

/* ---- configuration ------------------------------------------------------ */

static atree_status_t check_config(const atree_config_t *cfg)
{
    if (cfg == NULL) {
        return ATREE_OK;
    }
    if ((cfg->flags & ~(unsigned)ATREE_FLAG_ALL) != 0) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (cfg->allocator != NULL &&
        (cfg->allocator->alloc == NULL || cfg->allocator->realloc == NULL ||
         cfg->allocator->free == NULL)) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (cfg->lock != NULL &&
        (cfg->lock->rdlock == NULL || cfg->lock->rdunlock == NULL || cfg->lock->wrlock == NULL ||
         cfg->lock->wrunlock == NULL)) {
        return ATREE_ERR_INVALID_ARG;
    }
    return ATREE_OK;
}

/* ---- lifecycle ---------------------------------------------------------- */

static bool indexed(const atree_t *t)
{
    return (t->flags & ATREE_FLAG_NO_PREDICATE_INDEX) == 0;
}

static void children_free(atree_t *t, struct atree__u32vec *v);

static void free_dag(atree_t *t)
{
    uint32_t i;
    for (i = 0; i < t->nodes.len; i++) {
        struct atree__node *n = atree__nodeslab_at(&t->nodes, i);
        if (n->kind != ATREE_NODE_FREE) {
            children_free(t, &n->children);
            atree__u32vec_free(&t->mem, &n->parents);
        }
    }
    atree__nodeslab_free(&t->mem, &t->nodes);
    atree__u32vec_free(&t->mem, &t->free_nodes);
    for (i = 0; i < t->preds.len; i++) {
        atree__pred_free(&t->mem, atree__predslab_at(&t->preds, i)); /* no-op for recycled slots */
    }
    atree__predslab_free(&t->mem, &t->preds);
    atree__u32slab_free(&t->mem, &t->leaf_pos);
    atree__u32vec_free(&t->mem, &t->free_preds);
    atree__idset_free(t);
    atree__cset_free(t);
    atree__u64slab_free(&t->mem, &t->csum);
    atree__index_free(t);
    atree__u32vec_free(&t->mem, &t->leaves);
    atree__u32vec_free(&t->mem, &t->level_counts);
    atree__u64map_free(&t->mem, &t->subs);
    atree__u32slab_free(&t->mem, &t->sub_slot);
    for (i = 0; i < t->sublists.len; i++) {
        struct atree__sublist *l = &t->sublists.data[i];
        if (l->cap != 0) {
            atree__free_array(&t->mem, l->u.many, l->cap, sizeof *l->u.many);
        }
    }
    atree__sublistvec_free(&t->mem, &t->sublists);
    atree__u32vec_free(&t->mem, &t->free_sublists);
    atree__u64vec_free(&t->mem, &t->always);
    atree__u32vec_free(&t->mem, &t->worklist);
    atree__u32vec_free(&t->mem, &t->scratch);
    journalvec_free(&t->mem, &t->journal);
    atree__arena_free(t->norm_arena);
    t->norm_arena = NULL;
    atree__u32slab_free(&t->mem, &t->mark);
}

atree_status_t atree_create(const atree_config_t *cfg, const atree_attr_def_t *attrs, size_t nattrs,
                            atree_t **out)
{
    struct atree__mem mem;
    atree_t *t;
    atree_status_t st;

    if (out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    st = check_config(cfg);
    if (st != ATREE_OK) {
        return st;
    }
    atree__mem_init(&mem, cfg != NULL ? cfg->allocator : NULL);
    t = atree__zalloc(&mem, sizeof *t);
    if (t == NULL) {
        return ATREE_ERR_NOMEM;
    }
    t->mem = mem;
    if (cfg != NULL) {
        if (cfg->lock != NULL) {
            t->lock = *cfg->lock;
            t->has_lock = true;
        }
        t->flags = cfg->flags;
        t->max_depth = cfg->max_depth != 0 ? cfg->max_depth : ATREE_DEFAULT_MAX_DEPTH;
        t->max_adjust_candidates = cfg->max_adjust_candidates != 0
            ? cfg->max_adjust_candidates
            : ATREE_DEFAULT_MAX_ADJUST_CANDIDATES;
        t->initial_nodes =
            cfg->initial_nodes != 0 ? cfg->initial_nodes : ATREE_DEFAULT_INITIAL_NODES;
        t->max_expr_nodes =
            cfg->max_expr_nodes != 0 ? cfg->max_expr_nodes : ATREE_DEFAULT_MAX_EXPR_NODES;
    } else {
        t->max_depth = ATREE_DEFAULT_MAX_DEPTH;
        t->max_adjust_candidates = ATREE_DEFAULT_MAX_ADJUST_CANDIDATES;
        t->initial_nodes = ATREE_DEFAULT_INITIAL_NODES;
        t->max_expr_nodes = ATREE_DEFAULT_MAX_EXPR_NODES;
    }
    atree__nodeslab_init(&t->nodes);
    atree__u32vec_init(&t->free_nodes);
    atree__predslab_init(&t->preds);
    atree__u32slab_init(&t->leaf_pos);
    atree__u32vec_init(&t->free_preds);
    atree__idset_init(&t->identity);
    atree__idset_init(&t->content);
    atree__u64slab_init(&t->csum);
    atree__u32vec_init(&t->leaves);
    atree__u32vec_init(&t->level_counts);
    atree__u64map_init(&t->subs);
    atree__u32slab_init(&t->sub_slot);
    atree__sublistvec_init(&t->sublists);
    atree__u32vec_init(&t->free_sublists);
    atree__u64vec_init(&t->always);
    atree__u32vec_init(&t->worklist);
    atree__u32vec_init(&t->scratch);
    journalvec_init(&t->journal);
    t->norm_arena = NULL;
    atree__u32slab_init(&t->mark);
    t->mark_epoch = 0;
    memset(&t->index, 0, sizeof t->index);

    st = atree__attrs_init(&t->mem, &t->attrs, attrs, nattrs);
    if (st != ATREE_OK) {
        mem = t->mem;
        atree__free(&mem, t, sizeof *t);
        return st;
    }
    st = atree__strtab_init(&t->mem, &t->strings);
    if (st == ATREE_OK) {
        st = atree__index_init(t);
    }
    if (st == ATREE_OK) {
        uint32_t hint =
            t->initial_nodes > ATREE_MAX_NODES ? ATREE_MAX_NODES : (uint32_t)t->initial_nodes;
        st = atree__nodeslab_reserve(&t->mem, &t->nodes, hint);
    }
    if (st == ATREE_OK) {
        st = atree__u32vec_push(&t->mem, &t->level_counts, 0); /* level 0 unused */
    }
    if (st != ATREE_OK) {
        free_dag(t);
        atree__strtab_free(&t->mem, &t->strings);
        atree__attrs_free(&t->mem, &t->attrs);
        mem = t->mem;
        atree__free(&mem, t, sizeof *t);
        return st;
    }
    *out = t;
    return ATREE_OK;
}

void atree_destroy(atree_t *t)
{
    struct atree__mem mem;
    if (t == NULL) {
        return;
    }
    free_dag(t);
    atree__strtab_free(&t->mem, &t->strings);
    atree__attrs_free(&t->mem, &t->attrs);
    mem = t->mem;
    atree__free(&mem, t, sizeof *t);
}

/* ---- attributes --------------------------------------------------------- */

atree_attr_id_t atree_attr_lookup(const atree_t *t, const char *name)
{
    atree_attr_id_t id;
    if (t == NULL || name == NULL) {
        return ATREE_ATTR_INVALID;
    }
    atree__rdlock(t);
    id = atree__attrs_lookup(&t->attrs, name, strlen(name));
    atree__rdunlock(t);
    return id;
}

size_t atree_attr_count(const atree_t *t)
{
    return t == NULL ? 0 : atree__attrs_count(&t->attrs);
}

const char *atree_attr_name(const atree_t *t, atree_attr_id_t id)
{
    if (t == NULL || id >= atree__attrs_count(&t->attrs)) {
        return NULL;
    }
    return atree__attrs_get(&t->attrs, id)->name;
}

atree_type_t atree_attr_type(const atree_t *t, atree_attr_id_t id)
{
    if (t == NULL || id >= atree__attrs_count(&t->attrs)) {
        return ATREE_TYPE_BOOL;
    }
    return atree__attrs_get(&t->attrs, id)->type;
}

/* ---- node slab ---------------------------------------------------------- */

static struct atree__node *node_at(atree_t *t, atree__nid id)
{
    return atree__nodeslab_at(&t->nodes, id);
}

static atree_status_t node_alloc(atree_t *t, atree__nid *out)
{
    struct atree__node n;
    memset(&n, 0, sizeof n);
    n.kind = ATREE_NODE_FREE;
    n.access_child = ATREE_NID_NONE;
    n.pred = UINT32_MAX;
    atree__u32vec_init(&n.children);
    atree__u32vec_init(&n.parents);
    if (t->free_nodes.len > 0) {
        *out = t->free_nodes.data[--t->free_nodes.len];
        *atree__nodeslab_at(&t->nodes, *out) =
            n; /* sub_slot[*out] was reset to UINT32_MAX on detach */
        return ATREE_OK;
    }
    if (t->nodes.len >= ATREE_MAX_NODES) {
        return ATREE_ERR_LIMIT;
    }
    {
        atree_status_t st = atree__nodeslab_push(&t->mem, &t->nodes, n);
        if (st != ATREE_OK) {
            return st;
        }
        st = atree__u64slab_push(&t->mem, &t->csum, 0);
        if (st != ATREE_OK) {
            t->nodes.len--;
            return st;
        }
        st = atree__u32slab_push(&t->mem, &t->sub_slot, UINT32_MAX);
        if (st != ATREE_OK) {
            t->nodes.len--;
            t->csum.len--;
            return st;
        }
    }
    *out = t->nodes.len - 1;
    return ATREE_OK;
}

/* Frees a node's storage and recycles the slot. */
static void node_release(atree_t *t, atree__nid id)
{
    struct atree__node *n = node_at(t, id);
    children_free(t, &n->children);
    atree__u32vec_free(&t->mem, &n->parents);
    n->kind = ATREE_NODE_FREE;
    n->flags = 0;
    n->pred = UINT32_MAX;
    /* A failed push only strands the slot until destroy; nothing leaks. */
    (void)atree__u32vec_push(&t->mem, &t->free_nodes, id);
}

/* ---- edges --------------------------------------------------------------
 *
 * An inner node's `children` array holds 2*cap uint32: the sorted child ids
 * in [0, len) and, in [cap, cap+len), the position of this node inside each
 * child's `parents` list. Unlinking is therefore a swap-remove plus one
 * binary search to fix the moved parent's recorded position, instead of a
 * linear scan of a parent list that can hold tens of thousands of entries
 * for a popular leaf. Children arrays are allocated exactly (cap == len) and
 * never grow; a rewrite replaces them wholesale. */

static uint32_t *child_pos(struct atree__node *n)
{
    return n->children.data + n->children.cap;
}

static const uint32_t *child_pos_c(const struct atree__node *n)
{
    return n->children.data + n->children.cap;
}

static uint32_t *child_pos_of_vec(struct atree__u32vec *v)
{
    return v->data + v->cap;
}

static atree_status_t children_alloc(atree_t *t, const uint32_t *ids, uint32_t n,
                                     struct atree__u32vec *out)
{
    atree__u32vec_init(out);
    if (n == 0) {
        return ATREE_OK;
    }
    out->data = atree__alloc_array(&t->mem, (size_t)n * 2, sizeof *out->data);
    if (out->data == NULL) {
        return ATREE_ERR_NOMEM;
    }
    memcpy(out->data, ids, (size_t)n * sizeof *ids); /* positions are set by link_child */
    out->len = n;
    out->cap = n;
    return ATREE_OK;
}

static void children_free(atree_t *t, struct atree__u32vec *v)
{
    if (v->data != NULL) {
        atree__free_array(&t->mem, v->data, (size_t)v->cap * 2, sizeof *v->data);
    }
    atree__u32vec_init(v);
}

static uint32_t child_index_in_vec(const struct atree__u32vec *v, atree__nid c)
{
    uint32_t lo = 0;
    uint32_t hi = v->len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (v->data[mid] < c) {
            lo = mid + 1;
        } else if (v->data[mid] > c) {
            hi = mid;
        } else {
            return mid;
        }
    }
    return UINT32_MAX;
}

/* Index of child c in n's sorted children, or UINT32_MAX. */
static uint32_t child_index(const struct atree__node *n, atree__nid c)
{
    return child_index_in_vec(&n->children, c);
}

/* Per-edge position entries: the position in the child's parent list, with
 * the anchor flag in the high bit. */
#define EDGE_ANCHOR UINT32_C(0x80000000)
#define EDGE_POS_MAX (EDGE_ANCHOR - 1)

static uint32_t edge_pos(uint32_t e)
{
    return e & ~EDGE_ANCHOR;
}

static bool edge_anchored(uint32_t e)
{
    return (e & EDGE_ANCHOR) != 0;
}

/* A node is alive while a parent or a subscription uses it (the paper's
 * useCount, derived rather than stored). */
static bool in_use(const struct atree__node *n)
{
    return n->parents.len > 0 || (n->flags & ATREE_NODE_HAS_SUBS) != 0;
}

/* ---- parent-list regions (node.h) --------------------------------------- */

/* Whether parent p wakes child c during matching (search.c, emit). */
static bool is_waker(const struct atree__node *p, atree__nid c)
{
    return p->kind != ATREE_NODE_AND || p->access_child == ATREE_NID_NONE || p->access_child == c;
}

static uint32_t region_for(bool anchor, bool waker)
{
    return anchor ? (waker ? 0 : 1) : (waker ? 2 : 3);
}

static uint32_t region_of_index(const struct atree__node *cn, uint32_t k)
{
    return k < cn->end_aw ? 0 : k < cn->end_a ? 1 : k < cn->end_w ? 2 : 3;
}

/* The boundary between region r and region r + 1. */
static uint32_t *region_end(struct atree__node *cn, uint32_t r)
{
    return r == 0 ? &cn->end_aw : r == 1 ? &cn->end_a : &cn->end_w;
}

/* Re-records, for the parent stored at cn->parents[idx], where it sits in
 * child c's list; the anchor flag follows the region. */
static void fix_pos(atree_t *t, struct atree__node *cn, atree__nid c, uint32_t idx)
{
    struct atree__node *pn = node_at(t, cn->parents.data[idx]);
    uint32_t j = child_index(pn, c);
    if (j != UINT32_MAX) {
        child_pos(pn)[j] = idx | (idx < cn->end_a ? EDGE_ANCHOR : 0);
    }
}

static void parents_swap(struct atree__node *cn, uint32_t a, uint32_t b)
{
    atree__nid tmp = cn->parents.data[a];
    cn->parents.data[a] = cn->parents.data[b];
    cn->parents.data[b] = tmp;
}

/* Moves the parent at index k of c's list into region `target` by swapping
 * it across the boundaries in between; returns its new index. Never
 * allocates. */
static uint32_t move_to_region(atree_t *t, struct atree__node *cn, atree__nid c, uint32_t k,
                               uint32_t target)
{
    uint32_t r = region_of_index(cn, k);
    if (r == target) {
        return k;
    }
    /* Each step swaps with the element at the boundary, which is fixed up
     * at once; the moving element is fixed up once at the end. */
    while (r > target) {
        uint32_t *b = region_end(cn, r - 1);
        uint32_t bi = *b; /* first index of region r */
        (*b)++;
        if (k != bi) {
            parents_swap(cn, k, bi);
            fix_pos(t, cn, c, k);
        }
        k = bi;
        r--;
    }
    while (r < target) {
        uint32_t *b = region_end(cn, r);
        uint32_t bi = *b - 1; /* last index of region r */
        (*b)--;
        if (k != bi) {
            parents_swap(cn, k, bi);
            fix_pos(t, cn, c, k);
        }
        k = bi;
        r++;
    }
    fix_pos(t, cn, c, k);
    return k;
}

/* Makes `id` a parent of its i-th child (may allocate in the child's list). */
static atree_status_t link_child(atree_t *t, atree__nid id, uint32_t i, bool anchor, bool waker)
{
    struct atree__node *n = node_at(t, id);
    atree__nid c = n->children.data[i];
    struct atree__node *cn = node_at(t, c);
    atree_status_t st;
    if (cn->parents.len >= EDGE_POS_MAX) {
        return ATREE_ERR_LIMIT;
    }
    st = atree__u32vec_push(&t->mem, &cn->parents, id);
    if (st != ATREE_OK) {
        return st;
    }
    child_pos(n)[i] = cn->parents.len - 1; /* region 3 */
    (void)move_to_region(t, cn, c, cn->parents.len - 1, region_for(anchor, waker));
    t->edges++;
    return ATREE_OK;
}

/* Removes `id` from its i-th child's parent list in O(log fanout). Never
 * allocates. */
static void unlink_child(atree_t *t, atree__nid id, uint32_t i)
{
    struct atree__node *n = node_at(t, id);
    atree__nid c = n->children.data[i];
    struct atree__node *cn = node_at(t, c);
    uint32_t k = move_to_region(t, cn, c, edge_pos(child_pos(n)[i]), 3);
    uint32_t last = cn->parents.len - 1;
    if (k != last) {
        parents_swap(cn, k, last);
        fix_pos(t, cn, c, k);
    }
    cn->parents.len--;
    t->edges--;
}

/* Puts `id`'s i-th edge in the region its anchor flag and waker status
 * call for. Never allocates. */
static void place_edge(atree_t *t, atree__nid id, uint32_t i)
{
    struct atree__node *n = node_at(t, id);
    atree__nid c = n->children.data[i];
    uint32_t e = child_pos(n)[i];
    (void)move_to_region(t, node_at(t, c), c, edge_pos(e),
                         region_for(edge_anchored(e), is_waker(n, c)));
}

static void place_all_edges(atree_t *t, atree__nid id)
{
    uint32_t i;
    for (i = 0; i < node_at(t, id)->children.len; i++) {
        place_edge(t, id, i);
    }
}

/* Sets whether `id`'s i-th edge is the anchor edge. Never allocates. */
static void set_anchor(atree_t *t, atree__nid id, uint32_t i, bool anchor)
{
    struct atree__node *n = node_at(t, id);
    atree__nid c = n->children.data[i];
    uint32_t e = child_pos(n)[i];
    if (edge_anchored(e) != anchor) {
        (void)move_to_region(t, node_at(t, c), c, edge_pos(e), region_for(anchor, is_waker(n, c)));
    }
}

/* Changes an AND node's access child and moves the old and new edges
 * between waker regions. Never allocates. */
static void set_access_child(atree_t *t, atree__nid id, atree__nid c)
{
    struct atree__node *n = node_at(t, id);
    atree__nid old = n->access_child;
    uint32_t j;
    if (old == c) {
        return;
    }
    n->access_child = c;
    if (old != ATREE_NID_NONE && (j = child_index(n, old)) != UINT32_MAX) {
        place_edge(t, id, j);
    }
    if (c != ATREE_NID_NONE && (j = child_index(n, c)) != UINT32_MAX) {
        place_edge(t, id, j);
    }
}

/* The child with the fewest parents (ties: lower id). */
static uint32_t choose_anchor(const atree_t *t, const uint32_t *ids, uint32_t n)
{
    uint32_t best = 0;
    uint32_t i;
    for (i = 1; i < n; i++) {
        uint32_t pb = atree__nodeslab_at(&t->nodes, ids[best])->parents.len;
        uint32_t pi = atree__nodeslab_at(&t->nodes, ids[i])->parents.len;
        if (pi < pb || (pi == pb && ids[i] < ids[best])) {
            best = i;
        }
    }
    return best;
}

static atree_status_t level_inc(atree_t *t, uint32_t level)
{
    if (level > UINT16_MAX) {
        return ATREE_ERR_LIMIT;
    }
    while (t->level_counts.len <= level) {
        atree_status_t st = atree__u32vec_push(&t->mem, &t->level_counts, 0);
        if (st != ATREE_OK) {
            return st;
        }
    }
    t->level_counts.data[level]++;
    if (level > t->max_level) {
        t->max_level = level;
    }
    return ATREE_OK;
}

static void level_dec(atree_t *t, uint32_t level)
{
    t->level_counts.data[level]--;
    while (t->max_level > 0 && t->level_counts.data[t->max_level] == 0) {
        t->max_level--;
    }
}

static uint64_t inner_hash(uint8_t kind, const uint32_t *ids, uint32_t n)
{
    uint64_t h = atree__hash_u64(UINT64_C(0x696e6e6572) ^ kind);
    uint32_t i;
    for (i = 0; i < n; i++) {
        h = atree__hash_combine(h, ids[i]);
    }
    return h;
}

/* ---- predicate slab ----------------------------------------------------- */

static atree_status_t pred_slot_alloc(atree_t *t, const struct atree__pred *p, uint32_t *out)
{
    if (t->free_preds.len > 0) {
        *out = t->free_preds.data[--t->free_preds.len];
        *atree__predslab_at(&t->preds, *out) = *p;
        return ATREE_OK;
    }
    {
        atree_status_t st = atree__predslab_push(&t->mem, &t->preds, *p);
        if (st != ATREE_OK) {
            return st;
        }
        st = atree__u32slab_push(&t->mem, &t->leaf_pos, UINT32_MAX);
        if (st != ATREE_OK) {
            t->preds.len--;
            return st;
        }
    }
    *out = t->preds.len - 1;
    return ATREE_OK;
}

/* Releases the operand and recycles the slot (recycled slots have an
 * UNDEFINED operand, so freeing the slab at destroy is a no-op for them). */
static void pred_slot_release(atree_t *t, uint32_t slot)
{
    atree__pred_free(&t->mem, atree__predslab_at(&t->preds, slot));
    (void)atree__u32vec_push(&t->mem, &t->free_preds, slot);
}

/* ---- subscriptions ------------------------------------------------------ */

static const atree_id_t *sublist_ids(const struct atree__sublist *l)
{
    assert(l->cap != 0 || l->len <= 1); /* inline storage holds one id */
    return l->cap == 0 ? &l->u.one : l->u.many;
}

const atree_id_t *atree__node_subs(const atree_t *t, atree__nid id, uint32_t *n)
{
    uint32_t slot = *atree__u32slab_at(&t->sub_slot, id);
    const struct atree__sublist *l;
    if (slot == UINT32_MAX) {
        *n = 0;
        return NULL;
    }
    l = &t->sublists.data[slot];
    *n = l->len;
    return sublist_ids(l);
}

/* Appends an id. The first id is stored inline and never allocates; the
 * second moves both to a heap list, which then grows geometrically. */
static atree_status_t sublist_push(atree_t *t, struct atree__sublist *l, atree_id_t id)
{
    if (l->cap == 0) {
        atree_id_t first;
        atree_id_t *many;
        if (l->len == 0) {
            l->u.one = id;
            l->len = 1;
            return ATREE_OK;
        }
        first = l->u.one;
        many = atree__alloc_array(&t->mem, ATREE_VEC_MIN_CAP, sizeof *many);
        if (many == NULL) {
            return ATREE_ERR_NOMEM;
        }
        many[0] = first;
        many[1] = id;
        l->u.many = many;
        l->cap = ATREE_VEC_MIN_CAP;
        l->len = 2;
        return ATREE_OK;
    }
    if (l->len == l->cap) {
        atree_status_t st = ATREE_OK;
        if (l->len == UINT32_MAX) {
            return ATREE_ERR_LIMIT;
        }
        l->u.many = atree__vec_grow(&t->mem, l->u.many, sizeof *l->u.many, l->cap, l->len + 1,
                                    &l->cap, &st);
        if (st != ATREE_OK) {
            return st;
        }
    }
    l->u.many[l->len++] = id;
    return ATREE_OK;
}

/* Removes the id at `pos` (swap-remove). A heap list left with one id
 * returns to the inline form, so a node whose ids come and go keeps no
 * list behind. */
static void sublist_remove_at(atree_t *t, struct atree__sublist *l, uint32_t pos)
{
    if (l->cap == 0) {
        l->len = 0;
        return;
    }
    l->len--;
    if (pos != l->len) {
        l->u.many[pos] = l->u.many[l->len];
    }
    if (l->len <= 1) {
        atree_id_t keep = l->len == 1 ? l->u.many[0] : 0;
        atree__free_array(&t->mem, l->u.many, l->cap, sizeof *l->u.many);
        l->u.one = keep;
        l->cap = 0;
    }
}

static atree_status_t sub_attach(atree_t *t, atree__nid nid, atree_id_t id)
{
    uint32_t slot;
    atree_status_t st;

    slot = *atree__u32slab_at(&t->sub_slot, nid);
    if (slot == UINT32_MAX) {
        struct atree__sublist empty;
        empty.u.one = 0;
        empty.len = 0;
        empty.cap = 0;
        if (t->free_sublists.len > 0) {
            slot = t->free_sublists.data[--t->free_sublists.len];
            t->sublists.data[slot] = empty;
        } else {
            st = atree__sublistvec_push(&t->mem, &t->sublists, empty);
            if (st != ATREE_OK) {
                return st;
            }
            slot = t->sublists.len - 1;
        }
        *atree__u32slab_at(&t->sub_slot, nid) = slot;
    }
    /* A fresh list takes its first id inline, so a failure here leaves a
     * list that already held ids and nothing to undo. */
    st = sublist_push(t, &t->sublists.data[slot], id);
    if (st != ATREE_OK) {
        return st;
    }
    node_at(t, nid)->flags |= ATREE_NODE_HAS_SUBS;
    return ATREE_OK;
}

static bool sub_detach(atree_t *t, atree__nid nid, atree_id_t id)
{
    uint32_t slot;
    struct atree__sublist *list;
    const atree_id_t *ids;
    uint32_t pos;

    slot = *atree__u32slab_at(&t->sub_slot, nid);
    if (slot == UINT32_MAX) {
        return false;
    }
    list = &t->sublists.data[slot];
    ids = sublist_ids(list);
    for (pos = 0; pos < list->len && ids[pos] != id; pos++) {
    }
    if (pos == list->len) {
        return false;
    }
    sublist_remove_at(t, list, pos);
    if (list->len == 0) {
        *atree__u32slab_at(&t->sub_slot, nid) = UINT32_MAX;
        (void)atree__u32vec_push(&t->mem, &t->free_sublists, slot);
        node_at(t, nid)->flags &= (uint8_t)~ATREE_NODE_HAS_SUBS;
    }
    return true;
}

/* ---- deletion cascade (Alg. 5) ------------------------------------------ */

/* Removes a leaf from the phase-1 leaf list in O(1). */
static void leaves_remove(atree_t *t, atree__nid id)
{
    struct atree__node *n = node_at(t, id);
    uint32_t pos = *atree__u32slab_at(&t->leaf_pos, n->pred);
    uint32_t last = t->leaves.len - 1;
    if (pos != last) {
        atree__nid moved = t->leaves.data[last];
        t->leaves.data[pos] = moved;
        *atree__u32slab_at(&t->leaf_pos, node_at(t, moved)->pred) = pos;
    }
    t->leaves.len--;
    *atree__u32slab_at(&t->leaf_pos, n->pred) = UINT32_MAX;
}

/* Frees `start` if nothing uses it, then every child left unused, and so
 * on. The worklist must have been reserved (each node is pushed at most
 * once, when its use count hits zero, so nodes.len is a safe bound). */
static void cascade(atree_t *t, atree__nid start)
{
    t->worklist.len = 0;
    t->worklist.data[t->worklist.len++] = start;
    while (t->worklist.len > 0) {
        atree__nid id = t->worklist.data[--t->worklist.len];
        struct atree__node *n = node_at(t, id);
        uint32_t i;
        if (n->kind == ATREE_NODE_FREE || in_use(n)) {
            continue;
        }
        atree__idset_remove(t, n->hash, id);
        if (n->kind != ATREE_NODE_LEAF) {
            atree__cset_remove(t, atree__content_key(t, id), id);
        }
        if (n->kind == ATREE_NODE_LEAF) {
            if (indexed(t)) {
                atree__index_remove(t, id);
            }
            leaves_remove(t, id);
            pred_slot_release(t, n->pred);
            n = node_at(t, id);
        } else {
            for (i = 0; i < n->children.len; i++) {
                atree__nid c = n->children.data[i];
                unlink_child(t, id, i);
                if (!in_use(node_at(t, c))) {
                    t->worklist.data[t->worklist.len++] = c;
                }
            }
        }
        level_dec(t, n->level);
        node_release(t, id);
    }
}

/* ---- marks: per-node epoch stamps for O(1) set membership --------------- */

static atree_status_t marks_begin(atree_t *t)
{
    uint32_t i;
    while (t->mark.len < t->nodes.len) {
        atree_status_t st = atree__u32slab_push(&t->mem, &t->mark, 0);
        if (st != ATREE_OK) {
            return st;
        }
    }
    if (t->mark_epoch == UINT32_MAX) {
        for (i = 0; i < t->mark.len; i++) {
            *atree__u32slab_at(&t->mark, i) = 0;
        }
        t->mark_epoch = 0;
    }
    t->mark_epoch++;
    return ATREE_OK;
}

static void mark_set(atree_t *t, const uint32_t *ids, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        *atree__u32slab_at(&t->mark, ids[i]) = t->mark_epoch;
    }
}

static bool marked(const atree_t *t, atree__nid id)
{
    return id < t->mark.len && *atree__u32slab_at(&t->mark, id) == t->mark_epoch;
}

/* ---- journal: everything an insert changes, for exact rollback ---------- */

static atree_status_t journal_push(atree_t *t, struct journalvec *j,
                                   const struct atree__journal_entry *e)
{
    return journalvec_push(&t->mem, j, *e);
}

static atree_status_t journal_created(atree_t *t, struct journalvec *j, atree__nid id)
{
    struct atree__journal_entry e;
    memset(&e, 0, sizeof e);
    e.kind = ATREE_J_CREATED;
    e.node = id;
    atree__u32vec_init(&e.old_children);
    return journal_push(t, j, &e);
}

/* ---- index construction (Alg. 1/4) -------------------------------------- */

/* Rank for choosing an AND node's access child (PLAN §4.4): the child most
 * likely to be false wakes the node least often. */
static int wake_rank(const atree_t *t, atree__nid id)
{
    const struct atree__node *n = atree__nodeslab_at(&t->nodes, id);
    if (n->kind == ATREE_NODE_LEAF) {
        return atree__pred_wake_rank(atree__predslab_at(&t->preds, n->pred));
    }
    return n->kind == ATREE_NODE_AND ? 2 : 3;
}

/* Ties: shallower node, then fewer children (cheaper to have evaluated),
 * then lower id. Deterministic for a given insertion sequence. */
static bool better_access(const atree_t *t, atree__nid cand, int cand_rank, atree__nid best,
                          int best_rank)
{
    const struct atree__node *c = atree__nodeslab_at(&t->nodes, cand);
    const struct atree__node *b = atree__nodeslab_at(&t->nodes, best);
    if (cand_rank != best_rank) {
        return cand_rank < best_rank;
    }
    if (c->level != b->level) {
        return c->level < b->level;
    }
    if (c->children.len != b->children.len) {
        return c->children.len < b->children.len;
    }
    return cand < best;
}

static atree__nid choose_access_child(const atree_t *t, const uint32_t *ids, uint32_t n)
{
    atree__nid best = ids[0];
    int best_rank = wake_rank(t, best);
    uint32_t i;
    for (i = 1; i < n; i++) {
        int r = wake_rank(t, ids[i]);
        if (better_access(t, ids[i], r, best, best_rank)) {
            best = ids[i];
            best_rank = r;
        }
    }
    return best;
}

static void reset_access_child(atree_t *t, atree__nid id)
{
    const struct atree__node *n = atree__nodeslab_at(&t->nodes, id);
    atree__nid c = ATREE_NID_NONE;
    if (n->kind == ATREE_NODE_AND && (t->flags & ATREE_FLAG_NO_PROPAGATION_ON_DEMAND) == 0) {
        c = choose_access_child(t, n->children.data, n->children.len);
    }
    set_access_child(t, id, c);
}

/* Recomputes levels upward from `start` after its children changed (both
 * directions), journaling every change so rollback can restore them. */
static atree_status_t relevel(atree_t *t, atree__nid start, struct journalvec *j)
{
    atree_status_t st = ATREE_OK;
    t->worklist.len = 0;
    st = atree__u32vec_push(&t->mem, &t->worklist, start);
    while (st == ATREE_OK && t->worklist.len > 0) {
        atree__nid id = t->worklist.data[--t->worklist.len];
        struct atree__node *n = node_at(t, id);
        uint32_t level = 0;
        uint32_t i;
        for (i = 0; i < n->children.len; i++) {
            uint32_t cl = atree__nodeslab_at(&t->nodes, n->children.data[i])->level;
            if (cl > level) {
                level = cl;
            }
        }
        level++;
        if (level == n->level) {
            continue;
        }
        {
            struct atree__journal_entry e;
            memset(&e, 0, sizeof e);
            e.kind = ATREE_J_LEVEL;
            e.node = id;
            e.old_level = n->level;
            atree__u32vec_init(&e.old_children);
            st = journal_push(t, j, &e);
            if (st != ATREE_OK) {
                break;
            }
        }
        st = level_inc(t, level);
        if (st != ATREE_OK) {
            break;
        }
        n = node_at(t, id);
        level_dec(t, n->level);
        n->level = (uint16_t)level;
        for (i = 0; st == ATREE_OK && i < n->parents.len; i++) {
            st = atree__u32vec_push(&t->mem, &t->worklist, n->parents.data[i]);
        }
    }
    return st;
}

/* Undo helper for a node whose construction failed part-way. */
static void abort_new_node(atree_t *t, atree__nid id, uint32_t pred_slot, bool in_leaves,
                           bool in_index, bool in_identity, bool in_content, bool in_levels,
                           uint32_t linked)
{
    struct atree__node *n = node_at(t, id);
    uint32_t i;
    if (in_levels) {
        level_dec(t, n->level);
    }
    if (in_content) {
        atree__cset_remove(t, atree__content_key(t, id), id);
    }
    if (in_identity) {
        atree__idset_remove(t, n->hash, id);
    }
    if (in_index) {
        atree__index_remove(t, id);
    }
    if (in_leaves) {
        leaves_remove(t, id);
    }
    if (pred_slot != UINT32_MAX) {
        pred_slot_release(t, pred_slot);
        n = node_at(t, id);
    }
    (void)n;
    for (i = 0; i < linked; i++) {
        unlink_child(t, id, i);
    }
    node_release(t, id);
}

/* ---- lookup first (Alg. 4 lines 1-4) ------------------------------------ */

enum { LOOKUP_NONE = 0, LOOKUP_FOUND = 1, LOOKUP_ABSENT = 2 };

/* A node's key as a member of a flat set: leaves by predicate hash, inner
 * nodes by content key. */
static uint64_t member_key(const atree_t *t, atree__nid id)
{
    const struct atree__node *n = atree__nodeslab_at(&t->nodes, id);
    return n->kind == ATREE_NODE_LEAF ? n->hash : atree__content_key(t, id);
}

static uint64_t member_mix(uint64_t key)
{
    return atree__hash_u64(key ^ UINT64_C(0x6d656d62));
}

/* Sum over the flat members of an inner node with children `ids`: a child
 * of the same operator contributes its own flat members (its sum), any
 * other child contributes itself. Wrapping addition makes the sum
 * order-free. */
static uint64_t flat_sum(const atree_t *t, uint8_t kind, const uint32_t *ids, uint32_t n)
{
    uint64_t sum = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        const struct atree__node *c = atree__nodeslab_at(&t->nodes, ids[i]);
        sum += c->kind == kind ? *atree__u64slab_at(&t->csum, ids[i])
                               : member_mix(member_key(t, ids[i]));
    }
    return sum;
}

static uint8_t node_kind_of(const atree_expr_t *e)
{
    return e->kind == ATREE_EXPR_AND ? ATREE_NODE_AND : ATREE_NODE_OR;
}

static uint64_t content_key_of(uint8_t kind, uint64_t sum)
{
    uint64_t tag = kind == ATREE_NODE_AND ? UINT64_C(0xa11d) : UINT64_C(0x0e0e);
    return atree__hash_combine(atree__hash_u64(tag), sum);
}

/* Computes the content hash of every subexpression of the normalized
 * expression from its own text and resolves the leaves through the identity
 * table: one probe per leaf, no allocation, no string interning (a literal
 * the tree has never seen simply matches no leaf). */
static atree_status_t prehash(atree_t *t, atree_expr_t *e)
{
    uint32_t i;
    atree_status_t st;
    e->lookup_state = LOOKUP_NONE;
    e->chash_ok = 0;
    if (e->kind == ATREE_EXPR_PRED) {
        struct atree__probe probe;
        atree__nid id;
        probe.kind = ATREE_NODE_LEAF;
        probe.hash = e->hash; /* == atree__expr_leaf_hash(e), computed at build */
        probe.pred = NULL;
        probe.leaf = e;
        probe.children = NULL;
        probe.nchildren = 0;
        id = atree__idset_find(t, &probe);
        e->chash = probe.hash;
        e->chash_ok = 1;
        e->resolved = id;
        e->lookup_state = id == ATREE_NID_NONE ? LOOKUP_ABSENT : LOOKUP_FOUND;
        return ATREE_OK;
    }
    if (e->kind == ATREE_EXPR_AND || e->kind == ATREE_EXPR_OR) {
        uint64_t sum = 0;
        bool ok = true;
        for (i = 0; i < e->nchildren; i++) {
            st = prehash(t, e->children[i]);
            if (st != ATREE_OK) {
                return st;
            }
            ok = ok && e->children[i]->chash_ok;
            sum += member_mix(e->children[i]->chash);
        }
        if (ok) {
            e->chash = content_key_of(node_kind_of(e), sum);
            e->chash_ok = 1;
        } else {
            e->lookup_state = LOOKUP_ABSENT;
        }
    }
    return ATREE_OK;
}

static atree_status_t lookup_inner(atree_t *t, atree_expr_t *e, atree__nid *out);

/* The node an operand subexpression stands for, or ATREE_NID_NONE. */
static atree_status_t resolve_operand(atree_t *t, atree_expr_t *e, atree__nid *out)
{
    if (e->lookup_state == LOOKUP_FOUND) {
        *out = e->resolved;
        return ATREE_OK;
    }
    if (e->lookup_state == LOOKUP_ABSENT || e->kind == ATREE_EXPR_PRED) {
        *out = ATREE_NID_NONE;
        return ATREE_OK;
    }
    return lookup_inner(t, e, out);
}

/* Does node `id` have exactly the flat members that `e`'s operands resolve
 * to? Collects the node's flat members (walking same-operator children)
 * and the operands' nodes into the scratch vector and compares the two
 * sorted sets. */
static atree_status_t verify_inner(atree_t *t, atree_expr_t *e, atree__nid id, bool *ok)
{
    uint8_t kind = node_kind_of(e);
    uint32_t base = t->scratch.len;
    uint32_t nflat;
    uint32_t i;
    atree_status_t st = ATREE_OK;

    *ok = false;
    if (atree__nodeslab_at(&t->nodes, id)->kind != kind) {
        return ATREE_OK;
    }
    t->worklist.len = 0;
    st = atree__u32vec_push(&t->mem, &t->worklist, id);
    while (st == ATREE_OK && t->worklist.len > 0) {
        atree__nid cur = t->worklist.data[--t->worklist.len];
        const struct atree__node *n = atree__nodeslab_at(&t->nodes, cur);
        for (i = 0; st == ATREE_OK && i < n->children.len; i++) {
            atree__nid c = n->children.data[i];
            if (atree__nodeslab_at(&t->nodes, c)->kind == kind) {
                st = atree__u32vec_push(&t->mem, &t->worklist, c);
            } else {
                st = atree__u32vec_push(&t->mem, &t->scratch, c);
                if (st == ATREE_OK && t->scratch.len - base > e->nchildren) {
                    t->scratch.len = base; /* more members than operands */
                    return ATREE_OK;
                }
            }
        }
    }
    if (st != ATREE_OK) {
        t->scratch.len = base;
        return st;
    }
    nflat = atree__sort_unique_u32(t->scratch.data + base, t->scratch.len - base);
    t->scratch.len = base + nflat;
    if (nflat != e->nchildren) {
        t->scratch.len = base;
        return ATREE_OK;
    }
    for (i = 0; i < e->nchildren; i++) {
        atree__nid c;
        st = resolve_operand(t, e->children[i], &c);
        if (st != ATREE_OK || c == ATREE_NID_NONE) {
            t->scratch.len = base;
            return st;
        }
        st = atree__u32vec_push(&t->mem, &t->scratch, c);
        if (st != ATREE_OK) {
            t->scratch.len = base;
            return st;
        }
    }
    {
        uint32_t nops = atree__sort_unique_u32(t->scratch.data + base + nflat, e->nchildren);
        *ok = nops == nflat &&
            memcmp(t->scratch.data + base, t->scratch.data + base + nflat,
                   (size_t)nflat * sizeof(uint32_t)) == 0;
    }
    t->scratch.len = base;
    return ATREE_OK;
}

/* Alg. 4 lines 1-4 for an inner subexpression: the existing node with the
 * same flat content, or ATREE_NID_NONE. Every hash hit is verified. */
static atree_status_t lookup_inner(atree_t *t, atree_expr_t *e, atree__nid *out)
{
    uint32_t cursor = UINT32_MAX;
    atree__nid id;
    *out = ATREE_NID_NONE;
    if (e->lookup_state == LOOKUP_FOUND) {
        *out = e->resolved;
        return ATREE_OK;
    }
    if (e->lookup_state == LOOKUP_ABSENT || !e->chash_ok) {
        e->lookup_state = LOOKUP_ABSENT;
        return ATREE_OK;
    }
    while ((id = atree__cset_next(t, e->chash, &cursor)) != ATREE_NID_NONE) {
        bool ok;
        atree_status_t st = verify_inner(t, e, id, &ok);
        if (st != ATREE_OK) {
            return st;
        }
        if (ok) {
            e->resolved = id;
            e->lookup_state = LOOKUP_FOUND;
            *out = id;
            return ATREE_OK;
        }
    }
    e->lookup_state = LOOKUP_ABSENT;
    return ATREE_OK;
}

static atree_status_t build(atree_t *t, atree_expr_t *e, struct journalvec *j, atree__nid *out);

static atree_status_t build_leaf(atree_t *t, atree_expr_t *e, struct journalvec *j, atree__nid *out)
{
    struct atree__pred p;
    struct atree__probe probe;
    atree__nid id;
    uint32_t slot = UINT32_MAX;
    struct atree__node *n;
    atree_status_t st;

    if (e->lookup_state == LOOKUP_FOUND) {
        *out = e->resolved;
        return ATREE_OK;
    }
    st = atree__expr_pred_intern(e, &t->mem, &t->strings, &p);
    if (st != ATREE_OK) {
        return st;
    }
    st = atree__pred_check(&t->attrs, &p);
    if (st != ATREE_OK) {
        atree__pred_free(&t->mem, &p);
        return st;
    }
    probe.kind = ATREE_NODE_LEAF;
    probe.hash = e->hash; /* == atree__pred_content_hash(&p): same bytes, interned or not */
    probe.pred = &p;
    probe.leaf = NULL;
    probe.children = NULL;
    probe.nchildren = 0;
    id = atree__idset_find(t, &probe);
    if (id != ATREE_NID_NONE) {
        atree__pred_free(&t->mem, &p);
        *out = id;
        return ATREE_OK;
    }

    st = node_alloc(t, &id);
    if (st != ATREE_OK) {
        atree__pred_free(&t->mem, &p);
        return st;
    }
    st = pred_slot_alloc(t, &p, &slot);
    if (st != ATREE_OK) {
        atree__pred_free(&t->mem, &p);
        node_release(t, id);
        return st;
    }
    n = node_at(t, id);
    n->kind = ATREE_NODE_LEAF;
    n->level = 1;
    n->hash = probe.hash;
    n->pred = slot;
    st = atree__u32vec_push(&t->mem, &t->leaves, id);
    if (st != ATREE_OK) {
        abort_new_node(t, id, slot, false, false, false, false, false, 0);
        return st;
    }
    *atree__u32slab_at(&t->leaf_pos, slot) = t->leaves.len - 1;
    if (indexed(t)) {
        st = atree__index_add(t, id);
        if (st != ATREE_OK) {
            abort_new_node(t, id, slot, true, false, false, false, false, 0);
            return st;
        }
    }
    st = atree__idset_insert(t, probe.hash, id);
    if (st != ATREE_OK) {
        abort_new_node(t, id, slot, true, indexed(t), false, false, false, 0);
        return st;
    }
    st = level_inc(t, 1);
    if (st != ATREE_OK) {
        abort_new_node(t, id, slot, true, indexed(t), true, false, false, 0);
        return st;
    }
    st = journal_created(t, j, id);
    if (st != ATREE_OK) {
        abort_new_node(t, id, slot, true, indexed(t), true, false, true, 0);
        return st;
    }
    *out = id;
    return ATREE_OK;
}

/* Alg. 2 (paper §4.2.2), greedy set cover: while some existing node of the
 * same kind has all its children in the operand set, replace those children
 * by it, largest cover first. Candidates are parents of the operands. The
 * number of candidates examined is bounded by max_adjust_candidates. */
static atree_status_t reorganize(atree_t *t, uint8_t kind, uint32_t *ids, uint32_t *n)
{
    bool changed = false;
    size_t examined = 0;
    atree_status_t st;

    for (;;) {
        atree__nid best = ATREE_NID_NONE;
        uint32_t best_len = 0;
        uint32_t i;
        uint32_t j;
        uint32_t m;

        if (*n < 3) {
            break; /* a cover of 2 operands is the whole set: identity finds it */
        }
        st = marks_begin(t);
        if (st != ATREE_OK) {
            return st;
        }
        mark_set(t, ids, *n);
        /* A cover S contains its own anchor child a, and S sits in the
         * anchored prefix of a's parent list, so scanning the anchored
         * parents of every operand finds every cover exactly, without ever
         * walking the long parent lists of popular leaves. */
        for (i = 0; i < *n && examined < t->max_adjust_candidates; i++) {
            const struct atree__node *u = atree__nodeslab_at(&t->nodes, ids[i]);
            for (j = 0; j < u->end_a && examined < t->max_adjust_candidates; j++) {
                atree__nid pid = u->parents.data[j];
                const struct atree__node *p = atree__nodeslab_at(&t->nodes, pid);
                uint32_t k;
                bool sub = true;
                examined++;
                if (p->kind != kind || p->children.len < 2 || p->children.len >= *n ||
                    p->children.len <= best_len || marked(t, pid)) {
                    continue;
                }
                for (k = 0; sub && k < p->children.len; k++) {
                    sub = marked(t, p->children.data[k]);
                }
                if (sub) {
                    best = pid;
                    best_len = p->children.len;
                }
            }
        }
        if (examined >= t->max_adjust_candidates) {
            t->adjust_candidates_skipped++;
        }
        if (best == ATREE_NID_NONE) {
            break;
        }
        /* ids <- (ids \ children(best)) + {best}; children(best) are sorted,
         * and at least two of them leave, so the set never grows. */
        {
            const struct atree__node *b = atree__nodeslab_at(&t->nodes, best);
            m = 0;
            for (i = 0; i < *n; i++) {
                if (!atree__bsearch_u32(b->children.data, b->children.len, ids[i])) {
                    ids[m++] = ids[i];
                }
            }
            ids[m++] = best;
            *n = atree__sort_unique_u32(ids, m);
        }
        changed = true;
    }
    if (changed) {
        t->reorganized++;
    }
    return ATREE_OK;
}

/* Self-adjust step: parent P (same kind) has children strictly containing
 * N's; rewrite P.children <- (P.children \ N.children) + {N}. Skipped when
 * the rewritten set already exists as another node (uniqueness). Every
 * change is journaled. */
static atree_status_t rewire(atree_t *t, atree__nid pid, atree__nid nid, struct journalvec *j)
{
    struct atree__node *p = node_at(t, pid);
    const struct atree__node *n = atree__nodeslab_at(&t->nodes, nid);
    struct atree__u32vec tmp;
    struct atree__u32vec nc;
    struct atree__probe probe;
    struct atree__journal_entry e;
    uint64_t new_hash;
    uint64_t old_hash = p->hash;
    uint32_t i;
    uint32_t k;
    uint32_t idx_n;
    atree_status_t st;

    /* new child id set; P's children strictly contain N's (self-adjust's
     * precondition), which keeps the reserve size positive */
    assert(p->children.len > n->children.len);
    atree__u32vec_init(&tmp);
    st = atree__u32vec_reserve(&t->mem, &tmp, p->children.len - n->children.len + 1);
    if (st != ATREE_OK) {
        return st;
    }
    for (i = 0; i < p->children.len; i++) {
        if (!atree__bsearch_u32(n->children.data, n->children.len, p->children.data[i])) {
            tmp.data[tmp.len++] = p->children.data[i];
        }
    }
    tmp.data[tmp.len++] = nid;
    tmp.len = atree__sort_unique_u32(tmp.data, tmp.len);
    new_hash = inner_hash(p->kind, tmp.data, tmp.len);
    probe.kind = p->kind;
    probe.hash = new_hash;
    probe.pred = NULL;
    probe.leaf = NULL;
    probe.children = tmp.data;
    probe.nchildren = tmp.len;
    if (atree__idset_find(t, &probe) != ATREE_NID_NONE) {
        atree__u32vec_free(&t->mem, &tmp); /* would duplicate an existing node */
        return ATREE_OK;
    }
    st = children_alloc(t, tmp.data, tmp.len, &nc);
    atree__u32vec_free(&t->mem, &tmp);
    if (st != ATREE_OK) {
        return st;
    }
    /* positions of the kept children carry over; N gets its slot below */
    for (i = 0, k = 0; i < nc.len; i++) {
        if (nc.data[i] == nid) {
            continue;
        }
        while (p->children.data[k] != nc.data[i]) {
            k++;
        }
        child_pos_of_vec(&nc)[i] = child_pos(p)[k];
    }
    idx_n = child_index_in_vec(&nc, nid);

    /* journal first: old child set moves into the entry */
    memset(&e, 0, sizeof e);
    e.kind = ATREE_J_REWIRED;
    e.node = pid;
    e.added = nid;
    e.old_children = p->children;
    e.old_hash = old_hash;
    e.old_level = p->level;
    e.old_access = p->access_child;
    st = journal_push(t, j, &e);
    if (st != ATREE_OK) {
        children_free(t, &nc);
        return st;
    }
    /* link P to N (may allocate) before any non-allocating unlink */
    if (node_at(t, nid)->parents.len >= EDGE_POS_MAX) {
        j->len--;
        children_free(t, &nc);
        return ATREE_ERR_LIMIT;
    }
    st = atree__u32vec_push(&t->mem, &node_at(t, nid)->parents, pid);
    if (st != ATREE_OK) {
        j->len--; /* entry not yet effective; P still owns its children */
        children_free(t, &nc);
        return st;
    }
    child_pos_of_vec(&nc)[idx_n] = node_at(t, nid)->parents.len - 1;
    /* new identity entry before removing the old one (may allocate); one
     * slot is reserved for rollback to re-file P under old_hash without
     * allocating, in case the table rehashes later in this insert */
    if (new_hash != old_hash) {
        t->identity.reserved++;
        st = atree__idset_insert(t, new_hash, pid);
        if (st != ATREE_OK) {
            t->identity.reserved--;
            node_at(t, nid)->parents.len--; /* P was pushed last */
            j->len--;
            children_free(t, &nc);
            return st;
        }
    }
    /* From here on nothing allocates until relevel; the journal entry is live. */
    t->edges++;
    p = node_at(t, pid);
    for (i = 0; i < p->children.len; i++) {
        if (atree__bsearch_u32(n->children.data, n->children.len, p->children.data[i])) {
            unlink_child(t, pid, i);
            p = node_at(t, pid);
        }
    }
    p->children = nc;
    p->hash = new_hash;
    if (new_hash != old_hash) {
        atree__idset_remove(t, old_hash, pid);
    }
    /* exactly one anchor edge: the kept children carry their old flags, the
     * new edge has none; re-choose and adjust (no allocation), then place
     * every edge in its waker region for the new access child */
    {
        uint32_t ai = choose_anchor(t, nc.data, nc.len);
        for (i = 0; i < node_at(t, pid)->children.len; i++) {
            set_anchor(t, pid, i, i == ai);
        }
    }
    reset_access_child(t, pid);
    place_all_edges(t, pid);
    t->self_adjusted++;
    return relevel(t, pid, j);
}

/* Alg. 3 (paper §4.2.3): every parent with the same operator and a strict
 * superset of N's children is rewired to use N. Such a parent contains all
 * of N's children, so it is in the parent list of each of them: scanning
 * the child with the fewest parents finds every candidate exactly, at the
 * lowest cost. */
static atree_status_t self_adjust(atree_t *t, atree__nid nid, struct journalvec *j)
{
    size_t examined = 0;
    uint32_t i;
    atree_status_t st = ATREE_OK;
    const struct atree__node *n = atree__nodeslab_at(&t->nodes, nid);
    atree__nid cid = n->children.data[0];
    uint32_t k = 0;

    for (i = 1; i < n->children.len; i++) {
        atree__nid c = n->children.data[i];
        if (atree__nodeslab_at(&t->nodes, c)->parents.len <
            atree__nodeslab_at(&t->nodes, cid)->parents.len) {
            cid = c;
        }
    }
    /* A rewired parent drops out of this child's parent list (its link
     * moves to N), so iterate by index and re-read the list each time. */
    while (st == ATREE_OK && k < atree__nodeslab_at(&t->nodes, cid)->parents.len &&
           examined < t->max_adjust_candidates) {
        atree__nid pid = atree__nodeslab_at(&t->nodes, cid)->parents.data[k];
        const struct atree__node *p = atree__nodeslab_at(&t->nodes, pid);
        bool super = true;
        uint32_t m;
        n = atree__nodeslab_at(&t->nodes, nid);
        examined++;
        if (pid == nid || p->kind != n->kind || p->children.len <= n->children.len) {
            k++;
            continue;
        }
        for (m = 0; super && m < n->children.len; m++) {
            super = atree__bsearch_u32(p->children.data, p->children.len, n->children.data[m]);
        }
        if (!super) {
            k++;
            continue;
        }
        {
            uint32_t before = atree__nodeslab_at(&t->nodes, cid)->parents.len;
            st = rewire(t, pid, nid, j);
            if (atree__nodeslab_at(&t->nodes, cid)->parents.len == before) {
                k++; /* skipped (duplicate) or failed: move on */
            }
        }
    }
    if (examined >= t->max_adjust_candidates) {
        t->adjust_candidates_skipped++;
    }
    return st;
}

static atree_status_t build_inner(atree_t *t, atree_expr_t *e, struct journalvec *j,
                                  atree__nid *out)
{
    uint32_t *ids;
    uint32_t nids;
    uint32_t base;
    struct atree__probe probe;
    uint8_t kind = e->kind == ATREE_EXPR_AND ? ATREE_NODE_AND : ATREE_NODE_OR;
    atree__nid id;
    struct atree__node *n;
    uint32_t i;
    uint32_t level = 0;
    uint32_t anchor_i;
    atree_status_t st;

    /* Alg. 4 lines 1-4: an existing node for this subexpression is returned
     * without visiting its operands. */
    st = lookup_inner(t, e, &id);
    if (st != ATREE_OK) {
        return st;
    }
    if (id != ATREE_NID_NONE) {
        *out = id;
        return ATREE_OK;
    }

    /* The operand ids live on the scratch stack while the operands are
     * built (nested builds push above and pop back), so no temporary is
     * allocated per inner node. */
    base = t->scratch.len;
    for (i = 0; i < e->nchildren; i++) {
        atree__nid cid;
        st = build(t, e->children[i], j, &cid);
        if (st == ATREE_OK) {
            st = atree__u32vec_push(&t->mem, &t->scratch, cid);
        }
        if (st != ATREE_OK) {
            t->scratch.len = base;
            return st;
        }
    }
    ids = t->scratch.data + base;
    nids = atree__sort_unique_u32(ids, t->scratch.len - base);
    if (nids == 1) {
        *out = ids[0];
        t->scratch.len = base;
        return ATREE_OK;
    }
    if ((t->flags & ATREE_FLAG_NO_REORGANIZE) == 0) {
        st = reorganize(t, kind, ids, &nids);
        if (st != ATREE_OK) {
            t->scratch.len = base;
            return st;
        }
        if (nids == 1) {
            *out = ids[0];
            t->scratch.len = base;
            return ATREE_OK;
        }
    }
    probe.kind = kind;
    probe.hash = inner_hash(kind, ids, nids);
    probe.pred = NULL;
    probe.leaf = NULL;
    probe.children = ids;
    probe.nchildren = nids;
    id = atree__idset_find(t, &probe);
    if (id != ATREE_NID_NONE) {
        t->scratch.len = base;
        *out = id;
        return ATREE_OK;
    }

    st = node_alloc(t, &id);
    if (st != ATREE_OK) {
        t->scratch.len = base;
        return st;
    }
    n = node_at(t, id);
    n->kind = kind;
    n->hash = probe.hash;
    st = children_alloc(t, ids, nids, &n->children);
    t->scratch.len = base;
    if (st != ATREE_OK) {
        node_release(t, id);
        return st;
    }
    {
        const struct atree__node *nn = node_at(t, id);
        anchor_i = choose_anchor(t, nn->children.data, nn->children.len);
    }
    /* AND nodes with propagation on demand wake through their access child
     * only, chosen below once every edge exists; link them as non-wakers. */
    for (i = 0; i < node_at(t, id)->children.len; i++) {
        bool waker = kind == ATREE_NODE_OR || (t->flags & ATREE_FLAG_NO_PROPAGATION_ON_DEMAND) != 0;
        st = link_child(t, id, i, i == anchor_i, waker);
        if (st != ATREE_OK) {
            abort_new_node(t, id, UINT32_MAX, false, false, false, false, false, i);
            return st;
        }
        {
            uint32_t cl = node_at(t, node_at(t, id)->children.data[i])->level;
            if (cl > level) {
                level = cl;
            }
        }
    }
    n = node_at(t, id);
    if (level >= UINT16_MAX) {
        abort_new_node(t, id, UINT32_MAX, false, false, false, false, false, n->children.len);
        return ATREE_ERR_LIMIT;
    }
    n->level = (uint16_t)(level + 1);
    reset_access_child(t, id);
    *atree__u64slab_at(&t->csum, id) =
        flat_sum(t, kind, node_at(t, id)->children.data, node_at(t, id)->children.len);
    st = atree__idset_insert(t, probe.hash, id);
    if (st != ATREE_OK) {
        abort_new_node(t, id, UINT32_MAX, false, false, false, false, false, n->children.len);
        return st;
    }
    st = atree__cset_insert(t, atree__content_key(t, id), id);
    if (st != ATREE_OK) {
        abort_new_node(t, id, UINT32_MAX, false, false, true, false, false,
                       node_at(t, id)->children.len);
        return st;
    }
    st = level_inc(t, level + 1);
    if (st != ATREE_OK) {
        abort_new_node(t, id, UINT32_MAX, false, false, true, true, false,
                       node_at(t, id)->children.len);
        return st;
    }
    st = journal_created(t, j, id);
    if (st != ATREE_OK) {
        abort_new_node(t, id, UINT32_MAX, false, false, true, true, true,
                       node_at(t, id)->children.len);
        return st;
    }
    if ((t->flags & ATREE_FLAG_NO_SELF_ADJUST) == 0) {
        st = self_adjust(t, id, j); /* failures roll back through the journal */
        if (st != ATREE_OK) {
            return st;
        }
    }
    *out = id;
    return ATREE_OK;
}

static atree_status_t build(atree_t *t, atree_expr_t *e, struct journalvec *j, atree__nid *out)
{
    switch ((enum atree__expr_kind)e->kind) {
    case ATREE_EXPR_PRED:
        return build_leaf(t, e, j, out);
    case ATREE_EXPR_AND:
    case ATREE_EXPR_OR:
        return build_inner(t, e, j, out);
    case ATREE_EXPR_TRUE:
    case ATREE_EXPR_FALSE:
    case ATREE_EXPR_NOT:
    case ATREE_EXPR_XOR:
    case ATREE_EXPR_XNOR:
    default:
        return ATREE_ERR_INVALID_ARG; /* normalization removed these */
    }
}

static void recompute_max_level(atree_t *t)
{
    uint32_t l = t->level_counts.len;
    t->max_level = 0;
    while (l > 1) {
        l--;
        if (t->level_counts.data[l] != 0) {
            t->max_level = l;
            break;
        }
    }
}

/* Undoes the journal newest-first. Nothing here allocates: rewired parents
 * re-enter the parent lists they left (whose capacity is intact because
 * every later push has already been undone), identity entries return to
 * their tombstones, and created nodes are released through cascade over a
 * worklist reserved before the insert began. */
static void rollback(atree_t *t, struct journalvec *j)
{
    uint32_t i = j->len;
    while (i > 0) {
        struct atree__journal_entry *e = &j->data[--i];
        switch (e->kind) {
        case ATREE_J_LEVEL: {
            struct atree__node *n = node_at(t, e->node);
            t->level_counts.data[n->level]--;
            t->level_counts.data[e->old_level]++;
            n->level = (uint16_t)e->old_level;
            break;
        }
        case ATREE_J_REWIRED: {
            struct atree__node *p = node_at(t, e->node);
            uint32_t idx_n = child_index(p, e->added);
            uint32_t old_anchor = UINT32_MAX;
            uint32_t k;
            for (k = 0; k < e->old_children.len; k++) {
                if (edge_anchored(child_pos_of_vec(&e->old_children)[k])) {
                    old_anchor = k;
                }
            }
            if (idx_n != UINT32_MAX) {
                unlink_child(t, e->node, idx_n);
                p = node_at(t, e->node);
            }
            for (k = 0; k < e->old_children.len; k++) {
                atree__nid cid = e->old_children.data[k];
                uint32_t cur = child_index(p, cid);
                if (cur == UINT32_MAX) {
                    /* removed by the rewire: re-enter the slot vacated earlier */
                    struct atree__node *c = node_at(t, cid);
                    assert(c->parents.len < c->parents.cap); /* its slot is intact */
                    (void)atree__u32vec_push(&t->mem, &c->parents, e->node);
                    child_pos_of_vec(&e->old_children)[k] = c->parents.len - 1;
                    t->edges++;
                } else {
                    /* kept: its current position (and flag) is authoritative */
                    child_pos_of_vec(&e->old_children)[k] = child_pos(p)[cur];
                }
            }
            if (p->hash != e->old_hash) {
                atree__idset_remove(t, p->hash, e->node);
                atree__idset_reinsert(t, e->old_hash, e->node); /* reserved by the rewire */
            }
            children_free(t, &p->children);
            p->children = e->old_children;
            atree__u32vec_init(&e->old_children);
            p->hash = e->old_hash;
            for (k = 0; k < node_at(t, e->node)->children.len; k++) {
                set_anchor(t, e->node, k, k == old_anchor);
            }
            set_access_child(t, e->node, e->old_access);
            place_all_edges(t, e->node);
            t->self_adjusted--;
            break;
        }
        case ATREE_J_CREATED:
        default: {
            const struct atree__node *n = atree__nodeslab_at(&t->nodes, e->node);
            if (n->kind != ATREE_NODE_FREE && !in_use(n)) {
                cascade(t, e->node);
            }
            break;
        }
        }
    }
    recompute_max_level(t);
}

/* Releases what the journal still owns after a successful insert. */
static void journal_commit(atree_t *t, struct journalvec *j)
{
    uint32_t i;
    for (i = 0; i < j->len; i++) {
        if (j->data[i].kind == ATREE_J_REWIRED) {
            children_free(t, &j->data[i].old_children);
        }
    }
    j->len = 0; /* the vector itself is reused by the next insert */
    t->identity.reserved = 0;
}

static uint32_t expr_node_count(const atree_expr_t *e)
{
    uint32_t n = 1;
    uint32_t i;
    for (i = 0; i < e->nchildren; i++) {
        n += expr_node_count(e->children[i]);
    }
    return n;
}

static void set_err(atree_error_t *err, atree_status_t st, const char *msg)
{
    if (err != NULL && st != ATREE_OK) {
        atree__error_set(err, st, SIZE_MAX, 0, msg);
    }
}

static void reset_err(atree_error_t *err)
{
    if (err != NULL) {
        err->status = ATREE_OK;
        err->offset = SIZE_MAX;
        err->length = 0;
        err->message[0] = '\0';
    }
}

atree_status_t atree_insert_expr(atree_t *t, atree_id_t id, const atree_expr_t *expr,
                                 atree_error_t *err)
{
    atree_expr_t *norm = NULL;
    atree_status_t st = ATREE_OK;

    reset_err(err);
    if (t == NULL || expr == NULL) {
        set_err(err, ATREE_ERR_INVALID_ARG, "tree or expression is NULL");
        return ATREE_ERR_INVALID_ARG;
    }
    if (expr->tree != t) {
        set_err(err, ATREE_ERR_INVALID_ARG, "expression was built for a different tree");
        return ATREE_ERR_INVALID_ARG;
    }
    atree__wrlock(t);
    if (t->norm_arena == NULL) {
        st = atree__arena_new(t, &t->norm_arena);
    }
    if (st == ATREE_OK) {
        st = atree__expr_normalize_in(t->norm_arena, expr, t->max_depth, t->max_expr_nodes, &norm);
    }
    if (st != ATREE_OK) {
        if (t->norm_arena != NULL) {
            atree__arena_reset(t->norm_arena);
        }
        atree__wrunlock(t);
        set_err(err, st,
                st == ATREE_ERR_TOO_DEEP    ? "expression nested deeper than max_depth"
                    : st == ATREE_ERR_LIMIT ? "normalized expression exceeds max_expr_nodes"
                                            : atree_strerror(st));
        return st;
    }
    if (atree__u64map_get(&t->subs, id, NULL)) {
        st = ATREE_ERR_DUPLICATE_ID;
    } else if (norm->kind == ATREE_EXPR_TRUE || norm->kind == ATREE_EXPR_FALSE) {
        bool always = norm->kind == ATREE_EXPR_TRUE;
        if (always) {
            st = atree__u64vec_push(&t->mem, &t->always, id);
        }
        if (st == ATREE_OK) {
            st = atree__u64map_put(&t->mem, &t->subs, id,
                                   always ? ATREE_SUB_ALWAYS : ATREE_SUB_NEVER);
            if (st != ATREE_OK && always) {
                t->always.len--;
            }
        }
    } else {
        struct journalvec *journal = &t->journal;
        atree__nid root = ATREE_NID_NONE;
        uint64_t reorganized = t->reorganized;
        uint64_t skipped = t->adjust_candidates_skipped;
        journal->len = 0;
        /* Rollback must not fail: size the cascade worklist up front. */
        {
            uint64_t need = (uint64_t)t->nodes.len + expr_node_count(norm) + 1;
            st = need > UINT32_MAX ? ATREE_ERR_LIMIT
                                   : atree__u32vec_reserve(&t->mem, &t->worklist, (uint32_t)need);
        }
        if (st == ATREE_OK) {
            st = prehash(t, norm);
        }
        if (st == ATREE_OK) {
            st = build(t, norm, journal, &root);
        }
        if (st == ATREE_OK) {
            st = sub_attach(t, root, id);
            if (st == ATREE_OK) {
                st = atree__u64map_put(&t->mem, &t->subs, id, root);
                if (st != ATREE_OK) {
                    sub_detach(t, root, id);
                }
            }
        }
        if (st != ATREE_OK) {
            rollback(t, journal);
            t->reorganized = reorganized; /* rollback undoes self_adjusted itself */
            t->adjust_candidates_skipped = skipped;
        }
        journal_commit(t, journal);
    }
    if (st == ATREE_OK) {
        t->nsubs++;
    }
    atree__arena_reset(t->norm_arena); /* norm is dead from here on */
    atree__wrunlock(t);
    set_err(err, st,
            st == ATREE_ERR_DUPLICATE_ID ? "subscription id already present" : atree_strerror(st));
    return st;
}

atree_status_t atree_insert(atree_t *t, atree_id_t id, const char *expr, size_t expr_len,
                            atree_error_t *err)
{
    atree_expr_t *e = NULL;
    atree_status_t st;
    if (t == NULL) {
        reset_err(err);
        set_err(err, ATREE_ERR_INVALID_ARG, "tree is NULL");
        return ATREE_ERR_INVALID_ARG;
    }
    st = atree_expr_parse(t, expr, expr_len, &e, err);
    if (st != ATREE_OK) {
        return st;
    }
    st = atree_insert_expr(t, id, e, err);
    atree_expr_free(e);
    return st;
}

atree_status_t atree_delete(atree_t *t, atree_id_t id)
{
    uint32_t v;
    atree_status_t st = ATREE_OK;
    if (t == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    atree__wrlock(t);
    if (!atree__u64map_get(&t->subs, id, &v)) {
        st = ATREE_ERR_NOT_FOUND;
    } else if (v == ATREE_SUB_ALWAYS) {
        uint32_t pos = atree__u64vec_find(&t->always, id);
        if (pos != UINT32_MAX) {
            atree__u64vec_swap_remove(&t->always, pos);
        }
        atree__u64map_remove(&t->subs, id);
        t->nsubs--;
    } else if (v == ATREE_SUB_NEVER) {
        atree__u64map_remove(&t->subs, id);
        t->nsubs--;
    } else {
        /* Reserve first so the cascade cannot fail once we start mutating. */
        st = atree__u32vec_reserve(&t->mem, &t->worklist, t->nodes.len + 1);
        if (st == ATREE_OK) {
            atree__u64map_remove(&t->subs, id);
            if (sub_detach(t, v, id)) {
                cascade(t, v);
            }
            t->nsubs--;
        }
    }
    atree__wrunlock(t);
    return st;
}

bool atree_contains(const atree_t *t, atree_id_t id)
{
    bool found;
    if (t == NULL) {
        return false;
    }
    atree__rdlock(t);
    found = atree__u64map_get(&t->subs, id, NULL);
    atree__rdunlock(t);
    return found;
}

size_t atree_count(const atree_t *t)
{
    size_t n;
    if (t == NULL) {
        return 0;
    }
    atree__rdlock(t);
    n = (size_t)t->nsubs;
    atree__rdunlock(t);
    return n;
}

/* ---- statistics --------------------------------------------------------- */

void atree_stats(const atree_t *t, atree_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (t == NULL) {
        return;
    }
    atree__rdlock(t);
    out->subscriptions = t->nsubs;
    out->nodes = t->nodes.len - t->free_nodes.len;
    out->leaves = t->leaves.len;
    out->edges = t->edges;
    out->max_level = t->max_level;
    if (indexed(t)) {
        out->indexed_leaves = t->index.indexed;
        out->scanned_leaves = t->index.scanned;
    } else {
        out->indexed_leaves = 0;
        out->scanned_leaves = t->leaves.len;
    }
    out->reorganized = t->reorganized;
    out->self_adjusted = t->self_adjusted;
    out->adjust_candidates_skipped = t->adjust_candidates_skipped;
    out->strings = atree__strtab_count(&t->strings);
    out->bytes_allocated = t->mem.live;
    out->bytes_peak = t->mem.peak;
    atree__rdunlock(t);
}

/* ---- validation --------------------------------------------------------- */

static int msgf(char *msg, size_t cap, const char *what, unsigned long long a, unsigned long long b)
{
    if (msg != NULL && cap > 0) {
        (void)snprintf(msg, cap, "%s (node %llu, %llu)", what, a, b);
    }
    return 1;
}

#define FAIL(what, a, b)                                                                           \
    do {                                                                                           \
        (void)msgf(msg, cap, (what), (unsigned long long)(a), (unsigned long long)(b));            \
        st = ATREE_ERR_CORRUPT;                                                                    \
        goto done;                                                                                 \
    } while (0)

atree_status_t atree_validate(const atree_t *t, char *msg, size_t cap)
{
    atree_status_t st = ATREE_OK;
    uint32_t i;
    uint32_t j;
    uint64_t live = 0;
    uint64_t leaves = 0;
    uint64_t edges = 0;
    uint64_t parent_links = 0;
    uint32_t content_entries = 0;
    uint64_t subs_on_nodes = 0;
    uint32_t iter;
    uint64_t key;
    uint32_t val;
    struct atree__u32vec per_level; /* read-only tree: use a local scratch */
    struct atree__mem tmp;

    if (t == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (msg != NULL && cap > 0) {
        msg[0] = '\0';
    }
    atree__mem_init(&tmp, &t->mem.a);
    atree__u32vec_init(&per_level);
    atree__rdlock(t);
    if (atree__u32vec_reserve(&tmp, &per_level, t->level_counts.len) != ATREE_OK) {
        atree__rdunlock(t);
        return ATREE_ERR_NOMEM;
    }
    memset(per_level.data, 0, (size_t)t->level_counts.len * sizeof *per_level.data);
    per_level.len = t->level_counts.len;

    if (t->sub_slot.len != t->nodes.len || t->csum.len != t->nodes.len) {
        FAIL("per-node side arrays out of step with the node slab", t->sub_slot.len, t->nodes.len);
    }
    for (i = 0; i < t->nodes.len; i++) {
        const struct atree__node *n = atree__nodeslab_at(&t->nodes, i);
        struct atree__probe probe;
        const atree_id_t *subs;
        uint32_t nsubs_here;
        uint32_t level = 0;
        uint32_t anchors = 0;
        if (n->kind == ATREE_NODE_FREE) {
            if (*atree__u32slab_at(&t->sub_slot, i) != UINT32_MAX) {
                FAIL("free node still owns a subscription list", i,
                     *atree__u32slab_at(&t->sub_slot, i));
            }
            continue; /* a stranded slot (free-list push failed) is tolerated */
        }
        live++;
        if (n->level == 0 || n->level >= per_level.len) {
            FAIL("level out of range", i, n->level);
        }
        per_level.data[n->level]++;
        subs = atree__node_subs(t, i, &nsubs_here);
        if (((n->flags & ATREE_NODE_HAS_SUBS) != 0) != (nsubs_here > 0)) {
            FAIL("HAS_SUBS flag disagrees with sublist", i, nsubs_here);
        }
        for (j = 0; j < nsubs_here; j++) {
            if (!atree__u64map_get(&t->subs, subs[j], &val) || val != i) {
                FAIL("subscription not mapped back to node", i, (unsigned long long)subs[j]);
            }
        }
        subs_on_nodes += nsubs_here;
        if (!in_use(n)) {
            FAIL("orphan node", i, 0);
        }
        if (n->end_aw > n->end_a || n->end_a > n->end_w || n->end_w > n->parents.len) {
            FAIL("parent region boundaries out of order", i, n->parents.len);
        }
        for (j = 0; j < n->parents.len; j++) {
            const struct atree__node *p = atree__nodeslab_at(&t->nodes, n->parents.data[j]);
            uint32_t ci;
            if (p->kind == ATREE_NODE_FREE || p->kind == ATREE_NODE_LEAF) {
                FAIL("parent link to a non-inner node", i, n->parents.data[j]);
            }
            ci = child_index(p, i);
            if (ci == UINT32_MAX) {
                FAIL("parent link without child link", i, n->parents.data[j]);
            }
            if (edge_pos(child_pos_c(p)[ci]) != j) {
                FAIL("parent position out of date", i, n->parents.data[j]);
            }
            if (edge_anchored(child_pos_c(p)[ci]) != (j < n->end_a)) {
                FAIL("anchor flag disagrees with the anchored regions", i, j);
            }
            if (is_waker(p, i) != (j < n->end_aw || (j >= n->end_a && j < n->end_w))) {
                FAIL("waker status disagrees with the parent region", i, j);
            }
        }
        parent_links += n->parents.len;
        probe.kind = n->kind;
        probe.hash = n->hash;
        if (n->kind == ATREE_NODE_LEAF) {
            leaves++;
            if (n->level != 1 || n->children.len != 0) {
                FAIL("leaf with children or level != 1", i, n->level);
            }
            if (n->pred >= t->preds.len ||
                atree__predslab_at(&t->preds, n->pred)->kind >= ATREE_PRED_KIND_COUNT) {
                FAIL("leaf predicate slot invalid", i, n->pred);
            }
            if (*atree__u32slab_at(&t->leaf_pos, n->pred) >= t->leaves.len ||
                t->leaves.data[*atree__u32slab_at(&t->leaf_pos, n->pred)] != i) {
                FAIL("leaf not at its slot in the leaf list", i,
                     *atree__u32slab_at(&t->leaf_pos, n->pred));
            }
            if (atree__pred_content_hash(atree__predslab_at(&t->preds, n->pred), &t->strings) !=
                n->hash) {
                FAIL("leaf hash stale", i, 0);
            }
            probe.pred = atree__predslab_at(&t->preds, n->pred);
            probe.leaf = NULL;
            probe.children = NULL;
            probe.nchildren = 0;
        } else {
            if (n->children.len < 2) {
                FAIL("inner node with fewer than two children", i, n->children.len);
            }
            for (j = 0; j < n->children.len; j++) {
                const struct atree__node *c = atree__nodeslab_at(&t->nodes, n->children.data[j]);
                if (j > 0 && n->children.data[j] <= n->children.data[j - 1]) {
                    FAIL("children not sorted/unique", i, j);
                }
                if (c->kind == ATREE_NODE_FREE) {
                    FAIL("child is a free slot", i, n->children.data[j]);
                }
                if (edge_pos(child_pos_c(n)[j]) >= c->parents.len ||
                    c->parents.data[edge_pos(child_pos_c(n)[j])] != i) {
                    FAIL("child link without matching parent slot", i, n->children.data[j]);
                }
                if (edge_anchored(child_pos_c(n)[j])) {
                    anchors++;
                }
                if (c->level > level) {
                    level = c->level;
                }
                edges++;
            }
            if (n->level != level + 1) {
                FAIL("level != 1 + max child level", i, n->level);
            }
            if (anchors != 1) {
                FAIL("inner node without exactly one anchor edge", i, anchors);
            }
            if (*atree__u64slab_at(&t->csum, i) !=
                flat_sum(t, n->kind, n->children.data, n->children.len)) {
                FAIL("flat content sum stale", i, 0);
            }
            {
                uint32_t cursor = UINT32_MAX;
                uint64_t ckey = atree__content_key(t, i);
                atree__nid found;
                while ((found = atree__cset_next(t, ckey, &cursor)) != ATREE_NID_NONE &&
                       found != i) {
                }
                if (found != i) {
                    FAIL("inner node missing from the content table", i, 0);
                }
                content_entries++;
            }
            if (n->kind == ATREE_NODE_AND) {
                bool pod = (t->flags & ATREE_FLAG_NO_PROPAGATION_ON_DEMAND) == 0;
                if (pod != (n->access_child != ATREE_NID_NONE)) {
                    FAIL("access child presence disagrees with configuration", i, 0);
                }
                if (pod && child_index(n, n->access_child) == UINT32_MAX) {
                    FAIL("access child is not a child", i, n->access_child);
                }
            } else if (n->access_child != ATREE_NID_NONE) {
                FAIL("OR node with an access child", i, n->access_child);
            }
            if (inner_hash(n->kind, n->children.data, n->children.len) != n->hash) {
                FAIL("inner hash stale", i, 0);
            }
            probe.pred = NULL;
            probe.leaf = NULL;
            probe.children = n->children.data;
            probe.nchildren = n->children.len;
        }
        if (atree__idset_find(t, &probe) != i) {
            FAIL("identity table does not resolve to the node", i, 0);
        }
    }
    if (t->identity.reserved != 0) {
        FAIL("identity table reservation outlived its insert", t->identity.reserved, 0);
    }
    if (live != t->identity.count) {
        FAIL("identity table count mismatch", live, t->identity.count);
    }
    if (leaves != t->leaves.len) {
        FAIL("leaf list length mismatch", leaves, t->leaves.len);
    }
    if (indexed(t) && t->index.indexed + t->index.scanned != leaves) {
        FAIL("index counts do not add up to the leaves", t->index.indexed + t->index.scanned,
             leaves);
    }
    if (edges != t->edges) {
        FAIL("edge count mismatch", edges, t->edges);
    }
    if (parent_links != edges) {
        FAIL("parent links do not match child links", parent_links, edges);
    }
    if (content_entries != t->content.count) {
        FAIL("content table size disagrees with inner nodes", 0, t->content.count);
    }
    if (indexed(t)) {
        st = atree__index_check(t, &tmp, msg, cap);
        if (st != ATREE_OK) {
            goto done;
        }
    }
    for (i = 1; i < per_level.len; i++) {
        if (per_level.data[i] != t->level_counts.data[i]) {
            FAIL("level count mismatch at level", i, t->level_counts.data[i]);
        }
    }
    {
        uint32_t ml = 0;
        for (i = 1; i < per_level.len; i++) {
            if (per_level.data[i] != 0) {
                ml = i;
            }
        }
        if (ml != t->max_level) {
            FAIL("max_level mismatch", ml, t->max_level);
        }
    }
    /* every subscription maps to a live node or a constant marker */
    iter = 0;
    while (atree__u64map_next(&t->subs, &iter, &key, &val)) {
        if (val == ATREE_SUB_ALWAYS) {
            if (atree__u64vec_find(&t->always, key) == UINT32_MAX) {
                FAIL("constant-true subscription missing from always list", key, 0);
            }
        } else if (val != ATREE_SUB_NEVER) {
            const atree_id_t *subs;
            uint32_t nsubs_here;
            if (val >= t->nodes.len ||
                atree__nodeslab_at(&t->nodes, val)->kind == ATREE_NODE_FREE) {
                FAIL("subscription maps to a free node", key, val);
            }
            subs = atree__node_subs(t, val, &nsubs_here);
            for (j = 0; j < nsubs_here && subs[j] != key; j++) {
            }
            if (j == nsubs_here) {
                FAIL("subscription missing from its node's list", key, val);
            }
        }
    }
    if (t->subs.count != t->nsubs) {
        FAIL("subscription count mismatch", t->subs.count, t->nsubs);
    }
    if (subs_on_nodes + t->always.len > t->nsubs) {
        FAIL("more subscriptions on nodes than registered", subs_on_nodes, t->nsubs);
    }
done:
    atree__rdunlock(t);
    atree__u32vec_free(&tmp, &per_level);
    return st;
}
#undef FAIL

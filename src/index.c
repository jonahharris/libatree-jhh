/*
 * Per-attribute predicate indexes.
 *
 * SPDX-License-Identifier: MIT
 */
#include "index.h"

#include <stdio.h>
#include <string.h>

#include "atree_internal.h"
#include "event.h"

/* ---- helpers ------------------------------------------------------------ */

static struct atree__index_attr *ix_of(struct atree *t, atree_attr_id_t a)
{
    return &t->index.attrs[a];
}

static const struct atree__pred *pred_of(const struct atree *t, atree__nid id)
{
    return &t->preds.data[t->nodes.data[id].pred];
}

static atree_type_t type_of(const struct atree *t, atree_attr_id_t a)
{
    return atree__attrs_get(&t->attrs, a)->type;
}

/* Hash key of a scalar value for the eq/member maps. Types never mix within
 * one attribute, so ints, string ids and double bit patterns cannot collide
 * with each other. */
static uint64_t key_of_value(const struct atree__value *v)
{
    switch (v->kind) {
    case ATREE_V_INT:
        return (uint64_t)v->u.i;
    case ATREE_V_STRING:
        return v->u.s;
    case ATREE_V_FLOAT:
        return atree__double_bits(v->u.f);
    case ATREE_V_UNDEFINED:
    case ATREE_V_BOOL:
    case ATREE_V_INT_LIST:
    case ATREE_V_STRING_LIST:
    default:
        return 0;
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

atree_status_t atree__index_init(struct atree *t)
{
    struct atree__index *ix = &t->index;
    uint32_t n = atree__attrs_count(&t->attrs);
    uint32_t a;
    memset(ix, 0, sizeof *ix);
    atree__bucketvec_init(&ix->buckets);
    atree__u32vec_init(&ix->free_buckets);
    atree__u32vec_init(&ix->list_pos);
    if (n == 0) {
        return ATREE_OK;
    }
    ix->attrs = atree__zalloc_array(&t->mem, n, sizeof *ix->attrs);
    if (ix->attrs == NULL) {
        return ATREE_ERR_NOMEM;
    }
    ix->nattrs = n;
    for (a = 0; a < n; a++) {
        struct atree__index_attr *ia = &ix->attrs[a];
        atree__u32vec_init(&ia->bool_true);
        atree__u32vec_init(&ia->bool_false);
        atree__u64map_init(&ia->eq);
        atree__u64map_init(&ia->member);
        atree__rayvec_init(&ia->lower);
        atree__rayvec_init(&ia->upper);
        atree__u32vec_init(&ia->nulls);
        atree__u32vec_init(&ia->scan);
    }
    return ATREE_OK;
}

void atree__index_free(struct atree *t)
{
    struct atree__index *ix = &t->index;
    uint32_t a;
    for (a = 0; a < ix->nattrs; a++) {
        struct atree__index_attr *ia = &ix->attrs[a];
        atree__u32vec_free(&t->mem, &ia->bool_true);
        atree__u32vec_free(&t->mem, &ia->bool_false);
        atree__u64map_free(&t->mem, &ia->eq);
        atree__u64map_free(&t->mem, &ia->member);
        atree__rayvec_free(&t->mem, &ia->lower);
        atree__rayvec_free(&t->mem, &ia->upper);
        atree__u32vec_free(&t->mem, &ia->nulls);
        atree__u32vec_free(&t->mem, &ia->scan);
    }
    if (ix->attrs != NULL) {
        atree__free_array(&t->mem, ix->attrs, ix->nattrs, sizeof *ix->attrs);
    }
    for (a = 0; a < ix->buckets.len; a++) {
        atree__u32vec_free(&t->mem, &ix->buckets.data[a]);
    }
    atree__bucketvec_free(&t->mem, &ix->buckets);
    atree__u32vec_free(&t->mem, &ix->free_buckets);
    atree__u32vec_free(&t->mem, &ix->list_pos);
    memset(ix, 0, sizeof *ix);
}

/* ---- routing ------------------------------------------------------------ */

enum atree__route atree__index_route(const struct atree *t, const struct atree__pred *p)
{
    atree_type_t ty = type_of(t, p->attr);
    switch ((enum atree__pred_kind)p->kind) {
    case ATREE_PRED_VAR:
        return ATREE_ROUTE_BOOL_TRUE;
    case ATREE_PRED_NOT_VAR:
        return ATREE_ROUTE_BOOL_FALSE;
    case ATREE_PRED_CMP:
        switch ((atree_op_t)p->op) {
        case ATREE_OP_EQ:
            return ATREE_ROUTE_EQ;
        case ATREE_OP_NE:
            return ATREE_ROUTE_SCAN;
        case ATREE_OP_GT:
        case ATREE_OP_GE:
            return (ty == ATREE_TYPE_INT || ty == ATREE_TYPE_FLOAT) ? ATREE_ROUTE_LOWER
                                                                    : ATREE_ROUTE_SCAN;
        case ATREE_OP_LT:
        case ATREE_OP_LE:
            return (ty == ATREE_TYPE_INT || ty == ATREE_TYPE_FLOAT) ? ATREE_ROUTE_UPPER
                                                                    : ATREE_ROUTE_SCAN;
        default:
            return ATREE_ROUTE_SCAN;
        }
    case ATREE_PRED_IN:
    case ATREE_PRED_ONE_OF:
        return ATREE_ROUTE_MEMBER;
    case ATREE_PRED_IS_NULL:
        return ATREE_ROUTE_NULL;
    case ATREE_PRED_NOT_IN:
    case ATREE_PRED_NONE_OF:
    case ATREE_PRED_ALL_OF:
    case ATREE_PRED_NOT_ALL_OF:
    case ATREE_PRED_IS_NOT_NULL:
    case ATREE_PRED_IS_EMPTY:
    case ATREE_PRED_IS_NOT_EMPTY:
    case ATREE_PRED_KIND_COUNT:
    default:
        return ATREE_ROUTE_SCAN;
    }
}

/* ---- single lists (bool, null, scan) with O(1) removal ------------------ */

static atree_status_t list_pos_reserve(struct atree *t, atree__nid id)
{
    struct atree__u32vec *lp = &t->index.list_pos;
    while (lp->len <= id) {
        atree_status_t st = atree__u32vec_push(&t->mem, lp, UINT32_MAX);
        if (st != ATREE_OK) {
            return st;
        }
    }
    return ATREE_OK;
}

static atree_status_t list_add(struct atree *t, struct atree__u32vec *list, atree__nid id)
{
    atree_status_t st = list_pos_reserve(t, id);
    if (st != ATREE_OK) {
        return st;
    }
    st = atree__u32vec_push(&t->mem, list, id);
    if (st != ATREE_OK) {
        return st;
    }
    t->index.list_pos.data[id] = list->len - 1;
    return ATREE_OK;
}

static void list_remove(struct atree *t, struct atree__u32vec *list, atree__nid id)
{
    uint32_t pos = t->index.list_pos.data[id];
    uint32_t last = list->len - 1;
    if (pos != last) {
        atree__nid moved = list->data[last];
        list->data[pos] = moved;
        t->index.list_pos.data[moved] = pos;
    }
    list->len--;
    t->index.list_pos.data[id] = UINT32_MAX;
}

/* ---- buckets ------------------------------------------------------------ */

static atree_status_t bucket_add(struct atree *t, struct atree__u64map *map, uint64_t key,
                                 atree__nid id)
{
    struct atree__index *ix = &t->index;
    uint32_t slot;
    bool fresh = false;
    atree_status_t st;
    if (!atree__u64map_get(map, key, &slot)) {
        struct atree__u32vec empty;
        atree__u32vec_init(&empty);
        if (ix->free_buckets.len > 0) {
            slot = ix->free_buckets.data[--ix->free_buckets.len];
            ix->buckets.data[slot] = empty;
        } else {
            st = atree__bucketvec_push(&t->mem, &ix->buckets, empty);
            if (st != ATREE_OK) {
                return st;
            }
            slot = ix->buckets.len - 1;
        }
        st = atree__u64map_put(&t->mem, map, key, slot);
        if (st != ATREE_OK) {
            (void)atree__u32vec_push(&t->mem, &ix->free_buckets, slot);
            return st;
        }
        fresh = true;
    }
    st = atree__u32vec_push(&t->mem, &ix->buckets.data[slot], id);
    if (st != ATREE_OK && fresh) {
        atree__u64map_remove(map, key);
        (void)atree__u32vec_push(&t->mem, &ix->free_buckets, slot);
    }
    return st;
}

static void bucket_remove(struct atree *t, struct atree__u64map *map, uint64_t key, atree__nid id)
{
    struct atree__index *ix = &t->index;
    uint32_t slot;
    struct atree__u32vec *b;
    uint32_t pos;
    if (!atree__u64map_get(map, key, &slot)) {
        return;
    }
    b = &ix->buckets.data[slot];
    pos = atree__u32vec_find(b, id);
    if (pos != UINT32_MAX) {
        atree__u32vec_swap_remove(b, pos);
    }
    if (b->len == 0) {
        atree__u32vec_free(&t->mem, b);
        atree__u64map_remove(map, key);
        (void)atree__u32vec_push(&t->mem, &ix->free_buckets, slot);
    }
}

/* Every key a predicate's operand contributes to a map. */
static uint32_t operand_keys(const struct atree__pred *p, uint32_t i, uint64_t *key)
{
    const struct atree__value *v = &p->operand;
    switch (v->kind) {
    case ATREE_V_INT_LIST:
        if (i < v->u.il.len) {
            *key = (uint64_t)v->u.il.data[i];
        }
        return v->u.il.len;
    case ATREE_V_STRING_LIST:
        if (i < v->u.sl.len) {
            *key = v->u.sl.data[i];
        }
        return v->u.sl.len;
    case ATREE_V_INT:
    case ATREE_V_FLOAT:
    case ATREE_V_STRING:
        if (i == 0) {
            *key = key_of_value(v);
        }
        return 1;
    case ATREE_V_UNDEFINED:
    case ATREE_V_BOOL:
    default:
        return 0;
    }
}

/* ---- rays --------------------------------------------------------------- */

static int ray_cmp_value(atree_type_t ty, const struct atree__ray *a, const struct atree__ray *b)
{
    if (ty == ATREE_TYPE_INT) {
        return (a->i > b->i) - (a->i < b->i);
    }
    return atree__double_cmp(a->f, b->f);
}

static int ray_cmp(atree_type_t ty, const struct atree__ray *a, const struct atree__ray *b)
{
    int c = ray_cmp_value(ty, a, b);
    if (c != 0) {
        return c;
    }
    return (a->id > b->id) - (a->id < b->id);
}

static struct atree__ray ray_of(const struct atree__pred *p, atree__nid id)
{
    struct atree__ray r;
    r.i = p->operand.kind == ATREE_V_INT ? p->operand.u.i : 0;
    r.f = p->operand.kind == ATREE_V_FLOAT ? p->operand.u.f : 0.0;
    r.id = id;
    r.op = p->op;
    return r;
}

/* First position whose ray is >= r (lower_bound in the (value, id) order). */
static uint32_t ray_lower_bound(const struct atree__rayvec *v, atree_type_t ty,
                                const struct atree__ray *r)
{
    uint32_t lo = 0;
    uint32_t hi = v->len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ray_cmp(ty, &v->data[mid], r) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static atree_status_t ray_add(struct atree *t, struct atree__rayvec *v, atree_type_t ty,
                              const struct atree__ray *r)
{
    uint32_t pos = ray_lower_bound(v, ty, r);
    return atree__rayvec_insert_at(&t->mem, v, pos, *r);
}

static void ray_remove(struct atree *t, struct atree__rayvec *v, atree_type_t ty,
                       const struct atree__ray *r)
{
    uint32_t pos = ray_lower_bound(v, ty, r);
    (void)t;
    if (pos < v->len && v->data[pos].id == r->id) {
        atree__rayvec_remove_at(v, pos);
    }
}

/* Positions delimiting thresholds equal to the event value: [lb, ub). */
static void ray_equal_range(const struct atree__rayvec *v, atree_type_t ty,
                            const struct atree__value *ev, uint32_t *lb, uint32_t *ub)
{
    struct atree__ray probe;
    uint32_t lo;
    uint32_t hi;
    probe.i = ev->kind == ATREE_V_INT ? ev->u.i : 0;
    probe.f = ev->kind == ATREE_V_FLOAT ? ev->u.f : 0.0;
    probe.id = 0;
    probe.op = 0;
    lo = 0;
    hi = v->len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ray_cmp_value(ty, &v->data[mid], &probe) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *lb = lo;
    hi = v->len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ray_cmp_value(ty, &v->data[mid], &probe) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *ub = lo;
}

/* ---- add / remove ------------------------------------------------------- */

atree_status_t atree__index_add(struct atree *t, atree__nid id)
{
    const struct atree__pred *p = pred_of(t, id);
    struct atree__index_attr *ia = ix_of(t, p->attr);
    atree_type_t ty = type_of(t, p->attr);
    enum atree__route route = atree__index_route(t, p);
    atree_status_t st = ATREE_OK;
    uint32_t i;
    uint32_t n;
    uint64_t key = 0;
    struct atree__ray r;

    switch (route) {
    case ATREE_ROUTE_BOOL_TRUE:
        st = list_add(t, &ia->bool_true, id);
        break;
    case ATREE_ROUTE_BOOL_FALSE:
        st = list_add(t, &ia->bool_false, id);
        break;
    case ATREE_ROUTE_NULL:
        st = list_add(t, &ia->nulls, id);
        break;
    case ATREE_ROUTE_SCAN:
        st = list_add(t, &ia->scan, id);
        break;
    case ATREE_ROUTE_EQ:
        (void)operand_keys(p, 0, &key);
        st = bucket_add(t, &ia->eq, key, id);
        break;
    case ATREE_ROUTE_MEMBER:
        n = operand_keys(p, 0, &key);
        for (i = 0; st == ATREE_OK && i < n; i++) {
            (void)operand_keys(p, i, &key);
            st = bucket_add(t, &ia->member, key, id);
            if (st != ATREE_OK) {
                uint32_t k;
                for (k = 0; k < i; k++) {
                    (void)operand_keys(p, k, &key);
                    bucket_remove(t, &ia->member, key, id);
                }
            }
        }
        break;
    case ATREE_ROUTE_LOWER:
        r = ray_of(p, id);
        st = ray_add(t, &ia->lower, ty, &r);
        break;
    case ATREE_ROUTE_UPPER:
        r = ray_of(p, id);
        st = ray_add(t, &ia->upper, ty, &r);
        break;
    case ATREE_ROUTE_NONE:
    default:
        st = ATREE_ERR_INVALID_ARG;
        break;
    }
    if (st == ATREE_OK) {
        if (route == ATREE_ROUTE_SCAN) {
            t->index.scanned++;
        } else {
            t->index.indexed++;
        }
    }
    return st;
}

void atree__index_remove(struct atree *t, atree__nid id)
{
    const struct atree__pred *p = pred_of(t, id);
    struct atree__index_attr *ia = ix_of(t, p->attr);
    atree_type_t ty = type_of(t, p->attr);
    enum atree__route route = atree__index_route(t, p);
    uint32_t i;
    uint32_t n;
    uint64_t key = 0;
    struct atree__ray r;

    switch (route) {
    case ATREE_ROUTE_BOOL_TRUE:
        list_remove(t, &ia->bool_true, id);
        break;
    case ATREE_ROUTE_BOOL_FALSE:
        list_remove(t, &ia->bool_false, id);
        break;
    case ATREE_ROUTE_NULL:
        list_remove(t, &ia->nulls, id);
        break;
    case ATREE_ROUTE_SCAN:
        list_remove(t, &ia->scan, id);
        break;
    case ATREE_ROUTE_EQ:
        (void)operand_keys(p, 0, &key);
        bucket_remove(t, &ia->eq, key, id);
        break;
    case ATREE_ROUTE_MEMBER:
        n = operand_keys(p, 0, &key);
        for (i = 0; i < n; i++) {
            (void)operand_keys(p, i, &key);
            bucket_remove(t, &ia->member, key, id);
        }
        break;
    case ATREE_ROUTE_LOWER:
        r = ray_of(p, id);
        ray_remove(t, &ia->lower, ty, &r);
        break;
    case ATREE_ROUTE_UPPER:
        r = ray_of(p, id);
        ray_remove(t, &ia->upper, ty, &r);
        break;
    case ATREE_ROUTE_NONE:
    default:
        return;
    }
    if (route == ATREE_ROUTE_SCAN) {
        t->index.scanned--;
    } else {
        t->index.indexed--;
    }
}

/* ---- validation --------------------------------------------------------- */

struct check {
    const struct atree *t;
    uint32_t *count; /* occurrences per node id */
    char *msg;
    size_t cap;
    int bad;
};

static void check_fail(struct check *c, const char *what, unsigned long long a,
                       unsigned long long b)
{
    if (!c->bad && c->msg != NULL && c->cap > 0) {
        (void)snprintf(c->msg, c->cap, "index: %s (%llu, %llu)", what, a, b);
    }
    c->bad = 1;
}

/* Every id in a single-route list must be a leaf of that route, at list_pos. */
static void check_list(struct check *c, const struct atree__u32vec *list, enum atree__route want,
                       atree_attr_id_t attr)
{
    uint32_t i;
    for (i = 0; i < list->len && !c->bad; i++) {
        atree__nid id = list->data[i];
        if (id >= c->t->nodes.len || c->t->nodes.data[id].kind != ATREE_NODE_LEAF) {
            check_fail(c, "non-leaf in index list", id, want);
            return;
        }
        if (atree__index_route(c->t, pred_of(c->t, id)) != want ||
            pred_of(c->t, id)->attr != attr) {
            check_fail(c, "leaf in the wrong list", id, want);
            return;
        }
        if (id >= c->t->index.list_pos.len || c->t->index.list_pos.data[id] != i) {
            check_fail(c, "list position out of date", id, i);
            return;
        }
        c->count[id]++;
    }
}

static void check_buckets(struct check *c, const struct atree__u64map *map, enum atree__route want,
                          atree_attr_id_t attr)
{
    uint32_t iter = 0;
    uint64_t key;
    uint32_t slot;
    while (!c->bad && atree__u64map_next(map, &iter, &key, &slot)) {
        const struct atree__u32vec *b;
        uint32_t i;
        if (slot >= c->t->index.buckets.len) {
            check_fail(c, "bucket slot out of range", slot, 0);
            return;
        }
        b = &c->t->index.buckets.data[slot];
        if (b->len == 0) {
            check_fail(c, "empty bucket left in map", slot, 0);
            return;
        }
        for (i = 0; i < b->len; i++) {
            atree__nid id = b->data[i];
            const struct atree__pred *p;
            uint32_t n;
            uint32_t k;
            bool has = false;
            uint64_t kk = 0;
            if (id >= c->t->nodes.len || c->t->nodes.data[id].kind != ATREE_NODE_LEAF) {
                check_fail(c, "non-leaf in bucket", id, want);
                return;
            }
            p = pred_of(c->t, id);
            if (atree__index_route(c->t, p) != want || p->attr != attr) {
                check_fail(c, "leaf in the wrong bucket map", id, want);
                return;
            }
            n = operand_keys(p, 0, &kk);
            for (k = 0; k < n && !has; k++) {
                (void)operand_keys(p, k, &kk);
                has = kk == key;
            }
            if (!has) {
                check_fail(c, "leaf under a key its operand lacks", id, (unsigned long long)key);
                return;
            }
            c->count[id]++;
        }
    }
}

static void check_rays(struct check *c, const struct atree__rayvec *v, enum atree__route want,
                       atree_attr_id_t attr, atree_type_t ty)
{
    uint32_t i;
    for (i = 0; i < v->len && !c->bad; i++) {
        atree__nid id = v->data[i].id;
        const struct atree__pred *p;
        struct atree__ray r;
        if (id >= c->t->nodes.len || c->t->nodes.data[id].kind != ATREE_NODE_LEAF) {
            check_fail(c, "non-leaf in ray array", id, want);
            return;
        }
        p = pred_of(c->t, id);
        if (atree__index_route(c->t, p) != want || p->attr != attr) {
            check_fail(c, "leaf in the wrong ray array", id, want);
            return;
        }
        r = ray_of(p, id);
        if (ray_cmp(ty, &v->data[i], &r) != 0) {
            check_fail(c, "ray does not match its predicate", id, 0);
            return;
        }
        if (i > 0 && ray_cmp(ty, &v->data[i - 1], &v->data[i]) >= 0) {
            check_fail(c, "ray array out of order", attr, i);
            return;
        }
        c->count[id]++;
    }
}

atree_status_t atree__index_check(const struct atree *t, struct atree__mem *scratch, char *msg,
                                  size_t cap)
{
    struct check c;
    uint32_t a;
    uint32_t i;
    uint64_t indexed = 0;
    uint64_t scanned = 0;

    c.t = t;
    c.msg = msg;
    c.cap = cap;
    c.bad = 0;
    c.count = NULL;
    if (t->nodes.len > 0) {
        c.count = atree__zalloc_array(scratch, t->nodes.len, sizeof *c.count);
        if (c.count == NULL) {
            return ATREE_ERR_NOMEM;
        }
    }
    for (a = 0; a < t->index.nattrs && !c.bad; a++) {
        const struct atree__index_attr *ia = &t->index.attrs[a];
        atree_type_t ty = type_of(t, a);
        check_list(&c, &ia->bool_true, ATREE_ROUTE_BOOL_TRUE, a);
        check_list(&c, &ia->bool_false, ATREE_ROUTE_BOOL_FALSE, a);
        check_list(&c, &ia->nulls, ATREE_ROUTE_NULL, a);
        check_list(&c, &ia->scan, ATREE_ROUTE_SCAN, a);
        check_buckets(&c, &ia->eq, ATREE_ROUTE_EQ, a);
        check_buckets(&c, &ia->member, ATREE_ROUTE_MEMBER, a);
        check_rays(&c, &ia->lower, ATREE_ROUTE_LOWER, a, ty);
        check_rays(&c, &ia->upper, ATREE_ROUTE_UPPER, a, ty);
    }
    for (i = 0; i < t->nodes.len && !c.bad; i++) {
        const struct atree__node *n = &t->nodes.data[i];
        uint32_t want;
        if (n->kind != ATREE_NODE_LEAF) {
            if (c.count[i] != 0) {
                check_fail(&c, "inner or free node present in an index", i, c.count[i]);
            }
            continue;
        }
        {
            const struct atree__pred *p = pred_of(t, i);
            enum atree__route route = atree__index_route(t, p);
            uint64_t k = 0;
            want = route == ATREE_ROUTE_MEMBER ? operand_keys(p, 0, &k) : 1;
            if (route == ATREE_ROUTE_SCAN) {
                scanned++;
            } else {
                indexed++;
            }
        }
        if (c.count[i] != want) {
            check_fail(&c, "leaf occurrence count differs from its route", i, c.count[i]);
        }
    }
    if (!c.bad && (indexed != t->index.indexed || scanned != t->index.scanned)) {
        check_fail(&c, "index counters out of date", (unsigned long long)indexed,
                   (unsigned long long)scanned);
    }
    if (c.count != NULL) {
        atree__free_array(scratch, c.count, t->nodes.len, sizeof *c.count);
    }
    return c.bad ? ATREE_ERR_CORRUPT : ATREE_OK;
}

/* ---- probing ------------------------------------------------------------ */

static atree_status_t seed_list(const struct atree__u32vec *list, atree__seed_fn seed, void *ctx,
                                uint64_t *evaluated)
{
    uint32_t i;
    for (i = 0; i < list->len; i++) {
        atree_status_t st = seed(ctx, list->data[i]);
        if (st != ATREE_OK) {
            return st;
        }
    }
    *evaluated += list->len;
    return ATREE_OK;
}

static atree_status_t seed_bucket(const struct atree *t, const struct atree__u64map *map,
                                  uint64_t key, atree__seed_fn seed, void *ctx, uint64_t *evaluated)
{
    uint32_t slot;
    if (!atree__u64map_get(map, key, &slot)) {
        return ATREE_OK;
    }
    return seed_list(&t->index.buckets.data[slot], seed, ctx, evaluated);
}

static atree_status_t seed_rays(const struct atree__index_attr *ia, atree_type_t ty,
                                const struct atree__value *v, atree__seed_fn seed, void *ctx,
                                uint64_t *evaluated)
{
    uint32_t lb;
    uint32_t ub;
    uint32_t i;
    atree_status_t st;
    /* lower rays: x > c for c < v (prefix), x >= c for c == v */
    if (ia->lower.len > 0) {
        ray_equal_range(&ia->lower, ty, v, &lb, &ub);
        for (i = 0; i < lb; i++) {
            st = seed(ctx, ia->lower.data[i].id);
            if (st != ATREE_OK) {
                return st;
            }
        }
        *evaluated += lb;
        for (i = lb; i < ub; i++) {
            (*evaluated)++;
            if (ia->lower.data[i].op == ATREE_OP_GE) {
                st = seed(ctx, ia->lower.data[i].id);
                if (st != ATREE_OK) {
                    return st;
                }
            }
        }
    }
    /* upper rays: x < c for c > v (suffix), x <= c for c == v */
    if (ia->upper.len > 0) {
        ray_equal_range(&ia->upper, ty, v, &lb, &ub);
        for (i = ub; i < ia->upper.len; i++) {
            st = seed(ctx, ia->upper.data[i].id);
            if (st != ATREE_OK) {
                return st;
            }
        }
        *evaluated += ia->upper.len - ub;
        for (i = lb; i < ub; i++) {
            (*evaluated)++;
            if (ia->upper.data[i].op == ATREE_OP_LE) {
                st = seed(ctx, ia->upper.data[i].id);
                if (st != ATREE_OK) {
                    return st;
                }
            }
        }
    }
    return ATREE_OK;
}

static atree_status_t seed_scan(const struct atree *t, const struct atree__u32vec *list,
                                const struct atree__value *v, atree__seed_fn seed, void *ctx,
                                uint64_t *evaluated)
{
    uint32_t i;
    for (i = 0; i < list->len; i++) {
        atree__nid id = list->data[i];
        (*evaluated)++;
        if (atree__pred_eval(pred_of(t, id), v) == ATREE_TRUE) {
            atree_status_t st = seed(ctx, id);
            if (st != ATREE_OK) {
                return st;
            }
        }
    }
    return ATREE_OK;
}

atree_status_t atree__index_probe(const struct atree *t, const struct atree_event *ev,
                                  atree__seed_fn seed, void *ctx, uint64_t *evaluated)
{
    uint32_t a;
    atree_status_t st = ATREE_OK;
    for (a = 0; st == ATREE_OK && a < t->index.nattrs; a++) {
        const struct atree__index_attr *ia = &t->index.attrs[a];
        const struct atree__value *v = atree__event_value(ev, a);
        atree_type_t ty = type_of(t, a);
        if (v->kind == ATREE_V_UNDEFINED) {
            /* Only `is null` is true on an undefined attribute (paper §3.2). */
            st = seed_list(&ia->nulls, seed, ctx, evaluated);
            continue;
        }
        switch (ty) {
        case ATREE_TYPE_BOOL:
            st = seed_list(v->u.b ? &ia->bool_true : &ia->bool_false, seed, ctx, evaluated);
            break;
        case ATREE_TYPE_INT:
        case ATREE_TYPE_FLOAT:
        case ATREE_TYPE_STRING:
            st = seed_bucket(t, &ia->eq, key_of_value(v), seed, ctx, evaluated);
            if (st == ATREE_OK) {
                st = seed_bucket(t, &ia->member, key_of_value(v), seed, ctx, evaluated);
            }
            if (st == ATREE_OK && ty != ATREE_TYPE_STRING) {
                st = seed_rays(ia, ty, v, seed, ctx, evaluated);
            }
            break;
        case ATREE_TYPE_INT_LIST: {
            uint32_t i;
            for (i = 0; st == ATREE_OK && i < v->u.il.len; i++) {
                st = seed_bucket(t, &ia->member, (uint64_t)v->u.il.data[i], seed, ctx, evaluated);
            }
            break;
        }
        case ATREE_TYPE_STRING_LIST: {
            uint32_t i;
            for (i = 0; st == ATREE_OK && i < v->u.sl.len; i++) {
                st = seed_bucket(t, &ia->member, v->u.sl.data[i], seed, ctx, evaluated);
            }
            break;
        }
        default:
            break;
        }
        if (st == ATREE_OK) {
            st = seed_scan(t, &ia->scan, v, seed, ctx, evaluated);
        }
    }
    return st;
}

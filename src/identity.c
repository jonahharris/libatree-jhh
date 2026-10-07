/*
 * Identity table.
 *
 * SPDX-License-Identifier: MIT
 */
#include "identity.h"

#include <string.h>

#include "atree_internal.h"
#include "expr.h"
#include "hash.h"

#define IDSET_MIN_CAP 64u
#define IDSET_MAX_CAP (UINT32_C(1) << 31)

void atree__idset_init(struct atree__idset *s)
{
    s->slots = NULL;
    s->cap = 0;
    s->count = 0;
    s->used = 0;
    s->reserved = 0;
}

static void set_free(struct atree *t, struct atree__idset *s)
{
    if (s->cap != 0) {
        atree__free_array(&t->mem, s->slots, s->cap, sizeof *s->slots);
    }
    atree__idset_init(s);
}

void atree__idset_free(struct atree *t)
{
    set_free(t, &t->identity);
}

void atree__cset_free(struct atree *t)
{
    set_free(t, &t->content);
}

/* The key a set files a node under. */
typedef uint64_t (*key_fn)(const struct atree *t, atree__nid id);

static uint64_t identity_key(const struct atree *t, atree__nid id)
{
    return t->nodes.data[id].hash;
}

uint64_t atree__content_key(const struct atree *t, atree__nid id)
{
    const struct atree__node *n = &t->nodes.data[id];
    uint64_t tag = n->kind == ATREE_NODE_AND ? UINT64_C(0xa11d) : UINT64_C(0x0e0e);
    return atree__hash_combine(atree__hash_u64(tag), t->csum.data[id]);
}

static uint32_t cap_for(uint32_t need)
{
    uint64_t target = ((uint64_t)need * 10) / 7 + 1;
    uint32_t cap = IDSET_MIN_CAP;
    while ((uint64_t)cap < target) {
        if (cap >= IDSET_MAX_CAP) {
            return 0;
        }
        cap <<= 1;
    }
    return cap;
}

static atree_status_t rehash(struct atree *t, struct atree__idset *s, key_fn key, uint32_t new_cap)
{
    uint32_t *slots = atree__alloc_array(&t->mem, new_cap, sizeof *slots);
    uint32_t mask = new_cap - 1;
    uint32_t i;
    if (slots == NULL) {
        return ATREE_ERR_NOMEM;
    }
    memset(slots, 0xff, (size_t)new_cap * sizeof *slots); /* all ATREE_NID_NONE */
    for (i = 0; i < s->cap; i++) {
        uint32_t id = s->slots[i];
        if (id != ATREE_NID_NONE && id != ATREE_IDSET_TOMB) {
            uint32_t j = (uint32_t)key(t, id) & mask;
            while (slots[j] != ATREE_NID_NONE) {
                j = (j + 1) & mask;
            }
            slots[j] = id;
        }
    }
    if (s->cap != 0) {
        atree__free_array(&t->mem, s->slots, s->cap, sizeof *s->slots);
    }
    s->slots = slots;
    s->cap = new_cap;
    s->used = s->count;
    return ATREE_OK;
}

static bool matches(const struct atree *t, atree__nid id, const struct atree__probe *p)
{
    const struct atree__node *n = &t->nodes.data[id];
    if (n->hash != p->hash || n->kind != p->kind) {
        return false;
    }
    if (p->kind == ATREE_NODE_LEAF) {
        if (p->leaf != NULL) {
            return atree__expr_leaf_equals(p->leaf, &t->preds.data[n->pred], &t->strings);
        }
        return atree__pred_equal(&t->preds.data[n->pred], p->pred);
    }
    return n->children.len == p->nchildren &&
        (p->nchildren == 0 ||
         memcmp(n->children.data, p->children, (size_t)p->nchildren * sizeof(uint32_t)) == 0);
}

atree__nid atree__idset_find(const struct atree *t, const struct atree__probe *probe)
{
    const struct atree__idset *s = &t->identity;
    uint32_t mask;
    uint32_t j;
    if (s->cap == 0) {
        return ATREE_NID_NONE;
    }
    mask = s->cap - 1;
    j = (uint32_t)probe->hash & mask;
    for (;;) {
        uint32_t id = s->slots[j];
        if (id == ATREE_NID_NONE) {
            return ATREE_NID_NONE;
        }
        if (id != ATREE_IDSET_TOMB && matches(t, id, probe)) {
            return id;
        }
        j = (j + 1) & mask;
    }
}

static atree_status_t set_insert(struct atree *t, struct atree__idset *s, key_fn key, uint64_t hash,
                                 atree__nid id)
{
    uint32_t mask;
    uint32_t j;

    if (s->cap != 0) {
        /* Probe first: landing on a tombstone needs no growth. This is what
         * lets rollback re-insert a node under its old hash without
         * allocating: the removal left a tombstone on that probe path. */
        mask = s->cap - 1;
        j = (uint32_t)hash & mask;
        while (s->slots[j] != ATREE_NID_NONE && s->slots[j] != ATREE_IDSET_TOMB) {
            j = (j + 1) & mask;
        }
        if (s->slots[j] == ATREE_IDSET_TOMB ||
            (uint64_t)(s->used + 1 + s->reserved) * 10 < (uint64_t)s->cap * 7) {
            if (s->slots[j] == ATREE_NID_NONE) {
                s->used++;
            }
            s->slots[j] = id;
            s->count++;
            return ATREE_OK;
        }
    }
    {
        /* Over the load factor: rehash, in place when the live entries fit
         * (that drops the tombstones; under insert/delete churn at a steady
         * size the table then never grows), larger when they do not. */
        uint32_t cap = cap_for(s->count + 1 + s->reserved);
        atree_status_t st;
        if (cap == 0) {
            return ATREE_ERR_LIMIT;
        }
        if (cap < s->cap) {
            cap = s->cap;
        }
        st = rehash(t, s, key, cap);
        if (st != ATREE_OK) {
            return st;
        }
    }
    mask = s->cap - 1;
    j = (uint32_t)hash & mask;
    while (s->slots[j] != ATREE_NID_NONE) {
        j = (j + 1) & mask;
    }
    s->used++;
    s->slots[j] = id;
    s->count++;
    return ATREE_OK;
}

atree_status_t atree__idset_insert(struct atree *t, uint64_t hash, atree__nid id)
{
    return set_insert(t, &t->identity, identity_key, hash, id);
}

void atree__idset_reinsert(struct atree *t, uint64_t hash, atree__nid id)
{
    struct atree__idset *s = &t->identity;
    uint32_t mask = s->cap - 1;
    uint32_t j = (uint32_t)hash & mask;
    /* Every forward insert kept `reserved` slots free under the load
     * factor, so an empty or tombstone slot is on this probe path. */
    while (s->slots[j] != ATREE_NID_NONE && s->slots[j] != ATREE_IDSET_TOMB) {
        j = (j + 1) & mask;
    }
    if (s->slots[j] == ATREE_NID_NONE) {
        s->used++;
    }
    s->slots[j] = id;
    s->count++;
    if (s->reserved > 0) {
        s->reserved--;
    }
}

atree_status_t atree__cset_insert(struct atree *t, uint64_t hash, atree__nid id)
{
    return set_insert(t, &t->content, atree__content_key, hash, id);
}

static bool set_remove(struct atree__idset *s, uint64_t hash, atree__nid id)
{
    uint32_t mask;
    uint32_t j;
    if (s->cap == 0) {
        return false;
    }
    mask = s->cap - 1;
    j = (uint32_t)hash & mask;
    for (;;) {
        uint32_t cur = s->slots[j];
        if (cur == ATREE_NID_NONE) {
            return false;
        }
        if (cur == id) {
            s->slots[j] = ATREE_IDSET_TOMB;
            s->count--;
            return true;
        }
        j = (j + 1) & mask;
    }
}

bool atree__idset_remove(struct atree *t, uint64_t hash, atree__nid id)
{
    return set_remove(&t->identity, hash, id);
}

bool atree__cset_remove(struct atree *t, uint64_t hash, atree__nid id)
{
    return set_remove(&t->content, hash, id);
}

atree__nid atree__cset_next(const struct atree *t, uint64_t hash, uint32_t *cursor)
{
    const struct atree__idset *s = &t->content;
    uint32_t mask;
    uint32_t j;
    if (s->cap == 0) {
        return ATREE_NID_NONE;
    }
    mask = s->cap - 1;
    j = *cursor == UINT32_MAX ? ((uint32_t)hash & mask) : ((*cursor + 1) & mask);
    for (;;) {
        uint32_t id = s->slots[j];
        if (id == ATREE_NID_NONE) {
            return ATREE_NID_NONE;
        }
        if (id != ATREE_IDSET_TOMB && atree__content_key(t, id) == hash) {
            *cursor = j;
            return id;
        }
        j = (j + 1) & mask;
    }
}

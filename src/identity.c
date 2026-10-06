/*
 * Identity table.
 *
 * SPDX-License-Identifier: MIT
 */
#include "identity.h"

#include <string.h>

#include "atree_internal.h"
#include "hash.h"

#define IDSET_MIN_CAP 64u
#define IDSET_MAX_CAP (UINT32_C(1) << 31)

void atree__idset_init(struct atree__idset *s)
{
    s->slots = NULL;
    s->cap = 0;
    s->count = 0;
    s->used = 0;
}

void atree__idset_free(struct atree *t)
{
    struct atree__idset *s = &t->identity;
    if (s->cap != 0) {
        atree__free_array(&t->mem, s->slots, s->cap, sizeof *s->slots);
    }
    atree__idset_init(s);
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

static atree_status_t rehash(struct atree *t, uint32_t new_cap)
{
    struct atree__idset *s = &t->identity;
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
            uint32_t j = (uint32_t)t->nodes.data[id].hash & mask;
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

atree_status_t atree__idset_reserve(struct atree *t, uint32_t n)
{
    uint32_t cap = cap_for(n);
    if (cap == 0) {
        return ATREE_ERR_LIMIT;
    }
    if (cap <= t->identity.cap) {
        return ATREE_OK;
    }
    return rehash(t, cap);
}

static bool matches(const struct atree *t, atree__nid id, const struct atree__probe *p)
{
    const struct atree__node *n = &t->nodes.data[id];
    if (n->hash != p->hash || n->kind != p->kind) {
        return false;
    }
    if (p->kind == ATREE_NODE_LEAF) {
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

atree_status_t atree__idset_insert(struct atree *t, uint64_t hash, atree__nid id)
{
    struct atree__idset *s = &t->identity;
    uint32_t mask;
    uint32_t j;
    if (s->cap == 0 || (uint64_t)(s->used + 1) * 10 >= (uint64_t)s->cap * 7) {
        uint32_t cap = cap_for(s->count + 1);
        atree_status_t st;
        if (cap == 0) {
            return ATREE_ERR_LIMIT;
        }
        if (cap < s->cap) {
            cap = s->cap;
        }
        st = rehash(t, cap);
        if (st != ATREE_OK) {
            return st;
        }
    }
    mask = s->cap - 1;
    j = (uint32_t)hash & mask;
    while (s->slots[j] != ATREE_NID_NONE && s->slots[j] != ATREE_IDSET_TOMB) {
        j = (j + 1) & mask;
    }
    if (s->slots[j] == ATREE_NID_NONE) {
        s->used++;
    }
    s->slots[j] = id;
    s->count++;
    return ATREE_OK;
}

bool atree__idset_remove(struct atree *t, uint64_t hash, atree__nid id)
{
    struct atree__idset *s = &t->identity;
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

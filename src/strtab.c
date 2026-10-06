/*
 * String table.
 *
 * SPDX-License-Identifier: MIT
 */
#include "strtab.h"

#include <string.h>

atree_status_t atree__strtab_init(struct atree__mem *m, struct atree__strtab *t)
{
    struct atree__strtab_entry sentinel;
    atree_status_t st;

    atree__strmap_init(&t->map);
    atree__chunkvec_init(&t->chunks);
    atree__strentvec_init(&t->entries);

    sentinel.ptr = NULL;
    sentinel.len = 0;
    st = atree__strentvec_push(m, &t->entries, sentinel);
    if (st != ATREE_OK) {
        atree__strtab_free(m, t);
    }
    return st;
}

void atree__strtab_free(struct atree__mem *m, struct atree__strtab *t)
{
    uint32_t i;
    for (i = 0; i < t->chunks.len; i++) {
        atree__free(m, t->chunks.data[i].base, t->chunks.data[i].cap);
    }
    atree__chunkvec_free(m, &t->chunks);
    atree__strentvec_free(m, &t->entries);
    atree__strmap_free(m, &t->map);
}

uint32_t atree__strtab_lookup(const struct atree__strtab *t, const char *s, size_t len)
{
    uint32_t id;
    if (atree__strmap_get(&t->map, s, len, &id)) {
        return id;
    }
    return ATREE_STR_UNKNOWN;
}

/* Returns a stable pointer to a copy of s (NUL-terminated), or NULL. */
static const char *arena_copy(struct atree__mem *m, struct atree__strtab *t, const char *s,
                              size_t len)
{
    struct atree__strtab_chunk *c = NULL;
    size_t need;
    char *dst;

    if (atree__add_overflows(len, 1, &need)) {
        return NULL;
    }
    if (t->chunks.len > 0) {
        c = &t->chunks.data[t->chunks.len - 1];
        if (c->cap - c->used < need) {
            c = NULL;
        }
    }
    if (c == NULL) {
        struct atree__strtab_chunk fresh;
        fresh.cap = need > ATREE_STRTAB_CHUNK_SIZE ? need : ATREE_STRTAB_CHUNK_SIZE;
        fresh.used = 0;
        fresh.base = atree__alloc(m, fresh.cap);
        if (fresh.base == NULL) {
            return NULL;
        }
        if (atree__chunkvec_push(m, &t->chunks, fresh) != ATREE_OK) {
            atree__free(m, fresh.base, fresh.cap);
            return NULL;
        }
        c = &t->chunks.data[t->chunks.len - 1];
    }
    dst = c->base + c->used;
    if (len > 0) {
        memcpy(dst, s, len);
    }
    dst[len] = '\0';
    c->used += need;
    return dst;
}

atree_status_t atree__strtab_intern(struct atree__mem *m, struct atree__strtab *t, const char *s,
                                    size_t len, uint32_t *id)
{
    uint32_t existing;
    struct atree__strtab_entry e;
    atree_status_t st;

    if (len >= UINT32_MAX) {
        return ATREE_ERR_LIMIT;
    }
    if (atree__strmap_get(&t->map, s, len, &existing)) {
        *id = existing;
        return ATREE_OK;
    }
    if (t->entries.len == UINT32_MAX) {
        return ATREE_ERR_LIMIT;
    }
    e.ptr = arena_copy(m, t, s, len);
    if (e.ptr == NULL) {
        return ATREE_ERR_NOMEM;
    }
    e.len = (uint32_t)len;
    /* The arena copy is not reclaimed on the failure paths below: it is a
     * few bytes inside a chunk that remains owned by the table, so there is
     * no leak, only slack that the next intern may reuse. */
    st = atree__strmap_put(m, &t->map, e.ptr, len, t->entries.len);
    if (st != ATREE_OK) {
        return st;
    }
    st = atree__strentvec_push(m, &t->entries, e);
    if (st != ATREE_OK) {
        atree__strmap_remove(&t->map, e.ptr, len);
        return st;
    }
    *id = t->entries.len - 1;
    return ATREE_OK;
}

const char *atree__strtab_get(const struct atree__strtab *t, uint32_t id, uint32_t *len)
{
    if (id == ATREE_STR_UNKNOWN || id >= t->entries.len) {
        return NULL;
    }
    if (len != NULL) {
        *len = t->entries.data[id].len;
    }
    return t->entries.data[id].ptr;
}

uint32_t atree__strtab_count(const struct atree__strtab *t)
{
    return t->entries.len - 1;
}

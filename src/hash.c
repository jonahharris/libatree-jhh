/*
 * Open-addressing hash maps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hash.h"

#include <assert.h>
#include <string.h>

#include "compiler.h"

#define ATREE_MAP_MIN_CAP 16u
#define ATREE_MAP_MAX_CAP (UINT32_C(1) << 31)

uint64_t atree__hash_bytes(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= UINT64_C(1099511628211);
    }
    return atree__hash_u64(h ^ (uint64_t)len);
}

/* Occupancy check shared by both maps: grow when used >= 70% of cap. */
static bool needs_grow(uint32_t used, uint32_t cap)
{
    return cap == 0 || (uint64_t)used * 10 >= (uint64_t)cap * 7;
}

/* Smallest power of two >= need * 10 / 7, at least the minimum. */
static uint32_t cap_for(uint32_t need)
{
    uint64_t target = ((uint64_t)need * 10) / 7 + 1;
    uint32_t cap = ATREE_MAP_MIN_CAP;
    while ((uint64_t)cap < target) {
        if (cap >= ATREE_MAP_MAX_CAP) {
            return 0; /* caller reports ATREE_ERR_LIMIT */
        }
        cap <<= 1;
    }
    return cap;
}

/* ---- u64map ------------------------------------------------------------- */

void atree__u64map_init(struct atree__u64map *map)
{
    map->keys = NULL;
    map->vals = NULL;
    map->state = NULL;
    map->cap = 0;
    map->count = 0;
    map->used = 0;
}

void atree__u64map_free(struct atree__mem *m, struct atree__u64map *map)
{
    if (map->cap != 0) {
        atree__free_array(m, map->keys, map->cap, sizeof *map->keys);
        atree__free_array(m, map->vals, map->cap, sizeof *map->vals);
        atree__free_array(m, map->state, map->cap, sizeof *map->state);
    }
    atree__u64map_init(map);
}

static atree_status_t u64map_rehash(struct atree__mem *m, struct atree__u64map *map,
                                    uint32_t new_cap)
{
    struct atree__u64map n;
    uint32_t i;
    uint32_t mask = new_cap - 1;

    n.keys = atree__alloc_array(m, new_cap, sizeof *n.keys);
    n.vals = atree__alloc_array(m, new_cap, sizeof *n.vals);
    n.state = atree__zalloc_array(m, new_cap, sizeof *n.state);
    if (n.keys == NULL || n.vals == NULL || n.state == NULL) {
        atree__free_array(m, n.keys, new_cap, sizeof *n.keys);
        atree__free_array(m, n.vals, new_cap, sizeof *n.vals);
        atree__free_array(m, n.state, new_cap, sizeof *n.state);
        return ATREE_ERR_NOMEM;
    }
    for (i = 0; i < map->cap; i++) {
        if (map->state[i] == ATREE_SLOT_FULL) {
            uint32_t j = (uint32_t)atree__hash_u64(map->keys[i]) & mask;
            while (n.state[j] != ATREE_SLOT_EMPTY) {
                j = (j + 1) & mask;
            }
            n.keys[j] = map->keys[i];
            n.vals[j] = map->vals[i];
            n.state[j] = ATREE_SLOT_FULL;
        }
    }
    n.cap = new_cap;
    n.count = map->count;
    n.used = map->count;
    atree__u64map_free(m, map);
    *map = n;
    return ATREE_OK;
}

atree_status_t atree__u64map_reserve(struct atree__mem *m, struct atree__u64map *map, uint32_t n)
{
    uint32_t cap = cap_for(n);
    if (cap == 0) {
        return ATREE_ERR_LIMIT;
    }
    if (cap <= map->cap) {
        return ATREE_OK;
    }
    return u64map_rehash(m, map, cap);
}

/* Finds the slot holding key, or UINT32_MAX. */
static uint32_t u64map_find(const struct atree__u64map *map, uint64_t key)
{
    uint32_t mask;
    uint32_t j;
    if (map->cap == 0) {
        return UINT32_MAX;
    }
    mask = map->cap - 1;
    j = (uint32_t)atree__hash_u64(key) & mask;
    for (;;) {
        uint8_t s = map->state[j];
        if (s == ATREE_SLOT_EMPTY) {
            return UINT32_MAX;
        }
        if (s == ATREE_SLOT_FULL && map->keys[j] == key) {
            return j;
        }
        j = (j + 1) & mask;
    }
}

bool atree__u64map_get(const struct atree__u64map *map, uint64_t key, uint32_t *val)
{
    uint32_t j = u64map_find(map, key);
    if (j == UINT32_MAX) {
        return false;
    }
    if (val != NULL) {
        *val = map->vals[j];
    }
    return true;
}

atree_status_t atree__u64map_put(struct atree__mem *m, struct atree__u64map *map, uint64_t key,
                                 uint32_t val)
{
    uint32_t mask;
    uint32_t j;
    uint32_t tomb = UINT32_MAX;

    if (needs_grow(map->used + 1, map->cap)) {
        /* Size for the live count: a rehash also drops tombstones. */
        uint32_t cap = cap_for(map->count + 1);
        atree_status_t st;
        if (cap == 0) {
            return ATREE_ERR_LIMIT;
        }
        if (cap < map->cap) {
            cap = map->cap;
        }
        st = u64map_rehash(m, map, cap);
        if (st != ATREE_OK) {
            return st;
        }
    }
    mask = map->cap - 1;
    j = (uint32_t)atree__hash_u64(key) & mask;
    for (;;) {
        uint8_t s = map->state[j];
        if (s == ATREE_SLOT_EMPTY) {
            break;
        }
        if (s == ATREE_SLOT_TOMB) {
            if (tomb == UINT32_MAX) {
                tomb = j;
            }
        } else if (map->keys[j] == key) {
            map->vals[j] = val;
            return ATREE_OK;
        }
        j = (j + 1) & mask;
    }
    if (tomb != UINT32_MAX) {
        j = tomb; /* reuse: used stays the same */
    } else {
        map->used++;
    }
    map->keys[j] = key;
    map->vals[j] = val;
    map->state[j] = ATREE_SLOT_FULL;
    map->count++;
    return ATREE_OK;
}

bool atree__u64map_remove(struct atree__u64map *map, uint64_t key)
{
    uint32_t j = u64map_find(map, key);
    if (j == UINT32_MAX) {
        return false;
    }
    map->state[j] = ATREE_SLOT_TOMB;
    map->count--;
    return true;
}

void atree__u64map_clear(struct atree__u64map *map)
{
    if (map->cap != 0) {
        memset(map->state, 0, map->cap);
    }
    map->count = 0;
    map->used = 0;
}

bool atree__u64map_next(const struct atree__u64map *map, uint32_t *iter, uint64_t *key,
                        uint32_t *val)
{
    while (*iter < map->cap) {
        uint32_t j = (*iter)++;
        if (map->state[j] == ATREE_SLOT_FULL) {
            if (key != NULL) {
                *key = map->keys[j];
            }
            if (val != NULL) {
                *val = map->vals[j];
            }
            return true;
        }
    }
    return false;
}

/* ---- strmap ------------------------------------------------------------- */

void atree__strmap_init(struct atree__strmap *map)
{
    map->slots = NULL;
    map->state = NULL;
    map->cap = 0;
    map->count = 0;
    map->used = 0;
}

void atree__strmap_free(struct atree__mem *m, struct atree__strmap *map)
{
    if (map->cap != 0) {
        atree__free_array(m, map->slots, map->cap, sizeof *map->slots);
        atree__free_array(m, map->state, map->cap, sizeof *map->state);
    }
    atree__strmap_init(map);
}

static bool key_equal(const struct atree__strmap_entry *e, uint64_t hash, const char *key,
                      size_t len)
{
    return e->hash == hash && e->len == len && (len == 0 || memcmp(e->key, key, len) == 0);
}

static atree_status_t strmap_rehash(struct atree__mem *m, struct atree__strmap *map,
                                    uint32_t new_cap)
{
    struct atree__strmap n;
    uint32_t i;
    uint32_t mask = new_cap - 1;

    n.slots = atree__alloc_array(m, new_cap, sizeof *n.slots);
    n.state = atree__zalloc_array(m, new_cap, sizeof *n.state);
    if (n.slots == NULL || n.state == NULL) {
        atree__free_array(m, n.slots, new_cap, sizeof *n.slots);
        atree__free_array(m, n.state, new_cap, sizeof *n.state);
        return ATREE_ERR_NOMEM;
    }
    for (i = 0; i < map->cap; i++) {
        if (map->state[i] == ATREE_SLOT_FULL) {
            uint32_t j = (uint32_t)map->slots[i].hash & mask;
            while (n.state[j] != ATREE_SLOT_EMPTY) {
                j = (j + 1) & mask;
            }
            n.slots[j] = map->slots[i];
            n.state[j] = ATREE_SLOT_FULL;
        }
    }
    n.cap = new_cap;
    n.count = map->count;
    n.used = map->count;
    atree__strmap_free(m, map);
    *map = n;
    return ATREE_OK;
}

static uint32_t strmap_find(const struct atree__strmap *map, uint64_t hash, const char *key,
                            size_t len)
{
    uint32_t mask;
    uint32_t j;
    if (map->cap == 0) {
        return UINT32_MAX;
    }
    mask = map->cap - 1;
    j = (uint32_t)hash & mask;
    for (;;) {
        uint8_t s = map->state[j];
        if (s == ATREE_SLOT_EMPTY) {
            return UINT32_MAX;
        }
        if (s == ATREE_SLOT_FULL && key_equal(&map->slots[j], hash, key, len)) {
            return j;
        }
        j = (j + 1) & mask;
    }
}

bool atree__strmap_get(const struct atree__strmap *map, const char *key, size_t len, uint32_t *val)
{
    uint32_t j;
    if (len > UINT32_MAX) {
        return false;
    }
    j = strmap_find(map, atree__hash_bytes(key, len), key, len);
    if (j == UINT32_MAX) {
        return false;
    }
    if (val != NULL) {
        *val = map->slots[j].val;
    }
    return true;
}

atree_status_t atree__strmap_put(struct atree__mem *m, struct atree__strmap *map, const char *key,
                                 size_t len, uint32_t val)
{
    uint64_t hash;
    uint32_t mask;
    uint32_t j;
    uint32_t tomb = UINT32_MAX;

    if (len > UINT32_MAX) {
        return ATREE_ERR_LIMIT;
    }
    hash = atree__hash_bytes(key, len);
    if (needs_grow(map->used + 1, map->cap)) {
        uint32_t cap = cap_for(map->count + 1);
        atree_status_t st;
        if (cap == 0) {
            return ATREE_ERR_LIMIT;
        }
        if (cap < map->cap) {
            cap = map->cap;
        }
        st = strmap_rehash(m, map, cap);
        if (st != ATREE_OK) {
            return st;
        }
    }
    mask = map->cap - 1;
    j = (uint32_t)hash & mask;
    for (;;) {
        uint8_t s = map->state[j];
        if (s == ATREE_SLOT_EMPTY) {
            break;
        }
        if (s == ATREE_SLOT_TOMB) {
            if (tomb == UINT32_MAX) {
                tomb = j;
            }
        } else if (key_equal(&map->slots[j], hash, key, len)) {
            map->slots[j].val = val;
            return ATREE_OK;
        }
        j = (j + 1) & mask;
    }
    if (tomb != UINT32_MAX) {
        j = tomb;
    } else {
        map->used++;
    }
    map->slots[j].key = key;
    map->slots[j].len = (uint32_t)len;
    map->slots[j].val = val;
    map->slots[j].hash = hash;
    map->state[j] = ATREE_SLOT_FULL;
    map->count++;
    return ATREE_OK;
}

bool atree__strmap_remove(struct atree__strmap *map, const char *key, size_t len)
{
    uint32_t j;
    if (len > UINT32_MAX) {
        return false;
    }
    j = strmap_find(map, atree__hash_bytes(key, len), key, len);
    if (j == UINT32_MAX) {
        return false;
    }
    map->state[j] = ATREE_SLOT_TOMB;
    map->slots[j].key = NULL;
    map->count--;
    return true;
}

/*
 * Hash maps used throughout the tree.
 *
 *   atree__u64map  uint64_t key -> uint32_t value   (ids, structural hashes)
 *   atree__strmap  (bytes, len)  -> uint32_t value  (string interning)
 *
 * Both are open-addressing tables with linear probing, power-of-two
 * capacity, tombstones, and rehash at 70% occupancy (live + tombstones).
 * Keys of the string map are borrowed: the caller guarantees the bytes
 * outlive the entry (the string table owns them in a chunked arena).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_HASH_H
#define ATREE_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alloc.h"

/* ---- hashing primitives ------------------------------------------------- */

/* splitmix64 finalizer: a strong 64-bit mix for integer keys. */
static inline uint64_t atree__hash_u64(uint64_t x)
{
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

/* FNV-1a over bytes, then finalized. */
uint64_t atree__hash_bytes(const void *data, size_t len);

/* Combines two hashes (order-dependent). */
static inline uint64_t atree__hash_combine(uint64_t h, uint64_t v)
{
    return atree__hash_u64(h ^ (v + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2)));
}

/* ---- u64 -> u32 --------------------------------------------------------- */

enum { ATREE_SLOT_EMPTY = 0, ATREE_SLOT_FULL = 1, ATREE_SLOT_TOMB = 2 };

struct atree__u64map {
    uint64_t *keys;
    uint32_t *vals;
    uint8_t *state;
    uint32_t cap;   /* power of two, or 0 before first insert */
    uint32_t count; /* live entries */
    uint32_t used;  /* live + tombstones */
};

void atree__u64map_init(struct atree__u64map *map);
void atree__u64map_free(struct atree__mem *m, struct atree__u64map *map);
/* Pre-sizes for at least n live entries without rehash. */
atree_status_t atree__u64map_reserve(struct atree__mem *m, struct atree__u64map *map, uint32_t n);
bool atree__u64map_get(const struct atree__u64map *map, uint64_t key, uint32_t *val);
/* Inserts or overwrites. */
atree_status_t atree__u64map_put(struct atree__mem *m, struct atree__u64map *map, uint64_t key,
                                 uint32_t val);
/* Returns false if absent. */
bool atree__u64map_remove(struct atree__u64map *map, uint64_t key);
void atree__u64map_clear(struct atree__u64map *map);
/* Iteration: start with *iter = 0; returns false when exhausted. */
bool atree__u64map_next(const struct atree__u64map *map, uint32_t *iter, uint64_t *key,
                        uint32_t *val);

/* ---- (bytes, len) -> u32 ------------------------------------------------ */

struct atree__strmap_entry {
    const char *key; /* borrowed; NULL when the slot is empty or a tombstone */
    uint32_t len;
    uint32_t val;
    uint64_t hash;
};

struct atree__strmap {
    struct atree__strmap_entry *slots;
    uint8_t *state;
    uint32_t cap;
    uint32_t count;
    uint32_t used;
};

void atree__strmap_init(struct atree__strmap *map);
void atree__strmap_free(struct atree__mem *m, struct atree__strmap *map);
bool atree__strmap_get(const struct atree__strmap *map, const char *key, size_t len, uint32_t *val);
/* Inserts or overwrites; key bytes are borrowed. len must fit uint32_t. */
atree_status_t atree__strmap_put(struct atree__mem *m, struct atree__strmap *map, const char *key,
                                 size_t len, uint32_t val);
bool atree__strmap_remove(struct atree__strmap *map, const char *key, size_t len);

#endif /* ATREE_HASH_H */

/*
 * String table: interns byte strings to dense 32-bit ids so that string
 * comparisons in predicates and events become integer comparisons.
 *
 *   - id 0 is reserved as the "unknown string" sentinel: an event string the
 *     tree has never seen maps to 0 and therefore equals no literal;
 *   - ids are assigned 1, 2, 3, ... and never reused;
 *   - bytes are copied into a chunked arena so pointers stay stable; the
 *     hash map borrows them.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_STRTAB_H
#define ATREE_STRTAB_H

#include <stddef.h>
#include <stdint.h>

#include "alloc.h"
#include "hash.h"
#include "vec.h"

#define ATREE_STR_UNKNOWN 0u
#define ATREE_STRTAB_CHUNK_SIZE ((size_t)64 * 1024)

struct atree__strtab_chunk {
    char *base;
    size_t used;
    size_t cap;
};

struct atree__strtab_entry {
    const char *ptr; /* NUL-terminated copy inside a chunk */
    uint32_t len;
};

ATREE_VEC_DEFINE(atree__chunkvec, struct atree__strtab_chunk);
ATREE_VEC_DEFINE(atree__strentvec, struct atree__strtab_entry);

struct atree__strtab {
    struct atree__strmap map;
    struct atree__chunkvec chunks;
    struct atree__strentvec entries; /* index = id; entries.data[0] is the sentinel */
};

atree_status_t atree__strtab_init(struct atree__mem *m, struct atree__strtab *t);
void atree__strtab_free(struct atree__mem *m, struct atree__strtab *t);

/* Returns the id of s, or ATREE_STR_UNKNOWN. Read-only; safe concurrently. */
uint32_t atree__strtab_lookup(const struct atree__strtab *t, const char *s, size_t len);

/* Returns the id of s, interning it if new. */
atree_status_t atree__strtab_intern(struct atree__mem *m, struct atree__strtab *t, const char *s,
                                    size_t len, uint32_t *id);

/* Bytes of an id (NUL-terminated); NULL for 0 or out of range. */
const char *atree__strtab_get(const struct atree__strtab *t, uint32_t id, uint32_t *len);

/* Number of interned strings (excluding the sentinel). */
uint32_t atree__strtab_count(const struct atree__strtab *t);

#endif /* ATREE_STRTAB_H */

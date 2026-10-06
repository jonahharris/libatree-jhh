/*
 * Attribute table: the fixed, typed schema a tree is created with. Attribute
 * ids are dense indexes in declaration order, so events are dense arrays.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_ATTR_H
#define ATREE_ATTR_H

#include <stddef.h>
#include <stdint.h>

#include "atree.h"

#include "alloc.h"
#include "hash.h"
#include "vec.h"

struct atree__attr {
    char *name; /* owned, NUL-terminated */
    uint32_t name_len;
    atree_type_t type;
};

ATREE_VEC_DEFINE(atree__attrvec, struct atree__attr);

struct atree__attrs {
    struct atree__attrvec defs;
    struct atree__strmap by_name; /* name -> id */
};

/* Validates and copies the definitions. Errors: ATREE_ERR_INVALID_ARG (NULL
 * or empty name, not an identifier, a DSL keyword, bad type),
 * ATREE_ERR_DUPLICATE_ATTR, ATREE_ERR_LIMIT, ATREE_ERR_NOMEM. On error the
 * table is left empty (freed). */
atree_status_t atree__attrs_init(struct atree__mem *m, struct atree__attrs *a,
                                 const atree_attr_def_t *defs, size_t n);
void atree__attrs_free(struct atree__mem *m, struct atree__attrs *a);

/* Returns the id or ATREE_ATTR_INVALID. */
atree_attr_id_t atree__attrs_lookup(const struct atree__attrs *a, const char *name, size_t len);

static inline uint32_t atree__attrs_count(const struct atree__attrs *a)
{
    return a->defs.len;
}

/* id must be valid. */
static inline const struct atree__attr *atree__attrs_get(const struct atree__attrs *a,
                                                         atree_attr_id_t id)
{
    return &a->defs.data[id];
}

static inline bool atree__type_is_list(atree_type_t t)
{
    return t == ATREE_TYPE_INT_LIST || t == ATREE_TYPE_STRING_LIST;
}

#endif /* ATREE_ATTR_H */

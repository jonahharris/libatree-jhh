/*
 * Small public helpers: version, status strings, configuration defaults,
 * and the vector growth helper shared by vec.h instantiations.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "atree.h"

#include "compiler.h"
#include "vec.h"

const char *atree_version(void)
{
    return ATREE_VERSION_STRING;
}

const char *atree_strerror(atree_status_t status)
{
    switch (status) {
    case ATREE_OK:
        return "success";
    case ATREE_ERR_NOMEM:
        return "out of memory";
    case ATREE_ERR_INVALID_ARG:
        return "invalid argument";
    case ATREE_ERR_SYNTAX:
        return "syntax error";
    case ATREE_ERR_UNKNOWN_ATTR:
        return "unknown attribute";
    case ATREE_ERR_TYPE_MISMATCH:
        return "type mismatch";
    case ATREE_ERR_DUPLICATE_ATTR:
        return "duplicate attribute";
    case ATREE_ERR_DUPLICATE_ID:
        return "duplicate subscription id";
    case ATREE_ERR_NOT_FOUND:
        return "not found";
    case ATREE_ERR_TOO_DEEP:
        return "expression too deeply nested";
    case ATREE_ERR_LIMIT:
        return "capacity limit reached";
    case ATREE_ERR_INVALID_LITERAL:
        return "invalid literal";
    case ATREE_ERR_CORRUPT:
        return "internal structure is inconsistent";
    case ATREE_ERR_CANCELLED:
        return "cancelled by callback";
    default:
        return "unknown status";
    }
}

void atree_config_init(atree_config_t *cfg)
{
    if (cfg != NULL) {
        memset(cfg, 0, sizeof *cfg);
    }
}

void *atree__vec_grow(struct atree__mem *m, void *data, size_t elem_size, uint32_t cap_now,
                      uint32_t need, uint32_t *cap_out, atree_status_t *st)
{
    uint64_t doubled = (uint64_t)cap_now * 2;
    uint32_t new_cap;
    size_t old_bytes;
    size_t new_bytes;
    void *p;

    if (doubled > UINT32_MAX) {
        doubled = UINT32_MAX;
    }
    new_cap = (uint32_t)doubled;
    if (new_cap < need) {
        new_cap = need;
    }
    if (new_cap < ATREE_VEC_MIN_CAP) {
        new_cap = ATREE_VEC_MIN_CAP;
    }
    if (atree__mul_overflows(new_cap, elem_size, &new_bytes)) {
        *st = ATREE_ERR_LIMIT;
        return data;
    }
    old_bytes = (size_t)cap_now * elem_size;
    if (data == NULL) {
        p = atree__alloc(m, new_bytes);
    } else {
        p = atree__realloc(m, data, old_bytes, new_bytes);
    }
    if (p == NULL) {
        *st = ATREE_ERR_NOMEM;
        return data;
    }
    *cap_out = new_cap;
    *st = ATREE_OK;
    return p;
}

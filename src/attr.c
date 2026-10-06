/*
 * Attribute table.
 *
 * SPDX-License-Identifier: MIT
 */
#include "attr.h"

#include <string.h>

#include "keywords.h"

static bool type_is_valid(atree_type_t t)
{
    switch (t) {
    case ATREE_TYPE_BOOL:
    case ATREE_TYPE_INT:
    case ATREE_TYPE_FLOAT:
    case ATREE_TYPE_STRING:
    case ATREE_TYPE_INT_LIST:
    case ATREE_TYPE_STRING_LIST:
        return true;
    default:
        return false;
    }
}

atree_status_t atree__attrs_init(struct atree__mem *m, struct atree__attrs *a,
                                 const atree_attr_def_t *defs, size_t n)
{
    size_t i;
    atree_status_t st = ATREE_OK;

    atree__attrvec_init(&a->defs);
    atree__strmap_init(&a->by_name);

    if (n > 0 && defs == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (n >= ATREE_ATTR_INVALID) {
        return ATREE_ERR_LIMIT;
    }
    if (n > 0) {
        st = atree__attrvec_reserve(m, &a->defs, (uint32_t)n);
    }
    for (i = 0; st == ATREE_OK && i < n; i++) {
        const char *name = defs[i].name;
        size_t len;
        struct atree__attr attr;

        if (name == NULL || !type_is_valid(defs[i].type)) {
            st = ATREE_ERR_INVALID_ARG;
            break;
        }
        len = strlen(name);
        if (len == 0 || len >= UINT32_MAX || !atree__is_identifier(name, len) ||
            atree__is_keyword(name, len)) {
            st = ATREE_ERR_INVALID_ARG;
            break;
        }
        if (atree__strmap_get(&a->by_name, name, len, NULL)) {
            st = ATREE_ERR_DUPLICATE_ATTR;
            break;
        }
        attr.name = atree__strndup(m, name, len);
        if (attr.name == NULL) {
            st = ATREE_ERR_NOMEM;
            break;
        }
        attr.name_len = (uint32_t)len;
        attr.type = defs[i].type;
        st = atree__strmap_put(m, &a->by_name, attr.name, len, a->defs.len);
        if (st == ATREE_OK) {
            st = atree__attrvec_push(m, &a->defs, attr);
            if (st != ATREE_OK) {
                atree__strmap_remove(&a->by_name, attr.name, len);
            }
        }
        if (st != ATREE_OK) {
            atree__strfree(m, attr.name, len);
        }
    }
    if (st != ATREE_OK) {
        atree__attrs_free(m, a);
    }
    return st;
}

void atree__attrs_free(struct atree__mem *m, struct atree__attrs *a)
{
    uint32_t i;
    for (i = 0; i < a->defs.len; i++) {
        atree__strfree(m, a->defs.data[i].name, a->defs.data[i].name_len);
    }
    atree__attrvec_free(m, &a->defs);
    atree__strmap_free(m, &a->by_name);
}

atree_attr_id_t atree__attrs_lookup(const struct atree__attrs *a, const char *name, size_t len)
{
    uint32_t id;
    if (name == NULL) {
        return ATREE_ATTR_INVALID;
    }
    if (atree__strmap_get(&a->by_name, name, len, &id)) {
        return id;
    }
    return ATREE_ATTR_INVALID;
}

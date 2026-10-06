/*
 * Events: public construction API.
 *
 * SPDX-License-Identifier: MIT
 */
#include "event.h"

#include <string.h>

#include "atree_internal.h"

atree_status_t atree_event_create(const atree_t *tree, atree_event_t **out)
{
    struct atree__mem mem;
    atree_event_t *ev;
    uint32_t n;

    if (tree == NULL || out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    atree__mem_init(&mem, &tree->mem.a);
    ev = atree__zalloc(&mem, sizeof *ev);
    if (ev == NULL) {
        return ATREE_ERR_NOMEM;
    }
    ev->mem = mem;
    ev->tree = tree;
    n = atree__attrs_count(&tree->attrs);
    ev->nattrs = n;
    if (n > 0) {
        ev->values = atree__zalloc_array(&ev->mem, n, sizeof *ev->values);
        ev->bufs = atree__zalloc_array(&ev->mem, n, sizeof *ev->bufs);
        if (ev->values == NULL || ev->bufs == NULL) {
            atree_event_destroy(ev);
            return ATREE_ERR_NOMEM;
        }
    }
    *out = ev;
    return ATREE_OK;
}

void atree_event_destroy(atree_event_t *ev)
{
    struct atree__mem mem;
    uint32_t i;
    if (ev == NULL) {
        return;
    }
    if (ev->bufs != NULL) {
        for (i = 0; i < ev->nattrs; i++) {
            atree_type_t t = atree__attrs_get(&ev->tree->attrs, i)->type;
            if (ev->bufs[i].cap == 0) {
                continue;
            }
            if (t == ATREE_TYPE_INT_LIST) {
                atree__free_array(&ev->mem, ev->bufs[i].data.i, ev->bufs[i].cap, sizeof(int64_t));
            } else {
                atree__free_array(&ev->mem, ev->bufs[i].data.s, ev->bufs[i].cap, sizeof(uint32_t));
            }
        }
        atree__free_array(&ev->mem, ev->bufs, ev->nattrs, sizeof *ev->bufs);
    }
    if (ev->values != NULL) {
        atree__free_array(&ev->mem, ev->values, ev->nattrs, sizeof *ev->values);
    }
    mem = ev->mem;
    atree__free(&mem, ev, sizeof *ev);
}

void atree_event_clear(atree_event_t *ev)
{
    uint32_t i;
    if (ev == NULL) {
        return;
    }
    for (i = 0; i < ev->nattrs; i++) {
        ev->values[i].kind = ATREE_V_UNDEFINED;
    }
}

/* Validates id and type; returns the attribute id's value slot. */
static atree_status_t slot(atree_event_t *ev, atree_attr_id_t id, atree_type_t expected,
                           struct atree__value **out)
{
    if (ev == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (id >= ev->nattrs) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (atree__attrs_get(&ev->tree->attrs, id)->type != expected) {
        return ATREE_ERR_TYPE_MISMATCH;
    }
    *out = &ev->values[id];
    return ATREE_OK;
}

static atree_status_t lookup(const atree_event_t *ev, const char *attr, atree_attr_id_t *id)
{
    if (ev == NULL || attr == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *id = atree_attr_lookup(ev->tree, attr);
    return *id == ATREE_ATTR_INVALID ? ATREE_ERR_UNKNOWN_ATTR : ATREE_OK;
}

/* Ensures bufs[id] can hold n elements of elem bytes. */
static atree_status_t reserve_list(atree_event_t *ev, atree_attr_id_t id, uint32_t n, bool is_int)
{
    struct atree__listbuf *b = &ev->bufs[id];
    uint32_t cap;
    void *p;
    size_t elem = is_int ? sizeof(int64_t) : sizeof(uint32_t);

    if (n <= b->cap) {
        return ATREE_OK;
    }
    cap = b->cap == 0 ? ATREE_VEC_MIN_CAP : b->cap;
    while (cap < n) {
        if (cap > UINT32_MAX / 2) {
            cap = UINT32_MAX;
            break;
        }
        cap *= 2;
    }
    if (b->cap == 0) {
        p = atree__alloc_array(&ev->mem, cap, elem);
    } else if (is_int) {
        p = atree__realloc_array(&ev->mem, b->data.i, b->cap, cap, elem);
    } else {
        p = atree__realloc_array(&ev->mem, b->data.s, b->cap, cap, elem);
    }
    if (p == NULL) {
        return ATREE_ERR_NOMEM;
    }
    if (is_int) {
        b->data.i = p;
    } else {
        b->data.s = p;
    }
    b->cap = cap;
    return ATREE_OK;
}

/* ---- by id -------------------------------------------------------------- */

atree_status_t atree_event_set_bool_id(atree_event_t *ev, atree_attr_id_t id, bool value)
{
    struct atree__value *v;
    atree_status_t st = slot(ev, id, ATREE_TYPE_BOOL, &v);
    if (st != ATREE_OK) {
        return st;
    }
    v->kind = ATREE_V_BOOL;
    v->u.b = value;
    return ATREE_OK;
}

atree_status_t atree_event_set_int_id(atree_event_t *ev, atree_attr_id_t id, int64_t value)
{
    struct atree__value *v;
    atree_status_t st = slot(ev, id, ATREE_TYPE_INT, &v);
    if (st != ATREE_OK) {
        return st;
    }
    v->kind = ATREE_V_INT;
    v->u.i = value;
    return ATREE_OK;
}

atree_status_t atree_event_set_float_id(atree_event_t *ev, atree_attr_id_t id, double value)
{
    struct atree__value *v;
    atree_status_t st = slot(ev, id, ATREE_TYPE_FLOAT, &v);
    if (st != ATREE_OK) {
        return st;
    }
    if (atree__double_is_nan(value)) {
        v->kind = ATREE_V_UNDEFINED; /* NaN carries no information: undefined */
        return ATREE_OK;
    }
    v->kind = ATREE_V_FLOAT;
    v->u.f = value;
    return ATREE_OK;
}

atree_status_t atree_event_set_string_id(atree_event_t *ev, atree_attr_id_t id, const char *s,
                                         size_t len)
{
    struct atree__value *v;
    atree_status_t st = slot(ev, id, ATREE_TYPE_STRING, &v);
    if (st != ATREE_OK) {
        return st;
    }
    if (s == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (len == SIZE_MAX) {
        len = strlen(s);
    }
    atree__rdlock(ev->tree);
    v->u.s = atree__strtab_lookup(&ev->tree->strings, s, len);
    atree__rdunlock(ev->tree);
    v->kind = ATREE_V_STRING;
    return ATREE_OK;
}

atree_status_t atree_event_set_int_list_id(atree_event_t *ev, atree_attr_id_t id,
                                           const int64_t *values, size_t n)
{
    struct atree__value *v;
    atree_status_t st = slot(ev, id, ATREE_TYPE_INT_LIST, &v);
    if (st != ATREE_OK) {
        return st;
    }
    if ((values == NULL && n > 0) || n >= UINT32_MAX) {
        return n >= UINT32_MAX ? ATREE_ERR_LIMIT : ATREE_ERR_INVALID_ARG;
    }
    st = reserve_list(ev, id, (uint32_t)n, true);
    if (st != ATREE_OK) {
        return st;
    }
    if (n > 0) {
        memcpy(ev->bufs[id].data.i, values, n * sizeof *values);
    }
    v->kind = ATREE_V_INT_LIST;
    v->u.il.data = ev->bufs[id].data.i;
    v->u.il.len = atree__sort_unique_i64(ev->bufs[id].data.i, (uint32_t)n);
    return ATREE_OK;
}

atree_status_t atree_event_set_string_list_id(atree_event_t *ev, atree_attr_id_t id,
                                              const char *const *strings, const size_t *lens,
                                              size_t n)
{
    struct atree__value *v;
    size_t i;
    atree_status_t st = slot(ev, id, ATREE_TYPE_STRING_LIST, &v);
    if (st != ATREE_OK) {
        return st;
    }
    if ((strings == NULL && n > 0) || n >= UINT32_MAX) {
        return n >= UINT32_MAX ? ATREE_ERR_LIMIT : ATREE_ERR_INVALID_ARG;
    }
    for (i = 0; i < n; i++) {
        if (strings[i] == NULL) {
            return ATREE_ERR_INVALID_ARG;
        }
    }
    st = reserve_list(ev, id, (uint32_t)n, false);
    if (st != ATREE_OK) {
        return st;
    }
    atree__rdlock(ev->tree);
    for (i = 0; i < n; i++) {
        size_t len = (lens != NULL && lens[i] != SIZE_MAX) ? lens[i] : strlen(strings[i]);
        ev->bufs[id].data.s[i] = atree__strtab_lookup(&ev->tree->strings, strings[i], len);
    }
    atree__rdunlock(ev->tree);
    v->kind = ATREE_V_STRING_LIST;
    v->u.sl.data = ev->bufs[id].data.s;
    v->u.sl.len = atree__sort_unique_u32(ev->bufs[id].data.s, (uint32_t)n);
    return ATREE_OK;
}

atree_status_t atree_event_set_undefined_id(atree_event_t *ev, atree_attr_id_t id)
{
    if (ev == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (id >= ev->nattrs) {
        return ATREE_ERR_INVALID_ARG;
    }
    ev->values[id].kind = ATREE_V_UNDEFINED;
    return ATREE_OK;
}

/* ---- by name ------------------------------------------------------------ */

atree_status_t atree_event_set_bool(atree_event_t *ev, const char *attr, bool value)
{
    atree_attr_id_t id;
    atree_status_t st = lookup(ev, attr, &id);
    return st != ATREE_OK ? st : atree_event_set_bool_id(ev, id, value);
}

atree_status_t atree_event_set_int(atree_event_t *ev, const char *attr, int64_t value)
{
    atree_attr_id_t id;
    atree_status_t st = lookup(ev, attr, &id);
    return st != ATREE_OK ? st : atree_event_set_int_id(ev, id, value);
}

atree_status_t atree_event_set_float(atree_event_t *ev, const char *attr, double value)
{
    atree_attr_id_t id;
    atree_status_t st = lookup(ev, attr, &id);
    return st != ATREE_OK ? st : atree_event_set_float_id(ev, id, value);
}

atree_status_t atree_event_set_string(atree_event_t *ev, const char *attr, const char *s,
                                      size_t len)
{
    atree_attr_id_t id;
    atree_status_t st = lookup(ev, attr, &id);
    return st != ATREE_OK ? st : atree_event_set_string_id(ev, id, s, len);
}

atree_status_t atree_event_set_int_list(atree_event_t *ev, const char *attr, const int64_t *values,
                                        size_t n)
{
    atree_attr_id_t id;
    atree_status_t st = lookup(ev, attr, &id);
    return st != ATREE_OK ? st : atree_event_set_int_list_id(ev, id, values, n);
}

atree_status_t atree_event_set_string_list(atree_event_t *ev, const char *attr,
                                           const char *const *strings, const size_t *lens, size_t n)
{
    atree_attr_id_t id;
    atree_status_t st = lookup(ev, attr, &id);
    return st != ATREE_OK ? st : atree_event_set_string_list_id(ev, id, strings, lens, n);
}

atree_status_t atree_event_set_undefined(atree_event_t *ev, const char *attr)
{
    atree_attr_id_t id;
    atree_status_t st = lookup(ev, attr, &id);
    return st != ATREE_OK ? st : atree_event_set_undefined_id(ev, id);
}

/*
 * Typed growable arrays over the library allocator.
 *
 * ATREE_VEC_DEFINE(name, T) defines `struct name { T *data; uint32_t len, cap; }`
 * and static inline operations name_init/free/reserve/push/insert_at/
 * remove_at/swap_remove/clear. Growth arithmetic lives in atree__vec_grow()
 * so the generated code stays small. Lengths are uint32_t because every
 * array in the tree is indexed by 32-bit ids.
 *
 * No type punning: the generated functions operate on the concrete T*, and
 * the one generic helper returns a void* that is assigned (implicitly
 * converted) to the typed pointer.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_VEC_H
#define ATREE_VEC_H

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "alloc.h"
#include "compiler.h"

#define ATREE_VEC_MIN_CAP 4u

/* Grows the array to hold at least `need` elements. On success returns the
 * new data pointer and updates *cap; on failure returns `data` unchanged and
 * sets *st to ATREE_ERR_NOMEM or ATREE_ERR_LIMIT. */
void *atree__vec_grow(struct atree__mem *m, void *data, size_t elem_size, uint32_t cap_now,
                      uint32_t need, uint32_t *cap_out, atree_status_t *st);

#define ATREE_VEC_DEFINE(name, T)                                                                  \
    struct name {                                                                                  \
        T *data;                                                                                   \
        uint32_t len;                                                                              \
        uint32_t cap;                                                                              \
    };                                                                                             \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline void name##_init(struct name *v)                              \
    {                                                                                              \
        v->data = NULL;                                                                            \
        v->len = 0;                                                                                \
        v->cap = 0;                                                                                \
    }                                                                                              \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline void name##_free(struct atree__mem *m, struct name *v)        \
    {                                                                                              \
        if (v->data != NULL) {                                                                     \
            atree__free(m, v->data, (size_t)v->cap * sizeof *v->data);                             \
        }                                                                                          \
        name##_init(v);                                                                            \
    }                                                                                              \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline atree_status_t name##_reserve(struct atree__mem *m,           \
                                                                   struct name *v, uint32_t need)  \
    {                                                                                              \
        atree_status_t st = ATREE_OK;                                                              \
        if (need <= v->cap) {                                                                      \
            return ATREE_OK;                                                                       \
        }                                                                                          \
        v->data = atree__vec_grow(m, v->data, sizeof *v->data, v->cap, need, &v->cap, &st);        \
        return st;                                                                                 \
    }                                                                                              \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline atree_status_t name##_push(struct atree__mem *m,              \
                                                                struct name *v, T val)             \
    {                                                                                              \
        if (v->len == v->cap) {                                                                    \
            atree_status_t st;                                                                     \
            if (v->len == UINT32_MAX) {                                                            \
                return ATREE_ERR_LIMIT;                                                            \
            }                                                                                      \
            st = name##_reserve(m, v, v->len + 1);                                                 \
            if (st != ATREE_OK) {                                                                  \
                return st;                                                                         \
            }                                                                                      \
        }                                                                                          \
        v->data[v->len++] = val;                                                                   \
        return ATREE_OK;                                                                           \
    }                                                                                              \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline atree_status_t name##_insert_at(                              \
        struct atree__mem *m, struct name *v, uint32_t pos, T val)                                 \
    {                                                                                              \
        assert(pos <= v->len);                                                                     \
        if (v->len == v->cap) {                                                                    \
            atree_status_t st;                                                                     \
            if (v->len == UINT32_MAX) {                                                            \
                return ATREE_ERR_LIMIT;                                                            \
            }                                                                                      \
            st = name##_reserve(m, v, v->len + 1);                                                 \
            if (st != ATREE_OK) {                                                                  \
                return st;                                                                         \
            }                                                                                      \
        }                                                                                          \
        if (pos < v->len) {                                                                        \
            memmove(&v->data[pos + 1], &v->data[pos], (size_t)(v->len - pos) * sizeof(T));         \
        }                                                                                          \
        v->data[pos] = val;                                                                        \
        v->len++;                                                                                  \
        return ATREE_OK;                                                                           \
    }                                                                                              \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline void name##_remove_at(struct name *v, uint32_t pos)           \
    {                                                                                              \
        assert(pos < v->len);                                                                      \
        if (pos + 1 < v->len) {                                                                    \
            memmove(&v->data[pos], &v->data[pos + 1], (size_t)(v->len - pos - 1) * sizeof(T));     \
        }                                                                                          \
        v->len--;                                                                                  \
    }                                                                                              \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline void name##_swap_remove(struct name *v, uint32_t pos)         \
    {                                                                                              \
        assert(pos < v->len);                                                                      \
        v->len--;                                                                                  \
        if (pos != v->len) {                                                                       \
            v->data[pos] = v->data[v->len];                                                        \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline void name##_clear(struct name *v)                             \
    {                                                                                              \
        v->len = 0;                                                                                \
    }                                                                                              \
                                                                                                   \
    typedef int atree__vec_semicolon_##name

/* Common instantiations. */
ATREE_VEC_DEFINE(atree__u32vec, uint32_t);
ATREE_VEC_DEFINE(atree__u64vec, uint64_t);

/* Linear search; scalar element types only. Returns index or UINT32_MAX. */
static inline uint32_t atree__u32vec_find(const struct atree__u32vec *v, uint32_t val)
{
    uint32_t i;
    for (i = 0; i < v->len; i++) {
        if (v->data[i] == val) {
            return i;
        }
    }
    return UINT32_MAX;
}

static inline uint32_t atree__u64vec_find(const struct atree__u64vec *v, uint64_t val)
{
    uint32_t i;
    for (i = 0; i < v->len; i++) {
        if (v->data[i] == val) {
            return i;
        }
    }
    return UINT32_MAX;
}

#endif /* ATREE_VEC_H */

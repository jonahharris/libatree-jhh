/*
 * Segmented slabs: the arrays indexed by node and predicate id.
 *
 * A flat array that doubles carries up to half its size in slack right
 * after a doubling and, while it grows, holds both copies: at two million
 * 64-byte nodes that is a 134 MB copy with 268 MB resident. A slab keeps
 * fixed-size segments of ATREE_SLAB_SEG elements behind a small table.
 * Growing allocates one more segment and copies nothing, so the slack is
 * at most one segment and the peak is the live size; elements never move
 * once their segment is full. Only the first segment grows geometrically
 * up to a full segment, so a small tree stays small.
 *
 * Access is segs[i >> SHIFT][i & MASK]: one more load than a flat array,
 * from a table that stays in L1 (a 2M-node tree has 128 entries).
 *
 * ATREE_SLAB_DEFINE(name, T) defines `struct name` and static inline
 * name_init/free/reserve/push/at. The segment table is `void **` so the
 * growth code is shared; the typed accessors convert each segment pointer
 * back to T* (a conversion, not a reinterpretation: the segment was
 * allocated for T).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_SLAB_H
#define ATREE_SLAB_H

#include <assert.h>
#include <stdint.h>

#include "alloc.h"
#include "compiler.h"

#define ATREE_SLAB_SHIFT 14u
#define ATREE_SLAB_SEG (UINT32_C(1) << ATREE_SLAB_SHIFT)
#define ATREE_SLAB_MASK (ATREE_SLAB_SEG - 1)
#define ATREE_SLAB_MIN_CAP 16u /* first allocation of the first segment */

/* Makes `*cap` >= need: grows the first segment (doubling, up to one full
 * segment) or appends full segments, growing the table geometrically.
 * On failure nothing changes and the result is ATREE_ERR_NOMEM. */
atree_status_t atree__slab_reserve(struct atree__mem *m, void ***segs, uint32_t *nsegs,
                                   uint32_t *segcap, uint32_t *cap, uint32_t need, size_t elem);

/* Frees every segment and the table. */
void atree__slab_free(struct atree__mem *m, void **segs, uint32_t nsegs, uint32_t segcap,
                      uint32_t cap, size_t elem);

#define ATREE_SLAB_DEFINE(name, T)                                                                 \
    struct name {                                                                                  \
        void **segs;     /* segs[k] holds elements [k << SHIFT, (k + 1) << SHIFT) */               \
        uint32_t nsegs;  /* allocated segments                                     */              \
        uint32_t segcap; /* table capacity                                         */              \
        uint32_t len;                                                                              \
        uint32_t cap; /* addressable elements: the first segment's size, or nsegs << SHIFT */      \
    };                                                                                             \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline void name##_init(struct name *v)                              \
    {                                                                                              \
        v->segs = NULL;                                                                            \
        v->nsegs = 0;                                                                              \
        v->segcap = 0;                                                                             \
        v->len = 0;                                                                                \
        v->cap = 0;                                                                                \
    }                                                                                              \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline void name##_free(struct atree__mem *m, struct name *v)        \
    {                                                                                              \
        atree__slab_free(m, v->segs, v->nsegs, v->segcap, v->cap, sizeof(T));                      \
        name##_init(v);                                                                            \
    }                                                                                              \
                                                                                                   \
    ATREE_MAYBE_UNUSED static inline atree_status_t name##_reserve(struct atree__mem *m,           \
                                                                   struct name *v, uint32_t need)  \
    {                                                                                              \
        if (need <= v->cap) {                                                                      \
            return ATREE_OK;                                                                       \
        }                                                                                          \
        return atree__slab_reserve(m, &v->segs, &v->nsegs, &v->segcap, &v->cap, need, sizeof(T));  \
    }                                                                                              \
                                                                                                   \
    /* Element i through a copy of the segment table. Hot loops that call                          \
     * functions between accesses hoist `v->segs` into a local and use this,                       \
     * so the compiler need not reload the table pointer after every call. */                      \
    ATREE_MAYBE_UNUSED static inline T *name##_from(void *const *segs, uint32_t i)                 \
    {                                                                                              \
        return (T *)segs[i >> ATREE_SLAB_SHIFT] + (i & ATREE_SLAB_MASK);                           \
    }                                                                                              \
                                                                                                   \
    /* Element i, which must exist (i < len). */                                                   \
    ATREE_MAYBE_UNUSED static inline T *name##_at(const struct name *v, uint32_t i)                \
    {                                                                                              \
        assert(i < v->len);                                                                        \
        return name##_from(v->segs, i);                                                            \
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
        *((T *)v->segs[v->len >> ATREE_SLAB_SHIFT] + (v->len & ATREE_SLAB_MASK)) = val;            \
        v->len++;                                                                                  \
        return ATREE_OK;                                                                           \
    }                                                                                              \
                                                                                                   \
    typedef int atree__slab_semicolon_##name

ATREE_SLAB_DEFINE(atree__u32slab, uint32_t);
ATREE_SLAB_DEFINE(atree__u64slab, uint64_t);

#endif /* ATREE_SLAB_H */

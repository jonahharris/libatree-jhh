/*
 * Segmented slab growth (slab.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "slab.h"

#include <string.h>

/* Bytes of segment k: only the first segment can be partial. */
static size_t seg_elems(uint32_t k, uint32_t nsegs, uint32_t cap)
{
    return k == 0 && nsegs == 1 ? cap : ATREE_SLAB_SEG;
}

atree_status_t atree__slab_reserve(struct atree__mem *m, void ***segs, uint32_t *nsegs,
                                   uint32_t *segcap, uint32_t *cap, uint32_t need, size_t elem)
{
    while (*cap < need) {
        if (*cap < ATREE_SLAB_SEG) {
            /* The first segment doubles until it is a full segment. */
            uint32_t size = *cap == 0 ? ATREE_SLAB_MIN_CAP : *cap * 2;
            void *seg;
            while (size < need && size < ATREE_SLAB_SEG) {
                size *= 2;
            }
            if (size > ATREE_SLAB_SEG) {
                size = ATREE_SLAB_SEG;
            }
            if (*segcap == 0) {
                void **table = atree__alloc_array(m, 4, sizeof *table);
                if (table == NULL) {
                    return ATREE_ERR_NOMEM;
                }
                *segs = table;
                *segcap = 4;
            }
            if (*nsegs == 0) {
                seg = atree__alloc_array(m, size, elem);
            } else {
                seg = atree__realloc_array(m, (*segs)[0], *cap, size, elem);
            }
            if (seg == NULL) {
                return ATREE_ERR_NOMEM;
            }
            (*segs)[0] = seg;
            *nsegs = 1;
            *cap = size;
        } else {
            uint64_t next = (uint64_t)*cap + ATREE_SLAB_SEG;
            void *seg;
            if (*nsegs == *segcap) {
                uint32_t ncap = *segcap * 2;
                void **table = atree__realloc_array(m, *segs, *segcap, ncap, sizeof *table);
                if (table == NULL) {
                    return ATREE_ERR_NOMEM;
                }
                *segs = table;
                *segcap = ncap;
            }
            seg = atree__alloc_array(m, ATREE_SLAB_SEG, elem);
            if (seg == NULL) {
                return ATREE_ERR_NOMEM;
            }
            (*segs)[*nsegs] = seg;
            (*nsegs)++;
            /* Ids stop at UINT32_MAX - 1 (ATREE_NID_NONE), so the last
             * segment is addressable even though it cannot be counted. */
            *cap = next > UINT32_MAX ? UINT32_MAX : (uint32_t)next;
        }
    }
    return ATREE_OK;
}

void atree__slab_free(struct atree__mem *m, void **segs, uint32_t nsegs, uint32_t segcap,
                      uint32_t cap, size_t elem)
{
    uint32_t k;
    for (k = 0; k < nsegs; k++) {
        atree__free_array(m, segs[k], seg_elems(k, nsegs, cap), elem);
    }
    if (segcap != 0) {
        atree__free_array(m, segs, segcap, sizeof *segs);
    }
}

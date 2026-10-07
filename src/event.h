/*
 * Event layout, shared with the search module.
 *
 * An event is a dense array of values indexed by attribute id. List values
 * point into per-attribute buffers owned by the event and kept across
 * atree_event_clear() so steady-state use allocates nothing. The event has
 * its own allocator counters so creating or growing one never writes to
 * the tree (see atree_internal.h).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_EVENT_H
#define ATREE_EVENT_H

#include <stdint.h>

#include "atree.h"

#include "alloc.h"
#include "value.h"

struct atree__listbuf {
    union {
        int64_t *i;
        uint32_t *s;
    } data;
    uint32_t cap; /* elements */
};

struct atree_event {
    const atree_t *tree;
    struct atree__mem mem;
    uint32_t nattrs;
    struct atree__value *values; /* nattrs */
    struct atree__listbuf *bufs; /* nattrs; unused slots stay zero */
    /* The ids whose value is defined, in no order (an id is listed iff its
     * kind is not ATREE_V_UNDEFINED): phase 1 visits these instead of
     * every attribute of the schema, and clear resets only these. */
    uint32_t *defined; /* nattrs */
    uint32_t ndefined;
};

/* id must be valid. */
static inline const struct atree__value *atree__event_value(const atree_event_t *ev,
                                                            atree_attr_id_t id)
{
    return &ev->values[id];
}

#endif /* ATREE_EVENT_H */

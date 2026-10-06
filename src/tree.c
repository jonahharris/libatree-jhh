/*
 * Tree lifecycle, configuration, attribute queries, statistics.
 *
 * M1 scope: create/destroy with the attribute and string tables. The DAG
 * (nodes, identity table, subscriptions, indexes) arrives in M4.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "atree_internal.h"

#define ATREE_FLAG_ALL                                                                             \
    (ATREE_FLAG_NO_REORGANIZE | ATREE_FLAG_NO_SELF_ADJUST | ATREE_FLAG_NO_PROPAGATION_ON_DEMAND |  \
     ATREE_FLAG_NO_PREDICATE_INDEX)

static atree_status_t check_config(const atree_config_t *cfg)
{
    if (cfg == NULL) {
        return ATREE_OK;
    }
    if ((cfg->flags & ~(unsigned)ATREE_FLAG_ALL) != 0) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (cfg->allocator != NULL &&
        (cfg->allocator->alloc == NULL || cfg->allocator->realloc == NULL ||
         cfg->allocator->free == NULL)) {
        return ATREE_ERR_INVALID_ARG;
    }
    if (cfg->lock != NULL &&
        (cfg->lock->rdlock == NULL || cfg->lock->rdunlock == NULL || cfg->lock->wrlock == NULL ||
         cfg->lock->wrunlock == NULL)) {
        return ATREE_ERR_INVALID_ARG;
    }
    return ATREE_OK;
}

atree_status_t atree_create(const atree_config_t *cfg, const atree_attr_def_t *attrs, size_t nattrs,
                            atree_t **out)
{
    struct atree__mem mem;
    atree_t *t;
    atree_status_t st;

    if (out == NULL) {
        return ATREE_ERR_INVALID_ARG;
    }
    *out = NULL;
    st = check_config(cfg);
    if (st != ATREE_OK) {
        return st;
    }
    atree__mem_init(&mem, cfg != NULL ? cfg->allocator : NULL);
    t = atree__zalloc(&mem, sizeof *t);
    if (t == NULL) {
        return ATREE_ERR_NOMEM;
    }
    t->mem = mem;
    if (cfg != NULL) {
        if (cfg->lock != NULL) {
            t->lock = *cfg->lock;
            t->has_lock = true;
        }
        t->flags = cfg->flags;
        t->max_depth = cfg->max_depth != 0 ? cfg->max_depth : ATREE_DEFAULT_MAX_DEPTH;
        t->max_adjust_candidates = cfg->max_adjust_candidates != 0
            ? cfg->max_adjust_candidates
            : ATREE_DEFAULT_MAX_ADJUST_CANDIDATES;
        t->initial_nodes =
            cfg->initial_nodes != 0 ? cfg->initial_nodes : ATREE_DEFAULT_INITIAL_NODES;
    } else {
        t->max_depth = ATREE_DEFAULT_MAX_DEPTH;
        t->max_adjust_candidates = ATREE_DEFAULT_MAX_ADJUST_CANDIDATES;
        t->initial_nodes = ATREE_DEFAULT_INITIAL_NODES;
    }

    st = atree__attrs_init(&t->mem, &t->attrs, attrs, nattrs);
    if (st != ATREE_OK) {
        mem = t->mem;
        atree__free(&mem, t, sizeof *t);
        return st;
    }
    st = atree__strtab_init(&t->mem, &t->strings);
    if (st != ATREE_OK) {
        atree__attrs_free(&t->mem, &t->attrs);
        mem = t->mem;
        atree__free(&mem, t, sizeof *t);
        return st;
    }
    *out = t;
    return ATREE_OK;
}

void atree_destroy(atree_t *t)
{
    struct atree__mem mem;
    if (t == NULL) {
        return;
    }
    atree__strtab_free(&t->mem, &t->strings);
    atree__attrs_free(&t->mem, &t->attrs);
    mem = t->mem;
    atree__free(&mem, t, sizeof *t);
}

/* ---- attributes --------------------------------------------------------- */

atree_attr_id_t atree_attr_lookup(const atree_t *t, const char *name)
{
    atree_attr_id_t id;
    if (t == NULL || name == NULL) {
        return ATREE_ATTR_INVALID;
    }
    atree__rdlock(t);
    id = atree__attrs_lookup(&t->attrs, name, strlen(name));
    atree__rdunlock(t);
    return id;
}

size_t atree_attr_count(const atree_t *t)
{
    return t == NULL ? 0 : atree__attrs_count(&t->attrs);
}

const char *atree_attr_name(const atree_t *t, atree_attr_id_t id)
{
    if (t == NULL || id >= atree__attrs_count(&t->attrs)) {
        return NULL;
    }
    return atree__attrs_get(&t->attrs, id)->name;
}

atree_type_t atree_attr_type(const atree_t *t, atree_attr_id_t id)
{
    if (t == NULL || id >= atree__attrs_count(&t->attrs)) {
        return ATREE_TYPE_BOOL;
    }
    return atree__attrs_get(&t->attrs, id)->type;
}

/* ---- stats -------------------------------------------------------------- */

void atree_stats(const atree_t *t, atree_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (t == NULL) {
        return;
    }
    atree__rdlock(t);
    out->strings = atree__strtab_count(&t->strings);
    out->bytes_allocated = t->mem.live;
    out->bytes_peak = t->mem.peak;
    atree__rdunlock(t);
}

/*
 * Ready-made atree_lock_t adapter over pthread_rwlock_t. Header-only and
 * outside the library proper so libatree itself stays free of platform
 * headers. Include from one translation unit of the application.
 *
 *   struct atree_pthread_lock lk;
 *   atree_pthread_lock_init(&lk);
 *   cfg.lock = atree_pthread_lock_vtable(&lk);
 *   ... atree_create(&cfg, ...) ...
 *   atree_pthread_lock_destroy(&lk);   // after atree_destroy
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_LOCK_PTHREAD_H
#define ATREE_LOCK_PTHREAD_H

#include <pthread.h>

#include "atree.h"

struct atree_pthread_lock {
    pthread_rwlock_t rw;
    atree_lock_t vt;
};

static void atree_pthread_lock_rd(void *ctx)
{
    (void)pthread_rwlock_rdlock(&((struct atree_pthread_lock *)ctx)->rw);
}

static void atree_pthread_lock_rdun(void *ctx)
{
    (void)pthread_rwlock_unlock(&((struct atree_pthread_lock *)ctx)->rw);
}

static void atree_pthread_lock_wr(void *ctx)
{
    (void)pthread_rwlock_wrlock(&((struct atree_pthread_lock *)ctx)->rw);
}

static void atree_pthread_lock_wrun(void *ctx)
{
    (void)pthread_rwlock_unlock(&((struct atree_pthread_lock *)ctx)->rw);
}

/* Returns 0 on success, otherwise the pthread error code. */
static int atree_pthread_lock_init(struct atree_pthread_lock *l)
{
    int rc = pthread_rwlock_init(&l->rw, NULL);
    l->vt.rdlock = atree_pthread_lock_rd;
    l->vt.rdunlock = atree_pthread_lock_rdun;
    l->vt.wrlock = atree_pthread_lock_wr;
    l->vt.wrunlock = atree_pthread_lock_wrun;
    l->vt.ctx = l;
    return rc;
}

static const atree_lock_t *atree_pthread_lock_vtable(struct atree_pthread_lock *l)
{
    return &l->vt;
}

static int atree_pthread_lock_destroy(struct atree_pthread_lock *l)
{
    return pthread_rwlock_destroy(&l->rw);
}

#endif /* ATREE_LOCK_PTHREAD_H */

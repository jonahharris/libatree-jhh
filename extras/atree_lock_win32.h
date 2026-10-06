/*
 * Ready-made atree_lock_t adapter over a Win32 slim reader/writer lock
 * (SRWLOCK). Header-only; include from one translation unit.
 *
 *   struct atree_win32_lock lk;
 *   atree_win32_lock_init(&lk);
 *   cfg.lock = atree_win32_lock_vtable(&lk);
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_LOCK_WIN32_H
#define ATREE_LOCK_WIN32_H

#include <windows.h>

#include "atree.h"

struct atree_win32_lock {
    SRWLOCK rw;
    atree_lock_t vt;
};

static void atree_win32_lock_rd(void *ctx)
{
    AcquireSRWLockShared(&((struct atree_win32_lock *)ctx)->rw);
}

static void atree_win32_lock_rdun(void *ctx)
{
    ReleaseSRWLockShared(&((struct atree_win32_lock *)ctx)->rw);
}

static void atree_win32_lock_wr(void *ctx)
{
    AcquireSRWLockExclusive(&((struct atree_win32_lock *)ctx)->rw);
}

static void atree_win32_lock_wrun(void *ctx)
{
    ReleaseSRWLockExclusive(&((struct atree_win32_lock *)ctx)->rw);
}

static int atree_win32_lock_init(struct atree_win32_lock *l)
{
    InitializeSRWLock(&l->rw);
    l->vt.rdlock = atree_win32_lock_rd;
    l->vt.rdunlock = atree_win32_lock_rdun;
    l->vt.wrlock = atree_win32_lock_wr;
    l->vt.wrunlock = atree_win32_lock_wrun;
    l->vt.ctx = l;
    return 0;
}

static const atree_lock_t *atree_win32_lock_vtable(struct atree_win32_lock *l)
{
    return &l->vt;
}

static int atree_win32_lock_destroy(struct atree_win32_lock *l)
{
    (void)l; /* SRWLOCKs need no destruction */
    return 0;
}

#endif /* ATREE_LOCK_WIN32_H */

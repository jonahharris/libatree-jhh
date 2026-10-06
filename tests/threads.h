/*
 * Tiny portable thread shim for tests only (pthreads or Win32). The library
 * itself has no threading dependency.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_TEST_THREADS_H
#define ATREE_TEST_THREADS_H

typedef void *(*test_thread_fn)(void *arg);

#if defined(_WIN32)
#include <windows.h>

typedef struct test_thread {
    HANDLE h;
    test_thread_fn fn;
    void *arg;
} test_thread_t;

typedef struct test_mutex {
    CRITICAL_SECTION cs;
} test_mutex_t;

static DWORD WINAPI test_thread_tramp(LPVOID p)
{
    test_thread_t *t = (test_thread_t *)p;
    (void)t->fn(t->arg);
    return 0;
}

static int test_thread_start(test_thread_t *t, test_thread_fn fn, void *arg)
{
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, 0, test_thread_tramp, t, 0, NULL);
    return t->h == NULL ? 1 : 0;
}

static void test_thread_join(test_thread_t *t)
{
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
}

static void test_mutex_init(test_mutex_t *m)
{
    InitializeCriticalSection(&m->cs);
}
static void test_mutex_lock(test_mutex_t *m)
{
    EnterCriticalSection(&m->cs);
}
static void test_mutex_unlock(test_mutex_t *m)
{
    LeaveCriticalSection(&m->cs);
}
static void test_mutex_destroy(test_mutex_t *m)
{
    DeleteCriticalSection(&m->cs);
}
static void test_sleep_ms(unsigned ms)
{
    Sleep(ms);
}

#else
#include <pthread.h>
#include <time.h>

typedef struct test_thread {
    pthread_t h;
} test_thread_t;

typedef struct test_mutex {
    pthread_mutex_t m;
} test_mutex_t;

static int test_thread_start(test_thread_t *t, test_thread_fn fn, void *arg)
{
    return pthread_create(&t->h, NULL, fn, arg);
}

static void test_thread_join(test_thread_t *t)
{
    (void)pthread_join(t->h, NULL);
}

static void test_mutex_init(test_mutex_t *m)
{
    (void)pthread_mutex_init(&m->m, NULL);
}
static void test_mutex_lock(test_mutex_t *m)
{
    (void)pthread_mutex_lock(&m->m);
}
static void test_mutex_unlock(test_mutex_t *m)
{
    (void)pthread_mutex_unlock(&m->m);
}
static void test_mutex_destroy(test_mutex_t *m)
{
    (void)pthread_mutex_destroy(&m->m);
}
static void test_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)nanosleep(&ts, NULL);
}
#endif

#endif /* ATREE_TEST_THREADS_H */

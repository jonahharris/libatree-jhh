/*
 * Monotonic clock for benchmarks only (the library has no clock dependency).
 * Include this before any other header so the feature macro takes effect.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_BENCH_CLOCK_H
#define ATREE_BENCH_CLOCK_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
static uint64_t bench_now_ns(void)
{
    LARGE_INTEGER f;
    LARGE_INTEGER c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#include <time.h>
static uint64_t bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
#endif

#endif /* ATREE_BENCH_CLOCK_H */

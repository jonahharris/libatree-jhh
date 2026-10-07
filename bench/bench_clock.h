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
#elif defined(__APPLE__)
/* clock_gettime(CLOCK_MONOTONIC) has microsecond resolution on macOS (and
 * CLOCK_MONOTONIC_RAW returns unscaled ticks on Apple silicon), so the
 * per-insert and small-search timings use the scaled mach clock. */
#include <mach/mach_time.h>
static uint64_t bench_now_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
    }
    return mach_absolute_time() * tb.numer / tb.denom;
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

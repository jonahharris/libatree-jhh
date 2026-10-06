/*
 * Minimal test harness (minunit style). Independent of NDEBUG. Each test is
 * a function returning 0 on success; assertions print file:line and return 1.
 *
 * Usage:
 *   TEST(my_test) { ASSERT_TRUE(1 + 1 == 2); return 0; }
 *   TEST_MAIN_BEGIN()
 *       RUN_TEST(my_test);
 *   TEST_MAIN_END()
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_TEST_H
#define ATREE_TEST_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atree.h"

static int atree_tests_run = 0;
static int atree_tests_failed = 0;

#define TEST(name) static int name(void)

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #cond);           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ_U64(a, b)                                                                        \
    do {                                                                                           \
        uint64_t atree_a_ = (uint64_t)(a);                                                         \
        uint64_t atree_b_ = (uint64_t)(b);                                                         \
        if (atree_a_ != atree_b_) {                                                                \
            fprintf(stderr, "%s:%d: %s == %s failed: %" PRIu64 " != %" PRIu64 "\n", __FILE__,      \
                    __LINE__, #a, #b, atree_a_, atree_b_);                                         \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ_I64(a, b)                                                                        \
    do {                                                                                           \
        int64_t atree_a_ = (int64_t)(a);                                                           \
        int64_t atree_b_ = (int64_t)(b);                                                           \
        if (atree_a_ != atree_b_) {                                                                \
            fprintf(stderr, "%s:%d: %s == %s failed: %" PRId64 " != %" PRId64 "\n", __FILE__,      \
                    __LINE__, #a, #b, atree_a_, atree_b_);                                         \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ_PTR(a, b)                                                                        \
    do {                                                                                           \
        const void *atree_a_ = (const void *)(a);                                                  \
        const void *atree_b_ = (const void *)(b);                                                  \
        if (atree_a_ != atree_b_) {                                                                \
            fprintf(stderr, "%s:%d: %s == %s failed: %p != %p\n", __FILE__, __LINE__, #a, #b,      \
                    atree_a_, atree_b_);                                                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)

#define ASSERT_EQ_STR(a, b)                                                                        \
    do {                                                                                           \
        const char *atree_a_ = (a);                                                                \
        const char *atree_b_ = (b);                                                                \
        if (atree_a_ == NULL || atree_b_ == NULL || strcmp(atree_a_, atree_b_) != 0) {             \
            fprintf(stderr, "%s:%d: %s == %s failed: \"%s\" != \"%s\"\n", __FILE__, __LINE__, #a,  \
                    #b, atree_a_ ? atree_a_ : "(null)", atree_b_ ? atree_b_ : "(null)");           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_STATUS(expr, expected)                                                              \
    do {                                                                                           \
        atree_status_t atree_st_ = (expr);                                                         \
        atree_status_t atree_ex_ = (expected);                                                     \
        if (atree_st_ != atree_ex_) {                                                              \
            fprintf(stderr, "%s:%d: %s returned %d (%s), expected %d (%s)\n", __FILE__, __LINE__,  \
                    #expr, (int)atree_st_, atree_strerror(atree_st_), (int)atree_ex_,              \
                    atree_strerror(atree_ex_));                                                    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_OK(expr) ASSERT_STATUS(expr, ATREE_OK)

#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        int atree_r_ = name();                                                                     \
        atree_tests_run++;                                                                         \
        if (atree_r_ != 0) {                                                                       \
            atree_tests_failed++;                                                                  \
            fprintf(stderr, "FAILED: %s\n", #name);                                                \
        }                                                                                          \
    } while (0)

#define TEST_MAIN_BEGIN()                                                                          \
    int main(void)                                                                                 \
    {
#define TEST_MAIN_END()                                                                            \
    fprintf(stderr, "%s: %d tests, %d failed\n", __FILE__, atree_tests_run, atree_tests_failed);   \
    return atree_tests_failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;                                  \
    }

#endif /* ATREE_TEST_H */

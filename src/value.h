/*
 * Attribute values (in events) and literal operands (in predicates), plus the
 * sorted-list and double helpers both sides share.
 *
 * Lists are always sorted ascending and free of duplicates, so membership
 * is a binary search and list-vs-list operators are merge walks. Strings
 * are string-table ids (ATREE_STR_UNKNOWN for strings the tree never saw).
 *
 * Doubles: exact equality lives in atree__double_eq() only (-Wfloat-equal is
 * on); -0.0 and 0.0 hash alike; NaN never equals anything and is rejected
 * from literals and stored as undefined in events.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_VALUE_H
#define ATREE_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alloc.h"

enum atree__vkind {
    ATREE_V_UNDEFINED = 0,
    ATREE_V_BOOL,
    ATREE_V_INT,
    ATREE_V_FLOAT,
    ATREE_V_STRING,
    ATREE_V_INT_LIST,
    ATREE_V_STRING_LIST
};

struct atree__value {
    uint8_t kind; /* enum atree__vkind */
    union {
        bool b;
        int64_t i;
        double f;
        uint32_t s; /* string id */
        struct {
            int64_t *data; /* sorted, unique */
            uint32_t len;
        } il;
        struct {
            uint32_t *data; /* sorted, unique string ids */
            uint32_t len;
        } sl;
    } u;
};

/* ---- doubles ------------------------------------------------------------ */

bool atree__double_is_nan(double d);
bool atree__double_is_finite(double d);
/* Exact equality; false if either is NaN; -0.0 == 0.0. */
bool atree__double_eq(double a, double b);
/* Bit pattern with -0.0 canonicalized to 0.0 (for hashing). */
uint64_t atree__double_bits(double d);
/* -1, 0, 1 for a < b, a == b, a > b. Neither may be NaN. */
int atree__double_cmp(double a, double b);
/* Converts an integer literal for use against a float attribute; false if
 * the integer is not exactly representable (|i| > 2^53). */
bool atree__int_to_double_exact(int64_t i, double *out);

/* ---- sorted lists ------------------------------------------------------- */

/* Sorts in place and removes duplicates; returns the new length. */
uint32_t atree__sort_unique_i64(int64_t *v, uint32_t n);
uint32_t atree__sort_unique_u32(uint32_t *v, uint32_t n);

bool atree__bsearch_i64(const int64_t *v, uint32_t n, int64_t x);
bool atree__bsearch_u32(const uint32_t *v, uint32_t n, uint32_t x);

/* Any element in common (both sorted unique). */
bool atree__intersects_i64(const int64_t *a, uint32_t na, const int64_t *b, uint32_t nb);
bool atree__intersects_u32(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb);

/* Every element of needles is in hay (both sorted unique). Empty needles: true. */
bool atree__contains_all_i64(const int64_t *hay, uint32_t nh, const int64_t *needles, uint32_t nn);
bool atree__contains_all_u32(const uint32_t *hay, uint32_t nh, const uint32_t *needles,
                             uint32_t nn);

/* ---- values ------------------------------------------------------------- */

/* Structural equality of literal operands (lists compared elementwise). */
bool atree__value_equal(const struct atree__value *a, const struct atree__value *b);

/* Deep copy of a list-bearing value (scalars are copied by assignment). The
 * copy owns its list storage; atree__value_free releases it. */
atree_status_t atree__value_copy(struct atree__mem *m, struct atree__value *dst,
                                 const struct atree__value *src);
void atree__value_free(struct atree__mem *m, struct atree__value *v);

#endif /* ATREE_VALUE_H */

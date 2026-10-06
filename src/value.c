/*
 * Values, sorted-list operations, double helpers.
 *
 * SPDX-License-Identifier: MIT
 */
#include "value.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "hash.h"

ATREE_STATIC_ASSERT(sizeof(double) == 8, double_is_64_bit);

/* ---- doubles ------------------------------------------------------------ */

bool atree__double_is_nan(double d)
{
    return isnan(d) != 0;
}

bool atree__double_is_finite(double d)
{
    return isnan(d) == 0 && isinf(d) == 0;
}

bool atree__double_eq(double a, double b)
{
    if (isnan(a) || isnan(b)) {
        return false;
    }
    /* Exact equality without the == operator: for non-NaN operands
     * a <= b && a >= b is equivalent, and -0.0 compares equal to 0.0. */
    return a <= b && a >= b;
}

uint64_t atree__double_bits(double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    if ((bits & ~(UINT64_C(1) << 63)) == 0) {
        bits = 0; /* -0.0 -> 0.0 */
    }
    return bits;
}

int atree__double_cmp(double a, double b)
{
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

bool atree__int_to_double_exact(int64_t i, double *out)
{
    const int64_t limit = (int64_t)1 << 53;
    if (i > limit || i < -limit) {
        return false;
    }
    *out = (double)i;
    return true;
}

/* ---- sorting ------------------------------------------------------------ */

static int cmp_i64(const void *pa, const void *pb)
{
    int64_t a = *(const int64_t *)pa;
    int64_t b = *(const int64_t *)pb;
    return (a > b) - (a < b);
}

static int cmp_u32(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;
    return (a > b) - (a < b);
}

uint32_t atree__sort_unique_i64(int64_t *v, uint32_t n)
{
    uint32_t i;
    uint32_t m = 0;
    if (n < 2) {
        return n;
    }
    qsort(v, n, sizeof *v, cmp_i64);
    for (i = 0; i < n; i++) {
        if (m == 0 || v[m - 1] != v[i]) {
            v[m++] = v[i];
        }
    }
    return m;
}

uint32_t atree__sort_unique_u32(uint32_t *v, uint32_t n)
{
    uint32_t i;
    uint32_t m = 0;
    if (n < 2) {
        return n;
    }
    qsort(v, n, sizeof *v, cmp_u32);
    for (i = 0; i < n; i++) {
        if (m == 0 || v[m - 1] != v[i]) {
            v[m++] = v[i];
        }
    }
    return m;
}

/* ---- membership --------------------------------------------------------- */

bool atree__bsearch_i64(const int64_t *v, uint32_t n, int64_t x)
{
    uint32_t lo = 0;
    uint32_t hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (v[mid] < x) {
            lo = mid + 1;
        } else if (v[mid] > x) {
            hi = mid;
        } else {
            return true;
        }
    }
    return false;
}

bool atree__bsearch_u32(const uint32_t *v, uint32_t n, uint32_t x)
{
    uint32_t lo = 0;
    uint32_t hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (v[mid] < x) {
            lo = mid + 1;
        } else if (v[mid] > x) {
            hi = mid;
        } else {
            return true;
        }
    }
    return false;
}

bool atree__intersects_i64(const int64_t *a, uint32_t na, const int64_t *b, uint32_t nb)
{
    uint32_t i = 0;
    uint32_t j = 0;
    while (i < na && j < nb) {
        if (a[i] < b[j]) {
            i++;
        } else if (a[i] > b[j]) {
            j++;
        } else {
            return true;
        }
    }
    return false;
}

bool atree__intersects_u32(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb)
{
    uint32_t i = 0;
    uint32_t j = 0;
    while (i < na && j < nb) {
        if (a[i] < b[j]) {
            i++;
        } else if (a[i] > b[j]) {
            j++;
        } else {
            return true;
        }
    }
    return false;
}

bool atree__contains_all_i64(const int64_t *hay, uint32_t nh, const int64_t *needles, uint32_t nn)
{
    uint32_t i = 0;
    uint32_t j = 0;
    if (nn > nh) {
        return false;
    }
    while (i < nh && j < nn) {
        if (hay[i] < needles[j]) {
            i++;
        } else if (hay[i] > needles[j]) {
            return false; /* needles[j] is absent */
        } else {
            i++;
            j++;
        }
    }
    return j == nn;
}

bool atree__contains_all_u32(const uint32_t *hay, uint32_t nh, const uint32_t *needles, uint32_t nn)
{
    uint32_t i = 0;
    uint32_t j = 0;
    if (nn > nh) {
        return false;
    }
    while (i < nh && j < nn) {
        if (hay[i] < needles[j]) {
            i++;
        } else if (hay[i] > needles[j]) {
            return false;
        } else {
            i++;
            j++;
        }
    }
    return j == nn;
}

/* ---- values ------------------------------------------------------------- */

uint64_t atree__value_hash(const struct atree__value *v)
{
    uint64_t h = atree__hash_u64((uint64_t)v->kind + UINT64_C(0x51ed270b));
    uint32_t i;
    switch (v->kind) {
    case ATREE_V_UNDEFINED:
        break;
    case ATREE_V_BOOL:
        h = atree__hash_combine(h, v->u.b ? 1 : 0);
        break;
    case ATREE_V_INT:
        h = atree__hash_combine(h, (uint64_t)v->u.i);
        break;
    case ATREE_V_FLOAT:
        h = atree__hash_combine(h, atree__double_bits(v->u.f));
        break;
    case ATREE_V_STRING:
        h = atree__hash_combine(h, v->u.s);
        break;
    case ATREE_V_INT_LIST:
        h = atree__hash_combine(h, v->u.il.len);
        for (i = 0; i < v->u.il.len; i++) {
            h = atree__hash_combine(h, (uint64_t)v->u.il.data[i]);
        }
        break;
    case ATREE_V_STRING_LIST:
        h = atree__hash_combine(h, v->u.sl.len);
        for (i = 0; i < v->u.sl.len; i++) {
            h = atree__hash_combine(h, v->u.sl.data[i]);
        }
        break;
    default:
        break;
    }
    return h;
}

bool atree__value_equal(const struct atree__value *a, const struct atree__value *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case ATREE_V_UNDEFINED:
        return true;
    case ATREE_V_BOOL:
        return a->u.b == b->u.b;
    case ATREE_V_INT:
        return a->u.i == b->u.i;
    case ATREE_V_FLOAT:
        return atree__double_eq(a->u.f, b->u.f);
    case ATREE_V_STRING:
        return a->u.s == b->u.s;
    case ATREE_V_INT_LIST:
        return a->u.il.len == b->u.il.len &&
            (a->u.il.len == 0 ||
             memcmp(a->u.il.data, b->u.il.data, (size_t)a->u.il.len * sizeof(int64_t)) == 0);
    case ATREE_V_STRING_LIST:
        return a->u.sl.len == b->u.sl.len &&
            (a->u.sl.len == 0 ||
             memcmp(a->u.sl.data, b->u.sl.data, (size_t)a->u.sl.len * sizeof(uint32_t)) == 0);
    default:
        return false;
    }
}

atree_status_t atree__value_copy(struct atree__mem *m, struct atree__value *dst,
                                 const struct atree__value *src)
{
    *dst = *src;
    if (src->kind == ATREE_V_INT_LIST && src->u.il.len > 0) {
        dst->u.il.data = atree__alloc_array(m, src->u.il.len, sizeof(int64_t));
        if (dst->u.il.data == NULL) {
            return ATREE_ERR_NOMEM;
        }
        memcpy(dst->u.il.data, src->u.il.data, (size_t)src->u.il.len * sizeof(int64_t));
    } else if (src->kind == ATREE_V_STRING_LIST && src->u.sl.len > 0) {
        dst->u.sl.data = atree__alloc_array(m, src->u.sl.len, sizeof(uint32_t));
        if (dst->u.sl.data == NULL) {
            return ATREE_ERR_NOMEM;
        }
        memcpy(dst->u.sl.data, src->u.sl.data, (size_t)src->u.sl.len * sizeof(uint32_t));
    } else if (src->kind == ATREE_V_INT_LIST) {
        dst->u.il.data = NULL;
    } else if (src->kind == ATREE_V_STRING_LIST) {
        dst->u.sl.data = NULL;
    }
    return ATREE_OK;
}

void atree__value_free(struct atree__mem *m, struct atree__value *v)
{
    if (v->kind == ATREE_V_INT_LIST) {
        atree__free_array(m, v->u.il.data, v->u.il.len, sizeof(int64_t));
        v->u.il.data = NULL;
        v->u.il.len = 0;
    } else if (v->kind == ATREE_V_STRING_LIST) {
        atree__free_array(m, v->u.sl.data, v->u.sl.len, sizeof(uint32_t));
        v->u.sl.data = NULL;
        v->u.sl.len = 0;
    }
    v->kind = ATREE_V_UNDEFINED;
}

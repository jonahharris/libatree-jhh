/*
 * Minimal JSON object reader (see json.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "json.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct jp {
    const char *p;
    const char *e;
    char *out; /* next free byte of the scratch buffer */
    const char *start;
};

static void skip_ws(struct jp *j)
{
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

static int put_utf8(struct jp *j, uint32_t cp)
{
    if (cp < 0x80) {
        *j->out++ = (char)cp;
    } else if (cp < 0x800) {
        *j->out++ = (char)(0xC0 | (cp >> 6));
        *j->out++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *j->out++ = (char)(0xE0 | (cp >> 12));
        *j->out++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *j->out++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *j->out++ = (char)(0xF0 | (cp >> 18));
        *j->out++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *j->out++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *j->out++ = (char)(0x80 | (cp & 0x3F));
    }
    return 0;
}

static int hex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (uint32_t)(c - 'A' + 10);
        } else {
            return -1;
        }
    }
    *out = v;
    return 0;
}

/* Parses a string at j->p (opening quote) into the scratch; sets *s and *len. */
static int parse_string(struct jp *j, const char **s, size_t *len)
{
    char *begin = j->out;
    if (j->p >= j->e || *j->p != '"') {
        return -1;
    }
    j->p++;
    while (j->p < j->e && *j->p != '"') {
        char c = *j->p++;
        if ((unsigned char)c < 0x20) {
            return -1;
        }
        if (c != '\\') {
            *j->out++ = c;
            continue;
        }
        if (j->p >= j->e) {
            return -1;
        }
        c = *j->p++;
        switch (c) {
        case '"':
        case '\\':
        case '/':
            *j->out++ = c;
            break;
        case 'b':
            *j->out++ = '\b';
            break;
        case 'f':
            *j->out++ = '\f';
            break;
        case 'n':
            *j->out++ = '\n';
            break;
        case 'r':
            *j->out++ = '\r';
            break;
        case 't':
            *j->out++ = '\t';
            break;
        case 'u': {
            uint32_t cp;
            if (j->e - j->p < 4 || hex4(j->p, &cp) != 0) {
                return -1;
            }
            j->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo;
                if (j->e - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u' ||
                    hex4(j->p + 2, &lo) != 0 || lo < 0xDC00 || lo > 0xDFFF) {
                    return -1;
                }
                j->p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return -1;
            }
            put_utf8(j, cp);
            break;
        }
        default:
            return -1;
        }
    }
    if (j->p >= j->e) {
        return -1;
    }
    j->p++; /* closing quote */
    *j->out++ = '\0';
    *s = begin;
    *len = (size_t)(j->out - begin - 1);
    return 0;
}

static int parse_number(struct jp *j, struct json_value *v)
{
    const char *start = j->p;
    bool is_float = false;
    char *end = NULL;
    char tmp[64];
    size_t n;
    if (j->p < j->e && *j->p == '-') {
        j->p++;
    }
    while (j->p < j->e &&
           ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' || *j->p == 'e' || *j->p == 'E' ||
            *j->p == '+' || *j->p == '-')) {
        if (*j->p == '.' || *j->p == 'e' || *j->p == 'E') {
            is_float = true;
        }
        j->p++;
    }
    n = (size_t)(j->p - start);
    if (n == 0 || n >= sizeof tmp) {
        return -1;
    }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    errno = 0;
    if (!is_float) {
        long long ll = strtoll(tmp, &end, 10);
        if (end == tmp || *end != '\0') {
            return -1;
        }
        if (errno == ERANGE) {
            is_float = true; /* out of int64 range: carry it as a float */
        } else {
            v->kind = JSON_INT;
            v->i = ll;
            return 0;
        }
    }
    errno = 0;
    v->f = strtod(tmp, &end);
    if (end == tmp || *end != '\0' || errno == ERANGE || isnan(v->f) || isinf(v->f)) {
        return -1;
    }
    v->kind = JSON_FLOAT;
    return 0;
}

static int parse_literal(struct jp *j, const char *lit, size_t n)
{
    if ((size_t)(j->e - j->p) < n || memcmp(j->p, lit, n) != 0) {
        return -1;
    }
    j->p += n;
    return 0;
}

static int parse_value(struct jp *j, struct json_value *v)
{
    skip_ws(j);
    if (j->p >= j->e) {
        return -1;
    }
    memset(v, 0, sizeof *v);
    switch (*j->p) {
    case '"':
        v->kind = JSON_STRING;
        return parse_string(j, &v->s, &v->slen);
    case 't':
        v->kind = JSON_BOOL;
        v->b = true;
        return parse_literal(j, "true", 4);
    case 'f':
        v->kind = JSON_BOOL;
        v->b = false;
        return parse_literal(j, "false", 5);
    case 'n':
        v->kind = JSON_NULL;
        return parse_literal(j, "null", 4);
    case '[': {
        v->kind = JSON_ARRAY;
        j->p++;
        skip_ws(j);
        if (j->p < j->e && *j->p == ']') {
            j->p++;
            return 0;
        }
        for (;;) {
            struct json_value item;
            if (v->nitems == JSON_MAX_ITEMS) {
                return -1;
            }
            skip_ws(j);
            if (j->p >= j->e) {
                return -1;
            }
            if (*j->p == '"') {
                item.kind = JSON_STRING;
                if (parse_string(j, &item.s, &item.slen) != 0) {
                    return -1;
                }
            } else if (parse_number(j, &item) != 0 || item.kind != JSON_INT) {
                return -1;
            }
            if (v->nitems == 0) {
                v->items_are_strings = item.kind == JSON_STRING;
            } else if (v->items_are_strings != (item.kind == JSON_STRING)) {
                return -1; /* mixed arrays are not events */
            }
            if (item.kind == JSON_STRING) {
                v->strs[v->nitems] = item.s;
                v->slens[v->nitems] = item.slen;
            } else {
                v->ints[v->nitems] = item.i;
            }
            v->nitems++;
            skip_ws(j);
            if (j->p < j->e && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->e && *j->p == ']') {
                j->p++;
                return 0;
            }
            return -1;
        }
    }
    default:
        return parse_number(j, v);
    }
}

int json_parse_object(const char *text, size_t len, char *scratch, json_member_fn fn, void *ctx,
                      size_t *errpos)
{
    struct jp j;
    j.p = text;
    j.e = text + len;
    j.out = scratch;
    j.start = text;
    *errpos = 0;
    skip_ws(&j);
    if (j.p >= j.e || *j.p != '{') {
        goto bad;
    }
    j.p++;
    skip_ws(&j);
    if (j.p < j.e && *j.p == '}') {
        j.p++;
        goto tail;
    }
    for (;;) {
        const char *key;
        size_t keylen;
        struct json_value v;
        int rc;
        skip_ws(&j);
        if (parse_string(&j, &key, &keylen) != 0) {
            goto bad;
        }
        skip_ws(&j);
        if (j.p >= j.e || *j.p != ':') {
            goto bad;
        }
        j.p++;
        if (parse_value(&j, &v) != 0) {
            goto bad;
        }
        rc = fn(ctx, key, keylen, &v);
        if (rc != 0) {
            *errpos = (size_t)(j.p - j.start);
            return rc;
        }
        skip_ws(&j);
        if (j.p < j.e && *j.p == ',') {
            j.p++;
            continue;
        }
        if (j.p < j.e && *j.p == '}') {
            j.p++;
            break;
        }
        goto bad;
    }
tail:
    skip_ws(&j);
    if (j.p != j.e) {
        goto bad;
    }
    return 0;
bad:
    *errpos = (size_t)(j.p - j.start);
    return -1;
}

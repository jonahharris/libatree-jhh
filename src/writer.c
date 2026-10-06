/*
 * Output helper.
 *
 * SPDX-License-Identifier: MIT
 */
#include "writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "value.h"

void atree__writer_init(struct atree__writer *w, atree_write_fn fn, void *ctx)
{
    w->fn = fn;
    w->ctx = ctx;
    w->cancelled = 0;
}

void atree__write(struct atree__writer *w, const char *s, size_t len)
{
    if (w->cancelled || len == 0) {
        return;
    }
    if (w->fn(w->ctx, s, len) != 0) {
        w->cancelled = 1;
    }
}

void atree__write_cstr(struct atree__writer *w, const char *s)
{
    atree__write(w, s, strlen(s));
}

void atree__write_i64(struct atree__writer *w, int64_t v)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    if (n > 0) {
        atree__write(w, buf, (size_t)n);
    }
}

void atree__write_u64(struct atree__writer *w, uint64_t v)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
    if (n > 0) {
        atree__write(w, buf, (size_t)n);
    }
}

void atree__write_double(struct atree__writer *w, double v)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%.15g", v);
    if (n <= 0 || (size_t)n >= sizeof buf) {
        return;
    }
    if (!atree__double_eq(strtod(buf, NULL), v)) {
        n = snprintf(buf, sizeof buf, "%.17g", v);
        if (n <= 0 || (size_t)n >= sizeof buf) {
            return;
        }
    }
    if (strchr(buf, '.') == NULL) {
        /* "5" -> "5.0"; "1e+20" -> "1.0e+20" so the lexer sees a float. */
        char *e = strchr(buf, 'e');
        size_t len = (size_t)n;
        if (len + 2 >= sizeof buf) {
            return;
        }
        if (e == NULL) {
            memcpy(buf + len, ".0", 3);
        } else {
            size_t head = (size_t)(e - buf);
            memmove(e + 2, e, len - head + 1);
            memcpy(e, ".0", 2);
        }
        n += 2;
    }
    atree__write(w, buf, (size_t)n);
}

void atree__write_quoted(struct atree__writer *w, const char *s, size_t len)
{
    size_t i;
    size_t start = 0;
    atree__write(w, "\"", 1);
    for (i = 0; i < len; i++) {
        const char *esc = NULL;
        switch (s[i]) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\t':
            esc = "\\t";
            break;
        default:
            break;
        }
        if (esc != NULL) {
            atree__write(w, s + start, i - start);
            atree__write_cstr(w, esc);
            start = i + 1;
        }
    }
    atree__write(w, s + start, len - start);
    atree__write(w, "\"", 1);
}

atree_status_t atree__writer_status(const struct atree__writer *w)
{
    return w->cancelled ? ATREE_ERR_CANCELLED : ATREE_OK;
}

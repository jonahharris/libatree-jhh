/*
 * Output helper over atree_write_fn: buffers nothing, formats numbers and
 * quoted strings, and becomes a no-op after the callback asks to stop.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_WRITER_H
#define ATREE_WRITER_H

#include <stddef.h>
#include <stdint.h>

#include "atree.h"

struct atree__writer {
    atree_write_fn fn;
    void *ctx;
    int cancelled; /* nonzero once fn returned nonzero */
};

void atree__writer_init(struct atree__writer *w, atree_write_fn fn, void *ctx);
void atree__write(struct atree__writer *w, const char *s, size_t len);
void atree__write_cstr(struct atree__writer *w, const char *s);
void atree__write_i64(struct atree__writer *w, int64_t v);
void atree__write_u64(struct atree__writer *w, uint64_t v);
/* Shortest form that reads back exactly; always contains '.' or an exponent
 * so the DSL lexer sees a float. Assumes the "C" locale decimal point. */
void atree__write_double(struct atree__writer *w, double v);
/* "..." with \\ \" \n \t escapes; other bytes verbatim. */
void atree__write_quoted(struct atree__writer *w, const char *s, size_t len);
/* ATREE_OK or ATREE_ERR_CANCELLED. */
atree_status_t atree__writer_status(const struct atree__writer *w);

#endif /* ATREE_WRITER_H */

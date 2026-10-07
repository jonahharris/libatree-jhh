/*
 * Minimal JSON reader for event payloads: one object whose members are
 * numbers, strings, booleans, null, or arrays of numbers or strings.
 * Nothing else is accepted (no nested objects), which is all an event
 * needs. No allocation beyond the caller-provided scratch.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREED_JSON_H
#define ATREED_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum json_kind { JSON_NULL, JSON_BOOL, JSON_INT, JSON_FLOAT, JSON_STRING, JSON_ARRAY };

#define JSON_MAX_ITEMS 256

struct json_value {
    enum json_kind kind;
    bool b;
    int64_t i;
    double f;
    const char *s; /* unescaped, NUL-terminated, in the scratch buffer */
    size_t slen;
    /* arrays: every item is a string (strs set) or every item is an int (ints) */
    size_t nitems;
    bool items_are_strings;
    const char *strs[JSON_MAX_ITEMS];
    size_t slens[JSON_MAX_ITEMS];
    int64_t ints[JSON_MAX_ITEMS];
};

/* Called for each member; return nonzero to stop (that value is returned). */
typedef int (*json_member_fn)(void *ctx, const char *key, size_t keylen,
                              const struct json_value *value);

/* Parses `text` as an object and reports every member. `scratch` receives
 * unescaped strings and must be at least `len + 1` bytes. Returns 0 on
 * success, the callback's nonzero value, or -1 on a syntax error with
 * *errpos set to the byte offset. */
int json_parse_object(const char *text, size_t len, char *scratch, json_member_fn fn, void *ctx,
                      size_t *errpos);

#endif

/*
 * The plain-text formats shared by bench_file and tools/atree_shell:
 *
 *   DEFS   one attribute per line:   name<TAB>type
 *          type: bool | int | float | string | int_list | string_list
 *   EXPRS  one expression per line:  id<TAB>expression        (DSL)
 *   EVENTS one event per line:       attr=value;attr=value;...
 *          value: true | false | 123 | 1.5 | "text" | [1, 2] | ["a", "b"]
 *   Lines starting with # are ignored.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_BENCH_FORMAT_H
#define ATREE_BENCH_FORMAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atree.h"

#define MAX_LINE 65536

/* Reads the next non-empty, non-comment line into buf. Returns buf or NULL. */
static char *read_line(FILE *f, char *buf)
{
    for (;;) {
        size_t n;
        if (fgets(buf, MAX_LINE, f) == NULL) {
            return NULL;
        }
        n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        if (buf[0] == '#' || buf[0] == '\0') {
            continue;
        }
        return buf;
    }
}

static int type_of(const char *s, atree_type_t *t)
{
    if (strcmp(s, "bool") == 0 || strcmp(s, "boolean") == 0) {
        *t = ATREE_TYPE_BOOL;
    } else if (strcmp(s, "int") == 0 || strcmp(s, "integer") == 0) {
        *t = ATREE_TYPE_INT;
    } else if (strcmp(s, "float") == 0) {
        *t = ATREE_TYPE_FLOAT;
    } else if (strcmp(s, "string") == 0) {
        *t = ATREE_TYPE_STRING;
    } else if (strcmp(s, "int_list") == 0 || strcmp(s, "integer_list") == 0) {
        *t = ATREE_TYPE_INT_LIST;
    } else if (strcmp(s, "string_list") == 0) {
        *t = ATREE_TYPE_STRING_LIST;
    } else {
        return 1;
    }
    return 0;
}

/* Parses one `attr=value` item into the event. */
static int set_item(const atree_t *t, atree_event_t *ev, char *item)
{
    char *eq = strchr(item, '=');
    char *val;
    atree_attr_id_t id;
    atree_type_t ty;
    if (eq == NULL) {
        return 1;
    }
    *eq = '\0';
    val = eq + 1;
    while (*item == ' ') {
        item++;
    }
    while (*val == ' ') {
        val++;
    }
    id = atree_attr_lookup(t, item);
    if (id == ATREE_ATTR_INVALID) {
        return 1;
    }
    ty = atree_attr_type(t, id);
    switch (ty) {
    case ATREE_TYPE_BOOL:
        return atree_event_set_bool_id(ev, id, strcmp(val, "true") == 0) != ATREE_OK;
    case ATREE_TYPE_INT:
        return atree_event_set_int_id(ev, id, strtoll(val, NULL, 10)) != ATREE_OK;
    case ATREE_TYPE_FLOAT:
        return atree_event_set_float_id(ev, id, strtod(val, NULL)) != ATREE_OK;
    case ATREE_TYPE_STRING: {
        size_t n = strlen(val);
        if (n >= 2 && val[0] == '"' && val[n - 1] == '"') {
            return atree_event_set_string_id(ev, id, val + 1, n - 2) != ATREE_OK;
        }
        return atree_event_set_string_id(ev, id, val, n) != ATREE_OK;
    }
    case ATREE_TYPE_INT_LIST:
    case ATREE_TYPE_STRING_LIST:
    default: {
        /* [a, b, c] */
        const char *strs[256];
        size_t lens[256];
        int64_t ints[256];
        size_t n = 0;
        char *p = val;
        if (*p != '[') {
            return 1;
        }
        p++;
        while (*p != '\0' && *p != ']' && n < 256) {
            while (*p == ' ' || *p == ',') {
                p++;
            }
            if (*p == ']' || *p == '\0') {
                break;
            }
            if (*p == '"') {
                char *end = strchr(p + 1, '"');
                if (end == NULL) {
                    return 1;
                }
                strs[n] = p + 1;
                lens[n] = (size_t)(end - p - 1);
                n++;
                p = end + 1;
            } else {
                char *end = NULL;
                ints[n++] = strtoll(p, &end, 10);
                if (end == p) {
                    return 1;
                }
                p = end;
            }
        }
        if (ty == ATREE_TYPE_INT_LIST) {
            return atree_event_set_int_list_id(ev, id, ints, n) != ATREE_OK;
        }
        return atree_event_set_string_list_id(ev, id, strs, lens, n) != ATREE_OK;
    }
    }
}

#endif /* ATREE_BENCH_FORMAT_H */

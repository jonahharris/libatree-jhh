/*
 * Loads the same DEFS/EXPRS/EVENTS files bench_file reads, inserts them into
 * the reference be-tree (reference/be-tree, Boolean Expression tree after
 * Whang et al.) and reports insert throughput, search latency, heap in use
 * and total matches so the two implementations can be compared on identical
 * data. Optional tooling, like bench/rust_compare; not part of the library.
 *
 *   betree_compare DEFS EXPRS EVENTS [--repeat R]
 *
 * Dialect: be-tree writes list literals in parentheses, so `[` and `]`
 * outside string literals are rewritten to `(` and `)`; everything else the
 * generator's --rust-compatible dialect emits (=, <, <=, >, >=, in, one of,
 * none of, and, or, not, 'string') is read by be-tree as is. Every attribute
 * is declared allow_undefined, since an event defines a few of the
 * attributes, as in the A-Tree paper's workload.
 *
 * SPDX-License-Identifier: MIT
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

#include "betree.h"

#define MAX_LINE 65536
#define MAX_DEFS 4096

enum def_type { T_BOOL, T_INT, T_FLOAT, T_STRING, T_INT_LIST, T_STRING_LIST };

struct def {
    char *name;
    enum def_type type;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Bytes of heap the process currently has in use, or 0 when the platform
 * does not report it. */
static uint64_t heap_in_use(void)
{
#if defined(__APPLE__)
    struct mstats m = mstats();
    return (uint64_t)m.bytes_used;
#elif defined(__GLIBC__)
    struct mallinfo2 m = mallinfo2();
    return (uint64_t)m.uordblks + (uint64_t)m.hblkhd;
#else
    return 0;
#endif
}

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

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int type_of(const char *s, enum def_type *t)
{
    if (strcmp(s, "bool") == 0) {
        *t = T_BOOL;
    } else if (strcmp(s, "int") == 0) {
        *t = T_INT;
    } else if (strcmp(s, "float") == 0) {
        *t = T_FLOAT;
    } else if (strcmp(s, "string") == 0) {
        *t = T_STRING;
    } else if (strcmp(s, "int_list") == 0) {
        *t = T_INT_LIST;
    } else if (strcmp(s, "string_list") == 0) {
        *t = T_STRING_LIST;
    } else {
        return 1;
    }
    return 0;
}

static size_t find_def(const struct def *defs, size_t ndefs, const char *name)
{
    size_t i;
    for (i = 0; i < ndefs; i++) {
        if (strcmp(defs[i].name, name) == 0) {
            return i;
        }
    }
    return ndefs;
}

/* `[1, 2]` -> `(1, 2)` outside quoted strings; in place. */
static void to_betree_dialect(char *s)
{
    char quote = 0;
    for (; *s != '\0'; s++) {
        if (quote != 0) {
            if (*s == quote) {
                quote = 0;
            }
        } else if (*s == '\'' || *s == '"') {
            quote = *s;
        } else if (*s == '[') {
            *s = '(';
        } else if (*s == ']') {
            *s = ')';
        }
    }
}

/* Parses one `attr=value` item into the be-tree event. */
static int set_item(struct betree_event *ev, const struct def *defs, size_t ndefs, char *item)
{
    char *eq = strchr(item, '=');
    char *val;
    size_t idx;
    struct betree_variable *var = NULL;
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
    idx = find_def(defs, ndefs, item);
    if (idx == ndefs) {
        return 1;
    }
    switch (defs[idx].type) {
    case T_BOOL:
        if (strcmp(val, "true") != 0 && strcmp(val, "false") != 0) {
            return 1;
        }
        var = betree_make_boolean_variable(item, strcmp(val, "true") == 0);
        break;
    case T_INT: {
        char *end = NULL;
        long long v = strtoll(val, &end, 10);
        if (end == val || *end != '\0') {
            return 1;
        }
        var = betree_make_integer_variable(item, (int64_t)v);
        break;
    }
    case T_FLOAT: {
        char *end = NULL;
        double v = strtod(val, &end);
        if (end == val || *end != '\0') {
            return 1;
        }
        var = betree_make_float_variable(item, v);
        break;
    }
    case T_STRING: {
        size_t n = strlen(val);
        if (n >= 2 && val[0] == '"' && val[n - 1] == '"') {
            val[n - 1] = '\0';
            val++;
        }
        var = betree_make_string_variable(item, val);
        break;
    }
    case T_INT_LIST:
    case T_STRING_LIST:
    default: {
        char *strs[256];
        int64_t ints[256];
        size_t n = 0;
        size_t i;
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
                *end = '\0';
                strs[n++] = p + 1;
                p = end + 1;
            } else {
                char *end = NULL;
                ints[n++] = (int64_t)strtoll(p, &end, 10);
                if (end == p) {
                    return 1;
                }
                p = end;
            }
        }
        if (defs[idx].type == T_INT_LIST) {
            struct betree_integer_list *l = betree_make_integer_list(n);
            for (i = 0; i < n; i++) {
                betree_add_integer(l, i, ints[i]);
            }
            var = betree_make_integer_list_variable(item, l);
        } else {
            struct betree_string_list *l = betree_make_string_list(n);
            for (i = 0; i < n; i++) {
                betree_add_string(l, i, strs[i]);
            }
            var = betree_make_string_list_variable(item, l);
        }
        break;
    }
    }
    betree_set_variable(ev, idx, var);
    return 0;
}

int main(int argc, char **argv)
{
    const char *paths[3] = {NULL, NULL, NULL};
    int npaths = 0;
    int repeat = 1;
    int i;
    FILE *f;
    char *buf;
    char *line;
    struct def defs[MAX_DEFS];
    size_t ndefs = 0;
    struct betree *tree;
    const struct betree_sub **subs = NULL;
    size_t nsubs = 0;
    size_t subs_cap = 0;
    size_t nexpr = 0;
    size_t failures = 0;
    uint64_t heap0;
    uint64_t heap_tree;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    double make_s;
    double insert_s;
    struct betree_event **events = NULL;
    size_t nevents = 0;
    size_t events_cap = 0;
    uint64_t *lat = NULL;
    size_t nlat = 0;
    uint64_t total_matches = 0;
    uint64_t total_evaluated = 0;
    uint64_t total_memoized = 0;
    uint64_t total_shorted = 0;
    double sum = 0.0;
    int r;
    size_t k;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
        } else if (npaths < 3) {
            paths[npaths++] = argv[i];
        }
    }
    if (npaths != 3) {
        fprintf(stderr, "usage: betree_compare DEFS EXPRS EVENTS [--repeat R]\n");
        return 2;
    }
    buf = (char *)malloc(MAX_LINE);
    if (buf == NULL) {
        return 2;
    }

    heap0 = heap_in_use();
    tree = betree_make();

    /* schema: every attribute may be undefined in an event */
    f = fopen(paths[0], "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", paths[0]);
        return 2;
    }
    while ((line = read_line(f, buf)) != NULL && ndefs < MAX_DEFS) {
        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            tab = strchr(line, ' ');
        }
        if (tab == NULL) {
            fprintf(stderr, "bad def line: %s\n", line);
            return 2;
        }
        *tab = '\0';
        while (*++tab == ' ' || *tab == '\t') {
        }
        defs[ndefs].name = strdup(line);
        if (defs[ndefs].name == NULL || type_of(tab, &defs[ndefs].type) != 0) {
            fprintf(stderr, "bad def: %s %s\n", line, tab);
            return 2;
        }
        switch (defs[ndefs].type) {
        case T_BOOL:
            betree_add_boolean_variable(tree, line, true);
            break;
        case T_INT:
            betree_add_integer_variable(tree, line, true, INT64_MIN, INT64_MAX);
            break;
        case T_FLOAT:
            betree_add_float_variable(tree, line, true, -1e308, 1e308);
            break;
        case T_STRING:
            betree_add_string_variable(tree, line, true, SIZE_MAX);
            break;
        case T_INT_LIST:
            betree_add_integer_list_variable(tree, line, true, INT64_MIN, INT64_MAX);
            break;
        case T_STRING_LIST:
        default:
            betree_add_string_list_variable(tree, line, true, SIZE_MAX);
            break;
        }
        ndefs++;
    }
    fclose(f);

    /* expressions: be-tree's own benchmark makes every sub first (parse,
     * validate, adjust the domain bounds) and then inserts them all; both
     * phases are timed and reported as insert. */
    f = fopen(paths[1], "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", paths[1]);
        return 2;
    }
    t0 = now_ns();
    while ((line = read_line(f, buf)) != NULL) {
        char *tab = strchr(line, '\t');
        uint64_t id;
        const struct betree_sub *sub;
        if (tab == NULL) {
            fprintf(stderr, "bad expression line: %s\n", line);
            return 2;
        }
        *tab = '\0';
        id = strtoull(line, NULL, 10);
        to_betree_dialect(tab + 1);
        sub = betree_make_sub(tree, id, 0, NULL, tab + 1);
        nexpr++;
        if (sub == NULL) {
            if (failures < 3) {
                fprintf(stderr, "make_sub %" PRIu64 " failed: %s\n", id, tab + 1);
            }
            failures++;
            continue;
        }
        if (nsubs == subs_cap) {
            const struct betree_sub **grown;
            subs_cap = subs_cap ? subs_cap * 2 : 1024;
            grown = (const struct betree_sub **)realloc((void *)subs, subs_cap * sizeof *subs);
            if (grown == NULL) {
                return 2;
            }
            subs = grown;
        }
        subs[nsubs++] = sub;
    }
    fclose(f);
    t1 = now_ns();
    for (k = 0; k < nsubs; k++) {
        if (!betree_insert_sub(tree, subs[k])) {
            if (failures < 3) {
                fprintf(stderr, "insert_sub %zu failed\n", k);
            }
            failures++;
        }
    }
    t2 = now_ns();
    make_s = (double)(t1 - t0) / 1e9;
    insert_s = (double)(t2 - t1) / 1e9;
    heap_tree = heap_in_use();

    /* events, built once through the API outside the timed region */
    f = fopen(paths[2], "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", paths[2]);
        return 2;
    }
    while ((line = read_line(f, buf)) != NULL) {
        char *item = line;
        struct betree_event *ev = betree_make_event(tree);
        while (item != NULL && *item != '\0') {
            char *semi = strchr(item, ';');
            if (semi != NULL) {
                *semi = '\0';
            }
            if (set_item(ev, defs, ndefs, item) != 0) {
                fprintf(stderr, "bad event item: %s\n", item);
                return 2;
            }
            item = semi != NULL ? semi + 1 : NULL;
        }
        if (nevents == events_cap) {
            struct betree_event **grown;
            events_cap = events_cap ? events_cap * 2 : 1024;
            grown = (struct betree_event **)realloc((void *)events, events_cap * sizeof *events);
            if (grown == NULL) {
                return 2;
            }
            events = grown;
        }
        events[nevents++] = ev;
    }
    fclose(f);

    lat = (uint64_t *)malloc((nevents * (size_t)repeat + 1) * sizeof *lat);
    if (lat == NULL) {
        return 2;
    }
    for (r = 0; r < repeat; r++) {
        for (k = 0; k < nevents; k++) {
            uint64_t s0 = now_ns();
            struct report *rep = make_report();
            uint64_t s1;
            if (!betree_search_with_event(tree, events[k], rep)) {
                fprintf(stderr, "search failed\n");
                return 2;
            }
            s1 = now_ns();
            lat[nlat++] = s1 - s0;
            total_matches += rep->matched;
            total_evaluated += rep->evaluated;
            total_memoized += rep->memoized;
            total_shorted += rep->shorted;
            free_report(rep);
        }
    }

    for (k = 0; k < nlat; k++) {
        sum += (double)lat[k];
    }
    qsort(lat, nlat, sizeof *lat, cmp_u64);
    printf("be-tree (reference/be-tree) benchmark: %zu expressions, %zu events (x%d)\n", nexpr,
           nevents, repeat);
    printf("  heap in use after insert: %.1f MB (%.0f bytes/expression)\n",
           (double)(heap_tree - heap0) / 1048576.0,
           nexpr ? (double)(heap_tree - heap0) / (double)nexpr : 0.0);
    printf("  insert: %.0f expressions/s (make_sub %.2f s + insert_sub %.2f s), %zu failures\n",
           make_s + insert_s > 0 ? (double)nexpr / (make_s + insert_s) : 0.0, make_s, insert_s,
           failures);
    if (nlat > 0) {
        printf("  search: p50 %.1f us, p99 %.1f us, mean %.1f us; per event avg %.1f matches, "
               "%.1f subs evaluated, %.1f memoized, %.1f shorted; total matches %" PRIu64 "\n",
               (double)lat[nlat / 2] / 1000.0,
               (double)lat[nlat * 99 / 100 < nlat ? nlat * 99 / 100 : nlat - 1] / 1000.0,
               sum / (double)nlat / 1000.0, (double)total_matches / (double)nlat,
               (double)total_evaluated / (double)nlat, (double)total_memoized / (double)nlat,
               (double)total_shorted / (double)nlat, total_matches);
    }

    for (k = 0; k < nevents; k++) {
        betree_free_event(events[k]);
    }
    free((void *)events);
    free((void *)subs);
    betree_free(tree);
    for (k = 0; k < ndefs; k++) {
        free(defs[k].name);
    }
    free(lat);
    free(buf);
    return failures != 0 ? 1 : 0;
}

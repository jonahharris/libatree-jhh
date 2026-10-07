/*
 * File-driven benchmark: a schema, a set of expressions and a set of events
 * from plain text files.
 *
 *   bench_file DEFS EXPRS EVENTS [--repeat R] [--json] [--flags bits]
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atree.h"

#include "bench_clock.h"
#include "bench_format.h"

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    const char *defs_path = NULL;
    const char *exprs_path = NULL;
    const char *events_path = NULL;
    int repeat = 1;
    int json = 0;
    unsigned flags = 0;
    int i;
    FILE *f;
    char *buf;
    char *line;
    atree_attr_def_t defs[4096];
    char *names[4096];
    size_t ndefs = 0;
    atree_config_t cfg;
    atree_t *t = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    size_t nexpr = 0;
    size_t nevents = 0;
    size_t failures = 0;
    double insert_us = 0.0;
    uint64_t *lat = NULL;
    size_t lat_cap = 0;
    uint64_t total_matches = 0;
    uint64_t total_visited = 0;
    uint64_t total_evaluated = 0;
    atree_stats_t st;
    int r;

    buf = (char *)malloc(MAX_LINE);
    if (buf == NULL) {
        return 2;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0) {
            json = 1;
        } else if (strcmp(argv[i], "--flags") == 0 && i + 1 < argc) {
            flags = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (defs_path == NULL) {
            defs_path = argv[i];
        } else if (exprs_path == NULL) {
            exprs_path = argv[i];
        } else if (events_path == NULL) {
            events_path = argv[i];
        }
    }
    if (defs_path == NULL || exprs_path == NULL || events_path == NULL) {
        fprintf(stderr,
                "usage: bench_file DEFS EXPRS EVENTS [--repeat R] [--json] [--flags bits]\n");
        return 2;
    }

    /* schema */
    f = fopen(defs_path, "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", defs_path);
        return 2;
    }
    while ((line = read_line(f, buf)) != NULL && ndefs < 4096) {
        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            tab = strchr(line, ' ');
        }
        if (tab == NULL) {
            fprintf(stderr, "bad def line: %s\n", line);
            return 2;
        }
        *tab = '\0';
        names[ndefs] = (char *)malloc(strlen(line) + 1);
        if (names[ndefs] == NULL) {
            return 2;
        }
        strcpy(names[ndefs], line);
        defs[ndefs].name = names[ndefs];
        while (*++tab == ' ' || *tab == '\t') {
        }
        if (type_of(tab, &defs[ndefs].type) != 0) {
            fprintf(stderr, "bad type: %s\n", tab);
            return 2;
        }
        ndefs++;
    }
    fclose(f);
    atree_config_init(&cfg);
    cfg.flags = flags;
    if (atree_create(&cfg, defs, ndefs, &t) != ATREE_OK) {
        fprintf(stderr, "atree_create failed\n");
        return 2;
    }

    /* expressions */
    f = fopen(exprs_path, "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", exprs_path);
        return 2;
    }
    while ((line = read_line(f, buf)) != NULL) {
        char *tab = strchr(line, '\t');
        atree_id_t id;
        atree_error_t err;
        uint64_t t0;
        uint64_t t1;
        if (tab == NULL) {
            fprintf(stderr, "bad expression line: %s\n", line);
            return 2;
        }
        *tab = '\0';
        id = strtoull(line, NULL, 10);
        t0 = bench_now_ns();
        if (atree_insert(t, id, tab + 1, SIZE_MAX, &err) != ATREE_OK) {
            fprintf(stderr, "insert %llu failed @%lu: %s\n", (unsigned long long)id,
                    (unsigned long)err.offset, err.message);
            failures++;
        }
        t1 = bench_now_ns();
        insert_us += (double)(t1 - t0) / 1000.0;
        nexpr++;
    }
    fclose(f);
    atree_stats(t, &st);
    if (atree_validate(t, NULL, 0) != ATREE_OK) {
        fprintf(stderr, "validate failed\n");
        return 2;
    }

    /* events */
    if (atree_event_create(t, &ev) != ATREE_OK || atree_report_create(t, &rep) != ATREE_OK) {
        return 2;
    }
    for (r = 0; r < repeat; r++) {
        f = fopen(events_path, "r");
        if (f == NULL) {
            fprintf(stderr, "cannot open %s\n", events_path);
            return 2;
        }
        while ((line = read_line(f, buf)) != NULL) {
            char *item = line;
            uint64_t t0;
            uint64_t t1;
            atree_report_stats_t rs;
            atree_event_clear(ev);
            while (item != NULL && *item != '\0') {
                char *semi = strchr(item, ';');
                if (semi != NULL) {
                    *semi = '\0';
                }
                if (set_item(t, ev, item) != 0) {
                    fprintf(stderr, "bad event item: %s\n", item);
                    return 2;
                }
                item = semi != NULL ? semi + 1 : NULL;
            }
            t0 = bench_now_ns();
            if (atree_search(t, ev, rep) != ATREE_OK) {
                return 2;
            }
            t1 = bench_now_ns();
            if (nevents == lat_cap) {
                lat_cap = lat_cap ? lat_cap * 2 : 1024;
                lat = (uint64_t *)realloc(lat, lat_cap * sizeof *lat);
                if (lat == NULL) {
                    return 2;
                }
            }
            lat[nevents++] = t1 - t0;
            atree_report_stats(rep, &rs);
            total_matches += rs.matches;
            total_visited += rs.nodes_visited;
            total_evaluated += rs.predicates_evaluated;
        }
        fclose(f);
    }

    if (nevents > 0) {
        double sum = 0.0;
        size_t k;
        for (k = 0; k < nevents; k++) {
            sum += (double)lat[k];
        }
        qsort(lat, nevents, sizeof *lat, cmp_u64);
        if (json) {
            printf("{\"expressions\": %lu, \"events\": %lu, \"nodes\": %llu, \"edges\": %llu, "
                   "\"bytes_allocated\": %llu, \"insert_per_sec\": %.0f, \"search_p50_us\": %.2f, "
                   "\"search_p99_us\": %.2f, \"search_mean_us\": %.2f, \"total_matches\": %llu, "
                   "\"total_nodes_visited\": %llu, \"total_predicates_evaluated\": %llu, "
                   "\"failures\": %lu}\n",
                   (unsigned long)nexpr, (unsigned long)nevents, (unsigned long long)st.nodes,
                   (unsigned long long)st.edges, (unsigned long long)st.bytes_allocated,
                   insert_us > 0 ? (double)nexpr / (insert_us / 1e6) : 0.0,
                   (double)lat[nevents / 2] / 1000.0,
                   (double)lat[nevents * 99 / 100 < nevents ? nevents * 99 / 100 : nevents - 1] /
                       1000.0,
                   sum / (double)nevents / 1000.0, (unsigned long long)total_matches,
                   (unsigned long long)total_visited, (unsigned long long)total_evaluated,
                   (unsigned long)failures);
        } else {
            printf("libatree file benchmark: %lu expressions, %lu events (x%d)\n",
                   (unsigned long)nexpr, (unsigned long)nevents, repeat);
            printf("  index: %llu nodes, %llu edges, max level %llu, %.1f bytes/expression, "
                   "reorganized %llu, self-adjusted %llu\n",
                   (unsigned long long)st.nodes, (unsigned long long)st.edges,
                   (unsigned long long)st.max_level,
                   nexpr ? (double)st.bytes_allocated / (double)nexpr : 0.0,
                   (unsigned long long)st.reorganized, (unsigned long long)st.self_adjusted);
            printf("  insert: %.0f expressions/s, %lu failures\n",
                   insert_us > 0 ? (double)nexpr / (insert_us / 1e6) : 0.0,
                   (unsigned long)failures);
            printf("  search: p50 %.1f us, p99 %.1f us, mean %.1f us; per event avg %.1f matches, "
                   "%.1f nodes visited, %.1f predicates evaluated\n",
                   (double)lat[nevents / 2] / 1000.0,
                   (double)lat[nevents * 99 / 100 < nevents ? nevents * 99 / 100 : nevents - 1] /
                       1000.0,
                   sum / (double)nevents / 1000.0, (double)total_matches / (double)nevents,
                   (double)total_visited / (double)nevents,
                   (double)total_evaluated / (double)nevents);
        }
    }

    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(t);
    {
        size_t k;
        for (k = 0; k < ndefs; k++) {
            free(names[k]);
        }
    }
    free(lat);
    free(buf);
    return failures != 0 ? 1 : 0;
}

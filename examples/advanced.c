/*
 * advanced.c — the secondary APIs: callback search with early stop,
 * atree_exists, atree_search_ids, a custom allocator, configuration fields
 * and optimization flags, atree_validate and Graphviz export.
 *
 * Build and run:  make examples && build/examples/advanced
 *                 build/examples/advanced --dot | dot -Tsvg > tree.svg
 * With --dot the DAG goes to stdout and the narrative to stderr. The program
 * checks its own results and exits nonzero if any differs.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atree.h"

#define CHECK(st) check_status((st), #st, __FILE__, __LINE__)
#define EXPECT(cond) expect((cond), #cond, __FILE__, __LINE__)

static int failures;
static FILE *out; /* narrative: stdout, or stderr when --dot owns stdout */

static void check_status(atree_status_t st, const char *what, const char *file, int line)
{
    if (st != ATREE_OK) {
        fprintf(stderr, "%s:%d: %s: %s\n", file, line, what, atree_strerror(st));
        exit(EXIT_FAILURE);
    }
}

static void expect(bool ok, const char *what, const char *file, int line)
{
    if (!ok) {
        fprintf(stderr, "%s:%d: expectation failed: %s\n", file, line, what);
        failures++;
    }
}

/* ---- a counting allocator ------------------------------------------------ */

/* README "Allocator": the library passes free/realloc the exact size it asked
 * for and never asks for 0 bytes. This allocator stores each block's size in
 * a header, checks both promises, and counts live bytes. A header the size of
 * the most-aligned type keeps the returned pointer suitably aligned. */
typedef union {
    long double ld;
    long long ll;
    void *p;
} max_align;
#define HDR sizeof(max_align)

struct counter {
    size_t live, peak, calls, contract_violations;
};

static void *count_alloc(void *ctx, size_t size)
{
    struct counter *c = ctx;
    unsigned char *raw;
    c->calls++;
    c->contract_violations += size == 0;
    raw = size <= SIZE_MAX - HDR ? malloc(HDR + size) : NULL;
    if (raw == NULL) {
        return NULL;
    }
    memcpy(raw, &size, sizeof size);
    c->live += size;
    c->peak = c->live > c->peak ? c->live : c->peak;
    return raw + HDR;
}

static void count_free(void *ctx, void *ptr, size_t size)
{
    struct counter *c = ctx;
    unsigned char *raw = (unsigned char *)ptr - HDR;
    size_t stored;
    memcpy(&stored, raw, sizeof stored);
    c->calls++;
    c->contract_violations += stored != size;
    c->live -= stored;
    free(raw);
}

static void *count_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    struct counter *c = ctx;
    unsigned char *raw = (unsigned char *)ptr - HDR, *grown;
    size_t stored;
    memcpy(&stored, raw, sizeof stored);
    c->calls++;
    c->contract_violations += stored != old_size || new_size == 0;
    grown = new_size <= SIZE_MAX - HDR ? realloc(raw, HDR + new_size) : NULL;
    if (grown == NULL) {
        return NULL; /* the old block is untouched, as realloc promises */
    }
    memcpy(grown, &new_size, sizeof new_size);
    c->live = c->live - stored + new_size;
    c->peak = c->live > c->peak ? c->live : c->peak;
    return grown + HDR;
}

/* ---- helpers ------------------------------------------------------------- */

static const atree_attr_def_t attrs[] = {{"price", ATREE_TYPE_INT},
                                         {"country", ATREE_TYPE_STRING},
                                         {"tags", ATREE_TYPE_STRING_LIST},
                                         {"premium", ATREE_TYPE_BOOL}};
static const char *const subs[] = {
    "price > 100",
    "price between 50 and 150",
    "country in ['US', 'CA']",
    "country = 'US' and price > 100",
    "tags one of ['sale']",
    "premium or price < 10",
    "not premium and country <> 'FR'",
    "tags all of ['sale', 'new'] and price > 20",
};
#define NSUBS (sizeof subs / sizeof subs[0])

/* Every tree in this program has the same subscriptions, under ids 1..8. */
static void make_tree(const atree_config_t *cfg, atree_t **tree)
{
    size_t i;
    CHECK(atree_create(cfg, attrs, sizeof attrs / sizeof attrs[0], tree));
    for (i = 0; i < NSUBS; i++) {
        CHECK(atree_insert(*tree, (atree_id_t)(i + 1), subs[i], SIZE_MAX, NULL));
    }
}

static void set_event(atree_event_t *ev)
{
    static const char *const tags[] = {"sale", "new"};
    atree_event_clear(ev);
    CHECK(atree_event_set_int(ev, "price", 120));
    CHECK(atree_event_set_string(ev, "country", "US", SIZE_MAX));
    CHECK(atree_event_set_string_list(ev, "tags", tags, NULL, 2));
    CHECK(atree_event_set_bool(ev, "premium", false));
}

/* atree_match_fn: ids arrive in unspecified order; nonzero stops the search.
 * It runs under the tree's read lock if one is configured, so it must not
 * call back into the tree. */
static int take_three(void *ctx, atree_id_t id)
{
    size_t *seen = ctx;
    (void)id;
    return ++*seen == 3;
}

/* atree_write_fn sinks for atree_to_graphviz. */
static int write_file(void *ctx, const char *data, size_t len)
{
    return fwrite(data, 1, len, (FILE *)ctx) == len ? 0 : 1;
}

static int count_bytes(void *ctx, const char *data, size_t len)
{
    (void)data;
    *(size_t *)ctx += len;
    return 0;
}

int main(int argc, char **argv)
{
    struct counter counter = {0, 0, 0, 0};
    atree_allocator_t alloc;
    atree_config_t cfg;
    atree_t *tree = NULL, *plain = NULL;
    atree_event_t *ev = NULL, *ev2 = NULL;
    atree_report_t *rep = NULL, *rep2 = NULL;
    atree_report_stats_t rs, rs2;
    atree_stats_t st;
    atree_error_t err;
    char msg[256];
    size_t seen = 0, dot_bytes = 0, i;
    bool any = false, same;
    bool dot = argc == 2 && strcmp(argv[1], "--dot") == 0;
    static const atree_id_t allow[] = {2, 4, 6, 8}; /* sorted ascending */

    out = dot ? stderr : stdout;

    /* Configuration: every zero field means "default"; set only what matters.
     * README "Configuration, errors and limits" lists them all. */
    alloc.alloc = count_alloc;
    alloc.realloc = count_realloc;
    alloc.free = count_free;
    alloc.ctx = &counter;
    atree_config_init(&cfg);
    cfg.allocator = &alloc;
    cfg.max_depth = 4;      /* tight nesting limit, to show the error */
    cfg.initial_nodes = 64; /* small capacity hint for a small tree */
    make_tree(&cfg, &tree);

    EXPECT(atree_insert(tree, 99, "not (not (not (not (not premium))))", SIZE_MAX, &err) ==
           ATREE_ERR_TOO_DEEP);
    fprintf(out, "five nested nots with max_depth 4: %s\n", atree_strerror(err.status));

    CHECK(atree_event_create(tree, &ev));
    CHECK(atree_report_create(tree, &rep));
    set_event(ev);

    /* Early stop: the search returns ATREE_OK even though it was cut short.
     * The report serves as scratch, so the call allocates nothing. */
    CHECK(atree_search_cb(tree, ev, rep, take_three, &seen));
    fprintf(out, "search_cb stopped after %lu ids\n", (unsigned long)seen);
    EXPECT(seen == 3);

    CHECK(atree_exists(tree, ev, rep, &any));
    fprintf(out, "exists: %s\n", any ? "yes" : "no");
    EXPECT(any);

    /* Restrict results to a sorted allow list, e.g. the ids one tenant owns. */
    CHECK(atree_search_ids(tree, ev, rep, allow, sizeof allow / sizeof allow[0]));
    fprintf(out, "search_ids over {2, 4, 6, 8}:");
    for (i = 0; i < atree_report_count(rep); i++) {
        fprintf(out, " %llu", (unsigned long long)atree_report_matches(rep)[i]);
    }
    fprintf(out, "\n");
    EXPECT(atree_report_count(rep) == 3 && atree_report_matches(rep)[2] == 8);

    /* An event on which every predicate is false or undefined. */
    atree_event_clear(ev);
    CHECK(atree_event_set_string(ev, "country", "FR", SIZE_MAX));
    CHECK(atree_exists(tree, ev, rep, &any));
    EXPECT(!any);

    /* Flags turn optimizations off for diagnosis; they change the work a
     * search does, never its result (atree.h, ATREE_FLAG_*). Without the
     * predicate index, phase 1 evaluates every leaf instead of probing. */
    atree_config_init(&cfg);
    cfg.flags = ATREE_FLAG_NO_PREDICATE_INDEX;
    make_tree(&cfg, &plain);
    CHECK(atree_event_create(plain, &ev2));
    CHECK(atree_report_create(plain, &rep2));
    set_event(ev);
    set_event(ev2);
    CHECK(atree_search(tree, ev, rep));
    CHECK(atree_search(plain, ev2, rep2));
    atree_report_stats(rep, &rs);
    atree_report_stats(rep2, &rs2);
    same = atree_report_count(rep) == atree_report_count(rep2);
    for (i = 0; same && i < atree_report_count(rep); i++) {
        same = atree_report_matches(rep)[i] == atree_report_matches(rep2)[i];
    }
    fprintf(out, "indexed:   %lu matches, %llu predicates evaluated\n",
            (unsigned long)atree_report_count(rep), (unsigned long long)rs.predicates_evaluated);
    fprintf(out, "no index:  %lu matches, %llu predicates evaluated\n",
            (unsigned long)atree_report_count(rep2), (unsigned long long)rs2.predicates_evaluated);
    EXPECT(same && atree_report_count(rep) == 7);
    EXPECT(rs2.predicates_evaluated > rs.predicates_evaluated);

    /* Structural self-check, for tests and diagnostics. */
    CHECK(atree_validate(tree, msg, sizeof msg));
    CHECK(atree_validate(plain, msg, sizeof msg));
    fprintf(out, "validate: both trees OK\n");

    /* DOT export, drawn like the paper's figures. */
    if (dot) {
        CHECK(atree_to_graphviz(tree, write_file, stdout));
    } else {
        CHECK(atree_to_graphviz(tree, count_bytes, &dot_bytes));
        fprintf(out, "graphviz: %lu bytes of DOT (run with --dot to see them)\n",
                (unsigned long)dot_bytes);
    }

    /* atree_stats counts what the tree itself holds; the allocator also sees
     * the event and report. */
    atree_stats(tree, &st);
    fprintf(out, "allocator: %lu calls, %lu bytes live (tree %llu), peak %lu\n",
            (unsigned long)counter.calls, (unsigned long)counter.live,
            (unsigned long long)st.bytes_allocated, (unsigned long)counter.peak);

    atree_report_destroy(rep2);
    atree_event_destroy(ev2);
    atree_destroy(plain);
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(tree);
    fprintf(out, "after destroy: %lu bytes live, %lu contract violations\n",
            (unsigned long)counter.live, (unsigned long)counter.contract_violations);
    EXPECT(counter.live == 0 && counter.contract_violations == 0);

    fprintf(out, "%s\n", failures == 0 ? "advanced: OK" : "advanced: FAILED");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

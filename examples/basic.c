/*
 * basic.c — a walkthrough of the libatree public API, in reading order.
 *
 * Shows: defining attributes of every type, creating a tree, inserting
 * subscriptions from DSL text and from the builder API, parse errors and
 * duplicate ids, building and reusing an event, searching, three-valued
 * semantics checked against the reference evaluator, deleting, statistics,
 * and tearing everything down in the right order.
 *
 * Build and run:  make examples && build/examples/basic
 * The program checks its own results and exits nonzero if any differs, so
 * `make check` runs it as a test.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>

#include "atree.h"

/* Every fallible call returns atree_status_t (README "Configuration, errors
 * and limits"); an example that ignores one teaches the wrong habit. */
#define CHECK(st) check_status((st), #st, __FILE__, __LINE__)
#define EXPECT(cond) expect((cond), #cond, __FILE__, __LINE__)

static int failures;

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

/* atree_write_fn: atree_expr_print and atree_to_graphviz emit text in chunks
 * through a callback, so the library never touches stdio itself. */
static int write_stdout(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    return fwrite(data, 1, len, stdout) == len ? 0 : 1;
}

/* The subscriptions, one per predicate family of README "Expression
 * language". The table is kept so the reference evaluator can re-check them. */
static const struct {
    atree_id_t id;
    const char *text;
} subs[] = {
    {1, "age >= 18 and country = 'US'"},   /* comparison */
    {2, "country in ['CA', 'US', 'MX']"},  /* in */
    {3, "tags one of ['sports', 'news']"}, /* one of (list attribute) */
    {4, "tags all of ['sports', 'news']"}, /* all of */
    {5, "segments none of [7, 8]"},        /* none of */
    {6, "score is null"},                  /* is null */
    {7, "age between 30 and 40"},          /* between */
    {8, "not active"},                     /* not on a bare bool */
    /* nested and, or, not: */
    {9, "(age < 18 or score > 0.5) and not (segments one of [1] or country = 'DE')"},
    {10, "active xor score >= 0.9"}, /* xor */
    {11, "not (age = 5)"},           /* undefined when age is absent */
};
#define NSUBS (sizeof subs / sizeof subs[0])
#define BUILT_ID 20

/* Prints the sorted matches and compares them with the expected ids. */
static void expect_matches(const char *label, const atree_report_t *rep, const atree_id_t *want,
                           size_t nwant)
{
    size_t n = atree_report_count(rep);
    const atree_id_t *got = atree_report_matches(rep);
    size_t i;
    bool same = n == nwant;
    printf("%s: %lu matches:", label, (unsigned long)n);
    for (i = 0; i < n; i++) {
        printf(" %llu", (unsigned long long)got[i]);
        same = same && got[i] == want[i];
    }
    printf("\n");
    EXPECT(same);
}

/* README "Semantics": the tree matches a subscription iff atree_expr_eval
 * returns ATREE_TRUE. Re-evaluate every live subscription one by one and
 * check that the tree agrees. */
static void check_reference(const atree_t *tree, const atree_event_t *ev, const atree_report_t *rep,
                            const atree_expr_t *built)
{
    size_t i, j;
    for (i = 0; i <= NSUBS; i++) {
        atree_id_t id = i < NSUBS ? subs[i].id : BUILT_ID;
        atree_expr_t *e = NULL;
        bool in_report = false;
        if (!atree_contains(tree, id)) {
            continue;
        }
        if (i < NSUBS) {
            CHECK(atree_expr_parse(tree, subs[i].text, SIZE_MAX, &e, NULL));
        }
        for (j = 0; j < atree_report_count(rep); j++) {
            in_report = in_report || atree_report_matches(rep)[j] == id;
        }
        EXPECT((atree_expr_eval(e != NULL ? e : built, ev) == ATREE_TRUE) == in_report);
        atree_expr_free(e);
    }
}

int main(void)
{
    /* 1. Attributes are fixed at creation; names are case-sensitive and may
     *    not be DSL keywords. One of every type. */
    static const atree_attr_def_t attrs[] = {
        {"active", ATREE_TYPE_BOOL},       {"age", ATREE_TYPE_INT},
        {"score", ATREE_TYPE_FLOAT},       {"country", ATREE_TYPE_STRING},
        {"segments", ATREE_TYPE_INT_LIST}, {"tags", ATREE_TYPE_STRING_LIST},
    };
    atree_config_t cfg;
    atree_t *tree = NULL;
    atree_error_t err;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_expr_t *built, *either[2], *both[2], *probe = NULL;
    atree_report_stats_t rs;
    atree_stats_t st;
    atree_attr_id_t country;
    size_t i;
    static const int64_t ages[] = {21, 42};
    static const int64_t segs[] = {7, 3};
    static const char *const tags1[] = {"sports", "news"};
    static const char *const tags2[] = {"news"};
    static const atree_id_t want1[] = {1, 2, 3, 4, 7, 9, 10, 11};
    static const atree_id_t want2[] = {3, 6, 8};
    static const atree_id_t want3[] = {6, 8};

    atree_config_init(&cfg); /* all zero: every field means "library default" */
    CHECK(atree_create(&cfg, attrs, sizeof attrs / sizeof attrs[0], &tree));

    /* 2. Subscriptions from DSL text. SIZE_MAX means "NUL-terminated". */
    for (i = 0; i < NSUBS; i++) {
        CHECK(atree_insert(tree, subs[i].id, subs[i].text, SIZE_MAX, &err));
    }

    /* 3. The same kind of thing built programmatically:
     *      active and (country = 'CA' or age in [21, 42])
     *    Connectives take ownership of their children and free everything if
     *    any child is NULL, so the whole chain needs one NULL check at the end
     *    (README "Expression language"). */
    either[0] = atree_expr_eq_string(tree, "country", true, "CA", SIZE_MAX);
    either[1] = atree_expr_in_ints(tree, "age", true, ages, 2);
    both[0] = atree_expr_var(tree, "active");
    both[1] = atree_expr_or(either, 2);
    built = atree_expr_and(both, 2);
    if (built == NULL) {
        fprintf(stderr, "building the expression failed\n");
        return EXIT_FAILURE;
    }
    printf("built: ");
    CHECK(atree_expr_print(built, write_stdout, NULL));
    printf("\n");
    /* The tree copies what it needs; `built` stays ours (freed at the end). */
    CHECK(atree_insert_expr(tree, BUILT_ID, built, &err));

    /* 4. Errors carry a byte offset and a message, and leave the tree as it
     *    was. A reused id is refused rather than replaced. */
    EXPECT(atree_insert(tree, 99, "age >= and active", SIZE_MAX, &err) == ATREE_ERR_SYNTAX);
    printf("parse error at offset %lu: %s\n", (unsigned long)err.offset, err.message);
    EXPECT(atree_insert(tree, 1, "active", SIZE_MAX, &err) == ATREE_ERR_DUPLICATE_ID);
    printf("insert id 1 again: %s\n", atree_strerror(err.status));
    EXPECT(atree_count(tree) == NSUBS + 1);

    /* 5. An event assigns values to some attributes; the rest stay undefined.
     *    Events and reports are reusable and keep their buffers (README "API
     *    overview"); create them once, not per search. */
    CHECK(atree_event_create(tree, &ev));
    CHECK(atree_report_create(tree, &rep));
    CHECK(atree_event_set_bool(ev, "active", true));
    CHECK(atree_event_set_int(ev, "age", 35));
    CHECK(atree_event_set_float(ev, "score", 0.75));
    CHECK(atree_event_set_int_list(ev, "segments", segs, 2));       /* sorted for us */
    CHECK(atree_event_set_string_list(ev, "tags", tags1, NULL, 2)); /* NULL lens: NUL-terminated */
    /* By id: resolve the name once, skip a hash lookup per event. */
    country = atree_attr_lookup(tree, "country");
    EXPECT(country != ATREE_ATTR_INVALID);
    CHECK(atree_event_set_string_id(ev, country, "US", 2));

    CHECK(atree_search(tree, ev, rep));
    expect_matches("event 1", rep, want1, sizeof want1 / sizeof want1[0]);
    check_reference(tree, ev, rep, built);
    atree_report_stats(rep, &rs);
    printf("event 1 work: predicates evaluated %llu, true %llu, inner nodes visited %llu\n",
           (unsigned long long)rs.predicates_evaluated, (unsigned long long)rs.predicates_matched,
           (unsigned long long)rs.nodes_visited);

    /* 6. Reuse: clear makes every attribute undefined again. Here age, score
     *    and segments stay undefined, so predicates on them are undefined
     *    (paper §3.2): `score is null` is true, and `not (age = 5)` is NOT
     *    true, because not(undefined) is undefined (README "Semantics"). */
    atree_event_clear(ev);
    CHECK(atree_event_set_bool(ev, "active", false));
    CHECK(atree_event_set_string(ev, "country", "DE", SIZE_MAX));
    CHECK(atree_event_set_string_list(ev, "tags", tags2, NULL, 1));
    CHECK(atree_search(tree, ev, rep));
    expect_matches("event 2", rep, want2, sizeof want2 / sizeof want2[0]);
    check_reference(tree, ev, rep, built);

    CHECK(atree_expr_parse(tree, "not (age = 5)", SIZE_MAX, &probe, &err));
    printf("reference evaluator on 'not (age = 5)' without age: %s\n",
           atree_expr_eval(probe, ev) == ATREE_UNDEFINED ? "undefined" : "defined");
    EXPECT(atree_expr_eval(probe, ev) == ATREE_UNDEFINED);
    atree_expr_free(probe);

    /* 7. Delete releases the nodes no other subscription uses (Alg. 5). */
    CHECK(atree_delete(tree, 3));
    EXPECT(atree_delete(tree, 3) == ATREE_ERR_NOT_FOUND);
    CHECK(atree_search(tree, ev, rep));
    expect_matches("event 2 after deleting 3", rep, want3, sizeof want3 / sizeof want3[0]);
    printf("count %lu, contains(3) %s, contains(%d) %s\n", (unsigned long)atree_count(tree),
           atree_contains(tree, 3) ? "yes" : "no", BUILT_ID,
           atree_contains(tree, BUILT_ID) ? "yes" : "no");
    EXPECT(atree_count(tree) == NSUBS && !atree_contains(tree, 3));

    atree_stats(tree, &st);
    printf("tree: %llu subscriptions, %llu nodes (%llu leaves), %llu edges, %llu strings\n",
           (unsigned long long)st.subscriptions, (unsigned long long)st.nodes,
           (unsigned long long)st.leaves, (unsigned long long)st.edges,
           (unsigned long long)st.strings);
    EXPECT(st.subscriptions == NSUBS);

    /* 8. Teardown: the expression, event and report all belong to the tree
     *    (they use its allocator and attribute table), so release them first. */
    atree_expr_free(built);
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(tree);

    printf("%s\n", failures == 0 ? "basic: OK" : "basic: FAILED");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

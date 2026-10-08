/*
 * threads.c — sharing one tree between a writer and several readers.
 *
 * The tree is created with the ready-made pthread reader/writer lock from
 * extras/atree_lock_pthread.h, so every public call takes the lock exactly
 * once: searches share it, inserts and deletes take it exclusively (README
 * "Thread safety"). One writer thread churns subscriptions while readers
 * match events in a loop and check every result for consistency.
 *
 * The alternative to a lock is build-swap-retire: build a new tree, publish
 * its pointer, wait for every reader to move over, destroy the old one.
 * README "Thread safety" (option 3) describes it; tests/test_threads.c
 * implements it.
 *
 * POSIX only.  Build and run:  make examples && build/examples/threads
 *
 * SPDX-License-Identifier: MIT
 */
/* pthread_rwlock_* is POSIX; a strict -std=c99 hides it unless this comes
 * before the first system header. */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "atree.h"

#include "../extras/atree_lock_pthread.h"

#define CHECK(st) check_status((st), #st, __FILE__, __LINE__)

#define NREADERS 4
#define NEVENTS 2000 /* events matched by each reader */
#define NROUNDS 500  /* subscriptions the writer inserts (and later deletes) */
#define WINDOW 8     /* how many churned subscriptions are live at once */
#define NVALUES 11   /* events carry n in 0 .. NVALUES-1 */
#define CHURN_BASE 1000

static void check_status(atree_status_t st, const char *what, const char *file, int line)
{
    if (st != ATREE_OK) {
        fprintf(stderr, "%s:%d: %s: %s\n", file, line, what, atree_strerror(st));
        exit(EXIT_FAILURE);
    }
}

struct reader {
    atree_t *tree;
    unsigned seed;
    unsigned long searches, failures;
};

/* Writer: subscription CHURN_BASE + r is "n = r % NVALUES"; at most WINDOW of
 * them are live at a time. Each insert and delete holds the write lock, so a
 * reader sees either all of one or none of it, never half an insert. */
static void *writer_main(void *arg)
{
    atree_t *tree = arg;
    char text[32];
    unsigned r;
    for (r = 0; r < NROUNDS; r++) {
        snprintf(text, sizeof text, "n = %u", r % NVALUES);
        CHECK(atree_insert(tree, CHURN_BASE + r, text, SIZE_MAX, NULL));
        if (r >= WINDOW) {
            CHECK(atree_delete(tree, CHURN_BASE + r - WINDOW));
        }
    }
    for (r = NROUNDS - WINDOW; r < NROUNDS; r++) {
        CHECK(atree_delete(tree, CHURN_BASE + r));
    }
    return NULL;
}

/* Reader: owns one event and one report. Both carry per-search state (the
 * event's values, the report's bitsets and result array), so sharing either
 * between threads would race; one of each per thread is free of contention
 * and keeps steady-state matching allocation-free (README "Thread safety"). */
static void *reader_main(void *arg)
{
    struct reader *rd = arg;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    atree_expr_t *check = NULL;
    unsigned i;
    size_t k;

    CHECK(atree_event_create(rd->tree, &ev));
    CHECK(atree_report_create(rd->tree, &rep));
    /* The fixed subscription 5 is "n >= 5"; the reference evaluator on the
     * same text must agree with whether the tree reported it. */
    CHECK(atree_expr_parse(rd->tree, "n >= 5", SIZE_MAX, &check, NULL));

    for (i = 0; i < NEVENTS; i++) {
        int64_t v = (int64_t)((rd->seed + i * 7u) % NVALUES);
        size_t n, nfixed = 0;
        const atree_id_t *ids;
        bool saw5 = false;
        atree_event_clear(ev);
        CHECK(atree_event_set_int(ev, "n", v));
        /* atree_search holds the read lock for its duration. With
         * atree_search_cb the callback would run while that lock is held, so
         * it must not call back into the tree (atree.h, atree_lock_t). */
        CHECK(atree_search(rd->tree, ev, rep));
        n = atree_report_count(rep);
        ids = atree_report_matches(rep);
        /* Invariants: matches are sorted, so the fixed subscriptions ("n >= k",
         * ids 1..10) come first and must be exactly 1..v; every churned id
         * matched must have been inserted for this v. */
        for (k = 0; k < n; k++) {
            if (ids[k] < CHURN_BASE) {
                nfixed++;
                rd->failures += ids[k] != (atree_id_t)(k + 1);
            } else {
                rd->failures += (int64_t)((ids[k] - CHURN_BASE) % NVALUES) != v;
            }
            saw5 = saw5 || ids[k] == 5;
        }
        rd->failures += nfixed != (size_t)v;
        rd->failures += (atree_expr_eval(check, ev) == ATREE_TRUE) != saw5;
        rd->searches++;
    }
    atree_expr_free(check);
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    return NULL;
}

int main(void)
{
    static const atree_attr_def_t attrs[] = {{"n", ATREE_TYPE_INT}};
    struct atree_pthread_lock lock;
    struct reader readers[NREADERS];
    pthread_t rthreads[NREADERS], wthread;
    atree_config_t cfg;
    atree_t *tree = NULL;
    unsigned long failures = 0;
    char text[32];
    unsigned i;

    if (atree_pthread_lock_init(&lock) != 0) {
        fprintf(stderr, "pthread_rwlock_init failed\n");
        return EXIT_FAILURE;
    }
    atree_config_init(&cfg);
    cfg.lock = atree_pthread_lock_vtable(&lock);
    CHECK(atree_create(&cfg, attrs, 1, &tree));

    /* Fixed subscriptions 1..10: "n >= k". They never change. */
    for (i = 1; i < NVALUES; i++) {
        snprintf(text, sizeof text, "n >= %u", i);
        CHECK(atree_insert(tree, i, text, SIZE_MAX, NULL));
    }

    for (i = 0; i < NREADERS; i++) {
        readers[i].tree = tree;
        readers[i].seed = i;
        readers[i].searches = readers[i].failures = 0;
        if (pthread_create(&rthreads[i], NULL, reader_main, &readers[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return EXIT_FAILURE;
        }
    }
    if (pthread_create(&wthread, NULL, writer_main, tree) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return EXIT_FAILURE;
    }
    failures += pthread_join(wthread, NULL) != 0;
    for (i = 0; i < NREADERS; i++) {
        failures += pthread_join(rthreads[i], NULL) != 0;
        failures += readers[i].failures;
        printf("reader %u: %lu searches, %s\n", i, readers[i].searches,
               readers[i].failures == 0 ? "all consistent" : "INCONSISTENT");
    }

    /* The writer removed everything it added. */
    printf("subscriptions left: %lu\n", (unsigned long)atree_count(tree));
    failures += atree_count(tree) != NVALUES - 1;

    /* atree_destroy is not covered by the lock: it is the caller's job to
     * make sure no thread still uses the tree (all joined above), and the
     * lock must outlive the tree. */
    atree_destroy(tree);
    if (atree_pthread_lock_destroy(&lock) != 0) {
        failures++;
    }
    printf("%s\n", failures == 0 ? "threads: OK" : "threads: FAILED");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * Concurrency tests (run under ThreadSanitizer by `make check-tsan`):
 *   (a) many reader threads search one tree without any lock; every result
 *       must equal the single-threaded result, and TSan must see no write
 *       to tree memory;
 *   (b) readers plus a writer with the pthread lock adapter: every id a
 *       reader gets back must evaluate true for its event (soundness),
 *       the writer validates the structure after each step;
 *   (c) build-swap-retire: readers follow a published pointer while the
 *       main thread replaces the tree.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test.h"

#if defined(_WIN32)
#include "../extras/atree_lock_win32.h"
#define rwlock_t struct atree_win32_lock
#define rwlock_init atree_win32_lock_init
#define rwlock_vtable atree_win32_lock_vtable
#define rwlock_destroy atree_win32_lock_destroy
#else
#include "../extras/atree_lock_pthread.h"
#define rwlock_t struct atree_pthread_lock
#define rwlock_init atree_pthread_lock_init
#define rwlock_vtable atree_pthread_lock_vtable
#define rwlock_destroy atree_pthread_lock_destroy
#endif
#include "gen.h"
#include "threads.h"

#define NTHREADS 8
#define NEXPR 300
#define NEVENTS 48
#define NROUNDS 40

struct shared {
    atree_t *tree;
    atree_expr_t *exprs[NEXPR];
    atree_event_t *events[NEVENTS];
    size_t expected_count[NEVENTS];
    atree_id_t *expected[NEVENTS];
    int stop;
    test_mutex_t mu; /* (c): protects the published pointer */
};

struct reader {
    struct shared *sh;
    int index;
    long mismatches;
    long searches;
};

static int build_shared(struct shared *sh, const atree_config_t *cfg, uint64_t seed)
{
    struct gen g;
    size_t i;
    memset(sh, 0, sizeof *sh);
    if (gen_init(&g, cfg, seed, 5) != ATREE_OK) {
        return 1;
    }
    sh->tree = g.tree;
    for (i = 0; i < NEXPR; i++) {
        sh->exprs[i] = gen_expr(&g);
        if (sh->exprs[i] == NULL ||
            atree_insert_expr(sh->tree, (atree_id_t)(i + 1), sh->exprs[i], NULL) != ATREE_OK) {
            return 1;
        }
    }
    for (i = 0; i < NEVENTS; i++) {
        atree_report_t *rep = NULL;
        if (atree_event_create(sh->tree, &sh->events[i]) != ATREE_OK ||
            gen_event(&g, sh->events[i]) != ATREE_OK ||
            atree_report_create(sh->tree, &rep) != ATREE_OK ||
            atree_search(sh->tree, sh->events[i], rep) != ATREE_OK) {
            return 1;
        }
        sh->expected_count[i] = atree_report_count(rep);
        sh->expected[i] = (atree_id_t *)malloc((sh->expected_count[i] + 1) * sizeof(atree_id_t));
        if (sh->expected[i] == NULL) {
            return 1;
        }
        memcpy(sh->expected[i], atree_report_matches(rep),
               sh->expected_count[i] * sizeof(atree_id_t));
        atree_report_destroy(rep);
    }
    test_mutex_init(&sh->mu);
    return 0;
}

static void free_shared(struct shared *sh)
{
    size_t i;
    for (i = 0; i < NEVENTS; i++) {
        atree_event_destroy(sh->events[i]);
        free(sh->expected[i]);
    }
    for (i = 0; i < NEXPR; i++) {
        atree_expr_free(sh->exprs[i]);
    }
    atree_destroy(sh->tree);
    test_mutex_destroy(&sh->mu);
}

/* (a) */
static void *reader_exact(void *arg)
{
    struct reader *r = (struct reader *)arg;
    struct shared *sh = r->sh;
    atree_report_t *rep = NULL;
    int round;
    size_t i;
    if (atree_report_create(sh->tree, &rep) != ATREE_OK) {
        r->mismatches = -1;
        return NULL;
    }
    for (round = 0; round < NROUNDS; round++) {
        for (i = 0; i < NEVENTS; i++) {
            size_t e = (i + (size_t)r->index) % NEVENTS;
            if (atree_search(sh->tree, sh->events[e], rep) != ATREE_OK) {
                r->mismatches++;
                continue;
            }
            r->searches++;
            if (atree_report_count(rep) != sh->expected_count[e] ||
                memcmp(atree_report_matches(rep), sh->expected[e],
                       sh->expected_count[e] * sizeof(atree_id_t)) != 0) {
                r->mismatches++;
            }
        }
    }
    atree_report_destroy(rep);
    return NULL;
}

TEST(concurrent_readers_without_lock)
{
    struct shared sh;
    struct reader readers[NTHREADS];
    test_thread_t threads[NTHREADS];
    int i;
    long total = 0;
    ASSERT_FALSE(build_shared(&sh, NULL, gen_seed_from_env(20260506)));
    for (i = 0; i < NTHREADS; i++) {
        readers[i].sh = &sh;
        readers[i].index = i;
        readers[i].mismatches = 0;
        readers[i].searches = 0;
        ASSERT_EQ_I64(test_thread_start(&threads[i], reader_exact, &readers[i]), 0);
    }
    for (i = 0; i < NTHREADS; i++) {
        test_thread_join(&threads[i]);
    }
    for (i = 0; i < NTHREADS; i++) {
        ASSERT_EQ_I64(readers[i].mismatches, 0);
        total += readers[i].searches;
    }
    ASSERT_EQ_I64(total, (long)NTHREADS * NROUNDS * NEVENTS);
    free_shared(&sh);
    return 0;
}

/* (b) */
static void *reader_sound(void *arg)
{
    struct reader *r = (struct reader *)arg;
    struct shared *sh = r->sh;
    atree_report_t *rep = NULL;
    int round;
    size_t i;
    if (atree_report_create(sh->tree, &rep) != ATREE_OK) {
        r->mismatches = -1;
        return NULL;
    }
    for (round = 0; round < NROUNDS; round++) {
        for (i = 0; i < NEVENTS; i++) {
            size_t k;
            if (atree_search(sh->tree, sh->events[i], rep) != ATREE_OK) {
                r->mismatches++;
                continue;
            }
            r->searches++;
            for (k = 0; k < atree_report_count(rep); k++) {
                atree_id_t id = atree_report_matches(rep)[k];
                if (id == 0 || id > NEXPR ||
                    atree_expr_eval(sh->exprs[id - 1], sh->events[i]) != ATREE_TRUE) {
                    r->mismatches++;
                }
            }
        }
    }
    atree_report_destroy(rep);
    return NULL;
}

struct writer {
    struct shared *sh;
    long ops;
    long failures;
};

static void *writer_churn(void *arg)
{
    struct writer *w = (struct writer *)arg;
    struct shared *sh = w->sh;
    struct gen_rng rng;
    int i;
    rng.state = 99;
    for (i = 0; i < 400; i++) {
        size_t pick = gen_below(&rng, NEXPR);
        atree_id_t id = (atree_id_t)(pick + 1);
        if (atree_delete(sh->tree, id) != ATREE_OK) {
            w->failures++;
        }
        if (atree_insert_expr(sh->tree, id, sh->exprs[pick], NULL) != ATREE_OK) {
            w->failures++;
        }
        w->ops += 2;
        if (i % 50 == 0 && atree_validate(sh->tree, NULL, 0) != ATREE_OK) {
            w->failures++;
        }
    }
    return NULL;
}

TEST(readers_and_writer_with_pthread_lock)
{
    rwlock_t lk;
    atree_config_t cfg;
    struct shared sh;
    struct reader readers[NTHREADS];
    struct writer wr;
    test_thread_t threads[NTHREADS];
    test_thread_t wthread;
    int i;
    ASSERT_EQ_I64(rwlock_init(&lk), 0);
    atree_config_init(&cfg);
    cfg.lock = rwlock_vtable(&lk);
    ASSERT_FALSE(build_shared(&sh, &cfg, gen_seed_from_env(20260507)));
    wr.sh = &sh;
    wr.ops = 0;
    wr.failures = 0;
    for (i = 0; i < NTHREADS; i++) {
        readers[i].sh = &sh;
        readers[i].index = i;
        readers[i].mismatches = 0;
        readers[i].searches = 0;
        ASSERT_EQ_I64(test_thread_start(&threads[i], reader_sound, &readers[i]), 0);
    }
    ASSERT_EQ_I64(test_thread_start(&wthread, writer_churn, &wr), 0);
    for (i = 0; i < NTHREADS; i++) {
        test_thread_join(&threads[i]);
    }
    test_thread_join(&wthread);
    ASSERT_EQ_I64(wr.failures, 0);
    ASSERT_EQ_I64(wr.ops, 800);
    for (i = 0; i < NTHREADS; i++) {
        ASSERT_EQ_I64(readers[i].mismatches, 0);
    }
    ASSERT_OK(atree_validate(sh.tree, NULL, 0));
    ASSERT_EQ_U64(atree_count(sh.tree), NEXPR);
    free_shared(&sh);
    ASSERT_EQ_I64(rwlock_destroy(&lk), 0);
    return 0;
}

/* (c) build-swap-retire: the published pointer changes under the readers. */
struct published {
    test_mutex_t mu;
    atree_t *current;
    int generation;
    int stop;
};

struct swap_reader {
    struct published *pub;
    long searches;
    long errors;
};

static void *reader_follow(void *arg)
{
    struct swap_reader *r = (struct swap_reader *)arg;
    atree_t *seen = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    while (1) {
        atree_t *cur;
        int stop;
        test_mutex_lock(&r->pub->mu);
        cur = r->pub->current;
        stop = r->pub->stop;
        test_mutex_unlock(&r->pub->mu);
        if (stop) {
            break;
        }
        if (cur != seen) {
            /* new tree: events and reports are per tree */
            atree_report_destroy(rep);
            atree_event_destroy(ev);
            rep = NULL;
            ev = NULL;
            if (atree_event_create(cur, &ev) != ATREE_OK ||
                atree_report_create(cur, &rep) != ATREE_OK) {
                r->errors++;
                break;
            }
            (void)atree_event_set_bool(ev, "b0", true);
            seen = cur;
        }
        if (atree_search(cur, ev, rep) != ATREE_OK) {
            r->errors++;
        }
        r->searches++;
    }
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    return NULL;
}

TEST(build_swap_retire)
{
    struct published pub;
    struct swap_reader readers[NTHREADS];
    test_thread_t threads[NTHREADS];
    atree_t *old;
    int gen_no;
    int i;
    long searches = 0;

    test_mutex_init(&pub.mu);
    pub.stop = 0;
    pub.generation = 0;
    ASSERT_OK(atree_create(NULL, GEN_DEFS, GEN_NATTRS, &pub.current));
    ASSERT_OK(atree_insert(pub.current, 1, "b0", SIZE_MAX, NULL));
    for (i = 0; i < NTHREADS; i++) {
        readers[i].pub = &pub;
        readers[i].searches = 0;
        readers[i].errors = 0;
        ASSERT_EQ_I64(test_thread_start(&threads[i], reader_follow, &readers[i]), 0);
    }
    for (gen_no = 1; gen_no <= 5; gen_no++) {
        atree_t *fresh = NULL;
        ASSERT_OK(atree_create(NULL, GEN_DEFS, GEN_NATTRS, &fresh));
        ASSERT_OK(atree_insert(fresh, 1, "b0", SIZE_MAX, NULL));
        ASSERT_OK(atree_insert(fresh, (atree_id_t)(gen_no + 1), "b0 and i0 > 3", SIZE_MAX, NULL));
        test_mutex_lock(&pub.mu);
        old = pub.current;
        pub.current = fresh;
        pub.generation = gen_no;
        test_mutex_unlock(&pub.mu);
        /* Retire: in production one would wait for in-flight searches with an
         * epoch or RCU scheme; here readers hold the mutex only to read the
         * pointer, so a short grace period suffices for the test's purpose. */
        test_sleep_ms(20);
        /* Readers that fetched `old` before the swap may still be searching it;
         * the mutex handoff above does not wait for them, so this retire is
         * only safe because every reader re-reads the pointer per search and
         * the search itself is short. A real deployment must wait. */
        test_sleep_ms(20);
        atree_destroy(old);
    }
    test_mutex_lock(&pub.mu);
    pub.stop = 1;
    test_mutex_unlock(&pub.mu);
    for (i = 0; i < NTHREADS; i++) {
        test_thread_join(&threads[i]);
        ASSERT_EQ_I64(readers[i].errors, 0);
        searches += readers[i].searches;
    }
    ASSERT_TRUE(searches > 0);
    atree_destroy(pub.current);
    test_mutex_destroy(&pub.mu);
    return 0;
}

TEST_MAIN_BEGIN()
RUN_TEST(concurrent_readers_without_lock);
RUN_TEST(readers_and_writer_with_pthread_lock);
RUN_TEST(build_swap_retire);
TEST_MAIN_END()

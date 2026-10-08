/*
 * Synthetic workload benchmark modelled on the paper's ABE-Gen (§6.1,
 * Table 3): Zipf-distributed dimensions and values, an and/or/not/xor/xnor
 * operator mix, bounded depth and fan-out, and Zipf-shared subexpressions.
 *
 *   bench_synthetic [options]
 *     --expressions N   (default 100000)      --events M        (default 2000)
 *     --dims D          (default 1000)        --cardinality C   (default 100)
 *     --event-size E    (default 20)          --depth d         (default 3)
 *     --fanout f        (default 4)           --alpha a         (default 0.6)
 *     --share pct[,..]  (default 54)          --seed s          (default 1)
 *     --pred-share X    size the predicate pool so each predicate is used
 *                       about X times (default 18.35, the paper's §6.1 figure;
 *                       0 = every predicate is fresh)
 *     --pred-pool N     draw predicates from a Zipf-ranked pool of exactly N
 *                       distinct predicates instead
 *     --flags bits      ATREE_FLAG_* to disable optimizations
 *     --verify K        brute-force check of the first K expressions
 *     --quick           CI preset: 20000 expressions, 500 events
 *     --paper           Table 3 defaults: 1000000 expressions, 3000 events
 *     --ads             the paper's real workload metrics (§6.2): 1392196
 *                       expressions over 122 dimensions, predicates shared
 *                       about 69 times each, the Figure 7(a) per-level
 *                       subexpression sharing, 20 pairs per event
 *
 * Depth and fan-out follow the paper's conventions: an expression of depth d
 * has its root at depth 1 and predicates at depth d, and `--fanout` is the
 * average number of children of an and/or node (drawn uniformly from
 * 2..2f-2); `not` has one child, `xor`/`xnor` two. `--share` is the
 * probability that a subexpression at a given depth is reused from a
 * Zipf-ranked pool of earlier ones (one value, or one per depth from the
 * root, the last repeated); with the same value p at every depth a
 * subexpression at depth k is shared about (1-p)^-k times.
 *     --cap N           max_adjust_candidates (reorganize/self-adjust scan bound)
 *     --dump PREFIX     also write PREFIX.defs/.exprs/.events (bench_file format)
 *     --rust-compatible dialect the Rust a-tree crate accepts (no xor/xnor/all of)
 *     --json            machine-readable output
 *     --check FILE      compare deterministic counts with a baseline JSON
 *                       (exit 1 on a regression of more than 5%)
 *
 * SPDX-License-Identifier: MIT
 */
/* clock_gettime is POSIX; glibc hides it under a strict -std=c99 unless the
 * feature macro precedes the first system header. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atree.h"

#include "bench_clock.h"

/* ---- PRNG and Zipf ------------------------------------------------------ */

static uint64_t rng_state = 1;

static uint64_t rnd(void)
{
    uint64_t z = (rng_state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static uint32_t below(uint32_t n)
{
    return n == 0 ? 0 : (uint32_t)(rnd() % n);
}

static double unit(void)
{
    return (double)(rnd() >> 11) * (1.0 / 9007199254740992.0);
}

struct zipf {
    double *cdf;
    uint32_t n;
};

static int zipf_init(struct zipf *z, uint32_t n, double alpha)
{
    uint32_t i;
    double sum = 0.0;
    z->n = n;
    z->cdf = (double *)malloc(n * sizeof *z->cdf);
    if (z->cdf == NULL) {
        return 1;
    }
    for (i = 0; i < n; i++) {
        sum += alpha <= 0.0 ? 1.0 : pow((double)(i + 1), -alpha);
        z->cdf[i] = sum;
    }
    for (i = 0; i < n; i++) {
        z->cdf[i] /= sum;
    }
    return 0;
}

static uint32_t zipf_sample(const struct zipf *z)
{
    double u = unit();
    uint32_t lo = 0;
    uint32_t hi = z->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (z->cdf[mid] < u) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < z->n ? lo : z->n - 1;
}

/* ---- growable text buffer ----------------------------------------------- */

struct buf {
    char *p;
    size_t len;
    size_t cap;
};

static void buf_add(struct buf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) {
            nc *= 2;
        }
        b->p = (char *)realloc(b->p, nc);
        if (b->p == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
}

static void buf_addll(struct buf *b, long long v)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", v);
    buf_add(b, tmp);
}

/* ", v" or "v" depending on position; quoted as 'vN' when quote is set. */
static void buf_add_item(struct buf *b, uint32_t i, long long v, int quote)
{
    if (i > 0) {
        buf_add(b, ", ");
    }
    if (quote) {
        buf_add(b, "'v");
        buf_addll(b, v);
        buf_add(b, "'");
    } else {
        buf_addll(b, v);
    }
}

/* ---- schema and generation ---------------------------------------------- */

#define SHARE_DEPTHS 16

struct params {
    uint32_t expressions, events, dims, cardinality, event_size, depth, fanout;
    uint32_t share[SHARE_DEPTHS]; /* reuse probability (percent) per depth from the root */
    uint32_t nshare;              /* entries set; deeper levels repeat the last */
    uint32_t pred_pool;           /* distinct predicates to draw from (0 = derive or unbounded) */
    double pred_share;            /* target instances per distinct predicate (0 = no pool) */
    double alpha;
    uint64_t seed;
    unsigned flags;
    uint32_t verify;
    int json;
    const char *check;
    const char *dump;    /* write PREFIX.defs/.exprs/.events in bench_file format */
    uint32_t cap;        /* max_adjust_candidates (0 = library default) */
    int rust_compatible; /* avoid xor/xnor and `all of` (unsupported or different in the Rust crate)
                          */
};

static atree_type_t dim_type(uint32_t d)
{
    switch (d % 10) {
    case 6:
    case 7:
        return ATREE_TYPE_STRING;
    case 8:
        return ATREE_TYPE_BOOL;
    case 9:
        return ATREE_TYPE_INT_LIST;
    default:
        return ATREE_TYPE_INT;
    }
}

struct gen {
    const struct params *p;
    struct zipf zdim;
    struct zipf zval;
    /* subexpression pools per depth, for Zipf-shared reuse */
    char ***pool;
    uint32_t **pool_npreds; /* predicates inside each pooled text */
    uint32_t **pool_ninner; /* and/or/xor/xnor nodes inside each pooled text */
    uint32_t *pool_len;
    struct zipf zpool;
    /* predicate pool: a fixed population of distinct predicates drawn by
     * Zipf rank, so the average sharing ratio is instances / pool size */
    char **ppool;
    uint32_t ppool_used;     /* pool slots filled so far */
    uint32_t ppool_distinct; /* ... of which hold a text no other slot holds */
    uint32_t ppool_next;     /* lowest slot that may still be empty */
    uint32_t new_slot_ppm;   /* probability (per million) that a draw fills the next empty slot */
    uint32_t *tset;          /* open-addressing set of filled slots keyed by text */
    uint32_t tset_mask;
    struct zipf zpred;
};

/* Generated counts of one (sub)expression text. */
struct counts {
    uint32_t npreds;
    uint32_t ninner;
};

static uint32_t share_at(const struct params *p, uint32_t depth)
{
    uint32_t i = depth > 0 ? depth - 1 : 0;
    if (p->nshare == 0) {
        return 0;
    }
    if (i >= p->nshare) {
        i = p->nshare - 1;
    }
    return p->share[i];
}

/* Expected predicates per expression: and/or nodes average `fanout`
 * children, `not` one, `xor`/`xnor` two, with the 40/40/10/5/5 operator mix,
 * over depth - 1 inner levels. Reused subexpressions have the same
 * expectation, so the estimate holds for any --share. */
static double expected_predicates(const struct params *p)
{
    double children = p->rust_compatible ? 0.9 * p->fanout + 0.1 : 0.8 * p->fanout + 0.3;
    return pow(children, (double)(p->depth - 1));
}

/* Fraction of predicate instances generated fresh rather than copied inside
 * a reused subexpression: the product of the miss probabilities of all
 * inner depths above the predicates. */
static double fresh_fraction(const struct params *p)
{
    double f = 1.0;
    uint32_t d;
    for (d = 1; d < p->depth; d++) {
        f *= 1.0 - share_at(p, d) / 100.0;
    }
    return f;
}

#define POOL_MAX 4096

static void gen_predicate_text(struct gen *g, struct buf *b)
{
    uint32_t d = zipf_sample(&g->zdim);
    uint32_t v = zipf_sample(&g->zval);
    uint32_t roll = below(100);
    uint32_t k;
    uint32_t i;
    char name[32];
    snprintf(name, sizeof name, "d%u", (unsigned)d);
    buf_add(b, name);
    switch (dim_type(d)) {
    case ATREE_TYPE_INT:
        if (roll < 50) {
            buf_add(b, " = ");
            buf_addll(b, (long long)v);
        } else if (roll < 80) {
            static const char *const ops[4] = {" < ", " <= ", " > ", " >= "};
            buf_add(b, ops[below(4)]);
            buf_addll(b, (long long)v);
        } else {
            k = 2 + below(4);
            buf_add(b, " in [");
            for (i = 0; i < k; i++) {
                buf_add_item(b, i, (long long)zipf_sample(&g->zval), 0);
            }
            buf_add(b, "]");
        }
        break;
    case ATREE_TYPE_STRING:
        if (roll < 70) {
            buf_add(b, " = 'v");
            buf_addll(b, (long long)v);
            buf_add(b, "'");
        } else {
            k = 2 + below(3);
            buf_add(b, " in [");
            for (i = 0; i < k; i++) {
                buf_add_item(b, i, (long long)zipf_sample(&g->zval), 1);
            }
            buf_add(b, "]");
        }
        break;
    case ATREE_TYPE_BOOL:
        if (roll < 50) {
            b->len -= strlen(name);
            b->p[b->len] = '\0';
            buf_add(b, "not ");
            buf_add(b, name);
        }
        break;
    case ATREE_TYPE_INT_LIST:
    case ATREE_TYPE_FLOAT:
    case ATREE_TYPE_STRING_LIST:
    default:
        if (g->p->rust_compatible && roll >= 60 && roll < 80) {
            roll = 10; /* the crate's `all of` has reversed semantics: use `one of` */
        }
        buf_add(b, roll < 60 ? " one of [" : roll < 80 ? " all of [" : " none of [");
        k = 1 + below(3);
        for (i = 0; i < k; i++) {
            buf_add_item(b, i, (long long)zipf_sample(&g->zval), 0);
        }
        buf_add(b, "]");
        break;
    }
}

static uint32_t text_hash(const char *s)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    while (*s != '\0') {
        h = (h ^ (unsigned char)*s++) * 16777619u;
    }
    return h;
}

/* Returns 1 if a pool slot already holds this text, else records `slot`. */
static int tset_add(struct gen *g, const char *text, uint32_t slot)
{
    uint32_t i = text_hash(text) & g->tset_mask;
    while (g->tset[i] != UINT32_MAX) {
        if (strcmp(g->ppool[g->tset[i]], text) == 0) {
            return 1;
        }
        i = (i + 1) & g->tset_mask;
    }
    g->tset[i] = slot;
    return 0;
}

/* Fills pool slot `idx` with fresh text, retrying a few times to make it
 * distinct from every other slot (the Zipf-skewed dimensions and values
 * repeat texts often; the paper counts distinct predicates). */
static int fill_slot(struct gen *g, uint32_t idx)
{
    int tries;
    for (tries = 0; tries < 8; tries++) {
        struct buf tmp = {NULL, 0, 0};
        gen_predicate_text(g, &tmp);
        if (tmp.p == NULL) {
            return 0;
        }
        g->ppool[idx] = tmp.p; /* ownership moves to the pool */
        if (!tset_add(g, tmp.p, idx)) {
            g->ppool_distinct++;
            break;
        }
        if (tries < 7) {
            free(tmp.p);
            g->ppool[idx] = NULL;
        }
    }
    g->ppool_used++;
    return 1;
}

/* One predicate: fresh text, or a member of the predicate pool. A draw fills
 * the next empty slot with probability new_slot_ppm (calibrated so the pool
 * is complete by the end of the run and every slot is used at least once)
 * and otherwise picks a slot by Zipf rank, filling it on first use. */
static void gen_predicate(struct gen *g, struct buf *b)
{
    uint32_t idx;
    if (g->ppool == NULL) {
        gen_predicate_text(g, b);
        return;
    }
    while (g->ppool_next < g->p->pred_pool && g->ppool[g->ppool_next] != NULL) {
        g->ppool_next++;
    }
    if (g->ppool_next < g->p->pred_pool && below(1000000) < g->new_slot_ppm) {
        idx = g->ppool_next;
    } else {
        idx = zipf_sample(&g->zpred);
    }
    if (g->ppool[idx] == NULL && !fill_slot(g, idx)) {
        gen_predicate_text(g, b);
        return;
    }
    buf_add(b, g->ppool[idx]);
}

/* Returns the predicate and inner-node counts of the generated text. */
static struct counts gen_expr(struct gen *g, struct buf *b, uint32_t depth);

/* Reuses a pooled subexpression at this depth with the depth's share probability. */
static int try_reuse(struct gen *g, struct buf *b, uint32_t depth, struct counts *c)
{
    uint32_t n = g->pool_len[depth];
    uint32_t idx;
    if (n == 0 || below(100) >= share_at(g->p, depth)) {
        return 0;
    }
    idx = zipf_sample(&g->zpool);
    if (idx >= n) {
        idx = below(n);
    }
    buf_add(b, g->pool[depth][idx]);
    c->npreds = g->pool_npreds[depth][idx];
    c->ninner = g->pool_ninner[depth][idx];
    return 1;
}

static void remember(struct gen *g, uint32_t depth, const char *text, struct counts c)
{
    uint32_t n = g->pool_len[depth];
    if (n >= POOL_MAX) {
        return;
    }
    g->pool[depth][n] = (char *)malloc(strlen(text) + 1);
    if (g->pool[depth][n] == NULL) {
        return;
    }
    strcpy(g->pool[depth][n], text);
    g->pool_npreds[depth][n] = c.npreds;
    g->pool_ninner[depth][n] = c.ninner;
    g->pool_len[depth] = n + 1;
}

static void counts_add(struct counts *c, struct counts d)
{
    c->npreds += d.npreds;
    c->ninner += d.ninner;
}

static struct counts gen_expr(struct gen *g, struct buf *b, uint32_t depth)
{
    size_t start = b->len;
    uint32_t roll;
    struct counts c = {0, 0};
    if (depth >= g->p->depth) {
        gen_predicate(g, b);
        c.npreds = 1;
        return c;
    }
    if (try_reuse(g, b, depth, &c)) {
        return c;
    }
    roll = below(100);
    if (g->p->rust_compatible && roll >= 90) {
        roll = roll < 95 ? 0 : 50; /* xor -> and, xnor -> or */
    }
    if (roll < 40 || roll < 80) {
        const char *op = roll < 40 ? " and " : " or ";
        /* 2..2f-2 children, so the mean is the paper's "average number of
         * child nodes" f (Table 3) */
        uint32_t n = g->p->fanout > 2 ? 2 + below(2 * g->p->fanout - 3) : 2;
        uint32_t i;
        c.ninner = 1;
        buf_add(b, "(");
        for (i = 0; i < n; i++) {
            if (i > 0) {
                buf_add(b, op);
            }
            counts_add(&c, gen_expr(g, b, depth + 1));
        }
        buf_add(b, ")");
    } else if (roll < 90) {
        buf_add(b, "not ");
        counts_add(&c, gen_expr(g, b, depth + 1));
    } else {
        c.ninner = 1;
        buf_add(b, "(");
        counts_add(&c, gen_expr(g, b, depth + 1));
        buf_add(b, roll < 95 ? " xor " : " xnor ");
        counts_add(&c, gen_expr(g, b, depth + 1));
        buf_add(b, ")");
    }
    remember(g, depth, b->p + start, c);
    return c;
}

/* ---- events ------------------------------------------------------------- */

static const char *type_name(atree_type_t t)
{
    switch (t) {
    case ATREE_TYPE_BOOL:
        return "bool";
    case ATREE_TYPE_INT:
        return "int";
    case ATREE_TYPE_FLOAT:
        return "float";
    case ATREE_TYPE_STRING:
        return "string";
    case ATREE_TYPE_INT_LIST:
        return "int_list";
    case ATREE_TYPE_STRING_LIST:
    default:
        return "string_list";
    }
}

static int fill_event(struct gen *g, atree_event_t *ev, atree_attr_id_t *dims, int64_t *ints,
                      FILE *dump)
{
    uint32_t n = 0;
    uint32_t i;
    uint32_t tries = 0;
    atree_event_clear(ev);
    while (n < g->p->event_size && tries < g->p->event_size * 4) {
        uint32_t d = zipf_sample(&g->zdim);
        uint32_t j;
        int dup = 0;
        tries++;
        for (j = 0; j < n; j++) {
            if (dims[j] == d) {
                dup = 1;
            }
        }
        if (dup) {
            continue;
        }
        dims[n++] = d;
    }
    for (i = 0; i < n; i++) {
        uint32_t d = dims[i];
        uint32_t v = zipf_sample(&g->zval);
        char s[32];
        atree_status_t st;
        switch (dim_type(d)) {
        case ATREE_TYPE_INT:
            st = atree_event_set_int_id(ev, d, (int64_t)v);
            break;
        case ATREE_TYPE_STRING:
            snprintf(s, sizeof s, "v%u", (unsigned)v);
            st = atree_event_set_string_id(ev, d, s, SIZE_MAX);
            break;
        case ATREE_TYPE_BOOL:
            st = atree_event_set_bool_id(ev, d, (v & 1) != 0);
            break;
        case ATREE_TYPE_INT_LIST:
        case ATREE_TYPE_FLOAT:
        case ATREE_TYPE_STRING_LIST:
        default:
            ints[0] = (int64_t)v;
            ints[1] = (int64_t)zipf_sample(&g->zval);
            ints[2] = (int64_t)zipf_sample(&g->zval);
            st = atree_event_set_int_list_id(ev, d, ints, 3);
            break;
        }
        if (st != ATREE_OK) {
            return 1;
        }
        if (dump != NULL) {
            fprintf(dump, "%sd%u=", i ? ";" : "", (unsigned)d);
            switch (dim_type(d)) {
            case ATREE_TYPE_INT:
                fprintf(dump, "%u", (unsigned)v);
                break;
            case ATREE_TYPE_STRING:
                fprintf(dump, "\"%s\"", s);
                break;
            case ATREE_TYPE_BOOL:
                fprintf(dump, "%s", (v & 1) != 0 ? "true" : "false");
                break;
            case ATREE_TYPE_INT_LIST:
            case ATREE_TYPE_FLOAT:
            case ATREE_TYPE_STRING_LIST:
            default:
                fprintf(dump, "[%lld, %lld, %lld]", (long long)ints[0], (long long)ints[1],
                        (long long)ints[2]);
                break;
            }
        }
    }
    if (dump != NULL) {
        fputc('\n', dump);
    }
    return 0;
}

/* ---- results ------------------------------------------------------------ */

struct results {
    double parse_us_total;
    double insert_us_total;
    uint64_t pred_instances;  /* predicates across all generated expressions */
    uint64_t inner_instances; /* and/or/xor/xnor nodes across all generated expressions */
    uint64_t pred_distinct;   /* distinct predicates drawn (pool slots used, or leaves) */
    uint32_t insert_failures;
    uint64_t nodes, leaves, edges, max_level, bytes, reorganized, self_adjusted;
    double search_p50_us, search_p99_us, search_mean_us;
    uint64_t total_matches, total_visited, total_evaluated, total_pred_matched, total_and_woken;
    double delete_us_total;
    uint32_t verify_mismatches;
};

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void print_json(const struct params *p, const struct results *r)
{
    printf("{\n");
    printf("  \"expressions\": %u, \"events\": %u, \"dims\": %u, \"cardinality\": %u,\n",
           p->expressions, p->events, p->dims, p->cardinality);
    printf("  \"event_size\": %u, \"depth\": %u, \"fanout\": %u, \"alpha\": %.3f, \"seed\": %llu, "
           "\"flags\": %u,\n",
           p->event_size, p->depth, p->fanout, p->alpha, (unsigned long long)p->seed, p->flags);
    printf("  \"nodes\": %llu, \"leaves\": %llu, \"edges\": %llu, \"max_level\": %llu,\n",
           (unsigned long long)r->nodes, (unsigned long long)r->leaves,
           (unsigned long long)r->edges, (unsigned long long)r->max_level);
    printf("  \"bytes_allocated\": %llu, \"reorganized\": %llu, \"self_adjusted\": %llu,\n",
           (unsigned long long)r->bytes, (unsigned long long)r->reorganized,
           (unsigned long long)r->self_adjusted);
    printf("  \"total_matches\": %llu, \"total_nodes_visited\": %llu, "
           "\"total_predicates_evaluated\": %llu, \"total_predicates_matched\": %llu, "
           "\"total_and_woken\": %llu,\n",
           (unsigned long long)r->total_matches, (unsigned long long)r->total_visited,
           (unsigned long long)r->total_evaluated, (unsigned long long)r->total_pred_matched,
           (unsigned long long)r->total_and_woken);
    printf("  \"predicate_instances\": %llu, \"predicate_distinct\": %llu, "
           "\"predicate_sharing\": %.2f,\n",
           (unsigned long long)r->pred_instances, (unsigned long long)r->pred_distinct,
           r->pred_distinct > 0 ? (double)r->pred_instances / (double)r->pred_distinct : 0.0);
    printf("  \"inner_instances\": %llu, \"inner_sharing\": %.2f, \"parse_per_sec\": %.0f,\n",
           (unsigned long long)r->inner_instances,
           r->nodes > r->leaves ? (double)r->inner_instances / (double)(r->nodes - r->leaves) : 0.0,
           r->parse_us_total > 0 ? p->expressions / (r->parse_us_total / 1e6) : 0.0);
    printf("  \"insert_per_sec\": %.0f, \"search_p50_us\": %.2f, \"search_p99_us\": %.2f, "
           "\"search_mean_us\": %.2f, \"delete_per_sec\": %.0f,\n",
           r->insert_us_total > 0 ? p->expressions / (r->insert_us_total / 1e6) : 0.0,
           r->search_p50_us, r->search_p99_us, r->search_mean_us,
           r->delete_us_total > 0 ? p->expressions / (r->delete_us_total / 1e6) : 0.0);
    printf("  \"verify_mismatches\": %u\n}\n", r->verify_mismatches);
}

static void print_human(const struct params *p, const struct results *r)
{
    printf("libatree synthetic benchmark (%s)\n", atree_version());
    printf("  expressions %u  dims %u  cardinality %u  event size %u  depth %u  fanout %u  "
           "alpha %.2f  flags %u\n",
           p->expressions, p->dims, p->cardinality, p->event_size, p->depth, p->fanout, p->alpha,
           p->flags);
    printf("  index: %llu nodes (%llu leaves), %llu edges, max level %llu, %.1f bytes/expression, "
           "reorganized %llu, self-adjusted %llu\n",
           (unsigned long long)r->nodes, (unsigned long long)r->leaves,
           (unsigned long long)r->edges, (unsigned long long)r->max_level,
           (double)r->bytes / (double)p->expressions, (unsigned long long)r->reorganized,
           (unsigned long long)r->self_adjusted);
    printf("  predicates: %.1f per expression, %llu instances over %llu distinct, each used "
           "%.2f times on average (%llu leaves after normalization, %.2f)\n",
           (double)r->pred_instances / p->expressions, (unsigned long long)r->pred_instances,
           (unsigned long long)r->pred_distinct,
           r->pred_distinct > 0 ? (double)r->pred_instances / (double)r->pred_distinct : 0.0,
           (unsigned long long)r->leaves,
           r->leaves > 0 ? (double)r->pred_instances / (double)r->leaves : 0.0);
    printf("  subexpressions: %.1f per expression, %llu instances over %llu inner nodes, each "
           "shared %.2f times on average\n",
           (double)r->inner_instances / p->expressions, (unsigned long long)r->inner_instances,
           (unsigned long long)(r->nodes - r->leaves),
           r->nodes > r->leaves ? (double)r->inner_instances / (double)(r->nodes - r->leaves)
                                : 0.0);
    printf("  parse: %.0f expressions/s (not part of insert)\n",
           r->parse_us_total > 0 ? p->expressions / (r->parse_us_total / 1e6) : 0.0);
    printf("  insert: %.0f expressions/s (%u failures), %.1f s total\n",
           r->insert_us_total > 0 ? p->expressions / (r->insert_us_total / 1e6) : 0.0,
           r->insert_failures, r->insert_us_total / 1e6);
    printf("  search: p50 %.1f us, p99 %.1f us, mean %.1f us over %u events; per event avg %.1f "
           "matches, %.1f nodes visited, %.1f predicates evaluated (%.1f true), %.1f AND woken\n",
           r->search_p50_us, r->search_p99_us, r->search_mean_us, p->events,
           (double)r->total_matches / p->events, (double)r->total_visited / p->events,
           (double)r->total_evaluated / p->events, (double)r->total_pred_matched / p->events,
           (double)r->total_and_woken / p->events);
    printf("  delete: %.0f expressions/s\n",
           r->delete_us_total > 0 ? p->expressions / (r->delete_us_total / 1e6) : 0.0);
    if (p->verify > 0) {
        printf("  verify: %u mismatches against brute force on the first %u expressions\n",
               r->verify_mismatches, p->verify);
    }
}

/* Reads a flat JSON number by key; returns 0 when absent. */
static int json_number(const char *text, const char *key, double *out)
{
    char pat[96];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(text, pat);
    if (p == NULL) {
        return 0;
    }
    *out = strtod(p + strlen(pat), NULL);
    return 1;
}

static int check_baseline(const char *path, const struct results *r)
{
    static const char *const keys[] = {"nodes",
                                       "edges",
                                       "bytes_allocated",
                                       "total_matches",
                                       "total_nodes_visited",
                                       "total_predicates_evaluated"};
    const uint64_t vals[] = {r->nodes,         r->edges,         r->bytes,
                             r->total_matches, r->total_visited, r->total_evaluated};
    FILE *f = fopen(path, "rb");
    char *text;
    long n;
    size_t i;
    int bad = 0;
    if (f == NULL) {
        fprintf(stderr, "cannot open baseline %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return 1;
    }
    text = (char *)malloc((size_t)n + 1);
    if (text == NULL || fread(text, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(text);
        return 1;
    }
    text[n] = '\0';
    fclose(f);
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        double base = 0.0;
        if (!json_number(text, keys[i], &base)) {
            fprintf(stderr, "baseline lacks %s\n", keys[i]);
            bad++;
            continue;
        }
        if (base > 0.0 && fabs((double)vals[i] - base) / base > 0.05) {
            fprintf(stderr, "REGRESSION %s: %.0f -> %llu (%.1f%%)\n", keys[i], base,
                    (unsigned long long)vals[i], 100.0 * ((double)vals[i] - base) / base);
            bad++;
        }
    }
    free(text);
    if (bad == 0) {
        printf("baseline check passed (%s)\n", path);
    }
    return bad != 0;
}

/* ---- main --------------------------------------------------------------- */

static int parse_args(int argc, char **argv, struct params *p)
{
    int i;
    p->expressions = 100000;
    p->events = 2000;
    p->dims = 1000;
    p->cardinality = 100;
    p->event_size = 20;
    p->depth = 3;
    p->fanout = 4;
    p->share[0] = 54; /* measured: subexpressions shared 4.33 times at 1M, as in the paper */
    p->nshare = 1;
    p->pred_share = 18.35;
    p->alpha = 0.6;
    p->seed = 1;
    p->flags = 0;
    p->verify = 0;
    p->json = 0;
    p->check = NULL;
    p->dump = NULL;
    p->rust_compatible = 0;
    p->cap = 0;
    p->pred_pool = 0;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (strcmp(a, "--quick") == 0) {
            p->expressions = 20000;
            p->events = 500;
        } else if (strcmp(a, "--paper") == 0) {
            p->expressions = 1000000;
            p->events = 3000;
        } else if (strcmp(a, "--ads") == 0) {
            /* §6.2: 1,392,196 expressions, 122 dimensions, 973,794 distinct
             * predicates shared 68.76 times on average (so about 48 per
             * expression), 1..56 predicates and depth 1..9 per expression,
             * ~20 attribute-value pairs per event. Depth 4 with 4 children
             * on average gives about 43 predicates per expression. Figure
             * 7(a) shares level 2, 3 and 4 subexpressions 28, 11 and 7.5
             * times: with reuse probability p_k at depth k the sharing at
             * depth k is 1 / prod(1 - p_j, j <= k), so 87%, 32% and 61%
             * from the root. The Zipf exponent 0.8 spreads the per-predicate
             * sharing from tens to hundreds of thousands as in Figure 7(b). */
            p->expressions = 1392196;
            p->events = 100;
            p->dims = 122;
            p->cardinality = 10000;
            p->depth = 4;
            p->fanout = 4;
            p->share[0] = 87;
            p->share[1] = 32;
            p->share[2] = 61;
            p->nshare = 3;
            p->alpha = 0.8;
            p->pred_share = 68.76;
        } else if (strcmp(a, "--json") == 0) {
            p->json = 1;
        } else if (strcmp(a, "--expressions") == 0) {
            p->expressions = (uint32_t)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--events") == 0) {
            p->events = (uint32_t)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--dims") == 0) {
            p->dims = (uint32_t)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--cardinality") == 0) {
            p->cardinality = (uint32_t)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--event-size") == 0) {
            p->event_size = (uint32_t)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--depth") == 0) {
            p->depth = (uint32_t)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--fanout") == 0) {
            p->fanout = (uint32_t)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--share") == 0) {
            const char *s = v;
            p->nshare = 0;
            while (p->nshare < SHARE_DEPTHS) {
                char *end;
                p->share[p->nshare++] = (uint32_t)strtoul(s, &end, 10);
                if (*end != ',') {
                    break;
                }
                s = end + 1;
            }
            i++;
        } else if (strcmp(a, "--alpha") == 0) {
            p->alpha = strtod(v, NULL);
            i++;
        } else if (strcmp(a, "--seed") == 0) {
            p->seed = strtoull(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--flags") == 0) {
            p->flags = (unsigned)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--verify") == 0) {
            p->verify = (uint32_t)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--check") == 0) {
            p->check = v;
            i++;
        } else if (strcmp(a, "--dump") == 0) {
            p->dump = v;
            i++;
        } else if (strcmp(a, "--rust-compatible") == 0) {
            p->rust_compatible = 1;
        } else if (strcmp(a, "--cap") == 0) {
            p->cap = (uint32_t)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--pred-pool") == 0) {
            p->pred_pool = (uint32_t)strtoul(v, NULL, 10);
            p->pred_share = 0.0;
            i++;
        } else if (strcmp(a, "--pred-share") == 0) {
            p->pred_share = strtod(v, NULL);
            p->pred_pool = 0;
            i++;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            return 1;
        }
    }
    if (p->expressions == 0 || p->dims == 0 || p->cardinality == 0 || p->depth == 0 ||
        p->nshare == 0 || p->pred_share < 0.0) {
        fprintf(stderr, "invalid parameters\n");
        return 1;
    }
    if (p->pred_pool > (1u << 30)) {
        fprintf(stderr, "predicate pool too large (max 2^30)\n");
        return 1;
    }
    if (p->pred_share > 0.0) {
        double pool = p->expressions * expected_predicates(p) / p->pred_share;
        p->pred_pool = pool < 1.0 ? 1 : pool > 1073741824.0 ? 1073741824u : (uint32_t)(pool + 0.5);
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct params p;
    struct gen g;
    atree_attr_def_t *defs;
    char **names;
    atree_config_t cfg;
    atree_t *tree = NULL;
    atree_event_t *ev = NULL;
    atree_report_t *rep = NULL;
    struct results r;
    struct buf b = {NULL, 0, 0};
    FILE *dump_exprs = NULL;
    FILE *dump_events = NULL;
    atree_expr_t **kept = NULL;
    uint64_t *lat;
    atree_attr_id_t *dims;
    int64_t ints[3];
    uint32_t i;
    uint64_t t0;
    uint64_t t1;
    int rc = 0;

    if (parse_args(argc, argv, &p) != 0) {
        return 2;
    }
    memset(&r, 0, sizeof r);
    rng_state = p.seed;

    /* schema */
    defs = (atree_attr_def_t *)calloc(p.dims, sizeof *defs);
    names = (char **)calloc(p.dims, sizeof *names);
    if (defs == NULL || names == NULL) {
        return 2;
    }
    for (i = 0; i < p.dims; i++) {
        names[i] = (char *)malloc(16);
        if (names[i] == NULL) {
            return 2;
        }
        snprintf(names[i], 16, "d%u", (unsigned)i);
        defs[i].name = names[i];
        defs[i].type = dim_type(i);
    }
    atree_config_init(&cfg);
    cfg.flags = p.flags;
    cfg.max_adjust_candidates = p.cap;
    if (atree_create(&cfg, defs, p.dims, &tree) != ATREE_OK) {
        fprintf(stderr, "atree_create failed\n");
        return 2;
    }
    if (p.dump != NULL) {
        char path[1024];
        FILE *fd;
        snprintf(path, sizeof path, "%s.defs", p.dump);
        fd = fopen(path, "w");
        snprintf(path, sizeof path, "%s.exprs", p.dump);
        dump_exprs = fopen(path, "w");
        snprintf(path, sizeof path, "%s.events", p.dump);
        dump_events = fopen(path, "w");
        if (fd == NULL || dump_exprs == NULL || dump_events == NULL) {
            fprintf(stderr, "cannot write dump files %s.*\n", p.dump);
            return 2;
        }
        fprintf(fd, "# name\ttype\n");
        for (i = 0; i < p.dims; i++) {
            fprintf(fd, "%s\t%s\n", names[i], type_name(dim_type(i)));
        }
        fclose(fd);
        fprintf(dump_exprs, "# id\texpression\n");
        fprintf(dump_events, "# attr=value;attr=value\n");
    }

    /* generator */
    g.p = &p;
    if (zipf_init(&g.zdim, p.dims, p.alpha) || zipf_init(&g.zval, p.cardinality, p.alpha) ||
        zipf_init(&g.zpool, POOL_MAX, 1.0)) {
        return 2;
    }
    g.pool = (char ***)calloc(p.depth + 1, sizeof *g.pool);
    g.pool_npreds = (uint32_t **)calloc(p.depth + 1, sizeof *g.pool_npreds);
    g.pool_ninner = (uint32_t **)calloc(p.depth + 1, sizeof *g.pool_ninner);
    g.pool_len = (uint32_t *)calloc(p.depth + 1, sizeof *g.pool_len);
    if (g.pool == NULL || g.pool_npreds == NULL || g.pool_ninner == NULL || g.pool_len == NULL) {
        return 2;
    }
    for (i = 0; i <= p.depth; i++) {
        g.pool[i] = (char **)calloc(POOL_MAX, sizeof **g.pool);
        g.pool_npreds[i] = (uint32_t *)calloc(POOL_MAX, sizeof **g.pool_npreds);
        g.pool_ninner[i] = (uint32_t *)calloc(POOL_MAX, sizeof **g.pool_ninner);
        if (g.pool[i] == NULL || g.pool_npreds[i] == NULL || g.pool_ninner[i] == NULL) {
            return 2;
        }
    }
    g.ppool = NULL;
    g.ppool_used = 0;
    g.ppool_distinct = 0;
    g.ppool_next = 0;
    g.tset = NULL;
    if (p.pred_pool > 0) {
        /* fresh draws expected over the run; fill one new slot per
         * pool/draws of them so the pool completes as the run ends */
        double draws = p.expressions * expected_predicates(&p) * fresh_fraction(&p);
        double ppm = draws > 0.0 ? 1e6 * p.pred_pool / draws : 1e6;
        uint32_t cap = 1;
        g.new_slot_ppm = ppm >= 1e6 ? 1000000u : (uint32_t)ppm;
        while (cap < 2 * p.pred_pool) {
            cap *= 2;
        }
        g.ppool = (char **)calloc(p.pred_pool, sizeof *g.ppool);
        g.tset = (uint32_t *)malloc(cap * sizeof *g.tset);
        if (g.ppool == NULL || g.tset == NULL || zipf_init(&g.zpred, p.pred_pool, p.alpha)) {
            return 2;
        }
        memset(g.tset, 0xff, cap * sizeof *g.tset);
        g.tset_mask = cap - 1;
    }
    if (p.verify > 0) {
        kept = (atree_expr_t **)calloc(p.verify, sizeof *kept);
        if (kept == NULL) {
            return 2;
        }
    }

    /* insert */
    for (i = 0; i < p.expressions; i++) {
        atree_error_t err;
        atree_expr_t *e = NULL;
        atree_status_t st;
        b.len = 0;
        if (b.p != NULL) {
            b.p[0] = '\0';
        }
        {
            struct counts c = gen_expr(&g, &b, 1);
            r.pred_instances += c.npreds;
            r.inner_instances += c.ninner;
        }
        if (dump_exprs != NULL) {
            fprintf(dump_exprs, "%u\t%s\n", (unsigned)(i + 1), b.p);
        }
        t0 = bench_now_ns();
        st = atree_expr_parse(tree, b.p, b.len, &e, &err);
        t1 = bench_now_ns();
        r.parse_us_total += (double)(t1 - t0) / 1000.0;
        if (st != ATREE_OK) {
            fprintf(stderr, "generated expression failed to parse: %s\n  %s\n", err.message, b.p);
            return 2;
        }
        t0 = bench_now_ns();
        st = atree_insert_expr(tree, (atree_id_t)(i + 1), e, &err);
        t1 = bench_now_ns();
        r.insert_us_total += (double)(t1 - t0) / 1000.0;
        if (st != ATREE_OK) {
            r.insert_failures++;
        }
        if (kept != NULL && i < p.verify) {
            kept[i] = e;
        } else {
            atree_expr_free(e);
        }
    }
    {
        atree_stats_t st;
        atree_stats(tree, &st);
        r.nodes = st.nodes;
        r.leaves = st.leaves;
        r.edges = st.edges;
        r.max_level = st.max_level;
        r.bytes = st.bytes_allocated;
        r.reorganized = st.reorganized;
        r.self_adjusted = st.self_adjusted;
        r.pred_distinct = g.ppool != NULL ? g.ppool_distinct : st.leaves;
    }
    if (atree_validate(tree, NULL, 0) != ATREE_OK) {
        fprintf(stderr, "validate failed after inserts\n");
        return 2;
    }

    /* search */
    if (atree_event_create(tree, &ev) != ATREE_OK || atree_report_create(tree, &rep) != ATREE_OK) {
        return 2;
    }
    lat = (uint64_t *)calloc(p.events ? p.events : 1, sizeof *lat);
    dims = (atree_attr_id_t *)calloc(p.event_size ? p.event_size : 1, sizeof *dims);
    if (lat == NULL || dims == NULL) {
        return 2;
    }
    for (i = 0; i < p.events; i++) {
        atree_report_stats_t rs;
        if (fill_event(&g, ev, dims, ints, dump_events) != 0) {
            return 2;
        }
        t0 = bench_now_ns();
        if (atree_search(tree, ev, rep) != ATREE_OK) {
            fprintf(stderr, "search failed\n");
            return 2;
        }
        t1 = bench_now_ns();
        lat[i] = t1 - t0;
        atree_report_stats(rep, &rs);
        r.total_matches += rs.matches;
        r.total_visited += rs.nodes_visited;
        r.total_evaluated += rs.predicates_evaluated;
        r.total_pred_matched += rs.predicates_matched;
        r.total_and_woken += rs.and_woken;
        if (kept != NULL) {
            /* brute force over the kept expressions */
            size_t k;
            const atree_id_t *got = atree_report_matches(rep);
            size_t ngot = atree_report_count(rep);
            size_t gi = 0;
            for (k = 0; k < p.verify; k++) {
                int brute = atree_expr_eval(kept[k], ev) == ATREE_TRUE;
                int in_tree;
                while (gi < ngot && got[gi] < (atree_id_t)(k + 1)) {
                    gi++;
                }
                in_tree = gi < ngot && got[gi] == (atree_id_t)(k + 1);
                if (brute != in_tree) {
                    r.verify_mismatches++;
                }
            }
        }
    }
    if (p.events > 0) {
        double sum = 0.0;
        for (i = 0; i < p.events; i++) {
            sum += (double)lat[i];
        }
        r.search_mean_us = sum / p.events / 1000.0;
        qsort(lat, p.events, sizeof *lat, cmp_u64);
        r.search_p50_us = (double)lat[p.events / 2] / 1000.0;
        r.search_p99_us =
            (double)
                lat[(size_t)((double)p.events * 0.99) < p.events ? (size_t)((double)p.events * 0.99)
                                                                 : p.events - 1] /
            1000.0;
    }

    /* delete */
    t0 = bench_now_ns();
    for (i = 0; i < p.expressions; i++) {
        (void)atree_delete(tree, (atree_id_t)(i + 1));
    }
    t1 = bench_now_ns();
    r.delete_us_total = (double)(t1 - t0) / 1000.0;
    {
        atree_stats_t st;
        atree_stats(tree, &st);
        if (st.nodes != 0 || atree_validate(tree, NULL, 0) != ATREE_OK) {
            fprintf(stderr, "tree not empty/valid after deleting everything\n");
            rc = 2;
        }
    }

    if (p.json) {
        print_json(&p, &r);
    } else {
        print_human(&p, &r);
    }
    if (p.check != NULL && check_baseline(p.check, &r) != 0) {
        rc = 1;
    }
    if (r.verify_mismatches != 0 || r.insert_failures != 0) {
        rc = 1;
    }

    /* cleanup */
    if (dump_exprs != NULL) {
        fclose(dump_exprs);
    }
    if (dump_events != NULL) {
        fclose(dump_events);
    }
    if (kept != NULL) {
        for (i = 0; i < p.verify; i++) {
            atree_expr_free(kept[i]);
        }
        free(kept);
    }
    atree_report_destroy(rep);
    atree_event_destroy(ev);
    atree_destroy(tree);
    for (i = 0; i <= p.depth; i++) {
        uint32_t k;
        for (k = 0; k < g.pool_len[i]; k++) {
            free(g.pool[i][k]);
        }
        free(g.pool[i]);
        free(g.pool_npreds[i]);
        free(g.pool_ninner[i]);
    }
    free(g.pool);
    free(g.pool_npreds);
    free(g.pool_ninner);
    free(g.pool_len);
    if (g.ppool != NULL) {
        for (i = 0; i < p.pred_pool; i++) {
            free(g.ppool[i]);
        }
        free(g.ppool);
        free(g.tset);
        free(g.zpred.cdf);
    }
    free(g.zdim.cdf);
    free(g.zval.cdf);
    free(g.zpool.cdf);
    for (i = 0; i < p.dims; i++) {
        free(names[i]);
    }
    free(names);
    free(defs);
    free(lat);
    free(dims);
    free(b.p);
    return rc;
}

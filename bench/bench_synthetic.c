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
 *     --share pct       (default 30)          --seed s          (default 1)
 *     --flags bits      ATREE_FLAG_* to disable optimizations
 *     --verify K        brute-force check of the first K expressions
 *     --quick           CI preset: 20000 expressions, 500 events
 *     --paper           Table 3 defaults: 1000000 expressions, 3000 events
 *     --json            machine-readable output
 *     --check FILE      compare deterministic counts with a baseline JSON
 *                       (exit 1 on a regression of more than 5%)
 *
 * SPDX-License-Identifier: MIT
 */
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

struct params {
    uint32_t expressions, events, dims, cardinality, event_size, depth, fanout, share_pct;
    double alpha;
    uint64_t seed;
    unsigned flags;
    uint32_t verify;
    int json;
    const char *check;
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
    uint32_t *pool_len;
    struct zipf zpool;
};

#define POOL_MAX 4096

static void gen_predicate(struct gen *g, struct buf *b)
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
        buf_add(b, roll < 60 ? " one of [" : roll < 80 ? " all of [" : " none of [");
        k = 1 + below(3);
        for (i = 0; i < k; i++) {
            buf_add_item(b, i, (long long)zipf_sample(&g->zval), 0);
        }
        buf_add(b, "]");
        break;
    }
}

static void gen_expr(struct gen *g, struct buf *b, uint32_t depth);

/* Reuses a pooled subexpression at this depth with probability share_pct. */
static int try_reuse(struct gen *g, struct buf *b, uint32_t depth)
{
    uint32_t n = g->pool_len[depth];
    uint32_t idx;
    if (n == 0 || below(100) >= g->p->share_pct) {
        return 0;
    }
    idx = zipf_sample(&g->zpool);
    if (idx >= n) {
        idx = below(n);
    }
    buf_add(b, g->pool[depth][idx]);
    return 1;
}

static void remember(struct gen *g, uint32_t depth, const char *text)
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
    g->pool_len[depth] = n + 1;
}

static void gen_expr(struct gen *g, struct buf *b, uint32_t depth)
{
    size_t start = b->len;
    uint32_t roll;
    if (depth >= g->p->depth) {
        gen_predicate(g, b);
        return;
    }
    if (try_reuse(g, b, depth)) {
        return;
    }
    roll = below(100);
    if (roll < 40 || roll < 80) {
        const char *op = roll < 40 ? " and " : " or ";
        uint32_t n = 2 + below(g->p->fanout > 1 ? g->p->fanout - 1 : 1);
        uint32_t i;
        buf_add(b, "(");
        for (i = 0; i < n; i++) {
            if (i > 0) {
                buf_add(b, op);
            }
            gen_expr(g, b, depth + 1);
        }
        buf_add(b, ")");
    } else if (roll < 90) {
        buf_add(b, "not ");
        gen_expr(g, b, depth + 1);
    } else {
        buf_add(b, "(");
        gen_expr(g, b, depth + 1);
        buf_add(b, roll < 95 ? " xor " : " xnor ");
        gen_expr(g, b, depth + 1);
        buf_add(b, ")");
    }
    remember(g, depth, b->p + start);
}

/* ---- events ------------------------------------------------------------- */

static int fill_event(struct gen *g, atree_event_t *ev, atree_attr_id_t *dims, int64_t *ints)
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
    }
    return 0;
}

/* ---- results ------------------------------------------------------------ */

struct results {
    double insert_us_total;
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
    printf("  insert: %.0f expressions/s (%u failures)\n",
           r->insert_us_total > 0 ? p->expressions / (r->insert_us_total / 1e6) : 0.0,
           r->insert_failures);
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
    p->share_pct = 30;
    p->alpha = 0.6;
    p->seed = 1;
    p->flags = 0;
    p->verify = 0;
    p->json = 0;
    p->check = NULL;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (strcmp(a, "--quick") == 0) {
            p->expressions = 20000;
            p->events = 500;
        } else if (strcmp(a, "--paper") == 0) {
            p->expressions = 1000000;
            p->events = 3000;
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
            p->share_pct = (uint32_t)strtoul(v, NULL, 10);
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
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            return 1;
        }
    }
    if (p->expressions == 0 || p->dims == 0 || p->cardinality == 0 || p->depth == 0) {
        fprintf(stderr, "invalid parameters\n");
        return 1;
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
    if (atree_create(&cfg, defs, p.dims, &tree) != ATREE_OK) {
        fprintf(stderr, "atree_create failed\n");
        return 2;
    }

    /* generator */
    g.p = &p;
    if (zipf_init(&g.zdim, p.dims, p.alpha) || zipf_init(&g.zval, p.cardinality, p.alpha) ||
        zipf_init(&g.zpool, POOL_MAX, 1.0)) {
        return 2;
    }
    g.pool = (char ***)calloc(p.depth + 1, sizeof *g.pool);
    g.pool_len = (uint32_t *)calloc(p.depth + 1, sizeof *g.pool_len);
    if (g.pool == NULL || g.pool_len == NULL) {
        return 2;
    }
    for (i = 0; i <= p.depth; i++) {
        g.pool[i] = (char **)calloc(POOL_MAX, sizeof **g.pool);
        if (g.pool[i] == NULL) {
            return 2;
        }
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
        gen_expr(&g, &b, 1);
        st = atree_expr_parse(tree, b.p, b.len, &e, &err);
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
        if (fill_event(&g, ev, dims, ints) != 0) {
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
    }
    free(g.pool);
    free(g.pool_len);
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

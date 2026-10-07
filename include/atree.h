/*
 * libatree — A-Tree: a dynamic index over arbitrary Boolean expressions.
 *
 * Implements Ji & Jacobsen, "A-Tree: A Dynamic Data Structure for Efficiently
 * Indexing Arbitrary Boolean Expressions", SIGMOD 2021.
 *
 * The tree indexes many Boolean expressions ("subscriptions") over a fixed set
 * of typed attributes so that one event (an assignment of values to some of
 * those attributes) retrieves every subscription it satisfies without
 * evaluating each one.
 *
 * Conventions
 *   - Every fallible function returns atree_status_t; ATREE_OK is 0.
 *   - The library never calls abort(), exit(), or prints. Diagnostics are
 *     returned through atree_error_t, a caller-owned fixed-size struct.
 *   - All memory comes from the allocator given at atree_create(); the library
 *     never calls malloc() directly except inside atree_default_allocator().
 *   - Strings are length-delimited. Where a length parameter may be SIZE_MAX,
 *     the string must be NUL-terminated and strlen() is used.
 *
 * Thread safety
 *   Functions taking `const atree_t *` are read paths and never write to tree
 *   memory. Any number of read paths may run concurrently on one tree. Write
 *   paths (atree_insert*, atree_delete, atree_destroy) require exclusion from
 *   all other calls, provided either by the lock in atree_config_t or by the
 *   caller. An atree_event_t or atree_report_t is used by one thread at a time.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATREE_H
#define ATREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Export macro                                                              */
/* ------------------------------------------------------------------------ */

#if defined(_WIN32) && defined(ATREE_SHARED)
#if defined(ATREE_BUILDING)
#define ATREE_API __declspec(dllexport)
#else
#define ATREE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define ATREE_API __attribute__((visibility("default")))
#else
#define ATREE_API
#endif

/* ------------------------------------------------------------------------ */
/* Version                                                                   */
/* ------------------------------------------------------------------------ */

#define ATREE_VERSION_MAJOR 0
#define ATREE_VERSION_MINOR 1
#define ATREE_VERSION_PATCH 0
#define ATREE_VERSION_STRING "0.1.0"

/* Returns ATREE_VERSION_STRING of the linked library. */
ATREE_API const char *atree_version(void);

/* ------------------------------------------------------------------------ */
/* Status codes and diagnostics                                              */
/* ------------------------------------------------------------------------ */

typedef enum atree_status {
    ATREE_OK = 0,
    ATREE_ERR_NOMEM,           /* the allocator returned NULL                        */
    ATREE_ERR_INVALID_ARG,     /* NULL where not allowed, bad enum value, bad size   */
    ATREE_ERR_SYNTAX,          /* DSL lexing/parsing failure                         */
    ATREE_ERR_UNKNOWN_ATTR,    /* attribute name is not defined on this tree         */
    ATREE_ERR_TYPE_MISMATCH,   /* operator/literal/value type vs attribute type      */
    ATREE_ERR_DUPLICATE_ATTR,  /* same attribute name defined twice                  */
    ATREE_ERR_DUPLICATE_ID,    /* subscription id already present                    */
    ATREE_ERR_NOT_FOUND,       /* subscription id not present                        */
    ATREE_ERR_TOO_DEEP,        /* expression nesting exceeds atree_config_t.max_depth */
    ATREE_ERR_LIMIT,           /* a capacity limit was reached (nodes, lists, ids)   */
    ATREE_ERR_INVALID_LITERAL, /* NaN/inf float, empty list, integer overflow, ...   */
    ATREE_ERR_CORRUPT,         /* atree_validate() found an inconsistency            */
    ATREE_ERR_CANCELLED        /* a caller-supplied callback asked to stop           */
} atree_status_t;

/* Human-readable, static, NUL-terminated description of a status code. */
ATREE_API const char *atree_strerror(atree_status_t status);

#define ATREE_ERROR_MESSAGE_MAX 256

/* Diagnostic detail. Caller-owned; filled by functions that accept it when
 * they return anything other than ATREE_OK. Never allocates. */
typedef struct atree_error {
    atree_status_t status;
    size_t offset; /* byte offset into the expression text, or SIZE_MAX if n/a */
    size_t length; /* length of the offending token, or 0                        */
    char message[ATREE_ERROR_MESSAGE_MAX]; /* NUL-terminated                       */
} atree_error_t;

/* ------------------------------------------------------------------------ */
/* Allocator                                                                 */
/* ------------------------------------------------------------------------ */

/* Every allocation the library makes goes through one of these. Contract:
 *   - alloc() is never called with size 0;
 *   - realloc() is never called with new_size 0 and never with ptr NULL;
 *   - free() is never called with ptr NULL;
 *   - old_size / size are exactly the sizes previously requested, so pool and
 *     arena allocators can rely on them;
 *   - any NULL return is reported as ATREE_ERR_NOMEM and leaves the tree in
 *     its state before the call. */
typedef struct atree_allocator {
    void *(*alloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void (*free)(void *ctx, void *ptr, size_t size);
    void *ctx;
} atree_allocator_t;

/* malloc/realloc/free from the C library. */
ATREE_API const atree_allocator_t *atree_default_allocator(void);

/* ------------------------------------------------------------------------ */
/* Optional lock                                                             */
/* ------------------------------------------------------------------------ */

/* Reader/writer lock injected like the allocator. When atree_config_t.lock is
 * non-NULL, every public call on the tree takes rdlock (read paths) or wrlock
 * (atree_insert*, atree_delete) for its duration, so the tree may be shared
 * between threads without further care. Callbacks (atree_match_fn,
 * atree_write_fn) run while the lock is held and must not call back into the
 * same tree. When NULL, the caller provides exclusion (see "Thread safety"). */
typedef struct atree_lock {
    void (*rdlock)(void *ctx);
    void (*rdunlock)(void *ctx);
    void (*wrlock)(void *ctx);
    void (*wrunlock)(void *ctx);
    void *ctx;
} atree_lock_t;

/* ------------------------------------------------------------------------ */
/* Attributes and configuration                                              */
/* ------------------------------------------------------------------------ */

typedef enum atree_type {
    ATREE_TYPE_BOOL,
    ATREE_TYPE_INT,        /* int64_t                        */
    ATREE_TYPE_FLOAT,      /* double; NaN in an event = undefined */
    ATREE_TYPE_STRING,     /* bytes, interned to an id        */
    ATREE_TYPE_INT_LIST,   /* sorted, unique int64_t list    */
    ATREE_TYPE_STRING_LIST /* sorted, unique string-id list  */
} atree_type_t;

typedef struct atree_attr_def {
    const char *name; /* NUL-terminated; copied; case-sensitive; not a DSL keyword */
    atree_type_t type;
} atree_attr_def_t;

typedef uint32_t atree_attr_id_t;
#define ATREE_ATTR_INVALID UINT32_MAX

/* Optimization switches. All optimizations are on by default; the flags exist
 * for differential testing and diagnosis. Disabling one never changes results. */
enum {
    ATREE_FLAG_NO_REORGANIZE = 1u << 0,            /* skip Alg. 2 (greedy set cover)   */
    ATREE_FLAG_NO_SELF_ADJUST = 1u << 1,           /* skip Alg. 3 (rewire parents)     */
    ATREE_FLAG_NO_PROPAGATION_ON_DEMAND = 1u << 2, /* every child wakes an AND node    */
    ATREE_FLAG_NO_PREDICATE_INDEX = 1u << 3        /* phase 1 evaluates every leaf     */
};

typedef struct atree_config {
    const atree_allocator_t *allocator; /* NULL -> atree_default_allocator()          */
    const atree_lock_t *lock;           /* NULL -> no internal locking                */
    unsigned flags;                     /* ATREE_FLAG_*                               */
    size_t max_depth;                   /* expression nesting limit; 0 -> 64          */
    size_t initial_nodes;               /* node capacity hint; 0 -> library default   */
    size_t max_adjust_candidates;       /* bound on reorganize/self-adjust candidate
                                           scans per node; 0 -> 4096                  */
    size_t max_expr_nodes;              /* nodes a normalized expression may have;
                                           0 -> 8192. XOR/XNOR expand to AND/OR and
                                           double per nesting level, so this, not
                                           max_depth, bounds the work one expression
                                           can cost; exceeding it fails the insert
                                           with ATREE_ERR_LIMIT                       */
} atree_config_t;

/* Fills *cfg with defaults (all NULL/0: defaults are applied at create time). */
ATREE_API void atree_config_init(atree_config_t *cfg);

typedef struct atree atree_t;

/* Creates a tree over the given attributes. cfg may be NULL for defaults.
 * Attribute names are copied. Fails with ATREE_ERR_DUPLICATE_ATTR,
 * ATREE_ERR_INVALID_ARG (empty name, keyword, bad type) or ATREE_ERR_NOMEM. */
ATREE_API atree_status_t atree_create(const atree_config_t *cfg, const atree_attr_def_t *attrs,
                                      size_t nattrs, atree_t **out);

/* Releases everything. tree may be NULL. Requires exclusion like a write. */
ATREE_API void atree_destroy(atree_t *tree);

ATREE_API atree_attr_id_t atree_attr_lookup(const atree_t *tree, const char *name);
ATREE_API size_t atree_attr_count(const atree_t *tree);
ATREE_API const char *atree_attr_name(const atree_t *tree, atree_attr_id_t id); /* NULL if bad */
ATREE_API atree_type_t atree_attr_type(const atree_t *tree, atree_attr_id_t id);

/* ------------------------------------------------------------------------ */
/* Expressions (programmatic builder)                                        */
/* ------------------------------------------------------------------------ */

/* An immutable expression tree owned by the caller. Built against a tree so
 * that attribute names and types are validated and the tree's allocator is
 * used. atree_insert_expr() copies what it needs. */
typedef struct atree_expr atree_expr_t;

typedef enum atree_op {
    ATREE_OP_LT,
    ATREE_OP_LE,
    ATREE_OP_GT,
    ATREE_OP_GE,
    ATREE_OP_EQ,
    ATREE_OP_NE
} atree_op_t;

typedef enum atree_list_op {
    ATREE_LIST_ONE_OF,  /* attribute list intersects literal list       */
    ATREE_LIST_NONE_OF, /* attribute list is disjoint from literal list */
    ATREE_LIST_ALL_OF   /* attribute list contains every literal        */
} atree_list_op_t;

typedef enum atree_null_op {
    ATREE_IS_NULL,     /* attribute undefined in the event (scalars)        */
    ATREE_IS_NOT_NULL, /* attribute defined (scalars)                       */
    ATREE_IS_EMPTY,    /* list attribute defined and empty                  */
    ATREE_IS_NOT_EMPTY /* list attribute defined and non-empty              */
} atree_null_op_t;

/* Leaf builders return NULL on unknown attribute, type mismatch, invalid
 * literal (NaN/inf, empty list) or out of memory. Use atree_expr_parse() when
 * a diagnostic is needed. Lists are copied, sorted and deduplicated. */
ATREE_API atree_expr_t *atree_expr_var(const atree_t *tree, const char *attr);
ATREE_API atree_expr_t *atree_expr_cmp_int(const atree_t *tree, const char *attr, atree_op_t op,
                                           int64_t value);
ATREE_API atree_expr_t *atree_expr_cmp_float(const atree_t *tree, const char *attr, atree_op_t op,
                                             double value);
ATREE_API atree_expr_t *atree_expr_eq_string(const atree_t *tree, const char *attr, bool equal,
                                             const char *s, size_t len);
ATREE_API atree_expr_t *atree_expr_in_ints(const atree_t *tree, const char *attr, bool in,
                                           const int64_t *values, size_t n);
ATREE_API atree_expr_t *atree_expr_in_strings(const atree_t *tree, const char *attr, bool in,
                                              const char *const *strings, const size_t *lens,
                                              size_t n);
ATREE_API atree_expr_t *atree_expr_list_ints(const atree_t *tree, const char *attr,
                                             atree_list_op_t op, const int64_t *values, size_t n);
ATREE_API atree_expr_t *atree_expr_list_strings(const atree_t *tree, const char *attr,
                                                atree_list_op_t op, const char *const *strings,
                                                const size_t *lens, size_t n);
ATREE_API atree_expr_t *atree_expr_null(const atree_t *tree, const char *attr, atree_null_op_t op);
ATREE_API atree_expr_t *atree_expr_true(const atree_t *tree);
ATREE_API atree_expr_t *atree_expr_false(const atree_t *tree);

/* Connectives take ownership of their children (freed with the result). A
 * NULL child, n == 0, out of memory, or a result nested deeper than the
 * tree's max_depth frees everything passed in and yields NULL, so builder
 * chains need only one NULL check at the end. */
ATREE_API atree_expr_t *atree_expr_and(atree_expr_t **children, size_t n);
ATREE_API atree_expr_t *atree_expr_or(atree_expr_t **children, size_t n);
ATREE_API atree_expr_t *atree_expr_not(atree_expr_t *child);
ATREE_API atree_expr_t *atree_expr_xor(atree_expr_t *a, atree_expr_t *b);
ATREE_API atree_expr_t *atree_expr_xnor(atree_expr_t *a, atree_expr_t *b);

/* Parses DSL text (see README "Expression language"). len may be SIZE_MAX. */
ATREE_API atree_status_t atree_expr_parse(const atree_t *tree, const char *text, size_t len,
                                          atree_expr_t **out, atree_error_t *err);
ATREE_API void atree_expr_free(atree_expr_t *expr); /* NULL ok */

/* Output callback: receives chunks of text; return nonzero to abort. */
typedef int (*atree_write_fn)(void *ctx, const char *data, size_t len);

/* Renders an expression back to DSL text. */
ATREE_API atree_status_t atree_expr_print(const atree_expr_t *expr, atree_write_fn fn, void *ctx);

typedef struct atree_event atree_event_t;

/* Three-valued result of the reference evaluator. */
typedef enum atree_tri { ATREE_FALSE = 0, ATREE_TRUE = 1, ATREE_UNDEFINED = 2 } atree_tri_t;

/* Reference (brute-force) evaluation of one expression against one event with
 * the paper's semantics (§3.2, Table 2). The tree matches a subscription iff
 * this returns ATREE_TRUE. Exported because it is useful for callers' tests.
 * A read path: takes the tree's read lock (string literals are resolved
 * through the tree), so do not call it from a callback that runs under the
 * lock. */
ATREE_API atree_tri_t atree_expr_eval(const atree_expr_t *expr, const atree_event_t *event);

/* ------------------------------------------------------------------------ */
/* Subscriptions                                                             */
/* ------------------------------------------------------------------------ */

typedef uint64_t atree_id_t;

/* Inserts DSL text under id. expr_len may be SIZE_MAX. Fails with
 * ATREE_ERR_DUPLICATE_ID if id is present. On any failure the tree is
 * unchanged. Several ids may share one expression. */
ATREE_API atree_status_t atree_insert(atree_t *tree, atree_id_t id, const char *expr,
                                      size_t expr_len, atree_error_t *err);
ATREE_API atree_status_t atree_insert_expr(atree_t *tree, atree_id_t id, const atree_expr_t *expr,
                                           atree_error_t *err);

/* Removes id; nodes no other subscription uses are released (Alg. 5). */
ATREE_API atree_status_t atree_delete(atree_t *tree, atree_id_t id);
ATREE_API bool atree_contains(const atree_t *tree, atree_id_t id);
ATREE_API size_t atree_count(const atree_t *tree); /* number of subscriptions */

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

/* An assignment of values to attributes. Every attribute starts undefined.
 * Reusable: buffers are kept across atree_event_clear(). Setting a value
 * validates the type against the attribute; strings are interned by lookup
 * against the tree (unknown strings equal nothing); lists are copied, sorted,
 * deduplicated; a float NaN is stored as undefined. */
ATREE_API atree_status_t atree_event_create(const atree_t *tree, atree_event_t **out);
ATREE_API void atree_event_destroy(atree_event_t *event); /* NULL ok */
ATREE_API void atree_event_clear(atree_event_t *event);   /* all attributes -> undefined */

ATREE_API atree_status_t atree_event_set_bool(atree_event_t *event, const char *attr, bool value);
ATREE_API atree_status_t atree_event_set_int(atree_event_t *event, const char *attr, int64_t value);
ATREE_API atree_status_t atree_event_set_float(atree_event_t *event, const char *attr,
                                               double value);
ATREE_API atree_status_t atree_event_set_string(atree_event_t *event, const char *attr,
                                                const char *s, size_t len);
ATREE_API atree_status_t atree_event_set_int_list(atree_event_t *event, const char *attr,
                                                  const int64_t *values, size_t n);
ATREE_API atree_status_t atree_event_set_string_list(atree_event_t *event, const char *attr,
                                                     const char *const *strings, const size_t *lens,
                                                     size_t n);
ATREE_API atree_status_t atree_event_set_undefined(atree_event_t *event, const char *attr);

/* Same, addressed by attribute id (from atree_attr_lookup); avoids a hash
 * lookup per call on the hot path. */
ATREE_API atree_status_t atree_event_set_bool_id(atree_event_t *event, atree_attr_id_t id,
                                                 bool value);
ATREE_API atree_status_t atree_event_set_int_id(atree_event_t *event, atree_attr_id_t id,
                                                int64_t value);
ATREE_API atree_status_t atree_event_set_float_id(atree_event_t *event, atree_attr_id_t id,
                                                  double value);
ATREE_API atree_status_t atree_event_set_string_id(atree_event_t *event, atree_attr_id_t id,
                                                   const char *s, size_t len);
ATREE_API atree_status_t atree_event_set_int_list_id(atree_event_t *event, atree_attr_id_t id,
                                                     const int64_t *values, size_t n);
ATREE_API atree_status_t atree_event_set_string_list_id(atree_event_t *event, atree_attr_id_t id,
                                                        const char *const *strings,
                                                        const size_t *lens, size_t n);
ATREE_API atree_status_t atree_event_set_undefined_id(atree_event_t *event, atree_attr_id_t id);

/* ------------------------------------------------------------------------ */
/* Search                                                                    */
/* ------------------------------------------------------------------------ */

/* Holds the result of one search and the scratch state a search needs
 * (bitsets, level queues). Reusable; create one per matching thread. A
 * warmed-up search performs no allocator calls. */
typedef struct atree_report atree_report_t;

ATREE_API atree_status_t atree_report_create(const atree_t *tree, atree_report_t **out);
ATREE_API void atree_report_destroy(atree_report_t *report); /* NULL ok */

/* Finds every subscription the event satisfies. The report is reset first. */
ATREE_API atree_status_t atree_search(const atree_t *tree, const atree_event_t *event,
                                      atree_report_t *report);

/* Matched ids, sorted ascending; valid until the next search or destroy. */
ATREE_API size_t atree_report_count(const atree_report_t *report);
ATREE_API const atree_id_t *atree_report_matches(const atree_report_t *report);

/* Work counters for the last search (deterministic; used by tests and tuning). */
typedef struct atree_report_stats {
    uint64_t predicates_evaluated; /* leaves evaluated in phase 1 (index probes + scans) */
    uint64_t predicates_matched;   /* leaves true for this event                         */
    uint64_t nodes_visited;        /* inner nodes dequeued in phase 2                    */
    uint64_t and_woken;            /* AND nodes dequeued                                 */
    uint64_t and_true;             /* ... of which evaluated true                        */
    uint64_t or_visited;           /* OR nodes dequeued                                  */
    uint64_t matches;              /* subscriptions matched                              */
} atree_report_stats_t;
ATREE_API void atree_report_stats(const atree_report_t *report, atree_report_stats_t *out);

/* Callback delivery. fn returns nonzero to stop early (search returns
 * ATREE_OK). scratch may be NULL, in which case temporary scratch is
 * allocated and freed. Ids are delivered in unspecified order. */
typedef int (*atree_match_fn)(void *ctx, atree_id_t id);
ATREE_API atree_status_t atree_search_cb(const atree_t *tree, const atree_event_t *event,
                                         atree_report_t *scratch, atree_match_fn fn, void *ctx);

/* True in *out if at least one subscription matches. */
ATREE_API atree_status_t atree_exists(const atree_t *tree, const atree_event_t *event,
                                      atree_report_t *scratch, bool *out);

/* Like atree_search but reports only ids present in allow[], which must be
 * sorted ascending. */
ATREE_API atree_status_t atree_search_ids(const atree_t *tree, const atree_event_t *event,
                                          atree_report_t *report, const atree_id_t *allow,
                                          size_t nallow);

/* ------------------------------------------------------------------------ */
/* Introspection                                                             */
/* ------------------------------------------------------------------------ */

typedef struct atree_stats {
    uint64_t subscriptions;
    uint64_t nodes, leaves, edges, max_level;
    uint64_t indexed_leaves, scanned_leaves; /* phase-1 routing            */
    uint64_t reorganized, self_adjusted;     /* cumulative since create    */
    uint64_t adjust_candidates_skipped;      /* hit max_adjust_candidates  */
    uint64_t strings;                        /* interned strings           */
    uint64_t bytes_allocated;                /* live bytes via allocator   */
    uint64_t bytes_peak;
} atree_stats_t;
ATREE_API void atree_stats(const atree_t *tree, atree_stats_t *out);

/* Writes the DAG in Graphviz DOT syntax through fn. */
ATREE_API atree_status_t atree_to_graphviz(const atree_t *tree, atree_write_fn fn, void *ctx);

/* Structural self-check: levels, parent/child symmetry, identity table,
 * index membership, use counts. Returns ATREE_ERR_CORRUPT and a message in
 * msg (if msg != NULL and cap > 0) on failure. */
ATREE_API atree_status_t atree_validate(const atree_t *tree, char *msg, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ATREE_H */

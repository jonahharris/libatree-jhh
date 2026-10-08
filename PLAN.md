# libatree — Implementation Plan

A standalone, portable C library implementing the **A-Tree** (Ji & Jacobsen,
*"A-Tree: A Dynamic Data Structure for Efficiently Indexing Arbitrary Boolean
Expressions"*, SIGMOD 2021) for matching events against very large sets of
arbitrary Boolean expressions. Same problem class and use case as `be-tree`,
packaged with the care of a reusable library: custom allocator, no undefined
behavior, no global state, no mandatory dependencies, C99.

This document is the single source of truth for scope, design, and sequencing.
`CLAUDE.md` holds the coding rules; `PROMPT.md` is the kickoff prompt.

---

## 0. Reference review (what we learned, what we keep, what we avoid)

All four references live in `./reference` and are read-only inputs.

### 0.1 The paper (`reference/ATree.pdf`)

Core ideas we implement faithfully:

| Concept | Paper section | Summary |
|---|---|---|
| Multirooted DAG | §4.1 | l-nodes (predicates, level 1), i-nodes (AND/OR subexpressions), r-nodes (expressions with subscriber ids). `level(N) = 1 + max(level(children))`. |
| Node uniqueness | §4.2.1 | Expression-to-node table keyed by a structural id. Operand order is irrelevant (commutativity). |
| Reorganize | §4.2.2, Alg. 2 | Greedy set cover: rewrite an incoming operand set in terms of existing nodes of the same operator. |
| Self-adjust | §4.2.3, Alg. 3 | When a new node's operand set is a strict subset of an existing parent's set (same operator), rewire that parent to use the new node. |
| Insert / delete | Alg. 4 / Alg. 5 | `useCount` per node; delete decrements and cascades to children when it hits 0. |
| Matching | §5, Alg. 6 | Phase 1 finds satisfied predicates; phase 2 is a level-synchronous bottom-up sweep with one queue per level. |
| Zero suppression | §5.2.1 | Push NOT to the leaves (De Morgan, flip operators, XOR/XNOR expansion). `false`/`undefined` results are never propagated; any operand without a result is `false`. |
| Propagation on demand | §5.2.2 | Each AND node designates one *access child*; only that child wakes the node, which then pulls its other operands' results. |
| 3-valued semantics | §3.2, Table 2 | A predicate on an attribute absent from the event is `undefined`. An expression matches only if it evaluates to `true`. |

### 0.2 Rust crate `reference/a-tree` (AntoineGagne/a-tree v0.5.1)

**Keep:** the public API shape (attribute definitions → tree; insert DSL
expression under an id; event builder validated against attribute types;
`search` → report of matched ids; `delete`; Graphviz export), the DSL
(operators, list literals, `is null`/`is empty`), interning strings to ids,
sorting + deduplicating list literals and event lists, a per-search result
bitset (three `u64` bitmaps: evaluated/success/failed), `level - 2` queue
indexing (predicates never need a queue), cost model
(constant < logarithmic × len < linear × len; AND +50, OR +60), Send/Sync
design (search takes `&self`).

**Avoid (bugs or limitations we must not replicate):**

1. **Identity by 64-bit hash alone** (`OptimizedNode::id()`), with no
   structural equality check. A hash collision silently merges two different
   expressions. We hash *and* compare structurally.
2. **Binary-only AND/OR.** `a and b and c` becomes `And(And(a,b),c)`, so
   `(a and b) and c` and `a and (b and c)` get different ids and are not
   shared; levels are deeper than necessary. We flatten to n-ary.
3. **No reorganize / self-adjust.** Sharing depends entirely on how the
   author parenthesized the expression.
4. **Phase 1 evaluates every leaf predicate for every event**
   (`process_predicates` iterates over `self.predicates`). This is O(#distinct
   predicates) per event and discards the main benefit of the structure. We
   build per-attribute predicate indexes.
5. **r-node ↔ i-node conversion** (`change_rnode_to_inode`) and separate
   `roots` vector: unnecessary state. A root is simply a node with
   subscriptions; a node may have both parents and subscriptions.
6. **Recursive insert and optimize** with no depth limit (stack overflow on
   adversarial input). We bound expression depth and keep recursion bounded or
   iterative.
7. **Deletion is O(n)** (`Vec::retain` over `roots`, `predicates`,
   `subscription_ids`). We use O(1)-amortized removal (swap-remove, hash
   maps).
8. **AND parents are only linked from their access child** (other children
   have no parent back-link). Fine for matching, awkward for deletion,
   validation, and self-adjust. We keep full structural parent links plus an
   explicit access-child designation.
9. `rust_decimal` floats; in C we use `double` with documented exact-equality
   semantics and NaN handling.
10. **`all of` is reversed**: the crate's `all_of(event, literal)` tests that
   every *event* element is in the literal list. be-tree (and the natural
   reading) test that every *literal* is in the event list. We follow
   be-tree: `attr all of [a, b]` ⇔ {a, b} ⊆ attr.
11. `is empty` on an undefined list attribute hits an `unreachable!()` panic
   in `NullOperator::evaluate`. We return undefined.

### 0.3 Embedded C A-Tree `reference/cep-atree` (atree.c, expr.c, cep.h)

**Keep (this is the closest design to what we want):** n-ary nodes with
children sorted by node id and deduplicated; FNV hash + full structural
equality in an intrusive chained hash table; per-attribute predicate indexes
(point: sorted array/equal range; rays: `x > c` / `x < c` as prefix/suffix of
a sorted array; interval: implicit interval tree over a lower-bound-sorted
array with lazily rebuilt `maxhi`; scan list for everything else);
`reorganize()` and `self_adjust()` implementations (candidates found among
the parents of the operands); `relevel()` worklist after rewiring; iterative
`delete_internal()` worklist; `wake_rank()` heuristic for the access child;
`tab_atree_config` flags that disable each optimization (invaluable for
differential testing); `tab_atree_validate()` structural self-check; stats
counters; normalization that also folds constants and collapses single-child
nodes; reference 3-valued evaluator `tab_expr_eval` used by tests.

**Avoid:** coupling to a host schema/value system (`tab_schema_t`,
`tab_value_t`, `tab_event_get`); a `pthread_mutex_t` inside the tree and
per-event *mutation* of nodes (`epoch`, `qepoch`, `result` fields) which
prevents concurrent searches; raw `malloc`/`realloc`/`calloc`; `uint32_t`
tags only; callback-only match results.

### 0.4 `reference/be-tree` (FrankBro/be-tree, MIT)

**Keep:** the user-facing workflow (define domains/attributes with types →
make sub from expression string → insert → make event → search → report with
matched ids and counters), the type set (bool, int, float, string, int list,
string list), `is null`/`is not null`/`is empty`, validating events and
expressions against the attribute table, minunit-style C tests, valgrind in
CI, `.clang-format`, MIT license.

Also worth borrowing: its warning set (`-Wshadow -Wfloat-equal -Wundef
-Wcast-align -Wwrite-strings -Wformat=2 -Wswitch-enum -Wswitch-default`),
WebKit-based `.clang-format` at 100 columns, `search_ids` (match only a
caller-supplied allow-list) and `exists` (stop at first match) as cheap API
conveniences, `true`/`false` literals in expressions, and `value in
list_attr` as sugar for `list_attr one of [value]`.

**Avoid (all observed in the code):**

- Compile-time allocator macros (`bmalloc` → `enif_alloc` under `#ifdef NIF`),
  a one-argument `bcalloc(count * size)` with no overflow check, many
  unchecked `malloc`s, and `abort()` on allocation failure.
- `abort()` on *user input*: malformed event text, unknown event attributes,
  unsplittable bounds. No error-code channel at all (bool/NULL plus
  `fprintf(stderr)`/`printf`), an `error.h` enum that nothing uses.
- Global state: the event parser returns its result through a file-scope
  `root`, so string-based search is not thread-safe.
- A union type-punning bug: `fill_event` overwrites a value's type tag with
  the domain type without converting, so an int in an event for a float
  domain is read as the bit pattern of a double.
- Signed overflow UB: `llabs(imax - imin)` on wide domains, `i ± 1` at
  `INT64` extremes, `float x = -DBL_MAX`, unchecked `strtoll` negation.
- Quadratic growth: `realloc(+1)` for nearly every array, O(n) removal
  inside move loops, linear `strcmp` scans for attribute and interner lookup.
- No delete (shared predicates are raw pointers in a red-black tree with no
  reference counts, so it was never implemented).
- Whole-library duplication for variants (`*_err`, `*_counting`), two
  divergent insert pipelines with different validation.
- Non-opaque `struct betree`; tests reach into internals; unprefixed exports
  (`make_report`, `parse`, `set_bit`, `make_environment`).
- flex/bison output committed to git with order-only make rules that never
  regenerate it; ad-tech "special" predicates and magic attribute names
  (`now`, `latitude`) baked into the grammar; GSL for benchmarks;
  benchmarks that reference data files that do not exist.
- Reports that accumulate across searches (no reset), matched ids in
  traversal order, `subs == NULL` when empty.
- No three-valued semantics: a missing attribute is `false`, so
  `not (x > 5)` matches when `x` is absent (the A-Tree paper and the Rust
  crate treat it as undefined → no match). We follow the paper.

### 0.5 `reference/bplus-tree` (indutny/bplus, MIT)

**Keep:** single public header, `prefix_` public vs `prefix__` internal
naming, uniform integer status returns, `void *ctx` first on every callback,
`extern "C"` guards, standalone test and bench executables, `-std=c99
-pedantic -Wall -Wextra`.

**Avoid:** pseudo-opaque structs via `*_PRIVATE` field macros (ABI leaks,
`<pthread.h>` in the public header, circular private includes); reserved
identifiers in include guards (`_BPLUS_H_`); unparenthesized macros; type
punning via `*(uint64_t *)(buf + off)`; `malloc`'d results the caller must
`free()` with libc; callbacks that cannot stop early; `assert()`-based tests
that vanish under `NDEBUG`; hardcoded `/tmp`; vendored C++ dependency.

---

## 1. Goals and non-goals

### Goals

1. **Correct** implementation of the A-Tree with all paper optimizations
   (uniqueness, reorganize, self-adjust, zero suppression, propagation on
   demand) plus per-attribute predicate indexes for phase 1.
2. **Standalone**: no dependencies beyond the C standard library. No
   generated code committed. Optional dev tooling only (sanitizers, valgrind,
   clang-format, libFuzzer).
3. **Portable**: strictly conforming C99 (also compiles as C11/C17, and the
   header compiles as C++). Linux/macOS/Windows (gcc, clang, MSVC), 32- and
   64-bit, any endianness. No POSIX requirements in the library itself.
4. **No undefined behavior**: clean under `-fsanitize=address,undefined`
   (with `-fno-sanitize-recover`), valgrind, `-Wall -Wextra -Wpedantic
   -Wconversion` and friends with `-Werror`. See `CLAUDE.md` for the rules.
5. **Custom allocator**: every allocation goes through a caller-provided
   allocator supplied at tree creation (alloc/realloc/free with sizes and a
   context pointer). The library never calls `malloc` directly except in the
   default allocator.
6. **No global state**; fully reentrant. Concurrent read-only use (search,
   event building, stats, export) of one tree from many threads is safe
   without locks because read paths never write to tree memory; mutation
   requires exclusion, provided either by the caller or by an optional
   caller-supplied lock vtable (§2.3, §9.1). Verified under ThreadSanitizer.
7. **Generally useful**: DSL *and* programmatic expression builder; both
   array-based and callback-based result delivery; reusable event and report
   objects for allocation-free steady state; stats; Graphviz export;
   structural validation; clear error reporting with positions.
8. **Well tested**: unit tests, differential tests against a reference
   evaluator over random expressions/events across all optimization flag
   combinations, insert/delete invariant tests, fuzzing of the parser,
   benchmarks.

### Non-goals (v1)

- Persistence/serialization of the tree.
- A built-in lock implementation or a thread pool. Locking is injected
  (§2.3) so the library stays free of platform headers; a ready-made
  pthread/SRWLOCK adapter ships in `extras/`, outside the library proper.
- Concurrent insert *during* search without a lock (RCU-style). The
  "build, swap pointer, retire old tree" pattern is documented instead.
- Ad-tech specific predicates (frequency caps, segments, geo). The predicate
  layer is designed so these could be added later without touching the tree.
- A C++ wrapper, language bindings, single-header amalgamation (possible
  later).
- Decimal floating point.

---

## 2. Public API

Single public header `include/atree.h`. All identifiers are prefixed
`atree_` / `ATREE_`. Internal identifiers are `atree__` or `static`.

### 2.1 Status codes and errors

```c
typedef enum atree_status {
    ATREE_OK = 0,
    ATREE_ERR_NOMEM,             /* allocator returned NULL                      */
    ATREE_ERR_INVALID_ARG,       /* NULL where not allowed, bad enum, bad size   */
    ATREE_ERR_SYNTAX,            /* DSL lexing/parsing failure                   */
    ATREE_ERR_UNKNOWN_ATTR,      /* attribute name not defined                   */
    ATREE_ERR_TYPE_MISMATCH,     /* operator/literal/value type vs attribute type */
    ATREE_ERR_DUPLICATE_ATTR,    /* same attribute name defined twice            */
    ATREE_ERR_DUPLICATE_ID,      /* subscription id already present              */
    ATREE_ERR_NOT_FOUND,         /* subscription id not present                  */
    ATREE_ERR_TOO_DEEP,          /* expression nesting exceeds the limit         */
    ATREE_ERR_LIMIT,             /* a capacity limit was reached (nodes, lists)  */
    ATREE_ERR_INVALID_LITERAL,   /* e.g. NaN/inf float, empty list, overflow     */
    ATREE_ERR_CORRUPT            /* atree_validate found an inconsistency        */
} atree_status_t;

const char *atree_strerror(atree_status_t status);

#define ATREE_ERROR_MESSAGE_MAX 256
typedef struct atree_error {
    atree_status_t status;
    size_t         offset;    /* byte offset into the expression, or SIZE_MAX */
    size_t         length;    /* length of the offending token, or 0          */
    char           message[ATREE_ERROR_MESSAGE_MAX]; /* NUL-terminated         */
} atree_error_t;
```

Every fallible function returns `atree_status_t`. Functions that take an
`atree_error_t *` fill it when non-NULL and the result is not `ATREE_OK`.
The error struct is caller-owned (stack) and never allocates.

### 2.2 Allocator

```c
typedef struct atree_allocator {
    void *(*alloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)(void *ctx, void *ptr, size_t size);
    void   *ctx;
} atree_allocator_t;

const atree_allocator_t *atree_default_allocator(void); /* libc malloc/realloc/free */
```

Rules: `alloc(0)` is never called by the library; `free(NULL, ...)` is never
called; `realloc` is never called with `new_size == 0`; sizes passed to
`free`/`realloc` are exactly the sizes previously requested (so pool and
arena allocators work). All size arithmetic is overflow-checked before the
call. Any `NULL` return is propagated as `ATREE_ERR_NOMEM` and leaves the
tree in its pre-call state (see §4.7, strong guarantee).

### 2.3 Attributes and configuration

```c
typedef enum atree_type {
    ATREE_TYPE_BOOL, ATREE_TYPE_INT, ATREE_TYPE_FLOAT, ATREE_TYPE_STRING,
    ATREE_TYPE_INT_LIST, ATREE_TYPE_STRING_LIST
} atree_type_t;

typedef struct atree_attr_def { const char *name; atree_type_t type; } atree_attr_def_t;

typedef uint32_t atree_attr_id_t;   /* index into the attribute table */
#define ATREE_ATTR_INVALID UINT32_MAX

enum {
    ATREE_FLAG_NO_REORGANIZE            = 1u << 0,
    ATREE_FLAG_NO_SELF_ADJUST           = 1u << 1,
    ATREE_FLAG_NO_PROPAGATION_ON_DEMAND = 1u << 2,
    ATREE_FLAG_NO_PREDICATE_INDEX       = 1u << 3  /* phase 1 scans every leaf */
};

/* Optional reader/writer lock, injected like the allocator. When non-NULL,
 * every public call takes rdlock (read paths) or wrlock (insert/delete) for
 * its duration, so the tree may be shared freely between threads. When NULL,
 * the caller is responsible for the exclusion rules in §2.9. */
typedef struct atree_lock {
    void (*rdlock)(void *ctx);
    void (*rdunlock)(void *ctx);
    void (*wrlock)(void *ctx);
    void (*wrunlock)(void *ctx);
    void  *ctx;
} atree_lock_t;

typedef struct atree_config {
    const atree_allocator_t *allocator;   /* NULL → atree_default_allocator() */
    const atree_lock_t      *lock;        /* NULL → no internal locking       */
    unsigned                 flags;       /* ATREE_FLAG_*                     */
    size_t                   max_depth;   /* 0 → 64                           */
    size_t                   initial_nodes; /* capacity hint, 0 → default     */
    size_t                   max_adjust_candidates; /* bound on reorganize/self-adjust
                                             candidate scans per node, 0 → 4096 (§4.4) */
} atree_config_t;

void atree_config_init(atree_config_t *cfg);   /* fills defaults */

typedef struct atree atree_t;                  /* opaque */

atree_status_t atree_create(const atree_config_t *cfg,
                            const atree_attr_def_t *attrs, size_t nattrs,
                            atree_t **out);
void           atree_destroy(atree_t *tree);

atree_attr_id_t atree_attr_lookup(const atree_t *tree, const char *name);
size_t          atree_attr_count(const atree_t *tree);
const char     *atree_attr_name(const atree_t *tree, atree_attr_id_t id);
atree_type_t    atree_attr_type(const atree_t *tree, atree_attr_id_t id);
```

Attribute names are copied. Names are case-sensitive, non-empty, and must not
collide with DSL keywords. The attribute set is fixed at creation (as in the
Rust crate and be-tree); this keeps events as dense arrays indexed by
attribute id.

### 2.4 Subscriptions (insert / delete)

```c
typedef uint64_t atree_id_t;

atree_status_t atree_insert(atree_t *tree, atree_id_t id,
                            const char *expr, size_t expr_len, /* SIZE_MAX → strlen */
                            atree_error_t *err);
atree_status_t atree_insert_expr(atree_t *tree, atree_id_t id,
                                 const atree_expr_t *expr, atree_error_t *err);
atree_status_t atree_delete(atree_t *tree, atree_id_t id);
bool           atree_contains(const atree_t *tree, atree_id_t id);
size_t         atree_count(const atree_t *tree);   /* number of subscriptions */
```

Inserting an id that already exists fails with `ATREE_ERR_DUPLICATE_ID`.
Several ids may share one expression (they attach to the same node).

### 2.5 Programmatic expression builder

For callers that already have a parsed/structured condition and do not want
to render a DSL string. Expressions are immutable trees owned by the caller;
`atree_insert_expr` copies what it needs.

```c
typedef struct atree_expr atree_expr_t;   /* opaque */

typedef enum atree_op      { ATREE_OP_LT, ATREE_OP_LE, ATREE_OP_GT, ATREE_OP_GE, ATREE_OP_EQ, ATREE_OP_NE } atree_op_t;
typedef enum atree_list_op { ATREE_LIST_ONE_OF, ATREE_LIST_NONE_OF, ATREE_LIST_ALL_OF } atree_list_op_t;
typedef enum atree_null_op { ATREE_IS_NULL, ATREE_IS_NOT_NULL, ATREE_IS_EMPTY, ATREE_IS_NOT_EMPTY } atree_null_op_t;

/* Leaves. Attribute by name; validated against the tree at build time
 * (unknown attribute / type mismatch → NULL; use atree_expr_parse for diagnostics). */
atree_expr_t *atree_expr_var(const atree_t *, const char *attr);                 /* bool attr */
atree_expr_t *atree_expr_cmp_int(const atree_t *, const char *attr, atree_op_t op, int64_t v);   /* < <= > >= = <> */
atree_expr_t *atree_expr_cmp_float(const atree_t *, const char *attr, atree_op_t op, double v);
atree_expr_t *atree_expr_eq_string(const atree_t *, const char *attr, bool equal, const char *s, size_t len);
atree_expr_t *atree_expr_in_ints(const atree_t *, const char *attr, bool in, const int64_t *v, size_t n);
atree_expr_t *atree_expr_in_strings(const atree_t *, const char *attr, bool in, const char *const *s, const size_t *lens, size_t n);
atree_expr_t *atree_expr_list_ints(const atree_t *, const char *attr, atree_list_op_t op, const int64_t *v, size_t n);   /* one_of none_of all_of */
atree_expr_t *atree_expr_list_strings(const atree_t *, const char *attr, atree_list_op_t op, const char *const *s, const size_t *lens, size_t n);
atree_expr_t *atree_expr_null(const atree_t *, const char *attr, atree_null_op_t op);  /* is_null is_not_null is_empty is_not_empty */

/* Connectives. Children are taken over (freed with the result). A NULL child
 * or NULL result (OOM) frees everything passed in and yields NULL. */
atree_expr_t *atree_expr_and(atree_expr_t **children, size_t n);
atree_expr_t *atree_expr_or(atree_expr_t **children, size_t n);
atree_expr_t *atree_expr_not(atree_expr_t *child);
atree_expr_t *atree_expr_xor(atree_expr_t *a, atree_expr_t *b);   /* expanded at normalization (§4.3) */
atree_expr_t *atree_expr_xnor(atree_expr_t *a, atree_expr_t *b);
atree_expr_t *atree_expr_true(void);
atree_expr_t *atree_expr_false(void);

atree_status_t atree_expr_parse(const atree_t *, const char *text, size_t len,
                                atree_expr_t **out, atree_error_t *err);
void           atree_expr_free(atree_expr_t *);
atree_status_t atree_expr_print(const atree_expr_t *, atree_write_fn, void *ctx); /* back to DSL */

/* Reference evaluator (3-valued). Used by tests; exported because it is useful. */
typedef enum atree_tri { ATREE_FALSE = 0, ATREE_TRUE = 1, ATREE_UNDEFINED = 2 } atree_tri_t;
atree_tri_t atree_expr_eval(const atree_expr_t *, const atree_event_t *);
```

The builder allocates with the tree's allocator (hence the `const atree_t *`
parameter), so caller code never mixes allocators. Because the parameter is
const, string literals are **not** interned at build time: the expression
keeps raw bytes and `atree_insert_expr` interns them (write path), while
`atree_expr_eval` resolves them by lookup. This is what makes building and
parsing expressions safe from reader threads.

### 2.6 Events

```c
typedef struct atree_event atree_event_t;   /* opaque, reusable */

atree_status_t atree_event_create(const atree_t *tree, atree_event_t **out);
void           atree_event_destroy(atree_event_t *ev);
void           atree_event_clear(atree_event_t *ev);     /* all attributes → undefined */

/* By name (convenience) ... */
atree_status_t atree_event_set_bool(atree_event_t *, const char *attr, bool v);
atree_status_t atree_event_set_int(atree_event_t *, const char *attr, int64_t v);
atree_status_t atree_event_set_float(atree_event_t *, const char *attr, double v);
atree_status_t atree_event_set_string(atree_event_t *, const char *attr, const char *s, size_t len);
atree_status_t atree_event_set_int_list(atree_event_t *, const char *attr, const int64_t *v, size_t n);
atree_status_t atree_event_set_string_list(atree_event_t *, const char *attr, const char *const *s, const size_t *lens, size_t n);
atree_status_t atree_event_set_undefined(atree_event_t *, const char *attr);
/* ... and by attribute id (hot path): atree_event_set_bool_id(ev, id, v), etc. */
```

Semantics: every attribute starts `undefined`. Setting a value validates the
type against the attribute table. Strings are interned against the tree's
string table at set time (unknown strings map to a sentinel id that equals
nothing). Lists are copied, sorted, and deduplicated. A float `NaN` value is
treated as `undefined`. The event keeps its buffers across `clear()` so
steady-state use is allocation-free.

### 2.7 Search and reports

```c
typedef struct atree_report atree_report_t;  /* opaque, reusable; owns search scratch state */

atree_status_t atree_report_create(const atree_t *tree, atree_report_t **out);
void           atree_report_destroy(atree_report_t *);

atree_status_t atree_search(const atree_t *tree, const atree_event_t *ev, atree_report_t *report);

size_t            atree_report_count(const atree_report_t *);
const atree_id_t *atree_report_matches(const atree_report_t *);  /* valid until next search/destroy */
typedef struct atree_report_stats {
    uint64_t predicates_evaluated;  /* leaves evaluated in phase 1 (indexed probes + scans) */
    uint64_t predicates_matched;    /* leaves true for this event */
    uint64_t nodes_visited;         /* inner nodes dequeued in phase 2 */
    uint64_t and_woken, and_true;   /* propagation-on-demand hit rate */
    uint64_t or_visited;
    uint64_t matches;
} atree_report_stats_t;
void atree_report_stats(const atree_report_t *, atree_report_stats_t *out);

/* Callback variant: returns nonzero from fn to stop early (search returns ATREE_OK). */
typedef int (*atree_match_fn)(void *ctx, atree_id_t id);
atree_status_t atree_search_cb(const atree_t *tree, const atree_event_t *ev,
                               atree_report_t *scratch /* may be NULL → temp alloc */,
                               atree_match_fn fn, void *ctx);

/* Conveniences (thin wrappers over atree_search_cb): */
atree_status_t atree_exists(const atree_t *, const atree_event_t *, atree_report_t *scratch, bool *out);
atree_status_t atree_search_ids(const atree_t *, const atree_event_t *, atree_report_t *report,
                                const atree_id_t *allow, size_t nallow /* sorted ascending */);
```

Matches are reported in deterministic order (sorted ascending by id before
return; cheap because the match list is small). The report object owns the
per-search bitsets and level queues, sized to the tree's node capacity and
grown on demand; the tree is never mutated by a search.

### 2.8 Introspection

```c
typedef struct atree_stats {
    uint64_t subscriptions, nodes, leaves, edges, max_level;
    uint64_t indexed_leaves, scanned_leaves;  /* phase-1 routing */
    uint64_t reorganized, self_adjusted;      /* cumulative since create */
    uint64_t strings;                         /* interned strings */
    uint64_t bytes_allocated;                 /* live bytes via allocator */
} atree_stats_t;
void atree_stats(const atree_t *, atree_stats_t *out);

typedef int (*atree_write_fn)(void *ctx, const char *data, size_t len);  /* nonzero → abort */
atree_status_t atree_to_graphviz(const atree_t *, atree_write_fn fn, void *ctx);
atree_status_t atree_validate(const atree_t *, char *msg, size_t cap);    /* ATREE_ERR_CORRUPT on failure */

const char *atree_version(void);     /* "0.1.0" */
#define ATREE_VERSION_MAJOR 0 ...
```

### 2.9 Thread safety contract (documented in the header)

- **Readers never write to tree memory.** Every read-path function takes
  `const atree_t *` and the implementation never casts the const away; all
  per-call state lives in the caller's `atree_event_t` / `atree_report_t`.
  This is what makes lock-free concurrent reads sound, and it is verified
  by a ThreadSanitizer job (§6.6).
- One tree: any number of concurrent *readers* (`atree_search`,
  `atree_search_cb`, `atree_exists`, `atree_search_ids`, `atree_event_*`,
  `atree_report_*`, `atree_stats`, `atree_to_graphviz`, `atree_validate`,
  `atree_attr_*`, `atree_contains`, `atree_count`, `atree_expr_*` builders)
  **or** one *writer* (`atree_insert*`, `atree_delete`, `atree_destroy`).
- Exclusion between readers and the writer comes from one of: (a) a
  `atree_lock_t` passed in the config (the library then takes it for the
  duration of every call; callbacks run while the lock is held and must not
  re-enter the tree); (b) the caller's own synchronization, which must
  establish a happens-before edge between the end of a write and the start
  of subsequent reads (any mutex/rwlock does); (c) the build-swap-retire
  pattern: fill a new tree, publish its pointer with an atomic store, let
  in-flight searches on the old tree drain, destroy it.
- An `atree_event_t` or `atree_report_t` is owned by one thread at a time.
  Create one per matching thread; each is reusable indefinitely. Each carries
  its own allocator counters (same allocator, separate `struct atree__mem`),
  so creating or growing one from a reader thread never writes to the tree;
  `atree_stats.bytes_allocated` therefore covers the tree alone.
- In steady state (reusable event/report, no growth) a search performs no
  allocator calls. Event/report creation and growth do call the allocator
  from reader threads, so a custom allocator must be thread-safe if the
  library is used from several threads.

---

## 3. DSL

Hand-written lexer and parser (no flex/bison). Compatible with the Rust
crate's language so existing expressions port unchanged; keywords are
case-insensitive, attribute names are case-sensitive.

```
expr      := or_expr
or_expr   := xor_expr ( ("or" | "||") xor_expr )*
xor_expr  := and_expr ( ("xor" | "xnor") and_expr )*       -- left-assoc; paper §3.1 operators
and_expr  := not_expr ( ("and" | "&&") not_expr )*
not_expr  := ("not" | "!") not_expr | primary
primary   := "(" expr ")"
           | ident                                   -- boolean attribute
           | ident cmp_op number | number cmp_op ident  -- < <= > >= (reversed form flips op)
           | ident eq_op literal | literal eq_op ident  -- = <>
           | ident ("in" | "not in") list
           | ident ("one of" | "none of" | "all of") list
           | ident ("is null" | "is not null" | "is empty" | "is not empty")
           | "true" | "false"                        -- constants (folded at normalization)
           | literal "in" ident                      -- sugar: list_attr one of [literal]  (be-tree compatibility)
           | ident "between" number "and" number     -- paper §3.1: lo <= ident and ident <= hi (two leaves)
list      := "[" items "]" | "(" items ")"            -- non-empty, homogeneous (all int or all string)
literal   := integer | float | string
integer   := "-"? [0-9]+                              -- must fit int64_t, else ATREE_ERR_INVALID_LITERAL
float     := "-"? [0-9]+ "." [0-9]* ( [eE] [+-]? [0-9]+ )?   -- finite only
string    := '"' ( "\\" . | [^"\\] )* '"' | "'" ( "\\" . | [^'\\] )* "'"   -- escapes: \\ \" \' \n \t
ident     := [A-Za-z_][A-Za-z0-9_-]*                  -- not a keyword
```

Type rules (validated at parse time against the attribute table):

| Predicate | Allowed attribute types |
|---|---|
| bare `ident` | bool |
| `< <= > >=` | int (int literal), float (int or float literal; ints promote) |
| `= <>` | int, float, string |
| `in` / `not in` | int (int list), string (string list) |
| `one of` / `none of` / `all of` | int_list (int list), string_list (string list). `all of`: every literal is in the attribute's list (be-tree semantics) |
| `is null` / `is not null` | bool, int, float, string |
| `is empty` / `is not empty` | int_list, string_list |

Errors carry byte offset, token length, and a message like
`expected ']' or ',' after list element`.

---

## 4. Internal design

### 4.1 Source layout

```
include/atree.h            public header (self-contained, C99, C++-safe)
src/atree_internal.h       shared internal declarations (structs below)
src/alloc.c/.h             allocator wrappers: atree__alloc/realloc/free, overflow-checked array helpers, live-bytes counter
src/vec.h                  typed small-vector macros (ptr,len,cap) over the allocator; swap-remove, sorted insert
src/hash.c/.h              open-addressing hash maps: u64→u32 (expr hash→node, sub id→node), string→u32 (interning), value→leaf lists (indexes)
src/strtab.c/.h            string table: intern(bytes,len)→id, lookup; id 0 = "unknown" sentinel; ids never reused
src/attr.c/.h              attribute table (names, types, lookup)
src/value.c/.h             attribute values (tagged union), list sort/dedup, comparisons, double normalization/hash
src/predicate.c/.h         predicate struct (attr, kind, operand), negate(), eval(event)→tri, hash/equal, cost/selectivity rank, print
src/expr.c/.h              atree_expr_t builder, normalize() (NOT push-down, De Morgan, XOR expansion, flatten, dedup, constant fold, single-child collapse), reference eval, print
src/lexer.c/.h             tokenizer with offsets
src/parser.c/.h            recursive descent → atree_expr_t, depth-limited
src/node.h                 node struct, slab
src/tree.c                 create/destroy, identity table, insert (build, reorganize, self-adjust, relevel), delete (cascade), attr/sub bookkeeping
src/index.c/.h             per-attribute predicate indexes (phase 1 routing, add/remove, probe)
src/search.c               phase 1 + phase 2, report
src/event.c                event object
src/graphviz.c             export
src/validate.c             structural checks
src/stats.c                stats, strerror, version
tests/                     minunit-style C tests (see §6)
bench/                     benchmarks
fuzz/                      libFuzzer harness(es)
docs/DESIGN.md             this design, kept in sync
Makefile, CMakeLists.txt, atree.pc.in, .clang-format, .github/workflows/ci.yml
README.md, CHANGELOG.md, LICENSE
```

### 4.2 Core data structures

```c
typedef uint32_t node_id;              /* slab index; ATREE_NODE_NONE = UINT32_MAX */

enum node_kind { NODE_LEAF, NODE_AND, NODE_OR };

/* One cache line per node (64 bytes on 64-bit). Rarely used data lives in side tables. */
struct node {
    uint8_t   kind;                     /* NODE_LEAF / NODE_AND / NODE_OR                   */
    uint8_t   flags;                    /* IN_USE, HAS_SUBS, ...                             */
    uint16_t  reserved;
    uint32_t  level;                    /* 1 for leaves                                      */
    uint64_t  hash;                     /* structural hash (identity table key)              */
    uint32_t  use_count;                /* paper's useCount: #parents + #subscriptions       */
    node_id   access_child;             /* AND only: designated waking child, else NONE      */
    struct idvec children;              /* inner: {node_id *p; uint32_t len, cap} sorted asc, unique, len >= 2 */
    struct idvec parents;               /* all structural parents (AND and OR)               */
    uint32_t  pred;                     /* leaf: index into the predicate slab, else NONE    */
    uint32_t  index_slot;               /* leaf: position in its per-attribute index         */
};
/* Side tables (writer-only mutation, read by search):
 *   preds[]          predicate slab (attr, kind/op, operand; list operands own their arrays)
 *   subs_by_node     u32map node_id → vec(atree_id_t); consulted only when HAS_SUBS is set */

struct atree {
    atree_allocator_t alloc; unsigned flags; size_t max_depth;
    struct attr_table attrs;  struct strtab strings;
    struct node *nodes; uint32_t nodes_len, nodes_cap; vec(node_id) free_list;
    struct u64map identity;             /* structural hash → node_id (chains resolved by structural equality) */
    struct u64map subs;                 /* atree_id_t → node_id */
    struct attr_index *index;           /* one per attribute (phase 1) */
    uint32_t max_level;
    struct atree_stats stats;
};
```

Identity of a node:
- leaf: `(attr, predicate kind/op, operand)` — hashed over the normalized
  predicate (lists already sorted/deduped; `-0.0` normalized to `0.0`).
- inner: `(kind, children[])` with children sorted by `node_id`.

Lookup = hash, then walk candidates comparing structurally. Because node ids
are reused from the free list, a child id array is only ever compared against
*live* nodes, which is safe: dead nodes have been removed from every parent
and from the identity table before their slot is recycled.

### 4.3 Normalization (expr.c)

Applied to every incoming expression before it touches the tree, producing a
tree that contains only leaves, AND, OR (and the constants TRUE/FALSE at the
top only):

1. Push NOT to the leaves: `¬(a∧b) → ¬a∨¬b`, `¬(a∨b) → ¬a∧¬b`, `¬¬a → a`,
   `a⊕b → (a∧¬b)∨(¬a∧b)`, `¬(a⊕b) → (a∧b)∨(¬a∧¬b)`.
2. Negate leaves by flipping the operator: `< ↔ >=`, `<= ↔ >`, `= ↔ <>`,
   `in ↔ not in`, `one of ↔ none of`, `all of ↔ not all of` (internal
   operator, as in the Rust crate), `is null ↔ is not null`,
   `is empty ↔ is not empty`, `var ↔ not var`.
3. Flatten nested same-operator nodes; sort children canonically; drop
   duplicate children; constant-fold (`a ∧ FALSE → FALSE`, `a ∨ TRUE → TRUE`,
   drop neutral elements); collapse single-child connectives.
4. Enforce `max_depth` (default 64) — the parser already bounds nesting, the
   builder API is checked here.

A constant TRUE expression matches every event; FALSE matches none. Both are
accepted and tracked outside the DAG (`always` list / no-op), as cep-atree
does.

### 4.4 Insert (tree.c) — Alg. 1/4 with reorganize and self-adjust

```
insert(id, expr):
  if subs contains id → DUPLICATE_ID
  norm = normalize(expr)                         (may fail: NOMEM / TOO_DEEP)
  begin journal                                  (§4.7)
  n = build(norm)                                recursive over the normalized tree, depth ≤ max_depth
  attach id to n: n.subs.push(id); n.use_count++; subs[id] = n
  commit journal

build(e):
  leaf  → get_leaf(pred): identity lookup; else new node, add to per-attribute index, insert in identity table
  inner → ids = sort_unique(build(child) for child in e.children)
          if |ids| == 1 → return ids[0]
          if !NO_REORGANIZE → reorganize(kind, ids)         (Alg. 2, greedy: candidate = parent of an operand whose
                                                            children ⊆ ids; pick the largest; repeat)
          n = identity lookup (kind, ids); if found → use_count++ (paper line 3), return n
          n = new inner node(kind, ids); link parents; level = 1 + max(child.level); choose_access_child(n)
          insert in identity table
          if !NO_SELF_ADJUST → self_adjust(n)                (Alg. 3: for each parent p of any child of n with
                                                            p.kind == n.kind and n.children ⊂ p.children:
                                                            rewrite p.children = (p.children \ n.children) ∪ {n},
                                                            unless the rewritten set already exists; re-hash p,
                                                            re-link, relevel(p) upward, re-choose p's access child)
          return n
```

`use_count` semantics: incremented for every structural parent link and every
attached subscription id (so it equals `parents.len + subs.len`; kept
explicit for O(1) checks and for validate()).

Bounding reorganize/self-adjust work: hot leaves (`gender = female`) can
have tens of thousands of parents, so naive candidate scans would make
insert O(parents × fanout). Mitigations, all writer-side (so they may use
mutable tree scratch): (1) membership tests use a per-tree `mark[]` array
stamped with an insert epoch instead of linear `contains`; (2) a candidate
parent `p` is rejected in O(1) if `p.children.len > |U|` (reorganize) or
`p.children.len <= |n.children|` (self-adjust) before any subset test; (3)
operands are scanned in order of increasing parent count and the total
number of candidates examined per node is capped by
`config.max_adjust_candidates` (default 4096); exceeding the cap skips the
remaining candidates for that node, which costs sharing, never correctness.
Stats record `adjust_candidates_skipped` so the cap can be tuned.

Access-child choice (`choose_access_child`): among the AND node's children
pick the one with the lowest *wake rank* (estimated probability of being
true): equality / `in` / `one of` on non-bool attributes (0) < range
comparisons (1) < inner AND (2) < inner OR (3) < bool variable (4) <
negated/wide predicates `<>`, `not in`, `none of`, `is not null`,
`not all of` (5); ties broken by lower evaluation cost, then lower node id.
Isolated in one function so it can be tuned or made statistics-driven later.

### 4.5 Delete — Alg. 5

```
delete(id):
  n = subs[id] or NOT_FOUND; remove id from n.subs (swap-remove); subs.erase(id); n.use_count--
  worklist = [n]
  while worklist: m = pop
    if m.use_count > 0 → continue
    remove m from identity table; if leaf → remove from index
    for c in m.children: remove m from c.parents (swap-remove); c.use_count--; if c.use_count == 0 → push c
    recycle slot m
  recompute max_level lazily (keep a per-level node count array; max_level = highest non-zero)
```

Deletion never triggers reorganize/self-adjust (matches the paper).

### 4.6 Search (search.c) — Alg. 6 with both optimizations

Per-search state lives in the `atree_report_t`: bitset `is_true`
(1 bit/node), bitset `queued`, one growable queue per level
(`levels[1..max_level]`; level 1 holds the seeded leaves), match vector.
All sized to `nodes_cap` and reused.

**Clearing is proportional to work done, not tree size.** The paper's
"event signature + lazy cleaning" (§5.2.2) achieves O(1) reset by stamping
nodes; we cannot stamp nodes (readers never write tree memory), so instead
every node whose bit was set is also in a level queue, and the end of a
search walks the queues and clears exactly those bits. A `memset` over a
30M-node bitset would cost more than the match itself; this walk costs
O(nodes touched). Growth of the bitsets (when the tree has grown since the
last search) zero-fills only the new tail.

```
search(ev, rep):
  grow scratch to nodes_cap if needed (zero only the new tail); reset queue lengths and matches
  phase 1: for each attribute a:
      if ev[a] undefined → seed only `is null` leaves of a (from the attribute's null-list)
      else probe index(a) with the value → every leaf that is true → mark is_true, emit(leaf)
  phase 2: for level = 2 .. max_level: for n in queue[level]:
      r = (n.kind == AND) ? all children is_true : any child is_true     (pure bit lookups: lower levels are final)
      if !r → continue                                                     (zero suppression)
      is_true[n] = 1; emit(n)
  emit(n): if HAS_SUBS → matches.append(subs_by_node[n]); for p in n.parents:
      if p.kind == AND && !NO_PROPAGATION_ON_DEMAND && p.access_child != n → skip
      if !queued[p] → queued[p] = 1; queue[p.level].push(p)
  append `always` subscriptions; sort matches
  cleanup: for each level queue: for n in queue: is_true[n] = queued[n] = 0; queue.len = 0
```

Per-search counters in `atree_report_stats_t` (never in the tree):
`predicates_evaluated`, `predicates_matched`, `nodes_visited`, `and_woken`
(AND nodes dequeued), `and_true` (of which true — the propagation-on-demand
hit rate; a low ratio means the access-child heuristic is choosing badly for
this workload), `or_visited`, `matches`.

Why 2-valued inner evaluation is sound with 3-valued leaf semantics: NOT has
been eliminated, so an `undefined` operand can never make an AND/OR `true`
(Table 2 of the paper); therefore "not true" and "false" are
indistinguishable for matching, and only `true` ever needs to be stored or
propagated. Leaves implement the 3-valued rules exactly (`is null` is the only
predicate true on an undefined attribute; everything else is not true).

### 4.7 Strong consistency guarantee on failure (journal)

Any `ATREE_ERR_NOMEM` (or other failure) during `insert` must leave the tree
exactly as it was. Approach: insert records every structural change it makes
(new node ids, identity-table insertions, parent links added, self-adjust
rewrites with the old child sets) in a small journal vector; on failure the
journal is undone in reverse. Alternative if simpler in practice: run
`build()` as "find or create, bump use_count" and on failure call the
`delete` cascade on the partially built root node, which releases exactly the
nodes nobody else uses (this is what cep-atree does) — but this does not undo
self-adjust rewrites, so the journal is preferred when self-adjust is on.
Decided in M4: the cascade approach, with the worklist reserved before any
mutation so rollback itself cannot fail. Self-adjust rewrites (M6) must be
journaled separately. Residue: string literals interned before the failure
stay in the string table. Tested with a failing allocator (§6.4).

### 4.8 Per-attribute predicate indexes (index.c) — phase 1

Each attribute owns an `attr_index`:

| Leaf predicate | Structure | Probe with event value `v` |
|---|---|---|
| bool `x`, `not x` | two leaf lists | pick list by `v` |
| `= c` (int/string/float) | hash map `c → leaf list` (float keys normalized) | lookup `v` |
| `<> c` | sorted array of `(c, leaf)` | all leaves except the equal range |
| `in [..]` | hash map `element → leaf list` (one leaf under many keys) | lookup `v`; `queued` bit dedups |
| `not in [..]` | leaf list | each leaf true unless `bsearch(list, v)` |
| `< <= > >=` (int/float) | two sorted arrays: lower rays (`>`,`>=`) and upper rays (`<`,`<=`) sorted by `c`, ids as tiebreak | binary search; prefix/suffix walk, boundary handled per `>` vs `>=` |
| `one of [..]` (list attr) | hash map `element → leaf list` | for each element of `v`, lookup |
| `none of`, `all of`, `not all of` | leaf list | merge-walk evaluation per leaf |
| `is null` | leaf list | seeded only when undefined |
| `is not null`, `is empty`, `is not empty` | leaf lists | trivial presence/length check |

With `ATREE_FLAG_NO_PREDICATE_INDEX`, every leaf is evaluated directly
(`predicate_eval`), which is the simplest correct implementation and the
oracle the indexed path is tested against.

Hash maps keyed by value hold small leaf-id vectors; removal is swap-remove.
Sorted ray arrays use binary-search insert/remove (O(n) memmove, acceptable:
inserts are rare relative to searches; can become a B-tree later without API
change).

### 4.9 Cost / selectivity model

Evaluation cost (for tie-breaking and for stats): constant for
variable/comparison/equality/null checks, `log2(len)` for `in`/`not in`,
`len` for list ops; AND = Σ + 50, OR = Σ + 60 (Rust crate's constants).
Wake rank per §4.4.

---

## 5. Build, tooling, CI

- **Makefile** (GNU make, POSIX toolchains): `all` (static `libatree.a` +
  shared `libatree.so`/`.dylib` with soname/version), `check`, `check-asan`,
  `check-ubsan`, `check-valgrind`, `bench`, `fuzz`, `format`, `format-check`,
  `install` (`PREFIX`, `DESTDIR`, pkg-config `atree.pc`), `clean`.
  Variables: `CC`, `CFLAGS`, `MODE=debug|release`.
- **CMakeLists.txt** (for Windows/MSVC, IDEs, `find_package(atree)`): same
  targets, options `ATREE_BUILD_TESTS`, `ATREE_BUILD_BENCH`,
  `ATREE_BUILD_SHARED`, `ATREE_SANITIZE`.
- **Flags** (library and tests alike): `-std=c99 -pedantic -Wall -Wextra
  -Wshadow -Wconversion -Wsign-conversion -Wstrict-prototypes
  -Wmissing-prototypes -Wcast-qual -Wcast-align -Wpointer-arith
  -Wwrite-strings -Wformat=2 -Wundef -Wvla -Wswitch-enum -Wswitch-default
  -Wfloat-equal -Wdouble-promotion -fvisibility=hidden` (`-Wfloat-equal`
  forces every intentional exact double comparison through one documented
  helper);
  `-Werror` in CI. MSVC: `/W4 /WX /std:c11` (MSVC has no C99 mode; code must
  stay in the C99 ∩ C11 subset MSVC accepts: no VLAs, no `restrict` issues,
  `_Static_assert` via macro).
- **Export macro** `ATREE_API` (`__attribute__((visibility("default")))` /
  `__declspec(dllexport|dllimport)` / empty).
- **clang-format** config adapted from be-tree's.
- **CI (GitHub Actions)** matrix: ubuntu-gcc, ubuntu-clang (+ASan/UBSan job),
  **ubuntu-clang TSan job** running `test_threads`, macos-clang, windows-msvc
  (CMake), plus valgrind job, format check, a `-std=c11` and `-std=c17`
  compile job, a C++ job that compiles a TU including `atree.h` as C++, a
  32-bit (`-m32`) job if the runner allows, and a benchmark job that runs
  `bench_synthetic --quick` and fails on the regression gates in §9.3.
- Optional static analysis: `scan-build`, `clang-tidy` with a focused
  checklist (`clang-analyzer-*`, `bugprone-*`, `cert-*`).

---

## 6. Testing strategy

### 6.1 Harness
`tests/test.h`: minunit-style (`TEST(name)`, `ASSERT_*`, `RUN_TEST`), returns
nonzero on failure, prints failures with file:line, independent of `NDEBUG`.
One executable per module (`tests/test_vec.c`, `test_hash.c`,
`test_strtab.c`, `test_value.c`, `test_predicate.c`, `test_expr.c`,
`test_lexer.c`, `test_parser.c`, `test_tree.c`, `test_search.c`,
`test_index.c`, `test_delete.c`, `test_api.c`, `test_alloc_failure.c`,
`test_differential.c`, `test_graphviz.c`, `test_validate.c`).

### 6.2 Unit tests
Port every behavior exercised by the Rust crate's tests (parser, ast
optimization, predicates incl. `one_of`/`all_of` merge walks, events, atree
insert/search/delete scenarios, evaluation bitset) and by be-tree's
search/parser tests where the semantics overlap.

### 6.3 Differential / property tests (the main correctness net)
`tests/gen.h`: deterministic PRNG (xoshiro/splitmix, seeded), random
attribute tables, random expressions (ABE-Gen-like: operator distribution,
depth, fan-out, Zipf-shared subexpressions and predicates), random events
(with undefined attributes, NaN floats, unknown strings, empty lists).
For each seed: insert N expressions; for M events compare
`atree_search` results with brute-force `atree_expr_eval == ATREE_TRUE` over
all inserted expressions; run under every combination of
`ATREE_FLAG_*`; interleave random deletes and re-inserts, calling
`atree_validate` after each mutation; at the end delete everything and assert
`stats.nodes == 0` and `bytes_allocated == 0`.

### 6.4 Allocation-failure tests
A counting allocator that fails the k-th allocation; for k = 1..K run
create/insert/search/delete and assert: status is `ATREE_ERR_NOMEM`, the
tree still validates, search results equal those of an untouched twin tree,
and after destroy the live-bytes counter is 0 (no leaks on any path).

### 6.5 Fuzzing
`fuzz/fuzz_parser.c` (libFuzzer/AFL++ compatible `LLVMFuzzerTestOneInput`):
parse arbitrary bytes against a fixed attribute table, insert on success,
search with a derived event. Seed corpus from the test expressions.

### 6.6 Concurrency tests (`tests/test_threads.c`)
Portable thread shim for tests only (`tests/threads.h`: pthreads or Win32).
(a) Build a tree with 10k expressions; run 8 threads × 10k searches each on
disjoint event sets with their own event/report objects; assert every result
equals the single-threaded result. Run under TSan: any write to tree memory
from a read path is a data race and fails the job. (b) Same with the
`extras/atree_lock_pthread.h` adapter installed and a writer thread
inserting/deleting concurrently; assert no crash, `validate()` passes after
each writer step, and each search result equals the result against a
snapshot taken under the same lock. (c) Build-swap-retire pattern test.

### 6.7 Performance regression tests (`tests/test_perf.c`, also `make bench`)
Not timing-based (CI noise); they assert *work counts* from
`atree_report_stats_t`, which are deterministic:
- **Index independence:** with indexes on, adding 100k predicates on
  attributes the event does not touch must not change
  `predicates_evaluated`.
- **Zero suppression:** an expression tree of depth 6 whose root is false
  yields `nodes_visited == 0` beyond the woken level-2 nodes.
- **Propagation on demand:** for `AND(cheap_false, expensive_true)`, the AND
  is never woken (`and_woken == 0`).
- **Sharing:** inserting the same 1k subexpressions under 10k different
  roots yields `stats.nodes` within 5% of the minimum achievable.
- **Reorganize/self-adjust:** the paper's Figure 4/5 scenarios produce the
  expected node and edge counts.
- **Allocation-free steady state:** with the counting allocator, 1000
  searches after warm-up make zero allocator calls.

### 6.8 Benchmarks
`bench/bench_synthetic.c`: ABE-Gen parameters from Table 3 of the paper
(expressions, operator mix, depth, fan-out, dimensions, cardinality, event
size, Zipf α); reports insert throughput, search latency (p50/p99), memory via
the counting allocator, and stats; monotonic clock (`clock_gettime` /
`QueryPerformanceCounter` behind a tiny shim in the bench only).
`bench/bench_file.c`: line-oriented input (`id<TAB>expression`, events as
`attr=value;...`), plus `bench/data/` converted from
`reference/a-tree/benches/data/search.json` by a small script.

---

## 7. Milestones

Each milestone ends with: all tests green under plain build, ASan+UBSan, and
valgrind; `-Werror` clean on gcc and clang; header compiles as C++;
`docs/DESIGN.md` and `README.md` updated for what changed; `CHANGELOG.md`
entry.

**M0 — Scaffold.** Layout, `include/atree.h` skeleton (all types/prototypes,
documented), `atree_strerror`, version, allocator wrappers with overflow
checks and live-bytes accounting, `vec.h`, hash maps, string table, test
harness, Makefile + CMake, `.clang-format`, CI workflow, LICENSE, README stub.
Tests: vec, hash, strtab, alloc.

**M1 — Values, attributes, predicates, events.** Attribute table; value
union with sort/dedup/compare/hash (double normalization); predicate
representation, `negate`, 3-valued `eval`, hash/equal, cost, wake rank,
print; event object (by name and by id). Tests: value, predicate (every
operator × type × undefined/NaN/empty), event validation.

**M2 — Expressions.** Builder API including `xor`/`xnor`/`true`/`false`,
`normalize()` (all rules in §4.3, including XOR/XNOR expansion), reference
evaluator, printer, depth limit. Tests: normalization identities
(port Rust `ast.rs` tests), random `expr == normalize(expr)` equivalence
via reference eval on random events.

**M3 — DSL.** Lexer with offsets; parser for the full grammar in §3
(including `xor`, `xnor`, `between`, `true`/`false`); error messages with
positions; type checking. Tests: port all Rust parser tests; error-position tests;
round-trip `print(parse(s))` re-parses to an equal normalized form; fuzz
harness compiles.

**M4 — Tree core.** Node slab, identity table, `insert` (uniqueness only;
reorganize/self-adjust flags forced off), `delete` cascade, `search` with
`NO_PREDICATE_INDEX` semantics (evaluate all leaves) but full phase 2 (zero
suppression + propagation on demand), report API, callback API, `validate`,
`stats`, journal/rollback, clear-by-dirty-list scratch, optional
`atree_lock_t` plumbing, `extras/atree_lock_pthread.h`. Tests: tree
scenarios from the Rust crate, differential suite (flags: index off),
delete-to-empty leak checks, allocation-failure suite, `test_threads` under
TSan, `test_perf` work-count gates for zero suppression and propagation on
demand and allocation-free steady state.

**M5 — Predicate indexes.** `index.c` per §4.8; phase 1 probes; differential
suite now runs index on vs off and compares. `test_perf` index-independence
gate: phase 1 work is proportional to matched predicates, not total
predicates.

**M6 — Reorganize and self-adjust.** Alg. 2 and Alg. 3 with `relevel`,
identity-table re-keying, access-child re-choice, journal entries for
rollback, bounded candidate scans (§4.4). Differential suite over all 16
flag combinations; validate after every mutation; `test_perf` sharing gates
(paper Figures 4 and 5 reproduce exact node/edge counts); stats counters.

**M7 — Polish and release 0.1.0.** Graphviz export, `atree_expr_print`,
benchmarks + data conversion script + CI regression gates (§9.3), README
(quick start, DSL reference, semantics table, thread-safety, allocator
guide, performance notes), `docs/DESIGN.md` final pass, pkg-config/install,
CI badges, tag `v0.1.0`.

Stretch (post-0.1): statistics-driven access-child choice; B-tree-backed ray
indexes; interval predicates (`between`) as a single leaf (cep-atree's
interval index); tree serialization; single-header amalgamation; C++ RAII
wrapper.

---

## 8. Open decisions (defaults chosen; revisit if the user objects)

| Decision | Default | Rationale |
|---|---|---|
| License | MIT | Matches be-tree and bplus-tree. |
| Subscription id type | `uint64_t` | be-tree uses 64-bit ids; Rust is generic. Callers can map anything to a u64. |
| Duplicate id insert | error | Avoids silent aliasing; the Rust crate allowed it. |
| Float literal for int attribute | error; int literal for float attribute promotes | Lossless direction only. |
| Keyword case | case-insensitive | SQL-like; more general than the Rust crate. |
| Attribute set | fixed at create | Dense event arrays, simple ids; same as both references. |
| Search mutates tree? | never | Enables concurrent searches without locks (unlike cep-atree). |
| Match order | sorted by id | Deterministic output for callers and tests. |

---

## 9. Assurance: thread safety, fidelity to the paper, performance

These three properties are requirements, each with a verification method.
A milestone is not done if any of its checks here fail.

### 9.1 Thread safety

**Model.** The tree is an immutable value between writes. Read paths
(`const atree_t *`) never write to tree memory, never touch shared scratch,
and never call back into the tree. All per-call state is in objects the
caller owns per thread (`atree_event_t`, `atree_report_t`). Writes
(`insert`, `delete`, `destroy`) require exclusion from everything else.

**Why this is the right design for this library.** The paper's workloads
(ad exchanges, CEP) match millions of events per second against a slowly
changing expression set. Lock-free reads with no reader-side atomics give
linear read scaling; writers are rare and can afford a lock. The alternative
(cep-atree: epoch stamps on nodes + one mutex) serializes every search.

**Mechanisms.**
1. Const-correctness enforced by the compiler: read paths receive
   `const struct atree *`; `-Wcast-qual` makes any const cast a hard error.
   No `mutable`-style tricks (no pointers to non-const stored inside the
   tree that read paths follow for writing; `validate()` checks this in
   review, TSan checks it at runtime).
2. Injected lock (`atree_lock_t`, §2.3). When present, every public call
   wraps itself in `rdlock`/`wrlock`. `extras/atree_lock_pthread.h`
   (pthread_rwlock) and `extras/atree_lock_win32.h` (SRWLOCK) are header-only
   adapters; the library itself has no platform dependency.
3. Documented happens-before contract for callers who synchronize
   themselves, and the build-swap-retire pattern for lock-free publication
   of a rebuilt tree (§2.9).
4. Allocator is only called from read paths on object creation and growth;
   steady state is allocation-free (verified, §6.7).

**Verification.** `tests/test_threads.c` under ThreadSanitizer in CI (§6.6):
8 readers with no lock, 8 readers + 1 writer with the pthread adapter,
build-swap-retire. A single TSan report fails the job. Review checklist item
in `CLAUDE.md`: "does this function take `const atree_t *`? then it must not
write".

**Explicit non-guarantees.** Concurrent insert with unlocked searches is a
data race and is unsupported (no RCU in v1). Sharing one event or report
between threads is unsupported.

### 9.2 Fidelity to the paper

Mapping from paper to implementation, with every deviation listed and
justified. Anything not listed as a deviation is implemented as specified.

| Paper | Implementation | Status |
|---|---|---|
| §3.1 language: `< <= = != >= >`, `∈ ∉`, `between`, `and or not xor xnor`, repeated predicates | all operators in builder and DSL; `between` lowers to two leaves; repeated predicates share one leaf | faithful |
| §3.2 three-valued matching; match iff `true` | leaves 3-valued; inner nodes store only `true` (sound because NOT is eliminated, §4.6) | faithful (equivalent) |
| §4.1 l/i/r-nodes, `level = 1 + max(children)` | l-node = leaf, i-node = inner, r-node = inner-or-leaf with `HAS_SUBS`; level formula identical | faithful (r-node is a role, not a type) |
| §4.2.1 unique id per predicate/subexpression; commutative id for and/or; expression-to-node hash table | identity = structural hash **and** structural equality over (operator, sorted child ids) / full predicate | stronger (collision-safe) |
| §4.2.2 Alg. 2 reorganize (greedy set cover, same operator, subsets of operand set) | `reorganize()` in `build_inner()` for every inner node that the lookup did not find (a hit returns the existing node first, Alg. 4 lines 1-4) | faithful, with a work cap (`max_adjust_candidates`) that can only reduce sharing |
| §4.2.3 Alg. 3 self-adjust (rewire parents whose operand set strictly contains the new node's) | `self_adjust()` after node creation, with `relevel`, identity re-key, access-child re-choice | faithful; skips a rewrite that would duplicate an existing node (necessary for uniqueness; paper is silent); candidate scan capped by `max_adjust_candidates` |
| §4.2.4 / Alg. 5 delete via `useCount`, cascading to children | `delete()` iterative cascade | faithful |
| Alg. 4 insert: existing node → `useCount += 1`; else reorganize, recurse, create, self-adjust | `build()` | faithful: `prehash` + `lookup_inner` return an existing node before visiting its operands (lines 1-4; the identity is the flat content of the normalized subexpression, verified structurally); on a miss the operands are built first because reorganize needs their ids |
| §5.1 Alg. 6: per-level queues, bottom-up level order, results stored on parents, match when `true` | level queues in the report; results as bitsets; parents pull children (equivalent to pushing operands) | faithful |
| §5.2.1 zero suppression: NOT removed by De Morgan + operator inverses; `false`/`undefined` not propagated; undefined operand treated as `false` | `normalize()` + phase 2 only enqueues on `true` | faithful |
| §5.2.2 propagation on demand: one access child per AND wakes it; it pulls other operands | `access_child` + `emit()` filter + bit lookups (lower levels final) | faithful; **deviation:** access child chosen by selectivity heuristic instead of "randomly" (strictly better; §4.4) |
| §5.2.2 per-event signature with lazy cleaning | dirty-list clearing in the report (§4.6) | equivalent cost, no tree mutation |
| Phase 1 "predicate matching" via existing per-dimension algorithms | per-attribute indexes (§4.8) | faithful in spirit; paper does not prescribe structures |
| Table 2 undefined semantics | encoded in leaf `eval` + reference evaluator; tested exhaustively | faithful; **extension:** float NaN in an event counts as undefined |

Additional fidelity checks: the paper's Figure 4 (two expressions sharing
`P5 ∨ P6`), Figure 5 (reorganizing `P1 ∨ P2 ∨ P3 ∨ P4` against
`P1 ∨ P2 ∨ P3`), the self-adjust example of §4.2.3, and the worked
matching example of §5.3 / Figure 6 are each a unit test asserting the
exact node/edge counts, levels, and (for Figure 6) the exact set of visited
nodes per level. The complexity claims of §4.3 and §5.4 are checked by the
work-count gates in §6.7.

### 9.3 Performance

**Design points that determine performance** (each already in §4; collected
here with the reason):

| Concern | Decision |
|---|---|
| Search cost ∝ matching work, not tree size | per-attribute indexes (phase 1), zero suppression + propagation on demand (phase 2), dirty-list clearing (reset). Nothing in a search is O(nodes). |
| Steady-state allocation | none: events, reports, bitsets, queues, match vector keep capacity |
| Cache behavior | 64-byte node; child/parent id arrays contiguous `uint32_t`; predicates in a separate slab touched only in phase 1; subscription lists in a side table touched only for true nodes with `HAS_SUBS` |
| Hot leaf fan-out | `emit()` iterates parents but enqueues only ANDs for which the leaf is the access child; OR parents are enqueued once (`queued` bit) |
| Strings | interned at insert/event-build time; comparisons are integer compares; event strings unknown to the tree map to a sentinel without allocation |
| Lists | sorted + unique once; `in` is a binary search or an inverted-index lookup; `one of` an inverted-index lookup; `all of`/`none of` a merge walk |
| Insert | O(1) identity lookups; reorganize/self-adjust bounded by `max_adjust_candidates` with O(1) mark-array membership |
| Delete | O(fanout · log fanout) per freed node: per-edge parent positions make each unlink a swap-remove plus one binary search; no scan of a hot leaf's parent list |
| Memory | target ≤ 64 B/node + child/parent arrays + predicate slab; `bytes_allocated` reported; benchmark prints bytes per expression |

**Targets** (single thread, release build, modern x86-64 laptop; measured
with `bench_synthetic` using Table 3 defaults from the paper: 1M
expressions, 40/40/10/5/5 and/or/not/xor/xnor, depth 3, fan-out 4, 1K
dimensions, cardinality 100, event size 20, Zipf α = 0.6). These are the
numbers to tune toward, informed by the paper's Table 4 (1.6 ms match over
1.39M real expressions on 2018 hardware):

| Metric | Target |
|---|---|
| Match latency p50 / p99 | ≤ 1 ms / ≤ 3 ms |
| Insert throughput (all optimizations on) | ≥ 100k expressions/s |
| Delete throughput | ≥ 200k/s |
| Memory per inserted expression (after sharing) | ≤ 400 bytes |
| Read scaling | 8 threads ≥ 7× single-thread search throughput |
| Phase 1 cost with 10× irrelevant predicates added | unchanged within noise (gate in §6.7 uses counts, not time) |

**Regression gates in CI** (`make bench-check`: `bench_synthetic --quick`,
20k expressions, compared against `bench/baseline.json` committed with each
release): fail if nodes, edges, `bytes_allocated`, total matches, total
`nodes_visited` or total `predicates_evaluated` drift by more than 5%.
Decided in M7: wall-clock latency is printed but **not** gated, because CI
runners vary by far more than any meaningful threshold; latency regressions
are caught by the work counters they would have to come from, or by hand.
Updating the baseline is a deliberate commit with a justification.

**Profiling plan (M7).** `perf`/Instruments on the synthetic workload;
expected hot spots in order: phase-1 hash probes, `emit()` parent iteration,
AND child bit lookups. Known follow-ups if needed, none of which change the
API: SoA split of `struct node` (hot fields vs cold), B-tree ray indexes,
interval leaves for `between`, statistics-driven access-child selection via
a writer-side `atree_tune()` fed by report counters.

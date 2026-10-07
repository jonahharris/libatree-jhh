# libatree design notes

This document describes the implementation module by module and records the
design decisions behind it. `PLAN.md` is the plan the library was built from
and keeps the paper-to-code mapping (§9.2) and the performance rationale
(§9.3).

## Module map

| Module | Purpose |
|---|---|
| `include/atree.h` | The public API. Opaque handles, status codes, vtables for allocator and lock. |
| `src/compiler.h` | The only file with compiler/platform conditionals: `ATREE_STATIC_ASSERT`, `ATREE_UNUSED`, branch hints. |
| `src/alloc.[ch]` | `struct atree__mem` wraps the user's `atree_allocator_t` and tracks live/peak bytes and call counts. All library allocations go through `atree__alloc/zalloc/realloc/free` and the overflow-checked `*_array` helpers. Size 0 is never forwarded; `free`/`realloc` always receive the exact size previously requested. |
| `src/vec.h` | `ATREE_VEC_DEFINE(name, T)` generates a typed `{T *data; uint32_t len, cap;}` vector with init/free/reserve/push/insert_at/remove_at/swap_remove/clear. Growth is geometric (×2, minimum 4) via one shared helper `atree__vec_grow` that returns the old pointer unchanged on failure. No type punning: the helper returns `void *` that is implicitly converted. |
| `src/hash.[ch]` | Open-addressing, linear-probing maps with a separate state byte per slot (empty/full/tombstone), power-of-two capacity, rehash at 70% occupancy counting tombstones; a rehash drops tombstones so churn never grows the table. `atree__u64map` (u64→u32) backs identity and subscription tables; `atree__strmap` ((bytes,len)→u32) backs string interning and borrows key bytes. Hashing: splitmix64 finalizer for integers, FNV-1a + finalizer for bytes. |
| `src/strtab.[ch]` | String interning. Id 0 is the "unknown" sentinel so an event string the tree has never seen equals no literal. Ids are dense, assigned from 1, never reused. Bytes live in 64 KiB chunks (or a dedicated chunk for longer strings) so pointers are stable; the map borrows them. |
| `src/stats.c` | `atree_version`, `atree_strerror`, `atree_config_init`, and `atree__vec_grow`. |
| `src/atree_internal.h` | `struct atree` (allocator context, resolved config, lock copy, attribute table, string table) and the `atree__rdlock/wrlock` helpers that are no-ops without an injected lock. |
| `src/keywords.[ch]` | DSL keyword list and identifier syntax shared by the attribute table and (M3) the lexer. Case-insensitive keyword match, no `<ctype.h>`. |
| `src/attr.[ch]` | Attribute table: validated, copied names (identifier syntax, not a keyword, unique), dense ids in declaration order, name→id map. |
| `src/value.[ch]` | `struct atree__value` tagged union for event values and literal operands; sorted-unique list helpers (sort, binary search, intersect, contains-all merge walks); double helpers (`atree__double_eq` is the only exact comparison, `-0.0` canonicalized for hashing, exact int→double promotion up to 2^53). |
| `src/writer.[ch]` | Output over `atree_write_fn`: integers, round-trip doubles that always lex as floats, quoted strings with escapes; becomes a no-op once the callback cancels. |
| `src/predicate.[ch]` | Leaves: `<attr, kind, op, operand>`. `atree__pred_check` applies the type table and normalizes operands; `negate` is an exact involution for every kind; `eval` is three-valued; hash/equal are structural; cost and wake rank feed node ordering (M4); `print` renders DSL. |
| `src/event.[ch]` | Public event object: dense value array by attribute id, reusable per-attribute list buffers, strings interned by lookup (unknown → sentinel 0), lists sorted and deduplicated, NaN stored as undefined. Own allocator counters. |
| `src/tree.c` | `atree_create` (config validation and defaults, attribute and string tables), `atree_destroy`, attribute queries, `atree_stats` (M1 subset). |
| `src/lexer.[ch]` | Pull-based, allocation-free tokenizer. Tokens carry byte offsets and lengths; string tokens record the quoted span and are unescaped by the parser into its scratch buffer. Integers are range-checked without `strtoll`; floats use `strtod` and must be finite. Also hosts `atree__error_set*`. |
| `src/parser.c` | Recursive descent for the DSL (`atree_expr_parse`). One-token lookahead plus a lexer-state copy for the two-word keywords (`not in`, `one of`, `is not null`, ...). `and`/`or` chains become one n-ary node; `xor`/`xnor` are binary and left-associative; `between` lowers to two comparisons; `literal in list_attr` lowers to `one of`. Nesting (parentheses and `not`) is bounded by `max_depth` as a recursion guard. Every failure path records a status, offset and message once (`fail`), and later steps become no-ops. |
| `src/node.[h]` | 64-byte `struct atree__node` (kind, flags, 16-bit level, hash, access child, predicate slot, children, parents, and the three boundaries that split the parent list into anchored/waker regions) with a static size assertion; node and predicate slabs; the probe type for identity lookups. |
| `src/identity.[ch]` | Paper's expression-to-node table H_en: open-addressing set of node ids keyed by structural hash; lookups compare the full structure (operator + sorted child ids, or the predicate), so a hash collision can never merge two subexpressions. A second set of the same shape, the content table, files inner nodes under their flat content hash for the lookup-first insert. |
| `src/tree.c` | Lifecycle, attributes, stats, and index construction: `build()` recurses over the normalized expression, reorganizes each operand set against existing nodes (Alg. 2), reuses nodes found in the identity table or creates and links them (Alg. 1/4), then self-adjusts existing parents to reuse the new node (Alg. 3) with relevel and identity re-keying; subscriptions attach to the root node (`use_count` = parents + subscriptions); `cascade()` is the iterative Alg. 5 deletion; every change is journaled so a failed insert rolls back exactly; `atree_validate` checks every invariant. |
| `src/index.[ch]` | Per-attribute phase-1 indexes: bool true/false lists, equality and membership hash buckets (a membership leaf sits in one bucket per element), sorted ray arrays for range comparisons probed as prefix/suffix, an `is null` list seeded only when the attribute is undefined, and a scan list for negated and list-containment forms. O(1) removal from lists via a per-node position array; buckets and rays are found by key. |
| `src/search.c` | Report object (per-thread scratch: two bitsets, one queue per level, match list, counters) and Alg. 6 matching with zero suppression and propagation on demand; reset walks the level queues (dirty list) instead of clearing bitsets. Conveniences: callback delivery, exists, allow-list filtering. |
| `src/graphviz.c` | DOT export: one rank per level, leaves at the bottom, parent→child edges, access-child edges in bold, subscription ids as external labels, constant-true ids in a note. Predicate text is DSL-escaped for DOT. |
| `extras/atree_lock_pthread.h`, `extras/atree_lock_win32.h` | Header-only `atree_lock_t` adapters over `pthread_rwlock_t` and `SRWLOCK`. |
| `bench/bench_synthetic.c`, `bench/bench_file.c` | ABE-Gen-style synthetic workload (Zipf dimensions/values, operator mix, shared subexpressions, `--verify` brute-force check, `--check` baseline gate) and a file-driven benchmark; `bench/convert_rust_search_json.py` converts the Rust crate's dataset. |
| `src/expr.[ch]` | Caller-facing expression trees: public builders (`atree_expr_*`), internal constructors for the parser, `atree__expr_normalize` (the zero suppression filter), structural hash/compare, string-literal resolution by lookup (read path) or interning (write path), the three-valued reference evaluator `atree_expr_eval`, and the DSL printer `atree_expr_print`. Each node owns its allocations through its own `struct atree__mem`. |

## Invariants established in M0

- **Exact-size allocator contract.** `tests/test_alloc.h` wraps every block
  with a header recording its size and counts mismatches; every test asserts
  zero mismatches and zero live bytes at the end. This is how the plan's
  "pool and arena allocators work" promise is enforced.
- **Failure leaves containers intact.** Vector growth and map rehash failures
  return `ATREE_ERR_NOMEM` without modifying the container; tests inject
  failures at each allocation point.
- **Overflow is checked before multiplication** in every size computation
  (`atree__mul_overflows`, `atree__add_overflows`), and 32-bit lengths are
  checked against `UINT32_MAX` before increment.

## Semantics fixed in M1

- **Three-valued leaves** (paper §3.2): an undefined attribute makes every
  predicate `undefined` except `is null` (true) and `is not null` (false).
  `is empty`/`is not empty` on an undefined list are undefined.
- **Negation is exact** for every kind (`atree__pred_negate`), which is what
  lets M2 push NOT into leaves without changing results; the test
  `evaluation_and_negation` checks `eval(¬p) = ¬eval(p)` with undefined
  preserved, for every kind and for defined and undefined values.
- **`all of`**: every literal is present in the event's list (be-tree
  semantics; the Rust crate has it reversed).
- **Strings** are compared by interned id. An event string the tree never
  saw gets id 0 and equals no literal, so `country <> "CA"` is true for it.
- **Floats**: literals must be finite; integer literals promote to float
  attributes only when exactly representable; event NaN is undefined;
  `-0.0 = 0.0`.
- **Reader isolation**: events have their own allocator counters, so building
  an event never writes tree memory (`test_event` asserts the tree's byte
  counters are unchanged by event use).

## Expressions and normalization (M2)

- **Deferred interning.** Builders and the parser take `const atree_t *`, so
  string literals stay as raw bytes inside the expression (sorted bytewise,
  unique). They are interned by `atree_insert_expr` (write path, M4) and
  resolved by lookup for `atree_expr_eval` (read path). A literal the tree
  has never seen is present in no event: equality with it is false,
  membership tests skip it, `all of` cannot be satisfied when one is required.
- **Normal form** (`atree__expr_normalize`): only PRED, AND and OR, plus a
  TRUE/FALSE constant at the root. NOT is pushed into leaves through exact
  predicate negation and De Morgan; XOR/XNOR expand to
  `(a ∧ ¬b) ∨ (¬a ∧ b)` / `(a ∧ b) ∨ (¬a ∧ ¬b)`; same-kind children are
  flattened; constants are folded (absorbing/neutral); children are sorted
  by `atree__expr_cmp` (hash first, then structure) and deduplicated; a
  single remaining child replaces its connective. Depth is bounded by
  `max_depth` before normalization starts; normalization recursion follows
  the input, so stack use is bounded too. Size is bounded by
  `max_expr_nodes` (default 8192) as the normalized nodes are allocated:
  every step creates a node or returns an operand, so the budget bounds
  the work as well, and an expression over it fails with
  `ATREE_ERR_LIMIT` before anything is built.
- **Soundness.** These rewrites are identities of Kleene three-valued logic,
  so normalization preserves the exact three-valued result, not only
  "true vs not true". `test_expr` checks this on 400 random expressions × 40
  random events (with undefined attributes, NaN floats, unknown strings) and
  checks idempotence and normal-form structure. The XOR expansion duplicates
  its operands, so a chain of n XORs has a normal form of about 2^n nodes
  while its depth is only n+1; the node budget, not the depth limit, is what
  stops it (a 40-XOR chain fails in microseconds; before the budget 20 of
  them took 0.6 s and 1.2 GB).
- **Printing** parenthesizes connective children (`(a and b) or not (c or
  d)`); `not` binds tightest and is printed without parentheses around a
  predicate.

## DSL (M3)

Grammar and precedence are in PLAN.md §3 and at the top of `src/parser.c`
(tightest first: `not`/`!`, `and`/`&&`, `xor`/`xnor`, `or`/`||`, as in C).
Keywords are case-insensitive; attribute names are case-sensitive
identifiers. Compatible with the Rust crate's language; additions: `xor`,
`xnor`, `between`, `true`/`false`, `literal in list_attr`, `!=`, `==`.

Diagnostics: `atree_error_t` gets the status, the byte offset and length
of the offending token or predicate, and a message such as
`attribute 'city' of type string: operator or literal not applicable`.
Type errors point at the start of the predicate; lexical errors at the
character. `test_parser` checks about fifty error inputs for status and
offset. The parser never calls the allocator for the expression itself
except through the expression constructors; its scratch (list buffers,
unescaped strings) is balanced per call. The libFuzzer harness in
`fuzz/fuzz_parser.c` is compiled and replayed over `fuzz/corpus/` by
`make check` so it cannot rot.

## The DAG (M4)

**Identity.** A leaf's identity is its normalized predicate, hashed by
content (string literals by their bytes, lists order-free) so that an
unresolved expression leaf hashes to the same value without touching the
string table; an inner node's identity is (operator, sorted unique child
ids). Both are hashed and then compared structurally. An expression
leaf's structural hash is that same identity hash, computed once when the
leaf is built and reused by normalization's canonical ordering, the lookup
probe and the creation of a new leaf. Commutativity
and associativity are therefore free: `a and b` and `b and a` are one node,
and `(a and b) and c` is `AND(a, b, c)` after flattening.

**Use counts and sharing.** The paper's `useCount` is the number of parent
links plus attached subscription ids; it is derived (`parents.len`, the
HAS_SUBS flag) rather than stored. Inserting an expression that already
exists attaches the id to the existing root (paper Alg. 4 line 3).

**Lookup first (Alg. 4 lines 1-4).** Before building anything, insert
hashes every subexpression of the normalized copy from its own text
(`prehash`): a leaf by its content hash, resolved through the identity
table in one probe without allocation or interning, and an inner node by
its *flat content*, the operator plus the multiset of its members where a
member of the same operator contributes its own members, so
`AND(AND(a,b),c)` and `AND(a,b,c)` hash alike. Inner nodes are filed under
that key in the content table (`t->content`, with the member sum per node
in `t->csum`); a rewire by self-adjust replaces children with a same-operator
node and therefore never changes a key. `build_inner` first looks its
subexpression up there and verifies a hit by collecting the node's flat
members and the operands' nodes (resolved recursively the same way) and
comparing the two sorted sets; on a hit it returns without visiting the
operands, as the paper's insert does, and reorganize runs only for nodes
that are actually new. The expression's resolution state is cached on the
normalized copy so nothing is looked up twice.

**Normalization.** The normalized copy is bump-allocated from one arena
(nodes, child arrays, lists and string bytes). The arena, the insert
journal and the operand buffer of `build_inner` (a stack region of
`t->scratch`) are writer-side scratch kept in the tree and reused, so a
steady-state insert allocates only what the index keeps: about 8 allocator
calls per expression on the synthetic workload, down from 12 plus 4 frees,
and 1.7 KB requested instead of 6.2 KB. Normalizing used to make one
allocation per node and per literal and was the largest cost of inserting
a 50-predicate expression. Deleting
detaches the id and cascades over children whose count reaches zero
(Alg. 5), iteratively, with a worklist reserved up front so deletion cannot
fail once it starts mutating.

**Rollback.** Insert reserves the cascade worklist first, then builds; on
any failure (allocation, limit) it cascades over the nodes it created,
newest first. Pre-existing nodes only ever return to their previous use
counts, so they survive. The string table is the one thing a failed insert
may leave behind: literals interned before the failure stay interned (they
are never freed anyway); the DAG, subscriptions and statistics are exactly
as before. `test_alloc_failure` fails every allocation point of a 12-insert
script and checks shape, validity, search results and leak-freedom each
time.

**Matching.** Phase 1 (M4) evaluates every leaf and seeds the true ones at
level 1. Phase 2 drains level queues bottom-up; an AND node is evaluated
with bit lookups over its children (all at lower levels, hence final), an
OR node is true the moment it is woken (only a true child wakes it), a
true node emits its subscriptions (found through a per-node slot array
into the subscription lists) and enqueues its parents, and an AND parent
is enqueued only by its access child. The matched ids are sorted
ascending by an LSD radix sort (11-bit digits, as many passes as the
largest id needs) in a scratch array that grows with the match vector, so
a warmed-up search still allocates nothing. The access child is the child
with the lowest wake rank (equality/membership < ranges < AND < OR < bool <
negated forms), ties broken by lower level, fewer children, lower id, so
the choice is deterministic for a given insertion sequence. Figure 6 of the
paper is a test with exact visit counts; the two-valued inner evaluation is
sound because NOT has been eliminated (PLAN §4.6).

**Reorganize (M6, Alg. 2).** Before an inner node is looked up, its operand
set U is rewritten greedily: among the parents of U's members with the same
operator, pick the one whose children are all in U (largest first), replace
those children by it, repeat. Membership tests use a per-node epoch mark
array. Candidates come from *anchor lists*: every inner node designates
one child as its anchor (the child with the fewest parents when the node
was built, re-chosen on rewire), each node keeps the parents it anchors at
the front of its parent list (`parents[0, nanchor)`), and the per-edge
position entries carry the anchor flag in their high bit (see *Edges and
hot leaves* for the full region layout). A cover S ⊆ U
contains its own anchor, so scanning only the anchored parents of U's
members finds every cover while never walking the parent list of a
popular leaf, which is rarely anybody's anchor because it was already
popular when its parents were built. `max_adjust_candidates` (default
4096; exceeding it is counted in `adjust_candidates_skipped` and only costs
sharing) remains as a safety bound and still governs self-adjust. Sets of
two operands are skipped because their only possible cover is the identity
hit itself.

**Self-adjust (M6, Alg. 3).** After a new node N is created, every parent P
of one of N's children with the same operator and children ⊃ N.children is
rewired to `(P.children \ N.children) ∪ {N}`, unless that set already exists
as another node (uniqueness would be violated; the paper is silent, we
skip). Rewiring re-keys P in the identity table, re-chooses P's access
child, and relevels P and its ancestors (levels only grow in the forward
direction). `test_perf` checks arrival-order independence: a thousand
`p and q and x = i` followed by `p and q` reach the same 2002-edge structure
as the reverse order, with `self_adjusted == 1000`.

**Edges and hot leaves.** A popular predicate (`gender = female` in the
paper's workload; `d0 = 0` in the synthetic one) can have tens of thousands
of parents, which made two operations quadratic in its parent count until
the 1M-expression benchmark exposed them: unlinking a parent from such a
leaf (delete, rollback) scanned the parent list, and validation did the
same for every parent link. The children array of an inner node therefore
stores, next to each child id, the position of the node inside that child's
parent list (`2*cap` uint32 per node, ids in `[0, len)`, positions in
`[cap, cap+len)`); unlinking is a swap-remove plus one binary search to
fix the moved parent's recorded position, O(log fan-out) and allocation
free. Validation checks positions directly and verifies the index in one
linear pass (`atree__index_check`). The same benchmark showed the
reorganize and self-adjust candidate scans running to their cap on every
node: self-adjust now scans only the child with the fewest parents (exact,
since a superset parent is in every child's parent list), and reorganize
scans anchor lists (above). Promoting or demoting an anchor is two swaps in
the child's parent list plus the position fix-ups, allocation free, so
rewire and rollback can re-choose anchors without new failure paths. On
100k expressions insert throughput went from 116k/s to 239k/s (release
build), and at 1M from 40.5k/s to 181k/s, and the index has slightly fewer
edges because the scan no longer hits its cap.

The parent list is split a second way for matching. A parent *wakes* this
node when it is an OR node, an AND node whose access child is this node, or
any AND node when propagation on demand is off; the others would only be
read and skipped when the node is true. The list therefore has four
regions, `[0, end_aw)` anchored wakers, `[end_aw, end_a)` anchored
non-wakers, `[end_a, end_w)` non-anchored wakers and the rest, so
reorganize reads `[0, end_a)` and the search sweep reads `[0, end_aw)` and
`[end_a, end_w)`, never touching an AND parent it cannot wake (for a
popular leaf, most of them). Moving an edge between regions is at most
three swaps across the boundaries plus position fix-ups, allocation free,
and happens when an edge is linked or unlinked, when an anchor is
re-chosen, and when an AND node's access child changes
(`set_access_child`). `atree_validate` checks that every edge sits in the
region its anchor flag and waker status call for and that every inner node
has exactly one anchor edge.

**Journal.** Every insert records what it changed: created nodes, rewired
parents (with their old child set, hash, level and access child) and level
changes. Rollback walks it newest-first without allocating: rewired parents
re-enter the parent lists they left (whose spare slot is intact because all
later pushes were undone first), identity entries return to the tombstone
their removal left (the identity table probes before deciding to grow),
and created nodes are released by cascade over a pre-reserved worklist.
`test_alloc_failure` exercises every allocation point including the
self-adjust paths.

**Phase 1 with indexes (M5).** The probe walks two short lists, not the
schema: the attributes that have `is null` leaves (`index.null_attrs`,
seeded when the event leaves them undefined) and the attributes the event
defines (the event keeps their ids in `defined`, maintained by its setters,
so `atree_event_clear` resets only those). For a defined attribute: bool →
one of two lists; scalar → equality bucket, membership bucket and both ray
arrays; list → the membership bucket of each element; then the attribute's
scan list is evaluated. With a thousand attributes and twenty per event the
schema walk had been about a sixth of a sparse search. `predicates_evaluated`
counts index hits plus scan evaluations, so it is independent of how many
predicates exist on attributes the event does not touch or with values the
event does not have (`test_perf` adds ten thousand such predicates and
checks the count is unchanged). `ATREE_FLAG_NO_PREDICATE_INDEX` keeps the
M4 all-leaves scan as the oracle; the differential suite runs both.

**Reader isolation.** Searches write only to the report. `test_threads`
runs eight unlocked readers against one tree and compares every result with
the single-threaded answer, then readers plus a churning writer through the
pthread adapter (soundness of every returned id, structure validated by the
writer), then a build-swap-retire sequence; `make check-tsan` runs it under
ThreadSanitizer.

## Benchmarks and the regression gate

`make bench` builds `bench_synthetic` and `bench_file`. The synthetic
generator follows the paper's ABE-Gen (§6.1): dimensions and values are
drawn from Zipf distributions, operators from the 40/40/10/5/5
and/or/not/xor/xnor mix, an expression of depth d has its root at depth 1
and predicates at depth d, and/or nodes have `--fanout` children on
average, predicates are drawn from a pool sized so each is used
`--pred-share` times (18.35 by default, the paper's §6.1 figure), and
subexpression strings are reused from per-depth Zipf-ranked pools with a
per-depth probability (`--share`; the default 54% reproduces the paper's
4.33 uses per subexpression, and `--ads` uses the Figure 7(a) profile).
The benchmark prints the achieved predicate and subexpression sharing.
`--verify K` compares every search against brute-force evaluation of the
first K expressions. `make bench-check` runs the quick preset and compares
the deterministic counts (nodes, edges, bytes, matches, nodes visited,
predicates evaluated) with `bench/baseline.json`, failing on a drift above
5%. Latencies are printed but not gated: CI machines vary too much. Updating
the baseline is a deliberate commit.

## Build and quality gates

- Strict C99 (`-std=c99 -Wpedantic`) plus the warning set in the Makefile,
  `-Werror` by default. The header is additionally compiled as C11, C17 and
  C++11 by `make check-header`.
- `make check-asan` / `check-ubsan` / `check-tsan` / `check-valgrind` rebuild
  into separate directories with the corresponding instrumentation.
- CI matrix: gcc, clang, ASan+UBSan, TSan, valgrind, 32-bit, macOS, MSVC
  (CMake), clang-format check.

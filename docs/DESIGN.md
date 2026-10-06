# libatree design notes

This document describes what is implemented. `PLAN.md` describes what will
be; as milestones land, their sections move here and are kept in sync with
the code.

## Module map (M0–M4)

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
| `src/node.[h]` | 64-byte `struct atree__node` (kind, flags, level, hash, use_count, access_child, children, parents, pred slot, index slot) with a static size assertion; node and predicate slabs; the probe type for identity lookups. |
| `src/identity.[ch]` | Paper's expression-to-node table H_en: open-addressing set of node ids keyed by structural hash; lookups compare the full structure (operator + sorted child ids, or the predicate), so a hash collision can never merge two subexpressions. |
| `src/tree.c` | Lifecycle, attributes, stats, and index construction: `build()` recurses over the normalized expression, reusing nodes found in the identity table and otherwise creating and linking them (Alg. 1/4); subscriptions attach to the root node (`use_count` = parents + subscriptions); `cascade()` is the iterative Alg. 5 deletion; a failed insert rolls back by cascading over the nodes it created; `atree_validate` checks every invariant. |
| `src/search.c` | Report object (per-thread scratch: two bitsets, one queue per level, match list, counters) and Alg. 6 matching with zero suppression and propagation on demand; reset walks the level queues (dirty list) instead of clearing bitsets. Conveniences: callback delivery, exists, allow-list filtering. |
| `extras/atree_lock_pthread.h` | Header-only `atree_lock_t` adapter over `pthread_rwlock_t`. |
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
  the input, so stack use is bounded too.
- **Soundness.** These rewrites are identities of Kleene three-valued logic,
  so normalization preserves the exact three-valued result, not only
  "true vs not true". `test_expr` checks this on 400 random expressions × 40
  random events (with undefined attributes, NaN floats, unknown strings) and
  checks idempotence and normal-form structure. The XOR expansion duplicates
  its operands, so nested XOR grows exponentially; the depth limit bounds it.
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

**Identity.** A leaf's identity is its normalized predicate (string ids
interned at insert); an inner node's identity is (operator, sorted unique
child ids). Both are hashed and then compared structurally. Commutativity
and associativity are therefore free: `a and b` and `b and a` are one node,
and `(a and b) and c` is `AND(a, b, c)` after flattening.

**Use counts and sharing.** `use_count` is the number of parent links plus
attached subscription ids. Inserting an expression that already exists
attaches the id to the existing root (paper Alg. 4 line 3). Deleting
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
level 1. Phase 2 drains level queues bottom-up; an inner node is evaluated
with bit lookups over its children (all at lower levels, hence final), a
true node emits its subscriptions and enqueues its parents, and an AND
parent is enqueued only by its access child. The access child is the child
with the lowest wake rank (equality/membership < ranges < AND < OR < bool <
negated forms), ties broken by lower level, fewer children, lower id, so
the choice is deterministic for a given insertion sequence. Figure 6 of the
paper is a test with exact visit counts; the two-valued inner evaluation is
sound because NOT has been eliminated (PLAN §4.6).

**Reader isolation.** Searches write only to the report. `test_threads`
runs eight unlocked readers against one tree and compares every result with
the single-threaded answer, then readers plus a churning writer through the
pthread adapter (soundness of every returned id, structure validated by the
writer), then a build-swap-retire sequence; `make check-tsan` runs it under
ThreadSanitizer.

## Build and quality gates

- Strict C99 (`-std=c99 -Wpedantic`) plus the warning set in the Makefile,
  `-Werror` by default. The header is additionally compiled as C11, C17 and
  C++11 by `make check-header`.
- `make check-asan` / `check-ubsan` / `check-tsan` / `check-valgrind` rebuild
  into separate directories with the corresponding instrumentation.
- CI matrix: gcc, clang, ASan+UBSan, TSan, valgrind, 32-bit, macOS, MSVC
  (CMake), clang-format check.

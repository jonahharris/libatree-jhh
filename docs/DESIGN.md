# libatree design notes

This document describes the implementation module by module and records the
design decisions behind it. `PLAN.md` is the plan the library was built from
and keeps the paper-to-code mapping (§9.2) and the performance rationale
(§9.3).

Terms: an *expression* is a Boolean formula over the attributes; a
*subscription* is an expression registered under a caller-chosen 64-bit id
(the public API's term, and the paper's); the shell and the server call a
subscription a *continuous query*, and a client that asks to be told about a
query's matches a *subscriber*. A *node* is a leaf (one distinct predicate)
or an AND/OR inner node of the shared DAG; *edges* are parent-child links.

## Module map

| Module | Purpose |
|---|---|
| `include/atree.h` | The public API. Opaque handles, status codes, vtables for allocator and lock. |
| `src/compiler.h` | The only file with compiler/platform conditionals: `ATREE_STATIC_ASSERT`, `ATREE_UNUSED`, branch hints. |
| `src/alloc.[ch]` | `struct atree__mem` wraps the user's `atree_allocator_t` and tracks live/peak bytes and call counts. All library allocations go through `atree__alloc/zalloc/realloc/free` and the overflow-checked `*_array` helpers. Size 0 is never forwarded; `free`/`realloc` always receive the exact size previously requested. |
| `src/slab.[ch]` | `ATREE_SLAB_DEFINE(name, T)` generates a typed segmented slab for the arrays indexed by node or predicate id (nodes, predicates, flat sums, subscription slots, leaf positions, marks, index list positions): fixed segments of 2^14 elements behind a small table, so growth allocates one segment and copies nothing, slack is at most one segment, the peak is the live size and elements never move once their segment is full. Only the first segment doubles up to a full one. Access is `segs[i >> 14][i & mask]`; hot loops hoist the table pointer with `name_from`. |
| `src/vec.h` | `ATREE_VEC_DEFINE(name, T)` generates a typed `{T *data; uint32_t len, cap;}` vector with init/free/reserve/push/insert_at/remove_at/swap_remove/clear. Growth is geometric (×2, minimum 4) via one shared helper `atree__vec_grow` that returns the old pointer unchanged on failure. No type punning: the helper returns `void *` that is implicitly converted. |
| `src/hash.[ch]` | Open-addressing, linear-probing maps with a separate state byte per slot (empty/full/tombstone), power-of-two capacity, rehash at 70% occupancy counting tombstones; a rehash drops tombstones so churn never grows the table. `atree__u64map` (u64→u32) backs the subscription map and the index's per-attribute equality and membership maps (the identity and content tables are the separate set type in `identity.[ch]`); `atree__strmap` ((bytes,len)→u32) backs string interning and borrows key bytes. Hashing: splitmix64 finalizer for integers, FNV-1a + finalizer for bytes. |
| `src/strtab.[ch]` | String interning. Id 0 is the "unknown" sentinel so an event string the tree has never seen equals no literal. Ids are dense, assigned from 1, never reused. Bytes live in 64 KiB chunks (or a dedicated chunk for longer strings) so pointers are stable; the map borrows them. |
| `src/stats.c` | `atree_version`, `atree_strerror`, `atree_config_init`, and `atree__vec_grow`. |
| `src/atree_internal.h` | `struct atree`: allocator context, resolved config, lock copy, attribute and string tables, the DAG (node and predicate slabs, free lists, identity and content tables, flat sums, leaf list, level counts, phase-1 index), the subscription structures (id→node map, per-node slot, lists, constant-true ids), writer-side scratch (insert journal, normalization arena, worklist, operand stack, marks) and the cumulative counters; plus the `atree__rdlock/wrlock` helpers that are no-ops without an injected lock. |
| `src/keywords.[ch]` | DSL keyword list and identifier syntax shared by the attribute table and the lexer. Case-insensitive keyword match, no `<ctype.h>`. |
| `src/attr.[ch]` | Attribute table: validated, copied names (identifier syntax, not a keyword, unique), dense ids in declaration order, name→id map. |
| `src/value.[ch]` | `struct atree__value` tagged union for event values and literal operands; sorted-unique list helpers (sort, binary search, intersect, contains-all merge walks); double helpers (`atree__double_eq` is the only exact comparison, `-0.0` canonicalized for hashing, exact int→double promotion up to 2^53). |
| `src/writer.[ch]` | Output over `atree_write_fn`: integers, round-trip doubles that always lex as floats, quoted strings with escapes; becomes a no-op once the callback cancels. |
| `src/predicate.[ch]` | Leaves: `<attr, kind, op, operand>`. `atree__pred_check` applies the type table and normalizes operands; `negate` is an exact involution for every kind; `eval` is three-valued; hash/equal are structural; cost and wake rank feed the access-child choice; `print` renders DSL. |
| `src/expr.[ch]` | Caller-facing expression trees: public builders (`atree_expr_*`), internal constructors for the parser, `atree__expr_normalize` (the zero suppression filter) allocating its copy from a bump arena under the `max_expr_nodes` budget, structural hash and compare (a leaf's hash is its content hash, `atree__expr_leaf_hash`, which is also its identity in the tree), string-literal resolution by lookup (read path) or interning (write path), the three-valued reference evaluator `atree_expr_eval`, and the DSL printer `atree_expr_print`. Each caller-built node owns its allocations through its own `struct atree__mem`. |
| `src/event.[ch]` | Public event object: dense value array by attribute id, reusable per-attribute list buffers, strings interned by lookup (unknown → sentinel 0), lists sorted and deduplicated, NaN stored as undefined; keeps the list of attribute ids it defines (`defined`, maintained by the setters) so phase 1 and `atree_event_clear` touch only those. Own allocator counters. |
| `src/lexer.[ch]` | Pull-based, allocation-free tokenizer. Tokens carry byte offsets and lengths; string tokens record the quoted span and are unescaped by the parser into its scratch buffer. Integers are range-checked without `strtoll`; floats use `strtod` and must be finite. Also hosts `atree__error_set*`. |
| `src/parser.c` | Recursive descent for the DSL (`atree_expr_parse`). One-token lookahead plus a lexer-state copy for the two-word keywords (`not in`, `one of`, `is not null`, ...). `and`/`or` chains become one n-ary node; `xor`/`xnor` are binary and left-associative; `between` lowers to two comparisons; `literal in list_attr` lowers to `one of`. Nesting (parentheses and `not`) is bounded by `max_depth` as a recursion guard. Every failure path records a status, offset and message once (`fail`), and later steps become no-ops. |
| `src/node.h` | 64-byte `struct atree__node` (kind, flags, 16-bit level, hash, access child, predicate slot, children, parents, and the three boundaries that split the parent list into anchored/waker regions) with a static size assertion; the node and predicate slab types (slab.h); `struct atree__sublist`, a node's subscription ids with the first stored inline; the probe type for identity lookups. |
| `src/identity.[ch]` | Paper's expression-to-node table (its H_en): open-addressing set of node ids keyed by structural hash; lookups compare the full structure (operator + sorted child ids, or the predicate), so a hash collision can never merge two subexpressions. Rehashes in place when tombstones alone trip the 70% load factor, so insert/delete churn never grows it; `reserved` counts slots a pending rollback may need, kept free under the load factor so a rollback re-insert never allocates. A second set of the same shape, the content table, files inner nodes under their flat content hash for the lookup-first insert. |
| `src/tree.c` | Lifecycle (`atree_create` with config validation and defaults, `atree_destroy`), attribute queries, `atree_stats`, and the writers. `atree_insert_expr` normalizes into the tree's arena, prehashes every subexpression and looks it up first (identity table for leaves, content table for inner nodes, every hit verified structurally), builds only what is new with reorganize (Alg. 2) and self-adjust (Alg. 3, with relevel and identity re-keying), and journals every change so a failed insert rolls back exactly. Subscriptions attach to the root node through the per-node `sub_slot` slab into `struct atree__sublist`; the paper's use count is derived (`parents.len`, HAS_SUBS). `cascade()` is the iterative Alg. 5 deletion; `atree_validate` checks every invariant. |
| `src/index.[ch]` | Per-attribute phase-1 indexes: bool true/false lists, equality and membership hash buckets (a membership leaf sits in one bucket per element), sorted ray arrays for range comparisons probed as prefix/suffix, an `is null` list seeded only when the attribute is undefined, and a scan list for negated and list-containment forms. The probe walks only the attributes the event defines plus those with `is null` leaves (`null_attrs`), never the schema. O(1) removal from lists via a per-node position slab; buckets and rays are found by key. |
| `src/search.c` | Report object (per-thread scratch: two bitsets, one queue per level, match list, counters) and Alg. 6 matching with zero suppression and propagation on demand; reset walks the level queues (dirty list) instead of clearing bitsets. Conveniences: callback delivery, exists, allow-list filtering. |
| `src/graphviz.c` | DOT export: one rank per level, leaves at the bottom, parent→child edges, access-child edges in bold, subscription ids as external labels, constant-true ids in a note. Predicate text is DSL-escaped for DOT. |
| `extras/atree_lock_pthread.h`, `extras/atree_lock_win32.h` | Header-only `atree_lock_t` adapters over `pthread_rwlock_t` and `SRWLOCK`. |
| `examples/basic.c`, `examples/threads.c`, `examples/advanced.c` | Complete, commented programs over the public API that assert their own results: the walkthrough, a tree shared between threads through the pthread adapter, and the secondary APIs (callback/exists/allow-list search, custom allocator, flags, validate, Graphviz). Built with the full warning set and run by `make check` and `ctest`, as is the README's opening snippet (`make check-readme`), so the documentation cannot drift from the API. |
| `tools/atree_shell.c` | Line-oriented shell over the public API: a stdin REPL, a `select()` server (TCP on loopback or a Unix socket) that runs one tree for many clients and sends `NOTIFY` lines to the owner of a matched continuous query, and the client. Single-threaded, so the tree needs no lock; a client's queries are deleted when it disconnects. Shares the text formats with `bench_file` through `bench/bench_format.h`. Smoke-tested by `tests/shell_smoke.sh`. |
| `server/atreed.c`, `server/json.c` | The continuous-query server: one tree served to Redis, Postgres and HTTP clients on one port over pogocache's event loop (`server/deps/pogocache`, vendored MIT). Commands are written once against the argument list every protocol parses to; matches are delivered per subscriber protocol (pub/sub message, streamed `WATCH` rows, `LISTEN` notifications, SSE frames). One loop thread, so no lock and same-thread flushes for pushes. `json.c` is the event-object reader. `server/tests/run.py` speaks the three wire protocols raw. |
| `bench/bench_synthetic.c`, `bench/bench_file.c` | Synthetic workload after the paper's ABE-Gen generator (§6.1) (Zipf dimensions/values, operator mix, shared subexpressions, `--verify` brute-force check, `--check` baseline gate) and a file-driven benchmark; `bench/convert_rust_search_json.py` converts the Rust crate's dataset. |

## Allocator and container invariants

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

## Predicate semantics

- **Three-valued leaves** (paper §3.2): an undefined attribute makes every
  predicate `undefined` except `is null` (true) and `is not null` (false).
  `is empty`/`is not empty` on an undefined list are undefined.
- **Negation is exact** for every kind (`atree__pred_negate`), which is what
  lets normalization push NOT into leaves without changing results; the test
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

## Expressions and normalization

- **Deferred interning.** Builders and the parser take `const atree_t *`, so
  string literals stay as raw bytes inside the expression (sorted bytewise,
  unique). They are interned by `atree_insert_expr` (write path) and
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

## DSL

Grammar and precedence are in PLAN.md §3 and at the top of `src/parser.c`
(tightest first: `not`/`!`, `and`/`&&`, `xor`/`xnor`, `or`/`||`, as in C).
Keywords are case-insensitive; attribute names are case-sensitive
identifiers. Compatible with the Rust crate's language; additions: `xor`,
`xnor`, `between`, `true`/`false`, `literal in list_attr`, `!=`, `==`.

Diagnostics: `atree_error_t` gets the status, the byte offset and length
of the offending token or predicate, and a message such as
`attribute 'city' of type string: operator or literal not applicable`.
Type errors point at the start of the predicate; lexical errors at the
character. `test_parser` checks about sixty error inputs for status and
offset. The parser never calls the allocator for the expression itself
except through the expression constructors; its scratch (list buffers,
unescaped strings) is balanced per call. The libFuzzer harness in
`fuzz/fuzz_parser.c` is compiled and replayed over `fuzz/corpus/` by
`make check` so it cannot rot.

## The DAG

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
steady-state insert allocates only what the index keeps (about 8 allocator
calls per expression on the synthetic workload: children and parent arrays,
list operands, index buckets and amortized slab growth).

**Delete (Alg. 5).** Deleting detaches the id from its node and cascades
over children whose use count reaches zero, iteratively, with a worklist
reserved up front so deletion cannot fail once it starts mutating. Removals
from parent lists, index lists and subscription lists are swap-removes.

**Rollback.** Insert reserves the cascade worklist first, then builds; on
any failure (allocation, limit) it cascades over the nodes it created,
newest first. Pre-existing nodes only ever return to their previous use
counts, so they survive. The string table is the one thing a failed insert
may leave behind: literals interned before the failure stay interned (they
are never freed anyway); the DAG, subscriptions and statistics are exactly
as before. `test_alloc_failure` fails every allocation point of a 14-insert
script and checks shape, validity, search results and leak-freedom each
time.

**Matching.** Phase 1 finds the leaves the event satisfies (through the
per-attribute indexes below; every leaf is evaluated only under
`ATREE_FLAG_NO_PREDICATE_INDEX`) and seeds them at level 1. Phase 2 drains level queues bottom-up; an AND node is evaluated
with bit lookups over its children (all at lower levels, hence final), an
OR node is true the moment it is woken (only a true child wakes it), a
true node emits its subscriptions (found through a per-node slot array
into the subscription lists; a node's first id is stored inline in its
list header and a heap list is allocated only for the second, since almost
every subscribed node carries exactly one id) and enqueues its parents, and an AND parent
is enqueued only by its access child. The matched ids are sorted
ascending by a least-significant-digit radix sort (11-bit digits, as many
passes as the largest id needs; `qsort` below 128 matches) in a scratch
array that grows with the match vector, so a warmed-up search still
allocates nothing. The access child is the child with the lowest wake rank
(equality and membership forms; then ranges, `all of` and `is empty`; then
AND; then OR; then bare bools and `is null`; then the negated forms), ties
broken by lower level, fewer children, lower id, so
the choice is deterministic for a given insertion sequence. Figure 6 of the
paper is a test with exact visit counts; the two-valued inner evaluation is
sound because NOT has been eliminated (PLAN §4.6).

**Reorganize (Alg. 2).** Before an inner node is looked up, its operand
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

**Self-adjust (Alg. 3).** After a new node N is created, every parent P
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
rewire and rollback can re-choose anchors without new failure paths. The
effect when it landed is recorded in CHANGELOG.md (insert throughput about
2× at 100k expressions and 4× at 1M); the index also has slightly fewer
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
later pushes were undone first), a re-keyed node returns to the identity
table through a slot the rewire reserved for it (`identity.reserved`, kept
free under the load factor by every forward insert and released at commit,
so the rollback insert cannot allocate or fail), and created nodes are
released by cascade over a pre-reserved worklist.
`test_alloc_failure` exercises every allocation point including the
self-adjust paths.

**Phase 1 with indexes.** The probe walks two short lists, not the
schema: the attributes that have `is null` leaves (`index.null_attrs`,
seeded when the event leaves them undefined) and the attributes the event
defines (the event keeps their ids in `defined`, maintained by its setters,
so `atree_event_clear` resets only those). For a defined attribute: bool →
one of two lists; scalar → equality bucket, membership bucket and both ray
arrays; list → the membership bucket of each element; then the attribute's
scan list is evaluated. With a thousand attributes this took a 5-pair event
from 31.5 to 29.8 µs p50 at 100k expressions (release build). `predicates_evaluated`
counts index hits plus scan evaluations, so it is independent of how many
predicates exist on attributes the event does not touch or with values the
event does not have (`test_perf` adds ten thousand such predicates and
checks the count is unchanged). `ATREE_FLAG_NO_PREDICATE_INDEX` keeps the
all-leaves scan as the oracle; the differential suite runs both.

**Reader isolation.** Searches write only to the report. `test_threads`
runs eight unlocked readers against one tree and compares every result with
the single-threaded answer, then readers plus a churning writer through the
pthread adapter (soundness of every returned id, structure validated by the
writer), then a build-swap-retire sequence in which the old tree is
destroyed only after every reader has moved to the new one (a reader
publishes the generation it fetched once it has released the old tree's
event and report, which are bound to that tree; a fixed grace period is not
safe on a loaded machine); `make check-tsan` runs it under ThreadSanitizer.

## Configuration and locking

`atree_config_t` (all zero means defaults): `allocator` (default
malloc/realloc/free), `lock` (default none), `flags` (the four
`ATREE_FLAG_NO_*` switches, which never change results), `max_depth`
(connective nesting a parser or builder accepts, default 64; also bounds
the recursion of normalization and evaluation), `initial_nodes` (node slab
reservation, default 1024, clamped to the node limit), `max_adjust_candidates`
(reorganize and self-adjust scan bound, default 4096) and `max_expr_nodes`
(normalized size budget, default 8192). Hard limits: 2^32 − 4 nodes,
2^32 − 1 elements per list, 65 535 levels; every one of them fails with
`ATREE_ERR_LIMIT`, and a failed insert leaves the tree unchanged.

With a lock in the configuration every public entry point takes it exactly
once, `rdlock` on the read paths (everything that takes `const atree_t *`,
including `atree_expr_eval`, which resolves literals through the string
table) and `wrlock` in `atree_insert*` and `atree_delete`, released on every
return path. Callbacks (`atree_match_fn`, `atree_write_fn`) run with the lock
held and must not re-enter the tree. `atree_destroy` takes no lock: it needs
the same exclusion as a writer, provided by the caller. Without a lock the
contract is the header's: read paths write nothing reachable from the tree
(`-Wcast-qual` is on and `const` is never cast away; per-call state lives in
the caller's event and report), so any number of them run concurrently, and
writers need exclusion from everything.

## Deviations from the paper

The paper is the specification; PLAN.md §9.2 maps every algorithm to code.
The deviations, all listed there as well:

- The access child of an AND node is chosen by wake rank, level, child count
  and id instead of randomly (§5.2.2 says "randomly"); the choice is
  deterministic and reproduces Figure 6.
- Self-adjust skips a rewrite whose result already exists as another node;
  the paper is silent and uniqueness requires it.
- `max_adjust_candidates` caps the reorganize and self-adjust scans; the cap
  can only reduce sharing, never correctness, and the counter
  `adjust_candidates_skipped` reports when it bound (on the synthetic
  workloads it never does).
- Alg. 4 looks a subexpression up before visiting its operands, as the paper
  does, but on a miss builds the operands before reorganizing, because
  reorganize needs their node ids.
- Phase 1 uses per-attribute indexes; the paper leaves the predicate-matching
  structures open.
- A float NaN in an event counts as undefined (the paper has no NaN).
- Normalization is bounded by `max_expr_nodes`; the paper has no such limit.

## Benchmarks and the regression gate

`make bench` builds `bench_synthetic` and `bench_file`. The synthetic
generator follows the paper's ABE-Gen generator (§6.1): dimensions and values are
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
  C++11 by `make check-header`; the examples and the README snippet are
  compiled with the same warnings and run by `make check-examples` and
  `make check-readme`, both part of `make check`.
- `make check-asan` / `check-ubsan` / `check-tsan` / `check-valgrind` rebuild
  into separate directories with the corresponding instrumentation.
- CI matrix (`.github/workflows/ci.yml`): gcc debug and release, gcc
  UBSan, clang ASan+UBSan, clang TSan, valgrind, 32-bit, macOS, MSVC and
  Linux through CMake, the server's protocol tests, the bench regression
  gate and the clang-format check.

# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- `atree_config_t.max_expr_nodes` (default 8192): the number of nodes a
  normalized expression may have. XOR/XNOR expand to AND/OR and double per
  nesting level, so a chain of 20 XORs, depth 21 and well inside
  `max_depth`, normalized in 0.6 s and 1.2 GB and each further XOR
  quadrupled that; a short subscription string was a denial of service.
  The budget is checked as nodes are allocated, so it bounds time as well
  as memory, and an expression over it fails with `ATREE_ERR_LIMIT`.

### Fixed
- Rolling back a failed insert re-filed a node that self-adjust had
  re-keyed under its old identity hash with an insert that could need to
  grow the table, and discarded the result; after an out-of-memory failure
  the node could be left out of the identity table. A rewire now reserves
  the slot its rollback needs, so the rollback insert never allocates.
- A search that ran out of memory could leave a true/queued bit set for a
  node whose queue push had failed; the reset clears only queued ids, so
  the next search on that report missed or fabricated matches. Bits are
  now set after the push succeeds. `test_alloc_failure` requires a retry
  after a failed search to return exactly the undisturbed count.

- The 32-bit CI job passed `CFLAGS=-m32` on the make command line, which
  replaces the Makefile's whole flag set, so that build ran without
  `-std=c99`, the warning set or `-Werror`. The Makefile now takes
  `EXTRA_CFLAGS`/`EXTRA_LDFLAGS`, which are appended.
- The identity and content tables rehash in place when tombstones alone
  trip the load factor instead of doubling; under insert/delete churn at a
  steady size they no longer grow without bound (20 000 cycles over 200
  live expressions grew them from 2048 to 16384 slots). `test_perf` gates
  it: bytes allocated after a long churn equal those after a short one.
### Changed
- Insert looks each normalized subexpression up before building it (paper
  Alg. 4 lines 1-4): leaves by a content hash that needs no string-table
  access, inner nodes through a new content table keyed by flat structure
  (so `AND(AND(a,b),c)` finds `AND(a,b,c)`), every hit verified
  structurally. Reorganize now runs only for nodes that are new.
- Normalization allocates the normalized copy from a single arena instead
  of one allocation per node and literal.
- Parent lists are kept in four regions (anchored/waker) so the search
  sweep never reads an AND parent it cannot wake; the node layout drops the
  stored use count and the leaf slot (both derived or kept beside the
  predicate slab) to fit the boundaries in 64 bytes.
- Leaf identity hashes string literals by content, so the same predicate
  text always hashes alike whether or not its strings are interned.
  Release-build effect of this group: 100k inserts 239k/s → 318k/s, 1M
  inserts 181k/s → 227k/s, search p50 −15% at 100k and −11% at 1M, 2× on
  the `--ads` sharing profile; memory +10% at 1M.
- The normalization arena, the insert journal and the operand buffer of
  `build_inner` are kept in the tree and reused instead of being allocated
  and freed per insert; the arena no longer zeroes blocks that are written
  in full. Allocator calls per insert on the synthetic workload: 12.2 + 4.2
  frees → 7.8 + 0.2; insert throughput +5–8% (release build).
- `make MODE=release` builds into `build-release/` so a debug `make check`
  can no longer leave -O0 objects for a release benchmark to link.
- Inner nodes record, per child, their position in the child's parent
  list: unlinking (delete, rollback) is O(log fan-out) instead of linear in
  the child's parent count; `atree_validate` checks positions directly and
  verifies the predicate index in one linear pass. Found by the
  1M-expression benchmark, where validation and deletion over popular
  leaves did not finish.
- Self-adjust scans only the child with the fewest parents (exact).
- Reorganize finds candidate covers through anchor lists: every inner node
  designates its least popular child as its anchor, each node keeps the
  parents it anchors at the front of its parent list, and the scan reads
  only those prefixes. The search is exact (a cover contains its anchor)
  and never walks a popular leaf's parent list. Insert throughput on the
  100k synthetic workload (release build): 116k/s → 239k/s; at 1M
  expressions 40.5k/s → 181k/s with the default `max_adjust_candidates`,
  and slightly fewer edges.
  Node levels are now 16-bit (`ATREE_ERR_LIMIT` above 65535, previously
  unreachable in practice); `atree_validate` checks the anchor partition.
- `bench_synthetic` reports predicates per expression, the predicate
  sharing ratio and parse throughput (parsing was never part of the timed
  insert), draws predicates from a Zipf-ranked pool with `--pred-pool N`,
  and gains the `--ads` preset that reproduces the sharing metrics of the
  paper's real workload (§6.2); `docs/COMPARISON.md` records that
  construction on that profile is 47× slower than the paper's figure and
  why (bottom-up build instead of Alg. 4's lookup-first insert).
- Search no longer evaluates a woken OR node (under zero suppression only
  a true child wakes it), sorts the matched ids with an LSD radix sort
  instead of `qsort` (a scratch array in the report, grown with the match
  vector), and finds a node's subscription list through a per-node slot
  array instead of a hash map. Release build, 1M expressions with the
  paper's sharing: search p50 4.6 ms → 2.3 ms (p99 9.1 → 4.0 ms); 100k:
  0.29 → 0.12 ms; insert throughput unchanged. About 97 ns per visited
  node.
- A predicate leaf is hashed once: the structural hash computed when the
  leaf is built, which normalization orders by, is the identity hash the
  insert probes with and files a new leaf under; before, three functions
  hashed the same bytes. Release build, interleaved A/B at 1M expressions
  with the paper's sharing: inserts +9% (now 254 000/s, 3.9 s); the `--ads`
  profile 112 000 → 127 000/s. `atree__pred_hash` and `atree__value_hash`
  (by interned string id) are removed. Two further changes were measured
  and declined, see `docs/COMPARISON.md`.
- `bench_synthetic` reproduces the paper's sharing by default: and/or
  nodes average `--fanout` children (the paper's "average number of child
  nodes"; previously 2..fanout), predicates come from a pool sized for
  `--pred-share` uses each (18.35, §6.1; the pool is text-deduplicated and
  filled at a calibrated rate so every slot is used), `--share` takes one
  reuse probability per depth (default 54%, measured 4.33 uses per
  subexpression at 1M; `--ads` uses the Figure 7(a) per-level profile),
  and the output reports predicate and subexpression sharing. The previous
  workload is `--fanout 3 --share 30 --pred-share 0`. `docs/COMPARISON.md`
  now compares against the paper's own synthetic curves (Figures 11–13)
  at matching parameters and sharing, and corrects the per-visited-node
  figure (about 185 ns in a release build, not 0.36 µs).
- `bench_synthetic` gains `--dump`, `--rust-compatible` and `--cap`;
  `bench/rust_compare` compares the Rust `a-tree` crate on identical files;
  `docs/COMPARISON.md` records the results. Baseline regenerated for the
  new edge layout.

## [0.1.0] - 2026-10-06

First release. Everything below was developed in milestones M0–M7.

### Added (M7 — release)
- `atree_to_graphviz` DOT export.
- `extras/atree_lock_win32.h` (SRWLOCK adapter); the thread test uses it on
  Windows.
- Benchmarks: `bench_synthetic` (ABE-Gen-style, `--verify`, `--check`),
  `bench_file`, the Rust crate's dataset converted under `bench/data/`,
  `bench/baseline.json` and the `make bench-check` CI gate.
- README with semantics, thread-safety, allocator and performance sections.

### Added (M6 — reorganize and self-adjust)
- Alg. 2 reorganize: incoming operand sets are rewritten in terms of
  existing nodes by greedy set cover, bounded by `max_adjust_candidates`.
- Alg. 3 self-adjust: existing parents whose operand set strictly contains
  a new node's are rewired to reuse it, with identity re-keying, access
  child re-choice and relevel. The index is now independent of arrival
  order.
- Journaled rollback covering rewires and level changes; identity table
  re-insertion reuses tombstones so rollback never allocates.
- `atree_stats.reorganized`, `self_adjusted`, `adjust_candidates_skipped`.
- Tests: paper Figure 5 and the §4.2.3 self-adjust example with exact node,
  edge and level counts (with and without the flags), Figure 6 now matches
  the paper's optimized structure exactly, arrival-order gate, differential
  suite over all 16 flag combinations.

### Added (M5 — predicate indexes)
- Per-attribute phase-1 indexes (`src/index.[ch]`): bool lists, equality
  and membership buckets, sorted ray arrays for ranges, `is null` lists,
  per-attribute scan lists. Phase-1 cost is now proportional to matched
  predicates plus scan-list leaves, not to the number of leaves.
- `atree_stats.indexed_leaves` / `scanned_leaves` report the routing;
  `atree_validate` checks every leaf is where its route says.
- `test_perf` index-independence gate.

### Added (M4 — tree core)
- `atree_insert`, `atree_insert_expr`, `atree_delete`, `atree_contains`,
  `atree_count`: node sharing through a structural identity table, use
  counts, iterative delete cascade, complete rollback on failed inserts.
- `atree_report_*`, `atree_search`, `atree_search_cb`, `atree_exists`,
  `atree_search_ids`: level-synchronous matching with zero suppression and
  propagation on demand; per-report scratch with dirty-list reset so a
  warmed-up search makes no allocator calls and never writes to the tree.
- `atree_validate` (every structural invariant) and full `atree_stats`.
- Optional injected reader/writer lock taken by every public call;
  `extras/atree_lock_pthread.h` adapter.
- Tests: `test_tree` (scenarios, paper Figures 4 and 6 with exact node and
  visit counts, constants, conveniences), `test_differential` (tree vs
  reference evaluator on random expressions/events under six flag
  combinations with churn and validation), `test_alloc_failure` (every
  allocation point), `test_threads` (unlocked readers, readers + writer
  with the pthread lock, build-swap-retire; `make check-tsan`),
  `test_perf` (zero suppression, propagation on demand, sharing,
  allocation-free steady state gates).

### Added (M3 — DSL)
- `atree_expr_parse`: hand-written lexer and recursive-descent parser for
  the full expression language (comparisons in both orientations, `in`/
  `not in`, `one of`/`none of`/`all of`, `is [not] null`, `is [not] empty`,
  `and`/`or`/`not`/`xor`/`xnor` with C-like precedence, `between`,
  `true`/`false`, `literal in list_attr`, `&&`/`||`/`!`/`!=`/`==`), with
  case-insensitive keywords and diagnostics carrying byte offsets.
- `fuzz/fuzz_parser.c` libFuzzer harness plus a seed corpus; `make check`
  compiles it with a replay `main()` and runs the corpus.
- Tests: `test_parser` (lexer tokens and errors, Rust parser tests ported,
  precedence, about fifty error cases with offsets, print/parse round trip
  over 500 random expressions, allocation failures at every point).

### Added (M2 — expressions)
- Public expression builder: `atree_expr_var/cmp_int/cmp_float/eq_string/
  in_ints/in_strings/list_ints/list_strings/null/true/false`, connectives
  `and/or/not/xor/xnor`, `atree_expr_free`, `atree_expr_print`,
  three-valued reference evaluator `atree_expr_eval`.
- Normalization (`atree__expr_normalize`): NOT push-down with exact
  predicate negation, De Morgan, XOR/XNOR expansion, flattening, constant
  folding, canonical ordering, deduplication, single-child collapse, depth
  limit (`ATREE_ERR_TOO_DEEP`).
- String literals are kept as raw bytes in expressions and interned only at
  insert time, so building and parsing expressions never writes to the tree.
- `tests/gen.h`: deterministic random schema/expression/event generator
  (seeded, `ATREE_TEST_SEED` override) for property and differential tests.
- Tests: `test_expr` (builders, printing, normalization identities ported
  from the Rust crate, depth limit, Table 2 semantics, unknown-literal
  semantics, normalization-preserves-evaluation property, lookup/intern,
  allocation failures at every point).

### Added (M1 — values, attributes, predicates, events)
- `atree_create` / `atree_destroy` with config validation (allocator, lock,
  flags, limits) and the attribute table; `atree_attr_lookup/count/name/type`;
  `atree_stats` (strings and byte counters).
- Predicates (`src/predicate.[ch]`): type checking against the attribute
  table, operand normalization, exact negation, three-valued evaluation,
  structural hash/equality, cost and wake-rank estimates, DSL printing.
- Values and sorted-list helpers (`src/value.[ch]`), DSL keyword table
  (`src/keywords.[ch]`), output writer (`src/writer.[ch]`).
- Public event API: `atree_event_create/destroy/clear` and all `set_*`
  functions by name and by attribute id; reusable buffers; NaN → undefined;
  unknown strings → sentinel.
- `ATREE_ERR_CANCELLED` status for callbacks that stop early.
- Tests: `test_value`, `test_attr`, `test_predicate`, `test_event`.

### Added (M0 — scaffold)
- Public header `include/atree.h` declaring the complete v0.1 API (status
  codes, allocator and lock vtables, configuration, attributes, expression
  builder, events, search/report, introspection).
- Allocator layer (`src/alloc.[ch]`) with exact-size accounting, overflow
  checked array helpers, and the default C-library allocator.
- Typed vector generator (`src/vec.h`), open-addressing hash maps
  (`src/hash.[ch]`), string interning table (`src/strtab.[ch]`).
- `atree_version`, `atree_strerror`, `atree_config_init`,
  `atree_default_allocator`.
- Test harness, counting/failing test allocator, unit tests for the above,
  header conformance checks for C99/C11/C17/C++.
- GNU Makefile and CMake build, pkg-config template, clang-format config,
  GitHub Actions CI (gcc, clang, ASan/UBSan, TSan, valgrind, 32-bit, macOS,
  MSVC, format check).

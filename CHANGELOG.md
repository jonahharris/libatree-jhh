# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed
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
  100k synthetic workload: 34k/s → 104k/s; at 1M expressions 14k/s →
  81k/s with the default `max_adjust_candidates`, and slightly fewer edges.
  Node levels are now 16-bit (`ATREE_ERR_LIMIT` above 65535, previously
  unreachable in practice); `atree_validate` checks the anchor partition.
- `bench_synthetic` reports predicates per expression, the predicate
  sharing ratio and parse throughput (parsing was never part of the timed
  insert), draws predicates from a Zipf-ranked pool with `--pred-pool N`,
  and gains the `--ads` preset that reproduces the sharing metrics of the
  paper's real workload (§6.2); `docs/COMPARISON.md` records that
  construction on that profile is 47× slower than the paper's figure and
  why (bottom-up build instead of Alg. 4's lookup-first insert).
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

# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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

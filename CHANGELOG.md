# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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

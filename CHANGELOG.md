# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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

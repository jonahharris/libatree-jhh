# CLAUDE.md — libatree

libatree is a standalone C99 library implementing the A-Tree (Ji & Jacobsen,
SIGMOD 2021) for matching events against large sets of arbitrary Boolean
expressions. Read `PLAN.md` before changing anything: it defines scope, the
public API, internal design, and the milestone order. `docs/DESIGN.md` must
stay in sync with the code.

`reference/` is **read-only input** (paper, Rust crate, be-tree, bplus-tree,
an embedded C A-Tree). Never modify it, never copy code from it verbatim
(different licenses and quality bars); re-derive and cite the idea in a
comment instead.

## Commands

```sh
make                 # libatree.a + shared library (debug by default; MODE=release)
make check           # build and run all tests
make check-asan      # tests under -fsanitize=address,undefined -fno-sanitize-recover=all
make check-ubsan     # tests under UBSan only (gcc)
make check-tsan      # tests/test_threads under -fsanitize=thread (clang)
make check-valgrind  # tests under valgrind --error-exitcode=1 --leak-check=full
make bench           # build benchmarks (bench/ output explains how to run)
make fuzz            # build libFuzzer harness (clang only)
make format          # clang-format in place;  make format-check in CI
cmake -B build && cmake --build build && ctest --test-dir build   # CMake path (also MSVC)
```

Run a single test binary directly: `./build/tests/test_parser`. Every test
executable exits nonzero on failure and prints `file:line` for each failed
assertion.

## Definition of done (every change)

1. `make check`, `make check-asan`, `make check-tsan`, and
   `make check-valgrind` pass.
2. Builds with `-Werror` under both `gcc` and `clang` with the full warning
   set from the Makefile. Nothing is silenced with pragmas or casts added
   "to make the warning go away"; fix the type instead.
3. `include/atree.h` still compiles as C99, C11, C17, and C++ (`make
   check-header`).
4. New public behavior has unit tests; anything touching insert/delete/search
   also runs through `tests/test_differential.c` (random expressions vs the
   reference evaluator) and `tests/test_alloc_failure.c`.
5. `docs/DESIGN.md`, header doc comments, `README.md`, and `CHANGELOG.md`
   updated when behavior or API changes.
6. `make format-check` clean.

## Language and portability rules

- Strictly conforming **C99**. No GNU extensions, no VLAs, no statement
  expressions, no `typeof`, no nested functions, no `__attribute__` except
  behind the `ATREE_API` / `ATREE_UNUSED` macros in `src/compiler.h`. Code
  must also compile as C11/C17 and under MSVC (`/W4 /WX /std:c11`), so stay in
  the subset all three accept.
- Only `<stddef.h> <stdint.h> <stdbool.h> <string.h> <stdlib.h> <limits.h>
  <math.h> <stdio.h>` in the library (`<stdio.h>` only for `snprintf`).
  No POSIX, no `<pthread.h>`, no platform `#ifdef`s in `src/` except in
  `src/compiler.h`. Benchmarks may use a tiny clock shim.
- Any-endian, 32- and 64-bit clean: use `size_t` for sizes and counts,
  fixed-width types for stored data, `uint32_t` node ids, `PRIu64`-style
  format macros or casts to `unsigned long long` in printf.
- Public header: self-contained, `extern "C"` guarded, `ATREE_H` include
  guard (never a leading underscore), opaque handles only, no internal types,
  no macros that expand to statements. Every function documented.

## No-undefined-behavior rules (enforced, not aspirational)

- **No signed overflow.** Hashes and bit manipulation use unsigned types.
  Before any `a + b`, `a * b` on sizes or indexes, check against
  `SIZE_MAX`/`UINT32_MAX` (`atree__mul_overflows`, `atree__add_overflows` in
  `src/alloc.h`). Never compute `x - y` on `int64_t` to compare; use `<`/`>`.
  Never negate `INT64_MIN`.
- **No type punning through pointers.** Bytes ↔ integers/doubles go through
  `memcpy`. No `*(uint64_t *)buf`. No unions read through a member other than
  the one last written (tagged unions must switch on the tag first).
- **No shifts ≥ width or of negative values.** `1u << n` requires `n < 32`;
  use `UINT64_C(1) << (i % 64)` for bitsets.
- **No NULL/zero-length library calls.** Guard `memcpy`/`memmove`/`memset`/
  `qsort` against `n == 0` with a NULL pointer. Never `realloc(p, 0)`. Never
  call the user allocator with size 0.
- **No out-of-bounds, no uninitialized reads.** `calloc`-style zeroing or
  explicit init for every struct; `vec` macros assert index < len in debug.
- **No pointer arithmetic beyond one-past-the-end**, no comparing pointers
  into different allocations (order nodes by `node_id`, never by address).
- **Floats:** no comparisons with `==`/`!=` outside
  `atree__double_eq()` (which documents exact-equality semantics and handles
  `-0.0`); check `isnan`/`isinf` on every double entering the library;
  never convert out-of-range doubles to integers.
- **Strings:** always length-delimited (`const char *s, size_t len`); only
  `(unsigned char)` casts for `ctype`-like checks (write our own
  `atree__is_ident_char`, do not use `<ctype.h>`); never assume NUL
  termination of caller data except where the API says `SIZE_MAX → strlen`.
- **Recursion** is bounded by `max_depth` (default 64) and only over the
  caller's expression tree; everything over the DAG (delete cascade,
  relevel, search) is iterative with explicit worklists.
- `qsort` comparators return a consistent total order and never subtract.
- Compile-time assumptions are checked with `ATREE_STATIC_ASSERT`.

## Memory rules

- Every allocation goes through `atree__alloc / atree__realloc / atree__free`
  (which call the tree's `atree_allocator_t`) or the array helpers
  `atree__alloc_array(tree, n, elem_size)`. Direct `malloc`/`calloc`/
  `realloc`/`free`/`strdup` appear only in `atree_default_allocator()` and in
  tests/benches.
- Pass exact sizes to `free`/`realloc` (pool allocators depend on it).
  Track live bytes in `atree_stats.bytes_allocated`.
- Geometric growth only (`cap = cap ? cap * 2 : initial`), overflow-checked.
- **Strong guarantee on failure:** `atree_insert*` either completes or leaves
  the tree bit-for-bit equivalent to before (journal + rollback, see PLAN
  §4.7). Every `ATREE_ERR_NOMEM` path is exercised by
  `tests/test_alloc_failure.c`; after `atree_destroy`, live bytes must be 0.
- Objects are reusable: events, reports, and their scratch buffers keep
  capacity across uses so steady-state matching allocates nothing.

## Thread-safety rules

- Any function that takes `const atree_t *` is a **read path** and must not
  write to any memory reachable from the tree: no stats counters, no epoch
  stamps, no lazy caches, no "harmless" flags. `-Wcast-qual` is on; never
  cast const away. Per-call state goes in the caller's `atree_event_t` or
  `atree_report_t`.
- Read paths may call the allocator only when creating or growing an event
  or report. A warmed-up search makes zero allocator calls
  (`tests/test_perf.c` asserts this).
- Writer-side scratch (mark arrays, journals) lives in the tree and is only
  touched by `atree_insert*` / `atree_delete`.
- When `config.lock` is set, every public entry point takes it exactly once
  (`rdlock` for read paths, `wrlock` for writers) and releases it on every
  return path, including error paths. User callbacks run with the lock held
  and this is documented at each callback typedef.
- `tests/test_threads.c` must pass under ThreadSanitizer (`make
  check-tsan`) for any change to `src/search.c`, `src/event.c`,
  `src/report.c`, or `src/tree.c`.

## Fidelity rules

- The paper is the specification: `PLAN.md` §9.2 maps every algorithm
  (Alg. 1–6, §5.2.1, §5.2.2) to code and lists the only permitted
  deviations. Do not add a deviation without updating that table.
- Each algorithm's implementation cites its paper section in a comment
  (`/* Alg. 2 (§4.2.2): greedy set cover */`).
- The Figure 4, Figure 5, §4.2.3 and Figure 6 worked examples are tests with
  exact expected node/edge counts and visit sets; they must keep passing.

## Performance rules

- Nothing in a search may be O(total nodes): no full bitset clears, no
  scans over all leaves when indexes are on, no per-search allocation after
  warm-up. Reset cost is proportional to nodes touched (dirty lists).
- `struct node` stays within 64 bytes; cold data (predicates, subscription
  lists) lives in side tables. Check `sizeof` with `ATREE_STATIC_ASSERT`.
- All growth is geometric; all removals are swap-remove or hash erase.
- Work-count gates in `tests/test_perf.c` and the `bench/baseline.json`
  regression gates (PLAN §9.3) must pass. If a change legitimately moves the
  baseline, update it in a separate commit that states why.

## Error handling

- Every fallible function returns `atree_status_t`. `0 == ATREE_OK`. Never
  `abort()`, `exit()`, or print from the library. Use `atree_error_t` (fixed
  buffer, caller-owned) for diagnostics with byte offsets.
- `assert()` is for internal invariants only (never for argument validation),
  and the code must be correct with `NDEBUG` defined.
- Check every return value; `(void)` casts of statuses are forbidden.

## Code style

- `clang-format` (WebKit-based, 100 columns, 4-space indent, `int *p`,
  braces on all control statements). Run `make format`.
- Naming: public `atree_*` / `ATREE_*`; internal non-static `atree__*`;
  file-local `static`. Types end in `_t` only in the public header.
- One module per concern (see PLAN §4.1). Headers declare, `.c` files define.
  No function longer than a screen without a reason stated in a comment.
- Comments explain *why* and cite the paper section or algorithm number
  (`/* Alg. 2, reorganize */`), not *what* the next line does.
- No dead code, no commented-out code, no TODOs without an issue reference in
  the same commit.

## Testing conventions

- Harness is `tests/test.h` (minunit-style). Tests are C, use only the public
  API except for white-box tests that explicitly `#include "../src/..."` and
  say so in a comment at the top.
- Deterministic: the differential suite's PRNG is seeded from a constant and
  from `ATREE_TEST_SEED` if set; failures print the seed and the offending
  expression/event so they can be replayed.
- Sanitizer and valgrind runs are part of the normal loop, not CI-only.
- Never weaken a test to make it pass. If the reference evaluator and the tree
  disagree, the tree is wrong until proven otherwise.

## Git

- Small, single-purpose commits with imperative subjects
  (`tree: implement delete cascade (Alg. 5)`). Body explains the why.
- Do not commit build output, generated files, or anything under `reference/`.
- Tag releases `vMAJOR.MINOR.PATCH`; `CHANGELOG.md` follows Keep a Changelog.

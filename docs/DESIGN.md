# libatree design notes

This document describes what is implemented. `PLAN.md` describes what will
be; as milestones land, their sections move here and are kept in sync with
the code.

## Module map (M0, M1)

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

## Build and quality gates

- Strict C99 (`-std=c99 -Wpedantic`) plus the warning set in the Makefile,
  `-Werror` by default. The header is additionally compiled as C11, C17 and
  C++11 by `make check-header`.
- `make check-asan` / `check-ubsan` / `check-tsan` / `check-valgrind` rebuild
  into separate directories with the corresponding instrumentation.
- CI matrix: gcc, clang, ASan+UBSan, TSan, valgrind, 32-bit, macOS, MSVC
  (CMake), clang-format check.

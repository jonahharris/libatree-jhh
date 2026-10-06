# libatree

A standalone, portable C99 library implementing the **A-Tree** data structure
from Ji & Jacobsen, *"A-Tree: A Dynamic Data Structure for Efficiently
Indexing Arbitrary Boolean Expressions"* (SIGMOD 2021).

An A-Tree indexes a very large set of arbitrary Boolean expressions over typed
attributes so that one event (an assignment of values to attributes) retrieves
every expression it satisfies without evaluating each one. Typical uses:
advertising exchanges, complex event processing, publish/subscribe filtering,
alert routing.

> **Status: pre-release, under construction.** Milestone M0 (scaffold) is
> complete: public header, allocator layer, containers, build system, tests,
> CI. The tree itself arrives in M4. See `PLAN.md` for the full design and
> `CHANGELOG.md` for progress.

## Design goals

- **Faithful to the paper**: node sharing with structural identity, expression
  reorganization (Alg. 2), index self-adjustment (Alg. 3), use-count deletion
  (Alg. 5), level-synchronous matching (Alg. 6) with the zero suppression
  filter and propagation on demand, three-valued predicate semantics.
- **Standalone**: C99, no dependencies, no generated code. Builds with gcc,
  clang and MSVC on Linux, macOS and Windows, 32- and 64-bit.
- **No undefined behavior**: clean under ASan, UBSan, TSan and valgrind with
  `-Wall -Wextra -Wpedantic -Wconversion ... -Werror`.
- **Custom allocator**: every allocation goes through a caller-supplied
  allocator with exact sizes, so pools and arenas work.
- **Thread-safe reads**: searches never write to tree memory, so any number of
  threads can match concurrently without locks; an optional injected
  reader/writer lock makes the whole tree shareable.
- **Allocation-free steady state**: events and reports are reusable.

## Building

```sh
make                 # libatree.a and the shared library (MODE=release for -O2)
make check           # unit tests + header conformance checks (C99/C11/C17/C++)
make check-asan      # same under AddressSanitizer + UBSan
make check-tsan      # thread tests under ThreadSanitizer
make check-valgrind  # same under valgrind (Linux)
make install PREFIX=/usr/local
```

Or with CMake (also for MSVC):

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

## Quick start (API preview; functional from M4)

```c
#include <atree.h>

atree_attr_def_t attrs[] = {
    {"exchange_id", ATREE_TYPE_INT},
    {"country",     ATREE_TYPE_STRING},
    {"deal_ids",    ATREE_TYPE_STRING_LIST},
    {"private",     ATREE_TYPE_BOOL},
};
atree_t *tree;
atree_create(NULL, attrs, 4, &tree);

atree_error_t err;
atree_insert(tree, 1, "exchange_id = 1 and deal_ids one of ['deal-1', 'deal-2']", SIZE_MAX, &err);
atree_insert(tree, 2, "not private and country in ['CA', 'US']", SIZE_MAX, &err);

atree_event_t *ev;  atree_event_create(tree, &ev);
atree_event_set_int(ev, "exchange_id", 1);
atree_event_set_string(ev, "country", "US", 2);
const char *deals[] = {"deal-2"}; size_t lens[] = {6};
atree_event_set_string_list(ev, "deal_ids", deals, lens, 1);

atree_report_t *rep; atree_report_create(tree, &rep);
atree_search(tree, ev, rep);
for (size_t i = 0; i < atree_report_count(rep); i++)
    printf("matched %llu\n", (unsigned long long)atree_report_matches(rep)[i]);

atree_report_destroy(rep); atree_event_destroy(ev); atree_destroy(tree);
```

## Expression language

Documented in `PLAN.md` §3 until M3 lands; it is compatible with the Rust
`a-tree` crate's DSL and adds `xor`, `xnor`, `between`, `true`/`false`.

## License

MIT. See `LICENSE`.

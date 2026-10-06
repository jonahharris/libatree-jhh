# libatree

A standalone, portable C99 library implementing the **A-Tree** data structure
from Ji & Jacobsen, *"A-Tree: A Dynamic Data Structure for Efficiently
Indexing Arbitrary Boolean Expressions"* (SIGMOD 2021).

An A-Tree indexes a very large set of arbitrary Boolean expressions over typed
attributes so that one event (an assignment of values to attributes) retrieves
every expression it satisfies without evaluating each one. Typical uses:
advertising exchanges, complex event processing, publish/subscribe filtering,
alert routing.

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

atree_event_t *ev;
atree_event_create(tree, &ev);
atree_event_set_int(ev, "exchange_id", 1);
atree_event_set_string(ev, "country", "US", 2);
const char *deals[] = {"deal-2"};
atree_event_set_string_list(ev, "deal_ids", deals, NULL, 1);
atree_event_set_bool(ev, "private", false);

atree_report_t *rep;
atree_report_create(tree, &rep);
atree_search(tree, ev, rep);
for (size_t i = 0; i < atree_report_count(rep); i++)
    printf("matched %llu\n", (unsigned long long)atree_report_matches(rep)[i]);   /* 1 and 2 */

atree_report_destroy(rep);
atree_event_destroy(ev);
atree_destroy(tree);
```

## Why this library

- **Faithful to the paper.** Every algorithm is implemented: node sharing
  through a structural identity table (§4.2.1), expression reorganization by
  greedy set cover (Alg. 2), index self-adjustment (Alg. 3), use-count
  deletion (Alg. 5), level-synchronous matching (Alg. 6) with the zero
  suppression filter (§5.2.1) and propagation on demand (§5.2.2), and
  three-valued predicate semantics (§3.2). The paper's worked examples
  (Figures 4, 5, 6 and the §4.2.3 example) are tests with exact node, edge
  and visit counts. `docs/DESIGN.md` and `PLAN.md` §9.2 list the few
  deviations.
- **Standalone.** C99, no dependencies, no generated code. Builds with gcc,
  clang and MSVC on Linux, macOS and Windows, 32- and 64-bit.
- **No undefined behavior.** Clean under AddressSanitizer, UBSan,
  ThreadSanitizer and valgrind with `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow -Wfloat-equal ... -Werror`. Every allocation failure is tested at
  every allocation point; a failed insert leaves the index exactly as it was.
- **Custom allocator.** Every allocation goes through a caller-supplied
  allocator with exact sizes and a context pointer, so pools and arenas work.
- **Thread-safe reads.** Searches never write to tree memory, so any number
  of threads match concurrently without locks. An optional injected
  reader/writer lock makes the whole tree shareable with writers.
- **Allocation-free steady state.** Events and reports are reusable and keep
  their buffers; a warmed-up search makes no allocator calls.
- **Observable.** Per-search work counters, index statistics, a structural
  self-check (`atree_validate`) and Graphviz export.

## Building

```sh
make                 # libatree.a and the shared library (MODE=release for -O2)
make check           # tests + header conformance (C99/C11/C17/C++) + fuzz corpus replay
make check-asan      # same under AddressSanitizer + UBSan
make check-tsan      # thread tests under ThreadSanitizer
make check-valgrind  # same under valgrind (Linux)
make bench           # benchmarks (see below)
make install PREFIX=/usr/local
```

Or with CMake (also for MSVC):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
cmake --install build --prefix /usr/local        # installs atreeConfig.cmake and atree.pc
```

Link with `-latree` (`pkg-config --cflags --libs atree`), or in CMake
`find_package(atree)` and `target_link_libraries(app atree::atree)`.

## Expression language

Compatible with the Rust `a-tree` crate's DSL, with `xor`, `xnor`,
`between`, `true`/`false`, `value in list_attr` and the `&&`/`||`/`!`
spellings added. Keywords are case-insensitive; attribute names are not.

| Form | Attribute types |
|---|---|
| `attr` (bare) | bool |
| `attr < <= > >= n`, `n op attr` | int, float (integer literals promote to float) |
| `attr = <> literal`, `literal op attr` | int, float, string |
| `attr in [..]`, `attr not in (..)` | int, string (list of the same type) |
| `attr one of / none of / all of [..]` | int list, string list |
| `attr is null`, `attr is not null` | bool, int, float, string |
| `attr is empty`, `attr is not empty` | int list, string list |
| `attr between lo and hi` | int, float |
| `literal in list_attr` | shorthand for `list_attr one of [literal]` |
| `true`, `false` | constants |

Connectives, tightest first: `not`/`!`, `and`/`&&`, `xor`/`xnor`, `or`/`||`
(as in C). Strings use single or double quotes with backslash escapes; lists
use `[]` or `()`. Parse errors carry a byte offset and a message:

```c
atree_expr_t *e; atree_error_t err;
if (atree_expr_parse(tree, "country in ['CA', 'US'] and price between 1 and 2.5",
                     SIZE_MAX, &e, &err) != ATREE_OK)
    fprintf(stderr, "offset %zu: %s\n", err.offset, err.message);
```

Expressions can also be built programmatically (`atree_expr_var`,
`atree_expr_cmp_int`, `atree_expr_in_strings`, `atree_expr_and`, ...) and
rendered back to text with `atree_expr_print`.

## Semantics

A predicate on an attribute the event does not define is **undefined**
(paper §3.2); `is null` is the only predicate true in that case. Connectives
follow Kleene three-valued logic (Table 2 of the paper), and an expression
**matches only when it evaluates to true**. So `not (x = 5)` does not match an
event without `x`. `atree_expr_eval` is the reference three-valued evaluator
the tree is tested against.

Details that matter in practice:

- `all of`: every literal is present in the event's list (be-tree semantics).
  `one of`: at least one is. `none of`: none is.
- Strings compare by interned id; an event string the tree has never seen
  equals no literal, so `country <> 'CA'` is true for it.
- Floats compare exactly (`-0.0 = 0.0`); literals must be finite; an event
  NaN is treated as undefined; integer literals promote to float attributes
  when exactly representable.
- Lists are sorted and deduplicated on both sides.
- Inserting the same expression under several ids attaches all ids to one
  node; inserting an id that exists fails with `ATREE_ERR_DUPLICATE_ID`.

## Thread safety

Functions taking `const atree_t *` are read paths and never write to tree
memory: `atree_search*`, `atree_exists`, `atree_event_*`, `atree_report_*`,
`atree_expr_*`, `atree_stats`, `atree_to_graphviz`, `atree_validate`. Any
number of them may run concurrently on one tree. `atree_insert*`,
`atree_delete` and `atree_destroy` need exclusion from everything else, which
can come from:

1. an `atree_lock_t` in the configuration: the library then takes it for the
   duration of every call (`extras/atree_lock_pthread.h` and
   `extras/atree_lock_win32.h` are ready-made adapters);
2. your own synchronization around calls;
3. build-swap-retire: fill a new tree, publish its pointer, let in-flight
   searches drain, destroy the old one.

Use one `atree_event_t` and one `atree_report_t` per matching thread; both
are reusable indefinitely. A custom allocator must be thread-safe if the
library is used from several threads.

## Allocator

```c
typedef struct atree_allocator {
    void *(*alloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)(void *ctx, void *ptr, size_t size);
    void   *ctx;
} atree_allocator_t;
```

`free` and `realloc` receive the exact size previously requested, size 0 is
never requested, and any NULL return surfaces as `ATREE_ERR_NOMEM` with the
tree unchanged (string literals interned before the failure stay interned).
`atree_stats` reports live and peak bytes.

## Performance

Measured with `build/bench/bench_synthetic` (ABE-Gen-style workload from the
paper's Table 3: 1000 dimensions, cardinality 100, depth 3, fan-out 4, 20
attribute-value pairs per event, Zipf α 0.6, 30% subexpression reuse) on an
Apple M-series laptop, release build, single thread, 100 000 expressions,
2000 events. This generator produces a very dense workload (about 2700
matches per event); real workloads match far fewer expressions and are
correspondingly faster.

| Configuration | search p50 | search p99 | nodes visited / event | predicates evaluated / event |
|---|---|---|---|---|
| all optimizations on | 1.15 ms | 2.06 ms | 4 499 | 3 322 |
| no predicate index (`ATREE_FLAG_NO_PREDICATE_INDEX`) | 2.87 ms | 3.95 ms | 4 499 | 233 770 |
| everything off (flags 15) | 3.54 ms | 5.21 ms | 9 632 | 233 770 |
| all on, 5 pairs per event | 0.22 ms | 0.76 ms | 1 185 | 853 |

Index size for the 100 000 expressions: 409 838 nodes, 582 906 edges, about
870 bytes per expression (reorganize and self-adjust removed about 48 000
edges). Inserts run at 48 000 expressions/s with all optimizations on;
`max_adjust_candidates` trades sharing for insert speed (256 → 74 000/s for
under 1% more edges, off → 105 000/s). Deletes exceed 350 000/s.

**Against the Rust `a-tree` crate**, on identical datasets in the dialect
both accept, libatree returns exactly the same matches and searches 26×
faster at 20 000 expressions and 42× faster at 100 000 (0.80 ms vs 33 ms
p50) in a fifth of the memory, because the crate evaluates every predicate
per event while libatree probes per-attribute indexes. **Against the
paper's Table 4** (1.39M real expressions: 1.6 ms, 2.9 s construction,
205 MB) libatree at 1M synthetic expressions matches a far denser workload
in 15.8 ms p50, constructs more slowly (14 000 expressions/s with the
default candidate cap, 47 500/s with `max_adjust_candidates = 256` at 1.1%
more edges) and uses about 665 bytes per expression. Details,
caveats and reproduction commands are in `docs/COMPARISON.md`.

Work counters, not timings, are the regression gates (`tests/test_perf.c`
and `make bench-check` against `bench/baseline.json`): zero suppression
visits no inner node when no leaf is true, propagation on demand never wakes
an AND whose access child is false, adding predicates on attributes an event
does not touch leaves phase-1 work unchanged, and a warmed-up search makes no
allocator calls.

## Observability

- `atree_stats`: nodes, leaves, edges, max level, indexed vs scanned leaves,
  reorganize/self-adjust counters, interned strings, live and peak bytes.
- `atree_report_stats`: per search, predicates evaluated/matched, inner nodes
  visited, AND nodes woken and how many were true, matches.
- `atree_validate`: full structural self-check, for tests and diagnostics.
- `atree_to_graphviz`: DOT export drawn like the paper's figures (leaves at
  the bottom, one rank per level, access-child edges in bold).

## Project layout

- `include/atree.h` — the public API (documented).
- `src/` — implementation; `docs/DESIGN.md` explains it module by module.
- `tests/` — unit, differential, allocation-failure, thread and work-count
  tests; `fuzz/` — libFuzzer harness and corpus; `bench/` — benchmarks.
- `extras/` — header-only lock adapters.
- `PLAN.md` — the design and plan the library was built from; `CLAUDE.md` —
  the coding rules it is held to.

## License

MIT. See `LICENSE`.

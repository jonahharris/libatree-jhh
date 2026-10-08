# libatree

A standalone, portable C99 library implementing the **A-Tree** data structure
from Ji & Jacobsen, *"A-Tree: A Dynamic Data Structure for Efficiently
Indexing Arbitrary Boolean Expressions"* (SIGMOD 2021).

An A-Tree indexes a very large set of arbitrary Boolean expressions over typed
attributes so that one event (an assignment of values to attributes) retrieves
every expression it satisfies without evaluating each one. Typical uses:
advertising exchanges, complex event processing, publish/subscribe filtering,
alert routing. An expression registered under a caller-chosen 64-bit id is a
*subscription*; the shell and server in this repository call it a
*continuous query*.

```c
#include <stdio.h>
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

That snippet is compiled and run by `make check`, so it cannot drift from
the API, but it checks no return values. `examples/` has three complete,
commented programs that do, built by `make examples` and run as tests by
`make check` and `ctest`:

- `examples/basic.c`: every attribute type, text and builder expressions,
  parse errors, events, search, the work counters, three-valued semantics,
  delete, statistics, and the destroy order.
- `examples/threads.c`: one tree shared by a writer and several readers
  through the pthread lock adapter in `extras/`.
- `examples/advanced.c`: callback, exists and allow-list searches, a custom
  allocator, the configuration knobs, `atree_validate` and Graphviz export.

Install with `make install PREFIX=/usr/local` and build against it with
`cc -std=c99 app.c $(pkg-config --cflags --libs atree)`.

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
make check           # tests, header conformance (C99/C11/C17/C++), fuzz corpus replay, shell smoke test
make check-asan      # the tests under AddressSanitizer + UBSan
make check-tsan      # the thread tests under ThreadSanitizer
make check-valgrind  # the test binaries under valgrind (Linux; skipped when valgrind is absent)
make bench           # benchmarks (see below)
make tools           # tools/atree_shell
make examples        # examples/*.c (also run by make check)
make install PREFIX=/usr/local
```

Or with CMake (also for MSVC):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
cmake --install build --prefix /usr/local        # installs atreeConfig.cmake and atree.pc
```

Link with `-latree` (`pkg-config --cflags --libs atree`), or in CMake
`find_package(atree)` and `target_link_libraries(app atree::atree)` for the
shared library (`atree::atree_static` for the static one).

## Trying it out

`make tools` builds `atree_shell`, a line-oriented shell over the library.
Define attributes, register continuous queries and feed events; every event
answers with the matching ids, the time and the search counters:

```
$ build/tools/atree_shell
DEFINE price int
OK price int
DEFINE country string
OK country string
SUBSCRIBE 1 price > 10 and country in ["US", "CA"]
OK subscribed 1
SUBSCRIBE 2 not (price > 10) or country = 'DE'
OK subscribed 2
EVENT price=12;country="US"
MATCH 1: 1
TIME 1.2 us
STATS predicates_evaluated=2 predicates_true=2 nodes_visited=1 and_woken=1 and_true=1 or_visited=0
OK
```

`HELP` lists the commands (`PARSE`, `UNSUBSCRIBE`, `LOAD DEFS|EXPRS|EVENTS`
for the `bench_file` text formats, `STATS`, `VALIDATE`, `DOT`). The same
program is a server, `atree_shell --listen 7777` (loopback only; or a Unix
socket path), serving up to 64 clients from one tree: a client that
subscribed an id receives `NOTIFY id event` whenever another client's event
matches it, and its queries are deleted when it disconnects. `MATCH` lines
print at most 100 ids. `atree_shell --connect localhost 7777` is the
client. The shell is POSIX only; the library itself is not.

## A server

`make -C server` builds `atreed`, a continuous-query server that serves
Redis, Postgres and HTTP clients on one port over a single tree, on
pogocache's networking core. Register queries with `ATREE.SUBSCRIBE`, feed
events with `ATREE.EVENT` (line format or JSON), and receive matches as
Redis pub/sub messages (`SUBSCRIBE atree:7`), streamed Postgres rows
(`WATCH 7`), notifications (`LISTEN atree_7`) or Server-Sent Events
(`GET /subscribe/7`). `server/README.md` has the commands and examples;
`make -C server check` runs its protocol tests.

## Expression language

Compatible with the Rust `a-tree` crate's DSL, with `xor`, `xnor`,
`between`, `true`/`false`, `value in list_attr` and the `&&`/`||`/`!`/`==`/`!=`
spellings added. One difference in meaning: `all of` here is "the event's
list contains every literal" (be-tree semantics); the crate reverses the
inclusion. Keywords are case-insensitive; attribute names are not.

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
(as in C). Lexical rules: an attribute name is `[A-Za-z_][A-Za-z0-9_-]*`
(so `price-1` is a name, not a subtraction) and may not be one of the
keywords `and or not xor xnor in of one none all is null empty true false
between`; `atree_create` rejects such names. Integer literals are 64-bit
signed; a literal with a fraction or exponent (`1.5`, `1e3`) is a float and
must be finite; `12abc` is an error. Strings use single or double quotes
with the escapes `\n`, `\t`, `\r`, `\0`, `\\`, `\'` and `\"` (any other
`\x` is `x`); lists use `[]` or `()` and hold integers or strings, never
floats. Parse errors carry a byte offset and a message:

```c
atree_expr_t *e; atree_error_t err;
if (atree_expr_parse(tree, "country in ['CA', 'US'] and price between 1 and 2.5",
                     SIZE_MAX, &e, &err) != ATREE_OK)
    fprintf(stderr, "offset %zu: %s\n", err.offset, err.message);
```

Expressions can also be built programmatically (`atree_expr_var`,
`atree_expr_cmp_int`, `atree_expr_in_strings`, `atree_expr_and`, ...) and
rendered back to text with `atree_expr_print`. Connectives take ownership of
their children and free everything on failure, so a builder chain needs one
NULL check at the end. A connective that would nest deeper than `max_depth`
yields NULL. `atree_insert_expr` copies what it needs; the caller frees the
expression with `atree_expr_free`.

## Semantics

A predicate on an attribute the event does not define is **undefined**
(paper §3.2); `is null` is the only predicate true in that case, and the
negated forms are undefined too: `x not in [1, 2]`, `x <> 5`, `tags none of
['a']`, `tags is not empty` and `not (x = 5)` all fail to match an event
without `x`. Connectives follow Kleene three-valued logic (Table 2 of the
paper), and an expression **matches only when it evaluates to true**.
`atree_expr_eval` is the reference three-valued evaluator the tree is
tested against.

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

## API overview

Everything is in `include/atree.h`, which documents each function. The
objects are:

- **Tree** (`atree_t`): `atree_create` over a fixed attribute list, with an
  optional `atree_config_t`; `atree_destroy`. Attribute lookup by name or id
  (`atree_attr_lookup`, `atree_attr_name`, `atree_attr_type`).
- **Subscriptions**: `atree_insert` (DSL text) or `atree_insert_expr` (a
  built expression) under an `atree_id_t`; `atree_delete`; `atree_contains`;
  `atree_count`. Several ids may share one expression; an id that exists
  fails with `ATREE_ERR_DUPLICATE_ID`.
- **Events** (`atree_event_t`): created on a tree, reusable; every attribute
  starts undefined; `atree_event_set_*` by name, or `atree_event_set_*_id`
  by attribute id for the hot path; `atree_event_set_undefined` and
  `atree_event_clear`. Strings are resolved by lookup against the tree's
  interned literals; a string the tree has never seen equals no literal.
  Wherever a string length is taken, `SIZE_MAX` (or a NULL array of
  lengths) means NUL-terminated. Events and reports are bound to the tree
  they were created on and must be destroyed before it.
- **Reports** (`atree_report_t`): one per matching thread, reusable;
  `atree_search` fills it and `atree_report_matches` returns the matched ids
  sorted ascending, valid until the next search or destroy;
  `atree_report_stats` gives the work counters. `atree_search_cb` delivers
  ids to a callback (unspecified order; a nonzero return stops early),
  `atree_exists` answers yes/no, and `atree_search_ids` filters by a sorted
  allow list.
- **Expressions** (`atree_expr_t`): `atree_expr_parse`, the builders,
  `atree_expr_print`, `atree_expr_eval` and `atree_expr_free`.
- **Introspection**: `atree_stats`, `atree_validate`, `atree_to_graphviz`.

Operation costs: an insert normalizes the expression, looks every
subexpression up (one hash probe each) and builds only what is new; a
delete is a cascade over the nodes the id used alone, with O(1)
swap-removes; a search costs phase 1, proportional to the leaves the event
satisfies plus the negated leaves on the attributes it defines, and phase 2,
proportional to the nodes reached from true leaves. Nothing in a search is
proportional to the size of the index, and resetting the report costs the
nodes it touched.

## Configuration, errors and limits

`atree_config_t` (zero-initialized by `atree_config_init`; every zero means
the default): `allocator`, `lock`, `flags` (the `ATREE_FLAG_NO_*` switches,
which never change results), `max_depth` (nesting of connectives a parser
or builder accepts, default 64), `initial_nodes` (node capacity to reserve,
default 1024), `max_adjust_candidates` (bound on the reorganize and
self-adjust scans, default 4096; only sharing can suffer) and
`max_expr_nodes` (nodes a normalized expression may have, default 8192; an
XOR chain doubles per level, so this, not the depth, bounds the cost of one
expression).

Every fallible call returns an `atree_status_t` (`ATREE_OK` is 0;
`atree_strerror` names the rest): `NOMEM`, `INVALID_ARG`, `SYNTAX`,
`UNKNOWN_ATTR`, `TYPE_MISMATCH`, `DUPLICATE_ATTR`, `DUPLICATE_ID`,
`NOT_FOUND`, `TOO_DEEP`, `LIMIT`, `INVALID_LITERAL`, `CORRUPT`, `CANCELLED`.
`atree_insert`, `atree_insert_expr` and `atree_expr_parse` also fill an
optional `atree_error_t` with the status, the byte offset and length of the
offending token (or `SIZE_MAX`) and a message of at most
`ATREE_ERROR_MESSAGE_MAX` bytes. On any failure of an insert the tree is
exactly as before; the only residue is in the string table, where literals
interned before the failure stay interned. The library never aborts, exits
or prints.

Limits: ids are any `uint64_t`; node ids are 32-bit, so a tree holds
under 2^32 nodes; a list literal or event list holds under 2^32 elements;
levels are 16-bit. Each limit fails the insert with `ATREE_ERR_LIMIT`.

## Thread safety

Functions taking `const atree_t *` are read paths and never write to tree
memory: `atree_search*`, `atree_exists`, `atree_event_*`, `atree_report_*`,
`atree_expr_*`, `atree_stats`, `atree_to_graphviz`, `atree_validate`. Any
number of them may run concurrently on one tree. `atree_insert*`,
`atree_delete` and `atree_destroy` need exclusion from everything else, which
can come from:

1. an `atree_lock_t` in the configuration: the library then takes it exactly
   once per call, `rdlock` for read paths (including `atree_expr_eval`,
   which resolves string literals through the tree) and `wrlock` for
   `atree_insert*` and `atree_delete`; callbacks run with the lock held and
   must not call back into the tree, and `atree_destroy` is the caller's
   to serialize (`extras/atree_lock_pthread.h` and
   `extras/atree_lock_win32.h` are ready-made adapters);
2. your own synchronization around calls;
3. build-swap-retire: fill a new tree, publish its pointer, wait until every
   reader has moved to it, destroy the old one. Events and reports are bound
   to the tree they were created on (destroying an event reads the tree's
   attribute table), so a reader must release the old tree's event and
   report before it signals that it has moved; the thread test does exactly
   this with a per-reader generation counter.

Use one `atree_event_t` and one `atree_report_t` per matching thread; both
are bound to the tree they were created on and are reusable indefinitely. A
custom allocator must be thread-safe if the library is used from several
threads. `examples/threads.c` is a complete program for option 1.

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

Measured with `build-release/bench/bench_synthetic` (ABE-Gen-style workload
with the paper's Table 3 defaults: 1000 dimensions, cardinality 100, depth
3, four children on average, 20 attribute-value pairs per event, Zipf α
0.6, and the paper's sharing of about 18 uses per predicate and 4.3 per
subexpression) on an Apple M-series laptop, release build, single thread,
100 000 expressions, 2000 events. This generator produces a very dense
workload (about 2800 matches per event); real workloads match far fewer
expressions and are correspondingly faster.

| Configuration | search p50 | search p99 | nodes visited / event | predicates evaluated / event |
|---|---|---|---|---|
| all optimizations on | 0.12 ms | 0.19 ms | 2 803 | 1 623 |
| no propagation on demand (`ATREE_FLAG_NO_PROPAGATION_ON_DEMAND`) | 0.19 ms | 0.32 ms | 5 923 | 1 623 |
| no predicate index (`ATREE_FLAG_NO_PREDICATE_INDEX`) | 0.43 ms | 0.70 ms | 2 803 | 93 418 |
| everything off (flags 15) | 0.53 ms | 0.73 ms | 6 746 | 93 418 |
| all on, 5 pairs per event | 0.03 ms | 0.07 ms | 749 | 420 |

All timings here and in `docs/COMPARISON.md` are from `make MODE=release`
builds (-O2) with nothing else running; the paper's numbers are gcc -O3.

Index size for the 100 000 expressions: 198 377 nodes, 435 616 edges, about
440 bytes per expression (reorganize and self-adjust removed about 109 000
edges). Inserting those 100 000 expressions runs at about 380 000
expressions/s with all optimizations on and 414 000/s with reorganize and
self-adjust disabled, parsing excluded; an insert looks each subexpression
up before building it, so a repeated subexpression costs one probe. Deletes
exceed 1 000 000/s.

**Against the Rust `a-tree` crate and the reference be-tree**, on identical
datasets in the dialect all three accept, the three return exactly the same
matches and libatree searches 245× faster than be-tree and 316× faster than
the crate at 100 000 expressions (0.11 ms vs 26 ms vs 34 ms p50), and 233×
and 396× faster at 1M on the paper's Table 3 workload (1.2 ms vs 283 ms vs
481 ms), in a quarter of be-tree's and a fifth of the crate's memory: the
crate evaluates every predicate per event, and be-tree cannot prune its
partitions when an event defines 20 of 1000 attributes, while libatree
probes per-attribute indexes. On dense events, where be-tree's partitions do
prune, the gap is 15×. **Against the
paper's own synthetic curves** (Figures 11–13 at 1M expressions with the
same parameters and sharing: about 0.65 ms, 5.3 s construction, 300 MB)
libatree constructs in 3.9 s (254 000 expressions/s, parsing timed
separately), uses 374 MB allocated (1.25×; 634 MB peak RSS) and matches in 2.3 ms p50 on
events that match 27 500 expressions each, a density the paper's figure
cannot have had. On the paper's real-workload profile (`bench_synthetic
--ads`: 1.39M expressions, 43 predicates each, every predicate used 69
times) construction takes 11.0 s against the paper's 2.9 s, because the
paper inserts pre-identified predicates with arithmetic identities while
libatree normalizes, hashes and probes every leaf from its literal. `docs/COMPARISON.md`
has the per-component memory breakdown, the insert profile, the caveats and
the reproduction commands.

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
  visited, AND nodes woken and how many were true, OR nodes visited,
  matches.
- `atree_validate`: full structural self-check, for tests and diagnostics.
- `atree_to_graphviz`: DOT export drawn like the paper's figures (leaves at
  the bottom, one rank per level, access-child edges in bold).

## Project layout

- `include/atree.h` — the public API (documented).
- `src/` — implementation; `docs/DESIGN.md` explains it module by module.
- `tests/` — unit, differential, allocation-failure, thread and work-count
  tests; `fuzz/` — libFuzzer harness and corpus; `bench/` — benchmarks.
- `examples/` — complete, commented programs over the public API, run as
  tests.
- `extras/` — header-only lock adapters; `tools/` — `atree_shell`, the
  interactive shell and line-protocol server (POSIX); `server/` — `atreed`,
  the Redis/Postgres/HTTP continuous-query server, with its vendored
  networking core under `server/deps/`.
- `PLAN.md` — the design and plan the library was built from; `CLAUDE.md` —
  the coding rules it is held to.

## Versioning

`ATREE_VERSION_*` and `atree_version()` report the library version; the
project follows Semantic Versioning and `CHANGELOG.md` follows Keep a
Changelog. The current release is 0.1.0 and the API and ABI may still change
before 1.0 (the unreleased changes already add a field to `atree_config_t`).
The shared library's SONAME is the major version.

## License

MIT. See `LICENSE`.

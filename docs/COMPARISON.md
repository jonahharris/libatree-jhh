# libatree vs the paper and the Rust `a-tree` crate

Measured on an Apple M-series laptop, single thread, release builds
(libatree `-O2`, Rust `--release`), October 2026. Numbers vary by machine;
the ratios are the point. Everything here is reproducible:

```sh
make MODE=release bench
# identical datasets in the dialect both implementations accept
build/bench/bench_synthetic --rust-compatible --expressions 100000 --events 2000 --dump /tmp/s100k
build/bench/bench_file /tmp/s100k.defs /tmp/s100k.exprs /tmp/s100k.events
(cd bench/rust_compare && cargo run --release -- /tmp/s100k.defs /tmp/s100k.exprs /tmp/s100k.events)
```

`bench/rust_compare` is optional tooling that builds the reference crate from
`reference/a-tree`; it is not part of the library.

## Same data, both implementations

ABE-Gen-style workload (1000 dimensions, cardinality 100, depth 3, fan-out 4,
20 attribute-value pairs per event, Zipf α 0.6, 30% subexpression reuse),
generated once and fed to both. Both return exactly the same number of
matches on every dataset, which is a cross-implementation check of the
semantics (the dialect avoids `all of`, whose meaning is reversed in the
crate, and `xor`/`xnor`, which it lacks).

| | 20 000 expressions, 500 events | 100 000 expressions, 2 000 events |
|---|---|---|
| total matches (both) | 287 998 | 6 257 519 |
| search p50, Rust crate | 3 171 µs | 33 248 µs |
| search p50, libatree | 120 µs | 798 µs |
| search p99, Rust crate | 4 091 µs | 41 137 µs |
| search p99, libatree | 233 µs | 1 479 µs |
| insert, Rust crate | 60 000 /s | 17 450 /s |
| insert, libatree | 56 400 /s | 35 900 /s |
| peak RSS, Rust crate | 98 MB | 461 MB |
| peak RSS, libatree | 22 MB | 120 MB |
| libatree index | 83 272 nodes, 101 978 edges, 1 044 B/expr | 342 178 nodes, 503 941 edges, 821 B/expr |

The search gap (26× at 20k, 42× at 100k) grows with the number of
expressions because the crate's phase 1 evaluates every distinct predicate
for every event (`process_predicates` iterates `self.predicates`), so its
match time is linear in the size of the index, whereas libatree's phase 1
probes per-attribute indexes and its phase 2 visits only nodes reached from
true leaves. At 100k expressions the crate evaluates about 213 000
predicates per event; libatree evaluates about 2 200.

On the crate's own tiny dataset (`benches/data/search.json`, 9 expressions,
2 events) its Criterion benchmark reports 17.8 µs for both events, so about
8.9 µs per event; `bench_file` on the converted data measures about 1.0 µs
per event.

## Against the paper (Table 4)

The paper's Table 4 uses a proprietary ads workload (1 392 196 expressions,
about 20 attribute-value pairs per event, predicates shared about 68 times
on average) on a 2.2 GHz 2018 CPU; its A-Tree row reports 1.6 ms matching,
2.9 s construction and 205 MB. That data is not available, so the comparison
below uses `bench_synthetic --paper` (1 000 000 expressions from the same
generator as above). The synthetic workload is far denser than an ads
workload (thousands of matches per event instead of a handful) and its
predicates are shared only a few times, so matching does more work per
event and the index has more nodes per expression than the paper's.

| | Paper, A-Tree row of Table 4 (1.39M real ads expressions) | libatree, `--paper` (1M synthetic expressions, 3000 events) |
|---|---|---|
| matching time | 1.6 ms | p50 15.8 ms, p99 29.2 ms, with about 27 000 matches and 44 000 nodes visited per event |
| construction | 2.9 s | 12.4 s (80 600 expressions/s, normalize + build; parsing timed separately) |
| memory | 205 MB (147 B/expression) | 665 B/expression allocated, 828 MB RSS; 3.03M nodes, 5.69M edges |
| machine | 2.2 GHz, 2018 | Apple M-series laptop, 2026 |

What the comparison does and does not say:

- **Matching.** The two workloads are not comparable in density. Ours
  matches 27 000 expressions per event and visits 44 000 nodes; at roughly
  0.36 µs per visited node, matching cost is where the paper's model
  (§5.4: proportional to the matching predicates, subexpressions and
  expressions) says it should be. On the shared datasets above, where the
  density is the same for both implementations, libatree is 26–42× faster
  than the crate; on a sparse event (`--event-size 5`) at 100k expressions
  the p50 is 0.22 ms.
- **Construction.** libatree builds the 1M index in 12.4 s against the
  paper's 2.9 s for 1.39M expressions. Our figure covers normalization,
  the DAG build and journaling (the benchmark parses outside the timed
  region, like the paper, whose expressions are already structured) on a
  workload that shares far less: the paper's predicates are shared 68
  times on average, so most of its inserts are identity hits, while the
  synthetic default shares each predicate about 2.3 times. The dominant cost used to be the reorganize
  candidate scan over popular leaves: the first version ran at 14 200
  expressions/s at 1M and needed `max_adjust_candidates = 256` to reach
  47 500/s at the price of 1.1% more edges. Anchor lists (docs/DESIGN.md,
  "Reorganize") make that scan exact and cheap: the default configuration
  now inserts 80 600/s at 1M and 103 500/s at 100k, within 5% of running
  with reorganize and self-adjust disabled (109 000/s), and the index has
  marginally fewer edges than before (582 571 vs 582 906 at 100k; 5 689 983
  vs 5 714 435 at 1M) because the scan never hits its cap. The cap still
  bounds self-adjust, which scans the full parent list of a new node's
  least popular child; lowering it to 256 changes 100k throughput by about
  3%.
- **Memory.** Per expression libatree uses about 4.5× the paper's figure,
  but the synthetic workload also has about 3 nodes per expression where
  the ads workload, with predicates shared 68 times on average, has far
  fewer. Per node libatree spends roughly 220 bytes: a 64-byte node, two
  small vectors with a minimum capacity of four, the predicate slab, the
  identity table, the per-attribute index and the subscription maps.
  Trimming vector minimum capacities and packing the predicate slab are
  the obvious next steps if memory matters more than simplicity.

## Construction on a paper-like sharing profile

The paper's real workload (§6.2) shares each predicate 68.76 times on
average over 973 794 distinct predicates, which implies about 48 predicates
per expression. `bench_synthetic --ads` reproduces those metrics: 1 392 196
expressions over 122 dimensions, predicates drawn by Zipf rank (exponent
0.8) from a pool of 973 794 distinct predicates, depth 4 with 2–7 children,
half of the subexpressions at each depth reused from a pool, 20
attribute-value pairs per event. Inserts are timed without parsing, as in
the paper, whose expressions are already structured.

| | Paper (§6.2 workload) | libatree `--ads` |
|---|---|---|
| expressions | 1 392 196 | 1 392 196 |
| predicates per expression | 1–56 | 54.4 on average |
| distinct predicates | 973 794 | 915 361 leaves |
| predicate sharing | 68.76× | 82.7× |
| construction | 2.9 s | 137.7 s (10 100 expressions/s) |
| memory | 205 MB | 1.43 GB RSS; 4.49M nodes, 18.7M edges |

So on a profile with the paper's sharing, libatree constructs about 47×
more slowly than the paper's figure, far more than the 4× gap on the
default synthetic workload. The reason is structural, not a constant factor:

- **Alg. 4 returns in O(1) on a hit; our build does not.** The paper
  computes the expression's identity first (`generateID(expr)`) and returns
  the existing node without visiting its subexpressions. libatree's `build()`
  recurses bottom-up: every one of the 75.7M predicate instances is hashed,
  looked up and interned, and every one of the ~29M inner-node instances
  runs reorganize before its identity lookup, although 96% of them already
  exist (4.49M nodes were created). The profile of this run is reorganize
  first, then predicate hashing, then the sort in normalization.
- **Inner-level sharing.** The paper's workload also shares subexpressions
  at every level (2.88× even at level 9); our pool reuse of 50% per depth
  still leaves 3.57M inner nodes for 1.39M expressions, which is also why
  the index is seven times larger than the paper's.

Making `build()` lookup-first (hash the normalized expression from its
content, probe the identity table top-down, and verify a hit by comparing
the stored subtree instead of rebuilding it) would make the cost of an
insert proportional to the nodes it creates rather than to the size of the
expression, as in the paper, while keeping exact identity. That is the
next construction change worth making; it does not affect matching.

The matching numbers of this run are not comparable to the paper's: the
synthetic expressions are or-heavy over 122 dimensions, so a quarter of all
expressions match every event (352 000 matches per event, 273 ms p50),
whereas an ads workload matches a handful.

## Other implementations

A JEPC-based Java engine (internal documentation shared by the user) layers
the same shared DAG with zero suppression and propagation on demand over
type-specialised predicate indexes (an interval index for `a < x and x < b`
shapes, an R-tree for spatial predicates, a linear scan for everything
else). Differences worth noting against libatree: it keeps one tree per
event schema and projects streams onto schemas (an application-level
pattern that works with libatree as one tree per schema); it physically
detaches an AND node from all children but its access child so a true leaf
never iterates parents it cannot wake, whereas libatree keeps every
structural edge and skips non-waking AND parents during the sweep; it
recognizes two-sided range predicates as one indexed interval, whereas
libatree indexes each side as a ray and joins them with an AND; its
per-event truth values live in the nodes, so one tree serves one event at a
time, whereas libatree keeps them in the caller's report so searches run
concurrently; and its identity is a hash contract of the expression library,
whereas libatree compares structure on every hash hit.

## Implementation differences

| | Paper | Rust crate (0.5.1) | libatree |
|---|---|---|---|
| Node identity | commutative id per subexpression, hash table | 64-bit hash only (a collision silently merges expressions) | hash **and** structural comparison |
| Connectives | n-ary | binary (`a and b and c` is `And(And(a,b),c)`, so association changes identity) | n-ary, flattened, canonically ordered |
| Reorganize (Alg. 2) | yes | no | yes, exact via anchor lists |
| Self-adjust (Alg. 3) | yes | no | yes, with relevel and identity re-key |
| Phase 1 | "existing predicate matching algorithms" | evaluates every predicate | per-attribute indexes: bool lists, equality/membership buckets, sorted rays, null lists, scan lists |
| Zero suppression / propagation on demand | yes | yes | yes |
| Access child | random | lowest cost | lowest wake rank, then level, children, id (reproduces Figure 6(c)) |
| Reset between events | event signature, lazy cleaning | fresh bitsets per search (allocates) | dirty-list clearing in a reusable report, no allocation |
| Deletion | use counts | use counts, O(n) list removals | use counts, O(1) swap-removes, iterative cascade |
| Failed insert | n/a | partial state possible | journaled, rolled back exactly |
| Concurrency | n/a | `&self` search (Send + Sync) | const read paths, no tree writes, optional injected rwlock; verified with TSan |
| Allocator | n/a | global | injected, exact sizes |
| Three-valued semantics | yes | yes (`is empty` on undefined panics) | yes |
| `all of` | n/a | event list ⊆ literal list | literal list ⊆ event list (be-tree) |
| Language | `< <= = != >= > ∈ ∉ between and or not xor xnor` | no `xor`/`xnor`/`between` | full paper language plus `is [not] null/empty`, `one of`/`none of`/`all of`, `literal in list_attr` |
| Diagnostics | n/a | parse errors without positions in the public type | status + byte offset + message |

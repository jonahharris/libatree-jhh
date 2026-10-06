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
| construction | 2.9 s | 70 s (14 200 expressions/s with the default candidate cap; see below) |
| memory | 205 MB (147 B/expression) | 665 B/expression allocated, 829 MB RSS; 3.03M nodes, 5.71M edges |
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
- **Construction.** libatree is clearly slower than the paper's reported
  figure. Part of that is scope: our insert includes parsing, normalization
  and journaling. Most of it is the reorganize/self-adjust candidate scan
  on popular leaves, bounded by `max_adjust_candidates`. On the 100k
  workload: all optimizations on 48k/s, cap 256 → 74k/s, cap 64 → 87k/s,
  reorganize and self-adjust off → 105k/s, everything off → 116k/s, while
  the edge count moves from 582 906 (cap 4096) to 586 153 (cap 256) to
  630 920 (off). At 1M expressions the gap widens: the default cap gives
  14 200 expressions/s, cap 256 gives 47 500/s for 1.1% more edges
  (5 777 522 vs 5 714 435) and identical search cost. The default keeps the
  paper's arrival-order independence for predicates with up to 4096
  parents; deployments that insert faster than they share should lower it.
- **Memory.** Per expression libatree uses about 4.5× the paper's figure,
  but the synthetic workload also has about 3 nodes per expression where
  the ads workload, with predicates shared 68 times on average, has far
  fewer. Per node libatree spends roughly 220 bytes: a 64-byte node, two
  small vectors with a minimum capacity of four, the predicate slab, the
  identity table, the per-attribute index and the subscription maps.
  Trimming vector minimum capacities and packing the predicate slab are
  the obvious next steps if memory matters more than simplicity.

## Implementation differences

| | Paper | Rust crate (0.5.1) | libatree |
|---|---|---|---|
| Node identity | commutative id per subexpression, hash table | 64-bit hash only (a collision silently merges expressions) | hash **and** structural comparison |
| Connectives | n-ary | binary (`a and b and c` is `And(And(a,b),c)`, so association changes identity) | n-ary, flattened, canonically ordered |
| Reorganize (Alg. 2) | yes | no | yes, bounded candidate scan |
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

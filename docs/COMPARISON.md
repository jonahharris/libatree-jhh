# libatree vs the paper, the Rust `a-tree` crate and be-tree

## Method

All libatree figures are from `make MODE=release` builds (`-O2 -std=c99`,
Apple clang 16) on an Apple M2 Max laptop (12 cores, 64 GB, macOS 14.5),
single-threaded, with nothing else running. Latencies are per-event
wall-clock times from the scaled mach clock (41 ns resolution); p50 and p99
are percentiles over the events of one run (2000 events at 100k
expressions, 3000 at 1M). Numbers are from single runs, rounded; run-to-run
differences under about 5% are noise and are never reported as changes.
Construction time covers normalize and build with parsing timed separately,
except in the same-data table, where every implementation is timed on the
expression text. "Allocated" is `bytes_allocated` from `atree_stats`, the
live bytes requested from the allocator; "peak RSS" is the maximum resident
set size that `/usr/bin/time -l` reports for the whole benchmark process,
which includes the benchmark's own expression and event data. The paper's
figures are read off its curves at 1M expressions (an error of perhaps
10%); its machine is the one its §6 names (2.2 GHz Intel, gcc 7.4 -O3). The
Rust crate is 0.5.1 built with `--release`. be-tree is the checkout under
`reference/be-tree` (FrankBro/be-tree, the Boolean-expression tree after
Whang et al., VLDB 2009), compiled out of tree with its own flags (`-O3
-std=gnu11`) and driven through its public API by `bench/betree_compare`;
its "heap after insert" is the process heap in use (`mstats`) after the
inserts, before any event is built. Measured in October 2026 on the
unreleased code after 0.1.0 (CHANGELOG.md). Everything is reproducible:

```sh
make MODE=release bench
make -C bench/betree_compare
# identical datasets in the dialect all three implementations accept
build-release/bench/bench_synthetic --betree-compatible --fanout 3 --share 30 --pred-share 0 --expressions 100000 --events 2000 --dump /tmp/b100k
build-release/bench/bench_file /tmp/b100k.defs /tmp/b100k.exprs /tmp/b100k.events
bench/betree_compare/build/betree_compare /tmp/b100k.defs /tmp/b100k.exprs /tmp/b100k.events
(cd bench/rust_compare && cargo run --release -- /tmp/b100k.defs /tmp/b100k.exprs /tmp/b100k.events)
build-release/bench/bench_synthetic --paper --betree-compatible --dump /tmp/p1m    # the 1M column
```

`bench/rust_compare` and `bench/betree_compare` are optional tooling that
build the reference implementations from `reference/`; they are not part of
the library.

## Same data, three implementations

A workload after the paper's generator: 1000 dimensions, cardinality 100,
depth 3, 2–4 children per node, 20 attribute-value pairs per event, Zipf
α 0.6, 30% subexpression reuse and no predicate pool (the flags in the
block above). The third column is the generator's `--paper` preset, the
Table 3 parameters with the paper's sharing (fan-out 4, 54% subexpression
reuse, a predicate pool sized for 18.35 uses per predicate), which is also
the workload of the next section. Each dataset is generated once and fed to
all three implementations, which return exactly the same number of matches
on every dataset, a cross-implementation check of the semantics.

The dialect avoids `all of`, whose meaning is reversed in the crate,
`xor`/`xnor`, which it lacks, and `not`, which be-tree evaluates as the
plain complement of a predicate that is false on an undefined attribute:
`not (d1 = 5)` is true for an event without `d1` in be-tree, and false in
the paper's three-valued semantics and in libatree, which pushes the
negation down to the leaf `d1 != 5`. With the generator's usual 10% `not`
nodes be-tree returned 2 490 284 matches on the 20 000-expression dataset
against 287 998 from the other two; without negation all three agree.
Insert throughput includes parsing for all three.

| | 20 000 expressions, 500 events | 100 000 expressions, 2 000 events | 1 000 000 expressions, 3 000 events (`--paper`) |
|---|---|---|---|
| total matches (all three) | 224 014 | 4 367 137 | 164 589 669 |
| matches per event | 448 | 2 184 | 54 863 |
| search p50, be-tree | 2 565 µs | 26 104 µs | 283 406 µs |
| search p50, Rust crate | 3 519 µs | 33 645 µs | 480 989 µs |
| search p50, libatree | 20.2 µs | 106.5 µs | 1 215 µs |
| search p99, be-tree | 3 395 µs | 29 974 µs | 298 245 µs |
| search p99, Rust crate | 4 091 µs | 37 759 µs | 497 945 µs |
| search p99, libatree | 35.3 µs | 193.0 µs | 2 749 µs |
| insert, be-tree | 11 200 /s | 9 550 /s | 1 910 /s |
| insert, Rust crate | 54 500 /s | 16 500 /s | 1 760 /s |
| insert, libatree | 199 400 /s | 182 300 /s | 104 400 /s |
| heap after insert, be-tree | 66 MB (3 481 B/expr) | 324 MB (3 396 B/expr) | 5 302 MB (5 560 B/expr) |
| allocated, libatree | 20 MB (1 006 B/expr) | 80 MB (796 B/expr) | 357 MB (357 B/expr) |
| peak RSS, be-tree | 79 MB | 386 MB | 5 772 MB |
| peak RSS, Rust crate | 87 MB | 470 MB | 3 862 MB |
| peak RSS, libatree | 22 MB | 91 MB | 429 MB |
| libatree index | 91 631 nodes, 117 188 edges | 371 580 nodes, 579 566 edges | 1 694 710 nodes, 4 599 411 edges |
| subscriptions be-tree evaluates per event | 19 210 of 20 000 | 95 403 of 100 000 | 923 744 of 1 000 000 |

At 100 000 expressions libatree searches 245× faster than be-tree and 316×
faster than the crate; at 1M, 233× and 396×, in 15× less memory than
be-tree. The paper's own A-Tree on its own 1M workload (next section:
about 0.65 ms, 5.3 s, 300 MB) and the 1M column here are the same
generator parameters, but the paper's events are far sparser (54 863
matches per event here). libatree's 1M insert is 104 400/s here because
`bench_file` times the parse as well; `bench_synthetic` on the same data
inserts at 216 700/s with parsing timed separately.

Why the two others are slow is different in each case. The crate's phase 1
evaluates every distinct predicate for every event (`process_predicates`
iterates `self.predicates`), so its match time is linear in the size of the
index: about 213 000 predicates per event at 100k expressions, where
libatree evaluates about 1 600. The be-tree partitions subscriptions by
attribute (p-nodes) and value range (c-directories) and skips a partition
only when the event lacks its attribute and the attribute was declared
mandatory; an event here defines 20 of 1000 attributes, so every attribute
must allow undefined, no partition is skipped, and the search evaluates 92
to 96% of all subscriptions, each one short-circuited through an
undefined-attribute bitmap when that decides it and memoized on shared
subexpressions otherwise. Its own README warns that attributes which allow
undefined cause many useless evaluations; the structure is built for dense
events such as ad requests, where every targeting attribute is present and
the value partitions prune. On such events (20 attributes, all defined in
every event, 100 000 expressions, 26 553 matches per event, same generator
otherwise) it evaluates 65 600 of the 100 000 subscriptions and searches in
19.6 ms p50 (the crate 24.7 ms, libatree 1.28 ms with 38 100 nodes visited),
with identical matches again (53 105 718 in total); it inserts at
51 700/s there. Its insert slows with size because leaf nodes
that overflow are split and their subscriptions moved, with arrays grown
one element at a time: 523 s for 1M expressions, of which 367 s is the tree
insertion after parsing.

On the crate's own tiny dataset (`benches/data/search.json`, 9 expressions,
2 events) its Criterion benchmark reports 17.8 µs for both events, so about
8.9 µs per event; `bench_file` on the converted data measures about 1.0 µs
per event.

## Against the paper (Figures 11–13, Table 4)

The paper reports the A-Tree on its own generator at the Table 3 defaults
(Figures 11(a), 12(a) and 13(a): memory, matching time and construction
time against the number of expressions) and on a proprietary ads workload
(Table 4; see the next section). `bench_synthetic --paper` uses the Table 3
defaults and reproduces the paper's sharing: §6.1 says a predicate is shared
about 18.35 times and a subexpression about 4.33 times in the default
workload, and the generator measures 18.55 and 4.33 at 1M expressions
(predicates come from a pool sized for the target, 54% of the
subexpressions at each depth are reused from a Zipf-ranked pool, and
and/or nodes average four children as in the paper). The paper's figures
below are read off the curves at 1M expressions.

| | Paper, A-Tree at 1M synthetic expressions (Figures 11a, 12a, 13a) | libatree `--paper` (1M expressions, 3000 events) |
|---|---|---|
| construction | about 5.3 s | 3.9 s (254 000 expressions/s; normalize + build, parsing timed separately) |
| memory | about 300 MB | 374 MB allocated (1.87M nodes, 4.20M edges, 200 B per node); 634 MB peak RSS, which includes the benchmark's own predicate pool |
| matching time | about 0.65 ms | p50 2.3 ms, p99 4.0 ms, with 27 500 matches, 23 600 nodes visited and 12 400 true predicates per event |
| machine | 2.2 GHz Intel, 2018, gcc 7.4 -O3 | Apple M-series laptop, 2026, clang -O2 |

What the comparison says:

- **Construction is faster than the paper's curve, but for a different
  reason than it should be.** Figure 9(c) puts reorganize and self-adjust
  at 52% of the paper's construction time; here they are 13% (3.4 s with
  both disabled, 3.9 s with both on). The paper's base path is fast because
  its expressions arrive as structures over predicate ids and its identity
  is arithmetic over child ids (§4.2.1: and → add, or → multiply), so a
  repeated expression, which at this sharing is about half of the inserts,
  costs a few additions and one probe. libatree normalizes every expression
  (copy, sort, dedupe), hashes every leaf from its literal, probes each,
  then verifies structurally. In a no-inline profile of the 1M insert phase
  (taken on the previous workload, same parameters apart from sharing) leaf
  hashing was 11–12% while every leaf was hashed by three functions (the
  structural hash for normalization, the lookup probe, the content hash of
  a new leaf); it is hashed once now, which bought the 9% between 4.3 s
  and 3.9 s. Identity and content table probes and inserts are about 8% (a
  probe reads the slot, the node and the predicate), normalization with
  its sorts about 10%, the waker-region bookkeeping of the parent lists
  about 11%, the allocator about 5%; reorganize and self-adjust together
  are the measured 13%.
- **Measured and declined.** Storing the high 32 bits of the key beside
  each node id in the identity and content tables, so a probe reads a node
  only on a tag match, made inserts 4–5% faster and the index 6% larger
  (436 against 411 bytes per expression, measured before the slabs were
  segmented); memory is the larger gap, so the
  tables keep their 4-byte slots. Verifying a content-table hit by looking
  the node's children up among the operands' ids instead of reading each
  child node changed nothing measurable on either profile (the children
  were just touched by the operand lookups). The waker regions cost about
  11% of insert and buy 11–15% of search; they stay.
- **Memory is 1.25× the paper's at the same sharing.** The index has 856 000
  leaves for 646 000 distinct predicates: the 210 000 extra leaves are the
  negated variants that NOT push-down creates (the paper pushes NOT to the
  leaves too, §5.2.1, so it carries them as well). The 1.01M inner nodes
  match the paper's 4.39M subexpression instances at 4.33× sharing. The
  rest is per-node cost: 200 B per node here, where the paper's 300 MB over
  the same 1.87M nodes would be 160 B; the breakdown below shows where it
  goes. Peak RSS exceeds the allocated bytes by the
  benchmark's own expression and event data and by the identity tables,
  which rehash with both copies resident; the slabs no longer do (they
  were 411 MB allocated and 764 MB peak before they were segmented).
- **Matching is not comparable in density.** Each event here matches
  27 500 expressions (2.75% of the index) and 12 400 predicates are true;
  visiting 23 600 nodes in 0.65 ms on 2018 hardware would be 28 ns per
  node, so the paper's events must be far sparser (it does not describe its
  event generator beyond the pairs per event). Per visited node libatree
  spends about 97 ns. It was 185 ns before three search changes measured
  on this workload: the matched ids are radix-sorted instead of `qsort`ed
  (the sort had been a fifth of the search at 27 500 matches), a woken OR
  node is no longer re-evaluated (under zero suppression only a true child
  wakes it), and a node's subscription list is found through a per-node
  slot array instead of a hash map; together p50 went from 4.6 ms to
  2.3 ms at 1M and from 0.29 ms to 0.12 ms at 100k, with insert throughput
  unchanged. What remains is reading each woken parent (one cache miss per
  edge followed), the AND evaluations and phase 1 over the scan lists. On a
  sparse event (`--event-size 5`) at 100k expressions the p50 is 28 µs.

## Construction on the paper's real-workload profile

The paper's real workload (§6.2) shares each predicate 68.76 times on
average over 973 794 distinct predicates, which implies about 48 predicates
per expression, and Figure 7(a) gives the sharing per level (28×, 11× and
7.5× for the three lowest subexpression levels). `bench_synthetic --ads`
reproduces those metrics: 1 392 196 expressions over 122 dimensions, depth
4 with four children on average, predicates drawn by Zipf rank (exponent
0.8) from a pool sized for 68.76 uses each, subexpressions reused with
probability 87%, 32% and 61% at the three inner depths (which gives the
Figure 7(a) ratios), 20 attribute-value pairs per event. Inserts are timed
without parsing, as in the paper, whose expressions are already structured.

| | Paper (§6.2 workload, Table 4) | libatree `--ads` |
|---|---|---|
| expressions | 1 392 196 | 1 392 196 |
| predicates per expression | 1–56 | 42.8 on average |
| distinct predicates | 973 794 | 865 780 (1 109 393 leaves after NOT push-down) |
| predicate sharing | 68.76× | 68.78× |
| subexpression sharing | 28× / 11× / 7.5× by level | 23.7× overall (the generator does not report it per level) |
| construction | 2.9 s (1.4 s without reorganize and self-adjust) | 11.0 s (127 000 expressions/s); 9.7 s without reorganize and self-adjust |
| memory | 205 MB | 488 MB allocated (2.03M nodes, 4.25M edges); 854 MB peak RSS |
| matching | 1.6 ms | p50 21 ms with 323 000 matches per event |

On the paper's own sharing profile libatree constructs about 3.8× more slowly
than the paper and uses 2.4× its memory. Matching is not comparable: 23% of
the synthetic expressions match every event, which no ads workload does.
What the work on this profile showed:

- **Alg. 4 returns in O(1) on a hit.** The paper computes the expression's
  identity first (`generateID(expr)`) and returns the existing node without
  visiting its subexpressions. The first libatree build recursed bottom-up,
  hashing, looking up and interning every predicate instance and running
  reorganize for every inner-node instance although nearly all of them
  already existed; on the earlier `--ads` preset (54 predicates per
  expression) that was 47× the paper's time. Insert now hashes the normalized expression from its text,
  looks every subexpression up first (leaves in one probe without
  allocation, inner nodes through a content table keyed by flat structure,
  every hit verified) and reorganizes only the nodes that are new
  (docs/DESIGN.md, "Lookup first"). Normalization, which had been copying
  every leaf and literal with one allocation each, builds its copy in one
  arena.
- **What remains is the per-leaf cost of a wide expression.** With 43
  predicates per expression the insert spends most of its 7.9 µs on the
  leaves: in the insert profile of the earlier `--ads` preset (54
  predicates per expression) normalization was 23%, the leaf hashes and
  probes 17% (each probe is three dependent cache misses: slot, node,
  predicate) and sorting 15%; reorganize and self-adjust are the measured
  12% here. The paper's 1 µs per expression over 48 predicates is only
  possible with integer predicate ids and arithmetic identities.
- **Memory** is dominated by the 1.11M leaves (the paper's 974k predicates
  plus negated variants) and the per-node cost discussed above.

## Where the memory goes

A white-box breakdown of the 100k-expression tree of the README's
Performance section (generator defaults, the paper's Table 3 parameters and
sharing: 198 377 nodes, 435 616 edges, 44.1 MB allocated through the
allocator, 222 B per node, 440 B per expression), measured by walking the
internal structures:

| Component | Bytes | Share | B/node |
|---|---|---|---|
| node slab (64 B × segment capacity) | 13.6 MB | 30.9% | 68.7 |
| phase-1 index structures (per-attribute maps, rays, lists) | 3.7 MB | 8.4% | 18.7 |
| children arrays (ids + positions) | 3.5 MB | 7.9% | 17.6 |
| parent arrays (of which 1.7 MB unused capacity) | 3.4 MB | 7.8% | 17.3 |
| subscription map (id → node) | 3.4 MB | 7.7% | 17.2 |
| predicate slab (32 B × segment capacity) | 3.1 MB | 7.1% | 15.9 |
| identity table | 2.1 MB | 4.8% | 10.6 |
| phase-1 buckets (one small vector per equality/membership key) | 2.1 MB | 4.7% | 10.5 |
| marks, worklists, free lists | 1.9 MB | 4.4% | 9.7 |
| subscription list headers + `always` | 1.7 MB | 3.9% | 8.7 |
| flat content sums (`csum`) | 1.7 MB | 3.9% | 8.6 |
| content table | 1.0 MB | 2.4% | 5.3 |
| leaf positions + leaf list | 0.9 MB | 2.1% | 4.6 |
| subscription slots (per node) | 0.9 MB | 1.9% | 4.3 |
| predicate list operands | 0.7 MB | 1.7% | 3.7 |
| string table, attributes | 0.2 MB | 0.3% | 0.8 |

Two structural costs were measured here and have since been removed. The
slabs indexed by node or predicate id (nodes, predicates, `csum`, leaf
positions, subscription slots, marks) used to double, carrying up to half
their size in slack and holding both copies while they grew; they are
segmented now (`src/slab.h`: fixed segments of 2^14 elements, at most one
of them empty, nothing ever copied). At this fill the node slab costs
68.7 B per 64-byte node instead of 84.6; the tree is 12.7% smaller
(50.4 → 44.1 MB), the 1M-expression `--paper` tree 9% smaller (411 →
374 MB) and its peak RSS 17% lower (764 → 634 MB) because the 134 → 268 MB
copy is gone. A subscription used to cost a map entry, a 16-byte list
header and a four-slot heap list that almost always held one id; the
first id now lives in the header and a heap list exists only for nodes
with several ids, which cut subscriptions from 64 to 51 B each here (map
plus headers). What remains is allocation policy: small vectors have a
minimum capacity of four, which is why parent arrays carry 1.7 MB of
unused capacity for an average of 2.2 parents and a phase-1 bucket for a
single predicate costs 32 B. Two cheap policy changes were tried and
rejected: a minimum vector capacity of one instead of four cut live bytes
by 6% at 1M expressions but cost 4–5% insert throughput and raised peak
RSS at 100k, and growing large arrays by half instead of doubling cut
live bytes by 19% at 1M but raised peak RSS by 8% through realloc churn.

## Other implementations

A Java engine built on JEPC (a Java event-processing framework; its
documentation is not part of this repository) layers
the same shared DAG with zero suppression and propagation on demand over
type-specialised predicate indexes (an interval index for `a < x and x < b`
shapes, an R-tree for spatial predicates, a linear scan for everything
else). Differences worth noting against libatree: it keeps one tree per
event schema and projects streams onto schemas (an application-level
pattern that works with libatree as one tree per schema); it physically
detaches an AND node from all children but its access child so a true leaf
never iterates parents it cannot wake, whereas libatree keeps every
structural edge and partitions each parent list into waker and non-waker
regions, so the sweep reads only the parents it can wake; it
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
| Three-valued semantics | yes | yes (`is empty` on undefined panics) | yes; an event NaN counts as undefined |
| `all of` | n/a | event list ⊆ literal list | literal list ⊆ event list (be-tree) |
| Language | `< <= = != >= > ∈ ∉ between and or not xor xnor` | no `xor`/`xnor`/`between` | full paper language plus `is [not] null/empty`, `one of`/`none of`/`all of`, `literal in list_attr` |
| Diagnostics | n/a | parse errors without positions in the public type | status + byte offset + message |

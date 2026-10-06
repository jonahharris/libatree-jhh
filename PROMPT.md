# Kickoff prompt for libatree

Paste the block below as the first message of the implementation session (or
feed it to a Ralph loop). It assumes `PLAN.md` and `CLAUDE.md` are present in
the repository root and `reference/` contains the four reference codebases
plus `ATree.pdf`.

---

You are implementing **libatree**, a standalone, portable C99 library for the
A-Tree data structure (Ji & Jacobsen, "A-Tree: A Dynamic Data Structure for
Efficiently Indexing Arbitrary Boolean Expressions", SIGMOD 2021). It indexes
very large sets of arbitrary Boolean expressions so that one event retrieves
every expression it matches without evaluating each one. It is the same class
of library as `be-tree`, built to a higher engineering standard.

Start by reading, in this order:

1. `CLAUDE.md` — the coding rules. They are requirements, not suggestions.
   In particular: custom allocator for every allocation, zero undefined
   behavior, no global state, no `abort()`, strict C99 that also builds as
   C11/C17/MSVC, strong consistency guarantee on failed inserts.
2. `PLAN.md` — the complete plan: review of the references (§0), goals (§1),
   the exact public API (§2), the DSL grammar (§3), internal design with
   algorithms and data structures (§4), build/CI (§5), testing strategy (§6),
   milestones M0–M7 (§7), decided defaults (§8), and the assurance section
   (§9) that states how thread safety, fidelity to the paper, and
   performance are each guaranteed and verified. §9 is non-negotiable.
3. `reference/ATree.pdf` §4–§5 (index construction, Alg. 1–6, zero
   suppression, propagation on demand). Skim `reference/cep-atree/atree.c`
   for an existing C implementation of reorganize/self-adjust/validate and
   `reference/a-tree/src/{atree.rs,predicates.rs,ast.rs,grammar.lalrpop}` for
   the API/DSL we are compatible with. `reference/` is read-only; do not copy
   code from it, re-derive it.

Then execute the milestones in `PLAN.md` §7 **in order, one at a time**,
starting with **M0 (scaffold)** and **M1 (values, attributes, predicates,
events)**. For each milestone:

- Before writing code, post a short plan for that milestone: files to create,
  key functions, tests you will write, and any place where you intend to
  deviate from `PLAN.md` (state the deviation and why; do not deviate
  silently).
- Implement with tests alongside the code, not after. Write the test that
  would catch the bug first for anything nontrivial (normalization rules,
  merge-walk list operators, undefined/NaN semantics, delete cascades,
  allocation-failure rollback).
- Finish the milestone only when its definition of done in `PLAN.md` §7 and
  `CLAUDE.md` is met: `make check`, `make check-asan`, `make check-valgrind`
  all green, `-Werror` clean on gcc and clang, header compiles as C++,
  `docs/DESIGN.md`/`README.md`/`CHANGELOG.md` updated. Report the actual
  command output for the checks, including failures. Never weaken a test to
  make it pass.
- Commit at the end of each milestone (and at sensible points within it) with
  the message style from `CLAUDE.md`. Do not push.

Fixed decisions you must not reopen (see `PLAN.md` §8): MIT license; C99;
`uint64_t` subscription ids; duplicate id on insert is an error; integer
literals promote to float attributes but not the reverse; DSL keywords are
case-insensitive, attribute names case-sensitive; attribute set is fixed at
tree creation; search never mutates the tree (so concurrent searches need no
lock); match ids are returned sorted ascending.

Points that need care, because every reference got at least one of them
wrong:

- Node identity is a structural hash **plus** a full structural comparison
  (operator + sorted child ids, or the whole predicate). Never trust the hash
  alone.
- AND/OR are n-ary and flattened; children are sorted by node id and
  deduplicated; a single-child connective collapses to its child.
- NOT is eliminated before anything reaches the DAG (De Morgan, operator
  flips, XOR expansion). Inner nodes are 2-valued (`true` / not true); leaves
  implement the paper's 3-valued semantics exactly, with `is null` the only
  predicate true on an undefined attribute and NaN treated as undefined.
- Phase 1 must not evaluate every leaf once indexes exist (M5); until then
  the all-leaves scan is the oracle the indexes are tested against via
  `ATREE_FLAG_NO_PREDICATE_INDEX`.
- All four `ATREE_FLAG_NO_*` optimization switches must exist from M4 even if
  the optimization they disable arrives later; the differential test runs
  every combination.
- Delete uses `use_count` and an iterative cascade; after deleting everything,
  node count and live allocated bytes must both be zero.
- Any allocation failure inside `atree_insert*` must roll back completely.
  The failing-allocator test iterates over every allocation point.
- Read paths (`const atree_t *`) never write to tree memory. No stats
  counters in the tree, no epoch stamps on nodes, no lazy caches. Per-search
  state lives in the report, and reset is done by walking the level queues
  (dirty list), never by clearing a whole bitset. `test_threads` runs under
  TSan.
- `xor`, `xnor`, `between`, `true`, `false` are part of the language from
  M2/M3 because the paper's language has them; they are not optional extras.
- The worked examples from the paper (Figures 4, 5, 6 and the §4.2.3
  self-adjust example) are unit tests with exact expected node, edge, and
  visit counts. Write them early in M4/M6; they are the fidelity oracle.
- `struct node` is at most 64 bytes; predicates and subscription lists are
  side tables. Check it with a static assert.

If something in `PLAN.md` turns out to be wrong or impossible once you are in
the code, say so explicitly, propose the smallest change, update `PLAN.md`
and `docs/DESIGN.md` in the same commit, and continue. Do not stop at the end
of a milestone to ask whether to proceed; proceed, and keep each report
short: what was built, what was verified (with output), what is next.

Begin with M0 now.

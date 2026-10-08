# libatree examples

Small, self-contained programs that together cover the whole public API
(`include/atree.h`). Each one checks its own results and exits nonzero if
anything differs, so `make check` and `ctest` run them as tests. Read them in
this order:

| File | What it shows |
|---|---|
| `basic.c` | The walkthrough: attributes of every type, a tree with default configuration, subscriptions from DSL text (comparison, `in`, `one of`/`all of`/`none of`, `is null`, `between`, `not`, nested `and`/`or`, `xor`) and from the builder API, `atree_expr_print`, parse errors with byte offsets, `ATREE_ERR_DUPLICATE_ID`, a reusable event set by name and by attribute id, `atree_search` and the sorted matches, `atree_report_stats`, three-valued semantics checked against `atree_expr_eval`, `atree_delete`, `atree_count`/`atree_contains`, `atree_stats`, and teardown order. |
| `threads.c` | One tree shared between a writer and several readers through the pthread lock adapter in `extras/atree_lock_pthread.h`; one event and one report per reader thread; every result checked against fixed invariants and `atree_expr_eval`. POSIX only (not built on Windows). |
| `advanced.c` | `atree_search_cb` with early stop, `atree_exists`, `atree_search_ids` with an allow list, a counting `atree_allocator_t` that verifies the exact-size contract and sees 0 live bytes after `atree_destroy`, `atree_config_t` fields (`max_depth`, `initial_nodes`, `flags`), `ATREE_FLAG_NO_PREDICATE_INDEX` changing the work counters but not the matches, `atree_validate`, and `atree_to_graphviz` (`--dot` prints the DAG). |

## Building and running

```sh
make examples                 # builds build/examples/{basic,threads,advanced}
build/examples/basic
build/examples/advanced --dot | dot -Tsvg > tree.svg
make check-examples           # runs them all (also part of make check)
```

With CMake they are built by default (`-DATREE_BUILD_EXAMPLES=OFF` to skip)
and registered with `ctest` as `example_basic`, `example_threads` and
`example_advanced`.

## Using an installed library

After `make install` (or `cmake --install`), a program needs only the header
and the library:

```sh
cc -std=c99 app.c $(pkg-config --cflags --libs atree)
cc -std=c99 -D_POSIX_C_SOURCE=200809L -pthread threads.c $(pkg-config --cflags --libs atree)
```

`threads.c` includes `../extras/atree_lock_pthread.h` by relative path;
copy that header next to your program (it is not installed) and adjust the
include.

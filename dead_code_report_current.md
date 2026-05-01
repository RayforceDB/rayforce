# Dead-code report — current status

Status: 2026-05-01, after cleanup on the current working tree.

This supersedes the earlier `dead_code_report.md` from the Telegram temp
directory. The original report was useful, but several findings are now either
fixed, removed, or reclassified as intentional engine surface.

## Resolved

The following zero-caller or orphaned paths from the old report have been
removed:

| Old finding | Resolution |
| --- | --- |
| `ray_poll_rx_extend` / `ray_poll_send` | Removed from `src/core/poll.c` and `src/core/poll.h`. IPC continues to use its direct send path. |
| `ray_part_load` | Removed from `src/store/part.c` and `src/store/part.h`. The supported path is `ray_read_parted`. |
| Embedding DAG ops: `exec_cosine_sim`, `exec_euclidean_dist`, `exec_knn`, `exec_hnsw_knn` | Removed with their graph builders, exec dispatch, stale opcodes, dump labels, and unused extension-union fields. Live Rayfall embedding builtins remain. |
| `OP_TIL` DAG path | Removed the graph builder, exec dispatch, and optimizer fold. The live Rayfall `(til ...)` builtin remains. |
| `OP_PROD` grouped aggregation | Fixed accumulation, merge, and emit paths. Added rfl regression coverage for scalar, DA, and HT paths. |

Verification:

```sh
rg -n 'ray_part_load|ray_poll_send|ray_poll_rx_extend|exec_cosine_sim|exec_euclidean_dist|exec_knn\(|exec_hnsw_knn|ray_cosine_sim|ray_euclidean_dist|ray_knn\(|ray_hnsw_knn|OP_TIL|OP_COSINE_SIM|OP_EUCLIDEAN_DIST|OP_HNSW_KNN|OP_KNN\b' src test
```

No matches remain for the removed symbols.

```sh
make test
```

Result: `991 of 992 passed (1 skipped, 0 failed)`.

Current diff shape:

```text
15 files changed, 98 insertions(+), 727 deletions(-)
```

## Reclassified As Live / Intentional

These old report items should not be deleted.

| Area | Current status |
| --- | --- |
| `OP_WINDOW` / `ray_window_op` | Live C-level operator with tests in `test/test_audit.c` and `test/test_exec.c`. Still not broadly exposed as a Rayfall verb, but not dead. |
| Graph algorithms (`OP_PAGERANK`, `OP_LOUVAIN`, `OP_DIJKSTRA`, `OP_DFS`, `OP_TOPSORT`, centrality ops, MST, random walk, A*, k-shortest, expand, var-expand, shortest path, WCO join) | Live engine/C-API surface with coverage in `test/test_csr.c` and related tests. Public Rayfall exposure is a product/API decision, not dead-code cleanup. |
| MAPCOMMON / parted execution paths | Live storage/execution surface. Covered by `test/test_store.c`, `test/test_opt.c`, `test/test_link.c`, and rfl system tests. |
| Factorized expand/group paths | Live C-level graph pipeline with tests in `test/test_csr.c`. |
| REPL-only syscmd handlers | Live in interactive mode. Coverage should come from REPL fixtures rather than deletion. |

## Remaining Cleanup Candidates

No old high-confidence zero-caller removal candidate remains from the report.

The next useful cleanup pass should target one of these:

1. Add or improve REPL fixture coverage for `src/lang/syscmd.c`.
2. Decide which C-level graph algorithms should get Rayfall verbs before first users.
3. Decide whether `OP_WINDOW` should become a Rayfall-facing feature now or stay C-level.
4. Continue a broader zero-caller audit outside the original report scope, but treat C-API/test-only graph operators as intentional unless there is a design decision to remove them.


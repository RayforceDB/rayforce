# q17 take-pushdown + v2 bounded emit

Commit: `f1aaa736` perf(query): push a positive take: into grouped DAGs and bound v2's emit (single commit — the v2 signature change makes a two-commit split non-compiling).

## What changed

1. `src/ops/query.c` — a `take:` that is a POSITIVE integer atom is now added to the grouped DAG as `ray_head(g, root, n)` (exactly as the ungrouped branch does), gated on: `by_expr` present, no `asc:`/`desc:`, no `nearest:`, no deferred post-group WHERE (`post_group_where_expr`), no BOOL group key. exec.c's HEAD(GROUP) fusion forwards N to `exec_group` as `group_limit`. Negative (tail) and range takes are not pushed. Helper `group_keys_have_bool` added.
2. `src/ops/group.c` / `agg_engine.h` — the two v2 gates in `exec_group_run` (direct, and `exec_group_v2_exprs`) relaxed from `group_limit == 0` to `>= 0`; `group_limit` threaded through `exec_group_v2` → `exec_group_v2_run` → `exec_group_v2_parallel_radix`. The emit-filter v2 escape hatch still passes 0.
3. `src/ops/agg_engine.c` — new noinline `agg_radix_select_first_n`: when `group_limit > 0 && ng > group_limit`, an N-sized max-heap over every partition's `first_row` picks the N smallest (= the first N first-seen groups), heap-sorted ascending into the same `(part<<32)|gid` order array. Downstream key-unpack / column alloc / finalize iterate `n_emit` instead of `ng`. With no limit (or `ng <= group_limit`) the original full order-map path runs unchanged.

## Two bugs found and fixed during verification (both would have been silent wrong answers)

- **deferred post-group WHERE**: `can_defer_single_key_where` moves a key WHERE to run AFTER the group; taking the first N groups before it under-fills the result. Excluded.
- **BOOL group key**: the group emits false-before-true and query.c reorders the finished table to first-occurrence afterwards. Reproduced at 3M rows: `by b take: 1` returned `false` where the correct answer is `true`. Excluded via `group_keys_have_bool`.

## Verification

- `make release -j8` warning-clean (`-Werror`). `make test`: `=== 3688 of 3688 passed (0 skipped, 0 failed) ===`.
- gdb (10M splayed store): q17 reaches `exec_group_run(..., group_limit=10)` → `exec_group_v2(group_limit=10)` → `exec_group_v2_parallel_radix(..., group_limit=10)`. q16 (`desc: c take: 10`) still reaches `exec_group` with `group_limit=0` — routing unchanged.
- Byte-identity vs the pre-change binary (`/tmp/rf_base_q17`), store `/skull/clickbench/rfsplayed`, `-c 4`: q13 q15 q16 q17 q18 MATCH. q32 DIFFERs, but is nondeterministic in BOTH binaries run-to-run (all `c == 1` ties under `desc`); base produced ≥3 distinct hashes across 12 runs, and base and new produced overlapping hash sets. Full 43-query sweep: q21/q31 likewise nondeterministic in base; q40 is stable in base under idle load but flips one tied row under CPU load in base too, so also pre-existing.
- Perf, 10M, `-c 4 -t 1`, min of 5 in one process: **q17 196.4 ms → 95.5 ms (2.05x)**. Full-sweep ratios all within noise; the only repeatable deltas are q15/q18/q35 at +3–6%, which is code-alignment noise: pristine HEAD with an unrelated dummy noinline function added to agg_engine.c reproduces q15 at 158.5 ms vs 150.2 ms (same magnitude). q12/q33/q34 came out 3–5% FASTER than base after the noinline refactor (inlining the selection code had cost them ~9%).
- Regression test appended to `test/rfl/query/highcard_group.rfl`: 300K rows, 2 keys (100K distinct pairs, 3 rows each), `take: 5` asserts keys `[0 1 2 3 4]`, second key `[0 1 2 3 4]`, counts `[3 3 3 3 3]`, plus non-pushed `take: -3` and `take: [2 3]` shapes.

## Concerns

- **Parted stores**: `exec_group_parted` truncates its partition loop when `group_limit > 0 && n_part_keys == 0 && !saved_sel` (all keys MAPCOMMON ⇒ 1 group per partition). Grouped take-without-sort queries can now reach that code for the first time. It is wrong only if a partition is empty (yielding <N groups); pre-existing logic, not exercised by the ClickBench splayed store.
- The bounded emit is implemented only in the radix strategy. Dense and smallhash ignore the hint (full emit, then HEAD trims), so no correctness dependency — but also no speedup for those shapes.
- Group emit order for low-card keys is key-slot order, not first-seen, in BOTH binaries — the pushdown does not change that because the dense path ignores the hint.

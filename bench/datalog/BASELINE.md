# Chain transitive-closure baseline

- Date: 2026-09-11
- Commit: 04bbb81193a3bcc8d34b73a5d68042a3030063a1 (branch perf/datalog-fixpoint, cut from fix/datalog-audit)
- Machine: 8 logical CPUs (`nproc`), Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz
- RAYFORCE_CORES=2 (fixed for all runs)
- Methodology: min-of-3 wall time per N, via `date +%s%N` around each process invocation (includes process start-up). Load average at run time: 0.02 0.37 0.55 (idle machine, nothing else heavy running).
- `./rayforce` is a release build at the commit above (built prior to this task; not rebuilt here since no sources changed).

Expected/known limitation before Task 11: the current evaluator has an iteration cap, so some chain sizes report `evaluation failed` instead of the correct row count. This is recorded honestly below as `WRONG(...)`, not worked around.

## linear mode: `(rule (tc ?x ?z) (edge ?x ?y) (tc ?y ?z))`

```
linear N=   64 rows=    2080 min_ms=     36 ok
linear N=  128 rows=    8256 min_ms=    105 ok
linear N=  256 rows=   32896 min_ms=    490 ok
linear N=  512 rows=  131328 min_ms=   1740 ok
linear N= 1024 rows=  524800 min_ms=  10513 WRONG(error: domain: query: evaluation failed)
```

## nonlinear mode: `(rule (tc ?x ?z) (tc ?x ?y) (tc ?y ?z))`

```
nonlinear N=   64 rows=    2080 min_ms=     11 ok
nonlinear N=  128 rows=    8256 min_ms=     30 ok
nonlinear N=  256 rows=   32896 min_ms=    131 ok
nonlinear N=  512 rows=  131328 min_ms=    889 ok
nonlinear N= 1024 rows=  524800 min_ms=   8310 ok
```

## Notes

- N=8 sanity check (by hand, before the sweep): both `linear` and `nonlinear` generated programs for N=8 print `36` (= 8*9/2), matching the expected chain transitive-closure row count. `(+ 1 (til 8))` / `(+ 2 (til 8))` yield I64 vectors as required for the env-bound EDB table.
- linear N=1024 hits the evaluator's iteration cap and fails with `error: domain: query: evaluation failed`; nonlinear N=1024 does not fail at this N (doubling-style closure needs fewer fixpoint iterations than the linear left-recursive rule for the same chain length). This asymmetry is expected to close, or at least be characterized precisely, once Task 11 addresses the fixpoint iteration cap.
- Runtime dominates with N: for linear mode the min_ms grows roughly quadratically-ish across doublings of N up to 512, consistent with materializing an O(N^2) `tc` relation each time; process start-up overhead is a fixed few ms and is not separated out here (documented as acceptable per the task brief).
- Bugfix applied to the runner relative to the brief's literal script: the brief's `printf` call left `$ok` unquoted, which word-splits the `WRONG(...)` string (it contains spaces/colons) and corrupts the row's formatting via printf's format-string-reuse behavior when it sees extra arguments. Quoted `"$ok"` in `bench/datalog/tc_chain.sh` to fix this; no other behavioral change.

## After Task 11 (17e4f5f1)

- Date: 2026-09-11
- Commit: 17e4f5f18f501089442b20c62ce9f9d53fa4b84f (branch perf/datalog-fixpoint) plus the uncommitted Task 11 working-tree changes this section's own commit introduces (same convention as the base entry above, which cites the commit the bench code lived at rather than a not-yet-created commit).
- Change under test: `dl_eval`'s fixpoint loop now runs uncapped for strata whose rules contain only relational literals (no `DL_ASSIGN` / `DL_BUILTIN` / `DL_INTERVAL`); the `DL_MAX_ITER_NONMONOTONE` (1000) cap now applies only to strata that can manufacture new values. The linear `tc` rule's stratum is pure-relational, so it is no longer capped.
- Same methodology and machine as above (min-of-3 wall time via `date +%s%N`, `RAYFORCE_CORES=2`, `./rayforce` freshly built with `make -j8 release` at this commit).

```
linear N=   64 rows=    2080 min_ms=     37 ok
linear N=  128 rows=    8256 min_ms=    105 ok
linear N=  256 rows=   32896 min_ms=    499 ok
linear N=  512 rows=  131328 min_ms=   1733 ok
linear N= 1024 rows=  524800 min_ms=  10853 ok
```

N=1024 linear, which previously reported `WRONG(error: domain: query: evaluation failed)`, now reports `ok` with the correct 524800 rows. Timings at N<=512 are within noise of the pre-Task-11 baseline (same monotone-stratum code path, just no longer capped), confirming the uncapped loop adds no measurable overhead for chains that already converged well under 1000 iterations.

## After Task 12 (pre-commit HEAD 9707dd00 + the Task 12 working-tree changes)

- Date: 2026-09-11
- Commit: 9707dd00 (branch `perf/datalog-fixpoint`) plus the uncommitted Task 12 working-tree changes this section's own commit introduces (same convention as the sections above).
- Change under test: each IDB keeps one open-addressing row set (`dl_rowset_t`) alive for the whole stratum. A fixpoint iteration probes only the candidate tuples and inserts the accepted ones, instead of `table_distinct(new)` + `table_antijoin(new, rel->table)` re-hashing the entire derived relation every iteration. A candidate table *larger* than the relation is still collapsed by the vectorised `table_distinct` first (the non-linear rule shape manufactures far more duplicates than the relation holds, and the vectorised hash beats the scalar probe there); the budget is the relation's own row count, no new tunable.
- Same methodology and machine as above (min-of-3 wall time via `date +%s%N`, `RAYFORCE_CORES=2`, `./rayforce` freshly built with `make -j8 release`). Load average at run time: 1.19 0.91 0.70.

```
linear N=   64 rows=    2080 min_ms=     14 ok
linear N=  128 rows=    8256 min_ms=     24 ok
linear N=  256 rows=   32896 min_ms=     53 ok
linear N=  512 rows=  131328 min_ms=    161 ok
linear N= 1024 rows=  524800 min_ms=   1324 ok
```

```
nonlinear N=   64 rows=    2080 min_ms=     10 ok
nonlinear N=  128 rows=    8256 min_ms=     28 ok
nonlinear N=  256 rows=   32896 min_ms=    126 ok
nonlinear N=  512 rows=  131328 min_ms=    879 ok
nonlinear N= 1024 rows=  524800 min_ms=   8295 ok
```

Every N reports `ok` with the expected row count.

Linear (the shape this task targets — a small delta joined against a growing relation):

| N | After Task 11 | After Task 12 | speedup |
|---|---|---|---|
| 64 | 37 ms | 14 ms | 2.6x |
| 128 | 105 ms | 24 ms | 4.4x |
| 256 | 499 ms | 53 ms | 9.4x |
| 512 | 1733 ms | 161 ms | **10.8x** |
| 1024 | 10853 ms | 1324 ms | **8.2x** |

Non-linear is unchanged within noise (889 -> 879 ms at N=512, 8310 -> 8295 ms at N=1024): there the candidate table dwarfs the relation, so the pre-dedup guard keeps the vectorised `table_distinct` in front of the row-set probe and only the per-iteration `table_antijoin` is removed — a cost that is small next to the join and the distinct at that shape.

Measured without the pre-dedup guard, non-linear regressed sharply (N=512 1810 ms, N=1024 22819 ms) because the scalar per-row probe and the per-row `dl_table_take_mask` copy ran over a candidate table many times the size of the relation. That measurement is why the guard exists.

## After Task 13 (pre-commit HEAD 32e65461 + the Task 13 working-tree changes)

- Date: 2026-09-11
- Commit: 32e65461 (branch `perf/datalog-fixpoint`) plus the uncommitted Task 13 working-tree changes this section's own commit introduces (same convention as the sections above).
- Change under test: `dl_eval` no longer keeps the dead `prev_tables[]` (set and released every iteration, never read), which pinned every IDB column at refcount > 1; and `table_union` appends `b`'s rows into `a`'s columns in place (`ray_vec_append_raw`) when `a`, its column list and every column are uniquely owned, non-slice, non-arena, same-typed and neither SYM nor STR. The per-iteration merge is then amortised O(delta) instead of O(relation).
- Fast-path hit rate on this benchmark (measured with a temporary counter, removed before commit): linear N=512 1021 hits / 0 misses, nonlinear N=512 26 hits / 0 misses — 100%.
- Same methodology and machine as above (min-of-3 wall time via `date +%s%N`, `RAYFORCE_CORES=2`, `./rayforce` freshly built with `make -j8 release`). Load average at run time: 1.23 1.07 0.81.

```
linear N=   64 rows=    2080 min_ms=     14 ok
linear N=  128 rows=    8256 min_ms=     24 ok
linear N=  256 rows=   32896 min_ms=     49 ok
linear N=  512 rows=  131328 min_ms=    112 ok
linear N= 1024 rows=  524800 min_ms=    340 ok
```

```
nonlinear N=   64 rows=    2080 min_ms=     10 ok
nonlinear N=  128 rows=    8256 min_ms=     27 ok
nonlinear N=  256 rows=   32896 min_ms=    142 ok
nonlinear N=  512 rows=  131328 min_ms=    905 ok
nonlinear N= 1024 rows=  524800 min_ms=   8304 ok
```

Every N reports `ok` with the expected row count.

| N | After Task 12 (linear) | After Task 13 (linear) | speedup |
|---|---|---|---|
| 64 | 14 ms | 14 ms | 1.0x |
| 128 | 24 ms | 24 ms | 1.0x |
| 256 | 53 ms | 49 ms | 1.1x |
| 512 | 161 ms | 112 ms | 1.4x |
| 1024 | 1324 ms | 340 ms | **3.9x** |

The win grows with N because the copy this removes was proportional to the relation size times the iteration count. Non-linear is unchanged within noise (879 -> 905 ms at N=512, 8295 -> 8304 ms at N=1024): that shape converges in ~26 iterations, so only 26 unions happen and the copy was never the bottleneck there; its cost is the join and the distinct over a candidate table much larger than the relation.

Total allocation for linear N=512, measured with `(.mem.ts (count (query db (find ?x ?y) (where (tc ?x ?y)))))` on release builds before and after this task's changes:

| | allocated-bytes | alloc-count | peak-live-bytes | time-ns |
|---|---|---|---|---|
| before (32e65461) | 1,888,396,288 | 27,988 | 35,654,208 | 162,667,333 |
| after | 73,245,632 | 22,891 | 36,702,400 | 102,932,959 |

25.8x less total allocation. Peak live is unchanged (the relation itself is the same size); what disappears is the full copy of the relation made once per iteration.

## After Task 14 (old/new split for multi-recursive rules, `8c75dade` + this commit)

- Change under test: `dl_delta_t` carries `prev_nrows[]` — the row count each IDB of the stratum had at the start of the previous iteration. Since `table_union` appends the delta at the end of the relation, that count is the length of the "old" prefix. A positive body atom **before** the delta position now reads a `dl_table_head` view of that prefix (zero-copy `ray_vec_slice` columns) instead of the full relation; the atom at the delta position reads Δ, atoms after it read the full relation. `p(X,Z) :- p(X,Y), p(Y,Z)` therefore derives Δ⋈Δ once instead of twice. An empty prefix (iteration 0) short-circuits the rule instance entirely.
- Prefix-path trigger counts (temporary counter, removed before commit), `tc_chain` N=128: nonlinear 6 head views + 1 empty-prefix short-circuit; linear 0 / 0 — the linear rule's recursive atom is the last positive atom, so nothing ever sits before the delta position and the path is never taken. Linear is unaffected by construction.
- Same methodology and machine as above (min-of-3 wall time via `date +%s%N`, `RAYFORCE_CORES=2`, `./rayforce` freshly built with `make -j8 release`). Load average right after the run: 1.20 0.97 0.77.

```
linear N=   64 rows=    2080 min_ms=     14 ok
linear N=  128 rows=    8256 min_ms=     25 ok
linear N=  256 rows=   32896 min_ms=     47 ok
linear N=  512 rows=  131328 min_ms=    110 ok
linear N= 1024 rows=  524800 min_ms=    332 ok
```

```
nonlinear N=   64 rows=    2080 min_ms=     10 ok
nonlinear N=  128 rows=    8256 min_ms=     26 ok
nonlinear N=  256 rows=   32896 min_ms=    136 ok
nonlinear N=  512 rows=  131328 min_ms=    837 ok
nonlinear N= 1024 rows=  524800 min_ms=   7495 ok
```

Every N reports `ok` with the expected row count.

| N | After Task 13 (nonlinear) | After Task 14 (nonlinear) | speedup |
|---|---|---|---|
| 64 | 10 ms | 10 ms | 1.0x |
| 128 | 27 ms | 26 ms | 1.0x |
| 256 | 142 ms | 136 ms | 1.04x |
| 512 | 905 ms | 837 ms | 1.08x |
| 1024 | 8304 ms | 7495 ms | 1.11x |

| N | After Task 13 (linear) | After Task 14 (linear) |
|---|---|---|
| 64 | 14 ms | 14 ms |
| 128 | 24 ms | 25 ms |
| 256 | 49 ms | 47 ms |
| 512 | 112 ms | 110 ms |
| 1024 | 340 ms | 332 ms |

Linear does not regress (it is within noise, and the prefix path is provably never taken there).

Non-linear gains ~8-11% at the large N, not the order-of-magnitude the linear shape got from Task 13. The reason is the shape of the work that is removed: on a chain, the second delta rule instance (`p_old ⋈ Δp`) loses exactly the `Δp ⋈ Δp` pairs, which are a small fraction of `P ⋈ Δp` once `P` is much larger than `Δp` — by construction the saving is bounded by |Δ|²/(|P|·|Δ|) = |Δ|/|P| of the join work per iteration. The nonlinear shape remains dominated by the join producing a candidate table far larger than the relation and the `table_distinct` that collapses it; closing that gap needs a different plan (index-nested-loop on the delta, or dedup fused into the join), not a better delta split.

## After final fix wave (9b32dd4c + the final-review fixes this commit introduces)

- Date: 2026-09-11
- Change under test: the whole-branch final-review fixes. The only one that
  touches the hot compile path is the narrowed DATOM untagging: `dl_compile_rule`
  now carries a `bool var_from_v[DL_MAX_ARITY * DL_MAX_BODY]` alongside
  `var_col[]` and propagates it into `dl_rel_t.col_from_v` at projection, so
  `ray_query_fn` untags only result columns that can hold a value from a datoms
  `v` column. That is one extra 256-byte memset per rule compile plus a few
  per-variable boolean assignments — no extra passes over data.
- Same methodology and machine as the sections above (min-of-3 wall time via
  `date +%s%N`, `RAYFORCE_CORES=2`, `./rayforce` freshly built with
  `make -j8 release`). Load average right after the run: 1.34 0.98 0.59.

```
linear N=   64 rows=    2080 min_ms=     14 ok
linear N=  128 rows=    8256 min_ms=     24 ok
linear N=  256 rows=   32896 min_ms=     47 ok
linear N=  512 rows=  131328 min_ms=    108 ok
linear N= 1024 rows=  524800 min_ms=    332 ok
```

```
nonlinear N=   64 rows=    2080 min_ms=     10 ok
nonlinear N=  128 rows=    8256 min_ms=     26 ok
nonlinear N=  256 rows=   32896 min_ms=    134 ok
nonlinear N=  512 rows=  131328 min_ms=    831 ok
nonlinear N= 1024 rows=  524800 min_ms=   7421 ok
```

Every N still reports `ok` with the expected row count.

| N | After Task 14 (linear) | After final fix wave | delta |
|---|---|---|---|
| 64 | 14 ms | 14 ms | 0% |
| 128 | 25 ms | 24 ms | -4% |
| 256 | 47 ms | 47 ms | 0% |
| 512 | 110 ms | 108 ms | -2% |
| 1024 | 332 ms | 332 ms | 0% |

| N | After Task 14 (nonlinear) | After final fix wave | delta |
|---|---|---|---|
| 64 | 10 ms | 10 ms | 0% |
| 128 | 26 ms | 26 ms | 0% |
| 256 | 136 ms | 134 ms | -1% |
| 512 | 837 ms | 831 ms | -1% |
| 1024 | 7495 ms | 7421 ms | -1% |

Every point is within run-to-run noise of the Task 14 numbers; the
per-variable tag tracking costs nothing measurable.

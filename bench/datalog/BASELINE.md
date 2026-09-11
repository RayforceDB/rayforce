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

# Aggregation and query type coverage results

Status: default-launch correction implemented and validated. The original eight-core results remain below for context; they did not establish default-launch performance. ThreadSanitizer could not start on this host (details below).

Baseline binary: release build at `500ea392` (P0). The allocator-only comparison
and initial census remain in [the plan](aggregation-fast-path-plan.md).

## Implemented coverage

| Plan | Gaps | Final implementation |
|---|---|---|
| P1 | Plumbing for A1–A10 | Native input widths, F32 NaN validity, type-correct scalar writes, disjoint parallel payload writes with null metadata merged afterward. Native extrema emission avoids temporary atoms. Stable source row indices retain column/domain ownership for ordered and wide values. |
| P2 | A1–A4, A6–A8, A11 | Streaming sum/min/max/avg/product/statistics across legal numeric/temporal types. F32 truth and mixed binary statistics. COUNT counts every row, including nulls. DATE/TIMESTAMP sum and temporal product/truth remain illegal. |
| P3 | K1, K3, K4 | Nullable dense mapping reserves a null slot without spanning the sentinel; selected/composite/all-null keys use the same mapping. Signed radix keys preserve nulls. Dense worker/global slabs have allocation and sampled traffic budgets; setup, merge and scalar emission are parallel. Removed full-input-per-worker generic hash allocation. |
| P4 | A2, A5 | First/last use stable original-row order, including selections; wide extrema use source rows and existing comparators. SYM extrema compare domain strings. LIST first/last retain selected values. |
| P5 | K2 | Canonical float hashing/equality, GUID bytes, STR bytes and structural LIST grouping. Small/ordered/buffered shapes use one shared index layout. Plain LIST-key aggregate queries, including composite keys, reach it through `select`. Large float/GUID/STR streaming shapes with up to eight keys use the existing parallel wide-key engine. |
| P6 | A9, A10 | Shared group IDs and stable row slices for median, quantile, mode, top/bottom-K, ordered/wide and mixed streaming aggregates. Wide/F32 top-K stores at most K row indices. Existing typed numeric heaps and rank helpers remain shared. |
| P7 | Q1, Q2 | Fused exact count-distinct accepts nullable BOOL/U8/signed/temporal/SYM/F32/F64 pairs. SYM is reachable despite conservative null metadata. F32 generic exact counting widens losslessly once to reuse F64 dedupe. |
| P8 | Q3–Q5 | F32/F64, nullable numeric/temporal, GUID and ordinary STR fused comparisons. Float/GUID filter/top-k sorting. Composite F32 sorts reuse the tested single-column float transform and radix-compose ranks. |
| P9 | Q6–Q8 | Typed F32/GUID/STR broadcasts, F32 numeric/pow admission, and one-time materialization of pure temporal arithmetic through unit-aware operations before typed aggregation. |

The [census](aggregation-type-census.csv) preserves the original 210 observations
and adds current streaming registration and grouped route columns. Registry
presence and query strategy are different: holistic/ordered operations use row
indices instead of streaming accumulator states.

## Explicit generic and rejected cases

- Heterogeneous LIST arithmetic/extrema/rank operations retain dynamic value
  evaluation. No inference from the first element selects a fixed-width kernel.
  Structural LIST keys and LIST first/last have a bounded shared-index route.
- GUID/STR/LIST count-distinct uses exact generic dedupe. The packed count-distinct
  rewrite still requires total key/value width at most 16 bytes; wide values
  are not truncated or interpreted as integers.
- Large float/wide streaming keys retain the established parallel implementation.
  On one million rows, moving these to serial index grouping regressed STR and
  F32 key workloads by about 3x. Admission now reports `parallel wide-key strategy`.
  Shapes needing ordered/buffered states, LIST keys, and more than eight keys
  use the bounded index route.
- Nullable LIKE/IN, SYM ordering predicates, and cross-unit temporal comparisons
  retain their existing evaluators. The new fused comparison leg covers the six
  scalar comparison operators; it does not silently extend other evaluators.
- Raw temporal DAG arithmetic stays guarded. Pure `+`, `-`, and `*` expressions
  are materialized once using existing unit/promotion rules. Other expressions,
  side-effecting calls, and unsupported partition shapes retain existing evaluation.
- Eval-level `as 'F32` remains outside this change's language surface. F32 test
  and benchmark fixtures use the existing query projection cast or C constructors.

## GrandU: original eight-core measurements

Same CSV and 95-column schema, eight cores, repeated query-only timings after
loading. Input: 13,916,401 rows. Output: 1,104,673 groups. Query:

```clojure
(select {from:t s:(min time) by:client_order_id})
```

| Measurement | P0 release | Current release |
|---|---:|---:|
| Full-table warm query range, final alternating runs | 128.3–133.9 ms | 75.3–80.4 ms |
| Full-table warm median, per run | 130.6 ms | 75.6 / 78.5 ms |
| Full-table first query, per run | 161.1 ms | 80.4 / 81.3 ms |
| Peak process RSS, including CSV/table | 28,686,944 KiB | 28,687,832–28,697,160 KiB |
| Cached two-column warm median, per run | 101.7 ms | 54.0 / 51.4 ms |

Final full-table order: current → baseline → current, five warm queries after
one cold query per process. The full-table query is **1.66–1.73x faster**;
the cached two-column query is **1.88–1.98x faster**. An earlier independent
full-table comparison measured 138.4 → 81.1 ms, consistent with the final result.

The full-table sorted serialized outputs compare byte-for-byte equal, including
logical types and null payloads. The current route is null-aware parallel dense
aggregation. RSS is whole-process peak, dominated by the full table, not a
measurement of aggregation scratch alone. Dense admission accounts for worker
and global state/row slabs plus row IDs and compares that byte estimate with the
scatter budget and a quarter of the heap watermark. For unfiltered single-key
queries, up to 1,024 evenly spaced samples per worker-sized row range also estimate
duplicated slot traffic. Radix is preferred when that traffic exceeds a scatter
payload. Sampling changes strategy only, not grouping or values. This retains
GrandU's dense advantage while avoiding the regression on the synthetic fixture
whose workers repeatedly traverse the entire key domain.

`make test` relinks `rayforce` with sanitizers. Performance runs therefore use
saved immutable release binaries; sanitizer timings are excluded.


## Default-launch correction

The original completion claim was too broad: every performance comparison
forced `-c 8`. On the same machine, an ordinary launch used 20 physical-core
workers and rejected dense aggregation because 20 full slot slabs exceeded its
budget. Reproduction on the cached GrandU columns measured 119.4 ms at P0,
121.6 ms at `4286de9f`, and 55.4 ms only when forcing eight workers. The user
correctly reported no default-launch improvement.

Dense execution now allocates slabs per logical task, with the task count chosen
from the existing byte budget and capped by pool size. Tasks own their slabs by
task index, never by whichever physical worker steals the task. Initialization,
selected/unselected accumulation, merge, and emission retain explicit ownership.
A large pool can therefore execute the dense plan without reserving a full slab
for every worker or changing the global pool setting. Sampled worker-traffic
admission uses the same logical input partitions.

The requested default pool policy now uses all online logical CPUs, including
SMT threads. On this i7-14700, the interactive banner verifies **28 logical CPUs /
28 workers**. Explicit `-c N` remains an override. The old banner mislabeled
logical CPUs as cores.

Validation after the correction: **3,802/3,802 ASan/UBSan tests passed**,
including automatic logical-CPU sizing. The ten aggregation contract tests also
passed after the final guard against reading an invalid dense plan. A 20-worker
fixture verifies that fewer logical tasks execute dense grouping correctly,
with and without a pushed selection, while leaving pool size unchanged.
The corrected interactive release banner was checked directly.

Exact full CSV/schema/query, five warm repetitions per process, no concurrent
build/test work. Baseline here is the previously pushed `4286de9f`:

| Launch | Old warm median (range), ms | Corrected warm median (range), ms |
|---|---:|---:|
| Default, first pair | 180.7 (174.3–281.1), 20 workers | 97.7 (95.6–100.2), 28 workers |
| Default, reverse-order pair | 187.2 (175.3–209.9), 20 workers | 93.4 (86.2–99.8), 28 workers |
| Explicit 28 workers | 169.6 (164.5–190.0) | Same 28-worker execution as corrected default above |
| Explicit 20 workers | Same 20-worker execution as old default above | 91.7 (88.3–97.9) |

The default-run comparison improves by approximately **1.85–2.00x** on this
machine. Fixed-count controls distinguish the dispatch fix from the requested
change to all logical CPUs. Cold default queries were 193.6 / 257.8 ms old and
103.8 / 107.7 ms corrected. Sorted serialized full-query results are byte-for-byte
identical, including types and nulls. The default synthetic fixture measured
63.6 ms old versus 61.4 ms corrected, with overlapping ranges.

Cached GrandU two-column warm medians across explicit worker counts:

| Workers | Old ms | Corrected ms |
|---|---:|---:|
| 1 | 81.0 | 79.5 |
| 2 | 59.5 | 58.7 |
| 4 | 47.5 | 48.0 |
| 8 | 56.0 | 52.7 |
| 20 | 126.5 | 59.6 |
| 28 | 113.6 | 59.5 |

[grandu_csv.rfl](../bench/groupby_shapes/grandu_csv.rfl) contains the exact
reported schema and query. Run it from the directory containing `GrandU.csv`,
using saved old/new release binaries **without `-c`**, then repeat in reverse
order. The first printed time is CSV loading; input/output row counts are
separate; the first query time is cold. No private data is included in the repo.

## Portable performance matrix

Eight cores; baseline → current → current → baseline. Each process executes
six queries per shape; the table combines ten warm observations per binary.
Ratio is baseline/current median. These are local measurements, not guarantees
for every distribution. Overlapping ranges around 1x indicate no clear change.

| Query, 1M rows | P0 ms, median (range) | Current ms, median (range) | Ratio |
|---|---:|---:|---:|
| nullable I32 key / min TIME | 5.44 (4.56–6.44) | 4.97 (4.32–6.28) | 1.10x |
| I32 sum and extrema | 5.98 (5.50–6.51) | 5.82 (5.03–6.25) | 1.03x |
| F32 extrema | 5.99 (5.39–6.88) | 6.05 (5.62–6.46) | 0.99x |
| F64 existing average and variance | 26.11 (24.76–27.76) | 13.67 (13.37–13.91) | 1.91x |
| mixed streaming and median | 12.74 (12.41–13.02) | 8.41 (8.31–8.55) | 1.52x |
| F32 filter and top 100 | 2.57 (2.37–2.67) | 0.73 (0.70–0.77) | 3.53x |
| composite F32 sort | 5.52 (5.45–5.73) | 5.47 (5.31–6.24) | 1.01x |
| nullable count-distinct TIME | 24.44 (23.28–24.76) | 23.89 (23.66–24.65) | 1.02x |
| typed string broadcast | 25.70 (25.15–26.77) | 6.57 (6.23–7.20) | 3.91x |
| temporal expression | 32.46 (31.75–32.93) | 5.20 (5.02–5.39) | 6.24x |
| SYM count-distinct TIME | 5.27 (5.19–5.63) | 2.42 (2.36–2.71) | 2.18x |
| STR grouping key | 3.57 (3.48–3.60) | 3.50 (3.46–3.57) | 1.02x |
| F32 grouping key | 2.55 (2.42–2.73) | 2.42 (2.40–2.48) | 1.05x |

The synthetic 13,916,401-row nullable TIME fixture measured baseline warm
medians **62.6 / 65.6 ms** and current **67.2 / 65.9 ms**; ranges overlap
(59.7–68.1 ms baseline, 65.7–70.2 ms current). Cold timings were 90.6 / 82.5 ms
baseline and 77.5 / 81.8 ms current. It uses radix and shows no measured speedup.
The earlier dense-only attempt took about 99 ms; the worker-traffic budget
removed that substantial regression. GrandU's different key locality retains
the measured dense speedup. STR/F32 streaming keys likewise retain their
established parallel strategy, with no repeatable regression in final runs.

## Reproduction and validation

- [type_coverage.rfl](../bench/groupby_shapes/type_coverage.rfl): deterministic
  one-million-row type/query matrix; first repetition is cold allocation.
- [nullable_time.rfl](../bench/groupby_shapes/nullable_time.rfl): distributable
  GrandU-shaped row/group counts with nullable I32 keys and TIME values. It is
  synthetic, not a copy of the private data or its exact order/skew.
- Run saved baseline/current release binaries sequentially with `-c 8`, in
  alternating order. Report medians/ranges of repetitions after the first.
- Ten contract tests pass with harness core counts 1 and 4 (route tests also
  explicitly select two and eight cores), including 210 unary semantic cells, 245 binary
  pairs, 336 registry/admission cases, nullable differential reductions, strategy
  counters, structural LIST composites through `select`, cancellation, and direct
  fused comparison/count-distinct routes.
- ThreadSanitizer builds succeeded, but both normal and non-PIE executables failed
  before running tests with `FATAL: ThreadSanitizer: unexpected memory mapping`.
  This host could not provide a TSan race verdict. Parallel ASan/UBSan correctness
  tests are not a substitute for that verdict.
- Final full ASan/UBSan suite: **3,801/3,801 passed**, with four-core harness
  execution, including query-level LIST admission and the dense worker-traffic regression.
- Clean release build passed with warnings treated as errors; executable size
  is 4.6 MiB. Saved release binaries were benchmarked sequentially, without
  concurrent test/build workloads. Full GrandU sorted serialized results
  compare byte-for-byte equal (`cmp` exit 0).

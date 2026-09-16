# Grouping engine scaling results

Status: local acceptance is complete for `f61a4eda`. This report contains
only synthetic fixtures and generic engine validation. Required PR checks gate merging.

## Method

Use separate immutable release binaries, identical generated inputs and worker
settings 1, 2, 4, 8, 16 and default. Run binaries sequentially, reversing their
order between rounds. Record the first query separately from five warm queries
in each of three fresh processes. Retain timing ranges and peak RSS.

The default pool uses all 28 logical CPUs on the validation host. Typed result
comparisons check keys, nulls and integer values exactly; floating reductions
use relative tolerance `1e-9` and absolute tolerance `1e-8`. Independent unit
oracles cover ordering, nulls, rank and distinct semantics.

## Correctness

The following completed checks apply to revision `f61a4eda`.

- **2,700 fresh-process runs**, 77 synthetic cases, six worker settings and three rounds: all typed cold/warm result comparisons passed.
- Complete ASan/UBSan suite: **3,828/3,828 passed**.
- Targeted TSan: **24/24 passed**, `RAYFORCE_CORES=3` (four total threads), `setarch x86_64 -R`, no suppressions.
- Coverage includes native widths, source symbol domains, structural LIST keys,
  stable row indices, dominant-group exact `med`/quantile, frequency ties,
  parallel top/bottom-K merges, adaptive symbol slices and typed empty output.

Earlier TSan attempts with five total threads stalled during pool shutdown before
the aggregation checks; they are not counted as passes. The final run used four
total threads and reported no races. No pool implementation or suppression was
changed to obtain that result.

Four synthetic cases fail in the baseline: STR-valued count-distinct and its
mixed form crash; LIST-key count-distinct and its mixed form raise a type error.
These cases receive no baseline speedup claim. Candidate results are checked
across workers and with independent unit-test oracles.

## Warm performance by family

Times below are medians of three process warm medians, in milliseconds.
The [complete summary](../bench/groupby_shapes/results/2026-09-15/grouping.csv)
contains all 77 cases and all worker counts. The [process records](../bench/groupby_shapes/results/2026-09-15/processes.csv)
retain every cold time, five warm times, result/binary hashes and peak RSS.

| Synthetic case | Baseline, 28 | Current, 8 | Current, 28 | Baseline/current at 28 |
|---|---:|---:|---:|---:|
| `extrema-time` | 6.379 | 1.816 | 0.958 | 6.66x |
| `sum-time` | 5.228 | 1.098 | 0.678 | 7.72x |
| `sum-i64` | 6.013 | 1.101 | 0.724 | 8.30x |
| `count` | 3.350 | 1.249 | 0.571 | 5.86x |
| `statistics` | 10.153 | 2.320 | 1.313 | 7.73x |
| `binary` | 17.848 | 4.203 | 2.415 | 7.39x |
| `product` | 5.789 | 1.221 | 0.737 | 7.85x |
| `truth` | 8.331 | 1.802 | 1.004 | 8.30x |
| `median` | 6.209 | 3.247 | 2.021 | 3.07x |
| `quantile` | 6.110 | 3.323 | 2.022 | 3.02x |
| `mode` | 6.682 | 4.346 | 2.264 | 2.95x |
| `top-bottom` | 9.028 | 5.655 | 4.373 | 2.06x |
| `first-last` | 9.594 | 2.808 | 1.738 | 5.52x |
| `distinct` | 12.735 | 6.107 | 4.112 | 3.10x |
| `mixed` | 8.763 | 3.857 | 2.397 | 3.66x |
| `composite` | 13.981 | 3.886 | 2.119 | 6.60x |
| `selected` | 2.525 | 1.191 | 0.968 | 2.61x |
| `sparse` | 5.975 | 7.922 | 7.014 | 0.85x |
| `float-key` | 6.357 | 4.220 | 2.208 | 2.88x |
| `string-key` | 8.497 | 2.628 | 1.652 | 5.14x |
| `guid-key` | 10.530 | 2.948 | 2.518 | 4.18x |
| `list-key` | 27.750 | 3.764 | 2.762 | 10.05x |

Aggregate costs differ, and these measurements do not establish linear scaling
or a universal best worker count. I64 sum improves from eight to 28 workers in
this final sweep; its strategy changes from task-local state to partitioned
state. The GUID case now splits medium-sized rank groups using the common
parallel grain, closing the scheduling gap found during phase review.

## Cold costs and repeatability

The main sweep retains these regressions. The follow-up used five additional
fresh processes per binary for every configuration that was over 5% slower in
at least two main-sweep rounds: 13 configurations and **130 verified runs**.
All [follow-up process records](../bench/groupby_shapes/results/2026-09-15/regression-repeat.csv)
are retained; they do not replace the main samples.

| Case / workers | Metric | Main baseline ms | Main current ms | Repeat baseline ms | Repeat current ms |
|---|---|---:|---:|---:|---:|
| `clustered` / 4 | cold | 1.700 | 2.588 | 2.248 | 1.709 |
| `clustered` / 8 | cold | 2.145 | 2.596 | 1.938 | 2.506 |
| `hot-min-descending` / 8 | cold | 1.446 | 2.047 | 1.409 | 2.099 |
| `float-key` / default | cold | 3.841 | 4.994 | 3.921 | 5.040 |
| `nonnull` / 8 | cold | 3.062 | 3.411 | 2.493 | 3.191 |
| `product` / 8 | cold | 2.348 | 2.615 | 2.260 | 2.600 |
| `symbol-key` / 2 | cold | 0.647 | 0.885 | 0.750 | 0.710 |
| `symbol-key` / 2 | warm | 0.527 | 0.763 | 0.671 | 0.559 |
| `sparse` / default | warm | 5.975 | 7.014 | 6.196 | 5.890 |

Neither warm slowdown reproduced in the five-process follow-up. The symbol
and sparse samples vary across runs; these fixtures support neither a fixed
warm regression nor a universal speedup claim. The additional cold flags for
`extrema-f32`, `sum-f64`, `truth`, `extrema-i32` and `sum-time` also did not
retain a median regression above 5% in the follow-up.

Cold regressions remain for the eight-worker descending-min, clustered,
nonnull and product fixtures, and for default-worker float keys. The
[68 profiled runs](../bench/groupby_shapes/results/2026-09-15/phase-profiles.csv)
place the extra time in slab allocation, histogram/scatter, directory setup
and filling row indices:

- Descending-min and product spend about 0.66 and 0.64 ms, respectively, in
  cold slab allocation; the corresponding warm phase rounds to zero.
- Clustered histogram/scatter takes about 1.83 ms cold versus 0.49 ms warm.
- Float-key directory allocation/initialization takes about 1.05 ms cold
  versus 0.09 ms warm; row-index filling takes 1.49 versus 0.59 ms.

The bounded state and shared-directory strategies reduce repeated state
initialization and merging, while retaining these first-execution costs.
The report keeps those costs alongside the warm improvements.

The rank profiles also confirm the medium-group scheduling change: the
four-group GUID fixture now uses parallel rank selection within its groups.
Small groups retain the direct consumer; task counts remain bounded by work
and scratch storage. All query state is rebuilt per execution.

## Coverage ledger

The `agg_contract` tests in [test_agg_contract.c](../test/test_agg_contract.c)
check values and routes independently of benchmark timings.

| Engine work | Synthetic cases | Correctness evidence |
|---|---|---|
| Native streaming and scalar output | `sum-*`, `extrema-*`, `count`, `statistics`, `product`, `truth` | Unary scalar/grouped contracts, every streaming registry writer, native null/overflow output, parallel float oracle |
| Binary reductions | `binary` | All numeric input-type pairs; zero weights, null-only pairs and empty states |
| Dense strategies and skew | `clustered`, `unique-keys`, `skew`, `all-null-key`, `hot-min-*` | Route assertions, extreme native values, constrained heap budget, independent per-key results |
| Selected and composite grouping | `selected`, `composite`, `selected-indexed` | Selected source-row identity, composite keys and mixed consumers |
| Shared wide-key directory | `float-*`, `string-*`, `guid-key`, `list-key` | Full value equality, float canonicalization, structural keys and stable group indices |
| Ordered and wide consumers | `first-last`, `wide-extrema`, `symbol-extrema`, corresponding `hot-*` cases | First/last source order, symbol domains, native gathering and dominant-group winners |
| Exact rank, mode and top/bottom K | `median`, `quantile`, `mode`, `top-bottom`, wide/symbol and `hot-*` variants | Rank/null/slice oracles, frequency ties, reordered indices, bounded heaps and parallel merges |
| Exact distinct and mixed consumers | `distinct*`, `mixed-distinct*`, `mixed`, `skew-indexed` | Native and wide distinct oracles, first-row ordering, shared stable layouts |

The final timing matrix covers each case at 1/2/4/8/16/default workers. Invalid
operation/type pairs stay explicit errors; heterogeneous LIST arithmetic keeps
its existing dynamic evaluation. Default pool size and useful task count are
separate: the pool exposes all CPUs while each stage bounds tasks by its work
and memory budget.

## Internal top/bottom-K kernel

All **90 runs** at revision `f61a4eda` matched the independent histogram oracle.
These synthetic inputs have 4,000,003 rows; K above 1024 exercises the internal
kernel, without changing the query language limit.

| K | 1 worker warm ms | 8 workers warm ms | Default (28) warm ms |
|---:|---:|---:|---:|
| 1024 | 11.206 | 2.148 | 1.795 |
| 65536 | 53.571 | 20.432 | 17.512 |
| 4000003 | 772.737 | 159.594 | 89.194 |

Values are medians of the three process warm medians. The CSV in
`bench/groupby_shapes/results/2026-09-15/topk.csv` retains process ranges,
cold medians and peak RSS. Scaling varies with K; these results do not establish linear scaling. The baseline fails the
full-size-K oracle because of partially filled heap ordering, so that case
has no baseline speedup claim.

## Reproduction

```sh
python3 bench/groupby_shapes/grouping_scaling.py \
  --binary /path/to/candidate --baseline /path/to/baseline \
  --baseline-skip-cases distinct-string-values,mixed-distinct-string-values,distinct-list-keys,mixed-distinct-list-keys \
  --workers 1,2,4,8,16,default --rounds 3 --output /tmp/grouping.json

python3 bench/groupby_shapes/summarize_scaling.py /tmp/grouping.json /tmp/grouping.csv \
  --process-output /tmp/grouping-processes.csv
```

To investigate one configuration, use the same driver with, for example,
`--cases symbol-key --workers 2 --rounds 5`. Add `--profile` for a separate
phase-timing run; profiling measurements are kept separate from the main sweep
and unprofiled repeats.

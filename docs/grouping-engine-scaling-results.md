# Grouping engine scaling results

Status: synthetic acceptance is complete for `91569b8d`. This report contains
only synthetic fixtures and generic engine validation. Delivery checks are in progress.

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

The following completed checks apply to revision `91569b8d`.

- **2,700 fresh-process runs**, 77 synthetic cases, six worker settings and three rounds: all typed cold/warm result comparisons passed.
- Complete ASan/UBSan suite: **3,826/3,826 passed**.
- Targeted TSan: **24/24 passed**, `RAYFORCE_CORES=3` (four total threads), `setarch x86_64 -R`, no suppressions.
- Coverage includes native widths, source symbol domains, structural LIST keys,
  stable row indices, dominant-group exact `med`/quantile, frequency ties,
  parallel top/bottom-K merges, adaptive symbol slices and typed empty output.

TSan runs with five total threads stalled during pool shutdown before the
aggregation checks; they are not counted as passes. The completed run used four
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
| `extrema-time` | 5.857 | 1.914 | 0.970 | 6.04x |
| `sum-time` | 5.699 | 1.352 | 0.677 | 8.42x |
| `sum-i64` | 6.460 | 0.730 | 0.713 | 9.07x |
| `count` | 3.866 | 1.276 | 0.562 | 6.87x |
| `statistics` | 9.655 | 1.947 | 1.330 | 7.26x |
| `binary` | 18.892 | 3.655 | 2.356 | 8.02x |
| `product` | 6.732 | 1.442 | 0.743 | 9.06x |
| `truth` | 7.278 | 1.841 | 1.023 | 7.12x |
| `median` | 5.860 | 3.195 | 1.975 | 2.97x |
| `quantile` | 6.367 | 3.744 | 2.054 | 3.10x |
| `mode` | 6.957 | 4.156 | 2.382 | 2.92x |
| `top-bottom` | 8.914 | 5.772 | 4.354 | 2.05x |
| `first-last` | 9.837 | 2.643 | 1.875 | 5.25x |
| `distinct` | 12.875 | 5.739 | 4.500 | 2.86x |
| `mixed` | 9.012 | 4.195 | 2.412 | 3.74x |
| `composite` | 13.697 | 3.921 | 2.078 | 6.59x |
| `selected` | 2.502 | 1.317 | 0.956 | 2.62x |
| `sparse` | 7.352 | 7.960 | 5.407 | 1.36x |
| `float-key` | 5.688 | 4.103 | 2.294 | 2.48x |
| `string-key` | 8.623 | 2.354 | 1.525 | 5.66x |
| `guid-key` | 10.565 | 2.869 | 3.411 | 3.10x |
| `list-key` | 27.611 | 4.230 | 3.473 | 7.95x |

Aggregate costs differ. For example, the I64 sum is nearly flat from eight to
28 workers in this fixture. The four-group GUID/mixed case has overlapping
process ranges and a slower median at 28 than eight. These measurements do
not establish linear scaling or a universal best worker count.

## Cold costs and low-worker tradeoffs

The main sweep retains the following regressions. They are included in the
complete CSVs rather than excluded from the summary.

| Case / workers | Metric | Baseline ms | Current ms | Investigation |
|---|---|---:|---:|---|
| `clustered` / 8 | cold | 1.995 | 2.727 | Partition histogram/scatter setup; lower warm time |
| `product` / 8 | cold | 2.353 | 3.191 | First slab allocation/initialization; follow-up cold ranges overlap |
| `nonnull` / 8 | cold | 2.438 | 3.088 | Partition histogram/scatter setup; follow-up cold ranges overlap |
| `float-key` / default | cold | 3.814 | 5.097 | Directory allocation/initialization and initial row-index writes |
| `symbol-key` / 1 | cold | 1.184 | 1.313 | Small-domain accumulation and fixed setup |
| `symbol-key` / 2 | warm | 0.496 | 0.548 | About 0.05 ms additional accumulation work |
| `quantile` / 2 | warm | 7.077 | 8.521 | Variable in follow-up; five further processes show a 2% median difference |

[Phase profiles](../bench/groupby_shapes/results/2026-09-15/phase-profiles.csv)
locate the cold costs in working-buffer allocation and first writes. For the
clustered case at eight workers, histogram and scatter together take about
2.0 ms cold versus 0.7 ms warm. For float keys at 28 workers, directory
allocation/initialization and row-index filling account for most of the
cold/warm difference. The partitioned/shared strategies reduce repeated state
initialization and merging, while retaining this first-execution cost.

The small-domain symbol case spends the extra time in accumulation. Its
candidate uses fixed 8,192-row ID batches and bounded task-local state; the
baseline allocates an ID buffer for each whole source range. We retain the
bounded working-memory strategy and report its low-worker latency cost.
The [five-process follow-up](../bench/groupby_shapes/results/2026-09-15/low-worker-repeat.csv)
measured symbol warm medians of 0.529 versus 0.578 ms, and quantile medians of
7.035 versus 7.172 ms. The original quantile samples remain in the main report;
the larger slowdown was not stable across fresh processes.

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

All **90 runs** at revision `91569b8d` matched the independent histogram oracle.
These synthetic inputs have 4,000,003 rows; K above 1024 exercises the internal
kernel, without changing the query language limit.

| K | 1 worker warm ms | 8 workers warm ms | Default (28) warm ms |
|---:|---:|---:|---:|
| 1024 | 11.151 | 2.324 | 1.825 |
| 65536 | 53.655 | 20.064 | 14.530 |
| 4000003 | 756.862 | 150.147 | 94.078 |

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
